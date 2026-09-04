// test-dab-ficdec.cpp — THE MILESTONE TEST: build a mux, transmit it, get the station list back.
//
// ★★★ This is a full round trip through everything: FIBs -> CRC -> pack -> scramble -> rate-1/4
//     encode -> puncture (PI 16/15 + tail) -> multiplex four codewords -> [channel] -> demux ->
//     depuncture -> soft Viterbi -> descramble -> CRC -> FIG parse -> services with labels.
//     If this passes with noise applied, the entire chain below the MSC is proven except for the
//     parts that touch real RF (sync, frequency offset), which have their own tests.
#include "vibe_dab_ficdec.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)
static uint32_t seed = 31337; static uint32_t rnd() { seed = seed*1664525u+1013904223u; return seed; }

static std::vector<uint8_t> label16(const char* s) {
    std::vector<uint8_t> v(16, ' ');
    for (size_t i = 0; i < 16 && s[i]; ++i) v[i] = uint8_t(s[i]);
    return v;
}
static std::vector<uint8_t> makeFib(const std::vector<std::pair<int, std::vector<uint8_t>>>& figs) {
    std::vector<uint8_t> fib(32, 0xFF);
    size_t i = 0;
    for (auto& f : figs) {
        if (i + 1 + f.second.size() > 30) break;
        fib[i++] = uint8_t((f.first << 5) | (f.second.size() & 0x1F));
        memcpy(&fib[i], f.second.data(), f.second.size()); i += f.second.size();
    }
    const uint16_t c = uint16_t(~crc16(fib.data(), 30));
    fib[30] = uint8_t(c >> 8); fib[31] = uint8_t(c & 0xFF);
    return fib;
}

/** The transmitter side: 3 FIBs -> 2304 punctured bits. */
static std::vector<uint8_t> encodeFic(const std::vector<uint8_t>& ninetySix) {
    std::vector<uint8_t> bits(kFicDataBits);
    for (int i = 0; i < kFicDataBits; ++i) bits[i] = (ninetySix[i / 8] >> (7 - (i % 8))) & 1;
    EnergyDispersal ed; ed.apply(bits.data(), bits.size());
    auto mother = convEncode(bits.data(), bits.size());          // 3096 bits
    std::vector<uint8_t> out;
    size_t w = 0;
    for (int blk = 0; blk < 21; ++blk) for (int sub = 0; sub < 4; ++sub) for (int i = 0; i < 32; ++i, ++w)
        if (!punctured(16, i)) out.push_back(mother[w]);
    for (int blk = 0; blk < 3; ++blk) for (int sub = 0; sub < 4; ++sub) for (int i = 0; i < 32; ++i, ++w)
        if (!punctured(15, i)) out.push_back(mother[w]);
    for (int i = 0; i < 24; ++i, ++w) if (!tailPunctured(i)) out.push_back(mother[w]);
    return out;
}

int main() {
    printf("test-dab-ficdec\n");

    // The lengths the spec states must fall out of the puncturing, not be asserted by hand.
    CHECK(21 * 4 * (8 + 16) + 3 * 4 * (8 + 15) + 12 == kFicCodewordBits,
          "21 blocks at PI16 + 3 at PI15 + 12 tail = 2304 bits");

    // ── build a small multiplex: ensemble label, two services, their labels ──
    std::vector<std::vector<uint8_t>> fibs;
    {
        std::vector<uint8_t> p{ 0x00, 0xC1, 0x85 };
        auto l = label16("BBC National"); p.insert(p.end(), l.begin(), l.end());
        fibs.push_back(makeFib({{1, p}}));
    }
    {
        std::vector<uint8_t> p{ 0x02 };
        p.insert(p.end(), { 0xC2, 0x21, 0x01, 0x00, uint8_t((3 << 2) | 0x02) });
        p.insert(p.end(), { 0xC2, 0x22, 0x01, 0x3F, uint8_t((5 << 2) | 0x02) });
        fibs.push_back(makeFib({{0, p}}));
    }
    {
        std::vector<uint8_t> p{ 0x01, 0xC2, 0x21 };
        auto l = label16("BBC Radio 4"); p.insert(p.end(), l.begin(), l.end());
        fibs.push_back(makeFib({{1, p}}));
    }
    std::vector<uint8_t> ninetySix;
    for (auto& f : fibs) ninetySix.insert(ninetySix.end(), f.begin(), f.end());
    CHECK(ninetySix.size() == 96, "three FIBs are 96 bytes = 768 bits");

    auto tx = encodeFic(ninetySix);
    CHECK(tx.size() == size_t(kFicCodewordBits), "the encoder produces exactly one codeword");

    // ── clean channel ───────────────────────────────────────────────────────
    {
        std::vector<int8_t> soft(kFicCodewordBits);
        for (int i = 0; i < kFicCodewordBits; ++i) soft[i] = tx[i] ? -100 : 100;
        Ensemble e; Viterbi v;
        const int ok = ficDecodeCodeword(soft.data(), e, v);
        CHECK(ok == 3, "all three FIBs pass CRC on a clean channel");
        CHECK(e.label == "BBC National", "ensemble label recovered");
        CHECK(e.services.size() == 2, "two services recovered");
        CHECK(e.services[0xC221].label == "BBC Radio 4", "★ THE STATION NAME CAME BACK");
        CHECK(e.services[0xC221].components[0].scType == 0, "and its codec (MPEG Layer II)");
        CHECK(e.services[0xC222].components[0].scType == 63, "and the DAB+ one");
    }

    // ── ★★ A NOISY CHANNEL: this is what the FEC is for ─────────────────────
    {
        std::vector<int8_t> soft(kFicCodewordBits);
        for (int i = 0; i < kFicCodewordBits; ++i) {
            int v = (tx[i] ? -70 : 70) + int(rnd() % 61) - 30;
            if ((rnd() % 100) < 5) v = -v;                    // 5% hard errors
            soft[i] = int8_t(v > 127 ? 127 : v < -127 ? -127 : v);
        }
        Ensemble e; Viterbi vit;
        const int ok = ficDecodeCodeword(soft.data(), e, vit);
        char m[96]; snprintf(m, sizeof m, "5%% symbol errors still yields all 3 FIBs (%d)", ok);
        CHECK(ok == 3, m);
        CHECK(e.services[0xC221].label == "BBC Radio 4", "and the station name survives noise");
    }

    // ── the four-codeword multiplex, and its PHASE ──────────────────────────
    {
        std::vector<int8_t> frame(9216);
        // Put our codeword in slot 2, filler elsewhere, and check demux picks the right one.
        for (int r = 0; r < 4; ++r)
            for (int i = 0; i < kFicCodewordBits; ++i)
                frame[4 * i + r] = (r == 2) ? int8_t(tx[i] ? -100 : 100) : int8_t(int(rnd() % 255) - 127);
        std::vector<int8_t> cw(kFicCodewordBits);
        ficDemux(frame.data(), 2, cw.data());
        Ensemble e; Viterbi v;
        CHECK(ficDecodeCodeword(cw.data(), e, v) == 3, "demux slot 2 recovers the codeword");
        CHECK(e.services.size() == 2, "and the services with it");
        // ★ The WRONG phase must fail — that is what proves the demux is doing anything.
        ficDemux(frame.data(), 1, cw.data());
        Ensemble bad; Viterbi v2;
        CHECK(ficDecodeCodeword(cw.data(), bad, v2) == 0, "the wrong slot decodes nothing");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
