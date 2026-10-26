// VibeSDR V4 — FT8 / FT4 decoder (wraps kgoba's ft8_lib, MIT).
//
// Buffers mono audio, aligns to UTC FT8 (15 s) / FT4 (7.5 s) slots, runs the
// ft8_lib STFT monitor + candidate search + LDPC decode, and reports each
// decoded message via onSpot. The shim turns those into {type:'digital_spot'}
// JSON frames on /ws/dxcluster so the existing VibeSDR digital-spots list shows
// local decodes (same mechanism as the UberSDR server skimmer feed).
#pragma once
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <functional>
#include <string>
#include <memory>
#include <vector>

#include "ftx_fine.h"

// ft8_lib C core
extern "C" {
#include "common/monitor.h"
#include "ft8/decode.h"
}

namespace vibe {

/* ★★ THE THREE FRAMES ON ONE CORE (2026-10-26). FT2 (IU8LMC's fast mode, Decodium, Feb 2026) is FT4's frame exactly —
 *  4-GFSK BT 1.0, 105 symbols (ramp, four 4-symbol Costas groups, 87 data symbols), LDPC(174,91) + CRC-14, the 77-bit
 *  message XORed with FT4's sequence, FT4's Gray map — at HALF the symbol length: 288 samples at 12 kHz (24 ms,
 *  41.67 Hz tones, ~167 Hz wide), a 3.75 s T/R period. So it is a parameterisation here, not another decoder:
 *  the monitor's symbol period and slot, and a wider time search (see kFt2*). docs/v12/JS8-FT2.md has the sources. */
enum class FtxMode { FT8, FT4, FT2 };
constexpr float kFt2SymbolPeriod = 0.024f;   ///< 288 samples at 12 kHz
constexpr float kFt2SlotTime     = 3.75f;    ///< the T/R period
/* ★ FT2's window and search. Decodium starts its transmissions 0, 100 or 300 ms into the slot depending on the
 *  version and platform, and its decoder looks -0.52 .. +1.52 s; a 24 ms block makes ft8_lib's -10 .. 20 blocks only
 *  -0.24 .. +0.48 s. So the window holds 5 s from the boundary (overlapping the next slot by 1.25 s — a signal of the
 *  next slot sits beyond the search) and the search runs -22 .. +64 blocks (-0.53 .. +1.54 s). */
constexpr float kFt2WindowTime   = 5.0f;
constexpr int   kFt2TimeMin      = -22;
constexpr int   kFt2TimeMax      = 65;
constexpr int   kFt4TimeMin      = -24;  ///< FT4's search, in 48 ms blocks (see the constructor)
constexpr int   kFt4TimeMax      = 36;

/* ★★★ THE HASHED-CALLSIGN TABLE (audit 2026-10-04, row 10). A non-standard call ("PJ4/K1ABC") goes over the air as a
 *  10-, 12- or 22-bit hash, and only a table of calls already heard in full can turn it back into a call. The old one
 *  held 512 calls, never forgot one, and stopped SAVING when full — every decoded call is saved, so on a busy band it
 *  filled within hours and the receiver could resolve nothing it heard after that. And it returned the FIRST call
 *  whose hash matched: 512 calls in 1024 ten-bit buckets put a second call under a 10-bit hash about half the time,
 *  so a hashed call printed as somebody else's.
 *  ★ Now: least-recently-used (each entry carries the stamp of its last save or lookup; a full table evicts the
 *    oldest), and a hash that matches two DIFFERENT calls is unknown — "<...>", which the spot filter drops —
 *    rather than a guess. WSJT-X keeps its table the same way (recent calls, never a guess). */
class Ft8CallHashTable {
public:
    static constexpr int kCapacity = 512;
    /** ft8_lib's save_hash: `call` heard in full, its 22-bit hash `n22` (the 12- and 10-bit hashes are its top bits). */
    void save(const char* call, uint32_t n22);
    /** ft8_lib's lookup_hash: `bits` = 10, 12 or 22. Writes the call (NUL-terminated, at most 11 chars) into `out[12]`,
     *  or "" and false when no call — or more than one different call — has that hash. */
    bool lookup(int bits, uint32_t hash, char* out);
    void clear();
    int  size();
private:
    struct Entry { char call[12]; uint32_t n22; uint64_t stamp; };   // stamp 0 = empty
    std::mutex mtx_;
    Entry e_[kCapacity] = {};
    uint64_t clock_ = 0;
};
/** The one table every FT8/FT4 decoder shares (call hashes are global — a call heard on FT8 resolves on FT4). */
Ft8CallHashTable& ft8CallHashes();

/** OSD order 1 on the 174 bit log-likelihoods (log p(1)/p(0), codeword order): fills `a91` (message + CRC, packed)
 *  and returns the hard-decision disagreements of the closest CRC-valid codeword, or -1 when none is within
 *  `maxErrors`. Exposed for test-ft8-fast. */
int ft8Osd1(const float* llr, int maxErrors, uint8_t a91[12]);
/** OSD-1, then every pair of flips among the `pairSpan` least reliable independent bits (0 = ft8Osd1 exactly). */
int ft8Osd(const float* llr, int maxErrors, int pairSpan, uint8_t a91[12]);


/* ★★★ A-PRIORI DECODING FOR A RECEIVER (2026-10-26, the sensitivity work). The FT4/FT8 protocol paper (K1JT, K9AN,
 *  G4WJS, QEX 2020) describes a-priori (AP) decoding: message bits the decoder can EXPECT are given to the LDPC
 *  decoder as near-certain, and a codeword of which 29-77 of the 77 message bits are already known is found several
 *  dB below where blind decoding gives up. A transceiver expects its own call, its QSO partner's and "CQ"; a RECEIVER
 *  has no call of its own, so here the expectations come from what this decoder has HEARD on this dial lately:
 *   · "CQ ? ?"            — any CQ (29 bits: the CQ token, then i3).
 *   · "X ? ?" / "? X ?"   — a call heard in the last 10 minutes, as the station called (a reply to its CQ) or as the
 *                            caller (its next transmission).
 *   · "X Y ?"             — a pair heard working each other, either way round (58 bits: the report or grid unknown).
 *   · "X Y RR73/73/RRR"   — the pair's sign-off, every bit known.
 *  Hundreds of hypotheses on a busy band, so each is SCREENED first — the agreement of its known bits with the
 *  candidate's own likelihoods (one multiply-add a bit) — and only the best few are decoded, with BP then OSD.
 *  ★★ Guards (every AP decode): the CRC-14 over all 77 bits; the known bits as hypothesised; the message unpacks as a
 *  standard message whose other call is a standard callsign and whose last field is a real grid or report; and the
 *  received hard decisions on the bits NOT assumed disagree with the codeword no more than a near miss does
 *  (ftx_sweep --noise with a full book: 0 false). A decode made with AP never feeds the book.
 *  One book per decoder: a dial and a mode. */
class FtxApBook {
public:
    /** `ft4` = the frame is scrambled (FT4/FT2); `maxAgeSlots` = how long a call is remembered. */
    FtxApBook(bool ft4, int maxAgeSlots) : ft4_(ft4), maxAge_(maxAgeSlots) {}
    /** A new slot is being decoded (ages the book). */
    void beginSlot();
    /** A message decoded WITHOUT a-priori help. */
    void heard(const ftx_message_t& msg);
    struct Hyp {
        uint64_t mask[2] = {0, 0}, val[2] = {0, 0};   // over codeword bits 0..76 (bit i: word i/64, bit i%64)
        int nbits = 0;
        uint8_t kind = 0;                              // 1 CQ, 2 call1, 3 call2, 4 pair, 5 sign-off
    };
    /** What the hypotheses are made of, rebuilt when the book changed: the CQ field's bits, i3's, the three sign-off
     *  tails' (bits 58..73: "RR73", "73", "RRR" — the same whatever the calls), and per call its bits as the first
     *  and as the second call; pairs as indices into those. The decoder scores every hypothesis from these by
     *  region sums, never building most of them. */
    struct Index {
        uint64_t cq[2] = {0, 0}, i3[2] = {0, 0}, tail[3][2] = {};
        std::vector<uint64_t> c1, c2;                  // 2 words per call
        std::vector<std::pair<int, int>> pairs;        // (first, second) as heard; both ways are hypothesised
        bool ok = false;
    };
    const Index& index();
    /** The hypothesis `kind` (1-5) for calls a, b (indices into index(); -1 = unused) and tail t (kind 5). */
    static Hyp make(const Index& x, int kind, int a, int b, int t);
    int calls() const { return (int)calls_.size(); }
    /** For the tests and the bench: put a call (and optionally its partner) in as if heard. */
    void seed(const std::string& call, const std::string& partner = std::string());
    static bool bitsOf(const char* text, bool ft4, uint64_t out[2]);   // a message's 77 codeword bits
private:
    struct Call { std::string call; uint64_t c1[2], c2[2]; int64_t last; };
    struct Pair { std::string a, b; int64_t last; };          // by name: calls_ is erased from
    void touchCall(const std::string& c);
    int  findCall(const std::string& c) const;
    bool ft4_;
    int  maxAge_;
    int64_t slot_ = 0;
    std::vector<Call> calls_;
    std::vector<Pair> pairs_;
    Index idx_;
    bool dirty_ = true;
};

/** One message decoded from a slot. */
struct Ft8Result {
    ftx_message_t msg;
    int   snr  = 0;      ///< dB in 2500 Hz, WSJT-X's definition (see snrDb in the .cpp)
    float hz   = 0;      ///< audio frequency of tone 0
    float dt   = 0;      ///< seconds after the slot's nominal start (+0.5 s FT8 / +0.5 s FT4), WSJT-X's DT
    int   pass = 1;      ///< which pass found it (2+ = only after the stronger ones were subtracted)
    bool  osd  = false;  ///< recovered by OSD after belief propagation failed
    bool  fine = false;  ///< decoded only on the second look (ftx_fine.h)
    int   ap   = 0;      ///< decoded with a-priori bits: FtxApBook::Hyp::kind (0 = none)
};

/* ★★★ THE PASSES ARE BOUGHT, NOT GIVEN (2026-10-05). Stuart: "FT8 we have to approach with caution, that can end
 *  up being very CPU heavy." The decode runs inside the server process beside the radio's DSP, on a Pi 2 as well as
 *  on a Mac. Pass 1 is exactly the old decode (same candidates, same LDPC), so a box that never earns a second pass
 *  costs what it cost before. A second or third pass — subtract every signal already decoded, rebuild the
 *  spectrogram, search again — runs only when `mayRunPass` says yes, and the live decoder only says yes while the
 *  passes so far left most of the slot unused AND the server reports no load (Ft8Decoder::loadProbe). */
struct Ft8SlotOptions {
    int maxPasses     = 1;
    int ldpcIters     = 25;
    int maxCandidates = 140;
    int minScore      = 10;
    /** OSD-1 after a failed LDPC: accept a codeword at most this many hard errors away (0 = off), only for
     *  candidates whose sync score is at least osdMinScore. */
    int osdMaxErrors  = 0;
    int osdMinScore   = 0;
    /** ★★ The second look (ftx_fine.h, 2026-10-26): a candidate the waterfall's likelihoods could not decode is
     *  measured again from the audio — fine sync, a matched filter per symbol, bit likelihoods over one, two and
     *  three symbols — and decoded again (BP on each set; OSD when osdMaxErrors > 0). 0 = off (the old decoder). */
    int fine          = 0;
    /** Costas symbols that must be their own strongest tone before the second look spends more (noise stops here).
     *  0 = the mode's default (kFineMinSync*). */
    int fineMinSync   = 0;
    /** The waterfall's BP stops after this many iterations without fewer unsatisfied checks (0 = all ldpcIters,
     *  the old decoder exactly). */
    int bpStall       = 0;
    /** Only decodes at least this strong (dB) are subtracted before the next pass. */
    int subtractMinSnr = -99;
    /** The second look's OSD also tries pairs of flips among this many least reliable bits (0 = OSD-1). */
    int osdPairSpan   = 0;
    /** Asked before pass 2 and 3 with the ms this decode has taken so far and the ms the last pass took.
     *  Empty = always yes (the bench). */
    std::function<bool(int nextPass, double elapsedMs, double lastPassMs)> mayRunPass;
    const std::atomic<bool>* abort = nullptr;
    /** ★ A-priori decoding (FtxApBook): the book of what this dial heard lately, or null = off. The second look must
     *  be on; AP is tried only on its near misses. */
    FtxApBook* ap = nullptr;
    int apMaxCands = 0;              ///< hypotheses decoded per candidate after screening (0 = the default, 3)
    int apMaxBpErr = 0;              ///< a near miss for AP: BP within this many checks (0 = the default; tests raise it)
};
struct Ft8SlotStats {
    int    passes = 0;
    double passMs[3] = {0, 0, 0};
    int    passDecodes[3] = {0, 0, 0};
    /* ★ Where each pass's time went (2026-10-25, the CPU work): the profile the optimisations are judged by. A dozen
     *  clock reads per candidate at most — under 0.1 % of a pass. */
    enum Stage { kSubtract, kWaterfall, kSearch, kLdpc, kOsd, kPost, kFine, kStages };
    double stageMs[3][kStages] = {};
    int    candidates[3] = {0, 0, 0};   ///< candidates the search returned
    int    osdTried[3] = {0, 0, 0};     ///< of them, how many failed BP and went to OSD
    int    fineTried[3] = {0, 0, 0};    ///< of them, how many had the second look
    int    finePassed[3] = {0, 0, 0};   ///< … and passed its sync test
    int    fineDecodes[3] = {0, 0, 0};  ///< … and decoded
    int    apTried[3] = {0, 0, 0};      ///< near misses that went to a-priori decoding
    int    apDecodes[3] = {0, 0, 0};    ///< … and decoded
};

/** One slot, decoded synchronously: what the worker thread runs, and what the bench and the tests drive directly.
 *  Owns its spectrogram and scratch, so one instance must not decode two slots at once. */
class Ft8SlotDecoder {
public:
    Ft8SlotDecoder(int sampleRate, bool ft4) : Ft8SlotDecoder(sampleRate, ft4 ? FtxMode::FT4 : FtxMode::FT8) {}
    Ft8SlotDecoder(int sampleRate, FtxMode mode);
    ~Ft8SlotDecoder();
    FtxMode mode() const { return mode_; }
    Ft8SlotDecoder(const Ft8SlotDecoder&) = delete;
    Ft8SlotDecoder& operator=(const Ft8SlotDecoder&) = delete;

    /** ★ Where the decoded window starts, relative to the UTC slot boundary (seconds; negative = before it). */
    static float leadSec(bool ft4);
    /** Samples one slot's decode reads, starting at slot boundary + leadSec(). */
    int windowSamples() const { return windowSamples_; }
    /** Decode `audio` (windowSamples() of it, from slot + leadSec()). ★ The buffer is MODIFIED: passes 2 and 3
     *  subtract what pass 1 decoded from it. Results are appended to `out` in the order found. */
    void decode(float* audio, int n, const Ft8SlotOptions& o, std::vector<Ft8Result>& out, Ft8SlotStats& st);

private:
    void buildWaterfall(const float* audio, int n);
    float colMean(size_t column);
    int  snrDb(const ftx_candidate_t& c, const uint8_t* tones);
    void subtract(float* audio, int n, const uint8_t* tones, double startSample, double f0);
    bool fineDecode(const float* audio, int n, double start, double f0, const Ft8SlotOptions& o, ftx_message_t& msg,
                    bool& viaOsd, int& apKind, double& startOut, double& f0Out, Ft8SlotStats& st, int pass);
    bool apDecode(const float* llr, const Ft8SlotOptions& o, ftx_message_t& msg, int& kind);
    bool a91ToMessage(const uint8_t* a91, ftx_message_t& msg) const;

    FtxMode mode_;
    bool  ft4_;                    // FT4's frame: FT4 and FT2
    int   rate_;
    int   nn_, nsps_;              // channel symbols, samples per symbol
    int   windowSamples_;
    monitor_t mon_;
    std::vector<ftx_candidate_t> cands_;
    std::vector<float> noise_;     // per waterfall column: mean power over the window (-1 = not yet measured)
    std::vector<float> pulse_;     // GFSK frequency pulse, 3 symbols long
    int pulseLo_ = 0, pulseHi_ = 0;        // where it is not ~0
    std::vector<float> dphi_;      // scratch: the reference's phase step per sample
    std::vector<float> refC_, refS_;       // scratch: the reference, cos and sin per sample (1.2 MB FT8)
    std::vector<float> blkR_, blkI_;       // scratch: audio × conj(reference), summed per 16-sample block
    std::vector<float> ampR_, ampI_;       // scratch: the amplitude at each block centre
    std::vector<float> zr_, zi_, toneR_, toneI_;   // scratch: the decimated baseband and its tone tables
    std::unique_ptr<FtxFine> fine_;
    FtxFine::Meas fineMeas_;
    float fineLlr_[FtxFine::kMaxSets][174];         // scratch: the second look's likelihood sets
    std::vector<float> rA_, rB_;                    // scratch: each call's AP agreement as first / second call
    int dbgHits_ = -1, dbgErr_ = -1, dbgSet_ = -1;                // diagnostics (FTX_CDBG)
    static constexpr int kFineBpStall = 5;
    static constexpr int kFineOsdMaxBpErr = 12;     // OSD only after BP came this close (unsatisfied checks)
    static constexpr int kApMaxBpErr = 19;          // a-priori decoding only for candidates BP took this close (real
                                                    // 40 m, two hours: every AP decode came from BP within 19)
    static constexpr int kApMaxCands = 3;           // hypotheses decoded per near miss, after screening
    static constexpr float kApMinZ = 2.0f;          // a hypothesis's screening z-score must reach this
    static constexpr int kApBpIters = 25;
    static constexpr double kApMaxFreeErrFrac = 0.30;   // hard disagreements allowed on the bits not assumed          // BP stops after this many iterations without a new best
    std::vector<double> decodedStart_;     // this slot's decodes' start samples, by result index (the near test)
    static constexpr int kFineMinSyncFt8 = 10;    // of 21 Costas symbols (noise: 21/8 ≈ 2.6)
    static constexpr int kFineMinSyncFt4 = 10;   // of 16 (noise: 4)        // the second look, made on first use (its window FFT: 1.5 MB FT8)
};

class Ft8Decoder {
public:
    Ft8Decoder(int sampleRate, bool ft4) : Ft8Decoder(sampleRate, ft4 ? FtxMode::FT4 : FtxMode::FT8) {}
    Ft8Decoder(int sampleRate, FtxMode mode);
    ~Ft8Decoder();
    FtxMode mode() const { return mode_; }

    /** Feed mono int16 audio at the construction sample rate. `captureUtc` is the corrected UTC (vibeUtcNow) at
     *  which the LAST sample of this block was captured — DecoderHost stamps it when the audio is handed over, so
     *  a decoder thread running behind does not move the slot. NaN = "now" (a caller with no timestamp). */
    void process(const int16_t* mono, int count, double captureUtc = NAN);
    /** ★ The same, float audio (±1.0) — Band mode's tuners make float, and a round trip through int16 would only cost. */
    void process(const float* mono, int count, double captureUtc = NAN);

    bool isFt4() const { return ft4; }

    /** ★ The callsign a spot may carry, from ft8_lib's de-call field: false for an empty one or an unresolved hash
     *  ("<...>"); a resolved hash ("<PJ4/K1ABC>") loses its brackets. Nothing with a '<' or '>' is ever spotted —
     *  the host's whitelist turned "<...>" into "" and the map got an EMPTY-callsign spot (audit 2026-10-04). */
    static bool spotCallsign(const char* deField, std::string& out);

    // call_to, call_de, grid (any may be empty), snr dB, audio offset Hz.
    std::function<void(const std::string& callTo, const std::string& callDe,
                       const std::string& grid, int snr, float audioHz)> onSpot;

    /** ★ The server's load, read before every extra pass: true = loaded, decode ONE pass (today's cost). Set
     *  before audio flows; empty = never loaded (a decoder-only sidecar, the bench). */
    std::function<bool()> loadProbe;
    /** Most passes the adaptive gate may grant (1 = the old decoder exactly). */
    std::atomic<int> maxPasses{3};

    /* ★★★ THE PASS GATE (2026-10-05). A second pass is granted only if, judged by the clock, it would still end
     *  inside kPassBudget of the time this slot's decode has before the NEXT slot is ready — and the server is not
     *  loaded. The next pass is costed as 1.5 × the last (a pass 2 searches a cleaner spectrogram, but it pays for
     *  the subtraction first). Pure, so the thresholds are tested. */
    static constexpr double kPassBudget = 0.40;
    static constexpr int    kOsdMaxErrors = 24;
    static bool passAllowed(double slotMs, double lagMs, double elapsedMs, double lastPassMs, bool loaded) {
        if (loaded) return false;
        const double left = slotMs - lagMs;              // until the next slot's window is complete
        return elapsedMs + 1.5 * lastPassMs < kPassBudget * left;
    }

private:
    /* ★★★ THE DECODE RUNS ON ITS OWN THREAD (2026-09-19). process() is called from the server's
     *  audio handler, and it used to call runDecode() INLINE when a slot filled: the whole candidate
     *  search and LDPC decode, every 15 s (FT8) and 7.5 s (FT4), on the thread that makes the audio.
     *  A fast machine hides it; a phone-class core does not — Stuart saw FT8 hang SDR++ Brown's
     *  audio on a Moto G35, and on a Pi 2 (VibeServer Lite) it would be a hitch every cycle.
     *  ★ The finished window is COPIED to the worker and the ring keeps filling at once. If a decode
     *    is still running when the next window completes (it had a whole slot's time), that slot is
     *    SKIPPED and counted — never waited for. */
    void runDecode();
    void advance(int count, double captureUtc);   // ★ the slot cutter, after a block is in the ring (both process())
    void workerLoop();

    FtxMode mode_;
    bool   ft4;                    // FT4's frame: FT4 and FT2
    int    rate;
    double slotPeriod;
    bool   ok = false;

    /* ★★★ SLOT TIMING FROM THE CAPTURE CLOCK (audit 2026-10-04). The window used to start when the DECODE
     *  thread's clock read slot + 0.8 s: the first 0.3 s of every signal was never seen (FT8 starts at +0.5),
     *  and when the decoder queue ran behind, its clock ran late and the window slid by the lag. Now every
     *  sample has a capture time and the window is cut from a ring by those times: slot + leadSec() for
     *  windowSamples(). */
    std::vector<float> ring_;
    int64_t total_ = 0;            // samples ever written
    int64_t floor_ = 0;            // first sample since the last re-anchor (older ones are not cut)
    double  utc0_ = 0;             // capture UTC of sample index 0 (lowest-latency estimate)
    bool    anchored_ = false;
    double  nextSlot_ = NAN;       // UTC of the next slot boundary to cut
    int     window_ = 0;

    Ft8SlotDecoder slot_;
    std::vector<float> work_;      // the window being decoded (the worker's, subtracted in place)
    double  workSlotEnd_ = 0;      // capture UTC at which that window's last sample arrived
    std::thread worker;
    std::mutex  wm;
    std::condition_variable wcv;
    bool pending = false, busy = false, stop = false;
    std::atomic<bool> abortDecode{false};
public:
    std::atomic<unsigned> slotsSkipped{0};
    std::atomic<unsigned> slotsDecoded{0};
    std::atomic<unsigned> reanchors{0};
    /** Passes the last decode ran (1-3) and how long it took — for the admin page and the tests. */
    std::atomic<int>    lastPasses{0};
    std::atomic<double> lastDecodeMs{0};
};

} // namespace vibe
