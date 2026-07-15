// VibeServer capacity benchmark — how many users fit on this box?
//
// Drives the REAL vibedsp RxPipeline (waterfall FFT + DDC + demod + resample),
// which is exactly what one connected VibeServer user costs. Feeds synthetic IQ,
// so it needs NO dongle, no root, and no librtlsdr — you can run it on a box
// that's already busy doing something else (e.g. an ADS-B feeder) without
// touching it.
//
// Reports CPU time, so background load doesn't skew the result.
//
//   build:  ./build.sh
//   run:    ./bench

#include "../../android/app/src/main/cpp/vibedsp/vibedsp.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>
#include <thread>

using namespace vibedsp;

// ── which code path did we actually compile? ────────────────────────────────
// vibedsp/simd_internal.h enables NEON only on __aarch64__. A 32-bit userland
// (armv7l) silently falls back to scalar C — several times slower. This is the
// single most important line of output.
static const char* simdPath() {
#if defined(__aarch64__)
    return "NEON (aarch64) — fast path";
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    return "SCALAR — CPU has NEON but vibedsp only enables it on __aarch64__ (32-bit OS)";
#elif defined(__x86_64__)
    return "SCALAR (x86-64 host — indicative only, not comparable to a Pi)";
#else
    return "SCALAR";
#endif
}

// CPU time actually consumed by this process (not wall clock).
static double cpuSeconds() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
// Synthetic IQ: broadband noise + a strong FM-modulated carrier at an offset.
// The carrier keeps the demodulators (and the WFM stereo PLL / RDS front-end)
// doing real work rather than idling on zeros.
static void makeIq(std::vector<cf32>& buf, double fs, double offsetHz) {
    unsigned s = 12345;
    auto rnd = [&s]() {
        s = s * 1664525u + 1013904223u;
        return (float)((int)(s >> 8) & 0xFFFF) / 32768.0f - 1.0f;
    };
    double ph = 0.0, mph = 0.0;
    const double dw = 2.0 * M_PI * offsetHz / fs;
    const double mdw = 2.0 * M_PI * 1000.0 / fs;   // 1 kHz modulating tone
    const double dev = 2.0 * M_PI * 75000.0 / fs;  // 75 kHz deviation (WFM)
    for (size_t i = 0; i < buf.size(); ++i) {
        mph += mdw;
        ph += dw + dev * std::sin(mph) / (2.0 * M_PI) * 0.01;
        const float n_i = rnd() * 0.05f, n_q = rnd() * 0.05f;
        buf[i] = cf32((float)std::cos(ph) * 0.5f + n_i,
                      (float)std::sin(ph) * 0.5f + n_q);
    }
}

struct Result {
    double coreFrac;   // fraction of ONE core to sustain one user in real time
    double specRows;   // spectrum rows emitted (sanity: pipeline really ran)
    double audioSecs;  // audio produced (sanity)
};

static Result runOne(double fs, RxPipeline::Mode mode, double bwHz,
                     int fftSize, double fftRate, double seconds) {
    // Callbacks just count — we're measuring DSP cost, not I/O.
    struct Ctx { long rows = 0; long frames = 0; int rate = 48000; } ctx;

    RxPipeline::Callbacks cb;
    cb.ctx = &ctx;
    cb.spectrum = [](void* c, const float*, int) {
        ((Ctx*)c)->rows++;
    };
    cb.audio = [](void* c, const float*, int frames, int, int rate) {
        auto* x = (Ctx*)c;
        x->frames += frames;
        x->rate = rate;
    };

    RxPipeline pipe;
    pipe.start(fs, fftSize, fftRate, 48000, cb);
    pipe.setTune(200000.0, mode, bwHz);   // 200 kHz off centre

    const int block = 65536;
    std::vector<cf32> iq(block);
    makeIq(iq, fs, 200000.0);

    const long long total = (long long)(fs * seconds);
    long long done = 0;

    // Warm-up (filter build, first allocations) — not counted.
    pipe.feed(iq.data(), block);

    const double c0 = cpuSeconds();
    while (done < total) {
        pipe.feed(iq.data(), block);
        done += block;
    }
    const double cpu = cpuSeconds() - c0;

    pipe.stop();

    const double iqSecs = (double)done / fs;
    Result r;
    r.coreFrac = cpu / iqSecs;          // 1.0 == needs a whole core to keep up
    r.specRows = (double)ctx.rows;
    r.audioSecs = (double)ctx.frames / (ctx.rate > 0 ? ctx.rate : 48000);
    return r;
}

int main(int argc, char** argv) {
    double seconds = 8.0;               // seconds of IQ per test
    if (argc > 1) seconds = atof(argv[1]);

    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());

    printf("\n");
    printf("VibeServer capacity benchmark\n");
    printf("=============================\n");
    printf("  DSP path : %s\n", simdPath());
    printf("  cores    : %u\n", cores);
    printf("  workload : %.0f s of IQ per test, waterfall 1024 bins @ 20 fps\n", seconds);
    printf("\n");
    printf("  'core%%' = CPU needed to sustain ONE user in real time.\n");
    printf("  100%% = one whole core. Lower is better.\n");
    printf("\n");

    struct Case { const char* name; RxPipeline::Mode mode; double bw; };
    const Case cases[] = {
        {"WFM (stereo + RDS)", RxPipeline::Mode::WFM,     180000.0},
        {"NFM",                RxPipeline::Mode::NFM,      12500.0},
        {"AM",                 RxPipeline::Mode::AM,       10000.0},
        {"SSB (USB)",          RxPipeline::Mode::SSB_USB,   2800.0},
    };
    const double rates[] = {2400000.0, 2048000.0, 1024000.0};

    for (double fs : rates) {
        printf("── %.3f MSPS ─────────────────────────────────────────────\n", fs / 1e6);
        printf("   %-22s %8s   %s\n", "mode", "core%", "users that fit (est.)");
        for (const auto& c : cases) {
            const Result r = runOne(fs, c.mode, c.bw, 1024, getenv("WFPS") ? atof(getenv("WFPS")) : 20.0, seconds);
            const double pct = r.coreFrac * 100.0;
            // Leave one core's worth of headroom for USB capture, the WebSocket
            // server, ADPCM and the OS — do not promise the last core.
            const double budget = (double)cores - 1.0;
            const int users = (r.coreFrac > 0.0)
                ? (int)(budget / r.coreFrac) : 0;
            printf("   %-22s %7.1f%%   %s%d\n", c.name, pct,
                   users >= 1 ? "~" : "", users);

            // Sanity: if the pipeline produced no audio, the number is garbage.
            if (r.audioSecs < 0.5) {
                printf("      !! produced only %.2fs of audio — result suspect\n",
                       r.audioSecs);
            }
        }
        printf("\n");
    }

    printf("Notes\n");
    printf("  * ADPCM encode + WebSocket serving are NOT in these figures, but they\n");
    printf("    are small next to the DSP (they run at 48 kHz, the DSP at %.1f MSPS).\n", rates[0] / 1e6);
    printf("  * USB capture costs extra CPU that this test cannot measure (no dongle).\n");
    printf("    On a Pi 3 running dump1090 at 2.4 MSPS that was ~30%% of one core.\n");
    printf("  * 'users that fit' assumes one whole core is reserved for capture,\n");
    printf("    networking and the OS. Treat it as an upper bound, then halve it\n");
    printf("    before you put it in a README.\n");
    if (std::string(simdPath()).find("SCALAR") != std::string::npos) {
        printf("\n");
        printf("  ****  THIS IS THE SLOW PATH.  ****\n");
        printf("  You are NOT getting the NEON DSP. Re-run on a 64-bit (aarch64) OS\n");
        printf("  for the real numbers — expect a substantial speed-up.\n");
    }
    printf("\n");
    return 0;
}
