// decoders/digimodes/q65_simd.h — four floats at a time for the Q65 (and FST4/MSK144) receivers' hot loops: NEON
// (ARMv7 and AArch64), SSE2 (the x86 floor), or plain C with the same lane structure. VIBE_FORCE_SCALAR builds the plain
// path anywhere. Every operation is the IEEE operation the scalar code does, lane by lane, and every reduction sums four
// lanes then (l0+l1)+(l2+l3) on every path, so the SIMD and scalar builds decode identically.
// ★ No fused multiply-add is used (NEON's vmla/vfma and a scalar a*b+c would round differently on some targets).
#pragma once
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#define Q65V_NEON 1
#elif (defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define Q65V_SSE2 1
#endif

namespace vibe {
namespace q65v {
#if Q65V_NEON
typedef float32x4_t V;
inline V ld(const float* p) { return vld1q_f32(p); }
inline void st(float* p, V v) { vst1q_f32(p, v); }
inline V set1(float x) { return vdupq_n_f32(x); }
inline V add(V a, V b) { return vaddq_f32(a, b); }
inline V sub(V a, V b) { return vsubq_f32(a, b); }
inline V mul(V a, V b) { return vmulq_f32(a, b); }
inline V vmax(V a, V b) { return vmaxq_f32(a, b); }
/** Four interleaved complex values → (re, im). */
inline void ld2(const float* p, V& re, V& im) { float32x4x2_t t = vld2q_f32(p); re = t.val[0]; im = t.val[1]; }
inline void st2(float* p, V re, V im) { float32x4x2_t t; t.val[0] = re; t.val[1] = im; vst2q_f32(p, t); }
/** Sixteen floats → four vectors of every fourth one (a = p0 p4 p8 p12, b = p1 p5 …), and back. */
inline void ld4(const float* p, V& a, V& b, V& c, V& d) { float32x4x4_t t = vld4q_f32(p); a = t.val[0]; b = t.val[1]; c = t.val[2]; d = t.val[3]; }
inline void st4(float* p, V a, V b, V c, V d) { float32x4x4_t t; t.val[0] = a; t.val[1] = b; t.val[2] = c; t.val[3] = d; vst4q_f32(p, t); }
#elif Q65V_SSE2
typedef __m128 V;
inline V ld(const float* p) { return _mm_loadu_ps(p); }
inline void st(float* p, V v) { _mm_storeu_ps(p, v); }
inline V set1(float x) { return _mm_set1_ps(x); }
inline V add(V a, V b) { return _mm_add_ps(a, b); }
inline V sub(V a, V b) { return _mm_sub_ps(a, b); }
inline V mul(V a, V b) { return _mm_mul_ps(a, b); }
inline V vmax(V a, V b) { return _mm_max_ps(a, b); }
inline void ld2(const float* p, V& re, V& im) {
    const __m128 a = _mm_loadu_ps(p), b = _mm_loadu_ps(p + 4);
    re = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)); im = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
}
inline void st2(float* p, V re, V im) { _mm_storeu_ps(p, _mm_unpacklo_ps(re, im)); _mm_storeu_ps(p + 4, _mm_unpackhi_ps(re, im)); }
inline void ld4(const float* p, V& a, V& b, V& c, V& d) {
    a = _mm_loadu_ps(p); b = _mm_loadu_ps(p + 4); c = _mm_loadu_ps(p + 8); d = _mm_loadu_ps(p + 12);
    _MM_TRANSPOSE4_PS(a, b, c, d);
}
inline void st4(float* p, V a, V b, V c, V d) {
    _MM_TRANSPOSE4_PS(a, b, c, d);
    _mm_storeu_ps(p, a); _mm_storeu_ps(p + 4, b); _mm_storeu_ps(p + 8, c); _mm_storeu_ps(p + 12, d);
}
#else
struct V { float v[4]; };
inline V ld(const float* p) { V r; for (int i = 0; i < 4; ++i) r.v[i] = p[i]; return r; }
inline void st(float* p, V a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
inline V set1(float x) { V r; for (int i = 0; i < 4; ++i) r.v[i] = x; return r; }
inline V add(V a, V b) { V r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] + b.v[i]; return r; }
inline V sub(V a, V b) { V r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] - b.v[i]; return r; }
inline V mul(V a, V b) { V r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i]; return r; }
inline V vmax(V a, V b) { V r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return r; }
inline void ld2(const float* p, V& re, V& im) { for (int i = 0; i < 4; ++i) { re.v[i] = p[2 * i]; im.v[i] = p[2 * i + 1]; } }
inline void st2(float* p, V re, V im) { for (int i = 0; i < 4; ++i) { p[2 * i] = re.v[i]; p[2 * i + 1] = im.v[i]; } }
inline void ld4(const float* p, V& a, V& b, V& c, V& d) { for (int i = 0; i < 4; ++i) { a.v[i] = p[4 * i]; b.v[i] = p[4 * i + 1]; c.v[i] = p[4 * i + 2]; d.v[i] = p[4 * i + 3]; } }
inline void st4(float* p, V a, V b, V c, V d) { for (int i = 0; i < 4; ++i) { p[4 * i] = a.v[i]; p[4 * i + 1] = b.v[i]; p[4 * i + 2] = c.v[i]; p[4 * i + 3] = d.v[i]; } }
#endif
/** The four lanes summed, (l0+l1)+(l2+l3). */
inline float hsum(V a) { float l[4]; st(l, a); return (l[0] + l[1]) + (l[2] + l[3]); }
} // namespace q65v
} // namespace vibe
