// ★★★ ONE NaN MUST BE A GLITCH, NOT THE END OF STEREO (2026-10-30).
//
// The Lenovo RSP1A's 6 MS/s capture of 96.1, replayed through the direct path (iq_replay), locked the pilot
// for half a second and then read "dev 0.00 kHz" for the rest of the file. Two faults, one after the other:
//   1. the CEQ (CmaEqualiser) diverged INSIDE a block — its runaway guard only ran at the block's end — and
//      wrote Inf/NaN into the channel, which the discriminator turned into a NaN in the MPX;
//   2. the pilot PLL's NaN guard re-seeded the oscillator and the metric but not df_/phase_, which had
//      already integrated the NaN, so it fired again on every sample after: lockAmp held at exactly 0.
// This pins both, on the classes themselves: the PLL re-locks after a NaN, and a CMA driven to diverge
// never writes a non-finite sample and says it put itself back.
#include "../vibedsp.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace vibedsp;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    std::printf("== one NaN is a glitch (StereoPLL, CmaEqualiser) ==\n");

    // ── the pilot PLL ──────────────────────────────────────────────────────────────────────────
    {
        const double fs = 250000.0;                 // the WFM channel rate every capture rate builds
        StereoPLL pll; pll.configure(19000.0, fs);
        const int n = (int)(fs * 0.25);
        std::vector<float> mpx((size_t)n), lmr((size_t)n);
        long t = 0;
        auto fill = [&] {                           // a 6.75 kHz pilot (0.09 of ±75 kHz) under a 1 kHz tone
            for (int i = 0; i < n; ++i, ++t) {
                const double s = t / fs;
                mpx[(size_t)i] = (float)(0.09 * std::sin(2 * M_PI * 19000.0 * s) + 0.4 * std::sin(2 * M_PI * 1000.0 * s));
            }
        };
        for (int k = 0; k < 4; ++k) { fill(); pll.processBlock(mpx.data(), n, lmr.data(), nullptr, nullptr, nullptr); }
        check(pll.locked(), "a clean pilot locks within a second");
        fill(); mpx[100] = NAN;                     // ★ ONE non-finite sample
        pll.processBlock(mpx.data(), n, lmr.data(), nullptr, nullptr, nullptr);
        for (int k = 0; k < 4; ++k) { fill(); pll.processBlock(mpx.data(), n, lmr.data(), nullptr, nullptr, nullptr); }
        char b[160];
        std::snprintf(b, sizeof b, "★ …and is locked again a second after one NaN (pilot %.2f kHz; was 0.00 for ever)",
                      pll.pilotDeviationKHz());
        check(pll.locked() && pll.pilotDeviationKHz() > 6.0f && std::isfinite(pll.lockAmp()), b);
        bool fin = true; for (int i = 0; i < n; ++i) if (!std::isfinite(lmr[(size_t)i])) fin = false;
        check(fin, "…and its L-R output is finite again");
    }

    // ── the CEQ ────────────────────────────────────────────────────────────────────────────────
    {
        // ★ A station at -51 dBFS (the capture's level) with noise, and a step size 5000× the pipeline's: the
        //   gradient descent runs away inside the first block, which is exactly the shape of the field fault.
        std::mt19937 rng(7); std::normal_distribution<float> g(0.0f, 1.0f);
        const int n = 2730;                         // the block size the replay diverged in
        std::vector<cf32> z((size_t)n);
        double ph = 0.0;
        CmaEqualiser eq; eq.configure(9);
        bool fin = true;
        for (int blk = 0; blk < 40; ++blk) {
            for (int i = 0; i < n; ++i) {
                ph += 0.3 * std::sin(0.01 * (blk * n + i));
                const float a = 0.0027f;
                z[(size_t)i] = cf32(a * (float)std::cos(ph) + 0.001f * g(rng), a * (float)std::sin(ph) + 0.001f * g(rng));
            }
            eq.process(z.data(), n, 1.0f);
            for (int i = 0; i < n; ++i) if (!std::isfinite(z[(size_t)i].real()) || !std::isfinite(z[(size_t)i].imag())) fin = false;
        }
        char b[160];
        std::snprintf(b, sizeof b, "★ a CMA driven to diverge never writes Inf/NaN (it put itself back %ld times)", eq.runaways());
        check(fin && eq.runaways() > 0, b);
        check(eq.effort() < 4.0f, "…and its taps stay bounded");
    }

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
