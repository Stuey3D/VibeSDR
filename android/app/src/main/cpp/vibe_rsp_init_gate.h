// ★★★ THE "SDRplay AGC INITIALISING" INDICATOR, AS A GATE THAT CANNOT WEDGE (2026-10-06).
//
// Saber's RSP1: the initialising notice went up and NEVER came down, and because the app shows it
// in the VTS it sat over RDS and the bookmark name — the ADVANCED RDS panel was decoding SLAM!
// perfectly underneath (his screenshots, 92.6 MHz, RF AGC OFF, IF AGC ON). His gain stage turned
// out to be faulty, but the wedge was ours, and it did not need a faulty radio:
//
//   · `sdrpInitAgc` was cleared in exactly ONE place — six seconds after the RF loop's coarse
//     placement — and that placement only runs with "Automatic RF gain" (`rfAgc`) ON. `rfAgc`
//     DEFAULTS TO OFF (vibeserver/main.cpp). So with the RF loop off the kick ran, the notice went
//     up, and nothing that could take it down was reachable. Nothing waited on an API readout; the
//     exit simply did not exist on that path.
//   · The kick only advances while the AGC is wanted and the API has not reported a failure, so a
//     kick left part-way (AGC switched off mid-kick, a failure loop) re-raised it on every tick.
//   · A manual-gain start (no kick) never cleared it either: the member started TRUE.
//
// ★★ So the gate ends on the FIRST of:
//      Complete        — the full cycle ran: kick, grace, coarse placement, six seconds after it;
//      NoPlacement     — the kick is over and nothing further is coming (RF loop off — the
//                        DEFAULT, and Saber's setting — or the AGC switched off): a short margin
//                        and it goes;
//      GainNoResponse  — the kick moved the gain and OUR OWN measurements did not move (below);
//      TimedOut        — kInitMaxSec from the start, whatever the radio did. The notice says
//                        "approx. 30 seconds"; a healthy RSP1A with the RF loop on finishes in
//                        ~33 s (Stuart's 648 kHz recording, 2026-10-06), so 60 leaves it room
//                        and is still the most anyone should ever watch it.
//
// ★★★ "THE GAIN NEVER MOVES AT ALL" — JUDGED ON WHAT WE MEASURE, NOT ON WHAT THE API SAYS.
//     Stuart: "Saber's gain never moves at all, the signals and noise floor never bounce about like
//     they do on mine that initialises perfectly." The kick is a known set of gain moves — step 1
//     puts the LNA at position 1, steps 3→4→5 walk the IF reduction 59→55→59 (4 dB each way), and
//     step 6 hands a 59 dB reduction to the IF AGC, which then pulls tens of dB of gain in. On a
//     working radio our band-edge noise floor and the ADC peak swing with all of that. So from the
//     start of the kick to kJudgeAfterHandoverSec after the handover we keep the min and max of
//     both; if the floor moved less than kMinFloorSwingDb AND the peak less than kMinPeakSwingDb,
//     the gain is not reaching the samples.
//   ★ Every API-fed figure (gRdB, gainVals, GainChange) can freeze while the samples stay honest
//     (rsp_gain_readout_froze_not_the_gain) — that is why none of them is an input here.
//   ★★ A WORKING RADIO MUST NOT TRIP IT, so the test only fires when BOTH stay flat: the floor
//      threshold is half the kick's smallest deliberate step (4 dB), and the peak — noisier,
//      modulation moves it — needs a full 6 dB before it counts as the gain moving. Either moving
//      is enough to call the radio responsive.
//   ★ It cannot tell a dead gain stage from an API whose gain writes are all being ignored — from
//     the samples' side those are the same fact, and the log says both.
//
// ★★ AND A RADIO THAT TIMED OUT (OR DID NOT RESPOND) DOES NOT RAISE IT AGAIN STRAIGHT AWAY. A stream
//    restarting every half-minute would otherwise re-kick, re-raise and re-end for ever — the same
//    wedge with a gap in it. For kRetryQuietSec a new cycle runs silently. ★ Never permanently
//    (never_limit_permanently): after the quiet spell it may show again, and a cycle in which the
//    gain DOES move clears the diagnostic.
//
// Pure, no clock of its own and no radio — `now` is seconds on any monotonic clock — so the stuck
// cases are tested without hardware (vibeserver/test-rsp-init-gate.cpp).
#pragma once

#include <chrono>

namespace vibersp {

constexpr double kInitMaxSec            = 60.0;   // hard ceiling on the indicator, from cycle start
constexpr double kNoPlacementSec        = 6.0;    // kick done, nothing more coming: this long, then go
constexpr double kRetryQuietSec         = 300.0;  // after a timeout / no response, cycles stay silent
constexpr double kJudgeAfterHandoverSec = 6.0;    // the IF AGC's own pull-in, after step 6
constexpr double kMinFloorSwingDb       = 2.0;    // half the kick's smallest deliberate step (4 dB)
constexpr double kMinPeakSwingDb        = 6.0;    // the peak breathes with modulation: ask for more

/** Seconds on the steady clock — the gate's callers share one ruler. */
inline double monoNowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

enum class InitEnd { None, Complete, NoPlacement, GainNoResponse, TimedOut };

inline const char* initEndName(InitEnd e) {
    switch (e) {
        case InitEnd::Complete:       return "complete";
        case InitEnd::NoPlacement:    return "kick done, no RF placement to wait for";
        case InitEnd::GainNoResponse: return "gain stage not responding";
        case InitEnd::TimedOut:       return "timed out";
        default:                      return "none";
    }
}

struct InitGate {
    bool   showing      = false;
    double startedAt    = -1.0;
    double kickDoneAt   = -1.0;
    double lastGiveUp   = -1e18;   // a timeout or a no-response verdict
    bool   suppressed   = false;   // the current cycle is running silently

    // ── the response test, per cycle ──
    double judgeAt      = -1.0;    // when to judge (handover + kJudgeAfterHandoverSec); -1 = not yet
    bool   judged       = false;
    double floorMin = 1e9, floorMax = -1e9, peakMin = 1e9, peakMax = -1e9;
    /** ★ The owner's diagnostic: the last judged cycle moved the gain and nothing moved. Cleared
     *  by a later cycle that does respond — never a permanent verdict. */
    bool   gainNoResponse = false;
    double lastFloorSwing = 0.0, lastPeakSwing = 0.0;

    /** A new start-up cycle has begun (kick step 1). Returns whether the indicator goes up.
     *  ★ Raised for EVERY kick, RF loop on or off: the kick drives the radio's own IF AGC too, and
     *    the floor bounces while it does (Stuart, 2026-10-06: "the AGC initialising is because we
     *    kick the IF AGC too"). What was wrong was only ever the ending.
     *  `floorDb` / `peakDbfs` are the levels BEFORE the kick's first write lands — the baseline. */
    bool begin(double now, double floorDb, double peakDbfs) {
        startedAt = now; kickDoneAt = -1.0;
        judgeAt = -1.0; judged = false;
        floorMin = peakMin = 1e9; floorMax = peakMax = -1e9;
        sample(floorDb, peakDbfs);
        suppressed = (now - lastGiveUp) < kRetryQuietSec;
        showing = !suppressed;
        return showing;
    }

    /** Kick step 6: the IF AGC has the gain. The verdict comes kJudgeAfterHandoverSec later. */
    void handover(double now) { if (!judged) judgeAt = now + kJudgeAfterHandoverSec; }

    /** Our own measurements, every tick of the cycle. Sentinels (≤ -99 floor, ≤ -199 peak) are
     *  "not measured yet" and ignored. */
    void sample(double floorDb, double peakDbfs) {
        if (judged || startedAt < 0) return;
        if (floorDb > -99.0)  { if (floorDb < floorMin) floorMin = floorDb; if (floorDb > floorMax) floorMax = floorDb; }
        if (peakDbfs > -199.0){ if (peakDbfs < peakMin) peakMin = peakDbfs; if (peakDbfs > peakMax) peakMax = peakDbfs; }
    }

    /** Once per tick.
     *  kickRunning       — the six-step kick has not finished (sdrpSettling);
     *  placementPending  — the coarse placement WILL run once the kick is over (RF loop on and the
     *                      IF AGC wanted); false means the kick is the whole cycle;
     *  placementComplete — the original end: coarse placement done and its six seconds served.
     *  Returns how the indicator ended on THIS tick, or None. The response verdict is taken even
     *  on a silent cycle, so the diagnostic stays true to the latest evidence. */
    InitEnd tick(double now, bool kickRunning, bool placementPending, bool placementComplete) {
        if (!judged && judgeAt >= 0 && now >= judgeAt) {
            judged = true;
            lastFloorSwing = floorMax > floorMin ? floorMax - floorMin : 0.0;
            lastPeakSwing  = peakMax  > peakMin  ? peakMax  - peakMin  : 0.0;
            const bool moved = lastFloorSwing >= kMinFloorSwingDb || lastPeakSwing >= kMinPeakSwingDb;
            gainNoResponse = !moved;
            if (!moved) {
                lastGiveUp = now;
                if (showing) { showing = false; return InitEnd::GainNoResponse; }
            }
        }
        if (!showing) return InitEnd::None;
        if (kickRunning) kickDoneAt = -1.0;
        else if (kickDoneAt < 0) kickDoneAt = now;
        if (!kickRunning && placementComplete) { showing = false; return InitEnd::Complete; }
        if (!kickRunning && !placementPending && kickDoneAt >= 0
            && now - kickDoneAt >= kNoPlacementSec && (judged || judgeAt < 0)) {
            showing = false; return InitEnd::NoPlacement;
        }
        if (now - startedAt >= kInitMaxSec) {
            showing = false; lastGiveUp = now; return InitEnd::TimedOut;
        }
        return InitEnd::None;
    }
};

}  // namespace vibersp
