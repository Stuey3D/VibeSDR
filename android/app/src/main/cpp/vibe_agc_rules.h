#pragma once
/**
 * vibe_agc_rules.h — three small AGC / auto-IF rules, pure so vibeserver/test-agc-rules.cpp can hold
 * them down without a radio. The shim (local_sdr_shim.cpp) is the only caller.
 *
 * All three come from one evening's log on the Sony (Lite, RTL-SDR, 2026-10-02 21:23 and 21:25), two
 * audio drops Stuart heard while changing mode:
 *
 *   21:23:02.5  DAB off -> FM 96.6
 *   21:23:03.9  adjacent narrow: ON — gain 3.6 dB > 3.0          (1.4 s after the retune)
 *               auto bandwidth: MPX S/N 26.2 dB -> 110 kHz
 *   21:23:05.9  adjacent narrow: OFF — narrow worth -4.0 dB      (2 s later: it was never worth it)
 *               auto bandwidth: -> 195 kHz
 *
 *   21:25:28.1  128.117 MHz (airband, AGC at 49.6 dB) -> 96.6 MHz, still in AM
 *   21:25:30.1  ADC OVERLOAD: 56.6 % of samples on the rail
 *   21:25:30.8  0.0 dBFS is 6.0 dB over the operating point — dropping 6.2 dB  (49.6 -> 43.4)
 *   21:25:32.8                                     ...dropping 6.2 dB          (43.4 -> 37.2)
 *   21:25:34.9                                     ...dropping 7.5 dB          (37.2 -> 29.7)
 *   21:25:36.9                                     ...dropping 6.8 dB          (29.7 -> 22.9)
 *   21:25:38.3  ADC overload cleared after 8 s      (and it settled at 22.9 dB, -5 dBFS)
 */
#include <algorithm>
#include <cmath>
#include <vector>

namespace vibeagc {

/* ★★★ 1. AFTER A RETUNE, A MODE CHANGE OR LEAVING DAB, THE AUTO-IF DECISIONS WAIT 6 s.
 *  The adjacent-channel arm steers by ifGainDb — a 0.05 EMA that its own note says "needs ~5 s to mean
 *  anything" — and auto bandwidth by an MPX S/N that is itself re-acquiring with the pilot. At 21:23
 *  both acted 1.4 s after the retune on numbers that had not settled, narrowed to 110 kHz (losing RDS
 *  and most of the stereo), and undid it two seconds later. 6 s = the EMA's ~5 s plus most of one 2 s
 *  tick, so the first decision is taken on settled figures; in steady state nothing changes, because
 *  the hold only follows those three events.
 *  ★ A HOLD, NOT A LIMIT: the new station starts WIDE (the neutral state — "never limit permanently"),
 *    and the controller then re-earns any narrowing from its own measurement. */
constexpr double kAutoIfSettleSec = 6.0;
inline bool autoIfHeld(double nowS, double quietUntilS) { return nowS < quietUntilS; }

/* ★★★ 2. A GROSS OVERLOAD IS SHED IN ONE MOVE, NOT IN 6 dB RUNGS.
 *  The back-off sizes its jump from the converter's peak over the operating point — and a peak cannot
 *  read above 0 dBFS, so with half the samples on the rail it reported "6.0 dB over" four times running
 *  and the loop walked down 6-7 dB per 2 s tick: 8 s of clipped audio to cover 26.7 dB. When the rail
 *  fraction is gross the peak has stopped being a measurement, so shed at least kGrossShedDb at once;
 *  the next tick's peak is readable again and sizes whatever remains.
 *  ★ Below kGrossClipPct the peak is still meaningful and the computed shed stands unchanged.
 *  ★ It cannot strand the gain low: the climb's own jump ("clear by X — jumping to ...") recovers a
 *    cut that overshot, which is the same safety the existing self-sizing relies on. */
constexpr double kGrossClipPct = 10.0;
constexpr double kGrossShedDb  = 18.0;
inline double grossOverloadShedDb(double computedShedDb, double clipPct) {
    return clipPct >= kGrossClipPct ? std::max(computedShedDb, kGrossShedDb) : computedShedDb;
}

/* ★★★ 3. A BAND IS ENTERED AT THE GAIN THAT LAST SETTLED THERE.
 *  DAB already does this per block ("entering block at the gain it last decoded at"). Everything else
 *  carried the last band's gain across the jump: airband wants the tuner's top, an FM transmitter two
 *  miles away wants half of it, and 49.6 dB on 96.6 put 56 % of the samples on the rail. The key is a
 *  coarse BAND, not a frequency: it is a starting point, and the loop still climbs or cuts from there.
 *  ★ Coarse on purpose. The edges are the ones that change what a front end sees (broadcast FM,
 *    airband, Band III …); finer keys would rarely have a memory to offer. */
inline int agcBandKey(double hz) {
    static const double kEdges[] = {
        300e3,    // LF
        3e6,      // MF
        30e6,     // HF
        87.5e6,   // VHF low
        108e6,    // FM broadcast
        137e6,    // airband
        174e6,    // VHF high
        240e6,    // Band III (DAB)
        470e6,    // UHF low
        1000e6,   // UHF
    };
    int k = 0;
    for (double e : kEdges) { if (hz < e) return k; ++k; }
    return k;                 // above 1 GHz
}

/* ★★★ 4. WHAT A RETUNE FORGETS — THE CHANNEL ALWAYS, THE FRONT END ONLY ON A REAL MOVE.
 *  ★★★ STUART'S DECISION, 2026-10-20 (Portable, RTL-SDR V4, hopping between close SSB signals: "it seemed to
 *      reset the AGC moving from one SSB signal to another in close range"). This partly reverses the 08-25
 *      rule, which forgot EVERYTHING once the dial moved more than half the demodulator's bandwidth — 1.35 kHz
 *      on SSB, so every hop cleared the overload memory, the margin, "settled" and the repeat count, and the
 *      8 s hurry let the loop re-climb into the gain that had just been cut.
 *  ★★ THE SPLIT. Two kinds of thing are learned, and they belong to different places:
 *       · the CHANNEL — separation averages, the peak-hold, a move on trial, the hill-climb's run baselines.
 *         A different station invalidates them, so they still go at half the demodulator's width (the 08-25
 *         figure, unchanged: SSB 1.35 kHz · CW 1.25 · AM 5 · NFM 6.25 · WFM 100 kHz — the shim floors the width
 *         at 2.5 kHz). That is agcChannelMoved().
 *       · the FRONT END — the gain that overloaded, the strike count, the learned margin. Those are facts
 *         about what the TUNER and the CONVERTER are being handed, which is the whole capture behind the IF
 *         filter, not the channel. A hop that keeps the same capture window and the same IF filter hands the
 *         front end the same signals, so they are KEPT.
 *  ★ A REAL MOVE (everything forgotten, as before): the dial leaving the capture window, the tuner moving by
 *    more than a quarter of the IF filter's width from where the front end was learned, a band change
 *    (agcBandKey), and — handled at their own call sites — a sample-rate change, the IF filter widening, DAB.
 *  ★ Measured from where each was LAST learned, never from the previous tune, so a drag of small nudges still
 *    adds up: 100 Hz steps re-learn the channel every 1.4 kHz on SSB and the front end once the tuner has
 *    travelled a quarter of the filter. */
constexpr double kRetuneForgetMinHz = 1000.0;
inline double agcRetuneForgetHz(double agcDemodBwHz) { return std::max(kRetuneForgetMinHz, 0.5 * agcDemodBwHz); }
inline bool agcChannelMoved(double learnedAtHz, double newHz, double agcDemodBwHz) {
    return std::fabs(newHz - learnedAtHz) > agcRetuneForgetHz(agcDemodBwHz);
}
/** The IF filter the front end actually sees: 0 (no filter / automatic) means the capture itself. */
inline double agcIfEffectiveHz(double tunerBwHz, double sampleRateHz) {
    return (tunerBwHz <= 0.0 || tunerBwHz > sampleRateHz) ? sampleRateHz : tunerBwHz;
}
constexpr double kFrontEndKeepFrac = 0.25;
inline double agcFrontEndKeepHz(double ifEffHz) { return kFrontEndKeepFrac * ifEffHz; }
/** Has the TUNER moved far enough that what reaches the front end is a different set of signals?
 *  `anchorHz` <= 0 means nothing has been learned yet — any position is a new one. */
inline bool agcFrontEndMoved(double anchorHz, double tunerHz, double ifEffHz) {
    return anchorHz <= 0.0 || std::fabs(tunerHz - anchorHz) > agcFrontEndKeepHz(ifEffHz);
}
enum class RetuneForget { None = 0, Channel = 1, Full = 2 };
inline const char* retuneForgetName(RetuneForget r) {
    return r == RetuneForget::Full ? "full" : r == RetuneForget::Channel ? "channel" : "none";
}
/** One decision for retune(): `tunerAnchorHz`/`tunerNewHz` are the tuner centre where the front end was
 *  learned and where it will be after this tune; `leavesWindow` = the dial is outside the capture. */
inline RetuneForget agcRetuneDecision(double learnedAtHz, double newHz, double agcDemodBwHz,
                                      double tunerAnchorHz, double tunerNewHz, double ifEffHz,
                                      bool leavesWindow, bool bandChanged) {
    if (learnedAtHz <= 0.0) return RetuneForget::Full;   // ★ the first tune: nothing learned to keep
    const bool chan = agcChannelMoved(learnedAtHz, newHz, agcDemodBwHz);
    if (!chan && !leavesWindow && !bandChanged) return RetuneForget::None;
    if (leavesWindow || bandChanged || agcFrontEndMoved(tunerAnchorHz, tunerNewHz, ifEffHz))
        return RetuneForget::Full;
    return RetuneForget::Channel;
}

/* ★★★ 5. CARRIERLESS MODES (SSB, CW) ARE JUDGED ON HEADROOM ONLY — Stuart, 2026-10-20.
 *  The before/after verdict asks "did that move leave the station further above its neighbourhood?". On a
 *  signal with no carrier that question is answered by the OPERATOR, not the gain: speech level swings
 *  several dB syllable to syllable and stops between words, so a 0.5 dB threshold reads the talker. Measured
 *  on synthetic SSB speech with NO change at all: 47 % of trials "better", 47 % "worse" even with equal
 *  windows (and 60 / 32 with the old hold). So SSB and CW are treated as DAB already is — the converter
 *  decides: climb while the predicted peak clears the ceiling, cut when it is hot or railing.
 *  ★★ AND THE PREDICTION READS A HELD PEAK. The ADC peak is a one-second maximum; a pause between overs
 *     drops it and the old loop climbed into the next syllable. The climb is predicted against the loudest
 *     second of the last kHeadroomPeakHoldSec, so a pause shorter than that cannot ask for gain, and a climb
 *     lands no higher than the operating point (kAgcTargetDbfs, -6). The CUT still reads the live second (two hot seconds, or the rail), so a genuine overload is shed at once. */
/*  ★★ 30 s AND A 5 dB DEADBAND, BOTH MEASURED ON THE MODEL in test-agc-rules.cpp (20 x 10 min of synthetic SSB
 *     speech, the station the loudest thing in the capture — the worst case): held 10 s and climbing to the -1 dBFS
 *     ceiling still moved the gain 431 times; 30 s and climbing only as far as the -6 dBFS operating point, 25 —
 *     about one move per ten minutes once settled. The CUT stays at -1 dBFS for two seconds, so the 5 dB between
 *     where a climb may land and where a cut begins is wider than any rung on an RTL (4 dB at most) plus a
 *     talker's syllable-to-syllable swing. agc_window_narrower_than_step, applied. */
constexpr int kHeadroomPeakHoldSec = 30;
struct PeakHold {
    double pk[kHeadroomPeakHoldSec];
    int n = 0, head = 0;
    PeakHold() { reset(); }
    void reset() { n = 0; head = 0; for (double& v : pk) v = -99.0; }
    void push(double dbfs) { pk[head] = dbfs; head = (head + 1) % kHeadroomPeakHoldSec; if (n < kHeadroomPeakHoldSec) ++n; }
    double max() const { double m = -99.0; for (int i = 0; i < n; ++i) m = std::max(m, pk[i]); return m; }
};

/* ★★★ 6. THE BURSTY CHANNEL LEVEL (AM, NFM): THE SAME STATISTIC BEFORE AND AFTER A MOVE.
 *  A peak-hold that decays 0.5 dB/s was compared, after a move, with a hold rebuilt from scratch: before = the
 *  loudest moment of a long history, after = the loudest of 2.7 s. The longer window reads higher, so every
 *  move looked worse than it was — −2.2 dB on average with no change at all (simulated). Now: the maximum
 *  over a FIXED window (kBurstyWindowSec), not fed while the gain is settling, and not averaged until the
 *  window is full — so "before" and "after" are maxima over the same length of time. Cleared on a gain write
 *  AND on a channel move (it held the LAST station's level and decayed for half a minute). */
constexpr double kBurstyWindowSec = 2.5;
class WindowMax {
public:
    void configure(int cap) { cap = std::max(1, cap); if (cap != cap_) { cap_ = cap; buf_.assign((size_t)cap_, -200.0f); } reset(); }
    void reset() { n_ = 0; head_ = 0; }
    void push(float v) {
        if (cap_ <= 0) configure(1);
        buf_[(size_t)head_] = v; head_ = (head_ + 1) % cap_; if (n_ < cap_) ++n_;
    }
    bool  full() const { return cap_ > 0 && n_ >= cap_; }
    int   size() const { return n_; }
    int   capacity() const { return cap_; }
    float max() const { float m = -200.0f; for (int i = 0; i < n_; ++i) m = std::max(m, buf_[(size_t)i]); return m; }
private:
    std::vector<float> buf_;
    int cap_ = 0, n_ = 0, head_ = 0;
};

}  // namespace vibeagc
