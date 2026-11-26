// vibe_aircraft_delta.h — V12 beta96 THE SHARED AIRCRAFT TABLE AS DELTAS (docs/v12/ADSB-FEED.md §Feed aircraft as deltas).
//
// ★★ WHY (coordinator's brief, 11-26, after Stuart's "so people on limited or sketchy connections can host it"): every AIR & SEA
//    map — a radio's (inside the web page / app) and the standalone viewer — fetched GET /vibeserver/aircraft.json, the WHOLE
//    table, every 3 s. On a server fed by a PiAware (150 aircraft) or an HFDL feed (a few hundred), that is the biggest thing a
//    map viewer costs the owner's uplink. This sends each viewer only what changed since its cursor.
//
// ── The shape ─────────────────────────────────────────────────────────────────────────────────────────────────────────
//   A row is split in two:
//     "s" STATIC  — what an aircraft IS: icao, reg, flight, type, the aircraft data's fields, route, from/to, label, app, via;
//     "y" DYNAMIC — where it is and when it was heard: lat, lon, pt (position time, UTC s), posSrc, alt, trk, gs, ground,
//                   posTime, src, st (each source's last-heard UTC s), lt (last heard UTC s), n.
//   ★★ TIMES ARE ABSOLUTE here (UTC seconds), never ages: an aircraft heard once and then silent changes NOTHING, so it is not
//      re-sent every poll. The client turns them back into aircraft.json's ages (now − t) — so everything downstream
//      (sharedSanitise, the maps, avNowRule) reads exactly the object it always did (src/services/aircraftDelta.ts).
//   A delta carries: rows whose static part changed (s + y), rows whose dynamic part changed (y), and the keys gone. The
//   header (now, count, the ADS-B state), the HFDL ground stations and the feeds are small and come whole every time.
//
// ── Cursors, not per-client state ──────────────────────────────────────────────────────────────────────────────────────
//   ONE store per process. It is rebuilt at most once a second (kMinBuildMs) whoever asks, each row remembering the build
//   (sequence number) its static and its dynamic part last changed in, and each key gone the build it went in. A viewer's
//   cursor "<epoch>.<seq>" is simply the build it last saw: the answer is every row changed after it. Nothing is kept per
//   viewer, so 1 or 100 viewers cost the same build. A cursor from another process life (epoch) or older than the gone
//   list's memory (kGoneKeepMs) gets the whole table again ("full":1).
//   ★ Bounded: the table is ≤ kCap (1024) rows; the gone list ≤ kGoneMax keys.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "vibe_aircraft_share.h"

namespace vibeacdelta {

static constexpr int64_t kMinBuildMs = 1000;        ///< at most one build a second, however many viewers ask
static constexpr int64_t kGoneKeepMs = 10 * 60000;  ///< keys gone are remembered 10 min — a cursor older than that gets the whole table
static constexpr size_t kGoneMax = 4096;

using LabelFn = std::function<std::string(const std::string&)>;
using GsInfoFn = std::function<bool(int, std::string&, double&, double&)>;

struct Parts { std::string key, s, y; };

/** The row's key exactly as aircraft.json writes it (icao hex, "r:<reg>", "f:<flight>"). */
inline std::string rowKey(const vibeavshare::Row& w) {
    char b[64];
    if (w.icao) { std::snprintf(b, sizeof b, "%06X", w.icao); return b; }
    if (!w.reg.empty()) return "r:" + vibeavshare::regKey(w.reg);
    return "f:" + vibeavshare::flightKey(w.flight);
}

/** ★ The two halves of one row, as JSON objects' members (no braces). The SAME fields and formatting as
 *  vibeavshare::toJson — test-aircraft-delta rebuilds every row from these and compares it with toJson's, field by field. */
inline Parts rowParts(const vibeavshare::Row& w, const LabelFn& labelName = nullptr, const vibeavshare::ExtraFn& extra = nullptr,
                      const std::vector<std::string>& radioReg = {}) {
    using vibeavshare::jsonEsc;
    Parts p;
    p.key = rowKey(w);
    char b[256];
    std::string& s = p.s;
    bool sFirst = true;
    auto sStr = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        if (!sFirst) s += ','; sFirst = false;
        s += '"'; s += k; s += "\":\""; s += jsonEsc(v); s += '"';
    };
    if (w.icao) { std::snprintf(b, sizeof b, "\"icao\":\"%06X\"", w.icao); s += b; sFirst = false; }
    vibeavshare::Extra ex;
    if (extra) extra(w, ex);
    sStr("reg", w.reg.empty() ? ex.reg : w.reg); sStr("flight", w.flight); sStr("type", w.type.empty() ? ex.type : w.type);
    if (!ex.fields.empty()) {   // ★ ",\"op\":\"RYR\",…" — leading comma already there
        if (sFirst) { s += ex.fields.substr(1); sFirst = false; } else s += ex.fields;
    }
    if (w.from.empty() && !ex.route.empty()) { if (!sFirst) s += ','; sFirst = false; s += "\"route\":"; s += ex.route; }
    if (!w.from.empty()) { sStr("from", w.from); sStr("to", w.to); }
    if (!w.label.empty()) { sStr("label", w.label); if (labelName) sStr("labelName", labelName(w.label)); }
    sStr("app", w.app); sStr("via", w.via);

    std::string& y = p.y;
    const long long lt = (long long)(w.lastMs / 1000);
    if (w.flags & 1) {
        std::snprintf(b, sizeof b, "\"lat\":%.5f,\"lon\":%.5f,\"pt\":%lld", w.lat, w.lon, (long long)(w.posMs / 1000));
        y += b;
        if (w.posSrc >= 0) { y += ",\"posSrc\":\""; y += vibeavshare::srcName(w.posSrc); y += '"'; }
        if (w.flags & 2) { std::snprintf(b, sizeof b, ",\"alt\":%d", w.altFt); y += b; }
        if (w.flags & 4) { std::snprintf(b, sizeof b, ",\"trk\":%.0f", (double)w.trk); y += b; }
        if (w.flags & 8) { std::snprintf(b, sizeof b, ",\"gs\":%.0f", (double)w.gs); y += b; }
        if (w.flags & 16) y += ",\"ground\":1";
        if (!w.posTime.empty()) { y += ",\"posTime\":\""; y += jsonEsc(w.posTime); y += '"'; }
        y += ',';
    } else if (w.flags & 2) { std::snprintf(b, sizeof b, "\"alt\":%d,", w.altFt); y += b; }
    y += "\"src\":[";
    bool f1 = true;
    for (int k = 0; k < vibeavshare::kNSrc; ++k) if ((w.srcMask >> k) & 1) { y += f1 ? "\"" : ",\""; y += vibeavshare::srcName(k); y += '"'; f1 = false; }
    y += "],\"st\":{";
    f1 = true;
    for (int k = 0; k < vibeavshare::kNSrc; ++k) if ((w.srcMask >> k) & 1) {
        std::snprintf(b, sizeof b, "%s\"%s\":%lld", f1 ? "" : ",", vibeavshare::srcName(k), (long long)(w.srcMs[k] / 1000));
        y += b; f1 = false;
    }
    std::snprintf(b, sizeof b, "},\"lt\":%lld,\"n\":%u", lt, w.count);
    y += b;
    /* ★★★ beta96 (b96-mapchat × b96-sbsfeed merge): aircraft.json's "first" (as "ft", absolute like every time here — the
     *  client makes it an age) and "radios" — the maps' THIS SESSION and OTHER RADIOS switch read them; without them a
     *  delta-polling map could not scope its aircraft. Both are dynamic (a second radio hears it; first never changes). */
    std::snprintf(b, sizeof b, ",\"ft\":%lld", (long long)((w.firstMs ? w.firstMs : w.lastMs) / 1000));
    y += b;
    if (w.radios && !radioReg.empty()) {
        const std::vector<std::string> ids = vibe::logradio::idsOf(radioReg, w.radios);
        if (!ids.empty()) { y += ",\"radios\":["; for (size_t i = 0; i < ids.size(); ++i) { if (i) y += ','; y += '"'; y += ids[i]; y += '"'; } y += ']'; }
    }
    return p;
}

/** aircraft.json's header members after "now"/"count" — the ADS-B state (vibeavshare::toJson's exact text). */
inline std::string adsbHeader(const std::vector<vibeavshare::Row>& rows, int64_t nowMs, int64_t adsbLiveMs, bool adsbOff) {
    if (adsbLiveMs < 0) return std::string();
    size_t n = 0;
    for (const auto& w : rows) if (vibeavshare::adsbHeardNow(w, adsbLiveMs, nowMs)) ++n;
    char b[256];
    std::snprintf(b, sizeof b, ",\"adsbLive\":%d,\"adsbLast\":%lld,\"adsbNow\":%zu,\"adsbWin\":%lld%s",
                  vibeavshare::adsbLiveNow(adsbLiveMs, nowMs) ? 1 : 0, adsbLiveMs > 0 ? (long long)std::max<int64_t>(0, (nowMs - adsbLiveMs) / 1000) : -1LL,
                  n, (long long)vibeavshare::kAdsbNowSec, adsbOff ? ",\"adsbOff\":1" : "");
    return b;
}
/** The HFDL ground stations, as aircraft.json's "stations" array (toJson's exact text). */
inline std::string stationsJson(const std::vector<vibeavshare::GsHeard>& gs, int64_t nowMs, const GsInfoFn& gsInfo) {
    std::string o = "[";
    bool first = true;
    char b[256];
    for (const auto& g : gs) {
        std::string name; double lat = NAN, lon = NAN;
        if (!gsInfo || !gsInfo(g.id, name, lat, lon)) continue;
        std::snprintf(b, sizeof b, "%s{\"id\":%d,\"name\":\"%s\",\"lat\":%.1f,\"lon\":%.1f,\"khz\":%.0f,\"age\":%lld}", first ? "" : ",", g.id,
                      vibeavshare::jsonEsc(name).c_str(), lat, lon, g.khz, (long long)std::max<int64_t>(0, (nowMs - g.ms) / 1000));
        o += b; first = false;
    }
    return o + "]";
}

/** ★★ THE STORE — one per process (store()). */
class Store {
public:
    Store() {
        // ★ the epoch: a cursor from an earlier life of this process is never mistaken for one of ours
        const uint64_t t = (uint64_t)vibeavshare::wallMs();
        char b[24]; std::snprintf(b, sizeof b, "%x", (unsigned)((t ^ (t >> 23) ^ (uint64_t)(uintptr_t)this) & 0x7fffffff));
        epoch_ = b;
    }
    /** Is a rebuild due (the caller then reads the table and calls update)? */
    bool due(int64_t nowMs) { std::lock_guard<std::mutex> lk(m_); return seq_ == 0 || nowMs - builtMs_ >= kMinBuildMs; }
    /** Rebuild from the table's rows (every row kept — the hour). */
    void update(const std::vector<vibeavshare::Row>& rows, int64_t nowMs, const LabelFn& labelName = nullptr,
                const vibeavshare::ExtraFn& extra = nullptr, const std::vector<std::string>& radioReg = {}) {
        std::vector<Parts> ps; ps.reserve(rows.size());
        for (const auto& w : rows) ps.push_back(rowParts(w, labelName, extra, radioReg));
        std::lock_guard<std::mutex> lk(m_);
        const uint64_t q = ++seq_;
        builtMs_ = nowMs;
        std::map<std::string, bool> seen;
        for (auto& p : ps) {
            if (seen.count(p.key)) continue;    // ★ one row per key (the table's merge guarantees it; a torn read must not)
            seen[p.key] = true;
            auto it = rows_.find(p.key);
            if (it == rows_.end()) {
                Ent e; e.s = std::move(p.s); e.y = std::move(p.y); e.sv = e.yv = q; e.order = 0;
                rows_.emplace(p.key, std::move(e));
                gone_.erase(p.key);
                continue;
            }
            Ent& e = it->second;
            if (e.s != p.s) { e.s = std::move(p.s); e.sv = q; }
            if (e.y != p.y) { e.y = std::move(p.y); e.yv = q; }
        }
        for (auto it = rows_.begin(); it != rows_.end();) {
            if (seen.count(it->first)) { ++it; continue; }
            gone_[it->first] = { q, nowMs };
            it = rows_.erase(it);
        }
        // ★ the gone list's memory: older than kGoneKeepMs (or over kGoneMax) is forgotten, and a cursor before it is too old
        for (auto it = gone_.begin(); it != gone_.end();) {
            if (nowMs - it->second.ms > kGoneKeepMs) { horizon_ = std::max(horizon_, it->second.seq); it = gone_.erase(it); }
            else ++it;
        }
        while (gone_.size() > kGoneMax) {
            auto old = gone_.begin();
            for (auto it = gone_.begin(); it != gone_.end(); ++it) if (it->second.seq < old->second.seq) old = it;
            horizon_ = std::max(horizon_, old->second.seq);
            gone_.erase(old);
        }
        order_.clear();
        for (const auto& p : ps) if (rows_.count(p.key)) order_.push_back(p.key);   // ★ newest first, as the table reads
    }
    /** The delta members for `cursor` ("" or another epoch = everything): "\"d\":1,\"c\":\"…\",\"full\":0|1,\"a\":[…],\"g\":[…]".
     *  `rowsSent` (optional): how many rows went. */
    std::string body(const std::string& cursor, size_t* rowsSent = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        uint64_t c = 0;
        bool full = true;
        const size_t dot = cursor.find('.');
        if (dot != std::string::npos && cursor.compare(0, dot, epoch_) == 0) {
            c = std::strtoull(cursor.c_str() + dot + 1, nullptr, 10);
            full = !(c >= horizon_ && c <= seq_ && c > 0);
        }
        std::string o = "\"d\":1,\"c\":\"" + epoch_ + "." + std::to_string((unsigned long long)seq_) + "\",\"full\":" + (full ? "1" : "0") + ",\"a\":[";
        bool first = true;
        size_t n = 0;
        for (const auto& k : order_) {
            auto it = rows_.find(k);
            if (it == rows_.end()) continue;
            const Ent& e = it->second;
            const bool ss = full || e.sv > c, yy = full || e.yv > c;
            if (!ss && !yy) continue;
            o += first ? "{\"k\":\"" : ",{\"k\":\""; first = false;
            o += vibeavshare::jsonEsc(k); o += '"';
            if (ss) { o += ",\"s\":{"; o += e.s; o += '}'; }
            o += ",\"y\":{"; o += e.y; o += "}}";   // ★ a static change always brings its dynamic part too
            ++n;
        }
        o += "],\"g\":[";
        first = true;
        if (!full) for (const auto& g : gone_) {
            if (g.second.seq <= c) continue;
            o += first ? "\"" : ",\""; first = false;
            o += vibeavshare::jsonEsc(g.first); o += '"';
        }
        o += ']';
        if (rowsSent) *rowsSent = n;
        return o;
    }
    std::string epoch() const { return epoch_; }
    uint64_t seq() { std::lock_guard<std::mutex> lk(m_); return seq_; }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return rows_.size(); }

private:
    struct Ent { std::string s, y; uint64_t sv = 0, yv = 0; int order = 0; };
    struct Gone { uint64_t seq; int64_t ms; };
    std::mutex m_;
    std::string epoch_;
    uint64_t seq_ = 0, horizon_ = 0;
    int64_t builtMs_ = 0;
    std::map<std::string, Ent> rows_;
    std::map<std::string, Gone> gone_;
    std::vector<std::string> order_;
};
inline Store& store() { static Store s; return s; }

}  // namespace vibeacdelta
