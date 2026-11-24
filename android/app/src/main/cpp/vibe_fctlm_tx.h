// vibe_fctlm_tx.h — a FUNcube (AO-40 FEC) BPSK 1200 TRANSMITTER, for tests and the benchmark (V12 beta90): frames →
// vibefc::encodeFrame → differential code → BPSK with raised-cosine pulses (drawn from the continuous formula at the
// audio rate, never the receiver's taps) → an SSB-audio tone at `carrierHz`, with a frequency offset, a Doppler ramp,
// a clock error and white noise. The receiver can only be trusted as far as this is independent of it.
#pragma once

#include "vibe_fctlm.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace vibefc {
namespace tx {

struct Cfg {
    double rate = 48000, baud = 1200;
    double carrierHz = 1200;      ///< where the signal sits in the audio
    double offsetHz = 0;          ///< a residual the receiver must find
    double rampHzS = 0;           ///< Doppler residual slope
    double clockPpm = 0;          ///< the satellite's symbol clock error (+ = slower). ★ AO-73 on air: −1400 (1201.6 Bd)
    double phase = 0.3;           ///< carrier start phase (rad)
    double ebn0Db = 99;           ///< noise: Eb/N0 in the 2.4 kHz audio (99 = none)
    double amp = 0.3;
    bool differential = true;     ///< AO-40's differential code before the modulator
    /** ★★★ 11-24 ON AIR: AO-73 sends a 1 as NO phase change (the framer's differential-inverted form). true = as the
     *  satellite does; false = the other convention (the framer tries both). */
    bool invertBits = true;
    double gapSym = 800;          ///< symbols between frames (5 s period − 5200 symbols at 1200 Bd)
    uint32_t seed = 1;
};

/** A raised-cosine pulse (α = 0.5) at time t (in symbols). */
inline double rc(double t) {
    const double a = 0.5;
    if (std::fabs(t) < 1e-9) return 1.0;
    const double den = 1.0 - 4.0 * a * a * t * t;
    const double s = std::sin(M_PI * t) / (M_PI * t);
    if (std::fabs(den) < 1e-9) return (M_PI / 4.0) * s;
    return s * std::cos(M_PI * a * t) / den;
}

/** Frames (256 bytes each) → audio. Random filler symbols between frames (a hard case for a false sync). */
inline std::vector<float> modulate(const std::vector<std::vector<uint8_t>>& frames, const Cfg& c, double leadSec = 1.0, double tailSec = 1.0) {
    std::mt19937 rng(c.seed);
    std::vector<uint8_t> bits;
    auto filler = [&](int n) { for (int i = 0; i < n; ++i) bits.push_back(uint8_t(rng() & 1)); };
    filler((int)(leadSec * c.baud));
    for (auto& f : frames) {
        uint8_t ch[kSyms];
        encodeFrame(f.data(), ch);
        bits.insert(bits.end(), ch, ch + kSyms);
        filler((int)c.gapSym);
    }
    filler((int)(tailSec * c.baud));
    // differential code: d[n] = d[n−1] XOR b[n]
    std::vector<double> sym(bits.size());
    int d = 0;
    for (size_t i = 0; i < bits.size(); ++i) {
        int b = bits[i] ^ (c.invertBits ? 1 : 0);
        if (c.differential) { d ^= b; b = d; }
        sym[i] = b ? 1.0 : -1.0;
    }
    const double tsym = (1.0 + c.clockPpm * 1e-6) / c.baud;   // the satellite's symbol period
    const size_t n = (size_t)((sym.size() * tsym) * c.rate) + 1;
    std::vector<float> out(n);
    std::normal_distribution<double> g(0.0, 1.0);
    // noise: Eb/N0 → per-sample σ. Signal power (real tone, amp A, RC pulses ≈ unit power) = A²/2 · Pp; Eb = Ps·Tb;
    // N0 = σ²/(rate/2) for real white noise.
    const double ps = c.amp * c.amp / 2.0 * 0.9;
    const double eb = ps / c.baud;
    const double n0 = eb / std::pow(10.0, c.ebn0Db / 10.0);
    const double sigma = c.ebn0Db >= 98 ? 0.0 : std::sqrt(n0 * c.rate / 2.0);
    double ph = c.phase;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / c.rate;
        const double k = t / tsym;
        const long long kc = (long long)std::floor(k);
        double bb = 0;
        for (long long m = kc - 4; m <= kc + 5; ++m) if (m >= 0 && m < (long long)sym.size()) bb += sym[(size_t)m] * rc(k - double(m) - 0.5);
        const double f = c.carrierHz + c.offsetHz + c.rampHzS * t;
        ph += 2 * M_PI * f / c.rate;
        if (ph > M_PI) ph -= 2 * M_PI;
        out[i] = float(c.amp * bb * std::cos(ph) + (sigma > 0 ? sigma * g(rng) : 0.0));
    }
    return out;
}

/** A plausible real-time frame for the tests: satellite id 2 (FUNcube-1), frame type `type` (0–23, WO1 = 0), fields filled. */
inline std::vector<uint8_t> sampleFrame(int type, uint32_t seq, const char* fitter = nullptr) {
    std::vector<uint8_t> d(kDataBytes, 0);
    size_t pos = 0;
    auto put = [&](uint32_t v, int w) { for (int i = w - 1; i >= 0; --i, ++pos) if ((v >> i) & 1u) d[pos >> 3] |= uint8_t(0x80 >> (pos & 7)); };
    put(2, 2); put((uint32_t)type, 6);
    for (int i = 0; i < 8; ++i) put(1000u + (uint32_t)i * 111u, 16);    // EPS 16-bit (photo V ×3, I, Vbat, Isys, reboots, swerr)
    for (int i = 0; i < 8; ++i) put(40u + (uint32_t)i, 8);
    for (int i = 0; i < 10; ++i) put(300u + (uint32_t)i, 10);
    for (int i = 0; i < 6; ++i) put(10u + (uint32_t)i, 8);
    for (int i = 0; i < 4; ++i) put(20u + (uint32_t)i, 8);
    put(77, 8); put(78, 8); put(1, 1); put(1, 1); put(0, 1); put(1, 1);
    put(seq, 24); put(5, 6); put(3, 5); put(1, 1);
    for (int i = 0; i < 7; ++i) put(1, 1);
    put(1, 1); put(0, 1);   // eclipse, safe
    put(0, 1); put(1, 1); put(0, 1);
    if (fitter) for (size_t i = 0; fitter[i] && i < 200; ++i) d[56 + i] = uint8_t(fitter[i]);
    else for (size_t i = 56; i < 256; ++i) d[i] = uint8_t(i * 7 + seq);
    return d;
}

}} // namespace vibefc::tx
