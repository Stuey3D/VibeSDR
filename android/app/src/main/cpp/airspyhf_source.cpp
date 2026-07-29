// Airspy HF+ capture source — see airspyhf_source.h for why this exists and how it differs
// from the dongle and RSP paths.
#include "airspyhf_source.h"
#include <atomic>
#include <chrono>

#ifdef VIBE_HAVE_AIRSPYHF
#ifdef VIBE_AIRSPYHF_HAS_FD
#include "airspyhf.h"          /* vendored + patched (Android) */
#else
#include <libairspyhf/airspyhf.h>   /* system/Homebrew (desktop) */
#endif
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <mutex>

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
    if (c->paused && *c->paused) return 0;   // idle: drop, never tear the device down
    (*c->sink)(reinterpret_cast<const float*>(t->samples), t->sample_count);
    return 0;   // non-zero would ask the library to STOP streaming
}

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

/** ★ OWNERSHIP OF THE DESCRIPTOR PASSES TO libusb. Do not close it here or in the caller: on
 *  Android the UsbDeviceConnection must outlive the stream, and closing it twice takes the
 *  radio down mid-capture. */
double AirspyHfSource::lastRxSecs() const { return impl_->lastRx.load(std::memory_order_relaxed); }

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
    if (airspyhf_open_fd(&impl_->dev, fd) != AIRSPYHF_SUCCESS || !impl_->dev) {
        impl_->dev = nullptr;
        err = "could not open the Airspy HF+ from the USB descriptor";
        return false;
    }
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
                 (unsigned)nearestRate(sampleRateHz),
                 airspyhf_is_low_if(impl_->dev) ? "LOW" : "zero", rates_.size());
    return true;
}

bool AirspyHfSource::start(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!open_ || !impl_->dev) { err = "device not open"; return false; }
    if (streaming_) return true;
    impl_->ctx = CbCtx{ &sink_, &lost_, &paused_, &impl_->lastRx };
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
    open_ = false;
}

// ── Tuning and rate ─────────────────────────────────────────────────────────
void AirspyHfSource::setFrequency(double hz) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->dev) return;
    airspyhf_set_freq(impl_->dev, (uint32_t)std::llround(hz));
}

uint32_t AirspyHfSource::nearestRate(double hz) const {
    if (rates_.empty()) return 0;
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
    const uint32_t r = nearestRate(hz);
    if (!r) return false;
    return airspyhf_set_samplerate(impl_->dev, r) == AIRSPYHF_SUCCESS;
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
}  // namespace vibe
#endif
