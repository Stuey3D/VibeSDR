// psk_synth.h — synthetic PSK31 / PSK63 for the PSK skimmer's bench (psk_bench.cpp) and tests (vibeserver/test-psk.cpp),
// built from the published description: Varicode characters separated by "00", a ZERO sent as a phase reversal, each
// reversal shaped by a cosine so the amplitude passes through zero (the raised-cosine pulse, two symbols long), idle =
// continuous reversals. Signals in noise at an SNR quoted in 2.5 kHz (the signal's mean power over the noise in 2.5 kHz),
// drift, and the character error rate of a decode.
#pragma once
#include "../decoders/psk_skimmer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace psksynth {
using cf = std::complex<float>;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() { double u = uni(), v = uni(); if (u < 1e-300) u = 1e-300; return std::sqrt(-2 * std::log(u)) * std::cos(2 * M_PI * v); }
};

/** The bit stream for `text`: `idleBefore` zeros, each character's code + "00", `idleAfter` zeros. */
inline std::vector<int> bits(const std::string& text, int idleBefore = 32, int idleAfter = 32) {
    std::vector<int> b((size_t)idleBefore, 0);
    for (unsigned char c : text) {
        const char* v = vibe::psk::varicode(c < 128 ? c : '?');
        for (const char* p = v; *p; ++p) b.push_back(*p == '1');
        b.push_back(0); b.push_back(0);
    }
    for (int i = 0; i < idleAfter; ++i) b.push_back(0);
    return b;
}

struct Sig {
    double hz = 1000;            // offset (IQ) or audio frequency
    double baud = 31.25;
    double snrDb = 10;           // the signal's mean power over the noise in 2.5 kHz
    double driftHzPerSec = 0;
    double startSec = 0.5;
    double amp = 1;              // ★ relative level, applied after the SNR (for "+20 dB neighbour")
    std::string text;
    int idleBefore = 32, idleAfter = 32;
    double clockPpm = 0;         // the transmitter's symbol clock error
    double cubic = 0;            // ★ an overdriven transmitter: the envelope becomes a − cubic·a³ (third-order IMD)
};
/** ★ The IMD a cubic of `k` gives on the idle (two tones at ±baud/2), from the envelope itself — the bench's reference:
 *  cos(πt/T) − k·cos³ = (1 − ¾k)·cos − ¼k·cos(3πt/T), so the products sit at ¼k ÷ (1 − ¾k) of the tones. */
inline double cubicImdDb(double k) { return k <= 0 ? -200.0 : 20.0 * std::log10(0.25 * k / (1.0 - 0.75 * k)); }

/** Baseband envelope (real, ±1 at symbol centres) for a bit stream at `rate`. */
inline std::vector<float> envelope(const std::vector<int>& b, double baud, double rate, double clockPpm = 0) {
    const double T = 1.0 / (baud * (1 + clockPpm * 1e-6));
    std::vector<float> d(b.size() + 2, 0.0f);
    float cur = 1.0f;
    for (size_t k = 0; k < b.size(); ++k) { if (b[k] == 0) cur = -cur; d[k + 1] = cur; }   // d[0] = 0: a ramp up
    // d[b.size()+1] = 0: a ramp down
    const size_t n = (size_t)((double)(b.size() + 1) * T * rate);
    std::vector<float> e(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / rate / T;            // in symbols; centre k at t = k
        const size_t k = (size_t)t; const double tau = t - (double)k;
        const double w = 0.5 * (1 + std::cos(M_PI * tau));
        const float a = k < d.size() ? d[k] : 0.0f, c = k + 1 < d.size() ? d[k + 1] : 0.0f;
        e[i] = (float)(a * w + c * (1 - w));
    }
    return e;
}

/** Mix signals + noise. Noise power is 1 in the full band (IQ: complex, rate wide; real: rate/2 wide). */
inline void synth(std::vector<Sig>& sigs, double rate, bool complexOut, double seconds, Rng& rng,
                  std::vector<cf>& iq, std::vector<float>& re, double noiseOn = 1.0) {
    const size_t n = (size_t)(seconds * rate);
    if (complexOut) iq.assign(n, cf(0, 0)); else re.assign(n, 0.0f);
    const double bw = complexOut ? rate : rate / 2;
    for (auto& s : sigs) {
        auto env = envelope(bits(s.text, s.idleBefore, s.idleAfter), s.baud, rate, s.clockPpm);
        if (s.cubic > 0) for (auto& v : env) v = (float)(v - s.cubic * v * v * v);
        double ms = 0; for (float v : env) ms += v * v; ms /= std::max<size_t>(1, env.size());
        // mean power so that P / (noise in 2500 Hz) = snr
        const double p = std::pow(10.0, s.snrDb / 10.0) * 2500.0 / bw;
        const double amp = s.amp * (complexOut ? std::sqrt(p / ms) : std::sqrt(2 * p / ms));
        double ph = rng.uni() * 2 * M_PI;
        const size_t s0 = (size_t)(s.startSec * rate);
        for (size_t i = 0; i < env.size() && s0 + i < n; ++i) {
            const double f = s.hz + s.driftHzPerSec * ((double)i / rate);
            ph += 2 * M_PI * f / rate;
            if (ph > 2 * M_PI) ph -= 2 * M_PI;
            const double a = amp * env[i];
            if (complexOut) iq[s0 + i] += cf((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
            else re[s0 + i] += (float)(a * std::cos(ph));
        }
    }
    const double sg = complexOut ? std::sqrt(0.5) : 1.0;
    if (noiseOn > 0)
        for (size_t i = 0; i < n; ++i) {
            if (complexOut) iq[i] += cf((float)(sg * rng.gauss() * noiseOn), (float)(sg * rng.gauss() * noiseOn));
            else re[i] += (float)(rng.gauss() * noiseOn);
        }
}
/** Seconds a signal lasts. */
inline double duration(const Sig& s) {
    return (double)(bits(s.text, s.idleBefore, s.idleAfter).size() + 1) / s.baud;
}

// ── scoring: semi-global edit distance (the decoder may join late and stop early) ──
inline std::string squash(const std::string& s) {
    std::string o; bool sp = true;
    for (char c : s) { if (c == ' ' || c == '\n' || c == '\r') { if (!sp) o += ' '; sp = true; } else { o += c; sp = false; } }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}
/** Character errors of `got` against the best-matching span of `ref` — and the characters of ref it MISSED outside that
 *  span (a decoder that copies half the text has not scored 0 %). */
inline double cer(const std::string& refIn, const std::string& gotIn) {
    const std::string ref = squash(refIn), got = squash(gotIn);
    const size_t R = ref.size(), G = got.size();
    if (R == 0) return G ? 1.0 : 0.0;
    if (G == 0) return 1.0;
    // ★ plain edit distance over the whole text: what was missed at either end counts
    std::vector<int> prev(R + 1), cur(R + 1);
    for (size_t j = 0; j <= R; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= G; ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= R; ++j)
            cur[j] = std::min({ prev[j - 1] + (got[i - 1] == ref[j - 1] ? 0 : 1), prev[j] + 1, cur[j - 1] + 1 });
        std::swap(prev, cur);
    }
    return (double)prev[R] / (double)R;
}

static const char* const kQso[] = {
    "cq cq cq de g4abc g4abc g4abc pse k\n",
    "g4abc de dl1xyz dl1xyz - good morning om, tnx for the call. ur rst 599 599. name hans, qth berlin. hw cpy? g4abc de dl1xyz kn\n",
    "dl1xyz de g4abc - fb hans, rst 579. name john, qth leeds. rig ic-7300 50w, ant dipole. 73 es gl. dl1xyz de g4abc sk\n",
};
} // namespace psksynth
