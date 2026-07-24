// VibeSDR V5 — polyphase rational resampler (real/mono). Original VibeSDR code.
#include "vibedsp.h"
#include "simd_internal.h"   // dotReal (NEON)
#include <cmath>
#include <algorithm>

namespace vibedsp {

static int gcd_(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

RationalResampler::RationalResampler(int inRate, int outRate) {
    const int g = gcd_(inRate, outRate);
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
    // with L <= kMaxL instead. The rate error is parts-per-million — far below
    // anything audible, and well inside what the audio jitter buffer already
    // absorbs — while the tap table drops to a few kilobytes.
    constexpr int kMaxL = 256;
    if (L_ > kMaxL) {
        const double target = (double)outRate / (double)inRate;
        double bestErr = 1e30;
        int bestL = 1, bestM = 1;
        for (int l = 1; l <= kMaxL; ++l) {
            const int m = (int)std::llround((double)l / target);
            if (m < 1) continue;
            const double err = std::fabs((double)l / (double)m - target) / target;
            if (err < bestErr) { bestErr = err; bestL = l; bestM = m; if (err < 1e-7) break; }
        }
        const int g2 = gcd_(bestL, bestM);
        L_ = bestL / g2;
        M_ = bestM / g2;
    }

    // Prototype low-pass at the L-upsampled rate. Cutoff must anti-alias both the
    // interpolation images (0.5/L) and the decimation (0.5/M); take the lower.
    const double cutoff = 0.5 / std::max(L_, M_) * 0.90;   // small guard margin
    const double trans  = 0.5 / std::max(L_, M_) * 0.40;
    std::vector<float> proto = designLowpass(cutoff, trans);

    // Pad to a whole number of polyphase branches (length multiple of L_).
    phaseLen_ = (int)std::ceil((double)proto.size() / L_);
    std::vector<float> h((size_t)phaseLen_ * L_, 0.0f);
    for (size_t i = 0; i < proto.size(); ++i) h[i] = proto[i] * (float)L_;  // gain comp

    // Reorganise the strided polyphase taps into L_ CONTIGUOUS, REVERSED branches:
    // rBranch_[b*phaseLen + m] = h[b + (phaseLen-1-m)*L]. Then output(base,branch)
    // = dot(rBranch_[branch], &buf_[windowStart], phaseLen) over contiguous samples.
    rBranch_.assign((size_t)L_ * phaseLen_, 0.0f);
    for (int b = 0; b < L_; ++b)
        for (int m = 0; m < phaseLen_; ++m)
            rBranch_[(size_t)b * phaseLen_ + m] = h[b + (phaseLen_ - 1 - m) * L_];

    buf_.assign(phaseLen_, 0.0f);   // phaseLen samples of history
}

void RationalResampler::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    buf_.resize(phaseLen_);
    inCount_ = 0; outCount_ = 0;
}

int RationalResampler::process(const float* in, int n, float* out) {
    // buf_ = [phaseLen_ history][block]; buf_[p] is global input index
    // (inCount_ - phaseLen_) + p. Emit every output whose support is now available.
    buf_.resize((size_t)phaseLen_ + n);
    std::copy(in, in + n, buf_.begin() + phaseLen_);
    const long long avail = inCount_ + n - 1;   // newest global input index
    int outn = 0;
    while (true) {
        const long long u = outCount_ * (long long)M_;
        const long long base = u / L_;          // newest input index this output uses
        if (base > avail) break;
        const int branch = (int)(u % L_);
        const int windowStart = (int)(base - inCount_ + 1);   // >=0 once warmed up
        if (windowStart < 0) { ++outCount_; out[outn++] = 0.0f; continue; }  // startup guard
        out[outn++] = dotReal(&rBranch_[(size_t)branch * phaseLen_], &buf_[windowStart], phaseLen_);
        ++outCount_;
    }
    // Carry the last phaseLen_ samples as history.
    std::copy(buf_.end() - phaseLen_, buf_.end(), buf_.begin());
    buf_.resize(phaseLen_);
    inCount_ += n;
    return outn;
}

} // namespace vibedsp
