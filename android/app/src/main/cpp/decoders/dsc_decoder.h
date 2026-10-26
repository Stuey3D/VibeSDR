// VibeSDR V12 — maritime DIGITAL SELECTIVE CALLING (DSC): MF/HF 100 Bd FSK and VHF channel 70 1200 Bd AFSK.
//
// Built from the published standard, ITU-R M.493 (DSC for the maritime mobile service) and M.541 (its operational
// procedures), with the MMSI numbering of M.585 (vibe_maritime_mid.h). No other decoder's source was copied.
//
// ── The signal (M.493 Annex 1 §2) ─────────────────────────────────────────────────────────────────────────────────
//  • MF/HF: F1B/J2B, 100 Bd, 170 Hz shift; through an SSB transmitter the audio centre is 1700 Hz (tones 1615/1785 Hz,
//    so a USB dial 1.7 kHz below the assigned frequency: 2187.5 kHz → 2185.8 kHz USB).
//  • VHF (channel 70, 156.525 MHz): an FM subcarrier keyed 1300/2100 Hz at 1200 Bd.
//  • Which tone is binary 1 (the "B" state) is NOT left to a setting: the phasing search tries both senses, so the
//    decoder works on USB or LSB, and through a receiver that mirrors the audio, without anyone choosing.
//
// ── The code (M.493 §3) ───────────────────────────────────────────────────────────────────────────────────────────
//  • A symbol is 10 bits: 7 information bits (the value 0–127), least significant FIRST, then 3 check bits giving the
//    NUMBER OF 0 BITS among the seven, most significant first. Only 128 of the 1024 words are valid (distance 2).
//  • TIME DIVERSITY: every symbol is sent twice — DX, then its RX copy four symbol periods later — the two streams
//    interleaved DX RX DX RX … (on air: DX[i] in slot 2i, RX copy in slot 2i+5).
//  • PHASING: the DX stream opens with six 125s, the RX stream with 111, 110, … 104 (RX7 … RX0), so the receiver knows
//    both the symbol boundary and which stream is which. Before it, a dot pattern (alternate 1/0: 200 bits on MF/HF
//    calls a coast station must hear while scanning, 20 otherwise).
//  • A call: format specifier (twice), then its fields, the end-of-sequence symbol (117 acknowledgement requested ·
//    122 acknowledgement given · 127 any other), and the ERROR-CHECK CHARACTER: the bitwise XOR of the format
//    specifier (once), every message symbol and the EOS. The DX stream sends the EOS twice more after the ECC (so the
//    RX copies have time to finish); an EXPANSION message (M.821 — enhanced position, speed, course …) follows the
//    primary ECC with its own EOS and ECC.
//
// ── What this receiver does beyond reading the code as written ─────────────────────────────────────────────────────
//  ★ SOFT DECISIONS, BOTH COPIES: each symbol is the best of the 128 valid words on the SUM of its DX and RX soft bits
//    (a faded copy weighs less because its soft bits are smaller), never "RX if valid, else DX".
//  ★ The FORMAT SPECIFIER is judged on all four of its copies, and the EOS on up to four.
//  ★ The ERROR-CHECK CHARACTER CORRECTS, not only detects: a call whose check fails is searched for the cheapest change
//    (one symbol, or two among the least certain) that makes it pass and still reads as a valid call. It is then
//    marked "corrected"; one that cannot be put right is marked "unconfirmed" — shown, never passed off as clean.
//  ★ Fine timing tracked through the call (early/late at bit transitions), so a long expansion does not slip.
//
// Input: mono float audio (48 kHz from the server; the HF receiver also takes 24/12 kHz — the bench's recordings).
// Cost: two short decimating FIRs (NEON / SSE2 / scalar), two tone correlators at 12 (HF) or 8 (VHF) samples a bit.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace dsc {

// ── The 10-bit code ──────────────────────────────────────────────────────────────────────────────────────────────
/** The 10 bits of symbol `v` (0–127) as transmitted: bit k of the result is the k-th bit sent. */
uint16_t codeword(int v);
/** The symbol a 10-bit word (bit k = k-th bit sent) is, or -1 if it is not a valid word. */
int decodeWord(uint16_t w);

// ── The symbols' meanings (M.493 Annex 1 Tables) ─────────────────────────────────────────────────────────────────
enum Format { kGeoArea = 102, kDistress = 112, kGroup = 114, kAllShips = 116, kIndividual = 120, kAutomatic = 123 };
enum Category { kRoutine = 100, kBusiness = 106, kSafety = 108, kUrgency = 110, kCatDistress = 112 };
enum Eos { kEosAckRq = 117, kEosAckBq = 122, kEosOther = 127 };
bool isFormat(int v);
bool isEos(int v);
const char* formatWord(int v);       ///< "distress alert", "all ships" …
const char* categoryWord(int v);     ///< "routine", "safety" …
const char* natureWord(int v);       ///< "Sinking" …
const char* tc1Word(int v);          ///< "J3E telephony" …
const char* tc2Word(int v);          ///< "busy" …
const char* subsequentWord(int v);   ///< a distress alert's "subsequent communications"

struct Position {
    bool known = false;
    double lat = 0, lon = 0;          ///< degrees, + north / + east
    int quadrant = -1;                ///< 0 NE · 1 NW · 2 SE · 3 SW
    int latDeg = 0, latMin = 0, lonDeg = 0, lonMin = 0;
    int latFrac = -1, lonFrac = -1;   ///< ten-thousandths of a minute from an expansion message (M.821), -1 = none
};
struct Area {                          ///< a geographic-area call's rectangle
    bool valid = false;
    int quadrant = -1, latDeg = 0, lonDeg = 0, dLat = 0, dLon = 0;
    double lat = 0, lon = 0;           ///< the reference corner, signed
};

/** One decoded call. Every string is built here from digits and our own tables — nothing free-form comes off the air. */
struct Call {
    int format = 0;
    int category = -1;                 ///< -1: a distress ALERT has no category field
    std::string address;               ///< 9-digit MMSI called (individual, group, automatic)
    Area area;                         ///< geographic-area calls
    std::string self;                  ///< 9-digit MMSI of the caller
    int tc1 = -1, tc2 = -1;
    // distress information: the alert itself, or the ship a relay / acknowledgement is about
    std::string distressId;            ///< relay / acknowledgement: the ship in distress (an alert's is `self`)
    int nature = -1;
    Position pos;
    int utcHour = -1, utcMin = -1;     ///< the time of the position (88:88 = unknown → -1)
    int subsequent = -1;
    // the rest of a non-distress call
    std::vector<std::string> freqs;    ///< "2182.0 kHz", "Ch 16", "HF channel 1601"
    bool hasPosReply = false;          ///< a position sent in the frequency fields (a position request's reply)
    Position posReply;
    int posReplyHour = -1, posReplyMin = -1;
    std::string number;                ///< a semi-automatic call's telephone number
    int eos = 0;
    // expansion (M.821)
    double speedKn = NAN, courseDeg = NAN;
    int persons = -1;
    int expansions = 0;
    // how well it was received
    enum class Check { Ok, Corrected, Failed };
    Check check = Check::Ok;
    int corrected = 0;                 ///< symbols the check character put right
    int disagreed = 0;                 ///< symbols whose DX and RX copies read differently (soft combining chose)
    float quality = 0;                 ///< 0–1, the mean soft margin of the call's symbols
    float fixCost = 0;                 ///< the soft evidence the check's correction overruled (share of a symbol's energy)
    float fixGap = -1;                 ///< …and how much more the next alternative would have cost (-1 = none found)
    int doubt = 0;                     ///< positions in doubt (best word barely ahead)
    bool vhf = false;
    std::vector<int> symbols;          ///< F, fields, EOS (the primary) — for the tests and the bench, never shown
    /** 3 distress · 2 urgency · 1 safety · 0 routine (and business) — what the clients and the gallery highlight. */
    int level() const;
};

/** Read a primary call: `syms` = format specifier, the fields, EOS. False (with a reason) if it is not a valid call. */
bool parse(const std::vector<int>& syms, Call& out, std::string* why = nullptr);
/** Add an expansion message (specifier, data …, EOS) to `c`. False if it cannot be read. */
bool parseExpansion(const std::vector<int>& syms, Call& c);
/** The error-check character for F … EOS (`syms` as parse takes them). */
int eccOf(const std::vector<int>& syms);
/** The readable line: "DISTRESS · MMSI 235012345 (United Kingdom) · Sinking · 50°12'N 004°10'W · 14:32 UTC · …". */
std::string describe(const Call& c);
/** "235012345 (United Kingdom)", "coast station 002320004 (United Kingdom)" … */
std::string describeMmsi(const std::string& d9);
std::string positionText(const Position& p);

// ── Building calls (the tests and the bench transmit what they decode) ───────────────────────────────────────────
namespace build {
void mmsi(std::vector<int>& s, const std::string& d9);        ///< 5 symbols (the 10th digit 0)
void position(std::vector<int>& s, double lat, double lon);   ///< 5 symbols
void unknownPosition(std::vector<int>& s);                    ///< 99 99 99 99 99
void time(std::vector<int>& s, int h, int m);                 ///< 2 symbols (h < 0: 88 88)
void freqKhz(std::vector<int>& s, double khz);                ///< 3 symbols, 100 Hz units
void vhfChannel(std::vector<int>& s, int ch);                 ///< 3 symbols: 9 0 cccc
void noFreq(std::vector<int>& s);                             ///< 126 126 126
/** The on-air bits of a whole transmission: dot pattern, phasing, the call (DX/RX interleaved), optional expansion
 *  messages (each specifier … EOS, its ECC added), as 0/1. */
std::vector<uint8_t> transmission(const std::vector<int>& primary, const std::vector<std::vector<int>>& expansions,
                                  int dotBits);
/** That transmission as audio: continuous-phase FSK at `rate`, MF/HF (centre `centreHz`, 170 Hz shift, 100 Bd, 1 = the
 *  higher tone unless `invert`) or VHF (1300 = 1, 2100 = 0, 1200 Bd). Amplitude 1. */
std::vector<float> modulate(const std::vector<uint8_t>& bits, bool vhf, double rate, double centreHz = 1700,
                            bool invert = false, double baudPpm = 0, double startPhase = 0);
} // namespace build

// ── The receiver ─────────────────────────────────────────────────────────────────────────────────────────────────
struct RxConfig {
    bool   vhf = false;
    double rate = 48000;          ///< input rate: MF/HF 48000, 24000 or 12000; VHF 48000
    double centreHz = 1700;       ///< MF/HF audio centre (VHF is fixed at 1700, the 1300/2100 pair)
};

class Receiver {
public:
    explicit Receiver(const RxConfig& cfg);
    void process(const float* x, int n);
    /** Change the MF/HF audio centre in place (a listener's dial moved a little). */
    void setCentre(double hz);

    std::function<void(const Call&)> onCall;
    /** 0 listening · 1 a call is coming in (phasing found, format read). */
    std::function<void(int)> onState;

    struct Stats {
        unsigned long syncs = 0;        ///< phasing found
        unsigned long falseSyncs = 0;   ///< …that did not become a call (format unreadable, signal gone)
        unsigned long calls = 0, corrected = 0, failed = 0;
        unsigned long inverted = 0;     ///< calls found in the mirrored sense
    };
    const Stats& stats() const { return st_; }
    int state() const { return state_; }
    double samplesPerBit() const { return S_; }

private:
    struct Cand;
    void frontEnd(const float* x, int n);
    void pushSoft(float hiMag, float loMag);
    void trigger(int64_t t, int r, int pol);
    void evalPending();
    bool readSlot(Cand& c, int j);
    void advance(Cand& c);
    bool finishPrimary(Cand& c, int mEos);
    void emit(Cand& c);
    float slotSoft(int64_t idx) const { return ring_[(size_t)(idx & ringMask_)]; }
    void setState(int s);

    RxConfig cfg_;
    int S_ = 12;                      // samples per bit at the correlator rate
    double fd_ = 1200;                // correlator rate
    double toneHz_ = 85;              // ± from the centre
    // front end: NCO + two decimating FIRs (the second is skipped on VHF)
    double ncoRe_ = 1, ncoIm_ = 0, ncoStRe_ = 1, ncoStIm_ = 0; int ncoN_ = 0;
    int d1_ = 8, d2_ = 5, ph1_ = 0, ph2_ = 0;
    std::vector<float> h1_, h2_;
    std::vector<float> b1re_, b1im_, b2re_, b2im_;   // delay lines, doubled so a window is contiguous
    int p1_ = 0, p2_ = 0;
    // tone correlators
    double tRe_ = 1, tIm_ = 0, tStRe_ = 1, tStIm_ = 0; int tN_ = 0;
    std::vector<float> hiRe_, hiIm_, loRe_, loIm_; int bx_ = 0;
    float hiOn_ = 0, loOn_ = 0, noise_ = 0;          // the tones' "on" levels and the noise level (slow)
    // soft-sample ring and the hard registers for the phasing search
    std::vector<float> ring_;
    int64_t ringMask_ = 0;
    int64_t n_ = 0;                   // soft samples so far
    std::vector<uint16_t> reg_;       // per bit phase: the last 10 hard bits, newest in bit 0
    int8_t lut_[1024];                // register → RX phasing index r (0–7), -1 none
    struct Pending { int64_t t; int r, pol; };
    std::vector<Pending> pending_;
    std::vector<Cand*> cands_;
    int state_ = 0;
    Stats st_;
    // the last call emitted (a transmission's repeats are the client's business; this only stops one call twice)
public:
    ~Receiver();
    Receiver(const Receiver&) = delete; Receiver& operator=(const Receiver&) = delete;
};

/** One line of JSON for a call (the clients' "dsc_call" data), with `extra` fields (already JSON) spliced in. */
std::string toJson(const Call& c, const std::string& extra = "");

} // namespace dsc
} // namespace vibe
