// gen-iq: write an RTL-style u8 IQ file holding the bench's decodable WFM+RDS station at +200 kHz, plus noise
// and two AM carriers, so the full server can be driven by a paced rtl_tcp replay. usage: gen-iq RATE SECS OUT
#include "vibe_benchmark.h"
#include <random>
int main(int argc, char** argv) {
    const double fs = atof(argv[1]), secs = atof(argv[2]);
    auto iq = vibe::benchdetail::fmStation(fs, secs, 200000.0);
    std::mt19937 rng(1); std::normal_distribution<float> n(0.f, 0.03f);
    std::vector<uint8_t> out(iq.size() * 2);
    double p1 = 0, p2 = 0;
    for (size_t i = 0; i < iq.size(); ++i) {
        p1 += 2 * M_PI * -350000.0 / fs; p2 += 2 * M_PI * 520000.0 / fs;
        float re = iq[i].real() * 0.6f + 0.05f * (float)std::cos(p1) + 0.02f * (float)std::cos(p2) + n(rng);
        float im = iq[i].imag() * 0.6f + 0.05f * (float)std::sin(p1) + 0.02f * (float)std::sin(p2) + n(rng);
        auto q = [](float x) { int v = (int)std::lround(127.4f + x * 128.f); return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
        out[2 * i] = q(re); out[2 * i + 1] = q(im);
    }
    FILE* f = fopen(argv[3], "wb"); fwrite(out.data(), 1, out.size(), f); fclose(f);
    return 0;
}
