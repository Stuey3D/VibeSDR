// ism-offline.cpp — run the V12 ISM decoder (vibe_ism*.h) over a recorded capture and print what it heard.
//   ism-offline <capture> <rate Hz> <centre Hz> [u8|cf32] [--bursts]
// u8 = rtl_tcp bytes (what VibeServer's raw IQ out sends), cf32 = interleaved float32. Prints one readable line per
// decode (the same text the clients get), then a summary. --bursts also lists every burst with its slicing, for
// working on an unknown device — ★ never with its bits: a key fob's code is not printed here either.
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o ism-offline vibeserver/ism-offline.cpp -lpthread
#include "vibe_ism_service.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vibeism;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <capture> <rate> <centre Hz> [u8|cf32] [--bursts]\n", argv[0]); return 2; }
    const double rate = std::atof(argv[2]), centre = std::atof(argv[3]);
    const bool f32 = argc > 4 && std::strcmp(argv[4], "cf32") == 0;
    bool listBursts = false;
    for (int i = 4; i < argc; ++i) if (std::strcmp(argv[i], "--bursts") == 0) listBursts = true;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    FrontEnd fe(rate);
    fe.setDcGuardHz(15e3);
    Tracker tr;
    double t = 0;
    uint64_t nb = 0, nd = 0;
    std::vector<uint8_t> u(1 << 20);
    std::vector<float> fb(1 << 20);
    std::vector<cf> x;
    const auto t0 = std::chrono::steady_clock::now();
    auto onBurst = [&](const Burst& b) {
        ++nb;
        std::vector<Reading> got;
        Service::decodeBurst(b, centre, got);
        if (listBursts)
            std::printf("  burst %8.3f s  %9.4f MHz  level %5.1f dBFS  snr %4.1f dB  %s%zu runs (shortest %.0f µs)%s\n", b.tSec,
                        (centre + b.offHz + b.carrierHz) / 1e6, b.levelDb, b.levelDb - b.noiseDb,
                        b.fsk.empty() ? "OOK " : "FSK ", b.fsk.empty() ? b.ook.size() : b.fsk.size(),
                        FrontEnd::shortestRun(b.fsk.empty() ? b.ook : b.fsk), got.empty() ? "" : "  → decoded");
        for (auto& r : got) {
            ++nd;
            const int before = int(tr.size());
            tr.accept(r, b.tSec);
            (void)before;
            std::printf("%8.3f s  %9.4f MHz  %s\n", b.tSec, r.freqHz / 1e6, readable(r, 0).c_str());
        }
    };
    for (;;) {
        size_t n;
        if (f32) { n = std::fread(fb.data(), sizeof(float), fb.size(), f) / 2; x.resize(n); for (size_t i = 0; i < n; ++i) x[i] = cf(fb[2 * i], fb[2 * i + 1]); }
        else { n = std::fread(u.data(), 1, u.size(), f) / 2; x.resize(n); for (size_t i = 0; i < n; ++i) x[i] = cf((u[2 * i] - 127.5f) / 128.0f, (u[2 * i + 1] - 127.5f) / 128.0f); }
        if (!n) break;
        fe.feed(x.data(), n, onBurst);
        t += n / rate;
    }
    fe.flush(onBurst);
    std::fclose(f);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%.1f s of capture: %llu bursts, %llu decodes, %zu devices; noise floor %.1f dBFS/bin; %.2f s CPU (%.1f %% of real time)\n",
                t, (unsigned long long)nb, (unsigned long long)nd, tr.size(), fe.noiseDb(), sec, 100.0 * sec / std::max(t, 1e-9));
    std::printf("\nthe device list (as the clients get it):\n%s\n", tr.json(t).c_str());
    return 0;
}
