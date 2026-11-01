// vibe_navtex_map.h — V12 2026-11-01 THE NAVTEX MAP, stage 1: the STATIONS RECEIVED (Stuart: "it would be cool to be
// able to open a map and see which navtex stations you've received").
//
// ★★★ THE GALLERY IS THE SOURCE OF TRUTH — no second store. Every NAVTEX message this server heard (a listener's decoder
//     or background decoding) is already a gallery item whose index line carries its time, its dial and its TITLE — and
//     the title already names the station (vibe_bgdecode.h → navtex::label, the resolver every client shows:
//     "G · Cullercoats, United Kingdom 🇬🇧 · Nav warning · #98"). This file READS that index, line by line (never the
//     whole file in RAM), and folds it per station. When the gallery's cap evicts an item its line is gone, so the map
//     never shows what the gallery no longer holds ("ALL" = everything still in the gallery).
// ★★ ONE RESOLVER: the station is the one the title names (the label users already see). Where the label could not
//    decide — "W · Corsen 🇫🇷 or Valentia 🇮🇪?" or "Station W" — the map says AMBIGUOUS and lists the candidates
//    (navtex::resolveAll's own pair), and only then adds what the label never had: the time of day. A candidate beyond
//    MF ground-wave reach at a time when the path between is in daylight is dropped; one left = LIKELY. A station the
//    message named from its text, not in the table for that letter, is LIKELY, placed by name where the table has it.
// ★ Bounded: at most kMaxStations stations, kRecent message ids each; the scan streams the index with fgets.
// ★ No message text is read for this layer (titles only) — it costs one sequential read of index.jsonl (~2 MB for the
//   Lenovo's 4 500 messages).
#pragma once
#include "vibe_navtex_stations.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vibe {
namespace navmap {

static constexpr size_t kMaxStations = 400;
static constexpr size_t kRecent = 12;
static constexpr double kDxKm = 6000;   // ★ the furthest a NAVTEX sky-wave catch is believed (transatlantic at night)

// ── one index line ───────────────────────────────────────────────────────────────────────────────────────────────────
/** A string field of one JSON line as the gallery writes it (jsonEsc: \" \\ \uXXXX) — enough for our own index. */
inline bool jstr(const std::string& l, const char* key, std::string& out) {
    const std::string k = std::string("\"") + key + "\":\"";
    size_t p = l.find(k);
    if (p == std::string::npos) return false;
    p += k.size();
    out.clear();
    for (; p < l.size(); ++p) {
        const char c = l[p];
        if (c == '"') return true;
        if (c == '\\' && p + 1 < l.size()) {
            const char e = l[++p];
            if (e == 'u' && p + 4 < l.size()) { const unsigned v = (unsigned)std::strtoul(l.substr(p + 1, 4).c_str(), nullptr, 16); if (v < 0x80) out += (char)v; p += 4; }
            else if (e == 'n') out += ' ';
            else out += e;
            continue;
        }
        out += c;
    }
    return false;
}
inline bool jnum(const std::string& l, const char* key, double& out) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = l.find(k);
    if (p == std::string::npos) return false;
    char* e = nullptr;
    out = std::strtod(l.c_str() + p + k.size(), &e);
    return e != l.c_str() + p + k.size();
}

// ── the title ────────────────────────────────────────────────────────────────────────────────────────────────────────
/** What a NAVTEX gallery title says (src/utils/navtex.ts navtexTitle — keep in step):
 *  "G · Cullercoats, United Kingdom 🇬🇧 · Nav warning · #98 · no errors"   one station named
 *  "G · Name, Country 🇨🇦 · DX 4,700 km · Met warning · #02"                 …and far away
 *  "W · Corsen 🇫🇷 or Valentia 🇮🇪? · Nav warning · #12"                       the label's pair
 *  "W · Somewhere, Country 🇫🇷? · …"                                        named by the message's own text (before 2026-11-01)
 *  "E · Suspected Mondolfo, Italy 🇮🇹 · …"                                   the label's LIKELY (2026-11-01)
 *  "Start lost · N · Suspected Piombino, Italy 🇮🇹 · …"                      a start-lost message named by its own text
 *  "Station G · Met warning · #98"                                           no station could be named */
struct Title {
    char b1 = 0, b2 = 0;
    std::string num;                 ///< "98"
    std::string name, alt;           ///< the station named; the pair's other
    bool pair = false, textNamed = false, generic = false;
    int lost = -1;                   ///< "· N lost" / "no errors" (0) / not said (-1)
};
inline const std::vector<std::pair<const char*, char>>& subjects() {
    static const std::vector<std::pair<const char*, char>> s = {
        {"Nav warning (more)", 'L'}, {"Nav warning", 'A'}, {"Met warning", 'B'}, {"Ice report", 'C'}, {"SAR / piracy", 'D'},
        {"Weather forecast", 'E'}, {"Pilot / VTS", 'F'}, {"AIS", 'G'}, {"LORAN", 'H'}, {"Not used", 'I'}, {"SATNAV", 'J'},
        {"Other navaid", 'K'}, {"Test", 'T'}, {"Special service", 'V'}, {"No messages on hand", 'Z'},
    };
    return s;
}
inline std::vector<std::string> splitDots(const std::string& s) {
    static const std::string d = " \xc2\xb7 ";
    std::vector<std::string> f; size_t p = 0;
    for (;;) { const size_t q = s.find(d, p); if (q == std::string::npos) { f.push_back(s.substr(p)); break; } f.push_back(s.substr(p, q - p)); p = q + d.size(); }
    return f;
}
/** "Cullercoats, United Kingdom 🇬🇧" → "Cullercoats"; "Corsen 🇫🇷" → "Corsen". */
inline std::string placeName(std::string s) {
    const size_t c = s.find(','); if (c != std::string::npos) s.resize(c);
    while (!s.empty() && ((unsigned char)s.back() >= 0x80 || s.back() == ' ' || s.back() == '?')) s.pop_back();   // the flag, "?"
    return s;
}
inline Title parseTitle(const std::vector<std::string>& f, bool lostStart);
inline Title parseTitle(const std::string& title) {
    const auto f = splitDots(title);
    if (f.empty()) return Title();
    // ★ 2026-11-01: "Start lost · N · Suspected Piombino, Italy 🇮🇹 · …" — a start-lost message its own text names (likely)
    std::vector<std::string> g;
    if (f[0] == "Start lost" && f.size() > 2 && f[1].size() == 1 && f[1][0] >= 'A' && f[1][0] <= 'Z') { g.assign(f.begin() + 1, f.end()); return parseTitle(g, true); }
    return parseTitle(f, false);
}
inline Title parseTitle(const std::vector<std::string>& f, bool lostStart) {
    Title t;
    size_t i = 0;
    if (f.empty()) return t;
    if (f[0].size() == 9 && f[0].compare(0, 8, "Station ") == 0 && f[0][8] >= 'A' && f[0][8] <= 'Z') { t.b1 = f[0][8]; t.generic = true; i = 1; }
    else if (f[0].size() == 1 && f[0][0] >= 'A' && f[0][0] <= 'Z' && f.size() > 1) {
        t.b1 = f[0][0];
        const std::string& p = f[1];
        const size_t o = p.find(" or ");
        if (o != std::string::npos && !p.empty() && p.back() == '?') { t.pair = true; t.name = placeName(p.substr(0, o)); t.alt = placeName(p.substr(o + 4)); }
        // ★ 2026-11-01: "Suspected Mondolfo, Italy 🇮🇹" — the label's LIKELY (a clue, a corrected header letter)
        else if (p.compare(0, 10, "Suspected ") == 0) { t.name = placeName(p.substr(10)); t.textNamed = true; }
        else { t.name = placeName(p); t.textNamed = (!p.empty() && p.back() == '?') || lostStart; }
        i = 2;
        if (i < f.size() && f[i].compare(0, 3, "DX ") == 0) ++i;
    } else return t;
    for (; i < f.size(); ++i) {
        const std::string& x = f[i];
        if (!t.b2) for (const auto& s : subjects()) if (x == s.first) { t.b2 = s.second; break; }
        if (x.size() >= 2 && x[0] == '#') t.num = x.substr(1, 2);
        if (x == "no errors") t.lost = 0;
        if (x.size() > 5 && x.compare(x.size() - 5, 5, " lost") == 0) t.lost = std::atoi(x.c_str());
    }
    return t;
}

// ── the stations ─────────────────────────────────────────────────────────────────────────────────────────────────────
inline const navtex::Station* byName(char b1, double band, const std::string& name, bool anyBand) {
    size_t n = 0; const navtex::Station* t = navtex::stations(n);
    const std::string want = navtex::letters(name);
    if (want.empty()) return nullptr;
    for (size_t k = 0; k < n; ++k) if (t[k].b1 == b1 && (anyBand || t[k].khz == band) && navtex::letters(t[k].name) == want) return &t[k];
    if (anyBand) for (size_t k = 0; k < n; ++k) if (navtex::letters(t[k].name) == want) return &t[k];
    return nullptr;
}
/** ★ The sun's elevation (degrees) at a place and time — NOAA's low-precision formulae; ±1° is plenty here. */
inline double sunElev(double lat, double lon, int64_t t) {
    const double d = (double)t / 86400.0 + 2440587.5 - 2451545.0;
    const double r = 3.14159265358979323846 / 180.0;
    const double g = std::fmod(357.529 + 0.98560028 * d, 360.0) * r;
    const double q = std::fmod(280.459 + 0.98564736 * d, 360.0);
    const double L = (q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * r;
    const double e = (23.439 - 0.00000036 * d) * r;
    const double dec = std::asin(std::sin(e) * std::sin(L));
    const double ra = std::atan2(std::cos(e) * std::sin(L), std::cos(L));
    const double gmst = std::fmod(18.697374558 + 24.06570982441908 * d, 24.0) * 15.0 * r;
    const double ha = gmst + lon * r - ra;
    return std::asin(std::sin(lat * r) * std::sin(dec) + std::cos(lat * r) * std::cos(dec) * std::cos(ha)) / r;
}
/** ★ Could a sky-wave path carry this far at this time? MF beyond ground-wave reach needs the path's MIDPOINT in
 *  darkness (the D layer gone); HF (4209.5) is allowed at any hour. */
inline bool plausible(const navtex::Station* s, double rxLat, double rxLon, int64_t t) {
    if (!std::isfinite(rxLat) || !std::isfinite(rxLon)) return true;
    if (s->khz > 1000) return true;
    if (navtex::km(rxLat, rxLon, s->lat, s->lon) <= navtex::kReachMfKm) return true;
    return sunElev((rxLat + s->lat) / 2, (rxLon + s->lon) / 2, t) < -6.0;
}

struct Recent { std::string id, file; int64_t t = 0; char b2 = 0; };
struct Agg {
    const navtex::Station* st = nullptr;              ///< the station (nullptr = not placed)
    std::vector<const navtex::Station*> cands;        ///< ambiguous: the candidates
    std::string conf;                                  ///< "confident" | "likely" | "ambiguous"
    std::string name, iso, country; char b1 = 0;
    std::set<double> khz;
    int64_t first = 0, last = 0;
    uint64_t n = 0;
    std::set<std::string> ids;                         ///< distinct B2B3B4 (the map's dedupe — repeats counted once)
    std::map<char, uint64_t> subj;
    std::vector<Recent> recent;                        ///< newest first, kRecent
    uint64_t night = 0;                                ///< receptions at night at the path's midpoint (DX evidence)
    uint64_t folded = 0;                               ///< messages whose label was a pair, settled by this station's own confident copies
    // ── ★ the WHY (the card's reasons, 2026-11-01) ──
    uint64_t outright = 0;                             ///< messages whose label named this station outright
    std::map<std::string, uint64_t> ev;                ///< ★ how the others were placed: text / series / link / run / neighbour / dx / night
    uint64_t inSlot = 0;                               ///< messages saved inside the letter's transmit slot (slotFits)
};
/** ★★ THE NAVTEX SCHEDULE (IMO NAVTEX Manual / ITU-R M.540; NGA Pub. 117 lists the same plan): each letter transmits for
 *  10 minutes every 4 hours — A at 00:00, 04:00 … B at 00:10 … X at 03:50 UTC. The gallery's time is when the message
 *  ENDED (saved at NNNN), so a message fits its letter's slot when it ended between the slot's start and 25 minutes
 *  after (a long message runs over; a station-specific schedule in Pub. 117 is not in our table — those count as not
 *  fitting, never as evidence against). Evidence for the LETTER (a garbled B1 shows up as a run of misfits). */
inline bool slotFits(char b1, int64_t t) {
    if (b1 < 'A' || b1 > 'X') return false;
    const int64_t start = (int64_t)(b1 - 'A') * 600;
    const int64_t d = (((t % 14400) - start) % 14400 + 14400) % 14400;
    return d <= 25 * 60;
}
/** How many stations in the world table use this letter on this band. */
inline int sharing(char b1, double band) {
    size_t n = 0; const navtex::Station* t = navtex::stations(n);
    int c = 0; for (size_t i = 0; i < n; ++i) c += t[i].b1 == b1 && t[i].khz == band;
    return c;
}
struct Result {
    std::vector<Agg> stations;                         ///< most recently heard first
    uint64_t messages = 0, unplaced = 0, scanned = 0;
    uint64_t byRun = 0;                                ///< undecided labels settled by their transmission run
    uint64_t bySeries = 0;                             ///< …by the issuing authority's series among the candidates
    uint64_t byText = 0, byDx = 0, byNight = 0;        ///< …by the message's own text, the night-time nearest, the time of day
    uint64_t generic = 0, pairs = 0;                   ///< "Station X" titles and the label's pairs seen
    uint64_t bySeriesLink = 0, linkClash = 0;          ///< settled as the same warning number as a named message; numbers two authorities used
    uint64_t startLost = 0, startLostPlaced = 0;       ///< "Start lost" items, and those placed by their run
    int64_t oldest = 0;
};

/** One NAVTEX index line into the aggregate. */
/** ★ Reads one gallery item's text (bounded) — used ONLY for a title the label could not name ("Station F"): the
 *  resolver then gets the message's own evidence (positions, names, language), as the decoder's label would have had
 *  with a fuller table. Empty function = titles only. */
using ReadText = std::function<std::string(const std::string& file)>;
/** ★★ A TRANSMISSION RUN: a station sends its messages back to back in its slot, so every message on one band with one
 *  letter in one slot window (the same 4-hour cycle of that letter's slot) is the same transmitter. The key. */
inline std::string runKey(char b1, double band, int64_t t) {
    const int64_t start = b1 >= 'A' && b1 <= 'X' ? (int64_t)(b1 - 'A') * 600 : 0;
    return std::string(1, b1) + "|" + std::to_string((int)band) + "|" + std::to_string((long long)((t - start) / 14400));
}
using Runs = std::map<std::string, const navtex::Station*>;
/** A message whose label named its station outright: when, on which band, which letter — what a "Start lost" item
 *  (its header gone) is matched against: the named message beside it in the same slot. */
struct Named { int64_t t; double band; char b1; const navtex::Station* st; };
/** ★★ THE SAME WARNING ON ANOTHER TRANSMITTER: a file's series key ("BRE|370/26" — vibe_navtex_warn.h seriesKey, from the
 *  warnings index; "" when unknown). A named message carrying it gives its AUTHORITY to an undecided or start-lost message
 *  with the same key — the authority's transmitter on that band (Corsen sends A on 518 and E on 490). LIKELY. */
using SeriesOf = std::function<std::string(const std::string& file)>;
struct Ctx { Runs runs; std::vector<Named> named; std::map<std::string, const navtex::Station*> links; const SeriesOf* seriesOf = nullptr; };
/** The station `name`'s transmitter on `band` (its letter there), or nullptr. */
inline const navtex::Station* txOn(const navtex::Station* s, double band) {
    size_t n = 0; const navtex::Station* t = navtex::stations(n);
    for (size_t i = 0; i < n; ++i) if (t[i].khz == band && std::strcmp(t[i].name, s->name) == 0 && std::fabs(t[i].lat - s->lat) < 0.5) return &t[i];
    return nullptr;
}

/** ★★ ISSUING-AUTHORITY SERIES (2026-11-01) — the header a warning service puts on its own messages. ONLY where the label
 *  could not decide, and ONLY to choose among the label's own candidates (never a station the table does not offer):
 *  stations relay each other's series (Oostende sends WZ, Niton AVURNAV, Varberg Danish warnings — 264 of 2 290 named
 *  Lenovo messages), so a series is a clue to the CANDIDATE that issues it, not a name. Tolerant of one wrong letter
 *  ("AVUCNAV", "AVURNAVKBREST"). `name` empty = any candidate in that country. */
struct Series { const char* word; const char* place; const char* iso; const char* name; };
inline const std::vector<Series>& series() {
    static const std::vector<Series> t = {
        {"AVURNAV", "BREST", "FR", "Corsen"},          // Préfecture maritime de l'Atlantique (CROSS Corsen)
        {"AVURNAV", "CHERBOURG", "FR", ""},            // Manche et mer du Nord
        {"ESTONIAN", "NAV", "EE", "Tallinn"},
        {"SWEDISH", "NAV", "SE", ""},
        {"NORWEGIAN", "NAV", "NO", ""},
        {"DANISH", "NAV", "DK", ""},
        {"COSTAVURNAV", "", "IT", ""},
        {"OOSTENDERADIO", "", "BE", "Oostende"}, {"OOSTENDRADIO", "", "BE", "Oostende"},
    };
    return t;
}
/** `w` in the text, at most one letter wrong (same length), at a word start. */
inline bool fuzzy(const std::string& U, const char* w, size_t* end = nullptr) {
    const size_t n = std::strlen(w);
    for (size_t i = 0; i + n <= U.size(); ++i) {
        if (i && std::isalpha((unsigned char)U[i - 1])) continue;
        int bad = 0;
        for (size_t k = 0; k < n && bad <= 1; ++k) bad += U[i + k] != w[k];
        if (bad <= 1) { if (end) *end = i + n; return true; }
    }
    return false;
}
/** The candidate a series clue names, or nullptr (no clue, or it fits none / more than one). */
inline const navtex::Station* seriesPick(const std::string& text, const std::vector<const navtex::Station*>& cands) {
    if (cands.empty() || text.empty()) return nullptr;
    const std::string U = navtex::upperAscii(text);
    for (const auto& sr : series()) {
        size_t e = 0;
        if (!fuzzy(U, sr.word, &e)) continue;
        if (*sr.place) { const size_t from = e; const std::string rest = U.substr(from, 24); if (!fuzzy(rest, sr.place) && rest.find(sr.place) == std::string::npos) continue; }
        const navtex::Station* hit = nullptr; int n = 0;
        for (auto c : cands) if (std::strcmp(c->iso, sr.iso) == 0 && (!*sr.name || std::strcmp(c->name, sr.name) == 0)) { hit = c; ++n; }
        if (n == 1) return hit;
    }
    // ★ "WZ 649/26" at a line's start: the UK's series
    for (size_t p = 0; p < U.size(); p = U.find('\n', p) + 1) {
        size_t q = p; while (q < U.size() && U[q] == ' ') ++q;
        if (U.compare(q, 3, "WZ ") == 0 && q + 3 < U.size() && std::isdigit((unsigned char)U[q + 3])) {
            const navtex::Station* hit = nullptr; int n = 0;
            for (auto c : cands) if (std::strcmp(c->iso, "GB") == 0) { hit = c; ++n; }
            if (n == 1) return hit;
            break;
        }
        if (U.find('\n', p) == std::string::npos) break;
    }
    return nullptr;
}
inline void take(std::map<std::string, Agg>& by, Result& res, const std::string& line, double rxLat, double rxLon, int64_t since,
                 const ReadText& readText = nullptr, Runs* runs = nullptr, bool settle = false, std::vector<Named>* named = nullptr,
                 Ctx* ctx = nullptr) {
    if (line.find("\"k\":\"navtex\"") == std::string::npos) return;
    double t = 0, hz = 0;
    std::string title, file;
    if (!jnum(line, "t", t) || !jnum(line, "hz", hz) || !jstr(line, "title", title)) return;
    jstr(line, "f", file);
    res.scanned++;
    if ((int64_t)t < since) return;
    Title ti = parseTitle(title);
    const navtex::Station* lostSt = nullptr;
    const std::string skey = ctx && ctx->seriesOf && *ctx->seriesOf ? (*ctx->seriesOf)(file) : std::string();
    bool bySeriesLink = false;
    // ★ undecided or start lost, and the same warning number was carried by a named message: its authority's transmitter
    if (settle && ctx && !skey.empty() && (ti.generic || ti.pair || !ti.b1) && (int64_t)t >= since) {
        const auto it = ctx->links.find(skey);
        if (it != ctx->links.end() && it->second) {
            if (const navtex::Station* tx = txOn(it->second, navtex::bandKhz(hz)))
                if (!ti.b1 || tx->b1 == ti.b1) { lostSt = tx; ti.b1 = tx->b1; bySeriesLink = true; if (title.compare(0, 10, "Start lost") == 0) { res.startLost++; res.startLostPlaced++; } }
        }
    }
    if (!lostSt && !ti.b1 && title.compare(0, 10, "Start lost") == 0) {
        res.startLost++;
        /* ★★ A START-LOST ITEM (its ZCZC header damaged or missed): the station of the message beside it — a message named
         *  outright on the same band within 12 minutes, in that letter's slot; a surviving "ZCZC EA" fragment must agree.
         *  Only when every such neighbour is ONE station. LIKELY, never confident. */
        if (settle && named && (int64_t)t >= since) {
            char frag = 0;
            const std::string text = readText && !file.empty() ? readText(file) : std::string();
            const size_t z = text.find("ZCZC ");
            if (z != std::string::npos && z + 5 < text.size() && std::isupper((unsigned char)text[z + 5])) frag = text[z + 5];
            const double bnd = navtex::bandKhz(hz);
            const navtex::Station* one = nullptr; char b1 = 0; bool clash = false;
            for (const auto& nm : *named) {
                if (nm.band != bnd || std::llabs(nm.t - (int64_t)t) > 12 * 60 || !slotFits(nm.b1, (int64_t)t) || (frag && nm.b1 != frag)) continue;
                if (one && one != nm.st) clash = true;
                one = nm.st; b1 = nm.b1;
            }
            if (one && !clash) { lostSt = one; ti.b1 = b1; res.startLostPlaced++; }
        }
    }
    if (!ti.b1) { res.unplaced++; return; }
    res.messages++;
    if (!res.oldest || (int64_t)t < res.oldest) res.oldest = (int64_t)t;
    const double band = navtex::bandKhz(hz);
    const navtex::Station* st = nullptr;
    std::vector<const navtex::Station*> cands;
    std::string conf;
    if (settle && ti.generic) res.generic++;
    if (settle && ti.pair) res.pairs++;
    std::string why;         // ★ the evidence that placed an undecided message (the card's plain-English lines)
    bool fromText = false;   // ★ settled by the message's own text (the resolver with it, or its series): a neighbour may lean on it
    bool byRun = lostSt != nullptr;
    if (lostSt) { st = lostSt; conf = "likely"; why = bySeriesLink ? "link" : "neighbour"; }
    // ★ the label could not decide, but its run did: a message in the same slot window named the station outright
    if (!lostSt && settle && runs && (ti.pair || ti.generic)) {
        const auto it = runs->find(runKey(ti.b1, band, (int64_t)t));
        if (it != runs->end() && it->second) { st = it->second; conf = "likely"; byRun = true; why = "run"; }
    }
    if (byRun) {
        // settled by the run (above)
    } else if (!ti.pair && !ti.generic && !ti.name.empty()) {
        st = byName(ti.b1, band, ti.name, false);
        if (st) conf = ti.textNamed ? "likely" : "confident";
        else if ((st = byName(ti.b1, band, ti.name, true))) conf = "likely";     // ★ named by its text, another band in the table
    } else {
        if (ti.pair) { if (auto a = byName(ti.b1, band, ti.name, false)) cands.push_back(a); if (auto b = byName(ti.b1, band, ti.alt, false)) cands.push_back(b); }
        else {
            const std::string text = readText && !file.empty() ? readText(file) : std::string();
            const auto r = navtex::resolveAll(ti.b1, hz, rxLat, rxLon, text);
            cands = r.stations;
            if (r.stations.size() == 1) { st = r.stations[0]; conf = "likely"; cands.clear(); res.byText++; fromText = true; why = "text"; }   // ★ the table now names it
            /* ★ "Station F": nothing on that letter and band within ground-wave reach, so the label named none. Sky wave
             *  at night carries 518/490 kHz thousands of km (Nick's Canadian catches): every station on the letter and
             *  band, nearest first, out to kDxKm, those the time of day allows. One, or one clearly nearest (half the
             *  next one's distance) = LIKELY; otherwise the nearest two, AMBIGUOUS. */
            if (!st && cands.empty() && band && std::isfinite(rxLat) && std::isfinite(rxLon)) {
                size_t n = 0; const navtex::Station* tb = navtex::stations(n);
                std::vector<std::pair<double, const navtex::Station*>> all;
                for (size_t k = 0; k < n; ++k)
                    if (tb[k].b1 == ti.b1 && tb[k].khz == band) {
                        const double d = navtex::km(rxLat, rxLon, tb[k].lat, tb[k].lon);
                        if (d <= kDxKm && plausible(&tb[k], rxLat, rxLon, (int64_t)t)) all.push_back({d, &tb[k]});
                    }
                std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                if (all.size() == 1 || (all.size() > 1 && all[0].first <= 0.5 * all[1].first)) { st = all[0].second; conf = "likely"; cands.clear(); res.byDx++; why = "dx"; }
                else for (size_t k = 0; k < all.size() && k < 2; ++k) cands.push_back(all[k].second);
            }
        }
        // ★ the label could not decide: the issuing authority's series, among its own candidates
        if (!st && !cands.empty()) {
            std::vector<const navtex::Station*> pool = cands;
            if (pool.size() < 2 && band) { pool.clear(); size_t n = 0; const navtex::Station* tb = navtex::stations(n);
                for (size_t k = 0; k < n; ++k) if (tb[k].b1 == ti.b1 && tb[k].khz == band) pool.push_back(&tb[k]); }
            const std::string text = readText && !file.empty() ? readText(file) : std::string();
            if (const navtex::Station* sp = seriesPick(text, pool)) { st = sp; conf = "likely"; res.bySeries++; cands.clear(); fromText = true; why = "series"; }
        }
        // ★ the label could not decide: what the time of day rules out
        if (!st) {
            std::vector<const navtex::Station*> ok;
            for (auto c : cands) if (plausible(c, rxLat, rxLon, (int64_t)t)) ok.push_back(c);
            if (ok.size() == 1 && cands.size() > 1) { st = ok[0]; conf = "likely"; res.byNight++; why = "night"; }
            else if (!cands.empty()) { st = cands[0]; conf = "ambiguous"; }
        }
    }
    if (!st) { res.unplaced++; return; }
    if (named && ((conf == "confident" && !ti.textNamed) || fromText) && named->size() < 200000) named->push_back({(int64_t)t, band, ti.b1, st});
    if (ctx && !skey.empty() && conf == "confident" && !ti.textNamed) {
        auto& l = ctx->links[skey];
        if (!l) l = st; else if (std::strcmp(l->name, st->name) != 0) { l = nullptr; res.linkClash++; }   // ★ two authorities: no link
    }
    if (bySeriesLink) res.bySeriesLink++;
    if (runs && conf == "confident" && !ti.textNamed) {
        auto& slot = (*runs)[runKey(ti.b1, band, (int64_t)t)];
        if (!slot) slot = st; else if (slot != st) slot = nullptr;   // ★ two stations in one window: no inheritance
    }
    if (byRun && !lostSt) res.byRun++;
    const std::string key = conf == "ambiguous"
        ? std::string("?") + ti.b1 + "|" + std::to_string((int)band) + "|" + cands[0]->name + "|" + (cands.size() > 1 ? cands[1]->name : "")
        : std::string(1, ti.b1) + "|" + st->name;
    auto it = by.find(key);
    if (it == by.end()) { if (by.size() >= kMaxStations) { res.unplaced++; return; } it = by.emplace(key, Agg{}).first; }
    Agg& a = it->second;
    if (!a.n) { a.st = st; a.cands = cands; a.conf = conf; a.b1 = ti.b1; a.name = st->name; a.iso = st->iso; a.country = st->country; a.first = (int64_t)t; }
    else if (conf == "confident") a.conf = "confident";                    // ★ any confident copy settles it
    a.n++;
    if (conf == "confident" && !ti.textNamed) a.outright++;
    if (!why.empty()) a.ev[why]++;
    if (slotFits(ti.b1, (int64_t)t)) a.inSlot++;
    a.khz.insert(band ? band : hz / 1000.0);
    if ((int64_t)t < a.first) a.first = (int64_t)t;
    if ((int64_t)t > a.last) a.last = (int64_t)t;
    const std::string id = std::string(1, ti.b1) + (ti.b2 ? std::string(1, ti.b2) : "?") + (ti.num.empty() ? "??" : ti.num);
    a.ids.insert(id.substr(1));
    if (ti.b2) a.subj[ti.b2]++;
    if (std::isfinite(rxLat) && sunElev((rxLat + st->lat) / 2, (rxLon + st->lon) / 2, (int64_t)t) < -6.0) a.night++;
    a.recent.push_back({id, file, (int64_t)t, ti.b2});
    std::sort(a.recent.begin(), a.recent.end(), [](const Recent& x, const Recent& y) { return x.t > y.t; });
    if (a.recent.size() > kRecent) a.recent.resize(kRecent);
}

/** ★★ The source: every live NAVTEX item's current line, oldest first. `each(fn)` calls fn(line) for each — the
 *  gallery Store's forEachLive (the append-only index read through its table: replaced and removed items never seen
 *  twice or at all), or a plain file of lines for tests and tools. */
using EachLine = std::function<void(const std::function<void(const std::string&)>&)>;
inline EachLine fileLines(const std::string& path) {
    return [path](const std::function<void(const std::string&)>& fn) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return;
        char buf[4096];
        std::string cur;
        while (std::fgets(buf, sizeof buf, f)) {
            cur += buf;
            if (cur.back() != '\n') { if (cur.size() > 65536) cur.clear(); continue; }
            cur.pop_back();
            fn(cur);
            cur.clear();
        }
        std::fclose(f);
    };
}
/** ★ Fold the NAVTEX messages heard since `since` per station. A title the label could not name whose text is wanted
 *  is DEFERRED (the line kept, at most kDefer) and read after the walk — `each` may hold the Store's lock. */
static constexpr size_t kDefer = 4000;
inline Result scan(const EachLine& each, double rxLat, double rxLon, int64_t since, const ReadText& readText = nullptr,
                   const SeriesOf& seriesOf = nullptr) {
    Ctx ctx; ctx.seriesOf = &seriesOf;
    Result res;
    std::map<std::string, Agg> by;
    std::vector<std::string> later, lost;
    Runs runs;
    std::vector<Named> named;
    // ★ pass 1: every title that names its station (and records its run); the undecided ones — "Station F", a pair —
    //   are kept (≤ kDefer) for pass 2, after every run is known
    each([&](const std::string& l) {
        const bool undecided = l.find("\"title\":\"Station ") != std::string::npos || l.find("? \xc2\xb7 ") != std::string::npos
                            || l.find("\"title\":\"Start lost") != std::string::npos;
        if (undecided && later.size() + lost.size() < kDefer) { (l.find("\"title\":\"Start lost") != std::string::npos ? lost : later).push_back(l); return; }
        take(by, res, l, rxLat, rxLon, since, nullptr, &runs, false, &named, &ctx);
    });
    for (const auto& l : later) take(by, res, l, rxLat, rxLon, since, readText, &runs, true, &named, &ctx);
    for (const auto& l : lost) take(by, res, l, rxLat, rxLon, since, readText, &runs, true, &named, &ctx);   // ★ last: every neighbour known
    /* ★ A PAIR SETTLED BY THE REST OF THE HISTORY: "M · Jeløy or …?" when Jeløy on that letter and band has been named
     *  outright (or likely, from its messages' own text) by other messages, and no other candidate has — the pair's
     *  messages join it (counted as `folded`). Otherwise the pair stays a "?". */
    for (auto it = by.begin(); it != by.end();) {
        Agg& a = it->second;
        Agg* home = nullptr;
        int homes = 0;   // ★ only when exactly ONE candidate has been named outright — two would be a guess
        if (a.conf == "ambiguous")
            for (auto c : a.cands) {
                auto h = by.find(std::string(1, a.b1) + "|" + c->name);
                if (h != by.end() && h->second.conf != "ambiguous" && h->second.khz.count(c->khz)) { home = &h->second; ++homes; }
            }
        if (homes != 1) home = nullptr;
        if (!home) { ++it; continue; }
        home->n += a.n; home->folded += a.n; home->night += a.night; home->inSlot += a.inSlot;
        for (auto& e : a.ev) home->ev[e.first] += e.second;
        home->first = std::min(home->first, a.first); home->last = std::max(home->last, a.last);
        for (auto& i : a.ids) home->ids.insert(i);
        for (auto& s2 : a.subj) home->subj[s2.first] += s2.second;
        for (auto& r : a.recent) home->recent.push_back(r);
        std::sort(home->recent.begin(), home->recent.end(), [](const Recent& x, const Recent& y) { return x.t > y.t; });
        if (home->recent.size() > kRecent) home->recent.resize(kRecent);
        it = by.erase(it);
    }
    for (auto& kv : by) res.stations.push_back(std::move(kv.second));
    std::sort(res.stations.begin(), res.stations.end(), [](const Agg& x, const Agg& y) { return x.last > y.last; });
    return res;
}

/** A file of index lines (tests, tools). */
inline Result scanIndex(const std::string& indexPath, double rxLat, double rxLon, int64_t since, const ReadText& readText = nullptr) {
    return scan(fileLines(indexPath), rxLat, rxLon, since, readText);
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
/** The answer: {"rx":{lat,lon}|null,"messages":N,"unplaced":U,"oldest":T,"stations":[…]}. */
inline std::string toJson(const Result& r, double rxLat, double rxLon, int64_t now) {
    char b[256];
    std::string o = "{";
    if (std::isfinite(rxLat) && std::isfinite(rxLon)) { std::snprintf(b, sizeof b, "\"rx\":{\"lat\":%.4f,\"lon\":%.4f},", rxLat, rxLon); o += b; }
    else o += "\"rx\":null,";
    std::snprintf(b, sizeof b, "\"now\":%lld,\"messages\":%llu,\"unplaced\":%llu,\"byRun\":%llu,\"oldest\":%lld,\"stations\":[", (long long)now,
                  (unsigned long long)r.messages, (unsigned long long)r.unplaced, (unsigned long long)r.byRun, (long long)r.oldest);
    o += b;
    bool firstS = true;
    for (const Agg& a : r.stations) {
        if (!firstS) o += ','; firstS = false;
        std::snprintf(b, sizeof b, "{\"b1\":\"%c\",\"lat\":%.3f,\"lon\":%.3f,\"first\":%lld,\"last\":%lld,\"n\":%llu,\"uniq\":%zu,\"night\":%llu,\"folded\":%llu,",
                      a.b1, a.st->lat, a.st->lon, (long long)a.first, (long long)a.last, (unsigned long long)a.n, a.ids.size(), (unsigned long long)a.night,
                      (unsigned long long)a.folded);
        o += b;
        o += "\"name\":\"" + jesc(a.name) + "\",\"iso\":\"" + jesc(a.iso) + "\",\"country\":\"" + jesc(a.country) + "\",\"conf\":\"" + a.conf + "\"";
        if (std::isfinite(rxLat)) { std::snprintf(b, sizeof b, ",\"km\":%.0f", navtex::km(rxLat, rxLon, a.st->lat, a.st->lon)); o += b; }
        o += ",\"khz\":[";
        bool f1 = true; for (double k : a.khz) { if (!f1) o += ','; f1 = false; std::snprintf(b, sizeof b, "%g", k); o += b; }
        o += "],\"subj\":{";
        f1 = true; for (const auto& s : a.subj) { if (!f1) o += ','; f1 = false; std::snprintf(b, sizeof b, "\"%c\":%llu", s.first, (unsigned long long)s.second); o += b; }
        o += "}";
        // ★ the why: messages named outright, inside the letter's slot; how many stations share the letter and band; the
        //   nearest two of them to the receiver
        {
            const double band = a.khz.empty() ? 0 : *a.khz.begin();
            std::snprintf(b, sizeof b, ",\"outright\":%llu,\"inSlot\":%llu,\"share\":%d", (unsigned long long)a.outright,
                          (unsigned long long)a.inSlot, sharing(a.b1, band));
            o += b;
            o += ",\"ev\":{";
            bool fe = true; for (auto& e : a.ev) { if (!fe) o += ','; fe = false; o += "\"" + e.first + "\":" + std::to_string((unsigned long long)e.second); }
            o += "}";
            if (std::isfinite(rxLat)) {
                size_t n = 0; const navtex::Station* t = navtex::stations(n);
                std::vector<std::pair<double, const navtex::Station*>> near;
                for (size_t i = 0; i < n; ++i) if (t[i].b1 == a.b1 && t[i].khz == band) near.push_back({navtex::km(rxLat, rxLon, t[i].lat, t[i].lon), &t[i]});
                std::sort(near.begin(), near.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                o += ",\"near\":[";
                for (size_t i = 0; i < near.size() && i < 3; ++i) {
                    if (i) o += ',';
                    std::snprintf(b, sizeof b, "{\"km\":%.0f,\"name\":\"", near[i].first);
                    o += b; o += jesc(near[i].second->name) + "\",\"iso\":\"" + jesc(near[i].second->iso) + "\"}";
                }
                o += "]";
            }
        }
        if (a.conf == "ambiguous" && a.cands.size() > 1) {
            o += ",\"cands\":[";
            for (size_t i = 0; i < a.cands.size(); ++i) {
                if (i) o += ',';
                std::snprintf(b, sizeof b, "{\"lat\":%.3f,\"lon\":%.3f,", a.cands[i]->lat, a.cands[i]->lon);
                o += b; o += "\"name\":\"" + jesc(a.cands[i]->name) + "\",\"iso\":\"" + jesc(a.cands[i]->iso) + "\"";
                if (std::isfinite(rxLat)) { std::snprintf(b, sizeof b, ",\"km\":%.0f", navtex::km(rxLat, rxLon, a.cands[i]->lat, a.cands[i]->lon)); o += b; }
                o += "}";
            }
            o += "]";
        }
        o += ",\"recent\":[";
        for (size_t i = 0; i < a.recent.size(); ++i) {
            if (i) o += ',';
            std::snprintf(b, sizeof b, "{\"t\":%lld,", (long long)a.recent[i].t);
            o += b; o += "\"id\":\"" + jesc(a.recent[i].id) + "\",\"f\":\"" + jesc(a.recent[i].file) + "\"}";
        }
        o += "]}";
    }
    return o + "]}";
}

} // namespace navmap
} // namespace vibe
