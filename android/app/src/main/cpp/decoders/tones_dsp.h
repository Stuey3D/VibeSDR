// decoders/tones_dsp.h — V12 tone decoders (DTMF, 5-tone selective calling): the shared front end. docs/v12/TONES.md.
//
//  • Decim6 — the listener's 48 kHz audio to 8 kHz: a 96-tap Hamming-windowed sinc lowpass (cut 3.8 kHz: flat to 2.9 kHz,
//    the highest tone either decoder reads is 2800 Hz), one output per six inputs.
//  • goertzel — a bank of Goertzel filters over one block, FOUR FILTERS A LANE.
//  ★★ Both hot loops run NEON / SSE2 four lanes at a time (decoders/digimodes/q65_simd.h) and have a SCALAR path that does
//     the same IEEE operations in the same order — `volatile` products so no compiler fuses a multiply-add — so a SIMD and a
//     scalar build decode identically. test-tones checks the two are BIT FOR BIT equal in one binary.
#pragma once
#include "decoders/digimodes/q65_simd.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace vibe {
namespace tones {

inline const char* simdName() {
#if Q65V_NEON
    return "NEON";
#elif Q65V_SSE2
    return "SSE2";
#else
    return "scalar";
#endif
}

/** 48 → 8 kHz. Bounded: a 2 × 96-sample history (written twice so every window is contiguous). */
class Decim6 {
public:
    static constexpr int kTaps = 96, kDec = 6;
    Decim6() {
        h_.resize(kTaps);
        const double fc = 3800.0 / 48000.0;
        double sum = 0;
        for (int i = 0; i < kTaps; ++i) {
            const double m = i - (kTaps - 1) / 2.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
            const double w = 0.54 - 0.46 * std::cos(2 * M_PI * i / (kTaps - 1));
            h_[(size_t)i] = (float)(s * w); sum += s * w;
        }
        for (auto& v : h_) v = (float)(v / sum);
        buf_.assign(2 * kTaps, 0.f);
    }
    /** Appends floor((phase + n) / 6) samples to `out`. */
    void process(const float* x, int n, std::vector<float>& out, bool scalar = false) {
        for (int i = 0; i < n; ++i) {
            buf_[(size_t)pos_] = x[i]; buf_[(size_t)(pos_ + kTaps)] = x[i];
            if (++pos_ == kTaps) pos_ = 0;
            if (++ph_ < kDec) continue;
            ph_ = 0;
            const float* w = &buf_[(size_t)pos_];   // oldest … newest
            out.push_back(scalar ? dotScalar(w, h_.data()) : dot(w, h_.data()));
        }
    }
    static inline float dotScalar(const float* x, const float* h) {
        volatile float l0 = x[0] * h[0], l1 = x[1] * h[1], l2 = x[2] * h[2], l3 = x[3] * h[3];
        for (int t = 4; t < kTaps; t += 4) {
            volatile float p0 = x[t] * h[t], p1 = x[t + 1] * h[t + 1], p2 = x[t + 2] * h[t + 2], p3 = x[t + 3] * h[t + 3];
            l0 = l0 + p0; l1 = l1 + p1; l2 = l2 + p2; l3 = l3 + p3;
        }
        return (l0 + l1) + (l2 + l3);
    }
    static inline float dot(const float* x, const float* h) {
#if Q65V_NEON || Q65V_SSE2
        using namespace q65v;
        V a = mul(ld(x), ld(h));
        for (int t = 4; t < kTaps; t += 4) a = add(a, mul(ld(x + t), ld(h + t)));
        return hsum(a);
#else
        return dotScalar(x, h);
#endif
    }
private:
    std::vector<float> h_, buf_;
    int pos_ = 0, ph_ = 0;
};

/** ★ The Goertzel bank: for each coefficient c = 2 cos ω (K of them, K a multiple of 4), the power of x[0..N) at ω,
 *  s = x + c·s1 − s2, power = s1² + s2² − c·s1·s2. A tone of amplitude A on its frequency gives (A·N/2)². */
inline void goertzelScalar(const float* x, int N, const float* c, int K, float* pw) {
    for (int g = 0; g < K; g += 4) {
        float s1[4] = { 0, 0, 0, 0 }, s2[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < N; ++i)
            for (int l = 0; l < 4; ++l) {
                volatile float p = c[g + l] * s1[l];
                const float s = x[i] + (p - s2[l]);
                s2[l] = s1[l]; s1[l] = s;
            }
        for (int l = 0; l < 4; ++l) {
            volatile float a = s1[l] * s1[l], b = s2[l] * s2[l], m = c[g + l] * s1[l];
            volatile float mm = m * s2[l];
            pw[g + l] = (a + b) - mm;
        }
    }
}
inline void goertzel(const float* x, int N, const float* c, int K, float* pw) {
#if Q65V_NEON || Q65V_SSE2
    using namespace q65v;
    for (int g = 0; g < K; g += 4) {
        const V cc = ld(c + g);
        V s1 = set1(0.f), s2 = set1(0.f);
        for (int i = 0; i < N; ++i) {
            const V s = add(set1(x[i]), sub(mul(cc, s1), s2));
            s2 = s1; s1 = s;
        }
        st(pw + g, sub(add(mul(s1, s1), mul(s2, s2)), mul(mul(cc, s1), s2)));
    }
#else
    goertzelScalar(x, N, c, K, pw);
#endif
}

/** Σ x² over a block, four lanes, (l0+l1)+(l2+l3) — the same on every path. N a multiple of 4 is not required. */
inline float energy(const float* x, int N) {
    volatile float l[4] = { 0, 0, 0, 0 };
    int i = 0;
    for (; i + 4 <= N; i += 4)
        for (int k = 0; k < 4; ++k) { volatile float p = x[i + k] * x[i + k]; l[k] = l[k] + p; }
    for (; i < N; ++i) { volatile float p = x[i] * x[i]; l[0] = l[0] + p; }
    return (l[0] + l[1]) + (l[2] + l[3]);
}

/** Vertex of the parabola through (−1, a), (0, b), (1, c): the offset in [−1, 1] where it peaks. */
inline double vertex(double a, double b, double c) {
    const double d = a - 2 * b + c;
    if (!(d < 0)) return 0;
    double v = 0.5 * (a - c) / d;
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

} // namespace tones
} // namespace vibe
