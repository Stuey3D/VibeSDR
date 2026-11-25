// vibe_adsb_range.h — THE ADS-B RANGE OUTLINE: how far this radio hears on every bearing (V12 beta94).
//
// ★★ Stuart, 2026-11-25: "can our adsb maps for the server put a jagged ring around the receiver showing the maximum range
//    detected. my virtual radar is setup like that" — Virtual Radar Server's receiver range plot: a jagged polygon whose
//    radius on each bearing is the furthest aircraft position received on that bearing.
// ★★★ A ROLLING WINDOW, NOT ALL-TIME (Stuart, the same day: "i wonder if it should have an expiry so one super strong signal
//     one day doesn't permanently skew the ring"). Each 1° bearing keeps its DAILY maximum for the last kDays (90) UTC
//     days; the outline drawn is the maximum per bearing over the owner's window — 7, 30 (the default) or 90 days, or all
//     time. A one-day tropo opening fades out of the outline when its day leaves the window. The ALL-TIME maximum per
//     bearing is kept separately (the map's optional dotted line), with the furthest catch ever as a record of its own —
//     a great catch is never forgotten.
// ★★ WHAT COUNTS: a position from an aircraft that has had at least two CONSISTENT positions in a row (vibe_adsb.h setPos
//    posOk — the second within flying distance of the first), at most kMaxKm from the receiver. One bad decode never
//    makes a spike. The Tracker's own checks (CPR pairs, the 360 NM global cap, the single-frame wait) come first.
// ★★ RESET: the owner's "Reset range outline" (setup page, the app's server screen) clears it all; so does the receiver
//    moving more than kMovedKm (it is a different site's outline).
// ★ BOUNDED: 90 × 360 uint16 (0.1 km) + 360 all-time maxima + their dates = ~68 KB in memory and on disk, whatever the sky.
//    Saved atomically (temp file + rename), at most once a minute and when ADS-B stops.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vibeadsb {

class RangeOutline {
public:
    static constexpr int kBins = 360;          ///< 1° each, bin i = bearings [i, i+1)
    static constexpr int kDays = 90;           ///< daily maxima kept (the longest window offered)
    static constexpr double kMaxKm = 650;      ///< ADS-B: further than this from a ground receiver is a bad fix, not range
    static constexpr double kMovedKm = 5;      ///< the receiver moved further than this: a new site, a new outline
    static constexpr int kWindowDefault = 30;  ///< days; 0 = all time
    static bool validWindow(int d) { return d == 0 || d == 7 || d == 30 || d == 90; }

    explicit RangeOutline(double maxKm = kMaxKm) : maxKm_(maxKm) { wipe(); }
    double maxKm() const { return maxKm_; }

    static int64_t dayOf(int64_t utc) { return utc >= 0 ? utc / 86400 : (utc - 86399) / 86400; }
    static double distKm(double la1, double lo1, double la2, double lo2) {
        const double r = 3.14159265358979323846 / 180.0;
        const double a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
        const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
        return 2 * 6371.0088 * std::asin(std::min(1.0, std::sqrt(h)));
    }
    /** Initial great-circle bearing, degrees [0, 360). */
    static double bearingDeg(double la1, double lo1, double la2, double lo2) {
        const double r = 3.14159265358979323846 / 180.0;
        const double y = std::sin((lo2 - lo1) * r) * std::cos(la2 * r);
        const double x = std::cos(la1 * r) * std::sin(la2 * r) - std::sin(la1 * r) * std::cos(la2 * r) * std::cos((lo2 - lo1) * r);
        double b = std::atan2(y, x) / r;
        b = std::fmod(b + 360.0, 360.0);
        return b >= 360.0 ? 0.0 : b;
    }

    /** The receiver's position. The first one adopts it; one more than kMovedKm from the outline's own clears the outline
     *  (true = cleared). `utc` stamps the new outline's start. */
    bool setCentre(double lat, double lon, int64_t utc) {
        if (!(lat == lat) || !(lon == lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) return false;
        if (hasCentre_ && distKm(cLat_, cLon_, lat, lon) <= kMovedKm) return false;
        const bool had = hasCentre_ && !empty();
        const int w = window_;
        wipe();
        window_ = w;
        hasCentre_ = true; cLat_ = lat; cLon_ = lon; since_ = utc;
        return had;
    }
    bool hasCentre() const { return hasCentre_; }

    /** One believed position. True if anything grew (worth saving). */
    bool offer(double lat, double lon, int64_t utc) {
        if (!hasCentre_ || !(lat == lat) || !(lon == lon)) return false;
        const double km = distKm(cLat_, cLon_, lat, lon);
        if (!(km > 0.05) || km > maxKm_) return false;
        const int bin = std::min(kBins - 1, std::max(0, (int)std::floor(bearingDeg(cLat_, cLon_, lat, lon))));
        const uint16_t q = (uint16_t)std::min(65535.0, std::round(km * 10.0));
        const int64_t d = dayOf(utc);
        const int slot = (int)(((d % kDays) + kDays) % kDays);
        bool grew = false;
        if (slotDay_[slot] != d) { slotDay_[slot] = d; std::fill(day_.begin() + slot * kBins, day_.begin() + (slot + 1) * kBins, 0); grew = true; }
        uint16_t& v = day_[size_t(slot * kBins + bin)];
        if (q > v) { v = q; grew = true; }
        if (q > all_[bin]) { all_[bin] = q; allAt_[bin] = utc; grew = true; }
        if (q > recKm_) { recKm_ = q; recBin_ = bin; recAt_ = utc; }
        if (!since_) since_ = utc;
        return grew;
    }

    void clear(int64_t utc) {
        const int w = window_; const bool hc = hasCentre_; const double la = cLat_, lo = cLon_;
        wipe();
        window_ = w; hasCentre_ = hc; cLat_ = la; cLon_ = lo; since_ = utc;
    }
    bool empty() const { for (uint16_t v : all_) if (v) return false; return true; }

    int window() const { return window_; }
    bool setWindow(int days) { if (!validWindow(days)) return false; window_ = days; return true; }

    /** The outline over `days` (0 = all time) as of `utc`: km ×10 per bin, and the UTC day each came from (-1 = none). */
    void windowed(int days, int64_t utc, std::vector<uint16_t>& km, std::vector<int64_t>& day) const {
        km.assign(kBins, 0); day.assign(kBins, -1);
        if (days == 0) {
            for (int b = 0; b < kBins; ++b) if (all_[b]) { km[size_t(b)] = all_[b]; day[size_t(b)] = dayOf(allAt_[b]); }
            return;
        }
        const int64_t today = dayOf(utc);
        for (int s = 0; s < kDays; ++s) {
            const int64_t d = slotDay_[s];
            if (d == kNoDay || d > today || today - d >= days) continue;
            const uint16_t* row = &day_[size_t(s * kBins)];
            for (int b = 0; b < kBins; ++b) if (row[b] > km[size_t(b)]) { km[size_t(b)] = row[b]; day[size_t(b)] = d; }
        }
    }

    /** What the map and the setup page read (GET /adsb/range):
     *  {"ok":1,"window":30,"lat":…,"lon":…,"since":<utc>,"now":<utc>,"km":[360 km, 0 = none],"day":[360 UTC day numbers, -1],
     *   "allKm":[360],"days":<days with data in the window>,"far":{"km","brg","at"} (the window's furthest) | null,
     *   "record":{"km","brg","at"} (the furthest ever) | null}. ~7 KB. */
    std::string json(int64_t utc) const {
        std::vector<uint16_t> km; std::vector<int64_t> day;
        windowed(window_, utc, km, day);
        std::string o;
        o.reserve(8000);
        char b[160];
        std::snprintf(b, sizeof b, "{\"ok\":1,\"window\":%d,", window_);
        o += b;
        if (hasCentre_) { std::snprintf(b, sizeof b, "\"lat\":%.5f,\"lon\":%.5f,", cLat_, cLon_); o += b; }
        else o += "\"lat\":null,\"lon\":null,";
        std::snprintf(b, sizeof b, "\"since\":%lld,\"now\":%lld,\"km\":[", (long long)since_, (long long)utc);
        o += b;
        int fb = -1;
        for (int i = 0; i < kBins; ++i) {
            if (i) o += ',';
            o += fmtKm(km[size_t(i)]);
            if (km[size_t(i)] && (fb < 0 || km[size_t(i)] > km[size_t(fb)])) fb = i;
        }
        o += "],\"day\":[";
        for (int i = 0; i < kBins; ++i) { if (i) o += ','; o += std::to_string((long long)day[size_t(i)]); }
        o += "],\"allKm\":[";
        for (int i = 0; i < kBins; ++i) { if (i) o += ','; o += fmtKm(all_[i]); }
        o += "],";
        int days = 0;
        const int64_t today = dayOf(utc);
        if (window_ == 0) { int64_t first = since_ ? dayOf(since_) : today; days = (int)std::max<int64_t>(0, today - first + 1); }
        else for (int s = 0; s < kDays; ++s) { const int64_t d = slotDay_[s]; if (d != kNoDay && d <= today && today - d < window_) ++days; }
        std::snprintf(b, sizeof b, "\"days\":%d,\"far\":", days);
        o += b;
        if (fb >= 0) { std::snprintf(b, sizeof b, "{\"km\":%s,\"brg\":%d,\"at\":%lld}", fmtKm(km[size_t(fb)]).c_str(), fb, (long long)(day[size_t(fb)] * 86400)); o += b; }
        else o += "null";
        o += ",\"record\":";
        if (recKm_) { std::snprintf(b, sizeof b, "{\"km\":%s,\"brg\":%d,\"at\":%lld}", fmtKm(recKm_).c_str(), recBin_, (long long)recAt_); o += b; }
        else o += "null";
        return o + "}";
    }

    /* ── persistence: a small binary file, little-endian, versioned ─────────────────────────────────────────────── */
    std::string serialize() const {
        std::string s;
        s.reserve(80000);
        put(s, kMagic); put(s, (uint32_t)kVersion);
        put(s, (int32_t)window_); put(s, (int32_t)(hasCentre_ ? 1 : 0)); put(s, cLat_); put(s, cLon_); put(s, (int64_t)since_);
        put(s, (uint16_t)recKm_); put(s, (int32_t)recBin_); put(s, (int64_t)recAt_);
        for (int i = 0; i < kDays; ++i) put(s, (int64_t)slotDay_[i]);
        for (uint16_t v : day_) put(s, v);
        for (uint16_t v : all_) put(s, v);
        for (int64_t v : allAt_) put(s, v);
        return s;
    }
    bool parse(const std::string& s) {
        size_t p = 0;
        uint32_t mg = 0, ver = 0;
        if (!get(s, p, mg) || mg != kMagic || !get(s, p, ver) || ver != kVersion) return false;
        RangeOutline r(maxKm_);
        int32_t w = 0, hc = 0, rb = 0; int64_t since = 0, ra = 0; uint16_t rk = 0;
        if (!get(s, p, w) || !get(s, p, hc) || !get(s, p, r.cLat_) || !get(s, p, r.cLon_) || !get(s, p, since)) return false;
        if (!get(s, p, rk) || !get(s, p, rb) || !get(s, p, ra)) return false;
        for (int i = 0; i < kDays; ++i) { int64_t d; if (!get(s, p, d)) return false; r.slotDay_[i] = d; }
        for (auto& v : r.day_) if (!get(s, p, v)) return false;
        for (auto& v : r.all_) if (!get(s, p, v)) return false;
        for (auto& v : r.allAt_) if (!get(s, p, v)) return false;
        if (p != s.size()) return false;
        r.window_ = validWindow(w) ? w : kWindowDefault;
        r.hasCentre_ = hc != 0 && r.cLat_ == r.cLat_ && r.cLon_ == r.cLon_;
        r.since_ = since; r.recKm_ = rk; r.recBin_ = (rb >= 0 && rb < kBins) ? rb : 0; r.recAt_ = ra;
        // ★ a slot whose day does not belong in it is damage: dropped, never trusted
        for (int i = 0; i < kDays; ++i) if (r.slotDay_[i] != kNoDay && (((r.slotDay_[i] % kDays) + kDays) % kDays) != i) {
            r.slotDay_[i] = kNoDay; std::fill(r.day_.begin() + i * kBins, r.day_.begin() + (i + 1) * kBins, 0);
        }
        *this = std::move(r);
        return true;
    }
    bool load(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::string s; char buf[8192]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && s.size() < kFileMax) s.append(buf, n);
        std::fclose(f);
        return parse(s);
    }
    bool save(const std::string& path) const {
        if (path.empty()) return false;
        const std::string tmp = path + ".tmp", s = serialize();
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
        if (std::fclose(f) != 0 || !ok) { std::remove(tmp.c_str()); return false; }
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }
    static constexpr size_t kFileMax = 200000;   ///< the file is ~68 KB; anything past this is not ours

private:
    static constexpr int64_t kNoDay = INT64_MIN;
    static constexpr uint32_t kMagic = 0x52425356u;   // "VSBR"
    static constexpr uint32_t kVersion = 1;

    void wipe() {
        window_ = kWindowDefault; hasCentre_ = false; cLat_ = cLon_ = 0; since_ = 0;
        recKm_ = 0; recBin_ = 0; recAt_ = 0;
        std::fill(std::begin(slotDay_), std::end(slotDay_), kNoDay);
        day_.assign(size_t(kDays) * kBins, 0);
        all_.assign(kBins, 0); allAt_.assign(kBins, 0);
    }
    static std::string fmtKm(uint16_t q) { char b[16]; std::snprintf(b, sizeof b, "%.1f", q / 10.0); std::string s = b; if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2); return s; }
    template <class T> static void put(std::string& s, T v) { unsigned char b[sizeof(T)]; std::memcpy(b, &v, sizeof v); s.append(reinterpret_cast<char*>(b), sizeof b); }
    template <class T> static bool get(const std::string& s, size_t& p, T& v) { if (p + sizeof(T) > s.size()) return false; std::memcpy(&v, s.data() + p, sizeof v); p += sizeof v; return true; }

    double maxKm_ = kMaxKm;
    int window_ = kWindowDefault;
    bool hasCentre_ = false;
    double cLat_ = 0, cLon_ = 0;
    int64_t since_ = 0;                 ///< when this outline began (reset / new site / first catch), UTC seconds
    uint16_t recKm_ = 0; int recBin_ = 0; int64_t recAt_ = 0;   ///< the furthest catch ever
    int64_t slotDay_[kDays];            ///< the UTC day each slot holds (kNoDay = empty); slot = day % kDays
    std::vector<uint16_t> day_;         ///< kDays × kBins daily maxima, 0.1 km
    std::vector<uint16_t> all_;         ///< kBins all-time maxima, 0.1 km
    std::vector<int64_t> allAt_;        ///< when each all-time maximum was set, UTC seconds
};

/* ══ THE RADIO'S OUTLINES — one per SOURCE, in its own file beside this radio's other state ═══════════════════════════
 *  Only ADS-B today. Stuart asked (11-25) whether ACARS / VDL2 could have one too, then held it: they rarely carry a
 *  position. A source is one more RangeSrc, its name and its distance limit — the store, the endpoint (?src=), the map
 *  and the setup page already take it. HFDL is HF (worldwide) and would never have one.
 *  One per radio PROCESS (each radio is its own process), like vibeavi::sharedTracker().
 *  ★ Decoder threads offer; the HTTP thread reads; one mutex, held for microseconds (a save is a ~68 KB write, at most
 *    once a minute per source, on the decoder's own thread — never the DSP's). */
enum RangeSrc { kRangeAdsb = 0, kRangeSrcN = 1 };
inline const char* rangeSrcName(int s) { return s == kRangeAdsb ? "adsb" : ""; }
inline double rangeSrcMaxKm(int s) { return s == kRangeAdsb ? RangeOutline::kMaxKm : RangeOutline::kMaxKm; }
inline int rangeSrcOf(const std::string& n) { for (int i = 0; i < kRangeSrcN; ++i) if (n == rangeSrcName(i)) return i; return -1; }

class RangeStore {
public:
    static constexpr int64_t kSaveEverySec = 60;
    RangeStore() { for (int i = 0; i < kRangeSrcN; ++i) o_[i] = RangeOutline(rangeSrcMaxKm(i)); }

    /** Where this radio keeps them: `<prefix>-adsb.bin` (one file per source) — read at once. "" = memory only. */
    void setFiles(const std::string& prefix) {
        std::lock_guard<std::mutex> lk(m_);
        for (int i = 0; i < kRangeSrcN; ++i) {
            path_[i] = prefix.empty() ? std::string() : prefix + "-" + rangeSrcName(i) + ".bin";
            RangeOutline r(o_[i].maxKm());
            if (!path_[i].empty() && r.load(path_[i])) o_[i] = std::move(r);
            dirty_[i] = false;
        }
    }
    /** The receiver's position (the server's location). Moved more than kMovedKm: every outline starts again. */
    void setCentre(double lat, double lon, int64_t utc) {
        std::lock_guard<std::mutex> lk(m_);
        for (int i = 0; i < kRangeSrcN; ++i) {
            const bool had = o_[i].hasCentre();
            if (o_[i].setCentre(lat, lon, utc) || !had) { dirty_[i] = true; saveLocked(i, utc); }
        }
    }
    /** One believed position from source `src` (the caller's own plausibility rule first). */
    bool offer(int src, double lat, double lon, int64_t utc) {
        if (src < 0 || src >= kRangeSrcN) return false;
        std::lock_guard<std::mutex> lk(m_);
        if (!o_[src].offer(lat, lon, utc)) return false;
        dirty_[src] = true;
        if (utc - saved_[src] >= kSaveEverySec) saveLocked(src, utc);
        return true;
    }
    void flush(int64_t utc) { std::lock_guard<std::mutex> lk(m_); for (int i = 0; i < kRangeSrcN; ++i) if (dirty_[i]) saveLocked(i, utc); }
    /** The owner's reset: one source, or all (src < 0). Saved at once. */
    void clear(int src, int64_t utc) {
        std::lock_guard<std::mutex> lk(m_);
        for (int i = 0; i < kRangeSrcN; ++i) if (src < 0 || src == i) { o_[i].clear(utc); dirty_[i] = true; saveLocked(i, utc); }
    }
    /** The owner's window (7 / 30 / 90 / 0 = all time): one source, or all (src < 0). false = not a window offered. */
    bool setWindow(int src, int days, int64_t utc) {
        if (!RangeOutline::validWindow(days)) return false;
        std::lock_guard<std::mutex> lk(m_);
        for (int i = 0; i < kRangeSrcN; ++i) if (src < 0 || src == i) { o_[i].setWindow(days); dirty_[i] = true; saveLocked(i, utc); }
        return true;
    }
    std::string json(int src, int64_t utc) {
        std::lock_guard<std::mutex> lk(m_);
        if (src >= 0 && src < kRangeSrcN) return o_[src].json(utc);
        std::string o = "{\"ok\":1,\"outlines\":{";
        for (int i = 0; i < kRangeSrcN; ++i) { if (i) o += ','; o += std::string("\"") + rangeSrcName(i) + "\":" + o_[i].json(utc); }
        return o + "}}";
    }
    RangeOutline snapshot(int src) { std::lock_guard<std::mutex> lk(m_); return o_[src]; }

private:
    void saveLocked(int i, int64_t utc) {
        saved_[i] = utc;
        if (path_[i].empty()) { dirty_[i] = false; return; }
        if (o_[i].save(path_[i])) dirty_[i] = false;
    }
    std::mutex m_;
    RangeOutline o_[kRangeSrcN];
    std::string path_[kRangeSrcN];
    bool dirty_[kRangeSrcN] = {};
    int64_t saved_[kRangeSrcN] = {};
};
/** This radio process's outlines. */
inline RangeStore& rangeStore() { static RangeStore s; return s; }

} // namespace vibeadsb
