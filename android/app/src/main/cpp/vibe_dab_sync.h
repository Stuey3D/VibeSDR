// vibe_dab_sync.h — DAB frame acquisition, stage 1: find the null symbol.
//
// ★★★ THE NULL SYMBOL IS THE ONLY THING IN A DAB SIGNAL YOU CAN FIND WITHOUT KNOWING ANYTHING
//     ELSE. Every 96 ms (Mode I) the transmitter goes quiet for 2656 samples. No carrier
//     recovery, no timing, no FFT — just a hole in the energy, once a frame, in a signal that is
//     otherwise flat noise-like across 1.536 MHz. So everything else hangs off finding it: the
//     phase reference is the symbol immediately after it, and the frame boundary is its end.
//
// ★★ ACQUIRE THEN TRACK, and they are different problems.
//     ACQUIRE: nothing is known, so scan a whole frame's worth of samples and take the global
//     minimum of the windowed energy. Costs one frame of latency and cannot be fooled by a quiet
//     passage, because it compares against the same frame's own average.
//     TRACK: the next null is 196608 samples after this one, give or take the receiver's clock
//     error. Search a SMALL window around the prediction. That is both cheaper and far more
//     robust — a deep fade that swallows one null does not throw the frame timing away, it just
//     fails to refine it.
//
// ★ Energy, not correlation. A matched filter against the null is tempting and pointless: the
//   null has no content to match, it is an ABSENCE. Correlating against silence finds silence
//   everywhere the signal happens to be weak.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "vibe_dab_modes.h"

namespace vibedab {

/** One complex sample, interleaved as the shim already carries them. */
struct Cplx { float re, im; };

/** Result of a search: where the null STARTS, and how confident we are.
 *  ★ `depth` is the ratio of the frame's mean power to the null window's mean power. A real Mode I
 *    null is 20 dB down or better; a fade might be 3 dB. Reporting the number rather than a
 *    boolean lets the caller decide, and lets the DX panel show it. */
struct NullSearch {
    bool   found   = false;
    size_t offset  = 0;      ///< index of the first sample of the null
    float  depth   = 0.0f;   ///< frameMeanPower / nullMeanPower (higher is better)
};

/** ★★ The minimum depth we will call a null. 4x power (6 dB) is deliberately generous: a real one
 *  is far deeper, and being generous here costs nothing because the phase-reference correlation
 *  downstream is what actually confirms the frame. Refusing a marginal null outright would mean a
 *  receiver that cannot start on a weak mux at all. */
inline constexpr float kMinNullDepth = 4.0f;

/** Search `n` samples for the deepest null-shaped dip. `nullLen` samples wide.
 *
 *  ★ ONE PASS, with a running sum: the window slides by one sample, so the cost is O(n) regardless
 *    of the null length. At 2.048 MHz a frame is 196608 samples and this runs on every one of them
 *    while unlocked — an O(n·nullLen) version would be 2656x that and would not keep up on a Pi.
 */
/** @param refMeanPower  Mean power to judge the null AGAINST, or 0 to use the search window's own.
 *  ★★★ THIS PARAMETER IS THE WHOLE TRACKING PATH. depth is meanAll/meanNull, and when the search
 *      window is only +/-64 samples around a predicted null it is ALMOST ENTIRELY NULL — so
 *      meanAll == meanNull, depth == 1, and `found` is false however perfect the lock. The TRACK
 *      branch could therefore never report a frame: it missed four times, dropped the lock, and
 *      re-acquired. That is why offer() has always been used with resetSync() on every frame,
 *      why syncConsumed() is dead code, and why wiring tracking up "properly" measured far WORSE
 *      (63 frames of 311) — the design was sound and its depth test was not.
 *  ★★ Re-acquiring every 96 ms is what produces the +/-1 sample jitter, the occasional +/-4, and
 *     with it the frames that decode as noise: acquisition takes a GLOBAL MINIMUM over a frame,
 *     so it only has to lose once. Measured on 60 s of captured air: three burst events, one of
 *     19 consecutive audio frames.
 *  ★ A caller that tracks passes the signal's own mean power, measured over the buffer. */
inline NullSearch findNull(const Cplx* x, size_t n, size_t nullLen, double refMeanPower = 0.0) {
    NullSearch r;
    /* ★★★ n >= nullLen, NOT nullLen * 2. The old guard demanded a span of two null lengths, which
     *  every ACQUISITION call satisfies and NO TRACKING call ever did: the ±64-sample window is
     *  nullLen + 128, so this returned the empty result — offset 0, depth 0 — on every tracked
     *  frame, and the prediction walked 64 samples backwards per frame until the lock was lost.
     *  That is the fault under the 4.1.89 "tracking measured worse" note, and the reason the
     *  parameter above could never do what it was added for. One window plus room to slide is
     *  all the running sum needs. */
    if (!x || nullLen == 0 || n < nullLen + 1) return r;

    // Running sum of |x|^2 over the window.
    double win = 0.0;
    for (size_t i = 0; i < nullLen; ++i) win += double(x[i].re) * x[i].re + double(x[i].im) * x[i].im;

    double total = win;                 // running total over everything seen, for the mean
    double best  = win;
    size_t bestAt = 0;

    /* ★★★ A RUNNING SUM DRIFTS, AND THE DRIFT CHOSE THE WRONG NULL. `win += add - sub` accumulates
     *  rounding over 200 000 samples, and under gcc -O2 on the Pi (which contracts a*b+c into a
     *  fused multiply-add, so `add` and `sub` round differently) the sum over a genuinely silent
     *  null went slightly NEGATIVE a few hundred samples after the true minimum. Strict `<` then
     *  moved bestAt to the END of the null, the frame overran the buffer, and test-dab-receiver
     *  failed at -O2 and passed at -O1 with no undefined behaviour anywhere. On air the null is
     *  never exactly zero, so the same drift shows as a few samples of timing jitter instead —
     *  see the note in FrameSync about tracking. Two fixes: the window is clamped at zero, and a
     *  new minimum has to beat the old one by more than rounding noise, so ties go to the FIRST
     *  position, which for a silent null is the true start of it. */
    const double eps = 1e-9 * (win + 1e-30);
    for (size_t i = nullLen; i < n; ++i) {
        const double add = double(x[i].re) * x[i].re + double(x[i].im) * x[i].im;
        const double sub = double(x[i - nullLen].re) * x[i - nullLen].re
                         + double(x[i - nullLen].im) * x[i - nullLen].im;
        win += add - sub;
        if (win < 0.0) win = 0.0;
        total += add;
        if (win < best - eps) { best = win; bestAt = i - nullLen + 1; }
    }

    const double meanAll  = refMeanPower > 0.0 ? refMeanPower : total / double(n);
    const double meanNull = best  / double(nullLen);
    // ★ A perfectly silent null gives meanNull == 0; report a large depth rather than dividing by
    //   zero. Synthetic test signals do exactly this, and so does a muted input.
    r.depth  = meanNull > 0.0 ? float(meanAll / meanNull) : 1e9f;
    r.found  = r.depth >= kMinNullDepth;
    r.offset = bestAt;
    return r;
}

/** Frame-synchroniser: acquires once, then tracks.
 *
 *  ★★ THE TRACKING WINDOW IS THE WHOLE POINT. Once locked we only look +/- `slack` samples around
 *     where the next null is predicted, which is what makes this cheap AND what stops a momentary
 *     fade re-acquiring from scratch and losing the audio for a frame.
 */
/* ★★★ THE NULL SYMBOL FINDS A CANDIDATE; THE PHASE REFERENCE CONFIRMS IT. That is the split the
 *  reference receivers make and this one did not, and it is why 9A never locked on the Pi while
 *  OpenWebRX on the same aerial model decoded it cleanly (Stuart, 2026-09-08).
 *
 *  ★★★ WHAT WAS WRONG. Lock was decided HERE, from the null symbol's energy dip alone, against a
 *      fixed depth of 4 (6 dB): meanAll / meanNull is 1 + S/N, so a signal below ~5 dB SNR could
 *      NEVER lock — however decodable it was (DQPSK with rate-1/3 coding decodes at ~5–7 dB). And
 *      the service reset this object after EVERY frame, so the ±64-sample tracking branch below
 *      never ran once: every frame was a cold acquisition needing that 6 dB. On 9A the depth sat
 *      at 1–2 and the receiver reported "searching" for a minute with a PRS correlation of 0.05.
 *
 *  ★★ THE SHAPE NOW. offer() returns the best null position it can find — a CANDIDATE, with a
 *     deliberately low depth floor that only rejects flat noise. The receiver then correlates the
 *     symbol after it against the phase reference (the impulse response it already computes for
 *     the panel) and calls confirm() or reject(). All 1536 carriers of the PRS add coherently in
 *     that impulse, so its peak stands 30 dB over the noise floor at an SNR where the null dip is
 *     invisible. Lock, timing and the miss count are therefore decided by the thing that cannot
 *     be fooled by noise, and the null does only what it is good at: saying roughly where.
 *  ★ The 4.1.89 note in vibe_dab_service.h records the previous attempt at tracking measuring
 *    worse ON AIR. It changed what was CONSUMED, not the detector; this changes the detector and
 *    leaves consumption exactly as it was. Proved on the 12B capture before it went anywhere
 *    near a radio: same 452 clean frames. */
class FrameSync {
public:
    explicit FrameSync(const Mode& m, uint32_t sampleRateHz = kCanonicalRateHz)
        :
          nullLen_(size_t((uint64_t(m.nullSamples)  * sampleRateHz) / kCanonicalRateHz)),
          frameLen_(size_t((uint64_t(m.frameSamples) * sampleRateHz) / kCanonicalRateHz)) {}
    bool   locked()    const { return locked_; }
    size_t frameLen()  const { return frameLen_; }
    size_t nullLen()   const { return nullLen_; }
    /** The confirmed frame's depth while locked; the current candidate's while not. */
    float  lastDepth() const { return locked_ ? depth_ : candDepth_; }
    /** A candidate frame start, or -1 when there is nothing worth testing. Not a lock: the caller
     *  must confirm() or reject() it. */
    long offer(const Cplx* x, size_t n, size_t slack = 64) {
        if (!x || n < frameLen_) return -1;
        if (locked_) {
            const size_t centre = predicted_;
            const size_t lo = centre > slack ? centre - slack : 0;
            const size_t hiEnd = centre + slack + nullLen_;
            if (hiEnd <= n) {
                double ref = 0.0; size_t cnt = 0;
                for (size_t i = 0; i < n; i += 64) {
                    ref += double(x[i].re) * x[i].re + double(x[i].im) * x[i].im; ++cnt;
                }
                ref = cnt ? ref / double(cnt) : 0.0;
                /* ★ Tracking: the best dip in the window is the candidate WHATEVER its depth. At the
                 *  SNR this exists for, the depth is meaningless; the phase reference decides. */
                NullSearch s = findNull(x + lo, (hiEnd - lo), nullLen_, ref);
                candDepth_ = s.depth;
                return long(lo + s.offset);
            }
            /* The prediction has run off the end of what we hold: count it as a miss and move on. */
            reject();
            return -1;
        }
        const size_t span = frameLen_ + nullLen_;
        NullSearch s = findNull(x, n < span ? n : span, nullLen_);
        candDepth_ = s.depth;
        if (s.depth < kCandidateDepth) return -1;          // flat noise: nothing to test
        return long(s.offset);
    }
    /** The phase reference agreed: this is a frame. `at` may carry the impulse-response timing
     *  correction, so the next prediction is centred where the strongest path actually is. */
    void confirm(long at) {
        locked_    = true;
        depth_     = candDepth_;
        predicted_ = size_t(at < 0 ? 0 : at) + frameLen_;
        misses_    = 0;
    }
    /** The phase reference did not agree. A locked receiver keeps its prediction moving and drops
     *  the lock after kMaxMisses; an unlocked one simply searches again next frame. */
    void reject() {
        if (!locked_) return;
        if (++misses_ >= kMaxMisses) { locked_ = false; misses_ = 0; return; }
        predicted_ += frameLen_;
    }
    void reset() { locked_ = false; misses_ = 0; predicted_ = 0; depth_ = 0.0f; candDepth_ = 0.0f; }
    void consumed(size_t n) { predicted_ = predicted_ > n ? predicted_ - n : 0; }
private:
    /** ★ Only flat noise is rejected here (1 dB of dip); a real but weak signal passes to the
     *  phase-reference test, which is where the decision belongs. */
    static constexpr float kCandidateDepth = 1.25f;
    static constexpr int   kMaxMisses = 4;
    size_t nullLen_, frameLen_;
    bool   locked_    = false;
    int    misses_    = 0;
    size_t predicted_ = 0;
    float  depth_     = 0.0f;
    float  candDepth_ = 0.0f;
};

}  // namespace vibedab
