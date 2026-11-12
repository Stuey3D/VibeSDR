// wf_gl.cpp — WF_SKSL (src/components/WaterfallView.tsx) in GLSL ES 3.00, line for line, and the spectrum trace.
// Differences from the SkSL, all mechanical:
//   • wf.eval(float2(x, r)) on a Nearest/Decal image → texelFetch of texel (floor x, floor r); the callers already clamp
//     x into the image, exactly as the SkSL does.
//   • the four motion planes the app packs under the ring image are a separate 1024×1 RGBA8 texture (hHi, hLo, gHi, gLo
//     per slot) — the same 16-bit codes, read and decoded with the same arithmetic.
//   • xy = the pixel centre, top-down, inside the draw rect (Skia's local coordinates).
//   • uHeadF / uMoLo / uMoHi are rebased by a multiple of uRows on the CPU so float32 stays exact after days of rows; every
//     difference and mod(…, uRows) the shader takes is unchanged.
#include "wf_gl.h"
#include <cmath>
#include <cstdio>

namespace wf {

static const char* WF_VS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

static const char* WF_FS = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D wf;      // the ring: R8, uTexW × uRows, ring order
uniform highp sampler2D motion;  // per slot (hHi, hLo, gHi, gLo)
uniform highp sampler2D lut;     // 256 × 1 RGBA
uniform float uHeadF;
uniform float uFrac;
uniform float uN;
uniform float uQuant;
uniform float uRows;
uniform float uVisibleRows;
uniform float uTexW;
uniform float uDrawW;
uniform float uDrawH;
uniform float uSharp;
uniform float uContrast;
uniform float uMoLo;
uniform float uMoHi;
uniform float uRowPx;
uniform float uColPx;
uniform float uZA;
uniform float uZB;
uniform vec2 uOrigin;   // GL window coords of the rect's top-left corner (x, y of its top edge)
out vec4 fragColor;

float ev(float x, float r) { return texelFetch(wf, ivec2(int(floor(x)), int(floor(r))), 0).r; }
float px(float x, float r) {
  if (x < 0.0 || x > uTexW) { return 0.0; }
  return ev(clamp(x, 0.5, uTexW - 0.5), r);
}

void main() {
  vec2 xy = vec2(gl_FragCoord.x - uOrigin.x, uOrigin.y - gl_FragCoord.y);
  float Lc = xy.y / uDrawH * uVisibleRows;
  float L  = uQuant > 0.5 ? floor(Lc) : Lc;
  float R  = uFrac * uN;
  if (uQuant > 0.5) { R = floor(R + 0.0001); }
  float A  = (uHeadF - 2.0) * uN + R - L;
  float fI = floor(A / uN);
  float t  = A / uN - fI;
  if (uQuant > 0.5) { t = step(0.5, t); }
  float r1 = mod(fI, uRows) + 0.5;
  float r2 = mod(fI + 1.0, uRows) + 0.5;
  float tx = (uZA * (xy.x / uDrawW) + uZB) * uTexW;
  float h = 0.0;
  float g = 1.0;
  if (fI >= uMoLo && fI <= uMoHi && (uQuant < 0.5 || t == 0.0)) {
    float ms = mod(fI, uRows);
    vec4 m = texelFetch(motion, ivec2(int(floor(ms)), 0), 0);
    h = (floor(m.r * 255.0 + 0.5) * 256.0 + floor(m.g * 255.0 + 0.5) - 32768.0) / 64.0;
    g = exp2((floor(m.b * 255.0 + 0.5) * 256.0 + floor(m.a * 255.0 + 0.5) - 32768.0) / 4096.0);
  }
  float mid = uTexW * 0.5;
  float c, l, r;
  if (tx < 0.0 || tx > uTexW) {
    c = 0.0;
  } else if (h != 0.0 || g != 1.0) {
    float tc = uQuant > 0.5 ? 1.0 - (Lc - L) : t;
    float xa0 = mid + ((tx - mid) - tc * h) / (1.0 + tc * (g - 1.0));
    float mv = (g - 1.0) * (xa0 - mid) + h;
    float K  = clamp(ceil(abs(mv) * uRowPx * uColPx), 1.0, 6.0);
    float acc = 0.0;
    float mx  = 0.0;
    for (int k = 0; k < 6; k++) {
      if (float(k) < K) {
        float tk = clamp(tc + ((float(k) + 0.5) / K - 0.5) * uRowPx, 0.0, 1.0);
        float xa = mid + ((tx - mid) - tk * h) / (1.0 + tk * (g - 1.0));
        float xb = mid + g * (xa - mid) + h;
        bool ina = xa >= 0.0 && xa <= uTexW;
        bool inb = xb >= 0.0 && xb <= uTexW;
        float v = ina ? px(xa, r1) : px(xb, r2);
        if (uQuant < 0.5 && ina && inb) { v = mix(v, px(xb, r2), tk); }
        acc += v;
        mx = max(mx, v);
      }
    }
    float mean = acc / K;
    c = mix(mean, mx, smoothstep(0.03, 0.10, mx - mean));
    if (uSharp > 0.0 && K < 1.5) {
      float xa = xa0;
      float xb = mid + g * (xa - mid) + h;
      l = px(xa - 1.0, r1);
      r = px(xa + 1.0, r1);
      if (uQuant < 0.5) { l = mix(l, px(xb - 1.0, r2), tc); r = mix(r, px(xb + 1.0, r2), tc); }
      c = c + uSharp * (c - (l + r) * 0.5);
    }
  } else {
    tx = clamp(tx, 0.5, uTexW - 0.5);
    float xl = max(tx - 1.0, 0.5);
    float xr = min(tx + 1.0, uTexW - 0.5);
    if (t == 0.0 || t == 1.0) {
      float rr = t == 0.0 ? r1 : r2;
      c = ev(tx, rr);
      if (uSharp > 0.0) {
        l = ev(xl, rr);
        r = ev(xr, rr);
        c = c + uSharp * (c - (l + r) * 0.5);
      }
    } else {
      c = mix(ev(tx, r1), ev(tx, r2), t);
      if (uSharp > 0.0) {
        l = mix(ev(xl, r1), ev(xl, r2), t);
        r = mix(ev(xr, r1), ev(xr, r2), t);
        c = c + uSharp * (c - (l + r) * 0.5);
      }
    }
  }
  if (fI + uRows < uHeadF + 8.0) { c = 0.0; }
  float raw = clamp(c, 0.0, 1.0);
  float s   = raw * raw * (3.0 - 2.0 * raw);
  float v   = uContrast > 0.0
    ? mix(raw, s, uContrast)
    : mix(raw, raw * 0.5 + 0.25, -uContrast);
  fragColor = texelFetch(lut, ivec2(min(255, int(floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5))), 0), 0);
}
)";

static GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "wf shader: %s\n", log); glDeleteShader(s); return 0; }
  return s;
}
static GLuint link(const char* vs, const char* fs) {
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
  glDeleteShader(v); glDeleteShader(f);
  GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "wf link: %s\n", log); glDeleteProgram(p); return 0; }
  return p;
}
static GLuint makeTex(GLenum ifmt, GLenum fmt, int w, int h, const void* data) {
  GLuint t; glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

bool WaterfallGL::init() {
  prog_ = link(WF_VS, WF_FS);
  if (!prog_) return false;
#define U(n) n##_ = glGetUniformLocation(prog_, #n)
  U(uHeadF); U(uFrac); U(uN); U(uQuant); U(uRows); U(uVisibleRows); U(uTexW); U(uDrawW); U(uDrawH); U(uSharp); U(uContrast);
  U(uMoLo); U(uMoHi); U(uRowPx); U(uColPx); U(uZA); U(uZB); U(uOrigin);
#undef U
  glUseProgram(prog_);
  glUniform1i(glGetUniformLocation(prog_, "wf"), 0);
  glUniform1i(glGetUniformLocation(prog_, "motion"), 1);
  glUniform1i(glGetUniformLocation(prog_, "lut"), 2);
  std::vector<uint8_t> zero((size_t)RING_W * RING_ROWS, 0);
  texRing_ = makeTex(GL_R8, GL_RED, RING_W, RING_ROWS, zero.data());
  std::vector<uint8_t> mz(4 * (size_t)RING_ROWS);
  for (int i = 0; i < RING_ROWS; i++) { mz[4 * i] = 128; mz[4 * i + 1] = 0; mz[4 * i + 2] = 128; mz[4 * i + 3] = 0; }
  texMotion_ = makeTex(GL_RGBA8, GL_RGBA, RING_ROWS, 1, mz.data());
  const Palette p = builtinPalette(paletteByName("Jet"));
  texLut_ = makeTex(GL_RGBA8, GL_RGBA, 256, 1, p.rgba);
  const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
  glGenVertexArrays(1, &vao_); glBindVertexArray(vao_);
  glGenBuffers(1, &vbo_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glBindVertexArray(0);
  return true;
}
void WaterfallGL::destroy() {
  if (prog_) glDeleteProgram(prog_);
  GLuint t[3] = { texRing_, texMotion_, texLut_ }; glDeleteTextures(3, t);
  glDeleteBuffers(1, &vbo_); glDeleteVertexArrays(1, &vao_);
  prog_ = 0;
}
void WaterfallGL::setPalette(const Palette& p) {
  glBindTexture(GL_TEXTURE_2D, texLut_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, p.rgba);
}
void WaterfallGL::sync(WaterfallModel& m) {
  m.takeDirty(rows_, mot_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (!rows_.empty()) {
    glBindTexture(GL_TEXTURE_2D, texRing_);
    const uint8_t* img = m.imageBytes();
    if (rows_.size() == 1 && rows_[0] == WaterfallModel::DIRTY_ALL)   // the whole ring, once (a dark screen's backlog)
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, RING_W, RING_ROWS, GL_RED, GL_UNSIGNED_BYTE, img);
    else
      for (int s : rows_) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, s, RING_W, 1, GL_RED, GL_UNSIGNED_BYTE, img + (size_t)s * RING_W);
  }
  if (!mot_.empty()) {   // 4 KB: one upload of the whole motion row
    glBindTexture(GL_TEXTURE_2D, texMotion_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, RING_ROWS, 1, GL_RGBA, GL_UNSIGNED_BYTE, m.motion().tex.data());
  }
}
void WaterfallGL::draw(const WaterfallModel& m, int x, int y, int w, int h, int fbW, int fbH, int rowPx) {
  (void)fbW;
  const WfUniforms u = m.uniforms();
  // WaterfallView: rows = clamp(round(wfH), 64, 1024); wfRenderH = wfH + ceil(wfH / rows) + 2 (the hidden overscan row);
  // shaderRows = wfRenderH × dpr / devRowPx; uRowPx = 1 / devRowPx; uColPx = width × dpr / RING_W. (dpr = 1 here.)
  const double rows = std::min(1024.0, std::max(64.0, std::floor(h + 0.5)));
  const double drawH = h + std::ceil(h / rows) + 2;
  const double visRows = drawH / rowPx;
  // float32-safe head: rebase by a multiple of the ring (every difference and mod the shader takes is unchanged)
  double base = std::floor((u.headF - RING_ROWS) / RING_ROWS) * RING_ROWS;
  if (base < 0) base = 0;
  const bool none = u.moLo > u.moHi;
  glViewport(x, fbH - y - h, w, h);
  glUseProgram(prog_);
  glUniform1f(uHeadF_, (float)(u.headF - base)); glUniform1f(uFrac_, (float)u.frac); glUniform1f(uN_, 1.0f);
  glUniform1f(uQuant_, (float)u.quant); glUniform1f(uRows_, (float)RING_ROWS); glUniform1f(uVisibleRows_, (float)visRows);
  glUniform1f(uTexW_, (float)RING_W); glUniform1f(uDrawW_, (float)w); glUniform1f(uDrawH_, (float)drawH);
  glUniform1f(uSharp_, (float)u.sharp); glUniform1f(uContrast_, (float)u.contrast);
  glUniform1f(uMoLo_, none ? 1.0f : (float)(u.moLo - base)); glUniform1f(uMoHi_, none ? 0.0f : (float)(u.moHi - base));
  glUniform1f(uRowPx_, (float)(1.0 / rowPx)); glUniform1f(uColPx_, (float)((double)w / RING_W));
  glUniform1f(uZA_, (float)u.zA); glUniform1f(uZB_, (float)u.zB);
  glUniform2f(uOrigin_, (float)x, (float)(fbH - y));
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texRing_);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texMotion_);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, texLut_);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
}

// ── the spectrum trace ──
// Fill: WaterfallView's path (0, baseline) → a point every SPEC_PX_STEP px at baseline − v·h → (w, baseline), filled with
// a LinearGradient of the palette's 9 stops from the trace's top to the baseline. Peak: the peak-hold polyline, the needle
// colour at 0.85, softened as the app's MaskFilter blur (σ 2 px) does — drawn here as a ±6 px band with a Gaussian falloff.
static const char* SP_VS = R"(#version 300 es
layout(location = 0) in vec3 aPos;   // x, y in rect px (top-down), d = signed distance (peak band)
uniform vec4 uRect;                  // x, y, w, h of the rect in window px (top-down)
uniform vec2 uFb;                    // framebuffer size
out float vY; out float vD;
void main() {
  vec2 p = vec2(uRect.x + aPos.x, uRect.y + aPos.y);
  gl_Position = vec4(p.x / uFb.x * 2.0 - 1.0, 1.0 - p.y / uFb.y * 2.0, 0.0, 1.0);
  vY = aPos.y / uRect.w; vD = aPos.z;
}
)";
static const char* SP_FILL_FS = R"(#version 300 es
precision highp float;
uniform vec3 uStops[9];
in float vY; in float vD;
out vec4 fragColor;
void main() {
  float t = clamp(vY, 0.0, 1.0) * 8.0;
  int i = int(min(floor(t), 7.0));
  fragColor = vec4(mix(uStops[i], uStops[i + 1], t - float(i)), 1.0);
}
)";
static const char* SP_PEAK_FS = R"(#version 300 es
precision highp float;
uniform vec4 uCol;
in float vY; in float vD;
out vec4 fragColor;
void main() {
  // a 1 px line blurred by a Gaussian of σ = 2 px: coverage ≈ exp(−d²/2σ²) / (σ√2π)
  float a = uCol.a * exp(-vD * vD / 8.0) / 5.0133;
  fragColor = vec4(uCol.rgb, a);
}
)";

bool SpectrumGL::init() {
  progFill_ = link(SP_VS, SP_FILL_FS);
  progPeak_ = link(SP_VS, SP_PEAK_FS);
  if (!progFill_ || !progPeak_) return false;
  fRect_ = glGetUniformLocation(progFill_, "uRect"); fFb_ = glGetUniformLocation(progFill_, "uFb");
  fStops_ = glGetUniformLocation(progFill_, "uStops");
  pRect_ = glGetUniformLocation(progPeak_, "uRect"); pFb_ = glGetUniformLocation(progPeak_, "uFb");
  pCol_ = glGetUniformLocation(progPeak_, "uCol");
  glGenVertexArrays(1, &vao_); glBindVertexArray(vao_);
  glGenBuffers(1, &vbo_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glBindVertexArray(0);
  setPalette(builtinPalette(paletteByName("Jet")));
  return true;
}
void SpectrumGL::destroy() {
  if (progFill_) glDeleteProgram(progFill_);
  if (progPeak_) glDeleteProgram(progPeak_);
  glDeleteBuffers(1, &vbo_); glDeleteVertexArrays(1, &vao_);
  progFill_ = progPeak_ = 0;
}
void SpectrumGL::setPalette(const Palette& p) {
  uint8_t s[9][3]; spectrumGradient(p, s);
  for (int i = 0; i < 9; i++) for (int c = 0; c < 3; c++) stops_[i * 3 + c] = s[i][c] / 255.0f;
}
void SpectrumGL::draw(WaterfallModel& m, int x, int y, int w, int h, int fbW, int fbH) {
  m.setTraceWidth(w);
  const std::vector<double>& disp = m.traceDisp();
  if (disp.empty() || h <= 0) return;
  const int n = (int)disp.size();
  const double baseline = h, sh = h;
  const int STEP = 2;
  const int nDec = (int)std::ceil((double)w / STEP);
  verts_.clear();
  auto put = [&](double X, double Y, double D) { verts_.push_back((float)X); verts_.push_back((float)Y); verts_.push_back((float)D); };
  put(0, baseline, 0); put(0, baseline, 0);
  for (int p = 0; p < w; p += STEP) {
    const double v = disp[n == nDec ? p / STEP : (int)std::floor(((double)p / w) * n)];
    put(p, baseline, 0); put(p, baseline - v * sh, 0);
  }
  put(w, baseline, 0); put(w, baseline, 0);
  glViewport(0, 0, fbW, fbH);
  glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, verts_.size() * sizeof(float), verts_.data(), GL_STREAM_DRAW);
  glUseProgram(progFill_);
  glUniform4f(fRect_, (float)x, (float)y, (float)w, (float)h); glUniform2f(fFb_, (float)fbW, (float)fbH);
  glUniform3fv(fStops_, 9, stops_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, (GLsizei)(verts_.size() / 3));
  const std::vector<double>& pk = m.peakLine();
  if (!pk.empty()) {
    verts_.clear();
    const double R = 6;
    for (size_t k = 0; k < pk.size(); k++) {
      const double X = (double)(k * STEP), Y = baseline - pk[k] * sh;
      put(X, Y - R, -R); put(X, Y + R, R);
    }
    glBufferData(GL_ARRAY_BUFFER, verts_.size() * sizeof(float), verts_.data(), GL_STREAM_DRAW);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(progPeak_);
    glUniform4f(pRect_, (float)x, (float)y, (float)w, (float)h); glUniform2f(pFb_, (float)fbW, (float)fbH);
    glUniform4f(pCol_, ((peakRgb_ >> 16) & 255) / 255.f, ((peakRgb_ >> 8) & 255) / 255.f, (peakRgb_ & 255) / 255.f, 0.85f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, (GLsizei)(verts_.size() / 3));
    glDisable(GL_BLEND);
  }
  glBindVertexArray(0);
}

}  // namespace wf
