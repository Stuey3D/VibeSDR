// ★★★ THE RSP RF LOOP MUST NOT WALK TO MAXIMUM GAIN ON A READOUT FROZEN AT A RAIL (vibe_rsp_rail_step.h).
// 2026-10-16, Lenovo RSP1A on FM: the IF reduction read 20.0 through five of our LNA steps (8 -> 3) while
// our own ADC peak barely moved — the IF was absorbing every rung and only its readout was dead. The rail
// exemption let the loop keep going to RF 1/9: overload. These replay that log and the cases that must
// still step.
//   build: c++ -std=c++17 -I ../android/app/src/main/cpp -o /tmp/trail test-rsp-rail-step.cpp
#include "vibe_rsp_rail_step.h"
#include <cstdio>

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}

using namespace vibersp;

static RfStepEvidence ev(int grAt, double grNow, float gAt, float gNow, double pkAt, double pkNow, int dir) {
    RfStepEvidence e;
    e.grAtStep = grAt; e.grMeanNow = grNow; e.structAtStep = gAt; e.structNow = gNow;
    e.peakAtStep = pkAt; e.peakNow = pkNow; e.dir = dir; e.setPointDbfs = -30;
    return e;
}

/** The loop's bookkeeping around the verdict, as vsSdrplayRfAgcTick does it: two Frozen = HOLD, and the
 *  hold returns the LNA to where the first dead step began. */
struct Loop {
    int deadSteps = 0, walkFrom = -1, lna;
    bool held = false;
    explicit Loop(int start) : lna(start) {}
    void judged(RfStepVerdict v, int lnaBeforeStep) {
        if (v == RfStepVerdict::Frozen) { if (++deadSteps == 1) walkFrom = lnaBeforeStep; }
        else if (v == RfStepVerdict::Answered) { deadSteps = 0; walkFrom = -1; }
        if (deadSteps >= 2) { held = true; if (walkFrom >= 0) lna = walkFrom; }
    }
};

int main() {
    printf("\n2026-10-16 17:51:45, replayed: the reduction frozen at the max-gain rail\n");
    {
        // (state before, state after, struct gain before/after, peak before/after) from the journal.
        // System gain is gainVals.curr (the API was not reporting); the peak is the next step's "before".
        struct S { int from, to; float gA, gN; double pA, pN; };
        const S steps[] = {
            {8, 7, 30.4f, 49.6f, -26.4, -22.9},
            {7, 6, 49.6f, 55.4f, -22.9, -23.4},
            {6, 5, 55.4f, 61.7f, -23.4, -21.0},
            {5, 4, 61.7f, 67.9f, -21.0, -19.3},
        };
        Loop L(8);
        int taken = 0;
        for (const S& s : steps) {
            if (L.held) break;
            L.lna = s.to; ++taken;
            L.judged(judgeRfStep(ev(20, 20.0, s.gA, s.gN, s.pA, s.pN, -1)), s.from);
        }
        ok(judgeRfStep(ev(20, 20.0, 30.4f, 49.6f, -26.4, -22.9, -1)) == RfStepVerdict::Frozen,
           "8 -> 7: a 19 dB rung moved the peak 3.5 dB — absorbed, so the 20 is not a rail");
        ok(judgeRfStep(ev(20, 20.0, 49.6f, 55.4f, -22.9, -23.4, -1)) == RfStepVerdict::Frozen,
           "7 -> 6: a 6 dB rung, the peak went DOWN — frozen");
        ok(L.held, "the loop holds");
        ok(taken == 2, "after two steps, not five (it walked 8 -> 1 on air)");
        ok(L.lna == 8, "and goes back to state 8, where the blind walk began");
    }

    printf("\nA genuine max-gain rail still gets its RF gain back\n");
    {
        // IF wide open, the level at target: every rung shows in the peak.
        ok(judgeRfStep(ev(20, 20.0, 30.0f, 49.0f, -29.0, -11.0, -1)) == RfStepVerdict::Answered,
           "19 dB rung, peak +18 dB — the IF could not absorb it: a real rail, keep stepping");
        ok(judgeRfStep(ev(20, 20.0, 49.0f, 55.0f, -40.0, -34.5, -1)) == RfStepVerdict::Answered,
           "6 dB rung, peak +5.5 dB — real");
        // Stuart's 40 m case: starved, the band mostly noise — the peak need not climb at all.
        ok(judgeRfStep(ev(20, 20.0, 20.0f, 26.0f, -45.0, -45.5, -1)) == RfStepVerdict::Answered,
           "starved (peak 15 dB under the set point): the rail stands even with a flat peak");
    }

    printf("\nThe minimum-gain rail, both ways\n");
    {
        ok(judgeRfStep(ev(59, 59.0, 40.0f, 21.0f, -8.0, -27.0, +1)) == RfStepVerdict::Answered,
           "59 rail, 19 dB taken out, peak fell 19 dB — real");
        // 2026-09-13's six-rung shed, at 59: ~21 dB rungs, the peak wandering -13 .. -20.
        ok(judgeRfStep(ev(59, 59.0, 30.0f, 9.0f, -13.3, -15.1, +1)) == RfStepVerdict::Frozen,
           "59 'rail', 21 dB taken out, peak fell 1.8 dB — frozen (09-13 walked six rungs on this)");
    }

    printf("\n2026-10-30 16:02, replayed: Band II at LNA 0, a REAL 59 rail with the peak 20 dB over the set point\n");
    {
        // System gain 51.5 -> 45.3 -> 39.0 (the 6.3 dB rungs); peaks from the journal.
        ok(judgeRfStep(ev(59, 59.0, 51.5f, 45.2f, -8.1, -5.7, +1)) == RfStepVerdict::Answered,
           "0 -> 1: the peak went UP 2.4 dB, but at -5.7 dBFS the IF is plainly not holding -30 — a real rail");
        ok(judgeRfStep(ev(59, 59.0, 45.3f, 39.0f, -5.9, -8.6, +1)) == RfStepVerdict::Answered,
           "1 -> 2: -2.7 dB on a 6.3 dB rung, at -8.6 dBFS — still a real rail (it HELD and undid to 0 on air)");
        ok(holdUndoTarget(2, 0, -8.4, -30, false) == 2,
           "and a hold would never undo 2 -> 0 (more RF gain) into a -8.4 dBFS peak");
        ok(heldSteerDir(-8.4, -30, false) == +1, "a held loop at -8.4 dBFS sheds RF gain by our own peak");
        ok(heldSteerDir(-20.0, -30, false) == 0, "and leaves a level at set point + 10 alone");
        ok(heldSteerDir(-22.0, -30, true) == +1, "a real overload sheds whatever the peak says");
    }

    printf("\nWhat the 09-13 test already did is unchanged\n");
    {
        ok(judgeRfStep(ev(45, 52.0, 30.0f, 36.0f, -25.0, -25.0, -1)) == RfStepVerdict::Answered,
           "the reduction moved: answered, whatever the peak did");
        ok(judgeRfStep(ev(45, 45.0, 30.0f, 36.0f, -25.0, -19.0, -1)) == RfStepVerdict::Frozen,
           "off a rail and bit-identical through a rung: frozen");
        ok(judgeRfStep(ev(45, 45.0, 30.0f, 30.0f, -25.0, -25.0, -1)) == RfStepVerdict::Unhonoured,
           "gainVals.curr never moved: our write did not land");
    }

    printf("\nA rail we cannot check is not evidence either way\n");
    {
        ok(judgeRfStep(ev(20, 20.0, 67.9f, 69.9f, -19.3, -19.2, -1)) == RfStepVerdict::Inconclusive,
           "a 2 dB rung hides in the peak's wander");
        ok(judgeRfStep(ev(20, 20.0, 30.0f, 49.0f, NAN, -22.0, -1)) == RfStepVerdict::Inconclusive,
           "no peak at the step");
        ok(judgeRfStep(ev(20, 20.0, -999.0f, -999.0f, -26.0, -22.0, -1)) == RfStepVerdict::Inconclusive,
           "no gain readback");
    }

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
