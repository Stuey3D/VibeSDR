// VibeSDR V4 — FSK (RTTY / NAVTEX) decoder.
//
// C++ port of UberSDR's ka9q audio_extensions/fsk (decoder.go + fsk_demod.go +
// biquad.go + ita2.go). Takes mono int16 audio at a fixed sample rate and emits
// decoded characters (+ a coarse decoder state) via callbacks. The shim wraps
// this in the /ws/dxcluster audio-extension protocol so the existing VibeSDR
// decoder UI works unchanged. Encoding: ITA2 (RTTY) implemented; CCIR476
// (NAVTEX) is a later add.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <map>

namespace vibe {

// ── Biquad (RBJ cookbook) — only the bandpass + lowpass we need ──────────────
class BiQuad {
public:
    enum Type { Bandpass, Lowpass };
    void configure(Type type, double freq, double sampleRate, double q);
    double filter(double in);
    void reset() { x1 = x2 = y1 = y2 = 0; }
private:
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
};

// ── ITA2 / Baudot (RTTY) with async framing ─────────────────────────────────
class Ita2 {
public:
    explicit Ita2(const std::string& framing);
    void reset() { shift = false; lastCode = 0; firstChar = true; }
    /** ★ UNSHIFT ON SPACE (2026-10-04): back to letters after every space, as many amateur machines send. OFF by
     *  default — DWD sends whole groups of figures under ONE FIGS shift, which this would turn into letters. */
    bool usos = false;
    /** ★ ASCII (2026-10-04, the full RTTY spec): 7 or 8 data bits, read as characters directly — no LTRS/FIGS shift. */
    bool ascii = false;
    int nbits() const { return nbits_; }
    uint16_t msb() const { return (uint16_t)(1 << (nbits_ - 1)); }
    bool checkBits(uint16_t code) const;
    // Returns decoded char (0 = none).
    char32_t processChar(uint16_t code);
private:
    char32_t codeToChar(uint8_t code, bool fig) const;
    char32_t ltrs[32], figs[32];
    std::map<uint8_t, char32_t> codeLtrs, codeFigs;
    bool shift = false, firstChar = true;
    uint8_t lastCode = 0;
    const uint8_t letters = 0x1f, figures = 0x1b;
    int dataBits = 5, nbits_ = 5;
    char parity_ = 'N';     // N none, E even, O odd, M mark (always 1), S space (always 0)
};

// ── CCIR476 (NAVTEX / SITOR-B) — 7-bit FEC, 4 mark bits per char ─────────────
// ★ SITOR-B sends every character twice: the DX copy, then the RX copy five character slots (35 bits) later, the two
//   streams interleaved. `alphaPhase` = "this slot is an RX slot" (fldigi's name: the RX slot is the one decoded, the
//   DX copy five slots back — c1 here — is the spare). Phasing: DX slots carry 0x66 (rep), RX slots 0x0F (alpha).
class Ccir476 {
public:
    Ccir476();
    void reset() { shift = false; alphaPhase = false; phaseKnown = false; phaseVotes = 0; c1 = c2 = c3 = 0; }
    int nbits() const { return 7; }
    uint16_t msb() const { return 0x40; }
    bool checkBits(uint16_t code) const;
    /** Returns the decoded char (0 = none). `score` is fldigi's process_bytes result for an RX slot — +1 the RX copy
     *  was good, 0 the DX copy repaired it (or the slot was phasing), -2 neither copy was readable (prints '_') —
     *  and 0 for a DX slot, which is only stored for later. */
    char32_t processChar(uint16_t code, int& score);
    /** ★ A character slot went by with no word read (the bit hunt after a resync): keep the DX/RX phase and the
     *  DX history in step, so a resync does not have to re-learn the phase from the next phasing signal. */
    void skipSlot();
private:
    static bool fourMarkBits(uint8_t v);
    char32_t codeToChar(uint8_t code, bool fig) const;
    char32_t decode(uint8_t chr);
    char32_t ltrs[128], figs[128]; bool validCodes[128] = {false};
    std::map<uint8_t, char32_t> codeLtrs, codeFigs;
    bool shift = false, alphaPhase = false, phaseKnown = false;
    int phaseVotes = 0;
    uint8_t c1 = 0, c2 = 0, c3 = 0;
    const uint8_t codeAlpha = 0x0f, codeBeta = 0x33, codeChar32 = 0x6a,
                  codeRep = 0x66, letters = 0x5a, figures = 0x36;
};

// ── FSK demodulator + decoder ───────────────────────────────────────────────
class FskDecoder {
public:
    enum State { NoSignal, Sync1, Sync2, ReadData };
    FskDecoder(int sampleRate, double centerFreq, double shiftHz, double baudRate,
               const std::string& framing, const std::string& encoding, bool inverted);
    void process(const int16_t* samples, int count);

    std::function<void(char32_t)> onChar;   // decoded character
    std::function<void(int)>      onState;  // State change (0..3)

    // ── ★★★ HEALTH, for the admin page ─────────────────────────────────────────────────────
    // ★★ `resyncs` is the diagnostic that matters. Whenever the envelope falls below
    //    `audioMinimum` the decoder declares NoSignal and THROWS AWAY its whole state — bit
    //    count, code bits, sync history — then re-hunts for valid codes. Each one of those costs
    //    a burst of garbage while it re-acquires, which reads to a user as "it decodes clean for
    //    a bit then slips out" (Stuart, 2026-08-06) rather than as what it is: the decoder giving
    //    up and starting again because the signal dipped.
    // ★ `audioLevel` is what it is actually seeing, on the int16 scale the threshold uses, so the
    //   two can be compared directly instead of guessed at.
    unsigned long resyncs() const { return resyncCount_; }
    unsigned long framingErrors() const { return framingErrors_; }
    /** ★ Frames that passed the start/stop check while decoding (ITA2) — RttyAuto scores candidates on this. */
    unsigned long goodFrames() const { return goodFrames_; }
    /** Unshift on space (ITA2 only) — see Ita2::usos. */
    void setUsos(bool on) { if (ita2) ita2->usos = on; }
    double        audioLevel() const { return audioAverage; }
    double        audioThreshold() const { return audioMinimum; }
    int           stateNow() const { return (int)state; }

private:
    void updateFilters();
    void setState(State s);
    void processBit(bool bit);
    int  processCharacter(uint16_t code);   // ITA2: 1; CCIR476: the RX-slot score (Ccir476::processChar)

    double sampleRate, centerFrequency, shiftHz, deviationF, baudRate;
    bool inverted;
    std::string framing, encoding;

    double lowpassFilterF = 140.0, markSpaceFilterQ = 0, markF = 0, spaceF = 0;
    double audioAverageTC = 0, audioMinimum = 256.0;
    int bitSampleCount = 0, halfBitSampleCount = 0;

    BiQuad biquadMark, biquadSpace, biquadLowpass;
    // ★★ OPTIMAL ATC (2026-10-04) — each tone's envelope smoothed on its own (biquadLpMark/Space), with a peak
    //    tracker per tone and a shared noise floor, so a tone fading on its own (HF selective fading) no longer
    //    tilts the mark/space decision. Kahn's diversity-combining rule, as fldigi's "optimal ATC". See process().
    BiQuad biquadLpMark, biquadLpSpace;
    double markEnv = 0, spaceEnv = 0, noiseFloor = 0, symLen = 0;
    // ★ Framing errors in ReadData (ITA2) — each one re-hunts the start bit instead of printing garbage on.
    unsigned long framingErrors_ = 0, goodFrames_ = 0;
    int lockRequired_ = 2;

    State state = NoSignal;
    double audioAverage = 0.1;
    unsigned long resyncCount_ = 0;
    int signalAccumulator = 0, bitDuration = 0, sampleCount = 0, nextEventCount = 0;
    bool averagedMarkState = false, oldMarkState = false, pulseEdgeEvent = false;

    int zeroCrossingSamples = 16, zeroCrossingsDivisor = 4, zeroCrossingCount = 0;
    std::vector<int> zeroCrossings;
    double syncDelta = 0;

    int bitCount = 0; uint16_t codeBits = 0; int nbits = 0; uint16_t msb = 0;
    bool syncSetup = false; std::vector<uint16_t> syncChars; int validCount = 0, errorCount = 0;
    bool waiting = false, stopVariable = false;

    // ★ NAVTEX (2026-10-04): a resync forced by bad characters keeps the coder's shift and DX/RX phase (only a loss of
    //   signal resets them); bitClock_/ccirLastEnd_ count the bits the re-hunt skipped, so the phase stays in step.
    bool keepCoderState_ = false;
    unsigned long bitClock_ = 0, ccirLastEnd_ = 0;

    Ita2*    ita2 = nullptr;
    Ccir476* ccir476 = nullptr;
};

} // namespace vibe
