// SDRplay (RSP) capture source — see sdrplay_source.h for why this exists.
#include "sdrplay_source.h"

#ifdef VIBE_HAVE_SDRPLAY
#include <sdrplay_api.h>
#include <dlfcn.h>
#include <cstring>
#include <mutex>
#include <vector>
#include <atomic>
#include <future>
#include <chrono>
#include <cstdio>

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
    if (g_apiRefs > 0) --g_apiRefs;
    // ★★★ WE NEVER CALL sdrplay_api_Close(). Closing the API around a device release is what
    // leaves the SYSTEM-WIDE lock held: the service is left mid-operation and every later
    // caller — in any process — blocks forever on LockDeviceApi. Observed as "the USB icon
    // comes up next to it and drops away straight away", and as the API being dead
    // afterwards until the service is restarted (Stuart, 2026-07-26).
    // ★ So the handle is deliberately kept for the life of the process. That is what
    // SDRplay's own software does, and an "unreleased" handle at exit costs nothing —
    // whereas a poisoned lock costs every other program on the machine.
    // ★★ Reference counting is kept because it still governs Select/Release pairing; only
    // the final Close is skipped.
}
}  // namespace

struct SdrplaySource::Impl {
    // ★★ SERIALISES EVERY API-TOUCHING CALL ON THIS OBJECT. Until the stall watchdog existed
    // the only callers were control sockets, which are effectively serialised by the client;
    // restartStream() is called from a WATCHDOG THREAD while gain changes keep arriving, so
    // Uninit/Init could otherwise run underneath an in-flight Update on the same handle.
    std::recursive_mutex api_mtx;
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

// ★★★ NEVER BLOCK THE CALLER. sdrplay_api_LockDeviceApi() takes a SYSTEM-WIDE shared mutex,
// so ANY other process holding it — including one that has crashed or been killed while
// inside the API — blocks us forever. This is not hypothetical: it hung VibeServer at launch
// with no menu bar icon and no crash report, because enumeration runs from Server.init() on
// the main thread (2026-07-26).
// ★★ A HANG IS THE WORST FAILURE MODE. It is worse than a crash: there is no report, nothing
// to see, and nothing to tell the user — so the API is called behind a TIMEOUT and a failure
// to answer is reported as a fact rather than waited on indefinitely.
static std::atomic<bool> g_apiHung{false};
// ★★ ONCE IT IS KNOWN WEDGED, STOP ASKING. A deadline makes a hang SURVIVABLE; it does not
// make it harmless. The status poll ran once a second, so every second spawned another
// detached thread onto a lock that never frees — they pile up, and the wait itself still
// costs the caller its full timeout each time. Polling a stuck lock forever was the real
// bug; the timeout only hid it (Stuart, 2026-07-26: "hanging again due to the api").
// ★ Cleared by an explicit Refresh, which is the user saying "I have fixed it, try again" —
// the only signal that actually means anything here.
std::atomic<bool> g_apiGiveUp{false};

/** True if the SDRplay API stopped answering. Surfaced to the operator rather than hidden. */
bool SdrplaySource::apiUnresponsive() { return g_apiHung.load() || g_apiGiveUp.load(); }

void SdrplaySource::retryApi() {
    // The operator has pressed Refresh — they are asserting the API is healthy again, which
    // is the only signal worth acting on. One more attempt is allowed.
    g_apiGiveUp.store(false);
    g_apiHung.store(false);
}

namespace {
/** Run an API call with a deadline. Returns false if it did not answer in time. */
template <typename F>
bool withTimeout(F&& fn, int ms) {
    // ★ The worker is DETACHED deliberately: if the API never returns, joining would inherit
    // the very hang we are avoiding. A leaked blocked thread is a far smaller problem than a
    // frozen application, and it unblocks itself if the API ever recovers.
    auto promise = std::make_shared<std::promise<void>>();
    auto fut = promise->get_future();
    std::thread([promise, fn = std::forward<F>(fn)]() mutable {
        fn();
        promise->set_value();
    }).detach();
    if (fut.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready) {
        g_apiHung.store(false);
        return true;
    }
    g_apiHung.store(true);
    return false;
}
}  // namespace

int SdrplaySource::deviceCount() {
    if (g_apiGiveUp.load()) return 0;      // wedged and already reported — do not re-block
    int out = 0;
    withTimeout([&out]{
        std::string err;
        if (!apiOpen(err)) return;
        sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
        unsigned int n = 0;
        api().Lock();
        api().GetDevices(devs, &n, SDRPLAY_MAX_DEVICES);
        api().Unlock();
        apiClose();
        out = (int)n;
    }, 2000);
    if (g_apiHung.load()) g_apiGiveUp.store(true);
    return out;
}

std::string SdrplaySource::deviceName(int index) {
    if (g_apiGiveUp.load()) return "";
    std::string name;
    withTimeout([&name, index]{ name = deviceNameLocked(index); }, 2000);
    return name;
}

std::string SdrplaySource::deviceNameLocked(int index) {
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
// Deliberately holds only what the callbacks touch — a raw device handle rather than the
// private Impl, so this needs no access to the class's internals.
struct CbCtx { std::vector<int16_t>* ilv; SdrplaySource::IqSink* sink; bool* lost; bool* paused;
               bool* overload; HANDLE dev;
               // The AGC's live figures — see the note on liveGr_ in the header.
               std::atomic<int>* gr; std::atomic<int>* lnaGr; std::atomic<float>* gain;
               std::atomic<bool>* valid; };
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
    // ★ Remember WHICH radio this is, by serial. reopen() cannot trust the index — enumeration
    //   order is not stable across a re-plug. Captured here, while the device struct is fresh.
    curSerial_ = impl_->dev.SerNo[0] ? std::string(impl_->dev.SerNo) : std::string();

    if ((e = api().GetDeviceParams(impl_->dev.dev, &impl_->params))
            != sdrplay_api_Success || !impl_->params) {
        close();
        err = std::string("GetDeviceParams: ") + errStr(e);
        return false;
    }

    auto* dp = impl_->params->devParams;
    auto* ch = impl_->params->rxChannelA;
    if (sampleRateHz < 2000000.0) sampleRateHz = 2000000.0;   // zero-IF minimum
    if (dp) dp->fsFreq.fsHz = sampleRateHz;
    if (ch) {
        ch->tunerParams.rfFreq.rfHz = centreHz;
        // ★ Zero-IF with a bandwidth wide enough for the whole FM MPX. RDS rides at 57 kHz,
        // so anything narrower than ~200 kHz would remove the very thing this device was
        // fetched to receive — and it would do so silently, looking like a decode failure.
        ch->tunerParams.bwType = (sdrplay_api_Bw_MHzT)bandwidthKHzForRate(sampleRateHz);
        // ★★ ZERO-IF IS ONLY LEGAL AT 2 MHz AND ABOVE. Below that the RSP needs a LOW-IF
        // mode with its own bandwidth pairings, so the rate is raised to the minimum rather
        // than asking for a configuration the hardware will refuse or silently mangle.
        // ★ Narrow spans should eventually use the API's DECIMATION — ADC at a legal rate,
        // decimated to the output rate — which is how Soapy reaches 62.5 kHz. Not built yet,
        // so the rate list stops at 2 MHz and this clamp is the belt to that braces.
        ch->tunerParams.ifType = sdrplay_api_IF_Zero;
        ch->ctrlParams.decimation.enable = 0;
        ch->ctrlParams.dcOffset.DCenable = 1;
        ch->ctrlParams.dcOffset.IQenable = 1;
    }
    // ★ Give the AGC real loop dynamics BEFORE it starts. The API defaults them all to zero,
    // which is not "fast" but "undefined behaviour dressed as a default".
    // ★ Before Init, so the struct handed to the API is already coherent: a starting IF
    // reduction, a target, and real loop dynamics.
    ch->tunerParams.gain.gRdB = 59;      // least gain until the AGC has settled — see setIfAgc
    setIfAgcSetPoint(-30);
    // ★★★ SLOW, RELUCTANT DECAY — AND A FAST ATTACK. These were SDRconnect's numbers
    //     (500/500/200/5), and SDRconnect is tuned for a listener on ONE channel. This receiver is
    //     locked across 8 MHz of HF, so the AGC's input is the WHOLE BAND: the Buzzer, a broadcast
    //     carrier fading up, any one of a hundred signals nobody is listening to moves the gain for
    //     EVERY listener at once. MEASURED on the Pi at 4625 kHz: 94 gain moves a minute, 228 dB of
    //     travel a minute, over a 10 dB range — a change every 0.6 s, which draws as horizontal
    //     banding across the entire waterfall (Stuart, 2026-08-05: "the AGC going up and down like
    //     a YoYo"). tools/vibeserver-probes/agcpump.mjs is the measurement.
    // ★★ THE ASYMMETRY IS THE WHOLE FIX, and it follows from what each half is FOR:
    //     - ATTACK protects the ADC from a real overload, and an overload is destructive and
    //       instant. It stays fast. Slowing it to reduce pumping would trade a cosmetic problem
    //       for a damaging one.
    //     - DECAY only ever buys back a few dB of sensitivity, and nothing is harmed by sitting
    //       slightly low. Every decay step, by contrast, is a visible brightness jump across the
    //       band for everyone watching. So it is slow, delayed, and needs a bigger excursion
    //       before it will act at all.
    // ★ decay_threshold is the hysteresis: 5 dB is inside the ordinary breathing of an HF band,
    //   so the loop was chasing QSB it should have ignored. 10 dB is a real change.
    setIfAgcDynamics(/*attack*/ 500, /*decay*/ 5000, /*decayDelay*/ 5000, /*threshold*/ 20);
    setGainTenthDb(gainTenthDb);

    static CbCtx ctx;
    ctx = CbCtx{ &impl_->ilv, &sink_, &lost_, &paused_, &overload_, impl_->dev.dev,
                 &liveGr_, &liveLna_, &liveGain_, &liveValid_ };
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
    curRate_ = sampleRateHz; curCentre_ = centreHz; curGain_ = gainTenthDb;

    // ★★★ CYCLE THE AGC AFTER Init. Setting agc.enable in the params struct BEFORE Init does
    // not start the loop — only a transition does, applied through Update once the device is
    // running. So we perform exactly the sequence Stuart found by hand: off, then on.
    // ★ This is the last of three separate reasons SDRplay AGC misbehaves under third-party
    // software, and the one no amount of reading the struct would have revealed. It was found
    // by a user noticing that toggling the switch fixed it, then by narrowing what the toggle
    // actually did: first the gRdB seed (fixed), then this transition (2026-07-26).
    // ★★ A SETTING THAT ONLY TAKES EFFECT ON CHANGE is not a setting, it is an event — and
    // initialising it by assignment silently does nothing.
    {
        auto& agc = impl_->params->rxChannelA->ctrlParams.agc;
        const auto want = agc.enable;
        if (want != sdrplay_api_AGC_DISABLE) {
            agc.enable = sdrplay_api_AGC_DISABLE;
            api().Update(impl_->dev.dev, impl_->dev.tuner,
                         sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
            agc.enable = want;
            api().Update(impl_->dev.dev, impl_->dev.tuner,
                         sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
        }
    }
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
    curCentre_ = hz;                      // remembered for reopen()
    impl_->params->rxChannelA->tunerParams.rfFreq.rfHz = hz;
    api().Update(impl_->dev.dev, impl_->dev.tuner,
                       sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setSampleRate(double hz) {
    if (!open_ || !impl_->params || !impl_->params->devParams) return;
    if (hz < 2000000.0) hz = 2000000.0;      // zero-IF minimum — see open()
    curRate_ = hz;                           // remembered for reopen()
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

// ★★★ THE STREAM DIED BUT NOTHING SAID SO. See the header for the failure mode; this is the
// recovery. Uninit tears down only the streaming half, leaving the selected device and the
// params struct intact, so re-Initing restores the session with the operator's gain, AGC and
// tuning still in place — nothing to re-push and nothing for the listener to notice beyond a
// gap. The AGC transition is repeated because Init resets the loop and, as open() records at
// length, agc.enable only takes effect on a CHANGE.
bool SdrplaySource::restartStream(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    if (!open_ || !impl_->selected) { err = "device not open"; return false; }

    if (impl_->streaming) {
        // ★ A failure here is EXPECTED and must not abort the restart: the whole reason we are
        // in this function is that the API is misbehaving, and refusing to re-Init because the
        // teardown of a already-broken stream complained would leave the radio dead for good.
        const sdrplay_api_ErrT ue = api().Uninit(impl_->dev.dev);
        if (ue != sdrplay_api_Success)
            std::fprintf(stderr, "sdrplay restart: Uninit said %s - continuing anyway\n", errStr(ue));
        impl_->streaming = false;
    }

    static CbCtx ctx;
    ctx = CbCtx{ &impl_->ilv, &sink_, &lost_, &paused_, &overload_, impl_->dev.dev,
                 &liveGr_, &liveLna_, &liveGain_, &liveValid_ };
    sdrplay_api_CallbackFnsT fns{};
    fns.StreamACbFn = &streamCb;
    fns.StreamBCbFn = nullptr;
    fns.EventCbFn   = &eventCb;
    const sdrplay_api_ErrT e = api().Init(impl_->dev.dev, &fns, &ctx);
    if (e != sdrplay_api_Success) {
        err = std::string("re-Init: ") + errStr(e);
        std::fprintf(stderr, "sdrplay restart FAILED: %s\n", err.c_str());
        return false;
    }
    impl_->streaming = true;
    lost_ = false;

    if (impl_->params && impl_->params->rxChannelA) {
        auto& agc = impl_->params->rxChannelA->ctrlParams.agc;
        const auto want = agc.enable;
        if (want != sdrplay_api_AGC_DISABLE) {
            agc.enable = sdrplay_api_AGC_DISABLE;
            api().Update(impl_->dev.dev, impl_->dev.tuner,
                         sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
            agc.enable = want;
            api().Update(impl_->dev.dev, impl_->dev.tuner,
                         sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
        }
    }
    std::fprintf(stderr, "sdrplay stream re-initialised after a stall\n");
    return true;
}

// ★★★ See the header for why this exists, why it is safe here, and why it is the LAST resort.
// The short version: a nudged USB plug re-enumerates the device, so the selected handle is dead
// and no amount of Uninit + Init on it can ever work. Only a full teardown and a fresh
// SelectDevice can, which is precisely what stopping and starting the server was doing by hand.
bool SdrplaySource::reopen(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);

    // Take the live settings BEFORE the teardown — close() drops the params struct they live in.
    const std::string serial = curSerial_;
    const double rate   = curRate_   > 0.0 ? curRate_   : 2000000.0;
    const double centre = curCentre_;
    const int    gain   = curGain_;

    // ★ Unconditional, and it must not care whether any of it succeeds. We are here because the
    //   device is already gone; a complaint from Uninit or ReleaseDevice about a handle that no
    //   longer exists is the EXPECTED outcome, not a reason to leave the radio dead.
    close();

    // ★★ FIND IT AGAIN BY SERIAL, and accept that it may not be back yet. A device that is still
    //    re-enumerating simply is not in the list — that is a failure this attempt, not a
    //    permanent one, and nothing about our state is poisoned by it. This is exactly the
    //    property the Airspy's deep restart lacked, which is what made ONE missed attempt there
    //    permanent (2026-08-02).
    if (!apiOpen(err)) return false;
    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int n = 0;
    api().Lock();
    api().GetDevices(devs, &n, SDRPLAY_MAX_DEVICES);
    api().Unlock();

    int idx = -1;
    for (unsigned int i = 0; i < n; i++) {
        if (serial.empty()) { idx = (int)i; break; }              // only one radio, no serial read
        if (devs[i].SerNo[0] && serial == devs[i].SerNo) { idx = (int)i; break; }
    }
    if (idx < 0) {
        err = serial.empty() ? "no SDRplay device present"
                             : "the RSP has not come back yet (serial " + serial + ")";
        std::fprintf(stderr, "sdrplay reopen: %s\n", err.c_str());
        return false;
    }

    // open() replays the whole sequence — params, bandwidth for the rate, the gRdB seed, the AGC
    // dynamics and the post-Init AGC transition. Reusing it is the point: a hand-rolled subset
    // here would drift out of step with open() the first time that sequence changed, and the AGC
    // in particular fails SILENTLY when a step is missed (see open()).
    if (!open(idx, rate, centre, gain, err)) {
        std::fprintf(stderr, "sdrplay reopen FAILED: %s\n", err.c_str());
        return false;
    }
    std::fprintf(stderr, "sdrplay: device reopened after a re-enumeration (serial %s)\n",
                 serial.empty() ? "unknown" : serial.c_str());
    return true;
}

void SdrplaySource::setGainTenthDb(int tenthDb) {
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    curGain_ = tenthDb;                   // remembered for reopen()
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
    // ★ ZERO MEANS UNSET, NOT "no gain". vs_default_config leaves gainTenthDb at 0, and
    // reading that literally mapped it to LNA state 9 — MINIMUM RF gain — so a freshly
    // started server sat at the bottom of its range with the slider pinned at 0 while the
    // readout said otherwise (Stuart, 2026-07-26). A sentinel and a value must not share a
    // representation.
    if (tenthDb <= 0) {
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
    // ★★ THE SMALLEST BANDWIDTH THAT COVERS THE WHOLE SPAN — deliberately NOT Soapy's
    // mapping, which picks the largest bandwidth BELOW the rate. That is right for a
    // receiver decoding one channel, and wrong for a SPECTRUM DISPLAY: at 2.4 MSPS it fits a
    // 1.536 MHz filter, so the outer third of the visible span is filtered away and the
    // waterfall shows a great rolled-off dome that looks like broken calibration (Stuart,
    // on air 2026-07-26 — "waterfall calibration is well off").
    // ★ A filter narrower than the span does not merely dim the edges, it makes the display
    // lie about what is on the air out there.
    const double k = fs / 1000.0;
    if (k <= 200)  return 200;
    if (k <= 300)  return 300;
    if (k <= 600)  return 600;
    if (k <= 1536) return 1536;
    if (k <= 5000) return 5000;
    if (k <= 6000) return 6000;
    if (k <= 7000) return 7000;
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

float SdrplaySource::systemGainDb() const {
    // Prefer what the AGC last REPORTED; fall back to the struct only before the first event.
    if (liveValid_.load(std::memory_order_relaxed))
        return liveGain_.load(std::memory_order_relaxed);
    if (!open_ || !impl_->params || !impl_->params->rxChannelA) return 0.0f;
    return impl_->params->rxChannelA->tunerParams.gain.gainVals.curr;
}
int SdrplaySource::currentIfGr() const {
    if (liveValid_.load(std::memory_order_relaxed))
        return liveGr_.load(std::memory_order_relaxed);
    if (!impl_->params || !impl_->params->rxChannelA) return 0;
    return impl_->params->rxChannelA->tunerParams.gain.gRdB;
}
int SdrplaySource::currentLnaState() const {
    if (!impl_->params || !impl_->params->rxChannelA) return 0;
    return (int)impl_->params->rxChannelA->tunerParams.gain.LNAstate;
}

void SdrplaySource::setLnaState(int state) {
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    if (!impl_->params || !impl_->params->rxChannelA) return;
    const int n = lnaStateCount();
    if (state < 0) state = 0;
    if (state >= n) state = n - 1;
    impl_->params->rxChannelA->tunerParams.gain.LNAstate = (unsigned char)state;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setIfGainReduction(int gRdB) {
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
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
    auto* ch = impl_->params->rxChannelA;
    auto& agc = ch->ctrlParams.agc;
    // ★★ SEED gRdB BEFORE ENABLING. The loop starts from whatever the IF reduction register
    // holds, and if it has never been written the AGC does not engage — it sat inert with a
    // huge apparent gain until the user disabled AGC, moved the IF slider (which writes
    // gRdB) and re-enabled it. That workaround IS the diagnosis: write the register first,
    // then hand it over (Stuart, 2026-07-26).
    // ★ Order matters and must not be reversed: writing gRdB AFTER enabling is refused,
    // because the AGC owns it by then.
    // ★★ SEED WHENEVER TURNING ON, not only on a transition from DISABLE. The API's DEFAULT
    // for agc.enable is already AGC_50HZ — so a "currently off?" guard never fires at
    // startup, gRdB is left at the API default, and the loop still has no starting point.
    // That is why seeding it appeared not to fix anything on a restart (Stuart, 2026-07-26).
    // ★ A guard that tests for a state the system is never in is the same as no guard at all,
    // and it reads as though the case were handled.
    if (on) {
        // ★★ MINIMUM GAIN during the handover, not mid. Two reasons, and both matter:
        // there is a brief window with the AGC disengaged where a strong signal could
        // overload (Stuart), and starting the loop at minimum gain makes it ramp UP to its
        // target rather than starting hot and clipping on the way down. Approaching from
        // the quiet side is always the safe direction for an automatic gain control.
        ch->tunerParams.gain.gRdB = 59;      // 59 dB of reduction = least gain
        if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
        // ★★★ ENABLING IS ALWAYS A TRANSITION. The API starts the loop on a CHANGE, so
        // "enable" when it is already enabled does nothing at all — and that is precisely
        // how the fix kept coming undone: the client re-sends its saved settings on connect,
        // which cancelled the start-up kick and then performed a no-op enable, leaving the
        // AGC inert again (Stuart, 2026-07-26 — "now its stuck again").
        // ★ Make the operation IDEMPOTENT IN EFFECT rather than in bytes written: force the
        // register through DISABLE first, so asking for AGC always yields a running AGC no
        // matter what state it was in or who asked.
        if (open_ && agc.enable != sdrplay_api_AGC_DISABLE) {
            agc.enable = sdrplay_api_AGC_DISABLE;
            api().Update(impl_->dev.dev, impl_->dev.tuner,
                         sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
        }
    }
    agc.enable = on ? sdrplay_api_AGC_50HZ : sdrplay_api_AGC_DISABLE;
    // ★ The setpoint is set separately (setIfAgcSetPoint) and NOT forced here — it is a
    // user-facing target, so toggling the AGC must not quietly discard their choice.
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setIfAgcSetPoint(int dBfs) {
    if (!impl_->params || !impl_->params->rxChannelA) return;
    if (dBfs > -10) dBfs = -10;
    if (dBfs < -72) dBfs = -72;
    impl_->params->rxChannelA->ctrlParams.agc.setPoint_dBfs = dBfs;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
}

void SdrplaySource::setIfAgcDynamics(int attackMs, int decayMs, int delayMs, int threshDb) {
    if (!impl_->params || !impl_->params->rxChannelA) return;
    auto& agc = impl_->params->rxChannelA->ctrlParams.agc;
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    agc.attack_ms          = (unsigned short)clampi(attackMs, 0, 5000);
    agc.decay_ms           = (unsigned short)clampi(decayMs, 0, 5000);
    agc.decay_delay_ms     = (unsigned short)clampi(delayMs, 0, 5000);
    agc.decay_threshold_dB = (unsigned short)clampi(threshDb, 0, 40);
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

static void eventCb(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner,
                    sdrplay_api_EventParamsT* prm, void* ctx) {
    auto* c = (CbCtx*)ctx;
    if (!c) return;
    if (id == sdrplay_api_PowerOverloadChange && prm) {
        if (c->overload)
            *c->overload = (prm->powerOverloadParams.powerOverloadChangeType
                            == sdrplay_api_Overload_Detected);
        // ★ ACKNOWLEDGE, or the API stops reporting overloads entirely — and a warning that
        // fires once and then goes quiet is worse than none, because its silence reads as
        // "all clear".
        if (c->dev)
            api().Update(c->dev, tuner,
                         sdrplay_api_Update_Ctrl_OverloadMsgAck,
                         sdrplay_api_Update_Ext1_None);
        return;
    }
    // ★★★ THE AGC TELLS US WHAT IT DID, HERE AND NOWHERE ELSE. Without this the readouts show only
    //     what WE last wrote — so with the AGC in charge the IF slider never moved and the system
    //     gain never changed, while the loop was in fact working (Stuart, 2026-08-03: "auto gain is
    //     working but not reporting its figures").
    if (id == sdrplay_api_GainChange && prm) {
        if (c->gr)    c->gr->store((int)prm->gainParams.gRdB, std::memory_order_relaxed);
        if (c->lnaGr) c->lnaGr->store((int)prm->gainParams.lnaGRdB, std::memory_order_relaxed);
        if (c->gain)  c->gain->store((float)prm->gainParams.currGain, std::memory_order_relaxed);
        if (c->valid) c->valid->store(true, std::memory_order_relaxed);
        return;
    }
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
bool SdrplaySource::restartStream(std::string&) { return false; }
bool SdrplaySource::reopen(std::string&) { return false; }
void SdrplaySource::setGainTenthDb(int) {}
void SdrplaySource::setBiasT(bool) {}
void SdrplaySource::setLnaState(int) {}
void SdrplaySource::setIfGainReduction(int) {}
void SdrplaySource::setIfAgc(bool) {}
void SdrplaySource::setIfAgcSetPoint(int) {}
void SdrplaySource::setIfAgcDynamics(int, int, int, int) {}
void SdrplaySource::setRfNotch(bool) {}
void SdrplaySource::setDabNotch(bool) {}
int  SdrplaySource::lnaStateCount() const { return 0; }
bool SdrplaySource::hasRfNotch() const { return false; }
bool SdrplaySource::hasDabNotch() const { return false; }
bool SdrplaySource::hasBiasT() const { return false; }
std::string SdrplaySource::model() const { return ""; }
bool SdrplaySource::apiUnresponsive() { return false; }
void SdrplaySource::retryApi() {}
std::string SdrplaySource::deviceNameLocked(int) { return ""; }
float SdrplaySource::systemGainDb() const { return 0.0f; }
int SdrplaySource::currentIfGr() const { return 0; }
int SdrplaySource::currentLnaState() const { return 0; }
int SdrplaySource::bandwidthKHzForRate(double) { return 0; }
}  // namespace vibe

#endif
