// vibe_dab_punct.h — puncturing vectors and depuncturing (EN 300 401 clause 11.1.2, table 13).
//
// ★★★ DEPUNCTURING IS AN INSERTION, NOT A DECISION. The transmitter drops code bits according to
//     a 32-bit vector; we put them back as soft value ZERO — "no opinion" — and the Viterbi is
//     already built to treat that correctly (see vibe_dab_fec.h). So there is no special decoder
//     mode, no separate rate table inside the decoder, and no chance of the two disagreeing about
//     which rate is in force. One mechanism, used by every protection level.
//
// ★★ THE VECTORS ARE MACHINE-EXTRACTED FROM THE SPEC, like tables 23/24. Twenty-four 32-bit
//    patterns is a transcription that WILL go wrong by hand, and a wrong vector shifts every
//    subsequent bit — so the decode does not degrade, it produces noise, for one protection level
//    only. That is a miserable bug to find on air: most stations work, one does not.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vibedab {

// GENERATED from ETSI EN 300 401 V2.1.1 table 13 by machine extraction — not hand-typed.
inline constexpr uint32_t kPunctureVec[25] = {
    0u,   // PI 0 unused
    0xC8888888u,   // PI  1  rate 8/9  11001000 10001000 10001000 10001000
    0xC888C888u,   // PI  2  rate 8/10  11001000 10001000 11001000 10001000
    0xC8C8C888u,   // PI  3  rate 8/11  11001000 11001000 11001000 10001000
    0xC8C8C8C8u,   // PI  4  rate 8/12  11001000 11001000 11001000 11001000
    0xCCC8C8C8u,   // PI  5  rate 8/13  11001100 11001000 11001000 11001000
    0xCCC8CCC8u,   // PI  6  rate 8/14  11001100 11001000 11001100 11001000
    0xCCCCCCC8u,   // PI  7  rate 8/15  11001100 11001100 11001100 11001000
    0xCCCCCCCCu,   // PI  8  rate 8/16  11001100 11001100 11001100 11001100
    0xECCCCCCCu,   // PI  9  rate 8/17  11101100 11001100 11001100 11001100
    0xECCCECCCu,   // PI 10  rate 8/18  11101100 11001100 11101100 11001100
    0xECECECCCu,   // PI 11  rate 8/19  11101100 11101100 11101100 11001100
    0xECECECECu,   // PI 12  rate 8/20  11101100 11101100 11101100 11101100
    0xEEECECECu,   // PI 13  rate 8/21  11101110 11101100 11101100 11101100
    0xEEECEEECu,   // PI 14  rate 8/22  11101110 11101100 11101110 11101100
    0xEEEEEEECu,   // PI 15  rate 8/23  11101110 11101110 11101110 11101100
    0xEEEEEEEEu,   // PI 16  rate 8/24  11101110 11101110 11101110 11101110
    0xFEEEEEEEu,   // PI 17  rate 8/25  11111110 11101110 11101110 11101110
    0xFEEEFEEEu,   // PI 18  rate 8/26  11111110 11101110 11111110 11101110
    0xFEFEFEEEu,   // PI 19  rate 8/27  11111110 11111110 11111110 11101110
    0xFEFEFEFEu,   // PI 20  rate 8/28  11111110 11111110 11111110 11111110
    0xFFFEFEFEu,   // PI 21  rate 8/29  11111111 11111110 11111110 11111110
    0xFFFEFFFEu,   // PI 22  rate 8/30  11111111 11111110 11111111 11111110
    0xFFFFFFFEu,   // PI 23  rate 8/31  11111111 11111111 11111111 11111110
    0xFFFFFFFFu,   // PI 24  rate 8/32  11111111 11111111 11111111 11111111
};

/** Bit i (0..31) of vector PI: 1 = transmitted, 0 = punctured.
 *  ★ Stored MSB-first so the hex reads in the same order the spec prints the bits. */
inline constexpr bool punctured(int pi, int i) {
    return ((kPunctureVec[pi] >> (31 - i)) & 1u) == 0u;
}

/** How many of the 32 mother-code bits survive one sub-block at this PI. */
inline constexpr int keptPerBlock(int pi) {
    int n = 0;
    for (int i = 0; i < 32; ++i) if (!punctured(pi, i)) ++n;
    return n;
}

/** Expand received soft values back onto the mother code.
 *
 *  @param rx        the soft values actually received
 *  @param nRx       how many
 *  @param pi        puncturing index for these sub-blocks
 *  @param out       mother-code soft values, punctured positions set to 0
 *  @param outCap    capacity of `out`
 *  @return number of mother-code positions written
 */
inline size_t depuncture(const int8_t* rx, size_t nRx, int pi, int8_t* out, size_t outCap) {
    if (pi < 1 || pi > 24 || !rx || !out) return 0;
    size_t r = 0, w = 0;
    while (r < nRx && w + 32 <= outCap) {
        for (int i = 0; i < 32; ++i) {
            if (punctured(pi, i)) out[w++] = 0;              // no opinion
            else                  out[w++] = r < nRx ? rx[r++] : 0;
        }
    }
    return w;
}

}  // namespace vibedab
