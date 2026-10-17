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
//    (~±15 Hz; ±50 Hz on SSB, plus a hysteresis — see kSsbEnterHz) so on-tune audio sits centre-head and does not
//    wander. The far ear is never silenced: at most
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
//      SSB, COARSE    — the CALLER again, from the waterfall (binauralOffset.ts ssbCoarseCarrier): where the
//                       voice's band starts. Reaches a few kHz where the estimator reaches ±300 Hz, so it is
//                       what makes the lean trigger when you ARRIVE off a voice. It only steps in where the
//                       estimator cannot answer — see setSsbHint() for the hand-off.
//
// ★★ ONE BEHAVIOUR, TWO CUES (V12 — through build 476 it was two exclusive STYLES, GUIDE or SPREAD). Stuart,
//    2026-10-17, on 476: "I thought we were combining both binural methods so that when on signal it presents in
//    mono as it would before and then pan left and right based on signal and audio response when off tune."
//    • ON TUNE (inside the dead zone) the audio is UNTOUCHED: no lean, no spread.
//    • OFF TUNE the guide lean (everything above) AND the spread (BinauralSpread: each AUDIO frequency placed by
//      where it lies in the passband, higher RF to the right in every sideband) fade in TOGETHER, both driven by
//      the SAME off-tune weight w = |binauralPanFor(offset)| — 0 in the dead zone, 1 at full scale. The lean says
//      which way to tune; the spread spreads what is in the passband around it.
//    • The spread exists only where the audio IS a slice of spectrum — SSB and CW (setSpread's kind, stated by the
//      caller per mode). AM, FM and DAB get the lean alone.
//    • The SSB estimator is fed whatever the spread is doing: on SSB it is the offset source that drives w.
//    ★ The spread bank has one frame of latency (10.7 ms at 48 kHz), so on SSB/CW the WHOLE path — on tune too —
//      runs through a matching delay: on tune it is the input delayed by exactly that frame, bit for bit, and as
//      w rises the output moves from that delayed copy to the bank's, which is time-aligned with it. The latency
//      is CONSTANT while binaural is on in SSB/CW, so tuning on and off never shifts the timing. See BinauralSpread.
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

#include "vibedsp.h"   // RealFFT, cf32 (SPREAD's filter bank)

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

/** ★★ SPREAD — each audio frequency placed by where it lies in the passband (Stuart, 2026-10-17: "if say you are
 *  halfway between 2 signals you get one in the left and one in the right" · "so a crowded band gives you stereo
 *  separation of the signals").
 *
 *  ★★ AN OVERLAP-ADD FILTER BANK. Short FFT frames (≈ 94 Hz bins: 512 points at 48 kHz, scaled with the rate),
 *     50 % overlap, sqrt-Hann on analysis AND synthesis (their product, the Hann, overlap-adds to exactly 1, so
 *     with every weight at 1 the output IS the input, delayed). Each bin of the mono input is resynthesised
 *     twice, once per ear, through a complex weight: that ear's far-ness treatment at that bin's frequency —
 *     the SAME three cues as BinauralSpatialiser (level ≤ kMaxDropDb, delay ≤ kMaxItdSec as a phase turn, and
 *     the head-shadow shelf's magnitude at that frequency). Not hard L/R: nothing is ever silenced.
 *     ✗ Not a bank of IIR band-passes: crossovers that sum flat need careful design per band count, and a pan
 *       that varies smoothly with frequency is exactly what per-bin weights give for free.
 *     ★ 50 %, not 75 %, overlap: half the FFTs (cost matters on the 1 GB phones), and it is enough because the
 *       weights change only as slowly as the lean does and are SMOOTH across frequency, so each ear's filter is
 *       short (≲ 2 ms against a 10.7 ms frame) and the circular wrap that 75 % would hide is already negligible.
 *  ★★ LATENCY AND THE DRY PATH. The bank is one frame late: N samples, 10.7 ms at 48 kHz (latencySamples()). On
 *     tune the spread is nothing, so the output is the INPUT DELAYED BY EXACTLY N — a plain delay line per ear, not
 *     the bank, so it is the original audio bit for bit (Stuart: "presents in mono as it would before"). Off tune
 *     the output moves from that delayed copy to the bank's output; the two are time-aligned, so the move is a pure
 *     change of spread, never a comb or a jump in timing.
 *     ✗ NOT "dry with no delay on tune, the bank off tune": the two would be 10.7 ms apart, so every crossing of
 *       the dead zone would slip the timing by a frame and comb-filter the crossfade (a 94 Hz-spaced flanger) —
 *       and you cross it every time you tune. A constant 10.7 ms is inaudible on a radio stream; a moving one is not.
 *     ★ The delay begins when the bank engages (binaural switched on in SSB/CW, or a change into SSB/CW), with one
 *       kWetSec crossfade from the undelayed input, and ends the same way. Between, it never moves.
 *     ★ On tune the bank does no FFT at all on SSB (its overlap-add is kept alive in the time domain, so the first
 *       frame off tune joins seamlessly); CW still takes the forward FFT, to keep measuring the note.
 *
 *  ★★ THE SCALE IS FIXED Hz PER POSITION, from the DEFAULT SSB passband (the caller passes it: 50-2700 Hz, the
 *     app's MODE_BANDWIDTHS.usb). Its low edge is full LEFT, its high edge full RIGHT, its middle (1375 Hz) the
 *     centre, linear in Hz between — and it does NOT follow the filter, so a signal keeps its place when you widen
 *     or narrow. Beyond the default edges it clamps at full. ("you wouldnt hear them anyway if the passband is too
 *     narrow" — a narrow filter simply shows the middle of the picture.)
 *     • USB: higher audio IS higher RF → higher audio to the right.
 *     • LSB: higher audio is LOWER RF, so the map is mirrored: higher RF stays to the right in both sidebands.
 *       (Stuart confirmed it on 476: "in LSB if i go off tune downwards it becomes deeper sounding … Obviously USB
 *       will be the reverse of this." — ✗ do not flip it.)
 *     • CW: the same Hz-per-position, centred on the CW note so the tuned signal is dead centre. The note is not
 *       ours to know (VibeServer beats at 600 Hz, a Kiwi at 500, others their own, and none reports it — see
 *       binauralOffset.ts), so it is MEASURED: the middle of the band the receiver's noise fills in the audio
 *       (cwCentreHz_). Every backend centres its CW filter on its own note, so the passband's middle IS the note.
 *       Until the noise has been heard, kCwDefaultHz.
 *       ★ The same Hz-per-position means a 400-500 Hz CW filter shows only the MIDDLE of the picture: a signal 200 Hz
 *         off the note leans a fraction of a dB. That is the rule as asked ("you wouldnt hear them anyway if the
 *         passband is too narrow"); widen the filter and the CW band spreads out like SSB.
 *  ★ THE PAN LAW: far-ness = |position|^kShape. A voice's energy sits low in the passband (most of it a few hundred
 *    Hz either side of 1 kHz), so a straight line would hold one voice off to the left. The power law keeps the
 *    middle of the picture gentle and still reaches full treatment at the edges, where a second signal lives.
 *  ★★ HOW MUCH: `amount` (0…1) — the controller passes the off-tune weight w, the SAME number that sets the lean.
 *     It is smoothed here with the lean's own time constant (BinauralSpatialiser::kPanTauSec), so the spread and
 *     the lean grow and shrink together. A change of sideband glides the spread to nothing (kSwapSec), swaps the
 *     map, and glides it back; the delay stays put throughout.
 *  Audio thread only. */
class BinauralSpread {
public:
    enum Kind : int { Off = 0, Usb = 1, Lsb = 2, CwUpper = 3, CwLower = 4 };
    static constexpr float kShape = 1.5f;          ///< far-ness = |position|^kShape (see THE PAN LAW)
    static constexpr float kSwapSec = 0.15f;       ///< a change of map: the spread out, then back in, over this each way
    static constexpr float kWetSec = 0.04f;        ///< engaging / disengaging: undelayed input ↔ the delayed path
    /** The delayed copy → the bank's output by the time the spread reaches this amount (a crossfade between two
     *  ALIGNED signals that differ only by the spread, so it is only there for a stereo input's sake). */
    static constexpr float kMixFullAt = 0.05f;
    static constexpr float kCwDefaultHz = 600.0f;  ///< CW note before the passband has been measured
    static constexpr float kBinHz = 94.0f;         ///< target bin width: N = the power of two giving ≤ this

    BinauralSpread();
    ~BinauralSpread();
    BinauralSpread(const BinauralSpread&) = delete;
    BinauralSpread& operator=(const BinauralSpread&) = delete;

    /** Stereo in place. `kind` Off = glide out and bypass (AM, FM, DAB; or binaural switched off). `amount` 0…1:
     *  how much spread (0 = the input, delayed). `edgeLo/HiHz`: the default SSB passband's audio edges (full left /
     *  full right before any mirroring). */
    void process(float* L, float* R, int n, double rate, int kind, float amount, float edgeLoHz, float edgeHiHz);
    /** Out of the path at once (binaural switched back on: the next use warms up from silence, never from a stale
     *  frame of whatever played before). */
    void reset() { running_ = false; amt_ = 0; eff_ = 0; map_ = 1; wet_ = 0; }
    /** Is the bank in the signal path (running, or gliding in or out)? */
    bool running() const { return running_; }
    /** The spread actually applied, 0…1 (the smoothed amount, times the map-swap glide). */
    float amount() const { return eff_; }
    float wet() const { return wet_; }
    float cwCentreHz() const { return cwCentre_; }
    int latencySamples() const { return running_ ? n_ : 0; }

    /** The per-ear far-ness at an audio frequency for a kind and scale (the pan law alone: + = the LEFT ear is far,
     *  i.e. the sound sits RIGHT). Exposed for the tests. */
    static float panAt(float audioHz, int kind, float centreHz, float halfHz);

private:
    void configure_(double rate);
    void frame_();
    void weights_();
    void cwMeasure_();
    void cwForget_();

    double fs_ = 0;
    int n_ = 0, h_ = 0, pos_ = 0, warm_ = 0, dpos_ = 0;
    bool running_ = false;
    int kind_ = Off;                 // the map in use (changes only at zero spread)
    float lo_ = 50, hi_ = 2700;
    float amt_ = 0;                  // the smoothed amount (follows the caller's w)
    float map_ = 1;                  // the map-swap glide: 1 normally, through 0 while the map changes
    float eff_ = 0;                  // amt_ × map_: what the weights are built for
    float wet_ = 0;                  // undelayed input (0) ↔ the delayed path (1)
    float wAmt_ = -1;                // the amount the weights were built for (-1 = rebuild)
    float cwCentre_ = kCwDefaultHz;
    bool cwMeasured_ = false;
    int cwHops_ = 0;
    std::unique_ptr<RealFFT> fwd_, inv_;
    std::vector<float> win_, win2_, in_, frame_buf_, outL_, outR_, tmp_, dryL_, dryR_;
    std::vector<cf32> spec_, specL_, specR_, wL_, wR_;
    std::vector<float> psm_, floor_;  // CW: smoothed power and its slow floor, per bin (dB)
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
    /** No accepted answer for this long → back to centre (estimator answers only; a reckoned one never goes stale).
     *  ★★ TWO MINUTES, NOT 8 s (Stuart, 2026-10-17: "actually I cant trigger it again"). The dial can only move an
     *  answer that is HELD — so when 8 s of a pause between overs dropped even the on-tune lock, turning off it
     *  then did nothing until several more seconds of on-tune speech re-locked it. A slightly old lock costs far
     *  less than none: a newer accepted answer replaces it, and only long silence lets it go. */
    static constexpr float kSsbStaleSec = 120.0f;
    /** ★★★ HOW FAR A DIAL-RECKONED ANSWER IS HELD (Stuart, 2026-10-17, on V12 474: "if i am on a signal then tune off
     *  it pans but then I wait just off and it centres again even though I can hear badly modulated voices" — "I
     *  use a 500Hz step rate"). One step is beyond the estimator's ±300 Hz, so nothing could ever confirm the lean,
     *  the 8 s stale timer dropped it, and a held answer past 4 × 300 Hz was thrown away. An answer moved by the
     *  dial is not a guess — it is exactly how far the knob turned from a voice we had locked — so it is held to
     *  the edge of an SSB passband and never goes stale while the dial stays put. */
    static constexpr float kSsbHoldHz = 4000.0f;
    /** ★★★ AFTER A DIAL MOVE THE KNOB'S FIGURE IS IN CHARGE (Stuart: "if i switch to 100Hz it pans but then centers
     *  even if i am still off tune"). 100 Hz is inside the estimator's reach, but FAST leans toward SMALL offsets
     *  (ssb_offset.h says so), so its answers on the mistuned voice came back near 0 and out-voted the exact
     *  reckoned figure within seconds. While reckoned, an estimator answer may only REFINE the held one — it is
     *  accepted if it lies within this of it, ignored otherwise — until the dial moves again. */
    static constexpr float kReckonTrustHz = 60.0f;
    /** ★ A dial change smaller than this is not a move: radiod stamps frames with ±1 Hz of jitter, and every
     *  "move" restarts the estimator — it would never finish one. (Mirrored by BIN_DIAL_MOVE_HZ in JS.) */
    static constexpr double kDialMoveHz = 2.0;
    /** ★★★ HYSTERESIS ON AN ESTIMATOR LEAN (Stuart on 477: "deep male voice in the right, higher pitched female voice
     *  in the left when they are talking to each other and the panning left and right constantly" — on LSB, low
     *  audio to the right IS the spread's map, so the spread was switching ON during an on-tune QSO). Two things
     *  made it: SSB used AM's ±15 Hz dead zone, but a voice cannot be measured that finely and the stations of a QSO
     *  sit a few tens of Hz apart; and every new speaker swings the median. Now the SSB dead zone is ±50 Hz (the
     *  caller's BIN_DEAD_SSB_HZ), and from CENTRED an estimator answer must pass this to START a lean; once leaning it
     *  lets go only inside the dead zone. Between the two, an answer changes nothing — the state it found stays.
     *  ★ A DIAL-RECKONED answer (or the waterfall cue, which has its own ±kCoarseMinHz rule) does NOT have to pass it:
     *    turning the knob off a locked voice leans at once, exactly as before — that figure is not a guess.
     *  ★ The lean and the spread share w, so the spread follows the same hysteresis: none on an on-tune QSO.
     *  PROVISIONAL (synthetic voices, no off-air clips): 90 Hz clears FAST's scatter on a voice and the ~30 Hz between
     *  QSO stations by a margin, and stays well inside the estimator's ±300 Hz reach. */
    static constexpr float kSsbEnterHz = 90.0f;
    /** ★★ HIGH VOICES COUNT FOR LESS (the same report: "its a deep male voice and a childrens radio club talking").
     *  A child's voice (F0 ~250-400 Hz) puts only 6-10 harmonics in a 2.7 kHz passband, and the comb k·f0 + d fits
     *  d ± f0 (and d ± f0/2) almost as well as d — and those aliases now fall INSIDE the ±300 Hz search. FAST was
     *  tuned on adult voices only. Above kHighF0Hz (SsbOffsetEstimate::f0Hz, the estimator's own pitch figure):
     *  1. the gate rises by kHighF0GatePerHz per Hz of pitch, up to ×kHighF0GateMax (×2 from 300 Hz up);
     *  2. while a held answer stands that a LOWER voice helped establish (lowVoiceHeld_), a high-voice answer more than
     *     kAgreeHz from it is ignored — it may refine that answer, never drag it. A high voice ALONE is not held to
     *     its own first answers that way (a wrong first lock would then defend itself for two minutes);
     *  3. a high-voice answer that sits where its own aliases of "on tune" would (within kAgreeHz of ±f0 or ±f0/2)
     *     is ignored unless it agrees with the held answer — with nothing held yet, a near-monotone child otherwise
     *     locks a pitch off tune before anyone else has spoken. The cost: a child mistuned by exactly half or one
     *     pitch gets no estimator answer (the dial and the waterfall cue still lean); never the wrong ear.
     *  PROVISIONAL — chosen on synthetic voices (vibeserver/test-binaural.cpp §10), no off-air clips. Rule 1 alone,
     *  measured over 32 FAST windows each: a near-monotone 300 Hz voice mistuned −150 Hz had 28 aliases pass the
     *  gate, 10 with it; a 250 Hz one on tune 16 → 16 (that case is rule 3's). Without rule 3 a monotone 250 Hz
     *  child speaking first in an on-tune QSO leaned at 5 s (pan 0.74); with it, never. Calibrate on real children's
     *  voices before trusting the numbers. */
    static constexpr float kHighF0Hz = 200.0f;
    static constexpr float kHighF0GatePerHz = 0.01f;
    static constexpr float kHighF0GateMax = 2.0f;
    /** ★ FAST's own gate, as a scale on the SLOW gate (SsbOffsetEstimator::gateFor). PROVISIONAL — measured
     *  on synthetic voices in vibeserver/test-binaural.cpp, NOT on the off-air clips (VIBE_SSB_CLIPS was not
     *  available when this was written). ssb_offset.h asks for it to be calibrated on them: do that before
     *  trusting the number. */
    static constexpr float kFastGateScale = 0.8f;
    /** ★★ THE COARSE CUE'S OWN ERROR. The waterfall puts the carrier ~300 Hz below where the voice's band starts,
     *  but transmitters cut their low end anywhere from ~100 to ~400 Hz, so the cue is good to about ±150 Hz. A
     *  coarse answer smaller than this cannot tell "on tune" from "just off", and it never leans on its own. */
    static constexpr float kCoarseMinHz = 150.0f;
    /** A beyond-reach answer the DIAL reckoned and a coarse cue within this of each other are the same voice: the
     *  knob's exact figure is kept, the cue's ±150 Hz is not allowed to replace it. */
    static constexpr float kCoarseAgreeHz = 250.0f;

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
    /** ★ Enabled, OR switched off but still gliding back to the untouched input (the lean to centre, the spread's
     *  delay out). A caller that skips process*() when this is false never cuts the effect off mid-lean or jumps
     *  the 10.7 ms delay — that would be a click. */
    bool active() const { return enabled() || activePub_.load(std::memory_order_relaxed); }
    /** The offset source, and the scale: 0 pan inside ±deadHz, full pan at ±fullHz. A change of source
     *  forgets the SSB estimate. */
    void setSource(int source, float deadHz, float fullHz);
    /** Source::Given: the caller's offset (signed Hz). valid=false → back to centre. */
    void setOffset(float hz, bool valid);
    void setStereoHint(int hint);
    /** The dial, Hz. A change moves the held SSB answer by the same amount (the signal stayed where it was;
     *  the dial moved), so the lean follows the knob at once instead of waiting a second for a new estimate. */
    void setDial(double hz);
    /** ★★ THE COARSE SSB CUE, from the waterfall: the voice's suppressed carrier as an ABSOLUTE RF frequency (Hz),
     *  valid=false when the caller sees no voice. Absolute, not an offset, so it cannot race setDial: the offset is
     *  always taken against the dial as it is NOW.
     *  ★★ THE HAND-OFF (worker thread, coarseStep_), in order of authority:
     *    1. A valid estimator/reckoned answer WITHIN ±kSsbRangeHz is never touched — the cue's ±150 Hz must not
     *       fight a fine answer the estimator can still refine.
     *    2. No answer: the cue seeds one if it says ≥ kCoarseMinHz — the arriving-off-tune case the estimator
     *       cannot reach. A COARSE-HELD answer then follows the cue, moves with the dial like any other, and is
     *       handed back the moment two estimator answers agree (they may REPLACE it, not just refine it: the
     *       estimator is the better witness once the voice is within its reach).
     *    3. A held answer BEYOND reach: kept if the cue agrees within kCoarseAgreeHz (the knob's figure is exact);
     *       replaced if the cue disagrees — a different voice now fills the passband — or let go if the cue puts a
     *       voice right on tune, so the estimator can look.
     *    The cue going invalid releases a coarse-held answer (the caller already holds its reading through a
     *    pause between overs). */
    void setSsbHint(double carrierHz, bool valid);
    /** ★★ THE SPREAD MAP for the current mode: BinauralSpread::Off (AM, FM, DAB — the lean alone) or
     *  Usb/Lsb/CwUpper/CwLower. NOT a style: how MUCH spread is always the off-tune weight w (none on tune).
     *  `edgeLo/HiHz` = the default SSB passband's audio edges (the fixed scale). Any thread. */
    void setSpread(int kind, float edgeLoHz, float edgeHiHz);

    // Audio thread:
    void processPlanar(float* L, float* R, int n, int rate);
    /** int16 in (1 or 2 channels, interleaved) → int16 STEREO interleaved out (2·frames). Returns frames. */
    int processInt16(const int16_t* in, int inCh, int frames, int rate, int16_t* outStereo);

    /** [enabled, source, offsetHz, offsetValid, pan, stereoWeight, ssbEstimates, ssbAccepted, ringDrops,
     *   spreadAmount, coarseHeld, spreadCwCentreHz]. */
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
    void ssbAccept_(float signalHz, float f0Hz);
    void coarseStep_();
    void release_();                  // drop the held SSB answer (worker)
    void leanUpdate_(bool firm);      // the SSB lean's hysteresis (worker): firm = the dial or the cue set this answer

    BinauralSpatialiser fx_;
    std::atomic<bool> enabled_{false}, resetFx_{false};
    std::atomic<int> source_{None};
    std::atomic<float> dead_{15.0f}, full_{300.0f};
    std::atomic<float> given_{0.0f};
    std::atomic<bool> givenValid_{false};
    std::atomic<int> stereoHint_{Mono};
    std::atomic<double> dial_{0.0};
    std::atomic<int> gen_{0};         // bumped on a source change: the worker starts over
    std::atomic<double> hintCarrier_{0.0};
    std::atomic<bool> hintValid_{false};
    std::atomic<int> spreadKind_{BinauralSpread::Off};
    std::atomic<float> spreadLo_{50.0f}, spreadHi_{2700.0f};
    BinauralSpread spread_;           // audio thread only
    std::atomic<float> spreadPub_{0.0f}, cwCentrePub_{0.0f};
    std::atomic<bool> activePub_{false};   // audio thread: still in the path (see active())
    bool glidingOut_ = false;              // audio thread: the last block ran switched off, still gliding out

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
    bool reckoned_ = false;   ///< the held SSB answer was moved by the dial since it was last measured (worker only)
    bool coarseHeld_ = false; ///< the held SSB answer came from the waterfall cue, not the estimator (worker only)
    std::atomic<bool> coarsePub_{false};
    std::vector<float> hist_;         // accepted answers, SIGNAL terms (sideband applied), newest last
    std::vector<float> histF0_;       // …and the pitch (SsbOffsetEstimate::f0Hz) of the voice that gave each
    bool lowVoiceHeld_ = false;       ///< the held answer was established with a voice at or under kHighF0Hz (worker)
    bool lean_ = false;               ///< the SSB lean's hysteresis state (worker): see kSsbEnterHz
    std::atomic<bool> leanPub_{false};///< …published for the audio thread
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
/** 1 while the caller should run process*(): enabled, or switched off and still gliding out (BinauralTuning::active). */
int   vibe_binaural_enabled(void* h);
/** source: 0 none, 1 given (setOffset), 2 SSB USB, 3 SSB LSB. */
void  vibe_binaural_set_source(void* h, int source, float deadHz, float fullHz);
void  vibe_binaural_set_offset(void* h, float hz, int valid);
/** 0 mono / not a stereo mode, 1 stereo locked, 2 unknown (measure the audio). */
void  vibe_binaural_set_stereo(void* h, int hint);
void  vibe_binaural_set_dial(void* h, double hz);
/** The waterfall's SSB carrier, ABSOLUTE Hz (valid 0 = none seen). See BinauralTuning::setSsbHint. */
void  vibe_binaural_set_ssb_hint(void* h, double carrierHz, int valid);
/** The mode's spread map: 0 none (AM, FM, DAB), 1 USB, 2 LSB, 3 CW (upper), 4 CW (lower). Edges = the default SSB
 *  passband (audio Hz). How much spread is the off-tune weight — none on tune. */
void  vibe_binaural_set_spread(void* h, int kind, float edgeLoHz, float edgeHiHz);
/** Audio thread. Planar stereo float, in place. */
void  vibe_binaural_process_planar(void* h, float* L, float* R, int n, int rate);
/** Audio thread. int16 interleaved (inCh 1 or 2) → int16 stereo interleaved (2·frames). Returns frames. */
int   vibe_binaural_process_i16(void* h, const int16_t* in, int inCh, int frames, int rate, int16_t* outStereo);
/** See BinauralTuning::stats. Returns the count written. */
int   vibe_binaural_stats(void* h, float* out, int cap);
}
