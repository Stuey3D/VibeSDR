// tools/textmode-bench/tm_file.cpp — decode a raw float32 mono file (or a synthetic signal) with the text-mode decoder.
//   tm_file mfsk|olivia|thor MODE FILE.f32 RATE        (MODE = auto or an id)
//   tm_file mfsk|olivia|thor MODE --synth SUBMODE "TEXT" SNR_DB_IN_3K [HZ]
#include "decoders/textmodes.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace vibe;
int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage\n"); return 2; }
    TextModeDecoder::Family f; if (!TextModeDecoder::familyOf(argv[1], f)) return 2;
    std::vector<float> x; double fs = 8000;
    if (std::string(argv[3]) == "--synth") {
        fs = 48000;
        x = textModeSynth(f, argv[4], argv[5], fs, argc > 7 ? std::atof(argv[7]) : 1500);
        std::vector<float> pre((size_t)(fs * 3), 0.f); x.insert(x.begin(), pre.begin(), pre.end()); x.resize(x.size() + (size_t)(fs * 3), 0.f);
        double p = 0; for (float v : x) p += v * v; p /= x.size();
        const double snr = std::atof(argv[6]);
        // signal power over the transmission (≈ half the RMS of the burst incl. silence is fine for a dev tool)
        const float sig = (float)std::sqrt(p / std::pow(10.0, snr / 10.0) * (fs / 2) / 3000.0);
        std::mt19937 r(std::getenv("VIBE_SEED") ? std::atoi(std::getenv("VIBE_SEED")) : 7); std::normal_distribution<float> g(0.f, 1.f);
        for (auto& v : x) v += sig * g(r);
    } else {
        FILE* fp = std::fopen(argv[3], "rb"); if (!fp) return 1;
        float b[4096]; size_t n; while ((n = std::fread(b, 4, 4096, fp)) > 0) x.insert(x.end(), b, b + n);
        std::fclose(fp); fs = std::atof(argv[4]);
    }
    TextModeDecoder d(f, fs, argv[2]);
    std::string all;
    d.onText = [&](const std::string& s) { all += s; };
    d.onStatus = [&](const std::string& j) { std::fprintf(stderr, "status %s\n", j.c_str()); };
    const int blk = (int)(fs / 50);
    for (size_t i = 0; i < x.size(); i += blk) d.feed(x.data() + i, (int)std::min<size_t>(blk, x.size() - i));
    std::printf("%s\n", all.c_str());
    return 0;
}
