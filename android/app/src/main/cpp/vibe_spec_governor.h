// vibe_spec_governor.h — the waterfall gives way to the CPU: a dynamic spectrum ceiling for a box that cannot cope.
//
// ★★★ WHY (Pi 2, 2026-10-17): after 86f10ea1 made the spectrum handoff a 4-slot queue, a Pi 2 asked for 20 fps chased it
//     flat out — vibe-spec ~90 % of a core beside vibe-dab ~72 %, load 4-6, and the DAB audio broke up. Capped by hand at
//     5 fps: vibe-spec 30 %, load 2.5, "audio perfect". Stuart (13:45): "deliberately holding back the waterfall if a
//     server cannot cope is a good idea, especially useful for DAB and Advanced RDS. Server may limit itself to 5 or 10
//     when starved for CPU but can ramp up to 20 if say listening to SSB on HF."
//
// ★★★ THE AGREED DESIGN, and what each word means here:
//   · The OWNER's maxFps stays the hard top, and a LISTENER may still ask for less (per-listener decimation) — this only
//     ever LOWERS the engine rate below what those two already allow. ceiling() == 0 means "no ceiling of mine".
//   · It lowers the rate ONLY while the box is STARVED, judged from the MEASURED load — the DSP chain's share of real
//     time (dspCpu), the demod queue blocking (the worst-block signal: a block that cannot be handed on), the IQ backlog,
//     the whole machine at CRITICAL — NEVER from the frame shortfall. The shortfall lags the audio trouble: by the time
//     the waterfall is visibly short, the audio has been breaking up for seconds.
//   · FAST DOWN, SLOW UP, and no hunting: steps 20 → 10 → 5. A step down after ~4 s of starvation; one rung at a time
//     with a settle between (the load figures are 2-5 s windows, so the next judgement sees the new rate). A step up
//     only after a long calm spell (60 s); an up-step that starves again within 2 minutes steps straight back down and
//     DOUBLES the calm needed before the next try (to 64 min), so a box that sits on the edge settles on the lower rung
//     instead of see-sawing — at worst one ~4 s probe an hour. The penalty is forgiven after a quiet 2 h, or at once
//     when the circumstances change (newCircumstances(): a mode change or DAB on/off — the load is a different load,
//     which is exactly Stuart's "ramp up to 20 if say listening to SSB on HF").
//   · A middle band (busy, not starved) holds where it is — neither timer runs.
//
// ★★ INDEPENDENT OF THE AUDIO-FIRST GATE (vibe_wifi_qos.h, dd9983c2), and composable with it: this sets the ENGINE's rate
//    (frames computed, for the whole radio); the gate drops whole frames per LISTENER on the way out when that listener's
//    audio is queued on the network. A frame never computed cannot be sent, and a computed one the gate drops is not
//    sent — so each listener gets the LOWER of the two, with no code that knows about both. Proven in
//    test-spec-governor.cpp on the real AudioFirst class.
// ★ VIBESERVER_SPEC_GOVERNOR=0 turns it off (the caller reads it): the engine then runs exactly as before.
// Pure logic on a caller-supplied clock: deterministic, tested in vibeserver/test-spec-governor.cpp.
#pragma once
#include <algorithm>
#include <cstdint>

namespace vibespec {

/** What the governor is told on each tick — the server's own load figures (the same ones tune pacing reads). */
struct Load {
    double dspPct       = 0;     ///< dspCpu: the DSP chain's share of real time (>100 = cannot keep up)
    bool   demodBlocked = false; ///< the DSP thread blocked on a FULL demod queue recently (a block that could not move on)
    double backlogMs    = 0;     ///< IQ queued ahead of the DSP, in ms of signal
    int    cpuLevel     = 0;     ///< the health pill's CPU rung: 0 OK, 1 WARM, 2 HIGH (>=75 %), 3 CRIT (>=90 %)
};

/** ★ Starved: the audio is (or is about to be) paying for the waterfall. Any one is enough. */
inline bool starved(const Load& l) {
    return l.dspPct >= 90.0 || l.demodBlocked || l.backlogMs >= 250.0 || l.cpuLevel >= 3;
}
/** ★ Calm: room to spare on every count — only then may the waterfall climb back. */
inline bool calm(const Load& l) {
    return l.dspPct < 60.0 && !l.demodBlocked && l.backlogMs < 100.0 && l.cpuLevel <= 1;
}

class Governor {
public:
    static constexpr double kRungs[2]       = {10.0, 5.0};   // the steps below "unlimited": 20 → 10 → 5
    static constexpr double kDownAfterMs    = 4000.0;        // starved this long ⇒ one rung down (fast down)
    static constexpr double kSettleMs       = 8000.0;        // after any change, judge nothing for this long
    static constexpr double kUpAfterMs      = 60000.0;       // calm this long ⇒ one rung up (slow up)…
    static constexpr double kUpMaxMs        = 64 * 60000.0;  //   …doubling per failed try, to this
    static constexpr double kProbationMs    = 120000.0;      // starved this soon after an up-step = a failed try
    static constexpr double kForgiveMs      = 120 * 60000.0; // the doubled wait is forgiven after this long

    /** One tick (every ~2 s). `wantFps` = what the engine would run at without this governor (the fastest listener's
     *  rate, already under the owner's ceiling). Returns true when ceiling() changed — the caller re-applies the rate. */
    bool feed(double nowMs, const Load& l, double wantFps) {
        const double before = ceil_;
        const bool st = starved(l), ca = calm(l);
        if (st) { if (starvedSince_ < 0) starvedSince_ = nowMs; } else starvedSince_ = -1;
        if (ca) { if (calmSince_ < 0) calmSince_ = nowMs; } else calmSince_ = -1;
        if (upWaitMs_ > kUpAfterMs && nowMs - lastFailMs_ >= kForgiveMs) upWaitMs_ = kUpAfterMs;
        if (nowMs - changedAt_ < kSettleMs) return false;

        if (st && nowMs - starvedSince_ >= kDownAfterMs) {
            const double running = effective(wantFps);
            double next = -1;
            for (double r : kRungs) if (r < running - 0.01) { next = r; break; }   // the highest rung that is a cut
            if (next > 0) {
                if (lastUpAt_ >= 0 && nowMs - lastUpAt_ < kProbationMs) {           // the climb did not hold
                    upWaitMs_ = std::min(kUpMaxMs, upWaitMs_ * 2.0);
                    lastFailMs_ = nowMs;
                }
                lastUpAt_ = -1;
                ceil_ = next;
                ++downs_;
                mark(nowMs);
            }
        } else if (ca && ceil_ > 0 && nowMs - calmSince_ >= upWaitMs_) {
            ceil_ = (ceil_ < kRungs[0] - 0.01) ? kRungs[0] : 0.0;                   // 5 → 10 → unlimited
            lastUpAt_ = nowMs;
            ++ups_;
            mark(nowMs);
        }
        return ceil_ != before;
    }

    /** ★ The load is a different load now (a mode change, DAB on or off): what the failed climbs taught no longer
     *  applies, so the next climb waits only the base calm spell. The ceiling itself stays until the load says so. */
    void newCircumstances() { upWaitMs_ = kUpAfterMs; lastUpAt_ = -1; }

    /** The fps ceiling in force, or 0 for none. */
    double ceiling() const { return ceil_; }
    /** `wantFps` with the ceiling applied. */
    double effective(double wantFps) const { return ceil_ > 0 ? std::min(wantFps, ceil_) : wantFps; }
    double upWaitMs() const { return upWaitMs_; }
    uint64_t downs() const { return downs_; }
    uint64_t ups() const { return ups_; }

private:
    void mark(double nowMs) { changedAt_ = nowMs; starvedSince_ = calmSince_ = -1; }
    double ceil_ = 0;                  // 0 = no ceiling
    double starvedSince_ = -1, calmSince_ = -1;
    double changedAt_ = -1e18;
    double lastUpAt_ = -1;             // the last up-step, while it is on probation (-1 = none)
    double upWaitMs_ = kUpAfterMs;
    double lastFailMs_ = -1e18;
    uint64_t downs_ = 0, ups_ = 0;
};

}  // namespace vibespec
