// vibe_sonde_disc.h — V12 radiosondes: the SIMD kernels every sonde demodulator shares (moved out of vibe_rs41_rx.h when
// the DFM / M10 / M20 joined, docs/v12/SONDES-TYPES.md): the FM discriminator (a vector atan2, NEON / SSE2 / scalar)
// and the scan's smoothed power per bin. Unchanged code — the RS41's tests and the SIMD audit cover it.
#pragma once
#include "vibedsp/vibedsp.h"
#include "vibedsp/simd_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vibers41 {

using cf32 = vibedsp::cf32;

// ── SIMD kernels ─────────────────────────────────────────────────────────────────────────────────────────────────────

/** The scalar reference of the vector atan2 below: Abramowitz & Stegun 4.4.49 on [0, 1] (|error| ≤ 1e-5 rad), folded
 *  to the whole circle. */
inline float atan2Fast(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    const float a = mn / (mx + 1e-30f), s = a * a;
    const float r0 = ((((0.0208351f * s - 0.0851330f) * s + 0.1801410f) * s - 0.3302995f) * s + 0.9998660f) * a;
    float r = r0;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

/** out[k] = arg(z[k] · conj(z[k−1])), z[−1] = `prev` (updated). The FM discriminator, rad per sample. `tmp` scratch. */
inline void fmDisc(const cf32* z, int n, cf32& prev, float* out, std::vector<cf32>& tmp) {
    if (n <= 0) return;
    tmp.resize(size_t(n) + 1);
    tmp[0] = prev;
    std::memcpy(tmp.data() + 1, z, size_t(n) * sizeof(cf32));
    prev = z[n - 1];
    const float* cf = reinterpret_cast<const float*>(tmp.data() + 1);   // current
    const float* pf = reinterpret_cast<const float*>(tmp.data());       // previous
    int k = 0;
#if VIBE_NEON
    const float32x4_t c1 = vdupq_n_f32(0.0208351f), c2 = vdupq_n_f32(-0.0851330f), c3 = vdupq_n_f32(0.1801410f),
                      c4 = vdupq_n_f32(-0.3302995f), c5 = vdupq_n_f32(0.9998660f), hp = vdupq_n_f32(1.57079637f),
                      pi = vdupq_n_f32(3.14159274f), tiny = vdupq_n_f32(1e-30f), zero = vdupq_n_f32(0.f);
    for (; k + 4 <= n; k += 4) {
        const float32x4x2_t c = vld2q_f32(cf + 2 * k), p = vld2q_f32(pf + 2 * k);
        const float32x4_t re = vaddq_f32(vmulq_f32(c.val[0], p.val[0]), vmulq_f32(c.val[1], p.val[1]));
        const float32x4_t im = vsubq_f32(vmulq_f32(c.val[1], p.val[0]), vmulq_f32(c.val[0], p.val[1]));
        const float32x4_t ax = vabsq_f32(re), ay = vabsq_f32(im);
        const float32x4_t mx = vaddq_f32(vmaxq_f32(ax, ay), tiny), mn = vminq_f32(ax, ay);
        float32x4_t inv = vrecpeq_f32(mx);
        inv = vmulq_f32(inv, vrecpsq_f32(mx, inv));
        inv = vmulq_f32(inv, vrecpsq_f32(mx, inv));
        const float32x4_t a = vmulq_f32(mn, inv), s = vmulq_f32(a, a);
        float32x4_t r = vaddq_f32(vmulq_f32(c1, s), c2);
        r = vaddq_f32(vmulq_f32(r, s), c3);
        r = vaddq_f32(vmulq_f32(r, s), c4);
        r = vaddq_f32(vmulq_f32(r, s), c5);
        r = vmulq_f32(r, a);
        r = vbslq_f32(vcgtq_f32(ay, ax), vsubq_f32(hp, r), r);
        r = vbslq_f32(vcltq_f32(re, zero), vsubq_f32(pi, r), r);
        r = vbslq_f32(vcltq_f32(im, zero), vnegq_f32(r), r);
        vst1q_f32(out + k, r);
    }
#elif VIBE_SSE
    const __m128 c1 = _mm_set1_ps(0.0208351f), c2 = _mm_set1_ps(-0.0851330f), c3 = _mm_set1_ps(0.1801410f),
                 c4 = _mm_set1_ps(-0.3302995f), c5 = _mm_set1_ps(0.9998660f), hp = _mm_set1_ps(1.57079637f),
                 pi = _mm_set1_ps(3.14159274f), tiny = _mm_set1_ps(1e-30f), zero = _mm_setzero_ps(),
                 absm = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), sgn = _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u)));
    for (; k + 4 <= n; k += 4) {
        __m128 cr, ci, pr, pim;
        vibedsp::sseLoad2(cf + 2 * k, cr, ci); vibedsp::sseLoad2(pf + 2 * k, pr, pim);
        const __m128 re = _mm_add_ps(_mm_mul_ps(cr, pr), _mm_mul_ps(ci, pim));
        const __m128 im = _mm_sub_ps(_mm_mul_ps(ci, pr), _mm_mul_ps(cr, pim));
        const __m128 ax = _mm_and_ps(re, absm), ay = _mm_and_ps(im, absm);
        const __m128 a = _mm_div_ps(_mm_min_ps(ax, ay), _mm_add_ps(_mm_max_ps(ax, ay), tiny)), s = _mm_mul_ps(a, a);
        __m128 r = _mm_add_ps(_mm_mul_ps(c1, s), c2);
        r = _mm_add_ps(_mm_mul_ps(r, s), c3);
        r = _mm_add_ps(_mm_mul_ps(r, s), c4);
        r = _mm_add_ps(_mm_mul_ps(r, s), c5);
        r = _mm_mul_ps(r, a);
        __m128 m = _mm_cmpgt_ps(ay, ax);
        r = _mm_or_ps(_mm_and_ps(m, _mm_sub_ps(hp, r)), _mm_andnot_ps(m, r));
        m = _mm_cmplt_ps(re, zero);
        r = _mm_or_ps(_mm_and_ps(m, _mm_sub_ps(pi, r)), _mm_andnot_ps(m, r));
        r = _mm_xor_ps(r, _mm_and_ps(im, sgn));
        _mm_storeu_ps(out + k, r);
    }
#endif
    for (; k < n; ++k) {
        const float re = cf[2 * k] * pf[2 * k] + cf[2 * k + 1] * pf[2 * k + 1];
        const float im = cf[2 * k + 1] * pf[2 * k] - cf[2 * k] * pf[2 * k + 1];
        out[k] = atan2Fast(im, re);
    }
}

/** acc[k] += a · (|b[k]|² − acc[k]) — the scan's smoothed power per bin. NEON / SSE2 / scalar. */
inline void powerEma(const cf32* b, float* acc, int n, float a) {
    const float* bf = reinterpret_cast<const float*>(b);
    int k = 0;
#if VIBE_NEON
    const float32x4_t av = vdupq_n_f32(a);
    for (; k + 4 <= n; k += 4) {
        const float32x4x2_t x = vld2q_f32(bf + 2 * k);
        const float32x4_t p = vaddq_f32(vmulq_f32(x.val[0], x.val[0]), vmulq_f32(x.val[1], x.val[1]));
        const float32x4_t q = vld1q_f32(acc + k);
        vst1q_f32(acc + k, vaddq_f32(q, vmulq_f32(av, vsubq_f32(p, q))));
    }
#elif VIBE_SSE
    const __m128 av = _mm_set1_ps(a);
    for (; k + 4 <= n; k += 4) {
        __m128 xr, xi; vibedsp::sseLoad2(bf + 2 * k, xr, xi);
        const __m128 p = _mm_add_ps(_mm_mul_ps(xr, xr), _mm_mul_ps(xi, xi));
        const __m128 q = _mm_loadu_ps(acc + k);
        _mm_storeu_ps(acc + k, _mm_add_ps(q, _mm_mul_ps(av, _mm_sub_ps(p, q))));
    }
#endif
    for (; k < n; ++k) { const float p = bf[2 * k] * bf[2 * k] + bf[2 * k + 1] * bf[2 * k + 1]; acc[k] += a * (p - acc[k]); }
}

}  // namespace vibers41
