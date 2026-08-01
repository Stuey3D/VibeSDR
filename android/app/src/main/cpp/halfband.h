// halfband.h — cascaded half-band decimation for interleaved complex float IQ.
//
// ★★★ WHY THIS EXISTS. Two reasons, and the second is the one that matters for weak signals:
//
//   1. THE RADIO'S OWN LOW RATES ARE NOT TRUSTWORTHY. An HF+ Discovery on firmware R5.0.1-CD
//      advertises seven sample rates, delivers three, and the lowest of those (228 kHz) tunes to
//      the wrong frequency (measured 2026-08-01/02 — see airspyhf_source.cpp). Decimating 456 kHz
//      by two gives an honest 228 kHz span from a rate the radio demonstrably gets right.
//      ★ This is exactly what SDR# does and does not tell you: Edouard Griffiths, on the Airspy
//      list, "SDR# does software decimation without telling you… 228 = 456 / 2".
//
//   2. IT IS REAL PROCESSING GAIN. Halving the bandwidth halves the noise power while a signal
//      inside the passband is untouched, so each stage is worth ~3 dB of SNR on a weak carrier.
//      That is the whole point for a DXer, and it is why this belongs in the product rather than
//      being a workaround for a firmware bug.
//
// ★★ AND IT SAVES UPLINK. A narrower span is fewer bins for the same resolution, so on VibeServer
// this REDUCES what the owner sends rather than costing them — the same argument as SDR++ Brown's
// "Fill-In bandwidth", which is the same machinery pointed at the HF+'s dead lobes.
//
// ── Why half-band ────────────────────────────────────────────────────────────
// A half-band filter has every second tap exactly zero and a centre tap of 0.5, so a decimate-by-2
// costs a little over a quarter of the multiplies of a general FIR of the same length — you skip
// the zero taps AND only compute every other output. It is the standard tool for exactly this job.
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace vibe {

/** One decimate-by-2 stage for interleaved complex float. */
class HalfBandStage {
public:
    /** @param taps must be ODD and of the form 4k+3 (…, 11, 15, 19, 23, 31) so the zero pattern
     *  works out; 31 is a good quality/cost point at these rates. */
    explicit HalfBandStage(int taps = 31) { design(taps); }

    void reset() { std::fill(hist_.begin(), hist_.end(), 0.0f); pos_ = 0; phase_ = 0; }

    /** Decimate `nComplex` input samples into `out` (interleaved). Returns complex samples written
     *  — normally nComplex/2, ±1 depending on where the phase started. */
    int process(const float* in, int nComplex, float* out) {
        int written = 0;
        for (int i = 0; i < nComplex; i++) {
            // Push into the circular history (complex: two floats per slot).
            hist_[pos_ * 2]     = in[i * 2];
            hist_[pos_ * 2 + 1] = in[i * 2 + 1];
            pos_ = (pos_ + 1) % len_;

            // Emit on every second input — that IS the decimation.
            phase_ ^= 1;
            if (phase_) continue;

            float re = 0.0f, im = 0.0f;
            // ★ Only the non-zero taps. The centre tap is handled with them; the rest of the even
            //   positions are exactly zero by construction and are never touched.
            for (size_t k = 0; k < nz_.size(); k++) {
                const int tap = nz_[k];
                // History index for this tap: newest sample is at pos_-1.
                int idx = pos_ - 1 - tap;
                while (idx < 0) idx += len_;
                const float c = nzc_[k];
                re += c * hist_[idx * 2];
                im += c * hist_[idx * 2 + 1];
            }
            out[written * 2]     = re;
            out[written * 2 + 1] = im;
            written++;
        }
        return written;
    }

private:
    void design(int taps) {
        if (taps < 7) taps = 7;
        if ((taps & 1) == 0) taps++;              // must be odd
        len_ = taps;
        hist_.assign((size_t)len_ * 2, 0.0f);
        const int M = (len_ - 1) / 2;

        // ★ Windowed-sinc half-band, computed rather than pasted from a table: cutoff at fs/4, so
        //   h[n] = sinc((n-M)/2)/2, which is exactly zero at every even offset except the centre.
        //   A Hamming window keeps the stopband about -53 dB, which is well below the noise on any
        //   HF signal this will ever see.
        std::vector<float> h((size_t)len_, 0.0f);
        for (int n = 0; n < len_; n++) {
            const int d = n - M;
            double v;
            if (d == 0) v = 0.5;
            else if ((d & 1) == 0) v = 0.0;        // the half-band zeros
            else v = std::sin(M_PI * d * 0.5) / (M_PI * d);
            const double w = 0.54 - 0.46 * std::cos(2.0 * M_PI * n / (len_ - 1));
            h[(size_t)n] = (float)(v * w);
        }
        // Unity DC gain — otherwise each stage would quietly change the level, and the auto-range
        // downstream would chase it.
        double sum = 0.0;
        for (float c : h) sum += c;
        if (sum > 1e-9) for (float& c : h) c = (float)(c / sum);

        nz_.clear(); nzc_.clear();
        for (int n = 0; n < len_; n++)
            if (h[(size_t)n] != 0.0f) { nz_.push_back(n); nzc_.push_back(h[(size_t)n]); }
    }

    int len_ = 0, pos_ = 0, phase_ = 0;
    std::vector<float> hist_;     // interleaved complex history
    std::vector<int>   nz_;       // indices of the non-zero taps
    std::vector<float> nzc_;      // their coefficients
};

/** A cascade of half-band stages: decimation of 1, 2, 4, 8 … */
class HalfBandChain {
public:
    /** @param factor 1 (bypass), 2, 4, 8 … must be a power of two. */
    void setFactor(int factor) {
        int stages = 0;
        for (int f = factor; f > 1; f >>= 1) stages++;
        if ((1 << stages) != factor) stages = 0;    // not a power of two → bypass
        factor_ = 1 << stages;
        stages_.clear();
        for (int i = 0; i < stages; i++) stages_.emplace_back(31);
        scratch_.assign(2, std::vector<float>());
    }
    int factor() const { return factor_; }
    void reset() { for (auto& s : stages_) s.reset(); }

    /** Decimate in place-ish: returns a pointer to the output (owned here) and its complex count.
     *  With factor 1 this hands the input straight back — no copy, no cost. */
    const float* process(const float* in, int nComplex, int& outCount) {
        if (stages_.empty()) { outCount = nComplex; return in; }
        const float* src = in;
        int n = nComplex;
        for (size_t i = 0; i < stages_.size(); i++) {
            auto& buf = scratch_[i & 1];
            if ((int)buf.size() < n) buf.assign((size_t)n * 2, 0.0f);
            n = stages_[i].process(src, n, buf.data());
            src = buf.data();
        }
        outCount = n;
        return src;
    }

private:
    int factor_ = 1;
    std::vector<HalfBandStage> stages_;
    std::vector<std::vector<float>> scratch_{2};
};

} // namespace vibe
