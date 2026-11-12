// waterfall.h — the handheld's waterfall + spectrum, one object: the ported app pipeline (wf_model.h) + its GLES 3.0
// drawing (wf_gl.h). This is the API the main loop uses; see README.md.
//
//   wf::Waterfall w;  w.init();                        // with a GLES 3.0 context current
//   on each SPEC frame:  w.pushFrame(binsU8, n, /*fftOrder*/true, frameCentreHz, totalBandwidthHz, tSec);
//   on each SPWB frame:  w.pushWide(frameBytes, len, tSec);   // (V12 full-capture row, `&wide=1` — wf_wide.h)
//   when the UI tunes/zooms: w.setView(centreHz, spanHz); w.notifyInteraction(tSec);
//   every display frame:  w.draw(x, y, wW, wH, fbW, fbH, nowSec);  w.drawSpectrum(x, ys, wW, hS, fbW, fbH);
#pragma once
#include "wf_gl.h"
#include "wf_model.h"
#include "wf_palette.h"
#include <string>

namespace wf {

class Waterfall {
 public:
  bool init() { return gl_.init() && spec_.init(); }
  void destroy() { gl_.destroy(); spec_.destroy(); }

  // ── data in ──
  /** An engine SPEC frame: u8 bins (dBFS = u8 − 256, 0 = no data), `fftOrder` = DC first (rotated by N/2), the frame's
   *  view centre and span (config.totalBandwidth), arrival time in seconds (monotonic). */
  void pushFrame(const uint8_t* bins, int n, bool fftOrder, double centreHz, double spanHz, double tSec) {
    model_.pushFrameU8(bins, n, fftOrder, centreHz, spanHz, tSec * 1000);
  }
  void pushFrameDb(const float* db, int n, double centreHz, double spanHz, double tSec) {
    model_.pushFrameDb(db, n, centreHz, spanHz, tSec * 1000);
  }
  /** ★ V12 a full-capture row: an SPWB v2 binary frame from /ws/user-spectrum (asked with `&wide=1`), the bytes exactly
   *  as received, its arrival time in seconds (the same clock as pushFrame). It is the layer under the history that the
   *  edges of an eased row (a tune glide, a zoom-in) are filled from. False = not an SPWB v2 frame (ignored). */
  bool pushWide(const uint8_t* frame, size_t len, double tSec) { return model_.pushWide(frame, len, tSec * 1000); }
  /** True for an SPWB frame (by its magic) — test this BEFORE treating a binary frame as SPEC. */
  static bool isWideFrame(const uint8_t* frame, size_t len) { return wf::isWideFrame(frame, len); }
  /** The view the UI now shows (after a tune or zoom, before the server's frames catch up) — rows are placed against it. */
  void setView(double centreHz, double spanHz) { model_.setView(centreHz, spanHz); }
  /** A tune/zoom input happened now (the gesture mode + the interaction boost run for a short tail after it). */
  void notifyInteraction(double tSec) { model_.noteInteraction(tSec * 1000); }

  // ── look ──
  void setPalette(int id) { pal_ = builtinPalette(id); gl_.setPalette(pal_); spec_.setPalette(pal_); }
  void setPalette(const std::string& name) { setPalette(paletteByName(name)); }
  void setCustomPalette(const Palette& p) { pal_ = p; gl_.setPalette(pal_); spec_.setPalette(pal_); }
  void setPeakColour(uint32_t rgb) { spec_.setPeakColour(rgb); }
  /** Display settings, the app's ranges: contrast −10..10, sharpness 0..10, brightness −20..20 dB, auto contrast 0..20. */
  void setContrast(double v) { WaterfallConfig c = model_.config(); c.wfContrast = v; model_.setConfig(c); }
  void setSharpness(double v) { WaterfallConfig c = model_.config(); c.wfSharpness = v; model_.setConfig(c); }
  void setBrightness(double v) { WaterfallConfig c = model_.config(); c.sig.wfBrightness = v; model_.setConfig(c); }
  void setAutoContrast(double v) { WaterfallConfig c = model_.config(); c.sig.autoContrast = v; model_.setConfig(c); }
  void setSpeed(Scroll s) { WaterfallConfig c = model_.config(); c.scroll = s; model_.setConfig(c); }
  /** The speaker's playout delay, ms (< 0 = no speaker) — the rows wait for it (WaterfallModel::setAudioDelayMs). */
  void setAudioDelayMs(double ms, double nowSec) { model_.setAudioDelayMs(ms, nowSec * 1000); }
  void setConfig(const WaterfallConfig& c) { model_.setConfig(c); }
  const WaterfallConfig& config() const { return model_.config(); }

  // ── drawing (once per display frame, the waterfall first: it advances the clock and the pace) ──
  void draw(int x, int y, int w, int h, int fbW, int fbH, double nowSec, int rowPx = 1) {
    const double now = nowSec * 1000;
    const double gap = lastNow_ > 0 ? now - lastNow_ : 16;
    lastNow_ = now;
    model_.tick(now, gap);
    gl_.sync(model_);
    gl_.draw(model_, x, y, w, h, fbW, fbH, rowPx);
  }
  /** The same frame drawn again (a second scissored strip of it) — GL only: the model and its pace are NOT ticked again. */
  void drawAgain(int x, int y, int w, int h, int fbW, int fbH, int rowPx = 1) { gl_.draw(model_, x, y, w, h, fbW, fbH, rowPx); }
  void drawSpectrum(int x, int y, int w, int h, int fbW, int fbH) { spec_.draw(model_, x, y, w, h, fbW, fbH); }

  WaterfallModel& model() { return model_; }
  double dbMin() const { return model_.dbMin(); }
  double dbMax() const { return model_.dbMax(); }

 private:
  WaterfallModel model_;
  WaterfallGL gl_;
  SpectrumGL spec_;
  Palette pal_ = builtinPalette(paletteByName("Jet"));
  double lastNow_ = 0;
};

}  // namespace wf
