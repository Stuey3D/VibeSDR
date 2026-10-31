// decoders/digimodes/msk144_code.cpp — the MSK144 frame and code (msk144_code.h for where they came from).
#include "msk144_code.h"
#include "ldpc_small.h"
#include "msk144_tables.h"

#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include "ft8/message.h"
}
#include "../ft8_decoder.h"   // ft8CallHashes(), ft8OsdPlausible(), Ft8Decoder::spotCallsign()

namespace vibe {
namespace msk144 {

namespace {
inline int bitOf(const uint8_t* p, int i) { return (p[i / 8] >> (7 - i % 8)) & 1; }
inline void setBit(uint8_t* p, int i, int b) { if (b) p[i / 8] |= (uint8_t)(0x80 >> (i % 8)); else p[i / 8] &= (uint8_t)~(0x80 >> (i % 8)); }

bool htLookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    const int bits = type == FTX_CALLSIGN_HASH_10_BITS ? 10 : type == FTX_CALLSIGN_HASH_12_BITS ? 12 : 22;
    return ft8CallHashes().lookup(bits, hash, callsign);
}
void htSave(const char* callsign, uint32_t n22) { ft8CallHashes().save(callsign, n22); }
ftx_callsign_hash_interface_t g_hash = {htLookup, htSave};
} // namespace

uint16_t crc13(const uint8_t* bits) {
    // ★ the augmented form, as solved: 77 bits then 6 + 13 zeros through the register
    uint32_t r = 0;
    for (int i = 0; i < 77 + 19; ++i) {
        r = (r << 1) | (i < 77 ? (bits[i] & 1u) : 0u);
        if (r & 0x2000u) r ^= 0x35D7u;
    }
    return (uint16_t)(r & 0x1FFFu);
}

const ldpc::Code& code() {
    static const ldpc::Code c = [] {
        std::vector<std::vector<int>> ch;
        for (const auto& row : kMskChecks) {
            std::vector<int> v;
            for (int16_t x : row) if (x >= 0) v.push_back(x);
            ch.push_back(v);
        }
        return ldpc::Code(kN, kK, ch);
    }();
    return c;
}

void encode(const uint8_t* bits77, uint8_t cw[kN]) {
    uint8_t info[kK];
    for (int i = 0; i < kMsg; ++i) info[i] = bits77[i] & 1;
    const uint16_t c = crc13(bits77);
    for (int i = 0; i < 13; ++i) info[kMsg + i] = (uint8_t)((c >> (12 - i)) & 1);
    code().encode(info, cw);
}

bool crcOk(const uint8_t cw[kN]) {
    uint16_t want = 0;
    for (int i = 0; i < 13; ++i) want = (uint16_t)((want << 1) | (cw[kMsg + i] & 1));
    return crc13(cw) == want;
}

void frameBits(const uint8_t cw[kN], uint8_t f[kFrameBits]) {
    for (int i = 0; i < kFrameBits; ++i) if (isSync(i)) f[i] = (uint8_t)syncBit(i);
    for (int j = 0; j < kN; ++j) f[framePos(j)] = cw[j] & 1;
}

void tones(const uint8_t cw[kN], uint8_t t[kFrameBits]) {
    uint8_t f[kFrameBits], C[kFrameBits];
    frameBits(cw, f);
    for (int i = 0; i < kFrameBits; ++i) C[i] = (uint8_t)(f[i] ^ ((i & 2) ? 1 : 0));
    for (int i = 0; i < kFrameBits; ++i) t[i] = (uint8_t)(C[i] ^ C[(i + 1) % kFrameBits]);
}

bool encodeText(const char* text, uint8_t t[kFrameBits]) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, &g_hash, text) != FTX_MESSAGE_RC_OK) return false;
    uint8_t b[kMsg];
    for (int i = 0; i < kMsg; ++i) b[i] = (uint8_t)bitOf(m.payload, i);
    uint8_t cw[kN];
    encode(b, cw);
    tones(cw, t);
    return true;
}

bool unpack(const uint8_t cw[kN], bool learn, std::string& text, std::string& call, std::string& grid, bool& plausible) {
    text.clear(); call.clear(); grid.clear(); plausible = false;
    ftx_message_t m; ftx_message_init(&m);
    std::memset(m.payload, 0, sizeof m.payload);
    for (int i = 0; i < kMsg; ++i) setBit(m.payload, i, cw[i]);
    char txt[FTX_MAX_MESSAGE_LENGTH] = {};
    ftx_message_offsets_t offs;
    if (ftx_message_decode(&m, learn ? &g_hash : nullptr, txt, &offs) != FTX_MESSAGE_RC_OK) return false;
    text = txt;
    plausible = ft8OsdPlausible(m);
    if (ftx_message_get_type(&m) == FTX_MESSAGE_TYPE_STANDARD || ftx_message_get_i3(&m) == 2) {
        char to[24] = {}, de[24] = {}, ex[24] = {};
        ftx_field_t fl[FTX_MAX_MESSAGE_FIELDS];
        if (ftx_message_decode_std(&m, learn ? &g_hash : nullptr, to, de, ex, fl) == FTX_MESSAGE_RC_OK) {
            std::string cs;
            if (Ft8Decoder::spotCallsign(de, cs)) call = cs;
            const bool realGrid = std::strlen(ex) == 4 && ex[0] >= 'A' && ex[0] <= 'R' && ex[1] >= 'A' && ex[1] <= 'R'
                && ex[2] >= '0' && ex[2] <= '9' && ex[3] >= '0' && ex[3] <= '9' && std::strcmp(ex, "RR73") != 0;
            if (realGrid) grid = ex;
        }
    }
    return true;
}

} // namespace msk144
} // namespace vibe
