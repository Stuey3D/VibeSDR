// test-lrpt.cpp — METEOR-M LRPT (docs/v12/LRPT.md): the FEC pieces against their encoders, our own LRPT transmitter
// (pictures → JPEG-like MCUs → packets → VCDUs → RS → randomiser → convolutional code → QPSK / OQPSK / 80k) through
// the whole receiver, noise (no picture, ever), the SIMD paths against scalar, and the CPU cost.
//
//   test-lrpt                 the synthetic suite
//   test-lrpt --quick         the same, shorter
//   test-lrpt --wav F [DIR]   decode a 16-bit IQ .wav recording; PNGs into DIR (default .)
//   test-lrpt --soft F [DIR] [--diff] [--int]   decode an int8 soft-symbol (.s) file
#include "vibe_lrpt.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibelrpt;

static bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return (std::fclose(f) == 0) && ok;
}

static void report(const Receiver& rx, double secs, double cpu) {
    const Stats s = rx.stats();
    std::printf("  %.1f s: mode %s diff %d | carrier lock %d (%.2f) sync %d | CADUs %llu RS ok %llu fail %llu fixed %llu | packets %llu bad %llu blocks %llu | scid %d | cpu %.3f s (%.2f %% of real time)\n",
                secs, rx.modeName(), rx.diffWinner(), rx.demodLocked(), rx.lockMetric(), rx.syncLocked(),
                (unsigned long long)s.cadus, (unsigned long long)s.rsOk, (unsigned long long)s.rsFail, (unsigned long long)s.rsFixed,
                (unsigned long long)s.packets, (unsigned long long)s.badPackets, (unsigned long long)s.blocks, s.scid, cpu,
                secs > 0 ? 100.0 * cpu / secs : 0.0);
    for (auto& kv : rx.channels())
        std::printf("    APID %d: %d strips, %llu blocks\n", kv.first, kv.second.strips(), (unsigned long long)kv.second.goodBlocks());
}

static int savePictures(const Receiver& rx, const std::string& dir, const std::string& stem) {
    int n = 0;
    for (auto& p : renderPictures(rx)) {
        std::string nm = p.name; for (auto& c : nm) if (c == ' ') c = '_';
        const std::string path = dir + "/" + stem + "-" + nm + ".png";
        if (writeFile(path, p.png)) { std::printf("  wrote %s (%dx%d, %zu bytes)\n", path.c_str(), p.w, p.h, p.png.size()); n++; }
    }
    return n;
}

static int decodeWav(const char* path, const std::string& dir, const ReceiverConfig& base) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 2; }
    uint8_t hdr[12]; if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) { std::printf("not a WAV\n"); std::fclose(f); return 2; }
    uint32_t rate = 0; uint16_t ch = 0, bits = 0; long dataAt = -1; uint32_t dataLen = 0;
    for (;;) {
        uint8_t c[8]; if (std::fread(c, 1, 8, f) != 8) break;
        const uint32_t len = uint32_t(c[4]) | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24;
        if (!std::memcmp(c, "fmt ", 4)) {
            uint8_t fm[16]; if (std::fread(fm, 1, 16, f) != 16) break;
            ch = uint16_t(fm[2] | fm[3] << 8); rate = uint32_t(fm[4]) | uint32_t(fm[5]) << 8 | uint32_t(fm[6]) << 16 | uint32_t(fm[7]) << 24;
            bits = uint16_t(fm[14] | fm[15] << 8);
            std::fseek(f, long(len) - 16, SEEK_CUR);
        } else if (!std::memcmp(c, "data", 4)) { dataAt = std::ftell(f); dataLen = len; break; }
        else std::fseek(f, long(len), SEEK_CUR);
    }
    if (dataAt < 0 || ch != 2 || bits != 16) { std::printf("need 2-channel 16-bit IQ (got ch %d bits %d)\n", ch, bits); std::fclose(f); return 2; }
    std::printf("%s: %u S/s, %.1f s\n", path, rate, dataLen / 4.0 / rate);
    ReceiverConfig c = base; c.sampleRate = rate;
    Receiver rx(c);
    if (std::getenv("LRPT_DIAG")) rx.diag = [](int a, int s, int m, uint32_t ms, int q) { std::printf("PKT apid %d seq %d mcu %d ms %u q %d\n", a, s, m, ms, q); };
    std::vector<int16_t> raw(size_t(rate / 10) * 2);
    std::vector<float> iq(raw.size());
    double secs = 0, cpu = 0, nextRep = 10;
    for (;;) {
        const size_t got = std::fread(raw.data(), 4, raw.size() / 2, f);
        if (!got) break;
        for (size_t i = 0; i < got * 2; i++) iq[i] = raw[i] / 32768.f;
        const auto t0 = std::chrono::steady_clock::now();
        rx.feedIq(iq.data(), got);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        secs += double(got) / rate;
        if (secs >= nextRep) { report(rx, secs, cpu); nextRep += 10; }
    }
    std::fclose(f);
    report(rx, secs, cpu);
    std::string stem = path; { const size_t sl = stem.rfind('/'); if (sl != std::string::npos) stem = stem.substr(sl + 1); const size_t d = stem.rfind('.'); if (d != std::string::npos) stem = stem.substr(0, d); }
    const auto t0 = std::chrono::steady_clock::now();
    const int n = savePictures(rx, dir, stem);
    std::printf("  pictures: %d (rendered in %.3f s)\n", n, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return n > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    ReceiverConfig c;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--qpsk")) c.mod = ReceiverConfig::Qpsk;
        if (!std::strcmp(argv[i], "--oqpsk")) c.mod = ReceiverConfig::Oqpsk;
        if (!std::strcmp(argv[i], "--80k")) { c.symbolRate = 80000; c.interleaved = true; }
        if (!std::strcmp(argv[i], "--scalar")) c.forceScalar = true;
    }
    if (argc >= 3 && !std::strcmp(argv[1], "--wav")) return decodeWav(argv[2], argc >= 4 && argv[3][0] != '-' ? argv[3] : ".", c);
    std::printf("(synthetic suite not written yet)\n");
    return 0;
}
