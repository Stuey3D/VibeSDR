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

#if defined(__aarch64__)
  #include <arm_neon.h>
  #define VIBE_NEON 1
#endif

namespace vibedsp {

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
#else
    for (int i = 0; i < n; ++i) { out[2*i] = l[i]; out[2*i+1] = r[i]; }
#endif
}

} // namespace vibedsp
