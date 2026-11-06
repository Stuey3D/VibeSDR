// wf_row_place.h — where one spectrum row goes in the waterfall's history ring: the per-row placement, the MOTION
// (tune h / zoom g between rows, drawn by the shader), the zoom/tune EASE (the fan), the slope cap.
// A line-for-line C++ port of the app's src/utils/wfRowPlace.ts (V12, branch v12-r36s). Same maths, same constants.
// No GL here: it is tested against the TS module by handheld-ui/test/run_golden.sh.
//
// ★★★ THE HARD RULE (Stuart, 2026-10-27/28): every row is written ONCE, placed against the view it lands in, and the
//     stored history is never rewritten. A zoom NEVER rescales the history — old rows stay as drawn, new rows arrive at
//     the new zoom with a short ease (the fan); every tune step past 2 columns eases over a few rows; edges with no data
//     continue the row below (never black wedges).
//
// Not ported (see README): the full-capture layer (wfWide.ts — SPWB rows; fanRow/fanAfterRow call layerFillRow only
// when a layer exists) and the JUMP re-map branch (JUMP_KEEPS_HISTORY is true in the app, so that branch never runs).
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>

namespace wf {

// JS Math.round: floor(x + 0.5) semantics (half toward +inf), exact for the edge cases.
inline double jsRound(double x) { double r = std::floor(x); return (x - r >= 0.5) ? r + 1.0 : r; }

constexpr int MOTION_ROWS = 4;
constexpr double ZOOM_SCALE = 4096;
constexpr double MOTION_SCALE = 64;
constexpr int MOTION_ZERO = 32768;
constexpr double MOTION_MAX_FRAC = 1.0 / 8;

constexpr double ZOOM_EASE_S_PER_OCTAVE = 0.33;
constexpr double ZOOM_EASE_MIN_S = 0.25;
constexpr double ZOOM_EASE_MAX_S = 0.5;
constexpr double TUNE_EASE_S = 0.25;
constexpr double TUNE_EASE_MAX_S = 0.35;
constexpr double EASE_ROW_RATE = 30;
constexpr double TUNE_EASE_MIN_FRAC = 2.0 / 1024;
constexpr double SLOPE_CAP_FRAC = 1.0 / 256;
constexpr double EASE_CEIL_S = 1.0;
constexpr bool JUMP_KEEPS_HISTORY = true;

int rowViewOffset(double rowCenterHz, double rowHzPerBin, double viewCenterHz, double viewBwHz, int n);
void writeRow(uint8_t* ring, int slotOff, const uint8_t* row, int n, int off);
bool spanChanged(double span0, double span1);
double placedStartHz(double rowCenterHz, double rowHzPerBin, int off, int n);

int encodeMotion(double h);
double decodeMotion(int hi, int lo);
int encodeZoom(double g);
double decodeZoom(int hi, int lo);

/** Per-slot placement + the move to the next newer row. `tex` is the shader's motion texture: RGBA8, one texel a slot
 *  (hHi, hLo, gHi, gLo) — the same 16-bit codes the app's image carries in its four motion planes (motionAt). */
struct MotionState {
  std::vector<double> startHz, hzPerCol;
  std::vector<float> h, g;
  std::vector<uint8_t> tex;   // 4 × rows
  int rows = 0, n = 0;
  double lo = 1, hi = 0;      // absolute frames whose pair f → f+1 moves; lo > hi = none
  // Slots whose motion texel changed since the last takeDirty (the GL side uploads only these).
  std::vector<int> dirty;
};
MotionState newMotionState(int rows, int n);
void setMotion(MotionState& ms, int slot, double g, double h, int n);

struct FanState {
  double lastLo = 0, lastSpan = 0;
  std::vector<double> xfA, xfB;
  std::vector<uint8_t> disp;   // the displayed ring; empty = the stored ring IS the display (hasDisp false)
  bool hasDisp = false;
  int sinceZoom = 0;
  double stepK = 1, stepC = 0;
  double dLo = NAN, dSpan = NAN; int easeLeft = 0;
  double eLo0 = NAN, eSpan0 = NAN; int eJ = 0, eT = 0;
  std::vector<double> span;
  double rowRate = EASE_ROW_RATE;
  double jumpHz = 0;
  std::vector<int32_t> lo_, hi_;   // fanTable scratch
};
FanState newFanState(int rows);

bool displayMap(const MotionState& ms, const std::vector<double>& xfA, const std::vector<double>& xfB, int r, int n,
                double& P, double& Q);
void pairMotion(const MotionState& ms, const std::vector<double>& xfA, const std::vector<double>& xfB, int a, int b, int n,
                double& g, double& h);
void motionAfterRow(MotionState& ms, const std::vector<double>& xfA, const std::vector<double>& xfB, int n, int rows,
                    long long f, double startHz, double hzPerCol, bool refanned);

double zoomEaseSec(double k);
double tuneEaseSec(double frac);
int zoomEaseRows(double k, double rowRate = EASE_ROW_RATE);
int tuneEaseRows(double frac, double rowRate = EASE_ROW_RATE);
double viewMoveFrac(double L0, double S0, double L1, double S1);
void easeView(double lo0, double span0, double lo1, double span1, double u, double& L, double& S);
int cappedEaseRows(double lo0, double span0, double lo1, double span1, int base, double rowRate = EASE_ROW_RATE,
                   double px = 1, bool linear = false);
void fanTable(double a, double b, int n, int32_t* lo, int32_t* hi);
void fanRow(FanState& st, const uint8_t* ring, int n, int r, int rows = 0);
bool fanNoteRow(FanState& st, const uint8_t* ring, int n, int rows, long long frames, int visible, double lo, double span);
void fanAfterRow(FanState& st, const uint8_t* ring, int slot, int n, int rows);
void takeFanStep(FanState& st, double& k, double& c);

/** The bytes the shader's ring texture shows (disp when a zoom has one, else the stored ring). */
inline const uint8_t* imageBytes(const FanState& st, const std::vector<uint8_t>& ring) {
  return st.hasDisp ? st.disp.data() : ring.data();
}

/** pushRow's whole placement (wfRowPlace.ts placeRow). Returns the slot written. */
int placeRow(std::vector<uint8_t>& ring, int rows, int n, long long frames, const uint8_t* row,
             double rowCenterHz, double rowHzPerBin, double viewCenterHz, double viewBwHz,
             MotionState& motion, FanState& fan, int visible, double* placedStart = nullptr, double* placedSpan = nullptr);

}  // namespace wf
