// tools/ale-bench/ale_file.cpp — run the ALE receiver (decoders/ale_decoder.h) on a raw float32 mono file and print its
// lines (and, with -w, every word). Silent; for real recordings (12 kHz UberSDR WAVs converted with ffmpeg) and tests.
//   ale_file [-w] FILE.f32 RATE
#include "decoders/ale_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace vibe;
int main(int argc, char** argv) {
    int a = 1; bool words = false;
    if (a < argc && !std::strcmp(argv[a], "-w")) { words = true; ++a; }
    if (argc - a < 2) { std::fprintf(stderr, "usage: ale_file [-w] FILE.f32 RATE\n"); return 2; }
    FILE* fp = std::fopen(argv[a], "rb"); if (!fp) return 1;
    std::vector<float> x; float b[4096]; size_t n; while ((n = std::fread(b, 4, 4096, fp)) > 0) x.insert(x.end(), b, b + n);
    std::fclose(fp);
    const double fs = std::atof(argv[a + 1]);
    AleDecoder d(fs); size_t at = 0;
    d.clock = [&]() { char t[16]; const int s = (int)(at / fs); std::snprintf(t, sizeof t, "+%02d:%02d", s / 60, s % 60); return std::string(t); };
    d.onLine = [&](const std::string& pub, const std::string& priv) { std::printf("%s", priv.empty() ? pub.c_str() : priv.c_str()); };
    if (words) d.onWord = [&](const AleDecoder::Word& w) { std::printf("  %7.2f s  %-4s %c%c%c  err %d  bad %d  off %+.0f\n", at / fs, AleDecoder::preambleName(w.pre), w.c[0], w.c[1], w.c[2], w.errors, w.votesBad, w.offsetHz); };
    const int blk = (int)(fs / 50);
    for (at = 0; at < x.size(); at += blk) d.feed(x.data() + at, (int)std::min<size_t>(blk, x.size() - at));
    std::vector<float> z((size_t)(fs * 3), 0.f); d.feed(z.data(), (int)z.size());   // flush the last transmission
    return 0;
}
