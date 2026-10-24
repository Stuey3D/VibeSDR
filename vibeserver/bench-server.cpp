// vibeserver-bench — run the server benchmark (android/app/src/main/cpp/vibe_benchmark.h) from a shell and print
// the JSON. The same engine the setup page and the Lite app run; this is how it is checked on a box.
// usage: vibeserver-bench [seconds per scenario] [clip.vbu8] [adsb-clip.vbu8]   — with no clip it fetches (and caches) one.
#include "vibe_benchmark_dab.h"
#include "vibe_benchmark_adsb.h"
#include "vibe_thread_layout.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    // ★★ THE SERVER'S OWN THREAD LAYOUT (vibe_thread_layout.h — every multi-core server since 2026-10-16). Without
    //    it this measured a single-threaded receiver no server runs: WFM 2.048 read 103 % on the Pi 2 against 59 %
    //    as served. Called here, not left to the shim's initialiser: this binary may not link the shim's object.
    vibe::vibeApplyDefaultThreadLayout();
    const double secs = argc > 1 ? std::atof(argv[1]) : 4.0;
    // ★ A path given, or the cached download (fetched on first use) — see ensureDabClip.
    const std::string clip = argc > 2 ? std::string(argv[2])
                                      : vibe::ensureDabClip(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    const std::string adsbClip = argc > 3 ? std::string(argv[3])
                                          : vibe::ensureAdsbClip(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    // ★ VIBE_BENCH_OFFLINE=1: no uplink measurement (it posts 4 MB to the directory) — for a bench
    //   run that must not touch the network.
    const double uplink = std::getenv("VIBE_BENCH_OFFLINE") ? -1 : -2;
    const std::string j = vibe::runBenchmark([](int d, int n, const std::string& l) {
        std::fprintf(stderr, "[%d/%d] %s\n", d, n, l.c_str()); }, secs, uplink,
        [&] { auto r = vibe::runDabRows(clip, secs);
              for (auto& a : vibe::runAdsbRows(adsbClip, secs)) r.push_back(a);
              return r; }, 4);
    std::puts(j.c_str());
    return 0;
}
