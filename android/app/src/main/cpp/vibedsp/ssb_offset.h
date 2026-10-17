// ★★★ SSB OFFSET ESTIMATOR — how far a single-sideband voice is mistuned, from the voice itself.
//
// WHY. A voice on SSB is a comb of harmonics at k·f0. Mistune the dial by d Hz and every harmonic
// moves by the SAME d — the comb is no longer a set of multiples, it is k·f0 + d. People hear that
// as "Donald Duck"; the on-device speech recogniser hears it as a worse match: measured on a strong
// 80 m LSB contact (V12 research, 2026-10-16) ±100 Hz cost 0.78-0.82 transcript agreement and
// ±200 Hz 0.50-0.65. Shifting the audio back by the estimated d recovered 0.89-0.97.
//
// HOW — TWO STAGES (2026-10-17; stage 2 is a port of the research rig's `auto` estimator, chain.py est_offset):
//   The comb alone fixes d finely but only MODULO the pitch — k·f0 + d fits d ± f0 nearly as well — which is why it
//   was confined to ±300 Hz. The owner's ear supplies the other half (on LSB, "tuning below makes it deep and a bit
//   robot sounding, above makes it super thin"): a mistuning slides the voice's whole energy block, so WHERE it sits
//   in the passband says roughly how far, and on which side. Together they reach ±1200 Hz.
//   1. Decimate to 8 kHz. Cut 80 ms frames; in each block of audio keep the LOUDEST third (voice
//      is louder than the noise between words) — the voiced-frame selection — and measure the QUIETEST frames too.
//   2. Each kept frame: log spectrum, whitened (minus its own ~180 Hz moving average), so a
//      harmonic is a bump whatever the band's tilt.
//   3. FINE — pitch-harmonic alignment: for every offset d (±1200 Hz) the best comb k·f0 + d over the frame's
//      plausible pitches scores harmonics minus the mid-points between them — and, new, scores AGAINST d any
//      harmonic energy at d, d − f0, … (a voice has nothing at or below its carrier: that refutes the alias a pitch
//      above the truth, and is the "super thin" empty bottom heard by the comb). A frame that fits no comb well is
//      not voiced and is dropped.
//   4. Each frame's curve over d is z-scored and SUMMED over frames. A wrong d fits only frames
//      whose pitch happens to alias it (d mod f0), the true d fits all of them as the pitch moves
//      with intonation — so the true d is a SHARP peak and aliases are broad. A high-pass on the
//      summed curve keeps the sharp one.
//   5. COARSE — where the voice's energy sits (coarse_): the voice's lower edge (voiced frames over the quiet ones)
//      against the receiver's own passband floor (the quiet frames' noise). A transmitter cuts its low end anywhere
//      from ~60 to ~400 Hz, so the edge only bounds the carrier — and on the deep side, where the edge is hidden below
//      the floor, only from above. That interval VETOES the comb's peaks outside it; the answer is the strongest peak
//      inside, and a much stronger one outside means the two cannot be reconciled (reconciled = false, not believed).
//   6. The pitch is found per frame from the whitened spectrum's autocorrelation (shift-invariant, so the mistuning
//      does not disturb it) and refined before the comb search; the offset is searched in 2 Hz steps (FAST 4), then
//      parabolic-refined.
//
// ★★ THE ANSWER COMES WITH A CONFIDENCE, AND A CALLER MUST GATE ON IT (SsbOffsetEstimate::passes).
//    `confidence` is the peak's STRENGTH — the mean per-frame z-score at the winning offset, how firmly every voiced
//    frame agrees on it; `clarity` how far it stands above its best in-range rival (a pitch alias, another station).
//    Each profile carries its own gate, measured on off-air recordings — VOLMET at eleven known dials and QSOs from
//    busy-band I/Q (vibeserver/test-ssb-offset-real.cpp; the numbers beside passes() in ssb_offset.cpp).
//
// ★ PROFILES. SLOW — transcription (SsbAutoTune below): ~10 s of voiced audio per estimate (the loudest third of
//   ~30 s of speech), ~0.2 % of a desktop core. FAST — the binaural tuning aid: 40 ms frames, an answer per ~0.6 s of
//   voiced audio (1-2 s of speech), built on a MEMORY of the last ~8 s of that voice (the deep side needs seconds of
//   intonation to tell one pitch alias from the next); a QSO's two voices get a memory each (a pitch change swaps
//   them), and a dial move MOVES the memory (shiftAudio) instead of discarding it. ~0.3 % of a desktop core.
//   FAST for the binaural aid:
//     SsbOffsetEstimator est(16000, SsbOffsetEstimator::Profile::Fast);   // or the audio's own rate
//     if (est.push(buf, n)) {                      // true when a new estimate completed in this call
//         const auto& e = est.last();              // e.offsetHz (+ = audio HIGH), e.confidence, e.passes()
//     }                                            // est.reset() on a retune; est.shiftAudio(Hz) on a known dial move
//
// ★ Thread rules: none of these classes locks. One instance per feeding thread. The C API at the
//   bottom (vibe_rxfeed_*) is what Swift and JNI hold; its set_tuning may be called from ANY
//   thread (it only stores atomics) and the feeding thread applies it.
#pragma once
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace vibedsp {

class RationalResampler;
class RealFFT;

struct SsbOffsetEstimate {
    bool  valid = false;          ///< an estimate was made (enough voiced frames); NOT that it passed the gate
    float offsetHz = 0.0f;        ///< where the voice's harmonic comb sits: +d = the audio is d Hz HIGH
    float confidence = 0.0f;      ///< peak STRENGTH: the mean per-frame z-score at the winning offset (see above)
    float clarity = 0.0f;         ///< the winner over the best rival >30 Hz away, in curve spreads (diagnostic)
    float voicedSeconds = 0.0f;   ///< voiced audio the estimate is built from
    float snrDb = 0.0f;           ///< voiced frames over the quiet ones between words — a rough SNR
    int   frames = 0;             ///< voiced frames used
    int   minFrames = 60;         ///< the profile's floor for believing it (SLOW 60 = 4.8 s; FAST 20)
    /** ★ The voice's PITCH: the mean, over the voiced frames used, of each frame's F0 from the whitened spectrum's
     *  autocorrelation (Hz; 0 when not measured — Config::fullPitchSearch skips the autocorrelation).
     *  Diagnostic for the estimate itself — but the binaural aid gates on it (BinauralTuning::kHighF0Hz): a HIGH
     *  voice (a child, F0 ~250-400 Hz) has few, widely spaced harmonics in a 2.7 kHz passband, and the comb
     *  k·f0 + d fits d ± f0 almost as well as d, so its answer is far less certain. The autocorrelation is already
     *  computed per frame (it proposes the comb's pitches); reading the pitch off it costs one pass over ~85 lags. */
    float f0Hz = 0.0f;
    /** ★★ THE COARSE STAGE (two-stage estimate, 2026-10-17): the offsets that agree with WHERE the voice's energy sits in
     *  the passband — its lower edge (voiceLowHz) against the receiver's own (passbandLowHz, from the noise between
     *  words). A voice tuned high ("super thin") has a gap at the bottom; tuned low ("deep") it is piled against the
     *  floor. [coarseLoHz, coarseHiHz] is wide (a transmitter's own low cut is anywhere from ~60 to ~400 Hz), and on
     *  the deep side open-ended — so it is a VETO on the comb's candidates, not an answer. 0/0 = no position measured. */
    float coarseLoHz = 0.0f, coarseHiHz = 0.0f;
    float voiceLowHz = 0.0f;      ///< the voice's lower edge in the audio (Hz), 0 = not found
    float passbandLowHz = 0.0f;   ///< the receiver passband's lower edge, from the noise between words (Hz)
    /** false = the comb's strongest answer lies OUTSIDE the coarse interval by a clear margin — the two stages cannot
     *  be reconciled (two stations, or a voice nothing like a voice); never believed (passes() is false). */
    bool  reconciled = true;
    /** ★★ THE GATE THIS ESTIMATE IS JUDGED BY — the profile's own, measured on off-air clips (Config::gate*), copied in
     *  so passes() needs nothing else. See the note above kFastGate in ssb_offset.cpp for the numbers. */
    struct Gate {
        float confidence = 0.4f;  ///< confidence at least this…
        float clarity = 0.3f;     ///< …and the winner this many spreads clear of its best in-range rival…
        float largeHz = 40.0f;    ///< …and beyond ±largeHz:
        float largeSnrDb = 10.0f; ///<    a speech-to-gap ratio of at least this…
        int   largeFrames = 60;   ///<    …from at least this many voiced frames
        /** ★ DEEP AND PINNED: an answer at or below −deepHz while the coarse stage says the voice is piled against the
         *  passband floor (its interval open downward) cannot have the wrong SIGN — the veto forbids anything thin — so
         *  only its size is in doubt, and there a voice's strongest harmonics are below 0 Hz: the comb is weak by
         *  nature. It is believed at deepConfidence if its clarity reaches deepClarity. Off when deepConfidence ≥ confidence. */
        float deepHz = 400.0f, deepConfidence = 0.4f, deepClarity = 0.5f;
    } gate;
    bool  coarsePinned = false;   ///< the coarse stage found the voice piled against the floor (deep, open-ended)
    bool  passes() const;         ///< the gate: valid, reconciled, enough frames, and `gate` met
    bool  deepPinned() const;     ///< judged by the deep-and-pinned floor (Gate::deepConfidence)
    float confidenceFloor() const;///< the confidence this answer must reach (scaled by callers with rules of their own)
};

class SsbOffsetEstimator {
public:
    enum class Profile { Slow, Fast };

    struct Config {
        double rate = 8000.0;        ///< internal analysis rate
        int    frame = 640;          ///< samples per frame at `rate` (80 ms)
        int    hop = 640;            ///< frame step
        int    nfft = 4096;          ///< zero-padded FFT (1.95 Hz bins at 8 kHz)
        int    blockFrames = 37;     ///< selection block (~3 s): the loudest part of each is kept
        float  keepFraction = 1.0f / 3;   ///< …this much of it (the rig: a third)
        float  windowVoicedSec = 10; ///< an estimate is made once this much voiced audio is in
        float  maxWindowSec = 60;    ///< …or this much audio of any kind, if at least minVoicedSec is
        float  minVoicedSec = 4;
        float  dMaxHz = 1200;        ///< offsets searched: ±dMax (★ was ±300 until the two-stage estimate, 2026-10-17)
        float  dStepHz = 2;
        float  f0Lo = 70, f0Hi = 400;///< plausible voice pitch
        float  combLoHz = 120, combHiHz = 2600;   ///< harmonics scored inside this band
        float  voicedScore = 0.5f;   ///< a frame whose best comb scores below this is not voiced
        int    minFrames = 60;       ///< fewer voiced frames than this is never believed (noise can look sure)
        bool   fullPitchSearch = false; ///< every pitch 1 Hz apart (the rig's way, ~8x the cost) instead of the
                                        ///< autocorrelation's candidates refined to 0.5 Hz — kept for the bench
        /** ★ A COMB HAS NOTHING AT OR BELOW ITS CARRIER: harmonics k·f0 + d exist for k ≥ 1 only, so energy at d,
         *  d − f0, … counts AGAINST d (with this weight). It is what refutes an alias a pitch or more above the truth. */
        float  belowCarrierWeight = 1.0f;
        float  belowCarrierLoHz = 70;   ///< …looked for from here up (the passband's own floor is below it)
        /** ★ 0 = every estimate starts from nothing (SLOW). >0 = the evidence DECAYS with this horizon of voiced audio
         *  instead of being thrown away after each answer (FAST: answers every windowVoicedSec, built on ~this much). */
        float  memoryVoicedSec = 0;
        bool   positionCue = true;   ///< the coarse stage (where the voice's energy sits); off for the bench only
        int    pitchSteps = 13;      ///< the comb's pitch refinement around each autocorrelation candidate…
        float  pitchStepHz = 0.5f;   ///< …in steps of this (13 × 0.5 = ±3 Hz). ★ FAST uses 7 × 1 Hz: as good on the
                                     ///< off-air clips and ~40 % cheaper once the search reached ±1200 Hz; SLOW keeps
                                     ///< 0.5 Hz — a pitch error is multiplied by the harmonic number, and far off tune
                                     ///< only high harmonics are left (a synthetic −180 Hz read 3.7 Hz out at 1 Hz steps)
        SsbOffsetEstimate::Gate gate;   ///< what passes() asks of this profile's answers (measured; see ssb_offset.cpp)
        static Config forProfile(Profile p);
    };

    explicit SsbOffsetEstimator(double inRate, Profile p = Profile::Slow);
    SsbOffsetEstimator(double inRate, const Config& c);
    ~SsbOffsetEstimator();
    SsbOffsetEstimator(const SsbOffsetEstimator&) = delete;
    SsbOffsetEstimator& operator=(const SsbOffsetEstimator&) = delete;

    /** Feed mono audio at the input rate. Returns true when an estimate completed during this
     *  call (read it with last()). Cheap between estimates: decimation plus one energy sum per frame. */
    bool push(const float* x, int n);
    const SsbOffsetEstimate& last() const { return last_; }
    /** Estimates completed since construction/reset. */
    int count() const { return count_; }
    /** Forget everything (a retune or mode change: the old offset means nothing now). */
    void reset();
    /** ★★ THE DIAL MOVED by a known amount: the voice's audio moved by `audioHz` (USB: dial +Δ → −Δ; LSB: +Δ). The
     *  evidence gathered so far is MOVED with it rather than thrown away (the comb curve and the voice's spectrum are
     *  both in audio Hz), and the audio queued before the move is dropped. A FAST estimator (memoryVoicedSec > 0) keeps
     *  what it knew across a knob turn — the deep side needs seconds of intonation to tell one pitch alias from the next.
     *  ★★ A caller that cannot say how far the dial moved must reset() instead: FAST's memory otherwise holds the old
     *     offset for seconds. Measured on Stuart's in-app recording (no dial known), it read "on tune" for ~6 s after he
     *     tuned 500 Hz off before its confidence fell under the gate — and a detector for "the voice moved" was tried and
     *     dropped: at a safe threshold it never fired, at a lower one it replaced the memory with deep-side aliases. */
    void shiftAudio(float audioHz);
    /** Voiced audio collected toward the next estimate. */
    float pendingVoicedSeconds() const;

    /** One-shot over a whole buffer: every frame's evidence summed into ONE estimate. Offline use
     *  (tests, the research rig); a live caller uses push(). */
    static SsbOffsetEstimate estimate(const float* x, int n, double inRate, Profile p = Profile::Slow);
    static SsbOffsetEstimate estimate(const float* x, int n, double inRate, Config c);

    /** FAST: how often the memory was cut because another voice began (diagnostic, test-ssb-offset-real). */
    int speakerChanges() const { return speakerChanges_; }
    /** CPU spent in analysis (spectra + comb search), seconds — for the cost report. */
    double cpuSeconds() const { return cpuSec_; }

private:
    void frameReady_(const float* f);
    void flushBlock_();
    void analyse_(const float* f);      // one kept frame → accumulate into acc_
    void quiet_(const float* f);        // one quiet frame → the noise floor (pq_)
    void coarse_(SsbOffsetEstimate& e) const;   // pv_/pq_ → the coarse interval
    SsbOffsetEstimate finish_();        // acc_ → estimate, then clear the window

    Config c_;
    std::unique_ptr<RationalResampler> rs_;
    std::unique_ptr<RealFFT> fft_;
    std::vector<float> win_, buf_, rsOut_, fin_, logS_, white_, prefix_, acc_, cur_;
    std::vector<float> blockFrames_, blockEnergy_;   // the current selection block
    std::vector<float> ds_;
    std::vector<int> f0c_;
    std::vector<std::complex<float>> spec_;
    std::vector<float> sum_, cnt_;
    std::vector<double> pv_, pq_; // the coarse stage: voiced / quiet power per kBandHz band (decayed like acc_)
    double nv_ = 0, nq_ = 0;      // …and their (decayed) frame counts
    double floorE_ = 0;           // the quietest frames' energy over the last seconds (see flushBlock_)
    double f0Recent_ = 0;         // the pitch of the voice speaking now (~0.3 s average; FAST's speaker change)
    int speakerChanges_ = 0;      // …how often the active memory changed voice (diagnostic)
    struct Memory {               // FAST: the OTHER voice's memory, parked while this one speaks (see analyse_)
        std::vector<float> acc;
        std::vector<double> pv;
        double nv = 0, weight = 0, f0Sum = 0, f0N = 0, snrSum = 0, snrW = 0;
        int frames = 0;
    } park_;
    double gammaPark_ = 1;        // the parked memory's (slower) per-frame decay
    double weight_ = 0;           // decayed count of the frames in acc_ (== used_ when memoryVoicedSec is 0)
    double gamma_ = 1;            // per-voiced-frame decay (memoryVoicedSec)
    int framesTotal_ = 0;         // voiced frames since reset (the minFrames floor)
    int bufFill_ = 0, nBlock_ = 0, nd_ = 0;
    int used_ = 0;                // voiced frames in the current window
    long long windowSamples_ = 0; // input (internal-rate) samples since the window began
    double cpuSec_ = 0, snrSum_ = 0, snrW_ = 0, f0Sum_ = 0;   // f0Sum_: each voiced frame's pitch, summed…
    double f0N_ = 0;              // …over this many frames (decayed with memoryVoicedSec)
    SsbOffsetEstimate last_;
    int count_ = 0;
};

/** ★ The frequency shift itself: x → Re{ analytic(x) · e^{j2π·shift·t} }, every component moved by
 *  `shiftHz` (a single-sideband shift — a plain mix would also make a mirror image). Hilbert FIR
 *  (Blackman-windowed, 191 taps at 16 kHz: image below -40 dB from 250 Hz to 7.7 kHz) + a phase-
 *  continuous rotator, so changing the shift mid-stream does not click. The output is delayed by
 *  the filter's group delay ((taps-1)/2 samples) whatever the shift, including zero — so switching
 *  the correction on never jumps the audio in time. */
class SsbShifter {
public:
    explicit SsbShifter(double rate, int taps = 191);
    void setShift(double hz);
    double shift() const { return shiftHz_; }
    void process(float* x, int n);      // in place
    void reset();
private:
    double rate_, shiftHz_ = 0;
    int K_;
    std::vector<float> h_;              // Hilbert taps (odd ones only are non-zero)
    std::vector<int> hk_;               // window index of each non-zero tap
    std::vector<float> hc_;             // …and its coefficient
    std::vector<float> hist_;           // [K-1 history][block]
    double ph_ = 0, dph_ = 0;
};

/** ★★ SSB AUTO-TUNE FOR THE RECOGNISER FEED (never what the listener hears).
 *  Runs the SLOW estimator on the audio and shifts it back when — and only when — the estimate is
 *  believable: |offset| ≤ kMaxCorrectHz, confidence over the gate, and TWO CONSECUTIVE gated
 *  windows within kAgreeHz of each other. A window that fails the gate changes nothing (the dial has
 *  not moved — the voice just went quiet or the other station spoke). Reset on retune/mode change. */
class SsbAutoTune {
public:
    /** ★ ±1000 Hz (was ±250 while the estimator searched only ±300): the two-stage estimate reaches ±1200 and its SLOW
     *  gate passed no wrong window on the off-air VOLMET clips at ±1000 (test-ssb-offset-real). Two agreeing windows
     *  are still required — 20 s of voice — so a single wrong one never moves the recogniser's audio. */
    static constexpr float kMaxCorrectHz = 1000.0f;
    static constexpr float kAgreeHz = 15.0f;
    static constexpr float kDeadbandHz = 10.0f;   ///< smaller agreed offsets are left alone
    static constexpr float kRefineHz = 3.0f;      ///< an agreed offset this close to the applied one changes nothing
    explicit SsbAutoTune(double rate);
    ~SsbAutoTune();
    /** +1 USB, -1 LSB, 0 = not SSB (passes the audio through, delayed like the shift, estimator idle). */
    void setSideband(int sb);
    int sideband() const { return sb_; }
    void reset();                        ///< retune: forget the offset and any pending window
    void process(float* x, int n);       ///< estimate + shift in place
    /** The shift applied to the AUDIO, Hz (−offset). 0 = none. */
    float appliedHz() const { return applied_; }
    const SsbOffsetEstimate& lastEstimate() const { return est_->last(); }
    int estimates() const { return est_->count(); }
    int corrections() const { return corrections_; }
    double cpuSeconds() const { return est_->cpuSeconds(); }
private:
    std::unique_ptr<SsbOffsetEstimator> est_;
    SsbShifter shift_;
    int sb_ = 0;
    bool havePrev_ = false;
    float prev_ = 0, applied_ = 0;
    int corrections_ = 0;
};

/** ★ THE RECOGNISER'S FEED, both platforms: any input rate → 16 kHz mono through a proper anti-
 *  aliased polyphase resampler (RationalResampler — the Android tap's linear interpolator had
 *  none, so a wideband FM or DAB feed folded its top octave into the speech band), then SSB
 *  auto-tune. One instance per tap thread. */
class RecogniserFeed {
public:
    static constexpr int kOutRate = 16000;
    RecogniserFeed();
    ~RecogniserFeed();
    /** Returns the number of 16 kHz samples written to `out` (needs maxOut(n, inRate)). */
    int process(const float* in, int n, int inRate, float* out);
    static int maxOut(int n, int inRate) { return (int)((long long)n * kOutRate / (inRate > 0 ? inRate : 1)) + 8; }
    /** Thread-safe: stores the request; the feeding thread applies it (and resets on any change). */
    void requestTuning(int sideband, double dialHz);
    SsbAutoTune& tune() { return tune_; }
    /** RMS of the last processed block (16 kHz), for the stall watchdog's "is audio flowing". */
    float lastRms() const { return rms_; }
private:
    std::unique_ptr<RationalResampler> rs_;
    int rate_ = 0;
    SsbAutoTune tune_;
    std::atomic<int> reqSb_{0};
    std::atomic<long long> reqDial_{0};
    std::atomic<int> reqGen_{0};
    int appliedGen_ = 0;
    float rms_ = 0;
};

}  // namespace vibedsp

// ── C API (Swift via the bridging header, Kotlin via JNI) ─────────────────────────────────────────
// ★ Mirrored by hand in ios/VibeSDR/VibeSsbTune.h (Swift cannot read this C++ header). Keep the two
//   in step: same names, same argument order.
extern "C" {
void* vibe_rxfeed_create(void);
void  vibe_rxfeed_destroy(void* h);
/** sideband +1 USB, -1 LSB, 0 other. Any change of sideband OR dial resets the estimate. Any thread. */
void  vibe_rxfeed_set_tuning(void* h, int sideband, double dialHz);
/** Mono float in at inRate → 16 kHz float out. Returns samples written (≤ outCap). Feeding thread only. */
int   vibe_rxfeed_process(void* h, const float* in, int n, int inRate, float* out, int outCap);
/** The correction as a DIAL change, Hz (USB: +offset, LSB: −offset), 0 when none. Feeding thread. */
float vibe_rxfeed_dial_correction(void* h);
/** RMS of the last processed block. Feeding thread. */
float vibe_rxfeed_rms(void* h);
/** Last estimate: out[0]=offsetHz out[1]=confidence out[2]=voicedSeconds out[3]=passes(0/1)
 *  out[4]=estimates so far out[5]=corrections so far out[6]=audio shift applied. Returns 7. */
int   vibe_rxfeed_stats(void* h, float* out7);
}
