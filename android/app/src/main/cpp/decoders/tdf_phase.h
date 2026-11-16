// tdf_phase.h — V12 2026-11-16 TDF / ALS162 (Allouis, 162 kHz): the PHASE front end that turns the station's phase
// time code into the "carrier low" envelope the DCF77 reader already reads. docs/v12/TIME-TDF.md.
//
// ★★★ WHY A FRONT END AND NOT A NEW DECODER. ALS162 sends the DCF77 time code (bits 15–18 and 20–58 as DCF77; bits 1–2
//     leap-second warnings, 3–6 a check weight, 13–14 holidays) with the SAME symbol timing — one element (100 ms) for a
//     0, two (200 ms) for a 1, nothing at second 59 — but as PHASE modulation of a constant carrier, so the AM envelope
//     the other stations are read from is flat. Each element is a triangle of phase: +1 rad over 25 ms, −2 rad over
//     50 ms, +1 rad over 25 ms (ANFR's published format; the zero crossing in the middle of the first element is the top
//     of the second). Turn "an element is here" into "the carrier is low here" and the whole DCF77 reader — the
//     one-second flywheel, the templates, the vote, the partial fields, the two-minute corroboration — reads TDF as it is.
//
// The chain (scalar: ~10 multiply-adds per input sample at 48 kHz and a 101-tap correlator at 1 kHz — a few MFLOP/s,
// nothing to vectorise; the house SIMD rule is for hot loops and this is not one):
//   1. FIND THE TONE: the listener tunes CW/USB so the carrier is a beat note (300–3000 Hz). Goertzel over a 0.4 s block
//      (every 4th sample at 48 kHz), 10 Hz steps, parabolic refinement — once every 2 s while there is no tone (~0.7 M
//      multiply-adds a second), never once locked; again when the lock is lost.
//   2. MIX TO 0 Hz, LOW-PASS (three one-pole sections, ~120 Hz), DECIMATE to 1 kHz.
//   3. A SLOW PLL (≈ 3 Hz while acquiring, 0.7 Hz once settled) follows the carrier and the receiver's drift but not the
//      10 Hz triangle — whose mean phase is zero, so it does not bias the loop. Its phase error IS the modulation.
//   4. MATCHED FILTER: the error correlated with one element's triangle (101 taps), normalised so a full-strength
//      element reads 1.0 at its centre.
//   5. THE ENVELOPE: low within ±35 ms of any element centre (+ the correlation's own spread) (the window maximum of the correlation), so a 0 is 100 ms
//      low and a 1 is 200 ms low, the dip starting kDipAfterSecondMs after the top of the second — the reader's
//      flywheel takes the edge where it finds it; the host corrects the clock it offers by that (time_sync).
// ★ OUTPUT SCALE: 1000 when quiet, down to 50 at a full element — a number the reader's percentile normaliser and its
//   "carrier dB" line treat as they treat a real carrier (a clean signal reads ~20 dB, noise ~0–3 dB).
// ★ Not known from the published material we have: the format of the "additional non-timing data" ALS162 is said to
//   send by phase modulation later in each second. It is outside the reader's 250 ms window; how much it disturbs the
//   envelope's percentiles is an on-air question (docs/v12/TIME-TDF.md).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace vibe {

class TdfPhase {
public:
    /** ★ The time from the TOP OF THE SECOND to where this envelope's dip starts, ms. The first element starts 50 ms
     *  BEFORE the second, the correlator answers at its centre (the second), and the ±35 ms window opens the dip
     *  ~35 ms after that. The host adds it to the clock's receive delay (time_sync rxDelayMs). Measured against DCF77's
     *  AM read by the same reader (test-tdf: +35 ms), which fails if it moves. */
    static constexpr double kDipAfterSecondMs = 35.0;

    explicit TdfPhase(int sampleRate) : sr_(sampleRate > 0 ? sampleRate : 48000) {
        dec_ = std::max(1, sr_ / 1000);
        fsD_ = double(sr_) / dec_;
        const double fc = 120.0;
        lpA_ = 1.0 - std::exp(-2.0 * M_PI * fc / sr_);
        // ★ the matched filter: one element, +1 rad over 25 ms, −2 over 50, +1 over 25 (101 taps at 1 kHz)
        const int n = int(std::lround(0.100 * fsD_)) + 1;
        tpl_.resize(size_t(n));
        double e = 0;
        for (int k = 0; k < n; ++k) {
            const double t = k / fsD_;   // 0 … 0.1 s
            double ph;
            if (t < 0.025) ph = t / 0.025;
            else if (t < 0.075) ph = 1.0 - 2.0 * (t - 0.025) / 0.050;
            else ph = -1.0 + (t - 0.075) / 0.025;
            tpl_[size_t(k)] = ph; e += ph * ph;
        }
        for (double& v : tpl_) v /= e;   // ★ a full ±1 rad element correlates to 1.0
        err_.assign(tpl_.size(), 0.0);
        // ★ ±35 ms, not ±50: the correlation itself stays high ~±15 ms round a centre, so a ±50 ms window made a 0
        //   125 ms long and the reader's 100 / 200 ms templates told it from a 1 with only a quarter of their margin
        corr_.assign(size_t(std::lround(0.070 * fsD_)) + 1, 0.0);
        subN_ = std::max(1, sr_ / 12000);   // ★ every 4th sample at 48 kHz; every one at a 12 kHz recording
        search_.reserve(size_t(0.4 * sr_ / subN_) + 1);
    }

    /** One input sample → the envelope value now (held between 1 kHz updates). */
    double push(double x) {
        if (!locked_) { searchSample(x); return env_; }
        // 1–2: mix to 0 Hz and low-pass
        const double c = std::cos(ph_), s = std::sin(ph_);
        ph_ += w_; if (ph_ > M_PI) ph_ -= 2 * M_PI;
        double i = x * c, q = -x * s;
        for (int k = 0; k < 3; ++k) { li_[k] += lpA_ * (i - li_[k]); i = li_[k]; lq_[k] += lpA_ * (q - lq_[k]); q = lq_[k]; }
        if (++dc_ < dec_) return env_;
        dc_ = 0;
        step1k(i, q);
        return env_;
    }

    double toneHz() const { return locked_ ? w_ * sr_ / (2 * M_PI) : 0.0; }
    bool locked() const { return locked_; }
    /** The latest element strength (radians-equivalent, 1.0 = a clean full element). */
    double elementLevel() const { return lastPeak_; }

private:
    void searchSample(double x) {
        if (skip_ > 0) { --skip_; return; }
        if (++sub_ < subN_) return;
        sub_ = 0;
        search_.push_back(x);
        if (search_.size() < size_t(0.4 * sr_ / subN_)) return;
        const double srS = double(sr_) / subN_;
        skip_ = int(1.6 * sr_);   // ★ the next try 2 s on, if this one finds nothing
        // ★ Goertzel, 300–3000 Hz in 10 Hz steps, then a parabola through the best three
        const int n = int(search_.size());
        double bestP = 0, pL = 0, pR = 0; double bestF = 0;
        double prev = 0, prev2 = 0, prevF = 0;
        double total = 0;
        for (const double v : search_) total += v * v;
        for (double f = 300; f <= 3000; f += 10) {
            const double w = 2 * M_PI * f / srS, cw = 2 * std::cos(w);
            double s1 = 0, s2 = 0;
            for (int k = 0; k < n; ++k) { const double s0 = search_[size_t(k)] + cw * s1 - s2; s2 = s1; s1 = s0; }
            const double p = s1 * s1 + s2 * s2 - cw * s1 * s2;
            if (prevF > 0 && prev > bestP && prev >= p && prev >= prev2) { bestP = prev; bestF = prevF; pL = prev2; pR = p; }
            prev2 = prev; prev = p; prevF = f;
        }
        search_.clear();
        // ★ a tone, not noise: its bin holds a good share of the block's energy
        const double share = total > 0 ? bestP / (total * n / 2.0) : 0;
        if (bestF <= 0 || share < 0.05) return;
        const double den = pL - 2 * bestP + pR;
        const double d = den != 0 ? 0.5 * (pL - pR) / den : 0;
        const double f0 = bestF + std::max(-0.5, std::min(0.5, d)) * 10.0;
        w_ = 2 * M_PI * f0 / sr_; w0_ = w_; ph_ = 0;
        for (int k = 0; k < 3; ++k) li_[k] = lq_[k] = 0;
        locked_ = true; settle_ = 0; lowRun_ = 0; amp_ = 0; skip_ = 0;
        pllPh_ = 0; pllW_ = 0;
    }

    void step1k(double i, double q) {
        // ★ amplitude, for "is the tone still there"
        const double a = std::sqrt(i * i + q * q);
        amp_ += (amp_ == 0 ? 1.0 : 0.002) * (a - amp_);
        // 3: the slow PLL on the decimated carrier
        const double cr = std::cos(pllPh_), sr = std::sin(pllPh_);
        const double zi = i * cr + q * sr, zq = q * cr - i * sr;
        const double e = std::atan2(zq, zi);
        ++settle_;
        const double bn = settle_ < 3000 ? 3.0 : 0.7, zeta = 0.707;
        const double wn = 2 * bn / (zeta + 1 / (4 * zeta));
        const double kp = 2 * zeta * wn / fsD_, ki = (wn / fsD_) * (wn / fsD_);
        pllW_ += ki * e;
        pllPh_ += pllW_ + kp * e;
        if (pllPh_ > M_PI) pllPh_ -= 2 * M_PI; else if (pllPh_ < -M_PI) pllPh_ += 2 * M_PI;
        // ★ the mixer follows the PLL's frequency too, so the tone stays inside the low-pass as the receiver drifts
        w_ = w0_ + pllW_ * fsD_ / sr_;
        // 4: the matched filter
        err_[ei_] = e; ei_ = (ei_ + 1) % err_.size();
        double c = 0;
        const size_t n = tpl_.size();
        for (size_t k = 0; k < n; ++k) c += tpl_[k] * err_[(ei_ + k) % n];   // oldest first = tpl_[0]
        corr_[ci_] = c; ci_ = (ci_ + 1) % corr_.size();
        // 5: the envelope — low within ±35 ms of an element centre
        double m = 0;
        for (const double v : corr_) m = std::max(m, v);
        if (m > 0.3) lastPeak_ = m;
        env_ = 1000.0 * (1.0 - std::min(0.95, std::max(0.0, m)));
        // ★ the tone gone (a tuning change, a fade to nothing): search again
        if (settle_ > 2000 && a < 0.1 * amp_) { if (++lowRun_ > 1500) { locked_ = false; env_ = 1000.0; } }
        else lowRun_ = 0;
    }

    const int sr_;
    int dec_ = 48, dc_ = 0;
    double fsD_ = 1000;
    double lpA_ = 0, li_[3] = {0, 0, 0}, lq_[3] = {0, 0, 0};
    double ph_ = 0, w_ = 0, w0_ = 0;
    double pllPh_ = 0, pllW_ = 0;
    bool locked_ = false;
    int settle_ = 0, lowRun_ = 0, skip_ = 0, sub_ = 0, subN_ = 4;
    double amp_ = 0, lastPeak_ = 0;
    std::vector<double> search_, tpl_, err_, corr_;
    size_t ei_ = 0, ci_ = 0;
    double env_ = 1000.0;
};

}  // namespace vibe
