// wf_palette.cpp — built-in LUTs (generated) + ports of colormapUtils.ts lutStops / lutPos and WaterfallView's
// specGradColors.
#include "wf_palette.h"
#include "wf_palettes_gen.h"
#include "wf_row_place.h"   // jsRound
#include <algorithm>
#include <cmath>

namespace wf {

int paletteCount() { return PALETTE_COUNT; }
const char* paletteName(int id) { return id >= 0 && id < PALETTE_COUNT ? PALETTE_NAMES[id] : PALETTE_NAMES[0]; }
int paletteByName(const std::string& raw) {
  std::string name = raw;
  if (raw == "Sonar" || raw == "sonar") name = "Sonar Green";
  else if (raw == "green") name = "Classic Green";
  for (int i = 0; i < PALETTE_COUNT; i++) if (name == PALETTE_NAMES[i]) return i;
  return 0;   // gqrx
}
Palette builtinPalette(int id) {
  if (id < 0 || id >= PALETTE_COUNT) id = 0;
  Palette p;
  for (int i = 0; i < 256; i++) {
    p.rgba[i * 4 + 0] = PALETTE_RGB[id][i * 3 + 0]; p.rgba[i * 4 + 1] = PALETTE_RGB[id][i * 3 + 1];
    p.rgba[i * 4 + 2] = PALETTE_RGB[id][i * 3 + 2]; p.rgba[i * 4 + 3] = 255;
  }
  return p;
}
static inline void rgbOf(uint32_t c, double& r, double& g, double& b) { r = (c >> 16) & 255; g = (c >> 8) & 255; b = c & 255; }

Palette paletteFromStops(const std::vector<uint32_t>& stops) {
  Palette p{};
  if (stops.size() < 2) { const uint32_t c = stops.empty() ? 0 : stops[0]; return paletteFromStops({ c, c }); }
  const int n = (int)stops.size() - 1;
  for (int i = 0; i < 256; i++) {
    const double t = i / 255.0;
    const int seg = std::min((int)std::floor(t * n), n - 1);
    const double f = t * n - seg;
    double r0, g0, b0, r1, g1, b1; rgbOf(stops[seg], r0, g0, b0); rgbOf(stops[seg + 1], r1, g1, b1);
    p.rgba[i * 4 + 0] = (uint8_t)jsRound(r0 + (r1 - r0) * f);
    p.rgba[i * 4 + 1] = (uint8_t)jsRound(g0 + (g1 - g0) * f);
    p.rgba[i * 4 + 2] = (uint8_t)jsRound(b0 + (b1 - b0) * f);
    p.rgba[i * 4 + 3] = 255;
  }
  return p;
}
Palette paletteFromPositions(const std::vector<std::pair<int, uint32_t>>& stops) {
  Palette p{};
  if (stops.size() < 2) return paletteFromStops({ stops.empty() ? 0u : stops[0].second });
  size_t seg = 0;
  for (int i = 0; i < 256; i++) {
    while (seg < stops.size() - 2 && i > stops[seg + 1].first) seg++;
    const int p0 = stops[seg].first, p1 = stops[seg + 1].first;
    const double f = p1 > p0 ? std::max(0.0, std::min(1.0, (double)(i - p0) / (p1 - p0))) : 0;
    double r0, g0, b0, r1, g1, b1; rgbOf(stops[seg].second, r0, g0, b0); rgbOf(stops[seg + 1].second, r1, g1, b1);
    p.rgba[i * 4 + 0] = (uint8_t)jsRound(r0 + (r1 - r0) * f);
    p.rgba[i * 4 + 1] = (uint8_t)jsRound(g0 + (g1 - g0) * f);
    p.rgba[i * 4 + 2] = (uint8_t)jsRound(b0 + (b1 - b0) * f);
    p.rgba[i * 4 + 3] = 255;
  }
  return p;
}
void spectrumGradient(const Palette& p, uint8_t out[9][3]) {
  for (int gi = 0; gi <= 8; gi++) {
    const int idx = (int)std::max(0.0, std::min(255.0, jsRound(90 + (gi / 8.0) * 145)));
    for (int c = 0; c < 3; c++) out[8 - gi][c] = p.rgba[idx * 4 + c];   // reversed: hot colour at the top
  }
}

}  // namespace wf
