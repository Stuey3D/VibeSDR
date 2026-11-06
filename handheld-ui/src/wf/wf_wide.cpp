// wf_wide.cpp — port of src/utils/wfWide.ts. Comments name the TS function each block comes from; the design notes live in
// the TS file and are not repeated here. Float32Array stores are (float) casts of the double the TS computes.
#include "wf_wide.h"
#include "wf_row_place.h"   // jsRound
#include <algorithm>
#include <cstring>
#include <numeric>

namespace wf {

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// isWideFrame
bool isWideFrame(const uint8_t* u8, size_t len) {
  return len >= 4 && u8[0] == 0x53 && u8[1] == 0x50 && u8[2] == 0x57 && u8[3] == 0x42;
}

// parseWide
bool parseWide(const uint8_t* u8, size_t len, WideRow& out, double offsetDb) {
  if (len < (size_t)WIDE_HDR || !isWideFrame(u8, len) || u8[4] != WIDE_VERSION) return false;
  const int F = rd16(u8 + 26), C = rd16(u8 + 28), P = rd16(u8 + 30);
  if (!F || !C || len < (size_t)(WIDE_HDR + 2 * F + P * 3)) return false;
  out.floor.resize(F); out.spread.resize(F);
  for (int i = 0; i < F; i++) { const int b = u8[WIDE_HDR + i]; out.floor[i] = b == 0 ? NAN : (float)(b + offsetDb); }
  for (int i = 0; i < F; i++) out.spread[i] = (float)(u8[WIDE_HDR + F + i] / 10.0);
  out.pkCell.assign(P, 0); out.pkDb.assign(P, 0); out.pkShape.assign(P, 0);
  int n = 0, cell = -1; size_t o = WIDE_HDR + 2 * (size_t)F;
  for (int i = 0; i < P; i++, o += 3) {
    const int lvl = u8[o + 1];
    cell += u8[o];
    if (lvl == 0) continue;   // an escape: advance only
    if (cell >= 0 && cell < C) { out.pkCell[n] = cell; out.pkDb[n] = (float)(lvl + offsetDb); out.pkShape[n] = u8[o + 2]; n++; }
  }
  out.nPk = n; out.cells = C;
  out.centreHz = (double)rd32(u8 + 14) + (double)rd32(u8 + 18) * 4294967296.0;
  out.spanHz = (double)rd32(u8 + 22);
  return true;
}

// floorAt
double floorAt(const std::vector<float>& floor, double x) {
  const int F = (int)floor.size();
  if (F < 2) return F ? (double)floor[0] : NAN;
  const double p = x * F - 0.5;
  if (p != p) return NAN;   // (the TS reads floor[NaN] → undefined → NaN)
  double i = std::floor(p), t = p - i;
  if (i < 0) { i = 0; t = 0; }
  if (i >= F - 1) { i = F - 2; t = 1; }
  const double a = floor[(int)i], b = floor[(int)i + 1];
  return a == a ? (b == b ? a + (b - a) * t : a) : b;
}

// floorIdx
static const double GH_Z[5] = { 0, 1.3556262, -1.3556262, 2.8569700, -2.8569700 };
static const double GH_W[5] = { 0.5333333, 0.2220759, 0.2220759, 0.0112574, 0.0112574 };
int floorIdx(double m, double sigma, const ToIdx& toIdx) {
  if (!(sigma > 0.05)) return toIdx(m);
  double s = 0;
  for (int k = 0; k < 5; k++) s += GH_W[k] * toIdx(m + sigma * GH_Z[k]);
  return (int)jsRound(s);
}

// makeToIdx's closure
int ToIdx::operator()(double db) const {
  const double range = dbMax - dbMin > 0 ? dbMax - dbMin : 1, CLIP = 14.97;
  const double nrm = jsMax(0, jsMin(1, (db + offDb + brightDb - dbMin) / range));
  const double mag = nrm * 255;
  return (int)jsRound(mag < CLIP ? 0 : (mag - CLIP) / (255 - CLIP) * 255);
}

// medianOf
double medianOf(const float* a, int N, std::vector<float>& scratch) {
  if ((int)scratch.size() < N) scratch.assign(std::max(64, N), 0.0f);
  float* s = scratch.data();
  int m = 0;
  for (int i = 0; i < N; i++) { const float v = a[i]; if (v == v) s[m++] = v; }
  if (m < 8) return NAN;
  const int k = m >> 1;
  int lo = 0, hi = m - 1;
  while (lo < hi) {
    const float p = s[(lo + hi) >> 1];
    int i = lo, j = hi;
    while (i <= j) {
      while (s[i] < p) i++;
      while (s[j] > p) j--;
      if (i <= j) { const float t = s[i]; s[i] = s[j]; s[j] = t; i++; j--; }
    }
    if (k <= j) hi = j; else if (k >= i) lo = i; else break;
  }
  return s[k];
}

// wideArrive
void wideArrive(WideState& st, const WideRow& row, double now, double viewLo, double viewSpan, double detP50) {
  const int F = (int)row.floor.size(), C = row.cells;
  const bool same = st.hasRow && st.row.centreHz == row.centreHz && st.row.spanHz == row.spanHz && st.F == F && st.C == C;
  if (st.F != F) {
    st.F = F;
    st.wFloor.assign(F, 0); st.wSpread.assign(F, 0); st.pFloor.assign(F, 0);
    st.pSpread.assign(F, 0); st.bFloor.assign(F, 0); st.bSpread.assign(F, 0);
  }
  if (st.C != C) {
    st.C = C;
    st.wCell.assign(C, 0); st.wShape.assign(C, 0); st.pCell.assign(C, 0);
    st.pShape.assign(C, 0); st.bCell.assign(C, 0); st.bShape.assign(C, 0);
  }
  if (same) {
    const bool mid = st.hasPrev;
    st.pFloor = mid ? st.bFloor : st.wFloor; st.pSpread = mid ? st.bSpread : st.wSpread;
    st.pCell = mid ? st.bCell : st.wCell; st.pShape = mid ? st.bShape : st.wShape;
    st.hasPrev = true;
    const double gap = now - st.at;
    if (gap > 0 && gap < 5000) st.gap += (gap - st.gap) * 0.3;
  } else st.hasPrev = false;
  st.wFloor = row.floor; st.wSpread = row.spread;
  std::fill(st.wCell.begin(), st.wCell.end(), (float)NAN);
  for (int i = 0; i < row.nPk; i++) { st.wCell[row.pkCell[i]] = row.pkDb[i]; st.wShape[row.pkCell[i]] = row.pkShape[i]; }
  st.row = row; st.hasRow = true; st.at = now;
  // the floor match
  if (viewSpan > 0 && detP50 == detP50) {
    const double wLo = row.centreHz - row.spanHz / 2;
    double sum = 0; int n = 0;
    for (int i = 0; i < 16; i++) {
      const double f = floorAt(row.floor, (viewLo + (i + 0.5) / 16 * viewSpan - wLo) / row.spanHz);
      if (f == f) { sum += f; n++; }
    }
    if (n >= 8) {
      const double off = jsMax(-30, jsMin(30, detP50 - sum / n));
      st.off = st.offSet ? st.off + (off - st.off) * 0.3 : off;
      st.offSet = true;
    }
  }
}

// wideBlend
bool wideBlend(WideState& st, double now) {
  if (!st.hasRow || now - st.at > WIDE_STALE_MS) return false;
  const int F = st.F, C = st.C;
  const double t = st.hasPrev ? jsMax(0, jsMin(1, (now - st.at) / jsMax(50, st.gap))) : 1;
  if (t >= 1) { st.bFloor = st.wFloor; st.bSpread = st.wSpread; st.bCell = st.wCell; st.bShape = st.wShape; return true; }
  for (int i = 0; i < F; i++) {
    const double a = st.pFloor[i], b = st.wFloor[i];
    st.bFloor[i] = (float)(a == a && b == b ? a + (b - a) * t : b);
    st.bSpread[i] = (float)((double)st.pSpread[i] + ((double)st.wSpread[i] - (double)st.pSpread[i]) * t);
  }
  for (int c = 0; c < C; c++) {
    const double p = st.pCell[c], q = st.wCell[c];
    if (p != p && q != q) { st.bCell[c] = NAN; continue; }
    const double fl = floorAt(st.bFloor, (c + 0.5) / C);
    if (p == p && q == q) { st.bCell[c] = (float)(p + (q - p) * t); st.bShape[c] = st.wShape[c]; }
    else if (q == q) { st.bCell[c] = (float)(fl + jsMax(0, q - fl) * t); st.bShape[c] = st.wShape[c]; }
    else { st.bCell[c] = (float)(fl + jsMax(0, p - fl) * (1 - t)); st.bShape[c] = st.pShape[c]; }
  }
  return true;
}

// rowInCapture
bool rowInCapture(double rowLo, double rowSpan, double capCentreHz, double capSpanHz) {
  const double c = rowLo + rowSpan / 2;
  return std::fabs(c - capCentreHz) <= capSpanHz / 2;
}

// newLayerRing
LayerRing newLayerRing(int rows, int F, int C, int P) {
  LayerRing L;
  L.rows = rows; L.F = F; L.C = C; L.P = P;
  L.k.assign(rows, 0); L.m.assign(rows, 0);
  L.floor.assign((size_t)rows * F, 0);
  L.pkN.assign(rows, 0);
  L.pkCell.assign((size_t)rows * P, 0); L.pkIdx.assign((size_t)rows * P, 0); L.pkShape.assign((size_t)rows * P, 0);
  L.fadeTx = 6;
  L.tmpC.assign(C, 0); L.tmpI.assign(C, 0);
  return L;
}

// layerWriteRow
double layerWriteRow(LayerRing& L, int slot, WideState* st, double rowLo, double rowSpan, double now, const ToIdx& toIdx) {
  L.k[slot] = 0; L.pkN[slot] = 0;
  if (!st || !st->hasRow || !(rowSpan > 0) || !std::isfinite(rowLo) || rowSpan >= st->row.spanHz * LAYER_FULL || st->F != L.F
      || st->C != L.C || !rowInCapture(rowLo, rowSpan, st->row.centreHz, st->row.spanHz) || !wideBlend(*st, now)) return 0;
  const WideRow& w = st->row;
  const int F = L.F, C = L.C, P = L.P; const size_t fo = (size_t)slot * F;
  for (int i = 0; i < F; i++) {
    const double v = st->bFloor[i];
    L.floor[fo + i] = v == v ? (uint8_t)std::max(1, std::min(255, floorIdx(v, st->bSpread[i], toIdx))) : 0;
  }
  int n = 0;
  for (int c = 0; c < C; c++) {
    const double v = st->bCell[c];
    if (v != v) continue;
    const int ix = std::min(255, toIdx(v));
    if (ix < 1) continue;
    L.tmpC[n] = c; L.tmpI[n] = (uint8_t)ix; n++;
  }
  if (n > P) {   // keep the strongest P — the TS's stable sort by index descending, then back into cell order
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return L.tmpI[b] < L.tmpI[a]; });
    order.resize(P);
    std::sort(order.begin(), order.end());
    std::vector<int32_t> cs(P); std::vector<uint8_t> is(P);
    for (int i = 0; i < P; i++) { cs[i] = L.tmpC[order[i]]; is[i] = L.tmpI[order[i]]; }
    for (int i = 0; i < P; i++) { L.tmpC[i] = cs[i]; L.tmpI[i] = is[i]; }
    n = P;
  }
  const size_t po = (size_t)slot * P;
  for (int i = 0; i < n; i++) { L.pkCell[po + i] = (uint16_t)L.tmpC[i]; L.pkIdx[po + i] = L.tmpI[i]; L.pkShape[po + i] = st->bShape[L.tmpC[i]]; }
  L.pkN[slot] = (uint16_t)n;
  L.k[slot] = rowSpan / w.spanHz;
  L.m[slot] = (rowLo - (w.centreHz - w.spanHz / 2)) / w.spanHz;
  return L.k[slot];
}

// layerFloorRun — the floor into disp[at + x0 .. at + x1): linear between its bins, 0 where none or beyond the capture
static void layerFloorRun(const LayerRing& L, size_t fo, double A, double B, int x0, int x1, uint8_t* disp, size_t at) {
  if (x1 <= x0) return;
  const int F = L.F; const uint8_t* fl = L.floor.data();
  double xc0d = std::ceil(-B / A - 1e-9);
  if (xc0d < x0) xc0d = x0;
  if (xc0d > x1) xc0d = x1;
  double xc1d = std::floor((1 - B) / A + 1e-9) + 1;
  if (xc1d < xc0d) xc1d = xc0d;
  if (xc1d > x1) xc1d = x1;
  const int xc0 = (int)xc0d, xc1 = (int)xc1d;
  if (xc0 > x0) std::memset(disp + at + x0, 0, (size_t)(xc0 - x0));
  if (x1 > xc1) std::memset(disp + at + xc1, 0, (size_t)(x1 - xc1));
  if (F < 2) { if (xc1 > xc0) std::memset(disp + at + xc0, fl[fo], (size_t)(xc1 - xc0)); return; }
  const double P0 = B * F - 0.5, dP = A * F;
  int x = xc0;
  while (x < xc1) {
    const double p = P0 + dP * x;
    int i; double xe;
    if (p < 0) { i = -1; xe = std::ceil(-P0 / dP - 1e-9); }
    else { i = (int)p; xe = i >= F - 1 ? xc1 : std::ceil((i + 1 - P0) / dP - 1e-9); }
    if (xe <= x) xe = x + 1;
    if (xe > xc1) xe = xc1;
    const int xi = (int)xe;
    if (i < 0 || i >= F - 1) {
      const uint8_t fa = fl[fo + (i < 0 ? 0 : F - 2)], fb = fl[fo + (i < 0 ? 1 : F - 1)];
      std::memset(disp + at + x, i < 0 ? (fa ? fa : fb) : (fb ? fb : fa), (size_t)(xi - x));
      x = xi; continue;
    }
    const uint8_t fa = fl[fo + i], fb = fl[fo + i + 1];
    if (!fa || !fb) { std::memset(disp + at + x, fa ? fa : fb, (size_t)(xi - x)); x = xi; continue; }
    double v = fa + (double)(fb - fa) * (p - i) + 0.5;
    const double dv = (double)(fb - fa) * dP;
    for (; x < xi; x++, v += dv) disp[at + x] = (uint8_t)(int32_t)v;   // (v | 0: v is non-negative)
  }
}

// layerFloorOne — the floor at one capture fraction, or −1
static int layerFloorOne(const LayerRing& L, size_t fo, double uc) {
  if (uc < 0 || uc > 1) return -1;
  const int F = L.F; const uint8_t* fl = L.floor.data();
  if (F < 2) return fl[fo] ? fl[fo] : -1;
  const double p = uc * F - 0.5;
  double i = std::floor(p), t = p - i;
  if (i < 0) { i = 0; t = 0; } else if (i >= F - 1) { i = F - 2; t = 1; }
  const int fa = fl[fo + (int)i], fb = fl[fo + (int)i + 1];
  if (!fa && !fb) return -1;
  return !fa ? fb : !fb ? fa : (int)jsRound(fa + (fb - fa) * t);
}

// layerFillRow
void layerFillRow(LayerRing& L, int slot, double a, double b, int n, uint8_t* disp, size_t at, const int32_t* lo) {
  const double k = L.k[slot];
  if (!(k > 0) || !(a > 0)) return;
  const double A = k * a / n, B = k * (a * 0.5 / n + b) + L.m[slot];   // uc(x) = A·x + B
  const double XL = -b / a * n, XR = (1 - b) / a * n, fade = L.fadeTx;
  if (XL <= 0 && XR >= n) return;   // the detailed row covers the screen: nothing to fill, no edge to fade
  const size_t fo = (size_t)slot * L.F;
  int xa = 0; while (xa < n && lo[xa] < 0) xa++;
  int xb = n; while (xb > xa && lo[xb - 1] < 0) xb--;
  layerFloorRun(L, fo, A, B, 0, xa, disp, at);
  layerFloorRun(L, fo, A, B, xb, n, disp, at);
  // the soft edge's texels: inside the detailed run, within `fade` of an ON-SCREEN edge — [xa, eL) and [sR, xb)
  const bool soft = fade > 0 && xa < xb;
  const int eL = soft && XL > 0 ? (int)std::min((double)xb, std::ceil(XL + fade)) : xa;
  const int sR = soft && XR < n ? (int)std::max((double)eL, std::floor(XR - fade)) : xb;
  if ((int)L.lay.size() < n) L.lay.assign(n, 0);
  int16_t* lay = L.lay.data();
  for (int x = xa; x < eL; x++) lay[x] = (int16_t)layerFloorOne(L, fo, A * x + B);
  for (int x = sR; x < xb; x++) lay[x] = (int16_t)layerFloorOne(L, fo, A * x + B);
  // the peaks: into the blank runs (where the floor has data) and the soft edge's layer values
  const int C = L.C; const size_t po = (size_t)slot * L.P; const int np = L.pkN[slot]; const double half = 0.5 * A;
  const double cLo = std::floor((B - half) * C) - 1, cHi = std::ceil((A * n + B + half) * C) + 1;
  int i0 = 0, i1 = np;
  while (i0 < i1) { const int mid = (i0 + i1) >> 1; if ((double)L.pkCell[po + mid] < cLo) i0 = mid + 1; else i1 = mid; }
  for (int i = i0; i < np; i++) {
    const int cell = L.pkCell[po + i], sh = L.pkShape[po + i], ix = L.pkIdx[po + i];
    if (cell > cHi) break;
    const double c0 = (cell + ((sh & 15) + 0.5) / 16) / C, hw = jsMax(half, ((sh >> 4) + 1) / 32.0 / C);
    const double x0d = jsMax(0, std::ceil((c0 - hw - B) / A - 1e-9)), x1d = jsMin(n - 1, std::floor((c0 + hw - B) / A + 1e-9));
    if (!(x0d <= x1d)) continue;
    const int x0 = (int)x0d, x1 = (int)x1d;
    for (int x = x0; x <= x1; x++) {
      if (x < xa || x >= xb) { const int d = disp[at + x]; if (d > 0 && ix > d) disp[at + x] = (uint8_t)ix; }
      else if (x < eL || x >= sR) { const int v = lay[x]; if (v >= 0 && ix > v) lay[x] = (int16_t)ix; }
      else x = sR - 1;   // the detailed run between the soft edges: untouched
    }
  }
  for (int x = xa; x < xb; x++) {
    if (x == eL && x < sR) x = sR;
    if (x >= xb) break;
    const int v = lay[x];
    if (v < 0) continue;
    const double c = x + 0.5;
    double e = 1;
    if (XL > 0) e = jsMin(e, (c - XL) / fade);
    if (XR < n) e = jsMin(e, (XR - c) / fade);
    if (e >= 1) continue;
    if (e < 0) e = 0;
    const int d = disp[at + x];
    disp[at + x] = (uint8_t)(int)jsRound(v + (d - v) * e);
  }
}

}  // namespace wf
