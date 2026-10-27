// vibe_lora_msg.h — what one LoRa frame SAID, after the protocol parsers (vibe_lorawan.h, vibe_meshtastic.h,
// vibe_meshcore.h) and their privacy rules. The node table (vibe_lora_track.h) and the clients see only this.
//
// ★★★ PRIVACY IS DECIDED BEFORE THIS STRUCT IS FILLED (docs/v12/LORA-MESH.md §Privacy). There is no field here an
//     encrypted payload's bytes could travel in: LoRaWAN carries its length only; a mesh packet that does not open with
//     the network's PUBLISHED key carries its header only (`privateCh`).
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace vibelora {

struct Msg {
    std::string proto;        ///< "lorawan" | "meshtastic" | "meshcore" | "lora" (an unrecognised LoRa frame)
    std::string node;         ///< the sender as the network names it: "!a1b2c3d4" · MeshCore key prefix "a1b2c3d4" · DevAddr "260B1234"
    std::string what;         ///< "text", "position", "nodeinfo", "telemetry", "advert", "uplink", "join", "private", …
    // ── a node's own description (mesh networks publish these) ──
    std::string longName, shortName, hw, role;
    double lat = NAN, lon = NAN, altM = NAN;
    int precisionBits = 0;    ///< Meshtastic: how many bits of the position were kept (it is often deliberately coarse)
    int batt = -1;            ///< %, 101 = powered
    double volt = NAN, tempC = NAN, hum = NAN, hpa = NAN, chUtil = NAN, airUtil = NAN;
    int64_t uptimeS = -1;
    // ── a public message (published channel only) ──
    std::string text, channel, from;   ///< `from`: MeshCore's sender name as written in the message (unverified by design)
    // ── routing ──
    int hopLimit = -1, hopStart = -1;
    uint32_t to = 0; bool broadcast = false; bool privateCh = false; int chHash = -1;
    std::string route;        ///< MeshCore: "flood" / "direct"; Meshtastic: "via MQTT"
    int pathLen = -1;
    // ── LoRaWAN ──
    std::string mtype;        ///< "Unconfirmed up", "Confirmed up", "Join request", …
    bool uplink = true; int fcnt = -1, fport = -1, foptsLen = 0, encLen = -1;
    bool adr = false, ack = false, fpending = false, adrAckReq = false;
    std::string net;          ///< "The Things Network" when the DevAddr says so
    std::string joinEui, devEui;
    // ── the radio side (filled by the service) ──
    int sf = 0; double bw = 0; int cr = 0; uint8_t sync = 0; int len = 0;
    double freqHz = 0; float snrDb = 0, levelDb = -99;
};

inline std::string mkDot() { return " \xc2\xb7 "; }

/** Printable text from bytes a stranger chose: valid UTF-8 kept, control characters dropped, at most `max` bytes. */
inline std::string cleanText(const uint8_t* p, size_t n, size_t max = 240) {
    std::string s;
    for (size_t i = 0; i < n && s.size() < max;) {
        const uint8_t c = p[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + size_t(len) > n) { ++i; continue; }
        bool ok = true;
        for (int k = 1; k < len; ++k) if ((p[i + size_t(k)] & 0xC0) != 0x80) ok = false;
        if (!ok) { ++i; continue; }
        if (len == 1 && (c < 0x20 || c == 0x7F)) { if (c == '\n' || c == '\t') s += ' '; ++i; continue; }
        s.append(reinterpret_cast<const char*>(p + i), size_t(len));
        i += size_t(len);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  A protobuf reader — just the wire format (varint, 64-bit, length-delimited, 32-bit), from the Protocol Buffers
//  encoding documentation. Bounds-checked: a stranger's packet must never read past its end.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Pb {
    const uint8_t* p; size_t n; size_t i = 0; bool bad = false;
    Pb(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool varint(uint64_t& v) {
        v = 0;
        for (int s = 0; s < 64; s += 7) {
            if (i >= n) { bad = true; return false; }
            const uint8_t b = p[i++];
            v |= uint64_t(b & 0x7F) << s;
            if (!(b & 0x80)) return true;
        }
        bad = true; return false;
    }
    /** Next field: number, wire type, and (for 2) the bytes; (0/1/5) the value. False at the end or on damage. */
    bool next(int& field, int& wt, uint64_t& val, const uint8_t*& bytes, size_t& blen) {
        if (i >= n || bad) return false;
        uint64_t key;
        if (!varint(key)) return false;
        field = int(key >> 3); wt = int(key & 7);
        if (field == 0) { bad = true; return false; }
        switch (wt) {
            case 0: return varint(val);
            case 1: if (i + 8 > n) { bad = true; return false; } val = 0; for (int k = 7; k >= 0; --k) val = val << 8 | p[i + size_t(k)]; i += 8; return true;
            case 5: if (i + 4 > n) { bad = true; return false; } val = uint64_t(p[i]) | uint64_t(p[i + 1]) << 8 | uint64_t(p[i + 2]) << 16 | uint64_t(p[i + 3]) << 24; i += 4; return true;
            case 2: { uint64_t l; if (!varint(l) || l > n - i) { bad = true; return false; } bytes = p + i; blen = size_t(l); i += size_t(l); return true; }
            default: bad = true; return false;
        }
    }
};

} // namespace vibelora
