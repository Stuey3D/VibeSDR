// wf_wide.h — THE FULL-CAPTURE LAYER under the waterfall's history: a line-for-line C++ port of the app's
// src/utils/wfWide.ts (V12, branch v12-r36s; docs/v12/WATERFALL-FULL-CAPTURE.md). Same maths, same constants, the same
// float32 storage where the TS uses Float32Arrays, and JS's NaN-propagating Math.max/min where a NaN can reach them.
//
// WHAT IT IS FOR: a VibeServer asked with `&wide=1` (and whose hwinfo says "wide":2) follows every 4th SPEC frame, while
// the listener's view is narrower than the capture, with an SPWB v2 row of the WHOLE capture: its floor (F medians + their
// spread σ) and its peaks (≤ 110 cells of C whose strongest bin stands out). Every ring row written while a fresh SPWB is
// held also gets a LAYER ROW at the same slot, in capture space. Where the fan draws an eased row (a tune glide or a zoom
// ease — wf_row_place fanRow), the texels outside the row's own data are filled from its layer row through the same map
// instead of continuing the row below — so a long drag no longer streaks the edge column up the screen.
//
// Not ported from the TS (nothing calls them on the handheld): wideCapture (the app's history-covers-capture view hint).
// The wire format is android/app/src/main/cpp/vibe_wide_row.h (the encoder) and docs/PROTOCOL.md (2026-10-28).
//
// COST: no GPU work at all (the layer is written into the displayed ring on the CPU, in the rows that are uploaded
// anyway). RAM: 1024 × (128 + 4·160) B ≈ 0.8 MB, allocated at the first SPWB only. CPU: see README.md.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wf {

constexpr uint32_t WIDE_MAGIC = 0x42575053;   // 'SPWB' little-endian
constexpr int WIDE_HDR = 32;
constexpr int WIDE_VERSION = 2;
constexpr double WIDE_STALE_MS = 4000;        // a full-capture row older than this is not drawn
constexpr int LAYER_PEAKS = 160;              // per ring row, the most peaks the layer keeps
constexpr double LAYER_FULL = 0.98;           // a row covering ≥ this fraction of the capture has no layer

// JS Math.max / Math.min for two numbers: a NaN in either gives NaN (std::max/min do not).
inline double jsMax(double a, double b) { return (a != a || b != b) ? NAN : (a > b ? a : b); }
inline double jsMin(double a, double b) { return (a != a || b != b) ? NAN : (a < b ? a : b); }

/** One full-capture row, decoded: dB (float32, as the TS's Float32Arrays), NaN = no data. */
struct WideRow {
  double centreHz = 0, spanHz = 0;
  std::vector<float> floor, spread;   // F floor bins low → high; the floor's σ (dB)
  int cells = 0;                      // C
  std::vector<int32_t> pkCell; std::vector<float> pkDb; std::vector<uint8_t> pkShape; int nPk = 0;
};

/** Is this binary frame an SPWB row (by its magic)? Check it BEFORE the SPEC parser. */
bool isWideFrame(const uint8_t* u8, size_t len);
/** Parse an SPWB v2 frame into `out` (dB = byte + offsetDb; a 0 byte is no data). False if it is not one. */
bool parseWide(const uint8_t* u8, size_t len, WideRow& out, double offsetDb = -256);
/** The floor at capture fraction x (0..1): linear between floor-bin centres, NaN where there is none. */
double floorAt(const std::vector<float>& floor, double x);
/** E[idx(m + σZ)] by 5-point Gauss–Hermite (σ ≤ 0.05 → idx(m)). */
struct ToIdx {
  double dbMin, dbMax, brightDb, offDb;
  int operator()(double db) const;
};
int floorIdx(double m, double sigma, const ToIdx& toIdx);
inline ToIdx makeToIdx(double dbMin, double dbMax, double brightDb, double offDb) { return ToIdx{ dbMin, dbMax, brightDb, offDb }; }
/** The median of the finite values of a[0..n), NaN if fewer than 8 (quickselect on `scratch`, as the TS). */
double medianOf(const float* a, int n, std::vector<float>& scratch);

/** The newest picture of the capture and its time blend (wfWide.ts WideState). */
struct WideState {
  bool hasRow = false; WideRow row;
  double at = 0, gap = 400;
  int F = 0, C = 0;
  std::vector<float> wFloor, wSpread, wCell; std::vector<uint8_t> wShape;   // newest (per cell, NaN = none)
  std::vector<float> pFloor, pSpread, pCell; std::vector<uint8_t> pShape;   // previous (blend from)
  bool hasPrev = false;
  std::vector<float> bFloor, bSpread, bCell; std::vector<uint8_t> bShape;   // as drawn now
  double off = 0; bool offSet = false;                                      // the floor match (dB)
};
/** A row arrived at `now` (ms). viewLo/viewSpan = the latest detailed row's placement (Hz); detP50 = the median of its dB. */
void wideArrive(WideState& st, const WideRow& row, double now, double viewLo, double viewSpan, double detP50);
/** The layer as drawn for a row written at `now`, blended from the previous row to the newest. False = nothing fresh. */
bool wideBlend(WideState& st, double now);
/** A row whose centre is outside the capture gets no layer. */
bool rowInCapture(double rowLo, double rowSpan, double capCentreHz, double capSpanHz);

/** One layer row per ring slot, in capture space (wfWide.ts LayerRing). */
struct LayerRing {
  int rows = 0, F = 0, C = 0, P = 0;
  std::vector<double> k, m;            // per slot: k = rowSpan/capSpan (0 = no layer), m = (rowLo − capLo)/capSpan
  std::vector<uint8_t> floor;          // rows × F palette indices, 0 = no data
  std::vector<uint16_t> pkN;           // rows
  std::vector<uint16_t> pkCell; std::vector<uint8_t> pkIdx, pkShape;   // rows × P, by cell
  double fadeTx = 6;                   // the soft edge (texels) where a detailed row's edge is on screen
  std::vector<int16_t> lay;            // fill scratch
  std::vector<int32_t> tmpC; std::vector<uint8_t> tmpI;   // write scratch (C)
};
LayerRing newLayerRing(int rows, int F, int C, int P = LAYER_PEAKS);
/** The layer row for the ring row just written at `slot`, placed at (rowLo, rowSpan) Hz. Returns k (0 = none). */
double layerWriteRow(LayerRing& L, int slot, WideState* st, double rowLo, double rowSpan, double now, const ToIdx& toIdx);
/** Fill displayed row `slot` (drawn under map a, b into disp[at .. at+n)) outside its detailed extent from its layer row.
 *  `lo` = fanTable's lo for this map (−1 = no detailed data). */
void layerFillRow(LayerRing& L, int slot, double a, double b, int n, uint8_t* disp, size_t at, const int32_t* lo);

}  // namespace wf
