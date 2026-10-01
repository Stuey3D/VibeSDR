// vibe_chat_share.h — "share a station" in the canned chat: validate it, NAME it, and say it.
//
// ★★★ THE SENDER'S LABEL NEVER TRAVELS. Stuart, 2026-10-01: a bookmark's name is whatever its owner
//     typed, and the canned chat exists precisely so that nothing anybody types reaches the room
//     ("no custom usernames or anything in the chat"). A share therefore carries a NUMBER, a mode
//     from a closed list, an optional passband — and for DAB a block and a service id. Nothing else
//     is read from the client, so a bookmark called something offensive cannot be broadcast by
//     sharing it.
// ★★★ THE NAME THE ROOM SEES IS THE SERVER'S. What this receiver knows about that frequency: the
//     stations RDS has taught it ("RDS learnt stations keep their names"), the owner's own
//     bookmarks, the EiBi schedule for what is on air NOW, and for DAB the service labels it has
//     decoded off that multiplex. Nothing known = the bare frequency and mode, which is honest.
// ★★ VALIDATED HERE, NOT IN THE CLIENT: a share outside what the radio can reach, a mode the owner
//    switched off, or DAB on a radio/build that cannot decode it is DROPPED — pointing the room at a
//    station this receiver cannot visit is worse than saying nothing.
//
// ★ HEADER-ONLY AND DEPENDENCY-FREE (bar vibe_dab_channels.h, itself a table): the shim adapts its
//   stores into the plain structs below, and vibeserver/test-chat-share.cpp drives every rule here
//   without a radio, a socket or the rest of the core.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "vibe_dab_channels.h"

namespace vibechat {

/** What a client asked to share, AFTER parsing — every field is a number or a closed-list id. */
struct ShareReq {
    bool        dab = false;   // kind "dab": a multiplex (and optionally a service in it)
    double      hz = 0;        // analogue: the frequency; DAB: the block centre (set by validate)
    std::string mode;          // analogue: a mode id from kModes; DAB: "dab"
    bool        hasBw = false; // a passband the bookmark was saved with (Hz offsets from the carrier)
    int         bwLo = 0, bwHi = 0;
    int         block = -1;    // DAB: index into vibedab::kBandIII
    long long   sid = -1;      // DAB: service id, -1 = the multiplex as a whole
    long long   eid = -1;      // DAB: ensemble id when the sender knew it
};

/** One thing this receiver knows the name of. The shim fills these from its bookmark store. */
struct Known {
    std::string name;
    long long   hz = 0;
    std::string mode;          // "dab" for a DAB service
    long long   sid = -1, eid = -1;
    /** 2 = trustworthy (heard and settled over RDS, RadioDNS, decoded off a multiplex, or saved by
     *  the owner); 1 = a guess from a rotating PS; 0 = provisional ("PI4322 93.7MHz") — never used. */
    int         quality = 2;
};

/** ★ THE CLOSED LIST. A mode is an id the clients already draw, never a free string — the old
 *  "check out" accepted any mode text the owner had not blocked and echoed it to the room, which
 *  was a sentence-shaped hole in a vocabulary that is supposed to be closed. */
inline const std::vector<std::string>& kModes() {
    static const std::vector<std::string> v = {
        "wfm", "nfm", "am", "sam", "usb", "lsb", "cw", "cwu", "cwl", "iq",
        "rds", "rtty", "navtex", "wefax", "sstv", "ft8", "time",
    };
    return v;
}
inline bool knownMode(const std::string& m) {
    for (const auto& k : kModes()) if (k == m) return true;
    return false;
}

/** "C0D2", "0xc0d2" or a decimal number -> the value, or -1. Service ids are 16 bits for audio and
 *  32 for data; anything wider (or negative, or with junk after it) is not an id. */
inline long long parseId(const std::string& sIn, bool hexByDefault) {
    std::string s = sIn;
    while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' '))  s.pop_back();
    if (s.empty() || s.size() > 10) return -1;
    int base = hexByDefault ? 16 : 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s = s.substr(2); base = 16; }
    if (s.empty()) return -1;
    for (char c : s) {
        const bool dec = c >= '0' && c <= '9';
        const bool hex = dec || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (base == 10 ? !dec : !hex) return -1;
    }
    const long long v = std::strtoll(s.c_str(), nullptr, base);
    if (v < 0 || v > 0xFFFFFFFFLL) return -1;
    return v;
}

/** ★ ONE FIELD OF A FLAT JSON OBJECT, typed. The shim's jsonStr finds the next quote after the
 *  colon — so on `"sid":49362,"eid":…` it returns `eid`. A share mixes numbers and strings, so it
 *  reads each value as what it actually is. Returns false when the key is absent. */
inline bool readField(const std::string& j, const char* key, bool& isStr, std::string& str, double& num) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = 0;
    while ((p = j.find(pat, p)) != std::string::npos) {
        size_t c = p + pat.size();
        while (c < j.size() && (j[c] == ' ' || j[c] == '\t')) ++c;
        if (c >= j.size() || j[c] != ':') { p += pat.size(); continue; }   // a value, not a key
        ++c;
        while (c < j.size() && (j[c] == ' ' || j[c] == '\t')) ++c;
        if (c >= j.size()) return false;
        if (j[c] == '"') {
            const size_t e = j.find('"', c + 1);
            if (e == std::string::npos) return false;
            isStr = true; str = j.substr(c + 1, e - c - 1); num = 0;
            return true;
        }
        char* end = nullptr;
        num = std::strtod(j.c_str() + c, &end);
        if (end == j.c_str() + c) return false;                           // null, true, junk
        isStr = false; str.clear();
        return true;
    }
    return false;
}

/** ★★ THE REQUEST, READ FIELD BY FIELD — and ONLY these fields. There is deliberately no `name`,
 *  `label` or `text` read here: whatever a client puts in one is never looked at.
 *    {"type":"say","id":"check_out","kind":"bookmark","hz":96600000,"mode":"wfm","bwLo":-1e5,"bwHi":1e5}
 *    {"type":"say","id":"check_out","kind":"dab","block":"12B","sid":"C0D2","eid":"CC15","hz":225648000,"mode":"dab"}
 *  A DAB `sid`/`eid` may be a number or a hex string ("C0D2", "0xC0D2"); an unknown block name, an
 *  unparseable id or a mistyped field is a bad payload — the whole share is dropped. */
inline bool parseRequest(const std::string& msg, ShareReq& r) {
    r = ShareReq{};
    bool s = false; std::string str; double num = 0;
    if (readField(msg, "kind", s, str, num)) {
        if (!s || (str != "dab" && str != "bookmark")) return false;
        r.dab = (str == "dab");
    }
    if (readField(msg, "hz", s, str, num)) { if (s) return false; r.hz = num; }
    if (readField(msg, "mode", s, str, num)) { if (!s) return false; r.mode = str; }
    double lo = 0, hi = 0; bool hasLo = false, hasHi = false;
    if (readField(msg, "bwLo", s, str, num)) { if (s) return false; lo = num; hasLo = true; }
    if (readField(msg, "bwHi", s, str, num)) { if (s) return false; hi = num; hasHi = true; }
    if (hasLo && hasHi && std::isfinite(lo) && std::isfinite(hi) && std::fabs(lo) < 1e7 && std::fabs(hi) < 1e7) {
        r.hasBw = true; r.bwLo = (int)std::lround(lo); r.bwHi = (int)std::lround(hi);
    }
    if (readField(msg, "block", s, str, num)) {
        if (!s) return false;
        const vibedab::Channel* ch = vibedab::channelByName(str.c_str());
        if (!ch) return false;
        r.block = int(ch - vibedab::kBandIII);
        r.dab = true;
    }
    if (readField(msg, "sid", s, str, num)) {
        r.sid = s ? parseId(str, true)
                  : (num >= 1 && num <= 4294967295.0 && num == std::floor(num) ? (long long)num : -1);
        if (r.sid <= 0) return false;
    }
    if (readField(msg, "eid", s, str, num)) {
        r.eid = s ? parseId(str, true)
                  : (num >= 0 && num <= 65535.0 && num == std::floor(num) ? (long long)num : -1);
        if (r.eid < 0) return false;
    }
    if (r.sid > 0) r.dab = true;
    return true;
}

enum class Verdict { Ok, BadPayload, OutOfRange, ModeBlocked, NoDab };

inline const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::Ok:          return "ok";
        case Verdict::BadPayload:  return "bad payload";
        case Verdict::OutOfRange:  return "outside this receiver's range";
        case Verdict::ModeBlocked: return "mode not offered";
        case Verdict::NoDab:       return "no DAB on this receiver";
    }
    return "?";
}

/** What the receiver can do, as the shim knows it. `ranges` = the EFFECTIVE tunable set (hardware
 *  coverage after the owner's allow/block lists) — the same set the directory publishes. */
struct Caps {
    std::vector<std::pair<double, double>> ranges;
    std::function<bool(const std::string&)> modeBlocked;
    bool dabCapable = false;
};

inline bool inRanges(const Caps& c, double hz) {
    for (const auto& r : c.ranges) if (hz >= r.first && hz <= r.second) return true;
    return false;
}

/** ★★ CHECK AND NORMALISE IN ONE PLACE. On Ok, `r` is ready to name and to send: a DAB share has
 *  its block resolved to the table's centre (a client's frequency for a block is never echoed),
 *  an analogue one its mode lower-cased and checked against the closed list. */
inline Verdict validate(ShareReq& r, const Caps& c) {
    for (auto& ch : r.mode) ch = (char)std::tolower((unsigned char)ch);
    if (r.mode == "dab") r.dab = true;
    if (r.dab) {
        if (r.block < 0) {
            // ★ No block name: take the frequency, but only if it IS a block — within 50 kHz of a
            //   centre (the tolerance both clients' dabGoTo already use).
            if (!(r.hz > 0) || !std::isfinite(r.hz)) return Verdict::BadPayload;
            const int i = vibedab::nearestChannel((uint32_t)std::llround(r.hz));
            if (i < 0) return Verdict::BadPayload;
            if (std::fabs(double(vibedab::kBandIII[i].centreHz) - r.hz) > 50000.0) return Verdict::BadPayload;
            r.block = i;
        }
        if (r.block >= int(vibedab::kBandIIICount)) return Verdict::BadPayload;
        if (r.sid == 0 || r.sid > 0xFFFFFFFFLL) return Verdict::BadPayload;
        if (r.eid > 0xFFFF) return Verdict::BadPayload;
        r.hz = double(vibedab::kBandIII[r.block].centreHz);
        r.mode = "dab";
        r.hasBw = false; r.bwLo = r.bwHi = 0;
        if (!c.dabCapable) return Verdict::NoDab;
        if (!inRanges(c, r.hz)) return Verdict::OutOfRange;
        return Verdict::Ok;
    }
    if (!(r.hz > 0) || !std::isfinite(r.hz) || r.hz > 1e11) return Verdict::BadPayload;
    r.hz = std::round(r.hz);
    r.block = -1; r.sid = -1; r.eid = -1;
    // ★ An absent mode is allowed (the old composer's "(mode)"); an unknown one is not.
    if (!r.mode.empty() && !knownMode(r.mode)) return Verdict::BadPayload;
    if (r.hasBw) {
        // ★ A real passband or none: low below high, inside ±500 kHz (WFM's widest is ±150).
        if (!(r.bwHi > r.bwLo) || r.bwLo < -500000 || r.bwHi > 500000) { r.hasBw = false; r.bwLo = r.bwHi = 0; }
    }
    if (!inRanges(c, r.hz)) return Verdict::OutOfRange;
    if (!r.mode.empty() && c.modeBlocked && c.modeBlocked(r.mode)) return Verdict::ModeBlocked;
    return Verdict::Ok;
}

/** How close a known station must be to count as "that frequency", by mode. FM's raster is 100 kHz
 *  (50 in places), so ±20 kHz is the same station and never its neighbour; a voice channel is
 *  12.5 kHz; everything else (MW/SW broadcast on a 5/9/10 kHz raster, SSB, CW, data) is ±1 kHz. */
inline double toleranceHz(const std::string& mode) {
    if (mode == "wfm" || mode == "rds") return 20000.0;
    if (mode == "nfm") return 3000.0;
    return 1000.0;
}

/** "0600-0700 · E" or "0000-2400" -> on air at `utcMin` (minutes past midnight UTC)? A comment with
 *  no time in it says nothing about the hour, which is not the same as "off air": true. */
inline bool eibiOnAir(const std::string& comment, int utcMin) {
    if (comment.size() < 9) return true;
    for (int i = 0; i < 9; ++i) {
        if (i == 4) { if (comment[i] != '-') return true; continue; }
        if (comment[i] < '0' || comment[i] > '9') return true;
    }
    const int a = std::atoi(comment.substr(0, 4).c_str()), b = std::atoi(comment.substr(5, 4).c_str());
    const int s = (a / 100) * 60 + a % 100, e = (b / 100) * 60 + b % 100;
    if (s == e) return true;                          // 0000-0000: all day
    return s < e ? (utcMin >= s && utcMin < e) : (utcMin >= s || utcMin < e);
}

/** Read one JSON string value starting at the opening quote `p`; returns the index after the
 *  closing quote, or npos. Escapes are kept as the two characters they are (we re-escape on output
 *  anyway, and a station name with a backslash in it is not worth a decoder). */
inline size_t readJsonString(const std::string& j, size_t p, std::string& out) {
    out.clear();
    if (p >= j.size() || j[p] != '"') return std::string::npos;
    for (size_t i = p + 1; i < j.size(); ++i) {
        const char c = j[i];
        if (c == '\\') { if (i + 1 < j.size()) { out += j[i + 1]; ++i; } continue; }
        if (c == '"') return i + 1;
        out += c;
    }
    return std::string::npos;
}

/** ★ THE SCHEDULE, AS THE SERVER HOLDS IT: the `[{"name":…,"frequency":…,"comment":"HHMM-HHMM · …"}]`
 *  that eibi.cpp (or the hosting app) publishes for /stations. Scanned in place — the list is a few
 *  thousand short objects and a share is rate-limited to one per three seconds per listener, so a
 *  linear pass costs less than parsing it into a structure that would have to be kept in step.
 *  Returns the name of the nearest entry ON AIR NOW within ±1 kHz, or "". */
inline std::string eibiNameAt(const std::string& json, double hz, int utcMin) {
    std::string best;
    double bestD = 1001.0;
    size_t p = 0;
    while ((p = json.find("\"frequency\"", p)) != std::string::npos) {
        // The enclosing object: back to its '{', forward to its '}'. EiBi names carry no braces.
        const size_t ob = json.rfind('{', p);
        const size_t cb = json.find('}', p);
        size_t c = json.find(':', p);
        p += 11;
        if (ob == std::string::npos || cb == std::string::npos || c == std::string::npos || c > cb) continue;
        const double f = std::strtod(json.c_str() + c + 1, nullptr);
        const double d = std::fabs(f - hz);
        if (!(d <= 1000.0) || d >= bestD) continue;
        const std::string obj = json.substr(ob, cb - ob + 1);
        std::string name, comment;
        size_t n = obj.find("\"name\"");
        if (n == std::string::npos) continue;
        n = obj.find('"', obj.find(':', n));
        if (n == std::string::npos || readJsonString(obj, n, name) == std::string::npos || name.empty()) continue;
        size_t k = obj.find("\"comment\"");
        if (k != std::string::npos) {
            k = obj.find('"', obj.find(':', k));
            if (k != std::string::npos) readJsonString(obj, k, comment);
        }
        if (!eibiOnAir(comment, utcMin)) continue;
        best = name; bestD = d;
    }
    return best;
}

/** What the room will be told the station is called. */
struct Named {
    std::string name;          // "" = nothing known
    std::string ensemble;      // DAB: the multiplex's own label, when the receiver remembers it
    const char* source = "";   // "station" (bookmarks / RDS / DAB) | "eibi" | ""
};

/** ★★ NEAREST TRUSTWORTHY NAME WINS. Among the known stations within tolerance, the closest; a tie
 *  goes to the better-sourced one. A provisional "PI4322 93.7MHz" label is not a name and is never
 *  used. A DAB share matches on the SERVICE ID on that block (every service shares the frequency),
 *  and an ensemble id both sides know that disagrees means a different multiplex: no name.
 *  EiBi is consulted only below 30 MHz, only when nothing better is known, and only for what is
 *  on air now — a timetable entry for a broadcast that ended at noon is not what you are hearing. */
inline Named resolveName(const ShareReq& r, const std::vector<Known>& known,
                         const std::string* eibiJson, int utcMin,
                         const std::string& ensembleLabel = std::string()) {
    Named out;
    if (r.dab) {
        out.ensemble = ensembleLabel;
        if (r.sid <= 0) return out;
        for (const auto& k : known) {
            if (k.mode != "dab" || k.sid != r.sid || k.quality < 2 || k.name.empty()) continue;
            if (std::llabs(k.hz - (long long)std::llround(r.hz)) > 1000) continue;
            if (r.eid >= 0 && k.eid >= 0 && r.eid != k.eid) continue;
            out.name = k.name; out.source = "station";
            return out;
        }
        return out;
    }
    const double tol = toleranceHz(r.mode);
    const Known* best = nullptr;
    double bestD = 0;
    for (const auto& k : known) {
        if (k.mode == "dab" || k.quality < 1 || k.name.empty()) continue;
        const double d = std::fabs(double(k.hz) - r.hz);
        if (d > tol) continue;
        if (!best || d < bestD - 0.5 || (std::fabs(d - bestD) <= 0.5 && k.quality > best->quality)) {
            best = &k; bestD = d;
        }
    }
    if (best) { out.name = best->name; out.source = "station"; return out; }
    if (eibiJson && r.hz < 30e6 && r.mode != "wfm" && r.mode != "nfm") {
        out.name = eibiNameAt(*eibiJson, r.hz, utcMin);
        if (!out.name.empty()) out.source = "eibi";
    }
    return out;
}

/** The label every client draws for a mode — the same words as web chat.ts / src chatShare.ts. */
inline std::string modeLabel(const std::string& m) {
    static const std::pair<const char*, const char*> t[] = {
        {"wfm","WFM"},{"nfm","NFM"},{"am","AM"},{"sam","SAM"},{"usb","USB"},{"lsb","LSB"},{"cw","CW"},
        {"cwu","CW-U"},{"cwl","CW-L"},{"iq","IQ"},{"dab","DAB"},{"rds","Advanced RDS"},{"rtty","RTTY"},
        {"navtex","NAVTEX"},{"wefax","WEFAX"},{"sstv","SSTV"},{"ft8","FT8 / FT4"},{"time","Time signal"},
    };
    for (const auto& p : t) if (m == p.first) return p.second;
    return std::string();
}

/** "96.600 MHz" at and above 1 MHz, "198 kHz" (or "7.5 kHz") below — integer maths, no drift. */
inline std::string freqText(double hz) {
    char b[48];
    const long long h = (long long)std::llround(hz);
    if (h >= 1000000) {
        snprintf(b, sizeof b, "%lld.%03lld MHz", h / 1000000, (h % 1000000) / 1000);
    } else if (h % 1000 == 0) {
        snprintf(b, sizeof b, "%lld kHz", h / 1000);
    } else {
        snprintf(b, sizeof b, "%.1f kHz", h / 1000.0);
    }
    return b;
}

/** ★★ THE FALLBACK LINE, built from SERVER facts only. An older client that cannot draw the
 *  structured share can show this as an ordinary line; a new one draws its own from the fields.
 *    "shared 96.600 MHz WFM — Heart"
 *    "shared DAB Heart — 12B (225.648 MHz)"   /   "shared DAB 12B (225.648 MHz) — BBC National DAB" */
inline std::string fallbackText(const ShareReq& r, const Named& n) {
    if (r.dab) {
        const auto& ch = vibedab::kBandIII[r.block];
        const std::string where = std::string(ch.name) + " (" + freqText(ch.centreHz) + ")";
        if (!n.name.empty()) return "shared DAB " + n.name + " \xE2\x80\x94 " + where;
        if (!n.ensemble.empty()) return "shared DAB " + where + " \xE2\x80\x94 " + n.ensemble;
        return "shared DAB " + where;
    }
    std::string s = "shared " + freqText(r.hz);
    const std::string ml = modeLabel(r.mode);
    if (!ml.empty()) s += " " + ml;
    if (!n.name.empty()) s += " \xE2\x80\x94 " + n.name;
    return s;
}

/** The minimal JSON escaper the test uses; the shim passes its own UTF-8-cleaning jsonEscape. */
inline std::string plainEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c >= 0x20) o += c;
    }
    return o;
}

/** ★★ THE LINE THE ROOM RECEIVES. Keeps the top-level `hz` and `mode` the "check out" clients
 *  before this already read — an old browser draws "Hey, check out 96.6 MHz WFM" and its tap still
 *  works — and adds the structured share beside them:
 *    {"type":"said","from":3,"id":"check_out","hz":96600000,"mode":"wfm",
 *     "kind":"bookmark","bwLo":-100000,"bwHi":100000,"name":"Heart","nameSrc":"station",
 *     "text":"shared 96.600 MHz WFM — Heart"}
 *    {…,"kind":"dab","hz":225648000,"mode":"dab","block":"12B","sid":49362,"eid":52245,
 *     "name":"Heart","ensemble":"D1 National","text":"shared DAB Heart — 12B (225.648 MHz)"}
 *  ★ `name`, `ensemble` and `text` are written HERE from server facts; none is copied from the
 *    request, which carries no text at all. */
inline std::string saidJson(int from, bool admin, const ShareReq& r, const Named& n,
                            const std::function<std::string(const std::string&)>& esc = plainEscape) {
    std::string j = "{\"type\":\"said\",\"from\":" + std::to_string(from);
    if (admin) j += ",\"admin\":true";
    j += ",\"id\":\"check_out\"";
    char b[96];
    snprintf(b, sizeof b, ",\"hz\":%lld", (long long)std::llround(r.hz));
    j += b;
    if (!r.mode.empty()) j += ",\"mode\":\"" + r.mode + "\"";     // closed list — no escaping needed
    j += std::string(",\"kind\":\"") + (r.dab ? "dab" : "bookmark") + "\"";
    if (r.hasBw) { snprintf(b, sizeof b, ",\"bwLo\":%d,\"bwHi\":%d", r.bwLo, r.bwHi); j += b; }
    if (r.dab) {
        j += std::string(",\"block\":\"") + vibedab::kBandIII[r.block].name + "\"";
        if (r.sid > 0) { snprintf(b, sizeof b, ",\"sid\":%lld", r.sid); j += b; }
        if (r.eid >= 0) { snprintf(b, sizeof b, ",\"eid\":%lld", r.eid); j += b; }
        if (!n.ensemble.empty()) j += ",\"ensemble\":\"" + esc(n.ensemble) + "\"";
    }
    if (!n.name.empty()) j += ",\"name\":\"" + esc(n.name) + "\",\"nameSrc\":\"" + n.source + "\"";
    j += ",\"text\":\"" + esc(fallbackText(r, n)) + "\"";
    return j + "}";
}

}  // namespace vibechat
