// bench — what SSB / CW AUTO bandwidth COSTS per listener, through the real RxPipeline (V12, docs/v12/AM-SYNC.md §11).
//   bench [seconds] [only]   only: 0 USB, 1 USB + AUTO, 2 CW, 3 CW + AUTO; -1 = the signal alone (for qemu instruction
//   counts: subtract it). A 48 kHz listener channel (a per-listener server) carrying a pseudo-voice SSB signal and a
//   keyed tone in noise, so AUTO has something to measure and cut. Prints % of one core. ★ SILENT.
#include "vibedsp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <vector>
using namespace vibedsp;
using M = RxPipeline::Mode;
static double cpuNow() { timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void sink(void*, const float*, int, int, int) {}
int main(int argc, char** argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 10.0, fs = 48000.0;
    const int only = argc > 2 ? std::atoi(argv[2]) : -2;
    const long n = (long)(secs * fs);
    std::vector<cf32> z((size_t)n);
    std::mt19937 r(1); std::normal_distribution<float> g(0, 1);
    double ph = 0, f0 = 130;
    for (long i = 0; i < n; ++i) {
        const double t = i / fs;
        const double syl = 0.5 + 0.5 * std::sin(2 * M_PI * 4.0 * t);           // syllables
        f0 = 120 + 30 * std::sin(2 * M_PI * 0.7 * t); ph += 2 * M_PI * f0 / fs;
        std::complex<double> v(0, 0);
        for (int h = 3; h <= 20; ++h) v += std::polar(syl / h, h * ph);         // ~360-2600 Hz, one-sided (USB)
        const bool key = std::fmod(t, 0.24) < 0.12;                            // CW at 6 kHz + 600: keyed tone
        v *= std::polar(1.0, 2 * M_PI * 3000.0 * t);
        v += (key ? 0.3 : 0.0) * std::polar(1.0, 2 * M_PI * 9000.0 * t);
        z[i] = cf32((float)v.real() * 0.01f + 3e-4f * g(r), (float)v.imag() * 0.01f + 3e-4f * g(r));
    }
    struct C { const char* nm; M m; bool a; double off, bw; };
    const C cs[] = { { "USB", M::SSB_USB, false, 3000, 4000 }, { "USB + AUTO", M::SSB_USB, true, 3000, 4000 },
                     { "CW", M::CW, false, 8400, 1200 }, { "CW + AUTO", M::CW, true, 8400, 1200 } };
#ifdef VIBE_FORCE_SCALAR
    std::printf("SCALAR build, %.0f s\n", secs);
#else
    std::printf("SIMD build, %.0f s\n", secs);
#endif
    if (only == -1) return 0;
    int idx = -1;
    for (const C& c : cs) {
        if (++idx, only >= 0 && idx != only) continue;
        RxPipeline::Callbacks cb; cb.audio = sink;
        RxPipeline p; p.setMeasureThread(false);
        p.start(fs, 1024, 20.0, 48000, cb);
        p.setAmAutoBandwidth(c.a);
        p.setTune(c.off, c.m, c.bw);
        const double t0 = cpuNow();
        for (long i = 0; i < n; i += 4800) p.feed(z.data() + i, (int)std::min<long>(4800, n - i));
        const double dt = cpuNow() - t0;
        const auto st = p.autoPassbandStatus();
        std::printf("%-12s %.3f %% of one core   AUTO %s %.0f-%.0f Hz\n", c.nm, 100.0 * dt / secs, st.cut ? "cut" : "wide", st.loHz, st.hiHz);
        p.stop();
    }
}
