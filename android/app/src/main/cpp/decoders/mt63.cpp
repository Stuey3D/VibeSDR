// decoders/mt63.cpp — MT63 receiver and test transmitter. See mt63.h for where each fact came from. V12, 2026-11-19.
#include "mt63.h"
#include "digimodes/q65_simd.h"
#include "../vibedsp/third_party/pffft/pffft.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <cstdio>
#include <cstdlib>

namespace vibe {

// ★ OUR shape: Σ a_m cos(2π m x), x ∈ [−½, ½]; fitted (docs/v12/HF-DATA.md, scratch design script) so that the pulse's
//   autocorrelation is 0 at ±200 and ±400 samples of 512 (no ISI between a carrier's own symbols) and a carrier 4 bins
//   away is orthogonal to it at ±100 / ±300 (the staggered neighbour), with its energy inside ±6 bins.
static const double kShapeA[14] = {0.2576188194, 0.4288332057, 0.2073242803, 0.0387437463, 0.0361475461, 0.0345873175,
                                   0.0023956483, -0.0011147754, -0.0031158006, -0.0006678511, -0.0005439454,
                                   -0.0002846314, 0.0001972004, -0.0000688816};
double mt63Shape(double x) {
    if (x < -0.5 || x > 0.5) return 0.0;
    double s = 0; for (int m = 0; m < 14; ++m) s += kShapeA[m] * std::cos(2 * M_PI * m * x);
    return s;
}

namespace {

constexpr int kC = 64;           // carriers = chips
#ifndef VIBE_MT63_PHASES
#define VIBE_MT63_PHASES 8
#endif
constexpr int kPhases = VIBE_MT63_PHASES;   // timings tried a symbol (a hop each): 8 = an eighth of a symbol apart
constexpr int kHalf = kPhases / 2;           // the odd carriers' half-symbol, in hops
constexpr int kHopR = 200 / kPhases;         // a hop at the mode's own rate (samples)
constexpr int kCand = 7;         // places tried: the finder's ± 3 bins

/** interleave delays d_i (symbols after the character that carrier i sends its chip) */
void delays(bool longI, int* d, int& L) {
    L = longI ? 64 : 32;
    int p = 0;
    for (int i = 0; i < kC; ++i) {
        d[i] = p == 0 ? 0 : L - p;
        const int step = longI ? (i + 1) % 64 : 4 + (i & 3);
        p += step; if (p >= L) p -= L;
    }
}
/** the receive Walsh transform (each stage (a, b) → (a + b, b − a), step 1 … 32): v_k = ⟨chips, W_k⟩ for the
 *  transmitter's W_k = (stages (a, b) → (a − b, a + b), step 32 … 1) of the unit vector e_k */
void walshRx(float* v) {
    for (int i = 0; i < kC; i += 4) {   // steps 1 and 2 inside a group of four
        const float a = v[i], b = v[i + 1], c = v[i + 2], e = v[i + 3];
        const float a1 = a + b, b1 = b - a, c1 = c + e, e1 = e - c;
        v[i] = a1 + c1; v[i + 1] = b1 + e1; v[i + 2] = c1 - a1; v[i + 3] = e1 - b1;
    }
    for (int h = 4; h < kC; h <<= 1)
        for (int i = 0; i < kC; i += 2 * h)
            for (int j = i; j < i + h; j += 4) {
                const q65v::V a = q65v::ld(v + j), b = q65v::ld(v + j + h);
                q65v::st(v + j, q65v::add(a, b)); q65v::st(v + j + h, q65v::sub(b, a));
            }
}
void walshTx(float* v) {
    for (int h = kC / 2; h >= 1; h >>= 1)
        for (int i = 0; i < kC; i += 2 * h)
            for (int j = i; j < i + h; ++j) { const float a = v[j], b = v[j + h]; v[j] = a - b; v[j + h] = a + b; }
}

struct Dec {   // one interleave's de-interleaver and character reader on one candidate
    bool on = false, longI = false; int L = 32; int d[kC]; int dmax = 0;
    float q = 0;           // EMA of the characters' Walsh share (noise ≈ 0.06)
    bool lock = false; int esc = 0;
    /** ★ which way up: EMA of (the character is text) − (its inverse, code ^ 64, is text). A reading with the wrong
     *  half-turn reads every character inverted — idle NUL as '@', "CQ" as control codes, a space as '`' — with the same
     *  Walsh share; text is the only thing that tells (and idle, which is most of an over, tells at once). */
    float upright = 0;
    static bool texty(int c) { return c == 0 || c == 10 || c == 13 || c == ' ' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '?' || c == '/'; }
    std::string pre;       // characters read before the lock (the call)
    void init(bool l) {
        on = true; longI = l; delays(l, d, L);
        dmax = 0; for (int i = 0; i < kC; ++i) dmax = std::max(dmax, d[i]);
    }
};

struct Cand {
    int b = -1;                       // the lowest carrier's bin
    float zr[kPhases][kC], zi[kPhases][kC];   // each carrier at the last eight hops (index hop & 7)
    float met[kPhases];               // EMA of the products' strength at each timing
    int h = 0;                        // the timing read (even carriers at hops ≡ h, odd at h + 4)
    float er[kC], ei[kC]; int64_t evenHop = -100;   // the even carriers' products at the last even read
    float ur = 0, ui = 0;             // EMA of Σ d² (the common turn a symbol, doubled)
    float amp[kC];                    // EMA of |d| a carrier (fading / ripple across the band)
    float chips[64][kC]; int64_t nsym = 0;
    Dec dec[2];
    void reset(int b_) {
        b = b_; std::memset(zr, 0, sizeof zr); std::memset(zi, 0, sizeof zi); std::memset(met, 0, sizeof met);
        h = 0; evenHop = -100; ur = ui = 0; for (float& a : amp) a = 0; std::memset(chips, 0, sizeof chips); nsym = 0;
        for (Dec& x : dec) { x.q = 0; x.lock = false; x.esc = 0; x.pre.clear(); x.upright = 0; }
    }
};

} // namespace

struct Mt63Rx::Impl {
    int bw, D, N, hop; double binHz;
    std::string lab;
    std::vector<float> ring, win; int wp = 0, filled = 0, sinceHop = 0; int64_t hops = 0;
    PFFFT_Setup* setup = nullptr; float *fin = nullptr, *fout = nullptr, *work = nullptr;
    std::vector<float> P, Pavg, cum;
    float cosT[512], sinT[512];
    int kLo = 0, kHi = 0;
    Cand cand[kCand]; int centre = -1;   // the finder's place (cand[3].b)
    int seen = 0, seenB = -1;             // finder runs in a row that named (about) seenB
    bool en[2] = {false, false};
    int cur = -1, curD = 0;               // the locked (candidate, interleave), −1 none
    int64_t lastAt = -1000; int lastCode = -1;
    const bool dbg_ = std::getenv("VIBE_MT63_DEBUG") != nullptr;   // the hop time and code of the last character shown
    std::string* out = nullptr;

    Impl(int bw_, bool longI, const char* label) : bw(bw_), lab(label) {
        D = 8000 / (2 * bw); N = 512 * D; hop = kHopR * D; binHz = 8000.0 / N;
        ring.assign((size_t)2 * N, 0.f); win.assign(N, 0.f);
        for (int i = 0; i < N; ++i) win[i] = (float)mt63Shape((i + 0.5 - N / 2.0) / N);
        setup = pffft_new_setup(N, PFFFT_REAL);
        fin = (float*)pffft_aligned_malloc(N * sizeof(float));
        fout = (float*)pffft_aligned_malloc(N * sizeof(float));
        work = (float*)pffft_aligned_malloc(N * sizeof(float));
        P.assign(N / 2, 0.f); Pavg.assign(N / 2, 0.f); cum.assign(N / 2 + 1, 0.f);
        for (int m = 0; m < 512; ++m) { cosT[m] = (float)std::cos(2 * M_PI * m / 512); sinT[m] = (float)-std::sin(2 * M_PI * m / 512); }
        kLo = std::max(8, (int)std::ceil(200.0 / binHz)); kHi = std::min(N / 2 - 8, (int)std::floor(2900.0 / binHz));
        en[longI ? 1 : 0] = true;
    }
    ~Impl() { pffft_destroy_setup(setup); pffft_aligned_free(fin); pffft_aligned_free(fout); pffft_aligned_free(work); }

    void place(int b) {   // centre the candidates on b, keeping those already there
        if (b == centre) return;
        std::vector<Cand> old(cand, cand + kCand);   // (≈ 25 KB each: on the heap, not the decoder thread's stack)
        for (int c = 0; c < kCand; ++c) {
            const int nb = b - 3 + c; bool kept = false;
            for (int o = 0; o < kCand; ++o) if (old[o].b == nb) { cand[c] = old[o]; kept = true; break; }
            if (!kept) { cand[c].reset(nb); for (int k = 0; k < 2; ++k) if (en[k]) cand[c].dec[k].init(k == 1); }
        }
        if (cur >= 0) { int nc = -1; for (int c = 0; c < kCand; ++c) if (cand[c].b == old[cur].b) nc = c; cur = nc; }
        centre = b;
    }
    /** ★ is (candidate ci, interleave k) the best reading of all — the highest characters' share? A place one bin
     *  off reads the same carriers nearly as well but with a turn of 0.78π a symbol, every character inverted; it is
     *  always the weaker reading (measured: 0.29 / 0.31 beside 0.51 on the MT63 1000L reference). */
    bool bestReading(int ci, int k) const {
        const float q = cand[ci].dec[k].q;
        for (int c = 0; c < kCand; ++c) for (int x = 0; x < 2; ++x)
            if ((c != ci || x != k) && cand[c].dec[x].on && cand[c].dec[x].upright > 0.f && cand[c].dec[x].q > q) return false;
        return true;
    }
    /** the place of the band: the lowest carrier's bin where the 64 carriers' band most stands out from either side */
    void finder() {
        cum[0] = 0; for (int k = 0; k < N / 2; ++k) cum[k + 1] = cum[k] + Pavg[k];
        auto mean = [&](int a, int b) { return (cum[b + 1] - cum[a]) / (b - a + 1); };
        const int span = 4 * (kC - 1);
        auto score = [&](int b) {
            const float in = mean(b - 1, b + span + 1);
            const float lo = mean(b - 20, b - 6), hi = mean(b + span + 6, b + span + 20);
            return in / (0.5f * (lo + hi) + 1e-20f);
        };
        float best = 0; int bb = -1;
        for (int b = kLo + 22; b + span + 22 <= kHi; ++b) { const float sc = score(b); if (sc > best) { best = sc; bb = b; } }
        if (bb < 0) return;
        // ★ HELD unless clearly beaten (×1.3): a weak signal's edges wander on the noise by a few bins, and every move
        //   that leaves the seven places restarts their de-interleavers (MT63 1000 at −4 dB never filled one: 64 symbols)
        //   …but only while something there reads above noise: with nothing decoding, the place follows the best score
        //   at once (held regardless, a fresh over's place was reached seconds late and its first words lost —
        //   the de-interleavers start empty when the places move)
        if (centre >= 0) {
            bool reading = false;
            for (const Cand& c : cand) for (const Dec& d : c.dec) reading |= d.on && d.q > 0.09f;
            const float here = score(std::max(kLo + 22, std::min(centre, kHi - span - 22)));
            if (std::abs(bb - centre) <= 2) { seen = 0; return; }
            if (reading && best < 1.3f * here) { seen = 0; return; }
            if (cur >= 0 && best < 3.f) { seen = 0; return; }
        }
        // ★ and a new place must be the best three symbols running (within 2 bins): on a weak signal the best score
        //   wanders over the noise from symbol to symbol, and each move restarted the readings (MT63 1000L at −2 dB:
        //   nothing)
        if (seen > 0 && std::abs(bb - seenB) <= 2) ++seen; else { seen = 1; seenB = bb; }
        if (centre >= 0 && seen < 3) return;
        seen = 0;
        place(bb);
    }
    void feed(const float* x, int n) {
        for (int i = 0; i < n; ) {
            const int run = std::min(n - i, hop - sinceHop);
            for (int r = 0; r < run; ) {
                const int seg = std::min(run - r, N - wp);
                std::memcpy(ring.data() + wp, x + i + r, (size_t)seg * sizeof(float));
                std::memcpy(ring.data() + wp + N, x + i + r, (size_t)seg * sizeof(float));
                wp += seg; if (wp == N) wp = 0; r += seg;
            }
            i += run; filled = std::min(N, filled + run); sinceHop += run;
            if (sinceHop < hop) break;
            sinceHop = 0;
            if (filled < N) { ++hops; continue; }
            oneHop();
            ++hops;
        }
    }
    void oneHop() {
        const float* o = ring.data() + wp;
        for (int k = 0; k < N; k += 4) q65v::st(fin + k, q65v::mul(q65v::ld(o + k), q65v::ld(win.data() + k)));
        pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
        // powers, four bins a lane, and their average (≈ 4 symbols)
        const q65v::V A = q65v::set1(1.f / 32);   // (≈ 4 symbols)
        for (int k = 0; k < N / 2; k += 4) {
            q65v::V re, im; q65v::ld2(fout + 2 * k, re, im);
            const q65v::V p = q65v::add(q65v::mul(re, re), q65v::mul(im, im));
            const q65v::V a = q65v::ld(Pavg.data() + k);
            q65v::st(Pavg.data() + k, q65v::add(a, q65v::mul(A, q65v::sub(p, a))));
        }
        if (hops % kPhases == 0) finder();
        if (centre < 0) return;
        const int hm = (int)(hops % kPhases);
        const unsigned ph = (unsigned)((kHopR * hops) & 511);   // window start / N = kHopR·hops / 512 (mod 1)
        for (int c = 0; c < kCand; ++c) candHop(cand[c], hm, ph);
    }
    void candHop(Cand& c, int hm, unsigned ph) {
        if (c.b < 1 || c.b + 4 * (kC - 1) >= N / 2) return;
        float dr[kC], di[kC]; float sd[2] = {0, 0}, sz[2] = {0, 0};
        for (int i = 0; i < kC; ++i) {
            const int k = c.b + 4 * i;
            const unsigned m = ((unsigned)k * ph) & 511;
            const float xr = fout[2 * k], xi = fout[2 * k + 1];
            const float zr = xr * cosT[m] - xi * sinT[m], zi = xr * sinT[m] + xi * cosT[m];
            const float pr = c.zr[hm][i], pi = c.zi[hm][i];
            dr[i] = zr * pr + zi * pi; di[i] = zi * pr - zr * pi;   // z · conj(z eight hops ago)
            c.zr[hm][i] = zr; c.zi[hm][i] = zi;
            sd[i & 1] += std::sqrt(dr[i] * dr[i] + di[i] * di[i]);
            sz[i & 1] += 0.5f * (zr * zr + zi * zi + pr * pr + pi * pi);
        }
        // the timing metric: the even carriers read at this hop are timing hm; the odd ones are timing hm − 4
        c.met[hm] += 0.125f * (sd[0] / (sz[0] + 1e-20f) - c.met[hm]);
        const int ho = (hm + kHalf) % kPhases;
        c.met[ho] += 0.125f * (sd[1] / (sz[1] + 1e-20f) - c.met[ho]);
        if (hm == (c.h + kHalf) % kPhases && c.evenHop == hops - kHalf) {   // the odd carriers of the symbol whose even ones came 4 hops ago
            for (int i = 1; i < kC; i += 2) { c.er[i] = dr[i]; c.ei[i] = di[i]; }
            symbol(c);
        }
        if (hm == c.h) {
            for (int i = 0; i < kC; i += 2) { c.er[i] = dr[i]; c.ei[i] = di[i]; }
            c.evenHop = hops;
            // a timing that reads clearly better (by 10 %) is taken — at an even read, so a symbol is never split
            int bh = 0; for (int t = 1; t < kPhases; ++t) if (c.met[t] > c.met[bh]) bh = t;
            if (bh != c.h && c.met[bh] > 1.1f * c.met[c.h]) { c.h = bh; c.evenHop = -100; }
        }
    }
    void symbol(Cand& c) {
        // the common turn a symbol (a fractional-bin offset): the squared products' mean, halved
        float sr = 0, si = 0;
        for (int i = 0; i < kC; ++i) {
            const float m = std::sqrt(c.er[i] * c.er[i] + c.ei[i] * c.ei[i]) + 1e-20f;
            sr += (c.er[i] * c.er[i] - c.ei[i] * c.ei[i]) / m; si += 2 * c.er[i] * c.ei[i] / m;
        }
        c.ur += 0.25f * (sr - c.ur); c.ui += 0.25f * (si - c.ui);
        const float ang = 0.5f * std::atan2(c.ui, c.ur), rc = std::cos(ang), rs = -std::sin(ang);
        float* ch = c.chips[c.nsym & 63];
        for (int i = 0; i < kC; ++i) {
            const float s = c.er[i] * rc - c.ei[i] * rs;   // Re(d · e^(−j·turn))
            const float a = std::sqrt(c.er[i] * c.er[i] + c.ei[i] * c.ei[i]);
            c.amp[i] += 0.1f * (a - c.amp[i]);
            float v = -s / (c.amp[i] + 1e-20f);            // a kept phase is a 1, sent as a chip < 0
            ch[i] = std::max(-3.f, std::min(3.f, v));
        }
        ++c.nsym;
        const int ci = (int)(&c - cand);
        for (int k = 0; k < 2; ++k) if (c.dec[k].on) readChar(c, c.dec[k], ci, k);
    }
    void readChar(Cand& c, Dec& d, int ci, int k) {
        const int64_t t = c.nsym - 1, ch = t - d.dmax;   // the character whose last chip just arrived
        if (ch < 0) return;
        alignas(16) float v[kC];
        for (int i = 0; i < kC; ++i) v[i] = c.chips[(ch + d.d[i]) & 63][i];
        walshRx(v);
        float mx = 0, sum = 0; int im = 0;
        for (int i = 0; i < kC; ++i) { const float a = std::fabs(v[i]); sum += a; if (a > mx) { mx = a; im = i; } }
        const float q = sum > 0 ? mx / sum : 0.f;
        const int code = v[im] > 0 ? im : im + 64;
        d.q += 0.125f * (q - d.q);
        if (dbg_) std::fprintf(stderr, "D b%d %c n%lld q%.3f Q%.3f c%d up%.2f cur%d\n", c.b, d.longI ? 'L' : 'S', (long long)c.nsym, q, d.q, code, d.upright, cur);
        if (q > 0.09f) d.upright += 0.125f * ((Dec::texty(code) ? 1.f : 0.f) - (Dec::texty(code ^ 64) ? 1.f : 0.f) - d.upright);

        // ★ lock: the characters' share well above noise's for the last eight or so (noise, 5 min through each bandwidth:
        //   0.051 on average, 0.099 at most for one character, 0.061 at most averaged) — 0.12; held down to 0.085. ONE
        //   (place, interleave) is read at a time; a neighbouring place reads the same carriers a bin off and nearly as
        //   well, so another takes over only when it is clearly better (×1.25)
        const bool mine = cur == ci && curD == k;
        // ★★ ONLY THE PLACE NEAREST THE CARRIERS (2026-11-19). A turn of π a symbol cannot be told from a data flip on
        //   every carrier, so a reading one bin off (0.78π a symbol in every bandwidth) takes out the wrong half-turn and
        //   reads every character inverted (code ^ 64: "@@@` CQ" for idle and "CQ"). The place nearest the carriers is
        //   ≤ ½ bin off (≤ 0.39π, never ambiguous).
        //   The comb's energy does not show it (measured: the spectrum averaged over timings ripples with its maxima
        //   BETWEEN carriers; at each reading's own timing it picked an edge place on a real recording), the reading's
        //   own quality does: a lock is taken only by the best reading of all, and kept to the end (the last
        //   characters are still in the interleaver when the carriers stop).
        if (!mine && (!bestReading(ci, k) || d.upright <= 0.f)) { d.pre.clear(); return; }
        if (!mine && d.q > 0.12f && c.nsym > d.dmax + 8) {
            const float cq = cur >= 0 ? cand[cur].dec[curD].q : 0.f;
            if (cur < 0 || d.q > 1.15f * cq) {
                if (cur >= 0) cand[cur].dec[curD].lock = false;
                const bool fresh = cur < 0;
                d.lock = true; cur = ci; curD = k;
                if (fresh) for (char x : d.pre) text += x;
                lastAt = hops - kPhases * (int64_t)(d.dmax + 2);
                d.pre.clear();
            }
        } else if (mine && d.q < 0.085f) { d.lock = false; cur = -1; }
        if (q < 0.09f) return;
        // ★ each character once: a reading taking over from a neighbour has read the last few characters too (their
        //   time: the symbol its first chip was sent, in hops)
        const int64_t at = hops - kPhases * (int64_t)(d.dmax + 1);
        //   ★ (a neighbour's timing may differ by up to most of a symbol, so the time alone cannot tell: the same code
        //   within 1½ symbols of the last one shown is the same character)
        if (d.lock && cur == ci && curD == k) {
            if (at > lastAt + kHalf && !(code == lastCode && at < lastAt + kPhases + kHalf)) { put(d, code, text); lastAt = at; lastCode = code; }
        }
        // ★ before the lock, only once this reading has settled (its average above noise's): while the common turn
        //   settles a block of idle reads as its inverse ("@@@@@@@@" before every synthetic MT63 500S over)
        else if (!d.lock && q > 0.11f && d.q > 0.075f) { put(d, code, d.pre); if (d.pre.size() > 32) d.pre.erase(0, d.pre.size() - 32); }
        else if (!d.lock) d.pre.clear();
    }
    static void put(Dec& d, int c, std::string& out) {
        if (c == 127) { d.esc = 1; return; }
        if (d.esc) { c += 128; d.esc = 0; }
        else if (c < 8) return;
        if (c == 13 || c == 10) { if (out.empty() || out.back() != '\n') out += '\n'; return; }
        if (c == 8) { if (!out.empty() && out.back() != '\n') out.pop_back(); return; }
        if (c >= 32 && c < 127) { out += (char)c; return; }
        if (c >= 160 && c < 256) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
    }
    std::string text;
};

Mt63Rx::Mt63Rx(int bw, bool longIntlv, const char* label) : d_(new Impl(bw, longIntlv, label)) {}
Mt63Rx::~Mt63Rx() = default;
void Mt63Rx::enable(bool l) { d_->en[l ? 1 : 0] = true; for (Cand& c : d_->cand) if (c.b >= 0 && !c.dec[l ? 1 : 0].on) c.dec[l ? 1 : 0].init(l); }
int Mt63Rx::bandwidth() const { return d_->bw; }
void Mt63Rx::feed(const float* x, int n) { d_->feed(x, n); if (!d_->text.empty()) { text += d_->text; d_->text.clear(); } }
bool Mt63Rx::locked() const { return d_->cur >= 0; }
float Mt63Rx::quality() const { return d_->cur >= 0 ? d_->cand[d_->cur].dec[d_->curD].q : 0.f; }
float Mt63Rx::hz() const {
    if (d_->cur < 0) return 0.f;
    return (float)((d_->cand[d_->cur].b + 2 * (kC - 1)) * d_->binHz);
}
const char* Mt63Rx::label() const {
    static const char* names[3][2] = {{"MT63 500S", "MT63 500L"}, {"MT63 1000S", "MT63 1000L"}, {"MT63 2000S", "MT63 2000L"}};
    const int bi = d_->bw == 500 ? 0 : d_->bw == 1000 ? 1 : 2;
    if (d_->cur >= 0) return names[bi][d_->curD];
    return names[bi][d_->en[1] && !d_->en[0] ? 1 : 0];
}

// ── the test transmitter ─────────────────────────────────────────────────────────────────────────────────────────────
std::vector<float> mt63Synth(int bw, bool longI, const std::string& text, double fs, double lowHz, unsigned seed) {
    const double R = 2.0 * bw, Tsym = 200.0 / R, Tw = 512.0 / R, sp = bw / 64.0;
    int d[kC], L; delays(longI, d, L);
    std::mt19937 rng(seed);
    // the characters: idle (0) while the interleaver fills, the text (127 escapes a code ≥ 128), idle to flush it
    std::vector<int> codes;
    for (int i = 0; i < 8; ++i) codes.push_back(0);
    for (unsigned char c : text) { int v = c == '\n' ? 13 : c; if (v >= 128) { codes.push_back(127); v -= 128; } codes.push_back(v); }
    for (int i = 0; i < L + 8; ++i) codes.push_back(0);
    // the pipe: L symbols × 64 chips' bits, prefilled at random (as a transmitter's is)
    std::vector<uint8_t> pipe((size_t)L * kC);
    for (auto& b : pipe) b = rng() & 1;
    const size_t nS = codes.size();
    std::vector<uint8_t> bits(nS * kC);
    for (size_t t = 0; t < nS; ++t) {
        float w[kC] = {}; const int code = codes[t] & 127;
        if (code < 64) w[code] = 1.f; else w[code - 64] = -1.f;
        walshTx(w);
        const int slot = (int)(t % L);
        for (int i = 0; i < kC; ++i) pipe[(size_t)slot * kC + i] = w[i] < 0.f;
        for (int i = 0; i < kC; ++i) bits[t * kC + i] = pipe[(size_t)((slot + (L - d[i]) % L) % L) * kC + i];
    }
    // ★ (slot + (L − d) mod L) mod L = the slot written d symbols ago — p_i ahead in the pipe, as the format has it
    std::vector<double> phase((size_t)kC, 0.0);
    for (int i = 0; i < kC; ++i) phase[i] = (rng() & 1) ? M_PI : 0.0;
    std::vector<std::vector<double>> phk(kC, std::vector<double>(nS + 1));
    for (int i = 0; i < kC; ++i) { double p = phase[i]; phk[i][0] = p; for (size_t t = 0; t < nS; ++t) { if (!bits[t * kC + i]) p += M_PI; phk[i][t + 1] = p; } }
    // symbol k of carrier i: the window centred at Tw/2 + k·Tsym (+ Tsym/2 odd), phase phk[i][k + 1]; symbol −1 holds the start
    const double total = Tw + (nS + 2) * Tsym;
    std::vector<float> out((size_t)std::ceil(total * fs), 0.f);
    const double amp = 0.9 / std::sqrt((double)kC);
    for (size_t n = 0; n < out.size(); ++n) {
        const double t = n / fs; double acc = 0;
        for (int i = 0; i < kC; ++i) {
            const double off = Tw / 2 + ((i & 1) ? Tsym / 2 : 0.0);
            const double kc = (t - off) / Tsym;
            const long k0 = (long)std::floor(kc - Tw / Tsym / 2), k1 = (long)std::ceil(kc + Tw / Tsym / 2);
            const double w = 2 * M_PI * (lowHz + sp * i) * t;
            for (long k = std::max(-1L, k0); k <= std::min((long)nS - 1, k1); ++k) {
                const double x = (t - off - k * Tsym) / Tw;
                if (x < -0.5 || x > 0.5) continue;
                acc += mt63Shape(x) * std::cos(w + phk[i][(size_t)(k + 1)]);
            }
        }
        out[n] = (float)(amp * acc);
    }
    return out;
}

} // namespace vibe
