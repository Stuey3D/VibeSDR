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
//   2. OSD-1: ft8Osd1 (eliminates in codeword order, popcount first) vs the first version (permuted columns, kept
//      below as osd1Ref) — the same codeword and the same error count, including the near-misses BP gave up on.
//   3. the sync search: ftx_find_candidates (sixteen frequencies per vector) vs ftx_find_candidates_ref (ft8_lib's,
//      one at a time), FT8 and FT4, on waterfalls of noise with Costas arrays planted in them, several minimum
//      scores and heap sizes — the same candidates, in the same order, with the same scores.
//   4. the spectrogram through PFFFT vs Kiss FFT. NOT bit-identical (a different FFT rounds differently) — so the test
//      states how close: on a band of noise and GFSK-like tones, a waterfall byte (0.5 dB step) may differ by one step,
//      never more, and in under 1 in 10,000 bins.
//   5. the waterfall level by table vs by log10f — every one of the 255 step edges (and a float either side) and
//      a million random levels from -130 to +60 dB: identical.
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
#include "ft8/crc.h"
#include "ft8/decode.h"
#include "common/monitor.h"
}
#include "decoders/ft8_decoder.h"
#include <algorithm>

// ── the first OSD-1 (2026-10-05), verbatim but for its name: the reference ft8Osd1 is held to ──
namespace {
struct Bits174 { uint64_t w[3]; };
inline void setBit(Bits174& b, int i) { b.w[i >> 6] |= 1ull << (i & 63); }
inline bool getBit(const Bits174& b, int i) { return (b.w[i >> 6] >> (i & 63)) & 1; }
const Bits174* refGeneratorRows() {               // row k = the codeword of message bit k alone
    static Bits174 g[FTX_LDPC_K];
    static bool init = [] {
        for (int k = 0; k < FTX_LDPC_K; ++k) {
            g[k] = Bits174{{0, 0, 0}};
            setBit(g[k], k);
            for (int m = 0; m < FTX_LDPC_M; ++m)
                if (kFTX_LDPC_generator[m][k / 8] & (0x80u >> (k % 8))) setBit(g[k], FTX_LDPC_K + m);
        }
        return true;
    }();
    (void)init;
    return g;
}
/** OSD-1 on `llr` (log p(1)/p(0), codeword order). On success fills `a91` (packed message + CRC) and returns the
 *  number of hard-decision disagreements; -1 when no codeword passes the CRC within `maxErrors`. */
int osd1Ref(const float* llr, int maxErrors, uint8_t a91[FTX_LDPC_K_BYTES]) {
    const Bits174* g = refGeneratorRows();
    int perm[FTX_LDPC_N];
    for (int i = 0; i < FTX_LDPC_N; ++i) perm[i] = i;
    std::sort(perm, perm + FTX_LDPC_N, [&](int a, int b) { return std::fabs(llr[a]) > std::fabs(llr[b]); });
    int pos[FTX_LDPC_N];
    for (int p = 0; p < FTX_LDPC_N; ++p) pos[perm[p]] = p;
    Bits174 M[FTX_LDPC_K];
    for (int k = 0; k < FTX_LDPC_K; ++k) {
        M[k] = Bits174{{0, 0, 0}};
        for (int j = 0; j < FTX_LDPC_N; ++j) if (getBit(g[k], j)) setBit(M[k], pos[j]);
    }
    int pivot[FTX_LDPC_K]; int r = 0;
    for (int p = 0; p < FTX_LDPC_N && r < FTX_LDPC_K; ++p) {
        int i = r;
        while (i < FTX_LDPC_K && !getBit(M[i], p)) ++i;
        if (i == FTX_LDPC_K) continue;
        std::swap(M[i], M[r]);
        for (int q = 0; q < FTX_LDPC_K; ++q)
            if (q != r && getBit(M[q], p)) { M[q].w[0] ^= M[r].w[0]; M[q].w[1] ^= M[r].w[1]; M[q].w[2] ^= M[r].w[2]; }
        pivot[r++] = p;
    }
    if (r < FTX_LDPC_K) return -1;
    Bits174 hard{{0, 0, 0}};
    float w[FTX_LDPC_N];
    for (int p = 0; p < FTX_LDPC_N; ++p) { if (llr[perm[p]] > 0) setBit(hard, p); w[p] = std::fabs(llr[perm[p]]); }
    Bits174 c0{{0, 0, 0}};
    for (int q = 0; q < FTX_LDPC_K; ++q)
        if (getBit(hard, pivot[q])) { c0.w[0] ^= M[q].w[0]; c0.w[1] ^= M[q].w[1]; c0.w[2] ^= M[q].w[2]; }
    float bestD = 1e30f; int bestErr = -1;
    for (int f = -1; f < FTX_LDPC_K; ++f) {
        Bits174 c = c0;
        if (f >= 0) { c.w[0] ^= M[f].w[0]; c.w[1] ^= M[f].w[1]; c.w[2] ^= M[f].w[2]; }
        float d = 0; int err = 0;
        for (int x = 0; x < 3; ++x) {
            uint64_t diff = c.w[x] ^ hard.w[x];
            while (diff) { const int b = __builtin_ctzll(diff); d += w[x * 64 + b]; ++err; diff &= diff - 1; }
        }
        if (err > maxErrors || d >= bestD) continue;
        uint8_t m91[FTX_LDPC_K_BYTES] = {};
        for (int k = 0; k < FTX_LDPC_K; ++k) if (getBit(c, pos[k])) m91[k / 8] |= (uint8_t)(0x80u >> (k % 8));
        const uint16_t crcRx = ftx_extract_crc(m91);
        uint8_t t[FTX_LDPC_K_BYTES]; std::memcpy(t, m91, sizeof t);
        t[9] &= 0xF8; t[10] = 0; t[11] = 0;
        if (ftx_compute_crc(t, 96 - 14) != crcRx) continue;
        bestD = d; bestErr = err; std::memcpy(a91, m91, FTX_LDPC_K_BYTES);
    }
    return bestErr;
}

}


static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

/** A random message's codeword (systematic: 77 message bits + their CRC-14, then 83 parity bits from the generator). */
static void codeword(std::mt19937& rng, uint8_t cw[FTX_LDPC_N]) {
    uint8_t payload[10] = {}, a91[12] = {};
    for (int b = 0; b < 10; ++b) payload[b] = (uint8_t)rng();
    payload[9] &= 0xF8;                                   // 77 bits
    ftx_add_crc(payload, a91);
    for (int k = 0; k < FTX_LDPC_K; ++k) cw[k] = (uint8_t)((a91[k / 8] >> (7 - k % 8)) & 1);
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

static void testOsd() {
    std::printf("\n2. OSD-1 — ft8Osd1 vs the first version\n");
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    int same = 0, n = 0, found = 0, bpFailed = 0;
    for (int t = 0; t < 3000; ++t) {
        uint8_t cw[FTX_LDPC_N]; codeword(rng, cw);
        const float amp = 0.3f + 1.2f * u(rng);          // where OSD is asked: BP mostly fails here
        float llr[FTX_LDPC_N];
        for (int i = 0; i < FTX_LDPC_N; ++i) llr[i] = 3.0f * ((cw[i] ? amp : -amp) + g(rng));
        if (t % 40 == 0) for (int i = 0; i < FTX_LDPC_N; i += 2) llr[i] = llr[i + 1];          // ties in |LLR|
        float b[FTX_LDPC_N]; std::memcpy(b, llr, sizeof b); uint8_t plain[FTX_LDPC_N]; int e = 0;
        bp_decode(b, 25, plain, &e);
        bpFailed += e > 0;
        const int maxErr = t % 3 == 0 ? 40 : 24;
        uint8_t a1[12] = {}, a2[12] = {};
        const int r1 = osd1Ref(llr, maxErr, a1), r2 = vibe::ft8Osd1(llr, maxErr, a2);
        ++n;
        if (r1 == r2 && (r1 < 0 || std::memcmp(a1, a2, sizeof a1) == 0)) ++same;
        else if (n - same <= 3) std::printf("     differ: case %d: %d vs %d\n", t, r1, r2);
        found += r1 >= 0;
    }
    ok(same == n, "identical codeword and error count: " + std::to_string(same) + " / " + std::to_string(n));
    ok(found > 100 && bpFailed > 300, "the cases include OSD successes (" + std::to_string(found) + ") and BP failures ("
       + std::to_string(bpFailed) + ")");
}

static void testSearch() {
    std::printf("\n3. sync search — ftx_find_candidates vs ft8_lib's ftx_find_candidates_ref\n");
    std::mt19937 rng(42);
    int same = 0, n = 0; long found = 0;
    for (int ft4 = 0; ft4 < 2; ++ft4) {
        monitor_config_t cfg = {};
        cfg.f_min = 100; cfg.f_max = 3500; cfg.sample_rate = 12000; cfg.time_osr = 2; cfg.freq_osr = 2;
        cfg.protocol = ft4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
        monitor_t mon; monitor_init(&mon, &cfg);
        ftx_waterfall_t& wf = mon.wf;
        for (int t = 0; t < 12; ++t) {
            wf.num_blocks = t % 4 == 3 ? wf.max_blocks - 9 : wf.max_blocks;      // a short window too
            const size_t sz = (size_t)wf.max_blocks * wf.block_stride;
            for (size_t i = 0; i < sz; ++i) wf.mag[i] = (uint8_t)(std::min(255.0, std::max(0.0, 80 + 12.0 * std::normal_distribution<double>()(rng))));
            for (int sig = 0; sig < 30; ++sig) {                                      // Costas arrays to find
                const int ts = (int)(rng() % 2), fs = (int)(rng() % 2), f = (int)(rng() % (unsigned)(wf.num_bins - 8));
                const int t0 = (int)(rng() % 25) - 8, lift = 10 + (int)(rng() % 120);
                const int nsync = ft4 ? FT4_NUM_SYNC : FT8_NUM_SYNC, len = ft4 ? FT4_LENGTH_SYNC : FT8_LENGTH_SYNC;
                for (int m = 0; m < nsync; ++m)
                    for (int k = 0; k < len; ++k) {
                        const int b = t0 + (ft4 ? 1 + FT4_SYNC_OFFSET * m + k : FT8_SYNC_OFFSET * m + k);
                        if (b < 0 || b >= wf.num_blocks) continue;
                        const int tone = ft4 ? kFT4_Costas_pattern[m][k] : kFT8_Costas_pattern[k];
                        uint8_t& v = wf.mag[(size_t)b * wf.block_stride + ((size_t)ts * wf.freq_osr + fs) * wf.num_bins + f + tone];
                        v = (uint8_t)std::min(255, v + lift);
                    }
            }
            const int minScore = t % 3 == 0 ? 10 : t % 3 == 1 ? 0 : -3, cap = t % 2 ? 140 : 300;
            std::vector<ftx_candidate_t> a((size_t)cap), b((size_t)cap);
            const int na = ftx_find_candidates_ref(&wf, cap, a.data(), minScore);
            const int nb = ftx_find_candidates(&wf, cap, b.data(), minScore);
            bool eq = na == nb;
            for (int i = 0; eq && i < na; ++i)
                eq = a[(size_t)i].score == b[(size_t)i].score && a[(size_t)i].time_offset == b[(size_t)i].time_offset
                  && a[(size_t)i].freq_offset == b[(size_t)i].freq_offset && a[(size_t)i].time_sub == b[(size_t)i].time_sub
                  && a[(size_t)i].freq_sub == b[(size_t)i].freq_sub;
            ++n; same += eq; found += na;
            if (!eq) std::printf("     differ: %s case %d: %d vs %d candidates\n", ft4 ? "FT4" : "FT8", t, na, nb);
        }
        monitor_free(&mon);
    }
    ok(same == n, "identical candidate lists: " + std::to_string(same) + " / " + std::to_string(n) + " (" + std::to_string(found) + " candidates)");
}

static void testWaterfall() {
    std::printf("\n4. spectrogram — PFFFT vs Kiss FFT (not bit-identical: how close)\n");
    for (int ft4 = 0; ft4 < 2; ++ft4) {
        monitor_config_t cfg = {};
        cfg.f_min = 100; cfg.f_max = 3500; cfg.sample_rate = 12000; cfg.time_osr = 2; cfg.freq_osr = 2;
        cfg.protocol = ft4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
        monitor_t a, b; monitor_init(&a, &cfg); monitor_init(&b, &cfg);
        ok(a.pf_setup != nullptr, std::string(ft4 ? "FT4" : "FT8") + ": PFFFT takes nfft " + std::to_string(a.nfft));
        void* const bSetup = b.pf_setup;
        b.pf_setup = nullptr;                                            // b = Kiss FFT, as before
        std::mt19937 rng(ft4 ? 5 : 6);
        std::normal_distribution<float> g(0.0f, 0.02f);
        const int n = a.wf.max_blocks * a.block_size;
        std::vector<float> x((size_t)n);
        for (int i = 0; i < n; ++i) {
            float v = g(rng);
            for (int s = 0; s < 25; ++s) v += 0.01f * (float)(s % 5 + 1) * std::sin(2.0f * 3.14159265f * (300.0f + 123.4f * s + 6.25f * (float)((i / a.block_size + s) % 8)) * i / 12000.0f);
            x[(size_t)i] = v;
        }
        for (int pos = 0; pos + a.block_size <= n; pos += a.block_size) { monitor_process(&a, &x[(size_t)pos]); monitor_process(&b, &x[(size_t)pos]); }
        const size_t sz = (size_t)a.wf.num_blocks * a.wf.block_stride;
        long diff = 0; int worst = 0;
        for (size_t i = 0; i < sz; ++i) { const int d = std::abs((int)a.wf.mag[i] - (int)b.wf.mag[i]); diff += d != 0; worst = std::max(worst, d); }
        char line[160];
        std::snprintf(line, sizeof line, "%s: %ld of %zu waterfall bytes differ (%.1e), by at most %d step", ft4 ? "FT4" : "FT8", diff, sz, (double)diff / (double)sz, worst);
        ok(worst <= 1 && (double)diff / (double)sz < 1e-4, line);
        monitor_free(&a); b.pf_setup = bSetup; monitor_free(&b);
    }
}

static void testLevel() {
    std::printf("\n5. waterfall level — table vs log10f\n");
    monitor_config_t cfg = {};
    cfg.f_min = 100; cfg.f_max = 3500; cfg.sample_rate = 12000; cfg.time_osr = 2; cfg.freq_osr = 2; cfg.protocol = FTX_PROTOCOL_FT8;
    monitor_t m; monitor_init(&m, &cfg);
    long n = 0, same = 0;
    auto chk = [&](float x) { ++n; same += monitor_level(&m, x) == monitor_level_ref(x); };
    for (int k = 1; k < 256; ++k) {
        const float t = m.wf_thr[k];
        if (!std::isfinite(t)) continue;
        chk(t); chk(std::nextafter(t, 0.0f)); chk(std::nextafter(t, INFINITY));
    }
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-13.0f, 6.0f);
    for (int i = 0; i < 1000000; ++i) chk(1e-12f + std::pow(10.0f, u(rng)));
    ok(same == n, "identical levels: " + std::to_string(same) + " / " + std::to_string(n));
    monitor_free(&m);
}

int main() {
    std::printf("test-ft8-fast — the fast FT8 kernels against ft8_lib's originals\n");
    testBp();
    testOsd();
    testSearch();
    testWaterfall();
    testLevel();
    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
