// decoders/digi_tuner.h — Band mode's per-dial receiver: ONE mode dial's USB audio cut out of the band's wide IQ slice
// (V12, 2026-10-26). What a radio set to that dial in USB would hand WSJT-X, at the mode's own rate (12 kHz).
//
// ★★★ WHY AN IQ SLICE AND TUNERS, NOT A WIDE AUDIO CHANNEL (the brief asked for the cheapest correct route):
//   • A wide AUDIO channel (one 48 kHz USB demodulator across the band) would put 40 m's WSPR, FT4, FT8 and JS8 into one
//     real signal — and then every mode's engine would need its OWN frequency shift and filter anyway, from a signal that
//     a demodulator's AGC has already pumped on the strongest station in 43 kHz. And real audio is one-sided: a 43 kHz
//     band needs 86+ kHz of audio rate, which no listener channel has.
//   • The IQ slice is what the CW skimmer already takes (local_sdr_shim.cpp: one more extract of the forward FFT every
//     listener's channel comes from, clientCentreBin; vibe_wide_tap.h on a one-pipeline radio). On the DSP thread that is
//     ONE small inverse FFT a block — the whole cost to the radio. Everything below runs on the decode thread.
//   So: one slice per band, and here one TUNER per mode dial, on the decoder's own thread.
//
// ── Per tuner, per output sample (12 kHz) ───────────────────────────────────────────────────────────────────────────────
//  1. MIX (at the slice rate R): the slice × e^(−j2π·f·t), f = dial + passband centre − slice centre — the wanted
//     passband now sits around 0 Hz. Four lanes at a time (vibedsp rotateBlockIlv: NEON / SSE2 / scalar).
//  2. DECIMATE R → 12 kHz with a POLYPHASE windowed-sinc (Kaiser, ≈ 70 dB): only the outputs are computed, each one a
//     dot product of N1 taps from the nearest of 256 phases. Its stopband starts at 12 kHz − the passband, which is all
//     that keeps aliases out of the passband — so it is SHORT (≈ 40 taps at 75 kHz).
//  3. SHAPE at 12 kHz: a complex low-pass, ±(half the passband) with a 300 Hz transition, ≈ 60 dB. This is the filter
//     that matters: it removes the LOWER sideband (RF below the dial would otherwise fold onto the audio when the real
//     part is taken) and everything above the mode's top. ≈ 145 taps, both rails dot products (dotReal — NEON / SSE2).
//  4. Up by the passband centre and take the real part → USB audio, the dial at 0 Hz.
//  5. A SLOW level control (τ ≈ 8 s) to a fixed RMS: the slice is raw, unscaled IQ — whatever the radio's gain made it —
//     and ft8_lib's waterfall maps power to dB over a fixed 127 dB range. Slow, so no slot sees it move.
// Cost at R = 75 kHz, per tuner: ≈ 75 k complex mixes + 12 k × (40 + 2 × 145) MACs ≈ 4.4 M MAC/s — under 0.4 % of an
// M-class core, a few % of one Cortex-A7 core (the FT8 decode it feeds costs more). Measured: docs/v12/DIGITAL-SPOTS.md.
#pragma once
#include <complex>
#include <cstdint>
#include <vector>

namespace vibe {
namespace digi {

class DigiTuner {
public:
    using cf32 = std::complex<float>;
    /** `dialHz`: the mode's dial (USB). `loHz`..`hiHz`: the audio passband wanted above the dial. `outRate`: Hz. */
    DigiTuner(double dialHz, float loHz, float hiHz, int outRate = 12000);

    /** One block of the band's slice: `n` IQ samples at `rate` Hz centred on RF `centreHz`. Appends the dial's audio
     *  (±1-ish, levelled) to `out`. The slice may move (the listener retuned within the band): the mix follows, the
     *  filters do not restart. A different `rate` redesigns the decimator. */
    void feed(const cf32* x, int n, double rate, double centreHz, std::vector<float>& out);

    double dialHz() const { return dial_; }
    float  loHz() const { return lo_; }
    float  hiHz() const { return hi_; }
    int    outRate() const { return fo_; }
    /** Is the whole passband inside a slice of `rate` centred on `centreHz` (its outer 4 % each side is roll-off)? */
    static bool fits(double dialHz, float loHz, float hiHz, double rate, double centreHz);
    /** Taps in the two filters (tests, the docs' cost figures). */
    int stage1Taps() const { return n1_; }
    int stage2Taps() const { return (int)h2_.size(); }
    /** The level control's gain now (tests). */
    float gain() const { return gain_; }

private:
    void design1(double rate);
    double dial_; float lo_, hi_; int fo_;
    float mid_, half_;
    // mixer
    float cr_ = 1.0f, ci_ = 0.0f;
    // stage 1: polyphase decimator
    double rate_ = 0;
    int    n1_ = 0;
    static constexpr int kPhases = 256;
    std::vector<float> h1_;           // kPhases × n1_, phase-major
    std::vector<float> br_, bi_;      // mixed input not yet consumed (separate rails)
    double pos_ = 0;                  // next output's position in br_/bi_ (input samples)
    std::vector<float> mr_, mi_;      // scratch: this block's mix
    // stage 2: complex low-pass at fo_
    std::vector<float> h2_;
    std::vector<float> zr_, zi_;      // the last h2_.size()-1 stage-1 outputs, then this block's
    // up-mix + level
    double ph2_ = 0;
    float  gain_ = 0;                 // 0 = not yet measured
    double pow_ = 0;
};

} // namespace digi
} // namespace vibe
