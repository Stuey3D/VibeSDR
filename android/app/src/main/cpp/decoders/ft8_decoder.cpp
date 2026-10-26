// VibeSDR V4 — FT8 / FT4 decoder wrapper around ft8_lib (MIT).
#include "ft8_decoder.h"
#include "../vibe_clock.h"
#include "../vibedsp/vibe_denormals.h"
#include "ft8/vibe_simd4.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include "ft8/decode.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "ft8/constants.h"
#include "ft8/crc.h"
#include "ft8/ldpc.h"
}

namespace vibe {

// ── The hashed-callsign table (see Ft8CallHashTable in the header) ───────────
void Ft8CallHashTable::save(const char* call, uint32_t n22) {
    if (!call || !call[0]) return;
    std::lock_guard<std::mutex> lk(mtx_);
    const uint64_t now = ++clock_;
    int freeAt = -1, oldest = -1;
    for (int i = 0; i < kCapacity; i++) {
        Entry& x = e_[i];
        if (!x.stamp) { if (freeAt < 0) freeAt = i; continue; }
        if (x.n22 == n22 && std::strncmp(x.call, call, 11) == 0) { x.stamp = now; return; }   // heard again
        if (oldest < 0 || x.stamp < e_[oldest].stamp) oldest = i;
    }
    Entry& x = e_[freeAt >= 0 ? freeAt : oldest];   // ★ full: the least recently used makes way
    std::strncpy(x.call, call, 11); x.call[11] = '\0';
    x.n22 = n22; x.stamp = now;
}
bool Ft8CallHashTable::lookup(int bits, uint32_t hash, char* out) {
    out[0] = '\0';
    const int shift = bits == 10 ? 12 : bits == 12 ? 10 : 0;
    std::lock_guard<std::mutex> lk(mtx_);
    int hit = -1;
    for (int i = 0; i < kCapacity; i++) {
        const Entry& x = e_[i];
        if (!x.stamp || (x.n22 >> shift) != hash) continue;
        // ★ Two DIFFERENT calls under one hash: either could be the sender, so neither is printed.
        if (hit >= 0 && std::strncmp(e_[hit].call, x.call, 11) != 0) return false;
        hit = i;
    }
    if (hit < 0) return false;
    e_[hit].stamp = ++clock_;
    /* ★ TERMINATED (audit 2026-10-03): strncpy of 11 leaves no NUL when the stored call is 11 long, and ft8_lib's
     *  caller strlen()s it. Its buffer is char[12] (message.c). */
    std::strncpy(out, e_[hit].call, 11);
    out[11] = '\0';
    return true;
}
void Ft8CallHashTable::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (Entry& x : e_) x = Entry{};
    clock_ = 0;
}
int Ft8CallHashTable::size() {
    std::lock_guard<std::mutex> lk(mtx_);
    int n = 0; for (const Entry& x : e_) n += x.stamp != 0; return n;
}
Ft8CallHashTable& ft8CallHashes() { static Ft8CallHashTable t; return t; }

bool Ft8Decoder::spotCallsign(const char* de, std::string& out) {
    out.clear();
    if (!de || !de[0]) return false;
    std::string s(de);
    if (s.front() == '<') {                       // a hashed call: "<...>" unknown, "<CALL>" resolved
        if (s.size() < 3 || s.back() != '>') return false;
        s = s.substr(1, s.size() - 2);
        if (s == "...") return false;
    }
    if (s.find_first_of("<>") != std::string::npos) return false;
    bool alnum = false;
    for (char ch : s) alnum |= (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    if (!alnum) return false;                     // nothing the host's whitelist would keep
    out = s;
    return true;
}

namespace {
// ★ Shared by every decoder, and decodes run on worker threads (FT8 and FT4 can overlap) — the table locks itself.
bool ht_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    const int bits = type == FTX_CALLSIGN_HASH_10_BITS ? 10 : type == FTX_CALLSIGN_HASH_12_BITS ? 12 : 22;
    return ft8CallHashes().lookup(bits, hash, callsign);
}
void ht_save(const char* callsign, uint32_t n22) { ft8CallHashes().save(callsign, n22); }
ftx_callsign_hash_interface_t g_hashIf = { ht_lookup, ht_save };

constexpr int kFreqOsr = 2;
constexpr int kTimeOsr = 2;
constexpr double kTwoPi = 6.283185307179586;

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/* ★ A 4096-entry sine table for the subtraction's reference: 12 bits of phase is a 0.08 % amplitude error,
 *  -62 dB — far below anything the subtraction can take out — and a lookup instead of a sincosf per sample,
 *  which on a Pi 2 is the difference between 2 ms and 15 ms per decoded signal. */
struct SinTable {
    float s[4096], c[4096];
    SinTable() { for (int i = 0; i < 4096; ++i) { s[i] = (float)std::sin(kTwoPi * i / 4096); c[i] = (float)std::cos(kTwoPi * i / 4096); } }
};
const SinTable& sinTable() { static SinTable t; return t; }

/* ★★ ORDERED-STATISTICS DECODING, ORDER 1 (2026-10-05) — the "light OSD" of the decoder audit. When belief
 *  propagation gives up, take the 91 most reliable bits that are independent (Gaussian elimination on the
 *  generator, columns in order of |LLR|), re-encode from their hard decisions, and try every single flip among
 *  them: 92 codewords, each a valid LDPC codeword, the closest (least |LLR| disagreeing) one that passes the CRC
 *  wins. A codeword is only believed if it disagrees with the received hard bits in at most `maxErrors` places —
 *  the guard against the CRC-14 waving through a random codeword (WSJT-X guards its OSD the same way). */
struct Bits174 { uint64_t w[3]; };
inline void setBit(Bits174& b, int i) { b.w[i >> 6] |= 1ull << (i & 63); }
inline bool getBit(const Bits174& b, int i) { return (b.w[i >> 6] >> (i & 63)) & 1; }
const Bits174* generatorRows() {               // row k = the codeword of message bit k alone
    static Bits174 g[FTX_LDPC_K];
    static bool init = [] {
        for (int k = 0; k < FTX_LDPC_K; ++k) {
            g[k] = Bits174{{0, 0, 0}};
            setBit(g[k], k);
            for (int m = 0; m < FTX_LDPC_M; ++m)
                if (kFTX_LDPC_generator[m][k / 8] & (0x80u >> (k % 8))) setBit(g[k], FTX_LDPC_K + m);
        }
        return true;
    }();
    (void)init;
    return g;
}
/** OSD-1 on `llr` (log p(1)/p(0), codeword order). On success fills `a91` (packed message + CRC) and returns the
 *  number of hard-decision disagreements; -1 when no codeword passes the CRC within `maxErrors`.
 *  ★★ IN CODEWORD ORDER (2026-10-25, the FT8 CPU work). The first version copied the generator into a matrix with its
 *  columns permuted into reliability order, bit by bit (91 × 174 bit moves per call), then eliminated it. The rows
 *  that elimination ends with are unique — each is the codeword that is 1 at one pivot and 0 at the others — so it
 *  makes no difference whether the columns are moved first: this eliminates the generator's own rows, visiting the
 *  columns in reliability order. Each flip is judged by a popcount first (most are far over maxErrors and stop
 *  there); only a survivor's distance is summed, in the same reliability order as before, so it rounds the same.
 *  The result is the same codeword, the same error count, for every input (test-ft8-fast). */
} // namespace
int ft8Osd1(const float* llr, int maxErrors, uint8_t a91[FTX_LDPC_K_BYTES]) { return ft8Osd(llr, maxErrors, 0, a91); }

/* ★★ ORDER 2, PARTIALLY (2026-10-26, the sensitivity work). After every single flip, every PAIR of flips among the
 *  `pairSpan` least reliable of the 91 independent bits (C(span,2) more codewords: 435 at 30). A near miss's errors
 *  among the most reliable bits are usually one or two of the least reliable of them — so the pairs are where the
 *  second error hides, and the cost is one XOR and a popcount each. The same guards (CRC-14, at most maxErrors hard
 *  disagreements) judge every codeword; test-ft8-fast and ftx_sweep --noise count what that lets through. */
int ft8Osd(const float* llr, int maxErrors, int pairSpan, uint8_t a91[FTX_LDPC_K_BYTES]) {
    const Bits174* g = generatorRows();
    float wAbs[FTX_LDPC_N];
    for (int i = 0; i < FTX_LDPC_N; ++i) wAbs[i] = std::fabs(llr[i]);
    int perm[FTX_LDPC_N];
    for (int i = 0; i < FTX_LDPC_N; ++i) perm[i] = i;
    std::sort(perm, perm + FTX_LDPC_N, [&](int a, int b) { return wAbs[a] > wAbs[b]; });
    int pos[FTX_LDPC_N];
    for (int p = 0; p < FTX_LDPC_N; ++p) pos[perm[p]] = p;
    /* ★ ELIMINATED IN 32-BIT WORDS (2026-10-25). This loop is most of OSD: 91 pivots × 91 rows, a bit test each and
     *  a row XOR for about half. On a 32-bit ARM each 64-bit test or XOR is two of everything, so the rows are held
     *  here as six 32-bit words (padded to eight, so a row is two 128-bit vectors) — a one-word test, and the XOR as
     *  one 128-bit and one 64-bit operation. The same bits, the same pivots, the same rows. */
    struct Row32 { uint32_t w[8]; };
    alignas(16) Row32 M32[FTX_LDPC_K];
    for (int k = 0; k < FTX_LDPC_K; ++k) {
        for (int x = 0; x < 3; ++x) { M32[k].w[2 * x] = (uint32_t)g[k].w[x]; M32[k].w[2 * x + 1] = (uint32_t)(g[k].w[x] >> 32); }
        M32[k].w[6] = M32[k].w[7] = 0;
    }
    int pivot[FTX_LDPC_K]; int r = 0;          // pivot[q] = the CODEWORD position of row q's pivot
    for (int p = 0; p < FTX_LDPC_N && r < FTX_LDPC_K; ++p) {
        const int col = perm[p];
        const int wi = col >> 5; const uint32_t bit = 1u << (col & 31);
        int i = r;
        while (i < FTX_LDPC_K && !(M32[i].w[wi] & bit)) ++i;
        if (i == FTX_LDPC_K) continue;
        std::swap(M32[i], M32[r]);
        const Row32 R = M32[r];
#if VIBE_FT8_NEON
        const uint32x4_t R0 = vld1q_u32(R.w), R1 = vld1q_u32(R.w + 4);
        for (int q = 0; q < FTX_LDPC_K; ++q)
            if (q != r && (M32[q].w[wi] & bit)) {
                vst1q_u32(M32[q].w, veorq_u32(vld1q_u32(M32[q].w), R0));
                vst1q_u32(M32[q].w + 4, veorq_u32(vld1q_u32(M32[q].w + 4), R1));
            }
#elif VIBE_FT8_SSE2
        const __m128i R0 = _mm_load_si128((const __m128i*)R.w), R1 = _mm_load_si128((const __m128i*)(R.w + 4));
        for (int q = 0; q < FTX_LDPC_K; ++q)
            if (q != r && (M32[q].w[wi] & bit)) {
                _mm_store_si128((__m128i*)M32[q].w, _mm_xor_si128(_mm_load_si128((const __m128i*)M32[q].w), R0));
                _mm_store_si128((__m128i*)(M32[q].w + 4), _mm_xor_si128(_mm_load_si128((const __m128i*)(M32[q].w + 4)), R1));
            }
#else
        for (int q = 0; q < FTX_LDPC_K; ++q)
            if (q != r && (M32[q].w[wi] & bit))
                for (int x = 0; x < 6; ++x) M32[q].w[x] ^= R.w[x];
#endif
        pivot[r++] = col;
    }
    if (r < FTX_LDPC_K) return -1;
    Bits174 M[FTX_LDPC_K];
    for (int k = 0; k < FTX_LDPC_K; ++k)
        for (int x = 0; x < 3; ++x) M[k].w[x] = (uint64_t)M32[k].w[2 * x] | ((uint64_t)M32[k].w[2 * x + 1] << 32);
    Bits174 hard{{0, 0, 0}};
    for (int i = 0; i < FTX_LDPC_N; ++i) if (llr[i] > 0) setBit(hard, i);
    Bits174 c0{{0, 0, 0}};
    for (int q = 0; q < FTX_LDPC_K; ++q)
        if (getBit(hard, pivot[q])) { c0.w[0] ^= M[q].w[0]; c0.w[1] ^= M[q].w[1]; c0.w[2] ^= M[q].w[2]; }
    float bestD = 1e30f; int bestErr = -1;
    const int span = std::max(0, std::min(pairSpan, FTX_LDPC_K));
    const int nPairs = span * (span - 1) / 2;
    for (int idx = -1; idx < FTX_LDPC_K + nPairs; ++idx) {
        Bits174 c = c0;
        if (idx >= 0 && idx < FTX_LDPC_K) { c.w[0] ^= M[idx].w[0]; c.w[1] ^= M[idx].w[1]; c.w[2] ^= M[idx].w[2]; }
        else if (idx >= FTX_LDPC_K) {
            // pair number p among the last `span` rows (the least reliable pivots): (a, b), a < b
            int p = idx - FTX_LDPC_K, a = 0;
            while (p >= span - 1 - a) { p -= span - 1 - a; ++a; }
            const int ra = FTX_LDPC_K - span + a, rb = ra + 1 + p;
            c.w[0] ^= M[ra].w[0] ^ M[rb].w[0]; c.w[1] ^= M[ra].w[1] ^ M[rb].w[1]; c.w[2] ^= M[ra].w[2] ^ M[rb].w[2];
        }
        const uint64_t d0 = c.w[0] ^ hard.w[0], d1 = c.w[1] ^ hard.w[1], d2 = c.w[2] ^ hard.w[2];
        const int err = __builtin_popcountll(d0) + __builtin_popcountll(d1) + __builtin_popcountll(d2);
        if (err > maxErrors) continue;
        // ★ the distance summed in reliability order (ascending pos), as the permuted version summed it
        int ps[FTX_LDPC_N]; int m = 0;
        for (int x = 0; x < 3; ++x) {
            uint64_t diff = x == 0 ? d0 : x == 1 ? d1 : d2;
            while (diff) { ps[m++] = pos[x * 64 + __builtin_ctzll(diff)]; diff &= diff - 1; }
        }
        std::sort(ps, ps + m);
        float d = 0;
        for (int j = 0; j < m; ++j) d += wAbs[perm[ps[j]]];
        if (d >= bestD) continue;
        /* ★ Never the all-zero codeword (2026-10-26): its CRC is zero, so it always "passes", and a carrier or a
         *  strong station's residue makes a run of zero hard decisions — on the real FT4 hour OSD-1 returned it 27
         *  times (FT4's scrambling turns it into an empty message). Belief propagation refuses it the same way. */
        if (!(c.w[0] | c.w[1] | c.w[2])) continue;
        uint8_t m91[FTX_LDPC_K_BYTES] = {};
        for (int k = 0; k < FTX_LDPC_K; ++k) if (getBit(c, k)) m91[k / 8] |= (uint8_t)(0x80u >> (k % 8));
        const uint16_t crcRx = ftx_extract_crc(m91);
        uint8_t t[FTX_LDPC_K_BYTES]; std::memcpy(t, m91, sizeof t);
        t[9] &= 0xF8; t[10] = 0; t[11] = 0;
        if (ftx_compute_crc(t, 96 - 14) != crcRx) continue;
        bestD = d; bestErr = err; std::memcpy(a91, m91, FTX_LDPC_K_BYTES);
    }
    return bestErr;
}


// ── The a-priori book (see FtxApBook in the header) ─────────────────────────────────────────────
namespace {
inline void setBit77(uint64_t w[2], int i) { w[i >> 6] |= 1ull << (i & 63); }
inline bool bit77(const uint64_t w[2], int i) { return (w[i >> 6] >> (i & 63)) & 1; }
void maskRange(uint64_t m[2], int lo, int hi) { for (int i = lo; i <= hi; ++i) setBit77(m, i); }
bool isGrid4(const char* g) {
    return std::strlen(g) == 4 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R'
        && g[2] >= '0' && g[2] <= '9' && g[3] >= '0' && g[3] <= '9' && std::strcmp(g, "RR73") != 0;
}
/** The last field of a standard message as a person would send it: a grid, a report (R optional), RRR, RR73, 73. */
bool plausibleExtra(const char* e) {
    if (!e[0]) return true;
    if (isGrid4(e) || !std::strcmp(e, "RRR") || !std::strcmp(e, "RR73") || !std::strcmp(e, "73")) return true;
    const char* r = e[0] == 'R' ? e + 1 : e;
    if ((r[0] == '+' || r[0] == '-') && std::isdigit((unsigned char)r[1]) && std::isdigit((unsigned char)r[2]) && !r[3]) {
        const int v = std::atoi(r);
        return v >= -30 && v <= 30;
    }
    return false;
}
/** A standard callsign as the 28-bit field carries one: 3-6 characters, letters and digits, a digit among the
 *  first three, ending in a letter — and no hash, no portable suffix. */
bool plausibleCall(const char* c) {
    const int n = (int)std::strlen(c);
    if (n < 3 || n > 6) return false;
    bool digit3 = false;
    for (int i = 0; i < n; ++i) {
        if (!std::isalnum((unsigned char)c[i])) return false;
        if (i < 3 && std::isdigit((unsigned char)c[i])) digit3 = true;
    }
    return digit3 && std::isalpha((unsigned char)c[n - 1]);
}
}

namespace {
/** Any callsign field an FT8 message prints: "<...>" (a hash not yet heard), "<CALL>", a standard call with /P or /R,
 *  or a compound one ("PJ4/K1ABC", "W1AW/9"): every part letters and digits, the main part 3-7 long with a digit in
 *  it and a letter at its end, any other part 1-4 long with a letter in it (or a single district digit after it). */
bool plausibleAnyCall(std::string c) {
    if (c == "<...>") return true;
    if (c.size() > 2 && c.front() == '<' && c.back() == '>') c = c.substr(1, c.size() - 2);
    std::vector<std::string> parts;
    size_t a = 0;
    for (size_t b; (b = c.find('/', a)) != std::string::npos; a = b + 1) parts.push_back(c.substr(a, b - a));
    parts.push_back(c.substr(a));
    if (parts.size() > 2) return false;
    size_t mainIdx = 0;
    for (size_t i = 1; i < parts.size(); ++i) if (parts[i].size() > parts[mainIdx].size()) mainIdx = i;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        if (p.empty()) return false;
        bool letter = false, digit = false;
        for (char ch : p) {
            if (!std::isalnum((unsigned char)ch)) return false;
            letter |= std::isalpha((unsigned char)ch) != 0; digit |= std::isdigit((unsigned char)ch) != 0;
        }
        if (i == mainIdx) {
            if (p.size() < 3 || p.size() > 7 || !digit || !letter || !std::isalpha((unsigned char)p.back())) return false;
        } else if (!(p.size() <= 4 && letter) && !(i > mainIdx && p.size() == 1 && digit) && !(p == "P" || p == "R")) {
            return false;
        }
    }
    return true;
}
}

/* ★★ A CODEWORD OSD FOUND MUST READ LIKE A MESSAGE (2026-10-26). OSD searches hundreds of codewords for one the
 *  CRC-14 accepts; over enough noise one random codeword will pass and unpack (the FT2 noise run with every candidate
 *  tried found "1/Y2ESZU4Y3 <...> 73"). So an OSD decode must be a message people send — standard, non-standard
 *  call, or DXpedition — never free text or telemetry (71 arbitrary bits always "unpack"), and every call in it
 *  plausible. A decode that BP reached on its own keeps every type, as before. */
bool ft8OsdPlausible(const ftx_message_t& msg) {
    const ftx_message_type_t t = ftx_message_get_type(&msg);
    if (t != FTX_MESSAGE_TYPE_STANDARD && t != FTX_MESSAGE_TYPE_NONSTD_CALL && t != FTX_MESSAGE_TYPE_DXPEDITION
        && ftx_message_get_i3(&msg) != 2) return false;
    ftx_message_t m = msg; char txt[FTX_MAX_MESSAGE_LENGTH] = {}; ftx_message_offsets_t offs;
    if (ftx_message_decode(&m, nullptr, txt, &offs) != FTX_MESSAGE_RC_OK) return false;
    std::vector<std::string> tok;
    for (char* q = std::strtok(txt, " "); q; q = std::strtok(nullptr, " ")) tok.push_back(q);
    for (size_t i = 0; i < tok.size(); ++i) {
        std::string w = tok[i];
        if (!w.empty() && w.back() == ';') w.pop_back();
        if (w == "CQ" || w == "DE" || w == "QRZ" || w == "RR73" || w == "RRR" || w == "73" || w == "R") continue;
        if (plausibleExtra(w.c_str())) continue;                       // a grid or a report
        if (i == 1 && tok[0] == "CQ" && w.size() <= 4) {               // "CQ DX", "CQ POTA", "CQ 123"
            bool alnum = true; for (char ch : w) alnum &= std::isalnum((unsigned char)ch) != 0;
            if (alnum && tok.size() >= 3) continue;
        }
        if (!plausibleAnyCall(w)) return false;
    }
    return true;
}

bool FtxApBook::bitsOf(const char* text, bool ft4, uint64_t out[2]) {
    out[0] = out[1] = 0;
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, nullptr, text) != FTX_MESSAGE_RC_OK) return false;
    if (ftx_message_get_i3(&m) != 1) return false;           // ★ standard messages only
    uint8_t p[10];
    for (int b = 0; b < 10; ++b) p[b] = ft4 ? (uint8_t)(m.payload[b] ^ kFT4_XOR_sequence[b]) : m.payload[b];
    for (int i = 0; i < 77; ++i) if ((p[i / 8] >> (7 - i % 8)) & 1) setBit77(out, i);
    return true;
}

int FtxApBook::findCall(const std::string& c) const {
    for (size_t i = 0; i < calls_.size(); ++i) if (calls_[i].call == c) return (int)i;
    return -1;
}

void FtxApBook::touchCall(const std::string& c) {
    const int i = findCall(c);
    if (i >= 0) { calls_[(size_t)i].last = slot_; return; }
    Call x; x.call = c; x.last = slot_;
    if (!bitsOf((c + " K1ABC FN42").c_str(), ft4_, x.c1) || !bitsOf(("K1ABC " + c + " FN42").c_str(), ft4_, x.c2)) return;
    if (calls_.size() >= 400) {                                // ★ full: the least recently heard makes way
        size_t o = 0;
        for (size_t k = 1; k < calls_.size(); ++k) if (calls_[k].last < calls_[o].last) o = k;
        calls_.erase(calls_.begin() + (long)o);
    }
    calls_.push_back(x);
    dirty_ = true;
}

void FtxApBook::beginSlot() {
    ++slot_;
    const size_t n0 = calls_.size(), p0 = pairs_.size();
    calls_.erase(std::remove_if(calls_.begin(), calls_.end(), [&](const Call& c) { return slot_ - c.last > maxAge_; }), calls_.end());
    pairs_.erase(std::remove_if(pairs_.begin(), pairs_.end(), [&](const Pair& p) { return slot_ - p.last > maxAge_; }), pairs_.end());
    if (calls_.size() != n0 || pairs_.size() != p0) dirty_ = true;
}

void FtxApBook::seed(const std::string& call, const std::string& partner) {
    touchCall(call);
    if (partner.empty()) return;
    touchCall(partner);
    if (findCall(call) < 0 || findCall(partner) < 0) return;
    Pair p; p.a = call; p.b = partner; p.last = slot_;
    pairs_.push_back(p); dirty_ = true;
}

void FtxApBook::heard(const ftx_message_t& msg) {
    if (ftx_message_get_i3(&msg) != 1) return;
    char to[16] = {}, de[16] = {}, ex[16] = {};
    ftx_field_t ft[FTX_MAX_MESSAGE_FIELDS];
    if (ftx_message_decode_std(&msg, nullptr, to, de, ex, ft) != FTX_MESSAGE_RC_OK) return;
    const bool deOk = plausibleCall(de), toOk = plausibleCall(to);
    if (deOk) touchCall(de);
    if (toOk) touchCall(to);
    if (deOk && toOk) {                                         // ★ two stations working each other
        const std::string a = to, b = de;
        if (findCall(a) < 0 || findCall(b) < 0) return;
        for (Pair& p : pairs_)
            if ((p.a == a && p.b == b) || (p.a == b && p.b == a)) { p.last = slot_; return; }
        if (pairs_.size() >= 200) {
            size_t o = 0;
            for (size_t k = 1; k < pairs_.size(); ++k) if (pairs_[k].last < pairs_[o].last) o = k;
            pairs_.erase(pairs_.begin() + (long)o);
        }
        Pair p; p.a = a; p.b = b; p.last = slot_;
        pairs_.push_back(p);
        dirty_ = true;
    }
}

const FtxApBook::Index& FtxApBook::index() {
    if (!dirty_) return idx_;
    Index& x = idx_;
    x = Index{};
    uint64_t t[2];
    x.ok = bitsOf("CQ K1ABC FN42", ft4_, x.cq);
    for (int i = 74; i <= 76; ++i) if (bit77(x.cq, i)) setBit77(x.i3, i);
    const char* tails[3] = { "K1ABC W9XYZ RR73", "K1ABC W9XYZ 73", "K1ABC W9XYZ RRR" };
    for (int k = 0; k < 3; ++k) {
        x.ok = x.ok && bitsOf(tails[k], ft4_, t);
        for (int i = 58; i <= 73; ++i) if (bit77(t, i)) setBit77(x.tail[k], i);
    }
    x.c1.reserve(calls_.size() * 2); x.c2.reserve(calls_.size() * 2);
    for (const Call& c : calls_) { x.c1.push_back(c.c1[0]); x.c1.push_back(c.c1[1]); x.c2.push_back(c.c2[0]); x.c2.push_back(c.c2[1]); }
    for (const Pair& p : pairs_) {
        const int ia = findCall(p.a), ib = findCall(p.b);
        if (ia >= 0 && ib >= 0) x.pairs.push_back({ ia, ib });
    }
    dirty_ = false;
    return idx_;
}

FtxApBook::Hyp FtxApBook::make(const Index& x, int kind, int a, int b, int t) {
    Hyp h; h.kind = (uint8_t)kind;
    uint64_t m1[2] = {0, 0}, m2[2] = {0, 0}, m3[2] = {0, 0}, mt[2] = {0, 0};
    maskRange(m1, 0, 28); maskRange(m2, 29, 57); maskRange(m3, 74, 76); maskRange(mt, 58, 73);
    for (int w = 0; w < 2; ++w) {
        uint64_t mask = m3[w], val = x.i3[w];
        if (kind == 1) { mask |= m1[w]; val |= x.cq[w] & m1[w]; }
        if (kind == 2 || kind >= 4) { mask |= m1[w]; val |= x.c1[2 * (size_t)a + w] & m1[w]; }
        if (kind == 3) { mask |= m2[w]; val |= x.c2[2 * (size_t)a + w] & m2[w]; }
        if (kind >= 4) { mask |= m2[w]; val |= x.c2[2 * (size_t)b + w] & m2[w]; }
        if (kind == 5) { mask |= mt[w]; val |= x.tail[t][w]; }
        h.mask[w] = mask; h.val[w] = val & mask;
    }
    h.nbits = __builtin_popcountll(h.mask[0]) + __builtin_popcountll(h.mask[1]);
    return h;
}

// ── One slot ────────────────────────────────────────────────────────────────────────────────────

/* ★★ THE WINDOW STARTS AT THE SLOT BOUNDARY (2026-10-05). It used to start at +0.8 s, by the decode thread's
 *  clock — 0.3 s after an on-time FT8 signal begins, so the first two Costas symbols were never seen. ft8_lib's
 *  candidate search reaches 1.6 s before the window and ~3 s into it, so a window from the boundary still takes a
 *  station whose clock is 1.5 s fast, and keeps an on-time signal's LAST Costas array too (it ends at 13.14 s;
 *  the window, 93 symbols long, ends at 14.88 s). Measured on the real hour in the commit message. */
float Ft8SlotDecoder::leadSec(bool ft4) { (void)ft4; return 0.0f; }

Ft8SlotDecoder::Ft8SlotDecoder(int sampleRate, FtxMode mode)
    : mode_(mode), ft4_(mode != FtxMode::FT8), rate_(sampleRate) {
    const bool ft4 = ft4_;
    monitor_config_t cfg = {};
    cfg.f_min = 100.0f;
    cfg.f_max = 3500.0f;
    cfg.sample_rate = rate_;
    cfg.time_osr = kTimeOsr;
    cfg.freq_osr = kFreqOsr;
    cfg.protocol = ft4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
    /* ★★ FT4's TIME SEARCH WIDENED (2026-10-26). ft8_lib searches -10 .. +20 blocks, which for FT4's 48 ms blocks is
     *  a start of -0.48 .. +0.96 s — DT -0.98 .. +0.46 against FT4's nominal 0.5 s. A station whose clock or audio
     *  path is half a second late was never even a candidate (the synthetic band at DT -0.5 .. +1.0: decodes
     *  levelled off near 60 % however strong), and on the real 40 m FT4 hour our decodes piled up at the -0.98 edge.
     *  -24 .. +36 is DT -1.65 .. +1.23 s; the window (7.49 s) still holds the whole 5.04 s frame at the late end.
     *  The search is the cheap part of a pass (16 frequencies per vector). */
    if (mode == FtxMode::FT4) { cfg.time_min = kFt4TimeMin; cfg.time_max = kFt4TimeMax; }
    if (mode == FtxMode::FT2) {                 // ★ FT4's frame at 24 ms a symbol (see FtxMode)
        cfg.symbol_period = kFt2SymbolPeriod;
        cfg.slot_time = kFt2WindowTime;
        cfg.time_min = kFt2TimeMin;
        cfg.time_max = kFt2TimeMax;
    }
    monitor_init(&mon_, &cfg);
    nn_ = ft4 ? FT4_NN : FT8_NN;
    nsps_ = mon_.block_size;
    windowSamples_ = mon_.wf.max_blocks * mon_.block_size;
    // GFSK frequency pulse (ft8_lib's gen_ft8.c / WSJT-X gen_ft8wave): BT 2.0 for FT8, 1.0 for FT4.
    const float bt = ft4 ? 1.0f : 2.0f, k = 5.336446f;   // pi * sqrt(2 / ln 2)
    pulse_.resize((size_t)3 * nsps_);
    for (int i = 0; i < 3 * nsps_; ++i) {
        const float t = i / (float)nsps_ - 1.5f;
        pulse_[(size_t)i] = (std::erf(k * bt * (t + 0.5f)) - std::erf(k * bt * (t - 0.5f))) / 2;
    }
    // ★ Only where the pulse is not ~0: FT8's (BT 2) is under 1e-6 for most of its 3 symbols — half the adds gone.
    pulseLo_ = 0; pulseHi_ = 3 * nsps_;
    while (pulseLo_ < pulseHi_ && pulse_[(size_t)pulseLo_] < 1e-6f) ++pulseLo_;
    while (pulseHi_ > pulseLo_ && pulse_[(size_t)(pulseHi_ - 1)] < 1e-6f) --pulseHi_;
    const size_t span = (size_t)(nn_ + 2) * nsps_;
    dphi_.resize(span);
    refC_.resize((size_t)nn_ * nsps_);
    refS_.resize((size_t)nn_ * nsps_);
}
Ft8SlotDecoder::~Ft8SlotDecoder() { monitor_free(&mon_); }

void Ft8SlotDecoder::buildWaterfall(const float* a, int n) {
    monitor_reset(&mon_);
    // ★ monitor_reset keeps the STFT's history frame; a fresh window must not start with the last one's tail.
    std::memset(mon_.last_frame, 0, sizeof(float) * (size_t)mon_.nfft);
    for (int pos = 0; pos + mon_.block_size <= n; pos += mon_.block_size) monitor_process(&mon_, a + pos);
}

namespace {
// uint8 waterfall value (0.5 dB steps from -120 dB) -> linear power
const float* magPow() {
    static float t[256];
    static bool init = [] { for (int i = 0; i < 256; ++i) t[i] = std::pow(10.0f, (i * 0.5f - 120.0f) / 10.0f); return true; }();
    (void)init;
    return t;
}
}

/* ★ One spectrogram column's mean power over the window. LAZY and cached: only the columns around a decoded signal
 *  are ever asked for, which keeps it off the per-pass bill. */
float Ft8SlotDecoder::colMean(size_t c) {
    if (noise_[c] >= 0) return noise_[c];
    const ftx_waterfall_t& wf = mon_.wf;
    const float* lut = magPow();
    double sum = 0;
    for (int b = 0; b < wf.num_blocks; ++b) sum += lut[wf.mag[(size_t)b * wf.block_stride + c]];
    return noise_[c] = (float)(wf.num_blocks ? sum / wf.num_blocks : 1e-12);
}

/* ★★ SNR, WSJT-X's DEFINITION (audit 2026-10-04: we showed `score*0.5 - 24`, a sync score in a dB costume). WSJT-X
 *  reports signal power over the noise power in 2500 Hz, the noise being a BASELINE of the slot's average spectrum
 *  (its lower envelope — the gaps between stations), not the signal's own bins. Here the same: the mean power in the
 *  DECODED tone of every symbol, over the 20th percentile of the column means within ±250 Hz, minus one (the noise in
 *  the signal's own bin), scaled from one tone bin to 2500 Hz. The signal's own bins cannot be the noise: with a
 *  two-symbol window a strong station leaks into all eight of them, and an SNR read that way squashed +20 dB to +4.
 *  kSnrCal is the one empirical constant, set against UberSDR's jt9 on the same hour (commit message). Clamped to
 *  -30..+40 as WSJT-X's own figures are. */
int Ft8SlotDecoder::snrDb(const ftx_candidate_t& c, const uint8_t* tones) {
    const ftx_waterfall_t& wf = mon_.wf;
    const size_t row = ((size_t)c.time_sub * wf.freq_osr + c.freq_sub) * wf.num_bins;
    const size_t col0 = row + (size_t)c.freq_offset;
    const int span = (int)std::lround(250.0 * mon_.symbol_period);           // ±250 Hz in tone bins
    float v[256]; int m = 0;
    for (int f = std::max(0, c.freq_offset - span); f < std::min(wf.num_bins, c.freq_offset + span) && m < 256; ++f)
        v[m++] = colMean(row + (size_t)f);
    if (m < 4) return -30;
    std::nth_element(v, v + m / 5, v + m);
    const double noise = v[m / 5];
    const float* lut = magPow();
    double sig = 0; int cnt = 0;
    for (int k = 0; k < nn_; ++k) {
        const int b = c.time_offset + k;
        if (b < 0 || b >= wf.num_blocks) continue;
        sig += lut[wf.mag[(size_t)b * wf.block_stride + col0 + tones[k]]];
        ++cnt;
    }
    if (!cnt || noise <= 0) return -30;
    const double ratio = sig / cnt / noise - 1.0;
    const double binHz = 1.0 / mon_.symbol_period;
    /* ★★ ONE CONSTANT PER MODE, ON THE SCALE OTHER STATIONS REPORT (2026-10-26; Stuart: a spot's SNR reads as the
     *  network's does, as WSPR's reads on WSPRnet's). Each is the median of ours minus STUEY3D's on the same messages in
     *  the same slots (the UberSDR decoder stream recorded beside the WAV; test-ftx-sensitivity section 7):
     *    FT8: +3 read +2 dB high over 6,701 decodes on seven recordings -> +1.
     *    FT4: the earlier -9 (four stations) read 6 dB LOW over 926 decodes on four recordings -> -3 + 3 = 0 total.
     *    FT2: see kSnrCalFt2. */
    const double cal = mode_ == FtxMode::FT8 ? kSnrCalFt8 : mode_ == FtxMode::FT4 ? kSnrCalFt4 : kSnrCalFt2;
    double snr = 10.0 * std::log10(std::max(ratio, 1e-3)) + 10.0 * std::log10(binHz / 2500.0) + cal;
    snr = std::max(-30.0, std::min(40.0, snr));
    return (int)std::lround(snr);
}

/* ★★★ SUBTRACTION (WSJT-X's subtractft8, made cheap). A decoded signal is known exactly — its 79 (FT4: 105) tones
 *  — so it can be rebuilt and taken out of the audio, and what was under it decoded on the next pass.
 *  1. FINE TIME. The candidate's start is only known to half a symbol (80 ms): mix to baseband at its frequency,
 *     boxcar-decimate by 32 (375 Hz), and slide the known tones over ±0.6 symbol for the most energy.
 *  2. The reference: the exact GFSK waveform (ft8_lib's pulse), unit amplitude, at that start and frequency.
 *  3. FINE FREQUENCY from the reference itself: audio × conj(reference) is the signal's complex amplitude, and its
 *     phase drift from symbol to symbol is the frequency error (WSJT-X refines in its downsampler; this is cheaper).
 *  4. The amplitude is that product averaged over two symbols (a normalised boxcar — it follows HF fading), and
 *     2·Re(amplitude × reference) is subtracted.
 *  ★ Cost: a few passes over one signal's samples, no FFT. The bench measures it per pass. */
void Ft8SlotDecoder::subtract(float* a, int n, const uint8_t* tones, double start, double f0, int snr) {
    const int N = nn_ * nsps_;
    const double fs = rate_;
    const SinTable& tab = sinTable();
    const double T = mon_.symbol_period;
    double df = 0;

    // ── 1. fine time (and frequency) on a decimated baseband ──
    {
        const int D = 32, spsd = nsps_ / D;
        const int Md = (int)std::lround(0.6 * spsd);                  // ±0.6 symbol, in decimated samples
        const int i0 = (int)std::floor(start) - Md * D;
        const int nd = N / D + 2 * Md + 1;
        zr_.assign((size_t)nd, 0.0f); zi_.assign((size_t)nd, 0.0f);
        // mixer e^{-j 2π f0 idx/fs}, as a uint32 phase so it is exact across the slot
        const uint32_t step = (uint32_t)(int64_t)std::llround(f0 / fs * 4294967296.0);
        /* ★★ A BLOCK ROTATOR, NOT A LOOKUP PER SAMPLE (2026-10-25). e^{-jθ(b+q)} = e^{-jθ(b)} · e^{-jθ(q)}: the 32
         *  in-block phases are the same for every block, so they are looked up once, each block is a 32-tap complex
         *  dot product (four lanes at a time), and one rotation by the block's own phase. Both factors come from the
         *  same 4096-entry table as before, so the accuracy is the table's (-62 dB), as it was. */
        alignas(16) float wr[D], wi[D];
        for (int q = 0; q < D; ++q) { const int t = (int)(((uint32_t)q * step) >> 20); wr[q] = tab.c[t]; wi[q] = -tab.s[t]; }
        for (int j = 0; j < nd; ++j) {
            const int b = i0 + j * D;
            const uint32_t ph = (uint32_t)((int64_t)b * (int64_t)step);
            float ur = 0, ui = 0;
            if (b >= 0 && b + D <= n) {
                v4f accR = v4_set1(0.0f), accI = v4_set1(0.0f);
                for (int q = 0; q < D; q += 4) {
                    const v4f x = v4_load(a + b + q);
                    accR = v4_madd(x, v4_load(wr + q), accR);
                    accI = v4_madd(x, v4_load(wi + q), accI);
                }
                float tr[4], ti[4]; v4_store(tr, accR); v4_store(ti, accI);
                ur = (tr[0] + tr[1]) + (tr[2] + tr[3]); ui = (ti[0] + ti[1]) + (ti[2] + ti[3]);
            } else {
                for (int q = 0; q < D; ++q) {
                    if (b + q < 0 || b + q >= n) continue;
                    ur += a[b + q] * wr[q]; ui += a[b + q] * wi[q];
                }
            }
            const int t = (int)(ph >> 20);
            const float cb = tab.c[t], sb = tab.s[t];                    // × e^{-jθ(b)} = cb - j·sb
            zr_[(size_t)j] = cb * ur + sb * ui;
            zi_[(size_t)j] = cb * ui - sb * ur;
        }
        const int ntones = ft4_ ? 4 : 8;
        if ((int)toneR_.size() != ntones * spsd) {                      // tone t: t cycles per symbol
            toneR_.resize((size_t)ntones * spsd); toneI_.resize((size_t)ntones * spsd);
            for (int t = 0; t < ntones; ++t)
                for (int j = 0; j < spsd; ++j) {
                    toneR_[(size_t)t * spsd + j] = (float)std::cos(kTwoPi * t * j / spsd);
                    toneI_[(size_t)t * spsd + j] = (float)std::sin(kTwoPi * t * j / spsd);
                }
        }
        // per-symbol correlation with the known tone; `pairs` also returns Σ c[k+1]·conj(c[k])
        // ★ four lanes at a time (2026-10-25): 60 taps for FT8, 18 for FT4 (the last 2 scalar)
        const int sp4 = spsd & ~3;
        auto corr = [&](int m, double* pr, double* pi) {
            double e = 0, prevR = 0, prevI = 0, sR = 0, sI = 0; bool have = false;
            for (int k = 0; k < nn_; ++k) {
                const int b = m + k * spsd;
                if (b < 0 || b + spsd > nd) { have = false; continue; }
                const float* r = &toneR_[(size_t)tones[k] * spsd]; const float* s = &toneI_[(size_t)tones[k] * spsd];
                const float* x = &zr_[(size_t)b]; const float* y = &zi_[(size_t)b];
                v4f aR = v4_set1(0.0f), aI = v4_set1(0.0f);
                for (int j = 0; j < sp4; j += 4) {
                    const v4f X = v4_load(x + j), Y = v4_load(y + j), R = v4_load(r + j), S = v4_load(s + j);
                    aR = v4_madd(X, R, v4_madd(Y, S, aR));
                    aI = v4_madd(Y, R, v4_madd(v4_mul(X, v4_set1(-1.0f)), S, aI));
                }
                float tr[4], ti[4]; v4_store(tr, aR); v4_store(ti, aI);
                float cr = (tr[0] + tr[1]) + (tr[2] + tr[3]), ci = (ti[0] + ti[1]) + (ti[2] + ti[3]);
                for (int j = sp4; j < spsd; ++j) { cr += x[j] * r[j] + y[j] * s[j]; ci += y[j] * r[j] - x[j] * s[j]; }
                e += (double)cr * cr + (double)ci * ci;
                if (pr) {
                    if (have) { sR += cr * prevR + ci * prevI; sI += ci * prevR - cr * prevI; }
                    prevR = cr; prevI = ci; have = true;
                }
            }
            if (pr) { *pr = sR; *pi = sI; }
            return e;
        };
        int best = Md; double bestE = -1;
        for (int m = 0; m <= 2 * Md; m += 3) { const double e = corr(m, nullptr, nullptr); if (e > bestE) { bestE = e; best = m; } }
        const int c0 = best;
        for (int m = std::max(0, c0 - 2); m <= std::min(2 * Md, c0 + 2); ++m) {
            if (m == c0) continue;
            const double e = corr(m, nullptr, nullptr); if (e > bestE) { bestE = e; best = m; }
        }
        start = (double)i0 + (double)best * D;
        /* ★ The frequency error: a continuous-phase signal advances 2π(f0 + df + tone·Δf)·T over a symbol, and the
         *  tone term is a whole number of turns — so symbol k+1's correlation leads symbol k's by 2π·df·T. */
        double pr = 0, pi = 0;
        corr(best, &pr, &pi);
        df = std::atan2(pi, pr) / (kTwoPi * T);
    }

    // ── 2. the reference: GFSK phase steps (ft8_lib's synth_gfsk), only where the pulse is not ~0 ──
    const int s0 = (int)std::lround(start);
    std::fill(dphi_.begin(), dphi_.end(), 0.0f);
    {
        const int lo4 = pulseLo_ & ~3;                                  // the pulse is ~0 below pulseLo_: harmless
        for (int i = 0; i < nn_; ++i) {
            if (!tones[i]) continue;
            float* d = &dphi_[(size_t)i * nsps_];
            const v4f tv = v4_set1((float)tones[i]);
            int j = lo4;
            for (; j + 4 <= pulseHi_; j += 4) v4_store(d + j, v4_madd(tv, v4_load(&pulse_[(size_t)j]), v4_load(d + j)));
            for (; j < pulseHi_; ++j) d[j] += (float)tones[i] * pulse_[(size_t)j];
        }
    }
    for (int j = 0; j < 2 * nsps_; ++j) {                      // the dummy symbols either side
        dphi_[(size_t)j] += pulse_[(size_t)(j + nsps_)] * tones[0];
        dphi_[(size_t)(j + nn_ * nsps_)] += pulse_[(size_t)j] * tones[nn_ - 1];
    }
    const uint32_t base = (uint32_t)(int64_t)std::llround((f0 + df) / fs * 4294967296.0);
    const float toneStep = (float)(1.0 / T / fs * 4294967296.0);
    const float* dp = &dphi_[(size_t)nsps_];                   // sample k of the waveform is dphi[k + nsps]

    // ── 3. audio × conj(reference) over the part of the signal inside the window, summed per block ──
    /* ★★★ THE AMPLITUDE PER 16-SAMPLE BLOCK, NOT PER SAMPLE (2026-10-25). The amplitude is a two-symbol boxcar of
     *  audio × conj(reference) — it follows HF fading, over 3,840 samples. Computing that boxcar afresh at every
     *  sample was 48 % of the subtraction on the Pi 2 build (53 instructions a sample, by the qemu counter); a box
     *  that moves in 16-sample steps differs from it by at most 16 of its 3,840 samples, and the amplitude between
     *  block centres is interpolated linearly, so it is continuous. Not bit-identical to the per-sample box;
     *  measured on the real recordings (commit message).
     *  ★ The reference itself is generated ONCE (refC_/refS_, the same table values the per-sample loops looked up)
     *    and both the block sums and the subtraction then run four samples at a time. The phase is the same uint32
     *    accumulator as before, its increments computed four at a time (a float→int convert truncates as the cast
     *    does), so the reference is the same sample for sample. RAM: 2 × 152 K floats (1.2 MB) for FT8 — what the
     *    stored products (prodRe_/prodIm_) took before, which are gone. */
    const int kLo = std::max(0, -s0), kHi = std::min(N, n - s0);
    if (kHi <= kLo) return;
    int B = 16;
    while (B > 1 && nsps_ % B) B /= 2;                                 // a block divides a symbol (FT8 1920, FT4 576)
    const int len = kHi - kLo, nb = (len + B - 1) / B, H = std::max(1, (snr >= kSubShortBoxSnr ? nsps_ / 2 : nsps_) / B);  // H blocks either side
    /* ★ ±½ SYMBOL FOR A STRONG SIGNAL (2026-10-26). The amplitude box was ±1 symbol for every signal. A strong station's
     *  residue is what hides the most under it, and its amplitude is measured well over half the span, so it can
     *  follow faster fading: on the busy 40 m hour ±½ symbol for decodes at -5 dB or more gave +52 decodes (2,403 ->
     *  2,455), +29 on the busy 20 m, the rest unchanged; ±½ for every signal lost a few weak ones, ±¼ lost many. */
    uint32_t ph = 0;
    for (int k = 0; k < kLo; ++k) ph += base + (uint32_t)(int32_t)(dp[k] * toneStep);
    float* rc = refC_.data();
    float* rs = refS_.data();
    {
        // the phase increments, four at a time; then the accumulator and the table, one sample at a time
        int32_t inc[16];
        const v4f ts = v4_set1(toneStep);
        for (int k0 = kLo; k0 < kHi; k0 += 16) {
            const int m = std::min(16, kHi - k0);
            int j = 0;
            for (; j + 4 <= m; j += 4) {
                const v4f x = v4_mul(v4_load(dp + k0 + j), ts);
#if VIBE_FT8_NEON
                vst1q_s32(inc + j, vcvtq_s32_f32(x));
#elif VIBE_FT8_SSE2
                _mm_storeu_si128((__m128i*)(inc + j), _mm_cvttps_epi32(x));
#else
                for (int l = 0; l < 4; ++l) inc[j + l] = (int32_t)x.v[l];
#endif
            }
            for (; j < m; ++j) inc[j] = (int32_t)(dp[k0 + j] * toneStep);
            for (j = 0; j < m; ++j) {
                const int t = (int)(ph >> 20);
                rc[k0 + j] = tab.c[t]; rs[k0 + j] = tab.s[t];
                ph += base + (uint32_t)inc[j];
            }
        }
    }
    blkR_.resize((size_t)nb); blkI_.resize((size_t)nb);
    const float* xa = a + s0;
    for (int bk = 0; bk < nb; ++bk) {
        const int k0 = kLo + bk * B, k1 = std::min(kHi, k0 + B);
        int k = k0;
        v4f accR = v4_set1(0.0f), accI = v4_set1(0.0f);
        for (; k + 4 <= k1; k += 4) {
            const v4f x = v4_load(xa + k);
            accR = v4_madd(x, v4_load(rc + k), accR);
            accI = v4_madd(x, v4_load(rs + k), accI);
        }
        float tr[4], ti[4]; v4_store(tr, accR); v4_store(ti, accI);
        float sr = (tr[0] + tr[1]) + (tr[2] + tr[3]), si = (ti[0] + ti[1]) + (ti[2] + ti[3]);
        for (; k < k1; ++k) { sr += xa[k] * rc[k]; si += xa[k] * rs[k]; }
        blkR_[(size_t)bk] = sr; blkI_[(size_t)bk] = -si;
    }

    // ── 4. the amplitude at each block centre (the box of ±H blocks, normalised), and the subtraction ──
    ampR_.resize((size_t)nb); ampI_.resize((size_t)nb);
    {
        double sr = 0, si = 0; int lo = 0, hi = 0;
        auto blkLen = [&](int bk) { return std::min(B, len - bk * B); };
        int cnt = 0;
        for (int bk = 0; bk < nb; ++bk) {
            const int wantHi = std::min(nb, bk + H), wantLo = std::max(0, bk - H);
            while (hi < wantHi) { sr += blkR_[(size_t)hi]; si += blkI_[(size_t)hi]; cnt += blkLen(hi); ++hi; }
            while (lo < wantLo) { sr -= blkR_[(size_t)lo]; si -= blkI_[(size_t)lo]; cnt -= blkLen(lo); ++lo; }
            const double inv = 1.0 / cnt;
            ampR_[(size_t)bk] = (float)(sr * inv); ampI_[(size_t)bk] = (float)(si * inv);
        }
    }
    // each block's samples: the amplitude runs linearly from the midpoint with the previous block's to the midpoint
    // with the next's (the ends hold their own), so it is continuous across blocks
    const float invB = 1.0f / (float)B;
    float* xo = a + s0;
    static const float kHalfSteps[4] = { 0.5f, 1.5f, 2.5f, 3.5f };
    const v4f halfSteps = v4_load(kHalfSteps);
    for (int bk = 0; bk < nb; ++bk) {
        const float cR = ampR_[(size_t)bk], cI = ampI_[(size_t)bk];
        const float pR = bk > 0 ? ampR_[(size_t)bk - 1] : cR, pI = bk > 0 ? ampI_[(size_t)bk - 1] : cI;
        const float nR = bk + 1 < nb ? ampR_[(size_t)bk + 1] : cR, nI = bk + 1 < nb ? ampI_[(size_t)bk + 1] : cI;
        const float aR0 = 0.5f * (pR + cR), aI0 = 0.5f * (pI + cI);
        const float dR = (0.5f * (cR + nR) - aR0) * invB, dI = (0.5f * (cI + nI) - aI0) * invB;
        const int k0 = kLo + bk * B, k1 = std::min(kHi, k0 + B);
        int k = k0;
        // a -= 2·(ar·c − ai·s), with ar, ai the interpolated amplitude: four samples at a time
        const v4f m2dR = v4_set1(-2.0f * dR), m2dI = v4_set1(2.0f * dI);
        const v4f m2aR = v4_set1(-2.0f * aR0), m2aI = v4_set1(2.0f * aI0);
        for (; k + 4 <= k1; k += 4) {
            const v4f f = v4_add(halfSteps, v4_set1((float)(k - k0)));
            const v4f gr = v4_madd(m2dR, f, m2aR), gi = v4_madd(m2dI, f, m2aI);    // −2·ar, +2·ai
            v4_store(xo + k, v4_madd(gi, v4_load(rs + k), v4_madd(gr, v4_load(rc + k), v4_load(xo + k))));
        }
        for (; k < k1; ++k) {
            const float f = (float)(k - k0) + 0.5f;
            const float ar = aR0 + dR * f, ai = aI0 + dI * f;
            xo[k] -= 2.0f * (ar * rc[k] - ai * rs[k]);
        }
    }
}

/** The 91 bits (77 + CRC-14, packed) of a codeword already checked → the message, as ftx_decode_candidate makes it
 *  (FT4/FT2: the XOR sequence taken off). */
bool Ft8SlotDecoder::a91ToMessage(const uint8_t* a91, ftx_message_t& msg) const {
    ftx_message_init(&msg);
    for (int b = 0; b < 10; ++b) msg.payload[b] = ft4_ ? (uint8_t)(a91[b] ^ kFT4_XOR_sequence[b]) : a91[b];
    msg.payload[9] &= 0xF8;
    uint8_t t[FTX_LDPC_K_BYTES]; std::memcpy(t, a91, sizeof t); t[9] &= 0xF8; t[10] = 0; t[11] = 0;
    msg.hash = ftx_compute_crc(t, 96 - 14);
    return true;
}

namespace {
/** BP's hard decisions → the 91 message+CRC bits, packed, if the CRC holds (ftx_decode_candidate's check). */
bool plainCrcOk(const uint8_t* plain174, uint8_t a91[FTX_LDPC_K_BYTES]) {
    std::memset(a91, 0, FTX_LDPC_K_BYTES);
    for (int i = 0; i < FTX_LDPC_K; ++i) if (plain174[i]) a91[i / 8] |= (uint8_t)(0x80u >> (i % 8));
    const uint16_t rx = ftx_extract_crc(a91);
    uint8_t t[FTX_LDPC_K_BYTES]; std::memcpy(t, a91, sizeof t);
    t[9] &= 0xF8; t[10] = 0; t[11] = 0;
    return ftx_compute_crc(t, 96 - 14) == rx;
}
}

/* ★★ THE SECOND LOOK (ftx_fine.h). BP on the one-, two- and three-symbol likelihoods in turn, then OSD on them —
 *  only for a candidate whose fine sync says it is a signal at all. */
bool Ft8SlotDecoder::fineDecode(const float* audio, int n, double start, double f0, const Ft8SlotOptions& o,
                                ftx_message_t& msg, bool& viaOsd, int& apKind, double& startOut, double& f0Out, Ft8SlotStats& st, int pass) {
    (void)audio; (void)n;
    ++st.fineTried[pass - 1];
    const int minSync = o.fineMinSync > 0 ? o.fineMinSync : (ft4_ ? kFineMinSyncFt4 : kFineMinSyncFt8);
    FtxFine::Meas& M = fineMeas_;
    if (!fine_->measure(start, f0, minSync, M)) return false;
    ++st.finePassed[pass - 1];
    startOut = M.start; f0Out = M.f0;
    uint8_t plain[FTX_LDPC_N], a91[FTX_LDPC_K_BYTES];
    float (&L)[FtxFine::kMaxSets][FTX_LDPC_N] = fineLlr_;
    /* ★ The order is what decodes first on the synthetic band at the threshold (-21/-20 dB): the two-symbol set
     *  most often, then the three-symbol (FT4: four), then the one-symbol; OSD on the longest groups only. */
    const int order[3] = { 1, 2, 0 };
    int bestErr = 1 << 20;
    for (int i = 0; i < M.nsets; ++i) {
        const int k = order[i];
        fine_->llrGroup(fine_->groupOf(k), L[k]);
        int err = 0;
        bp_decode_ex(L[k], o.ldpcIters, kFineBpStall, plain, &err);
        bestErr = std::min(bestErr, err);
        if (err == 0 && plainCrcOk(plain, a91)) { a91ToMessage(a91, msg); viaOsd = false; ++st.fineDecodes[pass - 1]; return true; }
    }
    /* ★ OSD only for a near miss: on the real 80 m hour every candidate OSD rescued had come within 10 unsatisfied
     *  checks in BP, while most of those it could not rescue were further out (Stuart: spend cycles only where they
     *  find decodes). */
    if (o.osdMaxErrors > 0 && bestErr <= (o.fineOsdMaxBpErr > 0 ? o.fineOsdMaxBpErr : kFineOsdMaxBpErr)) {
        for (int k : { 2, 1 }) {
            const int e = ft8Osd(L[k], o.osdMaxErrors, o.osdPairSpan, a91);
            if (e >= 0) { a91ToMessage(a91, msg); viaOsd = true; ++st.fineDecodes[pass - 1]; return true; }
        }
    }
    /* ★ A-priori decoding: near misses only (BP came within kApMaxBpErr checks), on the two-symbol set (the one that
     *  decodes most often), then the three-symbol. */
    if (o.ap && o.apTry && bestErr <= (o.apMaxBpErr > 0 ? o.apMaxBpErr : kApMaxBpErr)) {
        ++st.apTried[pass - 1];
        for (int k : { 1, 2 }) {
            if (apDecode(L[k], o, msg, apKind)) { viaOsd = false; ++st.apDecodes[pass - 1]; ++st.fineDecodes[pass - 1]; return true; }
        }
    }
    return false;
}


/* ★★★ A-PRIORI DECODING of a near miss (FtxApBook). Screen every hypothesis on the candidate's own likelihoods — the
 *  z-score of its known bits' agreement, Σ ±L / √Σ L² (≈ N(0,1) for a wrong hypothesis) — then decode the best few
 *  with their bits clamped to near-certain (1.01 × the largest |L|, as the paper's decoder does): BP, then OSD. A
 *  codeword is believed only through every guard in apCheck. */
namespace {
bool apCheck(const float* L, const FtxApBook::Hyp& h0, const Bits174& cw, const ftx_message_t& msg, int maxFreeErr, int* freeErrOut, int* kindOut) {
    FtxApBook::Hyp h = h0;
    bool held = true;
    for (int w = 0; w < 2 && held; ++w)
        for (uint64_t m = h.mask[w]; m; m &= m - 1) {
            const int b = __builtin_ctzll(m), j = w * 64 + b;
            if (getBit(cw, j) != (((h.val[w] >> b) & 1) != 0)) { held = false; break; }   // the assumption must survive
        }
    if (!held) {
        /* ★ A sign-off hypothesis ("X Y RR73") can bring out the pair's OTHER message ("X Y -12"): the pair's 58 bits
         *  held and the last field did not. That is a pair (kind 4) decode, judged as one. */
        if (h.kind != 5) return false;
        h.kind = 4;
        h.mask[0] &= (1ull << 58) - 1; h.mask[1] &= 0x7ull << 10;   // bits 0..57 and 74..76
        h.val[0] &= h.mask[0]; h.val[1] &= h.mask[1];
        h.nbits = __builtin_popcountll(h.mask[0]) + __builtin_popcountll(h.mask[1]);
        for (int w = 0; w < 2; ++w)
            for (uint64_t m = h.mask[w]; m; m &= m - 1) {
                const int b = __builtin_ctzll(m), j = w * 64 + b;
                if (getBit(cw, j) != (((h.val[w] >> b) & 1) != 0)) return false;
            }
        maxFreeErr = (int)(maxFreeErr * (double)(FTX_LDPC_N - h.nbits) / (FTX_LDPC_N - h0.nbits));
    }
    *kindOut = h.kind;
    int err = 0;
    for (int j = 0; j < FTX_LDPC_N; ++j) {
        if (j < 77 && ((h.mask[j >> 6] >> (j & 63)) & 1)) continue;
        err += (L[j] > 0) != getBit(cw, j);
    }
    *freeErrOut = err;
    if (err > maxFreeErr) return false;
    if (ftx_message_get_i3(&msg) != 1) return false;
    char to[16] = {}, de[16] = {}, ex[16] = {};
    ftx_field_t ft[FTX_MAX_MESSAGE_FIELDS];
    if (ftx_message_decode_std(&msg, nullptr, to, de, ex, ft) != FTX_MESSAGE_RC_OK) return false;
    if (!plausibleExtra(ex)) return false;
    switch (h.kind) {   // (h may have become a pair hypothesis above)
    case 1: return !std::strcmp(to, "CQ") && plausibleCall(de) && (!ex[0] || isGrid4(ex));
    case 2: return plausibleCall(de);
    case 3: {                                   // its next over, or its CQ ("CQ", "CQ DX", "CQ POTA", "CQ 123")
        if (plausibleCall(to)) return true;
        if (std::strncmp(to, "CQ", 2) || (ex[0] && !isGrid4(ex))) return false;
        if (!to[2]) return true;
        if (to[2] != ' ') return false;
        const size_t n = std::strlen(to + 3);
        if (n < 1 || n > 4) return false;
        for (const char* q = to + 3; *q; ++q) if (!std::isalnum((unsigned char)*q)) return false;
        return true;
    }
    default: return true;
    }
}
}

bool Ft8SlotDecoder::apDecode(const float* L, const Ft8SlotOptions& o, ftx_message_t& msg, int& kind) {
    const FtxApBook::Index& X = o.ap->index();
    if (!X.ok) return false;
    const int K = std::min(8, o.apMaxCands > 0 ? o.apMaxCands : kApMaxCands);
    /* ★ Scored by REGION, not hypothesis by hypothesis (2026-10-26): a hypothesis's agreement Σ ±L over its known
     *  bits is a sum over at most four regions — the first call (bits 0..28), the second (29..57), the last field
     *  (58..73) and i3 (74..76) — and a region's sum is 2·Σ L over its set bits − Σ L over the region. So each call is
     *  scored once as a first and once as a second call, and every pair and sign-off is three additions. The
     *  normaliser √Σ L² depends only on WHICH regions are known. */
    auto sumSet = [&](const uint64_t* v, int lo, int hi) {
        float sacc = 0;
        for (int w = lo >> 6; w <= hi >> 6; ++w) {
            uint64_t m = v[w];
            const int b0 = w * 64;
            if (lo > b0) m &= ~0ull << (lo - b0);
            if (hi < b0 + 63) m &= (2ull << (hi - b0)) - 1;
            for (; m; m &= m - 1) sacc += L[b0 + __builtin_ctzll(m)];
        }
        return sacc;
    };
    float S[4] = {0, 0, 0, 0}, Q[4] = {0, 0, 0, 0};     // regions: 0 = 0..28, 1 = 29..57, 2 = 58..73, 3 = 74..76
    for (int i = 0; i < 77; ++i) { const int r = i < 29 ? 0 : i < 58 ? 1 : i < 74 ? 2 : 3; S[r] += L[i]; Q[r] += L[i] * L[i]; }
    const float Ri3 = 2 * sumSet(X.i3, 74, 76) - S[3];
    const float iq13 = 1.0f / std::sqrt(Q[0] + Q[3]), iq23 = 1.0f / std::sqrt(Q[1] + Q[3]);
    const float iq123 = 1.0f / std::sqrt(Q[0] + Q[1] + Q[3]), iqAll = 1.0f / std::sqrt(Q[0] + Q[1] + Q[2] + Q[3]);
    float Rt[3];
    for (int t = 0; t < 3; ++t) Rt[t] = 2 * sumSet(X.tail[t], 58, 73) - S[2];
    const int nc = (int)X.c1.size() / 2;
    rA_.resize((size_t)nc); rB_.resize((size_t)nc);
    for (int c = 0; c < nc; ++c) {
        rA_[(size_t)c] = 2 * sumSet(&X.c1[2 * (size_t)c], 0, 28) - S[0];
        rB_[(size_t)c] = 2 * sumSet(&X.c2[2 * (size_t)c], 29, 57) - S[1];
    }
    struct Pick { float z; int kind, a, b, t; } best[8]; int nb = 0;
    auto offer = [&](float z, int kd, int a, int b, int t) {
        if (z < kApMinZ) return;
        if (nb < K) best[nb++] = { z, kd, a, b, t };
        else if (z > best[K - 1].z) best[K - 1] = { z, kd, a, b, t };
        else return;
        for (int q = nb - 1; q > 0 && best[q].z > best[q - 1].z; --q) std::swap(best[q], best[q - 1]);
    };
    offer((2 * sumSet(X.cq, 0, 28) - S[0] + Ri3) * iq13, 1, -1, -1, 0);
    for (int c = 0; c < nc; ++c) {
        offer((rA_[(size_t)c] + Ri3) * iq13, 2, c, -1, 0);
        offer((rB_[(size_t)c] + Ri3) * iq23, 3, c, -1, 0);
    }
    for (const auto& pr : X.pairs)
        for (int dir = 0; dir < 2; ++dir) {
            const int a = dir ? pr.second : pr.first, b = dir ? pr.first : pr.second;
            const float r4 = rA_[(size_t)a] + rB_[(size_t)b] + Ri3;
            offer(r4 * iq123, 4, a, b, 0);
            for (int t = 0; t < 3; ++t) offer((r4 + Rt[t]) * iqAll, 5, a, b, t);
        }
    float amax = 0;
    for (int j = 0; j < FTX_LDPC_N; ++j) amax = std::max(amax, std::fabs(L[j]));
    const float A = 1.01f * amax;
    for (int k = 0; k < nb; ++k) {
        const FtxApBook::Hyp h = FtxApBook::make(X, best[k].kind, best[k].a, best[k].b, best[k].t);
        if (h.kind == 5) {
            /* ★★ A SIGN-OFF IS A WHOLE CODEWORD (2026-10-26). "X Y RR73" fixes all 77 message bits, so its CRC and its 83
             *  parity bits are fixed too: nothing is left to decode, only to DETECT. The codeword is built and held
             *  against the received bits — the 97 not assumed must agree all but kApFullMaxFree times (a wrong codeword
             *  disagrees in ~48: P(≤ 20) ≈ 1e-8 per trial), and the whole correlation must be strong. On the busy
             *  40 m hour this found more sign-offs than BP + OSD with the bits clamped, at a fraction of the cost. */
            uint8_t p10[10] = {}, a91[FTX_LDPC_K_BYTES] = {};
            for (int i = 0; i < 77; ++i) if ((h.val[i >> 6] >> (i & 63)) & 1) p10[i / 8] |= (uint8_t)(0x80u >> (i % 8));
            ftx_add_crc(p10, a91);
            Bits174 cw{{0, 0, 0}};
            const Bits174* g = generatorRows();
            for (int b = 0; b < FTX_LDPC_K; ++b) if ((a91[b / 8] >> (7 - b % 8)) & 1) { cw.w[0] ^= g[b].w[0]; cw.w[1] ^= g[b].w[1]; cw.w[2] ^= g[b].w[2]; }
            int fe = 0; double sc = 0, q = 0;
            for (int j = 0; j < FTX_LDPC_N; ++j) { const bool bj = getBit(cw, j); if (j >= 77) fe += (L[j] > 0) != bj; sc += bj ? L[j] : -L[j]; q += (double)L[j] * L[j]; }
            const double zfull = q > 0 ? sc / std::sqrt(q) : 0;
            ftx_message_t m;
            a91ToMessage(a91, m);
            int fe2 = -1, kk = 5;
            const bool okc = fe <= kApFullMaxFree && zfull >= kApFullMinZ && apCheck(L, h, cw, m, kApFullMaxFree, &fe2, &kk);
            if (!okc) continue;
            msg = m; kind = 5;
            return true;
        }
        float Lc[FTX_LDPC_N];
        std::memcpy(Lc, L, sizeof Lc);
        for (int w = 0; w < 2; ++w)
            for (uint64_t m = h.mask[w]; m; m &= m - 1) { const int b = __builtin_ctzll(m); Lc[w * 64 + b] = ((h.val[w] >> b) & 1) ? A : -A; }
        uint8_t plain[FTX_LDPC_N], a91[FTX_LDPC_K_BYTES];
        int err = 0;
        bp_decode_ex(Lc, kApBpIters, kFineBpStall, plain, &err);
        Bits174 cw{{0, 0, 0}};
        bool got = false, viaO = false;
        if (err == 0 && plainCrcOk(plain, a91)) {
            got = true;
            for (int j = 0; j < FTX_LDPC_N; ++j) if (plain[j]) setBit(cw, j);
        } else if (o.osdMaxErrors > 0 && best[k].z >= kApOsdMinZ && ft8Osd(Lc, o.osdMaxErrors, o.apOsdPairSpan >= 0 ? o.apOsdPairSpan : o.osdPairSpan, a91) >= 0) {
            got = viaO = true;
            const Bits174* g = generatorRows();
            for (int b = 0; b < FTX_LDPC_K; ++b)
                if ((a91[b / 8] >> (7 - b % 8)) & 1) { cw.w[0] ^= g[b].w[0]; cw.w[1] ^= g[b].w[1]; cw.w[2] ^= g[b].w[2]; }
        }
        if (!got) continue;
        ftx_message_t m;
        a91ToMessage(a91, m);
        const int nfree = FTX_LDPC_N - h.nbits;
        const int maxFree = (int)(kApMaxFreeErrFrac * nfree);
        int fe = -1, kk = h.kind;
        const bool okc = apCheck(L, h, cw, m, maxFree, &fe, &kk);
        if (!okc) continue;
        msg = m; kind = kk;
        return true;
    }
    return false;
}

void Ft8SlotDecoder::decode(float* audio, int n, const Ft8SlotOptions& o, std::vector<Ft8Result>& out, Ft8SlotStats& st) {
    st = Ft8SlotStats{};
    const size_t first = out.size();
    decodedStart_.clear();
    prevKeys_.clear(); curKeys_.clear();
    if (o.ap) o.ap->beginSlot();
    struct Sub { uint8_t tones[FT4_NN > FT8_NN ? FT4_NN : FT8_NN]; double start; double f0; int snr; };
    std::vector<Sub> subs;
    if ((int)cands_.size() < o.maxCandidates) cands_.resize((size_t)o.maxCandidates);
    const auto t0 = std::chrono::steady_clock::now();
    const int passes = std::max(1, std::min(3, o.maxPasses));
    for (int pass = 1; pass <= passes; ++pass) {
        if (pass > 1) {
            if (subs.empty()) break;                               // nothing new to take out: the same search again
            if (o.mayRunPass && !o.mayRunPass(pass, msSince(t0), st.passMs[pass - 2])) break;
        }
        const auto tp = std::chrono::steady_clock::now();
        double* sm = st.stageMs[pass - 1];
        auto lap = [&, t = tp]() mutable { const auto now = std::chrono::steady_clock::now();
                                          const double d = std::chrono::duration<double, std::milli>(now - t).count(); t = now; return d; };
        /* ★★ ONLY WHERE THE AUDIO CHANGED (2026-10-26). A pass after the first sees the audio with the last pass's
         *  decodes taken out — and nowhere else changed. A candidate the last pass already tried, at the same time and
         *  frequency, whose band touches no subtracted signal, would be measured from the same samples and fail the
         *  same way; it is skipped. New candidates (the search's list no longer full of strong stations) and those
         *  beside a subtraction are tried. On busy bands this was most of passes 2 and 3. */
        changed_.clear();
        const float baud = 1.0f / mon_.symbol_period, ntones = ft4_ ? 4.0f : 8.0f;
        for (const Sub& s : subs) changed_.push_back({ (float)s.f0 - 2.0f * baud, (float)s.f0 + (ntones + 1.0f) * baud });
        for (const Sub& s : subs) subtract(audio, n, s.tones, s.start, s.f0, s.snr);
        subs.clear();
        prevKeys_.swap(curKeys_);
        std::sort(prevKeys_.begin(), prevKeys_.end());
        curKeys_.clear();
        sm[Ft8SlotStats::kSubtract] += lap();
        buildWaterfall(audio, n);
        if (fine_) fine_->invalidate();                         // ★ the audio changed (a new slot, or subtracted)
        noise_.assign((size_t)mon_.wf.block_stride, -1.0f);     // ★ unknown until a decode asks (colMean)
        sm[Ft8SlotStats::kWaterfall] += lap();
        const ftx_waterfall_t* wf = &mon_.wf;
        const int nc = ftx_find_candidates(wf, o.maxCandidates, cands_.data(), o.minScore);
        sm[Ft8SlotStats::kSearch] += lap();
        st.candidates[pass - 1] = nc;
        int found = 0;
        failed_.clear();
        for (int i = 0; i < nc; ++i) {
            if (o.abort && o.abort->load(std::memory_order_relaxed)) { st.passes = pass; return; }
            const ftx_candidate_t& c = cands_[(size_t)i];
            const uint32_t key = ((uint32_t)(c.time_offset + 1024) << 14) ^ ((uint32_t)c.time_sub << 12) ^ ((uint32_t)c.freq_offset << 2) ^ c.freq_sub;
            if (o.onlyChanged && pass > 1 && std::binary_search(prevKeys_.begin(), prevKeys_.end(), key)) {
                const float lo = (mon_.min_bin + c.freq_offset) / mon_.symbol_period - 1.5f * baud, hi = lo + (ntones + 2.0f) * baud;
                bool touched = false;
                for (const auto& r : changed_) touched |= hi > r.first && lo < r.second;
                if (!touched) { curKeys_.push_back(key); ++st.skipped[pass - 1]; continue; }
            }
            curKeys_.push_back(key);
            ftx_message_t msg; ftx_decode_status_t ds;
            bool viaOsd = false;
            const bool bpOk = (o.fine && o.fineOnly) ? false : ftx_decode_candidate_ex(wf, &c, o.ldpcIters, o.bpStall, &msg, &ds);
            sm[Ft8SlotStats::kLdpc] += lap();
            const float cHz = (mon_.min_bin + c.freq_offset + (float)c.freq_sub / wf->freq_osr) / mon_.symbol_period;
            const double cStart = (double)(c.time_offset - 1) * mon_.block_size + (double)c.time_sub * mon_.subblock_size;
            double fineStart = cStart, fineF0 = cHz;
            bool viaFine = false;
            int apKind = 0;
            if (!bpOk && o.fine) {
                /* ★ Not a second look at a signal this pass already decoded: a strong station leaves a cluster of
                 *  candidates a sub-bin or a sub-symbol apart, and each would cost a measurement to find it again. */
                bool near = false;
                for (size_t r = first; r < out.size() && !near; ++r)
                    near = std::fabs(out[r].hz - cHz) < 1.0f / mon_.symbol_period
                        && std::fabs(decodedStart_[r - first] - cStart) < 0.75 * nsps_;
                if (near) continue;
                sm[Ft8SlotStats::kPost] += lap();
                if (!fine_) fine_.reset(new FtxFine((int)mode_, rate_, nsps_, windowSamples_));
                if (!fine_->ready()) fine_->ensure(audio, n);
                viaFine = fineDecode(audio, n, cStart, cHz, o, msg, viaOsd, apKind, fineStart, fineF0, st, pass);
                sm[Ft8SlotStats::kFine] += lap();
                if (!viaFine) { failed_.push_back({ cHz, cStart }); continue; }
            } else if (!bpOk) {
                if (o.osdMaxErrors <= 0 || c.score < o.osdMinScore) { failed_.push_back({ cHz, cStart }); continue; }
                ++st.osdTried[pass - 1];
                float llr[FTX_LDPC_N]; uint8_t a91[FTX_LDPC_K_BYTES];
                ftx_candidate_log174(wf, &c, llr);
                const int osdErr = ft8Osd1(llr, o.osdMaxErrors, a91);
                sm[Ft8SlotStats::kOsd] += lap();
                if (osdErr < 0) { failed_.push_back({ cHz, cStart }); continue; }
                a91ToMessage(a91, msg);
                viaOsd = true;
            }
            {   /* ★ A codeword is not yet a message (2026-10-26): one that does not unpack (a type the 77-bit format
                 *  does not define, a field out of range) is noise that happened to satisfy the CRC — OSD, which
                 *  searches for such codewords, found them on real audio. Checked without the hash table, which
                 *  unpacking would otherwise feed. */
                ftx_message_t t = msg; char txt[FTX_MAX_MESSAGE_LENGTH]; ftx_message_offsets_t offs;
                if (ftx_message_decode(&t, nullptr, txt, &offs) != FTX_MESSAGE_RC_OK) { sm[Ft8SlotStats::kPost] += lap(); continue; }
            }
            if (viaOsd && !ft8OsdPlausible(msg)) { sm[Ft8SlotStats::kPost] += lap(); continue; }
            bool dup = false;   // ★ one message per slot, whichever pass and candidate found it first
            for (size_t r = first; r < out.size() && !dup; ++r)
                dup = out[r].msg.hash == msg.hash && std::memcmp(out[r].msg.payload, msg.payload, sizeof msg.payload) == 0;
            if (dup) { sm[Ft8SlotStats::kPost] += lap(); continue; }
            Sub s;
            // ★ The tones as transmitted (ft4_encode applies FT4's XOR itself, as ftx_decode_candidate removed it).
            if (ft4_) ft4_encode(msg.payload, s.tones); else ft8_encode(msg.payload, s.tones);
            // ★ Start sample: the STFT frame of block b, sub s ENDS at (b·osr + s + 1)·subblock and is two symbols
            //   long, so a symbol fills it best when it starts at (b − 1)·block + s·subblock. The second look
            //   measured both finer (to 1/32 symbol and 1/16 tone).
            const float hz = viaFine ? (float)fineF0 : cHz;
            const double startSample = viaFine ? fineStart : cStart;
            Ft8Result r;
            r.msg = msg;
            r.hz = hz;
            // ★ DT from the nominal start: FT8/FT4 0.5 s into the slot (WSJT-X's); FT2 at the boundary (Decodium's
            //   transmitters start 0-300 ms in, by version and platform).
            r.dt = (float)(leadSec(ft4_) + startSample / rate_ - (mode_ == FtxMode::FT2 ? 0.0 : 0.5));
            r.snr = snrDb(c, s.tones);
            r.pass = pass;
            r.osd = viaOsd;
            r.fine = viaFine;
            r.ap = apKind;
            out.push_back(r);
            if (o.ap && !apKind) o.ap->heard(msg);     // ★ only what was heard without assumptions teaches the book
            decodedStart_.push_back(startSample);
            s.start = startSample; s.f0 = hz; s.snr = r.snr;
            if (r.snr >= o.subtractMinSnr) subs.push_back(s);
            ++found;
            sm[Ft8SlotStats::kPost] += lap();
        }
        /* ★★ SUBTRACT ONLY WHAT HIDES SOMETHING (2026-10-26). Taking a signal out costs a pass over its 12.6 s of
         *  samples (~7 M instructions each on the Pi 2 build — most of a busy band's later passes), and it only helps a
         *  weaker signal that overlaps it in frequency. So a decode is subtracted only if this pass left a candidate
         *  undecoded within its band that is not one of its own cluster. */
        if (o.subtractNeeded && pass < passes) {
            const float tone = 1.0f / mon_.symbol_period, nt = ft4_ ? 4.0f : 8.0f;
            subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Sub& sb) {
                for (const auto& fc : failed_) {
                    const float df = fc.first - (float)sb.f0;
                    if (df < -(nt + 1.5f) * tone || df > (nt + 1.5f) * tone) continue;
                    if (std::fabs(df) < 1.5f * tone && std::fabs(fc.second - sb.start) < nsps_) continue;   // its own cluster
                    return false;
                }
                ++st.subtractSkipped[pass - 1];
                return true;
            }), subs.end());
        }
        st.passes = pass;
        st.passMs[pass - 1] = msSince(tp);
        st.passDecodes[pass - 1] = found;
    }
}

// ── The live decoder ────────────────────────────────────────────────────────────────────────────

Ft8Decoder::Ft8Decoder(int sampleRate, FtxMode mode)
    : mode_(mode), ft4(mode != FtxMode::FT8), rate(sampleRate), slot_(sampleRate, mode),
      apBook_(mode != FtxMode::FT8, mode == FtxMode::FT2 ? 160 : mode == FtxMode::FT4 ? 80 : 40) {   // ★ ten minutes of slots
    slotPeriod = mode == FtxMode::FT2 ? kFt2SlotTime : ft4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    window_ = slot_.windowSamples();
    // ★ The ring holds a window plus 3 s: a window is cut the moment its last sample lands, so this is slack
    //   for a late block, not history.
    ring_.assign((size_t)window_ + (size_t)(3 * rate), 0.0f);
    work_.assign((size_t)window_, 0.0f);
    ok = true;
    worker = std::thread([this] { workerLoop(); });
}

Ft8Decoder::~Ft8Decoder() {
    // ★ Abort a decode in progress (checked between candidates) rather than wait seconds for it:
    //   the caller — stopSpots — holds the lock the audio thread's feedSpots needs.
    abortDecode = true;
    { std::lock_guard<std::mutex> lk(wm); stop = true; }
    wcv.notify_all();
    if (worker.joinable()) worker.join();
}

void Ft8Decoder::workerLoop() {
    vibe::vibeFlushDenormals();   // ★ its own thread, not a vibe_thread.h role — see vibedsp/vibe_denormals.h
    std::unique_lock<std::mutex> lk(wm);
    for (;;) {
        wcv.wait(lk, [this] { return pending || stop; });
        if (stop) return;
        pending = false; busy = true;
        lk.unlock();
        runDecode();
        lk.lock();
        busy = false;
    }
}

void Ft8Decoder::process(const int16_t* in, int count, double captureUtc) {
    if (!ok || count <= 0) return;
    const int64_t R = (int64_t)ring_.size();
    for (int i = 0; i < count; i++) ring_[(size_t)((total_ + i) % R)] = (float)in[i] / 32768.0f;
    advance(count, captureUtc);
}

void Ft8Decoder::process(const float* in, int count, double captureUtc) {
    if (!ok || count <= 0) return;
    const int64_t R = (int64_t)ring_.size();
    for (int i = 0; i < count; i++) ring_[(size_t)((total_ + i) % R)] = in[i];
    advance(count, captureUtc);
}

void Ft8Decoder::advance(int count, double captureUtc) {
    if (!std::isfinite(captureUtc)) captureUtc = vibe::vibeUtcNow();
    const int64_t R = (int64_t)ring_.size();
    total_ += count;

    /* ★★ THE ANCHOR: the capture UTC of sample 0, from this block's stamp. A stamp is late by however long the
     *  audio took to reach the decoder, never early — so a stamp EARLIER than the anchor is the better one and
     *  is taken at once, while a later one only leaks in slowly (30 s), following the sample clock's drift
     *  against UTC without chasing one late block. A stamp a whole second later means audio was LOST (the host
     *  drops what it is 4 s behind on): the count no longer says the time, so re-anchor and re-pick the slot. */
    const double est = captureUtc - (double)total_ / rate;
    if (!anchored_ || est - utc0_ > 1.0) {
        if (anchored_) reanchors.fetch_add(1);
        utc0_ = est; anchored_ = true; nextSlot_ = NAN;
        floor_ = total_ - count;       // ★ audio from before the gap has no true time any more: never cut from it
    } else if (est < utc0_) {
        utc0_ = est;
    } else {
        utc0_ += (est - utc0_) * std::min(1.0, (double)count / rate / 30.0);
    }
    const int64_t oldest = std::max<int64_t>(floor_, total_ - R);
    const double lead = Ft8SlotDecoder::leadSec(ft4);
    if (std::isnan(nextSlot_)) {
        // the first slot whose whole window is still ahead of (or in) what we hold
        const double oldestUtc = utc0_ + (double)oldest / rate;
        nextSlot_ = std::ceil((oldestUtc - lead) / slotPeriod) * slotPeriod;
    }
    for (;;) {
        const int64_t i0 = (int64_t)std::llround((nextSlot_ + lead - utc0_) * rate);
        if (i0 < oldest) { nextSlot_ += slotPeriod; continue; }      // its start is gone: wait for the next
        if (total_ < i0 + window_) break;                            // not complete yet
        {
            std::lock_guard<std::mutex> lk(wm);
            if (!busy && !pending) {
                for (int i = 0; i < window_; ++i) work_[(size_t)i] = ring_[(size_t)((i0 + i) % R)];
                workSlotEnd_ = captureUtc;     // ★ when the window was complete, on the capture clock
                workSlotUtc_ = nextSlot_;      // ★ and the slot it is — the spots' time (WSJT-X's)
                pending = true;
                wcv.notify_one();
            } else {
                slotsSkipped.fetch_add(1);     // ★ the previous decode is still running: skip, never wait
            }
        }
        nextSlot_ += slotPeriod;
    }
}

void Ft8Decoder::runDecode() {
    const auto t0 = std::chrono::steady_clock::now();
    // ★ How late this decode starts after its window was complete (a queue that ran behind, a busy core).
    const double lagMs = std::max(0.0, std::min(slotPeriod * 1000.0, (vibe::vibeUtcNow() - workSlotEnd_) * 1000.0));
    Ft8SlotOptions o;
    o.maxPasses = std::max(1, std::min(3, maxPasses.load()));
    o.abort = &abortDecode;
    /* ★★ OSD only on spare CPU, like the passes: a loaded box decodes exactly as before (2026-10-05). 24 hard
     *  errors: on the real hour 30 let one garbage message through ("7Z1OSX ZF7IZT R QM53") and 36 two; 24
     *  none, for +1.7 % of jt9's decodes at +2 ms a slot on a Mac.
     *  ★★★ THE SENSITIVITY WORK (2026-10-26, docs/v12/FTX-SENSITIVITY.md). The second look runs on EVERY server: one
     *  pass of it (BP only) costs less than the old single pass did — 160 M against 334 M instructions a busy slot on
     *  the Pi 2 build — and decodes more than the old full depth. Spare CPU then buys, as before, the passes and OSD
     *  (now with pairs), and a-priori decoding. The book learns on every decode, loaded or not, so it is ready when the
     *  CPU is. */
    const bool loadedAtStart = loadProbe ? loadProbe() : false;
    const bool deep = !loadedAtStart && o.maxPasses >= 2;
    o.fine = 1;
    o.bpStall = kBpStall;
    o.onlyChanged = true;
    o.osdMaxErrors = deep && mode_ != FtxMode::FT2 ? kOsdMaxErrors : 0;   // ★ FT2: none (docs/v12/JS8-FT2.md)
    o.osdPairSpan = deep ? kOsdPairSpan : 0;
    o.ap = &apBook_;
    o.apTry = deep;
    o.mayRunPass = [&](int, double elapsedMs, double lastMs) {
        const bool loaded = loadProbe ? loadProbe() : false;
        return passAllowed(slotPeriod * 1000.0, lagMs, elapsedMs, lastMs, loaded);
    };
    std::vector<Ft8Result> res;
    Ft8SlotStats st;
    slot_.decode(work_.data(), window_, o, res, st);
    lastPasses.store(st.passes);
    lastDecodeMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    slotsDecoded.fetch_add(1);
    if (abortDecode.load()) return;

    for (const Ft8Result& r : res) {
        char callTo[24] = {}, callDe[24] = {}, grid[24] = {};
        ftx_field_t fields[FTX_MAX_MESSAGE_FIELDS];
        ftx_message_rc_t rc = ftx_message_decode_std(&r.msg, &g_hashIf, callTo, callDe, grid, fields);
        if (rc != FTX_MESSAGE_RC_OK) continue;
        std::string deCall;
        if (!spotCallsign(callDe, deCall)) continue;   // ★ empty or an unresolved "<...>": not a spot
        // ★★ ft8_lib's third field is "grid OR report": "IO92", but also "-12", "R-12", "73", "RRR" and
        //    "RR73". Only a real 4-character locator is a grid (B10: the map warned about ~40 "unparseable
        //    grids" on every update, and every RR73 sign-off — a VALID-LOOKING square near the North Pole —
        //    was plotted in the Arctic). Anything else is sent as no grid at all.
        const bool realGrid = std::strlen(grid) == 4
            && grid[0] >= 'A' && grid[0] <= 'R' && grid[1] >= 'A' && grid[1] <= 'R'
            && grid[2] >= '0' && grid[2] <= '9' && grid[3] >= '0' && grid[3] <= '9'
            && std::strcmp(grid, "RR73") != 0;
        if (!realGrid) grid[0] = 0;
        if (onSpot) onSpot(callTo, deCall, grid, r.snr, r.hz);
    }
}

} // namespace vibe
