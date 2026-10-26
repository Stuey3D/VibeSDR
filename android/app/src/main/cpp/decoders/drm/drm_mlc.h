// decoders/drm/drm_mlc.h — one coded BLOCK (an FAC block, an SDC block or a multiplex frame) between its bits and its QAM
// cells, both ways: partitioning (§7.3.1.1), the component codes, the bit interleavers and the mapping (§7.3–7.4).
//
// ★★★ MULTISTAGE DECODING. The constellations are set-partitioned per axis (figures 26/29/30: the LOWEST bit of each axis
//     separates the closest points), which is why level 0 carries the strongest code. The decoder exploits exactly that:
//     level 0 is decoded from soft bits that know nothing, then RE-ENCODED and re-interleaved, and level 1's soft bits
//     are computed with level 0's points fixed — a 64-QAM axis collapses to 4 candidates, then 2. A second pass then
//     re-decodes every level with all the OTHER levels known (`passes` = 2), which recovers most of what the first
//     pass's level-0 errors cost. Both passes are measured in test-drm.
#pragma once
#include "drm_coding.h"
#include <complex>

namespace vibedrm {

/** Bits → cells. `u` is the block AFTER energy dispersal (L1 + L2 bits). */
inline std::vector<std::complex<float>> mlcEncode(const std::vector<uint8_t>& u, const MlcLayout& L) {
    const int ncell = L.n1 + L.n2;
    std::vector<int> rI(size_t(ncell), 0), rQ(size_t(ncell), 0);
    int offA = 0, offB = L.l1;
    for (int p = 0; p < L.levels; ++p) {
        std::vector<uint8_t> x;
        x.insert(x.end(), u.begin() + offA, u.begin() + offA + L.m1[p]);
        x.insert(x.end(), u.begin() + offB, u.begin() + offB + L.m2[p]);
        offA += L.m1[p]; offB += L.m2[p];
        const std::vector<uint8_t> v = convEncode(x, L.sch[p]);
        std::vector<uint8_t> y(v.size());
        const int t = L.fac ? 21 : bitInterleaverT(L.levels, p);
        if (t == 0) y = v;
        else {
            const auto pa = permutation(2 * L.n1, t), pb = permutation(2 * L.n2, t);
            for (int i = 0; i < 2 * L.n1; ++i) y[size_t(i)] = v[size_t(pa[size_t(i)])];
            for (int i = 0; i < 2 * L.n2; ++i) y[size_t(2 * L.n1 + i)] = v[size_t(2 * L.n1 + pb[size_t(i)])];
        }
        for (int i = 0; i < ncell; ++i) { rI[size_t(i)] |= y[size_t(2 * i)] << p; rQ[size_t(i)] |= y[size_t(2 * i + 1)] << p; }
    }
    std::vector<std::complex<float>> z(static_cast<size_t>(ncell));
    for (int i = 0; i < ncell; ++i) z[size_t(i)] = { axisValue(L.levels, rI[size_t(i)]), axisValue(L.levels, rQ[size_t(i)]) };
    return z;
}

/** ★ Cells → bits. `z` are EQUALISED cells, `w` their CSI weights. Returns the L1 + L2 bits (still energy-dispersed). */
class MlcDecoder {
public:
    std::vector<uint8_t> decode(const std::complex<float>* z, const float* w, const MlcLayout& L, int passes = 2) {
        const int ncell = L.n1 + L.n2;
        yI_.resize(size_t(ncell)); yQ_.resize(size_t(ncell)); w_.assign(w, w + ncell);
        for (int i = 0; i < ncell; ++i) { yI_[size_t(i)] = z[i].real(); yQ_[size_t(i)] = z[i].imag(); }
        hardI_.assign(size_t(ncell), 0); hardQ_.assign(size_t(ncell), 0);
        x_.assign(size_t(L.levels), {});
        for (int pass = 0; pass < passes; ++pass)
            for (int p = 0; p < L.levels; ++p) {
                if (pass > 0 && L.levels == 1) break;
                decodeLevel(L, p, pass > 0);
            }
        std::vector<uint8_t> u(size_t(L.l1 + L.l2));
        int offA = 0, offB = L.l1;
        for (int p = 0; p < L.levels; ++p) {
            std::copy(x_[size_t(p)].begin(), x_[size_t(p)].begin() + L.m1[p], u.begin() + offA);
            std::copy(x_[size_t(p)].begin() + L.m1[p], x_[size_t(p)].end(), u.begin() + offB);
            offA += L.m1[p]; offB += L.m2[p];
        }
        return u;
    }
private:
    void decodeLevel(const MlcLayout& L, int p, bool allKnown) {
        const int ncell = L.n1 + L.n2;
        // the known bits: lower levels (first pass), every other level (second pass) — a candidate must match them
        // ★ axisLlr conditions on the bits BELOW p; for the second pass the higher levels are fixed too, which is the
        //   same as conditioning on the full r with bit p free — done by a small scalar loop (it is the minority path).
        llrI_.resize(size_t(ncell)); llrQ_.resize(size_t(ncell));
        if (!allKnown) {
            axisLlr(L.levels, p, yI_.data(), w_.data(), hardI_.data(), llrI_.data(), ncell);
            axisLlr(L.levels, p, yQ_.data(), w_.data(), hardQ_.data(), llrQ_.data(), ncell);
        } else {
            for (int i = 0; i < ncell; ++i) {
                llrI_[size_t(i)] = pairLlr(L.levels, p, yI_[size_t(i)], hardI_[size_t(i)]) * w_[size_t(i)];
                llrQ_[size_t(i)] = pairLlr(L.levels, p, yQ_[size_t(i)], hardQ_[size_t(i)]) * w_[size_t(i)];
            }
        }
        // interleaved stream y_p: cell i → (I, Q)
        std::vector<float> y(size_t(2 * ncell)), v(size_t(2 * ncell));
        for (int i = 0; i < ncell; ++i) { y[size_t(2 * i)] = llrI_[size_t(i)]; y[size_t(2 * i + 1)] = llrQ_[size_t(i)]; }
        const int t = L.fac ? 21 : bitInterleaverT(L.levels, p);
        std::vector<int> pa, pb;
        if (t == 0) v = y;
        else {
            pa = permutation(2 * L.n1, t); pb = permutation(2 * L.n2, t);
            for (int i = 0; i < 2 * L.n1; ++i) v[size_t(pa[size_t(i)])] = y[size_t(i)];
            for (int i = 0; i < 2 * L.n2; ++i) v[size_t(2 * L.n1 + pb[size_t(i)])] = y[size_t(2 * L.n1 + i)];
        }
        x_[size_t(p)] = vit_.decode(v.data(), L.sch[size_t(p)], size_t(L.m1[p] + L.m2[p]));
        // re-encode → the decided bits of this level, back in cell order
        const std::vector<uint8_t> ve = convEncode(x_[size_t(p)], L.sch[size_t(p)]);
        std::vector<uint8_t> ye(ve.size());
        if (t == 0) ye = ve;
        else {
            for (int i = 0; i < 2 * L.n1; ++i) ye[size_t(i)] = ve[size_t(pa[size_t(i)])];
            for (int i = 0; i < 2 * L.n2; ++i) ye[size_t(2 * L.n1 + i)] = ve[size_t(2 * L.n1 + pb[size_t(i)])];
        }
        const uint8_t bit = uint8_t(1u << p);
        for (int i = 0; i < ncell; ++i) {
            hardI_[size_t(i)] = uint8_t((hardI_[size_t(i)] & ~bit) | (ye[size_t(2 * i)] << p));
            hardQ_[size_t(i)] = uint8_t((hardQ_[size_t(i)] & ~bit) | (ye[size_t(2 * i + 1)] << p));
        }
    }
    /** Two candidates only: r with bit p = 0 and = 1, every other bit as decided. */
    static float pairLlr(int levels, int p, float y, uint8_t r) {
        const int r0 = r & ~(1 << p), r1 = r0 | (1 << p);
        const float d0 = y - axisValue(levels, r0), d1 = y - axisValue(levels, r1);
        return d1 * d1 - d0 * d0;
    }
    std::vector<float> yI_, yQ_, w_, llrI_, llrQ_;
    std::vector<uint8_t> hardI_, hardQ_;
    std::vector<std::vector<uint8_t>> x_;
    Viterbi vit_;
};

/** ★ The MSC cell interleaver (§7.6): Π over N_MUX with t0 = 5, and for long interleaving each output cell i taken from
 *  the frame D-later by Γ(i) = i mod D. The DEinterleaver below undoes it with a D-frame history. */
class CellDeinterleaver {
public:
    void reset(int nMux, int depth) {
        n_ = nMux; d_ = depth; perm_ = permutation(nMux, 5);
        histZ_.assign(size_t(d_), std::vector<std::complex<float>>(size_t(n_)));
        histW_.assign(size_t(d_), std::vector<float>(size_t(n_), 0.0f));
        filled_ = 0; head_ = 0;
    }
    int nMux() const { return n_; }
    int depth() const { return d_; }
    /** Push the received (interleaved) frame ẑ_m; returns true and fills z/w with the deinterleaved frame z_{m−D+1} once
     *  D frames are in. */
    bool push(const std::complex<float>* zh, const float* wh, std::vector<std::complex<float>>& z, std::vector<float>& w) {
        std::copy(zh, zh + n_, histZ_[size_t(head_)].begin());
        std::copy(wh, wh + n_, histW_[size_t(head_)].begin());
        head_ = (head_ + 1) % d_;
        if (filled_ < d_) ++filled_;
        if (filled_ < d_) return false;
        // the oldest slot holds ẑ_{m−D+1}; slot for ẑ_{m−D+1+g} is (head_ + g) % d_
        z.assign(size_t(n_), {}); w.assign(size_t(n_), 0.0f);
        for (int i = 0; i < n_; ++i) {
            const int g = d_ == 1 ? 0 : i % d_;
            const int slot = (head_ + g) % d_;
            z[size_t(perm_[size_t(i)])] = histZ_[size_t(slot)][size_t(i)];
            w[size_t(perm_[size_t(i)])] = histW_[size_t(slot)][size_t(i)];
        }
        return true;
    }
private:
    int n_ = 0, d_ = 1, filled_ = 0, head_ = 0;
    std::vector<int> perm_;
    std::vector<std::vector<std::complex<float>>> histZ_;
    std::vector<std::vector<float>> histW_;
};

/** The matching interleaver for the test transmitter. */
class CellInterleaver {
public:
    void reset(int nMux, int depth) {
        n_ = nMux; d_ = depth; perm_ = permutation(nMux, 5);
        hist_.assign(size_t(d_), std::vector<std::complex<float>>(size_t(n_), std::complex<float>(0, 0)));
        head_ = 0;
    }
    std::vector<std::complex<float>> push(const std::vector<std::complex<float>>& z) {
        hist_[size_t(head_)] = z;                       // slot head_ = frame n
        std::vector<std::complex<float>> o(static_cast<size_t>(n_));
        for (int i = 0; i < n_; ++i) {
            const int g = d_ == 1 ? 0 : i % d_;         // take from frame n − g
            const int slot = ((head_ - g) % d_ + d_) % d_;
            o[size_t(i)] = hist_[size_t(slot)][size_t(perm_[size_t(i)])];
        }
        head_ = (head_ + 1) % d_;
        return o;
    }
private:
    int n_ = 0, d_ = 1, head_ = 0;
    std::vector<int> perm_;
    std::vector<std::vector<std::complex<float>>> hist_;
};

}  // namespace vibedrm
