// vibe_dab_modes.h — the four DAB transmission modes.
//
// Source: ETSI EN 300 401, as tabulated by Rohde & Schwarz ("Understanding Digital Audio
// Broadcasting"), which is where these numbers were taken from — Stuart shared the slide
// 2026-09-04 because the parameters are what the OFDM acquisition is built around.
//
// ★★★ EVERY MODE OCCUPIES THE SAME 1.536 MHz. carriers x spacing is 1 536 000 Hz in all four:
//     1536x1k, 384x4k, 192x8k, 768x2k. That is the invariant worth knowing, because it means the
//     CAPTURE requirement never changes with the mode — the gate in vibe_dab_channels.h is right
//     for all of them, and a future L-band mode does not move it.
//
// ★★ THE UK IS MODE I, and Band III is Mode I everywhere. The others exist for L-band and
//    satellite (Mode II/IV) and for the 8 kHz-spaced Mode III. We implement Mode I first and keep
//    the table complete, because a demodulator that has hard-coded 1536 in forty places cannot
//    later be told otherwise.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vibedab {

struct Mode {
    int      number;          ///< I, II, III, IV as 1..4
    int      symbolsPerFrame; ///< OFDM symbols in a frame, EXCLUDING the null symbol
    int      carriers;        ///< active carriers
    int      spacingHz;       ///< carrier spacing
    // ★★★ TIMING IN SAMPLES AT 2.048 MHz, NOT IN MICROSECONDS. See the note below.
    int      usefulSamples;   ///< T_u, the FFT length at 2.048 MHz
    int      guardSamples;    ///< cyclic prefix
    int      nullSamples;     ///< the null symbol — what acquisition hunts for
    int      frameSamples;    ///< null + symbolsPerFrame x (useful + guard), EXACTLY
};

/* ★★★ WHY SAMPLES AND NOT MICROSECONDS — the tests found this, which is the point of them.
 *
 *  The published tables (ETSI, and the Rohde & Schwarz slide Stuart shared) give the timings in
 *  ROUNDED microseconds: Mode I "1246 us", Mode II "312 us". Transcribed literally they do not add
 *  up — null + N x symbol came out 36 us long for Modes II and III and 7 us short for Mode I, and
 *  the frame-duration check failed. Nothing was mistyped; the SOURCE is rounded, because the real
 *  figures are not whole microseconds (Mode I's guard is 246.09375 us).
 *
 *  ★★ In samples at 2.048 MHz every one of them is an exact integer, and every frame adds up to
 *     the microsecond: Mode I is 2656 + 76 x 2552 = 196608 samples = 96 ms EXACTLY. So that is how
 *     they are stored, and the arithmetic identity becomes a real test rather than one with a
 *     tolerance hiding a transcription error.
 *  ★ It is also the form the demodulator wants: it counts samples, never microseconds.
 */
inline constexpr uint32_t kCanonicalRateHz = 2048000;

inline constexpr Mode kModes[] = {
    // number, syms, carriers, spacing, useful, guard, null, frame  (samples @ 2.048 MHz)
    {  1,  76, 1536, 1000, 2048, 504, 2656, 196608 },
    {  2,  76,  384, 4000,  512, 126,  664,  49152 },
    {  3, 153,  192, 8000,  256,  63,  345,  49152 },
    {  4,  76,  768, 2000, 1024, 252, 1328,  98304 },
};

inline constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

/** Total symbol length in samples at the canonical rate. */
inline constexpr int symbolSamples(const Mode& m) { return m.usefulSamples + m.guardSamples; }

/** Nominal durations, DERIVED — never stored, so they cannot drift from the samples. */
inline constexpr int frameUs(const Mode& m) {
    return int((int64_t(m.frameSamples) * 1000000ll) / kCanonicalRateHz);
}
inline constexpr int usefulUs(const Mode& m) { return 1000000 / m.spacingHz; }

inline constexpr const Mode& modeI() { return kModes[0]; }

inline const Mode* modeByNumber(int n) {
    for (size_t i = 0; i < kModeCount; ++i) if (kModes[i].number == n) return &kModes[i];
    return nullptr;
}

/** Occupied bandwidth = carriers x spacing. The same 1.536 MHz for every mode. */
inline constexpr uint32_t occupiedHz(const Mode& m) {
    return uint32_t(m.carriers) * uint32_t(m.spacingHz);
}

/** FFT size at an arbitrary input rate — the transform the demodulator will actually run.
 *  ★ At the canonical rate this is simply usefulSamples (2048 for Mode I), which is why 2.048 MSPS
 *    is canonical. An RTL-SDR running 2.4 MSPS gets 2400, and will need resampling or a longer
 *    transform; that decision belongs to the demodulator, not to this table. */
inline constexpr int fftSizeFor(const Mode& m, uint32_t sampleRateHz) {
    return int((uint64_t(sampleRateHz) * uint64_t(m.usefulSamples)) / kCanonicalRateHz);
}

/** Guard-interval samples at a given rate — the correlation window for fine timing. */
inline constexpr int guardSamplesFor(const Mode& m, uint32_t sampleRateHz) {
    return int((uint64_t(sampleRateHz) * uint64_t(m.guardSamples)) / kCanonicalRateHz);
}

}  // namespace vibedab
