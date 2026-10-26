// VibeSDR V12 — WSPR-2 decoder, our own engine (see wspr_decoder.h for the method).
// Written from the published protocol description; no other implementation's code, tables or structure.
#include "wspr_decoder.h"
#include "../../vibe_clock.h"
#include "../../vibedsp/vibe_denormals.h"
extern "C" {
#include "../../vibedsp/third_party/pffft/pffft.h"
}
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

/* ★ Four floats at a time for the three hot loops (FIR dot products, the search's row sums, the tone DFTs):
 *  NEON on ARMv7 and AArch64, SSE2 as the x86 floor, plain C otherwise. VIBE_FORCE_SCALAR builds the plain path
 *  anywhere — the reference the vector ones are tested against (test-wspr runs both on the bench file). */
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VIBE_FORCE_SCALAR)
  #include <arm_neon.h>
  #define VIBE_WSPR_NEON 1
#elif (defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)) && !defined(VIBE_FORCE_SCALAR)
  #include <emmintrin.h>
  #define VIBE_WSPR_SSE2 1
#endif

namespace vibe {
namespace wspr {

// ── The published constants ─────────────────────────────────────────────────
const uint8_t kSyncVector[kSymbols] = {
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,
    0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,
    0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,
    0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
    0,0 };
static constexpr uint32_t kPoly1 = 0xF2D05351u, kPoly2 = 0xE4613C47u;   // K = 32, r = ½ generators
static_assert((kPoly1 & kPoly2 & 1u) != 0, "the Fano branch metrics rely on both generators tapping the newest bit");

/* ★ Parity: one instruction or two on x86 (the parity flag) and AArch64, but a LIBRARY CALL on 32-bit ARM — on the
 *  Pi 2 build it was 13 % of a busy slot's instructions (qemu count, 2026-10-26). There it is folded to four bits and
 *  looked up in a 16-bit constant. */
static inline int parity32(uint32_t v) {
#if defined(__arm__) && !defined(__aarch64__)
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4;
    return (0x6996u >> (v & 15u)) & 1;
#else
    return __builtin_parity(v);
#endif
}

// ── The bit-reversal interleaver ────────────────────────────────────────────
struct Interleave {
    int pos[kSymbols];
    Interleave() {
        int p = 0;
        for (int i = 0; i < 256 && p < kSymbols; ++i) {
            int j = 0;
            for (int b = 0; b < 8; ++b) if (i & (1 << b)) j |= 1 << (7 - b);
            if (j < kSymbols) pos[p++] = j;
        }
    }
};
static const Interleave& il() { static const Interleave t; return t; }
int interleavePos(int p) { return il().pos[p]; }

// ── Characters ──────────────────────────────────────────────────────────────
static int chv(char c) {           // 0-9 → 0..9, A-Z → 10..35, space → 36
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c == ' ') return 36;
    return -1;
}
static char vch(int v) { return v < 10 ? char('0' + v) : v < 36 ? char('A' + v - 10) : ' '; }
static std::string upper(std::string s) { for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 32); return s; }
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
static bool isLetter(char c) { return c >= 'A' && c <= 'Z'; }
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

/* The 28-bit callsign field: six positions, the third always a digit (a call whose digit is second gets a leading
 *  space), the last three letters or trailing spaces. */
static bool packCall6(const std::string& callIn, uint32_t& n) {
    const std::string c = upper(callIn);
    if (c.size() < 2 || c.size() > 6) return false;
    char a[6] = {' ', ' ', ' ', ' ', ' ', ' '};
    if (c.size() >= 3 && isDigit(c[2])) { for (size_t i = 0; i < c.size(); ++i) a[i] = c[i]; }
    else if (isDigit(c[1]) && c.size() <= 5) { for (size_t i = 0; i < c.size(); ++i) a[i + 1] = c[i]; }
    else return false;
    if (chv(a[0]) < 0 || a[1] == ' ' || chv(a[1]) < 0 || chv(a[1]) > 35 || !isDigit(a[2])) return false;
    for (int i = 3; i < 6; ++i) if (!(isLetter(a[i]) || a[i] == ' ')) return false;
    uint32_t v = (uint32_t)chv(a[0]);
    v = v * 36 + (uint32_t)chv(a[1]);
    v = v * 10 + (uint32_t)chv(a[2]);
    for (int i = 3; i < 6; ++i) v = v * 27 + (uint32_t)(a[i] == ' ' ? 26 : a[i] - 'A');
    n = v;
    return true;
}
static bool unpackCall6(uint32_t n, char a[6]) {
    if (n >= 262177560u) return false;                  // 37·36·10·27³
    for (int i = 5; i >= 3; --i) { const int v = (int)(n % 27); n /= 27; a[i] = v == 26 ? ' ' : char('A' + v); }
    a[2] = char('0' + n % 10); n /= 10;
    a[1] = vch((int)(n % 36)); n /= 36;
    a[0] = vch((int)n);
    return true;
}
/** A call as a station would send it: the suffix's spaces only trailing and at least one letter, no "space, digit"
 *  start. Everything else the field can hold is noise that the code happened to accept. */
static bool plausibleCall(const char a[6]) {
    if (a[0] == ' ' && !isLetter(a[1])) return false;
    if (a[3] == ' ') return false;                       // no suffix letter at all
    if (a[4] == ' ' && a[5] != ' ') return false;
    return true;
}
static bool legalDbm(int d) { if (d < 0 || d > 60) return false; const int u = d % 10; return u == 0 || u == 3 || u == 7; }
static bool packGrid4(const std::string& gIn, uint32_t& m) {
    const std::string g = upper(gIn);
    if (g.size() != 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isDigit(g[2]) || !isDigit(g[3])) return false;
    m = (uint32_t)((179 - 10 * (g[0] - 'A') - (g[2] - '0')) * 180 + 10 * (g[1] - 'A') + (g[3] - '0'));
    return true;
}
static bool unpackGrid4(uint32_t m, std::string& g) {
    if (m >= 32400) return false;
    const int v = 179 - (int)(m / 180), r = (int)(m % 180);
    const int l1 = v / 10, l3 = v % 10, l2 = r / 10, l4 = r % 10;
    if (l1 < 0 || l1 > 17 || l2 > 17) return false;
    g = { char('A' + l1), char('A' + l2), char('0' + l3), char('0' + l4) };
    return true;
}
static bool validGrid6(const std::string& g) {
    return g.size() == 6 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' && isDigit(g[2]) && isDigit(g[3])
        && g[4] >= 'A' && g[4] <= 'X' && g[5] >= 'A' && g[5] <= 'X';
}

// ── The callsign hash: Bob Jenkins' lookup3 ("hashlittle", public domain), byte-wise ─
static inline uint32_t rot(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
uint32_t callHash(const std::string& callIn) {
    const std::string s = upper(callIn);
    const uint8_t* k = (const uint8_t*)s.data();
    size_t len = s.size();
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + (uint32_t)len + 146u;
    while (len > 12) {
        a += k[0] | (uint32_t)k[1] << 8 | (uint32_t)k[2] << 16 | (uint32_t)k[3] << 24;
        b += k[4] | (uint32_t)k[5] << 8 | (uint32_t)k[6] << 16 | (uint32_t)k[7] << 24;
        c += k[8] | (uint32_t)k[9] << 8 | (uint32_t)k[10] << 16 | (uint32_t)k[11] << 24;
        a -= c; a ^= rot(c, 4);  c += b;
        b -= a; b ^= rot(a, 6);  a += c;
        c -= b; c ^= rot(b, 8);  b += a;
        a -= c; a ^= rot(c, 16); c += b;
        b -= a; b ^= rot(a, 19); a += c;
        c -= b; c ^= rot(b, 4);  b += a;
        len -= 12; k += 12;
    }
    if (len == 0) return c & 32767u;
    // the last 1…12 bytes, zero-padded into a, b, c
    uint32_t w[3] = {0, 0, 0};
    for (size_t i = 0; i < len; ++i) w[i / 4] |= (uint32_t)k[i] << (8 * (i % 4));
    a += w[0]; b += w[1]; c += w[2];
    c ^= b; c -= rot(b, 14);
    a ^= c; a -= rot(c, 11);
    b ^= a; b -= rot(a, 25);
    c ^= b; c -= rot(b, 16);
    a ^= c; a -= rot(c, 4);
    b ^= a; b -= rot(a, 14);
    c ^= b; c -= rot(b, 24);
    return c & 32767u;
}

std::string Message::text() const {
    char p[8]; std::snprintf(p, sizeof p, "%d", dbm);
    if (type == 2) return call + " " + p;
    if (type == 3) return "<" + (call.empty() ? std::string("...") : call) + "> " + grid + " " + p;
    return call + " " + grid + " " + p;
}

// ── Pack / unpack the 50 bits ───────────────────────────────────────────────
static void putBits(uint32_t n, uint32_t m, uint8_t bits[7]) {
    const uint64_t v = ((uint64_t)n << 22) | (m & 0x3FFFFFu);
    std::memset(bits, 0, 7);
    for (int i = 0; i < kMsgBits; ++i) if ((v >> (49 - i)) & 1) bits[i / 8] |= (uint8_t)(0x80 >> (i % 8));
}
static void getBits(const uint8_t bits[7], uint32_t& n, uint32_t& m) {
    uint64_t v = 0;
    for (int i = 0; i < kMsgBits; ++i) v = (v << 1) | ((bits[i / 8] >> (7 - i % 8)) & 1);
    n = (uint32_t)(v >> 22); m = (uint32_t)(v & 0x3FFFFFu);
}

bool pack(const Message& msg, uint8_t bits[7]) {
    if (msg.type == 1) {
        uint32_t n, g;
        if (!legalDbm(msg.dbm) || !packCall6(msg.call, n) || !packGrid4(msg.grid, g)) return false;
        putBits(n, g * 128 + (uint32_t)(msg.dbm + 64), bits);
        return true;
    }
    if (msg.type == 2) {
        const std::string c = upper(msg.call);
        const size_t sl = c.find('/');
        if (sl == std::string::npos || c.find('/', sl + 1) != std::string::npos || !legalDbm(msg.dbm)) return false;
        const std::string before = c.substr(0, sl), after = c.substr(sl + 1);
        uint32_t n, n3;
        bool isSuffix = false;
        if (after.size() == 1 && chv(after[0]) >= 0 && chv(after[0]) < 36) { isSuffix = true; n3 = 60000u + (uint32_t)chv(after[0]); }
        else if (after.size() == 2 && isDigit(after[0]) && isDigit(after[1]) && after[0] != '0') {
            isSuffix = true; n3 = 60000u + 26u + (uint32_t)(10 * (after[0] - '0') + (after[1] - '0'));
        }
        if (isSuffix) { if (!packCall6(before, n)) return false; }
        else {
            if (before.empty() || before.size() > 3 || !packCall6(after, n)) return false;
            uint32_t v = 0;
            const std::string p3 = std::string(3 - before.size(), ' ') + before;   // right-aligned
            for (char ch : p3) { const int x = chv(ch); if (x < 0) return false; v = v * 37 + (uint32_t)x; }
            n3 = v;
        }
        const int nadd = (int)(n3 / 32768u) + 1;
        const uint32_t ng = n3 - 32768u * (uint32_t)(nadd - 1);
        putBits(n, ng * 128 + (uint32_t)(msg.dbm + nadd + 64), bits);
        return true;
    }
    if (msg.type == 3) {
        const std::string g = upper(msg.grid);
        if (!validGrid6(g) || msg.dbm < 0 || msg.dbm > 60 || msg.call.empty()) return false;
        uint32_t n;
        if (!packCall6(g.substr(1) + g.substr(0, 1), n)) return false;
        const uint32_t h = callHash(msg.call);
        putBits(n, h * 128 + (uint32_t)(63 - msg.dbm), bits);
        return true;
    }
    return false;
}

bool unpack(const uint8_t bits[7], Message& out, CallHashTable* table) {
    uint32_t n, m;
    getBits(bits, n, m);
    char a[6];
    if (!unpackCall6(n, a)) return false;
    const int ntype = (int)(m & 127) - 64;
    const uint32_t ng = m >> 7;
    out = Message();
    if (ntype >= 0 && ntype <= 62) {
        const int nu = ntype % 10;
        if (!plausibleCall(a)) return false;
        const std::string base = trim(std::string(a, 6));
        if (nu == 0 || nu == 3 || nu == 7) {                       // type 1
            std::string g;
            if (!unpackGrid4(ng, g)) return false;
            out.type = 1; out.call = base; out.grid = g; out.dbm = ntype;
            return true;
        }
        int nadd = nu; if (nu > 3) nadd = nu - 3; if (nu > 7) nadd = nu - 7;
        const uint32_t n3 = ng + 32768u * (uint32_t)(nadd - 1);
        std::string call;
        if (n3 < 60000u) {                                        // prefix of 1…3 characters
            char p[3]; uint32_t v = n3;
            for (int i = 2; i >= 0; --i) { p[i] = vch((int)(v % 37)); v /= 37; }
            if (v) return false;
            // spaces only leading (what a sender produces), at least one character
            int first = 0; while (first < 3 && p[first] == ' ') ++first;
            if (first == 3) return false;
            for (int i = first; i < 3; ++i) if (p[i] == ' ') return false;
            call = std::string(p + first, 3 - first) + "/" + base;
        } else {
            const uint32_t nc = n3 - 60000u;
            if (nc <= 35) call = base + "/" + vch((int)nc);
            else if (nc <= 125) { const int d = (int)nc - 26; call = base + "/" + char('0' + d / 10) + char('0' + d % 10); }
            else return false;
        }
        const int dbm = ntype - nadd;
        if (!legalDbm(dbm)) return false;
        out.type = 2; out.call = call; out.dbm = dbm;
        return true;
    }
    if (ntype < 0) {                                              // type 3: hashed call + 6-char grid
        const int dbm = -(ntype + 1);
        if (!legalDbm(dbm)) return false;
        const std::string g = std::string(1, a[5]) + std::string(a, 5);
        if (!validGrid6(g)) return false;
        out.type = 3; out.grid = g; out.dbm = dbm; out.hash = ng;
        if (table) table->lookup(ng, out.call);
        return true;
    }
    return false;
}

void encodeBits(const uint8_t bits[7], uint8_t tones[kSymbols]) {
    uint8_t coded[kSymbols];
    uint32_t reg = 0;
    for (int i = 0; i < kCodedBits; ++i) {
        const int b = i < kMsgBits ? (bits[i / 8] >> (7 - i % 8)) & 1 : 0;
        reg = (reg << 1) | (uint32_t)b;
        coded[2 * i] = (uint8_t)parity32(reg & kPoly1);
        coded[2 * i + 1] = (uint8_t)parity32(reg & kPoly2);
    }
    uint8_t chan[kSymbols];
    for (int p = 0; p < kSymbols; ++p) chan[il().pos[p]] = coded[p];
    for (int j = 0; j < kSymbols; ++j) tones[j] = (uint8_t)(kSyncVector[j] + 2 * chan[j]);
}
bool encode(const Message& m, uint8_t tones[kSymbols]) {
    uint8_t bits[7];
    if (!pack(m, bits)) return false;
    encodeBits(bits, tones);
    return true;
}

/* ★★ THE FANO DECODER. A sequential search of the code tree: move forward along the better branch while the running
 *  metric stays above a threshold T; when it cannot, back up and try the other branch, lowering T by Δ only when
 *  nothing above it is left; on the first visit to a node, raise T as far as the metric allows. The branch metric is
 *  Fano's: log2 p(r|x)/p(r) − R per coded bit, from the calibrated LLRs, so a correct path climbs ~0.2-0.5 a branch and
 *  a wrong one falls fast. The last 31 bits are the known zero tail: one branch each. Work is bounded by `maxCycles`. */
bool fanoDecode(const float llr[kSymbols], long maxCycles, float delta, uint8_t bits[7], long* cyclesOut,
                const std::atomic<bool>* abort) {
    float bm[kSymbols][2];
    for (int p = 0; p < kSymbols; ++p) {
        const float L = std::max(-30.f, std::min(30.f, llr[p]));
        // log2(2·P(b)) − ½ with P(1) = 1/(1+e^−L): 1 − log2(1+e^∓L) − ½
        auto sp = [](float x) { return x > 20.f ? x : std::log1p(std::exp(x)); };   // softplus
        bm[p][1] = 0.5f - sp(-L) * 1.44269504f;
        bm[p][0] = 0.5f - sp(L) * 1.44269504f;
    }
    struct Node { uint32_t state; float gamma; float m[2]; uint8_t best; uint8_t tried; };
    Node nd[kCodedBits + 1];
    auto branches = [&](int i) {
        Node& x = nd[i];
        // both generators are odd, so the 1-branch's two outputs are the 0-branch's inverted: two parities, not four
        const uint32_t r = x.state << 1;
        const int o1 = parity32(r & kPoly1), o2 = parity32(r & kPoly2);
        x.m[0] = bm[2 * i][o1] + bm[2 * i + 1][o2];
        x.m[1] = bm[2 * i][o1 ^ 1] + bm[2 * i + 1][o2 ^ 1];
        if (i >= kMsgBits) { x.best = 0; x.m[1] = -1e30f; }
        else x.best = x.m[1] > x.m[0] ? 1 : 0;
    };
    nd[0].state = 0; nd[0].gamma = 0; nd[0].tried = 0;
    branches(0);
    float T = 0;
    int i = 0;
    long cyc = 0;
    bool done = false;
    for (; cyc < maxCycles; ++cyc) {
        if ((cyc & 4095) == 0 && abort && abort->load(std::memory_order_relaxed)) break;
        Node& x = nd[i];
        const int b = x.tried ? 1 - x.best : x.best;
        const float ng = x.gamma + x.m[b];
        if (ng >= T) {
            if (x.gamma < T + delta && ng >= T + delta)                    // first visit: tighten, in one step
                T += delta * std::floor((ng - T) / delta);
            Node& y = nd[i + 1];
            y.gamma = ng; y.tried = 0;
            y.state = (x.state << 1) | (uint32_t)b;
            ++i;
            if (i == kCodedBits) { done = true; break; }
            branches(i);
            continue;
        }
        for (;;) {                                                       // look back
            if (i == 0 || nd[i - 1].gamma < T) { T -= delta; nd[i].tried = 0; break; }
            --i;
            if (!nd[i].tried && i < kMsgBits) { nd[i].tried = 1; break; }
        }
    }
    if (cyclesOut) *cyclesOut = cyc;
    if (!done) return false;
    std::memset(bits, 0, 7);
    for (int k = 0; k < kMsgBits; ++k) if (nd[k + 1].state & 1) bits[k / 8] |= (uint8_t)(0x80 >> (k % 8));
    return true;
}

// ── The hash table ──────────────────────────────────────────────────────────
void CallHashTable::save(const std::string& callIn) {
    const std::string call = upper(callIn);
    if (call.empty() || call.size() > 13) return;
    const uint32_t h = callHash(call);
    std::lock_guard<std::mutex> lk(mtx_);
    const uint64_t now = ++clock_;
    int freeAt = -1, oldest = -1;
    for (int i = 0; i < kCapacity; ++i) {
        Entry& x = e_[i];
        if (!x.stamp) { if (freeAt < 0) freeAt = i; continue; }
        if (x.h == h && call == x.call) { x.stamp = now; return; }
        if (oldest < 0 || x.stamp < e_[oldest].stamp) oldest = i;
    }
    Entry& x = e_[freeAt >= 0 ? freeAt : oldest];
    std::snprintf(x.call, sizeof x.call, "%s", call.c_str());
    x.h = h; x.stamp = now;
}
bool CallHashTable::lookup(uint32_t h, std::string& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    int hit = -1;
    for (int i = 0; i < kCapacity; ++i) {
        const Entry& x = e_[i];
        if (!x.stamp || x.h != (h & 32767u)) continue;
        if (hit >= 0 && std::strcmp(e_[hit].call, x.call) != 0) { out.clear(); return false; }   // ambiguous
        hit = i;
    }
    if (hit < 0) { out.clear(); return false; }
    e_[hit].stamp = ++clock_;
    out = e_[hit].call;
    return true;
}
void CallHashTable::clear() { std::lock_guard<std::mutex> lk(mtx_); for (auto& x : e_) x = Entry{}; clock_ = 0; }
int CallHashTable::size() { std::lock_guard<std::mutex> lk(mtx_); int n = 0; for (auto& x : e_) n += x.stamp != 0; return n; }
CallHashTable& callHashes() { static CallHashTable t; return t; }

} // namespace wspr

// ── SIMD kernels ────────────────────────────────────────────────────────────
namespace {

/** Σ a[i]·b[i]. */
inline float dotf(const float* a, const float* b, int n) {
    int i = 0;
#if VIBE_WSPR_NEON
    float32x4_t s0 = vdupq_n_f32(0), s1 = vdupq_n_f32(0);
    for (; i + 8 <= n; i += 8) {
        s0 = vmlaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
        s1 = vmlaq_f32(s1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    }
    s0 = vaddq_f32(s0, s1);
    float t[4]; vst1q_f32(t, s0);
    float s = (t[0] + t[1]) + (t[2] + t[3]);
#elif VIBE_WSPR_SSE2
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    for (; i + 8 <= n; i += 8) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    s0 = _mm_add_ps(s0, s1);
    float t[4]; _mm_storeu_ps(t, s0);
    float s = (t[0] + t[1]) + (t[2] + t[3]);
#else
    float s = 0;
#endif
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}

/** acc[i] += sign·row[i] for n (a multiple of 4). */
inline void addRow(float* acc, const float* row, int n, bool neg) {
    int i = 0;
#if VIBE_WSPR_NEON
    if (neg) for (; i + 4 <= n; i += 4) vst1q_f32(acc + i, vsubq_f32(vld1q_f32(acc + i), vld1q_f32(row + i)));
    else     for (; i + 4 <= n; i += 4) vst1q_f32(acc + i, vaddq_f32(vld1q_f32(acc + i), vld1q_f32(row + i)));
#elif VIBE_WSPR_SSE2
    if (neg) for (; i + 4 <= n; i += 4) _mm_storeu_ps(acc + i, _mm_sub_ps(_mm_loadu_ps(acc + i), _mm_loadu_ps(row + i)));
    else     for (; i + 4 <= n; i += 4) _mm_storeu_ps(acc + i, _mm_add_ps(_mm_loadu_ps(acc + i), _mm_loadu_ps(row + i)));
#endif
    if (neg) for (; i < n; ++i) acc[i] -= row[i];
    else     for (; i < n; ++i) acc[i] += row[i];
}

/* The four tones' DFT bins of one 32-sample symbol (46.875 Hz, so bin t is exactly tone t), after mixing by w^n.
 *  yr/yi: the mixed samples. Tables: W_t[n] = e^{-j2π t n/32}, split real/imag, tone-major. */
struct ToneTables {
    alignas(16) float wr[4][32], wi[4][32];
    ToneTables() {
        for (int t = 0; t < 4; ++t) for (int n = 0; n < 32; ++n) {
            const double a = -2.0 * M_PI * t * n / 32.0;
            wr[t][n] = (float)std::cos(a); wi[t][n] = (float)std::sin(a);
        }
    }
};
const ToneTables& tt() { static const ToneTables t; return t; }

inline void toneBins(const float* yr, const float* yi, float P[4], float C[8]) {
    const ToneTables& T = tt();
#if VIBE_WSPR_NEON || VIBE_WSPR_SSE2
    for (int t = 0; t < 4; ++t) {
  #if VIBE_WSPR_NEON
        float32x4_t ar = vdupq_n_f32(0), ai = vdupq_n_f32(0);
        for (int n = 0; n < 32; n += 4) {
            const float32x4_t xr = vld1q_f32(yr + n), xi = vld1q_f32(yi + n);
            const float32x4_t cr = vld1q_f32(T.wr[t] + n), ci = vld1q_f32(T.wi[t] + n);
            ar = vmlaq_f32(ar, xr, cr); ar = vmlsq_f32(ar, xi, ci);
            ai = vmlaq_f32(ai, xr, ci); ai = vmlaq_f32(ai, xi, cr);
        }
        float r[4], im[4]; vst1q_f32(r, ar); vst1q_f32(im, ai);
  #else
        __m128 ar = _mm_setzero_ps(), ai = _mm_setzero_ps();
        for (int n = 0; n < 32; n += 4) {
            const __m128 xr = _mm_loadu_ps(yr + n), xi = _mm_loadu_ps(yi + n);
            const __m128 cr = _mm_load_ps(T.wr[t] + n), ci = _mm_load_ps(T.wi[t] + n);
            ar = _mm_add_ps(ar, _mm_sub_ps(_mm_mul_ps(xr, cr), _mm_mul_ps(xi, ci)));
            ai = _mm_add_ps(ai, _mm_add_ps(_mm_mul_ps(xr, ci), _mm_mul_ps(xi, cr)));
        }
        float r[4], im[4]; _mm_storeu_ps(r, ar); _mm_storeu_ps(im, ai);
  #endif
        const float sr = (r[0] + r[1]) + (r[2] + r[3]), si = (im[0] + im[1]) + (im[2] + im[3]);
        P[t] = sr * sr + si * si; C[2 * t] = sr; C[2 * t + 1] = si;
    }
#else
    for (int t = 0; t < 4; ++t) {
        float sr = 0, si = 0;
        for (int n = 0; n < 32; ++n) {
            sr += yr[n] * T.wr[t][n] - yi[n] * T.wi[t][n];
            si += yr[n] * T.wi[t][n] + yi[n] * T.wr[t][n];
        }
        P[t] = sr * sr + si * si; C[2 * t] = sr; C[2 * t + 1] = si;
    }
#endif
}

/** Blackman-windowed sinc, 2c+1 taps, cutoff fc (Hz) at rate fs, unity DC gain. */
std::vector<float> lowpass(int c, double fc, double fs) {
    std::vector<double> h((size_t)(2 * c + 1));
    double sum = 0;
    for (int k = -c; k <= c; ++k) {
        const double x = 2.0 * fc / fs * k;
        const double s = k == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double w = 0.42 + 0.5 * std::cos(M_PI * k / (c + 1)) + 0.08 * std::cos(2 * M_PI * k / (c + 1));
        h[(size_t)(k + c)] = s * w; sum += s * w;
    }
    std::vector<float> out(h.size());
    for (size_t i = 0; i < h.size(); ++i) out[i] = (float)(h[i] / sum);
    return out;
}

/** ln I0(x), x ≥ 0 (Abramowitz & Stegun 9.8.1 / 9.8.2). */
inline float lnI0(float x) {
    if (x < 3.75f) {
        const float t = (x / 3.75f) * (x / 3.75f);
        return std::log(1.f + t * (3.5156229f + t * (3.0899424f + t * (1.2067492f + t * (0.2659732f + t * (0.0360768f + t * 0.0045813f))))));
    }
    const float t = 3.75f / x;
    const float p = 0.39894228f + t * (0.01328592f + t * (0.00225319f + t * (-0.00157565f + t * (0.00916281f
                  + t * (-0.02057706f + t * (0.02635537f + t * (-0.01647633f + t * 0.00392377f)))))));
    return x - 0.5f * std::log(x) + std::log(p);
}

} // namespace

// ── WsprDecimator ───────────────────────────────────────────────────────────
WsprDecimator::WsprDecimator(int sampleRate) : rate_(sampleRate) {
    if (sampleRate < 1500 || sampleRate % 1500) return;
    d1_ = sampleRate / 1500;
    /* Stage 1: 1500 Hz audio → 0, low-pass 700 Hz, every d1-th sample. Blackman: transition ≈ 5.5·fs/N, so 64·fs/12k
     *  taps put the pass edge ≈ 190 Hz (the band needs ±125) and the stop edge ≈ 1210 Hz (aliases land from 1375). */
    c1_ = 32 * d1_ / 8;
    const std::vector<float> lp = lowpass(c1_, 700.0, sampleRate);
    h1r_.resize(lp.size()); h1i_.resize(lp.size());
    for (int k = -c1_; k <= c1_; ++k) {
        const double a = -2.0 * M_PI * wspr::kAudioCentre * k / sampleRate;
        h1r_[(size_t)(k + c1_)] = (float)(lp[(size_t)(k + c1_)] * std::cos(a));
        h1i_[(size_t)(k + c1_)] = (float)(lp[(size_t)(k + c1_)] * std::sin(a));
    }
    /* Stage 2: 1500 → 375 Hz complex, 81 taps cut at 187 Hz: pass ≈ ±136 Hz, stop ≥ 238 Hz (aliases from 250). */
    c2_ = 40;
    h2_ = lowpass(c2_, 187.0, 1500.0);
    reset();
}
void WsprDecimator::reset() {
    in_.assign((size_t)c1_, 0.f); inPos_ = 0;
    s2r_.assign((size_t)c2_, 0.f); s2i_.assign((size_t)c2_, 0.f); s2Pos_ = 0;
}
void WsprDecimator::push(const float* in, int n, std::vector<std::complex<float>>& out) {
    if (!ok() || n <= 0) return;
    in_.insert(in_.end(), in, in + n);
    const int L1 = 2 * c1_ + 1;
    while (inPos_ + (size_t)L1 <= in_.size()) {
        s2r_.push_back(dotf(in_.data() + inPos_, h1r_.data(), L1));
        s2i_.push_back(dotf(in_.data() + inPos_, h1i_.data(), L1));
        inPos_ += (size_t)d1_;
    }
    if (inPos_ > 16384) { in_.erase(in_.begin(), in_.begin() + (long)inPos_); inPos_ = 0; }
    stage2(out);
}
void WsprDecimator::stage2(std::vector<std::complex<float>>& out) {
    const int L2 = 2 * c2_ + 1;
    while (s2Pos_ + (size_t)L2 <= s2r_.size()) {
        out.emplace_back(dotf(s2r_.data() + s2Pos_, h2_.data(), L2), dotf(s2i_.data() + s2Pos_, h2_.data(), L2));
        s2Pos_ += 4;
    }
    if (s2Pos_ > 4096) {
        s2r_.erase(s2r_.begin(), s2r_.begin() + (long)s2Pos_);
        s2i_.erase(s2i_.begin(), s2i_.begin() + (long)s2Pos_);
        s2Pos_ = 0;
    }
}
void WsprDecimator::flush(std::vector<std::complex<float>>& out) {
    if (!ok()) return;
    std::vector<float> z((size_t)(c1_ + d1_ * (4 * c2_ + 8)), 0.f);
    push(z.data(), (int)z.size(), out);
}

// ── WsprSlotDecoder ─────────────────────────────────────────────────────────
namespace {
constexpr int    kFft = 512;
constexpr int    kHalf = 176;                  // bins either side of 0 kept: ±128.9 Hz
constexpr int    kPW = 2 * kHalf + 1;          // power row width
constexpr int    kQW = kPW - 6;                // sync-share row width (tone-0 bins)
constexpr double kBinHz = 375.0 / kFft;        // 0.732 Hz = half a tone
constexpr int    kBLo = 9, kBHi = 337;         // tone-0 bins searched: centre within ≈ ±120 Hz (1380…1620 Hz audio)
constexpr int    kMaxLag = 26;                 // frames (half symbols): starts from −2 s to +6.9 s after the minute (DT −3 … +5.9)
constexpr int    kNDrift = 17;                 // −4 … +4 Hz in 0.5 Hz
constexpr double kZRate = 375.0 / 8;           // 46.875 Hz, 32 samples a symbol
constexpr int    kZSps = 32;
constexpr float  kNoiseSigma = 5.69f;          // √(162 · 0.2): the score's spread on pure noise (Q ~ 2·Beta(2,2) − 1)
constexpr float  kCandSigma = 3.6f;            // candidates above this many σ
constexpr float  kMinExcess = 7.0f;            // a decode's tones must stand this many σ above the noise
constexpr float  kWeakSync = 0.20f;            // fit sync below this: noise-like (pure noise reaches ~0.19)
constexpr float  kStrongSync = 0.30f;          // above this: strong enough for the clipped third look
constexpr float  kStrongClip = 4.0f;           // |LLR| limit of that look
}

WsprSlotDecoder::WsprSlotDecoder() {
    fft_ = pffft_new_setup(kFft, PFFFT_COMPLEX);
    fftIn_ = (float*)pffft_aligned_malloc(sizeof(float) * 2 * kFft);
    fftOut_ = (float*)pffft_aligned_malloc(sizeof(float) * 2 * kFft);
    fftWork_ = (float*)pffft_aligned_malloc(sizeof(float) * 2 * kFft);
    /* 375 → 46.875 Hz after mixing the candidate to 0: 65 taps cut at 23 Hz — pass ≈ ±7.5 Hz (the tones ±2.2, drift
     *  ±2, error ±0.5), stop ≥ 39 Hz (aliases into ±6 come from 41). */
    c3_ = 32;
    h3_ = lowpass(c3_, 23.0, 375.0);
}
WsprSlotDecoder::~WsprSlotDecoder() {
    if (fft_) pffft_destroy_setup((PFFFT_Setup*)fft_);
    pffft_aligned_free(fftIn_); pffft_aligned_free(fftOut_); pffft_aligned_free(fftWork_);
}

/* ★★ THE SEARCH. Every frame (one symbol's samples every half symbol, zero-padded to 512 → bins half a tone apart)
 *  is reduced to ONE number per tone-0 bin: the sync pair's share of the four tones' power,
 *      Q = (P1 + P3 − P0 − P2) / (P0 + P1 + P2 + P3),
 *  which is independent of the start, the drift and the symbol it will be scored as. A hypothesis (bin, start, drift)
 *  then scores Σ_k ±Q[start + 2k][bin + shift_k(drift)] — signed by the sync bit — so the whole sub-band is searched
 *  as row additions (SIMD across bins). On pure noise Q is 2·Beta(2,2) − 1 whatever the noise level (σ² = 0.2), so
 *  the threshold is fixed in σ, not measured — a busy band cannot raise it. */
int WsprSlotDecoder::search(int pass, std::vector<Cand>& cands, int maxCands) {
    const std::vector<std::complex<float>>& x = *x_;
    const int N = (int)x.size();
    nFrames_ = (N - wspr::kSps375) / 128 + 1;
    q_.assign((size_t)nFrames_ * kQW, 0.f);
    if (pass == 1) { p_.assign((size_t)nFrames_ * kPW, 0.f); noise375_ = 0; }
    float P[kPW];
    std::memset(fftIn_, 0, sizeof(float) * 2 * kFft);
    for (int f = 0; f < nFrames_; ++f) {
        const std::complex<float>* s = x.data() + (size_t)f * 128;
        for (int i = 0; i < wspr::kSps375; ++i) { fftIn_[2 * i] = s[i].real(); fftIn_[2 * i + 1] = s[i].imag(); }
        pffft_transform_ordered((PFFFT_Setup*)fft_, fftIn_, fftOut_, fftWork_, PFFFT_FORWARD);
        for (int b = 0; b < kPW; ++b) {
            const int j = (b - kHalf + kFft) % kFft;
            P[b] = fftOut_[2 * j] * fftOut_[2 * j] + fftOut_[2 * j + 1] * fftOut_[2 * j + 1];
        }
        if (pass == 1) std::memcpy(p_.data() + (size_t)f * kPW, P, sizeof P);
        float* q = q_.data() + (size_t)f * kQW;
        for (int b = 0; b < kQW; ++b) {
            const float den = P[b] + P[b + 2] + P[b + 4] + P[b + 6];
            q[b] = den > 0 ? (P[b + 2] + P[b + 6] - P[b] - P[b + 4]) / den : 0.f;
        }
    }
    avgSpectrum();   // every pass: a pass-2 station is measured on the residual, its strong neighbour gone
    constexpr int W = kBHi - kBLo;                  // 328
    static_assert(W % 4 == 0, "row width");
    float best[W], acc[W];
    int   bestL[W]; float bestD[W];
    for (int b = 0; b < W; ++b) { best[b] = -1e9f; bestL[b] = 0; bestD[b] = 0; }
    const int maxLag = std::min(kMaxLag, nFrames_ - 1 - 2 * (wspr::kSymbols - 1));
    int shift[wspr::kSymbols];
    for (int di = 0; di < kNDrift; ++di) {
        const float D = -4.0f + 0.5f * di;
        for (int k = 0; k < wspr::kSymbols; ++k) shift[k] = (int)std::lround(D * (k - 80.5) / 161.0 / kBinHz);
        for (int L = 0; L <= maxLag; ++L) {
            std::memset(acc, 0, sizeof acc);
            for (int k = 0; k < wspr::kSymbols; ++k)
                addRow(acc, q_.data() + (size_t)(L + 2 * k) * kQW + kBLo + shift[k], W, !wspr::kSyncVector[k]);
            for (int b = 0; b < W; ++b) if (acc[b] > best[b]) { best[b] = acc[b]; bestL[b] = L; bestD[b] = D; }
        }
    }
    cands.clear();
    const float thr = kCandSigma * kNoiseSigma;
    for (int b = 0; b < W; ++b) {
        const float v = best[b];
        if (v < thr) continue;
        bool peak = true;
        for (int d = -2; d <= 2 && peak; ++d) {
            const int j = b + d;
            if (d == 0 || j < 0 || j >= W) continue;
            if (best[j] > v || (best[j] == v && j < b)) peak = false;
        }
        if (peak) cands.push_back({v, b + kBLo, bestL[b], bestD[b]});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    if ((int)cands.size() > maxCands) cands.resize((size_t)maxCands);
    return (int)cands.size();
}

/* ★★ THE NOISE FLOOR, for the SNR: the whole sub-band (±100 Hz) of pass 1's spectrogram, every other frame, full-bin
 *  steps (independent noise), its 30th percentile scaled to an exponential's mean (q30 = 0.357·mean) — the signals
 *  sit in the top of the distribution and barely move it (q5 … q30 agree within 0.7 dB on busy real 40 m). The band,
 *  not the tone bins beside the signal: those hold its own leakage, and on real 40 m read a −2 dB station as −19 dB.
 *  Per 375 Hz complex sample. Measured once per slot. */
double WsprSlotDecoder::bandNoise() {
    if (noise375_ > 0 || p_.empty() || nFrames_ <= 0) return noise375_;
    std::vector<float> pv;
    pv.reserve((size_t)(nFrames_ / 2 + 1) * 138);
    for (int f = 0; f < nFrames_; f += 2)
        for (int b = kHalf - 137; b <= kHalf + 137; b += 2) pv.push_back(p_[(size_t)f * kPW + (size_t)b]);
    const size_t qi = pv.size() * 3 / 10;
    std::nth_element(pv.begin(), pv.begin() + (long)qi, pv.end());
    noise375_ = pv[qi] / 0.35667 / wspr::kSps375;
    return noise375_;
}

/* ★★★ THE WSPRNET SNR SCALE (Stuart, 2026-10-27: "shift to wsprnet"). Spots are compared with other stations' reports,
 *  and those come from decoders that all read SNR the same way — not as the definition (signal power over the noise in
 *  2500 Hz, which `snrTrue` keeps) but from the SLOT-AVERAGED SPECTRUM: two-symbol (512-sample) sine-windowed frames
 *  every half symbol, their powers summed over the slot, smoothed by a 7-bin (5.1 Hz) moving sum that spans the four
 *  tones, and divided by the 30th percentile of that smoothed spectrum across ±150 Hz; the signal is the smoothed
 *  peak at its frequency less the noise (1), and 26.3 dB refers that ratio to 2500 Hz. Measured that way it carries
 *  what the definition does not: a neighbour's skirts inside the 5 Hz, and a busy band's raised percentile — which is
 *  why the gap to Stuart's UberSDR was 6.6 dB on 40 m but 3.7 dB on 20 m, and no constant could close both.
 *  Written from that description; computed at each pass on what that pass searches — the audio as received for pass
 *  1, the residual after subtraction for passes 2 and 3 (a weak station under a strong one otherwise reads the strong
 *  one's peak: real 40 m, PA5CA read −11 for the reference's −18). */
void WsprSlotDecoder::avgSpectrum() {
    const std::vector<std::complex<float>>& x = *x_;
    const int N = (int)x.size(), L = 2 * wspr::kSps375;   // 512
    if ((int)win512_.size() != L) {
        win512_.resize((size_t)L);
        for (int i = 0; i < L; ++i) win512_[(size_t)i] = (float)std::sin(M_PI * (i + 0.5) / L);
    }
    std::vector<double> acc((size_t)kFft, 0.0);
    for (int s0 = 0; s0 + L <= N; s0 += 128) {
        for (int i = 0; i < L; ++i) {
            const float w = win512_[(size_t)i];
            fftIn_[2 * i] = x[(size_t)(s0 + i)].real() * w; fftIn_[2 * i + 1] = x[(size_t)(s0 + i)].imag() * w;
        }
        pffft_transform_ordered((PFFFT_Setup*)fft_, fftIn_, fftOut_, fftWork_, PFFFT_FORWARD);
        for (int j = 0; j < kFft; ++j) acc[(size_t)j] += (double)fftOut_[2 * j] * fftOut_[2 * j] + (double)fftOut_[2 * j + 1] * fftOut_[2 * j + 1];
    }
    std::memset(fftIn_, 0, sizeof(float) * 2 * kFft);      // search() relies on the zero padding
    // centred at 0 Hz: index c ↔ bin (c − 205); 411 bins = ±150 Hz, each the 7-bin sum around it
    constexpr int kAvgHalf = 205, kAvgW = 2 * kAvgHalf + 1;
    avg_.assign((size_t)kAvgW, 0.f);
    for (int c = 0; c < kAvgW; ++c) {
        double sum = 0;
        for (int d = -3; d <= 3; ++d) sum += acc[(size_t)((c - kAvgHalf + d + kFft) % kFft)];
        avg_[(size_t)c] = (float)sum;
    }
    std::vector<float> srt = avg_;
    std::nth_element(srt.begin(), srt.begin() + kAvgW * 3 / 10, srt.end());
    const float noise = srt[(size_t)(kAvgW * 3 / 10)];
    for (float& v : avg_) v = noise > 0 ? v / noise - 1.f : 0.f;
}
double WsprSlotDecoder::snrWsprnet(double centreHz) const {
    if (avg_.empty()) return -99;
    const int c = (int)avg_.size() / 2 + (int)std::lround(centreHz / kBinHz);
    float pk = -1e9f;
    for (int d = -1; d <= 1; ++d) if (c + d >= 0 && c + d < (int)avg_.size()) pk = std::max(pk, avg_[(size_t)(c + d)]);
    const float floorV = 0.1f * std::pow(10.f, -0.8f);       // below −8 dB in the WSPR bandwidth the scale bottoms out
    return 10.0 * std::log10(std::max(pk, floorV)) - 26.3;
}

/** The candidate's centre mixed to 0 Hz and decimated to 46.875 Hz (32 samples a symbol), into zr_/zi_. */
void WsprSlotDecoder::mixDecimate(double fmix) {
    const std::vector<std::complex<float>>& x = *x_;
    const int N = (int)x.size();
    yr_.assign((size_t)(N + 2 * c3_ + 8), 0.f); yi_.assign(yr_.size(), 0.f);
    const double w = -2.0 * M_PI * fmix / wspr::kRate375;
    double cr = 1, ci = 0;
    const double wr = std::cos(w), wi = std::sin(w);
    for (int n = 0; n < N; ++n) {
        const float xr = x[(size_t)n].real(), xi = x[(size_t)n].imag();
        yr_[(size_t)(n + c3_)] = (float)(xr * cr - xi * ci);
        yi_[(size_t)(n + c3_)] = (float)(xr * ci + xi * cr);
        const double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
        if ((n & 1023) == 1023) { const double m = 1.0 / std::sqrt(cr * cr + ci * ci); cr *= m; ci *= m; }
    }
    const int nz = N / 8, L3 = 2 * c3_ + 1;
    zr_.resize((size_t)nz); zi_.resize((size_t)nz);
    for (int i = 0; i < nz; ++i) {
        zr_[(size_t)i] = dotf(yr_.data() + 8 * i, h3_.data(), L3);
        zi_[(size_t)i] = dotf(yi_.data() + 8 * i, h3_.data(), L3);
    }
    zMix_ = fmix;
}

/** Sync quality of a fit (0…1: the sync pair's excess share of the four tones' power); P = the 162 × 4 powers. */
float WsprSlotDecoder::evalFit(int t0, double df, float drift, float* P, float* C) {
    /* ★ The mixing phase runs ON across symbols. WSPR's tones are exactly one baud apart (h = 1), so a whole symbol
     *  of any tone advances the phase by a whole number of cycles: with a phase-continuous sender every symbol's tone
     *  bin then has the SAME phase, whatever the data — which is what the block soft bits below rely on. */
    const int nz = (int)zr_.size();
    alignas(16) float yr[kZSps], yi[kZSps];
    float sum = 0, tot = 0;
    double ph = 0;
    for (int k = 0; k < wspr::kSymbols; ++k) {
        const int st = t0 + kZSps * k;
        float p4[4] = {0, 0, 0, 0}, c8[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const double fk = -1.5 * wspr::kBaud + df + drift * (k - 80.5) / 161.0;
        if (st >= 0 && st + kZSps <= nz) {
            const float a = (float)(-2.0 * M_PI * fk / kZRate);
            const float wr = std::cos(a), wi = std::sin(a);
            float cr = (float)std::cos(-ph), ci = (float)std::sin(-ph);
            const float* zr = zr_.data() + st; const float* zi = zi_.data() + st;
            for (int n = 0; n < kZSps; ++n) {
                yr[n] = zr[n] * cr - zi[n] * ci;
                yi[n] = zr[n] * ci + zi[n] * cr;
                const float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
            toneBins(yr, yi, p4, c8);
        }
        ph = std::fmod(ph + 2.0 * M_PI * fk * kZSps / kZRate, 2.0 * M_PI);
        const int s = wspr::kSyncVector[k];
        const float a = p4[s] + p4[s + 2], o = p4[1 - s] + p4[3 - s];
        sum += a - o; tot += a + o;
        if (P) std::memcpy(P + 4 * k, p4, sizeof p4);
        if (C) std::memcpy(C + 8 * k, c8, sizeof c8);
    }
    return tot > 0 ? sum / tot : 0.f;
}

/** Rebuild a decoded signal (phase-continuous 4-FSK on the fitted track) and (`apply`) subtract it, its complex amplitude
 *  followed through the slot by a one-symbol moving average — fading, slow phase wander and a sender's phase steps
 *  are tracked; what is left is the residual the next pass searches. */
double WsprSlotDecoder::track(const Fit& f, const uint8_t* tones, bool apply) {
    const double noise = bandNoise();
    std::vector<std::complex<float>>& x = *x_;
    const int N = (int)x.size();
    const int s0 = 8 * f.t0, len = wspr::kSymbols * wspr::kSps375;
    std::vector<std::complex<float>> ref((size_t)len), p((size_t)len);
    double ph = 0;
    for (int k = 0; k < wspr::kSymbols; ++k) {
        const double fk = f.f0 + f.drift * (k - 80.5) / 161.0 + tones[k] * wspr::kBaud;
        const double dph = 2.0 * M_PI * fk / wspr::kRate375;
        for (int n = 0; n < wspr::kSps375; ++n) {
            ref[(size_t)(k * wspr::kSps375 + n)] = std::complex<float>((float)std::cos(ph), (float)std::sin(ph));
            ph += dph;
        }
        ph = std::fmod(ph, 2.0 * M_PI);
    }
    for (int n = 0; n < len; ++n) {
        const int i = s0 + n;
        p[(size_t)n] = (i >= 0 && i < N) ? x[(size_t)i] * std::conj(ref[(size_t)n]) : std::complex<float>(0, 0);
    }
    // centred moving average of 256 via a running sum (double, so 41 472 steps do not drift)
    const int H = wspr::kSps375 / 2;
    std::complex<double> run(0, 0);
    double pw = 0; int pn = 0, wsum = 0;
    int lo = 0, hi = 0;   // window [lo, hi)
    for (int n = 0; n < len; ++n) {
        const int wantHi = std::min(len, n + H), wantLo = std::max(0, n - H);
        while (hi < wantHi) { run += std::complex<double>(p[(size_t)hi]); ++hi; }
        while (lo < wantLo) { run -= std::complex<double>(p[(size_t)lo]); ++lo; }
        const std::complex<float> g((float)(run.real() / (hi - lo)), (float)(run.imag() / (hi - lo)));
        const int i = s0 + n;
        if (i >= 0 && i < N) {
            pw += std::norm(g); ++pn; wsum += hi - lo;
            if (apply) x[(size_t)i] -= g * ref[(size_t)n];
        }
    }
    if (!pn) return 0;
    // each g is a mean of (hi − lo) samples: its noise is the floor / (hi − lo)
    return std::max(0.0, pw / pn - noise * pn / std::max(1, wsum));
}

bool WsprSlotDecoder::refineAndDecode(const Cand& c, const WsprSlotOptions& o, double dialHz, double slotUtc, int pass,
                                      std::vector<WsprSpot>& out, WsprSlotStats& st,
                                      std::vector<std::pair<Fit, std::vector<uint8_t>>>& decoded) {
    using clk = std::chrono::steady_clock;
    const auto t0c = clk::now();
    const double f0c = (c.bin - kHalf) * kBinHz;                 // tone 0, Hz from 1500
    mixDecimate(f0c + 1.5 * wspr::kBaud);
    /* Coordinate refinement: start (±1 frame in 1/16 symbol, then ±2/32), frequency (±0.5 Hz in 0.1, then ±0.05 in
     *  0.025), drift (±0.5 Hz in 0.25). ~40 fits of 162 × 4 32-point DFT bins. */
    int bt = c.lag * 16; double bf = 0; float bd = c.drift;
    float bm = evalFit(bt, bf, bd, nullptr, nullptr);
    auto tryT = [&](int lo, int hi, int step) { const int base = bt; for (int d = lo; d <= hi; d += step) { if (!d) continue;
        const float m = evalFit(base + d, bf, bd, nullptr, nullptr); if (m > bm) { bm = m; bt = base + d; } } };
    auto tryF = [&](double lo, double hi, double step) { const double base = bf; for (double d = lo; d <= hi + 1e-9; d += step) {
        if (std::fabs(d) < 1e-9) continue; const float m = evalFit(bt, base + d, bd, nullptr, nullptr); if (m > bm) { bm = m; bf = base + d; } } };
    auto tryD = [&](float lo, float hi, float step) { const float base = bd; for (float d = lo; d <= hi + 1e-6f; d += step) {
        if (std::fabs(d) < 1e-6f) continue; const float m = evalFit(bt, bf, base + d, nullptr, nullptr); if (m > bm) { bm = m; bd = base + d; } } };
    tryT(-16, 16, 2);
    tryF(-0.5, 0.5, 0.1);
    tryD(-0.5f, 0.5f, 0.25f);
    tryT(-2, 2, 1);
    tryF(-0.05, 0.05, 0.025);
    float P[wspr::kSymbols * 4], C[wspr::kSymbols * 8];
    bm = evalFit(bt, bf, bd, P, C);
    st.refineMs += std::chrono::duration<double, std::milli>(clk::now() - t0c).count();

    /* ★★ CALIBRATED SOFT BITS. Per symbol the sync bit names the pair {s, s+2}; the data bit is which of the two holds
     *  the tone. Non-coherent FSK: LLR = ln I0(2A·|r1|/N) − ln I0(2A·|r0|/N), with the noise N per tone bin measured
     *  from the pair that never carries the signal, and the signal A² from the pair that does, less its noise. */
    double nsum = 0, ssum = 0;
    for (int k = 0; k < wspr::kSymbols; ++k) {
        const int s = wspr::kSyncVector[k];
        nsum += P[4 * k + 1 - s] + P[4 * k + 3 - s];
        ssum += P[4 * k + s] + P[4 * k + s + 2];
    }
    const double Nn = std::max(1e-30, nsum / (2.0 * wspr::kSymbols));
    const double A2 = std::max(0.02 * Nn, ssum / wspr::kSymbols - 2.0 * Nn);
    const double A = std::sqrt(A2);
    float llrChan[wspr::kSymbols], llr[wspr::kSymbols];
    const auto t1c = clk::now();
    uint8_t bits[7];
    bool okF = false;
    /* ★★ UP TO THREE LOOKS, CHEAPEST FIRST. (1) one symbol at a time; (2) block soft bits (below) — about a decibel
     *  on a steady, phase-continuous path; (3) for a candidate whose sync says it is STRONG, the soft bits clipped at
     *  ±kStrongClip: a strong signal's LLRs are enormous, so one symbol hit by an overlapping station or a model
     *  mismatch (a wobbling sender, a fast drift) is a confident wrong bit the tree search cannot climb past — clipped,
     *  the code corrects it (real 40 m, 2026-10-26: a −3 dB station under another at 0.6 Hz decoded only this way).
     *  ★ A candidate whose fit barely beats noise gets a quarter of the cycle budget and one look: on pure noise
     *  every candidate is that, and the Fano search is ~90 % of the CPU. */
    const int nTries = bm >= kStrongSync ? 3 : bm >= kWeakSync ? 2 : 1;
    const long budget = o.fanoCyclesPerBit * wspr::kCodedBits / (bm >= kWeakSync ? 1 : 4);
    for (int attempt = 0; attempt < nTries && !okF; ++attempt) {
        const int nb = attempt == 1 ? 3 : 1;
        const float clip = attempt == 2 ? kStrongClip : 0.f;
        if (nb == 1) {
            for (int k = 0; k < wspr::kSymbols; ++k) {
                const int s = wspr::kSyncVector[k];
                const float a0 = std::sqrt(P[4 * k + s]), a1 = std::sqrt(P[4 * k + s + 2]);
                llrChan[k] = o.llrScale * (lnI0((float)(2 * A * a1 / Nn)) - lnI0((float)(2 * A * a0 / Nn)));
            }
        } else {
            /* ★★ BLOCK SOFT BITS: the symbol's tone bin summed COHERENTLY with its neighbours' (h = 1, so a
             *  phase-continuous signal keeps one phase), the neighbours' unknown data bits maximised out — each of the
             *  2^nb sums is a longer, cleaner look at the same phase. On a sender that steps its phase each symbol it
             *  is worse, which is why it is the second look, not the only one. */
            const int h = nb / 2;
            for (int k = 0; k < wspr::kSymbols; ++k) {
                const int lo = std::max(0, k - h), hi = std::min(wspr::kSymbols - 1, k - h + nb - 1);
                const int m = hi - lo + 1;
                float best[2] = {0, 0};
                for (int combo = 0; combo < (1 << m); ++combo) {
                    float sr = 0, si = 0;
                    int dk = 0;
                    for (int j = lo; j <= hi; ++j) {
                        const int d = (combo >> (j - lo)) & 1;
                        if (j == k) dk = d;
                        const int t = wspr::kSyncVector[j] + 2 * d;
                        sr += C[8 * j + 2 * t]; si += C[8 * j + 2 * t + 1];
                    }
                    const float mag = std::sqrt(sr * sr + si * si);
                    if (mag > best[dk]) best[dk] = mag;
                }
                // the sum of m bins carries amplitude m·A in noise m·N: the I0 argument is still 2A·|S|/N
                const double sc = 2 * A / Nn;
                llrChan[k] = o.llrScale * (lnI0((float)(sc * best[1])) - lnI0((float)(sc * best[0])));
            }
        }
        for (int p = 0; p < wspr::kSymbols; ++p) {
            llr[p] = llrChan[wspr::interleavePos(p)];
            if (clip > 0) llr[p] = std::max(-clip, std::min(clip, llr[p]));
        }
        long cyc = 0;
        okF = wspr::fanoDecode(llr, budget, o.fanoDelta, bits, &cyc, o.abort);
        st.fanoCycles += cyc;
    }
    st.fanoMs += std::chrono::duration<double, std::milli>(clk::now() - t1c).count();
    if (!okF) { st.fanoFails++; return false; }
    wspr::Message msg;
    if (!wspr::unpack(bits, msg, &wspr::callHashes())) return false;

    // ★ The decoded tones must stand clear of the noise across the whole transmission (a sequential decoder will
    //   always find SOME path; a false one leaves its tones near the noise).
    uint8_t tones[wspr::kSymbols];
    wspr::encodeBits(bits, tones);
    double sigP = 0, othP = 0;
    for (int k = 0; k < wspr::kSymbols; ++k) {
        const float* p4 = P + 4 * k;
        sigP += p4[tones[k]];
        othP += (double)p4[0] + p4[1] + p4[2] + p4[3] - p4[tones[k]];
    }
    const double N3 = std::max(1e-30, othP / (3.0 * wspr::kSymbols));
    const double excess = (sigP / N3 - wspr::kSymbols) / std::sqrt((double)wspr::kSymbols);
    if (excess < kMinExcess) return false;

    WsprSpot sp;
    sp.slotUtc = slotUtc;
    sp.audioHz = wspr::kAudioCentre + zMix_ + bf;
    sp.freqHz = dialHz + sp.audioHz;
    /* Two SNRs. `snrTrue`, the definition: the signal's mean power (its amplitude followed through the slot) over the
     *  band's noise per 375 Hz sample, × 375/2500 — exact on test-wspr's synthetic slots. `snr`, the WSPRNET scale the
     *  spots carry (avgSpectrum above) — what other receivers' reports of the same station read. */
    {
        Fit tf; tf.f0 = zMix_ + bf - 1.5 * wspr::kBaud; tf.t0 = bt; tf.drift = bd;
        const double ps = track(tf, tones, false);
        const double nl = bandNoise();
        const double snr = nl > 0 ? 10.0 * std::log10(std::max(ps, 1e-6 * nl) / nl * 375.0 / 2500.0) : -99;
        sp.snrTrue = (float)std::max(-40.0, std::min(30.0, snr));
        sp.snr = (int)std::lround(std::max(-40.0, std::min(30.0, snrWsprnet(zMix_ + bf))));
    }
    sp.dt = (float)(bt / kZRate - wspr::kLeadSec - wspr::kStartSec);
    sp.drift = bd;
    sp.type = msg.type; sp.dbm = msg.dbm; sp.grid = msg.grid; sp.pass = pass; sp.sync = bm;
    if (msg.type == 3 && msg.call.empty()) return false;          // an unresolved hash is not a spot
    sp.call = msg.call;
    sp.message = msg.text();
    if (msg.type != 3) wspr::callHashes().save(msg.call);
    Fit f; f.f0 = zMix_ + bf - 1.5 * wspr::kBaud; f.t0 = bt; f.drift = bd; f.metric = bm;
    for (const WsprSpot& e : out) if (e.message == sp.message) return true;   // already have it (an alias, pass 2)
    out.push_back(sp);
    decoded.emplace_back(f, std::vector<uint8_t>(tones, tones + wspr::kSymbols));
    st.decodes[pass - 1]++;
    return true;
}

void WsprSlotDecoder::decode(std::vector<std::complex<float>>& x, double dialHz, double slotUtc,
                             const WsprSlotOptions& o, std::vector<WsprSpot>& out, WsprSlotStats& st) {
    using clk = std::chrono::steady_clock;
    const auto tStart = clk::now();
    x.resize(kSlotSamples, std::complex<float>(0, 0));
    x_ = &x;
    std::vector<std::pair<Fit, std::vector<uint8_t>>> decoded;
    size_t subtracted = 0;
    const int maxPasses = std::max(1, std::min(3, o.maxPasses));
    double lastPassMs = 0;
    for (int pass = 1; pass <= maxPasses; ++pass) {
        if (o.abort && o.abort->load()) break;
        const auto tp = clk::now();
        if (pass > 1) {
            if (decoded.size() == subtracted) break;               // nothing new to take away: the residual is the same
            const double el = std::chrono::duration<double, std::milli>(tp - tStart).count();
            if (o.mayRunPass && !o.mayRunPass(pass, el, lastPassMs)) break;
            const auto ts = clk::now();
            for (; subtracted < decoded.size(); ++subtracted) track(decoded[subtracted].first, decoded[subtracted].second.data(), true);
            st.subtractMs += std::chrono::duration<double, std::milli>(clk::now() - ts).count();
        }
        std::vector<Cand> cands;
        const auto ts = clk::now();
        st.candidates[pass - 1] = search(pass, cands, o.maxCandidates);
        st.searchMs += std::chrono::duration<double, std::milli>(clk::now() - ts).count();
        for (const Cand& c : cands) {
            if (o.abort && o.abort->load()) break;
            /* ★ A candidate on top of a signal already decoded is its residual; one TWO TONES (2.93 Hz) either side of
             *  it is its alias — the sync pair is {s, s+2}, so a two-tone shift scores almost as well — and on a busy
             *  band those aliases were most of the failed Fano searches (most of the CPU). Pass 1 skips both; a real
             *  station sitting there is found by pass 2, once the strong one has been subtracted. */
            const double fc = (c.bin - kHalf) * kBinHz;
            bool near = false;
            for (const auto& d : decoded) {
                const double df = std::fabs(d.first.f0 - fc);
                if (df < 1.0 || (pass == 1 && std::fabs(df - 2 * wspr::kBaud) < 0.6)) { near = true; break; }
            }
            if (near) continue;
            refineAndDecode(c, o, dialHz, slotUtc, pass, out, st, decoded);
        }
        lastPassMs = std::chrono::duration<double, std::milli>(clk::now() - tp).count();
        st.passMs[pass - 1] = lastPassMs;
        st.passes = pass;
    }
    x_ = nullptr;
}

void WsprSlotDecoder::decodeAudio(const float* audio, int n, int sampleRate, double dialHz, double slotUtc,
                                  const WsprSlotOptions& o, std::vector<WsprSpot>& out, WsprSlotStats& st) {
    const auto t0 = std::chrono::steady_clock::now();
    WsprDecimator d(sampleRate);
    if (!d.ok()) return;
    std::vector<std::complex<float>> x;
    x.reserve(kSlotSamples + 64);
    d.push(audio, n, x);
    d.flush(x);
    if ((int)x.size() > kSlotSamples) x.resize(kSlotSamples);
    st.decimateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    decode(x, dialHz, slotUtc, o, out, st);
}

// ── WsprDecoder (live) ──────────────────────────────────────────────────────
namespace { constexpr int kLiveWindow = WsprSlotDecoder::kSlotSamples; }   // 120 s from 2 s before the minute

WsprDecoder::WsprDecoder(int sampleRate) : rate_(sampleRate), dec_(sampleRate) {
    ring_.assign((size_t)(2 * WsprSlotDecoder::kSlotSamples + 4096), std::complex<float>(0, 0));
    work_.reserve(WsprSlotDecoder::kSlotSamples);
    if (dec_.ok()) worker_ = std::thread([this] { workerLoop(); });
}
WsprDecoder::~WsprDecoder() {
    abort_.store(true);
    { std::lock_guard<std::mutex> lk(wm_); stop_ = true; }
    wcv_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void WsprDecoder::workerLoop() {
    vibe::vibeFlushDenormals();
    std::unique_lock<std::mutex> lk(wm_);
    for (;;) {
        wcv_.wait(lk, [this] { return stop_ || pending_; });
        if (stop_) return;
        pending_ = false; busy_ = true;
        lk.unlock();
        runDecode();
        lk.lock();
        busy_ = false;
    }
}
void WsprDecoder::process(const int16_t* in, int count, double captureUtc) {
    if (!dec_.ok() || count <= 0) return;
    fbuf_.resize((size_t)count);
    for (int i = 0; i < count; ++i) fbuf_[(size_t)i] = in[i] * (1.0f / 32768.0f);
    process(fbuf_.data(), count, captureUtc);
}
void WsprDecoder::process(const float* in, int count, double captureUtc) {
    if (!dec_.ok() || count <= 0) return;
    if (!std::isfinite(captureUtc)) captureUtc = vibe::vibeUtcNow();
    tmp_.clear();
    dec_.push(in, count, tmp_);
    inTotal_ += count;
    const int64_t R = (int64_t)ring_.size();
    for (const auto& v : tmp_) ring_[(size_t)(total_++ % R)] = v;

    /* ★★ THE ANCHOR (FT8's rule): the capture UTC of input sample 0. A stamp is late by however long the audio took
     *  to arrive, never early, so an earlier estimate is taken at once and a later one leaks in over 30 s; a second
     *  later means audio was LOST — re-anchor, and never cut a slot across the gap. The decimator's filters are
     *  centred, so 375 Hz sample m is input sample m·rate/375 — the same clock. */
    const double est = captureUtc - (double)inTotal_ / rate_;
    if (!anchored_ || est - utc0_ > 1.0) {
        if (anchored_) reanchors.fetch_add(1);
        utc0_ = est; anchored_ = true; nextSlot_ = NAN;
        floor_ = total_;
    } else if (est < utc0_) utc0_ = est;
    else utc0_ += (est - utc0_) * std::min(1.0, (double)count / rate_ / 30.0);

    const int64_t oldest = std::max<int64_t>(floor_, total_ - R);
    if (std::isnan(nextSlot_)) nextSlot_ = std::ceil((utc0_ + (double)oldest / wspr::kRate375 + wspr::kLeadSec) / wspr::kSlotSec) * wspr::kSlotSec;
    for (;;) {
        const int64_t i0 = (int64_t)std::llround((nextSlot_ - wspr::kLeadSec - utc0_) * wspr::kRate375);
        if (i0 < oldest) { nextSlot_ += wspr::kSlotSec; continue; }
        if (total_ < i0 + kLiveWindow) break;
        {
            std::lock_guard<std::mutex> lk(wm_);
            if (!busy_ && !pending_) {
                work_.resize((size_t)kLiveWindow);
                for (int i = 0; i < kLiveWindow; ++i) work_[(size_t)i] = ring_[(size_t)((i0 + i) % R)];
                workSlotUtc_ = nextSlot_;
                workDial_ = dialHz.load();
                pending_ = true;
                wcv_.notify_one();
            } else {
                slotsSkipped.fetch_add(1);      // ★ still decoding the last one: skip, never wait
            }
        }
        nextSlot_ += wspr::kSlotSec;
    }
}
void WsprDecoder::runDecode() {
    const auto t0 = std::chrono::steady_clock::now();
    WsprSlotOptions o;
    o.maxPasses = std::max(1, std::min(3, maxPasses.load()));
    o.fanoCyclesPerBit = std::max(1000L, fanoCyclesPerBit.load());
    o.abort = &abort_;
    o.mayRunPass = [&](int, double elapsedMs, double lastMs) {
        return passAllowed(wspr::kSlotSec * 1000.0, elapsedMs, lastMs, loadProbe ? loadProbe() : false);
    };
    std::vector<WsprSpot> res;
    WsprSlotStats st;
    slot_.decode(work_, workDial_, workSlotUtc_, o, res, st);
    lastPasses.store(st.passes);
    lastDecodeMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    slotsDecoded.fetch_add(1);
    if (abort_.load()) return;
    if (onSpot) for (const WsprSpot& s : res) onSpot(s);
}

} // namespace vibe
