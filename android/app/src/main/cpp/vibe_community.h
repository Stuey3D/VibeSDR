// vibe_community.h — V12 beta94 THE VibeSDR COMMUNITY MAP: what this server shares, when its owner says so.
// docs/v12/COMMUNITY-MAP.md.
//
// ★★★ STUART (2026-11-25): "right now the lenovo is getting all the navtex and ndb, the rsp1b on the pi500 is handling dsc,
//     and the v4l does adsb. I'd like a combined world map that everyone can contribute to with a simple toggle."
//
// ── THE DESIGN: PULL, DIRECT, CACHED ─────────────────────────────────────────────────────────────────────────────────
//   · ONE endpoint, GET /vibeserver/community.json, answered by any process of the server (a radio or the front door) —
//     every source below is the MACHINE's (the shared aircraft table, the gallery and its ship store), never one radio's.
//   · The community map page (directory/public/community-map.html) reads the directory's list of contributors ONCE, then
//     asks each contributor for this file DIRECTLY over its own tunnel (CORS: vibe_cors.h echoes the directory origin) —
//     never through the directory's Worker. At most once a minute per open tab, nothing while the tab is hidden.
//   · ONE BUILD PER kCacheSec, whoever asks: ten viewers cost the server what one does. Cache-Control lets each browser
//     keep it for the same time.
//
// ── WHAT IS IN IT: THE EXISTING FORMATS, NOT A NEW ONE ───────────────────────────────────────────────────────────────
//   "aircraft" — /vibeserver/aircraft.json's object (vibe_aircraft_share.h toJson): positioned rows only, heard in the
//                last kAircraftAgeSec, newest first, at most kAircraftMax. Its "stations" (HFDL ground stations) kept.
//   "ships"    — /vibeserver/gallery/ais-ships.json's object (vibe_ais_store.h toJson): heard in kShipsSinceSec, no tracks.
//   "dsc"      — /vibeserver/gallery.json?kind=dsc's object (the gallery index lines; "plat"/"plon" pin a position).
//   "navtex"   — /vibeserver/gallery/navtex-stations.json's object (vibe_navtex_map.h toJson) with rx = null: no receiver
//                position, no distances from it.
//   "ndb"      — {"now","log":[…]}: the live NDB report's log entry shape (web/client/src/ndb.ts NdbLog) read from the
//                gallery's day cards (vibe_ndb_gallery.h parseCard), WITHOUT km/brg (they would give the receiver away).
//   "layers"   — the modes present: adsb acars vdl2 hfdl ais dsc navtex ndb — a layer with nothing in it is not named
//                (no dead controls on the map: the page draws a filter only for what somebody shares).
//
// ── PRIVACY ───────────────────────────────────────────────────────────────────────────────────────────────────────────
//   ★★★ OFF BY DEFAULT, the owner's switch (config "communityMap"). Off → 404 and nothing else; vibeserver.json says
//       nothing either, so the directory never lists the server as a contributor.
//   ★★ A PIN-LOCKED server shares nothing (its decodes are behind the PIN; this endpoint must not walk round it).
//   ★★ A PRIVATE GALLERY ("galleryPublic" off) shares nothing that comes from the gallery — ships, DSC, NAVTEX, NDB —
//      only aircraft, as /vibeserver/aircraft.json already does for anyone.
//   ★★ A mode the owner switched off (vsModeBlocked) is left out, as each endpoint above already does.
//   ★ What is shared is what the stations broadcast — aircraft and ships' own positions, NAVTEX stations heard, DSC
//     positions, NDB idents (placed from the published list) — and never the receiver's own position: the page draws the
//     receiver from the directory listing's grid square, cut to four characters (≈ 100 × 200 km).
//
// ── BOUNDS (NO OOM, the 256 MB boxes) ─────────────────────────────────────────────────────────────────────────────────
//   Each section has its own row cap and age cap (below), and assemble() drops any section that would take the whole
//   answer past kMaxBytes, naming it in "dropped". One body in RAM per process (the cache), at most kMaxBytes.
#pragma once
#include "vibe_aircraft_share.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vibecommunity {

static constexpr int64_t kCacheSec = 30;                 ///< one build per 30 s per process, whoever asks
static constexpr size_t  kMaxBytes = 640 * 1024;          ///< the whole answer, at most
static constexpr size_t  kAircraftMax = 600;
static constexpr int64_t kAircraftAgeSec = 900;          ///< a position older than 15 min is not "now" for anybody
static constexpr size_t  kShipsMax = 400;
static constexpr int64_t kShipsSinceSec = 6 * 3600;      ///< the shipping map's own SHIP_KEEP_SEC
static constexpr size_t  kDscMax = 150;
static constexpr int64_t kDscSinceSec = 7 * 86400;       ///< the shipping map's DSC_ALERT_KEEP_SEC (the page applies the day rule)
static constexpr int64_t kNavtexSinceSec = 7 * 86400;
static constexpr size_t  kNdbMax = 300;
static constexpr int64_t kNdbSinceSec = 86400;

/** The layer names, in the order the page lists them. */
inline const std::vector<std::string>& layerOrder() {
    static const std::vector<std::string> v = { "adsb", "acars", "vdl2", "hfdl", "ais", "dsc", "navtex", "ndb" };
    return v;
}

/** One part of the answer: its key, its JSON value (an object or array, already built) and the layers it carries. */
struct Section {
    std::string key, json;
    std::vector<std::string> layers;
};

inline std::string jsonEsc(const std::string& s) {
    std::string o; o.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

/** ★★ The answer: {"v":1,"on":true,"now":N,"layers":[…],<sections…>,"dropped":[…]}. Sections are taken in the order given;
 *  one that would push the answer past `maxBytes` is left out and named in "dropped" (its layers with it). An empty
 *  section json is skipped silently. `layersOut` receives the layers actually present, in layerOrder(). */
inline std::string assemble(int64_t now, const std::vector<Section>& in, size_t maxBytes = kMaxBytes,
                            std::vector<std::string>* layersOut = nullptr) {
    std::string body, dropped;
    std::vector<std::string> have;
    size_t budget = maxBytes > 512 ? maxBytes - 512 : 0;   // ★ the envelope's own few hundred bytes
    for (const Section& s : in) {
        if (s.json.empty() || s.key.empty()) continue;
        const size_t cost = s.key.size() + s.json.size() + 4;
        if (cost > budget) { if (!dropped.empty()) dropped += ','; dropped += "\"" + jsonEsc(s.key) + "\""; continue; }
        budget -= cost;
        body += ",\"" + jsonEsc(s.key) + "\":" + s.json;
        for (const auto& l : s.layers) if (std::find(have.begin(), have.end(), l) == have.end()) have.push_back(l);
    }
    std::vector<std::string> ordered;
    for (const auto& l : layerOrder()) if (std::find(have.begin(), have.end(), l) != have.end()) ordered.push_back(l);
    std::string o = "{\"v\":1,\"on\":true,\"now\":" + std::to_string((long long)now) + ",\"layers\":[";
    for (size_t i = 0; i < ordered.size(); i++) o += (i ? ",\"" : "\"") + ordered[i] + "\"";
    o += "]" + body;
    if (!dropped.empty()) o += ",\"dropped\":[" + dropped + "]";
    o += "}";
    if (layersOut) *layersOut = ordered;
    return o;
}

/** The answer when the server shares nothing: why, in a word ("off", "pin"). */
inline std::string offJson(const std::string& why) {
    return "{\"v\":1,\"on\":false,\"why\":\"" + jsonEsc(why) + "\"}";
}

/** ★ The aircraft to share: positioned, position no older than kAircraftAgeSec, newest first, at most kAircraftMax. */
inline std::vector<vibeavshare::Row> aircraftRows(std::vector<vibeavshare::Row> rows, int64_t nowMs,
                                                   size_t maxRows = kAircraftMax, int64_t ageSec = kAircraftAgeSec) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const vibeavshare::Row& w) {
        return !(w.flags & 1) || !std::isfinite(w.lat) || !std::isfinite(w.lon) || nowMs - w.posMs > ageSec * 1000;
    }), rows.end());
    std::sort(rows.begin(), rows.end(), [](const vibeavshare::Row& a, const vibeavshare::Row& b) { return a.posMs > b.posMs; });
    if (rows.size() > maxRows) rows.resize(maxRows);
    return rows;
}

/** The aviation layers those rows carry (ADS-C rides one of the bearers, so it names none of its own). */
inline std::vector<std::string> aircraftLayers(const std::vector<vibeavshare::Row>& rows) {
    uint32_t m = 0;
    for (const auto& w : rows) m |= w.srcMask;
    std::vector<std::string> out;
    if (m & (1u << vibeavshare::kAdsb)) out.push_back("adsb");
    if (m & (1u << vibeavshare::kAcars)) out.push_back("acars");
    if (m & (1u << vibeavshare::kVdl2)) out.push_back("vdl2");
    if (m & (1u << vibeavshare::kHfdl)) out.push_back("hfdl");
    return out;
}

/** One NDB beacon for the map — the NdbLog shape (web/client/src/ndb.ts) less km / brg. */
struct NdbRow {
    std::string ident, name, cc;
    double hz = 0, lat = NAN, lon = NAN;
    float best = 0;
    int64_t first = 0, last = 0;
};
/** {"now":N,"log":[…]} — placed beacons heard within `sinceSec`, newest first, at most `maxRows`; "" when there are none. */
inline std::string ndbJson(std::vector<NdbRow> rows, int64_t now, size_t maxRows = kNdbMax, int64_t sinceSec = kNdbSinceSec) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const NdbRow& r) {
        return r.ident.empty() || !std::isfinite(r.lat) || !std::isfinite(r.lon) || now - r.last > sinceSec;
    }), rows.end());
    std::sort(rows.begin(), rows.end(), [](const NdbRow& a, const NdbRow& b) { return a.last > b.last; });
    // ★ one per ident + frequency (the cards of two days name the same beacon): the newest
    std::vector<NdbRow> uniq;
    for (const auto& r : rows) {
        bool dup = false;
        for (const auto& u : uniq) if (u.ident == r.ident && std::fabs(u.hz - r.hz) < 500) { dup = true; break; }
        if (!dup) uniq.push_back(r);
        if (uniq.size() >= maxRows) break;
    }
    if (uniq.empty()) return "";
    std::string o = "{\"now\":" + std::to_string((long long)now) + ",\"log\":[";
    char b[256];
    for (size_t i = 0; i < uniq.size(); i++) {
        const NdbRow& r = uniq[i];
        std::snprintf(b, sizeof b, "%s{\"ident\":\"%s\",\"hz\":%.0f,\"lat\":%.4f,\"lon\":%.4f,\"best\":%.0f,\"first\":%lld,\"last\":%lld",
                      i ? "," : "", jsonEsc(r.ident).c_str(), r.hz, r.lat, r.lon, (double)r.best, (long long)r.first, (long long)r.last);
        o += b;
        o += ",\"name\":\"" + jsonEsc(r.name.substr(0, 80)) + "\",\"cc\":\"" + jsonEsc(r.cc.substr(0, 4)) + "\"}";
    }
    return o + "]}";
}

/** FNV-1a 64 of the body, hex — the ETag. */
inline std::string etag(const std::string& body) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : body) { h ^= c; h *= 1099511628211ULL; }
    char b[24]; std::snprintf(b, sizeof b, "\"%016llx\"", (unsigned long long)h);
    return b;
}

/** ★★ ONE BUILD PER kCacheSec, WHOEVER ASKS. get() returns the cached body while it is younger than `ttlSec`, else builds
 *  (under the lock — a second caller waits for the first build rather than starting another) and keeps it.
 *  `layers` is what the last build carried (vibeserver.json names them). */
class Cache {
public:
    struct Out { std::string body, tag; std::vector<std::string> layers; int64_t at = 0; };
    Out get(int64_t now, int64_t ttlSec, const std::function<std::string(std::vector<std::string>&)>& build) {
        std::lock_guard<std::mutex> lk(m_);
        if (!(have_ && now - c_.at < ttlSec && now >= c_.at)) {
            std::vector<std::string> layers;
            c_.body = build(layers);
            c_.tag = etag(c_.body);
            c_.layers = std::move(layers);
            c_.at = now;
            have_ = true;
            ++builds_;
        }
        return c_;
    }
    void clear() { std::lock_guard<std::mutex> lk(m_); have_ = false; c_ = Out{}; }
    unsigned builds() const { std::lock_guard<std::mutex> lk(m_); return builds_; }
private:
    mutable std::mutex m_;
    Out c_;
    bool have_ = false;
    unsigned builds_ = 0;
};

}  // namespace vibecommunity
