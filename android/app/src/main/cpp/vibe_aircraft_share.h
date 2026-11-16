// vibe_aircraft_share.h — V12 2026-11-16 THE SHARED AIRCRAFT TABLE: every aircraft any radio on this machine heard — ADS-B
// (1090), ACARS, VDL2, HFDL and the ADS-C positions they carry — ONE ROW PER AIRCRAFT, for the AVIATION MAP.
// docs/v12/AVIATION.md §The shared table.
//
// ★★★ WHY (Stuart, 2026-11-16): a server with two SDRs — one on ADS-B, one on ACARS/VDL2 — must not show the same aircraft
//     twice. Each radio is its own PROCESS (vibeserver/main.cpp superviseRadios, or systemd's vibeserver@<serial>), and each
//     holds its own tables (vibe_adsb.h Tracker, vibe_avi_service.h Tracker). So the rows have to meet where every radio
//     can read them: one small fixed file, under a kernel lock — the ship store's pattern (vibe_ais_store.h).
//
// ── The merge (one rule, applied by every writer under the lock) ────────────────────────────────────────────────────
//   1. by the ICAO 24-bit address where it is known (ADS-B, VDL2's AVLC address, HFDL logons);
//   2. else by REGISTRATION (ACARS, the ADS-B aircraft database) — compared without dots, hyphens or spaces, and never
//      across two different ICAO addresses;
//   3. else by FLIGHT ID — exactly, and never across two different addresses or registrations.
//   A row that learns both keys (a VDL2 frame carries the address AND an ACARS registration; ADS-B's database maps the
//   address to its registration) absorbs a row that had only the other: an aircraft first heard on ACARS (registration)
//   and then on ADS-B (address + registration) becomes ONE row with both sources.
//   ★ Field by field the newest wins; the position is the newest FIX by arrival (ADS-B's every second while it is in range,
//     a datalink report when it is not), with the source it came from.
//   ★ Each row carries WHICH SOURCES heard it (a bit each) and when each last did.
//
// ── The file: <runtime dir>/aircraft-live.dat ─────────────────────────────────────────────────────────────────────────
//   header (64 B) · HFDL ground stations heard (128 × 16 B) · records (kCap × 256 B, fixed) = 264 256 B, made at full size.
//   ★★ BOUNDED BY CONSTRUCTION: kCap rows. Full: the row heard longest ago gives its slot up. A row unheard for kKeepSec
//      is free. Nothing grows.
//   ★★ IN RAM, NOT ON THE SD CARD: the runtime directory (vibeserver/main.cpp handoffDir: /run/vibeserver, a tmpfs, on a
//      Linux server). ADS-B rewrites its aircraft every couple of seconds — a Pi's card must never see that. Nothing in it
//      is meant to outlive the server: it is the live picture, not a log.
//   ★★ NO DIRECTORY (the Android app's one radio, a test): the same table in memory, under a mutex — one code path.
//   ★ OFF EVERY DSP AND DECODER THREAD: one Sharer thread per process gathers what changed (ADS-B's snapshot, the datalink
//     table's rows) every kFlushSec and writes it; a map's read is one pread of the file under a shared lock.
//   ⚖ What is in it: addresses, registrations, flights, positions, altitudes, airports, the last label — flight data, shown
//     by default like the registration. NEVER a message body (vibe_avi_service.h keeps those behind the owner's opt-in).
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "vibe_thread.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vibeavshare {

/** The sources, a bit each. ADS-C is an application inside ACARS (over VHF ACARS, VDL2 or HFDL): its bit is set beside
 *  the bearer's when an ADS-C report gave the position. */
enum Src : int { kAdsb = 0, kAcars = 1, kVdl2 = 2, kHfdl = 3, kAdsc = 4, kNSrc = 5 };
inline const char* srcName(int s) { static const char* n[kNSrc] = { "ADS-B", "ACARS", "VDL2", "HFDL", "ADS-C" }; return s >= 0 && s < kNSrc ? n[s] : ""; }

static constexpr const char* kFile = "aircraft-live.dat";
static constexpr uint32_t kMagic = 0x31525641;     // "AVR1"
static constexpr uint32_t kVersion = 1;
static constexpr uint32_t kCap = 1024;             ///< rows (≈ 260 KB) — an ADS-B site sees a few hundred, HFDL a few hundred more
static constexpr uint32_t kRecSize = 256;
static constexpr uint32_t kHeader = 64;
static constexpr uint32_t kGs = 128, kGsSize = 16;
static constexpr int64_t kKeepSec = 3600;          ///< a row unheard for an hour is free (the datalink table's own keep)
static constexpr double kFlushSec = 2.0;

/** What one writer knows about one aircraft (only the fields it knows are set). Times are wall-clock milliseconds. */
struct Update {
    uint32_t icao = 0;
    std::string reg, flight, type, from, to, label, app, posTime, via;
    uint8_t srcMask = 0;
    int64_t srcMs[kNSrc] = {};          ///< when each source in srcMask last heard it
    int64_t heardMs = 0;                ///< the newest of srcMs
    bool hasPos = false; double lat = NAN, lon = NAN; int64_t posMs = 0; int posSrc = -1;
    bool hasAlt = false; int altFt = 0;
    bool hasTrk = false; float trk = 0;
    bool hasGs = false; float gs = 0;
    bool ground = false;
    uint32_t count = 0;                 ///< messages since this writer's last update (added)
};
struct GsHeard { int id = -1; int64_t ms = 0; double khz = 0; };

#pragma pack(push, 1)
/** ★ ONE AIRCRAFT, 256 bytes on disk. Wall-clock milliseconds; positions 1e-7 degree. */
struct Rec {
    uint32_t icao;
    uint8_t used, srcMask, flags, posSrc;     ///< flags: 1 pos · 2 alt · 4 trk · 8 gs · 16 on the ground
    int64_t firstMs, lastMs, posMs;
    int64_t srcMs[kNSrc];
    int32_t lat, lon, altFt;
    float trk, gs;
    uint32_t count;
    char reg[12], flight[12], type[8], from[8], to[8], label[4], app[8], posTime[12], via[40];
};
#pragma pack(pop)
static_assert(sizeof(Rec) <= kRecSize, "an aircraft record is 256 bytes");
struct GsRec { int64_t ms; double khz; };
static_assert(sizeof(GsRec) == kGsSize, "a ground station is 16 bytes");
struct Header { uint32_t magic = kMagic, version = kVersion, cap = kCap, recSize = kRecSize; uint32_t pad[12] = {}; };
static_assert(sizeof(Header) == kHeader, "the header is 64 bytes");
static constexpr uint64_t kFileBytes = kHeader + (uint64_t)kGs * kGsSize + (uint64_t)kCap * kRecSize;

inline void putStr(char* dst, size_t n, const std::string& s) { std::memset(dst, 0, n); std::memcpy(dst, s.data(), std::min(n - 1, s.size())); }
inline std::string getStr(const char* src, size_t n) { size_t k = 0; while (k < n && src[k]) ++k; return std::string(src, k); }
/** Printable ASCII only (a space kept inside, none at the ends), at most `max` (they reach JSON and text frames). */
inline std::string clean(const std::string& x, size_t max) {
    std::string o;
    for (char c : x) if (c >= 0x20 && c < 0x7F) o += c;
    const size_t a = o.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    o = o.substr(a, o.find_last_not_of(' ') - a + 1);
    if (o.size() > max) o.resize(max);
    return o;
}
/** ★ The registration as a KEY: upper case, no dots, hyphens or spaces ("G-EZWK", ".G-EZWK" and "GEZWK" are one). */
inline std::string regKey(const std::string& r) {
    std::string o;
    for (char c : r) { if (c == '.' || c == '-' || c == ' ') continue; o += (char)std::toupper((unsigned char)c); }
    return o;
}
inline std::string flightKey(const std::string& f) {
    std::string o;
    for (char c : f) { if (c == ' ' || c == '.') continue; o += (char)std::toupper((unsigned char)c); }
    return o;
}

/** A row in memory (the file's Rec with strings). */
struct Row {
    bool used = false;
    uint32_t icao = 0;
    uint8_t srcMask = 0, flags = 0; int posSrc = -1;
    int64_t firstMs = 0, lastMs = 0, posMs = 0;
    int64_t srcMs[kNSrc] = {};
    double lat = NAN, lon = NAN; int altFt = 0; float trk = 0, gs = 0;
    uint32_t count = 0;
    std::string reg, flight, type, from, to, label, app, posTime, via;
};
inline Row fromRec(const Rec& r) {
    Row w;
    w.used = r.used == 1; w.icao = r.icao & 0xFFFFFF; w.srcMask = r.srcMask & 0x1F; w.flags = r.flags;
    w.posSrc = r.posSrc < kNSrc ? r.posSrc : -1;
    w.firstMs = r.firstMs; w.lastMs = r.lastMs; w.posMs = r.posMs;
    for (int s = 0; s < kNSrc; ++s) w.srcMs[s] = r.srcMs[s];
    w.lat = (r.flags & 1) ? r.lat / 1e7 : NAN; w.lon = (r.flags & 1) ? r.lon / 1e7 : NAN;
    w.altFt = r.altFt; w.trk = r.trk; w.gs = r.gs; w.count = r.count;
    w.reg = getStr(r.reg, sizeof r.reg); w.flight = getStr(r.flight, sizeof r.flight); w.type = getStr(r.type, sizeof r.type);
    w.from = getStr(r.from, sizeof r.from); w.to = getStr(r.to, sizeof r.to); w.label = getStr(r.label, sizeof r.label);
    w.app = getStr(r.app, sizeof r.app); w.posTime = getStr(r.posTime, sizeof r.posTime); w.via = getStr(r.via, sizeof r.via);
    // ★ a hostile or torn record never reaches the JSON as anything but printable text and finite numbers
    if (!(std::isfinite(w.lat) && std::isfinite(w.lon) && std::fabs(w.lat) <= 90 && std::fabs(w.lon) <= 180)) { w.flags &= ~1; w.lat = w.lon = NAN; }
    if (!std::isfinite(w.trk)) w.flags &= ~4;
    if (!std::isfinite(w.gs)) w.flags &= ~8;
    return w;
}
inline Rec toRec(const Row& w) {
    Rec r; std::memset(&r, 0, sizeof r);
    r.used = w.used ? 1 : 0; r.icao = w.icao; r.srcMask = w.srcMask; r.flags = w.flags; r.posSrc = (uint8_t)(w.posSrc < 0 ? 0xFF : w.posSrc);
    r.firstMs = w.firstMs; r.lastMs = w.lastMs; r.posMs = w.posMs;
    for (int s = 0; s < kNSrc; ++s) r.srcMs[s] = w.srcMs[s];
    if (w.flags & 1) { r.lat = (int32_t)std::lround(w.lat * 1e7); r.lon = (int32_t)std::lround(w.lon * 1e7); }
    r.altFt = w.altFt; r.trk = w.trk; r.gs = w.gs; r.count = w.count;
    putStr(r.reg, sizeof r.reg, w.reg); putStr(r.flight, sizeof r.flight, w.flight); putStr(r.type, sizeof r.type, w.type);
    putStr(r.from, sizeof r.from, w.from); putStr(r.to, sizeof r.to, w.to); putStr(r.label, sizeof r.label, w.label);
    putStr(r.app, sizeof r.app, w.app); putStr(r.posTime, sizeof r.posTime, w.posTime); putStr(r.via, sizeof r.via, w.via);
    return r;
}

/** ★ Apply one update to a row (newest wins field by field). */
inline void apply(Row& w, const Update& u) {
    const bool newer = u.heardMs >= w.lastMs;
    if (!w.used) { w = Row(); w.used = true; w.firstMs = u.heardMs; }
    if (u.icao) w.icao = u.icao;
    auto take = [&](std::string& dst, const std::string& v, size_t max) { const std::string c = clean(v, max); if (!c.empty() && (newer || dst.empty())) dst = c; };
    take(w.reg, u.reg, 11); take(w.flight, u.flight, 11); take(w.type, u.type, 7);
    take(w.label, u.label, 3); take(w.app, u.app, 7); take(w.via, u.via, 39);
    if (!u.from.empty() || !u.to.empty()) { if (newer || w.from.empty()) { w.from = clean(u.from, 7); w.to = clean(u.to, 7); } }
    w.srcMask |= u.srcMask;
    for (int s = 0; s < kNSrc; ++s) if ((u.srcMask >> s) & 1) w.srcMs[s] = std::max(w.srcMs[s], u.srcMs[s] ? u.srcMs[s] : u.heardMs);
    w.lastMs = std::max(w.lastMs, u.heardMs);
    if (!w.firstMs || u.heardMs < w.firstMs) w.firstMs = std::min(w.firstMs ? w.firstMs : u.heardMs, u.heardMs);
    w.count += u.count;
    if (u.hasPos && std::isfinite(u.lat) && std::isfinite(u.lon) && std::fabs(u.lat) <= 90 && std::fabs(u.lon) <= 180
        && (!(w.flags & 1) || u.posMs >= w.posMs)) {
        w.lat = u.lat; w.lon = u.lon; w.posMs = u.posMs; w.posSrc = u.posSrc;
        w.flags = uint8_t((w.flags & ~(1 | 2 | 4 | 8 | 16)) | 1 | (u.hasAlt ? 2 : 0) | (u.hasTrk ? 4 : 0) | (u.hasGs ? 8 : 0) | (u.ground ? 16 : 0));
        w.altFt = u.hasAlt ? u.altFt : 0; w.trk = u.hasTrk ? u.trk : 0; w.gs = u.hasGs ? u.gs : 0;
        w.posTime = clean(u.posTime, 11);
    } else if (u.hasAlt && newer && !(w.flags & 1)) { w.flags |= 2; w.altFt = u.altFt; }
}
/** ★ Absorb row `b` into row `a` (they turned out to be one aircraft): every source, the newest of each field. */
inline void absorb(Row& a, const Row& b) {
    const bool bNewer = b.lastMs > a.lastMs;
    if (!a.icao) a.icao = b.icao;
    auto fill = [&](std::string& dst, const std::string& v) { if (!v.empty() && (dst.empty() || bNewer)) dst = v; };
    fill(a.reg, b.reg); fill(a.flight, b.flight); fill(a.type, b.type); fill(a.label, b.label); fill(a.app, b.app); fill(a.via, b.via);
    if (!b.from.empty() && (a.from.empty() || bNewer)) { a.from = b.from; a.to = b.to; }
    a.srcMask |= b.srcMask;
    for (int s = 0; s < kNSrc; ++s) a.srcMs[s] = std::max(a.srcMs[s], b.srcMs[s]);
    a.firstMs = std::min(a.firstMs ? a.firstMs : b.firstMs, b.firstMs ? b.firstMs : a.firstMs);
    a.lastMs = std::max(a.lastMs, b.lastMs);
    a.count += b.count;
    if ((b.flags & 1) && (!(a.flags & 1) || b.posMs > a.posMs)) {
        a.lat = b.lat; a.lon = b.lon; a.posMs = b.posMs; a.posSrc = b.posSrc; a.altFt = b.altFt; a.trk = b.trk; a.gs = b.gs; a.posTime = b.posTime;
        a.flags = b.flags;
    }
}

/** ★★ THE MERGE over a whole table: `rows` (kCap, from the file), the updates of one writer; returns the indexes changed. */
inline std::vector<size_t> mergeInto(std::vector<Row>& rows, const std::vector<Update>& ups, int64_t nowMs) {
    std::vector<char> dirty(rows.size(), 0);
    // expiry: an hour unheard frees the slot
    for (size_t i = 0; i < rows.size(); ++i) if (rows[i].used && nowMs - rows[i].lastMs > kKeepSec * 1000) { rows[i] = Row(); dirty[i] = 1; }
    std::unordered_map<uint32_t, size_t> byIcao;
    std::unordered_map<std::string, size_t> byReg, byFlight;
    auto index = [&](size_t i) {
        const Row& w = rows[i];
        if (!w.used) return;
        if (w.icao) byIcao[w.icao] = i;
        if (!w.reg.empty()) byReg[regKey(w.reg)] = i;
        if (!w.flight.empty()) byFlight[flightKey(w.flight)] = i;
    };
    auto unindex = [&](size_t i) {
        const Row& w = rows[i];
        if (w.icao) { auto it = byIcao.find(w.icao); if (it != byIcao.end() && it->second == i) byIcao.erase(it); }
        if (!w.reg.empty()) { auto it = byReg.find(regKey(w.reg)); if (it != byReg.end() && it->second == i) byReg.erase(it); }
        if (!w.flight.empty()) { auto it = byFlight.find(flightKey(w.flight)); if (it != byFlight.end() && it->second == i) byFlight.erase(it); }
    };
    for (size_t i = 0; i < rows.size(); ++i) index(i);
    auto icaoOk = [&](const Row& w, uint32_t ic) { return !ic || !w.icao || w.icao == ic; };
    auto regOk = [&](const Row& w, const std::string& rk) { return rk.empty() || w.reg.empty() || regKey(w.reg) == rk; };
    for (const Update& u : ups) {
        if (!u.icao && u.reg.empty() && u.flight.empty()) continue;
        const std::string rk = regKey(clean(u.reg, 11)), fk = flightKey(clean(u.flight, 11));
        long a = -1, b = -1, c = -1;
        if (u.icao) { auto it = byIcao.find(u.icao); if (it != byIcao.end()) a = (long)it->second; }
        if (!rk.empty()) { auto it = byReg.find(rk); if (it != byReg.end() && icaoOk(rows[it->second], u.icao)) b = (long)it->second; }
        if (a < 0 && b < 0 && !fk.empty()) {
            auto it = byFlight.find(fk);
            if (it != byFlight.end() && icaoOk(rows[it->second], u.icao) && regOk(rows[it->second], rk)) c = (long)it->second;
        }
        long at = a >= 0 ? a : b >= 0 ? b : c;
        // ★ the update names two rows (its address one, its registration the other): they are one aircraft
        if (a >= 0 && b >= 0 && a != b) { unindex((size_t)b); absorb(rows[(size_t)a], rows[(size_t)b]); rows[(size_t)b] = Row(); dirty[(size_t)b] = 1; }
        if (at < 0) {
            // a free slot, else the row heard longest ago
            long freeI = -1, oldest = -1;
            for (size_t i = 0; i < rows.size(); ++i) {
                if (!rows[i].used) { freeI = (long)i; break; }
                if (oldest < 0 || rows[i].lastMs < rows[(size_t)oldest].lastMs) oldest = (long)i;
            }
            at = freeI >= 0 ? freeI : oldest;
            if (at < 0) continue;
            unindex((size_t)at);
            rows[(size_t)at] = Row();
        } else unindex((size_t)at);
        apply(rows[(size_t)at], u);
        dirty[(size_t)at] = 1;
        index((size_t)at);
    }
    std::vector<size_t> out;
    for (size_t i = 0; i < rows.size(); ++i) if (dirty[i]) out.push_back(i);
    return out;
}

/** ★ The table: a file in `dir` under flock, or (dir "") the same bytes in memory under a mutex. */
class Table {
public:
    void setDir(const std::string& d) { std::lock_guard<std::mutex> lk(m_); dir_ = d; }
    std::string dir() { std::lock_guard<std::mutex> lk(m_); return dir_; }
    std::string path() { std::lock_guard<std::mutex> lk(m_); return dir_.empty() ? std::string() : dir_ + "/" + kFile; }

    /** Merge one writer's updates and the ground stations it heard. Returns the rows changed. */
    size_t write(const std::vector<Update>& ups, const std::vector<GsHeard>& gs, int64_t nowMs) {
        Io io(*this, false);
        if (!io.ok()) return 0;
        std::vector<Row> rows(kCap);
        std::vector<Rec> recs(kCap);
        io.read(kHeader + kGs * kGsSize, recs.data(), (size_t)kCap * kRecSize, true);
        for (uint32_t i = 0; i < kCap; ++i) rows[i] = fromRec(recs[i]);
        const std::vector<size_t> d = mergeInto(rows, ups, nowMs);
        for (size_t i : d) { const Rec r = toRec(rows[i]); io.write(kHeader + (uint64_t)kGs * kGsSize + (uint64_t)i * kRecSize, &r, sizeof r); }
        if (!gs.empty()) {
            GsRec g[kGs];
            io.readRaw(kHeader, g, sizeof g);
            for (const auto& h : gs) if (h.id >= 0 && h.id < (int)kGs && h.ms >= g[h.id].ms) { g[h.id].ms = h.ms; g[h.id].khz = h.khz; }
            io.write(kHeader, g, sizeof g);
        }
        return d.size();
    }
    /** Every row heard since `sinceMs` (0 = all still kept), newest first, and the ground stations. */
    std::vector<Row> read(int64_t nowMs, int64_t sinceMs, std::vector<GsHeard>* gs = nullptr) {
        std::vector<Row> out;
        Io io(*this, true);
        if (!io.ok()) return out;
        std::vector<Rec> recs(kCap);
        io.read(kHeader + kGs * kGsSize, recs.data(), (size_t)kCap * kRecSize, true);
        for (const Rec& r : recs) {
            if (r.used != 1) continue;
            Row w = fromRec(r);
            if (nowMs - w.lastMs > kKeepSec * 1000 || w.lastMs < sinceMs) continue;
            out.push_back(std::move(w));
        }
        std::sort(out.begin(), out.end(), [](const Row& a, const Row& b) { return a.lastMs > b.lastMs; });
        if (gs) {
            GsRec g[kGs];
            io.readRaw(kHeader, g, sizeof g);
            for (int i = 0; i < (int)kGs; ++i) if (g[i].ms > 0 && nowMs - g[i].ms <= kKeepSec * 1000 && std::isfinite(g[i].khz)) gs->push_back({ i, g[i].ms, g[i].khz });
        }
        return out;
    }
    /** Forget everything (tests). */
    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        mem_.clear();
        if (!dir_.empty()) ::unlink((dir_ + "/" + kFile).c_str());
    }

private:
    /** ★ One locked access: the file (flock, made at its full size, a wrong magic / version / size re-made empty) or the
     *  memory copy (the mutex). */
    struct Io {
        Table& t; bool shared; int fd = -1; bool mem = false; std::unique_lock<std::mutex> lk;
        Io(Table& tt, bool sh) : t(tt), shared(sh), lk(tt.m_) {
            if (t.dir_.empty()) {
                mem = true;
                if (t.mem_.size() != kFileBytes) { t.mem_.assign(kFileBytes, 0); Header h; std::memcpy(t.mem_.data(), &h, sizeof h); }
                return;
            }
            lk.unlock();   // ★ the file's own lock serialises writers across processes; the mutex only guards dir_ / mem_
            const std::string p = t.dir_ + "/" + kFile;
            fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) return;
            ::flock(fd, LOCK_EX);   // ★ the check-and-make below needs it exclusive; a reader downgrades after
            struct stat st{};
            Header h{};
            const bool good = ::fstat(fd, &st) == 0 && (uint64_t)st.st_size == kFileBytes && ::pread(fd, &h, sizeof h, 0) == (ssize_t)sizeof h
                              && h.magic == kMagic && h.version == kVersion && h.cap == kCap && h.recSize == kRecSize;
            if (!good) {
                if (::ftruncate(fd, 0) != 0 || ::ftruncate(fd, (off_t)kFileBytes) != 0) { ::flock(fd, LOCK_UN); ::close(fd); fd = -1; return; }
                Header nh;
                if (::pwrite(fd, &nh, sizeof nh, 0) != (ssize_t)sizeof nh) { ::flock(fd, LOCK_UN); ::close(fd); fd = -1; return; }
            }
            if (shared) ::flock(fd, LOCK_SH);
        }
        ~Io() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
        bool ok() const { return mem || fd >= 0; }
        void readRaw(uint64_t at, void* p, size_t n) {
            if (mem) { std::memcpy(p, t.mem_.data() + at, n); return; }
            char* c = (char*)p; size_t got = 0;
            while (got < n) { const ssize_t k = ::pread(fd, c + got, n - got, (off_t)(at + got)); if (k <= 0) break; got += (size_t)k; }
            if (got < n) std::memset(c + got, 0, n - got);
        }
        void read(uint64_t at, Rec* recs, size_t bytes, bool) {
            // records are kRecSize apart on disk, sizeof(Rec) in memory
            std::vector<char> raw(bytes);
            readRaw(at, raw.data(), bytes);
            for (uint32_t i = 0; i < kCap; ++i) std::memcpy(&recs[i], raw.data() + (size_t)i * kRecSize, sizeof(Rec));
        }
        void write(uint64_t at, const void* p, size_t n) {
            if (mem) { std::memcpy(t.mem_.data() + at, p, n); return; }
            const char* c = (const char*)p; size_t put = 0;
            while (put < n) { const ssize_t k = ::pwrite(fd, c + put, n - put, (off_t)(at + put)); if (k <= 0) break; put += (size_t)k; }
        }
    };
    std::mutex m_;
    std::string dir_;
    std::vector<char> mem_;
};
inline Table& table() { static Table t; return t; }

// ── The JSON the AVIATION MAP reads (GET /vibeserver/aircraft.json) ────────────────────────────────────────────────
inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20 || c == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}
/** `labelName(label)` → the plain-words meaning (vibe_avi_service.h's table — passed in so this header stands alone). */
inline std::string toJson(const std::vector<Row>& rows, const std::vector<GsHeard>& gs, int64_t nowMs,
                          const std::function<std::string(const std::string&)>& labelName = nullptr,
                          const std::function<bool(int, std::string&, double&, double&)>& gsInfo = nullptr) {
    char b[256];
    std::snprintf(b, sizeof b, "{\"now\":%lld,\"count\":%zu,\"aircraft\":[", (long long)(nowMs / 1000), rows.size());
    std::string o = b;
    bool first = true;
    for (const Row& w : rows) {
        o += first ? "{" : ",{"; first = false;
        if (w.icao) std::snprintf(b, sizeof b, "\"key\":\"%06X\",\"icao\":\"%06X\"", w.icao, w.icao);
        else if (!w.reg.empty()) std::snprintf(b, sizeof b, "\"key\":\"r:%s\"", jsonEsc(regKey(w.reg)).c_str());
        else std::snprintf(b, sizeof b, "\"key\":\"f:%s\"", jsonEsc(flightKey(w.flight)).c_str());
        o += b;
        auto str = [&](const char* k, const std::string& v) { if (!v.empty()) { o += ",\""; o += k; o += "\":\""; o += jsonEsc(v); o += '"'; } };
        str("reg", w.reg); str("flight", w.flight); str("type", w.type);
        if (w.flags & 1) {
            std::snprintf(b, sizeof b, ",\"lat\":%.5f,\"lon\":%.5f,\"posAge\":%lld", w.lat, w.lon, (long long)std::max<int64_t>(0, (nowMs - w.posMs) / 1000));
            o += b;
            if (w.posSrc >= 0) str("posSrc", srcName(w.posSrc));
            if (w.flags & 2) { std::snprintf(b, sizeof b, ",\"alt\":%d", w.altFt); o += b; }
            if (w.flags & 4) { std::snprintf(b, sizeof b, ",\"trk\":%.0f", (double)w.trk); o += b; }
            if (w.flags & 8) { std::snprintf(b, sizeof b, ",\"gs\":%.0f", (double)w.gs); o += b; }
            if (w.flags & 16) o += ",\"ground\":1";
            str("posTime", w.posTime);
        } else if (w.flags & 2) { std::snprintf(b, sizeof b, ",\"alt\":%d", w.altFt); o += b; }
        if (!w.from.empty()) { str("from", w.from); str("to", w.to); }
        if (!w.label.empty()) { str("label", w.label); if (labelName) str("labelName", labelName(w.label)); }
        str("app", w.app); str("via", w.via);
        o += ",\"src\":[";
        bool f1 = true;
        for (int s = 0; s < kNSrc; ++s) if ((w.srcMask >> s) & 1) { o += f1 ? "\"" : ",\""; o += srcName(s); o += '"'; f1 = false; }
        o += "],\"srcAge\":{";
        f1 = true;
        for (int s = 0; s < kNSrc; ++s) if ((w.srcMask >> s) & 1) {
            std::snprintf(b, sizeof b, "%s\"%s\":%lld", f1 ? "" : ",", srcName(s), (long long)std::max<int64_t>(0, (nowMs - w.srcMs[s]) / 1000));
            o += b; f1 = false;
        }
        std::snprintf(b, sizeof b, "},\"age\":%lld,\"n\":%u}", (long long)std::max<int64_t>(0, (nowMs - w.lastMs) / 1000), w.count);
        o += b;
    }
    o += "],\"stations\":[";
    first = true;
    for (const auto& g : gs) {
        std::string name; double lat = NAN, lon = NAN;
        if (!gsInfo || !gsInfo(g.id, name, lat, lon)) continue;
        std::snprintf(b, sizeof b, "%s{\"id\":%d,\"name\":\"%s\",\"lat\":%.1f,\"lon\":%.1f,\"khz\":%.0f,\"age\":%lld}", first ? "" : ",", g.id,
                      jsonEsc(name).c_str(), lat, lon, g.khz, (long long)std::max<int64_t>(0, (nowMs - g.ms) / 1000));
        o += b; first = false;
    }
    return o + "]}";
}

inline int64_t wallMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

/** ★★ THE SHARER: one thread per radio process. Every kFlushSec it asks each provider for what changed since its last
 *  flush (ADS-B's aircraft, the datalink table's rows — each under its own lock, briefly) and writes them, the decoders
 *  never waiting on the file. Bounded: a provider's batch is at most kCap updates. */
class Sharer {
public:
    using Provider = std::function<void(int64_t sinceMs, std::vector<Update>& out, std::vector<GsHeard>& gs)>;
    Sharer() { (void)table(); }   // ★ the table is made first, so it outlives this thread at exit
    ~Sharer() { stop(); }
    /** Register (or replace) a provider by name: "adsb", "avi". */
    void provide(const std::string& name, Provider p) {
        { std::lock_guard<std::mutex> lk(m_); prov_[name] = { std::move(p), 0 }; }
        start_();
    }
    void remove(const std::string& name) { std::lock_guard<std::mutex> lk(m_); prov_.erase(name); }
    /** Write now (a test; a mode leaving, so its last updates are not lost). Never from a DSP thread. */
    void flushNow() { std::lock_guard<std::mutex> fl(flushMtx_); flush_(); }
    void stop() {
        { std::lock_guard<std::mutex> lk(m_); run_ = false; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

private:
    void start_() {
        std::lock_guard<std::mutex> lk(m_);
        if (run_) return;
        run_ = true;
        th_ = std::thread([this] {
            vibeDecoderThread("vibe-avshare");   // ★ decoder priority (vibe_thread.h): below network, audio and spectrum
            std::unique_lock<std::mutex> lk2(m_);
            while (run_) {
                cv_.wait_for(lk2, std::chrono::milliseconds((int)(kFlushSec * 1000)), [this] { return !run_; });
                if (!run_) break;
                lk2.unlock();
                { std::lock_guard<std::mutex> fl(flushMtx_); flush_(); }
                lk2.lock();
            }
        });
    }
    void flush_() {
        std::vector<std::pair<std::string, Provider>> ps;
        std::map<std::string, int64_t> since;
        { std::lock_guard<std::mutex> lk(m_); for (auto& kv : prov_) { ps.push_back({ kv.first, kv.second.fn }); since[kv.first] = kv.second.lastMs; } }
        const int64_t now = wallMs();
        std::vector<Update> ups;
        std::vector<GsHeard> gs;
        for (auto& p : ps) {
            std::vector<Update> u;
            // ★ a little overlap: an update stamped just before the last flush's clock read is not lost
            p.second(since[p.first] > 0 ? since[p.first] - 500 : 0, u, gs);
            if (u.size() > kCap) u.resize(kCap);
            ups.insert(ups.end(), std::make_move_iterator(u.begin()), std::make_move_iterator(u.end()));
        }
        { std::lock_guard<std::mutex> lk(m_); for (auto& p : ps) { auto it = prov_.find(p.first); if (it != prov_.end()) it->second.lastMs = now; } }
        if (ups.empty() && gs.empty()) return;
        table().write(ups, gs, now);
    }
    struct P { Provider fn; int64_t lastMs = 0; };
    std::mutex m_, flushMtx_;
    std::condition_variable cv_;
    std::map<std::string, P> prov_;
    bool run_ = false;
    std::thread th_;
};
inline Sharer& sharer() { static Sharer s; return s; }

}  // namespace vibeavshare
