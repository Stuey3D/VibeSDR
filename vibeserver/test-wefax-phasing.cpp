// ★★★ WEFAX PHASING SURVIVES THE LINE'S SEAM AND A FEW BAD LINES (2026-10-07).
//
// Stuart's JMH off a Japanese Kiwi: the start tone was caught, a new chart began — and the phasing was thrown away, on
// one chart and not the next. Two faults in one check (wefax_decoder.cpp decodeFaxLine):
//   1. The pulse position WRAPS at the line's end. JMH's pulse sat at 98 % of the decoder's line, read 97–99 % on some
//      lines and 0–2 % on others, and the plain 10–90 % spread called that 51 %. Where the pulse lands is wherever the
//      decoder's line happened to start — so it failed on some charts and not others.
//   2. Real air fades: 27 of JMH's 38 phasing lines agreed within 1 %, 11 were noise, and a 10–90 % spread counts the 11.
// Now: the ±2 % window on the circle holding the most pulses; phased if at least a third of the lines agree.
//
// ★ Fax audio is SYNTHESISED here (FM, black 1500 Hz / white 2300 Hz, 120 lpm): a 300 Hz start tone, 30 s of phasing
// (black lines, a white pulse), then a white chart with one black bar a known distance after the pulse. A phased
// chart draws that bar where it belongs; an unphased one draws it wherever the decoder's line started.
// ★ DECODER ARTEFACT, FOUND 2026-11-15: about one phasing line in seven misread the pulse (59 %, 3.7 %, 46.6 % against
//   52.5 %) — 28 of 38 agreed at best — and some alignments misread EVERY line. The pulse search ran on raw demod
//   samples, which saturate in a ripple from the FIR's passed mixer image; it now runs on ~47-sample bin averages
//   (wefax_decoder.cpp pulseFromBins) and clean phasing agrees 38 of 38.
// ★ SILENT: audio is generated and decoded, never played.
#include "decoders/wefax_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using vibe::WefaxDecoder;

static int fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) fails++;
}

static const int FS = 48000, SPL = FS / 2;          // 120 lpm: 0.5 s a line
static const double PULSE_W = 0.05, BAR_AT = 0.30, BAR_W = 0.04;

struct Synth {
    std::vector<int16_t> out; double ph = 0; std::mt19937 rng{1};
    void tone(double v01) {   // one sample at grey level v (0 black … 1 white)
        ph += 2 * M_PI * (1500.0 + 800.0 * v01) / FS;
        out.push_back((int16_t)std::lrint(12000.0 * std::sin(ph)));
    }
    void noise(int n) { std::uniform_real_distribution<double> u(0, 1); for (int i = 0; i < n; i++) tone(u(rng)); }
};

struct Result { bool phased = false; bool sawVerdict = false; int barCol = -1; int starts = 0; std::string diag; };

// pulseStart: where the pulse begins in a transmitted line (fraction); lead: noise before the start tone (samples) —
// that is what decides where the decoder's own line boundary falls against the pulse.
static Result run(double pulseStart, int lead, int badPhasingLines, unsigned seed, std::vector<int> garbleToneLines = {},
                  int toneLines = 10) {
    Synth s; s.rng.seed(seed);
    s.noise(lead);
    for (int l = 0; l < toneLines; l++) {                                                 // start tone: 300 Hz black/white (10 lines = 5 s)
        if (std::find(garbleToneLines.begin(), garbleToneLines.end(), l) != garbleToneLines.end()) { s.noise(SPL); continue; }
        for (int i = 0; i < SPL; i++) { int k = l * SPL + i; s.tone(((k * 300 * 2) / FS) & 1 ? 1.0 : 0.0); }
    }
    std::vector<int> bad(60, 0);
    { std::mt19937 r(seed * 7 + 1); std::vector<int> idx(60); for (int i = 0; i < 60; i++) idx[i] = i;
      std::shuffle(idx.begin(), idx.end(), r); for (int i = 0; i < badPhasingLines; i++) bad[idx[i]] = 1; }
    for (int l = 0; l < 60; l++) {                                                        // 30 s of phasing
        if (bad[l]) { s.noise(SPL); continue; }
        for (int i = 0; i < SPL; i++) {
            double x = (double)i / SPL, d = x - pulseStart; if (d < 0) d += 1;
            s.tone(d < PULSE_W ? 1.0 : 0.0);
        }
    }
    const double pc = pulseStart + PULSE_W / 2;                                           // the pulse's centre
    for (int l = 0; l < 300; l++)                                                         // the chart: white, one bar
        for (int i = 0; i < SPL; i++) {
            double x = (double)i / SPL, d = x - (pc + BAR_AT); d -= std::floor(d);
            s.tone(d < BAR_W ? 0.0 : 1.0);
        }

    WefaxDecoder::Config cfg; cfg.lpm = 120; cfg.usePhasing = true; cfg.autoStart = false; cfg.autoStop = false;
    WefaxDecoder dec(FS, cfg);
    Result r;
    std::vector<double> colSum(dec.width(), 0); int lines = 0;
    dec.onDiag = [&](const std::string& m) { if (getenv("WEFAX_TEST_VERBOSE")) std::printf("      %s\n", m.c_str()); if (m.rfind("phasing:", 0) == 0) { r.diag = m; r.sawVerdict = true; r.phased = m.find("used") != std::string::npos; } };
    dec.onStart = [&]() { r.starts++; };
    dec.onLine = [&](uint32_t ln, uint32_t w, const uint8_t* px) {
        if (ln < 100) return;                                                             // past the phasing, settled
        for (uint32_t x = 0; x < w; x++) colSum[x] += px[x];
        lines++;
    };
    for (size_t i = 0; i < s.out.size(); i += 4096) dec.process(&s.out[i], (int)std::min<size_t>(4096, s.out.size() - i));
    // the bar = the darkest BAR_W-wide window of column averages (circular) — immune to the auto-level's speckle
    const int W = dec.width(), bw = (int)std::lround(BAR_W * W);
    double best = 1e18;
    for (int x = 0; x < W && lines > 0; x++) {
        double sum = 0; for (int k = 0; k < bw; k++) sum += colSum[(x + k) % W];
        if (sum < best) { best = sum; r.barCol = x; }
    }
    return r;
}

// ★★ THE STATUS DOES NOT FLICKER (2026-10-07): "standing by" / "receiving" switched 80× through 50 min of NMF off K3FEF
// and 149× through a noisy JMH chart — one correlation threshold, a weak chart hovering on it. Now two (CORR_ON/OFF).
// A chart of random strokes (lines that look like the line above, as a real chart's do) under heavy noise, and pure
// noise: counts how often the status changes between 0 standing by and 3 receiving.
static int flips(bool chart, double noiseAmp, int lines, unsigned seed, int* lastPhase) {
    Synth s; s.rng.seed(seed); std::normal_distribution<double> g(0, 1); std::mt19937 r2(seed + 3);
    std::vector<double> row(SPL, 1.0);
    for (int l = 0; l < lines; l++) {
        if (chart && l % 6 == 0) {   // a new pattern of dark strokes every few lines, held between (coasts, isobars)
            std::fill(row.begin(), row.end(), 1.0);
            std::uniform_int_distribution<int> at(0, SPL - 400);
            for (int k = 0; k < 30; k++) { int a = at(r2); for (int i = 0; i < 300; i++) row[a + i] = 0.0; }
        }
        for (int i = 0; i < SPL; i++) {
            double v = chart ? row[i] : 0.5;
            v += noiseAmp * g(s.rng); v = v < 0 ? 0 : (v > 1 ? 1 : v);
            s.tone(v);
        }
    }
    WefaxDecoder::Config cfg; cfg.lpm = 120; cfg.usePhasing = true; cfg.autoStart = false; cfg.autoStop = false;
    WefaxDecoder dec(FS, cfg);
    int n = 0, prev = -1;
    dec.onPhase = [&](int p) { if ((p == 0 || p == 3) && prev >= 0 && p != prev) n++; if (p == 0 || p == 3) prev = p; };
    for (size_t i = 0; i < s.out.size(); i += 4096) dec.process(&s.out[i], (int)std::min<size_t>(4096, s.out.size() - i));
    *lastPhase = prev;
    return n;
}


// ★★★ A NEW CHART WITHOUT ITS START TONE (2026-11-15, Stuart's R36S: a stop tone, the next start tone lost in a fade, its
// phasing heard — and the new chart was drawn onto the end of the old one, no "image start", line numbers running on).
// Transmissions are built from parts; the decoder runs with the clients' options; every START and every line number is
// recorded, so a test can say how many pictures a client would draw and where each began.
struct Tx {
    Synth s; std::mt19937 r2;
    explicit Tx(unsigned seed) : r2(seed + 5) { s.rng.seed(seed); }
    void startTone(int lines = 10) { for (int k = 0; k < lines * SPL; k++) s.tone(((k * 300 * 2) / FS) & 1 ? 1.0 : 0.0); }
    void stopTone() { for (int k = 0; k < 10 * SPL; k++) s.tone(((k * 450 * 2) / FS) & 1 ? 1.0 : 0.0); for (int k = 0; k < 10 * SPL; k++) s.tone(0.0); }
    void phasing(int lines, double pulseStart = 0.40) {
        for (int l = 0; l < lines; l++) for (int i = 0; i < SPL; i++) {
            double x = (double)i / SPL, d = x - pulseStart; if (d < 0) d += 1;
            s.tone(d < PULSE_W ? 1.0 : 0.0);
        }
    }
    // a chart: dark strokes on white that run down the page (as coasts and isobars do), a new pattern every few lines
    void chart(int lines, double noiseAmp = 0.15) {
        std::normal_distribution<double> g(0, 1); std::vector<double> row(SPL, 1.0);
        for (int l = 0; l < lines; l++) {
            if (l % 6 == 0) {
                std::fill(row.begin(), row.end(), 1.0);
                std::uniform_int_distribution<int> at(0, SPL - 400);
                for (int k = 0; k < 30; k++) { int a = at(r2); for (int i = 0; i < 300; i++) row[a + i] = 0.0; }
            }
            for (int i = 0; i < SPL; i++) { double v = row[i] + noiseAmp * g(s.rng); s.tone(v < 0 ? 0 : (v > 1 ? 1 : v)); }
        }
    }
    // a white chart with one black bar BAR_AT after the pulse (as run() above): where it lands says whether it was phased
    void barChart(int lines, double pulseStart = 0.40) {
        const double pc = pulseStart + PULSE_W / 2;
        for (int l = 0; l < lines; l++) for (int i = 0; i < SPL; i++) {
            double x = (double)i / SPL, d = x - (pc + BAR_AT); d -= std::floor(d);
            s.tone(d < BAR_W ? 0.0 : 1.0);
        }
    }
    void noise(double sec) { s.noise((int)(sec * FS)); }
    // ★ receiver noise as a real SSB passband gives it — a new grey every 24 samples (~2 kHz), not every sample: per-sample
    //   white noise averages out over the decoder's 8-pixel blocks and reads FLAT (the steady-tone rule holds the verdict)
    void rxNoise(double sec) { std::uniform_real_distribution<double> u(0, 1); double v = 0.5; for (int k = 0; k < (int)(sec * FS); k++) { if (k % 24 == 0) v = u(s.rng); s.tone(v); } }
};
struct Pictures { int starts = 0; std::vector<int> firstLine, lines; std::vector<std::vector<double>> colSum; bool backwards = false; std::string diag; };
static Pictures decode(const std::vector<int16_t>& a, bool background = false) {
    // ★ background: the options an unattended WEFAX VFO runs with (vibe_bgdecode_config.h attachMsg — auto_start + auto_stop)
    WefaxDecoder::Config cfg; cfg.lpm = 120; cfg.usePhasing = true; cfg.autoStart = background; cfg.autoStop = background;
    WefaxDecoder dec(FS, cfg);
    Pictures p; p.firstLine.push_back(-1); p.lines.push_back(0); p.colSum.emplace_back(dec.width(), 0.0);
    int last = -1;
    dec.onDiag = [&](const std::string& m) { if (getenv("WEFAX_TEST_VERBOSE")) std::printf("      %s\n", m.c_str()); p.diag += m + " | "; };
    dec.onPhase = [&](int ph) { if (getenv("WEFAX_TEST_VERBOSE")) std::printf("      phase %d\n", ph); };
    dec.onStart = [&]() { p.starts++; p.firstLine.push_back(-1); p.lines.push_back(0); p.colSum.emplace_back(dec.width(), 0.0); last = -1; };
    dec.onLine = [&](uint32_t ln, uint32_t w, const uint8_t* px) {
        if ((int)ln <= last) p.backwards = true;   // a client would start a new picture here
        last = (int)ln;
        if (p.firstLine.back() < 0) p.firstLine.back() = (int)ln;
        p.lines.back()++;
        if (p.lines.back() > 40) for (uint32_t x = 0; x < w; x++) p.colSum.back()[x] += px[x];
    };
    for (size_t i = 0; i < a.size(); i += 4096) dec.process(&a[i], (int)std::min<size_t>(4096, a.size() - i));
    return p;
}
static int barColumn(const std::vector<double>& colSum) {
    const int W = (int)colSum.size(), bw = (int)std::lround(BAR_W * W);
    double best = 1e18; int at = -1;
    for (int x = 0; x < W; x++) { double sum = 0; for (int k = 0; k < bw; k++) sum += colSum[(x + k) % W]; if (sum < best) { best = sum; at = x; } }
    return at;
}

int main() {
    const int W = 1809, wantBar = (int)std::lround(BAR_AT * W);
    auto colErr = [&](int c) { int d = std::abs(c - wantBar) % W; return std::min(d, W - d); };
    // The decoder's line starts where the audio started; `lead` moves the pulse within ITS line.
    // pulse at ~50 % of the decoder's line:
    auto a = run(0.40, SPL * 3 + SPL / 10, 0, 1);
    std::printf("   .. mid-line: %s | bar at col %d (want %d)\n", a.diag.c_str(), a.barCol, wantBar);
    check(a.phased && colErr(a.barCol) <= 20, "a pulse mid-line is used, and the bar lands where it belongs");
    // ★ the seam: lead chosen so the pulse sits at ~98 % of the decoder's line — the JMH case
    auto b = run(0.40, SPL * 3 + (int)(SPL * 0.555), 0, 2);   // measured = pulse centre + lead → ~98 %
    std::printf("   .. on the seam: %s | bar at col %d\n", b.diag.c_str(), b.barCol);
    check(b.diag.find("pulse at 9") != std::string::npos, "the seam case really puts the pulse at 90-99 % of the decoder's line");
    check(b.phased && colErr(b.barCol) <= 20, "★ a pulse ON THE LINE'S SEAM is used (it read as a 51 % spread and was thrown away)");
    // ★ a quarter of the phasing lines lost to noise — on the seam too
    auto c = run(0.40, SPL * 3 + (int)(SPL * 0.555), 15, 3);
    std::printf("   .. seam + 15 of 60 phasing lines noise: %s | bar at col %d\n", c.diag.c_str(), c.barCol);
    check(c.phased && colErr(c.barCol) <= 20, "★ a quarter of the phasing lines faded out still phases (JMH: 27 of 38 agreed)");
    // most lines noise: refused, not guessed
    auto d = run(0.40, SPL * 3 + SPL / 10, 42, 4);
    std::printf("   .. 42 of 60 phasing lines noise: %s\n", d.diag.c_str());
    check(d.sawVerdict && !d.phased, "phasing that is mostly noise is REJECTED, never guessed");
    // ★ a start tone that wobbles across the threshold (garbled lines right after it is reached) fires ONE start
    auto g = run(0.40, SPL * 3 + SPL / 10, 0, 6, {7, 8}, 20);   // JMH-length 10 s tone: the run climbs back to 6
    std::printf("   .. 10 s start tone garbled at lines 7-8: %d start(s), %s\n", g.starts, g.diag.c_str());
    check(g.starts == 1, "★ a start tone with a garbled line fires ONE new chart, not two (the == trigger fired twice)");
    check(g.phased && colErr(g.barCol) <= 20, "…and that chart is still phased");
    check(a.starts == 1, "a clean start tone fires exactly one new chart");
    // ★★ A FADING start tone — every other line lost (NMF off K3FEF read 7.4, 4.1, 8.4, 4.4 …): the old rule wanted six
    //    tone lines IN A ROW and never fired; 7 of the last 10 does, once.
    auto h = run(0.40, SPL * 3 + SPL / 10, 0, 7, {1, 3, 5, 7, 9, 11, 13}, 20);
    std::printf("   .. 10 s start tone, every other line faded out: %d start(s), %s\n", h.starts, h.diag.c_str());
    check(h.starts == 1, "★ a start tone fading in and out on alternate lines still starts ONE new chart");
    check(h.phased && colErr(h.barCol) <= 20, "…and phases it");
    // no phasing at all — a start tone, then 30 s of noise: refused
    auto f = run(0.40, SPL * 3 + SPL / 10, 60, 5);
    std::printf("   .. phasing entirely noise: %s\n", f.diag.c_str());
    check(f.sawVerdict && !f.phased, "a start tone followed by noise, not phasing, is REJECTED");
    // sweep the seam: wherever the decoder's line starts, a clean phasing is used and the bar lands right
    int ok = 0, total = 0;
    for (int k = 0; k < 20; k++) {
        auto e = run(0.40, SPL * 3 + (int)(SPL * k / 20.0), 0, 10 + k);
        total++; if (e.phased && colErr(e.barCol) <= 20) ok++;
        else std::printf("   .. sweep %d/20 failed: %s | bar %d\n", k, e.diag.c_str(), e.barCol);
    }
    check(ok == total, "★ a clean phasing is used wherever the decoder's line happens to start (" + std::to_string(ok) + "/" + std::to_string(total) + ")");
    { int last = -1; int f1 = flips(true, 2.1, 600, 21, &last);   // 2.1: the previous decoder flickered 16 times here
      std::printf("   .. weak chart under heavy noise, 600 lines: %d status changes, ends %s\n", f1, last == 3 ? "receiving" : "standing by");
      check(f1 <= 4 && last == 3, "★ a weak chart under noise reads 'receiving' and stays there (≤ 4 changes in 5 minutes)");
      int f2 = flips(false, 2.1, 400, 22, &last);
      std::printf("   .. pure noise, 400 lines: %d status changes, ends %s\n", f2, last == 3 ? "receiving" : "standing by");
      check(last != 3 && f2 == 0, "pure noise never reads 'receiving'"); }
    // ── A NEW CHART WITHOUT ITS START TONE ────────────────────────────────────────────────────────────────────────
    { // (a) chart A, its stop tone, noise, NO start tone, phasing, chart B — the R36S case
      Tx t(31); t.noise(3.1); t.startTone(); t.phasing(60); t.chart(300); t.stopTone(); t.noise(40); t.phasing(60, 0.70); t.barChart(300, 0.70);
      auto p = decode(t.s.out);
      const int last = (int)p.firstLine.size() - 1;
      std::printf("   .. chart, stop tone, no start tone, phasing, chart: %d START(s), picture B from line %d, %d lines, bar at col %d\n",
                  p.starts, p.firstLine[last], p.lines[last], barColumn(p.colSum[last]));
      check(p.starts == 2, "★ a chart after the stop tone, its start tone lost, is a NEW picture — image start sent once for it");
      // ★ "line 0": imageLine is reset to 0 exactly as the start tone resets it; the line blend emits its first line at 1–2
      //   either way, so the test is that picture B begins where a start tone's picture (A) began.
      check(p.firstLine[last] == p.firstLine[1] && p.firstLine[1] <= 2, "★ …and it starts at line 0, as the start tone's picture does (first line "
            + std::to_string(p.firstLine[last]) + " vs " + std::to_string(p.firstLine[1]) + ")");
      check(p.diag.find("no start tone — it looks like phasing") != std::string::npos && p.diag.rfind("used") != std::string::npos,
            "…its phasing is found and used");
      check(colErr(barColumn(p.colSum[last])) <= 20, "…so chart B is drawn phased (the bar where it belongs)");
      check(p.lines[1] >= 290, "chart A keeps its own lines (" + std::to_string(p.lines[1]) + ")"); }
    { // (b) chart, stop tone, a chart with neither start tone nor phasing
      Tx t(32); t.noise(3.1); t.startTone(); t.phasing(60); t.chart(300); t.stopTone(); t.noise(30); t.chart(300);
      auto p = decode(t.s.out);
      const int last = (int)p.firstLine.size() - 1;
      std::printf("   .. chart, stop tone, chart with no start tone or phasing: %d START(s), picture B from line %d, %d lines\n",
                  p.starts, p.firstLine[last], p.lines[last]);
      check(p.starts == 2 && p.firstLine[last] == p.firstLine[1], "★ a chart after the stop tone with no start tone and no phasing is a new picture from line 0");
      check(p.lines[last] >= 280, "…and none of it is swallowed as phasing (" + std::to_string(p.lines[last]) + " of 300 lines)"); }
    { // (c) one long chart with a 40 s fade in the middle: ONE picture
      Tx t(33); t.noise(3.1); t.startTone(); t.phasing(60); t.chart(400); t.noise(40); t.chart(400);
      auto p = decode(t.s.out);
      std::printf("   .. one chart with a 40 s fade: %d START(s), %d lines\n", p.starts, p.lines.back());
      check(p.starts == 1 && !p.backwards, "★ a fade in the middle of a chart never splits it — one picture, line numbers never go back"); }
    { // (d) a chart with no stop tone, then the next transmission's phasing with no start tone either
      Tx t(34); t.noise(3.1); t.startTone(); t.phasing(60); t.chart(400); t.phasing(60, 0.70); t.barChart(300, 0.70);
      auto p = decode(t.s.out);
      const int last = (int)p.firstLine.size() - 1;
      std::printf("   .. chart, then phasing with no tone either side: %d START(s), picture B from line %d, bar at col %d\n",
                  p.starts, p.firstLine[last], barColumn(p.colSum[last]));
      check(p.starts == 2 && p.firstLine[last] == p.firstLine[1], "★ phasing on its own, well into a chart, starts a new picture from line 0");
      check(colErr(barColumn(p.colSum[last])) <= 20, "…phased on those very lines"); }
    { // (e) the tail of a start tone's own phasing (it runs past the window) must not start a second chart
      Tx t(35); t.noise(3.1); t.startTone(); t.phasing(90); t.chart(300);
      auto p = decode(t.s.out);
      std::printf("   .. 45 s of phasing after a start tone: %d START(s)\n", p.starts);
      check(p.starts == 1, "the rest of a long phasing after a start tone is not a second chart"); }
    { // (f) a black band across the whole chart with white margins — the nearest a chart gets to phasing — 8 lines tall
      Tx t(36); t.noise(3.1); t.startTone(); t.phasing(60); t.chart(200);
      for (int l = 0; l < 8; l++) for (int i = 0; i < SPL; i++) { double x = (double)i / SPL; t.s.tone(x < 0.03 || x > 0.98 ? 1.0 : 0.0); }
      t.chart(200);
      auto p = decode(t.s.out);
      std::printf("   .. a chart with an 8-line black band: %d START(s)\n", p.starts);
      check(p.starts == 1, "a short black band in a chart is not phasing"); }
    { // ten minutes of noise
      Tx t(37); t.noise(600);
      auto p = decode(t.s.out);
      check(p.starts == 0, "ten minutes of noise starts no chart"); }
    // ── ★★★ WAITING IN THE BACKGROUND: A CHART OUT OF THE NOISE (2026-11-22) ──────────────────────────────────────────
    // Northwood 4610 off Stuart's RX888 at 10:00 UTC: noise, then a chart faded in ~2½ minutes after it began — no start
    // tone (best 2 of 10), no phasing (0 of 20) — and its stop tone at 10:11:09. Waiting for a start, the background decoder
    // drew nothing (wefax_wav bg: "0 begun"). The numbers here are that recording's: ~70 s of noise before, a 940-line chart
    // (≈ 1250 sent less the 2½ minutes lost), the stop tone. Measured on the recording after the fix: begun at 2246.4 s,
    // 880 lines, ended on the stop tone; 5 h of other modes and noise (NAVTEX 2 h, FT8 1 h, SSTV 1 h 45, MSF, DCF77): 0 begun.
    { Tx t(41); t.noise(70); t.chart(940); t.stopTone(); t.noise(20);
      auto p = decode(t.s.out, true);
      std::printf("   .. background, a chart that fades in with no start tone or phasing: %d START(s), %d lines\n", p.starts, p.lines.back());
      check(p.starts == 1 && p.lines.back() >= 900, "★★ background: a chart out of the noise (no start tone, no phasing) is begun and drawn ("
            + std::to_string(p.lines.back()) + " of 940 lines)");
      check(p.diag.find("a chart out of the noise") != std::string::npos && p.diag.find("stop tone on") != std::string::npos, "…said why in the log, and ended on its stop tone"); }
    { Tx t(42); t.noise(3.1); t.startTone(); t.phasing(60); t.chart(300); t.stopTone(); t.noise(30); t.chart(300);
      auto p = decode(t.s.out, true);
      std::printf("   .. background, chart, stop tone, chart with no start tone or phasing: %d START(s), last %d lines\n", p.starts, p.lines.back());
      check(p.starts == 2 && p.lines.back() >= 280, "★ background: a chart after the stop tone is a new chart too (the rule never fired in background before: "
            "the chart/noise verdict was only measured on lines being drawn)"); }
    { Tx t(43); t.noise(3.1); t.startTone(); t.phasing(60); t.chart(300);
      auto p = decode(t.s.out, true);
      check(p.starts == 1 && p.diag.find("out of the noise") == std::string::npos, "background: a start tone's own 300 Hz stripes never begin a chart out of the noise first"); }
    { // a false start — 100 lines of something that runs down the page, then noise: begun, and ended after 3 minutes of noise
      Tx t(44); t.rxNoise(30); t.chart(100); t.rxNoise(240);
      auto p = decode(t.s.out, true);
      check(p.starts == 1 && p.diag.find("has been noise for 360 lines") != std::string::npos,
            "★ background: a chart out of the noise that goes back to noise is ended after 360 lines (never drawn on until a stop tone)"); }
    { Tx t(45); t.noise(300); t.rxNoise(300);
      auto p = decode(t.s.out, true);
      check(p.starts == 0, "★ background: ten minutes of noise (white, then a receiver's) begins no chart"); }
    std::printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
