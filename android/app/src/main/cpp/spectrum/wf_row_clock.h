// wf_row_clock.h — THE WATERFALL'S FIXED OUTPUT CLOCK: over-interpolate and discard. A LINE-FOR-LINE port of
// src/assets/wfClock.ts (the app's JS path and the web client use that one); test_spectrum_engine.cpp proves the engine
// with it, scripts/test_wf_fixed_clock.ts the TS. Change one, change both.
//
// ★★★ Stuart (2026-10-29): "if we are only doing a straight 2x 3x interpolation an incoming rate that drifts about will
//     make it feel juddery which is why I said to over interpolate and discard to avoid this issue" — and, in the Eco
//     note, "the interpolation always over interpolates and discards leaving headroom to account for link rate judder".
//  • The OUTPUT rate is fixed (Minimum 2, Low 5, Eco 10, Default 30, Smooth 60): tick k at t0 + k·1000/rate, never an arrival.
//  • Each tick's row is the newest REAL frame presented by then — a repeat, never a blend (7bce170d); none new = the last
//    again. A late frame costs one more repeat; two frames in one tick drop the older (a faster feed thinned evenly).
//  • Frames are PRESENTED on a de-jittered schedule (≈0.2 × the feed interval, ≤ 40 ms, after the mean arrival), so a
//    frame a little late is still on time and the repeats fall in the feed's own pattern (3,3,3 at 10 fps → 30).
//  • A frame held past 2.5 intervals + 100 ms (a dead link) stops the clock — no one row painted down the screen.
//  NATIVE (cap 0) is NOT on this clock: one real row per frame, as it comes ("pure unmodified lines").
#pragma once
#include "wf_row_target.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace vibewf {

struct ClockCfg { double delayFrac = 0.2, delayMaxMs = 40; };
inline ClockCfg& clockCfg() { static ClockCfg c; return c; }   // mutable only for the test's sweep, as the TS
inline double clockDelayMs(double ivl) { return std::min(clockCfg().delayMaxMs, clockCfg().delayFrac * ivl); }
inline double holdLimitMs(double ivl) { return std::max(300.0, 2.5 * ivl + 100); }
/** ★★ V12 2026-10-29 (v12-wf-no-jb) — src/utils/wfPace.ts traceLagMs: how long after a frame's presentation time its row
 *  is on screen (the depth the UI's clockPace holds: PLAYOUT_MS 40, at least one row), so the spectrum trace, retargeted
 *  on the same schedule, lands with the row it goes with. `rate` rows/s; 0 = off the clock (no wait). */
inline double traceLagMs(double rate) { return rate > 0 ? std::max(1.0, rate * 40.0 / 1000.0) * 1000.0 / rate : 0; }

struct ClockFrame { long id; double p; double h = 0; };   // h: the A/V hold it entered with (wf_av_sync.h)
struct RowClock {
    double base = 0, ivl = 100, lastT = 0, ps = 0;   // the feed's schedule
    std::deque<ClockFrame> q;                        // waiting for their presentation time, oldest first
    long cur = -1; double curP = 0;                  // the frame held
    double curH = 0, hold = 0;                       // ★ the A/V hold it entered with; the last arrival's
    double rate = 0, t0 = 0; long long k = 0;        // the output clock
};

/** A real frame arrived at `t`; `instant` (a gesture) presents it at once. Returns its presentation time.
 *  ★★★ V12 2026-10-30 `holdMs` — THE WATERFALL WAITS FOR THE SOUND (wf_av_sync.h; src/assets/wfClock.ts clockArrive, line
 *      for line). The frame enters the schedule `holdMs` after it arrived; the schedule moves with the hold, the feed's
 *      interval is measured on the REAL arrivals. Stuart (2026-10-30): "to prevent scrolling judder whilst the jitter
 *      buffer enables we can simply duplicate the previous lines if needed and then the waterfall still reacts to the
 *      audio in time but still stays smooth" — a hold that GROWS is carried by the ticks repeating the row on screen (the
 *      dead-link limit lengthened by the growth); one that SHRINKS discards the frames now due after a newer one. 0 is
 *      the clock exactly as it was. */
inline double clockArrive(RowClock& c, long id, double t0, bool instant = false, double holdMs = 0) {
    const double dH = holdMs - c.hold;
    c.hold = holdMs;
    if (dH != 0 && c.base > 0) { c.base += dH; c.ps += dH; }   // the schedule moves with the hold
    const double t = t0 + holdMs;
    const double gap = c.lastT > 0 ? t0 - c.lastT : 0;         // the feed's interval: real arrivals
    c.lastT = t0;
    double p;
    if (instant || c.base <= 0 || !(gap > 0) || gap > 4 * c.ivl || gap > 2000) {
        if (gap > 0 && gap <= 2000 && c.base > 0 && !instant) c.ivl = c.ivl * 0.5 + std::min(gap, 4 * c.ivl) * 0.5;
        c.base = t;
        p = instant ? t : t + clockDelayMs(c.ivl);
        c.ps = p;
    } else {
        c.ivl = c.ivl * 0.9 + std::max(0.5 * c.ivl, std::min(2 * c.ivl, gap)) * 0.1;
        const double pred = c.base + c.ivl;
        const double err = t - pred;
        c.base = pred + 0.15 * std::max(-0.5 * c.ivl, std::min(0.5 * c.ivl, err));
        if (c.base > t) c.base = std::max(t, c.base - 0.5 * c.ivl);
        const double d = clockDelayMs(c.ivl);
        const double want = c.base + d, dead = std::min(0.15 * c.ivl, 10.0);
        c.ps += c.ivl;
        const double e = want - c.ps;
        if (e > dead) c.ps += e - dead; else if (e < -dead) c.ps += e + dead;
        p = std::min(std::max(t, c.ps), t + d + 0.25 * c.ivl);
    }
    // ★ The hold shrank: frames still waiting later than this one are older than it — discarded, not shown after it.
    if (dH < 0) while (!c.q.empty() && c.q.back().p > p) c.q.pop_back();
    const double lastP = !c.q.empty() ? c.q.back().p : (c.cur >= 0 ? c.curP : -INFINITY);
    if (p < lastP) p = lastP;
    c.q.push_back({id, p, holdMs});
    const size_t qMax = holdMs > 0 ? 4 + (size_t)std::ceil(holdMs / std::max(10.0, c.ivl)) : 4;
    while (c.q.size() > qMax) c.q.pop_front();
    return p;
}

inline void clockSetRate(RowClock& c, double rate, double now) {
    if (rate == c.rate) return;
    if (c.rate > 0 && rate > 0) {
        const double last = c.t0 + (double)(c.k - 1) * 1000.0 / c.rate;
        c.t0 = std::max(last + 1000.0 / rate, now); c.k = 0;
    } else if (rate > 0) { c.t0 = now; c.k = 0; }
    c.rate = rate;
}

inline double clockNextAt(const RowClock& c) { return c.rate > 0 ? c.t0 + (double)c.k * 1000.0 / c.rate : -1; }

/** Every tick due by `now`: the frame it shows, appended to `out`. At most `maxRows` (older ticks skipped). */
inline int clockRun(RowClock& c, double now, std::vector<long>& out, int maxRows = 6) {
    if (!(c.rate > 0)) return 0;
    const double per = 1000.0 / c.rate;
    long long due = (long long)std::floor((now - c.t0) / per + 1e-9) - c.k + 1;
    if (due <= 0) return 0;
    if (due > maxRows) { c.k += due - maxRows; due = maxRows; }
    int made = 0;
    for (long long i = 0; i < due; i++) {
        const double tt = c.t0 + (double)c.k * per;
        c.k++;
        while (!c.q.empty() && c.q.front().p <= tt) { c.cur = c.q.front().id; c.curP = c.q.front().p; c.curH = c.q.front().h; c.q.pop_front(); }
        if (c.cur < 0) continue;
        if (tt - c.curP > holdLimitMs(c.ivl) + std::max(0.0, c.hold - c.curH)) continue;   // ★ + the A/V hold's growth (repeats, not a dead link)
        out.push_back(c.cur); made++;
    }
    if (c.k > 1000000) { c.t0 += (double)c.k * per; c.k = 0; }
    return made;
}

inline void clockReset(RowClock& c) { c.q.clear(); c.cur = -1; c.curP = 0; c.curH = 0; c.hold = 0; c.base = 0; c.lastT = 0; c.ps = 0; }
inline bool clockIdle(const RowClock& c, double now) { return c.q.empty() && (c.cur < 0 || now - c.curP > holdLimitMs(c.ivl) + std::max(0.0, c.hold - c.curH)); }

/** The clock's rate from the feed (wfClock.ts clockRowRate): the rule on the latched feed, taking a step the feed is
 *  within 5 % of (Smooth only), and not stepping DOWN while the feed still supports the rate within 15 %. `cap` as EngineConfig. */
inline double clockRowRate(RowTargetState& st, int cap, double feedFps) {
    if (cap <= 0) { st.clockRate = 0; return 0; }
    stepRowTarget(st, cap, feedFps);
    const double fps = st.latched;
    double r = nominalRowRate(cap, fps);
    if (cap >= 60) { const double up = snappedRowTarget(cap, fps * 1.05); if (up > r) r = up; }   // ★ Smooth only (see the TS)
    const double held = st.clockRate;
    if (held > 0 && r < held && nominalRowRate(cap, fps * 1.15) >= held) r = held;
    st.clockRate = r;
    return r;
}

/** Under a FRAME RATE cap below 60: the largest of {2,5,10,12,15,20,30,60} ≤ the rate that divides the cap. */
inline double clockRateUnderCap(double rate, int capHz) {
    if (!(rate > 0) || capHz <= 0 || capHz >= 60) return rate;
    static const int rates[] = {2, 5, 10, 12, 15, 20, 30, 60};
    int best = 0;
    for (int r : rates) if (r <= rate && r <= capHz && capHz % r == 0) best = r;
    return best > 0 ? best : rate;
}


// ★★★ THE INTERACTION BOOST — the twin of src/assets/wfClock.ts boostRowRate (Stuart 2026-10-29: "increase during
//     interaction … we dont try to pull down more data than a server can supply even if it means a 12x frame generation"
//     / "if the app is set to 120 we do 120"). While a tune/zoom is in flight the clock's rate steps up to the display's
//     maximum as set, ≤ 120, ≤ 12 × the feed; every extra row is a frame already received. Nothing is asked of the
//     server. NATIVE (base 0) → 0, untouched. Change one, change both.
constexpr double BOOST_MAX_RATE = 120, BOOST_FEED_X = 12, BOOST_TAIL_MS = 750;
inline double boostRowRate(double base, double feedFps, double displayHz, bool enabled = true) {
    if (!(base > 0)) return 0;
    if (!enabled) return base;   // ★★ V12 2026-10-30 "SPEED UP WHILE TUNING" off (wfClock.ts): never raised
    const double hz = displayHz > 0 ? displayHz : 60;
    const double lim = std::min(std::min(BOOST_MAX_RATE, hz * 1.02), (feedFps > 0 ? feedFps : 0) * BOOST_FEED_X * 1.0001);
    static const double R[] = {120, 60, 30, 20, 15, 12, 10, 5};
    for (double r : R) {
        if (r > lim) continue;
        if (r == 120 && hz < 119) continue;
        return std::max(base, r);
    }
    return base;
}
struct BoostState { double lastMove = 0; bool on = false; double peak = 0, offAt = 0; };
inline void boostNote(BoostState& b, double now) { b.lastMove = now; b.on = true; }
inline bool boostActive(BoostState& b, double now, bool easing) {
    b.on = b.lastMove > 0 && (now - b.lastMove < BOOST_TAIL_MS || (b.on && easing));
    return b.on;
}
// ★★★ THE BOOST TAPERS OUT — the twin of wfClock.ts boostTaper (Stuart 2026-10-31: "the sudden FPS drop from 120→30 is
//     causing what appears to be a slight pause"): down through the rates the panel draws on whole frames, each step
//     1.5-2×, equal rows a rung, over BOOST_TAPER_MS. An input during it is the boost again at once. Change one, change both.
constexpr double BOOST_TAPER_MS = 600;
inline double boostTaper(BoostState& b, double base, double boosted, double now, double displayHz) {
    if (!(base > 0)) { b.peak = 0; b.offAt = 0; return base; }
    if (b.on) { b.peak = std::max(base, boosted); b.offAt = 0; return b.peak; }
    if (!(b.peak > base)) { b.peak = 0; b.offAt = 0; return base; }
    if (!(b.offAt > 0)) b.offAt = now;
    static const double TR[] = {120, 60, 40, 30, 24, 20, 15, 12, 10, 5};
    const double hz = displayHz > 0 ? displayHz : 60, whole = hz >= 119 ? 120 : 60;
    double rungs[10]; int n = 0; double prev = b.peak, inv = 0;
    for (double x : TR)
        if (x < b.peak && x > base && std::fmod(whole, x) == 0 && prev / x >= 1.5 - 1e-9) { rungs[n++] = x; prev = x; inv += 1 / x; }
    double t = now - b.offAt;
    for (int i = 0; i < n; i++) { const double d = BOOST_TAPER_MS * (1 / rungs[i]) / inv; if (t < d) return rungs[i]; t -= d; }
    b.peak = 0; b.offAt = 0;
    return base;
}

}  // namespace vibewf
