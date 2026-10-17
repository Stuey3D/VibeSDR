// ★★★ BINAURAL TUNING — the audio leans toward the ear on the side the signal lies, and comes back to the
//     middle of the head as you tune onto it (V12).
//
// Stuart, 2026-10-16: "If you are below the signal and need to tune up, signal to the right of the VFO, the
// audio will pan to the right ear and pan back to centre the closer you get to being on tune, above it and vice
// versa." … "the left ear still gets audio but made to sound like you are physically further away."
//
// ★★ SPATIALISE, DON'T BALANCE. A balance control that just turns one side down sounds like a fault in the
//    headphones. A real source off to one side reaches the far ear (a) QUIETER, (b) LATER — up to ~0.6 ms, the
//    width of a head — and (c) DULLER, because the head shadows the treble. All three are applied to the FAR ear
//    only, all three scale with how far off tune you are, and all three ease to nothing inside a dead zone
//    (~±15 Hz) so on-tune audio sits centre-head and does not wander. The far ear is never silenced: at most
//    10 dB down (BinauralSpatialiser::kMaxDropDb).
//
// ★★ WHERE THE OFFSET COMES FROM is per mode, and mostly NOT here:
//      SSB (USB/LSB)  — HERE: the shared voice-harmonic estimator (ssb_offset.h), FAST profile, run on its own
//                       worker thread, its raw answers smoothed by a running median. The audio is the only
//                       place an SSB mistuning shows — there is no carrier to look at.
//      everything else — the CALLER (the app's JS, from the spectrum it already draws): AM/SAM carrier peak, CW
//                       peak, NFM/FM/WFM power centroid, each against the passband centre. Passed in with
//                       setOffset(). See src/services/binauralOffset.ts for why CW is measured there and not as
//                       "tone vs pitch" here.
//      WFM            — active only while MONO: it fades out as stereo locks (setStereoHint), with hysteresis and
//                       a slow crossfade, so a flickering pilot near lock cannot make it alternate.
//      DAB            — never (the caller sends Source::None).
//
// ★★ THREAD RULES — the audio thread is never made to wait (Network > Audio > Spectrum > Decoders):
//    • every setter may be called from ANY thread; it only stores atomics.
//    • process*() runs on the audio thread. Per sample it is a one-pole, two short delay-line reads and a
//      few multiplies; the exponentials are evaluated once per 16-sample sub-block and ramped across it.
//    • the SSB estimator is a DECODER and lives on its own low-priority worker thread. The audio thread hands
//      it samples through a lock-free single-producer ring, and when the ring is full the samples are DROPPED
//      (a late estimate is fine; a late audio buffer is not).
//
// ★ C API at the bottom — Swift holds it via ios/VibeSDR/VibeBinaural.h (a hand-kept mirror), Kotlin via JNI
//   (android/app/src/main/cpp/vibe_binaural_jni.cpp).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace vibedsp {

class SsbOffsetEstimator;

/** Offset (signed Hz, + = signal ABOVE the dial → tune up → lean RIGHT) to a pan target in −1…+1.
 *  0 inside ±deadHz, then a smoothstep to ±1 at ±fullHz — eased at both ends, so neither entering the
 *  dead zone nor reaching full scale has a corner the ear can hear as a jolt. */
float binauralPanFor(float offsetHz, float deadHz, float fullHz);

/** ★ THE EFFECT. Stereo in place (planar L/R). `panTarget` is where the image should lean: + = toward the
 *  RIGHT ear (the LEFT ear is the far one). The pan is smoothed here (time constant kPanTauSec), so a target
 *  that jumps — an SSB estimate arrives about once a second — glides; no zipper, no click. */
class BinauralSpatialiser {
public:
    static constexpr float kMaxDropDb = 10.0f;    ///< far-ear level drop at full pan — never silent
    static constexpr float kMaxItdSec = 0.0006f;  ///< far-ear delay at full pan (≈ the width of a head)
    static constexpr float kShelfHz = 1500.0f;    ///< head shadow: the far ear loses treble above about here…
    static constexpr float kShelfCut = 0.5f;      ///< …by up to this much (−6 dB) at full pan
    static constexpr float kPanTauSec = 0.12f;    ///< pan smoothing. ★ Also bounds the delay's slew, and with it
                                                  ///  the Doppler a moving delay makes: 0.6 ms over ~0.12 s ≈ 0.5 %
    static constexpr int kSub = 16;               ///< sub-block over which the derived parameters are ramped

    void process(float* L, float* R, int n, double rate, float panTarget);
    void reset();
    float pan() const { return pan_; }

private:
    struct Ear {                       // one ear's far-ness treatment
        float g = 1, shelf = 1, delay = 0;   // current (end of last sub-block) parameters
        float lp = 0;                  // the head-shadow low-pass state
        std::vector<float> ring;       // delay line
    };
    void configure_(double rate);
    static void params_(float far, double fs, float& g, float& shelf, float& delay);
    void ear_(Ear& e, float* x, int n, float g1, float sh1, float d1);

    double fs_ = 0;
    float pan_ = 0;
    float lpB_ = 0;                    // one-pole coefficient for kShelfHz
    int mask_ = 0, w_ = 0;             // shared ring write index (both ears advance together)
    Ear l_, r_;
};

/** ★★ THE CONTROLLER: which offset, how much pan, the WFM stereo crossfade, the SSB worker. One per output. */
class BinauralTuning {
public:
    enum Source : int { None = 0, Given = 1, SsbUsb = 2, SsbLsb = 3 };
    /** WFM stereo, from the caller: not a stereo mode / mono, locked, or unknown (measure the audio itself). */
    enum Stereo : int { Mono = 0, Locked = 1, Unknown = 2 };

    // ── SSB smoothing (FAST profile) ──
    static constexpr int   kMedianOf = 5;          ///< running median over the last this-many accepted answers…
    /** …published once at least this many are in. ★ TWO, not three: FAST gives an answer per ~2 s of real
     *  speech (measured on the synthetic voice: 6 in 12 s), so a third would hold the first lean back ~6 s —
     *  too slow for a knob that is turning. Two that AGREE is the same rule SsbAutoTune uses. */
    static constexpr int   kMedianMin = 2;
    static constexpr float kAgreeHz = 40.0f;       ///< …and a MAJORITY of them (at least two) lie this close to it
    static constexpr float kSsbRangeHz = 300.0f;   ///< the estimator searches ±300 Hz; beyond is not an answer
    static constexpr float kSsbStaleSec = 8.0f;    ///< no accepted answer for this long → back to centre
    /** ★ A dial change smaller than this is not a move: radiod stamps frames with ±1 Hz of jitter, and every
     *  "move" restarts the estimator — it would never finish one. (Mirrored by BIN_DIAL_MOVE_HZ in JS.) */
    static constexpr double kDialMoveHz = 2.0;
    /** ★ FAST's own gate, as a scale on the SLOW gate (SsbOffsetEstimator::gateFor). PROVISIONAL — measured
     *  on synthetic voices in vibeserver/test-binaural.cpp, NOT on the off-air clips (VIBE_SSB_CLIPS was not
     *  available when this was written). ssb_offset.h asks for it to be calibrated on them: do that before
     *  trusting the number. */
    static constexpr float kFastGateScale = 0.8f;

    // ── WFM stereo hand-off ──
    static constexpr float kStereoOnHoldSec = 0.8f;   ///< stereo must hold this long before the aid fades out
    static constexpr float kStereoOffHoldSec = 1.5f;  ///< mono must hold this long before it fades back in
    static constexpr float kStereoFadeSec = 1.5f;     ///< the crossfade itself, either way
    static constexpr float kSideOnRatio = 0.03f;      ///< Unknown: L−R over L+R power above this = stereo (−15 dB)
    static constexpr float kSideOffRatio = 0.01f;     ///< …and below this = mono (−20 dB): hysteresis in the measure too

    /** startWorker=false: no SSB worker thread — the tests turn the handle with pumpSsbForTest(). */
    explicit BinauralTuning(bool startWorker = true);
    ~BinauralTuning();
    BinauralTuning(const BinauralTuning&) = delete;
    BinauralTuning& operator=(const BinauralTuning&) = delete;

    // Any thread:
    void setEnabled(bool on);
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    /** The offset source, and the scale: 0 pan inside ±deadHz, full pan at ±fullHz. A change of source
     *  forgets the SSB estimate. */
    void setSource(int source, float deadHz, float fullHz);
    /** Source::Given: the caller's offset (signed Hz). valid=false → back to centre. */
    void setOffset(float hz, bool valid);
    void setStereoHint(int hint);
    /** The dial, Hz. A change moves the held SSB answer by the same amount (the signal stayed where it was;
     *  the dial moved), so the lean follows the knob at once instead of waiting a second for a new estimate. */
    void setDial(double hz);

    // Audio thread:
    void processPlanar(float* L, float* R, int n, int rate);
    /** int16 in (1 or 2 channels, interleaved) → int16 STEREO interleaved out (2·frames). Returns frames. */
    int processInt16(const int16_t* in, int inCh, int frames, int rate, int16_t* outStereo);

    /** [enabled, source, offsetHz, offsetValid, pan, stereoWeight, ssbEstimates, ssbAccepted, ringDrops]. */
    int stats(float* out, int cap) const;

    // Exposed for the tests: the SSB answer the worker last published, and a synchronous pump of the worker's
    // job (the tests feed audio and turn the handle themselves instead of racing a thread).
    bool ssbOffset(float& hz) const;
    void pumpSsbForTest();

private:
    float targetPan_(int n, int rate, const float* L, const float* R);
    void ssbFeed_(const float* L, const float* R, int n, int rate);
    void ensureWorker_();
    void workerLoop_();
    void ssbStep_();                  // one pass of the worker's job (worker thread, or a test)
    void ssbAccept_(float signalHz);

    BinauralSpatialiser fx_;
    std::atomic<bool> enabled_{false}, resetFx_{false};
    std::atomic<int> source_{None};
    std::atomic<float> dead_{15.0f}, full_{300.0f};
    std::atomic<float> given_{0.0f};
    std::atomic<bool> givenValid_{false};
    std::atomic<int> stereoHint_{Mono};
    std::atomic<double> dial_{0.0};
    std::atomic<int> gen_{0};         // bumped on a source change: the worker starts over

    // WFM stereo state — audio thread only
    bool stereoState_ = false;
    double stereoHeld_ = 0;           // seconds the raw reading has disagreed with stereoState_
    float stereoW_ = 1;               // the aid's weight: 1 = full, 0 = faded out under stereo
    float sideRatio_ = 0;             // smoothed L−R / L+R power (Unknown hint only)
    bool sideStereo_ = false;
    std::atomic<float> panPub_{0.0f}, stereoWPub_{1.0f};   // the audio thread's state, published for stats

    // SSB: audio thread → worker ring (single producer, single consumer, lock-free)
    static constexpr uint32_t kRing = 1u << 16;
    std::vector<float> ring_;
    std::atomic<uint32_t> wr_{0}, rd_{0};
    std::atomic<int> ringRate_{0};
    std::atomic<uint32_t> drops_{0};

    // SSB: worker-owned
    std::unique_ptr<SsbOffsetEstimator> est_;
    int estRate_ = 0, workerGen_ = -1, workerSrc_ = None;
    double lastDial_ = 0; bool haveDial_ = false;
    std::vector<float> hist_;         // accepted answers, SIGNAL terms (sideband applied), newest last
    double lastAccept_ = 0;
    std::vector<float> chunk_;
    std::atomic<int> estimates_{0}, accepted_{0};
    std::atomic<float> ssbHz_{0.0f};
    std::atomic<bool> ssbValid_{false};

    std::thread worker_;
    std::atomic<bool> quit_{false}, workerUp_{false};
    bool startWorker_ = true;

    // int16 path scratch (audio thread)
    std::vector<float> sL_, sR_;
};

}  // namespace vibedsp

// ── C API (Swift via the bridging header, Kotlin via JNI) ─────────────────────────────────────────
// ★ Mirrored by hand in ios/VibeSDR/VibeBinaural.h (Swift cannot read this C++ header). Same names, same order.
extern "C" {
void* vibe_binaural_create(void);
void  vibe_binaural_destroy(void* h);
void  vibe_binaural_set_enabled(void* h, int on);
int   vibe_binaural_enabled(void* h);
/** source: 0 none, 1 given (setOffset), 2 SSB USB, 3 SSB LSB. */
void  vibe_binaural_set_source(void* h, int source, float deadHz, float fullHz);
void  vibe_binaural_set_offset(void* h, float hz, int valid);
/** 0 mono / not a stereo mode, 1 stereo locked, 2 unknown (measure the audio). */
void  vibe_binaural_set_stereo(void* h, int hint);
void  vibe_binaural_set_dial(void* h, double hz);
/** Audio thread. Planar stereo float, in place. */
void  vibe_binaural_process_planar(void* h, float* L, float* R, int n, int rate);
/** Audio thread. int16 interleaved (inCh 1 or 2) → int16 stereo interleaved (2·frames). Returns frames. */
int   vibe_binaural_process_i16(void* h, const int16_t* in, int inCh, int frames, int rate, int16_t* outStereo);
/** See BinauralTuning::stats. Returns the count written. */
int   vibe_binaural_stats(void* h, float* out, int cap);
}
