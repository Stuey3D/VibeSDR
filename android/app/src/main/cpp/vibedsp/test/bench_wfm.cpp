// VibeSDR — WFM stereo CPU bench (host only, not part of the app build).
//
// The WFM stereo MPX chain is the most expensive thing VibeDSP does, and it runs
// on the phone for local hardware and on VibeServer for every client. This times
// the whole IQ->stereo-audio path so a change can be judged in % of real time
// instead of by feel.
//
//   cmake --build build && ./build/vibedsp_bench_wfm
#include "vibedsp.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <vector>
#include <string>
#include <cstdlib>

using namespace vibedsp;

static void onAud(void*, const float*, int, int, int) {}
static void onSpec(void*, const float*, int) {}

int main(int argc, char** argv) {
    const double fs = (argc > 1) ? std::atof(argv[1]) : 1920000.0;
    // Optional 2nd arg: "nfm" to time the narrowband path instead (same DDC +
    // discriminator, no MPX) — that is what local hardware uses most of the time.
    const bool nfm = (argc > 2 && std::string(argv[2]) == "nfm");
    const double fc = 300000.0;
    const int Ni = 1 << 21;                       // ~1.09 s of IQ at 1.92 MSPS
    const double secs = Ni / fs;

    std::vector<cf32> iq(Ni);
    double ph = 0.0;
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        const double L = 0.3 * std::cos(2.0 * M_PI * 1000.0 * t);
        const double R = 0.3 * std::cos(2.0 * M_PI * 4000.0 * t);
        const double mpx = (L + R)
                         + 0.1 * std::sin(2.0 * M_PI * 19000.0 * t)
                         + (L - R) * std::sin(2.0 * M_PI * 38000.0 * t);
        ph += 2.0 * M_PI * (fc + 50000.0 * mpx) / fs;
        iq[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }

    RxPipeline pipe;
    RxPipeline::Callbacks cb;
    cb.audio = onAud; cb.spectrum = onSpec;
    pipe.start(fs, 1024, 20.0, 48000, cb);
    pipe.setTune(fc, nfm ? RxPipeline::Mode::NFM : RxPipeline::Mode::WFM,
                 nfm ? 12500.0 : 200000.0);

    // Warm up (first feed() rebuilds the audio chain and faults the buffers in).
    for (int o = 0; o < Ni; o += 65536) pipe.feed(iq.data() + o, std::min(65536, Ni - o));

    const int reps = 5;
    double best = 1e9;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int o = 0; o < Ni; o += 65536) pipe.feed(iq.data() + o, std::min(65536, Ni - o));
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el < best) best = el;
    }
    std::printf("%s @", nfm ? "NFM        " : "WFM stereo ");
    std::printf(" %.3f MSPS: %.1f ms of CPU per %.2f s of audio = %.2f%% of a core\n",
                fs / 1e6, best * 1e3, secs, 100.0 * best / secs);
    return 0;
}
