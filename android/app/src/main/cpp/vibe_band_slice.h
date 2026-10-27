// vibe_band_slice.h — Band spots' (and the CW/PSK skimmers') IQ SLICES off the channeliser: how wide, how flat, and
// where the extra ones go (V12, 2026-10-27).
//
// ★★ ONE RULE, THREE READERS: the per-listener extract (local_sdr_shim.cpp feedOneClient), the one-pipeline tap
//    (vibe_wide_tap.h) and the plan (digi::DigiTuner::fits). They had a copy each, and the copies disagreed: the slice was
//    sized as if only its outer 4 % rolled off (w / 0.92) while Channelizer::extract rolled off the outer QUARTER on each
//    side — so 40 m's WSPR/PSK/JS8 sat 2–3 dB down the taper at 960 kS/s and JTTY 8–17 dB down at 2.4 MS/s.
//    ★ The slice now takes a narrow roll-off (kTaperDiv: a sixteenth a side → flat over 7/8 of its rate) and is sized
//      for that flat part. Measured (test-band-slice): every mode's passband edge within 0.5 dB at every capture rate,
//      block-rate spurs below −90 dB — the same cost as before (the bins a slice needs did not change on any band at
//      2.4 MS/s; at 960 kS/s 40 m goes 30 → 60 kHz — one inverse FFT of 64 → 128 points per block, nothing).
#pragma once
#include "decoders/digi_bands.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace vibe {

/** The slice's roll-off: chanBins / kTaperDiv bins at each edge (Channelizer::extract's `taperDiv`). */
constexpr int kBandTaperDiv = 16;
/** What part of the slice's rate is flat: 1 − 2/kTaperDiv, less a cushion for the raised cosine's first bin. */
constexpr double kBandFlatFrac = 1.0 - 2.0 / kBandTaperDiv - 0.01;
static_assert(kBandTaperDiv == 16, "digi::DigiTuner::kFlatHalfFrac assumes a sixteenth a side");

/** The slice's size in bins (a power of two, at most the FFT) for a wanted (flat) width. */
inline int bandSliceBins(double sampleRate, int fftSize, double widthHz) {
    int b = 64;
    while (b < fftSize && sampleRate * (double)b / (double)fftSize * kBandFlatFrac < widthHz) b <<= 1;
    return std::min(b, fftSize);
}

/** ★ One extra slice: its RF centre and the flat width it needs. */
struct ExtraSlice { double centreHz = 0, widthHz = 0; };
constexpr int kMaxExtraSlices = 3;

/** The extra slices a Band run on `dialHz` wants (the band's clusters beyond its main one — digi::Span::extras). */
inline std::vector<ExtraSlice> bandExtraSlices(double dialHz) {
    std::vector<ExtraSlice> o;
    const digi::Band* b = digi::bandAt(dialHz);
    if (!b) return o;
    const digi::Span sp = digi::spanOf(*b, digi::modes());
    if (!sp.ok) return o;
    for (const digi::SubSpan& e : sp.extras) {
        if ((int)o.size() >= kMaxExtraSlices) break;
        o.push_back({ e.centreHz, e.widthHz() });
    }
    return o;
}

/** ★ Can a capture centred on `hwCentreHz` at `sampleRate` hold a slice centred on `centreHz` with `rate`? Its whole width
 *  inside the inner 90 % of the capture (an RTL's and an Airspy's edges are filter skirt). */
inline bool captureHolds(double sampleRate, double hwCentreHz, double centreHz, double rate) {
    return std::fabs(centreHz - hwCentreHz) + rate / 2.0 <= sampleRate * 0.45;
}

} // namespace vibe

/** The IQ width Band mode wants for a dial (DecoderHost::bandWidthFor): the registered modes' main span of the band the
 *  dial is on, wide enough either side of the dial to hold it all. Off any band: 24 kHz. */
inline double DecoderHostBandWidth(double dialHz) {
    using namespace vibe;
    const digi::Band* b = digi::bandAt(dialHz);
    if (!b) return 24000.0;
    const digi::Span sp = digi::spanOf(*b, digi::modes());
    if (!sp.ok) return 24000.0;
    return std::max(4000.0, 2.0 * std::max(std::fabs(sp.hiHz - dialHz), std::fabs(dialHz - sp.loHz)));
}
