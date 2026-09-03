// vibe_dab_ofdm.h — DAB OFDM: fractional frequency offset, symbol extraction, DQPSK demapping.
//
// Stage 3, on top of vibe_dab_sync.h. What happens to one frame:
//
//   null | phase reference | 75 data symbols        (Mode I: L = 76 counting the reference)
//         \__ symbol 0 __/   \__ symbols 1..75 __/
//
// ★★★ DAB IS DIFFERENTIALLY ENCODED, WHICH IS WHY THERE IS NO CARRIER RECOVERY HERE.
//     Each symbol's phase is relative to the SAME CARRIER in the PREVIOUS symbol, so a constant
//     phase rotation — from a frequency offset, or from not knowing the transmitter's absolute
//     phase at all — cancels in the subtraction. That is the whole reason DAB survives on a cheap
//     dongle with a several-kHz error, and it means we need only the FRACTIONAL offset (to keep
//     the carriers in their bins) and never the absolute one.
//
// ★★ THE FRACTIONAL OFFSET COMES FREE FROM THE CYCLIC PREFIX. The guard interval is a copy of the
//    tail of the useful part, so correlating a symbol's first `guard` samples against the samples
//    one useful-period later gives a complex number whose ANGLE is the residual frequency error
//    scaled by the carrier spacing. No pilots, no search — one dot product per symbol.
//    ★ It only resolves +/- half a carrier spacing (+/-500 Hz in Mode I). The INTEGER part shifts
//      the whole spectrum by whole bins and is recovered later from the phase reference symbol,
//      which is a known sequence. Both are needed; this is the cheap half.
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "vibe_dab_modes.h"
#include "vibe_dab_sync.h"

namespace vibedab {

using C32 = std::complex<float>;

/** Fractional carrier-frequency offset, in CARRIER SPACINGS (so -0.5 .. +0.5).
 *
 *  Correlates the cyclic prefix with its copy one useful-period later, over `symbols` symbols,
 *  accumulating before taking the angle — ★ accumulating the COMPLEX SUM and then taking one
 *  angle is not the same as averaging the angles, and is the correct thing: it weights each
 *  symbol by its own confidence and cannot be dragged about by the +/-pi wrap of a noisy one.
 *
 *  @param x       samples starting at the first symbol AFTER the null
 *  @param n       how many samples are available
 */
inline float fractionalOffset(const Cplx* x, size_t n, const Mode& m, int symbols = 8) {
    const size_t U = size_t(m.usefulSamples), G = size_t(m.guardSamples), S = U + G;
    if (!x || n < S * size_t(symbols)) return 0.0f;
    double sr = 0.0, si = 0.0;
    for (int s = 0; s < symbols; ++s) {
        const Cplx* p = x + size_t(s) * S;
        for (size_t i = 0; i < G; ++i) {
            // conj(prefix) * (its copy one useful period later)
            const float ar = p[i].re,       ai = p[i].im;
            const float br = p[i + U].re,   bi = p[i + U].im;
            sr += double(ar) * br + double(ai) * bi;
            si += double(ar) * bi - double(ai) * br;
        }
    }
    if (sr == 0.0 && si == 0.0) return 0.0f;
    // angle / 2pi = fraction of a carrier spacing
    return float(std::atan2(si, sr) / (2.0 * M_PI));
}

/** Turn the fractional offset into Hz for the DX panel. */
inline float offsetHz(float fractional, const Mode& m) { return fractional * float(m.spacingHz); }

/** De-rotate `n` samples by `cyclesPerSample` (a fraction of a cycle per sample).
 *  ★ Recurrence rather than a sin/cos per sample: at 2.048 MSPS a trig call per sample is the
 *    difference between real time and not on a Pi. Renormalised every 1024 steps because the
 *    recurrence drifts in magnitude, which would otherwise scale the constellation slowly. */
inline void derotate(Cplx* x, size_t n, double cyclesPerSample) {
    const double w = -2.0 * M_PI * cyclesPerSample;
    const double cs = std::cos(w), sn = std::sin(w);
    double cr = 1.0, ci = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double xr = x[i].re, xi = x[i].im;
        x[i].re = float(xr * cr - xi * ci);
        x[i].im = float(xr * ci + xi * cr);
        const double nr = cr * cs - ci * sn, ni = cr * sn + ci * cs;
        cr = nr; ci = ni;
        if ((i & 1023u) == 1023u) {          // renormalise
            const double mag = std::sqrt(cr * cr + ci * ci);
            if (mag > 0) { cr /= mag; ci /= mag; }
        }
    }
}

/** ★★ DQPSK: the phase DIFFERENCE between the same carrier in consecutive symbols carries two
 *  bits. ETSI EN 300 401 maps them so that the pair is Gray-coded around the circle, which is why
 *  a wrong decision at the boundary costs one bit rather than two.
 *
 *  Returns the two soft bits for one carrier. ★ SOFT, not hard: the Viterbi decoder downstream
 *  gains 2 dB from soft decisions, which on a marginal mux is the difference between audio and
 *  silence. Scaled to roughly +/-127 so the decoder can work in 8-bit.
 */
struct SoftBits { int8_t b0, b1; };

inline SoftBits dqpskSoft(C32 cur, C32 prev) {
    // cur * conj(prev) — the differential phase.
    const float re = cur.real() * prev.real() + cur.imag() * prev.imag();
    const float im = cur.imag() * prev.real() - cur.real() * prev.imag();
    // ★ The two bits are the two axes of the rotated constellation: pi/4 offset means the decision
    //   boundaries are the axes themselves, so the soft value IS the coordinate. No trig.
    const float mag = std::sqrt(re * re + im * im);
    const float scale = mag > 1e-12f ? 127.0f / mag : 0.0f;
    auto clamp8 = [](float v) -> int8_t {
        if (v >  127.0f) return  127;
        if (v < -127.0f) return -127;
        return int8_t(v);
    };
    return { clamp8(re * scale), clamp8(im * scale) };
}

/** Which FFT bin holds carrier k? DAB numbers carriers -K/2..+K/2 EXCLUDING zero (the centre
 *  carrier is not transmitted — EN 300 401 clause 14.5, "for k = 0, z = 0"), and the FFT puts
 *  negative frequencies in the upper half.
 *  ★ Getting this wrong mirrors the spectrum and every bit is noise, which is indistinguishable
 *    from "the demodulator does not work" — hence the test. */
inline int binForCarrier(int k, int fftSize) {
    return k >= 0 ? k : fftSize + k;
}

/** The carriers of one symbol, in DAB order (-K/2 .. -1, +1 .. +K/2), skipping DC. */
inline void carriersFromFft(const C32* spectrum, int fftSize, int carriers, C32* out) {
    const int half = carriers / 2;
    int j = 0;
    for (int k = -half; k <= half; ++k) {
        if (k == 0) continue;                       // centre carrier is not transmitted
        out[j++] = spectrum[binForCarrier(k, fftSize)];
    }
}

}  // namespace vibedab
