// vibe_dvsig_fec.h — THE CODES of the digital-voice SIGNALLING decoder (V12, docs/v12/DV-SIGNALLING.md): the block codes,
// CRCs, scramblers and interleavers of DMR, D-STAR, System Fusion (YSF), NXDN and dPMR, both ways (the encoders exist for
// our own transmitter in vibe_dvsig_tx.h, which the tests and the benchmark row run through the receiver).
//
// ★★★ IDENTITY AND DATA ONLY — NEVER VOICE. Nothing here touches a vocoder frame: AMBE / AMBE+2 / IMBE are permanently
//     excluded (Stuart). The voice bits of every system are skipped by position and never even sliced into bytes.
//
// Where every fact came from (the house rule: published standards first; GPL code READ for facts, never copied):
//   DMR    ETSI TS 102 361-1 V2.7.1 (2026-05) Annex B (every generator matrix, the BPTC layouts and interleave, the
//          RS(12,9) generator matrix and GF(2⁸) polynomial, CRC-CCITT, the 5-bit checksum, the CRC masks); -2 V2.5.1 for
//          the LC layouts (vibe_dvsig.h).
//   dPMR   ETSI TS 102 490 V1.9.1 (2016-08) §7 (CRC7, CRC8, the shortened Hamming(12,8) matrix, the x⁹+x⁵+1 scrambler, the
//          12×6 and 12×10 interleavers).
//   D-STAR JARL "アマチュア無線のデジタル化技術の標準方式" (STD7.0), Ap1 (the x⁷+x⁴+1 scrambler, all ones at the start,
//          LSB first on air), Ap2 (rate-½ K=3 code, 24-row interleave matrix). The generator taps (1+D+D², 1+D²) and the
//          order they leave the encoder are our reading of Ap2 plus the MMDVM firmware (GPL-2, read for facts only); the
//          interleave AND the scrambler as we generate them were checked bit for bit against that firmware's tables in a
//          scratch script (not shipped) — they agree.
//   YSF    No free Yaesu specification exists. The facts (Golay(24,12) FICH, CRC-CCITT, the K=5 code, the 9×20 / 5×20
//          dibit interleavers, the 20-byte whitening) are from MMDVMHost (GPL-2), READ for facts. The whitening is not
//          copied as a table: it is the x⁹+x⁵+1 sequence, generated here, which we showed reproduces those 20 bytes.
//   NXDN   The NXDN Forum's specifications need a member/guest log-in, which this work did not use. The facts (PN9 symbol
//          scrambling, LICH, the SACCH/FACCH1 codes, CRC6/CRC12, the puncture positions, the interleavers) are from
//          MMDVMHost (GPL-2), READ for facts; the scrambling sequence is generated (x⁹+x⁵+1), shown to reproduce its table.
//
// ★★ SOFT/HARD. The convolutional codes (YSF, NXDN, D-STAR) take soft bits (int8, positive = "1"); YSF and NXDN reuse
//    M17's K=5 Viterbi (vibe_m17_fec.h — identical generator polynomials, and its add-compare-select is the NEON / SSE2 /
//    scalar-bit-exact kernel). The block codes (DMR, dPMR) are hard-decision except Golay, which reuses M17's Chase decoder.
// ★ SIMD: the hot loops of this decoder are in the front end (vibe_dvsig.h: the discriminator, the matched filters and
//   the sync correlators). The codes here run once per 30–100 ms frame; profiled on the M5 Pro they are under 2 % of the
//   decoder's time, so they stay scalar (DECODER-SIMD-AUDIT.md's "say so rather than force it").
#pragma once
#include "vibe_m17_fec.h"   // Golay(24,12) (M17's = YSF's = the parent of DMR's (20,8)), the K=5 Viterbi, conv encoder

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vibe {
namespace dvsig {

using Bits = std::vector<uint8_t>;   ///< one bit per byte (0/1), in the order sent

// ── bits ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint32_t takeBits(const uint8_t* b, int pos, int n) { uint32_t v = 0; for (int i = 0; i < n; ++i) v = (v << 1) | (b[pos + i] & 1u); return v; }
inline void putBits(uint8_t* b, int pos, int n, uint32_t v) { for (int i = 0; i < n; ++i) b[pos + i] = uint8_t((v >> (n - 1 - i)) & 1u); }
inline void bitsToBytesMsb(const uint8_t* b, int nBits, uint8_t* out) {
    std::memset(out, 0, size_t((nBits + 7) / 8));
    for (int i = 0; i < nBits; ++i) if (b[i]) out[i >> 3] |= uint8_t(0x80u >> (i & 7));
}
inline void bytesToBitsMsb(const uint8_t* p, int nBits, uint8_t* out) { for (int i = 0; i < nBits; ++i) out[i] = uint8_t((p[i >> 3] >> (7 - (i & 7))) & 1u); }
inline int popc(uint32_t x) { return __builtin_popcount(x); }

// ── A systematic block code from its generator matrix's parity half (one row per data bit, MSB first) ─────────────────
//    encode(d) = d << (n−k) | parity; decode by syndrome: 0 = clean, a single-bit error corrected, anything else refused.
struct Hamming {
    int n = 0, k = 0;
    std::vector<uint32_t> rows;     ///< parity of data bit (k−1−i) is rows[i]
    std::vector<int16_t> fix;       ///< syndrome → bit (0 = MSB of the codeword) to flip; −1 = not a single error
    Hamming(int n_, int k_, std::vector<uint32_t> r) : n(n_), k(k_), rows(std::move(r)), fix(size_t(1u << (n_ - k_)), int16_t(-1)) {
        for (int pos = 0; pos < n; ++pos) fix[syndrome(1u << (n - 1 - pos))] = int16_t(pos);
        fix[0] = -2;   // ★ clean
    }
    uint32_t parity(uint32_t d) const { uint32_t p = 0; for (int i = 0; i < k; ++i) if ((d >> (k - 1 - i)) & 1u) p ^= rows[size_t(i)]; return p; }
    uint32_t encode(uint32_t d) const { return (d << (n - k)) | parity(d); }
    uint32_t syndrome(uint32_t cw) const { return (cw & ((1u << (n - k)) - 1u)) ^ parity(cw >> (n - k)); }
    /** −1 = uncorrectable, 0 = clean, 1 = one bit corrected (cw updated). */
    int correct(uint32_t& cw) const {
        const int f = fix[syndrome(cw)];
        if (f == -2) return 0;
        if (f < 0) return -1;
        cw ^= 1u << (n - 1 - f);
        return 1;
    }
};
// ETSI TS 102 361-1 tables B.14 – B.17 (parity halves, one row per data bit, top row = MSB)
inline const Hamming& h13_9() { static const Hamming h(13, 9, {0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3}); return h; }
inline const Hamming& h15_11() { static const Hamming h(15, 11, {0x9, 0xD, 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3}); return h; }
inline const Hamming& h16_11() { static const Hamming h(16, 11, {0x13, 0x1A, 0x1F, 0x1C, 0x0E, 0x15, 0x0B, 0x16, 0x19, 0x0D, 0x07}); return h; }
inline const Hamming& h7_4() { static const Hamming h(7, 4, {0x5, 0x7, 0x6, 0x3}); return h; }
// ETSI TS 102 490 table 7.1 — dPMR's shortened (12,8)
inline const Hamming& h12_8() { static const Hamming h(12, 8, {0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3}); return h; }

// ── DMR Golay(20,8): the extended Golay(24,12) shortened by four (TS 102 361-1 B.3.1). Its table B.11 is M17's Golay ─────
//    generator with the top four data bits zero — the test checks all eight rows against B.11.
inline uint32_t golay20Encode(uint8_t d) { return m17::golayEncode(uint16_t(d)) & 0xFFFFFu; }
/** 20 soft bits (positive = 1) → the 8 data bits, or −1. The four shortened positions go in as certain zeros. */
inline int golay20Decode(const int8_t* soft20, int* dist = nullptr) {
    int8_t s[24];
    for (int i = 0; i < 4; ++i) s[i] = -127;
    for (int i = 0; i < 20; ++i) s[4 + i] = soft20[i];
    const int d = m17::golaySoft(s, dist);
    if (d < 0 || (d & 0xF00)) return -1;
    return d;
}

// ── DMR Quadratic Residue (16,7,6) for the EMB (B.3.2, table B.12) ─────────────────────────────────────────────────────
inline uint16_t qr16Encode(uint8_t d7) {
    static const uint16_t kRows[7] = {0x04F, 0x11E, 0x1B7, 0x1E2, 0x1C9, 0x0E5, 0x073};
    uint16_t p = 0;
    for (int i = 0; i < 7; ++i) if ((d7 >> (6 - i)) & 1u) p ^= kRows[i];
    return uint16_t((uint16_t(d7 & 0x7F) << 9) | p);
}
/** Minimum distance over all 128 codewords (d = 6: two errors corrected). Returns the 7 data bits, or −1. */
inline int qr16Decode(uint16_t r, int* errs = nullptr) {
    int best = -1, bd = 99;
    for (int d = 0; d < 128; ++d) { const int e = popc(uint32_t(qr16Encode(uint8_t(d)) ^ r)); if (e < bd) { bd = e; best = d; } }
    if (bd > 2) return -1;
    if (errs) *errs = bd;
    return best;
}

// ── DMR BPTC(196,96) (B.1.1): a 13 × 15 matrix — 9 rows of Hamming(15,11), 15 columns of Hamming(13,9) ─────────────────
//    Matrix index m = 1 + 15·row + col (index 0 is the extra reserved R(3)); transmitted position = m·181 mod 196.
//    Info bit I(95) (the first sent, the MSB of byte 0) sits at row 0 col 3; R(2..0) at row 0 cols 0–2.
inline void bptc196Encode(const uint8_t* info96, uint8_t* tx196) {
    uint8_t m[13][15] = {};
    int k = 0;
    for (int r = 0; r < 9; ++r)
        for (int c = (r == 0 ? 3 : 0); c < 11; ++c) m[r][c] = info96[k++];
    for (int r = 0; r < 9; ++r) {
        uint32_t d = 0; for (int c = 0; c < 11; ++c) d = (d << 1) | m[r][c];
        const uint32_t cw = h15_11().encode(d);
        for (int c = 0; c < 15; ++c) m[r][c] = uint8_t((cw >> (14 - c)) & 1u);
    }
    for (int c = 0; c < 15; ++c) {
        uint32_t d = 0; for (int r = 0; r < 9; ++r) d = (d << 1) | m[r][c];
        const uint32_t cw = h13_9().encode(d);
        for (int r = 0; r < 13; ++r) m[r][c] = uint8_t((cw >> (12 - r)) & 1u);
    }
    tx196[0] = 0;   // R(3)
    for (int idx = 1; idx < 196; ++idx) tx196[(idx * 181) % 196] = m[(idx - 1) / 15][(idx - 1) % 15];
}
/** 196 received bits → 96 info bits. Rows and columns corrected alternately (three passes); valid only when every row
 *  and column ends clean. `fixes` = bits corrected. */
inline bool bptc196Decode(const uint8_t* rx196, uint8_t* info96, int* fixes = nullptr) {
    uint8_t m[13][15];
    for (int idx = 1; idx < 196; ++idx) m[(idx - 1) / 15][(idx - 1) % 15] = rx196[(idx * 181) % 196] & 1u;
    int nfix = 0;
    bool clean = false;
    for (int pass = 0; pass < 3 && !clean; ++pass) {
        clean = true;
        for (int c = 0; c < 15; ++c) {
            uint32_t cw = 0; for (int r = 0; r < 13; ++r) cw = (cw << 1) | m[r][c];
            const int e = h13_9().correct(cw);
            if (e != 0) clean = false;
            if (e == 1) { ++nfix; for (int r = 0; r < 13; ++r) m[r][c] = uint8_t((cw >> (12 - r)) & 1u); }
        }
        for (int r = 0; r < 9; ++r) {
            uint32_t cw = 0; for (int c = 0; c < 15; ++c) cw = (cw << 1) | m[r][c];
            const int e = h15_11().correct(cw);
            if (e != 0) clean = false;
            if (e == 1) { ++nfix; for (int c = 0; c < 15; ++c) m[r][c] = uint8_t((cw >> (14 - c)) & 1u); }
        }
    }
    if (!clean) {   // ★ one last look: every code must now be clean
        for (int r = 0; r < 9 && clean; ++r) { uint32_t cw = 0; for (int c = 0; c < 15; ++c) cw = (cw << 1) | m[r][c]; if (h15_11().syndrome(cw)) return false; }
        for (int c = 0; c < 15; ++c) { uint32_t cw = 0; for (int r = 0; r < 13; ++r) cw = (cw << 1) | m[r][c]; if (h13_9().syndrome(cw)) return false; }
    }
    int k = 0;
    for (int r = 0; r < 9; ++r)
        for (int c = (r == 0 ? 3 : 0); c < 11; ++c) info96[k++] = m[r][c];
    if (fixes) *fixes = nfix;
    return true;
}

// ── DMR embedded signalling BPTC (B.2.1, figure B.3): 8 × 16 — 7 rows of Hamming(16,11), a column parity row ────────────
//    72 LC bits + the 5-bit checksum; read out by columns into four 32-bit fragments (bursts B, C, D, E of the superframe).
inline uint8_t lcChecksum(const uint8_t* lc9) { unsigned s = 0; for (int i = 0; i < 9; ++i) s += lc9[i]; return uint8_t(s % 31); }
inline void embEncode(const uint8_t* lc9, uint8_t frag[4][32]) {
    uint8_t lc[72]; bytesToBitsMsb(lc9, 72, lc);
    const uint8_t cs = lcChecksum(lc9);
    uint8_t m[8][16] = {};
    int k = 0;
    for (int r = 0; r < 7; ++r) {
        const int nlc = r < 2 ? 11 : 10;
        uint32_t d = 0;
        for (int i = 0; i < nlc; ++i) d = (d << 1) | lc[k++];
        if (r >= 2) d = (d << 1) | ((cs >> (6 - r)) & 1u);   // CS(4) in row 2 … CS(0) in row 6
        const uint32_t cw = h16_11().encode(d);
        for (int c = 0; c < 16; ++c) m[r][c] = uint8_t((cw >> (15 - c)) & 1u);
    }
    for (int c = 0; c < 16; ++c) { uint8_t p = 0; for (int r = 0; r < 7; ++r) p ^= m[r][c]; m[7][c] = p; }
    for (int q = 0; q < 128; ++q) frag[q / 32][q % 32] = m[q % 8][q / 8];
}
/** Four fragments → the 9 LC bytes, or false (a row past one error, a column parity or the checksum wrong). */
inline bool embDecode(const uint8_t frag[4][32], uint8_t* lc9, int* fixes = nullptr) {
    uint8_t m[8][16];
    for (int q = 0; q < 128; ++q) m[q % 8][q / 8] = frag[q / 32][q % 32] & 1u;
    int nfix = 0;
    for (int r = 0; r < 7; ++r) {
        uint32_t cw = 0; for (int c = 0; c < 16; ++c) cw = (cw << 1) | m[r][c];
        const int e = h16_11().correct(cw);
        if (e < 0) return false;
        if (e == 1) { ++nfix; for (int c = 0; c < 16; ++c) m[r][c] = uint8_t((cw >> (15 - c)) & 1u); }
    }
    for (int c = 0; c < 16; ++c) { uint8_t p = 0; for (int r = 0; r < 8; ++r) p ^= m[r][c]; if (p) return false; }
    uint8_t lc[72]; int k = 0; uint8_t cs = 0;
    for (int r = 0; r < 7; ++r) {
        const int nlc = r < 2 ? 11 : 10;
        for (int i = 0; i < nlc; ++i) lc[k++] = m[r][i];
        if (r >= 2) cs = uint8_t((cs << 1) | m[r][10]);
    }
    bitsToBytesMsb(lc, 72, lc9);
    if (lcChecksum(lc9) != cs) return false;
    if (fixes) *fixes = nfix;
    return true;
}

// ── DMR Reed-Solomon (12,9) over GF(2⁸)/x⁸+x⁴+x³+x²+1 (B.3.6) — its parity from table B.18's generator matrix ───────────
inline uint8_t gfMul(uint8_t a, uint8_t b) {
    unsigned r = 0, x = a;
    for (int i = 0; i < 8; ++i) { if ((b >> i) & 1u) r ^= x; x <<= 1; if (x & 0x100u) x ^= 0x11Du; }
    return uint8_t(r);
}
/** The three parity octets (p2 p1 p0, in the order sent) of 9 LC octets (m8 first). */
inline void rs129Parity(const uint8_t* m9, uint8_t* p3) {
    static const uint8_t kG[9][3] = {{0x1C, 0xBC, 0xFD}, {0x89, 0x31, 0x08}, {0xAD, 0x41, 0x36}, {0x7D, 0x71, 0x16}, {0xF3, 0xA6, 0x3A},
                                     {0x08, 0x83, 0x7B}, {0x3F, 0x6F, 0x02}, {0x6C, 0x0D, 0xA7}, {0x0E, 0x38, 0x40}};
    p3[0] = p3[1] = p3[2] = 0;
    for (int i = 0; i < 9; ++i) for (int j = 0; j < 3; ++j) p3[j] ^= gfMul(m9[i], kG[i][j]);
}
// The Data Type CRC masks (table B.21)
constexpr uint32_t kMaskVoiceHdr = 0x969696, kMaskTerm = 0x999999;
constexpr uint16_t kMaskPi = 0x6969, kMaskCsbk = 0xA5A5, kMaskMbc = 0xAAAA, kMaskDataHdr = 0xCCCC, kMaskUsbd = 0x3333;

// ── CRC-16 CCITT, MSB first, preset 0, inverted (DMR B.3.8; YSF's is the same) ───────────────────────────────────────────
inline uint16_t crcCcittMsb(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; ++i) { c ^= uint16_t(p[i]) << 8; for (int b = 0; b < 8; ++b) c = uint16_t((c & 0x8000u) ? (c << 1) ^ 0x1021u : (c << 1)); }
    return uint16_t(~c);
}
/** CRC-16 X.25 (reflected 0x1021, preset 0xFFFF, inverted; stored low byte first) — D-STAR's P_FCS. */
inline uint16_t crcX25(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) { c ^= p[i]; for (int b = 0; b < 8; ++b) c = uint16_t((c & 1u) ? (c >> 1) ^ 0x8408u : (c >> 1)); }
    return uint16_t(~c);
}
/** A plain MSB-first CRC over `n` bits (0/1) — NXDN CRC6/CRC12 (presets all ones) and dPMR CRC7/CRC8 (preset 0). */
inline uint32_t crcBits(const uint8_t* b, int n, int width, uint32_t poly, uint32_t preset) {
    uint32_t c = preset;
    const uint32_t top = 1u << (width - 1), mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    for (int i = 0; i < n; ++i) { const bool fb = ((c & top) != 0) ^ (b[i] & 1u); c = (c << 1) & mask; if (fb) c ^= poly; }
    return c & mask;
}

// ── Linear-feedback scramblers ────────────────────────────────────────────────────────────────────────────────────────
/** The sequence of a Fibonacci LFSR whose OUTPUT is its feedback bit (taps a and b, 1-based from the newest). */
inline Bits lfsrSeq(int nbits, int tapA, int tapB, uint32_t init, int count) {
    Bits o(static_cast<size_t>(count));
    uint32_t s = init;
    const uint32_t mask = (1u << nbits) - 1u;
    for (int i = 0; i < count; ++i) {
        const uint32_t fb = ((s >> (tapA - 1)) ^ (s >> (tapB - 1))) & 1u;
        o[size_t(i)] = uint8_t(fb);
        s = ((s << 1) | fb) & mask;
    }
    return o;
}
/** D-STAR (Ap1.1): x⁷+x⁴+1, all ones at the start of each scrambled section. */
inline const Bits& dstarPn() { static const Bits s = lfsrSeq(7, 4, 7, 0x7F, 700); return s; }
/** YSF's whitening (the x⁹+x⁵+1 sequence; its first 20 bytes are the published table). */
inline const Bits& ysfWhite() { static const Bits s = lfsrSeq(9, 5, 9, 0x1CE, 160); return s; }
/** NXDN: one sequence bit per SYMBOL from symbol 10 (after the FSW); a 1 inverts that symbol's sign. */
inline const Bits& nxdnPn() {
    static const Bits s = [] { Bits v(192, 0); const Bits q = lfsrSeq(9, 5, 9, 140, 182); for (int i = 0; i < 182; ++i) v[size_t(10 + i)] = q[size_t(i)]; return v; }();
    return s;
}
/** dPMR (§7.4): x⁹+x⁵+1, preset all ones, restarted for each block. ⚠ The output tap is our reading of figure 3. */
inline const Bits& dpmrPn() { static const Bits s = lfsrSeq(9, 5, 9, 0x1FF, 128); return s; }

// ── D-STAR header FEC (Ap2): rate ½, K = 3, then the 24-row interleave ─────────────────────────────────────────────────
//    The 41 header octets go in LSB first, then two zero flush bits: 330 → 660. Per input bit the encoder sends
//    d⊕d₁⊕d₂ then d⊕d₂. Interleave: transmitted bit t holds coded bit r + 24·c for (row r, column c) read row by row,
//    rows 0–11 having 28 columns and rows 12–23 having 27.
inline const std::array<uint16_t, 660>& dstarInterleave() {   // [t] = coded bit index sent at time t
    static const std::array<uint16_t, 660> t = [] {
        std::array<uint16_t, 660> a{}; int k = 0;
        for (int r = 0; r < 24; ++r) for (int c = 0; c < (r < 12 ? 28 : 27); ++c) a[size_t(k++)] = uint16_t(r + 24 * c);
        return a;
    }();
    return t;
}
inline Bits dstarHeaderEncode(const uint8_t* hdr41) {
    Bits in; in.reserve(330);
    for (int i = 0; i < 41; ++i) for (int b = 0; b < 8; ++b) in.push_back(uint8_t((hdr41[i] >> b) & 1u));
    in.push_back(0); in.push_back(0);
    Bits coded; coded.reserve(660);
    unsigned d1 = 0, d2 = 0;
    for (uint8_t d : in) { coded.push_back(uint8_t(d ^ d1 ^ d2)); coded.push_back(uint8_t(d ^ d2)); d2 = d1; d1 = d; }
    Bits tx(660);
    const auto& il = dstarInterleave();
    const Bits& pn = dstarPn();
    for (int t = 0; t < 660; ++t) tx[size_t(t)] = uint8_t(coded[il[size_t(t)]] ^ pn[size_t(t)]);
    return tx;
}
/** 660 soft bits as received (positive = 1) → the 41 octets; false when the CRC fails. `errs` = coded bits that disagree
 *  with the re-encoded winner. ★ A 4-state Viterbi: 330 steps once per transmission — scalar (it does not pay to vectorise). */
inline bool dstarHeaderDecode(const int8_t* rx660, uint8_t* hdr41, int* errs = nullptr) {
    int8_t coded[660];
    const auto& il = dstarInterleave();
    const Bits& pn = dstarPn();
    for (int t = 0; t < 660; ++t) { const int8_t v = rx660[t]; coded[il[size_t(t)]] = pn[size_t(t)] ? int8_t(-v) : v; }
    int pm[4] = {0, -100000, -100000, -100000};
    std::vector<uint8_t> dec(330);
    for (int k = 0; k < 330; ++k) {
        const int a = coded[2 * k], b = coded[2 * k + 1];
        int nw[4]; uint8_t dk = 0;
        for (int ns = 0; ns < 4; ++ns) {   // state = (d1, d2) as d1<<1|d2; next state from input d: (d, d1)
            const unsigned d = (ns >> 1) & 1u, d1 = ns & 1u;
            int best = -(1 << 30); unsigned bestD2 = 0;
            for (unsigned d2 = 0; d2 < 2; ++d2) {
                const int prev = int((d1 << 1) | d2);
                const unsigned o1 = d ^ d1 ^ d2, o2 = d ^ d2;
                const int m = pm[prev] + (o1 ? a : -a) + (o2 ? b : -b);
                if (m > best) { best = m; bestD2 = d2; }
            }
            nw[ns] = best; if (bestD2) dk |= uint8_t(1u << ns);
        }
        dec[size_t(k)] = dk;
        for (int s = 0; s < 4; ++s) pm[s] = nw[s];
    }
    uint8_t bits[330];
    unsigned s = 0;   // ★ ends in state 0 (two flush zeros)
    for (int k = 329; k >= 0; --k) {
        bits[k] = uint8_t((s >> 1) & 1u);
        const unsigned d1 = s & 1u, d2 = (dec[size_t(k)] >> s) & 1u;
        s = (d1 << 1) | d2;
    }
    std::memset(hdr41, 0, 41);
    for (int i = 0; i < 328; ++i) if (bits[i]) hdr41[i >> 3] |= uint8_t(1u << (i & 7));
    if (errs) {
        const Bits re = dstarHeaderEncode(hdr41);
        int e = 0; for (int t = 0; t < 660; ++t) if ((rx660[t] > 0) != (re[size_t(t)] != 0)) ++e;
        *errs = e;
    }
    const uint16_t c = crcX25(hdr41, 39);
    return hdr41[39] == uint8_t(c & 0xFF) && hdr41[40] == uint8_t(c >> 8);
}

// ── YSF: the FICH (Golay ×4 + CRC → K=5 → 20×5 dibit interleave) and the DCH (CRC → K=5 → 20×9 / 20×5 interleave) ───────
//    Interleave (both): coded pair i goes to the bit pair at 2·(i / W) + 2·H·(i % W)... written out: pair i → position
//    40·(i % W) + 2·(i / W), W = 5 (FICH, VD2 DCH) or 9 (header CSD / VD1 DCH).
inline int ysfIl(int i, int w) { return 40 * (i % w) + 2 * (i / w); }
/** `nInfoBytes` octets (CRC included) → coded, interleaved bits (2 × (8·n + 4)). */
inline Bits ysfConvIl(const uint8_t* bytes, int nInfoBytes, int w) {
    std::vector<uint8_t> in; m17::bytesToBits(bytes, size_t(nInfoBytes) * 8, in);
    const std::vector<uint8_t> c = m17::convEncode(m17::withFlush(in));
    Bits o(c.size());
    for (size_t i = 0; i < c.size() / 2; ++i) { const int p = ysfIl(int(i), w); o[size_t(p)] = c[2 * i]; o[size_t(p + 1)] = c[2 * i + 1]; }
    return o;
}
/** The inverse: soft interleaved bits → the info octets via the Viterbi. Returns the re-encode error count. */
inline int ysfDeIlViterbi(const int8_t* soft, int nInfoBytes, int w, uint8_t* out) {
    const int nPairs = nInfoBytes * 8 + 4;
    std::vector<int8_t> c(static_cast<size_t>(2 * nPairs));
    for (int i = 0; i < nPairs; ++i) { const int p = ysfIl(i, w); c[size_t(2 * i)] = soft[p]; c[size_t(2 * i + 1)] = soft[p + 1]; }
    const m17::ViterbiOut v = m17::viterbi(c.data(), c.size());
    std::memset(out, 0, size_t(nInfoBytes));
    for (int i = 0; i < nInfoBytes * 8 && i < int(v.bits.size()); ++i) if (v.bits[size_t(i)]) out[i >> 3] |= uint8_t(0x80u >> (i & 7));
    const Bits re = ysfConvIl(out, nInfoBytes, w);
    int e = 0; for (size_t i = 0; i < re.size(); ++i) if (soft[i] != 0 && (soft[i] > 0) != (re[i] != 0)) ++e;
    return e;
}
inline bool ysfCrcOk(const uint8_t* p, int nWithCrc) {
    const uint16_t c = crcCcittMsb(p, size_t(nWithCrc - 2));
    return p[nWithCrc - 2] == uint8_t(c >> 8) && p[nWithCrc - 1] == uint8_t(c & 0xFF);
}
inline void ysfAddCrc(uint8_t* p, int nWithCrc) { const uint16_t c = crcCcittMsb(p, size_t(nWithCrc - 2)); p[nWithCrc - 2] = uint8_t(c >> 8); p[nWithCrc - 1] = uint8_t(c & 0xFF); }
/** FICH: 4 octets → 200 interleaved coded bits. */
inline Bits ysfFichEncode(const uint8_t* fich4) {
    uint8_t f[6]; std::memcpy(f, fich4, 4); ysfAddCrc(f, 6);
    const uint16_t w0 = uint16_t((f[0] << 4) | (f[1] >> 4)), w1 = uint16_t(((f[1] & 0x0F) << 8) | f[2]);
    const uint16_t w2 = uint16_t((f[3] << 4) | (f[4] >> 4)), w3 = uint16_t(((f[4] & 0x0F) << 8) | f[5]);
    uint8_t g[12];
    const uint32_t c[4] = {m17::golayEncode(w0), m17::golayEncode(w1), m17::golayEncode(w2), m17::golayEncode(w3)};
    for (int i = 0; i < 4; ++i) { g[3 * i] = uint8_t(c[i] >> 16); g[3 * i + 1] = uint8_t(c[i] >> 8); g[3 * i + 2] = uint8_t(c[i]); }
    return ysfConvIl(g, 12, 5);
}
/** 200 soft bits → the 4 FICH octets; false unless the four Golay words and the CRC pass. */
inline bool ysfFichDecode(const int8_t* soft200, uint8_t* fich4, int* errs = nullptr) {
    uint8_t g[12];
    const int e = ysfDeIlViterbi(soft200, 12, 5, g);
    uint16_t w[4];
    for (int i = 0; i < 4; ++i) {
        const uint32_t r = (uint32_t(g[3 * i]) << 16) | (uint32_t(g[3 * i + 1]) << 8) | g[3 * i + 2];
        uint32_t cw; int ge = 0;
        if (!m17::golayHard(r, &cw, &ge)) return false;
        w[i] = uint16_t(cw >> 12);
    }
    uint8_t f[6] = {uint8_t(w[0] >> 4), uint8_t(((w[0] & 0xF) << 4) | (w[1] >> 8)), uint8_t(w[1]), uint8_t(w[2] >> 4), uint8_t(((w[2] & 0xF) << 4) | (w[3] >> 8)), uint8_t(w[3])};
    if (!ysfCrcOk(f, 6)) return false;
    std::memcpy(fich4, f, 4);
    if (errs) *errs = e;
    return true;
}
inline void ysfWhiten(uint8_t* p, int n) {
    const Bits& w = ysfWhite();
    for (int i = 0; i < n * 8; ++i) if (w[size_t(i)]) p[i >> 3] ^= uint8_t(0x80u >> (i & 7));
}

// ── NXDN: SACCH (26 bits + CRC6 → K=5 → 12 punctured → 60, 12×5) and FACCH1 (80 + CRC12 → K=5 → 48 punctured → 144, 16×9)
inline int nxdnIlSacch(int i) { return 5 * (i % 12) + i / 12; }
inline int nxdnIlFacch(int i) { return 9 * (i % 16) + i / 16; }
inline bool nxdnPunctSacch(int n) { return n % 6 == 5; }      // coded positions 5, 11, …, 71 are not sent
inline bool nxdnPunctFacch(int n) { return n % 4 == 1; }      // coded positions 1, 5, …, 189 are not sent
/** info bits (CRC included, no tail) → the sent bits, interleaved. */
inline Bits nxdnEncode(const uint8_t* info, int nInfo, bool facch) {
    std::vector<uint8_t> in(info, info + nInfo);
    const std::vector<uint8_t> c = m17::convEncode(m17::withFlush(in));
    Bits kept;
    for (size_t n = 0; n < c.size(); ++n) if (!(facch ? nxdnPunctFacch(int(n)) : nxdnPunctSacch(int(n)))) kept.push_back(c[n]);
    Bits o(kept.size());
    for (size_t i = 0; i < kept.size(); ++i) o[size_t(facch ? nxdnIlFacch(int(i)) : nxdnIlSacch(int(i)))] = kept[i];
    return o;
}
/** soft sent bits → info bits (nInfo, CRC included); returns the re-encode error count. */
inline int nxdnDecode(const int8_t* soft, int nInfo, bool facch, uint8_t* info) {
    const int nSent = facch ? 144 : 60, nCoded = 2 * (nInfo + 4);
    std::vector<int8_t> kept(static_cast<size_t>(nSent));
    for (int i = 0; i < nSent; ++i) kept[size_t(i)] = soft[facch ? nxdnIlFacch(i) : nxdnIlSacch(i)];
    std::vector<int8_t> c(static_cast<size_t>(nCoded), 0);
    for (int n = 0, k = 0; n < nCoded; ++n) if (!(facch ? nxdnPunctFacch(n) : nxdnPunctSacch(n))) c[size_t(n)] = kept[size_t(k++)];
    const m17::ViterbiOut v = m17::viterbi(c.data(), c.size());
    for (int i = 0; i < nInfo; ++i) info[i] = i < int(v.bits.size()) ? v.bits[size_t(i)] : 0;
    const Bits re = nxdnEncode(info, nInfo, facch);
    int e = 0; for (int i = 0; i < nSent; ++i) if (soft[i] != 0 && (soft[i] > 0) != (re[size_t(i)] != 0)) ++e;
    return e;
}
inline uint32_t nxdnCrc6(const uint8_t* b, int n) { return crcBits(b, n, 6, 0x27, 0x3F); }
inline uint32_t nxdnCrc12(const uint8_t* b, int n) { return crcBits(b, n, 12, 0x80F, 0xFFF); }

// ── dPMR (§7): CRC → octets → Hamming(12,8) each → interleave (12 rows × N columns: in by columns, out by rows) → PN ────
inline uint32_t dpmrCrc7(const uint8_t* b, int n) { return crcBits(b, n, 7, 0x09, 0); }    // x⁷ + x³ + 1
inline uint32_t dpmrCrc8(const uint8_t* b, int n) { return crcBits(b, n, 8, 0x07, 0); }    // x⁸ + x² + x + 1
/** `nOct` × 12 coded bits: input index (column j, row i) = 12·j + i; output row-major: out[i·nOct + j]. */
inline Bits dpmrBlockEncode(const uint8_t* bits, int nBits /* = 8·nOct */) {
    const int nOct = nBits / 8;
    Bits coded(static_cast<size_t>(12 * nOct));
    for (int o = 0; o < nOct; ++o) {
        const uint32_t cw = h12_8().encode(takeBits(bits, 8 * o, 8));
        for (int i = 0; i < 12; ++i) coded[size_t(12 * o + i)] = uint8_t((cw >> (11 - i)) & 1u);
    }
    Bits tx(coded.size());
    for (int i = 0; i < 12; ++i) for (int j = 0; j < nOct; ++j) tx[size_t(i * nOct + j)] = coded[size_t(12 * j + i)];
    const Bits& pn = dpmrPn();
    for (size_t t = 0; t < tx.size(); ++t) tx[t] ^= pn[t];
    return tx;
}
/** The inverse on hard bits; false when an octet's Hamming word has more than one error. */
inline bool dpmrBlockDecode(const uint8_t* tx, int nOct, uint8_t* bits, int* fixes = nullptr) {
    const Bits& pn = dpmrPn();
    int nfix = 0;
    for (int o = 0; o < nOct; ++o) {
        uint32_t cw = 0;
        for (int i = 0; i < 12; ++i) { const int t = i * nOct + o; cw = (cw << 1) | ((tx[t] ^ pn[size_t(t)]) & 1u); }
        const int e = h12_8().correct(cw);
        if (e < 0) return false;
        nfix += e;
        putBits(bits, 8 * o, 8, cw >> 4);
    }
    if (fixes) *fixes = nfix;
    return true;
}

}  // namespace dvsig
}  // namespace vibe
