// Host test for wf_motion.h + wf_zoom_fan.h + wf_wide.h — the same cases as the motion, zoom-fan and full-capture-layer
// sections of scripts/test_wf_row_place.ts (and scripts/test_wf_wide_app.ts for the layer's pure parts).
//   clang++ -std=c++17 -O2 test_wf_motion.cpp -o /tmp/test_wf_motion && /tmp/test_wf_motion
#include "wf_motion.h"
#include "wf_zoom_fan.h"   // + wf_wide.h (V12 the full-capture layer)
#include <algorithm>
#include <array>
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
        { double k0, c0; takeFanStep(mf, k0, c0); }   // (the jump above's step, not this pinch's)
        const int from = mfr; double bw = vbw;
        for (int i = 0; i < 12; i++) { bw *= 0.95; mpush(noisyRow(vc, bw), vc, bw, vc, bw); }
        int bad = 0; for (int f = from - 1; f < mfr - 1; f++) if (!(ms.g[f % MR] > 1.0005) || std::fabs(ms.h[f % MR]) > 1.0 / 64) bad++;
        CHECK(bad == 0, "pinch: %d pairs without a spreading zoom (g > 1, h = 0)", bad);
        const uint8_t* d = wfImageBytes(mf, ms, M.data(), N, MR); bool same = true;
        for (int r = 0; r < MR; r++) if (std::fabs(decodeZoom(d[(MR + 2) * N + r], d[(MR + 3) * N + r]) - ms.g[r]) > 2e-4 * ms.g[r]) same = false;
        CHECK(same, "pinch: the image does not carry g");
        double k, c; takeFanStep(mf, k, c);
        CHECK(k == 1 && c == 0, "pinch: a screen ease (%g %g) — the history must not move", k, c);
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
        // ★★★ A ZOOM LEAVES THE HISTORY AS DRAWN (Stuart 2026-10-28) — the zoom section of scripts/test_wf_row_place.ts
        int ha, hb; dlit(10, ha, hb); const int histCol = ha;
        auto atHist = [&](int f0, int f1) { int bad = 0; for (int f = f0; f < f1; f++) { int a, b; if (!dlit(f, a, b) || a != histCol || b != histCol) bad++; } return bad; };
        auto asDrawn = [&](int f0, int f1) { const uint8_t* d = D(); int k = 0; for (size_t i = (size_t)f0 * N; i < (size_t)f1 * N; i++) if (d[i] != S[i]) k++; return k; };
        vbw = 100'000; fpush(1'030'000, 200'000);                         // one old-span frame in flight
        const int zf = fr;
        for (int i = 0; i < 24; i++) fpush(vc, vbw);
        CHECK(atHist(0, 60) == 0 && asDrawn(0, 60) == 0, "x2 zoom in: the history moved (%d rows)", atHist(0, 60));
        CHECK(std::memcmp(stored0.data(), S.data(), stored0.size()) == 0, "a zoom rewrote the stored rows");
        CHECK(offCol(zf + ZOOM_EASE_ROWS - 1, fr) == 0, "x2 zoom in: %d eased-in rows off the live column", offCol(zf + ZOOM_EASE_ROWS - 1, fr));
        { std::vector<int> cs; for (int f = zf - 1; f < zf + ZOOM_EASE_ROWS; f++) { int a, b; dlit(f, a, b); cs.push_back(a); }
          bool mono = true; int mx = 0; for (size_t i = 1; i < cs.size(); i++) { if (!(cs[i] > cs[i - 1])) mono = false; mx = std::max(mx, cs[i] - cs[i - 1]); }
          CHECK(mono && mx < 20 && cs[1] - cs[0] <= 3 && cs.back() - cs[cs.size() - 2] <= 3, "x2 zoom in: the ease is not an S-curve fan (%d .. %d)", cs.front(), cs.back()); }
        { double k, c; takeFanStep(fan, k, c); CHECK(k == 1 && c == 0, "x2 zoom: a screen ease (%g %g)", k, c); }
        vbw = 400'000; for (int i = 0; i < 30; i++) fpush(vc, vbw);
        CHECK(atHist(0, 60) == 0 && offCol(fr - 4, fr) == 0 && asDrawn(0, 60) == 0, "zoom out: history moved or new rows off (%d, %d)", atHist(0, 60), offCol(fr - 4, fr));
        vbw = 200'000; for (int i = 0; i < 30; i++) fpush(vc, vbw);
        { const int from = fr - 1;
          for (int i = 0; i < 12; i++) { vbw *= 0.95; fpush(vc, vbw); }
          int pred = 0, moved = 0;
          for (int f = from; f < fr - 1; f++) { int pa, pb, qa, qb; dlit(f, pa, pb); dlit(f + 1, qa, qb);
            const double p = (pa + pb + 1) / 2.0, q = (qa + qb + 1) / 2.0; const int r = f % R;
            if (std::fabs(N / 2.0 + gl.g[r] * (p - N / 2.0) + gl.h[r] - q) > 1.5) pred++; if (q != p) moved++; }
          CHECK(pred == 0 && atHist(0, 60) == 0 && moved >= 10, "pinch: %d steps (g, h) does not predict, %d moved", pred, moved); }
        for (int i = 0; i < 30; i++) fpush(vc, vbw);
        vc += vbw * 0.4; const int preJump = fr; fpush(vc, vbw);
        CHECK(offCol(preJump - 6, fr) == 0 && !fan.disp.empty(), "jump: %d rows not moved to the live column", offCol(preJump - 6, fr));
        { const int from = fr; std::vector<double> cs;
          for (int i = 1; i <= 15; i++) { vc += 1'500; cs.push_back(vc); fpush(cs[std::max(0, (int)cs.size() - 3)], vbw); }
          int bad = 0;   // with moved history on screen, every moved pair's h predicts the displayed move
          for (int f = from; f < fr - 1; f++) { int a0, b0, a1, b1; if (!dlit(f, a0, b0) || !dlit(f + 1, a1, b1)) { bad++; continue; }
            if (std::fabs((a0 + gl.h[f % R]) - a1) > 1.01) bad++; }
          CHECK(bad == 0, "jump + motion: %d pairs whose h does not predict the displayed move", bad); }
        for (int i = 0; i < R; i++) fpush(vc, vbw);
        CHECK(fan.disp.empty() && D() == S.data(), "the display buffer was not freed once the moved history scrolled away");
        {   // cost: a full 1024-row ring, every row re-made per step
            const int RR = 1024; std::vector<uint8_t> SS((size_t)N * (RR + MOTION_ROWS)); MotionState g2(RR, N); FanState f2(RR); int k = 0;
            std::vector<uint8_t> row(N); for (int i = 0; i < N; i++) row[i] = (uint8_t)(40 + (i * 7919 % 50));
            for (int i = 0; i < RR; i++) placeRow(SS.data(), RR, N, k++, row.data(), 1e6, 200'000.0 / N, 1e6, 200'000, g2, f2);
            double cc = 1e6;
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20; i++) { cc += (i % 2 ? -1 : 1) * 60'000; placeRow(SS.data(), RR, N, k++, row.data(), cc, 200'000.0 / N, cc, 200'000, g2, f2); }
            auto t1 = std::chrono::steady_clock::now();
            std::printf("  jump cost (1024-row ring, native, this host): %.2f ms a jump\n", std::chrono::duration<double, std::milli>(t1 - t0).count() / 20);
        }
    }
    // ── V12 THE FULL-CAPTURE LAYER (wf_wide.h) — the same cases as scripts/test_wf_wide_app.ts and the layer section of
    //    scripts/test_wf_row_place.ts: rows recorded while zoomed in carry a layer row of the whole capture, drawn through
    //    the SAME per-row map wherever the detailed row has no data ──
    {
        const double CAPC = 1'500'000, CAPS = 2'400'000, CLO = CAPC - CAPS / 2, CAR = 1'050'000;
        const int C = 1024, F = 128, R = 256;
        // an SPWB v2 frame: the floor bytes, its spread, peaks (cell, level byte, shape) — delta-coded with the escape
        auto spwb = [&](uint8_t floorB, uint8_t spreadB, std::vector<std::array<int, 3>> pk, double centre = 1'500'000, int ver = 2) {
            std::vector<uint8_t> trip; int prev = -1;
            for (auto& p : pk) { int d = p[0] - prev; while (d > 255) { trip.push_back(255); trip.push_back(0); trip.push_back(0); d -= 255; }
                                 trip.push_back((uint8_t)d); trip.push_back((uint8_t)p[1]); trip.push_back((uint8_t)p[2]); prev = p[0]; }
            std::vector<uint8_t> u(WIDE_HDR + 2 * F, 0);
            u[0] = 'S'; u[1] = 'P'; u[2] = 'W'; u[3] = 'B'; u[4] = (uint8_t)ver;
            const uint64_t c = (uint64_t)centre; for (int i = 0; i < 8; i++) u[14 + i] = (uint8_t)(c >> (8 * i));
            const uint32_t sp = (uint32_t)CAPS; for (int i = 0; i < 4; i++) u[22 + i] = (uint8_t)(sp >> (8 * i));
            u[26] = F & 255; u[27] = F >> 8; u[28] = C & 255; u[29] = C >> 8;
            const int P = (int)trip.size() / 3; u[30] = P & 255; u[31] = P >> 8;
            for (int i = 0; i < F; i++) { u[WIDE_HDR + i] = floorB; u[WIDE_HDR + F + i] = spreadB; }
            u.insert(u.end(), trip.begin(), trip.end());
            return u;
        };
        auto carrierPeak = [&]() { const double cf = (CAR - CLO) / CAPS * C; const int cell = (int)std::floor(cf);
                                   return std::array<int, 3>{cell, 196, std::min(15, (int)std::floor((cf - cell) * 16))}; };
        // the parser
        {
            WideRow w;
            auto b = spwb(146, 28, {{100, 200, 7}, {700, 180, (11 << 4) | 8}});
            b[WIDE_HDR] = 0;
            CHECK(parseWide(b.data(), b.size(), w) && w.centreHz == CAPC && w.spanHz == CAPS && w.floor.size() == (size_t)F && w.floor[1] == -110
                  && std::isnan(w.floor[0]) && std::fabs(w.spread[3] - 2.8) < 1e-6, "SPWB v2 parses: centre, span, floor (0 = no data), spread");
            CHECK(w.nPk == 2 && w.pkCell[1] == 700 && w.pkDb[0] == -56 && w.pkShape[1] == ((11 << 4) | 8), "the peaks, across an escape (%d)", w.nPk);
            auto hi = spwb(146, 28, {}, 10'368'000'000.0); CHECK(parseWide(hi.data(), hi.size(), w) && w.centreHz == 10'368'000'000.0, "a centre past 4 GHz");
            auto v1 = spwb(146, 28, {}, CAPC, 1); CHECK(!parseWide(v1.data(), v1.size(), w), "a v1 frame is refused");
            auto sp = spwb(146, 28, {}); sp[3] = 'C'; CHECK(!isWideFrame(sp.data(), sp.size()) && !parseWide(sp.data(), sp.size(), w), "SPEC is not SPWB");
            const ToIdx t(-112, -82, 0, 0);
            CHECK(t(-111) == 0 && floorIdx(-111, 3, t) > 0 && floorIdx(-111, 0, t) == 0, "the floor's colour is its speckle's average (%d)", floorIdx(-111, 3, t));
            std::vector<float> a(4097), sc; for (size_t i = 0; i < a.size(); i++) a[i] = (float)(-110 + 20 * rnd());
            std::vector<float> s2(a); std::sort(s2.begin(), s2.end());
            CHECK(medianOf(a.data(), (int)a.size(), sc) == s2[s2.size() / 2], "the median is the sort's");
        }
        const ToIdx toIdx(-125, -50, 0, 0);
        struct Run { std::vector<uint8_t> S; MotionState gl; FanState fan; int fr = 0; double t = 0; WideState st; double vc, vbw; bool wide;
                     double lastLo = NAN, lastSpan = 0;
                     Run(int R, bool w) : S((size_t)N * (R + MOTION_ROWS), 0), gl(R, N), fan(R), vc(1'500'000), vbw(2'400'000), wide(w) {} };
        std::vector<float> sc;
        auto push = [&](Run& X, int visible = 256) {
            std::vector<float> db(N); for (int i = 0; i < N; i++) { double z = 0; for (int k = 0; k < 6; k++) z += rnd(); db[i] = (float)(-110 + 3 * (z - 3) * std::sqrt(2.0)); }
            const int bin = (int)std::floor((CAR - (X.vc - X.vbw / 2)) / (X.vbw / N)); if (bin >= 0 && bin < N) db[bin] = -60;
            std::vector<uint8_t> row(N); for (int i = 0; i < N; i++) row[i] = (uint8_t)toIdx(db[i]);
            if (X.wide && X.fr % 4 == 0 && X.vbw < CAPS * 0.98) {
                auto b = spwb(146, 30, {carrierPeak()}); WideRow w; parseWide(b.data(), b.size(), w);
                wideArrive(X.st, w, X.t, X.lastLo, X.lastSpan, medianOf(db.data(), N, sc));
                if (!X.fan.layer) X.fan.layer.reset(new LayerRing(R, F, C));
            }
            double start = NAN;
            const int slot = placeRow(X.S.data(), R, N, X.fr, row.data(), X.vc, X.vbw / N, X.vc, X.vbw, X.gl, X.fan, visible, &start);
            X.lastLo = start; X.lastSpan = X.vbw;
            if (X.fan.layer) { layerWriteRow(*X.fan.layer, slot, &X.st, start, X.vbw, X.t, ToIdx(-125, -50, 0, X.st.off)); fanRow(X.fan, X.S.data(), N, slot, R); }
            X.fr++; X.t += 50;
        };
        auto D = [&](Run& X) { return wfImageBytes(X.fan, X.gl, X.S.data(), N, R); };
        auto liveCol = [&](Run& X) { return (CAR - (X.vc - X.vbw / 2)) / (X.vbw / N); };
        struct Scan { int atCol, black, widest; };
        auto scan = [&](Run& X, int f0, int f1) {
            const uint8_t* d = D(X); const double want = liveCol(X); Scan s{0, 0, 0};
            for (int f = f0; f < f1; f++) {
                const uint8_t* r = d + (size_t)(f % R) * N; int a = -1, b = -1, z = 0;
                for (int x = 0; x < N; x++) { if (r[x] >= 150) { if (a < 0) a = x; b = x; } if (r[x] == 0) z++; }
                if (a >= 0 && a + 0.5 >= want - 1.5 && b + 0.5 <= want + 1.5) s.atCol++;
                if (z) s.black++;
                if (a >= 0) s.widest = std::max(s.widest, b - a + 1);
            }
            return s;
        };
        auto zoomInOut = [&](Run& X, double zc, int rows, int visible = 256) {
            for (int i = 0; i < 20; i++) push(X);
            X.vc = zc; X.vbw = 100'000; const int f0 = X.fr;
            for (int i = 0; i < rows; i++) push(X, visible);
            X.vc = CAPC; X.vbw = CAPS; push(X, visible);
            return f0;
        };
        const int EZ = zoomEaseRows(100'000 / CAPS);   // the zoom-in's eased rows: drawn at in-between zooms (the fan)
        // a zoom in and out no longer re-maps (Stuart 2026-10-28): the zoomed rows past the ease stay as drawn, no black
        {
            Run W(R, true); const int z0 = zoomInOut(W, 1'400'000, 60);
            const uint8_t* d = D(W); int diff = 0;
            for (int f = z0 + EZ; f < z0 + 60; f++) for (int x = 0; x < N; x++) if (d[(size_t)(f % R) * N + x] != W.S[(size_t)(f % R) * N + x]) diff++;
            CHECK(diff == 0 && scan(W, z0, z0 + 60).black == 0, "the layer: zoomed rows not as drawn past the ease (%d texels) or black in the ease", diff);
        }
        // a pan across the capture at the narrow zoom
        {
            Run P(R, true); for (int i = 0; i < 4; i++) push(P);
            P.vc = 1'200'000; P.vbw = 100'000; const int p0 = P.fr; for (int i = 0; i < 40; i++) push(P);
            P.vc = 1'500'000; push(P);
            CHECK(scan(P, p0 + EZ, P.fr - 1).black == 0, "the layer: a pan across the capture — no black in the history");
            P.vc = CAR + 20'000; push(P);
            const Scan s = scan(P, p0 + EZ, P.fr - 1);
            CHECK(s.atCol == P.fr - 1 - (p0 + EZ) && s.black == 0, "the layer: panned onto the carrier, every row past the ease shows it (%d/%d)", s.atCol, P.fr - 1 - (p0 + EZ));
        }
        // rotation / resize: portrait (32 rows made) → landscape (all) → portrait
        {
            Run Q(R, true); for (int i = 0; i < 4; i++) push(Q);
            Q.vc = 1'200'000; Q.vbw = 100'000; const int q0 = Q.fr + EZ; for (int i = 0; i < 60; i++) push(Q, 32);
            Q.vc = CAR + 20'000; push(Q, 32); const int q1 = Q.fr - 1;
            const int before = scan(Q, q0, q1).atCol;
            // (the engine always makes the whole ring; the JS path's fanRefresh does this for a taller view)
            if (!Q.fan.disp.empty() && Q.fr - std::min(R, Q.fr) < Q.fan.validFrom) fanRegen(Q.fan, Q.S.data(), N, R, Q.fr, R);
            const Scan s = scan(Q, q0, q1);
            CHECK(before < q1 - q0 && s.atCol == q1 - q0 && s.black == 0, "the layer: portrait → landscape keeps both layers (%d then %d/%d, %d black)", before, s.atCol, q1 - q0, s.black);
        }
        // the fill's cost on this host: a full 1024-row ring, every row re-made at a 12× zoom out
        {
            const int RR = 1024; LayerRing L2(RR, F, C); WideState s2; WideRow w;
            std::vector<std::array<int, 3>> many; for (int i = 0; i < 110; i++) many.push_back({i * 9 + 3, 190, 7});
            auto b = spwb(146, 28, many); parseWide(b.data(), b.size(), w); wideArrive(s2, w, 0, 1'350'000, 100'000, -110);
            auto t0 = std::chrono::steady_clock::now();
            for (int r = 0; r < RR; r++) layerWriteRow(L2, r, &s2, 1'350'000, 100'000, 10, toIdx);
            auto t1 = std::chrono::steady_clock::now();
            std::vector<uint8_t> d2((size_t)RR * N); std::vector<int32_t> lo2(N);
            for (int x = 0; x < N; x++) { const double c = std::floor(12 * (x + 0.5) - 5.5 * N); lo2[x] = c >= 0 && c < N ? (int)c : -1; }
            for (int r = 0; r < RR; r++) layerFillRow(L2, r, 12, -5.5, N, d2.data() + (size_t)r * N, lo2.data());
            auto t2 = std::chrono::steady_clock::now();
            std::printf("  layer cost (native, this host): %.2f us a row written, %.2f us a row filled (12x zoom out); RAM %zu KiB for %d rows\n",
                        std::chrono::duration<double, std::micro>(t1 - t0).count() / RR, std::chrono::duration<double, std::micro>(t2 - t1).count() / RR,
                        (size_t)(F + 4 * L2.P + 16) * RR / 1024, RR);
        }
    }
    std::printf("  wf_motion + wf_zoom_fan: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
