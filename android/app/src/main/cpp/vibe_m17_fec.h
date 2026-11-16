// vibe_m17_fec.h — M17's DATA LINK LAYER, both ways (V12, docs/v12/M17.md): CRC, the base-40 address, Golay(24,12), the
// K=5 rate-½ convolutional code with its three puncturing patterns, the QPP interleaver, the randomiser, and the four
// frames (LSF, stream, packet, EoT) built and taken apart. Written from the M17 Protocol Specification (spec.m17project.org,
// v2.0.10, 2026-10-16) — every constant below is quoted from it, nothing is taken from anyone's code.
//
// ★★ BOTH DIRECTIONS ON PURPOSE. The decoder is what the server runs; the encoder exists so the tests (and the benchmark
//    row's proof) can make M17 frames from the spec alone and feed them through the receiver end to end. A mistake made
//    identically in both would still pass, so the spec's own vectors are checked where it gives them: the CRC table, the
//    address example (AB1CD = 0x9FDD51), the QPP table's first entries, Golay's generator rows and minimum distance 8.
// ★★ SOFT DECISIONS THROUGHOUT. A soft bit is an int8: positive = "1", negative = "0", its size the confidence, 0 = no
//    idea (a punctured bit). The Viterbi takes soft bits; Golay is decoded Chase-style from soft bits (the four least
//    reliable positions tried both ways, then hard-decision correction of up to three errors, the codeword closest to
//    what was received wins).
// ★ SIMD: the Viterbi's add-compare-select runs as one 8-lane int16 butterfly per bit on NEON (ARMv7 and A64) and SSE2,
//   with a scalar path that is BIT-EXACT with both (the test runs all three on the same input). It is the one hot loop
//   here: 16 states × ~25 frames a second is small, but a Pi 2 serving several listeners runs one per listener.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#define VIBE_M17_NEON 1
#elif (defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define VIBE_M17_SSE 1
#endif

namespace vibe {
namespace m17 {

// ── The physical layer's constants (spec §2) ───────────────────────────────────────────────────────────────────────────
constexpr int kSymRate = 4800;          // symbols a second
constexpr int kFrameSyms = 192;         // 8 sync + 184 payload = 40 ms
constexpr int kPayloadBits = 368;
constexpr double kDevPerUnit = 800.0;   // ★ ±1 → ±800 Hz, ±3 → ±2400 Hz (Table "Dibit Symbol Mapping")

/** The sync bursts as symbols, in the order sent (Table "Frame Specific Sync Bursts"; EoT §2.5.5). */
enum class Sync : int { None = 0, Lsf, Stream, Packet, Bert, Eot };
inline const int8_t* syncSymbols(Sync s) {
    static const int8_t kLsf[8]    = { +3, +3, +3, +3, -3, -3, +3, -3 };   // 0x55F7
    static const int8_t kStream[8] = { -3, -3, -3, -3, +3, +3, -3, +3 };   // 0xFF5D
    static const int8_t kPacket[8] = { +3, -3, +3, +3, -3, -3, -3, -3 };   // 0x75FF
    static const int8_t kBert[8]   = { -3, +3, -3, -3, +3, +3, +3, +3 };   // 0xDF55
    static const int8_t kEot[8]    = { +3, +3, +3, +3, +3, +3, -3, +3 };   // 0x555D, repeated through the 40 ms marker
    switch (s) {
        case Sync::Lsf: return kLsf; case Sync::Stream: return kStream; case Sync::Packet: return kPacket;
        case Sync::Bert: return kBert; case Sync::Eot: return kEot; default: return nullptr;
    }
}
inline uint16_t syncWord(Sync s) {
    switch (s) { case Sync::Lsf: return 0x55F7; case Sync::Stream: return 0xFF5D; case Sync::Packet: return 0x75FF;
                 case Sync::Bert: return 0xDF55; case Sync::Eot: return 0x555D; default: return 0; }
}
/** Dibit (MSB first) → symbol: 01 → +3, 00 → +1, 10 → −1, 11 → −3. */
inline int dibitToSymbol(int msb, int lsb) { return msb ? (lsb ? -3 : -1) : (lsb ? +3 : +1); }

/** The randomiser: 46 bytes, applied MSB first to the 368 payload bits of every frame (Appendix "Randomizer Sequence"). */
inline int randBit(int k) {
    static const uint8_t kSeq[46] = {
        0xD6, 0xB5, 0xE2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xBA, 0x4E, 0x96, 0x90, 0xD8, 0x98, 0xDD, 0x5D,
        0x0C, 0xC8, 0x52, 0x43, 0x91, 0x1D, 0xF8, 0x6E, 0x68, 0x2F, 0x35, 0xDA, 0x14, 0xEA, 0xCD, 0x76,
        0x19, 0x8D, 0xD5, 0x80, 0xD1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2D, 0x29, 0x78, 0xC3 };
    return (kSeq[(k >> 3) % 46] >> (7 - (k & 7))) & 1;
}

/** The QPP interleaver π(x) = (45x + 92x²) mod 368 (Appendix "Interleaving"). It is its own inverse (π(π(x)) = x — the
 *  table shows 1 → 137 and 137 → 1), so one table interleaves and deinterleaves. */
inline const std::array<uint16_t, kPayloadBits>& qpp() {
    static const std::array<uint16_t, kPayloadBits> t = [] {
        std::array<uint16_t, kPayloadBits> a{};
        for (uint32_t x = 0; x < uint32_t(kPayloadBits); ++x) a[x] = uint16_t((45u * x + 92u * x * x) % 368u);
        return a;
    }();
    return t;
}

// ── CRC (spec §3.8): polynomial 0x5935, initial 0xFFFF, MSB first, nothing reflected, no final XOR ──────────────────
inline uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= uint16_t(p[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x5935) : uint16_t(c << 1);
    }
    return c;
}

// ── Addresses (Appendix "Address Encoding"): base 40, the FIRST character in the LEAST significant place ──────────────
constexpr uint64_t kBroadcast = 0xFFFFFFFFFFFFull;
constexpr uint64_t kMaxStandard = 0xEE6B27FFFFFFull;
inline uint64_t encodeCallsign(const std::string& cs) {
    uint64_t a = 0;
    const size_t n = std::min<size_t>(cs.size(), 9);
    for (size_t k = n; k-- > 0;) {
        const char c = cs[k];
        unsigned v = 0;
        if (c >= 'A' && c <= 'Z') v = unsigned(c - 'A' + 1);
        else if (c >= 'a' && c <= 'z') v = unsigned(c - 'a' + 1);
        else if (c >= '0' && c <= '9') v = unsigned(c - '0' + 27);
        else if (c == '-') v = 37; else if (c == '/') v = 38; else if (c == '.') v = 39;
        a = a * 40u + v;
    }
    return a;
}
/** The address as text: "" for the reserved 0, "@ALL" for BROADCAST (the M17 convention for it), "#XXXXXXXXXXXX" for an
 *  address outside the standard range (an application's own; shown, not interpreted). Trailing spaces trimmed. */
inline std::string decodeCallsign(uint64_t a) {
    if (a == 0) return "";
    if (a == kBroadcast) return "@ALL";
    if (a > kMaxStandard) {
        char b[16]; std::snprintf(b, sizeof b, "#%012llX", (unsigned long long)a);
        return b;
    }
    static const char kAlpha[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
    std::string s;
    while (a) { s += kAlpha[a % 40u]; a /= 40u; }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
inline uint64_t addrFrom(const uint8_t* p) { uint64_t a = 0; for (int i = 0; i < 6; ++i) a = (a << 8) | p[i]; return a; }
inline void addrTo(uint64_t a, uint8_t* p) { for (int i = 5; i >= 0; --i) { p[i] = uint8_t(a & 0xFF); a >>= 8; } }

// ── Golay(24,12) (Appendix "Golay Encoder"): data in bits 23..12, check bits 11..1, parity bit 0 ────────────────────────
/** The parity half of G = [I | P], row i for data bit 11−i — the spec's matrix, which the test proves is the cyclic code of
 *  g(x) = 0xC75 with an even-parity bit. */
inline uint16_t golayCheck(uint16_t d) {
    static const uint16_t kRows[12] = { 0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB };
    uint16_t c = 0;
    for (int i = 0; i < 12; ++i) if ((d >> (11 - i)) & 1) c ^= kRows[i];
    return c;
}
inline uint32_t golayEncode(uint16_t d) { d &= 0xFFF; return (uint32_t(d) << 12) | golayCheck(d); }
inline uint16_t golaySyndrome(uint32_t r) { return uint16_t((r & 0xFFF) ^ golayCheck(uint16_t((r >> 12) & 0xFFF))); }
/** Syndrome → the error pattern of weight ≤ 3 that makes it (0xFFFFFFFF where none does: four or more errors). */
inline const std::vector<uint32_t>& golayTable() {
    static const std::vector<uint32_t> t = [] {
        std::vector<uint32_t> v(4096, 0xFFFFFFFFu);
        v[0] = 0;
        for (int a = 0; a < 24; ++a) {
            const uint32_t ea = 1u << a; v[golaySyndrome(ea)] = ea;
            for (int b = a + 1; b < 24; ++b) {
                const uint32_t eb = ea | (1u << b); v[golaySyndrome(eb)] = eb;
                for (int c = b + 1; c < 24; ++c) { const uint32_t ec = eb | (1u << c); v[golaySyndrome(ec)] = ec; }
            }
        }
        return v;
    }();
    return t;
}
/** Hard decision: the corrected 24-bit word, or false when it has four or more errors. `errs` = bits corrected. */
inline bool golayHard(uint32_t r, uint32_t* out, int* errs) {
    const uint32_t e = golayTable()[golaySyndrome(r)];
    if (e == 0xFFFFFFFFu) return false;
    *out = r ^ e;
    if (errs) *errs = __builtin_popcount(e);
    return true;
}
/** ★ Soft decision (Chase-II): 24 soft bits, MSB first (soft[0] = bit 23). The 4 least reliable bits are tried both ways,
 *  each test word decoded hard, and the codeword that best matches the soft values wins. Returns the 12 data bits, or −1
 *  when no test word decodes. `dist` = the received bits (hard) the winner disagrees with. */
inline int golaySoft(const int8_t* soft, int* dist = nullptr) {
    uint32_t r = 0;
    for (int i = 0; i < 24; ++i) r = (r << 1) | (soft[i] > 0 ? 1u : 0u);
    int idx[24];
    for (int i = 0; i < 24; ++i) idx[i] = i;
    std::partial_sort(idx, idx + 4, idx + 24, [&](int a, int b) { return std::abs(int(soft[a])) < std::abs(int(soft[b])); });
    int best = -1; long bestScore = -(1L << 30); uint32_t bestCw = 0;
    for (int m = 0; m < 16; ++m) {
        uint32_t t = r;
        for (int k = 0; k < 4; ++k) if ((m >> k) & 1) t ^= 1u << (23 - idx[k]);
        uint32_t cw;
        if (!golayHard(t, &cw, nullptr)) continue;
        long score = 0;
        for (int i = 0; i < 24; ++i) score += ((cw >> (23 - i)) & 1) ? soft[i] : -soft[i];
        if (score > bestScore) { bestScore = score; best = int(cw >> 12); bestCw = cw; }
    }
    if (dist && best >= 0) *dist = __builtin_popcount(bestCw ^ r);
    return best;
}

// ── The convolutional code (Appendix "Convolutional Encoder"): K = 5, rate ½, G1 = 1 + D³ + D⁴, G2 = 1 + D + D² + D⁴ ──
/** `bits` (0/1, with the 4 flush zeros already appended) → 2·n coded bits, G1 then G2 for each input bit. */
inline std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> o;
    o.reserve(bits.size() * 2);
    unsigned sr = 0;   // sr bit k = the input k steps ago (bit 0 = the current one)
    for (uint8_t b : bits) {
        sr = ((sr << 1) | (b & 1u)) & 0x1Fu;
        const unsigned d0 = sr & 1, d1 = (sr >> 1) & 1, d2 = (sr >> 2) & 1, d3 = (sr >> 3) & 1, d4 = (sr >> 4) & 1;
        o.push_back(uint8_t(d0 ^ d3 ^ d4));
        o.push_back(uint8_t(d0 ^ d1 ^ d2 ^ d4));
    }
    return o;
}

/** The three puncturing patterns (Appendix "Code Puncturing"), as linearised there: 1 = keep. */
enum class Punct { P1, P2, P3 };
inline const std::vector<uint8_t>& punctPattern(Punct p) {
    static const std::vector<uint8_t> kP1 = [] {   // [1, M×15], M = [1 0 1 1] — 61 entries, 46 ones
        std::vector<uint8_t> v{1};
        for (int i = 0; i < 15; ++i) { v.push_back(1); v.push_back(0); v.push_back(1); v.push_back(1); }
        return v;
    }();
    static const std::vector<uint8_t> kP2 = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
    static const std::vector<uint8_t> kP3 = { 1, 1, 1, 1, 1, 1, 1, 0 };
    return p == Punct::P1 ? kP1 : p == Punct::P2 ? kP2 : kP3;
}
inline std::vector<uint8_t> puncture(const std::vector<uint8_t>& coded, Punct p) {
    const auto& pat = punctPattern(p);
    std::vector<uint8_t> o;
    for (size_t i = 0; i < coded.size(); ++i) if (pat[i % pat.size()]) o.push_back(coded[i]);
    return o;
}
/** Soft punctured bits → soft coded bits of length `codedLen`, zeros (no information) where a bit was punctured out. */
inline std::vector<int8_t> depuncture(const int8_t* soft, size_t n, size_t codedLen, Punct p) {
    const auto& pat = punctPattern(p);
    std::vector<int8_t> o(codedLen, 0);
    size_t k = 0;
    for (size_t i = 0; i < codedLen && k < n; ++i) if (pat[i % pat.size()]) o[i] = soft[k++];
    return o;
}

// ── The Viterbi decoder ───────────────────────────────────────────────────────────────────────────────────────────────
/* ★ The trellis. A state is the last four inputs, the newest in bit 3: next = (b << 3) | (s >> 1). The predecessors of
 *   state ns are 2j and 2j+1 with j = ns & 7, so states j and j+8 form a butterfly over the pair (2j, 2j+1). For
 *   predecessor 2j and input 0 the code bits are o1 = j & 1, o2 = (j>>1 ^ j>>2) & 1; predecessor 2j+1 flips both, and so
 *   does input 1 — so ONE branch metric A[j] serves all four branches of the butterfly:
 *       new[j]   = max(old[2j] + A, old[2j+1] − A)        new[j+8] = max(old[2j] − A, old[2j+1] + A)
 *   (A = ±s1 ± s2, the soft pair for this bit). Ties keep the even predecessor, on every path. */
struct ViterbiOut {
    std::vector<uint8_t> bits;   ///< the decoded input bits, flush bits removed
    int metric = 0;              ///< the winning path's metric, normalised (largest possible = Σ|soft|)
};
namespace detail {
inline int16_t sat16(int v) { return int16_t(std::max(-32768, std::min(32767, v))); }
/** The butterfly signs: sg1[j] = o1 ? +1 : −1, sg2[j] = o2 ? +1 : −1, for predecessor 2j and input 0. */
struct Signs { int16_t s1[8], s2[8]; };
inline const Signs& signs() {
    static const Signs s = [] {
        Signs x{};
        for (int j = 0; j < 8; ++j) {
            x.s1[j] = (j & 1) ? 1 : -1;
            x.s2[j] = (((j >> 1) ^ (j >> 2)) & 1) ? 1 : -1;
        }
        return x;
    }();
    return s;
}
/** One step, scalar — the reference the vector paths must equal. `pm` is updated in place; returns the 16 decisions. */
inline uint16_t acsScalar(int16_t* pm, int a1, int a2) {
    const Signs& sg = signs();
    int16_t nw[16];
    uint16_t dec = 0;
    for (int j = 0; j < 8; ++j) {
        const int16_t A = sat16(sg.s1[j] * a1 + sg.s2[j] * a2);
        const int16_t e = pm[2 * j], o = pm[2 * j + 1];
        const int16_t x0 = sat16(e + A), y0 = sat16(o - A), x1 = sat16(e - A), y1 = sat16(o + A);
        nw[j] = y0 > x0 ? y0 : x0;       if (y0 > x0) dec |= uint16_t(1u << j);
        nw[j + 8] = y1 > x1 ? y1 : x1;   if (y1 > x1) dec |= uint16_t(1u << (j + 8));
    }
    const int16_t ref = nw[0];   // ★ renormalise against state 0 every step (the spread is bounded — no overflow)
    for (int s = 0; s < 16; ++s) pm[s] = sat16(nw[s] - ref);
    return dec;
}
#if VIBE_M17_NEON
inline uint16_t acsNeon(int16_t* pm, int a1, int a2) {
    const Signs& sg = signs();
    const int16x8_t A = vqaddq_s16(vmulq_n_s16(vld1q_s16(sg.s1), int16_t(a1)), vmulq_n_s16(vld1q_s16(sg.s2), int16_t(a2)));
    const int16x8x2_t eo = vld2q_s16(pm);                 // [0] = even states, [1] = odd states
    const int16x8_t x0 = vqaddq_s16(eo.val[0], A), y0 = vqsubq_s16(eo.val[1], A);
    const int16x8_t x1 = vqsubq_s16(eo.val[0], A), y1 = vqaddq_s16(eo.val[1], A);
    const uint16x8_t d0 = vcgtq_s16(y0, x0), d1 = vcgtq_s16(y1, x1);
    const int16x8_t n0 = vmaxq_s16(x0, y0), n1 = vmaxq_s16(x1, y1);
    static const uint16_t kW[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    const uint16x8_t w = vld1q_u16(kW);
    const uint16x8_t b0 = vandq_u16(d0, w), b1 = vandq_u16(d1, w);
    // ★ horizontal sums by pairwise adds (ARMv7 has no vaddvq)
    uint16x4_t s0 = vadd_u16(vget_low_u16(b0), vget_high_u16(b0)); s0 = vpadd_u16(s0, s0); s0 = vpadd_u16(s0, s0);
    uint16x4_t s1 = vadd_u16(vget_low_u16(b1), vget_high_u16(b1)); s1 = vpadd_u16(s1, s1); s1 = vpadd_u16(s1, s1);
    const uint16_t dec = uint16_t(vget_lane_u16(s0, 0) | (vget_lane_u16(s1, 0) << 8));
    const int16x8_t ref = vdupq_n_s16(vgetq_lane_s16(n0, 0));
    vst1q_s16(pm, vqsubq_s16(n0, ref));
    vst1q_s16(pm + 8, vqsubq_s16(n1, ref));
    return dec;
}
#endif
#if VIBE_M17_SSE
inline uint16_t acsSse(int16_t* pm, int a1, int a2) {
    const Signs& sg = signs();
    const __m128i A = _mm_adds_epi16(_mm_mullo_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sg.s1)), _mm_set1_epi16(int16_t(a1))),
                                     _mm_mullo_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sg.s2)), _mm_set1_epi16(int16_t(a2))));
    const __m128i lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pm));       // states 0..7
    const __m128i hi = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pm + 8));   // states 8..15
    // ★ deinterleave even / odd int16 lanes with SSE2 only: sign-extend the low half of each 32-bit lane (even states),
    //   arithmetic-shift the high half (odd states), and pack — the values are int16 already, so the pack never saturates
    const __m128i ev = _mm_packs_epi32(_mm_srai_epi32(_mm_slli_epi32(lo, 16), 16), _mm_srai_epi32(_mm_slli_epi32(hi, 16), 16));
    const __m128i od = _mm_packs_epi32(_mm_srai_epi32(lo, 16), _mm_srai_epi32(hi, 16));
    const __m128i x0 = _mm_adds_epi16(ev, A), y0 = _mm_subs_epi16(od, A);
    const __m128i x1 = _mm_subs_epi16(ev, A), y1 = _mm_adds_epi16(od, A);
    const __m128i d0 = _mm_cmpgt_epi16(y0, x0), d1 = _mm_cmpgt_epi16(y1, x1);
    const __m128i n0 = _mm_max_epi16(x0, y0), n1 = _mm_max_epi16(x1, y1);
    const uint16_t dec = uint16_t(_mm_movemask_epi8(_mm_packs_epi16(d0, d1)));   // lane j → bit j, lane j of d1 → bit 8+j
    const __m128i ref = _mm_set1_epi16(int16_t(_mm_extract_epi16(n0, 0)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(pm), _mm_subs_epi16(n0, ref));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(pm + 8), _mm_subs_epi16(n1, ref));
    return dec;
}
#endif
}  // namespace detail

/** Which ACS kernel to use — the build's vector path by default; the test forces each in turn. */
enum class Acs { Auto, Scalar };
inline const char* acsName() {
#if VIBE_M17_NEON
    return "NEON";
#elif VIBE_M17_SSE
    return "SSE2";
#else
    return "scalar";
#endif
}
/** Soft coded bits (2 per input bit, depunctured; length 2·n) → n − 4 data bits. The encoder starts AND ends in state 0
 *  (the four flush bits), so the trace-back starts from state 0. */
inline ViterbiOut viterbi(const int8_t* soft, size_t codedLen, Acs which = Acs::Auto) {
    ViterbiOut out;
    const size_t n = codedLen / 2;
    if (n < 5) return out;
    int16_t pm[16];
    pm[0] = 0;
    for (int s = 1; s < 16; ++s) pm[s] = -8000;   // ★ start in state 0
    std::vector<uint16_t> dec(n);
    long total = 0;
    for (size_t t = 0; t < n; ++t) {
        const int a1 = soft[2 * t], a2 = soft[2 * t + 1];
        total += std::abs(a1) + std::abs(a2);
        if (which == Acs::Scalar) { dec[t] = detail::acsScalar(pm, a1, a2); continue; }
#if VIBE_M17_NEON
        dec[t] = detail::acsNeon(pm, a1, a2);
#elif VIBE_M17_SSE
        dec[t] = detail::acsSse(pm, a1, a2);
#else
        dec[t] = detail::acsScalar(pm, a1, a2);
#endif
    }
    // ★ the metric of state 0 relative to the best: renormalisation subtracts the same from every state, so it is the
    //   winning path's margin that is lost — the absolute value is recovered below by re-encoding (frameQuality)
    unsigned s = 0;
    std::vector<uint8_t> rev(n);
    for (size_t t = n; t-- > 0;) {
        rev[t] = uint8_t(s >> 3);
        const unsigned j = s & 7u;
        s = (j << 1) | ((dec[t] >> s) & 1u);
    }
    out.bits.assign(rev.begin(), rev.begin() + long(n - 4));
    out.metric = int(std::min<long>(total, 1 << 30));
    return out;
}

// ── Bits and bytes ────────────────────────────────────────────────────────────────────────────────────────────────────
inline void bytesToBits(const uint8_t* p, size_t nBits, std::vector<uint8_t>& o) {
    for (size_t i = 0; i < nBits; ++i) o.push_back(uint8_t((p[i >> 3] >> (7 - (i & 7))) & 1));
}
inline void bitsToBytes(const uint8_t* b, size_t nBits, uint8_t* p) {
    std::memset(p, 0, (nBits + 7) / 8);
    for (size_t i = 0; i < nBits; ++i) if (b[i]) p[i >> 3] |= uint8_t(1u << (7 - (i & 7)));
}

// ── The link setup frame's contents (spec §3.6, §4) ───────────────────────────────────────────────────────────────────
struct Lsf {
    uint8_t b[30] = {};   ///< DST 0–5, SRC 6–11, TYPE 12–13, META 14–27, CRC 28–29
    uint64_t dst() const { return addrFrom(b); }
    uint64_t src() const { return addrFrom(b + 6); }
    uint16_t type() const { return uint16_t((b[12] << 8) | b[13]); }
    const uint8_t* meta() const { return b + 14; }
    bool stream() const { return type() & 1; }
    int dataType() const { return (type() >> 1) & 3; }      ///< 1 data, 2 voice (3200), 3 voice + data (1600)
    int encType() const { return (type() >> 3) & 3; }       ///< 0 none, 1 scrambler, 2 AES
    int encSub() const { return (type() >> 5) & 3; }        ///< with encType 0: 0 text, 1 GNSS, 2 extended callsign
    int can() const { return (type() >> 7) & 0xF; }
    bool signedStream() const { return (type() >> 11) & 1; }
    bool crcOk() const { return crc16(b, 30) == 0; }
    void setCrc() { const uint16_t c = crc16(b, 28); b[28] = uint8_t(c >> 8); b[29] = uint8_t(c); }
};
/** TYPE for a stream: P/S 1, the data type, no encryption (subtype = the META kind), the channel access number. */
inline uint16_t streamType(int dataType, int metaKind, int can) {
    return uint16_t(1 | ((dataType & 3) << 1) | ((metaKind & 3) << 5) | ((can & 0xF) << 7));
}
inline uint16_t packetType(int can) { return uint16_t((can & 0xF) << 7); }
inline Lsf makeLsf(const std::string& dst, const std::string& src, uint16_t type, const uint8_t* meta14) {
    Lsf l;
    addrTo(dst == "@ALL" ? kBroadcast : encodeCallsign(dst), l.b);
    addrTo(encodeCallsign(src), l.b + 6);
    l.b[12] = uint8_t(type >> 8); l.b[13] = uint8_t(type);
    if (meta14) std::memcpy(l.b + 14, meta14, 14);
    l.setCrc();
    return l;
}

// ── META (spec §4.2.2, null encryption) ───────────────────────────────────────────────────────────────────────────────
struct Gnss {
    bool posValid = false, altValid = false, velValid = false, radiusValid = false;
    int source = 0, station = 0;    ///< data source (0 M17 client, 1 OpenRTX, 15 other), station type (0 fixed, 1 mobile, 2 handheld)
    double lat = 0, lon = 0;        ///< degrees
    double altM = 0;                ///< metres
    int bearing = 0;                ///< degrees
    double speedKmh = 0;
    int radiusM = 0;                ///< 2^n metres
};
inline int32_t s24(const uint8_t* p) {
    int32_t v = (int32_t(p[0]) << 16) | (int32_t(p[1]) << 8) | p[2];
    if (v & 0x800000) v -= 0x1000000;
    return v;
}
inline Gnss parseGnss(const uint8_t* m) {
    Gnss g;
    g.source = m[0] >> 4; g.station = m[0] & 0xF;
    const int valid = m[1] >> 4;
    g.posValid = valid & 8; g.altValid = valid & 4; g.velValid = valid & 2; g.radiusValid = valid & 1;
    g.radiusM = 1 << ((m[1] >> 1) & 7);
    g.bearing = ((m[1] & 1) << 8) | m[2];
    g.lat = double(s24(m + 3)) * 90.0 / 8388607.0;
    g.lon = double(s24(m + 6)) * 180.0 / 8388607.0;
    g.altM = double((m[9] << 8) | m[10]) * 0.5 - 500.0;
    g.speedKmh = double((m[11] << 4) | (m[12] >> 4)) * 0.5;
    return g;
}
inline void makeGnss(const Gnss& g, uint8_t* m) {
    std::memset(m, 0, 14);
    m[0] = uint8_t(((g.source & 0xF) << 4) | (g.station & 0xF));
    int r = 0; while (r < 7 && (1 << r) < g.radiusM) ++r;
    const int valid = (g.posValid ? 8 : 0) | (g.altValid ? 4 : 0) | (g.velValid ? 2 : 0) | (g.radiusValid ? 1 : 0);
    m[1] = uint8_t((valid << 4) | (r << 1) | ((g.bearing >> 8) & 1));
    m[2] = uint8_t(g.bearing);
    auto put24 = [](uint8_t* p, double v) { int32_t x = int32_t(std::lround(v)); x &= 0xFFFFFF; p[0] = uint8_t(x >> 16); p[1] = uint8_t(x >> 8); p[2] = uint8_t(x); };
    put24(m + 3, g.lat / 90.0 * 8388607.0);
    put24(m + 6, g.lon / 180.0 * 8388607.0);
    const int alt = int(std::lround((g.altM + 500.0) * 2)); m[9] = uint8_t(alt >> 8); m[10] = uint8_t(alt);
    const int sp = int(std::lround(g.speedKmh * 2)); m[11] = uint8_t(sp >> 4); m[12] = uint8_t((sp & 0xF) << 4);
}
/** One text block of META: the control byte (high nibble = the blocks the message needs, low = this block's bit) and up
 *  to 13 bytes, space-padded. */
inline void makeTextBlock(const std::string& msg, int block, uint8_t* m) {
    std::memset(m, ' ', 14);
    const int blocks = std::max(1, std::min(4, int((msg.size() + 12) / 13)));
    m[0] = uint8_t((((1 << blocks) - 1) << 4) | (1 << block));
    for (int i = 0; i < 13; ++i) { const size_t k = size_t(block) * 13 + size_t(i); if (k < msg.size()) m[1 + i] = uint8_t(msg[k]); }
}

// ── Frames: the 368 type-4 bits each carries, then the randomiser ─────────────────────────────────────────────────────
/** 368 type-2/3 bits → interleaved → randomised → with the sync burst in front: 192 symbols. */
inline std::vector<int8_t> frameSymbols(Sync s, const std::vector<uint8_t>& t3) {
    std::vector<int8_t> sym(syncSymbols(s), syncSymbols(s) + 8);
    const auto& pi = qpp();
    uint8_t t4[kPayloadBits];
    for (int i = 0; i < kPayloadBits; ++i) t4[pi[size_t(i)]] = t3[size_t(i)];
    for (int i = 0; i < kPayloadBits; ++i) t4[i] ^= uint8_t(randBit(i));
    for (int i = 0; i < kPayloadBits; i += 2) sym.push_back(int8_t(dibitToSymbol(t4[i], t4[i + 1])));
    return sym;
}
inline std::vector<uint8_t> withFlush(std::vector<uint8_t> b) { b.insert(b.end(), 4, 0); return b; }
inline std::vector<int8_t> lsfFrame(const Lsf& l) {
    std::vector<uint8_t> t1; bytesToBits(l.b, 240, t1);
    return frameSymbols(Sync::Lsf, puncture(convEncode(withFlush(t1)), Punct::P1));
}
/** LICH for frame `fn`: the 40-bit chunk (fn mod 6) of the LSF and the counter, Golay-coded in four 12-bit parts. */
inline std::vector<uint8_t> lichBits(const Lsf& l, int cnt) {
    uint8_t lich[6];
    std::memcpy(lich, l.b + 5 * cnt, 5);
    lich[5] = uint8_t((cnt & 7) << 5);
    std::vector<uint8_t> o;
    for (int g = 0; g < 4; ++g) {
        const int bit0 = g * 12;
        uint16_t d = 0;
        for (int i = 0; i < 12; ++i) d = uint16_t((d << 1) | ((lich[(bit0 + i) >> 3] >> (7 - ((bit0 + i) & 7))) & 1));
        const uint32_t cw = golayEncode(d);
        for (int i = 23; i >= 0; --i) o.push_back(uint8_t((cw >> i) & 1));
    }
    return o;
}
/** A stream frame: frame number (bit 15 = the last frame) and 16 bytes of payload. */
inline std::vector<int8_t> streamFrame(const Lsf& l, int lichCnt, uint16_t fn, const uint8_t* payload16) {
    std::vector<uint8_t> t3 = lichBits(l, lichCnt);
    uint8_t c[18]; c[0] = uint8_t(fn >> 8); c[1] = uint8_t(fn); std::memcpy(c + 2, payload16, 16);
    std::vector<uint8_t> t1; bytesToBits(c, 144, t1);
    const std::vector<uint8_t> p = puncture(convEncode(withFlush(t1)), Punct::P2);
    t3.insert(t3.end(), p.begin(), p.end());
    return frameSymbols(Sync::Stream, t3);
}
/** A packet frame: 25 bytes and the 6 metadata bits (EOF, then the frame counter or, on the last, the bytes used). */
inline std::vector<int8_t> packetFrame(const uint8_t* chunk25, bool eof, int counter) {
    uint8_t c[26]; std::memcpy(c, chunk25, 25);
    c[25] = uint8_t(((eof ? 1 : 0) << 7) | ((counter & 0x1F) << 2));
    std::vector<uint8_t> t1; bytesToBits(c, 206, t1);
    return frameSymbols(Sync::Packet, puncture(convEncode(withFlush(t1)), Punct::P3));
}
/** The whole packet (type byte(s) + data) with its CRC, cut into packet frames. */
inline std::vector<std::vector<int8_t>> packetFrames(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> d = data;
    const uint16_t c = crc16(d.data(), d.size());
    d.push_back(uint8_t(c >> 8)); d.push_back(uint8_t(c));
    std::vector<std::vector<int8_t>> f;
    for (size_t at = 0, k = 0; at < d.size(); at += 25, ++k) {
        uint8_t chunk[25] = {};
        const size_t n = std::min<size_t>(25, d.size() - at);
        std::memcpy(chunk, d.data() + at, n);
        const bool last = at + n >= d.size();
        f.push_back(packetFrame(chunk, last, last ? int(n) : int(k)));
    }
    return f;
}
inline std::vector<int8_t> preamble(bool lsf = true) {
    std::vector<int8_t> p;
    for (int i = 0; i < kFrameSyms; ++i) p.push_back(int8_t(((i & 1) == 0) == lsf ? +3 : -3));
    return p;
}
inline std::vector<int8_t> eotMarker() {
    std::vector<int8_t> e;
    for (int i = 0; i < kFrameSyms / 8; ++i) e.insert(e.end(), syncSymbols(Sync::Eot), syncSymbols(Sync::Eot) + 8);
    return e;
}

// ── Taking frames apart ───────────────────────────────────────────────────────────────────────────────────────────────
/** 184 soft symbols (in symbol units: ±1, ±3) → 368 soft type-3 bits (int8, positive = 1): the dibits, then the randomiser
 *  undone (a sign flip), then the QPP undone. `scale` maps one symbol unit to the int8 range. */
inline void softBits(const float* sym184, float scale, int8_t* t3) {
    int8_t t4[kPayloadBits];
    auto q = [&](float v) { const float x = v * scale; return int8_t(std::max(-127.f, std::min(127.f, x))); };
    for (int k = 0; k < 184; ++k) {
        const float s = sym184[k];
        int8_t msb = q(-s);                  // ★ the sign bit: −1 / −3 are MSB 1
        int8_t lsb = q(std::fabs(s) - 2.f);  // ★ outer (±3) is LSB 1
        if (randBit(2 * k)) msb = int8_t(-msb);
        if (randBit(2 * k + 1)) lsb = int8_t(-lsb);
        t4[2 * k] = msb; t4[2 * k + 1] = lsb;
    }
    const auto& pi = qpp();
    for (int i = 0; i < kPayloadBits; ++i) t3[i] = t4[pi[size_t(i)]];
}
/** Re-encode the decoded bits and count how many of the received (hard, non-zero) punctured bits disagree — the frame's
 *  channel bit errors, the receiver's quality measure. */
inline int recodeErrors(const std::vector<uint8_t>& dataBits, const int8_t* soft, size_t nSoft, Punct p) {
    const std::vector<uint8_t> re = puncture(convEncode(withFlush(dataBits)), p);
    int e = 0;
    for (size_t i = 0; i < std::min(re.size(), nSoft); ++i) if (soft[i] != 0 && (soft[i] > 0) != (re[i] != 0)) ++e;
    return e;
}
struct Decoded {
    std::vector<uint8_t> bytes;   ///< the type-1 contents (LSF 30 bytes, stream 18, packet 26 — the last 6 bits in byte 25)
    int errors = 0;               ///< channel bits corrected
};
inline Decoded decodeLsf(const int8_t* t3, Acs a = Acs::Auto) {
    Decoded d;
    const std::vector<int8_t> c = depuncture(t3, kPayloadBits, 488, Punct::P1);
    const ViterbiOut v = viterbi(c.data(), c.size(), a);
    d.bytes.resize(30); bitsToBytes(v.bits.data(), 240, d.bytes.data());
    d.errors = recodeErrors(v.bits, t3, kPayloadBits, Punct::P1);
    return d;
}
inline Decoded decodeStreamPayload(const int8_t* t3, Acs a = Acs::Auto) {
    Decoded d;
    const std::vector<int8_t> c = depuncture(t3 + 96, 272, 296, Punct::P2);
    const ViterbiOut v = viterbi(c.data(), c.size(), a);
    d.bytes.resize(18); bitsToBytes(v.bits.data(), 144, d.bytes.data());
    d.errors = recodeErrors(v.bits, t3 + 96, 272, Punct::P2);
    return d;
}
inline Decoded decodePacket(const int8_t* t3, Acs a = Acs::Auto) {
    Decoded d;
    const std::vector<int8_t> c = depuncture(t3, kPayloadBits, 420, Punct::P3);
    const ViterbiOut v = viterbi(c.data(), c.size(), a);
    d.bytes.resize(26); bitsToBytes(v.bits.data(), 206, d.bytes.data());
    d.errors = recodeErrors(v.bits, t3, kPayloadBits, Punct::P3);
    return d;
}
/** The LICH: 6 bytes and the counter, or false when a Golay part could not be decoded. `golayErrs` = bits corrected. */
inline bool decodeLich(const int8_t* t3, uint8_t lich[6], int* cnt, int* golayErrs) {
    uint8_t bits[48];
    int tot = 0;
    for (int g = 0; g < 4; ++g) {
        int dist = 0;
        const int d = golaySoft(t3 + 24 * g, &dist);
        if (d < 0) return false;
        tot += dist;
        for (int i = 0; i < 12; ++i) bits[g * 12 + i] = uint8_t((d >> (11 - i)) & 1);
    }
    bitsToBytes(bits, 48, lich);
    if (cnt) *cnt = lich[5] >> 5;
    if (golayErrs) *golayErrs = tot;
    return (lich[5] >> 5) < 6;
}

}  // namespace m17
}  // namespace vibe
