// make-ism-records — the ISM records of decoders-bench.vbu8 (README-decoders.txt), from Stuart's own captures with EVERY
// REAL DEVICE LEFT OUT (Stuart, 2026-10-31: a capture holds neighbours' sensor IDs, key-fob codes and tyre IDs).
//
//   make-ism-records <433.u8 @2.4 MS/s on 433.92 MHz> <868.u8 @2.4 MS/s on 868.6 MHz> <out.part>
//
// ★★★ PRIVACY, BY CONSTRUCTION. From each capture it takes a half-second window in which:
//     • ISM's own front end — the burst detector the server runs, on the WHOLE 2.4 MHz capture, not just the band — finds
//       NO burst at all (so no device transmitted anything it can even see, let alone decode), and
//     • on 868, LoRa's detectors (every plan channel the capture holds, no budget) find NO preamble,
//     and then adds SYNTHETIC devices into it: test-ism's and test-wmbus's own transmitters, built from the published
//     specs, with the tests' made-up IDs. What is real is the noise floor, the band's carriers and the tuner's spurs —
//     which is what the burst finder's cost depends on. The windows chosen, and what was decoded from the result, are
//     printed; anything other than exactly the synthetic devices refuses to write.
// ★ Tool only (vibeserver/bench-clip). Build: see README-decoders.txt.
#include "vibe_ism_service.h"
#include "vibe_lora_service.h"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using cf = std::complex<float>;
std::vector<cf> ism433Synthetic(double centre, double rate, double lenS);
size_t ism433Bursts(const std::vector<cf>& x, double rate, std::vector<std::string>* models);
std::vector<cf> ism868Synthetic(double centre, double rate, double lenS);
size_t ism868Bursts(const std::vector<cf>& x, double rate, double hw, std::vector<std::string>* models);

static const double kRate = 2.4e6, kLen = 0.5;

static bool readWindow(const char* path, double startS, std::vector<cf>& x) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    const size_t n = size_t(kRate * kLen);
    std::vector<uint8_t> u(n * 2);
    if (std::fseek(f, long(size_t(startS * kRate) * 2), SEEK_SET) != 0 || std::fread(u.data(), 1, u.size(), f) != u.size()) { std::fclose(f); return false; }
    std::fclose(f);
    x.resize(n);
    for (size_t i = 0; i < n; ++i) x[i] = cf((u[2 * i] - 127.5f) / 127.5f, (u[2 * i + 1] - 127.5f) / 127.5f);
    return true;
}
static bool loraQuiet(const std::vector<cf>& x, double centre) {
    vibelora::Engine e;
    e.startInline("868", centre, kRate, INFINITY);
    for (size_t o = 0; o < x.size(); o += 65536) e.process(&x[o], std::min<size_t>(65536, x.size() - o));
    uint64_t ok = 0, hdr = 0, crc = 0; e.counts(ok, hdr, crc);
    return ok == 0 && hdr == 0 && crc == 0 && e.stats().empty();
}
static void writeRecord(FILE* out, const char* id, double centre, const std::vector<cf>& x) {
    char name[8] = {0}; std::snprintf(name, sizeof name, "%s", id);
    const uint32_t bits = 8, n = uint32_t(x.size());
    std::fwrite(name, 1, 8, out); std::fwrite(&kRate, 8, 1, out); std::fwrite(&centre, 8, 1, out);
    std::fwrite(&bits, 4, 1, out); std::fwrite(&n, 4, 1, out);
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i]     = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].real() * 127.5f + 127.5f))));
        u[2 * i + 1] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].imag() * 127.5f + 127.5f))));
    }
    std::fwrite(u.data(), 1, u.size(), out);
}

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: make-ism-records <433.u8> <868.u8> <out.part>\n"); return 2; }
    FILE* out = std::fopen(argv[3], "wb");
    if (!out) return 1;
    struct Want { const char* id; const char* path; double centre; bool m868; std::set<std::string> expect; };
    const Want wants[] = { { "ism433", argv[1], 433.92e6, false, {} }, { "ism868", argv[2], 868.6e6, true, {} } };
    for (const Want& w : wants) {
        bool done = false;
        for (double s = 0.5; s < 29.0 && !done; s += kLen) {
            std::vector<cf> x;
            if (!readWindow(w.path, s, x)) break;
            const size_t real = w.m868 ? ism868Bursts(x, kRate, w.centre, nullptr) : ism433Bursts(x, kRate, nullptr);
            if (real) continue;
            if (w.m868 && !loraQuiet(x, w.centre)) continue;
            // ★ the window is empty of devices: synthetic ones in, and what comes out must be exactly them
            const auto syn = w.m868 ? ism868Synthetic(w.centre, kRate, kLen) : ism433Synthetic(w.centre, kRate, kLen);
            std::vector<cf> y(x.size());
            for (size_t i = 0; i < x.size(); ++i) y[i] = x[i] + syn[i];
            // ★ quantise as the record will hold it, and decode THAT
            for (auto& v : y) v = cf((std::round(std::max(0.0f, std::min(255.0f, v.real() * 127.5f + 127.5f))) - 127.5f) / 127.5f,
                                     (std::round(std::max(0.0f, std::min(255.0f, v.imag() * 127.5f + 127.5f))) - 127.5f) / 127.5f);
            std::vector<std::string> models, synModels;
            const size_t nb = w.m868 ? ism868Bursts(y, kRate, w.centre, &models) : ism433Bursts(y, kRate, &models);
            if (w.m868) ism868Bursts(syn, kRate, w.centre, &synModels); else ism433Bursts(syn, kRate, &synModels);
            std::sort(models.begin(), models.end()); std::sort(synModels.begin(), synModels.end());
            std::printf("%s: window at %.1f s — no real burst%s; with the synthetic devices: %zu bursts, decoded:", w.id, s,
                        w.m868 ? ", no LoRa preamble" : "", nb);
            for (const auto& m : models) std::printf(" [%s]", m.c_str());
            std::printf("\n");
            if (models.empty() || models != synModels) { std::printf("   …not exactly the synthetic devices — next window\n"); continue; }
            writeRecord(out, w.id, w.centre, y);
            done = true;
        }
        if (!done) { std::fprintf(stderr, "%s: no window qualified — nothing written\n", w.id); std::fclose(out); std::remove(argv[3]); return 1; }
    }
    std::fclose(out);
    return 0;
}
