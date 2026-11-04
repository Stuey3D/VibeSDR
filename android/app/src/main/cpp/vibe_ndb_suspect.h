// vibe_ndb_suspect.h — WHAT AN UNLISTED NDB PROBABLY IS, and when a LISTED match is probably wrong. V12 2026-11-04.
//
// The published lists miss many real beacons (the UK's SND / CDF / EMW were heard for days, "not in the published
// list", before we added them by hand). The NAVTEX station resolver's pattern (vibe_navtex_*.h): a "Suspected X" clue
// with its plain evidence beside it — NEVER shown as the ident's station, never counted, never a solid dot. The server
// decides; the clients only display. The owner may CONFIRM a clue (OwnerBeacons, vibe_ndb_db.h): from then on it is an
// Exact match on that server.
//
// ★★ 1. THE AIRFIELD-NAME CLUE (suspects()). A locator is nearly always named after its airfield. Candidates: the
//    airfields of vibe_ndb_airfields.h (OurAirports, public domain: every large/medium/small airport with an ICAO
//    indicator) within reach of the receiver, scored on how the ident's letters fit the airfield (best rule wins):
//        100  IATA code == ident                     ("NQY is Newquay's IATA code")
//         75  initials of consecutive name words     ("the initials of East Midlands")
//         70  ICAO code ends in the ident (3 letters) ("the end of its ICAO code EGSN")
//         70  the first letters of one name word     ("the first letters of Cambridge"; 55 for a 2-letter ident)
//         60  the letters, in order, of one name word, starting with its first ("the letters of Southend"),
//             +4 for each of its letters that sits next to the one before (SND: the N-D of SoutheND; ≤ 68)
//         55  ICAO code ends in the ident (2 letters)
//         45  the letters, in order, across the whole name from its first word (3+ letters only)
//      +5 large / +3 medium airport (a bigger field is likelier to have a locator); then minus 1 per 25 km.
//    Generic words never count (Airport, International, RAF, Air Base…), so "RAF Mildenhall" is matched on Mildenhall.
//    ★ Absurd matches refused: idents of letters only, 2–4 long; a score below 45 (55 for a two-letter ident, whose
//      letters fit far too many names) is no clue at all; two-letter idents only within 300 km and never a small
//      field; one-letter words never count; beyond 600 km only a fit of 70+; an airfield already published with a
//      beacon of the SAME ident (on another frequency) is the better explanation, so it wins by +20.
//    Reach: 600 km if the beacon was heard by day (ground wave), 1 500 km if only after dark, else 600 km.
//
// ★★ 2. DAY OR NIGHT (DayNight). LF/MF sky wave is absorbed by the D layer in daylight: a beacon heard STEADILY with
//    the sun well up at the receiver (≥ 6°: clear of the grey line's dawn/dusk enhancement) arrives by ground wave,
//    which for a 25–100 W NDB reaches a few hundred km — 600 km is generous for a good receiver and antenna. After dark (sun ≤ −6°) sky wave can bring one from 1 000s of km.
//      • "heard by day"  = ≥ 10 minutes heard with the sun ≥ 6° up at the receiver.
//      • "only after dark" = ≥ 10 minutes with the sun ≤ −6°, and fewer than 3 by day.
//    ★ An EXACT list match > 1 500 km away that was heard by day is UNLIKELY (the real case: "IS" on 349.97 kHz,
//      matched to Ishurdi, Bangladesh, 7 901 km — heard through the morning: a data carrier whose noise reads as I S).
//      1 500 km: 2.5× the ground-wave reach above, so a long sea path or an exceptional day never trips it. An unlikely
//      match keeps its name on the card, says so plainly, is never the furthest and draws no dot.
//
// ★ 3. THE TONE is NOT used: the airfield table has no tones, and 400 vs 1 020 Hz says nothing about which airfield.
#pragma once
#include "vibe_ndb_airfields.h"
#include "vibe_ndb_db.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vibendb {

constexpr double kDayReachKm = 600, kNightReachKm = 1500, kUnlikelyKm = 1500;
constexpr double kSunDayDeg = 6, kSunNightDeg = -6;
constexpr int kClassMin = 10;

/** The sun's elevation (°) at (lat, lon) at UTC second t — the low-precision solar position (≈ 0.1°, plenty here). */
inline double sunElevationDeg(double lat, double lon, long long t) {
    const double r = M_PI / 180.0;
    const double d = (double)t / 86400.0 - 10957.5;   // days since J2000.0
    const double g = (357.529 + 0.98560028 * d) * r;
    const double q = 280.459 + 0.98564736 * d;
    const double L = (q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * r;
    const double e = (23.439 - 0.00000036 * d) * r;
    const double ra = std::atan2(std::cos(e) * std::sin(L), std::cos(L));
    const double dec = std::asin(std::sin(e) * std::sin(L));
    const double gmst = std::fmod(18.697374558 + 24.06570982441908 * d, 24.0) * 15.0 * r;
    const double ha = gmst + lon * r - ra;
    return std::asin(std::sin(lat * r) * std::sin(dec) + std::cos(lat * r) * std::cos(dec) * std::cos(ha)) / r;
}

/** Minutes a beacon was heard by day and after dark (the receiver's sun). */
struct DayNight {
    int day = 0, night = 0;
    void add(double sunDeg) { if (sunDeg >= kSunDayDeg) ++day; else if (sunDeg <= kSunNightDeg) ++night; }
    bool byDay() const { return day >= kClassMin; }
    bool nightOnly() const { return night >= kClassMin && day < 3; }
    const char* words() const { return byDay() ? "heard by day" : nightOnly() ? "heard only after dark" : ""; }
};

/** "" or why an EXACT match is unlikely (plain words). */
inline std::string unlikelyWhy(double km, const DayNight& dn) {
    if (km <= kUnlikelyKm || !dn.byDay()) return std::string();
    char b[200];
    std::snprintf(b, sizeof b, "%.0f km away yet heard by day \xe2\x80\x94 too far for a daytime beacon: probably a data carrier or another station", km);
    return b;
}

struct Suspect {
    const AirfieldRec* a = nullptr;
    double km = 0, deg = 0, rank = 0;
    int score = 0;
    std::string why;    ///< "SND — the letters of Southend · 62 km · heard by day"
    std::string icao() const { return a ? std::string(a->icao) : std::string(); }
    std::string name() const { return a ? std::string(kAirfieldNames + a->name) : std::string(); }
    std::string cc() const { return a && std::strcmp(a->cc, "--") ? std::string(a->cc) : std::string(); }
};

namespace detail {
inline bool generic(const std::string& w) {
    static const char* const k[] = { "AIRPORT", "AIRPORTS", "AIRFIELD", "AERODROME", "AERODROMO", "AEROPORTO", "AEROPORT", "AEROPUERTO", "FLUGHAFEN", "FLUGPLATZ",
                                     "INTERNATIONAL", "INTL", "AIR", "BASE", "AB", "AFB", "RAF", "RNAS", "AAC", "MOD", "NAS", "ARMY", "NAVAL", "STATION",
                                     "FIELD", "REGIONAL", "MUNICIPAL", "AERO", "CLUB", "FLYING", "GLIDING", "HELIPORT", "THE", "OF", "DE", "DU", "DEL", "LA", "LE",
                                     "AND", "AIRSTRIP", "STRIP", "LANDING", "COUNTY", "MILITARY", "JOINT", "RESERVE" };
    for (const char* g : k) if (w == g) return true;
    return false;
}
/** A significant word of a name: upper case for matching (an accented letter is '#'), and as written. */
struct Word { std::string up, show; };
inline std::vector<Word> words(const std::string& name) {
    std::vector<Word> v; std::string w, sh;
    // ★ one-letter words never count ("Land's End" is LANDS END, not LAND S END); a letter with an accent (UTF-8
    //   bytes) stays INSIDE its word but matches nothing, so "Leźnica" is one word, not "nica"
    auto flush = [&] { if (w.size() >= 2 && !generic(w)) v.push_back({ w, sh }); w.clear(); sh.clear(); };
    for (unsigned char c : name) {
        if (c == '\'') { sh += (char)c; continue; }
        if (std::isalpha(c)) { w += (char)std::toupper(c); sh += (char)c; }
        else if (c >= 0x80) { if ((c & 0xC0) != 0x80) w += '#'; sh += (char)c; }
        else flush();
    }
    flush();
    return v;
}
/** Is `id` a subsequence of `s` starting at s[0]? `adj`: how many of the ident's letters sit next to the one before
 *  (the earliest fit) — "SND" in SOUTHEND has N-D together, in STANSTED none: a locator's letters are usually the
 *  name's own consonant clusters, so each such pair is worth a little (+4). */
inline bool subseqFromStart(const std::string& id, const std::string& s, int* adj = nullptr) {
    if (id.empty() || s.empty() || s[0] != id[0]) return false;
    size_t j = 1, prev = 0; int a = 0;
    for (size_t i = 1; i < s.size() && j < id.size(); ++i) if (s[i] == id[j]) { a += i == prev + 1; prev = i; ++j; }
    if (adj) *adj = a;
    return j == id.size();
}
/** The score of one airfield for an ident, and the evidence. */
inline int score(const std::string& id, const AirfieldRec& a, std::string& how) {
    int best = 0;
    auto take = [&](int s, const std::string& h) { if (s > best) { best = s; how = h; } };
    if (id.size() == 3 && id == a.iata) take(100, id + " is its IATA code");
    if (id.size() == 3 && id == std::string(a.icao + 1)) take(70, "the end of its ICAO code " + std::string(a.icao));
    if (id.size() == 2 && id == std::string(a.icao + 2)) take(55, "the end of its ICAO code " + std::string(a.icao));
    const std::vector<Word> w = words(kAirfieldNames + a.name);
    for (size_t i = 0; i + id.size() <= w.size(); ++i) {
        bool ok = true; std::string ws;
        for (size_t k = 0; k < id.size(); ++k) { ok = ok && w[i + k].up[0] == id[k]; ws += (k ? " " : "") + w[i + k].show; }
        if (ok) take(75, "the initials of " + ws);
    }
    for (const auto& x : w) {
        if (x.up.size() > id.size() && x.up.compare(0, id.size(), id) == 0) take(id.size() == 2 ? 55 : 70, "the first letters of " + x.show);
        else if (int adj = 0; id.size() >= 3 && subseqFromStart(id, x.up, &adj)) take(60 + 4 * adj, "the letters of " + x.show);
    }
    // ★ across the name's words — only from its FIRST significant word (a run starting mid-name fits anything)
    if (id.size() >= 3 && best < 45 && w.size() >= 2) {
        std::string all; for (const auto& x : w) all += x.up;
        if (subseqFromStart(id, all)) {
            std::string ws; for (size_t k = 0; k < w.size(); ++k) ws += (k ? " " : "") + w[k].show;
            take(45, "the letters of " + ws);
        }
    }
    return best;
}
} // namespace detail

/** Is `ident` one the clue may try (letters only, 2–4)? */
inline bool suspectable(const std::string& id) {
    if (id.size() < 2 || id.size() > 4) return false;
    bool informative = false;
    for (char c : id) { if (c < 'A' || c > 'Z') return false; informative |= !std::strchr("EISHT", c); }
    // ★ only E I S H T (all dots, or one dash): what noise and data carriers read as (the cards' EEI, IEE, EIE, ES, SE
    //   on a dozen frequencies each, IS on Ishurdi's) — no airfield is guessed for them
    return informative;
}

/** The best airfields for an unlisted ident heard on hz, best first (at most `n`). Empty: no position, an ident the
 *  clue does not try, or nothing that fits. `key` (OwnerBeacons::key) filters the owner's rejected airfields. */
inline std::vector<Suspect> suspects(const std::string& ident, double hz, bool havePos, double lat, double lon,
                                     const DayNight& dn, size_t n = 3) {
    std::vector<Suspect> out;
    if (!havePos || !suspectable(ident)) return out;
    const double reach = std::min(dn.nightOnly() ? kNightReachKm : kDayReachKm, ident.size() == 2 ? 300.0 : 1e9);
    const int minScore = ident.size() == 2 ? 55 : 45;
    const std::string key = OwnerBeacons::key(ident, hz);
    // ★ a latitude band first (the table is sorted by latitude): reach / 111 km per degree
    const double dLat = reach / 111.0 + 0.1;
    const AirfieldRec* b = kAirfields; const AirfieldRec* e = kAirfields + kAirfieldCount;
    const AirfieldRec* lo = std::lower_bound(b, e, lat - dLat, [](const AirfieldRec& r, double v) { return r.lat < v; });
    for (const AirfieldRec* a = lo; a < e && a->lat <= lat + dLat; ++a) {
        std::string how;
        int s = detail::score(ident, *a, how);
        if (s < minScore) continue;
        if (ident.size() == 2 && a->size == 0) continue;   // ★ two letters fit too many small fields' names
        double km, deg; distBearing(lat, lon, a->lat, a->lon, km, deg);
        if (km > reach) continue;
        if (km > kDayReachKm && s < 70) continue;           // ★ beyond ground-wave reach only a strong fit is a clue
        if (ownerBeacons().rejected(key, a->icao)) continue;
        // ★ the same ident published at this airfield on another frequency (a beacon that moved, a list's old entry)
        bool samePub = false;
        for (size_t i = 0; i < kNavaidCount && !samePub; ++i) {
            const NavaidRec& r = kNavaids[i];
            // ★ not our own receptions (src 3): those were placed AT the airfield from this very clue's reasoning
            if (r.src != 3 && ident == r.ident && std::fabs(r.hz - hz) > kTolHz && std::fabs(r.lat - a->lat) < 0.3 && std::fabs(r.lon - a->lon) < 0.5) samePub = true;
        }
        if (samePub) { s += 20; how += "; it is published there on another frequency"; }
        Suspect x; x.a = a; x.km = km; x.deg = deg; x.score = s;
        x.rank = s + (a->size == 2 ? 5 : a->size == 1 ? 3 : 0) - km / 25.0;
        char tail[96];
        std::snprintf(tail, sizeof tail, " \xc2\xb7 %.0f km", km);
        x.why = ident + " \xe2\x80\x94 " + how + tail + (*dn.words() ? std::string(" \xc2\xb7 ") + dn.words() : std::string());
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Suspect& p, const Suspect& q) { return p.rank != q.rank ? p.rank > q.rank : p.km < q.km; });
    if (out.size() > n) out.resize(n);
    return out;
}

inline const AirfieldRec* airfieldByIcao(const std::string& icao) {
    if (icao.size() != 4) return nullptr;
    for (size_t i = 0; i < kAirfieldCount; ++i) if (icao == kAirfields[i].icao) return &kAirfields[i];
    return nullptr;
}

/** ★★ THE OWNER'S DECISION on a clue — {"type":"ndb_confirm","act":"confirm"|"reject"|"forget","ident","hz","airfield"}
 *  (the caller has already checked the owner's admin authority). The airfield's name and position come from OUR table,
 *  never from the client. "" = done; otherwise why not, in plain words. `note`: the server-log line for a confirm (the
 *  candidates for scripts/ndb-heard.json are collected from it). */
inline std::string ownerDecide(const std::string& act, const std::string& ident, double hz, const std::string& icao,
                               long long now, std::string& note) {
    if (!suspectable(ident) && !(act == "forget" && !ident.empty())) return "not an ident the clue covers";
    if (!(hz >= 150000 && hz <= 1750000)) return "not an NDB frequency";
    if (act == "forget") return ownerBeacons().forget(ident, hz) ? std::string() : "nothing was decided for it";
    const AirfieldRec* a = airfieldByIcao(icao);
    if (!a) return "unknown airfield";
    OwnerBeacons::Rec r;
    r.ident = ident; r.hz = hz; r.airfield = a->icao; r.name = kAirfieldNames + a->name; r.cc = std::strcmp(a->cc, "--") ? a->cc : "";
    r.lat = a->lat; r.lon = a->lon; r.at = now; r.rejected = act == "reject";
    if (act != "confirm" && act != "reject") return "unknown action";
    if (!ownerBeacons().decide(r)) return "the store of decisions is full";
    char b[300];
    std::snprintf(b, sizeof b, "[NDB] the owner %s %s %.1f kHz = %s %s (%.4f, %.4f)%s", r.rejected ? "REJECTED" : "CONFIRMED", ident.c_str(),
                  std::lround(hz / 500.0) * 0.5, r.name.c_str(), a->icao, a->lat, a->lon,
                  r.rejected ? "" : " \xe2\x80\x94 a candidate for scripts/ndb-heard.json");
    note = b;
    return std::string();
}

} // namespace vibendb
