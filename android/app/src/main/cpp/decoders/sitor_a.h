// VibeSDR V12 — SITOR-A (AMTOR mode A): the maritime ARQ direct-printing telegraph, as a LISTENER hears it.
//
// Built from the published standards — ITU-R M.476-5 (direct-printing telegraph equipment, mode A ARQ), ITU-R M.625-4
// (the same with automatic identification: CS4/CS5, 7-signal calls, check-sums) and ITU-R M.491-1 (translation between
// identity numbers and identification signals). No other decoder's source was read for or copied into this file.
//
// ── The signal (M.476 Annex 1 §1, M.625 §1) ──────────────────────────────────────────────────────────────────────
//  • F1B/J2B, 100 Bd, 170 Hz shift. "B" is the HIGHER emitted frequency, "Y" the lower (M.476 Table 1 note 1).
//  • A 7-unit constant-ratio code: every valid signal has four B and three Y (35 words). Bit position 1 is sent first.
//  • A 450 ms cycle (M.476 §3.1.1.3). The information sending station (ISS) sends a BLOCK of three signals (210 ms);
//    the information receiving station (IRS) answers with one CONTROL SIGNAL (70 ms) — CS1 "send block 1", CS2 "send
//    block 2", CS3 "change over" (and in M.625, CS4/CS5 "start identification" after a call / a rephasing call).
//  • The MASTER (the calling station) keeps the clock: its transmission starts at the same moment of every cycle. The
//    SLAVE answers a fixed time after the master's signal ENDS (tE, M.476 §3.1.1.6). So when the master's 210 ms block
//    becomes a 70 ms control signal at a change-over, the master's start stays put and the slave's moves 140 ms earlier
//    — which is how this receiver tells master from slave without hearing the call.
//
// ── What a listener has to do that the two stations don't ──────────────────────────────────────────────────────────
//  ★ HEAR TWO TRANSMITTERS. Each station has its own level, its own frequency error and its own timing; the tracker
//    keeps one of each per station, on that station's own 70 ms character grid, and decides every cycle which of them
//    sent what (a block, a control signal, or nothing) — by energy and code validity, with the protocol's own rules
//    for what may change as the prior: one station sends blocks at a time, which one changes only at a change-over,
//    and a slave's burst starts where the master's just-ended burst puts it.
//  ★ NO DUPLICATES. A block the IRS asks for again (it repeats its control signal, §3.1.3.3/4) is the SAME block: it is
//    printed once. The request each block answers is read from the IRS's control signal; when the IRS cannot be heard,
//    the copies are compared.
//  ★ REPEATS ARE A SECOND COPY, NOT A NUISANCE. The soft values of every copy of a block are summed before it is
//    decoded (the copies are independent fades), so a block nobody could read once can be read from two weak copies —
//    the way the NAVTEX receiver uses its DX/RX copies. Each character is the maximum-likelihood word of the 35: the
//    four largest of its seven soft values are the B elements.
//  ★ CALL-UP AND IDENTIFICATION. The call blocks name the CALLED station (4 signals → a 5-digit selcall number, M.491
//    Annex I; 7 signals → a 9-digit MMSI, Annex II); the M.625 identification blocks name the CALLER, and the called
//    station's check-sum signals (§2.5) confirm the called identity.
//
// Input: mono audio (float, or int16) at any rate ≥ 4 kHz, with the two tones centred on `centreHz` (default 1000).
// Cost: two short decimating FIRs to ~2 kHz (NEON / SSE2 / scalar), then work per BIT, not per sample, while locked.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace sitora {

// ── The code (M.476 Annex 1 §2, Tables 1 and 2; M.625 §2.2–2.3) ─────────────────────────────────────────────────
/** A 7-unit signal from its B/Y spelling as printed in the tables ("BBBYYYB"): bit i = position i+1, 1 = B. */
constexpr uint8_t sig(const char* by) {
    uint8_t v = 0;
    for (int i = 0; i < 7; i++) if (by[i] == 'B') v = (uint8_t)(v | (1 << i));
    return v;
}
constexpr uint8_t kCs1 = sig("BYBYYBB"), kCs2 = sig("YBYBYBB"), kCs3 = sig("BYYBBYB"), kCs4 = sig("BYBYBBY"),
                  kCs5 = sig("BYYBYBB"), kBeta = sig("BBYYBBY"), kAlpha = sig("BBBBYYY"), kRq = sig("YBBYYBB");
constexpr uint8_t kLtrs = sig("YBYBBYB"), kFigs = sig("YBBYBBY"), kCr = sig("YYYBBBB"), kLf = sig("YYBBYBB"),
                  kSpace = sig("YYBBBYB"), kNoInfo = sig("YBYBYBB");   // combination 32 — the same word as CS2

bool valid(uint8_t w);                 ///< four B, three Y
/** Letter case of a traffic signal (A–Z), 0 if it has none (shifts, CR, LF, space, combination 32, service signals). */
char32_t letterOf(uint8_t w);
/** Figure case: '-', '?', ':' … ; 0 where Table 1 assigns nothing (F, G, H), and for D (WRU) and J (the bell). */
char32_t figureOf(uint8_t w);
uint8_t letterSignal(char32_t c);      ///< the signal whose letter case is `c` (A–Z), 0 if none
uint8_t figureSignal(char32_t c);      ///< the signal whose figure case is `c`, 0 if none

// ── Identities (M.625 §2.4–2.5, Tables 3a/3b; M.491-1 Annexes I and II) ─────────────────────────────────────────────
bool isIdSignal(char32_t c);           ///< one of the 20 identification signals A B C D E F I K M O P Q R S T U V X Y Z
int idValue(char32_t c);               ///< Table 3a: its equivalent number 0–19, -1 if not an identification signal
char32_t checksumSignal(int cn);       ///< Table 3b: the check-sum signal for a check-sum number 0–19
/** The three check-sum signals of a 7-signal identity (§2.5: N1⊕N2⊕N3, N3⊕N4⊕N5, N5⊕N6⊕N7, ⊕ = modulo 20). */
std::string checksums(const std::string& id7);
/** The identity NUMBER of 4 or 7 identification signals: 4 → the 5-digit (or 4-digit) selcall number by M.491 Annex I;
 *  7 → the 9-digit number by Annex II (base 20, Table III). Empty if the signals are not a valid identity. */
std::string identityNumber(const std::string& ids);
/** The reverse: 4, 5 or 9 digits → the 4 or 7 identification signals (for the synthesiser and the tests). */
std::string identitySignals(const std::string& number);

// ── The receiver ─────────────────────────────────────────────────────────────────────────────────────────────────
struct Options {
    bool combine = true;      ///< sum the soft values of every copy of a repeated block before deciding it
    bool afc = true;          ///< follow each station's own frequency error (±80 Hz)
    bool fineTiming = true;   ///< follow each station's timing to a fraction of a bit
    bool atc = true;          ///< each tone's own amplitude in the soft values (selective fading)
};

enum class Link { NoSignal = 0, Searching, Calling, Identifying, Traffic, Idle, ChangeOver, Ended };
const char* linkName(Link l);

struct Status {
    Link link = Link::NoSignal;
    int iss = -1;              ///< the station sending the blocks now (0/1), -1 unknown
    int master = -1;           ///< which station is the master (the caller), -1 not yet known
    bool heard[2] = {false, false};
    std::string ids[2];        ///< identification signals as received ("QCXT", "PEARDBY"); empty if not heard
    std::string numbers[2];    ///< their identity numbers ("32610", "364775427")
    bool idChecked = false;    ///< the called station's check-sums matched its identity
    unsigned long blocks = 0;  ///< information blocks printed
    unsigned long repeats = 0; ///< blocks the IRS asked for again (and RQ blocks), i.e. the RQ count
    unsigned long lost = 0;    ///< characters printed as '_' (no copy could be read)
    unsigned long combined = 0;///< blocks read only by summing two or more copies
    double snrDb[2] = {0, 0};  ///< per station, in the 100 Hz bit bandwidth
    double offsetHz[2] = {0, 0};
    bool inverted = false;
};

class Receiver {
public:
    explicit Receiver(double rate, double centreHz = 1000.0, const Options& o = Options());
    ~Receiver();
    Receiver(const Receiver&) = delete; Receiver& operator=(const Receiver&) = delete;
    void process(const float* x, int n);
    void process(const int16_t* x, int n);
    void setCentre(double hz);
    double centre() const { return centre_; }
    /** Forget the link (a retune): both stations, the text state, the counts. */
    void reset();

    /** A printed character, from station `side` (0/1) — the ISS of the moment. '_' = a character no copy could read. */
    std::function<void(int side, char32_t c)> onChar;
    /** Anything in Status changed (link state, who is sending, identities, counts — counts at most once a block). */
    std::function<void(const Status&)> onStatus;
    /** A protocol event worth a line in a log: "call QCXT (32610)", "over", "end of communication" … */
    std::function<void(const std::string&)> onEvent;

    const Status& status() const { return st_; }

    struct Impl;
private:
    Impl* p_;
    Status st_;
    double centre_;
};

} // namespace sitora
} // namespace vibe
