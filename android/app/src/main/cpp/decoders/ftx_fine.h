// VibeSDR V12 — the FT8 / FT4 / FT2 decoder's SECOND LOOK at a candidate: measured again from the audio, symbol by
// symbol, instead of from the search waterfall.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vibe {

/* ★★★ WHY A SECOND LOOK (2026-10-26, the sensitivity work). ft8_lib finds candidates on a waterfall — a Hann-windowed
 *  STFT two symbols long, a half symbol and a half tone apart, stored as uint8 in half-dB steps — and decodes from the
 *  same numbers: each bit's likelihood is a difference of the strongest tones' LEVELS IN dB. That is a good search and
 *  a poor detector: the window is twice a symbol (it averages in the neighbours), the grid can be a quarter symbol
 *  and a quarter tone off, and a log-level is not a likelihood. The textbook detector for continuous-phase FSK is the
 *  MATCHED FILTER: each symbol correlated with each tone over exactly one symbol, at the right start and frequency —
 *  and because the tone spacing equals the symbol rate, every tone ends a symbol on a whole number of cycles, so the
 *  phase carries from one symbol to the next and two or three symbols can be detected TOGETHER (multiple-symbol
 *  noncoherent detection of CPFSK: the bits of a group judged on |Σ of the group's correlations|, which keeps the
 *  phase information a symbol-by-symbol detector throws away). The K1JT/K9AN/G4WJS QEX paper (2020) describes the
 *  same three steps for FT8/FT4: a downsampled baseband per candidate, fine time/frequency sync on the Costas
 *  arrays, and bit metrics over one, two and three symbols, each tried by the LDPC decoder in turn.
 *
 *  As built here:
 *   1. ONE real FFT of the whole window per pass (only when the first candidate needs it). A candidate's baseband is
 *      cut from that spectrum — a raised-cosine band from 1.5 tones below tone 0 to 0.5 above the top tone — and
 *      brought back by an inverse FFT 60× (FT8) shorter: an ideal band-pass and decimator in one, which a time-domain
 *      boxcar is not (it lets a neighbour 100 Hz away fold in at -4 to -13 dB).
 *   2. FINE SYNC on the Costas symbols: start ±(½ symbol + a bit) in 1/32-symbol steps, then frequency ±⅜ tone in
 *      1/16-tone steps, then the start again ±2.
 *   3. Each symbol's complex amplitude at each tone (the matched filter), phase-referenced to the slot, so groups add.
 *   4. EARLY OUT: a candidate whose Costas symbols are mostly not the strongest tone is noise — counted, nothing more
 *      spent on it (Stuart, 2026-10-26: "beat the competition on CPU").
 *   5. Bit log-likelihoods, max-log over |Σ group| for groups of 1, 2 and 3 symbols (FT4/FT2: 1, 2 and 4), each set
 *      normalised as ft8_lib normalises its own.
 *  Cost (measured, docs/v12/FTX-SENSITIVITY.md): one window FFT per pass, then per candidate one inverse FFT of 3,072
 *  (FT8) and ~45 k complex multiply-adds — four lanes at a time (NEON / SSE2 / scalar, ft8_lib's vibe_simd4.h). */
class FtxFine {
public:
    /** `mode` 0 FT8, 1 FT4, 2 FT2 (FtxMode's order). `nsps` = audio samples per symbol, `window` = samples a slot holds. */
    FtxFine(int mode, int rate, int nsps, int window);
    ~FtxFine();
    FtxFine(const FtxFine&) = delete;
    FtxFine& operator=(const FtxFine&) = delete;

    /** The window's spectrum, for this pass: call before measure() whenever the audio changed (a new slot, or a
     *  pass after subtraction). Cheap to call again with no change: `invalidate()` + `ensure()`. */
    void invalidate() { have_ = false; }
    void ensure(const float* audio, int n);
    bool ready() const { return have_; }

    static constexpr int kMaxSets = 3;
    struct Meas {
        int   nsets = 0;           ///< likelihood sets available (llrGroup(groupOf(k)) for k < nsets)
        int   syncHits = 0;        ///< Costas symbols whose strongest tone is the expected one
        int   syncTotal = 0;
        double start = 0;          ///< audio sample where symbol 0 starts (fine)
        double f0 = 0;             ///< Hz of tone 0 (fine)
        float  syncSnr = 0;        ///< mean Costas tone power over the mean of the others (linear)
    };
    /** Measure a candidate: `start` = coarse audio sample of symbol 0, `f0` = coarse Hz of tone 0. Returns false — and
     *  costs only the sync — when fewer than `minSyncHits` Costas symbols agree (noise). */
    bool measure(double start, double f0, int minSyncHits, Meas& m);

    /** Set k's grouping: 1, 2, then 3 symbols (FT4/FT2: 1, 2, 4). */
    int groupOf(int k) const { return k == 0 ? 1 : k == 1 ? 2 : (ntones_ == 8 ? 3 : 4); }
    /** Bit log-likelihoods (log p(1)/p(0), codeword order) from the symbol amplitudes of the LAST measure(), over
     *  groups of `group` symbols (1..4). */
    void llrGroup(int group, float* llr) const;

    int frameSymbols() const { return nn_; }
    int decimation() const { return dd_; }
    int basebandLength() const { return nd_; }
    std::size_t ramBytes() const;

private:
    void extract(double f0);                 // → zr_/zi_ (nd_ samples at rate_/dd_), tone 0 near 0 Hz
    void corr(int at, const float* c, const float* s, float& re, float& im) const;

    int mode_, rate_, nsps_, window_;
    int nn_, ntones_, bps_;                  // channel symbols, tones, bits per symbol
    int sps_, dd_, nd_, nfft_;               // baseband samples per symbol, decimation, baseband length, window FFT size
    double T_, baud_, df_;                   // symbol period, tone spacing, window-FFT bin width
    std::vector<int> syncSym_, syncTone_, dataSym_;
    const uint8_t* gray_;
    void* fwd_ = nullptr;                    // PFFFT_Setup* (real, nfft_)
    void* inv_ = nullptr;                    // PFFFT_Setup* (complex, nd_)
    float* spec_ = nullptr;                  // nfft_ floats: the window's ordered spectrum
    float* work_ = nullptr;                  // nfft_ floats: PFFFT scratch (the complex one needs 2·nd_ ≤ nfft_)
    float* bb_ = nullptr;                    // 2·nd_ floats: the cut spectrum, then the baseband (interleaved)
    std::vector<float> zr_, zi_;             // the baseband, split
    std::vector<float> taper_;               // the band's edge, one entry per window-FFT bin
    int tLo_ = 0, tHi_ = 0;                  // the band in window-FFT bins relative to tone 0
    int nDelta_ = 0;                         // frequency grid: (nDelta_ * 2 + 1) steps of baud/16
    std::vector<float> tabC_, tabS_;         // [delta][tone][sps]: e^{-j2π(tone + δ/baud) j / sps}
    std::vector<float> csR_, csI_;           // [symbol][tone]: the matched filter outputs of the last measure()
    bool have_ = false;
};

} // namespace vibe
