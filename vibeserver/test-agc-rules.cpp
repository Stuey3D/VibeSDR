// ★★★ THE THREE RULES FROM THE SONY'S TWO DROPS (vibe_agc_rules.h, 2026-10-02 21:23 and 21:25).
// Pure functions, so the evening's own numbers are the test: the auto-IF hold after a new start, the
// gross-overload shed, and the band key that carries a settled gain across a retune — plus the retune-forget
// distance (§4), pinned on Stuart's two-SSB-signals case.
//   build: c++ -std=c++17 -I ../android/app/src/main/cpp -o /tmp/tagc test-agc-rules.cpp
#include "vibe_agc_rules.h"
#include <cstdio>
#include <cmath>

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}

int main() {
    using namespace vibeagc;
    printf("\nAuto-IF hold after a new start\n");
    // 21:23:02.5 DAB off; adjacent narrow fired at 21:23:03.9 (1.4 s) and was undone at 21:23:05.9.
    const double start = 1000.0, quiet = start + kAutoIfSettleSec;
    ok(autoIfHeld(start + 1.4, quiet), "1.4 s after the retune: held (the 21:23 narrowing could not happen)");
    ok(autoIfHeld(start + 3.4, quiet), "3.4 s: still held (its reversal was on numbers just as unsettled)");
    ok(!autoIfHeld(start + 6.0, quiet), "6 s: free — the ifGain EMA's ~5 s has passed");
    ok(!autoIfHeld(start + 600.0, quiet), "steady state: never held (nothing changes without a new start)");
    ok(!autoIfHeld(start, 0.0), "no new start yet (quietUntil 0): never held");

    printf("\nGross overload — one move, not 6 dB rungs\n");
    // 21:25: peak pinned at 0.0 dBFS, target -6 → computed shed 6 dB, while 56.6 % sat on the rail.
    ok(grossOverloadShedDb(6.0, 56.6) >= 18.0, "56.6 % on the rail: at least 18 dB at once (was 6.2)");
    ok(grossOverloadShedDb(6.0, 10.0) >= 18.0, "exactly 10 %: gross");
    ok(std::fabs(grossOverloadShedDb(6.0, 1.473) - 6.0) < 1e-9,
       "1.47 % (the 21:25:36 tail): the peak is a measurement again — computed shed stands");
    ok(std::fabs(grossOverloadShedDb(25.0, 56.6) - 25.0) < 1e-9, "a larger computed shed is never reduced");
    ok(std::fabs(grossOverloadShedDb(0.5, 0.0) - 0.5) < 1e-9, "no clipping: unchanged");
    // The evening's case end to end: 49.6 dB → 18 dB shed lands ≤ 31.6, then one sized move to 22.9.
    ok(49.6 - grossOverloadShedDb(6.0, 56.6) <= 31.7, "49.6 dB → ≤ 31.6 dB in the first write (four writes became two)");

    printf("\nBand keys — a settled gain belongs to its band\n");
    ok(agcBandKey(96.6e6) != agcBandKey(128.1167e6), "96.6 (FM) and 128.117 (airband) are different bands — the 21:25 jump crosses");
    ok(agcBandKey(96.6e6) == agcBandKey(107.7e6), "96.6 and 107.7 share the FM band — no seed within it");
    ok(agcBandKey(128.1167e6) == agcBandKey(118.0e6), "airband is one band");
    ok(agcBandKey(225.648e6) != agcBandKey(96.6e6), "Band III is not FM");
    ok(agcBandKey(87.5e6) == agcBandKey(96.6e6) && agcBandKey(87.49e6) != agcBandKey(96.6e6),
       "the FM band starts at 87.5 MHz");
    ok(agcBandKey(1.25e6) != agcBandKey(7.1e6), "MW and HF are different bands");
    ok(agcBandKey(1090e6) == agcBandKey(1766e6), "above 1 GHz is one band");

    printf("\nRetune forget — two SSB signals a few kHz apart (the Portable, 2026-10-20)\n");
    {
        // The widths the shim actually passes: max(2.5 kHz, the mode's bandwidth) — see vibe_agc_rules.h §4.
        const double ssb = 2700.0, cw = 2500.0, am = 10000.0, nfm = 12500.0, wfm = 200000.0;
        ok(std::fabs(agcRetuneForgetHz(ssb) - 1350.0) < 1e-9, "SSB: a move of more than 1.35 kHz forgets");
        ok(std::fabs(agcRetuneForgetHz(cw)  - 1250.0) < 1e-9, "CW: 1.25 kHz (the 2.5 kHz floor, not the 1 kHz one)");
        ok(std::fabs(agcRetuneForgetHz(am)  - 5000.0) < 1e-9 && std::fabs(agcRetuneForgetHz(nfm) - 6250.0) < 1e-9,
           "AM 5 kHz, NFM 6.25 kHz");
        ok(std::fabs(agcRetuneForgetHz(wfm) - 100000.0) < 1e-9, "WFM: 100 kHz (Stuart's FM figure)");

        // Two USB stations 3 kHz apart on 20 m: hopping between them is a full forget every time.
        const double a = 14.200e6, b = 14.203e6;
        double learned = a;
        ok(agcRetuneForgets(learned, b, ssb), "14.200 -> 14.203 USB: FORGETS (hysteresis, margin, settled all cleared)");
        learned = b;
        ok(agcRetuneForgets(learned, a, ssb), "...and back again: forgets again");
        ok(!agcRetuneForgets(a, a + 1000.0, ssb), "a 1 kHz touch-up on the same SSB signal: kept");
        ok(!agcRetuneForgets(a, b, wfm), "the same 3 kHz on WFM: kept");

        // A drag in 100 Hz steps: measured from where it LAST forgot, so it forgets on the 14th step.
        learned = a; int forgets = 0, firstAt = -1;
        for (int k = 1; k <= 30; ++k) {
            const double f = a + 100.0 * k;
            if (agcRetuneForgets(learned, f, ssb)) { ++forgets; if (firstAt < 0) firstAt = k; learned = f; }
        }
        ok(firstAt == 14, "100 Hz drag steps: the first forget is at +1.4 kHz (step 14)");
        ok(forgets == 2, "a 3 kHz drag forgets twice (at +1.4 and +2.8 kHz)");
    }

    printf("\nagc rules: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
