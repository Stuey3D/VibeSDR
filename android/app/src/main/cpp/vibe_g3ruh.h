// vibe_g3ruh.h — V12: the 9600 bit/s packet receiver (G3RUH-compatible scrambled FSK / GFSK, as terrestrial 9k6 packet
// and many cubesats send it), from a listener's channel IQ or from FM-discriminator audio. docs/v12/AX25-9600-FX25-IL2P.md.
//
// ★★★ FROM THE PUBLISHED DESIGN, our own code: James Miller G3RUH, "9600 Baud Packet Radio Modem Design" (AMSAT /
//     Amateur Radio, 1988): direct FM of the carrier by the shaped baseband, a self-synchronising scrambler
//     1 + x^12 + x^17 after NRZI, ±3 kHz peak deviation in a 25 kHz channel, a receive low-pass then a data slicer and a
//     clock recovered from the zero crossings. AX.25 v2.2 for the framing above it (vibe_hdlc.h), FX.25 and IL2P beside
//     it (vibe_fx25.h, vibe_il2p.h, wired together in vibe_pktlink.h).
//
// ★★ TWO WAYS IN, ONE SLICER BANK:
//    IQ   (feedIq):    the channel IQ, dial at DC, any rate ≥ 24 kS/s → an NCO (the AFC's correction) → a channel
//                      low-pass ±6 kHz (stopping by ±10), decimated to ≥ 32 kS/s (vibedsp FirDecimator — NEON/SSE2) → the FM
//                      discriminator (vibedsp FmDemod — NEON/SSE2), scaled so ±3 kHz deviation is ±1;
//    audio (feedAudio): FM-discriminator audio (a radio's 9600 data port, or a WAV of one) at any rate, any polarity
//                      (NRZI and the scrambler do not care; IL2P checks both ways round);
//    then both → resampled to exactly 48 kS/s, five samples a bit (vibedsp RationalResampler — NEON/SSE2) → the
//    receive low-pass (vibedsp RealFir — NEON/SSE2) → the SLICER BANK.
// ★★ THE SLICER BANK, for weak and lopsided signals: a peak and a valley tracker give the eye's centre and size (a
//    receiver's DC — a tuning error, Doppler — is followed, not assumed away); `slicers` thresholds across the eye
//    (centre ± 0.12 and ± 0.24 of its half-height for five), each with its own zero-crossing bit clock (SymClock
//    below), descrambler, HDLC deframer, FX.25 and IL2P receivers (vibepkt::LinkRx). An asymmetric eye — a signal
//    off tune through a narrow receiver, a transmitter's own offset — puts the best threshold off centre; one of the
//    five sits nearer it. A frame several slicers find is passed on once (vibepkt::Merger).
// ★ THE AFC (IQ only): while a slicer is inside a frame AND a carrier is there (the channel's power 5 dB over its floor,
//   or an open eye — a chance flag in noise also opens "a frame"), the eye's centre IS the carrier's offset (×3 kHz); it
//   moves the NCO a third of the way every 5.3 ms, clamped to ±12 kHz — so a cubesat's residual Doppler or an RTL's crystal is pulled
//   back to the middle of the channel filter between frames. A satellite engine that corrects Doppler itself just leaves
//   less for it to do (docs/v12/AX25-9600-FX25-IL2P.md "For the satellite engine").
// ★ Hot loops (filters, discriminator, resampler, NCO) are vibedsp's SIMD kernels. What is left per sample is the
//   slicer bank: a compare, a few adds and a branch per slicer — a state machine where vector lanes do not pay (the
//   bit clocks wrap at different samples); it is measured in test-ax25hs.
#pragma once
#include "vibe_pktlink.h"
#include "vibedsp/vibedsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace vibeg3ruh {

using cf32 = std::complex<float>;

struct Opts {
    bool fx25 = true, il2p = true;
    int baud = 9600;
    int slicers = 5;         ///< 1 = one balanced slicer (the baseline the bank is measured against)
    double devHz = 3000;     ///< nominal peak deviation (sets the discriminator's scale on the IQ path)
    bool afc = true;
    double rxCutHz = 0;      ///< the receive low-pass corner; 0 = 0.5625 × baud (5.4 kHz at 9600 — measured best of 3)
    double chanCutHz = 6000; ///< the IQ channel filter's corner (±; it stops by ±10 kHz) — measured best of 6, see the doc
    double chanTransHz = 4000;
    float gainHunt = 0.3f, gainIn = 0.1f;    ///< the bit clocks' share of a crossing's error, hunting / inside a frame
    bool clickLimit = true;                   ///< IQ path: hold the discriminator within ±1.6 × deviation of the eye's centre
    float eyeRelease = 1.0f / 500.0f;          ///< the eye trackers' release a sample (1/500 measured best of 3: recovers from a noise gap before the preamble ends)
    float maxStep = 0.06f;                    ///< the most one crossing may move a bit clock (of a bit)
};

/** ★★ THE BIT CLOCK FOR 9600 — a zero-crossing DPLL made for five samples a bit in noise. The general one (vibe_hdlc.h
 *  BitClock) moves its phase by a fixed share of every crossing's error; at 5 samples a bit a noise crossing near a bit
 *  centre then moves it by up to half a bit, and measured on random data it SLIPPED (gained or lost a bit) 18 times in
 *  40 000 bits at 12 dB Eb/N0 — every slip ends a frame and every FX.25 / IL2P block after it. Here:
 *    - a crossing's error (its phase − ½) counts in proportion to how steep the crossing is against the eye's size —
 *      a real edge between two bits crosses fast, noise wandering about the threshold crosses slowly;
 *    - errors beyond ±0.4 of a bit are ignored (a crossing at a bit centre says nothing about which way to go);
 *    - each correction is capped at `maxStep` of a bit, and a correction never wraps the phase itself (see push) —
 *      only the clock's own advance emits a bit, so a correction can neither repeat a bit nor skip one. */
class SymClock {
public:
    SymClock(double fs, double baud) : step_(baud / fs) {}
    template <class Lvl> void push(float x, float gain, float maxStep, Lvl&& level) {
        const double prevPh = ph_;
        ph_ += step_;
        if (ph_ >= 1.0) {
            ph_ -= 1.0;
            const double back = std::min(1.0, std::max(0.0, ph_ / step_));
            level(float(x - (x - last_) * back));
        }
        if ((x >= 0) != (last_ >= 0) && x != last_) {
            const double frac = double(last_) / double(last_ - x);
            double pc = prevPh + step_ * frac;
            if (pc >= 1.0) pc -= 1.0;
            const double e = pc - 0.5;
            if (std::fabs(e) < 0.4) {
                // steepness: a full edge between ±1 changes by ~2/(samples a bit) a sample at its middle
                const double slope = std::fabs(double(x - last_)) / (2.0 * step_ * 1.6);
                const double w = std::min(1.0, slope);
                double c = e * double(gain) * w;
                c = std::max(-double(maxStep), std::min(double(maxStep), c));
                // ★★★ NEVER WRAP HERE. A crossing just before a bit centre is often handled on the sample just AFTER
                //     the wrap (ph ≈ 0.01); a retard of 0.02 then takes the phase below 0, and wrapping it to 0.99 emitted
                //     the same bit again on the next sample — the slips measured (5 in 40 000 bits at 12 dB, all one way).
                //     A phase a little below 0 simply waits; one a little over 1 wraps (and emits) on the next sample.
                ph_ -= c;
            }
        }
        last_ = x;
    }
private:
    double step_, ph_ = 0;
    float last_ = 0;
};

class Demod {
public:
    explicit Demod(Opts o = {}) : o_(o), merger_(kFs, 3.0 * kFs / o.baud, 0.6) {
        const double cut = o_.rxCutHz > 0 ? o_.rxCutHz : 0.5625 * o_.baud;
        rx_.reset(new vibedsp::RealFir(vibedsp::designLowpass(cut / kFs, 0.25 * o_.baud / kFs)));
        const int ns = std::max(1, o_.slicers);
        for (int k = 0; k < ns; ++k) {
            Slicer s;
            s.off = ns == 1 ? 0.f : float((k - (ns - 1) / 2.0) * (ns <= 3 ? 0.2 : 0.12));
            s.clk.reset(new SymClock(kFs, o_.baud));
            vibepkt::LinkOpts lo;
            lo.fx25 = o_.fx25; lo.il2p = o_.il2p; lo.g3ruh = true;
            lo.hdlc.minBytes = 17; lo.hdlc.maxBytes = 1100; lo.hdlc.repairMax = 0;
            s.link.reset(new vibepkt::LinkRx(lo));
            sl_.push_back(std::move(s));
        }
        centre_ = (ns - 1) / 2;
    }
    int slicers() const { return int(sl_.size()); }
    /** The carrier offset the AFC has found (Hz, IQ path; + = the signal sits above the dial). */
    double afcHz() const { return afcHz_; }
    void setAfcHz(double hz) { afcHz_ = hz; if (nco_) nco_->setFreq(afcHz_ / iqFs_); }

    /** FM-discriminator audio at `fs` Hz. `out(const vibepkt::PktFrame&)` for each new frame. */
    template <class Out> void feedAudio(const float* x, int n, double fs, Out&& out) {
        if (!(fs > 0) || n <= 0) return;
        if (fs != audFs_) { audFs_ = fs; audRs_.reset(std::fabs(fs - kFs) < 0.5 ? nullptr : new vibedsp::RationalResampler(fs, int(kFs), vibedsp::RationalResampler::ExactRate{})); }
        to48_(audRs_.get(), x, n, out);
    }
    /** Channel IQ at `fs` Hz with the signal at (about) 0 Hz. */
    template <class Out> void feedIq(const cf32* x, int n, double fs, Out&& out) {
        if (!(fs > 0) || n <= 0) return;
        if (fs != iqFs_) buildIq_(fs);
        mix_.resize(size_t(n));
        nco_->mix(x, mix_.data(), n);
        dec_.resize(size_t(chan_->maxOut(n)));
        const int m = chan_->process(mix_.data(), n, dec_.data());
        // ★ is a carrier there? The channel's power against its own floor (the quietest blocks, forgotten over ~10 s):
        //   an FM carrier is a constant envelope well above the floor at any SNR the slicers can use.
        if (m > 0) {
            double p = 0; for (int i = 0; i < m; ++i) p += double(std::norm(dec_[size_t(i)]));
            p /= m;
            if (!(floor_ > 0) || p < floor_) floor_ = p; else floor_ += (p - floor_) * (double(m) / (10.0 * iqFs_ / chan_->decim()));
            carrier_ = p > 3.0 * floor_;
        }
        disc_.resize(size_t(m));
        fm_.process(dec_.data(), disc_.data(), m);
        // ★ CLICK LIMITING: near the FM threshold the phasor brushes the origin and the discriminator throws a 2π
        //   "click" — sixteen times the deviation in one sample. Through the receive filter that is several bits of
        //   nonsense, false zero crossings for the bit clocks and a jolt to the eye trackers. Nothing real lies beyond the
        //   peak deviation, so the samples are held within ±1.6 of it about the eye's centre (the carrier's offset).
        if (o_.clickLimit) {
            const float lo = mid_ - 1.6f, hi = mid_ + 1.6f;
            for (int i = 0; i < m; ++i) disc_[size_t(i)] = std::min(hi, std::max(lo, disc_[size_t(i)]));
        }
        to48_(iqRs_.get(), disc_.data(), m, out);
    }
    /** ★ Tests only: every slicer's line levels as they are decided (the BER measurement in test-ax25hs). */
    std::function<void(int, float)> levelTap;
    /** The eye's openness (0 = perfect, ~0.3+ = noise) — diagnostics and the AFC's gate. */
    float eye() const { return eye_; }
    uint64_t dupes() const { return merger_.dupes(); }
    uint64_t fecDupes() const { return merger_.fecDupes(); }
    /** Totals over the bank (diagnostics). */
    uint64_t fx25Tags() const { uint64_t t = 0; for (auto& s : sl_) if (s.link->fx25()) t += s.link->fx25()->tags(); return t; }
    uint64_t fx25RsFailed() const { uint64_t t = 0; for (auto& s : sl_) if (s.link->fx25()) t += s.link->fx25()->rsFailed(); return t; }
    uint64_t fx25Decoded() const { uint64_t t = 0; for (auto& s : sl_) if (s.link->fx25()) t += s.link->fx25()->decoded(); return t; }
    uint64_t il2pSyncs() const { uint64_t t = 0; for (auto& s : sl_) if (s.link->il2p()) t += s.link->il2p()->syncs(); return t; }

private:
    static constexpr double kFs = 48000;
    struct Slicer { float off = 0; std::unique_ptr<SymClock> clk; std::unique_ptr<vibepkt::LinkRx> link; };

    void buildIq_(double fs) {
        iqFs_ = fs;
        const int D = std::max(1, int(fs / 32000.0));
        chan_.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(o_.chanCutHz / fs, o_.chanTransHz / fs), D));
        const double f2 = fs / D;
        fm_.setGain(float(f2 / (2 * M_PI * o_.devHz)));
        fm_.reset();
        nco_.reset(new vibedsp::NCO(afcHz_ / fs));
        iqRs_.reset(std::fabs(f2 - kFs) < 0.5 ? nullptr : new vibedsp::RationalResampler(f2, int(kFs), vibedsp::RationalResampler::ExactRate{}));
    }
    template <class Out> void to48_(vibedsp::RationalResampler* rs, const float* x, int n, Out& out) {
        const float* p = x; int m = n;
        if (rs) { r48_.resize(size_t(rs->maxOut(n))); m = rs->process(x, n, r48_.data()); p = r48_.data(); }
        y_.resize(size_t(rx_->maxOut(m)));
        const int k = rx_->process(p, m, y_.data());
        slice_(y_.data(), k, out);
    }
    template <class Out> void slice_(const float* y, int n, Out& out) {
        auto emit = [&](const vibepkt::PktFrame& f) { out(f); };
        for (int i = 0; i < n; ++i) {
            const float x = y[i];
            // ★ the eye: peak and valley, fast to follow a new edge, slow to forget (≈ 200 bits)
            if (x > hi_) hi_ += 0.25f * (x - hi_); else hi_ += o_.eyeRelease * (mid_ - hi_);
            if (x < lo_) lo_ += 0.25f * (x - lo_); else lo_ += o_.eyeRelease * (mid_ - lo_);
            mid_ = 0.5f * (hi_ + lo_);
            const float half = std::max(0.5f * (hi_ - lo_), 1e-6f), inv = 1.f / half;
            bool any = false;
            for (size_t k = 0; k < sl_.size(); ++k) {
                Slicer& s = sl_[k];
                const float v = (x - mid_) * inv - s.off;
                const bool in = s.link->inFrame();
                any = any || s.link->hdlcInFrame();
                s.clk->push(v, in ? o_.gainIn : o_.gainHunt, o_.maxStep, [&](float lvl) {
                    // ★ the eye's openness, from the centre slicer's bit decisions: (|level| − 1)² is small on a signal
                    //   (levels sit at ±1 of the eye) and large on noise (they scatter) — the AFC's "is this a signal?"
                    if (levelTap) levelTap(int(k), lvl);
                    if (k == centre_) { const float e = std::fabs(lvl) - 1.f; eye_ += 0.03f * (std::min(e * e, 4.f) - eye_); }
                    s.link->push(lvl, [&](const vibepkt::PktFrame& f) { vibepkt::PktFrame g = f; g.slicer = int(k); merger_.offer(g, t_, emit); });
                });
            }
            // ★ AFC: only inside a frame AND with an open eye (a chance flag in noise sets "in a frame" too)
            if (o_.afc && any && nco_ && (carrier_ || eye_ < kEyeOpen)) { afcAcc_ += mid_; ++afcN_; }
            ++t_;
            if ((t_ & 255) == 0) {
                if ((t_ & 1023) == 0) merger_.tick(t_, emit);
                if (afcN_ >= 128 && nco_) {
                    // a third of the way every 5.3 ms: a preamble of 20 ms pulls a 6 kHz error in before the frame
                    const double off = afcAcc_ / afcN_ * o_.devHz;
                    const double step = std::max(-12000.0, std::min(12000.0, afcHz_ + kAfcGain * off)) - afcHz_;
                    afcHz_ += step;
                    nco_->setFreq(afcHz_ / iqFs_);
                    // the discriminator's DC moves with the correction: move the eye with it, so nothing re-learns it
                    const float d = float(step / o_.devHz); hi_ -= d; lo_ -= d; mid_ -= d;
                }
                afcAcc_ = 0; afcN_ = 0;
            }
        }
        merger_.tick(t_, emit);
    }

    static constexpr float kEyeOpen = 0.12f;     // measured: a clean eye ≈ 0.02–0.06, noise ≈ 0.3+ (test-ax25hs prints it)
    static constexpr double kAfcGain = 0.33;
    int centre_ = 0;
    float eye_ = 1.f;
    double floor_ = 0; bool carrier_ = false;
    Opts o_;
    vibepkt::Merger merger_;
    std::unique_ptr<vibedsp::RealFir> rx_;
    std::vector<Slicer> sl_;
    // IQ front end
    double iqFs_ = 0, afcHz_ = 0;
    std::unique_ptr<vibedsp::NCO> nco_;
    std::unique_ptr<vibedsp::FirDecimator> chan_;
    vibedsp::FmDemod fm_;
    std::unique_ptr<vibedsp::RationalResampler> iqRs_, audRs_;
    double audFs_ = 0;
    std::vector<cf32> mix_, dec_;
    std::vector<float> disc_, r48_, y_;
    float hi_ = 0.1f, lo_ = -0.1f, mid_ = 0;
    double afcAcc_ = 0; int afcN_ = 0;
    uint64_t t_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE TRANSMITTER — for the tests, the benchmark row and the server test clip. Never on the air.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** AX.25 / FX.25 DATA bits → G3RUH line bits: NRZI (0 = change) then the scrambler. `pre` bits of flags are the caller's. */
inline std::vector<int> lineFromData(const std::vector<int>& data, vibepkt::Scrambler& scr, int& lvl) {
    std::vector<int> o; o.reserve(data.size());
    for (int b : data) { if (!b) lvl ^= 1; o.push_back(scr.bit(lvl)); }
    return o;
}
/** Line bits → shaped baseband (±1 at the bit centres) at `fs`: NRZ through a Gaussian filter, BT = `bt`. */
inline std::vector<float> baseband(const std::vector<int>& line, double fs, int baud, double bt = 0.5) {
    const double spb = fs / baud;
    const int half = int(std::ceil(spb * 2));
    std::vector<float> g(size_t(2 * half + 1));
    const double sig = std::sqrt(std::log(2.0)) / (2 * M_PI * bt) * spb;   // the Gaussian's σ in samples
    double sum = 0;
    for (int i = -half; i <= half; ++i) { g[size_t(i + half)] = float(std::exp(-0.5 * i * i / (sig * sig))); sum += g[size_t(i + half)]; }
    for (auto& v : g) v = float(v / sum);
    const size_t N = size_t(std::ceil(line.size() * spb)) + size_t(2 * half);
    std::vector<float> nrz(N, 0.f), out(N, 0.f);
    for (size_t i = 0; i < N; ++i) { const size_t b = size_t(double(i) / spb); nrz[i] = b < line.size() ? (line[b] ? 1.f : -1.f) : 0.f; }
    for (size_t i = 0; i < N; ++i) {
        double a = 0;
        for (int k = -half; k <= half; ++k) { const long j = long(i) + k; if (j >= 0 && j < long(N)) a += g[size_t(k + half)] * nrz[size_t(j)]; }
        out[i] = float(a);
    }
    return out;
}
/** Baseband → FM IQ at `fs`, ±`devHz` for ±1, carrier at `offHz`. */
inline std::vector<cf32> fmIq(const std::vector<float>& bb, double fs, double devHz, double offHz = 0, double phase0 = 0) {
    std::vector<cf32> o(bb.size());
    double ph = phase0;
    for (size_t i = 0; i < bb.size(); ++i) { ph += 2 * M_PI * (devHz * bb[i] + offHz) / fs; o[i] = cf32(float(std::cos(ph)), float(std::sin(ph))); }
    return o;
}

} // namespace vibeg3ruh
