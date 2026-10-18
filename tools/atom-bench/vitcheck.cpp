// vitcheck: decode random-but-structured soft input (a real encoded stream plus noise), hash the bits,
// count errors vs the source, and time it. Run with and without VIBE_NO_SSSE3=1: hashes must match.
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
#include "vibe_dab_fec.h"
int main() {
    std::mt19937 rng(3); std::normal_distribution<float> n(0.f, 55.f);
    const size_t nbits = 3072; vibedab::Viterbi vit;
    uint64_t h = 1469598103934665603ull; size_t errs = 0; double secs = 0;
    for (int it = 0; it < 300; ++it) {
        std::vector<uint8_t> src(nbits); for (auto& b : src) b = rng() & 1;
        std::vector<uint8_t> packed((nbits + 7) / 8, 0);
        for (size_t i = 0; i < nbits; ++i) packed[i / 8] |= uint8_t(src[i] << (i % 8));
        auto enc = vibedab::convEncode(src.data(), nbits);
        std::vector<int8_t> soft(enc.size());
        for (size_t i = 0; i < enc.size(); ++i) { float v = (enc[i] ? -64.f : 64.f) + n(rng); soft[i] = int8_t(v > 127 ? 127 : v < -127 ? -127 : v); }
        auto t0 = std::chrono::steady_clock::now();
        auto out = vit.decode(soft.data(), nbits);
        secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (size_t i = 0; i < nbits; ++i) { h = (h ^ out[i]) * 1099511628211ull; errs += out[i] != src[i]; }
    }
    printf("hash %016llx  bit errors %zu / %zu  %.1f ns/bit\n", (unsigned long long)h, errs, nbits * 300, secs / (nbits * 300.0) * 1e9);
}
