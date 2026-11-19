// vibe_lrpt_demod.h — METEOR-M LRPT demodulator (V12, 2026-11-19): complex baseband in, soft symbols out.
//
//   IQ (float, Doppler-corrected or with the Doppler given) → NCO (Doppler feed-forward + the carrier loop)
//      → AGC → root-raised-cosine matched filter (α 0.6) → cubic interpolator at two samples a symbol
//      → Gardner timing loop → Costas carrier loop (QPSK, or OQPSK with Q half a symbol late) → int8 soft pairs
//
// ★★★ THE INTERFACE (for the satellite engine, branch v12-sat — see docs/v12/LRPT.md §Interface): complex baseband at
//     ANY rate from 1.8× to 8× the symbol rate (144–600 kS/s for 72k; ~150–250 kS/s recommended), centred on the
//     published downlink. Doppler EITHER already removed by the engine, OR passed in with setDopplerHz() as often as
//     the engine likes (each call takes effect at the next sample; it is a feed-forward, the carrier loop tracks the
//     rest). Without either, the x⁴ acquisition still finds a carrier anywhere in ±Fs/8 — Doppler at 137 MHz is
//     ±3.1 kHz — it just takes the loop a moment longer each time it slips.
//
// ★★ Acquisition is an FFT of the 4th power (QPSK's four phases fold to one line at 4·Δf), re-run only while the lock
//    detector says unlocked; once locked the second-order Costas loop carries the slow Doppler slope (≤ 65 Hz/s).
//
// ★ SIMD: the matched filter (the only hot loop — 21–40 taps at the input rate) is NEON / SSE2 / scalar with the SAME
//   four-lane partial sums and the same final addition order, so all three give bit-identical floats. The interpolator,
//   loops and FFT run at symbol rate or rarer and stay scalar.
#pragma once

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#define VIBE_LRPT_FIR_NEON 1
#elif defined(__SSE2__) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define VIBE_LRPT_FIR_SSE2 1
#endif

namespace vibelrpt {

struct DemodConfig {
    double sampleRate = 0;        ///< input complex samples a second
    double symbolRate = 72000;    ///< 72000 or 80000 (per satellite, from sats.json — never hard-coded by the caller)
    bool   oqpsk = false;         ///< METEOR-M N2-2 onwards transmit OQPSK
    double rrcAlpha = 0.6;
    double carrierBw = 0.005;     ///< loop noise bandwidth × symbol period
    double timingBw  = 0.002;
};

/** Real FIR of a complex stream with a doubled delay line (no wrap in the inner loop). Taps padded to a multiple of 4. */
class CFir {
public:
    void setTaps(const std::vector<float>& h) {
        n_ = (h.size() + 3) & ~size_t(3);
        taps_.assign(n_, 0.f);
        for (size_t i = 0; i < h.size(); i++) taps_[i] = h[h.size() - 1 - i];   // reversed: dot with oldest-first history
        re_.assign(2 * n_, 0.f); im_.assign(2 * n_, 0.f); pos_ = 0;
    }
    void push(float r, float i, float& orr, float& oi) {
        re_[pos_] = re_[pos_ + n_] = r;
        im_[pos_] = im_[pos_ + n_] = i;
        pos_ = (pos_ + 1) % n_;
        orr = dot(&re_[pos_]); oi = dot(&im_[pos_]);
    }
    bool forceScalar = false;
private:
    float dot(const float* x) const {
        const float* h = taps_.data();
        float l[4];
        if (forceScalar) { dotScalar(x, h, l); return (l[0] + l[1]) + (l[2] + l[3]); }
#if defined(VIBE_LRPT_FIR_NEON)
        float32x4_t a = vdupq_n_f32(0.f);
        for (size_t i = 0; i < n_; i += 4) a = vaddq_f32(a, vmulq_f32(vld1q_f32(x + i), vld1q_f32(h + i)));
        vst1q_f32(l, a);
#elif defined(VIBE_LRPT_FIR_SSE2)
        __m128 a = _mm_setzero_ps();
        for (size_t i = 0; i < n_; i += 4) a = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(x + i), _mm_loadu_ps(h + i)));
        _mm_storeu_ps(l, a);
#else
        dotScalar(x, h, l);
#endif
        return (l[0] + l[1]) + (l[2] + l[3]);
    }
    void dotScalar(const float* x, const float* h, float* l) const {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        l[0] = l[1] = l[2] = l[3] = 0.f;
        for (size_t i = 0; i < n_; i += 4)
            for (int k = 0; k < 4; k++) l[k] = l[k] + x[i + size_t(k)] * h[i + size_t(k)];   // ★ no fused multiply-add
    }
    std::vector<float> taps_, re_, im_;
    size_t n_ = 0, pos_ = 0;
};

/** Root-raised-cosine taps, unit DC gain, ±span symbols. */
inline std::vector<float> rrcTaps(double sps, double alpha, int span = 4) {
    const int half = int(std::ceil(span * sps));
    std::vector<float> h(size_t(2 * half + 1));
    double sum = 0;
    for (int i = -half; i <= half; i++) {
        const double t = i / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1.0 - alpha + 4.0 * alpha / M_PI;
        else if (std::fabs(std::fabs(4.0 * alpha * t) - 1.0) < 1e-9)
            v = alpha / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * alpha)));
        else v = (std::sin(M_PI * t * (1 - alpha)) + 4 * alpha * t * std::cos(M_PI * t * (1 + alpha))) / (M_PI * t * (1 - 16 * alpha * alpha * t * t));
        h[size_t(i + half)] = float(v); sum += v;
    }
    for (auto& v : h) v = float(v / sum);
    return h;
}

/** In-place radix-2 FFT (n a power of two) — acquisition only, a few times a second while unlocked. */
inline void fftInPlace(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / double(len);
        const std::complex<float> wl(float(std::cos(ang)), float(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t j = 0; j < len / 2; j++) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl;
            }
        }
    }
}

class Demod {
public:
    explicit Demod(const DemodConfig& c) { configure(c); }
    void configure(const DemodConfig& c) {
        cfg_ = c;
        sps_ = c.sampleRate / c.symbolRate;
        fir_.setTaps(rrcTaps(sps_, c.rrcAlpha));
        loopGains(c.carrierBw, cA_, cB_);
        loopGains(c.timingBw, tA_, tB_);
        hist_.assign(4, {0.f, 0.f}); hold_ = false;
        tau_ = 0; nIn_ = 0; half_ = 0; freq_ = 0; tFreq_ = 0; nr_ = 1; ni_ = 0; stepW_ = 1e9; agcG_ = 1.f;
        acq_.clear(); acq_.reserve(kAcqN); acqDue_ = 0; acqPow_.clear(); acqLooks_ = 0;
        lockAcc_ = 0; lockN_ = 0; lockMetric_ = 0; locked_ = false;
        agc_ = 1.f; amp_ = 1.f;
    }
    /** ★ Doppler feed-forward from the satellite engine, Hz (the shift to REMOVE: positive = signal above nominal). */
    void setDopplerHz(double hz) { doppler_.store(hz, std::memory_order_relaxed); }
    /** ★ Frames are DECODING (Reed–Solomon passing): never re-acquire (at Es/N0 ≈ 3 dB the constellation lock metric dips
     *  under its threshold while frames still decode, and a re-acquisition then moved the NCO under a working loop). */
    void setHold(bool h) { hold_ = h; if (h) { acq_.clear(); acqPow_.clear(); acqLooks_ = 0; } }
    uint64_t acquisitions() const { return acqHits_; }

    /** Feed `n` complex samples (interleaved float I, Q); appends soft pairs (I, Q, I, Q …) to `out`. */
    void feed(const float* iq, size_t n, std::vector<int8_t>& out) {
        const double twoPi = 2 * M_PI;
        for (size_t s = 0; s < n; s++) {
            float xr = iq[2 * s], xi = iq[2 * s + 1];
            if (!std::isfinite(xr) || !std::isfinite(xi)) xr = xi = 0.f;
            // ── AGC: slow power follower on the raw input (the matched filter keeps its gain) ──
            const float p = xr * xr + xi * xi;
            pow_ += 1e-4f * (p - pow_);
            // ★ the gain itself every 64 samples (a 1/√ per sample was a Pi 2's worth of work for a 10⁻⁴ follower)
            if ((nIn_ & 63) == 0) agcG_ = pow_ > 1e-20f ? 1.f / std::sqrt(pow_) : 1.f;
            xr *= agcG_; xi *= agcG_;
            // ── acquisition buffer (x⁴), only while unlocked ──
            if (!locked_ && !hold_) acqPush(xr, xi);
            // ── NCO ──
            /* ★★ THE NCO IS A ROTATING PHASOR, NOT A sin/cos A SAMPLE. The step e^{−jw} is remade only when the loop or
             *  the Doppler moves w; the step is remade from w every 64 samples and the phasor renormalised with it (float drift
             *  ≈ 10⁻⁷ a step). Two transcendental calls a sample at 300 kS/s were most of this receiver's cost on a Pi. */
            if ((nIn_ & 63) == 0) {
                const double dop = doppler_.load(std::memory_order_relaxed);
                const double w = freq_ + twoPi * dop / cfg_.sampleRate;
                if (w != stepW_) { stepW_ = w; stepC_ = std::cos(w); stepS_ = -std::sin(w); }
                const double m = std::sqrt(nr_ * nr_ + ni_ * ni_); if (m > 0) { nr_ /= m; ni_ /= m; }
            }
            const float c = float(nr_), sn = float(ni_);
            { const double r = nr_ * stepC_ - ni_ * stepS_, i = nr_ * stepS_ + ni_ * stepC_; nr_ = r; ni_ = i; }
            const float mr = xr * c - xi * sn, mi = xr * sn + xi * c;
            float fr, fi;
            fir_.push(mr, mi, fr, fi);
            hist_[0] = hist_[1]; hist_[1] = hist_[2]; hist_[2] = hist_[3]; hist_[3] = {fr, fi};
            nIn_++;
            // ── interpolation points falling between hist_[1] and hist_[2] ──
            const double base = double(nIn_) - 3.0;          // time index of hist_[1]
            while (tau_ < base + 1.0) {
                if (tau_ < base) { tau_ = base; }             // fell behind (start-up): catch up
                const float mu = float(tau_ - base);
                std::complex<float> y = interp(mu);
                onPoint(y, out);
                tau_ += sps_ / 2 + (half_ == 0 ? tFreq_ * 0.5 : tFreq_ * 0.5);
            }
        }
    }

    bool locked() const { return locked_; }
    double lockMetric() const { return lockMetric_; }
    double carrierHz() const { return freq_ * cfg_.sampleRate / (2 * M_PI) + doppler_.load(std::memory_order_relaxed); }
    /** Average |soft| (≈ 45 when well-scaled) — with the lock metric, the SNR figure the report shows. */
    double softLevel() const { return amp_; }
    const DemodConfig& config() const { return cfg_; }
    CFir& fir() { return fir_; }

private:
    static constexpr size_t kAcqN = 8192;
    static constexpr int kAcqLooks = 4;
    static void loopGains(double bnT, double& a, double& b) {
        const double z = 0.707, th = bnT / (z + 0.25 / z), d = 1 + 2 * z * th + th * th;
        a = 4 * z * th / d; b = 4 * th * th / d;
    }
    std::complex<float> interp(float mu) const {
        // Cubic Lagrange through hist_[0..3] at t = 1 + mu.
        const float t = 1.f + mu;
        const float l0 = -(t - 1) * (t - 2) * (t - 3) / 6.f, l1 = t * (t - 2) * (t - 3) / 2.f,
                    l2 = -t * (t - 1) * (t - 3) / 2.f, l3 = t * (t - 1) * (t - 2) / 6.f;
        return hist_[0] * l0 + hist_[1] * l1 + hist_[2] * l2 + hist_[3] * l3;
    }
    void acqPush(float xr, float xi) {
        if (acqDue_ > 0) { acqDue_--; return; }
        const std::complex<float> z(xr, xi), z2 = z * z;
        acq_.push_back(z2 * z2);
        if (acq_.size() < kAcqN) return;
        fftInPlace(acq_);
        if (acqPow_.size() != kAcqN) acqPow_.assign(kAcqN, 0.f);
        for (size_t i = 0; i < kAcqN; i++) acqPow_[i] += std::norm(acq_[i]);
        acq_.clear();
        if (++acqLooks_ < kAcqLooks) return;
        /* ★★ FOUR LOOKS AVERAGED, THEN A LINE 8× THE MEAN BIN. One 4096-point look needed the line 20× the mean and
         *  missed a 500 Hz offset at Es/N0 3 dB (the 4th power costs the line ~4× its SNR); the loop then never pulled in.
         *  The average of four looks has a quarter of the variance, so a lower bar is safe: on noise the largest of 8192
         *  four-look bins passes 8× about once in 10¹⁰. ±1 bin around the peak, power-weighted, for a sub-bin estimate. */
        size_t best = 0; float bp = 0, sum = 0;
        for (size_t i = 0; i < kAcqN; i++) { sum += acqPow_[i]; if (acqPow_[i] > bp) { bp = acqPow_[i]; best = i; } }
        if (bp > 8.f * sum / float(kAcqN)) {
            const float l = acqPow_[(best + kAcqN - 1) % kAcqN], r = acqPow_[(best + 1) % kAcqN];
            double bin = double(best) + double(r - l) / double(l + bp + r);
            if (bin >= kAcqN / 2) bin -= double(kAcqN);
            const double f4 = bin * cfg_.sampleRate / double(kAcqN);
            const double dop = doppler_.load(std::memory_order_relaxed);
            freq_ = 2 * M_PI * (f4 / 4.0 - dop) / cfg_.sampleRate;
            acqHits_++;
        }
        acqPow_.assign(kAcqN, 0.f); acqLooks_ = 0;
        acqDue_ = size_t(cfg_.sampleRate * 0.25);           // ★ four decisions a second at most
    }
    void onPoint(std::complex<float> y, std::vector<int8_t>& out) {
        if (half_ == 1) { mid_ = y; half_ = 0; return; }
        half_ = 1;
        const std::complex<float> sym = y;
        float I, Q;
        if (!cfg_.oqpsk) {
            // ── Gardner (QPSK) ──
            const float e = (prev_.real() - sym.real()) * mid_.real() + (prev_.imag() - sym.imag()) * mid_.imag();
            timing(e);
            I = sym.real(); Q = sym.imag();
            carrier(I, Q);
            emit(I, Q, out);
        } else {
            // ── OQPSK: I is judged at the symbol point, Q half a symbol later (the next mid point) ──
            // The pair (I_k, Q_{k+½}) is completed on the NEXT symbol point: (prevI_, mid_.imag()).
            const float eI = (prev_.real() - sym.real()) * mid_.real();
            const float eQ = (prevMid_.imag() - mid_.imag()) * prev_.imag();
            timing(eI + eQ);
            I = prev_.real(); Q = mid_.imag();
            if (primed_) { carrier(I, Q); emit(I, Q, out); }
            primed_ = true;
            prevMid_ = mid_;
        }
        prev_ = sym;
    }
    void timing(float e) {
        if (e > 1.f) e = 1.f; else if (e < -1.f) e = -1.f;
        tFreq_ += tB_ * e * sps_;
        if (tFreq_ > 0.01 * sps_) tFreq_ = 0.01 * sps_; else if (tFreq_ < -0.01 * sps_) tFreq_ = -0.01 * sps_;
        tau_ += tA_ * e * sps_;
    }
    void carrier(float I, float Q) {
        float e = (I > 0 ? Q : -Q) - (Q > 0 ? I : -I);
        const float r = std::sqrt(I * I + Q * Q) + 1e-9f;
        e /= r;
        // Applied as the NCO's phase/frequency: the NCO turns backwards by w a sample, so a positive error turns it back
        // by α·e now (a small rotation of the phasor) and adds β·e to the frequency.
        { const double a = -cA_ * e, ca = 1.0 - 0.5 * a * a, sa = a;   // ★ |α·e| ≤ 0.02 rad: second order is exact enough
          const double r = nr_ * ca - ni_ * sa, i = nr_ * sa + ni_ * ca; nr_ = r; ni_ = i; }
        freq_ += cB_ * e / sps_;   // ★ reaches the phasor's step at the next 64-sample boundary
        const double lim = 2 * M_PI * 0.2 * cfg_.symbolRate / cfg_.sampleRate;
        if (freq_ > lim) freq_ = lim; else if (freq_ < -lim) freq_ = -lim;
        // ── lock detector: 1 − E(|I|−|Q|)² / E(I²+Q²) — 1.0 locked and clean, ≈0.64 unlocked or on noise ──
        const float d = std::fabs(I) - std::fabs(Q);
        lockAcc_ += double(d * d) / double(r * r);
        if (++lockN_ == 2048) {
            lockMetric_ = 1.0 - lockAcc_ / 2048.0;
            lockAcc_ = 0; lockN_ = 0;
            locked_ = lockMetric_ > 0.80;
        }
    }
    void emit(float I, float Q, std::vector<int8_t>& out) {
        const float a = 0.5f * (std::fabs(I) + std::fabs(Q));
        agc_ += 2e-4f * (a - agc_);
        const float k = agc_ > 1e-9f ? 45.f / agc_ : 1.f;
        auto q = [&](float v) { float s = v * k; if (s > 127.f) s = 127.f; else if (s < -127.f) s = -127.f; return int8_t(std::lrint(s)); };
        out.push_back(q(I)); out.push_back(q(Q));
        amp_ += 1e-3 * (0.5 * (std::fabs(double(out[out.size() - 2])) + std::fabs(double(out.back()))) - amp_);
    }

    DemodConfig cfg_;
    double sps_ = 2;
    CFir fir_;
    std::vector<std::complex<float>> hist_;
    double tau_ = 0, tFreq_ = 0; uint64_t nIn_ = 0; int half_ = 0;
    std::complex<float> prev_{0, 0}, mid_{0, 0}, prevMid_{0, 0}; bool primed_ = false;
    double freq_ = 0, cA_ = 0, cB_ = 0, tA_ = 0, tB_ = 0;
    double nr_ = 1, ni_ = 0, stepW_ = 1e9, stepC_ = 1, stepS_ = 0;   ///< the NCO phasor and its per-sample step
    std::atomic<double> doppler_{0.0};
    float pow_ = 1.f, agc_ = 1.f, agcG_ = 1.f; double amp_ = 0;
    std::vector<std::complex<float>> acq_; std::vector<float> acqPow_; size_t acqDue_ = 0; int acqLooks_ = 0; uint64_t acqHits_ = 0;
    double lockAcc_ = 0, lockMetric_ = 0; int lockN_ = 0; bool locked_ = false, hold_ = false;
};

} // namespace vibelrpt
