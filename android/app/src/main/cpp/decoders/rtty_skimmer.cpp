// decoders/rtty_skimmer.cpp — see rtty_skimmer.h for the design. Original VibeSDR code: overlap-save fast convolution
// (the PSK skimmer's shape), the published ITA2 / FSK descriptions, FskDecoder for the bits and RTTY AUTO's rules
// (rttyscore) for the search.
#include "rtty_skimmer.h"
#include "cw_skimmer.h"                    // ★ the callsign rules (isCallsign / identify) — the same on every keyboard mode
#include "fsk_decoder.h"
#include "rtty_auto.h"                     // ★ rttyscore: the settings, the frame score and the winner rule
#include "../vibedsp/simd_internal.h"     // ★ VIBE_NEON / VIBE_SSE (and VIBE_FORCE_SCALAR) — the project's one SIMD switch
#include "../vibedsp/third_party/pffft/pffft.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace vibe {
namespace rtty {

const char* qualityWord(float q) { return q >= 0.75f ? "high" : q >= 0.5f ? "medium" : q >= 0.3f ? "low" : "poor"; }

// ── ITA2 for the tests' generator (US teleprinter figures — the set FskDecoder's Ita2 reads) ──────────────────────
int ita2Code(char c, bool& figs) {
    static const char L[33] = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";
    static const char F[33] = "\0" "3\n- \a87\r$4',!:(5\")2#6019?&\0./;\0";
    const char u = (char)std::toupper((unsigned char)c);
    if (u == ' ' || u == '\n' || u == '\r') { return u == ' ' ? 4 : u == '\n' ? 2 : 8; }   // in both sets
    for (int i = 1; i < 32; ++i) if (L[i] == u && u) { figs = false; return i; }
    for (int i = 1; i < 32; ++i) if (F[i] == u && u) { figs = true; return i; }
    return -1;
}

// ★★ THE TEXT GATE: does it read as teleprinter text? Noise read as ITA2 through a decoder that checks every frame still
//    prints — random letters with a space one time in 32 (words thirty long), or in figures a spray of - ' ! : ( ) " #.
//    Real RTTY — a CQ, a contest exchange, a weather bulletin, figures groups — has words of a few characters, spaces
//    every few, a dozen different characters, and few figure-shift symbols.
bool readsAsText(const std::string& in) {
    const std::string t = in.size() > 64 ? in.substr(in.size() - 64) : in;
    const size_t n = t.size();
    if (n < 20) return false;
    int seps = 0, punct = 0, words = 0, letters = 0, distinct = 0; bool seen[128] = {}; bool inWord = false;
    for (char c : t) {
        const unsigned char u = (unsigned char)c & 0x7f;
        if (!seen[u]) { seen[u] = true; ++distinct; }
        const bool sep = c == ' ' || c == '\n' || c == '\r';
        if (sep) { ++seps; inWord = false; continue; }
        if (!std::isalnum(u)) ++punct;
        ++letters; if (!inWord) { ++words; inWord = true; }
    }
    const double sp = (double)seps / n, meanWord = words ? (double)letters / words : 99;
    return sp >= 0.05 && sp <= 0.40 && distinct >= (n >= 40 ? 10 : 8) && meanWord >= 1.6 && meanWord <= 9.0
        && punct * 100 <= (int)n * 12;
}

namespace {
using cf = std::complex<float>;
constexpr double kTwoPi = 6.283185307179586;
constexpr double kChRate = 3000.0;      // ★ every channel's rate: 45.45 baud = 66 samples (33 a half bit), 50 = 60, 75 = 40
constexpr int    kW = 2;                // a tone's power: ±2 bins (≈ ±12 Hz)
constexpr double kLostSec = 6.0;        // tones under the floor this long: the signal has gone
constexpr double kProbeMinSec = 1.5;    // channel seconds before the alternation test is read
constexpr double kProbeMaxSec = 4.0;    // …and the longest a candidate is tested before it is dropped
constexpr double kRejectHoldSec = 30.0; // a pair that did not alternate is not tried again for this long
constexpr int    kMaxShowCarry = 600;

// ── The kernels: NEON (ARMv7-safe intrinsics), SSE2, and the scalar reference ──────────────────────────────────
/** ★ THE PER-BIN PASS (as the PSK skimmer's): the Hann window applied in the frequency domain (½X[k] − ¼X[k−1] −
 *  ¼X[k+1] — the forward FFT is unwindowed, because it is also the channeliser's overlap-save transform), its power,
 *  and two running averages: F (≈ 1 s, the finder) and S (≈ 6 s, the floor's). x = n+2 interleaved complex bins. */
void binPass(const float* x, float* F, float* S, int n, float aF, float aS) {
    int i = 0;
#if VIBE_NEON
    const float32x4_t h = vdupq_n_f32(0.5f), q = vdupq_n_f32(-0.25f), vaF = vdupq_n_f32(aF), vaS = vdupq_n_f32(aS);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t a = vld2q_f32(x + 2 * i), b = vld2q_f32(x + 2 * (i + 1)), c = vld2q_f32(x + 2 * (i + 2));
        const float32x4_t re = vmlaq_f32(vmulq_f32(b.val[0], h), vaddq_f32(a.val[0], c.val[0]), q);
        const float32x4_t im = vmlaq_f32(vmulq_f32(b.val[1], h), vaddq_f32(a.val[1], c.val[1]), q);
        const float32x4_t p = vmlaq_f32(vmulq_f32(re, re), im, im);
        float32x4_t f = vld1q_f32(F + i), s = vld1q_f32(S + i);
        f = vmlaq_f32(f, vsubq_f32(p, f), vaF);
        s = vmlaq_f32(s, vsubq_f32(p, s), vaS);
        vst1q_f32(F + i, f); vst1q_f32(S + i, s);
    }
#elif VIBE_SSE
    const __m128 h = _mm_set1_ps(0.5f), q = _mm_set1_ps(-0.25f), vaF = _mm_set1_ps(aF), vaS = _mm_set1_ps(aS);
    for (; i + 4 <= n; i += 4) {
        __m128 ar, ai, br, bi, cr, ci;
        vibedsp::sseLoad2(x + 2 * i, ar, ai); vibedsp::sseLoad2(x + 2 * (i + 1), br, bi); vibedsp::sseLoad2(x + 2 * (i + 2), cr, ci);
        const __m128 re = _mm_add_ps(_mm_mul_ps(br, h), _mm_mul_ps(_mm_add_ps(ar, cr), q));
        const __m128 im = _mm_add_ps(_mm_mul_ps(bi, h), _mm_mul_ps(_mm_add_ps(ai, ci), q));
        const __m128 p = _mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im));
        __m128 f = _mm_loadu_ps(F + i), s = _mm_loadu_ps(S + i);
        f = _mm_add_ps(f, _mm_mul_ps(_mm_sub_ps(p, f), vaF));
        s = _mm_add_ps(s, _mm_mul_ps(_mm_sub_ps(p, s), vaS));
        _mm_storeu_ps(F + i, f); _mm_storeu_ps(S + i, s);
    }
#endif
    for (; i < n; ++i) {
        const float* a = x + 2 * i;
        const float re = 0.5f * a[2] - 0.25f * (a[0] + a[4]), im = 0.5f * a[3] - 0.25f * (a[1] + a[5]);
        const float p = re * re + im * im;
        F[i] += (p - F[i]) * aF;
        S[i] += (p - S[i]) * aS;
    }
}
/** out = in · m, complex (interleaved) by real — a channel's mask on its bins. */
void mulMask(const float* in, const float* m, float* out, int n) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        float32x4x2_t z = vld2q_f32(in + 2 * i);
        const float32x4_t w = vld1q_f32(m + i);
        z.val[0] = vmulq_f32(z.val[0], w); z.val[1] = vmulq_f32(z.val[1], w);
        vst2q_f32(out + 2 * i, z);
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        const __m128 w = _mm_loadu_ps(m + i);
        _mm_storeu_ps(out + 2 * i,     _mm_mul_ps(_mm_loadu_ps(in + 2 * i),     _mm_unpacklo_ps(w, w)));
        _mm_storeu_ps(out + 2 * i + 4, _mm_mul_ps(_mm_loadu_ps(in + 2 * i + 4), _mm_unpackhi_ps(w, w)));
    }
#endif
    for (; i < n; ++i) { out[2 * i] = in[2 * i] * m[i]; out[2 * i + 1] = in[2 * i + 1] * m[i]; }
}
/** ★ A tone's power around every searched bin: T[s] = Σ_{|i|≤kW} F[s+i] — a sliding sum, four outputs at a time. */
void toneSum(const float* F, float* T, int n) {
    // T[s] valid for kW ≤ s < n − kW; the edges stay 0
    for (int s = 0; s < std::min(n, kW); ++s) T[s] = 0;
    for (int s = std::max(kW, n - kW); s < n; ++s) T[s] = 0;
    int s = kW;
#if VIBE_NEON
    for (; s + 4 <= n - kW; s += 4) {
        float32x4_t a = vld1q_f32(F + s - 2);
        a = vaddq_f32(a, vld1q_f32(F + s - 1)); a = vaddq_f32(a, vld1q_f32(F + s));
        a = vaddq_f32(a, vld1q_f32(F + s + 1)); a = vaddq_f32(a, vld1q_f32(F + s + 2));
        vst1q_f32(T + s, a);
    }
#elif VIBE_SSE
    for (; s + 4 <= n - kW; s += 4) {
        __m128 a = _mm_loadu_ps(F + s - 2);
        a = _mm_add_ps(a, _mm_loadu_ps(F + s - 1)); a = _mm_add_ps(a, _mm_loadu_ps(F + s));
        a = _mm_add_ps(a, _mm_loadu_ps(F + s + 1)); a = _mm_add_ps(a, _mm_loadu_ps(F + s + 2));
        _mm_storeu_ps(T + s, a);
    }
#endif
    for (; s < n - kW; ++s) T[s] = F[s - 2] + F[s - 1] + F[s] + F[s + 1] + F[s + 2];
}
/** ★ The channel's middle half → int16 audio with the band centred on rate/4: a = Re(z · j^i) · g. j^i cycles
 *  1, j, −1, −j, so this is a gather of ±re / ±im — four outputs at a time. `z` interleaved, n a multiple of 4. */
void toAudio(const float* z, int n, float g, int16_t* out) {
    int i = 0;
#if VIBE_NEON
    const float32x4_t sgn = { 1.0f, -1.0f, -1.0f, 1.0f }, vg = vdupq_n_f32(g);
    const float32x4_t lo = vdupq_n_f32(-32767.0f), hi = vdupq_n_f32(32767.0f);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(z + 2 * i);   // re0..3, im0..3
        // pick re, im, re, im (lanes 0..3) then the signs +, −, −, +
        const float32x4_t pick = { vgetq_lane_f32(v.val[0], 0), vgetq_lane_f32(v.val[1], 1),
                                   vgetq_lane_f32(v.val[0], 2), vgetq_lane_f32(v.val[1], 3) };
        float32x4_t a = vmulq_f32(vmulq_f32(pick, sgn), vg);
        a = vminq_f32(hi, vmaxq_f32(lo, a));
        const int32x4_t r = vcvtq_s32_f32(a);
        vst1_s16(out + i, vmovn_s32(r));
    }
#elif VIBE_SSE
    const __m128 sgn = _mm_setr_ps(1.0f, -1.0f, -1.0f, 1.0f), vg = _mm_set1_ps(g);
    const __m128 lo = _mm_set1_ps(-32767.0f), hi = _mm_set1_ps(32767.0f);
    for (; i + 4 <= n; i += 4) {
        __m128 re, im; vibedsp::sseLoad2(z + 2 * i, re, im);
        // lanes: re0, im1, re2, im3 — unpack re/im pairs and take the even/odd we need
        const __m128 a0 = _mm_shuffle_ps(re, im, _MM_SHUFFLE(3, 1, 2, 0));   // re0 re2 im1 im3
        const __m128 pick = _mm_shuffle_ps(a0, a0, _MM_SHUFFLE(3, 1, 2, 0)); // re0 im1 re2 im3
        __m128 a = _mm_mul_ps(_mm_mul_ps(pick, sgn), vg);
        a = _mm_min_ps(hi, _mm_max_ps(lo, a));
        const __m128i r = _mm_cvttps_epi32(a);
        const __m128i p = _mm_packs_epi32(r, r);
        _mm_storel_epi64((__m128i*)(out + i), p);
    }
#endif
    for (; i < n; ++i) {
        const float* p = z + 2 * i;
        float a;
        switch (i & 3) { case 0: a = p[0]; break; case 1: a = -p[1]; break; case 2: a = -p[0]; break; default: a = p[1]; }
        a *= g;
        a = std::max(-32767.0f, std::min(32767.0f, a));
        out[i] = (int16_t)a;
    }
}
bool smooth235(int n) { for (int p : { 2, 3, 5 }) while (n % p == 0) n /= p; return n == 1; }
bool pffftSize(int n) { return n >= 32 && n % 16 == 0 && smooth235(n / 16); }
} // namespace

// ── A job: one signal's stored channel, searched for its settings on a worker ───────────────────────────────────
struct Job {
    int sigId = 0;
    double strength = 0;          // queue order: strongest first
    std::vector<int16_t> audio;   // ★ a COPY of the stored channel — the worker touches nothing else
    uint64_t endSample = 0;       // the channel sample just after the copy
    double centre = 0, shift = 0; // audio Hz at kChRate
    bool requeue = false;
    // the result
    bool found = false;
    rttyscore::Setting win{};
    std::unique_ptr<FskDecoder> dec;
    std::string text;             // what the winner decoded over the copy
    unsigned long good = 0, bad = 0;
};

struct Skimmer::Impl {
    struct Sig {
        int id = 0;                      // 0 while a candidate (not listed)
        enum Stage { Cand, Listed, Set } stage = Cand;
        bool active = true;
        double fc = 0, shift = 0;        // centre (Hz from the input's 0) and the standard shift
        int k0 = 0; double delta = 0;    // the channel's centre bin, and fc − k0·Δ
        std::vector<float> mask;
        double strength = 0;             // dB, the weaker tone over the floor (as last measured)
        double lo = 0, hi = 0, fitErr = 0;   // the tones as found, and how far their spacing was from the standard shift
        double bornSec = 0, listedSec = -1, setSec = -1, quietSec = 0;
        bool catchup = true;
        // the channel, stored
        std::vector<int16_t> ring; uint64_t wr = 0;
        double P = -1;                   // channel power (AGC)
        // the alternation test (candidates)
        double phM = 0, phS = 0, wM = 0, wS = 0;   // the two tones' baseband rotators
        int L = 18, inBlk = 0; cf accM{0, 0}, accS{0, 0};
        double n = 0, sm = 0, ss = 0, smm = 0, sss = 0, sms = 0;
        double alt = 0;
        // parameters
        bool queued = false;
        int searches = 0;
        std::unique_ptr<FskDecoder> dec;
        rttyscore::Setting st{};
        uint64_t decAt = 0;
        unsigned long lastGood = 0, lastBad = 0, secGood = 0, secBad = 0;
        double secScore = 0, badSec = 0, secClock = 0, fq = 0;
        // text
        std::string pend, held; bool textOk = false; bool lastCr = false;
        std::string text, settings, call, word;
        std::vector<std::string> words;
        std::map<std::string, int> heard;
        std::map<std::string, double> spotted;
        unsigned chars = 0;
        int64_t firstMs = 0, lastMs = 0;
    };

    Skimmer* owner = nullptr;
    Config cfg;
    int N = 0, H = 0, M = 0;
    double binHz = 0, fsCh = 0, fsScale = 1;   // fsScale = kChRate / fsCh (the decoders' Hz from the channel's)
    PFFFT_Setup* big = nullptr; PFFFT_Setup* small = nullptr;
    float *fin = nullptr, *fout = nullptr, *work = nullptr, *cin = nullptr, *cout = nullptr, *cwork = nullptr;
    std::vector<float> tail; int fill = 0;
    int kLo = 0, nS = 0;
    std::vector<float> xs, Fa, Sa, floorV, Ts, scratch, blk2;
    std::vector<int16_t> aud;
    uint64_t hopsDone = 0, samples = 0;
    int64_t startMs = 0;
    int kHist = 12;
    std::vector<std::vector<float>> hist;
    std::vector<std::unique_ptr<Sig>> sigs, gone;
    std::vector<std::pair<double, double>> holdOff;   // (Hz, until)
    int nextId = 1;
    uint64_t rejected = 0, notRttyN = 0, suppressed = 0, requeuesN = 0;
    std::atomic<uint64_t> jobsRunN{0};
    // ★ the queue — the only lock, taken a handful of times per signal, never per hop
    mutable std::mutex jm;
    std::vector<std::shared_ptr<Job>> queue, done;
    std::atomic<int> running{0};

    explicit Impl(const Config& c) : cfg(c) {
        // ★★ THE SIZES: N (the forward transform, ≈ 6 Hz bins) and M (each channel's inverse) such that the channel's
        //    rate M·rate/N is kChRate within 0.2 % — whatever the slicer's rate (it is the radio's rate × bins / FFT
        //    size, rarely round). The inverse of M bins of an N-point transform samples the band at N/M-spaced times
        //    whether or not N/M is whole: band-limited resampling for free.
        double bestErr = 1e9, bestBin = 1e9; int bn = 0, bm = 0;
        for (int n = 256; n <= (1 << 17); n += 16) {
            if (!pffftSize(n)) continue;
            const double bh = cfg.rate / n;
            if (bh < 3.0 || bh > 9.0) continue;
            const int m0 = (int)std::lround(kChRate * n / cfg.rate);
            for (int m = std::max(32, m0 - 48); m <= m0 + 48; m += 16) {
                if (!pffftSize(m) || m > n) continue;
                const double err = std::fabs(m * cfg.rate / n - kChRate) / kChRate;
                const double bd = std::fabs(bh - 5.86);
                if (err < bestErr - 1e-6 || (std::fabs(err - bestErr) <= 1e-6 && bd < bestBin)) { bestErr = err; bestBin = bd; bn = n; bm = m; }
            }
        }
        if (bn == 0) { bn = 8192; bm = 512; }
        N = bn; M = bm; H = N / 2;
        binHz = cfg.rate / N; fsCh = M * cfg.rate / N; fsScale = kChRate / fsCh;
        big = pffft_new_setup(N, PFFFT_COMPLEX);
        small = pffft_new_setup(M, PFFFT_COMPLEX);
        fin = (float*)pffft_aligned_malloc(2 * N * sizeof(float));
        fout = (float*)pffft_aligned_malloc(2 * N * sizeof(float));
        work = (float*)pffft_aligned_malloc(2 * N * sizeof(float));
        cin = (float*)pffft_aligned_malloc(2 * M * sizeof(float));
        cout = (float*)pffft_aligned_malloc(2 * M * sizeof(float));
        cwork = (float*)pffft_aligned_malloc(2 * M * sizeof(float));
        tail.assign((size_t)2 * N, 0.0f);
        kHist = std::max(4, (int)std::ceil(1.0 / (H / cfg.rate)));
        hist.assign((size_t)kHist, std::vector<float>((size_t)2 * N, 0.0f));
        const double lo = std::max(cfg.loHz, -cfg.rate / 2 + 200), hi = std::min(cfg.hiHz, cfg.rate / 2 - 200);
        kLo = (int)std::ceil(lo / binHz); nS = std::max(16, (int)std::floor(hi / binHz) - kLo + 1);
        xs.assign((size_t)2 * (nS + 2), 0.0f);
        Fa.assign((size_t)nS, 0.0f); Sa.assign((size_t)nS, 0.0f); floorV.assign((size_t)nS, 1e-30f); Ts.assign((size_t)nS, 0.0f);
        aud.assign((size_t)M / 2, 0);
        startMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    ~Impl() {
        if (big) pffft_destroy_setup(big);
        if (small) pffft_destroy_setup(small);
        for (float* p : { fin, fout, work, cin, cout, cwork }) if (p) pffft_aligned_free(p);
    }
    double nowSec() const { return (double)samples / cfg.rate; }
    int64_t nowMs() const { return startMs + (int64_t)(nowSec() * 1000.0); }
    int binIndex(int k) const { k %= N; return k < 0 ? k + N : k; }
    double hzOfS(double s) const { return (kLo + s) * binHz; }
    size_t ringCap() const { return (size_t)std::lround(cfg.storeSec * fsCh); }

    // ── input ──
    void feed(const cf* x, int n) {
        for (int i = 0; i < n; ) {
            const int take = std::min(n - i, H - fill);
            std::memcpy(tail.data() + 2 * (size_t)(N - H + fill), x + i, sizeof(float) * 2 * (size_t)take);
            fill += take; i += take; samples += (uint64_t)take;
            if (fill == H) { hop(); std::memmove(tail.data(), tail.data() + 2 * (size_t)H, sizeof(float) * 2 * (size_t)(N - H)); fill = 0; }
        }
    }

    // ── one hop ──
    void hop() {
        applyResults();
        std::memcpy(fin, tail.data(), sizeof(float) * 2 * (size_t)N);
        pffft_transform_ordered(big, fin, fout, work, PFFFT_FORWARD);
        ++hopsDone;
        for (int s = 0; s < nS + 2; ++s) {
            const int b = binIndex(kLo - 1 + s);
            xs[2 * (size_t)s] = fout[2 * (size_t)b]; xs[2 * (size_t)s + 1] = fout[2 * (size_t)b + 1];
        }
        const double hopSec = H / cfg.rate;
        const float aF = std::max((float)std::min(1.0, hopSec / 1.0), 1.0f / (float)hopsDone);
        const float aS = std::max((float)std::min(1.0, hopSec / 6.0), 1.0f / (float)hopsDone);
        binPass(xs.data(), Fa.data(), Sa.data(), nS, aF, aS);
        updateFloor();
        toneSum(Fa.data(), Ts.data(), nS);
        if (nowSec() >= 1.0) findPairs();
        std::memcpy(hist[(size_t)(hopsDone % (uint64_t)kHist)].data(), fout, sizeof(float) * 2 * (size_t)N);
        for (auto& g : sigs) {
            if (g->catchup) {
                g->catchup = false;
                const uint64_t from = hopsDone > (uint64_t)kHist - 1 ? hopsDone - (uint64_t)kHist + 1 : 1;
                for (uint64_t k = from; k < hopsDone; ++k) runChannel(*g, hist[(size_t)(k % (uint64_t)kHist)].data(), k);
            }
            runChannel(*g, fout, hopsDone);
            afterChannel(*g);
        }
        housekeeping();
        if (cfg.workers <= 0) while (runOne()) applyResults();   // ★ no workers: the jobs run here, in order
    }

    // ★ The floor (the PSK skimmer's): the 25th percentile of the slow average in blocks of 32 bins, a block that is
    //   mostly signal taking its lower neighbour's value, straight lines between the blocks' centres.
    static constexpr float kFloorBias = 1.13f;
    void updateFloor() {
        const int B = 32, nb = (nS + B - 1) / B;
        std::vector<float>& blk = scratch;
        blk.assign((size_t)nb + 2, 0.0f);
        float tmp[32];
        for (int j = 0; j < nb; ++j) {
            const int a = j * B, e = std::min(nS, a + B), m = e - a;
            for (int i = 0; i < m; ++i) tmp[i] = Sa[(size_t)(a + i)];
            std::nth_element(tmp, tmp + m / 4, tmp + m);
            blk[(size_t)j + 1] = tmp[m / 4];
        }
        blk[0] = blk[1]; blk[(size_t)nb + 1] = blk[(size_t)nb];
        std::vector<float>& v = blk2;
        v.assign((size_t)nb, 0.0f);
        for (int j = 0; j < nb; ++j) v[(size_t)j] = std::min(blk[(size_t)j + 1], std::max(blk[(size_t)j], blk[(size_t)j + 2])) * kFloorBias;
        static_assert(B == 32, "the shift below is a floor division by 2B = 64");
        for (int i = 0; i < nS; ++i) {
            const double x = (i + 0.5) / B - 0.5;
            const int j0 = std::max(0, std::min(nb - 1, (2 * i + 1 - B) >> 6)), j1 = std::min(nb - 1, j0 + 1);
            const double t = std::max(0.0, std::min(1.0, x - j0));
            floorV[(size_t)i] = std::max((float)(v[(size_t)j0] * (1 - t) + v[(size_t)j1] * t), 1e-30f);
        }
    }
    /** A tone's excess over the floor at `hz` (linear, T / (5·floor) − 1); −1 off the searched range. */
    double excessAt(double hz) const {
        const int s = (int)std::lround(hz / binHz) - kLo;
        if (s < kW || s >= nS - kW) return -1;
        return Ts[(size_t)s] / ((2 * kW + 1) * floorV[(size_t)s]) - 1.0;
    }

    // ── the finder: tone peaks, then pairs a standard shift apart ──
    void findPairs() {
        const double now = nowSec();
        holdOff.erase(std::remove_if(holdOff.begin(), holdOff.end(), [&](const std::pair<double, double>& h) { return h.second < now; }), holdOff.end());
        if ((int)sigs.size() >= cfg.maxSignals) return;
        const float candEx = (float)(std::pow(10.0, cfg.candidateDb / 10.0) - 1.0);
        struct Peak { int s; float ex; double hz; };
        Peak pk[256]; int np = 0;
        for (int s = kW + 3; s + kW + 3 < nS; ++s) {
            const float t = Ts[(size_t)s];
            const float ex = t / ((2 * kW + 1) * floorV[(size_t)s]) - 1.0f;
            if (ex < candEx) continue;
            bool peak = true;
            for (int i = 1; i <= 3 && peak; ++i) peak = t >= Ts[(size_t)(s - i)] && t > Ts[(size_t)(s + i)];
            if (!peak) continue;
            // sub-bin: parabola on the log of the tone sums
            const double y0 = std::log(Ts[(size_t)s - 1] + 1e-30), y1 = std::log(t + 1e-30), y2 = std::log(Ts[(size_t)s + 1] + 1e-30);
            const double den = y0 - 2 * y1 + y2;
            const double hz = hzOfS(s + (den < 0 ? 0.5 * (y0 - y2) / den : 0.0));
            if (np < 256) pk[np++] = { s, ex, hz };
            else { int w = 0; for (int i = 1; i < np; ++i) if (pk[i].ex < pk[w].ex) w = i; if (pk[w].ex < ex) pk[w] = { s, ex, hz }; }
        }
        struct Pair { double fc, shift, strength, err, lo, hi; int a, b; };
        Pair pr[64]; int npr = 0;
        for (int a = 0; a < np; ++a) for (int b = 0; b < np; ++b) {
            if (pk[b].hz <= pk[a].hz) continue;
            const double d = pk[b].hz - pk[a].hz;
            const double S = rttyscore::snapShift(d);
            if (S <= 0) continue;
            // ★ the two tones of one transmitter are within 12 dB of each other (selective fading aside)
            const double ra = pk[a].ex, rb = pk[b].ex;
            if (std::min(ra, rb) * 16 < std::max(ra, rb)) continue;
            Pair p{ (pk[a].hz + pk[b].hz) / 2, S, 10 * std::log10(1 + std::min(ra, rb)), std::fabs(d - S) / S, pk[a].hz, pk[b].hz, a, b };
            if (npr < 64) pr[npr++] = p;
            else { int w = 0; for (int i = 1; i < npr; ++i) if (pr[i].strength < pr[w].strength) w = i; if (pr[w].strength < p.strength) pr[w] = p; }
        }
        // ★★ A CLOSE FIT TO THE STANDARD SHIFT RANKS FIRST, and each tone peak joins ONE pair a hop: in a crowded window a
        //    tone of one signal and a tone of its neighbour can stand a standard shift apart too (within 12 %) — measured,
        //    24 kHz with 30 signals: a 425 Hz "pair" across two signals took a real 170 Hz signal's place. 12 % off costs
        //    ≈ 5 dB here; an exact shift nothing.
        auto rank = [](const Pair& x) { return x.strength - 40 * x.err; };
        std::sort(pr, pr + npr, [&](const Pair& x, const Pair& y) { return rank(x) > rank(y); });
        bool used[256] = {};
        int added = 0;
        for (int i = 0; i < npr && added < 4 && (int)sigs.size() < cfg.maxSignals; ++i) {
            const Pair& p = pr[i];
            if (used[p.a] || used[p.b]) continue;
            used[p.a] = used[p.b] = true;
            bool near = false;
            // ★ a pair that shares a tone with a signal we have: the same tone is not two transmitters. A not-yet-set signal
            //   whose pair fitted the standard shift clearly worse gives way; otherwise the new pair does.
            auto share = [](double a, double b) { return std::fabs(a - b) < 20.0; };
            for (auto& g : sigs) {
                if (!g->active) continue;
                // ★★ ONE SIGNAL'S OWN SPECTRUM: a wide shift's keying makes lesser peaks BETWEEN its tones, and two of them
                //    can stand 170 Hz apart and take turns as the tones do (measured: 450 Hz signals at 96 kHz each grew a
                //    "170 Hz signal" 60–75 Hz off-centre that half-decoded). A pair inside a signal's tones is that signal.
                //    (A WIDER pair round signals we have is tried: two neighbours' outer tones can be 850 Hz apart, and the
                //    alternation test, not this, tells — see contains() at listing.)
                if (p.lo > g->lo - 20 && p.hi < g->hi + 20 && p.shift < g->shift) { near = true; break; }
                // ★ …and a pair with ONE tone inside a signal's tones and one outside is that signal's keying sidelobes (a
                //   170 Hz signal at h ≈ 3.7 has peaks a baud or two either side of its tones): two transmitters do not
                //   overlap. Measured: three such "signals" 60–75 Hz beside real ones at 96 kHz / 30 signals.
                auto inside = [&](double f) { return f > g->lo + 20 && f < g->hi - 20; };
                if ((inside(p.lo) != inside(p.hi)) && !(g->lo > p.lo - 20 && g->hi < p.hi + 20)) { near = true; break; }
                if (g->lo > p.lo - 20 && g->hi < p.hi + 20 && g->shift < p.shift && g->stage == Sig::Set && g->strength + 3 >= p.strength) { near = true; break; }
                if (!(share(p.lo, g->lo) || share(p.lo, g->hi) || share(p.hi, g->lo) || share(p.hi, g->hi))) continue;
                if (std::fabs(g->fc - p.fc) < 15.0) { near = true; break; }
                if (g->stage != Sig::Set && p.err + 0.03 < g->fitErr) {
#ifdef RTTY_TRACE
                    std::fprintf(stderr, "[%.2f] x %.1f Hz gives way to %.1f\n", nowSec(), g->fc, p.fc);
#endif
                    g->active = false; if (g->queued) dropJobsOf(g->id);
                } else { near = true; break; }
            }
            if (near) continue;
            for (auto& g : sigs) {
                if (!g->active) continue;
                const double tol = std::max(25.0, 0.3 * std::min(p.shift, g->shift));
                if (std::fabs(g->fc - p.fc) < tol) {
                    near = true;
                    // ★★ TONES THAT MOVED: the same shift a little way off, where this signal's own tones have faded —
                    //    a retune of the sender (or ours). The signal follows and is searched again.
                    if (g->stage == Sig::Set && g->shift == p.shift && std::fabs(g->fc - p.fc) > 15.0) {
                        const double own = std::min(excessAt(g->fc - g->shift / 2), excessAt(g->fc + g->shift / 2));
                        if (10 * std::log10(1 + std::max(0.0, own)) + 6 < p.strength) moveSig(*g, p.fc);
                    }
                    break;
                }
            }
            for (auto& h : holdOff) if (std::fabs(h.first - p.fc) < 25.0) { near = true; break; }
            if (near) continue;
#ifdef RTTY_TRACE
            std::fprintf(stderr, "[%.2f] + %.1f Hz shift %.0f (%.1f..%.1f) %.1f dB err %.3f\n", nowSec(), p.fc, p.shift, p.lo, p.hi, p.strength, p.err);
#endif
            addSig(p.fc, p.shift, p.strength); ++added;
            sigs.back()->lo = p.lo; sigs.back()->hi = p.hi; sigs.back()->fitErr = p.err;
        }
    }

    /** `in` lies between `out`'s tones and has the smaller shift. */
    static bool contains(const Sig& out, const Sig& in) {
        return in.shift < out.shift && in.lo > out.lo - 20 && in.hi < out.hi + 20;
    }
    void setCentre(Sig& g, double hz) {
        g.fc = hz; g.k0 = (int)std::lround(hz / binHz); g.delta = hz - g.k0 * binHz;
        // ★ the mask: flat to shift/2 + 220 Hz (the tones and their keying sidebands at 75 baud), a cosine to nothing
        //   60 Hz further — short in time, so the middle half of every inverse is clean overlap-save output
        g.mask.assign((size_t)M, 0.0f);
        const double flat = g.shift / 2 + 220, taper = 60;
        for (int o = -M / 2; o < M / 2; ++o) {
            const double f = std::fabs(o * binHz - g.delta);
            const double v = f <= flat ? 1.0 : f >= flat + taper ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * (f - flat) / taper);
            g.mask[(size_t)(o + M / 2)] = (float)v;
        }
        // the alternation test's tones, at baseband
        g.wM = kTwoPi * (g.shift / 2 + g.delta) / fsCh; g.wS = kTwoPi * (-g.shift / 2 + g.delta) / fsCh;
        // ★ the integrate-and-dump length that puts the OTHER tone in a null: a whole number of its beats, ≈ 6–7 ms
        const int k = std::max(1, (int)std::lround(g.shift * 0.007));
        g.L = std::max(4, (int)std::lround(k * fsCh / g.shift));
    }
    void addSig(double hz, double shift, double strength) {
        std::unique_ptr<Sig> g(new Sig());
        g->shift = shift; g->strength = strength;
        setCentre(*g, hz);
        g->ring.assign(ringCap(), 0);
        g->bornSec = nowSec();
        sigs.push_back(std::move(g));
    }
    void moveSig(Sig& g, double hz) {
        g.lo += hz - g.fc; g.hi += hz - g.fc;
        setCentre(g, hz);
        g.wr = 0; g.P = -1; g.dec.reset(); g.queued = false;
        g.stage = Sig::Listed; ++requeuesN;
        g.pend.clear();
        dropJobsOf(g.id);
    }

    // ── a channel: M bins → inverse → the middle half → int16 audio, stored ──
    void runChannel(Sig& g, const float* X, uint64_t hopIdx) {
        auto put = [&](int oFrom, int count, int dst) {
            int o = oFrom;
            while (count > 0) {
                const int b = binIndex(g.k0 + o);
                const int run = std::min(count, N - b);
                mulMask(X + 2 * (size_t)b, g.mask.data() + (o + M / 2), cin + 2 * (size_t)dst, run);
                o += run; dst += run; count -= run;
            }
        };
        put(0, M / 2, 0);
        put(-M / 2, M / 2, M / 2);
        pffft_transform_ordered(small, cin, cout, cwork, PFFFT_BACKWARD);
        // ★★ PHASE CONTINUITY: the inverse's output is the band mixed down by k0·Δ from the WINDOW's start (hop·H − N);
        //    relative to the input's own time that is a factor (−1)^(k0·hop) — so blocks join without a step.
        const float* z = cout + 2 * (M / 4);
        const int nOut = M / 2;
        double pw = 0;
        for (int j = 0; j < nOut; ++j) pw += (double)z[2 * j] * z[2 * j] + (double)z[2 * j + 1] * z[2 * j + 1];
        pw /= nOut;
        if (g.P < 0) g.P = std::max(pw, 1e-30); else g.P += (pw - g.P) * 0.05;   // ≈ 2 s at 12 hops a second
        const float sign = ((g.k0 & 1) && (hopIdx & 1)) ? -1.0f : 1.0f;
        const float gain = sign * (float)(3000.0 / std::sqrt(std::max(g.P, 1e-30)));
        // ★ int16 audio, centred on kChRate/4 (+ delta): ring
        toAudio(z, nOut, gain, aud.data());
        const size_t cap = g.ring.size();
        for (int j = 0; j < nOut; ) {
            const size_t at = (size_t)(g.wr % cap);
            const int run = (int)std::min<size_t>((size_t)(nOut - j), cap - at);
            std::memcpy(g.ring.data() + at, aud.data() + j, sizeof(int16_t) * (size_t)run);
            j += run; g.wr += (uint64_t)run;
        }
        // ★ a candidate: the two tones' envelopes, for the alternation test
        if (g.stage == Sig::Cand) {
            const float gs = (float)(1.0 / std::sqrt(std::max(g.P, 1e-30)));
            const cf stM((float)std::cos(g.wM), (float)-std::sin(g.wM)), stS((float)std::cos(g.wS), (float)-std::sin(g.wS));
            cf rM((float)std::cos(g.phM), (float)-std::sin(g.phM)), rS((float)std::cos(g.phS), (float)-std::sin(g.phS));
            for (int j = 0; j < nOut; ++j) {
                const cf v(z[2 * j] * gs * sign, z[2 * j + 1] * gs * sign);
                g.accM += v * rM; g.accS += v * rS;
                rM *= stM; rS *= stS;
                if (++g.inBlk >= g.L) {
                    const double m = std::abs(g.accM), s = std::abs(g.accS);
                    g.n += 1; g.sm += m; g.ss += s; g.smm += m * m; g.sss += s * s; g.sms += m * s;
                    g.accM = g.accS = cf(0, 0); g.inBlk = 0;
                }
            }
            g.phM = std::fmod(g.phM + g.wM * nOut, kTwoPi); g.phS = std::fmod(g.phS + g.wS * nOut, kTwoPi);
        }
        // ★ a set signal's one decoder runs on the new audio
        if (g.stage == Sig::Set && g.dec) feedDec(g);
    }
    void feedDec(Sig& g) {
        const size_t cap = g.ring.size();
        if (g.wr > cap && g.decAt < g.wr - cap) g.decAt = g.wr - cap;
        while (g.decAt < g.wr) {
            const size_t at = (size_t)(g.decAt % cap);
            const int run = (int)std::min<uint64_t>(g.wr - g.decAt, cap - at);
            g.dec->process(g.ring.data() + at, run);
            g.decAt += (uint64_t)run;
        }
    }

    double chanSec(const Sig& g) const { return g.wr / fsCh; }

    // ── after each hop's channel: the tests and the text ──
    void afterChannel(Sig& g) {
        const double now = nowSec();
        const double own = std::min(excessAt(g.fc - g.shift / 2), excessAt(g.fc + g.shift / 2));
        g.strength = 10 * std::log10(1 + std::max(0.0, own));
        if (own < 0.26) g.quietSec += H / cfg.rate; else g.quietSec = 0;   // ★ both tones within 1 dB of the floor
        if (g.stage == Sig::Cand) {
            if (chanSec(g) < kProbeMinSec || g.n < 20) return;
            const double vm = g.smm / g.n - (g.sm / g.n) * (g.sm / g.n), vs = g.sss / g.n - (g.ss / g.n) * (g.ss / g.n);
            const double cov = g.sms / g.n - (g.sm / g.n) * (g.ss / g.n);
            g.alt = vm > 0 && vs > 0 ? -cov / std::sqrt(vm * vs) : 0;
            if (g.alt >= cfg.alternation) {
                // ★ inside a wider pair that is itself RTTY, this is that signal's own spectrum: dropped; inside one still
                //   being tested, it waits for that one's answer (an 850 Hz "pair" across two neighbours fails its own test)
                bool wait = false, inner = false;
                for (auto& o : sigs) {
                    if (o.get() == &g || !o->active || !contains(*o, g)) continue;
                    if (o->stage == Sig::Cand) wait = true; else inner = true;
                }
                // ★ overlapping a listed signal (a sidelobe pair found first, or second): the stronger one is the signal
                for (auto& o : sigs) {
                    if (o.get() == &g || !o->active || o->stage == Sig::Cand || contains(*o, g) || contains(g, *o)) continue;
                    if (!(g.lo < o->hi - 20 && g.hi > o->lo + 20)) continue;
                    if (g.strength <= o->strength || o->stage == Sig::Set) inner = true;
                    else { o->active = false; if (o->queued) dropJobsOf(o->id); }
                }
                if (inner) { g.active = false; holdOff.push_back({ g.fc, now + kRejectHoldSec }); return; }
                if (wait && now - g.bornSec < 3 * kProbeMaxSec) return;
                // …and a signal now listed takes the place of any not-yet-set (or clearly weaker) one inside its tones
                for (auto& o : sigs) {
                    if (o.get() == &g || !o->active || !contains(g, *o)) continue;
                    if (o->stage != Sig::Set || o->strength + 3 < g.strength) { o->active = false; if (o->queued) dropJobsOf(o->id); }
                }
                // ★★ LISTED AT ONCE — "RTTY detected — waiting for parameter detection"
                g.stage = Sig::Listed; g.id = nextId++; g.listedSec = now;
                g.firstMs = nowMs() - (int64_t)(std::min(chanSec(g), now - g.bornSec + 1.0) * 1000.0); g.lastMs = nowMs();
            } else if (now - g.bornSec > kProbeMaxSec) {
#ifdef RTTY_TRACE
                std::fprintf(stderr, "[%.2f] - %.1f Hz rejected alt %.2f\n", now, g.fc, g.alt);
#endif
                g.active = false; ++rejected;
                holdOff.push_back({ g.fc, now + kRejectHoldSec });
            }
            return;
        }
        if (g.quietSec < 0.5) g.lastMs = nowMs();
        if (g.stage == Sig::Listed) {
            if (!g.queued && chanSec(g) >= cfg.jobSec) enqueue(g, false);
            return;
        }
        // ── Set: the text gate, and frames going bad ──
        const unsigned long good = g.dec->goodFrames(), bad = g.dec->framingErrors();
        const unsigned long dg = good - g.lastGood, db = bad - g.lastBad;
        g.lastGood = good; g.lastBad = bad;
        g.secGood += dg; g.secBad += db;
        if (!g.pend.empty()) {
            if (db > 0 && db >= dg) { suppressed += g.pend.size(); g.pend.clear(); }   // ★ a hop of bad frames: its text is noise
            else { show(g, g.pend); g.pend.clear(); }
        }
        g.secClock += H / cfg.rate;
        if (g.secClock >= 1.0) {
            g.secClock -= 1.0;
            g.secScore = g.secScore * 0.7 + rttyscore::frameScore((double)g.secGood, (double)g.secBad);
            if (g.secGood + g.secBad > 0) g.fq += ((double)g.secGood / (g.secGood + 3.0 * g.secBad) - g.fq) * 0.3;
            // ★ frames gone bad — or none at all: a decoder at the wrong speed or polarity mostly never LOCKS, so it counts
            //   no framing errors either — while the tones are still there: the station changed its settings. Search again.
            const bool none = g.secGood == 0;
            g.secGood = g.secBad = 0;
            if ((g.secScore < -6 || none) && g.quietSec == 0) { if ((g.badSec += 1.0) >= 8 && !g.queued) requeue(g); }
            else g.badSec = 0;
        }
    }

    void requeue(Sig& g) {
#ifdef RTTY_TRACE
        std::fprintf(stderr, "[%.2f] R %.1f Hz requeued\n", nowSec(), g.fc);
#endif
        ++requeuesN;
        g.badSec = 0; g.secScore = 0;
        g.dec.reset(); g.stage = Sig::Listed;
        g.pend.clear();
        enqueue(g, true);
    }

    // ── the queue ──
    void enqueue(Sig& g, bool again) {
        auto j = std::make_shared<Job>();
        j->sigId = g.id; j->strength = g.strength; j->requeue = again;
        const size_t cap = g.ring.size();
        // ★ the whole store on a first search (the signal from when it was first heard); the last jobSec + 2 s on a
        //   repeat (the frames that went bad are the station's NEW settings)
        size_t n = (size_t)std::min<uint64_t>(g.wr, cap);
        if (again) n = std::min(n, (size_t)std::lround((cfg.jobSec + 2) * fsCh));
        j->audio.resize(n);
        const uint64_t from = g.wr - n;
        for (size_t i = 0; i < n; ) {
            const size_t at = (size_t)((from + i) % cap);
            const size_t run = std::min(n - i, cap - at);
            std::memcpy(j->audio.data() + i, g.ring.data() + at, run * sizeof(int16_t));
            i += run;
        }
        j->endSample = g.wr;
        j->centre = (fsCh / 4 + g.delta) * fsScale; j->shift = g.shift * fsScale;
        g.queued = true;
        { std::lock_guard<std::mutex> lk(jm); queue.push_back(std::move(j)); }
        if (owner->onWork) owner->onWork();
    }
    void dropJobsOf(int id) {
        std::lock_guard<std::mutex> lk(jm);
        queue.erase(std::remove_if(queue.begin(), queue.end(), [id](const std::shared_ptr<Job>& j) { return j->sigId == id; }), queue.end());
    }

    /** ★ A worker: the strongest queued job, run here without any lock, its result posted for the feeding thread. */
    bool runOne() {
        std::shared_ptr<Job> j;
        {
            std::lock_guard<std::mutex> lk(jm);
            if (queue.empty()) return false;
            size_t b = 0;
            for (size_t i = 1; i < queue.size(); ++i) if (queue[i]->strength > queue[b]->strength) b = i;
            j = queue[b]; queue.erase(queue.begin() + (long)b);
            running.fetch_add(1);
        }
        search(*j);
        jobsRunN.fetch_add(1);
        { std::lock_guard<std::mutex> lk(jm); done.push_back(std::move(j)); running.fetch_sub(1); }
        return true;
    }

    /** ★★ THE SEARCH, over a copy of the stored channel: the fast path, then the full contest (rttyscore). */
    static void search(Job& j) {
        const auto& all = rttyscore::all();
        const int n = (int)all.size();
        struct C { std::unique_ptr<FskDecoder> d; std::string text; bool ran = false; };
        std::vector<C> c((size_t)n);
        auto runC = [&](int i) {
            const auto& s = all[(size_t)i];
            c[(size_t)i].d.reset(new FskDecoder((int)kChRate, j.centre, j.shift, s.baud, s.oneStop ? "5N1" : "5N1.5", "ITA2", s.inv));
            c[(size_t)i].d->setAfc(false);
            std::string* t = &c[(size_t)i].text;
            c[(size_t)i].d->onChar = [t](char32_t ch) { if (ch < 0x80 && t->size() < 4000) t->push_back((char)ch); };
            c[(size_t)i].d->process(j.audio.data(), (int)j.audio.size());
            c[(size_t)i].ran = true;
        };
        std::vector<double> sc((size_t)n, -1e9);
        auto score = [&](int i) { const auto& d = *c[(size_t)i].d; return rttyscore::frameScore((double)d.goodFrames(), (double)d.framingErrors()); };
        // 1. ★ THE FAST PATH: 45.45 baud, 1.5 stop, normal and reverse — most amateur RTTY
        int fast[2] = { -1, -1 }, nf = 0;
        for (int i = 0; i < n && nf < 2; ++i) if (all[(size_t)i].baud < 46 && !all[(size_t)i].oneStop) fast[nf++] = i;
        for (int k = 0; k < nf; ++k) { runC(fast[k]); sc[(size_t)fast[k]] = score(fast[k]); }
        int win = -1;
        if (nf == 2) {
            const int b = sc[(size_t)fast[0]] >= sc[(size_t)fast[1]] ? fast[0] : fast[1], o = b == fast[0] ? fast[1] : fast[0];
            const auto& d = *c[(size_t)b].d;
            // ★ "frames cleanly": the winner rule, AND eight good frames for every bad one
            if (sc[(size_t)b] >= 12 && sc[(size_t)b] >= sc[(size_t)o] + 6 && d.goodFrames() >= 8 * std::max(1ul, d.framingErrors())) win = b;
        }
        // 2. the full contest — only when the fast path did not frame cleanly
        if (win < 0) {
            for (int i = 0; i < n; ++i) if (!c[(size_t)i].ran) { runC(i); sc[(size_t)i] = score(i); }
            win = rttyscore::pickWinner(all.data(), sc.data(), n);
        }
        if (win < 0) return;
        j.found = true; j.win = all[(size_t)win];
        j.good = c[(size_t)win].d->goodFrames(); j.bad = c[(size_t)win].d->framingErrors();
        j.text = std::move(c[(size_t)win].text);
        j.dec = std::move(c[(size_t)win].d);
        j.dec->onChar = nullptr;
        j.dec->setAfc(true);   // ★ AFC on the winner only, as RTTY AUTO
    }

    void applyResults() {
        std::vector<std::shared_ptr<Job>> res;
        { std::lock_guard<std::mutex> lk(jm); if (done.empty()) return; res.swap(done); }
        for (auto& j : res) {
            Sig* g = nullptr;
            for (auto& s : sigs) if (s->id == j->sigId && s->active) { g = s.get(); break; }
            if (!g || g->stage != Sig::Listed || !g->queued) continue;
            g->queued = false; ++g->searches;
            if (!j->found) {
                // ★ NOT RTTY: nothing framed at any setting — dropped, and not tried again here for a while
                ++notRttyN;
#ifdef RTTY_TRACE
                std::fprintf(stderr, "[%.2f] N %.1f Hz not RTTY\n", nowSec(), g->fc);
#endif
                g->active = false;
                holdOff.push_back({ g->fc, nowSec() + cfg.notRttyHoldSec });
                continue;
            }
            g->st = j->win; g->stage = Sig::Set; g->setSec = nowSec();
            g->dec = std::move(j->dec);
            g->lastGood = g->dec->goodFrames(); g->lastBad = g->dec->framingErrors();
            g->fq = (double)j->good / std::max(1.0, j->good + 3.0 * j->bad);
            g->secScore = 0; g->badSec = 0; g->secGood = g->secBad = 0;
            char b[96];
            std::snprintf(b, sizeof b, "%s baud / %.0f Hz / %s", rttyscore::baudWord(g->st.baud), g->shift, g->st.inv ? "Reverse" : "Normal");
            g->settings = b;
            // ★★ THE SETTINGS GO INTO THE TEXT — and scroll away with it (Stuart: no fixed column)
            const std::string line = std::string(g->text.empty() ? "" : "\n") + "[" + g->settings + "] ";
            commit(*g, line, false);
            // ★ the text decoded over the stored channel — from when the signal was first heard — then the rest of the ring
            g->held.clear(); g->textOk = false;
            Sig* gp = g;
            g->dec->onChar = [gp](char32_t ch) { if (ch < 0x80) gp->pend.push_back((char)ch); };
            show(*g, j->text);
            g->decAt = j->endSample;
            feedDec(*g);
            if (!g->pend.empty()) { show(*g, g->pend); g->pend.clear(); }
            g->lastGood = g->dec->goodFrames(); g->lastBad = g->dec->framingErrors();
        }
    }

    // ── the text: held until it reads as text, then shown ──
    void show(Sig& g, const std::string& raw) {
        std::string t;
        for (char c : raw) {
            const bool lfAfterCr = c == '\n' && g.lastCr;
            g.lastCr = c == '\r';
            if (lfAfterCr) continue;
            if (c == '\r') c = '\n';
            if (c == '\a') continue;
            if ((unsigned char)c < 32 && c != '\n') continue;
            t.push_back(c);
        }
        if (t.empty()) return;
        if (!g.textOk) {
            g.held += t;
            if (g.held.size() > 400) { suppressed += g.held.size() - 400; g.held.erase(0, g.held.size() - 400); }
            if (!readsAsText(g.held)) return;
            g.textOk = true;
            t.swap(g.held); g.held.clear();
        }
        commit(g, t, true);
    }
    void commit(Sig& g, const std::string& t, bool words) {
        for (char c : t) {
            g.text.push_back(c); if (words) { ++g.chars; spotWord(g, c); }
        }
        if (g.text.size() > (size_t)kMaxShowCarry + 400) g.text.erase(0, g.text.size() - kMaxShowCarry);
        g.lastMs = nowMs();
        if (owner->onText) owner->onText(g.id, g.fc, t);
    }
    void spotWord(Sig& g, char c) {
        const bool sep = !(std::isalnum((unsigned char)c) || c == '/');
        if (!sep) { if (g.word.size() < 20) g.word.push_back((char)std::toupper((unsigned char)c)); return; }
        if (g.word.empty()) return;
        g.words.push_back(g.word); g.word.clear();
        if (g.words.size() > 10) g.words.erase(g.words.begin());
        std::string call, how;
        if (!cw::identify(g.words, call, how)) return;
        int& n = g.heard[call];
        ++n;
        if (n < cfg.spotRepeats) return;
        const double now = nowSec();
        auto it = g.spotted.find(call);
        if (it != g.spotted.end() && now - it->second < cfg.respotSec) return;
        g.spotted[call] = now; g.call = call;
        if (owner->onSpot) {
            Spot sp; sp.call = call; sp.offsetHz = g.fc; sp.snrDb = (float)g.strength; sp.how = how;
            char m[32]; std::snprintf(m, sizeof m, "RTTY %s/%.0f", rttyscore::baudWord(g.st.baud), g.shift);
            sp.mode = m; sp.utcMs = nowMs(); sp.signalId = g.id;
            owner->onSpot(sp);
        }
    }

    void housekeeping() {
        const double now = nowSec();
        const double lo = hzOfS(0), hi = hzOfS(nS - 1);
        for (size_t i = 0; i < sigs.size(); ) {
            Sig& g = *sigs[i];
            bool drop = !g.active;
            if (g.stage != Sig::Cand && g.quietSec > kLostSec) drop = true;     // its tones have gone
            if (g.fc < lo - 20 || g.fc > hi + 20) drop = true;
            if (drop) {
#ifdef RTTY_TRACE
                if (g.stage != Sig::Cand) std::fprintf(stderr, "[%.2f] D %.1f Hz dropped (active %d quiet %.1f stage %d)\n", now, g.fc, (int)g.active, g.quietSec, (int)g.stage);
#endif
                if (g.queued) dropJobsOf(g.id);
                if (g.chars > 0) {   // ★ heard (a re-search that found nothing too): kept in the list as last heard
                    std::unique_ptr<Sig> o = std::move(sigs[i]);
                    o->active = false; o->dec.reset();
                    o->ring.clear(); o->ring.shrink_to_fit(); o->mask.clear(); o->mask.shrink_to_fit();
                    gone.insert(gone.begin(), std::move(o));
                }
                sigs.erase(sigs.begin() + (long)i);
                continue;
            }
            ++i;
        }
        (void)now;
        const int64_t nm = nowMs();
        gone.erase(std::remove_if(gone.begin(), gone.end(), [&](const std::unique_ptr<Sig>& g) {
            return nm - g->lastMs > (int64_t)cfg.keepGoneSec * 1000; }), gone.end());
        if (gone.size() > 80) gone.resize(80);
    }

    SignalView view(const Sig& g) const {
        SignalView v;
        v.id = g.id; v.offsetHz = g.fc; v.shiftHz = g.shift; v.active = g.active;
        v.set = g.stage == Sig::Set || (!g.active && !g.settings.empty());
        if (v.set) { v.baud = g.st.baud; v.reverse = g.st.inv; v.oneStop = g.st.oneStop; v.settings = g.settings; }
        v.snrDb = (float)g.strength; v.quality = (float)std::max(0.0, std::min(1.0, g.fq));
        v.firstMs = g.firstMs; v.lastMs = g.lastMs; v.call = g.call; v.text = g.text; v.chars = g.chars;
        if (v.text.size() > 400) v.text.erase(0, v.text.size() - 400);
        return v;
    }
};

Skimmer::Skimmer(const Config& c) : d_(new Impl(c)) { d_->owner = this; }
Skimmer::~Skimmer() = default;

void Skimmer::feedIq(const std::complex<float>* x, int n) { if (n > 0) d_->feed(x, n); }
bool Skimmer::workOnce() { return d_->runOne(); }
int Skimmer::queuedJobs() const { std::lock_guard<std::mutex> lk(d_->jm); return (int)d_->queue.size(); }
int Skimmer::runningJobs() const { return d_->running.load(); }

std::vector<SignalView> Skimmer::signals(bool includeGone) const {
    std::vector<SignalView> v;
    for (auto& g : d_->sigs) if (g->stage != Impl::Sig::Cand && g->active) v.push_back(d_->view(*g));
    std::sort(v.begin(), v.end(), [](const SignalView& a, const SignalView& b) { return a.offsetHz < b.offsetHz; });
    if (includeGone) for (auto& g : d_->gone) v.push_back(d_->view(*g));
    return v;
}
bool Skimmer::signal(int id, SignalView& out) const {
    for (auto& g : d_->sigs) if (g->stage != Impl::Sig::Cand && g->id == id) { out = d_->view(*g); return true; }
    for (auto& g : d_->gone) if (g->id == id) { out = d_->view(*g); return true; }
    return false;
}
bool Skimmer::timesOf(int id, double& l, double& s) const {
    for (auto& g : d_->sigs) if (g->id == id && g->stage != Impl::Sig::Cand) { l = g->listedSec; s = g->setSec; return true; }
    for (auto& g : d_->gone) if (g->id == id) { l = g->listedSec; s = g->setSec; return true; }
    return false;
}
void Skimmer::setStartUtcMs(int64_t ms) { d_->startMs = ms - (int64_t)(d_->nowSec() * 1000.0); }
int64_t Skimmer::nowMs() const { return d_->nowMs(); }
double Skimmer::nowSec() const { return d_->nowSec(); }
int Skimmer::fftSize() const { return d_->N; }
double Skimmer::binHz() const { return d_->binHz; }
double Skimmer::channelRate() const { return d_->fsCh; }
int Skimmer::channelSize() const { return d_->M; }
int Skimmer::binsSearched() const { return d_->nS; }
uint64_t Skimmer::hops() const { return d_->hopsDone; }
int Skimmer::tracked() const { return (int)d_->sigs.size(); }
int Skimmer::listed() const { int n = 0; for (auto& g : d_->sigs) n += g->stage != Impl::Sig::Cand && g->active; return n; }
int Skimmer::settled() const { int n = 0; for (auto& g : d_->sigs) n += g->stage == Impl::Sig::Set && g->active; return n; }
uint64_t Skimmer::candidatesRejected() const { return d_->rejected; }
uint64_t Skimmer::notRtty() const { return d_->notRttyN; }
uint64_t Skimmer::jobsRun() const { return d_->jobsRunN.load(); }
uint64_t Skimmer::requeues() const { return d_->requeuesN; }
uint64_t Skimmer::charsSuppressed() const { return d_->suppressed; }
const char* Skimmer::simdName() {
#if VIBE_NEON
    return "neon";
#elif VIBE_SSE
    return "sse2";
#else
    return "scalar";
#endif
}

} // namespace rtty
} // namespace vibe
