// dxscan-fm-replay — a recorded capture (8-bit IQ .cu8) through the DX scanner's FM hop engine, exactly as a sweep runs it:
// which peaks it finds and how many clean RDS groups each gives (dev tool; real captures stay in ~/VibeSDR-test-audio/fm).
//   dxscan-fm-replay <file.cu8> <rateHz> <centreHz> <loHz> <hiHz> [dwellSec] [peaksPerRound] [workers]
#include "vibe_dxscan_fm.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: %s file.cu8 rate centre lo hi [dwell] [peaksPerRound] [workers]\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "rb"); if (!f) { std::perror(argv[1]); return 2; }
    std::vector<unsigned char> raw; unsigned char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) raw.insert(raw.end(), buf, buf + n);
    std::fclose(f);
    vibe::dx::FmHopEngine eng; vibe::dx::FmHopEngine::Params p;
    p.rateHz = std::atof(argv[2]); p.centreHz = p.dcHz = std::atof(argv[3]); p.loHz = std::atof(argv[4]); p.hiHz = std::atof(argv[5]);
    if (argc > 6) p.dwellSec = std::atof(argv[6]);
    if (argc > 7) p.peaksPerRound = std::atoi(argv[7]);
    if (argc > 8) p.workers = std::atoi(argv[8]);
    p.settleSec = 0;
    eng.start(p);
    std::vector<vibe::dx::cf32> blk(16384);
    for (size_t i = 0; i + 2 <= raw.size() && !eng.done();) {
        int k = 0;
        for (; k < (int)blk.size() && i + 2 <= raw.size(); ++k, i += 2) blk[(size_t)k] = { (raw[i] - 127.4f) / 128.f, (raw[i + 1] - 127.4f) / 128.f };
        eng.feed(blk.data(), k);
        // ★ the queue holds 0.5 s by design (a small board's memory): feed no faster than the engine takes it
        while (eng.queuedSec() > 0.2 && !eng.done()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // ★ the engine decodes on its own threads: wait for it (feeding silence only if the capture ran out mid-dwell)
    for (int w = 0; w < 600 && !eng.done(); ++w) {
        if (w % 10 == 0) { std::vector<vibe::dx::cf32> z(16384); eng.feed(z.data(), (int)z.size()); }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!eng.done()) std::printf("(the engine did not finish)\n");
    const auto st = eng.stats();
    std::printf("peaks %d, rounds %d, workers %d, extended %d, floor %.1f dBFS, dropped %ld\n", st.peaks, st.rounds, st.workers, st.extended, st.floorDb, st.droppedBlocks);
    for (const auto& h : eng.results())
        std::printf("  %7.1f MHz  PI %04X  groups %3d  PS '%s'%s  level %.1f dBFS  SNR %.1f dB  pilot %s  %s\n", h.khz / 1000.0, h.pi, h.groups, h.ps.c_str(),
                    h.psComplete ? " (complete)" : "", h.levelDb, h.snrDb, h.pilot ? "LOCKED" : "no", vibe::dx::fmConfirmed(h.groups, h.psComplete) ? "IDENTIFIED" : "—");
}
