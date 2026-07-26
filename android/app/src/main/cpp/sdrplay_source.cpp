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
struct CbCtx { std::vector<int16_t>* ilv; SdrplaySource::IqSink* sink; bool* lost; bool* paused; };
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
        ch->tunerParams.bwType = (sdrplay_api_Bw_MHzT)bandwidthKHzForRate(sampleRateHz);
        ch->tunerParams.ifType = sdrplay_api_IF_Zero;
        ch->ctrlParams.decimation.enable = 0;
        ch->ctrlParams.dcOffset.DCenable = 1;
        ch->ctrlParams.dcOffset.IQenable = 1;
    }
    setGainTenthDb(gainTenthDb);

    static CbCtx ctx;
    ctx = CbCtx{ &impl_->ilv, &sink_, &lost_, &paused_ };
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
    // ★ The IF bandwidth must follow the rate, or a wider span arrives already filtered.
    if (impl_->params->rxChannelA)
        impl_->params->rxChannelA->tunerParams.bwType =
            (sdrplay_api_Bw_MHzT)bandwidthKHzForRate(hz);
    api().Update(impl_->dev.dev, impl_->dev.tuner,
                 (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Dev_Fs
                                              | sdrplay_api_Update_Tuner_BwType),
                 sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setGainTenthDb(int tenthDb) {
    if (!impl_->params || !impl_->params->rxChannelA) return;
    auto* ch = impl_->params->rxChannelA;
    // ★★ ON AN RSP THE SLIDER DRIVES THE LNA, NOT THE IF — the opposite of a dongle, and the
    // correction to a genuinely bad first attempt. The first version pinned LNAstate to 0,
    // which is MAXIMUM RF gain, and moved only the IF reduction: the front end sat wide open
    // whatever the user did, and an RSP with its LNA wide open on the FM band floods itself
    // (Stuart, on air 2026-07-26 — "the uncontrollable gain that is flooding it").
    // ★ RF overload is what destroys the RDS subcarrier, the entire reason this hardware is
    // here, so the one control a single slider gets must be the one that governs it.
    const int n = lnaStateCount();
    int st;
    if (tenthDb < 0) {
        // "Auto" gets a MIDDLE LNA state, never 0 — automatic must not mean wide open.
        st = n / 2;
    } else {
        // 0..49 dB onto the LNA ladder, INVERTED: state 0 is the most RF gain.
        st = (int)((1.0 - (double)tenthDb / 490.0) * (n - 1) + 0.5);
    }
    if (st < 0) st = 0;
    if (st > n - 1) st = n - 1;
    ch->tunerParams.gain.LNAstate = (unsigned char)st;
    setIfAgc(true);
    if (open_)
        api().Update(impl_->dev.dev, impl_->dev.tuner,
                     sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
}

int SdrplaySource::bandwidthKHzForRate(double fs) {
    // SoapySDRPlay3's mapping. ★ 1.536 MHz is the one that matters for FM: the MPX runs to
    // 57 kHz plus sidebands, so anything narrower silently removes RDS — the very thing this
    // hardware was fetched to receive.
    if (fs < 300000)   return 200;
    if (fs < 600000)   return 300;
    if (fs < 1536000)  return 600;
    if (fs < 5000000)  return 1536;
    if (fs < 6000000)  return 5000;
    if (fs < 7000000)  return 6000;
    if (fs < 8000000)  return 7000;
    return 8000;
}

int SdrplaySource::lnaStateCount() const {
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP1_ID:    return 4;
        case SDRPLAY_RSP1A_ID:
        case SDRPLAY_RSP1B_ID:   return 10;
        case SDRPLAY_RSP2_ID:    return 9;
        case SDRPLAY_RSPduo_ID:  return 10;
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID: return 28;
        default:                 return 4;
    }
}

bool SdrplaySource::hasRfNotch() const {
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP1A_ID: case SDRPLAY_RSP1B_ID: case SDRPLAY_RSP2_ID:
        case SDRPLAY_RSPduo_ID: case SDRPLAY_RSPdx_ID: case SDRPLAY_RSPdxR2_ID: return true;
        default: return false;
    }
}
bool SdrplaySource::hasDabNotch() const {
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP1A_ID: case SDRPLAY_RSP1B_ID:
        case SDRPLAY_RSPduo_ID: case SDRPLAY_RSPdx_ID: case SDRPLAY_RSPdxR2_ID: return true;
        default: return false;
    }
}
bool SdrplaySource::hasBiasT() const { return hasDabNotch() || impl_->dev.hwVer == SDRPLAY_RSP2_ID; }

std::string SdrplaySource::model() const {
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP1_ID:    return "RSP1";
        case SDRPLAY_RSP1A_ID:   return "RSP1A";
        case SDRPLAY_RSP1B_ID:   return "RSP1B";
        case SDRPLAY_RSP2_ID:    return "RSP2";
        case SDRPLAY_RSPduo_ID:  return "RSPduo";
        case SDRPLAY_RSPdx_ID:   return "RSPdx";
        case SDRPLAY_RSPdxR2_ID: return "RSPdx-R2";
        default:                 return "RSP";
    }
}

void SdrplaySource::setLnaState(int state) {
    if (!impl_->params || !impl_->params->rxChannelA) return;
    const int n = lnaStateCount();
    if (state < 0) state = 0;
    if (state >= n) state = n - 1;
    impl_->params->rxChannelA->tunerParams.gain.LNAstate = (unsigned char)state;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setIfGainReduction(int gRdB) {
    if (!impl_->params || !impl_->params->rxChannelA) return;
    // ★★ REFUSED WHILE THE AGC IS ON. The API's own documentation is explicit that IFGR
    // cannot be adjusted with AGC enabled — and writing it anyway is exactly the "bodge" that
    // makes SDRplay AGC behave worse under third-party software than under SDRuno, despite
    // being the same API underneath (Stuart, 2026-07-26). Two controllers fighting over one
    // register is not a compromise; it is a bug that presents as poor hardware.
    if (impl_->params->rxChannelA->ctrlParams.agc.enable != sdrplay_api_AGC_DISABLE) return;
    if (gRdB < 20) gRdB = 20;
    if (gRdB > 59) gRdB = 59;
    impl_->params->rxChannelA->tunerParams.gain.gRdB = gRdB;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setIfAgc(bool on) {
    if (!impl_->params || !impl_->params->rxChannelA) return;
    auto& agc = impl_->params->rxChannelA->ctrlParams.agc;
    agc.enable = on ? sdrplay_api_AGC_50HZ : sdrplay_api_AGC_DISABLE;
    // ★★ AND GIVE IT A SENSIBLE SETPOINT. The API default is -60 dBfs, which drives the front
    // end far harder than SDRuno does — which is why "auto gain overloads but manual is fine"
    // is a recurring complaint about SDRplay under third-party software (Stuart, on OWRX).
    // -30 dBfs is the conventional working point and leaves real headroom. The API is not the
    // problem; it is being asked for the wrong thing.
    agc.setPoint_dBfs = -30;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setRfNotch(bool on) {
    if (!impl_->params || !impl_->params->rxChannelA || !hasRfNotch()) return;
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP1A_ID: case SDRPLAY_RSP1B_ID:
            // ★ On devParams, not the channel: the notches are in the RF path BEFORE the
            // tuner, so they are a property of the RADIO rather than of a receive channel.
            if (impl_->params->devParams)
                impl_->params->devParams->rsp1aParams.rfNotchEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_Rsp1a_RfNotchControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        default: break;
    }
}

void SdrplaySource::setDabNotch(bool on) {
    if (!impl_->params || !impl_->params->rxChannelA || !hasDabNotch()) return;
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP1A_ID: case SDRPLAY_RSP1B_ID:
            if (impl_->params->devParams)
                impl_->params->devParams->rsp1aParams.rfDabNotchEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_Rsp1a_RfDabNotchControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        default: break;
    }
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
    if (c->paused && *c->paused) return;      // idle: drop, never tear the device down
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
void SdrplaySource::setLnaState(int) {}
void SdrplaySource::setIfGainReduction(int) {}
void SdrplaySource::setIfAgc(bool) {}
void SdrplaySource::setRfNotch(bool) {}
void SdrplaySource::setDabNotch(bool) {}
int  SdrplaySource::lnaStateCount() const { return 0; }
bool SdrplaySource::hasRfNotch() const { return false; }
bool SdrplaySource::hasDabNotch() const { return false; }
bool SdrplaySource::hasBiasT() const { return false; }
std::string SdrplaySource::model() const { return ""; }
int SdrplaySource::bandwidthKHzForRate(double) { return 0; }
}  // namespace vibe

#endif
