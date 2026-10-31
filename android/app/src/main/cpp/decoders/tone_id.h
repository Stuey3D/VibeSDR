// decoders/tone_id.h — CTCSS / DCS TONE IDENTIFICATION on NFM: "what tone is this repeater?" (V12, 2026-10-31).
//
// ★★★ WHY IT HAS ITS OWN DISCRIMINATOR. The decoder host's NFM audio is post-voice-filter: in NFM VOICE the 300 Hz
//     high-pass (vibedsp setNfmVoice) takes CTCSS down by ≥ 40 dB — exactly the band this reads. So ToneId takes the
//     listener's CHANNEL IQ (the per-client slice the demodulator is fed, the same one the raw-IQ tap splits off) and
//     runs its OWN FM discriminator; or, where only a discriminator output exists, the raw discriminator (feedDisc).
//     It never touches RxPipeline.
//
// ★★ HOW.
//   • Discriminator: arg(x[n]·conj(x[n−1])) — vmath4.h's branch-free atan2, four samples a time (NEON / SSE2 / the
//     same lanes in plain C) — in Hz.
//   • Down to ≈ 1 kHz: a 3-stage CIC to ≈ 2 kHz, then a 63-tap low-pass (300 Hz) and ÷2. Voice (300 Hz – 3 kHz) is
//     gone; CTCSS (67–254.1 Hz) and DCS (134.4 bit/s NRZ, its energy below ≈ 200 Hz) remain.
//   • CTCSS: every 0.5 s a 2048-point FFT (≈ 2 s, Hann) of the last 2 s; the strongest bin in 60–260 Hz must stand
//     kCtcssSnrDb over the band's median and its frequency (log-parabola) within ±0.6 % of one of the 51 standard
//     tones; the same tone in three analyses running (1.5 s) LOCKS it; 2.5 s without it UNLOCKS.
//   • DCS: 134.4 bit/s NRZ: the low band's sign sampled at the best of 16 bit phases (integrate-and-dump), the bits
//     folded at the 23-bit word period over ≈ 2 s, then every cyclic shift, both bit orders and both polarities tried
//     against the Golay (23,12) code (generator x¹¹+x¹⁰+x⁶+x⁵+x⁴+x²+1, 0xC75) with the frame's fixed "100"; a code in
//     the standard list, twice running, LOCKS it.
//     ★ HOW THE DCS CODE WAS PINNED: the published DCS "inverted code" equivalences (023N = 047I, 025N = 244I, 026 =
//       464, 031 = 627, 032 = 051, 043 = 445, 054 = 413, 065 = 271, 071 = 306, 072 = 245, 073 = 506, 074 = 174 …)
//       were reproduced 12 of 12 ONLY by 0xC75 with the 12-bit word = the 9-bit code under "100" (scratch Python, not
//       shipped). The polarity convention (which is "N") is NOT pinned by that — both readings are reported
//       ("DCS 023N (= 047I)"): they are the same signal, and a radio set to either opens on it.
//   • Never on noise: a lock needs the tone's narrow peak 3 analyses running (CTCSS) or a valid Golay word twice
//     (DCS: 2⁻¹¹ a random word).
// ★ COST: the discriminator at the channel rate (≈ 4 lanes: a multiply, an atan2 polynomial) and the CIC; then 1 kHz
//   work. ≈ 0.2 ms a second of 24 kHz IQ on the Mac (tools: test-tone-id prints it).
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class ToneId {
public:
    /** `rateHz` = the IQ (or discriminator) sample rate. */
    explicit ToneId(double rateHz);
    ~ToneId();
    ToneId(const ToneId&) = delete;
    ToneId& operator=(const ToneId&) = delete;

    /** Channel IQ (complex baseband, the NFM carrier at 0 Hz). */
    void feedIq(const std::complex<float>* x, int n);
    /** A raw (unfiltered) FM discriminator output in Hz of deviation, at the same rate. */
    void feedDisc(const float* hz, int n);
    /** Forget everything (retune). */
    void reset();

    struct State {
        bool   ctcss = false;      ///< a CTCSS tone is locked
        float  ctcssHz = 0;        ///< the standard tone (88.5 …)
        float  measuredHz = 0;     ///< what was measured
        float  snrDb = 0;
        bool   dcs = false;        ///< a DCS code is locked
        int    dcsCode = 0;        ///< octal digits as a number: 023 → 23 (print with %03d)
        bool   dcsInverted = false;///< in our polarity (see the file note): the other reading is dcsAlt
        int    dcsAlt = 0;
        std::string label() const; ///< "CTCSS 88.5 Hz", "DCS 023N (= 047I)", "" = nothing
    };
    State state() const { return st_; }
    /** Called on every change of state() (lock, unlock, a different tone). */
    std::function<void(const State&)> onChange;

    /** The 50 standard CTCSS tones radios offer (EIA/TIA-603 and the common additions: 51). */
    static const std::vector<float>& ctcssTones();
    /** The standard DCS codes (octal digits as a number). */
    static const std::vector<int>& dcsCodes();
    /** DCS: the 23-bit word of a code (bit i = the i-th bit sent, LSB-first order, our polarity: 1 = +deviation). */
    static uint32_t dcsWord(int octalCode);

    double ms = 0;                 ///< processing time so far (the test reports it)
private:
    void lowBand(const float* d, int n);
    void analyseCtcss();
    void analyseDcs();
    void publish(const State& s);
    double rate_;
    int cicD_ = 1;
    double rate1_ = 1000;          // after the CIC and the ÷2
    std::complex<float> last_{1, 0};
    double i1_ = 0, i2_ = 0, i3_ = 0, c1_ = 0, c2_ = 0, c3_ = 0;   // CIC state
    int cicN_ = 0;
    std::vector<float> fir_, firHist_;
    int firPhase_ = 0;
    std::vector<float> low_;       // the ≈ 1 kHz band, a ring of the last 4 s
    int64_t lowTotal_ = 0, lastCtcss_ = 0, lastDcs_ = 0;
    struct Fft; std::unique_ptr<Fft> fft_;
    // CTCSS lock
    int candTone_ = -1, candRuns_ = 0, missRuns_ = 0;
    // DCS lock
    int candDcs_ = -1, candDcsRuns_ = 0, dcsMiss_ = 0;
    State st_;
    std::vector<float> disc_;
};

} // namespace vibe
