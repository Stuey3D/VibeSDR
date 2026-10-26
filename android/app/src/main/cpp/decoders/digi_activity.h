// decoders/digi_activity.h — BAND MODE'S ACTIVITY GATE (V12, 2026-10-27): is there anything in a mode's window? Read from
// the band slice's OWN FFT bins — the forward transform every listener's channel is already cut from — so it costs no
// transform of its own: a sum per bin per block.
//
// ★★★ WHY (Stuart, 2026-10-27): "we want to make the band digital spots mode pull everything it can down, but we also have
//     to be mindful of CPU running that many decoders at once." Most of Band's cost is DECODES (a slot's candidate search
//     and LDPC, WSPR's Fano search, JTTY's spectrogram), and most windows on most bands are EMPTY most of the day: WSPR
//     on a dead 10 m, JTTY anywhere but 14.090, PSK nearly everywhere. A decoder told about an empty window does a full
//     search of noise. This gate decides, per mode window and per gate period (the mode's slot), whether anything is
//     there; a period with nothing is never handed to the engine (decoders/digi_spotter.cpp: the slot's audio is held
//     while the gate is undecided, flushed to the engine the moment the window lights up, dropped if it never does).
//
// ── The measure ─────────────────────────────────────────────────────────────────────────────────────────────────────
//  • Each IQ block of the slice comes with its bins' POWER (slicePower: Hann-windowed in the frequency domain —
//    ½X[k] − ¼X[k±1] — so a strong station's leakage stays within a couple of bins, lowest frequency first).
//  • THE FLOOR: every half second, the MEDIAN over the slice's inner 84 % of each bin's half-second mean. A band slice is
//    mostly empty spectrum between the modes' windows (20 m: ≈ 17 of 75 kHz carry anything), so the median is noise; it
//    is averaged over the last eight half-seconds.
//  • A WINDOW (dial + audioLo … audioHi, in RF) is OCCUPIED in a period once any of its bins' mean power over the period
//    so far stands above the floor by more than chance allows: mean/floor > 1 + z/√N (N blocks averaged; each bin's
//    power is exponential on noise, so its N-block mean has σ/μ = 1/√N), z = kZ. The longer the period runs, the
//    weaker the signal it can see: over a whole FT8 slot (≈ 2500 blocks at 2.4 MS/s) +0.4 dB in a 146 Hz bin is
//    enough — an FT8 station near −20 dB in 2500 Hz.
// ★★ ERRS TOWARDS DECODING. A false "occupied" costs one decode (what Band paid for every window before); a false "empty"
//    loses spots. So z is modest and a window is judged on its strongest bin. The one blind spot is stated: a station
//    too weak to raise its 146 Hz bin over a whole period (WSPR below ≈ −26 dB, FT8 below ≈ −21 dB in 2500 Hz) ALONE in
//    an otherwise empty window is not decoded. For that the owner can set a mode to Always (no gate, never shed) —
//    docs/v12/DIGITAL-SPOTS.md "The Band governor".
// ★ No bins (a test feeding bare IQ, the benchmark): the gate says "occupied" — exactly the old behaviour.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace vibe {
namespace digi {

/** ★ The slice's bins' POWER, lowest frequency first: out[k] is the bin at (k − nb/2) bins from `centreBin`, in the
 *  forward FFT's own layout (DC at 0, negatives at the top, wrapped — as Channelizer::extract gathers them). Hann-windowed
 *  in the frequency domain (½X[k] − ¼X[k−1] − ¼X[k+1]), scaled by 1/fftSize² (the extract's own 1/fftSize, squared). */
inline void slicePower(const std::complex<float>* bins, int fftSize, int centreBin, int nb, float* out) {
    if (!bins || fftSize <= 0 || nb <= 0) return;
    const float g = 1.0f / ((float)fftSize * (float)fftSize);
    auto at = [&](int k) { int i = (centreBin + k) % fftSize; if (i < 0) i += fftSize; return bins[i]; };
    const int half = nb / 2;
    for (int k = 0; k < nb; ++k) {
        const int off = k - half;
        const std::complex<float> v = 0.5f * at(off) - 0.25f * (at(off - 1) + at(off + 1));
        out[k] = (v.real() * v.real() + v.imag() * v.imag()) * g;
    }
}

/** ★ The slice's noise floor, from its bins (see the file note). Fed once per block; `floor()` is in the bins' units. */
class SliceFloor {
public:
    void reset() { acc_.clear(); n_ = 0; hist_.clear(); floor_ = 0; nb_ = 0; }
    /** One block's powers. `blocksPerTick`: how many blocks make the half-second the median is taken over. */
    void block(const float* p, int nb, int blocksPerTick) {
        if (nb != nb_) { reset(); nb_ = nb; acc_.assign((size_t)nb, 0.0); }
        for (int k = 0; k < nb; ++k) acc_[(size_t)k] += p[k];
        if (++n_ < std::max(1, blocksPerTick)) return;
        // ★ the median over the inner 84 %: the outer 8 % each side is the slice's own roll-off
        const int lo = nb * 8 / 100, hi = nb - lo;
        tmp_.assign(acc_.begin() + lo, acc_.begin() + hi);
        if (tmp_.empty()) return;
        std::nth_element(tmp_.begin(), tmp_.begin() + tmp_.size() / 2, tmp_.end());
        const double med = tmp_[tmp_.size() / 2] / n_;
        /* ★ The median of exponential powers is ln2 ≈ 0.693 of their mean — but these are n_-block means, nearly
         *  Gaussian, whose median IS their mean. No correction. */
        hist_.push_back(med);
        if (hist_.size() > 8) hist_.erase(hist_.begin());
        double s = 0; for (double v : hist_) s += v;
        floor_ = s / hist_.size();
        std::fill(acc_.begin(), acc_.end(), 0.0); n_ = 0;
    }
    double floor() const { return floor_; }
    bool ready() const { return floor_ > 0; }
private:
    std::vector<double> acc_, tmp_, hist_;
    int n_ = 0, nb_ = 0;
    double floor_ = 0;
};

/** ★ One mode window's energy over a gate period: per-bin sums since the period began. */
class WindowMeter {
public:
    /** ★ The z of the test (see the file note). Measured on noise (test-digi-spots): at z = 4.5 a window of ~25 bins tested
     *  every block over a whole FT8 slot is judged occupied in ~1 % of empty slots — which only costs the decode Band
     *  always paid; lower lets a weaker lone station open its window. */
    static constexpr double kZ = 4.5;
    /** Where the window sits in the slice's bins: [k0, k1). */
    void place(int k0, int k1) { if (k0 != k0_ || k1 != k1_) { k0_ = k0; k1_ = k1; restart(); } }
    void restart() { acc_.assign((size_t)std::max(0, k1_ - k0_), 0.0); n_ = 0; }
    bool placed() const { return k1_ > k0_; }
    void block(const float* p, int nb) {
        if (k1_ <= k0_ || k0_ < 0 || k1_ > nb) return;
        for (int k = k0_; k < k1_; ++k) acc_[(size_t)(k - k0_)] += p[k];
        ++n_;
    }
    /** The strongest bin's mean over the period, as a ratio to `floor` (0 when nothing is measured yet). */
    double peakRatio(double floor) const {
        if (n_ <= 0 || !(floor > 0) || acc_.empty()) return 0;
        return *std::max_element(acc_.begin(), acc_.end()) / (n_ * floor);
    }
    /** ★ Anything there? Needs at least `minBlocks` averaged (the first block or two say nothing). */
    bool occupied(double floor, int minBlocks = 16) const {
        if (n_ < minBlocks || !(floor > 0)) return false;
        return peakRatio(floor) > 1.0 + kZ / std::sqrt((double)n_);
    }
    int blocks() const { return n_; }
private:
    std::vector<double> acc_;
    int k0_ = 0, k1_ = 0, n_ = 0;
};

} // namespace digi
} // namespace vibe
