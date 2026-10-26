// VibeSDR V12 — JS8's LDPC(174,87) + CRC-12. See js8_ldpc.h.
#include "js8_ldpc.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace vibe {
namespace js8 {
namespace {

/* The parity checks: 87 rows of codeword-bit indices (0-origin, on-air order), 255 = unused. From ft8_lib's early
 * history (MIT) — see the header. */
const uint8_t kNm[kM][7] = {
    {  0,  29,  59,  88, 117, 146, 255}, {  1,  30,  60,  89, 118, 146, 255}, {  2,  31,  61,  90, 119, 147, 255}, {  3,  32,  62,  91, 120, 148, 255},
    {  1,  33,  63,  92, 121, 149, 255}, {  4,  32,  64,  93, 122, 147, 255}, {  5,  33,  65,  94, 123, 150, 255}, {  6,  34,  66,  95, 119, 151, 255},
    {  7,  35,  67,  96, 124, 152, 255}, {  8,  36,  68,  97, 125, 151, 255}, {  9,  37,  69,  98, 126, 153, 255}, { 10,  38,  70,  99, 125, 154, 255},
    { 11,  39,  60, 100, 127, 144, 255}, {  9,  32,  59,  94, 127, 155, 255}, { 12,  40,  71,  96, 125, 156, 255}, { 12,  41,  72,  89, 128, 155, 255},
    { 13,  38,  73,  98, 129, 157, 255}, { 14,  42,  74, 101, 130, 158, 255}, { 15,  42,  70, 102, 117, 159, 255}, { 16,  43,  75,  97, 129, 155, 255},
    { 17,  44,  59,  95, 131, 160, 255}, { 18,  45,  72,  82, 132, 161, 255}, { 11,  37,  76, 101, 133, 162, 255}, { 18,  46,  77, 103, 134, 146, 255},
    {  0,  31,  76, 104, 135, 163, 255}, { 19,  47,  72, 105, 122, 162, 255}, { 20,  40,  78, 106, 136, 164, 255}, { 21,  41,  65, 107, 137, 151, 255},
    { 17,  41,  79, 108, 138, 153, 255}, { 22,  48,  80, 109, 134, 165, 255}, { 15,  49,  81,  90, 128, 157, 255}, {  2,  47,  62, 106, 123, 166, 255},
    {  5,  50,  66, 110, 133, 154, 255}, { 23,  34,  76,  99, 121, 161, 255}, { 19,  44,  75, 111, 139, 156, 255}, { 20,  35,  63,  91, 129, 158, 255},
    {  7,  51,  82, 110, 117, 165, 255}, { 20,  52,  83, 112, 137, 167, 255}, { 24,  50,  78,  88, 121, 157, 255}, { 21,  43,  74, 106, 132, 154, 171},
    {  8,  53,  83,  89, 140, 168, 255}, { 21,  53,  84, 109, 135, 160, 255}, {  7,  36,  64, 101, 128, 169, 255}, { 18,  38,  84, 113, 138, 149, 255},
    { 25,  54,  70,  92, 141, 166, 255}, { 26,  55,  64,  95, 132, 159, 173}, { 27,  30,  85,  99, 116, 170, 255}, { 27,  51,  69, 103, 131, 143, 255},
    { 23,  56,  67,  94, 136, 141, 255}, {  6,  29,  71, 109, 142, 150, 255}, {  3,  50,  75, 114, 126, 167, 255}, { 15,  44,  86, 113, 124, 171, 255},
    { 14,  29,  85, 114, 122, 149, 255}, { 22,  45,  63,  90, 143, 172, 255}, { 22,  34,  74, 112, 144, 152, 255}, { 13,  40,  86, 107, 116, 148, 169},
    { 24,  39,  84,  93, 123, 158, 255}, { 24,  57,  68, 115, 142, 173, 255}, { 28,  42,  60, 115, 131, 161, 255}, { 14,  57,  87, 111, 120, 163, 255},
    {  3,  58,  71, 113, 118, 162, 172}, { 26,  46,  85,  97, 133, 152, 255}, {  4,  43,  77, 108, 140, 255, 255}, {  9,  45,  68, 102, 135, 164, 255},
    {  8,  49,  58,  92, 127, 163, 255}, { 13,  56,  57, 108, 119, 165, 255}, { 16,  54,  61, 115, 124, 153, 255}, {  2,  53,  69, 100, 139, 169, 255},
    {  0,  35,  81, 107, 126, 173, 255}, {  4,  52,  80, 104, 139, 255, 255}, { 28,  52,  66,  98, 141, 172, 255}, { 17,  48,  73,  96, 114, 166, 255},
    {  1,  56,  62, 102, 137, 156, 255}, { 25,  37,  78, 111, 134, 170, 255}, { 10,  51,  65,  87, 118, 147, 255}, { 19,  39,  67, 116, 140, 159, 255},
    { 10,  47,  80,  88, 145, 168, 255}, { 28,  46,  79,  91, 145, 171, 255}, {  5,  31,  86, 103, 144, 168, 255}, { 26,  33,  73, 105, 130, 164, 255},
    { 11,  55,  83,  87, 138, 255, 255}, { 12,  55,  61, 110, 145, 170, 255}, { 25,  36,  79, 104, 143, 150, 255}, { 16,  30,  81, 112, 120, 160, 255},
    { 27,  48,  58,  93, 136, 255, 255}, {  6,  54,  82, 100, 130, 167, 255}, { 23,  49,  77, 105, 142, 148, 255},
};

/* ★ Everything else is built from kNm once, at first use. */
struct Tables {
    uint8_t deg[kM];                 // bits per check
    int16_t chkEdge0[kM + 1];        // check m's edges are chkEdge0[m] .. chkEdge0[m+1]-1
    int16_t edgeVar[kM * 7];         // the bit each edge joins
    int16_t varEdge[kN][3];          // each bit's three edges
    uint64_t gen[kK][3];             // row k: the codeword of information bit k alone (on-air order, 174 bits)
    bool ok = false;
};

inline void setb(uint64_t* w, int i) { w[i >> 6] |= 1ull << (i & 63); }
inline int getb(const uint64_t* w, int i) { return (int)((w[i >> 6] >> (i & 63)) & 1); }

const Tables& tables() {
    static Tables t;
    static bool init = [] {
        int e = 0; int vc[kN] = {};
        for (int m = 0; m < kM; ++m) {
            t.chkEdge0[m] = (int16_t)e;
            int d = 0;
            for (int j = 0; j < 7; ++j) {
                const int v = kNm[m][j];
                if (v == 255) break;
                t.edgeVar[e] = (int16_t)v;
                if (vc[v] < 3) t.varEdge[v][vc[v]] = (int16_t)e;
                ++vc[v]; ++e; ++d;
            }
            t.deg[m] = (uint8_t)d;
        }
        t.chkEdge0[kM] = (int16_t)e;
        bool ok = true;
        for (int v = 0; v < kN; ++v) ok &= vc[v] == 3;
        /* ★ THE GENERATOR, BY ELIMINATION. H = [Hp | Hi] (parity columns 0..86, information 87..173); a codeword
         *  has Hp·p = Hi·i, so p = Hp⁻¹·Hi·i. Gauss-Jordan on [Hp | Hi] over GF(2): when Hp has become the identity,
         *  the right half's column k is the parity of information bit k. Hp must be invertible — it is (the test
         *  checks every generated codeword against every check). */
        uint64_t row[kM][3] = {};
        for (int m = 0; m < kM; ++m)
            for (int j = 0; j < t.deg[m]; ++j) row[m][kNm[m][j] >> 6] ^= 1ull << (kNm[m][j] & 63);
        for (int c = 0; c < kM && ok; ++c) {
            int p = -1;
            for (int r = c; r < kM; ++r) if (getb(row[r], c)) { p = r; break; }
            if (p < 0) { ok = false; break; }
            if (p != c) for (int w = 0; w < 3; ++w) std::swap(row[p][w], row[c][w]);
            for (int r = 0; r < kM; ++r)
                if (r != c && getb(row[r], c)) for (int w = 0; w < 3; ++w) row[r][w] ^= row[c][w];
        }
        for (int k = 0; k < kK; ++k) {
            uint64_t* g = t.gen[k]; g[0] = g[1] = g[2] = 0;
            setb(g, kM + k);
            for (int m = 0; m < kM; ++m) if (getb(row[m], kM + k)) setb(g, m);   // row m now says p_m = Σ i_k
        }
        t.ok = ok;
        return true;
    }();
    (void)init;
    return t;
}

/* ★ Padé approximants (the [3/2] of tanh, the [5/4] of atanh): 0.2 % at worst over the range BP uses, no libm call
 *  per edge — sum-product's cost is these two per edge per iteration. */
inline float fastTanh(float x) {
    if (x > 4.97f) return 0.9999f;
    if (x < -4.97f) return -0.9999f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
inline float fastAtanh(float x) {
    const float x2 = x * x;
    return x * (945.0f - 735.0f * x2 + 64.0f * x2 * x2) / (945.0f - 1050.0f * x2 + 225.0f * x2 * x2);
}

} // namespace

int checkDegree(int m) { return tables().deg[m]; }
const uint8_t* checkBits(int m) { return kNm[m]; }

uint16_t crc12(const uint8_t* b) {
    uint32_t reg = 0;
    for (int i = 0; i < 88; ++i) {                       // 75 bits, then 13 zeros (the CRC's place + the spare bit)
        const uint32_t bit = i < 75 ? (b[i] & 1u) : 0u;
        const uint32_t top = (reg >> 11) & 1u;
        reg = ((reg << 1) | bit) & 0xFFFu;
        if (top) reg ^= 0xC06u;
    }
    return (uint16_t)(reg ^ 42u);
}

void encode(const uint8_t* info, uint8_t* cw) {
    const Tables& t = tables();
    uint64_t c[3] = {0, 0, 0};
    for (int k = 0; k < kK; ++k) if (info[k] & 1) { c[0] ^= t.gen[k][0]; c[1] ^= t.gen[k][1]; c[2] ^= t.gen[k][2]; }
    for (int i = 0; i < kN; ++i) cw[i] = (uint8_t)getb(c, i);
}

int bpDecode(const float* llr, int maxIters, uint8_t* cw) {
    const Tables& t = tables();
    if (!t.ok) return -1;
    constexpr int E = kN * 3;                            // 522 edges
    float c2v[E];                                        // check → bit, per edge
    float tv[E];                                         // tanh(bit → check / 2), per edge
    std::fill(c2v, c2v + E, 0.0f);
    float post[kN];
    int best = 1 << 30, stall = 0;
    for (int it = 0; it <= maxIters; ++it) {
        // posterior and hard decision
        for (int v = 0; v < kN; ++v) {
            const int16_t* ve = t.varEdge[v];
            post[v] = llr[v] + c2v[ve[0]] + c2v[ve[1]] + c2v[ve[2]];
            cw[v] = post[v] > 0 ? 1 : 0;
        }
        int bad = 0;
        for (int m = 0; m < kM; ++m) {
            int s = 0;
            for (int e = t.chkEdge0[m]; e < t.chkEdge0[m + 1]; ++e) s ^= cw[t.edgeVar[e]];
            bad += s;
        }
        if (!bad) {
            int err = 0;
            for (int v = 0; v < kN; ++v) err += (cw[v] != (llr[v] > 0 ? 1 : 0));
            return err;
        }
        if (it == maxIters) break;
        /* ★ Give up early on a candidate that is going nowhere (most of them are noise): no fewer unsatisfied checks
         *  for 5 iterations running, after the 10th, with many still unsatisfied. */
        if (bad < best) { best = bad; stall = 0; } else if (++stall >= 5 && it >= 10 && bad > 15) break;
        /* ★ bit → check (extrinsic). The tanh rule is for log p(0)/p(1); ours are log p(1)/p(0), so each message
         *  goes in negated and comes out negated — without that the sign of every check's answer depends on its
         *  degree. */
        for (int v = 0; v < kN; ++v) {
            const int16_t* ve = t.varEdge[v];
            for (int j = 0; j < 3; ++j) tv[ve[j]] = fastTanh(-0.5f * (post[v] - c2v[ve[j]]));
        }
        // check → bit: -2·atanh(Π of the others), by prefix and suffix products (no divide, no zero trouble)
        for (int m = 0; m < kM; ++m) {
            const int e0 = t.chkEdge0[m], d = t.chkEdge0[m + 1] - e0;
            float pre[8], suf[8];
            pre[0] = 1.0f;
            for (int j = 0; j < d; ++j) pre[j + 1] = pre[j] * tv[e0 + j];
            suf[d] = 1.0f;
            for (int j = d - 1; j >= 0; --j) suf[j] = suf[j + 1] * tv[e0 + j];
            for (int j = 0; j < d; ++j) {
                float p = pre[j] * suf[j + 1];
                p = std::max(-0.9999f, std::min(0.9999f, p));
                c2v[e0 + j] = -2.0f * fastAtanh(p);
            }
        }
    }
    return -1;
}

int osd1(const float* llr, int maxErrors, uint8_t* cw) {
    const Tables& t = tables();
    if (!t.ok) return -1;
    // bits in order of reliability
    int order[kN];
    for (int i = 0; i < kN; ++i) order[i] = i;
    std::sort(order, order + kN, [&](int a, int b) { return std::fabs(llr[a]) > std::fabs(llr[b]); });
    uint64_t hard[3] = {0, 0, 0};
    for (int i = 0; i < kN; ++i) if (llr[i] > 0) setb(hard, i);
    /* ★ Eliminate the generator so that each of 87 rows is 1 at one pivot bit and 0 at the other pivots, the pivots
     *  taken greedily in reliability order (a bit that depends on the ones already taken is skipped). */
    uint64_t g[kK][3];
    std::memcpy(g, t.gen, sizeof g);
    int pivot[kK]; int np = 0;
    for (int oi = 0; oi < kN && np < kK; ++oi) {
        const int col = order[oi];
        int r = -1;
        for (int q = np; q < kK; ++q) if (getb(g[q], col)) { r = q; break; }
        if (r < 0) continue;
        if (r != np) for (int w = 0; w < 3; ++w) std::swap(g[r][w], g[np][w]);
        for (int q = 0; q < kK; ++q)
            if (q != np && getb(g[q], col)) for (int w = 0; w < 3; ++w) g[q][w] ^= g[np][w];
        pivot[np++] = col;
    }
    if (np < kK) return -1;
    uint64_t c0[3] = {0, 0, 0};
    for (int q = 0; q < kK; ++q) if (getb(hard, pivot[q])) for (int w = 0; w < 3; ++w) c0[w] ^= g[q][w];
    float bestD = 1e30f; int bestErr = -1; uint64_t bestC[3] = {0, 0, 0};
    for (int f = -1; f < kK; ++f) {
        uint64_t c[3] = {c0[0], c0[1], c0[2]};
        if (f >= 0) for (int w = 0; w < 3; ++w) c[w] ^= g[f][w];
        const uint64_t d[3] = {c[0] ^ hard[0], c[1] ^ hard[1], c[2] ^ hard[2]};
        const int err = __builtin_popcountll(d[0]) + __builtin_popcountll(d[1]) + __builtin_popcountll(d[2]);
        if (err > maxErrors) continue;
        float dist = 0;
        for (int w = 0; w < 3; ++w) { uint64_t x = d[w]; while (x) { dist += std::fabs(llr[w * 64 + __builtin_ctzll(x)]); x &= x - 1; } }
        if (dist >= bestD) continue;
        uint8_t info[kK];
        for (int k = 0; k < kK; ++k) info[k] = (uint8_t)getb(c, kM + k);
        uint16_t rx = 0;
        for (int k = 75; k < 87; ++k) rx = (uint16_t)((rx << 1) | info[k]);
        if (crc12(info) != rx) continue;
        bestD = dist; bestErr = err; std::memcpy(bestC, c, sizeof bestC);
    }
    if (bestErr < 0) return -1;
    for (int i = 0; i < kN; ++i) cw[i] = (uint8_t)getb(bestC, i);
    return bestErr;
}

} // namespace js8
} // namespace vibe
