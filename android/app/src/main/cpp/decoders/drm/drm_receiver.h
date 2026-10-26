// decoders/drm/drm_receiver.h — a DRM30 RECEIVER (robustness modes A–D) from ETSI ES 201 980 V4.3.1: complex baseband in,
// FAC / SDC / service information / text messages / MSC logical frames out. The audio codec is NOT here — DRM's audio
// leaves as access units for the platform's own decoder (drm_audio.h), exactly as DAB+ does (vibe_dab_aacdec.h).
//
// ── The chain ────────────────────────────────────────────────────────────────────────────────────────────────────
//  0. Resample to 24 kHz (one FIR + cubic interpolation; any input rate ≥ 12 kHz). 24 kHz makes every mode's symbol a
//     whole number of samples and every carrier an FFT bin (drm_params.h).
//  1. SEARCH — mode and symbol timing together: the guard interval is a copy of the symbol's tail, so x[n]·x*[n+Tu]
//     summed over Tg peaks at each guard; FOLDED modulo Ts over 0.8 s the peak stands clear of noise, its height names
//     the mode (each mode's Tu/Tg differ), its position is the timing and its phase the fractional frequency offset.
//     ★ The peak must stand over the fold's MEDIAN, not just be high: an AM carrier correlates at every lag (flat fold),
//       noise nowhere — neither is a DRM signal, and that is the first of the three gates against false locks.
//  2. FREQUENCY — the integer carrier offset: the three frequency references (750, 2250, 3000 Hz) are continuous tones,
//     so their bins sum COHERENTLY over a frame while data cells cancel. Searched ±1.5 kHz.
//  3. FRAME — symbol 0 carries the time references (tables 53–56); their phase pattern, compared carrier-to-neighbour
//     (immune to the channel's slow phase), scores each symbol position over two frames.
//  4. TRACK, per symbol — FFT (PFFFT: NEON/SSE; kiss for 352/224), frequency tracked from the reference tones' phase
//     step, timing from the channel's phase slope (integer window steps; their rate IS the sample-clock offset).
//  5. CHANNEL — the scattered gain references (table 58) interpolated in TIME (linear, y−1 symbols of look-ahead) then in
//     FREQUENCY (linear, after removing the slope that the window offset puts across the band); equalised cells carry
//     |H|² as their CSI weight. SIMD equaliser.
//  6. FAC every frame (CRC-8 AND field validity — the second gate; the third is two agreeing FACs before anything is
//     announced), SDC every super frame (CRC-16), MSC through the cell deinterleaver (short or the 5-frame long one) and
//     the multistage MLC decoder (drm_mlc.h).
//
// ★ No threads, no locks, no allocation in the steady state beyond vector reuse: the caller (the server's DRM listener)
//   owns the thread and feeds it behind a drop-never-wait queue.
#pragma once
#include "drm_info.h"
#include "drm_mlc.h"
#include "drm_params.h"
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace vibedsp { class ComplexFFT; }

namespace vibedrm {

struct RxStatus {
    // the six lights
    bool timeSync = false, freqSync = false, frameSync = false, facOk = false, sdcOk = false, mscOk = false;
    bool locked = false;              // two agreeing FACs: a DRM signal is CERTAINLY here
    Mode mode = Mode::None;
    int so = -1;
    double freqOffsetHz = 0;          // the signal's offset from the dial (positive = above)
    double sroPpm = 0;                // sample-clock offset, transmitter vs us
    float facMerDb = 0, mscMerDb = 0; // MER of the FAC (4-QAM, robust) and of the MSC cells
    float wmerDb = 0;                 // ★ the FAC MER is what we call "SNR" to a listener — it is the decision SNR
    int mscLevels = 0, sdcLevels = 0, protA = 0, protB = 0; bool longInterleave = true;
    double codeRateB = 0;             // overall MSC code rate of part B
    int bitrateBps = 0;               // the audio stream's net rate
    Fac fac; Sdc sdc;
    std::string text;
    int64_t facGood = 0, facBad = 0, sdcGood = 0, sdcBad = 0, superFrames = 0;
};

/** One logical frame of an MSC stream (400 ms). */
struct LogicalFrame { int stream = 0; std::vector<uint8_t> bytes; int lenA = 0; };

class Receiver {
public:
    Receiver();
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void reset();
    /** Complex baseband centred on the DRM reference frequency (the dial), at `rateHz` (≥ 12 kHz). */
    void feed(const cf* iq, int n, double rateHz);

    std::function<void(const LogicalFrame&)> onLogicalFrame;
    /** Fires after every decoded super frame (and on loss of sync) — the moment to publish status. */
    std::function<void()> onSuperFrame;

    const RxStatus& status() const { return st_; }
    /** Samples processed since reset, and the DSP time those cost (for the CPU row). */
    int64_t samplesIn() const { return samplesIn24_; }
    /** Fraction of samples the pulse blanker has zeroed. */
    double blankedFraction() const { return samplesIn24_ ? double(blanked_) / double(samplesIn24_) : 0.0; }

private:
    enum class State { Search, Freq, Track };
    // ── resampler ──
    void resampleIn(const cf* x, int n, double rate);
    double rsRate_ = 0; std::vector<float> rsH_; std::vector<cf> rsHist_, rsF_; double rsPos_ = 0;
    // ── sample buffer (24 kHz) ──
    std::vector<cf> buf_; int64_t bufBase_ = 0; int64_t samplesIn24_ = 0;
    void trimBuffer(int64_t keepFrom);
    void blankNew();
    int64_t blankFrom_ = 0; float pAvg_ = 0; float blankWin_[8] = {0}; uint32_t blankWi_ = 0; int blankHold_ = 0, blankRun_ = 0; int64_t blanked_ = 0;
    cf at(int64_t absIdx) const { return buf_[size_t(absIdx - bufBase_)]; }
    int64_t bufEnd() const { return bufBase_ + int64_t(buf_.size()); }
    // ── state machine ──
    State state_ = State::Search;
    void runSearch();
    void runFreq();
    void runTrack();
    void loseSync(const char* why);
    Mode mode_ = Mode::None;
    int tu_ = 0, tg_ = 0, ts_ = 0, kw_ = 0;   // kw_: the widest carrier index tracked (SO 3's)
    int64_t winPos_ = 0;                       // absolute index of the next FFT window start
    double freq_ = 0;                          // Hz, removed from the signal
    double ncoPhase_ = 0; int64_t ncoIdx_ = 0;
    std::unique_ptr<vibedsp::ComplexFFT> fft_;
    std::vector<cf> tin_, tout_;
    void fftSymbol(int64_t pos, std::vector<cf>& carriers);   // carriers[-kw..kw]
    // ── tracking ──
    std::vector<cf> prevSym_; bool havePrev_ = false; int prevStep_ = 0;
    double tauAvg_ = 0; int tauN_ = 0; int64_t stepSum_ = 0, trackStart_ = 0;
    int pendingStep_ = 0;
    // ── frame sync ──
    std::vector<double> tScore_; int symCount_ = 0;
    bool frameSynced_ = false; int sIdx_ = 0;  // s of the NEXT symbol to be processed
    int64_t gIdx_ = 0;                         // global symbol counter (aligned so gIdx_ % ns == s)
    // ── channel estimation ──
    struct Sym { int64_t g = 0; int s = 0; std::vector<cf> X; std::vector<cf> Hp; std::vector<uint8_t> hasP; };
    std::deque<Sym> ring_;
    CellMap emap_;                              // the estimator's map (SO 3 until the FAC says otherwise)
    int estSo_ = 3;
    void pushEstimator(std::vector<cf>& carriers);
    void equalise(const Sym& sy, std::vector<cf>& Y, std::vector<float>& W);
    void applyStepToRing(int delta);
    // ── frames / super frames ──
    std::vector<cf> frY_; std::vector<float> frW_; int frS_ = 0;   // current frame's equalised cells, s × nk
    void frameDone();
    struct FrameStore { bool have = false; std::vector<cf> Y; std::vector<float> W; Fac fac; bool facOk = false; };
    FrameStore sf_[3];
    int lastFrameIdx_ = -1;
    int facFailRun_ = 0, facGoodRun_ = 0;
    Fac lastFac_; bool haveFac_ = false;
    void superFrameDone();
    CellMap smap_;                              // map for the super frame decode (FAC's mode/SO)
    MlcDecoder mlc_;
    MlcLayout facL_;
    CellDeinterleaver cdi_;
    TextMessageDecoder text_;
    RxStatus st_;
    float facMerAcc_ = 0, mscMerAcc_ = 0; bool merInit_ = false;
};

}  // namespace vibedrm
