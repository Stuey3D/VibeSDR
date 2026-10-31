// decoders/textmodes.cpp — MFSK16/32, OLIVIA, CONTESTIA and THOR receivers (and their test transmitter). See textmodes.h
// for the sources each detail was taken from. V12, 2026-10-31.
#include "textmodes.h"
#include "mfsk_varicode.h"
#include "digimodes/q65_simd.h"
#include "../vibedsp/third_party/pffft/pffft.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <cstdlib>

namespace vibe {
namespace {

// ── the sub-modes ────────────────────────────────────────────────────────────────────────────────────────────────────
enum Kind { kMfsk, kThor, kOlivia, kContestia };
struct Sub {
    const char* id; const char* label; Kind kind;
    int M;          // tones
    int sps;        // samples a symbol at `rate`
    int rate;       // 8000 or 11025
    int stride;     // half-bins (rate / 2·sps) from one tone to the next: 2 = spacing equals the baud, 4 = double
    bool common;    // in AUTO
};
const Sub kSubs[] = {
    {"mfsk16", "MFSK16", kMfsk, 16, 512, 8000, 2, true},
    {"mfsk32", "MFSK32", kMfsk, 16, 256, 8000, 2, true},
    {"thor4",  "THOR 4",  kThor, 18, 2048, 8000, 4, true},
    {"thor5",  "THOR 5",  kThor, 18, 2048, 11025, 4, true},
    {"thor8",  "THOR 8",  kThor, 18, 1024, 8000, 4, true},
    {"thor11", "THOR 11", kThor, 18, 1024, 11025, 2, true},
    {"thor16", "THOR 16", kThor, 18, 512, 8000, 2, true},
    {"thor22", "THOR 22", kThor, 18, 512, 11025, 2, true},
    // Olivia/Contestia: sps = 8000·M/BW; tones one FFT bin pair apart (spacing = BW/M = 8000/sps)
    {"olivia-4-125",   "OLIVIA 4/125",   kOlivia, 4, 256, 8000, 2, true},
    {"olivia-4-250",   "OLIVIA 4/250",   kOlivia, 4, 128, 8000, 2, false},
    {"olivia-8-250",   "OLIVIA 8/250",   kOlivia, 8, 256, 8000, 2, true},
    {"olivia-8-500",   "OLIVIA 8/500",   kOlivia, 8, 128, 8000, 2, true},
    {"olivia-16-500",  "OLIVIA 16/500",  kOlivia, 16, 256, 8000, 2, true},
    {"olivia-16-1000", "OLIVIA 16/1000", kOlivia, 16, 128, 8000, 2, true},
    {"olivia-32-1000", "OLIVIA 32/1000", kOlivia, 32, 256, 8000, 2, true},
    {"olivia-64-2000", "OLIVIA 64/2000", kOlivia, 64, 256, 8000, 2, false},
    {"contestia-4-125",   "CONTESTIA 4/125",   kContestia, 4, 256, 8000, 2, false},
    {"contestia-4-250",   "CONTESTIA 4/250",   kContestia, 4, 128, 8000, 2, false},
    {"contestia-8-250",   "CONTESTIA 8/250",   kContestia, 8, 256, 8000, 2, true},
    {"contestia-8-500",   "CONTESTIA 8/500",   kContestia, 8, 128, 8000, 2, false},
    {"contestia-16-500",  "CONTESTIA 16/500",  kContestia, 16, 256, 8000, 2, true},
    {"contestia-16-1000", "CONTESTIA 16/1000", kContestia, 16, 128, 8000, 2, false},
    {"contestia-32-1000", "CONTESTIA 32/1000", kContestia, 32, 256, 8000, 2, true},
    {"contestia-64-2000", "CONTESTIA 64/2000", kContestia, 64, 256, 8000, 2, false},
};
TextModeDecoder::Family familyOfKind(Kind k) {
    return k == kMfsk ? TextModeDecoder::Family::Mfsk : k == kThor ? TextModeDecoder::Family::Thor : TextModeDecoder::Family::Olivia;
}
const Sub* subById(const std::string& id) { for (const Sub& s : kSubs) if (id == s.id) return &s; return nullptr; }
const char* groupOfKind(int k) { return k == kMfsk ? "MFSK" : k == kThor ? "THOR" : k == kOlivia ? "OLIVIA" : "CONTESTIA"; }
const char* groupId(int k) { return k == kMfsk ? "mfsk" : k == kThor ? "thor" : k == kOlivia ? "olivia" : "contestia"; }
/** "any-olivia" → kOlivia; −1 for anything else */
int kindOfAny(const std::string& id) {
    for (int k : {kMfsk, kOlivia, kContestia, kThor}) if (id == std::string("any-") + groupId(k)) return k;
    return -1;
}

constexpr uint64_t kOliviaScr = 0xE257E6D0291574ECull;
constexpr uint64_t kContestiaScr = 0xEDB88320ull;
constexpr float kAudioLo = 200.f, kAudioHi = 2900.f;   // where a signal is looked for (the listener's audio)

inline int parity(uint32_t v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (int)(v & 1); }
inline int grayOf(int v) { return v ^ (v >> 1); }
inline int grayInv(int g) { int b = 0; while (g) { b ^= g; g >>= 1; } return b; }
/** the published Olivia symbol shape, x over [−π, π] across 2·sps samples */
inline double oliviaShape(double x) {
    return 1.0 + 1.1913785723 * std::cos(x) - 0.0793018558 * std::cos(2 * x) - 0.2171442026 * std::cos(3 * x) - 0.0014526076 * std::cos(4 * x);
}

// ── varicode ─────────────────────────────────────────────────────────────────────────────────────────────────────────
struct VaricodeTable {
    int16_t ch[1 << 13];   // index (1 << len) | code, len ≤ 12  → character + 1, 0 = none
    VaricodeTable() {
        std::memset(ch, 0, sizeof ch);
        for (int c = 0; c < 256; ++c) {
            const char* s = kMfskVaricode[c]; int len = (int)std::strlen(s), v = 0;
            for (int i = 0; i < len; ++i) v = (v << 1) | (s[i] == '1');
            ch[(1 << len) | v] = (int16_t)(c + 1);
        }
    }
};
const VaricodeTable& vtab() { static const VaricodeTable t; return t; }

/** bits in, characters out: a code ends where a 1 follows two or more zeros ("001") */
struct VaricodeRx {
    uint32_t code = 0; int len = 0, zeros = 0;
    int push(int bit) {   // → character, or −1
        int out = -1;
        if (bit && zeros >= 2 && len > 0) {
            if (len <= 12) { const int c = vtab().ch[(1 << len) | (int)code]; if (c) out = c - 1; }
            code = 0; len = 0; zeros = 0;
        }
        if (len == 0 && !bit) return out;   // idle zeros between characters
        if (len < 31) { code = (code << 1) | (uint32_t)bit; }
        ++len; zeros = bit ? 0 : zeros + 1;
        return out;
    }
};

// ── K = 7, R = 1/2 Viterbi (the MFSK FEC coder: taps O1+O3+O4+O6+O7, O1+O2+O3+O4+O7; O1 the newest bit) ─────────────
constexpr int kPoly1 = 0x6D, kPoly2 = 0x4F;
struct Viterbi7 {
    static constexpr int D = 48;      // traceback depth (≈ 7 K)
    float m[64]; uint64_t dec[D]; int head = 0, steps = 0;
    float gain = 0, span = 0;         // EMAs of the best path's gain a step and of the most it could gain
    float quality() const { return span > 1e-6f ? gain / span : 0.f; }
    static const uint8_t* outs() {     // outs[reg] = 2·o1 + o2 for the 7-bit register (newest in bit 0)
        static uint8_t t[128]; static bool init = false;
        if (!init) { for (int r = 0; r < 128; ++r) t[r] = (uint8_t)(parity(r & kPoly1) * 2 + parity(r & kPoly2)); init = true; }
        return t;
    }
    Viterbi7() { reset(); }
    void reset() { for (int s = 0; s < 64; ++s) m[s] = s ? -1e4f : 0.f; head = 0; steps = 0; std::memset(dec, 0, sizeof dec); }
    /** one coded pair (soft, + = 1); returns the decoded bit D steps back, or −1 while filling */
    int push(float a, float b) {
        const uint8_t* o = outs();
        float nm[64]; uint64_t d = 0;
        const float br[4] = {-a - b, -a + b, a - b, a + b};   // index 2·o1 + o2
        for (int ns = 0; ns < 64; ++ns) {
            const int bit = ns & 1, s0 = ns >> 1, s1 = (ns >> 1) | 32;
            const float m0 = m[s0] + br[o[((s0 << 1) | bit) & 127]];
            const float m1 = m[s1] + br[o[((s1 << 1) | bit) & 127]];
            if (m1 > m0) { nm[ns] = m1; d |= 1ull << ns; } else nm[ns] = m0;
        }
        float best = nm[0]; int bs = 0;
        for (int s = 1; s < 64; ++s) if (nm[s] > best) { best = nm[s]; bs = s; }
        float prevBest = m[0]; for (int s = 1; s < 64; ++s) prevBest = std::max(prevBest, m[s]);
        gain += (1.f / 48) * ((best - prevBest) - gain);
        span += (1.f / 48) * ((std::fabs(a) + std::fabs(b)) - span);
        for (int s = 0; s < 64; ++s) m[s] = nm[s] - best;
        dec[head] = d; if (++head == D) head = 0; ++steps;
        if (steps < D) return -1;
        int s = bs, h = head == 0 ? D - 1 : head - 1;
        for (int k = 0; k < D - 1; ++k) { const int x = (int)((dec[h] >> s) & 1); s = (s >> 1) | (x << 5); h = h == 0 ? D - 1 : h - 1; }
        return s & 1;   // the input bit that entered state s, D − 1 steps before the newest
    }
};

// ── front end: 8 kHz and 11.025 kHz from the listener's rate ─────────────────────────────────────────────────────────
struct Resampler {
    int q = 1, qn = 0; float acc = 0; float inv = 1;   // box average by q
    double step = 1.0, inc = 1.0, frac = 0; float prev = 0; bool interp = false;
    std::vector<float> out;
    void init(double inRate, double target) {
        q = std::max(1, (int)std::floor(inRate / target + 1e-9));
        while (q > 1 && inRate / q < target * 0.999) --q;
        inv = 1.f / (float)q;
        const double mid = inRate / q;
        interp = std::fabs(mid - target) > 1e-6;
        step = target / mid;   // outputs per mid sample
        inc = mid / target;
    }
    void feed(const float* x, int n) {
        out.clear();
        for (int i = 0; i < n; ++i) {
            acc += x[i];
            if (++qn < q) continue;
            const float v = acc * inv; acc = 0; qn = 0;
            if (!interp) { out.push_back(v); continue; }
            // linear interpolation: outputs fall at mid-sample positions frac … stepping 1/step
            while (frac < 1.0) { out.push_back(prev + (float)frac * (v - prev)); frac += inc; }
            frac -= 1.0; prev = v;
        }
    }
};

// ── spectra: |FFT|² of 2·sps every sps/4 ─────────────────────────────────────────────────────────────────────────────
struct Stream {
    int sps, rate, N, hop; bool shaped;
    std::vector<float> ring, win; int wp = 0, sinceHop = 0, filled = 0; int64_t frame = 0;
    PFFFT_Setup* setup = nullptr; float* fin = nullptr; float* fout = nullptr; float* work = nullptr;
    std::vector<float> P;
    std::vector<std::function<void(int64_t, const float*)>> sinks;
    Stream(int sps_, int rate_, bool shaped_) : sps(sps_), rate(rate_), N(2 * sps_), hop(sps_ / 4), shaped(shaped_) {
        ring.assign(N, 0.f); win.assign(N, 0.f); P.assign(N / 2, 0.f);
        for (int i = 0; i < N; ++i) {
            if (shaped) win[i] = (float)(oliviaShape(M_PI * (2.0 * (i + 0.5) / N - 1.0)) / 2.4);
            else win[i] = i >= N - sps ? 1.f : 0.f;   // the last symbol, zero-padded to twice its length (half-spacing bins)
        }
        setup = pffft_new_setup(N, PFFFT_REAL);
        fin = (float*)pffft_aligned_malloc(N * sizeof(float));
        fout = (float*)pffft_aligned_malloc(N * sizeof(float));
        work = (float*)pffft_aligned_malloc(N * sizeof(float));
    }
    ~Stream() { pffft_destroy_setup(setup); pffft_aligned_free(fin); pffft_aligned_free(fout); pffft_aligned_free(work); }
    double binHz() const { return (double)rate / N; }
    void feed(const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            ring[wp] = x[i]; wp = (wp + 1) == N ? 0 : wp + 1;
            if (filled < N) ++filled;
            if (++sinceHop < hop) continue;
            sinceHop = 0;
            if (filled < N) { ++frame; continue; }
            // oldest first: ring[wp … N) then ring[0 … wp)
            const int a = N - wp;
            std::memcpy(fin, ring.data() + wp, a * sizeof(float));
            std::memcpy(fin + a, ring.data(), wp * sizeof(float));
            for (int k = 0; k < N; k += 4) q65v::st(fin + k, q65v::mul(q65v::ld(fin + k), q65v::ld(win.data() + k)));
            pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
            // fout: [r0, r(N/2), r1, i1, r2, i2, …] — powers four bins a lane (bin 0 fixed after)
            for (int k = 0; k < N / 2; k += 4) {
                q65v::V re, im; q65v::ld2(fout + 2 * k, re, im);
                q65v::st(P.data() + k, q65v::add(q65v::mul(re, re), q65v::mul(im, im)));
            }
            P[0] = fout[0] * fout[0];
            for (auto& s : sinks) s(frame, P.data());
            ++frame;
        }
    }
};

// ── the signal finder: where (base bin, quarter-symbol phase) a tone set sits ─────────────────────────────────────────
struct Finder {
    int M, s, lo, hi, nb;          // bases lo … lo + nb − 1 (half-bins)
    std::vector<float> own[4], v, g, h, seq;
    /** ★ ONE SET OF SCORES FOR TWINS: OLIVIA and CONTESTIA of the same tones and baud read the same spectra with the
     *  same finder, so the second of them reads the first's scores instead of computing them again (`score` points at
     *  the owner's; its update() does nothing). What each does with them — its place, its bans, its hunt — stays its
     *  own. The owner is registered first, so its update() has run when the twin's frame() reads them. */
    std::vector<float>* score = own; bool shared = false;
    void shareScoresOf(Finder& o) { score = o.own; shared = true; }
    int curB = -1, curP = 0, L_ = 0;
    /** ★ THE HUNT: a place where this sub-mode could not lock is set aside for a while (symbols), so a second signal in
     *  the passband gets its turn — measured: OLIVIA 16/500 beside a stronger MFSK16 never decoded before this. */
    /** ★ …AND A PLACE SET ASIDE AGAIN SOON AFTER IS SET ASIDE FOR LONGER (×2, ×4): a steady other signal is not worth
     *  re-checking every time its ban runs out — each re-check is huntLimit() symbols blind to the passband (measured on
     *  the web test's clip: OLIVIA 16/500 beside a stronger MFSK16, the finder back on the MFSK16 at the start of every
     *  other over). Forgotten when twice the ban has passed after it without another strike. */
    std::vector<int> ban, mem; std::vector<uint8_t> strikes;
    void setAside(int around, int halfWidth, int forSymbols) {
        const int i0 = std::max(0, around - halfWidth), i1 = std::min(nb - 1, around + halfWidth);
        int st = 0; for (int i = i0; i <= i1; ++i) st = std::max<int>(st, strikes[i]);
        st = std::min(3, st + 1);
        const int len = forSymbols << (st - 1);
        for (int i = i0; i <= i1; ++i) { ban[i] = len; mem[i] = 3 * len; strikes[i] = (uint8_t)st; }
        curB = -1;
    }
    void tickBans() {
        for (int i = 0; i < nb; ++i) { if (ban[i] > 0) --ban[i]; if (mem[i] > 0 && --mem[i] == 0) strikes[i] = 0; }
    }
    float alpha;
    void init(int M_, int stride, double binHz, int nBins) {
        M = M_; s = stride;
        lo = std::max(stride, (int)std::ceil(kAudioLo / binHz));
        const int top = std::min(nBins - 1 - stride, (int)std::floor(kAudioHi / binHz));
        hi = top - stride * (M - 1);            // last base whose top tone and the bin above it fit
        nb = std::max(0, hi - lo + 1);
        nb = (nb + 3) & ~3;                     // whole lanes (the extra bases read zeros past the band: never chosen)
        for (auto& sc : own) sc.assign(nb, 0.f);
        score = own; shared = false;
        v.assign(nb, 0.f); ban.assign(nb, 0); mem.assign(nb, 0); strikes.assign(nb, 0);
        alpha = 1.f / 12;
        L_ = (nb + s * M + s) / s + M + 2;
        seq.assign((size_t)L_ * s, 0.f); g.assign(seq.size(), 0.f); h.assign(seq.size(), 0.f);
    }
    /** one frame (phase p): the score of every base — the strongest of the M tones less the stronger neighbour outside */
    void update(const float* P, int nBins, int p) {
        if (nb == 0 || shared) return;
        double sum = 0; const int a = lo, b = std::min(nBins - 1, hi + s * M);
        for (int k = a; k <= b; ++k) sum += P[k];
        const float inv = (float)((b - a + 1) / (sum + 1e-20));   // one divide a frame: powers in units of the band's mean
        // sliding maximum of M tones, stride s (van Herk / Gil-Werman), one residue at a time. ★ No divide or modulo
        //   anywhere in here: ARMv7 has no hardware divide, and the first build spent 60 % of its Pi 2 time in
        //   __aeabi_idivmod (qemu count) on `i % s`, `j % M` and the like.
        const int L = L_;
        for (int r = 0; r < s; ++r) {
            float* q = seq.data() + (size_t)r * L; float* G = g.data() + (size_t)r * L; float* H = h.data() + (size_t)r * L;
            for (int j = 0, k = lo + r; j < L; ++j, k += s) q[j] = k < nBins ? P[k] : 0.f;
            for (int j = 0, c = 0; j < L; ++j) { G[j] = c == 0 ? q[j] : std::max(G[j - 1], q[j]); if (++c == M) c = 0; }
            int c = (L - 1) - ((L - 1) / M) * M;   // (L − 1) mod M, once a frame
            for (int j = L - 1; j >= 0; --j) { H[j] = (c == M - 1 || j == L - 1) ? q[j] : std::max(H[j + 1], q[j]); if (--c < 0) c = M - 1; }
            for (int j = 0, i = r; i < nb; ++j, i += s) {
                const float wmax = std::max(H[j], G[std::min(L - 1, j + M - 1)]);
                const int kb = lo + i - s, ka = lo + i + s * M;
                const float out = std::max(kb >= 0 && kb < nBins ? P[kb] : 0.f, ka < nBins ? P[ka] : 0.f);
                v[i] = (lo + i > hi) ? -1e3f : wmax - out;
            }
        }
        // EMA, four bases a lane
        const q65v::V A = q65v::set1(alpha), I = q65v::set1(inv);
        float* sc = score[p].data();
        for (int i = 0; i < nb; i += 4) {
            const q65v::V cur = q65v::ld(sc + i);
            q65v::st(sc + i, q65v::add(cur, q65v::mul(A, q65v::sub(q65v::mul(q65v::ld(v.data() + i), I), cur))));
        }
    }
    /** the best (base, phase), held unless another beats it by `margin` (15 % hunting; 80 % while a receiver is locked —
     *  enough for a drift of half a bin, never for the noise of an idle carrier, which moved MFSK32's base a tone) */
    /** `near` ≥ 0: only bases within `near` half-bins of the current one (a locked receiver: one tone, for drift), and
     *  only to a place scoring `minScore`. ★ A lock follows ITS signal: free to choose, a locked OLIVIA 16/500 left its
     *  own over for a stronger MFSK16 beside it, read the MFSK16's tones under the lock and printed "OLI6" for "OLIV"
     *  (measured on the web test's clip, 2026-10-31). */
    void choose(float margin, int near = -1, float minScore = -1e9f) {
        float best = -1e9f; int bb = -1, bp = 0;
        const int i0 = near >= 0 && curB >= 0 ? std::max(0, curB - near) : 0, i1 = near >= 0 && curB >= 0 ? std::min(nb - 1, curB + near) : nb - 1;
        for (int p = 0; p < 4; ++p) for (int i = i0; i <= i1; ++i) if (!ban[i] && score[p][i] > best) { best = score[p][i]; bb = i; bp = p; }
        if (bb < 0 || (curB >= 0 && best < minScore)) return;
        if (curB < 0 || best > margin * score[curP][curB] || score[curP][curB] <= 0) { curB = bb; curP = bp; }
    }
    float current() const { return curB < 0 ? 0.f : score[curP][curB]; }
};

// ── receivers ────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Rx {
    const Sub* sub; Stream* st = nullptr; Finder fd; int64_t lastIdx = -1; std::string text;
    explicit Rx(const Sub* s) : sub(s) {}
    virtual ~Rx() {}
    virtual void symbol(const float* T, int gap) = 0;
    virtual float quality() const = 0;
    virtual bool locked() const = 0;
    virtual int toneShift() const { return 0; }
    virtual bool holdFinder() const { return false; }   // an idle carrier says nothing about where the tone set sits
    virtual int huntLimit() const { return 96; }           // symbols without a lock before the place is set aside
    /** no sign of this mode at all at the place (a weak signal of it shows a rising contrast / block quality first —
     *  setting that aside cost decodes: THOR16 at −6 dB CER 4 % → 38 %) */
    virtual bool hopeless() const { return true; }
    /** how fast a hopeless place ages, in quarters (4 = set aside after huntLimit() symbols) */
    virtual int huntWeight() const { return 4; }
    /** the finder's base has moved by `delta` half-bins (what was measured at the old place may not hold at the new one) */
    virtual void moved(int delta) { (void)delta; }
    int huntAge = 0;   // quarter-symbols a signal place has shown no sign of the mode
    float hz() const { return fd.curB < 0 ? 0.f : (float)((fd.lo + fd.curB + fd.s * toneShift() + 0.5 * fd.s * (sub->M - 1)) * st->binHz()); }
    /** (1 − α)^frames: the part of the finder's score that is still its zero start (≈ 0 once it is warm) */
    float warmD = 1.f;
    void frame(int64_t f, const float* P) {
        const int nBins = st->N / 2, p = (int)(f & 3);
        if (warmD > 0.f) { warmD *= 1.f - fd.alpha; if (warmD < 1e-6f) warmD = 0.f; }
        fd.update(P, nBins, p);
        if (p != 3) return;   // choose once a symbol
        // ★ a ban counts HUNTING symbols only: run down during a lock, it expired in the over's tail and the next over
        //   began with the finder back on the other signal (measured, the web test's clip)
        if (!locked()) fd.tickBans();
        const int wasB = fd.curB;   // (before the hunt: a place set aside is a move like any other — see moved())
        const float wasScore = fd.current();
        // ★ only a place with a SIGNAL on it (finder score ≥ 4.5; noise reads 2–3) is set aside — set aside on noise, the
        //   bans covered where the signal later came up (measured: every sub-mode failed). ★ The age is of ONE STRETCH of
        //   signal at one place: it starts again when the place goes quiet, or the finder moves off the tone set or to a
        //   much stronger view. Carried over, an over's idle tail and the gap after it aged the place so far that the
        //   NEXT over there was set aside 1.3 s in, before its code could show (MFSK16 on the web test's clip, 7 in 40).
        if (locked() || fd.curB < 0 || fd.current() < 4.5f) huntAge = 0;
        else if (hopeless() && (huntAge += huntWeight()) > 4 * huntLimit()) { fd.setAside(fd.curB, fd.s * sub->M, 2 * huntLimit()); huntAge = 0; }
        // ★ A LOCKED receiver's finder stays within a tone of its signal (see Finder::choose) and moves only to a SIGNAL
        //   (score ≥ 4.5; noise reads 2–3): in the over's tail it wandered on the noise, and the next over found the
        //   lock a few tones off its own signal (lost "CQ D", the web test's clip)
        if (!holdFinder()) { if (locked()) fd.choose(1.8f, fd.s, 4.5f); else fd.choose(1.15f); }
        if (wasB >= 0 && fd.curB >= 0 && fd.curB != wasB) {
            // a new stretch: another place, or a much better view of this one (measured: aged at the edge of an MFSK16
            // signal while its centre was set aside, then set aside 12 symbols after reaching the centre)
            if (std::abs(fd.curB - wasB) >= fd.s * sub->M || fd.current() > 1.5f * wasScore) huntAge = 0;
            moved(fd.curB - wasB);
        }
        // the symbol of the chosen phase, if this frame is it — handled on the frame of that phase below
    }
    /** ★★ A PHASE MOVE ACROSS THE WRAP IS A QUARTER OF A SYMBOL, NOT A WHOLE ONE (2026-10-31, eighth run; found while
     *  hunting the web test's lost "E" — see presRing for that one). The symbol index was (frame − phase) / 4, so a
     *  finder moving its phase 3 → 0 (a quarter later) counted a symbol that never came — one erasure INSERTED into the
     *  stream — and 0 → 3 (a quarter earlier) skipped one — a symbol DELETED. Either shifts every later symbol against the de-interleaver until it has passed through
     *  (≈ 30 symbols of wrong bits, 4–7 characters), and an OLIVIA block's chips against their alignment. Measured on
     *  the web test's clip (OLIVIA 16/500 + MFSK16) looped from 400 start offsets: a wrong MFSK16 join ("'E G4XYZ",
     *  "c G4XYZ", "ÐZ MFSK") — the phase settles most while a receiver joins an over, which is why it showed
     *  as a JOIN fault. Moves of 1 or 2 within 0 … 3 were always right; a move of 3 is now read as the quarter it is. */
    int symP = -1; int64_t idxAdj = 0;
    void maybeSymbol(int64_t f, const float* P) {
        if (fd.curB < 0 || (int)(f & 3) != fd.curP) return;
        if (symP >= 0) { const int dP = fd.curP - symP; if (dP == 3) ++idxAdj; else if (dP == -3) --idxAdj; }
        symP = fd.curP;
        const int64_t idx = ((f - fd.curP) >> 2) + idxAdj;
        if (idx <= lastIdx) return;
        const int gap = lastIdx < 0 ? 0 : (int)std::min<int64_t>(64, idx - lastIdx - 1);
        lastIdx = idx;
        float T[66];   // the tone below, the M tones, the tone above
        const int b0 = fd.lo + fd.curB, nBins = st->N / 2;
        for (int k = -1; k <= sub->M; ++k) { const int bin = b0 + fd.s * k; T[k + 1] = bin >= 0 && bin < nBins ? P[bin] : 0.f; }
        symbol(T, gap);
    }
    /** log-likelihoods of each tone, from powers (noise = the mean of the others) — x_k = P_k / N0, capped */
    static void toneLogs(const float* T, int M, float* x) {
        float mx = 0; double sum = 0; int im = 0;
        for (int k = 0; k < M; ++k) { sum += T[k]; if (T[k] > mx) { mx = T[k]; im = k; } }
        (void)im;
        const double n0 = std::max(1e-20, (sum - mx) / (M - 1));
        const float inv = (float)(1.0 / n0);
        for (int k = 0; k < M; ++k) x[k] = std::min(30.f, T[k] * inv);
    }
};

/** bit LLRs (+ = 1) from tone logs: log Σ_{bit=1} e^x − log Σ_{bit=0} e^x — one exp a tone (the largest taken out), two
 *  logs a bit; halved and capped at ±12 (a burst of noise must not outvote the code). out[i] = bit of weight 2^i. */
void bitLlrs(const float* x, const int* val, int n, int nbits, float* out) {
    float mx = -1e9f; for (int k = 0; k < n; ++k) mx = std::max(mx, x[k]);
    float e[64]; for (int k = 0; k < n; ++k) e[k] = std::exp(x[k] - mx);
    for (int i = 0; i < nbits; ++i) {
        double s1 = 1e-30, s0 = 1e-30;
        for (int k = 0; k < n; ++k) { if ((val[k] >> i) & 1) s1 += e[k]; else s0 += e[k]; }
        const float l = (float)(std::log(s1) - std::log(s0));
        out[i] = std::max(-12.f, std::min(12.f, l * 0.5f));
    }
}

void appendChar(std::string& out, int c) {
    if (c == 13 || c == 10) { if (out.empty() || out.back() != '\n') out += '\n'; return; }
    if (c == 8) { if (!out.empty() && out.back() != '\n') out.pop_back(); return; }
    if (c >= 32 && c < 127) { out += (char)c; return; }
    if (c >= 160 && c < 256) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
}

/** MFSK16/32 and THOR: tones → 4 soft bits → the diagonal de-interleaver → two Viterbis (dibit 1 first, or one late) */
struct MfskRx : Rx {
    float ring[32][4]; int n = 0; int prevTone = -1;
    Viterbi7 vit[2]; VaricodeRx vr[2]; float carry = 0;
    bool lock = false; int lockAge = 0;
    explicit MfskRx(const Sub* s) : Rx(s) { std::memset(ring, 0, sizeof ring); }
    float repeat = 0.5f;   // EMA of "this tone again" (MFSK) / an IFK+ step never sent (THOR): a slower signal read too fast
    float split = 0.5f;    // EMA of the second strongest tone over the strongest: a faster signal read too slowly splits
    uint8_t recent[32] = {}; int nRecent = 0;   // the last 32 hard tones: a real signal uses nearly all M of them
    int distinct() const {
        uint32_t m = 0; const int n = std::min(nRecent, 32);
        for (int i = 0; i < n; ++i) m |= 1u << recent[i];
        return __builtin_popcount(m);
    }
    /** ★ SIGNAL PRESENT, AS IT WAS WHEN THE BITS LEFT: the strongest tone over the mean of the others (≈ 3.3 on noise,
     *  ≈ 19 for MFSK16 at −10 dB in 3 kHz), averaged over 8 symbols, checked at both ends of the 24–54 symbols a bit spends in
     *  the de-interleaver and the Viterbi — so the few characters the code makes of the noise after an over are not shown. */
    float presence = 0; float presRing[64] = {};
    // the oldest and the newest symbol in the bit — and now: the traceback starts from the newest path, so noise that
    // has just begun can still bend the last few bits (measured: "KuåDAR p5W" after an MFSK32 over)
    bool presentThen() const { return presRing[(n + 64 - 54) & 63] > 6.f && presRing[(n + 64 - 24) & 63] > 6.f && presence > 6.f; }
    /** ★ STARTED MID-OVER (the listener opened the decoder on a signal already sending): the coder is in no known state
     *  and the Viterbi's first bits are guesses, so a character whose first bit came through the first 24 symbols is
     *  not shown (measured: " rMFSK" for "Z MFSK", "nK" for "SK" — 4 starts in 40 on the web test's clip). Cleared once
     *  the place is quiet: an over that begins after quiet starts the coder from idle, as the Viterbi assumes.
     *  Dead ends (2026-10-31): the presence check applied to the character's first bit too (lost MFSK32's first "C"
     *  at −6 dB while the presence rose); an unbiased Viterbi start (6 starts in 40 still wrong, and THOR — which
     *  starts from idle — lost decodes); the same rule on every finder move off the tone set (the finder arrives at an
     *  over's start that way: MFSK16 at −6 dB 20/20 → 13/20). */
    int joinN = 0;
    bool joinedThen(int began) const { return joinN >= 0 && began - 54 < joinN + 24; }
    int codeStart[2] = {-1, -1};   // the symbol at which each decoder's code in progress took its first bit
    int lockK = 0; int zeroRun = 0;   // decoded zeros in a row on the locked pairing: an idle (THOR's idle is +2 steps, not one tone)
    bool holdFinder() const override { return lock && repeat > 0.4f; }
    bool hopeless() const override { return contrast() < 0.04f; }
    /** ★ an IDLE CARRIER (one tone held) is how an over starts: it ages at a quarter of the rate, so a long preamble is
     *  not set aside before the text (measured: the Wikimedia MFSK16 slogan idles ≈ 8 s; set aside after 6 s, its first
     *  sentence was lost). A birdie still gives way, after four times as long. */
    int huntWeight() const override { return repeat > 0.5f ? 1 : 4; }
    std::vector<int> pre;  // characters the best decoder read in the seconds before the lock (≤ 8)
    /** ★ THE FRAMING CONTRAST. On anything that is not this mode, the best path of a K = 7 code agrees with ≈ 89 % of
     *  the bits whichever way the dibits are paired (≈ 0.75 here) — so a high agreement proves nothing. A real signal
     *  agrees on ONE pairing and not the other: the difference is the evidence (measured: 0.24 on MFSK32 at −3 dB,
     *  0.00–0.03 on OLIVIA 16/500, which has MFSK32's very tones and baud). */
    float contrast() const { return std::fabs(vit[0].quality() - vit[1].quality()); }
    float quality() const override { return contrast() - 0.5f * repeat - 0.25f * split; }
    bool locked() const override { return lock; }
    /** decoder 0 pairs coded bits (0,1), (2,3) …; decoder 1 pairs (1,2), (3,4) … — one of them is the coder's framing */
    void coded(float c) {
        if (nCoded > 0) emit((nCoded & 1) ? 0 : 1, carry, c);
        carry = c; ++nCoded;
    }
    int64_t nCoded = 0;
    void emit(int k, float a, float b) {
        const int bit = vit[k].push(a, b);
        if (bit < 0) return;
        if (k == lockK) zeroRun = bit ? 0 : zeroRun + 1;
        const bool starts = bit && (vr[k].len == 0 || vr[k].zeros >= 2);   // this bit begins a code (VaricodeRx::push)
        const int began = codeStart[k];
        const int c = vr[k].push(bit);
        if (starts) codeStart[k] = n;
        // ★ the pairing is chosen when the lock is taken and kept while it holds: in an idle carrier both pairings read
        //   the all-zero codeword perfectly, and switching then printed the other decoder's leftovers ("TaTaT")
        const int best = lock ? lockK : (vit[1].quality() > vit[0].quality() ? 1 : 0);
        if (c < 0 || k != best) return;
        if (lock) { if (presentThen() && !joinedThen(began)) appendChar(text, c); }
        // ★ (no pre-lock buffer: tried, it printed the code's start-up guesses — " tnj" before "CQ" even at +20 dB)
    }
    void symbol(const float* Tg, int gap) override {
        const float* T = Tg + 1;   // the M tones (the guards either side are for OLIVIA's hypotheses)
        float x[18];
        const int M = sub->M;
        float soft[4];
        if (sub->kind == kMfsk) {
            toneLogs(T, M, x);
            static int val[16]; static bool init = false;
            if (!init) { for (int k = 0; k < 16; ++k) val[k] = grayOf(k); init = true; }
            float w[4]; bitLlrs(x, val, 16, 4, w);
            for (int i = 0; i < 4; ++i) soft[i] = w[3 - i];   // the first-arriving bit is the heaviest
            int cur = 0; for (int k = 1; k < M; ++k) if (T[k] > T[cur]) cur = k;
            repeat += (1.f / 16) * ((cur == prevTone ? 1.f : 0.f) - repeat);
            prevTone = cur;
        } else {
            // IFK+: the value is the step from the previous tone, less 2, modulo 18 (16 and 17 are never sent)
            toneLogs(T, M, x);
            int cur = 0; for (int k = 1; k < M; ++k) if (T[k] > T[cur]) cur = k;
            if (prevTone < 0 || gap) { prevTone = cur; std::memset(soft, 0, sizeof soft); }
            else {
                float y[16]; int val[16];
                for (int v = 0; v < 16; ++v) { y[v] = x[(prevTone + 2 + v) % 18]; val[v] = v; }
                float w[4]; bitLlrs(y, val, 16, 4, w);
                for (int i = 0; i < 4; ++i) soft[i] = w[3 - i];
                repeat += (1.f / 16) * ((((cur - prevTone - 2 + 36) % 18) >= 16 ? 1.f : 0.f) - repeat);
                prevTone = cur;
            }
        }
        {   int cur = 0; for (int k = 1; k < M; ++k) if (T[k] > T[cur]) cur = k;
            recent[nRecent++ & 31] = (uint8_t)cur;
            float xm = 0; for (int k = 0; k < M; ++k) xm = std::max(xm, x[k]);
            presence += 0.125f * (xm - presence);
            // the place quiet (the hunt's test): what follows is an over's start. ★ Judged against the finder's WARM-UP
            //   (2026-10-31, eighth run): its score is an average that starts from zero, so on the first symbols even a
            //   strong over read "quiet" — the join rule was switched off at once on every receiver opened mid-over, and
            //   the first characters it shows were the Viterbi's start-up guesses ("'E G4XYZ", "c G4XYZ", "cST PSE K":
            //   9 in 1200 joins of the web test's clip).
            if (fd.current() < 4.5f * (1.f - warmD)) joinN = -1;
            // ★★ EVERY SYMBOL'S SLOT, the skipped ones too (2026-10-31, eighth run — THE WEB TEST'S LOST "E"): a phase move
            //   to an earlier quarter (3 → 2) skips a symbol, pushed below as an erasure, and its slot here kept the
            //   presence of 64 symbols before — the quiet before the over. 54 symbols later presentThen() read that stale
            //   slot for the "E" of "DE" and hid it: "CQ CQ D G4XYZ", reproduced exactly from the server's own decoder
            //   audio (dumped from a failing run of test-web-textmodes). Section 12 saw the same stale slots (THOR16, a
            //   128-slot ring) and kept the 64; filling them is the fix.
            for (int g = 0; g <= gap; ++g) presRing[(n + g) & 63] = presence;
        }
        {   float m1 = 0, m2 = 0;
            for (int k = 0; k < M; ++k) { if (T[k] > m1) { m2 = m1; m1 = T[k]; } else if (T[k] > m2) m2 = T[k]; }
            split += (1.f / 16) * ((m1 > 0 ? m2 / m1 : 1.f) - split);   // one divide a symbol
        }
        for (int g = 0; g < gap; ++g) push(nullptr);
        push(soft);
        // ★ the lock is TAKEN on all three (a code that agrees, tones that change, one tone a symbol) and HELD on the code
        //   alone — the idle carrier at the end of an over repeats one tone, and the last words are still in the Viterbi
        // ★ …and not before the Viterbi has given its first bit (D steps): its quality is an average of the first few
        //   steps only, and read 2 steps in it said 1.00 / 0.00 — a "contrast" of 1 — on a place one tone off an MFSK16
        //   over the receiver had just joined; the lock held through the over's idle tail and printed its garbage
        //   ("teî tttzA", measured once the phase fix above had moved the timing of that join). D steps is n ≈ 55: an
        //   over that begins after quiet still locks before its first character leaves the pipeline (n ≈ 70).
        if (!lock && vit[0].steps >= Viterbi7::D && contrast() > 0.12f && repeat < 0.15f && split < 0.3f && nRecent >= 32 && distinct() >= (M * 3) / 4) {
            lock = true; lockAge = 0; lockK = vit[1].quality() > vit[0].quality() ? 1 : 0;
        } else if (lock && ((repeat < 0.3f && zeroRun < 24 && contrast() < 0.05f) || (repeat < 0.25f && distinct() < M / 2))) lock = false;
        // ★ (an idle carrier — one tone held, repeat → 1 — keeps the lock: the over's last words are still in the pipeline)   // tones that vary over too few
        ++lockAge;
    }
    void push(const float* soft) {
        const int t = n & 31;
        for (int i = 0; i < 4; ++i) ring[t][i] = soft ? soft[i] : 0.f;
        ++n;
        if (n < 31) return;
        // in symbol s = t − 30: bit i left 10·i symbols after it
        const int s = n - 31;
        for (int i = 0; i < 4; ++i) coded(ring[(s + 10 * i) & 31][i]);
    }
};

/** OLIVIA and CONTESTIA: tones → b soft bits → 64- (32-) symbol blocks → descramble → Walsh transform → characters */
struct OliviaRx : Rx {
    int b, K, cbits; uint64_t scr; int rotStep;
    /** ★ THREE TONE HYPOTHESES: the finder's base, one tone below and one above. With 32 or 64 tones the edge tones are
     *  rarely sent, so the finder alone is often a tone out (measured: CONTESTIA 64/2000 locked 2 times in 7 at +8 dB);
     *  the Walsh transform tells the right one at once. */
    struct Hyp { std::vector<float> ring; std::vector<float> E; std::vector<std::vector<int>> past; };   // past: K × (2 blocks of b codes, q)
    Hyp hyp[3];
    int64_t n = 0; int pos = 0;
    int lockH = 1, lockA = -1; bool lock = false; int candH = -1, candA = -1, candB = -1; float candScore = 0;
    int val[64];
    std::vector<uint64_t> scrJ;  // per character: the scrambler rotated
    explicit OliviaRx(const Sub* s) : Rx(s) {
        b = 0; while ((1 << b) < s->M) ++b;
        const bool con = s->kind == kContestia;
        K = con ? 32 : 64; cbits = con ? 6 : 7; scr = con ? kContestiaScr : kOliviaScr; rotStep = con ? 5 : 13;
        for (Hyp& h : hyp) { h.ring.assign((size_t)K * b, 0.f); h.E.assign(K, 0.f); h.past.assign(K, {}); }
        for (int k = 0; k < s->M; ++k) val[k] = grayInv(k);
        const uint64_t mask = K == 64 ? ~0ull : ((1ull << K) - 1);
        for (int j = 0; j < b; ++j) {
            const int r = (rotStep * j) % K;
            scrJ.push_back(r ? (((scr >> r) | (scr << (K - r))) & mask) : scr);
        }
    }
    float quality() const override { return lockA < 0 ? 0.f : hyp[lockH].E[lockA]; }
    bool locked() const override { return lock; }
    int toneShift() const override { return lockH - 1; }
    int huntLimit() const override { return 4 * K; }   // four blocks: two to lock, and room for the blocks before
    /** ★ STRONG EVIDENCE MUST NOT TRAVEL. The block qualities are kept per hypothesis and alignment; carried to another
     *  signal they let a lock be taken there on the old one's blocks (measured: an OLIVIA idle block read at 1500 Hz,
     *  the finder then moved to an MFSK16 signal at 766 Hz and locked there on the decaying 0.58 — "[OLIVIA 16/500 ·
     *  766 Hz]", the web test's clip). So a move off the tone set (no hypothesis reads a tone of the old place) caps
     *  every quality at the hold threshold — below the lock's, so a lock elsewhere must be earned by blocks read there —
     *  and forgets the characters kept for a lock. What a weak signal's finder carries on its hops to the noise and
     *  back is kept, as before. Dead ends (2026-10-31), each measured at −12 dB, 20 trials: wiping the qualities on
     *  every such move (OLIVIA 4/125 13/20 → 3/20); wiping on any move of three tones (OLIVIA 64/2000 at −6 dB 13 → 11,
     *  its finder slides inside the 2 kHz signal); renaming the hypotheses on a whole-tone move (the same); parking
     *  each place's qualities and taking them back on a return (CONTESTIA 8/250 8 → 6). */
    int64_t farMoveN = -1;   // symbols pushed when the finder last moved off the tone set (see push: a block across it)
    void moved(int delta) override {
        if (std::abs(delta) <= fd.s * (sub->M + 1)) return;
        farMoveN = n;
        if (lock) return;
        const float cap = K == 64 ? 0.085f : 0.13f;   // = thrOff in push()
        for (Hyp& h : hyp) { for (float& e : h.E) e = std::min(e, cap); for (auto& p : h.past) p.clear(); }
        candH = candA = -1;
    }
    bool hopeless() const override {
        float e = 0; for (const Hyp& h : hyp) for (float v : h.E) e = std::max(e, v);
        return e < (K == 64 ? 0.075f : 0.12f);
    }
    static void fht(float* v, int n) {
        for (int h = 1; h < n; h <<= 1) {
            if (h >= 4) {   // four butterflies a lane (q65v); the first two stages within a lane, scalar — the same adds
                for (int i = 0; i < n; i += 2 * h)
                    for (int j = i; j < i + h; j += 4) {
                        const q65v::V a = q65v::ld(v + j), c = q65v::ld(v + j + h);
                        q65v::st(v + j, q65v::add(a, c)); q65v::st(v + j + h, q65v::sub(a, c));
                    }
            } else {
                for (int i = 0; i < n; i += 2 * h)
                    for (int j = i; j < i + h; ++j) { const float a = v[j], c = v[j + h]; v[j] = a + c; v[j + h] = a - c; }
            }
        }
    }
    /** T holds M + 2 powers: the tone below the finder's base, the M tones, the tone above */
    /** ★★ TONE COVERAGE (2026-10-31, found with fldigi as a black box): a scrambled Olivia/Contestia signal uses its
     *  tones evenly, so a receiver for MORE tones at the same spacing and symbol rate sees only a subset — and its Walsh
     *  blocks still read about half the characters of the narrower signal, well enough to lock: fldigi's CONTESTIA 4/125
     *  through AUTO printed "[CONTESTIA 16/500] EFKYGH*BIJ!CKLR@…". So a lock also needs the strongest tone of the last
     *  max(2K, 4M) symbols to have visited ¾ of the tone set (all but one of 4) — and a lock held is dropped only below
     *  half (a 32-tone signal's 64-symbol window dipped under ¾ mid-message: measured, so 4M and the hysteresis). On noise the strongest tone wanders
     *  everywhere, so this never costs a weak signal anything; only a narrower signal fails it. */
    std::vector<uint8_t> top; int topAt = 0;
    int coverNeed() const { return sub->M <= 4 ? sub->M - 1 : (3 * sub->M) / 4; }
    int coverHold() const { return sub->M <= 4 ? sub->M - 2 : sub->M / 2; }
    int coverage() const {
        uint64_t seen = 0;
        for (uint8_t t : top) if (t < 64) seen |= 1ull << t;
        return __builtin_popcountll(seen);
    }
    void noteTop(const float* T) {
        if (top.empty()) top.assign((size_t)std::max(2 * K, 4 * sub->M), 0xFF);
        const float* x = T + (lock ? lockH : 1);
        int im = 0; for (int i = 1; i < sub->M; ++i) if (x[i] > x[im]) im = i;
        top[(size_t)topAt] = (uint8_t)im; if (++topAt == (int)top.size()) topAt = 0;
    }
    void symbol(const float* T, int gap) override {
        for (int g = 0; g < gap && g < K; ++g) push(nullptr);
        noteTop(T);
        float soft[3][8];
        for (int h = 0; h < 3; ++h) {
            float x[64];
            toneLogs(T + h, sub->M, x);
            bitLlrs(x, val, sub->M, b, soft[h]);
        }
        push(soft);
    }
    /** the block that ends with the newest symbol, read under one hypothesis: its characters and its quality */
    float block(const Hyp& hy, int* code) {
        alignas(16) float v[64];
        float q = 0;
        // the oldest symbol of the block sits at ring slot `pos` (the newest was written just before it); no modulo:
        // slots and bit positions are counters that wrap
        for (int j = 0; j < b; ++j) {
            int slot = pos, bit = j;
            for (int k = 0; k < K; ++k) {
                const float c = hy.ring[(size_t)slot * b + bit];
                v[k] = ((scrJ[j] >> k) & 1) ? c : -c;     // −(descrambled chip): a 1 is −1
                if (++slot == K) slot = 0;
                if (++bit == b) bit = 0;
            }
            fht(v, K);
            float mx = 0, sum = 0; int im = 0;
            for (int k = 0; k < K; ++k) { const float a = std::fabs(v[k]); sum += a; if (a > mx) { mx = a; im = k; } }
            q += sum > 0 ? mx / sum : 0;   // one divide a character a symbol
            const int sg = v[im] < 0;
            code[j] = im | ((sg ^ parity((uint32_t)im)) << (cbits - 1));
        }
        return q / b;
    }
    void push(const float (*soft)[8]) {
        for (int h = 0; h < 3; ++h) {
            float* r = hyp[h].ring.data() + (size_t)pos * b;
            for (int i = 0; i < b; ++i) r[i] = soft ? soft[h][i] : 0.f;
        }
        ++n;
        if (++pos == K) pos = 0;   // now the oldest slot
        if (n < K) return;
        // ★ (a gate that skipped the transforms while the finder saw nothing was tried — 2.2× cheaper on noise — and
        //   REMOVED: it cost decodes at the edge (OLIVIA 16/500 at −10 dB 10/10 → 8/10). Never decodes for speed.)
        const int a = pos;         // the alignment: where the block ends, modulo K
        int code[3][8]; float q[3];
        // noise reads ≈ 0.057 (64 chips) / 0.10 (32): the best of 64 / 32 alignments × 3 hypotheses must not reach thrOn
        const float thrOn = K == 64 ? 0.12f : 0.17f, thrOff = K == 64 ? 0.085f : 0.13f;
        // ★★ A BLOCK ACROSS A MOVE OFF THE TONE SET NAMES NO CANDIDATE AND IS NOT KEPT TO BE SHOWN (2026-10-31, eighth
        //   run). moved() caps what was measured before the move, but the K blocks read in the K symbols AFTER it still
        //   hold symbols read at the old place: the first of them is nearly all old signal. Measured on the web test's
        //   clip (400 start offsets): (1) an OLIVIA's last block before its finder left for the MFSK16 read 0.50 and
        //   became the lock candidate; one noise-level block later the lock was taken ON THE MFSK16 ("[OLIVIA 16/500 ·
        //   797 Hz]", the MFSK16's text lost while it was the winner); (2) a block read half at the old place was kept
        //   for the lock and printed "OLIvIA K" / "iAK" (one bit wrong a character). Its quality still counts as before:
        //   capping it at the hold threshold too (tried first) cost a weak signal whose finder hops to the noise and back
        //   (OLIVIA 4/125 at −12 dB 101/200 → 90/200) — its evidence must survive the trip (section 12).
        const bool straddles = n - K < farMoveN;
        for (int h = 0; h < 3; ++h) { q[h] = block(hyp[h], code[h]); hyp[h].E[a] += 0.5f * (q[h] - hyp[h].E[a]); }
        // lock: the best (hypothesis, alignment)
        int bh = 1, ba = 0;
        for (int h = 0; h < 3; ++h) for (int i = 0; i < K; ++i) if (hyp[h].E[i] > hyp[bh].E[ba]) { bh = h; ba = i; }
        // ★ a lock is taken only when the same (hypothesis, alignment) is still the best one block later — a block
        //   half in silence or a neighbouring hypothesis can top the table once (measured: "=Q00GDG0D" at +10 dB)
        bool take = false;
        // ★ …and not when the finder has since slid off to the noise: the over ended between the two blocks and the lock
        //   was taken on the old evidence 250 Hz above its signal ("[OLIVIA 16/500 · 1750 Hz]", nothing after; measured)
        if (!lock && a == ba && hyp[bh].E[ba] > thrOn) {
            // ★ …nor when the place has gone QUIET since the candidate block (its finder score under half what it was):
            //   the over's last block (0.92) and one noise block after it still average over the threshold, and the lock
            //   was taken on the noise 200 Hz above the signal ("[OLIVIA 16/500 · 1703 Hz]", nothing printed but the
            //   MFSK16 beside it lost the box; measured 2 in 1200 joins of the web test's clip with noise added). A weak
            //   signal's score stays about level from block to block, so this asks nothing of it.
            const bool stayed = (std::abs(fd.curB - candB) < fd.s * sub->M / 2 && fd.current() > 0.5f * candScore) || fd.current() >= 4.5f;
            if (candH == bh && candA == ba && stayed) take = true;
            else if (straddles) candH = candA = -1;   // ★ (see straddles above)
            else { candH = bh; candA = ba; candB = fd.curB; candScore = fd.current(); }
        }
        if (take && coverage() < coverNeed()) take = false;   // ★ a narrower signal of the same spacing (see coverage)
        if (take) {
            lock = true; lockH = bh; lockA = ba; candA = -1;
            // ★ the block or two read at this alignment before the lock was sure (a QSO's first words: the call)
            const std::vector<int>& pb = hyp[bh].past[ba];
            for (int c : pb) emitCode(c);   // the blocks before this one (this one is shown below)
        }
        else if (lock && (hyp[lockH].E[lockA] < thrOff || coverage() < coverHold())) lock = false;
        if (!lock) { lockH = bh; lockA = ba; }
        // remember this block's characters at this alignment (the last two blocks, those good enough to show)
        for (int h = 0; h < 3; ++h) {
            std::vector<int>& pb = hyp[h].past[a];
            if (q[h] <= thrOff || straddles) { pb.clear(); continue; }   // ★ (see straddles above)   // (thrOn here lost OLIVIA 16/500 at −10 dB 10/10 → 8/10; thrOff can show one start-up character)
            pb.insert(pb.end(), code[h], code[h] + b);
            if ((int)pb.size() > 3 * b) pb.erase(pb.begin(), pb.begin() + b);
        }
        if (lock && a == lockA && q[lockH] > thrOff) for (int j = 0; j < b; ++j) emitCode(code[lockH][j]);
    }
    void emitCode(int c) {
        // ★ CONTESTIA (confirmed against fldigi run as a black box, 2026-10-31): 59 = space, 60 = newline, 0 = idle,
        //   the rest ASCII − 32 (fldigi sends lower case as upper and [ \ ] ^ _ ` { | } ~ as '?')
        if (sub->kind == kContestia) { if (c == 0) return; c = c == 59 ? ' ' : c == 60 ? '\n' : c + 32; }
        else if (c == 0) return;
        appendChar(text, c);
    }
};

} // namespace

// ── the family decoder ───────────────────────────────────────────────────────────────────────────────────────────────
struct TextModeDecoder::Impl {
    Family fam; double inRate;
    Resampler rs8, rs11; bool need8 = false, need11 = false;
    std::vector<std::unique_ptr<Stream>> streams;
    std::vector<std::unique_ptr<Rx>> rx;
    Rx* winner = nullptr; std::string lastStatus; int state = 0;
    TextModeDecoder* self = nullptr;
    Stream* streamFor(int sps, int rate, bool shaped) {
        for (auto& s : streams) if (s->sps == sps && s->rate == rate && s->shaped == shaped) return s.get();
        streams.emplace_back(new Stream(sps, rate, shaped));
        return streams.back().get();
    }
    void add(const Sub* s) {
        std::unique_ptr<Rx> r;
        if (s->kind == kMfsk || s->kind == kThor) r.reset(new MfskRx(s)); else r.reset(new OliviaRx(s));
        const bool shaped = s->kind == kOlivia || s->kind == kContestia;
        r->st = streamFor(s->sps, s->rate, shaped);
        r->fd.init(s->M, s->stride, r->st->binHz(), r->st->N / 2);
        // an earlier receiver on the same spectra with the same tone set (OLIVIA ↔ CONTESTIA): read its scores
        for (auto& o : rx) if (o->st == r->st && o->sub->M == s->M && o->sub->stride == s->stride && !o->fd.shared) { r->fd.shareScoresOf(o->fd); break; }
        Rx* p = r.get();
        r->st->sinks.push_back([p](int64_t f, const float* P) { p->frame(f, P); p->maybeSymbol(f, P); });
        if (s->rate == 8000) need8 = true; else need11 = true;
        rx.push_back(std::move(r));
    }
    void after() {
        // the winner: the locked receiver with the best quality; only its text is shown
        Rx* best = nullptr;
        for (auto& r : rx) if (r->locked() && (!best || r->quality() > best->quality())) best = r.get();
        if (best != winner) {
            winner = best;
            if (best && self->onText) {
                char h[64]; std::snprintf(h, sizeof h, "\n[%s \xc2\xb7 %.0f Hz]\n", best->sub->label, best->hz());
                self->onText(h);
            }
        }
        for (auto& r : rx) {
            if (r.get() == winner && !r->text.empty() && self->onText) self->onText(r->text);
            r->text.clear();
        }
        static const bool dbg = std::getenv("VIBE_TM_DEBUG") != nullptr;   // the bench's trace (tools/textmode-bench)
        if (dbg) {
            static int tick = 0;
            if (++tick % 50 == 0) { for (auto& r : rx) { const MfskRx* m = dynamic_cast<const MfskRx*>(r.get());
                std::fprintf(stderr, "%s %.2f%s[f%.2f]", r->sub->id, r->quality(), r->locked() ? "*" : "", r->fd.current());
                if (const OliviaRx* o = dynamic_cast<const OliviaRx*>(r.get())) { float e = o->quality(); std::fprintf(stderr, "(E%.2f b%d p%d hz%.0f)", e, r->fd.curB, r->fd.curP, r->hz()); }
                if (m) std::fprintf(stderr, "(r%.2f s%.2f d%d v%.2f/%.2f)", m->repeat, m->split, m->distinct(), m->vit[0].quality(), m->vit[1].quality()); std::fprintf(stderr, "  "); } std::fprintf(stderr, "\n"); }
        }
        const int st = winner ? 1 : 0;
        if (st != state) { state = st; if (self->onState) self->onState(st); }
        char js[160];
        std::snprintf(js, sizeof js, "{\"detected\":\"%s\",\"hz\":%.0f}", winner ? winner->sub->label : "",
                      winner ? std::round(winner->hz() / 10) * 10 : 0.0);
        if (js != lastStatus) { lastStatus = js; if (self->onStatus) self->onStatus(js); }
    }
};

TextModeDecoder::TextModeDecoder(Family f, double inRate, const std::string& mode) : d_(new Impl) {
    d_->fam = f; d_->inRate = inRate; d_->self = this;
    const Sub* one = subById(mode);
    const int anyK = f == Family::All ? kindOfAny(mode) : -1;
    for (const Sub& s : kSubs) {
        // ★ All: every family's common sub-modes in ONE Impl, so the ones with the same symbol length and rate share a
        //   stream (one FFT) and the same tone set shares a finder (OLIVIA ↔ CONTESTIA, section 13)
        if (f != Family::All && familyOfKind(s.kind) != f) continue;
        if (one) { if (&s == one) d_->add(&s); }
        else if (anyK >= 0) { if (s.kind == anyK && s.common) d_->add(&s); }
        else if (s.common) d_->add(&s);
    }
    if (d_->need8) d_->rs8.init(inRate, 8000);
    if (d_->need11) d_->rs11.init(inRate, 11025);
}
TextModeDecoder::~TextModeDecoder() = default;

void TextModeDecoder::feed(const float* x, int n) {
    if (d_->need8) {
        d_->rs8.feed(x, n);
        for (auto& s : d_->streams) if (s->rate == 8000) s->feed(d_->rs8.out.data(), (int)d_->rs8.out.size());
    }
    if (d_->need11) {
        d_->rs11.feed(x, n);
        for (auto& s : d_->streams) if (s->rate == 11025) s->feed(d_->rs11.out.data(), (int)d_->rs11.out.size());
    }
    d_->after();
}

std::string TextModeDecoder::detected() const { return d_->winner ? d_->winner->sub->label : ""; }
float TextModeDecoder::hz() const { return d_->winner ? d_->winner->hz() : 0.f; }

std::vector<TextModeDecoder::ModeInfo> TextModeDecoder::modes(Family f) {
    std::vector<ModeInfo> v{{"auto", "AUTO", ""}};
    if (f == Family::All) {
        // ★★★ grouped (src/services/autoModes.ts draws FAMILY, then MODE): each group's ANY (its own AUTO) first, then
        //     EVERY sub-mode we decode — the common ones and the manual-only ones (OLIVIA 64/2000, CONTESTIA 4/125 …)
        for (int k : {kMfsk, kOlivia, kContestia, kThor}) {
            const char* g = groupOfKind(k);
            v.push_back({std::string("any-") + groupId(k), "ANY", g});
            for (const Sub& s : kSubs) if (s.kind == k) {
                std::string l = s.label; const size_t sp = l.find(' ');
                l = sp == std::string::npos ? l.substr(4) : l.substr(sp + 1);   // "MFSK16" → "16", "OLIVIA 8/250" → "8/250"
                v.push_back({s.id, l, g});
            }
        }
        return v;
    }
    for (const Sub& s : kSubs) if (familyOfKind(s.kind) == f) v.push_back({s.id, s.label, ""});
    return v;
}
std::string TextModeDecoder::modesJson(Family f) {
    std::string j = "[";
    for (const ModeInfo& m : modes(f)) {
        if (j.size() > 1) j += ',';
        j += "{\"id\":\"" + m.id + "\",\"label\":\"" + m.label + "\"";
        if (f == Family::All) j += ",\"group\":\"" + m.group + "\"";
        j += "}";
    }
    return j + "]";
}
bool TextModeDecoder::familyOf(const std::string& ext, Family& f) {
    if (ext == "mfsk") { f = Family::All; return true; }
    if (ext == "olivia") { f = Family::Olivia; return true; }
    if (ext == "thor") { f = Family::Thor; return true; }
    return false;
}
const char* TextModeDecoder::extName(Family f) { return f == Family::Mfsk || f == Family::All ? "mfsk" : f == Family::Olivia ? "olivia" : "thor"; }

// ── the test transmitter ─────────────────────────────────────────────────────────────────────────────────────────────
std::vector<float> textModeSynth(TextModeDecoder::Family, const std::string& mode, const std::string& text, double fs,
                                 double audioHz, uint32_t seed) {
    const Sub* s = subById(mode);
    std::vector<float> out;
    if (!s) return out;
    std::mt19937 rng(seed);
    const double baud = (double)s->rate / s->sps;
    const double spacing = baud * (s->stride / 2);
    if (s->kind == kMfsk || s->kind == kThor) {
        // bits: varicode, then NUL and zeros to flush the coder and the interleaver
        std::vector<int> bits;
        auto put = [&](int c) { for (const char* p = kMfskVaricode[c & 255]; *p; ++p) bits.push_back(*p == '1'); };
        if (s->kind == kThor) for (int i = 0; i < 64; ++i) bits.push_back(0);   // zeros first: IFK+ steps of 2 (idle)
        for (unsigned char c : text) put(c == '\n' ? 13 : c);
        put(0);
        for (int i = 0; i < 160; ++i) bits.push_back(0);
        std::vector<int> coded; int reg = 0;
        for (int b : bits) { reg = ((reg << 1) | b) & 127; coded.push_back(parity(reg & kPoly1)); coded.push_back(parity(reg & kPoly2)); }
        while (coded.size() % 4) coded.push_back(0);
        const int nIn = (int)coded.size() / 4, nOut = nIn + 30;
        std::vector<int> tones;
        for (int i = 0; i < 8; ++i) tones.push_back(0);   // the idle carrier (MFSK: lowest tone)
        int prev = 0;
        for (int t = 0; t < nOut; ++t) {
            int w = 0;
            for (int i = 0; i < 4; ++i) { const int si = t - 10 * i; const int bit = si >= 0 && si < nIn ? coded[(size_t)si * 4 + i] : 0; w = (w << 1) | bit; }
            if (s->kind == kMfsk) tones.push_back(grayInv(w));
            else { prev = (prev + 2 + w) % 18; tones.push_back(prev); }
        }
        const double base = audioHz - spacing * (s->M - 1) / 2;
        const double symT = 1.0 / baud; double ph = 0;
        const size_t total = (size_t)std::ceil((tones.size() + 2) * symT * fs);
        out.assign(total, 0.f);
        for (size_t i = 0; i < total; ++i) {
            const size_t k = (size_t)((double)i / fs / symT);
            if (k >= tones.size()) break;
            ph += 2 * M_PI * (base + spacing * tones[k]) / fs;
            if (ph > 2 * M_PI) ph -= 2 * M_PI;
            out[i] = (float)std::sin(ph);
        }
        return out;
    }
    // Olivia / Contestia
    int b = 0; while ((1 << b) < s->M) ++b;
    const bool con = s->kind == kContestia;
    const int K = con ? 32 : 64, cbits = con ? 6 : 7; const uint64_t scr = con ? kContestiaScr : kOliviaScr; const int rotStep = con ? 5 : 13;
    const uint64_t mask = K == 64 ? ~0ull : ((1ull << K) - 1);
    std::vector<int> codes;
    for (int i = 0; i < b; ++i) codes.push_back(0);   // a block of idle first
    for (unsigned char c : text) {
        int code;
        if (con) {
            int u = (c >= 'a' && c <= 'z') ? c - 32 : c;
            if (u == ' ') code = 59; else if (u == '\n') code = 60; else if (u > 32 && u < 91) code = u - 32; else continue;
        } else code = c == '\n' ? 13 : (c & 127);
        codes.push_back(code);
    }
    while (codes.size() % b) codes.push_back(0);
    for (int i = 0; i < b; ++i) codes.push_back(0);
    std::vector<int> tones;
    for (size_t blk = 0; blk < codes.size(); blk += b) {
        std::vector<int> sym(K, 0);
        for (int j = 0; j < b; ++j) {
            const int code = codes[blk + j], idx = code & (K - 1), top = (code >> (cbits - 1)) & 1;
            const int sg = top ^ parity((uint32_t)idx);
            const int r = (rotStep * j) % K;
            const uint64_t sj = r ? (((scr >> r) | (scr << (K - r))) & mask) : scr;
            for (int k = 0; k < K; ++k) {
                const int d = parity((uint32_t)(idx & k)) ^ sg;
                const int e = d ^ (int)((sj >> k) & 1);
                sym[k] |= e << ((j + k) % b);
            }
        }
        for (int k = 0; k < K; ++k) tones.push_back(grayOf(sym[k]));
    }
    const double bw = spacing * s->M;
    const double base = audioHz - bw / 2 + spacing / 2;
    const int sps = (int)std::lround(fs / baud);
    out.assign((size_t)(tones.size() + 3) * sps, 0.f);
    double ph = 0;
    std::uniform_int_distribution<int> coin(0, 1);
    for (size_t t = 0; t < tones.size(); ++t) {
        ph += coin(rng) ? M_PI / 2 : -M_PI / 2;
        const double w = 2 * M_PI * (base + spacing * tones[t]) / fs;
        for (int i = 0; i < 2 * sps; ++i) {
            const double x = M_PI * (2.0 * (i + 0.5) / (2 * sps) - 1.0);
            out[t * sps + i] += (float)(0.42 * oliviaShape(x) / 2.4 * std::sin(ph + w * i));
        }
        ph += w * sps;
    }
    return out;
}

} // namespace vibe
