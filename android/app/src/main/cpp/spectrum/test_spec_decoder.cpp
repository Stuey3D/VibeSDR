// Host test for SpecDecoder (the VibeServer/UberSDR SPEC frame) — see spec_decoder.h.
#include "spec_decoder.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace vibespec;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static std::vector<uint8_t> hdr(uint8_t flags, uint64_t freq) {
    std::vector<uint8_t> b = {'S','P','E','C', 1, flags};
    for (int i = 0; i < 8; i++) b.push_back(0);                       // timestamp
    for (int i = 0; i < 8; i++) b.push_back((uint8_t)(freq >> (8 * i)));
    return b;
}
static void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 255); b.push_back(v >> 8); }
static void putf(std::vector<uint8_t>& b, float f) { uint32_t u; std::memcpy(&u, &f, 4); for (int i = 0; i < 4; i++) b.push_back((uint8_t)(u >> (8 * i))); }
int main() {
    SpecDecoder d; double fq = 0;
    // delta before any full frame: sized from the config hint (the UberSDR case)
    d.setBinCount(8);
    auto b = hdr(0x04, 100000000ULL); put16(b, 2); put16(b, 1); b.push_back(200); put16(b, 7); b.push_back(10);
    CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kSpectrum && d.bins().size() == 8, "delta u8 into the config-sized array");
    CHECK(d.bins()[1] == -56 && d.bins()[7] == -246 && d.bins()[0] == 0, "delta u8 values");
    CHECK(fq == 100000000.0, "frequency %f", fq);
    // full u8
    b = hdr(0x03, 7000000000ULL); for (int i = 0; i < 16; i++) b.push_back((uint8_t)(i * 10));
    CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kSpectrum && d.bins().size() == 16 && d.bins()[3] == 30 - 256, "full u8");
    CHECK(fq == 7000000000.0, "64-bit frequency %f", fq);
    // full f32 + delta f32
    b = hdr(0x01, 1); for (int i = 0; i < 4; i++) putf(b, -100.5f + i);
    CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kSpectrum && d.bins().size() == 4 && d.bins()[2] == -98.5f, "full f32");
    b = hdr(0x02, 1); put16(b, 1); put16(b, 3); putf(b, -12.25f);
    CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kSpectrum && d.bins()[3] == -12.25f && d.bins()[2] == -98.5f, "delta f32");
    // guards
    b = hdr(0x03, 1);                       CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kFault && d.bins().size() == 4, "header-only full frame dropped");
    b = hdr(0x01, 1); b.push_back(1);       CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kFault, "odd float body");
    b = hdr(0x03, 1); b[0] = 'X'; b.push_back(1); CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kFault, "bad magic");
    uint8_t gz[] = {0x1f, 0x8b, 8, 0};      CHECK(d.feed(gz, 4, fq) == SpecDecoder::kNotSpec, "gzip is not spectrum");
    b = hdr(0x04, 1); put16(b, 3); put16(b, 0); b.push_back(1); CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kSpectrum, "truncated delta applies what fits");
    b = hdr(0x04, 1); b.push_back(1);       CHECK(d.feed(b.data(), b.size(), fq) == SpecDecoder::kIgnored, "delta body < 2: silent, like the JS");
    SpecDecoder f; CHECK(f.bins().size() == 1024 && f.binCount() == 1024, "starts as the JS does: 1024 zeros");
    f.setBinCount(2048); CHECK(f.bins().size() == 2048, "config binCount change reallocates");
    std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
