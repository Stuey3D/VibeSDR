// VibeSDR V12 — auto bandwidth for SSB and CW (AutoPassband). Original VibeSDR code. See the class note in
// vibedsp.h for the design and docs/v12/AM-SYNC.md §11 for the measurements behind every constant here.
#include "vibedsp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vibedsp {

namespace {
// ★ The FIR's transition (Hz). Fixed, so the tap count never changes and every redesign is an in-place retune.
//   SSB: 150 Hz (264 Hamming taps at a 12 kHz channel). CW: 100 Hz (397 taps) — a CW skirt has to be steeper.
constexpr double kSsbTransHz = 150.0, kCwTransHz = 100.0;
constexpr double kSsbBandHz  = 100.0;     // SSB measurement band
constexpr double kSsbHiFloor = 2000.0;    // the top is never cut below this
constexpr double kSsbLoCeil  = 300.0;     // …and the bottom never above this
constexpr double kSsbCorrThr = 0.15;      // a band "belongs to this voice" at this envelope correlation (see the doc)
constexpr double kCwSearchHz = 250.0;     // the tone is looked for this far either side of the beat note
constexpr double kCwHalfHz   = 150.0;     // ±150 Hz around it
constexpr float  kCwPeakOver = 4.0f;      // 6 dB over the median bin
int pow2Near(double x) { int n = 1; while (n * 2 <= x * 1.4142) n *= 2; return std::max(16, n); }
}

AutoPassband::AutoPassband() = default;
AutoPassband::~AutoPassband() = default;

void AutoPassband::configure(Kind k, double fs, double userHiHz) {
    kind_ = k; fs_ = fs;
    userHi_ = std::max(100.0, std::min(userHiHz, fs * 0.45));
    // the filter: one fixed length for this rate and kind
    const double tr = (k == Kind::CW ? kCwTransHz : kSsbTransHz) / fs;
    taps_ = (int)designLowpass(0.25, tr, false).size();
    std::vector<float> d((size_t)taps_, 0.0f); d[(size_t)taps_ / 2] = 1.0f;
    fir_ = std::make_unique<RealFir>(d, 1);
    curLo_ = 0.0f; curHi_ = 0.0f;
    // the measurement
    N_ = pow2Near(fs / (k == Kind::CW ? 12.0 : 50.0));
    fft_ = std::make_unique<RealFFT>(N_);
    buf_.assign((size_t)N_, 0.0f); frame_.assign((size_t)N_, 0.0f); spec_.assign((size_t)N_ / 2 + 1, cf32(0, 0));
    win_.resize((size_t)N_);
    for (int i = 0; i < N_; ++i) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2.0 * M_PI * i / N_));
    fps_ = (float)(fs / N_);
    decideEvery_ = std::max(1, (int)std::lround(0.5 * fps_));
    nBands_ = std::max(1, (int)std::ceil(std::min(userHi_, 6000.0) / kSsbBandHz));
    bandOf_.assign((size_t)N_ / 2 + 1, -1);
    for (int b = 0; b <= N_ / 2; ++b) {
        const double f = (double)b * fs / N_;
        const int band = (int)(f / kSsbBandHz);
        if (band < nBands_) bandOf_[(size_t)b] = band;
    }
    coreLo_ = 5; coreHi_ = std::min(nBands_, 15);            // 500–1500 Hz
    aMom_ = 1.0 / (5.0 * fps_);
    aAvg_ = (float)(1.0 / (1.5 * fps_));
    retune();
}

void AutoPassband::setUserHi(double userHiHz) {
    const double u = std::max(100.0, std::min(userHiHz, fs_ * 0.45));
    if (std::fabs(u - userHi_) < 1.0) return;
    // ★ A new width is the listener's own act: the measurement is resized to it and every verdict starts again.
    configure(kind_, fs_, u);
}

void AutoPassband::retune() {
    fill_ = 0; frames_ = 0; sinceDecide_ = 0;
    bandP_.assign((size_t)nBands_, 0.0f);
    mB_.assign((size_t)nBands_, 0.0); mBB_.assign((size_t)nBands_, 0.0); mBC_.assign((size_t)nBands_, 0.0);
    mC_ = mCC_ = 0.0;
    avg_.assign((size_t)N_ / 2 + 1, 0.0f);
    pendTone_ = 0.0f; pendCount_ = 0; lostCount_ = 0;
    narrowVotes_ = 0;
    apply_(0.0f, 0.0f);
}

void AutoPassband::apply_(float lo, float hi) {
    if (!fir_) return;
    const bool full = hi <= 0.0f || (hi >= (float)userHi_ - 1.0f && lo <= 0.0f);
    if (full) { lo = 0.0f; hi = 0.0f; }
    if (lo == curLo_ && hi == curHi_) return;
    std::vector<float> t;
    if (full) {
        t.assign((size_t)taps_, 0.0f); t[(size_t)taps_ / 2] = 1.0f;   // a pure delay — the same latency as a cut
    } else {
        const double tr = (kind_ == Kind::CW ? kCwTransHz : kSsbTransHz) / fs_;
        t = designLowpass(std::min(0.49, hi / fs_), tr, false);
        if (lo > 0.0f) {
            const std::vector<float> l = designLowpass(lo / fs_, tr, false);
            for (size_t i = 0; i < t.size() && i < l.size(); ++i) t[i] -= l[i];
        }
    }
    if ((int)t.size() != taps_ || !fir_->retune(t)) fir_ = std::make_unique<RealFir>(t, 1);
    curLo_ = lo; curHi_ = hi;
    st_.cut = !full;
    st_.loHz = full ? 0.0f : lo;
    st_.hiHz = full ? (float)userHi_ : hi;
}

void AutoPassband::process(float* x, int n) {
    if (!fir_ || n <= 0) return;
    // ── measure the UNCUT audio ──
    int i = 0;
    while (i < n) {
        const int take = std::min(n - i, N_ - fill_);
        std::memcpy(buf_.data() + fill_, x + i, sizeof(float) * (size_t)take);
        fill_ += take; i += take;
        if (fill_ < N_) break;
        fill_ = 0;
        for (int k = 0; k < N_; ++k) frame_[(size_t)k] = buf_[(size_t)k] * win_[(size_t)k];
        fft_->forward(frame_.data(), spec_.data());
        if (kind_ == Kind::SSB) measureSsb_(spec_.data()); else measureCw_(spec_.data());
        ++frames_;
        if (++sinceDecide_ >= decideEvery_) {
            sinceDecide_ = 0;
            if (forceHi_ > 0.0f) apply_(forceLo_, forceHi_);
            else if (kind_ == Kind::SSB) decideSsb_(); else decideCw_();
        }
    }
    // ── then filter it ──
    if ((int)out_.size() < n + 1) out_.resize((size_t)n + 1);
    const int m = fir_->process(x, n, out_.data());
    std::memcpy(x, out_.data(), sizeof(float) * (size_t)std::min(m, n));
}

// ── SSB ──────────────────────────────────────────────────────────────────────────────────────────
void AutoPassband::measureSsb_(const cf32* X) {
    std::fill(bandP_.begin(), bandP_.end(), 0.0f);
    for (int b = 0; b <= N_ / 2; ++b) {
        const int band = bandOf_[(size_t)b];
        if (band >= 0) bandP_[(size_t)band] += X[b].real() * X[b].real() + X[b].imag() * X[b].imag();
    }
    float core = 0.0f;
    for (int b = coreLo_; b < coreHi_; ++b) core += bandP_[(size_t)b];
    const double eps = 1e-20;
    const double ec = std::log((double)core + eps);
    const double a = frames_ < 8 ? 1.0 / (frames_ + 1) : std::max(aMom_, 1.0 / (frames_ + 1));
    mC_ += a * (ec - mC_); mCC_ += a * (ec * ec - mCC_);
    for (int b = 0; b < nBands_; ++b) {
        const double e = std::log((double)bandP_[(size_t)b] + eps);
        mB_[(size_t)b] += a * (e - mB_[(size_t)b]);
        mBB_[(size_t)b] += a * (e * e - mBB_[(size_t)b]);
        mBC_[(size_t)b] += a * (e * ec - mBC_[(size_t)b]);
    }
}

void AutoPassband::decideSsb_() {
    if (frames_ < (int)(3.0f * fps_)) return;                       // 3 s of evidence first
    const double vc = mCC_ - mC_ * mC_;
    if (!(vc > 1e-6)) return;
    std::vector<double> rho((size_t)nBands_, 0.0);
    for (int b = 0; b < nBands_; ++b) {
        const double vb = mBB_[(size_t)b] - mB_[(size_t)b] * mB_[(size_t)b];
        const double cv = mBC_[(size_t)b] - mB_[(size_t)b] * mC_;
        rho[(size_t)b] = vb > 1e-9 ? cv / std::sqrt(vb * vc) : 0.0;
    }
    /* ★★ NOTHING TO FIT TO, NOTHING CUT. With no voice the core's log power barely moves (noise alone: a variance
     *  of ~0.05 over a 1 kHz band; a talker's syllables and pauses: well over 1). Fitting a passband to noise would
     *  leave the next voice to arrive cut until the evidence caught up. */
    if (vc < 0.3) { propose_(0.0f, (float)userHi_, 0.0f); return; }
    /* ★★ A BAND COUNTS WHEN IT AND ITS INNER NEIGHBOUR BOTH CORRELATE. A three-band average leaked the voice
     *  outward — measured: 200–300 Hz kept for a 300 Hz transmitter, and a louder second talker's band at the top
     *  kept because the band below it was this voice's. Two in a row is as robust against one noisy band. */
    const int topFloor = (int)(kSsbHiFloor / kSsbBandHz);
    double hi = std::min(userHi_, kSsbHiFloor);
    for (int b = nBands_ - 1; b >= topFloor; --b)
        if (rho[(size_t)b] >= kSsbCorrThr && rho[(size_t)b - 1] >= kSsbCorrThr) { hi = std::min(userHi_, (b + 1) * kSsbBandHz + 75.0); break; }
    double lo = kSsbLoCeil;
    for (int b = 0; b < (int)(kSsbLoCeil / kSsbBandHz); ++b)
        if (rho[(size_t)b] >= kSsbCorrThr && rho[(size_t)b + 1] >= kSsbCorrThr) { lo = std::max(0.0, b * kSsbBandHz - 75.0); break; }
    if (lo < 100.0) lo = 0.0;
    if (std::getenv("VIBE_APB_DEBUG")) {
        std::fprintf(stderr, "[apb] vc %.2f rho:", vc);
        for (int b = 0; b < nBands_; ++b) std::fprintf(stderr, " %d:%.2f", b, rho[(size_t)b]);
        std::fprintf(stderr, " -> %.0f-%.0f\n", lo, hi);
    }
    propose_((float)lo, (float)hi, 0.0f);
}

/** ★ Widening is at once; narrowing needs four decisions running (2 s) and takes the widest of them. */
void AutoPassband::propose_(float lo, float hi, float tone) {
    const float cLo = st_.cut ? curLo_ : 0.0f, cHi = st_.cut ? curHi_ : (float)userHi_;
    const bool wider = lo < cLo - 1.0f || hi > cHi + 1.0f;
    const bool narrower = lo > cLo + 1.0f || hi < cHi - 1.0f;
    st_.toneHz = tone;
    if (wider) {
        // widen the edge that asked at once; an edge that would narrow keeps its place
        apply_(std::min(lo, cLo), std::max(hi, cHi));
        narrowVotes_ = 0;
        return;
    }
    if (!narrower) { narrowVotes_ = 0; return; }
    if (narrowVotes_ == 0) { narrowLo_ = lo; narrowHi_ = hi; }
    else { narrowLo_ = std::min(narrowLo_, lo); narrowHi_ = std::max(narrowHi_, hi); }
    if (++narrowVotes_ >= 4) { apply_(narrowLo_, narrowHi_); narrowVotes_ = 0; }
}

// ── CW ───────────────────────────────────────────────────────────────────────────────────────────
void AutoPassband::measureCw_(const cf32* X) {
    const float a = frames_ < 4 ? 1.0f / (float)(frames_ + 1) : std::max(aAvg_, 1.0f / (float)(frames_ + 1));
    for (int b = 0; b <= N_ / 2; ++b) {
        const float p = X[b].real() * X[b].real() + X[b].imag() * X[b].imag();
        avg_[(size_t)b] += a * (p - avg_[(size_t)b]);
    }
}

void AutoPassband::decideCw_() {
    if (frames_ < (int)(2.0f * fps_)) return;
    const double binHz = fs_ / N_;
    const int kLo = std::max(1, (int)(100.0 / binHz)), kHi = std::min(N_ / 2 - 1, (int)((userHi_ - 100.0) / binHz));
    if (kHi - kLo < 8) return;
    std::vector<float> tmp(avg_.begin() + kLo, avg_.begin() + kHi + 1);
    std::nth_element(tmp.begin(), tmp.begin() + (long)tmp.size() / 2, tmp.end());
    const float floorP = std::max(tmp[tmp.size() / 2], 1e-30f);
    auto S = [&](int k) { return (avg_[(size_t)k - 1] + avg_[(size_t)k] + avg_[(size_t)k + 1]) * (1.0f / 3.0f); };
    const double centre = userHi_ * 0.5;
    const int sLo = std::max(kLo, (int)((centre - kCwSearchHz) / binHz)), sHi = std::min(kHi, (int)((centre + kCwSearchHz) / binHz) + 1);
    /* ★★★ THE PEAK NEAREST THE BEAT NOTE, NOT THE LOUDEST. The listener put the station they want on the beat
     *  note; the loudest tone in reach can be a neighbour. Measured: "strongest within ±250 Hz" locked onto a
     *  neighbour 6 dB louder 250 Hz away and scored −17.5 dB. A peak is a local maximum ≥ 6 dB over the median bin. */
    int best = -1; float bestS = 0.0f; double bestD = 1e9;
    for (int k = std::max(sLo, kLo + 1); k <= std::min(sHi, kHi - 1); ++k) {
        const float s = S(k);
        if (s < kCwPeakOver * floorP || s < S(k - 1) || s < S(k + 1)) continue;
        const double d = std::fabs(k * binHz - centre);
        if (d < bestD - 1e-9) { bestD = d; best = k; bestS = s; }
    }
    /* ★★ HOLD THE TONE YOU HAVE. While a cut is in place and its tone is still there (a peak within 40 Hz of it), it
     *  is kept — measured on 40 m at night: without this, a pause in the wanted signal re-centred the passband on a
     *  neighbour 300 Hz up for a few seconds. A cut tone that has gone for 3 s widens to the full passband first;
     *  only from there is a new tone taken. */
    if (st_.cut && st_.toneHz > 0.0f) {
        int keep = -1; float keepS = 0.0f;
        for (int k = std::max(kLo + 1, (int)((st_.toneHz - 40.0) / binHz)); k <= std::min(kHi - 1, (int)((st_.toneHz + 40.0) / binHz) + 1); ++k) {
            const float s = S(k);
            if (s >= kCwPeakOver * floorP && s >= S(k - 1) && s >= S(k + 1) && s > keepS) { keepS = s; keep = k; }
        }
        if (keep < 0) {
            if (++lostCount_ >= 6) { apply_(0.0f, 0.0f); lostCount_ = 0; st_.toneHz = 0.0f; pendCount_ = 0; }
            return;
        }
        best = keep; bestS = keepS; pendCount_ = std::max(pendCount_, 1); pendTone_ = st_.toneHz;
    }
    const bool here = best > 0 && bestS >= kCwPeakOver * floorP;
    if (!here) {
        pendCount_ = 0;
        // ★ the tone has gone: after 3 s, back to the listener's full CW passband
        if (st_.cut && ++lostCount_ >= 6) { apply_(0.0f, 0.0f); lostCount_ = 0; st_.toneHz = 0.0f; }
        return;
    }
    lostCount_ = 0;
    // parabolic peak on the smoothed spectrum
    double tone = best * binHz;
    { const double y0 = S(best - 1 > kLo ? best - 1 : best), y1 = bestS, y2 = S(best + 1 < kHi ? best + 1 : best);
      const double den = y0 - 2.0 * y1 + y2; if (den < 0.0) tone += binHz * 0.5 * (y0 - y2) / den; }
    if (pendCount_ > 0 && std::fabs(tone - pendTone_) <= 30.0) { ++pendCount_; pendTone_ = (float)(0.5 * (pendTone_ + tone)); }
    else { pendTone_ = (float)tone; pendCount_ = 1; return; }
    if (pendCount_ < 2) return;
    // ★ a neighbour closer than the half-width: narrow further so it stays out (never under ±60 Hz)
    double half = kCwHalfHz;
    for (int k = kLo + 1; k < kHi; ++k) {
        const float s = S(k);
        if (s < kCwPeakOver * floorP || s < S(k - 1) || s < S(k + 1)) continue;
        const double d = std::fabs(k * binHz - pendTone_);
        if (d > 50.0) half = std::min(half, std::max(60.0, d - 80.0));
    }
    const float lo = (float)std::max(0.0, pendTone_ - half), hi = (float)std::min(userHi_, pendTone_ + half);
    // ★ a confirmed tone (two decisions, 1 s) is applied at once: re-centring is not a narrowing to vote on
    if (!st_.cut || std::fabs(0.5f * (curLo_ + curHi_) - pendTone_) > 20.0f || std::fabs((curHi_ - curLo_) - (hi - lo)) > 20.0f)
        apply_(lo, hi);
    st_.toneHz = pendTone_;
}

} // namespace vibedsp
