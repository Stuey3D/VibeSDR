// VibeSDR V4 — NLMS automatic notch filter (adaptive line enhancer, notch mode).
#include "auto_notch.h"
#include "../vibedsp/simd_internal.h"   // dotReal + the vector NLMS update
#include <algorithm>

namespace vibe {

namespace {
/** ★ dotReal(w, s, L) and dotReal(s, s, L) in ONE pass over s (2026-10-31, the decoder SIMD audit): the same two
 *  accumulators per product, the same partial sums and the same final reduction as vibedsp::dotReal, so both results
 *  are bit-identical to the two calls — with s loaded once instead of three times. */
inline void dotPair(const float* w, const float* s, int K, float& fir, float& pwr) {
#if VIBE_NEON
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f), b0 = vdupq_n_f32(0.0f), b1 = vdupq_n_f32(0.0f);
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        const float32x4_t s0 = vld1q_f32(s + j), s1 = vld1q_f32(s + j + 4);
        a0 = vmlaq_f32(a0, vld1q_f32(w + j), s0); a1 = vmlaq_f32(a1, vld1q_f32(w + j + 4), s1);
        b0 = vmlaq_f32(b0, s0, s0);               b1 = vmlaq_f32(b1, s1, s1);
    }
    if (j + 4 <= K) { const float32x4_t s0 = vld1q_f32(s + j); a0 = vmlaq_f32(a0, vld1q_f32(w + j), s0); b0 = vmlaq_f32(b0, s0, s0); j += 4; }
    float f = vaddvq_f32(vaddq_f32(a0, a1)), p = vaddvq_f32(vaddq_f32(b0, b1));
    for (; j < K; ++j) { f += w[j] * s[j]; p += s[j] * s[j]; }
    fir = f; pwr = p;
#elif VIBE_SSE
    __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps(), b0 = _mm_setzero_ps(), b1 = _mm_setzero_ps();
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        const __m128 s0 = _mm_loadu_ps(s + j), s1 = _mm_loadu_ps(s + j + 4);
        a0 = vibedsp::sseMla(a0, _mm_loadu_ps(w + j), s0); a1 = vibedsp::sseMla(a1, _mm_loadu_ps(w + j + 4), s1);
        b0 = vibedsp::sseMla(b0, s0, s0);                   b1 = vibedsp::sseMla(b1, s1, s1);
    }
    if (j + 4 <= K) { const __m128 s0 = _mm_loadu_ps(s + j); a0 = vibedsp::sseMla(a0, _mm_loadu_ps(w + j), s0); b0 = vibedsp::sseMla(b0, s0, s0); j += 4; }
    float f = vibedsp::sseAddv(_mm_add_ps(a0, a1)), p = vibedsp::sseAddv(_mm_add_ps(b0, b1));
    for (; j < K; ++j) { f += w[j] * s[j]; p += s[j] * s[j]; }
    fir = f; pwr = p;
#else
    fir = vibedsp::dotReal(w, s, K);
    pwr = vibedsp::dotReal(s, s, K);
#endif
}
}  // namespace

AutoNotch::AutoNotch() { buf.assign(2 * M, 0.0f); w.assign(L, 0.0f); }

void AutoNotch::reset() {
    std::fill(buf.begin(), buf.end(), 0.0f);
    std::fill(w.begin(), w.end(), 0.0f);
    p = 0;
}

void AutoNotch::process(float* x, int count) {
    for (int n = 0; n < count; n++) {
        // Decreasing write pointer + mirror at p+M: the most recent M samples are
        // always contiguous at buf[p .. p+M-1], newest first — so x[n-d] = buf[p+d]
        // with no per-tap modulo.
        p = (p == 0) ? M - 1 : p - 1;
        float in = x[n];
        buf[p] = in; buf[p + M] = in;

        // FIR predicts the periodic part from samples delayed by D..D+L-1;
        // pwr is the energy of those same taps for the NLMS normalisation.
        int base = p + D;                // index of x[n-D]; base+i = x[n-D-i]
        /* ★ 160 taps, twice, per audio sample = 15 M multiply-adds a second — the heaviest
         *  audio-rate loop in the engine, and it was scalar (2026-09-16). The prediction and
         *  the tap energy are two dot products; the update is an axpy. L is a multiple of 8. */
        const float* s = &buf[(size_t)base];
        float fir, pwr;
        dotPair(w.data(), s, L, fir, pwr);
        float err = in - fir;            // tones removed → notch output
        x[n] = err;

        // Leaky NLMS coefficient update.
        float g = mu * err / (eps + pwr);
        {
            int i = 0;
#if VIBE_NEON
            const float32x4_t lk = vdupq_n_f32(leak), gv = vdupq_n_f32(g);
            for (; i + 4 <= L; i += 4)
                vst1q_f32(&w[(size_t)i], vmlaq_f32(vmulq_f32(lk, vld1q_f32(&w[(size_t)i])), gv, vld1q_f32(s + i)));
#elif VIBE_SSE
            const __m128 lk = _mm_set1_ps(leak), gv = _mm_set1_ps(g);
            for (; i + 4 <= L; i += 4)
                _mm_storeu_ps(&w[(size_t)i], _mm_add_ps(_mm_mul_ps(lk, _mm_loadu_ps(&w[(size_t)i])), _mm_mul_ps(gv, _mm_loadu_ps(s + i))));
#endif
            for (; i < L; i++) w[i] = leak * w[i] + g * s[i];
        }
    }
}

} // namespace vibe
