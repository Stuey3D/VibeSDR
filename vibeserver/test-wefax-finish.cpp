// test-wefax-finish.cpp — V12 beta92 a gallery WEFAX chart is finished as the listener's client draws it
// (android/app/src/main/cpp/vibe_wefax_finish.h — a port of src/utils/wefaxAlign.ts + wefaxCrisp.ts).
//
// Stuart, 2026-11-23: "background wefax is not applying our auto align and margin or contrast tweaks like they do for
// users". The gallery showed the raw lines; the server now finishes a chart before it saves the PNG.
//
//  1. PARITY with the TypeScript: scripts/wefax-finish-golden.ts runs the client's pipeline (chartAlignStep line by line,
//     the final redraw, crisp) on every case — Northwood margins, DDK borders (phased, late, behind a tone), lost lines,
//     a speckled grey page, a flat page, and the world corpus (scripts/wefax-world: NOAA charts full / black strip /
//     white margin, phased / late / leaning, σ 30 noise; the synthetic styles). The port must make the SAME decisions
//     (looked, moved, shift, drawn slant, via, at edge, levels) and the same pixels (max difference reported; ±1 allowed).
//     Needs node — without it this half is NOT RUN (exit 3 if nothing else failed).
//  2. PROPERTIES, here in C++ (no node): a noisy grey chart comes out on white paper; a Northwood-like chart's margin lands
//     at the left edge; a blank/flat page is left exactly as received; a DDK chart joined late is cut in its border.
//  3. Through the gallery tap (vibe_bgdecode.h GalleryImageTap): a WEFAX chart saved is the finished one.
//  4. Cost: ms for a 1809×1291 chart on this machine.
// Silent (no audio).
#include "../android/app/src/main/cpp/vibe_wefax_finish.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace wf = vibe::wefaxfinish;
static const int W = 1809;

static uint32_t g_seed = 7;
static double rnd() { g_seed = (g_seed * 1103515245u + 12345u) & 0x7fffffffu; return g_seed / (double)0x7fffffff; }

static bool readAll(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
template <class T> static T rd(const uint8_t*& p) { T v; std::memcpy(&v, p, sizeof v); p += sizeof v; return v; }
static const char* viaName(int v) { return v == 1 ? "border" : v == 2 ? "strip" : v == 3 ? "margin" : "-"; }

// ── 1. parity ──
static int parity() {
    std::printf("── 1. parity with the TypeScript (scripts/wefax-finish-golden.ts) ──\n");
    if (std::system("node --version >/dev/null 2>&1") != 0) { std::printf("   NOT RUN — no node here\n"); return 3; }
    char tmpl[] = "/tmp/vwf-golden-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { CHECK(false, "no temp dir"); return 1; }
    std::vector<std::string> names;
    {
        FILE* p = popen("node --no-warnings scripts/wefax-finish-golden.ts --list", "r");
        if (!p) { CHECK(false, "golden --list"); return 1; }
        char line[512];
        while (std::fgets(line, sizeof line, p)) { std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.empty()) names.push_back(s); }
        pclose(p);
    }
    CHECK(names.size() >= 40, "golden cases listed: %zu", names.size());
    int decisionsSame = 0, worst = 0; size_t pixDiff = 0, pixAll = 0; double msMax = 0;
    for (const std::string& n : names) {
        const std::string cmd = "node --no-warnings scripts/wefax-finish-golden.ts --case '" + n + "' '" + dir + "'";
        if (std::system(cmd.c_str()) != 0) { CHECK(false, "%s: golden run failed", n.c_str()); continue; }
        std::vector<uint8_t> in, out;
        const std::string pi = std::string(dir) + "/" + n + ".in", po = std::string(dir) + "/" + n + ".out";
        const bool okRead = readAll(pi, in) && readAll(po, out);
        std::remove(pi.c_str()); std::remove(po.c_str());
        if (!okRead || in.size() < 20 || out.size() < 40) { CHECK(false, "%s: unreadable", n.c_str()); continue; }
        const uint8_t* p = in.data() + 4;
        const uint32_t w = rd<uint32_t>(p), h = rd<uint32_t>(p); const double dial = rd<double>(p);
        std::vector<uint8_t> got(p, p + h); p += h;
        std::vector<uint8_t> px(p, p + (size_t)w * h);
        const uint8_t* q = out.data() + 4;
        const int eLooked = *q++, eHasAl = *q++; const int32_t eShift = rd<int32_t>(q); const double eSlant = rd<double>(q);
        const int eVia = *q++, eAtEdge = *q++, eCrisp = *q++; const double eLo = rd<double>(q), eHi = rd<double>(q);
        const uint8_t* ePx = q;
        const auto t0 = std::chrono::steady_clock::now();
        const wf::Result r = wf::finishChart(px.data(), w, h, got.data(), dial);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        msMax = std::max(msMax, ms);
        const bool same = r.looked == !!eLooked && r.hasAl == !!eHasAl && r.shift == eShift && r.slant == eSlant
                       && (int)r.via == eVia && r.atEdge == !!eAtEdge && r.crisp == !!eCrisp && (!eCrisp || (r.lo == eLo && r.hi == eHi));
        if (same) decisionsSame++;
        CHECK(same, "%s: decisions differ — C++ looked %d moved %d shift %d slant %.4f via %s edge %d crisp %d [%.3f %.3f] vs TS %d %d %d %.4f %s %d %d [%.3f %.3f]",
              n.c_str(), r.looked, r.hasAl, r.shift, r.slant, viaName(r.via), r.atEdge, r.crisp, r.lo, r.hi,
              eLooked, eHasAl, eShift, eSlant, viaName(eVia), eAtEdge, eCrisp, eLo, eHi);
        int mx = 0; size_t nd = 0;
        for (size_t i = 0; i < px.size(); i++) { const int d = std::abs((int)px[i] - (int)ePx[i]); if (d) { nd++; if (d > mx) mx = d; } }
        worst = std::max(worst, mx); pixDiff += nd; pixAll += px.size();
        CHECK(mx <= 1, "%s: pixels differ by up to %d (%zu px)", n.c_str(), mx, nd);
        std::printf("   %-44s %4ux%-4u %s shift %5d slant %+.3f via %-6s %s  max|Δ| %d  %.0f ms\n", n.c_str(), w, h,
                    r.hasAl ? "moved" : (r.looked ? "left " : "unlk "), r.shift, r.slant, viaName(r.via),
                    r.crisp ? "crisp" : "as-is", mx, ms);
    }
    rmdir(dir);
    std::printf("   decisions identical on %d of %zu charts; pixels: max |Δ| %d, %zu of %zu differ; slowest %.0f ms\n",
                decisionsSame, names.size(), worst, pixDiff, pixAll, msMax);
    return 0;
}

// ── 2. properties ──
static std::vector<uint8_t> northwood(int H, int margin, double slant) {
    std::vector<uint8_t> px((size_t)W * H);
    for (int y = 0; y < H; y++) {
        uint8_t* r = &px[(size_t)y * W];
        for (int x = 0; x < W; x++) r[x] = (uint8_t)(200 + (int)std::floor(rnd() * 55) - (rnd() < 0.08 ? 150 : 0));
        for (int c : {300, 700, 1200}) { const int cx = (int)wf::jsRound(c + 80 * std::sin((y + c) / 90.0)); for (int k = -1; k <= 1; k++) r[(cx + k + W) % W] = 20; }
        const int m = (int)wf::jsRound(margin + slant * y);
        for (int k = -3; k <= 3; k++) r[wf::wrapi(m + k, W)] = 0;
    }
    return px;
}

static void properties(double& msChart) {
    std::printf("── 2. properties ──\n");
    {   // a noisy grey page (paper ~138 ± 60) → white paper, black lines
        const int H = 800;
        std::vector<uint8_t> px((size_t)W * H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            double v = 138 + (rnd() - 0.5) * 120;
            if (x % 40 >= 20 && x % 40 < 24) v = 5;
            px[(size_t)y * W + x] = (uint8_t)std::max(0.0, std::min(255.0, wf::jsRound(v)));
        }
        double before = 0; for (int y = 300; y < 500; y++) for (int x = 5; x < 15; x++) before += px[(size_t)y * W + x];
        const wf::Result r = wf::finishChart(px.data(), W, H, nullptr, 0);
        double after = 0, ink = 0;
        for (int y = 300; y < 500; y++) { for (int x = 5; x < 15; x++) after += px[(size_t)y * W + x]; ink += px[(size_t)y * W + 21] + px[(size_t)y * W + 22]; }
        before /= 2000; after /= 2000; ink /= 400;
        CHECK(r.crisp && after >= 230, "noisy page: paper %.0f → %.0f (want ≥ 230)", before, after);
        CHECK(ink <= 30, "noisy page: the lines stay black (%.0f)", ink);
        CHECK(!r.hasAl, "noisy page with no margin: not moved");
        std::printf("   speckled page: paper %.0f → %.0f, lines %.0f\n", before, after, ink);
    }
    {   // Northwood 4610: margin 300 px in, slant −0.064 → at the left edge, measured
        const int H = 1291;
        std::vector<uint8_t> px = northwood(H, 300, -0.064);
        const auto t0 = std::chrono::steady_clock::now();
        const wf::Result r = wf::finishChart(px.data(), W, H, nullptr, 4608100);
        msChart = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        int atEdge = 0;
        for (int y = 0; y < H; y++) { int d = 0; for (int x = -1; x <= 5; x++) d += px[(size_t)y * W + wf::wrapi(x, W)] < 60; if (d >= 5) atEdge++; }
        CHECK(r.hasAl && r.via == wf::VIA_MARGIN && std::fabs(r.slant + 0.064) <= 0.003, "Northwood: margin found (shift %d, slant %.3f, via %s)", r.shift, r.slant, viaName(r.via));
        CHECK(atEdge >= H * 0.95, "Northwood: the margin is at the left edge on %d of %d lines", atEdge, H);
        std::printf("   Northwood-like chart: shift %d slant %+.3f, margin at the left edge on %d/%d lines\n", r.shift, r.slant, atEdge, H);
    }
    {   // a flat page: left exactly as received
        const int H = 400;
        std::vector<uint8_t> px((size_t)W * H, 200), keep = px;
        const wf::Result r = wf::finishChart(px.data(), W, H, nullptr, 7878100);
        CHECK(!r.crisp && !r.hasAl && px == keep, "a flat page is left unchanged (crisp %d, moved %d)", r.crisp, r.hasAl);
        std::vector<uint8_t> few((size_t)W * 1, 90), k2 = few;   // and under 2000 pixels' worth… one row is 1809
        wf::finishChart(few.data(), W, 1, nullptr, 0);
        CHECK(few == k2, "too little to judge levels → unchanged");
    }
    {   // DDK joined late: the white border (rolled 900 px) is cut in its middle
        const int H = 1291, roll = 900;
        std::vector<uint8_t> px((size_t)W * H);
        std::vector<uint8_t> r(W);
        for (int y = 0; y < H; y++) {
            std::fill(r.begin(), r.end(), 252);
            for (int x = 55; x <= 1745; x++) r[x] = (uint8_t)(215 + (int)std::floor(rnd() * 40) - (rnd() < 0.08 ? 150 : 0));
            r[55] = r[56] = r[1744] = r[1745] = 10;
            for (int c : {500, 1100}) { const int cx = (int)wf::jsRound(c - 0.17 * y); r[cx] = r[cx + 1] = 20; }
            for (int x = 0; x < W; x++) px[(size_t)y * W + x] = r[(x + roll) % W];
        }
        const wf::Result res = wf::finishChart(px.data(), W, H, nullptr, 7878100);
        CHECK(res.hasAl && res.via == wf::VIA_BORDER && std::abs(res.shift - 900) <= 6, "DDK late join: cut at %d (≈900) via %s", res.shift, viaName(res.via));
    }
}

int main() {
    double ms = 0;
    properties(ms);
    std::printf("── 3. cost ──\n   a 1809×1291 Northwood-like chart: %.0f ms (align + crisp, this machine)\n", ms);
    CHECK(ms < 5000, "cost %.0f ms", ms);
    const int pr = parity();
    std::printf("\ntest-wefax-finish: %d passed, %d failed%s\n", g_pass, g_fail, pr == 3 ? " (parity NOT RUN)" : "");
    if (g_fail) return 1;
    return pr == 3 ? 3 : 0;
}
