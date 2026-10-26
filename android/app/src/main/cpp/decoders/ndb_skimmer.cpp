// decoders/ndb_skimmer.cpp — see ndb_skimmer.h for the design. Original VibeSDR code from first principles: textbook
// short-time Fourier analysis, two-means clustering, the Morse code as ITU-R M.1677 defines it (the CW skimmer's own
// table — cw::morseText), and the NDB as ICAO Annex 10 describes it.
#include "ndb_skimmer.h"
#include "cw_skimmer.h"                   // ★ cw::morseText — one Morse table for every decoder
#include "../vibedsp/simd_internal.h"     // ★ VIBE_NEON / VIBE_SSE (and VIBE_FORCE_SCALAR) — the project's one SIMD switch
#include "../vibedsp/third_party/pffft/pffft.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#ifdef NDB_DEBUG
#include <cstdio>
#include <cstdlib>
#endif

namespace vibe {
namespace ndb {

// ── The vote ───────────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
bool hasUnknown(const std::string& s) { return s.find('?') != std::string::npos; }
/** ★ One-letter idents exist (a few hundred worldwide) but E, T, I, A, N, M are what noise makes most: a single letter
 *  needs twice the agreement, and never one of those. */
/** ★★ How much a reading SAYS: its dots and dashes. Noise makes short, simple characters (E, T, I, EE, TE…) far more
 *  often than long ones, and a systematic misreading (a wrong speed) collapses an ident to a few elements — so a
 *  reading with few elements needs more agreeing repeats: ≥ 6 elements three, 4–5 four, fewer six. */
int elements(const std::string& s) {
    int n = 0;
    for (char c : s) n += (int)vibe::cw::morseCode(std::string(1, c)).size();
    return n;
}
/** ★ A reading that could be an ident at all: four elements or more, at least one character of two or more (a speed read
 *  three times too slow turns every dash into a dot — "EEEE", "ETTE"), and not one letter over and over. */
bool informative(const std::string& s) {
    if (elements(s) < 4) return false;
    bool multi = false, same = true;
    for (char c : s) { multi |= vibe::cw::morseCode(std::string(1, c)).size() >= 2; same &= c == s[0]; }
    return multi && !same;
}
}

void IdentVote::expire(int64_t ms) {
    while ((int)b_.size() > r_.window) b_.pop_front();
    while (!b_.empty() && ms - b_.front().second > r_.maxAgeMs) b_.pop_front();
}

int IdentVote::votes() const {
    if (ident_.empty()) return 0;
    int n = 0;
    for (auto& b : b_) n += b.first == ident_;
    return n;
}

std::string IdentVote::leading() const {
    std::map<std::string, int> c;
    for (auto& b : b_) if (!hasUnknown(b.first)) ++c[b.first];
    std::string best; int bn = 0;
    for (auto& kv : c) if (kv.second > bn) { bn = kv.second; best = kv.first; }
    return best;
}

bool IdentVote::add(const std::string& reading, int64_t ms) {
    if (reading.empty() || reading.size() > 6) return false;
    b_.push_back({ reading, ms });
    expire(ms);
    if (reading == ident_) lastMs_ = ms;
    // ── exact agreement ──
    std::map<std::string, int> c;
    for (auto& b : b_) if (!hasUnknown(b.first)) ++c[b.first];
    std::string best; int bn = 0, second = 0;
    for (auto& kv : c) {
        if (kv.second > bn) { second = bn; bn = kv.second; best = kv.first; }
        else if (kv.second > second) second = kv.second;
    }
    std::string won;
    if (!best.empty() && best.size() >= 2 && best.size() <= 5 && informative(best)) {   // ★ one letter is never confirmed by the vote alone
        const int el = elements(best);
        const int need = el >= 6 ? r_.exact : el >= 4 ? r_.exact + 1 : 2 * r_.exact;
        if (bn >= need && bn >= r_.dominance * second) won = best;
    }
    // ── letter-by-letter majority, among the readings of the commonest length ──
    if (won.empty()) {
        std::map<size_t, int> lens;
        for (auto& b : b_) if (b.first.size() >= 2 && b.first.size() <= 5) ++lens[b.first.size()];
        size_t L = 0; int ln = 0;
        for (auto& kv : lens) if (kv.second > ln) { ln = kv.second; L = kv.first; }
        if (L >= 2 && ln >= r_.positional) {
            std::string maj(L, '?');
            bool ok = true;
            for (size_t i = 0; i < L && ok; ++i) {
                std::map<char, int> lc; int known = 0;
                for (auto& b : b_) if (b.first.size() == L && b.first[i] != '?') { ++lc[b.first[i]]; ++known; }
                char bc = 0; int bcn = 0;
                for (auto& kv : lc) if (kv.second > bcn) { bcn = kv.second; bc = kv.first; }
                if (known < r_.positional || bcn * 10 < known * 7) ok = false; else maj[i] = bc;
            }
            if (ok && informative(maj)) {
                const auto it = c.find(maj);
                if (it != c.end() && it->second >= r_.positionalWhole) won = maj;
            }
        }
    }
    if (won.empty() || won == ident_) return false;
    if (!fold_.empty() && fold_ != won) return false;
    if (!ident_.empty()) {
        // ★ A NEW ident on this frequency (two beacons share it; the other faded in) — only if it now clearly beats the
        //   old one AND the old one has not been read for three minutes, so one noisy minute cannot flip a confirmed ident.
        const int oldN = c.count(ident_) ? c[ident_] : 0;
        if (c[won] < 2 * std::max(1, oldN) || ms - lastMs_ < 180000) return false;
        others_.erase(std::remove(others_.begin(), others_.end(), ident_), others_.end());
        others_.insert(others_.begin(), ident_);
        if (others_.size() > 4) others_.resize(4);
        others_.erase(std::remove(others_.begin(), others_.end(), won), others_.end());
    }
    ident_ = won; firstMs_ = ms; lastMs_ = ms;
    return true;
}

bool IdentVote::addFolded(const std::vector<std::string>& g, int64_t ms) {
    if (g.size() < 2) return false;
    for (const auto& w : g) if (w.empty() || w != g[0] || hasUnknown(w)) return false;
    const std::string& w = g[0];
    if (w.size() < 2 || w.size() > 5 || !informative(w)) return false;
    if (w == ident_) { lastMs_ = ms; return false; }
    // ★ A confirmed ident is CORRECTED by three agreeing groups (nine repeats or more, averaged three by three) — stronger
    //   evidence than any count of single repeats — but never by two.
    if (!ident_.empty()) {
        if (g.size() < 3) return false;
        others_.erase(std::remove(others_.begin(), others_.end(), ident_), others_.end());
        others_.insert(others_.begin(), ident_);
        if (others_.size() > 4) others_.resize(4);
        others_.erase(std::remove(others_.begin(), others_.end(), w), others_.end());
    }
    ident_ = w; firstMs_ = lastMs_ = ms;
    return true;
}

bool IdentVote::addFoldWindow(const std::string& w, int64_t ms) {
    if (w.size() < 2 || w.size() > 5 || hasUnknown(w) || !informative(w)) return false;
    win_.push_back(w);
    while (win_.size() > 4) win_.pop_front();
    if (w == ident_) { lastMs_ = ms; return false; }
    if (!ident_.empty()) return false;
    int same = 0; for (auto& x : win_) same += x == w;
    if (same < 2) return false;
    ident_ = w; firstMs_ = lastMs_ = ms;
    return true;
}

// ── The kernels ────────────────────────────────────────────────────────────────────────────────────────────────────
// ★★ Every loop over ALL bins: a NEON body (ARMv7-safe intrinsics only, so the Pi 2 runs the same code as arm64), an SSE2
//    body (nothing newer — a 32-bit Atom runs it) and the scalar reference (VIBE_FORCE_SCALAR; what the ARMv6 Pi runs).
namespace {
/** out = in · w, complex interleaved. */
void windowCplx(const float* in, const float* w, float* out, int n) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        float32x4x2_t z = vld2q_f32(in + 2 * i);
        const float32x4_t ww = vld1q_f32(w + i);
        z.val[0] = vmulq_f32(z.val[0], ww); z.val[1] = vmulq_f32(z.val[1], ww);
        vst2q_f32(out + 2 * i, z);
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        const __m128 ww = _mm_loadu_ps(w + i);
        const __m128 lo = _mm_unpacklo_ps(ww, ww), hi = _mm_unpackhi_ps(ww, ww);   // ★ r0 i0 r1 i1 take w0 w0 w1 w1
        _mm_storeu_ps(out + 2 * i,     _mm_mul_ps(_mm_loadu_ps(in + 2 * i), lo));
        _mm_storeu_ps(out + 2 * i + 4, _mm_mul_ps(_mm_loadu_ps(in + 2 * i + 4), hi));
    }
#endif
    for (; i < n; ++i) { out[2 * i] = in[2 * i] * w[i]; out[2 * i + 1] = in[2 * i + 1] * w[i]; }
}
/** ★ P[i] = |z[i]|² AND L[i] += a·(P[i] − L[i]) — one pass over the bins, the power and its long average together. */
void powerAvg(const float* z, float* P, float* L, float a, int n) {
    int i = 0;
#if VIBE_NEON
    const float32x4_t aa = vdupq_n_f32(a);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(z + 2 * i);
        const float32x4_t p = vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]);
        float32x4_t l = vld1q_f32(L + i);
        l = vmlaq_f32(l, vsubq_f32(p, l), aa);
        vst1q_f32(P + i, p); vst1q_f32(L + i, l);
    }
#elif VIBE_SSE
    const __m128 aa = _mm_set1_ps(a);
    for (; i + 4 <= n; i += 4) {
        __m128 re, im; vibedsp::sseLoad2(z + 2 * i, re, im);
        const __m128 p = _mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im));
        __m128 l = _mm_loadu_ps(L + i);
        l = _mm_add_ps(l, _mm_mul_ps(_mm_sub_ps(p, l), aa));
        _mm_storeu_ps(P + i, p); _mm_storeu_ps(L + i, l);
    }
#endif
    for (; i < n; ++i) {
        const float p = z[2 * i] * z[2 * i] + z[2 * i + 1] * z[2 * i + 1];
        P[i] = p; L[i] += a * (p - L[i]);
    }
}
/** Indices where L[i] > k·F[i] — the carrier scan's vector pre-test; a group of four with nothing in it costs one compare. */
int above(const float* L, const float* F, float k, int n, int* idx, int maxIdx) {
    int c = 0, i = 0;
#if VIBE_NEON
    const float32x4_t kk = vdupq_n_f32(k);
    for (; i + 4 <= n && c + 4 <= maxIdx; i += 4) {
        const uint32x4_t g = vcgtq_f32(vld1q_f32(L + i), vmulq_f32(vld1q_f32(F + i), kk));
        const uint32x2_t o = vorr_u32(vget_low_u32(g), vget_high_u32(g));
        if ((vget_lane_u32(o, 0) | vget_lane_u32(o, 1)) == 0) continue;
        for (int j = 0; j < 4; ++j) if (L[i + j] > k * F[i + j]) idx[c++] = i + j;
    }
#elif VIBE_SSE
    const __m128 kk = _mm_set1_ps(k);
    for (; i + 4 <= n && c + 4 <= maxIdx; i += 4) {
        const int mask = _mm_movemask_ps(_mm_cmpgt_ps(_mm_loadu_ps(L + i), _mm_mul_ps(_mm_loadu_ps(F + i), kk)));
        if (!mask) continue;
        for (int j = 0; j < 4; ++j) if (mask & (1 << j)) idx[c++] = i + j;
    }
#endif
    for (; i < n && c < maxIdx; ++i) if (L[i] > k * F[i]) idx[c++] = i;
    return c;
}
bool pffftSize(int n) {
    if (n < 32 || n % 32) return false;
    while (n % 2 == 0) n /= 2;
    while (n % 3 == 0) n /= 3;
    while (n % 5 == 0) n /= 5;
    return n == 1;
}
int nearestPffftSize(double target) {
    int best = 32; double bd = 1e9;
    for (int n = 32; n <= 131072; n += 32) {
        if (!pffftSize(n)) continue;
        const double d = std::fabs(std::log((double)n / target));
        if (d < bd) { bd = d; best = n; }
    }
    return best;
}

constexpr int kBlock = 64;            // bins per floor block
/** ★ RAM ↔ SENSITIVITY: 96 s of two floats a hop (≈ 36 KB a carrier; 3.6 MB for a hundred) — nine repeats of a 10 s
 *  ident cycle, three disjoint groups of three for the fold. The streaming decode looks at the last 48 s of it. */
constexpr double kHistSec = 96.0;
constexpr double kStreamSec = 48.0;
/** ★★ THE FLOOR'S BIAS, MEASURED (ndb_bench bias, pure noise): the 30th percentile of L sits this far below its mean.
 *  test-ndb checks noiseCalibration() stays within ±0.3 dB of 1. */
constexpr float kFloorBias = 1.045f;
constexpr double kDefaultWpm = 7.0;   // ICAO: "approximately 7 words per minute"

/** Two-means on values v (already in the domain to cluster): the split, the two means, their variances and counts. */
struct TwoMeans { double lo = 0, hi = 0, vlo = 0, vhi = 0; int nlo = 0, nhi = 0; double split = 0; };
TwoMeans twoMeans(const std::vector<float>& v, double initLo, double initHi) {
    TwoMeans t; t.lo = initLo; t.hi = initHi;
    for (int it = 0; it < 12; ++it) {
        const double s = 0.5 * (t.lo + t.hi);
        double a = 0, b = 0; int na = 0, nb = 0;
        for (float x : v) { if (x < s) { a += x; ++na; } else { b += x; ++nb; } }
        if (!na || !nb) break;
        const double lo = a / na, hi = b / nb;
        const bool same = std::fabs(lo - t.lo) < 1e-6 && std::fabs(hi - t.hi) < 1e-6;
        t.lo = lo; t.hi = hi;
        if (same) break;
    }
    t.split = 0.5 * (t.lo + t.hi);
    double a = 0, b = 0;
    for (float x : v) { if (x < t.split) { a += (x - t.lo) * (x - t.lo); ++t.nlo; } else { b += (x - t.hi) * (x - t.hi); ++t.nhi; } }
    t.vlo = t.nlo ? a / t.nlo : 0; t.vhi = t.nhi ? b / t.nhi : 0;
    return t;
}
} // namespace

const char* Skimmer::simdName() {
#if VIBE_NEON
    return "neon";
#elif VIBE_SSE
    return "sse2";
#else
    return "scalar";
#endif
}

// ── The engine ─────────────────────────────────────────────────────────────────────────────────────────────────────
struct Skimmer::Impl {
    Skimmer* owner = nullptr;     // ★ for its callbacks
    Config cfg;
    int N = 0, H = 0;
    double binHz = 0, hopSec = 0;
    PFFFT_Setup* setup = nullptr;
    float *fin = nullptr, *fout = nullptr, *work = nullptr;
    std::vector<float> win, in;
    int have = 0;                 // complex samples in `in` (fills to N, then each hop moves it by H)
    int s0 = 0, s1 = 0;           // searched carrier bins (shifted spectrum)
    int w0 = 0, w1 = 0, nw = 0;   // work bins (searched ± the tone window)
    int kLo = 0, kHi = 0;         // tone offsets, bins
    std::vector<char> toneOk;     // per offset kLo..kHi: may a tone be there (the ICAO windows)
    std::vector<float> P, L, Fl, blockQ, scratch;
    std::vector<int> cand;
    float aL = 0;
    uint64_t hopN = 0;
    int hopsPerTick = 47;
    int64_t startMs = 0;
    int nextId = 1;
    int R = 0;                    // envelope ring length (hops)
    uint64_t words = 0;
    bool floorReady = false;

    /** One decoding channel of a carrier: the tone's sidebands (A2A) or the carrier itself (A1A). */
    struct Chan {
        std::vector<float> e;     // ring, R hops
        uint64_t from = 0;        // absolute hop of the oldest valid sample
        uint64_t decodedTo = 0;   // absolute hop decoded up to (a point inside a word gap)
        float unit = 0;           // dot, hops (0 = not yet known)
        bool unitSure = false;    // ★ learnt from marks that fell into two clusters, a dash's length from a dot's
        float markLin = 0, noiseLin = 0;   // the last decode's cluster means
        bool keyed = false;
        bool keyedStrong = false; // ★ has ever been keyed eight spreads clear of its noise (a real keyed carrier, A1A)
        int64_t lastWordMs = 0;
        double period = 0;        // the ident's repeat period, hops (0 = not found)
        int64_t lastFoldMs = 0;
        std::string foldReading;  // the whole history's fold, read (a guess until the groups agree)
        uint64_t foldBallotEnd = 0;   // ★ the last whole-fold ballot's window ended here (the next must start after it)
        void reset(int R, uint64_t at) { e.assign((size_t)R, 0.0f); from = at; decodedTo = at; keyed = false; markLin = noiseLin = 0; period = 0; foldReading.clear(); }
    };
    struct Car {
        int id = 0;
        int bin = 0;              // work-relative index of its peak
        double frac = 0;          // sub-bin refinement
        bool live = false;
        int seen = 0, missed = 0;
        bool suspect = false;     // ★ shaped like a sideband: goes live only if it proves STEADY (a carrier is; a keyed tone is not)
        float lmin = 0, lmax = 0; // its long average's range while tentative
        float lvl = 0, snr = 0;
        int tone = 0;             // bins either side (0 = none yet)
        double toneFrac = 0;
        std::vector<float> acc;   // the tone search's long averages, per offset kLo..kHi
        bool accSeeded = false;
        int toneTicks = 0;        // ★ tone-search seconds so far: no tone is chosen on fewer than eight
        Chan t, c;
        IdentVote vote;
        std::string copy;
        int64_t firstMs = 0, lastMs = 0, lastToneWordMs = 0;
        bool keyedCarrier = false;
        // ★★ "FOUR ON FREQ": beacons sharing a frequency land in one bin. When the other ICAO tone window holds a keyed
        //    tone too, a CHILD (same bin, that window only) decodes it as its own beacon, with its own vote.
        int parent = 0, child = 0;  // ids (0 = none)
        int window = 0;             // 0 = either tone window, 1 = 400 Hz, 2 = 1020 Hz
        bool dirty = false;         // ★ the carrier's own bin is keyed by something else — the tone is detected incoherently
    };
    Car* byId(int id) { for (auto& c : cars) if (c->id == id) return c.get(); return nullptr; }
    int toneWindow(int k) const { return k * binHz < 700.0 ? 1 : 2; }
    std::vector<std::unique_ptr<Car>> cars;
    std::vector<std::unique_ptr<Car>> born;   // children made during a tick, added after it
    std::deque<BeaconView> gone;

    // ── geometry ──
    double offsetOf(double s) const { return (s - N / 2) * binHz; }
    int64_t now() const { return startMs + (int64_t)std::llround((double)hopN * hopSec * 1000.0); }

    void init(const Config& c) {
        cfg = c;
        N = nearestPffftSize(c.rate / c.binHz);
        H = N / 2;
        binHz = c.rate / N;
        hopSec = H / c.rate;
        hopsPerTick = std::max(1, (int)std::lround(1.0 / hopSec));
        setup = pffft_new_setup(N, PFFFT_COMPLEX);
        fin = (float*)pffft_aligned_malloc((size_t)N * 2 * sizeof(float));
        fout = (float*)pffft_aligned_malloc((size_t)N * 2 * sizeof(float));
        work = (float*)pffft_aligned_malloc((size_t)N * 2 * sizeof(float));
        win.resize((size_t)N);
        // ★ Hann (periodic), 50 % overlap: sidelobes −31 dB falling 18 dB an octave; a carrier's main lobe is ±2 bins.
        for (int i = 0; i < N; ++i) win[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2.0 * M_PI * i / N);
        in.assign((size_t)N * 2, 0.0f);
        kLo = std::max(3, (int)std::lround(c.toneLoHz / binHz));
        kHi = std::max(kLo + 1, (int)std::lround(c.toneHiHz / binHz));
        toneOk.assign((size_t)(kHi - kLo + 1), 1);
        if (c.toneIcaoOnly)
            for (int k = kLo; k <= kHi; ++k) {
                const double hz = k * binHz;
                toneOk[(size_t)(k - kLo)] = (hz >= 360 - binHz && hz <= 440 + binHz) || (hz >= 960 - binHz && hz <= 1080 + binHz);
            }
        const int edge = (int)std::ceil(N * 0.03);   // ★ never the outermost 3 % — the slice's own filter roll-off
        const double lo = std::max(c.loHz, -c.rate / 2), hi = std::min(c.hiHz, c.rate / 2);
        s0 = (int)std::floor(lo / binHz) + N / 2;
        s1 = (int)std::ceil(hi / binHz) + N / 2 + 1;
        s0 = std::max(s0, edge); s1 = std::min(s1, N - edge);
        if (s1 <= s0) s1 = s0 + 1;
        w0 = std::max(0, s0 - kHi - 4); w1 = std::min(N, s1 + kHi + 4); nw = w1 - w0;
        P.assign((size_t)nw, 0.0f); L = P; Fl.assign((size_t)nw, 1e-30f);
        cand.assign((size_t)nw, 0);
        blockQ.assign((size_t)(nw / kBlock + 2), 0.0f);
        aL = (float)(1.0 - std::exp(-hopSec / 1.5));
        R = (int)std::ceil(kHistSec / hopSec);
        startMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    ~Impl() {
        if (setup) pffft_destroy_setup(setup);
        if (fin) pffft_aligned_free(fin);
        if (fout) pffft_aligned_free(fout);
        if (work) pffft_aligned_free(work);
    }

    void feed(const std::complex<float>* x, int n) {
        const float* f = reinterpret_cast<const float*>(x);
        while (n > 0) {
            const int room = N - have;
            const int take = std::min(room, n);
            std::memcpy(in.data() + 2 * (size_t)have, f, sizeof(float) * 2 * (size_t)take);
            have += take; f += 2 * take; n -= take;
            if (have == N) {
                doHop();
                std::memmove(in.data(), in.data() + 2 * (size_t)H, sizeof(float) * 2 * (size_t)(N - H));
                have = N - H;
            }
        }
    }

    void doHop() {
        windowCplx(in.data(), win.data(), fin, N);
        pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
        // ★ Shifted bin s ↔ natural bin (s + N/2) mod N: two contiguous runs.
        const int half = N / 2;
        const float a = hopN < 8 ? 0.5f : (hopN < (uint64_t)hopsPerTick * 2 ? std::max(aL, 0.05f) : aL);   // ★ warm-up
        const int a1 = std::min(w1, half);
        if (w0 < a1) powerAvg(fout + 2 * (w0 + half), P.data(), L.data(), a, a1 - w0);
        const int b0 = std::max(w0, half);
        if (b0 < w1) powerAvg(fout + 2 * (b0 - half), P.data() + (b0 - w0), L.data() + (b0 - w0), a, w1 - b0);
        ++hopN;
        for (auto& c : cars) if (c->live) record(*c);
        if (hopN % (uint64_t)hopsPerTick == 0 && hopN >= (uint64_t)hopsPerTick * 2) tick();
    }

    /** ★★ THE AM DETECTOR, IN THE FILTERBANK, COHERENTLY. An A2A ident is two sidebands phase-locked to the carrier:
     *  U = a·e^{j(φc+φt)}, L = a·e^{j(φc−φt)}. Turned by the carrier's own phase (C/|C|, from the same frame), U·C̃* and
     *  L*·C̃ are both a·e^{jφt} — the same phasor — so their SUM is 2a while the two bins' noises add only in power:
     *  3 dB better than adding the two sidebands' powers, and 6 dB better than either alone. (A frame's window delay
     *  turns U, C and L by amounts proportional to their offsets from the bin centres, which the products cancel in the
     *  same way, so it holds wherever the tone falls between bins.) */
    void record(Car& c) {
        const size_t i = (size_t)(hopN % (uint64_t)R);
        if (c.tone) {
            const float* cz = bin(c.bin); const float* uz = bin(c.bin + c.tone); const float* lz = bin(c.bin - c.tone);
            const float cm = std::sqrt(cz[0] * cz[0] + cz[1] * cz[1]);
            if (cm > 0 && !c.dirty && cfg.coherent) {
                const float cr = cz[0] / cm, ci = cz[1] / cm;
                // U·C̃* + L*·C̃
                const float zr = (uz[0] * cr + uz[1] * ci) + (lz[0] * cr + lz[1] * ci);
                const float zi = (uz[1] * cr - uz[0] * ci) + (lz[0] * ci - lz[1] * cr);
                c.t.e[i] = zr * zr + zi * zi;
            } else c.t.e[i] = P[(size_t)(c.bin + c.tone)] + P[(size_t)(c.bin - c.tone)];
        }
        if (!c.parent) c.c.e[i] = P[(size_t)c.bin];
    }
    /** The complex FFT output of work bin b (interleaved re, im). */
    const float* bin(int b) const { const int s = w0 + b, nat = (s + N / 2) % N; return fout + 2 * (size_t)nat; }

    // ── per second ──
    void tick() {
        updateFloor();
        scan();
        for (auto& c : cars) if (c->live) { toneSearch(*c); decodeCar(*c); }
        for (auto& b : born) cars.push_back(std::move(b));
        born.clear();
        // ★ A "carrier" sitting on another's tone sideband is that sideband (an ident sent without pause looks steady):
        //   it goes, and isSideband keeps it from coming back.
        for (size_t i = 0; i < cars.size();) {
            bool sb = false;
            for (auto& o : cars)
                if (o.get() != cars[i].get() && o->live && o->tone && o->bin != cars[i]->bin && o->snr > cars[i]->snr + 3.0f &&
                    (std::abs(cars[i]->bin - (o->bin + o->tone)) <= 1 || std::abs(cars[i]->bin - (o->bin - o->tone)) <= 1)) { sb = true; break; }
            if (sb) cars.erase(cars.begin() + (long)i); else ++i;
        }
        const int64_t t = now();
        while (!gone.empty() && t - gone.back().lastMs > (int64_t)cfg.keepGoneSec * 1000) gone.pop_back();
    }

    /** ★ The noise floor: per 64-bin block, the 30th percentile of L (a block of 1.5 kHz holds at most a few carriers'
     *  main lobes and sidebands, never 70 % of it), interpolated between block centres, times the measured bias. */
    void updateFloor() {
        const int nb = (nw + kBlock - 1) / kBlock;
        for (int b = 0; b < nb; ++b) {
            const int lo = b * kBlock, hi = std::min(nw, lo + kBlock);
            scratch.assign(L.begin() + lo, L.begin() + hi);
            const size_t q = scratch.size() * 3 / 10;
            std::nth_element(scratch.begin(), scratch.begin() + (long)q, scratch.end());
            blockQ[(size_t)b] = std::max(1e-30f, scratch[q] * kFloorBias);
        }
        for (int i = 0; i < nw; ++i) {
            const double pos = (i + 0.5) / kBlock - 0.5;
            int b = (int)std::floor(pos); double f = pos - b;
            if (b < 0) { b = 0; f = 0; }
            if (b >= nb - 1) { b = nb - 1; f = 0; }
            const float v0 = blockQ[(size_t)b], v1 = blockQ[(size_t)std::min(nb - 1, b + 1)];
            Fl[(size_t)i] = (float)(v0 + (v1 - v0) * f);
        }
        floorReady = true;
    }

    Car* carNear(int b, int within) {
        for (auto& c : cars) if (std::abs(c->bin - b) <= within) return c.get();
        return nullptr;
    }
    bool isLocalPeak(int b) const {
        if (b < 2 || b + 2 >= nw) return false;
        const float m = L[(size_t)b];
        return m >= L[(size_t)(b - 1)] && m > L[(size_t)(b + 1)] && m >= L[(size_t)(b - 2)] && m > L[(size_t)(b + 2)];
    }
    /** ★ A window sidelobe of a much stronger carrier: Hann's −31 dB at 2.5 bins, falling 18 dB an octave — a candidate
     *  must stand 10 dB clear of what the strongest neighbour within 40 bins leaks into its bin. */
    bool isSidelobe(int b) const {
        const float m = L[(size_t)b];
        for (int d = 3; d <= 40; ++d) {
            const float side = (float)(7.1e-4 * std::pow(2.5 / d, 6.0)) * 10.0f;
            if (b - d >= 0 && L[(size_t)(b - d)] * side > m) return true;
            if (b + d < nw && L[(size_t)(b + d)] * side > m) return true;
        }
        return false;
    }
    /** ★ An IQ IMAGE: the mirror about the radio's DC of a carrier ≥ 15 dB stronger (an RTL's I/Q imbalance puts one
     *  there, 25–40 dB down) — a beacon exactly mirrored by a far stronger one is not a beacon. */
    bool isImage(int b) const {
        if (cfg.avoidWidthHz <= 0) return false;
        const double mirror = 2.0 * cfg.avoidHz - offsetOf(w0 + b);
        const int m = (int)std::lround(mirror / binHz) + N / 2 - w0;
        if (m < 1 || m + 1 >= nw) return false;
        const float lm = std::max(L[(size_t)m], std::max(L[(size_t)(m - 1)], L[(size_t)(m + 1)]));
        return lm > 31.6f * L[(size_t)b];
    }

    /** ★★ A CARRIER IS A LINE. Its excess over the floor must be four times that of the bins around it (3–8 bins out,
     *  beyond its main lobe): a data signal's hump — the maritime DGPS beacons' MSK at 283.5–325 kHz, a broadcaster's
     *  sidebands — has local peaks too, a few dB over its own shoulders, and they key like noise. */
    bool isNarrow(int b) const {
        double sur = 0; int n = 0;
        for (int d = 3; d <= 8; ++d) {
            if (b - d >= 0) { sur += L[(size_t)(b - d)] - Fl[(size_t)(b - d)]; ++n; }
            if (b + d < nw) { sur += L[(size_t)(b + d)] - Fl[(size_t)(b + d)]; ++n; }
        }
        sur = n ? sur / n : 0;
        return (L[(size_t)b] - Fl[(size_t)b]) >= 4.0 * std::max(0.0, sur) + 0.1 * Fl[(size_t)b];
    }

    /** ★★ One of a keyed tone's two sidebands: a carrier ≥ 6 dB stronger sits a tone's distance away, and the mirror
     *  image on its other side is there too. (Two real beacons placed symmetrically about a third, both 6 dB under it,
     *  is the case this gets wrong; the tone search's suppression below catches the rest.) */
    bool isSideband(int b) const {
        const float m = L[(size_t)b];
        for (int d = kLo - 1; d <= kHi + 1; ++d) {
            // ★ Only at a tone's distance (the ICAO windows): carriers on Europe's 0.5 kHz raster are 500 Hz apart, and the
            //   middle of three is not the outer two's tone.
            const int dk = std::max(kLo, std::min(kHi, d));
            if (!toneOk[(size_t)(dk - kLo)]) continue;
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const int c = b + sgn * d, mir = c + sgn * d;
                if (c < 2 || c + 2 >= nw || mir < 1 || mir + 1 >= nw) continue;
                if (L[(size_t)c] < 4.0f * m || !isLocalPeak(c)) continue;
                const float mm = std::max(L[(size_t)mir], std::max(L[(size_t)(mir - 1)], L[(size_t)(mir + 1)]));
                if (mm >= 0.25f * m && mm > 1.3f * Fl[(size_t)mir]) return true;
            }
        }
        for (auto& c : cars)
            if (c->live && c->tone && (std::abs(b - (c->bin + c->tone)) <= 1 || std::abs(b - (c->bin - c->tone)) <= 1)) return true;
        return false;
    }

    void scan() {
        const int lo = s0 - w0, hi = s1 - w0;
        const float k = (float)std::pow(10.0, cfg.carrierDb / 10.0);
        for (auto& c : cars) c->seen = c->seen > 0 ? -c->seen : c->seen;   // ★ negative = not seen this scan (yet)
        const int nc = above(L.data() + lo, Fl.data() + lo, k, hi - lo, cand.data(), (int)cand.size());
        for (int j = 0; j < nc; ++j) {
            const int b = cand[(size_t)j] + lo;
            if (!isLocalPeak(b)) continue;
            if (std::fabs(offsetOf(w0 + b) - cfg.avoidHz) <= cfg.avoidWidthHz) continue;   // ★ the radio's DC
            if (isImage(b)) continue;
            Car* near = carNear(b, 2);
            if (near) {
                if (near->seen <= 0) near->seen = -near->seen + 1;
                // ★ The peak moved a bin (it sits between two): follow it, keep the history.
                if (near->bin != b && L[(size_t)b] > L[(size_t)near->bin] * 1.26f && !near->live) near->bin = b;
                continue;
            }
            if (isSidelobe(b) || !isNarrow(b)) continue;
            const bool suspect = isSideband(b);
            if ((int)cars.size() >= cfg.maxCarriers) {
                // ★ Full: a clearly stronger newcomer takes the weakest TENTATIVE's place; live carriers stay.
                size_t weak = cars.size(); float wl = 0;
                for (size_t i = 0; i < cars.size(); ++i)
                    if (!cars[i]->live && (weak == cars.size() || L[(size_t)cars[i]->bin] < wl)) { weak = i; wl = L[(size_t)cars[i]->bin]; }
                if (weak == cars.size() || L[(size_t)b] < 2.0f * wl) continue;
                cars.erase(cars.begin() + (long)weak);
            }
            auto c = std::make_unique<Car>();
            c->id = nextId++; c->bin = b; c->seen = 1; c->suspect = suspect;
            c->lmin = c->lmax = L[(size_t)b];
            c->firstMs = c->lastMs = now();
            c->vote = IdentVote(cfg.vote);
            cars.push_back(std::move(c));
        }
        // ★ Hysteresis: a live carrier is still "there" down to 1.5 dB under the detection threshold.
        const float keep = k * 0.7f;
        for (size_t i = 0; i < cars.size();) {
            Car& c = *cars[i];
            const bool seenNow = c.seen > 0 || (c.live && L[(size_t)c.bin] > keep * Fl[(size_t)c.bin]);
            if (seenNow) {
                if (c.seen <= 0) c.seen = -c.seen + 1;
                c.missed = 0; c.lastMs = now();
                if (!c.live) {
                    const float l = L[(size_t)c.bin];
                    c.lmin = std::min(c.lmin, l); c.lmax = std::max(c.lmax, l);
                    // ★ A sideband-shaped candidate must stay above the threshold for fifteen seconds without a break: a keyed
                    //   ident's sideband falls to the floor in every pause between idents (and a tentative is dropped the
                    //   first scan it is missing); a carrier that happens to sit under a stronger beacon's sideband never does.
                    //   …and within 6 dB of itself all that time: a STRONG beacon's sideband stays above the threshold through
                    //   the pauses, but swings far more than that between ident and pause.
                    if (c.suspect ? (c.seen >= 15 && c.lmax < 4.0f * c.lmin) : c.seen >= 2) goLive(c);
                    else if (c.suspect && c.lmax >= 4.0f * c.lmin) { c.seen = 0; c.missed = 1; }
                }
            } else {
                c.seen = 0; ++c.missed;
            }
            const bool drop = c.live ? c.missed > 60 : c.missed > 0;   // ★ a live carrier survives a minute's fade
            if (drop) {
#ifdef NDB_DEBUG
                if (c.live) std::fprintf(stderr, "[ndb] %6.1f s car %d DROPPED (missed %d)\n", now() / 1000.0, c.id, c.missed);
#endif
                if (c.live) { gone.push_front(view(c, false)); if (gone.size() > 400) gone.pop_back(); }
                cars.erase(cars.begin() + (long)i);
                continue;
            }
            if (c.live) {
                const float l = L[(size_t)c.bin], f = Fl[(size_t)c.bin];
                c.lvl = l; c.snr = (float)(10.0 * std::log10(std::max(1e-3, (double)l / f - 1.0)));
                const float a = L[(size_t)(c.bin - 1)], m = L[(size_t)c.bin], z = L[(size_t)(c.bin + 1)];
                const double la = std::log(std::max(a, 1e-30f)), lm = std::log(std::max(m, 1e-30f)), lz = std::log(std::max(z, 1e-30f));
                const double den = la - 2 * lm + lz;
                c.frac = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (la - lz) / den)) : 0.0;
            }
            ++i;
        }
    }
    void goLive(Car& c) {
#ifdef NDB_DEBUG
        std::fprintf(stderr, "[ndb] %6.1f s car %d LIVE at bin %d (%+.1f Hz)\n", now() / 1000.0, c.id, c.bin, offsetOf(w0 + c.bin));
#endif
        c.live = true;
        c.c.reset(R, hopN + 1);
        c.acc.assign((size_t)(kHi - kLo + 1), 0.0f);
    }

    /** ★ The tone: of the sideband PAIRS kLo..kHi bins out, the one whose weaker side has stood highest over the floor
     *  on a ≈ 10 s average. The weaker side, so a neighbouring carrier on one side is never taken for a tone. */
    void toneSearch(Car& c) {
        const float a = c.accSeeded ? 0.1f : 1.0f;
        int best = 0; float bv = 0;
        for (int k = kLo; k <= kHi; ++k) {
            const int u = c.bin + k, d = c.bin - k;
            float v = 0;
            if (toneOk[(size_t)(k - kLo)] && (!c.window || toneWindow(k) == c.window) && d >= 1 && u + 1 < nw) {
                bool blocked = false;
                // ★ Blocked by another carrier's main lobe — but only one within 6 dB of this carrier: anything weaker
                //   sitting there may be this carrier's own sideband taken for a carrier, and must not hide the tone.
                for (auto& o : cars)
                    if (o.get() != &c && o->live && o->bin != c.bin && L[(size_t)o->bin] >= 0.25f * L[(size_t)c.bin] &&
                        (std::abs(o->bin - u) <= 2 || std::abs(o->bin - d) <= 2)) { blocked = true; break; }
                if (!blocked) v = std::min(L[(size_t)u] / Fl[(size_t)u], L[(size_t)d] / Fl[(size_t)d]);
            }
            float& acc = c.acc[(size_t)(k - kLo)];
            acc += a * (v - acc);
            if (acc > bv) { bv = acc; best = k; }
        }
        c.accSeeded = true;
        ++c.toneTicks;
        // ★★ NOT ON THE FIRST LOOK. One second's L of a dozen noise bins easily reaches 1.15 somewhere; after eight
        //    (an ident cycle, and the average's spread down to ≈ 3 %) only a keyed tone stands above 1.10. A tone this
        //    strong (3 dB) is taken at once.
        if (!best || bv < 1.10f || (c.toneTicks < 8 && bv < 2.0f)) return;
        // ★ The tone chosen has faded into the noise (a wrong early pick, or the ident stopped): free to choose again.
        if (c.tone && c.acc[(size_t)(c.tone - kLo)] < 1.06f && std::abs(best - c.tone) > 1) { c.tone = 0; }
        // ★ A second keyed tone in the OTHER window, on a parent with no child yet: a beacon sharing this frequency.
        if (!c.parent && !c.child && c.tone && !c.window) {
            const int w = toneWindow(c.tone);
            int b2 = 0; float v2 = 0;
            for (int k = kLo; k <= kHi; ++k) if (toneWindow(k) != w && c.acc[(size_t)(k - kLo)] > v2) { v2 = c.acc[(size_t)(k - kLo)]; b2 = k; }
            if (b2 && v2 >= 1.20f && c.toneTicks >= 12) {
                c.window = w;
                auto ch = std::make_unique<Car>();
                ch->id = nextId++; ch->bin = c.bin; ch->frac = c.frac; ch->parent = c.id; ch->window = 3 - w; ch->seen = c.seen;
                ch->firstMs = ch->lastMs = now(); ch->vote = IdentVote(cfg.vote);
                ch->live = true; ch->c.reset(R, hopN + 1); ch->acc = c.acc;
                for (int k = kLo; k <= kHi; ++k) if (toneWindow(k) != ch->window) ch->acc[(size_t)(k - kLo)] = 0;
                ch->accSeeded = true;
                c.child = ch->id;
                born.push_back(std::move(ch));
            }
        }
        if (c.tone && std::abs(best - c.tone) <= 1) best = c.tone;
        if (c.tone == best) {
            const int i = best - kLo;
            if (i > 0 && i + 1 < (int)c.acc.size()) {
                const double l0 = c.acc[(size_t)(i - 1)], l1 = c.acc[(size_t)i], l2 = c.acc[(size_t)(i + 1)];
                const double den = l0 - 2 * l1 + l2;
                c.toneFrac = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (l0 - l2) / den)) : 0.0;
            }
            return;
        }
        // ★ Switch only for a clearly better tone: half as much again above the noise.
        if (c.tone && (bv - 1.0f) < 1.5f * (c.acc[(size_t)(c.tone - kLo)] - 1.0f)) return;
        c.tone = best; c.toneFrac = 0;
        c.t.reset(R, hopN + 1);
#ifdef NDB_DEBUG
        std::fprintf(stderr, "[ndb] %6.1f s car %d (%+.1f Hz) TONE %.0f Hz (acc %.2f)\n", now() / 1000.0, c.id, offsetOf(w0 + c.bin), best * binHz, bv);
#endif
    }

    // ── the decode ──
    float wpmOf(float unitHops) const { return unitHops > 0 ? (float)(1.2 / (unitHops * hopSec)) : 0.0f; }
    float hopsOf(double wpm) const { return (float)(1.2 / wpm / hopSec); }

    void decodeCar(Car& c) {
        const int64_t t = now();
        // ★ Is the carrier's own bin steady? (Another beacon's keyed sideband can land on it: 1 kHz raster, 1020 Hz tones.)
        //   If its power swings more than 3 dB between the 10th and 90th percentile over 10 s, the carrier's phase is no
        //   reference: the tone is then detected incoherently (the sidebands' powers summed).
        if (!c.parent) {
            const int w = std::min((int)(hopN + 1 - c.c.from), (int)std::lround(10.0 / hopSec));
            const int ma = std::max(2, (int)std::lround(0.2 / hopSec));   // ★ 0.2 s means: noise averaged out, a dot kept
            if (w > hopsPerTick * 3) {
                scratch.resize((size_t)(w - ma));
                double a = 0;
                for (int i = 0; i < w; ++i) {
                    a += c.c.e[(size_t)((hopN - (uint64_t)i) % (uint64_t)R)];
                    if (i >= ma) a -= c.c.e[(size_t)((hopN - (uint64_t)(i - ma)) % (uint64_t)R)];
                    if (i >= ma) scratch[(size_t)(i - ma)] = (float)a;
                }
                const int nn = (int)scratch.size();
                std::nth_element(scratch.begin(), scratch.begin() + nn / 10, scratch.end());
                const float p10 = scratch[(size_t)(nn / 10)];
                std::nth_element(scratch.begin(), scratch.begin() + nn * 9 / 10, scratch.end());
                const float p90 = scratch[(size_t)(nn * 9 / 10)];
                c.dirty = p90 > 3.0f * std::max(p10, 1e-30f) && !c.keyedCarrier;
#ifdef NDB_DEBUG
                if (std::getenv("NDB_DIRTY")) std::fprintf(stderr, "[ndb] car %d p90/p10 %.2f dirty %d\n", c.id, p90 / std::max(p10, 1e-30f), c.dirty);
#endif
            }
        } else if (Car* p = byId(c.parent)) c.dirty = p->dirty;
        bool toneWords = false;
        if (c.tone) toneWords = decodeChan(c, c.t, false);
        if (toneWords) c.lastToneWordMs = t;
        // ★ The carrier itself (A1A) only when the tone has said nothing for a minute — a steady A2A carrier's QSB
        //   would otherwise feed noise ballots into the vote.
        if (!c.parent && (!c.tone || t - c.lastToneWordMs > 60000)) {
            if (decodeChan(c, c.c, true)) c.keyedCarrier = true;
        }
        // ★★ The fold, for what the repeats one by one cannot read: every 5 s until the ident is confirmed, then every 30 s
        //    (it keeps a weak beacon's "last identified" fresh). The tone if there is one, else the carrier.
        if (c.parent && !c.tone) return;
        // ★ The carrier's own fold (A1A) only for a carrier that has been seen clearly keyed: a steady carrier's QSB, a
        //   neighbour's sideband or a data signal folded long enough reads as letters too, and A1A beacons are rare.
        if (!c.tone && !c.c.keyedStrong) return;
        Chan& fc = c.tone ? c.t : c.c;
        if (t - fc.lastFoldMs >= (c.vote.confirmed() ? 30000 : 5000)) { fc.lastFoldMs = t; fold(c, fc, !c.tone); }
    }

    // ── the fold ───────────────────────────────────────────────────────────────────────────────────────────────────
    /** The repeat period of a channel's envelope, hops (0 = none): the autocorrelation's first strong peak 3.5–30 s,
     *  coarse on a 4-hop decimation, then refined at full resolution on its third multiple (a third of the error). */
    double findPeriod(const Chan& ch, uint64_t start, int n) const {
        const int D = 4, nd = n / D;
        std::vector<float> x((size_t)nd);
        double mean = 0;
        for (int j = 0; j < nd; ++j) {
            float a = 0; for (int k = 0; k < D; ++k) a += ch.e[(size_t)((start + (uint64_t)(j * D + k)) % (uint64_t)R)];
            x[(size_t)j] = a / D; mean += a / D;
        }
        mean /= std::max(1, nd);
        double e0 = 0; for (auto& v : x) { v -= (float)mean; e0 += (double)v * v; }
        if (e0 <= 0) return 0;
        const int lmin = (int)std::lround(3.5 / (hopSec * D)), lmax = std::min((int)std::lround(30.0 / (hopSec * D)), nd / 3);
        if (lmax <= lmin + 2) return 0;
        std::vector<double> r((size_t)(lmax + 2), 0.0);
        double rmax = 0;
        for (int l = lmin - 1; l <= lmax + 1; ++l) {
            double a = 0; for (int j = 0; j + l < nd; ++j) a += (double)x[(size_t)j] * x[(size_t)(j + l)];
            r[(size_t)l] = a / e0 * nd / (nd - l);
            if (l >= lmin && l <= lmax) rmax = std::max(rmax, r[(size_t)l]);
        }
        if (rmax < 0.12) return 0;
        int best = 0;
        for (int l = lmin; l <= lmax; ++l)
            if (r[(size_t)l] >= 0.8 * rmax && r[(size_t)l] >= r[(size_t)(l - 1)] && r[(size_t)l] >= r[(size_t)(l + 1)]) { best = l; break; }
        if (!best) return 0;
        // ★★ Refine by EPOCH FOLDING: of the periods within ±3 hops of the coarse one (steps of a tenth of a hop), the one
        //    whose folded profile has the most variance — the profile is sharpest exactly in step. Nine repeats folded with
        //    a period 0.2 % out smear the oldest by a dot; this pins it to a hundredth of one.
        const double T0 = (double)best * D;
        double bestT = 0, bestV = -1;
        std::vector<double> sum, cnt;
        for (double T = T0 - 3.0; T <= T0 + 3.0; T += 0.1) {
            const int Lf = (int)std::ceil(T);
            sum.assign((size_t)Lf, 0.0); cnt.assign((size_t)Lf, 0.0);
            double ph = 0;
            for (int i = 0; i < n; ++i) {
                const int b = std::min(Lf - 1, (int)ph);
                sum[(size_t)b] += ch.e[(size_t)((start + (uint64_t)i) % (uint64_t)R)]; cnt[(size_t)b] += 1;
                ph += 1.0; if (ph >= T) ph -= T;
            }
            double m = 0, m2 = 0, w = 0;
            for (int b = 0; b < Lf; ++b) if (cnt[(size_t)b] > 0) { const double v = sum[(size_t)b] / cnt[(size_t)b]; m += v * cnt[(size_t)b]; m2 += v * v * cnt[(size_t)b]; w += cnt[(size_t)b]; }
            const double var = w > 0 ? m2 / w - (m / w) * (m / w) : 0;
            if (var > bestV) { bestV = var; bestT = T; }
        }
        return bestT;
    }

    /** Read one folded period (circular): the ident's marks and gaps, starting after the longest gap. "" = unreadable. */
    std::string readFolded(const std::vector<float>& f, double mu, float unitHint, int groups, bool& clean, float* unitOut = nullptr) const {
        clean = false;
        const int Lf = (int)f.size();
        const float u0 = unitHint > 0 ? unitHint : hopsOf(kDefaultWpm);
        const int m = std::max(2, std::min(12, (int)std::lround(0.9f * u0)));
        std::vector<float> s((size_t)Lf);
        for (int i = 0; i < Lf; ++i) { double a = 0; for (int k = 0; k < m; ++k) a += f[(size_t)((i - k + Lf) % Lf)]; s[(size_t)i] = (float)(a / m); }
        double q = 0; int nq = 0;
        for (float v : s) if (v < mu) { q += (v - mu) * (v - mu); ++nq; }
        const double sigma = nq >= Lf / 20 ? std::sqrt(q / nq) : mu / std::sqrt((double)m * groups);
        std::vector<float> srt = s; std::sort(srt.begin(), srt.end());
        double mark = srt[(size_t)(Lf * 97 / 100)];
        int nh = 0;
        for (int it = 0; it < 8; ++it) {
            const double t = mu + 0.5 * (mark - mu); double a = 0; int k = 0;
            for (float v : s) if (v > t) { a += v; ++k; }
            if (!k) break; mark = a / k; nh = k;
        }
        const double hiFrac = (double)nh / Lf;
        if ((mark - mu) < 5.0 * sigma || hiFrac < 0.03 || hiFrac > 0.75) return "";
        const double thr = std::max(mu + 0.5 * (mark - mu), mu + 2.5 * sigma);
        const double hys = std::min(0.1 * thr, 0.4 * sigma + 0.02 * thr);
        // circular runs
        std::vector<std::pair<int, bool>> runs;
        bool on = s[0] > thr;
        for (int i = 0; i < Lf; ++i) {
            const double v = s[(size_t)i];
            const bool nw_ = on ? v > thr - hys : v > thr + hys;
            if (runs.empty() || nw_ != on) { runs.push_back({ 0, nw_ }); on = nw_; }
            ++runs.back().first;
        }
        if (runs.size() >= 2 && runs.front().second == runs.back().second) { runs.front().first += runs.back().first; runs.pop_back(); }
        const int minRun = std::max(1, (int)std::lround(0.5f * u0));
        std::vector<std::pair<int, bool>> rr;
        for (auto& r : runs) {
            if (!rr.empty() && r.first < minRun) { rr.back().first += r.first; continue; }
            if (!rr.empty() && rr.back().second == r.second) { rr.back().first += r.first; continue; }
            rr.push_back(r);
        }
        if (rr.size() >= 2 && rr.front().second == rr.back().second) { rr.front().first += rr.back().first; rr.pop_back(); }
        if (rr.size() < 2) return "";
        // start after the longest gap
        size_t g = 0; int gl = -1;
        for (size_t i = 0; i < rr.size(); ++i) if (!rr[i].second && rr[i].first > gl) { gl = rr[i].first; g = i; }
        std::rotate(rr.begin(), rr.begin() + (long)((g + 1) % rr.size()), rr.end());
        // ★ The dot: from the FOLDED marks themselves when they fall into two clusters (a fold is the cleanest look at the
        //   timing a weak beacon gives), else the hint.
        float u = unitHint;
        {
            std::vector<float> marks; for (auto& r : rr) if (r.second && r.first >= 2) marks.push_back(std::log((float)r.first));
            if (marks.size() >= 2) {
                std::vector<float> ms = marks; std::sort(ms.begin(), ms.end());
                const TwoMeans mk = twoMeans(marks, ms.front(), ms.back());
                const double dot = std::exp(mk.lo), dash = std::exp(mk.hi);
                if (mk.nlo && mk.nhi && dash / dot >= 2.0 && dash / dot <= 4.5) u = (float)(0.5 * (dot + dash / 3.0));
                else if (u <= 0) u = (float)std::exp(ms.front());
            }
            if (u <= 0) return "";
            const float w = wpmOf(u);
            if (w < 4.0f || w > 16.0f) return "";
        }
        std::vector<std::string> wordsOut;
        std::string word; unsigned code = 1; int nel = 0; bool bad = false; float worst = 0;
        auto dev = [&](int len) { return std::min(std::fabs(len - u) / u, std::fabs(len - 3 * u) / (3 * u)); };
        auto flushChar = [&]() {
            if (!nel) return;
            const char* t = (!bad && nel <= 6) ? cw::morseText(code) : nullptr;
            if (t && std::strlen(t) == 1 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) word += t; else word += '?';
            code = 1; nel = 0; bad = false;
        };
        auto flushWord = [&]() { flushChar(); if (!word.empty()) wordsOut.push_back(word); word.clear(); };
        for (size_t i = 0; i < rr.size(); ++i) {
            const int len = rr[i].first;
            if (rr[i].second) {
                if (len > 6.0f * u) { flushWord(); continue; }   // a long tone
                worst = std::max(worst, dev(len));
                if (nel < 7) { code = code * 2 + (len >= 2.0f * u ? 1u : 0u); ++nel; } else bad = true;
            } else {
                if (len >= 5.0f * u) { flushWord(); continue; }
                worst = std::max(worst, dev(len));
                if (len >= 2.0f * u) flushChar();
            }
        }
        flushWord();
        if (wordsOut.empty()) return "";
        for (auto& w : wordsOut) if (w != wordsOut[0]) return "";   // ★ one ident a period (or the same one twice)
        clean = worst <= 0.5f && wordsOut[0].find('?') == std::string::npos;
        if (unitOut) *unitOut = u;
        return wordsOut[0];
    }

    void fold(Car& car, Chan& ch, bool carrierChan) {
        const uint64_t end = hopN + 1;
        const uint64_t start = std::max(ch.from, end > (uint64_t)R ? end - (uint64_t)R : 0);
        const int n = (int)(end - start);
        if (n * hopSec < 30.0) return;
        double T = findPeriod(ch, start, n);
#ifdef NDB_DEBUG
        if (std::getenv("NDB_FORCE_T")) T = std::atof(std::getenv("NDB_FORCE_T"));
        std::fprintf(stderr, "[ndb] period %.2f hops\n", T);
#endif
        // ★ The period is a property of the beacon's keyer: estimates within 4 hops of the last are averaged (the folds
        //   are only as sharp as it is exact), a different one replaces it.
        if (T > 0 && ch.period > 0 && std::fabs(T - ch.period) < 4.0) T = 0.75 * ch.period + 0.25 * T;
        ch.period = T;
        if (T <= 0) return;
        const int K = (int)std::floor(n / T);
        if (K < 3) return;
        const int Lf = (int)std::floor(T);
        const double base = (double)end - K * T;   // ★ the K most recent whole periods
        const double mu = std::max(1e-30, carrierChan ? (double)Fl[(size_t)car.bin]
                                                      : (double)Fl[(size_t)(car.bin + car.tone)] + Fl[(size_t)(car.bin - car.tone)]);
        auto foldRange = [&](int k0, int k1) {
            std::vector<float> f((size_t)Lf, 0.0f);
            for (int k = k0; k < k1; ++k) {
                const uint64_t b = (uint64_t)std::llround(base + k * T);
                for (int i = 0; i < Lf; ++i) f[(size_t)i] += ch.e[(size_t)((b + (uint64_t)i) % (uint64_t)R)];
            }
            for (auto& v : f) v /= (float)(k1 - k0);
            return f;
        };
        bool clean = false; float uf = 0;
        const std::string all = readFolded(foldRange(0, K), mu, ch.unit, K, clean, &uf);
        ch.foldReading = clean ? all : std::string();
        car.vote.setFoldHint(K >= 6 ? ch.foldReading : std::string());
        // ★ A clean fold is the best measure of the speed there is on a weak beacon: the streaming decode adopts it.
        if (clean && uf > 0) { ch.unit = uf; ch.unitSure = true; }
#ifdef NDB_DEBUG
        std::fprintf(stderr, "[ndb] %6.1f s car %d fold T %.2f s K %d all '%s'%s\n", now() / 1000.0, car.id, T * hopSec, K, all.c_str(), clean ? "" : " (not clean)");
#endif
        // ★ A whole-fold ballot needs six repeats or more in it, and none of the last ballot's data.
        if (clean && K >= 6 && (uint64_t)base >= ch.foldBallotEnd) {
            ch.foldBallotEnd = end;
            if (car.vote.addFoldWindow(all, now()) && owner->onIdent) { owner->onIdent(view(car, true)); return; }
        }
        if (K < 6 || !clean) return;
        // ★ Disjoint groups of at least three repeats each: three groups from nine repeats, else two.
        const int G = K >= 9 ? 3 : 2;
        std::vector<std::string> g;
        for (int j = 0; j < G; ++j) {
            bool c2 = false;
            const std::string w = readFolded(foldRange(j * K / G, (j + 1) * K / G), mu, ch.unit, K / G, c2);
            g.push_back(c2 ? w : std::string());
        }
#ifdef NDB_DEBUG
        std::fprintf(stderr, "[ndb] %6.1f s car %d FOLD T %.2f s K %d all '%s' groups '%s' '%s' '%s'\n", now() / 1000.0, car.id, T * hopSec, K,
                     all.c_str(), g[0].c_str(), g[1].c_str(), G > 2 ? g[2].c_str() : "-");
#endif
        if (car.vote.addFolded(g, now()) && owner->onIdent) owner->onIdent(view(car, true));
    }

    /** One channel: slice its history into marks and gaps, learn the dot, read the complete words not yet read. */
    bool decodeChan(Car& car, Chan& ch, bool carrierChan) {
        // ★ Valid hops are [from, hopN] — record() writes hop hopN after the increment — so `end` is one past the newest.
        const uint64_t end = hopN + 1;
        const uint64_t span = (uint64_t)std::ceil(kStreamSec / hopSec);
        const uint64_t start = std::max(ch.from, end > span ? end - span : 0);
        if (end <= start + (uint64_t)hopsPerTick * 4) return false;
        const int n = (int)(end - start);
        // ★ Until the dot is known, smooth for a FAST ident (12 WPM): a slow one's marks survive a short average, but a
        //   fast one's gaps are filled by a long one, its marks merge — and the speed learnt from them is wrong for good.
        const float u0 = ch.unit > 0 ? ch.unit : hopsOf(12.0);
        // ★ A moving average ≈ one dot long: the matched filter for a rectangular dot, and at half height a smoothed mark
        //   (and gap) keeps its true length.
        const int m = std::max(2, std::min(12, (int)std::lround(0.9f * u0)));
        std::vector<float> s((size_t)n);
        double acc = 0;
        for (int i = 0; i < n; ++i) {
            acc += ch.e[(size_t)((start + (uint64_t)i) % (uint64_t)R)];
            if (i >= m) acc -= ch.e[(size_t)((start + (uint64_t)(i - m)) % (uint64_t)R)];
            s[(size_t)i] = (float)(acc / std::min(i + 1, m));
        }
        // ★★ IS IT KEYED AT ALL? The noise level is KNOWN — the floor under the channel's bins — so it is not guessed
        //    from the envelope (a two-means split of pure noise always finds "two clusters"). The noise's own spread is
        //    measured below that level (marks are never below it); the marks' level is the mean of what stands above the
        //    midpoint, iterated; keyed = the marks stand five spreads clear of the noise.
        double mu = std::max(1e-30, carrierChan ? (double)Fl[(size_t)car.bin]
                                                : (double)Fl[(size_t)(car.bin + car.tone)] + Fl[(size_t)(car.bin - car.tone)]);
        std::vector<float> srt = s;
        // ★ …unless something else sits in the channel's bins (a neighbour's carrier under one of our sidebands): then the
        //   pauses never reach the floor, and their own level (the 10th percentile) is the reference instead.
        std::nth_element(srt.begin(), srt.begin() + (long)(n / 10), srt.end());
        mu = std::max(mu, (double)srt[(size_t)(n / 10)]);
        double q = 0; int nq = 0;
        for (float v : s) if (v < mu) { q += (v - mu) * (v - mu); ++nq; }
        const double sigma = nq >= n / 20 ? std::sqrt(q / nq) : mu / std::sqrt((double)m);
        std::nth_element(srt.begin(), srt.begin() + (long)(n * 97 / 100), srt.end());
        double mark = srt[(size_t)(n * 97 / 100)];
        int nh = 0;
        for (int it = 0; it < 8; ++it) {
            const double t = mu + 0.5 * (mark - mu);
            double a = 0; int k = 0;
            for (float v : s) if (v > t) { a += v; ++k; }
            if (!k) break;
            mark = a / k; nh = k;
        }
        const double hiFrac = (double)nh / n;
        const double clear = (mark - mu) / std::max(1e-30, sigma);
        ch.keyed = clear >= 5.0 && hiFrac >= 0.03 && hiFrac <= (carrierChan ? 0.92 : 0.75);
#ifdef NDB_DEBUG
        std::fprintf(stderr, "[ndb] %6.1f s car %d %s n %d mark/noise %.1f dB clear %.1f sigma hi %.2f unit %.1f -> %s\n", now() / 1000.0, car.id,
                     carrierChan ? "carrier" : "tone", n, 10 * std::log10(std::max(1e-9, mark / mu)), clear, hiFrac, ch.unit, ch.keyed ? "keyed" : "-");
#endif
        if (!ch.keyed) return false;
        if (clear >= 8.0) ch.keyedStrong = true;
        ch.noiseLin = (float)mu; ch.markLin = (float)mark;
        // ★ The threshold half-way between noise and marks in POWER — where a smoothed rectangular mark crosses at its true
        //   length (the CW skimmer's lesson: the dB midpoint reads every mark long on a strong signal) — but never within
        //   2.5 spreads of the noise.
        const double thr = std::max(mu + 0.5 * (mark - mu), mu + 2.5 * sigma);
        const double hys = std::min(0.1 * thr, 0.4 * sigma + 0.02 * thr);
        // ── runs (+mark / −gap, hops), with hysteresis, then a debounce ──
        struct Run { int len; bool on; uint64_t at; };
        std::vector<Run> runs;
        bool on = s[0] > thr;
        for (int i = 0; i < n; ++i) {
            const double v = s[(size_t)i];
            const bool now_ = on ? v > thr - hys : v > thr + hys;
            if (runs.empty() || now_ != on) { runs.push_back({ 0, now_, start + (uint64_t)i }); on = now_; }
            ++runs.back().len;
        }
        // ★ Machine keying has no element shorter than a dot: anything under half a dot is noise, absorbed.
        const int minRun = std::max(1, (int)std::lround(0.5f * u0));
        std::vector<Run> rr;
        for (const Run& r : runs) {
            if (!rr.empty() && r.len < minRun) { rr.back().len += r.len; continue; }
            if (!rr.empty() && rr.back().on == r.on) { rr.back().len += r.len; continue; }
            rr.push_back(r);
        }
        // ── the dot, from the marks (machine keying: one dot, one dash of three) ──
        std::vector<float> marks;
        for (size_t i = 1; i + 1 < rr.size(); ++i) if (rr[i].on && rr[i].len >= 2 && rr[i].len < 14 * u0) marks.push_back(std::log((float)rr[i].len));
        if (marks.size() >= 4) {
            std::vector<float> ms = marks; std::sort(ms.begin(), ms.end());
            const TwoMeans mk = twoMeans(marks, ms.front(), ms.back());
            const double dot = std::exp(mk.lo), dash = std::exp(mk.hi);
            double unit = 0;
            const bool two = mk.nlo && mk.nhi && dash / dot >= 2.0 && dash / dot <= 4.5;
            if (two) unit = 0.5 * (dot + dash / 3.0);
            else {
                double mean = 0; for (float x : marks) mean += std::exp(x);
                mean /= marks.size();
                unit = std::fabs(std::log(mean / u0)) < std::fabs(std::log(mean / (3 * u0))) ? mean : mean / 3.0;
            }
            const float w = wpmOf((float)unit);
            // ★ NDB idents are keyed at 5–12 WPM (ICAO: about 7). A reading outside 4–16 is noise breaking the marks up (or
            //   joining them), and following it would make the next decode worse — the CW skimmer's speed trap.
            // ★ …and only from a signal clear enough that its marks are whole (8 spreads); a weak one's speed comes from its fold.
            if (w >= 4.0f && w <= 16.0f && (two || !ch.unitSure) && (clear >= 8.0 || !ch.unitSure)) {
                ch.unit = ch.unit > 0 ? (float)(0.7 * ch.unit + 0.3 * unit) : (float)unit;
                if (two && clear >= 8.0) ch.unitSure = true;
            }
        }
        if (ch.unit <= 0) return false;
        const float u = ch.unit;
        // ── the complete words after decodedTo ──
        bool any = false;
        unsigned code = 1; int nel = 0; bool bad = false;
        std::string word; bool inWord = false; uint64_t lastGapStart = 0;
        // ★ How far the word's worst element is from machine timing (a dot, a dash of three, gaps of one and three) —
        //   a word read through noise has marks broken or joined somewhere; only a CLEAN word is a ballot.
        float worst = 0;
        auto dev = [&](int len) { const float a = std::fabs(len - u) / u, b = std::fabs(len - 3 * u) / (3 * u); return std::min(a, b); };
        auto flushChar = [&]() {
            if (nel == 0) return;
            const char* t = (!bad && nel <= 6) ? cw::morseText(code) : nullptr;
            if (t && std::strlen(t) == 1 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) word += t;
            else word += '?';
            code = 1; nel = 0; bad = false;
        };
        auto flushWord = [&](uint64_t at) {
            flushChar();
            if (!word.empty()) {
                if (word.size() <= 6) emitWord(car, word, worst <= 0.5f && ch.unitSure);
                any = true;
            }
            word.clear(); inWord = false; worst = 0;
            ch.decodedTo = at;
        };
        for (size_t i = 0; i < rr.size(); ++i) {
            const Run& r = rr[i];
            const uint64_t rEnd = r.at + (uint64_t)r.len;
            if (rEnd <= ch.decodedTo) continue;
            const bool last = i + 1 == rr.size();
            if (r.on) {
                if (r.at < ch.decodedTo || i == 0) continue;   // ★ cut by the history's start, or read already
                if (last) break;                                // ★ still keying — the word is not over
                if (r.len > 6.0f * u) { word.clear(); code = 1; nel = 0; bad = false; inWord = false; worst = 0; ch.decodedTo = rEnd; continue; }   // a long tone
                inWord = true;
                worst = std::max(worst, dev(r.len));
                const bool dash = r.len >= 2.0f * u;
                if (nel < 7) { code = code * 2 + (dash ? 1u : 0u); ++nel; } else bad = true;
            } else {
                if (!inWord) { if (last && r.len >= 5.0f * u) ch.decodedTo = std::max(ch.decodedTo, rEnd - (uint64_t)std::lround(2 * u)); continue; }
                if (r.len >= 5.0f * u) { flushWord(r.at + (uint64_t)std::lround(2 * u)); lastGapStart = r.at; continue; }
                if (last) break;                                // ★ a gap still growing: char or word, not known yet
                worst = std::max(worst, dev(r.len));
                if (r.len >= 2.0f * u) flushChar();
            }
        }
        (void)lastGapStart;
        return any;
    }

    void emitWord(Car& car, const std::string& w, bool clean) {
        ++words;
        const int64_t t = now();
        car.copy += (car.copy.empty() ? "" : " ") + w;
        if (car.copy.size() > 80) car.copy.erase(0, car.copy.size() - 80);
        // ★ Ballot first, so a reading that CONFIRMS the ident is already "high"
        const bool confirmedNow = clean && w.find('?') == std::string::npos && car.vote.add(w, t);
        if (owner->onCopy) {
            Copy c; c.id = car.id; c.offsetHz = offsetOf(w0 + car.bin + car.frac); c.text = w; c.utcMs = t;
            c.q = w.find('?') != std::string::npos ? "poor" : (!car.vote.ident().empty() && w == car.vote.ident()) ? "high" : clean ? "medium" : "low";
            owner->onCopy(c);
        }
        if (confirmedNow && owner->onIdent) owner->onIdent(view(car, true));
    }

    BeaconView view(const Car& c, bool active) const {
        BeaconView v;
        v.id = c.id; v.offsetHz = offsetOf(w0 + c.bin + c.frac); v.snrDb = c.snr;
        v.levelDb = (float)(10.0 * std::log10(std::max(1e-30, (double)c.lvl / (0.25 * (double)N * N))));
        v.toneHz = c.tone ? (float)((c.tone + c.toneFrac) * binHz) : 0.0f;
        const Chan& ch = c.keyedCarrier && !c.tone ? c.c : c.t;
        if (ch.markLin > 0 && ch.noiseLin > 0) v.toneSnrDb = (float)(10.0 * std::log10(std::max(1e-3, (double)ch.markLin / ch.noiseLin - 1.0)));
        v.keyedCarrier = c.keyedCarrier && !c.tone;
        v.wpm = wpmOf(c.tone ? c.t.unit : c.c.unit);
        if (v.wpm == 0) v.wpm = wpmOf(c.c.unit);
        v.ident = c.vote.ident(); v.votes = c.vote.votes(); v.ballots = c.vote.ballots();
        v.reading = c.vote.confirmed() ? std::string() : c.vote.leading();
        if (v.reading.empty() && !c.vote.confirmed()) v.reading = (c.tone ? c.t : c.c).foldReading;
        v.others = c.vote.others();
        v.copy = c.copy;
        v.firstMs = c.firstMs; v.lastMs = c.lastMs;
        v.identMs = c.vote.confirmedMs(); v.lastIdentMs = c.vote.lastMs();
        v.active = active;
        return v;
    }
};

Skimmer::Skimmer(const Config& c) : d_(new Impl) { d_->owner = this; d_->init(c); }
Skimmer::~Skimmer() = default;
void Skimmer::feedIq(const std::complex<float>* x, int n) { if (n > 0) d_->feed(x, n); }
std::vector<BeaconView> Skimmer::beacons(bool includeGone) const {
    std::vector<BeaconView> out;
    for (auto& c : d_->cars) if (c->live) out.push_back(d_->view(*c, true));
    std::sort(out.begin(), out.end(), [](const BeaconView& a, const BeaconView& b) { return a.snrDb > b.snrDb; });
    if (includeGone) for (auto& g : d_->gone) out.push_back(g);
    return out;
}
void Skimmer::setStartUtcMs(int64_t ms) { d_->startMs = ms; }
int64_t Skimmer::nowMs() const { return d_->now(); }
int Skimmer::fftSize() const { return d_->N; }
int Skimmer::hop() const { return d_->H; }
double Skimmer::binHzActual() const { return d_->binHz; }
double Skimmer::hopSec() const { return d_->hopSec; }
double Skimmer::searchedLoHz() const { return d_->offsetOf(d_->s0); }
double Skimmer::searchedHiHz() const { return d_->offsetOf(d_->s1); }
uint64_t Skimmer::hops() const { return d_->hopN; }
int Skimmer::carriers() const { int n = 0; for (auto& c : d_->cars) n += c->live; return n; }
int Skimmer::identified() const { int n = 0; for (auto& c : d_->cars) n += c->live && c->vote.confirmed(); return n; }
uint64_t Skimmer::wordsRead() const { return d_->words; }
double Skimmer::noiseCalibration() const {
    if (!d_->floorReady) return 0;
    double s = 0; int n = 0;
    for (int b = d_->s0 - d_->w0; b < d_->s1 - d_->w0; ++b) { s += d_->L[(size_t)b] / d_->Fl[(size_t)b]; ++n; }
    return n ? s / n : 0;
}

} // namespace ndb
} // namespace vibe
