// vibe_dab_resample.h — 2.4 MS/s in, 2.048 MS/s out. Rational, 64/75, polyphase.
//
// ★★★ WHY WE DO NOT SIMPLY ASK THE DONGLE FOR 2.048.
//
//  Because in practice RTL-SDR dongles do not like it, and that is not a theory — it is what
//  every working DAB implementation does. Stuart's OpenWebRX has decoded DAB off this aerial for
//  a year and EVERY ONE of its DAB profiles is 2.4 MS/s, never 2.048; he reports SDRangel having
//  had the same trouble at 2.048; and our own measurement on the XCover showed only 94% of the
//  samples arriving at 2.048 while the Pi managed 100%. 2.4 MS/s is an exact 28.8/12 division of
//  the RTL2832U's crystal and is the rate the hardware is happiest at.
//
//  ★★ The DECODER still wants exactly 2.048 MS/s, and that is not negotiable: Mode I's useful
//     symbol is 2048 samples only at that rate, which is what makes the FFT one symbol with no
//     resampling inside the demodulator. So the resampling happens HERE, once, at the edge.
//
//  ★ 2 400 000 x 64 / 75 = 2 048 000 EXACTLY, so this is a rational resampler with no accumulating
//    phase error — not an arbitrary-ratio one that would slowly drift the symbol timing, which is
//    the very fault we are trying to remove.
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace vibedab {

/** Polyphase rational resampler for complex samples: L/M = 64/75 (2.4 MS/s -> 2.048 MS/s). */
class Resample24to2048 {
public:
    static constexpr int kL = 64;      ///< interpolation
    static constexpr int kM = 75;      ///< decimation
    static constexpr int kTapsPerPhase = 12;

    Resample24to2048() {
        /* ★ A windowed sinc, cut at the OUTPUT Nyquist (the lower of the two rates) so nothing
         *  above 1.024 MHz folds back into the ensemble. Blackman window: the stopband matters
         *  more than a fraction of a dB of passband ripple, because what leaks in here lands on
         *  carriers the Viterbi will believe. */
        const int n = kL * kTapsPerPhase;
        h_.resize(size_t(n));
        const double fc = 0.5 / double(kL) * (double(kL) / double(kM) < 1.0 ? double(kL) / double(kM) : 1.0);
        for (int i = 0; i < n; ++i) {
            const double t = double(i) - double(n - 1) * 0.5;
            const double s = (std::fabs(t) < 1e-9) ? 2.0 * fc
                                                   : std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
            const double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1))
                                  + 0.08 * std::cos(4.0 * M_PI * i / (n - 1));
            h_[size_t(i)] = float(s * w * double(kL));
        }
        hist_.assign(size_t(kTapsPerPhase), {0.0f, 0.0f});
    }

    /** Push interleaved complex input; appends interleaved complex output. */
    void process(const float* in, size_t nSamples, std::vector<float>& out) {
        for (size_t i = 0; i < nSamples; ++i) {
            // Shift the history by one input sample.
            for (size_t k = kTapsPerPhase - 1; k > 0; --k) hist_[k] = hist_[k - 1];
            hist_[0] = { in[2 * i], in[2 * i + 1] };
            /* ★ Every input sample advances the phase by L; each time it passes M we owe one
             *  output sample. That is the whole of rational resampling, and it never drifts. */
            phase_ += kL;
            while (phase_ >= kM) {
                phase_ -= kM;
                const int p = phase_;                       // 0..kM-1, selects the sub-filter
                const int base = (p * kL) / kM;             // which polyphase branch
                float re = 0.0f, im = 0.0f;
                for (int k = 0; k < kTapsPerPhase; ++k) {
                    const int hi = base + k * kL;
                    if (hi >= int(h_.size())) break;
                    re += hist_[size_t(k)].re * h_[size_t(hi)];
                    im += hist_[size_t(k)].im * h_[size_t(hi)];
                }
                out.push_back(re / float(kL));
                out.push_back(im / float(kL));
            }
        }
    }

    void reset() { for (auto& s : hist_) s = {0.0f, 0.0f}; phase_ = 0; }

private:
    struct C { float re, im; };
    std::vector<float> h_;
    std::vector<C>     hist_;
    int                phase_ = 0;
};

}  // namespace vibedab
