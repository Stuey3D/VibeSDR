// wf_clock.cpp — ports of src/assets/wfRowTarget.ts and src/assets/wfClock.ts. Design notes are in the TS files.
#include "wf_clock.h"
#include "wf_row_place.h"   // jsRound
#include <algorithm>
#include <cmath>
#include <limits>

namespace wf {

const int ROW_RATES[6] = { 10, 12, 15, 20, 30, 60 };
ClockCfg CLOCK_CFG;
static constexpr double SNAP_SLACK = 1.03;
static constexpr int CLOCK_QUEUE = 4;

struct Rule { double cap, mult; };
static Rule ruleOf(Scroll s) {
  switch (s) {
    case Scroll::Min2: return { 2, 6 };
    case Scroll::Low: return { 5, 6 };
    case Scroll::Eco: return { 10, 6 };
    case Scroll::Default: return { 30, 3 };
    case Scroll::Smooth: return { 60, 6 };
    default: return { 0, 0 };
  }
}

Scroll scrollForCap(int cap) {
  return cap >= 60 ? Scroll::Smooth : cap >= 20 ? Scroll::Default : cap >= 10 ? Scroll::Eco : cap >= 5 ? Scroll::Low
       : cap > 0 ? Scroll::Min2 : Scroll::Sharp;
}

// snappedTarget
double snappedTarget(Scroll s, double fps) {
  if (s == Scroll::Sharp || !(fps > 0)) return 0;
  const Rule r = ruleOf(s);
  const double raw = std::min(r.cap, r.mult * fps) * SNAP_SLACK;
  double best = 0;
  if (s == Scroll::Default) {
    if (r.mult * fps * SNAP_SLACK < 10) return 0;
    const double want = std::min(r.cap, r.mult * fps) / SNAP_SLACK;
    for (double x : { 30.0, 15.0, 10.0 }) if (x >= want && x <= 6 * fps * SNAP_SLACK) best = x;
    return best != 0 ? best : (raw >= 10 ? 10 : 0);
  }
  if (s == Scroll::Low) { if (5 <= raw && 5 <= r.cap) best = 5; return best; }
  if (s == Scroll::Min2) { if (2 <= raw && 2 <= r.cap) best = 2; return best; }
  for (int x : ROW_RATES) if (x <= raw && x <= r.cap) best = x;
  return best;
}

double nominalRowRate(Scroll s, double fps) {
  if (s == Scroll::Sharp || !(fps > 0)) return 0;
  double t = snappedTarget(s, fps);
  if ((s == Scroll::Default || s == Scroll::Smooth) && t > 0 && t < fps / (1 + ROW_ACTUAL_TOL)) {
    t = 60;
    for (int i = 5; i >= 0; i--) if (ROW_RATES[i] >= fps / (1 + ROW_ACTUAL_TOL)) t = ROW_RATES[i];
  }
  return t;
}

double rowTargetFor(Scroll s, double fps) {
  const double r = nominalRowRate(s, fps);
  return r > 0 && std::fabs(r - fps) > fps * ROW_ACTUAL_TOL ? r : 0;
}

int rowsForArrival(double& carry, double durMs, double target) {
  if (!(target > 0) || !(durMs > 0)) return 1;
  carry += durMs / (1000 / target);
  const double n = std::floor(carry);
  carry -= n;
  return (int)std::min(n, 9.0);
}

double stepRowTarget(RowTargetState& st, Scroll s, double feedFps) {
  if (!(feedFps > 0)) return st.target;
  if (st.latched <= 0 || ++st.frames <= ROW_WARMUP_FRAMES) { st.latched = feedFps; st.away = 0; }
  else {
    const double dev = std::fabs(feedFps - st.latched) / st.latched;
    if (dev > ROW_LATCH_FAST) { st.latched = feedFps; st.away = 0; }
    else if (dev > ROW_LATCH_SLOW) { if (++st.away >= ROW_LATCH_FRAMES) { st.latched = feedFps; st.away = 0; } }
    else st.away = 0;
  }
  st.target = rowTargetFor(s, st.latched);
  return st.target;
}

double rowTargetUnderCap(double target, double feedFps, double capHz) {
  if (!(target > 0) || !(capHz > 0) || capHz >= 60) return target;
  double best = 0;
  for (int r : ROW_RATES) if (r <= target && r <= capHz && std::fmod(capHz, r) == 0) best = r;
  return best > 0 && std::fabs(best - feedFps) > feedFps * ROW_ACTUAL_TOL ? best : 0;
}

double clockDelayMs(double ivl) { return std::min(CLOCK_CFG.delayMaxMs, CLOCK_CFG.delayFrac * ivl); }
double holdLimitMs(double ivl) { return std::max(300.0, 2.5 * ivl + 100); }

// clockArrive
double clockArrive(RowClockState& c, long long id, double t0, bool instant, double holdMs) {
  const double dH = holdMs - c.hold;
  c.hold = holdMs;
  if (dH != 0 && c.base > 0) { c.base += dH; c.ps += dH; }
  const double t = t0 + holdMs;
  const double gap = c.lastT > 0 ? t0 - c.lastT : 0;
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
  if (dH < 0) while (!c.q.empty() && c.q.back().p > p) c.q.pop_back();
  const double lastP = !c.q.empty() ? c.q.back().p : (c.cur >= 0 ? c.curP : -std::numeric_limits<double>::infinity());
  if (p < lastP) p = lastP;
  c.q.push_back({ id, p, holdMs });
  const size_t qMax = holdMs > 0 ? CLOCK_QUEUE + (size_t)std::ceil(holdMs / std::max(10.0, c.ivl)) : CLOCK_QUEUE;
  while (c.q.size() > qMax) c.q.pop_front();
  return p;
}

void clockSetRate(RowClockState& c, double rate, double now) {
  if (rate == c.rate) return;
  if (c.rate > 0 && rate > 0) {
    const double last = c.t0 + (c.k - 1) * 1000 / c.rate;
    c.t0 = std::max(last + 1000 / rate, now); c.k = 0;
  } else if (rate > 0) { c.t0 = now; c.k = 0; }
  c.rate = rate;
}

double clockNextAt(const RowClockState& c) { return c.rate > 0 ? c.t0 + c.k * 1000 / c.rate : -1; }

int clockRun(RowClockState& c, double now, std::vector<long long>& out, int maxRows) {
  if (!(c.rate > 0)) return 0;
  const double per = 1000 / c.rate;
  long long due = (long long)std::floor((now - c.t0) / per + 1e-9) - c.k + 1;
  if (due <= 0) return 0;
  if (due > maxRows) { c.k += due - maxRows; due = maxRows; }
  int made = 0;
  for (long long i = 0; i < due; i++) {
    const double tt = c.t0 + c.k * per;
    c.k++;
    while (!c.q.empty() && c.q.front().p <= tt) { const ClockFrame f = c.q.front(); c.q.pop_front(); c.cur = f.id; c.curP = f.p; c.curH = f.h; }
    if (c.cur < 0) continue;
    if (tt - c.curP > holdLimitMs(c.ivl) + std::max(0.0, c.hold - c.curH)) continue;
    out.push_back(c.cur); made++;
  }
  if (c.k > 1000000) { c.t0 += c.k * per; c.k = 0; }
  return made;
}

void clockReset(RowClockState& c) {
  c.q.clear(); c.cur = -1; c.curP = 0; c.curH = 0; c.hold = 0; c.base = 0; c.lastT = 0; c.ps = 0;
}

double clockRowRate(RowTargetState& st, Scroll s, double feedFps) {
  if (s == Scroll::Sharp) return (st.clockRate = 0);
  stepRowTarget(st, s, feedFps);
  const double fps = st.latched;
  double r = nominalRowRate(s, fps);
  if (s == Scroll::Smooth) { const double up = snappedTarget(s, fps * 1.05); if (up > r) r = up; }
  const double held = st.clockRate;
  if (held > 0 && r < held && nominalRowRate(s, fps * (1 + ROW_HYST)) >= held) r = held;
  st.clockRate = r;
  return r;
}

double clockRateUnderCap(double rate, double capHz) {
  if (!(rate > 0) || !(capHz > 0) || capHz >= 60) return rate;
  double best = 0;
  const int rs[8] = { 2, 5, 10, 12, 15, 20, 30, 60 };
  for (int r : rs) if (r <= rate && r <= capHz && std::fmod(capHz, r) == 0) best = r;
  return best != 0 ? best : rate;
}

bool clockIdle(const RowClockState& c, double now) {
  return c.q.empty() && (c.cur < 0 || now - c.curP > holdLimitMs(c.ivl) + std::max(0.0, c.hold - c.curH));
}

double boostRowRate(double base, double feedFps, double displayHz, bool enabled) {
  if (!(base > 0)) return 0;
  if (!enabled) return base;
  const double hz = displayHz > 0 ? displayHz : 60;
  const double lim = std::min({ BOOST_MAX_RATE, hz * 1.02, (feedFps > 0 ? feedFps : 0) * BOOST_FEED_X * 1.0001 });
  for (double r : { 120.0, 60.0, 30.0, 20.0, 15.0, 12.0, 10.0, 5.0 }) {
    if (r > lim) continue;
    if (r == 120 && hz < 119) continue;
    return std::max(base, r);
  }
  return base;
}
void boostNote(BoostState& b, double now) { b.lastMove = now; b.on = true; }
bool boostActive(BoostState& b, double now, bool easing) {
  b.on = b.lastMove > 0 && (now - b.lastMove < BOOST_TAIL_MS || (b.on && easing));
  return b.on;
}
std::vector<double> taperRungs(double lo, double hi, double displayHz) {
  const double hz = displayHz > 0 ? displayHz : 60, whole = hz >= 119 ? 120 : 60;
  std::vector<double> out; double prev = hi;
  for (double x : { 120.0, 60.0, 40.0, 30.0, 24.0, 20.0, 15.0, 12.0, 10.0, 5.0 })
    if (x < hi && x > lo && std::fmod(whole, x) == 0 && prev / x >= 1.5 - 1e-9) { out.push_back(x); prev = x; }
  return out;
}
double boostTaper(BoostState& b, double base, double boosted, double now, double displayHz) {
  if (!(base > 0)) { b.peak = 0; b.offAt = 0; return base; }
  if (b.on) { b.peak = std::max(base, boosted); b.offAt = 0; return b.peak; }
  const double peak = b.peak;
  if (!(peak > base)) { b.peak = 0; b.offAt = 0; return base; }
  if (!(b.offAt > 0)) b.offAt = now;
  const std::vector<double> rungs = taperRungs(base, peak, displayHz);
  double inv = 0; for (double r : rungs) inv += 1 / r;
  double t = now - b.offAt;
  for (double r : rungs) { const double d = BOOST_TAPER_MS * (1 / r) / inv; if (t < d) return r; t -= d; }
  b.peak = 0; b.offAt = 0;
  return base;
}

}  // namespace wf
