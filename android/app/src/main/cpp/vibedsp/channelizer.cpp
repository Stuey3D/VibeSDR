// VibeSDR — Channelizer: fast convolution (overlap-save) channel extraction.
//
// See the class note in vibedsp.h for WHY. This file is the how.
//
// The architecture is ka9q-radio's: one forward FFT of the whole band, and every channel is a
// slice of those bins inverse-transformed at its own rate. Written 2026-08-02 for VibeServer's
// multi-listener mode, where a per-client DDC would put a complex multiply per INPUT sample on
// every listener.
#include "vibedsp.h"
#include <cmath>
#include <cstring>

namespace vibedsp {

Channelizer::Channelizer(int fftSize)
    : n_(fftSize), fwd_(fftSize, /*inverse=*/false) {
    hist_.assign(n_, cf32(0.0f, 0.0f));
    block_.assign(n_, cf32(0.0f, 0.0f));
    spec_.assign(n_, cf32(0.0f, 0.0f));
    have_ = 0;
}

void Channelizer::feed(const cf32* in, int n,
                       const std::function<void(const cf32*, int)>& onBlock) {
    const int ov  = n_ / OVERLAP_DIV;      // samples carried between blocks
    const int hop = n_ - ov;               // new samples needed to complete a block
    int i = 0;
    while (i < n) {
        const int want = hop - have_;
        const int take = (n - i) < want ? (n - i) : want;
        std::memcpy(&hist_[ov + have_], in + i, (size_t)take * sizeof(cf32));
        have_ += take;
        i     += take;
        if (have_ < hop) return;           // not a full block yet

        // ★ The block is [overlap from last time][hop new samples] — that IS overlap-save: the
        //   history is what makes the convolution linear rather than circular once the first
        //   `ov/D` output samples of each channel are thrown away.
        fwd_.forward(hist_.data(), spec_.data());
        onBlock(spec_.data(), n_);
        ++blocks_;

        // Carry the LAST `ov` samples of this block forward as the next block's history.
        std::memmove(hist_.data(), &hist_[n_ - ov], (size_t)ov * sizeof(cf32));
        have_ = 0;
    }
}

int Channelizer::extract(const cf32* bins, int centreBin, int chanBins, cf32* out) {
    if (chanBins <= 0 || chanBins > n_ || (n_ % chanBins) != 0) return 0;

    auto it = inv_.find(chanBins);
    if (it == inv_.end())
        it = inv_.emplace(chanBins, std::make_unique<ComplexFFT>(chanBins, /*inverse=*/true)).first;
    ComplexFFT& inv = *it->second;

    if ((int)slice_.size() < chanBins) slice_.resize(chanBins);

    // ★★ GATHER THE CHANNEL'S BINS, WRAPPING. The slice is centred on `centreBin` and laid out
    //    for the inverse transform in the SAME convention the forward one produced: DC of the
    //    channel at slice index 0, positive frequencies ascending, negatives at the top. Getting
    //    this wrong does not fail loudly — it produces a channel tuned somewhere else, which is
    //    why test_channelizer checks the recovered tone's FREQUENCY and not just its presence.
    const int half = chanBins / 2;
    for (int k = 0; k < chanBins; ++k) {
        // k: 0..half-1 = positive offsets, half..chanBins-1 = negative offsets
        const int off = (k < half) ? k : k - chanBins;
        int src = centreBin + off;
        src %= n_;
        if (src < 0) src += n_;
        slice_[k] = bins[src];
    }

    // ★★★ THE CHANNEL'S TRANSFER FUNCTION — NOT AN OPTIONAL POLISH. ka9q's constraint is that the
    //     channel filter's impulse response must be SHORTER THAN THE OVERLAP. A rectangular block
    //     of bins is a brick wall in frequency, whose impulse response is a SINC — infinitely long
    //     — so the constraint is violated outright and consecutive blocks can never join cleanly,
    //     however well their phase is matched. The join discontinuity then radiates spurs at the
    //     BLOCK RATE, fs/(hop) = a fixed 325 Hz here, which is invisible on a wide view and becomes
    //     a picket fence once the span is narrow enough to resolve it (Stuart, 2026-08-02: "when
    //     you zoom right in at the final 2 zoom levels you get these noise lines").
    //     ★ A raised-cosine roll-off over the outer quarter of the slice shortens the impulse
    //       response into the overlap. The caller asks for a WIDER channel than it needs and crops
    //       the tapered edges away, so nothing the user sees is attenuated.
    {
        const int edge = chanBins / 4;                 // bins of roll-off at each band edge
        for (int e = 0; e < edge; ++e) {
            const float w = 0.5f * (1.0f - std::cos((float)M_PI * (float)(e + 1) / (float)(edge + 1)));
            // The slice is laid out DC-first, so the band edges are the two sides of its middle.
            const int hi = chanBins / 2 - 1 - e;       // top of the positive half
            const int lo = chanBins / 2 + e;           // bottom of the negative half
            if (hi >= 0)        slice_[hi] *= w;
            if (lo < chanBins)  slice_[lo] *= w;
        }
    }

    // ★ SCALE. KissFFT normalises neither direction, so a forward of n_ followed by an inverse of
    //   chanBins leaves a gain of n_. Dividing by n_ makes a unit-amplitude input tone come back
    //   out at unit amplitude, which is what every caller expects and what the test asserts.
    const float g = 1.0f / (float)n_;
    for (int k = 0; k < chanBins; ++k) slice_[k] *= g;

    // ★★★ PHASE-CONTINUE THE CHANNEL ACROSS BLOCKS. Each block is transformed independently, so
    //     the extracted channel's phase reference RESTARTS every block. The true signal at
    //     `centreBin` advances by 2*pi*centreBin*hop/n_ between blocks, and unless that is put
    //     back, every block boundary is a phase DISCONTINUITY. Repeating at the block rate, that
    //     radiates a COMB OF SPURS either side of any strong signal — which is what it looked
    //     like on air: a picket fence spreading +/-10 kHz around DDK (Stuart, 2026-08-02, "the
    //     detail is terrible"). It also puts a small error on the recovered frequency, and THAT
    //     is how it was first caught — by sweeping centreBin in the tests. Judging it by the
    //     frequency error alone badly understated it: the audible/visible damage is the comb.
    //     ★ Zero whenever centreBin is a multiple of OVERLAP_DIV, which is why the original
    //       test — centre bin 512 — never saw it. Sweep, do not sample.
    {
        const double turns = std::fmod((double)centreBin * (double)blocks_
                                       * (double)hop() / (double)n_, 1.0);
        if (turns != 0.0) {
            const double a = -2.0 * M_PI * turns;
            const cf32 rot((float)std::cos(a), (float)std::sin(a));
            for (int k = 0; k < chanBins; ++k) slice_[k] *= rot;
        }
    }

    inv.forward(slice_.data(), out);       // cfg was built inverse — this IS the inverse transform

    // ★★★ DISCARD THE CORRUPTED HEAD. Overlap-save's whole trick: the first `ov/D` output samples
    //     of every block are the circular-convolution wrap-around, and only what follows is the
    //     linear result. Keeping them would put a click at every block boundary.
    const int drop = chanBins / OVERLAP_DIV;
    const int keep = chanBins - drop;
    std::memmove(out, out + drop, (size_t)keep * sizeof(cf32));
    return keep;
}

}  // namespace vibedsp
