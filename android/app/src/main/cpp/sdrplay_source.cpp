// SDRplay (RSP) capture source — see sdrplay_source.h for why this exists.
#include "sdrplay_source.h"

#ifdef VIBE_HAVE_SDRPLAY
#include <sdrplay_api.h>
#include <dlfcn.h>
#include <cstring>
#include <algorithm>     // ★ std::find — the antenna name check
#include <mutex>
#include <vector>
#include <atomic>
#include <future>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cmath>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#include "vibedsp/neon_compat.h"
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

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
        // ★★ SAY WHAT TO DO, NOT JUST WHAT IS WRONG. This is the FIRST thing an owner meets after
        //   `apt install vibeserver` on a fresh box with an RSP: the API is SDRplay's own
        //   installer into /opt and we are not permitted to redistribute it, so the package
        //   cannot pull it in and no amount of dependency declaration will help. Without the URL
        //   the message reads as "your radio is broken" rather than "one more download".
        err = "SDRplay API not installed. The RSP needs SDRplay's own driver, which we are not "
              "allowed to redistribute — download it from https://www.sdrplay.com/downloads "
              "(API/HW driver for Linux), run the installer, then restart VibeServer.";
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
               std::atomic<bool>* valid;
               // ★ Cleared by a GainChange event — see liveStale_ in the header.
               std::atomic<bool>* stale;
               // ★ OUR OWN LEVEL MEASUREMENT — see adcPeakDbfs() in the header for why the RSP
               //   needs one of its own rather than borrowing the AGC's reduction figure.
               std::atomic<double>* peak; std::atomic<double>* clip; std::atomic<unsigned>* wins;
               std::atomic<unsigned>* gen;
               // ★ Set when the API reports sdrplay_api_DeviceFailure — its own words.
               std::atomic<bool>* apiFailed; };
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
    applyDuoChoice();            // ★ Duo only: pick the tuner BEFORE SelectDevice freezes it
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
    /* ★ Hi-Z on the Duo is the AM PORT of tuner 1, set on the channel params once they exist —
     *  the tuner itself was chosen before SelectDevice (applyDuoChoice). Both halves are written
     *  every time so the two cannot disagree: picking "Tuner 1 50Ω" must put the AM port BACK,
     *  or the 50Ω socket stays bypassed and the radio reads as deaf above the AM band. */
    if (impl_->dev.hwVer == SDRPLAY_RSPduo_ID && ch) {
        ch->rspDuoTunerParams.tuner1AmPortSel =
            (antenna_ == "Tuner 1 Hi-Z") ? sdrplay_api_RspDuo_AMPORT_1
                                         : sdrplay_api_RspDuo_AMPORT_2;
    }
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
                 &liveGr_, &liveLna_, &liveGain_, &liveValid_, &liveStale_,
                 &peakDbfs_, &clipPct_, &windows_, &gen_, &apiFailed_ };
    sdrplay_api_CallbackFnsT fns{};
    fns.StreamACbFn = &streamCb;
    fns.StreamBCbFn = nullptr;
    fns.EventCbFn   = &eventCb;
    // ★★★ AN ILLEGAL GAIN PAIR MUST NOT MAKE THE RADIO UNSTARTABLE. The API validates the
    //     COMBINATION, not the parts: it rejected `absolute gRdB=59 LNAstate=7` even though 59 is
    //     the legal maximum IF reduction and 7 is a legal LNA state, because the TOTAL reduction
    //     those two imply is out of range. Init then fails, and the whole receiver is down until
    //     somebody edits a config file — a reboot cannot help, because the offending number is
    //     saved (Stuart, 2026-08-10: "the SDRplay is broken ... and I've rebooted the Pi too").
    //
    // ★★ SO: BACK OFF AND TRY AGAIN, rather than refusing to start. Each rung asks for LESS total
    //    reduction than the last, so it walks toward the legal region from whichever side it began
    //    on. We do not model the per-band LNA reduction table to predict the limit — that table is
    //    the API's business and would be one more thing to keep in step with new hardware. Asking
    //    is cheaper and cannot go stale.
    // ★ IT SAYS WHAT IT DID. A receiver quietly running at a gain nobody chose is its own bug, and
    //   the owner needs to know their setting was not honoured — that is the difference between
    //   "it recovered" and "it lies".
    e = api().Init(impl_->dev.dev, &fns, &ctx);
    if (e != sdrplay_api_Success && impl_->params && impl_->params->rxChannelA) {
        auto* chg = impl_->params->rxChannelA;
        const int n = lnaStateCount();
        const int mid = n > 0 ? n / 2 : 0;
        const struct { int gr; int lna; const char* why; } ladder[] = {
            { 40, -1,  "IF reduction 40" },                       // most likely culprit first
            { 40, mid, "IF reduction 40, LNA to the middle" },
            { 20,  0,  "IF reduction 20, LNA wide open" },        // the least total reduction there is
        };
        const int wantedGr = (int)chg->tunerParams.gain.gRdB;
        const int wantedLna = (int)chg->tunerParams.gain.LNAstate;
        for (const auto& rung : ladder) {
            chg->tunerParams.gain.gRdB = (float)rung.gr;
            if (rung.lna >= 0) chg->tunerParams.gain.LNAstate = (unsigned char)rung.lna;
            e = api().Init(impl_->dev.dev, &fns, &ctx);
            if (e == sdrplay_api_Success) {
                std::fprintf(stderr,
                    "VibeServer: the radio refused gain (gRdB %d, LNAstate %d) — started with %s "
                    "instead. Your saved gain was not applied; set it again and it will be kept.\n",
                    wantedGr, wantedLna, rung.why);
                break;
            }
        }
    }
    if (e != sdrplay_api_Success) {
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
    /* ★★★ UNDER api_mtx, LIKE EVERY OTHER API-TOUCHING CALL — measured 2026-09-15 19:51 on the
     *     Lenovo: the stall watchdog was inside reopen() with Uninit hung for 20 s on a service
     *     that had stopped answering, the idle release fired on another thread and ran THIS with
     *     no lock, and the two teardowns of one device struct ended in SIGSEGV (status=11). The
     *     mutex is recursive, so reopen() closing its own device is unaffected; the release
     *     simply waits its turn and then closes whatever the reopen left. */
    if (!impl_) { open_ = false; return; }
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    if (impl_->streaming) {
        const sdrplay_api_ErrT ue = api().Uninit(impl_->dev.dev);
        if (ue == sdrplay_api_ServiceNotResponding) serviceDead_.store(true, std::memory_order_relaxed);
        impl_->streaming = false;
    }
    if (impl_->selected)  { api().ReleaseDevice(&impl_->dev); impl_->selected = false; }
    if (open_) apiClose();
    open_ = false;
}

void SdrplaySource::setFrequency(double hz) {
    /* ★★★ UNDER THE SAME LOCK AS THE GAIN WRITES, OR THE TUNE CAN BE LOST. Every other writer
     *     of the device parameter block takes api_mtx — setLnaState, setIfGainReduction,
     *     setSampleRate — and this one did not. The tune runs on the hardware-writer thread while
     *     the gain loop writes from its own, both mutating impl_->params and both calling
     *     Update() on the same device.
     *  ★ It became a live fault the moment the gain loop started writing AT RETUNE TIME (the
     *     remembered resting gain), which is precisely when the frequency write is in flight.
     *     Stuart: "large jumps seems to make the signal misaligned", "had to move up 200KHz and
     *     back again" — a small move only shifts the VFO inside the existing capture and never
     *     touches the hardware, which is exactly why only LARGE jumps showed it.
     *  ★★ The mutex is recursive, so callers already holding it are unaffected. */
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    if (!open_ || !impl_->params || !impl_->params->rxChannelA) return;
    curCentre_ = hz;                      // remembered for reopen()
    impl_->params->rxChannelA->tunerParams.rfFreq.rfHz = hz;
    api().Update(impl_->dev.dev, impl_->dev.tuner,
                       sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
    /* ★★★ AND RECALIBRATE THE DC OFFSET, BECAUSE IT IS FREQUENCY-DEPENDENT TOO. It is not only
     *     gain that shifts it — a retune does as well, and without this the correction computed
     *     for the OLD frequency goes on being applied to the new one until the tuner's periodic
     *     calibration next comes round. A wrong DC correction is not a null operation: it injects
     *     an offset rather than removing one.
     * ★★★ THIS IS THE SHAPE OF A FAULT STUART HAS SEEN FOR A LONG TIME, on FM and AM alike and
     *     long before any AGC work: "you click on a signal and it will be all distorted broken up
     *     and nasty, tune slightly away then back again and its perfect", and "moved to 96.6
     *     broken and distorted so I checked the gain no overload reported so tuned to 96.7 then
     *     back to 96.6 and it cleaned up" (2026-09-12). Tuning away and back works because the
     *     second retune lands while the calibration has caught up. Nothing about it correlates
     *     with signal level, which is exactly what he observed.
     * ★ The same one call that follows a gain change — see dcRecalibrate(). */
    dcRecalibrate();
}

void SdrplaySource::setSampleRate(double hz) {
    if (!open_ || !impl_->params || !impl_->params->devParams) return;
    if (hz < 2000000.0) hz = 2000000.0;      // zero-IF minimum — see open()
    curRate_ = hz;                           // remembered for reopen()
    /* ★ Decimated DAB — see setDabDecimation(). The converter runs at 2x through the 5 MHz
     *   filter and the API hands back the requested rate. Every other rate: no decimation. */
    const bool decim = dabDecim_ && hz > 2040000.0 && hz < 2060000.0;
    impl_->params->devParams->fsFreq.fsHz = decim ? hz * 2.0 : hz;
    if (impl_->params->rxChannelA) {
        auto* ch = impl_->params->rxChannelA;
        // ★ The IF bandwidth must follow the rate, or a wider span arrives already filtered.
        ch->tunerParams.bwType = decim ? sdrplay_api_BW_5_000
                                       : (sdrplay_api_Bw_MHzT)bandwidthKHzForRate(hz);
        ch->ctrlParams.decimation.enable           = decim ? 1 : 0;
        ch->ctrlParams.decimation.decimationFactor = decim ? 2 : 1;
        ch->ctrlParams.decimation.wideBandSignal   = decim ? 1 : 0;
    }
    if (decim) std::fprintf(stderr, "sdrplay: %.0f S/s served as %.0f S/s decimated by 2 through the 5 MHz filter (DAB decimation)\n", hz, hz * 2.0);
    api().Update(impl_->dev.dev, impl_->dev.tuner,
                 (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Dev_Fs
                                              | sdrplay_api_Update_Tuner_BwType
                                              | sdrplay_api_Update_Ctrl_Decimation),
                 sdrplay_api_Update_Ext1_None);
    /* ★★★ A RATE CHANGE SILENCES THE AGC. Measured 2026-09-15 on the RSP1A entering DAB
     *     (3 MS/s -> 2.048): after this Update not one GainChange event arrived, so every readout
     *     fell back to the struct's commanded 59 dB and the RF loop, steering on that echo,
     *     walked the LNA from state 3 to 9. restartStream() already knows the cure — the loop
     *     restarts on a CHANGE of agc.enable — so the same off/on is applied here, and the last
     *     report is marked stale until the loop speaks again. */
    if (impl_->params->rxChannelA) {
        auto& agc = impl_->params->rxChannelA->ctrlParams.agc;
        const auto want = agc.enable;
        if (want != sdrplay_api_AGC_DISABLE) {
            agc.enable = sdrplay_api_AGC_DISABLE;
            api().Update(impl_->dev.dev, impl_->dev.tuner,
                         sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
            agc.enable = want;
            api().Update(impl_->dev.dev, impl_->dev.tuner,
                         sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
            noteAgcRestart();
        }
        liveStale_.store(true, std::memory_order_relaxed);
    }
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
        /* ★★★ A SERVICE THAT DID NOT ANSWER HAS NOT UNINITIALISED ANYTHING. 2026-09-15 19:49: Uninit
         *     said ServiceNotResponding, we carried on, Init said AlreadyInitialised — and so did
         *     every Init after it, for ever, because the stream was still up inside a service that
         *     was no longer listening. Nothing this process can send will cure that: the flag tells
         *     the watchdog to stop re-Initing and ask for the service itself to be restarted. */
        if (ue == sdrplay_api_ServiceNotResponding) serviceDead_.store(true, std::memory_order_relaxed);
        impl_->streaming = false;
    }

    static CbCtx ctx;
    ctx = CbCtx{ &impl_->ilv, &sink_, &lost_, &paused_, &overload_, impl_->dev.dev,
                 &liveGr_, &liveLna_, &liveGain_, &liveValid_, &liveStale_,
                 &peakDbfs_, &clipPct_, &windows_, &gen_, &apiFailed_ };
    sdrplay_api_CallbackFnsT fns{};
    fns.StreamACbFn = &streamCb;
    fns.StreamBCbFn = nullptr;
    fns.EventCbFn   = &eventCb;
    const sdrplay_api_ErrT e = api().Init(impl_->dev.dev, &fns, &ctx);
    if (e != sdrplay_api_Success) {
        err = std::string("re-Init: ") + errStr(e);
        std::fprintf(stderr, "sdrplay restart FAILED: %s\n", err.c_str());
        // ★ "Already initialised" after our own Uninit means the Uninit never landed — the service
        //   is wedged with the stream up. Same verdict as ServiceNotResponding (see above).
        if (e == sdrplay_api_AlreadyInitialised || e == sdrplay_api_ServiceNotResponding)
            serviceDead_.store(true, std::memory_order_relaxed);
        return false;
    }
    impl_->streaming = true;
    lost_ = false;
    serviceDead_.store(false, std::memory_order_relaxed);

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
            noteAgcRestart();
        }
    }
    /* ★★ RE-ASSERT THE FRONT-END FILTERS. Init re-sends the params struct, but a notch Update
     *    issued while the stream was down is lost — measured 2026-09-15: entering DAB, the auto
     *    notch switched the DAB notch off in the struct, the re-init landed on top, and the tuner
     *    kept the notch IN while every readout said off (Stuart: "the DAB notch says its off but
     *    the gain levels say its on still"). Written again from the struct, the way a fresh open
     *    would see them. */
    /* ★★★ AND FOR EVERY MODEL, NOT JUST THE TWO ON THE BENCH. This re-assert existed for the
     *  RSP1A/1B and fell out of a `default: break;` for the RSP2, the Duo and the RSPdx — so on
     *  those radios the fault the note above describes was never actually fixed: a stall re-init
     *  would leave the tuner holding a notch every readout said was off. The models differ in
     *  WHERE the fields live (channel params on the RSP2/Duo, device params on the dx) and in
     *  whether the reason is an ordinary one or Ext1, which is why each needs naming.
     *  ★ Guarded on `params` alone now: the devParams test was right for the RSP1A, whose notches
     *    live there, and wrong as a gate for the radios whose notches do not. */
    if (impl_->params && hasRfNotch()) {
        switch (impl_->dev.hwVer) {
            case SDRPLAY_RSP1A_ID: case SDRPLAY_RSP1B_ID:
                api().Update(impl_->dev.dev, impl_->dev.tuner,
                             (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Rsp1a_RfNotchControl
                                                          | sdrplay_api_Update_Rsp1a_RfDabNotchControl),
                             sdrplay_api_Update_Ext1_None);
                break;
            case SDRPLAY_RSP2_ID:
                // ★ No DAB notch on an RSP2 — hasDabNotch() says so, and asking for one here
                //   would be asking the API for a control this radio has never had.
                api().Update(impl_->dev.dev, impl_->dev.tuner,
                             (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Rsp2_RfNotchControl
                                                          | sdrplay_api_Update_Rsp2_AntennaControl
                                                          | sdrplay_api_Update_Rsp2_AmPortSelect),
                             sdrplay_api_Update_Ext1_None);
                break;
            case SDRPLAY_RSPduo_ID:
                api().Update(impl_->dev.dev, impl_->dev.tuner,
                             (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_RspDuo_RfNotchControl
                                                          | sdrplay_api_Update_RspDuo_RfDabNotchControl),
                             sdrplay_api_Update_Ext1_None);
                break;
            case SDRPLAY_RSPdx_ID:
            case SDRPLAY_RSPdxR2_ID:
                /* ★ The dx carries its antenna selection here too: it lives on devParams beside
                 *  the notches, so a re-init that restated one and not the other could leave the
                 *  radio listening to a different aerial than the one on screen. */
                api().Update(impl_->dev.dev, impl_->dev.tuner,
                             sdrplay_api_Update_None,
                             (sdrplay_api_ReasonForUpdateExtension1T)
                                 (sdrplay_api_Update_RspDx_RfNotchControl
                                | sdrplay_api_Update_RspDx_RfDabNotchControl
                                | sdrplay_api_Update_RspDx_AntennaControl));
                break;
            default: break;
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
        if (err.find("ServiceNotResponding") != std::string::npos || err.find("AlreadyInitialised") != std::string::npos)
            serviceDead_.store(true, std::memory_order_relaxed);
        return false;
    }
    serviceDead_.store(false, std::memory_order_relaxed);
    std::fprintf(stderr, "sdrplay: device reopened after a re-enumeration (serial %s)\n",
                 serial.empty() ? "unknown" : serial.c_str());
    return true;
}

void SdrplaySource::setGainTenthDb(int tenthDb) {
    /* ★ OUR WRITE INVALIDATES THE AGC'S LAST WORD until it answers with a GainChange —
     *   see liveStale_. Without this the readouts kept describing the PREVIOUS setting. */
    liveStale_.store(true, std::memory_order_relaxed);

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
    /* ★★★ 2.048 MS/s IS DAB, AND DAB WANTS THE MATCHED FILTER. The rule above — the smallest
     *     bandwidth covering the whole SPAN — is right for a spectrum display and wrong for this
     *     one case: at 2.048 MS/s it reaches past 1536 and picks the 5 MHz filter, so a 1.536 MHz
     *     multiplex is received through a filter three times too wide and the ADJACENT BLOCKS
     *     come straight in. They sit ±1.7 MHz away, well inside 5 MHz, and they then drive the
     *     gain — which is why 10D, "wedged in between 2 much stronger blocks" (Stuart), has been
     *     the hardest of the lot while its neighbours behaved.
     * ★ A DAB multiplex IS 1.536 MHz. The filter and the signal are the same width, so nothing
     *   wanted is lost and everything unwanted is rejected before it reaches the converter.
     * ★★ The waterfall's outer edges roll off as a result, which the rule above exists to avoid.
     *    Stuart's call, and the right one: "the waterfall is just set dressing to be honest ...
     *    choose the best rate for DAB". On a block you are trying to decode, reception beats
     *    display fidelity — and there is nothing outside the multiplex worth displaying anyway. */
    if (k <= 2100) return 1536;
    if (k <= 5000) return 5000;
    if (k <= 6000) return 6000;
    if (k <= 7000) return 7000;
    return 8000;
}

/** ★★★ HOW MANY LNA STATES THIS RADIO HAS **IN THIS BAND** — IT IS NOT ONE NUMBER.
 *  Every RSP has fewer LNA states below 60 MHz and in L-band than it does in between, and the
 *  API says so itself: RSPIA_NUM_LNA_STATES is 10 but RSPIA_NUM_LNA_STATES_AM is 7 and
 *  _LBAND is 9. We returned the flat per-model number, so on HF the loop was offered three
 *  states that do not exist.
 *  ★ THAT IS WHAT THE 40 m MEASUREMENT WAS SHOWING AND I READ IT AS HARDWARE. Sweeping the
 *    states with the IF pinned gave 47.3, 40.7, 34.3, 9.3, -13.7, -13.7, -13.7 dB — I recorded
 *    "uneven, and saturated above state 6" and built rules around the saturation. There is no
 *    saturation. On HF the RSP1A has SEVEN states; 7, 8 and 9 were the same state, clamped,
 *    and the gain loop spent its time walking into a dead zone it could not tell from a rail.
 *  ★★ The numbers come from the API's own #defines, used as the interface intends — nothing
 *    of SDRplay's is copied or derived. Band edges are the standard ones the API switches on.
 *  ★★★ lnaStateCount() with no argument answers for WHERE THE RADIO IS NOW, so every existing
 *      caller becomes band-correct without changing. */
int SdrplaySource::lnaStateCount() const { return lnaStateCount(curCentre_); }

int SdrplaySource::lnaStateCount(double hz) const {
    const bool am    = hz <  60.0e6;
    const bool b420  = hz >= 420.0e6 && hz < 1000.0e6;
    const bool lband = hz >= 1000.0e6;
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP1_ID:    return 4;                       // one table, all bands
        case SDRPLAY_RSP1A_ID:
        case SDRPLAY_RSP1B_ID:   return am ? 7 : lband ? 9 : 10;
        case SDRPLAY_RSP2_ID:    return b420 ? 6 : 9;
        case SDRPLAY_RSPduo_ID:  return am ? 7 : lband ? 9 : 10;
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID: return hz < 250.0e6 ? 27 : b420 ? 21 : lband ? 19 : 28;
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

/** ★★★ THE TUNER'S DC OFFSET IS GAIN-DEPENDENT, SO IT MUST BE RECALIBRATED WHEN THE GAIN MOVES.
 *  The API corrects DC for us — DCenable and IQenable are both set, and dcCal defaults to 3
 *  (Periodic) — which is exactly why DC spikes are not normally visible in ordinary use: the gain
 *  sits still and the periodic calibration converges on it. Ours does not sit still. Every LNA or
 *  gRdB change shifts the offset, and periodic recalibration then spends its whole interval
 *  chasing, which is when the spikes appear. Stuart asked the right question — "how does SDRPlay
 *  work around the DC spikes, I'm pretty sure you dont see them in regular use" (2026-09-12) —
 *  and the answer is that they do work around them, and the difference here is us.
 *  ★ sdrplay_api_Update_Tuner_DcOffset asks for that calibration NOW rather than at the next
 *    interval. One extra API call per gain change, and gain changes are now rare.
 *  ★★ This is the fix at the source. The AGC also ignores DC when measuring level (see streamCb),
 *     because a loop that counts DC as signal winds itself down chasing its own offset — but that
 *     is robustness, not a substitute for correcting the offset in the first place. */
void SdrplaySource::dcRecalibrate() {
    if (!open_ || !impl_->selected) return;
    api().Update(impl_->dev.dev, impl_->dev.tuner,
                 sdrplay_api_Update_Tuner_DcOffset, sdrplay_api_Update_Ext1_None);
}

float SdrplaySource::systemGainDb() const {
    /* ★★★ EXACTLY THE FAULT currentIfGr() HAD, one function away, and it survived that fix.
     *     This preferred the AGC's reported gain UNCONDITIONALLY — and that value only updates
     *     when the radio's AGC fires a GainChange event. Under VibeAGC that AGC is switched off,
     *     so no events ever come and this froze at whatever it last said, no matter how far the
     *     gain was actually moved. A frozen 6.4 dB on screen while the front end was being walked
     *     four LNA states (Stuart's screenshot, 2026-09-12).
     * ★ With the AGC ON, its report is the truth. With it OFF, the struct is — the API refreshes
     *   gainVals.curr on every Update_Tuner_Gr, so it is live and it is ours.
     * ★★ AND THIS IS NOW LOad-BEARING, not just a readout: VibeAGC reads it either side of an LNA
     *    step to learn what that step was actually WORTH, instead of assuming. See the note on
     *    the compensation in vsSdrplayVibeAgcTick. */
    const bool agcOn = impl_->params && impl_->params->rxChannelA
        && impl_->params->rxChannelA->ctrlParams.agc.enable != sdrplay_api_AGC_DISABLE;
    if (agcOn && liveValid_.load(std::memory_order_relaxed)
              && !liveStale_.load(std::memory_order_relaxed))
        return liveGain_.load(std::memory_order_relaxed);
    /* ★ -999 is "cannot read it", NOT 0.0 — zero is a legitimate system gain on this radio at
     *   medium wave, where the LNA states are attenuators, so returning 0 for "unknown" made a
     *   real reading and a missing one indistinguishable. The client draws a dash only for the
     *   sentinel. */
    if (!open_ || !impl_->params || !impl_->params->rxChannelA) return -999.0f;
    return impl_->params->rxChannelA->tunerParams.gain.gainVals.curr;
}
float SdrplaySource::structGainDb() const {
    /* ★ Deliberately NOT systemGainDb(): no event value, no AGC preference. See the header. */
    if (!open_ || !impl_->params || !impl_->params->rxChannelA) return -999.0f;
    return impl_->params->rxChannelA->tunerParams.gain.gainVals.curr;
}
int SdrplaySource::currentIfGr() const {
    if (!impl_->params || !impl_->params->rxChannelA) return 0;
    /* ★★★ THE AGC'S EVENT IS ONLY THE TRUTH WHILE THE AGC IS RUNNING. This preferred liveGr_
     *     unconditionally — and that value only ever updates when the AGC fires an event. Switch
     *     the AGC off and no events come, so the readout froze at whatever the AGC last said and
     *     stayed there no matter what was set by hand: the slider moved, the radio obeyed, and
     *     every figure on screen insisted nothing had happened.
     * ★★★ IT COST HOURS TONIGHT. It is why "the IF AGC is dead" and "it responds fine" were both
     *     observed within minutes of each other — the numbers moved whenever the AGC was on and
     *     firing, and froze the instant anything turned it off. Stuart got there first: "Or if it
     *     is the display isnt showing it working anymore" (2026-09-12).
     * ★ So: with the AGC ON, report what it says. With it OFF, report what we COMMANDED, which is
     *   the struct — the only thing that can be true when nobody is sending events. */
    const bool agcOn = impl_->params->rxChannelA->ctrlParams.agc.enable
                       != sdrplay_api_AGC_DISABLE;
    if (agcOn && liveValid_.load(std::memory_order_relaxed)
              && !liveStale_.load(std::memory_order_relaxed))
        return liveGr_.load(std::memory_order_relaxed);
    return (int)impl_->params->rxChannelA->tunerParams.gain.gRdB;
}
int SdrplaySource::currentLnaState() const {
    if (!impl_->params || !impl_->params->rxChannelA) return 0;
    return (int)impl_->params->rxChannelA->tunerParams.gain.LNAstate;
}

/** ★★★ LEARN THE LNA LADDER FROM THE MOVES THE LOOP ALREADY MAKES — DO NOT SWEEP FOR IT.
 *  The first version wrote every LNA state in turn and read gainVals back. It kept returning
 *  nonsense: ten identical rungs when the sweep crossed a device rebuild, then rungs in exact
 *  duplicated PAIRS ("76.7 76.7 62.1 62.1 ...") whatever delay was used. gainVals is refreshed on
 *  the API's own schedule and a burst of writes outruns it, so the readings belong to the wrong
 *  rungs — and a ladder wrong at one rung sends the loop to exactly the wrong state.
 *  ★ It was also the wrong shape of solution: ten register writes purely to calibrate, on a live
 *    radio with a listener on it, repeated per band.
 *  ★★ THE RADIO IS ALREADY TELLING US. Every settled tick reports the total gain it is
 *    delivering, and the LNA's own contribution is that total plus the IF reduction currently
 *    applied — gRdB is a REDUCTION, so adding it back removes the IF from the figure and leaves
 *    the front end's. Record that against the state we are in, per band, and the ladder fills
 *    itself in from ordinary operation with no writes, no disturbance and no race.
 *  ★★★ STATES WE HAVE NOT VISITED STAY UNKNOWN, and the loop falls back to a single step for
 *      those — which is exactly what it did before, so nothing is worse while it learns and
 *      everything is better once it has. A measurement taken from the radio in its real state
 *      beats one taken from a sweep that disturbs the thing it measures. */
void SdrplaySource::noteLnaGain(int state, float totalGainDb, int ifGrDb) {
    const int band = lnaBandId(curCentre_);
    if (band < 0 || band >= kLnaBands) return;
    if (state < 0 || state >= kLnaStatesMax) return;
    if (totalGainDb < -900.0f || ifGrDb <= 0) return;
    const float lnaOnly = totalGainDb + (float)ifGrDb;   // ★ gRdB is a reduction; add it back
    float& slot = lnaObs_[band][state];
    if (lnaSeen_[band][state]) slot += (lnaOnly - slot) * 0.25f;   // ★ gentle, in case of noise
    else { slot = lnaOnly; lnaSeen_[band][state] = true; }
}

/** The learned gain of an LNA state in the band the radio is in now, or NaN if never visited. */
float SdrplaySource::lnaGainDb(int state) const {
    const int band = lnaBandId(curCentre_);
    if (band < 0 || band >= kLnaBands || state < 0 || state >= kLnaStatesMax) return NAN;
    if (!lnaSeen_[band][state]) return NAN;
    return lnaObs_[band][state];
}

/** ★ Which of the API's gain-table bands a frequency falls in. The tables change at these edges,
 *  so a ladder learned on one side of one does not apply on the other. */
int SdrplaySource::lnaBandId(double hz) {
    if (hz <   60.0e6) return 0;
    if (hz <  250.0e6) return 1;
    if (hz <  420.0e6) return 2;
    if (hz < 1000.0e6) return 3;
    return 4;
}

void SdrplaySource::setLnaState(int state) {
    /* ★ OUR WRITE INVALIDATES THE AGC'S LAST WORD until it answers with a GainChange —
     *   see liveStale_. Without this the readouts kept describing the PREVIOUS setting. */
    liveStale_.store(true, std::memory_order_relaxed);

    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    if (!impl_->params || !impl_->params->rxChannelA) return;
    const int n = lnaStateCount();
    if (state < 0) state = 0;
    if (state >= n) state = n - 1;
    impl_->params->rxChannelA->tunerParams.gain.LNAstate = (unsigned char)state;
    /* ★★★ CARRY THE AGC'S OWN CURRENT gRdB, OR THIS UPDATE CLOBBERS IT. Update_Tuner_Gr submits
     *     the WHOLE gain struct, so it writes gRdB as a side effect — and the struct still holds
     *     whatever was last put there by hand (59, from the start-up kick). setIfGainReduction
     *     below refuses to write gRdB while the AGC owns it, quoting SDRplay's own documentation;
     *     this function was doing exactly that behind its back, every time it was called.
     *
     * ★★★ IT DID NOT MATTER UNTIL TONIGHT. The LNA used to move only on a retune, so the AGC was
     *     knocked once in a while and recovered. The new RF AGC steps it every few seconds, and
     *     the IF AGC stopped dead: ifgr pinned at 52, lna 5, system gain 30.9 dB, unchanged for
     *     twenty-two seconds on a live DAB signal (Stuart, 2026-09-12: "IF agc seems to be broken
     *     fully" — it was, and it was my doing).
     * ★ So write back what the AGC last REPORTED, which makes the gRdB half of the update a
     *   no-op. liveValid_ is false only before the first event; then the struct value stands, as
     *   it did before. */
    /* ★ ONLY A FRESH REPORT IS WRITTEN BACK. A stale one — the AGC has not spoken since our last
     *   write — is whatever it said on another band or before a rate change, and writing it into
     *   gRdB SETS the reduction to that old figure (measured 2026-09-15: 28 dB from medium wave
     *   carried into DAB, system gain 37.7 dB, ADC peak -14 dBFS). Leave the struct alone then. */
    if (liveValid_.load(std::memory_order_relaxed) && !liveStale_.load(std::memory_order_relaxed)) {
        const int gr = liveGr_.load(std::memory_order_relaxed);
        if (gr >= 20 && gr <= 59)
            impl_->params->rxChannelA->tunerParams.gain.gRdB = (float)gr;
    }
    liveStale_.store(true, std::memory_order_relaxed);
    if (open_) {
        api().Update(impl_->dev.dev, impl_->dev.tuner,
                     sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
        dcRecalibrate();          // ★ the DC offset is gain-dependent — see dcRecalibrate()
    }
    /* ★★★ AND GIVE THE AGC ITS REGISTER BACK. Update_Tuner_Gr submits gRdB, and on this API a
     *     manual gain write is precisely how you TAKE the IF reduction away from the AGC — so a
     *     single LNA change silently stops it, and it never runs again until something re-enables
     *     it. Stuart put his finger on it exactly: "it looks like it does the kick then never
     *     works afterwards" (2026-09-12). The kick hands over, the first LNA update takes it back,
     *     and every figure sits still for ever.
     * ★★★ IT IS NOT ONLY THE RF AGC THAT DOES THIS. The retune path calls setLnaState to enforce
     *     a band's gain cap, so an ordinary band change could stop the AGC on any receiver — this
     *     is older than tonight's loop, it simply took a loop that moves the LNA every few seconds
     *     to make it obvious.
     * ★★ RE-ASSERTED BY TRANSITION, not by writing the same value: `agc.enable` only takes effect
     *    on a CHANGE — the reason open() performs a disable/enable dance after Init, and the
     *    reason the start-up kick exists at all. So disable, then enable, exactly as they do.
     * ★ Only when the AGC is supposed to be running; with it off there is nothing to restore. */
    if (open_ && impl_->params->rxChannelA->ctrlParams.agc.enable != sdrplay_api_AGC_DISABLE) {
        auto& agc = impl_->params->rxChannelA->ctrlParams.agc;
        const auto want = agc.enable;
        agc.enable = sdrplay_api_AGC_DISABLE;
        api().Update(impl_->dev.dev, impl_->dev.tuner,
                     sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
        agc.enable = want;
        api().Update(impl_->dev.dev, impl_->dev.tuner,
                     sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
        noteAgcRestart();
    }
}

bool SdrplaySource::setIfGainReduction(int gRdB) {
    /* ★ OUR WRITE INVALIDATES THE AGC'S LAST WORD until it answers with a GainChange —
     *   see liveStale_. Without this the readouts kept describing the PREVIOUS setting. */
    liveStale_.store(true, std::memory_order_relaxed);

    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    if (!impl_->params || !impl_->params->rxChannelA) return false;
    // ★★ REFUSED WHILE THE AGC IS ON. The API's own documentation is explicit that IFGR
    // cannot be adjusted with AGC enabled — and writing it anyway is exactly the "bodge" that
    // makes SDRplay AGC behave worse under third-party software than under SDRuno, despite
    // being the same API underneath (Stuart, 2026-07-26). Two controllers fighting over one
    // register is not a compromise; it is a bug that presents as poor hardware.
    if (impl_->params->rxChannelA->ctrlParams.agc.enable != sdrplay_api_AGC_DISABLE) return false;
    if (gRdB < 20) gRdB = 20;
    if (gRdB > 59) gRdB = 59;
    impl_->params->rxChannelA->tunerParams.gain.gRdB = gRdB;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    return true;
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

int SdrplaySource::ifAgcSetPointDbfs() const {
    if (!impl_->params || !impl_->params->rxChannelA) return -30;
    return impl_->params->rxChannelA->ctrlParams.agc.setPoint_dBfs;
}

void SdrplaySource::restartIfAgc(int gr) {
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    if (!open_ || !impl_->params || !impl_->params->rxChannelA) return;
    auto& agc = impl_->params->rxChannelA->ctrlParams.agc;
    const auto want = agc.enable == sdrplay_api_AGC_DISABLE ? sdrplay_api_AGC_CTRL_EN : agc.enable;
    agc.enable = sdrplay_api_AGC_DISABLE;
    api().Update(impl_->dev.dev, impl_->dev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
    if (gr < 20) gr = 20; if (gr > 59) gr = 59;
    impl_->params->rxChannelA->tunerParams.gain.gRdB = (float)gr;
    api().Update(impl_->dev.dev, impl_->dev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    agc.enable = want;
    api().Update(impl_->dev.dev, impl_->dev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
    liveStale_.store(true, std::memory_order_relaxed);
    noteAgcRestart();
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
        // ★ RSP2 and Duo keep theirs on the CHANNEL params, not the device — see the note above
        //   setDabNotch for why all four models were reported and only two were wired.
        case SDRPLAY_RSP2_ID:
            impl_->params->rxChannelA->rsp2TunerParams.rfNotchEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_Rsp2_RfNotchControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        case SDRPLAY_RSPduo_ID:
            impl_->params->rxChannelA->rspDuoTunerParams.rfNotchEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_RspDuo_RfNotchControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID:
            if (impl_->params->devParams)
                impl_->params->devParams->rspDxParams.rfNotchEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_None,
                                    sdrplay_api_Update_RspDx_RfNotchControl);
            break;
        default: break;
    }
}

/* ★★★ THE NOTCHES AND THE BIAS-T EXISTED ON PAPER FOR FOUR MODELS AND WERE WIRED FOR TWO.
 *
 *  hasRfNotch() answers true for the RSP1A, RSP1B, RSP2, RSPduo, RSPdx and RSPdx-R2; hasDabNotch()
 *  for all but the RSP2; hasBiasT() for all of them. The SETTERS below handled `case RSP1A, RSP1B`
 *  and fell out of a `default: break;` for everything else — so on a Duo, an RSPdx or an RSP2 the
 *  client drew all three switches, the owner pressed them, and the radio never heard a word of it.
 *  Found while adding the antenna selector those same owners are missing (GitHub #29, 2026-09-24).
 *
 *  ★★ SAME FAULT AS THE ANTENNA, AND THE SAME SHAPE THE WHOLE PROJECT KEEPS PAYING FOR: a
 *     capability that ANSWERS for a radio the code cannot actually drive. The capability reporter
 *     and the setter are two readers of one fact, and only one was ever updated.
 *  ★ The fields live in a different struct per model — channel params on the RSP2 and Duo, DEVICE
 *    params on the dx — and the dx's update reasons are Ext1 rather than ordinary reasons.
 *  ✗ UNTESTED ON HARDWARE: only RSP1A/1B here. Written from the vendor headers.
 */
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
        case SDRPLAY_RSPduo_ID:
            impl_->params->rxChannelA->rspDuoTunerParams.rfDabNotchEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_RspDuo_RfDabNotchControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID:
            if (impl_->params->devParams)
                impl_->params->devParams->rspDxParams.rfDabNotchEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_None,
                                    sdrplay_api_Update_RspDx_RfDabNotchControl);
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
        case SDRPLAY_RSP2_ID:
            impl_->params->rxChannelA->rsp2TunerParams.biasTEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_Rsp2_BiasTControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        case SDRPLAY_RSPduo_ID:
            impl_->params->rxChannelA->rspDuoTunerParams.biasTEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_RspDuo_BiasTControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID:
            if (impl_->params->devParams)
                impl_->params->devParams->rspDxParams.biasTEnable = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_None,
                                    sdrplay_api_Update_RspDx_BiasTControl);
            break;
        default: break;
    }
}

/* ══ HDR, THE AM NOTCH, THE REFERENCE OUTPUT AND PPM ══════════════════════════════════════════
 *  The remainder of the sweep. Every one existed in the API with no path to it from anywhere in
 *  this project. ✗ None is testable here — only RSP1A/1B on the bench.
 */
bool SdrplaySource::hasHdr() const {
    return impl_->dev.hwVer == SDRPLAY_RSPdx_ID || impl_->dev.hwVer == SDRPLAY_RSPdxR2_ID;
}

void SdrplaySource::setHdr(bool on) {
    if (!hasHdr() || !impl_->params || !impl_->params->devParams) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    impl_->params->devParams->rspDxParams.hdrEnable = on ? 1 : 0;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_None,
                            sdrplay_api_Update_RspDx_HdrEnable);
    std::fprintf(stderr, "sdrplay: HDR %s\n", on ? "on" : "off");
}

bool SdrplaySource::hasAmNotch() const { return impl_->dev.hwVer == SDRPLAY_RSPduo_ID; }

void SdrplaySource::setAmNotch(bool on) {
    if (!hasAmNotch() || !impl_->params || !impl_->params->rxChannelA) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    impl_->params->rxChannelA->rspDuoTunerParams.tuner1AmNotchEnable = on ? 1 : 0;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_RspDuo_Tuner1AmNotchControl,
                            sdrplay_api_Update_Ext1_None);
}

bool SdrplaySource::hasExtRefOut() const {
    return impl_->dev.hwVer == SDRPLAY_RSP2_ID || impl_->dev.hwVer == SDRPLAY_RSPduo_ID;
}

void SdrplaySource::setExtRefOut(bool on) {
    if (!hasExtRefOut() || !impl_->params || !impl_->params->devParams) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP2_ID:
            impl_->params->devParams->rsp2Params.extRefOutputEn = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_Rsp2_ExtRefControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        case SDRPLAY_RSPduo_ID:
            impl_->params->devParams->rspDuoParams.extRefOutputEn = on ? 1 : 0;
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_RspDuo_ExtRefControl,
                                    sdrplay_api_Update_Ext1_None);
            break;
        default: break;
    }
}

void SdrplaySource::setPpm(int ppm) {
    if (!impl_->params || !impl_->params->devParams) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    impl_->params->devParams->ppm = (double)ppm;
    if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                            sdrplay_api_Update_Dev_Ppm, sdrplay_api_Update_Ext1_None);
    std::fprintf(stderr, "sdrplay: ppm %d\n", ppm);
}

/* ══ ANTENNA PORTS ════════════════════════════════════════════════════════════════════════════
 *  GitHub #29. Built from the API headers and SoapySDRPlay3, and ✗ NEVER RUN ON THE HARDWARE —
 *  nobody here owns a multi-antenna RSP. Everything below is therefore shaped so that a radio
 *  with ONE socket cannot reach the write at all: antennaPorts() returns empty for the RSP1
 *  family, and every caller — the caps, the UI, the per-band rules — is driven by that list.
 */
std::vector<std::string> SdrplaySource::antennaPorts() const {
    switch (impl_->dev.hwVer) {
        /* ★ A and B are 50Ω sockets; Hi-Z is the high-impedance AM port, which the API selects
         *  through amPortSel rather than antennaSel — a different field, same control to a user. */
        case SDRPLAY_RSP2_ID:    return { "A", "B", "Hi-Z" };
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID: return { "A", "B", "C" };
        /* ★★★ THE RSPduo's "ANTENNA" IS A TUNER, and that is why its names say so. Tuner 1 owns
         *  both the 50Ω socket and the Hi-Z port (Hi-Z is amPortSel, not a socket of its own);
         *  tuner 2 has one 50Ω socket and no Hi-Z. Naming them "A/B/C" would be a lie about the
         *  hardware and unmatchable to the labels on the case.
         *  ★★ Selecting one is NOT an Update: it is chosen on the DeviceT before SelectDevice, so
         *     a change costs a re-open (see setAntenna). That is the hardware's price, not ours —
         *     SwapRspDuoActiveTuner exists but only within an already-running dual/master session,
         *     which is a different feature (two radios from one Duo) and not this one. */
        case SDRPLAY_RSPduo_ID:  return { "Tuner 1 50\u03a9", "Tuner 1 Hi-Z", "Tuner 2 50\u03a9" };
        default:                 return {};
    }
}

std::string SdrplaySource::antenna() const {
    if (antennaPorts().empty()) return "";
    return antenna_.empty() ? "A" : antenna_;      // A is every model's power-on default
}

void SdrplaySource::setAntenna(const std::string& port) {
    const auto ports = antennaPorts();
    if (ports.empty()) return;                     // single-socket radio: nothing to choose
    // ★ An unknown name is ignored rather than guessed at. The caller in the shim logs the
    //   choice and the ports the radio offers, which is where a mismatch is legible.
    if (std::find(ports.begin(), ports.end(), port) == ports.end()) return;
    antenna_ = port;                               // remembered across a re-Init — see the member
    if (!impl_->params || !impl_->params->rxChannelA) return;
    /* ★★★ UNDER api_mtx, LIKE EVERY OTHER API-TOUCHING CALL. close(), setFrequency and the gain
     *  writers all take it, and the note on close() records what two unsynchronised teardowns of
     *  one device struct cost: SIGSEGV. A control write racing a reopen is the same shape. */
    std::lock_guard<std::recursive_mutex> lk(impl_->api_mtx);
    switch (impl_->dev.hwVer) {
        case SDRPLAY_RSP2_ID: {
            auto& t = impl_->params->rxChannelA->rsp2TunerParams;
            /* ★ Hi-Z IS THE AM PORT, not a third antennaSel value — the enum has only A and B.
             *  Selecting it means amPortSel = AMPORT_1; choosing A or B means putting the AM port
             *  back to AMPORT_2, or the 50Ω socket stays bypassed and the radio appears deaf on
             *  everything above the AM band. Both fields, every time, so the two cannot disagree. */
            const bool hiZ = (port == "Hi-Z");
            t.amPortSel  = hiZ ? sdrplay_api_Rsp2_AMPORT_1 : sdrplay_api_Rsp2_AMPORT_2;
            if (!hiZ) t.antennaSel = (port == "B") ? sdrplay_api_Rsp2_ANTENNA_B
                                                   : sdrplay_api_Rsp2_ANTENNA_A;
            if (open_) {
                api().Update(impl_->dev.dev, impl_->dev.tuner,
                             sdrplay_api_Update_Rsp2_AmPortSelect, sdrplay_api_Update_Ext1_None);
                if (!hiZ) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                       sdrplay_api_Update_Rsp2_AntennaControl,
                                       sdrplay_api_Update_Ext1_None);
            }
            break;
        }
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID: {
            if (!impl_->params->devParams) break;
            auto& d = impl_->params->devParams->rspDxParams;
            d.antennaSel = (port == "C") ? sdrplay_api_RspDx_ANTENNA_C
                         : (port == "B") ? sdrplay_api_RspDx_ANTENNA_B
                                         : sdrplay_api_RspDx_ANTENNA_A;
            /* ★ RSPdx antenna control is an Ext1 reason, not a Reason — the dx params live on the
             *  DEVICE rather than the channel, and passing it as an ordinary reason is ignored
             *  silently (the API returns Success and nothing moves). */
            if (open_) api().Update(impl_->dev.dev, impl_->dev.tuner,
                                    sdrplay_api_Update_None,
                                    sdrplay_api_Update_RspDx_AntennaControl);
            break;
        }
        /* ★★★ THE DUO CANNOT BE SWITCHED IN PLACE. Which tuner is live is fixed on the DeviceT at
         *  SelectDevice time, so the honest implementation is: remember it, and rebuild the
         *  device. reopen() already finds the radio by serial and replays the whole open sequence
         *  (rate, centre, gain, AGC dynamics), which is exactly what this needs and is why it is
         *  reused rather than hand-rolled — see the note on reopen().
         *  ★ Silent otherwise: if the radio is not open yet, the choice is simply remembered and
         *    applied by the next open(). */
        case SDRPLAY_RSPduo_ID:
            if (open_) {
                std::string e;
                std::fprintf(stderr, "sdrplay: antenna -> %s (re-opening the Duo)\n", port.c_str());
                if (!reopen(e))
                    std::fprintf(stderr, "sdrplay: the Duo did not come back: %s\n", e.c_str());
            }
            break;
        default: break;
    }
}

/** ★★★ WHICH TUNER THE DUO WILL COME UP ON — decided HERE, on the DeviceT, because SelectDevice
 *  freezes it. Called from open() before SelectDevice, so it covers reopen() too (reopen replays
 *  open). On every other model this does nothing at all.
 *
 *  ★★ AND IT NAMES THE MODE. The code used to pass whatever GetDevices left in `rspDuoMode`,
 *     which is Unknown until somebody sets it — SelectDevice is entitled to refuse that, and a
 *     Duo has never been tested here, so "it probably worked" was never evidence. Single_Tuner is
 *     what a one-radio server means, stated explicitly.
 *  ✗ UNTESTED ON HARDWARE. Nobody here owns a Duo. */
void SdrplaySource::applyDuoChoice() {
    if (impl_->dev.hwVer != SDRPLAY_RSPduo_ID) return;
    impl_->dev.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    impl_->dev.tuner = (antenna_.rfind("Tuner 2", 0) == 0) ? sdrplay_api_Tuner_B
                                                           : sdrplay_api_Tuner_A;
}

static void streamCb(short* xi, short* xq, sdrplay_api_StreamCbParamsT*,
                     unsigned int numSamples, unsigned int, void* ctx) {
    auto* c = (CbCtx*)ctx;
    if (!c || !c->sink || !*c->sink || numSamples == 0) return;
    if (c->paused && *c->paused) return;      // idle: drop, never tear the device down
    auto& ilv = *c->ilv;
    if (ilv.size() < (size_t)numSamples * 2) ilv.resize((size_t)numSamples * 2);
    /* ★★★ MEASURE WHILE WE ARE ALREADY TOUCHING EVERY SAMPLE. The interleave below reads all of
     *     them anyway, so the peak costs two compares per sample and no extra pass. Done HERE
     *     rather than downstream because this is the only point that sees the radio's output
     *     before any of our own gain, filtering or decimation — a gain control must measure what
     *     the ADC actually produced, not what the DSP made of it.
     * ★ Peak of |I| and |Q| separately, not the vector magnitude: the ADC rails on each channel
     *   independently, and it is the rail we are trying to stay off. */
    /* ★★★ MEASURE THE SIGNAL, NOT THE DC OFFSET. This took max(|I|,|Q|) on the RAW samples, so any
     *     DC offset counted as level — and that is a runaway, not merely an error: DC inflates
     *     the peak, the AGC winds the gain down to "protect" against it, the wanted signal
     *     shrinks while the DC does not, so the peak stays inflated and the gain keeps falling.
     *     It ends with both stages at minimum and the signal under the noise.
     *     Measured on air (Stuart, 2026-09-12): 7D with the IF pegged at 59 and only 12.3 dB of
     *     system gain, MER down from 24.8 to 12.4 dB and 108 frames erased; 9A unable to resolve
     *     at all, with "large DC spikes present" plainly visible in the spectrum.
     * ★ The mean is taken per callback and applied to the NEXT one — DC moves slowly, a callback
     *   is a few milliseconds, and this costs one pass of adds rather than a division per sample.
     * ★★ CLIPPING still uses the RAW value, because the converter rails on the raw sample, DC and
     *    all. Level and overload are different questions about the same samples: one asks how
     *    loud the signal is, the other asks whether the ADC is in trouble. */
    static double dcI = 0.0, dcQ = 0.0;
    const int dcIi = (int)dcI, dcQi = (int)dcQ;
    long long sumI = 0, sumQ = 0;
    int peak = 0; unsigned rails = 0;
    unsigned i = 0;
    /* ★ Eight samples a go (NEON / SSE2), reduced once per callback — this ran per sample on
     *  every RSP sample, scalar, on the capture thread (2026-09-16). Same arithmetic: the sums
     *  are exact integers, the peak is max|x − dc|, a rail is |raw| ≥ 32000. */
#if defined(__ARM_NEON)
    {
        int32x4_t sI = vdupq_n_s32(0), sQ = vdupq_n_s32(0);
        int16x8_t pk = vdupq_n_s16(0);
        uint16x8_t railV = vdupq_n_u16(0);
        const int16x8_t dI = vdupq_n_s16((int16_t)dcIi), dQ = vdupq_n_s16((int16_t)dcQi);
        const uint16x8_t lim = vdupq_n_u16(32000);
        unsigned blk = 0;
        for (; i + 8 <= numSamples; i += 8) {
            const int16x8_t a = vld1q_s16(xi + i), b = vld1q_s16(xq + i);
            int16x8x2_t il; il.val[0] = a; il.val[1] = b;
            vst2q_s16(ilv.data() + i * 2, il);
            sI = vpadalq_s16(sI, a); sQ = vpadalq_s16(sQ, b);
            // ★ saturating: -32768 - dc would otherwise wrap; the peak only needs the magnitude
            pk = vmaxq_s16(pk, vmaxq_s16(vqabsq_s16(vqsubq_s16(a, dI)), vqabsq_s16(vqsubq_s16(b, dQ))));
            const uint16x8_t hit = vorrq_u16(vcgeq_u16(vreinterpretq_u16_s16(vqabsq_s16(a)), lim),
                                             vcgeq_u16(vreinterpretq_u16_s16(vqabsq_s16(b)), lim));
            railV = vsubq_u16(railV, hit);                 // hit lanes are 0xFFFF = -1
            if (++blk == 8192) {                            // widen before a lane could wrap
                rails += (unsigned)vaddvq_u32(vpaddlq_u16(railV)); railV = vdupq_n_u16(0);
                sumI += (long long)vaddvq_s32(sI); sumQ += (long long)vaddvq_s32(sQ);
                sI = vdupq_n_s32(0); sQ = vdupq_n_s32(0); blk = 0;
            }
        }
        rails += (unsigned)vaddvq_u32(vpaddlq_u16(railV));
        sumI += (long long)vaddvq_s32(sI); sumQ += (long long)vaddvq_s32(sQ);
        peak = (int)vmaxvq_s16(pk);
    }
#elif defined(__SSE2__)
    {
        __m128i sI = _mm_setzero_si128(), sQ = _mm_setzero_si128(), pk = _mm_setzero_si128(), railV = _mm_setzero_si128();
        const __m128i dI = _mm_set1_epi16((short)dcIi), dQ = _mm_set1_epi16((short)dcQi);
        const __m128i limM1 = _mm_set1_epi16(31999), zero = _mm_setzero_si128();
        auto abs16 = [&](__m128i v) { const __m128i m = _mm_cmpgt_epi16(zero, v); return _mm_subs_epi16(_mm_xor_si128(v, m), m); };   // saturating |v|
        unsigned blk = 0;
        for (; i + 8 <= numSamples; i += 8) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(xi + i));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(xq + i));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(ilv.data() + i * 2),     _mm_unpacklo_epi16(a, b));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(ilv.data() + i * 2 + 8), _mm_unpackhi_epi16(a, b));
            sI = _mm_add_epi32(sI, _mm_madd_epi16(a, _mm_set1_epi16(1)));
            sQ = _mm_add_epi32(sQ, _mm_madd_epi16(b, _mm_set1_epi16(1)));
            pk = _mm_max_epi16(pk, _mm_max_epi16(abs16(_mm_subs_epi16(a, dI)), abs16(_mm_subs_epi16(b, dQ))));
            const __m128i hit = _mm_or_si128(_mm_cmpgt_epi16(abs16(a), limM1), _mm_cmpgt_epi16(abs16(b), limM1));
            railV = _mm_sub_epi16(railV, hit);
            if (++blk == 8192) {
                const __m128i w = _mm_madd_epi16(railV, _mm_set1_epi16(1));
                alignas(16) int32_t t[4]; _mm_store_si128(reinterpret_cast<__m128i*>(t), w); rails += (unsigned)(t[0] + t[1] + t[2] + t[3]);
                _mm_store_si128(reinterpret_cast<__m128i*>(t), sI); sumI += (long long)t[0] + t[1] + t[2] + t[3];
                _mm_store_si128(reinterpret_cast<__m128i*>(t), sQ); sumQ += (long long)t[0] + t[1] + t[2] + t[3];
                sI = zero; sQ = zero; railV = zero; blk = 0;
            }
        }
        alignas(16) int32_t t[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(t), _mm_madd_epi16(railV, _mm_set1_epi16(1))); rails += (unsigned)(t[0] + t[1] + t[2] + t[3]);
        _mm_store_si128(reinterpret_cast<__m128i*>(t), sI); sumI += (long long)t[0] + t[1] + t[2] + t[3];
        _mm_store_si128(reinterpret_cast<__m128i*>(t), sQ); sumQ += (long long)t[0] + t[1] + t[2] + t[3];
        alignas(16) int16_t p16[8]; _mm_store_si128(reinterpret_cast<__m128i*>(p16), pk);
        for (int q = 0; q < 8; ++q) if (p16[q] > peak) peak = p16[q];
    }
#endif
    for (; i < numSamples; ++i) {
        const int a = xi[i], b = xq[i];
        sumI += a; sumQ += b;
        const int ac = a - dcIi, bc = b - dcQi;          // ★ DC-removed, for LEVEL
        const int ai = ac < 0 ? -ac : ac, bi = bc < 0 ? -bc : bc;
        if (ai > peak) peak = ai;
        if (bi > peak) peak = bi;
        /* ★ 32000 of 32767 — "at the rail" with a little slack, because a converter rarely hits
         *   the exact end code and waiting for it would under-report clipping badly.
         * ★★ On the RAW sample: the ADC clips on what it actually produced. */
        const int ar = a < 0 ? -a : a, br = b < 0 ? -b : b;
        if (ar >= 32000 || br >= 32000) ++rails;
        ilv[i * 2]     = a;
        ilv[i * 2 + 1] = b;
    }
    if (numSamples) {
        /* ★ Slew slowly toward the measured mean — a real DC offset is steady, and following it
         *   quickly would let a strong low-frequency component be mistaken for DC and removed. */
        dcI += ((double)sumI / (double)numSamples - dcI) * 0.05;
        dcQ += ((double)sumQ / (double)numSamples - dcQ) * 0.05;
    }
    if (c->peak) {
        /* ★ Accumulated across callbacks into a window of about a tenth of a second, because one
         *   callback is a few milliseconds and a gain loop steered by that would chase noise.
         *   Static locals are safe: the API runs exactly one stream callback thread per device,
         *   and this build opens one device. */
        static int      wPeak  = 0;
        static uint64_t wRails = 0, wTotal = 0;
        /* ★★★ THROW THE PART-BUILT WINDOW AWAY WHEN THE CAPTURE CHANGED. Without this a window
         *     straddling a retune or a rate change reports samples taken at the OLD gain as the
         *     new signal — a spuriously high peak, which the envelope then holds through its
         *     decay delay, leaving the gain slammed down for a signal that was never there.
         *     Measured as BISTABILITY: the same DAB block settling at -11.6 dB or +31.8 dB on
         *     alternate runs, 43 dB apart, each perfectly stable, depending purely on whether the
         *     first window happened to straddle the change. */
        static unsigned lastGen = 0;
        const unsigned g = c->gen ? c->gen->load(std::memory_order_relaxed) : 0;
        if (g != lastGen) { lastGen = g; wPeak = 0; wRails = 0; wTotal = 0; dcI = dcQ = 0.0; }
        if (peak > wPeak) wPeak = peak;
        wRails += rails; wTotal += numSamples;
        if (wTotal >= 200000) {                    // ~0.1 s at 2 MSPS; rate-independent enough
            const double frac = (double)wPeak / 32768.0;
            c->peak->store(frac > 0 ? 20.0 * std::log10(frac) : -99.0, std::memory_order_relaxed);
            if (c->clip) c->clip->store(100.0 * (double)wRails / (double)wTotal,
                                        std::memory_order_relaxed);
            if (c->wins) c->wins->fetch_add(1, std::memory_order_relaxed);
            wPeak = 0; wRails = 0; wTotal = 0;
        }
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
        // ★ The AGC has spoken since our last write, so its figures describe the gain
        //   that is actually set. See liveStale_.
        if (c->stale) c->stale->store(false, std::memory_order_relaxed);
        return;
    }
    // ★ A device removal is the one event the shim genuinely has to know about: its watchdog
    // already distinguishes "stream fault" from "radio gone", and telling it the truth is what
    // keeps a false "no radio" off a working waterfall.
    if (id == sdrplay_api_DeviceRemoved && c->lost) *c->lost = true;
    /* ★★★ AND THE API'S OWN FAILURE REPORT, WHICH WE WERE THROWING AWAY. sdrplay_api_DeviceFailure
     *     is the library saying outright that it has fallen over — no inference required. We
     *     handled overloads, gain changes and removals, and ignored the one event that means the
     *     thing has broken.
     *  ★ Stuart asked the right question after a night of me guessing from behaviour: "is there no
     *    way of detecting at the API level if it has failed or not?" There is, and it has been
     *    firing into a callback that dropped it.
     *  ★★ Behavioural detectors cannot do this job. A stalled AGC and a contented one look
     *    identical on a steady signal, which is how a watchdog built on "the reduction has not
     *    moved" came to repair a receiver that was working perfectly. This one cannot false-positive:
     *    the API either reported a failure or it did not. */
    if (id == sdrplay_api_DeviceFailure && c->apiFailed)
        c->apiFailed->store(true, std::memory_order_relaxed);
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
bool SdrplaySource::setIfGainReduction(int) { return false; }
void SdrplaySource::setIfAgc(bool) {}
void SdrplaySource::setIfAgcSetPoint(int) {}
void SdrplaySource::setIfAgcDynamics(int, int, int, int) {}
void SdrplaySource::setRfNotch(bool) {}
void SdrplaySource::setDabNotch(bool) {}
int  SdrplaySource::lnaStateCount() const { return 0; }
int  SdrplaySource::lnaStateCount(double) const { return 0; }
void  SdrplaySource::noteLnaGain(int, float, int) {}
float SdrplaySource::lnaGainDb(int) const { return NAN; }
int   SdrplaySource::lnaBandId(double) { return 0; }
bool SdrplaySource::hasRfNotch() const { return false; }
bool SdrplaySource::hasDabNotch() const { return false; }
bool SdrplaySource::hasBiasT() const { return false; }
std::string SdrplaySource::model() const { return ""; }
// ★ No SDRplay API in this build: no radio, so no ports and nothing to select.
bool SdrplaySource::hasHdr() const { return false; }
void SdrplaySource::setHdr(bool) {}
bool SdrplaySource::hasAmNotch() const { return false; }
void SdrplaySource::setAmNotch(bool) {}
bool SdrplaySource::hasExtRefOut() const { return false; }
void SdrplaySource::setExtRefOut(bool) {}
void SdrplaySource::setPpm(int) {}
std::vector<std::string> SdrplaySource::antennaPorts() const { return {}; }
std::string SdrplaySource::antenna() const { return ""; }
void SdrplaySource::setAntenna(const std::string&) {}
bool SdrplaySource::apiUnresponsive() { return false; }
void SdrplaySource::retryApi() {}
std::string SdrplaySource::deviceNameLocked(int) { return ""; }
float SdrplaySource::systemGainDb() const { return 0.0f; }
void SdrplaySource::dcRecalibrate() {}
int SdrplaySource::currentIfGr() const { return 0; }
float SdrplaySource::structGainDb() const { return -999.0f; }
int SdrplaySource::currentLnaState() const { return 0; }
int  SdrplaySource::ifAgcSetPointDbfs() const { return -30; }   // ★ no-SDRplay stub (Android/iOS link)
void SdrplaySource::restartIfAgc(int) {}
int SdrplaySource::bandwidthKHzForRate(double) { return 0; }
}  // namespace vibe

#endif
