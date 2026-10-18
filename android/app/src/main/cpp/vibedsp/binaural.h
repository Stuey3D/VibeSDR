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
//                       place an SSB mistuning shows — there is no carrier to look at. ★ Two-stage since 2026-10-17
//                       (where the voice's energy sits + the harmonic comb): it reaches ±1200 Hz, deep or thin.
//      everything else — the CALLER (the app's JS, from the spectrum it already draws): AM/SAM carrier peak, CW
//                       peak, NFM/FM/WFM power centroid, each against the passband centre. Passed in with
//                       setOffset(). See src/services/binauralOffset.ts for why CW is measured there and not as
//                       "tone vs pitch" here.
//      WFM            — active only while MONO: it fades out as stereo locks (setStereoHint), with hysteresis and
//                       a slow crossfade, so a flickering pilot near lock cannot make it alternate.
//      DAB            — never (the caller sends Source::None).
//      SSB, COARSE    — the CALLER again, from the waterfall (binauralOffset.ts ssbCoarseCarrier): where the
//                       voice's band starts. Reaches a few kHz where the estimator reaches ±1200 Hz, so it is
//                       what makes the lean trigger when you ARRIVE far off a voice. It only steps in where the
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
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "vibedsp.h"   // RealFFT, cf32 (SPREAD's filter bank)

namespace vibedsp {

class SsbOffsetEstimator;
struct SsbOffsetEstimate;

/** Offset (signed Hz, + = signal ABOVE the dial → tune up → lean RIGHT) to a pan target in −1…+1.
 *  0 inside ±deadHz, then a smoothstep to ±1 at ±fullHz — eased at both ends, so neither entering the
 *  dead zone nor reaching full scale has a corner the ear can hear as a jolt. */
float binauralPanFor(float offsetHz, float deadHz, float fullHz);

/** ★★★ SSB's OWN CURVE (2026-10-18, Stuart on 482: "no panning effect at all … at all levels of off tune").
 *  With the plain smoothstep from ±50 to ±1000 Hz a voice 100 Hz off leaned 0.8 % (0.1 dB between the ears) and one
 *  200 Hz off 6.7 % (under 1 dB, 40 µs) — measured on the off-air clips (test-ssb-offset-real §6), NOT ONE of 144 cold
 *  arrivals at ±100/±200 Hz ever reached an audible lean, even with the estimator reading the offset right. On AM the
 *  same curve is fine because AM is tuned off by kHz; an SSB voice is usually a few hundred Hz out.
 *  So SSB rises in TWO eased steps: to kSsbKneePan by the knee (200 Hz at the app's 50/1000 scale), then on to full.
 *  It keeps Stuart's own figures from 479 — "about half … for 500 Hz, and then 1 kHz has the full pan" (52 % at 500,
 *  100 % at 1000) — and is still "not as aggressive when slightly off tune" as build 479's 300 Hz full scale, but a
 *  voice 150-200 Hz off now leans 22-30 % (≈ 2-3 dB and 130-180 µs between the ears — plainly to one side).
 *  ±100 Hz: 8 %; ±300: 33 %. Signed like binauralPanFor; 0 inside ±deadHz. */
float binauralPanForSsb(float offsetHz, float deadHz, float fullHz);
constexpr float kSsbKneeAt = 0.158f;   ///< the knee, as a fraction of the way from the dead zone to full (→ 200 Hz)
constexpr float kSsbKneePan = 0.30f;   ///< the lean reached at the knee

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
 *  ★★★ THE IMAGE HOLDS STILL (2026-10-17, Stuart on 479: "it still flaps about a bit too"): placed band by band, a
 *     voice moved the whole image with every syllable (a vowel's low harmonics one side, its "s" the other). Each frame
 *     is now held to where the passband's energy has sat over the last kImageSlowSec — see steady_(). What sounds AT
 *     ONCE keeps its separation; a lone signal keeps its side.
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
    /** ★★★ THE IMAGE HOLDS STILL (Stuart on 479: "it still flaps about a bit too" — see steady_()): how much of each
     *  frame's departure from the held image centre is taken out (1 = all of it; 0 = the spread as it was to 479). */
    static constexpr float kSteadyImage = 1.0f;
    static constexpr float kImageSlowSec = 2.0f;   ///< the held centre: where the passband's energy has sat over this
    static constexpr float kImageFastSec = 0.01f;  ///< the correction's own glide across frames (never a step)

    // ── ★★★ CROWDED: MORE THAN ONE SIGNAL IN THE PASSBAND (2026-10-18, Stuart on 483: "when in between 2 signals rather
    //     than putting the one below us in the left ear and the one above us in the right it is blending them both and
    //     swirling them about between the ears. This is on CW too.") — see crowd() and binaural.cpp crowdSsb_/crowdCw_.
    /** The crowd weight's glide (seconds): the lean giving way and the spread coming up are never a step. */
    static constexpr float kCrowdTauSec = 0.25f;
    /** SSB: the evidence (another voice where the held one cannot reach) is held this long — the other station's over
     *  and the pause before it (test-binaural-two: overs of 2-5 s). A dial move forgets it at once. */
    static constexpr float kCrowdHoldSec = 8.0f;
    /** SSB: the held voice's band in the audio is taken as its transmitter's widest likely 100-2800 Hz, moved by the held
     *  answer; the region beyond it (on the far side) must be at least this wide to be judged… */
    static constexpr float kCrowdRegionMinHz = 250.0f;
    /** …and, averaged over kCrowdOccSec, sound kCrowdOverDb over its own noise floor kCrowdOccFrac of the time. */
    static constexpr float kCrowdOverDb = 6.0f;
    static constexpr float kCrowdOccSec = 1.0f;
    static constexpr float kCrowdOccFrac = 0.25f;
    /** CW: a carrier is a local peak standing this far over the passband's noise (the median of its floor)… */
    static constexpr float kCwPeakDb = 12.0f;
    /** …heard for at least this long (summed) before it counts — a key click or a noise spike is not a signal… */
    static constexpr float kCwTrackMinSec = 0.1f;
    /** …remembered for kCrowdHoldSec after it was last heard, and two of them at least this far apart are two signals
     *  (a tone between two bins wanders a bin; 150 Hz is 1.6 bins at the bank's 94 Hz). */
    static constexpr float kCwTrackApartHz = 150.0f;
    static constexpr float kCwTrackMatchHz = 70.0f;

    BinauralSpread();
    ~BinauralSpread();
    BinauralSpread(const BinauralSpread&) = delete;
    BinauralSpread& operator=(const BinauralSpread&) = delete;

    /** Stereo in place. `kind` Off = glide out and bypass (AM, FM, DAB; or binaural switched off). `amount` 0…1:
     *  how much spread (0 = the input, delayed). `edgeLo/HiHz`: the default SSB passband's audio edges (full left /
     *  full right before any mirroring). */
    void process(float* L, float* R, int n, double rate, int kind, float amount, float edgeLoHz, float edgeHiHz);
    /** ★★★ What the controller knows that the bank needs to judge a CROWDED passband (audio thread, before process()):
     *  `heldShiftHz` = the held SSB answer as an AUDIO shift (+ = the voice's audio is high: USB +offset, LSB −offset),
     *  NaN when none is held; `coarse` = it is the waterfall's ±150 Hz figure; `deadHz`/`fullHz` = the lean's scale (CW's
     *  crowded map is the lean's own curve); `dialSeq` changes on every dial move (the evidence belongs to the old dial). */
    void setContext(float heldShiftHz, bool coarse, float deadHz, float fullHz, uint32_t dialSeq);
    /** ★★★ 0…1, glided (kCrowdTauSec): how far the passband is judged CROWDED — more than one signal in it, so each is
     *  placed by its own frequency: the controller's whole-image lean gives way by this much and the spread comes up to
     *  full by it. CW: two carriers ≥ kCwTrackApartHz apart heard within kCrowdHoldSec. SSB: another voice sounding where
     *  the held one cannot reach. 0 while the bank is out of the path. */
    float crowd() const { return running_ ? crowd_ : 0.0f; }
    /** Out of the path at once (binaural switched back on: the next use warms up from silence, never from a stale
     *  frame of whatever played before). */
    void reset() { running_ = false; amt_ = 0; eff_ = 0; map_ = 1; wet_ = 0; crowd_ = 0; crowdForget_(); }
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
    /** ★★★ CW's CROWDED map: each audio frequency placed where the LEAN would place a lone carrier there —
     *  binauralPanFor(its offset from the note, dead, full). Two carriers either side of the note are one left, one right,
     *  as far as each would lean alone (Stuart's clip on 483: ±165 Hz → 4-7 dB, where the SSB scale gave < 1 dB). */
    static float panAtCwCrowded(float audioHz, int kind, float noteHz, float deadHz, float fullHz);

private:
    void configure_(double rate);
    void frame_();
    void weights_();
    void cwMeasure_();
    void cwForget_();
    void steady_(double eL, double eR);
    void crowdSsb_();
    void crowdCw_();
    void crowdForget_();
    float panMix_(float f, float centre, float half) const;

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
    std::vector<float> m2L_, m2R_;    // each ear's power gain per bin at FULL amount (steady_ judges the map on these)
    double eSlowL_ = 0, eSlowR_ = 0;  // the held image: each ear's full-amount energy averaged over kImageSlowSec
    float bal_ = 0;                   // the correction now applied, dB (R over L)
    double slowN_ = 0;                // frames in the held centre since it started (a running mean until it is full)

    // CROWDED (audio thread)
    float heldShift_ = NAN;           // setContext: the held SSB answer as an audio shift, NaN = none
    bool heldCoarse_ = false;
    float dead_ = 15, full_ = 300;    // the lean's scale (CW's crowded map)
    uint32_t dialSeq_ = 0;
    bool dialSeqSet_ = false;
    double clock_ = 0;                // seconds of frames since the bank started
    double occ_ = 0;                  // SSB: the far-side region's sounding fraction (kCrowdOccSec)
    double lastEv_ = -1e9;            // SSB: when that last passed kCrowdOccFrac
    float crowd_ = 0;                 // the glided weight (crowd())
    float crowdT_ = 0;                // …and what this hop's judgement says (0 / 1)
    std::vector<float> med_;          // CW: scratch for the noise median
    float wCrowd_ = -1;               // the crowd weight the weights were built for
    float noiseDb_ = -200;            // CW: the passband's noise (median floor), set by cwMeasure_
    int cwA_ = 0, cwZ_ = 0;           // CW: the passband's bins, set by cwMeasure_
    struct Track { float hz = 0, heard = 0; double last = -1e9; };
    Track tracks_[8];                 // CW: carriers heard lately (kCwTrack*)
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
    /** A new median this close to the held answer is not a change (2026-10-18): the SSB curve leans audibly from ~100 Hz,
     *  where FAST's few-Hz scatter between answers would otherwise wobble the pan (§5 counted 1.1 reversals a minute). */
    static constexpr float kHoldHz = 12.0f;
    /** The estimator searches ±1200 Hz (SsbOffsetEstimator::Config::dMaxHz); beyond is not an answer.
     *  ★ Was ±300 until the two-stage estimate (2026-10-17): its coarse stage — where the voice's energy sits in the
     *    passband — tells a pitch alias from the truth, so the comb is no longer confined to ±300. */
    static constexpr float kSsbRangeHz = 1200.0f;
    /** No accepted answer for this long → back to centre (estimator answers only; a reckoned one never goes stale).
     *  ★★ TWO MINUTES, NOT 8 s (Stuart, 2026-10-17: "actually I cant trigger it again"). The dial can only move an
     *  answer that is HELD — so when 8 s of a pause between overs dropped even the on-tune lock, turning off it
     *  then did nothing until several more seconds of on-tune speech re-locked it. A slightly old lock costs far
     *  less than none: a newer accepted answer replaces it, and only long silence lets it go. */
    static constexpr float kSsbStaleSec = 120.0f;
    /** ★★★ HOW FAR A DIAL-RECKONED ANSWER IS HELD (Stuart, 2026-10-17, on V12 474: "if i am on a signal then tune off
     *  it pans but then I wait just off and it centres again even though I can hear badly modulated voices" — "I
     *  use a 500Hz step rate"). One step was beyond the estimator's then ±300 Hz, so nothing could confirm the lean,
     *  the 8 s stale timer dropped it, and a held answer past 4 × 300 Hz was thrown away. An answer moved by the
     *  dial is not a guess — it is exactly how far the knob turned from a voice we had locked — so it is held to
     *  the edge of an SSB passband and never goes stale while the dial stays put. ★ Since 2026-10-17 a 500 Hz step is
     *  inside the estimator's ±1200 Hz, and its evidence moves with the dial (shiftAudio), so it REFINES the knob's
     *  figure within kReckonTrustHz; beyond ±1200 the knob's figure alone holds, as before. */
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
     *  QSO stations by a margin, and stays well inside the estimator's reach. */
    static constexpr float kSsbEnterHz = 90.0f;
    /** ★★ HIGH VOICES COUNT FOR LESS (the same report: "its a deep male voice and a childrens radio club talking").
     *  A child's voice (F0 ~250-400 Hz) puts only 6-10 harmonics in a 2.7 kHz passband, and the comb k·f0 + d fits
     *  d ± f0 (and d ± f0/2) almost as well as d — and those aliases fall INSIDE the estimator's search. FAST was
     *  tuned on adult voices only. Above kHighF0Hz (SsbOffsetEstimate::f0Hz, the estimator's own pitch figure):
     *  1. the gate rises by kHighF0GatePerHz per Hz of pitch, up to ×kHighF0GateMax (×2 from 300 Hz up);
     *  2. while a held answer stands that a LOWER voice helped establish (lowVoiceHeld_), a high-voice answer more than
     *     kAgreeHz from it is ignored — it may refine that answer, never drag it. A high voice ALONE is not held to
     *     its own first answers that way (a wrong first lock would then defend itself for two minutes);
     *  3. (REMOVED 2026-10-17) a high-voice answer at ±f0 or ±f0/2 — its own aliases of "on tune" — was ignored unless
     *     it agreed with the held answer. The two-stage estimator (ssb_offset.h) no longer makes those aliases: on the
     *     same §10 voices the monotone 250/300 Hz children on tune, speaking first too, never lean without the rule, and
     *     the child really 150 Hz off (half its pitch — the rule refused it) is read right. Rules 1 and 2 stay: a child
     *     still has few harmonics, and a lower voice's lock is still the better witness.
     *  PROVISIONAL — chosen on synthetic voices (vibeserver/test-binaural.cpp §10); the off-air clips
     *  (test-ssb-offset-real) have no children's voices yet. Calibrate on real children's voices before trusting them. */
    static constexpr float kHighF0Hz = 200.0f;
    static constexpr float kHighF0GatePerHz = 0.01f;
    static constexpr float kHighF0GateMax = 2.0f;
    // ★ kFastGateScale (0.8 × the SLOW gate, provisional, synthetic) is GONE (2026-10-17): FAST now carries its own
    //   gate, measured on off-air VOLMET and QSO clips (SsbOffsetEstimate::passes, test-ssb-offset-real). The high-F0
    //   rule scales THAT gate's confidence floor.
    /** ★★ THE COARSE CUE'S OWN ERROR. The waterfall puts the carrier ~180 Hz below where the voice's band starts (was 300),
     *  but transmitters cut their low end anywhere from ~100 to ~400 Hz, so the cue is good to about ±150 Hz. A
     *  coarse answer smaller than this cannot tell "on tune" from "just off", and it never leans on its own. */
    static constexpr float kCoarseMinHz = 150.0f;
    /** ★★ A COARSE-HELD answer is not dragged by the cue's own noise (Stuart on 479: "it still flaps about a bit too").
     *  It follows the cue only when they disagree by more than kCoarseFollowHz, and is let go only under
     *  kCoarseReleaseHz — a hysteresis under kCoarseMinHz, so a cue hovering at its floor cannot switch the lean on and
     *  off. Synthetic (test-binaural §11: ±40 Hz of jitter, a cue alternating 140/165 Hz); the cue has no off-air
     *  measurement yet — the real-audio harness has no waterfall. */
    static constexpr float kCoarseFollowHz = 100.0f;
    static constexpr float kCoarseReleaseHz = 100.0f;
    /** A beyond-reach answer the DIAL reckoned and a coarse cue within this of each other are the same voice: the
     *  knob's exact figure is kept, the cue's ±150 Hz is not allowed to replace it. */
    static constexpr float kCoarseAgreeHz = 250.0f;
    /** ★★ A COARSE-HELD answer leans by |cue| − this (2026-10-18). Measured on the composite sweep (scripts/test_binauralSweep.ts,
     *  real voices): the cue's 90th-percentile error is 160-240 Hz at 23-94 Hz bins, so with the SSB curve audible from
     *  ~150 Hz an on-tune voice would otherwise lean whenever the cue erred by 150. Shrunk, a cue must say ≥ 250 Hz to
     *  lean audibly (≥ 22 %) — on-tune frames that do: 0-9 %. A voice far off still leans nearly fully (1 kHz → 93 %). */
    static constexpr float kCoarseShrinkHz = 100.0f;
    /** ★★ THE KNOB HAS CARRIED A VOICE THIS FAR WITHOUT HEARING IT AGAIN (Stuart on 482: "it's not panning as I tune
     *  through the band"). A dial-reckoned answer is exact about the voice it was measured on — but once the knob has
     *  moved this far since the estimator last confirmed it, and the waterfall sees a DIFFERENT voice nearer the dial
     *  (the one now in the passband), the lean follows that one instead of pointing back at the voice you left.
     *  Within ±kSsbRangeHz this was never asked before (rule 1 of setSsbHint): on a busy band, where voices sit
     *  2-3 kHz apart, the old voice stayed in charge while the next filled your ears. */
    static constexpr float kReckonCueHz = 500.0f;

    // ── THE FIRST LEAN ON ARRIVAL (2026-10-18) ──
    /** ★★ A VOTE, FOR WHEN NOTHING IS HELD YET (Stuart on 482: "it's started working, but only after I've been on a
     *  signal for a while"). Arriving off tune, the estimator's GATED answers come sparsely at first — on the off-air
     *  clips the first two agreeing ones took a median 6-8 s and, one arrival in five, over 20 s (test-ssb-offset-real
     *  §6). But its UNGATED answers in that time are nearly always on the right side: kVoteOf of them IN A ROW, every
     *  one reconciled with where the voice's energy sits, over kVoteSnrDb, with at least kVoteConf of strength, all on
     *  one side, past kVoteMinHz and within kVoteSpread of their median, give a PROVISIONAL answer (the median) until the
     *  gated ones take over. Measured on the same clips (VOLMET + QSOs, 3 starts each, 60 s): no wrong-way lean and no
     *  lean on tune; the 80th-percentile first answer at ±200…±500 Hz fell from ~21 s to ~12 s. (3 in a row: one wrong
     *  way at −1000 Hz and one on-tune lean; no strength floor: one wrong way. Hence 4 and 0.2.) */
    static constexpr int   kVoteOf = 4;
    static constexpr float kVoteMinHz = 150.0f;
    static constexpr float kVoteSpread = 0.5f;
    static constexpr float kVoteConf = 0.2f;
    static constexpr float kVoteSnrDb = 8.0f;
    /** ★ The first lean on arrival FADES IN over this (a first estimate: visible, but gentle — never a jump). A lean the
     *  dial or the waterfall set is not faded: turning off a locked voice still leans at once. */
    static constexpr float kArrivalFadeSec = 1.0f;

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
     *   spreadAmount, coarseHeld, spreadCwCentreHz, crowd]. */
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
    void vote_(const SsbOffsetEstimate& e, int sb, float hiF0);   // the provisional first answer (kVoteOf)

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
    std::atomic<float> spreadPub_{0.0f}, cwCentrePub_{0.0f}, crowdPub_{0.0f};
    double lastDialAudio_ = 0;             // audio thread: the dial as last seen (a move bumps dialSeq_)
    bool haveDialAudio_ = false;
    uint32_t dialSeqAudio_ = 0;
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
    std::atomic<bool> softPub_{false};///< the lean began from an estimate, not the dial or the cue: fade it in
    float arrW_ = 1;                  ///< audio thread: the arrival fade's weight (kArrivalFadeSec)
    std::vector<float> votes_;        ///< worker: the last ungated-but-qualifying answers, in a row (kVoteOf)
    double movedSinceMeasure_ = 0;    ///< worker: how far the dial has carried the held answer since it was measured
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
