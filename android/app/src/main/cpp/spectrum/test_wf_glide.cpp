// Host test for wf_glide.h + wf_zoom_fan.h — the same cases as the glide and zoom-fan sections of
// scripts/test_wf_row_place.ts.
//   clang++ -std=c++17 -O2 test_wf_glide.cpp -o /tmp/test_wf_glide && /tmp/test_wf_glide
#include "wf_glide.h"
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
static std::vector<uint8_t> G(N * ROWS);
static GlideState st(N);
static int gf = 0;
static unsigned seed = 7;
static double rnd() { seed = (seed * 1103515245u + 12345u) & 0x7fffffffu; return seed / (double)0x7fffffff; }

static std::vector<uint8_t> noisyRow(double centre, double span) {
    std::vector<uint8_t> r(N);
    for (int i = 0; i < N; i++) r[i] = (uint8_t)(34 + (int)std::floor(rnd() * 13));
    const int bin = (int)std::floor((CARRIER - centre) / (span / N) + 0.5) + N / 2;
    for (int d = -2; d <= 2; d++) if (bin + d >= 0 && bin + d < N) r[bin + d] = d == 0 ? 230 : 200;
    return r;
}
// rowViewOffset + writeRow, as spectrum_engine.cpp pushRow does them
static double gpush(const std::vector<uint8_t>& row, double rc, double span, double vc, double vbw, bool glide = true) {
    uint8_t* dst = G.data() + (size_t)(gf % ROWS) * N;
    const double hpb = span / N; double off = 0;
    if (std::fabs(hpb - vbw / N) <= (vbw / N) * 1e-6) { off = std::floor((rc - vc) / hpb + 0.5); if (std::fabs(off) >= N) off = 0; }
    if (off == 0) std::memcpy(dst, row.data(), N);
    else { std::memset(dst, 0, N); const int o = (int)off; if (o > 0) std::memcpy(dst + o, row.data(), N - o); else std::memcpy(dst, row.data() - o, N + o); }
    const double sp = glide ? glideRow(dst, N, rc, hpb, off, st) : 0;
    gf++;
    return sp;
}
static bool lit(int f, int& a, int& b) {
    const uint8_t* s = G.data() + (size_t)(f % ROWS) * N; a = b = -1;
    for (int i = 0; i < N; i++) if (s[i] >= 200) { if (a < 0) a = i; b = i; }
    return a >= 0;
}
static int gaps(int from, int to) {
    int g = 0;
    for (int f = from + 1; f < to; f++) {
        int pa, pb, qa, qb;
        if (!lit(f - 1, pa, pb) || !lit(f, qa, qb)) { g++; continue; }
        if (qa > pb + 1 || qb < pa - 1) g++;
    }
    return g;
}

int main() {
    double vc = 1'000'000, vbw = 200'000;
    for (int i = 0; i < 10; i++) gpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
    std::vector<uint8_t> before(G.begin(), G.begin() + 10 * N);
    {   // steady row: byte-for-byte
        seed = 99; auto ref = noisyRow(vc, vbw);
        const size_t slot = (size_t)(gf % ROWS) * N;
        seed = 99; gpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
        CHECK(std::memcmp(ref.data(), G.data() + slot, N) == 0, "steady row changed");
    }
    {   // fast tune, rows built for the view two back
        const int from = gf; std::vector<double> cs;
        for (int i = 1; i <= 15; i++) {
            vc += 7'500; cs.push_back(vc);
            const double rc = cs[std::max(0, (int)cs.size() - 3)];
            gpush(noisyRow(rc, 200'000), rc, 200'000, vc, vbw);
        }
        for (int i = 0; i < 10; i++) gpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
        CHECK(gaps(from, gf) == 0, "fast tune: %d gaps", gaps(from, gf));
        const int want = N / 2 + (int)std::floor((CARRIER - vc) / (200'000.0 / N) + 0.5);
        bool straight = true;
        for (int f = gf - 9; f < gf; f++) { int a, b; if (!lit(f, a, b) || a != want - 2 || b != want + 2) straight = false; }
        CHECK(straight, "trail does not end straight at %d", want);
    }
    {   // control: no glide → dots
        double w = 1'000'000; st.has = false;
        for (int i = 0; i < 3; i++) gpush(noisyRow(w, vbw), w, vbw, w, vbw, false);
        const int from = gf; std::vector<double> cs;
        for (int i = 1; i <= 15; i++) { w += 7'500; cs.push_back(w); const double rc = cs[std::max(0, (int)cs.size() - 3)]; gpush(noisyRow(rc, 200'000), rc, 200'000, w, vbw, false); }
        CHECK(gaps(from, gf) >= 12, "control saw only %d gaps", gaps(from, gf));
        st.has = false;
    }
    {   // floor never raised
        double w = 1'000'000; int raised = 0;
        for (int i = 0; i < 3; i++) gpush(noisyRow(w, vbw), w, vbw, w, vbw);
        for (int i = 1; i <= 20; i++) {
            w -= 5'000; seed = 1000 + i; auto ref = noisyRow(w, vbw);
            const size_t slot = (size_t)(gf % ROWS) * N;
            seed = 1000 + i; gpush(noisyRow(w, vbw), w, vbw, w, vbw);
            for (int x = 0; x < N; x++) if (G[slot + x] != ref[x] && G[slot + x] < 200) raised++;
        }
        CHECK(raised == 0, "%d floor columns changed", raised);
    }
    {   // jump past GLIDE_MAX_FRAC: not streaked
        double w = 1'000'000;
        for (int i = 0; i < 3; i++) gpush(noisyRow(w, vbw), w, vbw, w, vbw);
        w += 200'000 * GLIDE_MAX_FRAC * 1.5;
        const double sp = gpush(noisyRow(w, vbw), w, vbw, w, vbw);
        int a, b; const bool l = lit(gf - 1, a, b);
        CHECK(sp == 0 && l && b - a == 4, "jump streaked: span %g", sp);
    }
    {   // a zoom is never streaked — ×2 step or 5 % pinch: the fan places the history (no double scaling)
        double a, b;
        CHECK(!glideMap(0, 195.3125, 0, 97.65625, N, a, b), "x2 zoom streaked");
        CHECK(!glideMap(0, 195.3125, 0, 195.3125 * 0.95, N, a, b), "pinch streaked");
        double w = 1'000'000, bw = 200'000; int changed = 0;
        for (int i = 0; i < 3; i++) gpush(noisyRow(w, bw), w, bw, w, bw);
        for (int i = 0; i < 12; i++) {
            bw *= 0.95; seed = 500 + i; auto ref = noisyRow(w, bw);
            const size_t slot = (size_t)(gf % ROWS) * N;
            seed = 500 + i; if (gpush(noisyRow(w, bw), w, bw, w, bw) != 0) changed++;
            if (std::memcmp(ref.data(), G.data() + slot, N) != 0) changed++;
        }
        CHECK(changed == 0, "pinch rows not stored as written (%d)", changed);
    }
    CHECK(std::memcmp(before.data(), G.data(), before.size()) == 0, "history before the tune was rewritten");
    // ── THE ZOOM FAN (wf_zoom_fan.h) — the same cases as the zoom-fan section of scripts/test_wf_row_place.ts ──
    {
        const int R = 256;
        std::vector<uint8_t> S((size_t)N * R, 0);
        GlideState gl(N); FanState fan(R);
        int fr = 0;
        double vc = 1'030'000, vbw = 200'000;
        auto fpush = [&](double rc, double span) {
            std::vector<uint8_t> r(N, 40);
            const int bin = (int)std::floor((CARRIER - rc) / (span / N) + 0.5) + N / 2;
            if (bin >= 0 && bin < N) r[bin] = 230;
            placeRow(S.data(), R, N, fr, r.data(), rc, span / N, vc, vbw, gl, fan); fr++;
        };
        auto D = [&]() { return fanImage(fan, S.data()); };
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
          CHECK(conn(from - 1, fr) == 0, "fan + glide: a fast tune has %d gaps on screen", conn(from - 1, fr)); }
        for (int i = 0; i < R; i++) fpush(vc, vbw);
        CHECK(fan.disp.empty() && D() == S.data(), "the display buffer was not freed once the zoomed history scrolled away");
        {   // cost: a full 1024-row ring, every row re-made per step
            const int RR = 1024; std::vector<uint8_t> SS((size_t)N * RR); GlideState g2(N); FanState f2(RR); int k = 0;
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
    std::printf("  wf_glide + wf_zoom_fan: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
