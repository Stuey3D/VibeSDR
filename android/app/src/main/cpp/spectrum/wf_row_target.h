// wf_row_target.h — the waterfall row target that follows the feed. A LINE-FOR-LINE port of src/assets/wfRowTarget.ts
// (the app's JS path and the web client use that one); test_spectrum_engine.cpp runs the same table as
// scripts/test_wf_row_target.ts. Change one, change both.
//
// ★★★ Stuart 10-20: Sharp = actual rows (x1); Default = min(20, 3 x feed fps); Smooth = min(60, 6 x feed fps); each
//     snapped DOWN to {10, 12, 15, 20, 30, 60} (rates that divide 60) and never below the feed (then actual rows).
//     5 fps → 5/15/30 · 10 → 10/20/60 · 20 → 20/20/60 · UberSDR 3.3 → 3.3/10/20 · OWRX ~9 → 9/20/30. Never above x6.
// ★★ Hysteresis on the FEED (a latch): a 9 <-> 10 fps wobble cannot flap Smooth 30/60; a governor step changes it once.
#pragma once
#include <algorithm>
#include <cmath>

namespace vibewf {

/** `cap` 20 = Default, 60 = Smooth, 10 = Eco, 5 = Minimum (V12), 0 = Sharp/Native (EngineConfig::targetRows carries the cap). */
inline int rowMultFor(int cap) { return cap >= 60 || cap == 10 || cap == 5 ? 6 : 3; }   // ★ Eco 10 / Minimum 5 = mult 6, as wfRowTarget.ts


/** The largest of {10,12,15,20,30,60} ≤ min(cap, mult × fps) (3 % slack: 3.33 × 6 → 20), or 0. */
inline double snappedRowTarget(int cap, double fps) {
    if (cap <= 0 || !(fps > 0)) return 0;
    static const double kRates[] = {10, 12, 15, 20, 30, 60};
    const double raw = std::min((double)cap, rowMultFor(cap) * fps) * 1.03;
    if (cap == 5) return raw >= 5 ? 5 : 0;   // ★ V12 MINIMUM: its one rate, 5 (only a sub-5 feed is filled in)
    double best = 0;
    for (double r : kRates) if (r <= raw && r <= cap) best = r;
    return best;
}

/** The rule on a settled feed rate: rows/s, or 0 = actual rows (a target within 5 % of the feed is the feed). */
inline double rowTargetFor(int cap, double fps) {
    const double s = snappedRowTarget(cap, fps);
    return s > fps * 1.05 ? s : 0;
}

/** One waterfall's latch: the target is the rule on a LATCHED feed rate, which moves at once past ±15 % or after 20
 *  frames in a row more than 4 % away — hysteresis on the feed, not the output (see wfRowTarget.ts for why). */
struct RowTargetState { double latched = 0; int away = 0; double target = 0; int frames = 0; };

/** Advance by one data frame. `feedFps` = the smoothed feed estimate. Returns rows/s, or 0 = actual rows. */
inline double stepRowTarget(RowTargetState& st, int cap, double feedFps) {
    if (!(feedFps > 0)) return st.target;
    if (st.latched <= 0 || ++st.frames <= 20) { st.latched = feedFps; st.away = 0; }   // warm-up: the estimate starts from a guess
    else {
        const double dev = std::abs(feedFps - st.latched) / st.latched;
        if (dev > 0.15) { st.latched = feedFps; st.away = 0; }
        else if (dev > 0.04) { if (++st.away >= 20) { st.latched = feedFps; st.away = 0; } }
        else st.away = 0;
    }
    st.target = rowTargetFor(cap, st.latched);
    return st.target;
}

/** Under a display cap below 60: the largest ROW_RATE ≤ the target and the cap that DIVIDES the cap (60 → 30, 20 → 15);
 *  0 = actual rows when that is no more than the feed (wfRowTarget.ts rowTargetUnderCap — Stuart 2026-10-28). */
inline double rowTargetUnderCap(double target, double feedFps, int capHz) {
    if (!(target > 0) || capHz <= 0 || capHz >= 60) return target;
    static const int rates[] = {10, 12, 15, 20, 30, 60};
    int best = 0;
    for (int r : rates) if (r <= target && r <= capHz && capHz % r == 0) best = r;
    return best > feedFps * 1.05 ? best : 0;
}

}  // namespace vibewf
