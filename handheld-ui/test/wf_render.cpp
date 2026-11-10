// wf_render.cpp — the PORT's render of a golden scenario, offscreen (EGL surfaceless + GLES 3.0 — Mesa llvmpipe in
// Docker, or Panfrost on the R36S itself): replays <name>.in.bin through wf::Waterfall and, at the requested display
// frames, writes <out>/<name>_<W>x<H>_<t>_port.png/.rgba (the waterfall alone, for compare_png.ts against the app's own
// shader) and <out>/<name>_handheld_<t>.png (the 640×480 layout: spectrum 120 px over a 360 px waterfall).
// Usage: wf_render <goldenDir> <name> <outDir> <W> <H> <palette> <tMs>...
#include "../src/wf/waterfall.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ── a minimal PNG writer (stored deflate blocks — no zlib dependency) ──
static uint32_t crcTab[256];
static uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xffffffffu) {
  if (!crcTab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1; crcTab[i] = k; }
  for (size_t i = 0; i < n; i++) c = crcTab[(c ^ p[i]) & 255] ^ (c >> 8);
  return c;
}
static void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  be32(out, (uint32_t)data.size());
  std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  be32(out, crc(td.data(), td.size()) ^ 0xffffffffu);
}
static void writePng(const std::string& path, const uint8_t* rgba, int w, int h) {
  std::vector<uint8_t> raw; raw.reserve((size_t)(w * 4 + 1) * h);
  for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4); }
  std::vector<uint8_t> z = { 0x78, 0x01 };
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n >= raw.size() ? 1 : 0);
    z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
    z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
  }
  be32(z, (b << 16) | a);
  std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 }, ih;
  be32(ih, w); be32(ih, h); ih.insert(ih.end(), { 8, 6, 0, 0, 0 });
  chunk(png, "IHDR", ih); chunk(png, "IDAT", z); chunk(png, "IEND", {});
  std::ofstream(path, std::ios::binary).write((const char*)png.data(), png.size());
}

static bool eglUp() {
  auto getDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay dpy = getDisplay ? getDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) { std::fprintf(stderr, "EGL: no display\n"); return false; }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
  EGLConfig cfg = (EGLConfig)0; EGLint nc = 0;   // (surfaceless: a pbuffer config, else none — EGL_KHR_no_config_context)
  if (!eglChooseConfig(dpy, ca, &cfg, 1, &nc) || nc < 1) cfg = (EGLConfig)0;
  const EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { std::fprintf(stderr, "EGL: no context\n"); return false; }
  std::printf("  GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  return true;
}

struct Fbo {
  GLuint fb = 0, tex = 0; int w = 0, h = 0;
  void make(int W, int H) {
    w = W; h = H;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  }
  std::vector<uint8_t> read() {   // top-down RGBA
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    std::vector<uint8_t> px((size_t)w * h * 4), out(px.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++) std::memcpy(&out[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    return out;
  }
};

int main(int argc, char** argv) {
  if (argc < 8) { std::fprintf(stderr, "usage: wf_render <goldenDir> <name> <outDir> <W> <H> <palette> <tMs>...\n"); return 2; }
  const std::string dir = argv[1], name = argv[2], out = argv[3], pal = argv[6];
  const int W = std::atoi(argv[4]), H = std::atoi(argv[5]);
  std::vector<double> snaps; for (int i = 7; i < argc; i++) snaps.push_back(std::atof(argv[i]));
  if (!eglUp()) return 1;
  std::ifstream f(dir + "/" + name + ".in.bin", std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
  size_t p = 4;
  auto u8 = [&]() { return d[p++]; };
  auto f64 = [&]() { double v; std::memcpy(&v, &d[p], 8); p += 8; return v; };
  auto i32 = [&]() { int32_t v; std::memcpy(&v, &d[p], 4); p += 4; return v; };
  const wf::Scroll scrolls[6] = { wf::Scroll::Min2, wf::Scroll::Low, wf::Scroll::Sharp, wf::Scroll::Eco, wf::Scroll::Default, wf::Scroll::Smooth };
  wf::WaterfallConfig cfg; cfg.scroll = scrolls[u8()]; cfg.displayHz = f64();
  // Two pipelines on the same feed: the bare waterfall at W×H (compared with the app) and the 640×480 handheld layout.
  wf::Waterfall A, B;
  A.setConfig(cfg); B.setConfig(cfg);
  if (!A.init() || !B.init()) return 1;
  A.setPalette(pal); B.setPalette(pal);
  A.model().setTraceWidth(W); B.model().setTraceWidth(640);
  Fbo fa, fb; fa.make(W, H); fb.make(640, 480);
  size_t next = 0;
  /* ★ WF_DARK=<fromMs>-<toMs>: the screen is off in that window — the bare waterfall (A) is NOT drawn, so neither ticked
   *  nor uploaded, exactly as the main loop while dark; its rows are made all the same. A long window makes more rows
   *  than DIRTY_ALL's threshold, so the first frame after it uploads the whole ring in one go (wf_model.cpp pushRow). */
  double darkFrom = -1, darkTo = -1;
  if (const char* e = std::getenv("WF_DARK")) std::sscanf(e, "%lf-%lf", &darkFrom, &darkTo);
  for (;;) {
    const uint8_t t = u8();
    if (t == 0) break;
    if (t == 1) {
      const double ts = f64(), c = f64(), s = f64(); const int n = i32();
      A.model().pushFrameU8(&d[p], n, true, c, s, ts); B.model().pushFrameU8(&d[p], n, true, c, s, ts); p += n;
    } else if (t == 2) { f64(); const double c = f64(), s = f64(); A.setView(c, s); B.setView(c, s); }
    else if (t == 3) { const double ts = f64(); A.notifyInteraction(ts / 1000); B.notifyInteraction(ts / 1000); }
    else if (t == 5) {   // ★ V12 a full-capture row (SPWB), as the main loop will hand it over
      const double ts = f64(); const int n = i32();
      A.pushWide(&d[p], (size_t)n, ts / 1000); B.pushWide(&d[p], (size_t)n, ts / 1000); p += n;
    }
    else {
      const double ts = f64(); f64();
      // every display frame: the waterfall draws (it advances the clock + the pace), as the main loop will
      glBindFramebuffer(GL_FRAMEBUFFER, fa.fb);
      if (!(ts >= darkFrom && ts < darkTo)) A.draw(0, 0, W, H, W, H, ts / 1000);
      glBindFramebuffer(GL_FRAMEBUFFER, fb.fb);
      glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
      B.draw(0, 120, 640, 360, 640, 480, ts / 1000);
      B.drawSpectrum(0, 0, 640, 120, 640, 480);
      if (next < snaps.size() && ts >= snaps[next]) {
        const std::vector<uint8_t> pa = fa.read(), pb = fb.read();
        char tag[64]; std::snprintf(tag, sizeof tag, "%.0f", snaps[next]);
        const std::string base = out + "/" + name + "_" + std::to_string(W) + "x" + std::to_string(H) + "_" + tag + "_port";
        writePng(base + ".png", pa.data(), W, H);
        std::ofstream(base + ".rgba", std::ios::binary).write((const char*)pa.data(), pa.size());
        writePng(out + "/" + name + "_handheld_" + tag + ".png", pb.data(), 640, 480);
        const wf::WfUniforms u = A.model().uniforms();
        std::printf("  port render %s.png  (uHead %.0f, quant %.0f, motion %.0f..%.0f)\n", base.c_str(), u.headF, u.quant, u.moLo, u.moHi);
        next++;
      }
    }
  }
  glFinish();
  return 0;
}
