// wf_golden_test.cpp — replays the golden scenarios (gen_golden.ts, the app's own TS modules) through the C++ port and
// compares every record: processed rows, placed ring rows + motion texels + fan maps, and every display frame's
// uniforms + spectrum trace. Then the palettes. Prints a summary per scenario and the row timing of the irregular feeds.
// Build/run: handheld-ui/test/run_golden.sh
#include "../src/wf/wf_model.h"
#include "../src/wf/wf_palette.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace wf;

struct Reader {
  std::vector<uint8_t> d; size_t p = 0;
  bool open(const std::string& f) { std::ifstream s(f, std::ios::binary); if (!s) return false; d.assign(std::istreambuf_iterator<char>(s), {}); return true; }
  uint8_t u8() { return d[p++]; }
  int32_t i32() { int32_t v; std::memcpy(&v, &d[p], 4); p += 4; return v; }
  double f64() { double v; std::memcpy(&v, &d[p], 8); p += 8; return v; }
  float f32() { float v; std::memcpy(&v, &d[p], 4); p += 4; return v; }
  const uint8_t* bytes(size_t n) { const uint8_t* r = &d[p]; p += n; return r; }
  bool eof() const { return p >= d.size(); }
};

struct Stats {
  long records = 0, fails = 0;
  long bytes = 0, byteOff1 = 0, byteOffMore = 0;
  double maxRel = 0, maxTrace = 0;
  long traceVals = 0;
  std::string firstFail;
};

static bool close(double a, double b, double tol = 1e-9) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (a == b) return true;
  return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

struct Run {
  Reader exp; Stats st; std::string name; long recNo = 0;
  void fail(const std::string& what) {
    st.fails++;
    if (st.firstFail.empty()) { char b[512]; std::snprintf(b, sizeof b, "record %ld: %s", recNo, what.c_str()); st.firstFail = b; }
  }
  void num(const char* what, double a, double b, double tol = 1e-9) {
    const double rel = std::fabs(a - b) / std::max(1.0, std::fabs(b));
    if (std::isfinite(rel) && rel > st.maxRel) st.maxRel = rel;
    if (!close(a, b, tol)) { char m[256]; std::snprintf(m, sizeof m, "%s: C++ %.12g vs TS %.12g", what, a, b); fail(m); }
  }
  void bytes(const char* what, const uint8_t* a, const uint8_t* b, int n) {
    int off1 = 0, offMore = 0;
    for (int i = 0; i < n; i++) { const int d = std::abs((int)a[i] - (int)b[i]); if (d == 1) off1++; else if (d > 1) offMore++; }
    st.bytes += n; st.byteOff1 += off1; st.byteOffMore += offMore;
    if (offMore) { char m[256]; std::snprintf(m, sizeof m, "%s: %d bytes differ by > 1", what, offMore); fail(m); }
  }
  void expectTag(uint8_t want) {
    recNo++; st.records++;
    const uint8_t t = exp.u8();
    if (t != want) { char m[128]; std::snprintf(m, sizeof m, "record tag %c expected, TS has %c", want, t); fail(m); std::fprintf(stderr, "%s: stream out of step — %s\n", name.c_str(), st.firstFail.c_str()); std::exit(2); }
  }
};

static Run* gRun = nullptr;
static void onFrame(void*, const ProcessedFrame& f) {
  Run& R = *gRun;
  R.expectTag('F');
  const int n = R.exp.i32();
  if (n != f.n) { R.fail("frame length"); R.exp.bytes(n); R.exp.f64(); R.exp.f64(); return; }
  const uint8_t* er = R.exp.bytes(n);
  R.bytes("processed row", f.row, er, n);
  const double eMin = R.exp.f64(), eMax = R.exp.f64();
  if (getenv("WF_DEBUG") && R.recNo < 12) {
    std::printf("F rec %ld: C++ [%g, %g] TS [%g, %g]  row[0..7] C++", R.recNo, f.dbMin, f.dbMax, eMin, eMax);
    for (int i = 0; i < 8; i++) std::printf(" %d", f.row[i + n / 2]);
    std::printf(" TS"); for (int i = 0; i < 8; i++) std::printf(" %d", er[i + n / 2]); std::printf("\n");
  }
  R.num("dbMin", f.dbMin, eMin); R.num("dbMax", f.dbMax, eMax);
}
static void onRow(void*, const WaterfallModel& m, int slot) {
  Run& R = *gRun;
  R.expectTag('R');
  const int s = R.exp.i32();
  if (s != slot) R.fail("slot");
  R.num("frames", (double)m.frames(), R.exp.f64());
  R.num("xfA", m.fan().xfA[slot], R.exp.f64()); R.num("xfB", m.fan().xfB[slot], R.exp.f64());
  R.num("easeLeft", m.fan().easeLeft, R.exp.f64());
  R.num("motion lo", m.motion().lo, R.exp.f64()); R.num("motion hi", m.motion().hi, R.exp.f64());
  const int prev = (slot + RING_ROWS - 1) % RING_ROWS;
  uint8_t mot[8]; std::memcpy(mot, &m.motion().tex[4 * slot], 4); std::memcpy(mot + 4, &m.motion().tex[4 * prev], 4);
  const uint8_t* em = R.exp.bytes(8);
  if (std::memcmp(mot, em, 8) != 0) R.fail("motion texels (h, g codes)");
  R.bytes("ring row", m.imageBytes() + (size_t)slot * RING_W, R.exp.bytes(RING_W), RING_W);
}

static Scroll scrollOf(int c) {
  switch (c) { case 0: return Scroll::Min2; case 1: return Scroll::Low; case 2: return Scroll::Sharp; case 3: return Scroll::Eco;
               case 5: return Scroll::Smooth; default: return Scroll::Default; }
}

static bool runScenario(const std::string& dir, const std::string& name) {
  Reader in; Run R; R.name = name; gRun = &R;
  if (!in.open(dir + "/" + name + ".in.bin") || !R.exp.open(dir + "/" + name + ".out.bin")) { std::printf("  %s: MISSING golden files\n", name.c_str()); return false; }
  in.bytes(4);
  WaterfallConfig cfg; cfg.scroll = scrollOf(in.u8()); cfg.displayHz = in.f64();
  WaterfallModel m(cfg);
  m.setTraceWidth(640);
  m.onFrame = onFrame; m.onRow = onRow;
  // row timing (uHead steps per display frame)
  std::map<int, long> stepHist; double lastHead = 0; long ticks = 0; std::vector<double> stepTimes;
  for (;;) {
    const uint8_t t = in.u8();
    if (t == 0) break;
    if (t == 1) {
      const double ts = in.f64(), c = in.f64(), span = in.f64(); const int n = in.i32();
      m.pushFrameU8(in.bytes(n), n, true, c, span, ts);
    } else if (t == 2) { in.f64(); const double c = in.f64(), s = in.f64(); m.setView(c, s); }
    else if (t == 3) { m.noteInteraction(in.f64()); }
    else if (t == 4) {
      const double ts = in.f64(), gap = in.f64();
      m.tick(ts, gap);
      const WfUniforms u = m.uniforms();
      R.expectTag('D');
      R.num("uHeadF", u.headF, R.exp.f64()); R.num("uFrac", u.frac, R.exp.f64(), 1e-7); R.num("uQuant", u.quant, R.exp.f64());
      R.num("uMoLo", u.moLo, R.exp.f64()); R.num("uMoHi", u.moHi, R.exp.f64());
      R.num("uZA", u.zA, R.exp.f64()); R.num("uZB", u.zB, R.exp.f64());
      R.num("clock rate", m.clock().rate, R.exp.f64()); R.num("pace target", m.pace().pTgt, R.exp.f64());
      R.num("avail (taken)", m.availTaken(), R.exp.f64());
      const int nt = R.exp.i32();
      const std::vector<double>& td = m.traceDisp();
      if ((int)td.size() != nt) R.fail("trace length");
      for (int i = 0; i < nt; i++) { const float e = R.exp.f32(); if (i < (int)td.size()) { const double d = std::fabs((float)td[i] - e); R.st.traceVals++; if (d > R.st.maxTrace) R.st.maxTrace = d; } }
      const int np = R.exp.i32();
      const std::vector<double>& pk = m.peakLine();
      if ((int)pk.size() != np) R.fail("peak length");
      for (int i = 0; i < np; i++) { const float e = R.exp.f32(); if (i < (int)pk.size()) { const double d = std::fabs((float)pk[i] - e); if (d > R.st.maxTrace) R.st.maxTrace = d; } }
      // timing
      ticks++;
      if (u.quant > 0.5 && lastHead > 0) { const int step = (int)(u.headF - lastHead); stepHist[step]++; if (step > 0) stepTimes.push_back(ts); }
      lastHead = u.headF;
    }
  }
  R.expectTag('E');
  if (R.st.maxTrace > 1e-5) R.fail("trace values differ > 1e-5");
  std::printf("  %-18s %s  records %ld, ring/processed bytes %ld (±1: %ld, >1: %ld), max rel err %.2g, trace max |d| %.2g (%ld values)\n",
              name.c_str(), R.st.fails ? "FAIL" : "ok  ", R.st.records, R.st.bytes, R.st.byteOff1, R.st.byteOffMore, R.st.maxRel,
              R.st.maxTrace, R.st.traceVals);
  if (R.st.fails) std::printf("      %ld mismatches; first: %s\n", R.st.fails, R.st.firstFail.c_str());
  if (name.rfind("irregular", 0) == 0) {
    // the steady part (after 2 s of warm-up): rows per display frame and the row interval
    std::string h; for (auto& kv : stepHist) { char b[32]; std::snprintf(b, sizeof b, "%d:%ld ", kv.first, kv.second); h += b; }
    double mean = 0, sd = 0; int cnt = 0; std::vector<double> iv;
    for (size_t i = 1; i < stepTimes.size(); i++) if (stepTimes[i] > 3000) iv.push_back(stepTimes[i] - stepTimes[i - 1]);
    for (double v : iv) mean += v; if (!iv.empty()) mean /= iv.size();
    for (double v : iv) sd += (v - mean) * (v - mean); if (!iv.empty()) sd = std::sqrt(sd / iv.size());
    (void)cnt; (void)ticks;
    std::printf("      row timing: rows/display-frame histogram {%s}; row interval %.2f ms ± %.2f (%zu rows, %.1f rows/s)\n",
                h.c_str(), mean, sd, iv.size(), mean > 0 ? 1000 / mean : 0);
  }
  return R.st.fails == 0;
}

static bool runPalettes(const std::string& dir) {
  Reader r; if (!r.open(dir + "/palettes.bin")) { std::printf("  palettes: MISSING\n"); return false; }
  long bad = 0, n = 0;
  const int nb = r.i32();
  for (int i = 0; i < nb; i++) {
    const int len = r.i32(); std::string name((const char*)r.bytes(len), len);
    const Palette p = builtinPalette(paletteByName(name));
    const uint8_t* e = r.bytes(1024);
    if (std::string(paletteName(paletteByName(name))) != name) { bad++; if (getenv("WF_DEBUG")) std::printf("name %s\n", name.c_str()); }
    for (int k = 0; k < 1024; k++) { n++; if (p.rgba[k] != e[k]) bad++; }
  }
  const int ns = r.i32();
  for (int i = 0; i < ns; i++) {
    const int len = r.i32(); std::vector<uint32_t> st; for (int k = 0; k < len; k++) st.push_back((uint32_t)r.i32());
    const Palette p = paletteFromStops(st); const uint8_t* e = r.bytes(1024);
    for (int k = 0; k < 1024; k++) { n++; if (p.rgba[k] != e[k]) { bad++; if (getenv("WF_DEBUG")) std::printf("stops %d byte %d: %d vs %d\n", i, k, p.rgba[k], e[k]); } }
  }
  const int np = r.i32();
  for (int i = 0; i < np; i++) {
    const int len = r.i32(); std::vector<std::pair<int, uint32_t>> st;
    for (int k = 0; k < len; k++) { const int p0 = r.i32(); st.push_back({ p0, (uint32_t)r.i32() }); }
    const Palette p = paletteFromPositions(st); const uint8_t* e = r.bytes(1024);
    for (int k = 0; k < 1024; k++) { n++; if (p.rgba[k] != e[k]) { bad++; if (getenv("WF_DEBUG")) std::printf("pos %d byte %d: %d vs %d\n", i, k, p.rgba[k], e[k]); } }
  }
  std::printf("  %-18s %s  %d built-in + %d stop-list + %d positioned LUTs, %ld bytes compared, %ld differ\n", "palettes",
              bad ? "FAIL" : "ok  ", nb, ns, np, n, bad);
  return bad == 0;
}

// CPU cost: replay a scenario with no hooks, time the data path (pushFrame) and the per-display-frame path (tick).
#include <chrono>
static void bench(const std::string& dir, const std::string& name) {
  Reader in; if (!in.open(dir + "/" + name + ".in.bin")) return;
  in.bytes(4); WaterfallConfig cfg; cfg.scroll = scrollOf(in.u8()); cfg.displayHz = in.f64();
  WaterfallModel m(cfg); m.setTraceWidth(640);
  using C = std::chrono::steady_clock; double tf = 0, tt = 0; long nf = 0, nt = 0, binsN = 0;
  std::vector<int> r, mo;
  for (;;) {
    const uint8_t t = in.u8(); if (t == 0) break;
    if (t == 1) { const double ts = in.f64(), c = in.f64(), s = in.f64(); const int n = in.i32(); const uint8_t* b = in.bytes(n);
                  auto a = C::now(); m.pushFrameU8(b, n, true, c, s, ts); tf += std::chrono::duration<double, std::milli>(C::now() - a).count(); nf++; binsN = n; }
    else if (t == 2) { in.f64(); const double c = in.f64(), s = in.f64(); m.setView(c, s); }
    else if (t == 3) m.noteInteraction(in.f64());
    else { const double ts = in.f64(), gap = in.f64(); auto a = C::now(); m.tick(ts, gap); m.takeDirty(r, mo);
           tt += std::chrono::duration<double, std::milli>(C::now() - a).count(); nt++; }
  }
  std::printf("  cpu %-16s %ld bins: %.3f ms a data frame (processor + clock + rows), %.4f ms a display frame (clock ticks, pace, trace tween)\n",
              name.c_str(), binsN, tf / nf, tt / nt);
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "golden";
  bool ok = true;
  for (const char* s : { "sweep", "zoom", "jump", "irregular_default", "irregular_smooth", "irregular_eco", "irregular_sharp", "picture" })
    ok = runScenario(dir, s) && ok;
  ok = runPalettes(dir) && ok;
  for (const char* s : { "sweep", "irregular_smooth" }) bench(dir, s);
  std::printf(ok ? "  ALL GOLDEN TESTS PASS\n" : "  GOLDEN TESTS FAILED\n");
  return ok ? 0 : 1;
}
