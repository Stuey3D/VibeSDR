// balloon_dsp.h — V12 balloons: the shared front end (docs/v12/BALLOONS.md).
//
//  • Decim4 — the listener's 48 kHz audio to 12 kHz: a 48-tap windowed-sinc lowpass (cut 5 kHz), one output per four
//    inputs. ★ The one hot loop every balloon decoder shares: NEON / SSE2 four lanes at a time, and a scalar path that sums
//    in the same four lanes and the same order (test-balloon checks the two agree).
//  • Spectrum — a 4096-point power spectrum of the 12 kHz audio (2.9 Hz bins, 0.34 s), smoothed over about a second, made
//    every 2048 samples: ONE FFT that both tone finders read (the RTTY pair and the Horus four).
//  • findPair / findFour — the RTTY mark and space, and Horus Binary's four equally spaced tones, from that spectrum.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>
#if defined(__ARM_NEON) && !defined(VIBE_BALLOON_SCALAR)
#include <arm_neon.h>
#define VIBE_BALLOON_NEON 1
#elif defined(__SSE2__) && !defined(VIBE_BALLOON_SCALAR)
#include <emmintrin.h>
#define VIBE_BALLOON_SSE2 1
#endif

namespace vibe {
namespace balloon {

/** Which inner loop this build runs — for the benchmark's label and the test's report. */
inline const char* simdName() {
#if defined(VIBE_BALLOON_NEON)
    return "NEON";
#elif defined(VIBE_BALLOON_SSE2)
    return "SSE2";
#else
    return "scalar";
#endif
}

/** 48 → 12 kHz. Bounded: a 48-sample history and the caller's output vector. */
class Decim4 {
public:
    static constexpr int kTaps = 48;
    Decim4() {
        // windowed sinc, cut 5 kHz of 48 kHz, Blackman window, unit DC gain
        double sum = 0;
        for (int i = 0; i < kTaps; ++i) {
            const double m = i - (kTaps - 1) / 2.0, fc = 5000.0 / 48000.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (kTaps - 1)) + 0.08 * std::cos(4 * M_PI * i / (kTaps - 1));
            h_[i] = (float)(s * w); sum += s * w;
        }
        for (float& v : h_) v = (float)(v / sum);
        std::memset(hist_, 0, sizeof hist_);
    }
    /** Appends floor((phase + n) / 4) samples to `out`. */
    void process(const float* x, int n, std::vector<float>& out) {
        for (int i = 0; i < n; ++i) {
            // a doubled history so the newest kTaps samples are always contiguous: hist_[pos_ … pos_ + kTaps)
            hist_[pos_] = hist_[pos_ + kTaps] = x[i];
            if (++pos_ == kTaps) pos_ = 0;
            if (++phase_ == 4) { phase_ = 0; out.push_back(dot(hist_ + pos_, h_)); }
        }
    }
    /** ★ The dot product, newest sample last — four lanes, summed (l0 + l1) + (l2 + l3) in every path. */
    static inline float dotScalar(const float* x, const float* h) {
        volatile float l0 = x[0] * h[0], l1 = x[1] * h[1], l2 = x[2] * h[2], l3 = x[3] * h[3];   // ★ volatile: no fused multiply-add,
        for (int t = 4; t < kTaps; t += 4) {                                                       //   so the lanes round as SIMD's do
            volatile float p0 = x[t] * h[t], p1 = x[t + 1] * h[t + 1], p2 = x[t + 2] * h[t + 2], p3 = x[t + 3] * h[t + 3];
            l0 = l0 + p0; l1 = l1 + p1; l2 = l2 + p2; l3 = l3 + p3;
        }
        return (l0 + l1) + (l2 + l3);
    }
    static inline float dot(const float* x, const float* h) {
#if defined(VIBE_BALLOON_NEON)
        float32x4_t a = vmulq_f32(vld1q_f32(x), vld1q_f32(h));
        for (int t = 4; t < kTaps; t += 4) a = vaddq_f32(a, vmulq_f32(vld1q_f32(x + t), vld1q_f32(h + t)));
        float l[4]; vst1q_f32(l, a);
        return (l[0] + l[1]) + (l[2] + l[3]);
#elif defined(VIBE_BALLOON_SSE2)
        __m128 a = _mm_mul_ps(_mm_loadu_ps(x), _mm_loadu_ps(h));
        for (int t = 4; t < kTaps; t += 4) a = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(x + t), _mm_loadu_ps(h + t)));
        alignas(16) float l[4]; _mm_store_ps(l, a);
        return (l[0] + l[1]) + (l[2] + l[3]);
#else
        return dotScalar(x, h);
#endif
    }
private:
    float h_[kTaps];
    float hist_[2 * kTaps];
    int pos_ = 0, phase_ = 0;
};

/** In-place radix-2 FFT (n a power of two), with its twiddles kept. */
class Fft {
public:
    explicit Fft(int n) : n_(n), w_((size_t)n / 2) {
        for (int k = 0; k < n / 2; ++k) w_[(size_t)k] = std::polar(1.0f, (float)(-2 * M_PI * k / n));
    }
    void run(std::vector<std::complex<float>>& a) const {
        const int n = n_;
        for (int i = 1, j = 0; i < n; i++) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[(size_t)i], a[(size_t)j]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < len / 2; k++) {
                    const auto u = a[(size_t)(i + k)], v = a[(size_t)(i + k + len / 2)] * w_[(size_t)(k * step)];
                    a[(size_t)(i + k)] = u + v; a[(size_t)(i + k + len / 2)] = u - v;
                }
        }
    }
private:
    int n_;
    std::vector<std::complex<float>> w_;
};

/** ★ The 12 kHz audio's smoothed power spectrum, remade every kHop samples. Bounded: kN samples, kN/2 + 1 bins. */
class Spectrum {
public:
    static constexpr int kN = 4096, kHop = 2048, kFs = 12000;
    static constexpr double kBin = (double)kFs / kN;
    Spectrum() : fft_(kN), ring_(kN, 0.f), win_(kN), psd_(kN / 2 + 1, 0.0), a_(kN) {
        for (int i = 0; i < kN; ++i) win_[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * i / (kN - 1));
    }
    /** Feeds 12 kHz samples; true when a new spectrum was made. */
    bool feed(const float* x, int n) {
        bool made = false;
        for (int i = 0; i < n; ++i) {
            ring_[(size_t)pos_] = x[i];
            if (++pos_ == kN) pos_ = 0;
            if (++since_ >= kHop) { since_ = 0; step_(); made = true; }
        }
        return made;
    }
    const std::vector<double>& psd() const { return psd_; }
    int frames() const { return frames_; }
    /** The median bin from 150 Hz to 4.5 kHz — the noise floor the tone finders measure against. */
    double noise() const { return noise_; }
    /** Sub-bin peak position (parabolic, on log power) in Hz. */
    double refine(int b) const {
        if (b <= 0 || b >= (int)psd_.size() - 1) return b * kBin;
        const double y0 = std::log(psd_[(size_t)b - 1] + 1e-12), y1 = std::log(psd_[(size_t)b] + 1e-12), y2 = std::log(psd_[(size_t)b + 1] + 1e-12);
        const double den = y0 - 2 * y1 + y2;
        return (b + (den != 0 ? 0.5 * (y0 - y2) / den : 0.0)) * kBin;
    }
    /** Local maxima (over ±2 bins) from 150 Hz to 4.5 kHz at least `overNoise` × the noise, strongest first, at most `max`. */
    std::vector<int> peaks(double overNoise, size_t max) const {
        std::vector<int> p;
        const int lo = (int)(150 / kBin), hi = (int)(4500 / kBin);
        const double th = noise_ * overNoise;
        for (int b = lo + 2; b < hi - 2; ++b) {
            const double v = psd_[(size_t)b];
            if (v < th) continue;
            bool m = true;
            for (int d = -2; d <= 2 && m; ++d) if (d && psd_[(size_t)(b + d)] > v) m = false;
            if (m) p.push_back(b);
        }
        std::sort(p.begin(), p.end(), [&](int a, int b) { return psd_[(size_t)a] > psd_[(size_t)b]; });
        if (p.size() > max) p.resize(max);
        return p;
    }
private:
    void step_() {
        for (int i = 0; i < kN; ++i) a_[(size_t)i] = { ring_[(size_t)((pos_ + i) % kN)] * win_[(size_t)i], 0.0f };
        fft_.run(a_);
        const double k = frames_ < 3 ? 1.0 / (frames_ + 1) : 0.3;   // ~1 s memory at ~6 spectra a second
        for (int b = 0; b <= kN / 2; b++) psd_[(size_t)b] += ((double)std::norm(a_[(size_t)b]) - psd_[(size_t)b]) * k;
        frames_++;
        const int lo = (int)(150 / kBin), hi = (int)(4500 / kBin);
        tmp_.assign(psd_.begin() + lo, psd_.begin() + hi);
        std::nth_element(tmp_.begin(), tmp_.begin() + tmp_.size() / 2, tmp_.end());
        noise_ = std::max(1e-12, tmp_[tmp_.size() / 2]);
    }
    Fft fft_;
    std::vector<float> ring_, win_;
    std::vector<double> psd_, tmp_;
    std::vector<std::complex<float>> a_;
    int pos_ = 0, since_ = 0, frames_ = 0;
    double noise_ = 1e-12;
};

/** ★ Four equally spaced tones (Horus Binary's 4-FSK): the strongest such set among the spectrum's peaks, each ≥ 9 dB over
 *  the noise and within 15 dB of the others, spaced 80–1500 Hz (or `wantSpacing` ±5 % when the listener set it). The
 *  tones in Hz, lowest first; false when there is no such set. */
inline bool findFour(const Spectrum& sp, double tones[4], double wantSpacing = 0) {
    if (sp.frames() < 3) return false;
    const std::vector<int> p0 = sp.peaks(8.0, 12);
    if (p0.size() < 4) return false;
    std::vector<int> p = p0;
    std::sort(p.begin(), p.end());
    const auto& psd = sp.psd();
    double best = -1;
    const size_t n = p.size();
    for (size_t a = 0; a < n; ++a) for (size_t b = a + 1; b < n; ++b) {
        const double d = (p[b] - p[a]) * Spectrum::kBin;
        if (d < 80 || d > 1500) continue;
        if (wantSpacing > 0 && std::fabs(d - wantSpacing) > wantSpacing * 0.05 + Spectrum::kBin) continue;
        const double tol = std::max(6.0, d * 0.04);
        for (size_t c = b + 1; c < n; ++c) {
            if (std::fabs((p[c] - p[b]) * Spectrum::kBin - d) > tol) continue;
            for (size_t e = c + 1; e < n; ++e) {
                if (std::fabs((p[e] - p[c]) * Spectrum::kBin - d) > tol) continue;
                const double v[4] = { psd[(size_t)p[a]], psd[(size_t)p[b]], psd[(size_t)p[c]], psd[(size_t)p[e]] };
                const double mn = std::min(std::min(v[0], v[1]), std::min(v[2], v[3])), mx = std::max(std::max(v[0], v[1]), std::max(v[2], v[3]));
                if (mn < mx * 0.0316) continue;   // within 15 dB
                if (mn > best) {
                    best = mn;
                    tones[0] = sp.refine(p[a]); tones[1] = sp.refine(p[b]); tones[2] = sp.refine(p[c]); tones[3] = sp.refine(p[e]);
                }
            }
        }
    }
    if (best < 0) return false;
    // ★ the four on one even grid: the spacing from the outer pair, the grid centred on the four
    const double s = (tones[3] - tones[0]) / 3, c = (tones[0] + tones[1] + tones[2] + tones[3]) / 4;
    for (int k = 0; k < 4; ++k) tones[k] = c + (k - 1.5) * s;
    return true;
}

/** ★ An RTTY pair: the strongest peak and the strongest other peak 150–1200 Hz from it (or `wantShift` ±8 %), both ≥ 15 dB
 *  over the noise, the weaker within 15 dB of the stronger. The low and high tone; false when there is none. */
inline bool findPair(const Spectrum& sp, double& lo, double& hi, double wantShift = 0) {
    if (sp.frames() < 3) return false;
    const std::vector<int> p = sp.peaks(30.0, 8);
    if (p.size() < 2) return false;
    const auto& psd = sp.psd();
    const int a = p[0];
    int b = -1;
    for (size_t i = 1; i < p.size(); ++i) {
        const double d = std::abs(p[i] - a) * Spectrum::kBin;
        if (d < 150 || d > 1200) continue;
        if (wantShift > 0 && std::fabs(d - wantShift) > wantShift * 0.08 + Spectrum::kBin) continue;
        b = p[i]; break;   // peaks are strongest first
    }
    if (b < 0 || psd[(size_t)b] < psd[(size_t)a] * 0.0316) return false;
    const double fa = sp.refine(a), fb = sp.refine(b);
    lo = std::min(fa, fb); hi = std::max(fa, fb);
    return true;
}

} // namespace balloon
} // namespace vibe
