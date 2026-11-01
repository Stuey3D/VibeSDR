// vibe_vdl2_fec.h — V12 VDL Mode 2: the burst's coding layers — scrambler, header (25,20) code, Reed-Solomon (255,249)
// with erasures, the octet interleave, HDLC framing. docs/v12/AVIATION.md §VDL2.
//
// ★★★ WRITTEN FRESH. ETSI EN 301 841-1/-2 (free) fix the structure: a reserved symbol, the 17-bit length and 5 header
//     check bits after the unique word, RS(255,249) then the interleave, scrambling of everything after the unique word,
//     HDLC frames with the ISO 13239 FCS. The parameters the free texts leave to ICAO Doc 9776 (paid) were taken as FACTS
//     from reading dumpvdl2 (GPL-3.0-or-later; read for inspiration, no code copied): the LFSR (x¹⁵ + x + 1, seed 0x6959,
//     reset at the header), the header code's five parity-check rows, RS over GF(2⁸) with x⁸+x⁷+x²+x+1 (0x187) and
//     first root α¹²⁰, the shortened last block's parity count (0 / 2 / 4 / 6 by its length), and the octet interleave
//     (the data dealt across the blocks a column at a time, then the parity likewise). Every routine here is our own.
// ★★ SOFT INFORMATION INTO A HARD CODE. The RS decoder takes ERASURES: a byte holding a weak symbol can be declared
//    erased (one parity byte instead of two), so a block with 1 error and 4 weak bytes still decodes (vibe_vdl2.h tries
//    errors-only first, then the weakest 2, 4 and 6 bytes erased; the HDLC FCS judges every frame in the end).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vibevdl2 {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  The scrambler — 15-stage LFSR, x¹⁵ + x + 1, reset to 0x6959 at the first header bit, every later bit XORed
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Lfsr {
    uint16_t s = 0x6959;
    int next() {
        const int b = (s ^ (s >> 14)) & 1;
        s = uint16_t((s >> 1) | (b << 14));
        return b;
    }
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  The header: 3 reserved bits, the 17-bit transmission length (LSB first), 5 check bits — a (25,20) linear code.
//  Words here put the FIRST header bit on the air in bit 24.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t kHdrRows[5] = { 0x001FFF0u, 0x07E1FE8u, 0x18E61E4u, 0x1B6A662u, 0x0D3CAA1u };

inline uint32_t hdrSyndrome(uint32_t w) {
    uint32_t s = 0;
    for (int i = 0; i < 5; ++i) s |= uint32_t(__builtin_popcount(w & kHdrRows[i]) & 1) << (4 - i);
    return s;
}
/** The lightest error pattern for each syndrome (our own table: every 1-bit pattern, then 2-bit patterns for the
 *  syndromes no single bit explains; bits in the reserved field are only blamed when nothing else fits). */
struct HdrTable {
    uint32_t err[32]; int weight[32];
    HdrTable() {
        for (int s = 0; s < 32; ++s) { err[s] = 0; weight[s] = s ? 99 : 0; }
        for (int pass = 0; pass < 2; ++pass)                       // pass 0: the 22 length + check bits; 1: also reserved
            for (int i = 0; i < 25; ++i) {
                if (pass == 0 && i >= 22) continue;
                const uint32_t e = 1u << i, s = hdrSyndrome(e);
                if (weight[s] > 1) { weight[s] = 1; err[s] = e; }
            }
        for (int i = 0; i < 22; ++i)
            for (int j = i + 1; j < 22; ++j) {
                const uint32_t e = (1u << i) | (1u << j), s = hdrSyndrome(e);
                if (weight[s] > 2) { weight[s] = 2; err[s] = e; }
            }
    }
    static const HdrTable& get() { static HdrTable t; return t; }
};
/** Correct a header word in place; returns the bits changed (0 clean), −1 if the reserved bits are not zero after. */
inline int hdrCorrect(uint32_t& w) {
    const uint32_t s = hdrSyndrome(w);
    const HdrTable& t = HdrTable::get();
    w ^= t.err[s];
    if (w >> 22) return -1;
    return t.weight[s];
}
/** The 17-bit length from a corrected header word (sent LSB first). */
inline uint32_t hdrLength(uint32_t w) {
    const uint32_t f = (w >> 5) & 0x1FFFFu;
    uint32_t r = 0;
    for (int i = 0; i < 17; ++i) r |= ((f >> (16 - i)) & 1u) << i;
    return r;
}
/** Encode a header word (tests): `len` bits, check bits from the rows (each row's parity must come out even). */
inline uint32_t hdrEncode(uint32_t len) {
    uint32_t f = 0;
    for (int i = 0; i < 17; ++i) f |= ((len >> i) & 1u) << (16 - i);
    uint32_t w = f << 5;
    // solve for the 5 check bits: try all 32 (the code is small)
    for (uint32_t c = 0; c < 32; ++c) if (hdrSyndrome(w | c) == 0) return w | c;
    return w;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  Reed-Solomon (255,249) over GF(2⁸) / 0x187, roots α^120 … α^125. Codeword byte i is the coefficient of x^(254−i):
//  data in 0…248, parity in 249…254.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Rs {
public:
    static constexpr int N = 255, K = 249, R = 6, FCR = 120;
    static const Rs& get() { static Rs r; return r; }

    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp_[log_[a] + log_[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp_[(log_[a] + 255 - log_[b]) % 255] : 0; }
    uint8_t pw(int e) const { return exp_[((e % 255) + 255) % 255]; }

    /** Systematic parity for cw[0…248] into cw[249…254] (tests and the generators). */
    void encode(uint8_t* cw) const {
        uint8_t par[R] = {0};
        for (int i = 0; i < K; ++i) {
            const uint8_t fb = cw[i] ^ par[0];
            for (int j = 0; j < R - 1; ++j) par[j] = par[j + 1] ^ mul(fb, gen_[R - 1 - j]);
            par[R - 1] = mul(fb, gen_[0]);
        }
        std::memcpy(cw + K, par, R);
    }
    /** Decode in place. `eras` = erased byte positions (≤ 6). `valid(i)` false = a position that is a known zero
     *  (a shortened block's padding): an error located there means failure. Returns bytes changed, or −1. */
    template <class Valid> int decode(uint8_t* cw, const int* eras, int ne, Valid&& valid) const {
        uint8_t S[R];
        bool any = false;
        for (int j = 0; j < R; ++j) {
            const uint8_t a = pw(FCR + j);
            uint8_t s = 0;
            for (int i = 0; i < N; ++i) s = mul(s, a) ^ cw[i];
            S[j] = s; any |= s != 0;
        }
        if (!any) return 0;
        if (ne > R) return -1;
        // Λ starts as the erasure locator Γ(x) = Π (1 − X_k x)
        uint8_t L[R + 1] = {1}, B[R + 1], T[R + 1];
        for (int k = 0; k < ne; ++k) {
            const uint8_t X = pw(N - 1 - eras[k]);
            for (int d = std::min(k + 1, R); d >= 1; --d) L[d] ^= mul(X, L[d - 1]);
        }
        std::memcpy(B, L, sizeof B);
        int deg = ne;
        for (int r = ne; r < R; ++r) {           // Berlekamp–Massey over the remaining syndromes
            uint8_t d = 0;
            for (int i = 0; i <= deg && i <= r; ++i) d ^= mul(L[i], S[r - i]);
            // B ← x·B
            for (int i = R; i >= 1; --i) B[i] = B[i - 1];
            B[0] = 0;
            if (!d) continue;
            for (int i = 0; i <= R; ++i) T[i] = L[i] ^ mul(d, B[i]);
            if (2 * deg <= r + ne) {
                deg = r + 1 + ne - deg;
                for (int i = 0; i <= R; ++i) B[i] = div(L[i], d);
            }
            std::memcpy(L, T, sizeof L);
        }
        int dl = R; while (dl > 0 && !L[dl]) --dl;
        if (dl == 0 || 2 * (dl - ne) + ne > R) return -1;
        // Ω = S·Λ mod x^R
        uint8_t O[R] = {0};
        for (int i = 0; i < R; ++i) for (int j = 0; j <= i && j <= dl; ++j) O[i] ^= mul(L[j], S[i - j]);
        // Chien + Forney
        int found = 0, pos[R]; uint8_t val[R];
        for (int i = 0; i < N && found <= dl; ++i) {
            const int e = N - 1 - i;                       // X = α^e
            const uint8_t xi = pw(-e);                     // X⁻¹
            uint8_t v = 0, p = 1;
            for (int k = 0; k <= dl; ++k) { v ^= mul(L[k], p); p = mul(p, xi); }
            if (v) continue;
            if (found == dl || !valid(i)) return -1;
            uint8_t num = 0; p = 1;
            for (int k = 0; k < R; ++k) { num ^= mul(O[k], p); p = mul(p, xi); }
            uint8_t den = 0; p = 1;                        // Λ'(X⁻¹): the odd terms
            for (int k = 1; k <= dl; k += 2) { den ^= mul(L[k], p); p = mul(p, mul(xi, xi)); }
            if (!den) return -1;
            // magnitude = X^(1−FCR) · Ω(X⁻¹) / Λ'(X⁻¹)
            val[found] = mul(pw(e * (1 - FCR)), div(num, den));
            pos[found++] = i;
        }
        if (found != dl) return -1;
        for (int k = 0; k < found; ++k) cw[pos[k]] ^= val[k];
        // ★ verify: a decoder past its power can "correct" into a wrong word; the syndromes say so
        for (int j = 0; j < R; ++j) {
            const uint8_t a = pw(FCR + j);
            uint8_t s = 0;
            for (int i = 0; i < N; ++i) s = mul(s, a) ^ cw[i];
            if (s) return -1;
        }
        int changed = 0;
        for (int k = 0; k < found; ++k) changed += val[k] != 0;
        return changed;
    }

private:
    Rs() {
        int x = 1;
        for (int i = 0; i < 255; ++i) { exp_[i] = uint8_t(x); log_[x] = uint8_t(i); x <<= 1; if (x & 0x100) x ^= 0x187; }
        for (int i = 255; i < 512; ++i) exp_[i] = exp_[i - 255];
        log_[0] = 0;
        // g(x) = Π_{j<R} (x − α^(FCR+j)), gen_[k] = coefficient of x^k
        uint8_t g[R + 1] = {1};
        for (int j = 0; j < R; ++j) {
            const uint8_t r = pw(FCR + j);
            for (int k = j + 1; k >= 1; --k) g[k] = g[k - 1] ^ mul(g[k], r);
            g[0] = mul(g[0], r);
        }
        std::memcpy(gen_, g, sizeof gen_);
    }
    uint8_t exp_[512]{}, log_[256]{}, gen_[R + 1]{};
};

/** Parity bytes on the shortened last block, by its data length. */
inline int lastParity(int len) { return len < 3 ? 0 : len < 31 ? 2 : len < 68 ? 4 : 6; }

/** The layout of a burst with `octets` data octets. */
struct Layout {
    int octets = 0, blocks = 0, lastLen = 0, lastPar = 0, fecOctets = 0;
    explicit Layout(int oct) : octets(oct) {
        blocks = oct / Rs::K; lastLen = oct % Rs::K;
        fecOctets = blocks * Rs::R;
        if (lastLen) { ++blocks; lastPar = lastParity(lastLen); fecOctets += lastPar; }
        else { lastLen = Rs::K; lastPar = Rs::R; }
    }
    int parOf(int b) const { return b == blocks - 1 ? lastPar : Rs::R; }
    int lenOf(int b) const { return b == blocks - 1 ? lastLen : Rs::K; }
};

/** Deal `n` octets across `rows` rows a column at a time, starting at column `off`, `width` columns, the last row only
 *  `lastW` wide. `place(row, col, srcIndex)`. The transmitter's interleave and our deinterleave are this same walk. */
template <class Place> inline void dealColumns(int n, int rows, int width, int lastW, int off, Place&& place) {
    int i = 0;
    for (int col = 0; col < width && i < n; ++col)
        for (int r = 0; r < rows && i < n; ++r) {
            if (r == rows - 1 && col >= lastW) continue;
            place(r, off + col, i++);
        }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  HDLC: frames between 0x7E flags, a 0 removed after five 1s, seven 1s abort; the FCS checked by the caller
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** Every byte-aligned frame between flags in `bits` (0/1 per entry). */
inline std::vector<std::vector<uint8_t>> hdlcFrames(const std::vector<uint8_t>& bits) {
    std::vector<std::vector<uint8_t>> out;
    std::vector<uint8_t> cur;            // destuffed bits since the last flag
    int ones = 0;
    bool inFrame = false;
    for (uint8_t b : bits) {
        if (b) {
            if (++ones > 6) { inFrame = false; cur.clear(); continue; }   // abort
            cur.push_back(1);
            continue;
        }
        if (ones == 6) {                 // 01111110: a flag. The six 1s (and the 0 before them) are not data.
            if (inFrame && cur.size() >= 7 + 8 * 4) {
                const size_t nb = cur.size() - 7;     // drop the flag's leading 0 and six 1s
                if (nb % 8 == 0) {
                    std::vector<uint8_t> f(nb / 8, 0);
                    for (size_t i = 0; i < nb; ++i) if (cur[i]) f[i / 8] |= uint8_t(1u << (i % 8));
                    out.push_back(std::move(f));
                }
            }
            inFrame = true; cur.clear(); ones = 0;
            continue;
        }
        if (ones == 5) { ones = 0; continue; }   // a stuffed 0
        ones = 0;
        cur.push_back(0);
    }
    return out;
}

/** ISO 13239 FCS-16 over a frame INCLUDING its two FCS bytes: the good residue is 0xF0B8. */
inline bool fcsGood(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
    }
    return c == 0xF0B8;
}

}  // namespace vibevdl2
