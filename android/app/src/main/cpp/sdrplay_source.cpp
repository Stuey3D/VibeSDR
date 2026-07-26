// SDRplay (RSP) capture source — see sdrplay_source.h for why this exists.
#include "sdrplay_source.h"

#ifdef VIBE_HAVE_SDRPLAY
#include <sdrplay_api.h>
#include <dlfcn.h>
#include <cstring>
#include <mutex>
#include <vector>

namespace vibe {

namespace {
// ★★ LOADED AT RUNTIME, NEVER LINKED. The SDRplay API is a separate install that lives in
// /usr/local/lib — and a notarised, hardened-runtime app that hard-links a dylib from there
// CRASHES ON LAUNCH for everyone who does not have it. That is the same trap the librtlsdr
// block in CMakeLists warns about, and it would break VibeServer for the overwhelming
// majority of users who have no RSP, in order to serve the few who do.
// So: dlopen on first use, resolve the dozen entry points we need, and if the library is
// absent simply report zero SDRplay devices. "Drag one .app out of a DMG and it works" stays
// true either way (2026-07-26).
struct Api {
    void* h = nullptr;
    sdrplay_api_Open_t              Open = nullptr;
    sdrplay_api_Close_t             Close = nullptr;
    sdrplay_api_GetDevices_t        GetDevices = nullptr;
    sdrplay_api_SelectDevice_t      SelectDevice = nullptr;
    sdrplay_api_ReleaseDevice_t     ReleaseDevice = nullptr;
    sdrplay_api_GetDeviceParams_t   GetDeviceParams = nullptr;
    sdrplay_api_Init_t              Init = nullptr;
    sdrplay_api_Uninit_t            Uninit = nullptr;
    sdrplay_api_Update_t            Update = nullptr;
    sdrplay_api_LockDeviceApi_t     Lock = nullptr;
    sdrplay_api_UnlockDeviceApi_t   Unlock = nullptr;
    sdrplay_api_GetErrorString_t    GetErrorString = nullptr;
    bool ok = false;
};

Api& api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [&]{
        // The installer's own location first, then the usual link paths.
        const char* paths[] = {
            "/usr/local/lib/libsdrplay_api.dylib",
            "/Library/SDRplayAPI/3.15.1/lib/libsdrplay_api.dylib",
            "libsdrplay_api.so.3",
            "libsdrplay_api.dylib",
        };
        for (const char* p : paths) {
            a.h = dlopen(p, RTLD_LAZY | RTLD_LOCAL);
            if (a.h) break;
        }
        if (!a.h) return;
        auto sym = [&](const char* n) { return dlsym(a.h, n); };
        a.Open            = (sdrplay_api_Open_t)            sym("sdrplay_api_Open");
        a.Close           = (sdrplay_api_Close_t)           sym("sdrplay_api_Close");
        a.GetDevices      = (sdrplay_api_GetDevices_t)      sym("sdrplay_api_GetDevices");
        a.SelectDevice    = (sdrplay_api_SelectDevice_t)    sym("sdrplay_api_SelectDevice");
        a.ReleaseDevice   = (sdrplay_api_ReleaseDevice_t)   sym("sdrplay_api_ReleaseDevice");
        a.GetDeviceParams = (sdrplay_api_GetDeviceParams_t) sym("sdrplay_api_GetDeviceParams");
        a.Init            = (sdrplay_api_Init_t)            sym("sdrplay_api_Init");
        a.Uninit          = (sdrplay_api_Uninit_t)          sym("sdrplay_api_Uninit");
        a.Update          = (sdrplay_api_Update_t)          sym("sdrplay_api_Update");
        a.Lock            = (sdrplay_api_LockDeviceApi_t)   sym("sdrplay_api_LockDeviceApi");
        a.Unlock          = (sdrplay_api_UnlockDeviceApi_t) sym("sdrplay_api_UnlockDeviceApi");
        a.GetErrorString  = (sdrplay_api_GetErrorString_t)  sym("sdrplay_api_GetErrorString");
        a.ok = a.Open && a.Close && a.GetDevices && a.SelectDevice && a.ReleaseDevice
            && a.GetDeviceParams && a.Init && a.Uninit && a.Update && a.Lock && a.Unlock;
    });
    return a;
}

const char* errStr(sdrplay_api_ErrT e) {
    return api().GetErrorString ? api().GetErrorString(e) : "sdrplay error";
}

std::mutex g_apiMtx;
int        g_apiRefs = 0;

bool apiOpen(std::string& err) {
    if (!api().ok) {
        err = "SDRplay API not installed on this machine";
        return false;
    }
    std::lock_guard<std::mutex> lk(g_apiMtx);
    if (g_apiRefs > 0) { ++g_apiRefs; return true; }
    const sdrplay_api_ErrT e = api().Open();
    if (e != sdrplay_api_Success) {
        err = std::string("sdrplay_api_Open: ") + errStr(e)
            + " (is the SDRplay API service running?)";
        return false;
    }
    ++g_apiRefs;
    return true;
}

void apiClose() {
    std::lock_guard<std::mutex> lk(g_apiMtx);
    if (g_apiRefs > 0 && --g_apiRefs == 0) api().Close();
}
}  // namespace

struct SdrplaySource::Impl {
    sdrplay_api_DeviceT       dev{};
    sdrplay_api_DeviceParamsT* params = nullptr;
    bool selected = false;
    bool streaming = false;
    // Interleaving scratch. ★ The API hands us SEPARATE I and Q buffers; everything
    // downstream (and the SpyServer path we are reusing) expects them interleaved. Reused
    // rather than allocated per callback — this runs on the API's streaming thread.
    std::vector<int16_t> ilv;
};

bool SdrplaySource::available() {
    std::string err;
    if (!apiOpen(err)) return false;
    apiClose();
    return true;
}

int SdrplaySource::deviceCount() {
    std::string err;
    if (!apiOpen(err)) return 0;
    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int n = 0;
    api().Lock();
    api().GetDevices(devs, &n, SDRPLAY_MAX_DEVICES);
    api().Unlock();
    apiClose();
    return (int)n;
}

std::string SdrplaySource::deviceName(int index) {
    std::string err;
    if (!apiOpen(err)) return "";
    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int n = 0;
    api().Lock();
    api().GetDevices(devs, &n, SDRPLAY_MAX_DEVICES);
    api().Unlock();
    std::string name;
    if (index >= 0 && (unsigned)index < n) {
        // ★ The MODEL, plus the serial. RSP1A/RSP1B/RSPdx behave differently enough that
        // "SDRplay" alone would leave a user guessing which settings apply — the same
        // reasoning as reading the dongle's USB strings rather than librtlsdr's guess.
        switch (devs[index].hwVer) {
            case SDRPLAY_RSP1_ID:   name = "SDRplay RSP1";   break;
            case SDRPLAY_RSP1A_ID:  name = "SDRplay RSP1A";  break;
            case SDRPLAY_RSP1B_ID:  name = "SDRplay RSP1B";  break;
            case SDRPLAY_RSP2_ID:   name = "SDRplay RSP2";   break;
            case SDRPLAY_RSPduo_ID: name = "SDRplay RSPduo"; break;
            case SDRPLAY_RSPdx_ID:  name = "SDRplay RSPdx";  break;
            case SDRPLAY_RSPdxR2_ID:name = "SDRplay RSPdx-R2"; break;
            default:                name = "SDRplay RSP";    break;
        }
        if (devs[index].SerNo[0]) name += std::string(" ") + devs[index].SerNo;
    }
    apiClose();
    return name;
}

SdrplaySource::SdrplaySource() : impl_(new Impl) {}
SdrplaySource::~SdrplaySource() { close(); delete impl_; }

// ── Streaming callbacks ──────────────────────────────────────────────────────
static void streamCb(short* xi, short* xq, sdrplay_api_StreamCbParamsT*,
                     unsigned int numSamples, unsigned int /*reset*/, void* ctx);
static void eventCb(sdrplay_api_EventT id, sdrplay_api_TunerSelectT,
                    sdrplay_api_EventParamsT* p, void* ctx);

namespace {
// Only the pieces the callbacks touch, so this needs no access to the private Impl.
struct CbCtx { std::vector<int16_t>* ilv; SdrplaySource::IqSink* sink; bool* lost; };
}

bool SdrplaySource::open(int index, double sampleRateHz, double centreHz,
                         int gainTenthDb, std::string& err) {
    if (open_) return true;
    if (!apiOpen(err)) return false;

    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int n = 0;
    api().Lock();
    api().GetDevices(devs, &n, SDRPLAY_MAX_DEVICES);
    if (index < 0 || (unsigned)index >= n) {
        api().Unlock();
        apiClose();
        err = "no SDRplay device at that index";
        return false;
    }
    impl_->dev = devs[index];
    sdrplay_api_ErrT e = api().SelectDevice(&impl_->dev);
    api().Unlock();
    if (e != sdrplay_api_Success) {
        apiClose();
        err = std::string("SelectDevice: ") + errStr(e);
        return false;
    }
    impl_->selected = true;

    if ((e = api().GetDeviceParams(impl_->dev.dev, &impl_->params))
            != sdrplay_api_Success || !impl_->params) {
        close();
        err = std::string("GetDeviceParams: ") + errStr(e);
        return false;
    }

    auto* dp = impl_->params->devParams;
    auto* ch = impl_->params->rxChannelA;
    if (dp) dp->fsFreq.fsHz = sampleRateHz;
    if (ch) {
        ch->tunerParams.rfFreq.rfHz = centreHz;
        // ★ Zero-IF with a bandwidth wide enough for the whole FM MPX. RDS rides at 57 kHz,
        // so anything narrower than ~200 kHz would remove the very thing this device was
        // fetched to receive — and it would do so silently, looking like a decode failure.
        ch->tunerParams.bwType = sdrplay_api_BW_1_536;
        ch->tunerParams.ifType = sdrplay_api_IF_Zero;
        ch->ctrlParams.decimation.enable = 0;
        ch->ctrlParams.dcOffset.DCenable = 1;
        ch->ctrlParams.dcOffset.IQenable = 1;
    }
    setGainTenthDb(gainTenthDb);

    static CbCtx ctx;
    ctx = CbCtx{ &impl_->ilv, &sink_, &lost_ };
    sdrplay_api_CallbackFnsT fns{};
    fns.StreamACbFn = &streamCb;
    fns.StreamBCbFn = nullptr;
    fns.EventCbFn   = &eventCb;
    if ((e = api().Init(impl_->dev.dev, &fns, &ctx)) != sdrplay_api_Success) {
        close();
        err = std::string("Init: ") + errStr(e);
        return false;
    }
    impl_->streaming = true;
    open_ = true;
    lost_ = false;
    return true;
}

void SdrplaySource::close() {
    if (impl_ && impl_->streaming) { api().Uninit(impl_->dev.dev); impl_->streaming = false; }
    if (impl_ && impl_->selected)  { api().ReleaseDevice(&impl_->dev); impl_->selected = false; }
    if (open_) apiClose();
    open_ = false;
}

void SdrplaySource::setFrequency(double hz) {
    if (!open_ || !impl_->params || !impl_->params->rxChannelA) return;
    impl_->params->rxChannelA->tunerParams.rfFreq.rfHz = hz;
    api().Update(impl_->dev.dev, impl_->dev.tuner,
                       sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setSampleRate(double hz) {
    if (!open_ || !impl_->params || !impl_->params->devParams) return;
    impl_->params->devParams->fsFreq.fsHz = hz;
    api().Update(impl_->dev.dev, impl_->dev.tuner,
                       sdrplay_api_Update_Dev_Fs, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setGainTenthDb(int tenthDb) {
    if (!impl_->params || !impl_->params->rxChannelA) return;
    auto* ch = impl_->params->rxChannelA;
    // ★★ THE GAIN MODEL IS TWO-DIMENSIONAL, and that is a real difference from a dongle
    // rather than a detail. An RSP has an LNA state (RF gain, in coarse steps whose meaning
    // depends on band and model) and an IF gain REDUCTION in dB. A single slider therefore
    // needs a POLICY, and the one that matters for our purpose is: prefer LNA gain low and
    // IF reduction low, because RF overload is what destroys the RDS subcarrier — the exact
    // failure we spent 2026-07-26 chasing on an 8-bit dongle.
    if (tenthDb < 0) {
        ch->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;   // API's own AGC
    } else {
        ch->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
        // Map 0..49 dB of "gain" onto IF gain reduction, which runs the other way: 20 dB of
        // reduction is minimum, 59 maximum. More gain = less reduction.
        int gr = 59 - (tenthDb / 10);
        if (gr < 20) gr = 20;
        if (gr > 59) gr = 59;
        ch->tunerParams.gain.gRdB = gr;
        ch->tunerParams.gain.LNAstate = 0;                  // most RF gain; see the note above
    }
    if (open_)
        api().Update(impl_->dev.dev, impl_->dev.tuner,
                           (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Tuner_Gr
                                                        | sdrplay_api_Update_Ctrl_Agc),
                           sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setBiasT(bool on) {
    if (!impl_->params || !impl_->params->rxChannelA) return;
    // Bias-T lives in a different struct per model, so only the common ones are wired.
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP1A_ID:
        case SDRPLAY_RSP1B_ID:
            impl_->params->rxChannelA->rsp1aTunerParams.biasTEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                          sdrplay_api_Update_Rsp1a_BiasTControl,
                                          sdrplay_api_Update_Ext1_None);
            break;
        default: break;
    }
}

static void streamCb(short* xi, short* xq, sdrplay_api_StreamCbParamsT*,
                     unsigned int numSamples, unsigned int, void* ctx) {
    auto* c = (CbCtx*)ctx;
    if (!c || !c->sink || !*c->sink || numSamples == 0) return;
    auto& ilv = *c->ilv;
    if (ilv.size() < (size_t)numSamples * 2) ilv.resize((size_t)numSamples * 2);
    for (unsigned i = 0; i < numSamples; ++i) {
        ilv[i * 2]     = xi[i];
        ilv[i * 2 + 1] = xq[i];
    }
    (*c->sink)(ilv.data(), (int)numSamples);
}

static void eventCb(sdrplay_api_EventT id, sdrplay_api_TunerSelectT,
                    sdrplay_api_EventParamsT*, void* ctx) {
    auto* c = (CbCtx*)ctx;
    if (!c) return;
    // ★ A device removal is the one event the shim genuinely has to know about: its watchdog
    // already distinguishes "stream fault" from "radio gone", and telling it the truth is what
    // keeps a false "no radio" off a working waterfall.
    if (id == sdrplay_api_DeviceRemoved && c->lost) *c->lost = true;
}

}  // namespace vibe

#else   // !VIBE_HAVE_SDRPLAY — the API is a separate install; degrade honestly.

namespace vibe {
struct SdrplaySource::Impl {};
SdrplaySource::SdrplaySource() = default;
SdrplaySource::~SdrplaySource() = default;
bool SdrplaySource::available() { return false; }
int  SdrplaySource::deviceCount() { return 0; }
std::string SdrplaySource::deviceName(int) { return ""; }
bool SdrplaySource::open(int, double, double, int, std::string& err) {
    err = "this build has no SDRplay support";
    return false;
}
void SdrplaySource::close() {}
void SdrplaySource::setFrequency(double) {}
void SdrplaySource::setSampleRate(double) {}
void SdrplaySource::setGainTenthDb(int) {}
void SdrplaySource::setBiasT(bool) {}
}  // namespace vibe

#endif
