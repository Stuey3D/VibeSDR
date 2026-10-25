// vibe_simd4.h — four floats at a time for the FT8/FT4 hot loops: NEON (ARMv7 and AArch64), SSE2, or plain C.
// VibeSDR (2026-10-25, the FT8 CPU work); not in upstream ft8_lib.
//
// ★★★ SAME ANSWER ON EVERY PATH. Each operation is the one the scalar C it replaces compiles to, lane by lane, so a
//     vector loop gives what the scalar loop gave — bit for bit — rather than "close". Two places that takes care:
//   • v4_madd(a, b, c) = a*b + c. Where the target HAS a fused multiply-add (AArch64; ARMv7 built -mfpu=neon-vfpv4)
//     the compiler contracts the scalar `c + a*b` into one, so the vector path fuses too; where it has none (SSE2,
//     plain ARMv7 NEON) it is a multiply then an add. Scalar code that needs the identical rounding calls v4s_madd.
//   • v4_div is a true IEEE divide on SSE2 (divps) and AArch64 (fdiv). ARMv7 NEON has no divide: there, and only
//     there, it is a reciprocal estimate refined twice (~2 ulp) — measured to change no decode (see v4_div below).
// ★ VIBE_FORCE_SCALAR builds the plain-C path on any machine: how the vector kernels are costed and checked.
// ★ No AVX tier, no runtime dispatch: SSE2 is the x86 floor (every x86-64, and the i386 package's -msse2), NEON the
//   ARM one — the machines this is for are the Pi 2 and the Atom, not the comfortable ones.
#ifndef _INCLUDE_VIBE_SIMD4_H_
#define _INCLUDE_VIBE_SIMD4_H_
#include <stdint.h>

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
#if defined(__aarch64__) && !defined(VIBE_FT8_RECIP_DIV)
static inline v4f v4_div(v4f a, v4f b) { return vdivq_f32(a, b); }
#elif defined(__aarch64__) || !defined(VIBE_FT8_EXACT_DIV)
/* ★★★ ARMv7: A RECIPROCAL, NOT FOUR DIVIDES (2026-10-25). ARMv7 NEON has no divide, and a VFP divide on a Cortex-A7
 *  (the Pi 2) takes ~15-18 cycles and blocks: belief propagation does 1,116 of them an iteration, about half its time
 *  there. So on ARMv7 only, a/b = a × (reciprocal estimate + two Newton steps) — within ~2 ulp of the divide. That is
 *  the ONE place this header is not bit-exact, and it is measured: on the five real recordings (2,800 decodes, 1/2/3
 *  passes) the decode lists are identical, and on 50,000 noisy codewords BP's outcome (bits and error count) is
 *  identical every time — a hard decision does not move for 2 ulp. -DVIBE_FT8_EXACT_DIV restores the four divides;
 *  -DVIBE_FT8_RECIP_DIV forces this path on AArch64 too (how it was measured on the Mac). */
static inline v4f v4_div(v4f a, v4f b)
{
    float32x4_t r = vrecpeq_f32(b);
    r = vmulq_f32(r, vrecpsq_f32(b, r));
    r = vmulq_f32(r, vrecpsq_f32(b, r));
    return vmulq_f32(a, r);
}
#else
static inline v4f v4_div(v4f a, v4f b)
{
    float x[4], y[4];
    vst1q_f32(x, a); vst1q_f32(y, b);
    x[0] /= y[0]; x[1] /= y[1]; x[2] /= y[2]; x[3] /= y[3];
    return vld1q_f32(x);
}
#endif
/* ★★ COMPARES ON THE BIT PATTERNS, NOT vcltq_f32 (2026-10-25). GCC's ARMv7 arm_neon.h, without -ffast-math, turns
 *  a float vector compare into four VFP compares and flag moves (ARMv7 NEON flushes denormals, so it is not IEEE) —
 *  the qemu instruction counter found BP's selects and hard decisions running scalar. For finite floats the bit
 *  patterns order the same way: a positive constant c is exceeded exactly when the signed int is larger; a value is
 *  below a NEGATIVE constant c exactly when its unsigned bits are larger than c's (more negative = bigger
 *  magnitude). Exact for every finite input; the LLRs here are never NaN past the first hard decision. */
/** lane = (a < c) ? t : f, for a negative constant c */
static inline v4f v4_sel_lt_negc(v4f a, float c, v4f t, v4f f)
{
    return vbslq_f32(vcgtq_u32(vreinterpretq_u32_f32(a), vreinterpretq_u32_f32(vdupq_n_f32(c))), t, f);
}
/** lane = (a > c) ? t : f, for a positive constant c */
static inline v4f v4_sel_gt_posc(v4f a, float c, v4f t, v4f f)
{
    return vbslq_f32(vcgtq_s32(vreinterpretq_s32_f32(a), vreinterpretq_s32_f32(vdupq_n_f32(c))), t, f);
}
/** bit i = lane i > 0 */
static inline unsigned v4_gt0_mask(v4f a)
{
    const uint32x4_t m = vcgtq_s32(vreinterpretq_s32_f32(a), vdupq_n_s32(0));
    const uint32_t bits[4] = { 1, 2, 4, 8 };
    const uint32x4_t b = vandq_u32(m, vld1q_u32(bits));
    uint32x2_t s = vorr_u32(vget_low_u32(b), vget_high_u32(b));
    return vget_lane_u32(s, 0) | vget_lane_u32(s, 1);
}
/** base[idx[l]] = lane l: four lane stores, no round trip through memory */
static inline void v4_scatter(float* base, const uint16_t* idx, v4f v)
{
    vst1q_lane_f32(base + idx[0], v, 0); vst1q_lane_f32(base + idx[1], v, 1);
    vst1q_lane_f32(base + idx[2], v, 2); vst1q_lane_f32(base + idx[3], v, 3);
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
static inline v4f v4_sel_lt_negc(v4f a, float c, v4f t, v4f f)
{
    const __m128 m = _mm_cmplt_ps(a, _mm_set1_ps(c));
    return _mm_or_ps(_mm_and_ps(m, t), _mm_andnot_ps(m, f));
}
static inline v4f v4_sel_gt_posc(v4f a, float c, v4f t, v4f f)
{
    const __m128 m = _mm_cmpgt_ps(a, _mm_set1_ps(c));
    return _mm_or_ps(_mm_and_ps(m, t), _mm_andnot_ps(m, f));
}
static inline unsigned v4_gt0_mask(v4f a) { return (unsigned)_mm_movemask_ps(_mm_cmpgt_ps(a, _mm_setzero_ps())); }
static inline void v4_scatter(float* base, const uint16_t* idx, v4f v)
{
    float o[4]; _mm_storeu_ps(o, v);
    base[idx[0]] = o[0]; base[idx[1]] = o[1]; base[idx[2]] = o[2]; base[idx[3]] = o[3];
}
#else
typedef struct { float v[4]; } v4f;
static inline v4f v4_load(const float* p) { v4f r; for (int i = 0; i < 4; ++i) r.v[i] = p[i]; return r; }
static inline void v4_store(float* p, v4f a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
static inline v4f v4_set1(float x) { v4f r; for (int i = 0; i < 4; ++i) r.v[i] = x; return r; }
static inline v4f v4_add(v4f a, v4f b) { for (int i = 0; i < 4; ++i) a.v[i] += b.v[i]; return a; }
static inline v4f v4_mul(v4f a, v4f b) { for (int i = 0; i < 4; ++i) a.v[i] *= b.v[i]; return a; }
static inline v4f v4_madd(v4f a, v4f b, v4f c) { for (int i = 0; i < 4; ++i) a.v[i] = v4s_madd(a.v[i], b.v[i], c.v[i]); return a; }
static inline v4f v4_div(v4f a, v4f b) { for (int i = 0; i < 4; ++i) a.v[i] /= b.v[i]; return a; }
static inline v4f v4_sel_lt_negc(v4f a, float c, v4f t, v4f f) { for (int i = 0; i < 4; ++i) t.v[i] = a.v[i] < c ? t.v[i] : f.v[i]; return t; }
static inline v4f v4_sel_gt_posc(v4f a, float c, v4f t, v4f f) { for (int i = 0; i < 4; ++i) t.v[i] = a.v[i] > c ? t.v[i] : f.v[i]; return t; }
static inline unsigned v4_gt0_mask(v4f a) { unsigned m = 0; for (int i = 0; i < 4; ++i) m |= (unsigned)(a.v[i] > 0) << i; return m; }
static inline void v4_scatter(float* base, const uint16_t* idx, v4f v) { for (int i = 0; i < 4; ++i) base[idx[i]] = v.v[i]; }
#endif

#endif // _INCLUDE_VIBE_SIMD4_H_
