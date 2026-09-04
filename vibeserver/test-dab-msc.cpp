// test-dab-msc.cpp — CIF geometry, the time deinterleaver, and the EEP profiles.
#include "vibe_dab_msc.h"
#include "vibe_dab_fec.h"
#include <cstdio>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)
static uint32_t seed = 5150; static uint32_t rnd() { seed = seed*1664525u+1013904223u; return seed; }

int main() {
    printf("test-dab-msc\n");

    // ── geometry, straight from clause 4.9 ──────────────────────────────────
    CHECK(kCifBits == 55296 && kCifCus == 864, "a CIF is 55 296 bits in 864 CUs");
    CHECK(kCuBits == 64, "a capacity unit is 64 bits");
    CHECK(kCifsPerFrameI == 4, "Mode I carries 4 CIFs per 96 ms frame");

    // ── ★★★ THE DELAY TABLE IS THE 4-BIT BIT REVERSAL ───────────────────────
    //     Extracted from Figure 56 and cross-checked against the bit-reversal here, so a
    //     mis-read of the figure and a wrong assumption would have to agree to slip through.
    {
        bool ok = true;
        for (int i = 0; i < 16; ++i) {
            int rev = 0;
            for (int b = 0; b < 4; ++b) if (i & (1 << b)) rev |= 8 >> b;
            if (kTimeDelay[i] != rev) ok = false;
        }
        CHECK(ok, "the delays are the 4-bit bit reversal, as Figure 56 tabulates");
        CHECK(kTimeDelay[0] == 0 && kTimeDelay[1] == 8 && kTimeDelay[8] == 1 && kTimeDelay[15] == 15,
              "spot values off the figure: a(r,0), a(r-8,1), a(r-1,8)");
    }

    // ── the deinterleaver reassembles a frame that was interleaved ──────────
    {
        const size_t N = 256;
        // Transmit side: bit i of frame f leaves in frame f + D[i mod 16].
        std::vector<std::vector<int8_t>> frames(40, std::vector<int8_t>(N));
        for (size_t f = 0; f < frames.size(); ++f)
            for (size_t i = 0; i < N; ++i)
                frames[f][i] = int8_t((int(f) * 7 + int(i)) % 127);

        std::vector<std::vector<int8_t>> air(frames.size(), std::vector<int8_t>(N, 0));
        for (size_t f = 0; f < frames.size(); ++f)
            for (size_t i = 0; i < N; ++i) {
                const size_t at = f + size_t(kTimeDelay[i & 15]);
                if (at < air.size()) air[at][i] = frames[f][i];
            }

        TimeDeinterleaver di(N);
        bool ok = true, everReady = false;
        for (size_t f = 0; f < air.size(); ++f) {
            const auto& out = di.push(air[f].data());
            if (!di.ready()) continue;
            everReady = true;
            // Once full, the output should be logical frame (f - 15).
            const size_t want = f - 15;
            for (size_t i = 0; i < N; ++i) if (out[i] != frames[want][i]) ok = false;
        }
        CHECK(everReady, "the deinterleaver fills after 16 frames");
        CHECK(ok, "★ every deinterleaved frame matches the original logical frame");
    }

    // ── ★ the latency is REAL and must not be optimised away ────────────────
    {
        TimeDeinterleaver di(64);
        std::vector<int8_t> f(64, 1);
        for (int i = 0; i < 15; ++i) { di.push(f.data()); CHECK(!di.ready(), "not ready before 16"); }
        di.push(f.data());
        CHECK(di.ready(), "ready on the 16th frame — 15 CIFs of latency is the interleaving depth");
    }

    // ── EEP profiles against tables 18 and 20 ───────────────────────────────
    {
        // Set A, table 18: 8n kbit/s.
        auto a4 = eepProfile(4, false, 8);       // 64 kbit/s, n = 8
        CHECK(a4.L1 == 4*8-3 && a4.L2 == 2*8+3 && a4.PI1 == 3 && a4.PI2 == 2, "4-A: 4n-3 / 2n+3, PI 3/2");
        auto a3 = eepProfile(3, false, 8);
        CHECK(a3.L1 == 6*8-3 && a3.L2 == 3 && a3.PI1 == 8 && a3.PI2 == 7, "3-A: 6n-3 / 3, PI 8/7");
        auto a1 = eepProfile(1, false, 8);
        CHECK(a1.L1 == 6*8-3 && a1.L2 == 3 && a1.PI1 == 24 && a1.PI2 == 23, "1-A: 6n-3 / 3, PI 24/23");
        // ★ 2-A at n = 1 is the special case the table spells out; the general formula gives -1.
        auto s = eepProfile(2, false, 1);
        CHECK(s.L1 == 5 && s.L2 == 1 && s.PI1 == 13 && s.PI2 == 12, "2-A at 8 kbit/s is the special case");
        auto g = eepProfile(2, false, 4);
        CHECK(g.L1 == 2*4-3 && g.L2 == 4*4+3 && g.PI1 == 14 && g.PI2 == 13, "2-A general: 2n-3 / 4n+3");
        // Set B, table 20: every level is 24n-3 / 3.
        for (int lv = 1; lv <= 4; ++lv) {
            auto b = eepProfile(lv, true, 4);    // 128 kbit/s, n = 4
            char m[64]; snprintf(m, sizeof m, "%d-B is 24n-3 / 3", lv);
            CHECK(b.L1 == 24*4-3 && b.L2 == 3, m);
        }
        CHECK(!eepProfile(5, false, 4).valid && !eepProfile(2, false, 0).valid, "bad input is refused");
    }

    // ── ★★ the coded length must match the sub-channel's actual capacity ────
    //     A 64 kbit/s service at 3-A occupies a whole number of CUs; if our depuncturing produced
    //     a different length the frame would slip and every subsequent one would be garbage.
    {
        auto p = eepProfile(3, false, 8);        // 64 kbit/s
        const int coded = eepCodedBits(p);
        CHECK(coded % kCuBits == 0, "the coded frame is a whole number of capacity units");
        /* ★★ DERIVED, NOT ASSERTED FROM MEMORY. I first wrote 84 here from nothing and the test
         *  correctly disagreed. Table 16 gives the average code rate for 64 kbit/s at P=3 as 0,50,
         *  and a 24 ms logical frame at 64 kbit/s is 1536 bits — so the coded frame is 3072 bits,
         *  which is 48 CU. Computing it from the spec's own code rate makes this a real check
         *  rather than a number that agrees with whatever the code happens to produce. */
        const int dataBitsPerFrame = 64 * 24;              // kbit/s x ms = bits
        CHECK(coded == dataBitsPerFrame * 2, "3-A is rate 1/2, so the coded frame is twice the data");
        CHECK(coded / kCuBits == 48, "64 kbit/s at 3-A occupies 48 CU");
    }

    // ── depuncture -> Viterbi round trip on a real EEP profile ──────────────
    {
        auto p = eepProfile(3, false, 8);
        const size_t dataBits = 8 * 8 * 24;      // 64 kbit/s x 24 ms = 1536 bits
        std::vector<uint8_t> msg(dataBits);
        for (auto& b : msg) b = uint8_t(rnd() & 1);
        auto mother = convEncode(msg.data(), msg.size());

        // Transmit: puncture per the profile.
        std::vector<int8_t> tx;
        size_t w = 0;
        auto emit = [&](int count, int pi) {
            for (int b = 0; b < count; ++b) for (int sub = 0; sub < 4; ++sub)
                for (int i = 0; i < 32; ++i, ++w)
                    if (!punctured(pi, i) && w < mother.size()) tx.push_back(int8_t(mother[w] ? -100 : 100));
        };
        emit(p.L1, p.PI1); emit(p.L2, p.PI2);
        for (int i = 0; i < 24 && w < mother.size(); ++i, ++w)
            if (!tailPunctured(i)) tx.push_back(int8_t(mother[w] ? -100 : 100));

        std::vector<int8_t> soft(mother.size(), 0);
        eepDepuncture(tx.data(), tx.size(), p, soft.data(), soft.size());
        Viterbi v;
        auto got = v.decode(soft.data(), dataBits);
        CHECK(got == msg, "★ a 64 kbit/s 3-A logical frame round-trips through the real profile");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
