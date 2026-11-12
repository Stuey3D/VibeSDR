// wide_vfo.h — THE DRAWN WIDTH OF A WHOLE-CHANNEL MODE on the R36S: a port of src/utils/wideVfo.ts (the app's and the web
// client's one table). Read that file's header for the why; in short (Stuart, 2026-11-12, a B210 on a 7 MHz span: "we need
// an extra wide VFO for things like DAB"), DAB's passband is drawn as the ensemble — 1.536 MHz about the block centre —
// not the audio filter's ~200 kHz.
//
// ★★★ DRAWN ONLY, NEVER SENT: nothing here goes to the server; the DAB pipeline and the listener's filter are untouched.
// ★★ THE SAME ANSWERS AS THE TS: handheld-ui/test/run_wide_vfo_test.sh runs gen_wide_vfo_golden.ts (the TS functions over
//    a table of cases) and replays them through these (wide_vfo_test.cpp). Change one, change both.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>

namespace hh {

enum WideVfoMode { WIDE_NONE = -1, WIDE_DAB = 0, WIDE_ADSB = 1, WIDE_AIS = 2 };

struct WideVfoEntry { const char* id; double half; const char* name; };
/** src/utils/wideVfo.ts WIDE_VFO — in WideVfoMode order. */
inline const WideVfoEntry kWideVfo[] = {
    {"dab", 768000, "DAB"},
    {"adsb", 1000000, "ADS-B"},
    {"ais", 37500, "AIS"},
};
/** WIDE_VFO_FILL_MAX: past this fraction of the view the passband is its edges only. */
constexpr double kWideVfoFillMax = 0.9;

inline WideVfoMode wideVfoMode(const std::string& id) {
    for (int i = 0; i < 3; i++) if (id == kWideVfo[i].id) return (WideVfoMode)i;
    return WIDE_NONE;
}

/** wideVfoPassband: [lo, hi] as offsets from the dial, centred on the channel's centre (the dial when <= 0). */
inline void wideVfoPassband(WideVfoMode m, double dialHz, double centreHz, double& lo, double& hi) {
    const double h = kWideVfo[m].half;
    const double c = centreHz > 0 ? centreHz : dialHz;
    lo = c - h - dialHz;
    hi = c + h - dialHz;
}

/** wideVfoLabel: "DAB 7D · 194.064M". */
inline std::string wideVfoLabel(WideVfoMode m, double centreHz, const std::string& block = "") {
    char f[32];
    std::snprintf(f, sizeof f, "%.3f", centreHz / 1e6);
    return std::string(kWideVfo[m].name) + (block.empty() ? "" : " " + block) + " \xC2\xB7 " + f + "M";
}

/** wideVfoFill: draw the passband's fill — false when its visible part covers more than kWideVfoFillMax of the view. */
inline bool wideVfoFill(double loX, double hiX, double width) {
    if (!(width > 0)) return false;
    const double vis = std::min(width, hiX) - std::max(0.0, loX);
    return vis < kWideVfoFillMax * width;
}

}  // namespace hh
