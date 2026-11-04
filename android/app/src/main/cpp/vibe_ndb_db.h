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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace vibendb {

/** ±Hz a carrier may sit from its published frequency: the list's frequencies are whole (or half) kHz, and a radio's
 *  clock may be 20 ppm out (8 Hz at 400 kHz) — 600 Hz takes the half-kHz raster without reaching the next beacon. */
constexpr double kTolHz = 600;

/** NavaidRec::src of a beacon this server's OWNER confirmed from the "Suspected …" clue (vibe_ndb_suspect.h): in no
 *  published list, so not in kNavaidSourceNames. */
constexpr uint8_t kSrcOwner = 0xF0;
constexpr const char* kOwnerSource = "confirmed by this server's owner";

struct Navaid {
    const NavaidRec* r = nullptr;
    const char* nm = nullptr;   // ★ an owner-confirmed beacon's name (its record is not in kNavaids: no blob offset)
    std::string ident() const { return r ? std::string(r->ident) : std::string(); }
    std::string name() const { return nm ? std::string(nm) : r ? std::string(kNavaidNames + r->name) : std::string(); }
    bool owner() const { return r && r->src == kSrcOwner; }
    std::string cc() const { return r && std::strcmp(r->cc, "--") ? std::string(r->cc) : std::string(); }
    double hz() const { return r ? r->hz : 0; }
    double lat() const { return r ? r->lat : 0; }
    double lon() const { return r ? r->lon : 0; }
    /** Where this beacon's position came from — the credit for it (kNavaidSourceNames). */
    std::string source() const { if (owner()) return kOwnerSource; return r && r->src < sizeof kNavaidSourceNames / sizeof kNavaidSourceNames[0] ? std::string(kNavaidSourceNames[r->src]) : std::string(); }
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

/** ★★ V12 2026-11-04 — THE OWNER'S DECISIONS on the "Suspected …" clue, per server: ndb-confirmed.json in the data
 *  directory, one JSON object per line, written whole to a .tmp and renamed (atomic). A CONFIRMED beacon is from then on
 *  an Exact match on THIS server — named, positioned, counted — its source "confirmed by this server's owner". A
 *  REJECTED airfield is never suggested again for that ident@frequency. Bounded: kMax live records of each kind (a
 *  full store refuses more). Records are never freed while the process runs (a forgotten one is only marked dead), so
 *  a Navaid pointing into them stays valid on any thread. */
class OwnerBeacons {
public:
    static constexpr size_t kMax = 500;
    struct Rec { std::string ident, airfield, name, cc; double hz = 0, lat = 0, lon = 0; long long at = 0; bool rejected = false, alive = true; };
    static std::string key(const std::string& ident, double hz) { return ident + "@" + std::to_string((long long)std::lround(hz / 500.0)); }
    /** Where the decisions live (read now). The same file again is a no-op; another drops what was held. */
    void setFile(const std::string& f) {
        std::lock_guard<std::mutex> lk(m_);
        if (f == file_) return;
        for (auto& r : recs_) r.alive = false;
        file_ = f; loadLocked(); ++gen_;
    }
    /** Test seam: forget everything in memory and stop writing (the file is untouched). */
    void resetForTest() { std::lock_guard<std::mutex> lk(m_); file_.clear(); for (auto& r : recs_) r.alive = false; ++gen_; }
    unsigned gen() const { std::lock_guard<std::mutex> lk(m_); return gen_; }
    /** Confirm (r.rejected false) or reject an airfield for ident@hz. A confirm replaces every earlier decision for the
     *  key; a reject replaces a confirm of that airfield. false: bad input, or the store is full. */
    bool decide(Rec r) {
        if (r.ident.empty() || r.ident.size() > 5 || r.hz <= 0) return false;
        const std::string k = key(r.ident, r.hz);
        std::lock_guard<std::mutex> lk(m_);
        for (auto& x : recs_) if (x.alive && key(x.ident, x.hz) == k && (!r.rejected || x.airfield == r.airfield)) x.alive = false;
        size_t live = 0; for (const auto& x : recs_) live += x.alive && x.rejected == r.rejected;
        if (live >= kMax) return false;
        r.hz = (double)std::lround(r.hz / 500.0) * 500.0; r.alive = true;
        push(r); ++gen_;
        saveLocked();
        return true;
    }
    /** Undo every decision for ident@hz. */
    bool forget(const std::string& ident, double hz) {
        const std::string k = key(ident, hz);
        std::lock_guard<std::mutex> lk(m_);
        bool any = false;
        for (auto& x : recs_) if (x.alive && key(x.ident, x.hz) == k) { x.alive = false; any = true; }
        if (any) { ++gen_; saveLocked(); }
        return any;
    }
    bool rejected(const std::string& k, const std::string& airfield) const {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& x : recs_) if (x.alive && x.rejected && x.airfield == airfield && key(x.ident, x.hz) == k) return true;
        return false;
    }
    /** The confirmed beacons within ±tol of hz, appended to out. */
    void onFrequency(double hz, double tol, std::vector<Navaid>& out) const {
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < recs_.size(); ++i) {
            const Rec& x = recs_[i];
            if (x.alive && !x.rejected && std::fabs(x.hz - hz) <= tol) out.push_back(Navaid{ &navs_[i], names_[i].c_str() });
        }
    }
    /** {"confirmed":[…],"rejected":[…]} */
    std::string json() const {
        std::lock_guard<std::mutex> lk(m_);
        std::string c, j;
        for (const auto& x : recs_) {
            if (!x.alive) continue;
            std::string& o = x.rejected ? j : c;
            if (!o.empty()) o += ',';
            o += line(x);
        }
        return "{\"confirmed\":[" + c + "],\"rejected\":[" + j + "]}";
    }

private:
    static std::string esc(const std::string& s) {
        std::string o;
        for (unsigned char ch : s) { if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; } else if (ch >= 0x20) o += (char)ch; }
        return o;
    }
    static std::string line(const Rec& x) {
        char b[480];
        std::snprintf(b, sizeof b, "{\"ident\":\"%s\",\"hz\":%.0f,\"airfield\":\"%s\",\"name\":\"%s\",\"cc\":\"%s\",\"lat\":%.4f,\"lon\":%.4f,\"at\":%lld,\"rejected\":%s}",
                      esc(x.ident).c_str(), x.hz, esc(x.airfield).c_str(), esc(x.name).c_str(), esc(x.cc).c_str(), x.lat, x.lon, x.at, x.rejected ? "true" : "false");
        return b;
    }
    static std::string str(const std::string& l, const char* k) {
        const std::string pat = std::string("\"") + k + "\":\"";
        size_t p = l.find(pat);
        if (p == std::string::npos) return std::string();
        std::string o;
        for (p += pat.size(); p < l.size() && l[p] != '"'; ++p) { if (l[p] == '\\' && p + 1 < l.size()) ++p; o += l[p]; }
        return o;
    }
    static double num(const std::string& l, const char* k) {
        const std::string pat = std::string("\"") + k + "\":";
        const size_t p = l.find(pat);
        return p == std::string::npos ? 0 : std::atof(l.c_str() + p + pat.size());
    }
    void push(const Rec& r) {
        recs_.push_back(r);
        NavaidRec n{}; n.hz = (uint32_t)r.hz; n.lat = (float)r.lat; n.lon = (float)r.lon; n.name = 0; n.src = kSrcOwner;
        std::snprintf(n.ident, sizeof n.ident, "%s", r.ident.c_str());
        std::snprintf(n.cc, sizeof n.cc, "%s", r.cc.size() == 2 ? r.cc.c_str() : "--");
        navs_.push_back(n); names_.push_back(r.name.substr(0, 60));
    }
    void loadLocked() {
        if (file_.empty()) return;
        FILE* f = std::fopen(file_.c_str(), "r");
        if (!f) return;
        char buf[640]; size_t n = 0;
        while (std::fgets(buf, sizeof buf, f) && n < 2 * kMax) {
            const std::string l(buf);
            if (l.find("\"ident\"") == std::string::npos) continue;
            Rec r; r.ident = str(l, "ident").substr(0, 5); r.hz = num(l, "hz"); r.airfield = str(l, "airfield").substr(0, 8);
            r.name = str(l, "name"); r.cc = str(l, "cc").substr(0, 2);
            r.lat = num(l, "lat"); r.lon = num(l, "lon"); r.at = (long long)num(l, "at"); r.rejected = l.find("\"rejected\":true") != std::string::npos;
            if (r.ident.empty() || r.hz <= 0) continue;
            push(r); ++n;
        }
        std::fclose(f);
        ++gen_;
    }
    void saveLocked() const {
        if (file_.empty()) return;
        const std::string tmp = file_ + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fputs("{\"about\":\"NDBs this server's owner confirmed or rejected from the Suspected clue (vibe_ndb_db.h)\",\"records\":[\n", f);
        bool first = true;
        for (const auto& x : recs_) if (x.alive) { if (!first) std::fputs(",\n", f); std::fputs(line(x).c_str(), f); first = false; }
        std::fputs("\n]}\n", f);
        const bool ok = std::fflush(f) == 0;
        std::fclose(f);
        if (ok) std::rename(tmp.c_str(), file_.c_str()); else std::remove(tmp.c_str());
    }
    mutable std::mutex m_;
    std::string file_;
    std::deque<Rec> recs_;
    std::deque<NavaidRec> navs_;     // ★ never erased: a Navaid may point here from any thread
    std::deque<std::string> names_;
    unsigned gen_ = 0;
};
inline OwnerBeacons& ownerBeacons() { static OwnerBeacons s; return s; }

/** The published beacons within ±tol of hz (the table is sorted by frequency) — and the owner's confirmed ones. */
inline std::vector<Navaid> onFrequency(double hz, double tol = kTolHz) {
    std::vector<Navaid> out;
    const NavaidRec* b = kNavaids; const NavaidRec* e = kNavaids + kNavaidCount;
    const NavaidRec* lo = std::lower_bound(b, e, hz - tol, [](const NavaidRec& r, double v) { return r.hz < v; });
    for (const NavaidRec* p = lo; p < e && p->hz <= hz + tol; ++p) out.push_back(Navaid{ p });
    ownerBeacons().onFrequency(hz, tol, out);
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
