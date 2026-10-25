// test-ft8-fast — the FT8/FT4 decoder's fast kernels give the SAME answers as ft8_lib's originals (2026-10-25).
//
// ★★★ WHY. Stuart (2026-10-25): "take a look at our Digital Spots decoder and see if we can improve on its CPU usage …
//     it also might mean the difference between enabling a higher depth on a lower end system." The adaptive gate
//     (Ft8Decoder::passAllowed) grants a second and third pass only on spare CPU, so every millisecond taken off a
//     pass is depth a Pi 2 can now afford. But a decoder that got cheaper by decoding differently has not got cheaper,
//     it has changed — so each kernel that is pure re-arrangement is held here to its original, BIT FOR BIT, on the
//     path this build takes (NEON on ARM, SSE2 on x86, plain C under -DVIBE_FORCE_SCALAR).
//   1. belief propagation: bp_decode vs bp_decode_ref (ft8_lib's), on real (174,91) codewords through noise from
//      hopeless to clean — the same `plain` and the same error count, every time.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "ft8/constants.h"
#include "ft8/ldpc.h"
}

static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

/** A random message's codeword (systematic: 91 message+CRC bits, then 83 parity bits from the generator). */
static void codeword(std::mt19937& rng, uint8_t cw[FTX_LDPC_N]) {
    for (int k = 0; k < FTX_LDPC_K; ++k) cw[k] = (uint8_t)(rng() & 1);
    for (int m = 0; m < FTX_LDPC_M; ++m) {
        int p = 0;
        for (int k = 0; k < FTX_LDPC_K; ++k) if (kFTX_LDPC_generator[m][k / 8] & (0x80 >> (k % 8))) p ^= cw[k];
        cw[FTX_LDPC_K + m] = (uint8_t)p;
    }
}

static void testBp() {
    std::printf("\n1. belief propagation — bp_decode vs ft8_lib's bp_decode_ref\n");
    std::mt19937 rng(20261025);
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    int same = 0, n = 0, decoded = 0, iterSet[] = { 1, 2, 5, 25, 50 };
    for (int t = 0; t < 6000; ++t) {
        uint8_t cw[FTX_LDPC_N]; codeword(rng, cw);
        const float amp = 0.2f + 2.6f * u(rng);          // from hopeless to clean
        float llr[FTX_LDPC_N];
        for (int i = 0; i < FTX_LDPC_N; ++i) llr[i] = 3.0f * ((cw[i] ? amp : -amp) + g(rng));
        if (t % 50 == 0) for (int i = 0; i < FTX_LDPC_N; ++i) llr[i] = (float)(i % 3) - 1.0f;   // zeros and ties
        const int iters = iterSet[t % 5];
        float a[FTX_LDPC_N], b[FTX_LDPC_N];
        std::memcpy(a, llr, sizeof a); std::memcpy(b, llr, sizeof b);
        uint8_t pa[FTX_LDPC_N], pb[FTX_LDPC_N]; int ea = -1, eb = -2;
        bp_decode_ref(a, iters, pa, &ea);
        bp_decode(b, iters, pb, &eb);
        ++n;
        if (ea == eb && std::memcmp(pa, pb, sizeof pa) == 0 && std::memcmp(a, b, sizeof a) == 0) ++same;
        else if (n - same <= 3) std::printf("     differ: case %d (iters %d): errors %d vs %d\n", t, iters, ea, eb);
        decoded += ea == 0;
    }
    ok(same == n, "identical plain bits and error counts: " + std::to_string(same) + " / " + std::to_string(n));
    ok(decoded > n / 4 && decoded < n * 9 / 10, "the cases span failures and successes (" + std::to_string(decoded) + " decoded)");
}

int main() {
    std::printf("test-ft8-fast — the fast FT8 kernels against ft8_lib's originals\n");
    testBp();
    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
