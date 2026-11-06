// wf_row_place.cpp — port of src/utils/wfRowPlace.ts. Comments name the TS function each block comes from; the design
// notes live in the TS file and are not repeated here.
#include "wf_row_place.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace wf {

// rowViewOffset
int rowViewOffset(double rowCenterHz, double rowHzPerBin, double viewCenterHz, double viewBwHz, int n) {
  if (!(rowHzPerBin > 0 && rowCenterHz > 0 && viewCenterHz > 0 && viewBwHz > 0 && n > 0)) return 0;
  const double viewHzPerBin = viewBwHz / n;
  if (std::fabs(rowHzPerBin - viewHzPerBin) > viewHzPerBin * 1e-6) return 0;
  const double off = jsRound((rowCenterHz - viewCenterHz) / rowHzPerBin);
  return std::fabs(off) >= n ? 0 : (int)off;
}

// writeRow
void writeRow(uint8_t* ring, int slotOff, const uint8_t* row, int n, int off) {
  if (off == 0) { std::memcpy(ring + slotOff, row, n); return; }
  std::memset(ring + slotOff, 0, n);
  if (off > 0) std::memcpy(ring + slotOff + off, row, n - off);
  else         std::memcpy(ring + slotOff, row - off, n + off);
}

bool spanChanged(double span0, double span1) {
  return span0 > 0 && span1 > 0 && std::fabs(span1 - span0) > span0 * 1e-6;
}

double placedStartHz(double rowCenterHz, double rowHzPerBin, int off, int n) {
  if (!(rowCenterHz > 0 && rowHzPerBin > 0 && n > 0)) return NAN;
  return rowCenterHz - (off + n / 2.0) * rowHzPerBin;
}

int encodeMotion(double h) {
  return (int)std::max(0.0, std::min(65535.0, jsRound(h * MOTION_SCALE) + MOTION_ZERO));
}
double decodeMotion(int hi, int lo) { return ((hi * 256 + lo) - MOTION_ZERO) / MOTION_SCALE; }
int encodeZoom(double g) {
  return (int)std::max(0.0, std::min(65535.0, jsRound(std::log2(g) * ZOOM_SCALE) + MOTION_ZERO));
}
double decodeZoom(int hi, int lo) { return std::pow(2.0, ((hi * 256 + lo) - MOTION_ZERO) / ZOOM_SCALE); }

MotionState newMotionState(int rows, int n) {
  MotionState ms;
  ms.rows = rows; ms.n = n;
  ms.startHz.assign(rows, NAN); ms.hzPerCol.assign(rows, 0);
  ms.h.assign(rows, 0); ms.g.assign(rows, 1);
  ms.tex.resize(4 * (size_t)rows);
  for (int r = 0; r < rows; r++) {
    ms.tex[4 * r + 0] = MOTION_ZERO >> 8; ms.tex[4 * r + 1] = MOTION_ZERO & 255;
    ms.tex[4 * r + 2] = MOTION_ZERO >> 8; ms.tex[4 * r + 3] = MOTION_ZERO & 255;
  }
  ms.lo = 1; ms.hi = 0;
  return ms;
}

// setMotion (the texel layout differs from the app's image planes; the codes are the same)
void setMotion(MotionState& ms, int slot, double g, double h, int /*n*/) {
  ms.h[slot] = (float)h; ms.g[slot] = (float)g;
  const int v = encodeMotion(h), z = encodeZoom(g);
  uint8_t* t = &ms.tex[4 * (size_t)slot];
  const uint8_t nv[4] = { (uint8_t)(v >> 8), (uint8_t)(v & 255), (uint8_t)(z >> 8), (uint8_t)(z & 255) };
  if (std::memcmp(t, nv, 4) != 0) { std::memcpy(t, nv, 4); ms.dirty.push_back(slot); }
}
static inline bool moves(double g, double h) { return g != 1 || h != 0; }

// displayMap
bool displayMap(const MotionState& ms, const std::vector<double>& xfA, const std::vector<double>& xfB, int r, int n,
                double& P, double& Q) {
  const double s = ms.startHz[r], hz = ms.hzPerCol[r];
  if (!std::isfinite(s) || !(hz > 0)) return false;
  P = s + hz * xfB[r] * n; Q = hz * xfA[r];
  return true;
}

// pairMotion
void pairMotion(const MotionState& ms, const std::vector<double>& xfA, const std::vector<double>& xfB, int a, int b, int n,
                double& gOut, double& hOut) {
  gOut = 1; hOut = 0;
  double pa, qa, pb, qb;
  if (!displayMap(ms, xfA, xfB, a, n, pa, qa) || !displayMap(ms, xfA, xfB, b, n, pb, qb)) return;
  const double m = n / 2.0;
  double g = qa / qb;
  const double c = (pa - pb) / qb;
  if (!std::isfinite(g) || !std::isfinite(c)) return;
  if (std::fabs(g - 1) <= 1e-3) {
    if (std::fabs(c) < 1 / MOTION_SCALE || std::fabs(c) > n * MOTION_MAX_FRAC) return;
    gOut = 1; hOut = c; return;
  }
  if (g < 1.0 / 256 || g > 256) return;
  const double h = c + (g - 1) * m;
  if (std::fabs(h) > 511) return;
  gOut = g; hOut = std::fabs(h) < 1 / MOTION_SCALE ? 0 : h;
}

// motionRecompute — only reached after a re-map (the jump branch, off in the app); kept for completeness.
static void motionRecompute(MotionState& ms, const std::vector<double>& xfA, const std::vector<double>& xfB, int n, int rows,
                            long long frames) {
  ms.lo = 1; ms.hi = 0;
  const long long first = std::max(0LL, frames - rows);
  for (long long f = first; f < frames; f++) {
    const int r = (int)(f % rows);
    double g = 1, h = 0;
    if (f + 1 < frames) pairMotion(ms, xfA, xfB, r, (int)((f + 1) % rows), n, g, h);
    setMotion(ms, r, g, h, n);
    if (moves(g, h)) { if (ms.lo > ms.hi) ms.lo = (double)f; ms.hi = (double)f; }
  }
}

// motionAfterRow
void motionAfterRow(MotionState& ms, const std::vector<double>& xfA, const std::vector<double>& xfB, int n, int rows,
                    long long f, double startHz, double hzPerCol, bool refanned) {
  const int slot = (int)(f % rows);
  ms.startHz[slot] = std::isfinite(startHz) && hzPerCol > 0 ? startHz : NAN;
  ms.hzPerCol[slot] = hzPerCol > 0 ? hzPerCol : 0;
  if (refanned) { motionRecompute(ms, xfA, xfB, n, rows, f + 1); return; }
  setMotion(ms, slot, 1, 0, n);
  if (f > 0) {
    const int p = (int)((f - 1) % rows);
    double g, h;
    pairMotion(ms, xfA, xfB, p, slot, n, g, h);
    setMotion(ms, p, g, h, n);
    if (moves(g, h)) { if (ms.lo > ms.hi) ms.lo = (double)(f - 1); ms.hi = (double)(f - 1); }
  }
  const double oldest = (double)(f + 1 - rows);
  if (ms.hi < oldest) { ms.lo = 1; ms.hi = 0; } else if (ms.lo < oldest) ms.lo = oldest;
}

FanState newFanState(int rows) {
  FanState st;
  st.xfA.assign(rows, 1); st.xfB.assign(rows, 0); st.span.assign(rows, 0);
  return st;
}

double zoomEaseSec(double k) {
  const double oct = std::fabs(std::log2(k));
  if (!(oct > 0)) return 0;
  const double t = oct * ZOOM_EASE_S_PER_OCTAVE;
  return oct < 0.2 ? t : std::max(ZOOM_EASE_MIN_S, std::min(ZOOM_EASE_MAX_S, t));
}
double tuneEaseSec(double frac) {
  return TUNE_EASE_S + (TUNE_EASE_MAX_S - TUNE_EASE_S) * std::min(1.0, std::max(0.0, frac));
}
int zoomEaseRows(double k, double rowRate) {
  const double r = rowRate > 0 ? rowRate : EASE_ROW_RATE;
  return (int)std::max(1.0, jsRound(zoomEaseSec(k) * r));
}
int tuneEaseRows(double frac, double rowRate) {
  const double r = rowRate > 0 ? rowRate : EASE_ROW_RATE;
  return (int)std::max(2.0, jsRound(tuneEaseSec(frac) * r));
}
double viewMoveFrac(double L0, double S0, double L1, double S1) {
  if (!(S0 > 0 && S1 > 0)) return 0;
  return std::max(std::fabs((L0 - L1) / S1), std::fabs((L0 + S0 - L1) / S1 - 1));
}
// easeView
void easeView(double lo0, double span0, double lo1, double span1, double u, double& L, double& S) {
  const double x = u < 0 ? 0 : u > 1 ? 1 : u, e = x * (1.5 - 0.5 * x);
  S = span0 * std::pow(span1 / span0, e);
  const double ds = span0 - span1;
  if (std::fabs(ds) <= span1 * 1e-9) { L = lo0 + (lo1 - lo0) * e; return; }
  const double xs = (lo1 - lo0) / ds;
  L = lo0 + span0 * xs - S * xs;
}
// cappedEaseRows
int cappedEaseRows(double lo0, double span0, double lo1, double span1, int base, double rowRate, double px, bool linear) {
  const double r = rowRate > 0 ? rowRate : EASE_ROW_RATE;
  const double ceil_ = std::max((double)base, jsRound(EASE_CEIL_S * r));
  const double cap = SLOPE_CAP_FRAC / std::max(1.0, px);
  if (!(span0 > 0 && span1 > 0) || !std::isfinite(lo0) || !std::isfinite(lo1)) return base;
  double N = std::max(1, base);
  for (int i = 0; i < 6 && N < ceil_; i++) {
    double L, S;
    if (linear) { L = lo0 + (lo1 - lo0) / N; S = span0 * std::pow(span1 / span0, 1 / N); }
    else easeView(lo0, span0, lo1, span1, 1 / N, L, S);
    const double d = viewMoveFrac(lo0, span0, L, S);
    if (!(d > cap * 1.0001)) break;
    N = std::min(ceil_, std::ceil(N * d / cap));
  }
  return (int)std::max((double)base, N);
}

// fanTable
void fanTable(double a, double b, int n, int32_t* lo, int32_t* hi) {
  const double bn = b * n;
  if (a <= 1) {
    for (int x = 0; x < n; x++) {
      const double c = std::floor(a * (x + 0.5) + bn);
      if (c >= 0 && c < n) { lo[x] = (int32_t)c; hi[x] = (int32_t)c; } else { lo[x] = -1; hi[x] = -1; }
    }
    return;
  }
  for (int x = 0; x < n; x++) {
    const double l = std::max(0.0, std::floor(a * x + bn)), h = std::min((double)(n - 1), std::ceil(a * (x + 1) + bn) - 1);
    if (l <= h) { lo[x] = (int32_t)l; hi[x] = (int32_t)h; } else { lo[x] = -1; hi[x] = -1; }
  }
}

// fanRow — one eased row from the stored ring through its map; no data → the row below as it was drawn
void fanRow(FanState& st, const uint8_t* ring, int n, int r, int rows) {
  const double a = st.xfA[r], b = st.xfB[r];
  if (!st.hasDisp || (a == 1 && b == 0)) return;
  if ((int)st.lo_.size() != n) { st.lo_.assign(n, 0); st.hi_.assign(n, 0); }
  int32_t* lo = st.lo_.data(); int32_t* hi = st.hi_.data();
  uint8_t* disp = st.disp.data();
  const size_t at = (size_t)r * n;
  const long long pv = rows > 0 ? (long long)((r + rows - 1) % rows) * n : -1;
  fanTable(a, b, n, lo, hi);
  for (int x = 0; x < n; x++) {
    const int l = lo[x];
    if (l < 0) { disp[at + x] = pv >= 0 ? disp[pv + x] : 0; continue; }
    uint8_t m = ring[at + l];
    for (int k = l + 1, h = hi[x]; k <= h; k++) { const uint8_t v = ring[at + k]; if (v > m) m = v; }
    disp[at + x] = m;
  }
}

// fanNoteRow (the JUMP re-map branch is behind JUMP_KEEPS_HISTORY = true in the app and is not ported)
bool fanNoteRow(FanState& st, const uint8_t* /*ring*/, int /*n*/, int /*rows*/, long long frames, int /*visible*/,
                double lo, double span) {
  st.jumpHz = 0;
  if (!(span > 0)) return false;
  if (!std::isfinite(lo)) {
    if (!(st.lastSpan > 0)) return false;
    lo = st.lastLo + (st.lastSpan - span) / 2;
  }
  if (frames > 0 && st.lastSpan > 0 && spanChanged(st.lastSpan, span)) {
    if (!(st.easeLeft > 0)) { st.dLo = st.lastLo; st.dSpan = st.lastSpan; }
    st.eLo0 = st.dLo; st.eSpan0 = st.dSpan; st.eJ = 0;
    st.eT = st.easeLeft = std::max(cappedEaseRows(st.dLo, st.dSpan, lo, span, zoomEaseRows(span / st.dSpan, st.rowRate), st.rowRate),
                                   st.easeLeft > 0 ? st.easeLeft : 0);
  }
  const double moved = std::fabs(lo - st.lastLo);
  if (frames > 0 && st.lastSpan > 0 && !spanChanged(st.lastSpan, span) && moved > st.lastSpan * TUNE_EASE_MIN_FRAC && moved < st.lastSpan) {
    if (!(st.easeLeft > 0)) { st.dLo = st.lastLo; st.dSpan = st.lastSpan; }
    st.eLo0 = st.dLo; st.eSpan0 = st.dSpan; st.eJ = 0;
    st.eT = st.easeLeft = std::max(cappedEaseRows(st.dLo, st.dSpan, lo, span, tuneEaseRows(moved / st.lastSpan, st.rowRate), st.rowRate),
                                   st.easeLeft > 0 ? st.easeLeft : 0);
  }
  static_assert(JUMP_KEEPS_HISTORY, "the jump re-map branch is not ported");
  st.lastLo = lo; st.lastSpan = span;
  return false;
}

// fanAfterRow
void fanAfterRow(FanState& st, const uint8_t* ring, int slot, int n, int rows) {
  st.span[slot] = st.lastSpan;
  if (st.easeLeft > 0 && st.lastSpan > 0 && st.eSpan0 > 0 && std::isfinite(st.eLo0)) {
    const double tl = st.lastLo, ts = st.lastSpan;
    st.eJ++; st.easeLeft = st.eT - st.eJ;
    double nl, ns;
    easeView(st.eLo0, st.eSpan0, tl, ts, (double)st.eJ / st.eT, nl, ns);
    st.dLo = nl; st.dSpan = ns;
    if (st.easeLeft > 0) {
      st.xfA[slot] = ns / ts; st.xfB[slot] = (nl - tl) / ts;
      if (!st.hasDisp) { st.disp.assign(ring, ring + (size_t)rows * n); st.hasDisp = true; }
      fanRow(st, ring, n, slot, rows);
      st.sinceZoom = 0;
      return;
    }
  } else st.easeLeft = 0;
  st.xfA[slot] = 1; st.xfB[slot] = 0;
  if (!st.hasDisp) return;
  const size_t at = (size_t)slot * n;
  std::memcpy(st.disp.data() + at, ring + at, n);
  if (++st.sinceZoom >= rows) { st.hasDisp = false; st.disp.clear(); st.disp.shrink_to_fit(); }
}

void takeFanStep(FanState& st, double& k, double& c) {
  k = st.stepK; c = st.stepC;
  st.stepK = 1; st.stepC = 0;
}

// placeRow
int placeRow(std::vector<uint8_t>& ring, int rows, int n, long long frames, const uint8_t* row,
             double rowCenterHz, double rowHzPerBin, double viewCenterHz, double viewBwHz,
             MotionState& motion, FanState& fan, int visible, double* placedStart, double* placedSpan) {
  const int slot = (int)(frames % rows);
  const int off = rowViewOffset(rowCenterHz, rowHzPerBin, viewCenterHz, viewBwHz, n);
  const double start = placedStartHz(rowCenterHz, rowHzPerBin, off, n);
  if (placedStart) *placedStart = start;
  if (placedSpan) *placedSpan = rowHzPerBin * n;
  const bool fanned = rowHzPerBin > 0 ? fanNoteRow(fan, ring.data(), n, rows, frames, visible, start, rowHzPerBin * n) : false;
  writeRow(ring.data(), slot * n, row, n, off);
  fanAfterRow(fan, ring.data(), slot, n, rows);
  motionAfterRow(motion, fan.xfA, fan.xfB, n, rows, frames, start, rowHzPerBin, fanned);
  return slot;
}

}  // namespace wf
