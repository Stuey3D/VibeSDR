// vibe_ndb_db.h — what a heard NDB IS: the published beacons (vibe_ndb_navaids.h — OurAirports and the FAA, public
// domain, and OpenStreetMap, ODbL; scripts/gen-ndb-navaids.mjs merges them), matched
// by frequency AND ident, with the distance and bearing from the receiver. docs/v12/NDB-SKIMMER.md.
//
// ★★ THE MATCH, in order (several beacons share every frequency — "four on freq" — so frequency alone names nothing):
//    1. ✓ "match"  — the CONFIRMED ident is published within ±kTolHz of the carrier: that beacon (the nearest, if the
//                    same ident is published twice on the frequency).
//    2. ≈ "near"   — a published ident on the frequency differs from the confirmed one in ONE letter (same length):
//                    the nearest such. A beacon read clearly but not as published (a changed ident, a list typo).
//    3. italic "candidate" — no ident yet (or none matches): the published beacons on the frequency ranked by distance,
//                    the nearest first — a GUESS, labelled as one, never shown as the ident.
//    A confirmed ident with no published beacon on the frequency stays "unlisted" (the list is incomplete — e.g. the
//    UK's EME / EMW are heard but in no source we may bundle): the ident is shown, the station is blank.
// ★ The receiver's own position comes from the server's location (g_bgLat/g_bgLon, the owner's setting).
// ★ Never on the DSP thread: the service calls this from its own thread, once a second at most per beacon.
#pragma once
#include "vibe_ndb_navaids.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vibendb {

/** ±Hz a carrier may sit from its published frequency: the list's frequencies are whole (or half) kHz, and a radio's
 *  clock may be 20 ppm out (8 Hz at 400 kHz) — 600 Hz takes the half-kHz raster without reaching the next beacon. */
constexpr double kTolHz = 600;

struct Navaid {
    const NavaidRec* r = nullptr;
    std::string ident() const { return r ? std::string(r->ident) : std::string(); }
    std::string name() const { return r ? std::string(kNavaidNames + r->name) : std::string(); }
    std::string cc() const { return r && std::strcmp(r->cc, "--") ? std::string(r->cc) : std::string(); }
    double hz() const { return r ? r->hz : 0; }
    double lat() const { return r ? r->lat : 0; }
    double lon() const { return r ? r->lon : 0; }
    /** Where this beacon's position came from — the credit for it (kNavaidSourceNames). */
    std::string source() const { return r && r->src < sizeof kNavaidSourceNames / sizeof kNavaidSourceNames[0] ? std::string(kNavaidSourceNames[r->src]) : std::string(); }
};

/** Great-circle distance (km, haversine on a 6371 km sphere) and initial bearing (° true) from (la1, lo1) to (la2, lo2). */
inline void distBearing(double la1, double lo1, double la2, double lo2, double& km, double& deg) {
    const double r = M_PI / 180.0;
    const double p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r, dp = p2 - p1;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    km = 2 * 6371.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1 - a)));
    const double y = std::sin(dl) * std::cos(p2), x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    deg = std::fmod(std::atan2(y, x) / r + 360.0, 360.0);
}

/** The published beacons within ±tol of hz (the table is sorted by frequency). */
inline std::vector<Navaid> onFrequency(double hz, double tol = kTolHz) {
    std::vector<Navaid> out;
    const NavaidRec* b = kNavaids; const NavaidRec* e = kNavaids + kNavaidCount;
    const NavaidRec* lo = std::lower_bound(b, e, hz - tol, [](const NavaidRec& r, double v) { return r.hz < v; });
    for (const NavaidRec* p = lo; p < e && p->hz <= hz + tol; ++p) out.push_back(Navaid{ p });
    return out;
}

/** How a heard beacon matches the list. */
struct Match {
    enum Kind { None, Exact, Near, Candidate } kind = None;
    Navaid n;
    double km = -1, deg = 0;   // from the receiver (km < 0: no receiver position)
    std::vector<Navaid> others;   // the rest on the frequency, nearest first (the "four on freq" list, at most 6)
};

/** One letter different, same length (≈). */
inline bool oneOff(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.size() < 2) return false;
    int d = 0; for (size_t i = 0; i < a.size(); ++i) d += a[i] != b[i];
    return d == 1;
}

inline Match match(double hz, const std::string& ident, bool havePos, double lat, double lon) {
    Match m;
    std::vector<Navaid> on = onFrequency(hz);
    if (on.empty()) return m;
    std::vector<std::pair<double, Navaid>> ranked;
    for (auto& n : on) {
        double km = 0, deg = 0;
        if (havePos) distBearing(lat, lon, n.lat(), n.lon(), km, deg);
        // ★ Without a receiver position, the nearest in FREQUENCY ranks first.
        ranked.push_back({ havePos ? km : std::fabs(n.hz() - hz), n });
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const Navaid* pick = nullptr; Match::Kind kind = Match::None;
    if (!ident.empty()) {
        for (auto& p : ranked) if (p.second.ident() == ident) { pick = &p.second; kind = Match::Exact; break; }
        if (!pick) for (auto& p : ranked) if (oneOff(p.second.ident(), ident)) { pick = &p.second; kind = Match::Near; break; }
    } else {
        pick = &ranked.front().second; kind = Match::Candidate;
    }
    if (pick) {
        m.kind = kind; m.n = *pick;
        if (havePos) distBearing(lat, lon, pick->lat(), pick->lon(), m.km, m.deg);
    }
    for (auto& p : ranked) { if (pick && p.second.r == pick->r) continue; if (m.others.size() < 6) m.others.push_back(p.second); }
    return m;
}

/** Every published beacon, nearest the receiver first (the "Find NDB" list): at most `limit`, optionally only those
 *  whose ident or name contains `q` (case-insensitive) or whose frequency is within 1 kHz of a number in it. */
struct Found { Navaid n; double km, deg; };
inline std::vector<Found> nearest(double lat, double lon, size_t limit, const std::string& q = std::string()) {
    std::string uq; for (char c : q) uq += (char)std::toupper((unsigned char)c);
    double qkhz = 0; { char* end = nullptr; qkhz = std::strtod(q.c_str(), &end); if (!end || end == q.c_str()) qkhz = 0; }
    std::vector<Found> v; v.reserve(kNavaidCount);
    for (size_t i = 0; i < kNavaidCount; ++i) {
        const NavaidRec& r = kNavaids[i];
        if (!uq.empty()) {
            bool ok = qkhz > 0 && std::fabs(r.hz / 1000.0 - qkhz) <= 1.0;
            if (!ok && std::strstr(r.ident, uq.c_str())) ok = true;
            if (!ok) { std::string n(kNavaidNames + r.name); for (auto& c : n) c = (char)std::toupper((unsigned char)c); ok = n.find(uq) != std::string::npos; }
            if (!ok) continue;
        }
        Found f{ Navaid{ &r }, 0, 0 };
        distBearing(lat, lon, r.lat, r.lon, f.km, f.deg);
        v.push_back(f);
    }
    const size_t k = std::min(limit, v.size());
    std::partial_sort(v.begin(), v.begin() + (long)k, v.end(), [](const Found& a, const Found& b) { return a.km < b.km; });
    v.resize(k);
    return v;
}

/** Compass point (16-wind) for a bearing. */
inline const char* compass(double deg) {
    static const char* k[16] = { "N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW" };
    return k[(int)std::lround(std::fmod(deg + 360.0, 360.0) / 22.5) % 16];
}

} // namespace vibendb
