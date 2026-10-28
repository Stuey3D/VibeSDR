// Host test for wf_glide.h — the same cases as the glide section of scripts/test_wf_row_place.ts.
//   clang++ -std=c++17 -O2 test_wf_glide.cpp -o /tmp/test_wf_glide && /tmp/test_wf_glide
#include "wf_glide.h"
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
    {   // ×2 zoom step: not streaked; pinch: connected
        double a, b;
        CHECK(!glideMap(0, 195.3125, 0, 97.65625, N, a, b), "x2 zoom streaked");
        double w = 1'000'000, bw = 200'000;
        for (int i = 0; i < 3; i++) gpush(noisyRow(w, bw), w, bw, w, bw);
        const int from = gf - 1;
        for (int i = 0; i < 12; i++) { bw *= 0.95; gpush(noisyRow(w, bw), w, bw, w, bw); }
        CHECK(gaps(from, gf) == 0, "pinch: %d gaps", gaps(from, gf));
    }
    CHECK(std::memcmp(before.data(), G.data(), before.size()) == 0, "history before the tune was rewritten");
    std::printf("  wf_glide: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
