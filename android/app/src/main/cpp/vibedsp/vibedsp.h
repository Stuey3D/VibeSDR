// VibeSDR V5 — clean-room, GPL-free on-device DSP.
//
// This module replaces the SDR++ Brown / FFTW / VOLK DSP used by
// local_sdr_shim.cpp. It is platform-independent C++17 (Android + iOS) and
// depends only on permissively-licensed code (KissFFT, BSD-3, vendored under
// third_party/). Everything here is original VibeSDR code unless noted.
//
// Build order (see Reference/VibeSDR_v5_Clean_DSP_Brief.md):
//   Phase 1  RealFFT (waterfall)            <-- this file starts here
//   Phase 2  DDC (NCO + decimate) + AM/SSB/CW/NFM
//   Phase 3  WFM mono + de-emphasis
//   Phase 4  WFM stereo (MPX)
//   Phase 5  RDS (redsea)
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <complex>
#include <memory>
#include <vector>

namespace vibedsp {

// Sample types. Match the layout the shim already uses (interleaved float I/Q,
// stereo float L/R) so the swap into local_sdr_shim.cpp is mechanical.
using cf32 = std::complex<float>;
struct stereo { float l, r; };

// ── RealFFT ────────────────────────────────────────────────────────────────
// Forward real-to-complex FFT for the waterfall. Wraps KissFFT's kiss_fftr.
// `size` must be even (we always use powers of two). Not thread-safe; one
// instance per pipeline thread.
class RealFFT {
public:
    explicit RealFFT(int size);
    ~RealFFT();
    RealFFT(const RealFFT&) = delete;
    RealFFT& operator=(const RealFFT&) = delete;

    int size() const { return n_; }
    int bins() const { return n_ / 2 + 1; } // unique non-negative-freq bins

    // Transform `n` real input samples -> `bins()` complex outputs.
    // Caller supplies input length == size(); out length == bins().
    void forward(const float* in, cf32* out);

    // Convenience: power spectrum in dB (10*log10(|X|^2)), length bins().
    // `scale` normalises for FFT size + window gain (caller-computed).
    void powerDb(const float* in, float* outDb, float scale = 1.0f);

private:
    int n_;
    void* cfg_ = nullptr;            // kiss_fftr_cfg
    std::vector<cf32> scratch_;      // bins() complex outputs
};

// ── ComplexFFT ───────────────────────────────────────────────────────────--
// Forward complex-to-complex FFT for the IQ WATERFALL. Output spans the full
// band -fs/2 .. +fs/2 (positive AND negative frequencies), so this — not RealFFT
// — is what the spectrum display uses. `size` is the bin count (power of two).
class ComplexFFT {
public:
    explicit ComplexFFT(int size);
    ~ComplexFFT();
    ComplexFFT(const ComplexFFT&) = delete;
    ComplexFFT& operator=(const ComplexFFT&) = delete;

    int size() const { return n_; }
    void forward(const cf32* in, cf32* out);  // raw, DC at bin 0

    // Power spectrum in dB, fftshifted so bin 0 = -fs/2 and bin n/2 = DC (the
    // layout a waterfall expects). `win` (length size) is applied if non-null.
    void powerDbShifted(const cf32* in, const float* win, float* outDb, float scale = 1.0f);

private:
    int n_;
    void* cfg_ = nullptr;            // kiss_fft_cfg
    std::vector<cf32> in_, out_;     // working buffers (length n_)
};

// ── Windows ──────────────────────────────────────────────────────────────--
// Fill `w` (length n) with the named window. Nuttall matches the current
// IQFrontEnd::FFTWindow::NUTTALL used by the shim so waterfall look is preserved.
void nuttallWindow(float* w, int n);
double windowCoherentGain(const float* w, int n); // sum(w)/n, for normalisation

// ── NCO / complex mixer ──────────────────────────────────────────────────--
// Frequency translation: multiplies the input by exp(-j*2*pi*f*n), i.e. shifts
// a signal at normalised frequency +f (cycles/sample, Hz/fs) down to 0 Hz.
// This is the "tune" stage of the DDC. Streaming; keeps phase across calls.
class NCO {
public:
    explicit NCO(double normFreq = 0.0) { setFreq(normFreq); }
    void setFreq(double normFreq);               // cycles/sample
    void mix(const cf32* in, cf32* out, int n);  // out[i] = in[i]*exp(-j*phase)
    void reset() { cur_ = cf32(1.0f, 0.0f); sinceNorm_ = 0; }
private:
    // Recursive complex rotator: cur_ holds exp(-j*phase), advanced by a constant
    // multiply per sample (no per-sample trig — this runs at the full input rate,
    // so it's the hottest loop). Renormalised periodically to fight drift.
    cf32 cur_ = cf32(1.0f, 0.0f);
    cf32 rot_ = cf32(1.0f, 0.0f);                // exp(-j*step)
    int  sinceNorm_ = 0;
};

// ── FIR low-pass design ──────────────────────────────────────────────────--
// Windowed-sinc real low-pass. `cutoff` and `transition` are normalised
// (cycles/sample). Returns unity-DC-gain taps. Used as the DDC anti-alias /
// channel filter ahead of decimation, and reusable for audio shaping.
// `deepStop` swaps the Hamming window (~53 dB stopband) for a Blackman (~74 dB).
// Costs ~1.7x the taps for the same transition. Worth it for the LAST decimation
// stage: whatever it fails to attenuate FOLDS into the audio, and on a crowded band
// a neighbour 10 kHz away can be 60 dB louder than the signal you actually want.
std::vector<float> designLowpass(double cutoff, double transition, bool deepStop = false);

// ── FIR decimator (complex) ──────────────────────────────────────────────--
// Applies a real-tap low-pass to a complex stream and keeps every Dth sample.
// Streaming: state persists across process() calls. Only computes the outputs
// it keeps (no wasted MACs on discarded samples).
class FirDecimator {
public:
    FirDecimator(std::vector<float> taps, int decim);
    // Filters `n` inputs; writes up to n/D (+/-1) outputs; returns the count.
    // `out` must hold at least n/decim + 1 samples.
    int process(const cf32* in, int n, cf32* out);
    int decim() const { return decim_; }
    int maxOut(int n) const { return n / decim_ + 1; }
    void reset();
private:
    // Block-contiguous convolution: buf_ holds the K-1 history followed by the
    // current block, so each output is a forward dot product over contiguous
    // samples with the reversed taps — vectorisable (NEON). phase_ counts down to
    // the next kept (decimated) output.
    std::vector<float> rtaps_;   // reversed taps
    std::vector<cf32>  buf_;     // [K-1 history][block]
    int decim_, phase_, K_;
};

// ── Rational resampler (real/mono) ───────────────────────────────────────--
// Polyphase up-by-L / down-by-M resampler giving an output rate of exactly
// inRate*L/M, with L/M = outRate/inRate reduced. Used to land demod audio on an
// exact 48 kHz so playback pitch is correct regardless of channel rate.
class RationalResampler {
public:
    RationalResampler(int inRate, int outRate);
    int process(const float* in, int n, float* out);   // returns #outputs
    int maxOut(int n) const { return (int)((long long)n * L_ / M_) + 2; }
    int L() const { return L_; }
    int M() const { return M_; }
    void reset();
private:
    int L_, M_, phaseLen_;
    long long inCount_ = 0, outCount_ = 0;
    // Polyphase branches stored CONTIGUOUSLY and reversed (rBranch_[b*phaseLen+m]),
    // so each output is a forward NEON dot over a contiguous window of buf_ =
    // [phaseLen history][block]. (Was a strided prototype + circular history.)
    std::vector<float> rBranch_; // L_ branches x phaseLen_ taps, reversed
    std::vector<float> buf_;     // [phaseLen_ history][block]
};

// ── AM demodulator ───────────────────────────────────────────────────────--
// Envelope detector: audio = |z| with the carrier DC removed (one-pole DC
// blocker), then a fixed gain. Input is the DDC'd baseband channel; output is
// mono float audio at the channel rate. Streaming.
class AmDemod {
public:
    AmDemod() = default;
    void process(const cf32* in, float* out, int n);
    void reset() { dc_ = 0.0f; }
private:
    float dc_ = 0.0f;                  // DC-blocker state (running carrier level)
    static constexpr float kPole = 0.9995f;  // DC-blocker pole (~carrier removal)
    static constexpr float kGain = 2.0f;
};

// ── FM demodulator (NFM/quadrature discriminator) ────────────────────────--
// out[n] = gain * arg(z[n] * conj(z[n-1])): the instantaneous frequency, which
// is the FM audio. Used for narrowband FM; wideband FM (stereo/RDS) is a later
// phase built on the same discriminator.
class FmDemod {
public:
    explicit FmDemod(float gain = 1.0f) : gain_(gain) {}
    void setGain(float g) { gain_ = g; }
    void process(const cf32* in, float* out, int n);
    void reset() { prev_ = cf32(1.0f, 0.0f); }
private:
    cf32 prev_ = cf32(1.0f, 0.0f);
    float gain_;
};

// ── De-emphasis (one-pole, real) ─────────────────────────────────────────--
// FM de-emphasis: y[n] = y[n-1] + a*(x[n]-y[n-1]), a = dt/(tau+dt). 50 us (EU)
// or 75 us (US). Reconstructs the audio's HF balance after FM and helps reject
// the 19 kHz pilot in mono.
class Deemphasis {
public:
    void configure(double tauSec, double rate) {
        const double dt = 1.0 / rate;
        a_ = (tauSec > 0.0) ? (float)(dt / (tauSec + dt)) : 1.0f;
    }
    void process(float* x, int n) { for (int i = 0; i < n; ++i) { y_ += a_ * (x[i] - y_); x[i] = y_; } }
    void reset() { y_ = 0.0f; }
private:
    float a_ = 1.0f, y_ = 0.0f;
};

// ── Real FIR (low-pass / optional decimate) ──────────────────────────────--
// Real-input FIR for audio shaping (e.g. the 15 kHz mono low-pass that removes
// the 19 kHz pilot). decim=1 = plain filter. Streaming.
class RealFir {
public:
    RealFir(std::vector<float> taps, int decim = 1);
    int process(const float* in, int n, float* out);  // returns #outputs
    int maxOut(int n) const { return n / decim_ + 1; }
    void reset();
private:
    // Same block-contiguous + NEON scheme as FirDecimator, real samples.
    std::vector<float> rtaps_;   // reversed taps
    std::vector<float> buf_;     // [K-1 history][block]
    int decim_, phase_, K_;
};

// ── Stereo pilot PLL ─────────────────────────────────────────────────────--
// Locks to the 19 kHz FM stereo pilot in the MPX and generates phase-coherent
// 38 kHz (for L-R coherent detection) and 57 kHz (for RDS) references. Reports
// lock via a smoothed in-phase pilot amplitude.
class StereoPLL {
public:
    void configure(double pilotHz, double rate);
    // Advance one MPX sample; outputs coherent references (any may be null):
    // ref38 (L-R detection), ref57 (RDS carrier), bitClk (RDS 1187.5 Hz data
    // clock = pilot/16, phase in [0,2*pi)).
    void step(float mpx, float* ref38, float* ref57, float* bitClk = nullptr);
    // Hysteretic lock: engages only on a sustained pilot, releases on loss — so
    // static noise (whose smoothed correlation occasionally spikes) can't toggle
    // stereo on/off. lockAmp() is the raw smoothed metric (for blend/diagnostics).
    bool locked() const { return lockState_; }
    float lockAmp() const { return lockAmp_; }
    void reset() { phase_ = 0.0; dphase_ = w0_; lockAmp_ = 0.0f; cycle_ = 0; lockState_ = false; }
private:
    double w0_ = 0.0, phase_ = 0.0, dphase_ = 0.0;
    double alpha_ = 0.0, beta_ = 0.0;
    int cycle_ = 0;            // pilot-cycle counter within a bit (0..15)
    float lockAmp_ = 0.0f;
    float lockSmooth_ = 0.0005f;     // lock-metric 1-pole coeff (set by rate ~50ms)
    bool  lockState_ = false;        // hysteretic lock state
    static constexpr float kLockEngage = 0.060f;   // pilot present (real ~0.08)
    static constexpr float kLockRelease = 0.035f;  // pilot lost
};

// ── SSB / CW demodulator (Weaver / third method — true single-sideband) ────--
// Taking just Re{baseband} folds BOTH sidebands together (= DSB). The Weaver
// method gives real image rejection without a near-DC Hilbert: mix the complex
// baseband DOWN by bw/2 so the wanted sideband centres on 0, low-pass at bw/2
// (the unwanted sideband moves out of the passband and is rejected), then mix
// back UP by bw/2 and take the real part. USB up-mixes by +bw/2, LSB by -bw/2.
// CW is treated as USB (a BFO offset upstream gives the audible beat).
class SsbDemod {
public:
    enum class Side { USB, LSB };
    void configure(Side side, double bwHz, double rate);
    void process(const cf32* in, float* out, int n);
    void reset();
private:
    Side side_ = Side::USB;
    // Fixed-frequency bw/2 mix via a recursive rotator (no per-sample trig).
    cf32 rot_ = cf32(1.0f, 0.0f), cur_ = cf32(1.0f, 0.0f);
    int  sinceNorm_ = 0;
    std::unique_ptr<RealFir> lpfI_, lpfQ_; // matched complex low-pass at bw/2
    std::vector<float> aI_, aQ_, cbuf_, sbuf_, fI_, fQ_;
};

// ── Audio AGC (AM/SSB/CW) ──────────────────────────────────────────────────--
// Feed-forward envelope AGC on the demodulated audio. FAST attack catches peaks
// (no clipping/crackle) while SLOW release avoids pumping, and a max-gain cap
// stops it blowing up noise in the gaps. Without it SSB/CW fade up and down with
// the signal and crackle on peaks (FM modes don't need it — they're amplitude
// limited). Operates in place on real mono audio at the channel rate.
class Agc {
public:
    void configure(double rate) {
        atk_ = (float)(1.0 - std::exp(-1.0 / (rate * 0.002)));   // ~2 ms attack
        rel_ = (float)(1.0 - std::exp(-1.0 / (rate * 0.400)));   // ~400 ms release
    }
    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            const float a = std::fabs(x[i]);
            env_ += (a > env_ ? atk_ : rel_) * (a - env_);
            float g = kTarget / (env_ + 1e-6f);
            if (g > kMaxGain) g = kMaxGain;
            x[i] *= g;
        }
    }
    void reset() { env_ = kTarget; }
private:
    float env_ = kTarget, atk_ = 0.05f, rel_ = 1e-4f;
    static constexpr float kTarget  = 0.25f;   // output setpoint
    static constexpr float kMaxGain = 256.0f;  // ceiling (don't amplify silence)
};

// ── RDS data-link decoder ────────────────────────────────────────────────--
// Recovered RDS data bits -> block sync (syndrome + offset words) -> group
// parsing (PI, PS name, RadioText). Clean-room implementation of EN 50067 /
// IEC 62106; no GPL. The DSP front-end (57 kHz coherent demod + biphase symbol
// recovery) feeds pushBit().
class RdsDecoder {
public:
    struct Callbacks {
        void* ctx = nullptr;
        void (*ps)(void* ctx, uint16_t pi, const char* ps8) = nullptr;        // 8-char station name
        void (*radiotext)(void* ctx, const char* rt64) = nullptr;             // up to 64 chars
        void (*ecc)(void* ctx, uint16_t pi, uint8_t ecc) = nullptr;           // Extended Country Code (group 1A)
    };
    void setCallbacks(const Callbacks& c) { cb_ = c; }
    void reset();
    void pushBit(int bit);            // one recovered data bit (post differential)

    // Exposed for the DSP layer / tests (encoder round-trip).
    static uint16_t checkword(uint16_t data);           // 10-bit, no offset
    static const uint16_t OFFSET[5];                    // A, B, C, C', D
    static uint16_t syndrome(uint32_t block26);

private:
    void parseGroup();
    uint32_t reg_ = 0;
    bool synced_ = false;
    int bitsLeft_ = 0, nextBlk_ = 0, badRun_ = 0;
    uint16_t blk_[4] = {0, 0, 0, 0};
    bool blkOk_[4] = {false, false, false, false};
    char ps_[9] = {0};
    char rt_[65] = {0};
    uint8_t ecc_ = 0;                 // last decoded Extended Country Code (0 = none)
    Callbacks cb_{};
};

// ── RDS DSP front-end ────────────────────────────────────────────────────--
// Coherent 57 kHz demod of the MPX -> biphase symbol recovery -> differential
// decode -> data bits into an RdsDecoder. Uses the StereoPLL's coherent 57 kHz
// reference and pilot-locked bit clock (no separate timing loop). Original code.
class RdsDemod {
public:
    // The bit clock is frequency-accurate (pilot-locked) but its symbol-boundary
    // phase is unknown, so we run NPH timing-phase hypotheses in parallel, each
    // feeding its own RdsDecoder; only the aligned one achieves block sync and
    // emits PS/RadioText via the shared callbacks.
    void configure(double mpxRate, const RdsDecoder::Callbacks& cb);
    void reset();
    // Per-block: mpx samples + the PLL's coherent ref57 and bitClk arrays.
    void process(const float* mpx, const float* ref57, const float* bitClk, int n);
private:
    static constexpr int NPH = 16;
    std::unique_ptr<RealFir> lpf_;     // isolate the RDS baseband after downconvert
    double groupDelayPhase_ = 0.0;     // LPF delay expressed in bit-clock phase
    float acc_[NPH] = {0};
    float prevPhC_[NPH] = {0};
    int   prevSym_[NPH] = {0};
    bool  started_ = false;
    RdsDecoder dec_[NPH];
    std::vector<float> xbuf_, sbuf_;
};

// ── RxPipeline (the native engine) ───────────────────────────────────────--
// The complete IQ -> {spectrum, audio} chain that the shim's "Local Hardware
// (Native)" path runs, replacing the SDR++ Brown graph. Feed it raw IQ from the
// same source the shim already has (USB/rtl_tcp); it calls back with fftshifted
// spectrum dB rows and exact-48 kHz mono PCM. Phase 2 supports AM; more modes
// land in later phases behind the same interface.
class RxPipeline {
public:
    enum class Mode { AM, SSB_USB, SSB_LSB, CW, NFM, WFM /* mono; stereo+RDS later */ };

    struct Callbacks {
        void* ctx = nullptr;
        // fftshifted dB row, length == fftSize (bin 0 = -fs/2, fftSize/2 = DC).
        void (*spectrum)(void* ctx, const float* dbRow, int bins) = nullptr;
        // float audio at exactly outRate Hz. channels=1 -> mono (length frames);
        // channels=2 -> interleaved L,R (length 2*frames). WFM stereo uses 2.
        void (*audio)(void* ctx, const float* pcm, int frames, int channels, int outRate) = nullptr;
        // Optional: WFM RDS programme-service name (8 chars) + station PI code.
        void (*rdsPs)(void* ctx, uint16_t pi, const char* ps8) = nullptr;
        // Optional: WFM RDS RadioText (up to 64 chars).
        void (*rdsText)(void* ctx, const char* rt64) = nullptr;
        // Optional: WFM RDS Extended Country Code (group 1A) → station country.
        void (*rdsEcc)(void* ctx, uint8_t ecc) = nullptr;
        // Optional: WFM stereo-pilot lock state for the UI stereo indicator.
        void (*stereo)(void* ctx, bool locked) = nullptr;
    };

    // sampleRate = input IQ rate; fftSize = waterfall bins; fftRate = frames/sec;
    // outRate = audio rate (48000). Safe to call once before feed().
    void start(double sampleRate, int fftSize, double fftRate, int outRate,
               const Callbacks& cb);
    // Tune the demod channel: offsetHz from band centre, mode, channel bandwidth.
    void setTune(double offsetHz, Mode mode, double bwHz);
    void feed(const cf32* iq, int n);   // raw IQ from the source
    void stop();
    int outRate() const { return outRate_; }
    // WFM: force mono (off) vs allow stereo (on, default). When on, the L-R is
    // blended in by pilot-lock confidence so weak/edge signals fade smoothly
    // instead of screeching as lock flickers. Thread-safe to call any time.
    void setStereoEnabled(bool on) { stereoEnabled_ = on; }
    // Spectrum frame rate (frames/sec), changeable LIVE. This is a real power
    // lever, not just a bandwidth one: the rate sets how many input samples pass
    // between FFTs, so lowering it genuinely skips FFT work on the serving phone
    // (unlike the client's set_rate divisor, which only drops frames at send
    // time — the FFTs are still computed). Audio is untouched, so a throttled
    // server still sounds identical. Thread-safe to call any time.
    void setFftRate(double r) {
        if (r <= 0.0 || sampleRate_ <= 0.0) return;
        fftRate_ = r;
        specStride_.store(std::max(1, (int)std::llround(sampleRate_ / r)),
                          std::memory_order_relaxed);
    }
    double fftRate() const { return fftRate_; }

    // FM de-emphasis time constant (seconds): 0 = off, 50e-6 (EU/UK), 75e-6 (US).
    // Applies to WFM and NFM. Takes effect on the next tune/rebuild.
    void setDeemphasis(double tauSec) { deempTau_ = tauSec; dirty_ = true; }
    // Diagnostics: smoothed 19 kHz pilot lock amplitude + current blend (0..1).
    float pilotLockAmp() const { return pll_.lockAmp(); }
    float stereoBlend()  const { return stereoBlend_; }

private:
    void rebuildAudio();
    // config
    double sampleRate_ = 0.0, fftRate_ = 20.0, offsetHz_ = 0.0, bwHz_ = 10000.0;
    int fftSize_ = 1024, outRate_ = 48000;
    Mode mode_ = Mode::AM;
    Callbacks cb_{};
    bool dirty_ = true;

    // spectrum
    std::unique_ptr<ComplexFFT> cfft_;
    std::vector<float> win_, specBuf_, specDb_;
    int specFill_ = 0;          // samples gathered toward the next frame
    // Input samples between emitted frames. Atomic: setFftRate() writes it from
    // the control thread while feed() reads it on the DSP thread.
    std::atomic<int> specStride_{0};
    long long sinceFrame_ = 0;

    // audio DDC chain
    NCO nco_;
    // Decimation CASCADE, not one filter. Filter cost scales with the rate it runs
    // at, so decimating 50:1 in one step needs ~750 taps at the full input rate;
    // split into 5x5x2 the early stages need only ~9-17 (they just stop aliases
    // folding into the channel) and the narrow channel filter runs last, slowest,
    // and cheapest. Same audio, ~3x less CPU. See rebuildAudio().
    std::vector<std::unique_ptr<FirDecimator>> decs_;
    std::unique_ptr<AmDemod> am_;
    std::unique_ptr<FmDemod> fm_;
    std::unique_ptr<SsbDemod> ssb_;
    Agc  agc_;                              // audio AGC (AM/SSB/CW)
    bool useAgc_ = false;
    std::unique_ptr<RealFir> audioLpf_;     // WFM: 15 kHz (L+R / mono) LPF
    Deemphasis deemph_;                     // mono / L+R de-emphasis
    bool useDeemph_ = false;
    std::unique_ptr<RationalResampler> resamp_;     // mono / left
    // WFM stereo
    bool stereo_ = false;
    StereoPLL pll_;
    std::unique_ptr<RealFir> lmrLpf_;       // L-R 15 kHz LPF after 38 kHz mix
    Deemphasis deemphR_;
    std::unique_ptr<RationalResampler> resampR_;    // right channel
    std::vector<float> lprBuf_, lmrBuf_, leftBuf_, rightBuf_, rOutBuf_, ilvBuf_;
    bool lastStereo_ = false;
    std::atomic<bool> stereoEnabled_{true};  // user force-mono toggle (off = mono)
    float stereoBlend_ = 0.0f;               // smoothed L-R blend 0..1 (anti-screech)
    std::atomic<double> deempTau_{50e-6};    // FM de-emphasis tau (0=off / 50us / 75us)
    // WFM RDS
    RdsDemod rdsDemod_;
    std::vector<float> ref57Buf_, bitClkBuf_;
    int chDecim_ = 1;
    double chFs_ = 0.0;
    std::vector<cf32> baseBuf_, chBuf_;
    std::vector<float> demodBuf_, lpfBuf_, audioBuf_;
};

} // namespace vibedsp
