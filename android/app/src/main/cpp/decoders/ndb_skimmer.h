// decoders/ndb_skimmer.h — NDB (aeronautical non-directional beacon) SKIMMER: every carrier in an LF slice found, its
// keyed tone decoded, and the ident CONFIRMED BY CONSENSUS across its repeats. docs/v12/NDB-SKIMMER.md.
//
// ★★★ WHAT AN NDB IS (ICAO Annex 10 Vol. I §3.4; ITU-R emission classes): a steady carrier, 190–1750 kHz (Europe mostly
//     255–526.5 kHz), identified in Morse — two or three letters (sometimes one, rarely four) at about 7 WPM, repeated at
//     least three times every 30 s. The ident keys a TONE on the carrier (A2A: ≈ 400 Hz or ≈ 1020 Hz, so both sidebands
//     are keyed) or, more rarely, keys the carrier itself (A1A). Several beacons may share one frequency; a strong one
//     can have a second harmonic.
//
// ★★★ THE TWO THINGS THIS DOES BETTER THAN A WAITING LIST
//   • EVERY CARRIER AT ONCE. One windowed FFT per hop over the whole slice (PFFFT: NEON on ARMv7/arm64, SSE on x86
//     including i386) is the filterbank — the CW skimmer's architecture (cw_skimmer.h), sized for NDBs: ≈ 23 Hz bins
//     (an ident's sidebands are ±10 Hz wide at 7 WPM, so a narrow bin is the matched filter) and a hop of half a window
//     (≈ 21 ms — a 7 WPM dot is eight hops; the CW skimmer's 5 ms hop would cost four times the transforms for nothing).
//     The AM detection is done IN THE FILTERBANK: an A2A ident is the power in the two sideband bins at carrier ± tone,
//     summed — both sidebands are keyed together, so the sum is 3 dB better than either. Nothing is queued, and a slice
//     with sixty beacons costs the same transform as an empty one.
//   • CLEAN IDENTS BY CONSENSUS. A single ident read through noise is a guess. Each repeat is decoded on its own, and an
//     ident is CONFIRMED only when several repeats agree (the same letters three times, or a letter-by-letter majority of
//     several repeats that was itself read whole at least twice). Noise read as letters never agrees with itself, so it is
//     never shown as an ident (tested: 30 minutes of noise → no ident).
//
// ── Per hop ─────────────────────────────────────────────────────────────────────────────────────────────────────────
//  1. Window + FFT; per bin: power P and its long average L (≈ 1.5 s). SIMD (four bins an instruction), scalar twin.
//  2. Per beacon: two numbers kept for the last 48 s — the tone envelope P[c+k] + P[c−k] and the carrier's own P[c].
// ── Per second ──────────────────────────────────────────────────────────────────────────────────────────────────────
//  3. The noise floor: a low quantile of L in 64-bin blocks (carriers occupy a few bins of each), calibrated to the mean.
//  4. Carriers: a local peak of L ≥ 3 dB over the floor, seen three seconds running, that is neither a window sidelobe of
//     a much stronger carrier nor one of a pair mirrored about one (a keyed tone's sidebands).
//  5. The tone: the sideband pair, 300–1150 Hz out, whose weaker side stands highest over the floor on a ≈ 10 s average.
//  6. The decode: the envelope (smoothed to ≈ ⅔ of a dot) sliced at the midpoint between its two clusters — only when
//     those clusters are separated well enough to be keying at all — into marks and gaps; the dot learnt from the
//     marks (machine keying: dash = 3 dots), characters per ITU-R M.1677, one WORD per repeat.
//  7. The vote (IdentVote): every repeat is a ballot.
//
// ★ Nothing here allocates per hop, takes a lock or knows about sockets: the caller (vibe_ndb_service.h) owns threads.
#pragma once
#include <complex>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace ndb {

/** ★★ THE CONSENSUS. Each decoded repeat of an ident is one ballot; an ident is confirmed only when the ballots agree.
 *  Exposed (not hidden in the engine) so the rule itself is tested. */
class IdentVote {
public:
    struct Rules {
        int exact = 3;            ///< the same reading this many times confirms it…
        int dominance = 2;        ///< …if it was read at least this many times as often as any other reading
        int positional = 4;       ///< or: this many readings of one length, each letter a majority (≥ 70 %) of them…
        int positionalWhole = 2;  ///< …whose majority string was itself read whole at least this often
        int window = 12;          ///< ballots kept (the oldest goes)
        int64_t maxAgeMs = 300000;   ///< and none older than 5 min
    };
    IdentVote() = default;
    explicit IdentVote(const Rules& r) : r_(r) {}
    /** One repeat's reading ("WOD"). Returns true when this ballot CONFIRMED an ident (first time, or a new one). */
    bool add(const std::string& reading, int64_t ms);
    /** ★★ CONSENSUS AT THE SIGNAL: the repeats split into disjoint GROUPS (two or three, of three repeats or more each),
     *  each group's envelopes averaged in step with the ident's period and read once. Groups share no data, so their
     *  agreement is independent repeats agreeing — every group reading the same 2–5 characters confirms it. */
    bool addFolded(const std::vector<std::string>& groupReadings, int64_t ms);
    /** ★ The deep path: one reading of a WHOLE fold (all the repeats in the history) — the engine hands one only per
     *  window of history that shares nothing with the last one, so two that agree are two independent consensuses. */
    bool addFoldWindow(const std::string& reading, int64_t ms);
    /** ★ The whole fold's current clean reading ("" = none). When there is one, a vote of single repeats that disagrees
     *  with it confirms nothing — one repeat at a time, a weak beacon is misread the SAME way often enough to win a vote
     *  ("OX" read "OTA": a gap inside X taken for a character gap); nine repeats averaged are not. */
    void setFoldHint(const std::string& f) { fold_ = f; }
    const std::string& ident() const { return ident_; }   ///< "" until confirmed
    bool confirmed() const { return !ident_.empty(); }
    int votes() const;                                    ///< ballots in the window that read the ident exactly
    int ballots() const { return (int)b_.size(); }
    /** The reading most often seen so far (an unconfirmed guess — never shown as the ident). */
    std::string leading() const;
    /** Other idents confirmed on this frequency before (two beacons sharing it), newest first. */
    const std::vector<std::string>& others() const { return others_; }
    int64_t confirmedMs() const { return firstMs_; }      ///< when it was first confirmed (stream clock)
    int64_t lastMs() const { return lastMs_; }            ///< the last ballot that read it

private:
    void expire(int64_t ms);
    Rules r_;
    std::deque<std::pair<std::string, int64_t>> b_;
    std::string ident_;
    std::vector<std::string> others_;
    std::deque<std::string> win_;     // whole-fold readings, one per disjoint window
    std::string fold_;
    int64_t firstMs_ = 0, lastMs_ = 0;
};

/** A word the decoder read on a beacon, as it arrives (the live copy). */
struct Copy {
    int id = 0;
    double offsetHz = 0;
    std::string text;
    int64_t utcMs = 0;
};

/** One carrier, as a list row. */
struct BeaconView {
    int     id = 0;
    double  offsetHz = 0;    ///< carrier, from the slice's centre (Hz, refined below a bin)
    float   snrDb = 0;       ///< carrier over the noise in one bin (≈ 23 Hz — Skimmer::binHzActual)
    float   levelDb = 0;     ///< carrier power, dB relative to full scale of the input (comparable between beacons)
    float   toneHz = 0;      ///< the keyed tone (A2A), 0 = none found
    float   toneSnrDb = 0;   ///< the ident's marks over the noise, two sideband bins
    bool    keyedCarrier = false;   ///< the ident keys the carrier itself (A1A)
    float   wpm = 0;         ///< 0 until the speed is known
    std::string ident;       ///< CONFIRMED ident ("" until the vote says so)
    int     votes = 0;       ///< ballots that read the ident
    int     ballots = 0;     ///< repeats decoded in the vote's window
    std::string reading;     ///< the leading unconfirmed reading ("" = none) — a guess, never the ident
    std::vector<std::string> others;   ///< other idents confirmed here before (a shared frequency)
    std::string copy;        ///< the last words decoded (bounded)
    int64_t firstMs = 0, lastMs = 0;   ///< carrier first and last seen (stream clock, UTC ms)
    int64_t identMs = 0;     ///< when the ident was first confirmed
    int64_t lastIdentMs = 0; ///< the last repeat that read it
    bool    active = false;  ///< the carrier is there now
};

class Skimmer {
public:
    struct Config {
        double rate = 192000;        ///< complex input rate (IQ, centre = 0 Hz)
        double loHz = -1e12, hiHz = 1e12;   ///< carrier offsets searched (clamped to the inner 94 % of the slice)
        double binHz = 23.4375;      ///< target bin width; the FFT size is the nearest PFFFT size to rate / binHz
        int    maxCarriers = 160;    ///< tracked at once (tentative ones included)
        int    keepGoneSec = 1800;   ///< a carrier that has gone stays in beacons() this long
        double toneLoHz = 300, toneHiHz = 1150;   ///< where a keyed tone is looked for, either side of the carrier
        /** ★ ICAO Annex 10 (3.4.5): the ident tone is 1020 ± 50 Hz or 400 ± 25 Hz. On (the default), only those two
         *  windows (a little wider, for old keyers) are searched — far fewer places for noise to pass for a weak tone. */
        bool toneIcaoOnly = true;
        bool coherent = true;        ///< the sidebands combined coherently on the carrier's phase (else their powers summed)
        double carrierDb = 3.0;      ///< a carrier's long average over the floor, in one bin
        IdentVote::Rules vote;
    };
    explicit Skimmer(const Config& c);
    ~Skimmer();
    Skimmer(const Skimmer&) = delete;
    Skimmer& operator=(const Skimmer&) = delete;

    void feedIq(const std::complex<float>* x, int n);

    std::function<void(const Copy&)> onCopy;               ///< every word read (unconfirmed ones too — the live copy)
    std::function<void(const BeaconView&)> onIdent;        ///< an ident confirmed (first time or a new one)

    /** Carriers: active first (strongest first), then the recently gone (newest first). */
    std::vector<BeaconView> beacons(bool includeGone = true) const;

    /** ★ The stream clock: UTC ms of the first sample (default: now at construction); times are start + samples/rate. */
    void setStartUtcMs(int64_t ms);
    int64_t nowMs() const;

    int    fftSize() const;
    int    hop() const;
    double binHzActual() const;
    double hopSec() const;
    double searchedLoHz() const;   ///< the offsets actually searched for carriers
    double searchedHiHz() const;
    uint64_t hops() const;
    int    carriers() const;       ///< active carriers (not tentative)
    int    identified() const;     ///< active carriers with a confirmed ident
    uint64_t wordsRead() const;
    /** ★ The floor's calibration, for the tests: mean over the searched bins of (L ÷ floor). 1.0 on pure noise. */
    double noiseCalibration() const;
    static const char* simdName();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace ndb
} // namespace vibe
