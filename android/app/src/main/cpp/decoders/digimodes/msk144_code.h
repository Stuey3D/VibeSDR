// decoders/digimodes/msk144_code.h — THE MSK144 FRAME AND CODE: our own, clean room (V12, 2026-10-31).
//
// ★★★ HOW THIS WAS LEARNT (as Q65 and FST4): no WSJT-X source read. The shipped utility `msk144code` (WSJT-X 2.7.0, run
//     as a black box in a container) prints each message's 144 "channel symbols". 400 messages were put through it —
//     272 telemetry words (200 random, 71 with one bit set, the zero word: near-arbitrary bit patterns) and 128
//     standard / non-standard / free-text messages — and the structure solved from that output (scratch Python, not
//     shipped; docs/v12/DECODERS-WAVE2.md "MSK144"):
//       • the printed symbols are the TONES (1 = the upper, 2000 Hz; 0 = 1000 Hz — checked by synthesising audio and
//         having jt9 decode it). Tone i = C[i] XOR C[i+1 mod 144], where C = the frame bits XOR the period-4 pattern
//         0011 0011 … — i.e. the frame is sent as offset-QPSK with half-sine pulses (MSK), each frame bit the sign
//         of one half-sine on I (even bits) or Q (odd bits), so the receiver reads it as coherent BPSK;
//       • the frame: sync 0x72 (01110010) at bits 0–7 and again at 56–63; the 128-bit codeword in bits 8–55, 64–143;
//       • the codeword: systematic — the 77-bit FT8-family payload (NOT scrambled), a CRC-13, 38 parity bits;
//       • CRC-13: polynomial x¹³+x¹²+x¹⁰+x⁸+x⁷+x⁶+x⁴+x²+x+1 (0x15D7), register 0, MSB first over the 77 bits followed by
//         six zeros (the 83-bit form — all 400 printed CRCs);
//       • the (128,90) LDPC code: 38 parity checks found as the sparse vectors of the dual of the observed codewords
//         (an information-set search found exactly 38, of weight 10–11, every column weight 3; rank 38; together
//         with the CRC they span the whole dual): msk144_tables.h. All 400 black-box vectors re-encode exactly.
//     Frame timing: 2000 bit/s, 144 bits = 72 ms, repeated without a gap for the whole transmission.
// ★ MSK40 (the 40-bit "short message" of hashed calls and a report, sent mid-QSO) is NOT decoded: it carries no
//   callsign a spot could use. Stated, not hidden.
#pragma once
#include <cstdint>
#include <string>

namespace vibe {
namespace ldpc { class Code; }
namespace msk144 {

constexpr int kFrameBits = 144, kN = 128, kK = 90, kMsg = 77;
constexpr int kBaud = 2000;
constexpr uint8_t kSync = 0x72;

/** Frame bit position of codeword bit j (0 … 127). */
inline int framePos(int j) { return j < 48 ? 8 + j : 64 + (j - 48); }
/** Frame bit i is a sync bit; its value. */
inline bool isSync(int i) { return i < 8 || (i >= 56 && i < 64); }
inline int syncBit(int i) { const int k = i < 8 ? i : i - 56; return (kSync >> (7 - k)) & 1; }

/** CRC-13 of 77 message bits (0/1 values). */
uint16_t crc13(const uint8_t* bits77);
/** The (128,90) code. */
const ldpc::Code& code();
/** 77 payload bits → the 128-bit codeword. */
void encode(const uint8_t* bits77, uint8_t cw[kN]);
/** Codeword → the 144 frame bits (sync included). */
void frameBits(const uint8_t cw[kN], uint8_t frame[kFrameBits]);
/** Codeword → the 144 tones (0 = 1000 Hz, 1 = 2000 Hz in WSJT-X's 1500 Hz-centred audio). */
void tones(const uint8_t cw[kN], uint8_t t[kFrameBits]);
/** Message text → tones (ft8_lib's packer). */
bool encodeText(const char* text, uint8_t t[kFrameBits]);
/** The CRC of a codeword holds. */
bool crcOk(const uint8_t cw[kN]);
/** Unpack (as fst4::unpack77, without FST4's scrambling). `plausible` = ft8OsdPlausible's judgement. */
bool unpack(const uint8_t cw[kN], bool learn, std::string& text, std::string& call, std::string& grid, bool& plausible);

} // namespace msk144
} // namespace vibe
