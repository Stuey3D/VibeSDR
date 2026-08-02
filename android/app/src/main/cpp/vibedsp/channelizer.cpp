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

    // ★ SCALE. KissFFT normalises neither direction, so a forward of n_ followed by an inverse of
    //   chanBins leaves a gain of n_. Dividing by n_ makes a unit-amplitude input tone come back
    //   out at unit amplitude, which is what every caller expects and what the test asserts.
    const float g = 1.0f / (float)n_;
    for (int k = 0; k < chanBins; ++k) slice_[k] *= g;

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
