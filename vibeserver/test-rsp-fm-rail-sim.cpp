// ★★★ THE RSP RF LOOP ON A SWAMPED BAND II — a closed-loop simulation of vsSdrplayRfAgcTick's carrier path
//     (local_sdr_shim.cpp) against a simulated RSP1A, before and after the 2026-10-30 fix.
//
// Stuart, 2026-10-30, Lenovo RSP1A moving to Broadcast FM: "the RF AGC didnt respond to the IF agc and got
// stuck at full". The journal:
//     RF gain state 0 -> 1 (ADC peak -8.1 dBFS)   ... moved our own ADC peak only +2.4 dB ... frozen (1)
//     RF gain state 1 -> 2 (ADC peak -6.0 dBFS)   ... moved our own ADC peak only -2.7 dB ... frozen (2)
//     HOLDING ... undoing the steps taken on the frozen reading — RF gain state 2 -> 0
// and then nothing, for good: full RF gain, IF pinned at its 59 dB minimum-gain rail.
//
// THE MODEL (one 500 ms window per tick, as kWindowMs):
//   · Band II LNA ladder, RSP1A 60-420 MHz: reductions 0, 6.3, 12.6, 18.9, 37, 42, 61, 66, 72, 77 dB — the
//     first three rungs are the 6.3 dB the journal measured; 3 -> 4 is the big one.
//   · The IF AGC needs reduction R = R0 - lnaRed[state] to hold its -30 dBFS set point, and can give
//     20..59. The reduction it reports is clamp(R) — or, in the FROZEN variants, a fixed 59 whatever it does.
//   · Our ADC peak: set point + 10 dB (peak over average, measured in DAB 09-15) + whatever the IF could NOT
//     take off (R - realGr), COMPRESSED by an overloading front end (12·tanh(x/6)) — the journal's peak sat
//     ~12 dB over that line at states 0, 1 and 2 alike (-8.1, -5.7/-6.0, -8.6), so a 6.3 dB rung barely
//     moved it — plus about a decibel of deterministic wander.
//   · The tick: 5 s quiet after our own step; then the open-loop test (judgeRfStep), two Frozen = HOLD; then
//     the 40/52 window rule (1 s sustain at a rail, 6 s otherwise; 6 s between steps; reversal refused
//     unless at a rail). A replica of the shim's decision order, not the shim: the shim needs a radio.
//
//   build: c++ -std=c++17 -I ../android/app/src/main/cpp -o /tmp/trailsim test-rsp-fm-rail-sim.cpp
#include "vibe_rsp_rail_step.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace vibersp;

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}

/* ★ The judgement and the hold as they were BEFORE 2026-10-30, kept here only to show the fault. */
static RfStepVerdict judgeBefore(const RfStepEvidence& e) {
    const bool haveG    = e.structNow > -998.0f && e.structAtStep > -998.0f;
    const bool wrLanded = haveG && std::fabs(e.structNow - e.structAtStep) > 0.05f;
    if (haveG && !wrLanded) return RfStepVerdict::Unhonoured;
    if ((int)std::lround(e.grMeanNow) != e.grAtStep) return RfStepVerdict::Answered;
    if (!rfReadingAtRail(e.grMeanNow)) return RfStepVerdict::Frozen;
    const bool maxGainRail = e.grMeanNow <= 20.5;
    if (maxGainRail && std::isfinite(e.peakNow) && e.peakNow < e.setPointDbfs - kStarvedBelowSetDb)
        return RfStepVerdict::Answered;
    const double rung = haveG ? std::fabs((double)e.structNow - (double)e.structAtStep) : NAN;
    if (!std::isfinite(rung) || rung < kMinJudgeRungDb || e.dir == 0 ||
        !std::isfinite(e.peakAtStep) || !std::isfinite(e.peakNow))
        return RfStepVerdict::Inconclusive;
    const double moved = (e.peakNow - e.peakAtStep) * (e.dir < 0 ? 1.0 : -1.0);
    return moved >= rung * kRungShareDb ? RfStepVerdict::Answered : RfStepVerdict::Frozen;
}

static const double kLnaRed[10] = {0, 6.3, 12.6, 18.9, 37, 42, 61, 66, 72, 77};
constexpr int kSet = -30;

struct Radio {
    double R0;              // the reduction the IF would need at LNA state 0
    bool   frozenReadout;   // the API's reduction readout stuck at 59
    int    lna;
    int    t = 0;
    double needR() const { return R0 - kLnaRed[lna]; }
    double realGr() const { return std::clamp(needR(), 20.0, 59.0); }
    int    readGr() const { return frozenReadout ? 59 : (int)std::lround(realGr()); }
    // gainVals.curr: the API computes it from the COMMANDED struct (LNA state + the gRdB written back),
    // which is why the journal's system gain fell 6.2 dB per rung with the IF reading 59 throughout.
    float  structGain() const { return (float)(110.0 - kLnaRed[lna] - readGr()); }
    double peak() const {
        const double over = needR() - realGr();            // what the IF could not take off (+ swamped)
        const double comp = over > 0 ? 12.0 * std::tanh(over / 6.0) : over;   // a hard-limiting front end
        static const double wander[] = {0.0, 0.8, -0.6, 1.1, -0.9, 0.4, -1.2, 0.7};
        return kSet + 10.0 + comp + wander[t % 8];
    }
};

struct Result { int finalLna; int finalGr; double finalPeak; bool held; int steps; int fullGainWindows; };

static Result run(Radio r, bool after, int seconds, bool verbose, bool startHeld = false) {
    const int kWin = 500;
    int msSinceStep = 1 << 20, outMs = 0, outDir = 0, lastDir = 0, deadSteps = 0, walkFrom = -1, steps = 0;
    int grAtStep = -1, dirAt = 0, lnaBefore = -1; float sAt = -999.0f; double pkAt = NAN;
    bool held = startHeld; int fullGainWindows = 0;
    if (startHeld) deadSteps = 2;
    auto step = [&](int to, double mean, double pk, int dir, const char* why) {
        if (verbose) printf("      t=%5.1fs  %s: RF gain state %d -> %d  (IF %.0f, peak %.1f dBFS)\n",
                            r.t * kWin / 1000.0, why, r.lna, to, mean, pk);
        grAtStep = (int)std::lround(mean); sAt = r.structGain(); pkAt = pk; dirAt = dir; lnaBefore = r.lna;
        r.lna = to; msSinceStep = 0; lastDir = dir; outMs = 0; outDir = 0; ++steps;
    };
    for (; r.t < seconds * 1000 / kWin; ++r.t) {
        msSinceStep += kWin;
        if (r.lna == 0) ++fullGainWindows;
        const double mean = r.readGr(), pk = r.peak();
        if (msSinceStep <= 5000) { outMs = 0; outDir = 0; continue; }           // the 5 s quiet
        if (grAtStep >= 0) {
            RfStepEvidence e;
            e.grAtStep = grAtStep; e.grMeanNow = mean; e.structAtStep = sAt; e.structNow = r.structGain();
            e.peakAtStep = pkAt; e.peakNow = pk; e.dir = dirAt; e.setPointDbfs = kSet;
            const RfStepVerdict v = after ? judgeRfStep(e) : judgeBefore(e);
            if (v == RfStepVerdict::Frozen) { if (++deadSteps == 1) walkFrom = lnaBefore; }
            else if (v == RfStepVerdict::Answered) { deadSteps = 0; walkFrom = -1; held = false; }
            if (verbose) printf("      t=%5.1fs    judged: %s\n", r.t * kWin / 1000.0,
                v == RfStepVerdict::Frozen ? "FROZEN" : v == RfStepVerdict::Answered ? "answered" : "inconclusive");
            grAtStep = -1;
        }
        if (deadSteps >= 2) {
            if (!held) {
                held = true;
                const int back = after ? holdUndoTarget(r.lna, walkFrom, pk, kSet, false) : walkFrom;
                walkFrom = -1;
                if (verbose) printf("      t=%5.1fs  HOLD\n", r.t * kWin / 1000.0);
                if (back >= 0 && back != r.lna) step(back, mean, pk, 0, "undo"), grAtStep = -1;
                continue;
            }
            if (!after) { outMs = 0; outDir = 0; continue; }                    // ✗ before: held blind
            const int hdir = heldSteerDir(pk, kSet, false);
            if (hdir == 0) { outMs = 0; outDir = 0; continue; }
            if (hdir != outDir) { outDir = hdir; outMs = 0; }
            outMs += kWin;
            if (outMs < 2000 || msSinceStep < 6000) continue;
            const int want = std::clamp(r.lna + hdir, 0, 9);
            if (want != r.lna) step(want, mean, pk, hdir, "held, by our own peak");
            continue;
        }
        const int dir = mean > 52 ? +1 : (mean < 40 ? -1 : 0);
        if (dir == 0) { outMs = 0; outDir = 0; continue; }
        if (dir != outDir) { outDir = dir; outMs = 0; }
        outMs += kWin;
        const bool rail = mean <= 22 || mean >= 57;
        if (outMs < (rail ? 1000 : 6000) || msSinceStep < 6000) continue;
        if (lastDir != 0 && dir != lastDir && !rail) { outMs = 0; outDir = 0; continue; }
        const int want = std::clamp(r.lna + dir, 0, 9);
        if (want != r.lna) step(want, mean, pk, dir, "window rule");
    }
    return {r.lna, r.readGr(), r.peak(), held, steps, fullGainWindows};
}

int main() {
    printf("\n1. Band II, live readout, the station + neighbours 20 dB more than the IF can take at full RF gain\n");
    {
        Radio r{59.0 + 20.0, false, 0};
        printf("   BEFORE\n");
        const Result b = run(r, false, 120, true);
        printf("   -> LNA %d, IF %d, peak %.1f dBFS, held %d, %d of 240 windows at full RF gain\n",
               b.finalLna, b.finalGr, b.finalPeak, b.held, b.fullGainWindows);
        ok(b.held && b.finalLna == 0, "before: walks 0 -> 1 -> 2, holds on a FALSE 'frozen', undoes to 0 (the 10-30 log)");
        printf("   AFTER\n");
        const Result a = run(r, true, 120, true);
        printf("   -> LNA %d, IF %d, peak %.1f dBFS, held %d\n", a.finalLna, a.finalGr, a.finalPeak, a.held);
        ok(!a.held, "after: never held — the 59 is confirmed by a peak 20 dB over the set point");
        ok(a.finalLna == 4 && a.finalGr >= 40 && a.finalGr <= 52,
           "after: settles at LNA 4 with the IF off its rail, inside the 40-52 window");
        ok(a.finalPeak < kSet + kSwampedOverSetDb, "after: the peak is back near set point + 10");
    }

    printf("\n2. Band II, readout FROZEN at 59 while the level is genuinely swamped (the real IF is pinned too)\n");
    {
        Radio r{59.0 + 20.0, true, 0};
        const Result a = run(r, true, 180, true);
        printf("   -> LNA %d, real IF %.0f, peak %.1f dBFS, held %d\n", a.finalLna, Radio{r.R0, false, a.finalLna}.realGr(), a.finalPeak, a.held);
        ok(a.finalLna >= 3, "after: the frozen 59 does not leave it at full gain — shed by our own peak");
        ok(a.finalPeak < kSet + kSwampedOverSetDb, "after: the peak is out of the swamped zone");
    }

    printf("\n3. Readout FROZEN at 59 over a LIVE IF that is coping (the 09-13 shape) — must NOT walk\n");
    {
        Radio r{90.0, true, 4};    // the IF really needs 53, 48, 29 dB at states 4, 5, 6: inside its range
        const Result b = run(r, false, 120, false);
        const Result a = run(r, true, 120, true);
        printf("   -> before LNA %d (steps %d) · after LNA %d (steps %d), held %d, peak %.1f\n",
               b.finalLna, b.steps, a.finalLna, a.steps, a.held, a.finalPeak);
        ok(a.held, "after: two rungs that did not move the peak's level = frozen, HOLD (unchanged)");
        ok(a.finalLna == 4, "after: and the hold goes back to where the walk began (unchanged)");
        ok(a.steps <= b.steps, "after: no more steps than before");
    }

    printf("\n4. The 10-16 shape: frozen at the MAX-gain rail (20) over a live IF — must not walk into overload\n");
    {
        // The verdicts are replayed from the journal in test-rsp-rail-step.cpp; here only the hold's two moves.
        ok(heldSteerDir(-22.9, kSet, false) == 0 && heldSteerDir(-19.3, kSet, false) == 0,
           "held at 10-16's peaks (-19 .. -26 dBFS) the peak steer adds NOTHING");
        ok(holdUndoTarget(1, 8, -20.3, kSet, false) == 8, "and the undo of a walk INTO gain still happens");
    }

    printf("\n5. Already HELD at full RF gain (where the 10-30 undo left it) on a swamped band — the hold must steer\n");
    {
        Radio r{59.0 + 20.0, true, 0};
        const Result b = run(r, false, 60, false, true);
        printf("   BEFORE -> LNA %d, peak %.1f dBFS (held blind)\n", b.finalLna, b.finalPeak);
        ok(b.finalLna == 0, "before: a held loop never moves again — stuck at full gain");
        const Result a = run(r, true, 60, true, true);
        printf("   AFTER  -> LNA %d, peak %.1f dBFS\n", a.finalLna, a.finalPeak);
        ok(a.finalLna >= 3 && a.finalPeak < kSet + kSwampedOverSetDb,
           "after: steered by our own peak until it leaves the swamped zone");
    }

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
