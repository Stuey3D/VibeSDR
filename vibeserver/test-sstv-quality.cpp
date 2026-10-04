// test-sstv-quality.cpp — the SSTV decoder's GEOMETRY and COLOUR, measured (audit 2026-10-04, rows 1-5).
//
// ★★★ WHY THIS TEST EXISTS. Five faults were found by reading the decoder against slowrx, QSSTV and
//     the Dayton/Barber mode spec, and every one of them is a number — a slant in px/line, a constant
//     offset in px, a colour error in levels. None of them can be judged by looking at one picture
//     that "came through fine": the slant search had NEVER returned a rate since a8b7464c, and nothing
//     noticed, because a picture with no clock error looks right whether or not the search works.
//     So this generates the signal with a KNOWN clock error and scores what comes out.
//
// ★★ THE TRANSMITTER'S CLOCK, NOT OURS. Slant is the sending station's sound card running at
//    48000·(1+ε) while it believes 48000 — 100 to 1000 ppm is normal. An SDR's clock being exact does
//    not remove it: the error is in the AUDIO, before it ever reaches the radio. The generator models
//    exactly that: every duration AND every tone is scaled by (1+ε), as a real sound card would.
//
// ★ Stuart's caution still stands (tools/sstv_harness.cpp): synthetic audio is the first gate, not the
//   last word — the Essex Ham recordings and off-air pictures remain the proof. This test exists
//   because a clean recording cannot carry a KNOWN slant, and a known slant is what row 2 is about.
//
// Signal path is the server's: 48 kHz float → the host's 4-sample box average → int16 12 kHz
// (vibe_decoder_host.h decode_) → SstvDecoder(12000, autoSync=true, adaptive=true).
//
//   test-sstv-quality            run the matrix and ASSERT (registered in scripts/run-tests.sh)
//   test-sstv-quality --report   print the numbers only (used to measure the OLD decoder)
#include "decoders/sstv_decoder.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

// ── The test card ────────────────────────────────────────────────────────────────────────────
// Top 3/4: eight 100 % colour bars (white yellow cyan green magenta red blue black) — vertical edges
// on every line, so the slant and offset can be read on every line, and flat fields for colour.
// Bottom 1/4: a black/white checker — horizontal AND vertical edges, the Robot/PD line pairing's test.
static const uint8_t kBars[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},
                                    {255,0,255},{255,0,0},{0,0,255},{0,0,0}};
struct Card { int W, H; std::vector<uint8_t> rgb; const uint8_t* px(int x, int y) const { return &rgb[((size_t)y*W + x)*3]; } };
static Card makeCard(int W, int H) {
    Card c{W, H, std::vector<uint8_t>((size_t)W*H*3)};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t* p = &c.rgb[((size_t)y*W + x)*3];
            if (y < H*3/4) { const uint8_t* b = kBars[x*8/W]; p[0]=b[0]; p[1]=b[1]; p[2]=b[2]; }
            else { const uint8_t v = (((x*8/W) + (y*16/H)) & 1) ? 255 : 0; p[0]=p[1]=p[2]=v; }
        }
    return c;
}

// ── The generator ────────────────────────────────────────────────────────────────────────────
// A tone schedule in TRANSMITTER time, then rendered phase-continuously at the real 48 kHz with
// the transmitter's clock error applied.
struct Seg { double f, dur; };
struct Sched { std::vector<Seg> s; void add(double f, double d) { s.push_back({f, d}); } };
static double lumHz(double v) { return 1500.0 + v * 3.1372549; }
// BT.601 studio range, as the Dayton spec and MMSSTV send it.
static void yuv(const uint8_t* p, double& Y, double& U, double& V) {
    const double R = p[0], G = p[1], B = p[2];
    Y = 16.0  + ( 65.738*R + 129.057*G +  25.064*B) / 256.0;
    U = 128.0 + (-37.945*R -  74.494*G + 112.439*B) / 256.0;
    V = 128.0 + (112.439*R -  94.154*G -  18.285*B) / 256.0;
}
static void vis(Sched& s, int code, double stop) {
    s.add(1900, 0.300); s.add(1200, 0.010); s.add(1900, 0.300); s.add(1200, 0.030);
    int par = 0;
    for (int b = 0; b < 7; b++) { const int bit = (code >> b) & 1; par ^= bit; s.add(bit ? 1100 : 1300, 0.030); }
    s.add(par ? 1100 : 1300, 0.030);
    s.add(1200, stop);
}
enum Mode { M1, S1, R36, PD120 };
static const char* modeName(Mode m) { return m==M1?"Martin M1":m==S1?"Scottie S1":m==R36?"Robot 36":"PD-120"; }
static Sched schedule(Mode m, const Card& c, double stop) {
    Sched s; s.add(1900, 0.0); // placeholder
    s.s.clear();
    const int W = c.W, H = c.H;
    auto chan = [&](int y, int ch, double pxT) { for (int x = 0; x < W; x++) s.add(lumHz(c.px(x, y)[ch]), pxT); };
    if (m == M1) {
        vis(s, 44, stop);
        for (int y = 0; y < H; y++) {
            s.add(1200, 4.862e-3); s.add(1500, 0.572e-3);
            chan(y, 1, 0.4576e-3); s.add(1500, 0.572e-3);
            chan(y, 2, 0.4576e-3); s.add(1500, 0.572e-3);
            chan(y, 0, 0.4576e-3); s.add(1500, 0.572e-3);
        }
    } else if (m == S1) {
        vis(s, 60, stop);
        s.add(1200, 9e-3);                                   // the one starting sync
        for (int y = 0; y < H; y++) {
            s.add(1500, 1.5e-3); chan(y, 1, 0.432e-3);
            s.add(1500, 1.5e-3); chan(y, 2, 0.432e-3);
            s.add(1200, 9e-3); s.add(1500, 1.5e-3); chan(y, 0, 0.432e-3);
        }
    } else if (m == R36) {
        vis(s, 8, stop);
        for (int y = 0; y < H; y++) {
            s.add(1200, 9e-3); s.add(1500, 3e-3);
            for (int x = 0; x < W; x++) { double Y, U, V; yuv(c.px(x, y), Y, U, V); s.add(lumHz(Y), 0.275e-3); }
            s.add((y & 1) ? 2300 : 1500, 4.5e-3); s.add(1900, 1.5e-3);
            for (int x = 0; x < W; x += 2) {                 // chroma at half resolution, 44 ms
                double Y, U0, V0, U1, V1; yuv(c.px(x, y), Y, U0, V0); yuv(c.px(x+1, y), Y, U1, V1);
                s.add(lumHz((y & 1) ? (U0+U1)/2 : (V0+V1)/2), 0.275e-3);
            }
        }
    } else {
        vis(s, 95, stop);
        for (int y = 0; y < H; y += 2) {
            s.add(1200, 20e-3); s.add(1500, 2.08e-3);
            double Y, U, V, Y2, U2, V2;
            for (int x = 0; x < W; x++) { yuv(c.px(x, y), Y, U, V); s.add(lumHz(Y), 0.19e-3); }
            for (int x = 0; x < W; x++) { yuv(c.px(x, y), Y, U, V); yuv(c.px(x, y+1), Y2, U2, V2); s.add(lumHz((V+V2)/2), 0.19e-3); }
            for (int x = 0; x < W; x++) { yuv(c.px(x, y), Y, U, V); yuv(c.px(x, y+1), Y2, U2, V2); s.add(lumHz((U+U2)/2), 0.19e-3); }
            for (int x = 0; x < W; x++) { yuv(c.px(x, y+1), Y, U, V); s.add(lumHz(Y), 0.19e-3); }
        }
    }
    return s;
}
/** 48 kHz float audio. ppm = transmitter clock error (+ = its sound card runs FAST). */
static std::vector<float> render(const Sched& sc, double ppm, double noiseSigma, unsigned seed) {
    const double fs = 48000.0, k = 1.0 + ppm * 1e-6;
    std::mt19937 rng(seed); std::normal_distribution<double> n(0.0, 1.0);
    std::vector<float> a;
    const double pre = 0.5, post = 4.0;
    for (int i = 0; i < (int)(pre*fs); i++) a.push_back((float)(noiseSigma * n(rng)));
    double total = 0; for (auto& g : sc.s) total += g.dur;
    const size_t N = (size_t)(total / k * fs);
    double ph = 0, segEnd = sc.s.empty() ? 0 : sc.s[0].dur; size_t si = 0;
    for (size_t i = 0; i < N; i++) {
        const double tau = (double)i / fs * k;            // transmitter's idea of the time
        while (si + 1 < sc.s.size() && tau >= segEnd) { si++; segEnd += sc.s[si].dur; }
        ph += 2.0 * M_PI * sc.s[si].f * k / fs;
        if (ph > 2*M_PI) ph -= 2*M_PI;
        a.push_back((float)(0.5 * std::sin(ph) + noiseSigma * n(rng)));
    }
    for (int i = 0; i < (int)(post*fs); i++) a.push_back((float)(noiseSigma * n(rng)));
    return a;
}

// ── Decode, as the server does ───────────────────────────────────────────────────────────────
struct Decoded { int W = 0, H = 0; std::vector<uint8_t> live, fin; bool redrew = false; std::vector<std::string> status; };
static Decoded decode(const std::vector<float>& a48) {
    Decoded d;
    std::vector<int16_t> pcm; pcm.reserve(a48.size()/4 + 1);
    float acc = 0; int n = 0;
    for (float v : a48) {                                     // ★ the host's box average, verbatim
        acc += v;
        if (++n >= 4) { const int s = (int)std::lround(acc / 4.0f * 32767.0f); pcm.push_back((int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s))); n = 0; acc = 0; }
    }
    std::mutex mu; std::atomic<int> completes{0}; std::atomic<bool> gaveUp{false};
    {
        SstvDecoder dec(12000, true, true);
        dec.onImageStart = [&](int w, int h) { std::lock_guard<std::mutex> l(mu); d.W = w; d.H = h; d.live.assign((size_t)w*h*3, 0); };
        dec.onLine = [&](int y, int w, const uint8_t* rgb) {
            std::lock_guard<std::mutex> l(mu);
            auto& img = d.redrew ? d.fin : d.live;
            if (y >= 0 && y < d.H && w == d.W) std::memcpy(&img[(size_t)y*w*3], rgb, (size_t)w*3);
        };
        dec.onRedrawStart = [&]() { std::lock_guard<std::mutex> l(mu); d.redrew = true; d.fin = d.live; };
        dec.onComplete = [&]() { completes++; };
        dec.onStatus = [&](const std::string& s) {
            std::lock_guard<std::mutex> l(mu); d.status.push_back(s);
            if (s.find("not corrected") != std::string::npos || s.find("alignment skipped") != std::string::npos) gaveUp = true;
        };
        for (size_t i = 0; i < pcm.size(); i += 1024) dec.process(&pcm[i], (int)std::min<size_t>(1024, pcm.size() - i));
        for (int i = 0; i < 1200 && completes.load() < 2 && !gaveUp.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!d.redrew) d.fin = d.live;
    return d;
}

// ── Scoring ──────────────────────────────────────────────────────────────────────────────────
struct Score { double slantFrame = NAN, offset = NAN, colMean = NAN, colMax = NAN; int rows = 0; };
/** The G channel falls 255→0 at x = W/2 on every bar row: find it per row to a fraction of a pixel. */
static Score score(const std::vector<uint8_t>& img, int W, int H) {
    Score sc;
    if (img.empty()) return sc;
    const double truth = W/2 - 0.5;
    std::vector<double> ys, xs;
    for (int y = 2; y < H*3/4 - 2; y++) {
        const uint8_t* row = &img[(size_t)y*W*3];
        int lo = 255, hi = 0;
        for (int x = W/4; x < 3*W/4; x++) { lo = std::min<int>(lo, row[x*3+1]); hi = std::max<int>(hi, row[x*3+1]); }
        if (hi - lo < 100) continue;
        const double th = (hi + lo) / 2.0;
        for (int x = W/4; x < 3*W/4 - 1; x++) {
            const double a = row[x*3+1], b = row[(x+1)*3+1];
            if (a >= th && b < th) { ys.push_back(y); xs.push_back(x + (a - th) / (a - b)); break; }
        }
    }
    sc.rows = (int)ys.size();
    if (sc.rows < H/4) return sc;
    // least squares x = a + b·y
    double sy=0, sx=0, syy=0, sxy=0; const double nn = (double)ys.size();
    for (size_t i = 0; i < ys.size(); i++) { sy += ys[i]; sx += xs[i]; syy += ys[i]*ys[i]; sxy += ys[i]*xs[i]; }
    const double b = (nn*sxy - sy*sx) / (nn*syy - sy*sy), a = (sx - b*sy) / nn;
    sc.slantFrame = b * H;
    sc.offset = a + b * (H*3/8.0) - truth;                  // the offset at the middle of the bars
    // colour: middle half of each bar, rows in the middle of the bar block, shifted by the fitted edge
    double sum = 0, mx = 0; int cnt = 0;
    for (int bar = 0; bar < 8; bar++) {
        double m[3] = {0,0,0}; int k = 0;
        for (int y = H/8; y < H*5/8; y++) {
            const int sh = (int)std::lround(a + b*y - truth);
            for (int x = bar*W/8 + W/32; x < (bar+1)*W/8 - W/32; x++) {
                const int xx = x + sh; if (xx < 0 || xx >= W) continue;
                for (int c = 0; c < 3; c++) m[c] += img[((size_t)y*W + xx)*3 + c];
                k++;
            }
        }
        if (!k) continue;
        for (int c = 0; c < 3; c++) { const double e = std::fabs(m[c]/k - kBars[bar][c]); sum += e; cnt++; if (e > mx) mx = e; }
    }
    sc.colMean = cnt ? sum / cnt : NAN; sc.colMax = mx;
    return sc;
}

int main(int argc, char** argv) {
    const bool report = argc > 1 && !std::strcmp(argv[1], "--report");
    std::printf("SSTV quality — synthetic transmissions with a known transmitter clock error, 20 dB SNR\n");
    std::printf("  %-10s %6s %-4s | %-27s | %-27s | %s\n", "mode", "ppm", "", "LIVE slant/frame  offset", "FINAL slant/frame offset", "colour mean/max  redraw");
    struct Case { Mode m; double ppm; double stop; const char* tag; };
    std::vector<Case> cases;
    for (Mode m : {M1, S1, R36, PD120}) for (double p : {0.0, 300.0, -500.0}) cases.push_back({m, p, 0.030, ""});
    // ★ Row 5: a picture that starts LATE — line 0's sync before the decode began. A 12 ms stop bit
    //   moves the picture 18 ms earlier against the VIS hand-off, which otherwise lands ~7 ms early.
    //   The old half-line wrap turned exactly this into a half-line shift.
    for (Mode m : {M1, S1, R36, PD120}) cases.push_back({m, 0.0, 0.012, "late"});
    std::vector<Score> finals(cases.size()), lives(cases.size());
    std::vector<int> redrew(cases.size());
    std::vector<std::string> lastStatus(cases.size());
    std::vector<int> sheared(cases.size());
    // ★ The cases are independent decoders — run them side by side so the suite stays quick.
    std::vector<std::thread> th;
    for (size_t i = 0; i < cases.size(); i++)
        th.emplace_back([&, i] {
            const Mode m = cases[i].m;
            const int W = (m == PD120) ? 640 : 320, H = (m == PD120) ? 496 : (m == R36 ? 240 : 256);
            const Card c = makeCard(W, H);
            // ★ SSTV_SEED / SSTV_NOISE (--report only) re-run the matrix on other noise, to see the spread.
            const char* es = report ? std::getenv("SSTV_SEED") : nullptr;
            const char* en = report ? std::getenv("SSTV_NOISE") : nullptr;
            const auto a = render(schedule(m, c, cases[i].stop), cases[i].ppm, en ? std::atof(en) : 0.1,
                                  (es ? (unsigned)std::atoi(es) : 1000u) + (unsigned)i);
            const Decoded d = decode(a);
            lives[i]  = (d.W == W) ? score(d.live, W, H) : Score{};
            finals[i] = (d.W == W) ? score(d.fin, W, H) : Score{};
            redrew[i] = d.redrew;
            if (!d.status.empty()) lastStatus[i] = d.status.back();
            for (auto& st : d.status) if (st.find("slant corrected") != std::string::npos) sheared[i] = 1;
        });
    for (auto& t : th) t.join();
    for (size_t i = 0; i < cases.size(); i++) {
        const Score& L = lives[i]; const Score& F = finals[i];
        std::printf("  %-10s %+6.0f %-4s | %8.2f px %8.2f px     | %8.2f px %8.2f px     | %5.1f / %5.1f   %s\n",
                    modeName(cases[i].m), cases[i].ppm, cases[i].tag, L.slantFrame, L.offset, F.slantFrame, F.offset,
                    F.colMean, F.colMax, redrew[i] ? "yes" : "no");
        if (report) std::printf("      last status: %s\n", lastStatus[i].c_str());
    }
    if (report) return 0;

    std::printf("\nAssertions (new decoder):\n");
    for (size_t i = 0; i < cases.size(); i++) {
        const Score& F = finals[i];
        char what[160];
        const char* tg = cases[i].tag;
        // ★★★ THE GATE (ca092485): a picture with no clock error must be SHIFTED, never sheared —
        //     and one with a real 300-500 ppm error must be.
        if (cases[i].ppm == 0) std::snprintf(what, sizeof what, "%s %s 0 ppm: no shear applied", modeName(cases[i].m), tg);
        else std::snprintf(what, sizeof what, "%s %+.0f ppm: the slant was corrected (status says so)", modeName(cases[i].m), cases[i].ppm);
        ok(sheared[i] == (cases[i].ppm != 0 ? 1 : 0), what);
        std::snprintf(what, sizeof what, "%s %+.0f ppm %s: residual slant %.2f px over the frame (< 1)", modeName(cases[i].m), cases[i].ppm, tg, F.slantFrame);
        ok(std::isfinite(F.slantFrame) && std::fabs(F.slantFrame) < 1.0, what);
        std::snprintf(what, sizeof what, "%s %+.0f ppm %s: constant offset %.2f px (within ±1)", modeName(cases[i].m), cases[i].ppm, tg, F.offset);
        ok(std::isfinite(F.offset) && std::fabs(F.offset) <= 1.0, what);
        std::snprintf(what, sizeof what, "%s %+.0f ppm %s: bar colour error mean %.1f, max %.1f levels (mean < 8, max < 20)",
                      modeName(cases[i].m), cases[i].ppm, tg, F.colMean, F.colMax);
        ok(std::isfinite(F.colMean) && F.colMean < 8.0 && F.colMax < 20.0, what);
    }
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
