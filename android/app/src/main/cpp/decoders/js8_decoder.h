// VibeSDR V12 — JS8 decoder: Normal, Fast, Turbo and Slow (2026-10-26).
//
// ★★★ WHAT JS8 IS, ON THE AIR. FT8's first frame (2017), kept when FT8 moved on: 79 channel symbols of 8-FSK —
//  a 7-symbol Costas array at symbols 0, 36 and 72, 29 parity symbols between the first two, 29 information symbols
//  between the last two — LDPC(174,87) + CRC-12 (js8_ldpc.h), three bits a symbol MSB first, no Gray map. The
//  waveform is plain continuous-phase FSK (no Gaussian shaping), tone spacing = the symbol rate. Four submodes
//  differ only in the symbol length, the T/R period, where in the period a transmission starts, and the Costas arrays:
//      submode  letter  samples/symbol @12 kHz  symbol   tones    period  starts  costas
//      Normal   A       1920                    160 ms   6.25 Hz  15 s    0.5 s   FT8's (4,2,5,6,1,3,0) ×3
//      Fast     B       1200                    100 ms   10 Hz    10 s    0.2 s   three different arrays (below)
//      Turbo    C        600                     50 ms   20 Hz     6 s    0.1 s   ″
//      Slow     E       3840                    320 ms   3.125 Hz 30 s    0.5 s   ″
//  The three arrays of Fast/Turbo/Slow: (0,6,2,3,5,4,1) · (1,5,0,2,3,6,4) · (2,5,0,6,4,1,3). Every period is a
//  whole number of seconds from 00:00 UTC, so the four submodes' slots all line up on the minute.
//  ★ Sources: JS8Call's own source (GPL-3) was READ to learn these facts; nothing of it is copied. The numbers are
//    typed in from their meaning and tested (test-js8: synthetic slots of every submode; real JS8 off the air).
//
// ★★ HOW IT DECODES (the FT8 core's way, docs/v12/JS8-FT2.md): ft8_lib's STFT waterfall (PFFFT, NEON/SSE) at the
//  submode's symbol length, two time and two frequency steps a symbol; a sync search over the three Costas arrays
//  sixteen frequency offsets per NEON/SSE2 vector; per-bit log-likelihoods from the eight tone levels; belief
//  propagation, OSD-1 on spare CPU; the CRC-12; the frame unpacked (js8_message.h). On spare CPU a second pass
//  subtracts every decoded signal and searches again.
#pragma once
#include "js8_message.h"
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

extern "C" {
#include "common/monitor.h"
}

namespace vibe {

/** JS8's own submode numbers (they are bit flags in JS8Call). */
enum class Js8Submode : int { Normal = 0, Fast = 1, Turbo = 2, Slow = 4 };

struct Js8SubmodeInfo {
    Js8Submode id;
    const char* name;            ///< "NORMAL" …
    char  letter;                ///< 'A' 'B' 'C' 'E' — JS8Call's log letters
    int   nsps12k;               ///< samples per symbol at 12 kHz
    float period;                ///< T/R period, s
    float startDelay;            ///< where a transmission starts in its period, s
    bool  modifiedCostas;        ///< false = FT8's array three times
};
const Js8SubmodeInfo& js8Info(Js8Submode s);
/** The four, in the order a live decoder runs them (fastest period first). */
constexpr Js8Submode kJs8Submodes[4] = { Js8Submode::Turbo, Js8Submode::Fast, Js8Submode::Normal, Js8Submode::Slow };
/** Bit of a submode in a mask (Normal 1, Fast 2, Turbo 4, Slow 8). */
constexpr unsigned js8Bit(Js8Submode s) {
    return s == Js8Submode::Normal ? 1u : s == Js8Submode::Fast ? 2u : s == Js8Submode::Turbo ? 4u : 8u;
}
constexpr unsigned kJs8AllSubmodes = 15u;

/** The 79 tones of a frame: `info87` = 72 message bits + 3 flags + the CRC-12 (js8::crc12), one bit per byte. */
void js8Tones(const uint8_t* info87, Js8Submode s, uint8_t* tones79);
/** The 87 information bits of a 72-bit message and its flags (fills in the CRC). */
void js8Info87(const uint8_t* bits72, int flags, uint8_t* info87);

struct Js8Result {
    js8::Frame frame;
    uint8_t info[87];        ///< as decoded (72 message, 3 flags, CRC)
    Js8Submode submode = Js8Submode::Normal;
    int   snr = 0;           ///< dB in 2500 Hz, WSJT-X's convention (as the FT8 decoder's)
    float hz = 0;            ///< audio frequency of tone 0
    float dt = 0;            ///< seconds from the submode's nominal start
    int   pass = 1;
    int   hardErrors = 0;
    bool  osd = false;
};

struct Js8SlotOptions {
    int maxPasses = 1;
    int ldpcIters = 30;
    int maxCandidates = 120;
    int minScore = 8;
    int osdMaxErrors = 0;        ///< 0 = no OSD
    int osdMinScore = 12;
    std::function<bool(int nextPass, double elapsedMs, double lastPassMs)> mayRunPass;
    const std::atomic<bool>* abort = nullptr;
};
struct Js8SlotStats {
    int passes = 0;
    double passMs[2] = {0, 0};
    int passDecodes[2] = {0, 0};
    int candidates[2] = {0, 0};
    int osdTried[2] = {0, 0};
    double waterfallMs = 0, searchMs = 0, ldpcMs = 0, osdMs = 0, subtractMs = 0;
};

/** One submode, one slot, decoded synchronously (what the live decoder's worker, the bench and the tests run). */
class Js8SlotDecoder {
public:
    Js8SlotDecoder(int sampleRate, Js8Submode s);
    ~Js8SlotDecoder();
    Js8SlotDecoder(const Js8SlotDecoder&) = delete;
    Js8SlotDecoder& operator=(const Js8SlotDecoder&) = delete;

    Js8Submode submode() const { return sub_; }
    /* ★★ SNR ON THE JS8 NETWORK'S SCALE (2026-10-26). The estimator is the FT8 decoder's (true SNR in 2500 Hz on the
     *  synthetic band, test-js8), but JS8 stations' own reports — what an operator compares a spot with, and what
     *  PSKReporter shows — read 11 dB lower: on the 40 m hour (~/VibeSDR-test-audio/js8/) ours minus STUEY3D's were
     *  +10.5 … +12 for the six stations both heard. So a spot's SNR is the true figure + this. */
    static constexpr int kSnrScaleDb = -11;
    /** Samples a slot's window holds, from the slot boundary (one period). */
    int windowSamples() const { return windowSamples_; }
    int samplesPerSymbol() const { return nsps_; }
    /** Decode one window (from the slot boundary). ★ The buffer is MODIFIED by pass 2's subtraction. */
    void decode(float* audio, int n, const Js8SlotOptions& o, std::vector<Js8Result>& out, Js8SlotStats& st);

    struct Cand { int16_t score, t, f; uint8_t ts, fs; };
private:
    void buildWaterfall(const float* a, int n);
    int  search(int maxCands, int minScore);
    void likelihoods(const Cand& c, float* llr);
    int  snrDb(const Cand& c, const uint8_t* tones);
    void subtract(float* a, int n, const uint8_t* tones, double start, double f0);

    Js8Submode sub_;
    int rate_, nsps_, windowSamples_;
    float T_;                       // symbol period, s
    int tMin_, tMax_;               // search range, blocks
    uint8_t costas_[3][7];
    monitor_t mon_;
    std::vector<Cand> cands_;
    std::vector<float> colMean_;    // lazily: mean power per waterfall column (-1 unknown)
    float colMean(size_t c);
};

/** One decoded JS8 transmission, for the host. */
struct Js8Spot {
    double slotUtc = 0;          ///< UTC of the slot boundary
    Js8Submode submode = Js8Submode::Normal;
    js8::Frame frame;            ///< from / to / grid / text / kind
    int   snr = 0;
    float audioHz = 0;           ///< tone 0 above the dial (USB)
    float dt = 0;
    /** True when this is a spot: a real sender's callsign was decoded (heartbeat, CQ, compound, directed). */
    bool isSpot() const { return js8::isSpotCall(frame.from); }
};

/* ★★★ THE LIVE DECODER. Fed audio on the host's decode thread (process returns at once); every enabled submode's
 *  window is cut from ONE ring by capture time (slot boundary .. + period), copied, and queued for ONE worker
 *  thread. A submode whose previous window is still queued or decoding is SKIPPED AND COUNTED for that slot, never
 *  waited for. RAM: the ring (int16, the longest window + 3 s: 33 s = 0.8 MB at 12 kHz) and one float window per
 *  enabled submode (Normal 0.72 MB, Fast 0.48, Turbo 0.29, Slow 1.44) + waterfalls (~1 MB all four). */
class Js8Decoder {
public:
    Js8Decoder(int sampleRate, unsigned submodeMask = kJs8AllSubmodes);
    ~Js8Decoder();
    Js8Decoder(const Js8Decoder&) = delete;
    Js8Decoder& operator=(const Js8Decoder&) = delete;

    /** Mono audio at the construction rate; `captureUtc` = the corrected UTC of the LAST sample (NaN = now). */
    void process(const int16_t* mono, int count, double captureUtc = NAN);
    void process(const float* mono, int count, double captureUtc = NAN);   ///< ±1.0 full scale

    std::function<void(const Js8Spot&)> onDecode;   ///< every decoded frame (data frames too); on the worker thread
    std::function<bool()> loadProbe;                ///< true = loaded: one pass, no OSD
    std::atomic<int> maxPasses{2};

    std::atomic<unsigned> slotsSkipped{0};
    std::atomic<unsigned> slotsDecoded{0};
    std::atomic<unsigned> reanchors{0};
    std::atomic<double>   lastDecodeMs{0};

private:
    struct Lane {
        Js8Submode sub;
        std::unique_ptr<Js8SlotDecoder> dec;
        std::vector<float> work;
        double nextSlot = NAN;
        bool queued = false;        // a window waits or is being decoded
        double slotUtc = 0, readyUtc = 0;
    };
    template <typename F> void ingest(int count, double captureUtc, F sample);
    void cutWindows(double captureUtc);
    void workerLoop();
    void runLane(Lane& l);

    int rate_;
    std::vector<Lane> lanes_;
    std::vector<int16_t> ring_;
    int64_t total_ = 0, floor_ = 0;
    double utc0_ = 0;
    bool anchored_ = false;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<int> jobs_;          // lane indices, FIFO
    bool stop_ = false;
    std::atomic<bool> abort_{false};
};

} // namespace vibe
