// tools/q65-bench/q65_synth.h — Q65 test signals generated from the frame description (decoders/digimodes/q65_code.h),
// for the bench and test-q65. Continuous-phase 65-FSK, tone k at f0 + k·spacing, in white Gaussian noise of unit
// variance; SNR is in 2500 Hz (WSJT-X's convention): signal power / (noise power in 2500 Hz of the 6 kHz band).
// Optional Doppler SPREAD: the tone is multiplied by complex Gaussian fading with a Gaussian spectrum of the given
// width (Hz, two-sided −3 dB), the EME channel's usual model.
#pragma once
#include "../decoders/digimodes/q65_decoder.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace q65synth {

struct Sig {
    const uint8_t* tones = nullptr;   // 85
    int    submode = 0;
    double f0 = 1000;                 // Hz, tone 0
    double dt = 0;                    // s from the nominal start
    double snrDb = -20;
    double spreadHz = 0;              // Doppler spread (0 = none)
    double driftHz = 0;               // Hz over the transmission (linear)
};

/** Add `sig` into `win` (a window as the decoder takes it: sample 0 = 1 s before the period). */
inline void addSignal(std::vector<float>& win, int periodSec, const Sig& s, uint32_t seed) {
    const vibe::q65::Period* p = vibe::q65::periodFor(periodSec);
    if (!p) return;
    const double fs = vibe::q65::kRate, baud = fs / p->nsps, sp = baud * (1 << s.submode);
    const double amp = std::sqrt(2.0 * std::pow(10.0, s.snrDb / 10.0) * 2500.0 / (fs / 2));
    const long start = std::lround((vibe::q65::kWinLeadSec + p->lead + s.dt) * fs);
    const long len = (long)vibe::q65::kSymbols * p->nsps;
    std::mt19937 rng(seed * 7919u + 17u);
    std::normal_distribution<double> g(0.0, 1.0);
    // fading: complex Gaussian through a one-pole-pair smoother updated every 10 samples (bandwidth ≈ spreadHz)
    double fr = 1, fi = 0, sr = 0, si = 0;
    const double a = s.spreadHz > 0 ? std::exp(-3.14159265 * s.spreadHz * 10 / fs) : 0;
    const double gn = s.spreadHz > 0 ? std::sqrt((1 - a * a) * (1 - a * a) / (1 + a * a)) : 0;   // unit-power output
    if (s.spreadHz > 0) for (int k = 0; k < 20000; ++k) { sr = a * sr + g(rng); si = a * si + g(rng); fr = a * fr + sr * gn; fi = a * fi + si * gn; }
    double ph = 0;
    for (long n = 0; n < len; ++n) {
        const long w = start + n;
        const int sym = (int)(n / p->nsps);
        const double f = s.f0 + s.tones[sym] * sp + s.driftHz * ((double)n / len - 0.5);
        ph += 6.283185307179586 * f / fs;
        if (ph > 6.283185307179586) ph -= 6.283185307179586;
        if (s.spreadHz > 0 && n % 10 == 0) { sr = a * sr + g(rng); si = a * si + g(rng); fr = a * fr + sr * gn; fi = a * fi + si * gn; }
        if (w < 0 || w >= (long)win.size()) continue;
        const double v = s.spreadHz > 0 ? (fr * std::cos(ph) - fi * std::sin(ph)) : std::cos(ph);
        win[(size_t)w] += (float)(amp * v);
    }
}

/** A window of white Gaussian noise (unit variance), scaled by `scale` (the decoder is scale-free). */
inline std::vector<float> noiseWindow(int periodSec, uint32_t seed, float scale = 0.01f) {
    const vibe::q65::Period* p = vibe::q65::periodFor(periodSec);
    std::vector<float> w(p ? (size_t)vibe::q65::windowSamples(*p) : 0);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, 1.f);
    for (float& v : w) v = g(rng);
    (void)scale;
    return w;
}

} // namespace q65synth
