// decoders/digimodes/msk144_decoder.h — MSK144, our own receiver (V12, 2026-10-31). Frame and code: msk144_code.h.
//
// ★★★ WHAT IT IS. MSK144 is the meteor-scatter mode of 6 m and 2 m: a 72 ms frame (2000 bit/s MSK, 1000/2000 Hz tones
//     around 1500 Hz audio) repeated back to back for the whole 15 s (or 30 s) transmission, and heard only while a
//     meteor trail reflects it — a "ping" of tens of milliseconds to a few seconds, at any moment. So the receiver is
//     FREE-RUNNING: it reads every stretch of audio for frames, however short.
//
// ★★ HOW IT DECODES (msk144_decoder.cpp has the detail).
//   0. STREAM — the 12 kHz audio mixed down from 1500 Hz and decimated to a 6 kHz complex baseband (vibedsp's NCO +
//      FIR, SIMD); a half-sine matched filter (5 taps). MSK is offset-QPSK with half-sine pulses: every frame bit is
//      the sign of the filter's output at its own instant, on the real axis (even bits) or the imaginary (odd), against
//      ONE carrier phase for the whole transmission — so the receiver treats it as coherent BPSK.
//   1. SEARCH, per 1.5 s window (1 s hop): for 9 frequency offsets (±200 Hz in 50 Hz steps) and every sample, the two
//      8-bit sync words' correlations (additions only — the reference is ±1/±j; SIMD over four starts); a frame's
//      score is the sum of their powers over the noise. Scores are folded at the frame period: runs of 1, 2, 4, 8 and 16
//      frames at each frame phase are judged against chance (a ping of one frame, or a long weak one).
//   2. REFINE — start to a fraction of a sample, frequency by the run's coherent sync sum (to ≈ 0.1 Hz on a long ping).
//   3. DETECT — the bits of the run's frames averaged COHERENTLY (one phase, the refined frequency: a long ping adds up
//      like one strong frame), and, as a second try, per-frame phases with each frame weighted by its own amplitude
//      (maximal-ratio combining: a fading or drifting ping). LLRs from the in-phase part over the quadrature's noise.
//   4. DECODE — belief propagation (ldpc_small.h), then OSD; CRC-13; guards (the decoded codeword must agree with what
//      was received, and a plausible message).
// ★ Thread: everything on the caller's (the host's decode) thread: ≈ 0.4 M complex additions a second of audio for the
//   search, a few hundred microseconds per candidate decoded.
#pragma once
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "msk144_code.h"

namespace vibedsp { class NCO; class FirDecimator; }

namespace vibe {
namespace msk144 {

constexpr int kRate = 12000;   ///< audio in
constexpr int kBbRate = 6000;  ///< baseband: 3 samples a bit
constexpr int kSpb = 3;
constexpr int kFrameSamples = kFrameBits * kSpb;   // 432

struct Result {
    std::string text, call, grid;
    int    snr = 0;            ///< dB in 2500 Hz (WSJT-X's convention)
    float  f0 = 1500;          ///< audio Hz of the carrier centre
    double utc = NAN;          ///< UTC of the ping's first frame (NaN when the receiver has no clock)
    double audioSec = 0;       ///< …and its time in the receiver's own audio clock (seconds since the first sample)
    int    frames = 0;         ///< frames averaged
    bool   osd = false;
    bool   mrc = false;        ///< decoded by the per-frame (maximal-ratio) detector
    int    iters = 0;
    int    errors = 0;         ///< hard decisions the decode overturned
    float  score = 0;          ///< the run's sync score (σ above chance)
};

struct Options {
    float centreHz = 1500;     ///< audio centre of the search (WSJT-X's default Rx frequency)
    float tolHz = 200;         ///< ± search
    int   maxCands = 6;        ///< candidates decoded per window
    float candZ = 4.5f;        ///< a run's score must stand this many σ above chance
    int   maxIter = 40;
    bool  osd = false;         // ★ OFF: CRC-13 cannot vet OSD's trials (DECODERS-WAVE2.md "MSK144": 19 of 31 OSD decodes wrong)
    int   osdPairs = 12;
    bool  loaded = false;      ///< the host is busy: fewer candidates, no OSD
};

struct Stats { uint64_t windows = 0, candidates = 0, attempts = 0, decodes = 0, rejected = 0; double ms = 0; };

/** The streaming receiver: audio in, results out (synchronously, from push()). */
class Receiver {
public:
    explicit Receiver(Options o = Options());
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    /** 12 kHz real audio, ±1. */
    void push(const float* audio, int n);
    /** Analyse whatever is buffered now (end of a file). */
    void flush();
    std::function<void(const Result&)> onResult;
    Options opts;
    Stats stats;
    double audioSec() const { return (double)audioTotal_ / kRate; }
    /** UTC of audio sample 0 (set by the live decoder; NaN = none). */
    double utc0 = NAN;
private:
    void analyse(int nValid);
    struct Cand { int tau; int f; int m0; int L; float z; };
    bool tryCand(const Cand& c, int nValid, int64_t base);
    std::unique_ptr<vibedsp::NCO> nco_;
    std::unique_ptr<vibedsp::FirDecimator> dec_;
    std::vector<std::complex<float>> a_, b_, d_;
    std::vector<std::complex<float>> zh_;          // the last 4 baseband samples (matched-filter history)
    std::vector<float> qre_, qim_;                 // matched-filter output, the window
    int64_t bbTotal_ = 0, audioTotal_ = 0, qStart_ = 0;   // baseband index of qre_[0]
    struct FftBuf { int n = 0; void* setup = nullptr; float *in = nullptr, *out = nullptr, *work = nullptr; ~FftBuf(); };
    FftBuf& fft(int n);
    std::vector<std::unique_ptr<FftBuf>> ffts_;
    std::vector<std::vector<float>> sScores_;     // per coarse frequency: each start's frame score (analyse → tryCand)
    std::vector<float> noiseHist_;
    bool debug_ = false;
    std::vector<std::vector<float>> rotRe_, rotIm_; // per coarse frequency: e^{−j2π·f·i/6000}, i < 512                          // MSK_DEBUG: every attempt to stderr (the bench)
    float noise_ = 0;
    struct Recent { std::string text; double t; };
    std::vector<Recent> recent_;
};

} // namespace msk144
} // namespace vibe
