// decoders/digimodes/fst4_decoder.h — FST4 and FST4W, our own receiver (V12, 2026-10-31). Frame and codes: fst4_code.h.
//
// ★★★ WHAT THEY ARE. FST4 (QSO) and FST4W (beacons, WSPR's successor on LF/MF and HF) are 4-GFSK with the tones one
//     baud apart, 160 symbols: 40 sync in five groups of eight, 120 data carrying an LDPC codeword of 240 bits. T/R
//     periods 15 … 1800 s; the longer the period, the slower the baud and the weaker the signal it decodes.
//
// ★★ HOW IT DECODES. Each (mode, period) keeps a STREAM: the 12 kHz audio mixed down from its band's centre, decimated
//    (vibedsp's NCO + FIR, SIMD) and resampled (vibedsp's polyphase resampler) to a complex baseband whose symbol is
//    an exact sample count Ns, chosen so 2·Ns is a fast FFT size — memory stays small even for a 30-minute period, and
//    every DFT below is exactly one symbol long (tones orthogonal). At the end of each period, on the worker thread:
//   1. SEARCH — an STFT (one symbol, zero-padded ×2 → half-baud bins, every quarter symbol), per-bin median noise; the
//      sync score of a bin at a start is the mean over the 40 sync symbols of (expected tone − the other three) —
//      SIMD over bins. Local maxima above a threshold are candidates.
//   2. REFINE start (to 1/32 symbol) and frequency (to 1/32 baud) by the sync symbols' power at their tones.
//   3. SYMBOLS — the four tone energies of every data symbol, normalised by the noise measured at eight tone positions
//      beside the signal; bit log-likelihoods for the Gray map (log-sum-exp over the two tones of each bit value).
//   4. DECODE — belief propagation (fst4_code.h), CRC-24. FST4W tries its (240,74) code, then the (240,101) one its
//      hashed-call messages use.
// ★ Thread: decode() on the live decoder's worker; streaming on the caller's (decode) thread, ≈ a few multiply-adds
//   per input sample.
#pragma once
#include <atomic>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fst4_code.h"

namespace vibedsp { class NCO; class FirDecimator; class RationalResampler; }

namespace vibe {
namespace fst4 {

constexpr int kRate = 12000;
enum class Mode { FST4, FST4W };

struct Period { int sec; int nsps; double lead; int ns4; int ns4w; };   // ns* = baseband samples a symbol (0 = not run)
const Period* periodFor(int sec);
constexpr double kWinLeadSec = 1.0, kWinTailSec = 1.0;

struct Result {
    Mode   mode = Mode::FST4;
    int    periodSec = 60;
    float  f0 = 0;          ///< audio Hz of tone 0 — WSJT-X's frequency column
    float  dt = 0;          ///< s from the nominal start
    int    snr = 0;         ///< dB in 2500 Hz
    int    iters = 0;
    float  sync = 0;
    bool   k101 = true;     ///< which code decoded it
    double slotUtc = NAN;
    int    dbm = 0;         ///< FST4W type 1
    std::string text, call, grid;
    std::string modeName() const;   ///< "FST4-60", "FST4W-120"
};

struct SlotOptions {
    float loHz = 300, hiHz = 2700;   ///< tone 0 from loHz; tone 3 at most hiHz
    int   maxCands = 20;
    int   maxIter = 40;
    float syncMin = 1.0f;            ///< candidate threshold (sync score; noise peaks ≈ 1.0 — 0.7 found 1 more in 20 at −26 for 5× the work)
    bool  loaded = false;
    const std::atomic<bool>* abort = nullptr;
};

struct SlotStats { int candidates = 0, attempts = 0, decodes = 0, rejected = 0; double totalMs = 0; };

/** One mode and period: the band centre, the baseband rate and the decode of one window. */
class SlotDecoder {
public:
    SlotDecoder(Mode m, int periodSec, double centreHz);
    ~SlotDecoder();
    bool ok() const { return p_ != nullptr && ns_ > 0; }
    const Period& period() const { return *p_; }
    Mode mode() const { return mode_; }
    double rate() const { return rate_; }        ///< baseband samples per second
    int ns() const { return ns_; }
    double centreHz() const { return fc_; }
    int windowSamples() const { return (int)std::lround((p_->sec + kWinLeadSec + kWinTailSec) * rate_); }
    /** `z` = the window at rate(), sample 0 = 1 s before the period, baseband 0 Hz = centreHz(). */
    int decode(const std::complex<float>* z, int n, const SlotOptions& o, std::vector<Result>& out, SlotStats* st = nullptr);
    static constexpr float kScoreMin = 1.6f;   ///< mean normalised energy at the decoded data tones (see the guard)
private:
    struct Cand { int bin; int frame; float score; };
    bool tryCand(const std::complex<float>* z, int n, const Cand& c, const SlotOptions& o, std::vector<Result>& out, SlotStats& st);
    const Period* p_ = nullptr;
    Mode mode_;
    double fc_ = 1500, rate_ = 0, baud_ = 0;
    int ns_ = 0, nfft_ = 0, hop_ = 0;
    void* setup_ = nullptr;
    float *fin_ = nullptr, *fout_ = nullptr, *fwork_ = nullptr;
    std::vector<float> P_, S4_;
    std::vector<uint8_t> seen_;
};

/** Audio (12 kHz real) → the complex baseband a SlotDecoder reads, streaming. */
class Stream {
public:
    Stream(Mode m, const Period& p, double centreHz);
    ~Stream();
    /** Appends the baseband for `n` audio samples to `out`. */
    void push(const float* audio, int n, std::vector<std::complex<float>>& out);
    /** The two halves of push(): the mix + first decimation (identical for every period of a mode — the live decoder
     *  runs it once), then this period's resampler. */
    void stage1(const float* audio, int n, std::vector<std::complex<float>>& d);
    void stage2(const std::complex<float>* d, int m, std::vector<std::complex<float>>& out);
    double rate() const { return rate_; }
    /** The chain's delay, in baseband samples: subtract from an index to get the audio time it came from. */
    double delay() const { return delay_; }
private:
    std::unique_ptr<vibedsp::NCO> nco_;
    std::unique_ptr<vibedsp::FirDecimator> dec_;
    std::unique_ptr<vibedsp::RationalResampler> rr_, ri_;
    std::vector<std::complex<float>> a_, b_;
    std::vector<float> re_, im_, ore_, oim_;
    double rate_ = 0, delay_ = 0;
};

/* ★★ THE LIVE DECODER (one mode, any of its periods): audio with its capture UTC in; one Stream and baseband ring per
 *  period; at the end of each period's window the window is copied to the worker thread. A window arriving while the
 *  same period's previous one is still queued is SKIPPED and counted, never waited for. */
class LiveDecoder {
public:
    LiveDecoder(Mode m, std::vector<int> periods, double centreHz, SlotOptions opts);
    ~LiveDecoder();
    void process(const float* audio, int n, double captureUtc);
    std::function<void(const Result&)> onResult;
    std::function<bool()> loadProbe;
    std::atomic<uint64_t> slotsSkipped{0}, slotsDecoded{0};
    std::atomic<double> lastDecodeMs{0};
    void drain();
private:
    struct Per {
        std::unique_ptr<SlotDecoder> dec;
        std::unique_ptr<Stream> st;
        std::vector<std::complex<float>> ring;
        int64_t total = 0;            // baseband samples ever written
        double next = NAN;            // UTC of the next period start to decode
        bool queued = false;
    };
    struct Job { int pi; double slotUtc; std::vector<std::complex<float>> win; };
    void worker();
    Mode mode_;
    SlotOptions opts_;
    std::vector<Per> per_;
    std::vector<std::complex<float>> tmp_, d1_;
    int64_t audioTotal_ = 0;
    double utc0_ = 0; bool anchored_ = false;
    std::thread th_;
    std::mutex m_; std::condition_variable cv_, idle_;
    std::deque<Job> q_;
    bool busy_ = false, stop_ = false;
    std::atomic<bool> abort_{false};
};

} // namespace fst4
} // namespace vibe
