// decoders/drm/drm_coding.h — DRM channel coding (ETSI ES 201 980 V4.3.1 clause 7, annex D): energy dispersal, the CRCs,
// the multilevel-coding arithmetic, the rate-1/6 K = 7 mother code with its puncturing, the bit and cell interleavers, the
// QAM mappings, and the soft-decision Viterbi decoder.
//
// ★★★ BOTH DIRECTIONS LIVE HERE. The receiver needs the encoder anyway — multistage decoding re-encodes each decoded
//     level to condition the next level's soft bits (§7.3.1: "the decoding … can be done either straightforwardly or
//     through an iterative process") — and the test transmitter (drm_tx.h) uses the same functions, so a synthetic super
//     frame is built by the standard's own arithmetic and every table is exercised in both directions.
// ★★ Soft bits are LLRs, positive = "0 is more likely", CSI-weighted by |H|² (max-log). The Viterbi is max-log too, so a
//    global scale never matters — only the RELATIVE weight of cells (a faded carrier must count for less).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VIBE_FORCE_SCALAR)
  #include <arm_neon.h>
  #include "../../vibedsp/neon_compat.h"   // ★ ARMv7 lacks vdivq_f32/vaddvq_f32 — the Pi 2 is exactly that machine
  #define VIBE_DRM_NEON 1
#elif (defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)) && !defined(VIBE_FORCE_SCALAR)
  #include <emmintrin.h>
  #define VIBE_DRM_SSE 1
#endif

namespace vibedrm {

// ── Bits ─────────────────────────────────────────────────────────────────────────────────────────────────────────

/** MSB-first reader over a bit vector (one bit per byte, 0/1) — the decoded streams are short, clarity wins. */
struct BitReader {
    const uint8_t* b; size_t n; size_t pos = 0;
    BitReader(const uint8_t* bits, size_t count) : b(bits), n(count) {}
    uint32_t get(int k) { uint32_t v = 0; for (int i = 0; i < k; ++i) v = (v << 1) | (pos < n ? b[pos++] : (pos++, 0u)); return v; }
    bool ok() const { return pos <= n; }
    size_t left() const { return pos < n ? n - pos : 0; }
};
inline void putBits(std::vector<uint8_t>& out, uint32_t v, int k) { for (int i = k - 1; i >= 0; --i) out.push_back(uint8_t((v >> i) & 1u)); }
inline std::vector<uint8_t> bitsToBytes(const uint8_t* bits, size_t nBits) {
    std::vector<uint8_t> o((nBits + 7) / 8, 0);
    for (size_t i = 0; i < nBits; ++i) if (bits[i]) o[i >> 3] |= uint8_t(0x80u >> (i & 7));
    return o;
}

// ── Energy dispersal (§7.2.2): PRBS x^9 + x^5 + 1, all ones at the start of every block ──────────────────────────

inline void prbs(uint8_t* bits, size_t n) {
    uint32_t r = 0x1FF;                         // stages 1..9, all ones
    for (size_t i = 0; i < n; ++i) {
        const uint32_t o = ((r >> 8) ^ (r >> 4)) & 1u;   // stage 9 ⊕ stage 5
        r = ((r << 1) | o) & 0x1FF;
        bits[i] ^= uint8_t(o);
    }
}

// ── CRC (annex D): MSB first, register all ones, result inverted ──────────────────────────────────────────────────

/** CRC over `n` bits (0/1 per byte) with generator `poly` (without the x^deg term) of degree `deg`. */
inline uint32_t crcBits(const uint8_t* bits, size_t n, uint32_t poly, int deg) {
    uint32_t reg = (1u << deg) - 1u;
    const uint32_t top = 1u << (deg - 1), mask = (1u << deg) - 1u;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t fb = ((reg & top) ? 1u : 0u) ^ (bits[i] & 1u);
        reg = (reg << 1) & mask;
        if (fb) reg ^= poly;
    }
    return (~reg) & mask;
}
constexpr uint32_t kCrc8Poly = 0x1D;      // x^8 + x^4 + x^3 + x^2 + 1
constexpr uint32_t kCrc16Poly = 0x1021;   // x^16 + x^12 + x^5 + 1
inline uint32_t crcBytes(const uint8_t* p, size_t n, uint32_t poly, int deg) {
    std::vector<uint8_t> bits; bits.reserve(n * 8);
    for (size_t i = 0; i < n; ++i) putBits(bits, p[i], 8);
    return crcBits(bits.data(), bits.size(), poly, deg);
}

// ── Code rates and puncturing (tables 27, 28) ─────────────────────────────────────────────────────────────────────

struct Rate { int rx, ry; };
/** Per input bit j (mod rx): which of the six mother outputs B0..B5 are sent (bit q = Bq). */
inline const std::vector<uint8_t>& punctureColumns(Rate r) {
    static const std::vector<uint8_t> r1_6{0x3F}, r1_4{0x0F}, r3_10{0x0F, 0x07, 0x07}, r1_3{0x07}, r4_11{0x07, 0x07, 0x07, 0x03},
        r2_5{0x07, 0x03}, r1_2{0x03}, r4_7{0x03, 0x05, 0x03, 0x01}, r3_5{0x03, 0x01, 0x03}, r2_3{0x03, 0x01},
        r8_11{0x03, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x01}, r3_4{0x03, 0x01, 0x01}, r4_5{0x03, 0x01, 0x01, 0x01},
        r7_8{0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01}, r8_9{0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01}, none;
    switch (r.rx * 100 + r.ry) {
        case 106: return r1_6;  case 104: return r1_4;  case 310: return r3_10; case 103: return r1_3; case 411: return r4_11;
        case 205: return r2_5;  case 102: return r1_2;  case 407: return r4_7;  case 305: return r3_5; case 203: return r2_3;
        case 811: return r8_11; case 304: return r3_4;  case 405: return r4_5;  case 708: return r7_8; case 809: return r8_9;
        default: return none;
    }
}
/** The six tail columns for index rp (table 28). */
inline void tailColumns(int rp, uint8_t col[6]) {
    static const char* b2[7] = { "000000", "100000", "100100", "110100", "110110", "111110", "111111" };
    static const char* b3[12] = { "000000", "000000", "000000", "000000", "000000", "000000", "000000",
                                  "100000", "100100", "110100", "110101", "111101" };
    for (int j = 0; j < 6; ++j) {
        uint8_t m = 0x03;
        if (rp >= 0 && rp <= 11) {
            if ((rp <= 6 ? b2[rp] : "111111")[j] == '1') m |= 0x04;
            if (b3[rp][j] == '1') m |= 0x08;
        }
        col[j] = m;
    }
}

/** ★ A level's PUNCTURING SCHEDULE: for each mother-code input step (data bits then the 6 tail bits), the mask of
 *  outputs sent. Part A (rate ra, steps nA), part B (rate rb, steps nB), then the tail by rp — or, for the FAC, the
 *  tail punctured with the ordinary pattern (tailRp < 0). */
struct Schedule {
    std::vector<uint8_t> mask;     // one per input step
    size_t outBits = 0;
    void build(Rate ra, size_t nA, Rate rb, size_t nB, int tailRp) {
        mask.clear(); outBits = 0;
        const auto& ca = punctureColumns(ra);
        const auto& cb = punctureColumns(rb);
        for (size_t i = 0; i < nA; ++i) mask.push_back(ca[i % ca.size()]);
        for (size_t i = 0; i < nB; ++i) mask.push_back(cb[i % cb.size()]);
        if (tailRp >= 0) { uint8_t t[6]; tailColumns(tailRp, t); for (int j = 0; j < 6; ++j) mask.push_back(t[j]); }
        else for (size_t i = nB; i < nB + 6; ++i) mask.push_back(cb[i % cb.size()]);
        for (uint8_t m : mask) outBits += size_t(__builtin_popcount(m));
    }
};

// ── The mother code (§7.3.2): K = 7, generators 133 171 145 133 171 145 (octal) ───────────────────────────────────

inline int parity(uint32_t v) { return __builtin_parity(v); }
/** The three distinct outputs for register (a_i << 6 | state); B3..B5 repeat B0..B2. */
inline uint32_t motherOut3(uint32_t reg) {
    return uint32_t(parity(reg & 0133)) | (uint32_t(parity(reg & 0171)) << 1) | (uint32_t(parity(reg & 0145)) << 2);
}

/** Encode `bits` (data, the 6 zero tail bits are appended) and puncture by `sch`. */
inline std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits, const Schedule& sch) {
    std::vector<uint8_t> out; out.reserve(sch.outBits);
    uint32_t st = 0;
    const size_t steps = bits.size() + 6;
    for (size_t i = 0; i < steps; ++i) {
        const uint32_t a = i < bits.size() ? (bits[i] & 1u) : 0u;
        const uint32_t reg = (a << 6) | st;
        const uint32_t o3 = motherOut3(reg);
        const uint8_t m = i < sch.mask.size() ? sch.mask[i] : 0;
        for (int q = 0; q < 6; ++q) if (m & (1u << q)) out.push_back(uint8_t((o3 >> (q % 3)) & 1u));
        st = reg >> 1;
    }
    return out;
}

/** ★ Soft Viterbi over the depunctured stream. `llr` holds the received soft bits in transmission order; the result is
 *  the `nData` data bits (the trellis is forced to state 0 by the tail). Returns the final path metric margin in
 *  `quality` (unused by callers that only want bits). */
class Viterbi {
public:
    std::vector<uint8_t> decode(const float* llr, const Schedule& sch, size_t nData) {
        const size_t steps = sch.mask.size();
        // depuncture into the three combined soft values per step (B0+B3, B1+B4, B2+B5)
        soft_.assign(steps * 3, 0.0f);
        size_t p = 0;
        for (size_t i = 0; i < steps; ++i) {
            const uint8_t m = sch.mask[i];
            for (int q = 0; q < 6; ++q) if (m & (1u << q)) soft_[i * 3 + size_t(q % 3)] += llr[p++];
        }
        dec_.assign(steps, 0);
        float pm[64], nm[64];
        for (int s = 0; s < 64; ++s) pm[s] = -1e30f;
        pm[0] = 0.0f;
        for (size_t i = 0; i < steps; ++i) {
            const float l0 = soft_[i * 3], l1 = soft_[i * 3 + 1], l2 = soft_[i * 3 + 2];
            float bm[8];
            for (int o = 0; o < 8; ++o) bm[o] = ((o & 1) ? -l0 : l0) + ((o & 2) ? -l1 : l1) + ((o & 4) ? -l2 : l2);
            uint64_t d = 0;
            for (int ns = 0; ns < 64; ++ns) {
                const uint32_t a = uint32_t(ns) >> 5;
                const uint32_t s0 = (uint32_t(ns) << 1) & 63u, s1 = s0 | 1u;
                const float m0 = pm[s0] + bm[outTab()[(a << 6) | s0]];
                const float m1 = pm[s1] + bm[outTab()[(a << 6) | s1]];
                if (m1 > m0) { nm[ns] = m1; d |= (1ull << ns); } else nm[ns] = m0;
            }
            dec_[i] = d;
            // ★ renormalise: float metrics would otherwise grow without bound over a long block
            const float base = nm[0];
            for (int s = 0; s < 64; ++s) pm[s] = nm[s] - base;
        }
        // traceback from state 0 (the tail forced it)
        std::vector<uint8_t> out(steps, 0);
        uint32_t st = 0;
        for (size_t i = steps; i-- > 0;) {
            out[i] = uint8_t(st >> 5);
            const uint32_t b = uint32_t((dec_[i] >> st) & 1u);
            st = ((st << 1) & 63u) | b;
        }
        out.resize(nData);
        return out;
    }
private:
    static const uint8_t* outTab() {
        static uint8_t t[128];
        static bool init = [] { for (uint32_t r = 0; r < 128; ++r) t[r] = uint8_t(motherOut3(r)); return true; }();
        (void)init;
        return t;
    }
    std::vector<float> soft_;
    std::vector<uint64_t> dec_;
};

// ── Interleavers (§7.3.3, §7.6) ───────────────────────────────────────────────────────────────────────────────────

/** Π(i) for an interleaver of `xin` elements with multiplier t. y[i] = v[Π(i)]. */
inline std::vector<int> permutation(int xin, int t) {
    std::vector<int> p(size_t(std::max(xin, 0)));
    if (xin <= 0) return p;
    int s = 1; while (s < xin) s <<= 1;
    const int q = s / 4 - 1;
    p[0] = 0;
    for (int i = 1; i < xin; ++i) {
        int v = int((int64_t(t) * p[size_t(i - 1)] + q) % s);
        while (v >= xin) v = int((int64_t(t) * v + q) % s);
        p[size_t(i)] = v;
    }
    return p;
}

// ── Constellations (§7.4) ─────────────────────────────────────────────────────────────────────────────────────────

/** levels: 1 = 4-QAM, 2 = 16-QAM, 3 = 64-QAM. Per axis the amplitude is (2^L − 1 − 2·r)·a, r = b0 + 2·b1 + 4·b2. */
inline float qamScale(int levels) { return levels == 3 ? 1.0f / std::sqrt(42.0f) : levels == 2 ? 1.0f / std::sqrt(10.0f) : 1.0f / std::sqrt(2.0f); }
inline float axisValue(int levels, int r) { return float((1 << levels) - 1 - 2 * r) * qamScale(levels); }

/** Bit interleaver multipliers per level (§7.3.3): 0 = level not interleaved. */
inline int bitInterleaverT(int levels, int p) {
    if (levels == 3) return p == 0 ? 0 : p == 1 ? 13 : 21;
    if (levels == 2) return p == 0 ? 13 : 21;
    return 21;
}

/** ★ Max-log soft bits of level `p` on one AXIS for `n` cells: y = equalised axis value, w = CSI weight (|H|²),
 *  known = the decided bits of the lower levels packed as r_low (b0 + 2·b1 …), unknown higher levels are searched.
 *  LLR > 0 means 0. SIMD across cells (four per instruction), scalar twin. */
inline void axisLlr(int levels, int p, const float* y, const float* w, const uint8_t* rLow, float* out, int n) {
    const int nPts = 1 << levels;
    float val[8];
    for (int r = 0; r < nPts; ++r) val[r] = axisValue(levels, r);
    const int lowMask = (1 << p) - 1;
    int i = 0;
#if VIBE_DRM_NEON || VIBE_DRM_SSE
    // ★ Four cells at a time. A candidate r is admissible for a cell iff (r & lowMask) == that cell's known low bits, so
    //   the low bits are compared per lane; bit p of r says which of the two minima it updates.
    for (; i + 4 <= n; i += 4) {
#if VIBE_DRM_NEON
        float32x4_t yy = vld1q_f32(y + i);
        float32x4_t m0 = vdupq_n_f32(1e30f), m1 = vdupq_n_f32(1e30f);
        const int32_t lowv[4] = { rLow[i] & lowMask, rLow[i + 1] & lowMask, rLow[i + 2] & lowMask, rLow[i + 3] & lowMask };
        const int32x4_t lo = vld1q_s32(lowv);
        for (int r = 0; r < nPts; ++r) {
            const float32x4_t d = vsubq_f32(yy, vdupq_n_f32(val[r]));
            const float32x4_t d2 = vmulq_f32(d, d);
            const uint32x4_t ok = vceqq_s32(lo, vdupq_n_s32(r & lowMask));
            const float32x4_t cand = vbslq_f32(ok, d2, vdupq_n_f32(1e30f));
            if ((r >> p) & 1) m1 = vminq_f32(m1, cand); else m0 = vminq_f32(m0, cand);
        }
        vst1q_f32(out + i, vmulq_f32(vsubq_f32(m1, m0), vld1q_f32(w + i)));
#else
        __m128 yy = _mm_loadu_ps(y + i);
        __m128 m0 = _mm_set1_ps(1e30f), m1 = _mm_set1_ps(1e30f);
        const __m128i lo = _mm_setr_epi32(rLow[i] & lowMask, rLow[i + 1] & lowMask, rLow[i + 2] & lowMask, rLow[i + 3] & lowMask);
        const __m128 big = _mm_set1_ps(1e30f);
        for (int r = 0; r < nPts; ++r) {
            const __m128 d = _mm_sub_ps(yy, _mm_set1_ps(val[r]));
            const __m128 d2 = _mm_mul_ps(d, d);
            const __m128 ok = _mm_castsi128_ps(_mm_cmpeq_epi32(lo, _mm_set1_epi32(r & lowMask)));
            const __m128 cand = _mm_or_ps(_mm_and_ps(ok, d2), _mm_andnot_ps(ok, big));
            if ((r >> p) & 1) m1 = _mm_min_ps(m1, cand); else m0 = _mm_min_ps(m0, cand);
        }
        _mm_storeu_ps(out + i, _mm_mul_ps(_mm_sub_ps(m1, m0), _mm_loadu_ps(w + i)));
#endif
    }
#endif
    for (; i < n; ++i) {
        float m0 = 1e30f, m1 = 1e30f;
        const int lo = rLow[i] & lowMask;
        for (int r = 0; r < nPts; ++r) {
            if ((r & lowMask) != lo) continue;
            const float d = y[i] - val[r], d2 = d * d;
            if ((r >> p) & 1) m1 = std::min(m1, d2); else m0 = std::min(m0, d2);
        }
        out[i] = (m1 - m0) * w[i];
    }
}

// ── MLC arithmetic (§7.2.1, §7.3.1.1, tables 29–32, 36–39) ────────────────────────────────────────────────────────

/** Per-level code rates of an MSC protection level (modes A–D), and RYlcm. */
inline bool mscRates(int levels, int prot, Rate r[3], int& ryLcm) {
    if (levels == 2) {
        if (prot == 0) { r[0] = {1, 3}; r[1] = {2, 3}; ryLcm = 3; return true; }
        if (prot == 1) { r[0] = {1, 2}; r[1] = {3, 4}; ryLcm = 4; return true; }
        return false;
    }
    if (levels == 3) {
        switch (prot) {
            case 0: r[0] = {1, 4}; r[1] = {1, 2}; r[2] = {3, 4}; ryLcm = 4; return true;
            case 1: r[0] = {1, 3}; r[1] = {2, 3}; r[2] = {4, 5}; ryLcm = 15; return true;
            case 2: r[0] = {1, 2}; r[1] = {3, 4}; r[2] = {7, 8}; ryLcm = 8; return true;
            case 3: r[0] = {2, 3}; r[1] = {4, 5}; r[2] = {8, 9}; ryLcm = 45; return true;
        }
    }
    return false;
}

/** ★ The complete layout of one multiplex frame (or SDC block): cells per part, input bits per level and part, the
 *  tail index. Built from the cell count and the SDC's protection profile. */
struct MlcLayout {
    int levels = 1;
    int n1 = 0, n2 = 0;              // cells in part A, part B
    Rate ra[3]{}, rb[3]{};
    int m1[3]{}, m2[3]{};            // input bits per level, part A / part B
    int rp[3]{};
    int l1 = 0, l2 = 0;              // L1, L2
    bool fac = false;
    Schedule sch[3];

    /** MSC (or SDC: protA < 0 and partABytes = 0). Returns false on an impossible profile. */
    bool buildMsc(int lv, int nMux, int protA, int protB, int partABytes) {
        levels = lv; fac = false;
        int lcmB = 1;
        if (!mscRates(lv, protB, rb, lcmB)) return false;
        n1 = 0;
        if (partABytes > 0) {
            int lcmA = 1;
            if (!mscRates(lv, protA, ra, lcmA)) return false;
            int den = 0;
            for (int p = 0; p < lv; ++p) den += 2 * ra[p].rx * (lcmA / ra[p].ry);
            n1 = ((8 * partABytes + den - 1) / den) * lcmA;
        } else for (int p = 0; p < lv; ++p) ra[p] = rb[p];
        n2 = nMux - n1;
        if (n2 < 20) return false;
        return finish();
    }
    bool buildSdc(int lv, int nSdc) {
        levels = lv; fac = false; n1 = 0; n2 = nSdc;
        if (lv == 2) { rb[0] = {1, 3}; rb[1] = {2, 3}; } else { rb[0] = {1, 2}; }
        for (int p = 0; p < lv; ++p) ra[p] = rb[p];
        return finish();
    }
    void buildFac() {
        levels = 1; fac = true; n1 = 0; n2 = 65;
        ra[0] = rb[0] = {3, 5};
        m1[0] = 0; m2[0] = 72; rp[0] = -1; l1 = 0; l2 = 72;
        sch[0].build(ra[0], 0, rb[0], 72, -1);
    }
private:
    bool finish() {
        l1 = l2 = 0;
        for (int p = 0; p < levels; ++p) {
            m1[p] = 2 * n1 * ra[p].rx / ra[p].ry;
            const int t = (2 * n2 - 12) / rb[p].ry;
            m2[p] = rb[p].rx * t;
            rp[p] = (2 * n2 - 12) - rb[p].ry * t;
            l1 += m1[p]; l2 += m2[p];
            sch[p].build(ra[p], size_t(m1[p]), rb[p], size_t(m2[p]), rp[p]);
            if (sch[p].outBits != size_t(2 * (n1 + n2))) return false;   // ★ the arithmetic proves itself
        }
        return true;
    }
};

}  // namespace vibedrm
