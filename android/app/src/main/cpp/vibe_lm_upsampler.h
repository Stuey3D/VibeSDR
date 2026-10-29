// vibe_lm_upsampler.h — the L/M polyphase upsampler to 48 kHz first written for DAB+ through FAAD2 (vibe_dab_faad.h,
// 2026-10-26), moved here unchanged (V12, 2026-10-29) so the in-process xHE-AAC decoder (vibe_xaac.h, DRM) can use it too:
// DRM xHE-AAC runs at rates like 19.2, 32 and 38.4 kHz, which no integer-ratio upsampler reaches 48 kHz from.
// ★ No codec in here and no licence beyond VibeSDR's own: it is our code, built on every server.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "vibedsp/simd_internal.h"

namespace vibedab {

/** ★ Four outputs of ONE polyphase branch whose windows start 2 inputs apart — the 2:3 resampler's natural block (every
 *  third output shares a branch, and three outputs consume two inputs). acc[q] = sum_m t[m] * x[2q + m], m < K (even K):
 *  each de-interleaving load gives the even-offset and odd-offset inputs of all four windows at once, so there is no
 *  horizontal sum and no tap reload per output. NEON (ARMv7 + AArch64) / SSE2 / scalar, as vibedsp. */
static inline void dabDot4Stride2(const float* t, const float* x, int K, float* o4) {
#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
    // ★ four taps per vector load, applied by lane (vmla by scalar lane) — no scalar tap loads in the loop (K % 4 == 0)
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
    for (int m = 0; m < K; m += 4) {
        const float32x4_t tq = vld1q_f32(t + m);
        const float32x2_t lo = vget_low_f32(tq), hi = vget_high_f32(tq);
        const float32x4x2_t v0 = vld2q_f32(x + m), v1 = vld2q_f32(x + m + 2);
        a0 = vmlaq_lane_f32(a0, v0.val[0], lo, 0);
        a1 = vmlaq_lane_f32(a1, v0.val[1], lo, 1);
        a0 = vmlaq_lane_f32(a0, v1.val[0], hi, 0);
        a1 = vmlaq_lane_f32(a1, v1.val[1], hi, 1);
    }
    vst1q_f32(o4, vaddq_f32(a0, a1));
#elif (defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)) && !defined(VIBE_FORCE_SCALAR)
    __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
    for (int m = 0; m < K; m += 2) {
        const __m128 p = _mm_loadu_ps(x + m), q = _mm_loadu_ps(x + m + 4);
        const __m128 ev = _mm_shuffle_ps(p, q, _MM_SHUFFLE(2, 0, 2, 0)), od = _mm_shuffle_ps(p, q, _MM_SHUFFLE(3, 1, 3, 1));
        a0 = _mm_add_ps(a0, _mm_mul_ps(ev, _mm_set1_ps(t[m])));
        a1 = _mm_add_ps(a1, _mm_mul_ps(od, _mm_set1_ps(t[m + 1])));
    }
    _mm_storeu_ps(o4, _mm_add_ps(a0, a1));
#else
    for (int q = 0; q < 4; ++q) {
        float a0 = 0, a1 = 0;
        for (int m = 0; m < K; m += 2) { a0 += t[m] * x[2 * q + m]; a1 += t[m + 1] * x[2 * q + m + 1]; }
        o4[q] = a0 + a1;
    }
#endif
}

/** ★ 32 kHz → 48 kHz (and any other small L/M), stereo, polyphase windowed-sinc. HE-AAC on a 16 kHz core decodes to
 *  32 kHz and its SBR band reaches ~15 kHz, so the filter must pass that — a generic audio resampler cut at 0.9 × ¼ of
 *  the up-sampled rate would dull the top octave.
 *  ★★ 32 TAPS A BRANCH, NOT 64 (2026-10-26 — the Pi 2's "FAAD2 · 11.87 % of a core"; on the Mac this resampler was a
 *     THIRD of FAAD2's whole cost). Kaiser β 7.0, cut-off 0.975 of the input's Nyquist (15.6 kHz at 32 kHz in), designed
 *     and checked numerically (docs/v12/DRM-AUDIO-ROUTES.md "DAB+ on its own thread"):
 *       passband flat within ±0.1 dB to 13.8 kHz, −2.4 dB at 15 kHz, ≥ 70 dB down from 17.85 kHz (the image of 14.15 kHz).
 *     FFmpeg's own resampler — what today's default path puts this audio through (swr: 32 taps, Kaiser β 9, cut-off
 *     0.97) — is flat only to 13.5 kHz and reaches −60 dB only at 18.1 kHz, so this is never worse than that path; the
 *     64-tap one it replaces kept more rejection between 16 and 17.8 kHz (images of 14.2–16 kHz SBR, ≤ −25 dB here).
 *  ★★ BOTH CHANNELS PER TAP LOAD. The two histories sit in one buffer, R a fixed distance after L, so vibedsp::dotReal2
 *     (NEON / SSE2 / scalar) runs L and R off each tap vector loaded once. A mono stream (no PS, one channel) is filtered
 *     once and copied. No allocation per call: the histories are flat arrays compacted only when they fill. */
class DabUpsampler {
public:
    static constexpr int kP = 32;                         ///< taps per polyphase branch
    void configure(int inRate, int outRate) {
        if (inRate == in_ && outRate == out_) return;
        in_ = inRate; out_ = outRate;
        int a = inRate, b = outRate; while (b) { const int t = a % b; a = b; b = t; }
        L_ = outRate / a; M_ = inRate / a;
        const int N = L_ * kP;
        const double fc = 0.5 * std::min(inRate, outRate) * kCut / (double(inRate) * L_);   // cycles per up-sample
        std::vector<double> h(static_cast<size_t>(N));
        double sum = 0;
        for (int n = 0; n < N; ++n) {
            const double x = n - (N - 1) / 2.0;
            const double s = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            const double r = 2.0 * n / (N - 1) - 1.0;
            h[size_t(n)] = s * bessel0(kBeta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(kBeta);
            sum += h[size_t(n)];
        }
        rb_.assign(size_t(N), 0.0f);
        for (int b2 = 0; b2 < L_; ++b2)
            for (int m = 0; m < kP; ++m) rb_[size_t(b2) * kP + size_t(m)] = float(h[size_t(b2 + (kP - 1 - m) * L_)] * L_ / sum);
        reset();
    }
    void reset() { std::fill(buf_, buf_ + 2 * kCap, 0.0f); s_ = 0; w_ = kP - 1; base_ = 0; br_ = 0; }
    /** Interleaved stereo in (n frames) → interleaved stereo appended to out. Passes straight through when in == out.
     *  `mono`: L and R are the same signal (filter once). */
    void process(const float* in, size_t n, std::vector<float>& out, bool mono = false) {
        if (L_ == M_) { out.insert(out.end(), in, in + 2 * n); return; }
        while (n) {
            if (w_ == kCap) compact_();
            const size_t take = std::min(n, kCap - w_);
            float* l = buf_ + w_; float* r = buf_ + kCap + w_;
            for (size_t i = 0; i < take; ++i) { l[i] = in[2 * i]; r[i] = in[2 * i + 1]; }
            w_ += take; in += 2 * take; n -= take;
            run_(out, mono);
        }
    }
    int outRate() const { return out_; }
private:
    static constexpr size_t kCap = 8192;                  ///< history per channel; a unit is ≤ 2048 frames
    static constexpr double kBeta = 7.0, kCut = 0.975;
    static double bessel0(double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
    /** Every output whose window [base, base + kP) is filled: base_ (inputs from s_) and br_ (the polyphase branch) step
     *  by M per output — ★ NO DIVISION. The first version kept one 64-bit phase and divided it by L twice per output; on
     *  32-bit ARM that is a library call (__aeabi_ldivmod), and it made this resampler cost MORE than FAAD2's whole
     *  decode on the Pi 2 (qemu-arm: 1.3 G of 2.15 G guest instructions for 59 s of programme). */
    void run_(std::vector<float>& out, bool mono) {
        const size_t have = w_ - s_;
        /* ★★ 2:3 (32 → 48 kHz, the HE-AAC v2 case): twelve outputs at a time — the three branches, four outputs each, by
         *  dabDot4Stride2 (windows 2 inputs apart); a block needs base_ + 7 + kP inputs (the furthest window starts at
         *  most 1 + 6 past base_). Same taps and inputs as the one-at-a-time path below. */
        if (L_ == 3 && M_ == 2) {
            while (base_ + 7 + size_t(kP) <= have) {
                const size_t o0 = out.size();
                out.resize(o0 + 24);
                float* o = out.data() + o0;
                size_t bb = base_; int rr = br_;
                for (int k = 0; k < 3; ++k) {
                    const float* t = &rb_[size_t(rr) * kP];
                    alignas(16) float l4[4], r4[4];
                    dabDot4Stride2(t, buf_ + s_ + bb, kP, l4);
                    if (mono) { for (int q = 0; q < 4; ++q) r4[q] = l4[q]; }
                    else dabDot4Stride2(t, buf_ + kCap + s_ + bb, kP, r4);
                    for (int q = 0; q < 4; ++q) { o[2 * (k + 3 * q)] = l4[q]; o[2 * (k + 3 * q) + 1] = r4[q]; }
                    rr += 2; if (rr >= 3) { rr -= 3; ++bb; }
                }
                base_ += 8;                                // twelve outputs = eight inputs; the branch is back where it began
            }
        }
        // one output at a time (other ratios, and the end of each block of input)
        while (base_ + size_t(kP) <= have) {
            const float* t = &rb_[size_t(br_) * kP];
            const float* x = buf_ + s_ + base_;
            float l, r;
            if (mono) { l = r = vibedsp::dotReal(t, x, kP); }
            else vibedsp::dotReal2(t, x, kP, int(kCap), l, r);   // ★ R is kCap after L
            out.push_back(l); out.push_back(r);
            br_ += M_;
            while (br_ >= L_) { br_ -= L_; ++base_; }
        }
        s_ += base_; base_ = 0;                            // inputs no output will need again
    }
    void compact_() {
        const size_t keep = w_ - s_;
        std::memmove(buf_, buf_ + s_, keep * sizeof(float));
        std::memmove(buf_ + kCap, buf_ + kCap + s_, keep * sizeof(float));
        s_ = 0; w_ = keep;
    }
    int in_ = 0, out_ = 0, L_ = 1, M_ = 1, br_ = 0;
    size_t s_ = 0, w_ = kP - 1, base_ = 0;
    std::vector<float> rb_;
    float buf_[2 * kCap] = {};
};

}  // namespace vibedab
