// vibe_dab_ddc.h — WIDE-RATE DAB: one multiplex taken digitally out of a wide capture (V12, 2026-11-04).
//
// ★★★ WHY. A radio that captures several MHz (USRP B2xx, RSP, HackRF, Airspy R2, the RTL at 2.4) was FORCED down to
//     2.048 MS/s for DAB: every entry, and on a USRP every block change, re-rated the radio — a full stop, a new master
//     clock (the AD9361 re-calibrates), a new streamer, a rebuilt DSP chain — twice, because the USRP has no 2.048 in
//     its list (Stuart, 2026-11-03: "the 2.4MHz sample rate killed it after a few tunes"). And the owner's wide view of
//     several multiplexes was thrown away the moment DAB started. Here the radio KEEPS its rate and centre, and the
//     decoder's 2.048 MS/s block is cut out of the capture in software: mix the block to DC, halve the rate while the
//     block still fits, then one rational polyphase step to exactly 2.048 MS/s. A block change inside the span is a
//     new mixer frequency: no hardware moves at all.
//
// ★★ THE STAGES, and why each is shaped as it is:
//   1. NCO MIX — four phasors a lane apart, stepped by e^(j4w), re-seeded in double every block so float rounding
//      never accumulates (the phase error stays at the 1e-7 level for any run length).
//   2. HALF-BAND ÷2, repeated while the next rate would still be >= 2.4 MS/s. A half-band has every other tap zero and
//      a centre tap of ½, so it is run as its two polyphase branches: the centre tap on the even samples, a dense
//      symmetric FIR on the odd ones — half the multiplies of the plain filter and contiguous SIMD loads. Each stage
//      only has to protect what will alias into the block LATER (fs/2 − 0.8 MHz up), so the early stages are short.
//   3. RATIONAL L/M to 2.048 MS/s — exact (2.4 → 64/75, 4 → 64/125, 3.84 → 8/15, 2.5 → 512/625 …), so the symbol
//      timing cannot drift (an arbitrary-ratio resampler would reintroduce exactly that). Kaiser-windowed, passing
//      ±0.80 MHz and stopping by 1.248 MHz — everything that would fold into the ensemble at 2.048.
// ★ The passband is the ENSEMBLE (±768 kHz) plus 32 kHz: a block tuned dead on needs no more, and unlike the old
//   2.4 path this one is centred by construction (the mix puts the block centre at DC exactly).
// ★★ SIMD: the mixer and both dot-product kernels have NEON (armv7 + arm64) and SSE2 paths; the scalar path sums in
//    the same four lanes in the same order, so all three give the same output (bit-for-bit without FMA contraction).
// ★ Memory: bounded and small — the filters (a few thousand floats) and one block of history per stage.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#include "vibedsp/neon_compat.h"
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace vibedab {

namespace ddc {

/** ★ The scalar reference, always compiled (test-dab-ddc compares the vector paths against it): two banks of four
 *  lanes (even groups, odd groups), summed in the order the vector paths sum. */
inline void dot4Scalar(const float* x, const float* hd, int n4, float& re, float& im) {
    float a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
    int k = 0;
    for (; k + 2 <= n4; k += 2) for (int q = 0; q < 4; ++q) { a[q] += x[4 * k + q] * hd[4 * k + q]; b[q] += x[4 * k + 4 + q] * hd[4 * k + 4 + q]; }
    if (k < n4) for (int q = 0; q < 4; ++q) a[q] += x[4 * k + q] * hd[4 * k + q];
    float l[4]; for (int q = 0; q < 4; ++q) l[q] = a[q] + b[q];
    re = l[0] + l[2]; im = l[1] + l[3];
}
/** Σ x·hd over `n4` groups of four floats (x interleaved complex, hd the taps duplicated re/im) → re, im.
 *  ★ Two accumulators: one chain of dependent multiply-adds was latency-bound (~4 cycles a link), two halve it. */
inline void dot4(const float* x, const float* hd, int n4, float& re, float& im) {
#if defined(__ARM_NEON)
    float32x4_t a = vdupq_n_f32(0.0f), b = vdupq_n_f32(0.0f);
    int k = 0;
    for (; k + 2 <= n4; k += 2) {
        a = vmlaq_f32(a, vld1q_f32(x + 4 * k), vld1q_f32(hd + 4 * k));
        b = vmlaq_f32(b, vld1q_f32(x + 4 * k + 4), vld1q_f32(hd + 4 * k + 4));
    }
    if (k < n4) a = vmlaq_f32(a, vld1q_f32(x + 4 * k), vld1q_f32(hd + 4 * k));
    float l[4]; vst1q_f32(l, vaddq_f32(a, b));
    re = l[0] + l[2]; im = l[1] + l[3];
#elif defined(__SSE2__)
    __m128 a = _mm_setzero_ps(), b = _mm_setzero_ps();
    int k = 0;
    for (; k + 2 <= n4; k += 2) {
        a = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(x + 4 * k), _mm_loadu_ps(hd + 4 * k)));
        b = _mm_add_ps(b, _mm_mul_ps(_mm_loadu_ps(x + 4 * k + 4), _mm_loadu_ps(hd + 4 * k + 4)));
    }
    if (k < n4) a = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(x + 4 * k), _mm_loadu_ps(hd + 4 * k)));
    float l[4]; _mm_storeu_ps(l, _mm_add_ps(a, b));
    re = l[0] + l[2]; im = l[1] + l[3];
#else
    dot4Scalar(x, hd, n4, re, im);
#endif
}

inline double bessel0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 50; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}
inline double kaiserBeta(double attDb) {
    return attDb > 50 ? 0.1102 * (attDb - 8.7) : attDb >= 21 ? 0.5842 * std::pow(attDb - 21, 0.4) + 0.07886 * (attDb - 21) : 0.0;
}
/** Windowed-sinc low-pass, `n` taps, cut-off `fc` in cycles per sample, Kaiser β, scaled to DC gain `gain`. */
inline std::vector<double> kaiserLowpass(int n, double fc, double beta, double gain) {
    std::vector<double> h(static_cast<size_t>(n));
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double t = i - (n - 1) * 0.5;
        const double s = std::fabs(t) < 1e-12 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
        const double r = n > 1 ? 2.0 * i / (n - 1) - 1.0 : 0.0;
        h[size_t(i)] = s * bessel0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(beta);
        sum += h[size_t(i)];
    }
    for (auto& v : h) v *= gain / sum;
    return h;
}

/** Stage 1 — complex NCO: x · e^(-j·2π·f·n/fs), in place.
 *  ★ Four phasors, one per lane, for each ABSOLUTE group of four samples, stepped by e^(j4w) per group and re-seeded
 *    in double at every absolute multiple of kSeed samples — so the sequence of phasors depends only on the sample
 *    index, never on how the input was chopped. A part group (a call that starts or ends mid-group) goes through the
 *    SAME vector multiply on a zero-padded copy, so every sample takes the same arithmetic whichever way it arrives. */
class Mixer {
public:
    static constexpr uint64_t kSeed = 4096;
    void set(double hz, double rate) { w_ = -2.0 * M_PI * hz / rate; on_ = std::fabs(hz) > 0.01; reset(); }
    bool on() const { return on_; }
    void reset() { n_ = 0; lane_ = 0; seed_(); }
    void run(float* x, size_t n) {
        if (!on_) return;
        size_t i = 0;
        while (i < n) {
            if (lane_ == 0 && n_ % kSeed == 0) seed_();
            if (lane_ != 0 || n - i < 4) {
                // ★ a part group: through the vector multiply on a padded copy, then back
                float g[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                const int take = int(std::min<size_t>(size_t(4 - lane_), n - i));
                std::memcpy(g + 2 * lane_, x + 2 * i, sizeof(float) * size_t(2 * take));
                mulGroups_(g, 1);
                std::memcpy(x + 2 * i, g + 2 * lane_, sizeof(float) * size_t(2 * take));
                lane_ += take; i += size_t(take); n_ += uint64_t(take);
                if (lane_ == 4) { rotate_(1); lane_ = 0; }
                continue;
            }
            const size_t toSeed = size_t(kSeed - n_ % kSeed) / 4;
            const size_t groups = std::min((n - i) / 4, toSeed);
            mulGroups_(x + 2 * i, groups, /*rotateEach=*/true);
            i += 4 * groups; n_ += 4 * groups;
        }
    }
private:
    void seed_() {
        const double ph0 = std::fmod(w_ * double(n_), 2 * M_PI);
        for (int q = 0; q < 4; ++q) { pr_[q] = float(std::cos(ph0 + w_ * q)); pi_[q] = float(std::sin(ph0 + w_ * q)); }
        sr_ = float(std::cos(4 * w_)); si_ = float(std::sin(4 * w_));
    }
    /** Multiply `groups` groups of four complex samples by the lane phasors, rotating after each when asked. */
    void mulGroups_(float* p, size_t groups, bool rotateEach = false) {
#if defined(__ARM_NEON)
        float32x4_t vr = vld1q_f32(pr_), vi = vld1q_f32(pi_);
        const float32x4_t vsr = vdupq_n_f32(sr_), vsi = vdupq_n_f32(si_);
        for (size_t k = 0; k < groups; ++k) {
            const float32x4x2_t a = vld2q_f32(p + 8 * k);
            float32x4x2_t o;
            o.val[0] = vmlsq_f32(vmulq_f32(a.val[0], vr), a.val[1], vi);
            o.val[1] = vmlaq_f32(vmulq_f32(a.val[0], vi), a.val[1], vr);
            vst2q_f32(p + 8 * k, o);
            if (rotateEach) { const float32x4_t nr = vmlsq_f32(vmulq_f32(vr, vsr), vi, vsi); vi = vmlaq_f32(vmulq_f32(vr, vsi), vi, vsr); vr = nr; }
        }
        vst1q_f32(pr_, vr); vst1q_f32(pi_, vi);
#elif defined(__SSE2__)
        __m128 vr = _mm_loadu_ps(pr_), vi = _mm_loadu_ps(pi_);
        const __m128 vsr = _mm_set1_ps(sr_), vsi = _mm_set1_ps(si_);
        for (size_t k = 0; k < groups; ++k) {
            const __m128 a0 = _mm_loadu_ps(p + 8 * k), a1 = _mm_loadu_ps(p + 8 * k + 4);
            const __m128 ar = _mm_shuffle_ps(a0, a1, _MM_SHUFFLE(2, 0, 2, 0)), ai = _mm_shuffle_ps(a0, a1, _MM_SHUFFLE(3, 1, 3, 1));
            const __m128 orr = _mm_sub_ps(_mm_mul_ps(ar, vr), _mm_mul_ps(ai, vi));
            const __m128 oi = _mm_add_ps(_mm_mul_ps(ar, vi), _mm_mul_ps(ai, vr));
            _mm_storeu_ps(p + 8 * k, _mm_unpacklo_ps(orr, oi));
            _mm_storeu_ps(p + 8 * k + 4, _mm_unpackhi_ps(orr, oi));
            if (rotateEach) { const __m128 nr = _mm_sub_ps(_mm_mul_ps(vr, vsr), _mm_mul_ps(vi, vsi)); vi = _mm_add_ps(_mm_mul_ps(vr, vsi), _mm_mul_ps(vi, vsr)); vr = nr; }
        }
        _mm_storeu_ps(pr_, vr); _mm_storeu_ps(pi_, vi);
#else
        for (size_t k = 0; k < groups; ++k) {
            for (int q = 0; q < 4; ++q) {
                const float ar = p[8 * k + 2 * q], ai = p[8 * k + 2 * q + 1];
                p[8 * k + 2 * q] = ar * pr_[q] - ai * pi_[q];
                p[8 * k + 2 * q + 1] = ar * pi_[q] + ai * pr_[q];
            }
            if (rotateEach) rotate_(1);
        }
#endif
    }
    void rotate_(int) {
#if defined(__ARM_NEON)
        const float32x4_t vr = vld1q_f32(pr_), vi = vld1q_f32(pi_), vsr = vdupq_n_f32(sr_), vsi = vdupq_n_f32(si_);
        vst1q_f32(pr_, vmlsq_f32(vmulq_f32(vr, vsr), vi, vsi)); vst1q_f32(pi_, vmlaq_f32(vmulq_f32(vr, vsi), vi, vsr));
#elif defined(__SSE2__)
        const __m128 vr = _mm_loadu_ps(pr_), vi = _mm_loadu_ps(pi_), vsr = _mm_set1_ps(sr_), vsi = _mm_set1_ps(si_);
        _mm_storeu_ps(pr_, _mm_sub_ps(_mm_mul_ps(vr, vsr), _mm_mul_ps(vi, vsi))); _mm_storeu_ps(pi_, _mm_add_ps(_mm_mul_ps(vr, vsi), _mm_mul_ps(vi, vsr)));
#else
        for (int q = 0; q < 4; ++q) { const float nr = pr_[q] * sr_ - pi_[q] * si_; pi_[q] = pr_[q] * si_ + pi_[q] * sr_; pr_[q] = nr; }
#endif
    }
    double w_ = 0; uint64_t n_ = 0; int lane_ = 0; bool on_ = false;
    float pr_[4] = {1, 1, 1, 1}, pi_[4] = {0, 0, 0, 0}, sr_ = 1, si_ = 0;
};

/** Stage 2 — half-band decimate by 2, as its two polyphase branches (see the header).
 *  With the full filter h[0..n−1], n = 4K+3, centre c = 2K+1: the non-zero taps are h[c] (= ½) and h[c ± odd]. For an
 *  output centred on input x(2m) (an EVEN sample): y[m] = h[c]·x(2m) + Σ_off h[c+off]·x(2m+off), off = ±1, ±3 … ±c.
 *  x(2m+off) is odd sample b[m + (off−1)/2] (b[q] = x(2q+1)), so the odd taps are ONE contiguous window
 *  b[m−kv … m+kv−1], kv = K+1, and the centre tap is e[m] (e[q] = x(2q)). */
class HalfBand {
public:
    void design(double inRate, double passHz, double attDb) {
        const double tw = (inRate / 2 - 2 * passHz) / inRate;            // transition, cycles/sample
        const double beta = kaiserBeta(attDb);
        int n = int(std::ceil((attDb - 7.95) / (14.36 * std::max(tw, 0.01)))) + 1;
        n = std::max(n, 7);
        while ((n - 3) % 4) ++n;                                          // 4K+3
        const std::vector<double> h = kaiserLowpass(n, 0.25, beta, 1.0);
        const int c = (n - 1) / 2;
        kv_ = (n + 1) / 4;                                                // K+1 odd taps each side
        taps_ = 2 * kv_;
        hd_.assign(size_t(2 * taps_), 0.0f);
        for (int j = 0; j < taps_; ++j) {
            const int off = 2 * j - (2 * kv_ - 1);                         // −(2kv−1) … +(2kv−1), odd
            const float v = float(h[size_t(c + off)]);
            hd_[size_t(2 * j)] = v; hd_[size_t(2 * j + 1)] = v;
        }
        centreTap_ = float(h[size_t(c)]);
        odd_.assign(size_t(2 * (taps_ + kBlock)), 0.0f);
        even_.assign(size_t(2 * (kBlock + 1)), 0.0f);
        reset();
    }
    int taps() const { return taps_ + 1; }   // the multiplies per output, centre included
    void reset() {
        std::fill(odd_.begin(), odd_.end(), 0.0f); std::fill(even_.begin(), even_.end(), 0.0f);
        oddCnt_ = kv_;   // b[−kv … −1] = 0: the first output's past
        evenCnt_ = 0; phase_ = 0;
    }
    /** in: n complex; appends about n/2 complex to out. */
    void run(const float* in, size_t n, std::vector<float>& out) {
        size_t i = 0;
        while (i < n) {
            // deinterleave until either branch is full (each holds at most a block beyond its history)
            if (i < n && phase_ && oddCnt_ < taps_ + kBlock) {   // finish a pair begun in the last call
                odd_[size_t(2 * oddCnt_)] = in[2 * i]; odd_[size_t(2 * oddCnt_ + 1)] = in[2 * i + 1]; ++oddCnt_; phase_ = 0; ++i;
            }
            {   // ★ whole pairs, no branch per sample: even → e, odd → b
                const size_t pairs = phase_ ? 0 : std::min({ (n - i) / 2, size_t(taps_ + kBlock - oddCnt_), size_t(kBlock + 1 - evenCnt_) });
                const float* s = in + 2 * i;
                float* e = &even_[size_t(2 * evenCnt_)]; float* o = &odd_[size_t(2 * oddCnt_)];
                for (size_t q = 0; q < pairs; ++q) {
                    e[2 * q] = s[4 * q]; e[2 * q + 1] = s[4 * q + 1];
                    o[2 * q] = s[4 * q + 2]; o[2 * q + 1] = s[4 * q + 3];
                }
                evenCnt_ += int(pairs); oddCnt_ += int(pairs); i += 2 * pairs;
            }
            if (i + 1 == n && !phase_ && evenCnt_ < kBlock + 1) {   // a lone even sample at the end
                even_[size_t(2 * evenCnt_)] = in[2 * i]; even_[size_t(2 * evenCnt_ + 1)] = in[2 * i + 1]; ++evenCnt_; phase_ = 1; ++i;
            }
            const int nOut = std::min(oddCnt_ - (taps_ - 1), evenCnt_);
            if (nOut <= 0) { if (i >= n) break; continue; }
            const size_t base = out.size();
            out.resize(base + size_t(2 * nOut));
            float* y = out.data() + base;
            for (int m = 0; m < nOut; ++m) {
                float re, im;
                dot4(&odd_[size_t(2 * m)], hd_.data(), taps_ / 2, re, im);
                y[2 * m]     = re + centreTap_ * even_[size_t(2 * m)];
                y[2 * m + 1] = im + centreTap_ * even_[size_t(2 * m + 1)];
            }
            std::memmove(odd_.data(), odd_.data() + 2 * nOut, sizeof(float) * size_t(2 * (oddCnt_ - nOut)));
            oddCnt_ -= nOut;
            std::memmove(even_.data(), even_.data() + 2 * nOut, sizeof(float) * size_t(2 * (evenCnt_ - nOut)));
            evenCnt_ -= nOut;
        }
    }
private:
    static constexpr int kBlock = 2048;
    std::vector<float> hd_, odd_, even_;
    float centreTap_ = 0.5f;
    int kv_ = 1, taps_ = 2, oddCnt_ = 0, evenCnt_ = 0, phase_ = 0;
};

/** Stage 3 — exact rational L/M polyphase resampler, complex in, complex out.
 *  Output k sits at input time k·M/L: n_k = ⌊kM/L⌋, branch p_k = kM mod L, y[k] = Σ_t h[p_k + tL]·x[n_k − t].
 *  The taps are stored oldest-first per branch, so each output is ONE contiguous dot over x[n_k−T+1 … n_k]. */
class Rational {
public:
    bool design(double inRate, double outRate, double passHz, double stopHz, double attDb, int maxL = 1024) {
        const long long a = std::llround(outRate), b = std::llround(inRate);
        const long long g = std::gcd(a, b);
        long long L = a / g, M = b / g;
        if (L > maxL) {   // ★ the best ratio with L <= maxL: its error is then a crystal's (well under 1 ppm)
            double best = 1e9; long long bl = 1, bm = 1;
            for (long long l = 1; l <= maxL; ++l) {
                const long long m = std::llround(double(l) * inRate / outRate);
                if (m < 1) continue;
                const double e = std::fabs(double(l) / double(m) - outRate / inRate);
                if (e < best) { best = e; bl = l; bm = m; }
            }
            L = bl; M = bm;
        }
        L_ = int(L); M_ = int(M);
        const double fsP = inRate * double(L_);
        const double tw = (stopHz - passHz) / fsP;
        int n = int(std::ceil((attDb - 7.95) / (14.36 * tw))) + 1;
        T_ = (n + L_ - 1) / L_;
        T_ = std::max(2, (T_ + 1) & ~1);                                  // pairs, for dot4
        n = T_ * L_;
        const std::vector<double> h = kaiserLowpass(n, 0.5 * (passHz + stopHz) / fsP, kaiserBeta(attDb), double(L_));
        hd_.assign(size_t(L_) * size_t(2 * T_), 0.0f);
        for (int p = 0; p < L_; ++p)
            for (int t = 0; t < T_; ++t) {
                const float v = float(h[size_t(p + (T_ - 1 - t) * L_)]);
                hd_[size_t(p) * size_t(2 * T_) + size_t(2 * t)] = v;
                hd_[size_t(p) * size_t(2 * T_) + size_t(2 * t + 1)] = v;
            }
        hist_.assign(size_t(2 * (T_ + kBlock)), 0.0f);
        reset();
        return true;
    }
    int L() const { return L_; }
    int M() const { return M_; }
    int taps() const { return T_; }
    void reset() { std::fill(hist_.begin(), hist_.end(), 0.0f); cnt_ = T_ - 1; pos_ = 0; ph_ = 0; }
    void run(const float* in, size_t n, std::vector<float>& out) {
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(size_t(T_ + kBlock - cnt_), n - i);
            std::memcpy(&hist_[size_t(2 * cnt_)], in + 2 * i, sizeof(float) * 2 * take);
            cnt_ += int(take); i += take;
            const size_t base = out.size();
            out.resize(base + size_t(2 * ((long long)(cnt_) * L_ / M_ + 2)));
            float* y = out.data() + base; size_t w = 0;
            while (pos_ + T_ <= cnt_) {
                float re, im;
                dot4(&hist_[size_t(2 * pos_)], &hd_[size_t(ph_) * size_t(2 * T_)], T_ / 2, re, im);
                y[2 * w] = re; y[2 * w + 1] = im; ++w;
                ph_ += M_; pos_ += ph_ / L_; ph_ %= L_;
            }
            out.resize(base + 2 * w);
            // ★ drop what no future output needs: everything before the next window's start
            const int drop = std::min(pos_, cnt_);
            std::memmove(hist_.data(), hist_.data() + 2 * drop, sizeof(float) * size_t(2 * (cnt_ - drop)));
            cnt_ -= drop; pos_ -= drop;
        }
    }
private:
    static constexpr int kBlock = 4096;
    std::vector<float> hd_, hist_;
    int L_ = 1, M_ = 1, T_ = 2, cnt_ = 0, pos_ = 0, ph_ = 0;
};

}  // namespace ddc

/** ★ The whole front end: any capture rate ≥ 2.048 MS/s, any block offset inside it → 2.048 MS/s at the block centre. */
class DabDdc {
public:
    static constexpr double kOutHz   = 2048000.0;
    static constexpr double kPassHz  = 800000.0;     // the ensemble (±768 kHz) and 32 kHz
    static constexpr double kStopHz  = kOutHz - kPassHz;   // 1.248 MHz: what folds into the ensemble at 2.048
    static constexpr double kMidMin  = 2400000.0;    // halve while the next rate stays at or above this
    static constexpr double kAttDb   = 80.0;

    /** @param inRate the capture's rate; @param shiftHz the block centre's offset from the capture's centre.
     *  @return false when the block does not fit (the rate is below 2.048, or the block runs off the capture). */
    bool configure(double inRate, double shiftHz) {
        ok_ = false;
        if (!(inRate >= kOutHz - 1.0)) return false;
        if (std::fabs(shiftHz) + kPassHz > inRate / 2) return false;
        inRate_ = inRate; shift_ = shiftHz;
        mix_.set(shiftHz, inRate);
        hb_.clear();
        double r = inRate;
        while (r / 2 >= kMidMin - 1.0) { hb_.emplace_back(); hb_.back().design(r, kPassHz, kAttDb); r /= 2; }
        midRate_ = r;
        passthrough_ = std::fabs(r - kOutHz) < 1.0;
        if (!passthrough_) rat_.design(r, kOutHz, kPassHz, kStopHz, kAttDb);
        bufA_.clear(); bufB_.clear();
        ok_ = true;
        return true;
    }
    bool ok() const { return ok_; }
    double inRate() const { return inRate_; }
    double shiftHz() const { return shift_; }
    int halfBands() const { return int(hb_.size()); }
    double midRate() const { return midRate_; }
    int ratL() const { return passthrough_ ? 1 : rat_.L(); }
    int ratM() const { return passthrough_ ? 1 : rat_.M(); }
    int ratTaps() const { return passthrough_ ? 0 : rat_.taps(); }
    /** The rate actually delivered: 2.048 MS/s exactly, or within 1 ppm where L/M had to be approximated (12.5, 25). */
    double outRateActual() const { return passthrough_ ? midRate_ : midRate_ * double(rat_.L()) / double(rat_.M()); }
    int halfBandTaps(int i) const { return hb_[size_t(i)].taps(); }
    void reset() { mix_.reset(); for (auto& h : hb_) h.reset(); rat_.reset(); }
    /** Interleaved complex in at inRate; APPENDS interleaved complex out at 2.048 MS/s. `in` is not modified. */
    void process(const float* in, size_t n, std::vector<float>& out) {
        if (!ok_ || !n) return;
        bufA_.assign(in, in + 2 * n);
        mix_.run(bufA_.data(), n);
        std::vector<float>* cur = &bufA_; std::vector<float>* nxt = &bufB_;
        for (auto& h : hb_) { nxt->clear(); h.run(cur->data(), cur->size() / 2, *nxt); std::swap(cur, nxt); }
        if (passthrough_) out.insert(out.end(), cur->begin(), cur->end());
        else rat_.run(cur->data(), cur->size() / 2, out);
    }
private:
    ddc::Mixer mix_;
    std::vector<ddc::HalfBand> hb_;
    ddc::Rational rat_;
    std::vector<float> bufA_, bufB_;
    double inRate_ = 0, shift_ = 0, midRate_ = 0;
    bool ok_ = false, passthrough_ = false;
};

}  // namespace vibedab
