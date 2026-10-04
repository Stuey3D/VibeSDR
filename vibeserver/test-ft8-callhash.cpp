// test-ft8-callhash.cpp — FT8's hashed-callsign table and the spot filter (audit 2026-10-04, row 10).
//
// ★★ WHY. A non-standard call goes over the air as a 10/12/22-bit hash, resolved from a table of calls heard in full.
//    The old table (512 entries) never forgot and stopped saving when full — every decoded call is saved, so a busy
//    band filled it within hours and nothing heard later could be resolved; it returned the FIRST call matching a
//    10-bit hash, so a collision printed somebody else's call; and an unresolved "<...>" became an EMPTY-callsign spot
//    once the host's whitelist had stripped the brackets. Checked here on the table itself, then through ft8_lib's
//    real pack/unpack so the hash values are the ones on the air.
#include "decoders/ft8_decoder.h"
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "ft8/message.h"
}

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

static bool lk(ftx_callsign_hash_type_t t, uint32_t h, char* c) {
    return ft8CallHashes().lookup(t == FTX_CALLSIGN_HASH_10_BITS ? 10 : t == FTX_CALLSIGN_HASH_12_BITS ? 12 : 22, h, c);
}
static void sv(const char* c, uint32_t n22) { ft8CallHashes().save(c, n22); }
static ftx_callsign_hash_interface_t hashIf = { lk, sv };

int main() {
    auto& t = ft8CallHashes();
    const int N = Ft8CallHashTable::kCapacity;
    char out[12];

    std::printf("── 1. least recently used: a full table keeps learning ──\n");
    t.clear();
    // 22-bit hashes spaced so no two share a 10-bit hash until the table wraps: i << 12 | i
    auto call = [](int i) { char b[12]; std::snprintf(b, sizeof b, "XX%04dZ", i); return std::string(b); };
    auto n22 = [](int i) { return (uint32_t)(((i % 1024) << 12) | (i & 0xFFF)); };
    for (int i = 0; i < N + 100; i++) t.save(call(i).c_str(), n22(i));
    ok(t.size() == N, "holds " + std::to_string(t.size()) + " after " + std::to_string(N + 100) + " saves (capacity " + std::to_string(N) + ")");
    bool newest = true;
    for (int i = 100; i < N + 100; i++) newest &= t.lookup(22, n22(i), out) && call(i) == out;
    ok(newest, "every one of the newest " + std::to_string(N) + " resolves (old table: the last 100 never saved)");
    bool oldestGone = true;
    for (int i = 0; i < 100; i++) oldestGone &= !t.lookup(22, n22(i), out) && out[0] == 0;
    ok(oldestGone, "the 100 oldest were evicted");
    // a lookup refreshes: touch the oldest survivor, add one more, and the NEXT oldest goes instead
    ok(t.lookup(22, n22(100), out), "the oldest survivor resolves (and is now recent)");
    t.save("NEWCALL", n22(9000 % 1024 + 2048));
    ok(t.lookup(22, n22(100), out) && call(100) == out, "...so it survives the next eviction");
    ok(!t.lookup(22, n22(101), out), "...and the next oldest is the one evicted");

    std::printf("── 2. an AMBIGUOUS hash is unknown, never a guess ──\n");
    t.clear();
    const uint32_t a = (0x155u << 12) | 0x001, b = (0x155u << 12) | 0x802;   // same 10-bit hash, different 12/22
    t.save("PJ4/K1ABC", a); t.save("VP2V/W1XYZ", b);
    ok(!t.lookup(10, 0x155, out) && out[0] == 0, "two calls under one 10-bit hash → unknown (old: the first one)");
    ok(t.lookup(12, a >> 10, out) && std::strcmp(out, "PJ4/K1ABC") == 0, "the 12-bit hashes differ → each resolves");
    ok(t.lookup(22, b, out) && std::strcmp(out, "VP2V/W1XYZ") == 0, "...and so do the 22-bit ones");
    t.save("PJ4/K1ABC", a);
    ok(t.size() == 2, "saving a call again does not duplicate it");
    t.clear(); t.save("PJ4/K1ABC", a);
    ok(t.lookup(10, 0x155, out) && std::strcmp(out, "PJ4/K1ABC") == 0, "one call under the hash → resolves");
    t.save("XXXXXXXXXXX", a + 1);   // 11 characters: the buffer is char[12]
    ok(t.lookup(22, a + 1, out) && std::strlen(out) == 11, "an 11-character call comes back terminated");

    std::printf("── 3. the spot filter ──\n");
    std::string s;
    ok(!Ft8Decoder::spotCallsign("", s), "empty → no spot");
    ok(!Ft8Decoder::spotCallsign("<...>", s), "\"<...>\" (unresolved) → no spot (old: an empty-callsign spot)");
    ok(Ft8Decoder::spotCallsign("<PJ4/K1ABC>", s) && s == "PJ4/K1ABC", "\"<PJ4/K1ABC>\" → PJ4/K1ABC");
    ok(Ft8Decoder::spotCallsign("G4ABC", s) && s == "G4ABC", "G4ABC → G4ABC");
    ok(!Ft8Decoder::spotCallsign("<G4ABC", s) && !Ft8Decoder::spotCallsign("G4<X>", s) && !Ft8Decoder::spotCallsign("<>", s),
       "anything else with '<' or '>' → no spot");

    std::printf("── 4. through ft8_lib: the hashes on the air ──\n");
    t.clear();
    ftx_message_t m;
    const bool enc = ftx_message_encode(&m, &hashIf, "W9XYZ PJ4/K1ABC -11") == FTX_MESSAGE_RC_OK;
    ok(enc && ftx_message_get_type(&m) == FTX_MESSAGE_TYPE_STANDARD, "encodes \"W9XYZ PJ4/K1ABC -11\" as a standard message, DE hashed");
    char to[24], de[24], ex[24]; ftx_field_t f[FTX_MAX_MESSAGE_FIELDS];
    t.clear();
    ok(ftx_message_decode_std(&m, &hashIf, to, de, ex, f) == FTX_MESSAGE_RC_OK && std::strcmp(de, "<...>") == 0,
       std::string("never heard in full: de = ") + de);
    ok(!Ft8Decoder::spotCallsign(de, s), "...and it is not spotted");
    ftx_message_t full;
    // heard in full: a type-4 message carries the non-standard call as plain text (58 bits)
    ok(ftx_message_encode_nonstd(&full, &hashIf, "W9XYZ", "PJ4/K1ABC", "RRR") == FTX_MESSAGE_RC_OK, "encodes the call in full (type 4)");
    t.clear();
    char text[64]; ftx_message_offsets_t offs;
    ftx_message_decode(&full, &hashIf, text, &offs);        // ★ the RECEIVER learns it by decoding it
    ok(ftx_message_decode_std(&m, &hashIf, to, de, ex, f) == FTX_MESSAGE_RC_OK && std::strcmp(de, "<PJ4/K1ABC>") == 0,
       std::string("after hearing it in full: de = ") + de);
    ok(Ft8Decoder::spotCallsign(de, s) && s == "PJ4/K1ABC", "...spotted as PJ4/K1ABC");

    if (fails) { std::printf("  %d FAILED\n", fails); return 1; }
    std::printf("  all passed\n");
    return 0;
}
