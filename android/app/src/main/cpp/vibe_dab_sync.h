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
    if (!x || n < nullLen * 2 || nullLen == 0) return r;

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
class FrameSync {
public:
    explicit FrameSync(const Mode& m, uint32_t sampleRateHz = kCanonicalRateHz)
        :
          nullLen_(size_t((uint64_t(m.nullSamples)  * sampleRateHz) / kCanonicalRateHz)),
          frameLen_(size_t((uint64_t(m.frameSamples) * sampleRateHz) / kCanonicalRateHz)) {}

    bool   locked()    const { return locked_; }
    size_t frameLen()  const { return frameLen_; }
    size_t nullLen()   const { return nullLen_; }
    /** Depth of the last accepted null — the honest "how good is this signal" number. */
    float  lastDepth() const { return depth_; }

    /** Offer a buffer holding at least one whole frame. Returns the offset of the null that starts
     *  the next frame, or -1 when nothing convincing was found.
     *  ★ `slack` is how far the prediction may drift between frames; at 2.048 MHz and a 50 ppm
     *    dongle that is ~10 samples a frame, so the default is comfortable rather than tight. */
    long offer(const Cplx* x, size_t n, size_t slack = 64) {
        if (!x || n < frameLen_) return -1;

        if (locked_) {
            // TRACK: look only around where the next null is due.
            const size_t centre = predicted_;
            const size_t lo = centre > slack ? centre - slack : 0;
            const size_t hiEnd = centre + slack + nullLen_;
            if (hiEnd <= n) {
                /* ★ The signal's mean power, sampled cheaply across the WHOLE buffer — this is
                 *  what the null has to be deep against. Strided so tracking stays cheap, which
                 *  was always the point of tracking. */
                double ref = 0.0; size_t cnt = 0;
                for (size_t i = 0; i < n; i += 64) {
                    ref += double(x[i].re) * x[i].re + double(x[i].im) * x[i].im; ++cnt;
                }
                ref = cnt ? ref / double(cnt) : 0.0;
                NullSearch s = findNull(x + lo, (hiEnd - lo), nullLen_, ref);
                if (s.found) {
                    depth_ = s.depth;
                    const size_t at = lo + s.offset;
                    predicted_ = at + frameLen_;
                    misses_ = 0;
                    return long(at);
                }
            }
            // ★ A miss is not a loss. Carry the prediction forward and try the next frame; only a
            //   run of them means we are genuinely off the signal, because a single deep fade is
            //   ordinary on a marginal mux and re-acquiring would cost a whole frame of audio.
            if (++misses_ >= kMaxMisses) { locked_ = false; misses_ = 0; }
            predicted_ += frameLen_;
            return -1;
        }

        /* ACQUIRE: nothing known — scan exactly ONE frame plus a null length.
         * ★★★ NOT TWO FRAMES. Searching two means two nulls are in range and the global minimum
         *     picks whichever noise made deeper — so acquisition landed on the SECOND one, a whole
         *     frame late, and everything downstream would have been offset by 96 ms. The test
         *     caught it immediately ("acquisition lands on the null" failed while "acquires"
         *     passed, which is the signature of finding A null rather than THE null).
         * ★ frameLen + nullLen is the right window: nulls are exactly frameLen apart, so any span
         *   that long contains exactly one WHOLE null wherever it starts. */
        const size_t span = frameLen_ + nullLen_;
        NullSearch s = findNull(x, n < span ? n : span, nullLen_);
        if (!s.found) return -1;
        locked_    = true;
        depth_     = s.depth;
        predicted_ = s.offset + frameLen_;
        misses_    = 0;
        return long(s.offset);
    }

    void reset() { locked_ = false; misses_ = 0; predicted_ = 0; depth_ = 0.0f; }

    /** ★★★ THE CALLER CONSUMED `n` SAMPLES — REBASE, DO NOT RESET.
     *  `predicted_` is an index into the buffer handed to offer(), so a caller that drops the
     *  front of that buffer must say so or the prediction points `n` samples too far ahead. This
     *  is what lets the TRACK path survive across calls, which is the whole reason it exists. */
    void consumed(size_t n) { predicted_ = predicted_ > n ? predicted_ - n : 0; }

private:
    /** ★ Four frames of nothing (~0.4 s in Mode I) before we admit we are lost. Long enough to ride
     *  out a fade under a bridge, short enough that a retune does not sit on a dead lock. */
    static constexpr int kMaxMisses = 4;

    // ★ The mode is captured in the derived lengths below; keeping a pointer we never read was
    //   dead weight the compiler correctly noticed.

    size_t nullLen_, frameLen_;
    bool   locked_    = false;
    size_t predicted_ = 0;
    int    misses_    = 0;
    float  depth_     = 0.0f;
};

}  // namespace vibedab
