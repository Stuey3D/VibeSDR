// vibe_dab_ficdec.h — the complete FIC decode path: soft bits in, ensemble out.
//
// This is the piece that turns everything below it into A STATION LIST, with no audio decoded at
// all — the milestone BRIEF-dab.md picks deliberately, because it proves the null detector, the
// OFDM demapping, the interleaving, the depuncturing, the Viterbi, the scrambler and the CRC all
// at once, and needs no codec to do it.
//
// EN 300 401, Mode I:
//   · 11.2.1 — a 768-bit vector is encoded to a 3096-bit mother codeword (768*4 + 24 tail);
//     the first 3072 bits are 24 blocks of 128, the first 21 punctured at PI=16 and the last 3 at
//     PI=15; the final 24 tail bits use V_T = 1100 1100 1100 1100 1100 1100, keeping 12.
//     21*4*24 + 3*4*23 + 12 = 2016 + 276 + 12 = 2304 — the codeword length the spec states.
//   · 14.4.1.1 — FOUR such codewords are multiplexed bit-by-bit into one vector and carried on
//     three OFDM symbols: 3 x 1536 carriers x 2 bits = 9216 = 4 x 2304. The interleave is
//     b'(4i + r mod 4) = b(r, i), i.e. plain round-robin between the four codewords.
//
// ★★ 768 decoded bits = 96 bytes = THREE FIBs, and four codewords per frame gives the 12 FIBs a
//    Mode I frame carries. Each FIB is independently CRC-checked, so one bad codeword costs three
//    FIBs and nothing else — which is what keeps the station list from flapping.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "vibe_dab_fec.h"
#include "vibe_dab_fic.h"
#include "vibe_dab_punct.h"

namespace vibedab {

inline constexpr int kFicCodewordBits = 2304;   ///< punctured, as received
inline constexpr int kFicMotherBits   = 3096;   ///< 768*4 + 24
inline constexpr int kFicDataBits     = 768;    ///< 3 FIBs
inline constexpr uint32_t kTailVec    = 0xCCCCCCu;  ///< V_T, 24 bits: 1100 x6 (MSB first)

/** Tail bit i (0..23): true when punctured (not transmitted). */
inline constexpr bool tailPunctured(int i) { return ((kTailVec >> (23 - i)) & 1u) == 0u; }

/** Expand one received FIC codeword (2304 soft values) onto the 3096-bit mother code. */
inline void ficDepuncture(const int8_t* rx, int8_t* mother) {
    size_t r = 0, w = 0;
    // 21 blocks of 128 mother bits at PI = 16 …
    for (int blk = 0; blk < 21; ++blk)
        for (int sub = 0; sub < 4; ++sub)
            for (int i = 0; i < 32; ++i)
                mother[w++] = punctured(16, i) ? int8_t(0) : rx[r++];
    // … then 3 blocks at PI = 15 …
    for (int blk = 0; blk < 3; ++blk)
        for (int sub = 0; sub < 4; ++sub)
            for (int i = 0; i < 32; ++i)
                mother[w++] = punctured(15, i) ? int8_t(0) : rx[r++];
    // … then the 24 tail bits.
    for (int i = 0; i < 24; ++i) mother[w++] = tailPunctured(i) ? int8_t(0) : rx[r++];
}

/** Decode one FIC codeword into 3 FIBs and merge them into `e`.
 *  @return how many of the three FIBs passed their CRC (0..3) — the honest error figure for the
 *          DX panel, updated 4 times a frame and therefore ~50 times a second. */
inline int ficDecodeCodeword(const int8_t* rx2304, Ensemble& e, Viterbi& v) {
    std::vector<int8_t> mother(kFicMotherBits);
    ficDepuncture(rx2304, mother.data());

    std::vector<uint8_t> bits = v.decode(mother.data(), kFicDataBits);

    // ★ The scrambler runs over the whole 768-bit vector, not per FIB — reset once, here.
    EnergyDispersal ed;
    ed.apply(bits.data(), bits.size());

    uint8_t fib[32];
    int ok = 0;
    for (int f = 0; f < 3; ++f) {
        for (int b = 0; b < 32; ++b) {
            uint8_t byte = 0;
            for (int k = 0; k < 8; ++k) byte = uint8_t((byte << 1) | (bits[size_t(f) * 256 + size_t(b) * 8 + size_t(k)] & 1));
            fib[b] = byte;
        }
        if (parseFib(fib, e)) ++ok;
    }
    return ok;
}

/** De-multiplex the four codewords a Mode I frame carries.
 *  ★ b'(4i + r) = b(r, i): the four codewords are round-robin interleaved, so codeword r is every
 *    fourth soft value starting at r. Getting the phase wrong here decodes four codewords of
 *    nonsense while every stage below reports itself healthy. */
inline void ficDemux(const int8_t* frame9216, int cw, int8_t* out2304) {
    for (int i = 0; i < kFicCodewordBits; ++i) out2304[i] = frame9216[4 * i + cw];
}

/** Whole-frame convenience: 9216 soft values (3 OFDM symbols' worth) -> ensemble updates.
 *  @return total FIBs that passed CRC this frame, out of 12. */
inline int ficDecodeFrame(const int8_t* frame9216, Ensemble& e, Viterbi& v) {
    std::vector<int8_t> cw(kFicCodewordBits);
    int ok = 0;
    for (int r = 0; r < 4; ++r) { ficDemux(frame9216, r, cw.data()); ok += ficDecodeCodeword(cw.data(), e, v); }
    return ok;
}

}  // namespace vibedab
