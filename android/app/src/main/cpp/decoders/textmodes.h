// decoders/textmodes.h — ★ V12 (2026-10-31): the MFSK keyboard modes — MFSK16/32, Olivia, Contestia and THOR — on the
// listener's audio, one decoder object per family with an AUTO that runs the common sub-modes side by side.
//
// ★ CLEAN ROOM. Every format detail comes from a published description; where the prose left a choice open, the choice
//   was settled by decoding published reference recordings with known text (docs/v12/DECODERS-WAVE2.md section 11):
//   • MFSK16/32 — ZL1BPU's MFSK16 Technical Specification rev 1.2 and the ARRL copy (16 tones, spacing = baud, Gray tone
//     weighting, the lowest tone all zeros, the first-arriving bit the heaviest), "The MFSK FEC Coder" (R = 1/2, K = 7,
//     taps O1+O3+O4+O6+O7 then O1+O2+O3+O4+O7, dibit 1 first), "The IZ8BLY Diagonal Interleaver" (10 concatenated 4 × 4:
//     bit i of a symbol leaves 10·i symbols later) and the IZ8BLY MFSK Varicode (mfsk_varicode.h; "001" ends a code).
//     MFSK16 = 15.625 Bd at 8 kHz (512 samples), MFSK32 = 31.25 Bd (256).
//   • THOR — ZL1BPU's DominoEX page (18 tones, IFK+: the data is the step from one tone to the next, offset by 2 so no
//     tone repeats) with MFSK16's FEC coder, interleaver and varicode (the same page and the THOR notes). The value is
//     the step itself (no Gray). THOR 4/8/16 run on 8 kHz (2048/1024/512 samples), 5/11/22 on 11.025 kHz; 4, 5 and 8
//     use double tone spacing.
//   • OLIVIA — P. Jalocha's description as Wikipedia gives it: 2^b tones over the bandwidth, 64-symbol blocks carrying b
//     characters, each a 64-chip Walsh function built diagonally (character j takes bit (j + k) mod b of symbol k),
//     scrambled with 0xE257E6D0291574EC rotated right 13·j bits, Gray tone coding, symbols every fs·M/BW samples shaped
//     by the published window over twice that. Settled against the two Wikimedia reference recordings ("Welcome to
//     Wikipedia, …", 16/500 and 32/1000): the chip k uses scrambler bit k (LSB first), symbol bit i has weight 2^i, the
//     tone is the Gray code of the value, and the character's top bit is the Walsh sign XOR the parity of its index.
//   • CONTESTIA — Olivia's structure with 32-symbol blocks of 6-bit characters (the description). The scrambler is not
//     in the prose: 0xEDB88320 rotated right 5·j bits was FOUND by search against public recordings (Olivia's constant
//     gave no lock); space is code 59, code 0 is idle, the rest are ASCII − 32 (uppercase). Named as a finding.
//
// ★ COST: one real FFT (PFFFT — its own NEON/SSE kernels) of 2 × the symbol per quarter symbol per symbol length in use,
//   powers in q65v lanes, Walsh transforms in q65v lanes, a 64-state Viterbi at the bit rate (≤ 62 bit/s, scalar: ≈ 8 k
//   compare-adds a second). No divide or round per sample.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class TextModeDecoder {
public:
    enum class Family { Mfsk, Olivia, Thor };
    struct ModeInfo { std::string id, label; };

    /** `mode` is "auto" or one of modes(family)'s ids; anything else is AUTO. */
    TextModeDecoder(Family f, double inRate, const std::string& mode = "auto");
    ~TextModeDecoder();
    void feed(const float* x, int n);

    std::function<void(const std::string&)> onText;     ///< decoded text (UTF-8), as it arrives; a "[MODE · Hz]" line on lock
    std::function<void(int)> onState;                    ///< 0 searching · 1 locked
    std::function<void(const std::string& json)> onStatus;   ///< {"detected","hz","mode"} when it changes

    std::string detected() const;    ///< "OLIVIA 16/500" while locked, "" otherwise
    float hz() const;                ///< the locked signal's centre (audio Hz), 0 when none

    static std::vector<ModeInfo> modes(Family f);        ///< AUTO first; the server sends this list as data
    static std::string modesJson(Family f);               ///< [{"id":"auto","label":"AUTO"}, …] for vibeserver.json
    static bool familyOf(const std::string& ext, Family& f);   ///< "mfsk" | "olivia" | "thor"
    static const char* extName(Family f);

    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

/** ★ Transmit side, for tests and benches only: the same published formats, synthesised (audio at `fs`, unit tone
 *  amplitude). `mode` as modes()'s ids. `lead`/`tail` seconds of idle carrier / silence. */
std::vector<float> textModeSynth(TextModeDecoder::Family f, const std::string& mode, const std::string& text, double fs,
                                 double audioHz, uint32_t seed = 1);

} // namespace vibe
