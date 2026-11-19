// decoders/ale_decoder.h — ★ V12 2026-11-19: 2G HF ALE (MIL-STD-188-141, Appendix A — Automatic Link Establishment)
// on the listener's USB audio: soundings, calls and links read back into lines ("call  K1ABC → W1XYZ").
//
// ★ CLEAN ROOM (docs/v12/HF-DATA.md, "ALE — where every fact came from"):
//   • 8-FSK, tones 750 … 2500 Hz 250 Hz apart, 125 Bd (8 ms), three bits a tone, the tone index Gray-coded (750 = 000,
//     1000 = 001, 1250 = 011, 1500 = 010, …, 2500 = 100), the first-sent bit the heaviest — Wavecom's MIL-188-141A page
//     and NTIA TM-95-166 for the layer, the tone/bit facts as LinuxALE (GPL, Charles Brain G4GUO) implements them, READ
//     only.
//   • A 24-bit word: a 3-bit preamble (DATA 0, THRU 1, TO 2, TWAS 3, FROM 4, TIS 5, CMD 6, REP 7) and three 7-bit
//     characters. Each 12-bit half is extended-Golay (24,12) coded (data then 12 check bits; the second half's check bits
//     inverted), the two codewords interleaved bit by bit (A1 B1 A2 B2 …), a stuff bit added (49), and the 49 sent three
//     times: 147 bits, 49 tones, 392 ms (TM-95-166 §2: 2-of-3 majority, "unanimous votes" for word sync).
//   • Addresses use the 38 characters A–Z 0–9 @ ?; '@' pads. An address longer than three characters continues in DATA /
//     REP words (alternating); a CMD word whose characters are from the 64-character set (0x20–0x5F) starts an AMD text
//     message, continued the same way.
//   The Golay generator was taken from the code's own definition (the parity of each of the 12 unit data words); the
//   decoder (a syndrome table for every error of weight ≤ 3) is our own.
//
// ★ THE RECEIVER: 8 tone filters (an 8 ms window) at 3 audio offsets (−62.5, 0, +62.5 Hz: an SSB dial a little off) and 8
//   timings (1 ms apart), each (offset, timing) a 147-bit register; a word is taken when the three copies agree on enough
//   bits, both Golay halves decode with ≤ 1 error and the word is an address word in the 38-character set — and the next
//   word, 49 tones on, confirms it. Then one word every 392 ms until two are missed.
//
// ⚖ What is shown: addresses (who called whom, who sounded) — not message content. An AMD message's TEXT is content: the
//   engine hands the host both lines (onLine: public without the text, private with it) and the host decides by the
//   dial's band (amateur: shown) and the owner's opt-in (vibe_decoder_host.h).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class AleDecoder {
public:
    struct Word { int pre; char c[4]; int errors; int votesBad; float offsetHz; };
    explicit AleDecoder(double inRate);
    ~AleDecoder();
    void feed(const float* x, int n);
    /** a finished transmission: `pub` (addresses only; an AMD text replaced by its length), `priv` (with the AMD text;
     *  empty when there is none). Lines end with '\n'. */
    std::function<void(const std::string& pub, const std::string& priv)> onLine;
    std::function<void(const Word&)> onWord;            ///< every word as read (tests, debugging)
    std::function<void(int)> onState;                   ///< 0 searching · 1 a transmission in progress
    std::function<void(const std::string& json)> onStatus;   ///< {"words":n,"offset":hz,"last":"…"} on change
    /** UTC "HH:MM:SS" for a line (the host's clock; tests set it). Empty = the system clock. */
    std::function<std::string()> clock;

    static const char* preambleName(int p);   ///< "DATA", "THRU", "TO", "TWAS", "FROM", "TIS", "CMD", "REP"
    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

// ── the format's pieces, shared with the tests and the bench ─────────────────────────────────────────────────────────
uint32_t aleGolayEncode(uint32_t data12);                 ///< 24-bit codeword: data << 12 | check
int aleGolayDecode(uint32_t code24, uint32_t& data12);    ///< errors corrected (0–3), −1 uncorrectable
uint32_t aleWord(int pre, const char* three);             ///< 24-bit ALE word
/** test transmitter: the words, back to back, as audio at `fs` (unit amplitude), the tones shifted by `offsetHz` */
std::vector<float> aleSynth(const std::vector<uint32_t>& words, double fs, double offsetHz = 0);
/** a whole call: `rep` scanning-call words "TO <first 3 of to>", then the address in full, then optional AMD text, then
 *  "TIS <from>" (or TWAS); a sounding when `to` is empty */
std::vector<uint32_t> aleCallWords(const std::string& to, const std::string& from, int rep, bool twas = false,
                                   const std::string& amd = "");

} // namespace vibe
