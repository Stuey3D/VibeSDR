// decoders/digimodes/msk144_decoder.cpp — see msk144_decoder.h.
#include "msk144_decoder.h"
#include "ldpc_small.h"
#include "vmath4.h"
#include "vibedsp/vibedsp.h"
#include "vibedsp/third_party/pffft/pffft.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace vibe {
namespace msk144 {

namespace {
using cf = std::complex<float>;
constexpr int kWin = 9000;              // 1.5 s of baseband per analysis
constexpr int kHop = 6000;              // 1.0 s
constexpr int kSync2 = 56 * kSpb;       // second sync word, samples after the first
constexpr int kNf = 9;                  // coarse frequency offsets
constexpr float kFstep = 50.f;
const int kRuns[5] = {1, 2, 4, 8, 16};
// the half-sine matched filter at 3 samples a bit: cos(π·m/6), m = −2 … 2
const float kMf[5] = {0.5f, 0.866025404f, 1.0f, 0.866025404f, 0.5f};

/** ln P(Gamma(k,1) > x) — the chance a run of k/2 frames of noise scores x or more. */
double lnGammaTail(int k, double x) {
    double term = 1, sum = 1;
    for (int i = 1; i < k; ++i) { term *= x / i; sum += term; }
    return -x + std::log(sum);
}

/** Catmull-Rom interpolation of a sampled sequence at a fractional index. */
inline float interp(const float* v, int n, double t) {
    int i = (int)t; if (t < 0) i = (int)t - 1;
    const float u = (float)(t - i);
    auto at = [&](int k) { return k < 0 || k >= n ? 0.f : v[k]; };
    const float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    return p1 + 0.5f * u * (p2 - p0 + u * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3 + u * (3.f * (p1 - p2) + p3 - p0)));
}

int syncSign(int k) { return syncBit(k) ? -1 : 1; }
/** dB added to the measured per-frame Es/N0·0.8 to give WSJT-X's SNR in 2500 Hz (calibrated on synthetic signals). */
constexpr double kSnrCal = 2.3;   // ★ sweeps 2026-10-31: the raw figure read 1.5–3 dB low
} // namespace

Receiver::FftBuf& Receiver::fft(int N) {
    for (auto& f : ffts_) if (f->n == N) return *f;
    std::unique_ptr<FftBuf> f(new FftBuf);
    f->n = N;
    f->setup = pffft_new_setup(N, PFFFT_COMPLEX);
    f->in = (float*)pffft_aligned_malloc(sizeof(float) * 2 * (size_t)N);
    f->out = (float*)pffft_aligned_malloc(sizeof(float) * 2 * (size_t)N);
    f->work = (float*)pffft_aligned_malloc(sizeof(float) * 2 * (size_t)N);
    ffts_.push_back(std::move(f));
    return *ffts_.back();
}
Receiver::FftBuf::~FftBuf() {
    if (setup) pffft_destroy_setup((PFFFT_Setup*)setup);
    pffft_aligned_free(in); pffft_aligned_free(out); pffft_aligned_free(work);
}

Receiver::Receiver(Options o) : opts(o) {
    debug_ = std::getenv("MSK_DEBUG") != nullptr;
    rotRe_.assign(kNf, std::vector<float>(512)); rotIm_.assign(kNf, std::vector<float>(512));
    for (int fi = 0; fi < kNf; ++fi) {
        const double fo = -o.tolHz + fi * kFstep;
        for (int i = 0; i < 512; ++i) { const double ph = -2 * M_PI * fo * i / kBbRate; rotRe_[(size_t)fi][(size_t)i] = (float)std::cos(ph); rotIm_[(size_t)fi][(size_t)i] = (float)std::sin(ph); }
    }
    nco_.reset(new vibedsp::NCO(o.centreHz / kRate));
    // ★ ±1.9 kHz kept (MSK's main lobe is ±1.5 kHz, plus the ±200 Hz search), the decimation by 2's alias band gone
    dec_.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(1900.0 / kRate, 700.0 / kRate, true), 2));
    zh_.assign(4, cf(0, 0));
}
Receiver::~Receiver() = default;

void Receiver::push(const float* audio, int n) {
    if (n <= 0) return;
    audioTotal_ += n;
    a_.resize((size_t)n); b_.resize((size_t)n);
    for (int i = 0; i < n; ++i) a_[(size_t)i] = cf(audio[i], 0.f);
    nco_->mix(a_.data(), b_.data(), n);
    d_.resize((size_t)dec_->maxOut(n));
    const int m = dec_->process(b_.data(), n, d_.data());
    // matched filter, centred: q[i] belongs to baseband sample i − 2 (the filter's delay is absorbed in the clock)
    for (int i = 0; i < m; ++i) {
        const cf x = d_[(size_t)i];
        const cf q = kMf[0] * zh_[0] + kMf[1] * zh_[1] + kMf[2] * zh_[2] + kMf[3] * zh_[3] + kMf[4] * x;
        zh_[0] = zh_[1]; zh_[1] = zh_[2]; zh_[2] = zh_[3]; zh_[3] = x;
        qre_.push_back(q.real()); qim_.push_back(q.imag());
    }
    bbTotal_ += m;
    while ((int)qre_.size() >= kWin) {
        analyse(kWin);
        qre_.erase(qre_.begin(), qre_.begin() + kHop);
        qim_.erase(qim_.begin(), qim_.begin() + kHop);
        qStart_ += kHop;
    }
}

void Receiver::flush() {
    if ((int)qre_.size() > kFrameSamples * 2) analyse((int)qre_.size());
    qStart_ += (int64_t)qre_.size();
    qre_.clear(); qim_.clear();
}

void Receiver::analyse(int nv) {
    using namespace q65v;
    const auto t0 = std::chrono::steady_clock::now();
    ++stats.windows;
    const float* qr = qre_.data();
    const float* qi = qim_.data();
    // ── noise: the matched filter's output power at its 20th percentile (|q|² is exponential on noise: P20 = 0.223·σ²;
    //    a ping must fill 80 % of a window to move it), and the median of the last eight windows' ──
    {
        std::vector<float> p;
        p.reserve((size_t)nv / 4 + 1);
        for (int i = 0; i < nv; i += 4) p.push_back(qr[i] * qr[i] + qi[i] * qi[i]);
        std::nth_element(p.begin(), p.begin() + p.size() / 5, p.end());
        noiseHist_.push_back(p[p.size() / 5] / 0.22314f);
        if (noiseHist_.size() > 8) noiseHist_.erase(noiseHist_.begin());
        std::vector<float> h = noiseHist_;
        std::nth_element(h.begin(), h.begin() + h.size() / 2, h.end());
        noise_ = h[h.size() / 2];
    }
    if (!(noise_ > 0)) return;
    const int nA = nv - 7 * kSpb - 1;               // sync correlation needs 8 bits
    const int nS = nA - kSync2;                     // a frame score needs both words
    if (nS < kFrameSamples) return;
    // run thresholds (Gamma tails at a fixed false-alarm chance per trial)
    const double lnPfa = std::log(1e-5);
    double thr[5];
    for (int r = 0; r < 5; ++r) {
        const int k = 2 * kRuns[r];
        double lo = k, hi = k + 200;
        for (int it = 0; it < 60; ++it) { const double mid = 0.5 * (lo + hi); (lnGammaTail(k, mid) > lnPfa ? lo : hi) = mid; }
        thr[r] = hi;
    }
    std::vector<float> rr((size_t)nv + 8), ri((size_t)nv + 8), are((size_t)nA + 4), aim((size_t)nA + 4);
    std::vector<std::vector<float>> S((size_t)kNf), P1((size_t)kNf);
    struct Hit { int tau, f, m0, L; double lnp; };
    std::vector<Hit> hits;
    const float inv = 1.f / (8.f * noise_);
    for (int fi = 0; fi < kNf; ++fi) {
        const double fo = -opts.tolHz + fi * kFstep;
        if (fo > opts.tolHz + 1e-3) { S[(size_t)fi].clear(); continue; }
        // rotate by −fo: the block's start phase (exact) times a table of the 512-sample ramp, then four samples a time
        {
            const std::vector<float>& tr = rotRe_[(size_t)fi];
            const std::vector<float>& ti = rotIm_[(size_t)fi];
            float wr[512], wi[512];
            for (int i0 = 0; i0 < nv; i0 += 512) {
                const double ph = -2 * M_PI * fo * i0 / kBbRate;
                const float cr = (float)std::cos(ph), ci = (float)std::sin(ph);
                const int len = std::min(512, nv - i0);
                for (int i = 0; i < len; ++i) { wr[i] = cr * tr[(size_t)i] - ci * ti[(size_t)i]; wi[i] = cr * ti[(size_t)i] + ci * tr[(size_t)i]; }
                int i = 0;
                for (; i + 4 <= len; i += 4) {
                    const V xr = ld(qr + i0 + i), xi = ld(qi + i0 + i), cr4 = ld(wr + i), ci4 = ld(wi + i);
                    st(&rr[(size_t)(i0 + i)], sub(mul(xr, cr4), mul(xi, ci4)));
                    st(&ri[(size_t)(i0 + i)], add(mul(xr, ci4), mul(xi, cr4)));
                }
                for (; i < len; ++i) {
                    const float xr = qr[i0 + i], xi = qi[i0 + i];
                    rr[(size_t)(i0 + i)] = xr * wr[i] - xi * wi[i]; ri[(size_t)(i0 + i)] = xr * wi[i] + xi * wr[i];
                }
            }
        }
        // A(τ) = Σ_k s_k·w_k·r(τ + 3k), w = 1, j, 1, j … — additions only, four starts at a time
        int t = 0;
        for (; t + 4 <= nA; t += 4) {
            V ar = set1(0.f), ai = set1(0.f);
            for (int k = 0; k < 8; ++k) {
                const float* pr = &rr[(size_t)(t + kSpb * k)];
                const float* pi = &ri[(size_t)(t + kSpb * k)];
                const bool pos = syncSign(k) > 0;
                if ((k & 1) == 0) { ar = pos ? add(ar, ld(pr)) : sub(ar, ld(pr)); ai = pos ? add(ai, ld(pi)) : sub(ai, ld(pi)); }
                else              { ar = pos ? sub(ar, ld(pi)) : add(ar, ld(pi)); ai = pos ? add(ai, ld(pr)) : sub(ai, ld(pr)); }
            }
            st(&are[(size_t)t], ar); st(&aim[(size_t)t], ai);
        }
        for (; t < nA; ++t) {
            float ar = 0, ai = 0;
            for (int k = 0; k < 8; ++k) {
                const float xr = rr[(size_t)(t + kSpb * k)], xi = ri[(size_t)(t + kSpb * k)];
                const float s = (float)syncSign(k);
                if ((k & 1) == 0) { ar += s * xr; ai += s * xi; } else { ar -= s * xi; ai += s * xr; }
            }
            are[(size_t)t] = ar; aim[(size_t)t] = ai;
        }
        std::vector<float>& pw = P1[(size_t)fi];
        pw.resize((size_t)nA);
        for (int i = 0; i < nA; ++i) pw[(size_t)i] = are[(size_t)i] * are[(size_t)i] + aim[(size_t)i] * aim[(size_t)i];
        std::vector<float>& s = S[(size_t)fi];
        s.resize((size_t)nS);
        for (int i = 0; i < nS; ++i) {
            const float p1 = are[(size_t)i] * are[(size_t)i] + aim[(size_t)i] * aim[(size_t)i];
            const float p2 = are[(size_t)(i + kSync2)] * are[(size_t)(i + kSync2)] + aim[(size_t)(i + kSync2)] * aim[(size_t)(i + kSync2)];
            s[(size_t)i] = (p1 + p2) * inv;
        }
        // fold at the frame period: runs of 1…16 frames at each phase. ★ Four phases a time: a run's sum minus its
        //   threshold, the lanes' maximum over every run (vmax — no compare); only a group with a lane over its
        //   threshold (rare on noise) is looked at again, one phase at a time, for the run that scored best.
        const int M = nS / kFrameSamples;                        // whole frames at every phase
        if (M < 1) continue;
        std::vector<float> cs((size_t)(M + 1) * 4);
        float thrF[5]; for (int r = 0; r < 5; ++r) thrF[r] = (float)thr[r];
        auto fullCheck = [&](int tau) {
            Hit best{tau, fi, 0, 0, 0};
            for (int r = 0; r < 5; ++r) {
                const int L = kRuns[r];
                if (L > M) break;
                double acc = 0;
                for (int m = 0; m < L; ++m) acc += s[(size_t)(tau + m * kFrameSamples)];
                for (int m0 = 0; ; ++m0) {
                    if (acc > thr[r]) {
                        const double lp = lnGammaTail(2 * L, acc);
                        if (lp < best.lnp) best = {tau, fi, m0, L, lp};
                    }
                    if (m0 + L >= M) break;
                    acc += s[(size_t)(tau + (m0 + L) * kFrameSamples)] - s[(size_t)(tau + m0 * kFrameSamples)];
                }
            }
            if (best.L > 0) hits.push_back(best);
        };
        for (int tau = 0; tau < kFrameSamples; tau += 4) {
            V acc = set1(0.f);
            st(&cs[0], acc);
            for (int m = 0; m < M; ++m) { acc = add(acc, ld(&s[(size_t)(tau + m * kFrameSamples)])); st(&cs[(size_t)(m + 1) * 4], acc); }
            V mx = set1(-1e30f);
            for (int r = 0; r < 5; ++r) {
                const int L = kRuns[r];
                if (L > M) break;
                const V th = set1(thrF[r]);
                for (int m0 = 0; m0 + L <= M; ++m0) mx = vmax(mx, sub(sub(ld(&cs[(size_t)(m0 + L) * 4]), ld(&cs[(size_t)m0 * 4])), th));
            }
            float l[4]; st(l, mx);
            for (int k = 0; k < 4; ++k) if (l[k] > -0.01f * thrF[0]) fullCheck(tau + k);   // ★ a margin: the prefix sums round
        }
    }
    stats.candidates += hits.size();
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.lnp < b.lnp; });
    std::vector<Hit> keep;
    const int maxC = opts.loaded ? std::max(1, opts.maxCands / 2) : opts.maxCands;
    for (const Hit& h : hits) {
        bool near = false;
        for (const Hit& k : keep) {
            int d = std::abs(h.tau - k.tau); d = std::min(d, kFrameSamples - d);
            if (d <= 4 && std::abs(h.f - k.f) <= 1) { near = true; break; }
        }
        if (near) continue;
        keep.push_back(h);
        if ((int)keep.size() >= maxC) break;
    }
    // ★ the per-frequency score arrays are kept for the refinement's timing step
    sScores_ = std::move(S);
    for (const Hit& h : keep) {
        Cand c{h.tau, h.f, h.m0, h.L, (float)(-h.lnp / 2.302585)};
        if (tryCand(c, nv, qStart_)) continue;
        /* ★ THE TWO SYNC WORDS ARE THE SAME (0x72), 56 bits apart in a 144-bit frame. A ping that holds only ONE of a
         *  frame's words (the frame's end, then the next frame's start) scores as well at the start that makes that word
         *  "sync 1" as at the one that makes it "sync 2". When one word carries the run's score, try the other reading:
         *  the start 168 samples later or earlier. */
        const std::vector<float>& pw = P1[(size_t)h.f];
        double w1 = 0, w2 = 0;
        for (int m = 0; m < h.L; ++m) {
            const int i = h.tau + (h.m0 + m) * kFrameSamples;
            if (i < (int)pw.size()) w1 += pw[(size_t)i];
            if (i + kSync2 < (int)pw.size()) w2 += pw[(size_t)(i + kSync2)];
        }
        const int alt = w2 > w1 ? kSync2 : -kSync2;
        if (std::max(w1, w2) < 3 * std::min(w1, w2)) continue;
        int t2 = h.tau + kFrameSamples * h.m0 + alt;
        if (t2 < 0) t2 += kFrameSamples;
        Cand c2{t2 % kFrameSamples, h.f, t2 / kFrameSamples, h.L, c.z};
        tryCand(c2, nv, qStart_);
    }
    sScores_.clear();
    stats.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool Receiver::tryCand(const Cand& c, int nv, int64_t base) {
    const bool dbg = debug_;
    const float* qr = qre_.data();
    const float* qi = qim_.data();
    const std::vector<float>& s = sScores_[(size_t)c.f];
    const int nS = (int)s.size();
    const int tauC = c.tau + kFrameSamples * c.m0;
    // ── 1. timing to a fraction of a sample: the run's score at ±1 ──
    auto runScore = [&](int t) { double a = 0; for (int m = 0; m < c.L; ++m) { const int i = t + m * kFrameSamples; if (i >= 0 && i < nS) a += s[(size_t)i]; } return a; };
    const double r0 = runScore(tauC - 1), r1 = runScore(tauC), r2 = runScore(tauC + 1);
    double frac = 0;
    { const double den = r0 - 2 * r1 + r2; if (den < 0) frac = std::max(-0.5, std::min(0.5, 0.5 * (r0 - r2) / den)); }
    const double tRun = tauC + frac;
    const double fCoarse = -opts.tolHz + c.f * kFstep;
    // the run's frames wholly in the window, and one more each side when there is room (a ping starts and ends anywhere
    // in a frame: its first and last parts are in the neighbours — the envelope weighting below decides what counts)
    int L = 0;
    while (L < c.L && tRun + (L + 1) * kFrameSamples + 2 < nv) ++L;
    if (L == 0) return false;
    const int mA = tRun - kFrameSamples >= 2 ? -1 : 0;
    const int mB = L + (tRun + (L + 1) * kFrameSamples + 2 < nv ? 1 : 0);
    const int NF = mB - mA, r0f = -mA;               // frame f ↔ m = mA + f; the run is f = r0f … r0f + L − 1
    const double tStart = tRun + mA * kFrameSamples;
    // ── the interpolated, w-rotated matched-filter values of every bit: v[f][k] = w_k·q(t) (no frequency yet) ──
    const size_t NB = (size_t)NF * kFrameBits;
    std::vector<cf> v(NB);
    for (int f = 0; f < NF; ++f)
        for (int k = 0; k < kFrameBits; ++k) {
            const double t = tStart + f * kFrameSamples + k * kSpb;
            const cf x(interp(qr, nv, t), interp(qi, nv, t));
            v[(size_t)f * kFrameBits + (size_t)k] = (k & 1) ? cf(-x.imag(), x.real()) : x;
        }
    auto tOf = [&](int f, int k) { return tStart + f * kFrameSamples + k * kSpb; };
    auto rot = [&](double F, int f, int k) {
        const double ph = -2 * M_PI * F * tOf(f, k) / kBbRate;
        return cf((float)std::cos(ph), (float)std::sin(ph));
    };
    // ── 2. frequency: the squared bits have no data in them (each is ±A·e^{jθ}), so the spectrum of the run's squared
    //    bits (2000 a second) peaks at twice the frequency error — every bit of the run counts, and an FFT covers the
    //    whole ±(coarse step + 25 Hz) at once. Zero-padded ×2 or more, then a parabola through the peak. ──
    double bestF = fCoarse;
    {
        const int nb = L * kFrameBits;
        int N = 1024; while (N < 2 * nb) N *= 2;
        FftBuf& fb = fft(N);
        std::fill(fb.in, fb.in + 2 * N, 0.f);
        for (int m = 0; m < L; ++m)
            for (int k = 0; k < kFrameBits; ++k) {
                const int f = r0f + m, n = m * kFrameBits + k;
                const cf x = v[(size_t)f * kFrameBits + (size_t)k] * rot(fCoarse, f, k);
                const cf x2 = x * x;
                fb.in[2 * n] = x2.real(); fb.in[2 * n + 1] = x2.imag();
            }
        pffft_transform_ordered((PFFFT_Setup*)fb.setup, fb.in, fb.out, fb.work, PFFFT_FORWARD);
        const double binHz = (double)kBaud / N;               // of 2F
        const int span = (int)std::ceil(2 * (kFstep + 25.0) / binHz);
        auto pw = [&](int b) { const int i = ((b % N) + N) % N; return (double)fb.out[2 * i] * fb.out[2 * i] + (double)fb.out[2 * i + 1] * fb.out[2 * i + 1]; };
        int bb = 0; double bp = -1;
        for (int b = -span; b <= span; ++b) { const double p = pw(b); if (p > bp) { bp = p; bb = b; } }
        double fr = 0;
        { const double a0 = pw(bb - 1), b0 = bp, c0 = pw(bb + 1), den = a0 - 2 * b0 + c0; if (den < 0) fr = std::max(-0.5, std::min(0.5, 0.5 * (a0 - c0) / den)); }
        // ★ e^{−j2π·2Δf·t} sampled each bit: the FFT's bin b is 2Δf = b·binHz (t counted from the run's first bit)
        bestF = fCoarse + 0.5 * (bb + fr) * binHz;
    }
    // ── the bits at that frequency, and each frame's sync sum ──
    std::vector<cf> u(NB), B((size_t)NF);
    for (int f = 0; f < NF; ++f) {
        cf acc(0, 0);
        for (int k = 0; k < kFrameBits; ++k) {
            const cf x = v[(size_t)f * kFrameBits + (size_t)k] * rot(bestF, f, k);
            u[(size_t)f * kFrameBits + (size_t)k] = x;
            if (isSync(k)) acc += (float)syncSign(k) * x;
        }
        B[(size_t)f] = acc;
    }
    // ── 3. detect, then 4. decode. mrc = false: one carrier phase for the whole ping (from the run's sync); true: each
    //    frame its own phase (a drifting or Doppler-smeared ping). Either way each bit is weighted by the ping's
    //    ENVELOPE at its instant (maximal-ratio combining in time): a(t) from the in-phase minus quadrature power,
    //    smoothed over ±16 bits — noise-only stretches (before and after the ping) get weight ≈ 0. ──
    std::vector<float> y(NB), z(NB), env(NB);
    auto attempt = [&](bool mrc) -> bool {
        cf Bt(0, 0); for (int m = 0; m < L; ++m) Bt += B[(size_t)(r0f + m)];
        if (std::abs(Bt) <= 0) return false;
        for (int f = 0; f < NF; ++f) {
            cf ph = std::conj(Bt) / std::abs(Bt);
            if (mrc) { const cf b = B[(size_t)f]; if (std::abs(b) > 0) ph = std::conj(b) / std::abs(b); }
            for (int k = 0; k < kFrameBits; ++k) {
                const cf x = u[(size_t)f * kFrameBits + (size_t)k] * ph;
                y[(size_t)f * kFrameBits + (size_t)k] = x.real(); z[(size_t)f * kFrameBits + (size_t)k] = x.imag();
            }
        }
        // ★ the quadrature is NOT noise here: at a bit's instant the matched filter also sees half of each neighbouring
        //   bit on the other axis (offset QPSK). The noise is the window's own measure (σ² per complex sample).
        const double sig2 = 0.5 * (double)noise_;            // per real dimension
        if (!(sig2 > 0)) return false;
        {   // envelope: running mean of y² − σ² over 33 bits
            const int H = 16;
            std::vector<double> cs(NB + 1, 0.0);
            for (size_t i = 0; i < NB; ++i) cs[i + 1] = cs[i] + (double)y[i] * y[i] - sig2;
            for (size_t i = 0; i < NB; ++i) {
                const size_t lo = i >= (size_t)H ? i - H : 0, hi = std::min(NB, i + H + 1);
                const double e = (cs[hi] - cs[lo]) / (double)(hi - lo);
                env[i] = (float)std::sqrt(std::max(0.0, e));
            }
        }
        float soft[kFrameBits];
        double sigE = 0, sigN = 0;
        for (int k = 0; k < kFrameBits; ++k) {
            double acc = 0;
            for (int f = 0; f < NF; ++f) { const size_t i = (size_t)f * kFrameBits + (size_t)k; acc += (double)env[i] * y[i]; }
            soft[k] = (float)(acc / sig2);
        }
        for (int m = 0; m < L; ++m) for (int k = 0; k < kFrameBits; ++k) { const double e = env[(size_t)(r0f + m) * kFrameBits + (size_t)k]; sigE += e * e; ++sigN; }
        float llr[kN];
        for (int j = 0; j < kN; ++j) llr[j] = -2.f * soft[framePos(j)];
        ++stats.attempts;
        uint8_t cw[kN];
        int unsat = 0;
        int it = code().bp(llr, opts.maxIter, cw, &unsat);
        if (dbg) {
            int sy = 0; for (int k = 0; k < kFrameBits; ++k) if (isSync(k)) sy += (soft[k] > 0) == (syncBit(k) == 0);
            std::fprintf(stderr, "cand tau %d f %d m0 %d L %d/%d NF %d z %.1f t %.1f F %.2f mrc %d | bp %d unsat %d sync %d/16 sig %.2f\n",
                         c.tau, c.f, c.m0, L, c.L, NF, c.z, tRun + base, bestF + opts.centreHz, mrc, it, unsat, sy, std::sqrt(sig2));
        }
        bool viaOsd = false;
        std::string text, call, grid;
        bool plaus = false;
        if (!(it > 0 && crcOk(cw) && unpack(cw, false, text, call, grid, plaus) && plaus)) {
            it = 0;
            if (!opts.osd || opts.loaded) return false;
            auto acc = [&](const uint8_t* x) {
                if (!crcOk(x)) return false;
                std::string t2, c2, g2; bool p2 = false;
                return unpack(x, false, t2, c2, g2, p2) && p2;
            };
            if (!code().osd(llr, opts.osdPairs, acc, cw)) return false;
            viaOsd = true;
            unpack(cw, false, text, call, grid, plaus);
        }
        // ── guards: the frame (sync too) must agree with what was received ──
        uint8_t fb[kFrameBits];
        frameBits(cw, fb);
        int errs = 0; double agree = 0, tot = 0;
        for (int k = 0; k < kFrameBits; ++k) {
            const float sv = fb[k] ? -soft[k] : soft[k];
            errs += sv < 0;
            agree += sv; tot += std::fabs(soft[k]);
        }
        const double rho = tot > 0 ? agree / tot : 0;
        if (dbg) std::fprintf(stderr, "   decoded '%s' osd %d errs %d rho %.2f\n", text.c_str(), viaOsd, errs, rho);
        const int maxErr = viaOsd ? 30 : 45;
        if (errs > maxErr || rho < (viaOsd ? 0.45 : 0.30)) { ++stats.rejected; return false; }
        unpack(cw, true, text, call, grid, plaus);   // ★ a decode we keep: its full calls go in the hash table
        Result r;
        r.text = text; r.call = call; r.grid = grid;
        r.frames = L; r.osd = viaOsd; r.mrc = mrc; r.iters = it; r.errors = errs; r.score = c.z;
        r.f0 = (float)(opts.centreHz + bestF);
        const double es = sigN > 0 ? sigE / sigN / (2.0 * sig2) : 0;   // Es/N0 over the run: a² / σ² (complex)
        r.snr = (int)std::lround(10.0 * std::log10(std::max(1e-4, es * 0.8)) + kSnrCal);
        r.audioSec = (double)(base + (int64_t)tRun) / kBbRate;
        r.utc = std::isfinite(utc0) ? utc0 + r.audioSec : NAN;
        // ★ one report per message per 30 s (a long ping straddles two windows; a QSO repeats each period)
        recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const Recent& x) { return std::fabs(r.audioSec - x.t) > 30; }), recent_.end());
        ++stats.decodes;
        for (const Recent& x : recent_) if (x.text == r.text) return true;
        recent_.push_back({r.text, r.audioSec});
        if (onResult) onResult(r);
        return true;
    };
    if (attempt(false)) return true;
    if (NF > 1 && attempt(true)) return true;
    return false;
}

} // namespace msk144
} // namespace vibe
