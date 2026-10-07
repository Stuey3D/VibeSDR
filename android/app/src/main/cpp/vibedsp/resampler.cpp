// VibeSDR V5 — polyphase rational resampler (real/mono). Original VibeSDR code.
#include "vibedsp.h"
#include "simd_internal.h"   // dotReal (NEON)
#include <cmath>
#include <algorithm>

namespace vibedsp {

static long long gcd_(long long a, long long b) { while (b) { long long t = a % b; a = b; b = t; } return a; }

/** ★ The fraction p/q (q <= qMax) nearest a positive double, by continued fractions — exact for
 *  any rate the server can actually produce (an integer sample rate, times a power of two from a
 *  channelizer, over an integer decimation: 3e6/296 comes back as 375000/37). */
static void toFraction_(double x, long long qMax, long long& p, long long& q) {
    long long h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    double y = x;
    for (int i = 0; i < 64; ++i) {
        const double af = std::floor(y);
        if (af > 9e15) break;
        const long long a = (long long)af;
        const long long h2 = a * h1 + h0, k2 = a * k1 + k0;
        if (k2 > qMax) break;
        h0 = h1; h1 = h2; k0 = k1; k1 = k2;
        if (std::fabs((double)h1 / (double)k1 - x) <= 1e-13 * x) break;
        const double fr = y - af;
        if (fr < 1e-15) break;
        y = 1.0 / fr;
    }
    if (k1 <= 0) { p = std::max(1LL, std::llround(x)); q = 1; return; }
    p = h1; q = k1;
}

RationalResampler::RationalResampler(int inRate, int outRate) {
    const long long g = gcd_(inRate, outRate);
    L_ = outRate / g;
    M_ = inRate / g;

    // ── Cap the interpolation factor ──────────────────────────────────────────
    // L/M is the EXACT ratio, which is fine when the rates share a factor
    // (320000 -> 48000 reduces to 3/20). But channel rates aren't always tidy:
    // 2.048 MSPS decimated by 6 gives 341333 Hz, which is coprime with 48000, so
    // L became 48000 and the polyphase tap table became L*phaseLen ~ 5.6 million
    // floats — 22 MB of taps for a filter that needs a hundred. Every output then
    // touched a cold branch and the resampler cost more than the entire rest of
    // the chain. (Measured: WFM at 2.048 MSPS was ~40% dearer than at 1.92 MSPS
    // purely because of this.)
    //
    // When the exact ratio is unreasonable, take the best rational approximation
    // with L <= kMaxL instead, while the tap table drops to a few kilobytes.
    // ★★★ (2026-10-07) "The rate error is parts-per-million — far below anything audible" was
    //     WRONG for a stream: a jitter buffer cannot absorb a rate error, it drains at it. −31.9 ppm
    //     is a step in a listener's audio every few minutes. The demod chain no longer comes here —
    //     it uses the ExactRate constructor. This one is left for the raw-IQ tap, unchanged.
    if (L_ > kMaxL) {
        const double target = (double)outRate / (double)inRate;
        double bestErr = 1e30;
        long long bestL = 1, bestM = 1;
        for (int l = 1; l <= kMaxL; ++l) {
            const long long m = std::llround((double)l / target);
            if (m < 1) continue;
            const double err = std::fabs((double)l / (double)m - target) / target;
            if (err < bestErr) { bestErr = err; bestL = l; bestM = m; if (err < 1e-7) break; }
        }
        const long long g2 = gcd_(bestL, bestM);
        L_ = bestL / g2;
        M_ = bestM / g2;
    }
    Lt_ = (int)L_;
    build_();
}

RationalResampler::RationalResampler(double inRateHz, int outRate, ExactRate) {
    long long p = 1, q = 1;
    toFraction_(inRateHz > 0.0 ? inRateHz : 1.0, 10000000LL, p, q);
    // out/in = outRate·q / p, reduced.
    const long long num = (long long)outRate * q;
    const long long g = gcd_(num, p);
    L_ = num / g;
    M_ = p / g;
    Lt_ = (int)std::min<long long>(L_, kMaxBranches);
    build_();
}

void RationalResampler::build_() {
    // Prototype low-pass at the Lt-upsampled rate. Cutoff must anti-alias both the
    // interpolation images (0.5/Lt) and the decimation (0.5/Mt, Mt = Lt·in/out); take the lower.
    // ★ With Lt_ == L_ this is exactly the old 0.5/max(L, M) — the table is unchanged.
    const double mt  = Lt_ == L_ ? (double)M_ : (double)Lt_ * (double)M_ / (double)L_;
    const double den = std::max((double)Lt_, mt);
    const double cutoff = 0.5 / den * 0.90;   // small guard margin
    const double trans  = 0.5 / den * 0.40;
    std::vector<float> proto = designLowpass(cutoff, trans);

    // Pad to a whole number of polyphase branches (length multiple of Lt_).
    phaseLen_ = (int)std::ceil((double)proto.size() / Lt_);
    std::vector<float> h((size_t)phaseLen_ * Lt_, 0.0f);
    for (size_t i = 0; i < proto.size(); ++i) h[i] = proto[i] * (float)Lt_;  // gain comp

    // Reorganise the strided polyphase taps into Lt_ CONTIGUOUS, REVERSED branches:
    // rBranch_[b*phaseLen + m] = h[b + (phaseLen-1-m)*Lt]. Then output(base,branch)
    // = dot(rBranch_[branch], &buf_[windowStart], phaseLen) over contiguous samples.
    rBranch_.assign((size_t)Lt_ * phaseLen_, 0.0f);
    for (int b = 0; b < Lt_; ++b)
        for (int m = 0; m < phaseLen_; ++m)
            rBranch_[(size_t)b * phaseLen_ + m] = h[(size_t)b + (size_t)(phaseLen_ - 1 - m) * Lt_];

    // The per-output advance, divided ONCE here (see process()).
    qStep_ = M_ / L_;
    rStep_ = M_ % L_;
    const long long s = rStep_ * (long long)Lt_;   // < L_·512: no overflow below L_ ~ 1.8e16
    sQ_ = s / L_;
    sR_ = s % L_;
    buf_.assign(phaseLen_, 0.0f);   // phaseLen samples of history
    reset();
}

void RationalResampler::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    buf_.resize(phaseLen_);
    inCount_ = 0; outCount_ = 0; outBase_ = 0;
    // ★ The invariant process() carries: bq_·L_ + br_ == ph_·Lt_ + L_/2 (round to nearest).
    ph_ = 0; bq_ = 0; br_ = L_ / 2;
}

int RationalResampler::process(const float* in, int n, float* out) {
    // buf_ = [phaseLen_ history][block]; buf_[p] is global input index
    // (inCount_ - phaseLen_) + p. Emit every output whose support is now available.
    buf_.resize((size_t)phaseLen_ + n);
    std::copy(in, in + n, buf_.begin() + phaseLen_);
    const long long avail = inCount_ + n - 1;   // newest global input index
    int outn = 0;
    /* ★ NO DIVISION PER OUTPUT (2026-09-16). This computed base = u/L and branch = u%L for every
     *  output sample with u a 64-bit product — on 32-bit ARM that is a software __udivmoddi4
     *  call, 11 % of a Pi 3's NFM budget at 250 kS/s. The quotient and remainder advance by the
     *  same constant every output (M = qStep·L + rStep), so they are carried instead: same base,
     *  same branch, exactly, for every outCount_.
     *  ★★ (2026-10-07) The same trick carries the QUANTISED branch, round(ph·Lt/L), as a quotient
     *     and remainder of its own — still no division per output. With Lt == L it is ph itself. */
    long long base = outBase_, ph = ph_, bq = bq_, br = br_;
    while (true) {
        long long eb = base, b = bq;
        if (b >= Lt_) { b -= Lt_; ++eb; }                     // rounded up onto the next input
        if (eb > avail) break;
        const long long windowStart = eb - inCount_ + 1;      // >=0 once warmed up
        if (windowStart < 0) out[outn++] = 0.0f;              // startup guard
        else out[outn++] = dotReal(&rBranch_[(size_t)b * phaseLen_], &buf_[(size_t)windowStart], phaseLen_);
        ++outCount_;
        base += qStep_; ph += rStep_; bq += sQ_; br += sR_;
        if (br >= L_) { br -= L_; ++bq; }
        if (ph >= L_) { ph -= L_; ++base; bq -= Lt_; }
    }
    outBase_ = base; ph_ = ph; bq_ = bq; br_ = br;
    // Carry the last phaseLen_ samples as history.
    std::copy(buf_.end() - phaseLen_, buf_.end(), buf_.begin());
    buf_.resize(phaseLen_);
    inCount_ += n;
    return outn;
}

} // namespace vibedsp
