// vibe_navtex_met.h — V12 2026-11-01 THE NAVTEX MAP, stage 3: the MET LABELS. A compact label per sea area from the
// latest forecast heard for it — "SW 6–8 · GALES · ROUGH" — gale or worse in a warning colour; the card shows the area's
// forecast, its history and the station.
//
// ★★★ THE SEA-AREA TABLE (below) IS OUR OWN, FROM CLEAN SOURCES ONLY.
//     · The area NAMES are the published names the forecasts themselves use — the UK shipping forecast areas (Met Office
//       / Maritime and Coastguard Agency) and the UK inshore waters stretches (headland to headland); Skagerrak and the
//       Belgian coast as Norwegian, Dutch and Belgian NAVTEX forecasts name them. A name is a fact, not a copyrighted work.
//     · The LABEL POINTS are DERIVED BY US (2026-11-01): a point inside each area, placed by hand from the area's
//       description (the sea between the named coasts or headlands) and checked against our own basemap (the bundled
//       Natural Earth coastline, public domain). They are label anchors, not boundaries — up to ~100 km from an area's
//       true centre, never on land.
//     · NO BOUNDARIES are drawn: we found no boundary set under an open licence or in the public domain that we could cite
//       here (the Met Office's area chart is Crown copyright, and its coordinates are not published as open data).
//       Boundaries wait for one.
//     · Not included (no clean source for a label point we were sure of): the Norwegian Sea areas beyond Viking and the
//       Utsires, the French Atlantic areas (Pazenn, Yeu, Rochebonne), the Spanish, Italian and Croatian areas.
// ★★ PARSED CONSERVATIVELY: the area's heading must be a line of known area names and nothing else (or "AREA[S]:" with
//    the forecast after the colon); its block ends at the next heading, an OUTLOOK / SYNOPSIS / FORECAST VALID line, a
//    day name, or NNNN — so the FIRST period of a forecast is read, never the outlook. From the block: the wind (the
//    first direction word and the force numbers of its first sentence), the gale words (GALE / SEVERE GALE / STORM /
//    VIOLENT STORM / HURRICANE FORCE, or a force of 8 and over — "NO GALES" is not a gale), the sea state (the
//    sentence straight after the wind, SMOOTH … PHENOMENAL, or a wave height in metres), the weather and the visibility.
//    A gale word in another language (COUP DE VENT, TEMPORAL, BURRASCA, STURM, OLUJA…) sets only the GALE flag.
// ★ Parsed ONCE, when the message is indexed (vibe_navtex_warn.h sync): gallery/navtex-met.idx, one line per area per
//   message (never the whole text). The query keeps the live lines (the gallery's own view), the latest per area.
#pragma once
#include "vibe_navtex_stations.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vibe {
namespace navmet {

static constexpr int64_t kStaleSec = 24 * 3600;   // ★ a forecast older than this is drawn faded (ALL) or not at all
static constexpr size_t kHistory = 6;
static constexpr size_t kBlockMax = 420;

/** kind: 's' shipping forecast area, 'i' UK inshore waters stretch, 'n' a neighbouring area. `match`: the uppercase
 *  forms a heading uses ('|'-separated); an inshore stretch matches its "<HEADLAND> TO <HEADLAND>" start. */
struct Area { const char* id; const char* name; double lat, lon; char kind; const char* match; };
inline const std::vector<Area>& areas() {
    static const std::vector<Area> a = {
        // ── UK shipping forecast areas (label points ours) ──
        {"viking", "Viking", 60.0, 2.2, 's', "VIKING"},
        {"nutsire", "North Utsire", 60.1, 4.1, 's', "NORTH UTSIRE|N UTSIRE|NORTH-UTSIRE"},
        {"sutsire", "South Utsire", 58.3, 4.4, 's', "SOUTH UTSIRE|S UTSIRE|SOUTH-UTSIRE"},
        {"forties", "Forties", 57.6, 1.0, 's', "FORTIES"},
        {"cromarty", "Cromarty", 57.9, -1.8, 's', "CROMARTY"},
        {"forth", "Forth", 56.5, -1.2, 's', "FORTH"},
        {"tyne", "Tyne", 55.2, -0.5, 's', "TYNE"},
        {"dogger", "Dogger", 54.8, 2.2, 's', "DOGGER"},
        {"fisher", "Fisher", 56.6, 5.6, 's', "FISHER"},
        {"gbight", "German Bight", 54.5, 6.6, 's', "GERMAN BIGHT|GERMANBIGHT"},
        {"humber", "Humber", 53.7, 1.7, 's', "HUMBER"},
        {"thames", "Thames", 52.2, 2.3, 's', "THAMES"},
        {"dover", "Dover", 50.95, 1.35, 's', "DOVER"},
        {"wight", "Wight", 50.3, -1.0, 's', "WIGHT"},
        {"portland", "Portland", 50.0, -2.8, 's', "PORTLAND"},
        {"plymouth", "Plymouth", 49.6, -4.6, 's', "PLYMOUTH"},
        {"biscay", "Biscay", 46.2, -5.0, 's', "BISCAY"},
        {"fitzroy", "FitzRoy", 45.5, -11.5, 's', "FITZROY"},
        {"trafalgar", "Trafalgar", 37.5, -10.5, 's', "TRAFALGAR"},
        {"sole", "Sole", 49.0, -10.0, 's', "SOLE"},
        {"lundy", "Lundy", 51.4, -5.4, 's', "LUNDY"},
        {"fastnet", "Fastnet", 50.9, -8.3, 's', "FASTNET"},
        {"irishsea", "Irish Sea", 53.5, -5.4, 's', "IRISH SEA"},
        {"shannon", "Shannon", 53.0, -12.0, 's', "SHANNON"},
        {"rockall", "Rockall", 57.5, -13.5, 's', "ROCKALL"},
        {"malin", "Malin", 55.8, -8.4, 's', "MALIN"},
        {"hebrides", "Hebrides", 58.2, -9.6, 's', "HEBRIDES"},
        {"bailey", "Bailey", 60.0, -15.0, 's', "BAILEY"},
        {"fairisle", "Fair Isle", 59.7, -2.6, 's', "FAIR ISLE"},
        {"faeroes", "Faeroes", 62.0, -5.0, 's', "FAEROES|FAROES|FAROE"},
        {"seiceland", "Southeast Iceland", 63.6, -12.5, 's', "SOUTHEAST ICELAND|SOUTH-EAST ICELAND|SE ICELAND"},
        // ── UK inshore waters, headland to headland (label points ours, offshore of the stretch's middle) ──
        {"i-capewrath-rattray", "Cape Wrath to Rattray Head", 58.75, -2.6, 'i', "CAPE WRATH TO RATTRAY HEAD"},
        {"i-rattray-berwick", "Rattray Head to Berwick", 56.9, -1.85, 'i', "RATTRAY HEAD TO BERWICK"},
        {"i-berwick-whitby", "Berwick to Whitby", 55.1, -1.15, 'i', "BERWICK UPON TWEED TO WHITBY|BERWICK-UPON-TWEED TO WHITBY|BERWICK TO WHITBY"},
        {"i-whitby-gibraltar", "Whitby to Gibraltar Point", 53.85, 0.35, 'i', "WHITBY TO GIBRALTAR POINT"},
        {"i-gibraltar-nforeland", "Gibraltar Point to North Foreland", 52.6, 1.95, 'i', "GIBRALTAR POINT TO NORTH FORELAND|GIBRALTAR POINT TO N FORELAND"},
        {"i-nforeland-selsey", "North Foreland to Selsey Bill", 50.65, 0.45, 'i', "NORTH FORELAND TO SELSEY BILL|N FORELAND TO SELSEY BILL"},
        {"i-selsey-lyme", "Selsey Bill to Lyme Regis", 50.45, -1.75, 'i', "SELSEY BILL TO LYME REGIS"},
        {"i-lyme-landsend", "Lyme Regis to Land's End", 50.05, -4.2, 'i', "LYME REGIS TO LANDS END"},
        {"i-landsend-stdavids", "Land's End to St David's Head", 51.3, -4.7, 'i', "LANDS END TO ST DAVIDS HEAD"},
        {"i-stdavids-greatorme", "St David's Head to Great Orme Head", 52.6, -4.6, 'i', "ST DAVIDS HEAD TO GREAT ORME HEAD"},
        {"i-greatorme-galloway", "Great Orme Head to Mull of Galloway", 54.0, -3.5, 'i', "GREAT ORME HEAD TO MULL OF GALLOWAY|GREAT ORME HEAD TO THE MULL OF GALLOWAY"},
        {"i-isleofman", "Isle of Man", 54.35, -4.95, 'i', "ISLE OF MAN"},
        {"i-galloway-kintyre", "Mull of Galloway to Mull of Kintyre", 55.3, -5.3, 'i', "MULL OF GALLOWAY TO MULL OF KINTYRE"},
        {"i-foyle-carlingford", "Lough Foyle to Carlingford Lough", 54.75, -5.55, 'i', "LOUGH FOYLE TO CARLINGFORD LOUGH"},
        {"i-kintyre-ardnamurchan", "Mull of Kintyre to Ardnamurchan Point", 56.2, -6.7, 'i', "MULL OF KINTYRE TO ARDNAMURCHAN POINT"},
        {"i-minch", "The Minch", 58.0, -5.9, 'i', "THE MINCH"},
        {"i-ardnamurchan-capewrath", "Ardnamurchan Point to Cape Wrath", 56.95, -6.85, 'i', "ARDNAMURCHAN POINT TO CAPE WRATH"},
        {"i-shetland", "Shetland Isles", 60.35, -0.9, 'i', "SHETLAND ISLES|SHETLAND"},
        {"i-channelislands", "Channel Islands", 49.35, -2.35, 'i', "CHANNEL ISLANDS"},
        // ── neighbours (the names as the forecasts use them; label points ours) ──
        {"n-innerskagerrak", "Inner Skagerrak", 58.7, 10.2, 'n', "INNER SKAGERRAK|INNER-SKAGERRAK"},
        {"n-outerskagerrak", "Outer Skagerrak", 57.9, 8.2, 'n', "OUTER SKAGERRAK|OUTER-SKAGERRAK"},
        {"n-belgiancoast", "Belgian coast", 51.35, 2.75, 'n', "BELGIAN COAST"},
    };
    return a;
}

/** One area's forecast, from one message. Levels: gale 0 / 8–12 (8 = native GALE flag too); sea -1 unknown, 0 calm …
 *  9 phenomenal; vis -1 unknown, 0 fog, 1 very poor, 2 poor, 3 moderate, 4 good. */
struct Fc {
    std::string area, dir, wx, block;
    int fmin = -1, fmax = -1, gale = 0, sea = -1, vis = -1;
    bool galeNative = false;
};

inline std::string norm(std::string s) {
    s = navtex::upperAscii(s);
    std::string o;
    for (char c : s) {
        if (c == '\'' || c == '`') continue;
        if (c == '\t' || c == '\r') c = ' ';
        if (c == ' ' && (o.empty() || o.back() == ' ')) continue;
        o += c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}
/** A heading: the line (its "- (SWW)" tag and end punctuation gone) made only of area names, separated by spaces,
 *  commas, full stops, "AND" or "&". The ids, or empty. An inshore stretch: its "X TO Y" start (an "INCLUDING …" tail
 *  allowed). */
inline std::vector<std::string> headingAreas(std::string l) {
    std::vector<std::string> out;
    l = norm(l);
    static const std::regex tag(R"(\s*-?\s*\((SWW|GW|SW)\)\s*$)");
    l = std::regex_replace(l, tag, "");
    while (!l.empty() && (l.back() == ':' || l.back() == '.' || l.back() == ',' || l.back() == ' ')) l.pop_back();
    if (l.size() < 4 || l.size() > 120) return out;
    // ★ inshore stretches: the start of the line
    for (const auto& a : areas()) {
        if (a.kind != 'i') continue;
        std::string m = a.match;
        size_t p = 0;
        while (p <= m.size()) {
            const size_t e = m.find('|', p);
            const std::string one = m.substr(p, e == std::string::npos ? std::string::npos : e - p);
            if (l == one || (l.compare(0, one.size(), one) == 0 && (l.compare(one.size(), 11, " INCLUDING ") == 0 || l.compare(one.size(), 12, ", INCLUDING ") == 0))) { out.push_back(a.id); return out; }
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
    // ★ a list of shipping areas: consumed whole, longest name first
    std::string rest = l;
    for (;;) {
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == ',' || rest[0] == '.' || rest[0] == '&' || rest[0] == '/')) rest.erase(0, 1);
        if (rest.empty()) break;
        if (rest.compare(0, 4, "AND ") == 0) { rest.erase(0, 4); continue; }
        size_t best = 0; const Area* hit = nullptr;
        for (const auto& a : areas()) {
            if (a.kind == 'i') continue;
            std::string m = a.match; size_t p = 0;
            while (p <= m.size()) {
                const size_t e = m.find('|', p);
                const std::string one = m.substr(p, e == std::string::npos ? std::string::npos : e - p);
                if (one.size() > best && rest.compare(0, one.size(), one) == 0 && (rest.size() == one.size() || std::strchr(" ,.&/:", rest[one.size()]))) { best = one.size(); hit = &a; }
                if (e == std::string::npos) break;
                p = e + 1;
            }
        }
        if (!hit) { out.clear(); return out; }
        if (std::find(out.begin(), out.end(), hit->id) == out.end()) out.push_back(hit->id);
        rest.erase(0, best);
    }
    return out;
}
/** A line that ends an area's block. */
inline bool terminator(const std::string& U) {
    static const std::regex re(R"(^\s*(OUTLOOK|GENERAL SITUATION|THE GENERAL SITUATION|GENERAL SYNOPSIS|SYNOPSIS|SYNOPTIC|FORECAST VALID|EXTENDED OUTLOOK|GALE WARNINGS?\b|STORM WARNING|ALL REGIONS|NNNN|FURTHER OUTLOOK|NEXT \d+ HOURS|IMMINENT:|SOON:|LATER:|MRSC\b|MRCC\b|(MONDAY|TUESDAY|WEDNESDAY|THURSDAY|FRIDAY|SATURDAY|SUNDAY)\s*:))");
    return std::regex_search(U, re);
}
/** The text's sentences: a full stop followed by a space or the end ("0.5 METER" and "2,5" kept), and line ends that close
 *  one in the Met Office's line-per-element layout. */
inline std::vector<std::string> sentences(const std::string& block) {
    std::vector<std::string> out; std::string cur;
    for (size_t i = 0; i < block.size(); ++i) {
        const char c = block[i];
        if (c == '.' && (i + 1 == block.size() || block[i + 1] == ' ' || block[i + 1] == '\n')) { out.push_back(cur); cur.clear(); continue; }
        if (c == '\n') { if (!cur.empty() && cur.back() != ' ') cur += ' '; continue; }
        cur += c;
    }
    out.push_back(cur);
    std::vector<std::string> o2;
    for (auto& s : out) { std::string t = norm(s); if (!t.empty()) o2.push_back(t); }
    return o2;
}
inline const char* dirAbbr(const std::string& w) {
    static const std::vector<std::pair<const char*, const char*>> t = {
        {"NORTHEASTERLY", "NE"}, {"NORTHWESTERLY", "NW"}, {"SOUTHEASTERLY", "SE"}, {"SOUTHWESTERLY", "SW"},
        {"NORTHEAST", "NE"}, {"NORTHWEST", "NW"}, {"SOUTHEAST", "SE"}, {"SOUTHWEST", "SW"},
        {"NORTH-EAST", "NE"}, {"NORTH-WEST", "NW"}, {"SOUTH-EAST", "SE"}, {"SOUTH-WEST", "SW"},
        {"NORTHERLY", "N"}, {"SOUTHERLY", "S"}, {"EASTERLY", "E"}, {"WESTERLY", "W"},
        {"NORTH", "N"}, {"SOUTH", "S"}, {"EAST", "E"}, {"WEST", "W"}, {"VARIABLE", "VRB"}, {"VRB", "VRB"}, {"CYCLONIC", "CYC"},
        {"NNE", "NNE"}, {"ENE", "ENE"}, {"ESE", "ESE"}, {"SSE", "SSE"}, {"SSW", "SSW"}, {"WSW", "WSW"}, {"WNW", "WNW"}, {"NNW", "NNW"},
        {"NE", "NE"}, {"NW", "NW"}, {"SE", "SE"}, {"SW", "SW"}, {"N", "N"}, {"S", "S"}, {"E", "E"}, {"W", "W"}};
    for (auto& x : t) if (w == x.first) return x.second;
    return nullptr;
}
inline int seaLevel(const std::string& s) {
    static const std::vector<std::pair<std::regex, int>> t = {
        {std::regex(R"(\bPHENOMENAL\b)"), 9}, {std::regex(R"(\bVERY HIGH\b)"), 8}, {std::regex(R"(\bVERY ROUGH\b)"), 6},
        {std::regex(R"(\bHIGH\b)"), 7}, {std::regex(R"(\bROUGH\b)"), 5}, {std::regex(R"(\b(MODERATE|MOD)\b)"), 4},
        {std::regex(R"(\b(SLIGHT|SLT|SLGT)\b)"), 3}, {std::regex(R"(\bSMOOTH\b)"), 2}, {std::regex(R"(\bCALM\b)"), 1}};
    // ★ "RATHER ROUGH" is between moderate and rough: it counts as MODERATE unless ROUGH stands alone too
    static const std::regex rather(R"(\bRATHER ROUGH\b)");
    std::string u = std::regex_replace(s, rather, "MODERATE");
    int best = -1;
    for (auto& x : t) if (std::regex_search(u, x.first)) best = std::max(best, x.second);
    return best;
}
inline bool seaStart(const std::string& s) {
    static const std::regex re(R"(^(SEA( STATE)?:?\s*)?(SMOOTH|SLIGHT|SLT|SLGT|MODERATE|MOD|ROUGH|RATHER ROUGH|VERY ROUGH|HIGH|VERY HIGH|PHENOMENAL|CALM)\b)");
    return std::regex_search(s, re);
}
/** WMO sea-state code from a significant wave height (m). */
inline int seaFromMetres(double m) {
    return m <= 0.1 ? 1 : m <= 0.5 ? 2 : m <= 1.25 ? 3 : m <= 2.5 ? 4 : m <= 4 ? 5 : m <= 6 ? 6 : m <= 9 ? 7 : m <= 14 ? 8 : 9;
}
inline int visLevel(const std::string& s) {
    static const std::regex good(R"(\b(GOOD|VERY GOOD)\b)"), mod(R"(\b(MODERATE|MOD)\b)"), poor(R"(\bPOOR\b)"), vpoor(R"(\bVERY POOR\b)"), fog(R"(\bFOG\b)");
    int v = -1;
    if (std::regex_search(s, good)) v = 4;
    if (std::regex_search(s, mod)) v = v < 0 ? 3 : std::min(v, 3);
    if (std::regex_search(s, poor)) v = v < 0 ? 2 : std::min(v, 2);
    if (std::regex_search(s, vpoor)) v = v < 0 ? 1 : std::min(v, 1);
    if (std::regex_search(s, fog)) v = v < 0 ? 0 : std::min(v, 0);
    return v;
}
inline bool visStart(const std::string& s) { static const std::regex re(R"(^(VISIBILITY:?\s*|VIS:?\s*)?(GOOD|VERY GOOD|MODERATE|MOD|POOR|VERY POOR|FOG)\b)"); return std::regex_search(s, re); }

/** One area block → its forecast fields. */
inline Fc read(const std::string& id, const std::string& block) {
    Fc f; f.area = id;
    std::string b = block; if (b.size() > kBlockMax) b.resize(kBlockMax);
    f.block = norm(b);
    const auto ss = sentences(block);
    // ★ "NO GALES", "GALES NOT EXPECTED", "GALE WARNINGS: NONE" are not gales
    auto clean = [](const std::string& s) {
        static const std::regex neg(R"(\bNO (SEVERE )?GALES?\b|\b(SEVERE )?GALES? (ARE )?NOT (EXPECTED|EXP)\b|\bGALE WARNINGS?:? (NONE|NIL)\b|\bNO GALE WARNINGS?\b|\bNO STORMS?\b|\b(SEVERE )?GALES? (NOW |HAS |HAVE )?(CEASED|ENDED)\b|\bNO LONGER\b[^.]*)");
        return std::regex_replace(s, neg, " ");
    };
    int wi = -1;
    static const std::regex force(R"((^|[\s,(])(\d{1,2})(?=($|[\s,.)-])))"), word(R"([A-Z-]+)"), galeAny(R"(\b(GALE|STORM|HURRICANE)\b)"),
        unit(R"(^\s*(HPA|MB|M\b|METER|METRE|NM|MILES|KM|UTC|Z\b|KT|KNOTS|HOURS|HR|DEG|N\b|S\b|E\b|W\b))"),
        g12(R"(\bHURRICANE\b)"), g11(R"(\bVIOLENT STORM\b)"), g10(R"(\bSTORM\b)"), g9(R"(\bSEVERE GALES?\b)"), g8(R"(\bGALES?\b)"),
        native(R"(\bCOUP DE VENT\b|\bTEMPORAL\b|\bBURRASCA\b|\bSTURM\w*\b|\bOLUJ\w*\b|\bSTORMVARSEL\b)"),
        waveS(R"(\bWAVE ?HEIGHTS?\b|^SEA:|\bSEA STATE\b)"), metres(R"((\d+(?:[.,]\d+)?)\s*(METERS?|METRES?|M)\b)"), decimal(R"((\d+[.,]\d+))");
    for (size_t i = 0; i < ss.size() && wi < 0; ++i) {
        const std::string s = clean(ss[i]);
        bool hasDir = false;
        for (auto it = std::sregex_iterator(s.begin(), s.end(), word); it != std::sregex_iterator(); ++it) if (dirAbbr(it->str())) { hasDir = true; break; }
        bool hasForce = false;
        for (auto it = std::sregex_iterator(s.begin(), s.end(), force); it != std::sregex_iterator(); ++it) { const int n = std::atoi((*it)[2].str().c_str()); if (n >= 0 && n <= 12) hasForce = true; }
        if ((hasDir && hasForce) || std::regex_search(s, galeAny)) wi = (int)i;
    }
    if (wi >= 0) {
        const std::string s = clean(ss[wi]);
        for (auto it = std::sregex_iterator(s.begin(), s.end(), word); it != std::sregex_iterator(); ++it) if (const char* d = dirAbbr(it->str())) { f.dir = d; break; }
        for (auto it = std::sregex_iterator(s.begin(), s.end(), force); it != std::sregex_iterator(); ++it) {
            // ★ not a figure of something else: hPa, metres, miles, UTC, a time
            const size_t after = (size_t)(it->position(0) + it->length(0));
            const std::string tail = s.substr(after, 8);
            if (std::regex_search(tail, unit)) continue;
            const int n = std::atoi((*it)[2].str().c_str());
            if (n < 0 || n > 12) continue;
            if (f.fmin < 0) f.fmin = n;
            f.fmax = std::max(f.fmax, n);
        }
        if (f.fmin < 0) f.fmax = -1;
    }
    // ★ gale words anywhere in the block (negations removed); a force of 8+ is a gale too
    {
        const std::string all = clean(f.block);
        if (std::regex_search(all, g12)) f.gale = 12;
        else if (std::regex_search(all, g11)) f.gale = 11;
        else if (std::regex_search(all, g10)) f.gale = 10;
        else if (std::regex_search(all, g9)) f.gale = 9;
        else if (std::regex_search(all, g8)) f.gale = 8;
        if (f.fmax >= 8) f.gale = std::max(f.gale, f.fmax);
        // ★ another language's gale word: the GALE flag only (its force is not read)
        if (!f.gale && std::regex_search(all, native)) { f.gale = 8; f.galeNative = true; }
    }
    // ★ sea: the sentence straight after the wind when it starts with a sea word; a wave height anywhere
    int si = -1;
    if (wi >= 0 && wi + 1 < (int)ss.size() && seaStart(ss[wi + 1])) { si = wi + 1; f.sea = seaLevel(ss[si]); }
    for (size_t i = 0; i < ss.size(); ++i) {
        if (!std::regex_search(ss[i], waveS)) continue;
        double mx = -1;
        for (auto it = std::sregex_iterator(ss[i].begin(), ss[i].end(), metres); it != std::sregex_iterator(); ++it) { std::string v = (*it)[1]; std::replace(v.begin(), v.end(), ',', '.'); mx = std::max(mx, std::atof(v.c_str())); }
        // "1.5 - 2.0 METER": the first figure of a range has no unit — read every figure of such a sentence
        for (auto it = std::sregex_iterator(ss[i].begin(), ss[i].end(), decimal); it != std::sregex_iterator(); ++it) { std::string v = (*it)[1]; std::replace(v.begin(), v.end(), ',', '.'); const double d = std::atof(v.c_str()); if (d < 20) mx = std::max(mx, d); }
        if (mx >= 0) f.sea = std::max(f.sea, seaFromMetres(mx));
        if (si < 0) si = (int)i;
    }
    // ★ visibility: the last sentence (after the sea) that starts with a visibility word
    int vi = -1;
    for (int i = (int)ss.size() - 1; i > std::max(wi, si); --i) if (visStart(ss[i])) { vi = i; break; }
    if (vi >= 0) f.vis = visLevel(ss[vi]);
    // ★ weather: its words, from the sentences that are not the wind, the sea or the visibility
    static const std::vector<std::pair<std::regex, const char*>> wx = {
        {std::regex(R"(\bTHUNDER\w*\b)"), "thundery"}, {std::regex(R"(\bSNOW\b)"), "snow"}, {std::regex(R"(\bSLEET\b)"), "sleet"}, {std::regex(R"(\bHAIL\b)"), "hail"},
        {std::regex(R"(\bWINTRY\b)"), "wintry"}, {std::regex(R"(\b(SHOWERS?|SHWRS?)\b)"), "showers"}, {std::regex(R"(\bRAIN\b)"), "rain"},
        {std::regex(R"(\bDRIZZLE\b)"), "drizzle"}, {std::regex(R"(\bFOG\b)"), "fog"}, {std::regex(R"(\bMIST\b)"), "mist"},
        {std::regex(R"(\bFAIR\b)"), "fair"}, {std::regex(R"(\bDRY\b)"), "dry"}};
    std::string w; int nw = 0;
    for (size_t i = 0; i < ss.size() && nw < 3; ++i) {
        if ((int)i == wi || (int)i == si || (int)i == vi) continue;
        for (auto& x : wx) if (nw < 3 && w.find(x.second) == std::string::npos && std::regex_search(ss[i], x.first)) { w += (w.empty() ? "" : ", "); w += x.second; ++nw; }
    }
    f.wx = w;
    return f;
}

/** ★ A forecast's text → its areas' forecasts (the first block for each area). */
inline std::vector<Fc> parse(const std::string& body) {
    std::vector<Fc> out;
    std::vector<std::string> lines;
    { size_t p = 0; while (p <= body.size()) { const size_t e = body.find('\n', p); lines.push_back(body.substr(p, e == std::string::npos ? std::string::npos : e - p)); if (e == std::string::npos) break; p = e + 1; } }
    std::vector<std::string> cur; std::string block;
    std::unordered_set<std::string> done;
    auto flush = [&]() {
        if (!cur.empty()) {
            for (auto& id : cur) {
                if (done.count(id)) continue;
                Fc f = read(id, block);
                if (f.fmin < 0 && !f.gale && f.sea < 0) continue;   // ★ nothing a label could say ("NO WARNING.")
                done.insert(id); out.push_back(f);
            }
        }
        cur.clear(); block.clear();
    };
    for (const auto& raw : lines) {
        const std::string U = norm(raw);
        if (U.empty()) { if (!block.empty()) block += "\n"; continue; }
        if (U == "24 HR FCST:" || U == "24 HR FCST" || U == "24-HR FORECASTS" || U == "24 HOURS FCST:") continue;
        auto ids = headingAreas(U);
        std::string after;
        if (ids.empty()) {
            // "PAZENN: S OR SE 4 OR 5" / "THAMES: 2 TO 3 FR ENE" — the areas before the colon, the forecast after it
            const size_t c = U.find(':');
            if (c != std::string::npos && c > 2 && c < 60) { ids = headingAreas(U.substr(0, c)); if (!ids.empty()) after = U.substr(c + 1); }
            // "WEATHERFORECAST FOR GERMAN BIGHT"
            if (ids.empty()) { std::smatch m; static const std::regex fcFor(R"(^(WEATHER ?)?FORECAST FOR (.+)$)"); if (std::regex_search(U, m, fcFor)) ids = headingAreas(m[2]); }
        }
        if (!ids.empty()) { flush(); cur = ids; block = after; continue; }
        if (terminator(U)) {
            static const std::regex day(R"(^(MONDAY|TUESDAY|WEDNESDAY|THURSDAY|FRIDAY|SATURDAY|SUNDAY)\s*:)");
            if (std::regex_search(U, day) && !cur.empty() && block.find_first_not_of(" \n") == std::string::npos) continue;   // ★ the first day: its forecast follows
            flush(); continue;
        }
        // ★ an area this table does not hold ("  EAST-TAMPEN", "FINISTERRE:") ends the block before it — never read into it
        {
            static const std::regex other(R"(^[A-Z][A-Z -]{2,30}:?$)");
            if (!cur.empty() && std::regex_search(U, other) && (raw.compare(0, 2, "  ") == 0 || U.back() == ':')) { flush(); continue; }
        }
        if (!cur.empty()) { if (!block.empty()) block += "\n"; block += raw; }
    }
    flush();
    return out;
}

// ── the index: one line per area per message ───────────────────────────────────────────────────────────────────────
inline std::string clip(std::string s, size_t n) { for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; if (s.size() > n) s.resize(n); return s; }
/** m1 \t file \t t \t area \t dir \t fmin \t fmax \t gale \t native \t sea \t vis \t wx \t block */
inline std::string format(const std::string& file, int64_t t, const Fc& f) {
    char b[128];
    std::snprintf(b, sizeof b, "\t%lld\t", (long long)t);
    return "m1\t" + file + b + f.area + "\t" + f.dir + "\t" + std::to_string(f.fmin) + "\t" + std::to_string(f.fmax) + "\t" + std::to_string(f.gale)
         + "\t" + (f.galeNative ? "1" : "0") + "\t" + std::to_string(f.sea) + "\t" + std::to_string(f.vis) + "\t" + clip(f.wx, 60) + "\t" + clip(f.block, kBlockMax) + "\n";
}
struct Line { std::string file, area; int64_t t = 0; Fc f; };
inline bool parseLine(const std::string& l, Line& o) {
    std::vector<std::string> v; size_t p = 0;
    for (;;) { const size_t q = l.find('\t', p); if (q == std::string::npos) { v.push_back(l.substr(p)); break; } v.push_back(l.substr(p, q - p)); p = q + 1; }
    if (v.size() != 13 || v[0] != "m1") return false;
    o.file = v[1]; o.t = std::atoll(v[2].c_str()); o.area = v[3];
    o.f.area = v[3]; o.f.dir = v[4]; o.f.fmin = std::atoi(v[5].c_str()); o.f.fmax = std::atoi(v[6].c_str()); o.f.gale = std::atoi(v[7].c_str());
    o.f.galeNative = v[8] == "1"; o.f.sea = std::atoi(v[9].c_str()); o.f.vis = std::atoi(v[10].c_str()); o.f.wx = v[11]; o.f.block = v[12];
    return !o.file.empty();
}
/** ★ Drop the lines of messages the gallery no longer holds — with the warnings index's compaction (tmp + rename). */
inline bool compact(const std::string& path, const std::unordered_set<uint64_t>& live) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    const std::string tmp = path + ".tmp";
    FILE* o = std::fopen(tmp.c_str(), "w");
    if (!o) { std::fclose(f); return false; }
    char buf[2048]; std::string cur;
    while (std::fgets(buf, sizeof buf, f)) {
        cur += buf;
        if (cur.back() != '\n') { if (cur.size() > 8192) cur.clear(); continue; }
        const size_t t1 = cur.find('\t'), t2 = t1 == std::string::npos ? t1 : cur.find('\t', t1 + 1);
        if (cur.compare(0, 3, "m1\t") == 0 && t2 != std::string::npos) {
            uint64_t h = 1469598103934665603ull;
            for (size_t i = t1 + 1; i < t2; ++i) { h ^= (unsigned char)cur[i]; h *= 1099511628211ull; }
            if (live.count(h)) std::fputs(cur.c_str(), o);
        }
        cur.clear();
    }
    std::fclose(f); std::fclose(o);
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
inline const Area* areaById(const std::string& id) { for (const auto& a : areas()) if (id == a.id) return &a; return nullptr; }
inline uint64_t fh(const std::string& s) { uint64_t h = 1469598103934665603ull; for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } return h; }

/** The latest forecast per area heard since `since` (live gallery items only), with up to kHistory earlier ones (repeats
 *  of the same text folded). */
struct AreaOut { const Area* a = nullptr; std::vector<Line> hist; uint32_t repeats = 0; };   // hist[0] = the latest
inline std::vector<AreaOut> query(const std::string& path, const std::unordered_set<uint64_t>& live, int64_t since) {
    std::map<std::string, AreaOut> by;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[2048]; std::string cur;
    while (std::fgets(buf, sizeof buf, f)) {
        cur += buf;
        if (cur.back() != '\n') { if (cur.size() > 8192) cur.clear(); continue; }
        cur.pop_back();
        Line l;
        const bool ok = parseLine(cur, l);
        cur.clear();
        if (!ok || l.t < since || !live.count(fh(l.file))) continue;
        const Area* a = areaById(l.area);
        if (!a) continue;
        auto& o = by[l.area]; o.a = a;
        o.hist.push_back(std::move(l));
    }
    std::fclose(f);
    std::vector<AreaOut> out;
    for (auto& kv : by) {
        auto& h = kv.second.hist;
        std::sort(h.begin(), h.end(), [](const Line& x, const Line& y) { return x.t > y.t; });
        // ★ the same forecast heard again (another slot, another station relaying it): one entry, counted
        std::vector<Line> keep; std::unordered_set<std::string> seen;
        for (auto& l : h) { if (!seen.insert(l.f.block).second) { if (!keep.empty() && keep[0].f.block == l.f.block) kv.second.repeats++; continue; } if (keep.size() <= kHistory) keep.push_back(l); }
        h.swap(keep);
        out.push_back(std::move(kv.second));
    }
    return out;
}
inline std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
/** `station(file)` → {name, conf} (the stations map's placement), or empty. */
using StationOf = std::function<std::pair<std::string, std::string>(const std::string& file)>;
inline std::string fcJson(const Line& l, const StationOf& st) {
    char b[200];
    std::snprintf(b, sizeof b, "{\"t\":%lld,\"fmin\":%d,\"fmax\":%d,\"gale\":%d,\"sea\":%d,\"vis\":%d", (long long)l.t, l.f.fmin, l.f.fmax, l.f.gale, l.f.sea, l.f.vis);
    std::string o = b;
    if (l.f.galeNative) o += ",\"native\":1";
    o += ",\"dir\":\"" + jesc(l.f.dir) + "\",\"wx\":\"" + jesc(l.f.wx) + "\",\"f\":\"" + jesc(l.file) + "\",\"txt\":\"" + jesc(l.f.block) + "\"";
    if (st) { const auto s = st(l.file); if (!s.first.empty()) o += ",\"st\":\"" + jesc(s.first) + "\",\"sc\":\"" + jesc(s.second) + "\""; }
    return o + "}";
}
inline std::string toJson(const std::vector<AreaOut>& v, int64_t now, const StationOf& st) {
    std::string o = "{\"now\":" + std::to_string((long long)now) + ",\"stale\":" + std::to_string((long long)kStaleSec) + ",\"areas\":[";
    bool first = true;
    char b[200];
    for (const auto& a : v) {
        if (a.hist.empty()) continue;
        if (!first) o += ','; first = false;
        std::snprintf(b, sizeof b, "{\"lat\":%.3f,\"lon\":%.3f,\"kind\":\"%c\",\"repeats\":%u,", a.a->lat, a.a->lon, a.a->kind, a.repeats);
        o += b;
        o += "\"id\":\"" + jesc(a.a->id) + "\",\"name\":\"" + jesc(a.a->name) + "\",\"fc\":[";
        for (size_t i = 0; i < a.hist.size(); ++i) { if (i) o += ','; o += fcJson(a.hist[i], st); }
        o += "]}";
    }
    return o + "]}";
}

} // namespace navmet
} // namespace vibe
