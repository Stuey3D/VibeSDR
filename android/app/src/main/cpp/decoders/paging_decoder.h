// decoders/paging_decoder.h — the PAGING receiver: ONE demodulator with an AUTO mode (V12, 2026-10-31).
//
// ★★★ SCOPE (Stuart's spec, docs/v12/DECODERS-WAVE2.md "PAGING"): one PAGE button, Android LOCAL HARDWARE ONLY (the
//     phone's own plugged-in radio) — never on any server, never to LAN clients, nothing to the gallery. This file is the
//     engine only; it knows nothing of where its audio comes from.
//
// ★★ WHAT IT DECODES TODAY: POCSAG (ITU-R M.584-2 Annex 1, "Radio-paging code No. 1", read from the ITU's own text) at
//    512, 1200 and 2400 bit/s. In AUTO the three rates run side by side on the same audio; whichever holds a batch lock
//    is what the readout names ("POCSAG 1200"), and each page says which rate carried it. A manual mode runs one rate.
//    ✗ FLEX is NOT here: M.584-2 Annex 2 (FLEX-TD) gives FLEX's outline (1.875 s frames, 1600 bit/s sync, 11 interleaved
//      160 ms blocks, the same BCH(31,21)+parity words) but not its sync patterns, frame-information or vector layouts —
//      those are in ARIB RCR STD-43 / Motorola's FLEX-93001, neither of which is freely published. Built only once Stuart
//      decides the source (see the doc's questions). The AUTO structure takes another receiver without change.
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
    enum class Mode { Auto, Pocsag512, Pocsag1200, Pocsag2400 };
    struct Page {
        std::string proto;      ///< "POCSAG"
        int      baud = 0;      ///< 512 / 1200 / 2400
        uint32_t address = 0;   ///< the 21-bit identity (18 bits from the word, 3 from its frame)
        int      function = 0;  ///< the two function bits, 0–3
        std::string kind;       ///< "numeric", "alpha" or "tone" (an address with no message)
        std::string text;       ///< printable only; control characters dropped
        int      corrected = 0; ///< bits corrected across the page's words
        bool     inverted = false;
        bool     truncated = false;   ///< the lock was lost before the message ended
    };

    /** ★★ THE MANUAL CHOICES, AS DATA (V12 wiring, 2026-10-31): the server sends this list to the client, which draws one
     *  key per entry — so a FLEX receiver joins as more entries here, and no client changes (Stuart: "AUTO … so FLEX can
     *  be added later without changing the UI"). The first entry is AUTO. Ids are stable wire names. */
    struct ModeInfo { Mode mode; const char* id; const char* label; };
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

    struct Rx;   // one bit rate's receiver (paging_decoder.cpp)
private:
    double fs_;
    Mode mode_;
    std::vector<std::unique_ptr<Rx>> rx_;
    void build_();
};

} // namespace vibe
