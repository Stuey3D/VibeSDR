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
// ★★★ __ARM_NEON, NOT __aarch64__: 32-bit ARMv7 (armeabi-v7a, the old phones) has NEON too and
//     was silently running the scalar fallback everywhere — see neon_compat.h (2026-09-16).
#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
  #include <arm_neon.h>
  #include "neon_compat.h"
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
// ★★★ TWO ACCUMULATORS, NOT ONE (2026-09-16). A fused multiply-add has a 4-cycle latency on the
//     in-order Cortex-A53 (Pi 3, Zero 2 W, every cheap phone) and a single accumulator makes each
//     one wait for the last: the FIR was the top symbol in the Pi 3 WFM profile (dotCplx 14 %,
//     dotReal 6 %) while using a quarter of the multiplier. Two independent chains halve the
//     stall; the out-of-order cores (Mac, Lenovo) were already hiding it and lose nothing.
//  ★ The scalar path does the same with four partial sums, which is what lets the ARMv6 VFP
//    (Pi Zero W — no NEON at all) pipeline its multiply-accumulates instead of serialising them.
//  ★ The order of float additions changes, so results differ from before in the last bit or two;
//    every kernel test still passes and the audio is unaffected.
static inline float dotReal(const float* a, const float* b, int K) {
#if VIBE_NEON
    float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f);
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        acc0 = vmlaq_f32(acc0, vld1q_f32(a + j),     vld1q_f32(b + j));
        acc1 = vmlaq_f32(acc1, vld1q_f32(a + j + 4), vld1q_f32(b + j + 4));
    }
    if (j + 4 <= K) { acc0 = vmlaq_f32(acc0, vld1q_f32(a + j), vld1q_f32(b + j)); j += 4; }
    float s = vaddvq_f32(vaddq_f32(acc0, acc1));
    for (; j < K; ++j) s += a[j] * b[j];
    return s;
#elif VIBE_SSE
    __m128 acc0 = _mm_setzero_ps(), acc1 = _mm_setzero_ps();
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        acc0 = sseMla(acc0, _mm_loadu_ps(a + j),     _mm_loadu_ps(b + j));
        acc1 = sseMla(acc1, _mm_loadu_ps(a + j + 4), _mm_loadu_ps(b + j + 4));
    }
    if (j + 4 <= K) { acc0 = sseMla(acc0, _mm_loadu_ps(a + j), _mm_loadu_ps(b + j)); j += 4; }
    float s = sseAddv(_mm_add_ps(acc0, acc1));
    for (; j < K; ++j) s += a[j] * b[j];
    return s;
#else
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int j = 0;
    for (; j + 4 <= K; j += 4) {
        s0 += a[j] * b[j];         s1 += a[j + 1] * b[j + 1];
        s2 += a[j + 2] * b[j + 2]; s3 += a[j + 3] * b[j + 3];
    }
    float s = (s0 + s1) + (s2 + s3);
    for (; j < K; ++j) s += a[j] * b[j];
    return s;
#endif
}

static inline cf32 dotCplx(const float* t, const float* z, int K) {
#if VIBE_NEON
    float32x4_t ar0 = vdupq_n_f32(0.0f), ai0 = vdupq_n_f32(0.0f);
    float32x4_t ar1 = vdupq_n_f32(0.0f), ai1 = vdupq_n_f32(0.0f);
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        const float32x4_t t0 = vld1q_f32(t + j), t1 = vld1q_f32(t + j + 4);
        const float32x4x2_t z0 = vld2q_f32(z + 2 * j), z1 = vld2q_f32(z + 2 * j + 8);
        ar0 = vmlaq_f32(ar0, t0, z0.val[0]);
        ai0 = vmlaq_f32(ai0, t0, z0.val[1]);
        ar1 = vmlaq_f32(ar1, t1, z1.val[0]);
        ai1 = vmlaq_f32(ai1, t1, z1.val[1]);
    }
    if (j + 4 <= K) {
        const float32x4_t tv = vld1q_f32(t + j);
        const float32x4x2_t zv = vld2q_f32(z + 2 * j);
        ar0 = vmlaq_f32(ar0, tv, zv.val[0]);
        ai0 = vmlaq_f32(ai0, tv, zv.val[1]);
        j += 4;
    }
    float re = vaddvq_f32(vaddq_f32(ar0, ar1)), im = vaddvq_f32(vaddq_f32(ai0, ai1));
    for (; j < K; ++j) { re += t[j] * z[2 * j]; im += t[j] * z[2 * j + 1]; }
    return cf32(re, im);
#elif VIBE_SSE
    __m128 ar0 = _mm_setzero_ps(), ai0 = _mm_setzero_ps(), ar1 = _mm_setzero_ps(), ai1 = _mm_setzero_ps();
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        __m128 zr, zi;
        sseLoad2(z + 2 * j, zr, zi);
        const __m128 t0 = _mm_loadu_ps(t + j);
        ar0 = sseMla(ar0, t0, zr); ai0 = sseMla(ai0, t0, zi);
        sseLoad2(z + 2 * j + 8, zr, zi);
        const __m128 t1 = _mm_loadu_ps(t + j + 4);
        ar1 = sseMla(ar1, t1, zr); ai1 = sseMla(ai1, t1, zi);
    }
    if (j + 4 <= K) {
        __m128 zr, zi;
        sseLoad2(z + 2 * j, zr, zi);
        const __m128 tv = _mm_loadu_ps(t + j);
        ar0 = sseMla(ar0, tv, zr); ai0 = sseMla(ai0, tv, zi);
        j += 4;
    }
    float re = sseAddv(_mm_add_ps(ar0, ar1)), im = sseAddv(_mm_add_ps(ai0, ai1));
    for (; j < K; ++j) { re += t[j] * z[2 * j]; im += t[j] * z[2 * j + 1]; }
    return cf32(re, im);
#else
    float r0 = 0.0f, i0 = 0.0f, r1 = 0.0f, i1 = 0.0f;
    int j = 0;
    for (; j + 2 <= K; j += 2) {
        r0 += t[j] * z[2 * j];         i0 += t[j] * z[2 * j + 1];
        r1 += t[j + 1] * z[2 * j + 2]; i1 += t[j + 1] * z[2 * j + 3];
    }
    float re = r0 + r1, im = i0 + i1;
    for (; j < K; ++j) { re += t[j] * z[2 * j]; im += t[j] * z[2 * j + 1]; }
    return cf32(re, im);
#endif
}

// ── Two outputs of a DECIMATING FIR per tap load ────────────────────────────
// ★★★ BIT-IDENTICAL TO TWO CALLS OF dotReal / dotCplx — the same accumulators, the same order, the
//     same horizontal sums and the same scalar tail, per output. All that changes is that each tap
//     vector is LOADED ONCE and used twice: the second output's window starts `D` samples after the
//     first's. The Cortex-A53 has a 64-bit load path and this loop is load-bound on it (see the
//     rejected tap-duplication experiment below — twice the tap loads cost more than the shuffle
//     they saved), so sharing the tap loads is the lever. Mac: 2.5 → 2.0 ms for a 43-tap /4
//     decimator over 3 M samples, 0 of 750000 outputs differing (2026-09-30).
// ★ Used by MpxMeasure (the Advanced RDS instrument), whose own decimators have no other caller;
//   the listener's FirDecimator/RealFir keep the single-output kernels untouched.
static inline void dotReal2(const float* a, const float* b, int K, int D, float& y0, float& y1) {
    const float* c = b + D;
#if VIBE_NEON
    float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f);
    float32x4_t bcc0 = vdupq_n_f32(0.0f), bcc1 = vdupq_n_f32(0.0f);
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        const float32x4_t t0 = vld1q_f32(a + j), t1 = vld1q_f32(a + j + 4);
        acc0 = vmlaq_f32(acc0, t0, vld1q_f32(b + j));
        acc1 = vmlaq_f32(acc1, t1, vld1q_f32(b + j + 4));
        bcc0 = vmlaq_f32(bcc0, t0, vld1q_f32(c + j));
        bcc1 = vmlaq_f32(bcc1, t1, vld1q_f32(c + j + 4));
    }
    if (j + 4 <= K) {
        const float32x4_t tv = vld1q_f32(a + j);
        acc0 = vmlaq_f32(acc0, tv, vld1q_f32(b + j));
        bcc0 = vmlaq_f32(bcc0, tv, vld1q_f32(c + j));
        j += 4;
    }
    float s = vaddvq_f32(vaddq_f32(acc0, acc1)), u = vaddvq_f32(vaddq_f32(bcc0, bcc1));
    for (; j < K; ++j) { s += a[j] * b[j]; u += a[j] * c[j]; }
    y0 = s; y1 = u;
#elif VIBE_SSE
    __m128 acc0 = _mm_setzero_ps(), acc1 = _mm_setzero_ps(), bcc0 = _mm_setzero_ps(), bcc1 = _mm_setzero_ps();
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        const __m128 t0 = _mm_loadu_ps(a + j), t1 = _mm_loadu_ps(a + j + 4);
        acc0 = sseMla(acc0, t0, _mm_loadu_ps(b + j));
        acc1 = sseMla(acc1, t1, _mm_loadu_ps(b + j + 4));
        bcc0 = sseMla(bcc0, t0, _mm_loadu_ps(c + j));
        bcc1 = sseMla(bcc1, t1, _mm_loadu_ps(c + j + 4));
    }
    if (j + 4 <= K) {
        const __m128 tv = _mm_loadu_ps(a + j);
        acc0 = sseMla(acc0, tv, _mm_loadu_ps(b + j));
        bcc0 = sseMla(bcc0, tv, _mm_loadu_ps(c + j));
        j += 4;
    }
    float s = sseAddv(_mm_add_ps(acc0, acc1)), u = sseAddv(_mm_add_ps(bcc0, bcc1));
    for (; j < K; ++j) { s += a[j] * b[j]; u += a[j] * c[j]; }
    y0 = s; y1 = u;
#else
    /* ★★ THE SCALAR PATH IS TWO PLAIN CALLS. Written out paired it came back DIFFERENT in 7544 of
     *  20800 outputs (2026-09-30): with no intrinsics pinning the operations, the compiler contracts
     *  and vectorises the two loop shapes differently, so "the same expression" is not the same
     *  arithmetic. The identity is the contract here, and this is the only form that keeps it. */
    y0 = dotReal(a, b, K); y1 = dotReal(a, c, K);
#endif
}

/** Two complex outputs, the second `D` complex samples after the first — see dotReal2. */
static inline void dotCplx2(const float* t, const float* z, int K, int D, cf32& y0, cf32& y1) {
    const float* w = z + 2 * D;
#if VIBE_NEON
    float32x4_t ar0 = vdupq_n_f32(0.0f), ai0 = vdupq_n_f32(0.0f), ar1 = vdupq_n_f32(0.0f), ai1 = vdupq_n_f32(0.0f);
    float32x4_t br0 = vdupq_n_f32(0.0f), bi0 = vdupq_n_f32(0.0f), br1 = vdupq_n_f32(0.0f), bi1 = vdupq_n_f32(0.0f);
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        const float32x4_t t0 = vld1q_f32(t + j), t1 = vld1q_f32(t + j + 4);
        const float32x4x2_t z0 = vld2q_f32(z + 2 * j), z1 = vld2q_f32(z + 2 * j + 8);
        const float32x4x2_t w0 = vld2q_f32(w + 2 * j), w1 = vld2q_f32(w + 2 * j + 8);
        ar0 = vmlaq_f32(ar0, t0, z0.val[0]); ai0 = vmlaq_f32(ai0, t0, z0.val[1]);
        ar1 = vmlaq_f32(ar1, t1, z1.val[0]); ai1 = vmlaq_f32(ai1, t1, z1.val[1]);
        br0 = vmlaq_f32(br0, t0, w0.val[0]); bi0 = vmlaq_f32(bi0, t0, w0.val[1]);
        br1 = vmlaq_f32(br1, t1, w1.val[0]); bi1 = vmlaq_f32(bi1, t1, w1.val[1]);
    }
    if (j + 4 <= K) {
        const float32x4_t tv = vld1q_f32(t + j);
        const float32x4x2_t zv = vld2q_f32(z + 2 * j), wv = vld2q_f32(w + 2 * j);
        ar0 = vmlaq_f32(ar0, tv, zv.val[0]); ai0 = vmlaq_f32(ai0, tv, zv.val[1]);
        br0 = vmlaq_f32(br0, tv, wv.val[0]); bi0 = vmlaq_f32(bi0, tv, wv.val[1]);
        j += 4;
    }
    float re = vaddvq_f32(vaddq_f32(ar0, ar1)), im = vaddvq_f32(vaddq_f32(ai0, ai1));
    float re2 = vaddvq_f32(vaddq_f32(br0, br1)), im2 = vaddvq_f32(vaddq_f32(bi0, bi1));
    for (; j < K; ++j) { re += t[j] * z[2 * j]; im += t[j] * z[2 * j + 1];
                         re2 += t[j] * w[2 * j]; im2 += t[j] * w[2 * j + 1]; }
    y0 = cf32(re, im); y1 = cf32(re2, im2);
#elif VIBE_SSE
    __m128 ar0 = _mm_setzero_ps(), ai0 = _mm_setzero_ps(), ar1 = _mm_setzero_ps(), ai1 = _mm_setzero_ps();
    __m128 br0 = _mm_setzero_ps(), bi0 = _mm_setzero_ps(), br1 = _mm_setzero_ps(), bi1 = _mm_setzero_ps();
    int j = 0;
    for (; j + 8 <= K; j += 8) {
        __m128 zr, zi, wr, wi;
        const __m128 t0 = _mm_loadu_ps(t + j), t1 = _mm_loadu_ps(t + j + 4);
        sseLoad2(z + 2 * j, zr, zi);     ar0 = sseMla(ar0, t0, zr); ai0 = sseMla(ai0, t0, zi);
        sseLoad2(w + 2 * j, wr, wi);     br0 = sseMla(br0, t0, wr); bi0 = sseMla(bi0, t0, wi);
        sseLoad2(z + 2 * j + 8, zr, zi); ar1 = sseMla(ar1, t1, zr); ai1 = sseMla(ai1, t1, zi);
        sseLoad2(w + 2 * j + 8, wr, wi); br1 = sseMla(br1, t1, wr); bi1 = sseMla(bi1, t1, wi);
    }
    if (j + 4 <= K) {
        __m128 zr, zi, wr, wi;
        const __m128 tv = _mm_loadu_ps(t + j);
        sseLoad2(z + 2 * j, zr, zi); ar0 = sseMla(ar0, tv, zr); ai0 = sseMla(ai0, tv, zi);
        sseLoad2(w + 2 * j, wr, wi); br0 = sseMla(br0, tv, wr); bi0 = sseMla(bi0, tv, wi);
        j += 4;
    }
    float re = sseAddv(_mm_add_ps(ar0, ar1)), im = sseAddv(_mm_add_ps(ai0, ai1));
    float re2 = sseAddv(_mm_add_ps(br0, br1)), im2 = sseAddv(_mm_add_ps(bi0, bi1));
    for (; j < K; ++j) { re += t[j] * z[2 * j]; im += t[j] * z[2 * j + 1];
                         re2 += t[j] * w[2 * j]; im2 += t[j] * w[2 * j + 1]; }
    y0 = cf32(re, im); y1 = cf32(re2, im2);
#else
    // ★★ Two plain calls — see the scalar note in dotReal2.
    y0 = dotCplx(t, z, K); y1 = dotCplx(t, w, K);
#endif
}

// ✗ TRIED AND REJECTED (2026-09-16): a dotCplx variant with the taps stored pair-duplicated
//   [t0 t0 t1 t1 …] so the interleaved samples multiply straight through with plain loads and no
//   vld2q de-interleave. Measured on the Pi 3 (Cortex-A53, ARMv7 NEON): WFM 1.92 MS/s 55.5 % →
//   62.1 % of a core, NFM 250 kS/s 5.4 % → 6.5 %. Twice the tap loads cost more than the
//   de-interleave saved — the A53 is load-bound here, not shuffle-bound. Do not retry without a
//   different reason. Commit 1a09ae5a's successor carries the full experiment.

// ── Complex magnitude, four at a time ───────────────────────────────────────
// |z| for n interleaved samples. AM detection and the noise blanker each did a scalar sqrt per
// IQ sample; the recurrences that follow them stay scalar, the square roots do not have to.
static inline void magnitudes(const cf32* z, float* out, int n) {
    const float* f = reinterpret_cast<const float*>(z);
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(f + 2 * i);
        const float32x4_t p = vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]);
        float32x4_t r = vrsqrteq_f32(p);
        r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(p, r), r));
        r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(p, r), r));
        const uint32x4_t nz = vcgtq_f32(p, vdupq_n_f32(1e-30f));
        vst1q_f32(out + i, vbslq_f32(nz, vmulq_f32(p, r), vdupq_n_f32(0.0f)));
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        __m128 re, im;
        sseLoad2(f + 2 * i, re, im);
        _mm_storeu_ps(out + i, _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im))));
    }
#endif
    for (; i < n; ++i) out[i] = std::sqrt(f[2 * i] * f[2 * i] + f[2 * i + 1] * f[2 * i + 1]);
}

// ── int16 interleaved IQ → float, scaled ────────────────────────────────────
// 16-bit radios (RSP, Airspy, SpyServer) — the u8 conversion had a NEON path, this had none.
static inline void convI16ToF32(const int16_t* in, float* out, int nF, float scale) {
    int i = 0;
#if VIBE_NEON
    const float32x4_t sc = vdupq_n_f32(scale);
    for (; i + 8 <= nF; i += 8) {
        const int16x8_t v = vld1q_s16(in + i);
        vst1q_f32(out + i,     vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(v))), sc));
        vst1q_f32(out + i + 4, vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(v))), sc));
    }
#elif VIBE_SSE
    const __m128 sc = _mm_set1_ps(scale);
    for (; i + 8 <= nF; i += 8) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
        // sign-extend 16 → 32 with the SSE2 shift trick (no pmovsx before SSE4.1)
        const __m128i lo = _mm_srai_epi32(_mm_unpacklo_epi16(v, v), 16);
        const __m128i hi = _mm_srai_epi32(_mm_unpackhi_epi16(v, v), 16);
        _mm_storeu_ps(out + i,     _mm_mul_ps(_mm_cvtepi32_ps(lo), sc));
        _mm_storeu_ps(out + i + 4, _mm_mul_ps(_mm_cvtepi32_ps(hi), sc));
    }
#endif
    for (; i < nF; ++i) out[i] = (float)in[i] * scale;
}

// ── Recursive complex rotator, four lanes, no per-sample trig ───────────────
// out[i] = in[i] * conj(e^{j·phase_i}) with phase advancing by `step` radians per sample —
// the SSB Weaver mix and the RDS guard-band rotation both did a scalar recurrence (or, in the
// RDS case, a libm cos/sin PER SAMPLE). The four lanes hold consecutive phases and advance by the
// four-sample twiddle; `cr/ci` carry the oscillator between calls and are renormalised here every
// call (n is a block, so drift cannot accumulate). If `cosOut/sinOut` are given, the per-sample
// c/s are written for a later re-mix. Sign convention: aI = zr*c + zi*s, aQ = zi*c - zr*s.
static inline void rotateBlock(const float* zr_in, const float* zi_in, int n,
                               float& cr, float& ci, float stepCos, float stepSin,
                               float* aI, float* aQ, float* cosOut, float* sinOut) {
    int i = 0;
#if VIBE_NEON || VIBE_SSE
    float pr[4], pi[4];
    pr[0] = cr; pi[0] = ci;
    for (int q = 1; q < 4; ++q) { pr[q] = pr[q-1] * stepCos - pi[q-1] * stepSin; pi[q] = pr[q-1] * stepSin + pi[q-1] * stepCos; }
    // cos 4w = 2cos²2w − 1, sin 4w = 2 sin 2w cos 2w  (★ the triple-angle form was here once —
    // it put the SSB tone 1 Hz out and let the wrong sideband through; the tests caught it)
    const float c2 = stepCos * stepCos - stepSin * stepSin, s2 = 2.0f * stepSin * stepCos;
    const float c4 = c2 * c2 - s2 * s2, s4 = 2.0f * s2 * c2;
#if VIBE_NEON
    float32x4_t vr = vld1q_f32(pr), vi = vld1q_f32(pi);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t zr = vld1q_f32(zr_in + i), zi = vld1q_f32(zi_in + i);
        vst1q_f32(aI + i, vmlaq_f32(vmulq_f32(zr, vr), zi, vi));
        vst1q_f32(aQ + i, vmlsq_f32(vmulq_f32(zi, vr), zr, vi));
        if (cosOut) { vst1q_f32(cosOut + i, vr); vst1q_f32(sinOut + i, vi); }
        const float32x4_t nr = vmlsq_n_f32(vmulq_n_f32(vr, c4), vi, s4);
        const float32x4_t ni = vmlaq_n_f32(vmulq_n_f32(vr, s4), vi, c4);
        vr = nr; vi = ni;
    }
    cr = vgetq_lane_f32(vr, 0); ci = vgetq_lane_f32(vi, 0);
#else
    __m128 vr = _mm_loadu_ps(pr), vi = _mm_loadu_ps(pi);
    const __m128 c4v = _mm_set1_ps(c4), s4v = _mm_set1_ps(s4);
    for (; i + 4 <= n; i += 4) {
        const __m128 zr = _mm_loadu_ps(zr_in + i), zi = _mm_loadu_ps(zi_in + i);
        _mm_storeu_ps(aI + i, _mm_add_ps(_mm_mul_ps(zr, vr), _mm_mul_ps(zi, vi)));
        _mm_storeu_ps(aQ + i, _mm_sub_ps(_mm_mul_ps(zi, vr), _mm_mul_ps(zr, vi)));
        if (cosOut) { _mm_storeu_ps(cosOut + i, vr); _mm_storeu_ps(sinOut + i, vi); }
        const __m128 nr = _mm_sub_ps(_mm_mul_ps(vr, c4v), _mm_mul_ps(vi, s4v));
        const __m128 ni = _mm_add_ps(_mm_mul_ps(vr, s4v), _mm_mul_ps(vi, c4v));
        vr = nr; vi = ni;
    }
    cr = _mm_cvtss_f32(vr); ci = _mm_cvtss_f32(vi);
#endif
#endif
    for (; i < n; ++i) {
        const float c = cr, sn = ci;
        aI[i] = zr_in[i] * c + zi_in[i] * sn;
        aQ[i] = zi_in[i] * c - zr_in[i] * sn;
        if (cosOut) { cosOut[i] = c; sinOut[i] = sn; }
        const float nr = c * stepCos - sn * stepSin, ni = c * stepSin + sn * stepCos;
        cr = nr; ci = ni;
    }
    const float m = std::sqrt(cr * cr + ci * ci);
    if (m > 0.0f) { cr /= m; ci /= m; }
}
/** The same rotator over interleaved complex input (the SSB Weaver mix takes the DDC output). */
static inline void rotateBlockIlv(const cf32* z, int n, float& cr, float& ci, float stepCos, float stepSin,
                                  float* aI, float* aQ, float* cosOut, float* sinOut) {
    const float* f = reinterpret_cast<const float*>(z);
    int i = 0;
#if VIBE_NEON || VIBE_SSE
    float pr[4], pi[4];
    pr[0] = cr; pi[0] = ci;
    for (int q = 1; q < 4; ++q) { pr[q] = pr[q-1] * stepCos - pi[q-1] * stepSin; pi[q] = pr[q-1] * stepSin + pi[q-1] * stepCos; }
    // cos 4w = 2cos²2w − 1, sin 4w = 2 sin 2w cos 2w  (★ the triple-angle form was here once —
    // it put the SSB tone 1 Hz out and let the wrong sideband through; the tests caught it)
    const float c2 = stepCos * stepCos - stepSin * stepSin, s2 = 2.0f * stepSin * stepCos;
    const float c4 = c2 * c2 - s2 * s2, s4 = 2.0f * s2 * c2;
#if VIBE_NEON
    float32x4_t vr = vld1q_f32(pr), vi = vld1q_f32(pi);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(f + 2 * i);
        vst1q_f32(aI + i, vmlaq_f32(vmulq_f32(v.val[0], vr), v.val[1], vi));
        vst1q_f32(aQ + i, vmlsq_f32(vmulq_f32(v.val[1], vr), v.val[0], vi));
        if (cosOut) { vst1q_f32(cosOut + i, vr); vst1q_f32(sinOut + i, vi); }
        const float32x4_t nr = vmlsq_n_f32(vmulq_n_f32(vr, c4), vi, s4);
        const float32x4_t ni = vmlaq_n_f32(vmulq_n_f32(vr, s4), vi, c4);
        vr = nr; vi = ni;
    }
    cr = vgetq_lane_f32(vr, 0); ci = vgetq_lane_f32(vi, 0);
#else
    __m128 vr = _mm_loadu_ps(pr), vi = _mm_loadu_ps(pi);
    const __m128 c4v = _mm_set1_ps(c4), s4v = _mm_set1_ps(s4);
    for (; i + 4 <= n; i += 4) {
        __m128 zr, zi;
        sseLoad2(f + 2 * i, zr, zi);
        _mm_storeu_ps(aI + i, _mm_add_ps(_mm_mul_ps(zr, vr), _mm_mul_ps(zi, vi)));
        _mm_storeu_ps(aQ + i, _mm_sub_ps(_mm_mul_ps(zi, vr), _mm_mul_ps(zr, vi)));
        if (cosOut) { _mm_storeu_ps(cosOut + i, vr); _mm_storeu_ps(sinOut + i, vi); }
        const __m128 nr = _mm_sub_ps(_mm_mul_ps(vr, c4v), _mm_mul_ps(vi, s4v));
        const __m128 ni = _mm_add_ps(_mm_mul_ps(vr, s4v), _mm_mul_ps(vi, c4v));
        vr = nr; vi = ni;
    }
    cr = _mm_cvtss_f32(vr); ci = _mm_cvtss_f32(vi);
#endif
#endif
    for (; i < n; ++i) {
        const float c = cr, sn = ci;
        aI[i] = f[2 * i] * c + f[2 * i + 1] * sn;
        aQ[i] = f[2 * i + 1] * c - f[2 * i] * sn;
        if (cosOut) { cosOut[i] = c; sinOut[i] = sn; }
        const float nr = c * stepCos - sn * stepSin, ni = c * stepSin + sn * stepCos;
        cr = nr; ci = ni;
    }
    const float m = std::sqrt(cr * cr + ci * ci);
    if (m > 0.0f) { cr /= m; ci /= m; }
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
// ★ The same bit-trick, four bins at a time — the spectrum's dB conversion ran per bin per frame
//   per mode, scalar, with a divide in it (2026-09-16). Lane-for-lane identical to powerToDb.
#if VIBE_NEON
static inline float32x4_t powerToDbq(float32x4_t p) {
    p = vmaxq_f32(p, vdupq_n_f32(1e-20f));
    const uint32x4_t bits = vreinterpretq_u32_f32(p);
    const float32x4_t mx = vreinterpretq_f32_u32(vorrq_u32(vandq_u32(bits, vdupq_n_u32(0x007FFFFFu)), vdupq_n_u32(0x3f000000u)));
    const float32x4_t y = vmulq_n_f32(vcvtq_f32_u32(bits), 1.1920928955078125e-7f);
    const float32x4_t den = vaddq_f32(vdupq_n_f32(0.3520887068f), mx);
    float32x4_t r = vrecpeq_f32(den);
    r = vmulq_f32(r, vrecpsq_f32(den, r));
    r = vmulq_f32(r, vrecpsq_f32(den, r));
    float32x4_t l = vsubq_f32(y, vdupq_n_f32(124.22551499f));
    l = vmlsq_n_f32(l, mx, 1.498030302f);
    l = vmlsq_n_f32(l, r, 1.72587999f);
    return vmulq_n_f32(l, kDbPerLog2);
}
#elif VIBE_SSE
static inline __m128 powerToDbq(__m128 p) {
    p = _mm_max_ps(p, _mm_set1_ps(1e-20f));
    const __m128i bits = _mm_castps_si128(p);
    const __m128 mx = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(bits, _mm_set1_epi32(0x007FFFFF)), _mm_set1_epi32(0x3f000000)));
    // unsigned int → float: the bit pattern of a positive float never has the sign bit set,
    // so the signed conversion is exact here
    const __m128 y = _mm_mul_ps(_mm_cvtepi32_ps(bits), _mm_set1_ps(1.1920928955078125e-7f));
    const __m128 den = _mm_add_ps(_mm_set1_ps(0.3520887068f), mx);
    __m128 l = _mm_sub_ps(y, _mm_set1_ps(124.22551499f));
    l = _mm_sub_ps(l, _mm_mul_ps(mx, _mm_set1_ps(1.498030302f)));
    l = _mm_sub_ps(l, _mm_div_ps(_mm_set1_ps(1.72587999f), den));
    return _mm_mul_ps(l, _mm_set1_ps(kDbPerLog2));
}
#endif
/** |z|²·scale → dB for n interleaved bins. */
static inline void powerToDbBlock(const float* z, float* outDb, int n, float scale) {
    int j = 0;
#if VIBE_NEON
    const float32x4_t sc = vdupq_n_f32(scale);
    for (; j + 4 <= n; j += 4) {
        const float32x4x2_t v = vld2q_f32(z + 2 * j);
        const float32x4_t p = vmulq_f32(vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]), sc);
        vst1q_f32(outDb + j, powerToDbq(p));
    }
#elif VIBE_SSE
    const __m128 sc = _mm_set1_ps(scale);
    for (; j + 4 <= n; j += 4) {
        __m128 re, im;
        sseLoad2(z + 2 * j, re, im);
        _mm_storeu_ps(outDb + j, powerToDbq(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im)), sc)));
    }
#endif
    for (; j < n; ++j) outDb[j] = powerToDb((z[2*j]*z[2*j] + z[2*j+1]*z[2*j+1]) * scale);
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
