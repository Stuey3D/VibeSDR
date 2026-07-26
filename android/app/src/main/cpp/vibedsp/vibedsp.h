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
    // Block form — what the pipeline actually calls. Advances n MPX samples and
    // writes the coherently-detected L-R (mpx * ref38 * 2), plus the RDS 57 kHz
    // reference and bit clock (both may be null to skip the RDS work entirely).
    // Same maths as step(), just without the per-sample call and null checks.
    // ref57/ref57q are the IN-PHASE and QUADRATURE 57 kHz references. RDS detection
    // needs both: with only the in-phase term, any phase error between our pilot-derived
    // carrier and the station's subcarrier scales the recovered data by cos(theta) — and
    // kills it outright at 90 degrees. All four may be null to skip that work.
    void processBlock(const float* mpx, int n, float* lmr,
                      float* ref57, float* ref57q, float* bitClk);
private:
    inline void advance(float mpx);          // one loop iteration (no trig)
public:
    // Hysteretic lock: engages only on a sustained pilot, releases on loss — so
    // static noise (whose smoothed correlation occasionally spikes) can't toggle
    // stereo on/off. lockAmp() is the raw smoothed metric (for blend/diagnostics).
    bool locked() const { return lockState_; }
    // ★★ A SEPARATE, MUCH LOWER BAR FOR RDS — because it is a different question.
    // locked() answers "is the pilot strong enough for stereo AUDIO", where the noisy
    // L-R sideband is added straight into what you hear, so the threshold is deliberately
    // cautious. RDS asks only "is the PLL TRACKING well enough to give a coherent
    // reference", and its differential detector cancels carrier phase algebraically, so
    // it tolerates far more phase noise than a stereo decoder ever could.
    // Gating RDS on the stereo threshold switched it OFF ENTIRELY on any station too weak
    // for clean stereo — not degraded, off — and on a marginal pilot the state flickers,
    // so RDS arrived in bursts with block sync discarded in between. That is why a strong
    // station was instant and everything else struggled (Stuart, on air, 2026-07-26).
    bool trackable() const { return trackState_; }
    float lockAmp() const { return lockAmp_; }
    void reset() { phase_ = 0.0; df_ = 0.0; lockAmp_ = 0.0f; cycle_ = 0; lockState_ = false; trackState_ = false;
                   oscC_ = 1.0f; oscS_ = 0.0f; sinceNorm_ = 0; }
private:
    // The oscillator is a RECURSIVE ROTATOR (same trick as Nco/SsbDemod), not a
    // sin/cos pair: it used to call std::sin AND std::cos in double precision on
    // every MPX sample at the channel rate, which was the single most expensive
    // thing in WFM. Now each sample is a complex multiply by exp(j*w0) times a
    // small-angle correction, renormalised periodically to fight float drift.
    double w0_ = 0.0, phase_ = 0.0, df_ = 0.0;   // df_ = VCO deviation from w0_
    double alpha_ = 0.0, beta_ = 0.0;
    float rotC_ = 1.0f, rotS_ = 0.0f;   // exp(j*w0), the nominal per-sample step
    float oscC_ = 1.0f, oscS_ = 0.0f;   // cos(phase_), sin(phase_) — running state
    // The oscillator AT the sample just processed, i.e. before advance() rotated
    // it on. The coherent references must be taken at the phase the MPX sample was
    // observed at; using the post-advance phase lags the 38 kHz subcarrier by a
    // whole channel sample (~43 degrees at 320 kHz) and wrecks stereo separation.
    float outC_ = 1.0f, outS_ = 0.0f;
    int   sinceNorm_ = 0;
    int cycle_ = 0;            // pilot-cycle counter within a bit (0..15)
    float lockAmp_ = 0.0f;
    float lockSmooth_ = 0.0005f;     // lock-metric 1-pole coeff (set by rate ~50ms)
    bool  lockState_ = false;        // hysteretic lock state (stereo audio)
    bool  trackState_ = false;       // hysteretic PLL-is-tracking state (RDS)
    static constexpr float kLockEngage = 0.060f;   // pilot present (real ~0.08)
    static constexpr float kLockRelease = 0.035f;  // pilot lost
    // Tracking-only thresholds: well below the stereo bar, with wide hysteresis so a
    // fade cannot chatter the gate and cost RDS its block sync.
    static constexpr float kTrackEngage = 0.015f;
    static constexpr float kTrackRelease = 0.007f;
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
        // ★ PI FIRST. PI rides in block A of EVERY group (~11 times a second) and is a
        // fixed 16-bit value, so it is confirmable by simple repetition — no assembly
        // across groups, no addressing, nothing to corrupt. It is therefore the FIRST
        // thing that becomes trustworthy on a weak signal, often long before a name can
        // be assembled, and on its own it identifies the station to a database lookup.
        // Reporting it separately means a DXer sees an identification where the old
        // all-or-nothing path showed an empty box (Stuart, 2026-07-26).
        void (*pi)(void* ctx, uint16_t pi) = nullptr;
    };
    void setCallbacks(const Callbacks& c) { cb_ = c; }
    void reset();
    // Arbitration support: RdsDemod runs NPH timing hypotheses, and a MISALIGNED one can
    // still stumble into block sync and emit rubbish through the shared callbacks — which
    // is how a good station ends up reporting a two-character name. So only the best
    // hypothesis is allowed to speak, judged on these.
    bool synced() const { return synced_; }
    int  recentGood() const;          // good blocks within the last kRateWindow
    void pushBit(int bit);            // one recovered data bit (post differential)

    // ── Reporting (the DX numbers) ───────────────────────────────────────────
    // ★ BLOCK ERROR RATE, defined as redsea defines it: the percentage of blocks that
    // arrived with a non-zero syndrome BEFORE correction, over the last kBerGroups
    // groups. Errors before correction — not after — is the honest measure, because it
    // describes the LINK rather than how hard we worked on it, and it stays meaningful
    // as the correction table widens. -1 until a full window has been seen.
    // This is the instrument this decoder has always lacked: it separates "the blocks
    // are damaged" from "we are throwing good blocks away", which are opposite faults
    // with opposite fixes.
    int blockErrorPercent() const;
    /** Last CONFIRMED PI (two agreeing receptions), or 0 if none yet. */
    uint16_t confirmedPi() const { return piConfirmedVal_; }

    // ── ★★ FIELDS THAT COST NOTHING ──────────────────────────────────────────
    // PTY and TP ride in BLOCK B, which every group we parse already requires and has
    // already error-checked — we were reading four of its sixteen bits and discarding the
    // rest. TA/MS/DI likewise, in type 0 groups. Same shape as the PI bug: recovered,
    // validated, then thrown away at the door (2026-07-26).
    int  pty() const { return pty_; }        // -1 = not yet seen
    int  tp()  const { return tp_; }         // traffic programme, -1 unknown
    int  ta()  const { return ta_; }         // traffic announcement now, -1 unknown
    int  ms()  const { return ms_; }         // 1 = music, 0 = speech, -1 unknown
    /** Alternative frequencies, kHz, de-duplicated. Rides in 0A block C, which we already
     *  receive. More than a display field: a list of where else the same PI can be found
     *  feeds the FM-DX dial and learned stations directly (Stuart). */
    /** DI — decoder identification: bit0 stereo, bit1 artificial head, bit2 compressed,
     *  bit3 dynamic PTY. Assembled across the four type-0 addresses. -1 = none yet. */
    int  di() const { return diSeen_ == 0xF ? di_ : -1; }
    /** Clock time from group 4A: minutes since UTC midnight, and the local offset in
     *  half-hours. -1 = not received. ★ A DXer's favourite: CT is transmitted once a
     *  minute, so receiving one at all proves the link is carrying whole groups intact,
     *  and its VALUE identifies the network's timezone. */
    int  ctMinutes() const { return ctMin_; }
    int  ctOffsetHalfHours() const { return ctOff_; }
    /** How many groups of each type/version (0A=0, 0B=1, 1A=2 ... ) have been decoded.
     *  ★ Identifies a transmitter's configuration at a glance, and shows whether a weak
     *  signal is delivering a representative mix or only the easy groups. */
    int  groupCount(int idx) const { return (idx >= 0 && idx < 32) ? grpCount_[idx] : 0; }
    int  groupTotal() const { return grpTotal_; }
    int  afCount() const { return afN_; }
    int  afKhz(int i) const { return (i >= 0 && i < afN_) ? afKhz_[i] : 0; }
    static constexpr int kMaxAf = 25;

    // ── Correction strength (the "advanced RDS" lever) ───────────────────────
    // Longest burst, in bits, the correction table will attempt to repair. The standard
    // permits 5. Wider means more blocks recovered on a weak signal AND more chances to
    // "repair" noise into something that looks valid, so it is a genuine trade rather
    // than a free win — which is exactly why it belongs to the user on a DX receiver and
    // not to us. Downstream trust is weighted by how many bits a block needed, so a wide
    // setting degrades confidence rather than silently corrupting the display.
    static void setMaxBurstBits(int bits);   // clamped to 1..5; default kDefaultBurst
    // ★★ DEFAULT 2, NOT 5 — measured on air 2026-07-26. Widening to the standard's full
    // 5-bit burst was tried and REGRESSED badly: BBC Radio 4, in solid stereo, produced no
    // RDS whatsoever. A false correction on block A yields a WRONG PI; the parser compares
    // it with the previous PI, they disagree, and because piLast_ is overwritten with each
    // invention, two readings never agree — so PI never confirms and, with PI gating
    // repaired groups, NOTHING gets through. A strong station needing no corrections was
    // unaffected, which is exactly the pattern observed.
    // ★ The old 2-bit cap was not timidity; the file said why, and it was right. The
    // synthetic probe could not see this at all — Gaussian noise does not produce the
    // burst patterns that make a wide table dangerous. Keep the lever for the FM-DX
    // toggle, where a user can accept the trade knowingly and watch the BER.
    static constexpr int kDefaultBurst = 2;

    // Exposed for the DSP layer / tests (encoder round-trip).
    static uint16_t checkword(uint16_t data);           // 10-bit, no offset
    static const uint16_t OFFSET[5];                    // A, B, C, C', D
    static uint16_t syndrome(uint32_t block26);

private:
    void parseGroup();
    // ── Weak-signal block recovery ───────────────────────────────────────────
    // Sync used to LATCH on a single block-A syndrome, and any one bad bit threw a
    // block away. Both are costly on a marginal signal, and both have standard
    // answers (EN 50067 / IEC 62106); the shape of the fix here follows the approach
    // taken by redsea (Oona Räisänen, MIT) — see the notes in rds.cpp. Our own code.
    //
    // 1. Burst-error correction. The 10-bit checkword is a shortened cyclic code, so
    //    a syndrome identifies a correctable error PATTERN, not merely "bad". We
    //    precompute syndrome -> error-vector for 1- and 2-bit bursts at every offset.
    // 2. Rhythm sync. A real block stream arrives on a 26-bit grid in the cyclic
    //    order A,B,C/C',D — so require several pulses that agree on that grid before
    //    declaring sync, instead of trusting one match that noise can fake.
    struct SyncPulse { uint8_t seq; uint64_t bitPos; };
    static const uint32_t* errorTable();     // 1024 syndromes -> error vector (0 = none)
    static int seqOfOffset(uint16_t offsetIdx);
    // Returns whether the block is trustworthy. `repairBits` reports HOW MANY bits had
    // to be flipped — 0 for a clean checkword match.
    // ★★ It used to report a bare bool, and that single bit of lost information is why
    // the group parser had to be so blunt: unable to tell a pristine block from one
    // rebuilt out of a 2-bit patch, its only safe policy was to distrust the WHOLE group
    // if anything anywhere had been touched. A count lets the parser WEIGH evidence
    // instead of VETOING it — the same thing a hardware RDS decoder gives its host,
    // which is how those parsers afford to be decisive (2026-07-26).
    bool tryCorrect(uint16_t offsetIdx, uint32_t& block26, int& repairBits) const;
    void notePulse(int seq);                 // candidate block boundary while unsynced
    static constexpr int kSyncPulses = 6;    // ring of recent candidates
    static constexpr int kSyncNeeded = 3;    // agreeing pulses required to declare sync
    static constexpr int kRateWindow = 50;   // blocks considered when dropping sync
    static constexpr int kRateDrop   = 25;   // ...and how many may be bad
    SyncPulse pulses_[kSyncPulses] = {};
    int pulseCount_ = 0;
    uint64_t bitPos_ = 0;                    // bits pushed since reset (the grid)
    uint64_t badHist_ = 0;                   // 1 = block failed, newest in bit 0
    int blocksSeen_ = 0;
    uint32_t reg_ = 0;
    bool synced_ = false;
    int bitsLeft_ = 0, nextBlk_ = 0;
    uint16_t blk_[4] = {0, 0, 0, 0};
    bool blkOk_[4] = {false, false, false, false};
    char ps_[9] = {0};
    char rt_[65] = {0};
    uint8_t ecc_ = 0;                 // last decoded Extended Country Code (0 = none)
    int pty_ = -1, tp_ = -1, ta_ = -1, ms_ = -1;
    int afKhz_[kMaxAf] = {0};
    int afN_ = 0;
    int di_ = 0, diSeen_ = 0;
    int ctMin_ = -1, ctOff_ = 0;
    int grpCount_[32] = {0};
    int grpTotal_ = 0;
    // ── Confirmation by repetition ────────────────────────────────────────────
    // Burst correction buys extra blocks, but a MIS-correction produces a block that
    // looks valid and isn't — and a wrong station name on screen is worse than no name
    // at all. RDS repeats everything continuously, so nothing is committed until the
    // same value arrives twice: PI (present in every group) gates the whole group, and
    // each PS/RadioText segment gates itself. Costs one extra group (~87 ms) on first
    // lock and makes a single bad block invisible instead of visible.
    // ★ Confirmation is only charged where it is EARNED. A block whose checkword
    // matches exactly is already strongly verified, so it commits at once; only a block
    // that needed burst correction has to wait for a repeat. Taxing every update made
    // strong stations visibly sluggish to refresh RadioText for no safety gain, because
    // on a strong station nothing is being corrected in the first place.
    int  grpRepairBits_ = 0;          // bits repaired across the current group
    int  blkRepair_[4] = {0, 0, 0, 0};// ...and per block, so the parser can weigh
    uint16_t piLast_ = 0;   bool piSeen_ = false;
    uint16_t piConfirmedVal_ = 0;     // last PI seen twice running (0 = none)
    // BER window: one bit per block, 1 = arrived with a non-zero syndrome (pre-correction).
    static constexpr int kBerGroups = 12;                 // redsea's averaging window
    static constexpr int kBerBlocks = kBerGroups * 4;
    uint64_t errHist_ = 0;
    int errSeen_ = 0;
    uint16_t psCand_[4] = {0, 0, 0, 0};       bool psSeen_[4] = {false, false, false, false};
    uint32_t rtCand_[16] = {0};               bool rtSeen_[16] = {false};
    uint8_t  eccCand_ = 0;  bool eccSeen_ = false;
    Callbacks cb_{};
};

// ── RDS DSP front-end ────────────────────────────────────────────────────--
// Coherent 57 kHz demod of the MPX -> biphase symbol recovery -> differential
// decode -> data bits into an RdsDecoder. Uses the StereoPLL's coherent 57 kHz
// reference and pilot-locked bit clock (no separate timing loop). Original code.
class RdsDemod {
public:
    // Complex (I/Q) coherent downconvert of the 57 kHz subcarrier, a decimating
    // baseband filter, an early/late timing loop that tracks the symbol phase, and
    // differential detection — feeding ONE RdsDecoder.
    void configure(double mpxRate, const RdsDecoder::Callbacks& cb);
    void reset();
    using Callbacks = RdsDecoder::Callbacks;
    // Per-block: mpx samples + the PLL's coherent 57 kHz references (in-phase AND
    // quadrature) and the bit clock.
    void process(const float* mpx, const float* ref57, const float* ref57q,
                 const float* bitClk, int n);
    /** Block error rate of whichever hypothesis is currently winning; -1 = none synced.
     *  ★ -1 is itself the answer that matters: it means NOTHING is in block sync, which is
     *  a completely different fault from a high error rate, and the two were indistinguishable
     *  from outside until this existed. */
    int blockErrorPercent() const;
    /** ★★ RMS of the recovered 57 kHz baseband, relative to the pilot's own lock
     *  amplitude, in dB. THE decisive measurement: it separates "the subcarrier is not
     *  reaching us" from "it is reaching us and we are wasting it" — opposite faults with
     *  opposite fixes, and every other reading we have looks identical for both.
     *  RDS is injected at ~2-4% of MPX deviation against the pilot's ~8-10%, so a healthy
     *  station should land very roughly -6 to -12 dB below the pilot. Far below that means
     *  the subcarrier is being lost UPSTREAM — channel filter, deviation, or the 8-bit
     *  dongle's quantisation floor, which is a real candidate: at 2-4% injection the RDS
     *  subcarrier sits ~30 dB down, and an 8-bit ADC only gives ~48 dB of range, so the
     *  audio can sound perfect while RDS drowns in quantisation noise (2026-07-26). */
    float subcarrierRelDb() const;
    /** Pilot amplitude at the same instant, so the ratio means something. */
    void setPilotRef(float amp) { pilotRef_ = amp; }
    /** ★★ RECENT SYMBOL POINTS — the RDS constellation, as every serious FM receiver
     *  displays it. Each decoded symbol already produces a complex value in the
     *  differential detector; we simply kept throwing them away. Two tight clusters means
     *  a healthy subcarrier, a diffuse cloud means it is buried in noise — which is the
     *  distinction that took an entire evening and three separate instruments to establish
     *  on 2026-07-26, and which this shows at a glance.
     *  Copies up to `maxPts` normalised x,y pairs; returns how many were written. */
    int constellation(float* xy, int maxPts) const;
    // ★ 256 symbols (~220 ms), not 64. At 64 the plot was SPARSE — sparse reads as noisy
    // whatever the signal is doing, because you are seeing a small sample rather than a
    // distribution. More points do not change what the plot MEANS; they make what it means
    // visible (Stuart's comparison against SDR++ Brown, 2026-07-26).
    static constexpr int kConstPts = 256;
    /** Extended fields from whichever hypothesis is winning; -1 / 0 when none is. */
    const RdsDecoder* best() const;
private:
    std::unique_ptr<RealFir> lpfI_, lpfQ_;  // complex RDS baseband (decimating)
    double groupDelayPhase_ = 0.0;     // LPF delay expressed in bit-clock phase
    // RDS baseband is +/-2.4 kHz but arrives at the CHANNEL rate (~300 kHz) — over a
    // hundred times oversampled. The LPF decimates by this, so both it and the biphase
    // loop below run at ~40 kHz instead. bclk_ is the bit clock subsampled to match.
    int   decim_ = 1;
    int   bphase_ = 1;                 // mirrors RealFir's decimation phase
    std::vector<float> bclk_;

    // ── Why the phase hypotheses stayed ──────────────────────────────────────
    // The bit clock is FREQUENCY-accurate (the pilot divided by 16) but its symbol
    // boundary PHASE is unknown, so NPH integrators run at NPH fixed phases and only the
    // aligned one achieves block sync. That looks wasteful, and on 2026-07-25 it was
    // replaced by three early/late gates driving a timing loop and a single decoder. It
    // measured beautifully on synthetic signals and was a DISASTER on air: a strong
    // station took ~30 SECONDS to produce a name instead of being immediate.
    //
    // ★ The brute force has a virtue worth more than the cycles: ZERO ACQUISITION TIME.
    // One hypothesis is always already aligned — no loop to pull in, no transient, and
    // nothing to re-acquire after a fade. A timing loop must find the phase before the
    // first bit is right, and every slip while it hunts breaks the differential chain and
    // costs block sync. Do not "optimise" this away again without measuring TIME TO FIRST
    // NAME on a real station: the synthetic tests cannot see this failure at all.
    //
    // What DID survive that attempt, because it is a real win: detection is now complex
    // (I/Q) and DIFFERENTIAL — the decision is Re{A_k * conj(A_(k-1))}, so the carrier
    // phase cancels algebraically. The old real-only detector scaled by cos(theta) against
    // the station's actual subcarrier phase and died completely near 90 degrees. Measured
    // in rds_snr_probe: total failure at 80-90 degrees where this is flat. That, not noise,
    // is why some perfectly strong stations produced nothing.
    // ── Why there are sixteen, and why a timing loop cannot help ─────────────
    // ★★ The bit clock is the PILOT DIVIDED BY 16 (19000/16 = 1187.5 exactly, see
    // stereo.cpp). Its FREQUENCY is therefore perfect — locked, through the pilot, to the
    // transmitter's own clock — and the only unknown is the divider's starting state,
    // which has exactly SIXTEEN possible values. So this bank is not a brute-force stand-in
    // for symbol timing recovery: it is an exhaustive enumeration of a discrete ambiguity,
    // and precisely one hypothesis is EXACTLY right rather than approximately right.
    //
    // ★★ That is why a timing loop has now failed TWICE. 2026-07-25 replaced the bank with
    // early/late gates: it measured well synthetically and took ~30 s to name a strong
    // station on air, because a loop must pull in while an exact clock never has to.
    // 2026-07-26 kept the bank for acquisition and seeded a tracked loop from it, removing
    // the pull-in objection entirely — and the probe still said 23 groups against the
    // bank's 35. Both attempts substituted an ESTIMATE for a value that was already EXACT.
    // There is no sampling instant better than the right one, so there is nothing here for
    // a loop to win, at any bandwidth, with any seeding. Do not try a third time.
    //
    // ★ What the pilot does NOT give us is the SUBCARRIER's phase — 19 kHz and 57 kHz take
    // different group delays through a multipath channel. That is the open problem, and it
    // is a CARRIER problem, not a clock one.
    static constexpr int NPH = 16;
    // Per-hypothesis callback trampoline: carries which hypothesis spoke, so the demod
    // can drop everything except the winner.
    struct Slot { RdsDemod* self; int idx; };
    Slot slots_[NPH] = {};
    Callbacks user_{};                 // where the winner's output actually goes
    int bestIdx() const;
    float accI_[NPH] = {0};
    float accQ_[NPH] = {0};
    float prevPh_[NPH] = {0};
    float prevAI_[NPH] = {0};
    float prevAQ_[NPH] = {0};
    bool  havePrev_[NPH] = {false};
    bool  started_ = false;
    RdsDecoder dec_[NPH];
    std::vector<float> xI_, xQ_, sI_, sQ_;
    float rdsRms_ = 0.0f;              // smoothed |baseband|, for subcarrierRelDb()
    // Constellation ring — written by whichever hypothesis is currently winning, so the
    // plot shows what the DECODER is actually working with rather than an also-ran.
    float constXY_[kConstPts * 2] = {0};
    int   constHead_ = 0;
    int   constBest_ = -1;             // refreshed once per process(), not per sample
    float pilotRef_ = 0.0f;            // pilot lock amplitude at the same instant
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
        // ★ Optional: the station's PI code ALONE, the moment it is confirmed.
        // PI used to reach the host only as a parameter of rdsPs, so a station whose
        // name had not yet assembled reported NO IDENTITY AT ALL — even though its PI
        // had been arriving, error-protected, eleven times a second. On a weak signal
        // that is the difference between "C06F, Pride Radio, 11 km" and a blank box.
        void (*rdsPi)(void* ctx, uint16_t pi) = nullptr;
        // Optional: RDS block error rate, 0-100 (-1 = not enough data yet).
        void (*rdsBer)(void* ctx, int percent) = nullptr;
        /** Recovered 57 kHz level relative to the pilot, dB. See subcarrierRelDb(). */
        void (*rdsSig)(void* ctx, float relDb) = nullptr;
        // ★ The Advanced RDS decoder's payload: the fields we used to discard, plus the
        // constellation. Only emitted when a client has the decoder OPEN — selecting it IS
        // the toggle, so nothing here is paid for while nobody is looking.
        void (*rdsExt)(void* ctx, int pty, int tp, int ta, int ms, int di,
                       int ctMinutes, int ctOffsetHalfHours,
                       const int* afKhz, int nAf,
                       const int* groupCounts, int groupTotal,
                       const float* constXY, int nPts) = nullptr;
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
    std::vector<float> ref57Buf_, ref57qBuf_, bitClkBuf_;
    int chDecim_ = 1;
    double chFs_ = 0.0;
    // WFM only: the rate the stereo audio post-chain runs at, = chFs_/audioDecim_.
    // The 15 kHz filters decimate as they filter, so everything after them (blend,
    // de-emphasis, resampling) costs a fraction of what it did at the channel rate.
    double audFs_ = 0.0;
    int    audioDecim_ = 1;
    std::vector<cf32> baseBuf_, chBuf_;
    std::vector<float> demodBuf_, lpfBuf_, audioBuf_;
};

} // namespace vibedsp
