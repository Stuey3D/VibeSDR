// wf_model.h — the waterfall + spectrum pipeline WITHOUT GL: what WaterfallView.tsx does on its JS thread (handleFrame,
// pumpClock, pushRow) and its UI-thread display driver (paceTick, the spectrum tween), composed from the ported pure
// modules. The GL side (wf_gl.h) only uploads the rows/texels this marks dirty and sets the uniforms it computes.
//
// CPU cost per data frame: the signal processor (a few passes over N bins) and one ring row (1024 bytes) per output
// row; per display frame: a handful of scalar updates and, at ≤ 30 Hz, the trace tween over ~w/2 points. Nothing per
// pixel.
#pragma once
#include "wf_clock.h"
#include "wf_combine.h"
#include "wf_pace.h"
#include "wf_row_place.h"
#include "wf_signal.h"
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace wf {

constexpr int RING_W = 1024;   // the app's RING_W
constexpr int RING_ROWS = 1024;   // the app's RING_MIN_ROWS (a 640×480 screen never needs the grown ring)

struct WaterfallConfig {
  Scroll scroll = Scroll::Default;   // DEFAULT = 30 rows/s on a 10 fps feed (wfRowTarget)
  bool smoothTune = true;            // the app's smoothTune (the gesture's continuous mode + trace follow)
  bool boostOn = true;               // SPEED UP WHILE TUNING
  double displayHz = 60;             // uiDisplayMaxHz(): the panel's maximum
  double frameCapHz = 0;             // uiFrameCapHz: the FRAME RATE cap (0 = none)
  SignalSettings sig;                // the processor's settings (app defaults)
  double wfSharpness = 5;            // Display settings 0..10 (app default 5)
  double wfContrast = 0;             // −10..+10 (app default 0)
  bool specShow = true;
  bool peakHold = true;              // (also sig.peakHold)
};

/** The waterfall shader's uniforms for one display frame (WaterfallView wfUniforms, less the layout ones). */
struct WfUniforms {
  double headF = 0, frac = 1, quant = 1, moLo = 1, moHi = 0, zA = 1, zB = 0, sharp = 0, contrast = 0;
};

class WaterfallModel {
 public:
  explicit WaterfallModel(const WaterfallConfig& cfg = WaterfallConfig());
  void setConfig(const WaterfallConfig& cfg);
  const WaterfallConfig& config() const { return cfg_; }

  /** The view the UI shows now (the app's centerHz / bwHz props). Until it is set, each frame's own geometry is the view. */
  void setView(double centreHz, double spanHz) { viewC_ = centreHz; viewBw_ = spanHz; viewSet_ = true; }
  /** A tune/zoom input at tMs (the app's lastInteractAt) — drives the gesture mode and the interaction boost. */
  void noteInteraction(double tMs) { lastInteractAt_ = tMs; }
  /** One spectrum frame in dBFS, bins LOW → HIGH, its own centre and span (the bins' true geometry). */
  void pushFrameDb(const float* db, int n, double centreHz, double spanHz, double tMs);
  /** One engine SPEC frame: u8 bins (dBFS = u8 − 256, 0 = no data). `fftOrder` = DC first (rotated by N/2 here). */
  void pushFrameU8(const uint8_t* bins, int n, bool fftOrder, double centreHz, double spanHz, double tMs);
  /** One display frame at tMs (gapMs since the last): runs the clock's due ticks, the display driver, the trace tween. */
  void tick(double tMs, double gapMs);

  // ── outputs ──
  const uint8_t* imageBytes() const { return wf::imageBytes(fan_, ring_); }   // RING_ROWS × RING_W, ring order
  const MotionState& motion() const { return ms_; }
  long long frames() const { return frameCount_; }
  double availTaken() const { return jsAvail_; }      // the newest row the display driver has taken (jsAvail)          // rows written (the image's newest = frames()-1)
  WfUniforms uniforms() const;
  /** Ring slots written since the last call (each needs one texture-row upload) — and motion texels changed. */
  void takeDirty(std::vector<int>& rows, std::vector<int>& motionSlots);
  /** The spectrum trace as displayed (0..1, one value per SPEC_PX_STEP px at the last width given) and its peak. */
  const std::vector<double>& traceDisp() const { return specDisp_; }
  const std::vector<double>& peakLine() const { return peakShown_; }
  bool traceValid() const { return !specDisp_.empty(); }
  void setTraceWidth(int w) { traceW_ = w; }
  double dbMin() const { return proc_.dbMin(); }
  double dbMax() const { return proc_.dbMax(); }
  const FanState& fan() const { return fan_; }
  const RowClockState& clock() const { return clock_; }
  const PaceState& pace() const { return pace_; }

  // hook for the golden test: called after every row is placed
  void (*onRow)(void* ctx, const WaterfallModel& m, int slot) = nullptr;
  void (*onFrame)(void* ctx, const ProcessedFrame& f) = nullptr;
  void* onRowCtx = nullptr;

 private:
  void pushRow(const uint8_t* src, int srcN, double rowCenterHz, double rowHzPerBin);
  void pumpClock(double now);
  void traceShow(std::vector<double>&& spec, std::vector<double>&& peak, bool hasPeak, double at, bool shrunk);
  void traceRun(double now);

  WaterfallConfig cfg_;
  SignalProcessor proc_;
  std::vector<uint8_t> ring_;
  std::vector<uint8_t> rowBuf_;
  MotionState ms_;
  FanState fan_;
  long long frameCount_ = 0;
  std::vector<int> dirtyRows_;
  // view / interaction
  double viewC_ = 0, viewBw_ = 0; bool viewSet_ = false;
  double lastInteractAt_ = 0;
  // handleFrame state
  double lastFrameTs_ = 0, avgFrameMs_ = 150, lastWfPushAt_ = 0, avgWfGapMs_ = 0, rowCarry_ = 0;
  RowTargetState rowTargetSt_; int rowTargetCap_ = -1;
  // the clock (pumpClock)
  RowClockState clock_;
  BoostState boost_;
  struct { double base = 0, boosted = 0, hz = 60; } cri_;
  int lastEaseLeft_ = 0;
  long long clockSeq_ = 0, clockShown_ = 0;
  std::map<long long, CombFrame> clockFrames_;
  RowCombiner clockComb_;
  std::vector<long long> clockOut_;
  // the display driver
  PaceState pace_;
  double jsAvail_ = 0; long long jsSeenGen_ = 0, pendGen_ = 0;
  double takenMoLo_ = 1, takenMoHi_ = 0;
  bool clockArmed_ = false; double lastPumpAt_ = 0;
  struct { double avail = 0, moLo = 1, moHi = 0, k = 1, c = 0; } pend_;
  // the spectrum trace (traceShow / the tween)
  struct TraceItem { double at; std::vector<double> spec, peak; bool hasPeak; };
  std::deque<TraceItem> traceQ_;
  std::vector<double> specTo_, specDisp_, peakShown_;
  double specAcc_ = 0; bool tweenActive_ = false;
  int traceW_ = 640;
  std::vector<float> dbScratch_;
};

}  // namespace wf
