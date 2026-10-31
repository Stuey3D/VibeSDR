// decoders/digimodes/q65_decoder.h — Q65, our own receiver (V12, 2026-10-31). The frame and the code: q65_code.h.
//
// ★★★ WHAT Q65 IS. A weak-signal mode for EME, ionospheric and meteor scatter (VHF and up; occasionally HF): the 77-bit
//     FT8-family message, a CRC-12, a Q-ary repeat-accumulate code over GF(64), 63 data symbols as 64-FSK (tones 1..64)
//     and 22 sync symbols on tone 0 — 85 symbols of 65-FSK. T/R periods 15, 30, 60, 120 and 300 s (1800, 3600, 7200,
//     16000 and 41472 samples a symbol at 12 kHz); submodes A–E space the tones 1, 2, 4, 8, 16 × the baud rate for
//     Doppler spread. A transmission starts 0.5 s into a 15/30 s period, 1.0 s into a longer one.
//
// ★★ HOW IT DECODES (one period's window, every signal in the passband at once):
//   1. SEARCH — a spectrogram (one symbol, zero-padded → half-baud bins, every quarter symbol), each bin's noise taken
//      as its median over the window; the sync score of a bin at a start is the mean noise-normalised power at the 22
//      sync positions MINUS the mean at the 63 data positions (data never uses tone 0, so a carrier or another mode
//      scores ≈ 0). SIMD over bins. The best start per bin; local maxima above a threshold are candidates.
//   2. BASEBAND — the window's ONE big FFT (per slot) is sliced around the candidate and transformed back: a complex
//      baseband at exactly 72 × 2^submode samples a symbol (so every tone is a bin of a symbol-long DFT).
//   3. REFINE — start to 1/72 of a symbol and frequency to 1/16 of a tone by the sync symbols' tone-0 power.
//   4. SYMBOLS — a DFT per symbol; each data symbol's 64 tone energies (summed over a Doppler width), noise-normalised.
//   5. DECODE — the energies become probabilities (the fading-channel metric, Es/N0 measured from the sync symbols),
//      belief propagation over GF(64) (q65_code.h), the CRC-12; then a-priori "CQ ? ?" if blind decoding fails.
//   6. GUARD — a decode must also (a) unpack, (b) be a message people send (the FT8 family's plausibility rules), and
//      (c) stand out: the mean normalised energy of its 63 tones above a threshold set from long noise runs.
// ★ Thread: decode() runs on the live decoder's own worker, never the audio thread. No allocation per candidate.
#pragma once
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "q65_code.h"

namespace vibe {
namespace q65 {

constexpr int kRate = 12000;

/** One T/R period: its length, samples a symbol, the nominal start into the period, and the big FFT's multiple. */
struct Period { int sec; int nsps; double lead; int c; };
/** 15, 30, 60, 120, 300 — or null. */
const Period* periodFor(int sec);
/** The window a slot is decoded from: from 1 s before the period starts to 1 s after it ends. */
constexpr double kWinLeadSec = 1.0, kWinTailSec = 1.0;
inline int windowSamples(const Period& p) { return (int)std::lround((p.sec + kWinLeadSec + kWinTailSec) * kRate); }

struct Result {
    int    periodSec = 60;
    int    submode = 0;          ///< 0..4 = A..E
    float  f0 = 0;               ///< audio Hz of tone 0 (the sync tone) — what WSJT-X's frequency column shows
    float  dt = 0;               ///< s from the nominal start
    int    snr = 0;              ///< dB in 2500 Hz
    float  esn0 = 0;             ///< measured Es/N0 (linear), for the bench
    float  score = 0;            ///< mean normalised energy at the decoded tones (the guard's number)
    float  sync = 0;             ///< the search's sync score
    int    iters = 0;
    int    hard = 0;             ///< data symbols whose decoded tone is also the strongest of the 64 (the guard's second number)
    int    ap = 0;               ///< 0 blind, 1 "CQ ? ?"
    double slotUtc = NAN;
    uint8_t payload[10] = {};
    std::string text;            ///< as unpacked
    std::string call, grid;      ///< the SENDER's call (no hashes) and own grid, or ""
    std::string mode() const;    ///< "Q65-60A"
};

struct SlotOptions {
    float loHz = 300, hiHz = 3500;   ///< tone 0 is searched from loHz; every tone of the widest submode tried stays ≤ hiHz
    unsigned submodes = 0x0F;        ///< bit m = try submode m (A=bit 0 … E=bit 4)
    int   maxCands = 16;
    int   maxIter = 0;                ///< BP iterations; 0 = by period (50 for 15/30 s, 100 for 60 s and longer)
    float syncMin = 0.9f;            ///< candidate threshold on the sync score (noise σ ≈ 0.25)
    bool  apCq = true;               ///< try "CQ ? ?" when blind decoding fails
    bool  loaded = false;            ///< the server is busy: no AP pass, fewer candidates
    const std::atomic<bool>* abort = nullptr;
};

struct SlotStats {
    int candidates = 0, attempts = 0, decodes = 0, rejected = 0;
    double searchMs = 0, fftMs = 0, candMs = 0, totalMs = 0;
};

/** Decodes one period's window. Owns its FFT setups and scratch (a few MB for a 60 s period). */
class SlotDecoder {
public:
    explicit SlotDecoder(int periodSec);
    ~SlotDecoder();
    SlotDecoder(const SlotDecoder&) = delete;
    SlotDecoder& operator=(const SlotDecoder&) = delete;
    bool ok() const { return p_ != nullptr; }
    const Period& period() const { return *p_; }
    /** `win` = windowSamples() samples, sample 0 = 1 s before the period starts (fewer: zero-padded). */
    int decode(const float* win, int n, const SlotOptions& o, std::vector<Result>& out, SlotStats* st = nullptr);

    // ★ Guard thresholds: the mean noise-normalised energy at the decoded tones over the 63 data symbols (noise ≈ 1).
    //   Measured (docs/v12/DECODERS-WAVE2.md "Q65"): with the guard off, 15A and 30A at their thresholds (2,400 slots)
    //   gave 21 blind CRC-passing wrong decodes, the highest 2.59 (one; the next 1.76), against true blind decodes no
    //   lower than 2.89; 31 a-priori wrong decodes at most 1.65 against true ones from 2.37 — but 1,000 windows of
    //   30 s noise then gave one a-priori false decode at 2.32 ("CQ CB6OAP/R R LE67"), so an a-priori decode needs
    //   2.6 AND must be a CQ with a real grid (what a CQ nearly always carries). A message that is not a standard one
    //   people send (free text, telemetry, odd calls) needs 0.5 more.
    static constexpr float kScoreMin   = 2.75f;  ///< blind decodes
    static constexpr float kScoreMinAp = 2.6f;   ///< a-priori ("CQ ? ?") decodes
private:
    struct Cand { int bin; int frame; float score; };
    void spectrogram(const float* win, int n);
    void search(const SlotOptions& o, std::vector<Cand>& cands);
    void bigFft(const float* win, int n);
    struct Ext { int m = 0; long kc = 0; double ft0 = 0, df = 0; int n0 = 0; float rho = 0; double z = 0; std::vector<float> symPow; };
    bool extract(const Cand& c, int m, const SlotOptions& o, Ext& x);
    bool decodeExt(const Cand& c, const Ext& x, const SlotOptions& o, std::vector<Result>& out, SlotStats& st);
    Ext ext_[5];

    const Period* p_ = nullptr;
    double baud_ = 0;
    // spectrogram
    int pad_ = 2, nfftS_ = 0, hop_ = 0, nFrames_ = 0, bLo_ = 0, bHi_ = 0, nB_ = 0;
    void* setupS_ = nullptr;
    float *sIn_ = nullptr, *sOut_ = nullptr, *sWork_ = nullptr;
    std::vector<float> P_;      // nFrames × nB, noise-normalised
    // big FFT
    int nBig_ = 0;
    void* setupBig_ = nullptr;
    float *bigBuf_ = nullptr, *bigWork_ = nullptr;
    bool bigDone_ = false;
    // per-submode baseband
    struct Sub { int M = 0, Ns = 0, nfft = 0; void* setupM = nullptr; void* setupN = nullptr; float *z = nullptr, *zw = nullptr, *y = nullptr, *yw = nullptr; };
    Sub sub_[5];
    std::vector<float> E_;      // 63 × 64 energies
    std::vector<float> pw_;     // per symbol power spectrum scratch
    std::vector<float> intr_;   // 63 × 64 probabilities
    BpScratch* bp_ = nullptr;
    uint8_t cqMask_[kMsg] = {}, cqVal_[kMsg] = {};
    std::vector<uint8_t> seen_; // payloads decoded this slot (10 bytes each)
};

/* ★★ THE LIVE DECODER: real USB audio at 12 kHz with its capture UTC in; it keeps a ring long enough for its longest
 *  period, and at the end of each period's window copies the window to its worker thread, which decodes it. A window
 *  that arrives while the same period's previous one is still queued is SKIPPED and counted, never waited for. */
class LiveDecoder {
public:
    /** `periods`: the T/R periods to decode (15, 30, 60, 120, 300 — any of them). */
    explicit LiveDecoder(std::vector<int> periods);
    ~LiveDecoder();
    void process(const float* audio, int n, double captureUtc);
    std::function<void(const Result&)> onResult;
    std::function<bool()> loadProbe;
    SlotOptions opts;                      ///< set before audio flows
    std::atomic<uint64_t> slotsSkipped{0}, slotsDecoded{0};
    std::atomic<double> lastDecodeMs{0};
    /** Wait (tests) until no window is queued or decoding. */
    void drain();
private:
    struct Job { int pi; double slotUtc; std::vector<float> win; };
    void worker();
    std::vector<int> periods_;
    std::vector<std::unique_ptr<SlotDecoder>> dec_;
    std::vector<double> next_;             // per period: UTC of the next period start to decode
    std::vector<float> ring_;
    int64_t total_ = 0;                    // samples ever written
    double utc0_ = 0; bool anchored_ = false;
    std::thread th_;
    std::mutex m_; std::condition_variable cv_, idle_;
    std::deque<Job> q_;
    std::vector<bool> queued_;
    bool busy_ = false, stop_ = false;
    std::atomic<bool> abort_{false};
};

} // namespace q65
} // namespace vibe
