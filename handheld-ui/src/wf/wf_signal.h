// wf_signal.h — the app's dBFS → display mapping (auto-range, spatial smooth, spectrum EMA, peak hold, clip + stretch):
// a port of src/assets/signalProcessor.ts (class SignalProcessor), line for line. The only change: the clock is passed
// in (nowMs, monotonic) instead of read from performance.now(), so the golden test can drive both with the same times.
// Output `row` = 0–255 intensity bytes for the ring (NOT LUT indices — unsharp, S-curve and the palette are shader-side),
// `spec`/`peak` = 0..1 trace values.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

namespace wf {

struct SignalSettings {
  double autoContrast = 5;      // app default (SDRScreen useState 5)
  double minRangeDb = 30;
  bool hasClipFloor = false; double clipFloorDb = 0;
  bool manual = false; double manualMinDb = -120, manualMaxDb = -20;
  double specFloor = 0;
  double specPeakScale = 10;
  double smoothingFrames = 5;
  double avgFrames = 0;
  bool spatialSmooth = true;
  bool peakHold = true;
  double wfBrightness = 0;
  double wfContrast = 0;
  double wfSharpness = 0;       // (the processor's copy; only its change matters here — it flushes the range)
};

struct ProcessedFrame {
  const uint8_t* row; const float* spec; const float* peak; int n;
  double dbMin, dbMax;
};

class SignalProcessor {
 public:
  void applySettings(const SignalSettings& s);
  const SignalSettings& settings() const { return s_; }
  ProcessedFrame process(const float* bins, int n, double centerHz, double bwHz, bool interacting, double nowMs);
  void resetPeakHold() { peakOn_ = false; }
  void reset();
  double dbMin() const { return actualMinDb_; }
  double dbMax() const { return actualMaxDb_; }
 private:
  double slewFloor(double want, double now);
  SignalSettings s_;
  std::vector<float> dbAvg_, specSmooth_, peakLine_, tmp_, normRow_, outSpec_, outPeak_;
  std::vector<uint8_t> outRow_;
  bool haveBufs_ = false, peakOn_ = false;
  struct Sample { double value, ts; };
  std::deque<Sample> minHistory_, maxHistory_;
  double lastMaxAt_ = 0, lastMinAt_ = 0;
  bool adoptRange_ = false;
  double lastCenterHz_ = 0, lastBwHz_ = 0;
  bool hasFrozen_ = false; double frozenFloorDb_ = 0;
  double actualMinDb_ = -120, actualMaxDb_ = -20;
  uint32_t dbHist_[300] = {};
  double lastFrameMs_ = 0, prevCenterHz_ = 0;
  int settingsVer_ = 0, settingsVerApp_ = 0;
};

}  // namespace wf
