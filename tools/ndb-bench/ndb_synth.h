// ndb_synth.h — synthetic NDBs for the NDB skimmer's bench (ndb_bench.cpp) and tests (vibeserver/test-ndb.cpp): a steady
// carrier, its ident keyed as ICAO Annex 10 describes (A2A: a 400 / 1020 Hz tone at 95 % depth on the carrier; A1A: the
// carrier itself), Morse timing per ITU-R M.1677 (a dot is 1.2/WPM s, machine-exact), repeated every few seconds; complex
// Gaussian noise. SNR is quoted as CARRIER over noise in 2.5 kHz (the CW bench's convention).
#pragma once
#include "decoders/cw_skimmer.h"   // cw::morseCode

#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace ndbsynth {
using cf = std::complex<float>;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() { double u = uni(), v = uni(); if (u < 1e-300) u = 1e-300; return std::sqrt(-2 * std::log(u)) * std::cos(2 * M_PI * v); }
};

struct Beacon {
    std::string ident = "WOD";
    double offsetHz = 0;       // from the slice centre
    double snrDb = 20;         // carrier / noise in 2.5 kHz
    double toneHz = 1020;      // 0 = A1A (the carrier keyed)
    double wpm = 7;
    double periodSec = 10;     // the ident starts every periodSec
    double phaseSec = 1;       // first ident starts here
    int    repeats = 1;        // idents sent back to back in each period ("WOD WOD")
    double depth = 0.95;       // A2A modulation depth
    double qsbDb = 0, qsbHz = 0.05;
};

/** The keying (0/1, raised-cosine 5 ms edges) of one beacon, sampled at `rate`, for `seconds`. */
inline std::vector<float> keying(const Beacon& b, double rate, double seconds) {
    const size_t n = (size_t)(seconds * rate);
    std::vector<float> k(n, 0.0f);
    const double u = 1.2 / b.wpm;
    // the marks of one ident group, in seconds from its start
    std::vector<std::pair<double, double>> marks;
    double t = 0;
    for (int r = 0; r < b.repeats; ++r) {
        for (size_t i = 0; i < b.ident.size(); ++i) {
            const std::string code = vibe::cw::morseCode(std::string(1, b.ident[i]));
            for (size_t e = 0; e < code.size(); ++e) {
                const double len = (code[e] == '-' ? 3 : 1) * u;
                marks.push_back({ t, t + len });
                t += len + (e + 1 < code.size() ? u : 0);
            }
            t += 3 * u;
        }
        t += 4 * u;   // ★ the word gap (7 dots) after the char gap already added
    }
    const double rise = 0.005;
    for (double g = b.phaseSec; g < seconds; g += b.periodSec) {
        for (auto& m : marks) {
            const double a = g + m.first, z = g + m.second;
            const size_t ia = (size_t)std::max(0.0, a * rate), iz = std::min(n, (size_t)std::max(0.0, z * rate));
            for (size_t i = ia; i < iz; ++i) {
                const double tt = i / rate;
                double v = 1.0;
                if (tt - a < rise) v = 0.5 - 0.5 * std::cos(M_PI * (tt - a) / rise);
                if (z - tt < rise) v = std::min(v, 0.5 - 0.5 * std::cos(M_PI * (z - tt) / rise));
                k[i] = (float)v;
            }
        }
    }
    return k;
}

/** Mix beacons + noise into `iq` (noise power 1 over the whole band). */
inline void synth(const std::vector<Beacon>& bs, double rate, double seconds, Rng& rng, std::vector<cf>& iq, double noise = 1.0) {
    const size_t n = (size_t)(seconds * rate);
    iq.assign(n, cf(0, 0));
    for (const auto& b : bs) {
        const std::vector<float> k = keying(b, rate, seconds);
        const double pc = std::pow(10.0, b.snrDb / 10.0) * 2500.0 / rate;
        const double A = std::sqrt(pc);
        double ph = rng.uni() * 2 * M_PI, tph = rng.uni() * 2 * M_PI;
        const double dph = 2 * M_PI * b.offsetHz / rate, dt = 2 * M_PI * b.toneHz / rate;
        for (size_t i = 0; i < n; ++i) {
            double a = A;
            if (b.qsbDb > 0) a *= std::pow(10.0, -b.qsbDb / 20.0 * (0.5 - 0.5 * std::cos(2 * M_PI * b.qsbHz * (double)i / rate)));
            if (b.toneHz > 0) a *= 1.0 + b.depth * k[i] * std::cos(tph);
            else a *= k[i];
            iq[i] += cf((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
            ph += dph; if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            tph += dt; if (tph > 2 * M_PI) tph -= 2 * M_PI;
        }
    }
    const double sg = std::sqrt(0.5) * noise;
    if (noise > 0) for (size_t i = 0; i < n; ++i) iq[i] += cf((float)(sg * rng.gauss()), (float)(sg * rng.gauss()));
}

} // namespace ndbsynth
