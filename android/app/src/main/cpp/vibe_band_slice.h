// vibe_band_slice.h — how wide a Band-spots / skimmer IQ slice is cut from the channeliser (V12).
//
// ONE RULE, used by both readers: the per-listener extract (local_sdr_shim.cpp feedOneClient) and the one-pipeline tap
// (vibe_wide_tap.h). They had a copy each.
#pragma once
#include "decoders/digi_bands.h"
#include <algorithm>
#include <cmath>

namespace vibe {

/** The slice's size in bins (a power of two, at most the FFT) for a wanted width. */
inline int bandSliceBins(double sampleRate, int fftSize, double widthHz) {
    int b = 64;
    while (b < fftSize && sampleRate * (double)b / (double)fftSize < widthHz / 0.92) b <<= 1;
    return std::min(b, fftSize);
}

} // namespace vibe

/** The IQ width Band mode wants for a dial (DecoderHost::bandWidthFor). */
inline double DecoderHostBandWidth(double dialHz) {
    using namespace vibe;
    const digi::Band* b = digi::bandAt(dialHz);
    if (!b) return 24000.0;
    const digi::Span sp = digi::spanOf(*b, digi::modes());
    if (!sp.ok) return 24000.0;
    return std::max(4000.0, 2.0 * std::max(std::fabs(sp.hiHz - dialHz), std::fabs(dialHz - sp.loHz)));
}
