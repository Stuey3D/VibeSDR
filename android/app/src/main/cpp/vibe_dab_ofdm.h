// vibe_dab_ofdm.h — DAB OFDM: fractional frequency offset, symbol extraction, DQPSK demapping.
//
// Stage 3, on top of vibe_dab_sync.h. What happens to one frame:
//
//   null | phase reference | 75 data symbols        (Mode I: L = 76 counting the reference)
//         \__ symbol 0 __/   \__ symbols 1..75 __/
//
// ★★★ DAB IS DIFFERENTIALLY ENCODED, WHICH IS WHY THERE IS NO CARRIER RECOVERY HERE.
//     Each symbol's phase is relative to the SAME CARRIER in the PREVIOUS symbol, so a constant
//     phase rotation — from a frequency offset, or from not knowing the transmitter's absolute
//     phase at all — cancels in the subtraction. That is the whole reason DAB survives on a cheap
//     dongle with a several-kHz error, and it means we need only the FRACTIONAL offset (to keep
//     the carriers in their bins) and never the absolute one.
//
// ★★ THE FRACTIONAL OFFSET COMES FREE FROM THE CYCLIC PREFIX. The guard interval is a copy of the
//    tail of the useful part, so correlating a symbol's first `guard` samples against the samples
//    one useful-period later gives a complex number whose ANGLE is the residual frequency error
//    scaled by the carrier spacing. No pilots, no search — one dot product per symbol.
//    ★ It only resolves +/- half a carrier spacing (+/-500 Hz in Mode I). The INTEGER part shifts
//      the whole spectrum by whole bins and is recovered later from the phase reference symbol,
//      which is a known sequence. Both are needed; this is the cheap half.
#pragma once
#include <string>
#include <cstdlib>
#include <cstdlib>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#include "vibedsp/neon_compat.h"
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
#include "vibe_dab_modes.h"
#include "vibe_dab_sync.h"

namespace vibedab {

using C32 = std::complex<float>;

/** Fractional carrier-frequency offset, in CARRIER SPACINGS (so -0.5 .. +0.5).
 *
 *  Correlates the cyclic prefix with its copy one useful-period later, over `symbols` symbols,
 *  accumulating before taking the angle — ★ accumulating the COMPLEX SUM and then taking one
 *  angle is not the same as averaging the angles, and is the correct thing: it weights each
 *  symbol by its own confidence and cannot be dragged about by the +/-pi wrap of a noisy one.
 *
 *  @param x       samples starting at the first symbol AFTER the null
 *  @param n       how many samples are available
 */
inline float fractionalOffset(const Cplx* x, size_t n, const Mode& m, int symbols = 8) {
    const size_t U = size_t(m.usefulSamples), G = size_t(m.guardSamples), S = U + G;
    if (!x || n < S * size_t(symbols)) return 0.0f;
    double sr = 0.0, si = 0.0;
    for (int s = 0; s < symbols; ++s) {
        const Cplx* p = x + size_t(s) * S;
        for (size_t i = 0; i < G; ++i) {
            // conj(prefix) * (its copy one useful period later)
            const float ar = p[i].re,       ai = p[i].im;
            const float br = p[i + U].re,   bi = p[i + U].im;
            sr += double(ar) * br + double(ai) * bi;
            si += double(ar) * bi - double(ai) * br;
        }
    }
    if (sr == 0.0 && si == 0.0) return 0.0f;
    // angle / 2pi = fraction of a carrier spacing
    return float(std::atan2(si, sr) / (2.0 * M_PI));
}

/** Turn the fractional offset into Hz for the DX panel. */
inline float offsetHz(float fractional, const Mode& m) { return fractional * float(m.spacingHz); }

/** De-rotate `n` samples by `cyclesPerSample` (a fraction of a cycle per sample).
 *  ★ Recurrence rather than a sin/cos per sample: at 2.048 MSPS a trig call per sample is the
 *    difference between real time and not on a Pi. Renormalised every 1024 steps because the
 *    recurrence drifts in magnitude, which would otherwise scale the constellation slowly. */
inline void derotate(Cplx* x, size_t n, double cyclesPerSample) {
    /* ★ Float, not double: this walks every sample of every frame (2 M a second), and the
     *  renormalisation every 1024 samples already bounds the drift a float oscillator has —
     *  the double version bought precision the renorm then threw away. The step itself is
     *  computed in double and rounded once.
     *  ★★ FOUR SAMPLES AT A TIME (2026-09-16): four oscillators a quarter-step apart advance by
     *     the 4-sample twiddle, so the recurrence is no longer a serial chain — the same idea as
     *     NCO::mix in vibedsp/ddc.cpp. Renormalised every 1024 samples (256 vector steps), as
     *     the scalar path is. 0.9 s of a Pi 3's 26 s DAB decode before this. */
    const double w = -2.0 * M_PI * cyclesPerSample;
    const float cs = float(std::cos(w)), sn = float(std::sin(w));
    float cr = 1.0f, ci = 0.0f;
    size_t i = 0;
#if defined(__ARM_NEON) || defined(__SSE2__)
    {
        const float c4 = float(std::cos(4.0 * w)), s4 = float(std::sin(4.0 * w));
        float pr[4], pi[4];                              // phases 0, w, 2w, 3w
        pr[0] = 1.0f; pi[0] = 0.0f;
        for (int q = 1; q < 4; ++q) { pr[q] = pr[q-1] * cs - pi[q-1] * sn; pi[q] = pr[q-1] * sn + pi[q-1] * cs; }
#if defined(__ARM_NEON)
        float32x4_t vr = vld1q_f32(pr), vi = vld1q_f32(pi);
        for (; i + 4 <= n; i += 4) {
            const float32x4x2_t v = vld2q_f32(reinterpret_cast<const float*>(x + i));
            float32x4x2_t o;
            o.val[0] = vmlsq_f32(vmulq_f32(v.val[0], vr), v.val[1], vi);
            o.val[1] = vmlaq_f32(vmulq_f32(v.val[0], vi), v.val[1], vr);
            vst2q_f32(reinterpret_cast<float*>(x + i), o);
            const float32x4_t nr = vmlsq_n_f32(vmulq_n_f32(vr, c4), vi, s4);
            const float32x4_t ni = vmlaq_n_f32(vmulq_n_f32(vr, s4), vi, c4);
            vr = nr; vi = ni;
            if (((i + 4) & 1023u) == 0) {                // renormalise all four lanes
                const float32x4_t m2 = vmlaq_f32(vmulq_f32(vr, vr), vi, vi);
                float32x4_t r = vrsqrteq_f32(m2);
                r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(m2, r), r));
                r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(m2, r), r));
                vr = vmulq_f32(vr, r); vi = vmulq_f32(vi, r);
            }
        }
        cr = vgetq_lane_f32(vr, 0); ci = vgetq_lane_f32(vi, 0);
#else
        __m128 vr = _mm_loadu_ps(pr), vi = _mm_loadu_ps(pi);
        const __m128 c4v = _mm_set1_ps(c4), s4v = _mm_set1_ps(s4);
        for (; i + 4 <= n; i += 4) {
            const __m128 a = _mm_loadu_ps(reinterpret_cast<const float*>(x + i));
            const __m128 b = _mm_loadu_ps(reinterpret_cast<const float*>(x + i) + 4);
            const __m128 xr = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)), xi = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
            const __m128 orr = _mm_sub_ps(_mm_mul_ps(xr, vr), _mm_mul_ps(xi, vi));
            const __m128 oi  = _mm_add_ps(_mm_mul_ps(xr, vi), _mm_mul_ps(xi, vr));
            _mm_storeu_ps(reinterpret_cast<float*>(x + i),     _mm_unpacklo_ps(orr, oi));
            _mm_storeu_ps(reinterpret_cast<float*>(x + i) + 4, _mm_unpackhi_ps(orr, oi));
            const __m128 nr = _mm_sub_ps(_mm_mul_ps(vr, c4v), _mm_mul_ps(vi, s4v));
            const __m128 ni = _mm_add_ps(_mm_mul_ps(vr, s4v), _mm_mul_ps(vi, c4v));
            vr = nr; vi = ni;
            if (((i + 4) & 1023u) == 0) {
                const __m128 m = _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(vr, vr), _mm_mul_ps(vi, vi)));
                vr = _mm_div_ps(vr, m); vi = _mm_div_ps(vi, m);
            }
        }
        cr = _mm_cvtss_f32(vr); ci = _mm_cvtss_f32(vi);
#endif
    }
#endif
    for (; i < n; ++i) {
        const float xr = x[i].re, xi = x[i].im;
        x[i].re = xr * cr - xi * ci;
        x[i].im = xr * ci + xi * cr;
        const float nr = cr * cs - ci * sn, ni = cr * sn + ci * cs;
        cr = nr; ci = ni;
        if ((i & 1023u) == 1023u) {          // renormalise
            const float mag = std::sqrt(cr * cr + ci * ci);
            if (mag > 0) { cr /= mag; ci /= mag; }
        }
    }
}

/** ★★ DQPSK: the phase DIFFERENCE between the same carrier in consecutive symbols carries two
 *  bits. ETSI EN 300 401 maps them so that the pair is Gray-coded around the circle, which is why
 *  a wrong decision at the boundary costs one bit rather than two.
 *
 *  Returns the two soft bits for one carrier. ★ SOFT, not hard: the Viterbi decoder downstream
 *  gains 2 dB from soft decisions, which on a marginal mux is the difference between audio and
 *  silence. Scaled to roughly +/-127 so the decoder can work in 8-bit.
 */
struct SoftBits { int8_t b0, b1; };

/** The differential product for one carrier, unscaled. Its MAGNITUDE is the channel state. */
inline C32 dqpskProduct(C32 cur, C32 prev) {
    return C32(cur.real() * prev.real() + cur.imag() * prev.imag(),
               cur.imag() * prev.real() - cur.real() * prev.imag());
}

/** ★★★ CHANNEL STATE INFORMATION — THE SINGLE BIGGEST WIN ON A MARGINAL MULTIPLEX.
 *
 *  This used to normalise EVERY CARRIER TO FULL SCALE:
 *
 *      const float scale = mag > 1e-12f ? 127.0f / mag : 0.0f;
 *
 *  which throws the magnitude away — and the magnitude IS the reliability. DAB is COFDM through
 *  multipath: at any instant some carriers sit in deep fades and carry almost pure noise, while
 *  others are strong. Normalising per carrier hands the Viterbi a faded carrier's noise with
 *  EXACTLY the same confidence as a clean carrier's signal, so the decoder cannot tell them
 *  apart and weights rubbish equally. That is the opposite of what soft decisions are for.
 *
 *  Scaling by a SYMBOL-WIDE average instead keeps the relative magnitudes, so a strong carrier
 *  arrives at +-100 and a faded one at +-10, and the Viterbi's path metric discounts the faded
 *  one automatically. This is what welle.io and dab-cmdline do, and it is worth several dB of
 *  effective sensitivity in fading — the difference between audio and silence on a weak mux,
 *  which is exactly the case Stuart wants to be best at.
 *
 *  ★ Soft-decision Viterbi already buys ~2 dB over hard decisions; CSI weighting is most of the
 *    remaining gap to what the standard's designers assumed a receiver would do.
 */
/* ★★★ THE SOFT DECISION IS AN AMPLITUDE, NOT A POWER — AND ITS SCALE WAS 5 dB OF MARGIN.
 *
 *  The DQPSK product cur·conj(prev) has magnitude |cur||prev| ≈ A², so scaling it linearly hands
 *  the Viterbi an LLR proportional to the carrier's POWER: a carrier 3 dB stronger than average
 *  counted twice as much, one 3 dB weaker half as much, and the whole distribution sat so low
 *  against the ±127 clip (average mapped to 64) that most bits were effectively soft-to-nothing.
 *
 *  ★★★ MEASURED on the 9A capture (Rugby+Daventry at the edge of reception, 32 kbit/s MP2, EEP),
 *      bad MP2 frames out of 1403, everything else identical:
 *          linear ×64 (what shipped)   70.3 %
 *          linear ×96                  51.8 %
 *          linear ×256                 15.6 %
 *          linear ×400                 14.1 %
 *          sqrt   ×200                 13.5 %   <- this
 *      FIB pass 0.83 → 0.997 alongside. 12B and 10C: 0.0 % bad before and after — a strong
 *      signal saturates either way, which is why this was never seen. welle's own MSC dump of
 *      the same 9A capture is ~45 % valid frames; this is now better than the reference on
 *      identical samples.
 *  ★ Magnitude → sqrt → amplitude, normalised by the frame's mean amplitude, then ×200 so the
 *    average carrier lands past the clip and only genuinely weak carriers stay soft. That is the
 *    shape a log-likelihood ratio should have on a channel where the noise is the same on every
 *    carrier and the signal is not.
 *  ★ VIBE_DAB_SOFT_SCALE and VIBE_DAB_SOFT_MODE=linear remain as measurement hooks. */
inline SoftBits dqpskSoftScaled(C32 product, float invAvgMag) {
    auto clamp8 = [](float v) -> int8_t {
        if (v >  127.0f) return  127;
        if (v < -127.0f) return -127;
        return int8_t(v);
    };
    static const float kScale = std::getenv("VIBE_DAB_SOFT_SCALE")
                             ? float(atof(std::getenv("VIBE_DAB_SOFT_SCALE"))) : 200.0f;
    static const bool linearMode = std::getenv("VIBE_DAB_SOFT_MODE")
                             && std::string(std::getenv("VIBE_DAB_SOFT_MODE")) == "linear";
    if (linearMode) {
        const float k = kScale * invAvgMag;
        return { clamp8(product.real() * k), clamp8(product.imag() * k) };
    }
    const float mag = std::sqrt(product.real() * product.real() + product.imag() * product.imag());
    if (mag <= 1e-12f) return { 0, 0 };
    const float amp = std::sqrt(mag) * std::sqrt(invAvgMag);   // A / mean A, roughly
    const float k = kScale * amp / mag;
    return { clamp8(product.real() * k), clamp8(product.imag() * k) };
}

/** ★★★ THE WHOLE SYMBOL, FOUR CARRIERS AT A TIME — the receiver's single largest cost.
 *
 *  Profiled on a Pi 3 (ARMv7 NEON build, 2026-09-16, gprof with inlining off): the per-carrier
 *  loop in DabReceiver — dqpskProduct, a double sqrt for the mean magnitude, dqpskSoftScaled's
 *  two sqrts and a divide, then the MER's sqrt and divide — was 42 % of the entire decode, more
 *  than the Viterbi, the FFT and the MP2 decoder put together. 1536 carriers × 76 symbols × 10.4
 *  frames a second is 1.2 million carriers a second, and each one paid ~6 transcendental calls.
 *
 *  ★★ SAME MATHS, DIFFERENT SHAPE. Everything dqpskSoftScaled computed is here, algebraically
 *     folded so the vector unit does it with estimates and no library calls:
 *       mag  = |p|                                   (sqrt of the power)
 *       k    = kScale · sqrt(|p|/avg) / |p|          (dqpskSoftScaled's amp/mag)
 *            = kScale · sqrt(invAvg) · rsqrt(mag)    — ONE reciprocal square root per carrier
 *       MER  = (|re|/mag − 1/√2)² + (|im|/mag − 1/√2)²   — the same reciprocal, reused
 *     The products are formed in CARRIER order, which is contiguous memory: the frequency
 *     de-interleave is a permutation, so the mean magnitude is the same whichever order it is
 *     summed in, and only the final byte scatter into QPSK-symbol order needs the table.
 *
 *  ★ The estimates: vrsqrte + two Newton steps is ~1 ulp, and the soft bits are rounded to int8
 *    anyway, so an occasional ±1 on a value near ±127 is the only visible difference. Verified
 *    by replaying the 12B and 10C captures: FIB rate, erased frames and MP2 bad-frame counts
 *    unchanged. The scalar path below is the reference and what a Pi Zero W (ARMv6, no NEON)
 *    runs; it keeps the same folded arithmetic so it too calls sqrt once per carrier, not four.
 *  ★ VIBE_DAB_SOFT_SCALE and VIBE_DAB_SOFT_MODE=linear remain honoured, read once. */
struct DemapSoftParams {
    float kScale = 200.0f;
    bool  linear = false;
    static const DemapSoftParams& get() {
        static const DemapSoftParams p = [] {
            DemapSoftParams q;
            if (const char* e = std::getenv("VIBE_DAB_SOFT_SCALE")) q.kScale = float(atof(e));
            if (const char* m = std::getenv("VIBE_DAB_SOFT_MODE")) q.linear = std::string(m) == "linear";
            return q;
        }();
        return p;
    }
};

/** Pass 1: products in carrier order and their magnitudes; returns the magnitude sum. */
inline double dqpskProductsAndMags(const C32* cur, const C32* prev, int K, C32* prod, float* mag) {
    double sum = 0.0;
    int c = 0;
#if defined(__ARM_NEON)
    {
        float32x4_t acc = vdupq_n_f32(0.0f);
        for (; c + 4 <= K; c += 4) {
            const float32x4x2_t a = vld2q_f32(reinterpret_cast<const float*>(cur + c));    // re, im
            const float32x4x2_t b = vld2q_f32(reinterpret_cast<const float*>(prev + c));
            float32x4x2_t p;
            p.val[0] = vmlaq_f32(vmulq_f32(a.val[0], b.val[0]), a.val[1], b.val[1]);       // ar*br + ai*bi
            p.val[1] = vmlsq_f32(vmulq_f32(a.val[1], b.val[0]), a.val[0], b.val[1]);       // ai*br - ar*bi
            vst2q_f32(reinterpret_cast<float*>(prod + c), p);
            const float32x4_t pw = vmlaq_f32(vmulq_f32(p.val[0], p.val[0]), p.val[1], p.val[1]);
            // sqrt(pw) = pw * rsqrt(pw); a zero power must give zero, not NaN
            float32x4_t r = vrsqrteq_f32(pw);
            r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(pw, r), r));
            r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(pw, r), r));
            const uint32x4_t nz = vcgtq_f32(pw, vdupq_n_f32(1e-30f));
            const float32x4_t m = vbslq_f32(nz, vmulq_f32(pw, r), vdupq_n_f32(0.0f));
            vst1q_f32(mag + c, m);
            acc = vaddq_f32(acc, m);
        }
        sum = double(vaddvq_f32(acc));
    }
#elif defined(__SSE2__)
    {
        __m128 acc = _mm_setzero_ps();
        for (; c + 4 <= K; c += 4) {
            const __m128 a0 = _mm_loadu_ps(reinterpret_cast<const float*>(cur + c));
            const __m128 a1 = _mm_loadu_ps(reinterpret_cast<const float*>(cur + c) + 4);
            const __m128 b0 = _mm_loadu_ps(reinterpret_cast<const float*>(prev + c));
            const __m128 b1 = _mm_loadu_ps(reinterpret_cast<const float*>(prev + c) + 4);
            const __m128 ar = _mm_shuffle_ps(a0, a1, _MM_SHUFFLE(2, 0, 2, 0)), ai = _mm_shuffle_ps(a0, a1, _MM_SHUFFLE(3, 1, 3, 1));
            const __m128 br = _mm_shuffle_ps(b0, b1, _MM_SHUFFLE(2, 0, 2, 0)), bi = _mm_shuffle_ps(b0, b1, _MM_SHUFFLE(3, 1, 3, 1));
            const __m128 pr = _mm_add_ps(_mm_mul_ps(ar, br), _mm_mul_ps(ai, bi));
            const __m128 pi = _mm_sub_ps(_mm_mul_ps(ai, br), _mm_mul_ps(ar, bi));
            _mm_storeu_ps(reinterpret_cast<float*>(prod + c),     _mm_unpacklo_ps(pr, pi));
            _mm_storeu_ps(reinterpret_cast<float*>(prod + c) + 4, _mm_unpackhi_ps(pr, pi));
            const __m128 m = _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(pr, pr), _mm_mul_ps(pi, pi)));
            _mm_storeu_ps(mag + c, m);
            acc = _mm_add_ps(acc, m);
        }
        __m128 t = _mm_add_ps(acc, _mm_movehl_ps(acc, acc));
        t = _mm_add_ss(t, _mm_shuffle_ps(t, t, _MM_SHUFFLE(1, 1, 1, 1)));
        sum = double(_mm_cvtss_f32(t));
    }
#endif
    for (; c < K; ++c) {
        const C32 p = dqpskProduct(cur[c], prev[c]);
        prod[c] = p;
        const float m = std::sqrt(p.real() * p.real() + p.imag() * p.imag());
        mag[c] = m;
        sum += double(m);
    }
    return sum;
}

/** Pass 2: soft bits (in carrier order, ±127) and the MER error sum, from the products and the
 *  symbol's mean magnitude. `softRe`/`softIm` are K bytes each. */
inline double dqpskSoftFromProducts(const C32* prod, const float* mag, int K, float invAvg,
                                    int8_t* softRe, int8_t* softIm) {
    const DemapSoftParams& sp = DemapSoftParams::get();
    const float kLin = sp.kScale * invAvg;                         // linear mode: k = kScale/avg
    const float kAmp = sp.kScale * std::sqrt(invAvg);              // sqrt mode:   k = kAmp · rsqrt(mag)
    const float kR2  = 0.70710678f;
    double err = 0.0;
    int c = 0;
#if defined(__ARM_NEON)
    {
        float32x4_t eacc = vdupq_n_f32(0.0f);
        const float32x4_t lo = vdupq_n_f32(-127.0f), hi = vdupq_n_f32(127.0f), r2 = vdupq_n_f32(kR2);
        const float32x4_t tiny = vdupq_n_f32(1e-12f), zero = vdupq_n_f32(0.0f);
        for (; c + 4 <= K; c += 4) {
            const float32x4x2_t p = vld2q_f32(reinterpret_cast<const float*>(prod + c));
            const float32x4_t m = vld1q_f32(mag + c);
            const uint32x4_t ok = vcgtq_f32(m, tiny);
            // 1/mag, for the MER and (in sqrt mode) for the scale
            float32x4_t rm = vrecpeq_f32(m);
            rm = vmulq_f32(rm, vrecpsq_f32(m, rm));
            rm = vmulq_f32(rm, vrecpsq_f32(m, rm));
            float32x4_t k;
            if (sp.linear) {
                k = vdupq_n_f32(kLin);
            } else {
                float32x4_t rs = vrsqrteq_f32(m);
                rs = vmulq_f32(rs, vrsqrtsq_f32(vmulq_f32(m, rs), rs));
                rs = vmulq_f32(rs, vrsqrtsq_f32(vmulq_f32(m, rs), rs));
                k = vmulq_n_f32(rs, kAmp);
            }
            k = vbslq_f32(ok, k, zero);
            const float32x4_t sr = vminq_f32(hi, vmaxq_f32(lo, vmulq_f32(p.val[0], k)));
            const float32x4_t si = vminq_f32(hi, vmaxq_f32(lo, vmulq_f32(p.val[1], k)));
            // float → int8, truncating toward zero exactly as int8_t(float) does
            const int16x4_t ir = vmovn_s32(vcvtq_s32_f32(sr)), ii = vmovn_s32(vcvtq_s32_f32(si));
            const int8x8_t br = vmovn_s16(vcombine_s16(ir, ir)), bi = vmovn_s16(vcombine_s16(ii, ii));
            vst1_lane_s32(reinterpret_cast<int32_t*>(softRe + c), vreinterpret_s32_s8(br), 0);
            vst1_lane_s32(reinterpret_cast<int32_t*>(softIm + c), vreinterpret_s32_s8(bi), 0);
            // MER against the ideal point at this carrier's own radius
            const float32x4_t ar = vsubq_f32(vmulq_f32(vabsq_f32(p.val[0]), rm), r2);
            const float32x4_t ai = vsubq_f32(vmulq_f32(vabsq_f32(p.val[1]), rm), r2);
            const float32x4_t e = vmlaq_f32(vmulq_f32(ar, ar), ai, ai);
            eacc = vaddq_f32(eacc, vbslq_f32(ok, e, zero));
        }
        err = double(vaddvq_f32(eacc));
    }
#elif defined(__SSE2__)
    {
        __m128 eacc = _mm_setzero_ps();
        const __m128 lo = _mm_set1_ps(-127.0f), hi = _mm_set1_ps(127.0f), r2 = _mm_set1_ps(kR2);
        const __m128 tiny = _mm_set1_ps(1e-12f), absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
        for (; c + 4 <= K; c += 4) {
            const __m128 p0 = _mm_loadu_ps(reinterpret_cast<const float*>(prod + c));
            const __m128 p1 = _mm_loadu_ps(reinterpret_cast<const float*>(prod + c) + 4);
            const __m128 pr = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(2, 0, 2, 0)), pi = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(3, 1, 3, 1));
            const __m128 m = _mm_loadu_ps(mag + c);
            const __m128 ok = _mm_cmpgt_ps(m, tiny);
            const __m128 msafe = _mm_max_ps(m, tiny);
            const __m128 rm = _mm_div_ps(_mm_set1_ps(1.0f), msafe);
            __m128 k = sp.linear ? _mm_set1_ps(kLin) : _mm_div_ps(_mm_set1_ps(kAmp), _mm_sqrt_ps(msafe));
            k = _mm_and_ps(ok, k);
            const __m128 sr = _mm_min_ps(hi, _mm_max_ps(lo, _mm_mul_ps(pr, k)));
            const __m128 si = _mm_min_ps(hi, _mm_max_ps(lo, _mm_mul_ps(pi, k)));
            const __m128i ir = _mm_cvttps_epi32(sr), ii = _mm_cvttps_epi32(si);
            const __m128i b = _mm_packs_epi16(_mm_packs_epi32(ir, ii), _mm_setzero_si128());   // re0..3, im0..3
            // ★ _mm_storel_epi64, NOT _mm_cvtsi128_si64: the latter is x86-64 only and stopped the
            //   i386 build (2026-10-17). Same single movq on amd64; plain SSE2 on 32-bit.
            alignas(16) int8_t both[8];
            _mm_storel_epi64(reinterpret_cast<__m128i*>(both), b);
            std::memcpy(softRe + c, both, 4);
            std::memcpy(softIm + c, both + 4, 4);
            const __m128 ar = _mm_sub_ps(_mm_mul_ps(_mm_and_ps(pr, absMask), rm), r2);
            const __m128 ai = _mm_sub_ps(_mm_mul_ps(_mm_and_ps(pi, absMask), rm), r2);
            eacc = _mm_add_ps(eacc, _mm_and_ps(ok, _mm_add_ps(_mm_mul_ps(ar, ar), _mm_mul_ps(ai, ai))));
        }
        __m128 t = _mm_add_ps(eacc, _mm_movehl_ps(eacc, eacc));
        t = _mm_add_ss(t, _mm_shuffle_ps(t, t, _MM_SHUFFLE(1, 1, 1, 1)));
        err = double(_mm_cvtss_f32(t));
    }
#endif
    for (; c < K; ++c) {
        const float re = prod[c].real(), im = prod[c].imag(), m = mag[c];
        if (!(m > 1e-12f)) { softRe[c] = 0; softIm[c] = 0; continue; }
        const float rm = 1.0f / m;
        const float k = sp.linear ? kLin : kAmp / std::sqrt(m);
        const float vr = re * k, vi = im * k;
        softRe[c] = int8_t(vr > 127.0f ? 127.0f : vr < -127.0f ? -127.0f : vr);
        softIm[c] = int8_t(vi > 127.0f ? 127.0f : vi < -127.0f ? -127.0f : vi);
        const float ar = std::fabs(re) * rm - kR2, ai = std::fabs(im) * rm - kR2;
        err += double(ar * ar + ai * ai);
    }
    return err;
}

inline SoftBits dqpskSoft(C32 cur, C32 prev) {
    const C32 p = dqpskProduct(cur, prev);
    const float mag = std::sqrt(p.real() * p.real() + p.imag() * p.imag());
    return dqpskSoftScaled(p, mag > 1e-12f ? 2.0f / mag : 0.0f);
}

/** Which FFT bin holds carrier k? DAB numbers carriers -K/2..+K/2 EXCLUDING zero (the centre
 *  carrier is not transmitted — EN 300 401 clause 14.5, "for k = 0, z = 0"), and the FFT puts
 *  negative frequencies in the upper half.
 *  ★ Getting this wrong mirrors the spectrum and every bit is noise, which is indistinguishable
 *    from "the demodulator does not work" — hence the test. */
inline int binForCarrier(int k, int fftSize) {
    return k >= 0 ? k : fftSize + k;
}

/** The carriers of one symbol, in DAB order (-K/2 .. -1, +1 .. +K/2), skipping DC. */
inline void carriersFromFft(const C32* spectrum, int fftSize, int carriers, C32* out) {
    const int half = carriers / 2;
    int j = 0;
    for (int k = -half; k <= half; ++k) {
        if (k == 0) continue;                       // centre carrier is not transmitted
        out[j++] = spectrum[binForCarrier(k, fftSize)];
    }
}

}  // namespace vibedab
