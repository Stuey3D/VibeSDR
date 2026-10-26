// decoders/jtty_decoder.h — JTTY (the WSJT-X 3.2 keyboard mode): EVERY JTTY signal in the passband, with no time slots.
//
// ★★★ WHY (Stuart, 2026-10-26): "look into JTTY as it is new, it is a hybrid of FT8 and RTTY, if we can get in early this
//     would give us a significant boost". The protocol, with each fact's source and confidence, is docs/v12/JTTY.md.
//
// ── The air interface in one paragraph (docs/v12/JTTY.md §1) ──────────────────────────────────────────────────────
//  A transmission is 1–16 back-to-back FRAMES of 59 symbols: 13 sync then 46 data. 4-GFSK (BT 2.0, h = 1), 31.25 baud,
//  tones 31.25 Hz apart, so one frame is 1.888 s and ≈ 127 Hz wide. Each frame carries ONE 34-bit source atom (32 bits of
//  grammar, a reserved zero, an end-of-message flag), a CRC-12 (x¹²+x¹¹+x³+x²+x+1) makes 46 bits, a tail-biting rate-½ K=10
//  convolutional code (1167, 1545 octal) makes 92, Gray-mapped two bits a tone. A frame can start at ANY sample.
//
// ── How this receiver finds and reads them ───────────────────────────────────────────────────────────────────────
//  1. ONE spectrogram for the whole passband: a 768-point real FFT (PFFFT — NEON / SSE) of a one-symbol window every
//     eighth of a symbol. Bins are half a tone (15.6 Hz), so at the right alignment every tone sits on a bin centre and
//     leaks nothing into its neighbours. A passband with ten JTTY stations costs the same transform as an empty one.
//  2. A SYNC SCORE for every (eighth-symbol, half-tone) cell: the 13 sync tones' power against the other three tones'
//     power at the same instants. Self-normalising (a carrier or RTTY scores ≈ 1, noise ≈ 1, a JTTY frame 1 + Es/N0),
//     vector loops over frequency (four bins an instruction), scalar twin as the reference.
//  3. A local peak above the threshold is a candidate. It is mixed down, low-passed and decimated to 750 Hz (a 97-tap
//     FIR — SIMD dot products), then searched finely: the sync alone every 1/24 symbol and 1.5 Hz, then all 59 symbols
//     (decision-directed) every 0.5 Hz. A best found at the edge of the search is searched again, centred there.
//  4. Every symbol's four tone correlations → a soft metric per tone (ln I₀ of the matched-filter output — the exact
//     noncoherent likelihood, not a hard decision) → a TAIL-BITING LIST VITERBI (two circular passes, every end state
//     traced back, the best paths that close on themselves) → each path through the CRC-12, the reserved-zero bit and the
//     WHOLE source grammar. If none passes, a COHERENT pass: h = 1 makes the transmitter phase-continuous, so the best
//     path's tones give the carrier phase symbol by symbol, and the coherent likelihood 2·A·Re(z·e^−jψ)/N is decoded again.
//  5. ★★ The CRC alone lets a path through noise pass now and then (12 bits, many candidates a second). So a frame must
//     also be CONSISTENT WITH ITSELF: its tones must line up in phase (COHERENCE — noise ≈ 1, a frame ≈ 5–8; measured
//     gates per pass), and a strongly synced frame must put its tones where its strength says (a strong FT8 signal can
//     light a sync; it cannot fake 38 of 46 data symbols). Ten hours of noise and an hour of real FT8/FT4: no frame.
//     A frame that decoded also says where the NEXT frame of its transmission is (59 symbols on, same frequency): that
//     position is tried directly even if its sync never crossed the threshold.
//  6. Frames on one frequency in sequence are one MESSAGE; the end-of-message flag closes it. Calls in call atoms are
//     SPOTS ("CQ K1ABC CQ" and "TU K1ABC CQ" are a station calling; any other call atom is a station in a QSO).
//
// ★ Nothing here allocates per sample, takes a lock or knows about sockets: the caller (vibe_jtty_run.h) owns that.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace jtty {

// ── Constants of the air interface ────────────────────────────────────────────────────────────────────────────────
constexpr int    kSyncSyms = 13, kDataSyms = 46, kFrameSyms = 59;
constexpr int    kPayloadBits = 34, kCrcBits = 12, kInfoBits = 46;
constexpr int    kMaxFrames = 16;          ///< a message is at most sixteen frames
constexpr double kBaud = 31.25;            ///< = tone spacing (h = 1)
constexpr double kFrameSec = kFrameSyms / kBaud;   ///< 1.888 s
/** The sync tones (13 symbols, values 0–3). */
extern const uint8_t kSync[kSyncSyms];

// ── The source grammar (docs/v12/JTTY.md §3) ──────────────────────────────────────────────────────────────────────
enum class Kind : uint8_t { Call, ExchNum, ExchLoc, ExchPair, NumTime, Control, Grid4, Text5 };
/** Call actions (Kind::Call, `sub`). */
enum CallAction { kCq = 0, kCall = 1, kTuCq = 2, kCallTu = 3, kCallAgn = 4, kTuNow = 5 };
/** EXCH_NUM kinds (`sub`). */
enum NumKind { kSerial = 0, kCqZone = 1, kItuZone = 2, kAge = 3, kPower = 4, kCheck = 5, kLicenceYear = 6, kGenericNum = 7 };
/** EXCH_LOC kinds (`sub`). */
enum LocKind { kStateProvince = 0, kArrlSection = 1, kCountryPrefix = 2, kGenericQth = 3, kLocalAdmin = 4 };
/** EXCH_PAIR schemas (`sub`). */
enum PairSchema { kZoneLoc3 = 0, kClassSection = 1 };

/** One source atom — what one frame carries.
 *    Call     sub = action, text = the call
 *    ExchNum  sub = kind, role (1 = "599 " prefix), value
 *    ExchLoc  sub = kind, role, text = 2–3 base-36 characters
 *    ExchPair sub = schema; ZONE_LOC3: value = zone, text = token; CLASS_SECTION: value = count, value2 = section index
 *             (1–86), text = class letter A–F
 *    NumTime  role, value = serial (0–16383), value2 = minute of the day (0–1439)
 *    Control  sub = phrase id (0–17)
 *    Grid4    role, text = 4-character locator
 *    Text5    text = exactly five characters of the 64-character alphabet */
struct Atom {
    Kind kind = Kind::Text5;
    int  sub = 0, role = 0, value = 0, value2 = 0;
    std::string text;
};

/** The 64-character alphabet of TEXT5 frames. */
extern const char kAlphabet[65];
/** ★ The 34-bit word: bit 1 (the grammar's first) is the integer's bit 33; bit 33 is reserved (zero), bit 34 is EOM. */
bool packAtom(const Atom& a, bool eom, uint64_t& bits34);
/** ★ Every validity rule of the grammar: a word that fails ANY of them is not a frame (false). */
bool unpackAtom(uint64_t bits34, Atom& a, bool& eom);
/** The canonical rendering ("CQ K1ABC CQ", "599 123", "HELLO" — TEXT5 as its five characters, padding included). */
std::string render(const Atom& a);

/** Operator text folded into the alphabet: upper case, single spaces, anything else '#'. */
std::string normalise(const std::string& text);
/** ★ Automatic packing (the Unknown exchange profile): the FEWEST frames that reproduce the normalised text exactly —
 *  call atoms, control phrases, generic numbers and QTHs, grids, class/section and TEXT5. The last frame carries EOM.
 *  False when the text needs more than sixteen frames. Used by the tests' transmitter; we never transmit. */
bool packMessage(const std::string& text, std::vector<uint64_t>& frames);
/** The receiver's text for a sequence of atoms: structured atoms rendered with one separating space, TEXT5 verbatim,
 *  the trailing padding trimmed. */
std::string renderMessage(const std::vector<Atom>& atoms);

/** ★ A STANDARD callsign that the FT8-family 28-bit field carries exactly (prefix with a letter, call area digit at
 *  position 2 or 3, a 1–3 letter suffix, ≤ 6 characters, not starting with Q). */
bool standardCall(const std::string& call);
bool packCall28(const std::string& call, uint32_t& n28);
bool unpackCall28(uint32_t n28, std::string& call);
/** ARRL/RAC section by its index 1–86 ("" outside), and the reverse (0 = unknown). */
const char* arrlSection(int index);
int arrlSectionIndex(const std::string& s);

// ── FEC and the waveform ──────────────────────────────────────────────────────────────────────────────────────────
uint16_t crc12(uint64_t bits34);
/** 34 bits → 59 tones (sync + data). */
void encodeFrame(uint64_t bits34, uint8_t tones[kFrameSyms]);
/** ★ The 4-GFSK waveform of `tones` (BT 2.0), lowest tone at `f0` Hz, ADDED into `out` from sample `at` (grown as
 *  needed), peak amplitude `amp`, with the transmitter's raised-cosine ramps. Our own modulator, for the tests. */
void synthesise(const uint8_t* tones, int nTones, double f0, double rate, float amp, std::vector<float>& out, size_t at,
                double phase0 = 0.0);

// ── The receiver ──────────────────────────────────────────────────────────────────────────────────────────────────
/** One decoded frame. */
struct Frame {
    int64_t sample = 0;        ///< the frame's first sample (stream count at the decoder's rate)
    double  audioHz = 0;       ///< tone 0's frequency
    float   snrDb = 0;         ///< WSJT-X convention: signal over noise in 2500 Hz
    float   quality = 0;       ///< 0..1
    int     rank = 0;          ///< which Viterbi path passed (1 = the best)
    int     agree = 0;         ///< data symbols whose strongest tone is the decoded one (of 46)
    bool    eom = false;
    bool    continuation = false;   ///< found where the previous frame said it would be, not by its sync
    uint64_t bits = 0;
    Atom    atom;
    int64_t utcMs = 0;
    int     signalId = 0;
};
/** One callsign heard. */
struct Spot {
    std::string call;
    double audioHz = 0;
    float  snrDb = 0;
    std::string how;      ///< "CQ" (calling CQ, or TU … CQ) or "QSO" (in an exchange)
    int64_t utcMs = 0;
    int    signalId = 0;
};
/** One signal (a station's frequency), as a list row. */
struct SignalView {
    int     id = 0;
    double  audioHz = 0;
    float   snrDb = 0;
    float   quality = 0;
    int64_t firstMs = 0, lastMs = 0;
    std::string call;     ///< the call that CQ'd here, else the last call heard here ("" none)
    std::string text;     ///< the recent messages, oldest first (bounded); an unfinished one last
    bool    active = false;
    unsigned frames = 0;
};
const char* qualityWord(float q);   ///< "high" / "medium" / "low" / "poor"

class Decoder {
public:
    struct Config {
        double rate = 12000;          ///< ★ must be 12000 (the caller decimates) — the symbol is 384 samples
        double loHz = 150, hiHz = 3300;   ///< where tone 0 may be (+94 Hz for the top tone)
        float  syncThreshold = 0;     ///< 0 = the built-in, measured value
        int    listSize = 4;          ///< Viterbi paths tried against the CRC, per pass (noncoherent, coherent)
        int    keepGoneSec = 900;     ///< how long a signal no longer heard stays in signals()
        bool   continuation = true;   ///< try the next frame's position directly (step 5 above)
    };
    explicit Decoder(const Config& c);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    void feed(const float* x, int n);

    std::function<void(const Frame&)> onFrame;
    /** A message changed: its text so far, `complete` once its EOM frame (or a timeout) closed it. */
    std::function<void(int signalId, double audioHz, const std::string& text, bool complete)> onMessage;
    std::function<void(const Spot&)> onSpot;

    std::vector<SignalView> signals(bool includeGone = true) const;
    void setStartUtcMs(int64_t ms);
    int64_t nowMs() const;

    // ── counters, for the tests, the admin line and the health pill ──
    uint64_t samples() const;
    uint64_t candidates() const;      ///< sync peaks tried
    uint64_t continuations() const;   ///< next-frame positions tried
    uint64_t framesDecoded() const;
    uint64_t crcRejected() const;     ///< candidates whose every path failed
    int      activeSignals() const;
    float    threshold() const;
    static const char* simdName();

    // ── exposed for the tests ──
    /** The sync score field's raw statistic at one cell (for calibrating the threshold on noise). */
    struct Stats { double meanScore = 0; uint64_t cells = 0; uint64_t above = 0; };
    Stats scoreStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace jtty
} // namespace vibe
