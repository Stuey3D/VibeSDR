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
#include <string>

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
    /** ★★ PILOT DEVIATION IN kHz — the number a broadcast analyser shows, and the one an
     *  FM-DXer already reads fluently. The MPX is scaled so ±1 is ±75 kHz (see the FmDemod
     *  gain in pipeline.cpp), so this is simply the pilot's amplitude in those units.
     *  Spec is 8-10% of 75 kHz, i.e. 6.0-7.5 kHz — and the lock threshold's own comment,
     *  written long before this, notes a real pilot measures ~0.08, which is 6 kHz. The
     *  calibration was already right; we had just never expressed it in the units the
     *  industry uses (2026-07-26, prompted by a Pira reading 6.8 kHz). */
    float pilotDeviationKHz() const { return std::fabs(lockAmp_) * 75.0f; }
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
    /** ★ THE SAME FIELDS BEFORE CONFIRMATION — what was last seen on air, accepted or not.
     *  For the client's RAW view: the flicker is evidence about the signal path, so it is
     *  offered rather than hidden. Never mix these into the sticky aggregate. */
    int  ptyRaw() const { return ptyRaw_; }
    int  tpRaw()  const { return tpRaw_; }
    int  taRaw()  const { return taRaw_; }
    int  msRaw()  const { return msRaw_; }
    int  diRaw()  const { return diSeenRaw_ == 0xF ? diRaw_ : -1; }
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
    // ── ★★ RT+ and LONG PS — asked for directly by the FM-DX community ────────
    // Both ride on the 57 kHz subcarrier; neither needs any network access.
    /** RT+ tagging (ODA 4BD7, announced in group 3A): the artist and title carved out of
     *  RadioText by start/length pointers. This is what turns a scrolling message into
     *  now-playing metadata — and it feeds the OS media card we already publish. */
    const char* rtPlusTitle()  const { return rtpTitle_; }
    const char* rtPlusArtist() const { return rtpArtist_; }
    /** Long PS — 32 UTF-8 characters in group 15A, where ordinary PS is 8. Stations use it
     *  to send a full name instead of an abbreviation squeezed into eight slots. */
    const char* longPs() const { return lpsSeen_ ? longPs_ : ""; }
    // ── ★ The rest of the standard, so nothing a station transmits is thrown away ──
    /** PTYN (10A) — the station's OWN words for its programme type ("Rock Show" rather
     *  than the generic "Pop Music"), 8 characters. */
    const char* ptyn() const { return ptynSeen_ == 0x3 ? ptyn_ : ""; }
    /** Language code (1A, slow labelling variant 3). 0 = not received. */
    int  languageCode() const { return lang_; }
    /** PIN (1A block D) — programme item number: day of month, hour, minute. */
    int  pinDay() const { return pinDay_; }
    int  pinHour() const { return pinHour_; }
    int  pinMinute() const { return pinMin_; }
    /** ★★ EON (14A) — OTHER NETWORKS: what the sister stations are called, where they are,
     *  and whether one of them has a traffic announcement running. BBC Northampton spends
     *  30% of its groups on this and we were binning every one. */
    struct Eon { uint16_t pi; char ps[9]; int afKhz; int ta; };
    /** ★ Which ODAs (Open Data Applications) this station announces in 3A, and in which
     *  group each rides. RT+ is only ONE of these — a station can announce eRT, TMC or an
     *  in-house application instead, so "3A present" does NOT mean "RT+ present". Listing
     *  them turns "why is Now Playing empty?" from a guess into a reading (2026-07-26). */
    struct Oda { uint16_t aid; uint8_t group; };
    int  odaCount() const { return odaN_; }
    const Oda& oda(int i) const { return oda_[i]; }
    static constexpr int kMaxOda = 8;
    int  eonCount() const { return eonN_; }
    const Eon& eon(int i) const { return eon_[i]; }
    static constexpr int kMaxEon = 8;
    int  afCount() const { return afN_; }
    int  afKhz(int i) const { return (i >= 0 && i < afN_) ? afKhz_[i] : 0; }
    int  afHits(int i) const { return (i >= 0 && i < afN_) ? afHits_[i] : 0; }
    /** Sightings before an AF is believed. The list repeats endlessly, so a genuine entry
     *  returns within seconds while a mis-corrected block's invention does not. */
    static constexpr int kAfConfirm = 2;
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
    /** TA/MS/DI out of block B, confirmed by the same rule as PTY and TP. `addr` is the
     *  segment address, which selects WHICH DI bit this group carries. */
    void acceptBlockBFlags(int addr);
    void applyRtPlus(int type, int start, int len);
    void endRadioTextAtCr();
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
    // Confirmation state for the block-B fields — see the note in parseGroup.
    // ★★ TA, MS and DI ride in the SAME block B and were accepted unconditionally long after
    // PTY and TP were fixed. That is where "Artificial head · Compressed" came from on a
    // station at 87% block errors (Hans/Stuart, 2026-07-27) — DI is four single bits, so one
    // bad group flips a flag and the sticky aggregate keeps it forever.
    int ptyCand_ = -1, tpCand_ = -1, taCand_ = -1, msCand_ = -1;
    int diCandBit_[4] = { -1, -1, -1, -1 };   // per SEGMENT ADDRESS, not per DI bit number
    bool ptySeen_ = false, trustedB_ = false;
    // ★★ THE UNCONFIRMED READING, KEPT DELIBERATELY. Confirmation happens here in the decoder,
    // so a rejected value is normally discarded and no amount of UI could show it. A DXer
    // working a marginal signal wants to SEE that flicker — it is evidence about the path, not
    // noise to be hidden — so every confirmed field keeps its last raw sighting alongside.
    // ★ These are LIVE, not sticky: raw is "what just arrived", and its coming and going is
    // the whole point. The confirmed values remain the sticky ones.
    // ★ Both are always published; choosing between them is entirely the CLIENT's business, so
    // one listener switching to raw cannot affect what anyone else on a shared receiver sees.
    int ptyRaw_ = -1, tpRaw_ = -1, taRaw_ = -1, msRaw_ = -1;
    int diRaw_ = 0, diSeenRaw_ = 0;
    // ★ CONFIRMATION BY REPETITION, as everything else here uses. An AF that arrived once
    // could be a mis-corrected block; the list repeats endlessly, so a real one comes back
    // within seconds. Nothing is published until it has been seen kAfConfirm times.
    int afKhz_[kMaxAf] = {0};
    int afHits_[kMaxAf] = {0};
    int afN_ = 0;
    int di_ = 0, diSeen_ = 0;
    // ★ The local-time OFFSET is constant for a station, so unlike the time itself it can
    // be confirmed by repetition. See the note in parseGroup's 4A branch.
    int ctOffCand_ = 0; bool ctOffSeen_ = false;
    // RT+ : group 3A names which group type carries the tags (commonly 11A), so we cannot
    // parse them until that announcement arrives. -1 = not yet announced.
    int  rtpGroup_ = -1;
    int  rtAb_ = 0; bool rtAbSeen_ = false;   // RadioText A/B flag — see parseGroup
    // UTF-8 renderings handed to callers — RDS G0 never escapes this class.
    std::string psU8_, rtU8_;
    char ptyn_[9] = {0};
    int  ptynSeen_ = 0;
    int  lang_ = 0;
    int  pinDay_ = 0, pinHour_ = -1, pinMin_ = 0;
    Oda  oda_[kMaxOda] = {};
    int  odaN_ = 0;
    Eon  eon_[kMaxEon] = {};
    int  eonN_ = 0;
    Eon* eonFor(uint16_t pi);
    char rtpTitle_[65] = {0};
    char rtpArtist_[65] = {0};
    char longPs_[33] = {0};
    int  lpsSeen_ = 0;          // bitmask of the 8 segments received
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
    /** ★ RDS DEVIATION IN kHz, on the same ±1 = ±75 kHz scale as the pilot. Typical is
     *  2-4 kHz with 7.5% (5.6 kHz) the ceiling — a station pushing 4.9 is being generous,
     *  which is good for reception and worth being able to see.
     *  ★ Derived from the mean envelope of the recovered baseband and scaled to a peak by a
     *  SIMULATED crest factor, so treat it as indicative rather than calibrated: an analyser
     *  measures the deviation directly, we infer it after filtering. Known to read ~1.3 dB
     *  low against a Pira analyser — see rdsDeviationKHz() in rds.cpp for why that residual
     *  is believed to be a real signal-path loss rather than a scaling error. */
    float rdsDeviationKHz() const;
    /** ★ Turn on the guard-band noise measurement. Costs a second decimating filter pair on the
     *  RDS front end, so it is the operator's call — see the note on guardPow_. Without it the
     *  deviation figure is reported uncorrected and can read high on a weak signal. */
    void setNoiseCorrection(bool on);
    bool noiseCorrection() const { return guardOn_; }
    /** ★★★ RDS-TO-PILOT PHASE, in degrees — the measurement HansVanEijsden (FMDX.org,
     *  2026-07-26) called "one verrrrry much requested thing… as far as I know, no software
     *  solution yet", and carries a Pira broadcast analyser to get. A correctly encoded
     *  station sits near 0 degrees, or near 90 for quadrature encoding; anything between is
     *  a transmitter fault worth knowing about.
     *  ★ We were already measuring it and throwing it away. Our 57 kHz reference is the
     *  PILOT tripled, so the angle the constellation sits at IS the phase between the
     *  station's subcarrier and its own pilot. It was removed an hour earlier purely to make
     *  the plot look conventional.
     *  ★ BPSK is 180-degree ambiguous, so the raw estimate is modulo 180 — but this returns
     *  it folded into [0,90], an unsigned angular DISTANCE, which is what an analyser reports
     *  and what "8 degrees out" means. ★★ It used to return the raw [0,180) value, so every
     *  station read as its own reflection (8->172, 45->131); it looked right only because the
     *  0 and 90 cases are the two the error cannot affect. See pilotPhaseDeg() in rds.cpp.
     *  Heavily smoothed: it is a transmitter characteristic, not something that should
     *  flicker. -1 = no lock. */
    float pilotPhaseDeg() const;
    /** ★ Degrees per second the RDS-to-pilot phase is turning. Our 57 kHz reference IS the
     *  station's own pilot tripled, so a locked encoder sits still no matter how weak the
     *  signal — a steady march means the station's subcarrier is genuinely not 3x its pilot,
     *  which is a transmitter fault and not a reception one. -1 = not measurable. */
    float pilotPhaseDriftDegPerSec() const { return phDriftDeg_; }
private:
    void measurePhaseDrift();
public:
    /** ★★ HOW COHERENT that phase estimate is, 0..1 — and the reason the number above must
     *  never be shown without it. The estimate averages a unit vector at twice the symbol
     *  angle: if the phase is STEADY the vectors agree and the average keeps its length, but
     *  if our 57 kHz reference has even a slight frequency error the angle ROTATES, the
     *  vectors cancel, and the average collapses towards zero while still yielding a
     *  perfectly plausible-looking angle.
     *  ★ That is exactly what was observed on air: Classic FM cycling red/amber/green, Heart
     *  reading 45 then 27 degrees. A phase cannot be measured against a reference that is
     *  itself turning, and a confident wrong number is worse than no number — this one would
     *  have told broadcasters their transmitters were faulty (Stuart, 2026-07-26). */
    float pilotPhaseCoherence() const;
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

    // ★★ EVERY FIELD IS STICKY AND STATION-SCOPED, exactly as AF is.
    // Reading these straight off best() had the same fault the AF list had: each of the
    // sixteen hypotheses keeps its OWN copy, so an arbitration switch swapped the whole set
    // for a different receiver's partial knowledge, and fields that had been decoded
    // perfectly well flicked back to dashes. Anything a user watches must not blink out
    // because the decoder changed its mind about which timing phase it prefers.
    // So: accumulate above the bank, accept only values that are KNOWN, never overwrite
    // something known with something unknown, and clear the lot only when the PI changes —
    // because that, and only that, means a different station (Stuart, 2026-07-26).
    struct Agg {
        int pty = -1, tp = -1, ta = -1, ms = -1, di = -1;
        int ctMinutes = -1, ctOffsetHalfHours = 0;
        int language = 0, pinDay = 0, pinHour = -1, pinMinute = 0;
        char ptyn[9] = {0};
        char rtpTitle[65] = {0};
        char rtpArtist[65] = {0};
        char longPs[33] = {0};
        // ★ Group counts belong here too. Taken from the winner they JUMP whenever
        // arbitration switches — and a rate computed from successive totals read that jump
        // as throughput, reporting 16.7 groups/sec against a physical maximum of 11.4
        // (Stuart, 2026-07-26). Monotonic per station, like everything else here.
        int groupCounts[32] = {0};
        int groupTotal = 0;
    };
    const Agg& aggregate() const { return agg_; }
    /** Merged EON and ODA, same stickiness. */
    int mergedEon(RdsDecoder::Eon* out, int maxOut);
    int mergedOda(RdsDecoder::Oda* out, int maxOut);
    /** ★★ AF merged ACROSS hypotheses. Each of the sixteen has its own decoder and its own
     *  partial list, so when arbitration switched winner the reported AFs visibly vanished
     *  and reappeared — nothing was forgotten, we were simply reading a different receiver's
     *  notes (Stuart, on air 2026-07-26: "some kept arriving then disappearing"). Merging
     *  above the bank makes the list monotonic within a station, and only a retune or a PI
     *  change clears it. Entries appear only once CONFIRMED by repetition.  */
    int mergedAf(int* khzOut, int maxOut, int* seenOut = nullptr);
private:
    std::unique_ptr<RealFir> lpfI_, lpfQ_;  // complex RDS baseband (decimating)
    double groupDelayPhase_ = 0.0;     // LPF delay expressed in bit-clock phase
    double mpxRate_ = 0.0;             // channel rate, for the sample-derived clock
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
    // ★★★ NOISE-SUBTRACTED DEVIATION. The ±2.4 kHz RDS baseband is never empty: with no
    // subcarrier it still holds NOISE, and scaling that into a deviation produced confident
    // nonsense — 12.9 kHz on a dead carrier and 10.5 kHz on a weak one, both past the 5.6 kHz
    // spec ceiling, and the reading went UP as the signal got WORSE (Stuart, 2026-07-27).
    // An estimate that rises as the station weakens is measuring the wrong thing.
    // ★ So measure a GUARD BAND too — the same filter, offset clear of the subcarrier — and
    // subtract it in POWER. What is left is the subcarrier alone, and it now degrades toward
    // zero on a weak signal instead of inflating.
    // ★ POWER, hence mean-square rather than mean-envelope: noise adds and removes as power,
    // and only a mean-square is one. The peak/RMS crest factor differs accordingly (1.381 vs
    // the 1.520 peak/mean-envelope) — both measured by tools/rdsdev_cal.
    float rdsPow_ = 0.0f;              // smoothed mean-square of the RDS baseband
    float guardPow_ = 0.0f;            // ...and of the guard band beside it
    float sigPowSlow_ = 0.0f;          // (rds - guard), smoothed over SECONDS
    double guardPhase_ = 0.0;          // guard NCO phase, carried across blocks
    double guardStep_ = 0.0;           // radians per sample for the guard offset
    bool  guardOn_ = false;            // costs a second filter pair — operator opt-in
    std::unique_ptr<RealFir> lpfGI_, lpfGQ_;
    std::vector<float> xGI_, xGQ_, sGI_, sGQ_;
    // Doubled-angle accumulator: doubling folds the two BPSK lobes onto one, so they can be
    // averaged without cancelling. Slow, because this describes the transmitter.
    float phCos2_ = 0.0f, phSin2_ = 0.0f;
    // ★★★ HOW FAST THE PHASE IS TURNING, deg/s. Coherence alone cannot answer this: it
    // collapses only when rotation is FAST relative to the averaging window, so a slowly
    // rotating station keeps a high "steady" figure while the angle marches all the way round
    // underneath it. Both are the same transmitter fault seen at two speeds — Classic FM
    // rotates fast (constellation draws a CIRCLE, coherence dies, already detected) and
    // Harborough FM rotates slowly (67% steady, phase walking 0->82 deg in six seconds, and
    // the old detector said nothing). Stuart caught the slow one on air, 2026-07-27.
    float phDriftDeg_ = 0.0f;      // smoothed |drift|, degrees per second
    float phLastDeg_  = -1.0f;     // previous sample of the RAW (unfolded) angle
    double phLastAt_  = 0.0;       // seconds, for dt
    double phClock_   = 0.0;       // sample-derived clock; no wall time in the DSP layer
    // Constellation ring — written by whichever hypothesis is currently winning, so the
    // plot shows what the DECODER is actually working with rather than an also-ran.
    Agg agg_{};
    RdsDecoder::Eon aggEon_[RdsDecoder::kMaxEon] = {};
    int aggEonN_ = 0;
    RdsDecoder::Oda aggOda_[RdsDecoder::kMaxOda] = {};
    int aggOdaN_ = 0;
    void updateAggregate();
    int mergedAf_[RdsDecoder::kMaxAf] = {0};
    int mergedAfN_ = 0;
    uint16_t mergedAfPi_ = 0;          // whose list this is; a new PI starts a new list
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
    /** MPX spectrum geometry — DC to 100 kHz, which covers L+R, pilot, L-R and RDS with a
     *  little room above for anything unusual a station is carrying. */
    static constexpr int    kMpxFft  = 1024;
    static constexpr int    kMpxBins = 128;
    static constexpr double kMpxSpanHz = 100000.0;

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
        // ★ A STRUCT, not a parameter list. It reached fifteen arguments and every new RDS
        // field meant editing four signatures in three files — the kind of friction that
        // quietly argues against adding the next field, which is the opposite of what this
        // work needs (2026-07-26).
        struct RdsExt {
            int pty, tp, ta, ms, di;
            /** ★★ THE SAME FIELDS UNCONFIRMED — the client's RAW view. LIVE, not sticky: these
             *  are the last thing seen on air whether or not it passed confirmation, so they
             *  flicker on a marginal signal, which is exactly the diagnostic. Both sets are
             *  always sent; which to show is purely the client's choice, so one listener in
             *  RAW cannot change what anyone else on a shared receiver sees.
             *  ★ -1 = nothing seen at all yet, same convention as the confirmed fields. */
            int ptyRaw, tpRaw, taRaw, msRaw, diRaw;
            int ctMinutes, ctOffsetHalfHours;
            const int* afKhz; int nAf; int afSeen;
            const int* groupCounts; int groupTotal;
            const char* rtpTitle; const char* rtpArtist; const char* longPs;
            const char* ptyn; int language;
            int pinDay, pinHour, pinMinute;
            const RdsDecoder::Eon* eon; int nEon;
            const RdsDecoder::Oda* oda; int nOda;
            const float* constXY; int nPts;
            float pilotPhaseDeg;
            float pilotPhaseCoherence;
            float pilotPhaseDriftDegPerSec;   // >0 = the phase is turning; see the note on it
            float pilotDevKHz;      // pilot injection, kHz deviation
            float rdsDevKHz;        // RDS injection, kHz deviation
            /** ★★ THE MPX SPECTRUM, 0-100 kHz — the view SDRconnect calls "MPX SP" and the
             *  most analyser-like display there is: L+R at the bottom, the 19 kHz pilot, the
             *  L-R sidebands around 38 kHz, RDS at 57 kHz, and anything else a station is
             *  carrying up there. We already compute the MPX for the stereo and RDS
             *  decoders, so this is one FFT away (Stuart, 2026-07-26).
             *  dB, one entry per bin, 0 Hz to kMpxSpanHz. */
            const float* mpx; int nMpx;
        };
        void (*rdsExt)(void* ctx, const RdsExt& x) = nullptr;
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
    /** ★ Turn on the RDS guard-band noise measurement — the second filter pair that makes the
     *  DEVIATION READOUT honest on a weak signal. Costs CPU, and buys nothing but accuracy of a
     *  number, so it is driven by whether anyone has the analyser OPEN rather than by a setting.
     *  ★ It does NOT touch the channel filter. Widening that was tried and measured to cost
     *  10 dB of RDS SNR — see the chHalf note in pipeline.cpp before considering it again. */
    void setRdsNoiseCorrection(bool on) { rdsNoiseCorr_ = on; dirty_ = true; }
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
    std::atomic<bool>   rdsNoiseCorr_{false};  // guard-band deviation correction only
    std::atomic<double> deempTau_{50e-6};    // FM de-emphasis tau (0=off / 50us / 75us)
    // WFM RDS
    RdsDemod rdsDemod_;
    std::vector<float> ref57Buf_, ref57qBuf_, bitClkBuf_;
    int chDecim_ = 1;
    double chFs_ = 0.0;
    // MPX spectrum for the Advanced RDS panel. Only computed while somebody is looking at
    // it — an extra FFT per block otherwise buys nothing.
    std::unique_ptr<RealFFT> mpxFft_;
    std::vector<float> mpxWin_, mpxIn_, mpxDb_, mpxOut_;
    std::vector<float> mpxAcc_;      // fills across blocks — see the note in pipeline.cpp
    int mpxAccN_ = 0;
    // WFM only: the rate the stereo audio post-chain runs at, = chFs_/audioDecim_.
    // The 15 kHz filters decimate as they filter, so everything after them (blend,
    // de-emphasis, resampling) costs a fraction of what it did at the channel rate.
    double audFs_ = 0.0;
    int    audioDecim_ = 1;
    std::vector<cf32> baseBuf_, chBuf_;
    std::vector<float> demodBuf_, lpfBuf_, audioBuf_;
};

} // namespace vibedsp
