// VibeSDR V5 — RxPipeline: IQ -> {spectrum, audio}. Original VibeSDR code.
#include "vibedsp.h"
#include "simd_internal.h"   // stereoMatrixBlend / interleave2 (NEON)
#include <cmath>
#include <algorithm>

namespace vibedsp {

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
    offsetHz_ = offsetHz;
    mode_     = mode;
    bwHz_     = bwHz;
    dirty_    = true;
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
    useDeemph_ = false; stereo_ = false; useAgc_ = false;
    // Only WFM decimates inside its audio filters; every other mode's post-chain
    // runs at the channel rate, which is already as low as that mode needs.
    audioDecim_ = 1; audFs_ = chFs_;
    switch (mode_) {
        case Mode::AM:                          am_ = std::make_unique<AmDemod>();
                                                useAgc_ = true; agc_.configure(chFs_); agc_.reset(); break;
        case Mode::SSB_USB: case Mode::SSB_LSB:
        case Mode::CW:
            ssb_ = std::make_unique<SsbDemod>();
            ssb_->configure(mode_ == Mode::SSB_LSB ? SsbDemod::Side::LSB : SsbDemod::Side::USB,
                            bwHz_, chFs_);
            useAgc_ = true; agc_.configure(chFs_); agc_.reset(); break;
        case Mode::NFM:
            fm_ = std::make_unique<FmDemod>((float)(chFs_ / (2.0 * M_PI * std::max(1.0, bwHz_ * 0.5))));
            if (deempTau_.load() > 0.0) { deemph_.configure(deempTau_.load(), chFs_); deemph_.reset(); useDeemph_ = true; }
            break;
        case Mode::WFM: {
            // Wideband FM. Discriminator -> MPX. Mono path = 15 kHz L+R LPF +
            // de-emphasis. Stereo path adds a 19 kHz pilot PLL, 38 kHz coherent
            // L-R recovery, a second 15 kHz LPF, and per-channel de-emphasis.
            fm_ = std::make_unique<FmDemod>((float)(chFs_ / (2.0 * M_PI * 75000.0)));
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
}

void RxPipeline::feed(const cf32* iq, int n) {
    if (dirty_) rebuildAudio();

    // ── Spectrum ───────────────────────────────────────────────────────────
    // Gather fftSize contiguous samples for a frame, then skip to the next slot.
    if (cb_.spectrum) {
        // Gather fftSize contiguous samples into a frame, FFT + emit, then DROP the
        // remaining (specStride - fftSize) samples to honour the frame rate. This is
        // O(n) — never the per-sample buffer shift (O(n*fftSize)) that can't keep up
        // at MS/s. `sinceFrame_` doubles as the inter-frame drop countdown.
        cf32* sb = reinterpret_cast<cf32*>(specBuf_.data());
        for (int i = 0; i < n; ++i) {
            if (sinceFrame_ > 0) { --sinceFrame_; continue; }
            sb[specFill_++] = iq[i];
            if (specFill_ >= fftSize_) {
                const float scale = 1.0f / (float)(fftSize_ * fftSize_);
                cfft_->powerDbShifted(sb, win_.data(), specDb_.data(), scale);
                cb_.spectrum(cb_.ctx, specDb_.data(), fftSize_);
                specFill_   = 0;
                sinceFrame_ = std::max(0LL, (long long)specStride_ - fftSize_);
            }
        }
    }

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

        demodBuf_.resize(nc);
        if (am_)       am_->process(chBuf_.data(), demodBuf_.data(), nc);
        else if (fm_)  fm_->process(chBuf_.data(), demodBuf_.data(), nc);
        else if (ssb_) ssb_->process(chBuf_.data(), demodBuf_.data(), nc);

        if (stereo_) {
            // ── WFM stereo MPX decode ────────────────────────────────────────
            // demodBuf_ is the MPX. L+R = LPF(mpx); L-R = LPF(mpx * 38kHz_ref).
            // Only generate the 57 kHz reference and bit clock when something is
            // actually listening for RDS — that is a third of the PLL's per-sample
            // work, and with no subscriber it was being computed and thrown away.
            const bool wantRds = (cb_.rdsPs || cb_.rdsText || cb_.rdsPi || cb_.rdsSig || cb_.rdsExt);
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
            const int n1 = audioLpf_->process(lprBuf_.data(), nc, leftBuf_.data()); // L+R
            const int n2 = lmrLpf_->process(lmrBuf_.data(),  nc, rightBuf_.data()); // L-R
            const int nm = std::min(n1, n2);
            // Stereo BLEND (anti-screech): fade the L-R in/out by a smoothed
            // pilot-lock confidence rather than hard-switching. forceMono or no
            // lock -> target 0 (clean mono); solid lock -> 1. The per-sample ramp
            // (~ a few ms) stops the harsh on/off when an edge signal flickers.
            const bool wantStereo = stereoEnabled_.load();
            // Target full stereo when the pilot is locked (locked() has hysteresis
            // so it won't chatter on an edge signal), else mono. The ramp does the
            // smoothing so the transition fades instead of screeching.
            const float target = (wantStereo && pll_.locked()) ? 1.0f : 0.0f;
            const float ramp = (float)(1.0 / (audFs_ * 0.04));   // ~40 ms blend time constant
            stereoBlend_ = stereoMatrixBlend(leftBuf_.data(), rightBuf_.data(),
                                             lprBuf_.data(), lmrBuf_.data(),
                                             nm, stereoBlend_, ramp, target);
            const bool lk = stereoBlend_ > 0.5f;   // indicator follows audible state
            if (useDeemph_) {                  // off -> skip (tau=0)
                deemph_.process(lprBuf_.data(), nm);
                deemphR_.process(lmrBuf_.data(), nm);
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
            if (cb_.stereo && lk != lastStereo_) { lastStereo_ = lk; cb_.stereo(cb_.ctx, lk); }
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
            if (useAgc_) agc_.process(audioIn, nd);   // AM/SSB/CW level + anti-clip
            audioBuf_.resize(resamp_->maxOut(nd));
            const int na = resamp_->process(audioIn, nd, audioBuf_.data());
            if (na > 0) cb_.audio(cb_.ctx, audioBuf_.data(), na, 1, outRate_);
        }
    }
}

void RxPipeline::stop() {
    cfft_.reset(); decs_.clear(); am_.reset(); resamp_.reset();
}

} // namespace vibedsp
