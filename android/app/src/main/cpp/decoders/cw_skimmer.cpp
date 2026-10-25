// decoders/cw_skimmer.cpp — see cw_skimmer.h for the design. Original VibeSDR code, from first principles: textbook
// short-time Fourier analysis, a two-means classifier, and the Morse code as ITU-R M.1677 defines it.
#include "cw_skimmer.h"
#include "../vibedsp/simd_internal.h"     // ★ VIBE_NEON / VIBE_SSE (and VIBE_FORCE_SCALAR) — the project's one SIMD switch
#include "../vibedsp/third_party/pffft/pffft.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#ifdef CW_DEBUG
#include <cstdio>
#include <cstdlib>
#endif

namespace vibe {
namespace cw {

// ── Morse (ITU-R M.1677-1) ─────────────────────────────────────────────────────────────────────────────────────────
namespace {
struct Code { const char* morse; const char* text; };
// ★ Letters, figures, punctuation, then the prosigns M.1677 gives a sequence but no character. AR (.-.-.) is '+' and
//   BT (-...-) is '=' — the same sequences — and KN (-.--.) is '(' exactly as the Recommendation lists it.
const Code kCodes[] = {
    {".-","A"},{"-...","B"},{"-.-.","C"},{"-..","D"},{".","E"},{"..-.","F"},{"--.","G"},{"....","H"},{"..","I"},
    {".---","J"},{"-.-","K"},{".-..","L"},{"--","M"},{"-.","N"},{"---","O"},{".--.","P"},{"--.-","Q"},{".-.","R"},
    {"...","S"},{"-","T"},{"..-","U"},{"...-","V"},{".--","W"},{"-..-","X"},{"-.--","Y"},{"--..","Z"},
    {".----","1"},{"..---","2"},{"...--","3"},{"....-","4"},{".....","5"},{"-....","6"},{"--...","7"},{"---..","8"},
    {"----.","9"},{"-----","0"},
    {".-.-.-","."},{"--..--",","},{"---...",":"},{"..--..","?"},{".----.","'"},{"-....-","-"},{"-..-.","/"},
    {"-.--.","("},{"-.--.-",")"},{".-..-.","\""},{"-...-","="},{".-.-.","+"},{".--.-.","@"},
    {"...-.","<SN>"},{".-...","<AS>"},{"...-.-","<SK>"},{"-.-.-","<KA>"},{"........","<HH>"},
};
struct Table {
    const char* t[512] = {};
    Table() {
        for (const auto& c : kCodes) {
            unsigned idx = 1;
            for (const char* p = c.morse; *p; ++p) idx = idx * 2 + (*p == '-' ? 1u : 0u);
            if (idx < 512) t[idx] = c.text;
        }
    }
};
const Table& table() { static const Table t; return t; }
} // namespace

const char* morseText(unsigned code) { return code < 512 ? table().t[code] : nullptr; }
std::string morseCode(const std::string& ch) {
    for (const auto& c : kCodes) if (ch == c.text) return c.morse;
    return "";
}
const char* qualityWord(float q) { return q >= 0.75f ? "high" : q >= 0.5f ? "medium" : q >= 0.3f ? "low" : "poor"; }

// ── Callsigns ──────────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
bool isLetter(char c) { return c >= 'A' && c <= 'Z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
/** The core of a call — PREFIX DIGIT SUFFIX — with no '/'. */
bool isCallCore(const std::string& s) {
    if (s.size() < 3 || s.size() > 8) return false;
    for (char c : s) if (!isLetter(c) && !isDigit(c)) return false;
    // ★ The suffix: 1–4 letters at the end, after the LAST digit.
    size_t d = std::string::npos;
    for (size_t i = 0; i < s.size(); ++i) if (isDigit(s[i])) d = i;
    if (d == std::string::npos || d == s.size() - 1) return false;
    const size_t suffix = s.size() - 1 - d;
    if (suffix < 1 || suffix > 4) return false;
    // ★ The prefix: everything before the call-area digit run — 1 to 3 characters with at least one letter.
    size_t a = d; while (a > 0 && isDigit(s[a - 1])) --a;   // "9A1A": prefix "9A", digit "1" — the run is the digit
    if (d - a > 1) return false;                              // ★ at most two digits in a row (4X4ABC, 3DA0AB)
    const std::string pre = s.substr(0, a);         // "2E0ABC" → "2E", "3DA0AB" → "3DA", "4ABC" → "" (no prefix)
    if (pre.empty() || pre.size() > 3) return false;
    bool letter = false; for (char c : pre) letter |= isLetter(c);
    return letter;
}
const char* const kWords[] = { "CQ","DE","TEST","TU","TNX","QRZ","QSL","QRL","QRS","QRQ","QTH","NAME","RST","UR","FB","OM",
                               "GM","GA","GE","GN","PSE","AGN","HW","CPY","ES","73","88","5NN","599","K","KN","BK","R","RR",
                               "POTA","SOTA","WWFF","IOTA","DX","NA","EU","AS","AF","OC","SA","JA" };
bool isWord(const std::string& s) { for (const char* w : kWords) if (s == w) return true; return false; }
} // namespace

bool isCallsign(const std::string& in) {
    if (in.size() < 3 || in.size() > 14 || isWord(in)) return false;
    // ★ Portable forms: G4ABC/P, EA8/G4ABC, G4ABC/MM — the longest part is the call, the rest is short.
    std::vector<std::string> parts; std::string cur;
    for (char c : in) { if (c == '/') { parts.push_back(cur); cur.clear(); } else cur += c; }
    parts.push_back(cur);
    if (parts.size() > 3) return false;
    size_t best = 0;
    for (size_t i = 1; i < parts.size(); ++i) if (parts[i].size() > parts[best].size()) best = i;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty() || (parts[i].size() > 4 && i != best)) return false;
        for (char c : parts[i]) if (!isLetter(c) && !isDigit(c)) return false;
    }
    if (!isCallCore(parts[best])) return false;
    // ★ Strings noise likes to make: a call is not all one letter repeated around a digit ("EEE5E", "TTT0T").
    std::string letters; for (char c : parts[best]) if (isLetter(c)) letters += c;
    bool same = letters.size() >= 3; for (char c : letters) same &= c == letters[0];
    return !same;
}

bool identify(const std::vector<std::string>& w, std::string& call, std::string& how) {
    call.clear(); how.clear();
    const size_t n = w.size();
    if (n < 2) return false;
    std::string c = w[n - 1], p = w[n - 2];
    auto ctx = [&](const char* word, size_t back) {
        for (size_t i = n > back ? n - back : 0; i < n; ++i) if (w[i] == word) return true;
        return false;
    };
    auto howFor = [&]() -> std::string { return ctx("TEST", 6) ? "TEST" : ctx("CQ", 6) ? "CQ" : "DE"; };
    // ★ "CALL TEST" — a contest CQ sent call first.
    if (c == "TEST" && isCallsign(p)) { call = p; how = "TEST"; return true; }
    // ★ A word glued to its context by a missed gap: "DEG4ABC", "CQG4ABC".
    for (const char* g : { "CQ", "DE" }) {
        const size_t gl = std::strlen(g);
        if (c.size() > gl + 2 && c.compare(0, gl, g) == 0 && isCallsign(c.substr(gl))) { p = g; c = c.substr(gl); break; }
    }
    // ★★ A call broken by a gap read as a word space ("DE IU 5R DL"): up to three short pieces after DE/CQ/TEST/TU
    //    joined, the longest that is a call. Two different readings of one call never make a spot by themselves — a spot
    //    needs the SAME call twice.
    for (size_t k = std::min<size_t>(3, n - 1); k >= 2; --k) {
        const std::string& ctxw = w[n - 1 - k];
        if (ctxw != "DE" && ctxw != "CQ" && ctxw != "TEST" && ctxw != "TU") continue;
        std::string j; bool shortParts = true;
        for (size_t i = n - k; i < n; ++i) { if (w[i].size() > 4 || isWord(w[i])) shortParts = false; j += w[i]; }
        if (shortParts && isCallsign(j)) { call = j; how = ctxw == "TEST" ? "TEST" : howFor(); return true; }
    }
    if (!isCallsign(c)) return false;
    if (p == "DE" || p == "TU") { call = c; how = howFor(); return true; }
    if (p == "CQ" || p == "TEST") { call = c; how = p == "TEST" || ctx("TEST", 6) ? "TEST" : "CQ"; return true; }
    // ★ "CQ DX G4ABC", "CQ POTA G4ABC", "CQ CQ TEST G4ABC" — a modifier between CQ and the call.
    if (n >= 3 && (w[n - 3] == "CQ" || w[n - 3] == "TEST") && isWord(p)) { call = c; how = howFor(); return true; }
    // ★ The call sent twice — "G4ABC G4ABC" — how every station signs.
    if (p == c) { call = c; how = howFor(); return true; }
    return false;
}

// ── The kernels ───────────────────────────────────────────────────────────────────────────────────────────────────
// ★★ Every loop the skimmer runs over ALL bins is here, as a NEON body, an SSE2 body and the scalar reference.
//    NEON: only ARMv7-safe intrinsics (no vaddvq / vmaxvq — those are AArch64-only), so the Pi 2 runs the same code
//    as arm64. SSE2: nothing newer (no blendv, no hadd), so a 32-bit Atom runs it.
namespace {
/** out = in · w, complex (interleaved re/im) — the window on IQ. */
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
        // ★ interleaved: lanes r0 i0 r1 i1 take w0 w0 w1 w1
        const __m128 lo = _mm_unpacklo_ps(ww, ww), hi = _mm_unpackhi_ps(ww, ww);
        _mm_storeu_ps(out + 2 * i,     _mm_mul_ps(_mm_loadu_ps(in + 2 * i), lo));
        _mm_storeu_ps(out + 2 * i + 4, _mm_mul_ps(_mm_loadu_ps(in + 2 * i + 4), hi));
    }
#endif
    for (; i < n; ++i) { out[2 * i] = in[2 * i] * w[i]; out[2 * i + 1] = in[2 * i + 1] * w[i]; }
}
void windowReal(const float* in, const float* w, float* out, int n) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) vst1q_f32(out + i, vmulq_f32(vld1q_f32(in + i), vld1q_f32(w + i)));
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(out + i, _mm_mul_ps(_mm_loadu_ps(in + i), _mm_loadu_ps(w + i)));
#endif
    for (; i < n; ++i) out[i] = in[i] * w[i];
}
/** p[i] = |z[i]|² over interleaved complex bins. */
void power(const float* z, float* p, int n) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(z + 2 * i);
        vst1q_f32(p + i, vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]));
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        __m128 re, im; sseLoad2(z + 2 * i, re, im);
        _mm_storeu_ps(p + i, _mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im)));
    }
#endif
    for (; i < n; ++i) p[i] = z[2 * i] * z[2 * i] + z[2 * i + 1] * z[2 * i + 1];
}
/** ★★ THE PER-BIN STATE, ONE PASS: the short average S, the long average M, and the floor F, which falls at aD toward
 *  S and rises at aU — but never by more than cU·F a hop, so a strong keyed carrier cannot drag its own floor up. */
struct Rates { float aS, aM, aD, aU, cU, tiny; };
void track(const float* P, float* S, float* M, float* F, int n, const Rates& r) {
    int i = 0;
#if VIBE_NEON
    const float32x4_t aS = vdupq_n_f32(r.aS), aM = vdupq_n_f32(r.aM), aD = vdupq_n_f32(r.aD), aU = vdupq_n_f32(r.aU),
                      cU = vdupq_n_f32(r.cU), tiny = vdupq_n_f32(r.tiny);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t p = vld1q_f32(P + i);
        float32x4_t s = vld1q_f32(S + i), m = vld1q_f32(M + i), f = vld1q_f32(F + i);
        s = vmlaq_f32(s, vsubq_f32(p, s), aS);
        m = vmlaq_f32(m, vsubq_f32(p, m), aM);
        const float32x4_t d = vsubq_f32(s, f);
        const float32x4_t down = vmulq_f32(d, aD);
        const float32x4_t up = vminq_f32(vmulq_f32(d, aU), vmulq_f32(f, cU));
        f = vmaxq_f32(vaddq_f32(f, vbslq_f32(vcltq_f32(s, f), down, up)), tiny);
        vst1q_f32(S + i, s); vst1q_f32(M + i, m); vst1q_f32(F + i, f);
    }
#elif VIBE_SSE
    const __m128 aS = _mm_set1_ps(r.aS), aM = _mm_set1_ps(r.aM), aD = _mm_set1_ps(r.aD), aU = _mm_set1_ps(r.aU),
                 cU = _mm_set1_ps(r.cU), tiny = _mm_set1_ps(r.tiny);
    for (; i + 4 <= n; i += 4) {
        const __m128 p = _mm_loadu_ps(P + i);
        __m128 s = _mm_loadu_ps(S + i), m = _mm_loadu_ps(M + i), f = _mm_loadu_ps(F + i);
        s = _mm_add_ps(s, _mm_mul_ps(_mm_sub_ps(p, s), aS));
        m = _mm_add_ps(m, _mm_mul_ps(_mm_sub_ps(p, m), aM));
        const __m128 d = _mm_sub_ps(s, f);
        const __m128 down = _mm_mul_ps(d, aD);
        const __m128 up = _mm_min_ps(_mm_mul_ps(d, aU), _mm_mul_ps(f, cU));
        const __m128 lt = _mm_cmplt_ps(s, f);
        const __m128 step = _mm_or_ps(_mm_and_ps(lt, down), _mm_andnot_ps(lt, up));   // ★ SSE2's select
        f = _mm_max_ps(_mm_add_ps(f, step), tiny);
        _mm_storeu_ps(S + i, s); _mm_storeu_ps(M + i, m); _mm_storeu_ps(F + i, f);
    }
#endif
    for (; i < n; ++i) {
        S[i] += (P[i] - S[i]) * r.aS;
        M[i] += (P[i] - M[i]) * r.aM;
        const float d = S[i] - F[i];
        const float step = S[i] < F[i] ? d * r.aD : std::min(d * r.aU, F[i] * r.cU);
        F[i] = std::max(F[i] + step, r.tiny);
    }
}
/** Indices i in [0, n) where M[i] > k·F[i] — the scan for carriers. Four bins tested per instruction; only a group
 *  with something in it is looked at one by one. */
int above(const float* M, const float* F, float k, int n, int* idx, int maxIdx) {
    int c = 0, i = 0;
#if VIBE_NEON
    const float32x4_t kk = vdupq_n_f32(k);
    for (; i + 4 <= n && c + 4 <= maxIdx; i += 4) {
        const uint32x4_t g = vcgtq_f32(vld1q_f32(M + i), vmulq_f32(vld1q_f32(F + i), kk));
        const uint32x2_t o = vorr_u32(vget_low_u32(g), vget_high_u32(g));
        if ((vget_lane_u32(o, 0) | vget_lane_u32(o, 1)) == 0) continue;
        for (int j = 0; j < 4; ++j) if (M[i + j] > k * F[i + j]) idx[c++] = i + j;
    }
#elif VIBE_SSE
    const __m128 kk = _mm_set1_ps(k);
    for (; i + 4 <= n && c + 4 <= maxIdx; i += 4) {
        const int mask = _mm_movemask_ps(_mm_cmpgt_ps(_mm_loadu_ps(M + i), _mm_mul_ps(_mm_loadu_ps(F + i), kk)));
        if (!mask) continue;
        for (int j = 0; j < 4; ++j) if (mask & (1 << j)) idx[c++] = i + j;
    }
#endif
    for (; i < n && c < maxIdx; ++i) if (M[i] > k * F[i]) idx[c++] = i;
    return c;
}

/** PFFFT's sizes: a multiple of 32 whose only factors are 2, 3 and 5 (the stricter real-transform rule, so one rule
 *  serves both). */
bool pffftSize(int n) {
    if (n < 32 || n % 32) return false;
    while (n % 2 == 0) n /= 2;
    while (n % 3 == 0) n /= 3;
    while (n % 5 == 0) n /= 5;
    return n == 1;
}
int nearestPffftSize(double target) {
    int best = 32; double bd = 1e9;
    for (int n = 32; n <= 65536; n += 32) {
        if (!pffftSize(n)) continue;
        const double d = std::fabs(std::log((double)n / target));
        if (d < bd) { bd = d; best = n; }
    }
    return best;
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
namespace {
constexpr int kGuard = 5;          // bins either side of the searched band, so a signal's neighbours always exist
constexpr int kHist = 192;         // ≈ 1 s of a signal's power, for the ghost test
constexpr int kRing = 24;          // marks kept for the two-means
constexpr int kGapRing = 40;       // gaps kept for the three-means (word gaps are rarer)
constexpr int kLanes = 4;
constexpr size_t kRunLog = 4096;   // a tentative signal's on/off runs, re-decoded when it is confirmed
/** ★★ THE FLOOR'S BIAS, MEASURED (cw_bench bias, pure noise, IQ and audio): the floor settles at a low quantile of the
 *  short average, this many times below the noise's mean. scripts/test-cw.sh checks it stays within ±0.5 dB. */
constexpr float kFloorBias = 1.53f;
/** ★★★ FOUR SPEED LANES, each a whole decoder with a FIXED smoothing (see Lane). Target WPM and the range each serves —
 *  they overlap, so a signal is always well inside one. */
struct LaneSpec { float wpm, lo, hi; };
constexpr LaneSpec kLaneSpec[kLanes] = { { 45, 30, 60 }, { 25, 16, 40 }, { 13, 8.5f, 21 }, { 7, 5, 11 } };

/** ★ The character assembler — the same rules live and when a tentative signal's runs are re-decoded. */
struct Assembler {
    unsigned code = 1; int nel = 0; bool overflow = false; float conf = 1.f;
    void element(bool dash) { if (nel < 9) { code = code * 2 + (dash ? 1u : 0u); ++nel; } else overflow = true; }
    /** The character so far, or nullptr when it is not one (and the state is cleared either way). */
    const char* take() {
        const char* t = (!overflow && nel > 0) ? morseText(code) : nullptr;
        code = 1; nel = 0; overflow = false; conf = 1.f;
        return t;
    }
};
}

struct Skimmer::Impl {
    Skimmer* owner = nullptr;     // ★ for its callbacks
    Config cfg;
    int N = 0, H = 0;             // FFT size, hop
    double binHz = 0, hopSec = 0;
    PFFFT_Setup* setup = nullptr;
    float *fin = nullptr, *fout = nullptr, *work = nullptr;
    std::vector<float> win;
    std::vector<float> in;        // the last N samples (complex: 2N floats)
    std::vector<float> stage;     // the next hop, filling
    int staged = 0;
    // ★ The work bins: shifted-spectrum indices [w0, w1); the searched ones are [w0+kGuard, w1-kGuard).
    int w0 = 0, w1 = 0, nw = 0;
    std::vector<float> P, S, M, F;
    std::vector<int> cand;
    Rates rates{};
    uint64_t hopN = 0;
    int64_t startMs = 0;
    int nextId = 1;
    uint64_t ghosts = 0, tentDropped = 0, broadband = 0;
    bool primed = false;           // ★ the trackers are seeded from the first full window's power, not from zero
    uint64_t warmEnd = 0;

    struct HoldOff { int bin; uint64_t until; };
    std::vector<HoldOff> hold;

    struct Run { uint16_t len; bool mark; };
    /** ★★★ ONE LANE = ONE COMPLETE DECODER AT ONE FIXED SMOOTHING. The smoothing that suits a signal depends on its
     *  speed, and the speed is learnt from what the smoothing lets through — so a single decoder that adapts its own
     *  smoothing has a trap either way: a weak signal's marks break up, the speed reads fast, the smoothing shortens, the
     *  marks break up more ("146 WPM"); a fast signal's gaps fill in, the speed reads slow, the smoothing lengthens, the
     *  gaps vanish. Fixed smoothing has no loop. Four lanes (45, 25, 13 and 7 WPM) each learn the speed inside their
     *  own range; the lane whose keying looks most like Morse is the one shown. A lane is a few dozen operations a hop,
     *  so four of them for each signal cost less than a twentieth of the FFT. */
    struct Lane {
        int L = 1, minRun = 1; float uLo = 1, uHi = 100, dotLane = 11.25f;   // ★ dotLane: the dot at the lane's own speed
        float sm[24] = {}; int smPos = 0; float smSum = 0;
        float markLvl = 0, mu = 1;     // ★ mu: the noise under it, this hop
        bool on = false; int run = 0, pend = 0;
        double onSum = 0, offSum = 0; uint32_t onN = 0, offN = 0;
        float unit = 11.25f, markSplit = 22.5f, charSplit = 22.5f, wordSplit = 56.f;
        float marks[kRing] = {}; int nMarks = 0, mPos = 0;
        float gaps[kGapRing] = {}; int nGaps = 0, gPos = 0;
        float dotC = 0, dashC = 0, markSpread = 1.f, gapRatio = 0; bool markSep = false;   // ★ the ring's clusters: STATS
        // ★★★ THE LIVE TIMING (human fists, 2026-10-25): five running centres, each followed element by element — the dot,
        //     the dash, and the element / character / word gaps — so the dash:dot ratio (2.5–4:1), the weighting and the
        //     spacing are each the sender's own, not 1:3:7 assumed. One odd element moves its centre at most 6 % (a
        //     clipped step), so a speed CHANGE is followed within a few characters and a fumble is not.
        float cDot = 11.25f, cDash = 33.75f, cG1 = 11.25f, cG3 = 33.75f, cG7 = 78.75f;
        bool seedM = false, seedG = false;
        int gapTick = 0, markTick = 0;
        // ★★ A NEW OVER RE-SYNCS: after a pause the first marks are held (not decoded) until there are enough to say the
        //    new speed, then decoded with it — an operator back at 30 WPM after 15 was read as all dots for a word.
        bool buffering = false; std::vector<Run> ov;
        Assembler as;
        bool wordOpen = false;         // a character has been emitted since the last word space
        std::vector<Run> runs;         // ★ while tentative: every committed run, for the re-decode
        std::string text, word;
        std::vector<std::string> words;
        unsigned chars = 0, valid = 0, elems = 0;   // ★ elems: in the valid characters — noise makes E and T, text ≈ 3
        float quality = 0, validRate = 0;
        float recentEpc = 2.5f;        // ★ elements per valid character, recently (a moving mean) — noise sinks to ≈ 1.5
        int etRun = 0;                 // ★ one-element characters (E, T) in a row — three is how a station's tail starts
        // ★ This OVER's characters (since the last pause), held while the signal is on hold (Sig::overHold)
        unsigned overChars = 0, overValid = 0, overElems = 0;
        std::string overText; std::vector<std::string> overWords;
        double contrast() const { return onN && offN ? (onSum / onN) / std::max(1e-9, offSum / offN) : 0.0; }
    };
    struct Sig {
        int id = 0, bin = 0, best = 1;
        bool confirmed = false;
        uint64_t born = 0, lastMark = 0, lastGhostCheck = 0;
        bool ghostChecked = false;
        float hist[kHist] = {}; int hpos = 0, hfill = 0;
        // ★ The power ±190 and ±375 Hz either side, over the same second — the broadband test (isNotCw).
        float probe[4][kHist] = {};
        float noise = 0;
        Lane lane[kLanes];
        std::map<std::string, int> callCount;
        std::map<std::string, uint64_t> callSpotted;
        std::string call, shown;        // ★ shown: the text sent to the listener (show), for the signals list
        float shownWpm = 0;             // ★ …and the speed it was read at
        int64_t firstMs = 0, lastMs = 0;
        uint64_t noisySince = 0;
        int pendingBest = -1;          // ★ a better lane, waiting for a word boundary to take over (chooseLane)
        /** ★★★ AN OVER STARTS ON HOLD (human fists: the reply is another operator, another speed). After a pause every lane
         *  decodes the new over but none is shown; once one lane has read it clearly (or after a few seconds) the BEST LANE
         *  FOR THIS OVER is chosen and its text so far shown in one piece — so a change of speed between overs, which can
         *  move a signal to another lane, never loses the start of the over to the lane that suited the last one. */
        bool overHold = false; uint64_t holdSince = 0;       // ★ a confirmed signal whose keying stopped looking like Morse, since when (0 = it does)
        const Lane& bestLane() const { return lane[best]; }
    };
    std::vector<std::unique_ptr<Sig>> sigs;
    std::deque<SignalView> gone;

    // ── geometry ──
    double offsetOf(double shiftedBin) const {
        return cfg.complexIn ? (shiftedBin - N / 2) * binHz : shiftedBin * binHz;
    }
    int64_t now() const { return startMs + (int64_t)std::llround((double)hopN * hopSec * 1000.0); }
    uint64_t hopsFor(double sec) const { return (uint64_t)std::max(1.0, sec / hopSec); }
    float dotHops(float wpm) const { return (float)(1.2 / wpm / hopSec); }

    void init(const Config& c) {
        cfg = c;
        N = nearestPffftSize(c.rate / c.binHz);
        H = N / 4;
        binHz = c.rate / N;
        hopSec = H / c.rate;
        setup = pffft_new_setup(N, c.complexIn ? PFFFT_COMPLEX : PFFFT_REAL);
        const size_t fl = (size_t)N * (c.complexIn ? 2 : 1);
        fin = (float*)pffft_aligned_malloc(fl * sizeof(float));
        fout = (float*)pffft_aligned_malloc(fl * sizeof(float));
        work = (float*)pffft_aligned_malloc(fl * sizeof(float));
        win.resize((size_t)N);
        // ★ Hann (periodic): sidelobes −31 dB falling 18 dB an octave — a 47 Hz bin with a main lobe of ±2 bins, so two
        //   signals 100 Hz apart are two peaks. Leakage further out is the ghost test's job, not the window's.
        for (int i = 0; i < N; ++i) win[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2.0 * M_PI * i / N);
        in.assign(fl, 0.0f);
        stage.assign((size_t)H * (c.complexIn ? 2 : 1), 0.0f);
        // ★ The searched band, in shifted-spectrum bins (complex: bin N/2 is 0 Hz; real: bin k is k·binHz).
        int s0, s1;
        if (c.complexIn) {
            s0 = (int)std::floor(c.loHz / binHz) + N / 2;
            s1 = (int)std::ceil(c.hiHz / binHz) + N / 2 + 1;
            const int edge = std::max(kGuard, N / 32);   // ★ never the outermost 3 % — the channel filter's roll-off
            s0 = std::max(s0, edge + kGuard); s1 = std::min(s1, N - edge - kGuard);
        } else {
            s0 = (int)std::floor(c.loHz / binHz);
            s1 = (int)std::ceil(c.hiHz / binHz) + 1;
            s0 = std::max(s0, 1 + kGuard); s1 = std::min(s1, N / 2 - kGuard);
        }
        if (s1 <= s0) s1 = s0 + 1;
        w0 = s0 - kGuard; w1 = s1 + kGuard; nw = w1 - w0;
        P.assign((size_t)nw, 0.0f); S = P; M = P; F = P;
        cand.assign((size_t)nw, 0);
        const double hz = 1.0 / hopSec;
        auto a = [&](double tauSec) { return (float)(1.0 - std::exp(-1.0 / (tauSec * hz))); };
        // ★ S ≈ 50 ms, M ≈ 0.4 s; the floor falls toward S over ≈ 0.6 s and rises over ≈ 20 s, never faster than 1 dB/s.
        rates.aS = a(0.05); rates.aM = a(0.4); rates.aD = a(0.6); rates.aU = a(20.0);
        rates.cU = (float)(std::pow(10.0, 1.0 / 10.0 / hz) - 1.0);
        rates.tiny = 1e-30f;
        startMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    ~Impl() {
        if (setup) pffft_destroy_setup(setup);
        if (fin) pffft_aligned_free(fin);
        if (fout) pffft_aligned_free(fout);
        if (work) pffft_aligned_free(work);
    }
    void initLane(Lane& l, const LaneSpec& sp, float markLvl) {
        const float dot = dotHops(sp.wpm);
        // ★★ THE MATCHED FILTER, ROUGHLY: the window already integrates ≈ 10 ms; the moving sum brings the total to about
        //    three quarters of the lane's dot — most of the noise out, the gaps left their depth.
        l.L = std::max(1, std::min(24, (int)std::lround((0.75 * dot * hopSec * 1000.0 - 10.0) / (hopSec * 1000.0))));
        l.minRun = std::max(1, (int)std::lround(0.35f * dot));   // ★ a run shorter than a third of a dot is a flicker
        l.uLo = dotHops(sp.hi); l.uHi = dotHops(sp.lo); l.dotLane = dot;
        l.unit = dot; l.markSplit = 2.0f * dot; l.charSplit = 2.0f * dot; l.wordSplit = 5.0f * dot;
        l.cDot = dot; l.cDash = 3 * dot; l.cG1 = dot; l.cG3 = 3 * dot; l.cG7 = 7 * dot;
        l.markLvl = markLvl;
        l.runs.reserve(128);
    }

    // ── the hop ──
    void doHop() {
        if (cfg.complexIn) {
            windowCplx(in.data(), win.data(), fin, N);
            pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
            // ★ Shifted bin s ↔ natural bin (s + N/2) mod N: two contiguous runs, two calls of the kernel.
            const int half = N / 2;
            const int a1 = std::min(w1, half);
            if (w0 < a1) power(fout + 2 * (w0 + half), P.data(), a1 - w0);
            const int b0 = std::max(w0, half);
            if (b0 < w1) power(fout + 2 * (b0 - half), P.data() + (b0 - w0), w1 - b0);
        } else {
            windowReal(in.data(), win.data(), fin, N);
            pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
            power(fout + 2 * w0, P.data(), nw);   // ★ w0 ≥ 1: bin 0's slot holds DC and Nyquist, never read
        }
        ++hopN;
        // ★★ WARM-UP. The first windows are mostly the zeros the buffer started with, so nothing is seeded from them: the
        //    trackers start at the first FULL window, and for the first second the floor follows the noise both ways at
        //    the same rate (its +1 dB a second cap is for keyed carriers, which there is no knowing yet). Seeded from a
        //    part-empty window, the floor began 20 dB low and every bin was a "signal" for half a minute.
        if (hopN < 4) return;
        if (!primed) {
            for (int i = 0; i < nw; ++i) { const float p = std::max(P[(size_t)i], 1e-30f); S[(size_t)i] = M[(size_t)i] = p; F[(size_t)i] = p / kFloorBias; }
            primed = true; warmEnd = hopN + hopsFor(1.0);
        }
        if (hopN < warmEnd) {
            Rates w = rates; w.aU = w.aD = rates.aD * 4.0f; w.cU = 1e9f; w.aM = std::max(rates.aM, 0.05f);
            track(P.data(), S.data(), M.data(), F.data(), nw, w);
            return;
        }
        track(P.data(), S.data(), M.data(), F.data(), nw, rates);
        for (auto& s : sigs) stepSignal(*s);
        if ((hopN & 7) == 0) scan();
        if ((hopN & 31) == 0) housekeeping();
    }

    /** μ: the noise's mean at bin b. ★ From the floor BESIDE it (±3–4 bins, outside a carrier's main lobe), averaged —
     *  its own floor is lifted a little by the signal, and one bin's floor alone is a noisy estimate. */
    float noiseAt(int b) const {
        const float own = F[(size_t)b];
        const float side = 0.25f * (F[(size_t)(b - 4)] + F[(size_t)(b - 3)] + F[(size_t)(b + 3)] + F[(size_t)(b + 4)]);
        return std::max(1e-30f, std::min(own, side) * kFloorBias);
    }
    bool nearSignal(int b, int within) const {
        for (auto& s : sigs) if (std::abs(s->bin - b) <= within) return true;
        return false;
    }
    void scan() {
        const int lo = kGuard, hi = nw - kGuard;
        // ★ The vector pre-test over every bin: long average over 1.6·(own floor·bias) — deliberately looser than the
        //   real test below (2.5·μ, ≈ 4 dB), because a carrier's own floor is lifted a little and μ is taken beside it.
        //   Only the few bins it passes are looked at one by one.
        const int nc = above(M.data() + lo, F.data() + lo, 1.3f * kFloorBias, hi - lo, cand.data(), (int)cand.size());
        for (int k = 0; k < nc; ++k) {
            const int b = cand[(size_t)k] + lo;
            const float m = M[(size_t)b];
            // ★ A local peak of the long average over ±2 bins: one carrier, one candidate (its main lobe merges).
            if (!(m >= M[(size_t)(b - 1)] && m > M[(size_t)(b + 1)] && m >= M[(size_t)(b - 2)] && m > M[(size_t)(b + 2)])) continue;
            if (m < 2.0f * noiseAt(b)) continue;
            if (nearSignal(b, 2)) continue;
            bool held = false;
            for (auto& h : hold) if (std::abs(h.bin - b) <= 1 && h.until > hopN) { held = true; break; }
            if (held) continue;
            if ((int)sigs.size() >= cfg.maxSignals) {
                // ★ Full: a clearly stronger newcomer takes the weakest TENTATIVE's place; confirmed signals stay.
                size_t weak = sigs.size(); float wm = 0;
                for (size_t i = 0; i < sigs.size(); ++i)
                    if (!sigs[i]->confirmed && (weak == sigs.size() || M[(size_t)sigs[i]->bin] < wm)) { weak = i; wm = M[(size_t)sigs[i]->bin]; }
                if (weak == sigs.size() || m < 2.0f * wm) continue;
                ++tentDropped; dropSig(weak, false);
            }
            auto s = std::make_unique<Sig>();
            s->id = nextId++; s->bin = b; s->born = hopN; s->lastMark = hopN;
            s->noise = noiseAt(b);
            s->firstMs = s->lastMs = now();
            for (int i = 0; i < kLanes; ++i) initLane(s->lane[i], kLaneSpec[i], 2.0f * m);
            sigs.push_back(std::move(s));
        }
    }

    // ── one signal, one hop ──
    void stepSignal(Sig& s) {
        const float p = P[(size_t)s.bin];
        s.hist[s.hpos] = p;
        for (int k = 0; k < 4; ++k) s.probe[k][s.hpos] = P[(size_t)probeBin(s.bin, k)];
        s.hpos = (s.hpos + 1) % kHist; if (s.hfill < kHist) ++s.hfill;
        const float mu = noiseAt(s.bin);
        s.noise = mu;
        bool anyOn = false;
        for (int i = 0; i < kLanes; ++i) { stepLane(s, i, p, mu); anyOn |= s.lane[i].on; }
        if (anyOn) {
            if (s.overHold && !s.holdSince) s.holdSince = hopN;
            s.lastMark = hopN;
        } else if (s.confirmed && !s.overHold && hopN - s.lastMark == hopsFor(1.2)) beginHold(s);
        // ★★ A SHOWN SIGNAL THAT STARTS TO READ LIKE NOISE IS HELD TOO: when its station stops, what is left in the bin
        //    still "keys" (noise crosses the threshold now and then) and printed "E E T E E" for the 8 s it took to be
        //    dropped — on a real band every station left that tail. Held, it is shown only if it turns back into Morse.
        //    ★ Three E/T in a row starts it (text almost never has them; the moving mean took 5–8 characters).
        //    ★ …or the lane shown reading a speed outside its own range (noise in the 45 WPM lane reads "107 WPM").
        if (s.confirmed && !s.overHold && (s.bestLane().recentEpc < 2.0f || s.bestLane().etRun >= 3 || outOfRange(s.bestLane()))
            && atWordBoundary(s.bestLane())) beginHold(s);
        if (s.overHold && s.holdSince) {
            unsigned best = 0; for (const Lane& l : s.lane) best = std::max(best, l.overValid);
            if (best >= 10 || hopN - s.holdSince > hopsFor(4.0)) endHold(s);
        }
        if (s.pendingBest >= 0 && atWordBoundary(s.lane[s.best]) && atWordBoundary(s.lane[s.pendingBest])) {
            s.best = s.pendingBest; s.pendingBest = -1;
        }
    }
    void stepLane(Sig& s, int li, float p, float mu) {
        Lane& l = s.lane[li];
        l.smSum += p - l.sm[l.smPos]; l.sm[l.smPos] = p; l.smPos = (l.smPos + 1) % l.L;
        const float e = std::max(0.0f, l.smSum / (float)l.L);
        // ★★ THE MARK LEVEL IS THE MARKS' MEAN, NOT THEIR PEAKS: while keyed it follows the envelope over ≈ 0.25 s (so QSB
        //    is followed, up and down, mark by mark); a mark clearly stronger than it lifts it at once (a new signal, a
        //    fade coming back); between overs it sinks toward the noise over ≈ 4 s. Tracking the PEAKS put the threshold
        //    high in the noise on the mark, and a dash split in two at +15 dB.
        if (l.on) l.markLvl += (float)(hopSec / 0.25) * (e - l.markLvl);
        else if (e > 1.5f * l.markLvl) l.markLvl += 0.3f * (e - l.markLvl);
        else l.markLvl -= (l.markLvl - mu) * (float)(hopSec / 4.0);
        if (l.markLvl < mu) l.markLvl = mu;
        l.mu = mu;
        // ★★★ THE THRESHOLD IS HALF THE MARK'S OWN POWER above the noise (μ + (mark − μ)/2) — where a smoothed rectangular
        //     mark crosses at its TRUE length. A threshold at the dB midpoint sits far lower on a strong signal and reads
        //     every mark long and every gap short — which at 40 WPM eats the gaps entirely.
        // ★★ …and never below what the NOISE alone reaches at this lane's smoothing (≈ 3σ of an L-hop average, its hops
        //    half-overlapped): when a station stops, its mark level takes seconds to sink and the threshold with it — which
        //    used to print a tail of noise ("E E T E E") after every over.
        const float floorK = 1.0f + 3.0f / std::sqrt(std::max(1.0f, 0.5f * (float)l.L));
        bool raw = false;
        if (l.markLvl > 1.4f * floorK * mu) {
            const float thr = std::max(mu + 0.5f * (l.markLvl - mu), floorK * mu);
            raw = l.on ? e > thr * 0.92f : e > thr * 1.08f;     // ★ hysteresis, ±0.35 dB — the debounce does the rest
        }
        if (l.on) { l.onSum += e / mu; ++l.onN; } else { l.offSum += e / mu; ++l.offN; }
        if (l.onN + l.offN > 6000) { l.onSum *= 0.5; l.offSum *= 0.5; l.onN /= 2; l.offN /= 2; }   // ★ a ~30 s memory
        // ★ Debounce: a run shorter than ≈ a third of the lane's dot is absorbed into the one around it.
        if (raw == l.on) { l.run += l.pend + 1; l.pend = 0; }
        else if (++l.pend >= l.minRun) {
            if (l.on) onMark(s, li, l.run); else onGap(s, li, l.run);
            l.on = raw; l.run = l.pend; l.pend = 0;
        }
        if (!l.on) {
            // ★ A held over-start ends at its first word gap (or a pause): decode it with the speed it showed.
            //   ★ Not at the first long-looking gap with a handful of marks: at the OLD speed's word split, a slower
            //     operator's character gap already looks like a word — six marks first, or a real pause.
            if (l.buffering && !l.ov.empty() && ((l.run > l.wordSplit && countMarks(l.ov) >= 6) || l.run > (int)hopsFor(1.2)))
                flushOver(s, li);
            // ★ Characters and word spaces are decided WHILE the gap runs, so text arrives without waiting for a mark.
            if (l.as.nel > 0 && l.run > l.charSplit) endChar(s, li, (float)l.run);
            if (l.wordOpen && l.run > l.wordSplit) endWord(s, li);
        }
    }
    static float margin(float len, float split, float spread) {
        // 0 at the split, 1 at a cluster centre — how sure the classification was
        return std::min(1.0f, std::fabs(std::log(std::max(len, 0.5f) / split)) / std::max(0.2f, spread));
    }
    static void twoMeans(const float* v, int n, float& lo, float& hi, int& nLo, int& nHi, float* rms = nullptr) {
        lo = 1e9f; hi = -1e9f;
        for (int i = 0; i < n; ++i) { lo = std::min(lo, v[i]); hi = std::max(hi, v[i]); }
        nLo = nHi = 0;
        for (int it = 0; it < 8; ++it) {
            const float t = 0.5f * (lo + hi);
            float sl = 0, sh = 0; nLo = nHi = 0;
            for (int i = 0; i < n; ++i) { if (v[i] < t) { sl += v[i]; ++nLo; } else { sh += v[i]; ++nHi; } }
            const float nl = nLo ? sl / nLo : lo, nh = nHi ? sh / nHi : hi;
            if (nl == lo && nh == hi) break;
            lo = nl; hi = nh;
        }
        if (rms) {
            const float t = 0.5f * (lo + hi); double q = 0;
            for (int i = 0; i < n; ++i) { const float d = v[i] - (v[i] < t ? lo : hi); q += d * d; }
            *rms = n ? (float)std::sqrt(q / n) : 1.f;
        }
    }
    /** Three clusters of log gap length, started at 1, 3 and 7 units (or at `init`, the live centres). Returns the
     *  clusters' squared spread (log units), so two starts can be compared. */
    static float threeMeans(const float* v, int n, float logUnit, float c[3], int cnt[3], const float* init = nullptr) {
        c[0] = logUnit; c[1] = logUnit + std::log(3.0f); c[2] = logUnit + std::log(7.0f);
        if (init) { c[0] = init[0]; c[1] = init[1]; c[2] = init[2]; }
        for (int it = 0; it < 8; ++it) {
            float sum[3] = { 0, 0, 0 }; cnt[0] = cnt[1] = cnt[2] = 0;
            const float t01 = 0.5f * (c[0] + c[1]), t12 = 0.5f * (c[1] + c[2]);
            for (int i = 0; i < n; ++i) { const int k = v[i] < t01 ? 0 : v[i] < t12 ? 1 : 2; sum[k] += v[i]; ++cnt[k]; }
            bool moved = false;
            for (int k = 0; k < 3; ++k) if (cnt[k]) { const float nc = sum[k] / cnt[k]; moved |= nc != c[k]; c[k] = nc; }
            if (!moved) break;
        }
        const float t01 = 0.5f * (c[0] + c[1]), t12 = 0.5f * (c[1] + c[2]);
        float q = 0;
        for (int i = 0; i < n; ++i) { const float d = v[i] - c[v[i] < t01 ? 0 : v[i] < t12 ? 1 : 2]; q += d * d; }
        return q;
    }
    /** ★★★ THE GAP CLUSTERS, FROM TWO STARTS: the live centres, and 1 / 3 / 7 of the MARKS' dot. Started only from the
     *  live centres, a wrong start is a trap that holds — a lane that once took its element gaps for character gaps
     *  (seeded from a debounce sliver and a pause fragment) printed every element as a letter for the rest of the QSO
     *  ("G4ABC" → "TTE EIET"). The dot comes from the marks, which the gaps cannot corrupt. The live result is kept
     *  unless it is IMPOSSIBLE — an element-gap centre outside 0.4–2.2 dots, or a character-gap centre under 1.7 dots
     *  (a 30 % heavy fist still leaves 2.1) — or the dot-anchored one is clearly tighter. (Tightness alone cannot judge
     *  them: k-means always prefers using all three clusters, so splitting the element gaps in two "wins".) Only gaps the keying could have made are used: none shorter than a third of a dot (a
     *  sliver the debounce let through) and none past the cap (a pause, or a fragment of one cut by a noise blip). */
    float gapClusters(const Lane& l, float cap, const float* live, float c[3], int k[3]) const {
        float v[kGapRing]; int n = 0;
        const float lo = std::log(0.35f * l.cDot), hi = std::log(cap);
        for (int i = 0; i < std::min(l.nGaps, kGapRing); ++i) if (l.gaps[i] >= lo && l.gaps[i] < hi) v[n++] = l.gaps[i];
        if (n < 6) { k[0] = k[1] = k[2] = 0; return -1; }
        float ca[3]; int ka[3];
        const float qa = threeMeans(v, n, std::log(l.cDot), ca, ka);
        if (!live) { std::memcpy(c, ca, sizeof ca); std::memcpy(k, ka, sizeof ka); return qa; }
        const float ql = threeMeans(v, n, 0, c, k, live);
        const float d = std::log(l.cDot);
        auto bad = [&](const float* cc, const int* kk) {
            return (kk[0] && (cc[0] < d + std::log(0.4f) || cc[0] > d + std::log(2.2f))) || (kk[1] && cc[1] < d + std::log(1.7f));
        };
        const bool impossible = bad(c, k), anchoredBad = bad(ca, ka);
        // ★ Both impossible (the element gaps split in two either way): no update — splits() holds the centres apart.
        if (impossible && anchoredBad) { k[0] = k[1] = k[2] = 0; return ql; }
        // ★ A tie keeps the live centres (they carry Farnsworth and a fist's own spacing, which a 1:3:7 start can miss)
        if (impossible || (qa < 0.8f * ql && !anchoredBad)) { std::memcpy(c, ca, sizeof ca); std::memcpy(k, ka, sizeof ka); return qa; }
        return ql;
    }
    /** ★ The ring's statistics (what morseLike and laneScore judge), and the live centres' first values from them. */
    void retime(Lane& l) {
        if (l.nMarks >= 4) {
            float lo, hi; int nl, nh;
            twoMeans(l.marks, std::min(l.nMarks, kRing), lo, hi, nl, nh, &l.markSpread);
            const float ratio = std::exp(hi - lo);
            // ★ Noise thresholded at its median clusters too — at about 2.3:1. Morse is 2.5–4:1, more as measured.
            l.markSep = nl > 0 && nh > 0 && ratio >= 2.4f && ratio <= 6.0f && l.markSpread <= 0.45f;
            if (l.markSep) { l.dotC = std::exp(lo); l.dashC = std::exp(hi); }
            if (!l.seedM && l.markSep && l.nMarks >= 6) { l.cDot = l.dotC; l.cDash = l.dashC; l.seedM = true; }
        }
        if (l.nGaps >= 4) {
            float a, b; int na, nb; twoMeans(l.gaps, std::min(l.nGaps, kGapRing), a, b, na, nb);
            l.gapRatio = std::exp(b - a);
        }
        if (!l.seedG && l.seedM && l.nGaps >= 10) {
            float c[3]; int n[3];
            if (gapClusters(l, 40.0f * l.cDot, nullptr, c, n) < 0) { splits(l); return; }
            if (n[0]) l.cG1 = std::exp(c[0]);
            // ★ Text has more character gaps than word gaps. A "character" cluster left EMPTY is the character gaps
            //   stretched into the word one (Farnsworth: 25 WPM letters with 12 WPM spacing leaves 11-dot character gaps).
            //   ★ Empty, not merely outnumbered: one stray pause fragment outnumbering one character gap is not Farnsworth.
            if (n[1] == 0 && n[2] > 0) { l.cG3 = std::exp(c[2]); l.cG7 = 2.33f * l.cG3; }
            else {
                if (n[1]) l.cG3 = std::exp(c[1]);
                l.cG7 = n[2] ? std::exp(c[2]) : std::max(2.33f * l.cG3, 6.0f * l.cG1);
            }
            l.seedG = true;
        }
        splits(l);
    }
    /** ★ The decisions from the live centres: each split the geometric mean of its two neighbours (so a 4:1 fist and a
     *  2.5:1 fist are each split between their OWN dot and dash), the centres kept in order. */
    void splits(Lane& l) {
        const double hz = 1.0 / hopSec;
        const float dMin = (float)(1.2 / 70.0 * hz) * 0.7f, dMax = (float)(1.2 / 4.0 * hz) * 1.5f;
        l.cDot = std::max(dMin, std::min(dMax, l.cDot));
        l.cDash = std::max(1.8f * l.cDot, std::min(6.0f * l.cDot, l.cDash));
        l.cG1 = std::max(0.4f * l.cDot, std::min(2.5f * l.cDot, l.cG1));
        l.cG3 = std::max(std::max(1.6f * l.cG1, 1.7f * l.cDot), std::min(12.0f * l.cDot, l.cG3));
        // ★ A word gap is at least twice a character gap (7:3 by the book; Farnsworth stretches both alike) — without this
        //   floor, character gaps read as word gaps taught the word centre to be one, and every letter became a word.
        // ★★ …and at most 3.5 times one. The book says 7:3 and Farnsworth stretches both alike; a centre far above that
        //    has been pulled by pieces of the pauses between overs (a noise blip cuts a 1.5 s pause into word-sized parts),
        //    and its split then swallows every real word gap ("CQCQDEG4ABC"). An operator who pauses longer between words
        //    is still read: the split is only ≈ 1.9 character gaps.
        l.cG7 = std::max(2.0f * l.cG3, std::min(std::min(30.0f * l.cDot, 3.5f * l.cG3), l.cG7));
        l.markSplit = std::sqrt(l.cDot * l.cDash);
        l.charSplit = std::sqrt(l.cG1 * l.cG3);
        l.wordSplit = std::sqrt(l.cG3 * l.cG7);
        // ★★ THE UNIT (the speed shown, and the lane's range): the mean of the dot and the element gap — a threshold that
        //    ends a mark early starts its gap early by the same time, and a heavy or light fist moves them oppositely,
        //    so the mean is the sender's own unit whatever either does alone.
        l.unit = 0.5f * (l.cDot + l.cG1);
    }
    /** One element's step toward its centre, clipped to 30 % of the centre (× the rate) — at most 6 % a time. */
    static void follow(float& c, float len, float a) { c += a * std::max(-0.3f * c, std::min(0.3f * c, len - c)); }
    /** ★★★ HOW FAST THE CENTRES MOVE, BY HOW STEADY THE FIST IS. A keyer's elements (spread ≈ 0.05) are each a true
     *  sample of its speed, so the centres can chase them; a straight key at ±30 % puts one element in ten across the
     *  split, and every misread one drags its class the wrong way — followed at full rate, the centres wandered ±30 %
     *  over an over and doubled the errors the fist itself causes (cw_bench fists: the timing floor beside the CER).
     *  So the steps — and the re-centrings — shrink as the marks' spread grows, to a quarter at ±30 %. A real change
     *  of speed is still followed: the marks' re-centring and an over's re-sync see it in a dozen elements. */
    static float steady(const Lane& l) { return std::max(0.25f, std::min(1.0f, 0.10f / std::max(1e-3f, l.markSpread))); }
    static void blend(float& c, float target, float w) { c = std::exp(std::log(c) + w * (std::log(target) - std::log(c))); }
    void logRun(const Sig& s, Lane& l, int len, bool mark) {
        if (s.confirmed || l.runs.size() >= kRunLog) return;
        l.runs.push_back({ (uint16_t)std::min(len, 65535), mark });
    }
    void onMark(Sig& s, int li, int len) {
        Lane& l = s.lane[li];
        if (len <= 0) return;
        logRun(s, l, len, true);
        const float L = (float)len;
        l.marks[l.mPos] = std::log(L); l.mPos = (l.mPos + 1) % kRing; ++l.nMarks;
        retime(l);
        if (l.buffering) { l.ov.push_back({ (uint16_t)std::min(len, 65535), true }); if (countMarks(l.ov) >= 10) flushOver(s, li); return; }
        markElement(l, L);
    }
    void markElement(Lane& l, float L) {
        const bool dash = L > l.markSplit;
        const float spread = 0.5f * std::log(l.cDash / l.cDot);
        l.as.conf = std::min(l.as.conf, margin(L, l.markSplit, spread));
        // ★ A mark far longer than a dash is not Morse (a carrier, a tune-up): the character is spoilt.
        if (L > 2.5f * l.cDash) l.as.overflow = true;
        else if (l.seedM) {
            // ★★ A SPEED CHANGE MOVES EVERYTHING: the gaps are scaled by half the mark's own move (√f), so an operator
            //    speeding up has their spacing followed at once — not only after each gap has been misread once. (The
            //    gaps' own steps still set their RATIOS: Farnsworth, long character gaps, a fist's own spacing.)
            float& c = dash ? l.cDash : l.cDot;
            const float before = c;
            follow(c, L, 0.2f * steady(l));
            // ★ …and the marks re-centred every 8 the same way (two-means over the last 24, from the live centres).
            if (++l.markTick % 8 == 0 && l.nMarks >= 12) {
                const int n = std::min(l.nMarks, kRing);
                float lo = std::log(l.cDot), hi = std::log(l.cDash);
                for (int it = 0; it < 6; ++it) {
                    const float t = 0.5f * (lo + hi); float sl = 0, sh = 0; int nl = 0, nh = 0;
                    for (int i = 0; i < n; ++i) { if (l.marks[i] < t) { sl += l.marks[i]; ++nl; } else { sh += l.marks[i]; ++nh; } }
                    if (nl) lo = sl / nl;
                    if (nh) hi = sh / nh;
                }
                if (hi - lo > std::log(1.8f)) { const float w = 0.3f + 0.7f * steady(l); blend(l.cDot, std::exp(lo), w); blend(l.cDash, std::exp(hi), w); }
            }
            const float f = std::sqrt(c / before);
            l.cG1 *= f; l.cG3 *= f; l.cG7 *= f;
            splits(l);
        }
        l.as.element(dash);
    }
    void onGap(Sig& s, int li, int len) {
        Lane& l = s.lane[li];
        if (len <= 0) return;
        logRun(s, l, len, false);
        const float L = (float)len;
        // ★ Long pauses (between overs) would drag the gap clusters away from the keying; only gaps up to 30 units count (Farnsworth word gaps reach 27).
        if (L >= 0.35f * l.cDot && (l.seedG ? L < 1.5f * l.cG7 : L < 30.0f * l.unit)) { l.gaps[l.gPos] = std::log(L); l.gPos = (l.gPos + 1) % kGapRing; ++l.nGaps; }
        if (l.buffering) { l.ov.push_back({ (uint16_t)std::min(len, 65535), false }); return; }
        gapElement(l, L);
        // ★ A PAUSE — the end of an over: what follows may be another speed (a reply, a different fist). Hold it.
        if (l.seedM && L > std::max(2.5f * l.cG7, (float)(1.2 / hopSec))) { l.buffering = true; l.ov.clear(); }
        // the character itself ended inside stepLane (the gap passed charSplit while running)
    }
    void gapElement(Lane& l, float L) {
        if (!l.seedG) return;
        // ★★ RE-CENTRED FROM THE RECENT GAPS every 8: three-means over the last 40, started at the live centres — so a
        //    centre a run of misread gaps has dragged (a long character gap read as a word, a word as a pause) is put back
        //    where the sender's gaps really are. Between re-centrings each gap nudges its own class (below).
        if (++l.gapTick % 8 == 0 && l.nGaps >= 12) {
            float init[3] = { std::log(l.cG1), std::log(l.cG3), std::log(l.cG7) }, c[3]; int k[3];
            gapClusters(l, 1.5f * l.cG7, init, c, k);
            const float w = 0.3f + 0.7f * steady(l);
            if (k[0] >= 2) blend(l.cG1, std::exp(c[0]), w);
            // ★ The middle cluster EMPTY and the top one not: those are the character gaps (long, or Farnsworth) and the
            //   word gaps are beyond the cap — make room for them, and the next re-centring sees all three.
            if (k[1] == 0 && k[2] >= 3) { l.cG3 = std::exp(c[2]); l.cG7 = std::max(l.cG7, 2.33f * l.cG3); k[2] = 0; }
            if (k[1] >= 2) blend(l.cG3, std::exp(c[1]), w);
            // ★ The word centre only from three or more word gaps — fewer, and a run of long character gaps could take it
            //   over. Left stale, though, it lets word gaps join the character cluster and drag it up.
            if (k[2] >= 3) blend(l.cG7, std::exp(c[2]), w);
        }
        const float r = steady(l);
        if (L < l.charSplit) follow(l.cG1, L, 0.2f * r);
        else if (L < l.wordSplit) follow(l.cG3, L, 0.15f * r);
        else if (L > 1.8f * l.cG3 && L < 2.0f * l.cG7) follow(l.cG7, L, 0.1f * r);   // ★ never pulled down by a long character gap
        splits(l);
    }
    static int countMarks(const std::vector<Run>& v) { int n = 0; for (auto& r : v) n += r.mark; return n; }
    /** ★★ Decode a held over-start: its marks say its speed (two clusters → the new dot and dash; the gaps scale with
     *  them), then every held run is decoded with it, exactly as live. */
    void flushOver(Sig& s, int li) {
        Lane& l = s.lane[li];
        l.buffering = false;
        std::vector<Run> ov; ov.swap(l.ov);
        float v[64]; int n = 0;
        for (auto& r : ov) if (r.mark && n < 64) v[n++] = std::log((float)r.len);
        if (n >= 4) {
            float lo, hi; int nl, nh, dummy; float rms; (void)dummy;
            twoMeans(v, n, lo, hi, nl, nh, &rms);
            const float ratio = std::exp(hi - lo);
            if (nl > 0 && nh > 0 && ratio >= 2.2f && ratio <= 6.0f && rms <= 0.4f) {
                const float k = std::exp(lo) / l.cDot;
                // ★ A real change of speed — not this fist's ordinary scatter: beyond ±15 %, and beyond what ten marks of
                //   THIS spread could show by chance (a ±30 % straight key's over-start reads ±12 % from its own jitter).
                const float need = std::max(0.15f, 2.5f * rms / std::sqrt((float)std::max(1, std::min(nl, nh))));
                if (std::fabs(std::log(k)) > need) {
                    l.cDot = std::exp(lo); l.cDash = std::exp(hi);
                    l.cG1 *= k; l.cG3 *= k; l.cG7 *= k;
                    // ★ …and the history with it, or the next re-centring would drag the centres back to the last over's speed
                    //   — the OLDER entries only: this over's own marks and gaps are already in the rings at their true length.
                    const float lk = std::log(k);
                    int ovGaps = 0; for (auto& r : ov) ovGaps += !r.mark;
                    for (int i = 0, m = std::min(l.nGaps, kGapRing), keep = std::min(ovGaps, m); i < m - keep; ++i)
                        l.gaps[((l.gPos - keep - 1 - i) % kGapRing + kGapRing) % kGapRing] += lk;
                    for (int i = 0, m = std::min(l.nMarks, kRing), keep = std::min(n, m); i < m - keep; ++i)
                        l.marks[((l.mPos - keep - 1 - i) % kRing + kRing) % kRing] += lk;
                    splits(l);
                }
            }
        }
        for (const Run& r : ov) {
            if (r.mark) { markElement(l, (float)r.len); continue; }
            const float L = (float)r.len;
            if (l.as.nel > 0 && L > l.charSplit) endChar(s, li, L);
            if (l.wordOpen && L > l.wordSplit) endWord(s, li);
            gapElement(l, L);
        }
    }
    void endChar(Sig& s, int li, float gapLen) {
        Lane& l = s.lane[li];
        const float c = std::min(l.as.conf, margin(gapLen, l.charSplit, 0.5f));
        const int nel = l.as.nel;
        const char* t = l.as.take();
        ++l.chars; ++l.overChars;
        // ★★ QUALITY = how many characters are real codes (a running mean over the first 16, then a moving one) × how
        //    tight the timing is (the marks' spread about their dot and dash: a keyer ≈ 0.05, a good fist 0.15–0.25, noise
        //    ≥ 0.35). High / medium / low / poor at 0.75 / 0.5 / 0.3.
        (void)c;
        l.validRate += (l.chars <= 16 ? 1.0f / (float)l.chars : 0.06f) * ((t ? 1.0f : 0.0f) - l.validRate);
        const float tight = std::max(0.4f, std::min(1.0f, 1.0f - (l.markSpread - 0.12f) * 1.5f));
        l.quality = l.validRate * tight;
        if (!t) return;
        ++l.valid; l.elems += (unsigned)nel;
        ++l.overValid; l.overElems += (unsigned)nel;
        l.recentEpc += 0.1f * ((float)nel - l.recentEpc);
        l.etRun = nel == 1 ? l.etRun + 1 : 0;
        if (li == s.best) s.lastMs = now();
        l.word += t;
        l.wordOpen = true;
        emit(s, li, t);
    }
    void endWord(Sig& s, int li) {
        Lane& l = s.lane[li];
        l.wordOpen = false;
        emit(s, li, " ");
        if (!l.word.empty()) {
            l.words.push_back(l.word);
            if (l.words.size() > 8) l.words.erase(l.words.begin());
            l.word.clear();
            if (s.overHold) l.overWords.push_back(l.words.back());
            else if (li == s.best) checkCall(s);
        }
    }
    static void appendText(Lane& l, const std::string& t) {
        l.text += t;
        if (l.text.size() > 400) l.text.erase(0, l.text.size() - 300);
    }
    /** ★★ What the listener SAW: the text sent, kept for the signals list. It used to be the shown lane's own history —
     *  so after a hand-over, or a hold won by another lane, the list showed that lane's past (often its noise), not the
     *  text that had been on the screen. */
    void show(Sig& s, const std::string& t) {
        s.shown += t;
        s.shownWpm = (float)(1.2 / (s.bestLane().unit * hopSec));
        if (s.shown.size() > 400) s.shown.erase(0, s.shown.size() - 300);
        if (owner->onText) owner->onText(s.id, preciseOffset(s), t);
    }
    void emit(Sig& s, int li, const char* t) {
        Lane& l = s.lane[li];
        // ★ Held text is in nobody's view until the hold ends and keeps it (endHold) — the signals list shows l.text,
        //   and a held noise tail must not reach it either.
        if (s.overHold) { l.overText += t; return; }
        appendText(l, t);
        if (s.confirmed && li == s.best) show(s, t);
    }
    /** ★★ CONFIRMED — and the best lane's text so far DECODED AGAIN from its runs with the timing learnt since: the first
     *  seconds of a signal were read while its speed was still a guess, and they are usually the CQ and the call. */
    void confirm(Sig& s) {
        s.confirmed = true;
        Lane& l = s.lane[s.best];
        std::string text, word; std::vector<std::string> words;
        Assembler as; bool open = false;
        s.callCount.clear();
        auto endWordR = [&] {
            open = false; text += ' ';
            if (!word.empty()) { words.push_back(word); if (words.size() > 8) words.erase(words.begin()); word.clear();
                                 l.words = words; checkCall(s); }
        };
        for (const Run& r : l.runs) {
            if (r.mark) { if (r.len > 2.5f * l.cDash) as.overflow = true; as.element((float)r.len > l.markSplit); continue; }
            if (as.nel > 0 && r.len > l.charSplit) { if (const char* t = as.take()) { text += t; word += t; open = true; } }
            if (open && r.len > l.wordSplit) endWordR();
        }
        // ★ What follows is decoded live from the state at the end of the log: the character in progress (if the current
        //   gap or mark is still running) is the re-decoder's.
        l.as = as; l.word = word; l.words = words; l.wordOpen = open;
        l.buffering = false; l.ov.clear();
        if (text.size() > 400) text.erase(0, text.size() - 300);
        l.text = text;
        for (auto& x : s.lane) { x.runs.clear(); x.runs.shrink_to_fit(); }
        if (!text.empty()) show(s, text);
    }
    /** How much a lane's keying looks like Morse at its speed — what chooses the lane shown. */
    float laneScore(const Lane& l) const {
        if (l.valid < 3) return 0.0f;
        float sc = l.quality * ((float)l.valid / (float)std::max(1u, l.chars));
        if (!l.markSep) sc *= 0.6f;
        sc *= std::min(1.0f, (float)l.elems / (2.5f * (float)l.valid));
        if (l.unit < l.uLo * 0.8f || l.unit > l.uHi * 1.25f) sc *= 0.5f;   // ★ outside its range: the wrong lane
        return sc * smoothingFit(l);
    }
    /** ★★ A LANE SLOWER THAN THE SIGNAL IS MARKED DOWN. Its smoothing (three quarters of ITS dot) is then longer than the
     *  sender's shortest gaps, and a straight key's short element gap (0.6 of a dot at ±20 %) fills in: two dots merge into
     *  a dash and every character holding them is still a valid code (D → M), so nothing else in the score sees it. At
     *  18 WPM the 13 and 25 WPM lanes both read the speed right; the slower one doubled the errors. A faster lane only
     *  lets more noise through, which its own score does see. */
    static float smoothingFit(const Lane& l) {
        // ★ …in proportion to how ragged the fist is: a keyer's gaps are all a full dot, so a slower lane's extra
        //   smoothing costs it nothing.
        if (l.unit >= 0.85f * l.dotLane) return 1.0f;
        // ★ …and only on a signal well clear of the noise (its marks 12 → 18 dB over the bin's noise, ≈ −5 → +1 dB in
        //   2.5 kHz): on a weak one the spread is the NOISE's, and there the slower lane's smoothing is the whole point
        //   (20 WPM at −8 dB: 7 % CER against 30 % with the penalty).
        const float clear = std::max(0.0f, std::min(1.0f, (l.markLvl / std::max(1e-12f, l.mu) - 16.0f) / 47.0f));
        return 1.0f - 0.15f * clear * std::max(0.0f, std::min(1.0f, (l.markSpread - 0.10f) / 0.08f));
    }
    /** ★★ IS IT MORSE? Noise thresholded at its own median makes "marks" too — so a tentative signal is shown only when
     *  its keying has Morse's shape: two tight clusters of mark length about three to one, a speed well inside the lane,
     *  a clear on/off contrast, characters that are real codes and not just E and T. */
    bool morseLike(const Lane& l) const {
        if (l.nMarks < 20 || l.valid < 8 || !l.markSep) return false;
        const float ratio = l.dashC / l.dotC;
        if (ratio < 2.4f || ratio > 6.0f || l.markSpread > 0.45f) return false;
        if (l.unit < l.uLo * 0.8f || l.unit > l.uHi * 1.25f) return false;
        if ((float)l.elems < 2.2f * (float)l.valid || l.recentEpc < 2.2f) return false;   // ★ noise decodes as E and T; text ≈ 3
        if (l.contrast() < 3.0) return false;     // ★ PSK31 and other narrow data modes key softly; CW goes to the noise
        return (float)l.valid >= 0.8f * (float)l.chars;
    }
    void checkCall(Sig& s) {
        std::string call, how;
        if (!identify(s.lane[s.best].words, call, how)) return;
        int& n = s.callCount[call];
        ++n;
        if (s.callCount.size() > 16) s.callCount.erase(s.callCount.begin());
        if (n < cfg.spotRepeats || !s.confirmed) return;
        auto it = s.callSpotted.find(call);
        if (it != s.callSpotted.end() && hopN - it->second < hopsFor(cfg.respotSec)) return;
        s.callSpotted[call] = hopN;
        s.call = call;
        if (owner->onSpot) {
            Spot sp; sp.call = call; sp.offsetHz = preciseOffset(s); sp.wpm = wpmOf(s); sp.snrDb = snrOf(s);
            sp.how = how; sp.utcMs = now(); sp.signalId = s.id;
            owner->onSpot(sp);
        }
    }
    /** ★ The speed shown: PARIS, 1.2 s ÷ the unit — the mean of the live dot and element gap (splits). */
    /** ★ The speed of the text SHOWN (taken as it is sent), not of whatever the shown lane reads now: after a station
     *  stops, its fastest lane reads the noise as 90+ WPM, and the list said so. */
    float wpmOf(const Sig& s) const { return s.shownWpm > 0 ? s.shownWpm : (float)(1.2 / (s.bestLane().unit * hopSec)); }
    float snrOf(const Sig& s) const {
        return (float)(10.0 * std::log10(std::max(1e-3, (double)s.bestLane().markLvl / std::max(1e-30f, s.noise) - 1.0)));
    }
    double preciseOffset(const Sig& s) const {
        // ★ Parabolic interpolation of the long average's log over the peak and its neighbours: ≈ 5 Hz, not ±23.
        const double a = std::log(std::max(1e-30f, M[(size_t)(s.bin - 1)])), b = std::log(std::max(1e-30f, M[(size_t)s.bin])),
                     c = std::log(std::max(1e-30f, M[(size_t)(s.bin + 1)]));
        const double den = a - 2 * b + c;
        double d = den < 0 ? 0.5 * (a - c) / den : 0.0;
        d = std::max(-0.5, std::min(0.5, d));
        return offsetOf(w0 + s.bin + d);
    }

    // ── housekeeping: drift, lanes, merges, ghosts, endings — every 32 hops ──
    static float corr(const float* a, const float* b, int n) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        for (int i = 0; i < n; ++i) { sa += a[i]; sb += b[i]; saa += a[i] * (double)a[i]; sbb += b[i] * (double)b[i]; sab += a[i] * (double)b[i]; }
        const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
        if (va <= 0 || vb <= 0) return 0;
        return (float)((sab - sa * sb / n) / std::sqrt(va * vb));
    }
    void ordered(const Sig& s, float* out) const {
        for (int i = 0; i < kHist; ++i) out[i] = s.hist[(s.hpos + i) % kHist];
    }
    int probeBin(int b, int k) const {
        static const int off[4] = { -8, -4, 4, 8 };
        return std::max(0, std::min(nw - 1, b + off[k]));
    }
    /** ★★ NOT A CW SIGNAL, AND WHY: a ghost (keyed by a much stronger neighbour — a sidelobe, splatter, key clicks), or
     *  BROADBAND — speech on SSB, a data mode, a carrier sweeping — whose envelope is the same 200–400 Hz away on both
     *  sides. A CW carrier is narrow: its neighbours are noise, or other stations keyed by other hands. (On a contest
     *  weekend the SSB half of the band made dozens of "signals" decoding syllables as E and T.) */
    bool isNotCw(const Sig& s) {
        float a[kHist], b[kHist], db[kHist];
        ordered(s, a);
        int wide = 0;
        for (int k = 0; k < 4; ++k) {
            const int pb = probeBin(s.bin, k);
            if (M[(size_t)pb] < 1.6f * noiseAt(std::max(kGuard, std::min(nw - 1 - kGuard, pb)))) continue;   // ★ only where something is
            for (int i = 0; i < kHist; ++i) b[i] = s.probe[k][(s.hpos + i) % kHist];
            if (std::fabs(corr(a, b, kHist)) > 0.35f) ++wide;
        }
        if (wide >= 2) { ++broadband; return true; }
        return isGhost(s, a, b, db);
    }
    bool isGhost(const Sig& s, float* a, float* b, float* db) {
        bool have = true;
        for (auto& o : sigs) {
            if (o.get() == &s || std::abs(o->bin - s.bin) > 12 || o->hfill < kHist) continue;
            // ★ Only a MUCH stronger neighbour can cast a ghost: 10 dB on the long average.
            if (M[(size_t)o->bin] < 10.0f * M[(size_t)s.bin]) continue;
            if (!have) { ordered(s, a); have = true; }
            ordered(*o, b);
            for (int i = 0; i < kHist; ++i) db[i] = i ? std::fabs(b[i] - b[i - 1]) : 0.0f;
            // ★ Keyed BY the strong signal: its level (a sidelobe, splatter) or its edges (key clicks).
            if (corr(a, b, kHist) > 0.5f || corr(a, db, kHist) > 0.5f) return true;
        }
        return false;
    }
    void dropSig(size_t i, bool keep) {
        Sig& s = *sigs[i];
        // ★ The "heard" list keeps what was worth reading: a callsign, or text that decoded cleanly.
        if (keep && s.confirmed && (!s.call.empty() || s.bestLane().quality >= 0.5f)) {
            gone.push_front(view(s, false));
            if (gone.size() > 300) gone.pop_back();
        }
        sigs.erase(sigs.begin() + (long)i);
    }
    /** ★★ WHICH LANE IS SHOWN — and the hand-over happens only at a WORD BOUNDARY in both lanes (neither has a character
     *  or a word open, neither is keyed nor holding an over-start), so a speed change that moves a signal from one lane to
     *  another mid-over loses no character and repeats none: the old lane has shown every word up to here, the new one
     *  shows every word from here. Asked every 32 hops; the hand-over itself waits in stepSignal for the boundary. */
    void chooseLane(Sig& s) {
        int b = s.best; float bs = laneScore(s.lane[b]);
        for (int i = 0; i < kLanes; ++i) {
            const float sc = laneScore(s.lane[i]);
            if (i != s.best && sc > bs + 0.08f) { b = i; bs = sc; }    // ★ hysteresis: clearly better
        }
        if (!s.confirmed) { s.best = b; s.pendingBest = -1; return; }
        if (s.overHold) return;
        s.pendingBest = b != s.best ? b : -1;
    }
    void beginHold(Sig& s) {
        s.overHold = true; s.holdSince = 0; s.pendingBest = -1;
        for (Lane& l : s.lane) { l.overChars = l.overValid = l.overElems = 0; l.overText.clear(); l.overWords.clear(); }
    }
    void endHold(Sig& s) {
        s.overHold = false;
        int b = s.best; float bs = -1;
        for (int i = 0; i < kLanes; ++i) {
            const Lane& l = s.lane[i];
            if (!l.overChars) continue;
            // ★ This over's own score: real codes, not just E and T, and the lane's timing tight
            float sc = (float)l.overValid / (float)l.overChars * std::min(1.0f, (float)l.overElems / (2.4f * (float)std::max(1u, l.overValid)));
            sc *= std::max(0.4f, std::min(1.0f, 1.0f - (l.markSpread - 0.12f) * 1.5f));
            sc *= smoothingFit(l);
            if (i == s.best) sc += 0.05f;       // ★ a tie keeps the lane that was shown
            if (sc > bs) { bs = sc; b = i; }
        }
        s.best = b;
        Lane& l = s.lane[b];
        // ★ …and what was held is DROPPED if it reads as noise does: mostly E and T (under two elements a character —
        //   text is ≈ 3), or few real codes. Still noisy, the signal stays on hold; the housekeeping drops it in 8 s.
        const bool noise = l.overChars && ((float)l.overElems < 2.0f * (float)std::max(1u, l.overValid) || l.overValid * 10 < l.overChars * 6);
        if (noise) {
            beginHold(s);
            return;
        }
        for (Lane& m : s.lane) appendText(m, m.overText);   // ★ every lane keeps its own whole history
        if (!l.overText.empty()) show(s, l.overText);
        // ★ The over's words count toward spots as they would have live
        std::vector<std::string> keep = l.words;
        std::vector<std::string> w;
        for (auto& x : l.overWords) { w.push_back(x); if (w.size() > 8) w.erase(w.begin()); l.words = w; checkCall(s); }
        l.words = keep;
    }
    static bool outOfRange(const Lane& l) { return l.unit < l.uLo * 0.8f || l.unit > l.uHi * 1.25f; }
    static bool atWordBoundary(const Lane& l) { return !l.on && !l.wordOpen && l.as.nel == 0 && !l.buffering; }
    void housekeeping() {
        const uint64_t endHops = hopsFor(20.0), tentHops = hopsFor(10.0);
        for (size_t i = 0; i < sigs.size();) {
            Sig& s = *sigs[i];
            // ★ Drift: follow the long average's peak, one bin at a time — ★ only while the signal stands clear of the
            //   noise: in a pause the long average is noise in every bin, and following ITS peaks walked a station two
            //   bins off its own carrier, where it was lost and found again as another signal.
            const float m = M[(size_t)s.bin];
            if (m > 3.0f * noiseAt(s.bin) && s.bin - 1 >= kGuard && M[(size_t)(s.bin - 1)] > 1.15f * m && !nearSignalExcept(s.bin - 1, &s)) --s.bin;
            else if (m > 3.0f * noiseAt(s.bin) && s.bin + 1 < nw - kGuard && M[(size_t)(s.bin + 1)] > 1.15f * m && !nearSignalExcept(s.bin + 1, &s)) ++s.bin;
            chooseLane(s);
            // ★ Ghosts: checked once a signal has a second of history, and every two seconds after.
            if (s.hfill >= kHist && (!s.ghostChecked || hopN - s.lastGhostCheck >= hopsFor(2.0))) {
                s.lastGhostCheck = hopN;
                if (isNotCw(s)) {
                    ++ghosts; hold.push_back({ s.bin, hopN + hopsFor(10.0) });
                    dropSig(i, false); continue;
                }
                s.ghostChecked = true;
            }
            // ★★ CONFIRMED: Morse-shaped keying, keyed by nobody stronger. Until then nothing is shown.
            if (!s.confirmed && s.ghostChecked && morseLike(s.bestLane())) confirm(s);
            // ★ A tentative that is plainly not keyed (no lane with contrast after 3 s) goes early, freeing its place.
            bool flat = !s.confirmed && hopN - s.born > hopsFor(3.0);
            if (flat) for (const Lane& l : s.lane) if (!l.onN || !l.offN || l.contrast() >= 2.2) { flat = false; break; }
            if (!s.confirmed && (flat || hopN - s.born > tentHops)) {
#ifdef CW_DEBUG
                if (std::getenv("CW_RUNS") && std::abs(offsetOf(w0 + s.bin) - std::atof(std::getenv("CW_RUNS"))) < 30)
                    for (int q = 0; q < kLanes; ++q) {
                        const Lane& l = s.lane[q];
                        std::fprintf(stderr, "drop %d lane %d%s: marks %d valid %u/%u epc %.2f sep %d ratio %.2f spread %.2f wpm %.1f contrast %.2f q %.2f score %.2f [%.60s]\n",
                            s.id, q, q == s.best ? "*" : " ", l.nMarks, l.valid, l.chars, l.valid ? (double)l.elems / l.valid : 0.0, l.markSep,
                            l.dashC / std::max(1e-3f, l.dotC), l.markSpread, 1.2 / (l.unit * hopSec), l.contrast(), l.quality, laneScore(l), l.text.c_str());
                    }
#endif
                ++tentDropped; hold.push_back({ s.bin, hopN + hopsFor(3.0) }); dropSig(i, false); continue;
            }
            // ★★ A confirmed signal that has STOPPED looking like Morse — its station went, and what is left in the bin is
            //    noise making E and T — is gone after 8 s of it, not kept decoding noise for ever (noise always "keys").
            if (s.confirmed) {
                const Lane& l = s.bestLane();
                const bool noisy = l.recentEpc < 1.9f || l.contrast() < 2.0 || outOfRange(l);
                if (!noisy) s.noisySince = 0;
                else if (!s.noisySince) s.noisySince = hopN;
                else if (hopN - s.noisySince > hopsFor(8.0)) { if (l.wordOpen) endWord(s, s.best); dropSig(i, true); continue; }
            }
            if (hopN - s.lastMark > endHops) {
                Lane& l = s.lane[s.best];
                if (l.wordOpen) endWord(s, s.best);
                dropSig(i, true); continue;
            }
            ++i;
        }
        // ★ Two signals on one carrier (one drifted onto the other, or both sides of its peak): the weaker goes. One bin
        //   apart is always one carrier; two bins apart is one only if they are keyed alike — two real signals 94 Hz apart
        //   are two peaks and are kept.
        for (size_t i = 0; i < sigs.size(); ++i)
            for (size_t k = i + 1; k < sigs.size(); ++k)
                if (sameCarrier(*sigs[i], *sigs[k])) {
                    const bool iWins = sigs[i]->confirmed != sigs[k]->confirmed ? sigs[i]->confirmed
                                     : M[(size_t)sigs[i]->bin] >= M[(size_t)sigs[k]->bin];
                    dropSig(iWins ? k : i, true);
                    --k; if (!iWins) { --i; break; }
                }
        hold.erase(std::remove_if(hold.begin(), hold.end(), [&](const HoldOff& h) { return h.until <= hopN; }), hold.end());
        const int64_t t = now();
        while (!gone.empty() && t - gone.back().lastMs > (int64_t)cfg.keepGoneSec * 1000) gone.pop_back();
    }
    bool sameCarrier(const Sig& a, const Sig& b) const {
        const int d = std::abs(a.bin - b.bin);
        if (d <= 1) return true;
        if (d > 2 || a.hfill < kHist || b.hfill < kHist) return false;
        float x[kHist], y[kHist]; ordered(a, x); ordered(b, y);
        return corr(x, y, kHist) > 0.8f;
    }
    bool nearSignalExcept(int b, const Sig* me) const {
        for (auto& s : sigs) if (s.get() != me && std::abs(s->bin - b) <= 1) return true;
        return false;
    }
    SignalView view(const Sig& s, bool active) const {
        SignalView v;
        const Lane& l = s.bestLane();
        v.id = s.id; v.offsetHz = preciseOffset(s); v.wpm = wpmOf(s); v.snrDb = snrOf(s); v.quality = l.quality;
        v.firstMs = s.firstMs; v.lastMs = s.lastMs; v.call = s.call; v.text = s.shown; v.active = active; v.chars = l.valid;
        return v;
    }
};

Skimmer::Skimmer(const Config& c) : d_(new Impl) { d_->owner = this; d_->init(c); }
Skimmer::~Skimmer() = default;

void Skimmer::feedReal(const float* x, int n) {
    Impl& d = *d_;
    if (d.cfg.complexIn) return;
    for (int i = 0; i < n;) {
        const int take = std::min(n - i, d.H - d.staged);
        std::memcpy(d.stage.data() + d.staged, x + i, (size_t)take * sizeof(float));
        d.staged += take; i += take;
        if (d.staged == d.H) {
            std::memmove(d.in.data(), d.in.data() + d.H, (size_t)(d.N - d.H) * sizeof(float));
            std::memcpy(d.in.data() + (d.N - d.H), d.stage.data(), (size_t)d.H * sizeof(float));
            d.staged = 0;
            d.doHop();
        }
    }
}
void Skimmer::feedIq(const std::complex<float>* x, int n) {
    Impl& d = *d_;
    if (!d.cfg.complexIn) return;
    const float* f = reinterpret_cast<const float*>(x);
    for (int i = 0; i < n;) {
        const int take = std::min(n - i, d.H - d.staged);
        std::memcpy(d.stage.data() + 2 * d.staged, f + 2 * i, (size_t)take * 2 * sizeof(float));
        d.staged += take; i += take;
        if (d.staged == d.H) {
            std::memmove(d.in.data(), d.in.data() + 2 * d.H, (size_t)(d.N - d.H) * 2 * sizeof(float));
            std::memcpy(d.in.data() + 2 * (d.N - d.H), d.stage.data(), (size_t)d.H * 2 * sizeof(float));
            d.staged = 0;
            d.doHop();
        }
    }
}

std::vector<SignalView> Skimmer::signals(bool includeGone) const {
    std::vector<SignalView> v;
    for (auto& s : d_->sigs) if (s->confirmed) v.push_back(d_->view(*s, true));
    std::sort(v.begin(), v.end(), [](const SignalView& a, const SignalView& b) { return a.snrDb > b.snrDb; });
    if (includeGone) for (auto& g : d_->gone) v.push_back(g);
    return v;
}
bool Skimmer::signal(int id, SignalView& out) const {
    for (auto& s : d_->sigs) if (s->id == id && s->confirmed) { out = d_->view(*s, true); return true; }
    return false;
}
void Skimmer::setStartUtcMs(int64_t ms) { d_->startMs = ms - (int64_t)std::llround((double)d_->hopN * d_->hopSec * 1000.0); }
int64_t Skimmer::nowMs() const { return d_->now(); }
int Skimmer::fftSize() const { return d_->N; }
int Skimmer::hop() const { return d_->H; }
double Skimmer::binHzActual() const { return d_->binHz; }
double Skimmer::hopSec() const { return d_->hopSec; }
int Skimmer::binsSearched() const { return d_->nw - 2 * kGuard; }
uint64_t Skimmer::hops() const { return d_->hopN; }
int Skimmer::tracked() const { return (int)d_->sigs.size(); }
int Skimmer::confirmed() const { int n = 0; for (auto& s : d_->sigs) n += s->confirmed; return n; }
uint64_t Skimmer::ghostsDropped() const { return d_->ghosts; }
uint64_t Skimmer::broadbandDropped() const { return d_->broadband; }
uint64_t Skimmer::tentativesDropped() const { return d_->tentDropped; }
double Skimmer::noiseCalibration() const {
    double sum = 0; int n = 0;
    for (int i = kGuard; i < d_->nw - kGuard; ++i) { sum += d_->M[(size_t)i] / (d_->F[(size_t)i] * kFloorBias); ++n; }
    return n ? sum / n : 0.0;
}

} // namespace cw
} // namespace vibe
