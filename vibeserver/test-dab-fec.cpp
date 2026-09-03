// test-dab-fec.cpp — the rate-1/4 mother code, soft Viterbi, energy dispersal, FIB CRC.
#include "vibe_dab_fec.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

static uint32_t seed = 4242;
static uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed; }

/** Soft values from hard bits: +100 for a 0, -100 for a 1 (the dqpskSoft convention). */
static std::vector<int8_t> toSoft(const std::vector<uint8_t>& bits, int level = 100) {
    std::vector<int8_t> s(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) s[i] = int8_t(bits[i] ? -level : level);
    return s;
}

int main() {
    printf("test-dab-fec\n");

    // ── the polynomials are the spec's ───────────────────────────────────────
    CHECK(kPoly[0] == 0133 && kPoly[1] == 0171 && kPoly[2] == 0145 && kPoly[3] == 0133,
          "generator polynomials 133, 171, 145, 133 octal (EN 300 401 11.1.1)");
    CHECK(kK == 7 && kStates == 64, "constraint length 7, 64 states");

    // ★★ A KNOWN ENCODER OUTPUT, not just self-consistency. From the all-zero state a single 1
    //    puts the register at 0b1000000: the four outputs are the parities of 133,171,145,133
    //    against bit 6 alone, and 133/171/145/133 octal all have bit 6 set (0100 in the high
    //    digit), so every output is 1. If the register order were reversed this would be 0.
    CHECK(branchOutputs(0, 1) == 0b1111, "a lone 1 from the zero state sets all four outputs");
    CHECK(branchOutputs(0, 0) == 0, "a zero from the zero state outputs zeros");

    // ── encode -> decode, clean ──────────────────────────────────────────────
    {
        std::vector<uint8_t> msg(200);
        for (auto& b : msg) b = uint8_t(rnd() & 1);
        auto coded = convEncode(msg.data(), msg.size());
        CHECK(coded.size() == (msg.size() + kK - 1) * kRateOut, "coded length = (n+6)*4");
        auto soft = toSoft(coded);
        Viterbi v;
        auto got = v.decode(soft.data(), msg.size());
        CHECK(got == msg, "clean encode/decode round-trips exactly");
    }

    // ── ★★ SOFT DECISIONS EARN THEIR KEEP: heavy noise, still recovered ──────
    {
        std::vector<uint8_t> msg(300);
        for (auto& b : msg) b = uint8_t(rnd() & 1);
        auto coded = convEncode(msg.data(), msg.size());
        auto soft = toSoft(coded, 60);
        // Add noise to every symbol, and FLIP the sign of 8% of them outright.
        for (auto& s : soft) {
            int v = int(s) + (int(rnd() % 81) - 40);
            if ((rnd() % 100) < 8) v = -v;
            s = int8_t(v > 127 ? 127 : v < -127 ? -127 : v);
        }
        Viterbi vit;
        auto got = vit.decode(soft.data(), msg.size());
        size_t bad = 0;
        for (size_t i = 0; i < msg.size(); ++i) if (got[i] != msg[i]) ++bad;
        char m[80]; snprintf(m, sizeof m, "8%% flipped symbols still decode (%zu bit errors)", bad);
        CHECK(bad == 0, m);
    }

    // ── ★★★ PUNCTURING NEEDS NO SPECIAL CASE: a zero soft value is "no opinion" ──
    {
        std::vector<uint8_t> msg(150);
        for (auto& b : msg) b = uint8_t(rnd() & 1);
        auto coded = convEncode(msg.data(), msg.size());
        auto soft = toSoft(coded);
        // Puncture every 4th coded bit — rate 1/4 becomes effectively 1/3.
        for (size_t i = 3; i < soft.size(); i += 4) soft[i] = 0;
        Viterbi vit;
        auto got = vit.decode(soft.data(), msg.size());
        CHECK(got == msg, "a quarter of the code bits erased still decodes");
    }

    // ── energy dispersal is its own inverse, and is NOT the identity ─────────
    {
        std::vector<uint8_t> a(400);
        for (auto& b : a) b = uint8_t(rnd() & 1);
        auto orig = a;
        EnergyDispersal e; e.apply(a.data(), a.size());
        size_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i) if (a[i] != orig[i]) ++diff;
        CHECK(diff > a.size() / 4, "the scrambler actually scrambles");
        EnergyDispersal e2; e2.apply(a.data(), a.size());
        CHECK(a == orig, "applying it twice restores the original");
        // ★ The sequence must RESET per FIB, or the second one descrambles with the wrong phase.
        EnergyDispersal p; std::vector<uint8_t> z1(20, 0), z2(20, 0);
        p.apply(z1.data(), z1.size());
        p.reset(); p.apply(z2.data(), z2.size());
        CHECK(z1 == z2, "reset() returns the register to its preset");
    }

    // ── FIB CRC ─────────────────────────────────────────────────────────────
    {
        uint8_t fib[32];
        for (int i = 0; i < 30; ++i) fib[i] = uint8_t(rnd() & 0xFF);
        const uint16_t c = uint16_t(~crc16(fib, 30));
        fib[30] = uint8_t(c >> 8); fib[31] = uint8_t(c & 0xFF);
        CHECK(fibCrcOk(fib), "a correctly stamped FIB passes");
        fib[7] ^= 0x01;
        CHECK(!fibCrcOk(fib), "a single flipped bit fails the CRC");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
