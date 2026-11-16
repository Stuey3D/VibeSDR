// vibe_rs41_rx.h — V12 RS41 radiosonde: THE RECEIVER. One capture across a 400–406 MHz band → every sonde in it.
// docs/v12/RS41.md. Our own code; the frame itself is vibe_rs41.h.
//
// ★★★ ONE FORWARD FFT FOR THE WHOLE BAND, TWO USES. vibedsp's Channelizer (overlap-save, PFFFT) transforms the capture
//     once a block. (1) THE SCAN: each bin's power is smoothed over about a second (SIMD), and twice a second the band is
//     searched for something sonde-shaped — about 10 kHz wide, not a carrier (a spur has nearly all its power in a bin
//     or two), not a broad block (the survey's 240 kHz one at 402.6 MHz). (2) THE CHANNELS: each candidate becomes a slice
//     of those same bins inverse-transformed at ~16–19 kS/s (one 32-point FFT a block) — a fourth sonde costs a small FFT,
//     not a full-rate mixer. Up to `maxChannels` at once; a channel that has decoded nothing for 20 s gives its slot up
//     when another candidate is waiting.
// ★★ THE DEMODULATOR (per channel, all on the service thread): FM discriminator (a SIMD atan2: NEON / SSE2 / scalar)
//    → linear resampling to exactly 8 samples a bit → a one-bit integrator → the header's 64 bits searched at all eight
//    phases by a shift register and popcount (≤ 5 bits wrong, either polarity: no soft work on noise) → the best phase by
//    soft correlation → amplitude and frequency error by least squares on the header's known bits → bits at that phase
//    with an early–late timing nudge → de-whiten → Reed–Solomon (errors, then the weakest bytes erased) → blocks by CRC.
// ★ A SONDE EXISTS ONLY WHEN A FRAME PROVES IT: Reed–Solomon clean on both codewords AND the status block's CRC AND a
//   plausible serial. Peaks the scan finds on noise or spurs open channels that simply decode nothing.
#pragma once
#include "vibe_rs41.h"
#include "vibedsp/vibedsp.h"
#include "vibedsp/simd_internal.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace vibers41 {

using cf32 = vibedsp::cf32;

constexpr int kSps = 8;            ///< samples a bit after the resampler
constexpr int kMaxHdrErr = 5;      ///< header bits allowed wrong (of 64)
/** ★ The channel's rate: the smallest slice of at least 15 kS/s (32 bins: 18.75 kS/s at 2.4 MS/s, flat to ±4.7 kHz,
 *  shut by ±9.4 kHz). Measured (test-rs41 §6): a 64-bin slice (37.5 kS/s) let twice the noise into the discriminator
 *  and needed 10 dB for whole frames where this needs 8 dB. ±3.2 kHz deviation still decodes (§7). */
#ifndef RS41_CHAN_MIN
#define RS41_CHAN_MIN 15e3
#endif
constexpr double kChanMinRate = RS41_CHAN_MIN;

// ── SIMD kernels ─────────────────────────────────────────────────────────────────────────────────────────────────────

/** The scalar reference of the vector atan2 below: Abramowitz & Stegun 4.4.49 on [0, 1] (|error| ≤ 1e-5 rad), folded
 *  to the whole circle. */
inline float atan2Fast(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    const float a = mn / (mx + 1e-30f), s = a * a;
    const float r0 = ((((0.0208351f * s - 0.0851330f) * s + 0.1801410f) * s - 0.3302995f) * s + 0.9998660f) * a;
    float r = r0;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

/** out[k] = arg(z[k] · conj(z[k−1])), z[−1] = `prev` (updated). The FM discriminator, rad per sample. `tmp` scratch. */
inline void fmDisc(const cf32* z, int n, cf32& prev, float* out, std::vector<cf32>& tmp) {
    if (n <= 0) return;
    tmp.resize(size_t(n) + 1);
    tmp[0] = prev;
    std::memcpy(tmp.data() + 1, z, size_t(n) * sizeof(cf32));
    prev = z[n - 1];
    const float* cf = reinterpret_cast<const float*>(tmp.data() + 1);   // current
    const float* pf = reinterpret_cast<const float*>(tmp.data());       // previous
    int k = 0;
#if VIBE_NEON
    const float32x4_t c1 = vdupq_n_f32(0.0208351f), c2 = vdupq_n_f32(-0.0851330f), c3 = vdupq_n_f32(0.1801410f),
                      c4 = vdupq_n_f32(-0.3302995f), c5 = vdupq_n_f32(0.9998660f), hp = vdupq_n_f32(1.57079637f),
                      pi = vdupq_n_f32(3.14159274f), tiny = vdupq_n_f32(1e-30f), zero = vdupq_n_f32(0.f);
    for (; k + 4 <= n; k += 4) {
        const float32x4x2_t c = vld2q_f32(cf + 2 * k), p = vld2q_f32(pf + 2 * k);
        const float32x4_t re = vaddq_f32(vmulq_f32(c.val[0], p.val[0]), vmulq_f32(c.val[1], p.val[1]));
        const float32x4_t im = vsubq_f32(vmulq_f32(c.val[1], p.val[0]), vmulq_f32(c.val[0], p.val[1]));
        const float32x4_t ax = vabsq_f32(re), ay = vabsq_f32(im);
        const float32x4_t mx = vaddq_f32(vmaxq_f32(ax, ay), tiny), mn = vminq_f32(ax, ay);
        float32x4_t inv = vrecpeq_f32(mx);
        inv = vmulq_f32(inv, vrecpsq_f32(mx, inv));
        inv = vmulq_f32(inv, vrecpsq_f32(mx, inv));
        const float32x4_t a = vmulq_f32(mn, inv), s = vmulq_f32(a, a);
        float32x4_t r = vaddq_f32(vmulq_f32(c1, s), c2);
        r = vaddq_f32(vmulq_f32(r, s), c3);
        r = vaddq_f32(vmulq_f32(r, s), c4);
        r = vaddq_f32(vmulq_f32(r, s), c5);
        r = vmulq_f32(r, a);
        r = vbslq_f32(vcgtq_f32(ay, ax), vsubq_f32(hp, r), r);
        r = vbslq_f32(vcltq_f32(re, zero), vsubq_f32(pi, r), r);
        r = vbslq_f32(vcltq_f32(im, zero), vnegq_f32(r), r);
        vst1q_f32(out + k, r);
    }
#elif VIBE_SSE
    const __m128 c1 = _mm_set1_ps(0.0208351f), c2 = _mm_set1_ps(-0.0851330f), c3 = _mm_set1_ps(0.1801410f),
                 c4 = _mm_set1_ps(-0.3302995f), c5 = _mm_set1_ps(0.9998660f), hp = _mm_set1_ps(1.57079637f),
                 pi = _mm_set1_ps(3.14159274f), tiny = _mm_set1_ps(1e-30f), zero = _mm_setzero_ps(),
                 absm = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), sgn = _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u)));
    for (; k + 4 <= n; k += 4) {
        __m128 cr, ci, pr, pim;
        vibedsp::sseLoad2(cf + 2 * k, cr, ci); vibedsp::sseLoad2(pf + 2 * k, pr, pim);
        const __m128 re = _mm_add_ps(_mm_mul_ps(cr, pr), _mm_mul_ps(ci, pim));
        const __m128 im = _mm_sub_ps(_mm_mul_ps(ci, pr), _mm_mul_ps(cr, pim));
        const __m128 ax = _mm_and_ps(re, absm), ay = _mm_and_ps(im, absm);
        const __m128 a = _mm_div_ps(_mm_min_ps(ax, ay), _mm_add_ps(_mm_max_ps(ax, ay), tiny)), s = _mm_mul_ps(a, a);
        __m128 r = _mm_add_ps(_mm_mul_ps(c1, s), c2);
        r = _mm_add_ps(_mm_mul_ps(r, s), c3);
        r = _mm_add_ps(_mm_mul_ps(r, s), c4);
        r = _mm_add_ps(_mm_mul_ps(r, s), c5);
        r = _mm_mul_ps(r, a);
        __m128 m = _mm_cmpgt_ps(ay, ax);
        r = _mm_or_ps(_mm_and_ps(m, _mm_sub_ps(hp, r)), _mm_andnot_ps(m, r));
        m = _mm_cmplt_ps(re, zero);
        r = _mm_or_ps(_mm_and_ps(m, _mm_sub_ps(pi, r)), _mm_andnot_ps(m, r));
        r = _mm_xor_ps(r, _mm_and_ps(im, sgn));
        _mm_storeu_ps(out + k, r);
    }
#endif
    for (; k < n; ++k) {
        const float re = cf[2 * k] * pf[2 * k] + cf[2 * k + 1] * pf[2 * k + 1];
        const float im = cf[2 * k + 1] * pf[2 * k] - cf[2 * k] * pf[2 * k + 1];
        out[k] = atan2Fast(im, re);
    }
}

/** acc[k] += a · (|b[k]|² − acc[k]) — the scan's smoothed power per bin. NEON / SSE2 / scalar. */
inline void powerEma(const cf32* b, float* acc, int n, float a) {
    const float* bf = reinterpret_cast<const float*>(b);
    int k = 0;
#if VIBE_NEON
    const float32x4_t av = vdupq_n_f32(a);
    for (; k + 4 <= n; k += 4) {
        const float32x4x2_t x = vld2q_f32(bf + 2 * k);
        const float32x4_t p = vaddq_f32(vmulq_f32(x.val[0], x.val[0]), vmulq_f32(x.val[1], x.val[1]));
        const float32x4_t q = vld1q_f32(acc + k);
        vst1q_f32(acc + k, vaddq_f32(q, vmulq_f32(av, vsubq_f32(p, q))));
    }
#elif VIBE_SSE
    const __m128 av = _mm_set1_ps(a);
    for (; k + 4 <= n; k += 4) {
        __m128 xr, xi; vibedsp::sseLoad2(bf + 2 * k, xr, xi);
        const __m128 p = _mm_add_ps(_mm_mul_ps(xr, xr), _mm_mul_ps(xi, xi));
        const __m128 q = _mm_loadu_ps(acc + k);
        _mm_storeu_ps(acc + k, _mm_add_ps(q, _mm_mul_ps(av, _mm_sub_ps(p, q))));
    }
#endif
    for (; k < n; ++k) { const float p = bf[2 * k] * bf[2 * k] + bf[2 * k + 1] * bf[2 * k + 1]; acc[k] += a * (p - acc[k]); }
}

// ── One channel's demodulator ────────────────────────────────────────────────────────────────────────────────────────

class ChanRx {
public:
    explicit ChanRx(double chRate) : rate_(chRate), step_(chRate / (kSps * kBaud)), hdr_(headerBits()) {}
    double rate() const { return rate_; }
    uint64_t syncs() const { return syncs_; }
    uint64_t rsFails() const { return rsFails_; }

    /** Channel samples at rate(); onFrame(Frame&) for every frame whose Reed–Solomon and status block pass. */
    template <class F> void process(const cf32* z, int n, F&& onFrame) {
        if (n <= 0) return;
        d_.resize(size_t(n));
        fmDisc(z, n, prev_, d_.data(), tmp_);
        // ★ linear resampling of the discriminator to exactly kSps a bit (the discriminator is smooth: ±~5 kHz content
        //   at 32–38 kS/s); index −1 is the previous block's last value
        while (pos_ < double(n - 1)) {
            const int i = int(std::floor(pos_));
            const double fr = pos_ - i;
            const float a = i < 0 ? dPrev_ : d_[size_t(i)], b = d_[size_t(i + 1)];
            push_(float(a + fr * (b - a)), onFrame);
            pos_ += step_;
        }
        pos_ -= n;
        dPrev_ = d_[size_t(n - 1)];
    }

private:
    static constexpr int kRing = 2048;
    float at_(uint64_t m) const { return S_[m & (kRing - 1)]; }

    template <class F> void push_(float y, F& onFrame) {
        box_[bi_ & 7] = y; ++bi_;
        float s = 0;
        for (int j = 0; j < 8; ++j) s += box_[j];
        const uint64_t m = m_++;
        S_[m & (kRing - 1)] = s;
        const int p = int(m & 7);
        reg_[p] = (reg_[p] << 1) | (s > 0 ? 1u : 0u);
        if (state_ == kSearch) {
            if (m >= uint64_t(64 * kSps + 16)) {
                const int e = __builtin_popcountll(reg_[p] ^ hdr_);
                if (e <= kMaxHdrErr || e >= 64 - kMaxHdrErr) {
                    double c = 0, mag = 0;
                    for (int j = 0; j < 64; ++j) { const float v = at_(m - uint64_t(kSps * j)); c += ((hdr_ >> j) & 1) ? v : -v; mag += std::fabs(v); }
                    const double q = mag > 0 ? std::fabs(c) / mag : 0;
                    if (!pend_ || q > pendQ_) { pend_ = true; pendQ_ = q; pendM_ = m; pendUntil_ = m + kSps; }
                }
            }
            if (pend_ && m >= pendUntil_) { pend_ = false; commit_(pendM_); }
        } else {
            if (m == t_ + 1) symbol_(onFrame);
        }
    }

    /** The header ends at sample `mh`: amplitude, offset and noise by least squares on its 64 known bits. */
    void commit_(uint64_t mh) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int j = 0; j < 64; ++j) {
            const double x = ((hdr_ >> j) & 1) ? 1.0 : -1.0, y = at_(mh - uint64_t(kSps * j));
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double vx = sxx - sx * sx / 64, A = vx > 0 ? (sxy - sx * sy / 64) / vx : 0, D = (sy - A * sx) / 64;
        if (std::fabs(A) < 1e-6) return;
        double r2 = 0;
        for (int j = 0; j < 64; ++j) {
            const double x = ((hdr_ >> j) & 1) ? 1.0 : -1.0, y = at_(mh - uint64_t(kSps * j));
            r2 += (y - A * x - D) * (y - A * x - D);
        }
        A_ = A; D_ = D;
        snrDb_ = 10 * std::log10(A * A / std::max(1e-12, r2 / 62));
        ++syncs_;
        state_ = kCollect;
        t_ = mh + kSps;
        tau_ = 0;
        std::memcpy(raw_, headerOnAir(), 8);
        std::fill(soft_, soft_ + 8, 1e9f);
        nbits_ = 64; len_ = 0;
        std::memset(raw_ + 8, 0, sizeof raw_ - 8);
    }

    template <class F> void symbol_(F& onFrame) {
        const double a = std::fabs(A_);
        const double v = (at_(t_) - D_) / A_;
        const double e = (std::fabs(at_(t_ + 1) - D_) - std::fabs(at_(t_ - 1) - D_)) / a;
        const int bit = v > 0 ? 1 : 0;
        const int byte = nbits_ >> 3;
        raw_[byte] |= uint8_t(bit << (nbits_ & 7));
        soft_[byte] = (nbits_ & 7) ? std::min(soft_[byte], float(std::fabs(v))) : float(std::fabs(v));
        ++nbits_;
        tau_ += 0.06 * e;
        t_ += kSps;
        if (tau_ > 0.5) { ++t_; tau_ -= 1; } else if (tau_ < -0.5) { --t_; tau_ += 1; }
        if (nbits_ == (kTypePos + 1) * 8) {
            len_ = lengthForType(uint8_t(raw_[kTypePos] ^ mask()[kTypePos]));
            if (!len_) len_ = kStdLen;
        }
        if (len_ && nbits_ == len_ * 8) finish_(onFrame);
    }

    template <class F> void finish_(F& onFrame) {
        state_ = kSearch;
        uint8_t f[kMaxLen];
        std::memcpy(f, raw_, size_t(len_));
        whiten(f, len_);
        const int r = correct(f, len_, soft_);
        if (r < 0) { ++rsFails_; return; }
        Frame fr;
        fr.len = len_; fr.rsFixed = r;
        parse(f, fr);
        if (!fr.statusOk) return;
        fr.offsetHz = (D_ / kSps) * rate_ / (2 * M_PI);
        fr.snrDb = snrDb_;
        onFrame(fr);
    }

    enum { kSearch, kCollect };
    double rate_, step_;
    uint64_t hdr_;
    std::vector<float> d_;
    std::vector<cf32> tmp_;
    cf32 prev_{1, 0};
    float dPrev_ = 0;
    double pos_ = 0;
    float box_[8] = {0};
    uint64_t bi_ = 0, m_ = 0;
    float S_[kRing] = {0};
    uint64_t reg_[8] = {0};
    int state_ = kSearch;
    bool pend_ = false; double pendQ_ = 0; uint64_t pendM_ = 0, pendUntil_ = 0;
    double A_ = 1, D_ = 0, snrDb_ = 0, tau_ = 0;
    uint64_t t_ = 0;
    uint8_t raw_[kMaxLen] = {0};
    float soft_[kMaxLen] = {0};
    int nbits_ = 0, len_ = 0;
    uint64_t syncs_ = 0, rsFails_ = 0;
};

// ── The band: the scan and the channels ─────────────────────────────────────────────────────────────────────────────

class Receiver {
public:
    /** `rate` the capture rate, `hwHz` where the radio is tuned, [loHz, hiHz] the band to search (cut to what the
     *  capture holds), `maxChannels` sondes demodulated at once. */
    Receiver(double rate, double hwHz, double loHz, double hiHz, int maxChannels = 6)
        : rate_(rate), hw_(hwHz), maxCh_(std::max(1, maxChannels)) {
        const double span = std::max(std::fabs(loHz - hwHz), std::fabs(hiHz - hwHz)) + 15e3;
        double cur = rate;
        while (cur / 2 - span > span * 1.15) {
            const double out = cur / 2, trans = (out - 2 * span) / cur;
            hb_.emplace_back(new vibedsp::HalfbandDecimator(vibedsp::HalfbandDecimator::halfTapsFor(std::max(0.02, trans))));
            cur = out;
        }
        shared_ = cur;
        int n = 64;
        while (cur / n > 600.0) n *= 2;
        n_ = n; bin_ = cur / n;
        int cb = 8;
        while (cb * bin_ < kChanMinRate && cb < n) cb *= 2;
        cb_ = cb; chRate_ = cb * bin_;
        chz_.reset(new vibedsp::Channelizer(n));
        pw_.assign(size_t(n), 0.f);
        alpha_ = float(std::min(1.0, chz_->hop() / cur));     // ★ a ~1 s time constant
        lo_ = std::max(loHz, hwHz - 0.45 * cur);
        hi_ = std::min(hiHz, hwHz + 0.45 * cur);
        out_.resize(size_t(cb));
    }

    double rate() const { return rate_; }
    double sharedRate() const { return shared_; }
    double chanRate() const { return chRate_; }
    double binHz() const { return bin_; }
    int fftSize() const { return n_; }
    double lo() const { return lo_; }
    double hi() const { return hi_; }
    int maxChannels() const { return maxCh_; }

    struct ChanInfo { double hz; float levelDb; int frames; double openS; };
    std::vector<ChanInfo> channels() const {
        std::vector<ChanInfo> v;
        for (const auto& c : ch_) v.push_back({ c->hz, c->level, c->frames, now_ - c->opened });
        return v;
    }
    uint64_t opened() const { return opened_; }

    /** Feed capture samples; onFrame(const Frame&) on this thread for every frame decoded (fr.hz = the channel's
     *  centre after the frame's measured error). */
    template <class F> void feed(const cf32* iq, size_t n, F&& onFrame) {
        const size_t kBlk = 16384;
        for (size_t o = 0; o < n; o += kBlk) {
            const int m = int(std::min(kBlk, n - o));
            const cf32* src = iq + o;
            int cnt = m;
            for (auto& st : hb_) {
                b_.resize(size_t(cnt / 2 + 2));
                cnt = st->process(src, cnt, b_.data());
                a_.swap(b_); src = a_.data();
            }
            chz_->feed(src, cnt, [&](const cf32* bins, int) { block_(bins, onFrame); });
        }
    }

private:
    struct Chan {
        double hz = 0; int bin = 0; std::complex<double> rot{1, 0}, ph{1, 0};
        std::unique_ptr<ChanRx> rx;
        double opened = 0, seen = 0, lastFrame = -1; int frames = 0; float level = 0;
        double recentre = 0; bool want = false;
    };
    void place_(Chan& c, double hz) {
        c.hz = hz;
        const double off = hz - hw_;
        c.bin = int(std::lround(off / bin_));
        c.rot = std::polar(1.0, -2.0 * M_PI * (off - c.bin * bin_) / chRate_);
    }

    template <class F> void block_(const cf32* bins, F& onFrame) {
        const long long bi = chz_->blockIndex();
        now_ = double(bi) * chz_->hop() / shared_;
        powerEma(bins, pw_.data(), n_, alpha_);
        for (auto& cp : ch_) {
            Chan& c = *cp;
            const int k = chz_->extract(bins, c.bin, cb_, out_.data(), ctx_, bi);
            std::complex<double> p = c.ph;
            for (int i = 0; i < k; ++i) { out_[size_t(i)] *= cf32(float(p.real()), float(p.imag())); p *= c.rot; }
            c.ph = p / std::abs(p);
            c.rx->process(out_.data(), k, [&](Frame& fr) {
                fr.hz = c.hz + fr.offsetHz;
                ++c.frames; c.lastFrame = now_;
                c.recentre = std::max(-3000.0, std::min(3000.0, fr.offsetHz)); c.want = true;
                onFrame(static_cast<const Frame&>(fr));
            });
            if (c.want) { c.want = false; place_(c, c.hz + c.recentre); }
        }
        if (now_ - lastScan_ >= 0.5 && now_ >= 1.0) { lastScan_ = now_; scan_(); }
    }

    /** ★ Twice a second: sonde-shaped peaks in the smoothed power; channels opened, kept, parked and closed. */
    void scan_() {
        const int kLo = int(std::ceil((lo_ - hw_) / bin_)), kHi = int(std::floor((hi_ - hw_) / bin_));
        const int L = kHi - kLo + 1;
        const int W = std::max(5, int(std::lround(10e3 / bin_)) | 1), h = W / 2;
        if (L < 3 * W) return;
        P_.resize(size_t(L));
        for (int i = 0; i < L; ++i) P_[size_t(i)] = pw_[size_t((kLo + i) & (n_ - 1))];
        srt_ = P_;
        std::nth_element(srt_.begin(), srt_.begin() + L / 2, srt_.end());
        const double fl = std::max(1e-20, double(srt_[size_t(L / 2)]));
        cum_.assign(size_t(L) + 1, 0.0);
        for (int i = 0; i < L; ++i) cum_[size_t(i) + 1] = cum_[size_t(i)] + P_[size_t(i)];
        auto box = [&](int c) { const int a = std::max(0, c - h), b = std::min(L - 1, c + h); return cum_[size_t(b) + 1] - cum_[size_t(a)] - fl * (b - a + 1); };
        struct Cand { double hz; float db; };
        std::vector<Cand> cands;
        const double thr = std::pow(10.0, 2.0 / 10.0) - 1.0;   // ★ +2 dB over the floor in the 10 kHz box
        for (int c = h + W; c < L - h - W; ++c) {
            const double b = box(c);
            if (b <= thr * fl * W) continue;
            bool peak = true;
            for (int d = -W; d <= W && peak; ++d) if (d && box(c + d) > b) peak = false;
            if (!peak) continue;
            // ★ not a carrier: the middle three bins hold < 60 % of the excess; not a broad block: each flank < 40 %
            double mid = 0; for (int d = -1; d <= 1; ++d) mid += P_[size_t(c + d)] - fl;
            if (mid > 0.6 * b) continue;
            if (box(c - W) > 0.4 * b || box(c + W) > 0.4 * b) continue;
            double num = 0, den = 0;
            for (int d = -h; d <= h; ++d) { const double e = std::max(0.0, double(P_[size_t(c + d)]) - fl); num += e * d; den += e; }
            const double k = kLo + c + (den > 0 ? num / den : 0);
            cands.push_back({ hw_ + k * bin_, float(10 * std::log10(1 + b / (fl * W))) });
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.db > b.db; });
        // the channels: seen again, closed, merged
        for (auto& cp : ch_) for (const auto& x : cands) if (std::fabs(x.hz - cp->hz) < 6e3) { cp->seen = now_; cp->level = x.db; break; }
        for (size_t i = 0; i < ch_.size(); ++i)
            for (size_t j = i + 1; j < ch_.size(); ++j)
                if (std::fabs(ch_[i]->hz - ch_[j]->hz) < 3e3) { ch_[ch_[i]->frames >= ch_[j]->frames ? j : i]->seen = -1e9; ch_[ch_[i]->frames >= ch_[j]->frames ? j : i]->lastFrame = -1e9; }
        ch_.erase(std::remove_if(ch_.begin(), ch_.end(), [&](const std::unique_ptr<Chan>& c) {
            return now_ - std::max(c->seen, c->lastFrame) > 15.0; }), ch_.end());
        parked_.erase(std::remove_if(parked_.begin(), parked_.end(), [&](const std::pair<double, double>& p) { return p.second < now_; }), parked_.end());
        for (const auto& x : cands) {
            bool have = false;
            for (auto& cp : ch_) if (std::fabs(x.hz - cp->hz) < 6e3) have = true;
            for (auto& p : parked_) if (std::fabs(x.hz - p.first) < 6e3) have = true;
            if (have) continue;
            if (int(ch_.size()) >= maxCh_) {
                // ★ full: the channel that has decoded nothing longest gives its slot up (parked for a minute)
                auto it = ch_.end();
                for (auto c = ch_.begin(); c != ch_.end(); ++c)
                    if ((*c)->frames == 0 && now_ - (*c)->opened > 20.0 && (it == ch_.end() || (*c)->opened < (*it)->opened)) it = c;
                if (it == ch_.end()) break;
                parked_.push_back({ (*it)->hz, now_ + 60.0 });
                ch_.erase(it);
            }
            std::unique_ptr<Chan> c(new Chan);
            c->rx.reset(new ChanRx(chRate_));
            place_(*c, x.hz);
            c->opened = c->seen = now_; c->level = x.db;
            ch_.push_back(std::move(c));
            ++opened_;
        }
    }

    double rate_, hw_, shared_ = 0, bin_ = 0, chRate_ = 0, lo_ = 0, hi_ = 0;
    int maxCh_, n_ = 0, cb_ = 0;
    float alpha_ = 0;
    std::vector<std::unique_ptr<vibedsp::HalfbandDecimator>> hb_;
    std::unique_ptr<vibedsp::Channelizer> chz_;
    vibedsp::Channelizer::ExtractCtx ctx_;
    std::vector<float> pw_, P_, srt_;
    std::vector<double> cum_;
    std::vector<cf32> a_, b_, out_;
    std::vector<std::unique_ptr<Chan>> ch_;
    std::vector<std::pair<double, double>> parked_;
    double now_ = 0, lastScan_ = 0;
    uint64_t opened_ = 0;
};

}  // namespace vibers41
