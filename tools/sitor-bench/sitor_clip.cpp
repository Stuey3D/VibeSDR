// sitor_clip — the server tests' IQ clip (scripts/test-web-sitora.mjs): a whole SITOR-A link (sitor_synth.h — the call
// to 002320204 from 364775427, identification, traffic, a change-over each way, the end) as unsigned 8-bit IQ at `rate`,
// centred on `centre`, the link's tones ±85 Hz about `sitorHz` (B = the higher). vibeserver/fake-rtl-tcp.mjs loops it.
//
//   sitor_clip out.cu8 [rate=960000] [centre=4209500] [sitorHz=4209500]
#include "sitor_synth.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s out.cu8 [rate] [centre] [sitorHz]\n", argv[0]); return 2; }
    const double rate = argc > 2 ? std::atof(argv[2]) : 960000, centre = argc > 3 ? std::atof(argv[3]) : 4209500,
                 sitorHz = argc > 4 ? std::atof(argv[4]) : 4209500;
    using namespace sitorsynth;
    const Plan L = makeLink("002320204", "364775427", "QTH 43.10N 009.25E ETA LA SPEZIA 0600Z\nPLEASE ADVISE BERTH (MV TEST)",
                            "BERTH 4 CONFIRMED, PILOT AT 0530Z", "THANKS QRU", 4, 7);
    const double cyc = 0.45, spb = rate / 100.0, startS = 1.0, dMs = 30;
    const size_t cycles = (L.turns.size() + 1) / 2;
    const size_t n = (size_t)((startS + cycles * cyc + 2) * rate);
    std::vector<uint8_t> b(n * 2);
    std::mt19937 g(2026); std::normal_distribution<double> N(0, 1);
    std::vector<float> re(n, 0.0f), im(n, 0.0f);
    for (size_t k = 0; k < L.turns.size(); k += 2) {
        const double tm = startS + (k / 2) * cyc;
        const size_t mLen = L.turns[k].codes.empty() ? 1 : L.turns[k].codes.size();
        for (int j = 0; j < 2 && k + j < L.turns.size(); j++) {
            const Turn& t = L.turns[k + j];
            if (t.codes.empty()) continue;
            const double start = t.master ? tm : tm + mLen * 0.07 + dMs / 1000;
            const double amp = t.master ? 0.15 : 0.08, off = sitorHz - centre + (t.master ? 0 : 12);
            const size_t a0 = (size_t)(start * rate), len = (size_t)(t.codes.size() * 7 * spb);
            double ph = 0;
            for (size_t i = 0; i < len && a0 + i < n; i++) {
                const size_t bit = std::min((size_t)(i / spb), t.codes.size() * 7 - 1);
                const bool one = (t.codes[bit / 7] >> (bit % 7)) & 1;
                ph += 2 * M_PI * (off + (one ? 85.0 : -85.0)) / rate;
                re[a0 + i] += (float)(amp * std::cos(ph)); im[a0 + i] += (float)(amp * std::sin(ph));
            }
        }
    }
    auto q = [](double v) { const int x = (int)std::lround(127.5 + v * 18.0); return (uint8_t)std::max(0, std::min(255, x)); };
    for (size_t i = 0; i < n; i++) { b[2 * i] = q(re[i] + 0.707 * N(g)); b[2 * i + 1] = q(im[i] + 0.707 * N(g)); }
    FILE* f = std::fopen(argv[1], "wb"); if (!f) { std::perror(argv[1]); return 1; }
    std::fwrite(b.data(), 1, b.size(), f); std::fclose(f);
    std::printf("%s: %.0f s at %.0f Hz centred on %.0f, a SITOR-A link on %.0f Hz (%zu cycles)\n", argv[1], n / rate, rate, centre, sitorHz, cycles);
    return 0;
}
