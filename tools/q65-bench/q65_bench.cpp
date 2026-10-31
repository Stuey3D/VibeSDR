// tools/q65-bench/q65_bench.cpp — VibeSDR's Q65 decoder measured: the encoder against black-box vectors, sensitivity
// sweeps on synthetic signals, false decodes on noise, recordings (.wav, 12 kHz mono 16-bit) and CPU.
//   q65_bench vectors <file.tsv>                         message<TAB>85 tones — every one must be reproduced
//   q65_bench sweep <period> <sub A-E> <snrLo> <snrHi> <step> <trials> [spreadHz] [noAP]
//   q65_bench noise <period> <windows> [sub mask hex]
//   q65_bench wav <file.wav> <period> [sub mask hex]     the file starts at the period's start (as WSJT-X writes them)
// Silent: nothing is played.
#include "decoders/digimodes/q65_decoder.h"
#include "q65_synth.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vibe;

static int subOf(const char* s) { return (s[0] >= 'A' && s[0] <= 'E') ? s[0] - 'A' : std::atoi(s); }

static std::vector<float> readWav(const char* path) {
    std::vector<float> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 44, SEEK_SET);
    std::vector<int16_t> s((size_t)std::max(0L, (sz - 44) / 2));
    size_t got = std::fread(s.data(), 2, s.size(), f); std::fclose(f);
    out.resize(got);
    for (size_t i = 0; i < got; ++i) out[i] = s[i] / 32768.f;
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "see the file header\n"); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "vectors" && argc >= 3) {
        std::ifstream in(argv[2]); std::string line; int n = 0, bad = 0;
        while (std::getline(in, line)) {
            const size_t tab = line.find('\t'); if (tab == std::string::npos) continue;
            const std::string msg = line.substr(0, tab);
            std::istringstream ss(line.substr(tab + 1)); int t; std::vector<int> want; while (ss >> t) want.push_back(t);
            uint8_t tones[q65::kSymbols];
            bool ok = q65::encodeText(msg.c_str(), tones) && want.size() == q65::kSymbols;
            for (int i = 0; ok && i < q65::kSymbols; ++i) ok = tones[i] == want[(size_t)i];
            ++n; if (!ok) { ++bad; std::printf("MISMATCH %s\n", msg.c_str()); }
        }
        std::printf("vectors %d, mismatches %d\n", n, bad);
        return bad ? 1 : 0;
    }
    if (cmd == "sweep" && argc >= 8) {
        const int P = std::atoi(argv[2]), sub = subOf(argv[3]);
        const double lo = std::atof(argv[4]), hi = std::atof(argv[5]), step = std::atof(argv[6]);
        const int trials = std::atoi(argv[7]);
        const double spread = argc > 8 ? std::atof(argv[8]) : 0;
        const bool noAp = argc > 9 && !std::strcmp(argv[9], "noAP");
        q65::SlotDecoder d(P);
        if (!d.ok()) { std::fprintf(stderr, "bad period\n"); return 2; }
        const char* msgs[] = {"CQ K1ABC FN42", "G4ABC DL1AAA JO62", "W9XYZ K1ABC -15", "OH2BH VK2ABC RR73", "CQ M0XYZ IO91"};
        q65::SlotOptions o; o.submodes = std::getenv("Q65_MASK") ? (unsigned)std::strtoul(std::getenv("Q65_MASK"), nullptr, 16) : 1u << sub; o.apCq = !noAp;
        double ms = 0; int wins = 0;
        for (double snr = lo; snr <= hi + 1e-9; snr += step) {
            int okN = 0, wrong = 0, apN = 0; double dtErr = 0, fErr = 0, snrErr = 0;
            for (int t = 0; t < trials; ++t) {
                const char* m = msgs[t % 5];
                uint8_t tones[q65::kSymbols]; q65::encodeText(m, tones);
                auto w = q65synth::noiseWindow(P, 1000u + (uint32_t)t * 31u + (uint32_t)(snr * 10 + 500));
                q65synth::Sig s; s.tones = tones; s.submode = sub; s.f0 = 800 + 37.3 * (t % 11); s.dt = -0.4 + 0.27 * (t % 7);
                s.snrDb = snr; s.spreadHz = spread;
                q65synth::addSignal(w, P, s, (uint32_t)t);
                std::vector<q65::Result> res; q65::SlotStats st;
                d.decode(w.data(), (int)w.size(), o, res, &st);
                ms += st.totalMs; ++wins;
                bool got = false;
                for (auto& r : res) {
                    if (std::getenv("Q65_FEAT")) std::printf("FEAT %d %.1f %.3f %d %d %d %.2f %d\n", r.text == m ? 1 : 0, snr, r.score, r.hard, r.iters, r.ap, r.sync, r.snr);
                    if (r.text == m) { got = true; apN += r.ap ? 1 : 0; dtErr += std::fabs(r.dt - s.dt); fErr += std::fabs(r.f0 - s.f0); snrErr += r.snr - snr; }
                    else { ++wrong; std::printf("  WRONG %s (sent %s) score %.2f ap %d it %d sync %.2f snr %d\n", r.text.c_str(), m, r.score, r.ap, r.iters, r.sync, r.snr); }
                }
                okN += got;
            }
            std::printf("snr %6.1f  decoded %3d/%d (%5.1f%%)  ap %d  wrong %d  |dt| %.3f  |df| %.2f  snrBias %+.1f\n", snr, okN, trials,
                        100.0 * okN / trials, apN, wrong, okN ? dtErr / okN : 0, okN ? fErr / okN : 0, okN ? snrErr / okN : 0);
            std::fflush(stdout);
        }
        std::printf("mean decode %.1f ms per window\n", ms / std::max(1, wins));
        return 0;
    }
    if (cmd == "noise" && argc >= 4) {
        const int P = std::atoi(argv[2]), N = std::atoi(argv[3]);
        q65::SlotDecoder d(P);
        q65::SlotOptions o; if (argc > 4) o.submodes = (unsigned)std::strtoul(argv[4], nullptr, 16);
        int falseN = 0, cands = 0, attempts = 0, rejected = 0; double ms = 0;
        for (int i = 0; i < N; ++i) {
            auto w = q65synth::noiseWindow(P, 777u + (uint32_t)i * 101u);
            std::vector<q65::Result> res; q65::SlotStats st;
            d.decode(w.data(), (int)w.size(), o, res, &st);
            cands += st.candidates; attempts += st.attempts; rejected += st.rejected; ms += st.totalMs;
            for (auto& r : res) { if (std::getenv("Q65_FEAT")) std::printf("FEAT 0 99 %.3f %d %d %d %.2f %d\n", r.score, r.hard, r.iters, r.ap, r.sync, r.snr); ++falseN; std::printf("FALSE %s  score %.2f ap %d it %d sync %.2f\n", r.text.c_str(), r.score, r.ap, r.iters, r.sync); }
        }
        std::printf("noise windows %d: false %d, candidates %d, BP attempts %d, rejected by guard %d, %.1f ms/window\n",
                    N, falseN, cands, attempts, rejected, ms / std::max(1, N));
        return falseN ? 1 : 0;
    }
    if (cmd == "wav" && argc >= 4) {
        const int P = std::atoi(argv[3]);
        q65::SlotDecoder d(P);
        auto a = readWav(argv[2]);
        std::vector<float> w((size_t)q65::windowSamples(d.period()), 0.f);
        for (size_t i = 0; i < a.size() && i + q65::kRate < w.size(); ++i) w[i + q65::kRate] = a[i];
        q65::SlotOptions o; if (argc > 4) o.submodes = (unsigned)std::strtoul(argv[4], nullptr, 16);
        std::vector<q65::Result> res; q65::SlotStats st;
        d.decode(w.data(), (int)w.size(), o, res, &st);
        for (auto& r : res) std::printf("%s  %4d dB  dt %+.2f  %7.1f Hz  %s%s  (score %.1f, it %d)\n", r.mode().c_str(), r.snr, r.dt, r.f0, r.text.c_str(), r.ap ? "  [ap]" : "", r.score, r.iters);
        std::printf("cands %d attempts %d decodes %d  %.1f ms (search %.1f, fft %.1f, cands %.1f)\n", st.candidates, st.attempts, st.decodes, st.totalMs, st.searchMs, st.fftMs, st.candMs);
        return 0;
    }
    std::fprintf(stderr, "see the file header\n");
    return 2;
}
