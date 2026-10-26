// VibeSDR V12 — JS8's forward error correction: LDPC(174,87) and CRC-12 (2026-10-26).
//
// ★★ THE CODE. JS8 keeps the FIRST FT8's code (WSJT-X 1.8/1.9, 2017-18), not today's: 87 information bits (72 of
//  message, 3 of frame flags, 12 of CRC) and 87 parity bits — LDPC(174,87), every bit in 3 checks, every check over
//  5-7 bits. On the air the PARITY goes first (channel symbols 7-35) and the information after (43-71), three bits a
//  symbol, MSB first, with NO Gray map (tone = the three bits).
//  ★ The parity-check matrix below is the one table that cannot be derived from anything: it IS the code. It comes
//    from ft8_lib's early history (Kārlis Goba, MIT licence — the same library and licence as ft8_lib/), commit
//    6eaf85f^ "ft8/arrays.h", converted to 0-origin; the decoder's other tables (each bit's three checks, the
//    generator) are BUILT from it at start-up, so they cannot disagree with it. Checked on real JS8 off the air
//    (test-js8, the recordings in ~/VibeSDR-test-audio/js8/).
// ★ CRC-12: polynomial 0xC06 (x^12 + x^11 + x^10 + x^2 + x + 1 … the top bit implied), the 75 message+flag bits
//    followed by 13 zero bits divided through it MSB first, the remainder XOR 42 — JS8's own twist, so the original
//    FT8 decoders never took a JS8 frame for theirs.
#pragma once
#include <cstdint>

namespace vibe {
namespace js8 {

constexpr int kN = 174;     ///< codeword bits
constexpr int kK = 87;      ///< information bits (72 message + 3 flags + 12 CRC)
constexpr int kM = 87;      ///< parity bits / checks

/** CRC-12 of the first 75 bits of `bits` (one bit per byte, MSB-first order), JS8's (see above). */
uint16_t crc12(const uint8_t* bits75);

/** Encode 87 information bits (one per byte) into the 174-bit codeword in ON-AIR order: 87 parity, then the 87. */
void encode(const uint8_t* info87, uint8_t* cw174);

/** Sum-product belief propagation. `llr` = log p(1)/p(0) per codeword bit (on-air order). On success writes the
 *  codeword's hard bits to `cw174` and returns how many of them disagree with the sign of `llr` (≥ 0); -1 when no
 *  codeword was reached within `maxIters`. */
int bpDecode(const float* llr, int maxIters, uint8_t* cw174);

/** OSD order 1 (on a failed BP): the 87 most reliable independent bits, re-encoded, every single flip among them;
 *  the closest codeword whose CRC-12 checks, if it is at most `maxErrors` hard decisions away. Returns those errors
 *  or -1. */
int osd1(const float* llr, int maxErrors, uint8_t* cw174);

/** The parity-check matrix, for the tests: check m covers bits nm(m)[0..deg(m)-1]. */
int checkDegree(int m);
const uint8_t* checkBits(int m);

} // namespace js8
} // namespace vibe
