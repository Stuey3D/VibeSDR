// decoders/mt63.h — ★ V12 2026-11-19: MT63 (Pawel Jalocha SP9VRC's 64-carrier keyboard mode) — 500, 1000 and 2000 Hz,
// short and long interleave — on the listener's audio. A receiver inside the one text-mode key (textmodes.h, group
// "MT63"); its own spectra, since MT63 is 64 carriers at once, not one tone of a set.
//
// ★ CLEAN ROOM (docs/v12/HF-DATA.md, "MT63 — where every fact came from"). The format, as facts:
//   • 64 carriers, 4 FFT bins apart in a 512-point transform at the mode's own rate R = 2 × bandwidth (1000 / 2000 /
//     4000 Hz), so spacing = bandwidth / 64 (7.8125 / 15.625 / 31.25 Hz); symbols every 200 samples at R (5 / 10 / 20 Bd);
//     each symbol shaped by a 512-sample window (2.56 symbols, overlapping); the odd carriers half a symbol after the
//     even ones. The lowest carrier sits at 500 Hz audio when the signal is centred at 500 + bandwidth / 2.
//   • Differential BPSK on every carrier: a 1 keeps the carrier's phase, a 0 turns it by π.
//   • ONE CHARACTER A SYMBOL: its 7 bits pick one of 128 ± Walsh functions of 64 chips (code < 64: +W_code; else
//     −W_(code−64)); chip i goes to carrier i (a chip < 0 sends a 1).
//   • The interleaver: carrier i carries its chip d_i symbols after the character, d_i = (L − p_i) mod L, p_0 = 0,
//     p_(i+1) = (p_i + P_i) mod L — short: L = 32, P = 4,5,6,7,4,5,6,7…; long: L = 64, P = 1,2,3,…,63,0.
//   • Text: 7-bit ASCII; 127 escapes the next character (+128); codes under 8 are not shown.
//   These were READ from fldigi's MT63 source (GPL) as facts only — no line of it is in here; the symbol shape is OUR
//   OWN (14 cosine terms fitted by us to the properties the format needs: no ISI at the 200-sample step, a carrier
//   orthogonal to its neighbours half a symbol away), and settled against six public reference recordings.
//
// ★ THE RECEIVER: one real FFT (PFFFT) of the window every eighth of a symbol; the band's edges give the carriers'
//   place to a bin or two; seven neighbouring places are each read at the best of eight timings (where the carriers'
//   symbol-to-symbol products are strongest), with the common phase drift taken out (the mean of the squared
//   products), then de-interleaved both ways at once and every character's 64 chips Walsh-transformed: the strongest of
//   128 is the character, its share of the whole the evidence. AUTO = every bandwidth and both interleaves.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class Mt63Rx {
public:
    /** `bw` 500 / 1000 / 2000; `longIntlv` the first interleave enabled (enable() adds the other) */
    Mt63Rx(int bw, bool longIntlv, const char* label);
    ~Mt63Rx();
    void enable(bool longIntlv);   ///< also decode this interleave (AUTO runs both on one set of spectra)
    int bandwidth() const;
    void feed(const float* x8k, int n);   ///< 8 kHz audio
    bool locked() const;
    float quality() const;
    float hz() const;               ///< the signal's centre (audio Hz), 0 when none
    const char* label() const;      ///< "MT63 1000L" — the locked interleave
    std::string text;               ///< decoded since last cleared (the family decoder takes it)
    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

/** ★ Transmit side, for tests and benches only: `text` as MT63 at `bw` with the lowest carrier at `lowHz`, audio at `fs`.
 *  The interleaver starts full of random chips, as a real transmitter's does; `lead`/`tail` seconds of silence. */
std::vector<float> mt63Synth(int bw, bool longIntlv, const std::string& text, double fs, double lowHz = 500,
                             unsigned seed = 1);

/** our own MT63 symbol shape at x ∈ [−0.5, 0.5] (the 512-sample window's centre at 0), peak 1 */
double mt63Shape(double x);

} // namespace vibe
