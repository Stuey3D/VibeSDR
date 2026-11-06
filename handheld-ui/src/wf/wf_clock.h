// wf_clock.h — THE WATERFALL'S FIXED OUTPUT CLOCK ("over-interpolate and discard", V12 2026-10-29) + the feed-following
// row target it is driven by + the interaction boost and its taper. Ports of src/assets/wfClock.ts and
// src/assets/wfRowTarget.ts, line for line (same constants). No GL; tested against the TS by test/run_golden.sh.
// Rows come off a steady clock at exactly the mode's rate; each is the newest REAL frame presented by its tick
// (sample-and-hold, never a blend); a late frame is one more repeat, never a stall or a burst.
#pragma once
#include <deque>
#include <vector>

namespace wf {

enum class Scroll { Min2, Low, Sharp /* NATIVE */, Eco, Default, Smooth };

// ── wfRowTarget.ts ──
extern const int ROW_RATES[6];
constexpr double ROW_HYST = 0.15;
constexpr double ROW_ACTUAL_TOL = 0.025;
constexpr int ROW_WARMUP_FRAMES = 20;
constexpr double ROW_LATCH_FAST = ROW_HYST;
constexpr double ROW_LATCH_SLOW = 0.04;
constexpr int ROW_LATCH_FRAMES = 20;

Scroll scrollForCap(int cap);
double snappedTarget(Scroll s, double fps);
double nominalRowRate(Scroll s, double fps);
double rowTargetFor(Scroll s, double fps);
int rowsForArrival(double& carry, double durMs, double target);
struct RowTargetState { double latched = 0; int away = 0; double target = 0; int frames = 0; double clockRate = 0; };
double stepRowTarget(RowTargetState& st, Scroll s, double feedFps);
double rowTargetUnderCap(double target, double feedFps, double capHz);

// ── wfClock.ts ──
struct ClockCfg { double delayFrac = 0.2, delayMaxMs = 40; };
extern ClockCfg CLOCK_CFG;
struct ClockFrame { long long id; double p; double h; };
struct RowClockState {
  double base = 0, ivl = 100, lastT = 0, ps = 0;
  std::deque<ClockFrame> q;
  long long cur = -1; double curP = 0, curH = 0, hold = 0;
  double rate = 0, t0 = 0; long long k = 0;
};
double clockDelayMs(double ivl);
double holdLimitMs(double ivl);
double clockArrive(RowClockState& c, long long id, double t0, bool instant = false, double holdMs = 0);
void clockSetRate(RowClockState& c, double rate, double now);
double clockNextAt(const RowClockState& c);
int clockRun(RowClockState& c, double now, std::vector<long long>& out, int maxRows = 6);
void clockReset(RowClockState& c);
double clockRowRate(RowTargetState& st, Scroll s, double feedFps);
double clockRateUnderCap(double rate, double capHz);
bool clockIdle(const RowClockState& c, double now);

constexpr double BOOST_MAX_RATE = 120;
constexpr double BOOST_FEED_X = 12;
constexpr double BOOST_TAIL_MS = 750;
constexpr double BOOST_TAPER_MS = 600;
double boostRowRate(double base, double feedFps, double displayHz, bool enabled = true);
struct BoostState { double lastMove = 0; bool on = false; double peak = 0, offAt = 0; };
void boostNote(BoostState& b, double now);
bool boostActive(BoostState& b, double now, bool easing);
std::vector<double> taperRungs(double lo, double hi, double displayHz);
double boostTaper(BoostState& b, double base, double boosted, double now, double displayHz);

}  // namespace wf
