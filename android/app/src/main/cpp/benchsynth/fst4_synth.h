// tools/fst4-bench/fst4_synth.h — FST4 / FST4W test signals from the frame description (decoders/digimodes/fst4_code.h):
// 4-GFSK (Gaussian frequency pulse, BT = 1.0), the tones one baud apart, tone 0 at f0, in white Gaussian noise of unit
// variance; SNR in 2500 Hz. Windows as the decoder takes them: sample 0 = 1 s before the period starts.
#pragma once
#include "../decoders/digimodes/fst4_decoder.h"

#include <cmath>
#include <random>
#include <vector>

namespace fst4synth {

inline void addSignal(std::vector<float>& win, int periodSec, const uint8_t tones[160], double f0, double dt, double snrDb,
                      double driftHz = 0) {
    const vibe::fst4::Period* p = vibe::fst4::periodFor(periodSec);
    if (!p) return;
    const double fs = 12000, baud = fs / p->nsps, ns = p->nsps;
    const double amp = std::sqrt(2.0 * std::pow(10.0, snrDb / 10.0) * 2500.0 / (fs / 2));
    // Gaussian frequency pulse, BT = 1, over three symbols
    const double bt = std::getenv("FST4_BT") ? std::atof(std::getenv("FST4_BT")) : 1.0, sig = std::sqrt(std::log(2.0)) / (2 * M_PI * bt);
    auto pulse = [&](double t) {   // t in symbols from the symbol's start; integral of a Gaussian-filtered rectangle
        auto q = [&](double x) { return 0.5 * std::erfc(-x / (std::sqrt(2.0) * sig)); };
        return q(t) - q(t - 1.0);
    };
    const long start = std::lround((vibe::fst4::kWinLeadSec + p->lead + dt) * fs);
    const long len = 160L * p->nsps;
    double ph = 0;
    for (long n = 0; n < len; ++n) {
        const double ts = (double)n / ns;           // in symbols
        const int k = (int)ts;
        double dev = 0;
        for (int j = k - 2; j <= k + 2; ++j) {
            const int tj = j < 0 ? tones[0] : j >= 160 ? tones[159] : tones[j];
            dev += tj * pulse(ts - j);
        }
        const double f = f0 + dev * baud + driftHz * ((double)n / len - 0.5);
        ph += 2 * M_PI * f / fs;
        if (ph > 2 * M_PI) ph -= 2 * M_PI;
        // a short raised-cosine ramp at each end (an eighth of a symbol)
        double env = 1;
        const double r = ns / 8;
        if (n < r) env = 0.5 * (1 - std::cos(M_PI * n / r)); else if (n > len - r) env = 0.5 * (1 - std::cos(M_PI * (len - n) / r));
        const long w = start + n;
        if (w >= 0 && w < (long)win.size()) win[(size_t)w] += (float)(amp * env * std::cos(ph));
    }
}

inline std::vector<float> noiseWindow(int periodSec, uint32_t seed) {
    const vibe::fst4::Period* p = vibe::fst4::periodFor(periodSec);
    std::vector<float> w(p ? (size_t)((p->sec + 2) * 12000) : 0);
    std::mt19937 rng(seed); std::normal_distribution<float> g(0.f, 1.f);
    for (float& v : w) v = g(rng);
    return w;
}

/** Audio window → the baseband window a SlotDecoder reads (through the real Stream, delay removed). */
inline std::vector<std::complex<float>> toBaseband(const std::vector<float>& audio, vibe::fst4::Mode m, int periodSec, double fc) {
    const vibe::fst4::Period* p = vibe::fst4::periodFor(periodSec);
    vibe::fst4::Stream s(m, *p, fc);
    std::vector<std::complex<float>> z;
    s.push(audio.data(), (int)audio.size(), z);
    std::vector<float> pad(12000 * 2, 0.f);              // flush the filters
    s.push(pad.data(), (int)pad.size(), z);
    const long d = std::lround(s.delay());
    std::vector<std::complex<float>> out;
    for (size_t i = (size_t)std::max(0L, d); i < z.size(); ++i) out.push_back(z[i]);
    return out;
}

} // namespace fst4synth
