// vibe_avi_front.h — V12 aviation datalink: MANY FIXED CHANNELS FROM ONE CAPTURE (ACARS, VDL Mode 2).
// docs/v12/AVIATION.md.
//
// ★★★ ONE SHARED CHAIN, FLAT IN THE CHANNEL COUNT. The capture is cut down by a cascade of half-band decimators (vibedsp's
//     HalfbandDecimator: NEON on both ARMs, SSE2 on x86, scalar elsewhere — a few instructions an input sample) until it
//     is only as wide as the channel list needs, then ONE forward FFT a block feeds every channel (vibedsp::Channelizer,
//     overlap-save fast convolution, PFFFT). Each channel is a slice of those bins inverse-transformed at its own rate —
//     one small FFT a block, whatever the capture rate. A per-channel full-rate mixer (one complex multiply an input
//     sample a channel) is what makes a Pi drop channels; here a fourth channel costs a 32- or 64-point FFT.
// ★★ SELECTIVE. The channel filter is the slice itself: flat over half the channel's rate, a raised-cosine roll-off over
//    a quarter at each edge, nothing outside. A neighbour 25 kHz away is outside the slice, so it cannot leak in (the
//    integrate-and-dump front ends' sinc side lobes are only ~13 dB down — the "the same message on two channels" fault).
// ★ The slice is centred on the nearest BIN; the rest of the offset (at most half a bin, ~300 Hz) is taken out by a tiny
//   NCO at the channel rate, so every channel comes out centred on its own frequency.
#pragma once
#include "vibedsp/vibedsp.h"
#include "vibedsp/simd_internal.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace vibeavi {

using cf32 = vibedsp::cf32;

/** out[k] = a[k] · b[k] · r (complex). NEON / SSE2 / scalar; the same products in the same order on all three. */
inline void cmulRun(const cf32* a, const cf32* b, cf32 r, cf32* out, int n) {
    const float* af = reinterpret_cast<const float*>(a); const float* bf = reinterpret_cast<const float*>(b);
    float* of = reinterpret_cast<float*>(out);
    int k = 0;
#if VIBE_NEON
    const float32x4_t rr = vdupq_n_f32(r.real()), ri = vdupq_n_f32(r.imag());
    for (; k + 4 <= n; k += 4) {
        const float32x4x2_t x = vld2q_f32(af + 2 * k), y = vld2q_f32(bf + 2 * k);
        const float32x4_t pr = vsubq_f32(vmulq_f32(x.val[0], y.val[0]), vmulq_f32(x.val[1], y.val[1]));
        const float32x4_t pi = vaddq_f32(vmulq_f32(x.val[0], y.val[1]), vmulq_f32(x.val[1], y.val[0]));
        float32x4x2_t o;
        o.val[0] = vsubq_f32(vmulq_f32(pr, rr), vmulq_f32(pi, ri));
        o.val[1] = vaddq_f32(vmulq_f32(pr, ri), vmulq_f32(pi, rr));
        vst2q_f32(of + 2 * k, o);
    }
#elif VIBE_SSE
    const __m128 rr = _mm_set1_ps(r.real()), ri = _mm_set1_ps(r.imag());
    for (; k + 4 <= n; k += 4) {
        __m128 xr, xi, yr, yi;
        vibedsp::sseLoad2(af + 2 * k, xr, xi); vibedsp::sseLoad2(bf + 2 * k, yr, yi);
        const __m128 pr = _mm_sub_ps(_mm_mul_ps(xr, yr), _mm_mul_ps(xi, yi));
        const __m128 pi = _mm_add_ps(_mm_mul_ps(xr, yi), _mm_mul_ps(xi, yr));
        vibedsp::sseStore2(of + 2 * k, _mm_sub_ps(_mm_mul_ps(pr, rr), _mm_mul_ps(pi, ri)), _mm_add_ps(_mm_mul_ps(pr, ri), _mm_mul_ps(pi, rr)));
    }
#endif
    for (; k < n; ++k) {
        const float pr = af[2 * k] * bf[2 * k] - af[2 * k + 1] * bf[2 * k + 1], pi = af[2 * k] * bf[2 * k + 1] + af[2 * k + 1] * bf[2 * k];
        of[2 * k] = pr * r.real() - pi * r.imag(); of[2 * k + 1] = pr * r.imag() + pi * r.real();
    }
}

/** Σ t[j]·z[j], j < 8 (real taps, complex samples). NEON / SSE2 / scalar, the same pairwise order on all three. */
inline cf32 dot8(const float* t, const cf32* z) {
    const float* zf = reinterpret_cast<const float*>(z);
#if VIBE_NEON
    const float32x4x2_t a = vld2q_f32(zf), b = vld2q_f32(zf + 8);
    const float32x4_t t0 = vld1q_f32(t), t1 = vld1q_f32(t + 4);
    const float32x4_t re = vaddq_f32(vmulq_f32(t0, a.val[0]), vmulq_f32(t1, b.val[0]));
    const float32x4_t im = vaddq_f32(vmulq_f32(t0, a.val[1]), vmulq_f32(t1, b.val[1]));
    const float32x2_t r2 = vadd_f32(vget_low_f32(re), vget_high_f32(re)), i2 = vadd_f32(vget_low_f32(im), vget_high_f32(im));
    return cf32(vget_lane_f32(r2, 0) + vget_lane_f32(r2, 1), vget_lane_f32(i2, 0) + vget_lane_f32(i2, 1));
#elif VIBE_SSE
    __m128 ar, ai, br, bi;
    vibedsp::sseLoad2(zf, ar, ai); vibedsp::sseLoad2(zf + 8, br, bi);
    const __m128 t0 = _mm_loadu_ps(t), t1 = _mm_loadu_ps(t + 4);
    const __m128 re = _mm_add_ps(_mm_mul_ps(t0, ar), _mm_mul_ps(t1, br)), im = _mm_add_ps(_mm_mul_ps(t0, ai), _mm_mul_ps(t1, bi));
    alignas(16) float r[4], i[4]; _mm_store_ps(r, re); _mm_store_ps(i, im);
    return cf32((r[0] + r[2]) + (r[1] + r[3]), (i[0] + i[2]) + (i[1] + i[3]));
#else
    float r[4], i[4];
    for (int l = 0; l < 4; ++l) { r[l] = t[l] * zf[2 * l] + t[l + 4] * zf[2 * l + 8]; i[l] = t[l] * zf[2 * l + 1] + t[l + 4] * zf[2 * l + 9]; }
    return cf32((r[0] + r[2]) + (r[1] + r[3]), (i[0] + i[2]) + (i[1] + i[3]));
#endif
}

class MultiChannel {
public:
    /** `rate` the capture rate, `hwCentreHz` where the radio is tuned, `chanHz` the channels wanted (absolute Hz),
     *  `halfWidthHz` how much either side of each channel must pass, `minChanRate` the lowest channel rate accepted,
     *  `maxBinHz` the coarsest FFT bin accepted. Channels outside the capture are kept in the list but never fed. */
    MultiChannel(double rate, double hwCentreHz, const std::vector<double>& chanHz, double halfWidthHz,
                 double minChanRate, double maxBinHz = 600.0)
        : rate_(rate), hw_(hwCentreHz), chanHz_(chanHz) {
        double span = halfWidthHz;
        for (double h : chanHz) span = std::max(span, std::fabs(h - hwCentreHz) + halfWidthHz);
        span_ = span;
        // ★ half-band stages while the next rate still holds the span: pass ±span, stop from (out − span) so nothing
        //   that folds lands inside it.
        double cur = rate;
        while (cur / 2 - span > span * 1.15 && cur / 2 >= minChanRate * 2) {
            const double out = cur / 2, trans = (out - 2 * span) / cur;
            hb_.emplace_back(new vibedsp::HalfbandDecimator(vibedsp::HalfbandDecimator::halfTapsFor(std::max(0.02, trans))));
            cur = out;
        }
        shared_ = cur;
        int n = 64;
        while (cur / n > maxBinHz) n *= 2;
        n_ = n;
        bin_ = cur / n;
        int cb = 8;
        while (cb * bin_ < minChanRate && cb < n) cb *= 2;
        chanBins_ = cb;
        chRate_ = cb * bin_;
        chz_.reset(new vibedsp::Channelizer(n));
        ctx_.reset(new vibedsp::Channelizer::ExtractCtx);
        for (double h : chanHz) {
            Ch c;
            const double off = h - hwCentreHz;
            c.inside = std::fabs(off) + halfWidthHz < cur / 2;
            c.bin = int(std::lround(off / bin_));
            const double resid = off - c.bin * bin_;
            c.rot = std::polar(1.0, -2.0 * M_PI * resid / chRate_);
            ch_.push_back(std::move(c));
        }
    }

    double rate() const { return rate_; }
    double sharedRate() const { return shared_; }
    double chanRate() const { return chRate_; }
    double binHz() const { return bin_; }
    int fftSize() const { return n_; }
    int chanBins() const { return chanBins_; }
    int stages() const { return int(hb_.size()); }
    size_t channels() const { return ch_.size(); }
    bool inside(size_t c) const { return ch_[c].inside; }
    double chanHz(size_t c) const { return chanHz_[c]; }

    /** ★★ A CHANNEL'S OWN RESPONSE IN THE FREQUENCY DOMAIN (V12 VDL2, 2026-11-01). `h(fHz)` is sampled on this channel's
     *  bins (f relative to its centre) and applied to the slice before its inverse FFT, with a linear phase of half the
     *  discarded head so the response's both tails land where overlap-save throws samples away. It REPLACES a
     *  time-domain FIR after the channel (VDL2's 58-tap receive low-pass was 22 % of its whole Pi 2 instruction count):
     *  the multiply is one per bin a block instead of 58 per output. Without a response a channel keeps the slicer's
     *  standard quarter-taper (ACARS, HFDL unchanged). */
    template <class H> void setResponse(size_t c, H&& h) {
        Ch& ch = ch_[c];
        ch.w.assign(size_t(chanBins_), cf32(0, 0));
        const int half = chanBins_ / 2, d = chanBins_ / (2 * vibedsp::Channelizer::OVERLAP_DIV);
        for (int k = 0; k < chanBins_; ++k) {
            const int off = k < half ? k : k - chanBins_;
            const double g = h(off * bin_) / double(n_);
            const double a = -2.0 * M_PI * double(off) * double(d) / double(chanBins_);
            ch.w[size_t(k)] = cf32(float(g * std::cos(a)), float(g * std::sin(a)));
        }
        if (!ch.inv) ch.inv.reset(new vibedsp::ComplexFFT(chanBins_, /*inverse=*/true));
    }

    /** Per-thread scratch for slice() (a worker owns one). */
    struct Scratch { vibedsp::Channelizer::ExtractCtx ctx; std::vector<cf32> sl, gat; };

    /** ★ THE SHARED FRONT END ONLY: the half-bands and the one forward FFT. `onBlock(const cf32* bins, long long bi)`
     *  once a block; the bins are valid during the call. (The workers slice their own channels from a copy.) */
    template <class OnBlock> void feedBins(const cf32* iq, size_t n, OnBlock&& onBlock) {
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
            chz_->feed(src, cnt, [&](const cf32* bins, int) { onBlock(bins, chz_->blockIndex()); });
        }
    }
    /** One channel from a block's bins (its slice, its response, its sub-bin rotator) into `out` (≥ chanBins).
     *  Returns the samples written. A channel's state is touched only here, so one thread per channel at a time. */
    int slice(size_t c, const cf32* bins, long long bi, cf32* out, Scratch& sc) {
        Ch& h = ch_[c];
        if (!h.inside) return 0;
        const int k = h.w.empty() ? chz_->extract(bins, h.bin, chanBins_, out, sc.ctx, bi) : slice_(h, bins, bi, out, sc);
        std::complex<double> p = h.ph;
        const std::complex<double> r = h.rot;
        for (int i = 0; i < k; ++i) { out[i] *= cf32(float(p.real()), float(p.imag())); p *= r; }
        h.ph = p / std::abs(p);
        return k;
    }

    /** `out(int channel, const cf32* z, int n)`, once per channel per block (in channel order), on this thread. */
    template <class Out> void feed(const cf32* iq, size_t n, Out&& out) {
        ob_.resize(size_t(chanBins_));
        feedBins(iq, n, [&](const cf32* bins, long long bi) {
            for (size_t c = 0; c < ch_.size(); ++c) {
                const int k = slice(c, bins, bi, ob_.data(), own_);
                if (k > 0) out(int(c), ob_.data(), k);
            }
        });
    }

private:
    struct Ch {
        int bin = 0; bool inside = false; std::complex<double> rot{1, 0}, ph{1, 0};
        std::vector<cf32> w;                       // its own response (setResponse), empty = the standard taper
        std::unique_ptr<vibedsp::ComplexFFT> inv;
    };
    /** The slice × the channel's response × the block's phase reference, inverse-transformed; the head dropped. */
    int slice_(Ch& h, const cf32* bins, long long bi, cf32* out, Scratch& sc) {
        const int cb = chanBins_, half = cb / 2, m = n_ - 1;
        std::vector<cf32>& sl_ = sc.sl; std::vector<cf32>& gat_ = sc.gat;
        sl_.resize(size_t(cb));
        // the block's phase reference (overlap-save restarts each block's time origin), folded into the response
        const double turns = std::fmod(double(h.bin) * double(bi) * double(chz_->hop()) / double(n_), 1.0);
        const cf32 rot(float(std::cos(-2 * M_PI * turns)), float(std::sin(-2 * M_PI * turns)));
        gat_.resize(size_t(cb));
        for (int k = 0; k < cb; ++k) gat_[size_t(k)] = bins[(h.bin + (k < half ? k : k - cb)) & m];
        cmulRun(gat_.data(), h.w.data(), rot, sl_.data(), cb);
        h.inv->forward(sl_.data(), out);
        const int drop = cb / vibedsp::Channelizer::OVERLAP_DIV, keep = cb - drop;
        std::memmove(out, out + drop, size_t(keep) * sizeof(cf32));
        return keep;
    }
    Scratch own_;
    double rate_, hw_, shared_ = 0, bin_ = 0, chRate_ = 0, span_ = 0;
    int n_ = 0, chanBins_ = 0;
    std::vector<double> chanHz_;
    std::vector<std::unique_ptr<vibedsp::HalfbandDecimator>> hb_;
    std::unique_ptr<vibedsp::Channelizer> chz_;
    std::unique_ptr<vibedsp::Channelizer::ExtractCtx> ctx_;
    std::vector<Ch> ch_;
    std::vector<cf32> a_, b_, ob_;
};

/** RTL u8 IQ → cf32 (centred on 127.4, /128). NEON/SSE2 come from the server's own conversion; this is for tools. */
inline void u8ToCf(const uint8_t* in, size_t pairs, cf32* out) {
    for (size_t i = 0; i < pairs; ++i) out[i] = cf32((in[2 * i] - 127.4f) * (1.0f / 128), (in[2 * i + 1] - 127.4f) * (1.0f / 128));
}

}  // namespace vibeavi
