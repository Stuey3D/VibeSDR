// tools/msk144-bench/msk144_bench.cpp — VibeSDR's MSK144 receiver measured: the code against the black-box corpus,
// sweeps on synthetic pings, noise, and recordings (.wav, 12 kHz mono 16-bit). Silent: nothing is played.
//   msk144_bench code <msk144code output>…          every message re-encoded, tones compared
//   msk144_bench sweep <bits> <snrLo> <snrHi> <step> <trials> [fadeSec]   pings of <bits> bits (144 = one frame)
//   msk144_bench noise <seconds>                     false decodes on white noise
//   msk144_bench wav <file.wav>…                     decode files, one line a decode
#include "msk144_synth.h"
#include "decoders/digimodes/msk144_decoder.h"
#include "decoders/digimodes/ldpc_small.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace vibe;

static const char* kMsgs[] = {"G4ABC PA3XYZ JO22", "CQ DL1AAA JO62", "OH2BH G3WOS -05", "SP9XYZ OK1ABC R-03",
                              "I5WBE F6ABC RRR", "EA3ABC G0ISW 73", "CQ M0ABC IO91", "LZ1ABC YO3XYZ KN34",
                              "9A2AA S51ZZ RR73", "PA0EHG ON4ABC R+02", "CQ OZ1ABC JO55", "G8ABC GW4XYZ IO81"};

static int codeCheck(int argc, char** argv) {
    int n = 0, ok = 0;
    for (int a = 2; a < argc; ++a) {
        std::ifstream f(argv[a]); std::string line, msg; std::vector<std::string> L;
        while (std::getline(f, line)) L.push_back(line);
        for (size_t i = 0; i + 3 < L.size(); ++i) {
            if (L[i].rfind("=== ", 0) != 0) continue;
            msg = L[i].substr(4);
            const std::string want = L[i + 2] + L[i + 3];
            uint8_t t[144]; bool enc = false;
            // telemetry: 18 hex digits → 71 bits, n3 = 5, i3 = 0 (the FT8 family's packing)
            bool hex = msg.size() == 18 && msg.find_first_not_of("0123456789ABCDEF") == std::string::npos;
            if (hex) {
                uint8_t b[77] = {};
                for (int k = 0; k < 18; ++k) {
                    const int v = std::stoi(msg.substr((size_t)k, 1), nullptr, 16);
                    for (int j = 0; j < 4; ++j) { const int bit = 4 * k + j - 1; if (bit >= 0) b[bit] = (uint8_t)((v >> (3 - j)) & 1); }
                }
                b[71] = 1; b[72] = 0; b[73] = 1;
                uint8_t cw[msk144::kN]; msk144::encode(b, cw); msk144::tones(cw, t); enc = true;
            } else enc = msk144::encodeText(msg.c_str(), t);
            if (!enc) continue;
            std::string got; for (int k = 0; k < 144; ++k) got += (char)('0' + t[k]);
            ++n; ok += got == want;
            if (got != want) std::printf("MISMATCH %s\n", msg.c_str());
        }
    }
    std::printf("code: %d/%d black-box tone sequences reproduced\n", ok, n);
    return ok == n ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "see the file header\n"); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "code") return codeCheck(argc, argv);
    msk144::Options o;
    if (std::getenv("MSK_NOOSD")) o.osd = false;
    if (std::getenv("MSK_OSD")) o.osd = true;
    if (std::getenv("MSK_CANDS")) o.maxCands = std::atoi(std::getenv("MSK_CANDS"));
    if (cmd == "sweep" && argc >= 7) {
        const int bits = std::atoi(argv[2]);
        const double lo = std::atof(argv[3]), hi = std::atof(argv[4]), step = std::atof(argv[5]);
        const int trials = std::atoi(argv[6]);
        const double fade = argc > 7 ? std::atof(argv[7]) : 0;
        double ms = 0, sec = 0;
        for (double snr = lo; snr <= hi + 1e-9; snr += step) {
            int got = 0, wrong = 0, osd = 0, mrc = 0; double se = 0, fe = 0;
            const int only = std::getenv("MSK_ONLY") ? std::atoi(std::getenv("MSK_ONLY")) : -1;
            for (int t = 0; t < trials; ++t) {
                if (only >= 0 && t != only) continue;
                const char* want = kMsgs[t % 12];
                uint8_t tones[144]; msk144::encodeText(want, tones);
                auto a = msksynth::noise(4.0, 7u + (uint32_t)t * 131u + (uint32_t)((snr + 50) * 10));
                const double f = std::getenv("MSK_F") ? std::atof(std::getenv("MSK_F")) : 1500 + ((t * 37) % 101 - 50) * 1.0 + 0.37 * t;
                const double start = 0.7 + 0.13 * (t % 7);
                msksynth::addPing(a, tones, f, start, bits, (t * 53) % 144, snr, 0.7 * t, fade);
                msk144::Receiver r(o);
                bool ok = false;
                r.onResult = [&](const msk144::Result& x) {
                    if (std::getenv("MSK_GSTAT")) std::printf("G %s osd %d errs %d frames %d score %.1f iters %d mrc %d\n", x.text == want ? "OK" : "BAD", x.osd, x.errors, x.frames, x.score, x.iters, x.mrc);
                    if (x.text == want) { if (!ok) { se += x.snr - snr; fe = std::max(fe, (double)std::fabs(x.f0 - f)); osd += x.osd; mrc += x.mrc; } ok = true; }
                    else { ++wrong; std::printf("  WRONG %s (sent %s)\n", x.text.c_str(), want); }
                };
                r.push(a.data(), (int)a.size()); r.flush();
                ms += r.stats.ms; sec += a.size() / 12000.0;
                got += ok;
                if (!ok && std::getenv("MSK_MISS")) std::printf("  MISS t=%d f=%.2f start=%.2f first=%d\n", t, f, start, (t * 53) % 144);
            }
            std::printf("snr %6.1f  decoded %3d/%d (%5.1f%%)  wrong %d  osd %d mrc %d  max|df| %.2f  snrBias %+.1f\n", snr, got, trials,
                        100.0 * got / trials, wrong, osd, mrc, fe, got ? se / got : 0);
            std::fflush(stdout);
        }
        std::printf("analysis %.2f ms per second of audio\n", ms / std::max(1e-9, sec));
        return 0;
    }
    if (cmd == "noise" && argc >= 3) {
        const double secs = std::atof(argv[2]);
        msk144::Receiver r(o);
        int falseN = 0;
        r.onResult = [&](const msk144::Result& x) { ++falseN; std::printf("FALSE %s (osd %d frames %d errs %d score %.1f)\n", x.text.c_str(), x.osd, x.frames, x.errors, x.score); };
        double done = 0; uint32_t seed = 4242;
        while (done < secs) { auto a = msksynth::noise(10.0, seed++); r.push(a.data(), (int)a.size()); done += 10; }
        r.flush();
        std::printf("noise %.0f s: false %d, windows %llu, candidates %llu, attempts %llu, rejected %llu, %.2f ms per second of audio\n", secs, falseN,
                    (unsigned long long)r.stats.windows, (unsigned long long)r.stats.candidates, (unsigned long long)r.stats.attempts,
                    (unsigned long long)r.stats.rejected, r.stats.ms / secs);
        return falseN ? 1 : 0;
    }
    if (cmd == "cpu" && argc >= 3) {   // the whole receiver (mix, decimation, matched filter, search, decodes) on noise
        const double secs = std::atof(argv[2]);
        msk144::Receiver r(o);
        std::vector<std::vector<float>> blocks;
        for (int i = 0; i < 6; ++i) blocks.push_back(msksynth::noise(10.0, 99u + (uint32_t)i));
        const auto t0 = std::chrono::steady_clock::now();
        double done = 0; int bi = 0;
        while (done < secs) { auto& a = blocks[(size_t)(bi++ % 6)]; for (size_t i = 0; i < a.size(); i += 1200) r.push(a.data() + i, 1200); done += 10; }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("cpu: %.0f s of audio in %.1f ms → %.1f ms a minute (search+decodes %.1f ms a minute)\n", done, ms, ms / done * 60, r.stats.ms / done * 60);
        return 0;
    }
    if (cmd == "wav") {
        for (int a = 2; a < argc; ++a) {
            FILE* f = std::fopen(argv[a], "rb"); if (!f) continue;
            std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 44, SEEK_SET);
            std::vector<int16_t> s((size_t)(sz - 44) / 2); size_t got = std::fread(s.data(), 2, s.size(), f); std::fclose(f);
            std::vector<float> x(got); for (size_t i = 0; i < got; ++i) x[i] = s[i] / 32768.f;
            msk144::Receiver r(o);
            const char* name = std::strrchr(argv[a], '/'); name = name ? name + 1 : argv[a];
            std::printf("=== %s\n", name);
            r.onResult = [&](const msk144::Result& x) { std::printf("%6.2f %4d %6.1f %2d %s%s  %s\n", x.audioSec, x.snr, x.f0, x.frames, x.osd ? "o" : "-", x.mrc ? "m" : "-", x.text.c_str()); };
            r.push(x.data(), (int)x.size()); r.flush();
        }
        return 0;
    }
    std::fprintf(stderr, "see the file header\n");
    return 2;
}
