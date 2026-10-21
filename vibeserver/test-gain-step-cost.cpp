// test-gain-step-cost — WHAT ONE OF OUR OWN GAIN WRITES COSTS THE CAPTURE THREAD (2026-10-21).
//
// ★★★ THE PORTABLE'S STUTTER ON EVERY VibeAGC MOVE. 11.0.9's GainStepSmoother opens a 0.3 s search after
//     every gain write with no flag from the radio (every RTL write), and its finder summed both 1.5 ms
//     windows afresh for every 64-sample edge: ~100x the stream in power sums for those 0.3 s. On the
//     Pi 3A+ (Cortex-A53) that is more than a core, on the DSP's capture thread — "IQ overrun — dropping a
//     buffer (the DSP thread was blocked)" right after each "gain back up to …", heard as a stutter on every
//     gain change; on the Pi 500 the audio's buffer absorbed it but the waterfall hitched.
//  ★ Now each sample's power is summed once (a prefix over 64-sample pieces). This test fails if the
//    search window costs more than a small multiple of the idle smoother again, and checks a real step
//    after a write is still found and sized the same.
//  ★ A RATIO measured in the same run, not a time: the machine running it does not matter, and the old
//    code sits at ~100-150x where the new one is ~3-8x. Silent; no audio.
#include "../android/app/src/main/cpp/vibedsp/gainstep.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using cf32 = std::complex<float>;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("   %s   %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

// Median over a few repeats of (cost of the 0.3 s after a write) / (cost of the same signal idle).
static double ratioFor(int n) {
    const double rate = 2.048e6;
    std::vector<double> rs;
    for (int rep = 0; rep < 5; ++rep) {
        vibedsp::GainStepSmoother g; g.configure(rate);
        std::mt19937 r(1 + rep); std::normal_distribution<float> d(0.0f, 0.1f);
        std::vector<cf32> buf((size_t)n);
        auto run = [&](int blocks, bool hint) {
            double t = 0;
            for (int b = 0; b < blocks; ++b) {
                for (auto& z : buf) z = cf32(d(r), d(r));
                if (hint && b == 0) g.noteStepSoon(0.3);
                const auto a = std::chrono::steady_clock::now();
                g.process(buf.data(), n);
                t += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            }
            return t;
        };
        const int blocks = (int)(0.35 * rate / n) + 1;
        run(blocks, false);
        const double base = run(blocks, false), hint = run(blocks, true);
        rs.push_back(hint / std::max(base, 1e-3));
    }
    std::sort(rs.begin(), rs.end());
    return rs[rs.size() / 2];
}

int main() {
    std::printf("── the cost of a gain write's step search (noise only: the whole 0.3 s is searched) ──\n");
    const double r16k = ratioFor(16384), r1k = ratioFor(1024);
    std::printf("   .. 16384-sample blocks (RTL): x%.1f the idle smoother\n", r16k);
    std::printf("   .. 1024-sample blocks (RSP-sized): x%.1f the idle smoother\n", r1k);
    check(r16k < 12.0, "★★★ an RTL gain write's search costs < 12x idle (11.0.9 shipped at ~100x)");
    check(r1k < 25.0, "★★ and on small blocks < 25x idle");

    std::printf("── a real +8 dB step after a write is still found and sized ──\n");
    {
        const double rate = 2.048e6; const int n = 16384;
        vibedsp::GainStepSmoother g; g.configure(rate);
        std::mt19937 r(7); std::normal_distribution<float> d(0.0f, 0.1f);
        std::vector<cf32> buf((size_t)n);
        long long at = 0; const long long stepAt = (long long)(0.12 * rate);
        const float k = std::pow(10.0f, 8.0f / 20.0f);
        for (int b = 0; b < 60; ++b) {
            for (int i = 0; i < n; ++i) { buf[(size_t)i] = cf32(d(r), d(r)); if (at + i >= stepAt) buf[(size_t)i] *= k; }
            if (b == 2) g.noteStepSoon(0.3);
            g.process(buf.data(), n);
            at += n;
        }
        std::printf("   .. steps %u, last %+.2f dB at %lld (step at %lld)\n", g.steps(), g.lastStepDb(),
                    g.lastStepAt(), stepAt);
        check(g.steps() == 1, "one step found");
        check(std::fabs(g.lastStepDb() - 8.0f) < 0.5f, "sized within 0.5 dB of +8");
        check(std::llabs(g.lastStepAt() - stepAt) <= 64, "placed within one piece of the step");
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
