#pragma once
// ★★★ THE OWNER'S NAMES FOR A RADIO'S OWN ANTENNA PORTS (V12, 2026-11-03). A tester's USRP B210 clone has four RX ports
// (RX2 and TX/RX on channel A and on channel B): "Several antennas, so you should be able to add a name to each of them".
// Stuart: for EVERY radio with more than one antenna input — an RSPdx's A/B/C, an RSP2's A/B/Hi-Z, an RSPduo's tuners, a
// USRP's RX2 / TX/RX per channel — not only the USRP.
//
// ★★ KEYED BY THE PORT NAME THE DRIVER ALREADY PUBLISHES in hwinfo `antennas` ("A", "Hi-Z", "Tuner 2 50Ω", "RX2 (B)"), so a
//    new driver gets naming for free the moment it lists its ports. The {type:"antenna"} message, `antennaPort` and the
//    per-band `antennaMap` keep using the PORT name; a name is only ever what a person READS.
// ★★ NOT THE EXTERNAL SWITCH (vibe_antswitch.h). That is a relay box in front of the radio, whose antennas are names
//    already; this is the sockets on the radio itself. When a switch is set up the clients offer the switch's antennas and
//    these names are not published (LocalSdrShim::radioCapsJson).
//
// STORED as a flat JSON object in the radio's config entry, as a string — `antennaNames`, the same shape `antennaSwitch`
// uses: {"RX2 (A)":"Discone","TX/RX (A)":"FM Yagi"}. Published in hwinfo as the object itself, `radio.antennaNames`.
// ★ BOUNDED AND CLEANED HERE, the one place, on every way in (the config load, the setup save, the shim's setter):
//    at most kMaxPorts entries, a port key of at most kMaxPortBytes, a name of at most kMaxNameBytes (cut on a UTF-8
//    boundary), control characters dropped, whitespace trimmed and collapsed, an empty name = no entry. A malformed blob is
//    "no names", never half-applied.
// ★ ESCAPED for JSON here (toJson — quotes, backslashes, control characters, and < > & as < etc. so the text is
//    inert even inside an HTML <script>). The HTML side escapes at render (the setup page's swEsc, the clients' textContent
//    and React text). Tested by vibeserver/test-port-names.cpp.
#include "vibe_antswitch.h"   // ★ its small strict JSON reader (antswitch::Parser)

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace vibe {
namespace portnames {

constexpr size_t kMaxPorts     = 16;
constexpr size_t kMaxPortBytes = 32;
constexpr size_t kMaxNameBytes = 40;
constexpr size_t kMaxBlobBytes = 4000;   // the stored text; anything longer is corrupt or hostile, dropped whole

using Names = std::vector<std::pair<std::string, std::string>>;   // (port, name), in the order the owner gave them

/** Cut `s` to at most `max` bytes without splitting a UTF-8 sequence. */
inline std::string cutUtf8(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // s[n] is a continuation byte: back off
    return s.substr(0, n);
}

/** A name or a port key as a person may have typed it: control characters gone, runs of spaces one space, trimmed,
 *  then bounded. Invalid UTF-8 bytes are dropped rather than passed on. */
inline std::string clean(const std::string& in, size_t max) {
    std::string out;
    out.reserve(in.size());
    bool space = false;
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > in.size()) { ++i; continue; }                       // a stray byte: dropped
        bool ok = true;
        for (size_t k = 1; k < len; k++) if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) ok = false;
        if (!ok) { ++i; continue; }
        if (len == 1) {
            if (c < 0x20 || c == 0x7F || c == ' ') { space = !out.empty(); i++; continue; }   // tabs, newlines: a space
        } else if (len == 2 && c == 0xC2 && static_cast<unsigned char>(in[i + 1]) < 0xA0) { i += 2; continue; }   // C1 controls
        if (space) { out += ' '; space = false; }
        out.append(in, i, len);
        i += len;
    }
    return cutUtf8(out, max);
}

/** JSON string body (no quotes): " \ and every control character escaped, and < > & too, so the text cannot close a
 *  <script> or start a tag wherever a page embeds it. */
inline std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == '&') {
                    o += "\\u00"; o += hex[c >> 4]; o += hex[c & 15];
                } else o += static_cast<char>(c);
        }
    }
    return o;
}

/** Add or replace one entry, cleaned and bounded. An empty name REMOVES the port's entry. */
inline void set(Names& n, const std::string& port, const std::string& name) {
    const std::string p = clean(port, kMaxPortBytes), v = clean(name, kMaxNameBytes);
    if (p.empty()) return;
    for (auto it = n.begin(); it != n.end(); ++it)
        if (it->first == p) { if (v.empty()) n.erase(it); else it->second = v; return; }
    if (!v.empty() && n.size() < kMaxPorts) n.emplace_back(p, v);
}

/** The stored blob → entries. "" / malformed / not an object = none. Non-string values are skipped. */
inline Names parse(const std::string& blob) {
    Names out;
    if (blob.empty() || blob.size() > kMaxBlobBytes) return out;
    antswitch::Json j;
    try {
        antswitch::Parser ps(blob);
        if (!ps.parse(j) || j.kind != antswitch::Json::Obj) return out;
    } catch (...) { return out; }   // ★ the reader's \u uses stoul, which throws on "\uZZZZ"
    // ★ The reader's object is a std::map (sorted), so the owner's order is recovered from the text: each key's first
    //   appearance as "key": — good enough, and an order that is wrong costs nothing but where a row sits.
    std::vector<std::pair<size_t, std::string>> order;
    for (const auto& kv : j.o) {
        size_t at = blob.find("\"" + kv.first + "\"");
        order.emplace_back(at == std::string::npos ? blob.size() : at, kv.first);
    }
    std::sort(order.begin(), order.end());
    for (const auto& o : order) {
        const antswitch::Json& v = j.o.at(o.second);
        if (v.kind == antswitch::Json::Str) set(out, o.second, v.s);
    }
    return out;
}

/** Entries → the JSON object (also what hwinfo carries). "{}" when there are none. */
inline std::string toJson(const Names& n) {
    std::string o = "{";
    for (size_t i = 0; i < n.size(); i++) {
        if (i) o += ',';
        o += "\"" + esc(n[i].first) + "\":\"" + esc(n[i].second) + "\"";
    }
    return o + "}";
}

/** The stored form: cleaned and bounded, and "" (not "{}") when there are none, so an untouched radio writes nothing. */
inline std::string normalise(const std::string& blob) {
    const Names n = parse(blob);
    return n.empty() ? std::string() : toJson(n);
}

/** The owner's name for a port, or "". Exact match — the keys are the driver's own port names. */
inline std::string nameFor(const Names& n, const std::string& port) {
    for (const auto& e : n) if (e.first == port) return e.second;
    return "";
}

/** What a person reads: "Discone (RX2)", "Discone · RX2 (A)", or the bare port when it has no name.
 *  ★ The clients' twin is src/utils/antennaPortNames.ts portLabel — the same rule; test-port-names.cpp and
 *    scripts/test_antenna_port_names.ts pin the same cases. */
inline std::string display(const Names& n, const std::string& port) {
    const std::string nm = nameFor(n, port);
    if (nm.empty()) return port;
    // ★ A port that already ends in brackets (a B210's "RX2 (A)") is joined with a dot, not nested brackets.
    return (!port.empty() && port.back() == ')') ? nm + " \xC2\xB7 " + port : nm + " (" + port + ")";
}

}  // namespace portnames
}  // namespace vibe
