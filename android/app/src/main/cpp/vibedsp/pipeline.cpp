// VibeSDR V5 — RxPipeline: IQ -> {spectrum, audio}. Original VibeSDR code.
#include "vibedsp.h"
#include <cstring>
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
    offsetHz_ = offsetHz;
    mode_     = mode;
    bwHz_     = bwHz;
    if (sameChain) tuneReq_.store(true, std::memory_order_relaxed);
    else           dirty_ = true;
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
    double targetCh = std::max(bwHz_ * 3.0, 12000.0);
    // WFM is the exception: its MPX runs to 57 kHz (RDS) + sidebands, so it needs a
    // real channel regardless of the RF bandwidth the user picked.
    // ★★★ FM-DX NO LONGER WIDENS THE CHANNEL — the premise was wrong, and measurement killed
    // it. See the chHalf note below: widening recovers subcarrier AMPLITUDE and destroys
    // subcarrier SNR, which is the thing that actually decodes.
    if (mode_ == Mode::WFM) targetCh = std::max(bwHz_ * 1.5, 150000.0);
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
    const double transHz = std::max(chHalf * 0.5, chFs_ * 0.25 - chHalf);

    decs_.clear();
    std::vector<int> stages;
    {   // Decimate HARD and EARLY. Each stage costs roughly (its output rate x taps),
        // so the goal is to collapse the sample rate as fast as possible and do the
        // remaining work cheaply. Prime factors are the wrong tool: a decimation of 64
        // prime-factorises to SIX stages of 2, and a first stage of 2 barely reduces
        // the rate — so the expensive high-rate samples get dragged through stage after
        // stage. (Measured: that made NFM 40% dearer than doing nothing.) Greedily take
        // the LARGEST composite factor up to 8 instead: 64 -> 8x8, 50 -> 5x5x2.
        int d = chDecim_;
        while (d > 1) {
            int f = 1;
            for (int p = 8; p >= 2; --p) if (d % p == 0) { f = p; break; }
            if (f == 1) { stages.push_back(d); break; }   // awkward prime: one stage
            stages.push_back(f);
            d /= f;
        }
        if (stages.empty()) stages.push_back(1);   // chDecim_ == 1: a plain filter
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
                            bwHz_, chFs_);
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
            const double cut = 15000.0 / chFs_;
            audioLpf_ = std::make_unique<RealFir>(designLowpass(cut, cut * 0.4, /*deepStop=*/true), audioDecim_);
            lmrLpf_   = std::make_unique<RealFir>(designLowpass(cut, cut * 0.4, /*deepStop=*/true), audioDecim_);
            pll_.configure(19000.0, chFs_); pll_.reset();
            stereoBlend_ = 0.0f;               // new tune starts mono, blends up
            // ★ The noise meter reads the MPX at the CHANNEL rate (where 17 kHz still exists),
            //   not the audio rate. Starting wide open matters: a new station must be given the
            //   benefit of the doubt and narrowed if it earns it, not opened up from mono —
            //   which would be audible as a swell on every retune.
            mpxNoise_.configure(chFs_);
            multipath_.configure(chFs_);
            adaptIf_.configure(chFs_); adaptIf_.setBandwidth(ifBwHz_);
            shadowIf_.configure(chFs_); shadowIf_.setBandwidth(shadowBwHz_);
            shadowFm_.reset();
            shadowNoise_.configure(chFs_, 17000.0);
            shadowPilot_.configure(chFs_, 19000.0);
            widePilot_.configure(chFs_, 19000.0);
            ifGainDb_ = 0.0f; shadowTick_ = 0;
            lmrHiCutHz_ = 15000.0f; lmrHiCutY_ = 0.0f; blendSnrDb_ = 99.0f;
            audioHiCutHz_ = 15000.0f; hiCutYL_ = hiCutYR_ = hiCutYM_ = 0.0f;
            const int rch = (int)std::llround(audFs_);
            resampR_ = std::make_unique<RationalResampler>(rch, outRate_);
            stereo_ = true; lastStereo_ = false;

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
    // ★★ A GAP IN THE STREAM INVALIDATES EVERY RECURSIVE STATE. Honoured HERE because
    // this is the thread that owns them (see requestReset). The RDS decoder is the one
    // that mattered in the field: its timing hypotheses kept their scores across an
    // idle pause, so a stale one could out-score the correctly-aligned one for good.
    if (resetReq_.exchange(false, std::memory_order_relaxed)) {
        rdsDemod_.reset();
        pll_.configure(19000.0, chFs_);      // re-seed the pilot loop from scratch
        deemph_.reset();
        deemphR_.reset();
        dirty_ = true;                       // and rebuild the audio chain around them
    }
    // ★ Narrow resync: drop the previous station's RDS state and re-seed the pilot, but leave
    //   the audio chain (and its AGC) alone. Ordered after the full reset above so the two do not
    //   fight, and guarded on chFs_ because the pilot cannot be configured before the first build.
    if (rdsResyncReq_.exchange(false, std::memory_order_relaxed) && chFs_ > 0.0) {
        rdsDemod_.reset();
        pll_.configure(19000.0, chFs_);
    }
    if (dirty_) rebuildAudio();          // rebuildAudio() re-points the NCO itself
    // A same-chain retune: nothing to rebuild, just move the oscillator. Skipped when a
    // rebuild already ran this block, since that has applied the newer offset anyway.
    else if (tuneReq_.exchange(false, std::memory_order_relaxed))
        nco_.setFreq(offsetHz_ / sampleRate_);

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
        const long long stride = std::max(1, specStride_.load(std::memory_order_relaxed));
        for (int i = 0; i < n; ++i) {
            ring[specRingW_] = iq[i];
            if (++specRingW_ >= fftSize_) specRingW_ = 0;
            ++specRingFill_;
            if (++sinceEmit_ < stride) continue;
            sinceEmit_ = 0;
            if (specRingFill_ < fftSize_) continue;      // warm-up: not a full window yet
            // Unwrap oldest-first so the window is time-ordered; specRingW_ is the oldest
            // sample now that it has advanced past the newest.
            const int tail = fftSize_ - specRingW_;
            std::memcpy(sb,        ring + specRingW_, (size_t)tail       * sizeof(cf32));
            std::memcpy(sb + tail, ring,              (size_t)specRingW_ * sizeof(cf32));
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

    // ── Audio (DDC -> demod -> resample) ─────────────────────────────────────
    if (cb_.audio) {
        faultStage_ = nullptr;          // per-block: trace_() records the FIRST bad stage
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

        demodBuf_.resize(nc);
        // ★★★ MEASURED ON THE IQ, BEFORE DEMODULATION — this is the ONLY place the information
        //     exists. The FM demodulator throws amplitude away by design (that is what makes FM
        //     immune to AM noise), so after this line the envelope wobble that reveals multipath
        //     is simply gone. Read-only; chBuf_ is untouched.
        // ★ The adaptive IF sits BEFORE the multipath meter and the demod, because it is part of
        //   the receiver, not part of the measurement — everything downstream should see the
        //   signal as filtered, exactly as it would with a narrower crystal filter.
        // ★★★ THE SHADOW COPY IS TAKEN BEFORE THE ADAPTIVE FILTER, ALWAYS. If it were taken after,
        //     the comparison would depend on what the filter is currently doing and the control
        //     would be steering by its own output — the feedback trap this whole design is built to
        //     avoid ("a probe is part of the system it measures").
        if (mode_ == Mode::WFM && shadowTick_ + 1 >= 4) {
            shadowBuf_.assign(chBuf_.begin(), chBuf_.begin() + nc);
        }
        if (mode_ == Mode::WFM) {
            const double want = ifBwReq_.load(std::memory_order_relaxed);
            if (want != ifBwHz_) { ifBwHz_ = want; adaptIf_.setBandwidth(want); }
            adaptIf_.process(chBuf_.data(), nc);
        }
        if (mode_ == Mode::WFM) multipath_.process(chBuf_.data(), nc);
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
            if (weakProcOn_.load(std::memory_order_relaxed)) {
                if (ifGainDb_ > 3.0f) {
                    if (++ifDwell_ > 40) {            // ~3 s at one evaluation per 4 blocks
                        ifDwell_ = 0;
                        ifBwReq_.store(ifBwHz_ > 0.0 ? 0.0 : shadowBwHz_,
                                       std::memory_order_relaxed);
                    }
                } else ifDwell_ = 0;
            } else if (ifBwHz_ > 0.0) {
                ifBwReq_.store(0.0, std::memory_order_relaxed);   // switched off = wide open
                ifDwell_ = 0;
            }
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
            multipathValid_ = (blendSnrDb_ > 12.0f);
        } else { multipathCorr_ = 0.0f; multipathValid_ = false; }
        if (am_)       am_->process(chBuf_.data(), demodBuf_.data(), nc);
        else if (fm_)  fm_->process(chBuf_.data(), demodBuf_.data(), nc);
        else if (ssb_) ssb_->process(chBuf_.data(), demodBuf_.data(), nc);
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
                if (pilot > 1e-6f) {
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
            // RDS (only meaningful once the pilot is locked).
            rdsDemod_.setPilotRef(pll_.lockAmp());
            if (wantRds && cb_.rdsBer)
                cb_.rdsBer(cb_.ctx, pll_.trackable() ? rdsDemod_.blockErrorPercent() : -1);
            if (wantRds && cb_.rdsSig)
                cb_.rdsSig(cb_.ctx, rdsDemod_.subcarrierRelDb());
            // ★ The MPX spectrum, computed only when the Advanced RDS panel is watching.
            // demodBuf_ IS the MPX — the same buffer the stereo and RDS decoders read — so
            // this costs one FFT and no new signal path.
            if (wantRds && cb_.rdsExt) {
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
            if (wantRds && cb_.rdsExt) {
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
                x.groupCounts = a.groupCounts; x.groupTotal = a.groupTotal;
                x.rtpTitle = a.rtpTitle; x.rtpArtist = a.rtpArtist; x.longPs = a.longPs;
                x.ptyn = a.ptyn; x.language = a.language;
                x.pinDay = a.pinDay; x.pinHour = a.pinHour; x.pinMinute = a.pinMinute;
                x.eon = eons; x.nEon = nEon;
                x.oda = odas; x.nOda = nOda;
                x.constXY = xy; x.nPts = np;
                x.pilotPhaseDeg = rdsDemod_.pilotPhaseDeg();
                x.pilotPhaseCoherence = rdsDemod_.pilotPhaseCoherence();
                x.pilotPhaseDriftDegPerSec = rdsDemod_.pilotPhaseDriftDegPerSec();
                x.pilotDevKHz = pll_.pilotDeviationKHz();
                x.mpx = mpxOut_.empty() ? nullptr : mpxOut_.data();
                x.nMpx = (int)mpxOut_.size();
                x.rdsDevKHz   = rdsDemod_.rdsDeviationKHz();
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
            if (mpxNoise_.ready() && wantStereo && pll_.locked()
                && weakProcOn_.load(std::memory_order_relaxed)) {
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
                const float want = kNarrow + t * (kWide - kNarrow);
                // ★★★ MOVE SLOWLY. A corner that chases the signal sample-by-sample turns fading
                //     into PUMPING, which listeners notice far more readily than the hiss it is
                //     removing — the same lesson as the audio jitter buffer. Roughly a second.
                lmrHiCutHz_ = glideCorner(lmrHiCutHz_, want);
                if (!std::isfinite(lmrHiCutHz_)) lmrHiCutHz_ = kWide;
            } else if (!weakProcOn_.load(std::memory_order_relaxed)) {
                // Switched off by the listener — glide open, because that IS the instruction.
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
