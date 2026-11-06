// wf_gl.h — the GLES 3.0 side: the app's waterfall shader (WaterfallView.tsx WF_SKSL) ported to GLSL ES 3.00, the ring /
// motion / LUT textures it reads, and the spectrum trace (WaterfallView's gradient-filled path + soft peak line).
// Per data row: ONE glTexSubImage2D of 1024 bytes (the ring row) + 4 KB of motion texels. Per display frame: one quad,
// no CPU pixel work. Needs a current GLES 3.0 context.
#pragma once
#include "wf_model.h"
#include "wf_palette.h"
#include <GLES3/gl3.h>

namespace wf {

class WaterfallGL {
 public:
  bool init();
  void destroy();
  void setPalette(const Palette& p);
  /** Upload what changed since the last call (rows, motion texels). Cheap; call before draw. */
  void sync(WaterfallModel& m);
  /** Draw the waterfall into the rect (x, y top-left, w × h px) of a fbW × fbH framebuffer. `rowPx` = device px per ring
   *  row (the app's devRowPx: round(dpr) — 1 on the handheld). */
  void draw(const WaterfallModel& m, int x, int y, int w, int h, int fbW, int fbH, int rowPx = 1);
 private:
  GLuint prog_ = 0, vao_ = 0, vbo_ = 0, texRing_ = 0, texMotion_ = 0, texLut_ = 0;
  GLint uHeadF_, uFrac_, uN_, uQuant_, uRows_, uVisibleRows_, uTexW_, uDrawW_, uDrawH_, uSharp_, uContrast_, uMoLo_, uMoHi_,
        uRowPx_, uColPx_, uZA_, uZB_, uOrigin_;
  std::vector<int> rows_, mot_;
};

class SpectrumGL {
 public:
  bool init();
  void destroy();
  void setPalette(const Palette& p);   // the fill's 9-stop gradient comes from the waterfall palette
  void setPeakColour(uint32_t rgb) { peakRgb_ = rgb; }   // the VFO needle colour (app default #ff2020)
  /** Draw the trace into (x, y, w, h): baseline at the bottom (the waterfall's top), the gradient top → bottom. */
  void draw(WaterfallModel& m, int x, int y, int w, int h, int fbW, int fbH);
 private:
  GLuint progFill_ = 0, progPeak_ = 0, vao_ = 0, vbo_ = 0;
  GLint fRect_, fStops_, fFb_, pRect_, pCol_, pFb_;
  float stops_[9 * 3] = {};
  uint32_t peakRgb_ = 0xff2020;
  std::vector<float> verts_;
};

}  // namespace wf
