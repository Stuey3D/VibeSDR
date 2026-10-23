// vibe_synop.h — WEATHER REPORTS IN PLAIN ENGLISH: the RTTY decoder's SYNOP (FM 12) and SHIP (FM 13) translator (V12, 2026-10-23).
//
// ★★★ WHY (Stuart, 2026-10-22: "if you can do it that would be great"). DWD's weather RTTY — DDK2 4583 kHz, 50 Bd / 450 Hz,
//     heard on the Lenovo — is mostly WMO coded reports in five-figure groups: "10384 41/96 82508 10118 20072 30115 40206
//     57012 8807/". The decoder reads it right, and to a listener it looks like garbage. This turns each report into one line
//     — "10384 · Tempelhof, Germany · 06:00 UTC · wind 250° 8 kt (15 km/h) · 11.8 °C · 1020.6 hPa" — that rides BESIDE the
//     raw groups, never instead of them.
// ★★★ ON THE SERVER (Stuart: "a much wider range of devices can listen than can host"). The decoder host runs this over the
//     text its RTTY decoder prints and sends each finished report as its own frame (0x08 — vibe_decoder_host.h); the
//     clients only show what arrives, and the gallery writes the same line under the report in its .txt.
// ★★★ NEVER A VALUE FROM A MALFORMED GROUP. RTTY fades: a group arrives as "4005", "46/9_", "1O15". A group that is not five
//     figures (or that breaks its own code table, or is physically impossible — a 61 °C sea) is UNREADABLE: that group
//     alone is skipped and counted ("1 group unreadable"); the rest of the report still reads. The raw text is untouched.
//     The order is enforced too: section 1's groups come in indicator order (1…9), so a group out of order is not
//     guessed at — a lost "333" makes the section-3 groups after it unreadable rather than misread as section 1.
// ★★ STATION NAMES come only from vibe_synop_stations.h (NOAA NCEI's ISD station list — public domain). Not in it: the
//    number alone.
// ★★ BOUNDED, CHEAP — a Pi 2 is the target. One report's tokens at most (kMaxTokens × kMaxTok), a handful of finished
//    reports waiting for their line to end; parsing is a few comparisons a character, on the decoder thread.
//
// ── What a bulletin looks like on the air ──────────────────────────────────────────────────────────────────────
//     ZCZC 123
//     SMDL01 EDZW 230600            ← the WMO heading: ignored
//     AAXX 23064                    ← SYNOP: day 23, 06 UTC, iw 4 = wind in knots, measured (0/1 m/s, 3/4 kt)
//     10384 41/96 82508 10118 20072 30115 40206 57012 8807/ 333 10173 20065=
//     10385 NIL=
//     NNNN
//     BBXX                          ← SHIP: every report carries its own time and position
//     DBBH 23064 99543 10079 41/96 …=
//  ★ '=' ends a report. DWD's '=' is the FIGURES 'V' and this server's ITA2 table prints it as ';' (fsk_decoder.cpp), so
//    ';' ends one too. A report may run over several lines.
//  ★ A report met with no AAXX before it (the listener tuned in mid-bulletin) is still read when it is plainly a SYNOP —
//    starts a line, five figures, then a valid iRiXhVV and Nddff, ended by '=' — but its time and wind unit are NOT known
//    and it says so ("time not received", "46 (unit not received)"). Nothing is carried over from an older bulletin.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "vibe_synop_stations.h"

namespace vibe {
namespace synop {

struct Report {
    std::string kind;                 ///< "synop" | "ship"
    std::string raw;                  ///< the groups as received, one space apart
    std::string line;                 ///< the compact translation
    std::vector<std::string> details; ///< one plain line per thing the groups say
    int unreadable = 0;               ///< groups skipped as malformed
    int values = 0;                   ///< things actually read — 0 = nothing to say (the reader drops it; NIL is 1)
    /** The frame body the decoder socket carries (0x08 — see vibe_decoder_host.h). */
    std::string json() const;
};

// ── The WMO code tables, in short plain words (WMO-No. 306, Manual on Codes, Vol. I.1) ──────────────────────────────
/** Present weather ww, code table 4677 (manned station, or iX 4). */
inline const char* ww4677(int c) {
    static const char* const t[100] = {
        "cloud development not observed", "clouds dissolving", "sky unchanged", "clouds forming", "smoke haze", "haze",
        "dust in the air", "dust or sand raised by wind", "dust or sand whirls", "duststorm or sandstorm in sight",
        "mist", "patches of shallow fog", "shallow fog", "lightning, no thunder", "precipitation in sight, not reaching the ground",
        "precipitation in sight, distant", "precipitation in sight, nearby", "thunder, no precipitation", "squalls", "funnel cloud",
        "recent drizzle", "recent rain", "recent snow", "recent rain and snow", "recent freezing drizzle or rain",
        "recent rain showers", "recent snow showers", "recent hail showers", "recent fog", "recent thunderstorm",
        "slight or moderate duststorm, decreasing", "slight or moderate duststorm", "slight or moderate duststorm, increasing",
        "severe duststorm, decreasing", "severe duststorm", "severe duststorm, increasing",
        "slight drifting snow", "heavy drifting snow", "slight blowing snow", "heavy blowing snow",
        "fog at a distance", "fog patches", "fog thinning, sky visible", "fog thinning", "fog, sky visible", "fog",
        "fog thickening, sky visible", "fog thickening", "freezing fog, sky visible", "freezing fog",
        "intermittent slight drizzle", "slight drizzle", "intermittent moderate drizzle", "moderate drizzle",
        "intermittent heavy drizzle", "heavy drizzle", "slight freezing drizzle", "freezing drizzle",
        "slight drizzle and rain", "drizzle and rain",
        "intermittent slight rain", "slight rain", "intermittent moderate rain", "moderate rain", "intermittent heavy rain",
        "heavy rain", "slight freezing rain", "freezing rain", "slight rain and snow", "rain and snow",
        "intermittent slight snow", "slight snow", "intermittent moderate snow", "moderate snow", "intermittent heavy snow",
        "heavy snow", "diamond dust", "snow grains", "snow crystals", "ice pellets",
        "slight rain showers", "rain showers", "violent rain showers", "slight rain and snow showers", "rain and snow showers",
        "slight snow showers", "snow showers", "slight showers of snow pellets or small hail",
        "showers of snow pellets or small hail", "slight hail showers",
        "hail showers", "slight rain, thunder in the past hour", "rain, thunder in the past hour",
        "slight snow or hail, thunder in the past hour", "snow or hail, thunder in the past hour",
        "thunderstorm with rain or snow", "thunderstorm with hail", "heavy thunderstorm with rain or snow",
        "thunderstorm with duststorm", "heavy thunderstorm with hail" };
    return c >= 0 && c < 100 ? t[c] : nullptr;
}
/** Present weather wawa, code table 4680 (automatic station, iX 7). nullptr = a code the table reserves. */
inline const char* ww4680(int c) {
    switch (c) {
    case 0: return "no significant weather"; case 1: return "clouds dissolving"; case 2: return "sky unchanged";
    case 3: return "clouds forming"; case 4: return "haze, smoke or dust"; case 5: return "haze, smoke or dust, visibility under 1 km";
    case 10: return "mist"; case 11: return "diamond dust"; case 12: return "distant lightning"; case 18: return "squalls";
    case 20: return "recent fog"; case 21: return "recent precipitation"; case 22: return "recent drizzle or snow grains";
    case 23: return "recent rain"; case 24: return "recent snow"; case 25: return "recent freezing drizzle or rain";
    case 26: return "recent thunderstorm"; case 27: return "blowing or drifting snow or sand";
    case 28: return "blowing or drifting snow or sand, visibility 1 km or more";
    case 29: return "blowing or drifting snow or sand, visibility under 1 km";
    case 30: return "fog"; case 31: return "fog patches"; case 32: return "fog thinning"; case 33: return "fog";
    case 34: return "fog thickening"; case 35: return "freezing fog";
    case 40: return "precipitation"; case 41: return "slight or moderate precipitation"; case 42: return "heavy precipitation";
    case 43: return "slight or moderate rain or drizzle"; case 44: return "heavy rain or drizzle";
    case 45: return "slight or moderate snow"; case 46: return "heavy snow";
    case 47: return "slight or moderate freezing precipitation"; case 48: return "heavy freezing precipitation";
    case 50: return "drizzle"; case 51: return "slight drizzle"; case 52: return "moderate drizzle"; case 53: return "heavy drizzle";
    case 54: return "slight freezing drizzle"; case 55: return "moderate freezing drizzle"; case 56: return "heavy freezing drizzle";
    case 57: return "slight drizzle and rain"; case 58: return "drizzle and rain";
    case 60: return "rain"; case 61: return "slight rain"; case 62: return "moderate rain"; case 63: return "heavy rain";
    case 64: return "slight freezing rain"; case 65: return "moderate freezing rain"; case 66: return "heavy freezing rain";
    case 67: return "slight rain and snow"; case 68: return "rain and snow";
    case 70: return "snow"; case 71: return "slight snow"; case 72: return "moderate snow"; case 73: return "heavy snow";
    case 74: return "slight ice pellets"; case 75: return "moderate ice pellets"; case 76: return "heavy ice pellets";
    case 77: return "snow grains"; case 78: return "ice crystals";
    case 80: return "showers"; case 81: return "slight rain showers"; case 82: return "moderate rain showers";
    case 83: return "heavy rain showers"; case 84: return "violent rain showers"; case 85: return "slight snow showers";
    case 86: return "moderate snow showers"; case 87: return "heavy snow showers"; case 89: return "hail";
    case 90: return "thunderstorm"; case 91: return "slight or moderate thunderstorm"; case 92: return "thunderstorm with showers";
    case 93: return "thunderstorm with hail"; case 94: return "heavy thunderstorm"; case 95: return "heavy thunderstorm with showers";
    case 96: return "heavy thunderstorm with hail"; case 99: return "tornado";
    default: return nullptr;
    }
}
/** Past weather W1 W2, code table 4561 (manned). */
inline const char* past4561(int c) {
    static const char* const t[10] = { "cloud half or less", "variable cloud", "cloud more than half",
        "sandstorm, duststorm or blowing snow", "fog or thick haze", "drizzle", "rain", "snow, or rain and snow", "showers",
        "thunderstorm" };
    return c >= 0 && c < 10 ? t[c] : nullptr;
}
/** Past weather Wa1 Wa2, code table 4531 (automatic). */
inline const char* past4531(int c) {
    static const char* const t[10] = { "no significant weather", "visibility reduced", "blowing snow or sand", "fog",
        "precipitation", "drizzle", "rain", "snow or ice pellets", "showers", "thunderstorm" };
    return c >= 0 && c < 10 ? t[c] : nullptr;
}
/** Low, middle and high cloud (code tables 0513, 0515, 0509). Index 1…9; 0 = none of that kind. */
inline const char* cloudLow(int c) {
    static const char* const t[10] = { "no low cloud", "fair-weather cumulus", "towering cumulus", "cumulonimbus",
        "stratocumulus from cumulus", "stratocumulus", "stratus", "ragged cloud of bad weather", "cumulus and stratocumulus",
        "cumulonimbus with anvil" };
    return c >= 0 && c < 10 ? t[c] : nullptr;
}
inline const char* cloudMid(int c) {
    static const char* const t[10] = { "no middle cloud", "thin altostratus", "thick altostratus or nimbostratus",
        "thin altocumulus", "patchy altocumulus", "spreading altocumulus", "altocumulus from cumulus",
        "altocumulus with altostratus", "altocumulus castellanus", "altocumulus of a chaotic sky" };
    return c >= 0 && c < 10 ? t[c] : nullptr;
}
inline const char* cloudHigh(int c) {
    static const char* const t[10] = { "no high cloud", "cirrus", "dense cirrus", "cirrus from cumulonimbus",
        "spreading cirrus", "cirrostratus, low in the sky", "cirrostratus, high in the sky", "cirrostratus over the whole sky",
        "cirrostratus", "cirrocumulus" };
    return c >= 0 && c < 10 ? t[c] : nullptr;
}

// ── small helpers ──────────────────────────────────────────────────────────────────────────────────────────────────
inline std::string fmt(const char* f, double v) { char b[48]; std::snprintf(b, sizeof b, f, v); return b; }
inline std::string fmtI(const char* f, long v) { char b[48]; std::snprintf(b, sizeof b, f, v); return b; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
/** Five characters, every one a figure or '/' — the only shape a group may have. */
inline bool isGroup(const std::string& g) {
    if (g.size() != 5) return false;
    for (char c : g) if (!isDigit(c) && c != '/') return false;
    return true;
}
inline bool allDigits(const std::string& s, size_t from = 0, size_t n = std::string::npos) {
    if (n == std::string::npos) n = s.size() - from;
    if (from + n > s.size() || n == 0) return false;
    for (size_t i = from; i < from + n; ++i) if (!isDigit(s[i])) return false;
    return true;
}
inline int num(const std::string& s, size_t from, size_t n) { int v = 0; for (size_t i = from; i < from + n; ++i) v = v * 10 + (s[i] - '0'); return v; }
inline bool isWord(const std::string& t) {   // two or more letters and nothing else — text, not a group
    if (t.size() < 2) return false;
    for (char c : t) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    return true;
}
/** ★ Letters that are FIGURES seen through a lost FIGS shift: the top row (Q…P = 1…0) and X '/', Z '"', V ';' (DWD's
 *  '='), M '.'. In a bulletin a "word" made only of these is a damaged GROUP ("RPPYU" = 40067), not text — DWD's own
 *  synop lines do this on a fade (fsk_decoder.cpp's note on the provisional shift). It is never converted back: it is
 *  just not allowed to end the bulletin. */
inline bool figureRowWord(const std::string& t) {
    for (char c : t) if (!std::strchr("QWERTYUIOPXZVM", c)) return false;
    return !t.empty();
}
/** Text inside a bulletin: a word that cannot be shift damage. */
inline bool isText(const std::string& t) { return isWord(t) && t != "NIL" && !figureRowWord(t); }
inline std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
inline std::string Report::json() const {
    std::string o = "{\"type\":\"weather_report\",\"kind\":" + jsonStr(kind) + ",\"raw\":" + jsonStr(raw)
                  + ",\"line\":" + jsonStr(line) + ",\"unreadable\":" + std::to_string(unreadable) + ",\"details\":[";
    for (size_t i = 0; i < details.size(); ++i) { if (i) o += ','; o += jsonStr(details[i]); }
    return o + "]}";
}

/** Visibility VV, code table 4377, in km — false for 51–55 (not used) or "//". */
inline bool visibilityKm(const std::string& vv, double& km, std::string& words) {
    if (!allDigits(vv)) return false;
    const int v = num(vv, 0, 2);
    if (v == 0) { km = 0.0; words = "under 100 m"; return true; }
    if (v <= 50) { km = v / 10.0; words = fmt("%.1f km", km); return true; }
    if (v <= 55) return false;
    if (v <= 80) { km = v - 50; words = fmtI("%ld km", (long)km); return true; }
    if (v <= 88) { km = 30 + (v - 80) * 5; words = fmtI("%ld km", (long)km); return true; }
    if (v == 89) { km = 70; words = "over 70 km"; return true; }
    static const char* const sea[10] = { "under 50 m", "50 m", "200 m", "500 m", "1 km", "2 km", "4 km", "10 km", "20 km", "50 km or more" };
    static const double seaKm[10] = { 0.0, 0.05, 0.2, 0.5, 1, 2, 4, 10, 20, 50 };
    km = seaKm[v - 90]; words = sea[v - 90]; return true;
}
inline const char* cloudBase1600(char h) {
    static const char* const t[10] = { "under 50 m", "50–100 m", "100–200 m", "200–300 m", "300–600 m", "600–1000 m",
        "1000–1500 m", "1500–2000 m", "2000–2500 m", "2500 m or more, or no cloud" };
    return isDigit(h) ? t[h - '0'] : nullptr;
}
inline std::string octas(char n) {
    if (n == '9') return "sky obscured";
    if (n >= '0' && n <= '8') return std::string(1, n) + "/8";
    return "";
}

// ── One report, from its groups ─────────────────────────────────────────────────────────────────────────────────
struct Header {
    bool known = false;     ///< the YYGGi(w) was read
    int day = -1, hour = -1;
    int iw = -1;            ///< 0/1 m/s, 3/4 kt, -1 not known
};

/** ★ Parse YYGGi — the AAXX header's (or a SHIP report's own). False = unreadable. */
inline bool readTimeGroup(const std::string& g, Header& h) {
    if (!isGroup(g) || !allDigits(g, 0, 4)) return false;
    const int yy = num(g, 0, 2), gg = num(g, 2, 2);
    if (yy < 1 || yy > 31 || gg > 23) return false;
    h.known = true; h.day = yy; h.hour = gg;
    h.iw = (g[4] == '0' || g[4] == '1' || g[4] == '3' || g[4] == '4') ? g[4] - '0' : -1;
    return true;
}

/** The translation of one report's tokens. `tok[0]` is the station (SYNOP) or the call sign (SHIP). */
inline Report translate(const std::vector<std::string>& tok, const Header& bulletin, bool ship) {
    Report r;
    r.kind = ship ? "ship" : "synop";
    for (size_t i = 0; i < tok.size(); ++i) { if (i) r.raw += ' '; r.raw += tok[i]; }
    std::vector<std::string> line;                    // the compact line's pieces, in order
    std::vector<std::string> bad;                     // the unreadable groups, as received
    auto unreadable = [&](const std::string& g) { r.unreadable++; bad.push_back(g); };
    auto& det = r.details;
    Header h = bulletin;
    size_t i = 0;

    // ── Section 0: who and when ──
    if (!ship) {
        const std::string& id = tok.empty() ? std::string() : tok[0];
        if (id.size() == 5 && allDigits(id)) {
            std::string name, country;
            if (synop_stations::lookup((uint32_t)num(id, 0, 5), name, country)) {
                line.push_back(id + " \xc2\xb7 " + name + (country.empty() ? "" : ", " + country));
                det.push_back("Station " + id + ": " + name + (country.empty() ? "" : ", " + country));
            } else {
                line.push_back(id);
                det.push_back("Station " + id + " (not in the public station list)");
            }
        } else { line.push_back("station unreadable"); if (!tok.empty()) unreadable(tok[0]); }
        i = 1;
    } else {
        const std::string call = tok.empty() ? std::string() : tok[0];
        line.push_back(call.empty() ? "ship" : call);
        bool named = false;
        if (call.size() == 5 && allDigits(call)) {   // ★ a buoy or fixed platform (A1bwnbnbnb)
            std::string name, country;
            if (synop_stations::lookup((uint32_t)num(call, 0, 5), name, country)) {
                line.back() += " \xc2\xb7 " + name + (country.empty() ? "" : ", " + country);
                det.push_back("Platform " + call + ": " + name + (country.empty() ? "" : ", " + country)); named = true;
            }
        }
        if (!named) det.push_back((call.size() == 5 && allDigits(call) ? "Buoy or platform " : "Ship ") + call);
        i = 1;
        h = Header{};
        if (i < tok.size()) { if (!readTimeGroup(tok[i], h)) unreadable(tok[i]); ++i; }
        // 99LaLaLa QcLoLoLoLo
        double lat = NAN, lon = NAN;
        if (i < tok.size() && tok[i].size() == 5 && tok[i][0] == '9' && tok[i][1] == '9') {
            const std::string& la = tok[i];
            if (allDigits(la) && num(la, 2, 3) <= 900) lat = num(la, 2, 3) / 10.0; else unreadable(la);
            ++i;
            if (i < tok.size()) {
                const std::string& lo = tok[i];
                const char q = lo.empty() ? 0 : lo[0];
                if (lo.size() == 5 && allDigits(lo) && (q == '1' || q == '3' || q == '5' || q == '7') && num(lo, 1, 4) <= 1800) {
                    lon = num(lo, 1, 4) / 10.0;
                    if (q == '3' || q == '5') lat = -lat;          // ★ Qc: 1 NE, 3 SE, 5 SW, 7 NW
                    if (q == '5' || q == '7') lon = -lon;
                } else { unreadable(lo); lat = NAN; }
                ++i;
            }
        }
        if (std::isfinite(lat) && std::isfinite(lon)) {
            const std::string pos = fmt("%.1f\xc2\xb0", std::fabs(lat)) + (lat < 0 ? "S " : "N ") + fmt("%.1f\xc2\xb0", std::fabs(lon)) + (lon < 0 ? "W" : "E");
            line.push_back(pos);
            det.push_back("Position " + pos);
        }
    }
    const size_t base = det.size() - (ship && !det.empty() && det.back().rfind("Position", 0) == 0 ? 1 : 0);
    if (h.known) {
        line.push_back(fmtI("%02ld:00 UTC", h.hour));
        det.push_back(fmtI("Observed on day %ld of the month", h.day) + fmtI(" at %02ld:00 UTC", h.hour));
    } else {
        line.push_back("time not received");
    }
    if (i < tok.size() && tok[i] == "NIL") {
        line.push_back("no report (NIL)");
        r.values = 1;
        r.line.clear();
        for (size_t k = 0; k < line.size(); ++k) { if (k) r.line += " \xc2\xb7 "; r.line += line[k]; }
        return r;
    }

    // ── Section 1 ──
    // iRiXhVV
    int iX = -1;
    if (i < tok.size()) {
        const std::string& g = tok[i];
        // ★ The WHOLE group is checked before any of it is believed — a malformed visibility makes the cloud base beside
        //   it suspect too (one misread figure can be any of the five).
        double km = 0; std::string vw;
        bool ok = isGroup(g) && (g[0] == '/' || (g[0] >= '0' && g[0] <= '4')) && g[1] >= '1' && g[1] <= '7';
        const bool vvMissing = ok && g[3] == '/' && g[4] == '/';
        if (ok && !vvMissing && !visibilityKm(g.substr(3, 2), km, vw)) ok = false;
        if (!ok) unreadable(g);
        else {
            iX = g[1] - '0';
            if (g[0] == '3') det.push_back("No precipitation");
            if (const char* cb = cloudBase1600(g[2])) det.push_back(std::string("Lowest cloud ") + cb);
            if (!vvMissing) det.push_back("Visibility " + vw);
            det.push_back(iX >= 4 ? "Automatic station" : "Manned station");
        }
        ++i;
    }
    // Nddff (+ 00fff when ff is 99 or more)
    std::string windUnit = h.iw == 0 || h.iw == 1 ? "m/s" : h.iw == 3 || h.iw == 4 ? "kt" : "";
    auto speedWords = [&](int ff) -> std::string {
        if (windUnit == "kt") return fmtI("%ld kt", ff) + fmtI(" (%ld km/h)", std::lround(ff * 1.852));
        if (windUnit == "m/s") return fmtI("%ld m/s", ff) + fmtI(" (%ld kt", std::lround(ff * 1.943844)) + fmtI(", %ld km/h)", std::lround(ff * 3.6));
        return fmtI("%ld (unit not received)", ff);
    };
    if (i < tok.size()) {
        const std::string& g = tok[i];
        ++i;
        bool ok = isGroup(g) && (isDigit(g[0]) || g[0] == '/');
        int dd = -1, ff = -1;
        if (ok && allDigits(g, 1, 4)) {
            dd = num(g, 1, 2); ff = num(g, 3, 2);
            if (dd > 36 && dd != 99) ok = false;
            if (ok && ff == 99) {   // ★ 99 or more: the speed is in the 00fff group that follows
                const std::string x = i < tok.size() ? tok[i] : std::string();
                if (isGroup(x) && x[0] == '0' && x[1] == '0' && allDigits(x, 2, 3)) { ff = num(x, 2, 3); ++i; }
                else ok = false;
            }
        } else if (ok && g.substr(1) != "////") ok = false;
        if (!ok) unreadable(g);
        else {
            const std::string c = g[0] == '/' ? std::string() : octas(g[0]);
            if (!c.empty()) det.push_back("Cloud cover " + c);
            if (dd >= 0) {
                std::string w;
                if (dd == 0 && ff == 0) w = "calm";
                else if (dd == 99) w = "variable " + speedWords(ff);
                else w = fmtI("%ld\xc2\xb0 ", dd * 10) + speedWords(ff);
                line.push_back(w == "calm" ? "calm" : "wind " + w);
                det.push_back(w == "calm" ? "Wind calm" : "Wind " + w);
            }
        }
    }

    // Groups 1…9 of section 1, then the sections after it
    enum Sec { S1, S2, S3, SOther };
    Sec sec = S1;
    int last = 0;                         // ★ the last indicator read in this section — they come in order
    std::string temp, pressMsl, pressStn, wx;
    auto tempOf = [](const std::string& g, double& c) -> bool {   // snTTT, sn 0 = +, 1 = −
        if (!allDigits(g, 2, 3) || (g[1] != '0' && g[1] != '1')) return false;
        c = num(g, 2, 3) / 10.0 * (g[1] == '1' ? -1 : 1);
        return c > -80.0 && c < 60.0;
    };
    for (; i < tok.size(); ++i) {
        const std::string& g = tok[i];
        // section markers
        if (g == "222" || (g.size() == 5 && g.compare(0, 3, "222") == 0 && sec == S1)) {
            sec = S2; last = -1;
            if (ship && g.size() == 5 && isDigit(g[3]) && isDigit(g[4])) {
                static const char* const dir[10] = { "stationary", "heading NE", "heading E", "heading SE", "heading S",
                                                     "heading SW", "heading W", "heading NW", "heading N", "course unknown" };
                static const char* const spd[10] = { "0 kt", "1–5 kt", "6–10 kt", "11–15 kt", "16–20 kt", "21–25 kt",
                                                     "26–30 kt", "31–35 kt", "36–40 kt", "over 40 kt" };
                det.push_back(std::string("Ship ") + dir[g[3] - '0'] + (g[3] == '0' ? "" : std::string(", ") + spd[g[4] - '0']));
            }
            continue;
        }
        if (g == "333") { sec = S3; last = -1; continue; }
        if (g == "444" || g == "555" || g == "666") { sec = SOther; continue; }
        if (sec == SOther) continue;      // ★ national and cloud-below sections: raw only
        if (!isGroup(g)) { unreadable(g); continue; }
        if (!isDigit(g[0])) { unreadable(g); continue; }
        const int ind = g[0] - '0';
        // ★ In order: section 1 never repeats an indicator; section 3 repeats only 8 and 9 (several cloud layers, several
        //   special phenomena). Out of order = something was lost — not guessed at.
        const bool repeatOk = sec == S3 && (ind == 8 || ind == 9) && ind == last;
        if (ind < last || (ind == last && !repeatOk)) { unreadable(g); continue; }
        last = ind;
        if (sec == S1) {
            double c;
            switch (ind) {
            case 1: if (g.substr(1) == "////") break;
                    if (tempOf(g, c)) { temp = fmt("%.1f \xc2\xb0""C", c); det.push_back("Air temperature " + temp); } else unreadable(g); break;
            case 2: if (g.substr(1) == "////") break;
                    if (g[1] == '9' && allDigits(g, 2, 3) && num(g, 2, 3) <= 100) det.push_back(fmtI("Relative humidity %ld %%", num(g, 2, 3)));
                    else if (tempOf(g, c)) det.push_back("Dew point " + fmt("%.1f \xc2\xb0""C", c));
                    else unreadable(g);
                    break;
            case 3: case 4: {
                if (g.substr(1) == "////") break;
                if (!allDigits(g, 1, 4)) { unreadable(g); break; }
                const int p = num(g, 1, 4);
                if (ind == 4 && g[1] != '0' && g[1] != '9') {   // ★ 4a3hhh: a high station gives a standard level's height instead
                    static const int lvl[10] = { 0, 1000, 925, 0, 0, 500, 0, 700, 850, 0 };
                    const int a3 = g[1] - '0';
                    if (lvl[a3]) det.push_back(fmtI("Height of the %ld hPa level ", lvl[a3]) + fmtI("%03ld (geopotential metres, last figures)", num(g, 2, 3)));
                    else unreadable(g);
                    break;
                }
                const double hpa = (g[1] == '0' ? 10000 + p : p) / 10.0;
                const bool sane = ind == 4 ? (hpa >= 870.0 && hpa <= 1090.0) : (hpa >= 500.0 && hpa <= 1090.0);
                if (!sane) { unreadable(g); break; }
                const std::string s = fmt("%.1f hPa", hpa);
                if (ind == 4) { pressMsl = s; det.push_back("Pressure at sea level " + s); }
                else { pressStn = s; det.push_back("Pressure at the station " + s); }
                break;
            }
            case 5: {
                if (g.substr(1) == "////") break;
                if (!allDigits(g, 1, 4) || g[1] == '9') { unreadable(g); break; }
                const int a = g[1] - '0'; const double d = num(g, 2, 3) / 10.0;
                const char* trend = a == 4 ? "steady" : a < 4 ? "risen" : "fallen";
                static const char* const shape[9] = { "rising then falling", "rising then steady", "rising", "falling or steady, then rising",
                                                      "steady", "falling then rising", "falling then steady", "falling", "steady or rising, then falling" };
                // ★ The shape only where it says more than the trend (2 rising, 4 steady, 7 falling say nothing new).
                det.push_back(std::string("Pressure has ") + trend + (a == 4 ? "" : fmt(" %.1f hPa", d)) + " in 3 hours"
                              + (a == 2 || a == 4 || a == 7 ? std::string() : std::string(" (") + shape[a] + ")"));
                break;
            }
            case 6: {
                if (g.substr(1) == "////") break;
                if (!allDigits(g, 1, 3)) { unreadable(g); break; }
                const int rrr = num(g, 1, 3);
                std::string amt = rrr == 990 ? "a trace" : rrr >= 991 ? fmt("%.1f mm", (rrr - 990) / 10.0) : rrr == 989 ? "989 mm or more" : fmtI("%ld mm", rrr);
                static const char* const per[10] = { "", " in 6 hours", " in 12 hours", " in 18 hours", " in 24 hours", " in 1 hour",
                                                     " in 2 hours", " in 3 hours", " in 9 hours", " in 15 hours" };
                det.push_back("Precipitation " + amt + (isDigit(g[4]) ? per[g[4] - '0'] : ""));
                break;
            }
            case 7: {
                if (g.substr(1) == "////") break;
                const bool autoCodes = iX == 7;
                if (iX == 2 || iX == 3 || iX == 5 || iX == 6) { unreadable(g); break; }   // ★ iX said there is no 7 group
                std::string now, past;
                if (allDigits(g, 1, 2)) {
                    const int w = num(g, 1, 2);
                    const char* t = autoCodes ? ww4680(w) : ww4677(w);
                    if (!t) { unreadable(g); break; }
                    now = t;
                } else if (g[1] != '/' || g[2] != '/') { unreadable(g); break; }
                for (int k = 3; k <= 4; ++k) {
                    if (!isDigit(g[k])) continue;
                    const char* t = autoCodes ? past4531(g[k] - '0') : past4561(g[k] - '0');
                    if (k == 4 && g[4] == g[3]) continue;
                    if (t) past += (past.empty() ? "" : ", ") + std::string(t);
                }
                // ★ ww 00-03 say how the CLOUD changed, not what the weather is — kept off the compact line.
                if (!now.empty()) { det.push_back("Weather now: " + now); if (num(g, 1, 2) >= 4) wx = now; }
                if (!past.empty()) det.push_back("Weather before: " + past);
                break;
            }
            case 8: {
                if (g[1] != '/') { const std::string c = octas(g[1]); if (!c.empty()) det.push_back("Low or middle cloud " + c); }
                std::string kinds;
                const char* (*tbl[3])(int) = { cloudLow, cloudMid, cloudHigh };
                for (int k = 0; k < 3; ++k) {
                    const char ch = g[2 + k];
                    if (!isDigit(ch) || ch == '0') continue;
                    kinds += (kinds.empty() ? "" : ", ") + std::string(tbl[k](ch - '0'));
                }
                if (!kinds.empty()) det.push_back("Clouds: " + kinds);
                break;
            }
            case 9:
                if (allDigits(g, 1, 4) && num(g, 1, 2) <= 23 && num(g, 3, 2) <= 59) {
                    if (h.known && line.size() >= 2) {
                        for (auto& p : line) if (p == fmtI("%02ld:00 UTC", h.hour)) p = fmtI("%02ld:", num(g, 1, 2)) + fmtI("%02ld UTC", num(g, 3, 2));
                    }
                    det.push_back(fmtI("Exact time %02ld:", num(g, 1, 2)) + fmtI("%02ld UTC", num(g, 3, 2)));
                } else unreadable(g);
                break;
            default: unreadable(g); break;   // ★ 0 is not a section-1 group
            }
        } else if (sec == S2) {
            switch (ind) {
            case 0: {
                if (!isDigit(g[1]) || g[1] > '7' || !allDigits(g, 2, 3)) { if (g.substr(2) != "///") unreadable(g); break; }
                const double c = num(g, 2, 3) / 10.0 * ((g[1] - '0') % 2 ? -1 : 1);   // ★ ss: even = positive, odd = negative
                if (c < -3.0 || c > 36.0) { unreadable(g); break; }
                det.push_back("Sea temperature " + fmt("%.1f \xc2\xb0""C", c));
                break;
            }
            case 1: case 2: {
                if (!allDigits(g, 1, 4)) { if (g.substr(1) != "////") unreadable(g); break; }
                det.push_back(std::string(ind == 1 ? "Waves (measured) " : "Wind waves ") + fmt("%.1f m", num(g, 3, 2) * 0.5) + fmtI(", every %ld s", num(g, 1, 2)));
                break;
            }
            case 7:
                if (g[1] == '0' && allDigits(g, 2, 3)) det.push_back("Waves (measured) " + fmt("%.1f m", num(g, 2, 3) / 10.0));
                else if (g.substr(2) != "///") unreadable(g);
                break;
            default: break;   // ★ swell, ice, wet bulb — kept raw, not translated
            }
        } else if (sec == S3) {
            double c;
            switch (ind) {
            case 1: if (tempOf(g, c)) det.push_back("Highest temperature " + fmt("%.1f \xc2\xb0""C", c)); else if (g.substr(1) != "////") unreadable(g); break;
            case 2: if (tempOf(g, c)) det.push_back("Lowest temperature " + fmt("%.1f \xc2\xb0""C", c)); else if (g.substr(1) != "////") unreadable(g); break;
            case 4:
                if (allDigits(g, 2, 3)) {
                    const int s = num(g, 2, 3);
                    if (s >= 1 && s <= 996) det.push_back(fmtI("Snow depth %ld cm", s));
                    else if (s == 997) det.push_back("Snow depth under 0.5 cm");
                }
                break;
            case 5:
                if (g[1] == '5' && allDigits(g, 2, 3) && num(g, 2, 3) <= 240)   // ★ 55SSS; 553SS/554SS start 3/4, past 240
                    det.push_back("Sunshine yesterday " + fmt("%.1f hours", num(g, 2, 3) / 10.0));
                break;   // ★ the other 5-groups (evaporation, radiation, 24-h pressure change) — kept raw
            case 6: {
                if (!allDigits(g, 1, 3)) { if (g.substr(1) != "////") unreadable(g); break; }
                const int rrr = num(g, 1, 3);
                std::string amt = rrr == 990 ? "a trace" : rrr >= 991 ? fmt("%.1f mm", (rrr - 990) / 10.0) : rrr == 989 ? "989 mm or more" : fmtI("%ld mm", rrr);
                static const char* const per[10] = { "", " in 6 hours", " in 12 hours", " in 18 hours", " in 24 hours", " in 1 hour",
                                                     " in 2 hours", " in 3 hours", " in 9 hours", " in 15 hours" };
                det.push_back("Precipitation " + amt + (isDigit(g[4]) ? per[g[4] - '0'] : ""));
                break;
            }
            case 7:
                if (allDigits(g, 1, 4)) { const int v = num(g, 1, 4); det.push_back(v == 9999 ? std::string("Precipitation in 24 hours: a trace") : "Precipitation in 24 hours " + fmt("%.1f mm", v / 10.0)); }
                break;
            case 9:
                if (allDigits(g, 1, 4) && g[1] == '1' && (g[2] == '0' || g[2] == '1')) {
                    const int ff = num(g, 3, 2);
                    det.push_back(std::string(g[2] == '0' ? "Gusts (last 10 min) " : "Highest gust ") + speedWords(ff));
                }
                break;   // ★ the other special phenomena — kept raw
            default: break;
            }
        }
    }
    // ★ What was READ: every detail after who/when ("Observed on day …" is the header's, not the report's).
    for (size_t k = base; k < det.size(); ++k) if (det[k].rfind("Observed on day", 0) != 0) r.values++;
    if (!temp.empty()) line.push_back(temp);
    if (!pressMsl.empty()) line.push_back(pressMsl);
    else if (!pressStn.empty()) line.push_back(pressStn + " at the station");
    if (!wx.empty()) line.push_back(wx);
    if (r.unreadable) line.push_back(r.unreadable == 1 ? std::string("1 group unreadable") : fmtI("%ld groups unreadable", r.unreadable));
    if (!bad.empty()) {
        std::string b = "Unreadable (RTTY damage): ";
        for (size_t k = 0; k < bad.size(); ++k) { if (k) b += ' '; b += bad[k]; }
        det.push_back(b);
    }
    for (size_t k = 0; k < line.size(); ++k) { if (k) r.line += " \xc2\xb7 "; r.line += line[k]; }
    return r;
}

/** Strict test for a report met with no AAXX: five figures, a valid iRiXhVV and Nddff, at least one more group. */
inline bool plainlySynop(const std::vector<std::string>& t) {
    if (t.size() < 4 || t[0].size() != 5 || !allDigits(t[0])) return false;
    const std::string& a = t[1]; const std::string& b = t[2];
    if (!isGroup(a) || !((a[0] >= '0' && a[0] <= '4') || a[0] == '/') || a[1] < '1' || a[1] > '7') return false;
    double km; std::string w;
    if (!(a[3] == '/' && a[4] == '/') && !visibilityKm(a.substr(3, 2), km, w)) return false;
    if (!isGroup(b) || !allDigits(b, 1, 4)) return false;
    const int dd = num(b, 1, 2);
    if (dd > 36 && dd != 99) return false;
    int good = 0;
    for (size_t k = 3; k < t.size(); ++k) if (isGroup(t[k]) && isDigit(t[k][0])) ++good;
    return good >= 1;
}

// ── The stream reader ─────────────────────────────────────────────────────────────────────────────────────────────
/** Decoded RTTY text in, finished reports out. Not thread-safe: the decoder host calls it from its decode thread.
 *  ★ A report is RELEASED when its line ends (the next newline after its '='), or at the next printing character if
 *    none comes — so the frame lands under the raw text, not in the middle of it. `push` says where in the text each
 *    released report belongs (the byte offset AFTER which it goes). */
class Reader {
public:
    struct Out { size_t at; Report report; };
    static constexpr size_t kMaxTokens = 64;    // ★ a real report is ~20 groups; past this it is not a report
    static constexpr size_t kMaxTok = 24;
    static constexpr size_t kMaxHeld = 8;

    std::vector<Out> push(const std::string& text) {
        std::vector<Out> out;
        for (size_t k = 0; k < text.size(); ++k) {
            const char c = text[k];
            if (!held_.empty()) {
                if (c == '\n') { release(out, k + 1); continue; }
                if (c != ' ' && c != '\r') release(out, k);
            }
            if (c == ' ' || c == '\r' || c == '\n' || c == '\t') { endToken(); if (c == '\n') lineStart_ = true; continue; }
            if (c == '=' || c == ';') { endToken(); endReport(true); afterEnd_ = true; continue; }
            if (tok_.size() < kMaxTok) tok_ += c; else tokLong_ = true;
        }
        return out;
    }
    /** End of the stream (the decoder stopped): a report waiting for its '=' is read if it can be, and everything is
     *  released at the end of the text. */
    std::vector<Out> flush(size_t at = 0) {
        std::vector<Out> out;
        endToken(); endReport(false);
        release(out, at);
        return out;
    }
    /** Release everything held now (the host's idle timer: a report whose '=' ended the transmission). */
    std::vector<Out> releaseHeld(size_t at = 0) { std::vector<Out> out; release(out, at); return out; }
    void reset() { tok_.clear(); tokLong_ = false; cur_.clear(); held_.clear(); mode_ = Mode::None; hdr_ = Header{}; expectHeader_ = false; lineStart_ = true; afterEnd_ = false; inReport_ = false; }
    /** Reports finished and not yet released (a test, the admin page). */
    size_t held() const { return held_.size(); }

private:
    enum class Mode { None, Synop, Ship };
    void release(std::vector<Out>& out, size_t at) { for (auto& r : held_) out.push_back({ at, std::move(r) }); held_.clear(); }
    void hold(Report r) { if (held_.size() < kMaxHeld) held_.push_back(std::move(r)); }
    void endToken() {
        if (tok_.empty()) return;
        std::string t; t.swap(tok_);
        if (tokLong_) { t += "\xe2\x80\xa6"; tokLong_ = false; }   // ★ a run-on token: kept short, plainly damaged
        const bool atLineStart = lineStart_, atEnd = afterEnd_;
        lineStart_ = false; afterEnd_ = false;
        // ★ Bulletin boundaries, wherever they fall.
        if (t == "AAXX" || t == "BBXX") { endReport(false); mode_ = t == "AAXX" ? Mode::Synop : Mode::Ship; hdr_ = Header{}; expectHeader_ = mode_ == Mode::Synop; return; }
        if (t == "NNNN" || t == "ZCZC") { endReport(false); mode_ = Mode::None; hdr_ = Header{}; expectHeader_ = false; return; }
        if (expectHeader_) { expectHeader_ = false; if (!readTimeGroup(t, hdr_)) hdr_ = Header{}; return; }
        if (mode_ == Mode::None) {
            // ★ Headless: a candidate only from the start of a line or straight after a '='; judged when it ends.
            if (!inReport_) {
                if (t == "SYNOP") { afterEnd_ = true; return; }   // ★ a plain-language lead-in some services print
                if ((atLineStart || atEnd) && t.size() == 5 && allDigits(t)) { inReport_ = true; cur_.clear(); cur_.push_back(t); }
                return;
            }
            if (isWord(t) && t != "NIL") { cur_.clear(); inReport_ = false; return; }
            cur_.push_back(t);
            if (cur_.size() > kMaxTokens) { cur_.clear(); inReport_ = false; }
            return;
        }
        // In a bulletin.
        if (!inReport_) {
            if (isText(t) && !(mode_ == Mode::Ship)) { mode_ = Mode::None; return; }   // ★ text again: the bulletin is over
            if (mode_ == Mode::Ship && isText(t) && t.size() > 9) { mode_ = Mode::None; return; }
            inReport_ = true; cur_.clear(); cur_.push_back(t); return;
        }
        if (isText(t)) {
            // ★ Words inside a report: it ended without its '=', and the bulletin with it.
            endReport(false); mode_ = Mode::None; return;
        }
        cur_.push_back(t);
        if (cur_.size() > kMaxTokens) { cur_.clear(); inReport_ = false; mode_ = Mode::None; }
    }
    /** `terminated`: ended by '=' — a headless candidate is only believed then. */
    void endReport(bool terminated) {
        if (!inReport_) return;
        inReport_ = false;
        std::vector<std::string> t; t.swap(cur_);
        if (t.empty()) return;
        if (mode_ == Mode::None) {
            if (terminated && plainlySynop(t)) { Report r = translate(t, Header{}, false); if (r.values > 0) hold(std::move(r)); }
            return;
        }
        const bool ship = mode_ == Mode::Ship;
        // ★ Too little to say anything: a lone fragment between two '='s.
        if (t.size() < (ship ? 3u : 2u)) return;
        Report r = translate(t, hdr_, ship);
        if (r.values > 0) hold(std::move(r));   // ★ nothing readable in it at all: no line to show
    }

    std::string tok_;
    bool tokLong_ = false;
    std::vector<std::string> cur_;
    std::vector<Report> held_;
    Mode mode_ = Mode::None;
    Header hdr_;
    bool expectHeader_ = false, lineStart_ = true, afterEnd_ = false, inReport_ = false;
};

}  // namespace synop
}  // namespace vibe
