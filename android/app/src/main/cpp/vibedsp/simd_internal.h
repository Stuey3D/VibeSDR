// VibeSDR V5 — internal SIMD + fast-math kernels (NOT a public API).
//
// One home for the vectorised inner loops and fast approximations the engine's
// hot paths share. ARM NEON (AArch64) with scalar fallback; all GPL-free,
// original VibeSDR code except where a well-known public-domain approximation is
// noted. Accuracy of every approximation here is verified by the host test
// suite (FFT power, demod tones, WFM stereo separation, RDS).
#pragma once
#include "vibedsp.h"
#include <cmath>
#include <cstdint>

// ★★ VIBE_FORCE_SCALAR builds the fallback path on a machine that HAS NEON — the only way to
//    measure what the vector kernels are worth, and to prove the scalar path still computes the
//    same answers. It is also the path an x86 build would take, so this is how that port is costed
//    without owning an x86 box (2026-08-20).
#if defined(__aarch64__) && !defined(VIBE_FORCE_SCALAR)
  #include <arm_neon.h>
  #define VIBE_NEON 1
#endif

// ★★★ SSE2 ON x86-64, AND IT NEEDS NO RUNTIME CHECK, NO FLAG AND NO FAT BINARY.
//     SSE2 is part of the x86-64 ABI — every 64-bit x86 processor ever made has it, back to the
//     2003 Athlon 64 and including the first-generation Core i-series. That is the whole reason to
//     target it rather than AVX: one code path, always taken, no dispatch, and it reaches the
//     "right ancient machines" Stuart wants VibeServer to run on (2026-08-20).
//
// ★★ WHY IT IS WORTH DOING AT ALL. x86 shipped on the SCALAR path, which is correct and was fast
//    enough to measure well (12.8% of a core for WFM stereo at 1.92 MSPS on an i5-11300H). But the
//    first field report put a number on the other end of the range: ff-mish measured ~40% of a
//    thread on an i7-10750H with TURBO DISABLED (GitHub #21, 2026-08-21). Four-wide float is the
//    single biggest lever available, and it costs nothing at runtime.
//
// ★★ THE NEON BODIES BELOW ARE NOT TOUCHED. ARM is the shipping platform and its kernels are tuned
//    and proven; a "tidy" shared abstraction would put every Pi and every phone at risk to save
//    some duplication in a 200-line header. The SSE branches sit alongside, and the SCALAR path
//    remains the reference both are checked against (VIBE_FORCE_SCALAR builds it anywhere).
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(VIBE_FORCE_SCALAR)
  #include <emmintrin.h>          // SSE2
  #define VIBE_SSE 1
#endif

namespace vibedsp {

#if VIBE_SSE
// ── SSE2 helpers for the three NEON idioms x86 has no single instruction for ──
// ★ Written once here rather than inline in six kernels: a hand-rolled shuffle is exactly the kind
//   of thing that is right five times and subtly wrong the sixth.

/** Horizontal sum of the four lanes — NEON's vaddvq_f32. */
static inline float sseAddv(__m128 v) {
    // ★ movehl + shuffle, NOT haddps: haddps is SSE3, and the entire point of this file is that
    //   nothing here may exclude an older machine.
    __m128 t = _mm_add_ps(v, _mm_movehl_ps(v, v));          // [a0+a2, a1+a3, …]
    t = _mm_add_ss(t, _mm_shuffle_ps(t, t, _MM_SHUFFLE(1, 1, 1, 1)));
    return _mm_cvtss_f32(t);
}

/** De-interleave 8 consecutive floats into even (real) and odd (imag) lanes — NEON's vld2q_f32. */
static inline void sseLoad2(const float* p, __m128& re, __m128& im) {
    const __m128 a = _mm_loadu_ps(p);        // r0 i0 r1 i1
    const __m128 b = _mm_loadu_ps(p + 4);    // r2 i2 r3 i3
    re = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
    im = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
}

/** Interleave two vectors back into 8 consecutive floats — NEON's vst2q_f32. */
static inline void sseStore2(float* p, __m128 re, __m128 im) {
    _mm_storeu_ps(p,     _mm_unpacklo_ps(re, im));   // r0 i0 r1 i1
    _mm_storeu_ps(p + 4, _mm_unpackhi_ps(re, im));   // r2 i2 r3 i3
}

/** Lane-wise select — NEON's vbslq_f32(mask, a, b). */
static inline __m128 sseSel(__m128 mask, __m128 a, __m128 b) {
    return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
}

/** |x| and -x, by sign-bit masking (no branch, no constant load beyond the mask). */
static inline __m128 sseAbs(__m128 x) { return _mm_andnot_ps(_mm_set1_ps(-0.0f), x); }
static inline __m128 sseNeg(__m128 x) { return _mm_xor_ps(x, _mm_set1_ps(-0.0f)); }

/** a + b*c — NEON's vmlaq_f32. ★ SSE2 has no FMA, so this really is two operations; the result is
 *  the ROUNDED product plus a, which is what the scalar reference does too. (FMA would actually
 *  differ from scalar by keeping the intermediate at full width.) */
static inline __m128 sseMla(__m128 a, __m128 b, __m128 c) {
    return _mm_add_ps(a, _mm_mul_ps(b, c));
}
#endif  // VIBE_SSE

// ── Dot products ────────────────────────────────────────────────────────────
// Real: sum(a[j]*b[j]). Complex: sum(t[j]*z[j]), z interleaved re/im (len 2K).
static inline float dotReal(const float* a, const float* b, int K) {
#if VIBE_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    int j = 0;
    for (; j + 4 <= K; j += 4)
        acc = vmlaq_f32(acc, vld1q_f32(a + j), vld1q_f32(b + j));
    float s = vaddvq_f32(acc);
    for (; j < K; ++j) s += a[j] * b[j];
    return s;
#elif VIBE_SSE
    __m128 acc = _mm_setzero_ps();
    int j = 0;
    for (; j + 4 <= K; j += 4)
        acc = sseMla(acc, _mm_loadu_ps(a + j), _mm_loadu_ps(b + j));
    float s = sseAddv(acc);
    for (; j < K; ++j) s += a[j] * b[j];
    return s;
#else
    float s = 0.0f;
    for (int j = 0; j < K; ++j) s += a[j] * b[j];
    return s;
#endif
}

static inline cf32 dotCplx(const float* t, const float* z, int K) {
#if VIBE_NEON
    float32x4_t ar = vdupq_n_f32(0.0f), ai = vdupq_n_f32(0.0f);
    int j = 0;
    for (; j + 4 <= K; j += 4) {
        const float32x4_t tv = vld1q_f32(t + j);
        const float32x4x2_t zv = vld2q_f32(z + 2 * j);   // de-interleave re/im
        ar = vmlaq_f32(ar, tv, zv.val[0]);
        ai = vmlaq_f32(ai, tv, zv.val[1]);
    }
    float re = vaddvq_f32(ar), im = vaddvq_f32(ai);
    for (; j < K; ++j) { re += t[j] * z[2 * j]; im += t[j] * z[2 * j + 1]; }
    return cf32(re, im);
#elif VIBE_SSE
    __m128 ar = _mm_setzero_ps(), ai = _mm_setzero_ps();
    int j = 0;
    for (; j + 4 <= K; j += 4) {
        const __m128 tv = _mm_loadu_ps(t + j);
        __m128 zr, zi;
        sseLoad2(z + 2 * j, zr, zi);
        ar = sseMla(ar, tv, zr);
        ai = sseMla(ai, tv, zi);
    }
    float re = sseAddv(ar), im = sseAddv(ai);
    for (; j < K; ++j) { re += t[j] * z[2 * j]; im += t[j] * z[2 * j + 1]; }
    return cf32(re, im);
#else
    float re = 0.0f, im = 0.0f;
    for (int j = 0; j < K; ++j) { re += t[j] * z[2 * j]; im += t[j] * z[2 * j + 1]; }
    return cf32(re, im);
#endif
}

// ── Complex × real (windowing): out[i] = in[i] * w[i] ───────────────────────
static inline void mulComplexReal(const cf32* in, const float* w, cf32* out, int n) {
    const float* zin = reinterpret_cast<const float*>(in);
    float* zout = reinterpret_cast<float*>(out);
#if VIBE_NEON
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const float32x4_t wv = vld1q_f32(w + i);
        float32x4x2_t z = vld2q_f32(zin + 2 * i);
        z.val[0] = vmulq_f32(z.val[0], wv);
        z.val[1] = vmulq_f32(z.val[1], wv);
        vst2q_f32(zout + 2 * i, z);
    }
    for (; i < n; ++i) { zout[2*i] = zin[2*i] * w[i]; zout[2*i+1] = zin[2*i+1] * w[i]; }
#elif VIBE_SSE
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128 wv = _mm_loadu_ps(w + i);
        __m128 zr, zi;
        sseLoad2(zin + 2 * i, zr, zi);
        sseStore2(zout + 2 * i, _mm_mul_ps(zr, wv), _mm_mul_ps(zi, wv));
    }
    for (; i < n; ++i) { zout[2*i] = zin[2*i] * w[i]; zout[2*i+1] = zin[2*i+1] * w[i]; }
#else
    for (int i = 0; i < n; ++i) { zout[2*i] = zin[2*i] * w[i]; zout[2*i+1] = zin[2*i+1] * w[i]; }
#endif
}

// ── Fast log2 (Mineiro, public domain) — ~3e-4 max error ────────────────────
// Used for the waterfall power->dB (a uint8 display; tiny error is invisible)
// at ~millions of bins/sec, replacing std::log10.
static inline float fastLog2(float x) {
    union { float f; uint32_t i; } vx = { x };
    union { uint32_t i; float f; } mx = { (vx.i & 0x007FFFFFu) | 0x3f000000u };
    float y = (float)vx.i * 1.1920928955078125e-7f;
    return y - 124.22551499f - 1.498030302f * mx.f - 1.72587999f / (0.3520887068f + mx.f);
}
static constexpr float kDbPerLog2 = 3.0102999566398120f;   // 10*log10(2)
static inline float powerToDb(float p) {
    if (p < 1e-20f) p = 1e-20f;
    return kDbPerLog2 * fastLog2(p);
}

// ── Accurate fast atan2 — ~1e-6 max error (inaudible for FM) ─────────────────
// Minimax atan core on [-1,1] + octant reconstruction. Far cheaper than
// std::atan2 at the channel rate, without the gross error of cruder approxes
// (which corrupted the stereo L-R difference).
static inline float fastAtan2(float y, float x) {
    if (x == 0.0f && y == 0.0f) return 0.0f;
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float z = (ax > ay) ? (ay / ax) : (ax / ay);      // |t| in [0,1]
    const float z2 = z * z;
    // 7th-order odd minimax for atan(z), z in [0,1].
    float a = z * (0.99997726f + z2 * (-0.33262347f + z2 * (0.19354346f +
              z2 * (-0.11643287f + z2 * (0.05265332f - z2 * 0.01172120f)))));
    if (ay > ax) a = 1.57079632679f - a;     // fold into [0,pi/4] region
    if (x < 0.0f) a = 3.14159265359f - a;
    return (y < 0.0f) ? -a : a;
}

// ── 4-wide atan2 (NEON) ─────────────────────────────────────────────────────
// Same minimax core and octant reconstruction as fastAtan2, branch-free so four
// FM discriminator samples resolve at once. This is the hottest transcendental
// in the engine: WFM runs it at the channel rate (~320 kHz) and its output IS
// the MPX, so the accuracy has to match the scalar version exactly — it does,
// bit-for-bit apart from the reciprocal's rounding.
#if VIBE_NEON
static inline float32x4_t fastAtan2q(float32x4_t y, float32x4_t x) {
    const float32x4_t ax = vabsq_f32(x), ay = vabsq_f32(y);
    const float32x4_t num = vminq_f32(ax, ay);
    // Floor the denominator so the y==x==0 case divides to 0 (-> angle 0) rather
    // than NaN. Anything above the floor is untouched at float precision.
    const float32x4_t den = vmaxq_f32(vmaxq_f32(ax, ay), vdupq_n_f32(1e-30f));
    const float32x4_t z  = vdivq_f32(num, den);          // |t| in [0,1]
    const float32x4_t z2 = vmulq_f32(z, z);

    float32x4_t p = vdupq_n_f32(-0.01172120f);
    p = vmlaq_f32(vdupq_n_f32( 0.05265332f), z2, p);
    p = vmlaq_f32(vdupq_n_f32(-0.11643287f), z2, p);
    p = vmlaq_f32(vdupq_n_f32( 0.19354346f), z2, p);
    p = vmlaq_f32(vdupq_n_f32(-0.33262347f), z2, p);
    p = vmlaq_f32(vdupq_n_f32( 0.99997726f), z2, p);
    float32x4_t a = vmulq_f32(z, p);

    const float32x4_t kHalfPi = vdupq_n_f32(1.57079632679f);
    const float32x4_t kPi     = vdupq_n_f32(3.14159265359f);
    a = vbslq_f32(vcgtq_f32(ay, ax), vsubq_f32(kHalfPi, a), a);   // fold
    a = vbslq_f32(vcltq_f32(x, vdupq_n_f32(0.0f)), vsubq_f32(kPi, a), a);
    return vbslq_f32(vcltq_f32(y, vdupq_n_f32(0.0f)), vnegq_f32(a), a);
}
#endif

// ── 4-wide atan2 (SSE2) ─────────────────────────────────────────────────────
// ★★★ THE SAME MINIMAX CORE AND THE SAME OCTANT RECONSTRUCTION as the scalar and NEON versions,
//     coefficient for coefficient. This is the hottest transcendental in the engine — WFM runs it
//     at the channel rate and its output IS the MPX — so any divergence here is not a rounding
//     detail, it is the stereo difference signal and the RDS subcarrier.
// ★★ Branch-free by mask select, exactly as NEON is: the folds are data-dependent per lane, and a
//    branch would serialise four samples that have no reason to wait for each other.
// ★ The denominator floor is what makes y==x==0 divide to 0 (angle 0) instead of producing NaN —
//   the same guard, for the same reason, as the other two paths.
#if VIBE_SSE
static inline __m128 fastAtan2q(__m128 y, __m128 x) {
    const __m128 ax = sseAbs(x), ay = sseAbs(y);
    const __m128 num = _mm_min_ps(ax, ay);
    const __m128 den = _mm_max_ps(_mm_max_ps(ax, ay), _mm_set1_ps(1e-30f));
    const __m128 z  = _mm_div_ps(num, den);              // |t| in [0,1]
    const __m128 z2 = _mm_mul_ps(z, z);

    __m128 p = _mm_set1_ps(-0.01172120f);
    p = sseMla(_mm_set1_ps( 0.05265332f), z2, p);
    p = sseMla(_mm_set1_ps(-0.11643287f), z2, p);
    p = sseMla(_mm_set1_ps( 0.19354346f), z2, p);
    p = sseMla(_mm_set1_ps(-0.33262347f), z2, p);
    p = sseMla(_mm_set1_ps( 0.99997726f), z2, p);
    __m128 a = _mm_mul_ps(z, p);

    const __m128 kHalfPi = _mm_set1_ps(1.57079632679f);
    const __m128 kPi     = _mm_set1_ps(3.14159265359f);
    const __m128 zero    = _mm_setzero_ps();
    a = sseSel(_mm_cmpgt_ps(ay, ax), _mm_sub_ps(kHalfPi, a), a);   // fold
    a = sseSel(_mm_cmplt_ps(x, zero), _mm_sub_ps(kPi, a), a);
    return sseSel(_mm_cmplt_ps(y, zero), sseNeg(a), a);
}
#endif

// ── WFM stereo matrix + blend ───────────────────────────────────────────────
// L = 0.5*((L+R) + b*(L-R)), R = 0.5*((L+R) - b*(L-R)), where b is the stereo
// blend ramping one-pole-style toward `target` (anti-screech: see pipeline.cpp).
//
// The ramp b += k*(target-b) looks serial, but its error e = target-b is just a
// geometric sequence e_i = e_0*(1-k)^i, so four lanes can be evaluated exactly —
// no approximation, no per-sample dependency. Returns the blend after n samples.
static inline float stereoMatrixBlend(const float* lpr, const float* lmr,
                                      float* outL, float* outR,
                                      int n, float blend, float ramp, float target) {
    const float g = 1.0f - ramp;
    float e = target - blend;                 // ramp error, decays by g each sample
    int i = 0;
#if VIBE_NEON
    const float g2 = g * g, g4 = g2 * g2;
    const float32x4_t gpow = { 1.0f, g, g2, g2 * g };
    const float32x4_t tgt = vdupq_n_f32(target), half = vdupq_n_f32(0.5f);
    float32x4_t ev = vmulq_n_f32(gpow, e);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t b = vsubq_f32(tgt, ev);            // blend for these 4
        const float32x4_t s = vld1q_f32(lpr + i);            // L+R
        const float32x4_t d = vmulq_f32(vld1q_f32(lmr + i), b);  // blended L-R
        vst1q_f32(outL + i, vmulq_f32(half, vaddq_f32(s, d)));
        vst1q_f32(outR + i, vmulq_f32(half, vsubq_f32(s, d)));
        ev = vmulq_n_f32(ev, g4);
    }
    e = vgetq_lane_f32(ev, 0);
#elif VIBE_SSE
    const float g2 = g * g, g4 = g2 * g2;
    // ★ setr, not set: `_mm_setr_ps` takes its arguments in memory order, which is what the NEON
    //   initialiser list above means. `_mm_set_ps` would reverse the ramp and blend backwards.
    const __m128 gpow = _mm_setr_ps(1.0f, g, g2, g2 * g);
    const __m128 tgt = _mm_set1_ps(target), half = _mm_set1_ps(0.5f);
    const __m128 g4v = _mm_set1_ps(g4);
    __m128 ev = _mm_mul_ps(gpow, _mm_set1_ps(e));
    for (; i + 4 <= n; i += 4) {
        const __m128 b = _mm_sub_ps(tgt, ev);                 // blend for these 4
        const __m128 sv = _mm_loadu_ps(lpr + i);              // L+R
        const __m128 d = _mm_mul_ps(_mm_loadu_ps(lmr + i), b);// blended L-R
        _mm_storeu_ps(outL + i, _mm_mul_ps(half, _mm_add_ps(sv, d)));
        _mm_storeu_ps(outR + i, _mm_mul_ps(half, _mm_sub_ps(sv, d)));
        ev = _mm_mul_ps(ev, g4v);
    }
    e = _mm_cvtss_f32(ev);
#endif
    for (; i < n; ++i) {
        const float b = target - e;
        e *= g;
        const float s = lpr[i], d = lmr[i] * b;
        outL[i] = 0.5f * (s + d);
        outR[i] = 0.5f * (s - d);
    }
    return target - e;
}

// ── Interleave two mono channels into stereo frames ─────────────────────────
static inline void interleave2(const float* l, const float* r, float* out, int n) {
#if VIBE_NEON
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4x2_t v = { vld1q_f32(l + i), vld1q_f32(r + i) };
        vst2q_f32(out + 2 * i, v);
    }
    for (; i < n; ++i) { out[2*i] = l[i]; out[2*i+1] = r[i]; }
#elif VIBE_SSE
    int i = 0;
    for (; i + 4 <= n; i += 4)
        sseStore2(out + 2 * i, _mm_loadu_ps(l + i), _mm_loadu_ps(r + i));
    for (; i < n; ++i) { out[2*i] = l[i]; out[2*i+1] = r[i]; }
#else
    for (int i = 0; i < n; ++i) { out[2*i] = l[i]; out[2*i+1] = r[i]; }
#endif
}

} // namespace vibedsp
