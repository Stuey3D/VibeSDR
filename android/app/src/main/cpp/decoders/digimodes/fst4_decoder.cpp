// decoders/digimodes/fst4_decoder.cpp — FST4 / FST4W, our own receiver. Design in fst4_decoder.h; frame in fst4_code.h.
#include "fst4_decoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <functional>

#include "vibedsp/vibedsp.h"
#include "vibedsp/third_party/pffft/pffft.h"
#include "q65_simd.h"

namespace vibe {
namespace fst4 {

namespace {
const Period kPeriods[] = {
    {15, 720, 0.5, 160, 0},
    {30, 1680, 1.0, 384, 0},
    {60, 3888, 1.0, 864, 0},
    {120, 8200, 1.0, 1800, 192},
    {300, 21504, 1.0, 4800, 480},
    {900, 66560, 1.0, 0, 1440},
    {1800, 134400, 1.0, 0, 2880},
};
using cf = std::complex<float>;
double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
float* falloc(size_t n) { float* p = (float*)pffft_aligned_malloc(n * sizeof(float)); if (p) std::memset(p, 0, n * sizeof(float)); return p; }
constexpr double kTwoPi = 6.283185307179586;

/** (ar, ai) = Σ z[n]·(cr[n] + j·ci[n]) over n (a multiple of 4), four lanes, (l0+l1)+(l2+l3) on every path. */
inline void cdot(const cf* z, const float* cr, const float* ci, int n, float& ar, float& ai) {
    using namespace q65v;
    const float* zf = reinterpret_cast<const float*>(z);
    V accR = set1(0.f), accI = set1(0.f);
    for (int i = 0; i < n; i += 4) {
        V vr, vi; ld2(zf + 2 * i, vr, vi);
        const V r = ld(cr + i), m = ld(ci + i);
        accR = add(accR, sub(mul(vr, r), mul(vi, m)));
        accI = add(accI, add(mul(vr, m), mul(vi, r)));
    }
    ar = hsum(accR); ai = hsum(accI);
}
/** Twiddles e^{−j2π·cyc·n}, n = 0..N−1. */
void twiddle(double cyc, int N, float* cr, float* ci) {
    for (int n = 0; n < N; ++n) { const double ph = -kTwoPi * cyc * n; cr[n] = (float)std::cos(ph); ci[n] = (float)std::sin(ph); }
}
/** cdot for any n (the tail one lane at a time, in the same order on every path). */
inline void cdotAny(const cf* z, const float* cr, const float* ci, int n, float& ar, float& ai) {
    const int n4 = n & ~3;
    ar = 0; ai = 0;
    if (n4) cdot(z, cr, ci, n4, ar, ai);
    for (int i = n4; i < n; ++i) {
        const float zr = z[i].real(), zi = z[i].imag();
        const float pr = zr * cr[i] - zi * ci[i], pi = zr * ci[i] + zi * cr[i];
        ar += pr; ai += pi;
    }
}
/** ★ FST4's frequency pulse: a one-symbol rectangle through a Gaussian of BT = 1 (the GFSK the protocol sends). */
const double kFst4Bt = std::getenv("FST4_TBT") ? std::atof(std::getenv("FST4_TBT")) : 1.0;
/** The 64 (previous, this, next) templates of one symbol, integrated over its 32 sub-blocks, relative to the tones'
 *  centre (1.5 baud): e^{j2π(a·G(u+1) + b·G(u) + c·G(u−1) − 1.5u)}, G = the pulse's running integral. */
struct TrellisTables {
    std::vector<double> energy;            // Σ_j |T_j|², per triple
    mutable std::mutex m;
    mutable std::vector<std::pair<int, std::vector<cf>>> byNs;
    const std::vector<cf>& forNs(int Ns, double bt) const {
        std::lock_guard<std::mutex> lk(m);
        for (auto& e : byNs) if (e.first == Ns) return e.second;
        constexpr int J = 32;
        const double sig = std::sqrt(std::log(2.0)) / (kTwoPi * bt);
        auto Phi = [](double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
        auto I = [&](double x) { return x * Phi(x / sig) + sig * std::exp(-0.5 * x * x / (sig * sig)) / std::sqrt(kTwoPi); };
        auto G = [&](double u) { return I(u) - I(u - 1); };
        std::vector<cf> t((size_t)64 * J);
        std::vector<double> en(64, 0.0);
        for (int h = 0; h < 64; ++h) {
            const int a = h / 16, b = (h / 4) % 4, c = h % 4;
            for (int j = 0; j < J; ++j) {
                const int m0 = j * Ns / J, m1 = (j + 1) * Ns / J;
                std::complex<double> acc(0, 0);
                for (int m = m0; m < m1; ++m) {
                    const double u = (double)m / Ns;
                    const double ph = kTwoPi * (a * G(u + 1) + b * G(u) + c * G(u - 1) - 1.5 * u);
                    acc += std::complex<double>(std::cos(ph), std::sin(ph));
                }
                t[(size_t)h * J + (size_t)j] = cf((float)acc.real(), (float)acc.imag());
                en[(size_t)h] += std::norm(acc);
            }
        }
        // ★ energies are kept per Ns too (the last Ns asked for is the one in use — one SlotDecoder, one Ns)
        const_cast<TrellisTables*>(this)->energy = en;
        byNs.emplace_back(Ns, std::move(t));
        return byNs.back().second;
    }
};
const TrellisTables& trellisTables() { static TrellisTables t; return t; }

inline float li0(float x) { return x < 3.75f ? std::log1p(x * x * 0.25f + x * x * x * x * (1.0f / 64)) : x - 0.5f * std::log(6.2831853f * x); }
} // namespace

const uint8_t* g_truthTones = nullptr;
const Period* periodFor(int sec) { for (const Period& p : kPeriods) if (p.sec == sec) return &p; return nullptr; }

std::string Result::modeName() const { return std::string(mode == Mode::FST4W ? "FST4W-" : "FST4-") + std::to_string(periodSec); }

// ── The decoder of one window ──────────────────────────────────────────────────────────────────────────────────────
SlotDecoder::SlotDecoder(Mode m, int periodSec, double centreHz) : mode_(m), fc_(centreHz) {
    p_ = periodFor(periodSec);
    if (!p_) return;
    ns_ = m == Mode::FST4W ? p_->ns4w : p_->ns4;
    if (ns_ <= 0) return;
    rate_ = (double)ns_ * kRate / p_->nsps;
    baud_ = (double)kRate / p_->nsps;
    nfft_ = 2 * ns_;
    hop_ = ns_ / 4;
    setup_ = pffft_new_setup(nfft_, PFFFT_COMPLEX);
    fin_ = falloc(2 * (size_t)nfft_); fout_ = falloc(2 * (size_t)nfft_); fwork_ = falloc(2 * (size_t)nfft_);
    if (!setup_) ns_ = 0;
}
SlotDecoder::~SlotDecoder() {
    if (setup_) pffft_destroy_setup((PFFFT_Setup*)setup_);
    pffft_aligned_free(fin_); pffft_aligned_free(fout_); pffft_aligned_free(fwork_);
}

int SlotDecoder::decode(const cf* z, int n, const SlotOptions& o, std::vector<Result>& out, SlotStats* stOut) {
    SlotStats st;
    const double t0ms = nowMs();
    if (!ok()) return 0;
    const size_t first = out.size();
    seen_.clear();
    // signed half-baud bins covering tone 0 in [lo, hi − 3 baud] and its three tones
    const double bh = baud_ / 2;
    const int sbLo = (int)std::ceil((o.loHz - fc_) / bh), sbHi = (int)std::floor((o.hiHz - 3 * baud_ - fc_) / bh);
    if (sbHi <= sbLo || sbLo <= -ns_ || sbHi + 6 >= ns_) return 0;
    const int nb = sbHi - sbLo + 1, nr = nb + 6;                     // row: tone-0 bins plus the three tones above
    const int nFrames = (n - ns_) / hop_ + 1;
    if (nFrames < 4 * kSymbols) return 0;
    P_.assign((size_t)nFrames * nr, 0.f);
    for (int f = 0; f < nFrames; ++f) {
        std::memset(fin_, 0, 2 * (size_t)nfft_ * sizeof(float));
        std::memcpy(fin_, z + (size_t)f * hop_, (size_t)ns_ * sizeof(cf));
        pffft_transform_ordered((PFFFT_Setup*)setup_, fin_, fout_, fwork_, PFFFT_FORWARD);
        float* row = &P_[(size_t)f * nr];
        for (int k = 0; k < nr; ++k) {
            int b = sbLo + k; if (b < 0) b += nfft_;
            row[k] = fout_[2 * b] * fout_[2 * b] + fout_[2 * b + 1] * fout_[2 * b + 1];
        }
    }
    {   // per-bin noise: the median over the window
        std::vector<float> col(nFrames);
        for (int k = 0; k < nr; ++k) {
            for (int f = 0; f < nFrames; ++f) col[f] = P_[(size_t)f * nr + k];
            std::nth_element(col.begin(), col.begin() + nFrames / 2, col.end());
            const float med = col[nFrames / 2], s = med > 0 ? 0.6931472f / med : 0.f;
            for (int f = 0; f < nFrames; ++f) P_[(size_t)f * nr + k] *= s;
        }
    }
    // ── sync search: Σ over sync symbols of (4/3)·P(expected tone) − (1/3)·Σ four tones, SIMD over bins ──
    const double fps = rate_ / hop_;
    const int nominal = (int)std::lround((kWinLeadSec + p_->lead) * fps);
    const int tMin = std::max(0, nominal - (int)std::lround(1.0 * fps));
    const int tMax = std::min(nFrames - 1 - 4 * (kSymbols - 1), nominal + (int)std::lround(1.5 * fps));
    const int nb4 = (nb + 3) & ~3;
    std::vector<float> acc(nb4), best(nb4, -1e9f), s4(nb4 + 8);
    std::vector<int> bestT(nb4, 0);
    const float k43 = 4.0f / 3.0f / 40.0f, k13 = 1.0f / 3.0f / 40.0f;
    for (int t0 = tMin; t0 <= tMax; ++t0) {
        std::fill(acc.begin(), acc.end(), 0.f);
        for (int i = 0; i < kSymbols; ++i) {
            if (!isSync(i)) continue;
            const float* row = &P_[(size_t)(t0 + 4 * i) * nr];
            const int tn = syncTone(i);
            int b = 0;
            for (; b + 4 <= nb; b += 4) {
                using namespace q65v;
                const V s = add(add(ld(row + b), ld(row + b + 2)), add(ld(row + b + 4), ld(row + b + 6)));
                q65v::st(acc.data() + b, add(ld(acc.data() + b), sub(mul(ld(row + b + 2 * tn), set1(k43)), mul(s, set1(k13)))));
            }
            for (; b < nb; ++b) {
                const float s = (row[b] + row[b + 2]) + (row[b + 4] + row[b + 6]);
                acc[b] = acc[b] + (row[b + 2 * tn] * k43 - s * k13);
            }
        }
        for (int b = 0; b < nb; ++b) if (acc[b] > best[b]) { best[b] = acc[b]; bestT[b] = t0; }
    }
    std::vector<Cand> cands;
    for (int b = 0; b < nb; ++b) {
        if (best[b] < o.syncMin) continue;
        bool peak = true;
        for (int d = -3; d <= 3 && peak; ++d) if (d && b + d >= 0 && b + d < nb && best[b + d] > best[b]) peak = false;
        if (peak) cands.push_back({sbLo + b, bestT[b], best[b]});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    const int cap = o.loaded ? std::max(4, o.maxCands / 2) : o.maxCands;
    if ((int)cands.size() > cap) cands.resize(cap);
    st.candidates = (int)cands.size();
    for (const Cand& c : cands) {
        if (o.abort && o.abort->load()) break;
        tryCand(z, n, c, o, out, st);
    }
    st.totalMs = nowMs() - t0ms;
    if (stOut) *stOut = st;
    return (int)(out.size() - first);
}

bool SlotDecoder::tryCand(const cf* z, int n, const Cand& c, const SlotOptions& o, std::vector<Result>& out, SlotStats& st) {
    const int Ns = ns_;
    std::vector<float> cr(4 * Ns), ci(4 * Ns), wr(Ns), wi(Ns);
    // tone k (and off-signal noise tones) at baseband frequency (sb·½ + k + δ) baud → cycles/sample (… )/Ns
    auto symPower = [&](int a, double cyc) {
        if (a < 0 || a + Ns > n) return 0.0f;
        twiddle(cyc, Ns, wr.data(), wi.data());
        float ar, ai; cdot(z + a, wr.data(), wi.data(), Ns, ar, ai);
        return ar * ar + ai * ai;
    };
    (void)symPower;
    const double sb0 = c.bin * 0.5;          // tone 0, in baud from the band centre
    int bestN0 = c.frame * hop_; double bestDf = 0;
    auto stage = [&](int nc, double fc, int tHalf, int tStep, int fHalf, double fStep) {
        double bs = -1; int bn = nc; double bdf = fc;
        for (int fi = -fHalf; fi <= fHalf; ++fi) {
            const double df = fc + fi * fStep;
            for (int t = 0; t < 4; ++t) twiddle((sb0 + t + df) / Ns, Ns, &cr[(size_t)t * Ns], &ci[(size_t)t * Ns]);
            for (int ti = -tHalf; ti <= tHalf; ++ti) {
                const int n0 = nc + ti * tStep;
                double sc = 0;
                for (int i = 0; i < kSymbols; ++i) {
                    if (!isSync(i)) continue;
                    const int a = n0 + i * Ns;
                    if (a < 0 || a + Ns > n) continue;
                    const int t = syncTone(i);
                    float ar, ai; cdot(z + a, &cr[(size_t)t * Ns], &ci[(size_t)t * Ns], Ns, ar, ai);
                    sc += (double)ar * ar + (double)ai * ai;
                }
                if (sc > bs) { bs = sc; bn = n0; bdf = df; }
            }
        }
        bestN0 = bn; bestDf = bdf;
    };
    stage(bestN0, 0.0, 8, std::max(1, Ns / 32), 4, 1.0 / 8);
    stage(bestN0, bestDf, 2, std::max(1, Ns / 96), 4, 1.0 / 32);
    // ── fine frequency, COHERENTLY: each sync group is eight consecutive known tones with continuous phase (h = 1:
    //    a whole number of cycles a symbol), so |Σ of its eight symbol values|² peaks only at the right frequency —
    //    ±1/32 baud in 1/256ths. Multi-symbol detection below needs that accuracy (a 4-symbol block drifts 2π·4·Δf). ──
    std::vector<cf> X((size_t)kSymbols * 4);          // complex symbol values, ABSOLUTE phase reference
    auto symbolValues = [&](double df, bool all) {
        for (int t = 0; t < 4; ++t) twiddle((sb0 + t + df) / Ns, Ns, &cr[(size_t)t * Ns], &ci[(size_t)t * Ns]);
        for (int i = 0; i < kSymbols; ++i) {
            if (!all && !isSync(i)) continue;
            const int a0 = bestN0 + i * Ns;
            for (int t = 0; t < 4; ++t) {
                if (!all && t != syncTone(i)) continue;
                float ar = 0, ai = 0;
                if (a0 >= 0 && a0 + Ns <= n) cdot(z + a0, &cr[(size_t)t * Ns], &ci[(size_t)t * Ns], Ns, ar, ai);
                // ★ rotate to the absolute reference: the twiddle started at this symbol's first sample
                const double ph = -kTwoPi * (sb0 + t + df) / Ns * (double)(i * Ns);
                X[(size_t)i * 4 + t] = cf(ar, ai) * cf((float)std::cos(ph), (float)std::sin(ph));
            }
        }
    };
    {
        double bs = -1, bdf = bestDf;
        for (int fi = -8; fi <= 8; ++fi) {
            const double df = bestDf + fi / 256.0;
            symbolValues(df, false);
            double sc = 0;
            for (int g = 0; g < 5; ++g) {
                const int g0 = 38 * g;
                cf acc(0, 0);
                for (int k = 0; k < 8; ++k) acc += X[(size_t)(g0 + k) * 4 + syncTone(g0 + k)];
                sc += std::norm(acc);
            }
            if (sc > bs) { bs = sc; bdf = df; }
        }
        bestDf = bdf;
    }
    symbolValues(bestDf, true);
    // ── noise: eight tone positions clear of the signal (−7…−4 and +7…+10 baud) ──
    std::vector<float> noise; noise.reserve(kSymbols * 8);
    {
        std::vector<float> nr_(8 * (size_t)Ns), ni_(8 * (size_t)Ns);
        const int offs[8] = {-7, -6, -5, -4, 7, 8, 9, 10};
        for (int q = 0; q < 8; ++q) twiddle((sb0 + offs[q] + bestDf) / Ns, Ns, &nr_[(size_t)q * Ns], &ni_[(size_t)q * Ns]);
        for (int i = 0; i < kSymbols; ++i) {
            const int a0 = bestN0 + i * Ns;
            if (a0 < 0 || a0 + Ns > n) continue;
            for (int q = 0; q < 8; ++q) { float ar, ai; cdot(z + a0, &nr_[(size_t)q * Ns], &ni_[(size_t)q * Ns], Ns, ar, ai); noise.push_back(ar * ar + ai * ai); }
        }
    }
    if (noise.size() < 64) return false;
    std::nth_element(noise.begin(), noise.begin() + noise.size() / 2, noise.end());
    const float n0 = noise[noise.size() / 2] / 0.6931472f;
    if (!(n0 > 0)) return false;
    const float sn = 1.0f / std::sqrt(n0);
    float E[kSymbols][4];
    double rhoS = 0;
    for (int i = 0; i < kSymbols; ++i) {
        for (int t = 0; t < 4; ++t) { X[(size_t)i * 4 + t] *= sn; E[i][t] = std::norm(X[(size_t)i * 4 + t]); }
        if (isSync(i)) rhoS += E[i][syncTone(i)] - 1.0;
    }
    rhoS /= 40.0;
    const float rho = (float)std::max(0.5, rhoS);
    const float k2 = 2.0f * std::sqrt(rho);
    // ── bit log-likelihoods, log P(1)/P(0), from blocks of nb consecutive data symbols detected TOGETHER (coherent
    //    within a block: a tone sequence's metric is ln I0(2·√ρ·|Σ symbol values|)), nb = 1, 2, 4 — three views of the
    //    same channel; belief propagation tries each. Gray: bit 0 is 1 for tones 2, 3; bit 1 for tones 1, 2. ──
    static const uint8_t kBit0[4] = {0, 0, 1, 1}, kBit1[4] = {0, 1, 1, 0};
    int dataPos[kData]; for (int i = 0, d = 0; i < kSymbols; ++i) if (!isSync(i)) dataPos[d++] = i;
    std::vector<float> met(256);
    auto lse = [](float a, float e) { return a < -1e29f ? e : std::max(a, e) + std::log1p(std::exp(-std::fabs(a - e))); };
    auto blockLlr = [&](int nb, float* llr) {
        for (int d0 = 0; d0 < kData; ) {
            const int runEnd = (d0 / 30 + 1) * 30;           // a block never spans a sync group (the data runs are 30)
            const int m = std::min(nb, runEnd - d0);
            const int H = 1 << (2 * m);
            float lp1[8], lp0[8];
            for (int q = 0; q < 2 * m; ++q) { lp1[q] = -1e30f; lp0[q] = -1e30f; }
            float mx = -1e30f;
            for (int h = 0; h < H; ++h) {
                cf acc(0, 0);
                for (int k = 0; k < m; ++k) acc += X[(size_t)dataPos[d0 + k] * 4 + ((h >> (2 * k)) & 3)];
                met[(size_t)h] = li0(k2 * std::abs(acc));
                mx = std::max(mx, met[(size_t)h]);
            }
            for (int h = 0; h < H; ++h) {
                const float e = met[(size_t)h] - mx;
                for (int k = 0; k < m; ++k) {
                    const int t = (h >> (2 * k)) & 3;
                    float& a1 = kBit0[t] ? lp1[2 * k] : lp0[2 * k]; a1 = lse(a1, e);
                    float& b1 = kBit1[t] ? lp1[2 * k + 1] : lp0[2 * k + 1]; b1 = lse(b1, e);
                }
            }
            for (int q = 0; q < 2 * m; ++q) llr[2 * d0 + q] = lp1[q] - lp0[q];
            d0 += m;
        }
    };
    // ── decode ──
    uint8_t cw[kN];
    Result r;
    int it = 0;
    bool got = false;
    const Code codes[2] = {mode_ == Mode::FST4W ? Code::K74 : Code::K101, Code::K101};
    const int nTry = mode_ == Mode::FST4W ? 2 : 1;
    // ── the COHERENT view: the carrier's phase measured on each sync group (eight known tones, summed coherently),
    //    unwrapped and interpolated across the data between them; each data symbol is then detected with its phase
    //    known — Re(X·e^{−jφ}) — the full coherent gain on a stable channel (ground wave, a steady path). On a
    //    fading one the block views below are the ones that decode. ──
    auto coherentLlr = [&](float* llr) {
        double ph[5]; const double centre[5] = {3.5, 41.5, 79.5, 117.5, 155.5};
        for (int g = 0; g < 5; ++g) {
            cf acc(0, 0);
            for (int k = 0; k < 8; ++k) acc += X[(size_t)(38 * g + k) * 4 + syncTone(38 * g + k)];
            ph[g] = std::arg(acc);
            if (g) { while (ph[g] - ph[g - 1] > M_PI) ph[g] -= 2 * M_PI; while (ph[g] - ph[g - 1] < -M_PI) ph[g] += 2 * M_PI; }
        }
        for (int d = 0; d < kData; ++d) {
            const int i = dataPos[d];
            int g = 0; while (g < 3 && i > centre[g + 1]) ++g;
            const double w = (i - centre[g]) / (centre[g + 1] - centre[g]);
            const double phi = ph[g] + w * (ph[g + 1] - ph[g]);
            const cf rot((float)std::cos(phi), (float)-std::sin(phi));
            float m[4];
            for (int t = 0; t < 4; ++t) m[t] = k2 * (X[(size_t)i * 4 + t] * rot).real();
            float l1 = -1e30f, l0 = -1e30f, h1 = -1e30f, h0 = -1e30f;
            for (int t = 0; t < 4; ++t) {
                if (kBit0[t]) l1 = lse(l1, m[t]); else l0 = lse(l0, m[t]);
                if (kBit1[t]) h1 = lse(h1, m[t]); else h0 = lse(h0, m[t]);
            }
            llr[2 * d] = l1 - l0; llr[2 * d + 1] = h1 - h0;
        }
    };
    // ── the GLOBAL coherent view: one carrier phase for the whole transmission, its frequency pinned by maximising the
    //    COHERENT sum of all 40 sync symbols (±1/64 baud in 1/2048ths — 160 symbols at 1/2048 baud drift < 0.5 rad);
    //    the best view on a stable path. The symbol values are recomputed at that frequency. ──
    std::vector<cf> Xg;
    double dfGlobal = bestDf;
    /* ★ (V12 wave 2) the coherent sum of the five sync groups has GRATING LOBES 1/38 baud apart (the groups are 38
     *  symbols apart); at threshold noise can lift a neighbour over the true one, and then every coherent view is
     *  wrong (≈ 100 of 240 bits). So the search covers ±1/32 baud, keeps its local maxima, and the views are built at
     *  the best lobe and again at the second. */
    std::vector<std::pair<double, double>> lobes;     // (power, df)
    auto globalLlr = [&](float* llr, int lobe, double dfForce) {
        double bdf = bestDf;
        std::vector<cf> keep = X;
        if (lobes.empty() && !std::isfinite(dfForce)) {
            std::vector<double> pw;
            for (int fi = -64; fi <= 64; ++fi) {
                symbolValues(bestDf + fi / 2048.0, false);
                cf acc(0, 0);
                for (int i = 0; i < kSymbols; ++i) if (isSync(i)) acc += X[(size_t)i * 4 + syncTone(i)];
                pw.push_back(std::norm(acc));
            }
            for (int k = 0; k < (int)pw.size(); ++k) {
                const bool peak = (k == 0 || pw[(size_t)k] >= pw[(size_t)k - 1]) && (k + 1 == (int)pw.size() || pw[(size_t)k] > pw[(size_t)k + 1]);
                if (peak) lobes.push_back({pw[(size_t)k], bestDf + (k - 64) / 2048.0});
            }
            std::sort(lobes.begin(), lobes.end(), [](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first > b.first; });
        }
        if (std::isfinite(dfForce)) bdf = dfForce;
        else if (lobe < (int)lobes.size()) bdf = lobes[(size_t)lobe].second;
        dfGlobal = bdf;
        symbolValues(bdf, true);
        for (cf& v : X) v *= sn;
        cf acc(0, 0);
        for (int i = 0; i < kSymbols; ++i) if (isSync(i)) acc += X[(size_t)i * 4 + syncTone(i)];
        const cf rot = std::conj(acc) / std::max(1e-12f, std::abs(acc));
        for (int d = 0; d < kData; ++d) {
            const int i = dataPos[d];
            float m[4];
            for (int t = 0; t < 4; ++t) m[t] = k2 * (X[(size_t)i * 4 + t] * rot).real();
            float l1 = -1e30f, l0 = -1e30f, h1 = -1e30f, h0 = -1e30f;
            for (int t = 0; t < 4; ++t) {
                if (kBit0[t]) l1 = lse(l1, m[t]); else l0 = lse(l0, m[t]);
                if (kBit1[t]) h1 = lse(h1, m[t]); else h0 = lse(h0, m[t]);
            }
            llr[2 * d] = l1 - l0; llr[2 * d + 1] = h1 - h0;
        }
        X = keep;
    };
    /* ── the GFSK-MATCHED view (V12 wave 2): a coherent maximum-a-posteriori detector for the waveform actually sent.
     *    FST4's tones are Gaussian-smoothed (BT = kFst4Bt): each symbol's waveform depends on its neighbours, so a
     *    one-symbol DFT (the views above) both loses energy at every tone change and sees the neighbours as noise.
     *    Here the symbol is correlated with the exact waveform of every (previous, this, next) tone triple — 64
     *    templates, integrated over 32 sub-blocks a symbol — and a forward-backward pass over the 16 (previous, this)
     *    states turns those into each tone's probability, the sync symbols pinned. With h = 1 the phase at a symbol's
     *    start depends only on the triple, so one carrier phase (global, or interpolated between the sync groups when
     *    `piecewise`) holds for every template. ── */
    auto trellisLlr = [&](bool piecewise, float* llr) {
        constexpr int J = 32;
        const TrellisTables& T = trellisTables();
        const double cyc = (sb0 + 1.5 + dfGlobal) / Ns;
        std::vector<float> twr((size_t)Ns), twi((size_t)Ns);
        twiddle(cyc, Ns, twr.data(), twi.data());
        std::vector<int> bj(J + 1); for (int j = 0; j <= J; ++j) bj[(size_t)j] = j * Ns / J;
        // y[i][j]: the symbol's sub-block sums, mixed to the tones' centre, absolute phase, × (−1)^i
        std::vector<cf> y((size_t)kSymbols * J, cf(0, 0));
        for (int i = 0; i < kSymbols; ++i) {
            const int a0 = bestN0 + i * Ns;
            if (a0 < 0 || a0 + Ns > n) continue;
            const double ph0 = -kTwoPi * cyc * (double)a0;
            const cf r0((float)std::cos(ph0), (float)std::sin(ph0));
            for (int j = 0; j < J; ++j) {
                float ar, ai;
                const int m0 = bj[(size_t)j], len = bj[(size_t)j + 1] - m0;
                cdotAny(z + a0 + m0, twr.data() + m0, twi.data() + m0, len, ar, ai);
                cf v = cf(ar, ai) * r0;
                if (i & 1) v = -v;
                y[(size_t)i * J + (size_t)j] = v;
            }
        }
        const std::vector<cf>& tp = T.forNs(Ns, kFst4Bt);
        // Z[i][abc] = Σ_j y·conj(T)
        std::vector<cf> Z((size_t)kSymbols * 64);
        for (int i = 0; i < kSymbols; ++i)
            for (int h = 0; h < 64; ++h) {
                cf acc(0, 0);
                for (int j = 0; j < J; ++j) acc += y[(size_t)i * J + (size_t)j] * std::conj(tp[(size_t)h * J + (size_t)j]);
                Z[(size_t)i * 64 + (size_t)h] = acc;
            }
        auto tri = [](int a, int b, int c) { return a * 16 + b * 4 + c; };
        auto toneAt = [&](int i) { return i < 0 ? -2 : i >= kSymbols ? -3 : isSync(i) ? syncTone(i) : -1; };
        // carrier phase: interior sync symbols (both neighbours sync), their known triple
        double phg[5]; cf tot(0, 0);
        for (int g = 0; g < 5; ++g) {
            cf acc(0, 0);
            for (int k = 1; k < 7; ++k) { const int i = 38 * g + k; acc += Z[(size_t)i * 64 + (size_t)tri(syncTone(i - 1), syncTone(i), syncTone(i + 1))]; }
            phg[g] = std::arg(acc); tot += acc;
            if (g) { while (phg[g] - phg[g - 1] > M_PI) phg[g] -= 2 * M_PI; while (phg[g] - phg[g - 1] < -M_PI) phg[g] += 2 * M_PI; }
        }
        const double phAll = std::arg(tot);
        const double centre[5] = {3.5, 41.5, 79.5, 117.5, 155.5};
        auto thetaAt = [&](int i) {
            if (!piecewise) return phAll;
            int g = 0; while (g < 3 && i > centre[g + 1]) ++g;
            const double w = (i - centre[g]) / (centre[g + 1] - centre[g]);
            return phg[g] + w * (phg[g + 1] - phg[g]);
        };
        // amplitude and noise per sample: n0 is the DFT noise of Ns samples; rho the per-symbol SNR
        const double n0s = n0 / Ns;                                  // per sample
        const double A = std::sqrt(rho * n0) / Ns;
        const double L = (double)Ns / J;
        const double k1 = 2.0 * A / (L * n0s), k2e = A * A / (L * n0s);
        std::vector<float> M((size_t)kSymbols * 64);
        for (int i = 0; i < kSymbols; ++i) {
            const double th = thetaAt(i);
            const cf rot((float)std::cos(th), (float)-std::sin(th));
            for (int h = 0; h < 64; ++h) M[(size_t)i * 64 + (size_t)h] = (float)(k1 * (Z[(size_t)i * 64 + (size_t)h] * rot).real() - k2e * T.energy[(size_t)h]);
        }
        // forward-backward over states (prev, this): α_i(a,b) before symbol i's branch
        const float NEG = -1e30f;
        auto lse2 = [](float a, float b) { if (a < b) std::swap(a, b); return b < -1e29f ? a : a + std::log1p(std::exp(b - a)); };
        std::vector<float> alpha((size_t)(kSymbols + 1) * 16, NEG), beta((size_t)(kSymbols + 1) * 16, NEG);
        auto allowed = [&](int i, int t) { const int s0 = toneAt(i); return s0 < 0 || s0 == t; };
        for (int b = 0; b < 4; ++b) if (allowed(0, b)) alpha[(size_t)(b * 4 + b)] = 0;   // ★ before the start: the first tone
        for (int i = 0; i < kSymbols; ++i)
            for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) {
                const float al = alpha[(size_t)i * 16 + (size_t)(a * 4 + b)];
                if (al <= NEG) continue;
                for (int c = 0; c < 4; ++c) {
                    if (i + 1 < kSymbols ? !allowed(i + 1, c) : c != b) continue;   // ★ after the end: the last tone
                    float& d = alpha[(size_t)(i + 1) * 16 + (size_t)(b * 4 + c)];
                    d = lse2(d, al + M[(size_t)i * 64 + (size_t)tri(a, b, c)]);
                }
            }
        for (int s0 = 0; s0 < 16; ++s0) beta[(size_t)kSymbols * 16 + (size_t)s0] = 0;
        for (int i = kSymbols - 1; i >= 0; --i)
            for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) {
                float acc = NEG;
                for (int c = 0; c < 4; ++c) {
                    if (i + 1 < kSymbols ? !allowed(i + 1, c) : c != b) continue;
                    const float be = beta[(size_t)(i + 1) * 16 + (size_t)(b * 4 + c)];
                    if (be <= NEG) continue;
                    acc = lse2(acc, be + M[(size_t)i * 64 + (size_t)tri(a, b, c)]);
                }
                beta[(size_t)i * 16 + (size_t)(a * 4 + b)] = acc;
            }
        for (int d = 0; d < kData; ++d) {
            const int i = dataPos[d];
            float pt[4] = {NEG, NEG, NEG, NEG};
            for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) {
                const float v = alpha[(size_t)i * 16 + (size_t)(a * 4 + b)] + beta[(size_t)i * 16 + (size_t)(a * 4 + b)];
                if (v > NEG / 2) pt[b] = lse2(pt[b], v);
            }
            float l1 = NEG, l0 = NEG, h1 = NEG, h0 = NEG;
            for (int t = 0; t < 4; ++t) {
                if (kBit0[t]) l1 = lse2(l1, pt[t]); else l0 = lse2(l0, pt[t]);
                if (kBit1[t]) h1 = lse2(h1, pt[t]); else h0 = lse2(h0, pt[t]);
            }
            llr[2 * d] = std::max(-40.f, std::min(40.f, l1 - l0)); llr[2 * d + 1] = std::max(-40.f, std::min(40.f, h1 - h0));
        }
    };
    /* ★★ DECISION-DIRECTED FREQUENCY (V12 wave 2). At threshold the 40 sync symbols alone often pin the wrong
     *  frequency (a grating lobe, or noise): measured at −27 dB, HALF the trials had every coherent view wrong (≈ 120
     *  of 240 bits) while the 4-symbol non-coherent view was ≈ 80 % right. So the 4-symbol view's tone decisions (and
     *  the sync's known tones) make a 160-symbol reference: |Σ X_i[t̂_i]·e^{−j2πδi}|² over δ (±1/16 baud, 1/4096
     *  steps, a parabola) — all 160 symbols, no grating lobes — and the coherent views are rebuilt at that frequency;
     *  then once more from the best coherent view's own decisions. */
    auto ddRefine = [&](const float* ref) {
        static const uint8_t toneOf[4] = {0, 1, 3, 2};     // bits (b0 b1) → tone
        std::vector<cf> v((size_t)kSymbols);
        for (int i = 0, d = 0; i < kSymbols; ++i) {
            int t;
            if (isSync(i)) t = syncTone(i);
            else { t = toneOf[((ref[2 * d] > 0) << 1) | (ref[2 * d + 1] > 0)]; ++d; }
            v[(size_t)i] = X[(size_t)i * 4 + (size_t)t];
        }
        const int H = 256; const double step = 1.0 / 4096;
        std::vector<double> pw((size_t)(2 * H + 1));
        int bk = 0;
        for (int k = -H; k <= H; ++k) {
            const double dlt = k * step;
            const cf w((float)std::cos(-kTwoPi * dlt), (float)std::sin(-kTwoPi * dlt));
            cf rot(1, 0), acc(0, 0);
            for (int i = 0; i < kSymbols; ++i) { acc += v[(size_t)i] * rot; rot *= w; }
            pw[(size_t)(k + H)] = std::norm(acc);
            if (pw[(size_t)(k + H)] > pw[(size_t)(bk + H)]) bk = k;
        }
        double fr = 0;
        if (bk > -H && bk < H) {
            const double a0 = pw[(size_t)(bk + H - 1)], b0 = pw[(size_t)(bk + H)], c0 = pw[(size_t)(bk + H + 1)], den = a0 - 2 * b0 + c0;
            if (den < 0) fr = std::max(-0.5, std::min(0.5, 0.5 * (a0 - c0) / den));
        }
        return bestDf + (bk + fr) * step;
    };
    float llrs[16][kN];
    static const int kEnvNb = std::getenv("FST4_NB") ? std::atoi(std::getenv("FST4_NB")) : -1;   // ★ the bench: one view only (0 piecewise, 9 global, 1/2/4 blocks)
    // ★ the views, made one at a time and each tried before the next is made (most decodes need only the first)
    std::vector<std::function<void(float*)>> views;
    double dd1 = NAN;
    if (kEnvNb >= 0) {
        if (kEnvNb == 0) views.push_back([&](float* l) { coherentLlr(l); });
        else if (kEnvNb == 9) views.push_back([&](float* l) { globalLlr(l, 0, NAN); });
        else if (kEnvNb == 7 || kEnvNb == 8) views.push_back([&](float* l) { globalLlr(l, 0, NAN); trellisLlr(kEnvNb == 8, l); });
        else if (kEnvNb == 6) views.push_back([&](float* l) { blockLlr(4, l); dd1 = ddRefine(l); globalLlr(l, 0, dd1); trellisLlr(false, l); });
        else views.push_back([&, nb = kEnvNb](float* l) { blockLlr(nb, l); });
    } else if (o.loaded) {
        views.push_back([&](float* l) { globalLlr(l, 0, NAN); });
        views.push_back([&](float* l) { blockLlr(4, l); });
    } else {
        views.push_back([&](float* l) { globalLlr(l, 0, NAN); });                       // pins dfGlobal (best lobe)
        views.push_back([&](float* l) { trellisLlr(false, l); });
        views.push_back([&](float* l) { blockLlr(4, l); });
        views.push_back([&](float* l) { dd1 = ddRefine(llrs[2]); globalLlr(l, 0, dd1); });   // decision-directed
        views.push_back([&](float* l) { trellisLlr(false, l); });
        views.push_back([&](float* l) { trellisLlr(true, l); });
        views.push_back([&](float* l) { const double dd2 = ddRefine(llrs[4]); globalLlr(l, 0, dd2); trellisLlr(false, l); });
        views.push_back([&](float* l) { globalLlr(l, 1, NAN); trellisLlr(false, l); });  // the second lobe
        views.push_back([&](float* l) { coherentLlr(l); });
        views.push_back([&](float* l) { blockLlr(2, l); });
        views.push_back([&](float* l) { blockLlr(1, l); });
    }
    const int nViews = (int)views.size();
    int bestUnsat = 1 << 20, bestK = 0, made = 0;
    float postTmp[kN], postBest[kN];
    struct OsdIn { int unsat; int k; std::vector<float> post; };
    std::vector<OsdIn> osdPool;
    static const int kOsdPost = std::getenv("FST4_OSDCH") ? 0 : 1;   // ★ OSD orders by BP's accumulated posteriors (FST4_OSDCH: the channel's)
    for (int k = 0; k < nTry * nViews && !got; ++k) {
        if (k % nTry == 0) { views[(size_t)(k / nTry)](llrs[k / nTry]); made = k / nTry + 1; }
        ++st.attempts;
        const float* llr = llrs[k / nTry];
        int unsat = 0;
        it = bpDecode(codes[k % nTry], llr, o.maxIter, cw, &unsat, postTmp);
        if (!it) {
            if (unsat < bestUnsat) { bestUnsat = unsat; bestK = k; std::memcpy(postBest, postTmp, sizeof postTmp); }
            osdPool.push_back({unsat, k, std::vector<float>(postTmp, postTmp + kN)});
            continue;
        }
        bool plausible = true;
        if (codes[k % nTry] == Code::K74) {
            if (!unpack50(cw, r.text, r.call, r.grid, r.dbm)) continue;
        } else {
            if (!unpack77(cw, false, r.text, r.call, r.grid, plausible)) continue;
            if (!plausible && r.text.empty()) continue;
            unpack77(cw, true, r.text, r.call, r.grid, plausible);
        }
        r.k101 = codes[k % nTry] == Code::K101;
        got = true;
    }
    if (g_truthTones && std::getenv("FST4_VDBG")) {   // ★ the bench's oracle: bit errors per view made vs the sent tones
        static const int inv[4] = {0, 1, 3, 2};
        uint8_t tb[kN]; for (int d = 0; d < kData; ++d) { const int t = g_truthTones[dataPos[d]]; tb[2 * d] = (uint8_t)(inv[t] >> 1); tb[2 * d + 1] = (uint8_t)(inv[t] & 1); }
        std::fprintf(stderr, "oracle bit errors (%s):", got ? "decoded" : "failed ");
        for (int v = 0; v < made; ++v) { int e = 0; for (int j = 0; j < kN; ++j) e += (llrs[v][j] > 0) != (tb[j] != 0); std::fprintf(stderr, " %3d", e); }
        std::fprintf(stderr, "  rho %.2f\n", rho);
    }
    /* ★★ OSD (V12 wave 2): when every view's BP failed, ordered-statistics decoding on the view BP came closest with
     *  (ldpc_small.h: order 1 + pairs among the 16 least reliable information bits). The CRC-24 vets its trials — 2⁻²⁴ a
     *  trial, ≤ 96 trials — and the decode must still unpack to a message people send (FST4: ft8OsdPlausible's rules;
     *  FST4W: a real call and grid), then pass the guards below like any other. Not when loaded. */
    bool viaOsd = false;
    static const int kOsdSpan = std::getenv("FST4_OSDSPAN") ? std::atoi(std::getenv("FST4_OSDSPAN")) : 101;
    if (!got && o.osd && !o.loaded) {
        const Code cc = codes[bestK % nTry];
        std::string t2, c2, g2; int d2 = 0; bool pl = false;
        auto acc = [&](const uint8_t* x) {
            if (cc == Code::K74) return unpack50(x, t2, c2, g2, d2);
            return unpack77(x, false, t2, c2, g2, pl) && pl;
        };
        // ★ the views BP came closest with, best first (FST4_OSDN of them, default 3)
        static const int kOsdN = std::getenv("FST4_OSDN") ? std::atoi(std::getenv("FST4_OSDN")) : 3;
        std::stable_sort(osdPool.begin(), osdPool.end(), [](const OsdIn& a, const OsdIn& b) { return a.unsat < b.unsat; });
        bool okOsd = false;
        for (int q = 0; q < (int)osdPool.size() && q < kOsdN && !okOsd; ++q) {
            if (codes[osdPool[(size_t)q].k % nTry] != cc) continue;
            ++st.attempts;
            okOsd = osdDecode(cc, kOsdPost ? osdPool[(size_t)q].post.data() : llrs[osdPool[(size_t)q].k / nTry], kOsdSpan, acc, cw);
        }
        if (std::getenv("FST4_VDBG")) std::fprintf(stderr, "  OSD (best unsat %d): %s\n", bestUnsat, okOsd ? "decoded" : "nothing");
        if (okOsd) {
            if (cc == Code::K74) unpack50(cw, r.text, r.call, r.grid, r.dbm);
            else unpack77(cw, true, r.text, r.call, r.grid, pl);
            r.k101 = cc == Code::K101;
            got = viaOsd = true; it = 0;
        }
    }
    if (!got) return false;
    // one copy per message
    const int K = r.k101 ? 77 : 50;
    std::vector<uint8_t> key(cw, cw + K); key.push_back(r.k101);
    for (size_t q = 0; q + key.size() <= seen_.size(); q += key.size())
        if (std::equal(key.begin(), key.end(), seen_.begin() + (long)q)) return true;
    seen_.insert(seen_.end(), key.begin(), key.end());
    uint8_t tones[kSymbols]; codewordToTones(cw, tones);
    double sig = 0, sc = 0; int cnt[4] = {0, 0, 0, 0};
    for (int i = 0; i < kSymbols; ++i) { sig += E[i][tones[i]] - 1.0; if (!isSync(i)) { sc += E[i][tones[i]]; ++cnt[tones[i]]; } }
    sc /= kData;
    // ★ GUARDS beyond the CRC: a codeword that is mostly one tone is a carrier (the all-zero message is every data
    //   symbol on tone 0); and the decoded tones must stand out of the noise (mean normalised energy, noise ≈ 1).
    if (*std::max_element(cnt, cnt + 4) > 60 || sc < kScoreMin) { seen_.resize(seen_.size() - key.size()); ++st.rejected; return false; }
    const double rhoAll = std::max(1e-3, sig / kSymbols);
    r.mode = mode_; r.periodSec = p_->sec;
    r.f0 = (float)(fc_ + (sb0 + bestDf) * baud_);
    r.dt = (float)(bestN0 / rate_ - kWinLeadSec - p_->lead);
    r.snr = (int)std::lround(10.0 * std::log10(rhoAll * baud_ / 2500.0));
    r.iters = it; r.sync = c.score; r.osd = viaOsd;
    out.push_back(r);
    ++st.decodes;
    return true;
}

// ── The stream ─────────────────────────────────────────────────────────────────────────────────────────────────────
Stream::Stream(Mode m, const Period& p, double centreHz) {
    const int ns = m == Mode::FST4W ? p.ns4w : p.ns4;
    const int D1 = m == Mode::FST4W ? 8 : 2;
    rate_ = (double)ns * kRate / p.nsps;
    auto build = [&](std::unique_ptr<vibedsp::NCO>& nco, std::unique_ptr<vibedsp::FirDecimator>& dec,
                     std::unique_ptr<vibedsp::RationalResampler>& rr, std::unique_ptr<vibedsp::RationalResampler>& ri, double fc) {
        nco.reset(new vibedsp::NCO(fc / kRate));
        const double cut = m == Mode::FST4W ? 0.0375 : 0.21, tr = m == Mode::FST4W ? 0.02 : 0.05;
        dec.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(cut, tr, true), D1));
        // ★ the ratio Ns·D1 / nsps exactly: the resampler is given those as its "rates"
        rr.reset(new vibedsp::RationalResampler(p.nsps, ns * D1));
        ri.reset(new vibedsp::RationalResampler(p.nsps, ns * D1));
    };
    build(nco_, dec_, rr_, ri_, centreHz);
    // ★ the chain's delay: an impulse through an identical chain (mixed at 0 Hz), its response's peak
    {
        std::unique_ptr<vibedsp::NCO> n2; std::unique_ptr<vibedsp::FirDecimator> d2; std::unique_ptr<vibedsp::RationalResampler> r2, i2;
        build(n2, d2, r2, i2, 0.0);
        const int L = std::max(4 * p.nsps, 20000);
        std::vector<cf> x((size_t)L, cf(0, 0)), y((size_t)L / D1 + 8);
        x[(size_t)L / 4] = cf(1, 0);
        const int m1 = d2->process(x.data(), L, y.data());
        std::vector<float> re((size_t)m1), out((size_t)r2->maxOut(m1));
        for (int i = 0; i < m1; ++i) re[(size_t)i] = y[(size_t)i].real();
        const int m2 = r2->process(re.data(), m1, out.data());
        int pk = 0; for (int i = 1; i < m2; ++i) if (std::fabs(out[(size_t)i]) > std::fabs(out[(size_t)pk])) pk = i;
        double frac = 0;
        if (pk > 0 && pk + 1 < m2) { const double a = out[(size_t)pk - 1], b = out[(size_t)pk], c2 = out[(size_t)pk + 1]; const double den = a - 2 * b + c2; if (den != 0) frac = 0.5 * (a - c2) / den; }
        delay_ = pk + frac - (double)(L / 4) * rate_ / kRate;
    }
}
Stream::~Stream() = default;

void Stream::stage1(const float* audio, int n, std::vector<cf>& d) {
    d.clear();
    if (n <= 0) return;
    a_.resize((size_t)n); b_.resize((size_t)n);
    for (int i = 0; i < n; ++i) a_[(size_t)i] = cf(audio[i], 0.f);
    nco_->mix(a_.data(), b_.data(), n);
    d.resize((size_t)dec_->maxOut(n));
    d.resize((size_t)dec_->process(b_.data(), n, d.data()));
}

void Stream::stage2(const cf* d, int m, std::vector<cf>& out) {
    re_.resize((size_t)m); im_.resize((size_t)m);
    for (int i = 0; i < m; ++i) { re_[(size_t)i] = d[i].real(); im_[(size_t)i] = d[i].imag(); }
    ore_.resize((size_t)rr_->maxOut(m)); oim_.resize((size_t)ri_->maxOut(m));
    const int k1 = rr_->process(re_.data(), m, ore_.data());
    const int k2 = ri_->process(im_.data(), m, oim_.data());
    const int k = std::min(k1, k2);
    for (int i = 0; i < k; ++i) out.push_back(cf(ore_[(size_t)i], oim_[(size_t)i]));
}

void Stream::push(const float* audio, int n, std::vector<cf>& out) {
    std::vector<cf> d;
    stage1(audio, n, d);
    stage2(d.data(), (int)d.size(), out);
}

// ── The live decoder ───────────────────────────────────────────────────────────────────────────────────────────────
LiveDecoder::LiveDecoder(Mode m, std::vector<int> periods, double centreHz, SlotOptions opts) : mode_(m), opts_(opts) {
    for (int s : periods) {
        const Period* p = periodFor(s);
        if (!p || (m == Mode::FST4W ? p->ns4w : p->ns4) <= 0) continue;
        Per q;
        q.dec.reset(new SlotDecoder(m, s, centreHz));
        q.st.reset(new Stream(m, *p, centreHz));
        q.ring.assign((size_t)(q.dec->windowSamples() + (int)(8 * q.dec->rate())), cf(0, 0));
        per_.push_back(std::move(q));
    }
    th_ = std::thread([this] { worker(); });
}
LiveDecoder::~LiveDecoder() {
    abort_.store(true);
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}
void LiveDecoder::drain() { std::unique_lock<std::mutex> lk(m_); idle_.wait(lk, [this] { return q_.empty() && !busy_; }); }

void LiveDecoder::process(const float* audio, int n, double captureUtc) {
    if (n <= 0) return;
    if (std::isfinite(captureUtc)) {
        const double est = captureUtc - (double)(audioTotal_ + n) / kRate;
        if (!anchored_ || est < utc0_ - 1e-6 || est - utc0_ > 1.0) {
            const bool jump = anchored_;
            utc0_ = est; anchored_ = true;
            if (jump) for (Per& q : per_) q.next = NAN;
        }
    } else if (!anchored_) return;
    audioTotal_ += n;
    // ★ the mix and the first decimation are the same for every period of a mode: done ONCE, on the first period's
    //   stream, and only each period's resampler runs per period (FST4: 3 streams ≈ 35 → ≈ 17 ms a minute on the Mac)
    if (!per_.empty()) per_[0].st->stage1(audio, n, d1_);
    for (size_t pi = 0; pi < per_.size(); ++pi) {
        Per& q = per_[pi];
        tmp_.clear();
        q.st->stage2(d1_.data(), (int)d1_.size(), tmp_);
        const size_t R = q.ring.size();
        size_t w = (size_t)(q.total % (int64_t)R);
        for (const cf& v : tmp_) { q.ring[w] = v; if (++w == R) w = 0; }
        q.total += (int64_t)tmp_.size();
        const double rate = q.dec->rate(), P = q.dec->period().sec;
        // UTC of baseband sample k: utc0 + (k − delay)/rate
        const double tEnd = utc0_ + ((double)q.total - q.st->delay()) / rate;
        if (!std::isfinite(q.next)) { double s = std::ceil(tEnd / P) * P; q.next = s; }
        while (tEnd >= q.next + P + kWinTailSec) {
            const double ws = q.next - kWinLeadSec;
            const int W = q.dec->windowSamples();
            const double k0 = (ws - utc0_) * rate + q.st->delay();
            if (k0 >= (double)std::max<int64_t>(0, q.total - (int64_t)R) && k0 >= 0) {
                std::lock_guard<std::mutex> lk(m_);
                if (q.queued) slotsSkipped.fetch_add(1);
                else {
                    Job j; j.pi = (int)pi; j.slotUtc = q.next; j.win.resize((size_t)W);
                    const int64_t s0 = (int64_t)std::llround(k0);
                    size_t r = (size_t)(s0 % (int64_t)R);
                    for (int k = 0; k < W; ++k) { j.win[(size_t)k] = q.ring[r]; if (++r == R) r = 0; }
                    q_.push_back(std::move(j)); q.queued = true;
                    cv_.notify_one();
                }
            }
            q.next += P;
        }
    }
}

void LiveDecoder::worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (stop_) return;
            j = std::move(q_.front()); q_.pop_front(); busy_ = true;
        }
        SlotOptions o = opts_;
        o.loaded = loadProbe ? loadProbe() : false;
        o.abort = &abort_;
        std::vector<Result> res; SlotStats st;
        per_[(size_t)j.pi].dec->decode(j.win.data(), (int)j.win.size(), o, res, &st);
        lastDecodeMs.store(st.totalMs);
        slotsDecoded.fetch_add(1);
        for (Result& r : res) { r.slotUtc = j.slotUtc; if (onResult) onResult(r); }
        { std::lock_guard<std::mutex> lk(m_); per_[(size_t)j.pi].queued = false; busy_ = false; }
        idle_.notify_all();
    }
}

} // namespace fst4
} // namespace vibe
