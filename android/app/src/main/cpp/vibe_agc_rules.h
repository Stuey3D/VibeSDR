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

}  // namespace vibeagc
