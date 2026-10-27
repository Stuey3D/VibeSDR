// vibe_meshtastic.h — a Meshtastic packet: the 16-byte header every packet carries in the clear, and — ONLY for the
// published default channel — the payload: node info, positions, telemetry and public text.
// docs/v12/LORA-MESH.md §Meshtastic.
//
// Sources: meshtastic.org (docs/overview/mesh-algo: the on-air header, sync word 0x2B, 16-symbol preamble below 1 GHz;
// docs/overview/encryption: AES-CTR, "AQ==" is the published default key; docs/overview/radio-settings: the modem
// presets and EU_868 869.4-869.65 MHz). The message layout (Data, User, Position, Telemetry, PortNum, HardwareModel)
// from the public protobuf definitions (meshtastic/protobufs — GPL-3, READ for the field numbers; this parser is ours).
// ASSUMED from the firmware's documented behaviour, confirmed by test vectors here and to be confirmed on air: the
// default key's 16 bytes, the channel-hash rule (XOR of the name's bytes XOR the key's) and the CTR nonce (packet id
// as 64-bit little-endian, then the sender's node number, then zeros).
//
// ★★★ PRIVACY (Stuart, 2026-10-27: "show the public messages since they are accessible by design"). Decrypted ONLY with
//     the published default key, and only on a channel whose hash says it is a default-named channel. Anything else —
//     a private channel, a direct message under PKI (channel hash 0) — is listed by its HEADER: from, to, channel hash,
//     hop limit. No key is guessed or searched for. A text sent to ONE node (not broadcast) is not shown even when it
//     opens with the default key: it was addressed to someone, not published.
#pragma once
#include "vibe_lora_msg.h"
#include "vibe_lora_crypto.h"
#include <cstdio>

namespace vibelora {
namespace meshtastic {

/** The published default key ("AQ==" = default key #1). */
inline const uint8_t* defaultKey() {
    static const uint8_t k[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
    return k;
}
/** The default channel names — the modem presets' own names, which a default primary channel takes. */
inline const std::vector<std::string>& defaultNames() {
    static const std::vector<std::string> n = {"LongFast", "LongSlow", "LongModerate", "LongMod", "LongTurbo", "MediumSlow",
                                               "MediumFast", "MediumTurbo", "ShortSlow", "ShortFast", "ShortTurbo", "VLongSlow"};
    return n;
}
inline int xorHash(const uint8_t* p, size_t n) { uint8_t h = 0; for (size_t i = 0; i < n; ++i) h ^= p[i]; return h; }
inline int channelHash(const std::string& name, const uint8_t* key, size_t klen) {
    return xorHash(reinterpret_cast<const uint8_t*>(name.data()), name.size()) ^ xorHash(key, klen);
}
/** The slot a channel name lands on: the djb2 string hash of the name, modulo the slots the region has (radio-settings'
 *  "channel hashing algorithm" — US LongFast → slot 20, 906.875 MHz; EU_433 → 433.875 MHz; test-lora checks both). */
inline uint32_t slotHash(const std::string& s) { uint32_t h = 5381; for (unsigned char c : s) h = h * 33 + c; return h; }

inline const char* hwName(uint64_t v) {
    static const char* t[] = {"unset", "T-LoRa V2", "T-LoRa V1", "T-LoRa V2.1-1.6", "T-Beam", "Heltec V2.0", "T-Beam 0.7", "T-Echo",
        "T-LoRa V1.1.3", "RAK4631", "Heltec V2.1", "Heltec V1", "T-Beam S3 Core", "RAK11200", "Nano G1", "T-LoRa V2.1-1.8",
        "T-LoRa T3-S3", "Nano G1 Explorer", "Nano G2 Ultra", "LoRa Type", "WiPhone", "Wio WM1110", "RAK2560", "Heltec HRU-3601",
        "Heltec Wireless Bridge", "Station G1", "RAK11310", "Makerfabs tracker", "Makerfabs", "CanaryOne", "RP2040 LoRa",
        "Station G2", "LoRa Relay V1", "T-Echo Plus", "PPR", "GenieBlocks", "nRF52", "Linux (portduino)", "Android simulator",
        "DIY V1", "nRF52840 PCA10059", "DR-DEV", "M5Stack", "Heltec V3", "Heltec Wireless Stick Lite V3", "BetaFPV 2400 TX",
        "BetaFPV 900 Nano TX", "Raspberry Pi Pico", "Heltec Wireless Tracker", "Heltec Wireless Paper", "T-Deck", "T-Watch S3",
        "PiComputer S3", "Heltec HT62", "EBYTE ESP32-S3", "ESP32-S3 Pico", "Chatter 2", "Heltec Wireless Paper 1.0",
        "Heltec Wireless Tracker 1.0", "unPhone", "TD-LORAC", "CDEBYTE EoRa-S3", "TWC Mesh V4", "nRF52 ProMicro DIY",
        "RadioMaster 900 Bandit Nano", "Heltec Capsule Sensor V3", "Heltec Vision Master T190", "Heltec Vision Master E213",
        "Heltec Vision Master E290", "Heltec Mesh Node T114", "SenseCAP Indicator", "SenseCAP T1000-E", "RAK3172", "Wio-E5",
        "RadioMaster 900 Bandit", "ME25LS01", "RP2040 Feather RFM95", "M5Stack Core Basic", "M5Stack Core2", "Raspberry Pi Pico 2",
        "M5Stack CoreS3", "Seeed XIAO S3", "MS24SF1", "T-LoRa C6", "WisMesh Tap", "Routastic", "Mesh-Tab", "MeshLink",
        "XIAO nRF52 kit", "ThinkNode M1", "ThinkNode M2", "T-ETH Elite", "Heltec Sensor Hub", "Muzi Base", "Heltec Mesh Pocket",
        "Seeed Solar Node", "NomadStar Meteor Pro", "CrowPanel", "Link 32", "Wio Tracker L1", "Wio Tracker L1 E-Ink",
        "Muzi R1 Neo", "T-Deck Pro", "T-LoRa Pager", "M5Stack", "WisMesh Tag", "RAK3312", "ThinkNode M5", "Heltec Mesh Solar",
        "T-Echo Lite", "Heltec V4", "M5Stack C6L", "M5Stack Cardputer Adv", "Heltec Wireless Tracker V2", "T-Watch Ultra",
        "ThinkNode M3", "WisMesh Tap V2", "RAK3401", "RAK6421", "ThinkNode M4", "ThinkNode M6", "MeshStick 1262", "T-Beam 1 W",
        "T5-S3 E-Paper Pro", "T-Beam BPF", "Mini E-Paper S3", "T-Display S3 Pro", "Heltec Mesh Node T096", "Mesh Tracker X1",
        "ThinkNode M7", "ThinkNode M8", "ThinkNode M9", "Heltec V4 R8", "Heltec Mesh Node T1", "Station G3", "T-Impulse Plus",
        "T-Echo Card", "Wio Tracker L2", "CrowPanel P4", "Heltec Mesh Tower V2", "Meshnology W10", "Heltec RC32", "Heltec RC52",
        "Heltec RCC6", "Wio Tracker L1 Pro 1 W", "Meshnology W12", "MeshPager X2", "T-Connect Pro", "Axiometa Genesis Mini",
        "Makerfabs Nomad Terminal", "ThinkNode MX", "Nodara", "Heltec Mesh Pocket V2"};
    if (v == 255) return "private hardware";
    return v < sizeof(t) / sizeof(t[0]) ? t[v] : nullptr;
}
inline const char* roleName(uint64_t v) {
    static const char* t[] = {"client", "client (mute)", "router", "router-client", "repeater", "tracker", "sensor", "TAK",
                              "client (hidden)", "lost and found", "TAK tracker", "router (late)", "client base"};
    return v < sizeof(t) / sizeof(t[0]) ? t[v] : "";
}
inline std::string nodeId(uint32_t n) { char b[12]; std::snprintf(b, sizeof b, "!%08x", n); return b; }

/** A float from a protobuf fixed32 (IEEE-754 little-endian, as the wire format carries it). */
inline double f32(uint64_t v) { uint32_t u = uint32_t(v); float f; std::memcpy(&f, &u, 4); return double(f); }

inline void parseUser(const uint8_t* p, size_t n, Msg& m) {
    Pb r(p, n); int f, wt; uint64_t v = 0; const uint8_t* b = nullptr; size_t bl = 0;
    while (r.next(f, wt, v, b, bl)) {
        if (f == 2 && wt == 2) m.longName = cleanText(b, bl, 40);
        else if (f == 3 && wt == 2) m.shortName = cleanText(b, bl, 8);
        else if (f == 5 && wt == 0) { const char* h = hwName(v); m.hw = h ? h : "hardware #" + std::to_string(v); }
        else if (f == 7 && wt == 0) m.role = roleName(v);
    }
}
inline void parsePosition(const uint8_t* p, size_t n, Msg& m) {
    Pb r(p, n); int f, wt; uint64_t v = 0; const uint8_t* b = nullptr; size_t bl = 0;
    int32_t lat = 0, lon = 0; bool hl = false, hn = false;
    while (r.next(f, wt, v, b, bl)) {
        if (f == 1 && wt == 5) { lat = int32_t(uint32_t(v)); hl = true; }
        else if (f == 2 && wt == 5) { lon = int32_t(uint32_t(v)); hn = true; }
        else if (f == 3 && wt == 0) m.altM = double(int32_t(uint32_t(v)));
        else if (f == 23 && wt == 0) m.precisionBits = int(v);
    }
    // ★ 0,0 is "no fix" in practice; a latitude past ±90 is damage
    if (hl && hn && (lat || lon) && std::abs(lat) <= 900000000 && std::abs(lon) <= 1800000000) { m.lat = lat * 1e-7; m.lon = lon * 1e-7; }
}
inline void parseTelemetry(const uint8_t* p, size_t n, Msg& m) {
    Pb r(p, n); int f, wt; uint64_t v = 0; const uint8_t* b = nullptr; size_t bl = 0;
    while (r.next(f, wt, v, b, bl)) {
        if (wt != 2) continue;
        Pb s(b, bl); int g, w2; uint64_t x = 0; const uint8_t* bb = nullptr; size_t bbl = 0;
        if (f == 2) {          // DeviceMetrics
            while (s.next(g, w2, x, bb, bbl)) {
                if (g == 1 && w2 == 0) m.batt = int(x);
                else if (g == 2 && w2 == 5) m.volt = f32(x);
                else if (g == 3 && w2 == 5) m.chUtil = f32(x);
                else if (g == 4 && w2 == 5) m.airUtil = f32(x);
                else if (g == 5 && w2 == 0) m.uptimeS = int64_t(x);
            }
        } else if (f == 3) {   // EnvironmentMetrics
            while (s.next(g, w2, x, bb, bbl)) {
                if (g == 1 && w2 == 5) m.tempC = f32(x);
                else if (g == 2 && w2 == 5) m.hum = f32(x);
                else if (g == 3 && w2 == 5) m.hpa = f32(x);
            }
        }
    }
}

/** Decode one frame's bytes. False when it is too short to be Meshtastic. */
inline bool parse(const std::vector<uint8_t>& b, Msg& m) {
    if (b.size() < 16) return false;
    auto u32 = [&](size_t o) { return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24; };
    const uint32_t to = u32(0), from = u32(4), id = u32(8);
    const uint8_t flags = b[12];
    m.proto = "meshtastic";
    m.node = nodeId(from);
    m.to = to; m.broadcast = to == 0xFFFFFFFFu;
    m.hopLimit = flags & 7; m.hopStart = flags >> 5 & 7;
    if (flags & 0x10) m.route = "via MQTT";
    m.chHash = b[13];
    // ★ only a default-named channel under the published key: its hash must match, then its plaintext must be a Data
    for (const std::string& name : defaultNames()) {
        if (channelHash(name, defaultKey(), 16) != m.chHash) continue;
        uint8_t nonce[16] = {0};
        for (int k = 0; k < 4; ++k) { nonce[k] = uint8_t(id >> (8 * k)); nonce[8 + k] = uint8_t(from >> (8 * k)); }
        std::vector<uint8_t> pt(b.size() - 16);
        crypto::aesCtr(defaultKey(), nonce, b.data() + 16, pt.data(), pt.size());
        Pb r(pt.data(), pt.size()); int f, wt; uint64_t v = 0; const uint8_t* pl = nullptr; size_t pll = 0;
        int port = -1; const uint8_t* body = nullptr; size_t bodyLen = 0; bool first = true, ok = true;
        while (r.next(f, wt, v, pl, pll)) {
            if (first && !(f == 1 && wt == 0)) { ok = false; break; }   // ★ a Data begins with its portnum
            first = false;
            if (f == 1 && wt == 0) port = int(v);
            else if (f == 2 && wt == 2) { body = pl; bodyLen = pll; }
            else if (f > 15) { ok = false; break; }
        }
        if (!ok || r.bad || port < 0 || port > 511) continue;
        m.channel = name;
        switch (port) {
            case 1:   // TEXT_MESSAGE_APP
                if (m.broadcast) { m.what = "text"; m.text = cleanText(body, bodyLen); }
                else m.what = "direct";              // ★ addressed to one node: not shown
                break;
            case 3: m.what = "position"; parsePosition(body, bodyLen, m); break;
            case 4: m.what = "nodeinfo"; parseUser(body, bodyLen, m); break;
            case 67: m.what = "telemetry"; parseTelemetry(body, bodyLen, m); break;
            case 5: m.what = "routing"; break;
            case 70: m.what = "traceroute"; break;
            case 71: m.what = "neighbours"; break;
            case 73: m.what = "map report"; break;
            case 66: m.what = "range test"; break;
            case 8: m.what = "waypoint"; break;
            default: m.what = "app " + std::to_string(port); break;
        }
        return true;
    }
    m.privateCh = true;
    m.what = m.chHash == 0 ? "direct (PKI)" : "private";
    return true;
}

/** "Meshtastic · Bob's T-Beam (BOB) · position · 2 hops" — the readable line of one packet. */
inline std::string line(const Msg& m) {
    const std::string d = mkDot();
    std::string s = "Meshtastic" + d + (m.longName.empty() ? m.node : m.longName);
    if (!m.shortName.empty()) s += " (" + m.shortName + ")";
    if (m.privateCh) {
        char h[48]; std::snprintf(h, sizeof h, "channel hash 0x%02X", m.chHash & 0xFF);
        return s + d + (m.chHash == 0 ? "direct message (encrypted)" : std::string("private channel")) + d + h;
    }
    if (m.what == "text") s += d + "\xe2\x80\x9c" + m.text + "\xe2\x80\x9d";
    else s += d + m.what;
    return s;
}

} // namespace meshtastic
} // namespace vibelora
