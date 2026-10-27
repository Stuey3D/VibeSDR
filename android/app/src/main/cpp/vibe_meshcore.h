// vibe_meshcore.h — a MeshCore packet: header, route, path, and the payloads that are public by design — node
// ADVERTS (signed, never encrypted: name, kind, position) and group text on the built-in PUBLIC channel.
// docs/v12/LORA-MESH.md §MeshCore.
//
// Sources: MeshCore's own documentation (github.com/meshcore-dev/MeshCore docs/packet_format.md, payloads.md,
// companion_protocol.md, faq.md): the header byte (route type bits 0-1, payload type 2-5, version 6-7), transport codes,
// the path-length byte (hop count + hash size), the advert's fields and flags, group text's channel hash (the first byte
// of SHA-256 of the channel key), its 2-byte MAC and "<sender>: <message>" plaintext, and the Public channel's
// published key 8b3387e9c5cdea6ac9e5edbaa115cd72. The radio presets from MeshCore's own preset list (api.meshcore.nz
// config): EU/UK (Narrow) 869.618 MHz SF8 BW62.5 CR4/8.
// ASSUMED (the docs name a MAC and a cipher block but not the algorithms): AES-128 in ECB over zero-padded blocks, the
// MAC the first two bytes of HMAC-SHA-256 keyed with the 16-byte secret zero-padded to 32. A packet whose MAC does not
// verify is never shown as text — so an assumption that is wrong shows nothing, never garbage.
//
// ★★★ PRIVACY (Stuart, 2026-10-27): adverts are broadcast in the clear and signed so that everyone may read them; the
//     Public channel's key is published ("messages should be considered public"). Everything else — direct messages,
//     requests, responses, other group channels (including hashtag channels, whose names we do not guess) — is listed by
//     its HEADER only (type, route, hops). No key is guessed or searched for.
#pragma once
#include "vibe_lora_msg.h"
#include "vibe_lora_crypto.h"
#include <cstdio>

namespace vibelora {
namespace meshcore {

inline const std::vector<uint8_t>& publicKey() {
    static const std::vector<uint8_t> k = crypto::fromHex("8b3387e9c5cdea6ac9e5edbaa115cd72");
    return k;
}
inline int publicHash() {
    static const int h = [] { uint8_t d[32]; crypto::sha256(publicKey().data(), 16, d); return int(d[0]); }();
    return h;
}
inline const char* typeName(int t) {
    static const char* n[16] = {"request", "response", "direct message", "ack", "advert", "group message", "group data",
                                "anonymous request", "path", "trace", "multipart", "control", "type 12", "type 13", "type 14", "custom"};
    return n[t & 15];
}
inline const char* advKind(int f) {
    switch (f & 15) { case 1: return "chat node"; case 2: return "repeater"; case 3: return "room server"; case 4: return "sensor"; default: return "node"; }
}

/** Open a group-text payload with the Public key. True + `text` (and `from`) when the MAC verifies. */
inline bool openPublic(const uint8_t* p, size_t n, std::string& from, std::string& text, int64_t* ts = nullptr) {
    if (n < 3 + 16 || (n - 3) % 16) return false;
    if (p[0] != publicHash()) return false;
    uint8_t key32[32] = {0};
    std::memcpy(key32, publicKey().data(), 16);
    uint8_t mac[32];
    crypto::hmacSha256(key32, 32, p + 3, n - 3, mac);
    if (mac[0] != p[1] || mac[1] != p[2]) return false;
    crypto::Aes128 a(publicKey().data());
    std::vector<uint8_t> pt(n - 3);
    for (size_t i = 0; i + 16 <= pt.size(); i += 16) a.decrypt(p + 3 + i, pt.data() + i);
    if (pt.size() < 6) return false;
    if (ts) *ts = int64_t(pt[0] | pt[1] << 8 | pt[2] << 16 | uint32_t(pt[3]) << 24);
    if ((pt[4] >> 2) != 0) return false;                // ★ only a plain text message (txt_type 0)
    size_t end = 5; while (end < pt.size() && pt[end]) ++end;   // zero padding ends it
    const std::string all = cleanText(pt.data() + 5, end - 5);
    const size_t c = all.find(": ");
    if (c != std::string::npos && c < 40) { from = all.substr(0, c); text = all.substr(c + 2); }
    else text = all;
    return true;
}

inline bool parse(const std::vector<uint8_t>& b, Msg& m) {
    if (b.size() < 2) return false;
    const int hdr = b[0], route = hdr & 3, type = hdr >> 2 & 15, ver = hdr >> 6;
    if (ver != 0) return false;                          // ★ only version 1 is defined
    size_t i = 1;
    if (route == 0 || route == 3) i += 4;                // transport codes
    if (i >= b.size()) return false;
    const int pl = b[i++], hops = pl & 63, hsz = (pl >> 6) + 1;
    if (hsz == 4) return false;
    const size_t plen = size_t(hops) * size_t(hsz);
    if (plen > 64 || i + plen > b.size()) return false;
    i += plen;
    const uint8_t* p = b.data() + i; const size_t n = b.size() - i;
    m.proto = "meshcore";
    m.route = (route == 1 || route == 0) ? "flood" : "direct";
    m.pathLen = hops;
    m.what = typeName(type);
    if (type == 4) {                                     // ADVERT: pubkey 32, timestamp 4, signature 64, appdata
        if (n < 100 + 1) return false;
        m.node = crypto::toHex(p, 4);                    // ★ the key's first 4 bytes, as MeshCore's apps show a node
        const uint8_t fl = p[100];
        size_t a = 101;
        m.role = advKind(fl);
        if (fl & 0x10) {
            if (a + 8 > n) return false;
            const int32_t la = int32_t(uint32_t(p[a]) | uint32_t(p[a + 1]) << 8 | uint32_t(p[a + 2]) << 16 | uint32_t(p[a + 3]) << 24);
            const int32_t lo = int32_t(uint32_t(p[a + 4]) | uint32_t(p[a + 5]) << 8 | uint32_t(p[a + 6]) << 16 | uint32_t(p[a + 7]) << 24);
            a += 8;
            if ((la || lo) && std::abs(la) <= 90000000 && std::abs(lo) <= 180000000) { m.lat = la * 1e-6; m.lon = lo * 1e-6; }
        }
        if (fl & 0x20) a += 2;
        if (fl & 0x40) a += 2;
        if ((fl & 0x80) && a < n) m.longName = cleanText(p + a, n - a, 32);
        m.what = "advert";
        return true;
    }
    if (type == 5) {                                     // GROUP TEXT
        m.chHash = n ? p[0] : -1;
        std::string from, text;
        if (openPublic(p, n, from, text)) { m.what = "text"; m.channel = "Public"; m.from = from; m.text = text; m.broadcast = true; return true; }
        m.privateCh = true; m.what = "group message";
        return true;
    }
    // ★ everything else: header only (requests, responses, direct messages, acks, paths, traces …)
    m.privateCh = type != 3 && type != 9 && type != 8 && type != 11;
    return true;
}

/** "MeshCore · Northampton Rptr · repeater · advert" / "MeshCore · Public · Alice: hello". */
inline std::string line(const Msg& m) {
    const std::string d = mkDot();
    if (m.what == "advert") {
        std::string s = "MeshCore" + d + (m.longName.empty() ? m.node : m.longName) + d + m.role;
        if (m.lat == m.lat) s += d + "position";
        return s;
    }
    if (m.what == "text") return "MeshCore" + d + "Public" + d + (m.from.empty() ? "" : m.from + ": ") + "\xe2\x80\x9c" + m.text + "\xe2\x80\x9d";
    std::string s = "MeshCore" + d + m.what + d + m.route;
    if (m.pathLen > 0) s += d + std::to_string(m.pathLen) + (m.pathLen == 1 ? " hop" : " hops");
    if (m.privateCh) s += d + "encrypted";
    return s;
}

} // namespace meshcore
} // namespace vibelora
