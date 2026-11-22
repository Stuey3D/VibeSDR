// vibe_station_store.h — V12 beta90 THE STATION STORE: every station the digital-spots engines, the CW / PSK / RTTY
// skimmers and decoders heard — background decoding AND listeners' live decoders — ONE ROW PER CALL+MODE+BAND PER UTC DAY,
// for the DIGI MODES MAP (and the PSK Reporter upload, which hangs on subscribe()). docs/v12/DIGI-MODES-MAP.md.
//
// ★★★ WHY (Stuart, 2026-11-21): background decoding kept only a SUMMARY (vibe_bgdecode.h Ft8SummaryTap — the counts, the
//     furthest, the countries) and a listener's spots lived only in their own browser. "Background + live digital spots on
//     one map": the stations have to be kept where every radio on the machine can read them, by the day.
//
// ── The file: <gallery>/stations/YYYYMMDD.dat (one per UTC day) ─────────────────────────────────────────────────────
//   header (64 B) · records (n × 64 B, fixed size, appended)
//   ★★ BOUNDED BY CONSTRUCTION: at most dayCap() rows a day (3072–32768 by the machine's memory, 196 KB–2 MB) — see dayCapForRam.
//      A full day refuses new rows (counted in the header's `dropped`) and keeps updating the ones it has.
//      Days are pruned oldest first: at most kKeepDays files and never more than budgetFor(gallery size) bytes
//      (a 16th of the owner's gallery space, 1–64 MB), counted inside the gallery's size (vibe_gallery.h sideBytes).
//   ★★ WRITTEN INCREMENTALLY, OFF THE DECODE THREADS: a spot is a RAM map insert under a mutex (spot()); one flusher
//      thread per process (decoder priority) writes what changed every kFlushSec — ONE pread of the day's file to find
//      the rows, then ONE pwrite per changed row and an append per new one. Never per spot; a Pi 2's SD card is the target.
//   ★★ EVERY RADIO ON THE MACHINE: each radio is its own process (vibeserver/main.cpp superviseRadios); the flush is under
//      flock(the day's file), merging COUNTS AS DELTAS and every other field idempotently (min first, max last, max SNR,
//      the newest reception's grid / SNR / frequency), so two radios hearing one station make one row.
//   ★ NO GALLERY (the Android app's own radio, a test, an owner who switched the gallery off): the same rows in RAM only,
//     today's and yesterday's, never written.
// ── Read: mapJson() → the map's JSON — ONE ENTRY PER CALL over a window (1 h · 24 h · 7 d · 30 d · all) ─────────────
//   ★ Bounded: at most kMaxOut calls (newest days first), each file read with one pread under a shared lock, the
//     aggregation of the days before today cached (they no longer change) so a live poll every 15 s re-reads one file.
// ⚖ What is in it: callsigns, Maidenhead grids, modes, bands, SNRs and times — what an amateur station sends to be heard
//   by everyone, the same fields every public spotting network shows. No message text.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vibe_callsign_country.h"
#include "vibe_thread.h"

namespace vibestations {

enum Origin : uint8_t { kBackground = 1, kLive = 2, kSatellite = 4 };

static constexpr uint32_t kMagic = 0x31545356;     // "VST1"
static constexpr uint32_t kRecSize = 64, kHeader = 64;
/** ★★★ ROWS A DAY, BY THE MACHINE'S MEMORY (V12 beta91, 2026-11-22). A row is one call on one band in one mode. The cap
 *  was a flat 3072 ("a busy 20 m day at night is ~1500 calls") — but a Band VFO on four bands with every mode is many
 *  times that: Stuart's Pi 500 filled its day by mid-morning on 2026-11-22 and REFUSED 49,331 spots after it, so the map
 *  showed 70 stations for an hour in which PSK Reporter was sent 769 ("the numbers of spots in our map is very low").
 *  ★ Sized by physical memory, because today's rows live in each radio process's RAM (~250 B a row with its strings):
 *    ≥ 4 GB → 32768 rows (2 MB a day on disk) · ≥ 2 GB → 16384 · ≥ 1 GB → 8192 · less → 3072 (the 256/512 MB boards keep
 *    the old cap). Disk stays bounded by the budget (budgetFor) and kKeepDays — older days are pruned first.
 *  ★ A day file written under a larger cap is read up to this machine's cap (readDay) — never past it. */
inline uint32_t dayCapForRam(uint64_t ramBytes) {
    const uint64_t g = 1ull << 30;
    return ramBytes >= 4 * g ? 32768u : ramBytes >= 2 * g ? 16384u : ramBytes >= g ? 8192u : 3072u;
}
inline uint32_t& dayCapRef() {
    static uint32_t cap = [] {
        const long pages = ::sysconf(_SC_PHYS_PAGES), page = ::sysconf(_SC_PAGESIZE);
        return dayCapForRam(pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0);
    }();
    return cap;
}
/** Rows a day on disk and in one process's RAM (dayCapForRam). */
inline uint32_t dayCap() { return dayCapRef(); }
/** Tests only: pin the cap (the machine running them is not the one being modelled). */
inline void setDayCapForTest(uint32_t n) { dayCapRef() = n; }
static constexpr int kKeepDays = 400;
static constexpr size_t kMaxOut = 5000;            ///< calls in one map answer
static constexpr int kFlushSec = 20;
static constexpr int64_t kDaySec = 86400;

/** ONE RECEPTION, as a decoder heard it (what PSK Reporter uploads). */
struct Reception {
    std::string call, grid, mode, band;
    double  rfHz = 0;
    int     snr = 0;
    int64_t utc = 0;          ///< UTC seconds (FT8-family: the slot's start); 0 = now
    uint8_t origin = 0;
    bool    newToday = false; ///< set by the store: the first reception of this call+mode+band today (this process)
};

/** ONE STATION ROW: a call+mode+band over one UTC day. */
struct Station {
    std::string call, grid, mode, band;
    double  freqHz = 0;
    int     bestSnr = -99, lastSnr = 0;
    uint32_t count = 0;
    int64_t first = 0, last = 0;
    uint8_t origins = 0;
    int64_t day = 0;          ///< the UTC day (seconds at 00:00)
};

// ── validation (allow-lists: every field came off the air) ─────────────────────────────────────────────────────────
/** A callsign: '/'-parts of A–Z 0–9, the longest 3–10 long with a letter and a digit and ending in a letter; never an
 *  FT8 word. (The same rule as vibe_bgdecode.h ft8IsCallsign — the summary and the map must agree on who is a station.) */
inline bool validCall(const std::string& in, std::string* out = nullptr) {
    if (in.empty() || in.size() > 15) return false;
    std::string c;
    for (char ch : in) {
        const char u = (char)std::toupper((unsigned char)ch);
        if (!((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '/')) return false;
        c += u;
    }
    static const char* const words[] = { "DE", "CQ", "QRZ", "TO", "TU", "TNX", "RR73", "RRR", "73", "DX", "TEST" };
    for (const char* w : words) if (c == w) return false;
    std::string best, part;
    auto take = [&] { if (part.size() > best.size()) best = part; part.clear(); };
    for (char ch : c) { if (ch == '/') take(); else part += ch; }
    take();
    if (best.size() < 3 || best.size() > 10) return false;
    bool digit = false, letter = false;
    for (char ch : best) { if (ch >= '0' && ch <= '9') digit = true; else letter = true; }
    if (!digit || !letter || !(best.back() >= 'A' && best.back() <= 'Z')) return false;
    if (out) *out = c;
    return true;
}
/** A grid: Maidenhead AA00–RR99 (+ aa–xx), never RR73 (a sign-off) nor AA00 (a placeholder). Upper-cased into `out`. */
inline bool validGrid(const std::string& g, std::string* out = nullptr) {
    if (g.size() != 4 && g.size() != 6) return false;
    std::string u;
    for (char ch : g) u += (char)std::toupper((unsigned char)ch);
    if (!(u[0] >= 'A' && u[0] <= 'R' && u[1] >= 'A' && u[1] <= 'R' && u[2] >= '0' && u[2] <= '9' && u[3] >= '0' && u[3] <= '9')) return false;
    if (u.size() == 6 && !(u[4] >= 'A' && u[4] <= 'X' && u[5] >= 'A' && u[5] <= 'X')) return false;
    if (u == "RR73" || u == "AA00") return false;
    if (out) *out = u;
    return true;
}
/** The grid square's centre. */
inline bool gridCentre(const std::string& g, double& lat, double& lon) {
    std::string u;
    if (!validGrid(g, &u)) return false;
    lon = (u[0] - 'A') * 20.0 - 180.0 + (u[2] - '0') * 2.0;
    lat = (u[1] - 'A') * 10.0 - 90.0 + (u[3] - '0') * 1.0;
    if (u.size() == 6) { lon += (u[4] - 'A') * (2.0 / 24.0) + 1.0 / 24.0; lat += (u[5] - 'A') * (1.0 / 24.0) + 0.5 / 24.0; }
    else { lon += 1.0; lat += 0.5; }
    return true;
}
/** A mode or band word: A–Z a–z 0–9 + -, at most `cap`. Modes upper-cased; "BPSK31" → "PSK31". */
inline std::string cleanWord(const std::string& in, size_t cap, bool upper) {
    std::string o;
    for (char ch : in) {
        if (o.size() >= cap) break;
        const unsigned char u = (unsigned char)ch;
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '+' || u == '-')
            o += upper ? (char)std::toupper(u) : (char)u;
    }
    return o;
}
inline std::string cleanMode(const std::string& m) {
    std::string o = cleanWord(m, 8, true);
    if (o.rfind("BPSK", 0) == 0) o = o.substr(1);
    return o;
}
inline int64_t dayOf(int64_t utc) { return utc >= 0 ? utc / kDaySec * kDaySec : 0; }
inline std::string dayName(int64_t day) {
    const time_t t = (time_t)day; struct tm tm{};
    gmtime_r(&t, &tm);
    char b[16]; std::snprintf(b, sizeof b, "%04d%02d%02d.dat", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return b;
}
/** The day a file name stands for, -1 if it is not one of ours. */
inline int64_t dayFromName(const char* n) {
    int y, mo, d; char tail[8] = {};
    if (std::strlen(n) != 12 || std::sscanf(n, "%4d%2d%2d.%3s", &y, &mo, &d, tail) != 4 || std::strcmp(tail, "dat") != 0) return -1;
    struct tm tm{}; tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    return (int64_t)timegm(&tm);
}
/** The bytes the store may take in a gallery of `limitBytes`: a 16th, 1–64 MB. */
inline uint64_t budgetFor(uint64_t limitBytes) {
    return std::max<uint64_t>(1ull << 20, std::min<uint64_t>(64ull << 20, limitBytes / 16));
}

// ── the record on disk ─────────────────────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct Rec {
    char call[16]; char grid[8]; char mode[8]; char band[8];
    uint32_t first, last, count, freqHz;
    int8_t best, lastSnr; uint8_t origins, ver;
    uint8_t pad[4];
};
struct Hdr { uint32_t magic, recSize, n, dropped; int64_t day; uint8_t pad[40]; };
#pragma pack(pop)
static_assert(sizeof(Rec) == kRecSize, "Rec is 64 bytes");
static_assert(sizeof(Hdr) == kHeader, "Hdr is 64 bytes");

inline void putStr(char* dst, size_t cap, const std::string& s) { std::memset(dst, 0, cap); std::memcpy(dst, s.data(), std::min(cap - 1, s.size())); }
inline std::string getStr(const char* src, size_t cap) { size_t n = 0; while (n < cap && src[n]) ++n; return std::string(src, n); }
inline int8_t clampSnr(int v) { return (int8_t)std::max(-99, std::min(99, v)); }
inline std::string keyOf(const std::string& call, const std::string& mode, const std::string& band) { return call + '\x1f' + mode + '\x1f' + band; }

inline Station fromRec(const Rec& r, int64_t day) {
    Station s;
    s.call = getStr(r.call, 16); s.grid = getStr(r.grid, 8); s.mode = getStr(r.mode, 8); s.band = getStr(r.band, 8);
    s.first = r.first; s.last = r.last; s.count = r.count; s.freqHz = r.freqHz;
    s.bestSnr = r.best; s.lastSnr = r.lastSnr; s.origins = r.origins; s.day = day;
    return s;
}
inline Rec toRec(const Station& s) {
    Rec r{};
    putStr(r.call, 16, s.call); putStr(r.grid, 8, s.grid); putStr(r.mode, 8, s.mode); putStr(r.band, 8, s.band);
    r.first = (uint32_t)std::max<int64_t>(0, s.first); r.last = (uint32_t)std::max<int64_t>(0, s.last);
    r.count = s.count; r.freqHz = (uint32_t)std::max(0.0, std::min(4.0e9, s.freqHz));
    r.best = clampSnr(s.bestSnr); r.lastSnr = clampSnr(s.lastSnr); r.origins = s.origins; r.ver = 1;
    return r;
}
/** Merge `d` (carrying `deltaCount` new receptions) into `into`: counts add, every other field is idempotent. */
inline void mergeInto(Station& into, const Station& d, uint32_t deltaCount) {
    into.count += deltaCount;
    if (!into.first || (d.first && d.first < into.first)) into.first = d.first;
    if (d.last >= into.last) {
        into.last = d.last;
        if (!d.grid.empty()) into.grid = d.grid;
        into.lastSnr = d.lastSnr; if (d.freqHz > 0) into.freqHz = d.freqHz;
    } else if (into.grid.empty() && !d.grid.empty()) into.grid = d.grid;
    into.bestSnr = std::max(into.bestSnr, d.bestSnr);
    into.origins |= d.origins;
}

/** One day's file, read whole under a shared lock. False when it does not exist or is not ours. */
inline bool readDay(const std::string& path, int64_t day, std::vector<Station>& out, uint32_t* dropped = nullptr) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ::flock(fd, LOCK_SH);
    Hdr h{};
    bool ok = ::pread(fd, &h, sizeof h, 0) == (ssize_t)sizeof h && h.magic == kMagic && h.recSize == kRecSize;
    if (ok) {
        const uint32_t n = std::min(h.n, dayCap());
        std::vector<Rec> recs(n);
        const ssize_t want = (ssize_t)(n * sizeof(Rec));
        ok = n == 0 || ::pread(fd, recs.data(), (size_t)want, kHeader) == want;
        if (ok) { out.reserve(out.size() + n); for (const auto& r : recs) out.push_back(fromRec(r, day)); }
        if (dropped) *dropped = h.dropped;
    }
    ::flock(fd, LOCK_UN);
    ::close(fd);
    return ok;
}

/** ONE CALL over a window — what the map draws. */
struct MapRow {
    std::string call, grid, mode, band;
    double freqHz = 0;
    uint32_t count = 0;
    int bestSnr = -99, lastSnr = 0;
    int64_t first = 0, last = 0, gridAt = 0;
    uint8_t origins = 0;
    std::vector<std::string> modes, bands;   ///< ≤ 8 each
};
inline void aggregate(std::unordered_map<std::string, MapRow>& by, const Station& s, size_t cap) {
    auto it = by.find(s.call);
    if (it == by.end()) {
        if (by.size() >= cap) return;
        it = by.emplace(s.call, MapRow{}).first;
        it->second.call = s.call;
    }
    MapRow& a = it->second;
    a.count += s.count;
    if (!a.first || (s.first && s.first < a.first)) a.first = s.first;
    if (s.last >= a.last) { a.last = s.last; a.mode = s.mode; a.band = s.band; a.freqHz = s.freqHz; a.lastSnr = s.lastSnr; }
    if (!s.grid.empty() && s.last >= a.gridAt) { a.grid = s.grid; a.gridAt = s.last; }
    a.bestSnr = std::max(a.bestSnr, s.bestSnr);
    a.origins |= s.origins;
    if (!s.mode.empty() && a.modes.size() < 8 && std::find(a.modes.begin(), a.modes.end(), s.mode) == a.modes.end()) a.modes.push_back(s.mode);
    if (!s.band.empty() && a.bands.size() < 8 && std::find(a.bands.begin(), a.bands.end(), s.band) == a.bands.end()) a.bands.push_back(s.band);
}

class Store {
public:
    using Listener = std::function<void(const Reception&)>;
    /** `clock`: UTC seconds (tests); null = the wall clock. `flusher`: false = no thread (tests call flush()). */
    explicit Store(std::function<int64_t()> clock = nullptr, bool flusher = true) : clock_(std::move(clock)), flusher_(flusher) {}
    ~Store() { stop(); }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /** Where the days are kept (the gallery's directory — the store adds "/stations") and the gallery's size. Asked at
     *  every flush, so the owner switching the gallery on, off or moving it takes effect. Empty dir = RAM only. */
    void configure(std::function<std::string()> dir, std::function<uint64_t()> limitBytes) {
        std::lock_guard<std::mutex> lk(cfgMtx_);
        dirFn_ = std::move(dir); limitFn_ = std::move(limitBytes);
    }
    /** ★ The owner's switch ("spotsmap" under Digital spots): off = nothing kept, nothing served. */
    void setEnabled(bool on) { enabled_.store(on); }
    bool enabled() const { return enabled_.load(); }

    /** Feed one reception (a decode thread). Invalid calls / grids are dropped (false). */
    bool spot(const Reception& in) {
        if (!enabled_.load()) return false;
        Reception r = in;
        std::string call, grid;
        if (!validCall(r.call, &call)) return false;
        r.call = call;
        r.grid = validGrid(r.grid, &grid) ? grid : std::string();
        r.mode = cleanMode(r.mode);
        if (r.mode.empty()) return false;
        r.band = cleanWord(r.band, 7, false);
        const int64_t now = this->now();
        if (r.utc <= 0 || r.utc > now + 300 || r.utc < now - 2 * kDaySec) r.utc = now;
        {
            std::lock_guard<std::mutex> lk(m_);
            const int64_t day = dayOf(r.utc);
            Table& t = days_[day];
            const std::string k = keyOf(r.call, r.mode, r.band);
            auto it = t.rows.find(k);
            if (it == t.rows.end()) {
                if (t.rows.size() >= dayCap()) { ++t.droppedRam; return false; }
                Row row;
                row.s.call = r.call; row.s.mode = r.mode; row.s.band = r.band; row.s.day = day; row.s.first = r.utc;
                it = t.rows.emplace(k, std::move(row)).first;
                r.newToday = true;
            }
            Station& s = it->second.s;
            Station d; d.first = r.utc; d.last = r.utc; d.grid = r.grid; d.lastSnr = r.snr; d.bestSnr = r.snr;
            d.freqHz = r.rfHz; d.origins = r.origin;
            mergeInto(s, d, 1);
            it->second.dirty = true;
            ++spots_;
            // ★ RAM only: today and yesterday. With a directory, older days go once flushed (flush_()).
            while (days_.size() > 2 && days_.begin()->first < day && (!haveDir_.load() || days_.begin()->second.clean())) days_.erase(days_.begin());
        }
        startFlusher_();
        std::vector<std::pair<int, Listener>> ls;
        { std::lock_guard<std::mutex> lk(lsMtx_); ls = listeners_; }
        for (auto& l : ls) if (l.second) l.second(r);
        return true;
    }
    int subscribe(Listener fn) { std::lock_guard<std::mutex> lk(lsMtx_); const int id = ++nextId_; listeners_.push_back({ id, std::move(fn) }); return id; }
    void unsubscribe(int id) {
        std::lock_guard<std::mutex> lk(lsMtx_);
        listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [id](const std::pair<int, Listener>& p) { return p.first == id; }), listeners_.end());
    }

    /** Write what changed now (the flusher every kFlushSec; a test; shutdown). Never from a decode or DSP thread. */
    void flush() { std::lock_guard<std::mutex> fl(flushMtx_); flush_(); }
    void stop() {
        { std::lock_guard<std::mutex> lk(thMtx_); run_ = false; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        flush();
    }

    /** Every row heard at or after `sinceUtc`, disk + this process's unflushed part, newest day first. */
    size_t forEachSince(int64_t sinceUtc, const std::function<bool(const Station&)>& fn) const {
        const int64_t today = dayOf(now());
        size_t n = 0;
        const int64_t from = dayOf(std::max<int64_t>(0, sinceUtc));
        const int64_t oldest = std::max(from, today - (int64_t)kKeepDays * kDaySec);
        for (int64_t day = today; day >= oldest; day -= kDaySec) {
            std::vector<Station> v;
            dayRows_(day, v);
            for (const auto& s : v) {
                if (s.last < sinceUtc) continue;
                ++n;
                if (!fn(s)) return n;
            }
        }
        return n;
    }

    /** ★ THE MAP'S ANSWER. `window`: seconds back (0 = all); `changedSince`: only calls heard at or after it (a live
     *  poll), 0 = every call in the window. `lat`/`lon`: this server (NaN = unknown) for "km". */
    std::string mapJson(int64_t window, int64_t changedSince, double lat, double lon, size_t maxOut = kMaxOut) const {
        const int64_t t = now(), today = dayOf(t);
        if (!enabled_.load()) return "{\"now\":" + std::to_string((long long)t) + ",\"count\":0,\"stations\":[],\"off\":1}";
        // ★ 7 days and longer start at a day boundary: the day files are the unit (and the days before today are cached).
        int64_t since = window > 0 ? t - window : 0;
        if (window >= 7 * kDaySec) since = dayOf(since);
        std::unordered_map<std::string, MapRow> by;
        by.reserve(256);
        size_t rowsRead = 0;
        // today (and its unflushed rows) first: it is what changes, and the newest win the cap
        { std::vector<Station> v; dayRows_(today, v); rowsRead += v.size(); for (const auto& s : v) if (s.last >= since) aggregate(by, s, maxOut); }
        if (since < today) pastDays_(since, today, by, maxOut, rowsRead);
        // ★ newest first, then the cap
        std::vector<const MapRow*> rows;
        rows.reserve(by.size());
        for (const auto& kv : by) if (kv.second.last >= changedSince) rows.push_back(&kv.second);
        std::sort(rows.begin(), rows.end(), [](const MapRow* a, const MapRow* b) { return a->last != b->last ? a->last > b->last : a->call < b->call; });
        std::map<std::string, unsigned> byMode;
        for (const auto& kv : by) byMode[kv.second.mode]++;
        std::string o;
        o.reserve(256 + rows.size() * 200);
        o += "{\"now\":" + std::to_string((long long)t) + ",\"window\":" + std::to_string((long long)window)
           + ",\"since\":" + std::to_string((long long)since) + ",\"total\":" + std::to_string(by.size())
           + ",\"count\":" + std::to_string(rows.size()) + ",\"truncated\":" + (by.size() >= maxOut ? "1" : "0")
           + ",\"byMode\":{";
        bool first = true;
        for (const auto& kv : byMode) { if (!first) o += ','; first = false; o += "\"" + kv.first + "\":" + std::to_string(kv.second); }
        o += "},\"stations\":[";
        char b[512];
        first = true;
        for (const MapRow* a : rows) {
            double la = NAN, lo = NAN;
            const bool placed = gridCentre(a->grid, la, lo);
            double km = NAN;
            if (placed && std::isfinite(lat) && std::isfinite(lon)) km = gcKm(lat, lon, la, lo);
            std::string ctry;
            for (char ch : vibe::callsign::country(a->call)) { const unsigned char u = (unsigned char)ch; if (u >= 0x20 && u < 0x7f && u != '"' && u != '\\' && ctry.size() < 40) ctry += ch; }
            std::string modes, bands;
            for (const auto& m : a->modes) { if (!modes.empty()) modes += ','; modes += "\"" + m + "\""; }
            for (const auto& m : a->bands) { if (!bands.empty()) bands += ','; bands += "\"" + m + "\""; }
            int n = std::snprintf(b, sizeof b,
                "%s{\"call\":\"%s\",\"grid\":\"%s\",\"mode\":\"%s\",\"band\":\"%s\",\"freq\":%.0f,\"count\":%u,\"snr\":%d,\"lastSnr\":%d,"
                "\"first\":%lld,\"last\":%lld,\"src\":%u,\"modes\":[%s],\"bands\":[%s],\"country\":\"%s\"",
                first ? "" : ",", a->call.c_str(), a->grid.c_str(), a->mode.c_str(), a->band.c_str(), a->freqHz, a->count, a->bestSnr,
                a->lastSnr, (long long)a->first, (long long)a->last, (unsigned)a->origins, modes.c_str(), bands.c_str(), ctry.c_str());
            if (n <= 0) continue;
            o.append(b, std::min((size_t)n, sizeof b - 1));
            if (placed) { n = std::snprintf(b, sizeof b, ",\"lat\":%.4f,\"lon\":%.4f", la, lo); o.append(b, (size_t)n); }
            if (std::isfinite(km)) { n = std::snprintf(b, sizeof b, ",\"km\":%.0f", km); o.append(b, (size_t)n); }
            o += '}';
            first = false;
        }
        o += "]}";
        lastRowsRead_.store(rowsRead);
        return o;
    }

    // ── for the tests and the admin line ──
    uint64_t spots() const { std::lock_guard<std::mutex> lk(m_); return spots_; }
    size_t lastRowsRead() const { return lastRowsRead_.load(); }
    std::string dayPath(int64_t day) const { const std::string d = storeDir_(); return d.empty() ? std::string() : d + "/" + dayName(day); }
    /** Bytes the day files take (the gallery's side store count). */
    static uint64_t bytesIn(const std::string& galleryDir) {
        if (galleryDir.empty()) return 0;
        const std::string d = galleryDir + "/stations";
        uint64_t n = 0;
        if (DIR* dp = ::opendir(d.c_str())) {
            while (dirent* e = ::readdir(dp)) {
                if (dayFromName(e->d_name) < 0) continue;
                struct stat st{};
                if (::stat((d + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) n += (uint64_t)st.st_size;
            }
            ::closedir(dp);
        }
        return n;
    }

private:
    struct Row { Station s; uint32_t flushed = 0; bool dirty = false; };
    struct Job { int64_t day; std::string key; Station s; uint32_t delta; };
    struct Table {
        std::unordered_map<std::string, Row> rows;
        uint32_t droppedRam = 0;
        bool clean() const { for (const auto& kv : rows) if (kv.second.dirty) return false; return true; }
    };
    int64_t now() const { return clock_ ? clock_() : (int64_t)std::time(nullptr); }
    static double gcKm(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180.0;
        const double a = std::sin((la2 - la1) * r / 2), c = std::sin((lo2 - lo1) * r / 2);
        const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * c * c;
        return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(h)));
    }
    std::string galleryDir_() const { std::lock_guard<std::mutex> lk(cfgMtx_); return dirFn_ ? dirFn_() : std::string(); }
    uint64_t limit_() const { std::lock_guard<std::mutex> lk(cfgMtx_); return limitFn_ ? limitFn_() : 0; }
    std::string storeDir_() const { const std::string g = galleryDir_(); return g.empty() ? std::string() : g + "/stations"; }

    /** One day's rows: the file (every radio's flushed rows) with this process's unflushed part laid over it. */
    void dayRows_(int64_t day, std::vector<Station>& out) const {
        const std::string dir = storeDir_();
        if (!dir.empty()) readDay(dir + "/" + dayName(day), day, out);
        std::lock_guard<std::mutex> lk(m_);
        auto t = days_.find(day);
        if (t == days_.end()) return;
        std::unordered_map<std::string, size_t> idx;
        idx.reserve(out.size());
        for (size_t i = 0; i < out.size(); ++i) idx[keyOf(out[i].call, out[i].mode, out[i].band)] = i;
        for (const auto& kv : t->second.rows) {
            const Row& r = kv.second;
            const uint32_t delta = dir.empty() ? r.s.count : r.s.count - std::min(r.s.count, r.flushed);
            auto f = idx.find(kv.first);
            if (f != idx.end()) mergeInto(out[f->second], r.s, delta);
            else if (delta > 0 || dir.empty()) { Station s = r.s; s.count = delta; out.push_back(std::move(s)); }
        }
    }
    /** The days before today, aggregated per call — cached while none of their files changed. */
    void pastDays_(int64_t since, int64_t today, std::unordered_map<std::string, MapRow>& by, size_t maxOut, size_t& rowsRead) const {
        const std::string dir = storeDir_();
        const int64_t from = std::max(dayOf(since), today - (int64_t)kKeepDays * kDaySec);
        // the signature: each file's size + mtime (and this process's RAM rows of those days)
        std::string sig = dir + "|" + std::to_string((long long)since) + "|" + std::to_string(maxOut);
        std::vector<int64_t> have;
        if (!dir.empty()) {
            if (DIR* dp = ::opendir(dir.c_str())) {
                while (dirent* e = ::readdir(dp)) { const int64_t d = dayFromName(e->d_name); if (d >= from && d < today) have.push_back(d); }
                ::closedir(dp);
            }
        }
        { std::lock_guard<std::mutex> lk(m_); for (const auto& kv : days_) if (kv.first >= from && kv.first < today && std::find(have.begin(), have.end(), kv.first) == have.end()) have.push_back(kv.first); }
        std::sort(have.begin(), have.end(), std::greater<int64_t>());
        for (int64_t d : have) {
            struct stat st{};
            const bool f = !dir.empty() && ::stat((dir + "/" + dayName(d)).c_str(), &st) == 0;
            sig += "|" + std::to_string((long long)d) + ":" + (f ? std::to_string((long long)st.st_size) + "." + std::to_string((long long)st.st_mtime) : "-");
        }
        { std::lock_guard<std::mutex> lk(m_); for (const auto& kv : days_) if (kv.first < today) sig += "|r" + std::to_string(kv.second.rows.size()) + (kv.second.clean() ? "c" : "d"); }
        std::unordered_map<std::string, MapRow> past;
        bool hit = false;
        {
            std::lock_guard<std::mutex> ck(cacheMtx_);
            if (cacheSig_ == sig) { past = cache_; hit = true; }
        }
        if (!hit) {
            for (int64_t d : have) {
                std::vector<Station> v;
                dayRows_(d, v);
                rowsRead += v.size();
                for (const auto& s : v) if (s.last >= since) aggregate(past, s, maxOut);
            }
            std::lock_guard<std::mutex> ck(cacheMtx_);
            cacheSig_ = sig; cache_ = past;
        }
        // ★ today's rows are already in `by` (and win); the past days add counts, modes, bands and an older grid
        for (const auto& kv : past) {
            auto it = by.find(kv.first);
            if (it == by.end()) { if (by.size() < maxOut) by.emplace(kv.first, kv.second); continue; }
            MapRow& a = it->second; const MapRow& p = kv.second;
            a.count += p.count;
            if (p.first && (!a.first || p.first < a.first)) a.first = p.first;
            if (a.grid.empty() && !p.grid.empty()) { a.grid = p.grid; a.gridAt = p.gridAt; }
            a.bestSnr = std::max(a.bestSnr, p.bestSnr);
            a.origins |= p.origins;
            for (const auto& m : p.modes) if (a.modes.size() < 8 && std::find(a.modes.begin(), a.modes.end(), m) == a.modes.end()) a.modes.push_back(m);
            for (const auto& m : p.bands) if (a.bands.size() < 8 && std::find(a.bands.begin(), a.bands.end(), m) == a.bands.end()) a.bands.push_back(m);
        }
    }

    void startFlusher_() {
        if (!flusher_) return;
        std::lock_guard<std::mutex> lk(thMtx_);
        if (run_ || started_) return;
        run_ = true; started_ = true;
        th_ = std::thread([this] {
            vibeDecoderThread("vibe-stations");   // ★ decoder priority: below network, audio and spectrum
            std::unique_lock<std::mutex> lk2(thMtx_);
            while (run_) {
                cv_.wait_for(lk2, std::chrono::seconds(kFlushSec), [this] { return !run_; });
                if (!run_) break;
                lk2.unlock();
                flush();
                lk2.lock();
            }
        });
    }

    /** The dirty rows of every day → their files, as deltas, under each file's lock. */
    void flush_() {
        const std::string dir = storeDir_();
        haveDir_.store(!dir.empty());
        if (dir.empty()) return;
        std::vector<Job> jobs;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (auto& d : days_)
                for (auto& kv : d.second.rows) {
                    Row& r = kv.second;
                    if (!r.dirty) continue;
                    jobs.push_back({ d.first, kv.first, r.s, r.s.count - std::min(r.s.count, r.flushed) });
                    r.flushed = r.s.count; r.dirty = false;   // ★ optimistic: put back below if the write fails
                }
        }
        if (jobs.empty()) return;
        ::mkdir(dir.c_str(), 0755);
        std::map<int64_t, std::vector<const Job*>> byDay;
        for (const auto& j : jobs) byDay[j.day].push_back(&j);
        std::vector<const Job*> failed;
        for (auto& dj : byDay) if (!writeDay_(dir + "/" + dayName(dj.first), dj.first, dj.second)) failed.insert(failed.end(), dj.second.begin(), dj.second.end());
        if (!failed.empty()) {
            std::lock_guard<std::mutex> lk(m_);
            for (const Job* j : failed) {
                auto d = days_.find(j->day); if (d == days_.end()) continue;
                auto r = d->second.rows.find(j->key); if (r == d->second.rows.end()) continue;
                r->second.flushed -= std::min(r->second.flushed, j->delta); r->second.dirty = true;
            }
        }
        {   // ★ a finished day that is all on disk leaves RAM
            std::lock_guard<std::mutex> lk(m_);
            const int64_t today = dayOf(now());
            for (auto it = days_.begin(); it != days_.end();) { if (it->first < today && it->second.clean()) it = days_.erase(it); else ++it; }
        }
        prune_(dir);
    }
    bool writeDay_(const std::string& path, int64_t day, const std::vector<const Job*>& jobs) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return false;
        ::flock(fd, LOCK_EX);
        bool ok = true;
        Hdr h{};
        if (::pread(fd, &h, sizeof h, 0) != (ssize_t)sizeof h || h.magic != kMagic || h.recSize != kRecSize) {
            h = Hdr{}; h.magic = kMagic; h.recSize = kRecSize; h.n = 0; h.day = day;
        }
        h.n = std::min(h.n, dayCap());
        std::vector<Rec> recs(h.n);
        if (h.n && ::pread(fd, recs.data(), h.n * sizeof(Rec), kHeader) != (ssize_t)(h.n * sizeof(Rec))) { recs.clear(); h.n = 0; }
        std::unordered_map<std::string, uint32_t> idx;
        idx.reserve(recs.size() + jobs.size());
        for (uint32_t i = 0; i < recs.size(); ++i) idx[keyOf(getStr(recs[i].call, 16), getStr(recs[i].mode, 8), getStr(recs[i].band, 8))] = i;
        for (const Job* j : jobs) {
            auto f = idx.find(j->key);
            Station s;
            uint32_t at;
            if (f != idx.end()) { at = f->second; s = fromRec(recs[at], day); mergeInto(s, j->s, j->delta); }
            else {
                if (h.n >= dayCap()) { ++h.dropped; continue; }
                at = h.n++;
                s = j->s; s.count = j->delta;
                recs.push_back(Rec{});
                idx[j->key] = at;
            }
            recs[at] = toRec(s);
            if (::pwrite(fd, &recs[at], sizeof(Rec), kHeader + (off_t)at * kRecSize) != (ssize_t)sizeof(Rec)) ok = false;
        }
        if (::pwrite(fd, &h, sizeof h, 0) != (ssize_t)sizeof h) ok = false;
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return ok;
    }
    /** At most kKeepDays files and the budget's bytes, oldest first, never today's. At most once an hour. */
    void prune_(const std::string& dir) {
        const int64_t t = now();
        if (t - lastPrune_ < 3600 && lastPrune_ > 0) return;
        lastPrune_ = t;
        std::vector<std::pair<int64_t, uint64_t>> files;
        uint64_t total = 0;
        if (DIR* dp = ::opendir(dir.c_str())) {
            while (dirent* e = ::readdir(dp)) {
                const int64_t d = dayFromName(e->d_name);
                if (d < 0) continue;
                struct stat st{};
                if (::stat((dir + "/" + e->d_name).c_str(), &st) != 0) continue;
                files.push_back({ d, (uint64_t)st.st_size }); total += (uint64_t)st.st_size;
            }
            ::closedir(dp);
        }
        std::sort(files.begin(), files.end());
        const uint64_t budget = budgetFor(limit_());
        const int64_t today = dayOf(t);
        size_t i = 0;
        while (i < files.size() && files[i].first < today && ((int)(files.size() - i) > kKeepDays || total > budget)) {
            ::unlink((dir + "/" + dayName(files[i].first)).c_str());
            total -= files[i].second; ++i;
        }
    }

    std::function<int64_t()> clock_;
    bool flusher_;
    mutable std::mutex cfgMtx_;
    std::function<std::string()> dirFn_;
    std::function<uint64_t()> limitFn_;
    std::atomic<bool> enabled_{ true }, haveDir_{ false };
    mutable std::mutex m_;
    std::map<int64_t, Table> days_;
    uint64_t spots_ = 0;
    std::mutex lsMtx_;
    std::vector<std::pair<int, Listener>> listeners_;
    int nextId_ = 0;
    std::mutex flushMtx_;
    std::mutex thMtx_;
    std::condition_variable cv_;
    bool run_ = false, started_ = false;
    std::thread th_;
    int64_t lastPrune_ = 0;
    mutable std::mutex cacheMtx_;
    mutable std::string cacheSig_;
    mutable std::unordered_map<std::string, MapRow> cache_;
    mutable std::atomic<size_t> lastRowsRead_{ 0 };
};

/** The process's store. */
inline Store& shared() { static Store s; return s; }

/** The window words the map sends: "1h" "24h" "7d" "30d" "all" → seconds (0 = all). Unknown → 24 h. */
inline int64_t windowSec(const std::string& w) {
    if (w == "1h") return 3600;
    if (w == "24h" || w.empty()) return kDaySec;
    if (w == "7d") return 7 * kDaySec;
    if (w == "30d") return 30 * kDaySec;
    if (w == "all") return 0;
    return kDaySec;
}

}  // namespace vibestations
