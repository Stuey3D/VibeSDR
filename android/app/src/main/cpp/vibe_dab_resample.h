// vibe_dab_resample.h — 2.4 MS/s in, 2.048 MS/s out. Rational 64/75, polyphase.
//
// ★★★ WHY WE DO NOT SIMPLY ASK THE DONGLE FOR 2.048.
//
//  Because in practice RTL-SDR dongles are not reliable there, and that is not a theory — it is
//  what every working DAB implementation does. Every one of Stuart's OpenWebRX DAB profiles is
//  2.4 MS/s, never 2.048, on an aerial that has decoded DAB for a year; he reports SDRangel
//  having the same trouble at 2.048; and our own measurement showed the XCover delivering only
//  94% of the samples at 2.048 while the Pi managed 100%. 2.4 MS/s is an exact 28.8/12 division
//  of the RTL2832U's crystal.
//
//  ★★ The DECODER still wants exactly 2.048 MS/s and that is not negotiable: Mode I's useful
//     symbol is 2048 samples only at that rate. So the conversion happens HERE, once, at the edge.
//
//  ★ 2 400 000 x 64 / 75 = 2 048 000 EXACTLY, so the phase is periodic and the symbol timing
//    cannot drift — which an arbitrary-ratio resampler would have reintroduced.
//
// ★★★ THE FIRST VERSION OF THIS FILE WAS WRONG AND WENT ON AIR. It cost 36 dB of gain (the
//     polyphase branch already has unit DC gain, and it was divided by L a second time) and
//     wrecked the phase response off-centre, taking the live FIB pass rate from 1.0 to 0.624.
//     The mechanism was perfect — exact ratio, exact sample counts, zero drops — and the signal
//     was ruined, which is precisely the sort of failure a test catches in a second and a
//     listening test describes as "still breaking up". test-dab-resample.cpp now pins it: every
//     tone across the ensemble must come through within 1 dB and above 40 dB SNDR.
#pragma once
#include <cstdlib>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vibedab {

/** Polyphase rational resampler for interleaved complex samples: 64/75. */
class Resample24to2048 {
public:
    static constexpr int kL = 64;              ///< interpolation
    static constexpr int kM = 75;              ///< decimation
    static constexpr int kTaps = 16;           ///< taps per polyphase branch

    Resample24to2048() {
        /* Prototype low-pass at the OUTPUT Nyquist, in the upsampled domain. Cut at 1/(2*kM) of
         * the interpolated rate — the lower of the two Nyquists — so nothing folds into the
         * ensemble. Scaled by kL so each branch has unit DC gain: that scaling IS the
         * interpolation gain, and dividing by kL afterwards (as the first version did) throws the
         * signal away. */
        const int n = kL * kTaps;
        h_.assign(size_t(n), 0.0f);
        /* ★★★ THE RF BANDWIDTH, NOT JUST THE ANTI-ALIAS. Cutting at the output Nyquist
         *  (1.024 MHz) is the minimum this filter must do — it stops folding — but it passes
         *  256 kHz of noise and neighbouring-block skirt on EACH SIDE of a signal that is only
         *  +/-768 kHz wide, and all of it lands in the sync and the soft decisions.
         *  ★★★ Stuart, quoting SDRangel's DAB demodulator: "RF Bandwidth ... a filter that is
         *      applied to the input signal before decimation ... should typically be 1.537 MHz",
         *      with a 2.048 MHz decoder rate. A DAB ensemble is 1.536 MHz wide, so that is the
         *      band and everything outside it is noise by definition.
         *  ★★★ AND IT WAS MEASURED, AND IT MADE THINGS WORSE. Swept against 60 s of captured air
         *      carrying three real burst events, decoding BBC Radio 1 on 12B:
         *
         *          half-bandwidth   MP2 frames bad
         *          1.024 MHz (Nyquist, as now)   0.1%
         *          950 kHz                       0.1%
         *          850 kHz                       0.4%
         *          800 kHz                       0.8%
         *          768 kHz (the ensemble edge)   0.6%
         *
         *      Narrowing towards the nominal edge is monotonically WORSE. The reason is the one
         *      Stuart gave while it was being measured: "the 15KHz offset may be harming us with
         *      a 1.536MHz bandwidth as the ensemble is 1.536MHz wide so if we are 15KHz off tune
         *      that is the issue." We tune 15 kHz off deliberately (HW_OFFSET_HZ, to keep the
         *      dongle's DC spike out of the signal), so a filter cut at the nominal edge starts
         *      eating the outermost carriers on one side — and they carry data like every other.
         *  ★★ SO THE DEFAULT STAYS AT NYQUIST. The RF-bandwidth idea is sound in general and it
         *     is what SDRangel documents, but a filter is only free when it is wider than the
         *     signal AND centred on it. Ours is neither, and the anti-alias limit is already
         *     doing the part that matters.
         *  ★ Kept overridable so this can be re-measured on another aerial rather than argued
         *    from the spec — VIBE_DAB_RF_BW_HZ, in Hz of half-bandwidth. */
        static const double kBwHz = std::getenv("VIBE_DAB_RF_BW_HZ")
                                  ? atof(std::getenv("VIBE_DAB_RF_BW_HZ")) : 1e9;
        const double nyq = 0.5 / double(kM);           // the anti-alias limit: never exceed it
        const double want = kBwHz / (2400000.0 * double(kL));
        const double fc = want < nyq ? want : nyq;     // cycles per interpolated sample
        for (int i = 0; i < n; ++i) {
            const double t = double(i) - double(n - 1) * 0.5;
            const double s = (std::fabs(t) < 1e-9) ? 2.0 * fc
                                                   : std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
            const double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * double(i) / double(n - 1))
                                  + 0.08 * std::cos(4.0 * M_PI * double(i) / double(n - 1));
            h_[size_t(i)] = float(s * w * double(kL));
        }
        hist_.assign(kCap, C{0.0f, 0.0f});
    }

    /** Push interleaved complex input; appends interleaved complex output. */
    void process(const float* in, size_t nSamples, std::vector<float>& out) {
        for (size_t i = 0; i < nSamples; ++i) {
            hist_[size_t(inCount_ & (kCap - 1))] = { in[2 * i], in[2 * i + 1] };
            ++inCount_;
            /* ★ Output k is taken at input position k*M/L with sub-sample phase (k*M) mod L.
             *  Integer arithmetic throughout, so after every 64 outputs the phase returns exactly
             *  to where it began — no accumulating error, ever. */
            for (;;) {
                const uint64_t km = outCount_ * uint64_t(kM);
                const uint64_t n0 = km / uint64_t(kL);
                if (n0 + 1 >= inCount_) break;             // need more input
                const int p = int(km % uint64_t(kL));
                float re = 0.0f, im = 0.0f;
                for (int j = 0; j < kTaps; ++j) {
                    const int hi = p + j * kL;
                    if (hi >= int(h_.size())) break;
                    const uint64_t idx = n0 - uint64_t(j);
                    if (idx > n0) break;                   // wrapped past the start
                    const C& s = hist_[size_t(idx & (kCap - 1))];
                    re += s.re * h_[size_t(hi)];
                    im += s.im * h_[size_t(hi)];
                }
                out.push_back(re);
                out.push_back(im);
                ++outCount_;
            }
        }
    }

    void reset() {
        for (auto& s : hist_) s = { 0.0f, 0.0f };
        inCount_ = outCount_ = 0;
    }

private:
    struct C { float re, im; };
    static constexpr size_t kCap = 64;            ///< power of two, > kTaps

    std::vector<float> h_;
    std::vector<C>     hist_;
    uint64_t           inCount_ = 0, outCount_ = 0;
};

}  // namespace vibedab
