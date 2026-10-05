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
#include <vector>

// ft8_lib C core
extern "C" {
#include "common/monitor.h"
#include "ft8/decode.h"
}

namespace vibe {

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

/** One message decoded from a slot. */
struct Ft8Result {
    ftx_message_t msg;
    int   snr  = 0;      ///< dB in 2500 Hz, WSJT-X's definition (see snrDb in the .cpp)
    float hz   = 0;      ///< audio frequency of tone 0
    float dt   = 0;      ///< seconds after the slot's nominal start (+0.5 s FT8 / +0.5 s FT4), WSJT-X's DT
    int   pass = 1;      ///< which pass found it (2+ = only after the stronger ones were subtracted)
    bool  osd  = false;  ///< recovered by OSD after belief propagation failed
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
    /** Asked before pass 2 and 3 with the ms this decode has taken so far and the ms the last pass took.
     *  Empty = always yes (the bench). */
    std::function<bool(int nextPass, double elapsedMs, double lastPassMs)> mayRunPass;
    const std::atomic<bool>* abort = nullptr;
};
struct Ft8SlotStats {
    int    passes = 0;
    double passMs[3] = {0, 0, 0};
    int    passDecodes[3] = {0, 0, 0};
};

/** One slot, decoded synchronously: what the worker thread runs, and what the bench and the tests drive directly.
 *  Owns its spectrogram and scratch, so one instance must not decode two slots at once. */
class Ft8SlotDecoder {
public:
    Ft8SlotDecoder(int sampleRate, bool ft4);
    ~Ft8SlotDecoder();
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

    bool  ft4_;
    int   rate_;
    int   nn_, nsps_;              // channel symbols, samples per symbol
    int   windowSamples_;
    monitor_t mon_;
    std::vector<ftx_candidate_t> cands_;
    std::vector<float> noise_;     // per waterfall column: mean power over the window (-1 = not yet measured)
    std::vector<float> pulse_;     // GFSK frequency pulse, 3 symbols long
    int pulseLo_ = 0, pulseHi_ = 0;        // where it is not ~0
    std::vector<float> dphi_;      // scratch: the reference's phase step per sample
    std::vector<float> prodRe_, prodIm_;   // scratch: audio × conj(reference)
    std::vector<float> zr_, zi_, toneR_, toneI_;   // scratch: the decimated baseband and its tone tables
};

class Ft8Decoder {
public:
    Ft8Decoder(int sampleRate, bool ft4);
    ~Ft8Decoder();

    /** Feed mono int16 audio at the construction sample rate. `captureUtc` is the corrected UTC (vibeUtcNow) at
     *  which the LAST sample of this block was captured — DecoderHost stamps it when the audio is handed over, so
     *  a decoder thread running behind does not move the slot. NaN = "now" (a caller with no timestamp). */
    void process(const int16_t* mono, int count, double captureUtc = NAN);

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
    void workerLoop();

    bool   ft4;
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
