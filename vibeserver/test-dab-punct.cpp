// test-dab-punct.cpp — puncturing vectors, and that puncture->depuncture->Viterbi survives.
#include "vibe_dab_punct.h"
#include "vibe_dab_fec.h"
#include <cstdio>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)
static uint32_t seed = 7; static uint32_t rnd() { seed = seed*1664525u+1013904223u; return seed; }

int main() {
    printf("test-dab-punct\n");

    // ── the vectors match the spec's stated code rates ──────────────────────
    // ★★ THIS IS THE EXTRACTION CHECK. Table 13 labels each PI with rate 8/(8+PI); the mother code
    //    is rate 1/4, so 8 input bits produce 32 mother bits, and at rate 8/(8+PI) exactly (8+PI)
    //    of them must survive. If a digit were mis-extracted the count would not match its label.
    bool allMatch = true;
    for (int pi = 1; pi <= 24; ++pi) {
        if (keptPerBlock(pi) != 8 + pi) {
            printf("    PI %d keeps %d, expected %d\n", pi, keptPerBlock(pi), 8 + pi);
            allMatch = false;
        }
    }
    CHECK(allMatch, "every vector keeps exactly 8+PI of 32 bits, matching its stated code rate");

    // The first vector, spelled out against the spec's printed bits.
    CHECK(!punctured(1, 0) && !punctured(1, 1) && punctured(1, 2) && punctured(1, 3),
          "PI=1 begins 1100 as printed");
    CHECK(kPunctureVec[24] != 0, "PI=24 is present");

    // ── ★★★ THE REAL TEST: encode, puncture, depuncture, decode ─────────────
    for (int pi : { 8, 16, 24 }) {
        std::vector<uint8_t> msg(240);
        for (auto& b : msg) b = uint8_t(rnd() & 1);
        auto coded = convEncode(msg.data(), msg.size());

        // Puncture: keep only the bits the vector retains.
        std::vector<int8_t> tx;
        for (size_t i = 0; i < coded.size(); ++i)
            if (!punctured(pi, int(i % 32))) tx.push_back(int8_t(coded[i] ? -100 : 100));

        // Depuncture back onto the mother code and decode.
        std::vector<int8_t> soft(coded.size(), 0);
        const size_t w = depuncture(tx.data(), tx.size(), pi, soft.data(), soft.size());
        CHECK(w >= msg.size() * 4, "depuncturing restores the mother-code length");

        Viterbi v;
        auto got = v.decode(soft.data(), msg.size());
        char m[80]; snprintf(m, sizeof m, "PI=%d (rate 8/%d) round-trips through the decoder", pi, 8+pi);
        CHECK(got == msg, m);
    }

    // ── a bad index is refused rather than read out of bounds ───────────────
    { int8_t o[64]; int8_t r[8] = {0};
      CHECK(depuncture(r, 8, 0, o, 64) == 0, "PI 0 is not a valid index");
      CHECK(depuncture(r, 8, 25, o, 64) == 0, "PI 25 is out of range"); }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
