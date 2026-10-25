// decoders/cw_skimmer.h — CW (Morse): ONE filterbank over a band, a timing decoder only where a carrier is keyed.
//
// ★★★ THE WHOLE POINT (Stuart, 2026-10-25): "CW skimming would be awesome but … CW Skimming is EXTREMELY CPU
//     intensive so if we can do it and make it lightweight by using SIMD NEON SSE2 etc then that would be a HUGE win for
//     us." So the cost is designed in, not tuned afterwards:
//       • ONE windowed FFT per hop over the whole segment (PFFFT: NEON on ARMv7/arm64, SSE on x86 incl. i386) — never a
//         filter and a decoder per channel. A 96 kHz segment with forty stations costs the same transform as an empty one.
//       • Every bin's power, its noise floor and its "is anything here" test are VECTOR LOOPS (four bins an instruction),
//         with a scalar twin that is the reference (VIBE_FORCE_SCALAR builds it on any machine — that is how the speed-up
//         is measured, and the ARMv6 Pi Zero/1 runs it).
//       • The Morse decoder — the only per-signal work — runs only for carriers that are actually there: a few dozen
//         multiply-adds per signal per hop.
//     The same engine is the LISTENER's CW decoder (their audio passband is one small segment of it, real input) and the
//     SKIMMER (a complex IQ segment cut from the radio's channeliser, 24–96 kHz).
//
// ── What it does, per hop (≈ 5.3 ms) ───────────────────────────────────────────────────────────────────────────────
//  1. Window + FFT of the last N samples. N is chosen so a bin is ≈ 47 Hz wide whatever the input rate (one bin ≈ one
//     CW signal; neighbours 50–300 Hz apart resolve) and the hop is N/4 — the envelope sampled at ≈ 188 Hz, so a 50 WPM
//     dot (24 ms) still spans four hops.
//  2. Per bin: power P; a short average S; a long average M (≈ 0.4 s); a noise floor F that falls quickly and rises
//     slowly and never by more than ≈ 1 dB a second (so a keyed carrier cannot lift its own floor). All SIMD.
//  3. Every 8th hop: a scan for bins whose long average stands clearly above the floor AND is a local peak (a carrier's
//     main lobe is one peak — adjacent bins merge into it). Each becomes a TENTATIVE signal.
//  4. Per signal: the envelope (its bin's power, smoothed to a fraction of a dot — the matched filter), a mark level that
//     follows QSB, an on/off decision with hysteresis and a debounce, and the timing decoder: marks and gaps clustered
//     (two-means on log length) into dots/dashes and element/character/word gaps, so the speed is learnt from the signal
//     (5–60 WPM) and follows it. Characters per ITU-R M.1677.
//  5. A tentative signal becomes a SIGNAL only when it has decoded real characters at a real SNR AND its keying is not a
//     copy of a much stronger neighbour's — sidelobes, key clicks and splatter are keyed by the strong signal, so they
//     correlate with it; two operators never do. Ghosts are dropped and their bin held off for a while.
//  6. Callsigns: a call heard in an identifying pattern (CQ … CALL, DE CALL, TEST CALL, CALL TEST, CALL CALL) twice on
//     the same signal is a SPOT.
//
// ★ Nothing here allocates per hop, takes a lock or knows about sockets: the caller (vibe_decoder_host.h) owns threads.
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace cw {

/** One callsign heard identifying itself. */
struct Spot {
    std::string call;
    double offsetHz = 0;   ///< from the segment's centre (IQ) or the audio frequency (real input)
    float  wpm = 0;
    float  snrDb = 0;      ///< mark over noise in the decoder's own ≈ 47 Hz bin (see Skimmer::binHz)
    std::string how;       ///< "CQ", "TEST" or "DE" — what the station was doing when it was heard
    int64_t utcMs = 0;
    int     signalId = 0;
};

/** One signal, as a list row. */
struct SignalView {
    int     id = 0;
    double  offsetHz = 0;
    float   wpm = 0;
    float   snrDb = 0;
    float   quality = 0;   ///< 0..1: how cleanly its characters decode (timing margins, valid codes)
    int64_t firstMs = 0, lastMs = 0;   ///< first and last heard (stream clock, UTC ms)
    std::string call;      ///< the last callsign spotted on it ("" until one is)
    std::string text;      ///< the most recent decoded text (bounded)
    bool    active = false;   ///< still being tracked (false = heard recently, now gone)
    unsigned chars = 0;
};

/** Quality words the clients already show for Morse (high / medium / low / poor). */
const char* qualityWord(float q);

class Skimmer {
public:
    struct Config {
        double rate = 12000;          ///< input sample rate
        bool   complexIn = false;     ///< IQ (centre = 0 Hz) or real audio
        double loHz = 150, hiHz = 4000;   ///< the offsets searched (IQ: e.g. -12000..+12000)
        int    maxSignals = 48;       ///< tracked at once (tentative ones included)
        double binHz = 46.875;        ///< target bin width; the FFT size is the nearest PFFFT size to rate/binHz
        int    keepGoneSec = 600;     ///< how long a signal that has gone stays in signals() (the "heard" list)
        int    respotSec = 600;       ///< a call is spotted again on the same signal after this
        int    spotRepeats = 2;       ///< identifying patterns needed before a call is spotted
    };
    explicit Skimmer(const Config& c);
    ~Skimmer();
    Skimmer(const Skimmer&) = delete;
    Skimmer& operator=(const Skimmer&) = delete;

    void feedReal(const float* x, int n);
    void feedIq(const std::complex<float>* x, int n);

    /** Decoded text as it arrives, per CONFIRMED signal (text held while it was tentative is sent on confirmation). */
    std::function<void(int id, double offsetHz, const std::string& text)> onText;
    std::function<void(const Spot&)> onSpot;

    /** Confirmed signals, active first (strongest first), then the recently gone (newest first). */
    std::vector<SignalView> signals(bool includeGone = true) const;
    /** The signal with this id, if confirmed. */
    bool signal(int id, SignalView& out) const;

    /** ★ The stream clock: UTC ms of the first sample (default: now at construction). Times are
     *  start + samples/rate, so a test feeding faster than real time gets real-looking times. */
    void setStartUtcMs(int64_t ms);
    int64_t nowMs() const;

    int    fftSize() const;
    int    hop() const;
    double binHzActual() const;
    double hopSec() const;
    int    binsSearched() const;
    uint64_t hops() const;
    int    tracked() const;         ///< tentative + confirmed
    int    confirmed() const;       ///< active confirmed signals
    uint64_t ghostsDropped() const;      ///< ghosts AND broadband (speech, data) dropped
    uint64_t broadbandDropped() const;   ///< of those, broadband
    uint64_t tentativesDropped() const;
    /** ★ The floor's calibration, for the tests: the mean over the searched bins of (long average ÷ noise estimate).
     *  1.0 on pure noise when kFloorBias is right (cw_bench bias). */
    double noiseCalibration() const;
    /** Which kernels this build runs: "neon", "sse2" or "scalar". */
    static const char* simdName();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/** ★ Morse → text (ITU-R M.1677 letters, figures, punctuation and the prosigns that have no character).
 *  `code` is the tree index: start at 1, a dot doubles it, a dash doubles it and adds 1 — ".-" is 1→2→5.
 *  nullptr = not a Morse character. */
const char* morseText(unsigned code);
/** The reverse, for the tests' signal generator: "A" → ".-" (prosigns by their <XX> name). "" = unknown. */
std::string morseCode(const std::string& ch);

/** ★ A callsign the skimmer will spot: prefix (1–3 characters, at least one letter), a digit, a suffix of 1–4 letters,
 *  an optional /portable part; never a Q-code, a report or a word ("5NN", "TU", "CQ", "TEST"…). */
bool isCallsign(const std::string& s);
/** The station identifying itself in a run of decoded words, if any: "CQ CQ DE G4ABC G4ABC K" → G4ABC, "CQ".
 *  ★ Exposed for the tests. `how` is "CQ", "TEST" or "DE". */
bool identify(const std::vector<std::string>& words, std::string& call, std::string& how);

} // namespace cw
} // namespace vibe
