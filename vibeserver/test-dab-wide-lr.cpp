// test-dab-wide-lr.cpp — V12 DAB PHASE 2's pure rules (vibe_dab_wide.h): which blocks a locked wide capture holds, who
// qualifies (§11.0: 4 MS/s and two blocks), where a listener's block request lands, the station cap, and the radio's ONE
// PPM figure from several multiplexes (§11.2). Silent; exits non-zero on a failure.
#include "vibe_dab_wide.h"
#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { if (c) ++passes; else { ++fails; std::printf("  FAIL %s\n", what.c_str()); } }
static int idxOf(const char* name) {
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i) if (!std::strcmp(vibedab::kBandIII[i].name, name)) return i;
    return -1;
}
static std::string names(const std::vector<int>& v) {
    std::string s; for (int b : v) { if (!s.empty()) s += ","; s += vibedab::kBandIII[b].name; } return s;
}

int main() {
    using namespace vibedab::wide;
    const int c10C = idxOf("10C"), c10D = idxOf("10D"), c11A = idxOf("11A"), c11B = idxOf("11B"), c12B = idxOf("12B"), c5A = idxOf("5A");
    // ── the Lenovo's capture: RSP1A, 8 MS/s round 215.140 MHz (docs §11.4) ──
    const auto in = blocksIn(215140000.0, 8000000.0);
    auto has = [&](int b) { return std::find(in.begin(), in.end(), b) != in.end(); };
    ok(has(c10C) && has(c10D) && has(c11A), "the Lenovo capture holds 10C, 10D, 11A (" + names(in) + ")");
    ok(names(in) == "10C,10D,11A", "★ 11N (the N raster, 160 kHz from 11A) is not offered beside 11A: " + names(in));
    ok(std::find(in.begin(), in.end(), c11B) == in.end(), "11B (half off the edge) is not offered");
    ok(eligible(215140000.0, 8000000.0), "8 MS/s with three blocks: a wide locked range");
    ok(!eligible(215140000.0, 2048000.0) && !eligible(225648000.0, 2400000.0), "★ §11.0: 2.048 / 2.4 MS/s never qualify");
    ok(!eligible(215140000.0, 3500000.0), "★ §11.0: under 4 MS/s never qualifies, however many blocks");
    ok(!eligible(100000000.0, 8000000.0), "a capture with no Band III block (FM) does not qualify");
    ok(blocksIn(225648000.0, 4000000.0).size() == 1 && !eligible(225648000.0, 4000000.0), "4 MS/s round 12B holds ONE block: the ordinary DAB mode's job");
    ok(blocksIn(0, 8e6).empty() && blocksIn(215e6, 0).empty(), "no centre or rate: nothing");

    // ── where a request lands ──
    const std::vector<int> L = { c10C, c10D, c11A };
    ok(choose(c10D, -1, L) == c10D, "entering on a block inside → that block");
    ok(choose(c12B, -1, L) == c11A, "entering on a block outside (12B) → the nearest inside (11A)");
    ok(choose(c5A, -1, L) == c10C, "entering far below (5A) → the lowest inside (10C)");
    ok(choose(-1, -1, L) == c10D, "entering with no block named → the middle one");
    ok(choose(c11B, c11A, L) == c11A, "★ stepping up past the last block inside: stay (the keys know nothing of the capture)");
    ok(choose(c11B, c10D, L) == c11A, "★ stepping up from 10D to a block outside: the next inside that way (11A)");
    ok(choose(c10C - 1, c10C, L) == c10C, "stepping down past the first: stay");
    ok(choose(c5A, c11A, L) == c10D, "stepping down from 11A to a block outside: the next inside below (10D)");
    ok(choose(c11A, c10C, L) == c11A, "a block inside is taken as asked, from anywhere");
    ok(choose(-1, c10D, L) == c10D, "already in, no block named: stay");
    ok(choose(c10C, -1, {}) == -1, "a capture with no block: -1");

    // ── the station cap ──
    ok(stationCap(0, 16, 8) == 0, "owner off: no stations (a pick moves that multiplex's own for everyone ON IT)");
    ok(stationCap(-1, 16, 8) == 15, "the rule on (N 16 ≥ limit 8): N − 1, as Phase 1");
    ok(stationCap(-1, 8, 65) == 0, "the rule off (N 8 < limit 65): none");
    ok(stationCap(1, 8, 65) == 64, "★ forced on in a locked range: a station per listener (limit − 1), not Phase 1's 16");
    ok(stationCap(-1, 0, 4) == 0, "not measured: off");

    // ── one PPM figure for the radio (§11.2) ──
    double ppm = 0; int dropped = 0;
    ok(weightedMedianPpm({ { 1.2, 18.9 }, { 1.3, 9.2 }, { 1.25, 23.7 } }, 2.0, ppm, dropped) && std::fabs(ppm - 1.25) < 1e-9 && dropped == 0,
       "three multiplexes agree: the MER-weighted median (11A's, the strongest)");
    ok(weightedMedianPpm({ { 1.2, 18.9 }, { 9.0, 9.2 }, { 1.25, 23.7 } }, 2.0, ppm, dropped) && dropped == 1 && std::fabs(ppm - 1.25) < 1e-9,
       "★ a transmitter 8 ppm off frequency is dropped and counted, never followed");
    ok(!weightedMedianPpm({ { 1.0, 0.0 } }, 2.0, ppm, dropped), "no MER: no reading");
    ok(!weightedMedianPpm({}, 2.0, ppm, dropped), "no multiplex: no reading");

    std::printf("%s  test-dab-wide-lr: %d passed, %d failed\n", fails ? "FAIL" : "ok", passes, fails);
    return fails ? 1 : 0;
}
