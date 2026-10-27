// vibe_dab_resample.h — 2.4 MS/s in, 2.048 MS/s out. Rational 64/75, polyphase.
//
// ★★★ SINCE 2026-09-08 THIS IS THE FALLBACK, NOT THE PATH. DAB captures at 2.048 MS/s natively on
//     every radio (see the DAB-on path in local_sdr_shim.cpp); this converter runs only when a
//     radio REFUSES 2.048 and the shim captures at 2.4 instead. The history below is kept because
//     it explains why the fallback exists and why it must stay correct.
//
// ★★★ WHY WE DID NOT SIMPLY ASK THE DONGLE FOR 2.048 (until it was re-measured).
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

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstddef>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#include "vibedsp/neon_compat.h"   // ★ 32-bit ARMv7 runs this kernel too
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
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
        hr_.assign(size_t(kL) * kTaps, 0.0f);
        for (int p = 0; p < kL; ++p)
            for (int i = 0; i < kTaps; ++i)
                hr_[size_t(p) * kTaps + size_t(i)] = h_[size_t(p) + size_t(kTaps - 1 - i) * size_t(kL)];
        re_.assign(kHist + kBlock, 0.0f);
        im_.assign(kHist + kBlock, 0.0f);
    }

    /** Push interleaved complex input; appends interleaved complex output. */
    /** ★★★ THIS FUNCTION WAS 70 % OF THE ENTIRE DAB RECEIVER — MEASURED, NOT GUESSED.
     *
     *  `sample` on dab-offline replaying 12B (2026-09-08): Resample24to2048::process 1030 samples,
     *  DabReceiver::push 128, the OFDM FFT 118, FrameSync 50, Viterbi 25. Everything the DAB
     *  standard actually asks for was a rounding error beside the rate converter in front of it.
     *  And it ran on the DSP thread — the one that also owns the spectrum — under the decoder's
     *  mutex, so vibe-dsp sat at 37 % of an XCover core in DAB with the decoder itself unable to
     *  take IQ while it ran.
     *
     *  ★★ WHY IT WAS SLOW is instructive, because the maths was never the problem: 2.048 M outputs
     *     a second x 16 taps x 2 = 65 M multiply-adds, which NEON does in its sleep. It was the
     *     scaffolding around each of those: two bounds checks per tap, a masked ring index per tap,
     *     an interleaved struct load per tap, and a vector push_back per output that checked its
     *     capacity every time. Per output that is ~100 branches for 32 MACs.
     *
     *  ★★★ THE SHAPE THAT VECTORISES:
     *     - The taps are stored BRANCH-MAJOR and TIME-REVERSED: hr_[p][i] is what multiplies the
     *       (kTaps-1-i)th newest sample, so one output is a plain 16-long dot product of the branch
     *       against a contiguous window of history.
     *     - The history is SPLIT (re[], im[]) and FLAT: 16 samples of history, then a block of
     *       input, so every window is a plain offset (2026-10-27 — it was a doubled ring; see
     *       process() for why that was slow on a Pi 2).
     *     - The first outputs read history that was never written; it is zero from construction
     *       and from reset(), so the product is exactly the "break: wrapped past the start" of the
     *       old code. Same numbers, no branch.
     *
     *  ★ SAME FILTER, SAME COEFFICIENTS, SAME RATIO. test-dab-resample.cpp still pins every tone
     *    within 1 dB and above 40 dB SNDR, and dab-offline decodes the same capture to the same
     *    frame and super-frame counts. The summation ORDER changed (four lanes instead of one
     *    chain), which is a rounding difference at 1e-7 — that is the only difference there is.
     *  ★ The scalar path is the reference the NEON and SSE2 paths are checked against, exactly as
     *    VIBE_FORCE_SCALAR does for vibedsp. */
    void process(const float* in, size_t nSamples, std::vector<float>& out) {
        /* ★ The output is written through a pointer into a pre-grown buffer and trimmed once at
         *  the end, not push_back'd twice per sample: at 2 M outputs a second the capacity check
         *  alone was measurable. The bound is exact to within one sample. */
        const size_t at = out.size();
        out.resize(at + ((nSamples + size_t(kTaps)) * size_t(kL)) / size_t(kM) * 2 + 8);
        float* o = out.data() + at;
        /* ★★ A STRAIGHT BLOCK, NOT A RING, AND TWO OUTPUTS AT A TIME (2026-10-27). On a Pi 2 this
         *  converter cost 191 ns an output — ~172 cycles for 32 multiply-adds, 40 % of a core —
         *  and the multiply-adds were not why. Per input it wrote four scalar floats (the doubled
         *  ring); per output it masked ring positions and walked 64-bit counters that a 32-bit
         *  core keeps in register PAIRS, which ran out of registers and spilled to the stack
         *  every time round; and a Cortex-A7 is in-order, so it then sat out the latency of one
         *  dependent multiply-add chain after another.
         *  ★ Now a block of input is de-interleaved into a flat history that starts with the 16
         *    samples before it, every output's window is a plain offset into that, the loop state
         *    is 32-bit and local, and two independent outputs are formed side by side (dot16x2).
         *  ★★ The NUMBERS ARE THE SAME: each output is the same dot16 of the same 16 samples
         *     against the same branch, in the same order; the samples before the first input are
         *     zero, as the ring's never-written slots were. dab-prof's digest of a 60 s capture is
         *     identical before and after, on arm64, armhf and x86. */
        float* re = re_.data();
        float* im = im_.data();
        uint32_t phase = uint32_t(phase_);
        uint64_t outs  = 0;
        size_t i0 = 0;
        while (i0 < nSamples) {
            const size_t nb = std::min(kBlock, nSamples - i0);
            const float* src = in + 2 * i0;
            for (size_t j = 0; j < nb; ++j) { re[kHist + j] = src[2 * j]; im[kHist + j] = src[2 * j + 1]; }
            /* re[0] holds input inCount_ - kHist. The next output is centred on n0_ >= inCount_ - 1,
             *  so its window [n0_ - 15, n0_] begins at offset r = n0_ - inCount_ + 1 >= 0; it is
             *  ready once n0_ + 1 < inCount_ + nb, i.e. r < nb — the old per-sample test exactly. */
            uint32_t r = uint32_t(n0_ + 1 - inCount_);
            while (r < nb) {
                const float* h = &hr_[size_t(phase) * kTaps];
                const uint32_t r1 = r;
                phase += uint32_t(kM); r += phase >> 6; phase &= uint32_t(kL - 1);     // kL is 64
                if (r < nb) {                                // ★ the next one is ready too: both together
                    dot16x2(re + r1, im + r1, h, re + r, im + r, &hr_[size_t(phase) * kTaps], o);
                    o += 4; outs += 2;
                    phase += uint32_t(kM); r += phase >> 6; phase &= uint32_t(kL - 1);
                } else {
                    dot16(re + r1, im + r1, h, o);
                    o += 2; outs += 1;
                }
            }
            inCount_ += nb;
            n0_ = inCount_ + r - nb - 1;
            // the last kHist samples become the history in front of the next block
            std::memmove(re, re + nb, kHist * sizeof(float));
            std::memmove(im, im + nb, kHist * sizeof(float));
            i0 += nb;
        }
        phase_ = int(phase);
        outCount_ += outs;
        out.resize(size_t(o - out.data()));
    }
    void reset() {
        std::fill(re_.begin(), re_.end(), 0.0f);
        std::fill(im_.begin(), im_.end(), 0.0f);
        inCount_ = outCount_ = 0;
        n0_ = 0; phase_ = 0;
    }

private:
    static constexpr size_t kHist  = kTaps;       ///< samples kept in front of each block (an output may reach back 16)
    static constexpr size_t kBlock = 256;         ///< input samples de-interleaved per block

    /** One output: the branch's 16 reversed taps against the 16 newest samples, written as
     *  (re, im) straight to `o`. On NEON the two horizontal sums are folded into one pairwise
     *  chain that lands re and im in the two lanes of one register, so the result is a single
     *  8-byte store rather than two reductions and two scalar stores. */
    static inline void dot16(const float* r, const float* q, const float* h, float* o) {
#if defined(__ARM_NEON)
        float32x4_t ar = vmulq_f32(vld1q_f32(r),      vld1q_f32(h));
        float32x4_t ai = vmulq_f32(vld1q_f32(q),      vld1q_f32(h));
        float32x4_t h1 = vld1q_f32(h + 4), h2 = vld1q_f32(h + 8), h3 = vld1q_f32(h + 12);
        ar = vmlaq_f32(ar, vld1q_f32(r + 4),  h1);  ai = vmlaq_f32(ai, vld1q_f32(q + 4),  h1);
        ar = vmlaq_f32(ar, vld1q_f32(r + 8),  h2);  ai = vmlaq_f32(ai, vld1q_f32(q + 8),  h2);
        ar = vmlaq_f32(ar, vld1q_f32(r + 12), h3);  ai = vmlaq_f32(ai, vld1q_f32(q + 12), h3);
        const float32x2_t pr = vpadd_f32(vget_low_f32(ar), vget_high_f32(ar));   // re: 2 partials
        const float32x2_t pi = vpadd_f32(vget_low_f32(ai), vget_high_f32(ai));   // im: 2 partials
        vst1_f32(o, vpadd_f32(pr, pi));                                          // {re, im}
#elif defined(__SSE2__)
        __m128 ar = _mm_mul_ps(_mm_loadu_ps(r), _mm_loadu_ps(h));
        __m128 ai = _mm_mul_ps(_mm_loadu_ps(q), _mm_loadu_ps(h));
        for (int t = 4; t < kTaps; t += 4) {
            const __m128 hv = _mm_loadu_ps(h + t);
            ar = _mm_add_ps(ar, _mm_mul_ps(_mm_loadu_ps(r + t), hv));
            ai = _mm_add_ps(ai, _mm_mul_ps(_mm_loadu_ps(q + t), hv));
        }
        alignas(16) float tr[4], ti[4];
        _mm_store_ps(tr, ar); _mm_store_ps(ti, ai);
        o[0] = (tr[0] + tr[1]) + (tr[2] + tr[3]);
        o[1] = (ti[0] + ti[1]) + (ti[2] + ti[3]);
#else
        float ar0 = 0, ar1 = 0, ar2 = 0, ar3 = 0, ai0 = 0, ai1 = 0, ai2 = 0, ai3 = 0;
        for (int t = 0; t < kTaps; t += 4) {          // four lanes, as the SIMD paths sum
            ar0 += r[t] * h[t];     ai0 += q[t] * h[t];
            ar1 += r[t+1] * h[t+1]; ai1 += q[t+1] * h[t+1];
            ar2 += r[t+2] * h[t+2]; ai2 += q[t+2] * h[t+2];
            ar3 += r[t+3] * h[t+3]; ai3 += q[t+3] * h[t+3];
        }
        o[0] = (ar0 + ar1) + (ar2 + ar3);
        o[1] = (ai0 + ai1) + (ai2 + ai3);
#endif
    }

    /** Two outputs, each exactly as dot16 forms it, with their instructions interleaved so an
     *  in-order core always has an independent multiply-add to issue (see process()). */
    static inline void dot16x2(const float* r, const float* q, const float* h,
                               const float* r2, const float* q2, const float* g, float* o) {
#if defined(__ARM_NEON)
        const float32x4_t h0 = vld1q_f32(h), g0 = vld1q_f32(g);
        float32x4_t ar = vmulq_f32(vld1q_f32(r), h0),  ai = vmulq_f32(vld1q_f32(q), h0);
        float32x4_t br = vmulq_f32(vld1q_f32(r2), g0), bi = vmulq_f32(vld1q_f32(q2), g0);
        for (int t = 4; t < kTaps; t += 4) {
            const float32x4_t hv = vld1q_f32(h + t), gv = vld1q_f32(g + t);
            ar = vmlaq_f32(ar, vld1q_f32(r + t),  hv);  ai = vmlaq_f32(ai, vld1q_f32(q + t),  hv);
            br = vmlaq_f32(br, vld1q_f32(r2 + t), gv);  bi = vmlaq_f32(bi, vld1q_f32(q2 + t), gv);
        }
        const float32x2_t pr = vpadd_f32(vget_low_f32(ar), vget_high_f32(ar)), pi = vpadd_f32(vget_low_f32(ai), vget_high_f32(ai));
        const float32x2_t qr = vpadd_f32(vget_low_f32(br), vget_high_f32(br)), qi = vpadd_f32(vget_low_f32(bi), vget_high_f32(bi));
        vst1q_f32(o, vcombine_f32(vpadd_f32(pr, pi), vpadd_f32(qr, qi)));
#else
        dot16(r, q, h, o);                           // an out-of-order x86 overlaps these itself
        dot16(r2, q2, g, o + 2);
#endif
    }

    std::vector<float> h_;                        ///< prototype, tap-major (kept for inspection)
    std::vector<float> hr_;                       ///< branch-major, time-reversed: hr_[p*kTaps+i]
    std::vector<float> re_, im_;                  ///< split history: kHist samples, then the block — see process()
    uint64_t           inCount_ = 0, outCount_ = 0;
    uint64_t           n0_ = 0;                   ///< input index the NEXT output is centred on
    int                phase_ = 0;                ///< its polyphase branch, 0..kL-1
};

}  // namespace vibedab
