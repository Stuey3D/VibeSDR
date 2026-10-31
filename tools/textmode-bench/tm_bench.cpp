// tools/textmode-bench/tm_bench.cpp — sensitivity of the text-mode receivers on synthetic AWGN (SNR in 3 kHz, signal
// power over the transmission) and their cost.   tm_bench sweep MODE[,MODE…] SNR_LO SNR_HI STEP TRIALS [auto]
//                                                 tm_bench cost FAMILY SECONDS [MODE]   (noise through AUTO — or MODE, a
//                                                   manual sub-mode or "any-<family>" — for qemu counts)
#include "decoders/textmodes.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>
using namespace vibe;
using F = TextModeDecoder::Family;
static F famOf(const std::string& id) { return id.rfind("mfsk", 0) == 0 ? F::Mfsk : id.rfind("thor", 0) == 0 ? F::Thor : F::Olivia; }
static int edit(const std::string& a, const std::string& b) {
    std::vector<int> p(b.size() + 1), c(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) p[j] = (int)j;
    for (size_t i = 1; i <= a.size(); ++i) { c[0] = (int)i;
        for (size_t j = 1; j <= b.size(); ++j) c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + (a[i - 1] != b[j - 1])}); std::swap(p, c); }
    return p[b.size()];
}
int main(int argc, char** argv) {
    if (argc >= 4 && !std::strcmp(argv[1], "cost")) {
        F f; TextModeDecoder::familyOf(argv[2], f);
        const int secs = std::atoi(argv[3]);
        TextModeDecoder d(f, 48000, argc > 4 ? argv[4] : "auto"); int chars = 0;
        d.onText = [&](const std::string& s) { chars += (int)s.size(); };
        std::vector<float> x(48000); uint32_t s = 12345;
        for (auto& v : x) { s = s * 1664525u + 1013904223u; v = 0.3f * (float)((int32_t)s) * (1.0f / 2147483648.0f); }
        for (int k = 0; k < secs; ++k) for (int i = 0; i < 48000; i += 960) d.feed(x.data() + i, 960);
        std::printf("tm_bench cost %s %s: %d s, %d characters\n", argv[2], argc > 4 ? argv[4] : "auto", secs, chars);
        return 0;
    }
    if (argc < 7) return 2;
    const std::string msg = "CQ CQ DE G4XYZ G4XYZ IO91 PSE K";
    const double lo = std::atof(argv[3]), hi = std::atof(argv[4]), step = std::atof(argv[5]);
    const int trials = std::atoi(argv[6]); const bool useAuto = argc > 7;
    std::stringstream ss(argv[2]); std::string id;
    while (std::getline(ss, id, ',')) {
        std::printf("%-18s", id.c_str());
        for (double snr = lo; snr <= hi + 1e-9; snr += step) {
            int whole = 0, wrong = 0, chars = 0;
            for (int t = 0; t < trials; ++t) {
                const double fs = 48000;
                auto x = textModeSynth(famOf(id), id, msg, fs, 1100 + 37 * t, 1 + t);
                double p = 0; size_t n = 0; for (float v : x) if (v != 0.f) { p += v * v; ++n; } p /= n;
                std::vector<float> pad((size_t)(fs * 3), 0.f); x.insert(x.begin(), pad.begin(), pad.end()); x.insert(x.end(), pad.begin(), pad.end());
                const float sig = (float)std::sqrt(p / std::pow(10.0, snr / 10.0) * (fs / 2) / 3000.0);
                std::mt19937 r(1000 + t); std::normal_distribution<float> g(0.f, 1.f);
                for (auto& v : x) v += sig * g(r);
                TextModeDecoder d(famOf(id), fs, useAuto ? "auto" : id); std::string got;
                d.onText = [&](const std::string& s) { got += s; };
                for (size_t i = 0; i < x.size(); i += 960) d.feed(x.data() + i, (int)std::min<size_t>(960, x.size() - i));
                std::string body; for (char c : got) if (c != '\n') body += c;
                const size_t h = body.find(']'); if (h != std::string::npos && body[0] == '[') body = body.substr(h + 1);
                std::string want = msg;
                if (body.find(want) != std::string::npos) ++whole;
                const int e = edit(body, want); wrong += e; chars += (int)want.size();
                if (std::getenv("VIBE_TM_SHOW")) std::fprintf(stderr, "  [%s]\n", body.c_str());
            }
            std::printf(" %+5.1f:%2d/%d cer%3.0f%%", snr, whole, trials, 100.0 * wrong / chars);
        }
        std::printf("\n"); std::fflush(stdout);
    }
    return 0;
}
