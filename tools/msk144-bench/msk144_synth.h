// tools/msk144-bench/msk144_synth.h — MSK144 test signals from the frame description (decoders/digimodes/msk144_code.h):
// continuous-phase FSK, 2000 bit/s, tone 1 = centre + 500 Hz, tone 0 = centre − 500 Hz (the form jt9 decodes), frames
// back to back from a chosen bit of the frame, in white Gaussian noise of unit variance; SNR in 2500 Hz.
#pragma once
#include "decoders/digimodes/msk144_code.h"

#include <cmath>
#include <random>
#include <vector>

namespace msksynth {

/** Adds `nbits` bits of the frame (starting at frame bit `firstBit`) at `startSec`. Returns the sample of the first
 *  WHOLE frame's start (where bit 0 begins), or −1. `fadeSec` > 0: a meteor-like envelope (fast rise, exponential decay
 *  over the ping). */
inline long addPing(std::vector<float>& a, const uint8_t tones[144], double centre, double startSec, int nbits, int firstBit,
                    double snrDb, double phase0 = 0, double fadeSec = 0) {
    const double fs = 12000;
    const double amp = std::sqrt(2.0 * std::pow(10.0, snrDb / 10.0) * 2500.0 / (fs / 2));
    const long s0 = std::lround(startSec * fs);
    double ph = phase0;
    long firstFrame = -1;
    for (int b = 0; b < nbits; ++b) {
        const int fb = (firstBit + b) % 144;
        if (fb == 0 && firstFrame < 0) firstFrame = s0 + 6L * b;
        const double f = centre + (tones[fb] ? 500.0 : -500.0);
        for (int j = 0; j < 6; ++j) {
            ph += 2 * M_PI * f / fs;
            const long n = s0 + 6L * b + j;
            double env = 1;
            if (fadeSec > 0) { const double t = (6.0 * b + j) / fs; env = std::exp(-t / fadeSec); }
            if (n >= 0 && n < (long)a.size()) a[(size_t)n] += (float)(amp * env * std::sin(ph));
        }
    }
    return firstFrame;
}

inline std::vector<float> noise(double sec, uint32_t seed) {
    std::vector<float> w((size_t)(sec * 12000));
    std::mt19937 rng(seed); std::normal_distribution<float> g(0.f, 1.f);
    for (float& v : w) v = g(rng);
    return w;
}

} // namespace msksynth
