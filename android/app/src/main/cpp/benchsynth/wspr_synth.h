// wspr_synth.h — WSPR-2 test signals generated from the protocol (decoders/digimodes/wspr_decoder.h's encoder), for test-wspr
// and the bench. Real audio as a USB receiver would hand it over: a phase-continuous 4-FSK tone at its audio
// frequency, optionally drifting and fading, in white Gaussian noise whose level sets the SNR in 2500 Hz — the WSPR
// convention (signal power over the noise power in a 2500 Hz bandwidth).
// ★ Its own random generator (xorshift + Box–Muller), so a seed gives the same noise on every compiler and machine.
#pragma once
#include "../decoders/digimodes/wspr_decoder.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace wsprsynth {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return ((next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    double gauss() { return std::sqrt(-2.0 * std::log(uni())) * std::cos(2.0 * M_PI * uni()); }
};

struct Sig {
    vibe::wspr::Message msg;
    double centreHz = 1500;   // audio frequency of the signal's centre (between tones 1 and 2)
    double dt = 0;            // s after the nominal start (even minute + 1 s)
    double drift = 0;         // Hz over the transmission
    double snr = -20;         // dB in 2500 Hz
    double fadeHz = 0;        // > 0: Rayleigh-ish fading with this Doppler spread (a sum of a few slow phasors)
    bool   phaseSteps = false;   // a sender that re-programs its synthesiser each symbol (random phase per symbol)
};

/** White noise of standard deviation `sigma` into `a` (added). */
inline void addNoise(std::vector<float>& a, uint64_t seed, double sigma = 1.0) {
    Rng r(seed);
    for (float& v : a) v += (float)(sigma * r.gauss());
}

/** The signal into `a` (added; `a` starts kLeadSec before the even minute, as the decoder's window does), at `rate`, with the noise σ the SNR is quoted against. */
inline bool addSignal(std::vector<float>& a, int rate, const Sig& s, double sigma = 1.0, uint64_t seed = 7) {
    uint8_t tones[vibe::wspr::kSymbols];
    if (!vibe::wspr::encode(s.msg, tones)) return false;
    const double A = std::sqrt(2.0 * std::pow(10.0, s.snr / 10.0) * sigma * sigma * 2500.0 / (rate / 2.0));
    const double sps = rate * 8192.0 / 12000.0;
    const double f0 = s.centreHz - 1.5 * vibe::wspr::kBaud;
    const int64_t start = (int64_t)std::llround((vibe::wspr::kLeadSec + vibe::wspr::kStartSec + s.dt) * rate);
    Rng r(seed);
    // fading: three phasors of random Doppler within ±fadeHz, normalised to unit mean power
    double fd[3] = {0, 0, 0}, fp[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) { fd[i] = (r.uni() * 2 - 1) * s.fadeHz; fp[i] = r.uni() * 2 * M_PI; }
    double ph = r.uni() * 2 * M_PI;
    for (int k = 0; k < vibe::wspr::kSymbols; ++k) {
        if (s.phaseSteps) ph = r.uni() * 2 * M_PI;
        const double f = f0 + s.drift * (k - 80.5) / 161.0 + tones[k] * vibe::wspr::kBaud;
        const int64_t n0 = start + (int64_t)std::llround(k * sps), n1 = start + (int64_t)std::llround((k + 1) * sps);
        for (int64_t n = n0; n < n1; ++n) {
            ph += 2.0 * M_PI * f / rate;
            if (n < 0 || n >= (int64_t)a.size()) continue;
            double g = 1.0;
            if (s.fadeHz > 0) {
                const double t = (double)n / rate;
                double re = 0, im = 0;
                for (int i = 0; i < 3; ++i) { re += std::cos(2 * M_PI * fd[i] * t + fp[i]); im += std::sin(2 * M_PI * fd[i] * t + fp[i]); }
                g = std::sqrt((re * re + im * im) / 3.0);
            }
            a[(size_t)n] += (float)(A * g * std::cos(ph));
        }
        ph = std::fmod(ph, 2 * M_PI);
    }
    return true;
}

inline vibe::wspr::Message msg1(const char* call, const char* grid, int dbm) {
    vibe::wspr::Message m; m.type = 1; m.call = call; m.grid = grid; m.dbm = dbm; return m;
}

} // namespace wsprsynth
