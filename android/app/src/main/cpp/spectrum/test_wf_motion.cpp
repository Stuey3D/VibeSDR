// Host test for wf_motion.h + wf_zoom_fan.h — the same cases as the motion and zoom-fan sections of
// scripts/test_wf_row_place.ts.
//   clang++ -std=c++17 -O2 test_wf_motion.cpp -o /tmp/test_wf_motion && /tmp/test_wf_motion
#include "wf_motion.h"
#include "wf_zoom_fan.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vibewf;

static int fails = 0, passes = 0;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const int N = 1024, ROWS = 256;
static const double CARRIER = 1'050'000;
static unsigned seed = 7;
static double rnd() { seed = (seed * 1103515245u + 12345u) & 0x7fffffffu; return seed / (double)0x7fffffff; }

static std::vector<uint8_t> noisyRow(double centre, double span) {
    std::vector<uint8_t> r(N);
    for (int i = 0; i < N; i++) r[i] = (uint8_t)(34 + (int)std::floor(rnd() * 13));
    const int bin = (int)std::floor((CARRIER - centre) / (span / N) + 0.5) + N / 2;
    for (int d = -2; d <= 2; d++) if (bin + d >= 0 && bin + d < N) r[bin + d] = d == 0 ? 230 : 200;
    return r;
}
// ── MOTION: rows placed as spectrum_engine.cpp does (placeRow), the motion beside them ──
static const int MR = 256;
static std::vector<uint8_t> M((size_t)N * (MR + MOTION_ROWS), 0);
static MotionState ms(MR, N);
static FanState mf(MR);
static int mfr = 0;
static void mpush(const std::vector<uint8_t>& row, double rc, double span, double vc, double vbw) {
    placeRow(M.data(), MR, N, mfr, row.data(), rc, span / N, vc, vbw, ms, mf); mfr++;
}
static int mcol(int f) {   // the displayed carrier's centre column (the peak, 230)
    const uint8_t* d = wfImageBytes(mf, ms, M.data(), N, MR) + (size_t)(f % MR) * N;
    for (int i = 0; i < N; i++) if (d[i] == 230) return i;
    return -1;
}

int main() {
    double vc = 1'000'000, vbw = 200'000;
    for (int i = 0; i < 10; i++) mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
    {   // steady: stored byte-for-byte, no motion, no range
        seed = 99; auto ref = noisyRow(vc, vbw);
        const size_t slot = (size_t)(mfr % MR) * N;
        seed = 99; mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
        CHECK(std::memcmp(ref.data(), M.data() + slot, N) == 0, "steady row changed");
        bool zero = true; for (int f = 0; f < mfr; f++) if (ms.h[f % MR] != 0) zero = false;
        CHECK(zero && ms.lo > ms.hi, "steady rows have motion");
    }
    {   // encode / decode: 1/64 column, both signs, the zero code
        bool okc = true;
        for (double h : {0.0, 1.0 / 64, -1.0 / 64, 7.5, -38.25, 127.984375, -128.0}) {
            const int v = encodeMotion(h); if (std::fabs(decodeMotion(v >> 8, v & 255) - h) > 1e-9) okc = false;
        }
        CHECK(okc && encodeMotion(0) == MOTION_ZERO, "motion encoding does not round-trip");
    }
    {   // a FAST tune (38 bins a row; rows built for the view two back): every pair's h is the carrier's move
        const int from = mfr; std::vector<double> cs;
        for (int i = 1; i <= 15; i++) {
            vc += 7'500; cs.push_back(vc);
            const double rc = cs[std::max(0, (int)cs.size() - 3)];
            mpush(noisyRow(rc, 200'000), rc, 200'000, vc, vbw);
        }
        for (int i = 0; i < 10; i++) mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
        int bad = 0, moving = 0;
        for (int f = from - 1; f < mfr - 1; f++) {
            const double h = ms.h[f % MR]; const int a = mcol(f), b = mcol(f + 1);
            if (h != 0) moving++;
            if (a < 0 || b < 0 || std::fabs(a + h - b) > 1.01) bad++;
        }
        CHECK(bad == 0 && moving >= 14, "fast tune: %d pairs mis-predicted, %d moving", bad, moving);
        CHECK(ms.lo <= from && ms.hi >= from + 13 && ms.hi < mfr - 1, "moving range [%g, %g] does not cover the tune", ms.lo, ms.hi);
        CHECK(ms.h[(mfr - 1) % MR] == 0, "the newest row has motion (it has no newer row)");
        const uint8_t* img = wfImageBytes(mf, ms, M.data(), N, MR); bool same = true;
        for (int r = 0; r < MR; r++) if (std::fabs(decodeMotion(img[MR * N + r], img[(MR + 1) * N + r]) - ms.h[r]) > 1.0 / 64) same = false;
        CHECK(same, "image motion rows do not decode to the slots' h");
    }
    {   // a JUMP (bookmark): re-mapped like a zoom — history at the live column, the exposed edge blank, no motion
        for (int i = 0; i < 5; i++) mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
        const int jf = mfr;
        vc += 200'000 * MOTION_MAX_FRAC * 1.5;
        mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
        const double live = N / 2 + (CARRIER - vc) / (vbw / N);
        int off = 0; for (int f = jf - 5; f < jf; f++) if (std::fabs(mcol(f) - live) > 1.01) off++;
        CHECK(off == 0, "jump: %d history rows not shifted into place (a slice)", off);
        CHECK(ms.h[(jf - 1) % MR] == 0, "jump pair interpolated (%g)", ms.h[(jf - 1) % MR]);
        const uint8_t* d = wfImageBytes(mf, ms, M.data(), N, MR) + (size_t)((jf - 1) % MR) * N; int edge = 0;
        const int shift = (int)std::floor(200'000 * MOTION_MAX_FRAC * 1.5 / (vbw / N));
        for (int x = N - shift + 1; x < N; x++) if (d[x] != 0) edge++;
        CHECK(edge == 0, "jump: the newly exposed edge is not blank (%d px)", edge);
    }
    {   // a PINCH: never motion (the fan placed the history)
        const int from = mfr; double bw = vbw;
        for (int i = 0; i < 12; i++) { bw *= 0.95; mpush(noisyRow(vc, bw), vc, bw, vc, bw); }
        int mv = 0; for (int f = from - 1; f < mfr; f++) if (ms.h[f % MR] != 0) mv++;
        CHECK(mv == 0, "pinch: %d pairs carry motion", mv);
    }
    // ── THE ZOOM FAN (wf_zoom_fan.h) — the same cases as the zoom-fan section of scripts/test_wf_row_place.ts ──
    {
        const int R = 256;
        std::vector<uint8_t> S((size_t)N * (R + MOTION_ROWS), 0);
        MotionState gl(R, N); FanState fan(R);
        int fr = 0;
        double vc = 1'030'000, vbw = 200'000;
        auto fpush = [&](double rc, double span) {
            std::vector<uint8_t> r(N, 40);
            const int bin = (int)std::floor((CARRIER - rc) / (span / N) + 0.5) + N / 2;
            if (bin >= 0 && bin < N) r[bin] = 230;
            placeRow(S.data(), R, N, fr, r.data(), rc, span / N, vc, vbw, gl, fan); fr++;
        };
        auto D = [&]() { return wfImageBytes(fan, gl, S.data(), N, R); };
        auto dlit = [&](int f, int& a, int& b) {
            const uint8_t* d = D() + (size_t)(f % R) * N; a = b = -1;
            for (int i = 0; i < N; i++) if (d[i] >= 200) { if (a < 0) a = i; b = i; }
            return a >= 0;
        };
        auto liveCol = [&]() { return (CARRIER - (vc - vbw / 2)) / (vbw / N); };
        auto offCol = [&](int f0, int f1) {
            int bad = 0; const double want = liveCol();
            for (int f = f0; f < f1; f++) { int a, b; if (!dlit(f, a, b) || a > want + 1.5 || b < want - 2.5) bad++; }
            return bad;
        };
        auto offAll = [&]() { return offCol(0, 60) + offCol(64, fr); };   // rows 60–63 are the tune: they BEND
        auto conn = [&](int f0, int f1) {
            int g = 0;
            for (int f = f0 + 1; f < f1; f++) { int pa, pb, qa, qb; if (!dlit(f - 1, pa, pb) || !dlit(f, qa, qb) || qa > pb + 1 || qb < pa - 1) g++; }
            return g;
        };
        for (int i = 0; i < 60; i++) fpush(vc, vbw);
        CHECK(D() == S.data() && fan.disp.empty(), "unzoomed: the display is not the stored ring");
        std::vector<uint8_t> stored0(S.begin(), S.begin() + 60 * N);
        vc += 2'000; for (int i = 0; i < 4; i++) fpush(vc, vbw);
        bool ident = true; for (int i = 0; i < R; i++) if (fan.xfA[i] != 1 || fan.xfB[i] != 0) ident = false;
        CHECK(D() == S.data() && ident, "a tune touched the history");
        vc = 1'030'000; for (int i = 0; i < 4; i++) fpush(vc, vbw);
        const int preZoom = fr;
        vbw = 100'000; fpush(1'030'000, 200'000);                         // one old-span frame in flight
        for (int i = 0; i < 6; i++) fpush(vc, vbw);
        CHECK(offAll() == 0, "x2 zoom in: %d rows off the live column (slices)", offAll());
        CHECK(std::memcmp(stored0.data(), S.data(), stored0.size()) == 0, "a zoom rewrote the stored rows");
        { int c0 = -1; for (int x = 0; x < N; x++) if (S[x] >= 200) c0 = x;
          CHECK(std::fabs(c0 - liveCol()) > 50, "control: the stored row is not a slice (%d vs %g)", c0, liveCol()); }
        CHECK(conn(preZoom, fr) == 0, "x2 drum step: %d gaps on screen", conn(preZoom, fr));
        vbw = 400'000; for (int i = 0; i < 6; i++) fpush(vc, vbw);
        CHECK(offAll() == 0, "zoom out: %d rows off the live column (one-bin carrier lost?)", offAll());
        { const uint8_t* d = D(); int edge = 0;
          for (int f = 0; f < 60; f++) for (int x = 0; x < N / 4 - 1; x++) if (d[f * N + x] != 0 || d[f * N + N - 1 - x] != 0) edge++;
          CHECK(edge == 0, "zoom out: %d non-blank edge px", edge); }
        vbw = 200'000; fpush(vc, vbw);
        { const uint8_t* d = D(); int diff = 0; for (int i = 0; i < 60 * N; i++) if (d[i] != stored0[i]) diff++;
          CHECK(diff == 0, "zoom in then out: %d bytes differ from the stored history", diff); }
        CHECK(offAll() == 0, "after the round trip: %d rows off", offAll());
        { const double xs = liveCol(), span = 120'000, lo = CARRIER - xs * (span / N);
          vc = lo + span / 2; vbw = span; fpush(vc, vbw);
          CHECK(offAll() == 0 && std::fabs(liveCol() - xs) < 1e-6, "anchored zoom: %d rows off", offAll()); }
        vc = 1'030'000; vbw = 200'000; fpush(vc, vbw);
        { const int from = fr - 1;
          for (int i = 0; i < 12; i++) { vbw *= 0.95; fpush(vc, vbw); }
          CHECK(conn(from, fr) == 0 && offAll() == 0, "pinch: %d gaps, %d rows off", conn(from, fr), offAll()); }
        { const int from = fr; std::vector<double> cs;
          for (int i = 1; i <= 15; i++) { vc += 1'500; cs.push_back(vc); fpush(cs[std::max(0, (int)cs.size() - 3)], vbw); }
          int bad = 0;   // with zoomed history on screen, every moved pair's h predicts the displayed move
          for (int f = from; f < fr - 1; f++) { int a0, b0, a1, b1; if (!dlit(f, a0, b0) || !dlit(f + 1, a1, b1)) { bad++; continue; }
            if (std::fabs((a0 + gl.h[f % R]) - a1) > 1.01) bad++; }
          CHECK(bad == 0, "fan + motion: %d pairs whose h does not predict the displayed move", bad); }
        for (int i = 0; i < R; i++) fpush(vc, vbw);
        CHECK(fan.disp.empty() && D() == S.data(), "the display buffer was not freed once the zoomed history scrolled away");
        {   // cost: a full 1024-row ring, every row re-made per step
            const int RR = 1024; std::vector<uint8_t> SS((size_t)N * (RR + MOTION_ROWS)); MotionState g2(RR, N); FanState f2(RR); int k = 0;
            std::vector<uint8_t> row(N); for (int i = 0; i < N; i++) row[i] = (uint8_t)(40 + (i * 7919 % 50));
            for (int i = 0; i < RR; i++) placeRow(SS.data(), RR, N, k++, row.data(), 1e6, 200'000.0 / N, 1e6, 200'000, g2, f2);
            double bw = 200'000;
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20; i++) { bw *= 0.97; placeRow(SS.data(), RR, N, k++, row.data(), 1e6, bw / N, 1e6, bw, g2, f2); }
            auto t1 = std::chrono::steady_clock::now();
            for (int i = 0; i < 40; i++) { bw /= 0.97; placeRow(SS.data(), RR, N, k++, row.data(), 1e6, bw / N, 1e6, bw, g2, f2); }
            auto t2 = std::chrono::steady_clock::now();
            std::printf("  zoom fan cost (1024-row ring, native, this host): %.2f ms a zoom-in step, %.2f ms a zoom-out step\n",
                        std::chrono::duration<double, std::milli>(t1 - t0).count() / 20, std::chrono::duration<double, std::milli>(t2 - t1).count() / 40);
        }
    }
    std::printf("  wf_motion + wf_zoom_fan: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
