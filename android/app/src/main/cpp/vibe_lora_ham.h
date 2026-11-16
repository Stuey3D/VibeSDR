// vibe_lora_ham.h — three more networks on the LoRa engine (vibe_lora_service.h): LoRa-APRS, FANET and MeshCom.
// docs/v12/LORA-MESH.md §LoRa-APRS / FANET / MeshCom.
//
// ★★★ OUR OWN, FROM THE PUBLISHED DESCRIPTIONS:
//   · LoRa-APRS — the amateur community's convention (OE5BPA's lora-aprs firmware and its documentation, followed by
//     every tracker and iGate): an APRS packet in TNC2 text, "SRC>DEST,PATH:info", after the three bytes '<' 0xFF 0x01;
//     433.775 MHz, SF12, 125 kHz, CR 4/5, explicit header, CRC on, sync word 0x12. The info field is parsed by OUR APRS
//     parser (vibe_aprs.h parseInfo), the same one the 1200 Bd decoder uses.
//   · FANET — Skytraxx's open protocol (fanet-stm32 protocol.txt, read for the facts): 868.2 MHz SF7 250 kHz in Europe,
//     920.8 MHz SF7 500 kHz in zone 2 (the Americas, Australia/NZ, Japan, China), sync word 0xF1, explicit header, CRC
//     on. Header byte (extended-header flag, forward flag, 6-bit type), the 3-byte source (manufacturer, id LE), the
//     extended header (ACK, cast, signature), and types 1 tracking, 2 name, 3 message, 4 service, 7 ground tracking with
//     their published scalings (lat value / 93206, lon value / 46603, 24-bit LE two's complement).
//   · MeshCom 4.x — the amateur mesh of ICSSW / OE1KBC (icssw.org protocol page): a type byte (':' text, '!' position),
//     a 4-byte message id (LSB first), a MAX-HOP byte (hops in the low 3 bits, 0x80 = came via MQTT, 0x40 = path
//     requested), then "SRC[,PATH]>DEST" and the APRS-style payload, a 0x00, then hardware id, modulation id and a
//     16-bit FCS. 433.175 MHz SF11 250 kHz CR 4/6, sync word 0x2B (MeshCom's own LoRa settings).
//     ★ The published page states the FCS two ways (16-bit sum, 32-bit field); it is CHECKED as the 16-bit sum of the
//       bytes up to the modulation id and counted, but a packet is not refused on it alone — the LoRa CRC already held.
//
// ★★★ PRIVACY (vibe_lora_track.h §Privacy):
//   · LoRa-APRS and MeshCom are AMATEUR RADIO — callsigns and positions are public by law and by design: shown exactly,
//     as the APRS decoder shows them.
//   · FANET pilots: a paraglider or glider is a person. FANET carries the pilot's own "online tracking" bit — consent to
//     being tracked publicly (what the Open Glider Network honours). Set: the position is published exactly. Clear: it
//     leaves the node table rounded to ~1 km and marked approximate, as a mesh companion node's does.
#pragma once
#include "vibe_lora_msg.h"
#include "vibe_aprs.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vibelora {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  LoRa-APRS
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace loraaprs {

/** A callsign-SSID as APRS writes it: 1–9 letters/digits, an optional -SSID. */
inline bool callOk(const std::string& c) {
    if (c.empty() || c.size() > 12) return false;
    size_t dash = c.find('-');
    const std::string base = c.substr(0, dash);
    if (base.empty() || base.size() > 9) return false;
    for (char ch : base) if (!std::isalnum((unsigned char)ch)) return false;
    if (dash != std::string::npos) {
        const std::string s = c.substr(dash + 1);
        if (s.empty() || s.size() > 2) return false;
        for (char ch : s) if (!std::isalnum((unsigned char)ch)) return false;
    }
    return true;
}

/** Fill `m` from an APRS packet already split into source, destination, path and info (shared with MeshCom). */
inline void fromAprs(const std::string& src, const std::string& dest, const std::string& path, const std::string& info, Msg& m) {
    vibeaprs::Packet p;
    vibeaprs::parseInfo(src, dest, info, p);
    m.node = p.name.empty() ? src : p.name;
    m.longName = p.name.empty() ? src : p.name + " (" + src + ")";
    m.what = p.kind;
    if (p.hasPos && p.lat == p.lat && p.lon == p.lon && std::fabs(p.lat) <= 90 && std::fabs(p.lon) <= 180 && !(p.lat == 0 && p.lon == 0)) {
        m.lat = p.lat; m.lon = p.lon;
    }
    if (p.altFt == p.altFt) m.altM = p.altFt * 0.3048;
    if (p.speedKn == p.speedKn) m.speedKmh = p.speedKn * 1.852;
    if (p.course == p.course) m.course = p.course;
    m.sym = std::string(1, p.symTable) + std::string(1, p.symCode);
    m.role = vibeaprs::symbolName(p.symTable, p.symCode);
    m.aprsLine = vibeaprs::readable(p);
    m.path = path;
    if (p.wx.any()) {
        if (p.wx.tempF == p.wx.tempF) m.tempC = (p.wx.tempF - 32) * 5 / 9;
        if (p.wx.hum == p.wx.hum) m.hum = p.wx.hum;
        if (p.wx.baroMbar == p.wx.baroMbar) m.hpa = p.wx.baroMbar;
    }
    // ★ a message to someone, or a bulletin: amateur, public — kept as the APRS decoder keeps it
    if ((p.kind == "message" || p.kind == "bulletin") && !p.text.empty()) {
        m.text = p.text; m.channel = p.to; m.from = src; m.broadcast = p.kind == "bulletin";
    }
}

/** '<' 0xFF 0x01 then "SRC>DEST[,PATH]:info". False when it is not that shape. */
inline bool parse(const std::vector<uint8_t>& b, Msg& m) {
    if (b.size() < 3 + 5 || b[0] != '<' || b[1] != 0xFF || b[2] != 0x01) return false;
    std::string s(b.begin() + 3, b.end());
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == 0)) s.pop_back();
    const size_t gt = s.find('>'), co = s.find(':');
    if (gt == std::string::npos || co == std::string::npos || gt > co || gt == 0) return false;
    const std::string src = s.substr(0, gt);
    std::string head = s.substr(gt + 1, co - gt - 1), path;
    const size_t cm = head.find(',');
    if (cm != std::string::npos) { path = head.substr(cm + 1); head.resize(cm); }
    if (!callOk(src) || head.empty() || head.size() > 9) return false;
    m.proto = "loraaprs";
    // ★ off the air: Latin-1 and garbage are common in comments — the parser reads bytes, the JSON gets cleaned text
    fromAprs(src, head, path, s.substr(co + 1), m);
    return true;
}

} // namespace loraaprs

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FANET
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace fanet {

inline const char* aircraftName(int t) {
    static const char* n[8] = {"aircraft", "paraglider", "hang glider", "balloon", "glider", "powered aircraft", "helicopter", "drone"};
    return n[t & 7];
}
inline const char* groundName(int t) {
    switch (t & 15) {
        case 1: return "walking"; case 2: return "vehicle"; case 3: return "bike"; case 4: return "boat";
        case 8: return "needs a ride"; case 9: return "landed well"; case 12: return "needs technical support";
        case 13: return "needs medical help"; case 14: return "distress call"; case 15: return "distress call (automatic)";
        default: return "on the ground";
    }
}
/** 24-bit little-endian two's complement. */
inline int32_t s24(const uint8_t* p) {
    int32_t v = int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16);
    return (v & 0x800000) ? v - 0x1000000 : v;
}
inline bool pos(const uint8_t* p, Msg& m) {
    const double la = s24(p) / 93206.0, lo = s24(p + 3) / 46603.0;
    if (std::fabs(la) > 90 || std::fabs(lo) > 180 || (la == 0 && lo == 0)) return false;
    m.lat = la; m.lon = lo;
    return true;
}
/** 7-bit two's complement with a scale bit (FANET's climb / turn-rate bytes). */
inline int s7(uint8_t b) { int v = b & 0x7F; return (v & 0x40) ? v - 0x80 : v; }

inline bool parse(const std::vector<uint8_t>& b, Msg& m) {
    if (b.size() < 4) return false;
    const int type = b[0] & 0x3F;
    const bool ext = b[0] & 0x80;
    char id[16];
    std::snprintf(id, sizeof id, "%02X:%04X", b[1], unsigned(b[2] | b[3] << 8));
    size_t i = 4;
    if (ext) {
        if (i >= b.size()) return false;
        const uint8_t e = b[i++];
        if (e & 0x20) { if (i + 3 > b.size()) return false; m.to = uint32_t(b[i]) << 16 | uint32_t(b[i + 1] | b[i + 2] << 8); i += 3; }
        else m.broadcast = true;
        if (e & 0x10) { if (i + 4 > b.size()) return false; i += 4; }     // signature: not checked, not shown
    } else m.broadcast = true;
    const uint8_t* p = b.data() + i; const size_t n = b.size() - i;
    m.proto = "fanet";
    m.node = id;
    switch (type) {
        case 0: m.what = "ack"; return true;
        case 1: {
            if (n < 11) return false;
            if (!pos(p, m)) return false;
            const int st = p[6] | p[7] << 8;
            m.online = st & 0x8000;
            m.acType = (st >> 12) & 7;
            m.altM = (st & 0x7FF) * ((st & 0x800) ? 4 : 1);
            m.speedKmh = (p[8] & 0x7F) * 0.5 * ((p[8] & 0x80) ? 5 : 1);
            m.climb = s7(p[9]) * 0.1 * ((p[9] & 0x80) ? 5 : 1);
            m.course = p[10] * 360.0 / 256.0;
            m.role = aircraftName(m.acType);
            m.what = "tracking";
            return true;
        }
        case 2: m.what = "name"; m.longName = cleanText(p, n, 40); return !m.longName.empty();
        case 3: {
            if (n < 2) return false;
            m.what = "message";
            // ★ FANET messages are broadcast to everyone in range (a pilot's "landed", a club's notice); a unicast one
            //   was addressed to someone and is not shown (as Meshtastic's direct messages)
            if (!m.broadcast) { m.privateCh = true; return true; }
            m.text = cleanText(p + 1, n - 1, 200);
            return true;
        }
        case 4: {
            if (n < 1) return false;
            const uint8_t h = p[0];
            size_t k = 1 + ((h & 1) ? 1 : 0);
            m.what = "service";
            m.role = (h & 0x80) ? "internet gateway" : (h & 0x60) ? "weather station" : "ground station";
            if (k + 6 <= n && pos(p + k, m)) k += 6; else return h != 0;   // ★ flags only, no position
            if ((h & 0x40) && k < n) { m.tempC = int8_t(p[k]) * 0.5; ++k; }
            if ((h & 0x20) && k + 3 <= n) {
                m.windDir = p[k] * 360.0 / 256.0;
                m.windKmh = (p[k + 1] & 0x7F) * 0.2 * ((p[k + 1] & 0x80) ? 5 : 1);
                m.gustKmh = (p[k + 2] & 0x7F) * 0.2 * ((p[k + 2] & 0x80) ? 5 : 1);
                k += 3;
            }
            if ((h & 0x10) && k < n) { m.hum = p[k] * 0.4; ++k; }
            if ((h & 0x08) && k + 2 <= n) { m.hpa = (p[k] | p[k + 1] << 8) * 0.1 + 430.0; k += 2; }
            if ((h & 0x02) && k < n) { m.batt = int(std::lround((p[k] & 0x0F) * 100.0 / 15.0)); ++k; }
            return true;
        }
        case 7: {
            if (n < 7) return false;
            if (!pos(p, m)) return false;
            m.acType = -1;
            m.ground = (p[6] >> 4) & 15;
            m.online = p[6] & 1;
            m.role = groundName(m.ground);
            m.what = "ground";
            return true;
        }
        default:
            m.what = type == 5 ? "landmarks" : type == 6 ? "remote config" : type == 8 ? "hardware info" : type == 9 ? "thermal" : "type " + std::to_string(type);
            return true;
    }
}

} // namespace fanet

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  MeshCom
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace meshcom {

inline bool parse(const std::vector<uint8_t>& b, Msg& m) {
    if (b.size() < 1 + 4 + 1 + 5) return false;
    const uint8_t t = b[0];
    if (t != ':' && t != '!' && t != '@') return false;
    const uint8_t hop = b[5];
    // the APRS part runs to the 0x00 terminator
    size_t z = 6;
    while (z < b.size() && b[z] != 0) ++z;
    if (z >= b.size()) return false;
    std::string s(b.begin() + 6, b.begin() + long(z));
    for (char c : s) if (uint8_t(c) < 0x20) return false;   // ★ the header is text: a control byte means it is not MeshCom
    const size_t gt = s.find('>');
    if (gt == std::string::npos || gt == 0) return false;
    std::string src = s.substr(0, gt), path;
    const size_t cm = src.find(',');
    if (cm != std::string::npos) { path = src.substr(cm + 1); src.resize(cm); }
    if (!loraaprs::callOk(src)) return false;
    // destination then the payload; ':' text payload is "DEST:text", '!' position is "DEST!lat/lon…"
    const std::string rest = s.substr(gt + 1);
    const size_t sep = rest.find_first_of(t == ':' ? ":" : "!@=/");
    if (sep == std::string::npos || sep == 0 || sep > 12) return false;
    const std::string dest = rest.substr(0, sep);
    m.proto = "meshcom";
    m.hopLimit = hop & 7;
    if (hop & 0x80) m.route = "via MQTT";
    m.msgId = uint32_t(b[1] | b[2] << 8 | b[3] << 16 | uint32_t(b[4]) << 24);
    // the trailer: hardware id, modulation id, FCS (16-bit sum, LE) — checked and counted, see the header
    if (z + 3 <= b.size() - 1) {
        m.hwId = b[z + 1];
        unsigned sum = 0;
        for (size_t k = 0; k <= z + 2 && k < b.size(); ++k) sum += b[k];
        if (z + 4 < b.size()) m.fcsOk = ((sum & 0xFFFF) == unsigned(b[z + 3] | b[z + 4] << 8)) ? 1 : 0;
    }
    if (t == ':') {
        std::string text = rest.substr(sep + 1);
        // ★ an APRS-style addressed message ("CALL     :text") inside: the inner addressee
        m.what = "text";
        m.node = src; m.longName = src; m.from = src;
        m.channel = dest == "*" ? "all" : dest;
        m.broadcast = dest == "*" || std::all_of(dest.begin(), dest.end(), [](char c) { return std::isdigit((unsigned char)c); });
        const size_t br = text.rfind('{');
        if (br != std::string::npos && text.size() - br <= 6) text.resize(br);
        m.text = cleanText(reinterpret_cast<const uint8_t*>(text.data()), text.size(), 200);
        m.path = path;
        return true;
    }
    loraaprs::fromAprs(src, dest, path, rest.substr(sep), m);
    if (m.what == "other") m.what = "beacon";
    return true;
}

} // namespace meshcom

// ── the list's line for each ─────────────────────────────────────────────────────────────────────────────────────────
inline std::string hamLine(const Msg& m) {
    const std::string d = mkDot();
    char b[96];
    if (m.proto == "loraaprs" || (m.proto == "meshcom" && m.what != "text")) {
        std::string s = (m.proto == "loraaprs" ? std::string("LoRa APRS") : std::string("MeshCom")) + d + (m.aprsLine.empty() ? m.node : m.aprsLine);
        if (!m.path.empty()) s += d + "via " + m.path;
        return s;
    }
    if (m.proto == "meshcom") return "MeshCom" + d + m.from + " \xe2\x86\x92 " + m.channel + ": \xe2\x80\x9c" + m.text + "\xe2\x80\x9d";
    // FANET
    std::string s = "FANET" + d + (m.longName.empty() ? m.node : m.longName);
    if (m.what == "tracking") {
        s += d + m.role;
        if (m.altM == m.altM) { std::snprintf(b, sizeof b, "%.0f m", m.altM); s += d + b; }
        if (m.speedKmh == m.speedKmh) { std::snprintf(b, sizeof b, "%.0f km/h", m.speedKmh); s += d + b; }
        if (m.climb == m.climb && std::fabs(m.climb) >= 0.1) { std::snprintf(b, sizeof b, "%+.1f m/s", m.climb); s += d + b; }
    } else if (m.what == "ground") s += d + m.role;
    else if (m.what == "service") {
        s += d + m.role;
        if (m.windKmh == m.windKmh) { std::snprintf(b, sizeof b, "wind %.0f km/h from %.0f\xc2\xb0", m.windKmh, m.windDir); s += d + b; }
        if (m.tempC == m.tempC) { std::snprintf(b, sizeof b, "%.1f \xc2\xb0" "C", m.tempC); s += d + b; }
    } else if (m.what == "message" && !m.text.empty()) s += d + "\xe2\x80\x9c" + m.text + "\xe2\x80\x9d";
    else s += d + m.what;
    return s;
}

} // namespace vibelora
