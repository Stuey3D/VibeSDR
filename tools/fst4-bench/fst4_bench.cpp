// tools/fst4-bench/fst4_bench.cpp — VibeSDR's FST4 / FST4W decoder measured: sweeps on synthetic signals, noise, and
// recordings (.wav, 12 kHz mono 16-bit, starting at the period's start as WSJT-X writes them).
//   fst4_bench sweep <fst4|fst4w> <period> <snrLo> <snrHi> <step> <trials>
//   fst4_bench noise <fst4|fst4w> <period> <windows>
//   fst4_bench wav <fst4|fst4w> <period> <file.wav> [centreHz]
// Silent: nothing is played.
#include "fst4_synth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace vibe;
static fst4::Mode modeOf(const char* s) { return !std::strcmp(s, "fst4w") ? fst4::Mode::FST4W : fst4::Mode::FST4; }

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "see the file header\n"); return 2; }
    const std::string cmd = argv[1];
    const fst4::Mode m = modeOf(argv[2]);
    const int P = std::atoi(argv[3]);
    const bool w = m == fst4::Mode::FST4W;
    const double fc = w ? 1500 : 1500;
    fst4::SlotOptions o; if (w) { o.loHz = 1400; o.hiHz = 1600; }
    if (std::getenv("FST4_NOOSD")) o.osd = false;
    fst4::SlotDecoder d(m, P, fc);
    if (!d.ok()) { std::fprintf(stderr, "no such mode/period\n"); return 2; }
    if (cmd == "sweep" && argc >= 8) {
        const double lo = std::atof(argv[4]), hi = std::atof(argv[5]), step = std::atof(argv[6]);
        const int trials = std::atoi(argv[7]);
        double ms = 0; int wins = 0;
        for (double snr = lo; snr <= hi + 1e-9; snr += step) {
            int got = 0, wrong = 0; double de = 0, fe = 0, se = 0;
            for (int t = 0; t < trials; ++t) {
                uint8_t tones[160]; std::string want;
                if (w) { const char* calls[] = {"K1ABC", "G4ABC", "DL1AAA", "VK2ABC"}; const char* grids[] = {"FN42", "IO91", "JO62", "QF56"};
                         fst4::encodeWspr(calls[t % 4], grids[t % 4], 37, tones); want = std::string(calls[t % 4]) + " " + grids[t % 4] + " 37"; }
                else { const char* msgs[] = {"CQ K1ABC FN42", "G4ABC DL1AAA JO62", "W9XYZ K1ABC -15", "OH2BH VK2ABC RR73"};
                       fst4::encodeText(msgs[t % 4], tones); want = msgs[t % 4]; }
                auto a = fst4synth::noiseWindow(P, 100u + (uint32_t)t * 37u + (uint32_t)(snr * 10 + 500));
                const double f0 = w ? 1450 + 7.3 * (t % 9) : 900 + 61.7 * (t % 13), dt = -0.4 + 0.17 * (t % 6);
                fst4synth::addSignal(a, P, tones, f0, dt, snr);
                fst4::g_truthTones = tones;
                auto z = fst4synth::toBaseband(a, m, P, fc);
                std::vector<fst4::Result> res; fst4::SlotStats st;
                d.decode(z.data(), (int)z.size(), o, res, &st);
                ms += st.totalMs; ++wins;
                bool ok = false;
                for (auto& r : res) {
                    if (r.text == want) { ok = true; de = std::max(de, (double)std::fabs(r.dt - dt)); fe = std::max(fe, (double)std::fabs(r.f0 - f0)); se += r.snr - snr; }
                    else { ++wrong; std::printf("  WRONG %s (sent %s)\n", r.text.c_str(), want.c_str()); }
                }
                got += ok;
                if (!ok && std::getenv("FST4_MISS")) std::printf("  MISS t=%d f0=%.1f dt=%.2f\n", t, f0, dt);
            }
            std::printf("snr %6.1f  decoded %3d/%d (%5.1f%%)  wrong %d  max|dt| %.3f  max|df| %.3f  snrBias %+.1f\n", snr, got, trials, 100.0 * got / trials, wrong, de, fe, got ? se / got : 0);
            std::fflush(stdout);
        }
        std::printf("mean decode %.1f ms per window\n", ms / std::max(1, wins));
        return 0;
    }
    if (cmd == "noise" && argc >= 5) {
        const int N = std::atoi(argv[4]);
        int falseN = 0, cands = 0, att = 0; double ms = 0;
        for (int i = 0; i < N; ++i) {
            auto a = fst4synth::noiseWindow(P, 999u + (uint32_t)i * 7u);
            auto z = fst4synth::toBaseband(a, m, P, fc);
            std::vector<fst4::Result> res; fst4::SlotStats st;
            d.decode(z.data(), (int)z.size(), o, res, &st);
            cands += st.candidates; att += st.attempts; ms += st.totalMs;
            for (auto& r : res) { ++falseN; std::printf("FALSE %s\n", r.text.c_str()); }
        }
        std::printf("noise windows %d: false %d, candidates %d, BP attempts %d, %.1f ms/window (decode only)\n", N, falseN, cands, att, ms / std::max(1, N));
        return falseN ? 1 : 0;
    }
    if (cmd == "wav" && argc >= 5) {
        FILE* f = std::fopen(argv[4], "rb"); if (!f) return 2;
        std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 44, SEEK_SET);
        std::vector<int16_t> s((size_t)(sz - 44) / 2); size_t got = std::fread(s.data(), 2, s.size(), f); std::fclose(f);
        std::vector<float> a((size_t)(P + 2) * 12000, 0.f);
        for (size_t i = 0; i < got && i + 12000 < a.size(); ++i) a[i + 12000] = s[i] / 32768.f;
        static uint8_t truth[160];
        if (const char* tm = std::getenv("FST4_TRUTH")) { if (fst4::encodeText(tm, truth)) fst4::g_truthTones = truth; }
        auto z = fst4synth::toBaseband(a, m, P, fc);
        std::vector<fst4::Result> res; fst4::SlotStats st;
        d.decode(z.data(), (int)z.size(), o, res, &st);
        for (auto& r : res) std::printf("%s  %4d dB  dt %+.2f  %8.2f Hz  %s  (it %d, %s)\n", r.modeName().c_str(), r.snr, r.dt, r.f0, r.text.c_str(), r.iters, r.k101 ? "101" : "74");
        std::printf("cands %d attempts %d %.1f ms\n", st.candidates, st.attempts, st.totalMs);
        return 0;
    }
    return 2;
}
