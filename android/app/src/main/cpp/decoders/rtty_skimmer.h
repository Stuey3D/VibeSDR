// decoders/rtty_skimmer.h — the RTTY skimmer (V12, 2026-11-14): every RTTY signal across a 24–96 kHz segment, each
// with its OWN settings found for it. The spec Stuart agreed is docs/v12/RTTY-SKIMMER.md.
//
// ★★ BUILT FROM THE PUBLISHED DESCRIPTIONS: ITA2 (Baudot-Murray) five-unit code with a start bit (space) and 1, 1.5 or 2
//    stop bits (mark), LTRS/FIGS shifts; FSK as two tones a standard shift apart (170, 200, 425, 450, 850 Hz) at 45.45,
//    50 or 75 baud. The bit-level decoder is VibeSDR's own FskDecoder (decoders/fsk_decoder.h) and the search rules are
//    RTTY AUTO's (rttyscore, decoders/rtty_auto.h) — shared, so both improve together. No other skimmer's source was read.
//
// ── 1. Detect, then list (continuous, cheap) ──────────────────────────────────────────────────────────────────────
//   ONE forward FFT per hop over the whole input (PFFFT, ≈ 6 Hz bins), as the PSK skimmer's (psk_skimmer.h): it is
//   BOTH the finder (every bin's fast and slow power, a floor, tone peaks — vector loops) and the channeliser.
//   • A CANDIDATE is two tone peaks a standard shift apart (within 12 %, rttyscore::snapShift). Spacing = the shift,
//     midpoint = the centre — free from the shared FFT.
//   • Its CHANNEL is cut from the same transform: the bins around the centre, masked, one small inverse (overlap-save),
//     whose size is chosen so the channel comes out at 3000 samples a second EXACTLY whatever the input rate — every
//     standard bit is then a whole number of samples (45.45 baud 1.5 stop: 33 per half bit), which FskDecoder's bit
//     clock needs. The inverse IS the resampler: no mixer, filter or interpolator runs at the input rate for any signal.
//   • ★★ THE TEST THAT MAKES IT RTTY: the two tones TAKE TURNS — one up while the other is down, their sum steady. Each
//     tone's envelope is measured on the channel (integrate-and-dump over ≈ 6 ms, its length chosen so the OTHER tone
//     sits in a null), and their correlation over ≈ 1.5 s must be clearly NEGATIVE. Two unrelated carriers 170 Hz
//     apart (CW, PSK) are uncorrelated; noise is uncorrelated. Only a pair that passes is LISTED — at once, "RTTY
//     detected — waiting for parameter detection" — and its channel is STORED from then on (a 10 s ring, int16).
//   • ★ A NEW CHANNEL STARTS IN THE PAST: the last ≈ 1 s of transforms are kept and a new channel is run over them first.
//
// ── 2. Then set each signal's parameters (a queue, run by workers) ────────────────────────────────────────────────
//   • Listed signals with a few seconds stored queue, STRONGEST FIRST. A worker (workOnce(), on the CALLER's thread —
//     nothing here makes threads) takes one and runs over a COPY of its stored channel, much faster than real time:
//       1. the fast path — 45.45 baud at the measured shift, normal and reverse (1.5 stop);
//       2. only if that does not frame cleanly, the full contest (rttyscore::all(): 45.45 / 50 / 75 baud, 1 or 1.5 stop,
//          both polarities) scored as RTTY AUTO scores (frames passing the start/stop check against frames failing).
//   • The winner is SET for that signal: its decoder (already through the stored audio) is handed back, caught up from
//     the ring and runs live — ONE decoder per settled signal. Its text starts from when the signal was first heard.
//   • Nothing frames → "not RTTY": dropped, and that centre is not tried again for a while.
//   • A settled signal whose frames go bad for 8 s (the station changed its settings) is queued again.
//   • The settings are written INTO the text when set ("[45 baud / 170 Hz / Reverse] ") and scroll away with it
//     (Stuart: no fixed column — wasted space); SignalView keeps them for the card.
//
// ── Text you can trust ──────────────────────────────────────────────────────────────────────────────────────────
//   A signal's text is held until it READS AS TEXT (spaces, variety, word lengths, few figure-shift symbols — noise
//   decoded as ITA2 has none of these), and after that each stretch is shown only if its frames were clean.
//   Callsigns heard in CQ / DE / TEST come from the shared callsign rules (cw::identify), as the CW and PSK skimmers'.
//
// ★ Nothing allocates per hop or takes a lock per hop: a mutex guards only the job queue (a handful of times a signal).
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace rttysk {

/** One callsign heard identifying itself. */
struct Spot {
    std::string call;
    double offsetHz = 0;   ///< the signal's centre (midway between its tones), from the segment's centre
    float  snrDb = 0;      ///< the tones over the floor, dB
    std::string how;       ///< "CQ", "TEST" or "DE"
    std::string mode;      ///< "RTTY 45/170" etc.
    int64_t utcMs = 0;
    int     signalId = 0;
};

/** One signal, as a list row. */
struct SignalView {
    int     id = 0;
    double  offsetHz = 0;   ///< centre, Hz from the segment's centre
    double  shiftHz = 0;    ///< the standard shift its tones are apart
    bool    set = false;    ///< parameters found (false = "RTTY detected — waiting for parameter detection")
    double  baud = 0;       ///< 45.45 / 50 / 75 once set
    bool    reverse = false, oneStop = false;
    std::string settings;   ///< "45 baud / 170 Hz / Reverse" once set ("" before)
    float   snrDb = 0;      ///< the weaker tone over the floor, dB
    float   quality = 0;    ///< 0..1 (framing, once set)
    int64_t firstMs = 0, lastMs = 0;
    std::string call;
    std::string text;       ///< the most recent text shown (bounded; newlines kept; the settings line inside it)
    bool    active = false;
    unsigned chars = 0;
};

const char* qualityWord(float q);

class Skimmer {
public:
    struct Config {
        double rate = 48000;            ///< the IQ input's sample rate (centre = 0 Hz)
        double loHz = -22000, hiHz = 22000;   ///< the offsets searched
        int    maxSignals = 48;         ///< channels at once (candidates being tested included)
        int    workers = 0;             ///< 0: jobs run INLINE at the end of a hop (tests, and a caller with no threads);
                                        ///<   ≥ 1: the caller runs workOnce() on that many threads (onWork wakes them)
        int    keepGoneSec = 600;       ///< how long a signal that has gone stays in signals()
        int    respotSec = 600;
        int    spotRepeats = 2;
        double candidateDb = 4.0;       ///< each tone's power (±2 bins) over the floor for a pair to be tried
        /** ★ The alternation a pair needs to be listed: −(correlation of the two tones' envelopes). RTTY reads ≈ 0.6–0.9,
         *  unrelated carriers and noise ≈ 0 ± 0.1. A test lowers it below −1 to list everything and prove the TEXT gate
         *  alone keeps noise silent. */
        double alternation = 0.35;
        double storeSec = 10;           ///< each listed signal's stored channel
        double jobSec = 4;              ///< stored before its parameters are searched for
        double notRttyHoldSec = 90;     ///< a centre that would not frame is not tried again for this long
    };
    explicit Skimmer(const Config& c);
    ~Skimmer();
    Skimmer(const Skimmer&) = delete;
    Skimmer& operator=(const Skimmer&) = delete;

    void feedIq(const std::complex<float>* x, int n);

    /** ★ THE WORKERS' ENTRY: take the strongest queued signal, search its stored channel for its settings, hand the
     *  result back (applied by the next hop on the feeding thread). Thread-safe; any number of threads may call it.
     *  Returns false when nothing was queued. */
    bool workOnce();
    /** Called on the feeding thread when a job is queued — the caller wakes a worker. */
    std::function<void()> onWork;
    int queuedJobs() const;   ///< thread-safe
    int runningJobs() const;  ///< thread-safe

    /** Shown text as it is committed, per SET signal (the settings line included). */
    std::function<void(int id, double offsetHz, const std::string& text)> onText;
    std::function<void(const Spot&)> onSpot;

    /** Listed signals: active first (lowest offset first), then the recently gone (newest first). */
    std::vector<SignalView> signals(bool includeGone = true) const;
    bool signal(int id, SignalView& out) const;

    void setStartUtcMs(int64_t ms);
    int64_t nowMs() const;
    double nowSec() const;   ///< seconds of input fed

    int    fftSize() const;
    double binHz() const;
    double channelRate() const;     ///< each signal's channel, Hz (3000 — exactly, or within 0.2 %)
    int    channelSize() const;     ///< the per-signal inverse's size
    int    binsSearched() const;
    uint64_t hops() const;
    int    tracked() const;         ///< channels open (candidates being tested + listed)
    int    listed() const;          ///< active listed signals
    int    settled() const;         ///< active signals with their settings found
    uint64_t candidatesRejected() const;   ///< pairs that did not alternate (not RTTY by the tone test)
    uint64_t notRtty() const;              ///< listed signals that would not frame at any setting
    uint64_t jobsRun() const;
    uint64_t requeues() const;
    uint64_t charsSuppressed() const;      ///< decoded but never shown (the text gate)
    /** ★ When each listed signal was listed and when it was set (input seconds) — the tests' "listed before settings". */
    bool timesOf(int id, double& listedSec, double& setSec) const;
    static const char* simdName();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/** ★ ITA2 (US teleprinter figures): the five-bit code for an ASCII character in letters (`figs` false) or figures —
 *  -1 if none. For the tests' generator; the decoder's own table is FskDecoder's. */
int ita2Code(char c, bool& figs);
/** Plausible teleprinter text? The gate the skimmer shows text through (exposed for the tests). */
bool readsAsText(const std::string& t);

} // namespace rttysk
} // namespace vibe
