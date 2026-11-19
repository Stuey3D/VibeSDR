// decoders/nav_tx.h — V12 our own VOR and ILS transmitters (complex baseband), for test-vor, the server's end-to-end clip and
// the benchmark row. The signals as nav_rx.h describes them (ICAO Annex 10 Vol. I §3.1, §3.3). docs/v12/VOR-ILS.md.
#pragma once
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace vibe {
namespace nav {
namespace tx {

/** Morse keying of `word` at `wpm` (1 = key down), one sample per 1/rate s; ITU timing (dot 1, dash 3, gaps 1 / 3). */
inline std::vector<uint8_t> keying(const std::string& word, double wpm, double rate) {
    static const char* const codes[36] = { ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
        "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
        "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----." };
    const int dot = std::max(1, (int)std::lround(1.2 / wpm * rate));
    std::vector<uint8_t> k;
    for (size_t i = 0; i < word.size(); ++i) {
        const char c = word[i];
        const char* m = c >= 'A' && c <= 'Z' ? codes[c - 'A'] : c >= '0' && c <= '9' ? codes[26 + c - '0'] : nullptr;
        if (!m) continue;
        for (const char* p = m; *p; ++p) {
            k.insert(k.end(), (size_t)(*p == '-' ? 3 * dot : dot), 1);
            if (p[1]) k.insert(k.end(), (size_t)dot, 0);
        }
        if (i + 1 < word.size()) k.insert(k.end(), (size_t)(3 * dot), 0);
    }
    return k;
}

struct VorOpt {
    double rate = 37500;      ///< complex samples a second
    double bearing = 0;       ///< the radial (deg)
    double varDepth = 0.30, subDepth = 0.30, devHz = 480, identDepth = 0.10;
    double offsetHz = 0;      ///< carrier off the dial
    double snrDb = 40;        ///< carrier to noise in 25 kHz
    std::string ident = "LON";
    double wpm = 7, identEvery = 10;   ///< the ident starts every this many seconds
    double ref30 = 30.0;      ///< the 30 Hz (a station's rate is exact; a test can move it)
    double voiceDepth = 0;    ///< voice-like AM on top (0 = none)
    uint32_t seed = 1;
    bool doppler = false;     ///< a DVOR's subcarrier carries the sidebands' FM at the variable's phase — same at the receiver
};
/** Speech-like modulation (glottal pulses through moving formants), peak ≈ 1 — the talk-off source. */
inline std::vector<float> speechLike(double sec, double fs, uint32_t seed) {
    std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 1); std::normal_distribution<double> n(0, 1);
    std::vector<float> a; a.reserve((size_t)(sec * fs));
    double f0 = 120 + 100 * u(r), ph = 0;
    struct Res { double f, bw, y1 = 0, y2 = 0; } F[3] = { { 500, 80 }, { 1500, 100 }, { 2500, 120 } };
    double tgt[3] = { 500, 1500, 2500 };
    int left = 0; bool voiced = true, silent = false;
    while (a.size() < sec * fs) {
        if (left-- <= 0) {
            left = (int)((0.08 + 0.32 * u(r)) * fs);
            silent = u(r) < 0.15; voiced = u(r) < 0.8;
            tgt[0] = 250 + 600 * u(r); tgt[1] = 800 + 1600 * u(r); tgt[2] = 2000 + 1200 * u(r);
            f0 = std::max(85.0, std::min(260.0, f0 * (0.85 + 0.3 * u(r))));
        }
        double ex = 0;
        if (!silent) {
            if (voiced) { ph += f0 * (1 + 0.01 * n(r)) / fs; if (ph >= 1) { ph -= 1; ex = 1.0; } }
            else ex = 0.15 * n(r);
        }
        double y = ex;
        for (int k = 0; k < 3; ++k) {
            F[k].f += (tgt[k] - F[k].f) * 0.0005;
            const double R = std::exp(-M_PI * F[k].bw / fs), c = 2 * R * std::cos(2 * M_PI * F[k].f / fs);
            const double o = y + c * F[k].y1 - R * R * F[k].y2;
            F[k].y2 = F[k].y1; F[k].y1 = o; y = o * (1 - R);
        }
        a.push_back((float)y);
    }
    float mx = 1e-9f; for (float v : a) mx = std::max(mx, std::fabs(v));
    for (float& v : a) v /= mx;
    return a;
}
inline void addNoise(std::vector<std::complex<float>>& x, double snrDb, double rate, uint32_t seed) {
    std::mt19937 r(seed ^ 0x5bd1e995u); std::normal_distribution<float> n(0.f, 1.f);
    // carrier power 1; noise power in 25 kHz = 10^(−snr/10) → per-sample variance scaled to the full rate
    const float s = (float)std::sqrt(std::pow(10.0, -snrDb / 10) * rate / 25000.0 / 2.0);
    for (auto& v : x) v += std::complex<float>(s * n(r), s * n(r));
}
/** `sec` seconds of a VOR at complex baseband (carrier amplitude 1). */
inline std::vector<std::complex<float>> vor(double sec, const VorOpt& o) {
    const size_t N = (size_t)(sec * o.rate);
    std::vector<std::complex<float>> x(N);
    const std::vector<uint8_t> key = keying(o.ident, o.wpm, o.rate);
    const size_t every = (size_t)(o.identEvery * o.rate);
    std::vector<float> voice;
    if (o.voiceDepth > 0) voice = speechLike(sec, o.rate, o.seed * 7 + 3);
    const double th = o.bearing * M_PI / 180.0;
    for (size_t i = 0; i < N; ++i) {
        const double t = i / o.rate;
        const double w = 2 * M_PI * o.ref30 * t;
        double m = o.varDepth * std::cos(w - th);
        m += o.subDepth * std::cos(2 * M_PI * 9960.0 * t + (o.devHz / o.ref30) * std::sin(w));
        const size_t ki = every ? (i % every) : i;
        if (ki < key.size() && key[ki]) m += o.identDepth * std::cos(2 * M_PI * 1020.0 * t);
        if (!voice.empty()) m += o.voiceDepth * voice[i];
        const double a = std::max(0.0, 1.0 + m);
        const double ph = 2 * M_PI * o.offsetHz * t;
        x[i] = std::complex<float>((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
    }
    addNoise(x, o.snrDb, o.rate, o.seed);
    return x;
}

struct IlsOpt {
    double rate = 37500;
    double m90 = 0.20, m150 = 0.20;   ///< localiser 20 % + 20 % on the course; a glideslope 40 % + 40 %
    double identDepth = 0.05;         ///< a localiser's ident (a glideslope has none: 0)
    std::string ident = "ILL";
    double wpm = 7, identEvery = 8;
    double offsetHz = 0, snrDb = 40;
    uint32_t seed = 2;
};
inline std::vector<std::complex<float>> ils(double sec, const IlsOpt& o) {
    const size_t N = (size_t)(sec * o.rate);
    std::vector<std::complex<float>> x(N);
    const std::vector<uint8_t> key = keying(o.ident, o.wpm, o.rate);
    const size_t every = (size_t)(o.identEvery * o.rate);
    for (size_t i = 0; i < N; ++i) {
        const double t = i / o.rate;
        // the 90 and 150 Hz tones are phase-locked harmonics of 30 Hz in a real ILS
        double m = o.m90 * std::cos(2 * M_PI * 90.0 * t) + o.m150 * std::cos(2 * M_PI * 150.0 * t);
        const size_t ki = every ? (i % every) : i;
        if (o.identDepth > 0 && ki < key.size() && key[ki]) m += o.identDepth * std::cos(2 * M_PI * 1020.0 * t);
        const double a = std::max(0.0, 1.0 + m);
        const double ph = 2 * M_PI * o.offsetHz * t;
        x[i] = std::complex<float>((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
    }
    addNoise(x, o.snrDb, o.rate, o.seed);
    return x;
}
/** Plain airband AM speech (the talk-off case for a bearing): 80 % peak modulation, noise. */
inline std::vector<std::complex<float>> amSpeech(double sec, double rate, double snrDb, uint32_t seed) {
    const std::vector<float> v = speechLike(sec, rate, seed);
    std::vector<std::complex<float>> x(v.size());
    for (size_t i = 0; i < v.size(); ++i) x[i] = std::complex<float>((float)std::max(0.0, 1.0 + 0.8 * v[i]), 0.f);
    addNoise(x, snrDb, rate, seed);
    return x;
}

} // namespace tx
} // namespace nav
} // namespace vibe
