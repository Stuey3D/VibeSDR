// vibe_dvsig_front.h — THE SHARED FRONT END of the digital-voice signalling decoder (V12, docs/v12/DV-SIGNALLING.md).
//
// One channel's IQ → 24 kHz → a 6 kHz channel filter → vibedsp's FM discriminator (Hz) → two matched filters:
//   path A  root-raised-cosine α = 0.2 for 4800 symbols a second, at 24 kHz (5 samples a symbol): DMR, YSF, NXDN96,
//           and D-STAR's GMSK 4800 bit/s (two levels through the same filter);
//   path B  root-raised-cosine α = 0.2 for 2400 symbols a second, decimated to 12 kHz (5 samples a symbol): NXDN48, dPMR.
// Then every sync word of every system is correlated at every sample, on both paths at once:
//   A: DMR (BS, MS, direct TS1, direct TS2 — each pattern's voice and data syncs are each other's negative, so the sign
//      says which), YSF, D-STAR (frame sync, the voice frames' data sync, the end pattern), NXDN96's FSW;
//   B: dPMR FS1, FS2, FS3, FS4 and NXDN48's FSW.
// The correlation is normalised and mean-removed, so it ignores the signal's level, its deviation and the tuning error.
// A local peak above the pattern's threshold is handed to the systems (vibe_dvsig.h) as a CANDIDATE with its position
// interpolated to a fraction of a sample, which is also the frame's symbol timing — the M17 receiver's method
// (vibe_m17.h), generalised to many patterns of different lengths.
//
// ★★ AUTO IS FREE OF CHOICE: all the patterns run side by side, so the receiver says what a channel carries instead of
//    asking. A manual pin (one system) switches the other patterns' blocks off — the fallback when a channel is busy
//    with several systems, and a CPU saving.
// ★★★ SIMD — the hot loop is the correlator bank: 24 symbol-spaced samples against four patterns at a time. It is done in
//     INTEGERS (int16 samples × int16 coefficients → int32), so NEON (ARMv7 and A64), SSE2 and the scalar path are
//     BIT-IDENTICAL by construction — test-dvsig checks it on random input and on whole decodes. The discriminator and
//     both FIRs are vibedsp's NEON/SSE2 kernels.
#pragma once
#include "vibe_dvsig_fec.h"
#include "vibedsp/vibedsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <vector>

#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#define VIBE_DVSIG_NEON 1
#elif (defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define VIBE_DVSIG_SSE 1
#endif

namespace vibe {
namespace dvsig {

using cf32 = std::complex<float>;

inline const char* simdName() {
#if VIBE_DVSIG_NEON
    return "NEON";
#elif VIBE_DVSIG_SSE
    return "SSE2";
#else
    return "scalar";
#endif
}

// ── Filters ───────────────────────────────────────────────────────────────────────────────────────────────────────────
/** Root-raised-cosine, `sps` samples a symbol, `span` symbols long, unity DC gain. */
inline std::vector<float> rrcTaps(double sps, double alpha, int span) {
    const int n = int(span * sps) | 1, h = n / 2;
    std::vector<float> t(static_cast<size_t>(n));
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double x = (i - h) / sps;
        double v;
        if (std::fabs(x) < 1e-9) v = 1.0 - alpha + 4 * alpha / M_PI;
        else if (std::fabs(std::fabs(4 * alpha * x) - 1.0) < 1e-9)
            v = alpha / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * alpha)));
        else v = (std::sin(M_PI * x * (1 - alpha)) + 4 * alpha * x * std::cos(M_PI * x * (1 + alpha))) / (M_PI * x * (1 - 16 * alpha * alpha * x * x));
        t[size_t(i)] = float(v); sum += v;
    }
    for (auto& v : t) v = float(v / sum);
    return t;
}
/** Windowed-sinc low-pass (Blackman), unity DC gain. */
inline std::vector<float> lowpass(double fs, double cut, int n) {
    std::vector<float> t(static_cast<size_t>(n));
    const int h = n / 2; double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double x = i - h, w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
        const double s = x == 0 ? 2 * cut / fs : std::sin(2 * M_PI * cut / fs * x) / (M_PI * x);
        t[size_t(i)] = float(s * w); sum += s * w;
    }
    for (auto& v : t) v = float(v / sum);
    return t;
}

// ── A bounded sample history with absolute indexing ───────────────────────────────────────────────────────────────────
struct Ring {
    std::vector<float> y;
    long base = 0;   ///< absolute index of y[0]
    long end() const { return base + long(y.size()); }
    float at(long abs) const { const long i = abs - base; return (i >= 0 && i < long(y.size())) ? y[size_t(i)] : 0.0f; }
    /** Catmull-Rom cubic at a fractional absolute position. */
    float interp(double t) const {
        const long i = long(std::floor(t));
        const float u = float(t - double(i));
        const float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        return p1 + 0.5f * u * (p2 - p0 + u * (2 * p0 - 5 * p1 + 4 * p2 - p3 + u * (3 * (p1 - p2) + p3 - p0)));
    }
    void trim(long keep) { if (long(y.size()) > keep + 8192) { const long d = long(y.size()) - keep; y.erase(y.begin(), y.begin() + d); base += d; } }
};

// ── The sync patterns (symbols in the order sent; 4-FSK as ±1/±3, two-level as ±1) ────────────────────────────────────
enum Pat : int {
    // path A (4800 Bd)
    kDmrBs = 0, kDmrMs, kDmrDm1, kDmrDm2,      // block 0
    kYsf, kDsFs, kDsData, kDsEnd,               // block 1
    kNx96, kA9, kA10, kA11,                     // block 2 (one used)
    kPatsA = 12,
    // path B (2400 Bd) — numbered from 0 on their own path
    kPmFs1 = 0, kPmFs2, kPmFs3, kPmFs4,         // block 0
    kNx48, kB5, kB6, kB7,                       // block 1 (one used)
    kPatsB = 8,
};
constexpr int kWin = 24;   ///< the correlator window, symbols

inline int sym4(int msb, int lsb) { return msb ? (lsb ? -3 : -1) : (lsb ? +3 : +1); }   // 01 → +3, 00 → +1, 10 → −1, 11 → −3
inline std::vector<int8_t> hexSyms(uint64_t v, int nBits) {
    std::vector<int8_t> s;
    for (int i = nBits - 2; i >= 0; i -= 2) s.push_back(int8_t(sym4(int((v >> (i + 1)) & 1), int((v >> i) & 1))));
    return s;
}
inline std::vector<int8_t> bitSyms(const char* bits) { std::vector<int8_t> s; for (const char* p = bits; *p; ++p) s.push_back(*p == '1' ? 1 : -1); return s; }

// DMR TS 102 361-1 table 9.2 (the VOICE pattern of each; data = its negative)
constexpr uint64_t kDmrBsVoice = 0x755FD7DF75F7ull, kDmrMsVoice = 0x7F7D5DD57DFDull, kDmrDm1Voice = 0x5D577F7757FFull, kDmrDm2Voice = 0x7DFFD5F55D5Full;
constexpr uint64_t kYsfSync = 0xD471C9634Dull;     // YSF, 20 symbols
constexpr uint64_t kNxFsw = 0xCDF59ull;           // NXDN FSW, 10 symbols
constexpr uint64_t kPmFs1v = 0x57FF5F75D577ull, kPmFs2v = 0x5FF77Dull, kPmFs3v = 0x7DDFF5ull, kPmFs4v = 0xFD55F5DF7FDDull;   // TS 102 490 §6.1
// D-STAR (JARL STD7.0 §4.1): the last 9 bit-sync bits + the 15-bit frame sync; the data sync; the end pattern's last 24
constexpr const char* kDsFsBits = "010101010111011001010000";
constexpr const char* kDsDataBits = "101010101011010001101000";
constexpr const char* kDsEndBits = "101010100001001101011110";

struct PatDef { std::vector<int8_t> s; float thr = 0.8f; };
inline const std::vector<PatDef>& patsA() {
    static const std::vector<PatDef> v = [] {
        std::vector<PatDef> p(kPatsA);
        p[kDmrBs] = {hexSyms(kDmrBsVoice, 48), 0.80f}; p[kDmrMs] = {hexSyms(kDmrMsVoice, 48), 0.80f};
        p[kDmrDm1] = {hexSyms(kDmrDm1Voice, 48), 0.80f}; p[kDmrDm2] = {hexSyms(kDmrDm2Voice, 48), 0.80f};
        p[kYsf] = {hexSyms(kYsfSync, 40), 0.82f};
        p[kDsFs] = {bitSyms(kDsFsBits), 0.80f}; p[kDsData] = {bitSyms(kDsDataBits), 0.86f}; p[kDsEnd] = {bitSyms(kDsEndBits), 0.86f};
        p[kNx96] = {hexSyms(kNxFsw, 20), 0.93f};
        return p;
    }();
    return v;
}
inline const std::vector<PatDef>& patsB() {
    static const std::vector<PatDef> v = [] {
        std::vector<PatDef> p(kPatsB);
        p[kPmFs1] = {hexSyms(kPmFs1v, 48), 0.80f}; p[kPmFs2] = {hexSyms(kPmFs2v, 24), 0.90f};
        p[kPmFs3] = {hexSyms(kPmFs3v, 24), 0.90f}; p[kPmFs4] = {hexSyms(kPmFs4v, 48), 0.80f};
        p[kNx48] = {hexSyms(kNxFsw, 20), 0.93f};
        return p;
    }();
    return v;
}

// ── The correlator bank ───────────────────────────────────────────────────────────────────────────────────────────────
/* ★★★ INTEGER ARITHMETIC, SO THE KERNELS CANNOT DISAGREE. The filtered discriminator (in Hz) is rounded to int16 for the
 *     correlators, and each pattern's coefficients are integers: c = L·s − Σs (L times the zero-mean pattern). A dot
 *     product of int16 by int16 into int32 is exact however it is ordered, so NEON (vmull/vaddq), SSE2 (pmaddwd) and the
 *     scalar loop give the SAME numbers — no float rounding, no fused multiply-add to worry about (the shim is built with
 *     -ffp-contract=fast). The normalisation that follows is the same scalar code for all three. ρ = Σwc / (|c|·σ_w·√L). */
struct Bank {
    int nPat = 0;
    std::vector<int16_t> coef;           // nPat × kWin, lane-contiguous (pattern k's 24 taps at coef[k·24]); zero where unused
    std::vector<double> norm;            // |c| per pattern
    std::vector<int> len;                // per pattern (0 = unused)
    std::vector<float> thr;
    explicit Bank(const std::vector<PatDef>& p) {
        nPat = int(p.size());
        coef.assign(size_t(nPat * kWin), 0);
        norm.assign(size_t(nPat), 1.0); len.assign(size_t(nPat), 0); thr.assign(size_t(nPat), 2.f);
        for (int k = 0; k < nPat; ++k) {
            const int L = int(p[size_t(k)].s.size());
            if (!L) continue;
            len[size_t(k)] = L; thr[size_t(k)] = p[size_t(k)].thr;
            int sum = 0; for (int8_t v : p[size_t(k)].s) sum += v;
            double e = 0;
            for (int i = 0; i < L; ++i) {
                const int c = L * p[size_t(k)].s[size_t(i)] - sum;
                coef[size_t(k * kWin + (kWin - L + i))] = int16_t(c);
                e += double(c) * c;
            }
            norm[size_t(k)] = std::sqrt(e);
        }
    }
};
enum class Kern { Auto, Scalar };
/** Four patterns at once: acc[l] = Σ_t w[t]·coef[(k0+l)·24 + t]. Scalar reference. */
inline void corrScalar(const int16_t* w, const int16_t* coef4, int32_t* acc) {
    for (int l = 0; l < 4; ++l) { int32_t a = 0; const int16_t* c = coef4 + l * kWin; for (int t = 0; t < kWin; ++t) a += int32_t(w[t]) * c[t]; acc[l] = a; }
}
inline void corrSimd(const int16_t* w, const int16_t* coef4, int32_t* acc) {
#if VIBE_DVSIG_NEON
    const int16x8_t w0 = vld1q_s16(w), w1 = vld1q_s16(w + 8), w2 = vld1q_s16(w + 16);
    for (int l = 0; l < 4; ++l) {
        const int16_t* c = coef4 + l * kWin;
        int32x4_t a = vmull_s16(vget_low_s16(w0), vld1_s16(c));
        a = vmlal_s16(a, vget_high_s16(w0), vld1_s16(c + 4));
        a = vmlal_s16(a, vget_low_s16(w1), vld1_s16(c + 8));
        a = vmlal_s16(a, vget_high_s16(w1), vld1_s16(c + 12));
        a = vmlal_s16(a, vget_low_s16(w2), vld1_s16(c + 16));
        a = vmlal_s16(a, vget_high_s16(w2), vld1_s16(c + 20));
        // ★ horizontal sum without vaddvq (ARMv7 has none)
        const int32x2_t s = vadd_s32(vget_low_s32(a), vget_high_s32(a));
        acc[l] = vget_lane_s32(vpadd_s32(s, s), 0);
    }
#elif VIBE_DVSIG_SSE
    const __m128i w0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w)), w1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w + 8)),
                  w2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w + 16));
    for (int l = 0; l < 4; ++l) {
        const int16_t* c = coef4 + l * kWin;
        __m128i a = _mm_madd_epi16(w0, _mm_loadu_si128(reinterpret_cast<const __m128i*>(c)));
        a = _mm_add_epi32(a, _mm_madd_epi16(w1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(c + 8))));
        a = _mm_add_epi32(a, _mm_madd_epi16(w2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(c + 16))));
        a = _mm_add_epi32(a, _mm_shuffle_epi32(a, _MM_SHUFFLE(1, 0, 3, 2)));
        a = _mm_add_epi32(a, _mm_shuffle_epi32(a, _MM_SHUFFLE(2, 3, 0, 1)));
        acc[l] = _mm_cvtsi128_si32(a);
    }
#else
    corrScalar(w, coef4, acc);
#endif
}
inline int16_t q16(float v) { const float c = std::max(-32767.f, std::min(32767.f, v)); return int16_t(std::lrint(c)); }

/** One path: the sample history, and the correlations of every pattern at every sample (with peak picking). */
class Path {
public:
    using Cand = std::function<void(int pat, double pos, float rho)>;
    explicit Path(const std::vector<PatDef>& p) : bank_(p) {
        const size_t n = size_t(bank_.nPat);
        r0_.assign(n, 0.f); r1_.assign(n, 0.f); active_.assign(n, true);
    }
    Ring ring;
    Kern kern = Kern::Auto;
    void setActive(int pat, bool on) { if (pat >= 0 && pat < int(active_.size())) active_[size_t(pat)] = on; }
    long candidates = 0;
    static constexpr int kSps = 5;
    /** Correlate at every new sample from where the last call stopped to the ring's end. */
    void run(const Cand& cb) {
        const long e = ring.end();
        long p = std::max(from_, ring.base + long(kWin - 1) * kSps + 2);
        alignas(16) int16_t w[kWin];
        int32_t acc[4];
        const int nB = (bank_.nPat + 3) / 4;
        for (; p < e; ++p) {
            const float* y = ring.y.data() + (p - ring.base);
            for (int t = 0; t < kWin; ++t) w[t] = q16(y[-long(kWin - 1 - t) * kSps]);
            int64_t s1[kWin + 1], s2[kWin + 1];
            s1[0] = 0; s2[0] = 0;
            for (int L = 1; L <= kWin; ++L) { const int64_t v = w[kWin - L]; s1[L] = s1[L - 1] + v; s2[L] = s2[L - 1] + v * v; }
            for (int b = 0; b < nB; ++b) {
                bool any = false;
                for (int l = 0; l < 4; ++l) { const int k = 4 * b + l; if (k < bank_.nPat && bank_.len[size_t(k)] && active_[size_t(k)]) any = true; }
                if (!any) continue;
                const int16_t* c = bank_.coef.data() + size_t(4 * b * kWin);
                if (kern == Kern::Scalar) corrScalar(w, c, acc); else corrSimd(w, c, acc);
                for (int l = 0; l < 4; ++l) {
                    const int k = 4 * b + l;
                    if (k >= bank_.nPat || !bank_.len[size_t(k)] || !active_[size_t(k)]) continue;
                    const float rho = rhoOf(k, acc[l], s1, s2);
                    // ★ a peak at p−1: |ρ| above the threshold and above both neighbours (ties go to the earlier)
                    const float a0 = std::fabs(r0_[size_t(k)]), a1 = std::fabs(r1_[size_t(k)]), a2 = std::fabs(rho);
                    if (a1 >= bank_.thr[size_t(k)] && a1 >= a0 && a1 > a2) {
                        ++candidates;
                        const float den = a0 - 2 * a1 + a2;
                        const double frac = std::fabs(den) < 1e-9f ? 0.0 : std::max(-0.5, std::min(0.5, double(0.5f * (a0 - a2) / den)));
                        cb(k, double(p - 1) + frac, r1_[size_t(k)]);
                    }
                    r0_[size_t(k)] = r1_[size_t(k)]; r1_[size_t(k)] = rho;
                }
            }
        }
        from_ = e;
    }
    /** ρ of one pattern at one (integer) position — the tracking receivers' look for the next sync. */
    float rhoAt(int pat, long p) const {
        if (!bank_.len[size_t(pat)]) return 0;
        int16_t w[kWin];
        for (int t = 0; t < kWin; ++t) w[t] = q16(ring.at(p - long(kWin - 1 - t) * kSps));
        int64_t s1[kWin + 1], s2[kWin + 1]; s1[0] = s2[0] = 0;
        for (int L = 1; L <= kWin; ++L) { const int64_t v = w[kWin - L]; s1[L] = s1[L - 1] + v; s2[L] = s2[L - 1] + v * v; }
        int32_t a = 0; const int16_t* c = bank_.coef.data() + size_t(pat * kWin);
        for (int t = 0; t < kWin; ++t) a += int32_t(w[t]) * c[t];
        return rhoOf(pat, a, s1, s2);
    }
    /** Test hook: one block's dot products through either kernel, on a given window. */
    void dots(const int16_t* w, int block, Kern k, int32_t* acc) const {
        const int16_t* c = bank_.coef.data() + size_t(4 * block * kWin);
        if (k == Kern::Scalar) corrScalar(w, c, acc); else corrSimd(w, c, acc);
    }
private:
    float rhoOf(int k, int32_t acc, const int64_t* s1, const int64_t* s2) const {
        const int L = bank_.len[size_t(k)];
        const int64_t v = int64_t(L) * s2[L] - s1[L] * s1[L];   // L²·variance, exact
        if (v <= 0) return 0.f;
        return float(double(acc) / (bank_.norm[size_t(k)] * std::sqrt(double(v) / double(L))));
    }
    Bank bank_;
    std::vector<float> r0_, r1_;
    std::vector<bool> active_;
    long from_ = 0;
};

/** The whole front end: IQ in, the two paths' histories and candidates out. */
class Front {
public:
    static constexpr int kFsA = 24000, kFsB = 12000;
    Front() : pa(patsA()), pb(patsB()), chan_(lowpass(kFsA, 5800, 63), 1), fm_(float(kFsA / (2 * M_PI))),
              rrcA_(rrcTaps(5, 0.2, 10)), rrcB_(rrcTaps(10, 0.2, 10), 2) {}
    Path pa, pb;
    /** Complex baseband, the dial at DC, at any rate. */
    void feedIq(const cf32* x, int n, double rateHz) {
        if (n <= 0 || !(rateHz > 0)) return;
        if (std::fabs(rateHz - kFsA) < 1e-6) { feed24k(x, n); return; }
        if (!rsI_ || rsRate_ != rateHz) {
            rsI_.reset(new vibedsp::RationalResampler(rateHz, kFsA, vibedsp::RationalResampler::ExactRate{}));
            rsQ_.reset(new vibedsp::RationalResampler(rateHz, kFsA, vibedsp::RationalResampler::ExactRate{}));
            rsRate_ = rateHz;
        }
        re_.resize(size_t(n)); im_.resize(size_t(n));
        for (int i = 0; i < n; ++i) { re_[size_t(i)] = x[i].real(); im_[size_t(i)] = x[i].imag(); }
        const int mo = rsI_->maxOut(n);
        a_.resize(size_t(mo)); b_.resize(size_t(mo));
        const int na = rsI_->process(re_.data(), n, a_.data()), nb = rsQ_->process(im_.data(), n, b_.data());
        const int m = std::min(na, nb);
        z_.resize(size_t(m));
        for (int i = 0; i < m; ++i) z_[size_t(i)] = cf32(a_[size_t(i)], b_[size_t(i)]);
        feed24k(z_.data(), m);
    }
    void feed24k(const cf32* x, int n) {
        if (n <= 0) return;
        c_.resize(size_t(chan_.maxOut(n)));
        const int nc = chan_.process(x, n, c_.data());
        d_.resize(size_t(nc));
        fm_.process(c_.data(), d_.data(), nc);
        for (int i = 0; i < nc; ++i) if (!std::isfinite(d_[size_t(i)])) d_[size_t(i)] = 0;
        fa_.resize(size_t(rrcA_.maxOut(nc))); fb_.resize(size_t(rrcB_.maxOut(nc)));
        const int na = rrcA_.process(d_.data(), nc, fa_.data());
        const int nb = rrcB_.process(d_.data(), nc, fb_.data());
        pa.ring.y.insert(pa.ring.y.end(), fa_.begin(), fa_.begin() + na);
        pb.ring.y.insert(pb.ring.y.end(), fb_.begin(), fb_.begin() + nb);
    }
    /** ★ The receivers say how far back they still need (frames waiting for their samples); a second is ample. */
    void trim() { pa.ring.trim(kFsA); pb.ring.trim(kFsB); }
private:
    std::unique_ptr<vibedsp::RationalResampler> rsI_, rsQ_;
    double rsRate_ = 0;
    vibedsp::FirDecimator chan_;
    vibedsp::FmDemod fm_;
    vibedsp::RealFir rrcA_, rrcB_;
    std::vector<float> re_, im_, a_, b_, d_, fa_, fb_;
    std::vector<cf32> z_, c_;
};

// ── Reading a frame off a path ───────────────────────────────────────────────────────────────────────────────────────
/** A frame's symbols, relative to an anchor (the sync's LAST symbol = 0), levelled: z = (y − dc) / gain. */
struct Levels { double gain = 0, dc = 0; float sigma = 0; };
/** Least squares through known symbols at offsets `k[i]` with values `s[i]`. */
inline bool fitLevels(const Ring& r, double anchor, const int* k, const int8_t* s, int n, Levels& out) {
    double sy = 0, sp = 0, spp = 0, syp = 0;
    for (int i = 0; i < n; ++i) { const double y = r.interp(anchor + double(k[i]) * Path::kSps); sy += y; sp += s[i]; spp += double(s[i]) * s[i]; syp += y * s[i]; }
    const double den = n * spp - sp * sp;
    if (!(den > 0)) return false;
    out.gain = (n * syp - sp * sy) / den;
    out.dc = (sy - out.gain * sp) / n;
    return out.gain > 1e-6;
}
inline float symZ(const Ring& r, double anchor, int k, const Levels& l) { return float((r.interp(anchor + double(k) * Path::kSps) - l.dc) / l.gain); }
/** Soft dibit (positive = 1) from a 4-level symbol in units: the first bit is the sign, the second the outer level. */
inline void softDibit(float z, int8_t* two) {
    auto q = [](float v) { const float x = v * 48.f; return int8_t(std::max(-127.f, std::min(127.f, x))); };
    two[0] = q(-z); two[1] = q(std::fabs(z) - 2.f);
}
inline void hardDibit(float z, uint8_t* two) { two[0] = z < 0 ? 1 : 0; two[1] = std::fabs(z) > 2.f ? 1 : 0; }

}  // namespace dvsig
}  // namespace vibe
