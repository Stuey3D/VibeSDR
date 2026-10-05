// VibeSDR V4 — FSK (RTTY / NAVTEX) decoder.
//
// C++ port of UberSDR's ka9q audio_extensions/fsk (decoder.go + fsk_demod.go +
// biquad.go + ita2.go). Takes mono int16 audio at a fixed sample rate and emits
// decoded characters (+ a coarse decoder state) via callbacks. The shim wraps
// this in the /ws/dxcluster audio-extension protocol so the existing VibeSDR
// decoder UI works unchanged. Encodings: ITA2 and ASCII (RTTY, below); CCIR476
// (NAVTEX) has its own receiver, NavtexRx (2026-10-05).
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
//   streams interleaved. Phasing: DX slots carry 0x66 (rep), RX slots 0x0F (alpha).
// ★★ (2026-10-05) The coder works on SOFT words — seven bit values, sign = mark/space, size = how sure — so a character
//    neither copy of which reads cleanly can still be recovered from the two together (decodeSoft). The DX/RX phase
//    belongs to NavtexRx, which finds it in the bit stream itself. Ported from fldigi's navtex.cxx (process_bytes,
//    process_char) — Rémi Chateauneu F4ECW, Rik van Riel AB1KW; via madpsy/ubersdr_navtex (Franco Venturi's port). GPL-3.0+.
class Ccir476 {
public:
    Ccir476();
    void reset() { shift = false; lastCode = -1; }
    static bool fourMarkBits(uint8_t v);
    static uint8_t hardCode(const double* w);   // bit i = w[i] > 0 (least significant bit first, as sent)
    /** The tiers of fldigi's process_bytes, for one RX slot with (if `dx`) its DX copy 35 bits earlier:
     *    +1 the RX copy is a valid word · 0 the DX copy is (a straight repair) · -1 a SOFT repair (the two copies summed,
     *    or the least certain bit flipped in the RX copy, the DX copy, then the sum) · -2 nothing found.
     *  `code` gets the word to print (-1 = none). `softFec` false keeps only the first two tiers (the old decoder's).
     *  ★ An RX copy that reads as a PHASING code while its DX copy is a real character is a misread (phasing never
     *    puts a character in the DX slot): the DX copy is the one used (audit 2026-10-04, row 12). */
    static int decodeSoft(const double* rx, const double* dx, bool softFec, int& code, bool vote = true);
    /** Print a word: shift codes and phasing print nothing (0); FIGS BEL prints '\''. `phaseWrong` is set when this is
     *  the SECOND phasing rep (0x66) in a row read in an RX slot — fldigi's process_char: the DX/RX phase is out by one. */
    char32_t emit(int code, bool& phaseWrong);
    bool shifted() const { return shift; }
private:
    char32_t codeToChar(uint8_t code, bool fig) const;
    char32_t ltrs[128], figs[128]; bool validCodes[128] = {false};
    std::map<uint8_t, char32_t> codeLtrs, codeFigs;
    bool shift = false;
    int lastCode = -1;
    static const uint8_t codeAlpha = 0x0f, codeBeta = 0x33, codeChar32 = 0x6a,
                         codeRep = 0x66, letters = 0x5a, figures = 0x36;
};

// ── NAVTEX receiver: demodulator, bit clock, character sync and FEC ─────────
/** ★★★ ONE SWITCH PER MEASURED CHANGE (2026-10-05). Each was kept only if the CER held or improved on BOTH the
 *  synthetic bench (tools/rtty-bench/navtex_bench.sh) and Stuart's 518 kHz recording; the defaults are the shipped
 *  decoder. They exist so the bench can take one away and show what it was worth. */
struct NavtexOptions {
    bool rcDemod    = false;   // tones mixed to baseband + raised-cosine lowpass (else the Q≈3 biquad bandpasses)
    bool earlyLate  = true;   // early/prompt/late bit clock (else the zero-crossing histogram)
    bool logSoft    = true;   // bit values from log-compressed ATC levels (else ±1 per sample)
    bool atcHalf    = false;   // W7AY ATC: ½ and clipped to the noise floor (else ¼, clipped to the envelope only)
    bool softFec    = true;   // the soft FEC tiers (else RX, then DX, then '_')
    bool fecVote    = true;   // RX and DX both valid but different: the one the summed soft bits favour (else RX)
    bool autoInvert = true;   // the character sync tries both polarities
    bool afc        = false;  // follow a drifting signal
};

class NavtexRx {
public:
    enum State { NoSignal = 0, Hunting = 1, ReadData = 3 };   // the FskDecoder::State numbers (Sync1 = hunting)
    NavtexRx(int sampleRate, double centerFreq, double shiftHz, double baudRate, bool inverted,
             const NavtexOptions& opts = NavtexOptions());
    void process(const int16_t* samples, int count);
    /** One bit's soft value (> 0 mark) — the demodulator's output; public so the coder can be tested on exact streams. */
    void pushBit(double v);

    std::function<void(char32_t)> onChar;
    std::function<void(int)>      onState;

    /** ★ Per-character FEC outcome, for a later per-message report: clean (RX copy good), repaired (from the DX copy
     *  or the soft tiers), failed ('_'). `total` counts since start; `message` since the last ZCZC, and `lastMessage`
     *  is the finished one, frozen at its NNNN. */
    struct Counts { unsigned long clean = 0, repaired = 0, failed = 0; };
    Counts total, message, lastMessage;

    unsigned long resyncs() const { return resyncs_; }
    double audioLevel() const { return audioAverage_; }
    double audioThreshold() const { return audioMinimum_; }
    int    stateNow() const { return (int)state_; }
    bool   invertedNow() const { return pol_ < 0; }
    double afcOffsetHz() const { return afcHz_; }

private:
    void setState(State s);
    void frontSample(double m, double s, double level);   // the two tone magnitudes at frontRate_, + the gate level
    int  findAlpha(int& pol) const;
    void processRx(int pos);
    void retune();

    NavtexOptions o_;
    double fs_, cf_, shift_, baud_;
    int pol_ = 1;
    State state_ = NoSignal;
    unsigned long resyncs_ = 0;
    double audioAverage_ = 0.1, audioMinimum_ = 256.0, audioTC_ = 0;

    // ── front end ──
    int decim_ = 1; double frontRate_ = 0, bitSamples_ = 0;
    std::vector<double> decH_, decBuf_; int decPos_ = 0, decCount_ = 0;      // decimating lowpass (rcDemod)
    std::vector<double> rcH_; std::vector<double> rcBuf_[4]; int rcPos_ = 0; // raised cosine, mark I/Q + space I/Q
    double mRe_ = 1, mIm_ = 0, sRe_ = 1, sIm_ = 0, mStepRe_ = 1, mStepIm_ = 0, sStepRe_ = 1, sStepIm_ = 0;
    long   oscCount_ = 0;
    BiQuad bpMark_, bpSpace_, lpMark_, lpSpace_;                            // !rcDemod
    double markEnv_ = 0, spaceEnv_ = 0, markNoise_ = 0, spaceNoise_ = 0, noiseFloor_ = 0;
    // ── AFC ──
    double afcHz_ = 0;
    // ── bit clock ──
    long long sampleCount_ = 0;
    double early_ = 0, prompt_ = 0, late_ = 0, nextEarly_ = 0, nextPrompt_ = 0, nextLate_ = 0;
    double avgEarly_ = 0, avgPrompt_ = 0, avgLate_ = 0;
    // zero-crossing clock (!earlyLate) — the old FskDecoder's
    int zcBitCount_ = 0, zcHalf_ = 0, zcDuration_ = 0, zcNext_ = 0, zcRounds_ = 0; double zcDelta_ = 0;
    std::vector<int> zcHist_; bool zcOld_ = false;
    // ── bits and characters ──
    static const int kBits = 100;                      // one second of bit values: 14 characters
    double bits_[kBits] = {0};
    long long absBit_ = 0, lastRxAbs_ = -1000, lostAt_ = 0;
    bool quiet_ = false;
    int cursor_ = 0, errorCount_ = 0;
    bool alphaPhase_ = false;
    Ccir476 coder_;
    std::string tail_;                                 // the last four characters printed, for ZCZC / NNNN
};

// ── FSK demodulator + decoder ───────────────────────────────────────────────
class FskDecoder {
public:
    enum State { NoSignal, Sync1, Sync2, ReadData };
    FskDecoder(int sampleRate, double centerFreq, double shiftHz, double baudRate,
               const std::string& framing, const std::string& encoding, bool inverted,
               const NavtexOptions* navtexOpts = nullptr);
    ~FskDecoder();
    FskDecoder(const FskDecoder&) = delete; FskDecoder& operator=(const FskDecoder&) = delete;   // owns its coders
    void process(const int16_t* samples, int count);
    /** ★ NAVTEX (CCIR476) runs its own receiver (2026-10-05) — null for RTTY. */
    NavtexRx* navtex() const { return navtex_; }

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
    unsigned long resyncs() const { return navtex_ ? navtex_->resyncs() : resyncCount_; }
    unsigned long framingErrors() const { return framingErrors_; }
    /** ★ Frames that passed the start/stop check while decoding (ITA2) — RttyAuto scores candidates on this. */
    unsigned long goodFrames() const { return goodFrames_; }
    /** Unshift on space (ITA2 only) — see Ita2::usos. */
    void setUsos(bool on) { if (ita2) ita2->usos = on; }
    double        audioLevel() const { return navtex_ ? navtex_->audioLevel() : audioAverage; }
    double        audioThreshold() const { return navtex_ ? navtex_->audioThreshold() : audioMinimum; }
    int           stateNow() const { return navtex_ ? navtex_->stateNow() : (int)state; }

private:
    void updateFilters();
    void setState(State s);
    void processBit(bool bit);
    void processCharacter(uint16_t code);

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

    Ita2*     ita2 = nullptr;
    NavtexRx* navtex_ = nullptr;
};

} // namespace vibe
