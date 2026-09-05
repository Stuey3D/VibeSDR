// test-dab-resample.cpp — the resampler must pass a DAB-shaped signal through untouched.
//
// ★★★ THIS TEST EXISTS BECAUSE THE FIRST RESAMPLER WENT ON AIR BROKEN. It lost 36 dB (the
//     polyphase branch already has unit DC gain and was divided by L a second time) and wrecked
//     the phase response off-centre, taking the live FIB pass rate from 1.0 to 0.624 — while
//     every mechanical measure looked perfect: exact 64/75 ratio, exact sample counts, zero
//     drops. Mechanism right, signal ruined. Two seconds of this test would have caught it.
//
// A DAB ensemble occupies +/-768 kHz, so every tone in that range must survive within 1 dB and
// above 40 dB SNDR. The real filter manages 0.25 dB and 75 dB.
#include "../android/app/src/main/cpp/vibe_dab_resample.h"
#include <cstdio>
#include <cmath>
#include <vector>
using namespace vibedab;

/* Feed a pure complex tone at 2.4 MS/s, resample to 2.048, and measure how much of the output
 * is the SAME tone and how much is everything else. A DAB ensemble occupies +/-768 kHz, so every
 * tone in that range must come through essentially untouched. */
static void tone(double fHz, double& gainDb, double& sndrDb) {
    const double fsIn = 2400000.0, fsOut = 2048000.0;
    const size_t n = 240000;                       // 100 ms
    std::vector<float> in(2 * n);
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * M_PI * fHz * double(i) / fsIn;
        in[2 * i] = float(std::cos(ph));
        in[2 * i + 1] = float(std::sin(ph));
    }
    Resample24to2048 rs;
    std::vector<float> out;
    rs.process(in.data(), n, out);
    const size_t m = out.size() / 2;
    // skip the filter's start-up transient
    const size_t skip = 2000;
    double sr = 0, si = 0, tot = 0;
    for (size_t i = skip; i < m; ++i) {
        const double ph = 2.0 * M_PI * fHz * double(i) / fsOut;
        const double c = std::cos(ph), s = std::sin(ph);
        sr += out[2 * i] * c + out[2 * i + 1] * s;      // correlate with the ideal tone
        si += out[2 * i + 1] * c - out[2 * i] * s;
        tot += double(out[2 * i]) * out[2 * i] + double(out[2 * i + 1]) * out[2 * i + 1];
    }
    const double nn = double(m - skip);
    const double amp = std::sqrt(sr * sr + si * si) / nn;
    const double sig = amp * amp;
    const double noise = tot / nn - sig;
    gainDb = 20.0 * std::log10(amp > 1e-12 ? amp : 1e-12);
    sndrDb = 10.0 * std::log10(sig / (noise > 1e-18 ? noise : 1e-18));
}

int main() {
    printf("  tone(kHz)   gain(dB)   SNDR(dB)\n");
    bool ok = true;
    for (double f : { -768e3, -600e3, -300e3, 0.0, 300e3, 600e3, 768e3 }) {
        double g, s; tone(f, g, s);
        printf("  %8.0f   %7.2f   %8.1f%s\n", f / 1e3, g, s,
               (std::fabs(g) > 1.0 || s < 40.0) ? "   <-- BAD" : "");
        if (std::fabs(g) > 1.0 || s < 40.0) ok = false;
    }
    printf("%s\n", ok ? "PASS" : "FAIL: a DAB-shaped signal does not survive the resampler");
    return ok ? 0 : 1;
}
