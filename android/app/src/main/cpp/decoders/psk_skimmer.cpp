// decoders/psk_skimmer.cpp — see psk_skimmer.h for the design. Original VibeSDR code, from the published PSK31
// description (G3PLX) and textbook DSP: overlap-save fast convolution, a second-order Costas loop, square-law symbol
// timing, differential detection, the Varicode alphabet.
#include "psk_skimmer.h"
#include "cw_skimmer.h"                    // ★ the callsign rules (isCallsign / identify) are the same on every keyboard mode
#include "../vibedsp/simd_internal.h"     // ★ VIBE_NEON / VIBE_SSE (and VIBE_FORCE_SCALAR) — the project's one SIMD switch
#include "../vibedsp/third_party/pffft/pffft.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#ifdef PSK_DEBUG
#include <cstdio>
#include <cstdlib>
#endif

namespace vibe {
namespace psk {

// ── Varicode (G3PLX) ───────────────────────────────────────────────────────────────────────────────────────────────
// ★ The alphabet as published, ASCII 0–127 in order. Its shape is checked by the tests: 128 distinct codes, each
//   starting and ending in 1 with no "00" inside, and as many of each length as there can be (1, 1, 2, 3, 5, 8, 13, 21,
//   34 — every possible code up to nine bits — then 40 of ten): the commonest characters get the shortest codes.
namespace {
const char* const kVaricode[128] = {
    "1010101011","1011011011","1011101101","1101110111","1011101011","1101011111","1011101111","1011111101",
    "1011111111","11101111","11101","1101101111","1011011101","11111","1101110101","1110101011",
    "1011110111","1011110101","1110101101","1110101111","1101011011","1101101011","1101101101","1101010111",
    "1101111011","1101111101","1110110111","1101010101","1101011101","1110111011","1011111011","1101111111",
    "1","111111111","101011111","111110101","111011011","1011010101","1010111011","101111111",
    "11111011","11110111","101101111","111011111","1110101","110101","1010111","110101111",
    "10110111","10111101","11101101","11111111","101110111","101011011","101101011","110101101",
    "110101011","110110111","11110101","110111101","111101101","1010101","111010111","1010101111",
    "1010111101","1111101","11101011","10101101","10110101","1110111","11011011","11111101",
    "101010101","1111111","111111101","101111101","11010111","10111011","11011101","10101011",
    "11010101","111011101","10101111","1101111","1101101","101010111","110110101","101011101",
    "101110101","101111011","1010101101","111110111","111101111","111111011","1010111111","101101101",
    "1011011111","1011","1011111","101111","101101","11","111101","1011011",
    "101011","1101","111101011","10111111","11011","111011","1111","111",
    "111111","110111111","10101","10111","101","110111","1111011","1101011",
    "11011111","1011101","111010101","1010110111","110111011","1010110101","1011010111","1110110101",
};
struct Decode {
    int16_t c[1024];   // ★ a code read as an integer (≤ 10 bits, leading 1) → its character
    Decode() {
        for (auto& x : c) x = -1;
        for (int a = 0; a < 128; ++a) {
            uint32_t v = 0;
            for (const char* p = kVaricode[a]; *p; ++p) v = v * 2 + (*p == '1');
            c[v] = (int16_t)a;
        }
    }
};
const Decode& decodeTable() { static const Decode d; return d; }
} // namespace

const char* varicode(int a) { return a >= 0 && a < 128 ? kVaricode[a] : ""; }
int varicodeChar(uint32_t code) { return code < 1024 ? decodeTable().c[code] : -1; }
const char* qualityWord(float q) { return q >= 0.75f ? "high" : q >= 0.5f ? "medium" : q >= 0.3f ? "low" : "poor"; }

// ── The kernels ───────────────────────────────────────────────────────────────────────────────────────────────────
// ★★ NEON (ARMv7-safe intrinsics only — the Pi 2 runs what arm64 runs), SSE2 (nothing newer: a 32-bit Atom runs it) and
//    the scalar reference (VIBE_FORCE_SCALAR builds it anywhere; the ARMv6 Pi Zero/1 runs it).
namespace {
/** ★ THE PER-BIN PASS over the searched bins: the Hann window applied in the frequency domain (½X[k] − ¼X[k−1] −
 *  ¼X[k+1] — the forward FFT is unwindowed, because it is also the channeliser's overlap-save transform), its power, and
 *  two running averages: F (≈ 1 s, the carrier finder) and S (≈ 6 s, what the floor is taken from).
 *  `x` is n+2 interleaved complex bins: x[1..n] are the searched ones, x[0] and x[n+1] their neighbours. */
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
/** out = in · m, complex (interleaved) by real — a channel's mask on its 128 bins. */
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
/** ★ The NCO: re/im[i] = (z[i] · g) · e^{-j(φ + iω)} for interleaved z, n samples, deinterleaved out. Four phasors at
 *  once (stepped by e^{-j4ω}); the caller renormalises φ each block, so nothing drifts. */
void mixBlock(const float* z, int n, float gr, float gi, double ph, double w, float* outRe, float* outIm) {
    int i = 0;
#if VIBE_NEON || VIBE_SSE
    float pr[4], pi[4];
    for (int k = 0; k < 4; ++k) { pr[k] = (float)std::cos(ph + k * w); pi[k] = (float)-std::sin(ph + k * w); }
    const float sr = (float)std::cos(4 * w), si = (float)-std::sin(4 * w);
#endif
#if VIBE_NEON
    float32x4_t vpr = vld1q_f32(pr), vpi = vld1q_f32(pi);
    const float32x4_t vsr = vdupq_n_f32(sr), vsi = vdupq_n_f32(si), vgr = vdupq_n_f32(gr), vgi = vdupq_n_f32(gi);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(z + 2 * i);
        // (v · g)
        const float32x4_t ar = vmlsq_f32(vmulq_f32(v.val[0], vgr), v.val[1], vgi);
        const float32x4_t ai = vmlaq_f32(vmulq_f32(v.val[0], vgi), v.val[1], vgr);
        // · phasor
        vst1q_f32(outRe + i, vmlsq_f32(vmulq_f32(ar, vpr), ai, vpi));
        vst1q_f32(outIm + i, vmlaq_f32(vmulq_f32(ar, vpi), ai, vpr));
        const float32x4_t nr = vmlsq_f32(vmulq_f32(vpr, vsr), vpi, vsi);
        vpi = vmlaq_f32(vmulq_f32(vpr, vsi), vpi, vsr); vpr = nr;
    }
#elif VIBE_SSE
    __m128 vpr = _mm_loadu_ps(pr), vpi = _mm_loadu_ps(pi);
    const __m128 vsr = _mm_set1_ps(sr), vsi = _mm_set1_ps(si), vgr = _mm_set1_ps(gr), vgi = _mm_set1_ps(gi);
    for (; i + 4 <= n; i += 4) {
        __m128 vr, vi; vibedsp::sseLoad2(z + 2 * i, vr, vi);
        const __m128 ar = _mm_sub_ps(_mm_mul_ps(vr, vgr), _mm_mul_ps(vi, vgi));
        const __m128 ai = _mm_add_ps(_mm_mul_ps(vr, vgi), _mm_mul_ps(vi, vgr));
        _mm_storeu_ps(outRe + i, _mm_sub_ps(_mm_mul_ps(ar, vpr), _mm_mul_ps(ai, vpi)));
        _mm_storeu_ps(outIm + i, _mm_add_ps(_mm_mul_ps(ar, vpi), _mm_mul_ps(ai, vpr)));
        const __m128 nr = _mm_sub_ps(_mm_mul_ps(vpr, vsr), _mm_mul_ps(vpi, vsi));
        vpi = _mm_add_ps(_mm_mul_ps(vpr, vsi), _mm_mul_ps(vpi, vsr)); vpr = nr;
    }
#endif
    for (; i < n; ++i) {
        const double a = ph + i * w;
        const float c = (float)std::cos(a), s = (float)-std::sin(a);
        const float ar = z[2 * i] * gr - z[2 * i + 1] * gi, ai = z[2 * i] * gi + z[2 * i + 1] * gr;
        outRe[i] = ar * c - ai * s; outIm[i] = ar * s + ai * c;
    }
}
/** ★ The matched filter, n outputs at once: y[i] = Σ_{t<L} h[t]·x[i+t] (x holds n+L−1 samples). Vectorised ACROSS the
 *  outputs — four at a time, each tap broadcast — so a short filter (13–25 taps) still fills the lanes. */
void firBlock(const float* x, int n, const float* h, int L, float* y) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        float32x4_t acc = vdupq_n_f32(0.0f);
        for (int t = 0; t < L; ++t) acc = vmlaq_f32(acc, vld1q_f32(x + i + t), vdupq_n_f32(h[t]));
        vst1q_f32(y + i, acc);
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        __m128 acc = _mm_setzero_ps();
        for (int t = 0; t < L; ++t) acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(x + i + t), _mm_set1_ps(h[t])));
        _mm_storeu_ps(y + i, acc);
    }
#endif
    for (; i < n; ++i) { float a = 0; for (int t = 0; t < L; ++t) a += h[t] * x[i + t]; y[i] = a; }
}

using cf = std::complex<float>;
constexpr double kTwoPi = 6.283185307179586;
constexpr int kM = 128;                 // ★ a channel: 128 bins of the shared FFT → ≈ 375 Hz complex out
constexpr int kOut = kM / 2;            //   samples a channel yields per hop (the middle half of each inverse)
constexpr int kMaxTaps = 64;            //   the longest matched filter (PSK31 at the highest channel rate)
constexpr int kRing = 512;
constexpr int kLine = 512;              //   channel samples (≈ 1.4 s) in a squared-signal line search              //   symbols of short-SNR history (a char's gate looks back over its own)
inline float wrapPi(float a) { while (a > (float)M_PI) a -= (float)kTwoPi; while (a < -(float)M_PI) a += (float)kTwoPi; return a; }
bool goodChar(int c) {
    if (c == '\n' || c == '\r' || c == ' ') return true;
    if (c >= '0' && c <= '9') return true;
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return true;
    return std::strchr(".,?!/-=:;'\"()+@#&*%$<>_[]", c) != nullptr && c != 0;
}
bool isLetter(int c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
/** ★★ WORDS A READER WOULD KNOW — the last test before a signal is shown. Noise and non-PSK signals read as PSK make
 *  Varicode's short codes (e t o a i n and space) into "words" of their own — "ere en e g o ee a -etye thae tl" off a
 *  wide data signal at 7.0495 MHz passed every other gate. Real PSK text is QSO English and its abbreviations, numbers
 *  and callsigns: of the words held while tentative, at least three and 30 % must be one of those. */
bool knownWord(const std::string& w) {
    static const char* const kWords[] = {
        "cq","de","k","kn","sk","pse","tnx","tks","thx","fb","om","yl","op","name","qth","rst","ur","hw","cpy","copy","es","73","88",
        "gm","ga","ge","gn","dx","qso","qsl","qrz","qrl","qrm","qrn","qsb","rig","ant","pwr","wx","temp","hr","here","btu","bk",
        "the","and","to","of","a","in","is","it","you","for","on","my","me","i","am","at","be","are","this","that","with","as",
        "was","so","all","good","nice","thanks","thank","very","many","best","fine","hope","see","again","from","your","not","no",
        "yes","have","has","will","now","today","here","there","info","station","report","call","contact","psk","psk31","psk63",
        "bpsk","rtty","ft8","qrp","dipole","yagi","vertical","watts","w","hi","mni","tu","agn","sri","bye","ciao","care","band",
        "and","or","but","we","he","she","they","our","his","her","one","two","new","last","next","time","day","weather",
        "loc","locator","grid","age","years","since","licensed","radio","amateur","club","73s","cul","gl","gd","vy","wl" };
    for (const char* k : kWords) if (w == k) return true;
    bool digit = false, alpha = false;
    for (char c : w) { digit |= c >= '0' && c <= '9'; alpha |= isLetter((unsigned char)c); }
    if (digit && !alpha) return true;                                   // a number: a report, a time, a power
    if (digit && alpha) { std::string u = w; for (auto& c : u) c = (char)std::toupper((unsigned char)c); return cw::isCallsign(u); }
    return false;
}
bool readsAsText(const std::string& t) {
    int words = 0, known = 0; std::string w;
    // ★ a one-letter word proves nothing (noise makes e, t, a, i all day) — except K, the over's end
    auto flush = [&] { if (!w.empty()) { ++words; known += (w.size() > 1 || w == "k") && knownWord(w); w.clear(); } };
    for (char c : t) {
        if (std::isalnum((unsigned char)c)) w.push_back((char)std::tolower((unsigned char)c)); else flush();
    }
    flush();
    return words >= 4 && known >= 3 && known * 10 >= words * 3;
}
} // namespace

// ── The engine ────────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
// ★ Thresholds (Es/N0 from the constellation, dB). Measured on the bench: pure noise reads ≈ −3 dB (σ ≈ 1 dB over 32
//   symbols); PSK31 at Es/N0 7 dB copies at a few % character errors.
// ★★ THE QUALITY MEASURE: every symbol's error vector — its distance from the ideal BPSK point (the Costas reference and
//    the short amplitude average), squared, over that amplitude squared. Its mean over any stretch of symbols is an
//    Es/N0 for that stretch alone (1 / mean): a character is judged by its OWN symbols and the few after it, not by an
//    average still climbing out of the seconds before the loops locked. Amplitude counts as well as phase: a phase-only
//    measure passed RTTY, CW and noise-like signals (a loop that follows phase makes any slow phase look clean).
//    Pure noise reads ≈ −3 dB; PSK31 copies cleanly from ≈ +6.
constexpr float kConfirmDb = 5.0f;      // over the last 64 symbols, for a signal to be shown at all
constexpr float kCharDb = 3.0f;         // over a character's own symbols and the kCommitSyms after it, to count toward
                                        //   confirming a signal
constexpr float kCharDbLive = 1.0f;     // …and to be shown, once the signal is confirmed (and its long average holds)
constexpr float kCharDbHeld = 2.0f;     // …and what was held while it was tentative (its loops were still settling)
constexpr float kKeepDb = 2.0f;         // the long (≈ 1 s) EVM average below which, for kLostSec, a signal has gone
constexpr double kLostSec = 6.0;
constexpr double kProbeSec = 12.0;      // a tentative carrier that has not confirmed in this long is dropped
constexpr int kCommitSyms = 12;         // ★ symbols a character must be FOLLOWED by before it is committed
// ★ The loops, per symbol (measured on the bench — scripts/test-psk.sh): symbol timing from the envelope's baud-rate
//   component averaged over 16 symbols, nudged 12 % toward it a symbol; the Costas loop critically damped-ish (k2 large
//   enough to follow a 1 Hz/s drift with ≈ 0.1 rad of lag; 0.003 lost the copy at 0.5 Hz/s).
constexpr double kTimingSyms = 16.0, kTimingGain = 0.12;
constexpr float kCostas1 = 0.20f, kCostas2 = 0.02f;

struct PendingChar { int c; uint64_t sym0, sym1; };

struct Lane {
    double baud = 31.25, sps = 12, T = 0.032;
    int L = 25;
    float h[kMaxTaps] = {};
    // timing (square-law: the envelope's baud-rate component)
    std::vector<float> yr, yi;          // this block's filter outputs
    cf yh[32]; uint64_t n = 0;          // the last 32 outputs (interpolation, and the boundary before a symbol), counter
    cf C{0, 0}; float eAvg = 0;
    double tNext = 0;
    // detection
    cf prev{0, 0}; bool havePrev = false; float prevA = 0;
    // Costas loop (second order, at the symbol rate) + the slow FLL that keeps the channel centred
    float theta = 0, omega = 0; bool locked = false;
    cf fll{0, 0};
    // quality (EVM → Es/N0): short ≈ 8 symbols, long ≈ 32
    float As = 0, Vs = 1, Al = 0, Vl = 1;
    float snrS = -10, snrL = -10;       // EVM → Es/N0 (dB), short and long
    float eRing[kRing] = {};            // ★ each symbol's |error|² / A² (see kConfirmDb)
    uint64_t sym = 0;
    // Varicode
    uint32_t cur = 0; int len = 0; uint64_t codeSym0 = 0;
    int zeroRun = 0, onesRun = 0;
    // ★★ RUNS OF ONES. No Varicode character has more than nine ones running, and every one ends "…1" + "00": real PSK
    //    text cannot hold a run of a dozen. A CARRIER can — and a carrier (a CW key-down, an FSK tone, an FT8 symbol, a
    //    birdie) has a CLEAN constellation, so it passes every phase-quality test while its noise-made "00"s decode to
    //    the all-ones codes: e o n p I 3 ! and space. Seen on 30 m, 17 m, 12 m and 10 m clips before this test.
    uint64_t longOnes[8] = {}; int longOnesN = 0;
    float zeroFrac = 0.5f;              // ★ the share of reversals (≈ 128 bits): text 0.3–0.5, a carrier ≈ its BER
    // ★★ …and the share of bits in runs of SEVEN or more ones (≈ 256 bits). Only I, J, 3, ! and BS hold such a run, so
    //    text keeps it to a few %; an FSK signal read as BPSK (FT8: a tone held 160 ms = ten PSK63 symbols of "no
    //    change", then a jump) decodes to exactly those — "o,3IKII3i3e" from the 30 m FT8 segment — and scores 30 %+.
    float ones7 = 0.0f;
    bool longOnesNear(uint64_t a, uint64_t b) const {
        for (int i = 0; i < std::min(longOnesN, 8); ++i) if (longOnes[i] + 12 >= a && longOnes[i] <= b + 12) return true;
        return false;
    }
    std::deque<PendingChar> pend;
    std::vector<std::pair<char, float>> heldQ;   // ★ characters decoded while tentative, each with its quality
    // ★ the last 128 decisions and when they were made (the ghost test)
    uint8_t bitRing[128] = {}; double tRing[128] = {};
    unsigned decoded = 0, good = 0;     // characters decoded (gate or not) and plausible ones, over the lane's life
    std::deque<uint8_t> recentGood;     // plausibility of the last 16 decoded
    // ★★ THE TEXT'S TEXTURE: the last 48 characters decoded (gate or not). RTTY or a wide data signal read as PSK can
    //    hold a clean-enough constellation and make only "good" characters — and still be garbage: "t  t  t  t" (a space
    //    every other character), "rr==rrrr==rr" (three different characters), "ro su f elt notet t" (words of one or two
    //    letters). Real text has spaces about one character in six, a dozen or more different characters in 48, and
    //    words a few letters long. All three are asked, before a signal is shown and before each character is.
    std::string recentText;
    bool textureOk() const { return textTexture(recentText); }
    /** ★ The live bar, once a signal is shown: only text gone grossly wrong (a CQ — "cq cq cq de" — is words of two
     *  letters, and must pass): spaces over 45 %, or fewer than six different characters in the last 48. */
    bool textureGross() const {
        if (recentText.size() < 32) return false;
        int spaces = 0; bool seen[128] = {}; int distinct = 0;
        for (char c : recentText) { const unsigned char u = (unsigned char)c & 0x7f; if (!seen[u]) { seen[u] = true; ++distinct; } spaces += c == ' '; }
        return spaces > 0.45 * (double)recentText.size() || distinct < 6;
    }
    static bool textTexture(const std::string& t) {
        const size_t n = t.size();
        if (n < 16) return false;
        int spaces = 0; bool seen[128] = {}; int distinct = 0, words = 0, letters = 0; bool inWord = false;
        for (char c : t) {
            const unsigned char u = (unsigned char)c & 0x7f;
            if (!seen[u]) { seen[u] = true; ++distinct; }
            const bool sep = c == ' ' || c == '\n' || c == '\r';
            if (sep) { ++spaces; inWord = false; } else { ++letters; if (!inWord) { ++words; inWord = true; } }
        }
        const double sp = (double)spaces / n, meanWord = words ? (double)letters / words : 0;
        return sp >= 0.04 && sp <= 0.32 && distinct >= (n >= 40 ? 11 : n >= 24 ? 8 : 7) && meanWord >= 2.0 && meanWord <= 12;
    }
    // IMD — the idle's two tones (±baud/2) and their third-order products (±3·baud/2), measured on a clean idle
    float imdDb = 0; int imdN = 0; double imdLin = 0, imdDue = -1;

    void init(double b, double fs) {
        baud = b; T = 1.0 / b; sps = fs / b;
        L = std::min(kMaxTaps, (int)std::lround(2 * sps) + 1);
        float sum = 0;
        for (int i = 0; i < L; ++i) { h[i] = (float)(0.5 - 0.5 * std::cos(kTwoPi * (i + 1) / (L + 1))); sum += h[i]; }
        for (int i = 0; i < L; ++i) h[i] /= sum;
        tNext = sps + 4;
        // ★★ THE MATCHED FILTER'S OWN ISI. The transmitted pulse is zero at the neighbouring symbol centres; the pulse
        //    through its matched filter is not — ≈ 1/6 of a neighbour lands on every symbol, so a reversal between two
        //    reversals reads 0.67 and a steady run 1.33: an eye a third closed, and an EVM that could never read better
        //    than ≈ 10 dB. Measured from these taps against the pulse, and taken out at the symbol rate (equalise()).
        double g0 = 0, g1 = 0;
        const int span = (int)std::ceil(sps);
        auto pulse = [&](double t) { const double u = t / sps; return std::fabs(u) >= 1 ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * u); };
        const double mid = (L - 1) / 2.0;
        for (int i = 0; i < L; ++i) { g0 += h[i] * pulse(i - mid); g1 += h[i] * pulse(i - mid - sps); }
        (void)span;
        isi = (float)(g1 / g0);
    }
    float isi = 0;
    cf raw1{0, 0}, raw2{0, 0}; int rawN = 0;   // the last two symbols as sampled (equalise() is one symbol behind)
    float raw1b = 0;                    // the envelope at the boundary before raw1
    cf rawPrev{0, 0}; int rawPrevN = 0; // the last symbol as sampled (the timing detector)
    // ★★ A REVERSAL PASSES THROUGH ZERO — the defining property of the shaping. dip = the envelope at the boundary
    //    before each reversal, over the symbols' amplitude: ≈ 0.2 on PSK; ≈ 1 when the "reversal" was really an FSK tone
    //    changing (FT8, RTTY), a fade, or noise. Constant-envelope modes have clean phase AND no dips: this tells them apart.
    float dip = 1.0f, flat = 1.0f;      // mean (envelope at the boundary / amplitude)² before a reversal · before a one
    /** ★ The CONTRAST: a one's boundary is full height, a reversal's is zero — ≈ 1 on PSK whatever the noise (noise adds
     *  to both alike); ≈ 0 when nothing passes through zero (an FSK tone changing, a carrier, noise). */
    float dipContrast() const { return flat - dip; }
    float qual() const { return std::max(0.0f, std::min(1.0f, (snrL - 4.0f) / 12.0f)); }
};

struct Sig {
    int id = 0;
    bool confirmed = false, active = true;
    double fc = 0;            // the carrier, Hz from the input's 0 (IQ: centre; real: DC)
    int k0 = 0;               // the channel's centre bin
    double Phi = 0;           // the channel's LO phase at its first output of the next block (see runChannel)
    double ncoPh = 0;
    std::vector<float> xr, xi;   // the channel's samples: kMaxTaps−1 of history, then this block's kOut
    int nLanes = 0;
    Lane lane[2];
    int lead = 0;             // the lane that steers the AFC and whose text is shown
    double bornSec = 0, lastGoodSec = 0, confirmedSec = 0, bornHz = 0;
    double t0 = -1;           // ★ input time (s) of this channel's first output sample
    bool ghost = false;
    bool catchup = true;      // ★ run over the kept transforms before its first live hop
    float snrShown = 0, qShown = 0;
    bool lastCr = false;   // ★ as last heard well (a gone signal is listed as it was, not as its noise)
    int64_t firstMs = 0, lastMs = 0;
    std::string text;         // committed text (bounded)
    std::string call;
    unsigned chars = 0;
    // spotting
    std::string word;
    std::vector<std::string> words;
    std::map<std::string, int> heard;
    std::map<std::string, double> spotted;
    float imdDb = 0;
    // ★ the last kLine channel samples (after the NCO) — the squared-signal line search (lineSearch)
    std::vector<cf> zr; size_t zn = 0; uint64_t hops = 0; int linesDone = 0;
};

} // namespace

struct Skimmer::Impl {
    Skimmer* owner = nullptr;
    Config cfg;
    int N = 0, D = 0, H = 0;
    double binHz = 0, fsCh = 0;
    PFFFT_Setup* big = nullptr; PFFFT_Setup* small = nullptr; PFFFT_Setup* line = nullptr;
    float *lin = nullptr, *lout = nullptr, *lwork = nullptr; std::vector<float> lwin;
    float *fin = nullptr, *fout = nullptr, *work = nullptr;
    float *cin = nullptr, *cout = nullptr, *cwork = nullptr;
    std::vector<float> tail;          // the last N input samples, interleaved complex
    int fill = 0;                     // new samples since the last FFT (into tail's second half)
    // searched bins
    int kLo = 0, nS = 0;              // first searched bin (signed) and how many
    std::vector<float> xs, Fa, Sa, floorV;   // contiguous copy of the searched bins (+1 each side), averages, floor
    std::vector<float> mask, maskTmp;
    uint64_t hopsDone = 0;
    uint64_t samples = 0;
    int64_t startMs = 0;
    std::vector<std::unique_ptr<Sig>> sigs;      // live channels
    std::vector<std::unique_ptr<Sig>> gone;      // confirmed signals no longer heard (the list's history)
    std::vector<std::pair<double, double>> holdOff;   // (Hz, until sec) — a carrier that would not decode
    int nextId = 1;
    uint64_t dropped = 0, suppressed = 0, ghosts = 0;
    std::vector<float> scratch, blk2, blkRe, blkIm;
    std::vector<double> pendingAdd;   // carriers a line search found, added after the channel loop
    static constexpr int kHist = 6;   // ★ transforms kept (≈ 1 s): a new channel is run over them first
    std::vector<std::vector<float>> hist;

    explicit Impl(const Config& c) : cfg(c) {
        // ★ N = 128·m with m 5-smooth and nearest rate/375: every channel then runs at rate/m ≈ 375 Hz, and the bins are
        //   ≈ 2.9 Hz — fine enough to put two PSK31 signals 31 Hz apart ten bins apart.
        const double want = cfg.rate / 375.0;
        int best = 8; double bd = 1e9;
        for (int m = 8; m <= 4096; ++m) {
            int r = m; for (int p : { 2, 3, 5 }) while (r % p == 0) r /= p;
            if (r != 1) continue;
            const double dd = std::fabs(std::log(m / want));
            if (dd < bd) { bd = dd; best = m; }
        }
        D = best; N = kM * D; H = N / 2;
        binHz = cfg.rate / N; fsCh = cfg.rate / D;
        big = pffft_new_setup(N, PFFFT_COMPLEX);
        small = pffft_new_setup(kM, PFFFT_COMPLEX);
        line = pffft_new_setup(kLine, PFFFT_COMPLEX);
        lin = (float*)pffft_aligned_malloc(2 * kLine * sizeof(float));
        lout = (float*)pffft_aligned_malloc(2 * kLine * sizeof(float));
        lwork = (float*)pffft_aligned_malloc(2 * kLine * sizeof(float));
        lwin.resize(kLine);
        for (int i = 0; i < kLine; ++i) lwin[(size_t)i] = (float)(0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / kLine));
        fin = (float*)pffft_aligned_malloc(2 * N * sizeof(float));
        fout = (float*)pffft_aligned_malloc(2 * N * sizeof(float));
        work = (float*)pffft_aligned_malloc(2 * N * sizeof(float));
        cin = (float*)pffft_aligned_malloc(2 * kM * sizeof(float));
        cout = (float*)pffft_aligned_malloc(2 * kM * sizeof(float));
        cwork = (float*)pffft_aligned_malloc(2 * kM * sizeof(float));
        tail.assign((size_t)2 * N, 0.0f);
        hist.assign(kHist, std::vector<float>((size_t)2 * N, 0.0f));
        double lo = cfg.loHz, hi = cfg.hiHz;
        if (!cfg.complexIn) { lo = std::max(lo, 150.0); hi = std::min(hi, cfg.rate / 2 - 200); }
        else { lo = std::max(lo, -cfg.rate / 2 + 200); hi = std::min(hi, cfg.rate / 2 - 200); }
        kLo = (int)std::ceil(lo / binHz); nS = std::max(16, (int)std::floor(hi / binHz) - kLo + 1);
        xs.assign((size_t)2 * (nS + 2), 0.0f);
        Fa.assign((size_t)nS, 0.0f); Sa.assign((size_t)nS, 0.0f); floorV.assign((size_t)nS, 0.0f);
        // ★ The channel mask: flat to 0.30·fs, a cosine taper to nothing at 0.45·fs — short in time (≈ 40 ms), so the
        //   middle half of every 0.34 s inverse is clean overlap-save output.
        mask.assign(kM, 0.0f);
        for (int o = -kM / 2; o < kM / 2; ++o) {
            const double f = std::fabs(o * binHz) / fsCh;
            const double g = f <= 0.30 ? 1.0 : f >= 0.45 ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * (f - 0.30) / 0.15);
            mask[(size_t)(o + kM / 2)] = (float)g;
        }
        blkRe.assign(kOut, 0); blkIm.assign(kOut, 0);
        startMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    ~Impl() {
        if (big) pffft_destroy_setup(big);
        if (small) pffft_destroy_setup(small);
        if (line) pffft_destroy_setup(line);
        for (float* p : { fin, fout, work, cin, cout, cwork, lin, lout, lwork }) if (p) pffft_aligned_free(p);
    }
    double nowSec() const { return (double)samples / cfg.rate; }
    int64_t nowMs() const { return startMs + (int64_t)(nowSec() * 1000.0); }
    int binIndex(int k) const { k %= N; return k < 0 ? k + N : k; }

    // ── input ──
    template <class Get> void feed(int n, Get get) {
        for (int i = 0; i < n; ) {
            const int take = std::min(n - i, H - fill);
            float* dst = tail.data() + 2 * (size_t)(N - H + fill);
            for (int j = 0; j < take; ++j) { const cf v = get(i + j); dst[2 * j] = v.real(); dst[2 * j + 1] = v.imag(); }
            fill += take; i += take; samples += (uint64_t)take;
            if (fill == H) { hop(); std::memmove(tail.data(), tail.data() + 2 * (size_t)H, sizeof(float) * 2 * (size_t)(N - H)); fill = 0; }
        }
    }

    // ── one hop: the FFT, the bins, the carriers, every channel ──
    void hop() {
        std::memcpy(fin, tail.data(), sizeof(float) * 2 * (size_t)N);
        pffft_transform_ordered(big, fin, fout, work, PFFFT_FORWARD);
        ++hopsDone;
        for (int s = 0; s < nS + 2; ++s) {
            const int b = binIndex(kLo - 1 + s);
            xs[2 * (size_t)s] = fout[2 * (size_t)b]; xs[2 * (size_t)s + 1] = fout[2 * (size_t)b + 1];
        }
        // ★ both start as running means (so the first seconds are not one noisy frame), then settle to ≈ 1 s and ≈ 6 s
        const float aF = std::max(0.15f, 1.0f / (float)hopsDone), aS = std::max(0.03f, 1.0f / (float)hopsDone);
        binPass(xs.data(), Fa.data(), Sa.data(), nS, aF, aS);
        updateFloor();
        if (hopsDone >= 10) findCarriers();
        // ★★ A NEW CHANNEL STARTS IN THE PAST: the last kHist transforms are kept, and a channel opened this hop is run
        //    over them first — ≈ 1 s of the signal that the carrier finder's own average took to notice (a PSK63 CQ
        //    is over in that second). Cost: kHist copies of the transform (≈ 1.5 MB at 96 kHz), a few inverses once.
        std::memcpy(hist[(size_t)(hopsDone % kHist)].data(), fout, sizeof(float) * 2 * (size_t)N);
        for (auto& g : sigs) {
            if (g->catchup) {
                g->catchup = false;
                const uint64_t from = hopsDone > (uint64_t)kHist - 1 ? hopsDone - (uint64_t)kHist + 1 : 1;
                for (uint64_t k = from; k < hopsDone; ++k) runChannel(*g, hist[(size_t)(k % kHist)].data(), k);
            }
            runChannel(*g, fout, hopsDone);
        }
        for (double hz : pendingAdd) if ((int)sigs.size() < cfg.maxSignals) addSig(hz);
        pendingAdd.clear();
        housekeeping();
    }

    // ★ The floor: the 25th percentile of the slow average in blocks of 32 bins (≈ 94 Hz), the lowest of each block and
    //   its neighbours (a block that is mostly one strong signal does not lift it), × the percentile's own bias on noise.
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
        // ★ A block no lower than BOTH neighbours' higher one is mostly signal: it takes that neighbour's value. On a
        //   slope (a filter's skirt) the block keeps its own — so the edge of a passband is not read as a carrier.
        std::vector<float>& v = blk2;
        v.assign((size_t)nb, 0.0f);
        for (int j = 0; j < nb; ++j) v[(size_t)j] = std::min(blk[(size_t)j + 1], std::max(blk[(size_t)j], blk[(size_t)j + 2])) * kFloorBias;
        // ★ then straight lines between the blocks' centres
        // ★ floor(x) by integers (decoder audit 2026-10-31): x = (2i + 1 − B) / 2B exactly, so its floor is that numerator
        //   shifted down (an arithmetic shift is a floor for negatives too). std::floor is a library routine on 32-bit ARM
        //   (no rounding instruction before ARMv8) and this ran it for every searched bin every hop: the skimmer's
        //   largest single call on the Pi 2. Bit-identical.
        static_assert(B == 32, "the shift below is a floor division by 2B = 64");
        for (int i = 0; i < nS; ++i) {
            const double x = (i + 0.5) / B - 0.5;
            const int j0 = std::max(0, std::min(nb - 1, (2 * i + 1 - B) >> 6)), j1 = std::min(nb - 1, j0 + 1);
            const double t = std::max(0.0, std::min(1.0, x - j0));
            floorV[(size_t)i] = std::max((float)(v[(size_t)j0] * (1 - t) + v[(size_t)j1] * t), 1e-30f);
        }
    }

    double hzOfS(double s) const { return (kLo + s) * binHz; }

    void findCarriers() {
        if ((int)sigs.size() >= cfg.maxSignals) return;
        const double now = nowSec();
        holdOff.erase(std::remove_if(holdOff.begin(), holdOff.end(), [&](const std::pair<double, double>& h) { return h.second < now; }), holdOff.end());
        // ★ A carrier: the fast average summed over ±6 bins (≈ ±18 Hz — a PSK31 signal's idle tones and most of its
        //   text) standing ≥ 3 dB over the floor there, and the highest such sum within ±5 bins.
        const int W = 6, X = 5;
        std::vector<float> sum((size_t)nS, 0.0f);
        double run = 0;
        for (int i = 0; i < std::min(nS, 2 * W + 1); ++i) run += Fa[(size_t)i];
        for (int s = W; s + W < nS; ++s) {
            sum[(size_t)s] = (float)run;
            run += (s + W + 1 < nS ? Fa[(size_t)(s + W + 1)] : 0.0f) - Fa[(size_t)(s - W)];
        }
        const float candEx = (float)(std::pow(10.0, cfg.candidateDb / 10.0) - 1.0);
        struct Cand { int s; float ex; };
        std::vector<Cand> cands;
        for (int s = W + X; s + W + X < nS; ++s) {
            const float ex = sum[(size_t)s] / ((2 * W + 1) * floorV[(size_t)s]) - 1.0f;
            if (ex < candEx) continue;
            bool peak = true;
            for (int i = 1; i <= X && peak; ++i) peak = sum[(size_t)s] >= sum[(size_t)(s - i)] && sum[(size_t)s] > sum[(size_t)(s + i)];
            if (peak) cands.push_back({ s, ex });
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.ex > b.ex; });
        int added = 0;
        for (const Cand& c : cands) {
            if ((int)sigs.size() >= cfg.maxSignals || added >= 8) break;
            // ★ the carrier at the centroid of the excess over ±8 bins
            double wsum = 0, ws = 0;
            for (int i = -8; i <= 8; ++i) {
                const int s = c.s + i; if (s < 0 || s >= nS) continue;
                const double e = std::max(0.0f, Fa[(size_t)s] - floorV[(size_t)s]);
                wsum += e; ws += e * s;
            }
            const double hz = hzOfS(wsum > 0 ? ws / wsum : c.s);
            bool near = false;
            for (auto& g : sigs) if (std::fabs(g->fc - hz) < 12.0) { near = true; break; }
            for (auto& h : holdOff) if (std::fabs(h.first - hz) < 12.0) { near = true; break; }
            if (near) continue;
            addSig(hz); ++added;
        }
    }

    void addSig(double hz) {
        std::unique_ptr<Sig> g(new Sig());
        g->id = 0; g->fc = hz; g->bornHz = hz; g->k0 = (int)std::lround(hz / binHz);
        g->xr.assign(kMaxTaps - 1 + kOut, 0.0f); g->xi.assign(kMaxTaps - 1 + kOut, 0.0f);
        g->zr.assign(kLine, cf(0, 0));
        g->nLanes = 0;
        if (cfg.psk31) g->lane[g->nLanes++].init(31.25, fsCh);
        if (cfg.psk63) g->lane[g->nLanes++].init(62.5, fsCh);
        for (int i = 0; i < g->nLanes; ++i) { g->lane[i].yr.assign(kOut, 0); g->lane[i].yi.assign(kOut, 0); }
        g->bornSec = nowSec(); g->lastGoodSec = g->bornSec;
        // ★ Phi: the LO phase at this channel's first output of the NEXT hop — see runChannel.
        g->Phi = 0;
#ifdef PSK_DEBUG
        std::fprintf(stderr, "[%.2f] + %.2f Hz (tracked %zu)\n", nowSec(), hz, sigs.size() + 1);
#endif
        sigs.push_back(std::move(g));
    }

    // ── a channel: 128 bins → inverse → the middle half, de-rotated and mixed to the carrier, then each lane ──
    void runChannel(Sig& g, const float* X, uint64_t hopIdx) {
        // ★ block b's first valid output is input sample b·H − 3N/4 (the tail holds the last N; outputs are its middle half)
        if (g.t0 < 0) g.t0 = ((double)hopIdx * H - 0.75 * N) / cfg.rate;
        const int kc = (int)std::lround(g.fc / binHz);
        if (std::fabs(g.fc - g.k0 * binHz) > 0.75 * binHz) g.k0 = kc;
        // ★ Bins k0−64 … k0+63 into the inverse's order (offset o at index (o + 128) mod 128), masked. Two runs, each
        //   split again only where it wraps round the transform (IQ near its centre).
        auto put = [&](int oFrom, int count, int dst) {
            int o = oFrom;
            while (count > 0) {
                const int b = binIndex(g.k0 + o);
                const int run = std::min(count, N - b);
                mulMask(X + 2 * (size_t)b, mask.data() + (o + kM / 2), cin + 2 * (size_t)dst, run);
                o += run; dst += run; count -= run;
            }
        };
        put(0, kM / 2, 0);
        put(-kM / 2, kM / 2, kM / 2);
        pffft_transform_ordered(small, cin, cout, cwork, PFFFT_BACKWARD);
        // ★★ PHASE CONTINUITY. The inverse's output j is the band at time bH + j·D, mixed down by k0·Δ from the BLOCK's
        //    start. Multiplying by e^{j(π·k0/2 − Φ)} (Φ = the LO phase at this block's first output, j = M/4) makes the LO
        //    continuous across blocks — even when k0 moves — and the NCO then removes fc − k0·Δ. Φ advances by π·k0 a hop.
        const double rot = M_PI * g.k0 / 2.0 - g.Phi;
        const float sc = 1.0f / (float)N;
        const float gr = (float)std::cos(rot) * sc, gi = (float)std::sin(rot) * sc;
        const double w = kTwoPi * (g.fc - g.k0 * binHz) / fsCh;
        float* xr = g.xr.data() + (kMaxTaps - 1);
        float* xi = g.xi.data() + (kMaxTaps - 1);
        mixBlock(cout + 2 * (kM / 4), kOut, gr, gi, g.ncoPh, w, xr, xi);
        g.ncoPh = std::fmod(g.ncoPh + w * kOut, kTwoPi);
        g.Phi = std::fmod(g.Phi + M_PI * g.k0, kTwoPi);
        // ★ each lane: its matched filter over the block, then symbols
        for (int li = 0; li < g.nLanes; ++li) {
            Lane& l = g.lane[li];
            const int off = kMaxTaps - l.L;
            firBlock(g.xr.data() + off, kOut, l.h, l.L, l.yr.data());
            firBlock(g.xi.data() + off, kOut, l.h, l.L, l.yi.data());
            runLane(g, li);
        }
        // ★ keep the block for the line search and IMD
        for (int j = 0; j < kOut; ++j) { g.zr[g.zn % kLine] = cf(xr[j], xi[j]); ++g.zn; }
        for (int li = 0; li < g.nLanes; ++li) if (g.lane[li].imdDue >= 0) { measureImd(g, g.lane[li], g.lane[li].imdDue); g.lane[li].imdDue = -1; }
        ++g.hops;
        std::memmove(g.xr.data(), g.xr.data() + kOut, sizeof(float) * (kMaxTaps - 1));
        std::memmove(g.xi.data(), g.xi.data() + kOut, sizeof(float) * (kMaxTaps - 1));
        decideLead(g);
        // ★ a tentative carrier's frequency from its squared signal: once its ring first fills, then every 4 hops until
        //   it confirms (≤ 3 times), and every ≈ 4 s after (a big drift the loops would follow too slowly)
        if (g.zn >= (size_t)kLine && ((!g.confirmed && g.linesDone < 3 && g.hops % 4 == 0) || g.hops % 24 == 0)) lineSearch(g);
        commit(g);
    }

    /** ★★ THE SQUARED-SIGNAL LINE. Squaring BPSK takes the data off: z² is the envelope² (positive on average) turning at
     *  TWICE the carrier's offset — a spectral line, whatever is being sent. 512 channel samples (≈ 1.4 s) give the
     *  offset to ≈ 0.1 Hz over ±40 Hz: the carrier finder's ≈ 2.9 Hz bins and centroid get a tentative channel near a
     *  signal; this puts it ON the signal before the loops start (a 3.8 Hz start at −12 dB never pulled in), and a
     *  second line near by is a second signal (two PSK31 stations 31 Hz apart). */
    void lineSearch(Sig& g) {
        ++g.linesDone;
        for (int i = 0; i < kLine; ++i) {
            const cf z = g.zr[(g.zn + (size_t)i) % kLine];
            const cf z2 = z * z * lwin[(size_t)i];
            lin[2 * i] = z2.real(); lin[2 * i + 1] = z2.imag();
        }
        pffft_transform_ordered(line, lin, lout, lwork, PFFFT_FORWARD);
        const double res = fsCh / kLine;              // Hz per bin of 2·Δf
        const int span = (int)std::ceil(80.0 / res);   // ±40 Hz of Δf
        auto P = [&](int k) { const int b = (k % kLine + kLine) % kLine; return lout[2 * b] * lout[2 * b] + lout[2 * b + 1] * lout[2 * b + 1]; };
        std::vector<float> pw((size_t)(2 * span + 1));
        for (int k = -span; k <= span; ++k) pw[(size_t)(k + span)] = P(k);
        std::vector<float> sorted(pw); std::nth_element(sorted.begin(), sorted.begin() + (long)sorted.size() / 2, sorted.end());
        const float med = std::max(1e-30f, sorted[sorted.size() / 2]);
        // ★ the strongest line within ±20 Hz retunes this channel; one ≥ 40 % of it, 8–40 Hz away, is another carrier
        // ★★ A line ON the channel (within ±4 Hz) wins over a stronger one further off: two signals near each other put
        //    lines between them too (their cross product, at the midpoint, and each one's ±1/T side lines). A channel
        //    already on a carrier stays on it; only one with nothing under it goes to the strongest line within ±20 Hz.
        int best = 0; float bv = 0;
        const int centre = (int)std::ceil(8.0 / res), near = (int)std::ceil(40.0 / res);
        for (int k = -centre; k <= centre; ++k) if (pw[(size_t)(k + span)] > bv) { bv = pw[(size_t)(k + span)]; best = k; }
        if (bv < 12.0f * med) {
            bv = 0;
            for (int k = -near; k <= near; ++k) if (pw[(size_t)(k + span)] > bv) { bv = pw[(size_t)(k + span)]; best = k; }
        }
        if (bv < 12.0f * med) return;                 // ★ no line: noise, or not BPSK — leave the channel where it is
        const float a = P(best - 1), b = P(best), c = P(best + 1);
        const double den = a - 2 * b + c;
        const double frac = den < 0 ? 0.5 * (a - c) / den : 0.0;
        const double df = (best + frac) * res / 2.0;
#ifdef PSK_DEBUG
        std::fprintf(stderr, "[%.2f] line at %.2f Hz: Δf %+.2f (%.0f× median)\n", nowSec(), g.fc, df, bv / med);
#endif
        if (std::fabs(df) > 0.4) {
            g.fc += df;
            for (int li = 0; li < g.nLanes; ++li) {
                Lane& l = g.lane[li];
                l.omega = 0; l.fll = cf(0, 0);
                if (!g.confirmed) { l.locked = false; }
            }
        }
        // ★ another carrier near by
        if ((int)sigs.size() >= cfg.maxSignals) return;
        for (int k = -span; k <= span; ++k) {
            const float v = pw[(size_t)(k + span)];
            if (v < 0.4f * bv || v < 12.0f * med || std::abs(k - best) * res / 2.0 < 8.0) continue;
            if (k > -span && pw[(size_t)(k - 1 + span)] > v) continue;
            if (k < span && pw[(size_t)(k + 1 + span)] >= v) continue;
            const double hz = g.fc - df + k * res / 2.0 + (std::fabs(df) > 0.4 ? 0.0 : 0.0);
            bool taken = false;
            for (auto& o : sigs) if (std::fabs(o->fc - hz) < 6.0) { taken = true; break; }
            // ★ …and not at the midpoint of two carriers we have (their cross product's line)
            for (size_t x = 0; x < sigs.size() && !taken; ++x)
                for (size_t y = x + 1; y < sigs.size() && !taken; ++y)
                    if (std::fabs(sigs[x]->fc - sigs[y]->fc) < 90.0 && std::fabs(0.5 * (sigs[x]->fc + sigs[y]->fc) - hz) < 2.0) taken = true;
            for (auto& h : holdOff) if (std::fabs(h.first - hz) < 6.0) { taken = true; break; }
            if (!taken) { pendingAdd.push_back(hz); }
        }
    }

    // ── a lane: symbol timing, the Costas loop, detection, Varicode ──
    void runLane(Sig& g, int li) {
        Lane& l = g.lane[li];
        const double sps = l.sps;
        // ★ The envelope's baud-rate component: |y|² against e^{−j2πn/sps}. Recomputed from n each block (no drift).
        const double ph0 = -kTwoPi * std::fmod((double)l.n, sps) / sps;
        cf rot((float)std::cos(ph0), (float)std::sin(ph0));
        const cf step((float)std::cos(-kTwoPi / sps), (float)std::sin(-kTwoPi / sps));
        const float beta = (float)(1.0 / (sps * kTimingSyms));
        // ★ l.n as a double, kept alongside (exact below 2^53): a uint64 → double conversion is a library call on 32-bit
        //   ARM, and the loop asked for it every output (decoder audit 2026-10-31). The rotator by plain products — no
        //   std::complex NaN check per output.
        double nd = (double)l.n;
        for (int j = 0; j < kOut; ++j) {
            const cf y(l.yr[(size_t)j], l.yi[(size_t)j]);
            const float e = std::norm(y);
            l.C += beta * (e * rot - l.C);
            l.eAvg += beta * (e - l.eAvg);
            rot = cf(rot.real() * step.real() - rot.imag() * step.imag(), rot.real() * step.imag() + rot.imag() * step.real());
            l.yh[l.n & 31] = y;
            ++l.n; nd += 1.0;
            while (nd - 1.0 >= l.tNext + 2.0) {
                const double t = l.tNext;
                const uint64_t i = (uint64_t)std::floor(t);
                const float mu = (float)(t - (double)i);
                const cf y0 = l.yh[(i - 1) & 31], y1 = l.yh[i & 31], y2 = l.yh[(i + 1) & 31], y3 = l.yh[(i + 2) & 31];
                // Catmull-Rom
                const cf a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
                const cf a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const cf a2 = -0.5f * y0 + 0.5f * y2;
                const cf s = ((a0 * mu + a1) * mu + a2) * mu + y1;
                // ★ and half a symbol back — where a reversal's cosine passes through zero
                const double tb = t - sps / 2;
                const uint64_t ib = (uint64_t)std::floor(tb);
                const float mb = (float)(tb - (double)ib);
                const cf ym = l.yh[ib & 31] * (1.0f - mb) + l.yh[(ib + 1) & 31] * mb;
                equalise(g, li, s, std::abs(ym));
                // ★★ TIMING. In idle (continuous reversals) the envelope's baud-rate component is strong and says where
                //    the centres are outright. In TEXT it all but vanishes after the matched filter (0.03 of the mean,
                //    measured), so there the mid-point between two symbols does the work: a reversal crosses zero there
                //    when the timing is right, and the sample there leans toward whichever side is late — Re[ym·(s₋ − s)*]
                //    (the Gardner detector), phase-blind, averaged over the reversals as they come.
                double adj = 0;
                if (std::abs(l.C) > 0.2f * l.eAvg) {
                    const double peak = -std::arg(l.C) / kTwoPi * sps;
                    adj = std::max(-sps / 4, std::min(sps / 4, std::remainder(t - peak, sps)));
                } else if (l.rawPrevN) {
                    const float e = (ym * std::conj(l.rawPrev - s)).real();
                    const float norm = std::max(1e-30f, std::norm(l.rawPrev) + std::norm(s));
                    adj = -(double)std::max(-0.5f, std::min(0.5f, e / norm)) * sps * 0.5;
                }
                l.rawPrev = s; l.rawPrevN = 1;
                l.tNext += sps - kTimingGain * adj;
            }
        }
    }

    /** ★ s_k − c·(s_{k−1} + s_{k+1}): the first-order inverse of the matched filter's [c 1 c] (noise up 0.2 dB). */
    void equalise(Sig& g, int li, cf s, float boundary) {
        Lane& l = g.lane[li];
        if (l.rawN >= 2) symbol(g, li, l.raw1 - l.isi * (l.raw2 + s), l.raw1b);
        l.raw2 = l.raw1; l.raw1 = s; l.raw1b = boundary; ++l.rawN;
    }

    void symbol(Sig& g, int li, cf s, float boundary) {
        Lane& l = g.lane[li];
        const float amp = std::abs(s);
        if (!(amp > 0)) return;
        ++l.sym;
        // ★ Costas loop (BPSK): rotate by θ, the phase error is atan(b/a) whichever side a is; second order so a residual
        //   frequency offset leaves no standing phase error.
        if (!l.locked) { l.theta = std::arg(s * s) * 0.5f; l.locked = true; }
        const cf r = s * cf(std::cos(l.theta), -std::sin(l.theta));
        const float a = r.real(), b = r.imag();
        const float pe = std::atan(b / (std::fabs(a) > 1e-20f ? a : 1e-20f));
        // ★ k2 small and ω bounded (±0.25 rad a symbol ≈ ±1.2 Hz on PSK31): the FLL below does the frequency; a big ω
        //   here is the loop wandering in noise before it has locked (seen: 0.4–0.8 rad for ten seconds at −11 dB).
        l.theta = wrapPi(l.theta + l.omega + kCostas1 * pe);
        l.omega += kCostas2 * pe;
        l.omega = std::max(-0.35f, std::min(0.35f, l.omega));
        // ★ EVM → Es/N0, short and long
        const float aa = std::fabs(a);
        // ★ running means for the first symbols (the level is whatever the band gives — never a fixed start value)
        const float ks = std::max(0.125f, 1.0f / (float)l.sym), kl = std::max(0.03125f, 1.0f / (float)l.sym);
        l.As += ks * (aa - l.As); l.Vs += ks * ((aa - l.As) * (aa - l.As) + b * b - l.Vs);
        l.Al += kl * (aa - l.Al); l.Vl += kl * ((aa - l.Al) * (aa - l.Al) + b * b - l.Vl);
        l.snrS = 10.0f * std::log10(std::max(1e-6f, l.As * l.As / std::max(1e-30f, l.Vs)));
        l.snrL = 10.0f * std::log10(std::max(1e-6f, l.Al * l.Al / std::max(1e-30f, l.Vl)));
        const float ek = ((aa - l.As) * (aa - l.As) + b * b) / std::max(1e-30f, l.As * l.As);
#ifdef PSK_DEBUG
        if (std::getenv("PSK_SYM") && li == std::atoi(std::getenv("PSK_SYM")) && l.sym < 400) std::fprintf(stderr, "sym %llu a %.3g b %.3g pe %.2f th %.2f om %.3f t %.2f C %.2f/%.2f\n", (unsigned long long)l.sym, a, b, pe, l.theta, l.omega, std::fmod(l.tNext, l.sps), std::abs(l.C) / std::max(1e-30f, l.eAvg), -std::arg(l.C) / 6.2832 * l.sps);
#endif
        l.eRing[l.sym % kRing] = std::min(ek, 16.0f);
#ifdef PSK_DEBUG
        if (std::getenv("PSK_TRACE") && l.sym % 16 == 0) std::fprintf(stderr, "  [%.2f] %.2f Hz L%d snrS %.1f snrL %.1f om %.3f dip %.2f z %.2f fll %.2f\n", nowSec(), g.fc, li, l.snrS, l.snrL, l.omega, l.ones7, l.zeroFrac, std::arg(l.fll));
#endif
        // ★ the bit: coherent decisions, differentially decoded (a ONE is no reversal)
        int bit = 1;
        if (l.havePrev) {
            const cf d = s * std::conj(l.prev);
            bit = ((a >= 0) == (l.prevA >= 0)) ? 1 : 0;
            // ★ FLL: d² has the data removed; its phase is 4π·Δf·T — slowly re-centres the channel on the carrier
            //   (the Costas loop's ω is folded in too), so the matched filter sits on the signal.
            const cf d2 = d * d;
            const float m2 = std::abs(d2);
            if (m2 > 0) l.fll += 0.0625f * (d2 / std::sqrt(m2) - l.fll);
            if (li == g.lead) {
                const double ferr = std::arg(l.fll) / (2.0 * kTwoPi * l.T);
                const double fromPll = l.omega / (kTwoPi * l.T);
                const bool young = l.sym < 96;
                // ★★ Two estimators, one owner each: the FLL moves the channel; the Costas loop's ω is handed to the
                //    channel only in the part that is moved FROM it — so ω is reduced by exactly that. (Reducing ω by
                //    the FLL's moves too made a 1 Hz pull-in a 0.2 rad/symbol ω of the wrong sign: 3 s of bad phase.)
                // ★ weighted by how coherent the averaged d² is (|fll| → 1 on a clean signal, ≈ 0.25 on noise): at low SNR
                //   the FLL's own noise walked the channel ±3 Hz and the loops never settled.
                const double coh = (double)std::norm(l.fll);
                const double stepF = std::max(-0.1, std::min(0.1, (young ? 0.03 : 0.006) * coh * ferr));
                const double stepP = std::max(-0.1, std::min(0.1, 0.02 * fromPll));
                g.fc += stepF + stepP;
                l.omega -= (float)(kTwoPi * stepP * l.T);
            }
        }
        if (l.havePrev && l.As > 0) {
            const float r = std::min(2.0f, boundary / l.As);
            if (bit == 0) l.dip += 0.03125f * (r * r - l.dip); else l.flat += 0.03125f * (r * r - l.flat);
        }
        l.bitRing[l.sym % 128] = (uint8_t)bit;
        l.tRing[l.sym % 128] = g.t0 + (l.tNext - l.sps - (l.L - 1) / 2.0) / fsCh;
        l.prev = s; l.prevA = a; l.havePrev = true;
        varicodeBit(g, li, bit);
    }

    void varicodeBit(Sig& g, int li, int bit) {
        Lane& l = g.lane[li];
        l.zeroFrac += (1.0f / 128.0f) * ((bit == 0 ? 1.0f : 0.0f) - l.zeroFrac);
        l.ones7 *= (1.0f - 1.0f / 256.0f);
        if (bit == 0) {
            if (l.onesRun >= 7) l.ones7 += std::min(64, l.onesRun) / 256.0f;
            ++l.zeroRun; l.onesRun = 0;
            // ★ an idle long enough to measure IMD on: done after the block is in the ring (runChannel)
            if (l.zeroRun >= 20 && l.zeroRun % 8 == 4) l.imdDue = l.tNext - l.sps - (l.L - 1) / 2.0;
        }
        else {
            l.zeroRun = 0;
            if (++l.onesRun == 12) l.longOnes[(l.longOnesN++) % 8] = l.sym;
        }
        if (l.len == 0 && bit == 1) l.codeSym0 = l.sym;
        l.cur = (l.cur << 1) | (uint32_t)bit; ++l.len;
        if (l.len >= 2 && (l.cur & 3u) == 0) {
            const uint32_t code = l.cur >> 2; const int clen = l.len - 2;
            if (clen > 0) {
                const int c = clen <= 10 ? varicodeChar(code) : -1;
                if (c >= 0) {
                    l.pend.push_back({ c, l.codeSym0, l.sym });
                    ++l.decoded;
                    const bool gc = goodChar(c);
                    if (gc) ++l.good;
                    l.recentGood.push_back(gc ? 1 : 0);
                    l.recentText.push_back((char)c);
                    if (l.recentText.size() > 48) l.recentText.erase(0, l.recentText.size() - 48);
                    if (l.recentGood.size() > 16) l.recentGood.pop_front();
                } else {
                    l.recentGood.push_back(0);
                    if (l.recentGood.size() > 16) l.recentGood.pop_front();
                }
            }
            l.cur = 0; l.len = 0;
        } else if (l.len > 14) { l.cur &= 3u; l.len = 2; }   // ★ too long to be a code: wait for the next "00"
    }

    /** ★★ IMD, as G3PLX defines it: on a clean idle (continuous reversals = two tones at ±baud/2), the third-order
     *  products at ±3·baud/2 over the tones. Measured on the channel BEFORE the matched filter, over 16 symbols of the
     *  idle, Hann-windowed. Shown only where the signal is strong enough for the products to stand above the noise
     *  (Es/N0 ≥ 15 dB); else 0 (unknown). Averaged over idles in power. */
    void measureImd(Sig& g, Lane& l, double centre) {
        const int W = std::min(kLine - 8, (int)std::lround(16 * l.sps));
        const double endIdx = centre - l.sps;
        const double startIdx = endIdx - W;
        if (startIdx < 0 || (double)g.zn - startIdx > kLine - 2 || endIdx > (double)g.zn - 1) return;
        const uint64_t s0 = (uint64_t)startIdx;
        cf acc[4] = {};
        const double f[4] = { -1.5 * l.baud, -0.5 * l.baud, 0.5 * l.baud, 1.5 * l.baud };
        cf rot[4], step[4];
        for (int k = 0; k < 4; ++k) { rot[k] = cf(1, 0); step[k] = std::polar(1.0f, (float)(-kTwoPi * f[k] / fsCh)); }
        for (int i = 0; i < W; ++i) {
            const float w = (float)(0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / W));
            const cf x = g.zr[(s0 + (uint64_t)i) % kLine] * w;
            for (int k = 0; k < 4; ++k) { acc[k] += x * rot[k]; rot[k] *= step[k]; }
        }
        const double p1 = std::norm(acc[1]) + std::norm(acc[2]), p3 = std::norm(acc[0]) + std::norm(acc[3]);
        if (!(p1 > 0) || l.snrL < 15.0f) return;
        // ★ what noise alone puts in the product bins (the bin is ≈ fs/W wide; Es/N0 is over the baud rate)
        const double noiseRel = 2.0 * std::pow(10.0, -l.snrL / 10.0) * (fsCh / W) / l.baud * 1.5;
        const double r = p3 / p1;
        if (r < 2.0 * noiseRel) return;              // ★ the products are under the noise: better than we can measure
        l.imdLin = l.imdN ? 0.7 * l.imdLin + 0.3 * r : r;
        ++l.imdN;
        l.imdDb = (float)(10.0 * std::log10(l.imdLin));
    }

    /** The mean of the quality measure over a lane's last `n` symbols. */
    /** Es/N0 (dB) over a lane's last `n` symbols. */
    static float recentQ(const Lane& l, int n) {
        if (l.sym < (uint64_t)n) return -10;
        double e = 0; for (int k = 0; k < n; ++k) e += l.eRing[(l.sym - (uint64_t)k) % kRing];
        return (float)(10.0 * std::log10((double)n / std::max(1e-9, e)));
    }
    /** ★ Which lane reads this signal: a clean constellation AND characters coming out of it. A PSK31 signal read at
     *  62.5 Bd is half clean (every other sample is a PSK31 symbol centre) but never makes a "00", so no characters. */
    float laneScore(const Lane& l) const {
        float sc = recentQ(l, 32);
        int n = 0, gsum = 0;
        for (uint8_t v : l.recentGood) { ++n; gsum += v; }
        sc += n >= 4 ? 3.0f * ((float)gsum / (float)n) : -3.0f;
        return sc;
    }
    void decideLead(Sig& g) {
        // ★ fixed once the signal is confirmed: its mode is known, and a signal that has stopped must not turn into
        //   "PSK63" because the other lane reads its noise a shade better
        if (g.nLanes < 2 || g.confirmed) return;
        const int other = 1 - g.lead;
        // ★ a clear margin to switch, so two near-equal lanes do not swap every hop
        if (g.lane[other].sym > 48 && laneScore(g.lane[other]) > laneScore(g.lane[g.lead]) + 2.5f) {
            g.lead = other;
            g.lane[other].fll = g.lane[1 - other].fll;
        }
    }

    // ── committing characters (the text-quality gate), confirmation, spots ──
    void commit(Sig& g) {
        const double now = nowSec();
        for (int li = 0; li < g.nLanes; ++li) {
            Lane& l = g.lane[li];
            while (!l.pend.empty() && l.sym >= l.pend.front().sym1 + kCommitSyms) {
                const PendingChar pc = l.pend.front(); l.pend.pop_front();
#ifdef PSK_DEBUG
                if (std::getenv("PSK_CHARS")) std::fprintf(stderr, "    [%.2f] %.1f L%d%s '%c' snrS %.1f snrL %.1f\n", nowSec(), g.fc, li, li == g.lead ? "*" : " ", pc.c >= 32 ? pc.c : '~', l.snrS, l.snrL);
#endif
                double qs = 0; int qn = 0;
                const uint64_t a = pc.sym0 > 1 ? pc.sym0 - 1 : 0, e = pc.sym1 + kCommitSyms;
                if (e - a < (uint64_t)kRing) for (uint64_t k = a; k <= e; ++k) { qs += l.eRing[k % kRing]; ++qn; }
                float cq = qn ? (float)(10.0 * std::log10(qn / std::max(1e-9, qs))) : -10.0f;   // dB
                // ★ a character beside a carrier-like run of ones is not text, however clean the phase
                if (l.longOnesNear(pc.sym0 > 32 ? pc.sym0 - 32 : 0, pc.sym1 + kCommitSyms)) cq = -10.0f;
                if (!g.confirmed) {
                    // ★ tentative: every lane holds its OWN characters with their quality (a lead that changes lane must
                    //   not leave its text interleaved — a 20 m QSO went unconfirmed that way); confirm() decides
                    l.heldQ.push_back({ (char)pc.c, cq });
                    if (l.heldQ.size() > 400) l.heldQ.erase(l.heldQ.begin(), l.heldQ.begin() + 100);
                    continue;
                }
                if (li != g.lead) continue;
                if (cq < kCharDbLive || l.snrL < kKeepDb || l.textureGross()) { ++suppressed; continue; }
                emit(g, pc.c);
            }
        }
        const Lane& L = g.lane[g.lead];
        // ★ shown as heard WHILE TRANSMITTING: a slowly falling peak (0.1 dB a hop), so the fade at the end of an over — the
        //   last values above kKeepDb — is not what the list says (real 20 m: copy perfect, the row said "poor 3 dB")
        if (L.snrL >= kKeepDb) {
            g.lastGoodSec = now;
            g.snrShown = std::max(L.snrL, g.snrShown - 0.1f);
            g.qShown = std::max(0.0f, std::min(1.0f, (g.snrShown - 4.0f) / 12.0f));
        }
        if (!g.confirmed) {
            int gsum = 0; for (uint8_t v : L.recentGood) gsum += v;
            int strict = 0, letters = 0;
            for (auto& h : L.heldQ) if (h.second >= kCharDb) { ++strict; letters += isLetter((unsigned char)h.first); }
            const bool carrier = L.longOnesN > 0 && L.sym < L.longOnes[(L.longOnesN - 1) % 8] + 256;
            const bool texture = L.zeroFrac > 0.25f && L.zeroFrac < 0.75f && L.snrL >= kConfirmDb - 2.0f && L.ones7 < 0.06f;
            const bool plausible = !carrier && texture && L.textureOk() && L.recentGood.size() >= 8 && gsum * 100 >= 85 * (int)L.recentGood.size();
            // ★ …and what WOULD be shown must have text's texture too (the held characters at the strict bar)
            std::string shown; for (auto& hq : L.heldQ) if (hq.second >= kCharDb) shown.push_back(hq.first);
            if (shown.size() > 48) shown.erase(0, shown.size() - 48);
            if (recentQ(L, 64) >= kConfirmDb && plausible && strict >= 6 && letters >= 3
                && Lane::textTexture(shown) && readsAsText(shown)) {
#ifdef PSK_DEBUG
                std::fprintf(stderr, "[%.2f] confirming %.2f on shown '%s'\n", nowSec(), g.fc, shown.c_str());
#endif
                if (ghostOf(g)) { g.ghost = true; ++ghosts; } else confirm(g);
            }
        }
        if (g.confirmed) g.lastMs = g.lastGoodSec >= now - 1.0 ? nowMs() : g.lastMs;
    }

    /** ★★ A GHOST: a channel reading a STRONGER neighbour's symbols, not a station of its own. Half a baud from a strong
     *  signal the squared spectrum puts a side line right on the channel, the loops lock, and the neighbour's bits come
     *  out every one inverted (a π turn a symbol) — clean, confirmable garbage. Two operators' bits never agree; a ghost's
     *  agree (or disagree) with its neighbour's 90 % of the time. Compared decision by decision at the same instants. */
    bool ghostOf(const Sig& g) const {
        const int li = g.lead;
        const Lane& l = g.lane[li];
        if (l.sym < 80) return false;
        for (auto& op : sigs) {
            const Sig& o = *op;
            if (&o == &g || std::fabs(o.fc - g.fc) > 80.0 || o.nLanes <= li) continue;
            const Lane& m = o.lane[li];
            if (m.sym < 80 || m.Al < 1.2f * l.Al) continue;     // only a stronger neighbour makes ghosts
            int same = 0, n = 0;
            for (int k = 2; k < 66; ++k) {
                const uint64_t si = l.sym - (uint64_t)k;
                const double t = l.tRing[si % 128];
                const double back = (m.tRing[(m.sym - 2) % 128] - t) / m.T;
                const long kk = std::lround(back) + 2;
                if (kk < 2 || kk > 120) continue;
                const uint64_t sj = m.sym - (uint64_t)kk;
                if (std::fabs(m.tRing[sj % 128] - t) > m.T / 3) continue;
                ++n; same += l.bitRing[si % 128] == m.bitRing[sj % 128];
            }
            if (n >= 40 && (same >= 0.9 * n || same <= 0.1 * n)) return true;
        }
        return false;
    }

    void confirm(Sig& g) {
        // ★ two channels the loops pulled onto one carrier: only the first is ever confirmed (housekeeping drops the other)
        for (auto& o : sigs) if (o.get() != &g && o->confirmed && std::fabs(o->fc - g.fc) < 6.0) return;
        g.confirmed = true; g.confirmedSec = nowSec();
#ifdef PSK_DEBUG
        std::fprintf(stderr, "[%.2f] confirmed %.2f Hz lane %d snr %.1f held %zu dipx %.2f z %.2f\n", nowSec(), g.fc, g.lead, g.lane[g.lead].snrL, g.lane[g.lead].heldQ.size(), g.lane[g.lead].ones7, g.lane[g.lead].zeroFrac);
#endif
        // ★ the same station back on (nearly) the same frequency keeps its id and its history
        for (size_t i = 0; i < gone.size(); ++i) {
            if (std::fabs(gone[i]->fc - g.fc) < 6.0) {
                Sig& o = *gone[i];
                g.id = o.id; g.firstMs = o.firstMs; g.text = o.text; g.call = o.call; g.chars = o.chars;
                g.spotted = o.spotted; g.heard = o.heard;
                gone.erase(gone.begin() + (long)i);
                break;
            }
        }
        if (!g.id) { g.id = nextId++; g.firstMs = nowMs(); }
        g.lastMs = nowMs();
        // ★★ What was held: from after the last stretch that read like noise (three characters in a row under −1 dB —
        //    the seconds before the station keyed up, or before the loops locked), every character at the confirmed
        //    signal's own bar (kCharDbLive). The signal is real now; its first CQ is worth keeping.
        std::vector<std::pair<char, float>>& hq = g.lane[g.lead].heldQ;
        size_t from = 0; int low = 0;
        for (size_t i = 0; i < hq.size(); ++i) { low = hq[i].second < -1.0f ? low + 1 : 0; if (low >= 3) from = i + 1; }
        std::string h;
        for (size_t i = from; i < hq.size(); ++i) {
            int c = (unsigned char)hq[i].first;
            if (hq[i].second < kCharDbHeld) { ++suppressed; continue; }
            const bool lfAfterCr = c == '\n' && g.lastCr;   // ★ the same newline rule as emit()
            g.lastCr = c == '\r';
            if (lfAfterCr) continue;
            if (c == '\r') c = '\n';
            if ((c < 32 && c != '\n') || c == 127) continue;
            h.push_back((char)c);
        }
        hq.clear();
        for (int k = 0; k < g.nLanes; ++k) g.lane[k].heldQ.clear();
        for (char c : h) appendText(g, c);
        if (owner->onText && !h.empty()) owner->onText(g.id, g.fc, h);
    }

    void emit(Sig& g, int c) {
        // ★ CR, LF and CR LF are each ONE newline: senders differ (some send a bare CR — words ran together before)
        const bool lfAfterCr = c == '\n' && g.lastCr;
        g.lastCr = c == '\r';
        if (lfAfterCr) return;
        if (c == '\r') c = '\n';
        if (c < 32 && c != '\n') return;            // control codes are never text
        if (c == 127) return;
        appendText(g, (char)c);
        if (owner->onText) owner->onText(g.id, g.fc, std::string(1, (char)c));
    }
    void appendText(Sig& g, char c) {
        g.text.push_back(c); ++g.chars;
        if (g.text.size() > 600) g.text.erase(0, g.text.size() - 400);
        g.lastMs = nowMs();
        // ── words → spots (the CW rules: CQ … CALL, DE CALL, CALL CALL — twice) ──
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
            Spot sp; sp.call = call; sp.offsetHz = g.fc; sp.snrDb = g.lane[g.lead].snrL; sp.how = how;
            sp.mode = g.lane[g.lead].baud > 40 ? "BPSK63" : "BPSK31"; sp.utcMs = nowMs(); sp.signalId = g.id;
            owner->onSpot(sp);
        }
    }

    void housekeeping() {
        const double now = nowSec();
        for (size_t i = 0; i < sigs.size(); ) {
            Sig& g = *sigs[i];
            bool drop = false;
            if (!g.confirmed && (g.ghost || now - g.bornSec > kProbeSec)) { drop = true; ++dropped; holdOff.push_back({ g.fc, now + 20.0 }); }
            if (g.confirmed && now - g.lastGoodSec > kLostSec) drop = true;
            // ★ two channels that AFC'd onto one carrier: the confirmed one stays, else the older, else the first; the
            //   one that goes holds its birthplace off (a strong signal's spectral shoulders would make it again)
            for (size_t k = 0; k < sigs.size() && !drop; ++k)
                if (k != i && std::fabs(sigs[k]->fc - g.fc) < 4.0 && (sigs[k]->confirmed > g.confirmed ||
                    (sigs[k]->confirmed == g.confirmed && (sigs[k]->bornSec < g.bornSec || (sigs[k]->bornSec == g.bornSec && k < i))))) {
                    drop = true;
                    if (!g.confirmed && std::fabs(g.bornHz - g.fc) > 6.0) holdOff.push_back({ g.bornHz, now + 20.0 });
                }
            // ★ a channel the AFC walked out of the searched range
            const double lo = hzOfS(0), hi = hzOfS(nS - 1);
            if (g.fc < lo - 20 || g.fc > hi + 20) drop = true;
            if (drop) {
                if (g.confirmed) {
                    std::unique_ptr<Sig> o = std::move(sigs[i]);
                    o->active = false;
                    gone.insert(gone.begin(), std::move(o));
                } else if (!g.confirmed) { /* tentative: nothing to keep */ }
                sigs.erase(sigs.begin() + (long)i);
                continue;
            }
            ++i;
        }
        const int64_t nm = nowMs();
        gone.erase(std::remove_if(gone.begin(), gone.end(), [&](const std::unique_ptr<Sig>& g) {
            return nm - g->lastMs > (int64_t)cfg.keepGoneSec * 1000; }), gone.end());
        if (gone.size() > 80) gone.resize(80);
    }

    SignalView view(const Sig& g) const {
        SignalView v;
        v.id = g.id; v.offsetHz = g.fc; v.active = g.active;
        const Lane& l = g.lane[g.lead];
        v.baud = l.baud > 40 ? 63 : 31; v.snrDb = g.snrShown; v.quality = g.qShown; v.imdDb = l.imdDb;
        v.firstMs = g.firstMs; v.lastMs = g.lastMs; v.call = g.call; v.text = g.text; v.chars = g.chars;
        if (v.text.size() > 300) v.text.erase(0, v.text.size() - 300);
        return v;
    }
};

Skimmer::Skimmer(const Config& c) : d_(new Impl(c)) { d_->owner = this; }
Skimmer::~Skimmer() = default;

void Skimmer::feedReal(const float* x, int n) { if (n > 0) d_->feed(n, [x](int i) { return cf(x[i], 0.0f); }); }
void Skimmer::feedIq(const std::complex<float>* x, int n) { if (n > 0) d_->feed(n, [x](int i) { return x[i]; }); }

std::vector<SignalView> Skimmer::signals(bool includeGone) const {
    std::vector<SignalView> v;
    for (auto& g : d_->sigs) if (g->confirmed) v.push_back(d_->view(*g));
    std::sort(v.begin(), v.end(), [](const SignalView& a, const SignalView& b) { return a.offsetHz < b.offsetHz; });
    if (includeGone) for (auto& g : d_->gone) v.push_back(d_->view(*g));
    return v;
}
bool Skimmer::signal(int id, SignalView& out) const {
    for (auto& g : d_->sigs) if (g->confirmed && g->id == id) { out = d_->view(*g); return true; }
    for (auto& g : d_->gone) if (g->id == id) { out = d_->view(*g); return true; }
    return false;
}
void Skimmer::setStartUtcMs(int64_t ms) { d_->startMs = ms - (int64_t)(d_->nowSec() * 1000.0); }
int64_t Skimmer::nowMs() const { return d_->nowMs(); }
int Skimmer::fftSize() const { return d_->N; }
double Skimmer::binHz() const { return d_->binHz; }
double Skimmer::channelRate() const { return d_->fsCh; }
int Skimmer::binsSearched() const { return d_->nS; }
uint64_t Skimmer::hops() const { return d_->hopsDone; }
int Skimmer::tracked() const { return (int)d_->sigs.size(); }
int Skimmer::confirmed() const { int n = 0; for (auto& g : d_->sigs) n += g->confirmed ? 1 : 0; return n; }
uint64_t Skimmer::tentativesDropped() const { return d_->dropped; }
uint64_t Skimmer::charsSuppressed() const { return d_->suppressed; }
double Skimmer::noiseCalibration() const {
    double s = 0; int n = 0;
    for (int i = 0; i < d_->nS; ++i) { s += d_->Sa[(size_t)i] / d_->floorV[(size_t)i]; ++n; }
    return n ? s / n : 0;
}
const char* Skimmer::simdName() {
#if VIBE_NEON
    return "neon";
#elif VIBE_SSE
    return "sse2";
#else
    return "scalar";
#endif
}

} // namespace psk
} // namespace vibe
