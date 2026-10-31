// tools/paging-bench/paging_bench.cpp — the paging engine's cost: N seconds of noise at RATE through AUTO (three POCSAG
// rates). The noise is made once (a cheap LCG, 1 s, reused) so an instruction count is the receiver's.
//   paging_bench [seconds=60] [rate=12000]
#include "decoders/paging_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc, char** argv) {
    const int secs = argc > 1 ? std::atoi(argv[1]) : 60;
    const double fs = argc > 2 ? std::atof(argv[2]) : 12000;
    std::vector<float> x((size_t)fs);
    uint32_t s = 12345;
    for (auto& v : x) { s = s * 1664525u + 1013904223u; v = (float)((int32_t)s) * (1.0f / 2147483648.0f); }
    vibe::PagingDecoder d(fs);
    int pages = 0; d.onPage = [&](const vibe::PagingDecoder::Page&) { ++pages; };
    for (int k = 0; k < secs; ++k) d.feed(x.data(), (int)x.size());
    std::printf("paging_bench: %d s at %.0f Hz, %d pages\n", secs, fs, pages);
    return 0;
}
