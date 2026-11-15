// vibe_vdl2_simd.h — V12 VDL2's SIMD kernels (NEON / SSE2 / scalar). Each lane does the scalar arithmetic in the same
// order, so the three builds decode the same frames (test-vdl2 on armhf NEON, armhf VIBE_FORCE_SCALAR, i686 SSE2).
#pragma once
#include "vibedsp/vibedsp.h"
#include "vibedsp/simd_internal.h"
#include <algorithm>
#include <cmath>

namespace vibevdl2 {
using cf32 = vibedsp::cf32;

/** Four lag-one-symbol products p = y·conj(ylag) and their magnitudes. */
inline void lagProd4(const cf32* y, const cf32* ylag, cf32* p, float* a) {
    const float* yf = reinterpret_cast<const float*>(y); const float* lf = reinterpret_cast<const float*>(ylag);
#if VIBE_NEON
    const float32x4x2_t v = vld2q_f32(yf), l = vld2q_f32(lf);
    float32x4x2_t o;
    o.val[0] = vaddq_f32(vmulq_f32(v.val[0], l.val[0]), vmulq_f32(v.val[1], l.val[1]));
    o.val[1] = vsubq_f32(vmulq_f32(v.val[1], l.val[0]), vmulq_f32(v.val[0], l.val[1]));
    vst2q_f32(reinterpret_cast<float*>(p), o);
    const float32x4_t q = vaddq_f32(vmulq_f32(o.val[0], o.val[0]), vmulq_f32(o.val[1], o.val[1]));
    float32x4_t r = vrsqrteq_f32(vmaxq_f32(q, vdupq_n_f32(1e-30f)));
    r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(q, r), r));
    r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(q, r), r));
    vst1q_f32(a, vmulq_f32(q, r));
#elif VIBE_SSE
    __m128 vr, vi, lr, li;
    vibedsp::sseLoad2(yf, vr, vi); vibedsp::sseLoad2(lf, lr, li);
    const __m128 orr = _mm_add_ps(_mm_mul_ps(vr, lr), _mm_mul_ps(vi, li)), oi = _mm_sub_ps(_mm_mul_ps(vi, lr), _mm_mul_ps(vr, li));
    vibedsp::sseStore2(reinterpret_cast<float*>(p), orr, oi);
    _mm_storeu_ps(a, _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(orr, orr), _mm_mul_ps(oi, oi))));
#else
    for (int k = 0; k < 4; ++k) {
        const float re = yf[2 * k] * lf[2 * k] + yf[2 * k + 1] * lf[2 * k + 1], im = yf[2 * k + 1] * lf[2 * k] - yf[2 * k] * lf[2 * k + 1];
        p[k] = cf32(re, im); a[k] = std::sqrt(re * re + im * im);
    }
#endif
}

/** The unique-word correlator for FOUR consecutive samples: D[l] = Σ_i c_i·P[g_i + l], S[l] = Σ_i A[g_i + l],
 *  M[l] = min_i A[g_i + l]; g_i = the 4-aligned ring index of lag i. */
inline void corr16x4(const cf32* P, const float* A, const int* g, const cf32* c, float* Dr, float* Di, float* S, float* M) {
#if VIBE_NEON
    float32x4_t dr = vdupq_n_f32(0), di = vdupq_n_f32(0), sv = vdupq_n_f32(0), mv = vdupq_n_f32(1e30f);
    for (int i = 0; i < 16; ++i) {
        const float32x4x2_t x = vld2q_f32(reinterpret_cast<const float*>(P + g[i]));
        const float32x4_t cr = vdupq_n_f32(c[i].real()), ci = vdupq_n_f32(c[i].imag());
        dr = vaddq_f32(dr, vsubq_f32(vmulq_f32(x.val[0], cr), vmulq_f32(x.val[1], ci)));
        di = vaddq_f32(di, vaddq_f32(vmulq_f32(x.val[0], ci), vmulq_f32(x.val[1], cr)));
        const float32x4_t av = vld1q_f32(A + g[i]);
        sv = vaddq_f32(sv, av); mv = vminq_f32(mv, av);
    }
    vst1q_f32(Dr, dr); vst1q_f32(Di, di); vst1q_f32(S, sv); vst1q_f32(M, mv);
#elif VIBE_SSE
    __m128 dr = _mm_setzero_ps(), di = _mm_setzero_ps(), sv = _mm_setzero_ps(), mv = _mm_set1_ps(1e30f);
    for (int i = 0; i < 16; ++i) {
        __m128 xr, xi; vibedsp::sseLoad2(reinterpret_cast<const float*>(P + g[i]), xr, xi);
        const __m128 cr = _mm_set1_ps(c[i].real()), ci = _mm_set1_ps(c[i].imag());
        dr = _mm_add_ps(dr, _mm_sub_ps(_mm_mul_ps(xr, cr), _mm_mul_ps(xi, ci)));
        di = _mm_add_ps(di, _mm_add_ps(_mm_mul_ps(xr, ci), _mm_mul_ps(xi, cr)));
        const __m128 av = _mm_loadu_ps(A + g[i]);
        sv = _mm_add_ps(sv, av); mv = _mm_min_ps(mv, av);
    }
    _mm_storeu_ps(Dr, dr); _mm_storeu_ps(Di, di); _mm_storeu_ps(S, sv); _mm_storeu_ps(M, mv);
#else
    for (int l = 0; l < 4; ++l) {
        float dr = 0, di = 0, sv = 0, mv = 1e30f;
        for (int i = 0; i < 16; ++i) {
            const cf32 x = P[g[i] + l];
            dr += x.real() * c[i].real() - x.imag() * c[i].imag(); di += x.real() * c[i].imag() + x.imag() * c[i].real();
            sv += A[g[i] + l]; mv = std::min(mv, A[g[i] + l]);
        }
        Dr[l] = dr; Di[l] = di; S[l] = sv; M[l] = mv;
    }
#endif
}

/** Σ t[j]·z[j], j < 12 (real taps, complex samples): the polyphase resampler's dot product. */
inline cf32 dot12(const float* t, const cf32* z) {
    const float* zf = reinterpret_cast<const float*>(z);
#if VIBE_NEON
    const float32x4x2_t a = vld2q_f32(zf), b = vld2q_f32(zf + 8), c = vld2q_f32(zf + 16);
    const float32x4_t t0 = vld1q_f32(t), t1 = vld1q_f32(t + 4), t2 = vld1q_f32(t + 8);
    float32x4_t re = vmulq_f32(t0, a.val[0]), im = vmulq_f32(t0, a.val[1]);
    re = vaddq_f32(re, vmulq_f32(t1, b.val[0])); im = vaddq_f32(im, vmulq_f32(t1, b.val[1]));
    re = vaddq_f32(re, vmulq_f32(t2, c.val[0])); im = vaddq_f32(im, vmulq_f32(t2, c.val[1]));
    const float32x2_t p = vpadd_f32(vadd_f32(vget_low_f32(re), vget_high_f32(re)), vadd_f32(vget_low_f32(im), vget_high_f32(im)));
    return cf32(vget_lane_f32(p, 0), vget_lane_f32(p, 1));
#elif VIBE_SSE
    __m128 ar, ai, br, bi, cr, ci;
    vibedsp::sseLoad2(zf, ar, ai); vibedsp::sseLoad2(zf + 8, br, bi); vibedsp::sseLoad2(zf + 16, cr, ci);
    const __m128 t0 = _mm_loadu_ps(t), t1 = _mm_loadu_ps(t + 4), t2 = _mm_loadu_ps(t + 8);
    __m128 re = _mm_mul_ps(t0, ar), im = _mm_mul_ps(t0, ai);
    re = _mm_add_ps(re, _mm_mul_ps(t1, br)); im = _mm_add_ps(im, _mm_mul_ps(t1, bi));
    re = _mm_add_ps(re, _mm_mul_ps(t2, cr)); im = _mm_add_ps(im, _mm_mul_ps(t2, ci));
    alignas(16) float r[4], i[4]; _mm_store_ps(r, re); _mm_store_ps(i, im);
    return cf32((r[0] + r[2]) + (r[1] + r[3]), (i[0] + i[2]) + (i[1] + i[3]));
#else
    float r[4], i[4];
    for (int l = 0; l < 4; ++l) {
        r[l] = t[l] * zf[2 * l]; i[l] = t[l] * zf[2 * l + 1];
        r[l] += t[l + 4] * zf[2 * l + 8]; i[l] += t[l + 4] * zf[2 * l + 9];
        r[l] += t[l + 8] * zf[2 * l + 16]; i[l] += t[l + 8] * zf[2 * l + 17];
    }
    return cf32((r[0] + r[2]) + (r[1] + r[3]), (i[0] + i[2]) + (i[1] + i[3]));
#endif
}
}  // namespace vibevdl2
