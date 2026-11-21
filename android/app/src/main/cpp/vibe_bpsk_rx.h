// vibe_bpsk_rx.h — V12 beta90: a BPSK demodulator for satellite telemetry on a listener's SSB audio (docs/v12/
// SATELLITE-BEST.md 1.2 — FUNcube-1 / AO-73 at 1200 Bd). Our own code, written from textbook receiver structure: an
// NCO + low-pass decimator, a coarse AFC from the squared signal's spectrum line, a matched low-pass, a Costas loop and
// Gardner timing. Soft symbols (real, ±1 nominal) out; the framer (vibe_fctlm.h) resolves the phase ambiguity and the
// differential code with the sync vector — this file assumes neither.
//
// ★★ Where it listens: the pick tunes a FUNcube downlink 1200 Hz LOW in USB (vibe_sat_engine.h listenOffsetHz), so
//    the signal sits at ~1200 Hz audio; Doppler is taken out by the satellite lock, the residual (clock error, an old
//    element set) by the AFC (±900 Hz) and the Costas loop. Manual fallback: "centre" (300–2700 Hz), "afc":false.
// ★★ SIMD: the two FIRs' dot products are NEON / SSE2 / scalar with the same four-lane partial sums and the same final
//    order (and no fused multiply-add) → bit-identical output on every path (test-fctlm checks it).
// ★ Cost (est. before measuring): 48 kS/s × a complex mix + 9.6 kS/s × 2 × 96 taps + 9.6 kS/s × 2 × 32 taps ≈ 3 M
//   multiply-adds a second; a 512-point FFT twice a second while hunting. Bounded memory (< 20 kB).
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#define VIBE_BPSK_NEON 1
#elif defined(__SSE2__) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define VIBE_BPSK_SSE2 1
#endif

namespace vibebpsk {

/** ★ Test seam: 0 scalar, 1 NEON, 2 SSE2. */
inline int kernel() {
#if defined(VIBE_BPSK_NEON)
    return 1;
#elif defined(VIBE_BPSK_SSE2)
    return 2;
#else
    return 0;
#endif
}

inline void dotScalar4(const float* x, const float* h, size_t n, float* l) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    l[0] = l[1] = l[2] = l[3] = 0.f;
    for (size_t i = 0; i < n; i += 4)
        for (int k = 0; k < 4; k++) { const float p = x[i + size_t(k)] * h[i + size_t(k)]; l[k] = l[k] + p; }
}
/** Σ x·h over n (a multiple of 4) — four lanes, summed (l0 + l1) + (l2 + l3) on every path. */
inline float dot(const float* x, const float* h, size_t n, bool forceScalar = false) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    float l[4];
    if (forceScalar) { dotScalar4(x, h, n, l); return (l[0] + l[1]) + (l[2] + l[3]); }
#if defined(VIBE_BPSK_NEON)
    float32x4_t a = vdupq_n_f32(0.f);
    for (size_t i = 0; i < n; i += 4) a = vaddq_f32(a, vmulq_f32(vld1q_f32(x + i), vld1q_f32(h + i)));
    vst1q_f32(l, a);
#elif defined(VIBE_BPSK_SSE2)
    __m128 a = _mm_setzero_ps();
    for (size_t i = 0; i < n; i += 4) a = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(x + i), _mm_loadu_ps(h + i)));
    _mm_storeu_ps(l, a);
#else
    dotScalar4(x, h, n, l);
#endif
    return (l[0] + l[1]) + (l[2] + l[3]);
}

/** A windowed-sinc low-pass, `n` taps (a multiple of 4), cutoff `fc` at `fs`, unity DC gain (Blackman window). */
inline std::vector<float> lowpass(int n, double fc, double fs) {
    std::vector<float> h((size_t)n);
    double sum = 0;
    const double m = (n - 1) / 2.0;
    for (int i = 0; i < n; ++i) {
        const double x = i - m;
        const double s = x == 0 ? 2 * fc / fs : std::sin(2 * M_PI * fc / fs * x) / (M_PI * x);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
        h[(size_t)i] = float(s * w); sum += s * w;
    }
    for (auto& v : h) v = float(v / sum);
    return h;
}

/** A complex ring for a FIR: each sample is written twice (i and i+n) so the window is always contiguous. */
struct Ring {
    std::vector<float> re, im; size_t n = 0, pos = 0;
    void init(size_t taps) { n = taps; re.assign(2 * n, 0.f); im.assign(2 * n, 0.f); pos = 0; }
    void push(float r, float i) { re[pos] = re[pos + n] = r; im[pos] = im[pos + n] = i; pos = (pos + 1) % n; }
    const float* wr() const { return &re[pos]; }
    const float* wi() const { return &im[pos]; }
};

/** In-place radix-2 FFT (n a power of two) — the AFC's hunt only (twice a second at most). */
inline void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / double(len);
        const std::complex<float> wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

struct RxCfg {
    double rate = 48000;      ///< input audio rate (an integer multiple of 8 × baud is best: 48 k / 9.6 k = 5)
    double baud = 1200;
    double centreHz = 1200;   ///< where in the audio the signal is expected
    bool afc = true;          ///< the coarse hunt (±900 Hz); off = the Costas loop alone (±~60 Hz)
};

class Rx {
public:
    using Cfg = RxCfg;
    /** One soft symbol (≈ ±1 when locked) — called on the caller's thread, from feed(). */
    std::function<void(float)> onSymbol;
    bool forceScalar = false;     ///< test seam (SIMD vs scalar)

    explicit Rx(const Cfg& c = RxCfg()) : c_(c) {
        dec_ = std::max(1, (int)std::lround(c_.rate / (8.0 * c_.baud)));
        fs2_ = c_.rate / dec_;
        sps_ = fs2_ / c_.baud;
        h1_ = lowpass(96, 1600.0, c_.rate);
        h2_ = lowpass(32, 0.62 * c_.baud, fs2_);
        r1_.init(h1_.size()); r2_.init(h2_.size());
        coarse_ = 0;
        setNco1();
        hist_.assign(32, std::complex<float>(0, 0));
        nextStrobe_ = sps_;
        sq_.reserve(kFft);
    }
    double afcHz() const { return coarse_ + w2_ * fs2_ / (2 * M_PI); }
    double centreHz() const { return c_.centreHz; }
    bool afcOn() const { return c_.afc; }
    double snrDb() const { return 10.0 * std::log10(std::max(1e-6, (double)sigPow_) / std::max(1e-6, (double)noisePow_)); }
    /** The framer says a sync was found (or not, for long): the hunt stands down while locked. */
    void setLocked(bool on) { locked_ = on; }
    /** Samples fed so far (a clock for tests and the status). */
    uint64_t samples() const { return nIn_; }

    void feed(const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            // the NCO (a rotator, renormalised): x · e^{−jωt}
            const float re = x[i] * cr_, im = -x[i] * ci_;
            const float nr = cr_ * dr_ - ci_ * di_, ni = cr_ * di_ + ci_ * dr_;
            cr_ = nr; ci_ = ni;
            if ((++rot_ & 1023) == 0) { const float g = 1.0f / std::sqrt(cr_ * cr_ + ci_ * ci_); cr_ *= g; ci_ *= g; }
            r1_.push(re, im);
            ++nIn_;
            if (++phase1_ < dec_) continue;
            phase1_ = 0;
            stage2(std::complex<float>(dot(r1_.wr(), h1_.data(), h1_.size(), forceScalar), dot(r1_.wi(), h1_.data(), h1_.size(), forceScalar)));
        }
    }

private:
    static constexpr size_t kFft = 512;
    void setNco1() {
        const double w = 2 * M_PI * (c_.centreHz + coarse_) / c_.rate;
        dr_ = (float)std::cos(w); di_ = (float)std::sin(w);
    }
    /** 9.6 kS/s: the AFC's hunt, the Costas rotation, the matched filter, Gardner. */
    void stage2(std::complex<float> z) {
        if (c_.afc && !locked_ && n2_ >= huntHold_) {
            sq_.push_back(z * z);
            if (sq_.size() >= kFft) { hunt(); sq_.clear(); }
        } else if (!sq_.empty()) sq_.clear();
        // Costas rotation (phase p2, frequency w2 per sample)
        const std::complex<float> rot((float)std::cos(p2_), (float)-std::sin(p2_));
        p2_ += w2_;
        if (p2_ > M_PI) p2_ -= 2 * M_PI; else if (p2_ < -M_PI) p2_ += 2 * M_PI;
        const std::complex<float> y0 = z * rot;
        r2_.push(y0.real(), y0.imag());
        const std::complex<float> y(dot(r2_.wr(), h2_.data(), h2_.size(), forceScalar), dot(r2_.wi(), h2_.data(), h2_.size(), forceScalar));
        hist_[hpos_] = y; hpos_ = (hpos_ + 1) % hist_.size();
        ++n2_;
        // Gardner on linear interpolation: strobe once the newest sample (index n2_ − 1) has passed nextStrobe_
        if ((double)(n2_ - 1) < nextStrobe_) return;
        const std::complex<float> sNow = interp(nextStrobe_);
        const std::complex<float> sMid = interp(nextStrobe_ - sps_ / 2.0);
        // AGC on the symbol power
        const float p = std::norm(sNow);
        pow_ = pow_ * 0.995f + p * 0.005f;
        const float g = 1.0f / std::sqrt(std::max(pow_, 1e-12f));
        const float sr = sNow.real() * g, si = sNow.imag() * g;
        // timing error (Gardner), normalised
        const float te = (prev_.real() - sr) * (sMid.real() * g);
        prev_ = std::complex<float>(sr, si);
        double adj = std::max(-0.5, std::min(0.5, 0.06 * (double)te));
        nextStrobe_ += sps_ + adj;
        // Costas (BPSK): e = sign(I)·Q, a second-order loop at the symbol rate
        const float ce = (sr >= 0 ? si : -si);
        w2_ += kI_ * ce; p2_ += kP_ * ce;
        const double wMax = 2 * M_PI * 120.0 / fs2_;
        w2_ = std::max(-wMax, std::min(wMax, w2_));
        // ★ a Doppler ramp walks the loop's frequency off: past 20 Hz it is handed to the first NCO (phase-continuous —
        //   the rotator carries on from where it is — and the total frequency is unchanged), so the loop never meets its
        //   clamp. The first NCO's own range is the AFC's (±900 Hz around the centre).
        const double w2Hz = w2_ * fs2_ / (2 * M_PI);
        if (std::fabs(w2Hz) > 20.0 && std::fabs(coarse_ + w2Hz) <= 900.0) { coarse_ += w2Hz; w2_ = 0; setNco1(); }
        sigPow_ = sigPow_ * 0.99f + sr * sr * 0.01f;
        noisePow_ = noisePow_ * 0.99f + si * si * 0.01f;
        if (onSymbol) onSymbol(sr);
    }
    /** The matched-filter output at absolute (fractional) sample index `t` — linear between its neighbours. */
    std::complex<float> interp(double t) const {
        const double base = std::floor(t);
        const float f = (float)(t - base);
        const long long i0 = (long long)base;
        auto at = [&](long long abs) {
            const long long back = (long long)n2_ - 1 - abs;
            if (back < 0 || back >= (long long)hist_.size()) return std::complex<float>(0, 0);
            const size_t k = (hpos_ + hist_.size() - 1 - (size_t)back) % hist_.size();
            return hist_[k];
        };
        return at(i0) * (1.0f - f) + at(i0 + 1) * f;
    }
    /** ★ The coarse AFC: z² removes the BPSK modulation and leaves a line at 2Δf — its FFT peak, if it stands well
     *  above the floor, moves the NCO (the Costas loop then pulls in the rest). */
    void hunt() {
        std::vector<std::complex<float>> a(sq_);
        for (size_t i = 0; i < a.size(); ++i) a[i] *= (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / (a.size() - 1)));
        fft(a);
        const double binHz = fs2_ / (double)kFft;
        const int maxBin = (int)(2 * 900.0 / binHz);
        double best = 0, sum = 0; int bi = 0, cnt = 0;
        for (int k = -maxBin; k <= maxBin; ++k) {
            const double m = std::norm(a[(size_t)((k + (int)kFft) % (int)kFft)]);
            sum += m; ++cnt;
            if (m > best) { best = m; bi = k; }
        }
        const double mean = (sum - best) / std::max(1, cnt - 1);
        if (best < 25.0 * mean) return;   // ★ no clear line: keep listening where we are
        // ★★ ALREADY ON A SIGNAL: a clear line where we are tuned (|2Δf| ≤ 2 bins, ≈ ±19 Hz) — stay. The hunt runs until
        //   the first frame, and a real (USB) audio signal low in the passband has its mirror image close enough that
        //   signal × image makes a second line: −500 Hz from 1200 (a 700 Hz carrier) jumped to it and ran to −900 Hz.
        {
            double here = 0;
            for (int k = -2; k <= 2; ++k) here = std::max(here, (double)std::norm(a[(size_t)((k + (int)kFft) % (int)kFft)]));
            if (here >= 25.0 * mean) { huntHold_ = n2_ + (uint64_t)(kHoldSec * fs2_); return; }
        }
        // interpolate the peak (parabolic)
        auto mag = [&](int k) { return std::sqrt(std::norm(a[(size_t)((k + (int)kFft) % (int)kFft)])); };
        const double ym = mag(bi - 1), y0 = mag(bi), yp = mag(bi + 1);
        const double den = ym - 2 * y0 + yp;
        const double d = std::fabs(den) > 1e-12 ? 0.5 * (ym - yp) / den : 0.0;
        const double df = 0.5 * (bi + std::max(-0.5, std::min(0.5, d))) * binHz;   // Δf = (line)/2
        const double resid = df + w2_ * fs2_ / (2 * M_PI);
        if (std::fabs(resid) < 8.0) return;   // already there
        coarse_ = std::max(-900.0, std::min(900.0, coarse_ + df));
        w2_ = 0; p2_ = 0;
        setNco1();
        huntHold_ = n2_ + (uint64_t)(kHoldSec * fs2_);   // ★ found: let the Costas loop and the framer work
    }

    Cfg c_;
    int dec_ = 5, phase1_ = 0;
    double fs2_ = 9600, sps_ = 8;
    std::vector<float> h1_, h2_;
    Ring r1_, r2_;
    float cr_ = 1, ci_ = 0, dr_ = 1, di_ = 0;
    uint32_t rot_ = 0;
    uint64_t nIn_ = 0, n2_ = 0;
    double coarse_ = 0, p2_ = 0, w2_ = 0;
    static constexpr double kP_ = 0.04, kI_ = 0.0008;
    std::vector<std::complex<float>> hist_; size_t hpos_ = 0;
    double nextStrobe_ = 8;
    std::complex<float> prev_{0, 0};
    float pow_ = 1e-6f, sigPow_ = 0, noisePow_ = 1;
    bool locked_ = false;
    /* ★★ After a hunt finds a line (moved to it, or it is already where we are tuned), no hunt for kHoldSec — longer than
     *  a FUNcube frame period (5 s), so the framer gets a whole frame. Re-hunting every 53 ms until the first frame
     *  knocked the loop off a signal it had (−500 Hz ran to −900: the signal × its audio image is a second line). No
     *  frame by then: hunt again. Before, ONE hunt ran — only if the first block was ≥ 53 ms (20 ms server blocks: none). */
    static constexpr double kHoldSec = 6.0;
    uint64_t huntHold_ = 0;
    std::vector<std::complex<float>> sq_;
};

} // namespace vibebpsk
