// ★★★ SSB OFFSET ESTIMATOR — how far a single-sideband voice is mistuned, from the voice itself.
//
// WHY. A voice on SSB is a comb of harmonics at k·f0. Mistune the dial by d Hz and every harmonic
// moves by the SAME d — the comb is no longer a set of multiples, it is k·f0 + d. People hear that
// as "Donald Duck"; the on-device speech recogniser hears it as a worse match: measured on a strong
// 80 m LSB contact (V12 research, 2026-10-16) ±100 Hz cost 0.78-0.82 transcript agreement and
// ±200 Hz 0.50-0.65. Shifting the audio back by the estimated d recovered 0.89-0.97.
//
// HOW (a port of the research rig's `auto` estimator, scratchpad ssb/rig/chain.py est_offset):
//   1. Decimate to 8 kHz. Cut 80 ms frames; in each block of audio keep the LOUDEST third (voice
//      is louder than the noise between words) — the voiced-frame selection.
//   2. Each kept frame: log spectrum, whitened (minus its own ~180 Hz moving average), so a
//      harmonic is a bump whatever the band's tilt.
//   3. Pitch-harmonic alignment: for every offset d (±300 Hz) the best comb k·f0 + d over the
//      frame's plausible pitches scores harmonics minus the mid-points between them. A frame that
//      fits no comb well is not voiced and is dropped.
//   4. Each frame's curve over d is z-scored and SUMMED over frames. A wrong d fits only frames
//      whose pitch happens to alias it (d mod f0), the true d fits all of them as the pitch moves
//      with intonation — so the true d is a SHARP peak and aliases are broad. A high-pass on the
//      summed curve keeps the sharp one.
//   5. Coarse to fine: the pitch is found per frame from the whitened spectrum's autocorrelation
//      (shift-invariant, so the mistuning does not disturb it) and refined to 0.5 Hz before the
//      comb search; the offset is searched in 2 Hz steps, then parabolic-refined.
//
// ★★ THE ANSWER COMES WITH A CONFIDENCE, AND A CALLER MUST GATE ON IT (SsbOffsetEstimate::passes).
//    The research found a whole clip reading +162 Hz at 6 dB SNR (3648.2 kHz LSB) and QSO windows that
//    jump ~±200 Hz (the other station off-frequency, or a pitch alias). `confidence` is the peak's
//    STRENGTH — the mean per-frame z-score at the winning offset, i.e. how firmly every voiced frame
//    agrees on it. The gate asks more of it the larger the offset, and beyond ±40 Hz also asks for a
//    clean signal (snrDb). Measured, with the clips and numbers, in vibeserver/test-ssb-offset-clips.cpp;
//    the reasoning is beside kGateBase in ssb_offset.cpp. ★ That 3648.2 kHz reading may in fact be RIGHT
//    — its voice starts ~300 Hz higher in the spectrum than a well-tuned station's — but at 6 dB nothing
//    can tell, and the recogniser makes nothing of that signal either way, so the gate refuses it.
//
// ★ PROFILES. SLOW — transcription: ~10 s of voiced audio per estimate (the loudest third of ~30 s
//   of speech), ~0.05-0.1 % of a desktop core. FAST — the binaural tuning aid (next feature): 40 ms
//   frames, an answer per ~0.6 s of voiced audio (1-2 s of speech), rougher; it answers "which way,
//   and roughly how far" while the knob turns. Only SLOW is used today (SsbAutoTune below).
//
//   ★ The gate (passes) was measured for SLOW. On FAST it leans toward small offsets, so the binaural
//     aid should smooth FAST's raw answers (a running median of the last few) and calibrate its own gate.
//     Measured on the mistune clips: ~107 answers per 150 s, raw medians +1/+48/+98/−47/−84 Hz for
//     truths 0/+50/+100/−50/−100 (test-ssb-offset-clips).
//   FAST for the binaural aid:
//     SsbOffsetEstimator est(16000, SsbOffsetEstimator::Profile::Fast);   // or the audio's own rate
//     if (est.push(buf, n)) {                      // true when a new estimate completed in this call
//         const auto& e = est.last();              // e.offsetHz (+ = audio HIGH), e.confidence, e.passes()
//     }                                            // est.reset() on a retune; SsbShifter moves audio by Hz
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
    bool  passes() const;         ///< the gate: see SsbOffsetEstimator::gateFor and the note in ssb_offset.cpp
};

class SsbOffsetEstimator {
public:
    enum class Profile { Slow, Fast };
    /** ★ THE GATE RISES WITH THE OFFSET (measured: test-ssb-offset-clips.cpp). A pitch alias sits a pitch
     *  (~70-250 Hz) away from the truth, so a LARGE offset is exactly where a wrong answer lives; it must be
     *  backed by stronger agreement than a small one. gateFor(d) = kGateBase + kGatePerHz·|d|. */
    static const float kGateBase, kGatePerHz;
    /** …and beyond ±kLargeHz the window's speech-to-gap ratio must be at least kLargeMinSnrDb. */
    static const float kLargeHz, kLargeMinSnrDb;
    static float gateFor(float offsetHz);

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
        float  dMaxHz = 300;         ///< offsets searched: ±dMax
        float  dStepHz = 2;
        float  f0Lo = 70, f0Hi = 400;///< plausible voice pitch
        float  combLoHz = 120, combHiHz = 2000;   ///< harmonics scored inside this band
        float  voicedScore = 0.5f;   ///< a frame whose best comb scores below this is not voiced
        int    minFrames = 60;       ///< fewer voiced frames than this is never believed (noise can look sure)
        bool   fullPitchSearch = false; ///< every pitch 1 Hz apart (the rig's way, ~8x the cost) instead of the
                                        ///< autocorrelation's candidates refined to 0.5 Hz — kept for the bench
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
    /** Voiced audio collected toward the next estimate. */
    float pendingVoicedSeconds() const;

    /** One-shot over a whole buffer: every frame's evidence summed into ONE estimate. Offline use
     *  (tests, the research rig); a live caller uses push(). */
    static SsbOffsetEstimate estimate(const float* x, int n, double inRate, Profile p = Profile::Slow);

    /** CPU spent in analysis (spectra + comb search), seconds — for the cost report. */
    double cpuSeconds() const { return cpuSec_; }

private:
    void frameReady_(const float* f);
    void flushBlock_();
    void analyse_(const float* f);      // one kept frame → accumulate into acc_
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
    int bufFill_ = 0, nBlock_ = 0, nd_ = 0;
    int used_ = 0;                // voiced frames in the current window
    long long windowSamples_ = 0; // input (internal-rate) samples since the window began
    double cpuSec_ = 0, snrSum_ = 0;
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
    static constexpr float kMaxCorrectHz = 250.0f;
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
