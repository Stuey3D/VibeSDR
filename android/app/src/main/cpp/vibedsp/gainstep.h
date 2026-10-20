// VibeSDR — the front end's own gain steps, made level-continuous before anything downstream sees them.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <vector>

namespace vibedsp {

/**
 * ★★★ GainStepSmoother — OUR OWN GAIN CHANGE IS NOT A CHANGE IN THE SIGNAL, SO DO NOT LET THE DSP
 *     HEAR IT AS ONE (2026-10-19).
 *
 * Stuart, on the Lenovo's RSP1A: "RF gain movement always caused a pop to the signals even in MW and
 * that pop is what kills stereo." Every LNA rung the RF AGC takes (up to ~21 dB on FM) arrives in the
 * IQ as an abrupt level step, and everything after it that integrates level reads the step as a fact
 * about the signal. MEASURED on the real RxPipeline (vibeserver/test-gain-step.cpp), before this:
 *   · AM/MW: a −15 dB step is a thump 7.6 dB OVER the programme (the carrier DC blocker sees the
 *     carrier vanish), then a 1.3 s hole while the audio AGC releases; +15 dB is an 18 dB thump.
 *   · WFM: the multipath meter reads the step as 60-75 % "multipath". That engages CEQ on a perfectly
 *     clean station for 1-6 s (CEQ on a clean RSP signal is what wrecked RDS and the pilot reading
 *     on 96.6, 2026-09-12) and, with CEQ off, IMS throws the L−R corner 8.5 kHz down — stereo
 *     audibly collapsing towards mono, then taking ~4 s to reopen. With the RF AGC allowed to step
 *     every ~6 s, it never fully reopens.
 *   · WFM, marginal station: an UP-step trips the impulse blanker's hold, a click at 0 dB re programme.
 *   ★ The pilot PLL itself did NOT unlock in any synthetic case: an IQ step kicks a 190 Hz loop by
 *     ~0.01 rad and ~1.5 Hz. What "kills stereo" on the bench is the meters, not the lock.
 *
 * ★★ THE FIX IS AT THE SOURCE OF THE FAULT: the raw capture, before the shared pipeline, the
 *    channelizer, the waterfall and DAB. At the step the level is put back where it was (multiply by
 *    the inverse of the measured step), then the correction GLIDES away exponentially in dB. Downstream
 *    sees the station change level over ~a second, as a fade would, instead of in one sample — so the
 *    AM DC blocker and audio AGC follow it, the multipath meter's 5 kHz band sees nothing, and the
 *    blanker has no edge to trip on. One fix, every mode, every listener.
 * ★★ THE STEP IS MEASURED, NOT LOOKED UP. The RSP's LNA ladder is only ever approximately known (the
 *    shim learns it), the IF AGC moves in steps of its own, and an RTL's tables vary by tuner. The ratio
 *    of the power just before to just after the step is the truth for THIS radio at THIS frequency, and
 *    every signal in the capture moves by the same factor when the front end does.
 * ★ The moment comes from the driver: noteStepAtStart() when the source flags the packet the change
 *   took effect in (SDRplay's grChanged), noteStepSoon() when all we know is that WE just wrote a gain
 *   and the radio applies it some time later (an RTL's USB queue). Either way the exact sample is found
 *   by looking — the flag only says where to look, so a flag a packet early or late still lands on it.
 *
 * ★★★ IT LOOKS AHEAD, SO IT ADDS 4 ms OF LATENCY — ALWAYS, AND THAT IS THE PRICE OF GETTING IT RIGHT.
 *     The step is found as the piece edge where 1.5 ms after differs most from 1.5 ms before, AND the
 *     level jumps right there rather than sliding there (programme slides; a front end jumps), then
 *     measured over 4 ms each side. All of that has to be known before the step's first sample leaves,
 *     so the stream goes through a 4 ms delay line. A version without it guessed from what had arrived
 *     and was fooled by AM modulation into "finding" a 1.7 dB step before the real one (measured).
 *     4 ms is nothing beside the network buffering every listener already has.
 * ★★ TRADE-OFFS, STATED: for the glide the IQ level is not the antenna's — the waterfall and an S-meter
 *    computed from the IQ slide to the new level over ~a second instead of jumping. A genuine level
 *    change landing inside the short window after one of our own writes would be smoothed too; it is
 *    glided away, never kept. Steps under ~2 dB (flagged) or ~4 dB (searched for) are left alone, and a
 *    PHASE jump at the step cannot be undone this way — on the bench a 180° jump still ticks at −18 dB.
 * ★ COST: idle, the delay line's copies and nothing else. Only while a hint is open is the buffer
 *   scanned, and only while a correction is gliding is it multiplied — about a second after each step.
 *
 * ★★★ AND THE RUNG THAT DOES NOT STAY (11.0.9). On the real RSP (Lenovo, 2026-10-20) the steps that stay
 *     were caught, but the hardware IF AGC answers some rungs within MILLISECONDS (7→8, 6→7 with the IF
 *     AGC off its rail): the level drops and comes back — a transient, not a step — so the "a step
 *     stays" test above rightly refuses it, and it went through untouched: still a pop on AM, still a
 *     click and CEQ engaging on a marginal FM station. Measured in test-gain-step (case 9).
 *     ★★ So on a FLAGGED packet (the radio said "the gain changed here"):
 *        · the finder, in that window, bounds the level before and after an edge by the NEXT edge (the IF
 *          AGC's answer is a second edge a couple of ms on) and keeps looking past a rung it has kept —
 *          so a rung and its answer are both found and both put back, to the sample;
 *        · and the moment a sharp edge is found there — kept or thrown out, it does not matter — the level
 *          is HELD: from that edge, in 16-sample slices, any further JUMP in the level (> 3 dB from one
 *          slice to the next, or 5× what the noise does there) is cancelled by an equal and opposite one,
 *          and nothing else is touched — programme slides, a front end jumps. After 4 ms with no new edge
 *          (12 ms at most) whatever is left off the level before becomes the glide, and the hold fades out
 *          over 0.5 ms. So the level is continuous whether the rung stayed, came back, or half came back.
 *     ★ Only on a flag, and only from a sharp edge inside its window: a flag with no level event leaves
 *       the stream bit for bit alone (case 7).
 *     ★ TRADE-OFF: a dip shorter than ~1 ms is not found (the finder's windows are 1.5 ms) and goes through
 *       as before. A level more than 6 dB off inside the hold that did not get there by a jump is pulled back
 *       smoothly too (the backstop) — on a deeply modulated AM carrier that can touch the troughs for the
 *       few ms of a hold.
 *     ★ OFF FOR DAB (setTransientHold(false)): a DAB null symbol is a sharp 1.3 ms dip that comes back,
 *       and holding the level across one would fill the null the receiver syncs on.
 */
class GainStepSmoother {
public:
    using cf32 = std::complex<float>;
    /** @param glideSec time constant of the correction's return to unity, in dB. 0.4 s sits at the
     *   audio AGC's 400 ms release, so an AM listener's AGC can follow the glide down. */
    void configure(double rate, double glideSec = 0.4) {
        rate_ = rate > 0.0 ? rate : 0.0; glideSec_ = glideSec > 0.0 ? glideSec : 0.4;
        // ★ W = 1.5 ms each side of a candidate: past a cycle of most programme, so modulation on a
        //   strong AM carrier cannot pass for a step; D = 4 ms ≥ 2W so the whole after-window AND the
        //   climb to the contrast peak are in hand before the step's first sample has to leave.
        win_   = std::max(kSeg, (int)(0.0015 * rate_) / kSeg * kSeg);
        est_   = std::max(2 * kSeg, (int)(0.004 * rate_) / kSeg * kSeg);
        delay_ = std::max(2 * win_ + 2 * kSeg, est_ + 4 * kSeg);
        hist_  = est_;
        reset();
    }
    /** Clears the correction and any open hint. The delay line is zeroed, which costs one 4 ms
     *  dropout — so call it on a stream restart, never mid-stream. */
    void reset() {
        fifo_.assign((size_t)(hist_ + delay_), cf32(0.0f, 0.0f)); start_ = 0;
        gIn_ = 0; cDb_ = 0.0; compAtStep_ = 0.0; sinceStep_ = 0;
        hintFrom_ = hintTo_ = -1; exact_ = false; nextEdge_ = 0; events_.clear(); found_ = -1;
        holdOn_ = false; holdFrom_ = -1; fadeLeft_ = 0; gHoldDb_ = 0.0; lastCandAt_ = -1; recent_.clear();
    }
    /** The source says a gain change took effect in the NEXT buffer handed to process()
     *  (SDRplay: sdrplay_api_StreamCbParamsT::grChanged). Looked for from 1 ms before it to 2 ms after. */
    void noteStepAtStart() { pendingExact_ = true; }
    /** WE just wrote a gain; the radio applies it some time within `withinSec` (USB in flight, I2C). */
    void noteStepSoon(double withinSec = 0.3) { pendingSoon_ = withinSec; }
    /** ★ The flagged-transient hold (see the note above). On by default; the shim turns it off for DAB. */
    void setTransientHold(bool on) { holdEnabled_ = on; if (!on) { holdOn_ = false; holdFrom_ = -1; fadeLeft_ = 0; } }
    /** True while a flagged transient is being held, or its hold is fading out — the guard reads this too. */
    bool holding() const { return holdOn_ || fadeLeft_ > 0; }
    /** Flagged transients held since configure(). Diagnostics and tests. */
    unsigned holds() const { return holds_; }
    /** Samples of latency this adds — constant, whatever happens. */
    int delaySamples() const { return delay_; }
    /** True while the correction still exceeds `db` — the downstream estimators' guard reads this. */
    bool correcting(double db = 1.0) const { return std::fabs(cDb_) > db; }
    unsigned steps() const { return steps_; }
    /** Candidates found but thrown out because the level came back (see finalise). Diagnostics. */
    unsigned rejected() const { return rejected_; }
    float lastStepDb() const { return (float)lastStepDb_; }
    /** Input sample index (since configure) the last step was placed at. Diagnostics and tests. */
    long long lastStepAt() const { return lastStepAt_; }
    float correctionDb() const { return (float)cDb_; }

    /** In place, delayed by delaySamples(): what comes out is what went in 4 ms earlier, with any of
     *  our own gain steps in it put back to the level before them and glided away. */
    void process(cf32* z, int n) {
        if (n <= 0 || rate_ <= 0.0 || fifo_.empty()) return;
        // ── the delay line: [history H][pending D][new n], contiguous from start_ ──
        // ★ A FIFO that is compacted only once its dead prefix is as long as what it holds, so each
        //   sample is moved O(1) times however small the buffers: an RSP hands over ~0.5 ms packets, and
        //   copying the whole 8 ms line per packet would have been 16× the stream in memcpy.
        const int H = hist_, D = delay_;
        if (start_ >= (size_t)(H + D)) {
            std::copy(fifo_.begin() + start_, fifo_.begin() + start_ + H + D, fifo_.begin());
            fifo_.resize((size_t)(H + D)); start_ = 0;
        }
        fifo_.insert(fifo_.end(), z, z + n);
        wk_ = fifo_.data() + start_;
        const long long g0 = gIn_ - D - H;       // global index of wk_[0]
        const long long out0 = gIn_ - D;         // first sample leaving now
        if (pendingExact_) {
            pendingExact_ = false;
            openHint(gIn_ - (long long)(0.001 * rate_), gIn_ + n + (long long)(0.002 * rate_), true);
        }
        if (pendingSoon_ > 0.0) {
            openHint(gIn_, gIn_ + n + (long long)(pendingSoon_ * rate_), false);
            pendingSoon_ = 0.0;
        }
        gIn_ += n;
        if (hintTo_ >= 0 || found_ >= 0) analyse(g0, H + D + n, out0, n);
        // ── apply the correction to what leaves ──
        /* ★★ OUT OF THE LINE, THEN CORRECTED — never corrected in the line (11.0.9). The line is also the HISTORY the
         *    next calls measure "the level before" from; corrected in place, a step found while an earlier one was
         *    still gliding was measured against the corrected level on one side and the raw on the other (a +10 dB
         *    rung read +0.8 and was thrown out). The finder is meant to see the raw capture on both sides. */
        std::copy(wk_ + H, wk_ + H + n, z);
        cf32* o = z;                              // = global out0
        int i = 0;
        while (i < n) {
            const long long gi = out0 + i;
            if (!events_.empty() && events_.front().at <= gi) {
                compAtStep_ = cDb_ + events_.front().db; cDb_ = compAtStep_; sinceStep_ = 0;
                events_.erase(events_.begin());
            }
            int m = std::min(kSeg, n - i);
            if (!events_.empty() && events_.front().at < gi + m) m = (int)(events_.front().at - gi);
            if (holdFrom_ > gi && holdFrom_ < gi + m) m = (int)(holdFrom_ - gi);   // the hold starts ON its edge
            if (m <= 0) m = 1;
            if (cDb_ != 0.0) {
                const float g = (float)std::pow(10.0, cDb_ / 20.0);
                float* f = reinterpret_cast<float*>(o + i);
                for (int k = 0; k < 2 * m; ++k) f[k] *= g;
                sinceStep_ += m;
                double t = compAtStep_ * std::exp(-(double)sinceStep_ / (glideSec_ * rate_));
                if (std::fabs(t) < 0.01 || !std::isfinite(t)) t = 0.0;
                cDb_ = t;
            }
            if ((holdOn_ && holdFrom_ >= 0 && gi >= holdFrom_) || fadeLeft_ > 0) holdPiece(o + i, m, gi);
            i += m;
        }
        start_ += (size_t)n;
    }

private:
    static constexpr int kSeg = 64;
    static constexpr double kHoldDeadDb = 6.0;   // programme inside this of the level before is left alone
    static constexpr int kSlice = 16;           // the hold's resolution: 16 µs at 1 MS/s
    static constexpr double kHoldMinSec = 0.004;  // held at least this long after an edge: the IF AGC's answer
    static constexpr double kHoldResidSec = 0.002; // what a hold leaves to the glide is judged over this
    static constexpr double kHoldMaxSec = 0.012, kHoldFadeSec = 0.0005;
    struct Event { long long at; double db; };

    /** ★ A sharp edge was found in a FLAGGED window: hold the level that was there before it. A second
     *  edge inside a running hold (the IF AGC answering) only keeps it open — the level to hold is still
     *  the one before the FIRST. */
    void startHold(long long edge, long long g0) {
        if (!holdEnabled_) return;
        if (holdOn_) { holdHomeBy_ = std::max(holdHomeBy_, edge + (long long)(kHoldMinSec * rate_)); return; }
        const long long b0 = std::max(g0, edge - (long long)est_);
        const int nb = (int)(edge - b0);
        if (nb < kSeg) return;
        const double B = meanPow(wk_ + (b0 - g0), nb);
        if (!(B > 0.0)) return;
        // ★ In the CORRECTED level, which is what leaves: the raw level times the correction now in force.
        holdRef_ = B * std::pow(10.0, cDb_ / 10.0);
        // ★ How much a 16-sample slice wanders by itself here (noise on a weak station): a jump the hold acts on
        //   must stand well clear of it, or the hold would chase noise (measured: a marginal FM station's slices
        //   wander ~1.5 dB, and a 3 dB bar made CEQ engage for 2.7 s).
        {
            double s1 = 0.0, s2 = 0.0; int ns = 0, prevOk = 0; double prev = 0.0;
            for (int k = 0; k + kSlice <= nb; k += kSlice) {
                const double p = meanPow(wk_ + (b0 - g0) + k, kSlice);
                const double l = 10.0 * std::log10(std::max(p, 1e-30));
                if (prevOk) { const double dd = l - prev; s1 += dd; s2 += dd * dd; ++ns; }
                prev = l; prevOk = 1;
            }
            const double sd = ns > 1 ? std::sqrt(std::max(0.0, s2 / ns - (s1 / ns) * (s1 / ns))) : 0.0;
            holdJumpDb_ = std::max(3.0, 5.0 * sd);
        }
        holdOn_ = true; holdFrom_ = edge; fadeLeft_ = 0; lastD_ = 0.0; holdCorr_ = 0.0; contSign_ = 0;
        holdHomeBy_ = edge + (long long)(kHoldMinSec * rate_);
        holdUntil_ = edge + (long long)(kHoldMaxSec * rate_);
        recent_.clear(); gHoldDb_ = 0.0;
        ++holds_;
    }
    /** The first SHARP level edge in [a0, a0+len) of the line, as an offset from a0 (-1 = none): a 16-sample slice
     *  > 2.5 dB from the one before, with level ground either side of the jump (the slices before it agree to
     *  1 dB, and so do the two after it). Programme slides — a 1 kHz tone at 50 % on an AM carrier moves < 1 dB
     *  a slice — and a front end jumps; that is the whole test. */
    int nextEdgeIn(long long a0, int len) const {
        const int ns = len / kSlice;
        if (ns < 5) return -1;
        std::vector<double> L((size_t)ns);
        for (int k = 0; k < ns; ++k) L[(size_t)k] = 10.0 * std::log10(std::max(meanPow(wk_ + a0 + k * kSlice, kSlice), 1e-30));
        // ★ The jump may sit inside a slice (its level then half-way), so slice k may be skipped: ground at
        //   k-2,k-1, a jump to k+1 (or to k), ground from there.
        for (int k = 2; k + 2 < ns; ++k) {
            const double before = L[(size_t)k - 1];
            if (std::fabs(before - L[(size_t)k - 2]) >= 1.0) continue;
            if (std::fabs(L[(size_t)k] - before) > 2.5 && std::fabs(L[(size_t)k + 1] - L[(size_t)k]) < 1.0) return k * kSlice;
            if (std::fabs(L[(size_t)k + 1] - before) > 2.5 && std::fabs(L[(size_t)k + 2] - L[(size_t)k + 1]) < 1.0) return k * kSlice;
        }
        return -1;
    }
    /** One piece leaving, already step-corrected, in 16-sample slices (holdSlice).
     *  ★ A slice whose level JUMPS takes its new gain in one go — that jump IS an edge (the rung, the IF AGC's
     *    answer), and cancelling an edge needs an equal edge. Otherwise the gain is ramped across the slice so
     *    the hold adds no edge of its own. On closing, what is left becomes the glide and the hold fades out. */
    void holdPiece(cf32* z, int m, long long gi) {
        // ★ This piece was scaled by the glide BEFORE it got here, so a hold that closes part-way through hands its
        //   residue to the glide only from the next piece: until then the residue rides on the hold's own gain
        //   (pieceExtra_), and the gain is moved into the glide's frame at the end.
        pieceExtra_ = 0.0;
        for (int i = 0; i < m; i += kSlice) holdSlice(z + i, std::min(kSlice, m - i), gi + i);
        gHoldDb_ -= pieceExtra_;
        if (!holdOn_ && fadeLeft_ == 0) gHoldDb_ = 0.0;
        pieceExtra_ = 0.0;
    }
    void holdSlice(cf32* z, int m, long long gi) {
        double target; bool jump = false;
        if (holdOn_) {
            const double P = meanPow(z, m);
            const double d = P > 0.0 ? 10.0 * std::log10(P / holdRef_) : 0.0;
            /* ★★ AN EDGE INSIDE THE HOLD IS CANCELLED BY AN EQUAL EDGE, AND NOTHING ELSE IS TOUCHED. A front end
             *    jumps between two slices (or across one, half-way); programme slides — a 1 kHz tone at 50 % on an
             *    AM carrier moves < 1 dB a slice. So the hold's own correction changes only at a jump (> 3 dB) and
             *    is constant between: programme passes untouched, the IF AGC's answer is undone to the slice. */
            const double j1 = d - lastD_;
            if (std::fabs(j1) > holdJumpDb_) { holdCorr_ -= j1; jump = true; contSign_ = j1 > 0 ? 1 : -1; }
            else if (contSign_ != 0 && (j1 > 0 ? 1 : -1) == contSign_ && std::fabs(j1) > 0.3) {
                holdCorr_ -= j1; jump = true; contSign_ = 0;        // the rest of a jump that fell inside a slice
            } else contSign_ = 0;
            if (jump) holdHomeBy_ = std::max(holdHomeBy_, gi + (long long)(kHoldMinSec * rate_));
            lastD_ = d;
            // ★ …and anything still far from home after that (beyond the dead band — no programme gets there) is
            //   put back too, smoothly: the backstop for a level that drifted rather than jumped.
            const double r = d + holdCorr_;
            const double w = std::max(0.0, std::min(1.0, (std::fabs(r) - kHoldDeadDb) / 3.0));
            target = std::max(-30.0, std::min(30.0, holdCorr_ - r * w));
            recent_.push_back({gi, P * m, m});
            while (!recent_.empty() && recent_.front().at < gi - (long long)(kHoldResidSec * rate_)) recent_.pop_front();
            const long long end = gi + m;
            if (end >= holdHomeBy_ || end >= holdUntil_) {
                // ★ Closing. Whatever the level is off home by now — a rung that stayed (or half did), or what a
                //   mis-sized correction of one left — becomes the glide, as for a found step. Judged over the
                //   last 2 ms (two cycles of a 1 kHz tone: programme cannot pass for an offset), and only past
                //   1 dB, because a wrong residue on an AM carrier is itself a thump.
                double e = 0.0; long long cnt = 0;
                for (const auto& sl : recent_) { e += sl.energy; cnt += sl.n; }
                const double devR = (cnt > 0 && e > 0.0) ? 10.0 * std::log10(e / cnt / holdRef_) : 0.0;
                double resid = 0.0;
                if (std::fabs(devR) >= 1.0) {
                    resid = std::max(-30.0, std::min(30.0, -devR));
                    compAtStep_ = cDb_ + resid; cDb_ = compAtStep_; sinceStep_ = 0;
                }
                holdOn_ = false; holdFrom_ = -1; recent_.clear();
                fadeFromDb_ = target - resid;              // what is left of the hold, on top of the glide
                fadeLeft_ = fadeLen_ = std::max(1, (int)(kHoldFadeSec * rate_));
                pieceExtra_ = resid;
            }
        } else {
            target = fadeFromDb_ * (double)std::max(0, fadeLeft_ - m) / fadeLen_ + pieceExtra_;
            fadeLeft_ = std::max(0, fadeLeft_ - m);
        }
        if (gHoldDb_ == 0.0 && target == 0.0) return;      // bit for bit when there is nothing to hold
        const double g1 = std::pow(10.0, target / 20.0);
        if (jump) { for (int k = 0; k < m; ++k) z[k] *= (float)g1; }
        else {
            const double g0 = std::pow(10.0, gHoldDb_ / 20.0);
            for (int k = 0; k < m; ++k) z[k] *= (float)(g0 + (g1 - g0) * (k + 1) / m);
        }
        gHoldDb_ = target;
    }

    static double meanPow(const cf32* z, int n) {
        double s = 0.0;
        for (int k = 0; k < n; ++k) s += (double)z[k].real() * z[k].real() + (double)z[k].imag() * z[k].imag();
        return n > 0 ? s / n : 0.0;
    }
    /** ★ A flag from the source REPLACES a looser "we wrote a gain" window (it says exactly where, so the
     *  long low-confidence search has nothing left to do); a write never widens a flagged window — on an
     *  RSP both arrive for the same rung, the write first. */
    void openHint(long long from, long long to, bool exact) {
        // ★ A second flag while a flagged window is still being searched EXTENDS it (11.0.9): the IF AGC's answer
        //   is flagged too, a couple of ms after the rung, and replacing the window moved the search past the
        //   rung itself before it had been looked at — neither edge was then found.
        if (exact && exact_ && hintTo_ >= 0) { hintTo_ = std::max(hintTo_, to); hintFrom_ = std::min(hintFrom_, from); }
        else if (exact) { hintFrom_ = from; hintTo_ = to; exact_ = true; }
        else if (hintTo_ < 0) { hintFrom_ = from; hintTo_ = to; exact_ = false; }
        else if (!exact_) hintTo_ = std::max(hintTo_, to);
        else return;
        nextEdge_ = std::max(nextEdge_, (hintFrom_ / kSeg) * kSeg);
    }
    /** Find the step: the piece edge where the level after (W) differs most from the level before (W).
     *  ★ RAW powers on both sides, so the ratio is the front end's step and nothing of ours. Every
     *    edge is decided while its sample is still in the delay line. */
    void analyse(long long g0, int len, long long out0, int nOut) {
        if (found_ >= 0) { finalise(g0, len, out0, nOut); return; }
        const int w = win_ / kSeg;
        const long long gEnd = g0 + len;
        // decide every edge whose after-window is complete, but not one already gone out
        if (nextEdge_ < out0) nextEdge_ = ((out0 + kSeg - 1) / kSeg) * kSeg;
        // ★ Exact (the source flagged it): 2 dB — a big IF AGC step is worth smoothing too. Our own
        //   write with an unknown latency is looked for over ~0.3 s, so the bar is 4 dB: a false find
        //   costs a small level slide that glides away, but it should still be rare.
        const double thr = exact_ ? 2.0 : 4.0;
        long long best = -1; double bestC = 0.0, bestDb = 0.0;
        int sinceBest = 0; bool decided = false;
        /* ★★ A WINDOW PAST THE PEAK, NOT THE FIRST EDGE THAT FAILS TO BEAT IT. On a noisy station the
         *    first edge over the bar can be noise just ahead of the real step — whose own edge is not
         *    "sharp" on the way in — and stopping at the first non-improvement placed a +3 dB IF step
         *    672 samples early: 0.7 ms at the wrong level, a click at −20 dB (measured, 2026-10-19). */
        // Candidate piece k starts at e; the step is somewhere inside it. B = the W before it, A = the W
        // after it — the straddling piece itself is in neither, so its position cannot bias the ratio.
        for (long long e = nextEdge_; e + 3 * kSeg + (long long)w * kSeg <= gEnd && e <= hintTo_; e += kSeg) {
            if (e - (long long)(w + 2) * kSeg < g0) { nextEdge_ = e + kSeg; continue; }
            const int ie = (int)(e - g0);
            const double B = meanPow(wk_ + ie - w * kSeg, w * kSeg);
            const double A = meanPow(wk_ + ie + kSeg, w * kSeg);
            if (!(B > 0.0) || !(A > 0.0)) { nextEdge_ = e + kSeg; continue; }
            const double c = 10.0 * std::log10(A / B);
            /* ★★ AND IT MUST BE SHARP. The windows alone were fooled by programme: a 1 kHz tone at 50 %
             *    on an AM carrier read 1.7 dB between two 1.5 ms windows, and fired before the real step
             *    (measured, 2026-10-19). A front-end step is instantaneous, so the pieces either side of
             *    the straddling one already differ by most of it; modulation of that size takes a
             *    sizeable fraction of an audio cycle to get there. */
            const double pPrev = meanPow(wk_ + ie - kSeg, kSeg);
            const double pNext = meanPow(wk_ + ie + kSeg, kSeg);
            const double cAdj = (pPrev > 0.0 && pNext > 0.0) ? 10.0 * std::log10(pNext / pPrev) : 0.0;
            // ★ …and CONCENTRATED at this piece: the level moves here and hardly at all from one piece to
            //   the next on either side. Modulation spreads the same change evenly over its slope, so a
            //   tone's steepest point fails this however the windows fall.
            const double pPrev2 = meanPow(wk_ + ie - 2 * kSeg, kSeg);
            const double pNext2 = meanPow(wk_ + ie + 2 * kSeg, kSeg);
            const double sB = (pPrev2 > 0.0 && pPrev > 0.0) ? 10.0 * std::log10(pPrev / pPrev2) : 99.0;
            const double sA = (pNext2 > 0.0 && pNext > 0.0) ? 10.0 * std::log10(pNext2 / pNext) : 99.0;
            // the jump across the piece, less the programme's own slope either side of it
            const double jump = cAdj - 0.5 * (sB + sA) * 2.0;
            const bool sharp = (jump > 0) == (c > 0) && std::fabs(jump) >= 0.7 * std::fabs(c)
                            && std::fabs(jump) > 2.5 * std::max(std::fabs(sB), std::fabs(sA));
            if (best < 0) {
                if (std::fabs(c) > thr && sharp) { best = e; bestC = std::fabs(c); bestDb = c; sinceBest = 0; }
                else nextEdge_ = e + kSeg;
            } else if (std::fabs(c) > bestC && (c > 0) == (bestDb > 0) && sharp) {
                best = e; bestC = std::fabs(c); bestDb = c; sinceBest = 0;
            } else if (++sinceBest > w) {
                decided = true;                            // a whole window past the peak: it was the peak
                break;
            }
        }
        if (best >= 0) {
            // Not yet a window past it? Decide next time — unless the edge is about to leave.
            if (decided || best < out0 + nOut + (long long)(w + 4) * kSeg) {
                // ★ Over 30 dB is a signal arriving or the stream starting, not a rung of ours.
                if (bestC <= 30.0) { found_ = best + locate(best - g0, bestDb, g0); foundDb_ = bestDb;
                                     savedTo_ = hintTo_; savedExact_ = exact_;
                                     if (exact_) startHold(found_, g0); }
                hintFrom_ = hintTo_ = -1; nextEdge_ = best + kSeg;
                finalise(g0, len, out0, nOut);
                return;
            }
        }
        if (hintTo_ >= 0 && nextEdge_ > hintTo_) { hintFrom_ = hintTo_ = -1; }
    }

    /** ★ HOW BIG, measured over 4 ms each side once they are in hand (the 1.5 ms windows only FIND it:
     *  a tone on an AM carrier left a 15 dB step mis-measured by 1.6 dB over those). Decided no later
     *  than the step's own sample leaving the delay line. */
    void finalise(long long g0, int len, long long out0, int nOut) {
        const long long gEnd = g0 + len;
        const long long avail = gEnd - found_;
        if (avail < est_ && found_ >= out0 + nOut + kSeg) return;          // more to come, and time to wait
        long long b0 = std::max(g0, found_ - (long long)est_);
        // ★ …and, in a flagged window, the level BEFORE starts at the last edge (the rung, when this is the IF
        //   AGC's answer to it) — the same reason, the other side.
        if (savedExact_ && holdEnabled_ && lastCandAt_ >= 0 && lastCandAt_ + 8 > b0 && lastCandAt_ + 8 + kSeg <= found_) b0 = lastCandAt_ + 8;
        const int nb = (int)(found_ - b0);
        const int skip = 8;                                                // the step's own few samples
        int na = (int)std::min<long long>(est_, avail) - skip;
        /* ★★ IN A FLAGGED WINDOW, THE LEVEL AFTER ENDS AT THE NEXT EDGE (11.0.9). The IF AGC answering a rung a
         *    couple of ms later is a second edge INSIDE the 4 ms this measures over, and averaging across it
         *    sized a −6 dB rung at −4.3 (the hold then had to carry the rest). So the after-window stops where
         *    the level jumps again (nextEdgeIn), if it has had at least 0.25 ms. Only on a flag: a searched-for
         *    step keeps its 4 ms. */
        if (savedExact_ && holdEnabled_ && na > 0) {
            const int cut = nextEdgeIn(found_ - g0 + skip, na);
            if (cut >= 256) na = cut;
        }
        double db = foundDb_;
        if (nb >= kSeg && na >= kSeg) {
            const double B = meanPow(wk_ + (b0 - g0), nb);
            const double A = meanPow(wk_ + (found_ - g0) + skip, na);
            if (B > 0.0 && A > 0.0) {
                const double m = 10.0 * std::log10(A / B);
                /* ★★ A STEP STAYS; A DIP COMES BACK. The 4 ms figure must agree with the 1.5 ms one that
                 *    found it, or this was a transient level event, not a gain change — a DAB null symbol
                 *    is exactly that: a sharp ~1.3 ms drop every 96 ms that reads −10 dB over 1.5 ms and
                 *    −2 dB over 4. Correcting it would lift the null and the frame after it by 10 dB. */
                // ★ ±max(2 dB, 40 %) — programme on an AM carrier moves a 1.5 ms figure by ~2 dB — and at least 1 dB left over: a small "step" that halves on a longer
                //   look is noise inside the null itself (measured: −0.8 dB taken from a 1.3 ms null).
                if ((m > 0) != (foundDb_ > 0) || std::fabs(m) < 1.0
                    || std::fabs(m - foundDb_) > std::max(2.0, 0.4 * std::fabs(foundDb_))) {
                    // …and the search goes on: the real rung may still be ahead in the same window.
                    ++rejected_; lastCandAt_ = found_;
                    // ★ …merged with any window a later flag opened meanwhile (11.0.9), never replacing it
                    if (savedTo_ > found_) { exact_ = savedExact_ || (hintTo_ >= 0 && exact_);
                                             hintTo_ = std::max(hintTo_, savedTo_); hintFrom_ = found_ + kSeg;
                                             nextEdge_ = ((found_ + kSeg) / kSeg + 1) * kSeg; }
                    found_ = -1; return;
                }
                db = m;
            }
        }
        events_.push_back({found_, -db});
        lastStepDb_ = db; lastStepAt_ = found_; lastCandAt_ = found_; ++steps_;
        // ★ In a flagged window the search goes on past a kept rung too: the IF AGC's answer to it may follow.
        if (savedExact_ && holdEnabled_ && savedTo_ > found_ + kSeg) {
            hintTo_ = std::max(hintTo_, savedTo_); hintFrom_ = found_ + kSeg; exact_ = true;
            nextEdge_ = ((found_ + kSeg) / kSeg + 1) * kSeg;
        }
        found_ = -1;
    }

    /** Where inside the straddling piece the step is, to 8 samples: the split that best fits "old level
     *  before, new level after". Off by a piece, the samples in between would carry the wrong
     *  correction for 32 µs — a click of its own, smaller, but a click. */
    int locate(long long ie, double stepDb, long long) const {
        const int w = win_ / kSeg;
        const double B = meanPow(wk_ + ie - w * kSeg, w * kSeg);
        const double lb = std::log10(std::max(B, 1e-30)), la = lb + stepDb / 10.0;
        double lp[kSeg / 8];
        for (int j = 0; j < kSeg / 8; ++j)
            lp[j] = std::log10(std::max(meanPow(wk_ + ie + 8 * j, 8), 1e-30));
        int bestS = 0; double bestE = 1e300;
        for (int sp = 0; sp <= kSeg / 8; ++sp) {
            double err = 0.0;
            for (int j = 0; j < kSeg / 8; ++j) { const double d = lp[j] - (j < sp ? lb : la); err += d * d; }
            if (err < bestE) { bestE = err; bestS = sp; }
        }
        return 8 * bestS;
    }

    double rate_ = 0.0, glideSec_ = 0.4;
    int win_ = kSeg, delay_ = 2 * kSeg, hist_ = kSeg;
    std::vector<cf32> fifo_; size_t start_ = 0;
    cf32* wk_ = nullptr;                      // this call's view: fifo_ from start_, global index g0
    long long gIn_ = 0;                       // global index of the next sample to ARRIVE
    double cDb_ = 0.0, compAtStep_ = 0.0; long long sinceStep_ = 0;
    long long hintFrom_ = -1, hintTo_ = -1, nextEdge_ = 0; bool exact_ = false;
    long long found_ = -1; double foundDb_ = 0.0;     // located, size not yet measured over est_
    long long savedTo_ = -1; bool savedExact_ = false;  // the window it was found in, for a rejection
    int est_ = kSeg;
    bool pendingExact_ = false; double pendingSoon_ = 0.0;
    std::vector<Event> events_;
    unsigned steps_ = 0, rejected_ = 0; double lastStepDb_ = 0.0; long long lastStepAt_ = -1;
    // ── the flagged-transient hold ──
    bool holdEnabled_ = true, holdOn_ = false;
    long long holdFrom_ = -1, holdHomeBy_ = 0, holdUntil_ = 0;
    struct Slice { long long at; double energy; int n; };
    std::deque<Slice> recent_;        // the hold's last 2 ms, for what it hands to the glide
    double holdRef_ = 0.0, gHoldDb_ = 0.0, fadeFromDb_ = 0.0, lastD_ = 0.0, holdCorr_ = 0.0, holdJumpDb_ = 3.0, pieceExtra_ = 0.0;
    int fadeLeft_ = 0, fadeLen_ = 1, contSign_ = 0;
    unsigned holds_ = 0;
    long long lastCandAt_ = -1;      // the last edge found (kept or thrown out) — bounds the next one's level before
};

}  // namespace vibedsp
