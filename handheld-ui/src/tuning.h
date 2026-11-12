// tuning.h — the dial's arithmetic, ported from the app so the handheld steps exactly as the phone does:
// src/services/sdrTypes.ts (STEPS, STEPS_VHF, STEPS_AIRBAND, stepsForFreq) and src/utils/airband.ts (the 8.33 kHz
// sentinel, stepIndex / stepHz / stepFrom: an exact grid computed from the index, never accumulated).
#pragma once
#include "bandplan_gen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hh {

constexpr int STEP_833 = 8333;                       // ★ the 8.33 kHz airband raster's sentinel (exact = 25000 / 3)
constexpr double AIR_COM_LO = 118000000, AIR_COM_HI = 137000000, ON_CHANNEL_TOL_HZ = 2;

inline bool isAirbandCom(double hz) { return hz >= AIR_COM_LO - ON_CHANNEL_TOL_HZ && hz < AIR_COM_HI; }

inline const std::vector<int>& stepsForFreq(double hz) {
    static const std::vector<int> hf = {10, 100, 500, 1000, 5000, 9000, 10000};
    static const std::vector<int> vhf = {100, 500, 1000, 5000, 6250, 12500, 25000, 50000, 100000};
    static const std::vector<int> air = {100, 500, 1000, 5000, 6250, STEP_833, 12500, 25000, 50000, 100000};
    if (isAirbandCom(hz)) return air;
    return hz >= 30000000 ? vhf : hf;
}

inline double stepExactHz(int step) { return step == STEP_833 ? 25000.0 / 3.0 : step; }

inline long long stepIndex(double hz, int step, int how /* 0 round, -1 floor, 1 ceil */) {
    const double s = stepExactHz(step);
    const long long nearI = (long long)std::llround(hz / s);
    if (std::fabs(nearI * s - hz) <= ON_CHANNEL_TOL_HZ) return nearI;
    const double x = hz / s;
    return how < 0 ? (long long)std::floor(x) : how > 0 ? (long long)std::ceil(x) : nearI;
}
inline double stepHz(long long idx, int step) { return (double)std::llround(idx * stepExactHz(step)); }
/** `n` steps from hz — the tuner keys' rule: the first step up goes to the next grid point above, down to the next below. */
inline double stepFrom(double hz, int step, int n) {
    if (step <= 0 || n == 0) return hz;
    const long long base = n > 0 ? stepIndex(hz, step, -1) : stepIndex(hz, step, 1);
    return stepHz(base + n, step);
}
/** The step nearest `want` on the ladder for `hz` (crossing 30 MHz changes the ladder). */
inline int stepOnLadder(double hz, int want) {
    const auto& l = stepsForFreq(hz);
    int best = l.front();
    for (int s : l) if (std::fabs(std::log((double)s) - std::log((double)want)) < std::fabs(std::log((double)best) - std::log((double)want))) best = s;
    for (int s : l) if (s == want) return want;
    return best;
}
/** web/client/src/main.ts formatStep: "100Hz", "1kHz", "12.5kHz", "6.25kHz", "8.33kHz". */
inline std::string formatStep(int hz) {
    if (hz == STEP_833) return "8.33kHz";
    char b[32];
    if (hz >= 1000) {
        const double k = hz / 1000.0;
        if (std::fmod(k, 1.0) == 0) std::snprintf(b, sizeof b, "%dkHz", (int)k);
        else {
            std::snprintf(b, sizeof b, "%.2f", k);
            std::string s(b);
            while (!s.empty() && s.back() == '0') s.pop_back();
            return s + "kHz";
        }
        return b;
    }
    std::snprintf(b, sizeof b, "%dHz", hz);
    return b;
}

// ── the band plan (src/constants/bandPlan.ts: getBandsAtRegion, bandTuneDefaults, bandJumpDefaults) ─────────────────
struct BandDefaults { std::string mode; int step = 0; };
inline BandDefaults bandTuneDefaults(double hz, int region) {
    std::vector<const BandDef*> bands;
    std::vector<std::string> seen;
    for (int i = 0; i < kBandPlanN; i++) {
        const BandDef& b = kBandPlan[i];
        if (hz < b.lo || hz > b.hi) continue;
        if (b.regions && region && !(b.regions & (1 << region))) continue;
        const std::string key = std::string(b.name) + "|" + std::to_string(b.type);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        bands.push_back(&b);
    }
    std::stable_sort(bands.begin(), bands.end(), [](const BandDef* a, const BandDef* b) { return a->type < b->type; });
    const BandDef* primary = nullptr;
    for (auto* b : bands) if (*b->mode) { primary = b; break; }
    if (!primary && !bands.empty()) primary = bands.front();
    if (!primary) return {};
    return {primary->mode, primary->step};
}
/** A JUMP (typed frequency, bookmark …) — never the tune controls: the landing band's step always; its mode only when
 *  the jump changes band and that mode differs from both the band left and the mode in use. FM broadcast = WFM. */
inline BandDefaults bandJumpDefaults(double fromHz, double toHz, const std::string& curMode, int region) {
    auto modeOf = [&](double f) -> std::string {
        std::string m = bandTuneDefaults(f, region).mode;
        if (m.empty() && f >= 87.5e6 && f < 108e6) m = "wfm";
        return m;
    };
    const std::string toMode = modeOf(toHz), fromMode = fromHz > 0 ? modeOf(fromHz) : "";
    BandDefaults r;
    if (!toMode.empty() && toMode != fromMode && toMode != curMode) r.mode = toMode;
    r.step = bandTuneDefaults(toHz, region).step;
    return r;
}
/** src/services/stations.ts deriveItuRegion (main.ts ituRegion: unknown → 1). */
inline int ituRegion(bool haveLon, double lon) { if (!haveLon) return 1; return lon < -30 ? 2 : lon < 60 ? 1 : 3; }

/** web/client/src/main.ts toSUnit. */
inline std::string toSUnit(double dbfs) {
    char b[16];
    if (dbfs >= -73) { std::snprintf(b, sizeof b, "S9+%d", (int)std::min(60.0, std::round((dbfs + 73) / 6) * 6)); return b; }
    const int ladder[] = {-79, -85, -91, -97, -103, -109, -115};
    for (int i = 0; i < 7; i++) if (dbfs >= ladder[i]) { std::snprintf(b, sizeof b, "S%d", 8 - i); return b; }
    return "S1";
}

}  // namespace hh
