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
#include <string>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

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
    /** ★★ Samples delivered, for measuring the rate the radio is ACTUALLY running at. */
    std::atomic<long long>* samps;
    /** ★ Software decimation, applied before the sink — see setSampleRate. */
    HalfBandChain* chain;
    std::mutex* chainMtx;
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
    // ★★ COUNTED BEFORE THE PAUSE DROP: this is what the RADIO is doing, not what we kept.
    if (c->samps) c->samps->fetch_add(t->sample_count, std::memory_order_relaxed);
    // ★ THE BUFFER SIZE IS A FRAME-RATE CEILING. Both clients ask for 20 fps and get 14 — an odd,
    //   specific number (Stuart, 2026-08-01) — and 912000/65536 = 13.9. If the DSP emits one frame
    //   per callback, the radio's USB buffer decides the maximum and the request cannot beat it.
    //   Logged once so the arithmetic is on the record rather than inferred.
    { static bool once = false;
      if (!once) { once = true;
        std::fprintf(stderr, "airspyhf: USB buffer = %d samples -> %.1f callbacks/s at this rate\n",
                     t->sample_count, 912000.0 / (double)t->sample_count); } }
    if (c->paused && *c->paused) return 0;   // idle: drop, never tear the device down
    // ★★★ SOFTWARE DECIMATION HAPPENS HERE, before anything downstream exists. The DSP, the FFT,
    //     the waterfall calibration and the client's span all read currentRate(), which reports
    //     the EFFECTIVE rate — so none of them need to know this is happening.
    //     ★ try_lock, never lock: reconfiguration happens while the consumer is paused, and if the
    //     two ever did meet, dropping one USB buffer is invisible while blocking the USB callback
    //     would not be.
    if (c->chain && c->chainMtx && c->chainMtx->try_lock()) {
        int outN = 0;
        const float* out = c->chain->process(reinterpret_cast<const float*>(t->samples),
                                             t->sample_count, outN);
        if (outN > 0) (*c->sink)(out, outN);
        c->chainMtx->unlock();
        return 0;
    }
    if (c->chain && c->chain->factor() > 1) return 0;   // mid-reconfigure: drop this buffer
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
        // ★★★ KEEP THE RADIO'S OWN ORDER. libairspyhf turns a rate in Hz into an INDEX by
        //   matching against this array and sends the INDEX to the device — so the order is not
        //   cosmetic, it is the wire format. We sort a copy for display and must never let that
        //   sorted order stand in for the device's.
        rawRates_ = rates_;
        std::sort(rates_.begin(), rates_.end());
    }
    // ★ A LAST RESORT ONLY — the real list comes from the radio. 912 kHz is the Discovery's
    // top rate (measured on hardware 2026-07-27; earlier comments here said 768, which is
    // the older HF+ Dual Port's ceiling, not this one).
    if (rates_.empty()) rates_ = { 912000 };

    // ★★★ OFFER ONLY WHAT THE RADIO ACTUALLY DOES. This one advertises seven rates and
    //     implements THREE — its top rate halved. Driven and counted, 2026-08-01:
    //         912000 -> 912000   768000 -> 912000   650000 -> 912000
    //         456000 -> 456000   384000 -> 456000
    //         228000 -> 228000   192000 -> 228000
    //     and it reports success every time. The substituted rates are not merely useless, they
    //     are WRONG IN TWO WAYS: the DSP is built for a rate the radio is not running (heard as a
    //     pitch shift), and libairspyhf picks its IF architecture — zero-IF vs low-IF, which
    //     decides the LO — from the index that was ASKED for, so the radio ends up listening
    //     somewhere else entirely (Stuart, tuned to 104.7, hearing 104.2; and Horizon showing at
    //     104.9 instead of 104.7. At 912 kHz everything is perfect).
    //     ★★ A control that cannot work should not be offered — the project's own rule. Halving
    //     from the top is exactly what the measurements show, and it generalises: an HF+ Dual
    //     Port topping out at 768 kHz yields 768/384/192, which are ITS working rates.
    //     ★ The measurement in setSampleRate stays as the backstop. This stops anyone choosing a
    //     broken rate; that catches the radio doing something unexpected anyway.
    if (rates_.size() > 1) {
        const uint32_t top = rates_.back();          // sorted ascending
        std::vector<uint32_t> keep;
        for (uint32_t r : rates_) {
            for (uint32_t t = top; t >= 1000; t /= 2)
                if (r == t) { keep.push_back(r); break; }
        }
        if (keep.size() >= 2 && keep.size() < rates_.size()) {
            std::string dropped;
            for (uint32_t r : rates_)
                if (std::find(keep.begin(), keep.end(), r) == keep.end())
                    { dropped += " "; dropped += std::to_string(r); }
            std::fprintf(stderr, "airspyhf: dropping rates the radio does not implement:%s\n",
                         dropped.c_str());
            rates_ = keep;
        }
    }

    // ★★★ WHAT WE OFFER IS NOT WHAT THE RADIO OFFERS. Hardware rates down to the top halved, then
    //     SOFTWARE DECIMATION for everything below — see chooseRate(). That buys three things at
    //     once: rates the radio would otherwise get wrong (its own 228 tunes off-frequency),
    //     ~3 dB of processing gain per halving on weak signals, and fewer bins to send.
    //     ★ Stopping at 1/8 is deliberate: below about 50 kHz the span is narrower than much of
    //     what people tune, and each stage costs CPU on the serving device for less benefit.
    {
        const uint32_t top = rates_.back();
        const uint32_t floorHw = rates_.size() > 1 ? top / 2 : top;
        offered_.clear();
        for (uint32_t hw : rates_) {
            if (hw < floorHw) continue;
            offered_.push_back(hw);
            if (hw == floorHw) for (int d = 2; d <= 8; d <<= 1) offered_.push_back(hw / d);
        }
        std::sort(offered_.begin(), offered_.end());
        offered_.erase(std::unique(offered_.begin(), offered_.end()), offered_.end());
        std::string l; for (uint32_t v : offered_) { l += " "; l += std::to_string(v); }
        std::fprintf(stderr, "airspyhf: offering (hardware + decimated):%s\n", l.c_str());
    }

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
    // ★ WHICH RADIO AND WHICH FIRMWARE. The rate table comes straight out of firmware, so when it
    //   disagrees with the published set for this model that is the first thing anyone will ask.
    { char ver[128] = {0};
      if (airspyhf_version_string_read(impl_->dev, ver, sizeof(ver) - 1) == AIRSPYHF_SUCCESS)
          std::fprintf(stderr, "airspyhf: firmware \"%s\"\n", ver); }

    // ★★★ THE CANONICAL-RATE PROBE HAS BEEN REMOVED, and this note is why it must not come back
    //     as a startup step. It walked the device through every candidate rate at EVERY open to
    //     answer one question (does this firmware have 256? — no, it is rejected; the seven it
    //     reports are genuine). Each step re-tunes the LO and flips the IF architecture, and 912
    //     kHz — correct all evening — started tuning wrongly once it shipped (Stuart, 2026-08-02:
    //     "456 is correct, 912 is now broken. It was correct before all this work").
    //     ★ A diagnostic that perturbs the thing it measures is fine for one run and unacceptable
    //     as a permanent cost. If the question ever needs re-asking, ask it in a throwaway build.
    // ★ AND WHAT THEY ARE. "7 rates offered" is not enough to tell whether a rate the user picked
    //   is one the radio actually has — which is the whole question when some rates play at the
    //   wrong pitch.
    { std::string l; for (uint32_t v : rates_) { l += " "; l += std::to_string(v); }
      std::fprintf(stderr, "airspyhf: rates (sorted):%s\n", l.c_str());
      std::string r; for (size_t i = 0; i < rawRates_.size(); i++)
          r += " [" + std::to_string(i) + "]=" + std::to_string(rawRates_[i]);
      std::fprintf(stderr, "airspyhf: rates (device order, index=wire value):%s\n", r.c_str()); }
    return true;
}

bool AirspyHfSource::start(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!open_ || !impl_->dev) { err = "device not open"; return false; }
    if (streaming_) return true;
    impl_->ctx = CbCtx{ &sink_, &lost_, &paused_, &impl_->lastRx, &sampCount_, &chain_, &chainMtx_ };
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

// ★★★ See the header for why this is safe here and deliberately absent on the RTL path.
bool AirspyHfSource::restartStream(bool deep, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!open_ || !impl_->dev) { err = "device not open"; return false; }

    if (!deep) {
        // ── Shallow: the handle is still good, the stream just stopped delivering. ──
        // ★ A failure to stop is EXPECTED and must not abort the restart — we are here
        //   precisely because the device is misbehaving, and refusing to re-start because the
        //   teardown of an already-broken stream complained would leave the radio dead for
        //   good. Same reasoning as the RSP's Uninit.
        if (streaming_) { airspyhf_stop(impl_->dev); streaming_ = false; }
        // start() re-seeds the callback context and sets streaming_ — don't duplicate it here.
        if (!start(err)) return false;
        lost_ = false;
        std::fprintf(stderr, "airspyhf: stream restarted after a stall\n");
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

    if (streaming_) { airspyhf_stop(impl_->dev); streaming_ = false; }
    airspyhf_close(impl_->dev);
    impl_->dev = nullptr;
    open_ = false;

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
    uint32_t best = rates_.front();
    double bestErr = std::fabs((double)best - hz);
    for (uint32_t r : rates_) {
        const double e = std::fabs((double)r - hz);
        if (e < bestErr) { bestErr = e; best = r; }
    }
    return best;
}

/** ★★★ PICK A HARDWARE RATE THE RADIO GETS RIGHT, AND DIVIDE FOR THE REST.
 *  Hardware rates are used down to the top rate HALVED; anything below that is reached by
 *  decimating one of those in software. Three reasons, in order of weight:
 *    1. This radio's low rates are the broken ones — 228 kHz tunes to the wrong frequency
 *       (measured 2026-08-02), while 912 and 456 are exact. Dividing a good rate cannot inherit
 *       a bad one's fault.
 *    2. Each halving is ~3 dB of processing gain on a weak signal: the noise bandwidth halves
 *       and the carrier does not.
 *    3. Fewer bins for the same resolution = less uplink on someone else's server.
 *  ★ The cost is honest and worth stating: the RADIO still streams at the hardware rate, so USB
 *    traffic, ADC load and front-end selectivity are unchanged. Only what we process and send
 *    narrows. A true hardware rate change would narrow before the ADC — when the hardware can be
 *    trusted to do it. */
void AirspyHfSource::chooseRate(double wantHz, uint32_t& hwOut, int& decimOut) const {
    hwOut = nearestRate(wantHz); decimOut = 1;
    if (rates_.empty()) return;
    const uint32_t top = rates_.back();                 // sorted ascending
    const uint32_t floorHw = rates_.size() > 1 ? top / 2 : top;
    uint32_t bestHw = top; int bestDec = 1; double bestErr = 1e18;
    for (uint32_t hw : rates_) {
        if (hw < floorHw) continue;                     // below this, decimate instead
        for (int d = 1; d <= 16; d <<= 1) {
            const double eff = (double)hw / d;
            // Prefer the closest effective rate; on a tie prefer LESS decimation (less CPU).
            const double err = std::fabs(eff - wantHz) + d * 0.001;
            if (err < bestErr) { bestErr = err; bestHw = hw; bestDec = d; }
        }
    }
    hwOut = bestHw; decimOut = bestDec;
}

bool AirspyHfSource::setSampleRate(double hz) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->dev) return false;
    uint32_t r = 0; int wantDecim = 1;
    chooseRate(hz, r, wantDecim);
    if (!r) return false;
    if (wantDecim != decim_) {
        std::lock_guard<std::mutex> cl(chainMtx_);
        chain_.setFactor(wantDecim);
        chain_.reset();
        decim_ = wantDecim;
    }
    if (wantDecim > 1)
        std::fprintf(stderr, "airspyhf: %.0f Hz asked -> hardware %u / %d in software\n",
                     hz, (unsigned)r, wantDecim);
    // ★★★ SAY WHAT HAPPENED. A rate the device REFUSES leaves it running at the old one while
    //   everything downstream is rebuilt for the new figure — and a rate mismatch is heard as a
    //   pitch shift, not as an error (Stuart, 2026-08-01: 912/456/228 fine, 768/650/384/192 all
    //   slow). The failure was silent because this returned false and the caller ignored it.
    const int rc = airspyhf_set_samplerate(impl_->dev, r);
    if (rc != AIRSPYHF_SUCCESS) {
        std::fprintf(stderr, "airspyhf: setSampleRate %u REFUSED (rc %d)\n", (unsigned)r, rc);
        return false;
    }
    curRate_ = (double)r;      // provisional — the measurement below has the last word

    // ★★★ MEASURE IT. DO NOT BELIEVE THE LIST, AND DO NOT BELIEVE `rc 0`.
    //     This radio advertises seven rates and implements THREE. Measured on hardware
    //     (2026-08-01, HF+ Discovery, every rate driven and counted):
    //         912000 -> 912000     768000 -> 912000     650000 -> 912000
    //         456000 -> 456000     384000 -> 456000
    //         228000 -> 228000     192000 -> 228000
    //     It silently rounds UP to its top rate halved, and reports success either way. The DSP
    //     was then built for the number we asked for while the radio ran faster, and a rate
    //     mismatch is heard as a PITCH SHIFT — Stuart: 912/456/228 fine, everything else "Barry
    //     White", each one slow by exactly the ratio between what he picked and what it did.
    //     ★ So the rate is settled by counting samples, not by asking. Whatever the radio decides
    //     to do, the DSP is built for what it is ACTUALLY doing — which is also the only approach
    //     that survives a different HF+ model, a firmware update, or the next radio to do this.
    if (streaming_) {
        sampCount_.store(0, std::memory_order_relaxed);
        const double t0 = nowSecsMono();
        impl_->mtx.unlock();                       // let the stream callback run
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        impl_->mtx.lock();
        const double dt = nowSecsMono() - t0;
        const long long n = sampCount_.load(std::memory_order_relaxed);
        if (dt > 0.2 && n > 1000) {
            const double measured = (double)n / dt;
            const uint32_t snapped = nearestRate(measured);   // reject counting jitter
            if (snapped && std::fabs((double)snapped - (double)r) > 1.0) {
                std::fprintf(stderr, "airspyhf: asked %u but the radio is running %u "
                                     "(measured %.0f S/s) — using the measured rate\n",
                             (unsigned)r, (unsigned)snapped, measured);
                curRate_ = (double)snapped;
            } else {
                std::fprintf(stderr, "airspyhf: rate %u confirmed (measured %.0f S/s)\n",
                             (unsigned)r, measured);
            }
            // ★★★ AND WHICH IF ARCHITECTURE THIS RATE USES. It is a per-rate property and it
            //     changes the TUNING MATHS: libairspyhf applies its 5 kHz DEFAULT_IF_SHIFT only in
            //     ZERO-IF, and the LO floor differs too (180 kHz vs 84). 912 and 456 are correct
            //     and 228 "shifts everything" (Stuart, 2026-08-01) — so the first thing to know is
            //     whether 228 is the odd one out here. Logged rather than assumed.
            std::fprintf(stderr, "airspyhf: rate %u is %s-IF\n", (unsigned)curRate_,
                         airspyhf_is_low_if(impl_->dev) ? "LOW" : "zero");
        }
    }
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
