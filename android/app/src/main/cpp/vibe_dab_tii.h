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
//
// ★★★ WHY 11D AND BEDS/BUCKS/HERTS (10D) IDENTIFIED NOTHING — measured 2026-11-04 on the Pi 500's
//     V4L, captures in ~/VibeSDR-test-audio/dab/tii. The TII was there all along; two things hid it:
//   1. THE SECOND CARRIER'S PHASE. The product above assumes both carriers of a pair carry phi_k.
//      Daventry (11D, 23/2), Bow Brickhill and Sandy Heath (10D, 22/9 and 22/3) radiate each carrier
//      of a pair with ITS OWN reference phase, phi_k and phi_(k+1): with each carrier's PRS phase taken
//      out, the four sections add up to within six degrees; without, the product lands on +1, -1 or
//      +-j of that depending on the pair, so a transmitter 7800 sigma clear summed NEGATIVE on two of
//      its four blocks and its comb was rejected. 12D carries both conventions (one site each way), so
//      every block is measured BOTH ways — the second is the same products, each turned by
//      conj(z_k) z_(k+1), a multiple of a quarter turn: no extra transform — and the larger is kept.
//   2. THE NOISE REFERENCE. "Odd carriers" are not TII-free: every carrier belongs to some pair slot
//      (24 combs x 8 blocks x 2 = 384, a whole section), and the odd ones include the FIRST carrier of
//      every pair in sections three and four. On a strong multiplex the reference was the TII itself
//      — 5.3 against a true noise of 0.09 on 11D, 18 dB high — burying every weaker site. The new path
//      takes the noise as the lower quartile of the null's carrier powers (an exponential's 25th
//      percentile is 0.288 of its mean): a TII would have to light three quarters of the band to move it.
//   ★ The original test (plain product, odd-carrier reference) still runs FIRST and unchanged: every
//     comb it identified it still identifies, with the same figure. The new path sees only its rejects.
// ★★ COLLISIONS (QIRX: qirx.softsyst.com/QIRXTIICollisions). A dense SFN reuses a Sub Id under
//    different Main Ids — around Northamptonshire 20 of 11D's 23 nearby Sub Ids are shared — so a comb
//    can light 5..8 blocks and no single 4-of-8 pattern fits. The detector does not guess: it hands such
//    a comb out as a TiiShared (which blocks are lit, and how strongly), and tiiResolveShared() keeps
//    only the patterns made entirely of lit blocks. With a transmitter directory for the ensemble, only
//    codes that really exist there survive (kTiiResolved); without one each fitting pattern is reported
//    as a CANDIDATE (kTiiCandidate) — "one of", never one confident wrong name. The directory stays out
//    of this header: the caller passes `known(main, sub)`.
// ★★ LEAKAGE. A very strong transmitter spills into the same blocks of the neighbouring combs (two
//    carriers away): ~1 % of Daventry's 7800 sigma at the Pi 500 — 66 to 96 sigma, far above the noise.
//    On the new path a block counts as lit only if it also stands above kTiiLeak of the same block one or
//    two combs either side. That costs a site more than ~18 dB under its neighbour comb's same block; it
//    is what stops Daventry's spill being named as a transmitter.
// ★ CPU: the second measure re-weights the same 768 products by a precomputed unit factor, and the
//   noise quartile is one nth_element over 1536 floats — per TII frame, about five a second. Nothing here
//   touches the audio or the FIC: it is a side output of a null the receiver has already transformed.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "vibe_dab_prs.h"

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

/** How a hit was reached — `"c"` in the report (absent for kTiiClean). */
enum : uint8_t {
    kTiiClean     = 0,   ///< a clean 4-of-8 comb: a measurement on its own
    kTiiResolved  = 1,   ///< from a shared comb, and the directory has a site with this code here
    kTiiCandidate = 2,   ///< from a shared comb, no directory for the ensemble: ONE OF its candidates
};

struct TiiHit {
    int     mainId   = -1;     ///< pattern p
    int     subId    = -1;     ///< comb c
    float   strength = 0.0f;   ///< the pair energy of this transmitter over the null's noise, in dB
    uint8_t from     = kTiiClean;
};

/** ★ A comb with MORE than four blocks lit — several transmitters on one Sub Id (a collision).
 *  `v` is each block's pair energy and `noise` the null's carrier noise power, in the same units. */
struct TiiShared {
    int     subId  = -1;
    uint8_t active = 0;          ///< the lit blocks, bit 0x80 >> b as in tiiPattern
    float   v[8]   = {0};
    float   noise  = 0;
    bool    alongside = false;   ///< this comb ALSO gave a measured (clean) hit
};

/** ★★ Resolve shared combs into hits. `known(main, sub)`: does the transmitter directory hold a site
 *  with this code on this ensemble; `haveDirectory`: does it hold ANY TII code for the ensemble (when
 *  false, every fitting pattern is reported as a candidate). Appends to `hits`, then orders the lot —
 *  measured and resolved first, strongest first, candidates after — and caps it at `cap`.
 *  The strength of a collision hit is its WEAKEST block: the others may also carry a co-channel site. */
template <class Known>
inline void tiiResolveShared(const std::vector<TiiShared>& shared, bool haveDirectory, Known known,
                             std::vector<TiiHit>& hits, size_t cap = 8) {
    const size_t measured = hits.size();
    for (const auto& sc : shared) {
        if (sc.alongside && !haveDirectory) continue;   // a measured site already speaks for this Sub Id
        for (int p = 0; p < 70; ++p) {
            const uint8_t bits = tiiPattern(p);
            if ((bits & sc.active) != bits) continue;               // every one of its blocks must be lit
            if (haveDirectory && !known(p, sc.subId)) continue;      // no such site on this ensemble
            bool dup = false;
            for (size_t i = 0; i < measured; ++i) if (hits[i].mainId == p && hits[i].subId == sc.subId) dup = true;
            if (dup) continue;
            float weakest = 1e30f;
            for (int b = 0; b < 8; ++b) if (bits & (0x80 >> b)) weakest = std::min(weakest, sc.v[b]);
            TiiHit h; h.mainId = p; h.subId = sc.subId; h.from = haveDirectory ? kTiiResolved : kTiiCandidate;
            h.strength = 10.0f * std::log10(std::max(weakest, 1e-20f) / (sc.noise + 1e-20f));
            hits.push_back(h);
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const TiiHit& a, const TiiHit& b) {
        const bool ca = a.from == kTiiCandidate, cb = b.from == kTiiCandidate;
        if (ca != cb) return cb;
        return a.strength > b.strength;
    });
    if (hits.size() > cap) hits.resize(cap);
}

/** ★ Accumulates pair products over TII frames and reports the transmitters it can see. */
class TiiDetector {
public:
    /** Feed the K = 1536 carriers (our array order) of one TII-bearing null symbol. */
    void feed(const TiiC32* carriers, int K) {
        if (K != 1536) return;
        if (acc_.size() != 24u * 8u) { acc_.assign(24 * 8, TiiC32{}); accOwn_.assign(24 * 8, TiiC32{}); }
        if (ownTurn_.empty()) {
            /* ★ The own-phase measure (see the top): x_k conj(z_k) times conj(x_(k+1) conj(z_(k+1))) is
             *  the plain product turned by conj(z_k) z_(k+1) — a multiple of a quarter turn. */
            ownTurn_.resize(24 * 8 * 4);
            for (int c = 0; c < 24; ++c) for (int b = 0; b < 8; ++b) for (int s = 0; s < 4; ++s) {
                const int k = tiiPairCarrier(s, c, b);
                ownTurn_[size_t((c * 8 + b) * 4 + s)] = std::conj(prsCarrier(k)) * prsCarrier(k + 1);
            }
        }
        for (int c = 0; c < 24; ++c)
            for (int b = 0; b < 8; ++b) {
                TiiC32 sum{}, own{};
                for (int s = 0; s < 4; ++s) {
                    const int k  = tiiPairCarrier(s, c, b);
                    const int p0 = tiiCarrierPos(k), p1 = tiiCarrierPos(k + 1);
                    if (p0 < 0 || p1 >= K) continue;
                    const TiiC32 prod = carriers[p0] * std::conj(carriers[p1]);
                    sum += prod;
                    own += prod * ownTurn_[size_t((c * 8 + b) * 4 + s)];
                }
                acc_[size_t(c * 8 + b)] += sum;
                accOwn_[size_t(c * 8 + b)] += own;
            }
        // ★ The null's own noise power, from carriers no pattern uses (odd carriers, which are
        //   never the FIRST of a pair): the reference the hits are measured against.
        //   ★ Not true in sections three and four — see the top. Kept for the original test only.
        double noise = 0.0; int n = 0;
        for (int k = -767; k < 768; k += 2) {
            if (k == 0 || k == 1) continue;
            const int p = tiiCarrierPos(k);
            if (p < 0 || p >= K) continue;
            noise += std::norm(carriers[p]); ++n;
        }
        if (n) noiseAcc_ += noise / n;
        // ★ The new path's noise: the lower quartile of the carrier powers, as a mean (see the top).
        pw_.resize(size_t(K));
        for (int i = 0; i < K; ++i) pw_[size_t(i)] = std::norm(carriers[i]);
        std::nth_element(pw_.begin(), pw_.begin() + K / 4, pw_.end());
        noiseQ_ += double(pw_[size_t(K / 4)]) / 0.2877;
        if (++frames_ >= kFramesPerVerdict) {
            decide(); frames_ = 0; noiseAcc_ = 0.0; noiseQ_ = 0.0;
            std::fill(acc_.begin(), acc_.end(), TiiC32{}); std::fill(accOwn_.begin(), accOwn_.end(), TiiC32{});
        }
    }
    /** The combs that identified a transmitter on their own (kTiiClean), strongest first. */
    const std::vector<TiiHit>& hits() const { return hits_; }
    /** ★ The combs with more than four blocks lit — for tiiResolveShared, with the directory. */
    const std::vector<TiiShared>& shared() const { return shared_; }
    /** ★ Why a null symbol did NOT yield a hit: the best comb's four-strongest against the noise
     *  and against the fifth. For the pane and for tuning the test on a multiplex that identifies
     *  nothing (11D, Stuart, 2026-09-07: "the original commercial DAB multiplex of the UK").
     *  ★ Measured on the new path (both phase conventions, the quartile noise) since 2026-11-04 — the
     *    old figure called 11D "none transmitted" while Daventry stood 7800 sigma clear. `active` is how
     *    many blocks of that comb are lit (more than 4 = a shared Sub Id); `verdicts` counts decisions. */
    struct Diag { int comb = -1; float fourthOverSigma = 0, fourthOverFifth = 0; int frames = 0; int active = 0; unsigned verdicts = 0; };
    const Diag& diag() const { return diag_; }
    void reset() { acc_.clear(); accOwn_.clear(); frames_ = 0; noiseAcc_ = 0.0; noiseQ_ = 0.0; hits_.clear(); shared_.clear(); diag_ = Diag{}; }

    static constexpr int kFramesPerVerdict = 4;   ///< ~0.8 s of TII frames (every other frame)
    /** On the new path a block is lit above this many sigma (the original test's own floor)… */
    static constexpr float kTiiActiveSigma = 4.0f;
    /** …and above this fraction of the same block one or two combs either side (leakage; see the top). */
    static constexpr float kTiiLeak = 0.015f;

private:
    void decide() {
        hits_.clear(); shared_.clear();
        if (acc_.empty() || frames_ == 0) return;
        bool taken[24] = {false};
        /* A transmitted pair sums coherently (real, positive): use the real part, which also
         * rejects half the noise. Per pair-slot power over `frames_` frames and 4 sections. */
        const double norm = 1.0 / (double(frames_) * 4.0);
        const double noisePair = (noiseAcc_ / frames_) * 1.0;    // E|n_k n_k+1*| ~ noise power
        { const unsigned nv = diag_.verdicts + 1; diag_ = Diag{}; diag_.frames = frames_; diag_.verdicts = nv; }
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
            /* ★★ THE FIGURE IS OVER THE TRUE NOISE (Stuart, 2026-11-04: "if our SNR figures are wrong
             *  because we measure them wrong please fix them"). The DECISION above still uses the
             *  odd-carrier reference, untouched — what it accepts is exactly what it accepted — but that
             *  reference holds TII carriers (the top), so as a figure it read Daventry 15.8 dB on 12B
             *  against 33 on 11D. Now every hit, either path, is its mean over the quartile noise. */
            h.strength = 10.0f * std::log10(mean4 / float(noiseQ_ / frames_ + 1e-20));
            hits_.push_back(h);
            taken[c] = true;
        }
        decideOwn_(taken, norm);
        std::sort(hits_.begin(), hits_.end(), [](const TiiHit& a, const TiiHit& b) { return a.strength > b.strength; });
        if (hits_.size() > 8) hits_.resize(8);
    }

    /** ★ The new path, over the combs the test above rejected: each block the larger of the two
     *  phase measures, against the quartile noise; lit only clear of the noise AND of leakage. */
    void decideOwn_(bool* taken, double norm) {
        const double P = noiseQ_ / frames_;
        const float sigma = float(P / std::sqrt(4.0 * frames_));    // the same form as the test above
        float v[24][8];
        for (int c = 0; c < 24; ++c) for (int b = 0; b < 8; ++b)
            v[c][b] = float(std::max(acc_[size_t(c * 8 + b)].real(), accOwn_[size_t(c * 8 + b)].real()) * norm);
        float bestFourth = -1.0f;
        for (int c = 0; c < 24; ++c) {
            uint8_t lit = 0; float rest = 0.0f;   // rest: the strongest block neither lit nor leakage
            for (int b = 0; b < 8; ++b) {
                float nb = 0.0f;
                for (int d = -2; d <= 2; ++d) if (d && c + d >= 0 && c + d < 24) nb = std::max(nb, v[c + d][b]);
                const bool leak = v[c][b] < kTiiLeak * nb;
                if (v[c][b] > kTiiActiveSigma * sigma && !leak) lit |= uint8_t(0x80 >> b);
                else if (!leak) rest = std::max(rest, v[c][b]);
            }
            const int n = __builtin_popcount(unsigned(lit));
            float s8[8]; for (int b = 0; b < 8; ++b) s8[b] = v[c][b];
            std::sort(s8, s8 + 8, [](float a, float b2) { return a > b2; });
            if (s8[3] > bestFourth) {
                bestFourth = s8[3]; diag_.comb = c; diag_.active = n;
                diag_.fourthOverSigma = sigma > 0 ? s8[3] / sigma : 0;
                diag_.fourthOverFifth = s8[4] > 0 ? s8[3] / s8[4] : 0;
            }
            if (n < 4) continue;
            if (!taken[c]) {
                /* ★ The original test's rule on this measure: the four strongest lit blocks, the weakest
                 *  of them twice whatever is left (a fifth lit block included). A fifth block that only
                 *  just clears the noise is not a second transmitter — this keeps it from turning one
                 *  clean site into five "candidates". */
                int top[8], nt = 0;
                for (int b = 0; b < 8; ++b) if (lit & (0x80 >> b)) top[nt++] = b;
                std::sort(top, top + nt, [&](int a, int b2) { return v[c][a] > v[c][b2]; });
                float rest4 = rest, sum = 0.0f;
                for (int i = 4; i < nt; ++i) rest4 = std::max(rest4, v[c][top[i]]);
                uint8_t bits = 0;
                for (int i = 0; i < 4; ++i) { bits |= uint8_t(0x80 >> top[i]); sum += v[c][top[i]]; }
                if (v[c][top[3]] >= rest4 * 2.0f + 2.0f * sigma) {
                    const int p = tiiPatternIndex(bits);
                    if (p >= 0) {
                        TiiHit h; h.mainId = p; h.subId = c; h.from = kTiiClean;
                        h.strength = 10.0f * std::log10((sum / 4.0f) / float(P + 1e-20));
                        hits_.push_back(h);
                        taken[c] = true;
                    }
                }
            }
            /* ★ More than four lit: a shared Sub Id — even beside a clean hit, whose co-channel neighbour the
             *  directory may still name (tiiResolveShared skips the clean one, and offers no candidates
             *  on a Sub Id that already has a measured hit). */
            if (n > 4) {
                TiiShared sc; sc.subId = c; sc.active = lit; sc.noise = float(P); sc.alongside = taken[c];
                for (int b = 0; b < 8; ++b) sc.v[b] = v[c][b];
                shared_.push_back(sc);
            }
        }
    }

    std::vector<TiiC32> acc_;
    std::vector<TiiC32> accOwn_;      ///< the same products, each carrier's own PRS phase taken out
    std::vector<TiiC32> ownTurn_;     ///< conj(z_k) z_(k+1) per (comb, block, section)
    std::vector<float>  pw_;          ///< scratch: one null's carrier powers
    std::vector<TiiHit> hits_;
    std::vector<TiiShared> shared_;
    Diag diag_;
    int    frames_   = 0;
    double noiseAcc_ = 0.0;
    double noiseQ_   = 0.0;           ///< the quartile noise power, summed over the verdict's frames
};

}  // namespace vibedab
