// decoders/digimodes/q65_code.cpp — the Q65 frame, its code and its belief-propagation decoder. See q65_code.h for where
// every table came from (clean room: solved from the shipped encoder's output, no source read).
#include "q65_code.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
#include "ft8/message.h"
}
#include "../ft8_decoder.h"   // ft8CallHashes(): one table of heard calls for every FT8-family decoder

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#define Q65_NEON 1
#elif (defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define Q65_SSE2 1
#endif

namespace vibe {
namespace q65 {

const uint8_t kSyncPos[kSync] = {0, 8, 11, 12, 14, 21, 22, 25, 26, 32, 34, 37, 45, 49, 54, 59, 61, 65, 68, 73, 75, 84};
const uint8_t kStepInfo[kParity] = {13, 1, 3, 4, 8, 12, 9, 14, 10, 5, 0, 7, 1, 11, 8, 9, 12, 6, 3, 10, 7, 5, 2, 13, 12,
                                    4, 8, 0, 1, 11, 2, 9, 14, 5, 6, 13, 7, 12, 11, 2, 9, 0, 10, 4, 7, 14, 8, 11, 3, 6};
const uint8_t kStepW[kParity] = {1, 20, 1, 1, 10, 44, 1, 14, 31, 33, 56, 1, 21, 36, 33, 16, 8, 53, 34, 1, 53, 1, 60, 48, 1,
                                 55, 42, 57, 1, 1, 1, 33, 1, 32, 1, 49, 22, 37, 49, 61, 48, 1, 56, 54, 34, 15, 1, 20, 35, 52};

namespace {
struct Gf {
    uint8_t mul[64][64];
    uint8_t inv[64];
    Gf() {
        for (int a = 0; a < 64; ++a)
            for (int b = 0; b < 64; ++b) {
                int r = 0, x = a, y = b;
                while (y) { if (y & 1) r ^= x; y >>= 1; x <<= 1; if (x & 64) x ^= 0x43; }
                mul[a][b] = (uint8_t)r;
            }
        inv[0] = 0;
        for (int a = 1; a < 64; ++a) for (int b = 1; b < 64; ++b) if (mul[a][b] == 1) { inv[a] = (uint8_t)b; break; }
    }
};
const Gf& gf() { static const Gf g; return g; }

bool syncTable(int i) {
    static const struct T { bool s[kSymbols]; T() { std::memset(s, 0, sizeof s); for (uint8_t p : kSyncPos) s[p] = true; } } t;
    return t.s[i];
}

// ★ The hash interface for ft8_lib's packer/unpacker: the shared table every FT8-family decoder fills.
bool htLookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    const int bits = type == FTX_CALLSIGN_HASH_10_BITS ? 10 : type == FTX_CALLSIGN_HASH_12_BITS ? 12 : 22;
    return ft8CallHashes().lookup(bits, hash, callsign);
}
void htSave(const char* callsign, uint32_t n22) { ft8CallHashes().save(callsign, n22); }
ftx_callsign_hash_interface_t g_hash = {htLookup, htSave};
} // namespace

uint8_t gfMul(uint8_t a, uint8_t b) { return gf().mul[a & 63][b & 63]; }
uint8_t gfInv(uint8_t a) { return gf().inv[a & 63]; }
bool isSync(int i) { return i >= 0 && i < kSymbols && syncTable(i); }

void crc12(const uint8_t msg13[kMsg], uint8_t out2[2]) {
    unsigned r = 0;
    for (int s = 0; s < kMsg; ++s)
        for (int j = 0; j < 6; ++j) {
            const unsigned fb = (r & 1u) ^ ((msg13[s] >> j) & 1u);
            r >>= 1;
            if (fb) r ^= 0xF01u;
        }
    out2[0] = (uint8_t)(r & 63u);
    out2[1] = (uint8_t)(r >> 6);
}

void payloadToSymbols(const uint8_t p[10], uint8_t m[kMsg]) {
    for (int s = 0; s < kMsg; ++s) {
        int v = 0;
        for (int j = 0; j < 6; ++j) {
            const int bit = 6 * s + j;
            const int b = bit < 77 ? (p[bit / 8] >> (7 - bit % 8)) & 1 : 0;
            v = (v << 1) | b;
        }
        m[s] = (uint8_t)v;
    }
}

bool symbolsToPayload(const uint8_t m[kMsg], uint8_t p[10]) {
    std::memset(p, 0, 10);
    for (int bit = 0; bit < 78; ++bit) {
        const int b = (m[bit / 6] >> (5 - bit % 6)) & 1;
        if (bit == 77) return b == 0;
        if (b) p[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
    }
    return true;
}

void encodeInfo(const uint8_t x[kInfo], uint8_t cw[kCodeword]) {
    const Gf& g = gf();
    for (int i = 0; i < kInfo; ++i) cw[i] = x[i] & 63;
    uint8_t acc = 0;
    for (int k = 0; k < kParity; ++k) { acc ^= g.mul[kStepW[k]][x[kStepInfo[k]] & 63]; cw[kInfo + k] = acc; }
}

void encodeTones(const uint8_t msg13[kMsg], uint8_t tones[kSymbols]) {
    uint8_t x[kInfo], cw[kCodeword];
    std::memcpy(x, msg13, kMsg);
    crc12(msg13, x + kMsg);
    encodeInfo(x, cw);
    int d = 0;
    for (int i = 0; i < kSymbols; ++i) tones[i] = isSync(i) ? 0 : (uint8_t)(cw[dataToCodeword(d++)] + 1);
}

bool encodeText(const char* text, uint8_t tones[kSymbols]) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, &g_hash, text) != FTX_MESSAGE_RC_OK) return false;
    uint8_t s[kMsg];
    payloadToSymbols(m.payload, s);
    encodeTones(s, tones);
    return true;
}

// ── The 64-point Walsh-Hadamard transform ─────────────────────────────────────────────────────────────────────────────
// ★ Every path computes each output as the same sequence of IEEE adds and subtracts (a + (−b) is a − b exactly), so the
//   SIMD and scalar builds give bit-identical messages and identical decodes.
void wht64(float* v) {
#if Q65_NEON
    const float32x4_t s1 = {1.f, -1.f, 1.f, -1.f}, s2 = {1.f, 1.f, -1.f, -1.f};
    for (int i = 0; i < 64; i += 4) {
        float32x4_t x = vld1q_f32(v + i);
        float32x4x2_t t = vtrnq_f32(x, x);                         // [a a c c], [b b d d]
        x = vaddq_f32(t.val[0], vmulq_f32(t.val[1], s1));          // h = 1
        float32x4_t lo = vcombine_f32(vget_low_f32(x), vget_low_f32(x)), hi = vcombine_f32(vget_high_f32(x), vget_high_f32(x));
        vst1q_f32(v + i, vaddq_f32(lo, vmulq_f32(hi, s2)));       // h = 2
    }
    for (int h = 4; h < 64; h <<= 1)
        for (int i = 0; i < 64; i += 2 * h)
            for (int j = i; j < i + h; j += 4) {
                const float32x4_t a = vld1q_f32(v + j), b = vld1q_f32(v + j + h);
                vst1q_f32(v + j, vaddq_f32(a, b)); vst1q_f32(v + j + h, vsubq_f32(a, b));
            }
#elif Q65_SSE2
    const __m128 s1 = _mm_setr_ps(1.f, -1.f, 1.f, -1.f), s2 = _mm_setr_ps(1.f, 1.f, -1.f, -1.f);
    for (int i = 0; i < 64; i += 4) {
        __m128 x = _mm_loadu_ps(v + i);
        __m128 a = _mm_shuffle_ps(x, x, _MM_SHUFFLE(2, 2, 0, 0)), b = _mm_shuffle_ps(x, x, _MM_SHUFFLE(3, 3, 1, 1));
        x = _mm_add_ps(a, _mm_mul_ps(b, s1));
        a = _mm_shuffle_ps(x, x, _MM_SHUFFLE(1, 0, 1, 0)); b = _mm_shuffle_ps(x, x, _MM_SHUFFLE(3, 2, 3, 2));
        _mm_storeu_ps(v + i, _mm_add_ps(a, _mm_mul_ps(b, s2)));
    }
    for (int h = 4; h < 64; h <<= 1)
        for (int i = 0; i < 64; i += 2 * h)
            for (int j = i; j < i + h; j += 4) {
                const __m128 a = _mm_loadu_ps(v + j), b = _mm_loadu_ps(v + j + h);
                _mm_storeu_ps(v + j, _mm_add_ps(a, b)); _mm_storeu_ps(v + j + h, _mm_sub_ps(a, b));
            }
#else
    for (int h = 1; h < 64; h <<= 1)
        for (int i = 0; i < 64; i += 2 * h)
            for (int j = i; j < i + h; ++j) { const float a = v[j], b = v[j + h]; v[j] = a + b; v[j + h] = a - b; }
#endif
}

namespace {
// ★ Pointwise product, and normalisation to a probability vector. The sum is taken in four lanes then (l0+l1)+(l2+l3)
//   on every path, so scalar and SIMD agree to the bit.
inline void vmul(float* o, const float* a, const float* b) {
#if Q65_NEON
    for (int i = 0; i < 64; i += 4) vst1q_f32(o + i, vmulq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
#elif Q65_SSE2
    for (int i = 0; i < 64; i += 4) _mm_storeu_ps(o + i, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
#else
    for (int i = 0; i < 64; ++i) o[i] = a[i] * b[i];
#endif
}
constexpr float kFloor = 1e-9f;   // ★ no message is ever exactly 0: one overconfident symbol must not veto a codeword
/** Clamp negatives (the inverse transform's rounding) to 0, scale to sum 1, add the floor. */
inline void vnorm(float* v) {
#if Q65_NEON
    const float32x4_t z = vdupq_n_f32(0.f);
    float32x4_t acc = z;
    for (int i = 0; i < 64; i += 4) { float32x4_t x = vmaxq_f32(vld1q_f32(v + i), z); vst1q_f32(v + i, x); acc = vaddq_f32(acc, x); }
    float l[4]; vst1q_f32(l, acc);
    const float s = (l[0] + l[1]) + (l[2] + l[3]);
    if (!(s > 0.f) || !std::isfinite(s)) { for (int i = 0; i < 64; ++i) v[i] = 1.f / 64.f; return; }
    const float32x4_t k = vdupq_n_f32(1.f / s), f = vdupq_n_f32(kFloor);
    for (int i = 0; i < 64; i += 4) vst1q_f32(v + i, vaddq_f32(vmulq_f32(vld1q_f32(v + i), k), f));
#elif Q65_SSE2
    const __m128 z = _mm_setzero_ps();
    __m128 acc = z;
    for (int i = 0; i < 64; i += 4) { __m128 x = _mm_max_ps(_mm_loadu_ps(v + i), z); _mm_storeu_ps(v + i, x); acc = _mm_add_ps(acc, x); }
    float l[4]; _mm_storeu_ps(l, acc);
    const float s = (l[0] + l[1]) + (l[2] + l[3]);
    if (!(s > 0.f) || !std::isfinite(s)) { for (int i = 0; i < 64; ++i) v[i] = 1.f / 64.f; return; }
    const __m128 k = _mm_set1_ps(1.f / s), f = _mm_set1_ps(kFloor);
    for (int i = 0; i < 64; i += 4) _mm_storeu_ps(v + i, _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(v + i), k), f));
#else
    float l[4] = {0, 0, 0, 0};
    for (int i = 0; i < 64; ++i) { if (!(v[i] > 0.f)) v[i] = 0.f; l[i & 3] += v[i]; }
    const float s = (l[0] + l[1]) + (l[2] + l[3]);
    if (!(s > 0.f) || !std::isfinite(s)) { for (int i = 0; i < 64; ++i) v[i] = 1.f / 64.f; return; }
    const float k = 1.f / s;
    for (int i = 0; i < 64; ++i) v[i] = v[i] * k + kFloor;
#endif
}
/** o = normalised inverse transform of (wa · wb): the XOR-convolution of the two distributions whose transforms they are. */
inline void xconv(float* o, const float* wa, const float* wb) { vmul(o, wa, wb); wht64(o); vnorm(o); }

struct Edges {   // the checks each information symbol takes part in
    int n[kInfo] = {};
    int k[kInfo][6] = {};
    Edges() { for (int s = 0; s < kParity; ++s) { const int j = kStepInfo[s]; k[j][n[j]++] = s; } }
};
const Edges& edges() { static const Edges e; return e; }
} // namespace

struct alignas(16) BpScratch {
    alignas(16) float L[kCodeword][64];     // intrinsic, per codeword symbol (13, 14 uniform)
    alignas(16) float r[kParity][64];       // check → information symbol (x domain)
    alignas(16) float Q[kParity][64];       // information → check, as the distribution of w·x
    alignas(16) float WQ[kParity][64];
    alignas(16) float F[kParity][64];       // check k → p[k]
    alignas(16) float A[kParity][64];       // p[k] → check k+1
    alignas(16) float WA[kParity][64];
    alignas(16) float B[kParity + 1][64];   // check k → p[k−1]   (B[kParity] = uniform)
    alignas(16) float D[kParity][64];       // p[k] → check k
    alignas(16) float WD[kParity][64];
    alignas(16) float t[64], u[64];
    uint8_t perm[kParity][64];              // x → w·x
};

BpScratch* bpScratchNew() {
    void* p = nullptr;
#if defined(_WIN32)
    p = _aligned_malloc(sizeof(BpScratch), 16);
#else
    if (posix_memalign(&p, 16, sizeof(BpScratch)) != 0) p = nullptr;
#endif
    if (!p) return nullptr;
    BpScratch* s = new (p) BpScratch;
    const Gf& g = gf();
    for (int k = 0; k < kParity; ++k) for (int x = 0; x < 64; ++x) s->perm[k][x] = g.mul[kStepW[k]][x];
    return s;
}
void bpScratchFree(BpScratch* s) {
    if (!s) return;
#if defined(_WIN32)
    _aligned_free(s);
#else
    std::free(s);
#endif
}

int bpDecode(const float* intr, int maxIter, uint8_t msgOut[kMsg], BpScratch* s, const uint8_t* apMask, const uint8_t* apVal) {
    if (!s) return 0;
    const Edges& E = edges();
    // ── intrinsic ──
    for (int d = 0; d < kData; ++d) {
        float* L = s->L[dataToCodeword(d)];
        std::memcpy(L, intr + d * 64, 64 * sizeof(float));
    }
    for (int v = 0; v < 64; ++v) s->L[13][v] = s->L[14][v] = 1.f;
    for (int i = 0; i < kMsg; ++i) {
        uint8_t m = apMask ? apMask[i] : 0, val = apVal ? apVal[i] : 0;
        if (i == 12) { m |= 1; val &= 0x3E; }                 // ★ bit 77: always 0 on air
        if (m) for (int v = 0; v < 64; ++v) if (((uint8_t)v & m) != (val & m)) s->L[i][v] = 0.f;
    }
    for (int i = 0; i < kCodeword; ++i) vnorm(s->L[i]);
    for (int k = 0; k < kParity; ++k) for (int v = 0; v < 64; ++v) s->r[k][v] = 1.f / 64.f;
    for (int v = 0; v < 64; ++v) s->B[kParity][v] = 1.f / 64.f;

    uint8_t x[kInfo];
    for (int it = 1; it <= maxIter; ++it) {
        // ── information → check (leave-one-out), mapped to w·x, transformed ──
        for (int j = 0; j < kInfo; ++j)
            for (int e = 0; e < E.n[j]; ++e) {
                const int k = E.k[j][e];
                std::memcpy(s->t, s->L[j], sizeof s->t);
                for (int f = 0; f < E.n[j]; ++f) if (f != e) vmul(s->t, s->t, s->r[E.k[j][f]]);
                vnorm(s->t);
                for (int v = 0; v < 64; ++v) s->Q[k][s->perm[k][v]] = s->t[v];
                std::memcpy(s->WQ[k], s->Q[k], sizeof s->t);
                wht64(s->WQ[k]);
            }
        // ── forward along the accumulator: F[k] = (L·F)[k−1] ⊛ Q[k] ──
        std::memcpy(s->F[0], s->Q[0], sizeof s->t);
        for (int k = 1; k < kParity; ++k) {
            vmul(s->A[k - 1], s->L[kInfo + k - 1], s->F[k - 1]); vnorm(s->A[k - 1]);
            std::memcpy(s->WA[k - 1], s->A[k - 1], sizeof s->t); wht64(s->WA[k - 1]);
            xconv(s->F[k], s->WA[k - 1], s->WQ[k]);
        }
        // ── backward: B[k] (to p[k−1]) = (L·B)[p[k]] ⊛ Q[k] ──
        for (int k = kParity - 1; k >= 1; --k) {
            vmul(s->D[k], s->L[kInfo + k], s->B[k + 1]); vnorm(s->D[k]);
            std::memcpy(s->WD[k], s->D[k], sizeof s->t); wht64(s->WD[k]);
            xconv(s->B[k], s->WD[k], s->WQ[k]);
        }
        vmul(s->D[0], s->L[kInfo], s->B[1]); vnorm(s->D[0]);
        // ── check → information: the distribution of p[k] ⊕ p[k−1] = w·x, read back through w ──
        for (int v = 0; v < 64; ++v) s->r[0][v] = s->D[0][s->perm[0][v]];
        vnorm(s->r[0]);
        for (int k = 1; k < kParity; ++k) {
            xconv(s->u, s->WA[k - 1], s->WD[k]);
            for (int v = 0; v < 64; ++v) s->r[k][v] = s->u[s->perm[k][v]];
            vnorm(s->r[k]);
        }
        // ── decide ──
        for (int j = 0; j < kInfo; ++j) {
            std::memcpy(s->t, s->L[j], sizeof s->t);
            for (int f = 0; f < E.n[j]; ++f) vmul(s->t, s->t, s->r[E.k[j][f]]);
            int best = 0; float bv = s->t[0];
            for (int v = 1; v < 64; ++v) if (s->t[v] > bv) { bv = s->t[v]; best = v; }
            x[j] = (uint8_t)best;
        }
        uint8_t c[2];
        crc12(x, c);
        if (c[0] == x[13] && c[1] == x[14] && (x[12] & 1) == 0) {
            std::memcpy(msgOut, x, kMsg);
            return it;
        }
    }
    return 0;
}

} // namespace q65
} // namespace vibe

namespace vibe {
namespace q65 {

bool unpackPayload(const uint8_t payload10[10], bool learn, std::string& text, std::string& call, std::string& grid,
                   bool& plausible) {
    text.clear(); call.clear(); grid.clear(); plausible = false;
    ftx_message_t m; ftx_message_init(&m);
    std::memcpy(m.payload, payload10, 10);
    char txt[FTX_MAX_MESSAGE_LENGTH] = {};
    ftx_message_offsets_t offs;
    if (ftx_message_decode(&m, learn ? &g_hash : nullptr, txt, &offs) != FTX_MESSAGE_RC_OK) return false;
    text = txt;
    plausible = ft8OsdPlausible(m);
    const ftx_message_type_t t = ftx_message_get_type(&m);
    if (t == FTX_MESSAGE_TYPE_STANDARD || ftx_message_get_i3(&m) == 2) {
        char to[24] = {}, de[24] = {}, ex[24] = {};
        ftx_field_t f[FTX_MAX_MESSAGE_FIELDS];
        if (ftx_message_decode_std(&m, learn ? &g_hash : nullptr, to, de, ex, f) == FTX_MESSAGE_RC_OK) {
            std::string c;
            if (Ft8Decoder::spotCallsign(de, c)) call = c;
            const bool realGrid = std::strlen(ex) == 4 && ex[0] >= 'A' && ex[0] <= 'R' && ex[1] >= 'A' && ex[1] <= 'R'
                && ex[2] >= '0' && ex[2] <= '9' && ex[3] >= '0' && ex[3] <= '9' && std::strcmp(ex, "RR73") != 0;
            if (realGrid) grid = ex;
        }
    }
    return true;
}

void cqApMask(uint8_t mask[kMsg], uint8_t val[kMsg]) {
    std::memset(mask, 0, kMsg); std::memset(val, 0, kMsg);
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, nullptr, "CQ K1ABC FN42") != FTX_MESSAGE_RC_OK) return;
    uint8_t s[kMsg];
    payloadToSymbols(m.payload, s);
    auto known = [&](int bit) { mask[bit / 6] |= (uint8_t)(0x20 >> (bit % 6)); };
    for (int b = 0; b < 29; ++b) known(b);          // c28 = "CQ", and its ipa bit
    for (int b = 74; b < 78; ++b) known(b);         // i3 = 1, and the 0 bit
    for (int i = 0; i < kMsg; ++i) val[i] = s[i] & mask[i];
}

} // namespace q65
} // namespace vibe
