// decoders/digimodes/q65_decoder.cpp — Q65, our own receiver. The design is in q65_decoder.h; the frame in q65_code.h.
#include "q65_decoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vibedsp/third_party/pffft/pffft.h"
#include "q65_simd.h"

namespace vibe {
namespace q65 {

namespace {
const Period kPeriods[] = {
    {15, 1800, 0.5, 120},
    {30, 3600, 0.5, 108},
    {60, 7200, 1.0, 108},
    {120, 16000, 1.0, 96},
    {300, 41472, 1.0, 90},
};
double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
float* falloc(size_t n) { float* p = (float*)pffft_aligned_malloc(n * sizeof(float)); if (p) std::memset(p, 0, n * sizeof(float)); return p; }
PFFFT_Setup* S(void* p) { return (PFFFT_Setup*)p; }
/** A size PFFFT takes: 2^a·3^b·5^c, a multiple of 32 (real) or 16 (complex). */
bool fftSizeOk(int n, bool real) {
    if (n <= 0 || n % (real ? 32 : 16)) return false;
    for (int f : {2, 3, 5}) while (n % f == 0) n /= f;
    return n == 1;
}
/** (ar, ai) = Σ z[n]·(twr[n] + j·twi[n]), z interleaved complex, n a multiple of 4 (72·2^m always is). */
inline void cdot(const float* z, const float* twr, const float* twi, int n, float& ar, float& ai) {
    using namespace q65v;
    V accR = set1(0.f), accI = set1(0.f);
    for (int i = 0; i < n; i += 4) {
        V vr, vi; ld2(z + 2 * i, vr, vi);
        const V cr = ld(twr + i), ci = ld(twi + i);
        accR = add(accR, sub(mul(vr, cr), mul(vi, ci)));
        accI = add(accI, add(mul(vr, ci), mul(vi, cr)));
    }
    ar = hsum(accR); ai = hsum(accI);
}
/** y[n] = z[n]·(twr[n] + j·twi[n]) — the mix to DC, interleaved complex in and out. */
inline void cmix(float* y, const float* z, const float* twr, const float* twi, int n) {
    using namespace q65v;
    for (int i = 0; i < n; i += 4) {
        V vr, vi; ld2(z + 2 * i, vr, vi);
        const V cr = ld(twr + i), ci = ld(twi + i);
        st2(y + 2 * i, sub(mul(vr, cr), mul(vi, ci)), add(mul(vr, ci), mul(vi, cr)));
    }
}
constexpr double kTwoPi = 6.283185307179586;
/** Median of a Gamma(K, 1) sum (the noise in a K-bin energy, unit noise per bin) — Wilson-Hilferty, close enough. */
inline float gammaMedian(int K) { return (float)K - 1.0f / 3.0f + 0.02f / (float)K; }
} // namespace

const Period* periodFor(int sec) {
    for (const Period& p : kPeriods) if (p.sec == sec) return &p;
    return nullptr;
}

std::string Result::mode() const {
    return "Q65-" + std::to_string(periodSec) + std::string(1, (char)('A' + submode));
}

SlotDecoder::SlotDecoder(int periodSec) {
    p_ = periodFor(periodSec);
    if (!p_) return;
    baud_ = (double)kRate / p_->nsps;
    // ★ the search FFT: one symbol zero-padded 2× (half-baud bins) — 4× where 2× is not a size PFFFT takes (15 s)
    //   (PFFFT ASSERTS on a size it cannot do, so sizes are checked here first)
    for (pad_ = 2; pad_ < 16 && !fftSizeOk(pad_ * p_->nsps, true); pad_ *= 2) {}
    nfftS_ = pad_ * p_->nsps;
    if (fftSizeOk(nfftS_, true)) setupS_ = pffft_new_setup(nfftS_, PFFFT_REAL);
    hop_ = p_->nsps / 4;
    const int win = windowSamples(*p_);
    nFrames_ = (win - p_->nsps) / hop_ + 1;
    sIn_ = falloc(nfftS_); sOut_ = falloc(nfftS_); sWork_ = falloc(nfftS_);
    nBig_ = p_->c * p_->nsps;
    if (fftSizeOk(nBig_, true)) setupBig_ = pffft_new_setup(nBig_, PFFFT_REAL);
    bigBuf_ = falloc(nBig_); bigWork_ = falloc(nBig_);
    for (int m = 0; m < 5; ++m) {
        Sub& s = sub_[m];
        s.Ns = 72 << m;
        s.M = p_->c * s.Ns;
        s.nfft = m == 0 ? 144 : s.Ns;
        if (fftSizeOk(s.M, false)) s.setupM = pffft_new_setup(s.M, PFFFT_COMPLEX);
        if (fftSizeOk(s.nfft, false)) s.setupN = pffft_new_setup(s.nfft, PFFFT_COMPLEX);
        s.z = falloc(2 * (size_t)s.M); s.zw = falloc(2 * (size_t)s.M);
        s.y = falloc(2 * (size_t)s.nfft); s.yw = falloc(2 * (size_t)s.nfft);
    }
    E_.assign(kData * 64, 0.f);
    intr_.assign(kData * 64, 0.f);
    pw_.assign(2048, 0.f);
    bp_ = bpScratchNew();
    cqApMask(cqMask_, cqVal_);
    if (!setupS_ || !setupBig_ || !bp_) p_ = nullptr;
}

SlotDecoder::~SlotDecoder() {
    if (setupS_) pffft_destroy_setup(S(setupS_));
    if (setupBig_) pffft_destroy_setup(S(setupBig_));
    for (Sub& s : sub_) {
        if (s.setupM) pffft_destroy_setup(S(s.setupM));
        if (s.setupN) pffft_destroy_setup(S(s.setupN));
        pffft_aligned_free(s.z); pffft_aligned_free(s.zw); pffft_aligned_free(s.y); pffft_aligned_free(s.yw);
    }
    pffft_aligned_free(sIn_); pffft_aligned_free(sOut_); pffft_aligned_free(sWork_);
    pffft_aligned_free(bigBuf_); pffft_aligned_free(bigWork_);
    bpScratchFree(bp_);
}

// ── 1. The search spectrogram ─────────────────────────────────────────────────────────────────────────────────────
void SlotDecoder::spectrogram(const float* win, int n) {
    const int nsps = p_->nsps, step = pad_ / 2;     // half-baud bins = every (pad/2)-th FFT bin
    for (int f = 0; f < nFrames_; ++f) {
        const int a = f * hop_;
        std::memset(sIn_, 0, nfftS_ * sizeof(float));
        const int cnt = std::max(0, std::min(nsps, n - a));
        if (cnt > 0) std::memcpy(sIn_, win + a, cnt * sizeof(float));
        pffft_transform_ordered(S(setupS_), sIn_, sOut_, sWork_, PFFFT_FORWARD);
        float* row = &P_[(size_t)f * nB_];
        for (int b = 0; b < nB_; ++b) {
            const int k = (bLo_ + b) * step;
            const float re = sOut_[2 * k], im = sOut_[2 * k + 1];
            row[b] = re * re + im * im;
        }
    }
    // ★ Each bin's noise: its median over the window (a signal holds a bin for at most 22 of 85 symbols).
    std::vector<float> col(nFrames_);
    const float ln2 = 0.6931472f;
    for (int b = 0; b < nB_; ++b) {
        for (int f = 0; f < nFrames_; ++f) col[f] = P_[(size_t)f * nB_ + b];
        std::nth_element(col.begin(), col.begin() + nFrames_ / 2, col.end());
        const float med = col[nFrames_ / 2];
        const float k = med > 0 ? ln2 / med : 0.f;
        for (int f = 0; f < nFrames_; ++f) P_[(size_t)f * nB_ + b] *= k;
    }
}

// ── 1b. Sync search ───────────────────────────────────────────────────────────────────────────────────────────────
void SlotDecoder::search(const SlotOptions& o, std::vector<Cand>& cands) {
    cands.clear();
    const double fps = (double)kRate / hop_;
    const int nominal = (int)std::lround((kWinLeadSec + p_->lead) * fps);
    const int tMin = std::max(0, nominal - (int)std::lround(1.0 * fps));
    const int tMax = std::min(nFrames_ - 1 - 4 * (kSymbols - 1), nominal + (int)std::lround(3.0 * fps));
    if (tMax < tMin) return;
    const int nb4 = (nB_ + 3) & ~3;
    std::vector<float> sy(nb4), all(nb4), best(nb4, -1e9f);
    std::vector<int> bestT(nb4, 0);
    const float inv22 = 1.0f / kSync, inv63 = 1.0f / kData;
    for (int t0 = tMin; t0 <= tMax; ++t0) {
        std::fill(sy.begin(), sy.end(), 0.f); std::fill(all.begin(), all.end(), 0.f);
        for (int i = 0; i < kSymbols; ++i) {
            const float* row = &P_[(size_t)(t0 + 4 * i) * nB_];
            float* dst = isSync(i) ? sy.data() : all.data();
            int b = 0;
            for (; b + 4 <= nB_; b += 4) q65v::st(dst + b, q65v::add(q65v::ld(dst + b), q65v::ld(row + b)));
            for (; b < nB_; ++b) dst[b] += row[b];
        }
        for (int b = 0; b < nB_; ++b) {
            const float s = sy[b] * inv22 - all[b] * inv63;
            if (s > best[b]) { best[b] = s; bestT[b] = t0; }
        }
    }
    for (int b = 0; b < nB_; ++b) {
        if (best[b] < o.syncMin) continue;
        bool peak = true;
        for (int d = -2; d <= 2 && peak; ++d) if (d && b + d >= 0 && b + d < nB_ && best[b + d] > best[b]) peak = false;
        if (peak) cands.push_back({bLo_ + b, bestT[b], best[b]});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    const int cap = o.loaded ? std::max(4, o.maxCands / 2) : o.maxCands;
    if ((int)cands.size() > cap) cands.resize(cap);
}

void SlotDecoder::bigFft(const float* win, int n) {
    std::memset(bigBuf_, 0, nBig_ * sizeof(float));
    std::memcpy(bigBuf_, win, std::min(n, nBig_) * sizeof(float));
    pffft_transform_ordered(S(setupBig_), bigBuf_, bigBuf_, bigWork_, PFFFT_FORWARD);
    bigDone_ = true;
}

// ── 2-6. One candidate, one submode ───────────────────────────────────────────────────────────────────────────────
bool SlotDecoder::extract(const Cand& c, int m, const SlotOptions& o, Ext& x) {
    x.m = m;
    Sub& s = sub_[m];
    if (!s.setupM || !s.setupN) return false;
    const double sp = baud_ * (1 << m);                    // tone spacing
    const double f0c = c.bin * baud_ / 2;                   // coarse tone-0 frequency
    if (f0c + 65 * sp > o.hiHz + sp) return false;
    const double binBig = (double)kRate / nBig_;
    const long kc = std::lround((f0c + 32 * sp) / binBig);
    // ── baseband: M bins around kc, back to time ──
    const int M = s.M, half = M / 2;
    for (int i = 0; i < M; ++i) {
        const long k = kc + i - half;
        const int pos = i < half ? i + half : i - half;   // (i − half) mod M, with no divide (ARMv7 has none)
        float re = 0, im = 0;
        if (k >= 1 && k < nBig_ / 2) { re = bigBuf_[2 * k]; im = bigBuf_[2 * k + 1]; }
        s.z[2 * pos] = re; s.z[2 * pos + 1] = im;
    }
    pffft_transform_ordered(S(s.setupM), s.z, s.z, s.zw, PFFFT_BACKWARD);
    const int Ns = s.Ns;
    // tone 0's frequency in the baseband, cycles per sample
    const double ft0 = (f0c - kc * binBig) / (Ns * baud_);
    // ── refine: start and frequency by the sync symbols' tone-0 power, in two stages — ±3/8 symbol in 1/24ths and
    //    ±½ baud in ⅛ths (the search's grid is a quarter symbol and half a baud, and noise moves its peak by one
    //    step), then ±1/24 symbol in 1/72nds and ±⅛ baud in 1/32nds ──
    const int n0c = c.frame * Ns / 4;
    double bestScore = -1, bestDf = 0; int bestN0 = n0c;
    std::vector<float> twr(Ns), twi(Ns);
    auto stage = [&](int nc, double fc, int tHalf, int tStep, int fHalf, double fStep) {
        double bs = -1, bdf = fc; int bn = nc;
        for (int fi = -fHalf; fi <= fHalf; ++fi) {
            const double df = fc + fi * fStep;
            const double cyc = ft0 + df / Ns;
            for (int n = 0; n < Ns; ++n) { const double ph = -kTwoPi * cyc * n; twr[n] = (float)std::cos(ph); twi[n] = (float)std::sin(ph); }
            for (int ti = -tHalf; ti <= tHalf; ++ti) {
                const int n0 = nc + ti * tStep;
                double sc = 0;
                for (int q = 0; q < kSync; ++q) {
                    const int a0 = n0 + kSyncPos[q] * Ns;
                    if (a0 < 0 || a0 + Ns > M) continue;
                    float ar, ai;
                    cdot(s.z + 2 * a0, twr.data(), twi.data(), Ns, ar, ai);
                    sc += (double)ar * ar + (double)ai * ai;
                }
                if (sc > bs) { bs = sc; bn = n0; bdf = df; }
            }
        }
        bestScore = bs; bestN0 = bn; bestDf = bdf;
    };
    // ★ Tone 0 is the same whatever the submode: refine once per candidate (on the first, narrowest submode tried)
    //   and carry the start and frequency to the others.
    if (refined_) {
        bestN0 = (int)std::lround(refStart_ * Ns / p_->nsps);
        bestDf = (refF0_ - f0c) / baud_;
    } else {
        stage(n0c, 0.0, 9, std::max(1, Ns / 24), 4, 1.0 / 8);
        stage(bestN0, bestDf, 3, std::max(1, Ns / 72), 4, 1.0 / 32);
        refStart_ = (double)bestN0 * p_->nsps / Ns; refF0_ = f0c + bestDf * baud_; refined_ = true;
    }
    (void)bestScore;
    // ── symbols: mix tone 0 to DC, a DFT per symbol ──
    const double cyc = ft0 + bestDf / Ns;
    for (int n = 0; n < Ns; ++n) { const double ph = -kTwoPi * cyc * n; twr[n] = (float)std::cos(ph); twi[n] = (float)std::sin(ph); }
    const int pf = s.nfft / Ns;                             // 2 for submode A (zero-padded 144), else 1
    const int K = 1;                                        // ★ bins summed per tone (Doppler width) — see the h loop
    float syncE[kSync]; int qs = 0;
    std::vector<float>& symPow = x.symPow;                  // tone energies, all 85 symbols, tones 0..64
    symPow.assign((size_t)kSymbols * 65, 0.f);
    for (int i = 0; i < kSymbols; ++i) {
        const int a = bestN0 + i * Ns;
        std::memset(s.y, 0, 2 * s.nfft * sizeof(float));
        if (a >= 0 && a + Ns <= M) {
            cmix(s.y, s.z + 2 * a, twr.data(), twi.data(), Ns);
        }
        pffft_transform_ordered(S(s.setupN), s.y, s.y, s.yw, PFFFT_FORWARD);
        for (int t = 0; t <= 64; ++t) {
            int bin = (t << m) * pf; if (bin >= s.nfft) bin -= s.nfft;   // < 2·nfft always
            symPow[(size_t)i * 65 + t] = s.y[2 * bin] * s.y[2 * bin] + s.y[2 * bin + 1] * s.y[2 * bin + 1];
        }
        (void)qs;
    }
    (void)K;
    // ── noise: the median energy of the data symbols' tone bins (a signal holds 1 of 64 of them) ──
    {
        std::vector<float> all; all.reserve(kData * 64);
        for (int i = 0; i < kSymbols; ++i) if (!isSync(i)) for (int t = 1; t <= 64; ++t) all.push_back(symPow[(size_t)i * 65 + t]);
        std::nth_element(all.begin(), all.begin() + all.size() / 2, all.end());
        const float n0 = all[all.size() / 2] / gammaMedian(1);
        if (!(n0 > 0)) return false;
        const float k = 1.0f / n0;
        for (float& v : symPow) v *= k;
        // ★ …and per tone bin: a bin a carrier (a birdie, another station's steady tone) sits in is scaled by its OWN
        //   median over the data symbols, so a constant tone can never look like a codeword's symbols.
        float col[kData];
        for (int t = 0; t <= 64; ++t) {
            int q = 0;
            for (int i = 0; i < kSymbols; ++i) if (!isSync(i)) col[q++] = symPow[(size_t)i * 65 + t];
            std::nth_element(col, col + kData / 2, col + kData);
            const float cn = col[kData / 2] / gammaMedian(1);
            if (cn > 1.0f) { const float kk = 1.0f / cn; for (int i = 0; i < kSymbols; ++i) symPow[(size_t)i * 65 + t] *= kk; }
        }
    }
    double rhoS = 0;
    for (int q = 0; q < kSync; ++q) { syncE[q] = symPow[(size_t)kSyncPos[q] * 65]; rhoS += syncE[q]; }
    rhoS = rhoS / kSync - 1.0;
    const float rho = (float)std::max(0.5, rhoS);
    x.kc = kc; x.ft0 = ft0; x.df = bestDf; x.n0 = bestN0; x.rho = rho;
    // ★ how much the data symbols' strongest tones stand out on THIS submode's grid: ranks the submodes, so belief
    //   propagation runs on the likeliest one or two, not all four (noise costs four times less)
    double z = 0;
    for (int i = 0; i < kSymbols; ++i) {
        if (isSync(i)) continue;
        const float* e = &symPow[(size_t)i * 65 + 1];
        float mx = e[0];
        for (int v = 1; v < 64; ++v) mx = std::max(mx, e[v]);
        z += mx;
    }
    x.z = z / kData + rhoS;
    (void)o;
    return true;
}

bool SlotDecoder::decodeExt(const Cand& c, const Ext& x, const SlotOptions& o, std::vector<Result>& out, SlotStats& st, bool tryAp) {
    const int m = x.m;
    const std::vector<float>& symPow = x.symPow;
    const float rho = x.rho;
    const long kc = x.kc;
    const int Ns = sub_[m].Ns;
    const double binBig = (double)kRate / nBig_;
    const double ft0 = x.ft0, bestDf = x.df;
    const int bestN0 = x.n0;
    // ── probabilities: the non-coherent AWGN metric, ln I0(2·√(ρ·E)), sharpened ×1.5 — measured best on 60A at the
    //    threshold (−27.5 dB: Rayleigh ρ/(1+ρ)·E 28 %, ×1.6 48 %; this 50 %, flat from ×1.3 to ×2.0) ──
    {
        auto li0 = [](float x) { return x < 3.75f ? std::log1p(x * x * 0.25f + x * x * x * x * (1.0f / 64)) : x - 0.5f * std::log(6.2831853f * x); };
        constexpr float kSharpen = 1.5f;
        int d = 0;
        for (int i = 0; i < kSymbols; ++i) {
            if (isSync(i)) continue;
            const float* e = &symPow[(size_t)i * 65 + 1];
            float mx = e[0];
            for (int v = 1; v < 64; ++v) mx = std::max(mx, e[v]);
            const float lmx = li0(2 * std::sqrt(rho * mx));
            float* p = &intr_[(size_t)d * 64];
            for (int v = 0; v < 64; ++v) {
                p[v] = std::exp(kSharpen * (li0(2 * std::sqrt(rho * std::max(0.f, e[v]))) - lmx));
                E_[(size_t)d * 64 + v] = e[v];
            }
            ++d;
        }
    }
    // ── decode: blind, then "CQ ? ?" ──
    uint8_t msg[kMsg];
    int it = 0, ap = 0;
    ++st.attempts;
    const int iters = o.maxIter > 0 ? o.maxIter : 50;   // ★ 100 decoded 1.7 % more at 60A's threshold, for twice the cost on noise
    it = bpDecode(intr_.data(), iters, msg, bp_);
    if (!it && tryAp && o.apCq && !o.loaded) { ++st.attempts; it = bpDecode(intr_.data(), iters, msg, bp_, cqMask_, cqVal_); ap = it ? 1 : 0; }
    if (!it) return false;
    // ── guard ──
    uint8_t tones[kSymbols];
    encodeTones(msg, tones);
    double sc = 0, sig = 0;
    int hard = 0;
    for (int i = 0, d = 0; i < kSymbols; ++i) {
        const float e = symPow[(size_t)i * 65 + tones[i]];
        sig += e - 1.0;
        if (!isSync(i)) {
            const float* row = &E_[(size_t)d * 64];
            const float ec = row[tones[i] - 1];
            sc += ec;
            bool mx = true; for (int v = 0; v < 64 && mx; ++v) if (row[v] > ec) mx = false;
            hard += mx;
            ++d;
        }
    }
    sc /= kData;
    // ★ A codeword that is mostly ONE tone is a carrier, not a transmission (the all-zero message is every data symbol
    //   on tone 1); a real codeword's commonest symbol turns up 3-5 times in 63.
    {
        int cnt[65] = {}, mx = 0;
        for (int i = 0; i < kSymbols; ++i) if (!isSync(i)) mx = std::max(mx, ++cnt[tones[i]]);
        if (mx > 12) { ++st.rejected; return false; }
    }
    uint8_t payload[10];
    if (!symbolsToPayload(msg, payload)) { ++st.rejected; return false; }
    for (size_t q = 0; q + 10 <= seen_.size(); q += 10) if (std::memcmp(&seen_[q], payload, 10) == 0) return true;   // already
    Result r;
    bool plausible = false;
    if (!unpackPayload(payload, false, r.text, r.call, r.grid, plausible)) { ++st.rejected; return false; }
    const float need = (ap ? kScoreMinAp : kScoreMin) + (plausible ? 0.f : 0.5f);
    static const bool kNoGuard = std::getenv("Q65_NOGUARD") != nullptr;   // ★ the bench's data collection
    if (sc < need && !kNoGuard) { ++st.rejected; return false; }
    if (ap && (r.grid.empty() || r.call.empty()) && !kNoGuard) { ++st.rejected; return false; }
    if (!ap) unpackPayload(payload, true, r.text, r.call, r.grid, plausible);   // ★ a blind decode teaches the hash table
    r.periodSec = p_->sec; r.submode = m;
    r.f0 = (float)(kc * binBig + (ft0 + bestDf / Ns) * Ns * baud_);
    r.dt = (float)((double)bestN0 * p_->nsps / Ns / kRate - kWinLeadSec - p_->lead);
    const double rhoAll = std::max(1e-3, sig / kSymbols);
    r.esn0 = (float)rhoAll;
    r.snr = (int)std::lround(10.0 * std::log10(rhoAll * baud_ / 2500.0));
    r.score = (float)sc; r.hard = hard; r.sync = c.score; r.iters = it; r.ap = ap;
    std::memcpy(r.payload, payload, 10);
    seen_.insert(seen_.end(), payload, payload + 10);
    out.push_back(r);
    ++st.decodes;
    return true;
}

int SlotDecoder::decode(const float* win, int n, const SlotOptions& o, std::vector<Result>& out, SlotStats* stOut) {
    SlotStats st;
    const double t0 = nowMs();
    if (!p_) return 0;
    const size_t first = out.size();
    seen_.clear();
    bigDone_ = false;
    // search range: tone 0 from loHz; the A submode's top tone within hiHz
    bLo_ = std::max(1, (int)std::ceil(o.loHz / (baud_ / 2)));
    bHi_ = (int)std::floor((o.hiHz - 64 * baud_) / (baud_ / 2));
    nB_ = bHi_ - bLo_ + 1;
    if (nB_ <= 4) return 0;
    P_.assign((size_t)nFrames_ * nB_, 0.f);
    spectrogram(win, n);
    std::vector<Cand> cands;
    search(o, cands);
    st.candidates = (int)cands.size();
    const double t1 = nowMs();
    st.searchMs = t1 - t0;
    if (!cands.empty()) { bigFft(win, n); st.fftMs = nowMs() - t1; }
    const double t2 = nowMs();
    for (const Cand& c : cands) {
        if (o.abort && o.abort->load()) break;
        int nx = 0;
        refined_ = false;
        for (int m = 0; m < 5; ++m) {
            if (!(o.submodes & (1u << m))) continue;
            if (extract(c, m, o, ext_[nx])) ++nx;
        }
        int order[5] = {0, 1, 2, 3, 4};
        std::sort(order, order + nx, [&](int a, int b) { return ext_[a].z > ext_[b].z; });
        const int tries = std::min(nx, o.loaded ? 1 : 2);
        for (int k = 0; k < tries; ++k) if (decodeExt(c, ext_[order[k]], o, out, st, k == 0)) break;   // ★ a-priori on the likeliest submode only
    }
    st.candMs = nowMs() - t2;
    st.totalMs = nowMs() - t0;
    if (stOut) *stOut = st;
    return (int)(out.size() - first);
}

SharedSlotDecoder& sharedSlotDecoder(int periodSec) {
    static std::mutex mtx;
    static std::vector<std::unique_ptr<SharedSlotDecoder>> all;
    std::lock_guard<std::mutex> lk(mtx);
    for (auto& d : all) if (d->dec.ok() && d->dec.period().sec == periodSec) return *d;
    all.emplace_back(new SharedSlotDecoder(periodSec));
    return *all.back();
}

// ── The live decoder ──────────────────────────────────────────────────────────────────────────────────────────────
LiveDecoder::LiveDecoder(std::vector<int> periods) {
    int maxSec = 0;
    for (int p : periods) if (periodFor(p)) { periods_.push_back(p); maxSec = std::max(maxSec, p); }
    for (int p : periods_) dec_.push_back(&sharedSlotDecoder(p));
    next_.assign(periods_.size(), NAN);
    queued_.assign(periods_.size(), false);
    ring_.assign((size_t)(maxSec + kWinLeadSec + kWinTailSec + 8) * kRate, 0.f);
    th_ = std::thread([this] { worker(); });
}

LiveDecoder::~LiveDecoder() {
    abort_.store(true);
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void LiveDecoder::drain() {
    std::unique_lock<std::mutex> lk(m_);
    idle_.wait(lk, [this] { return q_.empty() && !busy_; });
}

void LiveDecoder::process(const float* audio, int n, double captureUtc) {
    if (n <= 0 || ring_.empty()) return;
    // ★ The clock: sample 0's UTC, anchored to the capture time of each block's LAST sample (lowest latency wins; a jump
    //   of more than a second — audio that was not fed — re-anchors and abandons the windows in flight).
    if (std::isfinite(captureUtc)) {
        const double est = captureUtc - (double)(total_ + n) / kRate;
        if (!anchored_ || est < utc0_ - 1e-6 || est - utc0_ > 1.0) {
            const bool jump = anchored_;
            utc0_ = est; anchored_ = true;
            if (jump) std::fill(next_.begin(), next_.end(), NAN);
        }
    } else if (!anchored_) {
        return;
    }
    const size_t R = ring_.size();
    {   // ★ one 64-bit modulo per block, not per sample (ARMv7 has no divide: each is a library call)
        size_t w = (size_t)(total_ % (int64_t)R);
        for (int i = 0; i < n; ++i) { ring_[w] = audio[i]; if (++w == R) w = 0; }
    }
    total_ += n;
    const double tEnd = utc0_ + (double)total_ / kRate;       // UTC just after the last sample
    const double tStart = utc0_ + (double)std::max<int64_t>(0, total_ - (int64_t)R + kRate) / kRate;
    for (size_t pi = 0; pi < periods_.size(); ++pi) {
        if (!dec_[pi]->dec.ok()) continue;
        const Period& p = dec_[pi]->dec.period();
        if (!std::isfinite(next_[pi])) {
            // the first period whose whole window is still ahead of us
            double s = std::ceil((tEnd - kWinLeadSec * 0) / p.sec) * p.sec;
            while (s - kWinLeadSec < utc0_) s += p.sec;
            next_[pi] = s;
        }
        while (tEnd >= next_[pi] + p.sec + kWinTailSec) {
            const double ws = next_[pi] - kWinLeadSec;
            const int W = windowSamples(p);
            if (ws >= tStart && ws >= utc0_) {
                std::lock_guard<std::mutex> lk(m_);
                if (queued_[pi]) {
                    slotsSkipped.fetch_add(1);                 // ★ the previous one is still waiting: skip, never wait
                } else {
                    Job j; j.pi = (int)pi; j.slotUtc = next_[pi]; j.win.resize(W);
                    const int64_t s0 = (int64_t)std::llround((ws - utc0_) * kRate);
                    size_t r = (size_t)(((s0 % (int64_t)R) + (int64_t)R) % (int64_t)R);
                    for (int k = 0; k < W; ++k) { j.win[k] = ring_[r]; if (++r == R) r = 0; }
                    q_.push_back(std::move(j)); queued_[pi] = true;
                    cv_.notify_one();
                }
            }
            next_[pi] += p.sec;
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
            j = std::move(q_.front()); q_.pop_front();
            busy_ = true;
        }
        SlotOptions o = opts;
        o.loaded = loadProbe ? loadProbe() : false;
        o.abort = &abort_;
        std::vector<Result> res;
        SlotStats st;
        {
            std::lock_guard<std::mutex> lk(dec_[(size_t)j.pi]->m);
            dec_[(size_t)j.pi]->dec.decode(j.win.data(), (int)j.win.size(), o, res, &st);
        }
        lastDecodeMs.store(st.totalMs);
        slotsDecoded.fetch_add(1);
        for (Result& r : res) { r.slotUtc = j.slotUtc; if (onResult) onResult(r); }
        {
            std::lock_guard<std::mutex> lk(m_);
            queued_[(size_t)j.pi] = false;
            busy_ = false;
        }
        idle_.notify_all();
    }
}

} // namespace q65
} // namespace vibe
