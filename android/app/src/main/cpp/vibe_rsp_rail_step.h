// ★★★ DID THE RADIO ANSWER OUR LAST RF STEP? — the RSP RF loop's open-loop test, as a pure function
//     (2026-10-16, Lenovo RSP1A: "the SDRplay RF gain went a bit haywire and decided to go maximum and the
//     SDR showed overload and it didn't respond").
//
// The RF loop (vsSdrplayRfAgcTick, local_sdr_shim.cpp) steers the LNA by the IF AGC's reported reduction.
// After each of its own LNA steps it asks whether that number ANSWERED: a rung is 6-21 dB and the IF AGC
// has 39 dB of range, so a live loop cannot sit still through it. Two unanswered steps = the readout is
// frozen (rsp_gain_readout_froze_not_the_gain, 2026-09-13) and the loop holds.
//
// ★★★ THE HOLE: A RAIL WAS EXEMPT ON THE REDUCTION'S OWN SAY-SO. At 20 (or 59) the IF has nothing left to
//     give, so a reading that stays put through a step is what a REAL rail looks like too — and since
//     2026-09-15 a rail reading never counted against the readout. So a readout frozen AT a rail walked
//     the LNA to its end stop with every safeguard satisfied. 2026-10-16 17:51:45-17:52:32 on 96-100 MHz:
//         IF reduction averaged 20.0 dB ... RF gain state 8 -> 7 (ADC peak -26.4 dBFS, system gain 30.4 dB)
//         20.0 ... 7 -> 6 (-22.9, 49.6)   20.0 ... 6 -> 5 (-23.4, 55.4)   20.0 ... 5 -> 4 (-21.0, 61.7)
//         20.0 ... 4 -> 3 (-19.3, 67.9)   25.0 ... 3 -> 2 (-19.2)         25.0 ... 2 -> 1 (-20.3)
//         HOLDING — ... read exactly 25 dB through 2 of our own LNA steps ... Holding RF gain at state 1.
//     ~45 dB of front end added and OUR OWN ADC peak moved 6 dB. If the IF really had been at its max-gain
//     rail it could not have absorbed a single decibel of that: the peak would have climbed rung for rung.
//     It didn't, so the IF WAS answering and only its readout was dead. The front end ended at RF 1 of 9 on
//     the FM band — overload. The same walk is in this server's journal on 10-01 (2->1->0), 10-02
//     (4->3->2->1->0, peak -23.5 -28.4 -26.2 -21.3) and 10-12 (8->6->5->4), all long before the 10-16
//     thread split: this is the rail exemption, not the threads.
//
// ★★ SO AT A RAIL THE STEP MUST SHOW IN WHAT WE MEASURE. The IF cannot absorb a step it has no range left
//    for, so the ADC peak (ours, from the samples — never fed by the API's event path) must move with the
//    rung, in the rung's direction. Under half the rung = absorbed = the IF answered = the readout is
//    frozen. The rung is what the API's own gainVals.curr moved by across OUR write (it is refreshed on
//    every Update_Tuner_Gr, so it measures the step whether or not the AGC is reporting).
//  ★ A STARVED RAIL STAYS EXEMPT. At the 20 rail with the level well under the AGC's set point the IF is
//    genuinely wide open and the band may be nothing but noise — which need not climb with the LNA once the
//    mixer's own noise dominates. Stuart on 40 m wanted that rung given back ("we seem to be reluctant to
//    give RF gain back"); the peak test must not take it away. Below set point − 3 the rail stands.
//  ★ Too small a rung (< kMinJudgeRungDb), or no peak, or no gain readback: INCONCLUSIVE — neither
//    evidence for the readout nor against it. The counters are left as they were.
//
// Pure: no clock, no radio — tested in vibeserver/test-rsp-rail-step.cpp with the 10-16 numbers.
#pragma once

#include <cmath>

namespace vibersp {

enum class RfStepVerdict {
    Answered,      // the reduction moved, or a genuine rail the samples confirm
    Frozen,        // the write landed and nothing we steer by answered it
    Unhonoured,    // our write never reached the radio (gainVals.curr did not move)
    Inconclusive,  // a rail we cannot check (rung too small, no peak, no readback)
};

constexpr double kMinJudgeRungDb   = 5.0;   // smaller rungs hide in the peak's own wander
constexpr double kRungShareDb      = 0.5;   // the peak must carry at least half the rung
constexpr double kStarvedBelowSetDb = 3.0;  // 20-rail exemption: peak this far under the set point

struct RfStepEvidence {
    int    grAtStep      = -1;       // the reduction we stepped ON (rounded window mean)
    double grMeanNow     = 0.0;      // the settled window mean since
    float  structAtStep  = -999.0f;  // gainVals.curr before our write (-999 = unreadable)
    float  structNow     = -999.0f;  // ... and now
    double peakAtStep    = NAN;      // our ADC peak, window mean, at the step (dBFS)
    double peakNow       = NAN;      // ... and now
    int    dir           = 0;        // +1 LNA state up (LESS RF gain), -1 state down (MORE)
    int    setPointDbfs  = -30;      // the IF AGC's set point
};

inline bool rfReadingAtRail(double grMean) { return grMean <= 20.5 || grMean >= 58.5; }

inline RfStepVerdict judgeRfStep(const RfStepEvidence& e) {
    const bool haveG    = e.structNow > -998.0f && e.structAtStep > -998.0f;
    const bool wrLanded = haveG && std::fabs(e.structNow - e.structAtStep) > 0.05f;
    if (haveG && !wrLanded) return RfStepVerdict::Unhonoured;
    if ((int)std::lround(e.grMeanNow) != e.grAtStep) return RfStepVerdict::Answered;
    if (!rfReadingAtRail(e.grMeanNow)) return RfStepVerdict::Frozen;     // off a rail: the 09-13 test

    // ★ At a rail. A starved one (max-gain rail, level well under target) is the IF genuinely wide open.
    const bool maxGainRail = e.grMeanNow <= 20.5;
    if (maxGainRail && std::isfinite(e.peakNow) && e.peakNow < e.setPointDbfs - kStarvedBelowSetDb)
        return RfStepVerdict::Answered;

    const double rung = haveG ? std::fabs((double)e.structNow - (double)e.structAtStep) : NAN;
    if (!std::isfinite(rung) || rung < kMinJudgeRungDb || e.dir == 0 ||
        !std::isfinite(e.peakAtStep) || !std::isfinite(e.peakNow))
        return RfStepVerdict::Inconclusive;
    // More RF gain (dir -1) must raise the peak; less (dir +1) must lower it.
    const double moved = (e.peakNow - e.peakAtStep) * (e.dir < 0 ? 1.0 : -1.0);
    return moved >= rung * kRungShareDb ? RfStepVerdict::Answered : RfStepVerdict::Frozen;
}

}  // namespace vibersp
