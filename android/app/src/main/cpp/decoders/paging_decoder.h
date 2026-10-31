// decoders/paging_decoder.h — the PAGING receiver: ONE demodulator with an AUTO mode (V12, 2026-10-31).
//
// ★★★ SCOPE (Stuart's spec, docs/v12/DECODERS-WAVE2.md "PAGING"): one PAGE button, Android LOCAL HARDWARE ONLY (the
//     phone's own plugged-in radio) — never on any server, never to LAN clients, nothing to the gallery. This file is the
//     engine only; it knows nothing of where its audio comes from.
//
// ★★ WHAT IT DECODES TODAY: POCSAG (ITU-R M.584-2 Annex 1, "Radio-paging code No. 1", read from the ITU's own text) at
//    512, 1200 and 2400 bit/s. In AUTO the three rates run side by side on the same audio; whichever holds a batch lock
//    is what the readout names ("POCSAG 1200"), and each page says which rate carried it. A manual mode runs one rate.
//    ★★ FLEX (added 2026-10-31, Stuart: "I'd like to get flex if we can"): 1600, 3200 and 6400 bit/s, 2- and 4-level FSK
//      — one receiver, because every FLEX frame starts with Sync 1 at 1600 bit/s 2-level, whose A word NAMES the frame's
//      speed and levels ("FLEX 6400/4"). Clean room, from free published material only: ITU-R M.584-2 Annex 2 (the outline,
//      the 4-bit checksum, the codeword's bit order), Motorola's public FLEXchip datasheet (MC68175, Appendices A/B: the
//      word, vector and message layouts, address ranges, capcodes), TI's SPRA193 FLEX decoder note, McCulley's 1997 thesis
//      (Virginia Tech) and the FLEX patents on Google Patents (US 5,627,860; 6,396,411; 5,550,829 …) for the sync
//      structure and the 4-level Gray mapping. The A-word mode codes were read in multimon-ng's FLEX source (GPL — READ
//      for understanding, nothing copied) and two of them confirmed independently (a hobbyist page; a public recording).
//      Every source per fact: docs/v12/DECODERS-WAVE2.md section 17. Not used: ARIB STD-43, Motorola FLEX-93001.
//
// ★★ THE SIGNAL. 2-FSK NRZ: the input is the RAW NFM discriminator (no 300 Hz voice high-pass — pagers need response
//    to near DC; vibedsp setNfmVoice(false)), at any rate ≥ 4.8 kHz. Which shift is binary 1 is a transmitter/receiver
//    matter M.584 does not fix, so the sync word is searched in BOTH polarities and the page says which it was.
//
// ★★ PER RATE: a DC tracker (fast while hunting, slow once locked, so long runs of one bit do not drag the slicer), a
//    one-pole low-pass at ≈ the bit rate for the zero crossings, a DPLL (the crossing should fall at the bit boundary;
//    gain 0.12 hunting, 0.04 locked), integrate-and-dump over the bit for the decision. Bits → a 32-bit register →
//    the sync codeword 0x7CD215D8 (Table 1) within 2 bits, either polarity → the batch: 16 codewords (8 frames × 2).
//    Every codeword: BCH(31,21) syndrome, up to 2 bits corrected (generator x¹⁰+x⁹+x⁸+x⁶+x⁵+x³+1), then even parity.
//    The next batch's sync is expected exactly 544 bits on (within 4 bits); a miss drops the lock.
//
// ★★★ NEVER ON NOISE. A random 31-bit word is within 2 bits of a BCH codeword about half the time, so "every codeword
//     corrects" proves nothing. A batch counts only if ≥ 12 of its 16 words need ≤ 1 correction and pass parity (noise:
//     ≈ 1.5 % a word); a page is sent only from batches that count, with every one of its words valid. The test runs
//     hours of noise at every rate for zero pages.
// ★ COST: per sample per rate a handful of multiply-adds and one compare — three rates at 12 kHz ≈ 0.7 M flops a second.
//   No SIMD: the work is a per-sample feedback loop (DPLL, DC tracker) with nothing to put in lanes, and it is already
//   ≈ 0.1 % of a Pi 2 core. No divide or round per sample (32-bit ARM has neither in hardware).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class PagingDecoder {
public:
    /** Auto = every POCSAG rate + FLEX · Pocsag = every POCSAG rate · one POCSAG rate · Flex (any speed — the frame says). */
    enum class Mode { Auto, Pocsag512, Pocsag1200, Pocsag2400, Pocsag, Flex };
    struct Page {
        std::string proto;      ///< "POCSAG" or "FLEX"
        int      baud = 0;      ///< POCSAG 512 / 1200 / 2400 · FLEX bit/s 1600 / 3200 / 6400
        int      levels = 2;    ///< FLEX: 2 or 4 (FSK levels)
        char     phase = 0;     ///< FLEX: the phase it came on ('A'…'D'); 0 for POCSAG
        std::string speed;      ///< FLEX: "1600/2" … "6400/4" ("" for POCSAG)
        uint32_t address = 0;   ///< POCSAG: the 21-bit identity (18 bits from the word, 3 from its frame) · FLEX: the capcode
        int      function = 0;  ///< the two function bits, 0–3
        std::string kind;       ///< "numeric", "alpha" or "tone" (an address with no message); FLEX also "binary", "secure"
        std::string text;       ///< printable only; control characters dropped
        int      corrected = 0; ///< bits corrected across the page's words
        bool     inverted = false;
        bool     truncated = false;   ///< the lock was lost before the message ended
    };

    /** ★★ THE MANUAL CHOICES, AS DATA (V12 wiring, 2026-10-31): the server sends this list to the client, which draws one
     *  key per entry — so a FLEX receiver joins as more entries here, and no client changes (Stuart: "AUTO … so FLEX can
     *  be added later without changing the UI"). The first entry is AUTO. Ids are stable wire names. */
    struct ModeInfo { Mode mode; const char* id; const char* label; const char* group; };   ///< group "" = AUTO
    static const std::vector<ModeInfo>& modes();
    /** The mode for a wire id ("auto", "pocsag1200" …); unknown → Auto. */
    static Mode modeFromId(const std::string& id);
    static const char* idOf(Mode m);

    explicit PagingDecoder(double rateHz, Mode m = Mode::Auto);
    ~PagingDecoder();
    PagingDecoder(const PagingDecoder&) = delete;
    PagingDecoder& operator=(const PagingDecoder&) = delete;

    /** Raw FM discriminator audio (any scale; DC is tracked). */
    void feed(const float* x, int n);
    void setMode(Mode m);
    Mode mode() const { return mode_; }
    void reset();
    /** What is locked now ("POCSAG 1200"), or "" — the readout. */
    std::string detected() const;

    std::function<void(const Page&)> onPage;

    // ── the code, exposed for the test ──
    static constexpr uint32_t kSync = 0x7CD215D8u, kIdle = 0x7A89C197u;
    /** A full 32-bit codeword (BCH check bits + even parity) from its 21 information bits (MSB = the flag bit). */
    static uint32_t encodeWord(uint32_t info21);
    /** Correct up to 2 bits; returns the number corrected (0–2), or −1 if uncorrectable or the parity fails. */
    static int correctWord(uint32_t& cw);

    // ── FLEX's code helpers, exposed for the test's transmitter ──
    /** The 21 information bits i0…i20 (i0 the FIRST sent, so the word's MSB) of a FLEX word from its value v (i0 = v's
     *  LSB) — FLEX sends its fields least significant bit first (M.584-2 Annex 2). */
    static uint32_t flexInfo(uint32_t v);
    /** The 4-bit checksum M.584-2 Annex 2 puts in i0…i3: the five nibbles from i0 and i20 sum to 0xF (mod 16). */
    static bool flexChecksumOk(uint32_t v);
    static uint32_t flexWithChecksum(uint32_t v);   ///< v (i4…i20 set) with i0…i3 filled in
    /** The alphanumeric fragment check: one's complement of the sum of each message word's 8 + 8 + 5-bit groups (the
     *  header's own 10 check bits left out), 10 bits. `words` are values (i0 = LSB). */
    static uint32_t flexAlphaCheck(const std::vector<uint32_t>& words);
    /** Sync 1's A word for a speed: the 16-bit mode code (1600/2, 1600/4 = 3200 bit/s, 3200/2, 3200/4 = 6400). */
    static uint16_t flexModeCode(int bps, int levels);

    struct Rx;       // one POCSAG bit rate's receiver (paging_decoder.cpp)
    struct FlexRx;   // the FLEX receiver (every speed)
private:
    double fs_;
    Mode mode_;
    std::vector<std::unique_ptr<Rx>> rx_;
    std::unique_ptr<FlexRx> flex_;
    void build_();
};

} // namespace vibe
