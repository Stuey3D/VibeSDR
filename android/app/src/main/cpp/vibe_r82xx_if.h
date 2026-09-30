// vibe_r82xx_if.h — the R820T/R828D's INTERMEDIATE FREQUENCY, as librtlsdr derives it, and what a
// mismatch between the tuner's copy and the RTL2832's copy does to the dial.
//
// ★★★ WHY THIS EXISTS — KIKO'S +1.950 MHz (Moto G 2014, Android 5.1, RTL-SDR Blog V3, Lite 11 b6,
//     2026-09-30). Every station sat a CONSTANT 1.95 MHz from the dial (107.7 heard at 109.65, 93.7
//     at 95.65). Constant in Hz rules out ppm; bigger than half the capture rules out anything in
//     our DSP (a shift we apply in software cannot reach outside ±rate/2). What CAN move the whole
//     dial by a constant is the IF: librtlsdr keeps it TWICE —
//       • the tuner's `priv->int_freq`, added to every tune:   LO = dial + int_freq
//       • the RTL2832's IF register (rtlsdr_set_if_freq), the frequency its DDC mixes back to 0 Hz
//     — and a station at DC is then RF = LO − IF_demod = dial + (int_freq − IF_demod).
//     librtlsdr always writes the two together, from ONE thread. r82xx_init() (run by
//     rtlsdr_set_direct_sampling(dev, 0), which Android sends on EVERY server start) resets
//     int_freq to 3.57 MHz; r820t_set_bw() (the IF filter, and every sample-rate change) sets it
//     from the bandwidth — and a 2.0 MHz or 2.048 MHz filter gives 1.625 MHz. 3.570 − 1.625 =
//     1.945 MHz: Kiko's number, to his reading precision. Two threads interleaving those two
//     sequences leave the halves disagreeing, and nothing ever rewrites them until the next
//     filter write.
// ★ This header mirrors r82xx_set_bandwidth() from osmocom rtl-sdr 2.0.3 (the tag
//   sdr-kit/build-librtlsdr.sh builds) so the diagnostics can print what librtlsdr's int_freq is
//   for the filter we asked for, and so test-r82xx-if.cpp can pin the arithmetic.
#pragma once
#include <cstdint>

namespace vibertl {

/** librtlsdr's R82XX_IF_FREQ — what r82xx_init() and the direct-sampling-off path set. */
constexpr int32_t kR82xxInitIfHz = 3570000;

/** The int_freq r82xx_set_bandwidth() leaves for a requested filter width `bw` (Hz). A request of 0
 *  means "the sample rate" to librtlsdr (rtlsdr_set_tuner_bandwidth), so pass the rate for 0. */
inline int32_t r82xxIntFreqForBw(int32_t bw) {
    static const int32_t lp[] = { 1700000, 1600000, 1550000, 1450000, 1200000,
                                  900000, 700000, 550000, 450000, 350000 };
    const int32_t hp1 = 350000, hp2 = 380000;
    if (bw > 7000000) return 4570000;
    if (bw > 6000000) return 4570000;
    if (bw > lp[0] + hp1 + hp2) return 3570000;
    int32_t f = 2300000, real = 0;
    if (bw > lp[0] + hp1) { bw -= hp2; f += hp2; real += hp2; }
    if (bw > lp[0])       { bw -= hp1; f += hp1; real += hp1; }
    int i = 0;
    for (; i < (int)(sizeof lp / sizeof lp[0]); ++i) if (bw > lp[i]) break;
    --i;
    if (i < 0) i = 0;   // unreachable after the two subtractions above; librtlsdr would read lp[-1]
    real += lp[i];
    return f - real / 2;
}

/** Where a station at the DDC's 0 Hz really is, relative to the dial, when the tuner's int_freq and
 *  the demodulator's IF disagree (R82xx: LO above the wanted signal). 0 when they agree. */
constexpr int32_t dialErrorHz(int32_t tunerIntFreq, int32_t demodIf) { return tunerIntFreq - demodIf; }

}  // namespace vibertl
