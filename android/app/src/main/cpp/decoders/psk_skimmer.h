// decoders/psk_skimmer.h — PSK31 / PSK63 (BPSK, Varicode): ONE transform over a band, a demodulator only where a
// carrier is — the listener's PSK decoder and a skimmer, from one light engine (V12, 2026-10-26).
//
// ★★ BUILT FROM THE PUBLISHED DESCRIPTION (G3PLX's PSK31 papers): 31.25 baud (PSK63: 62.5), binary phase shift keying
//    where a ZERO is a phase reversal and a ONE is no change; each reversal shaped by a cosine envelope so the amplitude
//    passes through zero (the raised-cosine pulse, two symbols long — the transmitted spectrum is the pulse's); the
//    Varicode alphabet (a character is a code with no two zeros running, characters are separated by "00", idle is
//    continuous zeros = continuous reversals). No other decoder's source was read.
//
// ★★★ THE COST IS DESIGNED IN (Stuart, 2026-10-25, on CW: "make it lightweight by using SIMD NEON SSE2"):
//       • ONE forward FFT per hop over the whole input (PFFFT), ≈ 2.9 Hz bins whatever the rate. It is BOTH the
//         carrier finder (every bin's power and averages — vector loops) AND the channeliser: each signal's channel is
//         128 of those bins, masked and inverse-transformed (overlap-save) — one 128-point inverse per signal per hop,
//         ≈ 375 complex samples a second out. No mixer or filter runs at the input rate for any signal.
//       • Per signal, at ≈ 375 Hz: a residual NCO (the AFC), the matched filter (the transmitted pulse itself, 2
//         symbols of Hann), symbol timing from the envelope, differential detection, Varicode. NEON/SSE2/scalar kernels
//         for the mask, the NCO and the matched filter.
//       • A tentative carrier gets two LANES — PSK31 and PSK63 demodulators — and the one whose constellation is clean
//         wins; a PSK63 signal read as PSK31 (or the reverse) is never clean.
//
// ── Text you can trust: zero garbage on noise ──────────────────────────────────────────────────────────────────────
//   Noise demodulated as BPSK decodes to Varicode just as happily (short codes: e, t, o, a, space). So text is gated
//   on a DECODED-TEXT quality, not a spectrum SNR: every symbol's distance from the ideal constellation (its EVM, as an
//   Es/N0), a character is committed only after it has been followed by clean symbols too (a signal that stops leaves
//   its last noise characters uncommitted), and a signal is shown only once its characters are plausible text at a
//   real Es/N0. Text held while tentative is sent on confirmation.
//
// ★ Nothing here allocates per hop, takes a lock or knows about sockets: the caller (vibe_psk_run.h) owns threads.
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace psk {

/** One callsign heard identifying itself. */
struct Spot {
    std::string call;
    double offsetHz = 0;   ///< from the segment's centre (IQ) or the audio frequency (real input)
    float  snrDb = 0;      ///< Es/N0 — signal over noise in the symbol rate's bandwidth (see SignalView::snrDb)
    std::string how;       ///< "CQ", "TEST" or "DE"
    std::string mode;      ///< "BPSK31" / "BPSK63"
    int64_t utcMs = 0;
    int     signalId = 0;
};

/** One signal, as a list row. */
struct SignalView {
    int     id = 0;
    double  offsetHz = 0;
    int     baud = 31;     ///< 31 (PSK31) or 63 (PSK63)
    float   snrDb = 0;     ///< ★ Es/N0 from the constellation (signal over noise in a bandwidth equal to the baud rate);
                           ///<   PSK31 copies cleanly from ≈ +7 dB, which is ≈ −12 dB in 2.5 kHz (subtract 19 dB; PSK63 16)
    float   imdDb = 0;     ///< ★ IMD as G3PLX defines it (the 3rd-order products of the idle's two tones over the tones,
                           ///<   e.g. −28 dB clean, −15 dB splattering); 0 = not measured yet (needs a clean idle)
    float   quality = 0;   ///< 0..1 (from Es/N0)
    int64_t firstMs = 0, lastMs = 0;
    std::string call;
    std::string text;      ///< the most recent decoded text (bounded; newlines kept)
    bool    active = false;
    unsigned chars = 0;
};

/** Quality words the clients show (high / medium / low / poor) — the health pill's four. */
const char* qualityWord(float q);

class Skimmer {
public:
    struct Config {
        double rate = 12000;          ///< input sample rate
        bool   complexIn = false;     ///< IQ (centre = 0 Hz) or real audio
        double loHz = 200, hiHz = 3600;   ///< the offsets searched (IQ: e.g. -6000..+6000)
        int    maxSignals = 32;       ///< channels at once (tentative ones included)
        bool   psk31 = true, psk63 = true;
        int    keepGoneSec = 600;     ///< how long a signal that has gone stays in signals()
        int    respotSec = 600;
        int    spotRepeats = 2;
        /** A carrier is tried when the band's power over ±18 Hz stands this far over the floor. ★ The tests lower it to
         *  make channels on pure noise and prove the text gate alone keeps them silent. */
        double candidateDb = 3.0;
    };
    explicit Skimmer(const Config& c);
    ~Skimmer();
    Skimmer(const Skimmer&) = delete;
    Skimmer& operator=(const Skimmer&) = delete;

    void feedReal(const float* x, int n);
    void feedIq(const std::complex<float>* x, int n);

    /** Decoded text as it is committed, per CONFIRMED signal. */
    std::function<void(int id, double offsetHz, const std::string& text)> onText;
    std::function<void(const Spot&)> onSpot;

    /** Confirmed signals: active first (lowest offset first), then the recently gone (newest first). */
    std::vector<SignalView> signals(bool includeGone = true) const;
    bool signal(int id, SignalView& out) const;

    void setStartUtcMs(int64_t ms);
    int64_t nowMs() const;

    int    fftSize() const;
    double binHz() const;
    double channelRate() const;     ///< each signal's channel, Hz (≈ 375)
    int    binsSearched() const;
    uint64_t hops() const;
    int    tracked() const;         ///< tentative + confirmed channels
    int    confirmed() const;       ///< active confirmed signals
    uint64_t tentativesDropped() const;
    uint64_t charsSuppressed() const;   ///< decoded but never shown (failed the text-quality gate)
    /** The floor's calibration on pure noise: mean (slow average ÷ floor) over the searched bins — ≈ 1.0 when right. */
    double noiseCalibration() const;
    static const char* simdName();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/** ★ Varicode (G3PLX): the code for an ASCII character (0–127) as '0'/'1' text, or "" — for the tests' generator. */
const char* varicode(int ascii);
/** The character for a code (its bits as an integer — the leading 1 makes it unique), or -1. */
int varicodeChar(uint32_t code);

} // namespace psk
} // namespace vibe
