// decoders/digimodes/fst4_code.cpp — the FST4 / FST4W frame and codes (fst4_code.h for where they came from).
#include "fst4_code.h"
#include "fst4_tables.h"
#include "ldpc_small.h"
#include <functional>
#include <cstdlib>
#include <vector>

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "ft8/constants.h"
#include "ft8/message.h"
}
#include "../ft8_decoder.h"   // ft8CallHashes(), ft8OsdPlausible(), Ft8Decoder::spotCallsign()

namespace vibe {
namespace fst4 {

namespace {
const uint8_t kSyncA[8] = {0, 1, 3, 2, 1, 0, 2, 3};
const uint8_t kSyncB[8] = {2, 3, 1, 0, 3, 2, 0, 1};
const int kGroupStart[5] = {0, 38, 76, 114, 152};
const uint8_t kPowers[19] = {0, 3, 7, 10, 13, 17, 20, 23, 27, 30, 33, 37, 40, 43, 47, 50, 53, 57, 60};

int groupOf(int i) { for (int g = 0; g < 5; ++g) if (i >= kGroupStart[g] && i < kGroupStart[g] + 8) return g; return -1; }

bool htLookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    const int bits = type == FTX_CALLSIGN_HASH_10_BITS ? 10 : type == FTX_CALLSIGN_HASH_12_BITS ? 12 : 22;
    return ft8CallHashes().lookup(bits, hash, callsign);
}
void htSave(const char* callsign, uint32_t n22) { ft8CallHashes().save(callsign, n22); }
ftx_callsign_hash_interface_t g_hash = {htLookup, htSave};

inline int bitOf(const uint8_t* p, int i) { return (p[i / 8] >> (7 - i % 8)) & 1; }
inline void setBit(uint8_t* p, int i, int b) { if (b) p[i / 8] |= (uint8_t)(0x80 >> (i % 8)); else p[i / 8] &= (uint8_t)~(0x80 >> (i % 8)); }

/** The bits of "CQ <call> <grid>" — FST4W's call and grid fields are exactly the standard message's. */
bool standardCqBits(const char* call, const char* grid, uint8_t payload[10]) {
    ftx_message_t m; ftx_message_init(&m);
    char txt[40]; std::snprintf(txt, sizeof txt, "CQ %s %s", call, grid);
    if (ftx_message_encode(&m, nullptr, txt) != FTX_MESSAGE_RC_OK || ftx_message_get_i3(&m) != 1) return false;
    std::memcpy(payload, m.payload, 10);
    return true;
}
} // namespace

bool isSync(int i) { return groupOf(i) >= 0; }
int syncTone(int i) { const int g = groupOf(i); if (g < 0) return -1; return (g & 1) ? kSyncB[i - kGroupStart[g]] : kSyncA[i - kGroupStart[g]]; }

uint32_t crc24(const uint8_t* bits, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t fb = ((r >> 23) & 1u) ^ (bits[i] & 1u);
        r = (r << 1) & 0xFFFFFFu;
        if (fb) r ^= 0x65Bu;
    }
    return r;
}

void encode(Code c, const uint8_t* msg, uint8_t cw[kN]) {
    const int K = c == Code::K101 ? 77 : 50;
    uint8_t acc[30] = {};
    for (int i = 0; i < K; ++i) {
        if (!(msg[i] & 1)) continue;
        const uint8_t* row = c == Code::K101 ? kFst4Gen101[i] : kFst4Gen74[i];
        for (int b = 0; b < 30; ++b) acc[b] ^= row[b];
    }
    for (int j = 0; j < kN; ++j) cw[j] = (uint8_t)bitOf(acc, j);
}

void codewordToTones(const uint8_t cw[kN], uint8_t tones[kSymbols]) {
    static const uint8_t gray[4] = {0, 1, 3, 2};   // bits (b0 b1) 00 01 10 11 → tones 0 1 3 2
    int d = 0;
    for (int i = 0; i < kSymbols; ++i) {
        if (isSync(i)) { tones[i] = (uint8_t)syncTone(i); continue; }
        const int b0 = cw[2 * d], b1 = cw[2 * d + 1];
        tones[i] = gray[(b0 << 1) | b1];
        ++d;
    }
}

bool encodeText(const char* text, uint8_t tones[kSymbols]) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, &g_hash, text) != FTX_MESSAGE_RC_OK) return false;
    uint8_t msg[77];
    for (int i = 0; i < 77; ++i) msg[i] = (uint8_t)(bitOf(m.payload, i) ^ bitOf(kFT4_XOR_sequence, i));
    uint8_t cw[kN];
    encode(Code::K101, msg, cw);
    codewordToTones(cw, tones);
    return true;
}

bool encodeWspr(const char* call, const char* grid, int dbm, uint8_t tones[kSymbols]) {
    uint8_t p[10];
    if (!standardCqBits(call, grid, p)) return false;
    int best = 0;
    for (int k = 1; k < 19; ++k) if (std::abs(kPowers[k] - dbm) < std::abs(kPowers[best] - dbm)) best = k;
    uint8_t msg[50];
    for (int i = 0; i < 28; ++i) msg[i] = (uint8_t)bitOf(p, 29 + i);
    for (int i = 0; i < 15; ++i) msg[28 + i] = (uint8_t)bitOf(p, 59 + i);
    for (int i = 0; i < 5; ++i) msg[43 + i] = (uint8_t)((best >> (4 - i)) & 1);
    msg[48] = msg[49] = 0;
    uint8_t cw[kN];
    encode(Code::K74, msg, cw);
    codewordToTones(cw, tones);
    return true;
}

bool crcOk(Code c, const uint8_t cw[kN]) {
    const int K = c == Code::K101 ? 77 : 50;
    uint32_t want = 0;
    for (int i = 0; i < 24; ++i) want = (want << 1) | (cw[K + i] & 1u);
    return crc24(cw, K) == want;
}

const ldpc::Code& ldpcOf(Code c) {
    auto build = [](const auto& tab, int k) {
        std::vector<std::vector<int>> ch;
        for (const auto& row : tab) { std::vector<int> v; for (int16_t x : row) if (x >= 0) v.push_back(x); ch.push_back(v); }
        return ldpc::Code(kN, k, ch);
    };
    static const ldpc::Code c101 = build(kFst4Check101, 101);
    static const ldpc::Code c74 = build(kFst4Check74, 74);
    return c == Code::K101 ? c101 : c74;
}

int bpDecode(Code c, const float llr[kN], int maxIter, uint8_t cw[kN], int* unsat, float* post) {
    // ★ the shared small-LDPC sum-product (ldpc_small.h: SIMD, identical scalar), the CRC on top
    const int it = ldpcOf(c).bp(llr, maxIter, cw, unsat, 12, post);
    return it > 0 && crcOk(c, cw) ? it : 0;
}

static const int kOsdTries = std::getenv("FST4_OSDTRIES") ? std::atoi(std::getenv("FST4_OSDTRIES")) : 3000;
bool osdDecode(Code c, const float llr[kN], int pairSpan, const std::function<bool(const uint8_t*)>& accept, uint8_t cw[kN], int* flips) {
    // ★ up to 1500 trials: the CRC-24 leaves ≈ 1500 / 2²⁴ ≈ 1e-4 a decode to the message checks after it
    return ldpcOf(c).osd(llr, pairSpan, [&](const uint8_t* x) { return crcOk(c, x) && accept(x); }, cw, nullptr, flips, 0, kOsdTries);
}

bool unpack77(const uint8_t cw[kN], bool learn, std::string& text, std::string& call, std::string& grid, bool& plausible) {
    text.clear(); call.clear(); grid.clear(); plausible = false;
    ftx_message_t m; ftx_message_init(&m);
    std::memset(m.payload, 0, sizeof m.payload);
    for (int i = 0; i < 77; ++i) setBit(m.payload, i, cw[i] ^ bitOf(kFT4_XOR_sequence, i));
    char txt[FTX_MAX_MESSAGE_LENGTH] = {};
    ftx_message_offsets_t offs;
    if (ftx_message_decode(&m, learn ? &g_hash : nullptr, txt, &offs) != FTX_MESSAGE_RC_OK) return false;
    text = txt;
    plausible = ft8OsdPlausible(m);
    if (ftx_message_get_type(&m) == FTX_MESSAGE_TYPE_STANDARD || ftx_message_get_i3(&m) == 2) {
        char to[24] = {}, de[24] = {}, ex[24] = {};
        ftx_field_t f[FTX_MAX_MESSAGE_FIELDS];
        if (ftx_message_decode_std(&m, learn ? &g_hash : nullptr, to, de, ex, f) == FTX_MESSAGE_RC_OK) {
            std::string cs;
            if (Ft8Decoder::spotCallsign(de, cs)) call = cs;
            const bool realGrid = std::strlen(ex) == 4 && ex[0] >= 'A' && ex[0] <= 'R' && ex[1] >= 'A' && ex[1] <= 'R'
                && ex[2] >= '0' && ex[2] <= '9' && ex[3] >= '0' && ex[3] <= '9' && std::strcmp(ex, "RR73") != 0;
            if (realGrid) grid = ex;
        }
    }
    return true;
}

bool unpack50(const uint8_t cw[kN], std::string& text, std::string& call, std::string& grid, int& dbm) {
    text.clear(); call.clear(); grid.clear(); dbm = 0;
    if (cw[48] || cw[49]) return false;            // ★ a compound call: layout not solved (fst4_code.h)
    int pi = 0; for (int i = 0; i < 5; ++i) pi = (pi << 1) | cw[43 + i];
    if (pi > 18) return false;
    uint8_t p[10];
    if (!standardCqBits("K1ABC", "FN42", p)) return false;   // the CQ token, ipa, R and i3 = 1 around our fields
    for (int i = 0; i < 28; ++i) setBit(p, 29 + i, cw[i]);
    setBit(p, 57, 0); setBit(p, 58, 0);
    for (int i = 0; i < 15; ++i) setBit(p, 59 + i, cw[28 + i]);
    ftx_message_t m; ftx_message_init(&m); std::memcpy(m.payload, p, 10);
    char to[24] = {}, de[24] = {}, ex[24] = {};
    ftx_field_t f[FTX_MAX_MESSAGE_FIELDS];
    if (ftx_message_decode_std(&m, nullptr, to, de, ex, f) != FTX_MESSAGE_RC_OK) return false;
    std::string cs;
    if (!Ft8Decoder::spotCallsign(de, cs)) return false;
    if (cs == "DE" || cs == "QRZ" || cs == "CQ" || cs.find_first_of("0123456789") == std::string::npos) return false;   // a token, not a call
    const bool realGrid = std::strlen(ex) == 4 && ex[0] >= 'A' && ex[0] <= 'R' && ex[1] >= 'A' && ex[1] <= 'R'
        && ex[2] >= '0' && ex[2] <= '9' && ex[3] >= '0' && ex[3] <= '9';
    if (!realGrid) return false;
    call = cs; grid = ex; dbm = kPowers[pi];
    text = call + " " + grid + " " + std::to_string(dbm);
    return true;
}

} // namespace fst4
} // namespace vibe
