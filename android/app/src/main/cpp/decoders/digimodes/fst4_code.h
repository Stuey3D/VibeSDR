// decoders/digimodes/fst4_code.h — THE FST4 / FST4W FRAME AND CODES: our own, clean room (V12, 2026-10-31).
//
// ★★★ HOW THIS WAS LEARNT (as Q65, q65_code.h): no WSJT-X source read. The shipped simulator `fst4sim` was run as a
//     black box (it prints each message's bits, its CRC-24 and its 160 tones) on 177 FST4 and 236 FST4W messages, and
//     the structure solved from that output (docs/v12/DECODERS-WAVE2.md "FST4 / FST4W"):
//       • FST4: the 77-bit FT8-family payload XORed with FT4's published scrambling sequence (ft8_lib's
//         kFT4_XOR_sequence) — the bits fst4sim prints — then a CRC-24, polynomial x²⁴+x¹⁰+x⁹+x⁶+x⁴+x³+x+1 (0x100065B),
//         register 0, MSB first over those 77 bits → 101 bits → a systematic LDPC (240,101);
//       • FST4W: a 50-bit message — the FT8 family's 28-bit call and 15-bit grid, a 5-bit power INDEX (0, 3, 7, 10 …
//         60 dBm as 0 … 18) and 2 type bits (00 = call + grid + power) — not scrambled, the same CRC-24 → 74 bits → a
//         systematic LDPC (240,74);
//       • both codes' parity checks found as the sparse vectors of the dual of the observed codewords: (240,101) 139
//         checks of weight 5–6, (240,74) 166 of weight 4–5, every column weight 3 (fst4_tables.h);
//       • 120 data symbols, two bits each, Gray-mapped (00→0, 01→1, 11→2, 10→3, first bit first); 40 sync symbols in
//         five groups of eight at 0, 38, 76, 114, 152: A B A B A, A = 0 1 3 2 1 0 2 3, B = 2 3 1 0 3 2 0 1;
//       • 4-GFSK, the tones one baud apart; symbol lengths 720, 1680, 3888, 8200, 21504, 66560, 134400 samples at
//         12 kHz for the 15, 30, 60, 120, 300, 900, 1800 s periods; a transmission starts 0.5 s into a 15 s period and
//         1.0 s into a longer one (measured from fst4sim's audio; the published description agrees).
//     FST4W messages with a COMPOUND call (type bits ≠ 00) carry the base call's 28 bits and a prefix/suffix field
//     whose layout was not solved: they are decoded but not unpacked (a stated gap). A hashed-call FST4W message
//     ("<K1ABC> FN42AB 37") is sent as a 77-bit message with the (240,101) code — the receiver tries both codes.
#pragma once
#include <cstdint>
#include <string>

namespace vibe {
namespace fst4 {

constexpr int kSymbols = 160, kData = 120, kN = 240;
enum class Code { K101 /* FST4, and FST4W's 77-bit messages */, K74 /* FST4W */ };

/** True if channel symbol i is a sync symbol; its tone then is syncTone(i). */
bool isSync(int i);
int  syncTone(int i);
/** CRC-24 (0x100065B, register 0, MSB first) over `n` bits (0/1 values). */
uint32_t crc24(const uint8_t* bits, int n);
/** Message bits (77 for K101 — the scrambled payload as sent —, 50 for K74) → the 240-bit codeword (0/1 values). */
void encode(Code c, const uint8_t* msgBits, uint8_t cw[kN]);
/** Codeword → the 160 channel tones (0..3). */
void codewordToTones(const uint8_t cw[kN], uint8_t tones[kSymbols]);
/** FST4: message text → tones (ft8_lib's packer, then FT4's scrambling). */
bool encodeText(const char* text, uint8_t tones[kSymbols]);
/** FST4W type 1: call, 4-char grid, dBm (rounded to the nearest power the protocol has) → tones. */
bool encodeWspr(const char* call, const char* grid, int dbm, uint8_t tones[kSymbols]);

/* ★★ BELIEF PROPAGATION. `llr` = 240 log-likelihood ratios in codeword order, log P(1)/P(0). Returns the iterations
 *  used when every parity check holds AND the CRC-24 matches (cw filled), else 0. Sum-product in the log domain. */
int  bpDecode(Code c, const float llr[kN], int maxIter, uint8_t cw[kN]);
/** The CRC of a decoded codeword. */
bool crcOk(Code c, const uint8_t cw[kN]);

/** FST4 (K101): unpack. `learn` = put full calls in the shared hash table. `plausible` as Q65's. */
bool unpack77(const uint8_t cw[kN], bool learn, std::string& text, std::string& call, std::string& grid, bool& plausible);
/** FST4W (K74), type 1 only: "K1ABC FN42 37", the call, the grid and dBm. False = not a type-1 message we can unpack. */
bool unpack50(const uint8_t cw[kN], std::string& text, std::string& call, std::string& grid, int& dbm);

} // namespace fst4
} // namespace vibe
