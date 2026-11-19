// vibe_lrpt_fec.h — METEOR-M LRPT forward error correction (V12, 2026-11-19): the K=7 rate-½ convolutional code and
// its soft Viterbi, the attached sync marker and its eight ambiguities, the CCSDS pseudo-random sequence, Reed–Solomon
// (255,223) interleaved to depth 4, and the 80k mode's marker-stripped convolutional deinterleaver and differential code.
//
// ★★★ SOURCES. CCSDS 131.0-B (TM synchronisation and channel coding): the ASM 0x1ACFFC1D, the rate-½ K=7 code with
//     generators 171 / 133 octal, the pseudo-randomiser h(x) = x⁸+x⁷+x⁵+x³+1 seeded all ones, RS(255,223) E=16 with
//     the field x⁸+x⁷+x²+x+1, first consecutive root 112 and root spacing 11, interleaving depth I=4. METEOR-specific
//     facts (the code word layout, no dual-basis transform, the 80k mode's 0x27 marker every 80 bits and the 36-branch
//     × 2048 interleaver, the differential code) were READ from dbdexter's meteor_decode, which is MIT-licensed — the
//     code below is ours; nothing was copied. GPL decoders were not read.
//
// ★★ THE CONVENTIONS, ONCE. The encoder register is (new bit << 6) | the six previous bits (newest at bit 5) — DAB's
//    (vibe_dab_fec.h) — and the two outputs are parity(reg & 0x79) then parity(reg & 0x5B). Soft values are int8,
//    POSITIVE = bit 1, magnitude = confidence, 0 = no opinion. What order and sign the satellite's I and Q carry the two
//    outputs in is NOT assumed: the sync search tries all eight (four rotations × the I/Q swap) and the Viterbi is fed
//    the stream through the one that matched — so a mirrored or inverted second generator (CCSDS inverts G2, METEOR
//    is believed not to) costs nothing either way.
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#include "vibedsp/neon_compat.h"
#define VIBE_LRPT_VIT_NEON 1
#elif defined(__SSE2__) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define VIBE_LRPT_VIT_SSE2 1
#endif

namespace vibelrpt {

// ── Sizes ───────────────────────────────────────────────────────────────────────────────────────────────────────────
inline constexpr int kAsmBytes   = 4;
inline constexpr uint32_t kAsm   = 0x1ACFFC1Du;
inline constexpr int kCaduBytes  = 1024;                 ///< ASM + 1020 bytes of RS code blocks
inline constexpr int kFrameBytes = 1020;                 ///< 4 × 255 interleaved
inline constexpr int kVcduBytes  = 892;                  ///< 4 × 223: what is left once the parity is gone
inline constexpr int kCaduBits   = kCaduBytes * 8;       ///< 8192 data bits a CADU
inline constexpr int kCaduSoft   = kCaduBits * 2;        ///< 16384 coded (soft) bits a CADU

inline constexpr int popc(uint32_t v) { return __builtin_popcount(v); }
inline constexpr int parity(uint32_t v) { return popc(v) & 1; }

// ── The convolutional code ──────────────────────────────────────────────────────────────────────────────────────────
inline constexpr uint32_t kG1 = 0x79, kG2 = 0x5B;
/** The two output bits (G1 in bit 1, G2 in bit 0) leaving `state` (six previous bits) on `in`. */
inline constexpr int convOut(int state, int in) {
    const uint32_t reg = (uint32_t(in) << 6) | uint32_t(state);
    return (parity(reg & kG1) << 1) | parity(reg & kG2);
}
/** ★ For TESTS and the sync pattern: encode bits (one per byte, 0/1) continuing from `state`; appends 2 bits a bit
 *  (G1 first). Returns the new state. */
inline int convEncode(const uint8_t* bits, size_t n, int state, std::vector<uint8_t>& out) {
    for (size_t i = 0; i < n; i++) {
        const int b = bits[i] & 1, o = convOut(state, b);
        out.push_back(uint8_t(o >> 1)); out.push_back(uint8_t(o & 1));
        state = (state >> 1) | (b << 5);
    }
    return state;
}

// ── The sync word, as it is on the air (coded), and its eight ambiguities ───────────────────────────────────────────
/** ★★ The coded ASM's LAST 52 bits (26 of its 32 bit-steps): the first six steps' outputs depend on the six bits before
 *  the marker (the previous frame's parity, i.e. anything), so they are masked out rather than guessed. */
struct SyncPattern {
    // pat[x] = the 64 hard bits a transform-x stream would show, newest bit in bit 0; mask = the 52 bits that are fixed.
    uint64_t pat[8] = {};
    uint64_t mask = 0;
};
/** Transform x of a soft pair (a = first, b = second) — the inverse of what the channel did. x&3 = rotation by x·90°,
 *  x&4 = the I/Q swap first. Applied to the RECEIVED pair it gives (G1, G2). */
inline void applyX(int x, int a, int b, int& oa, int& ob) {
    if (x & 4) { const int t = a; a = b; b = t; }
    switch (x & 3) {
        case 0: oa = a;  ob = b;  break;
        case 1: oa = -b; ob = a;  break;
        case 2: oa = -a; ob = -b; break;
        default: oa = b; ob = -a; break;
    }
}
/** The pattern table: for each transform x, what the RECEIVED hard bits look like when their x-transform is the ASM. */
inline const SyncPattern& syncPattern() {
    static const SyncPattern sp = [] {
        SyncPattern s;
        uint8_t bits[32];
        for (int i = 0; i < 32; i++) bits[i] = uint8_t((kAsm >> (31 - i)) & 1);
        std::vector<uint8_t> enc;
        convEncode(bits, 32, 0, enc);                         // 64 coded bits, G1 G2 G1 G2 …
        for (int i = 12; i < 64; i++) s.mask |= 1ull << (63 - i);
        for (int x = 0; x < 8; x++) {
            uint64_t p = 0;
            for (int k = 0; k < 32; k++) {
                // Find the received pair (ra, rb) ∈ {±1}² whose transform x gives the encoder's pair.
                const int g1 = enc[size_t(2 * k)] ? 1 : -1, g2 = enc[size_t(2 * k + 1)] ? 1 : -1;
                int ra = 1, rb = 1;
                for (int c = 0; c < 4; c++) {
                    const int ta = (c & 1) ? -1 : 1, tb = (c & 2) ? -1 : 1;
                    int oa, ob; applyX(x, ta, tb, oa, ob);
                    if (oa == g1 && ob == g2) { ra = ta; rb = tb; break; }
                }
                p = (p << 1) | uint64_t(ra > 0);
                p = (p << 1) | uint64_t(rb > 0);
            }
            s.pat[x] = p;
        }
        return s;
    }();
    return sp;
}
/** How many of the 52 fixed bits a 64-bit hard window matches under transform x. */
inline int syncScore(uint64_t window, int x) {
    const SyncPattern& s = syncPattern();
    return 52 - __builtin_popcountll((window ^ s.pat[x]) & s.mask);
}

// ── The pseudo-random sequence ──────────────────────────────────────────────────────────────────────────────────────
/** CCSDS 131.0-B §10: h(x) = x⁸+x⁷+x⁵+x³+1, all ones at the start of each code block, MSB first; period 255 bytes. */
inline const uint8_t* pnSequence() {
    static const std::array<uint8_t, 255> pn = [] {
        std::array<uint8_t, 255> t{};
        uint8_t s = 0xFF;
        for (int i = 0; i < 255; i++) {
            uint8_t byte = 0;
            for (int j = 0; j < 8; j++) {
                const uint8_t nb = uint8_t(((s >> 7) ^ (s >> 5) ^ (s >> 3) ^ s) & 1);
                byte = uint8_t((byte << 1) | (s & 1));
                s = uint8_t((s >> 1) | (nb << 7));
            }
            t[size_t(i)] = byte;
        }
        return t;
    }();
    return pn.data();
}
inline void derandomise(uint8_t* frame1020) {
    const uint8_t* pn = pnSequence();
    for (int i = 0; i < kFrameBytes; i++) frame1020[i] ^= pn[i % 255];
}

// ── Reed–Solomon (255,223), CCSDS parameters, conventional basis ───────────────────────────────────────────────────
class Rs223 {
public:
    static const Rs223& get() { static Rs223 r; return r; }
    static constexpr int N = 255, K = 223, R = 32, FCR = 112, PRIM = 11;

    /** ★ For TESTS and the synthetic encoder: the 32 parity bytes of `data223`, systematic (data first). */
    void encode(const uint8_t* data, uint8_t* parity32) const {
        uint8_t reg[R] = {};
        for (int i = 0; i < K; i++) {
            const uint8_t fb = uint8_t(data[i] ^ reg[0]);
            for (int j = 0; j < R - 1; j++) reg[j] = uint8_t(reg[j + 1] ^ mul(fb, gen_[R - 1 - j]));
            reg[R - 1] = mul(fb, gen_[0]);
        }
        std::memcpy(parity32, reg, R);
    }

    /** Decode one 255-byte code word in place (cw[0] is the coefficient of x²⁵⁴). Returns bytes corrected (0…16), or −1
     *  when the word is beyond the code — ★ and on −1 the word is left exactly as it came. */
    int decode(uint8_t* cw) const {
        uint8_t S[R];
        bool any = false;
        /* ★ Horner with a 256-byte "times this root" table per root (8 KB in all): two loads and an XOR a byte, where a
         *  log/exp multiply with its zero tests was four times that — the syndromes run on EVERY frame, clean or not. */
        for (int i = 0; i < R; i++) {
            const uint8_t* t = mulBy_[i];                     // × β^(FCR+i), β = α^PRIM
            uint8_t acc = 0;
            for (int j = 0; j < N; j++) acc = uint8_t(t[acc] ^ cw[j]);
            S[i] = acc; any |= acc != 0;
        }
        if (!any) return 0;
        // Berlekamp–Massey.
        uint8_t L[R + 1] = {1}, B[R + 1] = {1}, T[R + 1];
        int Ld = 0, m = 1; uint8_t b = 1;
        for (int n = 0; n < R; n++) {
            uint8_t d = S[n];
            for (int i = 1; i <= Ld; i++) d ^= mul(L[i], S[n - i]);
            if (d == 0) { m++; continue; }
            std::memcpy(T, L, sizeof(L));
            const uint8_t coef = div(d, b);
            for (int i = 0; i + m <= R; i++) L[i + m] ^= mul(coef, B[i]);
            if (2 * Ld <= n) { Ld = n + 1 - Ld; std::memcpy(B, T, sizeof(T)); b = d; m = 1; }
            else m++;
        }
        if (Ld > R / 2) return -1;
        // Chien search over β: error at power p (coefficient of x^p, byte index N-1-p) ⇔ Λ(β^-p) = 0.
        int pos[R / 2], nf = 0;
        for (int p = 0; p < N && nf <= Ld; p++) {
            const int e = (N - (p * PRIM) % N) % N;           // log_α of β^-p
            uint8_t v = 0;
            for (int i = Ld; i >= 0; i--) v = uint8_t(mul(v, exp_[e]) ^ L[i]);
            if (v == 0) { if (nf < R / 2) pos[nf] = p; nf++; }
        }
        if (nf != Ld) return -1;
        // Ω(x) = S(x)Λ(x) mod x^R; Forney: e = X^(1-FCR) Ω(X⁻¹) / Λ'(X⁻¹), X = β^p.
        uint8_t Om[R] = {};
        for (int i = 0; i < R; i++) {
            uint8_t a = 0;
            for (int j = 0; j <= i && j <= Ld; j++) a ^= mul(L[j], S[i - j]);
            Om[i] = a;
        }
        uint8_t fix[R / 2];
        for (int k = 0; k < nf; k++) {
            const int p = pos[k];
            const int xinv = (N - (p * PRIM) % N) % N;        // log of X⁻¹
            uint8_t om = 0, dl = 0;
            for (int i = R - 1; i >= 0; i--) om = uint8_t(mul(om, exp_[xinv]) ^ Om[i]);
            for (int i = 1; i <= Ld; i += 2) dl ^= mul(L[i], exp_[(xinv * (i - 1)) % N]);   // odd terms: formal derivative
            if (dl == 0) return -1;
            const int xlog = (p * PRIM) % N;
            const int pw = ((xlog * (1 - FCR)) % N + N) % N;
            fix[k] = mul(exp_[pw], div(om, dl));
            if (fix[k] == 0) return -1;
        }
        for (int k = 0; k < nf; k++) cw[N - 1 - pos[k]] ^= fix[k];
        return nf;
    }

    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp_[log_[a] + log_[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp_[(log_[a] + 255 - log_[b]) % 255] : 0; }

private:
    Rs223() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp_[i] = uint8_t(x); log_[x] = uint8_t(i); x <<= 1; if (x & 0x100) x ^= 0x187; }
        for (int i = 255; i < 512; i++) exp_[i] = exp_[i - 255];
        for (int i = 0; i < R; i++) rootPow_[i] = exp_[((FCR + i) * PRIM) % 255];
        for (int i = 0; i < R; i++) for (int v = 0; v < 256; v++) mulBy_[i][v] = mul(uint8_t(v), rootPow_[i]);
        // g(x) = Π (x − β^(FCR+i)); gen_[j] = coefficient of x^j (monic, gen_[R] = 1 implied).
        uint8_t g[R + 1] = {1};
        for (int i = 0; i < R; i++) {
            const uint8_t r = rootPow_[i];
            for (int j = i + 1; j > 0; j--) g[j] = uint8_t(g[j - 1] ^ mul(g[j], r));
            g[0] = mul(g[0], r);
        }
        for (int j = 0; j < R; j++) gen_[j] = g[j];
    }
    uint8_t exp_[512]{}, log_[256]{}, rootPow_[R]{}, gen_[R]{};
    uint8_t mulBy_[R][256]{};
};

/** De-interleave, correct and re-interleave a 1020-byte frame. Returns the total corrected, or −1 if ANY of the four
 *  words failed (★ the frame is then not trusted at all — a VCDU header in a failed word is how false images happen). */
inline int rsFrame(uint8_t* f, int perWord[4] = nullptr) {
    const Rs223& rs = Rs223::get();
    int total = 0; bool bad = false;
    for (int i = 0; i < 4; i++) {
        uint8_t w[255];
        for (int j = 0; j < 255; j++) w[j] = f[j * 4 + i];
        const int e = rs.decode(w);
        if (perWord) perWord[i] = e;
        if (e < 0) { bad = true; continue; }
        total += e;
        for (int j = 0; j < 255; j++) f[j * 4 + i] = w[j];
    }
    return bad ? -1 : total;
}
/** ★ Tests / synthetic: build the 1020 interleaved bytes from 892 VCDU bytes. */
inline void rsEncodeFrame(const uint8_t* vcdu892, uint8_t* f1020) {
    const Rs223& rs = Rs223::get();
    for (int i = 0; i < 4; i++) {
        uint8_t d[223], p[32];
        for (int j = 0; j < 223; j++) d[j] = vcdu892[j * 4 + i];
        rs.encode(d, p);
        for (int j = 0; j < 223; j++) f1020[j * 4 + i] = d[j];
        for (int j = 0; j < 32; j++) f1020[(223 + j) * 4 + i] = p[j];
    }
}

// ── The soft Viterbi ────────────────────────────────────────────────────────────────────────────────────────────────
/** ★★★ A BLOCK DECODER FOR A CONTINUOUS CODE. METEOR never terminates the code — one CADU's tail runs straight into the
 *  next ASM — so each CADU is decoded with LEAD steps before it (all states equal at the start: the start state is
 *  unknown) and TAIL steps after it (traceback from the best state, where the survivors have long since merged). 64 of
 *  each costs 1.6 % over 8192 and is five constraint lengths — more than the survivors need to converge.
 *
 *  ★★ The ACS is DAB's (vibe_dab_fec.h) reduced to rate ½: destination-side, one decision bit per state per step, the
 *  second predecessor's branch metric is the first's mirrored (both generators have bits 0 and 6 set, so the two
 *  predecessors emit complementary pairs). The branch metric is computed per lane from sign masks — (s ^ m) − m negates
 *  s where m = −1 — so NEON, SSE2 and scalar run the SAME integer arithmetic and the decisions are bit-identical (the
 *  test checks all three paths against each other and against the encoder). */
class Viterbi {
public:
    static constexpr int kStates = 64;
    Viterbi() {
        for (int ns = 0; ns < kStates; ns++) {
            const int in = ns >> 5, st0 = (ns & 31) << 1;
            const int o = convOut(st0, in);
            m1_[ns] = (o & 2) ? int16_t(-1) : int16_t(0);     // G1 bit set → negate s0 in the cost
            m2_[ns] = (o & 1) ? int16_t(-1) : int16_t(0);
        }
    }
    /** `soft` = 2·steps values (already transformed to G1, G2 order and sign). Writes `outSteps` bits (one a byte,
     *  0/1) for steps [lead, lead+outSteps). Returns the best path's metric per step ×100 (a quality figure: lower is
     *  better) — 0 on nonsense input. */
    int decode(const int8_t* soft, int steps, int lead, int outSteps, uint8_t* out) {
        if (steps <= 0 || lead < 0 || lead + outSteps > steps) return 0;
        dec_.resize(size_t(steps) * 8);
        alignas(16) uint16_t a[kStates], b[kStates];
        for (int i = 0; i < kStates; i++) a[i] = 0;
        uint16_t* cur = a; uint16_t* nxt = b;
        uint32_t total = 0;
        for (int t = 0; t < steps; t++) {
            const int16_t s0 = soft[2 * t], s1 = soft[2 * t + 1];
            uint8_t* dec = &dec_[size_t(t) * 8];
            const uint16_t mn = step(s0, s1, cur, nxt, dec);
            total += mn;
            for (int i = 0; i < kStates; i++) cur[i] = uint16_t(nxt[i] - mn);
        }
        int best = 0;
        for (int i = 1; i < kStates; i++) if (cur[i] < cur[best]) best = i;
        int st = best;
        for (int t = steps - 1; t >= 0; t--) {
            const int bit = (dec_[size_t(t) * 8 + size_t(st >> 3)] >> (st & 7)) & 1;
            if (t >= lead && t < lead + outSteps) out[t - lead] = uint8_t(st >> 5);
            st = ((st & 31) << 1) | bit;
        }
        return int(uint64_t(total) * 100 / uint64_t(steps));
    }
    /** ★ Test seam: which ACS ran (0 scalar, 1 NEON, 2 SSE2), and a switch to force scalar for the A/B test. */
    static int kernel() {
#if defined(VIBE_LRPT_VIT_NEON)
        return 1;
#elif defined(VIBE_LRPT_VIT_SSE2)
        return 2;
#else
        return 0;
#endif
    }
    bool forceScalar = false;

private:
    static constexpr int kMirror = 2 * 254;     // bm0 + bm1 for the two predecessors
    uint16_t stepScalar(int16_t s0, int16_t s1, const uint16_t* cur, uint16_t* nxt, uint8_t* dec) const {
        uint16_t mn = 0xFFFF;
        for (int j = 0; j < 8; j++) {
            uint8_t bits = 0;
            for (int k = 0; k < 8; k++) {
                const int ns = j * 8 + k, st0 = (ns & 31) << 1;
                const uint16_t b0 = uint16_t(int16_t((s0 ^ m1_[ns]) - m1_[ns]) + int16_t((s1 ^ m2_[ns]) - m2_[ns]) + 254);
                const uint16_t b1 = uint16_t(kMirror - b0);
                const uint16_t c0 = uint16_t(cur[st0] + b0), c1 = uint16_t(cur[st0 | 1] + b1);
                const bool w = c1 < c0;
                nxt[ns] = w ? c1 : c0;
                bits |= uint8_t(w) << k;
                if (nxt[ns] < mn) mn = nxt[ns];
            }
            dec[j] = bits;
        }
        return mn;
    }
    uint16_t step(int16_t s0, int16_t s1, const uint16_t* cur, uint16_t* nxt, uint8_t* dec) const {
        if (forceScalar) return stepScalar(s0, s1, cur, nxt, dec);
#if defined(VIBE_LRPT_VIT_NEON)
        const int16x8_t v0 = vdupq_n_s16(s0), v1 = vdupq_n_s16(s1), k254 = vdupq_n_s16(254);
        const uint16x8_t kM = vdupq_n_u16(uint16_t(kMirror));
        const uint8x8_t kW = { 1, 2, 4, 8, 16, 32, 64, 128 };
        uint16x8_t mnv = vdupq_n_u16(0xFFFF);
        for (int j = 0; j < 8; j++) {
            const int ns0 = j * 8, base = (ns0 & 31) << 1;
            const int16x8_t ma = vld1q_s16(&m1_[ns0]), mb = vld1q_s16(&m2_[ns0]);
            const int16x8_t t = vaddq_s16(vsubq_s16(veorq_s16(v0, ma), ma), vsubq_s16(veorq_s16(v1, mb), mb));
            const uint16x8_t b0 = vreinterpretq_u16_s16(vaddq_s16(t, k254));
            const uint16x8x2_t pr = vld2q_u16(&cur[base]);
            const uint16x8_t c0 = vaddq_u16(pr.val[0], b0), c1 = vaddq_u16(pr.val[1], vsubq_u16(kM, b0));
            const uint16x8_t w = vcltq_u16(c1, c0), best = vminq_u16(c0, c1);
            vst1q_u16(&nxt[ns0], best);
            mnv = vminq_u16(mnv, best);
            dec[j] = vaddv_u8(vand_u8(vmovn_u16(w), kW));
        }
        return vminvq_u16(mnv);
#elif defined(VIBE_LRPT_VIT_SSE2)
        // ★ uint16 compares on SSE2 without SSE4.1: metrics stay far below 0x8000 (renormalised each step, spread ≤ a
        //   few thousand), so the signed compare/min give the unsigned answer — the same argument as DAB's kernel.
        const __m128i v0 = _mm_set1_epi16(s0), v1 = _mm_set1_epi16(s1), k254 = _mm_set1_epi16(254);
        const __m128i kM = _mm_set1_epi16(short(kMirror)), lo = _mm_set1_epi32(0x0000FFFF);
        __m128i mnv = _mm_set1_epi16(0x7FFF);
        for (int j = 0; j < 8; j++) {
            const int ns0 = j * 8, base = (ns0 & 31) << 1;
            const __m128i ma = _mm_load_si128((const __m128i*)&m1_[ns0]), mb = _mm_load_si128((const __m128i*)&m2_[ns0]);
            const __m128i t = _mm_add_epi16(_mm_sub_epi16(_mm_xor_si128(v0, ma), ma), _mm_sub_epi16(_mm_xor_si128(v1, mb), mb));
            const __m128i b0 = _mm_add_epi16(t, k254);
            const __m128i x = _mm_loadu_si128((const __m128i*)&cur[base]), y = _mm_loadu_si128((const __m128i*)&cur[base + 8]);
            const __m128i ev = _mm_packs_epi32(_mm_and_si128(x, lo), _mm_and_si128(y, lo));
            const __m128i od = _mm_packs_epi32(_mm_srli_epi32(x, 16), _mm_srli_epi32(y, 16));
            const __m128i c0 = _mm_add_epi16(ev, b0), c1 = _mm_add_epi16(od, _mm_sub_epi16(kM, b0));
            const __m128i w = _mm_cmpgt_epi16(c0, c1), best = _mm_min_epi16(c0, c1);
            _mm_storeu_si128((__m128i*)&nxt[ns0], best);
            mnv = _mm_min_epi16(mnv, best);
            dec[j] = uint8_t(_mm_movemask_epi8(_mm_packs_epi16(w, w)) & 0xFF);
        }
        mnv = _mm_min_epi16(mnv, _mm_shuffle_epi32(mnv, _MM_SHUFFLE(1, 0, 3, 2)));
        mnv = _mm_min_epi16(mnv, _mm_shuffle_epi32(mnv, _MM_SHUFFLE(2, 3, 0, 1)));
        mnv = _mm_min_epi16(mnv, _mm_shufflelo_epi16(mnv, _MM_SHUFFLE(2, 3, 0, 1)));
        return uint16_t(_mm_extract_epi16(mnv, 0));
#else
        return stepScalar(s0, s1, cur, nxt, dec);
#endif
    }
    alignas(16) int16_t m1_[kStates] = {};
    alignas(16) int16_t m2_[kStates] = {};
    std::vector<uint8_t> dec_;
};

// ── The differential code (METEOR-M N2-2 and later) ─────────────────────────────────────────────────────────────────
/** ★ Each of the two soft streams (the pair's first and second values) is differentially decoded on its own: the bit is
 *  the XOR of this symbol and the last, so the soft value is the product of the two (its magnitude the geometric mean,
 *  which keeps an int8 range), the second stream inverted. A phase ambiguity then reduces to the swap / sign cases the
 *  sync search already tries. Stateful across calls (the last pair of one block is the first reference of the next). */
class DiffDecoder {
public:
    void run(int8_t* s, size_t n) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            const int a = s[i], b = s[i + 1];
            s[i] = sgnSqrt(a * pa_);
            s[i + 1] = sgnSqrt(-b * pb_);
            pa_ = a; pb_ = b;
        }
    }
    void reset() { pa_ = pb_ = 0; }
private:
    static int8_t sgnSqrt(int v) {
        // ★ integer square root of |v| ≤ 16129 → ≤ 127
        const int a = v < 0 ? -v : v;
        int r = 0;
        while ((r + 1) * (r + 1) <= a) r++;
        return int8_t(v < 0 ? -r : r);
    }
    int pa_ = 0, pb_ = 0;
};

// ── The 80k mode's interleaver ──────────────────────────────────────────────────────────────────────────────────────
/** ★★ 80k MODE: every 80 soft bits on the air are an 8-bit marker (0x27) and 72 data bits; the data bits went through a
 *  36-branch convolutional interleaver, branch b (= the bit's index mod 36) delayed b × 36 × 2048 bits. The receiver
 *  therefore delays branch b by (35 − b) × D (D = 73 728): every bit comes out 35·D = 2 580 480 bits (≈ 16 s at 80k)
 *  late and in order. 2.6 MB of int8 — bounded, allocated only in this mode.
 *  The MARKER is found separately (InterleaveSync) and stripped before this sees the bits. */
class Deinterleaver {
public:
    static constexpr int kBranches = 36;
    static constexpr int kD = 36 * 2048;
    static constexpr size_t kSize = size_t(kBranches) * kD;
    Deinterleaver() : buf_(kSize, 0) {}
    /** Push one data soft bit; returns the bit that leaves (0 until the pipe has filled). */
    int8_t push(int8_t v) {
        const int b = int(n_ % kBranches);
        buf_[(n_ + size_t(35 - b) * kD) % kSize] = v;       // ★ write ahead by the branch's delay …
        const int8_t o = buf_[n_ % kSize];                  //   … and read the slot whose time has come
        buf_[n_ % kSize] = 0;
        n_++;
        return o;
    }
    /** ★ Tests: the transmitter's side — branch b delayed b·D. */
    static std::vector<int8_t> interleave(const std::vector<int8_t>& in) {
        std::vector<int8_t> out(in.size(), 0);
        for (size_t n = 0; n < in.size(); n++) {
            const size_t b = n % kBranches, d = b * size_t(kD);
            if (n >= d) out[n] = in[n - d];
        }
        return out;
    }
    uint64_t pushed() const { return n_; }
    static constexpr uint64_t latency() { return uint64_t(35) * kD; }
private:
    std::vector<int8_t> buf_;
    uint64_t n_ = 0;
};
inline constexpr uint8_t kInterMarker = 0x27;
inline constexpr int kInterStride = 80;

} // namespace vibelrpt
