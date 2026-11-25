// vibe_skyplot.h — THE SKY PLOT (V12, 2026-11-24): where in the sky this station's aerial actually hears satellites.
//
// ★★★ WHAT IT IS (docs/v12/SKY-PLOT.md): a polar dome — outer ring the horizon, centre the zenith, north up, east right
//     (a map's view), rings at 30° and 60° — with one dot per decode at the satellite's azimuth / elevation at that moment,
//     coloured by SNR. It ACCUMULATES across passes, so after a few days it shows the aerial's real coverage: a YouLoop's
//     nulls, a vertical's overhead hole, a QFH's even dome, the house in the way to the north-east. A pass where the bird
//     was tracked but NOTHING decoded leaves grey dots — the blocked directions are the point.
//
// ★★ ONE FILE PER SERVER, FILTERED BY RADIO (the logs' rule, docs/v12/LOG-RADIO-FILTER.md): every radio process appends
//    to <gallery>/skyplot.dat under flock; each dot carries its radio's id (vibe_log_radio.h — never a serial). A sky plot
//    is about an AERIAL, so the views filter by radio (?radio=<id>); "all radios" is offered but is only meaningful
//    when they share an aerial.
//
// ★★ ANY SATELLITE DECODER CAN FEED IT (kind): ORBCOMM first (it is up almost all the time — a day fills the dome);
//    METEOR frames, APRS digipeats and FUNcube telemetry are kinds already, for when their decoders call add().
//
// ★ BOUNDED (256 MB boards): a dot is 24 bytes; at most kMaxDots (1.4 MB) — past that the oldest are dropped (a compaction
//   to kKeepDots under the lock). Callers aggregate: ORBCOMM writes one dot per satellite per kBucketSec, never a dot
//   per packet. The JSON answer is capped (kMaxOut, newest first) and the PNG is drawn from the same view.
//
// ★★★ beta94 THE CELLS — STORAGE BY SKY POSITION, NOT AGE (Stuart 11-25: "could keep it as tracks when less data then a
//     heatmap when it gets more"). Unattended ORBCOMM monitoring adds ~1,500–2,000 dots an hour, so the dot log above
//     only ever holds the last ~day: old directions would scroll away. Beside it, every dot is also counted into a CELL of
//     the sky — an equal-area-ish grid (namespace cells: 3° elevation bands, ~3° of azimuth at the horizon, fewer cells
//     per band towards the zenith so the zenith is not over-weighted; 2,289 cells) — per radio and kind, in 3-day
//     buckets kept for 30 days (the ADS-B outline's idea: old buckets expire whole). A cell holds its dot count, the dots
//     with nothing decoded (grey), the SNR sum (→ the mean) and max, and the last time it was heard.
//   · <gallery>/skycells.dat, under the same flock: a header naming up to kMaxCellRadios radio ids, then 16-byte records,
//     APPENDED every kCellFlushSec (a process keeps its new counts in RAM until then — ~10 KB a write, not the file), and
//     COMPACTED (duplicates merged, expired buckets dropped, then the oldest buckets until ≤ kKeepCellRecs) once it holds
//     more than kMaxCellRecs records: ≤ 400 KB (one radio's whole sky for the whole window fits).
//   · Built from the dot log the first time it is missing (a server updated to beta94 keeps what it had).
//   · The dot log stays as it was and now feeds only the recent TRACKS view.
//   · An owner's reset (resetRadio) clears one radio's dots and cells.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <cctype>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vibe_canvas.h"
#include "vibe_log_radio.h"
#include "vibe_png.h"

namespace vibe {
namespace skyplot {

enum Kind : uint8_t { kOrbcomm = 1, kMeteor = 2, kAprs = 3, kFuncube = 4 };
inline const char* kindName(int k) {
    switch (k) { case kOrbcomm: return "orbcomm"; case kMeteor: return "meteor"; case kAprs: return "aprs"; case kFuncube: return "funcube"; default: return "other"; }
}
inline int kindOf(const std::string& s) {
    if (s == "orbcomm") return kOrbcomm; if (s == "meteor") return kMeteor; if (s == "aprs") return kAprs; if (s == "funcube") return kFuncube;
    return 0;
}
constexpr int8_t kNothing = -128;          ///< "tracked, nothing decoded" (a grey dot)
constexpr size_t kMaxDots = 60000, kKeepDots = 50000, kMaxOut = 20000;
constexpr double kBucketSec = 5;           ///< the aggregation a decoder should use (one dot per bird per 5 s)

struct Dot {
    uint32_t t = 0;          ///< UTC seconds
    float azDeg = 0, elDeg = 0;
    int8_t snr = kNothing;   ///< dB (−127 … 127), kNothing = none decoded
    uint8_t kind = 0;
    uint16_t count = 0;      ///< decodes in the bucket
    uint32_t norad = 0;
    std::string radio;       ///< vibe_log_radio id ("" = unknown)
};

constexpr size_t kRec = 24;
inline void pack(const Dot& d, uint8_t* o) {
    std::memset(o, 0, kRec);
    auto p32 = [&](size_t at, uint32_t v) { o[at] = uint8_t(v); o[at + 1] = uint8_t(v >> 8); o[at + 2] = uint8_t(v >> 16); o[at + 3] = uint8_t(v >> 24); };
    auto p16 = [&](size_t at, uint16_t v) { o[at] = uint8_t(v); o[at + 1] = uint8_t(v >> 8); };
    p32(0, d.t);
    p16(4, uint16_t(std::lround(std::fmod(std::fmod(d.azDeg, 360.0) + 360.0, 360.0) * 10) % 3600));
    p16(6, uint16_t(int16_t(std::lround(std::max(-90.f, std::min(90.f, d.elDeg)) * 10))));
    o[8] = uint8_t(d.snr); o[9] = d.kind; p16(10, d.count); p32(12, d.norad);
    for (size_t i = 0; i < 8 && i < d.radio.size(); i++) o[16 + i] = uint8_t(d.radio[i]);
}
inline Dot unpack(const uint8_t* o) {
    auto g32 = [&](size_t at) { return uint32_t(o[at]) | uint32_t(o[at + 1]) << 8 | uint32_t(o[at + 2]) << 16 | uint32_t(o[at + 3]) << 24; };
    auto g16 = [&](size_t at) { return uint16_t(o[at] | o[at + 1] << 8); };
    Dot d; d.t = g32(0); d.azDeg = g16(4) / 10.f; d.elDeg = int16_t(g16(6)) / 10.f; d.snr = int8_t(o[8]); d.kind = o[9]; d.count = g16(10); d.norad = g32(12);
    for (size_t i = 0; i < 8 && o[16 + i]; i++) d.radio += char(o[16 + i]);
    if (!vibe::logradio::validId(d.radio)) d.radio.clear();
    return d;
}

/* ── ★★★ beta94 THE CELL GRID (the clients get the per-band counts in skyplot.json "cells.az" — never their own copy) ── */
namespace cells {
constexpr int kElStep = 3;                 ///< degrees per elevation band
constexpr int kBands = 90 / kElStep;       ///< 30
constexpr int kAzAtHorizon = 120;          ///< 3° of azimuth in the lowest band
constexpr int kBucketDays = 3;             ///< the aggregation: 3-day buckets …
constexpr int kWindowDays = 30;            ///< … kept 30 days
constexpr double kHeatCoverage = 0.30;     ///< AUTO → heatmap once this share of the sky's cells has been heard …
constexpr double kHeatDotsPerCell = 3.0;   ///< … with at least this many dots a heard cell on average (a colour is a mean, not one dot)
/** Cells in each band: round(kAzAtHorizon · cos(band middle)) — equal area to within ~±25 % (the top band's three). */
inline const std::vector<int>& azCounts() {
    static const std::vector<int> v = [] {
        std::vector<int> a;
        for (int b = 0; b < kBands; b++) a.push_back(std::max(1, int(std::lround(kAzAtHorizon * std::cos((b + 0.5) * kElStep * M_PI / 180.0)))));
        return a;
    }();
    return v;
}
inline const std::vector<int>& bandStart() {
    static const std::vector<int> v = [] { std::vector<int> s; int k = 0; for (int n : azCounts()) { s.push_back(k); k += n; } s.push_back(k); return s; }();
    return v;
}
inline int total() { return bandStart().back(); }
/** The cell of an az/el (el below 0 is the lowest band, 90 the top). */
inline int index(double azDeg, double elDeg) {
    const int b = std::max(0, std::min(kBands - 1, int(std::floor(std::max(0.0, elDeg) / kElStep))));
    const int n = azCounts()[size_t(b)];
    const double az = std::fmod(std::fmod(azDeg, 360.0) + 360.0, 360.0);
    const int k = std::max(0, std::min(n - 1, int(std::floor(az / 360.0 * n))));
    return bandStart()[size_t(b)] + k;
}
/** A cell's corners: az0…az1, el0…el1 (degrees). */
inline void bounds(int idx, double& az0, double& az1, double& el0, double& el1) {
    const auto& st = bandStart();
    int b = 0; while (b + 1 < kBands && st[size_t(b + 1)] <= idx) b++;
    const int n = azCounts()[size_t(b)], k = idx - st[size_t(b)];
    az0 = 360.0 * k / n; az1 = 360.0 * (k + 1) / n; el0 = b * kElStep; el1 = (b + 1) * kElStep;
}
/** The 3-day bucket a UTC time falls in. */
inline uint16_t bucketOf(uint32_t t) { return uint16_t(t / 86400u / uint32_t(kBucketDays)); }
/** The oldest bucket still inside the window at `now`. */
inline uint16_t firstBucket(uint32_t now, int days = kWindowDays) {
    const int keep = std::max(1, (std::min(days, kWindowDays) + kBucketDays - 1) / kBucketDays);
    const int b = int(bucketOf(now)) - keep + 1;
    return uint16_t(std::max(0, b));
}
/** One cell's tally (in a window, for a radio / every radio). */
struct Agg { uint32_t n = 0, grey = 0; int64_t sum = 0; int maxSnr = -128; uint32_t lastT = 0; };
inline void addDot(Agg& a, int snr, uint32_t t, uint32_t count = 1) {
    (void)count;
    if (snr == kNothing) a.grey++; else { a.n++; a.sum += snr; a.maxSnr = std::max(a.maxSnr, snr); }
    a.lastT = std::max(a.lastT, t);
}
inline void merge(Agg& a, const Agg& b) {
    a.n += b.n; a.grey += b.grey; a.sum += b.sum; a.maxSnr = std::max(a.maxSnr, b.maxSnr); a.lastT = std::max(a.lastT, b.lastT);
}
using Map = std::map<int, Agg>;   ///< cell → tally
/** AUTO's rule (src/utils/skyPlot.ts skyAutoMode is the same): heatmap once ≥ kHeatCoverage of the cells are heard (a dot,
 *  grey or not) and they average ≥ kHeatDotsPerCell dots; tracks before. Justification: under ~30 % the heatmap is mostly
 *  empty sky with a few passes' worth of single-dot cells — the tracks say more; above it the passes overlap and only a
 *  per-cell average stays readable. */
inline bool heatmapDue(const Map& m) {
    size_t heard = 0; uint64_t dots = 0;
    for (auto& kv : m) if (kv.second.n + kv.second.grey > 0) { heard++; dots += kv.second.n + kv.second.grey; }
    return heard > 0 && double(heard) >= kHeatCoverage * total() && double(dots) / double(heard) >= kHeatDotsPerCell;
}
/** The colour SNR of a cell: the MEAN of its dots' SNRs. Each dot is already the best of its 5 s (the decoders' buckets), so
 *  the mean of those maxima is a high-percentile figure of the raw signal that one freak burst cannot repaint (a cell's
 *  max would); n = 0 with grey > 0 → kNothing (tracked, nothing decoded). */
inline int colourSnr(const Agg& a) {
    if (!a.n) return kNothing;
    return int(std::lround(double(a.sum) / double(a.n)));
}
/** "STRONGEST E (AZ 70-110)" as words: the 40° azimuth window (10° steps) whose heard cells have the best mean SNR
 *  (n-weighted, ≥ 20 dots in it). az1 < 0 = none. */
inline void strongest(const Map& m, int& azLo, int& azHi, std::string& name) {
    azLo = -1; azHi = -1; name.clear();
    double best = -1e9;
    for (int c = 0; c < 360; c += 10) {
        int64_t sum = 0; uint64_t n = 0;
        for (auto& kv : m) {
            if (!kv.second.n) continue;
            double a0, a1, e0, e1; bounds(kv.first, a0, a1, e0, e1);
            double mid = 0.5 * (a0 + a1), d = std::fabs(std::fmod(mid - c + 540.0, 360.0) - 180.0);
            if (d <= 20.0) { sum += kv.second.sum; n += kv.second.n; }
        }
        if (n < 20) continue;
        const double mean = double(sum) / double(n);
        if (mean > best) { best = mean; azLo = (c - 20 + 360) % 360; azHi = (c + 20) % 360; }
    }
    if (azLo < 0) return;
    static const char* k8[] = { "north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west" };
    name = k8[((azLo + 20) % 360 + 22) / 45 % 8];
}
}  // namespace cells

/** The machine's one file. All calls take the flock (several radio processes) and a process mutex. */
class Store {
public:
    static constexpr const char* kMagic = "VSKY0001";
    void setDir(const std::string& dir) { std::lock_guard<std::mutex> lk(m_); dir_ = dir; }
    std::string dir() const { std::lock_guard<std::mutex> lk(m_); return dir_; }
    /** Append dots (this process's radio id is filled in when a dot has none). False = no directory / a write failed. */
    bool add(std::vector<Dot> dots) {
        if (dots.empty()) return true;
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) return false;
        const std::string me = vibe::logradio::mine();
        for (auto& d : dots) if (d.radio.empty()) d.radio = me;
        FLock fl(dir_);
        const std::string path = dir_ + "/skyplot.dat";
        struct stat st{};
        const bool exists = ::stat(path.c_str(), &st) == 0;
        std::FILE* f = std::fopen(path.c_str(), "ab");
        if (!f) return false;
        if (!exists || st.st_size < 8) std::fwrite(kMagic, 1, 8, f);
        uint8_t rec[kRec];
        for (auto& d : dots) { pack(d, rec); std::fwrite(rec, 1, kRec, f); }
        const bool ok = std::fflush(f) == 0;
        std::fclose(f);
        const size_t n = exists ? size_t(st.st_size > 8 ? (st.st_size - 8) / kRec : 0) + dots.size() : dots.size();
        if (n > kMaxDots) compact_(path);
        // ★★★ beta94 the cells: counted in RAM, written every kCellFlushSec (the first add builds the file if missing)
        const bool fresh = ::stat((dir_ + "/skycells.dat").c_str(), &st) != 0;
        if (!fresh) for (auto& d : dots) cells::addDot(pending_[CellKey{ d.radio, d.kind, cells::bucketOf(d.t), uint16_t(cells::index(d.azDeg, d.elDeg)) }], d.snr, d.t);
        const uint32_t now = nowUtc_();
        if (fresh || now - lastCellFlush_ >= kCellFlushSec || pending_.size() >= kPendingMax) flushCellsLocked_(now);
        return ok;
    }
    /** ★★★ beta94 the cells this process still holds → the file now (a stop, an owner's reset, the tests). */
    void flushCells() {
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) return;
        FLock fl(dir_);
        flushCellsLocked_(nowUtc_());
    }
    /** ★★★ beta94 the cells of `kind` heard by `radio` ('' = every radio) in the last `days` (≤ the 30-day window), with
     *  what this process has not written yet. `heardSince` (optional): the newest dot's time among them. */
    cells::Map readCells(int kind, const std::string& radio, int days = cells::kWindowDays) const {
        cells::Map m;
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) return m;
        const uint16_t first = cells::firstBucket(nowUtc_(), days > 0 ? days : cells::kWindowDays);
        auto want = [&](const std::string& r, uint8_t k, uint16_t b) {
            return (!kind || k == kind) && b >= first && (radio.empty() || r.empty() || r == radio);   // ★ untagged pass (the logs' rule)
        };
        {
            FLock fl(dir_);
            struct stat st{};
            if (::stat((dir_ + "/skycells.dat").c_str(), &st) != 0) const_cast<Store*>(this)->flushCellsLocked_(nowUtc_());
            CellFile cf; loadCells_(dir_ + "/skycells.dat", cf);
            for (auto& r : cf.recs) if (want(r.first.radio, r.first.kind, r.first.bucket)) cells::merge(m[r.first.cell], r.second);
        }
        for (auto& kv : pending_) if (want(kv.first.radio, kv.first.kind, kv.first.bucket)) cells::merge(m[kv.first.cell], kv.second);
        return m;
    }
    /** ★★★ beta94 THE OWNER'S RESET: `radio`'s dots and cells of `kind` gone (radio '' = every radio's). Returns the dots
     *  removed. Untagged dots stay unless every radio is reset. */
    size_t resetRadio(const std::string& radio, int kind = kOrbcomm) {
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) return 0;
        FLock fl(dir_);
        auto gone = [&](const std::string& r, int k) { return k == kind && (radio.empty() || r == radio); };
        const std::string path = dir_ + "/skyplot.dat";
        std::vector<Dot> v = load_(path), keep;
        for (auto& d : v) if (!gone(d.radio, d.kind)) keep.push_back(d);
        const size_t removed = v.size() - keep.size();
        if (removed) writeDots_(path, keep);
        for (auto it = pending_.begin(); it != pending_.end();) it = gone(it->first.radio, it->first.kind) ? pending_.erase(it) : std::next(it);
        CellFile cf; loadCells_(dir_ + "/skycells.dat", cf);
        std::map<CellKey, cells::Agg> all;
        for (auto& r : cf.recs) if (!gone(r.first.radio, r.first.kind)) cells::merge(all[r.first], r.second);
        writeCells_(dir_ + "/skycells.dat", all);
        return removed;
    }
    /** For the tests and the admin line: the cell file's size (bytes) and records. */
    size_t cellFileBytes() const {
        std::lock_guard<std::mutex> lk(m_);
        struct stat st{};
        return dir_.empty() || ::stat((dir_ + "/skycells.dat").c_str(), &st) != 0 ? 0 : size_t(st.st_size);
    }
    /** The tests: the oldest bucket in the cell file (−1 = none). */
    int oldestCellBucket() const {
        std::lock_guard<std::mutex> lk(m_);
        CellFile cf; loadCells_(dir_ + "/skycells.dat", cf);
        int b = -1; for (auto& r : cf.recs) if (b < 0 || r.first.bucket < b) b = r.first.bucket;
        return b;
    }
    /** The tests' clock (0 = the system's). */
    void setNowForTest(uint32_t t) { std::lock_guard<std::mutex> lk(m_); testNow_ = t; }
    static constexpr size_t kMaxCellRadios = 16, kCellRec = 16, kCellHeader = 8 + kMaxCellRadios * 8;
    /** ≤ 25,000 × 16 B = 400 KB; compacted to ≤ 23,000 — one radio's whole sky (2,292 cells) for all 10 buckets fits */
    static constexpr size_t kMaxCellRecs = 25000, kKeepCellRecs = 23000, kPendingMax = 3000;
    static constexpr uint32_t kCellFlushSec = 600;
    struct Filter { int kind = 0; std::string radio; uint32_t since = 0; long long norad = 0; };
    /** Newest first, at most `max`. `radios` (optional): how many dots each radio holds under the other filters. */
    std::vector<Dot> read(const Filter& fl, size_t max = kMaxOut, std::map<std::string, uint64_t>* radios = nullptr) const {
        std::vector<Dot> all;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (dir_.empty()) return all;
            FLock lock(dir_);
            all = load_(dir_ + "/skyplot.dat");
        }
        std::vector<Dot> out;
        for (size_t i = all.size(); i-- > 0;) {
            const Dot& d = all[i];
            if (fl.kind && d.kind != fl.kind) continue;
            if (fl.since && d.t < fl.since) continue;
            if (fl.norad && d.norad != uint32_t(fl.norad)) continue;
            if (radios) (*radios)[d.radio]++;
            if (!fl.radio.empty() && !d.radio.empty() && d.radio != fl.radio) continue;   // ★ untagged dots pass (the logs' rule)
            if (out.size() < max) out.push_back(d);
        }
        return out;
    }
private:
    struct FLock {
        int fd = -1;
        explicit FLock(const std::string& dir) { fd = ::open((dir + "/.skyplot.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644); if (fd >= 0) ::flock(fd, LOCK_EX); }
        ~FLock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
    };
    static std::vector<Dot> load_(const std::string& path) {
        std::vector<Dot> v;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return v;
        char mg[8];
        if (std::fread(mg, 1, 8, f) == 8 && std::memcmp(mg, kMagic, 8) == 0) {
            uint8_t rec[kRec];
            while (v.size() < kMaxDots * 2 && std::fread(rec, 1, kRec, f) == kRec) v.push_back(unpack(rec));
        }
        std::fclose(f);
        return v;
    }
    static void compact_(const std::string& path) {
        std::vector<Dot> v = load_(path);
        if (v.size() <= kKeepDots) return;
        v.erase(v.begin(), v.end() - long(kKeepDots));
        writeDots_(path, v);
    }
    static void writeDots_(const std::string& path, const std::vector<Dot>& v) {
        const std::string tmp = path + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return;
        std::fwrite(kMagic, 1, 8, f);
        uint8_t rec[kRec];
        for (auto& d : v) { pack(d, rec); std::fwrite(rec, 1, kRec, f); }
        const bool ok = std::fflush(f) == 0;
        std::fclose(f);
        if (ok) std::rename(tmp.c_str(), path.c_str()); else std::remove(tmp.c_str());
    }
    // ── ★★★ beta94 the cell file ──
    struct CellKey {
        std::string radio; uint8_t kind = 0; uint16_t bucket = 0; uint16_t cell = 0;
        bool operator<(const CellKey& o) const {
            if (bucket != o.bucket) return bucket < o.bucket;
            if (radio != o.radio) return radio < o.radio;
            if (kind != o.kind) return kind < o.kind;
            return cell < o.cell;
        }
    };
    struct CellFile { std::vector<std::string> radios; std::vector<std::pair<CellKey, cells::Agg>> recs; };
    uint32_t nowUtc_() const { return testNow_ ? testNow_ : uint32_t(std::time(nullptr)); }
    static void loadCells_(const std::string& path, CellFile& cf) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return;
        uint8_t h[kCellHeader];
        if (std::fread(h, 1, kCellHeader, f) != kCellHeader || std::memcmp(h, "VSKC0001", 8) != 0) { std::fclose(f); return; }
        for (size_t i = 0; i < kMaxCellRadios; i++) {
            std::string r; for (size_t k = 0; k < 8 && h[8 + i * 8 + k]; k++) r += char(h[8 + i * 8 + k]);
            cf.radios.push_back(vibe::logradio::validId(r) ? r : std::string());
        }
        uint8_t o[kCellRec];
        while (cf.recs.size() < kMaxCellRecs * 2 && std::fread(o, 1, kCellRec, f) == kCellRec) {
            auto g16 = [&](size_t at) { return uint16_t(o[at] | o[at + 1] << 8); };
            auto g32 = [&](size_t at) { return uint32_t(o[at]) | uint32_t(o[at + 1]) << 8 | uint32_t(o[at + 2]) << 16 | uint32_t(o[at + 3]) << 24; };
            CellKey k; k.cell = g16(0); k.bucket = g16(2); k.kind = uint8_t(o[13] & 7);
            if (k.cell >= cells::total()) continue;
            const uint8_t slot = uint8_t(o[13] >> 3);
            k.radio = slot < cf.radios.size() ? cf.radios[slot] : std::string();
            cells::Agg a; a.n = g16(4); a.grey = g16(6); a.sum = int32_t(g32(8)); a.maxSnr = int8_t(o[12]);
            a.lastT = uint32_t(k.bucket) * 86400u * uint32_t(cells::kBucketDays) + uint32_t(g16(14)) * 60u;   // ★ minutes into the bucket
            cf.recs.push_back({ k, a });
        }
        std::fclose(f);
    }
    static void packCell_(const CellKey& k, const cells::Agg& a, uint8_t slot, uint8_t* o) {
        std::memset(o, 0, kCellRec);
        auto p16 = [&](size_t at, uint16_t v) { o[at] = uint8_t(v); o[at + 1] = uint8_t(v >> 8); };
        auto p32 = [&](size_t at, uint32_t v) { o[at] = uint8_t(v); o[at + 1] = uint8_t(v >> 8); o[at + 2] = uint8_t(v >> 16); o[at + 3] = uint8_t(v >> 24); };
        // ★ saturating: a cell past 65,535 dots in 3 days keeps its mean (the sum scaled with the count)
        uint32_t n = a.n, gr = a.grey; int64_t sum = a.sum;
        if (n > 65535) { sum = sum * 65535 / int64_t(n); n = 65535; }
        if (gr > 65535) gr = 65535;
        p16(0, k.cell); p16(2, k.bucket); p16(4, uint16_t(n)); p16(6, uint16_t(gr));
        p32(8, uint32_t(int32_t(std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX, sum)))));
        o[12] = uint8_t(int8_t(std::max(-128, std::min(127, a.maxSnr)))); o[13] = uint8_t((k.kind & 7) | (slot & 15) << 3);
        const uint32_t b0 = uint32_t(k.bucket) * 86400u * uint32_t(cells::kBucketDays);
        p16(14, uint16_t(std::min<uint32_t>(65535, a.lastT > b0 ? (a.lastT - b0) / 60u : 0)));   // ★ minutes into the bucket (a 3-day bucket: ≤ 4,320)
    }
    /** The whole file from `all`: expired buckets out, then the oldest until ≤ kKeepCellRecs; ≤ kMaxCellRadios radios
     *  (the least recently heard go first). */
    void writeCells_(const std::string& path, std::map<CellKey, cells::Agg>& all) const {
        const uint16_t first = cells::firstBucket(nowUtc_());
        for (auto it = all.begin(); it != all.end();) it = it->first.bucket < first ? all.erase(it) : std::next(it);
        while (all.size() > kKeepCellRecs) {   // ★ the oldest bucket goes whole (the map is ordered by bucket first)
            const uint16_t b = all.begin()->first.bucket;
            for (auto it = all.begin(); it != all.end() && it->first.bucket == b;) it = all.erase(it);
        }
        std::map<std::string, uint32_t> last;
        for (auto& kv : all) last[kv.first.radio] = std::max(last[kv.first.radio], kv.second.lastT);
        std::vector<std::pair<uint32_t, std::string>> order;
        for (auto& kv : last) order.push_back({ kv.second, kv.first });
        std::sort(order.rbegin(), order.rend());
        std::map<std::string, uint8_t> slot;
        std::vector<std::string> radios;
        for (auto& o : order) if (radios.size() < kMaxCellRadios) { slot[o.second] = uint8_t(radios.size()); radios.push_back(o.second); }
        const std::string tmp = path + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return;
        uint8_t h[kCellHeader] = {};
        std::memcpy(h, "VSKC0001", 8);
        for (size_t i = 0; i < radios.size(); i++) for (size_t k = 0; k < 8 && k < radios[i].size(); k++) h[8 + i * 8 + k] = uint8_t(radios[i][k]);
        std::fwrite(h, 1, kCellHeader, f);
        uint8_t o[kCellRec];
        for (auto& kv : all) {
            auto sl = slot.find(kv.first.radio);
            if (sl == slot.end()) continue;
            packCell_(kv.first, kv.second, sl->second, o); std::fwrite(o, 1, kCellRec, f);
        }
        const bool ok = std::fflush(f) == 0;
        std::fclose(f);
        if (ok) std::rename(tmp.c_str(), path.c_str()); else std::remove(tmp.c_str());
    }
    /** Caller holds m_ and the flock. Missing file → built from the dot log (which already holds every pending dot). */
    void flushCellsLocked_(uint32_t now) {
        lastCellFlush_ = now;
        const std::string path = dir_ + "/skycells.dat";
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) {
            std::map<CellKey, cells::Agg> all;
            for (auto& d : load_(dir_ + "/skyplot.dat"))
                cells::addDot(all[CellKey{ d.radio, d.kind, cells::bucketOf(d.t), uint16_t(cells::index(d.azDeg, d.elDeg)) }], d.snr, d.t);
            pending_.clear();
            writeCells_(path, all);
            return;
        }
        if (pending_.empty()) return;
        CellFile cf; loadCells_(path, cf);
        bool known = true;
        for (auto& kv : pending_) if (std::find(cf.radios.begin(), cf.radios.end(), kv.first.radio) == cf.radios.end()) known = false;
        if (!known || cf.recs.size() + pending_.size() > kMaxCellRecs) {   // ★ a new radio, or full: merge and rewrite
            std::map<CellKey, cells::Agg> all;
            for (auto& r : cf.recs) cells::merge(all[r.first], r.second);
            for (auto& kv : pending_) cells::merge(all[kv.first], kv.second);
            pending_.clear();
            writeCells_(path, all);
            return;
        }
        std::FILE* f = std::fopen(path.c_str(), "ab");
        if (!f) return;
        uint8_t o[kCellRec];
        for (auto& kv : pending_) {
            const uint8_t sl = uint8_t(std::find(cf.radios.begin(), cf.radios.end(), kv.first.radio) - cf.radios.begin());
            packCell_(kv.first, kv.second, sl, o); std::fwrite(o, 1, kCellRec, f);
        }
        std::fclose(f);
        pending_.clear();
    }
    mutable std::mutex m_;
    std::string dir_;
    std::map<CellKey, cells::Agg> pending_;   // m_
    uint32_t lastCellFlush_ = 0, testNow_ = 0;
};
inline Store& shared() { static Store s; return s; }

/** The view's JSON: {"v":1,"kind":…,"dots":[[az10,el10,snr|null,t,kind,norad,count,radioIndex]…],"radioIds":[…],
 *  "radios":[{"id","name","n"}…],"count":N,"truncated":bool}. snr null = tracked, nothing decoded. */
inline std::string json(const std::vector<Dot>& dots, const std::map<std::string, uint64_t>& radios, const std::string& kind, bool truncated) {
    std::vector<std::string> ids;
    std::map<std::string, int> idx;
    std::string o = "{\"v\":1,\"kind\":\"" + vibe::logradio::jsonEsc(kind) + "\",\"dots\":[";
    for (size_t i = 0; i < dots.size(); i++) {
        const Dot& d = dots[i];
        int ri = -1;
        if (!d.radio.empty()) {
            auto it = idx.find(d.radio);
            if (it == idx.end()) { ri = int(ids.size()); idx[d.radio] = ri; ids.push_back(d.radio); } else ri = it->second;
        }
        char b[128];
        if (d.snr == kNothing) std::snprintf(b, sizeof b, "%s[%d,%d,null,%u,%d,%u,%u,%d]", i ? "," : "", int(std::lround(d.azDeg * 10)), int(std::lround(d.elDeg * 10)), d.t, d.kind, d.norad, d.count, ri);
        else std::snprintf(b, sizeof b, "%s[%d,%d,%d,%u,%d,%u,%u,%d]", i ? "," : "", int(std::lround(d.azDeg * 10)), int(std::lround(d.elDeg * 10)), int(d.snr), d.t, d.kind, d.norad, d.count, ri);
        o += b;
    }
    o += "],\"radioIds\":[";
    for (size_t i = 0; i < ids.size(); i++) o += (i ? ",\"" : "\"") + ids[i] + "\"";
    std::vector<std::pair<std::string, uint64_t>> counts; uint64_t unknown = 0;
    for (auto& r : radios) { if (r.first.empty()) unknown += r.second; else counts.push_back(r); }
    o += "],\"radios\":" + vibe::logradio::radiosJson(counts, unknown);
    o += ",\"count\":" + std::to_string(dots.size()) + ",\"truncated\":" + (truncated ? "true" : "false") + "}";
    return o;
}
/** ★★★ beta94 the cells' JSON (skyplot.json "cells"): the grid (elStep, az = cells per band from the horizon up — the
 *  clients never keep their own copy), the window, AUTO's verdict, and each heard cell [index, n, grey, mean×10 | null,
 *  max | null, lastT]. ≤ cells::total() rows (~2,300 × ~25 bytes). */
inline std::string cellsJson(const cells::Map& m, int days) {
    std::string o = "{\"v\":1,\"elStep\":" + std::to_string(cells::kElStep) + ",\"az\":[";
    for (size_t i = 0; i < cells::azCounts().size(); i++) o += (i ? "," : "") + std::to_string(cells::azCounts()[i]);
    o += "],\"total\":" + std::to_string(cells::total()) + ",\"days\":" + std::to_string(std::min(days > 0 ? days : cells::kWindowDays, cells::kWindowDays))
       + ",\"bucketDays\":" + std::to_string(cells::kBucketDays) + ",\"auto\":\"" + (cells::heatmapDue(m) ? "heatmap" : "tracks") + "\"";
    // ★ AUTO's rule travels with the grid, so a client re-judging after merging its live dots uses the server's figures
    { char b[96]; std::snprintf(b, sizeof b, ",\"heatCoverage\":%.2f,\"heatDotsPerCell\":%.1f", cells::kHeatCoverage, cells::kHeatDotsPerCell); o += b; }
    o += ",\"c\":[";
    bool first = true;
    for (auto& kv : m) {
        const cells::Agg& a = kv.second;
        if (!a.n && !a.grey) continue;
        char b[96];
        if (a.n) std::snprintf(b, sizeof b, "%s[%d,%u,%u,%lld,%d,%u]", first ? "" : ",", kv.first, a.n, a.grey, (long long)std::llround(10.0 * double(a.sum) / double(a.n)), a.maxSnr, a.lastT);
        else std::snprintf(b, sizeof b, "%s[%d,0,%u,null,null,%u]", first ? "" : ",", kv.first, a.grey, a.lastT);
        o += b; first = false;
    }
    return o + "]}";
}
/** The JSON with its cells: json() + ,"cells":{…} before the closing brace. */
inline std::string json(const std::vector<Dot>& dots, const std::map<std::string, uint64_t>& radios, const std::string& kind, bool truncated,
                        const cells::Map& m, int days) {
    std::string o = json(dots, radios, kind, truncated);
    o.pop_back();
    return o + ",\"cells\":" + cellsJson(m, days) + "}";
}

/** The SNR colour ramp (dB): 0 → deep blue, 6 → teal, 12 → green, 18+ → yellow. Grey = nothing decoded. The clients
 *  use the same four stops (src/utils/skyPlot.ts snrColour). */
inline void snrColour(int snr, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (snr == kNothing) { r = g = b = 110; return; }
    static const float stops[4][4] = { { 0, 50, 60, 200 }, { 6, 30, 170, 190 }, { 12, 60, 200, 80 }, { 18, 250, 225, 40 } };
    const float s = std::max(0.f, std::min(18.f, float(snr)));
    int k = 0; while (k < 2 && s > stops[k + 1][0]) k++;
    const float f = (s - stops[k][0]) / (stops[k + 1][0] - stops[k][0]);
    r = uint8_t(stops[k][1] + f * (stops[k + 1][1] - stops[k][1]));
    g = uint8_t(stops[k][2] + f * (stops[k + 1][2] - stops[k][2]));
    b = uint8_t(stops[k][3] + f * (stops[k + 1][3] - stops[k][3]));
}

/** The picture (the gallery's copy): a W×W dome + a legend strip, PNG. Oldest drawn first so the newest sit on top;
 *  grey (nothing decoded) under everything. `title` in the 5×7 font's characters (A–Z 0–9 . - space). */
/** ★★★ beta94 mode: 0 AUTO (cells::heatmapDue), 1 TRACKS, 2 HEATMAP. A heatmap fills each heard cell with its mean SNR's
 *  colour (grey: tracked, nothing decoded) and says "HEATMAP - 30 DAYS - 1240 CELLS HEARD - STRONGEST EAST AZ 70-110". */
enum Mode { kAuto = 0, kTracks = 1, kHeatmap = 2 };
inline bool heatMode(int mode, const cells::Map* cm) { return cm && (mode == kHeatmap || (mode == kAuto && cells::heatmapDue(*cm))); }
inline std::string heatLine(const cells::Map& cm, int days) {
    size_t heard = 0; for (auto& kv : cm) if (kv.second.n + kv.second.grey) heard++;
    int a0, a1; std::string nm; cells::strongest(cm, a0, a1, nm);
    std::string t = "HEATMAP - " + std::to_string(std::min(days > 0 ? days : cells::kWindowDays, cells::kWindowDays)) + " DAYS - " + std::to_string(heard) + " CELLS HEARD";
    if (a0 >= 0) { for (auto& ch : nm) ch = char(std::toupper((unsigned char)ch)); t += " - STRONGEST " + nm + " AZ " + std::to_string(a0) + "-" + std::to_string(a1); }
    return t;
}
/** The picture's pixels (png() encodes them; the tests read them). Dome centre (W/2, 36 + (W−72)/2), radius (W−72)/2. */
inline vibe::canvas::Canvas render(const std::vector<Dot>& dotsNewestFirst, const std::string& title, int W = 520, const cells::Map* cm = nullptr,
                                   int mode = kAuto, int days = cells::kWindowDays) {
    const int H = W + 64;
    vibe::canvas::Canvas c(W, H, 14, 18, 26);
    const int cx = W / 2, cy = 36 + (W - 72) / 2, R = (W - 72) / 2;
    auto ring = [&](double elDeg, uint8_t v) {
        const double rr = R * (90 - elDeg) / 90.0;
        const int n = int(2 * M_PI * rr);
        for (int i = 0; i < n; i++) { const double a = 2 * M_PI * i / n; c.dot(int(cx + rr * std::cos(a)), int(cy + rr * std::sin(a)), v, v, uint8_t(v + 12)); }
    };
    c.disc(cx, cy, R, 22, 28, 40);
    ring(0, 150); ring(30, 80); ring(60, 80);
    c.line(cx - R, cy, cx + R, cy, 50, 56, 70); c.line(cx, cy - R, cx, cy + R, 50, 56, 70);
    c.text(cx - 2, cy - R - 12, "N", 220, 220, 230); c.text(cx - 2, cy + R + 5, "S", 220, 220, 230);
    c.text(cx + R + 6, cy - 3, "E", 220, 220, 230); c.text(cx - R - 12, cy - 3, "W", 220, 220, 230);
    c.text(cx + 3, cy - int(R * 60 / 90.0) - 9, "30", 120, 128, 150); c.text(cx + 3, cy - int(R * 30 / 90.0) - 9, "60", 120, 128, 150);
    auto xy = [&](const Dot& d, int& x, int& y) {
        const double rr = R * (90 - std::max(0.f, d.elDeg)) / 90.0, a = d.azDeg * M_PI / 180.0;
        x = int(std::lround(cx + rr * std::sin(a))); y = int(std::lround(cy - rr * std::cos(a)));
    };
    const bool heat = heatMode(mode, cm);
    if (heat) {   // ★★★ beta94 every pixel of the dome → its cell → the cell's colour
        for (int y = cy - R; y <= cy + R; y++)
            for (int x = cx - R; x <= cx + R; x++) {
                const double dx = x - cx, dy = y - cy, rr = std::sqrt(dx * dx + dy * dy);
                if (rr > R - 0.5) continue;
                const double el = 90.0 * (1.0 - rr / R);
                double az = std::atan2(dx, -dy) * 180.0 / M_PI; if (az < 0) az += 360.0;
                auto it = cm->find(cells::index(az, el));
                if (it == cm->end() || (!it->second.n && !it->second.grey)) continue;
                uint8_t r, g, b; snrColour(cells::colourSnr(it->second), r, g, b);
                c.dot(x, y, r, g, b);
            }
        ring(0, 150); ring(30, 80); ring(60, 80);
        c.text(8, 20, heatLine(*cm, days).substr(0, size_t((W - 16) / 6)), 170, 176, 196);
    }
    for (int pass = 0; pass < 2 && !heat; pass++)   // grey first, then the decodes, oldest first
        for (size_t i = dotsNewestFirst.size(); i-- > 0;) {
            const Dot& d = dotsNewestFirst[i];
            if ((d.snr == kNothing) != (pass == 0) || d.elDeg < -1) continue;
            int x, y; xy(d, x, y);
            uint8_t r, g, b; snrColour(d.snr, r, g, b);
            c.disc(x, y, pass == 0 ? 1 : 2, r, g, b);
        }
    std::string t = title.substr(0, size_t((W - 16) / 6));
    c.text(8, 8, t, 230, 230, 240);
    // the legend: the SNR ramp 0–18 dB and the grey "nothing decoded"
    const int ly = H - 24, lx = 40, lw = W - 220;
    for (int i = 0; i < lw; i++) { uint8_t r, g, b; snrColour(int(std::lround(18.0 * i / lw)), r, g, b); for (int k = 0; k < 8; k++) c.dot(lx + i, ly + k, r, g, b); }
    c.text(lx - 8 - 6, ly, "0", 200, 200, 210); c.text(lx + lw + 4, ly, "18 DB SNR", 200, 200, 210);
    c.disc(W - 92, ly + 4, 3, 110, 110, 110); c.text(W - 84, ly, "NONE", 200, 200, 210);
    return c;
}
inline std::string png(const std::vector<Dot>& dotsNewestFirst, const std::string& title, int W = 520, const cells::Map* cm = nullptr,
                       int mode = kAuto, int days = cells::kWindowDays) {
    vibe::canvas::Canvas c = render(dotsNewestFirst, title, W, cm, mode, days);
    return vibe::png::encode(c.w, c.h, 3, c.px.data());
}

}  // namespace skyplot
}  // namespace vibe
