// tools/alert-bench/alert_bench.cpp — EAS / SAME and SELCAL cost: N seconds of noise at 48 kHz (a cheap LCG, 1 s, reused)
// through the decoder, so a qemu instruction count is the receiver's.   alert_bench same|selcal [seconds=60]
#include "decoders/same_decoder.h"
#include "decoders/selcal_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    const bool selcal = argc > 1 && std::strcmp(argv[1], "selcal") == 0;
    const int secs = argc > 2 ? std::atoi(argv[2]) : 60;
    std::vector<float> x(48000);
    uint32_t s = 12345;
    for (auto& v : x) { s = s * 1664525u + 1013904223u; v = 0.3f * (float)((int32_t)s) * (1.0f / 2147483648.0f); }
    int n = 0;
    if (selcal) { vibe::SelcalDecoder d(48000); d.onCode = [&](const vibe::SelcalDecoder::Code&) { ++n; };
                  for (int k = 0; k < secs; ++k) for (int i = 0; i < 48000; i += 960) d.feed(x.data() + i, 960); }
    else        { vibe::SameDecoder d(48000); d.onAlert = [&](const vibe::SameDecoder::Alert&) { ++n; };
                  for (int k = 0; k < secs; ++k) for (int i = 0; i < 48000; i += 960) d.feed(x.data() + i, 960); }
    std::printf("alert_bench %s: %d s, %d reports\n", selcal ? "selcal" : "same", secs, n);
    return 0;
}
