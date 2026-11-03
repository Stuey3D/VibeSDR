// vibeserver-bench — run the server benchmark (android/app/src/main/cpp/vibe_benchmark.h) from a shell and print
// the JSON. The same engine the setup page and the Lite app run; this is how it is checked on a box.
// usage: vibeserver-bench [seconds per scenario] [clip.vbu8] [adsb-clip.vbu8] [decoders-clip.vbu8]
//        — with no clip it fetches (and caches) one; "-" for a clip skips its rows (offline: they read "not measured").
// ★ V12 (2026-11-03) — the run tailored to the radios here (vibe_benchmark_hw.h):
//   VIBE_BENCH_RADIOS="usrp:3:locked,rtl,airspyhf" — pretend these radios are configured (driver[:usb speed][:locked]
//     [:ds]); unset = no radio configured = the full run, as before. The server passes its real radios itself.
//   VIBE_BENCH_WIDE_ONLY=1 — run the wide capture rows alone (vibe_benchmark_wide.h) for the radios above, and print
//     their rows and the capture verdict: how the wide rates are checked on a machine without the rest of the run.
#include "vibe_benchmark_dab.h"
#include "vibe_benchmark_adsb.h"
#include "vibe_thread_layout.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

static vibe::benchhw::Hw radiosFromEnv() {
    vibe::benchhw::Hw hw;
    const char* e = std::getenv("VIBE_BENCH_RADIOS");
    if (!e || !*e) return hw;
    std::stringstream ss(e);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::stringstream is(item);
        std::string tok, driver; int usb = 0; bool locked = false, ds = false;
        std::getline(is, driver, ':');
        while (std::getline(is, tok, ':')) {
            if (tok == "locked") locked = true; else if (tok == "ds") ds = true; else usb = std::atoi(tok.c_str());
        }
        if (!driver.empty()) hw.radios.push_back(vibe::benchhw::radioFor(driver, 0, 0, 0, ds, usb, locked));
    }
    return hw;
}

int main(int argc, char** argv) {
    // ★★ THE SERVER'S OWN THREAD LAYOUT (vibe_thread_layout.h — every multi-core server since 2026-10-16). Without
    //    it this measured a single-threaded receiver no server runs: WFM 2.048 read 103 % on the Pi 2 against 59 %
    //    as served. Called here, not left to the shim's initialiser: this binary may not link the shim's object.
    vibe::vibeApplyDefaultThreadLayout();
    const double secs = argc > 1 ? std::atof(argv[1]) : 4.0;
    const vibe::benchhw::Hw hw = radiosFromEnv();
    if (std::getenv("VIBE_BENCH_WIDE_ONLY")) {
        if (!vibedsp::RxPipeline::workerInit())
            vibedsp::RxPipeline::workerInit() = [](const char* n) { vibe::benchwide::nameThread(n); };
        std::vector<vibe::benchdetail::Result> res;
        std::vector<std::pair<std::string, std::string>> notRun;
        // ★ VIBE_BENCH_WIDE_RATES="30720000,16000000": measure only these (an A/B on one rate).
        std::vector<double> rates = vibe::benchhw::wideRates(hw);
        if (const char* wr = std::getenv("VIBE_BENCH_WIDE_RATES")) {
            rates.clear(); std::stringstream rs(wr); std::string t;
            while (std::getline(rs, t, ',')) if (!t.empty()) rates.push_back(std::atof(t.c_str()));
        }
        const auto v = vibe::benchwide::runWideRows(rates, secs, res, notRun,
                                                    [](const std::string& l) { std::fprintf(stderr, "%s\n", l.c_str()); });
        for (const auto& r : res) {
            std::printf("%-44s %6.1f %% hottest (%s)", r.label.c_str(), r.pct, r.hottest.c_str());
            for (const auto& t : r.threads) std::printf("  %s %.1f", t.first.c_str(), t.second);
            if (r.dropped >= 0) std::printf("  dropped %ld", r.dropped);
            std::printf("\n");
        }
        for (const auto& n : notRun) std::printf("%s: %s\n", n.first.c_str(), n.second.c_str());
        double l = 0, f = 0;
        vibe::benchwide::gateFrom(v, l, f);
        std::printf("gate: locked range keeps up to %.2f MS/s, full radio to %.2f MS/s\n", l / 1e6, f / 1e6);
        return 0;
    }
    // ★ A path given, or the cached download (fetched on first use) — see ensureDabClip.
    const std::string clip = argc > 2 ? std::string(argv[2])
                                      : vibe::ensureDabClip(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    const std::string adsbClip = argc > 3 ? std::string(argv[3])
                                          : vibe::ensureAdsbClip(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    const std::string decoderClip = argc > 4 ? std::string(argv[4])
                                             : vibe::ensureDecoderClip(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    // ★ VIBE_BENCH_OFFLINE=1: no uplink measurement (it posts 4 MB to the directory) — for a bench
    //   run that must not touch the network.
    const double uplink = std::getenv("VIBE_BENCH_OFFLINE") ? -1 : -2;
    const std::string j = vibe::runBenchmark([](int d, int n, const std::string& l) {
        std::fprintf(stderr, "[%d/%d] %s\n", d, n, l.c_str()); }, secs, uplink,
        [&] { auto r = vibe::runDabRows(clip, secs);
              for (auto& a : vibe::runAdsbRows(adsbClip, secs)) r.push_back(a);
              return r; }, 4, decoderClip == "-" ? std::string() : decoderClip, hw);
    std::puts(j.c_str());
    return 0;
}
