// vibe_ais_store.h — V12 2026-11-02 THE SHIP STORE: the ships AIS heard (latest state per MMSI, a short recent track),
// kept on disk beside the gallery so the SHIPPING MAP can draw them with nobody listening. docs/v12/SHIPPING-MAP.md.
//
// ★★★ WHY (Stuart, 2026-11-02): "a user with a multi SDR server could have one set for Navtex and another for AIS and or
//     DSC and then you will see all ships and forecasts etc on one map". AIS is a whole-radio mode: its ship list lives
//     in the AIS radio's process and goes when the mode ends. The map is served by ANY radio on the machine — so the
//     ships have to be where every radio can read them: the SHARED gallery directory, under the gallery's own lock.
//
// ── The file: <gallery>/ais-ships.dat ─────────────────────────────────────────────────────────────────────────────
//   header (64 B) · directory (cap × 8 B: mmsi, last heard) · records (used × 512 B, fixed size)
//   ★★ BOUNDED BY CONSTRUCTION: at most `cap` ships (≤ kMaxCap = 2048, and never more than a 32nd of the owner's
//      gallery space — 1 MB at the default 100 MB), each with at most kTrack track points. Full: the ship heard longest
//      ago gives its slot to the newcomer. The file only grows as slots are used.
//   ★★ COUNTED INSIDE THE OWNER'S GALLERY SIZE: vibe_gallery.h Store::sideBytes() adds this file to what the gallery
//      holds, so the items evicted to make room include the room this file takes.
//   ★★ WRITTEN INCREMENTALLY: the AIS service hands over only the ships that changed since its last flush (every 30 s
//      and when AIS stops); each is ONE pread + ONE pwrite of its 512 B record, plus the directory. Never the whole
//      file, never per message — a Pi 2's SD card is the target.
//   ★★ EVERY RADIO ON THE MACHINE: each change is under flock(<gallery>/.lock) — the same kernel lock the gallery's
//      index uses — and reads the directory FRESH under it, so two radios in AIS (two aerials, two coasts) merge into one
//      store: a ship heard by both keeps the newest position, and the track points of both, in time order.
//   ★ NO RAM TABLE: nothing is held between flushes but the AIS service's own (already bounded) list. A read for the
//     map streams the records it needs, one at a time.
// ── Read: query() → the map's JSON (the live AIS report's vessel shape, so every client draws it the same way) ──────
//   ?since=<seconds back> · ?range=<km from the receiver> · ?tracks=1 · ?limit= (newest first, at most 2048)
#pragma once
#include "vibe_ais_service.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vibeais {
namespace store {

static constexpr const char* kFile = "ais-ships.dat";
static constexpr uint32_t kMagic = 0x31534941;   // "AIS1"
static constexpr uint32_t kRecSize = 512;
static constexpr uint32_t kHeader = 64;
static constexpr uint32_t kMaxCap = 2048;
static constexpr uint32_t kMinCap = 32;
static constexpr int kTrack = 32;                 ///< track points a ship keeps (a point per 2 min or 300 m — about an hour)
static constexpr double kTrackMinSec = 120, kTrackMinM = 300;
static constexpr int32_t kNoPos = INT32_MIN;

/** The slots a gallery of `limitBytes` affords: a 32nd of the space, at most kMaxCap, at least kMinCap. */
inline uint32_t capFor(uint64_t limitBytes) {
    const uint64_t c = limitBytes / 32 / kRecSize;
    return (uint32_t)std::max<uint64_t>(kMinCap, std::min<uint64_t>(kMaxCap, c));
}
inline uint64_t fileBytesFor(uint32_t cap, uint32_t used) { return kHeader + (uint64_t)cap * 8 + (uint64_t)used * kRecSize; }

struct TrkPt { uint32_t t; int32_t lat, lon; };
/** ★ ONE SHIP, 512 bytes on disk. Times are UTC seconds; positions 1e-7 degree; kNoPos = none. */
struct Rec {
    uint32_t mmsi = 0, seen = 0, posAt = 0, first = 0;
    int32_t lat = kNoPos, lon = kNoPos;
    uint16_t sog10 = 0xFFFF, cog10 = 0xFFFF;
    int16_t hdg = -1, draught10 = -1;
    uint16_t len = 0, beam = 0;
    int16_t stype = -1, nav = -1;
    int16_t aton = -1; uint8_t kind = 0, flags = 0;       ///< kind: 0 A · 1 B · 2 base · 3 aton · 4 sar · 5 sart; flags 1 virtual · 2 off position
    uint16_t count = 0; uint8_t nTrk = 0, ch = 0;
    uint32_t imo = 0;
    int16_t alt = -1, spare = 0;
    char name[24] = {}, call[8] = {}, dest[24] = {}, eta[12] = {};
    TrkPt trk[kTrack] = {};                                ///< oldest first
};
static_assert(sizeof(Rec) <= kRecSize, "a ship record is 512 bytes");

inline const char* kindName(uint8_t k) { static const char* n[6] = { "A", "B", "base", "aton", "sar", "sart" }; return k < 6 ? n[k] : "A"; }
inline uint8_t kindCode(const std::string& k) {
    if (k == "B") return 1; if (k == "base") return 2; if (k == "aton") return 3; if (k == "sar") return 4; if (k == "sart") return 5; return 0;
}
inline void putStr(char* dst, size_t n, const std::string& s) { std::memset(dst, 0, n); std::memcpy(dst, s.data(), std::min(n - 1, s.size())); }
inline std::string getStr(const char* src, size_t n) { size_t k = 0; while (k < n && src[k]) ++k; return std::string(src, k); }
inline int32_t e7(double d) { return d == d ? (int32_t)std::lround(d * 1e7) : kNoPos; }
inline double de7(int32_t v) { return v == kNoPos ? NAN : v / 1e7; }
inline double metres(const TrkPt& a, double lat, double lon) {
    const double la = de7(a.lat), lo = de7(a.lon);
    const double dy = (lat - la) * 111320.0, dx = (lon - lo) * 111320.0 * std::cos(lat * M_PI / 180);
    return std::sqrt(dx * dx + dy * dy);
}

/** ★ A ship from the AIS service → a record: the state, and the track points (UTC) it has newer than `trackAfter`. */
inline Rec fromVessel(const Vessel& v, double svcNow, int64_t wallNow) {
    Rec r;
    auto wall = [&](double t) { return (uint32_t)std::max<int64_t>(0, wallNow - (int64_t)std::llround(svcNow - t)); };
    r.mmsi = v.mmsi; r.seen = wall(v.last); r.first = wall(v.first); r.posAt = v.posAt >= 0 ? wall(v.posAt) : 0;
    r.lat = e7(v.lat); r.lon = e7(v.lon);
    if (v.sog == v.sog) r.sog10 = (uint16_t)std::min(65534L, std::lround(v.sog * 10));
    if (v.cog == v.cog) r.cog10 = (uint16_t)std::min(3600L, std::lround(v.cog * 10));
    r.hdg = (int16_t)v.heading; r.draught10 = v.draught == v.draught ? (int16_t)std::lround(v.draught * 10) : -1;
    if (v.bow >= 0 && v.bow + v.stern > 0) { r.len = (uint16_t)(v.bow + v.stern); r.beam = (uint16_t)std::max(0, v.port + v.stbd); }
    r.stype = (int16_t)v.shipType; r.nav = (int16_t)v.nav; r.aton = (int16_t)v.atonType;
    r.kind = kindCode(v.kind); r.flags = (v.virt ? 1 : 0) | (v.offPos ? 2 : 0);
    r.count = (uint16_t)std::min(65535, v.count); r.ch = (uint8_t)v.ch; r.imo = v.imo; r.alt = (int16_t)std::min(32767, v.altM);
    putStr(r.name, sizeof r.name, v.name); putStr(r.call, sizeof r.call, v.callsign); putStr(r.dest, sizeof r.dest, v.dest);
    if (v.etaMonth > 0 && v.etaDay > 0) {
        char b[24]; std::snprintf(b, sizeof b, "%02d-%02d %02d:%02d", v.etaMonth, v.etaDay, v.etaHour < 24 ? v.etaHour : 0, v.etaMin < 60 ? v.etaMin : 0);
        putStr(r.eta, sizeof r.eta, b);
    }
    // ★ the track, thinned to a point per 2 min or 300 m, newest kTrack
    std::vector<TrkPt> pts;
    for (const auto& p : v.trail) {
        const TrkPt q{ wall(p.t), e7(p.lat), e7(p.lon) };
        if (!pts.empty() && q.t - pts.back().t < kTrackMinSec && metres(pts.back(), p.lat, p.lon) < kTrackMinM) continue;
        pts.push_back(q);
    }
    if (pts.size() > (size_t)kTrack) pts.erase(pts.begin(), pts.end() - kTrack);
    r.nTrk = (uint8_t)pts.size();
    for (size_t i = 0; i < pts.size(); ++i) r.trk[i] = pts[i];
    return r;
}
/** ★ A record back → a Vessel (for the readable line and the JSON every client already draws). */
inline Vessel toVessel(const Rec& r) {
    Vessel v;
    v.mmsi = r.mmsi; v.kind = kindName(r.kind); v.name = getStr(r.name, sizeof r.name); v.callsign = getStr(r.call, sizeof r.call);
    v.dest = getStr(r.dest, sizeof r.dest); v.imo = r.imo; v.shipType = r.stype; v.nav = r.nav; v.heading = r.hdg; v.atonType = r.aton; v.altM = r.alt;
    v.lat = de7(r.lat); v.lon = de7(r.lon);
    v.sog = r.sog10 == 0xFFFF ? NAN : r.sog10 / 10.0; v.cog = r.cog10 == 0xFFFF ? NAN : r.cog10 / 10.0;
    v.draught = r.draught10 < 0 ? NAN : r.draught10 / 10.0;
    if (r.len > 0) { v.bow = r.len; v.stern = 0; v.port = r.beam; v.stbd = 0; }
    v.virt = r.flags & 1; v.offPos = r.flags & 2; v.count = r.count; v.ch = r.ch;
    return v;
}
/** ★ Merge `in` (newer from one radio) into `have` (on disk, perhaps another radio's): the newest state wins field by
 *  field where `in` knows it; the tracks of both, in time order, newest kTrack. */
inline void merge(Rec& have, const Rec& in) {
    const bool newer = in.seen >= have.seen;
    Rec out = newer ? in : have;
    const Rec& old = newer ? have : in;
    // ★ static fields the newer report does not carry (a class A's name arrives every 6 min): keep the older's
    if (!out.name[0]) std::memcpy(out.name, old.name, sizeof out.name);
    if (!out.call[0]) std::memcpy(out.call, old.call, sizeof out.call);
    if (!out.dest[0]) std::memcpy(out.dest, old.dest, sizeof out.dest);
    if (!out.eta[0]) std::memcpy(out.eta, old.eta, sizeof out.eta);
    if (!out.imo) out.imo = old.imo;
    if (out.stype < 0) out.stype = old.stype;
    if (!out.len) { out.len = old.len; out.beam = old.beam; }
    if (out.draught10 < 0) out.draught10 = old.draught10;
    if (out.lat == kNoPos || (old.posAt > out.posAt && old.lat != kNoPos)) {
        out.lat = old.lat; out.lon = old.lon; out.posAt = old.posAt; out.sog10 = old.sog10; out.cog10 = old.cog10; out.hdg = old.hdg;
    }
    if (old.first && (!out.first || old.first < out.first)) out.first = old.first;
    out.count = (uint16_t)std::min<uint32_t>(65535, (uint32_t)std::max(have.count, in.count));
    // tracks: union, time order, a point within 20 s of another dropped (the same fix through two radios)
    std::vector<TrkPt> pts;
    for (int i = 0; i < have.nTrk && i < kTrack; ++i) pts.push_back(have.trk[i]);
    for (int i = 0; i < in.nTrk && i < kTrack; ++i) pts.push_back(in.trk[i]);
    std::sort(pts.begin(), pts.end(), [](const TrkPt& a, const TrkPt& b) { return a.t < b.t; });
    std::vector<TrkPt> u;
    for (const auto& p : pts) {
        if (!u.empty() && p.t <= u.back().t + 20) continue;   // the same fix (two radios, or two flushes)
        if (!u.empty() && p.t - u.back().t < kTrackMinSec && metres(u.back(), de7(p.lat), de7(p.lon)) < kTrackMinM) continue;
        u.push_back(p);
    }
    if (u.size() > (size_t)kTrack) u.erase(u.begin(), u.end() - kTrack);
    out.nTrk = (uint8_t)u.size();
    std::memset(out.trk, 0, sizeof out.trk);
    for (size_t i = 0; i < u.size(); ++i) out.trk[i] = u[i];
    have = out;
}

struct Header { uint32_t magic = kMagic, version = 1, cap = 0, used = 0, recSize = kRecSize; uint32_t pad[11] = {}; };
static_assert(sizeof(Header) == kHeader, "the header is 64 bytes");
struct DirEnt { uint32_t mmsi, seen; };

/** The machine-wide gallery lock (vibe_gallery.h Store::Lock's twin — the same file, so the same kernel lock). */
struct Lock {
    int fd = -1;
    Lock(const std::string& dir, bool shared) {
        fd = ::open((dir + "/.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) ::flock(fd, shared ? LOCK_SH : LOCK_EX);
    }
    ~Lock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
};
inline bool preadAll(int fd, void* p, size_t n, off_t at) {
    char* c = (char*)p; size_t got = 0;
    while (got < n) { const ssize_t k = ::pread(fd, c + got, n - got, at + (off_t)got); if (k <= 0) return false; got += (size_t)k; }
    return true;
}
inline bool pwriteAll(int fd, const void* p, size_t n, off_t at) {
    const char* c = (const char*)p; size_t put = 0;
    while (put < n) { const ssize_t k = ::pwrite(fd, c + put, n - put, at + (off_t)put); if (k <= 0) return false; put += (size_t)k; }
    return true;
}
inline off_t recAt(uint32_t cap, uint32_t slot) { return (off_t)(kHeader + (uint64_t)cap * 8 + (uint64_t)slot * kRecSize); }

/** Bytes the store takes on disk now (0 when absent) — the gallery's side bytes. */
inline uint64_t bytesOnDisk(const std::string& dir) {
    struct stat st{};
    return !dir.empty() && ::stat((dir + "/" + kFile).c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

/** ★★ WRITE: the ships that changed, merged into the store under the machine-wide lock. `limitBytes` is the owner's
 *  gallery size (0 = the gallery keeps nothing: nothing is written, and an old store is removed). @return records written. */
inline size_t write(const std::string& dir, uint64_t limitBytes, const std::vector<Rec>& in) {
    if (dir.empty()) return 0;
    const std::string path = dir + "/" + kFile;
    Lock lk(dir, false);
    if (limitBytes == 0) { ::unlink(path.c_str()); return 0; }
    if (in.empty()) return 0;
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return 0;
    Header h;
    struct stat st{};
    const bool fresh = ::fstat(fd, &st) != 0 || (uint64_t)st.st_size < kHeader || !preadAll(fd, &h, sizeof h, 0) || h.magic != kMagic
                    || h.recSize != kRecSize || h.cap < kMinCap || h.cap > kMaxCap || h.used > h.cap
                    || (uint64_t)st.st_size < fileBytesFor(h.cap, h.used);
    // ★ the owner shrank the gallery below what this store may reach: start again at the smaller size (the ships
    //   come back within minutes — AIS repeats every few seconds)
    const uint32_t want = capFor(limitBytes);
    if (fresh || h.cap > want) {
        h = Header(); h.cap = want; h.used = 0;
        if (::ftruncate(fd, 0) != 0 || !pwriteAll(fd, &h, sizeof h, 0)) { ::close(fd); return 0; }
        std::vector<DirEnt> z(h.cap, DirEnt{0, 0});
        if (!pwriteAll(fd, z.data(), z.size() * sizeof(DirEnt), kHeader)) { ::close(fd); return 0; }
    }
    std::vector<DirEnt> d(h.cap);
    if (!preadAll(fd, d.data(), d.size() * sizeof(DirEnt), kHeader)) { ::close(fd); return 0; }
    std::unordered_map<uint32_t, uint32_t> slotOf; slotOf.reserve(h.used * 2 + 8);
    for (uint32_t i = 0; i < h.used; ++i) if (d[i].mmsi) slotOf[d[i].mmsi] = i;
    std::vector<char> touched(h.cap, 0);
    size_t n = 0;
    Rec cur;
    for (const Rec& r : in) {
        if (!r.mmsi) continue;
        uint32_t slot;
        auto it = slotOf.find(r.mmsi);
        if (it != slotOf.end()) {
            slot = it->second;
            if (!preadAll(fd, &cur, sizeof cur, recAt(h.cap, slot)) || cur.mmsi != r.mmsi) cur = Rec();
            if (cur.mmsi) merge(cur, r); else cur = r;
        } else if (h.used < h.cap) {
            slot = h.used++;
            cur = r;
        } else {
            // ★ full: the ship heard longest ago (and not one written in this batch) gives its slot up
            uint32_t old = UINT32_MAX; uint32_t oldSeen = UINT32_MAX;
            for (uint32_t i = 0; i < h.cap; ++i) if (!touched[i] && d[i].seen < oldSeen) { oldSeen = d[i].seen; old = i; }
            if (old == UINT32_MAX) break;
            slotOf.erase(d[old].mmsi);
            slot = old; cur = r;
        }
        char buf[kRecSize] = {};
        std::memcpy(buf, &cur, sizeof cur);
        if (!pwriteAll(fd, buf, kRecSize, recAt(h.cap, slot))) break;
        d[slot] = DirEnt{ cur.mmsi, cur.seen };
        slotOf[cur.mmsi] = slot; touched[slot] = 1;
        ++n;
    }
    pwriteAll(fd, d.data(), (size_t)h.used * sizeof(DirEnt), kHeader);
    pwriteAll(fd, &h, sizeof h, 0);
    ::close(fd);
    return n;
}

/** A read: every ship heard at or after `sinceUtc`, newest first, at most `limit`; within `rangeKm` of (rxLat, rxLon)
 *  when both are given (a ship with no position yet is kept only with no range asked). */
struct Query { int64_t sinceUtc = 0; double rangeKm = 0, rxLat = NAN, rxLon = NAN; bool tracks = false; size_t limit = kMaxCap; };
inline std::vector<Rec> read(const std::string& dir, const Query& q, size_t* total = nullptr) {
    std::vector<Rec> out;
    if (total) *total = 0;
    if (dir.empty()) return out;
    Lock lk(dir, true);
    const int fd = ::open((dir + "/" + kFile).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return out;
    Header h;
    if (!preadAll(fd, &h, sizeof h, 0) || h.magic != kMagic || h.recSize != kRecSize || h.cap < kMinCap || h.cap > kMaxCap || h.used > h.cap) { ::close(fd); return out; }
    std::vector<DirEnt> d(h.used);
    if (h.used && !preadAll(fd, d.data(), d.size() * sizeof(DirEnt), kHeader)) { ::close(fd); return out; }
    std::vector<uint32_t> slots;
    for (uint32_t i = 0; i < h.used; ++i) if (d[i].mmsi && (int64_t)d[i].seen >= q.sinceUtc) slots.push_back(i);
    std::sort(slots.begin(), slots.end(), [&](uint32_t a, uint32_t b) { return d[a].seen > d[b].seen; });
    const bool ranged = q.rangeKm > 0 && q.rxLat == q.rxLat && q.rxLon == q.rxLon;
    Rec r;
    for (uint32_t s : slots) {
        if (out.size() >= q.limit) { if (total) ++*total; continue; }
        if (!preadAll(fd, &r, sizeof r, recAt(h.cap, s)) || r.mmsi != d[s].mmsi) continue;
        if (ranged) {
            if (r.lat == kNoPos) continue;
            const double la = de7(r.lat), lo = de7(r.lon);
            const double p1 = q.rxLat * M_PI / 180, p2 = la * M_PI / 180, dl = (lo - q.rxLon) * M_PI / 180;
            const double km = 6371.0 * std::acos(std::max(-1.0, std::min(1.0, std::sin(p1) * std::sin(p2) + std::cos(p1) * std::cos(p2) * std::cos(dl))));
            if (km > q.rangeKm) continue;
        }
        if (total) ++*total;
        out.push_back(r);
    }
    ::close(fd);
    return out;
}

/** {"now":…,"count":…,"total":…,"vessels":[…the live report's vessel shape, ages from now…]} */
inline std::string toJson(const std::vector<Rec>& v, int64_t now, bool tracks, size_t total) {
    std::string o = "{\"now\":" + std::to_string((long long)now) + ",\"count\":" + std::to_string(v.size()) + ",\"total\":" + std::to_string(total) + ",\"vessels\":[";
    bool first = true;
    for (const Rec& r : v) {
        if (!first) o += ',';
        first = false;
        Vessel s = toVessel(r);
        // ★ ages from now, in the service's own terms: t = now, last = seen …
        s.last = (double)r.seen; s.first = (double)(r.first ? r.first : r.seen); s.posAt = r.posAt ? (double)r.posAt : -1;
        std::string one;
        Service::vesselJson(s, (double)now, one);
        // the record's own ETA text and track size (the service's JSON counts its in-RAM trail)
        if (!one.empty() && one.back() == '}') one.pop_back();
        const std::string eta = getStr(r.eta, sizeof r.eta);
        if (!eta.empty() && one.find("\"eta\":") == std::string::npos) one += ",\"eta\":\"" + jsonEsc(eta) + "\"";
        one += ",\"stored\":1";
        if (tracks && r.nTrk) {
            one += ",\"pts\":[";
            for (int i = 0; i < r.nTrk && i < kTrack; ++i) {
                if (i) one += ',';
                one += "[" + fmtNum(de7(r.trk[i].lat), 5) + "," + fmtNum(de7(r.trk[i].lon), 5) + "," + std::to_string((long long)std::max<int64_t>(0, now - r.trk[i].t)) + "]";
            }
            one += "]";
        }
        o += one + "}";
    }
    return o + "]}";
}

} // namespace store
} // namespace vibeais
