// tools/paging-bench/paging_bench.cpp — the paging receiver's cost: N seconds of noise at RATE through AUTO (three POCSAG
// rates). The noise is made once (a cheap LCG, 1 s, reused) so an instruction count is the receiver's.
//   paging_bench [seconds=60] [rate=12000] [iq]
//   "iq": the WIRED path (vibe_paging_run.h) — complex channel IQ through our own discriminator (q65v lanes: NEON / SSE2,
//   or VIBE_FORCE_SCALAR) and then the engine, as the Android host runs it.
#include "decoders/paging_decoder.h"
#include "vibe_paging_run.h"
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    const int secs = argc > 1 ? std::atoi(argv[1]) : 60;
    const double fs = argc > 2 ? std::atof(argv[2]) : 12000;
    const bool iq = argc > 3 && std::strcmp(argv[3], "iq") == 0;
    // ★ argv[4]: a mode id (default auto) · argv[5] "fresh": new noise every second (the false-page runs; the default
    //   repeats one second, which is all a cost count needs)
    const vibe::PagingDecoder::Mode mode = vibe::PagingDecoder::modeFromId(argc > 4 ? argv[4] : "auto");
    const bool fresh = argc > 5 && std::strcmp(argv[5], "fresh") == 0;
    uint32_t s = 12345;
    auto rnd = [&] { s = s * 1664525u + 1013904223u; return (float)((int32_t)s) * (1.0f / 2147483648.0f); };
    int pages = 0;
    if (iq) {
        std::vector<std::complex<float>> x((size_t)fs);
        for (auto& v : x) { const float a = rnd(); v = std::complex<float>(a, rnd()); }
        vibe::PagingRun::Out o; o.text = [&](const std::string&) { ++pages; };
        vibe::PagingRun r(mode, std::move(o));
        const int blk = (int)(fs * 0.02);
        bool det = false;
        for (int k = 0; k < secs; ++k) {
            if (fresh && k) for (auto& v : x) { const float a = rnd(); v = std::complex<float>(a, rnd()); }
            for (int i = 0; i < (int)x.size(); i += blk) r.feedIq(x.data() + i, std::min(blk, (int)x.size() - i), fs);
            det |= !r.detected().empty();
        }
        if (det) std::printf("DETECTED something on noise\n");
    } else {
        std::vector<float> x((size_t)fs);
        for (auto& v : x) v = rnd();
        vibe::PagingDecoder d(fs, mode);
        d.onPage = [&](const vibe::PagingDecoder::Page&) { ++pages; };
        for (int k = 0; k < secs; ++k) { if (fresh && k) for (auto& v : x) v = rnd(); d.feed(x.data(), (int)x.size()); }
    }
    std::printf("paging_bench: %d s at %.0f Hz%s, %d pages\n", secs, fs, iq ? " (IQ, wired path)" : "", pages);
    return 0;
}
