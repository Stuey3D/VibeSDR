// wf_row_target.h — the waterfall row target that follows the feed. A LINE-FOR-LINE port of src/assets/wfRowTarget.ts
// (the app's JS path and the web client use that one); test_spectrum_engine.cpp runs the same table as
// scripts/test_wf_row_target.ts. Change one, change both.
//
// ★★★ Stuart 10-20: Sharp = actual rows (x1); Default = min(20, 3 x feed fps); Smooth = min(60, 6 x feed fps); each
//     snapped DOWN to {10, 12, 15, 20, 30, 60} (rates that divide 60). ★ 2026-10-29: a faster feed is thinned (Eco,
//     Low, Minimum) or Default/Smooth go up to the next rate — every choice but Native divides 60 (nominalRowRate).
//     5 fps → 5/15/30 · 10 → 10/20/60 · 20 → 20/20/60 · UberSDR 3.3 → 3.3/10/20 · OWRX ~9 → 9/20/30. Never above x6.
// ★★ Hysteresis on the FEED (a latch): a 9 <-> 10 fps wobble cannot flap Smooth 30/60; a governor step changes it once.
#pragma once
#include <algorithm>
#include <cmath>

namespace vibewf {

/** `cap` 20 = Default, 60 = Smooth, 10 = Eco, 5 = Low, 2 = Minimum (V12), 0 = Sharp/Native (EngineConfig::targetRows carries the cap). */
inline int rowMultFor(int cap) { return cap >= 60 || cap == 10 || cap == 5 || cap == 2 ? 6 : 3; }   // ★ Eco 10 / Low 5 / Minimum 2 = mult 6, as wfRowTarget.ts


/** The largest of {10,12,15,20,30,60} ≤ min(cap, mult × fps) (3 % slack: 3.33 × 6 → 20), or 0. */
inline double snappedRowTarget(int cap, double fps) {
    if (cap <= 0 || !(fps > 0)) return 0;
    static const double kRates[] = {10, 12, 15, 20, 30, 60};
    // ★ Default (carried as 20) runs up to 30 — Stuart, 2026-10-29, a 10 fps UberSDR: "we should be smoothing to 30".
    const double top = cap == 20 ? 30.0 : (double)cap;
    const double raw = std::min(top, rowMultFor(cap) * fps) * 1.03;
    if (cap == 5) return raw >= 5 ? 5 : 0;   // ★ V12 LOW (was MINIMUM): its one rate, 5 (only a sub-5 feed is filled in)
    if (cap == 2) return raw >= 2 ? 2 : 0;   // ★ V12 MINIMUM (#57): its one rate, 2 — a faster feed thinned, a slower filled in
    if (cap == 20) {   // ★★ DEFAULT never runs at 20 (Stuart 2026-10-29): 10 / 15 / 30, 3 × the feed rounded UP, ≤ 6 × the feed
        if (3 * fps * 1.03 < 10) return 0;   // the idle floor: a 2 fps feed stays actual rows
        const double want = std::min(30.0, 3 * fps) / 1.03;
        double best = 0;
        for (double r : {30.0, 15.0, 10.0}) if (r >= want && r <= 6 * fps * 1.03) best = r;
        return best > 0 ? best : (raw >= 10 ? 10 : 0);
    }
    double best = 0;
    for (double r : kRates) if (r <= raw && r <= top) best = r;
    return best;
}

/** ★★★ The rate a choice RUNS at (wfRowTarget.ts nominalRowRate, 2026-10-29 — Stuart on a 23 fps Kiwi: "interpolate and
 *  discard"): always a divisor of 60. Eco/Low/Minimum thin a faster feed to 10/5/2; Default/Smooth never thin below the feed —
 *  they go UP to the next rate (23 → 30); above 60, 60. 0 for Native or no feed. */
inline double nominalRowRate(int cap, double fps) {
    if (cap <= 0 || !(fps > 0)) return 0;
    double t = snappedRowTarget(cap, fps);
    if ((cap == 20 || cap >= 60) && t > 0 && t < fps / 1.025) {   // (0 = too slow to fill: actual rows)
        static const double kRates[] = {10, 12, 15, 20, 30, 60};
        t = 60;
        for (int i = 5; i >= 0; i--) if (kRates[i] >= fps / 1.025) t = kRates[i];
    }
    return t;
}

/** The rule on a settled feed rate: rows/s, or 0 = actual rows (a rate within 2.5 % of the feed is the feed). Below the
 *  feed the rows are THINNED (the engine discards evenly). */
inline double rowTargetFor(int cap, double fps) {
    const double s = nominalRowRate(cap, fps);
    return s > 0 && std::abs(s - fps) > fps * 0.025 ? s : 0;   // ROW_ACTUAL_TOL 2.5 %
}

/** Rows one arrival becomes at `target` rows/s over `durMs`, with a fractional carry: 0 = discarded, 1 = the frame,
 *  more = the frame and repeats; at most 9. target 0 = 1 (wfRowTarget.ts rowsForArrival). */
inline int rowsForArrival(double& carry, double durMs, double target) {
    if (!(target > 0) || !(durMs > 0)) return 1;
    carry += durMs * target / 1000.0;
    const int n = (int)std::floor(carry);
    carry -= n;
    return std::min(n, 9);
}

/** One waterfall's latch: the target is the rule on a LATCHED feed rate, which moves at once past ±15 % or after 20
 *  frames in a row more than 4 % away — hysteresis on the feed, not the output (see wfRowTarget.ts for why). */
struct RowTargetState { double latched = 0; int away = 0; double target = 0; int frames = 0;
                        double clockRate = 0; };   // ★ V12 the fixed clock's rate (wf_row_clock.h clockRowRate)

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
    return best > 0 && std::abs(best - feedFps) > feedFps * 0.025 ? best : 0;   // ★ thinned below the feed too (2026-10-29)
}

}  // namespace vibewf
