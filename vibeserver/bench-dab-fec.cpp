// bench-dab-fec.cpp — how long the DAB Viterbi actually takes, per second of audio.
// ★ The number that matters is not ns/bit but "fraction of one core to keep one service decoded",
//   because the target includes a Pi Zero 2 W.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "vibe_dab_fec.h"

int main(int argc, char** argv) {
    const size_t nbits = size_t(argc > 1 ? atoi(argv[1]) : 3072);
    const int    iters = argc > 2 ? atoi(argv[2]) : 200;
    std::vector<int8_t> soft(nbits * 4);
    // ★ Realistic soft values: mostly confident, some marginal — a decoder fed only +-127 can
    //   take shortcuts that vanish on air.
    unsigned r = 12345;
    for (auto& v : soft) { r = r * 1103515245u + 12345u; v = int8_t(int((r >> 16) % 255) - 127); }

    vibedab::Viterbi vit;
    auto t0 = std::chrono::steady_clock::now();
    size_t sink = 0;
    for (int i = 0; i < iters; ++i) sink += vit.decode(soft.data(), nbits).size();
    auto t1 = std::chrono::steady_clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double perCall = secs / iters * 1e3;
    /* ★ A DAB logical frame is 24 ms. Whatever a service needs per frame, divided by 24 ms, is the
     *  share of one core it costs — the figure that decides whether a Pi Zero 2 W can serve it. */
    printf("viterbi: %zu bits x %d iters in %.3f s -> %.3f ms/call, %.1f ns/bit  (sink %zu)\n",
           nbits, iters, secs, perCall, secs / iters / double(nbits) * 1e9, sink);
    printf("  a 24 ms logical frame's worth of this block = %.1f%% of one core\n",
           perCall / 24.0 * 100.0);
    return 0;
}
