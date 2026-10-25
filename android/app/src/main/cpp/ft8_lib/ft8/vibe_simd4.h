// vibe_simd4.h — four floats at a time for the FT8/FT4 hot loops: NEON (ARMv7 and AArch64), SSE2, or plain C.
// VibeSDR (2026-10-25, the FT8 CPU work); not in upstream ft8_lib.
//
// ★★★ SAME ANSWER ON EVERY PATH. Each operation is the one the scalar C it replaces compiles to, lane by lane, so a
//     vector loop gives what the scalar loop gave — bit for bit — rather than "close". Two places that takes care:
//   • v4_madd(a, b, c) = a*b + c. Where the target HAS a fused multiply-add (AArch64; ARMv7 built -mfpu=neon-vfpv4)
//     the compiler contracts the scalar `c + a*b` into one, so the vector path fuses too; where it has none (SSE2,
//     plain ARMv7 NEON) it is a multiply then an add. Scalar code that needs the identical rounding calls v4s_madd.
//   • v4_div is a true IEEE divide: divps on SSE2, fdiv on AArch64, and on ARMv7 — whose NEON has no divide — four
//     VFP divides, NOT the reciprocal estimate (that is ~1 ulp out, and an LLR one ulp out is a different decode).
// ★ VIBE_FORCE_SCALAR builds the plain-C path on any machine: how the vector kernels are costed and checked.
// ★ No AVX tier, no runtime dispatch: SSE2 is the x86 floor (every x86-64, and the i386 package's -msse2), NEON the
//   ARM one — the machines this is for are the Pi 2 and the Atom, not the comfortable ones.
#ifndef _INCLUDE_VIBE_SIMD4_H_
#define _INCLUDE_VIBE_SIMD4_H_

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#define VIBE_FT8_NEON 1
#elif (defined(__SSE2__) || defined(_M_X64)) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define VIBE_FT8_SSE2 1
#endif

#if defined(__aarch64__) || defined(__ARM_FEATURE_FMA) || defined(__FMA__)
#define VIBE_FT8_HAS_FMA 1
#endif

/** The scalar a*b + c, rounded exactly as the vector v4_madd rounds it. */
static inline float v4s_madd(float a, float b, float c)
{
#if VIBE_FT8_HAS_FMA
    return __builtin_fmaf(a, b, c);
#else
    return a * b + c;
#endif
}

#if VIBE_FT8_NEON
typedef float32x4_t v4f;
static inline v4f v4_load(const float* p) { return vld1q_f32(p); }
static inline void v4_store(float* p, v4f v) { vst1q_f32(p, v); }
static inline v4f v4_set1(float x) { return vdupq_n_f32(x); }
static inline v4f v4_add(v4f a, v4f b) { return vaddq_f32(a, b); }
static inline v4f v4_mul(v4f a, v4f b) { return vmulq_f32(a, b); }
#if VIBE_FT8_HAS_FMA
static inline v4f v4_madd(v4f a, v4f b, v4f c) { return vfmaq_f32(c, a, b); }
#else
static inline v4f v4_madd(v4f a, v4f b, v4f c) { return vaddq_f32(vmulq_f32(a, b), c); }
#endif
#if defined(__aarch64__)
static inline v4f v4_div(v4f a, v4f b) { return vdivq_f32(a, b); }
#else
static inline v4f v4_div(v4f a, v4f b)
{
    float x[4], y[4];
    vst1q_f32(x, a); vst1q_f32(y, b);
    x[0] /= y[0]; x[1] /= y[1]; x[2] /= y[2]; x[3] /= y[3];
    return vld1q_f32(x);
}
#endif
/** lane = (a < b) ? t : f */
static inline v4f v4_sel_lt(v4f a, v4f b, v4f t, v4f f) { return vbslq_f32(vcltq_f32(a, b), t, f); }
static inline v4f v4_sel_gt(v4f a, v4f b, v4f t, v4f f) { return vbslq_f32(vcgtq_f32(a, b), t, f); }
/** bit i = lane i > 0 */
static inline unsigned v4_gt0_mask(v4f a)
{
    const uint32x4_t m = vcgtq_f32(a, vdupq_n_f32(0.0f));
    const uint32_t bits[4] = { 1, 2, 4, 8 };
    const uint32x4_t b = vandq_u32(m, vld1q_u32(bits));
    uint32x2_t s = vorr_u32(vget_low_u32(b), vget_high_u32(b));
    return vget_lane_u32(s, 0) | vget_lane_u32(s, 1);
}
#elif VIBE_FT8_SSE2
typedef __m128 v4f;
static inline v4f v4_load(const float* p) { return _mm_loadu_ps(p); }
static inline void v4_store(float* p, v4f v) { _mm_storeu_ps(p, v); }
static inline v4f v4_set1(float x) { return _mm_set1_ps(x); }
static inline v4f v4_add(v4f a, v4f b) { return _mm_add_ps(a, b); }
static inline v4f v4_mul(v4f a, v4f b) { return _mm_mul_ps(a, b); }
static inline v4f v4_madd(v4f a, v4f b, v4f c)
{
#if VIBE_FT8_HAS_FMA
    // an x86 build with FMA on (-mfma): the scalar side fuses, so this must too — scalar per lane keeps it exact
    float x[4], y[4], z[4];
    _mm_storeu_ps(x, a); _mm_storeu_ps(y, b); _mm_storeu_ps(z, c);
    for (int i = 0; i < 4; ++i) x[i] = __builtin_fmaf(x[i], y[i], z[i]);
    return _mm_loadu_ps(x);
#else
    return _mm_add_ps(_mm_mul_ps(a, b), c);
#endif
}
static inline v4f v4_div(v4f a, v4f b) { return _mm_div_ps(a, b); }
static inline v4f v4_sel_lt(v4f a, v4f b, v4f t, v4f f)
{
    const __m128 m = _mm_cmplt_ps(a, b);
    return _mm_or_ps(_mm_and_ps(m, t), _mm_andnot_ps(m, f));
}
static inline v4f v4_sel_gt(v4f a, v4f b, v4f t, v4f f)
{
    const __m128 m = _mm_cmpgt_ps(a, b);
    return _mm_or_ps(_mm_and_ps(m, t), _mm_andnot_ps(m, f));
}
static inline unsigned v4_gt0_mask(v4f a) { return (unsigned)_mm_movemask_ps(_mm_cmpgt_ps(a, _mm_setzero_ps())); }
#else
typedef struct { float v[4]; } v4f;
static inline v4f v4_load(const float* p) { v4f r; for (int i = 0; i < 4; ++i) r.v[i] = p[i]; return r; }
static inline void v4_store(float* p, v4f a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
static inline v4f v4_set1(float x) { v4f r; for (int i = 0; i < 4; ++i) r.v[i] = x; return r; }
static inline v4f v4_add(v4f a, v4f b) { for (int i = 0; i < 4; ++i) a.v[i] += b.v[i]; return a; }
static inline v4f v4_mul(v4f a, v4f b) { for (int i = 0; i < 4; ++i) a.v[i] *= b.v[i]; return a; }
static inline v4f v4_madd(v4f a, v4f b, v4f c) { for (int i = 0; i < 4; ++i) a.v[i] = v4s_madd(a.v[i], b.v[i], c.v[i]); return a; }
static inline v4f v4_div(v4f a, v4f b) { for (int i = 0; i < 4; ++i) a.v[i] /= b.v[i]; return a; }
static inline v4f v4_sel_lt(v4f a, v4f b, v4f t, v4f f) { for (int i = 0; i < 4; ++i) t.v[i] = a.v[i] < b.v[i] ? t.v[i] : f.v[i]; return t; }
static inline v4f v4_sel_gt(v4f a, v4f b, v4f t, v4f f) { for (int i = 0; i < 4; ++i) t.v[i] = a.v[i] > b.v[i] ? t.v[i] : f.v[i]; return t; }
static inline unsigned v4_gt0_mask(v4f a) { unsigned m = 0; for (int i = 0; i < 4; ++i) m |= (unsigned)(a.v[i] > 0) << i; return m; }
#endif

#endif // _INCLUDE_VIBE_SIMD4_H_
