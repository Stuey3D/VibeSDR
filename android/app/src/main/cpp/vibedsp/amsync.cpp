// VibeSDR V12 — synchronous AM (SAM, SAM-U/L, ECSS-U/L), AM stereo (C-QUAM compatible) and AM auto
// bandwidth. Original VibeSDR code, written from the published descriptions of each technique; no GPL
// source was copied. See SyncAm in vibedsp.h for the design and docs/v12/AM-SYNC.md for the measurements.
#include "vibedsp.h"
#include "simd_internal.h"

#include <algorithm>
#include <cstring>

namespace vibedsp {

namespace {
inline float poleFor(double rate, double cornerHz) {          // one-pole coefficient for a corner
    return (float)(1.0 - std::exp(-2.0 * M_PI * cornerHz / std::max(1.0, rate)));
}
inline float poleTau(double rate, double tauSec) {             // one-pole coefficient for a time constant
    return (float)(1.0 - std::exp(-1.0 / std::max(1e-9, rate * tauSec)));
}
/** Σ x — four lanes where the build allows. */
inline float sumBlock(const float* x, int n) {
    int i = 0; float s = 0.0f;
#if VIBE_NEON
    float32x4_t a = vdupq_n_f32(0.0f);
    for (; i + 4 <= n; i += 4) a = vaddq_f32(a, vld1q_f32(x + i));
    s = vaddvq_f32(a);
#elif VIBE_SSE
    __m128 a = _mm_setzero_ps();
    for (; i + 4 <= n; i += 4) a = _mm_add_ps(a, _mm_loadu_ps(x + i));
    s = sseAddv(a);
#endif
    for (; i < n; ++i) s += x[i];
    return s;
}
/** out[i] = a + (b − a)·(p0 + i + 1)/K — the per-sample ramp of a value updated once per sub-block. */
inline void rampFill(float* out, int n, float a, float b, int p0, int K) {
    const float d = (b - a) / (float)K;
    const float base = a + d * (float)(p0 + 1);
    int i = 0;
#if VIBE_NEON
    const float idx[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
    float32x4_t v = vmlaq_n_f32(vdupq_n_f32(base), vld1q_f32(idx), d);
    const float32x4_t inc = vdupq_n_f32(4.0f * d);
    for (; i + 4 <= n; i += 4) { vst1q_f32(out + i, v); v = vaddq_f32(v, inc); }
#elif VIBE_SSE
    __m128 v = _mm_add_ps(_mm_set1_ps(base), _mm_mul_ps(_mm_setr_ps(0.0f, 1.0f, 2.0f, 3.0f), _mm_set1_ps(d)));
    const __m128 inc = _mm_set1_ps(4.0f * d);
    for (; i + 4 <= n; i += 4) { _mm_storeu_ps(out + i, v); v = _mm_add_ps(v, inc); }
#endif
    for (; i < n; ++i) out[i] = base + d * (float)i;
}
/** The coherent combine: id = (I − dcI), ed = (|z| − dcE); U = id − hq, L = id + hq. */
inline void sideSplit(const float* iD, const float* dcI, const float* hq, float* u, float* l, int n) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        const float32x4_t id = vsubq_f32(vld1q_f32(iD + i), vld1q_f32(dcI + i)), h = vld1q_f32(hq + i);
        vst1q_f32(u + i, vsubq_f32(id, h)); vst1q_f32(l + i, vaddq_f32(id, h));
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        const __m128 id = _mm_sub_ps(_mm_loadu_ps(iD + i), _mm_loadu_ps(dcI + i)), h = _mm_loadu_ps(hq + i);
        _mm_storeu_ps(u + i, _mm_sub_ps(id, h)); _mm_storeu_ps(l + i, _mm_add_ps(id, h));
    }
#endif
    for (; i < n; ++i) { const float id = iD[i] - dcI[i]; u[i] = id - hq[i]; l[i] = id + hq[i]; }
}
/** out = a·u + (1−a)·l (in place into u). */
inline void mix2(float* u, const float* l, float a, int n) {
    const float b = 1.0f - a;
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) vst1q_f32(u + i, vmlaq_n_f32(vmulq_n_f32(vld1q_f32(u + i), a), vld1q_f32(l + i), b));
#elif VIBE_SSE
    const __m128 av = _mm_set1_ps(a), bv = _mm_set1_ps(b);
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(u + i, _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(u + i), av), _mm_mul_ps(_mm_loadu_ps(l + i), bv)));
#endif
    for (; i < n; ++i) u[i] = a * u[i] + b * l[i];
}
/** out[i] = env + bl·(coh − env), env = (ed − dcE)·g, coh = c·g. */
inline void blendOut(const float* c, const float* ed, const float* dcE, const float* g, const float* bl, float* out, int n) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        const float32x4_t gv = vld1q_f32(g + i);
        const float32x4_t ev = vmulq_f32(vsubq_f32(vld1q_f32(ed + i), vld1q_f32(dcE + i)), gv);
        const float32x4_t cv = vmulq_f32(vld1q_f32(c + i), gv);
        vst1q_f32(out + i, vmlaq_f32(ev, vld1q_f32(bl + i), vsubq_f32(cv, ev)));
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        const __m128 gv = _mm_loadu_ps(g + i);
        const __m128 ev = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(ed + i), _mm_loadu_ps(dcE + i)), gv);
        const __m128 cv = _mm_mul_ps(_mm_loadu_ps(c + i), gv);
        _mm_storeu_ps(out + i, _mm_add_ps(ev, _mm_mul_ps(_mm_loadu_ps(bl + i), _mm_sub_ps(cv, ev))));
    }
#endif
    for (; i < n; ++i) { const float ev = (ed[i] - dcE[i]) * g[i]; const float cv = c[i] * g[i]; out[i] = ev + bl[i] * (cv - ev); }
}
/** C-QUAM-compatible decode, four at a time: S = (|y| − dcE)·g, D = |y|·Q/max(I, |y|/4)·g. */
inline void cquamDecode(const cf32* y, const float* dcE, const float* g, float* s, float* d, int n) {
    const float* f = reinterpret_cast<const float*>(y);
    int i = 0;
#if VIBE_NEON
    const float32x4_t q4 = vdupq_n_f32(0.25f), tiny = vdupq_n_f32(1e-20f);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(f + 2 * i);
        const float32x4_t m2 = vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]);
        // |y| = m2·rsqrt(m2), two Newton steps (the scalar path uses sqrt; agreement checked by the test)
        float32x4_t r = vrsqrteq_f32(vmaxq_f32(m2, tiny));
        r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(vmaxq_f32(m2, tiny), r), r));
        r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(vmaxq_f32(m2, tiny), r), r));
        const float32x4_t mag = vmulq_f32(m2, r);
        const float32x4_t gv = vld1q_f32(g + i);
        vst1q_f32(s + i, vmulq_f32(vsubq_f32(mag, vld1q_f32(dcE + i)), gv));
        const float32x4_t den = vmaxq_f32(v.val[0], vmaxq_f32(vmulq_f32(mag, q4), tiny));
        float32x4_t rec = vrecpeq_f32(den);
        rec = vmulq_f32(rec, vrecpsq_f32(den, rec));
        rec = vmulq_f32(rec, vrecpsq_f32(den, rec));
        vst1q_f32(d + i, vmulq_f32(vmulq_f32(vmulq_f32(mag, v.val[1]), rec), gv));
    }
#elif VIBE_SSE
    const __m128 q4 = _mm_set1_ps(0.25f), tiny = _mm_set1_ps(1e-20f);
    for (; i + 4 <= n; i += 4) {
        __m128 re, im; sseLoad2(f + 2 * i, re, im);
        const __m128 mag = _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im)));
        const __m128 gv = _mm_loadu_ps(g + i);
        _mm_storeu_ps(s + i, _mm_mul_ps(_mm_sub_ps(mag, _mm_loadu_ps(dcE + i)), gv));
        const __m128 den = _mm_max_ps(re, _mm_max_ps(_mm_mul_ps(mag, q4), tiny));
        _mm_storeu_ps(d + i, _mm_mul_ps(_mm_div_ps(_mm_mul_ps(mag, im), den), gv));
    }
#endif
    for (; i < n; ++i) {
        const float re = f[2 * i], im = f[2 * i + 1];
        const float mag = std::sqrt(re * re + im * im);
        s[i] = (mag - dcE[i]) * g[i];
        const float den = std::max(re, std::max(mag * 0.25f, 1e-20f));
        d[i] = mag * im / den * g[i];
    }
}
}  // namespace

// ── biquads ──────────────────────────────────────────────────────────────────
void SyncAm::Biq::lp(double fs, double f, double q) {
    const double w0 = 2.0 * M_PI * std::min(f, fs * 0.49) / fs;
    const double c = std::cos(w0), al = std::sin(w0) / (2.0 * q), a0 = 1.0 + al;
    b0 = (float)((1.0 - c) * 0.5 / a0); b1 = (float)((1.0 - c) / a0); b2 = b0;
    a1 = (float)(-2.0 * c / a0); a2 = (float)((1.0 - al) / a0);
}
void SyncAm::Biq::notch(double fs, double f, double q) {
    const double w0 = 2.0 * M_PI * std::min(f, fs * 0.49) / fs;
    const double c = std::cos(w0), al = std::sin(w0) / (2.0 * q), a0 = 1.0 + al;
    b0 = (float)(1.0 / a0); b1 = (float)(-2.0 * c / a0); b2 = b0;
    a1 = (float)(-2.0 * c / a0); a2 = (float)((1.0 - al) / a0);
}
void SyncAm::Lp8::design(double fs, double fc) {
    if (fc == f) return;      // ★ the states are KEPT across a redesign: no restart transient
    f = (float)fc;
    // 8th-order Butterworth: Q_k = 1 / (2 cos((2k−1)π/16))
    static const double kQ[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
    for (int k = 0; k < 4; ++k) s[k].lp(fs, fc, kQ[k]);
}
void SyncAm::Lp8::run(float* x, int n) {
    for (auto& b : s) {
        float z1 = b.z1, z2 = b.z2;
        const float b0 = b.b0, b1 = b.b1, b2 = b.b2, a1 = b.a1, a2 = b.a2;
        for (int i = 0; i < n; ++i) { const float y = b0 * x[i] + z1; z1 = b1 * x[i] - a1 * y + z2; z2 = b2 * x[i] - a2 * y; x[i] = y; }
        b.z1 = std::isfinite(z1) ? z1 : 0.0f; b.z2 = std::isfinite(z2) ? z2 : 0.0f;
    }
}

SyncAm::SyncAm() = default;
SyncAm::~SyncAm() = default;

void SyncAm::configure(Det d, double rate, double userEdgeHz) {
    det_ = d; fs_ = rate;
    userEdge_ = std::max(200.0, std::min(userEdgeHz, rate * 0.45));
    const double fu = rate / K, T = 1.0 / fu;
    auto gains = [&](double fnHz, float& kp, float& ki) {
        const double wn = 2.0 * M_PI * fnHz * T, zeta = 0.7071;
        kp = (float)(2.0 * zeta * wn); ki = (float)(wn * wn);
    };
    gains(20.0, kpAcq_, kiAcq_);
    // ★ STEREO tracks slower: the loop must not follow the 25 Hz pilot or the bass of L−R.
    gains(d == Det::STEREO ? 2.5 : 5.0, kpTrk_, kiTrk_);
    aLpAcq_ = poleFor(fu, 120.0);
    aLpTrk_ = poleFor(fu, d == Det::STEREO ? 10.0 : 30.0);
    aRef_  = poleTau(fu, 0.5);
    aLock_ = poleTau(fu, 0.3);
    aDc_   = poleFor(fu, 20.0);
    aLvl_  = poleTau(fu, 2.0);
    aLvlUp_ = poleTau(fu, 0.3);
    aBlUp_ = poleTau(fu, 0.15);
    aBlDn_ = poleTau(fu, 0.30);
    aPil_  = poleTau(fu, 0.25);
    aSep_  = poleTau(rate, 0.3);
    pilotStep_ = (float)(2.0 * M_PI * 25.0 / fu);
    maxOmega_ = 2.0 * M_PI * 1200.0 / rate * K;
    // acquisition: ~0.1 s of signal
    acqN_ = 1024; while (acqN_ < rate * 0.1 && acqN_ < 16384) acqN_ *= 2;
    acqFft_ = std::make_unique<ComplexFFT>(acqN_);
    acqBuf_.assign((size_t)acqN_, cf32(0, 0)); acqOut_.assign((size_t)acqN_, cf32(0, 0));
    // Hilbert: lower passband edge ~150 Hz (3·fs/N for the Blackman window), capped for cost.
    hM_ = 0; hTaps_.clear();
    if (coherent()) {
        hM_ = std::max(31, std::min(511, (int)std::lround(rate / 100.0)));
        if ((hM_ & 1) == 0) ++hM_;
        for (int m = 1; m <= hM_; m += 2) {
            const double w = 0.42 + 0.5 * std::cos(M_PI * m / (hM_ + 1)) + 0.08 * std::cos(2.0 * M_PI * m / (hM_ + 1));
            hTaps_.push_back((float)(2.0 / (M_PI * m) * w));
        }
    }
    // measurement: ~10 frames a second, ~35 Hz bins
    mN_ = rate > 24000.0 ? 1024 : 512;
    mFft_ = std::make_unique<ComplexFFT>(mN_);
    mBuf_.assign((size_t)mN_, cf32(0, 0)); mOut_.assign((size_t)mN_, cf32(0, 0));
    mWin_.resize((size_t)mN_);
    for (int i = 0; i < mN_; ++i) mWin_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * M_PI * i / mN_));
    mHop_ = std::max(mN_, (int)(rate / 10.0));
    decideEvery_ = 5;
    mAlpha_ = 0.025f;          // ~4 s average (10 frames/s): long enough to ride over a pause
    env_.reset(); rfCut_.reset();
    if (d == Det::ENV) env_ = std::make_unique<AmDemod>();
    if (d == Det::ENV || d == Det::STEREO) { rfCut_ = std::make_unique<AdaptiveIf>(); rfCut_->configure(rate); }
    reset();
}

void SyncAm::setUserEdge(double e) {
    userEdge_ = std::max(200.0, std::min(e, fs_ * 0.45));
    if (edgeU_ > userEdge_ || edgeU_ <= 0.0f) edgeU_ = (float)userEdge_;
    if (edgeL_ > userEdge_ || edgeL_ <= 0.0f) edgeL_ = (float)userEdge_;
    applyEdges_();
}

void SyncAm::retune() {
    st_ = St::ACQ; stCount_ = 0; badCount_ = 0; acqFill_ = 0;
    kp_ = kpAcq_; ki_ = kiAcq_; aLp_ = aLpAcq_;
    s1r_ = s1i_ = s2r_ = s2i_ = 0; ampRef_ = 0; lockNum_ = lockDen_ = 0;
    // measurement + every auto-bandwidth verdict: a limit never travels with the dial
    std::fill(pU_.begin(), pU_.end(), 0.0f); std::fill(pL_.begin(), pL_.end(), 0.0f); std::fill(pX_.begin(), pX_.end(), 0.0f);
    mFrames_ = 0; mFill_ = 0; mSkip_ = 0; sinceDecide_ = 0; narrowVotes_[0] = narrowVotes_[1] = 0; widenVotes_[0] = widenVotes_[1] = 0;
    edgeU_ = edgeL_ = (float)userEdge_; mrcTarget_ = 0.5f; mrcHiT_ = 0.5f; splitF_ = (float)userEdge_; notchF_[0] = notchF_[1] = 0.0f;
    pilotOn_ = false; pilotCount_ = 0; p1r_ = p1i_ = p2r_ = p2i_ = 0; pilotAmp_ = 0;
    applyEdges_();
}

void SyncAm::reset() {
    pos_ = 0; cr_ = 1.0f; ci_ = 0.0f; omega_ = 0.0; stepC_ = 1.0f; stepS_ = 0.0f;
    sI_ = sQ_ = sM_ = 0; dcE1_ = dcE2_ = 0; lvl_ = 0; blend_ = 0;
    dcIa_ = dcIb_ = dcEa_ = dcEb_ = ga_ = gb_ = bla_ = blb_ = 0;
    everLocked_ = false; mrcA_ = 0.5f; mrcHi_ = 0.5f;
    pU_.assign((size_t)mN_ / 2, 0.0f); pL_.assign((size_t)mN_ / 2, 0.0f); pX_.assign((size_t)mN_ / 2, 0.0f);
    dI_.assign((size_t)hM_, 0.0f); dE_.assign((size_t)hM_, 0.0f); hBuf_.assign((size_t)(2 * hM_), 0.0f);
    lpA_ = Lp8{}; lpW1_ = Lp8{}; lpW2_ = Lp8{};
    notch_[0] = Biq{}; notch_[1] = Biq{};
    sep_ = 0; pilotPh_ = 0; dSum_ = 0; dPos_ = 0;
    if (env_) env_->reset();
    if (rfCut_) rfCut_->reset();
    retune();
}

void SyncAm::applyEdges_() {
    if (fs_ <= 0.0) return;
    const double E = userEdge_;
    const double inactive = std::min(E * 1.3, fs_ * 0.45);
    float eu = edgeU_, el = edgeL_;
    if (forceHi_ > 0.0f) eu = std::min(forceHi_, (float)E);
    if (forceLo_ > 0.0f) el = std::min(forceLo_, (float)E);
    const double fu = eu >= E - 1.0 ? inactive : eu, fl = el >= E - 1.0 ? inactive : el;
    switch (det_) {
        case Det::SAM_U: case Det::ECSS_U: lpA_.design(fs_, fu); break;
        case Det::SAM_L: case Det::ECSS_L: lpA_.design(fs_, fl); break;
        case Det::SAM:
            // ★ no hard cut in SAM: the dirtier side's region above the split is weighted down, not removed
            splitF_ = (float)std::min(eu, el);
            if (splitF_ < E - 1.0 && splitF_ != hLowF_ && hM_ > 0) designHilbertLow_(splitF_);
            break;
        case Det::ENV: case Det::STEREO: {
            const double e = std::min(eu, el);
            if (rfCut_) rfCut_->setBandwidth(e < E - 1.0 ? 2.0 * e : 0.0);
            break;
        }
    }
    for (int s = 0; s < 2; ++s) if (notchF_[s] > 0.0f) notch_[s].notch(fs_, notchF_[s], std::max(5.0, notchF_[s] / 40.0));
}

// ── acquisition: where is the carrier? ───────────────────────────────────────
void SyncAm::acquire_(const cf32* in, int n) {
    for (int i = 0; i < n && acqFill_ < acqN_; ++i) acqBuf_[(size_t)acqFill_++] = in[i];
    if (acqFill_ < acqN_) return;
    acqFill_ = 0;
    // ★ The raw input, not y: the search is for the carrier's frequency in the channel itself.
    for (int i = 0; i < acqN_; ++i) acqBuf_[i] *= (float)(0.5 - 0.5 * std::cos(2.0 * M_PI * i / acqN_));
    acqFft_->forward(acqBuf_.data(), acqOut_.data());
    const double bin = fs_ / acqN_;
    const int kmax = std::min(acqN_ / 2 - 2, (int)(1000.0 / bin));
    std::vector<float> p((size_t)(2 * kmax + 1));
    int best = 0; float bestP = -1.0f;
    for (int k = -kmax; k <= kmax; ++k) {
        const cf32 v = acqOut_[(size_t)((k + acqN_) % acqN_)];
        const float pw = std::norm(v);
        p[(size_t)(k + kmax)] = pw;
        if (pw > bestP) { bestP = pw; best = k; }
    }
    std::vector<float> sorted = p;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float med = sorted[sorted.size() / 2];
    // ★ A carrier is a LINE: 15 dB over the median of ±1 kHz (programme and noise are spread).
    if (!(bestP > 30.0f * med) || bestP <= 0.0f) return;
    double delta = 0.0;
    if (best > -kmax && best < kmax) {
        const double a = std::log(p[(size_t)(best + kmax - 1)] + 1e-30), b = std::log(bestP + 1e-30),
                     c = std::log(p[(size_t)(best + kmax + 1)] + 1e-30);
        const double den = a - 2.0 * b + c;
        if (den < 0.0) delta = std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den));
    }
    const double fHz = (best + delta) * bin;
    omega_ = 2.0 * M_PI * fHz / fs_ * K;
    stepC_ = (float)std::cos(omega_ / K); stepS_ = (float)std::sin(omega_ / K);
    st_ = St::PULL; stCount_ = 0; badCount_ = 0;
    kp_ = kpAcq_; ki_ = kiAcq_; aLp_ = aLpAcq_;
    s1r_ = s1i_ = s2r_ = s2i_ = 0; ampRef_ = 0; lockNum_ = lockDen_ = 0;
}

// ── the loop, once per K samples ─────────────────────────────────────────────
void SyncAm::loopUpdate_() {
    const float inv = 1.0f / K;
    const float ybr = sI_ * inv, ybi = sQ_ * inv, mb = sM_ * inv;
    sI_ = sQ_ = sM_ = 0.0f;
    s1r_ += aLp_ * (ybr - s1r_); s1i_ += aLp_ * (ybi - s1i_);
    s2r_ += aLp_ * (s1r_ - s2r_); s2i_ += aLp_ * (s1i_ - s2i_);
    const float amp2 = s2r_ * s2r_ + s2i_ * s2i_, amp = std::sqrt(amp2);
    dcE1_ += aDc_ * (mb - dcE1_); dcE2_ += aDc_ * (dcE1_ - dcE2_);
    // ★ The level reference rises in 0.3 s and falls over 2 s: a station that fades (or goes) must not have its
    //   NOISE lifted to programme level within a second — that is the pumping this exists to remove.
    if (lvl_ <= 0.0f) lvl_ = dcE2_ > 0.0f ? dcE2_ : mb;
    else lvl_ += (dcE2_ > lvl_ ? aLvlUp_ : aLvl_) * (dcE2_ - lvl_);
    if (st_ != St::ACQ) {
        if (ampRef_ <= 0.0f) ampRef_ = amp; else ampRef_ += aRef_ * (amp - ampRef_);
        float w = amp / (ampRef_ + 1e-20f); if (w > 1.0f) w = 1.0f;
        const float w2 = w * w;
        const float e = fastAtan2(s2i_, s2r_);
        const float ew = e * w2;                       // ★ the flywheel: a faded carrier barely steers
        omega_ += ki_ * ew;
        if (omega_ > maxOmega_) omega_ = maxOmega_; else if (omega_ < -maxOmega_) omega_ = -maxOmega_;
        const float kick = kp_ * ew;
        const float kc = std::cos(kick), ks = std::sin(kick);
        const float nr = cr_ * kc - ci_ * ks, ni = cr_ * ks + ci_ * kc; cr_ = nr; ci_ = ni;
        stepC_ = (float)std::cos(omega_ / K); stepS_ = (float)std::sin(omega_ / K);
        const float c2 = (s2r_ * s2r_ - s2i_ * s2i_) / (amp2 + 1e-30f);
        lockNum_ += aLock_ * (w2 * c2 - lockNum_); lockDen_ += aLock_ * (w2 - lockDen_);
        const float lq = lockNum_ / (lockDen_ + 1e-30f);
        const int fu = (int)(fs_ / K);
        ++stCount_;
        if (st_ == St::PULL) {
            if (stCount_ > fu / 2 && lq > 0.6f) {
                st_ = St::TRACK; kp_ = kpTrk_; ki_ = kiTrk_; aLp_ = aLpTrk_; badCount_ = 0; everLocked_ = true;
            } else if (stCount_ > 2 * fu) { st_ = St::ACQ; acqFill_ = 0; }
        } else {                                        // TRACK
            badCount_ = lq < 0.3f ? badCount_ + 1 : 0;
            if (badCount_ > 2 * fu) {
                st_ = St::ACQ; acqFill_ = 0;
                // ★ no lock = no honest measurement: open the filter back up rather than hold a cut
                edgeU_ = edgeL_ = (float)userEdge_; notchF_[0] = notchF_[1] = 0.0f; mrcTarget_ = 0.5f; mrcHiT_ = 0.5f;
                mFrames_ = 0; applyEdges_();
            }
        }
    }
    const float lq = lockNum_ / (lockDen_ + 1e-30f);
    float bt = 0.0f;
    if (st_ == St::TRACK) bt = std::max(0.0f, std::min(1.0f, (lq - 0.35f) / 0.35f));
    if (!safety_ && st_ != St::ACQ) bt = 1.0f;
    blend_ += (bt > blend_ ? aBlUp_ : aBlDn_) * (bt - blend_);
    if (!std::isfinite(omega_) || !std::isfinite(cr_) || !std::isfinite(ci_) || !std::isfinite(blend_) ||
        !std::isfinite(lvl_) || !std::isfinite(s2r_) || !std::isfinite(dcE2_)) {
        reset(); return;   // ★ recursive state: a NaN here would be forever
    }
    dcIa_ = dcIb_; dcIb_ = s2r_;
    dcEa_ = dcEb_; dcEb_ = dcE2_;
    ga_ = gb_; gb_ = carrierLevel_ ? 0.5f / std::max(lvl_, 1e-12f) : 2.0f;
    bla_ = blb_; blb_ = blend_;
}

// ── Hilbert: out[i] = Σ h_m (x[i−M−m] − x[i−M+m]) — four outputs per tap ────
/** ★★ The BAND-LIMITED Hilbert, |f| < splitF_: h[m] = (1 − cos ω_s m)/(π m) for every m ≠ 0 (the full Hilbert is its
 *  ω_s = π case, where the even taps vanish). Same length and window as the full one, so the SAME delay — which is what
 *  lets SAM split the band into two regions with no phase error between them (an IIR split did not: −0.9 dB on 1044 kHz). */
void SyncAm::hilbertLow_(int n, float* out) {
    const int M = hM_;
    const float* b = hBuf_.data();
    const float* h = hLowTaps_.data();
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        float32x4_t acc = vdupq_n_f32(0.0f);
        const float* c = b + i + M;
        for (int m = 1; m <= M; ++m) acc = vmlaq_n_f32(acc, vsubq_f32(vld1q_f32(c - m), vld1q_f32(c + m)), h[m - 1]);
        vst1q_f32(out + i, acc);
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        __m128 acc = _mm_setzero_ps();
        const float* c = b + i + M;
        for (int m = 1; m <= M; ++m) acc = _mm_add_ps(acc, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(c - m), _mm_loadu_ps(c + m)), _mm_set1_ps(h[m - 1])));
        _mm_storeu_ps(out + i, acc);
    }
#endif
    for (; i < n; ++i) {
        float acc = 0.0f;
        const float* c = b + i + M;
        for (int m = 1; m <= M; ++m) acc += h[m - 1] * (c[-m] - c[m]);
        out[i] = acc;
    }
}
void SyncAm::designHilbertLow_(double fSplit) {
    hLowTaps_.assign((size_t)hM_, 0.0f);
    const double ws = 2.0 * M_PI * fSplit / fs_;
    for (int m = 1; m <= hM_; ++m) {
        const double w = 0.42 + 0.5 * std::cos(M_PI * m / (hM_ + 1)) + 0.08 * std::cos(2.0 * M_PI * m / (hM_ + 1));
        hLowTaps_[(size_t)(m - 1)] = (float)((1.0 - std::cos(ws * m)) / (M_PI * m) * w);
    }
    hLowF_ = (float)fSplit;
}
void SyncAm::hilbert_(const float* x, int n, float* out, bool compute, float* outLow) {
    const int M = hM_, H = 2 * M;
    hBuf_.resize((size_t)(H + n));
    std::memcpy(hBuf_.data() + H, x, sizeof(float) * (size_t)n);
    if (outLow) hilbertLow_(n, outLow);
    if (!compute) {                 // ★ history kept, so the day it is needed its first output is already right
        std::memset(out, 0, sizeof(float) * (size_t)n);
        std::memmove(hBuf_.data(), hBuf_.data() + n, sizeof(float) * (size_t)H);
        return;
    }
    const float* b = hBuf_.data();
    const int T = (int)hTaps_.size();
    const float* h = hTaps_.data();
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        float32x4_t acc = vdupq_n_f32(0.0f);
        const float* c = b + i + M;
        for (int t = 0; t < T; ++t) {
            const int m = 2 * t + 1;
            acc = vmlaq_n_f32(acc, vsubq_f32(vld1q_f32(c - m), vld1q_f32(c + m)), h[t]);
        }
        vst1q_f32(out + i, acc);
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        __m128 acc = _mm_setzero_ps();
        const float* c = b + i + M;
        for (int t = 0; t < T; ++t) {
            const int m = 2 * t + 1;
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(c - m), _mm_loadu_ps(c + m)), _mm_set1_ps(h[t])));
        }
        _mm_storeu_ps(out + i, acc);
    }
#endif
    for (; i < n; ++i) {
        float acc = 0.0f;
        const float* c = b + i + M;
        for (int t = 0; t < T; ++t) { const int m = 2 * t + 1; acc += h[t] * (c[-m] - c[m]); }
        out[i] = acc;
    }
    std::memmove(hBuf_.data(), hBuf_.data() + n, sizeof(float) * (size_t)H);
}

// ── measurement: the per-sideband S/N, judged on the uncut signal ───────────
void SyncAm::measure_(const float* yI, const float* yQ, int n) {
    int i = 0;
    while (i < n) {
        if (mSkip_ > 0) { const int k = std::min(mSkip_, n - i); mSkip_ -= k; i += k; continue; }
        const int k = std::min(mN_ - mFill_, n - i);
        for (int j = 0; j < k; ++j) mBuf_[(size_t)(mFill_ + j)] = cf32(yI[i + j] * mWin_[mFill_ + j], yQ[i + j] * mWin_[mFill_ + j]);
        mFill_ += k; i += k;
        if (mFill_ < mN_) break;
        mFill_ = 0; mSkip_ = mHop_ - mN_;
        mFft_->forward(mBuf_.data(), mOut_.data());
        const int half = mN_ / 2;
        const float a = mFrames_ == 0 ? 1.0f : mAlpha_;
        for (int b = 1; b < half; ++b) {
            const cf32 u = mOut_[(size_t)b], l = mOut_[(size_t)(mN_ - b)];
            const float pu = std::norm(u), pl = std::norm(l), x = u.real() * l.real() - u.imag() * l.imag();
            pU_[b] += a * (pu - pU_[b]); pL_[b] += a * (pl - pL_[b]); pX_[b] += a * (x - pX_[b]);
        }
        ++mFrames_;
        if (++sinceDecide_ >= decideEvery_) { sinceDecide_ = 0; decide_(); }
    }
}

void SyncAm::decide_() {
    if (!autoBw_) {
        if (edgeU_ < userEdge_ || edgeL_ < userEdge_ || notchF_[0] > 0 || notchF_[1] > 0 || mrcTarget_ != 0.5f || mrcHiT_ != 0.5f) {
            edgeU_ = edgeL_ = (float)userEdge_; notchF_[0] = notchF_[1] = 0; mrcTarget_ = 0.5f; mrcHiT_ = 0.5f; applyEdges_();
        }
        return;
    }
    if (st_ != St::TRACK || mFrames_ < 25) return;      // ★ 2.5 s of evidence before any cut
    const double bin = fs_ / mN_, E = userEdge_;
    const int half = mN_ / 2;
    constexpr double kBand = 250.0, kLow = 150.0;
    const int nb = std::max(1, (int)((E - kLow) / kBand));
    std::vector<double> S((size_t)nb, 0.0), Nu((size_t)nb, 0.0), Nl((size_t)nb, 0.0);
    for (int k = 1; k < half; ++k) {
        const double f = k * bin;
        if (f < kLow || f >= kLow + nb * kBand) continue;
        const int b = (int)((f - kLow) / kBand);
        const double s = std::max(0.0, (double)pX_[k]);
        S[b] += s; Nu[b] += std::max(1e-30, pU_[k] - s); Nl[b] += std::max(1e-30, pL_[k] - s);
    }
    const bool sideModes = det_ != Det::STEREO;           // ★ STEREO's L−R is not mirrored: no noise arm
    double want[2];                                        // [0] lower, [1] upper
    for (int side = 0; side < 2; ++side) {
        const std::vector<double>& Ns = side ? Nu : Nl;
        const std::vector<double>& No = side ? Nl : Nu;
        double e = E;
        if (armNoise_ && sideModes) {
            double prog = -1.0;
            for (int b = nb - 1; b >= 0; --b) if (S[b] >= 2.0 * Ns[b]) { prog = kLow + (b + 1) * kBand; break; }
            if (prog > 0.0) e = std::max(2000.0, prog + 200.0);
        }
        if (armInterf_) {
            /* ★★ A NEIGHBOUR, not noise: this side 6 dB dirtier than the other AND the dirt above OUR programme in
             *  that band (twice it where the cut is symmetric, envelope/stereo: there it costs both sides). The cut
             *  goes at the lowest band from which MOST (≥ 60 %) of the bands up to the edge are dirty — a neighbour's
             *  sideband is contiguous to the edge; a single dirty band is not a reason to throw away everything above.
             *  The first version took the first dirty band at all and chattered 1.5 ↔ 4.4 kHz on 7350 under 7355. */
            const double need = 4.0;   // ★ measured: 1× (coherent) / 2× (envelope) narrowed MW skywave stations it then hurt
            std::vector<char> dirty((size_t)nb, 0);
            for (int b = 0; b < nb; ++b) dirty[b] = Ns[b] > interfRatio_ * No[b] && Ns[b] > need * S[b];
            for (int b = 0; b < nb; ++b) {
                const double f0 = kLow + b * kBand;
                if (f0 < 1500.0) continue;
                int d = 0; for (int c = b; c < nb; ++c) d += dirty[c];
                if (dirty[b] && d >= 0.6 * (nb - b)) { e = std::min(e, std::max(1500.0, f0 - 250.0)); break; }
            }
        }
        want[side] = std::min(e, E);
    }
    // heterodyne: a steady line on ONE side only
    float newNotch[2] = { 0.0f, 0.0f };
    if (armNotch_) {
        for (int side = 0; side < 2; ++side) {
            const std::vector<float>& Ps = side ? pU_ : pL_;
            const std::vector<float>& Po = side ? pL_ : pU_;
            int bestK = -1; double bestR = 0.0;
            const int k0 = (int)(800.0 / bin), k1 = std::min(half - 12, (int)(E / bin));
            std::vector<float> loc;
            for (int k = std::max(k0, 12); k <= k1; ++k) {
                loc.assign(Ps.begin() + (k - 10), Ps.begin() + (k + 11));
                std::nth_element(loc.begin(), loc.begin() + 10, loc.end());
                const double med = std::max(1e-30, (double)loc[10]);
                const double r = Ps[k] / med;
                if (r > 40.0 && Ps[k] > 10.0 * Po[k] && Ps[k] >= Ps[k - 1] && Ps[k] >= Ps[k + 1] && r > bestR) { bestR = r; bestK = k; }
            }
            if (bestK > 0) {
                const double a = std::log(Ps[bestK - 1] + 1e-30), b = std::log(Ps[bestK] + 1e-30), c = std::log(Ps[bestK + 1] + 1e-30);
                const double den = a - 2.0 * b + c;
                const double d = den < 0.0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0.0;
                const float f = (float)((bestK + d) * bin);
                if (f < want[side] - 100.0) newNotch[side] = f;
            }
        }
    }
    bool changed = false;
    float* cur[2] = { &edgeL_, &edgeU_ };
    for (int side = 0; side < 2; ++side) {
        /* ★★ NARROW SLOWLY, WIDEN SOON, NEVER CHATTER. A narrowing needs four decisions running (2 s) and goes to
         *  the WIDEST of them (the least it can do that all four agreed on); a widening needs two (1 s). Each move
         *  is audible, so a late one beats a wrong one — the lesson of the FM controller. */
        const float w = (float)want[side];
        if (w < *cur[side] - 250.0f) {
            narrowMax_[side] = narrowVotes_[side] == 0 ? w : std::max(narrowMax_[side], w);
            widenVotes_[side] = 0;
            if (++narrowVotes_[side] >= 4) { *cur[side] = narrowMax_[side]; narrowVotes_[side] = 0; changed = true; }
        } else if (w > *cur[side] + 250.0f) {
            narrowVotes_[side] = 0;
            widenMin_[side] = widenVotes_[side] == 0 ? w : std::min(widenMin_[side], w);
            if (++widenVotes_[side] >= 2) { *cur[side] = widenMin_[side]; widenVotes_[side] = 0; changed = true; }
        } else { narrowVotes_[side] = 0; widenVotes_[side] = 0; }
        // notch: adopt only a line seen on two decisions running (±2 bins)
        if (newNotch[side] > 0.0f && notchF_[side] > 0.0f && std::fabs(newNotch[side] - notchF_[side]) < 2.0 * bin) {
            /* hold */
        } else if (newNotch[side] > 0.0f) {
            if (std::fabs(notchPend_[side] - newNotch[side]) < 2.0 * bin) { notchF_[side] = newNotch[side]; changed = true; }
            notchPend_[side] = newNotch[side];
        } else if (notchF_[side] > 0.0f) { notchF_[side] = 0.0f; changed = true; }
    }
    /* ★★★ SAM DOES NOT CUT, IT WEIGHS (2026-10-31, measured). A hard cut of the dirty side throws away the 3 dB a
     *  second copy of the programme is worth: on 972 kHz under a stronger 963 it cost 1.9 dB of coherence score
     *  against plain SAM. So the band is split where the neighbour starts, and each region combines the two
     *  sidebands with maximum-ratio weights from their own measured noise+interference (w_U = N_L / (N_U + N_L)):
     *  below the split usually 0.5 (plain DSB), above it the clean side dominates without the dirty one going to
     *  zero. Without a neighbour there is one region and the weights stay at 0.5 unless the sides differ by 3 dB. */
    if (det_ == Det::SAM) {
        const double split = std::min(edgeU_, edgeL_);
        double nuLo = 0, nlLo = 0, nuHi = 0, nlHi = 0;
        for (int b = 0; b < nb; ++b) {
            const double f1 = kLow + (b + 1) * kBand;
            if (f1 <= split) { nuLo += Nu[b]; nlLo += Nl[b]; } else { nuHi += Nu[b]; nlHi += Nl[b]; }
        }
        float lo = 0.5f, hi = 0.5f;
        if (armMrc_ && nuLo > 0 && nlLo > 0 && (nuLo > 4.0 * nlLo || nlLo > 4.0 * nuLo))
            lo = (float)std::max(0.05, std::min(0.95, nlLo / (nuLo + nlLo)));
        if (split < userEdge_ - 1.0 && nuHi > 0 && nlHi > 0) hi = (float)std::max(0.02, std::min(0.98, nlHi / (nuHi + nlHi)));
        else hi = lo;
        mrcTarget_ = lo; mrcHiT_ = hi;
    } else { mrcTarget_ = 0.5f; mrcHiT_ = 0.5f; }
    if (changed) applyEdges_();
}

// ── the block ────────────────────────────────────────────────────────────────
void SyncAm::process(const cf32* in, float* outL, float* outR, int n) {
    if (n <= 0) return;
    if (det_ == Det::ENV && !autoBw_) {          // ★ plain AM, byte-for-byte the shipping detector
        env_->process(in, outL, n);
        if (outR) std::memcpy(outR, outL, sizeof(float) * (size_t)n);
        return;
    }
    const size_t N = (size_t)n;
    if (yI_.size() < N) { yI_.resize(N); yQ_.resize(N); mag_.resize(N); dcI_.resize(N); dcE_.resize(N); g_.resize(N); bl_.resize(N);
                          hq_.resize(N); tmpA_.resize(N); tmpB_.resize(N); tmpC_.resize(N); d_.resize(N); }
    magnitudes(in, mag_.data(), n);
    // 1. the tracker
    int i = 0;
    while (i < n) {
        const int len = std::min(K - pos_, n - i);
        if (st_ == St::ACQ) acquire_(in + i, len);
        rotateBlockIlv(in + i, len, cr_, ci_, stepC_, stepS_, yI_.data() + i, yQ_.data() + i, nullptr, nullptr);
        sI_ += sumBlock(yI_.data() + i, len); sQ_ += sumBlock(yQ_.data() + i, len); sM_ += sumBlock(mag_.data() + i, len);
        rampFill(dcI_.data() + i, len, dcIa_, dcIb_, pos_, K);
        rampFill(dcE_.data() + i, len, dcEa_, dcEb_, pos_, K);
        rampFill(g_.data() + i, len, ga_, gb_, pos_, K);
        rampFill(bl_.data() + i, len, bla_, blb_, pos_, K);
        pos_ += len; i += len;
        if (pos_ == K) { pos_ = 0; loopUpdate_(); }
    }
    // 2. the measurement, on the uncut signal
    if (autoBw_ || forceLo_ > 0.0f || forceHi_ > 0.0f) {
        if (st_ == St::TRACK) measure_(yI_.data(), yQ_.data(), n);
    } else if (edgeU_ < userEdge_ || edgeL_ < userEdge_ || notchF_[0] > 0.0f || notchF_[1] > 0.0f) decide_();
    mrcA_ += 0.02f * (mrcTarget_ - mrcA_);
    if (std::fabs(mrcA_ - mrcTarget_) < 0.001f) mrcA_ = mrcTarget_;
    mrcHi_ += 0.02f * (mrcHiT_ - mrcHi_);
    if (std::fabs(mrcHi_ - mrcHiT_) < 0.001f) mrcHi_ = mrcHiT_;
    // 3. the detector
    switch (det_) {
        case Det::ENV: {
            yc_.resize(N);
            for (int k = 0; k < n; ++k) yc_[k] = cf32(yI_[k], yQ_[k]);
            if (rfCut_) rfCut_->process(yc_.data(), n);
            env_->process(yc_.data(), outL, n);
            for (int s = 0; s < 2; ++s) if (notchF_[s] > 0.0f) for (int k = 0; k < n; ++k) outL[k] = notch_[s].step(outL[k]);
            if (outR) std::memcpy(outR, outL, sizeof(float) * N);
            break;
        }
        case Det::STEREO: {
            yc_.resize(N);
            for (int k = 0; k < n; ++k) yc_[k] = cf32(yI_[k], yQ_[k]);
            if (rfCut_) rfCut_->process(yc_.data(), n);
            float* S = tmpA_.data(); float* D = d_.data();
            cquamDecode(yc_.data(), dcE_.data(), g_.data(), S, D, n);
            // pilot: 25 Hz on L−R, detected synchronously at the loop rate and subtracted exactly
            const float lockOk = blend_;
            const float target = (pilotOn_ && lockOk > 0.8f) ? 1.0f : 0.0f;
            const float ws = (float)(2.0 * M_PI * 25.0 / fs_);
            const float wc = std::cos(ws), wsn = std::sin(ws);
            float pc = std::cos(pilotPh_), ps = std::sin(pilotPh_);
            for (int k = 0; k < n; ++k) {
                dSum_ += D[k];
                // reconstructed pilot 2·Re(P·e^{jψ}) and its removal
                const float pil = 2.0f * (p2r_ * pc - p2i_ * ps);
                D[k] -= pil;
                if (++dPos_ == K) {
                    const float m = dSum_ / K; dSum_ = 0.0f; dPos_ = 0;
                    if (st_ == St::TRACK) {
                        const float mr = m * pc, mi = -m * ps;     // m·e^{−jψ}
                        p1r_ += aPil_ * (mr - p1r_); p1i_ += aPil_ * (mi - p1i_);
                        p2r_ += aPil_ * (p1r_ - p2r_); p2i_ += aPil_ * (p1i_ - p2i_);
                        pilotAmp_ = 2.0f * std::sqrt(p2r_ * p2r_ + p2i_ * p2i_);
                        // the pilot's phase must stand still against our 25 Hz (a real pilot is crystal)
                        const float ph = std::atan2(p2i_, p2r_);
                        float dph = ph - pilotPrevPh_;
                        if (dph > (float)M_PI) dph -= 2.0f * (float)M_PI; else if (dph < -(float)M_PI) dph += 2.0f * (float)M_PI;
                        pilotPrevPh_ = ph;
                        pilotDrift_ += 0.02f * (std::fabs(dph) * (float)(fs_ / K) - pilotDrift_);   // rad/s
                        const int fu = (int)(fs_ / K);
                        const bool present = pilotAmp_ > (pilotOn_ ? 0.008f : 0.015f) && pilotDrift_ < 2.0f * (float)M_PI * 0.5f;
                        if (present != pilotOn_) { if (++pilotCount_ > fu * 3 / 10) { pilotOn_ = present; pilotCount_ = 0; } }
                        else pilotCount_ = 0;
                    } else { pilotOn_ = false; p1r_ += aPil_ * (0 - p1r_); p1i_ += aPil_ * (0 - p1i_); p2r_ += aPil_ * (p1r_ - p2r_); p2i_ += aPil_ * (p1i_ - p2i_); }
                }
                const float npc = pc * wc - ps * wsn, nps = pc * wsn + ps * wc; pc = npc; ps = nps;
                sep_ += aSep_ * (target * std::min(1.0f, lockOk) - sep_);
                const float dd = sep_ * D[k];
                outL[k] = S[k] + dd;
                if (outR) outR[k] = S[k] - dd;
            }
            pilotPh_ = std::atan2(ps, pc);
            for (int s = 0; s < 2; ++s) if (notchF_[s] > 0.0f) {
                for (int k = 0; k < n; ++k) outL[k] = notch_[s].step(outL[k]);
            }
            if (!outR) break;
            if (notchF_[0] > 0.0f || notchF_[1] > 0.0f) {
                // ★ the same notch on R: mirror the L filter's design with its own state
                Biq* rn = notchR_;
                for (int s = 0; s < 2; ++s) if (notchF_[s] > 0.0f) {
                    rn[s].b0 = notch_[s].b0; rn[s].b1 = notch_[s].b1; rn[s].b2 = notch_[s].b2; rn[s].a1 = notch_[s].a1; rn[s].a2 = notch_[s].a2;
                    for (int k = 0; k < n; ++k) outR[k] = rn[s].step(outR[k]);
                }
            }
            break;
        }
        default: {                                         // the coherent family
            const int M = hM_;
            float* U = tmpA_.data(); float* L = tmpB_.data();
            // delayed I and |z| — aligned with the Hilbert's own delay (and, at ~10 ms, with the DC estimate's lag)
            dI_.resize((size_t)M + N); dE_.resize((size_t)M + N);
            std::memcpy(dI_.data() + M, yI_.data(), sizeof(float) * N);
            std::memcpy(dE_.data() + M, mag_.data(), sizeof(float) * N);
            /* ★ Plain DSB (equal weights, equal edges) is a·U + (1−a)·L = I exactly: the Hilbert term cancels, so it
             *  is not computed — that is most of SAM's cost, and the usual case. */
            const bool dsbPlain = det_ == Det::SAM && mrcA_ == 0.5f && mrcTarget_ == 0.5f && mrcHi_ == 0.5f && mrcHiT_ == 0.5f
                                  && notchF_[0] <= 0.0f && notchF_[1] <= 0.0f;
            const bool split = det_ == Det::SAM && splitF_ < (float)userEdge_ - 1.0f && std::fabs(mrcA_ - mrcHi_) > 1e-4f
                               && hLowF_ == splitF_;
            hqLow_.resize(N);
            hilbert_(yQ_.data(), n, hq_.data(), !dsbPlain, split ? hqLow_.data() : nullptr);
            sideSplit(dI_.data(), dcI_.data(), hq_.data(), U, L, n);
            if (notchF_[1] > 0.0f) for (int k = 0; k < n; ++k) U[k] = notch_[1].step(U[k]);
            if (notchF_[0] > 0.0f) for (int k = 0; k < n; ++k) L[k] = notch_[0].step(L[k]);
            float* C = U;
            switch (det_) {
                case Det::SAM_U: case Det::ECSS_U: lpA_.run(U, n); break;
                case Det::SAM_L: case Det::ECSS_L: lpA_.run(L, n); C = L; break;
                default: {                                 // SAM: both sides, two regions
                    /* C = a·U + (1−a)·L with U − L = −2·H{Q}: above the split a = aHi, below it aLo, so
                     *   out = aHi·U + (1−aHi)·L − 2(aLo − aHi)·H_low{Q}
                     * where H_low is the Hilbert band-limited to the split — linear phase, the full Hilbert's own delay. */
                    mix2(U, L, mrcHi_, n);
                    if (split) {
                        const float k2 = -2.0f * (mrcA_ - mrcHi_);
                        const float* hl = hqLow_.data();
                        for (int k = 0; k < n; ++k) U[k] += k2 * hl[k];
                    }
                    break;
                }
            }
            // envelope fallback, delayed the same, and the blend
            float* ed = dE_.data();
            blendOut(C, ed, dcE_.data(), g_.data(), bl_.data(), outL, n);
            std::memmove(dI_.data(), dI_.data() + n, sizeof(float) * (size_t)M);
            std::memmove(dE_.data(), dE_.data() + n, sizeof(float) * (size_t)M);
            if (outR) std::memcpy(outR, outL, sizeof(float) * N);
            break;
        }
    }
}

SyncAm::Status SyncAm::status() const {
    Status s;
    s.lockQ = lockDen_ > 0.0f ? std::max(0.0f, std::min(1.0f, lockNum_ / lockDen_)) : 0.0f;
    s.blend = blend_;
    s.locked = st_ == St::TRACK && blend_ > 0.5f;
    s.offsetHz = (float)(omega_ / K * fs_ / (2.0 * M_PI));
    s.pilot = pilotOn_ && det_ == Det::STEREO;
    s.pilotLevel = pilotAmp_;
    s.sep = sep_;
    const float E = (float)userEdge_;
    const float lo = forceLo_ > 0.0f ? std::min(forceLo_, E) : edgeL_, hi = forceHi_ > 0.0f ? std::min(forceHi_, E) : edgeU_;
    switch (det_) {
        case Det::SAM_U: case Det::ECSS_U: s.edgeLoHz = 0.0f; s.edgeHiHz = hi; break;
        case Det::SAM_L: case Det::ECSS_L: s.edgeLoHz = lo; s.edgeHiHz = 0.0f; break;
        case Det::SAM:   // ★ shown as an edge only where that side is really weighted down (< 0.5) above the split
            s.edgeLoHz = (lo < E - 1.0f && mrcHi_ > 0.5f) ? lo : E;
            s.edgeHiHz = (hi < E - 1.0f && mrcHi_ < 0.5f) ? hi : E;
            break;
        default: s.edgeLoHz = s.edgeHiHz = std::min(lo, hi); break;
    }
    s.autoCut = (s.edgeLoHz > 0.0f && s.edgeLoHz < E - 1.0f) || (s.edgeHiHz > 0.0f && s.edgeHiHz < E - 1.0f);
    s.notchHz = notchF_[1] > 0.0f ? notchF_[1] : (notchF_[0] > 0.0f ? -notchF_[0] : 0.0f);
    s.mrcUpper = mrcA_;
    s.mrcUpperHi = mrcHi_;
    return s;
}

}  // namespace vibedsp
