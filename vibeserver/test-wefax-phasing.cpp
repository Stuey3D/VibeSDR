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
// ★ KNOWN DECODER ARTEFACT (measured here): even on perfect audio about one phasing line in seven misreads the pulse,
//   in a repeating rhythm (59 %, 3.7 %, 46.6 % against 52.5 %) — 28 of 38 agree at best, and Stuart's off-air JMH scored
//   28 too. The majority test absorbs it; finding it would give real fading far more margin. Not chased yet.
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
    std::printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
