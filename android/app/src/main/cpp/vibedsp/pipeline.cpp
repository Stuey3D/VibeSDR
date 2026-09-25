// VibeSDR V5 — RxPipeline: IQ -> {spectrum, audio}. Original VibeSDR code.
#include "vibedsp.h"
#if defined(__linux__)
#include <pthread.h>
#endif
#include <cstring>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include "simd_internal.h"   // stereoMatrixBlend / interleave2 (NEON)
#include <cmath>
#include <algorithm>

namespace vibedsp {

/**
 * Move a filter corner toward its target — slowly, and not at all for small changes.
 *
 * ★★★ THE MOVEMENT IS AUDIBLE, WHICH IS THE ONE THING THIS MUST NOT BE. Stuart, listening to a
 *     weak station: "every now and again you hear the NR allowing slightly higher frequencies
 *     through, I'm assuming as the noise drops a little". Exactly that — the corner was tracking
 *     every dip and swell in the signal, and a treble control operating itself is more
 *     objectionable than the hiss it removes, because the ear locks on to CHANGE where it will
 *     happily ignore a steady state.
 *
 * ★★★ SO: A DEADBAND FIRST. Nothing moves for a difference smaller than kSlack. Most of what was
 *     heard is ordinary flutter about a stable average, and the honest response to it is to do
 *     nothing at all rather than to chase it more smoothly. A slower filter still moves; a
 *     deadband genuinely stops.
 *
 * ★★ AND ASYMMETRIC WHEN IT DOES MOVE. Closing down is what removes hiss the listener can already
 *    hear, so it may happen at a reasonable pace. OPENING UP is the audible direction — the top
 *    coming back — and nobody is harmed by it arriving late, so it is four times slower. The same
 *    reasoning as the noise meter's own attack and release, and the opposite way round from a
 *    compressor, deliberately.
 *
 * ★ Per BLOCK, not per sample, so the rates are in blocks: at ~8 ms a block, 0.02 is roughly half
 *   a second to close and 0.005 about two seconds to open.
 */
inline float glideCorner(float now, float want) {
    constexpr float kSlack = 600.0f;    // Hz — below this, hold still
    if (std::fabs(want - now) < kSlack) return now;
    const float k = (want < now) ? 0.02f : 0.005f;
    return now + k * (want - now);
}


void RxPipeline::start(double sampleRate, int fftSize, double fftRate,
                       int outRate, const Callbacks& cb) {
    sampleRate_ = sampleRate;
    fftSize_    = fftSize;
    fftRate_    = fftRate;
    outRate_    = outRate;
    cb_         = cb;

    // Spectrum: window + FFT, one frame every (sampleRate/fftRate) input samples.
    stopSpecThread_();                       // ★ it owns cfft_ while it runs — never replace it underneath
    stopDemodThread_();                      // ★ and this one owns the demod chain rebuildAudio() replaces
    cfft_ = std::make_unique<ComplexFFT>(fftSize_);
    win_.resize(fftSize_);
    nuttallWindow(win_.data(), fftSize_);
    specBuf_.assign(fftSize_ * 2, 0.0f);   // interleaved? no — store cf32 below
    specDb_.assign(fftSize_, 0.0f);
    specStride_ = std::max(1, (int)std::llround(sampleRate_ / fftRate_));
    specFill_   = 0;
    sinceFrame_ = 0;

    dirty_ = true;
    rebuildAudio();
    if (const char* e = std::getenv("VIBE_SPEC_THREAD")) if (e[0] == '1') specThreadWant_ = true;
    if (const char* e = std::getenv("VIBE_DEMOD_THREAD")) if (e[0] == '1') demodThreadWant_ = true;
    if (const char* e = std::getenv("VIBE_DSP_THREADS")) if (e[0] == '1') specThreadWant_ = demodThreadWant_ = true;
    if (specThreadWant_ && cb_.spectrum) startSpecThread_();
    if (demodThreadWant_ && cb_.audio) startDemodThread_();
}

std::function<void(const char*)>& RxPipeline::workerInit() {
    static std::function<void(const char*)> f;
    return f;
}

void RxPipeline::startDemodThread_() {
    if (demodOn_) return;
    demodHead_ = demodCount_ = 0; demodBusy_ = demodStop_ = false;
    demodOn_ = true;
    demodThread_ = std::thread([this] {
        if (workerInit()) workerInit()("vibe-demod");
#if defined(__linux__)
        else pthread_setname_np(pthread_self(), "vibe-demod");
#endif
        std::unique_lock<std::mutex> lk(demodM_);
        for (;;) {
            demodCv_.wait(lk, [this] { return demodCount_ > 0 || demodStop_; });
            if (demodCount_ == 0) return;                   // stopping, nothing left
            // ★ The head slot is never the one the DSP thread writes (that is head+count), so
            //   swapping it out under the lock is safe and costs no copy.
            demodIn_.swap(demodQ_[demodHead_]);
            const int nc = demodQn_[demodHead_];
            demodBusy_ = true;
            lk.unlock();
            demodTail_(demodIn_, nc);
            lk.lock();
            demodBusy_ = false;
            demodHead_ = (demodHead_ + 1) % kDemodQ;
            --demodCount_;
            demodIdleCv_.notify_all();
        }
    });
}

void RxPipeline::enqueueDemod_(const cf32* ch, int nc) {
    std::unique_lock<std::mutex> lk(demodM_);
    if (demodCount_ >= kDemodQ) {
        demodWaits_.fetch_add(1, std::memory_order_relaxed);
        demodIdleCv_.wait(lk, [this] { return demodCount_ < kDemodQ; });
    }
    const int slot = (demodHead_ + demodCount_) % kDemodQ;
    demodQ_[slot].assign(ch, ch + nc);
    demodQn_[slot] = nc;
    ++demodCount_;
    lk.unlock();
    demodCv_.notify_one();
}

void RxPipeline::flushDemod_() {
    if (!demodOn_) return;
    std::unique_lock<std::mutex> lk(demodM_);
    demodIdleCv_.wait(lk, [this] { return demodCount_ == 0 && !demodBusy_; });
}

void RxPipeline::stopDemodThread_() {
    if (!demodOn_) return;
    flushDemod_();
    { std::lock_guard<std::mutex> lk(demodM_); demodStop_ = true; }
    demodCv_.notify_all();
    if (demodThread_.joinable()) demodThread_.join();
    demodOn_ = false;
}

RxPipeline::~RxPipeline() { stopDemodThread_(); stopSpecThread_(); }

void RxPipeline::startSpecThread_() {
    if (specThreadOn_) return;
    for (int k = 0; k < kSpecQ; k++) {
        specWork_[k].assign((size_t)fftSize_, cf32{0.0f, 0.0f});
        specDone_[k].assign((size_t)fftSize_, 0.0f);
        specSlot_[k] = SPEC_FREE;
    }
    specWr_ = specCp_ = specRd_ = 0;
    specWorkN_ = fftSize_;
    specStop_ = false;
    specThreadOn_ = true;
    specThread_ = std::thread([this] {
        if (workerInit()) workerInit()("vibe-spec");
#if defined(__linux__)
        else pthread_setname_np(pthread_self(), "vibe-spec");
#endif
        std::unique_lock<std::mutex> lk(specM_);
        for (;;) {
            specCv_.wait(lk, [this] { return specSlot_[specCp_] == SPEC_PENDING || specStop_; });
            if (specStop_) return;
            const int k = specCp_;
            lk.unlock();
            // ★ Outside the lock: this slot and cfft_ are the worker's alone while it is PENDING.
            //   Nothing else may touch cfft_ while the thread runs — see setSpectrumThread.
            const float scale = 1.0f / (float)((double)specWorkN_ * (double)specWorkN_);
            cfft_->powerDbShifted(specWork_[k].data(), win_.data(), specDone_[k].data(), scale);
            lk.lock();
            specSlot_[k] = SPEC_READY;
            specCp_ = (specCp_ + 1) % kSpecQ;
        }
    });
}

/** Deliver every finished spectrum frame, oldest first, on the calling (DSP) thread.
 *  ★ The cursors only ever advance in order, so frames reach the callback in the order their
 *    windows were captured — a waterfall drawn out of order is worse than a slow one. */
void RxPipeline::drainSpecQueue_() {
    /* ★★★ ONE FRAME PER CALL — A BURST ON THIS THREAD IS AN AUDIO DROP.
     *
     *  This ran `for(;;)` until the queue was empty, and the callback it invokes converts a whole
     *  row to dB and hands it to every listener — ON THE DSP THREAD, which owes the audio a block
     *  every 32 ms. Draining four frames at once therefore spends four times that work inside one
     *  audio block, and on a Pi 2 (900 MHz A7, the demod thread already at three-quarters of a
     *  core) that is enough to miss the deadline. Stuart, 2026-09-25, after this shipped: "the pi2
     *  is breaking up and stuttering ... 2 distinct drops in the space of a few seconds" — on a box
     *  measured at 47 % CPU with its clock at maximum, so not a shortage of CPU but a BURST in the
     *  wrong place.
     *  ★★ Throughput is unaffected: this is called at the top of feed() AND at every emit point, so
     *     several frames still leave per block — they just leave one at a time, with audio work
     *     between them. The single-slot ceiling this replaced is still gone; what is gone now too
     *     is the spike.
     *  ★ The queue keeps its depth. Depth absorbs jitter in when the worker finishes; it was never
     *    meant to be emptied in one breath. */
    {
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(specM_);
            if (specSlot_[specRd_] == SPEC_READY) { specDb_.swap(specDone_[specRd_]); have = true; }
        }
        if (!have) return;
        cb_.spectrum(cb_.ctx, specDb_.data(), fftSize_);
        {
            std::lock_guard<std::mutex> lk(specM_);
            // ★ The swap above left the collected buffer where the slot's result was; it is the
            //   slot's scratch again now, so hand it back sized and free.
            specSlot_[specRd_] = SPEC_FREE;
            specRd_ = (specRd_ + 1) % kSpecQ;
        }
    }
}

void RxPipeline::stopSpecThread_() {
    if (!specThreadOn_) return;
    { std::lock_guard<std::mutex> lk(specM_); specStop_ = true; }
    specCv_.notify_all();
    if (specThread_.joinable()) specThread_.join();
    specThreadOn_ = false;
    for (int k = 0; k < kSpecQ; k++) specSlot_[k] = SPEC_FREE;
    specWr_ = specCp_ = specRd_ = 0;
}

// ── The AM chain-width ladder ────────────────────────────────────────────────
// ★★★ WHY A BAND AND NOT THE EXACT WIDTH. chFs_ is derived from the width (max(bw*3, 12000)), so
//     letting the chain follow the width exactly means chDecim_ moves on most steps — and a new
//     chDecim_ is a new cascade: redesigned, reallocated and cleared, on the DSP thread. Building
//     for a BAND pins chFs_ across the whole range a listener drags through, so the only thing
//     left to change is the final filter's cutoff, which can be swapped in place.
// ★★ AND THE BENEFIT IS KEPT, WHICH IS THE WHOLE POINT. Pinning one channel rate across all of AM
//    would have been simpler and would have thrown away the reason the rate is derived at all: a
//    narrow AM channel is cheap, and the low rate IS the saving (see the pi-bench figures above).
//    A ladder keeps a narrow signal on a narrow chain — it only stops the rate twitching per step.
// ★ Edges, not centres: the chain is built for the TOP of the band so the filter can always
//   narrow to the user's exact width, never widen past what the chain physically carries.
static bool pickBand(const double* e, int n, double bwHz, double& lo, double& hi) {
    // Outside the ladder there is no band to hold on to, so the caller keeps the old exact
    // behaviour — a rebuild per step. Those are widths nobody drags through.
    if (bwHz < e[0] || bwHz > e[n - 1]) return false;
    for (int i = 1; i < n; ++i)
        if (bwHz <= e[i]) { lo = e[i - 1]; hi = e[i]; return true; }
    return false;
}

// ★ Per mode, because the widths a listener actually drags through are nothing like each other:
//   SSB lives in hundreds of Hz, AM in single-digit kHz, NFM in tens. One shared ladder would
//   put the whole of SSB inside a single band (no selectivity control left) or split AM so
//   finely that every step crossed an edge and rebuilt anyway.
static bool chainBand(RxPipeline::Mode mode, double bwHz, double& lo, double& hi) {
    using M = RxPipeline::Mode;
    // ★★★ THE TOP OF EACH LADDER MUST CLEAR WHAT THE UI CAN ASK FOR, or the widest settings fall
    //     off the end and silently get the old rebuild — which is precisely how the first version
    //     of this shipped: AM stopped at 24 kHz while the client offers 40, so the top of the
    //     range still popped and stuttered on every step (Stuart, on air, 2026-08-25). The client's
    //     BW_EDGE_MAX is per EDGE, so the total width is twice it: SSB 12 k, AM 40 k, NFM 60 k,
    //     WFM 500 k. These ladders clear all four with a band to spare.
    switch (mode) {
        case M::AM:  { static const double e[] = { 1000.0, 6000.0, 12000.0, 24000.0, 48000.0 };
                       return pickBand(e, 5, bwHz, lo, hi); }
        case M::NFM: { static const double e[] = { 4000.0, 8000.0, 16000.0, 32000.0, 64000.0 };
                       return pickBand(e, 5, bwHz, lo, hi); }
        case M::SSB_USB: case M::SSB_LSB: case M::CW:
                     { static const double e[] = { 200.0, 1500.0, 3000.0, 6000.0, 12000.0 };
                       return pickBand(e, 5, bwHz, lo, hi); }
        // ★★★ WFM IS ON THIS PATH TOO, AND EXCLUDING IT WAS A MISREADING OF AdaptiveIf. That
        //     filter serves the AUTOMATIC narrowing (IMS and auto-bandwidth, via ifBwReq_/
        //     autoBwReq_) and never sees the listener's MANUAL width — which went through
        //     setTune, failed sameChain, and rebuilt the chain like every other mode. So dragging
        //     the WFM filter tore down the pilot PLL and RDS on every step, which is the same
        //     "tune away and RDS never comes back" family this file keeps fighting.
        // ★★ Its channel rate is floored at 150 kHz, so every width below ~100 kHz already shares
        //    one chain — the bottom band is deliberately huge because nothing there moves chFs_,
        //    and only the wide end needs banding at all.
        case M::WFM: { static const double e[] = { 1000.0, 100000.0, 200000.0, 400000.0, 800000.0 };
                       return pickBand(e, 5, bwHz, lo, hi); }
    }
    return false;
}

void RxPipeline::setTune(double offsetHz, Mode mode, double bwHz) {
    // ★★★ A RETUNE INSIDE THE SAME MODE AND BANDWIDTH MOVES ONE THING: THE NCO.
    // Everything rebuildAudio() does is a function of mode/bw/sampleRate — filter
    // design, the pilot PLL, the resampler tables, the AGC rate. None of them move
    // when the dial does. Rebuilding anyway cost a full audio BREAK on every step:
    //   - baseBuf_/chBuf_/demodBuf_/audioBuf_ are cleared, so the stream stops for as
    //     long as the chain takes to refill — the gap the jitter buffer has to cover,
    //     and why thinning it to 150 ms produced silence and a re-arm on every tune.
    //   - agc_.reset() puts env_ back to kTarget, i.e. gain EXACTLY 1.0. On a weak HF
    //     signal the converged gain is far higher, so the audio drops hard and crawls
    //     back over the 400 ms release. That is "tuning attenuates the audio" (Stuart,
    //     2026-08-03, Pi demo). FM was never affected because it has no audio AGC.
    // So: same chain -> just re-point the oscillator. The NCO is a recursive rotator,
    // so its phase stays continuous across the change and there is no click.
    //
    // ★ A REQUEST, not a retune: this is called from a socket/control thread and the
    // NCO is owned by the DSP thread. Same discipline as `dirty_` and `resetReq_`.
    const bool sameChain = (mode == mode_ && bwHz == bwHz_);
    // ★★★ A WIDTH CHANGE IS NOT A NEW CHAIN, and treating it as one is the dip. When the chain was
    //     built for a band that still contains the new width, the selectivity filter can simply be
    //     retuned — so this is a request to the DSP thread, exactly like tuneReq_, not a teardown.
    //     The NCO still has to move, so tuneReq_ is set alongside it rather than instead of it.
    // ★ Reads smoothBw_/chainBw*, which the DSP thread owns. Same discipline (and same accepted
    //   race) as the mode_/bwHz_ reads on the line above: the worst case is a needless rebuild or
    //   a refused retune, and applySmoothBandwidth() re-checks the band before it acts.
    const bool sameBand = !sameChain && mode == mode_ && smoothBw_ &&
                          bwHz >= chainBwLo_ && bwHz <= chainBwHi_;
    offsetHz_ = offsetHz;
    mode_     = mode;
    bwHz_     = bwHz;
    if (sameChain)     tuneReq_.store(true, std::memory_order_relaxed);
    else if (sameBand) { bwReq_.store(true, std::memory_order_relaxed);
                         tuneReq_.store(true, std::memory_order_relaxed); }
    else               dirty_ = true;
}

bool RxPipeline::applySmoothBandwidth() {
    // Re-checked on the DSP thread: setTune's test read state this thread owns, and the band may
    // have moved under it between the request and this block.
    if (!smoothBw_ || decs_.empty() || lastFs_ <= 0.0) return false;
    if (bwHz_ < chainBwLo_ || bwHz_ > chainBwHi_)       return false;
    const bool ssbLike = (mode_ == Mode::SSB_USB || mode_ == Mode::SSB_LSB || mode_ == Mode::CW);
    const double chHalf = std::max(1.0, ssbLike ? bwHz_ : bwHz_ * 0.5);
    const double cutoff = std::min(0.45 / lastDecim_, chHalf / lastFs_);
    // Same transition the chain was built with, so the design comes out the same length and the
    // swap is accepted; deepStop matches the last stage — this IS the adjacent-channel rejection.
    if (!decs_.back()->retune(designLowpass(cutoff, lastTrans_, /*deepStop=*/true))) return false;

    // ★★★ THE CHANNEL FILTER IS NOT THE WHOLE WIDTH. Each mode has its own thing derived from
    //     bwHz_, and leaving it stale is worse than the rebuild: SSB's Weaver sub-carrier sits at
    //     bw/2, so a stale one puts the sideband in the wrong place and the image comes back;
    //     NFM's discriminator gain scales the audio, so a stale one is a level error. If either
    //     refuses, say so and let the caller rebuild — the channel filter above has already
    //     moved, and a rebuild is what puts the whole chain back in agreement.
    if (ssbLike) {
        if (!ssb_) return false;
        return ssb_->retune(mode_ == Mode::SSB_LSB ? SsbDemod::Side::LSB : SsbDemod::Side::USB,
                            bwHz_);
    }
    if (mode_ == Mode::NFM) {
        if (!fm_) return false;
        fm_->setGain((float)(chFs_ / (2.0 * M_PI * std::max(1.0, bwHz_ * 0.5))));
    }
    return true;
}

void RxPipeline::rebuildAudio() {
    // Channel decimation: bring the IQ down to a manageable channel rate that
    // comfortably holds the demod bandwidth, then resample to exactly outRate.
    // 1.5x covers the RF channel while keeping the per-sample MPX/PLL/RDS work as
    // cheap as possible. Narrow modes are unaffected (floored by outRate).
    //
    // WFM needs a floor of its own: the MPX runs to 57 kHz (RDS) + sidebands, so
    // the channel must hold ~120 kHz of Nyquist regardless of how narrow the user
    // sets the RF bandwidth — otherwise RDS folds and stereo dies.
    //
    // This used to be bwHz*3, which cost 2-7x more CPU for no benefit. Worst at
    // LOW sample rates: at 1.024 MSPS the target (540 kHz) exceeded fs/2, so
    // floor(fs/target) came out as 1 — no decimation at all, and the entire MPX
    // chain ran at the full 1.024 MSPS. That is why lowering the sample rate made
    // WFM *more* expensive instead of less.
    // The channel rate does NOT have to be the audio rate. It used to be floored at
    // outRate_ (48 kHz) — but a 2.8 kHz SSB signal does not need a 48 kHz channel, and
    // that floor was the single most expensive line in the engine.
    //
    // Why: SsbDemod's Weaver filters use a deliberately SHARP ~80 Hz transition to
    // reject the wrong sideband, and tap count is 3.3/(transition/fs). At 48 kHz that
    // is ~2000 taps — TWICE (I and Q). Filter cost then scales with fs SQUARED: more
    // taps AND more samples through them. Dropping the channel to 12 kHz cuts it ~16x.
    // The demodulated audio (<=3 kHz) is resampled up to 48 kHz afterwards as always,
    // so nothing about the output changes.
    //
    // 12 kHz floor: enough for the widest narrow mode's audio plus filter transition
    // room, and enough for CW's beat note. 3x bandwidth keeps AM/NFM comfortable.
    // ★★★ THE CHAIN IS BUILT FOR A BAND so a width change inside it costs nothing — see
    //     chainBand(). AM, SSB/CW and NFM all take this path; each has its own follow-up work
    //     inside applySmoothBandwidth() (SSB's Weaver pair, NFM's demod gain), and WFM is
    //     excluded because AdaptiveIf already does this job better for it.
    smoothBw_ = chainBand(mode_, bwHz_, chainBwLo_, chainBwHi_);
    if (!smoothBw_) { chainBwLo_ = chainBwHi_ = 0.0; }
    const double chainBw = smoothBw_ ? chainBwHi_ : bwHz_;
    double targetCh = std::max(chainBw * 3.0, 12000.0);
    // WFM is the exception: its MPX runs to 57 kHz (RDS) + sidebands, so it needs a
    // real channel regardless of the RF bandwidth the user picked.
    // ★★★ FM-DX NO LONGER WIDENS THE CHANNEL — the premise was wrong, and measurement killed
    // it. See the chHalf note below: widening recovers subcarrier AMPLITUDE and destroys
    // subcarrier SNR, which is the thing that actually decodes.
    if (mode_ == Mode::WFM) targetCh = std::max(bwHz_ * 1.5, 150000.0);
    // ★ Raw IQ out needs the channel at least as wide as the consumer's rate — a 48 kHz consumer
    //   cannot be fed from the 12 kHz channel a narrow mode would otherwise build.
    { const double f = iqMinRate_.load(std::memory_order_relaxed); if (f > 0.0) targetCh = std::max(targetCh, f); }
    chDecim_ = std::max(1, (int)std::floor(sampleRate_ / targetCh));
    chFs_    = sampleRate_ / chDecim_;

    // ── Channel low-pass: a CASCADE, not one long filter ──────────────────────
    //
    // Tap count goes as 3.3/transition (designLowpass), and transition is normalised
    // to the rate the filter RUNS AT. So the same real-world filter costs ~25x more
    // taps at 2.4 MSPS than at 96 kHz. Decimating by 50 in ONE step therefore forced
    // a ~750-tap filter at the full input rate — 36M complex MACs/sec, which was
    // most of a core on a Pi 3 and a big slice of one on a budget phone.
    //
    // Instead, factor the decimation (50 -> 5x5x2) and give the early stages only
    // the job they actually have: stop anything folding INTO the final channel. That
    // is a hugely relaxed spec, so they cost ~9-17 taps each even though they run at
    // the high rates. The narrow, expensive, selectivity-defining filter then runs
    // LAST, at the lowest rate, where its taps are cheap.
    //
    // Same filter shape out, ~3x less work. Measured (tools/pi-bench, one user):
    //   SSB @ 2.4 MSPS   4.4% -> 1.6% of a core     AM   1.8% -> 0.9%
    // Channel half-width. For AM/FM the signal straddles the carrier, so it is bw/2.
    // For SSB and CW it is NOT: the wanted sideband runs from the carrier out to the
    // FULL bandwidth on one side (that is the premise SsbDemod's Weaver mixer is built
    // on — it down-mixes by bw/2 to centre a sideband that spans 0..bw). Using bw/2
    // here cut the sideband in half: for a 2.8 kHz SSB channel the filter closed at
    // 1.4 kHz and took the consonants with it.
    //
    // This was masked for years: the old single-stage filter had a ~10.6 kHz transition
    // and sloppily leaked 1.4-2.8 kHz back through. Tightening the filter did its job
    // properly and made the missing top half of the voice audible — measured against
    // an UberSDR recording of the same signal, we were 37 dB down over 2.0-2.7 kHz.
    const bool ssbLike = (mode_ == Mode::SSB_USB || mode_ == Mode::SSB_LSB ||
                          mode_ == Mode::CW);
    // ★★★ bw/2, AND THE ATTEMPT TO WIDEN IT IS RECORDED HERE SO IT IS NOT REPEATED.
    //
    // The observation that started it is real: at bw/2 the recovered RDS subcarrier is 1.86 dB
    // down while the pilot is untouched, because the filter clips the outer FM sidebands and
    // that takes the TOP of the MPX. It is why our RDS deviation reads low against a Pira
    // analyser (HansVanEijsden's six stations, 2026-07-27).
    //
    // ★★ THE FIX WAS WRONG BECAUSE THE MEASUREMENT WAS OF THE WRONG QUANTITY. Widening was
    // justified by RDS AMPLITUDE recovered. What decides whether RDS decodes is subcarrier
    // SNR, and FM's noise triangle puts noise power up as the SQUARE of baseband frequency —
    // so a wider IF dumps disproportionately more noise at exactly 57 kHz. Numerator measured,
    // denominator ignored.
    //
    // Measured properly (tools/wfm_mpx_loss.cpp, RDS SNR column):
    //                        2.4 MSPS        768 kHz (Airspy HF+)
    //     bw/2               +32.9 dB        +36.5 dB
    //     0.60 (was FM-DX)   +33.1 dB        +26.5 dB   <-- TEN dB worse
    //
    // ★ It looked harmless on a dongle, which is why simulation passed it. On a narrow-band
    // radio it is a catastrophe, and Stuart found it in one A/B on a strong local: constellation
    // scatter 16% -> 39%, block sync lost entirely, on a station WFM decodes instantly.
    // ★★ SO THE CHANNEL FILTER COSTS US READOUT ACCURACY, NOT DECODE MARGIN. An earlier version
    // of this comment claimed we were "RECEIVING RDS weak and losing decode margin" — that was
    // inferred from the amplitude figure and is not true. Do not re-derive it.
    const double chHalf = std::max(1.0, ssbLike ? bwHz_ : bwHz_ * 0.5);
    // The absolute transition width the old single-stage design worked out to. Keep it
    // identical so the audible filter shape does not change.
    double transHz = std::max(chHalf * 0.5, chFs_ * 0.25 - chHalf);   // re-derived below for the chosen channel rate

    decs_.clear();
    std::vector<int> stages;
    /* ★★★ THE DECIMATION IS CHOSEN BY COST, NOT BY floor(fs / target) (2026-09-16).
     *
     *  Each stage costs roughly (its output rate × its taps), and the last stage — the deep
     *  stopband channel filter, whose length is set by the transition at ITS input rate — is
     *  most of it. floor(fs/target) is blind to that: WFM at 1.024 MS/s got chDecim 3, ONE stage
     *  of 227 taps at the full rate (77 M MACs/s), where decimating by 4 gives [2, 2] with the
     *  113-tap deep filter at 512 kHz (35 M); NFM at 250 kS/s got a prime 5 (688 taps at the
     *  input rate, 34 M) where 8 gives [4, 2] (6.5 M). "Lowering the sample rate made WFM MORE
     *  expensive" — the note above — was this.
     *
     *  So every decimation between fs/(1.5·target) and 1.2·fs/target is planned with the SAME
     *  formulas the build loop uses, costed, and the cheapest taken (ties to the larger, i.e.
     *  the lower channel rate). The channel therefore lands between target/1.2 and 1.5·target —
     *  never below 2.5× the mode's band. Each stage's own spec is unchanged, so the channel
     *  filter out has the same shape; the adjacent-channel and passband tests pin the rejection.
     *
     *  ★★ The plan for a given decimation: the LAST stage is the smallest prime factor (it must
     *     run at the lowest rate and decimate by the least); the anti-alias stages before it are
     *     the other factors, 2s paired into 4s (one 20-tap stage at the input rate beats two
     *     10-tap ones — "4 then 2", measured 2026-09-14), largest first. Prime factors are
     *     otherwise the wrong tool — 64 as six stages of 2 drags the expensive high-rate samples
     *     through stage after stage (measured: NFM 40 % dearer than doing nothing). */
    const auto planFor = [&](int d, std::vector<int>& st, double& cost, double& trans_out) {
        st.clear();
        std::vector<int> primes;
        { int r = d; for (int q = 2; q * q <= r; ++q) while (r % q == 0) { primes.push_back(q); r /= q; } if (r > 1) primes.push_back(r); }
        if (primes.empty()) primes.push_back(1);
        std::sort(primes.begin(), primes.end());
        const int lastStage = primes.front();
        primes.erase(primes.begin());
        int twos = 0;
        for (int q : primes) { if (q == 2) ++twos; else st.push_back(q); }
        for (; twos >= 2; twos -= 2) st.push_back(4);
        if (twos) st.push_back(2);
        std::sort(st.begin(), st.end(), [](int x, int y) { return x > y; });
        st.push_back(lastStage);
        // cost, with the build loop's own spec
        const double chFsC = sampleRate_ / d;
        trans_out = std::max(chHalf * 0.5, chFsC * 0.25 - chHalf);
        cost = 0.0;
        double fsC = sampleRate_;
        for (size_t i = 0; i < st.size(); ++i) {
            const int D = st[i];
            const double fsOut = fsC / D;
            const bool last = (i + 1 == st.size());
            double cutoff, trans;
            if (last) {
                cutoff = std::min(0.45 / D, chHalf / fsC);
                trans  = std::max(cutoff * 0.5, trans_out / fsC);
                if (smoothBw_) { const double chHalfLo = ssbLike ? chainBwLo_ : chainBwLo_ * 0.5; trans = (chHalfLo * 0.5) / fsC; }
            } else {
                cutoff = chHalf / fsC;
                trans  = std::max((fsOut - chHalf) / fsC - cutoff, cutoff * 0.5);
            }
            trans = std::max(trans, 1e-3);
            int n = (int)std::ceil((last ? 5.5 : 3.3) / std::max(trans, 1e-4));
            if ((n & 1) == 0) ++n;
            if (n < 9) n = 9;
            cost += (double)n * fsOut;
            fsC = fsOut;
        }
    };
    {
        const int dFloor = chDecim_;
        const int dLo = std::max(1, (int)std::ceil(sampleRate_ / (1.5 * targetCh)));
        const int dHi = std::max(dFloor, (int)std::floor(1.2 * sampleRate_ / targetCh));
        double bestCost = -1.0, bestTrans = transHz;
        std::vector<int> st;
        for (int d = dLo; d <= dHi; ++d) {
            double c, t;
            planFor(d, st, c, t);
            if (bestCost < 0.0 || c < bestCost * 0.999 || (c <= bestCost * 1.001 && d > chDecim_)) {
                bestCost = c; chDecim_ = d; stages = st; bestTrans = t;
            }
        }
        chFs_   = sampleRate_ / chDecim_;
        transHz = bestTrans;
    }

    double fs = sampleRate_;
    for (size_t i = 0; i < stages.size(); ++i) {
        const int D = stages[i];
        const double fsOut = fs / D;
        const bool last = (i + 1 == stages.size());

        double cutoff, trans;
        if (last) {
            // The real channel filter: defines selectivity. Cheap here because fs is low.
            cutoff = std::min(0.45 / D, chHalf / fs);
            trans  = std::max(cutoff * 0.5, transHz / fs);
            if (smoothBw_) {
                // ★★★ FIX THE TRANSITION ACROSS THE BAND, BECAUSE THE TAP COUNT IS THE CONTRACT.
                //     designLowpass takes its length from the transition alone (3.3 or 5.5 over
                //     it), so a transition that tracks the cutoff gives a different length at
                //     every width — and a different length cannot be swapped into a running
                //     filter. Designing for the NARROWEST width in the band fixes the length and
                //     is the safe direction: every wider width in the band then gets a filter
                //     that is, if anything, sharper than it strictly needed.
                // The band bottom expressed the SAME way chHalf is — full width for SSB/CW,
                // half for AM/FM — or the length would be designed for the wrong filter.
                const double chHalfLo = ssbLike ? chainBwLo_ : chainBwLo_ * 0.5;
                trans = (chHalfLo * 0.5) / fs;
            }
            // Remembered so applySmoothBandwidth() can redesign against exactly this geometry.
            lastFs_ = fs; lastDecim_ = D; lastTrans_ = std::max(trans, 1e-3);
        } else {
            // Anti-alias only: protect the final channel from what folds at fsOut.
            // Everything between chHalf and (fsOut - chHalf) is allowed to be ugly —
            // a later stage will remove it — so the transition is enormous and the
            // filter is tiny.
            cutoff = chHalf / fs;
            trans  = std::max((fsOut - chHalf) / fs - cutoff, cutoff * 0.5);
        }
        // DEEP STOPBAND on the last stage. Whatever it fails to attenuate folds
        // straight into the audio and can never be removed afterwards — and the
        // fold lands at (channel rate +/- passband), which for a 12 kHz channel is
        // only ~10 kHz off tune. On a crowded band that neighbour can be 60 dB
        // louder than the signal you are trying to hear, and Hamming's ~53 dB is
        // not enough. Blackman's ~74 dB is, and the extra taps are nearly free at
        // the slowest rate in the chain. THIS IS THE ADJACENT-CHANNEL REJECTION —
        // see test_demod's alias-rejection case before touching it.
        decs_.push_back(std::make_unique<FirDecimator>(
            designLowpass(cutoff, std::max(trans, 1e-3), /*deepStop=*/last), D));
        fs = fsOut;
    }

    /* ★ VIBE_DSP_PLAN=1 prints the chain the planner built — stage decimations and tap counts,
     *  the channel rate — so a cost can be read off instead of guessed (2026-09-16). */
    if (std::getenv("VIBE_DSP_PLAN")) {
        std::fprintf(stderr, "[vibedsp] fs=%.0f mode=%d bw=%.0f target=%.0f chDecim=%d chFs=%.1f stages:",
                     sampleRate_, (int)mode_, bwHz_, targetCh, chDecim_, chFs_);
        for (size_t i = 0; i < stages.size(); ++i) std::fprintf(stderr, " /%d(%d taps)", stages[i], decs_[i]->taps());
        std::fprintf(stderr, "\n");
    }
    nco_.setFreq(offsetHz_ / sampleRate_);   // tune the channel to baseband

    // Construct the demod for the active mode. FM gain maps radians/sample to a
    // unit-ish audio level at the channel rate.
    am_.reset(); fm_.reset(); ssb_.reset(); audioLpf_.reset(); lmrLpf_.reset();
    // ★ Was the audio AGC already running and converged? If so its envelope is still a
    // good estimate of the signal we are listening to, and wiping it costs a full
    // re-acquisition (gain 1.0, then a 400 ms climb) — audible as a hard attenuation.
    // Only re-acquire when arriving from a mode that had NO audio AGC, where env_ is
    // genuinely stale. A bandwidth change inside AM/SSB is not a new signal.
    // env_ is a level, not a rate, so it stays valid across a configure() to a new chFs_.
    const bool hadAgc = useAgc_;
    useDeemph_ = false; stereo_ = false; useAgc_ = false;
    // Only WFM decimates inside its audio filters; every other mode's post-chain
    // runs at the channel rate, which is already as low as that mode needs.
    audioDecim_ = 1; audFs_ = chFs_;
    useFmDc_ = false;   // FM-only; must not survive a switch to AM/SSB
    switch (mode_) {
        case Mode::AM:                          am_ = std::make_unique<AmDemod>();
                                                useAgc_ = true; agc_.configure(chFs_);
                                                if (!hadAgc) agc_.reset();
                                                agc_.guard(); break;
        case Mode::SSB_USB: case Mode::SSB_LSB:
        case Mode::CW:
            ssb_ = std::make_unique<SsbDemod>();
            ssb_->configure(mode_ == Mode::SSB_LSB ? SsbDemod::Side::LSB : SsbDemod::Side::USB,
                            bwHz_, chFs_, smoothBw_ ? chainBwLo_ : 0.0);
            useAgc_ = true; agc_.configure(chFs_);
            if (!hadAgc) agc_.reset();
            agc_.guard(); break;
        case Mode::NFM:
            fm_ = std::make_unique<FmDemod>((float)(chFs_ / (2.0 * M_PI * std::max(1.0, bwHz_ * 0.5))));
            fmDc_.configure(chFs_); useFmDc_ = true;
            if (deempTau_.load() > 0.0) { deemph_.configure(deempTau_.load(), chFs_); deemph_.reset(); useDeemph_ = true; }
            break;
        case Mode::WFM: {
            // Wideband FM. Discriminator -> MPX. Mono path = 15 kHz L+R LPF +
            // de-emphasis. Stereo path adds a 19 kHz pilot PLL, 38 kHz coherent
            // L-R recovery, a second 15 kHz LPF, and per-channel de-emphasis.
            fm_ = std::make_unique<FmDemod>((float)(chFs_ / (2.0 * M_PI * 75000.0)));
            fmDc_.configure(chFs_); useFmDc_ = true;
            // ── DECIMATE INSIDE THE 15 kHz FILTERS ────────────────────────────
            // These two FIRs were the single most expensive thing in WFM: ~176
            // taps each, computed for every one of the ~320k channel samples a
            // second, to produce audio that is only 15 kHz wide. A FIR that
            // decimates only evaluates the outputs it keeps, so dropping to an
            // ~80 kHz audio rate here costs a quarter of the MACs for exactly the
            // same filter shape — and de-emphasis, the stereo matrix and the
            // resampler all then run at a quarter of the rate too.
            //
            // The catch is folding: after decimation everything above audFs/2
            // aliases in, and the MPX above 15 kHz is FULL of things we must not
            // hear (19 kHz pilot, the 23-53 kHz L-R subcarrier, RDS at 57 kHz).
            // So (a) keep the audio rate high enough that the pilot never folds,
            // and (b) give these filters the DEEP (Blackman, ~74 dB) stopband —
            // whatever leaks through folds into the audio permanently. The
            // pilot-rejection and stereo-separation cases in test_pipeline guard
            // both; check them before changing the 64 kHz floor.
            // Take the LARGEST decimation that keeps the audio rate above 64 kHz —
            // comfortably above twice the 15 kHz audio, and high enough that the
            // 19 kHz pilot can never fold back down into it. Prefer a factor that
            // divides the channel rate exactly: a non-integer audio rate leaves the
            // resampler with a coprime L/M and a polyphase tap table megabytes wide,
            // which costs more in cache misses than the decimation saves. Where no
            // exact factor exists, the largest one still wins — it shrinks M, and
            // therefore that table, by the same factor.
            const int maxDec = std::max(1, (int)std::floor(chFs_ / 64000.0));
            const long long chI = std::llround(chFs_);
            audioDecim_ = maxDec;
            for (int d = maxDec; d >= 2; --d) if (chI % d == 0) { audioDecim_ = d; break; }
            audFs_ = chFs_ / audioDecim_;

            const double tau = deempTau_.load();   // 0=off / 50us EU/UK / 75us US
            useDeemph_ = (tau > 0.0);
            if (useDeemph_) { deemph_.configure(tau, audFs_); deemph_.reset();
                              deemphR_.configure(tau, audFs_); deemphR_.reset(); }
            // ★ Two stages when the decimation is even — see RealFir2. The half-band stage only
            //   has to protect 15 kHz from what folds at chFs/2, so its transition is enormous
            //   and it is 9 taps; the deep 15 kHz design then runs at half the rate.
            auto makeAudioLpf = [&]() {
                if (audioDecim_ >= 2 && (audioDecim_ % 2) == 0) {
                    const double preCut = 15000.0 / chFs_;
                    const double preTrans = std::max((chFs_ * 0.5 - 15000.0) / chFs_ - preCut, preCut * 0.5);
                    const double cut2 = 15000.0 / (chFs_ * 0.5);
                    return std::make_unique<RealFir2>(
                        std::make_unique<RealFir>(designLowpass(preCut, preTrans, /*deepStop=*/false), 2),
                        std::make_unique<RealFir>(designLowpass(cut2, cut2 * 0.4, /*deepStop=*/true), audioDecim_ / 2));
                }
                const double cut = 15000.0 / chFs_;
                return std::make_unique<RealFir2>(nullptr,
                        std::make_unique<RealFir>(designLowpass(cut, cut * 0.4, /*deepStop=*/true), audioDecim_));
            };
            audioLpf_ = makeAudioLpf();
            lmrLpf_   = makeAudioLpf();
            pll_.configure(19000.0, chFs_); pll_.reset();
            stereoBlend_ = 0.0f;               // new tune starts mono, blends up
            // ★ The noise meter reads the MPX at the CHANNEL rate (where 17 kHz still exists),
            //   not the audio rate. Starting wide open matters: a new station must be given the
            //   benefit of the doubt and narrowed if it earns it, not opened up from mono —
            //   which would be audible as a swell on every retune.
            mpxNoise_.configure(chFs_);
            multipath_.configure(chFs_);
            adaptIf_.configure(chFs_); adaptIf_.setBandwidth(ifBwHz_);
            nb_.configure(chFs_, 0.020, 4.0f, 8.0 / std::max(1.0, chFs_)); nbRate_ = 0.0f;   // 8 samples, as before
            /* ★★★ THE DEVIATION PEAK-HOLD MUST NOT COUNT THE TUNE-IN TRANSIENT. A retune makes
             *   the discriminator produce a genuinely enormous excursion — the PLL slews, the IF
             *   filter rings, the AGC steps — so the hold caught it and then decayed at its own
             *   6 s rate, reading "110 kHz" on BBC Radio 1 and settling to 69. That is a real
             *   output, but it is not the STATION's deviation, and attributing it to the station
             *   is what made the readout look broken.
             * ★★ Stuart diagnosed it from the DECAY PATTERN — "peaked at over 100, slowly
             *   decayed, now hanging around 69" — while I had already written the measurement
             *   off as wrong on the strength of that same peak. The settled figures were right
             *   all along: R1 68-69, Heart 73-74, BBC Northampton 69.
             * ★ Cleared here AND held off briefly below, because the transient outlasts the
             *   reconfigure itself. */
            mpxDevSm_ = 0.0f; mpxDevAvg_ = 0.0f; mpxDevHold_ = 0.0f; mpxDevSettle_ = 0.0;
            mpxNoiseSm_ = 0.0f; mpxDevOut_ = 0.0f; mpxDevAvgOut_ = 0.0f; mpxDevNoise_ = 0.0f;
            devWinCnt_ = 0; devWinGp_ = 0.0; devHist_.assign(kDevHistN, 0u);
            ceq_.configure(9); ceqOut_.configure(chFs_);
            ceqEngaged_ = false; ceqDwell_ = 0; ceqEffort_ = 0.0f;
            shadowIf_.configure(chFs_); shadowIf_.setBandwidth(shadowBwHz_);
            shadowFm_.reset();
            shadowNoise_.configure(chFs_, 17000.0);
            shadowPilot_.configure(chFs_, 19000.0);
            widePilot_.configure(chFs_, 19000.0);
            // ★ Same rule on a rebuild: a rate or mode change is not a reason to keep a narrowing
            //   the new configuration has not earned.
            ifBwReq_.store(0.0, std::memory_order_relaxed);
            ifGainDb_ = 0.0f; shadowTick_ = 0; ifWarm_ = 0; ifDwell_ = 0;
            lmrHiCutHz_ = 15000.0f; lmrHiCutY_ = 0.0f; blendSnrDb_ = 99.0f;
            imsBlendHz_ = 0.0f; imsWhy_ = 1;
            audioHiCutHz_ = 15000.0f; hiCutYL_ = hiCutYR_ = hiCutYM_ = 0.0f;
            const int rch = (int)std::llround(audFs_);
            resampR_ = std::make_unique<RationalResampler>(rch, outRate_);
            // ★ VIBE_WFM_MONO=1 takes the plain mono path (discriminator, low-pass, de-emphasis — no
            //   pilot PLL, no L-R, no noise meter): the cost figure for a Pi Zero W class box (2026-09-16).
            stereo_ = !std::getenv("VIBE_WFM_MONO"); lastStereo_ = false;

            // RDS: coherent 57 kHz demod -> parallel-phase data-link decoders.
            RdsDecoder::Callbacks rcb; rcb.ctx = this;
            rcb.ps = [](void* c, uint16_t pi, const char* ps) {
                auto* self = (RxPipeline*)c;
                if (self->cb_.rdsPs) self->cb_.rdsPs(self->cb_.ctx, pi, ps);
            };
            rcb.radiotext = [](void* c, const char* rt) {
                auto* self = (RxPipeline*)c;
                if (self->cb_.rdsText) self->cb_.rdsText(self->cb_.ctx, rt);
            };
            rcb.ecc = [](void* c, uint16_t, uint8_t ecc) {
                auto* self = (RxPipeline*)c;
                if (self->cb_.rdsEcc) self->cb_.rdsEcc(self->cb_.ctx, ecc);
            };
            rcb.pi = [](void* c, uint16_t pi) {
                auto* self = (RxPipeline*)c;
                if (self->cb_.rdsPi) self->cb_.rdsPi(self->cb_.ctx, pi);
            };
            rdsDemod_.configure(chFs_, rcb);
            rdsDemod_.setNoiseCorrection(rdsNoiseCorr_.load());
            break;
        }
    }
    resamp_ = std::make_unique<RationalResampler>((int)std::llround(audFs_), outRate_);

    baseBuf_.clear(); chBuf_.clear(); demodBuf_.clear(); audioBuf_.clear();
    dirty_ = false;
    ++rebuilds_;
}

void RxPipeline::feed(const cf32* iq, int n) {
    // ★ Every request below rebuilds or re-seeds state the demod worker owns. Let it finish the
    //   blocks it has first, so the code below stays single-threaded exactly as it was written.
    //   A retune is rare; one block of waiting for it is nothing.
    if (demodOn_ && (dirty_ || resetReq_.load(std::memory_order_relaxed)
                     || rdsNoiseCorrReq_.load(std::memory_order_relaxed)
                     || rdsResyncReq_.load(std::memory_order_relaxed)
                     || bwReq_.load(std::memory_order_relaxed)
                     || tuneReq_.load(std::memory_order_relaxed)))
        flushDemod_();
    // ★★ A GAP IN THE STREAM INVALIDATES EVERY RECURSIVE STATE. Honoured HERE because
    // this is the thread that owns them (see requestReset). The RDS decoder is the one
    // that mattered in the field: its timing hypotheses kept their scores across an
    // idle pause, so a stale one could out-score the correctly-aligned one for good.
    if (resetReq_.exchange(false, std::memory_order_relaxed)) {
        rdsDemod_.reset();
        // ★ A FULL reset genuinely does want the loop rebuilt: a gap in the stream invalidates
        //   every recursive state, the PLL's included, and this path already rebuilds the audio
        //   chain around it. That is NOT true of the narrow resync below.
        pll_.configure(19000.0, chFs_);      // re-seed the pilot loop from scratch
        deemph_.reset();
        deemphR_.reset();
        dirty_ = true;                       // and rebuild the audio chain around them
    }
    // ★ Narrow resync: drop the previous station's RDS state, but leave the audio chain (and its
    //   AGC) alone. Ordered after the full reset above so the two do not fight.
    // ★★★ AND LEAVE THE PILOT LOCK ALONE TOO. This called pll_.configure(), which resets the
    //     phase, the loop integrator, lockAmp and lockState — so every RDS resync DESTROYED a
    //     perfectly good stereo lock and forced a full re-acquisition. The listener heard stereo
    //     drop and return together with the RDS data, and it had done so for as long as the
    //     feature existed: "stereo always dropped when switching between standard and advanced
    //     RDS too" (Stuart, 2026-08-14). A resync is asked for on every retune AND whenever a
    //     decoder is attached or detached — constantly, while somebody is listening.
    // ★★ The BIT CLOCK is all RDS needed re-timing, and that is a counter — (cycle*2pi + phase)/16
    //    — not the loop tracking the pilot. The decoder re-acquires its own symbol timing anyway;
    //    rdsDemod_.reset() is what clears its hypotheses.
    // ★ The guard-band switch, applied WITHOUT a rebuild — see setRdsNoiseCorrection. It is a flag
    //   on the decoder, and it is toggled every time somebody opens or closes the Advanced RDS
    //   panel, which is far too often to be spending an audio chain on.
    if (rdsNoiseCorrReq_.exchange(false, std::memory_order_relaxed))
        rdsDemod_.setNoiseCorrection(rdsNoiseCorr_.load(std::memory_order_relaxed));
    if (rdsResyncReq_.exchange(false, std::memory_order_relaxed) && chFs_ > 0.0) {
        rdsDemod_.reset();
        pll_.resyncBitClock();
    }
    bool rebuilt = false;
    if (dirty_) {
        rebuildAudio();                  // rebuildAudio() re-points the NCO itself
        rebuilt = true;
        // The rebuild designed for the exact current width, so a queued smooth request is
        // already satisfied; leaving it set would retune on the next block for nothing.
        bwReq_.store(false, std::memory_order_relaxed);
    }
    // ★ A width change the chain was built to absorb: swap the selectivity filter's taps and
    //   leave everything else running — no cleared buffers, so no gap in the audio and no stalled
    //   spectrum frame. If the chain cannot take it after all, fall back to the honest rebuild
    //   rather than run a block on a filter that no longer matches the requested width.
    else if (bwReq_.exchange(false, std::memory_order_relaxed) && !applySmoothBandwidth()) {
        rebuildAudio();
        rebuilt = true;
    }
    // A same-chain retune: nothing to rebuild, just move the oscillator. Skipped when a
    // rebuild already ran this block, since that has applied the newer offset anyway.
    if (!rebuilt && tuneReq_.exchange(false, std::memory_order_relaxed)) {
        nco_.setFreq(offsetHz_ / sampleRate_);
        // ★★★ AND RE-ACQUIRE THE PILOT, BECAUSE THIS IS A DIFFERENT STATION. Moving the NCO puts a
        //     step through the whole chain, and the pilot PLL is second-order with a deliberately
        //     narrow loop bandwidth (1% of the pilot frequency) — so its integrator can be kicked
        //     outside the pull-in range and simply never come back. The pilot is plainly present in
        //     the MPX and the loop sits there not finding it.
        // ★★★ THIS IS THE HALF I DELETED IN 3.0.0-92. The old code did it from the RDS resync,
        //     which was doing TWO jobs with one call: re-acquiring after a retune (essential) and
        //     tearing down a perfectly good lock whenever a decoder was attached (gratuitous). I
        //     removed the call to stop the second and lost the first with it — "RDS worked when I
        //     first switched to advanced, then I tuned away and tuned back and it never came back"
        //     (Stuart, 2026-08-14). One call, two purposes, and only one of them was in the comment.
        // ★ Here rather than in the resync, because THIS is the event that means "different
        //   signal". A decoder being attached does not.
        if (chFs_ > 0.0) pll_.configure(19000.0, chFs_);
        // ★★★ AND THE IF GOES BACK TO WIDE, BECAUSE NARROWING IS A DECISION ABOUT A STATION.
        //     The engage rule needs 3 dB of benefit in EITHER direction — deliberately, so it
        //     cannot chatter on the boundary — and the consequence is that BOTH states are stable.
        //     So a filter earned on one station was carried into the next one: "tune from 103.8
        //     which needs the IMS up to 104.2, the super strong Radio Northampton, and the IMS
        //     stays on; tune DOWN to 104.2 from above and it doesn't activate, which is the
        //     expected behaviour" (Stuart, 2026-08-15). Two routes to the same dial reading, two
        //     different receivers.
        // ★★ WIDE IS THE SAFE DEFAULT, and the design already says so: the cost of narrowing is
        //    distortion on deviation peaks, "which is why it must be earned rather than applied by
        //    default". Carrying it over is applying it by default to a station that never earned
        //    it — and on a strong local one there is nothing to earn it with.
        // ★ The warm-up is restarted too. The measurement now concerns a different signal, and
        //   deciding from an average of the previous one is the same fault as deciding from an
        //   unsettled one.
        ifBwReq_.store(0.0, std::memory_order_relaxed);
        ifGainDb_ = 0.0f; ifDwell_ = 0; ifWarm_ = 0;
    }

    // ── Spectrum ───────────────────────────────────────────────────────────
    // Gather fftSize contiguous samples for a frame, then skip to the next slot.
    if (cb_.spectrum) {
        // Gather fftSize contiguous samples into a frame, FFT + emit, then DROP the
        // remaining (specStride - fftSize) samples to honour the frame rate. This is
        // O(n) — never the per-sample buffer shift (O(n*fftSize)) that can't keep up
        // at MS/s. `sinceFrame_` doubles as the inter-frame drop countdown.
        if ((int)specRing_.size() != fftSize_ * 2) {
            specRing_.assign((size_t)fftSize_ * 2, 0.0f);
            specRingW_ = 0; specRingFill_ = 0; sinceEmit_ = 0;
        }
        cf32* ring = reinterpret_cast<cf32*>(specRing_.data());
        cf32* sb   = reinterpret_cast<cf32*>(specBuf_.data());
        // ★ Frames the worker finished since the last block are delivered HERE, on the DSP thread —
        //   the callback never runs anywhere else. See setSpectrumThread.
        // ★★ EVERY ready frame, not one: a feed submits several windows (see the kSpecQ note), so
        //    collecting a single one per call would leave the queue permanently full and re-create
        //    the ceiling the queue exists to remove.
        if (specThreadOn_) drainSpecQueue_();
        const long long stride = std::max(1, specStride_.load(std::memory_order_relaxed));
        for (int i = 0; i < n; ) {
            const int room   = fftSize_ - specRingW_;
            /* ★★★ THE STRIDE CAN SHRINK UNDER US (2026-09-17, the XCover crashing every two hours):
             *     a listener arriving lifts the frame rate from the 2 fps idle floor to 10, so
             *     `stride` drops below the samples already counted since the last frame, and
             *     stride − sinceEmit_ went NEGATIVE — memmove with a length of 0xffffffffffe2ab80,
             *     SIGSEGV in vibe-dsp, and the auto-restore hiding it as a "blank spectrum, off the
             *     tunnel, back in a few minutes" cycle. The per-sample loop this replaced could not
             *     do that: it only ever compared. Clamp to at least one sample, so an overdue frame
             *     is emitted on the very next sample. */
            long long toEmit = stride - sinceEmit_;
            if (toEmit < 1) toEmit = 1;
            const int chunk = (int)std::min<long long>({(long long)(n - i), (long long)room, toEmit});
            std::memcpy(ring + specRingW_, iq + i, (size_t)chunk * sizeof(cf32));
            i += chunk;
            specRingW_ += chunk; if (specRingW_ >= fftSize_) specRingW_ = 0;
            specRingFill_ += chunk;
            sinceEmit_ += chunk;
            if (sinceEmit_ < stride) continue;   // ★ >=, not ==: the overdue case above lands here too
            sinceEmit_ = 0;
            if (specRingFill_ < fftSize_) continue;      // warm-up: not a full window yet
            // Unwrap oldest-first so the window is time-ordered; specRingW_ is the oldest
            // sample now that it has advanced past the newest.
            const int tail = fftSize_ - specRingW_;
            std::memcpy(sb,        ring + specRingW_, (size_t)tail       * sizeof(cf32));
            std::memcpy(sb + tail, ring,              (size_t)specRingW_ * sizeof(cf32));
            if (specThreadOn_) {
                /* ★★★ REVERTED, DELIBERATELY, PENDING MEASUREMENT (2026-09-25).
                 *
                 *  Draining here as well as at the top of feed() is what lifted the 7.8 fps ceiling
                 *  — and it is also what made the Pi 2 stutter: `cb_.spectrum` converts a row and
                 *  hands it to every listener ON THE DSP THREAD, which owes the audio a block every
                 *  32 ms, so several deliveries inside one block miss the deadline. The box was at
                 *  47 % CPU with its clock at maximum, so this was never a shortage of CPU; it was a
                 *  BURST in a thread that cannot afford one.
                 *  ★★ Stuart's call, and the right one: "I'd rather have the broken 8fps and it
                 *     working than this." A waterfall at 7.8 instead of 10 is a cosmetic loss; audio
                 *     that breaks up is the product failing at its job. The thread priority rule
                 *     says the same thing — AUDIO outranks SPECTRUM, and this traded the first for
                 *     the second.
                 *  ★ The queue itself stays (kSpecQ), because it is harmless: with one collect per
                 *    feed it simply never fills. Re-raising the ceiling needs the delivery moved off
                 *    this thread, or spread across blocks — not more work per block — and that needs
                 *    measuring on the Pi 2 before it goes anywhere near it again.
                 *  ★ The original note, for whoever picks this up: */
                /* ★★★ DRAIN HERE TOO, NOT ONLY ONCE PER feed() — THIS COST THE WEAK BOXES HALF
                 *     THEIR WATERFALL (2026-09-24).
                 *
                 *  The collect at the top of feed() runs ONCE per call, and feed() is called once
                 *  per audio block: 48000/1536 = 31.25 times a second, fixed, whatever the RF
                 *  sample rate. Submission refused while the ONE slot was occupied — a finished
                 *  frame nobody had collected blocked the next — so the pair formed a single-slot
                 *  pipe drained at the audio cadence and the whole chain topped out at
                 *  31.25 / FFT_AVG = 7.8125 fps. Measured on the Pi 2 and the Sony TV: both sat at
                 *  7.87 fps when asked for 20, from DIFFERENT sample rates (1.2 and 2.048 MS/s),
                 *  with the CPU 50 % idle and vibe-spec using 19 % of one core. Everything above
                 *  7.8 went into specDropped_.
                 *  ★★ AND IT HIT ONLY THE MACHINES THE THREAD SPLIT EXISTS TO HELP: the worker runs
                 *     only where VIBE_DSP_THREADS is set, which main.cpp does under
                 *     `#if defined(__arm__) && !defined(__aarch64__)` and the Lite app does
                 *     explicitly. Every 64-bit server takes the inline path below and reached 20.
                 *  ★ Stuart, 2026-09-24: "8FPS isn't terrible ... its just a weird number that
                 *    looks like an error rather than intentional" — it WAS an error.
                 *  ★ Still delivered on the DSP thread, which is the invariant that matters (see
                 *    setSpectrumThread): the callback never runs on the worker. */
                bool taken = false;
                {
                    std::lock_guard<std::mutex> lk(specM_);
                    if (specSlot_[specWr_] == SPEC_FREE && specWorkN_ == fftSize_) {
                        std::memcpy(specWork_[specWr_].data(), sb, (size_t)fftSize_ * sizeof(cf32));
                        specSlot_[specWr_] = SPEC_PENDING;
                        specWr_ = (specWr_ + 1) % kSpecQ;
                        taken = true;
                    }
                }
                if (taken) specCv_.notify_one();
                else specDropped_.fetch_add(1, std::memory_order_relaxed);   // behind: drop, never wait
                continue;
            }
            const float scale = 1.0f / (float)(fftSize_ * fftSize_);
            cfft_->powerDbShifted(sb, win_.data(), specDb_.data(), scale);
            cb_.spectrum(cb_.ctx, specDb_.data(), fftSize_);
        }
    }

    // ── Zoom spectrum ──────────────────────────────────────────────────────
    // Runs only while a view is set (i.e. the user has zoomed past what the wide FFT can
    // resolve), so it costs nothing in the common case. Configured HERE, on the DSP thread,
    // rather than from whichever thread moved the view: it rebuilds filter state.
    if (cb_.zoomSpectrum) {
        if (zoomDirty_.exchange(false, std::memory_order_acq_rel)) {
            const double span = zoomSpanReq_.load(std::memory_order_relaxed);
            if (span <= 0.0) {
                if (zoom_) zoom_->disable();
                zoomSpanOut_.store(0.0, std::memory_order_relaxed);
            } else {
                // Rebuild on a width change too — and do it HERE, on the DSP thread. Destroying
                // it from the caller's thread would free buffers a feed() in flight is reading.
                const int wantBins = zoomBins_.load(std::memory_order_relaxed);
                if (zoom_ && zoom_->bins() != wantBins) zoom_.reset();
                if (!zoom_)
                    zoom_ = std::make_unique<ZoomSpectrum>(
                        sampleRate_,
                        sharedChannels_ ? ZoomSpectrum::Method::Shared
                                        : ZoomSpectrum::Method::Direct,
                        zoomBins_.load(std::memory_order_relaxed));
                if (zoomLog_) zoom_->setLog(zoomLog_);
                zoom_->configure(zoomOffReq_.load(std::memory_order_relaxed), span,
                                 zoomRateReq_.load(std::memory_order_relaxed));
                zoomSpanOut_.store(zoom_->spanHz(), std::memory_order_relaxed);
            }
        }
        if (zoom_ && zoom_->enabled())
            zoom_->feed(iq, n, [&](const float* db, int nb) {
                cb_.zoomSpectrum(cb_.ctx, db, nb);
            });
    }

    /* ★★★ SPECTRUM ONLY — FOR A SIGNAL THIS PIPELINE CANNOT DEMODULATE ANYWAY.
     *  In DAB the shim still feeds this pipeline, because the waterfall, the frame rate, the link
     *  meter and the view state all ride on the spectrum socket — without it the whole UI
     *  degrades. But it was running the ENTIRE audio chain as well: nco_.mix() over every sample
     *  at 2.4 MS/s, the full decimation cascade, the demodulator, RDS — and then the shim threw
     *  the audio away at the far end (see the g_dabMode early-returns in onAudio/onDecoders).
     *  ★★★ THE PI ABSORBED IT AND THE PHONE DID NOT. Measured on the Xcover, 2026-09-05: USB
     *      delivering a perfect 100% of 2.4 MS/s while iqDrops climbed at 5.71/s — 18% of the
     *      stream thrown away for want of DSP time — and 1981 of 2016 MP2 frames failing. The
     *      holes are punched in the IQ before the DAB receiver ever sees it.
     *  ★ The spectrum and zoom blocks above still run; only the audio half is skipped. */
    if (spectrumOnly_.load(std::memory_order_relaxed)) return;

    // ── Audio (DDC -> demod -> resample) ─────────────────────────────────────
    if (cb_.audio) {
        baseBuf_.resize(n);
        nco_.mix(iq, baseBuf_.data(), n);

        // Run the decimation cascade, ping-ponging between two buffers. Each stage
        // drops the rate by its own factor; the last one is the channel filter.
        int nc = n;
        const cf32* src = baseBuf_.data();
        for (auto& d : decs_) {
            std::vector<cf32>& dst = (src == baseBuf_.data()) ? chBuf_ : baseBuf_;
            dst.resize(d->maxOut(nc));
            nc = d->process(src, nc, dst.data());
            src = dst.data();
        }
        // Make sure the demods below always read from chBuf_, whichever buffer the
        // cascade happened to land in (an even number of stages ends on baseBuf_).
        if (src != chBuf_.data()) {
            chBuf_.assign(src, src + nc);
        }
        // ★ RAW IQ OUT tap — the channel as it stands, before the demod touches anything.
        // ★ THE CUT (setDemodThread): everything above is the DSP thread's, everything below may run
        //   on vibe-demod. The channel block is handed over whole; a full queue is WAITED for, never
        //   dropped — a hole in the channel is a click, and a late block is only a late block.
        if (demodOn_) { enqueueDemod_(chBuf_.data(), nc); return; }
        demodTail_(chBuf_, nc);
    }
}

/** ★ The second half of the audio path: channel IQ in, audio (and every RDS/stereo/meter callback)
 *  out. On the DSP thread by default; on vibe-demod with setDemodThread. `chB` is the channel buffer
 *  it owns for this call — the DSP thread keeps writing its own chBuf_ meanwhile. */
void RxPipeline::demodTail_(std::vector<cf32>& chB, int nc) {
    {
        faultStage_ = nullptr;          // per-block: trace_() records the FIRST bad stage
        if (cb_.iq && nc > 0) cb_.iq(cb_.ctx, chB.data(), nc, chFs_);

        demodBuf_.resize(nc);
        // ★★★ MEASURED ON THE IQ, BEFORE DEMODULATION — this is the ONLY place the information
        //     exists. The FM demodulator throws amplitude away by design (that is what makes FM
        //     immune to AM noise), so after this line the envelope wobble that reveals multipath
        //     is simply gone. Read-only; chB is untouched.
        // ★ The adaptive IF sits BEFORE the multipath meter and the demod, because it is part of
        //   the receiver, not part of the measurement — everything downstream should see the
        //   signal as filtered, exactly as it would with a narrower crystal filter.
        // ★★★ THE SHADOW COPY IS TAKEN BEFORE THE ADAPTIVE FILTER, ALWAYS. If it were taken after,
        //     the comparison would depend on what the filter is currently doing and the control
        //     would be steering by its own output — the feedback trap this whole design is built to
        //     avoid ("a probe is part of the system it measures").
        if (mode_ == Mode::WFM && shadowTick_ + 1 >= 4) {
            shadowBuf_.assign(chB.begin(), chB.begin() + nc);
        }
        if (mode_ == Mode::WFM) {
            // ★★★ THE NARROWER OF THE TWO REQUESTS — see setAutoBandwidth for why there are two.
            //     0 means "no request from me", not "open the filter", so it must never win a
            //     comparison; only a real width can.
            const double imsWant  = ifBwReq_.load(std::memory_order_relaxed);
            const double autoWant = autoBwReq_.load(std::memory_order_relaxed);
            const double want = (imsWant > 0.0 && autoWant > 0.0) ? std::min(imsWant, autoWant)
                              : (imsWant > 0.0 ? imsWant : autoWant);
            if (want != ifBwHz_) { ifBwHz_ = want; adaptIf_.setBandwidth(want); }
            adaptIf_.process(chB.data(), nc);
        }
        // ── NOISE BLANKER ────────────────────────────────────────────────────────────────────
        // ★★★ FIRST IN THE CHAIN, because everything after it AVERAGES. The noise meters, the
        //     multipath meter and the equaliser all integrate, so an impulse left in place is
        //     smeared across their readings and pulls every one of them the wrong way — and the
        //     equaliser would then try to "correct" a channel it had been told about wrongly.
        // ★★ AN HONEST LIMITATION, RECORDED: this runs at the CHANNEL rate, after decimation, and
        //    an impulse is wideband and microseconds long. The decimation filters have already
        //    smeared it into a short ring by the time we see it, so we excise the ring rather than
        //    the spike. Blanking on the raw input would be more effective and costs a magnitude
        //    per sample at the full rate — real money on a Pi that already runs its DSP near real
        //    time. If the measured benefit is not there, this belongs earlier, not tuned harder.
        // ★★ TWO SWITCHES, ONE BLANKER. On WFM this is the TEF6686-style NB in the Broadcast FM
        //    processing row (nbOn_), untouched. On every other mode it is the listener's own
        //    NOISE BLANKER in the audio menu (nbxOn_), which did not exist before — on HF, where
        //    the impulses are, a listener had no blanker at all (Stuart, 2026-09-10). Same
        //    vectorised engine either way (iqclean.cpp).
        const bool on = (mode_ == Mode::WFM) ? nbOn_.load(std::memory_order_relaxed)
                                             : nbxOn_.load(std::memory_order_relaxed);
        if (on) {
            nb_.process(chB.data(), nc);
            nbRate_ += 0.1f * (nb_.rate() - nbRate_);
            if (!std::isfinite(nbRate_)) nbRate_ = 0.0f;
        } else {
            nbRate_ = 0.0f;
        }

        // ★ The RAW reading first — what arrived, before we touch it. It is both the trigger for
        //   the equaliser and half of its scorecard, so it must never see the equaliser's output.
        if (mode_ == Mode::WFM) multipath_.process(chB.data(), nc);

        // ── CEQ ──────────────────────────────────────────────────────────────────────────────
        // ★★★ GATED ON A GOOD SIGNAL WITH REAL MULTIPATH, because those are the only conditions in
        //     which it can help. A CMA equaliser fed NOISE will happily contort itself trying to
        //     correct randomness and end up amplifying it — the same trap as the adaptive IF, where
        //     the right treatment applied to the wrong fault is a regression. multipathValid_ is
        //     what makes this decidable: below ~12 dB we do not even claim to know whether the
        //     trouble is a reflection.
        if (mode_ == Mode::WFM) {
            const bool ceqAllowed = ceqOn_.load(std::memory_order_relaxed);
            const bool strongEnough = blendSnrDb_ > 18.0f;
            /* ★★★ HYSTERESIS, BECAUSE ONE THRESHOLD ON A WANDERING MEASUREMENT IS A FLAP. This
             *     engaged above 6 % multipath and disengaged below the same 6 %, so a signal
             *     sitting near that figure walks in and out of equalisation for ever — and every
             *     crossing calls ceq_.reset(), which restarts a blind CMA equaliser from nothing
             *     in the middle of a composite that was perfectly good. The damage is done by the
             *     TRANSITIONS, not by the steady state, which is why the readout can honestly say
             *     "standing by · nothing to correct" while the audio is being wrecked.
             *  ★ Stuart's A/B on 96.6 MHz, his strongest local signal, seconds apart — multipath
             *     5.0 % with CEQ on and 5.4 % with it off, both within a whisker of the 6 % line:
             *         CEQ on — RDS errors 12 %, RDS deviation 0.0 kHz (no subcarrier at all),
             *                  pilot 5.5 kHz "low", 80 % constellation scatter, blend down to 13.7 k
             *         CEQ off — RDS errors 0 %, deviation 0.6 kHz, pilot 6.6 kHz nominal,
             *                  38 % scatter, "clean · no treatment"
             *     The RDS constellation told it plainest: two clean BPSK lobes became a rotating
             *     smear, and the RDS-to-pilot rotation went from 3°/s to 7°/s. An equaliser that
             *     rotates the composite is adding group delay, not removing it.
             *  ★★ So: engage only at 12 %, where a reflection is not in doubt, and do not let go
             *     until 5 %. Two thresholds that cannot meet, on a measurement that wanders by a
             *     point either way. "The CEQ we built and tested on RTL-SDR's is damaging here"
             *     (Stuart, 2026-09-12) — it was built against a front end whose multipath figure
             *     sat somewhere else entirely.
             *  ★★★ The same fault shape as the AGC window narrower than one LNA step, and the
             *      SECOND time today a single threshold on a noisy quantity has produced an
             *      oscillation nobody could see from the logs. */
            const bool worthIt = multipathValid_ &&
                (ceqEngaged_ ? multipathCorr_ > 0.05f : multipathCorr_ > 0.12f);
            const bool want = ceqAllowed && worthIt && strongEnough;
            // ★★ SAY WHICH CONDITION FAILED. "Standing by" is honest but useless on its own: the
            //    owner cannot tell whether the equaliser has declined, is broken, or is waiting for
            //    something. The S/N gate is the one that surprises people, because a signal being
            //    FOUGHT OVER reads as severe multipath while sitting well under 18 dB.
            ceqWhy_ = !ceqAllowed ? 1 : (!strongEnough ? 2 : (!worthIt ? 3 : 0));
            /* ★★★ IMS'S VERDICT BELONGS HERE TOO — NOT INSIDE THE BLEND BRANCH. It was assigned
             *     only where the L-R corner is computed, and that branch needs a LOCKED PILOT. So
             *     on exactly the signals worth asking about — Stuart's 105.4, 46% multipath,
             *     constellation reading "no lock" — the branch never ran, imsWhy_ kept its initial
             *     value of 1 ("switched off"), and the readout stayed silent however many times
             *     the message was fixed. Three rounds of "still not seeing any IMS status" came
             *     from this one line's position.
             *  ★★ THE LESSON IS THE ONE ceqWhy_ ALREADY LEARNED: a status must be produced on the
             *     same schedule as the thing it describes, not as a side effect of the code that
             *     happens to act. CEQ computes its verdict here, unconditionally, and has never
             *     had this problem.
             *  ★ The acting case (0) is set later, where the corner is actually taken. */
            imsBlendHz_ = 0.0f;
            imsWhy_ = !imsOn_.load(std::memory_order_relaxed) ? 1 : ceqEngaged_ ? 2
                    : !multipathValid_ ? 5 : multipathCorr_ <= 0.06f ? 3 : 4;
            if (want && !ceqEngaged_) {
                if (++ceqDwell_ > 20) { ceqEngaged_ = true; ceqDwell_ = 0; ceq_.reset(); }
            } else if (!want && ceqEngaged_) {
                if (++ceqDwell_ > 20) { ceqEngaged_ = false; ceqDwell_ = 0; ceq_.reset(); }
            } else ceqDwell_ = 0;

            if (ceqEngaged_) {
                // ★ A small step size. This runs at the channel rate, and a fast CMA on anything
                //   less than a clean signal is exactly how the algorithm goes wrong.
                ceq_.process(chB.data(), nc, 2.0e-4f);
                ceqEffort_ = ceq_.effort();
                ceqOut_.process(chB.data(), nc);   // ...and score ourselves on the result
            } else {
                ceqEffort_ = 0.0f;
                ceqOut_.reset();
            }
        }
        // ★★★ THE SHADOW RECEIVER — one block in four, a narrower copy demodulated in parallel so
        //     the BENEFIT of narrowing can be read off the panel instead of guessed at. Compared
        //     like with like: the same pilot-to-guard-band ratio, measured the same way on both,
        //     because the running PLL's lockAmp describes the WIDE signal and would flatter the
        //     narrow one. Nothing here touches the audio — it is a measurement, not a path.
        if (mode_ == Mode::WFM && ++shadowTick_ >= 4) {
            shadowTick_ = 0;
            // ★★★ THE SHADOW EVALUATES THE OTHER OPTION, not a fixed one. Wide open while the
            //     audio path is narrowed; narrowed while the audio path is wide. So ifGainDb_
            //     always answers ONE question — "would switching be better?" — and it answers it
            //     about a signal that has not been touched by the control it is steering.
            shadowIf_.setBandwidth(ifBwHz_ > 0.0 ? 0.0 : shadowBwHz_);
            shadowIf_.process(shadowBuf_.data(), nc);
            shadowMpx_.resize(nc);
            shadowFm_.process(shadowBuf_.data(), shadowMpx_.data(), nc);
            shadowNoise_.process(shadowMpx_.data(), nc);
            shadowPilot_.process(shadowMpx_.data(), nc);
            widePilot_.process(demodBuf_.data(), nc);
            const float sn = shadowNoise_.level(), sp = shadowPilot_.level();
            const float wn = mpxNoise_.level(),    wp = widePilot_.level();
            if (sn > 1e-9f && wn > 1e-9f && sp > 1e-9f && wp > 1e-9f) {
                const float narrowDb = 20.0f * std::log10(sp / sn);
                const float wideDb   = 20.0f * std::log10(wp / wn);
                const float g = narrowDb - wideDb;
                if (std::isfinite(g)) ifGainDb_ += 0.05f * (g - ifGainDb_);
            }
            if (!std::isfinite(ifGainDb_)) ifGainDb_ = 0.0f;

            // ★★★ THE POLICY, AND IT IS DRIVEN BY THE MEASURED BENEFIT — not by the noise figure.
            //     Measured at 110 kHz: alone and clean -9.8 dB, alone and hissy -1.0 dB, weak
            //     neighbour -1.1 dB, STRONG neighbour +10.7 dB. Narrowing does not help against
            //     noise at all; it trades noise for distortion and loses. It helps against an
            //     adjacent CHANNEL, and there it is worth ten decibels. Steering this from MPX S/N
            //     — the original plan — would have narrowed on exactly the signals it cannot help.
            // ★★ A LONG DWELL, because switching is audible and a wrong switch is worse than a
            //    late one. Three dB of margin and several seconds of agreement before it moves,
            //    and the SAME margin in both directions so it cannot sit on the boundary
            //    chattering between two states.
            // ★★★ NOTHING IS DECIDED UNTIL THE MEASUREMENT HAS SETTLED. ~5 s at one evaluation
            //     per 4 blocks; the averages behind it use a 0.05 coefficient, so they need
            //     roughly that long to mean anything at all.
            /* ★★★ IMS NO LONGER COMMANDS THE IF FILTER — AUTO BANDWIDTH OWNS IT NOW.
             *  ★★★ THE TWO WERE THE SAME FEATURE WEARING DIFFERENT NAMES, and Stuart saw it from
             *      the outside before the code admitted it: "auto bw and IMS seem to be the same
             *      thing", "IMS just seems to be an auto narrowing of the passband". Both narrowed
             *      this one filter; they differed only in what steered them, so they fought over
             *      the slot and whichever wrote last won.
             *  ★★★ AND OUR "IMS" WAS MISNAMED AGAINST THE PART WE COPIED IT FROM. On a TEF6686,
             *      iMS is Intelligent Multipath Suppression — a treatment for REFLECTIONS. Its
             *      bandwidth control is a separate feature, which is our auto bandwidth. So the
             *      name goes back to the job it describes (see the L-R corner below) and the
             *      bandwidth logic goes to the control that was already doing it.
             *  ★★ THE SHADOW MEASUREMENT STAYS AND STILL RUNS. It is what puts "wide would cost
             *     2.1 dB" in the IF NARROW readout, and it remains the only honest answer to
             *     "would narrowing actually help here?" — it just no longer commands anything. */
            if (ifWarm_ < 64) ++ifWarm_;
        }
        // ★★★ TAKE THE NOISE BACK OUT, OR THIS METER LIES WHERE IT MATTERS MOST. Noise shakes the
        //     envelope exactly as a reflection does, and the first version reported the sum. On
        //     air at 107.8 (S8, MPX S/N 6 dB) it showed 25.7% — "severe" — which would have sent
        //     the owner rotating an aerial to cure a reflection that was very likely not there
        //     (Stuart, 2026-08-14). The lab had only ever demonstrated the discrimination at
        //     MODERATE noise, and 6 dB is far outside that.
        // ★★ MEASURED, NOT ASSUMED. test-multipath-meter sweeps noise with NO echo at all and
        //    prints what the meter reads: 34 dB -> 0.001, 28 -> 0.074, 24 -> 0.117, 12 -> 0.166,
        //    3 -> 0.208. That curve is this table. It must be re-read whenever the meter's filters
        //    change — the printout exists so it cannot quietly go stale.
        // ★★★ SUBTRACTED IN THE POWER DOMAIN, because the two contributions are UNCORRELATED and
        //     therefore add as powers, not as amplitudes. Subtracting linearly would over-remove
        //     and report clean paths as pristine while hiding real reflections.
        if (mode_ == Mode::WFM) {
            static constexpr float kSnr[]   = { 34.3f, 28.0f, 23.8f, 12.5f, 3.0f, -15.0f };
            static constexpr float kDepth[] = { 0.001f, 0.074f, 0.117f, 0.166f, 0.208f, 0.241f };
            const float s = blendSnrDb_;
            float expect = kDepth[0];
            if (s <= kSnr[5]) expect = kDepth[5];
            else if (s < kSnr[0]) {
                for (int i = 0; i < 5; ++i) {
                    if (s <= kSnr[i] && s > kSnr[i + 1]) {
                        const float t = (kSnr[i] - s) / (kSnr[i] - kSnr[i + 1]);
                        expect = kDepth[i] + t * (kDepth[i + 1] - kDepth[i]);
                        break;
                    }
                }
            }
            const float d = multipath_.depth();
            const float p = d * d - expect * expect;
            multipathCorr_ = (p > 0.0f) ? std::sqrt(p) : 0.0f;
            // ★★ AND SAY WHEN WE CANNOT TELL. Below ~12 dB the correction is subtracting nearly
            //    everything it measured, so the residual is the small difference of two large
            //    numbers — the classic way to produce a confident-looking figure that means
            //    nothing. Better to report "cannot tell" than to invent a verdict.
            // ★ ...and multipath is only knowable when the S/N figure driving its noise correction
            //   is itself trustworthy, and when the raw reading was physically possible.
            // ★★★ HYSTERESIS, OR THE READING FLASHES IN AND OUT. A single threshold at 12 dB meant
            //     a signal sitting near it flipped between a figure and "too noisy to judge"
            //     several times a second — unreadable, and it makes a stable measurement look
            //     unstable (Stuart, 2026-08-14: "can you make the multipath % stay on as it keeps
            //     flashing in and out"). Becoming trustworthy needs 13 dB; ceasing to be needs a
            //     fall to 10. The gap is the point: a boundary crossed by noise alone is not a
            //     change of state.
            const float need = multipathValid_ ? 10.0f : 13.0f;
            multipathValid_ = snrValid_ && multipath_.plausible() && (blendSnrDb_ > need);
        } else { multipathCorr_ = 0.0f; multipathValid_ = false; }
        if (am_)       am_->process(chB.data(), demodBuf_.data(), nc);
        else if (fm_)  fm_->process(chB.data(), demodBuf_.data(), nc);
        else if (ssb_) ssb_->process(chB.data(), demodBuf_.data(), nc);
        trace_("demod", demodBuf_.data(), nc);

        // ★★★ Strip the discriminator's DC before ANYTHING downstream sees it. That DC is
        // the tuning error, and left in place it eats the headroom and mutes an off-tune
        // station outright — see DcBlocker. It has to happen here, on the MPX, so that the
        // mono path, the stereo matrix and the resampler all inherit a centred signal.
        // ★★ KEEP THIS AFTER THE WHOLE if/else DEMOD CHAIN. Placed in the middle of it (as it
        // first was) `else if (ssb_)` binds to THIS if instead of the demod selection — which
        // compiles, and happens to behave because useFmDc_ is false in SSB. An accident that
        // works is still a trap for the next edit; it broke the moment a line was added.
        if (useFmDc_) fmDc_.process(demodBuf_.data(), nc);
        trace_("fmDc", demodBuf_.data(), nc);

        // ★★★ MEASURED FOR BOTH PATHS, ABOVE THE BRANCH. This lived inside the stereo branch when
        //     it only fed the high-blend, but the hiss it measures is in MONO too — Stuart tuned
        //     107.4 to mono and found "noise in mono too", which high-blend cannot touch by
        //     construction (it only ever acts on L-R). The audio high-cut below needs the same
        //     number, and a station listened to in forced mono has to get it as well.
        // ★ Still WFM-only: the 15-19 kHz gap it reads is a property of the FM multiplex and
        //   means nothing on AM or SSB.
        if (mode_ == Mode::WFM) {
            mpxNoise_.process(demodBuf_.data(), nc);
            if (mpxNoise_.ready()) {
                const float noise = mpxNoise_.level();
                const float pilot = std::fabs(pll_.lockAmp());
                // ★ In MONO the pilot PLL still runs, so the reference survives; but if it is not
                //   tracking at all we have no yardstick and must not invent one — hold the last
                //   figure rather than reporting a signal as perfect or as hopeless.
                // ★★★ NO PILOT, NO MEASUREMENT. Both of these figures are ratios against the
                //     19 kHz pilot, so when the pilot collapses the denominators do too and the
                //     numbers become nonsense: 70 dB of "MPX S/N" on a meter whose leakage floor
                //     caps it near 34, beside 580.9% "multipath", on a station reading 1.0 kHz of
                //     pilot deviation with its encoder unlocked (Stuart, 2026-08-14). A real pilot
                //     is 6.0-7.5 kHz by specification; below about 2 kHz there is nothing to
                //     measure against and the honest output is to say so and HOLD the last figure.
                // ★ lockAmp is deviation/75 kHz, so 2 kHz is ~0.027.
                const bool pilotUsable = (pilot > 0.027f);
                snrValid_ = pilotUsable;
                /* ★★★ A PROBE FOR THE FAULT NOBODY HAS PINNED DOWN: no stereo and no RDS on a
                 *     station that is plainly audible, cured only by tuning one click away and
                 *     back (Stuart, for months — 105.4, 97.2, and 104.7 beside the beacons).
                 *  ★★★ WHY A LIVE PROBE AND NOT A TEST. I hypothesised a runaway in the PLL's
                 *      frequency integrator and bounded it — then ran the controlled experiment
                 *      (same signal, fresh pipeline vs after 12 s of static) and it made NO
                 *      DIFFERENCE. The harness cannot produce whatever the real fault needs, and
                 *      the cure rules the integrator out anyway: a one-click retune keeps the mode
                 *      and bandwidth, so it takes setTune's same-chain path and never resets the
                 *      PLL at all. So the next move is to watch the real thing.
                 *  ★★ FIRES ONLY IN THE FAULT'S OWN SHAPE — a pilot present and usable by the
                 *     deviation meter, while the lock says otherwise. On a healthy station or a
                 *     dead one it says nothing, so this is not a log that has to be read past.
                 *  ★ Once a second at most. Delete when the cause is found. */
                if (pilotUsable && !pll_.locked()) {
                    if (++pilotOddTicks_ >= 40) {
                        pilotOddTicks_ = 0;
                        std::fprintf(stderr,
                            "[pilot] present but NOT LOCKED — dev %.1f kHz, lockAmp %.4f, "
                            "trackable %d, mpxS/N %.1f dB, lmrCut %.0f Hz\n",
                            pll_.pilotDeviationKHz(), pll_.lockAmp(), (int)pll_.trackable(),
                            blendSnrDb_, lmrHiCutHz_);
                    }
                } else pilotOddTicks_ = 0;
                if (pilotUsable) {
                    const float snr = (noise > 1e-9f) ? (pilot / noise) : 1e6f;
                    const float db  = 20.0f * std::log10(std::max(snr, 1e-6f));
                    blendSnrDb_ += 0.05f * (db - blendSnrDb_);
                    if (!std::isfinite(blendSnrDb_)) blendSnrDb_ = 99.0f;
                }
            }
            // ★★★ ADAPTIVE HIGH-CUT — "weak signal processing", and the answer to hiss that
            //     survives a switch to mono. Baseband noise sits in the audio band itself, so no
            //     amount of stereo treatment reaches it; the only honest cure is to stop
            //     reproducing the part of the band that is more noise than programme. This is what
            //     every broadcast receiver has always done, and it is musically benign — a moving
            //     low-pass has no artefacts, no pumping and no spectral holes, unlike a spectral
            //     denoiser, which is also why speech-tuned NR was never usable on music.
            // ★★ IT MUST STAY ABOVE THE L-R CORNER, ALWAYS. high-blend has already narrowed L-R;
            //    if this cut below it, it would silently make that work irrelevant and we would be
            //    running two filters to do one filter's job. Held a comfortable margin clear.
            // ★ Switched OFF -> glide the corner back to wide open rather than jumping. An A/B
            //   test wants to hear the DIFFERENCE, not a click, and a step change in a filter
            //   corner is audible in its own right — which would colour the very comparison the
            //   switch exists to make.
            const bool wsp = weakProcOn_.load(std::memory_order_relaxed);
            constexpr float kClean = 30.0f, kRough = 14.0f;     // the same window as the blend
            constexpr float kWide  = 15000.0f, kNarrow = 4500.0f;
            float t = (blendSnrDb_ - kRough) / (kClean - kRough);
            t = std::min(1.0f, std::max(0.0f, t));
            float wantCut = wsp ? (kNarrow + t * (kWide - kNarrow)) : 15000.0f;
            // ★★★ ONLY WHEN THE BLEND IS ACTUALLY ACTING — and getting this wrong DISABLED THE
            //     HIGH-CUT IN MONO ENTIRELY. The guard exists so the two filters do not fight: the
            //     audio cut must not sit below the L-R corner, or it would make high-blend
            //     irrelevant. But in forced mono there is no L-R to protect, so lmrHiCutHz_ glides
            //     back to 15 kHz — and `max(want, 15000 * 1.25)` then pinned the audio filter WIDE
            //     OPEN. A listener who pressed MONO to escape the hiss got LESS help than one who
            //     stayed in stereo, which is precisely backwards.
            // ★★ FOUND FROM TWO ON-AIR RECORDINGS, not from reading this code: the mono take rolled
            //    off at ~8 kHz and the stereo take at ~4 kHz, one minute apart on the same station.
            //    The innocent explanation (a better signal for the second) was plausible enough
            //    that only a test could separate them — and the test failed immediately.
            if (lmrHiCutHz_ < 14000.0f) wantCut = std::max(wantCut, lmrHiCutHz_ * 1.25f);
            wantCut = std::min(wantCut, 15000.0f);
            audioHiCutHz_ = glideCorner(audioHiCutHz_, wantCut);
            if (!std::isfinite(audioHiCutHz_)) audioHiCutHz_ = 15000.0f;
        } else {
            audioHiCutHz_ = 15000.0f;
        }

        if (stereo_) {
            // ── WFM stereo MPX decode ────────────────────────────────────────
            // demodBuf_ is the MPX. L+R = LPF(mpx); L-R = LPF(mpx * 38kHz_ref).
            // Only generate the 57 kHz reference and bit clock when something is
            // actually listening for RDS — that is a third of the PLL's per-sample
            // work, and with no subscriber it was being computed and thrown away.
            const bool wantRds = rdsEnabled_.load(std::memory_order_relaxed)
                              && (cb_.rdsPs || cb_.rdsText || cb_.rdsPi || cb_.rdsSig || cb_.rdsExt);
            lprBuf_.assign(demodBuf_.begin(), demodBuf_.begin() + nc);   // L+R = MPX
            lmrBuf_.resize(nc);
            if (wantRds) { ref57Buf_.resize(nc); ref57qBuf_.resize(nc); bitClkBuf_.resize(nc); }
            pll_.processBlock(demodBuf_.data(), nc, lmrBuf_.data(),
                              wantRds ? ref57Buf_.data()  : nullptr,
                              wantRds ? ref57qBuf_.data() : nullptr,
                              wantRds ? bitClkBuf_.data() : nullptr);
            // ── THE COMPOSITE EYE ───────────────────────────────────────────────────────
            // ★★★ THE PILOT PLL IS THE TRIGGER. A scope needs an edge detector and its
            //     trigger jitters; we already have the recovered pilot phase, so every sweep
            //     is aligned by construction. bitClk = (cycle*2pi + phase)/16, so multiplying
            //     back by 16 recovers a phase that runs continuously across cycles — no extra
            //     per-sample work in the PLL loop, which is the hottest loop in WFM.
            // ★ Nobody reading the scope ⇒ none of it runs — see setRdsExtWantedFlag.
            const bool rdsExtWanted = !rdsExtWantedFlag_ || rdsExtWantedFlag_->load(std::memory_order_relaxed);
            // ★★ TWO CYCLES, because the cycle counter wraps at 16 and 16 is divisible by 2.
            //    Three cycles would leave one sweep in sixteen starting at the wrong phase and
            //    smear the whole picture.
            if (wantRds && cb_.rdsExt && rdsExtWanted) {
                // ★★ 96 COLUMNS, FIXED. The grid was sized to the channel rate for one release
                //    and came out at 30 columns on every radio — see the note on eyeW_ for why
                //    the argument was wrong and what the wire measured.
                eyeW_ = kEyeWMax;
                for (int b = 0; b < kEyeBands; ++b) {
                    if ((int)eyeAcc_[b].size() != eyeW_ * kEyeH) {
                        eyeAcc_[b].assign((size_t)eyeW_ * kEyeH, 0.0f);
                        eyeOut_[b].assign((size_t)eyeW_ * kEyeH, 0);
                    }
                }
                // ★★ The three resonators, designed once per rate. Q from what each component
                //    actually occupies: the pilot is a tone, L-R carries the stereo sidebands
                //    (wide), RDS is +/-2.4 kHz about 57.
                if (eyeBandFs_ != chFs_) {
                    for (int sct = 0; sct < 2; ++sct) {
                        eyeBand_[0][sct].design(chFs_, 19000.0, 14.0);   // pilot — a tone
                        eyeBand_[1][sct].design(chFs_, 38000.0,  1.6);   // L-R sidebands — wide
                        eyeBand_[2][sct].design(chFs_, 57000.0,  9.0);   // RDS  — +/-2.4 kHz
                    }
                    eyeBandFs_ = chFs_;
                }
                /* ★★★ THE DEVIATION FILTERS, AND THE TWO NOISE INTEGRALS — see mpxLp_ and
                 *   devNoiseK_ in the header. Designed once per channel rate. K and G are the
                 *   noise power each filter passes from FM's triangular (∝ f²) noise, so the
                 *   guard band's measured power scales to the measurement band's by K/G with no
                 *   fitted number in it. The guard sits at 80 kHz (between the US SCA slots at
                 *   67 and 92) and needs the channel to be flat there, so it is only trusted on
                 *   a channel wider than 180 kHz — narrower, the reading goes uncorrected, which
                 *   errs on the side it always did. */
                if (mpxLpFs_ != chFs_) {
                    static const double kBw6[3] = { 0.51763809, 0.70710678, 1.93185165 };
                    for (int k = 0; k < 3; ++k) {
                        mpxLp_[k].designLp(chFs_, 66000.0, kBw6[k]);
                        mpxGuard_[k].design(chFs_, 80000.0, 12.0);
                    }
                    devNoiseK_ = 0.0f;
                    if (chFs_ > 180000.0) {
                        double K = 0.0, G = 0.0;
                        const int nGrid = 2048;
                        for (int g = 1; g < nGrid; ++g) {
                            const double f = 0.5 * chFs_ * g / nGrid, w = 2.0 * M_PI * f / chFs_;
                            double hl = 1.0, hg = 1.0;
                            for (int k = 0; k < 3; ++k) { hl *= mpxLp_[k].mag2(w); hg *= mpxGuard_[k].mag2(w); }
                            K += f * f * hl; G += f * f * hg;
                        }
                        devNoiseK_ = (G > 0.0) ? (float)(K / G) : 0.0f;
                    }
                    devWinN_ = (int)(chFs_ * 0.05);          // 50 ms, whatever the block size
                    devWinCnt_ = 0; devWinGp_ = 0.0; devHist_.assign(kDevHistN, 0u);
                    mpxLpFs_ = chFs_;
                }
                // ★ Maintenance runs at the send rate, not per block — see eyeSince_. The
                //   ACCUMULATION below is per block; only the display work is gated.
                eyeSince_ += nc;
                const bool eyeMaint = (chFs_ > 0.0) && (eyeSince_ >= chFs_ / 6.0);
                // ── High-pass above the audio (see the note on eyeHpA_) ─────────────────
                if (eyeHpA_ <= 0.0f && chFs_ > 0.0) {
                    eyeHpA_ = (float)(1.0 - std::exp(-2.0 * M_PI * 15000.0 / chFs_));
                    // |H| of three cascaded one-pole high-passes at each band centre, so the
                    // per-band kHz figures report the composite, not the filtered copy.
                    static const double kBandHz[3] = { 19000.0, 38000.0, 57000.0 };
                    for (int b = 0; b < kEyeBands; ++b) {
                        const double r = kBandHz[b] / 15000.0;
                        eyeHpGain_[b] = (float)std::pow(r / std::sqrt(1.0 + r * r), 3.0);
                    }
                }
                // ★ Same reasoning as the biquads: a one-pole that goes non-finite stays there.
                if (!std::isfinite(eyeHp1_) || !std::isfinite(eyeHp2_) || !std::isfinite(eyeHp3_))
                    eyeHp1_ = eyeHp2_ = eyeHp3_ = 0.0f;
                /* ★ THE AVERAGE JOINS THE NaN GUARD. It feeds the guard-band verdict and sizes the
                 *  noise removal for BOTH figures now, so a NaN here would take the peak with it —
                 *  a new state must be added to this list or it is a hole in it. */
                if (!std::isfinite(mpxDevSm_) || !std::isfinite(mpxDevAvg_) || !std::isfinite(mpxDevHold_)
                    || !std::isfinite(mpxNoiseSm_) || !std::isfinite(devWinGp_))
                    { mpxDevSm_ = 0.0f; mpxDevAvg_ = 0.0f; mpxDevHold_ = 0.0f; mpxNoiseSm_ = 0.0f; devWinGp_ = 0.0; }
                if ((int)devHist_.size() != kDevHistN) devHist_.assign(kDevHistN, 0u);
                if (!std::isfinite(eyePeak_)) eyePeak_ = 0.0f;
                // ★ AUTOSCALE, with a slow decay so it cannot pump on every bass note. A quiet
                //   passage genuinely shrinks the composite, and a fixed full scale would hide
                //   the structure instead of magnifying it (Stuart, 2026-09-13).
                // ★ The scale used for THIS block is last block's peak (after its decay) — one
                //   block of lag on a 0.995/block autoscale is nothing, and it is what lets the
                //   whole thing run as one pass. A sample above it lands on the top row.
                // ★★ DECAY IN TIME, NOT PER BLOCK. 0.995 per block was 0.1 s on an RTL's 168-sample
                //    blocks and many seconds on a bench feeding 8192 at a time — the same
                //    block-size trap the deviation window fell into. 0.5 s is the constant.
                // ★ 2 s, not 0.5: at 0.5 s the scale followed the music bar by bar and the whole
                //   trace visibly breathed with it (Stuart, 2026-09-14).
                const float pkDecay = (chFs_ > 0.0) ? (float)std::exp(-(double)nc / chFs_ / 2.0) : 1.0f;
                eyePeak_ *= pkDecay;
                float blockPk = eyePeak_;
                float bpk[3], binv[3];
                for (int b = 0; b < kEyeBands; ++b) {
                    if (!std::isfinite(eyeBandPk_[b])) eyeBandPk_[b] = 0.0f;
                    eyeBandPk_[b] *= pkDecay; bpk[b] = eyeBandPk_[b];
                    binv[b] = (eyeBandPk_[b] > 1e-6f) ? (1.0f / eyeBandPk_[b]) : 0.0f;
                }
                // ★★ NO fmod. A fractional part is floor-and-subtract; working in TURNS (0..1)
                //    rather than radians removes the division too. bitClk = (cycle*2pi+phase)/16,
                //    so bitClk * 16/(4pi) is the position in two-pilot-cycle units.
                const float kTurns = (float)(16.0 / (2.0 * 2.0 * M_PI));
                const float halfH  = 0.5f * (float)kEyeH;
                float* acc0 = eyeAcc_[0].data(); float* acc1 = eyeAcc_[1].data(); float* acc2 = eyeAcc_[2].data();
                double devGp = devWinGp_;
                uint32_t* hist = devHist_.data();
                const float kHistScale = (float)kDevHistN / 1.28f;
                /* ★★★ ONE PASS. This was five sweeps over the block — high-pass into a copy,
                 *   three band-passes into three more copies, a peak scan, the deviation
                 *   low-pass, then the fold reading them all back. Every one of those is a serial
                 *   IIR, and the earlier NEON/SSE attempt proved four lanes cannot help a chain
                 *   like that (measured 0.96x — see the memory of 2026-09-13). What DOES help is
                 *   letting the three independent band chains, the deviation chain and the guard
                 *   chain sit in one loop body, where the core's out-of-order window overlaps
                 *   them for free instead of finishing one chain before starting the next.
                 * ★★★ BILINEAR SPLAT. Each sample lands at an exact (x, y) inside the grid and is
                 *   shared between the four cells around it by distance — that is the sub-cell
                 *   information the old nearest-cell deposit threw away, and it is what makes 96
                 *   columns draw as a smooth trace rather than a staircase. x wraps (the sweep is
                 *   periodic in the pilot), y clamps. */
                for (int i = 0; i < nc; ++i) {
                    const float x = demodBuf_[i];
                    eyeHp1_ += eyeHpA_ * (x - eyeHp1_);       const float h1 = x - eyeHp1_;
                    eyeHp2_ += eyeHpA_ * (h1 - eyeHp2_);      const float h2 = h1 - eyeHp2_;
                    eyeHp3_ += eyeHpA_ * (h2 - eyeHp3_);      const float h  = h2 - eyeHp3_;
                    const float y0 = eyeBand_[0][1].step(eyeBand_[0][0].step(h));
                    const float y1 = eyeBand_[1][1].step(eyeBand_[1][0].step(h));
                    const float y2 = eyeBand_[2][1].step(eyeBand_[2][0].step(h));
                    /* ★★★ THE SCALE COMES FROM WHAT IS DRAWN — the sum of the three band outputs —
                     *   not from the raw high-passed composite. That peak included every bit of
                     *   broadband noise above 15 kHz, so on a noisy station (Flex FM at 26 dB MPX
                     *   S/N) it read ±28 kHz while the pilot was 6.6: three rows of 48, and
                     *   Stuart saw "basically a straight line". On a clean station the two agree
                     *   and nothing changes. */
                    const float ah = std::fabs(y0 + y1 + y2);
                    if (ah > blockPk) blockPk = ah;
                    const float a0 = std::fabs(y0), a1 = std::fabs(y1), a2 = std::fabs(y2);
                    if (a0 > bpk[0]) bpk[0] = a0;  if (a1 > bpk[1]) bpk[1] = a1;  if (a2 > bpk[2]) bpk[2] = a2;
                    // Each band against ITS OWN peak — see eyeBandPk_. 0.92 keeps the crest inside the box.
                    /* ★★★ ONE AXIS, SCALED BY THE PILOT. Three scalings were tried today: the
                     *   composite's own peak (a loud stereo passage squashed the pilot to a white
                     *   line), and each band to its own peak (RDS, a filled eye by nature, strobed
                     *   the box — "a disco light"). What Stuart pointed at as the aim was the
                     *   2026-09-13 BBC Northampton shot, which by accident was pilot-scaled: the
                     *   pilot fills three quarters of the box, stereo draws at its TRUE size against
                     *   it and clips at the edges when loud (which is information), RDS stays the
                     *   small braid it really is. Every station has a pilot at ~6.75 kHz, so the
                     *   scale barely moves and nothing breathes. */
                    /* ★★★ AND THEN THE MUSIC STATION: Flex FM at 35 kHz of stereo on a pilot-sized
                     *   axis clipped everywhere and filled the box white. The reference shot was
                     *   SPEECH. So: each band on its own scale, at a FIXED share of the box — pilot
                     *   0.75 (the wave), stereo 0.55 (a braid, never a wall), RDS 0.35 (a small
                     *   braid) — with the reference's brightness rule keeping the spread bands dim
                     *   under the pilot line. Heights no longer compare amplitudes; the caption's
                     *   three kHz figures do. */
                    // ★ Three boxes now (web): each band fills its OWN box on its own scale.
                    const float u0 = y0 * binv[0] * 0.85f, u1 = y1 * binv[1] * 0.85f, u2 = y2 * binv[2] * 0.85f;
                    // Deviation: the whole composite, audio included, through the 66 kHz cascade.
                    const float d = mpxLp_[2].step(mpxLp_[1].step(mpxLp_[0].step(x)));
                    // ★ UNSIGNED compare: a NaN casts to INT_MIN, and "hb >= N" would let it through
                    //   to write kilobytes below the histogram. The step() guards return 0 for a
                    //   non-finite output, but the index must not trust that alone.
                    unsigned hb = (unsigned)(int)(std::fabs(d) * kHistScale);
                    if (hb >= (unsigned)kDevHistN) hb = kDevHistN - 1;
                    ++hist[hb];
                    const float g = mpxGuard_[2].step(mpxGuard_[1].step(mpxGuard_[0].step(x)));
                    devGp += (double)g * g;
                    // The fold — one x for all three bands: they share the trigger.
                    const float t = bitClkBuf_[i] * kTurns;
                    /* ★★★ A NaN HERE SEGFAULTED THE LENOVO's RSP CHILD (dev 5.6.0, 2026-09-14). The
                     *   old nearest-cell fold clamped its index, which happened to swallow a NaN
                     *   pilot clock (it casts to INT_MIN, and "< 0 → 0" caught it). The splat's
                     *   wrap arithmetic did not, and INT_MIN + 96 is still a write a long way
                     *   below the grid. The PLL can hand out a NaN clock briefly after a mode
                     *   change (here AM 648 kHz → WFM 96.6). Skip the sample; never index on it. */
                    if (!std::isfinite(t)) continue;
                    /* ★★★ ONE CELL PER HIT, NOT A BILINEAR SPLAT. The splat was tried today and it is
                     *   what made the traces fat: every hit spread over four cells, then bilinear
                     *   upscaling softened them again, and the pilot came out as a thick blurred
                     *   ribbon. The XCover's app eye — one cell per hit at 96x48 — is the look
                     *   Stuart pointed at: "this older look is the aim". Crisp beats smooth here. */
                    int cx = (int)((t - std::floor(t)) * (float)eyeW_);
                    if (cx < 0) cx = 0; else if (cx >= eyeW_) cx = eyeW_ - 1;
                    auto deposit = [&](float* acc, float u) {
                        // Row 0 is the TOP, so +full scale is at the top like a scope.
                        float fy = (1.0f - u) * halfH;
                        if (!(fy >= 0.0f)) fy = 0.0f;                       // NaN lands on row 0
                        int cy = (int)fy; if (cy >= kEyeH) cy = kEyeH - 1;
                        acc[(size_t)cy * eyeW_ + cx] += 1.0f;
                    };
                    deposit(acc0, u0); deposit(acc1, u1); deposit(acc2, u2);
                }
                eyePeak_ = blockPk;
                for (int b = 0; b < kEyeBands; ++b) eyeBandPk_[b] = bpk[b];
                devWinGp_ = devGp; devWinCnt_ += nc;
                /* ★★ THE 50 ms WINDOW CLOSES — see devWinN_. The bar is the AVERAGE of window
                 *  maxima on the panel's 1.5 s clock (Stuart: "average it the same as the other
                 *  measurements"); the tick is a slow peak-hold of the same corrected value. */
                if (devWinN_ > 0 && devWinCnt_ >= devWinN_) {
                    const double dtW = (double)devWinCnt_ / chFs_;
                    // The percentile — walk down from the top until `skip` samples have been
                    // passed. The bin's UPPER edge, so a clean tone is not read low.
                    float pk = 0.0f;
                    {
                        /* ★★★ A FIXED COUNT, NOT A FRACTION OF THE WINDOW. This was
                         *  `devWinCnt_ * 0.0003` — 0.3 per mille, which is ~5 samples at 160 kHz
                         *  but grows with the channel rate, so at 250 kHz it discarded the top
                         *  ~4 samples and at higher rates more again. A SUSTAINED TONE does not
                         *  care: it puts hundreds of samples in the top bin every window. A
                         *  SPARSE TRANSIENT — one orchestral attack, a consonant — is only a few
                         *  samples wide and was thrown away WHOLE, and the quieter the programme
                         *  the larger the share of its peaks that are sparse. That is part of
                         *  Onfliner's under-read (2026-09-25), and it is the half that hides
                         *  inside the rate.
                         *  ★ Two samples still rejects a lone impulse (the reason the skip
                         *    exists), and the 66 kHz band-limit plus the guard-band correction
                         *    are the other two defences. The bench figures that justified 0.3 per
                         *    mille were taken on a spiky multi-tone, not on real programme.
                         *  ✗ Do not restore a proportional skip: it makes the reading depend on
                         *    the sample rate, which is exactly what §3 of the brief proved this
                         *    measurement is otherwise free of. */
                        const uint32_t skip = 2;
                        uint32_t seen = 0; int b = kDevHistN - 1;
                        for (; b > 0; --b) { seen += hist[b]; if (seen > skip) break; }
                        pk = (float)(b + 1) / kHistScale;
                        devHist_.assign(kDevHistN, 0u);
                    }
                    float gp = (float)(devWinGp_ / (double)devWinCnt_);
                    devWinCnt_ = 0; devWinGp_ = 0.0;
                    /* ★★ IGNORE THE FIRST 0.4 s AFTER A RETUNE — see the note at the reconfigure.
                     *  ★★★ WAS 1.5 s, AND THAT WAS SIZED FOR THE OLD SLOW METER. With an
                     *  instant-attack peak the reading is meaningful as soon as the DC blocker and
                     *  the PLL have settled, so a 1.5 s blank is now just 1.5 s of the meter
                     *  saying nothing after every tune — on top of the attack, it was ~5-6 s
                     *  before the number meant anything (Onfliner: "the slow display of the
                     *  deviation scale"). */
                    if (mpxDevSettle_ < 0.4) { mpxDevSettle_ += dtW; pk = 0.0f; gp = 0.0f;
                                               mpxDevSm_ = 0.0f; mpxDevAvg_ = 0.0f;
                                               mpxDevHold_ = 0.0f; mpxNoiseSm_ = 0.0f; }
                    const float aSm = 1.0f - (float)std::exp(-dtW / 1.5);
                    /* ★★★ TWO STATISTICS FROM ONE WINDOW ARRAY — THE PEAK AND THE AVERAGE.
                     *
                     *  ★★★ WHAT WAS WRONG. This line used to be the ONLY one, and it published the
                     *  1.5 s SYMMETRIC AVERAGE of the window peaks while calling it peak deviation.
                     *  On processed programme (crest factor 2-4 dB) nearly every 50 ms window peaks
                     *  at the same value, so average ≈ peak and we read within 1 kHz of MPX Tool —
                     *  which is exactly what tgcfabian measured. On jazz, classical and speech
                     *  (crest 10-20 dB) most windows contain no peak at all, so a 75 kHz peak read
                     *  ≈ 38 — which is exactly what Onfliner measured. ★ TWO TESTERS CONTRADICTED
                     *  EACH OTHER AND BOTH WERE RIGHT; only the averaging predicts both reports,
                     *  and Onfliner himself confirmed it by adding that "noisy stations playing
                     *  loud music it was spot on" (2026-09-25).
                     *
                     *  ★★★ AND THE HEADER ABOVE THIS FIELD SAID IT WAS ALREADY A PEAK METER —
                     *  vibedsp.h claimed "FAST ATTACK, SLOW DECAY … rises INSTANTLY to a new peak"
                     *  for weeks after this became an average. One rule, two readers, and the
                     *  stale reader was the DESIGN NOTE, so auditing the source said the meter was
                     *  fine and it took an outside tester with a reference instrument to find it.
                     *  That note is now true again.
                     *
                     *  ★★ NOTHING IS LOST: the average is still computed and still sent, because
                     *  it is the steady number Stuart asked for ("average it the same as the other
                     *  measurements", 2026-09-13) and the one tgcfabian validated. It is
                     *  relabelled, not removed. PIRA's analysers do the same thing from a 50 ms
                     *  window identical to ours — they publish MAX, AVE and MIN; we published the
                     *  AVE alone and labelled it "deviation". */
                    mpxDevAvg_ += aSm * (pk - mpxDevAvg_);
                    /* ★★★ THE PEAK: INSTANT ATTACK, ~0.9 s DECAY. This is what a modulation
                     *  monitor is. A peak that arrives is published immediately; between peaks it
                     *  falls slowly enough that the bar reads as a level rather than a flicker.
                     *  ★ THE DIGITS DO NOT FOLLOW THIS — see mpxDevHold_ below. Stuart, 2026-09-25:
                     *    "if it is bouncing up and down like a yoyo then the number looks like a
                     *    stopwatch, how do you read that?" He is right, and the answer is that the
                     *    NUMBER must not be the fast thing: the bar moves, the digits hold. */
                    const float kPk = (float)std::exp(-dtW / 0.9);
                    mpxDevSm_ = (pk > mpxDevSm_) ? pk : mpxDevSm_ * kPk;
                    mpxNoiseSm_ += aSm * (gp - mpxNoiseSm_);
                    // σ² in the measurement band, then the quadrature removal — see devNoiseK_.
                    const float sig2 = mpxNoiseSm_ * devNoiseK_;
                    const float kC = 4.5f;   // fitted on the bench (bench_eye), see devNoiseK_
                    /* ★★★ A NEIGHBOUR IN THE GUARD BAND IS NOT NOISE. The guard sits at 80 kHz,
                     *  which is where an adjacent station 100 kHz away puts its sidebands. Classic
                     *  FM on 100.4 beside a strong 100.3 (Stuart, 2026-09-14): the "noise" read
                     *  7 kHz, the removal took 31 kHz off a 32 kHz raw peak, and the meter showed
                     *  8 kHz falling to nothing while every other readout said the transmitter
                     *  was fine. Real broadband noise never removes most of the reading on a
                     *  station whose pilot and RDS are locked — so if the removal would take more
                     *  than 60 % of the raw figure, the guard band is occupied: subtract nothing,
                     *  and report the noise as NEGATIVE so the panel can say why. */
                    const float removal = kC * std::sqrt(std::max(0.0f, sig2));
                    /* ★★★ THE CORRECTION IS JUDGED AND SIZED ON THE AVERAGE, THEN APPLIED TO BOTH.
                     *
                     *  ★★★ WHY NOT JUST RUN THE SAME FORMULA ON THE PEAK. `kC = 4.5` is a bench fit
                     *  (bench_eye) made against the AVERAGED statistic, and it is a quadrature
                     *  removal — the right shape for a quantity that behaves like an rms. An
                     *  instant-attack peak does not: the noise contribution to a single window
                     *  maximum is not σ-like, so 4.5σ subtracted from a peak is a number with no
                     *  derivation behind it. Getting that wrong on a weak signal is precisely the
                     *  "106 kHz peak, OVERMODULATED" failure the band-limit was added to prevent
                     *  (see the note on mpxDevSettle_ in vibedsp.h) — the fast attack is what makes
                     *  that failure reachable again.
                     *  ★★ So: correct the average with the fit that was made for it, take the
                     *  ABSOLUTE kHz that removed, and subtract the same absolute amount from the
                     *  peak. The noise floor under a peak and under an average of peaks is the
                     *  same noise floor; what is not justified is re-deriving its size from a
                     *  statistic the constant was never fitted against.
                     *  ▶ Re-fitting kC for a true peak on the bench is the proper answer and is
                     *    written up in briefs/BRIEF-deviation-meter.md. Until that is measured,
                     *    this is the honest approximation, and it errs towards removing too
                     *    little — which shows a reading as noisy rather than inventing silence.
                     *  ★ The guard-band test also stays on the average, because it is the stable
                     *    quantity: judging "is a neighbour sitting in my guard band?" off a value
                     *    that moves with every syllable would make the verdict flicker. */
                    const bool guardOccupied = mpxDevAvg_ > 0.02f && removal > 0.6f * mpxDevAvg_;
                    if (guardOccupied) {
                        mpxDevNoise_ = -std::sqrt(std::max(0.0f, sig2));
                        mpxDevOut_    = mpxDevSm_;
                        mpxDevAvgOut_ = mpxDevAvg_;
                    } else {
                        mpxDevNoise_ = std::sqrt(std::max(0.0f, sig2));
                        const float s2 = mpxDevAvg_ * mpxDevAvg_ - kC * kC * sig2;
                        mpxDevAvgOut_ = (s2 > 0.0f) ? std::sqrt(s2) : 0.0f;
                        const float removedAbs = mpxDevAvg_ - mpxDevAvgOut_;   // ≥ 0 by construction
                        mpxDevOut_ = std::max(0.0f, mpxDevSm_ - removedAbs);
                    }
                    /* ★★★ THE HOLD IS NOW THE FIGURE THE DIGITS SHOW, not a thin tick nobody reads.
                     *  Instant attack, 6 s decay: it sits still long enough to be read and then
                     *  steps down. This is the half of Stuart's objection that the fast bar cannot
                     *  answer on its own, and it is what MPX Tool's peak flasher gives you.
                     *  ★ It holds the CORRECTED peak, so it can never sit at a figure the bar has
                     *    not actually reached. */
                    const float kHold = (float)std::exp(-dtW / 6.0);
                    mpxDevHold_ = (mpxDevOut_ > mpxDevHold_) ? mpxDevOut_ : mpxDevHold_ * kHold;
                }
                /* ★★★ CONVERT WHAT WAS ACCUMULATED, *THEN* DECAY — order matters, and getting it
                 *   wrong is invisible in code review (it shipped the other way round and the
                 *   plot flickered; Stuart, 2026-09-13). */
                if (eyeMaint) {
                    eyeSince_ = 0.0;
                    /* ★★★ EACH BAND ON ITS OWN BRIGHTNESS. A shared intensity scale let the pilot
                     *   — a pure tone that lands in the same cells on every one of ~14000 sweeps —
                     *   set the scale for everything, and the stereo, spread across its envelope,
                     *   came out at a mean of 3.5/63 with fewer than one cell in 1440 changing
                     *   per frame (measured from the Pi's wire, 2026-09-14). That is the plot
                     *   Stuart described as not responding. A 4x lift cap had been tried; it was
                     *   not enough by a factor of five.
                     * ★★ STRENGTH IS STILL HONEST, because the VERTICAL scale stays shared: a weak
                     *   component draws small, and a DEAD one draws as a flat bright line at zero
                     *   (its noise is tiny against the shared axis) — which is exactly what
                     *   "nothing there" should look like. Scatter still reads as fuzz. */
                    /* ★★ THE GEOMETRIC MEAN OF ITS OWN MAXIMUM AND THE SHARED ONE. Fully own-scaled
                     *   (tried first today) every band saturated and the three added to white — the
                     *   RDS, a spread band, drew as fat violet blobs over the pilot (Stuart: "the
                     *   individual components are blending into each other too much"). Fully shared
                     *   (5.5.5) hid weak stereo. sqrt(bmx·mx) lifts a band 25x below the leader by
                     *   5x and one 100x below by 10x: a concentrated tone still reads brighter than
                     *   a spread band, which is the "strong line vs speckle" reading the colours
                     *   exist for, and nothing vanishes. */
                    float mx = 0.0f;
                    for (int b = 0; b < kEyeBands; ++b)
                        for (float v : eyeAcc_[b]) if (v > mx) mx = v;
                    for (int b = 0; b < kEyeBands; ++b) {
                        float bmxNow = 0.0f;
                        for (float v : eyeAcc_[b]) if (v > bmxNow) bmxNow = v;
                        /* ★ The brightness reference is SMOOTHED (~1 s at 6 Hz), not the instant
                         *   maximum: normalising each frame to its own peak made the stereo band
                         *   pulse with its own loudness — "almost breathes, fades in and out"
                         *   (Stuart, 2026-09-14). Rises are taken faster than falls so a band that
                         *   appears is not clipped while the reference catches up. */
                        float& bmx = eyeBmxSm_[b];
                        bmx += ((bmxNow > bmx) ? 0.35f : 0.15f) * (bmxNow - bmx);
                        /* ★ STRENGTH IN THE BRIGHTNESS: the band's true peak against the strongest
                         *   band's, square-rooted so a component at a tenth still draws at a third,
                         *   floored at 0.3 so nothing readable disappears. Vertical size no longer
                         *   carries strength; this does. */
                        /* Against its own NOMINAL, not the loudest band: judged against 29 kHz of
                         *  stereo a 4.4 kHz pilot looked weak when it was healthy. Nominals in
                         *  composite kHz: pilot 6.75, stereo 25 (typical peak L−R), RDS 2.5. RDS is
                         *  capped at 0.6 so it stays a texture behind the two waves. */
                        // The 5.5.5 brightness rule the reference shot was drawn with: each band on
                        // its own maximum, lifted at most 4x above the shared one.
                        float mxAll = 1e-6f;
                        for (int k = 0; k < kEyeBands; ++k) mxAll = std::max(mxAll, eyeBmxSm_[k]);
                        const float strength = std::min(1.0f, 4.0f * bmx / mxAll);
                        // ★ bmx^0.75 · mx^0.25: the geometric mean left Heart's stereo at 36/63 —
                        //   persistent on the wire, but on a retina Safari faint enough that
                        //   Stuart saw it only when a chorus pushed it to full ("flashes for a
                        //   split second, no persistence"). Three-quarters own scale keeps the
                        //   tone-vs-spread ordering while a band 3x below the leader draws at ~48.
                        const float es = (bmx > 1e-6f) ? (255.0f * strength / bmx) : 0.0f;
                        for (size_t j = 0; j < eyeAcc_[b].size(); ++j) {
                            const int v = (int)(eyeAcc_[b][j] * es);
                            eyeOut_[b][j] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
                        }
                    }
                    /* ★★★ PERSISTENCE ON THE SAME CLOCK AS THE REST OF THE PANEL: 0.889 at 6 Hz
                     *   is ~1.5 s, matching pilotDev, rdsDev, coherence and drift
                     *   ([[panel_readouts_need_one_clock]]). Fade AFTER publishing. */
                    for (int b = 0; b < kEyeBands; ++b)
                        for (float& v : eyeAcc_[b]) v *= 0.889f;
                }
            }

            // RDS (only meaningful once the pilot is locked).
            rdsDemod_.setPilotRef(pll_.lockAmp());
            if (wantRds && cb_.rdsBer)
                cb_.rdsBer(cb_.ctx, pll_.trackable() ? rdsDemod_.blockErrorPercent() : -1);
            if (wantRds && cb_.rdsSig)
                cb_.rdsSig(cb_.ctx, rdsDemod_.subcarrierRelDb());
            // ★ The MPX spectrum, computed only when the Advanced RDS panel is watching.
            // demodBuf_ IS the MPX — the same buffer the stereo and RDS decoders read — so
            // this costs one FFT and no new signal path.
            if (wantRds && cb_.rdsExt && rdsExtWanted) {
                if (!mpxFft_) {
                    mpxFft_ = std::make_unique<RealFFT>(kMpxFft);
                    mpxWin_.resize(kMpxFft);
                    nuttallWindow(mpxWin_.data(), kMpxFft);
                    mpxIn_.resize(kMpxFft);
                    mpxDb_.resize(kMpxFft / 2 + 1);
                    mpxOut_.assign(kMpxBins, -120.0f);
                    mpxAcc_.assign(kMpxFft, 0.0f);
                    mpxAccN_ = 0;
                }
                // ★ ACCUMULATE ACROSS BLOCKS. The first version required a single block of at
                // least kMpxFft samples — and the DSP block is smaller than that, so the FFT
                // never ran and the panel drew an empty box with labels on it (Stuart,
                // 2026-07-26). ★ Never gate work on a block size you do not control.
                for (int i = 0; i < nc && mpxAccN_ < kMpxFft; ++i)
                    mpxAcc_[mpxAccN_++] = demodBuf_[i];
                if (mpxAccN_ >= kMpxFft) {
                mpxAccN_ = 0;
                for (int i = 0; i < kMpxFft; ++i) mpxIn_[i] = mpxAcc_[i] * mpxWin_[i];
                mpxFft_->powerDb(mpxIn_.data(), mpxDb_.data(), 2.0f / kMpxFft);
                // Map DC..kMpxSpanHz onto kMpxBins, taking the PEAK of each group: the pilot
                // and RDS are narrow, and averaging would flatten the very features this
                // display exists to show.
                const double binHz = chFs_ / kMpxFft;
                const int hi = (int)std::min<double>(kMpxFft / 2.0, kMpxSpanHz / binHz);
                for (int i = 0; i < kMpxBins; ++i) {
                    const int a = (int)((double)i / kMpxBins * hi);
                    const int b = std::max(a + 1, (int)((double)(i + 1) / kMpxBins * hi));
                    float m = -200.0f;
                    for (int k = a; k < b && k < (int)mpxDb_.size(); ++k)
                        if (mpxDb_[k] > m) m = mpxDb_[k];
                    mpxOut_[i] = m;
                }
                }
            }
            if (wantRds && cb_.rdsExt && rdsExtWanted) {
                const RdsDecoder* d = rdsDemod_.best();
                float xy[RdsDemod::kConstPts * 2];
                const int np = rdsDemod_.constellation(xy, RdsDemod::kConstPts);
                int af[RdsDecoder::kMaxAf]; int afSeen = 0;
                // ★ mergedAf() also refreshes the sticky aggregate, so it must run first.
                const int nAf = rdsDemod_.mergedAf(af, RdsDecoder::kMaxAf, &afSeen);
                static RdsDecoder::Eon eons[RdsDecoder::kMaxEon];
                static RdsDecoder::Oda odas[RdsDecoder::kMaxOda];
                const int nEon = rdsDemod_.mergedEon(eons, RdsDecoder::kMaxEon);
                const int nOda = rdsDemod_.mergedOda(odas, RdsDecoder::kMaxOda);
                // ★★ EVERYTHING BELOW COMES FROM THE STICKY AGGREGATE, never straight from
                // the winning hypothesis — otherwise every field blinks out whenever
                // arbitration changes its mind, which is precisely the fault that made the
                // AF list appear and disappear (Stuart, 2026-07-26).
                const RdsDemod::Agg& a = rdsDemod_.aggregate();
                Callbacks::RdsExt x{};
                x.pty = a.pty; x.tp = a.tp; x.ta = a.ta; x.ms = a.ms; x.di = a.di;
                // ★ RAW comes straight from the WINNING hypothesis, deliberately bypassing the
                // sticky aggregate: "what is arriving right now", not "what we have ever known".
                // No winner yet = nothing is arriving, which -1 says honestly.
                if (const RdsDecoder* w = rdsDemod_.best()) {
                    x.ptyRaw = w->ptyRaw(); x.tpRaw = w->tpRaw();
                    x.taRaw  = w->taRaw();  x.msRaw = w->msRaw(); x.diRaw = w->diRaw();
                } else {
                    x.ptyRaw = x.tpRaw = x.taRaw = x.msRaw = x.diRaw = -1;
                }
                x.ctMinutes = a.ctMinutes;
                x.ctOffsetHalfHours = a.ctOffsetHalfHours;
                x.afKhz = af; x.nAf = nAf; x.afSeen = afSeen;
                // ★ ...and the full list with per-frequency confirmation. Taken AFTER mergedAf(),
                //   which is the call that rebuilds it.
                x.nAfAll = rdsDemod_.allAf(&x.afAllKhz, &x.afAllOk);
                x.groupCounts = a.groupCounts; x.groupTotal = a.groupTotal;
                x.rtpTitle = a.rtpTitle; x.rtpArtist = a.rtpArtist; x.longPs = a.longPs;
                x.ptyn = a.ptyn; x.language = a.language;
                x.pinDay = a.pinDay; x.pinHour = a.pinHour; x.pinMinute = a.pinMinute;
                x.eon = eons; x.nEon = nEon;
                x.oda = odas; x.nOda = nOda;
                x.constXY = xy; x.nPts = np;
                /* ★★★ ONE OBSERVATION WINDOW FOR THE WHOLE PANEL. These fields are assembled in
                 *     the same instant, but each arrived carrying a COMPLETELY DIFFERENT
                 *     averaging interval: the RDS deviation smoothed over seconds, the pilot
                 *     deviation essentially instantaneous, the block error rate over the
                 *     decoder's own window, the constellation over its last N symbols. A frame
                 *     was therefore a mosaic of a millisecond, a second and several seconds
                 *     presented as one moment — so no two numbers on it were commensurable, and
                 *     any contradiction between them was expected rather than diagnostic.
                 *  ★ Stuart, after an evening of comparing four radios on six stations:
                 *    "those numbers change so rapidly sub 1 second", "I think the issue is the
                 *    lack of smoothing those numbers show the data the millisecond it arrives but
                 *    ends up out of sync". He is right, and it invalidated most of a night's
                 *    conclusions — mine especially: I read single frames of jittering quantities
                 *    as measurements and built three theories on them, two of which were wrong.
                 *  ★★ A PANEL IS AN INSTRUMENT, AND AN INSTRUMENT NEEDS A STATED INTEGRATION TIME.
                 *    1.5 s, applied to every scalar here, so a frame describes one interval. The
                 *    coefficient is derived from the real block duration rather than assumed, so
                 *    the window is 1.5 seconds at any sample rate or block size.
                 *  ★★★ Pilot deviation and RDS deviation are TRANSMITTER CONSTANTS — they should
                 *      not move at all, and watching them jitter was the clue that none of this
                 *      was measuring what it claimed to. */
                {
                    const double dt  = (chFs_ > 0.0) ? (double)nc / chFs_ : 0.0;
                    const float  a   = (dt > 0.0) ? (float)(1.0 - std::exp(-dt / 1.5)) : 0.1f;
                    const float  pd  = pll_.pilotDeviationKHz();
                    const float  rd  = rdsDemod_.rdsDeviationKHz();
                    const float  coh = rdsDemod_.pilotPhaseCoherence();
                    const float  drf = rdsDemod_.pilotPhaseDriftDegPerSec();
                    if (!extAvgInit_) {
                        extAvgInit_ = true;
                        extPilotDev_ = pd; extRdsDev_ = rd; extCoh_ = coh; extDrift_ = drf;
                        extRdsBad_ = 0;
                    } else {
                        extPilotDev_ += a * (pd  - extPilotDev_);
                        extCoh_      += a * (coh - extCoh_);
                        extDrift_    += a * (drf - extDrift_);
                        /* ★★★ THE "CANNOT MEASURE" SENTINEL MUST EARN ITS PLACE. The deviation
                         *     carries a negative sentinel, and averaging a sentinel with a value
                         *     produces a number that is neither — so it cannot simply be mixed in.
                         *     But adopting it the instant it appears is just as wrong: a single
                         *     bad tick then blanks a figure that is otherwise perfectly healthy,
                         *     which is the whole fault being fixed here.
                         *  ★ Stuart on tgcfabian's FelineFM — 0 % block errors, carrier locked, full
                         *    radiotext, and the deviation showing "no subcarrier": "tgcfabian's
                         *    FelineFM has flashed 3.4KHz typical". The subcarrier was there all
                         *    along at a healthy 3.4 kHz; the panel was catching the dips.
                         *  ★★ So a real reading is adopted at once (it proves measurability), and
                         *    "cannot measure" only after it has held for about a second. */
                        if (rd >= 0.0f) {
                            extRdsBad_ = 0;
                            if (extRdsDev_ < 0.0f) extRdsDev_ = rd;          // first good reading
                            else                   extRdsDev_ += a * (rd - extRdsDev_);
                        } else if (++extRdsBad_ * dt > 1.0) {
                            extRdsDev_ = rd;                                  // genuinely gone
                        }
                    }
                }
                x.pilotPhaseDeg = rdsDemod_.pilotPhaseDeg();
                x.pilotPhaseCoherence = extCoh_;
                x.pilotPhaseDriftDegPerSec = extDrift_;
                x.pilotDevKHz = extPilotDev_;
                x.mpx = mpxOut_.empty() ? nullptr : mpxOut_.data();
                x.nMpx = (int)mpxOut_.size();
                // ★ The same 75 kHz convention as pilotDeviationKHz(), so the eye's scale
                //   readout is comparable with the pilot and RDS deviation figures beside it.
                const bool haveEye = !eyeOut_[0].empty();
                for (int b = 0; b < kEyeBands; ++b)
                    x.eyeBand[b] = haveEye ? eyeOut_[b].data() : nullptr;
                x.eyeW = haveEye ? eyeW_ : 0;
                x.eyeH = haveEye ? kEyeH : 0;
                x.eyeDevKHz = eyePeak_ * 75.0f;
                for (int b = 0; b < kEyeBands; ++b) x.eyeBandKHz[b] = eyeBandPk_[b] / eyeHpGain_[b] * 75.0f;
                // Full scale is the pilot's peak over 0.75 — the shared axis the plot is drawn on.
                x.eyeDevKHz = (eyeBandPk_[0] / 0.75f) / eyeHpGain_[0] * 75.0f;
                x.mpxDevKHz     = mpxDevOut_  * 75.0f;
                x.mpxDevAvgKHz  = mpxDevAvgOut_ * 75.0f;
                x.mpxDevNoiseKHz = mpxDevNoise_ * 75.0f;
                x.mpxDevHoldKHz = mpxDevHold_ * 75.0f;
                x.rdsDevKHz   = extRdsDev_;
                cb_.rdsExt(cb_.ctx, x);
            }
            if (wantRds && pll_.trackable())
                rdsDemod_.process(demodBuf_.data(), ref57Buf_.data(), ref57qBuf_.data(),
                                  bitClkBuf_.data(), nc);
            leftBuf_.resize(audioLpf_->maxOut(nc));
            rightBuf_.resize(lmrLpf_->maxOut(nc));
            trace_("pll_lmr", lmrBuf_.data(), nc);
            // ★ The noise meter now runs ABOVE THE BRANCH (it feeds the mono high-cut too). It ran
            //   here as well for one revision, so every block was fed to it TWICE — which is not a
            //   harmless duplicate: the filter integrates, and the asymmetric smoothing ran twice
            //   per block, so a clean signal measured 26 dB instead of 34 and the blend engaged on
            //   a perfect station. Caught by the clean-signal assertion, which is exactly the one
            //   written to catch this class of thing.
            const int n1 = audioLpf_->process(lprBuf_.data(), nc, leftBuf_.data()); // L+R
            const int n2 = lmrLpf_->process(lmrBuf_.data(),  nc, rightBuf_.data()); // L-R
            const int nm = std::min(n1, n2);
            trace_("lpf_lpr", leftBuf_.data(), nm);
            trace_("lpf_lmr", rightBuf_.data(), nm);
            // Stereo BLEND (anti-screech): fade the L-R in/out by a smoothed
            // pilot-lock confidence rather than hard-switching. forceMono or no
            // lock -> target 0 (clean mono); solid lock -> 1. The per-sample ramp
            // (~ a few ms) stops the harsh on/off when an edge signal flickers.
            const bool wantStereo = stereoEnabled_.load();
            // Target full stereo when the pilot is locked (locked() has hysteresis
            // so it won't chatter on an edge signal), else mono. The ramp does the
            // smoothing so the transition fades instead of screeching.
            const float target = (wantStereo && pll_.locked()) ? 1.0f : 0.0f;

            // ★★★ HIGH-BLEND — roll the TOP off L-R in proportion to how noisy the signal is.
            //     The measurement is the pilot (a fixed-injection reference the PLL recovers even
            //     in noise) against the 15-19 kHz guard band (transmitted silence, so anything
            //     there is noise). Their ratio is a real signal-to-noise figure; pilot amplitude
            //     ALONE is not, which is why a hissy S8 station still reads a nominal pilot
            //     deviation and why blending on that would have done nothing at all.
            // ★★ Only the HIGHS go. Bass and mid separation survive, so the station still sounds
            //    stereo instead of just narrow — see the note on lmrHiCutHz_.
            // ★ blendSnrDb_ is computed ABOVE THE BRANCH now — once, for both paths — because the
            //   mono high-cut needs the same figure. Smoothing it in two places would have made
            //   the time constant depend on which path was running, which is the sort of thing
            //   that is invisible until a station sounds different in mono for no stated reason.
            /* ★★★ EITHER SWITCH EARNS THIS BRANCH, NOT NR ALONE. NR treats noise and IMS treats
             *     a reflection; requiring weakProcOn_ here would have made IMS invisible whenever
             *     NR was off, which is precisely the A/B a listener reaches for when deciding
             *     what each control does. Each contributes its own target below and neither is
             *     allowed to act on the other's behalf. */
            const bool wspOn_ = weakProcOn_.load(std::memory_order_relaxed);
            const bool imsNow_ = imsOn_.load(std::memory_order_relaxed);
            if (mpxNoise_.ready() && wantStereo && pll_.locked() && (wspOn_ || imsNow_)) {
                // ★★ THE CURVE. Above kClean the signal is good and nothing is touched — a strong
                //    station must be bit-for-bit what it was before this existed, or the feature
                //    is a tone control that fires on everybody. Below kRough the image is held at
                //    a floor rather than taken to zero: even 2 kHz of separation reads as "stereo,
                //    quietly" and sounds better than a hard collapse to mono.
                // ★★★ CALIBRATED AGAINST THE METER, NOT AGAINST THEORY. This ratio is NOT a
                //     textbook SNR: it is pilot amplitude against what leaks into a 17 kHz window,
                //     and the measuring filter's own leakage puts a CEILING on it — a perfect
                //     synthetic signal reads about 34 dB and cannot read higher. So kClean sits
                //     below that ceiling (or a flawless station would still be narrowed) and
                //     kRough above the floor. Both figures come from test-stereo-highblend, which
                //     prints them; change the filter and these must be re-read, not reasoned about.
                // ★★★ CALIBRATED BY EAR, AND I NEARLY BROKE IT BY REASONING INSTEAD. Real readings:
                //     6 dB (107.8, very noisy), 11 and 13 dB (107.4/105.4, weak and hissy), 27 and
                //     30 dB (106.0/106.9 — "fairly weak stations normally", and fine to listen to).
                //     Seeing the first three all pinned at the FLOOR, I decided that must be wrong
                //     and widened the window to 5-28 so they would spread out.
                // ★★★ THEY WERE NOT WRONG. Stuart, listening to exactly those stations at exactly
                //     that floor: "this one is SIGNIFICANTLY cleaner to listen to", "sounds a lot
                //     cleaner to me". Widening would have handed back a good part of the hiss he
                //     had just gained. The shape of a set of numbers is NOT evidence that they are
                //     mis-scaled — the ear is the instrument here, and it had already answered.
                // ★★ So the window stays 14-30, and it maps well: everything below 14 dB is bad
                //    enough to want the full treatment, 27 dB gets a light touch (blend ~12.6k),
                //    30 dB gets nothing at all.
                constexpr float kClean = 30.0f, kRough = 14.0f;
                constexpr float kWide  = 15000.0f, kNarrow = 2000.0f;
                float t = (blendSnrDb_ - kRough) / (kClean - kRough);
                t = std::min(1.0f, std::max(0.0f, t));
                float want = wspOn_ ? (kNarrow + t * (kWide - kNarrow)) : kWide;
                /* ★★★ IMS: SUPPRESS THE MULTIPATH THAT CEQ CANNOT CORRECT. CEQ undoes a reflection
                 *      properly, but only above 18 dB MPX S/N — below that a CMA equaliser
                 *      contorts itself around noise and makes things worse, so it correctly
                 *      declines. That left a real hole: between "too weak for CEQ" and "clean",
                 *      NOTHING treated multipath AS multipath. The blend above is steered purely
                 *      by MPX S/N — by NOISE — so a station being fought over by two transmitters
                 *      got the wrong medicine entirely.
                 *  ★★★ WHY THE L-R CORNER IS THE RIGHT LEVER. Multipath is phase distortion, and
                 *      the stereo subcarrier at 38 kHz suffers it far worse than the mono sum:
                 *      L-R is recovered by multiplying against a pilot-derived reference, so a
                 *      phase error there lands directly in the difference signal. Blending toward
                 *      mono therefore SUPPRESSES multipath distortion specifically — it is not a
                 *      general-purpose retreat, it is the treatment that matches the fault.
                 *  ★★ SUPPRESSION IS THE SECOND CHOICE, ALWAYS. When CEQ is engaged it is actually
                 *     CORRECTING the reflection, and throwing stereo away on top of that would be
                 *     paying twice for one fault — so this stands down while CEQ has the job.
                 *  ★ The narrower of the two wants wins, the same rule the IF filter uses: noise
                 *    and multipath are different faults and either is reason enough to blend. */
                /* ★★★ WHERE THIS CAN ACTUALLY BE HEARD, STATED HONESTLY. Below about 14 dB the
                 *      NOISE curve above has already taken L-R to its floor, so on a signal that
                 *      is both weak and reflected this adds nothing measurable — the blend is
                 *      already as far as it goes. The window where IMS is the only thing acting
                 *      is roughly 14-18 dB MPX S/N: above the point where noise-blend backs off,
                 *      below the point where CEQ will take the job. It also acts at ANY S/N when
                 *      NR is switched off, because then nothing else is blending at all.
                 *  ★★ That window is narrow, and it is the truthful description of the feature.
                 *     Do not widen kMpSevere to make the control feel busier: a blend that fires
                 *     on a station that does not need it is a tone control, which is the exact
                 *     mistake the NR curve above was calibrated by ear to avoid. */
                /* ★★★ SAY WHICH CONDITION FAILED, exactly as ceqWhy_ does — and for the same
                 *     reason. "Not acting" is honest but useless on its own: the listener cannot
                 *     tell whether IMS declined, is waiting, or is broken. Stuart, looking at a
                 *     station with 9.5% multipath: "not seeing the IMS status".
                 *  ★ 0 acting · 1 switched off · 2 CEQ has the job · 3 no multipath worth treating
                 *    · 4 the noise blend is already narrower, so IMS would change nothing. */
                if (imsNow_ && multipathValid_ && !ceqEngaged_ && multipathCorr_ > 0.06f) {
                    constexpr float kMpLight = 0.06f, kMpSevere = 0.30f;
                    float m = (multipathCorr_ - kMpLight) / (kMpSevere - kMpLight);
                    m = std::min(1.0f, std::max(0.0f, m));
                    const float mpWant = kWide + m * (kNarrow - kWide);
                    /* ★★★ REPORT ONLY WHAT IMS IS THE REASON FOR. If the noise curve had already
                     *     asked for something narrower, IMS changed nothing — and a readout that
                     *     claimed otherwise would be crediting this control with the blend NR was
                     *     already doing. That is the whole reason it is hard to tell these two
                     *     apart by ear, so the panel must not add to the confusion. */
                    if (mpWant < want) { imsBlendHz_ = mpWant; imsWhy_ = 0; }
                    want = std::min(want, mpWant);
                }
                // ★★★ MOVE SLOWLY. A corner that chases the signal sample-by-sample turns fading
                //     into PUMPING, which listeners notice far more readily than the hiss it is
                //     removing — the same lesson as the audio jitter buffer. Roughly a second.
                lmrHiCutHz_ = glideCorner(lmrHiCutHz_, want);
                if (!std::isfinite(lmrHiCutHz_)) lmrHiCutHz_ = kWide;
            } else if (!weakProcOn_.load(std::memory_order_relaxed)
                       && !imsOn_.load(std::memory_order_relaxed)) {
                // Both switched off by the listener — glide open, because that IS the instruction.
                lmrHiCutHz_ = glideCorner(lmrHiCutHz_, 15000.0f);
            } else {
                // ★★★ HOLD. DO NOT OPEN. This branch used to rush the corner to 15 kHz at full
                //     speed whenever the pilot was not locked — and on a marginal signal the lock
                //     FLICKERS. Every flicker threw the L-R filter wide open, so the moment lock
                //     returned the listener got the entire unfiltered difference band back, hiss
                //     and all: "a couple of times in quick succession I just got a load of treble
                //     come back", at 4-7 dB MPX S/N (Stuart, 2026-08-14) — where the curve is
                //     pinned at its floor and NOTHING should have been moving.
                // ★★ Losing pilot lock is not evidence that the signal improved. It is usually
                //    evidence of the opposite, so the honest response is to keep the treatment we
                //    had and let the normal curve re-decide once there is something to measure.
                //    Reopening on the way DOWN was exactly backwards.
                // ★ The old reasoning — "so the next lock does not start half-shut" — had it the
                //   wrong way round too: on a signal this weak, starting half-shut is right.
            }
            // ★ Applied to the FILTERED L-R (rightBuf_), before the matrix turns L+R/L-R into L/R
            //   — after that point the noise is in both channels and cannot be told from music.
            if (lmrHiCutHz_ < 14000.0f && audFs_ > 0.0) {
                const float dt = (float)(1.0 / audFs_);
                const float rc = 1.0f / (2.0f * (float)M_PI * std::max(lmrHiCutHz_, 200.0f));
                const float a  = dt / (rc + dt);
                float y = lmrHiCutY_;
                for (int i = 0; i < nm; ++i) { y += a * (rightBuf_[i] - y); rightBuf_[i] = y; }
                lmrHiCutY_ = std::isfinite(y) ? y : 0.0f;
            } else {
                lmrHiCutY_ = 0.0f;
            }
            const float ramp = (float)(1.0 / (audFs_ * 0.04));   // ~40 ms blend time constant
            stereoBlend_ = stereoMatrixBlend(leftBuf_.data(), rightBuf_.data(),
                                             lprBuf_.data(), lmrBuf_.data(),
                                             nm, stereoBlend_, ramp, target);
            // ★★★ stereoBlend_ IS RECURSIVE STATE — it is fed back into the next block. A NaN
            // in it makes every subsequent sample NaN through `target - e`, which is one of the
            // two ways the FM mute latched. Same treatment as the other IIR states.
            if (!std::isfinite(stereoBlend_)) stereoBlend_ = 0.0f;
            // ★ The audio high-cut, on BOTH channels with the SAME corner. Identical treatment is
            //   not a detail: give L and R different corners and the stereo image shifts with the
            //   signal, which is far more objectionable than the hiss.
            if (audioHiCutHz_ < 14000.0f && audFs_ > 0.0) {
                const float dt = (float)(1.0 / audFs_);
                const float rc = 1.0f / (2.0f * (float)M_PI * std::max(audioHiCutHz_, 500.0f));
                const float a  = dt / (rc + dt);
                float yl = hiCutYL_, yr = hiCutYR_;
                for (int i = 0; i < nm; ++i) {
                    yl += a * (lprBuf_[i] - yl); lprBuf_[i] = yl;
                    yr += a * (lmrBuf_[i] - yr); lmrBuf_[i] = yr;
                }
                hiCutYL_ = std::isfinite(yl) ? yl : 0.0f;
                hiCutYR_ = std::isfinite(yr) ? yr : 0.0f;
            } else { hiCutYL_ = hiCutYR_ = 0.0f; }
            trace_("matrix_l", lprBuf_.data(), nm);
            trace_("matrix_r", lmrBuf_.data(), nm);
            const bool lk = stereoBlend_ > 0.5f;   // indicator follows audible state
            if (useDeemph_) {                  // off -> skip (tau=0)
                deemph_.process(lprBuf_.data(), nm);
                deemphR_.process(lmrBuf_.data(), nm);
                trace_("deemph_l", lprBuf_.data(), nm);
                trace_("deemph_r", lmrBuf_.data(), nm);
            }
            audioBuf_.resize(resamp_->maxOut(nm));
            rOutBuf_.resize(resampR_->maxOut(nm));
            const int na = resamp_->process(lprBuf_.data(), nm, audioBuf_.data());
            const int nb = resampR_->process(lmrBuf_.data(), nm, rOutBuf_.data());
            const int no = std::min(na, nb);
            if (no > 0) {
                ilvBuf_.resize(no * 2);
                interleave2(audioBuf_.data(), rOutBuf_.data(), ilvBuf_.data(), no);
                cb_.audio(cb_.ctx, ilvBuf_.data(), no, 2, outRate_);
            }
            const bool forced = stereoReport_.exchange(false, std::memory_order_relaxed);
            if (cb_.stereo && (lk != lastStereo_ || forced)) { lastStereo_ = lk; cb_.stereo(cb_.ctx, lk); }
        } else {
            // ── Mono post-chain (AM/SSB/CW/NFM + WFM-mono fallback) ───────────
            int nd = nc;
            float* audioIn = demodBuf_.data();
            if (useDeemph_) deemph_.process(demodBuf_.data(), nc);
            if (audioLpf_) {
                lpfBuf_.resize(audioLpf_->maxOut(nc));
                nd = audioLpf_->process(demodBuf_.data(), nc, lpfBuf_.data());
                audioIn = lpfBuf_.data();
            }
            // ★★ AND THE SAME CUT IN MONO — this is the path Stuart was listening on when he found
            //    the hiss that high-blend could not reach. `audioIn` may be lpfBuf_ or demodBuf_
            //    depending on whether the 15 kHz filter ran, so it is written through the pointer.
            if (mode_ == Mode::WFM && audioHiCutHz_ < 14000.0f && audFs_ > 0.0 && nd > 0) {
                const float dt = (float)(1.0 / audFs_);
                const float rc = 1.0f / (2.0f * (float)M_PI * std::max(audioHiCutHz_, 500.0f));
                const float a  = dt / (rc + dt);
                float* w = const_cast<float*>(audioIn);
                float y = hiCutYM_;
                for (int i = 0; i < nd; ++i) { y += a * (w[i] - y); w[i] = y; }
                hiCutYM_ = std::isfinite(y) ? y : 0.0f;
            } else { hiCutYM_ = 0.0f; }
            if (useAgc_) { agc_.process(audioIn, nd); agc_.guard(); }  // AM/SSB/CW level + anti-clip
            trace_("mono_out", audioIn, nd);
            audioBuf_.resize(resamp_->maxOut(nd));
            const int na = resamp_->process(audioIn, nd, audioBuf_.data());
            if (na > 0) cb_.audio(cb_.ctx, audioBuf_.data(), na, 1, outRate_);
        }
        // A fault is "in progress" for as long as blocks keep arriving bad; faultSeq_ only
        // moves on a fresh onset, so a reader can tell a new event from a continuing one.
        inFault_ = (faultStage_ != nullptr);
    }
}

void RxPipeline::stop() {
    stopDemodThread_();
    stopSpecThread_();
    cfft_.reset(); zoom_.reset(); decs_.clear(); am_.reset(); resamp_.reset();
}

/** Thread-safe request; the DSP thread applies it in feed(). Only a CHANGE marks dirty, so a
 *  client that resends its view every frame does not rebuild filters 20 times a second. */
void RxPipeline::setZoomView(double offsetHz, double spanHz, double rateHz) {
    if (rateHz > 0.0) {
        const double old = zoomRateReq_.exchange(rateHz, std::memory_order_relaxed);
        if (old != rateHz) zoomDirty_.store(true, std::memory_order_release);
    }
    if (spanHz <= 0.0) {
        if (zoomSpanReq_.exchange(0.0, std::memory_order_relaxed) != 0.0)
            zoomDirty_.store(true, std::memory_order_release);
        return;
    }
    const double oldOff  = zoomOffReq_.exchange(offsetHz, std::memory_order_relaxed);
    const double oldSpan = zoomSpanReq_.exchange(spanHz,  std::memory_order_relaxed);
    if (oldOff != offsetHz || oldSpan != spanHz)
        zoomDirty_.store(true, std::memory_order_release);
}

} // namespace vibedsp
