// test-vfo-dc.cpp — A FREE VFO AND THE TUNER'S DC (V12, 2026-10-27, branch v12-dc-nudge; vibe_vfo_dc.h vfoDcCentre).
//
// Stuart: "we know where a user is tuning and we know where the rf centre is so as the user approaches the DC spike in Free
// Tune mode just nudge the RF centre so the spike moves away from the listen VFO". The placement, checked on its own:
//   1. the zone: the passband (USB 0..+bw, LSB −bw..0, CW/AM/NFM ±bw/2) plus the 2.5 kHz guard — never stricter than the
//      LOCKED VFO (WFM on a dongle: ±15 kHz, the offset itself);
//   2. the LOCKED arrangement (centre on the dial, DC at dial + offset) never moves, in any mode, on any radio;
//   3. a sweep across the DC in 100 Hz steps, every mode, dongle and HackRF offsets: the DC is never in the passband + guard
//      after placement, ONE retune per crossing (behind the travel), two for there-and-back;
//   4. hysteresis: a VFO jittering at the edge, or wandering at random, costs a handful of retunes, not one per step;
//   5. a jump onto the DC: the nearer side; the caller's hard limits are never broken (the other side, or no move);
//      the view's room is preferred; VFO B's zone is never given the DC.
// SILENT, pure arithmetic.
#include "../android/app/src/main/cpp/vibe_vfo_dc.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>

using namespace vibe;
static int pass = 0, fail = 0;
static void ok(bool c, const std::string& what) { (c ? pass : fail)++; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str()); }

struct Mode { const char* name; double bw; double pbLo, pbHi; };
static Mode mode(const char* n, double bw) {
    const std::string s = n;
    if (s == "usb") return { n, bw, 0, bw };
    if (s == "lsb") return { n, bw, -bw, 0 };
    return { n, bw, -bw / 2, bw / 2 };   // cw (channel at −bw/2, 0..+bw of it), am, nfm, wfm
}
static const double INF = 1e18;

// The DC is clear of the listener when it is outside the passband + guard — or, where the radio's own offset sits inside
// that (WFM on a dongle), at least the offset away (what the LOCKED VFO gives).
static bool clear(double dc, double dial, const Mode& m, double off) {
    const VfoDcZone z = vfoDcZone(dial, m.pbLo, m.pbHi, off);
    return dc <= z.lo + 1e-6 || dc >= z.hi - 1e-6;
}

int main() {
    std::printf("── 1. the zone ──\n");
    {
        VfoDcZone z = vfoDcZone(7.1e6, 0, 2800, 15000);
        ok(std::fabs(z.lo - (7.1e6 - 2500)) < 1 && std::fabs(z.hi - (7.1e6 + 5300)) < 1, "USB 2.8 kHz: dial −2.5 .. +5.3 kHz");
        z = vfoDcZone(7.1e6, -2800, 0, 15000);
        ok(std::fabs(z.lo - (7.1e6 - 5300)) < 1 && std::fabs(z.hi - (7.1e6 + 2500)) < 1, "LSB 2.8 kHz: dial −5.3 .. +2.5 kHz");
        z = vfoDcZone(648e3, -5000, 5000, 15000);
        ok(std::fabs(z.lo - (648e3 - 7500)) < 1 && std::fabs(z.hi - (648e3 + 7500)) < 1, "AM 10 kHz: ±7.5 kHz (the locked DC at +15 is clear)");
        z = vfoDcZone(96.6e6, -100e3, 100e3, 15000);
        ok(std::fabs(z.lo - (96.6e6 - 15000)) < 1 && std::fabs(z.hi - (96.6e6 + 15000)) < 1, "WFM on a dongle: ±15 kHz — never stricter than the LOCKED VFO");
        z = vfoDcZone(96.6e6, -100e3, 100e3, 250000);
        ok(std::fabs(z.lo - (96.6e6 - 102500)) < 1 && std::fabs(z.hi - (96.6e6 + 102500)) < 1, "WFM on a HackRF (250 kHz): ±102.5 kHz");
    }

    const Mode modes[] = { mode("usb", 2800), mode("lsb", 2800), mode("cw", 1200), mode("am", 10000), mode("nfm", 12500), mode("wfm", 200000) };
    const double offs[] = { 15000, 250000 };

    std::printf("── 2. the LOCKED arrangement never moves ──\n");
    {
        bool allStay = true;
        for (const Mode& m : modes) for (double off : offs)
            for (double dial : { 648e3, 7.1e6, 96.6e6, 145.5e6 }) {
                const VfoDcPlace p = vfoDcCentre(dial, off, dial, dial - 100, vfoDcZone(dial, m.pbLo, m.pbHi, off), -INF, INF, -INF, INF);
                if (p.moved) { allStay = false; std::printf("     moved: %s off %.0f dial %.0f\n", m.name, off, dial); }
            }
        ok(allStay, "centre on the dial (DC at dial + offset): stays, every mode, dongle and HackRF");
    }

    std::printf("── 3. a FREE VFO swept across the DC, 100 Hz steps ──\n");
    for (const Mode& m : modes) for (double off : offs) {
        const double C = 14.2e6;                       // the centre the FREE VFO leaves behind
        double cur = C;
        const double span = (off > 100e3 ? 800e3 : 120e3) + m.bw;
        int movesUp = 0, movesDown = 0, onDc = 0, steps = 0;
        double maxMove = 0;
        auto step = [&](double dial, double prev, int& moves) {
            const VfoDcPlace p = vfoDcCentre(cur, off, dial, prev, vfoDcZone(dial, m.pbLo, m.pbHi, off), -INF, INF, -INF, INF);
            if (p.moved) { moves++; maxMove = std::max(maxMove, std::fabs(p.centre - cur)); cur = p.centre; }
            if (!clear(cur + off, dial, m, off)) onDc++;
            steps++;
        };
        double prev = C + off - span;
        for (double d = prev + 100; d <= C + off + span; d += 100) { step(d, prev, movesUp); prev = d; }
        for (double d = prev - 100; d >= C + off - span; d -= 100) { step(d, prev, movesDown); prev = d; }
        char b[256];
        std::snprintf(b, sizeof b, "%-3s %3.0f kHz offset: %d steps, DC in passband+guard after placement %d times, retunes up %d down %d (largest move %.1f kHz)",
                      m.name, off / 1e3, steps, onDc, movesUp, movesDown, maxMove / 1e3);
        ok(onDc == 0 && movesUp <= 1 && movesDown <= 1, b);
    }

    std::printf("── 4. hysteresis ──\n");
    {
        const Mode m = mode("usb", 2800);
        const double off = 15000, C = 7.0e6;
        // tune up to the zone's edge, then jitter ±200 Hz round it for 2000 steps
        double cur = C, prev = C;
        int moves = 0;
        auto at = [&](double dial) {
            const VfoDcPlace p = vfoDcCentre(cur, off, dial, prev, vfoDcZone(dial, m.pbLo, m.pbHi, off), -INF, INF, -INF, INF);
            if (p.moved) { moves++; cur = p.centre; }
            prev = dial;
        };
        for (double d = C; d <= C + off - 5300 + 50; d += 50) at(d);   // just inside: one move
        const int first = moves;
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> j(-200, 200);
        const double edge = prev;
        for (int i = 0; i < 2000; i++) at(edge + j(rng));
        ok(first == 1 && moves == 1, "a VFO jittering ±200 Hz at the edge for 2000 steps: 1 retune in all (" + std::to_string(moves) + ")");

        // a random walk across ±60 kHz of the centre in 10..500 Hz steps: never on the DC, and few retunes
        cur = C; prev = C; moves = 0;
        int onDc = 0;
        std::uniform_real_distribution<double> st(10, 500);
        std::uniform_int_distribution<int> dir(0, 9);
        double d = C, sgn = 1;
        for (int i = 0; i < 20000; i++) {
            if (dir(rng) == 0) sgn = -sgn;                       // keeps going one way for ~10 steps
            d += sgn * st(rng);
            if (d > C + 60e3) { d = C + 60e3; sgn = -1; }
            if (d < C - 60e3) { d = C - 60e3; sgn = 1; }
            at(d);
            if (!clear(cur + off, d, m, off)) onDc++;
        }
        ok(onDc == 0, "a 20 000-step random walk: the DC never in the passband + guard");
        ok(moves < 200, "…for " + std::to_string(moves) + " retunes (one per 100 steps would be 200)");
    }

    std::printf("── 5. no travel, limits, the view, VFO B ──\n");
    {
        const Mode m = mode("am", 10000);
        const double off = 15000, C = 1.0e6, dial = C + off + 2000;   // the DC 2 kHz below the dial
        const VfoDcZone z = vfoDcZone(dial, m.pbLo, m.pbHi, off);
        VfoDcPlace p = vfoDcCentre(C, off, dial, dial, z, -INF, INF, -INF, INF);
        ok(p.moved && p.centre + off < dial, "a jump onto the DC (no travel): the nearer side (DC below, where it was)");
        ok(!clear(C + off, dial, m, off) && clear(p.centre + off, dial, m, off), "…and clear of the passband + guard after");
        // moving up prefers DC below; forbid below → above
        p = vfoDcCentre(C, off, dial, dial - 100, z, z.lo - off, INF, -INF, INF);
        ok(p.moved && p.centre + off > dial, "the side behind the travel outside the hard limits: the other side");
        p = vfoDcCentre(C, off, dial, dial - 100, z, C - 100, C + 100, -INF, INF);
        ok(!p.moved && p.centre == C, "neither side inside the hard limits: no move (the caller's limits win)");
        // soft room only above → above even though moving up
        p = vfoDcCentre(C, off, dial, dial - 100, z, -INF, INF, z.hi - off, INF);
        ok(p.moved && p.centre + off > dial, "the view's room is preferred over the direction of travel");
        // soft room nowhere → falls back to the direction (a full-span view)
        p = vfoDcCentre(C, off, dial, dial - 100, z, -INF, INF, C, C);
        ok(p.moved && p.centre + off < dial, "no room in the view at all (full span): still moved, behind the travel");
        // VFO B sits just below → the below candidate would put the DC on B
        const VfoDcZone bz = vfoDcZone(z.lo - 10000, -10000, 10000, off);
        p = vfoDcCentre(C, off, dial, dial - 100, z, -INF, INF, -INF, INF, bz);
        ok(p.moved && p.centre + off > dial, "the DC is never handed to VFO B: the other side");
    }

    std::printf("── 6. VFO B's zone by ITS mode, and the hold the two-VFO placement uses (V12 beta24, fitCentre) ──\n");
    {
        // ★ The shim's watchDcZone: USB 0..+bw, LSB −bw..0, CW ±bw, the rest ±bw/2 — then vfoDcZone's guard and its
        //   never-stricter-than-locked rule, exactly as A's.
        const double off = 15000, b = 7.074e6;
        VfoDcZone z = vfoDcZone(b, 0, 2700, off);
        ok(z.lo == b - 2500 && z.hi == b + 2700 + 2500, "B on USB: carrier − guard … top of the passband + guard");
        ok(!(b + off > z.lo && b + off < z.hi), "…so B's own locked offset (dial + 15 kHz) is clear of it: a USB B never chases");
        z = vfoDcZone(b, -500, 500, off);
        ok(z.lo == b - 3000 && z.hi == b + 3000, "B on CW (±bw): 3 kHz either side");
        z = vfoDcZone(b, -20000, 20000, off);   // a wide AM B: 40 kHz, its locked offset inside → shrinks to ±off
        ok(z.lo == b - off && z.hi == b + off, "a wide AM B: never stricter than the locked VFO (±15 kHz)");
        // ★ the hold beyond the edge (fitCentre's edge candidates): the DC placed there is outside the zone, and the dial can
        //   travel `hold` further before it is inside again
        z = vfoDcZone(b, 0, 2700, off);
        const double hold = std::max(kVfoDcHoldMinHz, 0.5 * (z.hi - z.lo));
        const double below = z.lo - hold, above = z.hi + hold;
        ok(below < z.lo && above > z.hi, "the hold candidates sit outside the zone");
        const VfoDcZone moved = vfoDcZone(b - hold + 100, 0, 2700, off);   // B walks down by (hold - 100) Hz
        ok(!(below > moved.lo && below < moved.hi), "…and B can walk almost `hold` towards the DC before it is inside again");
    }

    std::printf("\n   %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
