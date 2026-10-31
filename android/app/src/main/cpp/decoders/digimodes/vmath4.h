// decoders/digimodes/vmath4.h — the transcendental and bit-level operations the small-LDPC belief propagation needs
// (ldpc_small.cpp), four lanes at a time, on top of q65_simd.h's lanes: NEON (ARMv7 and AArch64), SSE2, or plain C with
// the same lane structure (VIBE_FORCE_SCALAR). V12, 2026-10-31.
//
// ★★ IDENTICAL ON EVERY PATH. exp and log are our own polynomials (fitted in scratch Python to the true functions:
//    2^f on (−1, 0] to 1.4e-9, ln(1+t)/t on [√½−1, √2−1] to 6e-8) built only from IEEE add, multiply, convert and bit
//    operations, evaluated in the same order on every path — so NEON, SSE2 and the scalar build give the same bits, and
//    the decoders built on them decode identically. No fused multiply-add (each product is its own statement).
// ★ ARMv7 NEON has no divide: div() is four VFP divides there (exact IEEE, as SSE2's and AArch64's). It is the one
//   per-lane operation; the belief propagation needs one a message.
// ★ GCC armhf note (the v12-decoder-simd-audit finding): GCC lowers NEON float COMPARES (vcgtq_f32) to scalar code
//   unless `vibedsp/neon_compat.h` is used — that header is on branch v12-decoder-simd-audit, not here yet. The only
//   compare here is gtMask() (log's mantissa fold); when the header lands, route it through it.
#pragma once
#include "q65_simd.h"

#include <cstdint>
#include <cstring>

namespace vibe {
namespace q65v {

#if Q65V_NEON
inline V vmin(V a, V b) { return vminq_f32(a, b); }
inline V vabs(V a) { return vabsq_f32(a); }
inline V div(V a, V b) {
#if defined(__aarch64__)
    return vdivq_f32(a, b);
#else
    float x[4], y[4]; vst1q_f32(x, a); vst1q_f32(y, b);
    for (int i = 0; i < 4; ++i) x[i] = x[i] / y[i];
    return vld1q_f32(x);
#endif
}
/** The sign bit of `s` given to |m| (m ≥ 0). */
inline V withSign(V m, V s) {
    const uint32x4_t sb = vandq_u32(vreinterpretq_u32_f32(s), vdupq_n_u32(0x80000000u));
    return vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(m), sb));
}
/** e^x for x in [−87, 0]. */
inline V expNeg(V x) {
    const V y = vmulq_f32(x, vdupq_n_f32(1.44269504f));
    const int32x4_t n = vcvtq_s32_f32(y);                       // toward zero: n ≤ 0
    const V f = vsubq_f32(y, vcvtq_f32_s32(n));                // (−1, 0]
    V p = vdupq_n_f32(1.093875238e-04f);
    p = vaddq_f32(vmulq_f32(p, f), vdupq_n_f32(1.275716217e-03f));
    p = vaddq_f32(vmulq_f32(p, f), vdupq_n_f32(9.580058453e-03f));
    p = vaddq_f32(vmulq_f32(p, f), vdupq_n_f32(5.549103528e-02f));
    p = vaddq_f32(vmulq_f32(p, f), vdupq_n_f32(2.402243555e-01f));
    p = vaddq_f32(vmulq_f32(p, f), vdupq_n_f32(6.931470473e-01f));
    p = vaddq_f32(vmulq_f32(p, f), vdupq_n_f32(9.999999986e-01f));
    const int32x4_t e = vshlq_n_s32(vaddq_s32(n, vdupq_n_s32(127)), 23);
    return vmulq_f32(p, vreinterpretq_f32_s32(e));
}
/** ln x for x normal and > 0. */
inline V vlog(V x) {
    const int32x4_t b = vreinterpretq_s32_f32(x);
    int32x4_t e = vsubq_s32(vshrq_n_s32(b, 23), vdupq_n_s32(127));
    V m = vreinterpretq_f32_s32(vorrq_s32(vandq_s32(b, vdupq_n_s32(0x007FFFFF)), vdupq_n_s32(0x3F800000)));
    const uint32x4_t big = vcgtq_f32(m, vdupq_n_f32(1.41421356f));   // ★ neon_compat.h when it lands (file note)
    m = vbslq_f32(big, vmulq_f32(m, vdupq_n_f32(0.5f)), m);
    e = vsubq_s32(e, vreinterpretq_s32_u32(big));                    // big lanes are all-ones = −1
    const V t = vsubq_f32(m, vdupq_n_f32(1.0f));
    V p = vdupq_n_f32(-1.013405675e-01f);
    p = vaddq_f32(vmulq_f32(p, t), vdupq_n_f32(1.623418980e-01f));
    p = vaddq_f32(vmulq_f32(p, t), vdupq_n_f32(-1.724701330e-01f));
    p = vaddq_f32(vmulq_f32(p, t), vdupq_n_f32(1.989856505e-01f));
    p = vaddq_f32(vmulq_f32(p, t), vdupq_n_f32(-2.497007169e-01f));
    p = vaddq_f32(vmulq_f32(p, t), vdupq_n_f32(3.333511414e-01f));
    p = vaddq_f32(vmulq_f32(p, t), vdupq_n_f32(-5.000036309e-01f));
    p = vaddq_f32(vmulq_f32(p, t), vdupq_n_f32(9.999999425e-01f));
    const V lm = vmulq_f32(p, t);
    return vaddq_f32(lm, vmulq_f32(vcvtq_f32_s32(e), vdupq_n_f32(0.693147181f)));
}
#elif Q65V_SSE2
inline V vmin(V a, V b) { return _mm_min_ps(a, b); }
inline V vabs(V a) { return _mm_andnot_ps(_mm_set1_ps(-0.0f), a); }
inline V div(V a, V b) { return _mm_div_ps(a, b); }
inline V withSign(V m, V s) { return _mm_or_ps(m, _mm_and_ps(s, _mm_set1_ps(-0.0f))); }
inline V expNeg(V x) {
    const V y = _mm_mul_ps(x, _mm_set1_ps(1.44269504f));
    const __m128i n = _mm_cvttps_epi32(y);
    const V f = _mm_sub_ps(y, _mm_cvtepi32_ps(n));
    V p = _mm_set1_ps(1.093875238e-04f);
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(1.275716217e-03f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(9.580058453e-03f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(5.549103528e-02f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(2.402243555e-01f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(6.931470473e-01f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(9.999999986e-01f));
    const __m128i e = _mm_slli_epi32(_mm_add_epi32(n, _mm_set1_epi32(127)), 23);
    return _mm_mul_ps(p, _mm_castsi128_ps(e));
}
inline V vlog(V x) {
    const __m128i b = _mm_castps_si128(x);
    __m128i e = _mm_sub_epi32(_mm_srli_epi32(b, 23), _mm_set1_epi32(127));
    V m = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(b, _mm_set1_epi32(0x007FFFFF)), _mm_set1_epi32(0x3F800000)));
    const V big = _mm_cmpgt_ps(m, _mm_set1_ps(1.41421356f));
    m = _mm_or_ps(_mm_and_ps(big, _mm_mul_ps(m, _mm_set1_ps(0.5f))), _mm_andnot_ps(big, m));
    e = _mm_sub_epi32(e, _mm_castps_si128(big));
    const V t = _mm_sub_ps(m, _mm_set1_ps(1.0f));
    V p = _mm_set1_ps(-1.013405675e-01f);
    p = _mm_add_ps(_mm_mul_ps(p, t), _mm_set1_ps(1.623418980e-01f));
    p = _mm_add_ps(_mm_mul_ps(p, t), _mm_set1_ps(-1.724701330e-01f));
    p = _mm_add_ps(_mm_mul_ps(p, t), _mm_set1_ps(1.989856505e-01f));
    p = _mm_add_ps(_mm_mul_ps(p, t), _mm_set1_ps(-2.497007169e-01f));
    p = _mm_add_ps(_mm_mul_ps(p, t), _mm_set1_ps(3.333511414e-01f));
    p = _mm_add_ps(_mm_mul_ps(p, t), _mm_set1_ps(-5.000036309e-01f));
    p = _mm_add_ps(_mm_mul_ps(p, t), _mm_set1_ps(9.999999425e-01f));
    const V lm = _mm_mul_ps(p, t);
    return _mm_add_ps(lm, _mm_mul_ps(_mm_cvtepi32_ps(e), _mm_set1_ps(0.693147181f)));
}
#else
inline V vmin(V a, V b) { V r; for (int i = 0; i < 4; ++i) r.v[i] = b.v[i] < a.v[i] ? b.v[i] : a.v[i]; return r; }
inline V vabs(V a) { V r; for (int i = 0; i < 4; ++i) { uint32_t u; std::memcpy(&u, &a.v[i], 4); u &= 0x7FFFFFFFu; std::memcpy(&r.v[i], &u, 4); } return r; }
inline V div(V a, V b) { V r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] / b.v[i]; return r; }
inline V withSign(V m, V s) {
    V r;
    for (int i = 0; i < 4; ++i) { uint32_t a, b; std::memcpy(&a, &m.v[i], 4); std::memcpy(&b, &s.v[i], 4); a |= b & 0x80000000u; std::memcpy(&r.v[i], &a, 4); }
    return r;
}
inline float expNeg1(float x) {
    const float y = x * 1.44269504f;
    const int32_t n = (int32_t)y;
    const float f = y - (float)n;
    float p = 1.093875238e-04f, q;
    q = p * f; p = q + 1.275716217e-03f;
    q = p * f; p = q + 9.580058453e-03f;
    q = p * f; p = q + 5.549103528e-02f;
    q = p * f; p = q + 2.402243555e-01f;
    q = p * f; p = q + 6.931470473e-01f;
    q = p * f; p = q + 9.999999986e-01f;
    const int32_t eb = (n + 127) << 23;
    float s; std::memcpy(&s, &eb, 4);
    return p * s;
}
inline float vlog1(float x) {
    int32_t b; std::memcpy(&b, &x, 4);
    int32_t e = (b >> 23) - 127;
    const int32_t mb = (b & 0x007FFFFF) | 0x3F800000;
    float m; std::memcpy(&m, &mb, 4);
    if (m > 1.41421356f) { m = m * 0.5f; e += 1; }
    const float t = m - 1.0f;
    float p = -1.013405675e-01f, q;
    q = p * t; p = q + 1.623418980e-01f;
    q = p * t; p = q + -1.724701330e-01f;
    q = p * t; p = q + 1.989856505e-01f;
    q = p * t; p = q + -2.497007169e-01f;
    q = p * t; p = q + 3.333511414e-01f;
    q = p * t; p = q + -5.000036309e-01f;
    q = p * t; p = q + 9.999999425e-01f;
    const float lm = p * t;
    const float le = (float)e * 0.693147181f;
    return lm + le;
}
inline V expNeg(V x) { V r; for (int i = 0; i < 4; ++i) r.v[i] = expNeg1(x.v[i]); return r; }
inline V vlog(V x) { V r; for (int i = 0; i < 4; ++i) r.v[i] = vlog1(x.v[i]); return r; }
#endif

} // namespace q65v
} // namespace vibe
