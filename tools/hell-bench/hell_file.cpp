// tools/hell-bench/hell_file.cpp — draw Feld Hell from a raw float32 mono file (or the test transmitter) into a PGM, every
// strip one under another, as a client would show them. For looking at, and for settling the column direction.
//   hell_file FILE.f32 RATE OUT.pgm
//   hell_file --synth "TEXT" SNR_DB_IN_3K OUT.pgm [HZ] [PPM]
//   hell_file --cost SECONDS        (a keyed signal at 0 dB, 48 kHz in — for the qemu count, tools/hell-bench/qemu.sh)
#include "decoders/feld_hell.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>
using namespace vibe;
int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--cost") {
        const int secs = std::atoi(argv[2]);
        auto one = feldHellSynth("CQ CQ DE G4XYZ FELD HELL TEST K ", 48000, 1200);   // ≈ 13 s, looped: drawing all the time
        uint32_t s = 12345; for (auto& v : one) { s = s * 1664525u + 1013904223u; v += 0.3f * (float)((int32_t)s) * (1.0f / 2147483648.0f); }
        FeldHellDecoder d(48000); long lines = 0; d.onLine = [&](uint32_t, uint32_t, const uint8_t*) { ++lines; };
        size_t at = 0;
        for (long k = 0; k < (long)secs * 50; ++k) { if (at + 960 > one.size()) at = 0; d.feed(one.data() + at, 960); at += 960; }
        std::printf("hell_file cost: %d s, %ld lines\n", secs, lines);
        return 0;
    }
    if (argc < 4) { std::fprintf(stderr, "usage\n"); return 2; }
    std::vector<float> x; double fs = 8000; std::string out;
    if (std::string(argv[1]) == "--synth") {
        fs = 48000; out = argv[4];
        x = feldHellSynth(argv[2], fs, argc > 5 ? std::atof(argv[5]) : 1000, argc > 6 ? std::atof(argv[6]) : 0);
        std::vector<float> pad((size_t)(2 * fs), 0.f); x.insert(x.begin(), pad.begin(), pad.end());
        double p = 0; size_t n = 0; for (float v : x) if (v != 0.f) { p += v * v; ++n; } p /= n;
        const float sig = (float)std::sqrt(p / std::pow(10.0, std::atof(argv[3]) / 10.0) * (fs / 2) / 3000.0);
        std::mt19937 r(7); std::normal_distribution<float> g(0.f, 1.f);
        for (auto& v : x) v += sig * g(r);
    } else {
        FILE* fp = std::fopen(argv[1], "rb"); if (!fp) return 1;
        float b[4096]; size_t n; while ((n = std::fread(b, 4, 4096, fp)) > 0) x.insert(x.end(), b, b + n);
        std::fclose(fp); fs = std::atof(argv[2]); out = argv[3];
    }
    FeldHellDecoder d(fs);
    std::map<uint32_t, std::vector<uint8_t>> rows; uint32_t w = 0;
    d.onLine = [&](uint32_t ln, uint32_t wd, const uint8_t* px) { w = wd; rows[ln].assign(px, px + wd); };
    d.onStatus = [&](const std::string& j) { std::fprintf(stderr, "status %s\n", j.c_str()); };
    const int blk = (int)(fs / 50);
    for (size_t i = 0; i < x.size(); i += blk) d.feed(x.data() + i, (int)std::min<size_t>(blk, x.size() - i));
    const uint32_t h = rows.empty() ? 0 : rows.rbegin()->first + 1;
    FILE* fp = std::fopen(out.c_str(), "wb"); if (!fp) return 1;
    std::fprintf(fp, "P5\n%u %u\n255\n", w, h);
    std::vector<uint8_t> blank(w, 255);
    for (uint32_t y = 0; y < h; ++y) { auto it = rows.find(y); std::fwrite(it == rows.end() ? blank.data() : it->second.data(), 1, w, fp); }
    std::fclose(fp);
    std::printf("%s: %u × %u\n", out.c_str(), w, h);
    return 0;
}
