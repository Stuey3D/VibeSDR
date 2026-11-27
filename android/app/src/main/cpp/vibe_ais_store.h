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
#include "vibe_log_radio.h"   // ★ beta92 which radio heard each ship

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
                                                          ///< ★ V12 2026-11-16 flags 4 = AtoN status known, then light (bits 3–4), RACON (5–6), alarm (7)
    uint16_t count = 0; uint8_t nTrk = 0, ch = 0;
    uint32_t imo = 0;
    int16_t alt = -1, spare = 0;   ///< ★ beta95 spare bit 0 = DX (Vessel::dx) · bit 1 = coarse (type 27) position
                                   ///< ★ beta96 the HIGH BYTE = the last frame's SNR + 64 (1–255 → −63…191 dB), 0 = not kept
                                   ///<   (snrCode / snrOf) — older stores and writers leave it 0, older readers never look
    char name[24] = {}, call[8] = {}, dest[24] = {}, eta[12] = {};
    TrkPt trk[kTrack] = {};                                ///< oldest first
    /** ★ beta92 which radios heard it (bit n = <gallery>/radios.idx line n, vibe_log_radio.h) — the last 4 bytes of the
     *  512, always written 0 before (the write zero-fills the slot), so an older store reads as unknown. OR-merged. */
    uint32_t radios = 0;
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

/** ★ beta96 a ship's last SNR as the record's spare high byte: SNR + 64, 1–255; 0 = none. */
inline int snrCode(const Vessel& v) {
    if (!v.hasSnr || !(v.snr == v.snr)) return 0;
    return (int)std::max(1L, std::min(255L, std::lround(v.snr) + 64));
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
    { const asmsg::AtonStatus st = asmsg::atonStatus(v.atonStatus);
      if (st.valid) r.flags |= uint8_t(4 | (st.light << 3) | (st.racon << 5) | (st.alarm ? 128 : 0)); }
    r.count = (uint16_t)std::min(65535, v.count); r.ch = (uint8_t)v.ch; r.imo = v.imo; r.alt = (int16_t)std::min(32767, v.altM);
    r.spare = int16_t((v.dx ? 1 : 0) | (v.coarse ? 2 : 0) | (snrCode(v) << 8));
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
    v.dx = r.spare & 1; v.coarse = r.spare & 2;
    { const int c = (uint16_t(r.spare) >> 8) & 0xFF; v.hasSnr = c != 0; v.snr = c ? float(c - 64) : 0.f; }   // ★ beta96
    // ★ the status byte back in its IALA A-126 page-7 form (vibe_ais_asm.h atonStatus reads it)
    if (r.flags & 4) v.atonStatus = (7 << 5) | (((r.flags >> 5) & 3) << 3) | (((r.flags >> 3) & 3) << 1) | ((r.flags >> 7) & 1);
    return v;
}
/* ★★★ beta95 THE WHOLE-MINUTE SCRUB (vibe_ais_service.h kDxNm): a store written before beta95 may hold a fake — a
 *  moving station's position with latitude AND longitude on whole minutes (MMSI 244780619's 00°15.000′N 008°50.000′E,
 *  Stuart 11-25) — as its position or among its track points, which drew a dashed line from Rotterdam to Africa. Read
 *  and merged records lose them: the position (the ship stays listed, no dot) and each such track point. AtoNs and base
 *  stations (surveyed, may be round) and type 27's coarse grid are left alone. 1e-7° is 6e-6′: a whole minute stored
 *  is within 3e-6′ of it, the next real 1/10 000′ value 1e-4′ away (the test: within 1e-5′). */
inline bool wholeMinE7(int32_t v) {
    if (v == kNoPos) return false;
    const double m = v * 1e-7 * 60.0;
    return std::fabs(m - std::round(m)) < 1e-5;
}
inline bool fakePosE7(int32_t lat, int32_t lon) { return wholeMinE7(lat) && wholeMinE7(lon); }
inline bool movingKind(uint8_t k) { return k == 0 || k == 1 || k == 4 || k == 5; }
/** True if anything was taken out. */
inline bool scrub(Rec& r) {
    if (!movingKind(r.kind) || (r.spare & 2)) return false;
    bool any = false;
    if (fakePosE7(r.lat, r.lon)) { r.lat = r.lon = kNoPos; r.posAt = 0; r.spare &= ~1; any = true; }
    int w = 0;
    for (int i = 0; i < r.nTrk && i < kTrack; ++i) {
        if (fakePosE7(r.trk[i].lat, r.trk[i].lon)) { any = true; continue; }
        r.trk[w++] = r.trk[i];
    }
    for (int i = w; i < r.nTrk && i < kTrack; ++i) r.trk[i] = TrkPt{};
    r.nTrk = (uint8_t)w;
    return any;
}
/** ★ Merge `in` (newer from one radio) into `have` (on disk, perhaps another radio's): the newest state wins field by
 *  field where `in` knows it; the tracks of both, in time order, newest kTrack. */
inline void merge(Rec& have, const Rec& in0) {
    Rec in = in0;
    scrub(have); scrub(in);   // ★ beta95 an old fake never survives a write
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
    if (!(out.flags & 4) && (old.flags & 4)) out.flags = uint8_t((out.flags & 3) | (old.flags & ~3));
    if (!out.len) { out.len = old.len; out.beam = old.beam; }
    if (out.draught10 < 0) out.draught10 = old.draught10;
    if (out.lat == kNoPos || (old.posAt > out.posAt && old.lat != kNoPos)) {
        out.lat = old.lat; out.lon = old.lon; out.posAt = old.posAt; out.sog10 = old.sog10; out.cog10 = old.cog10; out.hdg = old.hdg;
        out.spare = int16_t((out.spare & ~3) | (old.spare & 3));   // ★ beta95 its DX / coarse marks travel with the position
    }
    if (old.first && (!out.first || old.first < out.first)) out.first = old.first;
    out.count = (uint16_t)std::min<uint32_t>(65535, (uint32_t)std::max(have.count, in.count));
    out.radios = have.radios | in.radios;   // ★ beta92 every radio that heard it
    if (!(uint16_t(out.spare) & 0xFF00)) out.spare = int16_t((uint16_t(out.spare) & 0x00FF) | (uint16_t(old.spare) & 0xFF00));   // ★ beta96 the SNR a newer record lacks
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
static constexpr const char* kMetFile = "ais-met.dat";   ///< ★ V12 2026-11-16 the stations' weather (below)
inline uint64_t bytesOnDisk(const std::string& dir) {
    if (dir.empty()) return 0;
    uint64_t n = 0;
    struct stat st{};
    if (::stat((dir + "/" + kFile).c_str(), &st) == 0) n += (uint64_t)st.st_size;
    if (::stat((dir + "/" + kMetFile).c_str(), &st) == 0) n += (uint64_t)st.st_size;
    return n;
}

/** ★★ WRITE: the ships that changed, merged into the store under the machine-wide lock. `limitBytes` is the owner's
 *  gallery size (0 = the gallery keeps nothing: nothing is written, and an old store is removed). @return records written. */
inline size_t write(const std::string& dir, uint64_t limitBytes, const std::vector<Rec>& in) {
    if (dir.empty()) return 0;
    const std::string path = dir + "/" + kFile;
    Lock lk(dir, false);
    if (limitBytes == 0) { ::unlink(path.c_str()); ::unlink((dir + "/" + kMetFile).c_str()); return 0; }
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
    const int bit = vibe::logradio::bitFor(dir, vibe::logradio::mine());   // ★ beta92 this radio's bit (-1 = none)
    for (const Rec& r0 : in) {
        if (!r0.mmsi) continue;
        Rec r = r0;
        if (bit >= 0) r.radios |= 1u << bit;
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
        scrub(r);   // ★ beta95 a fake written before the whole-minute check: never drawn
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

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ★★ V12 2026-11-16 THE MET STORE: <gallery>/ais-met.dat — the weather AtoN and port stations send (binary message 8,
//  vibe_ais_asm.h), so the shipping map shows a station's wind, tide and visibility with nobody listening.
//    header (64 B) · records (used × 288 B): MMSI, last heard, up to 4 sites with when each was heard
//    ★★ BOUNDED: at most metCapFor(limit) stations (8…128 — 37 KB at most), the station heard longest ago gives way;
//       counted inside the gallery's size (bytesOnDisk above). Same lock, same "only what changed" writes as the ships.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t kMetMagic = 0x314D4941;   // "AIM1"
static constexpr uint32_t kMetRecSize = 288;
static constexpr uint32_t kMetMaxCap = 128, kMetMinCap = 8;
struct MetRec {
    uint32_t mmsi = 0, seen = 0;
    uint32_t at[Vessel::kMetSites] = {};   ///< UTC each site was heard
    uint8_t n = 0, pad[3] = {};
    asmsg::Met site[Vessel::kMetSites];
};
static_assert(sizeof(MetRec) <= kMetRecSize, "a met record is 288 bytes");
inline uint32_t metCapFor(uint64_t limitBytes) {
    const uint64_t c = limitBytes / 256 / kMetRecSize;
    return (uint32_t)std::max<uint64_t>(kMetMinCap, std::min<uint64_t>(kMetMaxCap, c));
}
/** A ship from the service → its met record (n = 0: it has none). */
inline MetRec metFromVessel(const Vessel& v, double svcNow, int64_t wallNow) {
    MetRec r;
    r.mmsi = v.mmsi; r.seen = (uint32_t)std::max<int64_t>(0, wallNow - (int64_t)std::llround(svcNow - v.last));
    r.n = (uint8_t)std::max(0, std::min(v.nMet, (int)Vessel::kMetSites));
    for (int k = 0; k < r.n; ++k) { r.site[k] = v.met[k]; r.at[k] = (uint32_t)std::max<int64_t>(0, wallNow - (int64_t)std::llround(svcNow - v.metAt[k])); }
    return r;
}
/** Merge sites: the same place (within 500 m, or both without a position) keeps the newer; the oldest give way. */
inline void mergeMet(MetRec& have, const MetRec& in) {
    for (int i = 0; i < in.n; ++i) {
        const asmsg::Met& m = in.site[i];
        int slot = -1;
        for (int k = 0; k < have.n; ++k) {
            const asmsg::Met& o = have.site[k];
            bool same = o.lat == asmsg::kNoPos32 && m.lat == asmsg::kNoPos32;
            if (o.lat != asmsg::kNoPos32 && m.lat != asmsg::kNoPos32) {
                const double dy = (o.lat - m.lat) * 1e-7 * 111320.0, dx = (o.lon - m.lon) * 1e-7 * 111320.0 * std::cos(m.lat * 1e-7 * M_PI / 180);
                same = dx * dx + dy * dy < 500.0 * 500.0;
            }
            if (same) { slot = k; break; }
        }
        if (slot >= 0) { if (in.at[i] >= have.at[slot]) { have.site[slot] = m; have.at[slot] = in.at[i]; } continue; }
        if (have.n < Vessel::kMetSites) slot = have.n++;
        else { slot = 0; for (int k = 1; k < have.n; ++k) if (have.at[k] < have.at[slot]) slot = k; if (have.at[slot] > in.at[i]) continue; }
        have.site[slot] = m; have.at[slot] = in.at[i];
    }
    have.seen = std::max(have.seen, in.seen);
}
/** ★ WRITE the stations among `changed` that carry weather. @return records written. */
inline size_t writeMet(const std::string& dir, uint64_t limitBytes, const std::vector<Vessel>& changed, double svcNow, int64_t wallNow) {
    if (dir.empty() || limitBytes == 0) return 0;
    std::vector<MetRec> in;
    for (const Vessel& v : changed) if (v.nMet > 0) in.push_back(metFromVessel(v, svcNow, wallNow));
    if (in.empty()) return 0;
    const std::string path = dir + "/" + kMetFile;
    Lock lk(dir, false);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return 0;
    Header h;
    struct stat st{};
    const uint32_t want = metCapFor(limitBytes);
    const bool fresh = ::fstat(fd, &st) != 0 || (uint64_t)st.st_size < kHeader || !preadAll(fd, &h, sizeof h, 0) || h.magic != kMetMagic
                    || h.recSize != kMetRecSize || h.cap < kMetMinCap || h.cap > kMetMaxCap || h.used > h.cap
                    || (uint64_t)st.st_size < kHeader + (uint64_t)h.used * kMetRecSize;
    if (fresh || h.cap > want) {
        h = Header(); h.magic = kMetMagic; h.recSize = kMetRecSize; h.cap = want; h.used = 0;
        if (::ftruncate(fd, 0) != 0 || !pwriteAll(fd, &h, sizeof h, 0)) { ::close(fd); return 0; }
    }
    std::vector<MetRec> all(h.used);
    for (uint32_t i = 0; i < h.used; ++i) {
        char buf[kMetRecSize];
        if (!preadAll(fd, buf, kMetRecSize, (off_t)(kHeader + (uint64_t)i * kMetRecSize))) { h.used = i; all.resize(i); break; }
        std::memcpy(&all[i], buf, sizeof(MetRec));
        if (all[i].n > Vessel::kMetSites) all[i].n = Vessel::kMetSites;
    }
    std::vector<char> touched(h.cap, 0);
    size_t n = 0;
    for (const MetRec& r : in) {
        uint32_t slot = UINT32_MAX;
        for (uint32_t i = 0; i < h.used; ++i) if (all[i].mmsi == r.mmsi) { slot = i; break; }
        if (slot != UINT32_MAX) mergeMet(all[slot], r);
        else if (h.used < h.cap) { slot = h.used++; all.push_back(r); }
        else {
            uint32_t old = UINT32_MAX, oldSeen = UINT32_MAX;
            for (uint32_t i = 0; i < h.used; ++i) if (!touched[i] && all[i].seen < oldSeen) { oldSeen = all[i].seen; old = i; }
            if (old == UINT32_MAX) break;
            slot = old; all[slot] = r;
        }
        char buf[kMetRecSize] = {};
        std::memcpy(buf, &all[slot], sizeof(MetRec));
        if (!pwriteAll(fd, buf, kMetRecSize, (off_t)(kHeader + (uint64_t)slot * kMetRecSize))) break;
        touched[slot] = 1; ++n;
    }
    pwriteAll(fd, &h, sizeof h, 0);
    ::close(fd);
    return n;
}
inline std::vector<MetRec> readMet(const std::string& dir) {
    std::vector<MetRec> out;
    if (dir.empty()) return out;
    Lock lk(dir, true);
    const int fd = ::open((dir + "/" + kMetFile).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return out;
    Header h;
    if (preadAll(fd, &h, sizeof h, 0) && h.magic == kMetMagic && h.recSize == kMetRecSize && h.cap >= kMetMinCap && h.cap <= kMetMaxCap && h.used <= h.cap) {
        for (uint32_t i = 0; i < h.used; ++i) {
            char buf[kMetRecSize]; MetRec r;
            if (!preadAll(fd, buf, kMetRecSize, (off_t)(kHeader + (uint64_t)i * kMetRecSize))) break;
            std::memcpy(&r, buf, sizeof r);
            if (!r.mmsi) continue;
            if (r.n > Vessel::kMetSites) r.n = Vessel::kMetSites;
            out.push_back(r);
        }
    }
    ::close(fd);
    return out;
}
inline void metJsonFor(const std::vector<MetRec>& all, uint32_t mmsi, int64_t now, std::string& one) {
    for (const MetRec& r : all) {
        if (r.mmsi != mmsi || !r.n) continue;
        int idx[Vessel::kMetSites]; for (int k = 0; k < r.n; ++k) idx[k] = k;
        std::sort(idx, idx + r.n, [&](int a, int b) { return r.at[a] > r.at[b]; });
        one += ",\"met\":[";
        for (int k = 0; k < r.n; ++k) { if (k) one += ','; asmsg::metJson(r.site[idx[k]], (double)(now - (int64_t)r.at[idx[k]]), one); }
        one += "]";
        return;
    }
}

/** {"now":…,"count":…,"total":…,"vessels":[…the live report's vessel shape, ages from now…]} */
/** `dir` given: each station's stored weather (the met store, below) is added as the live report's "met" array. */
inline std::string toJson(const std::vector<Rec>& v, int64_t now, bool tracks, size_t total, const std::string& dir = std::string()) {
    const std::vector<MetRec> mets = dir.empty() ? std::vector<MetRec>() : readMet(dir);
    // ★★ beta92 the radios (vibe_log_radio.h): per ship "radios" (absent = unknown), and the answer's "radios" list
    const std::vector<std::string> reg = vibe::logradio::readRegistry(dir);
    std::vector<std::pair<std::string, uint64_t>> perRadio;
    uint64_t untagged = 0;
    std::string o = "{\"now\":" + std::to_string((long long)now) + ",\"snrv\":2,\"count\":" + std::to_string(v.size()) + ",\"total\":" + std::to_string(total) + ",\"vessels\":[";
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
        {
            const std::vector<std::string> ids = vibe::logradio::idsOf(reg, r.radios);
            if (ids.empty()) ++untagged;
            else {
                one += ",\"radios\":[";
                for (size_t i = 0; i < ids.size(); i++) {
                    if (i) one += ',';
                    one += "\"" + ids[i] + "\"";
                    auto it = std::find_if(perRadio.begin(), perRadio.end(), [&](const std::pair<std::string, uint64_t>& p) { return p.first == ids[i]; });
                    if (it == perRadio.end()) perRadio.push_back({ ids[i], 1 }); else ++it->second;
                }
                one += "]";
            }
        }
        if (!mets.empty()) metJsonFor(mets, r.mmsi, now, one);
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
    return o + "],\"radios\":" + vibe::logradio::radiosJson(perRadio, untagged) + "}";
}

} // namespace store
} // namespace vibeais
