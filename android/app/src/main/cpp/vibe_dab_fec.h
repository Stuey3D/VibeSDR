// vibe_dab_fec.h — DAB forward error correction: the rate-1/4 mother code, soft Viterbi,
// energy dispersal and the FIB CRC.
//
// Sources: ETSI EN 300 401 clause 11.1 (mother code + puncturing), read from the downloaded spec:
// "The octal forms of the generator polynomials are 133, 171, 145 and 133, respectively", with the
// register starting and ending in the all-zero state.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#define VIBE_DAB_VITERBI_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#if defined(__SSSE3__)
#include <tmmintrin.h>          // pshufb — the table lookup the branch metrics want
#endif
#define VIBE_DAB_VITERBI_SSE 1
#endif

namespace vibedab {

// ── The mother code ─────────────────────────────────────────────────────────
inline constexpr int kK        = 7;        ///< constraint length (6 delay elements)
inline constexpr int kStates   = 1 << (kK - 1);   ///< 64
inline constexpr int kRateOut  = 4;        ///< outputs per input bit
/** ★ Straight from the spec, in octal as it states them. */
inline constexpr unsigned kPoly[kRateOut] = { 0133, 0171, 0145, 0133 };

/** Parity of a 32-bit word — the convolutional output for one polynomial. */
inline constexpr int parity(unsigned v) {
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return int(v & 1u);
}

/** The 4 output bits for a transition: `state` is the 6 previous bits, `in` the new one.
 *  ★ The register is (in, state) with `in` the most recent bit, which is the convention that makes
 *    the octal polynomials read directly against it. Get this backwards and the code still decodes
 *    ITSELF perfectly while failing on every real signal — which is exactly the sort of bug a
 *    self-consistent test would bless, so the test below also checks a KNOWN encoder output. */
inline int branchOutputs(int state, int in) {
    const unsigned reg = (unsigned(in) << (kK - 1)) | unsigned(state);
    int bits = 0;
    for (int j = 0; j < kRateOut; ++j) bits |= parity(reg & kPoly[j]) << j;
    return bits;
}

/** Encode `nbits` bits (LSB-first in `in`) to `nbits*4` output bits, flushing with 6 zeros.
 *  ★ Present for TESTING and for nothing else — a decoder with no encoder to check it against can
 *    only be verified on live RF, which is the slowest possible feedback loop. */
inline std::vector<uint8_t> convEncode(const uint8_t* in, size_t nbits) {
    std::vector<uint8_t> out;
    out.reserve((nbits + kK - 1) * kRateOut);
    int state = 0;
    auto step = [&](int bit) {
        const int o = branchOutputs(state, bit);
        for (int j = 0; j < kRateOut; ++j) out.push_back(uint8_t((o >> j) & 1));
        state = ((state >> 1) | (bit << (kK - 2))) & (kStates - 1);
    };
    for (size_t i = 0; i < nbits; ++i) step(in[i] & 1);
    for (int i = 0; i < kK - 1; ++i) step(0);        // flush to the all-zero state
    return out;
}

/** Soft-input Viterbi for the rate-1/4 mother code.
 *
 *  ★★★ SOFT IN, and that is worth ~2 dB over hard decisions — on a marginal multiplex the
 *      difference between audio and silence. Inputs are the signed soft values from dqpskSoft:
 *      POSITIVE means bit 0, negative means bit 1, magnitude is confidence. A punctured (not
 *      transmitted) bit is passed as 0, which is exactly right: zero confidence contributes
 *      nothing to either branch, so depuncturing needs no special case in the decoder at all.
 *
 *  ★★ Terminated, not tail-biting: the encoder flushes 6 zeros, so traceback starts at state 0
 *     unconditionally rather than having to guess the ending state.
 */
class Viterbi {
public:
    /** @param soft   nbits*4 soft values (0 for punctured positions)
     *  @param nbits  number of DATA bits, EXCLUDING the 6 flush bits */
    Viterbi() {
        /* ★ The transition outputs never change — 64 states x 2 inputs, computed once here rather
         *  than parity()-ing four polynomials inside the hot loop. */
        for (int st = 0; st < kStates; ++st)
            for (int in = 0; in < 2; ++in)
                branchOut_[st][in] = uint8_t(branchOutputs(st, in));
        for (int ns = 0; ns < kStates; ++ns) {
            const int in  = ns >> (kK - 2);
            const int st0 = (ns & ((kStates >> 1) - 1)) << 1;
            branchOut0_[ns] = branchOut_[st0][in];
        }
    }

    std::vector<uint8_t> decode(const int8_t* soft, size_t nbits) {
        const size_t steps = nbits + (kK - 1);
        /* ★★★ ONE BIT PER STATE, NOT ONE BYTE — AND IT IS ABOUT CACHE, NOT MEMORY.
         *  This used to store the whole predecessor state in a byte per state per step: 64 bytes
         *  a step, ~197 kB for a 3072-bit block, written once and read once with nothing else
         *  fitting alongside it. That does not stay in any cache on a Pi, so every step paid main
         *  memory twice.
         *  ★★ The predecessor does not need storing at all. Destination-side ACS gives it for
         *     free: state ns has exactly two predecessors, ((ns&31)<<1) and that | 1, so ONE bit
         *     says which won. And the decoded data bit is not stored either — it is ns >> 5, by
         *     the definition of the transition. 64 bits a step, 8 bytes, which does stay resident.
         *  ★ Sized once and reused across calls: assign() on every decode was zeroing a buffer
         *    that is entirely overwritten. */
        const size_t decStride = kStates / 8;                  // 8 bytes per step
        if (decBits_.size() < steps * decStride) decBits_.resize(steps * decStride);

        /* ★★ 16-BIT METRICS. int32 doubled the working set of the survivor arrays for range that
         *  renormalising every step makes unnecessary: branch metrics are 0..1016 and the spread
         *  between survivors is bounded by the constraint length, so uint16 cannot overflow here.
         *  Half the width is half the loads, and it is what a NEON kernel would need. */
        std::array<uint16_t, kStates> cur{}, nxt{};
        cur.fill(kInit16);
        cur[0] = 0;                                   // known start state

        for (size_t t = 0; t < steps; ++t) {
            const int8_t* s = soft + t * kRateOut;
            // ★ bm[o] = cost of a branch whose output bits are `o`, offset to stay non-negative.
            uint16_t bm[16];
            for (int o = 0; o < 16; ++o) {
                int32_t m = 0;
                for (int j = 0; j < kRateOut; ++j)
                    m += ((o >> j) & 1) ? int32_t(s[j]) : -int32_t(s[j]);
                bm[o] = uint16_t(m + 127 * kRateOut);
            }
            uint8_t* dec = decBits_.data() + t * decStride;
            uint16_t mn = 0xFFFF;
#if defined(VIBE_DAB_VITERBI_NEON)
            /* ★★★ THE ACS, EIGHT STATES AT A TIME.
             *
             *  ★★★ THE LOAD IS THE TRICK. Destination state ns takes its two predecessors from
             *      cur[2*(ns&31)] and cur[2*(ns&31)+1] — ADJACENT, and consecutive ns walk them in
             *      pairs. vld2q_u16 de-interleaves exactly that in one instruction: `ev` is every
             *      first predecessor, `od` every second. No shuffling, no gather.
             *
             *  ★★★ AND THE BRANCH METRIC LOOKUP IS A TABLE INSTRUCTION, NOT A GATHER. There are
             *      only sixteen possible branch metrics per step, so the table fits a NEON vector
             *      register — vqtbl1_u8 looks up eight of them at once. They are 16-bit, so the
             *      table is split into low and high bytes and recombined; two table lookups and a
             *      widen replace eight dependent loads. This is the whole reason the metric table
             *      was built per step in the scalar version: it was already the shape a table
             *      instruction wants.
             *
             *  ★★ The second metric is still free — bm1 = 2*127*kRateOut - bm0 — because all four
             *     polynomials have bit 0 set, so the two predecessors emit complementary bits.
             *  ★ The scalar path below is kept and is not dead code: it is what armeabi-v7a and
             *    x86 build, and it is what the tests compare against on this Mac. */
            const uint8x8_t kWeights = { 1, 2, 4, 8, 16, 32, 64, 128 };
            uint8_t bmLo[16], bmHi[16];
            for (int o = 0; o < 16; ++o) { bmLo[o] = uint8_t(bm[o] & 0xFF); bmHi[o] = uint8_t(bm[o] >> 8); }
            const uint8x16_t tLo = vld1q_u8(bmLo), tHi = vld1q_u8(bmHi);
            const uint16x8_t kMirror = vdupq_n_u16(uint16_t(2 * 127 * kRateOut));
            uint16x8_t mnv = vdupq_n_u16(0xFFFF);
            for (int j = 0; j < 8; ++j) {
                const int ns0  = j * 8;
                const int base = (ns0 & ((kStates >> 1) - 1)) << 1;
                const uint16x8x2_t pr = vld2q_u16(&cur[size_t(base)]);   // ev = st0, od = st1
                const uint8x8_t  idx = vld1_u8(&branchOut0_[ns0]);
                const uint16x8_t b0  = vorrq_u16(vmovl_u8(vqtbl1_u8(tLo, idx)),
                                                 vshlq_n_u16(vmovl_u8(vqtbl1_u8(tHi, idx)), 8));
                const uint16x8_t c0  = vaddq_u16(pr.val[0], b0);
                const uint16x8_t c1  = vaddq_u16(pr.val[1], vsubq_u16(kMirror, b0));
                const uint16x8_t win = vcltq_u16(c1, c0);
                const uint16x8_t best = vminq_u16(c0, c1);
                vst1q_u16(&nxt[size_t(ns0)], best);
                mnv = vminq_u16(mnv, best);
                dec[j] = vaddv_u8(vand_u8(vmovn_u16(win), kWeights));
            }
            mn = vminvq_u16(mnv);
            /* ★ Renormalise, also vectorised — 64 subtractions is not worth a scalar loop beside
             *  a kernel this shape. */
            {
                const uint16x8_t mv = vdupq_n_u16(mn);
                for (int i = 0; i < kStates; i += 8)
                    vst1q_u16(&cur[size_t(i)], vsubq_u16(vld1q_u16(&nxt[size_t(i)]), mv));
            }
            continue;
#elif defined(VIBE_DAB_VITERBI_SSE)
            /* ★★★ THE SAME KERNEL FOR x86, AND IT IS THE SAME THREE IDEAS.
             *  The servers are not all ARM: the OpenWebRX box and any desktop VibeServer are
             *  x86_64, and leaving them on the scalar path would mean the machine most likely to
             *  carry several radios at once is the one running the slowest decoder.
             *
             *  ★★ WHERE IT DIFFERS FROM NEON, AND WHY:
             *   · There is no vld2q_u16. The de-interleave of adjacent predecessor pairs is done
             *     with two loads and a pack — _mm_shufflelo/hi + _mm_packs is the usual dance, but
             *     a shuffle-and-blend of 32-bit lanes is shorter here and needs no saturation.
             *   · pshufb (SSSE3, 2006) is the table lookup. Under bare SSE2 it does not exist, so
             *     the metrics are gathered scalar into a buffer and only the arithmetic is
             *     vectorised — still most of the win, and it keeps genuinely old hardware working
             *     rather than excluding it.
             *   · _mm_min_epu16 is SSE4.1, so the signed _mm_min_epi16 is used instead. Safe:
             *     metrics are renormalised every step and never approach 0x8000.
             *  ★ Verified against the NEON and scalar paths by test-dab-fec's known-encoder check,
             *    which is exactly what makes three implementations of one loop tolerable. */
            {
                uint16_t bmv[kStates];
                for (int ns = 0; ns < kStates; ++ns) bmv[ns] = bm[branchOut0_[ns]];
                const __m128i kMirror = _mm_set1_epi16(short(2 * 127 * kRateOut));
                __m128i mnv = _mm_set1_epi16(short(0x7FFF));
                for (int j = 0; j < 8; ++j) {
                    const int ns0  = j * 8;
                    const int base = (ns0 & ((kStates >> 1) - 1)) << 1;
                    // ★ De-interleave the 16 adjacent survivors into even (st0) and odd (st1).
                    const __m128i a = _mm_loadu_si128((const __m128i*)&cur[size_t(base)]);
                    const __m128i b = _mm_loadu_si128((const __m128i*)&cur[size_t(base) + 8]);
                    const __m128i mask = _mm_set1_epi32(0x0000FFFF);
                    const __m128i ev = _mm_packs_epi32(_mm_and_si128(a, mask), _mm_and_si128(b, mask));
                    const __m128i od = _mm_packs_epi32(_mm_srli_epi32(a, 16), _mm_srli_epi32(b, 16));
                    const __m128i b0 = _mm_loadu_si128((const __m128i*)&bmv[size_t(ns0)]);
                    const __m128i c0 = _mm_add_epi16(ev, b0);
                    const __m128i c1 = _mm_add_epi16(od, _mm_sub_epi16(kMirror, b0));
                    const __m128i win = _mm_cmpgt_epi16(c0, c1);          // c1 < c0
                    const __m128i best = _mm_min_epi16(c0, c1);
                    _mm_storeu_si128((__m128i*)&nxt[size_t(ns0)], best);
                    mnv = _mm_min_epi16(mnv, best);
                    dec[j] = uint8_t(_mm_movemask_epi8(_mm_packs_epi16(win, win)) & 0xFF);
                }
                // ★ Horizontal minimum, by folding the vector in half three times.
                mnv = _mm_min_epi16(mnv, _mm_shuffle_epi32(mnv, _MM_SHUFFLE(1, 0, 3, 2)));
                mnv = _mm_min_epi16(mnv, _mm_shuffle_epi32(mnv, _MM_SHUFFLE(2, 3, 0, 1)));
                mnv = _mm_min_epi16(mnv, _mm_shufflelo_epi16(mnv, _MM_SHUFFLE(2, 3, 0, 1)));
                mn = uint16_t(_mm_extract_epi16(mnv, 0));
                const __m128i mv = _mm_set1_epi16(short(mn));
                for (int i = 0; i < kStates; i += 8)
                    _mm_storeu_si128((__m128i*)&cur[size_t(i)],
                                     _mm_sub_epi16(_mm_loadu_si128((const __m128i*)&nxt[size_t(i)]), mv));
            }
            continue;
#endif
            for (int half = 0; half < 8; ++half) {
                uint8_t bits = 0;
                for (int k = 0; k < 8; ++k) {
                    const int ns  = half * 8 + k;
                    const int st0 = (ns & ((kStates >> 1) - 1)) << 1;
                    /* ★★★ THE SECOND BRANCH METRIC IS THE FIRST ONE MIRRORED, so only one lookup
                     *  is needed. All four generator polynomials (0133, 0171, 0145, 0133) have
                     *  bit 0 set, so flipping the OLDEST state bit — which is exactly what
                     *  distinguishes the two predecessors — flips every output bit. Hence
                     *  o1 = o0 ^ 15, and negating all four soft contributions turns bm into
                     *  (2 * 127 * kRateOut) - bm. */
                    const uint16_t b0 = bm[branchOut0_[ns]];
                    const uint16_t b1 = uint16_t(2 * 127 * kRateOut - b0);
                    const uint32_t c0 = uint32_t(cur[st0])     + b0;
                    const uint32_t c1 = uint32_t(cur[st0 | 1]) + b1;
                    const bool win1 = c1 < c0;
                    const uint32_t c = win1 ? c1 : c0;
                    nxt[ns] = uint16_t(c > 0xFFFF ? 0xFFFF : c);
                    bits |= uint8_t(win1) << k;
                    if (nxt[ns] < mn) mn = nxt[ns];
                }
                dec[half] = bits;
            }
            // ★ Renormalise so the metrics cannot run away over a long block. Subtracting a
            //   constant from every survivor changes no comparison.
            for (int st = 0; st < kStates; ++st) cur[st] = uint16_t(nxt[st] - mn);
        }

        /* ── Traceback from the known all-zero end state ──────────────────────────────────
         *  ★ The data bit is `st >> 5` because ns = (st >> 1) | (in << 5) — the input bit ends up
         *    as the top bit of the state it produced. Nothing needs to have stored it. */
        std::vector<uint8_t> out(nbits, 0);
        int st = 0;
        for (size_t t = steps; t-- > 0; ) {
            const int b = (decBits_[t * decStride + size_t(st >> 3)] >> (st & 7)) & 1;
            if (t < nbits) out[t] = uint8_t(st >> (kK - 2));
            st = (((st & ((kStates >> 1) - 1)) << 1) | b) & (kStates - 1);
        }
        return out;
    }

private:
    static constexpr int32_t kBig = 1 << 28;
    uint8_t  branchOut_[kStates][2] = {};
    /** ★ Output bits of the transition into state ns from its FIRST predecessor, precomputed. */
    uint8_t  branchOut0_[kStates] = {};
    static constexpr uint16_t kInit16 = 30000;   ///< "unreachable" without overflowing on +1016
    std::vector<uint8_t> decBits_;
    std::vector<uint8_t> decisions_;
};

// ── Energy dispersal ────────────────────────────────────────────────────────
/** ★★ THE SCRAMBLER, and an honest note about its provenance: the polynomial below could NOT be
 *  extracted from the downloaded PDF — clause 4.6's equations render as glyphs, not text — so it
 *  comes from the value every open implementation uses (x^9 + x^5 + 1, register preset to all
 *  ones) rather than from the standard in front of me.
 *  ★ The risk of that is low and SELF-DETECTING: a wrong scrambler means the FIB CRC never passes,
 *    ever, on any signal. It fails loudly and immediately rather than degrading, so it cannot ship
 *    unnoticed. ▶ Confirm it against the paper spec when convenient. */
class EnergyDispersal {
public:
    EnergyDispersal() { reset(); }
    void reset() { reg_ = 0x1FF; }               // all nine bits set
    /** XOR `n` bits in place with the PRBS. */
    void apply(uint8_t* bits, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const unsigned fb = ((reg_ >> 8) ^ (reg_ >> 4)) & 1u;   // x^9 + x^5 + 1
            reg_ = ((reg_ << 1) | fb) & 0x1FFu;
            bits[i] ^= uint8_t(fb);
        }
    }
private:
    unsigned reg_ = 0x1FF;
};

// ── FIB CRC ─────────────────────────────────────────────────────────────────
/** CRC-16-CCITT as DAB uses it: polynomial 0x1021, preset all ones, complemented before
 *  transmission — the same generator TS 102 563 names for the superframe header.
 *  @return true when the 32-byte FIB checks out. */
inline uint16_t crc16(const uint8_t* data, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

/** A FIB is 32 bytes: 30 of payload then a 2-byte complemented CRC over those 30. */
inline bool fibCrcOk(const uint8_t* fib32) {
    const uint16_t want = uint16_t(~crc16(fib32, 30));
    const uint16_t got  = uint16_t((fib32[30] << 8) | fib32[31]);
    return want == got;
}

}  // namespace vibedab
