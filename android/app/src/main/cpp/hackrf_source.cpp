// HackRF One capture source — EXPERIMENTAL, Linux only. See hackrf_source.h for why.
#include "hackrf_source.h"
#include <string>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(VIBE_HAS_HACKRF)
#include <libhackrf/hackrf.h>

namespace vibe {
namespace {

double nowSecsMono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/* ★★★ ONE hackrf_init() FOR THE PROCESS, AND IT MUST NOT BE PER-OPEN. libhackrf keeps a global
 *     libusb context; init/exit around every open works until two radios (or an enumeration
 *     during a capture) overlap, and then it tears down a context another device is still using.
 *     The other sources here have the same shape for the same reason. */
std::once_flag g_initOnce;
bool           g_initOk = false;
void ensureInit() {
    std::call_once(g_initOnce, [] { g_initOk = (hackrf_init() == HACKRF_SUCCESS); });
}

/** The rates worth offering — see the note in the header. 2 MSPS is the hardware floor; the top
 *  is chosen for what the DSP can carry, not what the radio can emit. */
const uint32_t kRates[] = { 2000000u, 2400000u, 4000000u, 5000000u, 8000000u, 10000000u };

/* ★★★ HOW FAST THE DC ESTIMATE FOLLOWS. A leaky integrator per channel: dc += (x - dc) * ALPHA,
 *     which is a one-pole high-pass at fs*ALPHA/2pi. At 1/4096 that is 78 Hz at 2 MSPS and 389 Hz
 *     at 10 MSPS — narrow enough that it can only ever remove DC itself, never anything a listener
 *     is tuned to, because the offset tuning already guarantees nobody is listening AT DC.
 *  ★★ DELIBERATELY SLOW. A faster estimate tracks the signal instead of the offset and starts
 *     eating real modulation at the centre of the span; the thing being removed here is LO leakage
 *     and converter bias, which drift with temperature over seconds, not milliseconds. */
constexpr float kDcAlpha = 1.0f / 4096.0f;

struct CbCtx {
    HackRfSource::IqSink* sink   = nullptr;
    std::atomic<bool>*    paused = nullptr;
    std::atomic<double>*  lastRx = nullptr;
    std::vector<float>*   scratch = nullptr;
    std::mutex*           scratchMtx = nullptr;
    /* ★ The running DC estimate, one per channel. It lives in the context rather than in a local
     *   because it must survive between callbacks — a per-buffer estimate would be a mean, not a
     *   tracker, and would change with the signal in the buffer. */
    float*                dcI = nullptr;
    float*                dcQ = nullptr;
};

}  // namespace

struct HackRfSource::Impl {
    hackrf_device*       dev = nullptr;
    std::recursive_mutex mtx;
    std::string          serial;
    double               rate = 0, centre = 0;
    std::atomic<bool>    paused{false};
    std::atomic<double>  lastRx{0.0};
    std::vector<float>   scratch;
    std::mutex           scratchMtx;
    // ★ Zeroed on every open() so a fresh session never inherits the last radio's bias.
    float                dcI = 0.0f, dcQ = 0.0f;
    CbCtx                ctx;
};

// ── Streaming callback ──────────────────────────────────────────────────────────────────────
/* ★★ int8 -> float IS EXACT AND COSTS ONE PASS. The dongle and RSP hand the shim int16 and it
 *    converts; going via int16 here would be two conversions to reach the same numbers. Divide by
 *    128, not 127: the range is -128..127, so /128 keeps the scale symmetric and cannot exceed 1.0
 *    — a value of exactly -1.0 is honest, and +1.0 is simply unreachable, which is correct for a
 *    two's-complement converter. */
static int rxCallback(hackrf_transfer* t) {
    if (!t || !t->rx_ctx) return 0;
    auto* c = (CbCtx*)t->rx_ctx;
    if (!c->sink || !*c->sink || t->valid_length <= 0) return 0;

    // ★★ LIVENESS BEFORE THE DROP — the same ordering the Airspy source documents. This buffer is
    //    proof the radio is alive; whether we keep it is our decision, not the hardware's.
    if (c->lastRx) c->lastRx->store(nowSecsMono(), std::memory_order_relaxed);
    if (c->paused && c->paused->load(std::memory_order_relaxed)) return 0;

    const int n = t->valid_length / 2;          // interleaved I,Q bytes -> complex samples
    if (n <= 0) return 0;

    std::lock_guard<std::mutex> lk(*c->scratchMtx);
    auto& out = *c->scratch;
    if ((int)out.size() < n * 2) out.resize((size_t)n * 2);
    const int8_t* in = reinterpret_cast<const int8_t*>(t->buffer);
    /* ★★★ AND TAKE THE DC OUT WHILE WE ARE ALREADY TOUCHING EVERY SAMPLE. A HackRF is direct
     *     conversion with no DC servo, so LO leakage and converter bias arrive as a constant
     *     offset — which is a carrier at 0 Hz, i.e. a permanent spike in the middle of the span.
     *
     * ★★★ WHY THIS AND NOT JUST THE OFFSET TUNING. hwOffsetHz() moves the spike 250 kHz away from
     *     the LOGICAL CENTRE, which protects the channel only while the VFO sits near that centre.
     *     It does not stay there: the shim lets the VFO roam +/-lim before it retunes the hardware,
     *     and lim is `usableSpan/2 - margin - rxBw/2` — 700 kHz at 2 MSPS and 3.1 MHz at 8, because
     *     edgeCutoffHz() is HF+-only and usableSpan is the whole rate here. So ordinary tuning
     *     across the waterfall walks the VFO straight over a spike that is pinned at +250 kHz.
     *     The offset fixes the centre case; this fixes the general one. Both are kept — the notch
     *     has finite width and the offset guarantees nothing is ever listening at DC.
     *
     * ★★ ONE PASS, NO EXTRA COST WORTH NAMING: the conversion loop already reads and writes every
     *    sample, and this adds two multiply-adds to each. Doing it here rather than in the shared
     *    engine also means it cannot affect any other radio — the RSP and the HF+ have their own DC
     *    handling and must not get a second one. */
    float dcI = c->dcI ? *c->dcI : 0.0f;
    float dcQ = c->dcQ ? *c->dcQ : 0.0f;
    for (int i = 0; i < n; i++) {
        const float xi = (float)in[i * 2]     * (1.0f / 128.0f);
        const float xq = (float)in[i * 2 + 1] * (1.0f / 128.0f);
        dcI += (xi - dcI) * kDcAlpha;
        dcQ += (xq - dcQ) * kDcAlpha;
        out[(size_t)(i * 2)]     = xi - dcI;
        out[(size_t)(i * 2 + 1)] = xq - dcQ;
    }
    if (c->dcI) *c->dcI = dcI;
    if (c->dcQ) *c->dcQ = dcQ;
    (*c->sink)(out.data(), n);
    return 0;   // non-zero asks libhackrf to STOP streaming
}

// ── Enumeration ─────────────────────────────────────────────────────────────────────────────
int HackRfSource::deviceCount() {
    ensureInit();
    if (!g_initOk) return 0;
    hackrf_device_list_t* l = hackrf_device_list();
    if (!l) return 0;
    const int n = l->devicecount;
    hackrf_device_list_free(l);
    return n < 0 ? 0 : n;
}

std::string HackRfSource::deviceName(int index) {
    ensureInit();
    if (!g_initOk) return "";
    hackrf_device_list_t* l = hackrf_device_list();
    if (!l) return "";
    std::string name;
    if (index >= 0 && index < l->devicecount) {
        const char* sn = l->serial_numbers[index];
        /* ★ THE TAIL OF THE SERIAL, NOT ALL OF IT. A HackRF serial is 32 hex characters of which
         *   the first twenty-odd are identical across every unit ever made; printing the lot gives
         *   an operator with two radios two labels that differ in the last few characters, buried
         *   at the end of a wall of zeroes. */
        std::string s = sn ? sn : "";
        if (s.size() > 8) s = s.substr(s.size() - 8);
        name = "HackRF One";
        if (!s.empty()) name += " (" + s + ")";
    }
    hackrf_device_list_free(l);
    return name;
}

bool HackRfSource::tuneRangeContains(double hz) {
    // 1 MHz - 6 GHz. See the header: below 1 MHz needs an upconverter and is simply absent.
    return hz >= 1.0e6 && hz <= 6.0e9;
}

HackRfSource::HackRfSource() : impl_(new Impl) {
    rates_.assign(std::begin(kRates), std::end(kRates));
}
HackRfSource::~HackRfSource() { close(); delete impl_; }

// ── Lifecycle ───────────────────────────────────────────────────────────────────────────────
bool HackRfSource::open(int index, double sampleRateHz, double centreHz,
                        int gainTenthDb, std::string& err) {
    if (open_) return true;
    ensureInit();
    if (!g_initOk) { err = "libhackrf could not start"; return false; }
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);

    hackrf_device_list_t* l = hackrf_device_list();
    if (!l || index < 0 || index >= l->devicecount) {
        /* ★★★ SAY WHICH OF THE TWO IT IS. "no HackRF at that index" is true of an empty bus and of
         *     a bad index alike, and those need completely different fixes — plug the radio in, or
         *     stop asking for radio 4 on a machine with one. It cost a round trip with the only
         *     person who owns one of these (2026-08-26: "using HackRF 4 (experimental)" followed
         *     by this line, on a box with a single radio). An error that does not separate the
         *     causes it is reporting is a question, not an answer. */
        const int n = l ? l->devicecount : 0;
        if (l) hackrf_device_list_free(l);
        if (n == 0) err = "no HackRF is attached (libhackrf sees none — on Android/chroot check "
                          "USB permissions: libhackrf needs access to the device node)";
        else        err = "no HackRF at index " + std::to_string(index) + " — "
                        + std::to_string(n) + " attached, so the valid range is 0.."
                        + std::to_string(n - 1);
        return false;
    }
    impl_->serial = l->serial_numbers[index] ? l->serial_numbers[index] : "";
    const int rc = hackrf_device_list_open(l, index, &impl_->dev);
    hackrf_device_list_free(l);
    if (rc != HACKRF_SUCCESS || !impl_->dev) {
        impl_->dev = nullptr;
        err = "could not open the HackRF (is another program using it?)";
        return false;
    }

    return finishOpen(sampleRateHz, centreHz, gainTenthDb, err);
}

/** ★★★ ANDROID ONLY IN PRACTICE. UsbManager hands the app an ALREADY-OPEN descriptor and forbids
 *  enumeration, so hackrf_device_list_open() above cannot run there at all — there is no device
 *  list to index into. hackrf_open_fd() is our patch to the vendored libhackrf; see
 *  cpp/libhackrf/hackrf.c.
 *
 *  ★ OWNERSHIP OF THE DESCRIPTOR PASSES TO libusb on success. Do not close it here or in the
 *  caller: the UsbDeviceConnection must outlive the stream, and closing it twice takes the radio
 *  down mid-capture. Same rule as AirspyHfSource::openFd(). */
bool HackRfSource::openFd(int fd, double sampleRateHz, double centreHz,
                          int gainTenthDb, std::string& err) {
#ifndef VIBE_HACKRF_HAS_FD
    /* ★★★ ONLY THE VENDORED COPY HAS THIS ENTRY POINT. A desktop build links the system or
     *     Homebrew libhackrf, which is stock and has no hackrf_open_fd — so this must compile
     *     to an honest refusal rather than an undefined symbol at link time. Exactly the shape
     *     AirspyHfSource::openFd() uses for the same reason. */
    (void)fd; (void)sampleRateHz; (void)centreHz; (void)gainTenthDb;
    err = "this build's libhackrf has no file-descriptor entry point";
    return false;
#else
    if (open_) return true;
    ensureInit();
    if (!g_initOk) { err = "libhackrf could not start"; return false; }
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (fd < 0) { err = "invalid USB file descriptor"; return false; }
    if (hackrf_open_fd(&impl_->dev, fd) != HACKRF_SUCCESS || !impl_->dev) {
        impl_->dev = nullptr;
        err = "could not open the HackRF from the USB descriptor";
        return false;
    }
    // ★ Enumeration is what carries the serial, and there is none here. Left empty rather than
    //   invented: a made-up serial would flow into the radio list and the directory.
    impl_->serial = "";
    return finishOpen(sampleRateHz, centreHz, gainTenthDb, err);
#endif
}

/** Everything after the handle exists — identical whichever way it was obtained. */
bool HackRfSource::finishOpen(double sampleRateHz, double centreHz,
                              int gainTenthDb, std::string& err) {
    (void)err;
    open_ = true;
    // ★ Start the DC tracker from nothing: a value learned before a retune, a rate change or a
    //   hand-over to another program describes a different front-end state, and 4096 samples is
    //   a couple of milliseconds to relearn it.
    impl_->dcI = impl_->dcQ = 0.0f;
    if (!setSampleRate(sampleRateHz)) {
        // ★ Not fatal: the radio is open and a rate it did accept is better than no radio. The
        //   picker will show what it actually ended up at.
        std::fprintf(stderr, "[hackrf] rate %.0f refused, using %u\n",
                     sampleRateHz, (unsigned)impl_->rate);
    }
    setFrequency(centreHz);
    // ★ The stages start where the owner last left them (the members' defaults on a fresh
    //   object), then the single slider is applied over the top if one was asked for.
    setAmpEnable(amp_ != 0);
    setLnaGainDb(lna_);
    setVgaGainDb(vga_);
    setBiasTee(bias_);
    if (gainTenthDb >= 0) setGainTenthDb(gainTenthDb);
    return true;
}

void HackRfSource::close() {
    if (!impl_) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (impl_->dev) {
        hackrf_stop_rx(impl_->dev);
        hackrf_close(impl_->dev);
        impl_->dev = nullptr;
    }
    open_ = false;
}

bool HackRfSource::start(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->dev) { err = "the HackRF is not open"; return false; }
    impl_->ctx = CbCtx{ &sink_, &impl_->paused, &impl_->lastRx,
                        &impl_->scratch, &impl_->scratchMtx,
                        &impl_->dcI, &impl_->dcQ };
    const int rc = hackrf_start_rx(impl_->dev, rxCallback, &impl_->ctx);
    if (rc != HACKRF_SUCCESS) { err = "the HackRF refused to start receiving"; return false; }
    impl_->lastRx.store(nowSecsMono(), std::memory_order_relaxed);
    return true;
}

void HackRfSource::stop() {
    if (!impl_) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (impl_->dev) hackrf_stop_rx(impl_->dev);
}

void HackRfSource::setPaused(bool paused) {
    if (impl_) impl_->paused.store(paused, std::memory_order_relaxed);
}

double HackRfSource::secondsSinceLastRx() const {
    if (!impl_) return 1e9;
    const double t = impl_->lastRx.load(std::memory_order_relaxed);
    return t <= 0.0 ? 1e9 : (nowSecsMono() - t);
}

// ── Tuning and rate ─────────────────────────────────────────────────────────────────────────
void HackRfSource::setFrequency(double hz) {
    if (!impl_) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->dev) return;
    impl_->centre = hz;
    hackrf_set_freq(impl_->dev, (uint64_t)llround(hz));
}

uint32_t HackRfSource::nearestRate(double hz) const {
    uint32_t best = rates_.empty() ? 2000000u : rates_.front();
    double bestD = 1e18;
    for (uint32_t r : rates_) {
        const double d = std::fabs((double)r - hz);
        if (d < bestD) { bestD = d; best = r; }
    }
    return best;
}

bool HackRfSource::setSampleRate(double hz) {
    if (!impl_) return false;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->dev) return false;
    const uint32_t r = nearestRate(hz);
    if (hackrf_set_sample_rate(impl_->dev, (double)r) != HACKRF_SUCCESS) return false;
    /* ★★ AND THE BASEBAND FILTER WITH IT, OR IT KEEPS THE OLD ONE. libhackrf does not move the
     *    filter when the rate changes — leave it and a narrower filter silently crops the wider
     *    span you just asked for, which reads as "the radio only hears the middle". Round DOWN to
     *    a supported width: too wide lets neighbours alias into an 8-bit converter. */
    const uint32_t bw = hackrf_compute_baseband_filter_bw_round_down_lt(r);
    hackrf_set_baseband_filter_bandwidth(impl_->dev, bw);
    impl_->rate = r;
    return (double)r == hz || std::fabs((double)r - hz) < 1.0;
}

// ── Gain ────────────────────────────────────────────────────────────────────────────────────
void HackRfSource::setAmpEnable(bool on) {
    amp_ = on ? 1 : 0;
    if (!impl_) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (impl_->dev) hackrf_set_amp_enable(impl_->dev, (uint8_t)amp_);
}

void HackRfSource::setLnaGainDb(int db) {
    // 0-40 in 8 dB steps, rounded DOWN — see the header for why down and not nearest.
    int v = db < 0 ? 0 : (db > 40 ? 40 : db);
    v = (v / 8) * 8;
    lna_ = v;
    if (!impl_) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (impl_->dev) hackrf_set_lna_gain(impl_->dev, (uint32_t)v);
}

void HackRfSource::setVgaGainDb(int db) {
    int v = db < 0 ? 0 : (db > 62 ? 62 : db);
    v = (v / 2) * 2;
    vga_ = v;
    if (!impl_) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (impl_->dev) hackrf_set_vga_gain(impl_->dev, (uint32_t)v);
}

void HackRfSource::setBiasTee(bool on) {
    bias_ = on;
    if (!impl_) return;
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (impl_->dev) hackrf_set_antenna_enable(impl_->dev, on ? 1 : 0);
}

void HackRfSource::setGainTenthDb(int tenthDb) {
    /* ★★★ LNA FIRST, THEN VGA, AND THE ORDER IS THE WHOLE POINT. Gain taken early (LNA) sets the
     *     noise figure; gain taken late (VGA) only amplifies what the LNA already decided,
     *     including its noise. So fill the LNA before touching the VGA — the reverse gives a
     *     noisier receiver for exactly the same number on the slider.
     * ★★ THE RF AMP IS LEFT ALONE. It is a 14 dB step, not a knob, and stepping it under a
     *    listener is a jump nobody asked for. It stays where the owner put it.
     * ★ Negative means "leave the stages alone" — this radio has no AGC to hand back to, so
     *   there is nothing else negative could sensibly mean here. */
    if (tenthDb < 0) return;
    int db = tenthDb / 10;
    if (db > 102) db = 102;                 // 40 LNA + 62 VGA
    const int lna = std::min(40, (db / 8) * 8);
    setLnaGainDb(lna);
    setVgaGainDb(db - lna);
}

}  // namespace vibe

#else   // ── no libhackrf in this build ──────────────────────────────────────────────────────
namespace vibe {
struct HackRfSource::Impl {};
HackRfSource::HackRfSource() = default;
HackRfSource::~HackRfSource() = default;
int  HackRfSource::deviceCount() { return 0; }
std::string HackRfSource::deviceName(int) { return ""; }
bool HackRfSource::tuneRangeContains(double) { return true; }
bool HackRfSource::openFd(int, double, double, int, std::string& err) {
    err = "no HackRF support"; return false;
}
bool HackRfSource::finishOpen(double, double, int, std::string& err) {
    err = "no HackRF support"; return false;
}
bool HackRfSource::open(int, double, double, int, std::string& err) {
    err = "this build has no HackRF support"; return false;
}
void HackRfSource::close() {}
bool HackRfSource::start(std::string& err) { err = "no HackRF support"; return false; }
void HackRfSource::stop() {}
void HackRfSource::setPaused(bool) {}
double HackRfSource::secondsSinceLastRx() const { return 1e9; }
void HackRfSource::setFrequency(double) {}
uint32_t HackRfSource::nearestRate(double) const { return 0; }
bool HackRfSource::setSampleRate(double) { return false; }
void HackRfSource::setGainTenthDb(int) {}
void HackRfSource::setAmpEnable(bool) {}
void HackRfSource::setLnaGainDb(int) {}
void HackRfSource::setVgaGainDb(int) {}
void HackRfSource::setBiasTee(bool) {}
}  // namespace vibe
#endif
