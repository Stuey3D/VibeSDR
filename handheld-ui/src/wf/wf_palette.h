// wf_palette.h — the waterfall's 256-entry colour LUTs. The built-ins are the app's own (wf_palettes_gen.h is generated
// from src/assets/colormapUtils.ts getColorLUT by test/gen_palettes.ts). Custom palettes are built from colour stops with
// the app's own interpolation (colormapUtils lutStops / lutPos, ported) — the colour customisation.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wf {

struct Palette { uint8_t rgba[256 * 4]; };

int paletteCount();
const char* paletteName(int id);
/** By name (the app's NAME_ALIASES too: 'Sonar' → 'Sonar Green', 'green' → 'Classic Green'); unknown → gqrx, as the app. */
int paletteByName(const std::string& name);
Palette builtinPalette(int id);
/** colormapUtils lutStops: evenly spaced stops ("#rrggbb" or 0xRRGGBB). */
Palette paletteFromStops(const std::vector<uint32_t>& rgbStops);
/** colormapUtils lutPos: stops at explicit indices 0..255. */
Palette paletteFromPositions(const std::vector<std::pair<int, uint32_t>>& stops);
/** The spectrum fill's 9 gradient stops, top (hot) → bottom (WaterfallView specGradColors: LUT 90 → 235, reversed). */
void spectrumGradient(const Palette& p, uint8_t out[9][3]);

}  // namespace wf
