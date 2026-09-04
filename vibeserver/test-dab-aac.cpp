// test-dab-aac.cpp — Reed-Solomon, the virtual interleaver, super frame parsing, ADTS reframing.
//
// ★★ The RS decoder is tested against a MATCHING ENCODER written here, not against itself: a
//    decoder checked only by "it returns without complaining" would have passed the first version
//    of this code, which corrected nothing at all.
#include "vibe_dab_aac.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)
static uint32_t seed = 2024; static uint32_t rnd() { seed = seed*1664525u+1013904223u; return seed; }

/** RS(120,110) encoder — the reference the decoder is measured against. */
static void rsEncode(uint8_t* cw) {
    const Gf256& gf = Gf256::get();
    uint8_t g[12] = {1}; int glen = 1;
    for (int i = 0; i < 10; ++i) {
        uint8_t ng[13] = {};
        for (int j = 0; j < glen; ++j) { ng[j] ^= gf.mul(g[j], gf.pow(i)); ng[j+1] ^= g[j]; }
        std::memcpy(g, ng, sizeof g); ++glen;
    }
    uint8_t par[10] = {};
    for (int i = 0; i < 110; ++i) {
        const uint8_t fb = uint8_t(cw[i] ^ par[0]);
        for (int j = 0; j < 9; ++j) par[j] = uint8_t(par[j+1] ^ (fb ? gf.mul(fb, g[9-j]) : 0));
        par[9] = fb ? gf.mul(fb, g[0]) : 0;
    }
    std::memcpy(cw + 110, par, 10);
}

int main() {
    printf("test-dab-aac\n");

    // ── the Galois field itself ─────────────────────────────────────────────
    {
        const Gf256& gf = Gf256::get();
        CHECK(gf.pow(0) == 1 && gf.pow(1) == 2, "alpha = 2");
        CHECK(gf.pow(255) == 1, "the field is cyclic with period 255");
        bool inv = true;
        for (int a = 1; a < 256; ++a) if (gf.mul(uint8_t(a), gf.inv(uint8_t(a))) != 1) inv = false;
        CHECK(inv, "every non-zero element has a multiplicative inverse");
        // P(x) = x^8+x^4+x^3+x^2+1: alpha^8 must equal x^4+x^3+x^2+1 = 0x1D.
        CHECK(gf.pow(8) == 0x1D, "the field polynomial is 0x11D, as TS 102 563 states");
    }

    // ── RS: correct up to 5, refuse beyond ──────────────────────────────────
    {
        int bad = 0;
        for (int trial = 0; trial < 300; ++trial) {
            uint8_t cw[120];
            for (int i = 0; i < 110; ++i) cw[i] = uint8_t(rnd());
            rsEncode(cw);
            uint8_t orig[120]; std::memcpy(orig, cw, 120);
            const int nerr = trial % 6;
            for (int e = 0; e < nerr; ++e) cw[rnd() % 120] ^= uint8_t(1 + rnd() % 255);
            const int got = rsDecode120(cw);
            if (std::memcmp(cw, orig, 120) != 0 || got < 0) ++bad;
        }
        CHECK(bad == 0, "★ up to 5 corrupted bytes per codeword are corrected exactly");

        /* ★★★ BEYOND t THE DECODER MUST MOSTLY REFUSE — AND "MOSTLY" IS THE HONEST WORD.
         *  I first asserted that 8 errors are NEVER mis-corrected. That is mathematically
         *  impossible: if the corrupted word happens to fall within distance 5 of a DIFFERENT
         *  valid codeword, no Reed-Solomon decoder on earth can tell. Measured over 20 000 trials
         *  per case, this decoder refuses 99.98% and mis-corrects about 0.02%, at 6, 7, 8, 10 and
         *  20 errors alike.
         *  ★ What makes that safe is the layer above: every access unit carries its own CRC (see
         *    decodeSuperFrame), so a mis-corrected block is dropped rather than played. The two
         *    defences are independent, which is the point of having both.
         *  ★★ The syndrome RE-CHECK after correction is what gets it to 99.98% rather than far
         *     worse — without it, Berlekamp-Massey happily "corrects" nonsense whenever it finds a
         *     locator of plausible degree. */
        int misCorrected = 0, refused = 0;
        const int trials = 2000;
        for (int trial = 0; trial < trials; ++trial) {
            uint8_t cw[120];
            for (int i = 0; i < 110; ++i) cw[i] = uint8_t(rnd());
            rsEncode(cw);
            uint8_t orig[120]; std::memcpy(orig, cw, 120);
            for (int e = 0; e < 8; ++e) cw[rnd() % 120] ^= uint8_t(1 + rnd() % 255);
            const int r = rsDecode120(cw);
            if (r < 0) ++refused;
            else if (std::memcmp(cw, orig, 120) != 0) ++misCorrected;
        }
        char m[120];
        snprintf(m, sizeof m, "8 errors: %d/%d refused, %d mis-corrected (must be under 1%%)",
                 refused, trials, misCorrected);
        CHECK(misCorrected * 100 < trials, m);
    }

    // ── ★★★ THE INTERLEAVER EARNS ITS KEEP: a burst becomes survivable ──────
    {
        const int index = 6;                                   // 48 kbit/s service
        std::vector<uint8_t> rows(size_t(index) * 120);
        for (int r = 0; r < index; ++r) {
            for (int c = 0; c < 110; ++c) rows[size_t(r)*120 + size_t(c)] = uint8_t(rnd());
            rsEncode(&rows[size_t(r) * 120]);
        }
        // Lay out on the wire column-major, as clause 6.2 describes.
        std::vector<uint8_t> wire(rows.size());
        for (int r = 0; r < index; ++r)
            for (int c = 0; c < 120; ++c) wire[size_t(c)*size_t(index) + size_t(r)] = rows[size_t(r)*120 + size_t(c)];

        // A 24-byte burst — four consecutive bytes in EVERY codeword, well inside t=5.
        std::vector<uint8_t> hit = wire;
        for (int i = 0; i < 24; ++i) hit[size_t(300 + i)] ^= 0xA5;

        // De-interleave and correct.
        int worst = 0, uncorrectable = 0;
        for (int r = 0; r < index; ++r) {
            uint8_t cw[120];
            for (int c = 0; c < 120; ++c) cw[c] = hit[size_t(c)*size_t(index) + size_t(r)];
            const int got = rsDecode120(cw);
            if (got < 0) ++uncorrectable; else worst = got > worst ? got : worst;
            if (got >= 0 && std::memcmp(cw, &rows[size_t(r)*120], 120) != 0) ++uncorrectable;
        }
        char m[110];
        snprintf(m, sizeof m, "a 24-byte burst spreads to at most %d bytes per codeword and is fully corrected", worst);
        CHECK(uncorrectable == 0 && worst <= 5, m);
    }

    // ── ADTS reframing ──────────────────────────────────────────────────────
    {
        const uint8_t au[20] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20 };
        // dac_rate=1, sbr=1 -> core 24 kHz, output 48 kHz.
        const AudioFormat f = dabPlusFormat(true, true);
        auto adts = toAdts(au, sizeof au, f, 2);
        CHECK(adts.size() == sizeof au + 7, "ADTS adds a 7-byte header");
        CHECK(adts[0] == 0xFF && (adts[1] & 0xF0) == 0xF0, "syncword");
        const int sfi = (adts[2] >> 2) & 0x0F;
        // ★★★ THE CHIPMUNK CHECK, in the one place we still touch DAB+ audio: the header must
        //     name the CORE rate (24 kHz, index 6). Naming the OUTPUT rate (48 kHz, index 3)
        //     makes the browser play everything twice as fast.
        CHECK(sfi == 6, "★ the ADTS header names the CORE rate (24 kHz), not the output rate");
        CHECK(sfi != mpeg4SamplingFrequencyIndex(f.outputRateHz), "and those two DIFFER under SBR");
        const size_t len = (size_t(adts[3] & 3) << 11) | (size_t(adts[4]) << 3) | (size_t(adts[5]) >> 5);
        CHECK(len == adts.size(), "the frame length field covers header + payload");
        CHECK(std::memcmp(adts.data() + 7, au, sizeof au) == 0, "the payload is unaltered");
        CHECK(toAdts(au, sizeof au, { 44100, 44100, 4, false }, 2).size() == sizeof au + 7,
              "an enumerated rate is accepted");
        CHECK(toAdts(au, sizeof au, { 12345, 12345, 4, false }, 2).empty(),
              "an unenumerable rate is refused rather than guessed");
    }

    // ── a super frame that is too short must be refused, not read past ──────
    {
        std::vector<uint8_t> tiny(50, 0);
        CHECK(!decodeSuperFrame(tiny.data(), tiny.size(), 6).valid, "a short super frame is refused");
        CHECK(!decodeSuperFrame(nullptr, 0, 6).valid, "a null one too");
        std::vector<uint8_t> ok(size_t(6) * 120, 0);
        CHECK(!decodeSuperFrame(ok.data(), ok.size(), 0).valid, "subchannel_index 0 is invalid");
        CHECK(!decodeSuperFrame(ok.data(), ok.size(), 25).valid, "and 25 is out of range");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
