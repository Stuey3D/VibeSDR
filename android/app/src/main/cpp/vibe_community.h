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
//                last kAircraftAgeSec (ADS-B) / kAircraftDatalinkAgeSec (datalink, beta96), newest first, at most kAircraftMax. Its "stations" (HFDL ground stations) kept.
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
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace vibecommunity {

static constexpr int64_t kCacheSec = 30;                 ///< one build per 30 s per process, whoever asks
static constexpr size_t  kMaxBytes = 640 * 1024;          ///< the whole answer, at most
static constexpr size_t  kAircraftMax = 600;
static constexpr int64_t kAircraftAgeSec = 300;          ///< ★ live: an ADS-B position older than 5 min is not "now" (Stuart 11-25)
/** ★★ beta96 (Stuart 11-26: "hold them for 30 is fine faded to show older"): a position that came by DATALINK (ACARS, VDL2,
 *  HFDL, ADS-C — posSrc not ADS-B) is reported minutes apart, often once an oceanic waypoint, so 5 min dropped nearly all
 *  of them. They are kept 30 min; the row's position age (aircraftCompact field 3) lets the map fade them by age. */
static constexpr int64_t kAircraftDatalinkAgeSec = 1800;
static constexpr size_t  kShipsMax = 300;
static constexpr int64_t kShipsSinceSec = 6 * 3600;      ///< the shipping map's own SHIP_KEEP_SEC
static constexpr size_t  kDscMax = 150;
static constexpr int64_t kDscSinceSec = 86400;           ///< ★ 24 h (Stuart 11-25: "optimise everything")
static constexpr int64_t kNavtexSinceSec = 86400;         ///< ★ NAVTEX: the last 24 h only (Stuart 11-25)
static constexpr size_t  kNdbMax = 300;
static constexpr int64_t kNdbSinceSec = 86400;

/** The layer names, in the order the page lists them. */
inline const std::vector<std::string>& layerOrder() {
    // ★ beta94 pass 2 (Stuart 11-25: "anything that we map now should be able to be contributed … sondes etc too" —
    //   "amateur spots excluded"): AVIATION adsb acars vdl2 hfdl ndb vor · MARITIME ais dsc navtex · MISC sondes aprs mesh
    static const std::vector<std::string> v = { "adsb", "acars", "vdl2", "hfdl", "ndb", "vor", "ais", "dsc", "navtex", "sondes", "aprs", "mesh" };
    return v;
}

inline std::set<std::string> offSet(const std::string& csv);   // ★ below

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
    std::string o = "{\"v\":2,\"on\":true,\"now\":" + std::to_string((long long)now) + ",\"layers\":[";
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

/** ★ A position from a datalink (not ADS-B)? An unknown source counts as ADS-B — the shorter hold. */
inline bool aircraftPosDatalink(const vibeavshare::Row& w) { return w.posSrc > vibeavshare::kAdsb && w.posSrc < vibeavshare::kNSrc; }
/** ★ The aircraft to share: positioned, position no older than kAircraftAgeSec (ADS-B) or kAircraftDatalinkAgeSec (a
 *  datalink position), newest first, at most kAircraftMax. */
inline std::vector<vibeavshare::Row> aircraftRows(std::vector<vibeavshare::Row> rows, int64_t nowMs,
                                                   size_t maxRows = kAircraftMax, int64_t ageSec = kAircraftAgeSec,
                                                   int64_t datalinkAgeSec = kAircraftDatalinkAgeSec) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const vibeavshare::Row& w) {
        const int64_t keep = aircraftPosDatalink(w) ? datalinkAgeSec : ageSec;
        return !(w.flags & 1) || !std::isfinite(w.lat) || !std::isfinite(w.lon) || nowMs - w.posMs > keep * 1000;
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
/** ★ Compact NDB rows — [ident, hz, lat, lon, last, name, cc] — placed beacons heard within `sinceSec`, newest first, one
 *  per ident + frequency, at most `maxRows`; "" when there are none. No distance or bearing from the receiver. */
inline std::string ndbRows(std::vector<NdbRow> rows, int64_t now, size_t maxRows = kNdbMax, int64_t sinceSec = kNdbSinceSec) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const NdbRow& r) {
        return r.ident.empty() || !std::isfinite(r.lat) || !std::isfinite(r.lon) || now - r.last > sinceSec;
    }), rows.end());
    std::sort(rows.begin(), rows.end(), [](const NdbRow& a, const NdbRow& b) { return a.last > b.last; });
    std::vector<NdbRow> uniq;
    for (const auto& r : rows) {
        bool dup = false;
        for (const auto& u : uniq) if (u.ident == r.ident && std::fabs(u.hz - r.hz) < 500) { dup = true; break; }
        if (!dup) uniq.push_back(r);
        if (uniq.size() >= maxRows) break;
    }
    std::string o;
    char b[160];
    for (size_t i = 0; i < uniq.size(); i++) {
        const NdbRow& r = uniq[i];
        std::snprintf(b, sizeof b, "%s[\"%s\",%.0f,%.4f,%.4f,%lld,\"", i ? "," : "", jsonEsc(r.ident).c_str(), r.hz, r.lat, r.lon, (long long)r.last);
        o += b;
        o += jsonEsc(r.name.substr(0, 60)) + "\",\"" + jsonEsc(r.cc.substr(0, 3)) + "\"]";
    }
    return o;
}

/** ★ Compact aircraft rows — [key, lat, lon, position age s, alt ft|null, track|null, speed kt|null, sources (bit mask: 1 ADS-B,
 *  2 ACARS, 4 VDL2, 8 HFDL, 16 ADS-C), flight, reg, type]. The rows as aircraftRows() chose them. */
inline std::string aircraftCompact(const std::vector<vibeavshare::Row>& rows, int64_t nowMs) {
    std::string o;
    char b[200];
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& w = rows[i];
        char key[40];
        if (w.icao) std::snprintf(key, sizeof key, "%06X", w.icao);
        else std::snprintf(key, sizeof key, "%s", jsonEsc(!w.reg.empty() ? "r:" + w.reg : "f:" + w.flight).substr(0, 30).c_str());
        auto opt = [](bool has, double v, char* out, size_t n) { if (has) std::snprintf(out, n, "%.0f", v); else std::snprintf(out, n, "null"); };
        char alt[16], trk[16], gs[16];
        opt(w.flags & 2, w.altFt, alt, sizeof alt); opt(w.flags & 4, w.trk, trk, sizeof trk); opt(w.flags & 8, w.gs, gs, sizeof gs);
        std::snprintf(b, sizeof b, "%s[\"%s\",%.4f,%.4f,%lld,%s,%s,%s,%u,\"", i ? "," : "", key, w.lat, w.lon,
                      (long long)std::max<int64_t>(0, (nowMs - w.posMs) / 1000), alt, trk, gs, (unsigned)w.srcMask);
        o += b;
        o += jsonEsc(w.flight.substr(0, 10)) + "\",\"" + jsonEsc(w.reg.substr(0, 10)) + "\",\"" + jsonEsc(w.type.substr(0, 8)) + "\"]";
    }
    return o;
}

/** Round to 4 decimals (≈ 11 m) as text. */
inline std::string c4(double v) { char b[24]; std::snprintf(b, sizeof b, "%.4f", v); return b; }

/** Which sections a layer needs (the ?s= parameter speaks layers). */
inline std::vector<std::string> sectionsOfLayer(const std::string& l) {
    if (l == "adsb" || l == "acars" || l == "vdl2" || l == "hfdl") return { "a" };
    if (l == "ais") return { "s" };
    if (l == "dsc") return { "d" };
    if (l == "navtex") return { "ns", "nw", "nm", "nx" };
    if (l == "ndb") return { "b" };
    if (l == "vor") return { "o" };
    if (l == "sondes") return { "z" };
    if (l == "aprs") return { "p" };
    if (l == "mesh") return { "m" };
    return {};
}
/** The sections a request asks for: ?s=adsb,ais,… (layer words); absent or empty = every section. Sorted, unique. */
inline std::vector<std::string> sectionsOfQuery(const std::string& csv) {
    std::set<std::string> out;
    const auto ls = offSet(csv);   // ★ the same word list (unknown words dropped)
    if (ls.empty()) for (const auto& l : layerOrder()) for (const auto& x : sectionsOfLayer(l)) out.insert(x);
    for (const auto& l : ls) for (const auto& x : sectionsOfLayer(l)) out.insert(x);
    return std::vector<std::string>(out.begin(), out.end());
}
/** ★ How long a built section is good for: the live ones 30 s, the slow ones (NAVTEX, NDB, VOR, sondes) 120 s. */
inline int64_t sectionTtl(const std::string& k) {
    return (k == "a" || k == "s" || k == "d" || k == "p" || k == "m") ? 30 : 120;
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


// ══ beta94 pass 2 — the owner's per-mode tick boxes, the live (in-process) sections, MeshCore, NAVTEX messages ══════════

/** ★ The owner's "leave this mode out" list (config communityOff, comma-separated layer words). Unknown words ignored. */
inline std::set<std::string> offSet(const std::string& csv) {
    std::set<std::string> out;
    std::string w;
    for (size_t i = 0; i <= csv.size(); ++i) {
        const char c = i < csv.size() ? csv[i] : ',';
        if (c == ',' || c == ' ' || c == ';') {
            if (std::find(layerOrder().begin(), layerOrder().end(), w) != layerOrder().end()) out.insert(w);
            w.clear();
        } else if (w.size() < 12) w += (char)std::tolower((unsigned char)c);
    }
    return out;
}

static constexpr size_t kSondesMax = 32, kSondePts = 120;
static constexpr int64_t kSondesSinceSec = 12 * 3600;  // ★ the current and the last flight (Stuart 11-25)
static constexpr size_t kAprsMax = 300;
static constexpr int64_t kAprsSinceSec = 6 * 3600;
static constexpr size_t kMeshMax = 300;
static constexpr int64_t kMeshSinceSec = 86400;
static constexpr size_t kVorMax = 200;
static constexpr size_t kNavWarnMax = 300;
static constexpr int64_t kNavWarnSinceSec = 86400;
static constexpr int64_t kNavMetSinceSec = 86400;
static constexpr size_t kNavMsgMax = 300;
static constexpr int64_t kNavMsgSinceSec = 86400;
static constexpr size_t kNavMsgText = 1600;              ///< one message's text, at most (a real one: 560 B mean, 3.2 KB max)
static constexpr size_t kNavMsgBytes = 220 * 1024;       ///< the messages section, at most (a real Lenovo day: ~150 KB)

/* ── THE REMEMBERED SECTIONS: sondes, APRS and the VOR map are decoded inside ONE radio PROCESS (a whole-radio mode, a
 *    decoder run) and have no store of their own in the gallery. So each radio keeps what it last decoded as one small file
 *    per section beside the gallery (<gallery>/.community/) — written only when it changed and at most every
 *    kLiveWriteSec, never per message (an SD card) — and whichever process answers community.json joins them. So a
 *    server shows its recent sondes and APRS stations after a restart, and while the mode is not running, within each
 *    section's window (the rows carry their own times). A file is comma-joined compact ROWS ("[…],[…]"), ≤ kLiveFileMax;
 *    one untouched for kLiveDropSec is removed. No gallery: this process's own memory alone. ── */
static constexpr size_t kLiveFileMax = 192 * 1024;
static constexpr int64_t kLiveWriteSec = 120;
static constexpr int64_t kLiveDropSec = 2 * 86400;
inline std::string liveTag(const std::string& raw) {
    std::string t;
    for (char c : raw) if (std::isalnum((unsigned char)c) || c == '-' || c == '_') t += c;
    return t.empty() ? std::string("radio") : t.substr(0, 40);
}
inline std::string liveName(const std::string& section, const std::string& tag) { return "live-" + section + "-" + liveTag(tag) + ".json"; }
/** Write (an empty body: remove) this radio's file for `section` in `dir`. Atomic: a temporary file renamed over it. */
inline bool writeLive(const std::string& dir, const std::string& section, const std::string& tag, const std::string& body) {
    if (dir.empty()) return false;
    ::mkdir(dir.c_str(), 0755);
    const std::string path = dir + "/" + liveName(section, tag);
    if (body.empty()) { ::unlink(path.c_str()); return true; }
    if (body.size() > kLiveFileMax) return false;
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < body.size()) { const ssize_t n = ::write(fd, body.data() + done, body.size() - done); if (n <= 0) break; done += (size_t)n; }
    ::close(fd);
    if (done != body.size() || ::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
}
/** Every radio's rows for `section`, comma-joined — at most `maxBytes` (whole files only). */
inline std::string readLive(const std::string& dir, const std::string& section, int64_t nowSec, size_t maxBytes = 2 * kLiveFileMax) {
    std::string out;
    if (dir.empty()) return out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    const std::string pre = "live-" + section + "-";
    int files = 0;
    while (struct dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.rfind(pre, 0) != 0 || n.size() < 5 || n.compare(n.size() - 5, 5, ".json") != 0 || ++files > 32) continue;
        const std::string path = dir + "/" + n;
        struct stat st{};
        if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > kLiveFileMax) continue;
        if (nowSec - (int64_t)st.st_mtime > kLiveDropSec) { ::unlink(path.c_str()); continue; }
        if (out.size() + (size_t)st.st_size + 1 > maxBytes) continue;
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        std::string b((size_t)st.st_size, '\0');
        size_t got = 0;
        while (got < b.size()) { const ssize_t k = ::read(fd, &b[got], b.size() - got); if (k <= 0) break; got += (size_t)k; }
        ::close(fd);
        b.resize(got);
        if (b.size() < 2 || b.front() != '[' || b.back() != ']') continue;   // ★ an array of rows: a torn file is skipped
        b = b.substr(1, b.size() - 2);                                        // ★ its rows, comma-joined
        if (b.empty()) continue;
        if (!out.empty()) out += ',';
        out += b;
    }
    ::closedir(d);
    return out;
}

/** ★ Keep the rows (comma-joined top-level arrays) whose field `idx` (a number: a UTC time) is ≥ `minT`, at most `maxRows`.
 *  A tiny scanner, strings and nesting respected — the rows are ours, but a torn one is simply dropped. */
inline std::string rowsSince(const std::string& rows, int idx, double minT, size_t maxRows) {
    std::string out;
    size_t n = 0, i = 0;
    while (i < rows.size() && n < maxRows) {
        while (i < rows.size() && rows[i] != '[') ++i;
        if (i >= rows.size()) break;
        const size_t start = i;
        int depth = 0, field = 0; bool str = false; size_t fieldAt = std::string::npos;
        for (; i < rows.size(); ++i) {
            const char c = rows[i];
            if (str) { if (c == '\\') ++i; else if (c == '"') str = false; continue; }
            if (c == '"') { str = true; continue; }
            if (c == '[') { if (++depth == 1 && field == idx) fieldAt = i + 1; continue; }
            if (c == ']') { if (--depth == 0) { ++i; break; } continue; }
            if (c == ',' && depth == 1 && ++field == idx) fieldAt = i + 1;
        }
        if (depth != 0 || fieldAt == std::string::npos) continue;
        const double t = std::atof(rows.c_str() + fieldAt);
        if (t >= minT) { if (n++) out += ','; out.append(rows, start, i - start); }
    }
    return out;
}

/** ★ Is `name` in the owner's comma/space/semicolon list of blocked modes? (Case-insensitive — vsModeBlocked's rule.) */
inline bool modeCsvHas(const std::string& csv, const std::string& name) {
    std::string want, tok;
    for (char c : name) want += char(std::tolower((unsigned char)c));
    for (size_t i = 0; i <= csv.size(); ++i) {
        const char c = i < csv.size() ? csv[i] : ',';
        if (c == ',' || c == ' ' || c == ';') { if (!tok.empty() && tok == want) return true; tok.clear(); }
        else tok += char(std::tolower((unsigned char)c));
    }
    return false;
}
/** ★ May a radio whose blocked modes are `blocked` share community layer `l`? The modes behind each layer. */
inline bool layerAllowed(const std::string& l, const std::function<bool(const char*)>& blocked) {
    if (l == "adsb") return !blocked("adsb");
    if (l == "acars" || l == "vdl2") return !blocked("avdata");
    if (l == "hfdl") return !blocked("hfdl");
    if (l == "ais") return !blocked("ais") && !blocked("aisstore");
    if (l == "vor") return !blocked("vormap") && !blocked("vor");
    if (l == "mesh") return !blocked("ism");
    return !blocked(l.c_str());
}
/** ★★ beta95 THE FRONT DOOR'S ANSWER IS THE WHOLE MACHINE'S: a layer is shared when ANY of the machine's radios may share it
 *  (each radio's own blocked modes — the door, which owns no radio, used to judge by the first radio's alone, so a
 *  multi-radio server's door answered only what that one radio allowed: Stuart's Pi 500, the RSP1B, DSC only). */
inline bool layerAllowedAnyRadio(const std::string& l, const std::vector<std::string>& radiosBlocked) {
    for (const auto& csv : radiosBlocked)
        if (layerAllowed(l, [&](const char* m) { return modeCsvHas(csv, m); })) return true;
    return false;
}

/** ★★ beta95 ONE MERGED FEED PER MACHINE: the remembered rows of every radio (and a process's own, held in memory AND in its
 *  file) can name the same sonde / APRS station / VOR twice. One row per key (field `keyIdx`, compared as written), the
 *  newest by field `timeIdx`, newest first, at most `maxRows`. Pure; the input is already bounded (readLive). */
inline std::string rowsDedupe(const std::string& rows, int keyIdx, int timeIdx, size_t maxRows) {
    struct R { std::string key; double t; size_t a, b; };
    std::vector<R> v;
    size_t i = 0;
    while (i < rows.size() && v.size() < 4096) {
        while (i < rows.size() && rows[i] != '[') ++i;
        if (i >= rows.size()) break;
        const size_t start = i;
        int depth = 0, field = 0; bool str = false;
        size_t keyAt = std::string::npos, keyEnd = std::string::npos, tAt = std::string::npos;
        for (; i < rows.size(); ++i) {
            const char c = rows[i];
            if (str) { if (c == '\\') ++i; else if (c == '"') str = false; continue; }
            if (c == '"') { str = true; continue; }
            if (c == '[') { if (++depth == 1) { if (field == keyIdx) keyAt = i + 1; if (field == timeIdx) tAt = i + 1; } continue; }
            if (c == ']') { if (--depth == 0) { if (field == keyIdx && keyEnd == std::string::npos) keyEnd = i; ++i; break; } continue; }
            if (c == ',' && depth == 1) {
                if (field == keyIdx) keyEnd = i;
                ++field;
                if (field == keyIdx) keyAt = i + 1;
                if (field == timeIdx) tAt = i + 1;
            }
        }
        if (depth != 0 || keyAt == std::string::npos || keyEnd == std::string::npos || tAt == std::string::npos) continue;
        v.push_back({ rows.substr(keyAt, keyEnd - keyAt), std::atof(rows.c_str() + tAt), start, i });
    }
    std::stable_sort(v.begin(), v.end(), [](const R& x, const R& y) { return x.t > y.t; });
    std::set<std::string> seen;
    std::string out;
    size_t n = 0;
    for (const auto& r : v) {
        if (n >= maxRows) break;
        if (!seen.insert(r.key).second) continue;
        if (n++) out += ',';
        out.append(rows, r.a, r.b - r.a);
    }
    return out;
}

/* ── MESH (the ISM map's MeshCore nodes, Stuart 11-25: "ism is only showing meshcore stations anyway") — from the gallery's
 *    "ismnodes-<band>-<date>" cards (vibe_ism_gallery.h writeNodes; the same 12 fields its restoreNodes reads). ONLY
 *    MeshCore nodes, which put their own position in a public advert; never any other ISM device, never the card's
 *    "Receiver:" line. A companion (a person) is rounded to 0.01° (~1 km), as every reader of the card does. ── */
struct MeshNode {
    std::string node, name, type;
    double lat = NAN, lon = NAN, mhz = 0; float snr = 0;
    int hops = -1; bool approx = false;
    int64_t first = 0, last = 0; uint64_t count = 0;
};
inline std::vector<MeshNode> meshNodesOfCard(const std::string& body, int64_t day) {
    std::vector<MeshNode> out;
    static const std::string dot = " \xc2\xb7 ";
    const size_t at = body.find("Each node, most recent first");
    if (at == std::string::npos) return out;
    auto hms = [&](const std::string& s, int64_t& o) { int h, m, sec; if (s.size() != 8 || std::sscanf(s.c_str(), "%2d:%2d:%2d", &h, &m, &sec) != 3) return false; o = day * 86400 + h * 3600 + m * 60 + sec; return true; };
    size_t p = body.find('\n', at);
    while (p != std::string::npos && p + 1 < body.size() && out.size() < 2000) {
        const size_t e = body.find('\n', p + 1);
        const std::string row = body.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
        p = e;
        std::vector<std::string> f; size_t q = 0;
        for (;;) { const size_t d = row.find(dot, q); if (d == std::string::npos) { f.push_back(row.substr(q)); break; } f.push_back(row.substr(q, d - q)); q = d + dot.size(); }
        if (f.size() != 12 || f[4] != "MeshCore") continue;
        MeshNode n;
        if (!hms(f[0], n.last) || !hms(f[1], n.first)) continue;
        n.count = std::strtoull(f[2].c_str(), nullptr, 10);
        n.type = f[5]; n.name = f[6].substr(0, 40); n.node = f[7] == "-" ? std::string() : f[7].substr(0, 16);
        n.approx = f[8].find("approx") != std::string::npos;
        if (!n.count || std::sscanf(f[8].c_str(), "%lf, %lf", &n.lat, &n.lon) != 2 || !(std::fabs(n.lat) <= 90) || !(std::fabs(n.lon) <= 180)) continue;
        if (n.type == "companion") { n.lat = std::round(n.lat * 100) / 100; n.lon = std::round(n.lon * 100) / 100; n.approx = true; }
        n.hops = f[9] == "-" ? -1 : std::atoi(f[9].c_str());
        n.snr = float(std::atof(f[10].c_str())); n.mhz = std::atof(f[11].c_str());
        out.push_back(std::move(n));
    }
    return out;
}
/** ★ Compact MeshCore rows — [id, name, type, lat, lon, last, times heard, approx 0|1] — one per node id (else name), the
 *  newest, heard within `sinceSec`, at most `maxRows`. */
inline std::string meshRows(std::vector<MeshNode> v, int64_t now, size_t maxRows = kMeshMax, int64_t sinceSec = kMeshSinceSec) {
    std::sort(v.begin(), v.end(), [](const MeshNode& a, const MeshNode& b) { return a.last > b.last; });
    std::set<std::string> seen;
    std::string o;
    char b[160];
    size_t n = 0;
    for (const auto& x : v) {
        if (now - x.last > sinceSec || x.last - now > 3600) continue;
        if (!seen.insert(x.node.empty() ? "n:" + x.name : x.node).second) continue;
        o += std::string(n ? "," : "") + "[\"" + jsonEsc(x.node) + "\",\"" + jsonEsc(x.name) + "\",\"" + jsonEsc(x.type) + "\"";
        std::snprintf(b, sizeof b, ",%.4f,%.4f,%lld,%llu,%d]", x.lat, x.lon, (long long)x.last, (unsigned long long)x.count, x.approx ? 1 : 0);
        o += b;
        if (++n >= maxRows) break;
    }
    return o;
}

/* ── NAVTEX MESSAGES (Stuart 11-25: "the community map needs all the maritime details from the navtex map we have") — every
 *    message heard in kNavMsgSinceSec, ONE per message id (B1 B2 B3B4, "GA98"), the CLEANEST copy (fewest lost characters,
 *    then the newest) as its text, with how many times it was heard; plus any message a shared warning or forecast names
 *    (its best copy), whatever its age. Text capped at kNavMsgText, the section at kNavMsgBytes. ── */
struct NavMsg {
    std::string id, file, title, text, st, sc, iso;
    int64_t first = 0, last = 0, issued = 0;
    int lost = 0; uint32_t n = 0; double khz = 0; bool pinned = false;
    /** ★★ beta96 (Stuart 11-26): the copy's DECODE QUALITY from its gallery record — so the community map can pick the best
     *  of several receivers' copies ("I hear it with a large red part in the middle, NickB hears it all green → use
     *  NickB's"). hasQ: clean / FEC / lost % (whole); hasSnr: mean SNR, the lowest slice (dB, whole) and the % of its
     *  characters decoded under 5 dB. Older records have neither — nothing is sent for them. Never the slices (size). */
    bool hasQ = false, hasSnr = false;
    int qClean = 0, qFec = 0, qLost = 0, snr = 0, snrMin = 0, redPct = 0;
};
/** ★ beta96: the quality fields of a gallery index line, when it has them. */
inline void navQualityFrom(NavMsg& m, const std::string& line, double (*num)(const std::string&, const char*)) {
    if (line.find("\"nqc\":") != std::string::npos) {
        const double c = num(line, "nqc"), f = num(line, "nqf"), l = num(line, "nql"), n = c + f + l;
        if (n > 0) { m.hasQ = true; m.qClean = (int)std::lround(100 * c / n); m.qFec = (int)std::lround(100 * f / n); m.qLost = (int)std::lround(100 * l / n); }
    }
    if (line.find("\"snr\":") != std::string::npos && line.find("\"red\":") != std::string::npos) {
        m.hasSnr = true; m.snr = (int)std::lround(num(line, "snr")); m.snrMin = (int)std::lround(num(line, "smin"));
        m.redPct = (int)std::lround(num(line, "red"));
    }
}
/** ★ beta96: is copy `a` better than `b`? With both measured: the fewest characters under 5 dB, then the most clean, then
 *  the highest lowest slice, then the highest mean SNR — the community map's order. Otherwise, as before: the fewest
 *  lost characters ('_' / '*'). Ties: the newer. */
inline bool navBetter(const NavMsg& a, const NavMsg& b) {
    if (a.hasSnr && b.hasSnr && a.hasQ && b.hasQ) {
        if (a.redPct != b.redPct) return a.redPct < b.redPct;
        if (a.qClean != b.qClean) return a.qClean > b.qClean;
        if (a.snrMin != b.snrMin) return a.snrMin > b.snrMin;
        if (a.snr != b.snr) return a.snr > b.snr;
        return a.last > b.last;
    }
    return a.lost < b.lost || (a.lost == b.lost && a.last > b.last);
}
/** "GA98" from "ZCZC GA98", or "". */
inline std::string navMsgId(const std::string& text) {
    const size_t z = text.find("ZCZC");
    if (z == std::string::npos) return "";
    size_t i = z + 4;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i + 4 > text.size()) return "";
    const std::string id = text.substr(i, 4);
    if (!std::isupper((unsigned char)id[0]) || !std::isupper((unsigned char)id[1]) || !std::isdigit((unsigned char)id[2]) || !std::isdigit((unsigned char)id[3])) return "";
    return id;
}
inline int navLost(const std::string& b) { int n = 0; for (char c : b) n += c == '*' || c == '_'; return n; }
/** Fold copies into messages: one per id (an id-less copy is its own), the cleanest copy's text. */
inline std::vector<NavMsg> navFold(const std::vector<NavMsg>& copies) {
    std::vector<NavMsg> out;
    std::map<std::string, size_t> at;
    for (const auto& c : copies) {
        const std::string k = c.id.empty() ? "f:" + c.file : c.id + "@" + std::to_string((long long)std::lround(c.khz));
        auto it = at.find(k);
        if (it == at.end()) { at[k] = out.size(); out.push_back(c); out.back().n = 1; continue; }
        NavMsg& m = out[it->second];
        ++m.n;
        m.first = std::min(m.first, c.first); m.pinned = m.pinned || c.pinned;
        const int64_t last = std::max(m.last, c.last);
        if (navBetter(c, m)) {   // ★ the cleanest copy, then the newest (beta96: by its measured quality when both have it)
            const uint32_t n = m.n; const int64_t first = m.first; const bool pin = m.pinned;
            m = c; m.n = n; m.first = first; m.pinned = pin;
        }
        m.last = last;
    }
    return out;
}
/** ★ The message's TITLE: its first line after the ZCZC header, ≤ 80 characters (the text is fetched when opened). */
inline std::string navTitle(const std::string& text) {
    size_t p = text.find("ZCZC");
    p = p == std::string::npos ? 0 : text.find('\n', p);
    p = p == std::string::npos ? 0 : p + 1;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t a = 0; while (a < l.size() && l[a] == ' ') ++a;
        l = l.substr(a);
        if (!l.empty() && l != "NNNN") {
            if (l.size() > 80) { l.resize(80); while (!l.empty() && ((unsigned char)l.back() & 0xC0) == 0x80) l.pop_back(); if (!l.empty() && ((unsigned char)l.back() & 0xC0) == 0xC0) l.pop_back(); }
            return l;
        }
        p = e + 1;
    }
    return std::string();
}
/** ★ Compact NAVTEX messages — [id, kHz, first, last, times heard, lost characters, station, conf, iso, file, title] —
 *  newest first, pinned ones (named by a shared warning or forecast) first in line, at most `maxRows` / `maxBytes`.
 *  ★★ beta96: + [clean %, FEC %, lost %] when the copy has the FEC split, then + [SNR dB, lowest slice dB, % weak] when it
 *  has the SNR too — 14 or 17 members; an older record's row stays 11 (nothing invented). */
inline std::string navMsgRows(std::vector<NavMsg> v, size_t maxRows = kNavMsgMax, size_t maxBytes = kNavMsgBytes) {
    std::stable_sort(v.begin(), v.end(), [](const NavMsg& a, const NavMsg& b) { return a.pinned != b.pinned ? a.pinned : a.last > b.last; });
    std::string o;
    size_t n = 0;
    char b[160];
    for (const auto& m : v) {
        if (n >= maxRows) break;
        std::snprintf(b, sizeof b, ",%g,%lld,%lld,%u,%d,\"", m.khz, (long long)m.first, (long long)m.last, m.n, m.lost);
        std::string q;
        if (m.hasQ) {
            char qb[96]; std::snprintf(qb, sizeof qb, ",%d,%d,%d", m.qClean, m.qFec, m.qLost); q = qb;
            if (m.hasSnr) { std::snprintf(qb, sizeof qb, ",%d,%d,%d", m.snr, m.snrMin, m.redPct); q += qb; }
        }
        const std::string one = "[\"" + jsonEsc(m.id) + "\"" + b + jsonEsc(m.st) + "\",\"" + jsonEsc(m.sc) + "\",\"" + jsonEsc(m.iso) + "\",\""
                              + jsonEsc(m.file) + "\",\"" + jsonEsc(navTitle(m.text)) + "\"" + q + "]";
        if (o.size() + one.size() + 2 > maxBytes) break;
        if (n) o += ',';
        o += one; ++n;
    }
    return o;
}
/** ★ A JSON object text without one string field (",\"key\":\"…\"" removed, escapes respected) — a forecast's text out of
 *  the poll (it is fetched when opened). */
inline std::string dropStrField(std::string j, const std::string& key) {
    const std::string k = ",\"" + key + "\":\"";
    for (size_t p; (p = j.find(k)) != std::string::npos;) {
        size_t i = p + k.size();
        while (i < j.size() && j[i] != '"') i += j[i] == '\\' ? 2 : 1;
        j.erase(p, std::min(j.size(), i + 1) - p);
    }
    return j;
}
}  // namespace vibecommunity
