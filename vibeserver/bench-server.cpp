// vibeserver-bench — run the server benchmark (android/app/src/main/cpp/vibe_benchmark.h) from a shell and print
// the JSON. The same engine the setup page and the Lite app run; this is how it is checked on a box.
// usage: vibeserver-bench [seconds per scenario] [clip.vbu8]   — with no clip it fetches (and caches) one.
#include "vibe_benchmark_dab.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
#if defined(__arm__) && !defined(__aarch64__)
    // ★★ THE SERVER'S OWN THREAD LAYOUT (main.cpp, and the Lite APK's MainApplication). Without it this measured a
    //    single-threaded receiver no 32-bit box runs: WFM 2.048 read 103 % on the Pi 2 against 59 % as served.
    setenv("VIBE_DSP_THREADS", "1", 0);
    setenv("VIBE_DAB_SPLIT",   "1", 0);
#endif
    const double secs = argc > 1 ? std::atof(argv[1]) : 4.0;
    // ★ A path given, or the cached download (fetched on first use) — see ensureDabClip.
    const std::string clip = argc > 2 ? std::string(argv[2])
                                      : vibe::ensureDabClip(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    // ★ VIBE_BENCH_OFFLINE=1: no uplink measurement (it posts 4 MB to the directory) — for a bench
    //   run that must not touch the network.
    const double uplink = std::getenv("VIBE_BENCH_OFFLINE") ? -1 : -2;
    const std::string j = vibe::runBenchmark([](int d, int n, const std::string& l) {
        std::fprintf(stderr, "[%d/%d] %s\n", d, n, l.c_str()); }, secs, uplink,
        [&] { return vibe::runDabRows(clip, secs); });
    std::puts(j.c_str());
    return 0;
}
