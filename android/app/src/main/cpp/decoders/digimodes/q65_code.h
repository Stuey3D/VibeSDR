// decoders/digimodes/q65_code.h — THE Q65 FRAME AND ITS CODE: our own, clean-room (V12, 2026-10-31).
//
// ★★★ HOW THIS WAS LEARNT — CLEAN ROOM (Stuart, 2026-10-31: "if you can get what you need whilst keeping us clean then
//     that's fine"). No WSJT-X source was read. The official WSJT-X 2.7.0 release was downloaded into a directory of its
//     own and its SHIPPED ENCODER UTILITY `q65code` run as a black box: it prints, for a message, the 90 bits of message +
//     CRC, the 65-symbol codeword and the 85 channel tones. 265 messages (160 random 71-bit telemetry words, 60 standard
//     QSO messages, 30 free texts and 15 of the other message types) were fed through it and the structure SOLVED from
//     its output by linear algebra over GF(2) (docs/v12/DECODERS-WAVE2.md "Q65" has the method):
//       • the 77-bit message is the FT8-family payload (the same packing, ft8_lib's), plus one 0 bit → 78 bits →
//         13 six-bit symbols, MSB first;
//       • a CRC-12, polynomial x¹²+x¹¹+x³+x²+x+1 (0x80F), register 0, fed the 13 symbols IN ORDER and each symbol's six
//         bits LEAST significant first (reflected: register shifted right, 0xF01) — its low six bits are info symbol 13,
//         its high six bits symbol 14 → 15 information symbols over GF(64);
//       • a systematic REPEAT-ACCUMULATE code over GF(64), field polynomial x⁶+x+1: 50 parity symbols, each the previous
//         one plus a weighted information symbol, p[k] = p[k−1] + w[k]·x[j[k]] (p[−1] = 0) — the (j, w) of every step
//         below; codeword = x[0..14], p[0..49] (65 symbols);
//       • the two CRC symbols are NOT sent (punctured): 63 data symbols, each as tone (symbol + 1);
//       • 22 SYNC symbols, tone 0, at the fixed positions below → 85 channel symbols.
//     Every one of the 265 black-box codewords and tone sequences is reproduced exactly by this file (test-q65 pins a
//     set of them). The timing (symbol lengths per period, tone spacing per submode, the nominal start) was measured
//     the same way from the shipped simulator's audio (`q65sim`), and agrees with the published description of the
//     protocol (Q65's announcement and the WSJT-X user guide: 65-FSK, periods 15–300 s, submodes A–E spacing ×1…×16).
// ★ The decoder side (belief propagation over GF(64) in the Walsh-Hadamard domain) is from the general published
//   theory of non-binary LDPC/RA decoding, written here for THIS graph — a chain (the accumulator) with the information
//   symbols hanging off it, decoded forward-backward along the chain every iteration.
#pragma once
#include <cstdint>
#include <string>

namespace vibe {
namespace q65 {

constexpr int kSymbols   = 85;   ///< channel symbols in a transmission
constexpr int kSync      = 22;   ///< of which sync (tone 0)
constexpr int kData      = 63;   ///< and data (tones 1..64)
constexpr int kInfo      = 15;   ///< information symbols: 13 message + 2 CRC
constexpr int kMsg       = 13;   ///< message symbols (78 bits: the 77-bit payload and a 0)
constexpr int kParity    = 50;
constexpr int kCodeword  = 65;   ///< info + parity
constexpr int kQ         = 64;   ///< GF(64)

extern const uint8_t kSyncPos[kSync];     ///< channel positions of the sync symbols, ascending
extern const uint8_t kStepInfo[kParity];  ///< parity step k adds information symbol kStepInfo[k] …
extern const uint8_t kStepW[kParity];     ///< … times this GF(64) weight

/** GF(64), polynomial x⁶+x+1. */
uint8_t gfMul(uint8_t a, uint8_t b);
uint8_t gfInv(uint8_t a);

/** CRC-12 over the 13 message symbols: [0] = info symbol 13 (low six bits), [1] = info symbol 14 (high six). */
void crc12(const uint8_t msg13[kMsg], uint8_t out2[2]);
/** 77-bit payload (ft8_lib's ftx_message_t::payload, 10 bytes MSB first) → the 13 message symbols. */
void payloadToSymbols(const uint8_t payload10[10], uint8_t msg13[kMsg]);
/** … and back (false if the 78th bit, always 0 on air, is 1). */
bool symbolsToPayload(const uint8_t msg13[kMsg], uint8_t payload10[10]);
/** 15 info symbols (13 message + CRC) → the 65-symbol codeword. */
void encodeInfo(const uint8_t info15[kInfo], uint8_t cw65[kCodeword]);
/** The 13 message symbols → the 85 channel tones (0 = sync, 1..64 = data). */
void encodeTones(const uint8_t msg13[kMsg], uint8_t tones85[kSymbols]);
/** A message's text → its 85 tones, through ft8_lib's packer (calls hashed into the shared table). False = not packable. */
bool encodeText(const char* text, uint8_t tones85[kSymbols]);
/** Codeword index of data symbol d (0..62): the punctured CRC symbols (13, 14) are skipped. */
inline int dataToCodeword(int d) { return d < kMsg ? d : d + 2; }
/** True if channel position i is a sync symbol. */
bool isSync(int i);

/* ★★ THE DECODER: belief propagation over GF(64) on the code's graph. `intr` = kData rows of kQ probabilities (any
 *  positive scale), one per DATA symbol in channel order: row d is codeword symbol dataToCodeword(d), entry v its
 *  probability of carrying symbol v (tone v + 1). `apMask`/`apVal` (optional, kMsg each): a-priori bits per message
 *  symbol — bits set in apMask are KNOWN to equal those of apVal. Bit 77 (the low bit of symbol 12) is always known 0.
 *  Returns the iterations used (≥ 1) and the 13 message symbols when the CRC of the decision matches, else 0. */
struct BpScratch;
BpScratch* bpScratchNew();
void       bpScratchFree(BpScratch*);
int  bpDecode(const float* intr /* kData × kQ */, int maxIter, uint8_t msgOut[kMsg], BpScratch* s,
              const uint8_t* apMask = nullptr, const uint8_t* apVal = nullptr);

/** 64-point Walsh-Hadamard transform in place (unnormalised), SIMD where built. Exposed for the tests and the bench. */
void wht64(float* v);

} // namespace q65
} // namespace vibe

namespace vibe {
namespace q65 {
/** Unpack a decoded payload: the message text, the SENDER's call (empty if none or an unresolved hash) and the
 *  sender's own 4-character grid ("" if the third field is a report). `learn`: put its full calls in the shared hash
 *  table (blind decodes only — an a-priori decode never teaches). `plausible`: a message people send (the FT8
 *  family's rules — standard messages with real calls and a real grid or report; any other type is "not plausible"
 *  and needs the stricter guard). False = it does not unpack. */
bool unpackPayload(const uint8_t payload10[10], bool learn, std::string& text, std::string& call, std::string& grid,
                   bool& plausible);
/** "CQ ? ?": the known bits of any standard CQ (the CQ token, its ipa bit, i3 = 1) as masks over the 13 symbols. */
void cqApMask(uint8_t mask13[kMsg], uint8_t val13[kMsg]);
} // namespace q65
} // namespace vibe
