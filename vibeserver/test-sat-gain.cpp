// ★★★ THE SATELLITE-PASS GAIN PROFILE (vibe_sat_gain.h, V12 beta91) — pure, no radio, silent.
//   The setter on each radio's table (RTL R820T/R828D list, the RSP's IF + LNA, the Airspy's 22 presets), the hold
//   against the very inputs that cut Stuart's METEOR-M2 4 pass on 2026-11-22 (converter "hot" at -13 dBFS, the
//   neighbourhood "spraying", "clean for 8 s" climbs), a genuine clip still stepping down, and the ordinary gain
//   resuming after the decoder stops / the pick goes / LOS (Watch).
//   build: c++ -std=c++17 -I ../android/app/src/main/cpp -o /tmp/tsg test-sat-gain.cpp
#include "vibe_sat_gain.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}
static int idxOf(const std::vector<int>& g, int tenth) {
    for (size_t i = 0; i < g.size(); ++i) if (g[i] == tenth) return (int)i;
    return -1;
}

int main() {
    using namespace vibesatgain;
    // The R820T/R828D tuner's own list (rtlsdr_get_tuner_gains), tenths of a dB — the V4's table.
    const std::vector<int> rtl = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338,
                                  364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
    const int top = (int)rtl.size() - 1;
    const Radio R = rtlRadio();

    printf("\nThe setter — RTL\n");
    {   // 137 MHz, quiet band: peak -20 dBFS at 32.8 dB → +12 dB of room to the -8 set point → 44.5 dB
        const Choice c = chooseGain(rtl, idxOf(rtl, 328), top, -20.0, -60.0, NAN, R);
        ok(rtl[(size_t)c.idx] == 445, "peak -20 dBFS at 32.8 dB: set to 44.5 dB (the highest under the -8 dBFS set point)");
        ok(c.predictedPk <= R.targetPk && c.predictedPk > R.targetPk - 4.0, "predicted peak within one rung under the set point");
        ok(std::string(c.limit) == "headroom", "decided by headroom");
    }
    {   // hot band: peak -2 dBFS at 32.8 → must come DOWN 6 dB or more
        const Choice c = chooseGain(rtl, idxOf(rtl, 328), top, -2.0, -60.0, NAN, R);
        ok(rtl[(size_t)c.idx] <= 268 && rtl[(size_t)c.idx] >= 229, "peak -2 dBFS at 32.8 dB: down to 25.4 dB (6 dB of room made)");
    }
    {   // very quiet band: everything fits — the top of the table, or the owner's ceiling
        const Choice c = chooseGain(rtl, idxOf(rtl, 197), top, -45.0, -80.0, NAN, R);
        ok(c.idx == top && std::string(c.limit) == "ceiling", "peak -45 dBFS: the top of the table (49.6 dB)");
        const Choice d = chooseGain(rtl, idxOf(rtl, 197), idxOf(rtl, 386), -45.0, -80.0, NAN, R);
        ok(rtl[(size_t)d.idx] == 386, "...and never past the owner's ceiling (38.6 dB)");
    }
    {   // a blowtorch: even 0 dB is over → as low as it goes
        const Choice c = chooseGain(rtl, idxOf(rtl, 77), top, 0.0, -40.0, NAN, R);
        ok(c.idx <= 1 && std::string(c.limit) == "bottom", "peak on the rail at 7.7 dB, 8 dB of room wanted: the bottom");
    }
    {   // the radio's own floor known: more gain than lifts the floor 10 dB over it is not taken
        // floor -70 at 32.8 dB, own floor -78 → +2 dB of gain lifts it 10 dB over: 34.0... the first ≥ is 36.4 (+3.6)
        const Choice c = chooseGain(rtl, idxOf(rtl, 328), top, -30.0, -70.0, -78.0, R);
        ok(rtl[(size_t)c.idx] == 364 && std::string(c.limit) == "floor",
           "own floor known: 36.4 dB is enough (floor 11.6 dB over the radio's own), headroom would have allowed 49.6");
        ok(std::isfinite(c.floorOverOwn) && c.floorOverOwn >= kFloorLiftDb, "...and says by how much");
        // the floor rule never buys gain past the headroom
        const Choice h = chooseGain(rtl, idxOf(rtl, 328), top, -10.0, -90.0, -78.0, R);
        ok(rtl[(size_t)h.idx] == 338 && std::string(h.limit) == "headroom", "floor never reaches 10 dB over the radio's own: headroom still caps it (33.8 dB, not more)");
    }

    printf("\nThe setter — SDRplay (IF + LNA)\n");
    {
        const Radio S = rspRadio();
        const std::vector<int> t = rspIfTable(20);
        ok((int)t.size() == 40 && t.front() == 0 && t.back() == 390, "IF table: 40 rungs, gRdB 59..20 as gain 0..39 dB");
        // gRdB 50 (idx 9), peak -24 dBFS: 14 dB more → gRdB 36
        const Choice c = chooseGain(t, rspIdxOfGr(50), (int)t.size() - 1, -24.0, NAN, NAN, S);
        ok(rspGrOfIdx(c.idx) == 36, "peak -24 dBFS at IF 50: IF to 36 (-10 dBFS set point)");
        // RF stays: 36 + a 6 dB rung = 42 ≤ 46 → one rung of RF in, IF counter-stepped
        const RspPlace p = rspPlace(3, rspGrOfIdx(c.idx), 20, 0, 10, true, 6.3, 6.3);
        ok(p.lnaChanged && p.lna == 2 && p.gr == 42, "...and with room for the whole rung: RF 3 -> 2, IF 36 -> 42 (VibeAGC's rule 3)");
        const RspPlace q = rspPlace(3, 44, 20, 0, 10, true, 6.3, 6.3);
        ok(!q.lnaChanged && q.gr == 44, "IF 44: no room for a rung + 6 dB margin — RF stays");
        const RspPlace o = rspPlace(3, 65, 20, 0, 10, true, 6.3, 18.9);
        ok(o.lnaChanged && o.lna == 4 && o.gr == 46, "the IF cannot shed it (wants 65): RF rung out, IF 46");
        const RspPlace m = rspPlace(3, 65, 20, 0, 10, false, 6.3, 18.9);
        ok(!m.lnaChanged && m.gr == 59, "...but not where the owner fixed the RF: IF to its 59 rail");
        const RspPlace f = rspPlace(3, 25, 30, 0, 10, false, 6.3, 6.3);
        ok(f.gr == 30, "the owner's IF floor (30) holds");
    }

    printf("\nThe setter — Airspy R2 / Mini (22 linearity presets)\n");
    {
        std::vector<int> asp; for (int i = 0; i <= 21; ++i) asp.push_back(i * 10);
        const Choice c = chooseGain(asp, 10, 21, -25.0, NAN, NAN, R);
        ok(c.idx == 21, "peak -25 at preset 10: +11 presets fit under -8 → preset 21");
        const Choice d = chooseGain(asp, 15, 21, -4.0, NAN, NAN, R);
        ok(d.idx == 11, "peak -4 at preset 15: 4 presets down → 11");
    }

    printf("\nThe hold — the 2026-11-22 inputs change nothing\n");
    {
        Hold h(R);
        In in; in.gains = &rtl; in.ceilIdx = top; in.curIdx = idxOf(rtl, 372); in.floorDb = -62.0;
        h.begin(0.0, "METEOR-M2 4 LRPT");
        ok(h.phase() == Phase::Measure, "decoder starts: measuring");
        Out o; double t = 0;
        // the window: peaks -16..-13 dBFS (the "converter hot (peak -13.2)" case — nowhere near the rail)
        for (double pk : {-16.0, -13.2, -14.0}) { in.now = t; in.peak = pk; o = h.tick(in); t += 2.0; }
        ok(h.phase() == Phase::Hold, "after 4 s: holding");
        ok(o.kind == Out::Set && rtl[(size_t)o.idx] == 421, "set once: 37.2 dB, peak -13.2 → 42.1 dB (-8.3 predicted)");
        ok(o.log.find("gain set to 42.1 dB") != std::string::npos && o.log.find("clips only") != std::string::npos,
           "the log says what it did, with numbers, and that it holds");
        printf("        %s\n", o.log.c_str());
        in.curIdx = o.idx;
        // ten minutes of the pass: the readings that produced 9 steps — hot peaks, spraying shoulders, clean runs
        int moves = 0;
        for (int s = 0; s < 300; ++s) {
            in.now = t; t += 2.0;
            in.peak = (s % 7 == 0) ? -1.6 : (s % 5 == 0) ? -30.0 : -9.0;     // hot seconds, quiet seconds, the set point
            in.floorDb = -62.0 + ((s % 11) - 5);                              // the floor wandering ±5 dB
            in.clipRun = (s % 13 == 0) ? 1 : 0;                               // single spark-plug seconds
            in.clipPct = in.clipRun ? 0.001 : 0.0;
            if (h.tick(in).kind != Out::None) ++moves;
        }
        ok(moves == 0, "600 s of hot peaks, quiet seconds, a wandering floor and single-second rails: NO cut, NO climb");
        ok(h.sets() == 1 && h.cuts() == 0, "one set, no cuts, for the whole pass");

        printf("\nThe hold — a genuine clip still steps down\n");
        in.now = t; in.peak = 0.0; in.clipRun = 2; in.clipPct = 0.02;
        o = h.tick(in);
        ok(o.kind == Out::ClipCut && o.idx < in.curIdx, "two seconds on the rail: a cut");
        ok((rtl[(size_t)in.curIdx] - rtl[(size_t)o.idx]) / 10.0 >= 8.0, "...sized to the 8 dB the set point needs");
        ok(o.log.find("still held") != std::string::npos, "...and it stays held");
        in.curIdx = o.idx; in.clipRun = 0; in.clipPct = 0; in.peak = -30.0;
        int ups = 0; for (int s = 0; s < 60; ++s) { in.now = (t += 2.0); if (h.tick(in).kind != Out::None) ++ups; }
        ok(ups == 0 && h.phase() == Phase::Hold, "after the clip, two minutes quiet: it does NOT climb back");
        in.clipRun = 3; in.clipPct = 40.0; in.peak = 0.0; in.now = (t += 2.0);
        const int from = in.curIdx; o = h.tick(in);
        ok(o.kind == Out::ClipCut && (rtl[(size_t)from] - rtl[(size_t)o.idx]) / 10.0 >= 17.0,
           "a gross rail (40 %): shed in one move, as the ordinary loop does (>= 18 dB or to the bottom)");

        printf("\nThe hold ends — the ordinary gain resumes\n");
        h.end();
        in.clipRun = 0; in.peak = -1.5; in.now = (t += 2.0);
        ok(!h.active() && h.tick(in).kind == Out::None, "after end(): inert — the shim's own loop has the radio again");
    }
    {   // a clip DURING the measurement window: shed at once, window restarts
        Hold h(R);
        In in; in.gains = &rtl; in.ceilIdx = top; in.curIdx = top;
        h.begin(0.0, "test");
        in.now = 0; in.peak = 0.0; in.clipRun = 2; in.clipPct = 30.0;
        const Out o = h.tick(in);
        ok(o.kind == Out::ClipCut && h.phase() == Phase::Measure, "railing at the start: shed now, measure again");
        in.curIdx = o.idx; in.clipRun = 0; in.clipPct = 0; in.peak = -20.0;
        Out last;
        for (double t = 1.0; t <= 6.0; t += 2.0) { in.now = t; last = h.tick(in); }
        ok(h.phase() == Phase::Hold && last.kind == Out::Set, "...then set from the clean window");
    }
    {   // already at the right gain: "stays at", no write
        Hold h(R);
        In in; in.gains = &rtl; in.ceilIdx = top; in.curIdx = idxOf(rtl, 421);
        h.begin(0.0, "test");
        Out o; for (double t = 0; t <= 4.0; t += 2.0) { in.now = t; in.peak = -8.5; o = h.tick(in); }
        ok(o.kind == Out::None && o.log.find("stays at 42.1 dB") != std::string::npos, "already right: says so, writes nothing");
    }
    {   // AOS re-measure on an armed pick
        Hold h(R);
        In in; in.gains = &rtl; in.ceilIdx = top; in.curIdx = idxOf(rtl, 328);
        h.begin(0.0, "test");
        for (double t = 0; t <= 4.0; t += 2.0) { in.now = t; in.peak = -20.0; h.tick(in); }
        ok(h.phase() == Phase::Hold, "armed before the rise: measured and holding");
        h.remeasure(100.0);
        ok(h.phase() == Phase::Measure, "AOS: measuring again");
        Out o; in.curIdx = idxOf(rtl, 445);
        for (double t = 100.0; t <= 104.0; t += 2.0) { in.now = t; in.peak = -6.0; o = h.tick(in); }
        ok(o.kind == Out::Set && o.idx < in.curIdx && h.phase() == Phase::Hold, "...and sets again for what the sky brought");
    }

    printf("\nThe hold — SDRplay (continuous gain)\n");
    {
        const Radio S = rspRadio();
        Hold h(S);
        In in; in.gainDb = 9.0; in.roomUpDb = 30.0;          // gains == nullptr: continuous
        h.begin(0.0, "METEOR-M2 4 LRPT");
        Out o;
        for (double t = 0; t <= 4.0; t += 1.0) { in.now = t; in.peak = (t == 2.0) ? -22.0 : -24.0; o = h.tick(in); }
        ok(h.phase() == Phase::Hold && o.kind == Out::Set && std::fabs(o.deltaDb - 12.0) < 1e-9,
           "peak -22 dBFS loudest: +12 dB to the -10 dBFS set point");
        printf("        %s\n", o.log.c_str());
        // the RSP turns it into IF 47 -> 35, then VibeAGC's rung rule: 35 + 6.3 <= 46 → RF in, IF 41
        const RspPlace p = rspPlace(3, (int)std::ceil(47 - o.deltaDb), 20, 0, 10, true, 6.3, 6.3);
        ok(p.lna == 2 && p.gr == 41, "...IF 47 -> 35, and the room takes one rung of RF: LNA 3 -> 2, IF 41");
        int moves = 0;
        for (int s2 = 0; s2 < 600; ++s2) {
            in.now = 5.0 + s2; in.peak = (s2 % 9 == 0) ? -2.0 : -12.0; in.clipRun = (s2 % 17 == 0) ? 1 : 0; in.clipPct = in.clipRun ? 0.01 : 0;
            if (h.tick(in).kind != Out::None) ++moves;
        }
        ok(moves == 0, "ten minutes of hot windows and single clipped seconds: nothing moves");
        in.clipRun = 2; in.clipPct = 0.5; in.peak = 0.0; in.now = 700.0;
        o = h.tick(in);
        ok(o.kind == Out::ClipCut && std::fabs(o.deltaDb + 10.0) < 1e-9, "two seconds clipping: down 10 dB (peak over the set point)");
        const RspPlace q = rspPlace(2, 57 + 10, 20, 0, 10, true, 6.3, 6.3);
        ok(q.lna == 3 && q.gr == 59, "...at IF 57 the IF cannot take 10 dB: one rung of RF out, IF 59 (rail)");
        in.roomUpDb = 3.0; Hold c(S); c.begin(0.0, "x"); in.clipRun = 0; in.clipPct = 0;
        for (double t = 0; t <= 4.0; t += 1.0) { in.now = t; in.peak = -40.0; o = c.tick(in); }
        ok(o.kind == Out::Set && std::fabs(o.deltaDb - 3.0) < 1e-9, "only 3 dB of gain left under the owner's caps: +3, not +30");
        Hold z(S); z.begin(0.0, "x"); in.roomUpDb = 30;
        for (double t = 0; t <= 4.0; t += 1.0) { in.now = t; in.peak = -10.4; o = z.tick(in); }
        ok(o.kind == Out::None && o.log.find("stays") != std::string::npos, "within a decibel of the set point: no write");
    }

    printf("\nWhen a pass is on (Watch)\n");
    {
        Watch w;
        ok(w.step(false, "", false, false) == Watch::None, "nothing running: nothing");
        ok(w.step(true, "pick:57166", true, true) == Watch::Begin, "a pick with the satellite up: Begin");
        ok(w.step(true, "pick:57166", true, true) == Watch::None, "...steady: nothing");
        ok(w.step(true, "pick:57166", true, false) == Watch::End, "LOS: End — the ordinary gain resumes");
        ok(w.step(true, "pick:57166", true, false) == Watch::None, "after LOS, pick still there: stays off");
        ok(w.step(true, "pick:57166", true, true) == Watch::Begin, "the next AOS on the same pick: Begin again");
        ok(w.step(false, "", false, false) == Watch::End, "the decoder stops / the pick goes: End");
    }
    {
        Watch w;
        ok(w.step(true, "pick:59051", true, false) == Watch::Begin, "armed before the rise: Begin (measure while waiting)");
        ok(w.step(true, "pick:59051", true, false) == Watch::None, "...waiting: nothing");
        ok(w.step(true, "pick:59051", true, true) == Watch::Aos, "AOS: Aos (measure again, hold stays)");
        ok(w.step(true, "pick:40069", true, true) == Watch::Begin, "a different pick: Begin afresh");
    }
    {
        Watch w;
        ok(w.step(true, "lrpt", false, false) == Watch::Begin, "LRPT started by hand (no orbit to ask): Begin");
        ok(w.step(true, "lrpt", false, false) == Watch::None, "...held while it runs");
        ok(w.step(false, "", false, false) == Watch::End, "...ends when it stops");
    }

    printf("\nThe official SDRplay API: the IF AGC's set point for a pass\n");
    {
        ok(rspApiPassSetPoint(-999) == -22, "owner never chose one: -22 dBFS average (the -10 peak, 12 dB peak-to-average)");
        ok(rspApiPassSetPoint(-30) == -30, "owner CHOSE -30 (more headroom than -22): kept — only the unset default is raised");
        ok(rspApiPassSetPoint(-15) == -22, "owner above it: lowered to -22 for the pass");
        ok(rspApiPassSetPoint(-45) == -45, "owner chose MORE headroom (-45): kept, a pass never raises it");
        ok(rspApiPassSetPoint(-22) == -22, "owner exactly at it: unchanged");
    }

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
