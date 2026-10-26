// VibeSDR V12 — WSPR-2 decoder, our own engine (2026-10-26).
//
// ★★★ WHAT IT IS. WSPR ("whisper", K1JT): a 50-bit message — a callsign, a 4-character Maidenhead locator and a power
//     in dBm — convolutionally coded (K = 32, r = ½, 162 channel bits), interleaved, and sent as 162 symbols of 4-FSK at
//     12000/8192 = 1.4648 baud with the tones 1.4648 Hz apart (about 6 Hz wide). Each symbol carries one known SYNC bit
//     (the published 162-bit pseudo-random vector) and one data bit: tone = sync + 2·data. A transmission starts one
//     second after an even UTC minute and lasts 110.6 s. Written from the published protocol description (K1JT's
//     WSPR notes and G4JNT's "The WSPR Coding Process"); the extended messages — type 2 (a compound callsign, no
//     locator) and type 3 (a 15-bit hash of the callsign with a 6-character locator) — from the same notes, and the
//     hash is Bob Jenkins' public-domain lookup3 (initial value 146, 15 bits), as the protocol specifies.
//
// ★★ HOW IT DECODES (one two-minute slot, the whole 200 Hz sub-band at once):
//   1. the slot's audio (any rate that is a multiple of 1500 Hz — 12, 24 or 48 kHz) is mixed down from 1500 Hz audio
//      and decimated to 375 Hz complex in two FIR stages (WsprDecimator) — 256 samples a symbol;
//   2. a spectrogram (one symbol's samples, zero-padded to 512 → half-tone bins, every half symbol) is turned into ONE
//      number per frame and bin — the sync-tone pair's share of the four tones' power — so the search over frequency ×
//      start × drift is nothing but signed sums of rows (SIMD, all bins at once);
//   3. each candidate is mixed to 0 Hz, decimated again to 46.875 Hz (32 samples a symbol) and refined (start to 1/32
//      of a symbol, frequency to 0.025 Hz, drift to 0.25 Hz) on the four tones' exact DFT bins;
//   4. the data bit of each symbol becomes a calibrated log-likelihood (non-coherent FSK: ln I0 of the two candidate
//      tones' amplitudes, signal and noise measured from the slot itself), de-interleaved, and the K = 32 code is
//      decoded sequentially (Fano), with a cycle budget per bit;
//   5. a decode is checked (callsign structure, locator, legal power; a type 3 only with a hash we have heard in
//      full), its SNR measured in 2500 Hz, and — if the CPU gate allows a second pass — the signal is rebuilt and
//      SUBTRACTED, and the residual searched again for what it hid.
// ★ No allocation in the hot loops. RAM per live decoder ≈ 2.5 MB: the 375 Hz ring (2 slots, 750 KB), the slot being
//   decoded (360 KB), the search maps (2 × 490 KB) and the per-candidate scratch (400 KB) — not 2 min of raw audio.
#pragma once
#include <atomic>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibe {
namespace wspr {

constexpr int    kSymbols     = 162;
constexpr int    kMsgBits     = 50;
constexpr int    kCodedBits   = 81;                 // 50 message + 31 zero tail bits into the encoder
constexpr double kBaud        = 12000.0 / 8192.0;   // 1.46484375 Bd — also the tone spacing in Hz
constexpr double kAudioCentre = 1500.0;             // the sub-band's centre in USB audio (dial + 1500 Hz)
constexpr double kSlotSec     = 120.0;
constexpr double kStartSec    = 1.0;                // a transmission starts this long after the even minute
constexpr int    kRate375     = 375;                // the decimated complex baseband
constexpr int    kSps375      = 256;                // samples per symbol there
constexpr double kTxSec       = kSymbols * 8192.0 / 12000.0;   // 110.592 s
/** ★ A slot's window starts this long BEFORE the even minute and runs 120 s: a sender whose clock is 3 s fast (DT −2)
 *  or 6 s slow (DT +5) still lies inside it — both are on the air (Stuart's UberSDR, 2026-10-26: DT −4 … +6). */
constexpr double kLeadSec     = 2.0;

/** The published sync vector (one bit per channel symbol). */
extern const uint8_t kSyncVector[kSymbols];

/** A WSPR message. type 1: call + 4-char grid + dBm. type 2: compound call ("PJ4/K1ABC", "K1ABC/P") + dBm, no grid.
 *  type 3: "<K1ABC>" as a hash + 6-char grid + dBm. */
struct Message {
    int type = 1;
    std::string call;    ///< as sent (type 3: the call the hash resolved to, or "" when unknown)
    std::string grid;    ///< "" (type 2), 4 chars (type 1) or 6 chars (type 3)
    int dbm = 0;
    uint32_t hash = 0;   ///< type 3: the 15-bit hash carried
    std::string text() const;   ///< "K1ABC FN42 37", "PJ4/K1ABC 37", "<K1ABC> FN42AB 37"
};

/** Bob Jenkins' lookup3 over the call's characters, initial value 146, low 15 bits — the protocol's callsign hash. */
uint32_t callHash(const std::string& call);

/** Pack a message into the 50 source bits (7 bytes, MSB first; the last 6 bits of byte 6 are zero).
 *  False when the call/grid/power cannot be sent as that type. */
bool pack(const Message& m, uint8_t bits[7]);
/** Unpack 50 source bits. Type 3's call is looked up in `table` (empty call when unknown). False when the bits are
 *  not a well-formed message (the first line of the false-decode defence). */
class CallHashTable;
bool unpack(const uint8_t bits[7], Message& out, CallHashTable* table);
/** The 162 channel symbols (0..3) for a message: pack → convolutional code → interleave → merge with sync. */
bool encode(const Message& m, uint8_t tones[kSymbols]);
/** The 162 channel symbols for 50 already-packed bits. */
void encodeBits(const uint8_t bits[7], uint8_t tones[kSymbols]);

/** Sequential (Fano) decoding of the K = 32, r = ½ code. `llr` = 162 log-likelihood ratios in CHANNEL order (after
 *  de-interleaving, i.e. encoder output order; > 0 favours a 1). Returns true and the 50 bits on success within
 *  `maxCycles`; `cycles` reports the work done. Exposed for the tests and the bench. */
bool fanoDecode(const float llr[kSymbols], long maxCycles, float delta, uint8_t bits[7], long* cycles,
                const std::atomic<bool>* abort = nullptr);
/** Channel position of encoder output bit p (the bit-reversal interleaver). */
int interleavePos(int p);

/* ★★ THE HASHED-CALLSIGN TABLE. A type-3 message carries only a 15-bit hash of the call; it can be printed only if
 *  the same call has been heard IN FULL (type 1 or 2) — the station alternates the two. Least-recently-used, and a
 *  hash that matches two DIFFERENT calls is unknown rather than a guess (the FT8 table's rule). */
class CallHashTable {
public:
    static constexpr int kCapacity = 1024;
    void save(const std::string& call);
    bool lookup(uint32_t hash15, std::string& out);
    void clear();
    int  size();
private:
    struct Entry { char call[14]; uint32_t h; uint64_t stamp; };
    std::mutex mtx_;
    Entry e_[kCapacity] = {};
    uint64_t clock_ = 0;
};
/** The one table every WSPR decoder on this server shares (a call heard on 40 m resolves its hash on 20 m). */
CallHashTable& callHashes();

} // namespace wspr

/** One decoded WSPR transmission. */
struct WsprSpot {
    double slotUtc = 0;      ///< UTC (s) of the even minute the slot began
    double freqHz  = 0;      ///< RF frequency of the signal's centre (dial + audio)
    double audioHz = 0;      ///< audio frequency of the centre (≈ 1400…1600 Hz)
    int    snr     = 0;      ///< dB in 2500 Hz (the WSPR convention)
    float  dt      = 0;      ///< s after the nominal start (even minute + 1 s)
    float  drift   = 0;      ///< Hz the signal moved over the transmission (≈ per two minutes), the WSPR convention
    std::string call, grid;  ///< grid: 4 or 6 chars, "" for a type 2
    int    dbm     = 0;
    int    type    = 1;
    int    pass    = 1;      ///< 2 = found only after the stronger signals were subtracted
    std::string message;     ///< "K1ABC FN42 37"
    std::string mode = "WSPR";
    float  sync    = 0;      ///< the fine sync score (0…1), for the bench
};

struct WsprSlotOptions {
    int  maxPasses     = 3;      ///< 1 = no subtraction (real 40 m: 1 pass 38 decodes, 2 → 51, 3 → 53)
    int  maxCandidates = 60;     ///< per pass
    long fanoCyclesPerBit = 5000;   ///< real 40 m + 20 m: 5000 decodes all 10000 does, at 55 % of the CPU
    float fanoDelta = 2.0f;      ///< the Fano threshold step (bits) — tuned on test-wspr's synthetic slots
    float llrScale  = 0.9f;      ///< the soft bits' confidence (1 = the measured channel exactly)
    /** Asked before pass 2 and 3 with the ms the decode has taken so far and the ms the last pass took. Empty = yes. */
    std::function<bool(int nextPass, double elapsedMs, double lastPassMs)> mayRunPass;
    const std::atomic<bool>* abort = nullptr;
};
struct WsprSlotStats {
    int    passes = 0;
    double passMs[3] = {0, 0, 0};
    int    candidates[3] = {0, 0, 0};
    int    decodes[3] = {0, 0, 0};
    long   fanoCycles = 0;
    int    fanoFails = 0;
    double searchMs = 0, refineMs = 0, fanoMs = 0, subtractMs = 0, decimateMs = 0;
};

/* ★★ AUDIO → 375 Hz COMPLEX, STREAMING. The live decoder keeps only this (90 000 complex samples for two slots,
 *  720 KB) rather than two minutes of 48 kHz audio (23 MB) — the Pi 2 has 1 GB and may run a VFO per band. Stage 1
 *  is a complex-coefficient FIR on the real audio (it mixes 1500 Hz to 0 and low-passes in one), decimating to
 *  1500 Hz; stage 2 a real FIR on the complex result, decimating by 4. Both centred (odd lengths), so output sample m
 *  is the signal at input sample m·rate/375 exactly — the slot decoder and the live one see the same timing. */
class WsprDecimator {
public:
    explicit WsprDecimator(int sampleRate);
    bool ok() const { return d1_ > 0; }
    /** Push audio (float, any scale); appends 375 Hz outputs. */
    void push(const float* in, int n, std::vector<std::complex<float>>& out);
    /** Push zeros to flush the centred filters' look-ahead (end of a recording). */
    void flush(std::vector<std::complex<float>>& out);
    void reset();
    int  rate() const { return rate_; }
private:
    void stage2(std::vector<std::complex<float>>& out);
    int rate_, d1_ = 0;
    std::vector<float> h1r_, h1i_, h2_;   // taps
    int c1_ = 0, c2_ = 0;                 // half lengths
    std::vector<float> in_;               // stage-1 input history (real)
    size_t inPos_ = 0;                    // next stage-1 output's centre index in in_
    std::vector<float> s2r_, s2i_;        // stage-2 input history (complex, split)
    size_t s2Pos_ = 0;
};

/** One slot, decoded synchronously: what the worker runs, and what the bench and tests drive directly. */
class WsprSlotDecoder {
public:
    WsprSlotDecoder();
    ~WsprSlotDecoder();
    WsprSlotDecoder(const WsprSlotDecoder&) = delete;
    WsprSlotDecoder& operator=(const WsprSlotDecoder&) = delete;

    /** Samples of 375 Hz baseband one slot uses (120 s); fewer are zero-padded. */
    static constexpr int kSlotSamples = 45000;
    /** Decode one slot of AUDIO starting kLeadSec before the even minute (`n` samples at `sampleRate`, ideally 120 s). */
    void decodeAudio(const float* audio, int n, int sampleRate, double dialHz, double slotUtc,
                     const WsprSlotOptions& o, std::vector<WsprSpot>& out, WsprSlotStats& st);
    /** Decode one slot of 375 Hz baseband starting kLeadSec before the even minute. ★ `x` is MODIFIED (pass 2 subtracts). */
    void decode(std::vector<std::complex<float>>& x, double dialHz, double slotUtc,
                const WsprSlotOptions& o, std::vector<WsprSpot>& out, WsprSlotStats& st);

    struct Cand { float score; int bin, lag; float drift; };
    struct Fit  { double f0; int t0; float drift; float metric; };   // f0: tone-0 Hz rel. 1500; t0: z index (46.875 Hz)

private:
    int  search(int pass, std::vector<Cand>& cands, int maxCands);
    bool refineAndDecode(const Cand& c, const WsprSlotOptions& o, double dialHz, double slotUtc, int pass,
                         std::vector<WsprSpot>& out, WsprSlotStats& st, std::vector<std::pair<Fit, std::vector<uint8_t>>>& decoded);
    void mixDecimate(double fmix);
    float evalFit(int t0, double df, float drift, float* P /* 162×4 or null */, float* C /* 162×4 complex or null */);
    /** Follow a decoded signal's complex amplitude through the slot; with `apply`, subtract it. Returns its mean power
     *  (per 375 Hz complex sample, the moving average's own noise taken off). */
    double track(const Fit& f, const uint8_t* tones, bool apply);
    double bandNoise();

    std::vector<std::complex<float>>* x_ = nullptr;
    int nFrames_ = 0;
    std::vector<float> q_;          // nFrames × kQW sync-share map
    float* fftIn_ = nullptr; float* fftOut_ = nullptr; float* fftWork_ = nullptr;   // pffft, 16-byte aligned
    void* fft_ = nullptr;
    std::vector<float> zr_, zi_;    // per-candidate 46.875 Hz baseband (split)
    std::vector<float> yr_, yi_;    // scratch: the mixed 375 Hz signal
    std::vector<float> h3_;         // 375 → 46.875 decimation taps
    int c3_ = 0;
    double zMix_ = 0;               // the mix frequency of the current z (Hz rel. 1500)
    std::vector<float> p_;          // pass 1's power spectrogram (nFrames × 341, 477 KB), for the noise floor
    double noise375_ = 0;           // the band's noise per 375 Hz complex sample (0 = not yet measured this slot)
};

/* ★★★ THE LIVE DECODER. process() is called with each block of demodulated USB audio and its capture UTC; it only
 *  decimates (≈ 15 multiply-adds per input sample at 12 kHz) and stores the 375 Hz result. When a slot's two minutes
 *  are complete they are COPIED to the worker thread, which decodes them; if the previous slot is somehow still
 *  decoding, the new one is SKIPPED and counted (slotsSkipped — the "TOO HEAVY" line), never waited for. The slot
 *  is cut by the capture clock (lowest-latency anchor, as FT8 does), so a decoder running behind never moves it. */
class WsprDecoder {
public:
    explicit WsprDecoder(int sampleRate);
    ~WsprDecoder();
    bool ok() const { return dec_.ok(); }

    /** Mono int16 audio at the construction rate; `captureUtc` = the corrected UTC the LAST sample was captured
     *  (NaN = now). */
    void process(const int16_t* mono, int count, double captureUtc = NAN);
    /** The same for float audio at ±1.0 full scale — digi::Engine::process's form (decoders/digi_mode.h, v12-spots). */
    void process(const float* mono, int count, double captureUtc = NAN);
    /** The dial (USB carrier) frequency, for the spots' RF frequency. A retune mid-slot spoils that slot only. */
    std::atomic<double> dialHz{0};

    std::function<void(const WsprSpot&)> onSpot;
    /** ★ The server's load, read before pass 2: true = loaded, decode ONE pass. Empty = never loaded. */
    std::function<bool()> loadProbe;
    std::atomic<int>  maxPasses{3};
    std::atomic<long> fanoCyclesPerBit{5000};

    /** Pass 2 (and 3) runs only if it is costed (1.5 × the last pass) to end inside 25 % of the slot and the server is
     *  not loaded. */
    static constexpr double kPassBudget = 0.25;
    static bool passAllowed(double slotMs, double elapsedMs, double lastPassMs, bool loaded) {
        if (loaded) return false;
        return elapsedMs + 1.5 * lastPassMs < kPassBudget * slotMs;
    }

    std::atomic<unsigned> slotsSkipped{0};
    std::atomic<unsigned> slotsDecoded{0};
    std::atomic<unsigned> reanchors{0};
    std::atomic<double>   lastDecodeMs{0};
    std::atomic<int>      lastPasses{0};

private:
    void workerLoop();
    void runDecode();

    int rate_;
    WsprDecimator dec_;
    std::vector<float> fbuf_;
    std::vector<std::complex<float>> tmp_;
    std::vector<std::complex<float>> ring_;   // 375 Hz, 2 slots + margin
    int64_t total_ = 0;      // 375 Hz samples ever written
    int64_t floor_ = 0;
    int64_t inTotal_ = 0;    // input samples ever pushed
    double  utc0_ = 0;       // capture UTC of input sample 0
    bool    anchored_ = false;
    double  nextSlot_ = NAN;

    WsprSlotDecoder slot_;
    std::vector<std::complex<float>> work_;
    double workSlotUtc_ = 0, workDial_ = 0;
    std::thread worker_;
    std::mutex wm_;
    std::condition_variable wcv_;
    bool pending_ = false, busy_ = false, stop_ = false;
    std::atomic<bool> abort_{false};
};

} // namespace vibe
