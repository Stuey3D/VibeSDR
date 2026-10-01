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

/* ═══════════════════════ WHERE THE TUNER REALLY IS — decoded from the CHIP's own registers ═══════════════════════
 *
 * ★★★ WHY (Kiko, Lite 11 b8, 2026-10-01). Below ~100 MHz his R820T2 showed a FLAT noise floor with no stations at
 *     all — ALINE 93.7 and MASSA 94.5 gone — while 102 and 107.7 sat in the right places, and hours later the same
 *     server received 93.7 at 23 dB with nothing changed in the code. Every figure our log could print said the
 *     tune was perfect: "rc=0 readback 93.715000 MHz". Both are librtlsdr talking about ITSELF:
 *       • `readback` is rtlsdr_get_center_freq() — dev->freq, librtlsdr's memory of what it was ASKED;
 *       • rc is 0 even when the PLL did NOT lock: r82xx_set_pll() prints "[R82XX] PLL not locked!" to stderr
 *         (which goes nowhere on Android) and returns 0, and r82xx_set_freq() hands that 0 straight back.
 *     And two more ways the chip can quietly differ from what librtlsdr believes, both in osmocom 2.0.3:
 *       • r82xx_set_pll() picks the output divider from the VCO "fine tune" bits it reads BEFORE programming the
 *         new frequency — i.e. from the PREVIOUS tune. If they are not the reference value it moves the divider one
 *         step without recomputing the VCO, which puts the LO a factor of TWO from the dial: a flat floor and no
 *         stations, history-dependent, cleared by some later sequence of tunes.
 *       • r82xx_write() skips any write whose value equals its shadow copy, and stores the shadow BEFORE the I2C
 *         transfer — so one failed transfer leaves a register wrong until the next init.
 *     None of that is visible without reading the chip, so librtlsdr-android.patch now exports
 *     rtlsdr_get_r82xx_state() and this decodes the PLL registers into the frequency the mixer is actually on.
 * ★ Register meanings are osmocom's own (tuner_r82xx.c, r82xx_set_pll): R16[7:5] = divider index (/2 << n),
 *   R16[4] = refdiv2, R18[3] = SDM off, R20 = Ni + (Si << 6) with Nint = 4·Ni + Si + 13, R21/R22 = SDM low/high,
 *   R2[6] = PLL lock, R4[5:4] = VCO fine tune; VCO = 2·ref·(Nint + SDM/65536), LO = VCO / divider. */

constexpr int kR82xxReadRegs = 30;   // librtlsdr's NUM_REGS: registers 0x00..0x1d, all a read can return

struct R82xxPll {
    bool    valid = false;   // the registers needed were present
    bool    locked = false;
    int     fineTune = 0;    // R4[5:4]
    int     divider = 0;     // 2, 4 … 128 — what the chip divides the VCO by
    int     nint = 0;
    uint32_t sdm = 0;
    double  vcoHz = 0.0;
    double  loHz = 0.0;      // the frequency the mixer is really on
};

/** Decode R82xx registers read from 0x00 (hw[r] = register r) into the PLL's real state. */
inline R82xxPll r82xxDecodePll(const uint8_t* hw, int len, uint32_t xtalHz) {
    R82xxPll p;
    if (!hw || len < 0x17 || xtalHz == 0) return p;
    p.locked   = (hw[0x02] & 0x40) != 0;
    p.fineTune = (hw[0x04] >> 4) & 0x03;
    p.divider  = 2 << ((hw[0x10] >> 5) & 0x07);
    const double ref = (hw[0x10] & 0x10) ? xtalHz / 2.0 : (double)xtalHz;
    const int ni = hw[0x14] & 0x3f, si = (hw[0x14] >> 6) & 0x03;
    p.nint = 4 * ni + si + 13;
    p.sdm  = (hw[0x12] & 0x08) ? 0u : (uint32_t)(hw[0x15] | (hw[0x16] << 8));
    p.vcoHz = 2.0 * ref * (p.nint + p.sdm / 65536.0);
    p.loHz  = p.vcoHz / p.divider;
    p.valid = true;
    return p;
}

/** ★★ THE BRIDGE RETURNS FEWER REGISTERS THAN THE PLL NEEDS. An R82xx read is one I2C transaction from
 *  register 0x00 through the RTL2832, and the Sony's NooElec R820T2 accepts 16 (24 and 23 refused,
 *  2026-10-01) — so the PLL registers 0x10-0x16 never come back from the chip. Build the image from the
 *  chip where it was read and from librtlsdr's copy (`shadow[r-5]` = register r, what r82xx_write last
 *  WROTE) beyond it: a divider moved by stale fine-tune bits is in what was written, so it still shows. */
inline void r82xxComposeImage(const uint8_t* hw, int nRead, const uint8_t* shadow, uint8_t* out) {
    for (int r = 0; r < kR82xxReadRegs; ++r)
        out[r] = r < nRead ? hw[r] : (r >= 5 ? shadow[r - 5] : 0);
}

/** The divider r82xx_set_pll() starts from for an LO (before its fine-tune adjustment): the first of 2, 4 … 64
 *  that puts the VCO in [1.77, 3.54) GHz. 0 when none does (an LO below ~27.7 MHz — the PLL cannot make it). */
inline int r82xxExpectedDivider(double loHz) {
    const uint32_t khz = (uint32_t)((loHz + 500.0) / 1000.0);
    for (int d = 2; d <= 64; d <<= 1) {
        const uint64_t v = (uint64_t)khz * (uint64_t)d;
        if (v >= 1770000u && v < 3540000u) return d;
    }
    return 0;
}

/** Is the mixer where librtlsdr was asked to put it? `wantLoHz` = tuner centre + int_freq. The RTL-SDR Blog V4
 *  upconverts HF by 28.8 MHz inside librtlsdr, so that LO is accepted too. Tolerance covers the PLL's SDM step
 *  and rounding (a few hundred Hz), nothing like the MHz a wrong divider or an unlocked loop leaves. */
inline bool r82xxLoMatches(double actualLoHz, double wantLoHz) {
    const double tol = 5000.0;
    const double d1 = actualLoHz - wantLoHz, d2 = actualLoHz - (wantLoHz + 28.8e6);
    return (d1 < tol && d1 > -tol) || (d2 < tol && d2 > -tol);
}

}  // namespace vibertl
