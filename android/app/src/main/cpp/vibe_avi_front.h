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

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace vibeavi {

using cf32 = vibedsp::cf32;

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
            ch_.push_back(c);
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

    /** `out(int channel, const cf32* z, int n)`, once per channel per block (in channel order). */
    template <class Out> void feed(const cf32* iq, size_t n, Out&& out) {
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
            chz_->feed(src, cnt, [&](const cf32* bins, int) {
                const long long bi = chz_->blockIndex();
                ob_.resize(size_t(chanBins_));
                for (size_t c = 0; c < ch_.size(); ++c) {
                    Ch& h = ch_[c];
                    if (!h.inside) continue;
                    const int k = chz_->extract(bins, h.bin, chanBins_, ob_.data(), *ctx_, bi);
                    // the residual offset: a rotator at the channel rate (renormalised each block)
                    std::complex<double> p = h.ph;
                    const std::complex<double> r = h.rot;
                    for (int i = 0; i < k; ++i) {
                        ob_[size_t(i)] *= cf32(float(p.real()), float(p.imag()));
                        p *= r;
                    }
                    h.ph = p / std::abs(p);
                    out(int(c), ob_.data(), k);
                }
            });
        }
    }

private:
    struct Ch { int bin = 0; bool inside = false; std::complex<double> rot{1, 0}, ph{1, 0}; };
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
