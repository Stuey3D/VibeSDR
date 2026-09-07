// vibe_dab_tii.h — Transmitter Identification Information: WHICH transmitter(s) we are hearing.
//
// ★★★ WHAT THIS IS FOR. EN 300 401 V2.2.1 clause 14.8. In an SFN every transmitter radiates the
//     same ensemble, so nothing in the FIC says which one reached the aerial. The null symbol
//     does: each transmitter switches on a sparse set of carrier PAIRS during the null of every
//     frame whose CIF count is 0..3 modulo 8, and the set encodes its Main Id (pattern p, 0..69)
//     and Sub Id (comb c, 0..23). Stuart's brief: "TII, which tells a DX-er WHICH transmitter
//     they have. Nobody in the list above surfaces this well and it is exactly our audience."
//
// The layout (14.8.1, mode I): four sections of 384 carriers; within each, comb c and bit b of
// the pattern select carrier k = base + 2c + 48b (base = -768, -384, 1, 385) and its neighbour
// k+1. Both carriers of a pair carry the PRS phase of the FIRST (z_k = A(k)e^{j phi_k} +
// A(k-1)e^{j phi_(k-1)}), so the product null[k] x conj(null[k+1]) is a real positive number for
// a transmitted pair and noise-like otherwise — coherent across the four sections, and coherent
// across frames, so it can be averaged for weak transmitters. That product is also invariant to
// the linear phase ramp any window position inside the null puts across the carriers, which is
// why the null need not be timed exactly.
//
// Table 26 (the patterns) is the seventy 8-bit values with exactly four ones, in ascending order
// — generated here and pinned against the printed table by test-dab-tii.
//
// ★ The approach — pair products, section sum, per-comb ranking, pattern lookup — is the one every
//   open decoder converges on (welle.io, dab-cmdline); this is our own implementation of it.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace vibedab {

using TiiC32 = std::complex<float>;

/** Table 26, generated: pattern p -> 8-bit a_p(b) with b = 0 the MSB, exactly four bits set. */
inline uint8_t tiiPattern(int p) {
    static uint8_t table[70] = {0};
    static bool built = false;
    if (!built) {
        int n = 0;
        for (int v = 0; v < 256 && n < 70; ++v)
            if (__builtin_popcount(unsigned(v)) == 4) table[n++] = uint8_t(v);
        built = true;
    }
    return (p >= 0 && p < 70) ? table[p] : 0;
}
/** The inverse: an 8-bit selection with four ones -> pattern number, or -1. */
inline int tiiPatternIndex(uint8_t bits) {
    if (__builtin_popcount(unsigned(bits)) != 4) return -1;
    for (int p = 0; p < 70; ++p) if (tiiPattern(p) == bits) return p;
    return -1;
}

/** Carrier index k (-768..768, 0 unused) -> position in our K = 1536 carrier array. */
inline int tiiCarrierPos(int k) { return k < 0 ? k + 768 : k + 767; }

/** The first carrier of pair (section s, comb c, bit b). */
inline int tiiPairCarrier(int section, int c, int b) {
    static const int base[4] = { -768, -384, 1, 385 };
    return base[section & 3] + 2 * c + 48 * b;
}

struct TiiHit {
    int   mainId  = -1;     ///< pattern p
    int   subId   = -1;     ///< comb c
    float strength = 0.0f;  ///< the pair energy of this transmitter over the null's noise, in dB
};

/** ★ Accumulates pair products over TII frames and reports the transmitters it can see. */
class TiiDetector {
public:
    /** Feed the K = 1536 carriers (our array order) of one TII-bearing null symbol. */
    void feed(const TiiC32* carriers, int K) {
        if (K != 1536) return;
        if (acc_.size() != 24u * 8u) acc_.assign(24 * 8, TiiC32{});
        for (int c = 0; c < 24; ++c)
            for (int b = 0; b < 8; ++b) {
                TiiC32 sum{};
                for (int s = 0; s < 4; ++s) {
                    const int k  = tiiPairCarrier(s, c, b);
                    const int p0 = tiiCarrierPos(k), p1 = tiiCarrierPos(k + 1);
                    if (p0 < 0 || p1 >= K) continue;
                    sum += carriers[p0] * std::conj(carriers[p1]);
                }
                acc_[size_t(c * 8 + b)] += sum;
            }
        // ★ The null's own noise power, from carriers no pattern uses (odd carriers, which are
        //   never the FIRST of a pair): the reference the hits are measured against.
        double noise = 0.0; int n = 0;
        for (int k = -767; k < 768; k += 2) {
            if (k == 0 || k == 1) continue;
            const int p = tiiCarrierPos(k);
            if (p < 0 || p >= K) continue;
            noise += std::norm(carriers[p]); ++n;
        }
        if (n) noiseAcc_ += noise / n;
        if (++frames_ >= kFramesPerVerdict) { decide(); frames_ = 0; noiseAcc_ = 0.0; std::fill(acc_.begin(), acc_.end(), TiiC32{}); }
    }
    const std::vector<TiiHit>& hits() const { return hits_; }
    void reset() { acc_.clear(); frames_ = 0; noiseAcc_ = 0.0; hits_.clear(); }

    static constexpr int kFramesPerVerdict = 4;   ///< ~0.8 s of TII frames (every other frame)

private:
    void decide() {
        hits_.clear();
        if (acc_.empty() || frames_ == 0) return;
        /* A transmitted pair sums coherently (real, positive): use the real part, which also
         * rejects half the noise. Per pair-slot power over `frames_` frames and 4 sections. */
        const double norm = 1.0 / (double(frames_) * 4.0);
        const double noisePair = (noiseAcc_ / frames_) * 1.0;    // E|n_k n_k+1*| ~ noise power
        for (int c = 0; c < 24; ++c) {
            float v[8]; int idx[8];
            for (int b = 0; b < 8; ++b) { v[b] = float(acc_[size_t(c * 8 + b)].real() * norm); idx[b] = b; }
            std::sort(idx, idx + 8, [&](int a, int b2) { return v[a] > v[b2]; });
            // the four strongest must all stand well above the noise, and above the other four
            const float fourth = v[idx[3]], fifth = v[idx[4]];
            /* ★ The noise in an AVERAGED pair product is the carrier noise power over the square
             *  root of the number of products summed (4 sections x frames); a transmitter must
             *  clear four of those on all four of its pairs, and the fifth-strongest slot must
             *  look like noise, or this is not a 4-of-8 pattern but a strong neighbour's leakage. */
            const float sigma = float(noisePair / std::sqrt(4.0 * frames_));
            if (fourth <= 4.0f * sigma) continue;
            if (fourth < fifth * 2.0f + 2.0f * sigma) continue;
            uint8_t bits = 0;
            for (int i = 0; i < 4; ++i) bits |= uint8_t(0x80 >> idx[i]);
            const int p = tiiPatternIndex(bits);
            if (p < 0) continue;
            TiiHit h;
            h.mainId = p; h.subId = c;
            const float mean4 = (v[idx[0]] + v[idx[1]] + v[idx[2]] + v[idx[3]]) / 4.0f;
            h.strength = 10.0f * std::log10(mean4 / float(noisePair + 1e-20));
            hits_.push_back(h);
        }
        std::sort(hits_.begin(), hits_.end(), [](const TiiHit& a, const TiiHit& b) { return a.strength > b.strength; });
        if (hits_.size() > 8) hits_.resize(8);
    }

    std::vector<TiiC32> acc_;
    std::vector<TiiHit> hits_;
    int    frames_  = 0;
    double noiseAcc_ = 0.0;
};

}  // namespace vibedab
