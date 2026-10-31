// bench — what each AM-family mode COSTS per listener, through the real RxPipeline.
//   bench [seconds] [rate] [only]   (only = index of one mode, -1 = the signal alone — for qemu instruction counts; default 10 s at 48 kHz: a listener's own channel on a per-listener server;
//                             pass 2400000 for a one-pipeline RTL radio)
// Synthetic AM (multitone, a 5 kHz neighbour, noise) so auto bandwidth has something to do. Prints % of one core in
// real time per mode, thread CPU only. Build with -DVIBE_FORCE_SCALAR for the scalar figure. ★ SILENT.
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
    const double secs = argc > 1 ? std::atof(argv[1]) : 10.0, fs = argc > 2 ? std::atof(argv[2]) : 48000.0;
    const int only = argc > 3 ? std::atoi(argv[3]) : -2;
    const long n = (long)(secs * fs);
    std::vector<cf32> z((size_t)n);
    std::mt19937 r(1); std::normal_distribution<float> g(0, 1);
    for (long i = 0; i < n; ++i) {
        const double t = i / fs;
        double m = 0; for (int k = 0; k < 12; ++k) m += 0.06 * std::cos(2 * M_PI * (250.0 + 330.0 * k) * t + k);
        const double off = 2 * M_PI * 3.0 * t, nb = 2 * M_PI * 5003.0 * t;
        z[i] = cf32((float)((1 + m) * std::cos(off) + 0.4 * std::cos(nb)), (float)((1 + m) * std::sin(off) + 0.4 * std::sin(nb)))
             * 0.01f + cf32(1e-4f * g(r), 1e-4f * g(r));
    }
    struct C { const char* nm; M m; bool a; double bw; };
    const C cs[] = { { "AM", M::AM, false, 10000 }, { "AM + auto BW", M::AM, true, 10000 }, { "SAM", M::SAM, false, 10000 },
                     { "SAM + auto BW", M::SAM, true, 10000 }, { "SAM-U", M::SAM_U, false, 10000 }, { "SAM-L + auto", M::SAM_L, true, 10000 },
                     { "ECSS-U", M::ECSS_U, false, 4500 }, { "ECSS-L + auto", M::ECSS_L, true, 4500 },
                     { "AM stereo", M::AM_STEREO, false, 10000 }, { "AM stereo + auto", M::AM_STEREO, true, 10000 },
                     { "USB (reference)", M::SSB_USB, false, 2700 } };
#ifdef VIBE_FORCE_SCALAR
    std::printf("SCALAR build, %.0f Hz input, %.0f s\n", fs, secs);
#else
    std::printf("SIMD build, %.0f Hz input, %.0f s\n", fs, secs);
#endif
    if (only == -1) return 0;
    int idx = -1;
    for (const C& c : cs) {
        if (++idx, only >= 0 && idx != only) continue;
        RxPipeline::Callbacks cb; cb.audio = sink;
        RxPipeline p; p.setMeasureThread(false);
        p.start(fs, 1024, 10.0, 48000, cb);
        p.setAmAutoBandwidth(c.a);
        p.setTune(0.0, c.m, c.bw);
        const int blk = (int)(fs / 50);
        const double t0 = cpuNow();
        for (long i = 0; i < n; i += blk) p.feed(z.data() + i, (int)std::min<long>(blk, n - i));
        const double dt = cpuNow() - t0;
        const auto st = p.amSyncStatus();
        std::printf("  %-18s %7.3f %%  (locked %d, edges %.0f/%.0f)\n", c.nm, 100.0 * dt / secs, st.locked, st.edgeLoHz, st.edgeHiHz);
        p.stop();
    }
}
