// Airspy HF+ capture source — see airspyhf_source.h for why this exists and how it differs
// from the dongle and RSP paths.
#include "airspyhf_source.h"
#include <memory>
#include <thread>
#include <future>
#include <atomic>
#include <chrono>

#ifdef VIBE_HAVE_AIRSPYHF
#ifdef VIBE_AIRSPYHF_HAS_FD
#include "airspyhf.h"          /* vendored + patched (Android) */
#else
#include <libairspyhf/airspyhf.h>   /* system/Homebrew (desktop) */
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include "vibe_thread.h"

namespace vibe {

namespace {
// The callback runs on libairspyhf's own streaming thread, so it may touch only what is
// listed here — never the object's public state.
struct CbCtx {
    AirspyHfSource::IqSink* sink;
    bool* lost;
    bool* paused;
    /** ★ Stamped on EVERY buffer, before the idle-park drop — see lastRxSecs(). */
    std::atomic<double>* lastRx;
    /** ★ Where the library's own dropped-sample count goes — see noteUsbDropped(). */
    AirspyHfSource* self;
};
}  // namespace

struct AirspyHfSource::Impl {
    airspyhf_device_t* dev = nullptr;
    uint64_t serial = 0;
    CbCtx ctx{};
    std::atomic<double> lastRx{0.0};   // see AirspyHfSource::lastRxSecs()
    // ★ Serialises every library call on this device. The shim's watchdog and the control
    // sockets both reach in, and libairspyhf makes no promise about concurrent use of one
    // handle. Cheap: these are configuration calls, not the sample path.
    std::recursive_mutex mtx;
};

// ── Enumeration ─────────────────────────────────────────────────────────────
// ★ airspyhf_list_devices(NULL, 0) returns the COUNT — the standard two-call form. Asking for
// serials up front would need a buffer sized from a number we do not have yet.
int AirspyHfSource::deviceCount() {
    const int n = airspyhf_list_devices(nullptr, 0);
    return n > 0 ? n : 0;
}

static uint64_t serialAt(int index) {
    const int n = airspyhf_list_devices(nullptr, 0);
    if (index < 0 || index >= n) return 0;
    std::vector<uint64_t> serials((size_t)n, 0);
    if (airspyhf_list_devices(serials.data(), (uint32_t)n) < 0) return 0;
    return serials[(size_t)index];
}

std::string AirspyHfSource::deviceName(int index) {
    const uint64_t sn = serialAt(index);
    if (!sn) return "";
    // ★ THE SERIAL IS PART OF THE NAME, not decoration. Two HF+ units are otherwise
    // identical in a picker, and an operator with two needs to know which one they chose.
    char buf[64];
    std::snprintf(buf, sizeof buf, "Airspy HF+ (%08X%08X)",
                  (unsigned)(sn >> 32), (unsigned)(sn & 0xFFFFFFFFu));
    return buf;
}

// ★ The two windows an HF+ Discovery actually has. The gap between them is REAL: 31-60 MHz is
// not merely poor, the hardware does not go there. A tolerance is allowed at the edges because
// a dial lands on round numbers and refusing 31.000 MHz exactly would be pedantic.
bool AirspyHfSource::tuneRangeContains(double hz) {
    return (hz >= 500.0 && hz <= 31.0e6) || (hz >= 60.0e6 && hz <= 260.0e6);
}

AirspyHfSource::AirspyHfSource() : impl_(new Impl) {}
AirspyHfSource::~AirspyHfSource() { close(); delete impl_; }

// ── Streaming callback ──────────────────────────────────────────────────────
// ★ Samples are ALREADY interleaved complex float at roughly +/-1 — the engine's own format.
// The dongle and RSP paths hand the shim int16 and it converts; going through int16 here would
// quantise an 18-bit-effective radio down to 16 and back for no reason at all.
static double nowSecsMono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static int streamCb(airspyhf_transfer_t* t) {
    if (!t || !t->ctx) return 0;
    auto* c = (CbCtx*)t->ctx;
    if (!c->sink || !*c->sink || t->sample_count <= 0) return 0;
    // ★★ LIVENESS FIRST, BEFORE THE DROP. This buffer is proof the radio is alive; whether we
    // keep it is our decision, not the hardware's. Stamping after the pause check made an
    // idle-parked radio indistinguishable from an unplugged one.
    if (c->lastRx) c->lastRx->store(nowSecsMono(), std::memory_order_relaxed);
    // ★★★ AND WHAT THE LIBRARY LOST BEFORE THIS BUFFER — counted before the idle drop too: it is a
    //     fact about the USB path, not about whether anyone is listening. See noteUsbDropped().
    if (c->self && t->dropped_samples) c->self->noteUsbDropped((uint64_t)t->dropped_samples);
    if (c->paused && *c->paused) return 0;   // idle: drop, never tear the device down
    (*c->sink)(reinterpret_cast<const float*>(t->samples), t->sample_count);
    return 0;   // non-zero would ask the library to STOP streaming
}

/* ★★★ THE LIBRARY'S OWN THREADS TAKE THE IQ PRIORITY. libairspyhf runs two: the libusb event thread
 *  that completes and resubmits the transfers, and the consumer that converts each buffer and calls
 *  streamCb. Both used to run nameless at the default priority beneath every thread we raise, and
 *  a late consumer is not a late sample — it is a DROPPED one (see noteUsbDropped). Upstream raises
 *  both to HIGHEST on Windows and nowhere else; this is our equivalent, through the one hook our
 *  vendored copy adds (airspyhf_set_thread_hook).
 *  ★ Names fit Linux's 15 characters. */
#ifdef VIBE_AIRSPYHF_HAS_FD
static void ahfThreadHook(int role) {
    vibeIqThread(role == AIRSPYHF_THREAD_USB ? "vibe-ahf-usb" : "vibe-ahf-iq");
}
#endif

// ── Lifecycle ───────────────────────────────────────────────────────────────
bool AirspyHfSource::open(int index, double sampleRateHz, double centreHz,
                          int gainTenthDb, std::string& err) {
    if (open_) return true;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);

    const uint64_t sn = serialAt(index);
    if (!sn) { err = "no Airspy HF+ at that index"; return false; }
    if (airspyhf_open_sn(&impl_->dev, sn) != AIRSPYHF_SUCCESS || !impl_->dev) {
        impl_->dev = nullptr;
        err = "could not open the Airspy HF+ (is another program using it?)";
        return false;
    }
    impl_->serial = sn;
    return finishOpen(sampleRateHz, centreHz, gainTenthDb, err);
}

double AirspyHfSource::lastRxSecs() const { return impl_->lastRx.load(std::memory_order_relaxed); }

/* ★★★ WE OPEN ON OUR OWN dup() OF THE DESCRIPTOR, NOT ON KOTLIN'S (2026-10-05) — the RTL path has done
 *  this since the use-after-free it documents in local_sdr_shim.cpp (`usbFd`). libusb_wrap_sys_device
 *  does NOT take ownership: libusb_close() leaves the fd open, so whoever closes it decides when the
 *  kernel's handle goes. Wrapping Kotlin's fd directly meant the UsbDeviceConnection's close() (a stop,
 *  or the re-attach recovery replacing a dead connection) could pull the descriptor out from under a
 *  libusb handle still in use — and a recycled fd number under a live handle sends our ioctls to some
 *  other file. Our dup is an independent descriptor on the same open file: Kotlin closes its copy when
 *  it likes, and we close ours in close(), AFTER airspyhf_close(). */
static int dupForLibusb(int fd) {
#ifdef VIBE_AIRSPYHF_HAS_FD
    return fd >= 0 ? ::dup(fd) : -1;
#else
    (void)fd; return -1;
#endif
}

bool AirspyHfSource::openFd(int fd, double sampleRateHz, double centreHz,
                            int gainTenthDb, std::string& err) {
#ifndef VIBE_AIRSPYHF_HAS_FD
    (void)fd; (void)sampleRateHz; (void)centreHz; (void)gainTenthDb;
    err = "this build's libairspyhf has no file-descriptor entry point";
    return false;
#else
    if (open_) return true;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (fd < 0) { err = "invalid USB file descriptor"; return false; }
    const int own = dupForLibusb(fd);   // ★ see dupForLibusb
    if (own < 0) {
        err = "could not duplicate the Airspy HF+ USB descriptor (errno " + std::to_string(errno) + ")";
        std::fprintf(stderr, "airspyhf: %s\n", err.c_str());
        return false;
    }
    if (airspyhf_open_fd(&impl_->dev, own) != AIRSPYHF_SUCCESS || !impl_->dev) {
        impl_->dev = nullptr;
        ::close(own);
        err = "could not open the Airspy HF+ from the USB descriptor";
        return false;
    }
    fd_ = own;
    fdOpened_ = true;
    impl_->serial = 0;   // enumeration is unavailable here, so there is no serial to read
    return finishOpen(sampleRateHz, centreHz, gainTenthDb, err);
#endif
}

/** Everything after the handle exists — identical whichever way it was obtained. */
bool AirspyHfSource::finishOpen(double sampleRateHz, double centreHz,
                                int gainTenthDb, std::string& err) {
    // ★ ASK THE RADIO what rates it has. An HF+ Discovery tops out near 912 kHz where a dongle
    // does 2.4 MSPS, so a hard-coded list would offer rates it cannot do — and the failure
    // would be a stream that never starts rather than an error anyone could read.
    uint32_t n = 0;
    if (airspyhf_get_samplerates(impl_->dev, &n, 0) == AIRSPYHF_SUCCESS && n > 0) {
        rates_.assign(n, 0);
        if (airspyhf_get_samplerates(impl_->dev, rates_.data(), n) != AIRSPYHF_SUCCESS)
            rates_.clear();
        std::sort(rates_.begin(), rates_.end());
    }
    // ★ A LAST RESORT ONLY — the real list comes from the radio. 912 kHz is the Discovery's
    // top rate (measured on hardware 2026-07-27; earlier comments here said 768, which is
    // the older HF+ Dual Port's ceiling, not this one).
    if (rates_.empty()) rates_ = { 912000 };

    // ★★★ ENABLE THE LIBRARY'S OWN DSP EXPLICITLY, never by inheriting a default. It does the
    // IQ correction AND — the part that matters — the IF SHIFT: an HF+ runs LOW-IF at some
    // sample rates (airspyhf_is_low_if reports which), and without this the tuned signal does
    // not arrive at baseband at all. Everything downstream assumes zero-IF, so a wrong default
    // here would be a quiet, whole-system sensitivity fault rather than an error anyone sees.
    // ★ Upstream does default it on, so this is belt-and-braces — but a default we depend on
    // this heavily should be stated, not assumed.
    airspyhf_set_lib_dsp(impl_->dev, 1);

    if (!setSampleRate(sampleRateHz)) {
        err = "the Airspy HF+ refused that sample rate";
        close();
        return false;
    }
    setFrequency(centreHz);
    setGainTenthDb(gainTenthDb);

    open_ = true;
    lost_ = false;
    // ★ Say what we actually got. The requested rate is a hint (the radio's list wins) and
    // low-IF vs zero-IF changes what the library is doing internally — both are worth having in
    // a log when someone reports the radio being deafer than it should be.
    std::fprintf(stderr, "airspyhf: open ok, rate %u Hz, %s-IF, %zu rates offered\n",
                 (unsigned)nearestRate(0),
                 airspyhf_is_low_if(impl_->dev) ? "LOW" : "zero", rates_.size());
    return true;
}

bool AirspyHfSource::start(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!open_ || !impl_->dev) { err = "device not open"; return false; }
    if (streaming_) return true;
    impl_->ctx = CbCtx{ &sink_, &lost_, &paused_, &impl_->lastRx, this };
#ifdef VIBE_AIRSPYHF_HAS_FD
    airspyhf_set_thread_hook(&ahfThreadHook);   // ★ before start: the threads are made there
#endif
    if (airspyhf_start(impl_->dev, &streamCb, &impl_->ctx) != AIRSPYHF_SUCCESS) {
        err = "the Airspy HF+ would not start streaming";
        return false;
    }
    streaming_ = true;
    return true;
}

void AirspyHfSource::stop() {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!streaming_ || !impl_->dev) return;
    airspyhf_stop(impl_->dev);
    streaming_ = false;
}

void AirspyHfSource::close() {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    stop();
    if (impl_->dev) { airspyhf_close(impl_->dev); impl_->dev = nullptr; }
    // ★ Our dup, and only AFTER the handle that used it is closed — see dupForLibusb.
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    open_ = false;
}

/* ★★★ STOP + START ON THE HANDLE WE HOLD — ON A DEADLINE (2026-10-05). The shallow and the fd-opened
 *  restarts used to run airspyhf_stop/airspyhf_start INLINE, holding impl_->mtx, with none of the
 *  deep path's guard. On a wedged-but-present HF+ (Nick's Pixel 6, overnight) a control transfer that
 *  never answered then held the mutex for ever: every control thread queued behind it, shutdown
 *  queued behind them, and on Android that is an ANR rather than a radio the watchdog can retry.
 *  libairspyhf's control transfers now time out at 1 s each, so this SHOULD always return — the
 *  deadline is the belt to those braces, because "should" is what the last hang said too.
 *  ★ 5 s, not the close's 3: a stop + start is several bounded transfers plus the transfer reap
 *    (up to 1 s), and a slow-but-alive radio must not be abandoned for being slow.
 *  ★★ ON A TIMEOUT THE HANDLE IS ABANDONED, NOT REUSED — the worker is still inside the library with
 *     it, and a second call on the same handle is how a stuck radio becomes a stuck process. The
 *     worker is detached (joining would inherit the hang), exactly as the deep path's close is, and
 *     the radio needs a fresh handle: a deep reopen by serial on a desktop, a fresh fd from
 *     UsbManager on Android. */
bool AirspyHfSource::restartOnHandle(std::string& err) {
    airspyhf_device* dev = impl_->dev;
    if (!dev) { err = "device not open"; return false; }
    const bool wasStreaming = streaming_;
    streaming_ = false;
    impl_->ctx = CbCtx{ &sink_, &lost_, &paused_, &impl_->lastRx, this };
#ifdef VIBE_AIRSPYHF_HAS_FD
    airspyhf_set_thread_hook(&ahfThreadHook);   // ★ before start: the threads are made there
#endif
    CbCtx* ctx = &impl_->ctx;
    auto done = std::make_shared<std::promise<int>>();
    auto fut  = done->get_future();
    std::thread([done, dev, wasStreaming, ctx]() {
        // ★ A failure to stop is EXPECTED and must not abort the restart — see restartStream.
        if (wasStreaming) airspyhf_stop(dev);
        done->set_value(airspyhf_start(dev, &streamCb, ctx));
    }).detach();
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        abandonHandle();
        err = "the Airspy HF+ did not answer a stream restart within 5 s — handle abandoned, it needs a fresh one";
        std::fprintf(stderr, "airspyhf: stream restart TIMED OUT after 5 s — abandoning the handle "
                             "(the radio is wedged; it needs reopening, or replugging on Android)\n");
        return false;
    }
    if (fut.get() != AIRSPYHF_SUCCESS) {
        err = "the Airspy HF+ would not start streaming";
        std::fprintf(stderr, "airspyhf: stream restart: start FAILED on the held handle\n");
        return false;
    }
    streaming_ = true;
    lost_ = false;
    return true;
}

/** ★ Forget a handle a timed-out worker still holds. Never closed from here — see restartOnHandle.
 *  ★ Its fd (our dup) is LEAKED with it, on purpose: closing a descriptor a stuck ioctl may still be
 *    using lets the number be reused, and the library's next call would land on some other file. */
void AirspyHfSource::abandonHandle() {
    if (fd_ >= 0) std::fprintf(stderr, "airspyhf: leaking fd %d with the abandoned handle\n", fd_);
    fd_ = -1;
    impl_->dev = nullptr;
    streaming_ = false;
    open_ = false;
    lost_ = true;
}

// ★★★ See the header for why this is safe here and deliberately absent on the RTL path.
bool AirspyHfSource::restartStream(bool deep, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    // ★★★ A DEEP RESTART MUST BE ALLOWED WITH NO HANDLE — that is the ONLY state it can help in.
    // This guard used to cover both paths, and it turned one failed attempt into a permanently
    // dead radio: a nudged USB plug re-enumerates the device, attempts 1-2 (shallow) fail because
    // the handle is stale, attempt 3 goes deep, closes the handle and sets dev=nullptr — and if the
    // reopen misses by a second, because the device has not finished re-enumerating, EVERY later
    // attempt returns "device not open" here without going near the radio. The back-off kept
    // ticking and the log kept saying it was retrying; nothing ever did. Only killing the process
    // cured it (Stuart, 2026-08-02: "a little nudge of the USB port is enough to kill it").
    // ★★ The serial survives in impl_->serial, which is what a deep restart reopens by, so a
    // handle-less device is recoverable — it just has to be ALLOWED to try.
    if (!deep && (!open_ || !impl_->dev)) { err = "device not open"; return false; }

    if (!deep) {
        // ── Shallow: the handle is still good, the stream just stopped delivering. ──
        // ★ A failure to stop is EXPECTED and must not abort the restart — we are here
        //   precisely because the device is misbehaving, and refusing to re-start because the
        //   teardown of an already-broken stream complained would leave the radio dead for
        //   good. Same reasoning as the RSP's Uninit.
        // ★ On a deadline since 2026-10-05 — see restartOnHandle.
        if (!restartOnHandle(err)) return false;
        std::fprintf(stderr, "airspyhf: stream restarted after a stall\n");
        return true;
    }

    // ★★★ NOT WHERE THE HANDLE CAME FROM AN fd (Android). openFd leaves impl_->serial at 0, so the reopen
    //     below could never succeed: the deep path only ever CLOSED the radio for good — and on Nick's
    //     Pixel 6 that close was the SIGABRT (Play vitals, 2026-10-04). A fresh fd has to come from the Java
    //     layer, which this cannot ask for, so keep the handle and let the shallow restart (which now really
    //     restarts a stream that died on its own — see libairspyhf kill_io_threads) keep trying.
    if (impl_->serial == 0) {
        err = "the Airspy HF+ stream stalled; retrying on the same handle (an fd-opened radio cannot be reopened here)";
        if (!open_ || !impl_->dev) return false;
        if (!restartOnHandle(err)) return false;
        std::fprintf(stderr, "airspyhf: stream restarted on the same handle (fd-opened; no deep reopen)\n");
        return true;
    }

    // ── Deep: the handle itself is suspect, so throw it away and open a fresh one. ──
    // ★ BY SERIAL, NOT BY INDEX. Enumeration order is not stable across a re-plug, and this
    //   box may well have more than one radio on it — reopening "device 0" could hand us a
    //   different radio than the operator was listening to.
    const uint64_t serial = impl_->serial;
    const double   rate   = curRate_ > 0.0 ? curRate_ : 912000.0;
    const double   centre = curCentre_;
    const int      gain   = curGainTenth_;
    const bool     wasStreaming = streaming_;

    // ★★★ CLOSE ON A DEADLINE — airspyhf_close() CAN HANG FOR EVER, and it took the whole
    //     recovery thread with it. Caught with gdb on the Pi (2026-08-08): with three radios
    //     running, this thread sat in airspyhf_close() called from here, and the watchdog's log
    //     simply STOPPED mid-ladder. The radio was down, and so was the only thing that could have
    //     brought it back — a hang is worse than the fault it was trying to cure.
    //
    // ★★ THE WORKER IS DETACHED DELIBERATELY, exactly as SdrplaySource::withTimeout does for the
    //    same reason: joining would inherit the hang we are escaping. A leaked blocked thread is a
    //    far smaller problem than a frozen watchdog, and it unwinds itself if the library ever
    //    returns.
    // ★ We do NOT then reopen after a timed-out close: the library still owns that handle, and
    //   opening a second one on the same device is how you turn a stuck radio into a stuck
    //   PROCESS. Report it, keep the server up, and let the ladder try again later.
    {
        auto done = std::make_shared<std::promise<void>>();
        auto fut  = done->get_future();
        airspyhf_device* dying = impl_->dev;
        const bool wasStreamingNow = streaming_;
        std::thread([done, dying, wasStreamingNow]() mutable {
            if (wasStreamingNow) airspyhf_stop(dying);
            airspyhf_close(dying);
            done->set_value();
        }).detach();
        streaming_ = false;
        impl_->dev = nullptr;
        open_ = false;
        if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
            err = "the Airspy HF+ did not respond to being closed — it needs replugging";
            std::fprintf(stderr, "airspyhf: close timed out; not reopening\n");
            return false;
        }
    }

    if (serial == 0 ||
        airspyhf_open_sn(&impl_->dev, serial) != AIRSPYHF_SUCCESS || !impl_->dev) {
        impl_->dev = nullptr;
        // ★ Report the truth. On Android a genuinely re-enumerated device needs a fresh USB
        //   fd that only the Java layer can obtain — see the header.
        err = "could not reopen the Airspy HF+ (it may have re-enumerated)";
        std::fprintf(stderr, "airspyhf: deep restart FAILED: %s\n", err.c_str());
        return false;
    }
    if (!finishOpen(rate, centre, gain, err)) {
        std::fprintf(stderr, "airspyhf: deep restart re-open FAILED: %s\n", err.c_str());
        return false;
    }
    // Put the HF+-specific switches back — finishOpen only replays rate/tune/gain.
    setAgcThreshold(agcHigh_);
    setLna(lna_);
    if (wasStreaming && !start(err)) {
        std::fprintf(stderr, "airspyhf: deep restart could not stream: %s\n", err.c_str());
        return false;
    }
    lost_ = false;
    std::fprintf(stderr, "airspyhf: device reopened after a stall (serial %016llx)\n",
                 (unsigned long long)serial);
    return true;
}

// ── Tuning and rate ─────────────────────────────────────────────────────────
void AirspyHfSource::setFrequency(double hz) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->dev) return;
    airspyhf_set_freq(impl_->dev, (uint32_t)std::llround(hz));
    curCentre_ = hz;           // remembered for restartStream(deep)
}

uint32_t AirspyHfSource::nearestRate(double hz) const {
    if (rates_.empty()) return 0;
    // ★★★ 0 MEANS "THE RATE THIS RADIO SHOULD RUN AT" — its highest, which is its default.
    // The HF+ is now opened at exactly one rate and never re-rated (see the header), so this is
    // the only rate that ever reaches the hardware in normal use. Two things depend on it and
    // BOTH break at any other rate: the dead-lobe crop is a per-rate table whose numbers are
    // measured at 912 and merely inherited from SDR++ Brown elsewhere, and 228 kHz tunes some
    // 7.8 kHz off frequency on this firmware (2026-08-02, open). Stuart: "I think we just offer
    // the default on the server too otherwise that breaks all the dead space fix we added."
    if (hz <= 0.0) return *std::max_element(rates_.begin(), rates_.end());
    uint32_t best = rates_.front();
    double bestErr = std::fabs((double)best - hz);
    for (uint32_t r : rates_) {
        const double e = std::fabs((double)r - hz);
        if (e < bestErr) { bestErr = e; best = r; }
    }
    return best;
}

bool AirspyHfSource::setSampleRate(double hz) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->dev) return false;
    // ★★★ THE HF+ RUNS AT ITS DEFAULT RATE, WHATEVER IT IS ASKED FOR — and this is the line that
    //     finally makes that true. nearestRate() has said "the HF+ is now opened at exactly one
    //     rate and never re-rated" for months, but it only honoured that for hz <= 0: a config
    //     carrying 768000 asked for 768000 and got it exactly.
    //
    // ★★★ AND THE ARCHITECTURE CHANGES WITH THE RATE. In libairspyhf, `is_low_if` is indexed BY
    //     SAMPLE RATE (`samplerate_architectures[samplerate]`), so different rates put the tuner in
    //     genuinely different modes — with a different IF and a different digital correction. At
    //     768 kHz Stuart's radio was tuning exactly 15.00 kHz high across the whole of medium wave
    //     (measured against the 9 kHz raster, 12/12 carriers agreeing to within 70 Hz) while being
    //     spot on at 9.4 MHz. At its default rate it is correct. Stuart, 2026-08-09: "if the radio
    //     is at 768 that was always broken, it needs to run at 912 truly."
    //
    // ★ An owner upgrading carries a stored 768000 that nothing rewrites, so refusing it HERE is
    //   what fixes an existing install — the setup page no longer offers it, but the config file
    //   still holds it.
    const uint32_t r = nearestRate(0);
    if (hz > 0 && (uint32_t)hz != r)
        std::fprintf(stderr, "airspyhf: asked for %.0f Hz, using this radio's own rate %u Hz "
                             "(other rates change the tuner architecture and mis-tune MW)\n", hz, r);
    if (!r) return false;
    if (airspyhf_set_samplerate(impl_->dev, r) != AIRSPYHF_SUCCESS) return false;
    curRate_ = (double)r;      // remembered for restartStream(deep)
    return true;
}

// ── Gain ────────────────────────────────────────────────────────────────────
// ★★ THE SLIDER DRIVES THE ATTENUATOR, and it runs BACKWARDS relative to a dongle. An HF+ has
// no variable gain to turn up — it has a fixed front end, a switchable +6 dB preamp and a
// 0-48 dB attenuator. So "more gain" means "less attenuation", and the useful operating range
// is almost entirely about backing the front end OFF on a crowded HF band.
// ★ Mapping: the client's tenth-dB value is treated as a desired gain from 0 (max attenuation)
// to 480 (none). Negative means "let the radio decide", which is what its own AGC is for and
// is the right default on an HF+ — unlike a dongle, its AGC is good.
void AirspyHfSource::setGainTenthDb(int tenthDb) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->dev) return;
    curGainTenth_ = tenthDb;   // remembered for restartStream(deep)
    if (tenthDb < 0) { setAgc(true); return; }
    setAgc(false);
    const int wantDb = std::min(480, tenthDb) / 10;      // 0..48 dB of wanted gain
    const int steps  = std::max(0, std::min(8, (48 - wantDb) / 6));
    setAttenuation(steps);
}

// ── HF+ specific ────────────────────────────────────────────────────────────
void AirspyHfSource::setAgc(bool on) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    agc_ = on;
    if (!impl_->dev) return;
    const int rc = airspyhf_set_hf_agc(impl_->dev, on ? 1 : 0);
    std::fprintf(stderr, "airspyhf: agc %s -> rc %d\n", on ? "ON" : "off", rc);
}

// ★ LOGGED, because "this control does nothing" is indistinguishable from "this control never
// arrived" — and several controls genuinely never arrived today. With the call visible in the
// log, a reported no-op is a fact about the RADIO rather than a guess about our plumbing.
// ★ Stuart reports the threshold making no audible difference (2026-07-27). Plumbing verified;
// whether it has any effect in the 60-260 MHz window is unknown — the API documents no
// semantics, and the VHF front end is different hardware from the HF one.
void AirspyHfSource::setAgcThreshold(bool high) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    agcHigh_ = high;
    if (!impl_->dev) return;
    const int rc = airspyhf_set_hf_agc_threshold(impl_->dev, high ? 1 : 0);
    std::fprintf(stderr, "airspyhf: agc threshold %s -> rc %d\n", high ? "HIGH" : "low", rc);
}

void AirspyHfSource::setAttenuation(int steps) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    att_ = std::max(0, std::min(8, steps));
    if (!impl_->dev) return;
    const int rc = airspyhf_set_hf_att(impl_->dev, (uint8_t)att_);
    std::fprintf(stderr, "airspyhf: attenuation %d dB -> rc %d\n", att_ * 6, rc);
}

void AirspyHfSource::setLna(bool on) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    lna_ = on;
    if (!impl_->dev) return;
    const int rc = airspyhf_set_hf_lna(impl_->dev, on ? 1 : 0);
    std::fprintf(stderr, "airspyhf: preamp %s -> rc %d\n", on ? "ON" : "off", rc);
}

void AirspyHfSource::setCalibrationPpb(int ppb) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (impl_->dev) airspyhf_set_calibration(impl_->dev, ppb);
}

std::string AirspyHfSource::model() const { return "Airspy HF+"; }

}  // namespace vibe

#else   // ── no libairspyhf in this build ─────────────────────────────────────
namespace vibe {
struct AirspyHfSource::Impl {};
AirspyHfSource::AirspyHfSource() = default;
AirspyHfSource::~AirspyHfSource() = default;
int  AirspyHfSource::deviceCount() { return 0; }
std::string AirspyHfSource::deviceName(int) { return ""; }
bool AirspyHfSource::tuneRangeContains(double) { return true; }
bool AirspyHfSource::open(int, double, double, int, std::string& err) {
    err = "this build has no Airspy HF+ support"; return false;
}
bool AirspyHfSource::openFd(int, double, double, int, std::string& err) {
    err = "this build has no Airspy HF+ support"; return false;
}
bool AirspyHfSource::finishOpen(double, double, int, std::string& err) {
    err = "this build has no Airspy HF+ support"; return false;
}
void AirspyHfSource::close() {}
bool AirspyHfSource::start(std::string& err) { err = "no Airspy HF+ support"; return false; }
void AirspyHfSource::stop() {}
void AirspyHfSource::setFrequency(double) {}
uint32_t AirspyHfSource::nearestRate(double) const { return 0; }
bool AirspyHfSource::setSampleRate(double) { return false; }
void AirspyHfSource::setGainTenthDb(int) {}
void AirspyHfSource::setAgc(bool) {}
void AirspyHfSource::setAgcThreshold(bool) {}
void AirspyHfSource::setAttenuation(int) {}
void AirspyHfSource::setLna(bool) {}
void AirspyHfSource::setCalibrationPpb(int) {}
std::string AirspyHfSource::model() const { return ""; }
double AirspyHfSource::lastRxSecs() const { return 0.0; }
bool AirspyHfSource::restartStream(bool, std::string& err) {
    err = "this build has no Airspy HF+ support"; return false;
}
}  // namespace vibe
#endif
