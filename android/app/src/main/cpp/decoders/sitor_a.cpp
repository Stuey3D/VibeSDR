// VibeSDR V12 — SITOR-A (AMTOR mode A) listener. See sitor_a.h for the design; the protocol references in the comments
// are to ITU-R M.476-5 Annex 1 ("M.476 §…"), ITU-R M.625-4 Annex 1 ("M.625 §…") and ITU-R M.491-1 ("M.491").
#include "sitor_a.h"
#include "../vibedsp/simd_internal.h"     // VIBE_NEON / VIBE_SSE — the project's one SIMD switch
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <cstdio>
#include <cstdlib>

namespace vibe {
namespace sitora {

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The code tables
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
namespace {
/* M.476 Table 1 / M.625 Table 1: combination, letter case, figure case, emitted 7-unit signal. Figure case 0 = none
 *  assigned (F, G, H — "reception of these signals should not initiate a request for repetition"), and D (WRU) and
 *  J (the audible signal), which print nothing here and are reported as events instead. */
struct Row { char32_t ltr, fig; const char* by; };
constexpr Row kTable[] = {
    {U'A', U'-', "BBBYYYB"}, {U'B', U'?', "YBYYBBB"}, {U'C', U':', "BYBBBYY"}, {U'D', 0, "BBYYBYB"},
    {U'E', U'3', "YBBYBYB"}, {U'F', 0, "BBYBBYY"},    {U'G', 0, "BYBYBBY"},    {U'H', 0, "BYYBYBB"},
    {U'I', U'8', "BYBBYYB"}, {U'J', 0, "BBBYBYY"},    {U'K', U'(', "YBBBBYY"}, {U'L', U')', "BYBYYBB"},
    {U'M', U'.', "BYYBBBY"}, {U'N', U',', "BYYBBYB"}, {U'O', U'9', "BYYYBBB"}, {U'P', U'0', "BYBBYBY"},
    {U'Q', U'1', "YBBBYBY"}, {U'R', U'4', "BYBYBYB"}, {U'S', U'\'', "BBYBYYB"}, {U'T', U'5', "YYBYBBB"},
    {U'U', U'7', "YBBBYYB"}, {U'V', U'=', "YYBBBBY"}, {U'W', U'2', "BBBYYBY"}, {U'X', U'/', "YBYBBBY"},
    {U'Y', U'6', "BBYBYBY"}, {U'Z', U'+', "BBYYYBB"},
};
struct Tables {
    char32_t ltr[128] = {0}, fig[128] = {0};
    bool ok[128] = {false};
    Tables() {
        for (int v = 0; v < 128; v++) { int c = 0; for (int b = 0; b < 7; b++) c += (v >> b) & 1; ok[v] = c == 4; }
        for (const Row& r : kTable) { const uint8_t w = sig(r.by); ltr[w] = r.ltr; fig[w] = r.fig; }
    }
};
const Tables& T() { static const Tables t; return t; }

// M.625 Table 3a — identification signal → equivalent number; Table 3b is the same pairing read the other way
// (and M.491 Table III, remainder → identification signal, is the same again).
constexpr char kIdByValue[21] = "VXQKMPCYFSTBUEOIRZDA";   // value 0 … 19
} // namespace

bool valid(uint8_t w) { return w < 128 && T().ok[w]; }
char32_t letterOf(uint8_t w) { return w < 128 ? T().ltr[w] : 0; }
char32_t figureOf(uint8_t w) { return w < 128 ? T().fig[w] : 0; }
uint8_t letterSignal(char32_t c) { for (const Row& r : kTable) if (r.ltr == c) return sig(r.by); return 0; }
uint8_t figureSignal(char32_t c) { for (const Row& r : kTable) if (r.fig == c && c) return sig(r.by); return 0; }

int idValue(char32_t c) {
    for (int i = 0; i < 20; i++) if ((char32_t)kIdByValue[i] == c) return i;
    return -1;
}
bool isIdSignal(char32_t c) { return idValue(c) >= 0; }
char32_t checksumSignal(int cn) { return cn >= 0 && cn < 20 ? (char32_t)kIdByValue[cn] : 0; }

std::string checksums(const std::string& id) {
    if (id.size() != 7) return {};
    int n[7];
    for (int i = 0; i < 7; i++) { n[i] = idValue((char32_t)id[(size_t)i]); if (n[i] < 0) return {}; }
    std::string s;
    s += (char)checksumSignal((n[0] + n[1] + n[2]) % 20);
    s += (char)checksumSignal((n[2] + n[3] + n[4]) % 20);
    s += (char)checksumSignal((n[4] + n[5] + n[6]) % 20);
    return s;
}

/* M.491-1 Annex I, Table I: a 5-digit number's FIRST digit picks, for each of the other four, which of two letter sets
 *  it is sent in — set V (V X Q K M P C Y F S for 0–9) or set T (T B U E O I R Z D A). Every first digit gives a
 *  different pattern of one or two set-T places; a 4-digit number (Table II) uses set V throughout. */
namespace {
constexpr char kSetV[11] = "VXQKMPCYFS", kSetT[11] = "TBUEOIRZDA";
constexpr const char* kPattern[10] = { "TVVV", "VTVV", "VVTV", "VVVT", "TTVV", "TVTV", "TVVT", "VTTV", "VTVT", "VVTT" };
int digitIn(const char* set, char c) { for (int i = 0; i < 10; i++) if (set[i] == c) return i; return -1; }
} // namespace

std::string identityNumber(const std::string& ids) {
    if (ids.size() == 7) {   // Annex II: IS1 … IS7 are the base-20 digits, most significant first
        unsigned long long v = 0;
        for (char c : ids) { const int d = idValue((char32_t)c); if (d < 0) return {}; v = v * 20 + (unsigned)d; }
        if (v > 999999999ULL) return {};
        char b[16]; std::snprintf(b, sizeof b, "%09llu", v);
        return b;
    }
    if (ids.size() != 4) return {};
    std::string pat, digits;
    for (char c : ids) {
        int d = digitIn(kSetV, c);
        if (d >= 0) { pat += 'V'; digits += (char)('0' + d); continue; }
        d = digitIn(kSetT, c);
        if (d < 0) return {};
        pat += 'T'; digits += (char)('0' + d);
    }
    if (pat == "VVVV") return digits;                       // Table II: a 4-digit number
    for (int f = 0; f < 10; f++) if (pat == kPattern[f]) return std::string(1, (char)('0' + f)) + digits;
    return {};
}

std::string identitySignals(const std::string& num) {
    for (char c : num) if (c < '0' || c > '9') return {};
    if (num.size() == 9) {
        unsigned long long v = std::stoull(num);
        std::string s(7, 'V');
        for (int i = 6; i >= 0; i--) { s[(size_t)i] = kIdByValue[v % 20]; v /= 20; }
        return s;
    }
    if (num.size() == 4) { std::string s; for (char c : num) s += kSetV[c - '0']; return s; }
    if (num.size() == 5) {
        const char* p = kPattern[num[0] - '0'];
        std::string s;
        for (int i = 0; i < 4; i++) s += (p[i] == 'T' ? kSetT : kSetV)[num[(size_t)i + 1] - '0'];
        return s;
    }
    return {};
}

const char* linkName(Link l) {
    switch (l) {
        case Link::NoSignal: return "no signal";
        case Link::Searching: return "searching";
        case Link::Calling: return "phasing";
        case Link::Identifying: return "identification";
        case Link::Traffic: return "traffic";
        case Link::Idle: return "idle";
        case Link::ChangeOver: return "change-over";
        case Link::Ended: return "ended";
    }
    return "";
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The receiver
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
namespace {
using cd = std::complex<double>;

std::vector<float> lowpass(int n, double fc, double fs) {      // Blackman-windowed sinc, unity gain at DC
    std::vector<float> h((size_t)n);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double m = i - (n - 1) / 2.0, x = 2 * fc / fs * m;
        const double s = std::fabs(m) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
        h[(size_t)i] = (float)(s * w); sum += s * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}
/** Two dot products of one tap set with the I and Q delay lines — the front end's whole cost. */
inline void dot2(const float* h, const float* re, const float* im, int n, float& yr, float& yi) {
    int i = 0;
#if VIBE_NEON
    float32x4_t ar = vdupq_n_f32(0), ai = vdupq_n_f32(0);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t hv = vld1q_f32(h + i);
        ar = vmlaq_f32(ar, hv, vld1q_f32(re + i));
        ai = vmlaq_f32(ai, hv, vld1q_f32(im + i));
    }
    float sr = vaddvq_f32(ar), si = vaddvq_f32(ai);
#elif VIBE_SSE
    __m128 ar = _mm_setzero_ps(), ai = _mm_setzero_ps();
    for (; i + 4 <= n; i += 4) {
        const __m128 hv = _mm_loadu_ps(h + i);
        ar = _mm_add_ps(ar, _mm_mul_ps(hv, _mm_loadu_ps(re + i)));
        ai = _mm_add_ps(ai, _mm_mul_ps(hv, _mm_loadu_ps(im + i)));
    }
    float sr = vibedsp::sseAddv(ar), si = vibedsp::sseAddv(ai);
#else
    float sr = 0, si = 0;
#endif
    for (; i < n; ++i) { sr += h[i] * re[i]; si += h[i] * im[i]; }
    yr = sr; yi = si;
}
struct Fir {                     // a decimating complex FIR (real taps), delay line doubled so a window is contiguous
    std::vector<float> h, re, im; int pos = 0, n = 0, d = 1, cnt = 0;
    void init(const std::vector<float>& taps, int dec) { h = taps; n = (int)h.size(); d = dec; re.assign((size_t)n * 2, 0); im.assign((size_t)n * 2, 0); pos = 0; cnt = 0; }
    bool push(float xr, float xi, float& yr, float& yi) {
        re[(size_t)pos] = re[(size_t)(pos + n)] = xr; im[(size_t)pos] = im[(size_t)(pos + n)] = xi;
        pos = (pos + 1) % n;
        if (++cnt < d) return false;
        cnt = 0;
        dot2(h.data(), &re[(size_t)pos], &im[(size_t)pos], n, yr, yi);   // oldest … newest (taps are symmetric)
        return true;
    }
};

constexpr int kRing = 16384;     // decimated samples kept (~8 s at 2 kHz): acquisition replays its whole evidence window
constexpr int kNW = 6;           // cycles of evidence the acquisition folds (2.7 s)
constexpr int kBins = 5;         // acquisition frequency bins (Hz offsets below)
constexpr double kBinHz[kBins] = { -64, -32, 0, 32, 64 };
constexpr double kShift = 170.0, kBaud = 100.0, kCycle = 0.45;
constexpr int kLose = 30;        // cycles a station may be silent before it is dropped (M.625 §3.7.12: rephase after 32)
constexpr float kMarginOk = 3.0f;    // a character's ML margin (LLR units) that counts as READ
constexpr float kMarginLost = 0.8f;  // …below which the best guess is printed as '_' when the block must be let go

/** Maximum-likelihood decision of a 4-of-7 word from its soft values: the four largest are the B elements. */
struct Word { uint8_t w = 0, hard = 0; float margin = 0, score = 0; bool clean = false; };
Word mlWord(const float* l) {
    int idx[7] = {0, 1, 2, 3, 4, 5, 6};
    std::sort(idx, idx + 7, [l](int a, int b) { return l[a] > l[b]; });
    Word r;
    for (int i = 0; i < 4; i++) r.w = (uint8_t)(r.w | (1 << idx[i]));
    for (int i = 0; i < 7; i++) { if (l[i] > 0) r.hard = (uint8_t)(r.hard | (1 << i)); r.score += (i < 4 ? 1 : -1) * l[idx[i]]; }
    r.margin = l[idx[3]] - l[idx[4]];
    r.clean = r.hard == r.w;
    return r;
}
bool inCsSet(uint8_t w) { return w == kCs1 || w == kCs2 || w == kCs3 || w == kCs4 || w == kCs5 || w == kRq; }
} // namespace

struct Receiver::Impl {
    Receiver& R;
    Options o;
    double rate, centre;
    // ── front end ──
    Fir f1, f2;
    double ncoRe = 1, ncoIm = 0, ncoStRe = 1, ncoStIm = 0; long ncoN = 0;
    double fi = 2000, K = 20, C = 900; int Ki = 20, Ci = 900, o7 = 140, o14 = 280;
    std::vector<float> zr, zi;   // the decimated complex baseband, kRing long
    int64_t n = 0;               // decimated samples so far
    double level = 0;            // input mean square (slow)

    // ── acquisition ──
    struct Bin {
        double fm = 0, fs = 0; cd pm{1, 0}, ps{1, 0}, stm, sts;
        std::vector<std::complex<float>> pmBuf, psBuf; cd sm{0, 0}, ss{0, 0}; int bp = 0;
        std::vector<uint8_t> sgn;          // the soft sign at every sample (bit ends), 256 deep
        std::vector<uint8_t> hist;         // per-sample flags for the last kNW cycles
        std::vector<uint8_t> cnt[4];       // fold counts: valid / valid inverted / control set / control set inverted
        std::vector<float> efold;          // energy, folded on the cycle (EMA)
    };
    Bin bins[kBins];
    bool hunting = true; int64_t huntFrom = 0; int64_t nextAcq = 0;

    // ── stations ──
    struct Station {
        bool on = false;
        double s = 0;            // the start of its burst in the cycle being processed (absolute decimated sample)
        int len = 3;             // 3 = a block, 1 = a control signal
        double df = 0; int pol = 1;
        int silent = 0; double snr = 0; float ampl = 0;
        int64_t lastSeen = 0, due = 0;
        double lastStart = 0, lastEnd = 0;   // its last burst (as decided, or as expected when nothing was seen)
        double d = 0; bool haveD = false;    // its delay after the other station's signal ends (its tE, if it is the slave)
        double aB = 0, aY = 0; bool haveA = false;   // its two tones' amplitudes at its last burst (scale)
        int failed = 0;                              // runs in a row with a word that did not read cleanly
    };
    Station S[2];
    double N0 = 1;               // noise: E|M|² + E|S|² per bit with no signal
    int swapExpected = 0;        // cycles left in which a change-over's swap of roles is expected

    // ── the listener (protocol) ──
    struct Pending {
        bool on = false; int st = -1; int req = 0; int copies = 0; bool printed = false; bool hadClean = false;
        bool strong = false;   // at least one copy the tracker called present (not only faded ones)
        float llr[21] = {0}; uint8_t words[3] = {0};
    };
    Pending pend;
    int ctrlSince[2] = {0, 0};   // the last control signal heard from station i since the other's last block (0 none)
    bool figs[2] = {false, false};
    int lastSig[2] = {0, 0};     // the last two printing signals in figure case ('+', '?') for "over"
    std::string callIds; int callStage = 0; int callerStage = 0; std::string callerIds; std::string ckGot;
    int idleRun = 0;

    Impl(Receiver& r, double rt, double cf, const Options& op) : R(r), o(op), rate(rt), centre(cf) { build(); }

    void build() {
        const int d1 = std::max(1, (int)std::lround(rate / 6000.0));
        const double r1 = rate / d1;
        const int d2 = std::max(1, (int)std::lround(r1 / 2000.0));
        f1.init(lowpass(std::max(12, 6 * d1), 500, rate), d1);
        f2.init(lowpass(48, 450, r1), d2);
        fi = r1 / d2; K = fi / kBaud; C = fi * kCycle;
        Ki = (int)std::lround(K); Ci = (int)std::lround(C); o7 = (int)std::lround(7 * K); o14 = (int)std::lround(14 * K);
        zr.assign(kRing, 0); zi.assign(kRing, 0);
        for (int b = 0; b < kBins; b++) {
            Bin& B = bins[b];
            B.fm = kShift / 2 + kBinHz[b]; B.fs = -kShift / 2 + kBinHz[b];
            B.stm = std::polar(1.0, -2 * M_PI * B.fm / fi); B.sts = std::polar(1.0, -2 * M_PI * B.fs / fi);
            B.pmBuf.assign((size_t)Ki, {0, 0}); B.psBuf.assign((size_t)Ki, {0, 0});
            if (B.bp >= Ki) B.bp = 0;   // ★ a shorter ring after a re-measured rate: the index must stay inside it
            B.sgn.assign(256, 0); B.hist.assign((size_t)(kNW * Ci), 0);
            for (auto& c : B.cnt) c.assign((size_t)Ci, 0);
            B.efold.assign((size_t)Ci, 0);
        }
        setCentre(centre);
    }
    void setCentre(double hz) {
        centre = hz;
        ncoStRe = std::cos(2 * M_PI * hz / rate); ncoStIm = -std::sin(2 * M_PI * hz / rate);
    }
    void resetLink() {
        for (auto& s : S) s = Station();
        restartHunt();
        pend = Pending(); ctrlSince[0] = ctrlSince[1] = 0; figs[0] = figs[1] = false; lastSig[0] = lastSig[1] = 0;
        callIds.clear(); callVotes_.clear(); callStage = 0; callerStage = 0; callerIds.clear(); ckGot.clear(); idleRun = 0;
        swapExpected = 0;
        R.st_ = Status();
    }
    void restartHunt() {
        hunting = true; huntFrom = n; nextAcq = n + (int64_t)kNW * Ci;
        for (Bin& B : bins) {
            std::fill(B.hist.begin(), B.hist.end(), 0);
            for (auto& c : B.cnt) std::fill(c.begin(), c.end(), 0);
            std::fill(B.efold.begin(), B.efold.end(), 0.f);
        }
    }

    // ── per input sample ──
    void input(float x) {
        level += (x * x - level) * 1e-4;
        const float a = (float)(x * ncoRe), b = (float)(x * ncoIm);
        const double t = ncoRe * ncoStRe - ncoIm * ncoStIm; ncoIm = ncoRe * ncoStIm + ncoIm * ncoStRe; ncoRe = t;
        if ((++ncoN & 4095) == 0) { const double g = 1.0 / std::hypot(ncoRe, ncoIm); ncoRe *= g; ncoIm *= g; }
        float yr, yi, zr1, zi1;
        if (!f1.push(a, b, yr, yi)) return;
        if (!f2.push(yr, yi, zr1, zi1)) return;
        const int k = (int)(n & (kRing - 1));
        zr[(size_t)k] = 2 * zr1; zi[(size_t)k] = 2 * zi1;    // ×2: a real tone of amplitude A mixes to A/2
        if (hunting || !S[0].on || !S[1].on) acqSample(k);
        n++;
        if (hunting && n >= nextAcq) { nextAcq = n + Ci; acquire(); }
        else if (!hunting && (!S[0].on || !S[1].on) && n >= nextAcq) { nextAcq = n + Ci; acquireOther(); }
        track();
    }

    // ── acquisition: fold code validity on the 450 ms cycle ─────────────────────────────────────────────────────
    /* ★ Each bin runs a sliding one-bit matched filter on both tones, at every sample, and keeps the sign of their
     *  difference. The seven signs one bit apart ending here make a word: is it one of the 35 (four B)? one of the 35
     *  inverted (three B — the other sideband)? one of the six control signals (either sense)? Those flags are counted
     *  at the sample's place in the cycle over the last kNW cycles: a real SITOR-A burst puts the same place at nearly
     *  kNW every time; noise reaches 27 % of it (35/128). */
    void acqSample(int k) {
        const int64_t nn = n;
        const int phase = (int)(nn % Ci), hIdx = (int)(nn % ((int64_t)kNW * Ci));
        // ★ the seven bit offsets, once a sample rather than once a bin (decoder audit 2026-10-31): std::lround is a library
        //   routine on 32-bit ARM, and seven per bin per sample were a fifth of SITOR-A on the Pi 2. Same values.
        int64_t back[7];
        for (int i = 0; i < 7; i++) back[i] = (int64_t)std::lround((6 - i) * K);
        for (int b = 0; b < kBins; b++) {
            Bin& B = bins[b];
            const cd z(zr[(size_t)k], zi[(size_t)k]);
            const cd vm = z * B.pm, vs = z * B.ps;
            B.pm *= B.stm; B.ps *= B.sts;
            if ((nn & 1023) == 0) { B.pm /= std::abs(B.pm); B.ps /= std::abs(B.ps); }
            B.sm += vm - cd(B.pmBuf[(size_t)B.bp]); B.ss += vs - cd(B.psBuf[(size_t)B.bp]);
            B.pmBuf[(size_t)B.bp] = std::complex<float>(vm); B.psBuf[(size_t)B.bp] = std::complex<float>(vs);
            if (++B.bp >= Ki) B.bp = 0;   // ★ % Ki without the divide
            const double em = std::norm(B.sm), es = std::norm(B.ss);
            B.sgn[(size_t)(nn & 255)] = em > es;
            uint8_t code = 0;
            for (int i = 0; i < 7; i++) {
                const int64_t at = nn - back[i];
                if (B.sgn[(size_t)(at & 255)]) code = (uint8_t)(code | (1 << i));
            }
            const uint8_t inv = (uint8_t)(~code & 0x7f);
            const uint8_t fl = (uint8_t)((valid(code) ? 1 : 0) | (valid(inv) ? 2 : 0) | (inCsSet(code) ? 4 : 0) | (inCsSet(inv) ? 8 : 0));
            const uint8_t old = B.hist[(size_t)hIdx];
            for (int f = 0; f < 4; f++) {
                const int d = ((fl >> f) & 1) - ((old >> f) & 1);
                if (d) B.cnt[f][(size_t)phase] = (uint8_t)(B.cnt[f][(size_t)phase] + d);
            }
            B.hist[(size_t)hIdx] = fl;
            float& e = B.efold[(size_t)phase];
            e += ((float)(em + es) - e) * (1.0f / kNW);
        }
    }
    float efoldMean(const Bin& B, double from, double len) const {
        double s = 0; int c = 0;
        for (int j = 0; j < (int)len; j++) { s += B.efold[(size_t)((((int64_t)std::lround(from) + j) % Ci + Ci) % Ci)]; c++; }
        return c ? (float)(s / c) : 0.f;
    }
    /** Look for the information blocks: three words a character apart that the fold says are valid, every cycle, with a
     *  quiet gap after them (an FEC or RTTY signal is never quiet). */
    void acquire() {
        if (n - huntFrom < (int64_t)kNW * Ci) return;
        int best = 0, bb = -1, bpol = 1, bphi = 0; float bE = 0, bGap = 0;
        for (int b = 0; b < kBins; b++) {
            const Bin& B = bins[b];
            for (int pol = 0; pol < 2; pol++) {
                const auto& c = B.cnt[pol];
                for (int ph = 0; ph < Ci; ph++) {
                    const int sc = c[(size_t)ph] + c[(size_t)((ph + o7) % Ci)] + c[(size_t)((ph + o14) % Ci)];
                    /* ★ 15 of the 18 words valid. MEASURED on the 4 and 8 MHz recordings (2026-10-29): at 60 % the 100 Bd
                     *  dot bursts beside the 4209.5 kHz NAVTEX (alternate bits — every other 7-bit word reads as valid) and a
                     *  DSC call locked and printed garbage; at 15 of 18 nothing real but SITOR-A does, and a -6 dB link (a
                     *  third of its words misread, unreadable anyway) was not one more character the better for the lower bar. */
                    if (sc < 3 * kNW - 3 || sc < best) continue;
                    const double start = ph - o7 + 1;                          // the block's first sample
                    const float eb = efoldMean(B, start + K, 20 * K);
                    float gap = 1e30f;
                    for (double w = start + 22 * K; w + 6 * K <= start + 45 * K; w += K) gap = std::min(gap, efoldMean(B, w, 6 * K));
                    if (gap > 0.4f * eb) continue;
                    if (sc > best || eb > bE) { best = sc; bb = b; bpol = pol ? -1 : 1; bphi = ph; bE = eb; bGap = gap; }
                }
            }
        }
        if (bb < 0) { if (R.st_.link != Link::Searching && level > 1e-9) setLink(Link::Searching); return; }
        // the block start in absolute samples: the most recent whose block is complete, then back to the first cycle of
        // the evidence (its text is still in the ring — the lock costs nothing that was sent while it formed)
        const int64_t phStart = ((int64_t)bphi - o7 + 1 + 4 * (int64_t)Ci) % Ci;
        int64_t s = n - 21 * Ki - 8; s -= ((s - phStart) % Ci + Ci) % Ci;
        s -= (int64_t)(kNW - 1) * Ci;
        // a new link: who was who belongs to the last one
        { const Link l = R.st_.link; const bool inv = R.st_.inverted; R.st_ = Status(); R.st_.link = l; R.st_.inverted = inv; }
        pend = Pending(); ctrlSince[0] = ctrlSince[1] = 0; figs[0] = figs[1] = false; lastSig[0] = lastSig[1] = 0;
        callIds.clear(); callVotes_.clear(); callStage = 0; callerStage = 0; callerIds.clear(); ckGot.clear(); idleRun = 0; swapExpected = 0;
        Station& A = S[0];
        A = Station(); A.on = true; A.s = (double)s; A.len = 3; A.df = kBinHz[bb]; A.pol = bpol; A.lastSeen = n;
        S[1] = Station();
        N0 = std::max(1e-12f, bGap);
        A.lastStart = A.s - C; A.lastEnd = A.lastStart + 21 * K;
        hunting = false;
        R.st_.inverted = bpol < 0;
        if (R.st_.link == Link::NoSignal || R.st_.link == Link::Searching || R.st_.link == Link::Ended) setLink(Link::Traffic);
        acquireOther();
    }
    /** With one station held, look for the other: the IRS's control signal in the gap, one of the six control words
     *  nearly every cycle at the same place. (A long-burst station that comes back after a change-over is found by the
     *  tracker itself.) */
    void acquireOther() {
        const int have = S[0].on ? 0 : (S[1].on ? 1 : -1);
        if (have < 0 || (S[0].on && S[1].on)) return;
        const Station& A = S[have];
        if (A.len != 3) return;
        if (quickOther(have)) return;
        if (n - huntFrom < (int64_t)kNW * Ci) return;
        int best = 0, bb = -1, bpol = 1; double brel = 0; float bE = 0;
        for (int b = 0; b < kBins; b++) {
            const Bin& B = bins[b];
            for (int pol = 0; pol < 2; pol++) {
                const auto& c = B.cnt[2 + pol];
                for (double rel = 21 * K + 2; rel <= 38 * K - 2; rel += 1) {
                    const int ph = (int)((((int64_t)std::lround(A.s + rel) + o7 - 1) % Ci + Ci) % Ci);
                    const int sc = c[(size_t)ph];
                    if (sc < kNW - 1 || sc < best) continue;
                    const float e = efoldMean(B, A.s + rel + K, 6 * K);
                    if (e < 2.5f * N0) continue;
                    if (sc > best || e > bE) { best = sc; bb = b; bpol = pol ? -1 : 1; brel = rel; bE = e; }
                }
            }
        }
        if (bb < 0) return;
        Station& Bst = S[1 - have];
        Bst = Station(); Bst.on = true; Bst.s = A.s + brel; Bst.len = 1; Bst.df = kBinHz[bb]; Bst.pol = bpol; Bst.lastSeen = n;
        Bst.lastStart = Bst.s - C; Bst.lastEnd = Bst.lastStart + 7 * K;
        Bst.d = brel - 21 * K; Bst.haveD = true;

    }

    /** ★ THE QUICK WAY IN for the second station: the slave answers a call within a cycle or two and then sends
     *  check-sum letters (M.625 §3.6), not control signals, so the fold above cannot see it until traffic has begun —
     *  and the identification would be gone. So in the gap after the held station's last block, the one strongest
     *  character there: a valid word 6 dB or more above the noise, in the same place (±3 samples) two cycles running, is the
     *  other station. Noise is not 6 dB over its own floor, as a valid word, in the same place twice. */
    bool quickOther(int have) {
        const Station& A = S[have];
        double base = A.lastStart;
        if (base + 45 * K + 2 > (double)n) base -= C;            // that gap is not all in yet: the cycle before
        if (base + 45 * K + 2 > (double)n || base < (double)(n - kRing + 64 * K)) return false;
        if (std::fabs(base - quickBase_) < C / 2) return false;  // already looked at this cycle
        const bool next = std::fabs(base - quickBase_ - C) < 8;
        quickBase_ = base;
        double bestE = 0, brel = 0; int bb = -1, bpol = 1;
        Station t; t.pol = 1;
        for (int b = 1; b < kBins - 1; b++) {
            t.df = kBinHz[b];
            for (double rel = 21 * K + 2; rel <= 38 * K - 2; rel += 4) {
                Char c; demodChar(t, base + rel, c);
                uint8_t hard = 0; for (int k = 0; k < 7; k++) if (c.d[k] > 0) hard = (uint8_t)(hard | (1 << k));
                const int pol = valid(hard) ? 1 : valid((uint8_t)(~hard & 0x7f)) ? -1 : 0;
                if (pol && c.e > 4 * N0 && c.e > bestE) { bestE = c.e; brel = rel; bb = b; bpol = pol; }
            }
        }
        if (bb < 0) { quickHave_ = false; return false; }
        // refine to the sample
        t.df = kBinHz[bb];
        for (double r = brel - 3; r <= brel + 3; r += 1) { Char c; demodChar(t, base + r, c); if (c.e > bestE) { bestE = c.e; brel = r; } }
        const bool same = next && quickHave_ && std::fabs(brel - quickRel_) <= 3 && quickPol_ == bpol;
        quickHave_ = true; quickRel_ = brel; quickPol_ = bpol;
        if (!same) return false;
        Station& Bst = S[1 - have];
        Bst = Station(); Bst.on = true; Bst.len = 1; Bst.df = kBinHz[bb]; Bst.pol = bpol; Bst.lastSeen = n;
        Bst.s = base + brel; while (Bst.s < A.s) Bst.s += C;      // its next burst after the held station's next
        Bst.lastStart = Bst.s - C; Bst.lastEnd = Bst.lastStart + 7 * K;
        Bst.d = brel - 21 * K; Bst.haveD = true;
        quickHave_ = false;
        return true;
    }
    double quickBase_ = -1e300, quickRel_ = 0; int quickPol_ = 0; bool quickHave_ = false;

    // ── demodulation of one character at a given start ──────────────────────────────────────────────────────────
    struct Char {
        double t = 0; float llr[7] = {0}; float d[7] = {0}; float mb[7] = {0}, my[7] = {0}; float eb[7] = {0}; float e = 0; float emax = 0; Word w; float p = 0;
        double afcNum = 0, afcDen = 0;
    };
    /** Seven one-bit matched filters on each tone (with the station's frequency error), half-bit sums kept for AFC. */
    void demodChar(const Station& st, double t0, Char& c) const {
        c.t = t0; c.e = 0; c.emax = 0; c.afcNum = c.afcDen = 0;
        const double fm = kShift / 2 + st.df, fs = -kShift / 2 + st.df;
        for (int i = 0; i < 7; i++) {
            const int64_t a = (int64_t)std::llround(t0 + i * K), b = (int64_t)std::llround(t0 + (i + 1) * K), mid = (a + b) / 2;
            cd pm = std::polar(1.0, -2 * M_PI * std::fmod(fm * (double)a / fi, 1.0)), ps = std::polar(1.0, -2 * M_PI * std::fmod(fs * (double)a / fi, 1.0));
            const cd sm = std::polar(1.0, -2 * M_PI * fm / fi), ss = std::polar(1.0, -2 * M_PI * fs / fi);
            cd m1{0, 0}, m2{0, 0}, s1{0, 0}, s2{0, 0};
            for (int64_t j = a; j < b; j++) {
                const size_t k = (size_t)(j & (kRing - 1));
                const cd z(zr[k], zi[k]);
                if (j < mid) { m1 += z * pm; s1 += z * ps; } else { m2 += z * pm; s2 += z * ps; }
                pm *= sm; ps *= ss;
            }
            const double M = std::abs(m1 + m2), Sv = std::abs(s1 + s2);
            c.d[i] = (float)(st.pol * (M - Sv));
            c.mb[i] = (float)(st.pol > 0 ? M : Sv); c.my[i] = (float)(st.pol > 0 ? Sv : M);   // the B (higher) and Y tones
            c.e += (float)(M * M + Sv * Sv);
            c.eb[i] = (float)(M * M + Sv * Sv);
            c.emax += (float)std::max(M * M, Sv * Sv);
            // AFC: the winning tone turns by 2π·Δf·(K/2)/fi between its two half-bit sums
            const cd r = M > Sv ? m2 * std::conj(m1) : s2 * std::conj(s1);
            c.afcNum += r.imag(); c.afcDen += r.real();
        }
        c.e /= 7; c.emax /= 7;
    }
    /** ln I0(x), the modified Bessel function — the series below 3.75, the asymptotic form above (good to ~1e-4). */
    static double lnI0(double x) {
        if (x < 3.75) { const double t = x * x / 4; return std::log(1 + t * (1 + t * (0.25 + t * (1.0 / 36 + t * (1.0 / 576 + t / 14400))))); }
        return x - 0.5 * std::log(2 * M_PI * x) + std::log(1 + 1 / (8 * x) + 9 / (128 * x * x));
    }
    /** ★★ SOFT VALUES FROM EACH TONE'S OWN AMPLITUDE (an automatic threshold correction). HF fades the two tones apart
     *  (selective fading): with one tone 15 dB down, "which tone is louder" is the noise's decision half the time. So
     *  each run first reads its words plainly, then measures the B tone's level on the elements it read as B and the
     *  Y tone's on those it read as Y — averaged with the station's last burst, the fade being far slower than a
     *  cycle — and gives each element the true non-coherent log-likelihood ratio for those two amplitudes:
     *      LLR = ln I0(aB·|B|/σ²) − aB²/2σ² − ln I0(aY·|Y|/σ²) + aY²/2σ²     (σ² per real dimension = N0/4)
     *  A faded tone's evidence then counts for what it is worth, a strong copy of a block for more than a weak one, and
     *  repeats can be summed as they are. */
    void scale(Char* c, int nc, float& A, Station* st = nullptr) const {
        double sB = 0, sY = 0; int nB = 0, nY = 0;
        for (int i = 0; i < nc; i++) {
            const Word w0 = mlWord(c[i].d);
            for (int b = 0; b < 7; b++) {
                if ((w0.w >> b) & 1) { sB += c[i].mb[b] * c[i].mb[b]; nB++; } else { sY += c[i].my[b] * c[i].my[b]; nY++; }
            }
        }
        const double s2 = std::max(1e-20, N0 / 4);              // per real dimension
        double aB = std::sqrt(std::max(0.0, (nB ? sB / nB : 0) - 2 * s2)), aY = std::sqrt(std::max(0.0, (nY ? sY / nY : 0) - 2 * s2));
        if (st) {
            if (st->haveA) { aB = 0.5 * (aB + st->aB); aY = 0.5 * (aY + st->aY); }
            st->aB = aB; st->aY = aY; st->haveA = true;
        }
        if (!o.atc) { const double m = 0.5 * (aB + aY); aB = aY = m; }
        A = (float)(0.5 * (aB + aY));
        for (int i = 0; i < nc; i++) {
            for (int b = 0; b < 7; b++) {
                const double l = lnI0(aB * c[i].mb[b] / s2) - aB * aB / (2 * s2) - lnI0(aY * c[i].my[b] / s2) + aY * aY / (2 * s2);
                c[i].llr[b] = (float)std::max(-60.0, std::min(60.0, l));
            }
            c[i].w = mlWord(c[i].llr);
        }
    }
    /** "Is a character of this station here?": its energy over the noise, and whether it reads as a valid word. */
    float presence(const Station& st, double t0) const {
        Char c; demodChar(st, t0, c);
        /* ★ A character has energy in EVERY element: one that is half silence and half someone else's signal has a fair
         *  MEAN energy and, by chance, may read as a valid word. So the level is the third-weakest element's: a
         *  character three-sevenths silent reads as noise, one with an element or two faded still counts. */
        float eb[7]; std::copy(c.eb, c.eb + 7, eb); std::nth_element(eb, eb + 2, eb + 7);
        const float x = 10 * std::log10(std::max(1e-6f, (float)(eb[2] / N0)));
        float pe = std::max(-2.f, std::min(2.f, (x - 2.5f) / 1.5f));
        uint8_t hard = 0; for (int b = 0; b < 7; b++) if (c.d[b] > 0) hard = (uint8_t)(hard | (1 << b));
        /* ★ Energy alone is not this station: the other's signal on this station's character grid is energetic too,
         *  but read a few bits out of step it is rarely a valid word. So the energy counts in full only for a word of the
         *  35; an energetic invalid word is close to no evidence either way. */
        const bool v = valid(hard);
        return v ? pe + 0.6f : 0.2f * pe - 0.4f;
    }

    // ── the tracker: one station's burst at a time, in time order ─────────────────────────────────────────────────
    /* ★★ WHERE EACH STATION'S NEXT BURST CAN START (M.476 §3.1.1.4–3.1.1.7). The master's transmissions start at the
     *  same moment of every cycle, whatever their length (start-fixed: "A"). The slave's start a fixed time tE after the
     *  master's signal ENDS (§3.1.1.6), so they move with the length of the master burst just before them (slave
     *  prediction: "B" = the other station's last end + this station's measured delay). Until the roles are known each
     *  station is read both ways; the first time the two predictions differ (a change-over: the master's block becomes
     *  a control signal), the evidence picks one — and that is the master, found without hearing the call. */
    struct Cand { double s; int len; float prior; int which; };
    static bool overlap1(double a0, double a1, double b0, double b1, double tol) { return std::min(a1, b1) - std::max(a0, b0) > tol; }
    bool overlapRun(double a0, double a1, double b0, double b1, double tol) const {
        return overlap1(a0, a1, b0, b1, tol) || overlap1(a0, a1, b0 - C, b1 - C, tol) || overlap1(a0, a1, b0 + C, b1 + C, tol);
    }
    /** The other station's air time near this burst: its last burst, and its next one (a block, to be safe). */
    bool otherBusy(int i, double a0, double a1) const {
        const Station& O = S[1 - i];
        if (!O.on) return false;
        return overlapRun(a0, a1, O.lastStart, O.lastEnd, K) || overlap1(a0, a1, O.s, O.s + 21 * K, K);
    }
    int nextStation() const {
        int b = -1;
        for (int i = 0; i < 2; i++) if (S[i].on && (b < 0 || S[i].s < S[b].s)) b = i;
        return b;
    }
    /** A station's burst is decided with the OTHER station's following burst in view (its start may hang on this one's
     *  length), so the decision waits until that is in the ring too. */
    int64_t dueFor(int i) const {
        const double last = S[1 - i].on ? std::max(S[i].s, S[1 - i].s) : S[i].s;
        return (int64_t)std::ceil(last + 35 * K) + 4;
    }
    void track() {
        for (;;) {
            if (hunting) return;
            const int i = nextStation();
            if (i < 0 || n < dueFor(i)) return;
            processStation(i);
            afterStation();
        }
    }

    /** The places one station's next burst may start: A (start-fixed, the master's rule) and B (after `prevEnd`, the
     *  end of the other station's burst before it, by this station's delay — the slave's rule). */
    void startsFor(int i, double prevEnd, std::vector<std::pair<double, int>>& v) const {
        const Station& X = S[i];
        v.clear();
        const double sA = X.s;
        double sB = sA; bool haveB = false;
        if (S[1 - i].on && X.haveD) { sB = prevEnd + X.d; haveB = std::fabs(sB - sA) > K / 2 && std::fabs(sB - sA) <= 14 * K + 4; }
        const int m = R.st_.master;
        if (m == i || !haveB) v.push_back({sA, 0});
        else if (m == 1 - i) v.push_back({sB, 1});
        else { v.push_back({sA, 0}); v.push_back({sB, 1}); }
    }
    float prior(const Station& X, int len) const {
        /* ★ The length only changes at a change-over (M.476 §3.1.6): outside one, staying as it was is worth three
         *  characters of evidence; just after a β α β block, any length is as likely. */
        return swapExpected > 0 ? 0.0f : (len == X.len ? 3.0f : 0.0f);
    }
    /** How silent [a0, a1) is, on this station's tones: + quiet, − a signal (clamped ±2). */
    float gapScore(const Station& st, double a0, double a1) const {
        const double fm = kShift / 2 + st.df, fs = -kShift / 2 + st.df;
        double e = 0; int nw = 0;
        for (double t = a0 + 2; t + K <= a1 - 2 + 0.5; t += K) {
            const int64_t a = (int64_t)std::llround(t), b = (int64_t)std::llround(t + K);
            cd pm = std::polar(1.0, -2 * M_PI * std::fmod(fm * (double)a / fi, 1.0)), ps = std::polar(1.0, -2 * M_PI * std::fmod(fs * (double)a / fi, 1.0));
            const cd sm = std::polar(1.0, -2 * M_PI * fm / fi), ss = std::polar(1.0, -2 * M_PI * fs / fi);
            cd m{0, 0}, s{0, 0};
            for (int64_t j = a; j < b; j++) { const size_t k = (size_t)(j & (kRing - 1)); const cd z(zr[k], zi[k]); m += z * pm; s += z * ps; pm *= sm; ps *= ss; }
            e += std::norm(m) + std::norm(s); nw++;
        }
        if (!nw) return 0;
        const float x = 10 * std::log10(std::max(1e-6f, (float)(e / nw / N0)));
        return -std::max(-2.f, std::min(2.f, (x - 3.0f) / 1.5f));
    }
    struct Pos { int st; double t; float p; };
    float pAt(std::vector<Pos>& cache, int st, double t) const {
        for (const Pos& q : cache) if (q.st == st && std::fabs(q.t - t) < 0.25) return q.p;
        const float p = presence(S[st], t);
        cache.push_back({st, t, p});
        return p;
    }

    void processStation(int i) {
        Station& X = S[i];
        const int j = 1 - i;
        const Station& O = S[j];
        std::vector<std::pair<double, int>> xs, os;
        startsFor(i, O.on ? O.lastEnd : -1e300, xs);
        struct XC { double s; int len; int which; float pr; };
        std::vector<XC> xc;
        for (auto& st : xs) for (int len : { 3, 1 }) xc.push_back({st.first, len, st.second, prior(X, len)});
        xc.push_back({X.s, 0, 0, 0.0f});
        std::vector<Pos> cache;
        // the union of characters each station's options could hold (the same set judged for every option)
        std::vector<double> wx, wo;
        auto addW = [this](std::vector<double>& w, double t) { for (double q : w) if (std::fabs(q - t) < K / 2) return; w.push_back(t); };
        for (const XC& c : xc) for (int k = 0; k < c.len; k++) addW(wx, c.s + 7 * K * k);
        std::vector<std::vector<XC>> ocs(xc.size());
        if (O.on) {
            for (size_t a = 0; a < xc.size(); a++) {
                const double end = xc[a].len ? xc[a].s + xc[a].len * 7 * K : X.s + X.len * 7 * K;
                startsFor(j, end, os);
                for (auto& st : os) for (int len : { 3, 1 }) ocs[a].push_back({st.first, len, st.second, prior(O, len)});
                ocs[a].push_back({O.s, 0, 0, 0.0f});
                for (const XC& c : ocs[a]) for (int k = 0; k < c.len; k++) addW(wo, c.s + 7 * K * k);
            }
        } else for (auto& v : ocs) v.push_back({0, 0, 0, 0.0f});
        auto inRun = [this](double t, const XC& c) { return c.len && t > c.s - K / 2 && t < c.s + c.len * 7 * K - K / 2; };
        auto hits = [this](double t, const XC& c) { return c.len && overlap1(t, t + 7 * K, c.s, c.s + c.len * 7 * K, K); };
        const XC lastO{O.lastStart, O.on ? (int)std::lround((O.lastEnd - O.lastStart) / (7 * K)) : 0, 0, 0}, lastX{X.lastStart, X.len, 0, 0};
        float best = -1e30f; int ba = -1, bb = -1;
        float bestRole[2] = {-1e30f, -1e30f};   // best with X read as master (A) / as slave (B), when they differ
        for (size_t a = 0; a < xc.size(); a++) {
            const XC& cx = xc[a];
            if (cx.len && hits(cx.s, lastO) && overlap1(cx.s, cx.s + cx.len * 7 * K, lastO.s, lastO.s + lastO.len * 7 * K, K)) continue;
            for (size_t b = 0; b < ocs[a].size(); b++) {
                const XC& co = ocs[a][b];
                if (cx.len && co.len && overlap1(cx.s, cx.s + cx.len * 7 * K, co.s, co.s + co.len * 7 * K, K)) continue;
                if (xs.size() == 2 && os.size() == 2 && cx.len && co.len && cx.which == 1 && co.which == 1) continue;   // two slaves
                /* ★ With both stations heard, the prior is on the ROLES, not each length: one sends blocks (the ISS), the
                 *  other control signals, and which one is the ISS changes only at a change-over. Outside one, the same
                 *  ISS is worth three characters; two blocks at once never happen. */
                float sc;
                if (O.on) {
                    const int iss = cx.len == 3 && co.len != 3 ? i : co.len == 3 && cx.len != 3 ? j : -1;
                    sc = (cx.len == 3 && co.len == 3) ? -3.0f : (swapExpected == 0 && iss >= 0 && iss == R.st_.iss) ? 3.0f : 0.0f;
                } else sc = cx.pr;
                for (double t : wx) { const bool in = inRun(t, cx); if (!in && (hits(t, co) || hits(t, lastO))) continue; const float p = pAt(cache, i, t); sc += in ? p : -p; }
                if (O.on) for (double t : wo) { const bool in = inRun(t, co); if (!in && (hits(t, cx) || hits(t, lastX))) continue; const float p = pAt(cache, j, t); sc += in ? p : -p; }
                // ★ the silence this pairing says lies between the two runs (tE, at least a bit long) must BE silent: a
                //   control signal then a block 30 ms later is not one 210 ms block, though both cover the same chars
                if (cx.len && co.len) {
                    const double e0 = cx.s + cx.len * 7 * K, s1 = co.s;
                    if (s1 - e0 >= K) sc += gapScore(X, e0, s1);
                }
                // the roles each pairing implies, when a prediction differed
                int role = -1;   // 0: X master, 1: X slave
                if (cx.len && xs.size() == 2) role = cx.which;
                else if (co.len && os.size() == 2) role = co.which == 1 ? 0 : 1;
                if (role >= 0) bestRole[role] = std::max(bestRole[role], sc);
                if (trace_ > 1) std::fprintf(stderr, "    cand X %d@%+.0f O %d@%+.0f  sc %.2f\n", cx.len, cx.s - X.s, co.len, co.s - X.s, sc);
                if (sc > best) { best = sc; ba = (int)a; bb = (int)b; }
            }
        }
        const XC c = ba >= 0 ? xc[(size_t)ba] : XC{X.s, 0, 0, 0};
        if (trace_) {
            std::fprintf(stderr, "t=%.2f S%d s=%.0f len%d df%+.0f N0=%.3g -> %d@%+.0f", n / fi, i, std::fmod(X.s, C), X.len, X.df, N0, c.len, c.s - X.s);
            if (O.on && ba >= 0) { const XC& co = ocs[(size_t)ba][(size_t)bb]; std::fprintf(stderr, "  (S%d %d@%+.0f)", j, co.len, co.s - O.s); }
            std::fprintf(stderr, "  roles %.1f/%.1f  p:", bestRole[0], bestRole[1]);
            for (const Pos& q : cache) std::fprintf(stderr, " S%d%+.0f:%+.1f", q.st, q.t - X.s, q.p);
            std::fprintf(stderr, "\n");
        }
        // the roles, when the two readings disagreed and the evidence chose clearly
        if (R.st_.master < 0 && bestRole[0] > -1e29f && bestRole[1] > -1e29f && std::fabs(bestRole[0] - bestRole[1]) >= 2.0f) {
            R.st_.master = bestRole[0] > bestRole[1] ? i : j; changed_ = true;
            event(std::string("master: station ") + (char)('1' + R.st_.master) + " (it keeps the cycle's start)");
        }
        if (!c.len) {
            X.lastStart = X.s; X.lastEnd = X.s + X.len * 7 * K;
            // nothing seen: a faded block is still a (light) copy for the combiner
            if (++X.silent <= 3 && X.len == 3 && o.combine) {
                Char ch[3];
                for (int k = 0; k < 3; k++) demodChar(X, X.s + 7 * K * k, ch[k]);
                float A; scale(ch, 3, A);
                onBlock(i, ch, true);
            }
            X.s += C;
            return;
        }
        X.silent = 0; X.lastSeen = n;
        /* ★ A station "present" every cycle whose words keep failing is being read out of step (a new link on another
         *  timing, or a lock that slipped): sixteen failed runs in a row (7 s, half the protocol's own 32-cycle
         *  rephase limit, M.625 §3.7.12) and it is let go, to be found again. */
        { bool bad = false; Char tc[3]; for (int k = 0; k < c.len; k++) { demodChar(X, c.s + 7 * K * k, tc[k]); if (!mlWord(tc[k].d).clean) bad = true; }
          X.failed = bad ? X.failed + 1 : 0;
          if (X.failed >= 16) { X.on = false; X.failed = 0; changed_ = true; return; } }
        // fine timing: the run read ±2 samples either side, the best score's parabola
        float sc[5]; int bd = 0;
        Char ch[3];
        for (int d = -2; d <= 2; d++) {
            Char tmp[3];
            for (int k = 0; k < c.len; k++) demodChar(X, c.s + d + 7 * K * k, tmp[k]);
            // judged on the tone differences themselves: the LLRs saturate on a strong signal and could not tell
            float s = 0; for (int k = 0; k < c.len; k++) s += mlWord(tmp[k].d).score;
            sc[d + 2] = s;
            if (d == -2 || s > sc[bd + 2]) { bd = d; for (int k = 0; k < c.len; k++) ch[k] = tmp[k]; }
        }
        float A; scale(ch, c.len, A, &X);
        double frac = 0;
        if (bd > -2 && bd < 2) {
            const double ym = sc[bd + 1], y0 = sc[bd + 2], yp = sc[bd + 3], den = ym - 2 * y0 + yp;
            if (den < 0) frac = std::max(-0.5, std::min(0.5, 0.5 * (ym - yp) / den));
        }
        /* ★ The timing moves only on a run that read cleanly, a third of the way: a noisy run's best offset is mostly
         *  the noise's (measured: at -3 dB, following every run cost more than not following at all). */
        bool runClean = true;
        for (int k = 0; k < c.len; k++) runClean = runClean && mlWord(ch[k].d).clean;
        const double tStart = c.s + (o.fineTiming && runClean ? 0.33 * (bd + frac) : 0.0);
        // AFC: the half-bit phase turn of the winning tone, over the run
        bool allClean = true;
        for (int k = 0; k < c.len; k++) allClean = allClean && ch[k].w.clean;
        if (o.afc && allClean) {
            double num = 0, den = 0;
            for (int k = 0; k < c.len; k++) { num += ch[k].afcNum; den += ch[k].afcDen; }
            if (den > 0) {
                const double err = std::atan2(num, den) * fi / (2 * M_PI * (K / 2));
                X.df = std::max(-80.0, std::min(80.0, X.df + 0.25 * err));
            }
        }
        const double snr = std::max(1e-3, (double)ch[0].e / N0 - 1);
        X.snr += (10 * std::log10(snr) - X.snr) * 0.2;
        X.ampl = A;
        // the noise, from this station's characters left empty that nobody was using
        if (ba >= 0) {
            const XC& co = ocs[(size_t)ba][(size_t)bb];
            for (double t : wx) {
                if (inRun(t, c) || hits(t, co) || hits(t, lastO)) continue;
                Char e; demodChar(X, t, e);
                if (e.e < 3 * N0) N0 += (e.e - N0) * 0.05;
            }
        }
        X.lastStart = tStart; X.lastEnd = tStart + c.len * 7 * K; X.len = c.len;
        if (O.on && O.lastEnd <= tStart + K) { X.d = tStart - O.lastEnd; X.haveD = true; }
        // the next start: a master's is this one + a cycle; a slave's is re-predicted from the master's burst anyway
        X.s = tStart + C;
        if (c.len == 3) onBlock(i, ch, false); else onControl(i, ch[0]);
    }
    int endDrop_ = 0;
    void afterStation() {
        /* ★ AFTER THE END, LET GO. Both stations revert to stand-by (M.476 §3.1.9.6–7): the next link — another pair, or
         *  the same one calling again — has its own timing, and holding on to this one's reads it a few bits out of step
         *  as garbage. A few bursts later (the acknowledgement, a repeated end block) the receiver hunts afresh. */
        if (endDrop_ > 0 && --endDrop_ == 0) { if (!pend.strong) pend.on = false; flushPending(); S[0].on = S[1].on = false; changed_ = true; }
        if (swapExpected > 0 && ++swapTick_ >= 2) { swapTick_ = 0; swapExpected--; }
        // ★ after the end of communication (M.476 §3.1.9) a few silent cycles are enough to say the link has gone
        const int lose = R.st_.link == Link::Ended ? 3 : kLose;
        for (int i = 0; i < 2; i++) if (S[i].on && S[i].silent > lose) { S[i].on = false; changed_ = true; }
        for (int i = 0; i < 2; i++) { R.st_.heard[i] = S[i].on; R.st_.snrDb[i] = S[i].snr; R.st_.offsetHz[i] = S[i].df; }
        if (!S[0].on && !S[1].on) {
            flushPending();
            restartHunt();
            setLink(R.st_.link == Link::Ended ? Link::Ended : Link::NoSignal);
            R.st_.iss = -1;   // ★ who was who stays on show until a new link is found (acquire resets it)
        }
        if (changed_) { changed_ = false; if (R.onStatus) R.onStatus(R.st_); }
    }
    int swapTick_ = 0;
    bool changed_ = false;
    int trace_ = std::getenv("SITOR_TRACE") ? std::atoi(std::getenv("SITOR_TRACE")) : 0;

    // ── the listener: what the blocks and control signals mean ──────────────────────────────────────────────────
    void setLink(Link l) { if (R.st_.link != l) { R.st_.link = l; changed_ = true; if (R.onStatus && hunting) { changed_ = false; R.onStatus(R.st_); } } }
    void event(const std::string& s) { if (R.onEvent) R.onEvent(s); }
    void emit(int side, char32_t c) { if (R.onChar) R.onChar(side, c); }

    void onControl(int st, const Char& c) {
        const uint8_t w = c.w.w;
        const bool ok = c.w.margin >= kMarginOk * 0.5f;
        int v = 7;
        if (ok) v = w == kCs1 ? 1 : w == kCs2 ? 2 : w == kCs3 ? 3 : w == kCs4 ? 4 : w == kCs5 ? 5 : w == kRq ? 6 : 7;
        // identification: the called station answers each identification block with a check-sum signal (M.625 §3.6)
        if (R.st_.link == Link::Identifying && ok && v == 7 && isIdSignal(letterOf(w))) {
            /* Check-sum signal k answers identification block k (§3.6.3–3.6.7) and is derived from the CALLED
             *  station's identity (§2.5): each one heard is checked against the identity the call named. Two right and
             *  none wrong confirm it (the first may come before this receiver has found the called station). */
            const int called = R.st_.master >= 0 ? 1 - R.st_.master : -1;
            if (called >= 0 && called == st && callerStage >= 1 && callerStage <= 3) {
                if (ckGot.size() < 3) ckGot.resize(3, ' ');
                ckGot[(size_t)callerStage - 1] = (char)letterOf(w);    // the check-sum signal as received
                // the reading of the call the check-sums bear out (a misread call block loses here)
                pickCalled(called);
                if (R.st_.ids[called].size() == 7 && !R.st_.idChecked && ckMatches(R.st_.ids[called]) >= 2 && ckMisses(R.st_.ids[called]) == 0) {
                    R.st_.idChecked = true; changed_ = true;
                    event("identity " + R.st_.numbers[called] + " confirmed by its check-sums");
                }
            }
        }
        if (v == 4 || v == 5) {    // M.625 §3.5.5 / §3.8.2: the called station asks for identification
            if (R.st_.link != Link::Identifying) { setLink(Link::Identifying); callerStage = 0; callerIds.clear(); ckGot.clear();
                event(v == 5 ? "rephasing: identification requested (CS5)" : "identification requested (CS4)"); }
            if (R.st_.master < 0) { R.st_.master = 1 - st; changed_ = true; }
        }
        if (v == 3 && R.st_.link != Link::ChangeOver) { setLink(Link::ChangeOver); event("change-over requested (CS3)"); }
        // ★ a 4-signal call answered with CS1/CS2 (M.625 §3.5.5–3.5.6, M.476 §3.1.4.4): traffic starts, no identification
        if ((v == 1 || v == 2) && R.st_.link == Link::Calling && callStage == 3) { pend = Pending(); setLink(Link::Traffic); }
        ctrlSince[st] = v;
    }

    void onBlock(int st, const Char* ch, bool weak) {
        if (weak && R.st_.link == Link::Ended) return;   // ★ the link has ended: silence is not a faded block
        uint8_t w[3]; bool clean = !weak; float minM = 1e30f;
        for (int k = 0; k < 3; k++) { w[k] = ch[k].w.w; clean = clean && ch[k].w.clean && ch[k].w.margin >= 1.0f; minM = std::min(minM, ch[k].w.margin); }
        const int req = ctrlSince[1 - st] >= 1 && ctrlSince[1 - st] <= 2 ? ctrlSince[1 - st] : 0;
        const int reqRaw = ctrlSince[1 - st];
        ctrlSince[1 - st] = 0;
        if (!weak) {
            if (R.st_.iss != st) {
                if (R.st_.iss >= 0 && pend.on && pend.st != st) flushPending();
                swapExpected = 0;          // the change-over has happened: the new ISS is sending
                R.st_.iss = st; changed_ = true;
            }
        }
        // service blocks that are not numbered (M.476 §3.1.2.6, M.625 §3.6, §3.7.11.5)
        const bool sure = clean || (!weak && minM >= kMarginOk);
        if (sure) {
            int nRq = 0, nId = 0;
            for (int k = 0; k < 3; k++) { nRq += w[k] == kRq; nId += isIdSignal(letterOf(w[k])) && w[k] != kRq; }
            if (nRq == 3) {
                R.st_.repeats++; changed_ = true;
                if (R.st_.link == Link::Identifying) { event("end of identification"); pend = Pending(); setLink(Link::Traffic); }
                return;
            }
            // the call (M.476 §3.1.4.3, M.625 §3.5.3)
            if (nRq == 1 && nId == 2) {
                const char a = (char)letterOf(w[0] == kRq ? w[1] : w[0]), b = (char)letterOf(w[2] == kRq ? w[1] : w[2]);
                if (w[1] == kRq) { callIds = std::string(1, a) + b; callStage = 1; }
                else if (callStage == 1 && w[2] == kRq) { finishCall(st, callIds + a + b); }
                else if (callStage == 1 && w[0] == kRq) { callIds += std::string(1, a) + b; callStage = 2; }
                if (R.st_.link != Link::Calling) setLink(Link::Calling);
                if (R.st_.master != st) { R.st_.master = st; changed_ = true; }
                return;
            }
            if (nId == 3 && callStage == 2 && R.st_.link == Link::Calling) {
                std::string ids = callIds; for (int k = 0; k < 3; k++) ids += (char)letterOf(w[k]);
                finishCall(st, ids);
                return;
            }
            // the caller's identification blocks (M.625 §3.6.2–3.6.6)
            if (w[1] == kAlpha && w[0] != kAlpha && isIdSignal(letterOf(w[0])) && isIdSignal(letterOf(w[2]))) {
                callerIds = std::string(1, (char)letterOf(w[0])) + (char)letterOf(w[2]); callerStage = 1;
                if (R.st_.link != Link::Identifying) setLink(Link::Identifying);
                return;
            }
            if (w[0] == kAlpha && w[1] != kAlpha && isIdSignal(letterOf(w[1])) && isIdSignal(letterOf(w[2]))) {
                if (callerStage == 1) { callerIds += std::string(1, (char)letterOf(w[1])) + (char)letterOf(w[2]); callerStage = 2; }
                return;
            }
            if (nId == 3 && callerStage == 2 && R.st_.link == Link::Identifying) {
                for (int k = 0; k < 3; k++) callerIds += (char)letterOf(w[k]);
                callerStage = 3;
                R.st_.ids[st] = callerIds; R.st_.numbers[st] = identityNumber(callerIds);
                if (R.st_.master < 0) R.st_.master = st;
                changed_ = true;
                event("caller " + callerIds + (R.st_.numbers[st].empty() ? "" : " (" + R.st_.numbers[st] + ")"));
                return;
            }
            if (R.st_.link == Link::Identifying && callerStage == 3 && nId == 3) return;   // a repeated identification block 3
        }
        /* ★ Call blocks and identification blocks are never traffic (M.625 §3.5–3.6). One that did not read cleanly
         *  must not be held as a "block" and printed when traffic starts — the protocol sends it again anyway. */
        if (R.st_.link == Link::Calling || R.st_.link == Link::Identifying) {
            // …but a CLEAN block that is none of those means traffic has begun (its CS1 was not heard)
            if (!clean) return;
            pend = Pending(); setLink(Link::Traffic);
        }
        // ── a numbered information block: one copy of it ──
        if (!pend.on || pend.st != st) { if (weak) return; startPending(st, req, ch, clean); return; }
        // ★ a faded block the tracker could not call present: a NEW block if the IRS asked for the next one (its control
        //   signal changed) — kept as a weak copy, printed with its best guess and '_' where nothing is readable
        if (weak && req && pend.req && req != pend.req) { flushPending(); weakStart_ = true; startPending(st, req, ch, false); weakStart_ = false; return; }
        bool repeat;
        const bool service = (w[0] == kBeta && w[1] == kAlpha && w[2] == kBeta) || (w[0] == kAlpha && w[1] == kAlpha && w[2] == kAlpha);
        if (clean && pend.printed && service && std::equal(w, w + 3, pend.words)) repeat = true;   // a change-over / end block sent again
        else if (req && pend.req) repeat = req == pend.req;
        else if (reqRaw == 3) repeat = false;
        else if (clean && pend.printed) repeat = std::equal(w, w + 3, pend.words);
        else {
            double num = 0, den = 0;
            for (int k = 0; k < 3; k++) for (int b = 0; b < 7; b++) {
                const float a = ch[k].llr[b], p = pend.llr[k * 7 + b];
                num += a * (p > 0 ? 1 : -1); den += std::fabs(a);
            }
            repeat = den > 0 && num / den > (weak ? 0.0 : 0.55);
            if (clean && pend.hadClean && !std::equal(w, w + 3, pend.words)) repeat = false;
        }
        if (trace_ >= 3) std::fprintf(stderr, "  block S%d req %d raw %d clean %d weak %d -> %s (pend req %d copies %d printed %d)\n", st, req, reqRaw, clean, weak, repeat ? "REPEAT" : "new", pend.req, pend.copies, pend.printed);
        if (weak && !repeat) return;
        if (!repeat) { flushPending(); startPending(st, req, ch, clean); return; }
        if (!weak) { R.st_.repeats++; changed_ = true; pend.strong = true; }
        if (!pend.req && req) pend.req = req;
        addCopy(ch, clean);
    }
    bool weakStart_ = false;
    /* ★ THE CALL IS SENT UNTIL IT IS ANSWERED (M.625 §3.5.4), so it is usually heard several times: each reading is a
     *  vote, and the identity shown is the one read most often (the latest of a tie). One block misread by a crash of
     *  static — a valid identity, but the wrong one — is outvoted by the next two copies. An identity that is not one
     *  (a 4-signal call whose letters fit no pattern of M.491 Table I) is not counted at all. */
    std::map<std::string, int> callVotes_;
    int ckMatches(const std::string& id) const {
        if (id.size() != 7 || ckGot.size() < 3) return 0;
        const std::string ck = checksums(id); int n = 0;
        for (int k = 0; k < 3; k++) n += ckGot[(size_t)k] != ' ' && ckGot[(size_t)k] == ck[(size_t)k];
        return n;
    }
    int ckMisses(const std::string& id) const {
        if (id.size() != 7 || ckGot.size() < 3) return 0;
        const std::string ck = checksums(id); int n = 0;
        for (int k = 0; k < 3; k++) n += ckGot[(size_t)k] != ' ' && ckGot[(size_t)k] != ck[(size_t)k];
        return n;
    }
    /** The called identity shown: the reading the check-sums bear out best, then the one read most often; on a tie the
     *  one already shown stays. */
    void pickCalled(int called) {
        std::string best = R.st_.ids[called]; int bs = -1;
        auto score = [&](const std::string& id, int votes) { return 100 * (ckMatches(id) - ckMisses(id)) + votes; };
        if (!best.empty() && callVotes_.count(best)) bs = score(best, callVotes_[best]);
        for (const auto& kv : callVotes_) { const int sc = score(kv.first, kv.second); if (sc > bs) { bs = sc; best = kv.first; } }
        if (best.empty() || best == R.st_.ids[called]) return;
        R.st_.ids[called] = best; R.st_.numbers[called] = identityNumber(best); changed_ = true;
        event("call to " + best + (R.st_.numbers[called].empty() ? "" : " (" + R.st_.numbers[called] + ")"));
    }
    void finishCall(int st, const std::string& read) {
        callStage = 3;
        const int called = 1 - st;
        if (identityNumber(read).empty()) return;
        ++callVotes_[read];
        pickCalled(called);
    }
    void startPending(int st, int req, const Char* ch, bool clean) {
        pend = Pending(); pend.on = true; pend.st = st; pend.req = req; pend.strong = !weakStart_;
        std::memset(pend.llr, 0, sizeof pend.llr);
        addCopy(ch, clean);
    }
    void addCopy(const Char* ch, bool clean) {
        if (pend.printed) { pend.copies++; return; }
        if (!o.combine || clean) std::memset(pend.llr, 0, sizeof pend.llr);   // a clean copy stands on its own
        for (int k = 0; k < 3; k++) for (int b = 0; b < 7; b++) pend.llr[k * 7 + b] += ch[k].llr[b];
        pend.copies++;
        if (clean) { for (int k = 0; k < 3; k++) pend.words[k] = ch[k].w.w; pend.hadClean = true; printPending(false); return; }
        /* ★ One copy that did not read cleanly is NOT printed yet, however good its best guess looks: if the IRS could
         *  not read it either, a repeat is coming, and the two together are worth far more than either; if the IRS
         *  could, the next block lets it go with its best guess (flushPending). */
        if (!o.combine || pend.copies < 2) return;
        Word ws[3]; bool ok = true;
        for (int k = 0; k < 3; k++) { ws[k] = mlWord(&pend.llr[k * 7]); ok = ok && ws[k].margin >= kMarginOk; }
        if (ok) {
            for (int k = 0; k < 3; k++) pend.words[k] = ws[k].w;
            if (pend.copies > 1) R.st_.combined++;
            printPending(false);
        }
    }
    /** The block is let go (the next one has come): print the best that its copies make, '_' where nothing could. */
    void flushPending() {
        if (!pend.on || pend.printed) { pend.on = false; return; }
        printPending(true);
        pend.on = false;
    }
    void printPending(bool forced) {
        uint8_t w[3]; bool lostC[3] = {false, false, false};
        for (int k = 0; k < 3; k++) {
            if (pend.hadClean) { w[k] = pend.words[k]; continue; }
            const Word ws = mlWord(&pend.llr[k * 7]);
            w[k] = ws.w;
            if (forced && ws.margin < kMarginLost) lostC[k] = true;
        }
        pend.printed = true;
        std::copy(w, w + 3, pend.words);
        const int st = pend.st;
        // the content of a numbered block
        if (w[0] == kBeta && w[1] == kAlpha && w[2] == kBeta) {   // M.476 §3.1.6.1: the ISS's answer to CS3
            setLink(Link::ChangeOver); swapExpected = 4; event("change-over");
            return;
        }
        if (w[0] == kAlpha && w[1] == kAlpha && w[2] == kAlpha) {  // M.476 §3.1.9.3: end of communication
            setLink(Link::Ended); event("end of communication");
            endDrop_ = 3;   // ★ M.476 §3.1.9.6–7: the IRS acknowledges, and both go to stand-by — so does this receiver
            return;
        }
        if (w[0] == kBeta && w[1] == kBeta && w[2] == kBeta) {
            if (++idleRun >= 2 && R.st_.link == Link::Traffic) setLink(Link::Idle);
            return;
        }
        R.st_.blocks++; changed_ = true;
        bool printed = false;
        for (int k = 0; k < 3; k++) {
            if (lostC[k]) { emit(st, U'_'); R.st_.lost++; printed = true; continue; }
            const uint8_t c = w[k];
            if (c == kBeta || c == kAlpha || c == kNoInfo || c == kRq || c == kCr) continue;
            if (c == kLtrs) { figs[st] = false; continue; }
            if (c == kFigs) { figs[st] = true; continue; }
            if (c == kLf) { emit(st, U'\n'); printed = true; continue; }
            if (c == kSpace) { emit(st, U' '); printed = true; continue; }
            if (figs[st]) {
                const char32_t f = figureOf(c);
                if (c == letterSignal(U'D')) { event("WRU (who are you?) — answer-back requested"); continue; }
                if (!f) continue;
                // M.476 §3.1.6.1 / M.625 §3.7.11: figure shift, '+', '?' is the ISS asking to change over
                const bool over = f == U'?' && lastSig[st] == U'+';
                lastSig[st] = (int)f;
                emit(st, f); printed = true;
                if (over) event("over (+?)");
            } else {
                const char32_t l = letterOf(c);
                if (!l) continue;
                lastSig[st] = (int)l;
                emit(st, l); printed = true;
            }
        }
        idleRun = 0;
        if (printed && R.st_.link != Link::Traffic) setLink(Link::Traffic);
    }
};

Receiver::Receiver(double rate, double centreHz, const Options& o) : p_(new Impl(*this, rate, centreHz, o)), centre_(centreHz) {}
Receiver::~Receiver() { delete p_; }
void Receiver::process(const float* x, int n) { for (int i = 0; i < n; i++) p_->input(x[i]); }
void Receiver::process(const int16_t* x, int n) { for (int i = 0; i < n; i++) p_->input(x[i] * (1.0f / 32768.0f)); }
void Receiver::setCentre(double hz) { centre_ = hz; p_->setCentre(hz); p_->resetLink(); }
void Receiver::reset() { p_->resetLink(); }

} // namespace sitora
} // namespace vibe
