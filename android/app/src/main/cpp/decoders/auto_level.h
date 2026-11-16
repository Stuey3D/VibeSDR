// auto_level.h — the picture decoders' one auto-level rule: the 2nd and 98th percentiles of a 256-bin histogram.
// WEFAX stretches that range to 0…255 as each line is drawn (wefax_decoder.cpp updateAutoLevel); SSDV measures its
// pictures' contrast with it when a picture completes (balloon_ssdv.h Image::levels_). One place, so both read a picture
// the same way.
#pragma once
#include <cstdint>

namespace vibe {

/** `lo` / `hi`: the values `pct` % up from the bottom and down from the top of `hist` (n samples in it). hi > lo always. */
inline void autoLevelRange(const uint32_t hist[256], uint64_t n, int& lo, int& hi, int pct = 2) {
    const uint64_t cut = n * (uint64_t)pct / 100;
    uint64_t acc = 0; lo = 0; hi = 255;
    for (int v = 0; v < 256; v++)  { acc += hist[v]; if (acc >= cut) { lo = v; break; } }
    acc = 0;
    for (int v = 255; v >= 0; v--) { acc += hist[v]; if (acc >= cut) { hi = v; break; } }
    if (hi <= lo) hi = lo + 1;
}

} // namespace vibe
