// m17.h — V12 M17 digital voice on the R36S: the rules of src/services/m17.ts (the app and the web page read that file),
// line for line. docs/v12/M17.md.
//
// ★★ OFFERED ONLY WHERE THE SERVER LISTS IT: vibeserver.json `modes` has "m17" only when the server's libcodec2 loaded and
//    its radio reaches 2 m or 70 cm. No "m17" = no M17 key (app.h offeredModes).
// ★ Nothing to choose inside the mode (no sub-modes; the receiver cuts its own channel, so no BANDWIDTH either). Entering
//   the mode asks for the report ({"type":"m17_status"}).
// ★ The bar has one text, so the VTS line and the info go together (m17Line): "M17 · G4ABC · VIBE TEST",
//   "M17 · last G4ABC · SMS G4ABC → M0XYZ: 73".
// Golden-tested against the TS: test/gen_vts_golden.ts (ops MV / MO / ML) → test/vts_golden_test.cpp.
#pragma once
#include "json.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hh {

inline const char* const kM17Label = "M17";

struct M17Gnss { double lat = 0, lon = 0; bool hasAlt = false, hasSpd = false; double alt = 0, spd = 0; };
struct M17Msg { bool have = false; double id = 0; std::string src, dst; double ptype = 0, bytes = 0; bool hasText = false; std::string text; };
/** The server's {"type":"m17"} report (m17.ts M17State; only what the line uses). */
struct M17State {
    bool sync = false;
    std::string kind, src, dst, enc, last, text, via;
    bool hasBer = false; int ber = 0;
    bool hasGnss = false; M17Gnss gnss;
    M17Msg msg;
};

namespace m17d {
/** One UTF-8 character at `i`: its code point and length (a malformed byte reads as itself, length 1). */
inline unsigned cp(const std::string& s, size_t i, size_t& len) {
    const unsigned char c = (unsigned char)s[i];
    auto cont = [&](size_t k) { return i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80; };
    if (c < 0x80) { len = 1; return c; }
    if ((c >> 5) == 6 && cont(1)) { len = 2; return ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F); }
    if ((c >> 4) == 14 && cont(1) && cont(2)) { len = 3; return ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F); }
    if ((c >> 3) == 30 && cont(1) && cont(2) && cont(3)) {
        len = 4; return ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) | (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
    }
    len = 1; return c;
}
/** JavaScript's \s (after the control characters became spaces). */
inline bool jsSpace(unsigned u) {
    return u == 0x20 || u == 0x09 || u == 0x0A || u == 0x0B || u == 0x0C || u == 0x0D || u == 0xA0 || u == 0x1680
        || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 || u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF;
}
}  // namespace m17d

/** m17.ts clean(): control characters to spaces, runs of white space to one, trimmed, at most `n` UTF-16 units. */
inline std::string m17Clean(const Json& v, size_t n, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (v.kind != Json::Str) return "";
    const std::string& s = v.str();
    std::string t;
    bool sp = false;
    for (size_t i = 0; i < s.size();) {
        size_t len; unsigned u = m17d::cp(s, i, len);
        if (u < 0x20 || u == 0x7F) u = 0x20;
        if (m17d::jsSpace(u)) { sp = true; i += len; continue; }
        if (sp && !t.empty()) t += ' ';
        sp = false;
        t.append(s, i, len);
        i += len;
    }
    // ★ slice(0, n) in UTF-16 units: a character above U+FFFF counts two
    std::string o; size_t units = 0;
    for (size_t i = 0; i < t.size();) {
        size_t len; const unsigned u = m17d::cp(t, i, len);
        const size_t w = u > 0xFFFF ? 2 : 1;
        if (units + w > n) break;
        o.append(t, i, len); units += w; i += len;
    }
    // ★ no trim after the cut, as the TS: slice(0, n) can end on the space it kept between two words
    if (ok) *ok = !o.empty();
    return o;
}

/** parseM17: false when the message is not a report. */
inline bool parseM17(const Json& m, M17State& st) {
    if (m.kind != Json::Obj || m["type"].str() != "m17") return false;
    st = M17State{};
    st.sync = m["state"].str() == "sync";
    auto finite = [](const Json& j) { return j.kind == Json::Num && std::isfinite(j.n); };
    if (st.sync) {
        const std::string k = m["kind"].str();
        if (k == "voice" || k == "voicedata" || k == "data" || k == "packet") st.kind = k;
        st.src = m17Clean(m["src"], 12); st.dst = m17Clean(m["dst"], 14);
        const std::string e = m["enc"].str();
        if (e == "scrambler" || e == "aes" || e == "other") st.enc = e;
        if (finite(m["ber"])) { st.hasBer = true; st.ber = (int)std::max(0.0, std::floor(m["ber"].n + 0.5)); }
    } else {
        st.last = m17Clean(m["last"], 12);
    }
    st.text = m17Clean(m["text"], 52);
    st.via = m17Clean(m["via"], 32);
    const Json& g = m["gnss"];
    if (g.kind == Json::Obj && finite(g["lat"]) && finite(g["lon"]) && std::fabs(g["lat"].n) <= 90 && std::fabs(g["lon"].n) <= 180) {
        st.hasGnss = true; st.gnss.lat = g["lat"].n; st.gnss.lon = g["lon"].n;
        if (finite(g["alt"])) { st.gnss.hasAlt = true; st.gnss.alt = g["alt"].n; }
        if (finite(g["spd"])) { st.gnss.hasSpd = true; st.gnss.spd = g["spd"].n; }
    }
    const Json& p = m["msg"];
    if (p.kind == Json::Obj && finite(p["id"]) && finite(p["ptype"]) && finite(p["bytes"])) {
        st.msg.have = true; st.msg.id = p["id"].n; st.msg.ptype = p["ptype"].n; st.msg.bytes = p["bytes"].n;
        st.msg.src = m17Clean(p["src"], 12); st.msg.dst = m17Clean(p["dst"], 14);
        bool ok = false;
        const std::string t = m17Clean(p["text"], 200, &ok);
        if (st.msg.ptype == 5 && ok) { st.msg.hasText = true; st.msg.text = t; }
    }
    return true;
}

/** Is M17 offered by THIS server (vibeserver.json `modes`)? */
inline bool m17Offered(const Json& j) {
    for (auto& m : j["modes"].a) if (m.str() == "m17") return true;
    return false;
}

/** m17Vts. `st` null = no report yet. */
inline std::string m17Vts(const M17State* st) {
    static const std::string dot = " \xc2\xb7 ", arrow = " \xe2\x86\x92 ";
    const std::string L = kM17Label;
    if (!st) return L + dot + "searching";
    if (!st->sync) return !st->last.empty() ? L + dot + "last " + st->last : L + dot + "searching";
    const std::string who = st->src.empty() ? std::string("\xe2\x80\xa6") : st->src;
    const std::string line = !st->dst.empty() && st->dst != "@ALL" ? L + dot + who + arrow + st->dst : L + dot + who;
    if (!st->enc.empty()) return line + dot + "encrypted";
    if (st->kind == "packet" || st->kind == "data") return line + dot + "data";
    return line;
}

/** JavaScript's toFixed(4) and Math.round, for the position line. */
namespace m17d {
inline std::string fixed4(double v) { char b[48]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
inline std::string jsRound(double v) { char b[48]; std::snprintf(b, sizeof b, "%.0f", std::floor(v + 0.5)); std::string s = b; return s == "-0" ? "0" : s; }
inline std::string num(double v) {   // String(number) for the integers a report carries
    if (std::floor(v) == v && std::fabs(v) < 1e15) { char b[32]; std::snprintf(b, sizeof b, "%.0f", v); return b; }
    char b[32]; std::snprintf(b, sizeof b, "%.17g", v); return b;
}
}  // namespace m17d
inline std::string m17Position(const M17Gnss& g) {
    std::string s = m17d::fixed4(std::fabs(g.lat)) + "\xc2\xb0 " + (g.lat >= 0 ? "N" : "S") + " "
                  + m17d::fixed4(std::fabs(g.lon)) + "\xc2\xb0 " + (g.lon >= 0 ? "E" : "W");
    if (g.hasAlt) s += ", " + m17d::jsRound(g.alt) + " m";
    if (g.hasSpd && g.spd > 0) s += ", " + m17d::jsRound(g.spd) + " km/h";
    return s;
}
inline std::string m17MsgLine(const M17Msg& m) {
    std::string who = m.src;
    if (!m.dst.empty()) who += (who.empty() ? "" : " ") + std::string("\xe2\x86\x92 ") + m.dst;
    if (m.ptype == 5 && m.hasText) return "SMS" + (who.empty() ? std::string() : " " + who) + ": " + m.text;
    return "Packet (type " + m17d::num(m.ptype) + ", " + m17d::num(m.bytes) + " bytes)" + (m.src.empty() ? std::string() : " from " + m.src);
}
inline std::string m17Info(const M17State* st) {
    if (!st) return "";
    static const std::string dot = " \xc2\xb7 ";
    std::vector<std::string> parts;
    if (!st->via.empty()) parts.push_back(st->via);
    if (!st->text.empty()) parts.push_back(st->text);
    if (st->hasGnss) parts.push_back(m17Position(st->gnss));
    if (st->msg.have) parts.push_back(m17MsgLine(st->msg));
    if (st->sync && st->hasBer && st->ber >= 5) parts.push_back(std::to_string(st->ber) + " % errors");
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) s += (i ? dot : "") + parts[i];
    return s;
}
/** m17Line: the VTS line and its info as one. */
inline std::string m17Line(const M17State* st) {
    const std::string a = m17Vts(st), b = m17Info(st);
    return b.empty() ? a : a + " \xc2\xb7 " + b;
}
/** The passband to draw (Hz from the dial): M17's 9 kHz channel (m17.ts M17_PASSBAND). */
inline void m17Passband(int& lo, int& hi) { lo = -4500; hi = 4500; }
inline std::string m17StatusMessage() { return "{\"type\":\"m17_status\"}"; }

}  // namespace hh
