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
        int L = 1, minRun = 1; float uLo = 1, uHi = 100;
        float sm[24] = {}; int smPos = 0; float smSum = 0;
        float markLvl = 0;
        bool on = false; int run = 0, pend = 0;
        double onSum = 0, offSum = 0; uint32_t onN = 0, offN = 0;
        float unit = 11.25f, markSplit = 22.5f, charSplit = 22.5f, wordSplit = 56.f;
        float marks[kRing] = {}; int nMarks = 0, mPos = 0;
        float gaps[kGapRing] = {}; int nGaps = 0, gPos = 0;
        float dotC = 0, dashC = 0, markSpread = 1.f, gapRatio = 0; bool markSep = false;
        Assembler as;
        bool wordOpen = false;         // a character has been emitted since the last word space
        std::vector<Run> runs;         // ★ while tentative: every committed run, for the re-decode
        std::string text, word;
        std::vector<std::string> words;
        unsigned chars = 0, valid = 0, elems = 0;   // ★ elems: in the valid characters — noise makes E and T, text ≈ 3
        float quality = 0, validRate = 0;
        float recentEpc = 2.5f;        // ★ elements per valid character, recently (a moving mean) — noise sinks to ≈ 1.5
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
        std::string call;
        int64_t firstMs = 0, lastMs = 0;
        uint64_t noisySince = 0;       // ★ a confirmed signal whose keying stopped looking like Morse, since when (0 = it does)
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
        l.uLo = dotHops(sp.hi); l.uHi = dotHops(sp.lo);
        l.unit = dot; l.markSplit = 2.0f * dot; l.charSplit = 2.0f * dot; l.wordSplit = 5.0f * dot;
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
        if (anyOn) s.lastMark = hopN;
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
            if (l.on) onMark(s, l, l.run); else onGap(s, l, l.run);
            l.on = raw; l.run = l.pend; l.pend = 0;
        }
        if (!l.on) {
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
    /** Three clusters of log gap length, started at 1, 3 and 7 units. */
    static void threeMeans(const float* v, int n, float logUnit, float c[3], int cnt[3]) {
        c[0] = logUnit; c[1] = logUnit + std::log(3.0f); c[2] = logUnit + std::log(7.0f);
        for (int it = 0; it < 8; ++it) {
            float sum[3] = { 0, 0, 0 }; cnt[0] = cnt[1] = cnt[2] = 0;
            const float t01 = 0.5f * (c[0] + c[1]), t12 = 0.5f * (c[1] + c[2]);
            for (int i = 0; i < n; ++i) { const int k = v[i] < t01 ? 0 : v[i] < t12 ? 1 : 2; sum[k] += v[i]; ++cnt[k]; }
            bool moved = false;
            for (int k = 0; k < 3; ++k) if (cnt[k]) { const float nc = sum[k] / cnt[k]; moved |= nc != c[k]; c[k] = nc; }
            if (!moved) break;
        }
    }
    void retime(Lane& l) {
        // ★★ The speed, learnt from the keying: the marks' log lengths fall into two clusters, dots and dashes.
        //    Only keying that LOOKS LIKE MORSE teaches it anything — two clusters ≈ 3:1 apart and tight (noise thresholded
        //    at its median clusters too, at about 2.3:1); anything else leaves the lane's speed where it was.
        if (l.nMarks >= 4) {
            float lo, hi; int nl, nh;
            twoMeans(l.marks, std::min(l.nMarks, kRing), lo, hi, nl, nh, &l.markSpread);
            float unit = l.unit;
            const float ratio = std::exp(hi - lo);
            const bool morse = nl > 0 && nh > 0 && ratio >= 2.6f && ratio <= 6.0f && l.markSpread <= 0.4f;
            if (morse) {
                l.markSep = true; l.dotC = std::exp(lo); l.dashC = std::exp(hi);
                // ★★ THE UNIT IS HALF THE DIFFERENCE, dash − dot = 2 units EXACTLY: whatever ends a mark early (the
                //    threshold, the smoothing, the sender's weighting) shortens a dot and a dash by the same time, so the
                //    difference carries no bias — where dot alone, or dash ÷ 3, reads a weak signal fast.
                unit = 0.5f * (l.dashC - l.dotC);
            } else if (hi - lo < 0.4f && l.markSpread <= 0.2f && l.nGaps >= 4) {
                // ★ All one kind (a run of E/I/S/H or T/M/O): which kind is told by the element gaps (always one unit).
                const float m = std::exp(0.5f * (lo + hi));
                float a, b; int na, nb; twoMeans(l.gaps, std::min(l.nGaps, kGapRing), a, b, na, nb);
                unit = m > 2.0f * std::exp(a) ? m / 3.0f : m;
            }
            unit = std::max(l.uLo, std::min(l.uHi, unit));          // ★ inside the lane's own range
            l.unit = l.nMarks < 8 ? unit : 0.7f * l.unit + 0.3f * unit;
            l.markSplit = l.markSep ? 0.5f * (l.dotC + l.dashC) : 2.0f * l.unit;
        }
        // ★★ Gaps: THREE kinds — element (1 unit), character (3) and word (7) — clustered from the lane's unit. Real
        //    fists leave long character gaps (4–5 units is common), and a word split taken as "5 units" broke calls in
        //    two on the air ("IU 5 R DL"); the word gaps the sender actually leaves set it instead.
        float cs = 2.0f * l.unit, ws = std::max(1.67f * cs, 5.0f * l.unit);
        if (l.nGaps >= 6) {
            float c[3]; int n[3];
            threeMeans(l.gaps, std::min(l.nGaps, kGapRing), std::log(l.unit), c, n);
            l.gapRatio = std::exp(c[1] - c[0]);
            if (n[0] && n[1] && c[1] - c[0] > 0.55f) cs = std::exp(0.5f * (c[0] + c[1]));
            if (n[1] && n[2] >= 2 && c[2] - c[1] > 0.45f) ws = std::exp(0.5f * (c[1] + c[2]));
            else if (n[1]) ws = std::max(2.0f * std::exp(c[1]), 5.0f * l.unit);
        }
        l.charSplit = std::max(1.5f * l.unit, std::min(4.5f * l.unit, cs));
        l.wordSplit = std::min(14.0f * l.unit, std::max(std::max(1.5f * l.charSplit, 4.5f * l.unit), ws));
    }
    void logRun(const Sig& s, Lane& l, int len, bool mark) {
        if (s.confirmed || l.runs.size() >= kRunLog) return;
        l.runs.push_back({ (uint16_t)std::min(len, 65535), mark });
    }
    void onMark(const Sig& s, Lane& l, int len) {
        if (len <= 0) return;
        logRun(s, l, len, true);
        const float L = (float)len;
        l.marks[l.mPos] = std::log(L); l.mPos = (l.mPos + 1) % kRing; ++l.nMarks;
        retime(l);
        const bool dash = L > l.markSplit;
        const float spread = l.markSep ? 0.5f * std::log(l.dashC / l.dotC) : 0.55f;
        l.as.conf = std::min(l.as.conf, margin(L, l.markSplit, spread));
        // ★ A mark far longer than a dash is not Morse (a carrier, a tune-up): the character is spoilt.
        if (L > 7.0f * l.unit) l.as.overflow = true;
        l.as.element(dash);
    }
    void onGap(const Sig& s, Lane& l, int len) {
        if (len <= 0) return;
        logRun(s, l, len, false);
        const float L = (float)len;
        // ★ Long pauses (between overs) would drag the gap clusters away from the keying; only gaps up to 14 units count.
        if (L < 14.0f * l.unit) { l.gaps[l.gPos] = std::log(L); l.gPos = (l.gPos + 1) % kGapRing; ++l.nGaps; }
        // the character itself ended inside stepLane (the gap passed charSplit while running)
    }
    void endChar(Sig& s, int li, float gapLen) {
        Lane& l = s.lane[li];
        const float c = std::min(l.as.conf, margin(gapLen, l.charSplit, 0.5f));
        const int nel = l.as.nel;
        const char* t = l.as.take();
        ++l.chars;
        // ★★ QUALITY = how many characters are real codes (a running mean over the first 16, then a moving one) × how
        //    tight the timing is (the marks' spread about their dot and dash: a keyer ≈ 0.05, a good fist 0.15–0.25, noise
        //    ≥ 0.35). High / medium / low / poor at 0.75 / 0.5 / 0.3.
        (void)c;
        l.validRate += (l.chars <= 16 ? 1.0f / (float)l.chars : 0.06f) * ((t ? 1.0f : 0.0f) - l.validRate);
        const float tight = std::max(0.4f, std::min(1.0f, 1.0f - (l.markSpread - 0.12f) * 1.5f));
        l.quality = l.validRate * tight;
        if (!t) return;
        ++l.valid; l.elems += (unsigned)nel;
        l.recentEpc += 0.1f * ((float)nel - l.recentEpc);
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
            if (li == s.best) checkCall(s);
        }
    }
    void emit(Sig& s, int li, const char* t) {
        Lane& l = s.lane[li];
        l.text += t;
        if (l.text.size() > 400) l.text.erase(0, l.text.size() - 300);
        if (s.confirmed && li == s.best && owner->onText) owner->onText(s.id, preciseOffset(s), t);
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
            if (r.mark) { if (r.len > 7.0f * l.unit) as.overflow = true; as.element((float)r.len > l.markSplit); continue; }
            if (as.nel > 0 && r.len > l.charSplit) { if (const char* t = as.take()) { text += t; word += t; open = true; } }
            if (open && r.len > l.wordSplit) endWordR();
        }
        // ★ What follows is decoded live from the state at the end of the log: the character in progress (if the current
        //   gap or mark is still running) is the re-decoder's.
        l.as = as; l.word = word; l.words = words; l.wordOpen = open;
        if (text.size() > 400) text.erase(0, text.size() - 300);
        l.text = text;
        for (auto& x : s.lane) { x.runs.clear(); x.runs.shrink_to_fit(); }
        if (owner->onText && !text.empty()) owner->onText(s.id, preciseOffset(s), text);
    }
    /** How much a lane's keying looks like Morse at its speed — what chooses the lane shown. */
    float laneScore(const Lane& l) const {
        if (l.valid < 3) return 0.0f;
        float sc = l.quality * ((float)l.valid / (float)std::max(1u, l.chars));
        if (!l.markSep) sc *= 0.6f;
        sc *= std::min(1.0f, (float)l.elems / (2.5f * (float)l.valid));
        if (l.unit <= l.uLo * 1.03f || l.unit >= l.uHi * 0.97f) sc *= 0.5f;   // ★ pinned at its range: the wrong lane
        return sc;
    }
    /** ★★ IS IT MORSE? Noise thresholded at its own median makes "marks" too — so a tentative signal is shown only when
     *  its keying has Morse's shape: two tight clusters of mark length about three to one, a speed well inside the lane,
     *  a clear on/off contrast, characters that are real codes and not just E and T. */
    bool morseLike(const Lane& l) const {
        if (l.nMarks < 20 || l.valid < 8 || !l.markSep) return false;
        const float ratio = l.dashC / l.dotC;
        if (ratio < 2.6f || ratio > 6.0f || l.markSpread > 0.4f) return false;
        if (l.unit <= l.uLo * 1.03f || l.unit >= l.uHi * 0.97f) return false;
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
    /** ★ The speed shown: PARIS, 1.2 s ÷ the unit — and the unit is (dash − dot) ÷ 2, which nothing biases (retime). */
    float wpmOf(const Sig& s) const { return (float)(1.2 / (s.bestLane().unit * hopSec)); }
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
    void chooseLane(Sig& s) {
        int b = s.best; float bs = laneScore(s.lane[b]);
        for (int i = 0; i < kLanes; ++i) {
            const float sc = laneScore(s.lane[i]);
            // ★ Hysteresis: another lane must be clearly better before the text shown changes hands.
            if (i != s.best && sc > bs + 0.08f) { b = i; bs = sc; }
        }
        if (b != s.best && s.confirmed) {
            // ★ Handing over mid-stream: the new lane picks up at its next character; a word break keeps the joins clean.
            if (owner->onText) owner->onText(s.id, preciseOffset(s), " ");
        }
        s.best = b;
    }
    void housekeeping() {
        const uint64_t endHops = hopsFor(20.0), tentHops = hopsFor(10.0);
        for (size_t i = 0; i < sigs.size();) {
            Sig& s = *sigs[i];
            // ★ Drift: follow the long average's peak, one bin at a time.
            const float m = M[(size_t)s.bin];
            if (s.bin - 1 >= kGuard && M[(size_t)(s.bin - 1)] > 1.15f * m && !nearSignalExcept(s.bin - 1, &s)) --s.bin;
            else if (s.bin + 1 < nw - kGuard && M[(size_t)(s.bin + 1)] > 1.15f * m && !nearSignalExcept(s.bin + 1, &s)) ++s.bin;
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
                const bool noisy = l.recentEpc < 1.9f || l.contrast() < 2.0;
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
        v.firstMs = s.firstMs; v.lastMs = s.lastMs; v.call = s.call; v.text = l.text; v.active = active; v.chars = l.valid;
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
