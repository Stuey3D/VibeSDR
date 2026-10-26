// VibeSDR V12 — the FT8 / FT4 / FT2 decoder's second look at a candidate (see ftx_fine.h).
#include "ftx_fine.h"
#include "ft8/vibe_simd4.h"
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "ft8/constants.h"
#include "../vibedsp/third_party/pffft/pffft.h"
}

namespace vibe {

namespace {
constexpr double kTwoPi = 6.283185307179586;
// the smallest n >= want of the form 2^a 3^b 5^c with 16 | n (a complex PFFFT size)
int pffftComplexSize(int want) {
    for (int n = std::max(16, want);; ++n) {
        if (n % 16) continue;
        int m = n;
        for (int p : { 2, 3, 5 }) while (m % p == 0) m /= p;
        if (m == 1) return n;
    }
}
}

FtxFine::FtxFine(int mode, int rate, int nsps, int window) : mode_(mode), rate_(rate), nsps_(nsps), window_(window) {
    const bool ft8 = mode == 0;
    nn_ = ft8 ? FT8_NN : FT4_NN;
    ntones_ = ft8 ? 8 : 4;
    bps_ = ft8 ? 3 : 2;
    gray_ = ft8 ? kFT8_Gray_map : kFT4_Gray_map;
    T_ = (double)nsps / rate;
    baud_ = 1.0 / T_;
    // ★ 32 baseband samples a symbol for FT8 (200 Hz for a 50 Hz-wide signal), 16 for the 4-tone frames
    sps_ = ft8 ? 32 : 16;
    while (sps_ > 4 && nsps_ % sps_) sps_ /= 2;
    dd_ = nsps_ / sps_;
    nd_ = pffftComplexSize((window + dd_ - 1) / dd_);
    nfft_ = nd_ * dd_;
    df_ = (double)rate / nfft_;
    if (ft8) {
        for (int g = 0; g < 3; ++g)
            for (int k = 0; k < 7; ++k) { syncSym_.push_back(36 * g + k); syncTone_.push_back(kFT8_Costas_pattern[k]); }
        for (int s = 7; s < 36; ++s) dataSym_.push_back(s);
        for (int s = 43; s < 72; ++s) dataSym_.push_back(s);
    } else {
        for (int g = 0; g < 4; ++g)
            for (int k = 0; k < 4; ++k) { syncSym_.push_back(1 + 33 * g + k); syncTone_.push_back(kFT4_Costas_pattern[g][k]); }
        for (int s = 5; s < 34; ++s) dataSym_.push_back(s);
        for (int s = 38; s < 67; ++s) dataSym_.push_back(s);
        for (int s = 71; s < 100; ++s) dataSym_.push_back(s);
    }
    // the band: 1.5 tones below tone 0 to 1.5 above the top tone, raised-cosine over the outer tone each side
    tLo_ = (int)std::floor(-1.5 * baud_ / df_);
    tHi_ = (int)std::ceil((ntones_ + 0.5) * baud_ / df_);
    taper_.resize((size_t)(tHi_ - tLo_ + 1));
    for (int k = tLo_; k <= tHi_; ++k) {
        const double f = k * df_ / baud_;           // in tones above tone 0
        double w = 1.0;
        if (f < -0.5) w = 0.5 - 0.5 * std::cos(M_PI * (f + 1.5));
        else if (f > ntones_ - 0.5) w = 0.5 + 0.5 * std::cos(M_PI * (f - (ntones_ - 0.5)));
        taper_[(size_t)(k - tLo_)] = (float)std::max(0.0, std::min(1.0, w));
    }
    // frequency grid ±6/16 tone; the tone tables at each step
    nDelta_ = 6;
    const int nD = 2 * nDelta_ + 1;
    tabC_.resize((size_t)nD * ntones_ * sps_);
    tabS_.resize(tabC_.size());
    for (int d = 0; d < nD; ++d)
        for (int t = 0; t < ntones_; ++t)
            for (int j = 0; j < sps_; ++j) {
                const double cyc = (t + (d - nDelta_) / 16.0) * j / sps_;
                const size_t i = ((size_t)d * ntones_ + t) * sps_ + j;
                tabC_[i] = (float)std::cos(kTwoPi * cyc);
                tabS_[i] = (float)std::sin(kTwoPi * cyc);
            }
    zr_.assign((size_t)nd_, 0.0f);
    zi_.assign((size_t)nd_, 0.0f);
    csR_.assign((size_t)nn_ * ntones_, 0.0f);
    csI_.assign((size_t)nn_ * ntones_, 0.0f);
    fwd_ = pffft_new_setup(nfft_, PFFFT_REAL);
    inv_ = pffft_new_setup(nd_, PFFFT_COMPLEX);
    spec_ = (float*)pffft_aligned_malloc(sizeof(float) * (size_t)nfft_);
    work_ = (float*)pffft_aligned_malloc(sizeof(float) * (size_t)std::max(nfft_, 2 * nd_));
    bb_ = (float*)pffft_aligned_malloc(sizeof(float) * 2 * (size_t)nd_);
}

FtxFine::~FtxFine() {
    if (fwd_) pffft_destroy_setup((PFFFT_Setup*)fwd_);
    if (inv_) pffft_destroy_setup((PFFFT_Setup*)inv_);
    pffft_aligned_free(spec_);
    pffft_aligned_free(work_);
    pffft_aligned_free(bb_);
}

std::size_t FtxFine::ramBytes() const {
    return sizeof(float) * ((size_t)nfft_ * 2 + 2 * (size_t)nd_ * 2 + tabC_.size() * 2 + csR_.size() * 2 + taper_.size());
}

void FtxFine::ensure(const float* audio, int n) {
    if (have_) return;
    const int m = std::min(n, nfft_);
    std::memcpy(spec_, audio, sizeof(float) * (size_t)m);
    if (m < nfft_) std::memset(spec_ + m, 0, sizeof(float) * (size_t)(nfft_ - m));
    pffft_transform_ordered((PFFFT_Setup*)fwd_, spec_, spec_, work_, PFFFT_FORWARD);
    have_ = true;
}

/* ★ The candidate's baseband: the window-FFT bins around tone 0, edge-tapered, placed at their offset from tone 0's
 *  nearest bin, and transformed back at nd_ points — sample m is audio sample m·dd_. Bins outside the band are zero:
 *  a neighbour two tones beyond the signal's edge is not attenuated, it is ABSENT. */
void FtxFine::extract(double f0) {
    const int i0 = (int)std::lround(f0 / df_);
    std::memset(bb_, 0, sizeof(float) * 2 * (size_t)nd_);
    const int half = nfft_ / 2;
    for (int k = tLo_; k <= tHi_; ++k) {
        const int i = i0 + k;
        if (i <= 0 || i >= half) continue;
        const float w = taper_[(size_t)(k - tLo_)];
        const int at = k < 0 ? nd_ + k : k;
        bb_[2 * at] = w * spec_[2 * i];
        bb_[2 * at + 1] = w * spec_[2 * i + 1];
    }
    pffft_transform_ordered((PFFFT_Setup*)inv_, bb_, bb_, work_, PFFFT_BACKWARD);
    for (int j = 0; j < nd_; ++j) { zr_[(size_t)j] = bb_[2 * j]; zi_[(size_t)j] = bb_[2 * j + 1]; }
}

// one symbol's correlation with one tone table: Σ z[at+j]·e^{-jθ_j}, four lanes at a time
void FtxFine::corr(int at, const float* c, const float* s, float& re, float& im) const {
    if (at < 0 || at + sps_ > nd_) { re = im = 0; return; }
    const float* x = &zr_[(size_t)at];
    const float* y = &zi_[(size_t)at];
    v4f aR = v4_set1(0.0f), aI = v4_set1(0.0f), bR = v4_set1(0.0f), bI = v4_set1(0.0f);
    for (int j = 0; j < sps_; j += 4) {
        const v4f X = v4_load(x + j), Y = v4_load(y + j), C = v4_load(c + j), S = v4_load(s + j);
        aR = v4_madd(X, C, aR); bR = v4_madd(Y, S, bR);      // re = Σ x·c + y·s
        aI = v4_madd(Y, C, aI); bI = v4_madd(X, S, bI);      // im = Σ y·c − x·s
    }
    float t1[4], t2[4], t3[4], t4[4];
    v4_store(t1, aR); v4_store(t2, bR); v4_store(t3, aI); v4_store(t4, bI);
    re = (t1[0] + t1[1]) + (t1[2] + t1[3]) + (t2[0] + t2[1]) + (t2[2] + t2[3]);
    im = (t3[0] + t3[1]) + (t3[2] + t3[3]) - ((t4[0] + t4[1]) + (t4[2] + t4[3]));
}

bool FtxFine::measure(double start, double f0, int minSyncHits, Meas& M) {
    M.nsets = 0;
    extract(f0);
    const int nS = (int)syncSym_.size();
    auto syncE = [&](int m, int d) {
        float e = 0;
        for (int i = 0; i < nS; ++i) {
            const size_t r = ((size_t)d * ntones_ + syncTone_[(size_t)i]) * sps_;
            float re, im; corr(m + syncSym_[(size_t)i] * sps_, &tabC_[r], &tabS_[r], re, im);
            e += re * re + im * im;
        }
        return e;
    };
    // ── fine sync: the start in steps of two, then ±1; the frequency; the start again ±2 ──
    const int m0 = (int)std::lround(start / dd_);
    const int R = sps_ / 2 + 4;
    int bm = m0; float be = -1;
    for (int m = m0 - R; m <= m0 + R; m += 2) { const float e = syncE(m, nDelta_); if (e > be) { be = e; bm = m; } }
    for (int m : { bm - 1, bm + 1 }) { const float e = syncE(m, nDelta_); if (e > be) { be = e; bm = m; } }
    int bd = nDelta_;
    for (int d = 0; d <= 2 * nDelta_; ++d) { if (d == nDelta_) continue; const float e = syncE(bm, d); if (e > be) { be = e; bd = d; } }
    const int c0 = bm;
    for (int m = c0 - 2; m <= c0 + 2; ++m) { if (m == c0) continue; const float e = syncE(m, bd); if (e > be) { be = e; bm = m; } }
    if (be <= 0) return false;
    const double delta = (bd - nDelta_) / 16.0 * baud_;

    // ── the matched filter: a symbol at every tone, phase-referenced to the slot ──
    auto filter = [&](int s) {
        const double ph = -kTwoPi * delta * s * T_;
        const float rc = (float)std::cos(ph), rs = (float)std::sin(ph);
        for (int t = 0; t < ntones_; ++t) {
            const size_t r = ((size_t)bd * ntones_ + t) * sps_;
            float re, im; corr(bm + s * sps_, &tabC_[r], &tabS_[r], re, im);
            csR_[(size_t)s * ntones_ + t] = re * rc - im * rs;
            csI_[(size_t)s * ntones_ + t] = re * rs + im * rc;
        }
    };
    // ── the early out, on the Costas symbols alone: how many are their own strongest tone ──
    int hits = 0; double pin = 0, pout = 0;
    for (int i = 0; i < nS; ++i) {
        const int s = syncSym_[(size_t)i], want = syncTone_[(size_t)i];
        filter(s);
        int best = 0; float bp = -1;
        for (int t = 0; t < ntones_; ++t) {
            const size_t k = (size_t)s * ntones_ + t;
            const float p = csR_[k] * csR_[k] + csI_[k] * csI_[k];
            if (p > bp) { bp = p; best = t; }
            if (t == want) pin += p; else pout += p;
        }
        hits += best == want;
    }
    M.syncHits = hits; M.syncTotal = nS;
    M.syncSnr = (float)(pout > 0 ? (pin / nS) / (pout / (nS * (ntones_ - 1))) : 0);
    M.start = (double)bm * dd_;
    M.f0 = std::lround(f0 / df_) * df_ + delta;
    if (hits < minSyncHits) return false;
    for (int s : dataSym_) filter(s);

    /* ★ The residual frequency from the phase itself: within each Costas group, symbol k+1's correlation leads
     *  symbol k's by 2π·δf·T (both are on known tones, and every tone ends a symbol on a whole cycle). Taken out
     *  symbol by symbol, so a group of three adds in phase. Only a correction smaller than the grid step is trusted. */
    {
        double pr = 0, pi = 0;
        for (int i = 0; i + 1 < nS; ++i) {
            if (syncSym_[(size_t)i + 1] != syncSym_[(size_t)i] + 1) continue;
            const size_t a = (size_t)syncSym_[(size_t)i] * ntones_ + syncTone_[(size_t)i];
            const size_t b = (size_t)syncSym_[(size_t)i + 1] * ntones_ + syncTone_[(size_t)i + 1];
            pr += (double)csR_[b] * csR_[a] + (double)csI_[b] * csI_[a];
            pi += (double)csI_[b] * csR_[a] - (double)csR_[b] * csI_[a];
        }
        const double dr = std::atan2(pi, pr) / (kTwoPi * T_);
        if (std::fabs(dr) < baud_ / 16.0) {
            for (int s = 0; s < nn_; ++s) {
                const double ph = -kTwoPi * dr * s * T_;
                const float rc = (float)std::cos(ph), rs = (float)std::sin(ph);
                for (int t = 0; t < ntones_; ++t) {
                    const size_t k = (size_t)s * ntones_ + t;
                    const float re = csR_[k], im = csI_[k];
                    csR_[k] = re * rc - im * rs; csI_[k] = re * rs + im * rc;
                }
            }
            M.f0 += dr;
        }
    }
    M.nsets = 3;                                         // ★ computed on demand: llrGroup(groupOf(k))
    return true;
}

/* ★ Max-log bit likelihoods over a group of `group` consecutive data symbols: every combination of their values,
 *  judged on |Σ of the chosen tones' complex amplitudes| — one symbol is the plain noncoherent detector; two or three
 *  keep the phase between them. Squared magnitudes are compared, and only the two winners per bit are rooted.
 *  ★ Four lanes at a time where it counts: the last symbol's tones are added to every partial sum as one or two
 *    vectors, and each bit's two maxima are taken over runs of the table (a bit's set and clear halves alternate in
 *    runs of its own weight; the two lowest bits come out of one lane-wise maximum over the whole table). */
void FtxFine::llrGroup(int group, float* llr) const {
    const int nd = (int)dataSym_.size();
    const int seg = 29;                                  // every frame's data runs in segments of 29 symbols
    alignas(16) float accR[1 << 12], accI[1 << 12], pw[1 << 12];
    alignas(16) float gr[8], gi[8];
    const int nt = ntones_;
    for (int s0 = 0; s0 < nd; s0 += seg) {
        for (int q0 = 0; q0 < seg; q0 += group) {
            const int r = std::min(group, seg - q0);
            const int bits = r * bps_, nc = 1 << bits;
            accR[0] = 0; accI[0] = 0;
            int have = 1;
            for (int q = 0; q + 1 < r; ++q) {            // all but the last symbol: the partial sums, from the top
                const int sym = dataSym_[(size_t)(s0 + q0 + q)];
                const float* cr = &csR_[(size_t)sym * nt];
                const float* ci = &csI_[(size_t)sym * nt];
                for (int v = have - 1; v >= 0; --v) {
                    const float br = accR[v], bi = accI[v];
                    for (int d = nt - 1; d >= 0; --d) { accR[v * nt + d] = br + cr[gray_[d]]; accI[v * nt + d] = bi + ci[gray_[d]]; }
                }
                have *= nt;
            }
            {                                            // the last symbol, straight into the powers
                const int sym = dataSym_[(size_t)(s0 + q0 + r - 1)];
                for (int d = 0; d < nt; ++d) { gr[d] = csR_[(size_t)sym * nt + gray_[d]]; gi[d] = csI_[(size_t)sym * nt + gray_[d]]; }
                for (int v = 0; v < have; ++v) {
                    const v4f br = v4_set1(accR[v]), bi = v4_set1(accI[v]);
                    for (int d = 0; d < nt; d += 4) {
                        const v4f xr = v4_add(br, v4_load(gr + d)), xi = v4_add(bi, v4_load(gi + d));
                        v4_store(pw + v * nt + d, v4_madd(xr, xr, v4_mul(xi, xi)));
                    }
                }
            }
            float* L = llr + bps_ * (s0 + q0);
            {                                            // the two lowest bits
                v4f m = v4_set1(0.0f);
                for (int v = 0; v < nc; v += 4) m = v4_max(m, v4_load(pw + v));
                float t[4]; v4_store(t, m);
                L[bits - 1] = std::sqrt(std::max(t[1], t[3])) - std::sqrt(std::max(t[0], t[2]));
                L[bits - 2] = std::sqrt(std::max(t[2], t[3])) - std::sqrt(std::max(t[0], t[1]));
            }
            for (int b = 0; b + 2 < bits; ++b) {
                const int mask = 1 << (bits - 1 - b);
                v4f a0 = v4_set1(0.0f), a1 = v4_set1(0.0f);
                for (int base = 0; base < nc; base += 2 * mask)
                    for (int j = 0; j < mask; j += 4) {
                        a0 = v4_max(a0, v4_load(pw + base + j));
                        a1 = v4_max(a1, v4_load(pw + base + mask + j));
                    }
                float t0[4], t1[4]; v4_store(t0, a0); v4_store(t1, a1);
                const float m0 = std::max(std::max(t0[0], t0[1]), std::max(t0[2], t0[3]));
                const float m1 = std::max(std::max(t1[0], t1[1]), std::max(t1[2], t1[3]));
                L[b] = std::sqrt(m1) - std::sqrt(m0);
            }
        }
    }
    // ★ normalised as ft8_lib normalises its own (unit variance × √24), so the same BP and OSD guards apply
    double sum = 0, sum2 = 0;
    for (int i = 0; i < FTX_LDPC_N; ++i) { sum += llr[i]; sum2 += (double)llr[i] * llr[i]; }
    const double var = (sum2 - sum * sum / FTX_LDPC_N) / FTX_LDPC_N;
    const float k = (float)(var > 1e-30 ? std::sqrt(24.0 / var) : 0.0);
    for (int i = 0; i < FTX_LDPC_N; ++i) llr[i] *= k;
}

} // namespace vibe
