// vibe_sbs.h — V12 beta96: BASESTATION (SBS-1) TEXT FEEDS read into the shared aircraft table (docs/v12/ADSB-FEED.md
// §"BaseStation / HFDL from UberSDR (dumphfdl)"). Stuart 2026-11-26: his UberSDR runs dumphfdl, which sends decoded HFDL
// aircraft as BaseStation text to a TCP address it CONNECTS to; he wants them on the AIR & SEA map and the community map,
// labelled HFDL.
//
// ★★★ THE FORMAT, OWN CODE, from the published BaseStation description: one line per message, comma separated, 22 fields —
//      1 "MSG"  2 transmission type 1..8  3 session  4 aircraft id  5 ICAO hex  6 flight id  7/8 date/time generated
//      9/10 date/time logged  11 callsign  12 altitude ft  13 ground speed kt  14 track °  15 latitude  16 longitude
//      17 vertical rate ft/min  18 squawk  19 alert (squawk change)  20 emergency  21 SPI (ident)  22 on the ground.
//    The flags are "-1" (true) or "0" in BaseStation's own output; some programs write "1" — both read as true. Lines end
//    in LF or CRLF. Other record kinds (SEL, ID, AIR, STA, CLK) carry nothing we draw and are skipped, counted.
// ★★ WHAT dumphfdl SENDS (read 2026-11-26 from its README and its basestation formatter): ONLY "MSG,3" lines (airborne
//    position) — the ICAO address as 6 upper-case hex digits, the callsign when known (HFDL's are 6 characters), latitude
//    and longitude with 6 decimals, altitude / speed / track EMPTY, squawk "0" (or the HFDL channel in kHz with its
//    --freq-as-squawk), on-ground "0", and the position's own time in the "generated" fields. A line is written only for an
//    aircraft whose ICAO address it knows (from a logon) and only from positions under 5 minutes old. Nothing in a line
//    says "HFDL" — so the OWNER says what a source is (its label), and an HFDL source is never counted as ADS-B.
// ★★ ROBUST: a line is at most kMaxLine bytes (longer is junk: dropped up to its LF, counted); a line that is not a well
//    formed MSG record is counted and skipped; numbers are checked (finite, in range); an address of all zeros, or one
//    marked non-ICAO ('~' first, as readsb writes TIS-B / anonymous ones), is skipped. Never throws, never grows.
// ★ POSITIONS ARE ABSOLUTE (no CPR): checked for range, for (0,0), and for a jump no aircraft could fly (over 1,200 kt
//   since the row's last fix — the fix is dropped, counted, and the row keeps its old one).
// ★ BOUNDED: kMaxRows aircraft per source; the row heard longest ago gives its place up. Rows unheard for an hour (a
//   datalink source; ADS-B: 5 min) are forgotten.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include "vibe_aircraft_share.h"

namespace vibe {
namespace sbs {

constexpr size_t kMaxLine = 512;      ///< a BaseStation line is ~120 bytes; anything past this is not one
constexpr size_t kMaxRows = 2000;     ///< aircraft per source

/** One parsed MSG record. Only the fields the line carried are set (has*). */
struct Msg {
    int type = 0;                      ///< 1..8
    uint32_t icao = 0;
    std::string callsign;
    bool hasAlt = false; int altFt = 0;
    bool hasGs = false; float gs = 0;
    bool hasTrk = false; float trk = 0;
    bool hasPos = false; double lat = NAN, lon = NAN;
    bool hasVr = false; int vr = 0;
    int squawk = -1;                   ///< as written (decimal digits), -1 none
    bool emergency = false, ground = false, hasGround = false;
    int64_t genMs = 0;                 ///< the "generated" date/time as UTC ms, 0 = none or unreadable
};

/** Counters: what came in and what was refused. */
struct Stats {
    uint64_t lines = 0, msgs = 0, skipped = 0, bad = 0, longLines = 0, badPos = 0;
};

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}
/** Strict decimal number (optional sign, digits, one dot). False on anything else, NaN or inf. */
inline bool num(const std::string& t, double& v) {
    if (t.empty() || t.size() > 24) return false;
    size_t i = 0; bool dig = false, dot = false;
    if (t[0] == '-' || t[0] == '+') i = 1;
    for (; i < t.size(); ++i) {
        if (t[i] >= '0' && t[i] <= '9') dig = true;
        else if (t[i] == '.' && !dot) dot = true;
        else return false;
    }
    if (!dig) return false;
    v = std::strtod(t.c_str(), nullptr);
    return std::isfinite(v);
}
inline bool flag(const std::string& t) { return t == "-1" || t == "1"; }
/** "YYYY/MM/DD" + "HH:MM:SS[.fff]" read as UTC → ms; 0 when either is unreadable. */
inline int64_t utcMs(const std::string& d, const std::string& t) {
    int Y, M, D, h, m; double s;
    if (std::sscanf(d.c_str(), "%4d/%2d/%2d", &Y, &M, &D) != 3) return 0;
    if (std::sscanf(t.c_str(), "%2d:%2d:%lf", &h, &m, &s) != 3) return 0;
    if (Y < 2000 || Y > 2200 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59 || !(s >= 0 && s < 61)) return 0;
    // days from the civil date (UTC; no time zone library)
    const int y = Y - (M <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = unsigned((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
    return (days * 86400 + h * 3600 + m * 60) * 1000 + int64_t(std::llround(s * 1000));
}

/** Parse one line (no LF). True for a usable MSG record; false (and nothing set) otherwise. `other` = a well formed record
 *  of another kind (SEL/ID/AIR/STA/CLK) — skipped, not junk. */
inline bool parseLine(const std::string& line, Msg& m, bool* other = nullptr) {
    if (other) *other = false;
    if (line.size() < 4 || line.size() > kMaxLine) return false;
    std::vector<std::string> f;
    f.reserve(22);
    size_t a = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') { f.push_back(trim(line.substr(a, i - a))); a = i + 1; if (f.size() > 30) return false; }
    }
    if (f.empty()) return false;
    if (f[0] != "MSG") {
        if (other && (f[0] == "SEL" || f[0] == "ID" || f[0] == "AIR" || f[0] == "STA" || f[0] == "CLK")) *other = true;
        return false;
    }
    if (f.size() < 11) return false;   // up to the callsign at least; later fields may be cut short by a writer
    while (f.size() < 22) f.emplace_back();
    Msg r;
    if (f[1].size() != 1 || f[1][0] < '1' || f[1][0] > '8') return false;
    r.type = f[1][0] - '0';
    const std::string& hx = f[4];
    if (hx.size() != 6) return false;   // ★ "~ABCDEF" (non-ICAO) and anything else: not an aircraft we can name
    uint32_t ic = 0;
    for (char c : hx) {
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        ic = ic << 4 | uint32_t(v);
    }
    if (!ic || ic == 0xFFFFFF) return false;
    r.icao = ic;
    r.genMs = utcMs(f[6], f[7]);
    // callsign: printable, no spaces inside, ≤ 8
    {
        std::string c;
        for (char ch : f[10]) if (ch > 0x20 && ch < 0x7F && ch != '"' && ch != '\\') c += ch;
        if (c.size() <= 8) r.callsign = c;
    }
    double v;
    if (num(f[11], v) && v >= -2000 && v <= 100000) { r.hasAlt = true; r.altFt = int(std::lround(v)); }
    if (num(f[12], v) && v >= 0 && v <= 4000) { r.hasGs = true; r.gs = float(v); }
    if (num(f[13], v) && v >= 0 && v <= 360) { r.hasTrk = true; r.trk = float(std::fmod(v, 360.0)); }
    double la, lo;
    if (num(f[14], la) && num(f[15], lo)) {
        if (std::fabs(la) <= 90 && std::fabs(lo) <= 180 && !(la == 0 && lo == 0)) { r.hasPos = true; r.lat = la; r.lon = lo; }
    }
    if (num(f[16], v) && std::fabs(v) <= 40000) { r.hasVr = true; r.vr = int(std::lround(v)); }
    if (!f[17].empty() && f[17].size() <= 5) {
        bool d = true; for (char ch : f[17]) if (ch < '0' || ch > '9') d = false;
        if (d) r.squawk = std::atoi(f[17].c_str());
    }
    r.emergency = flag(f[19]);
    if (!f[21].empty()) { r.hasGround = true; r.ground = flag(f[21]); }
    m = std::move(r);
    return true;
}

/** Splits a TCP byte stream into lines, bounded. Bytes that are not text (0x00, 0x1A…) make a line junk. */
class LineSplitter {
public:
    /** Feed bytes; each complete line goes to `f(line)`. Returns how many lines were handed on. */
    template <class F> size_t feed(const uint8_t* p, size_t n, F&& f) {
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) {
            const char c = char(p[i]);
            if (c == '\n') {
                if (!junk_ && !buf_.empty()) { f(buf_); ++k; }
                else if (junk_) ++junkLines_;
                buf_.clear(); junk_ = false; continue;
            }
            if (junk_) continue;
            if ((unsigned char)c < 0x20 && c != '\r' && c != '\t') { junk_ = true; buf_.clear(); continue; }
            if (buf_.size() >= kMaxLine) { junk_ = true; ++long_; buf_.clear(); continue; }
            buf_ += c;
        }
        return k;
    }
    uint64_t longLines() const { return long_; }
    uint64_t junkLines() const { return junkLines_; }
    void reset() { buf_.clear(); junk_ = false; }
private:
    std::string buf_;
    bool junk_ = false;
    uint64_t long_ = 0, junkLines_ = 0;
};

/** ★ The source's own label: which bit of the shared table its aircraft carry. */
inline int labelSrc(const std::string& label) {
    if (label == "hfdl") return vibeavshare::kHfdl;
    if (label == "vdl2") return vibeavshare::kVdl2;
    if (label == "acars") return vibeavshare::kAcars;
    return vibeavshare::kAdsb;
}
inline const char* labelWords(const std::string& label) {
    if (label == "hfdl") return "HFDL";
    if (label == "vdl2") return "VDL2";
    if (label == "acars") return "ACARS";
    return "ADS-B";
}

/** ★★ ONE SOURCE'S AIRCRAFT, keyed by ICAO address — handed to the shared table as updates tagged with the source's label.
 *  Not thread-safe: the owner holds a lock. Times are wall-clock ms. */
class Table {
public:
    explicit Table(const std::string& label = "adsb") { setLabel(label); }
    void setLabel(const std::string& l) { label_ = l; src_ = labelSrc(l); }
    const std::string& label() const { return label_; }
    /** ★ A position's age limit for this source: ADS-B's own minute; a datalink's 30 min (they report sparsely — the
     *  community feed's kAircraftDatalinkAgeSec). */
    int64_t posKeepMs() const { return src_ == vibeavshare::kAdsb ? 60 * 1000 : 30 * 60 * 1000; }
    int64_t rowKeepMs() const { return src_ == vibeavshare::kAdsb ? 5 * 60 * 1000 : 3600 * 1000; }

    /** Apply one record heard at `nowMs`. False when the record changed nothing usable (a refused position counts). */
    bool apply(const Msg& m, int64_t nowMs) {
        auto it = rows_.find(m.icao);
        if (it == rows_.end()) {
            if (rows_.size() >= kMaxRows) evictOldest_();
            it = rows_.emplace(m.icao, Row()).first;
            it->second.firstMs = nowMs;
        }
        Row& r = it->second;
        r.heardMs = nowMs; ++r.msgs;
        if (!m.callsign.empty()) r.callsign = m.callsign;
        if (m.hasAlt) { r.hasAlt = true; r.altFt = m.altFt; }
        if (m.hasGs) { r.hasGs = true; r.gs = m.gs; }
        if (m.hasTrk) { r.hasTrk = true; r.trk = m.trk; }
        if (m.hasGround) r.ground = m.ground;
        if (src_ == vibeavshare::kAdsb) {   // ★ HFDL has no squawk (dumphfdl writes 0, or the channel with --freq-as-squawk)
            if (m.squawk > 0 && m.squawk <= 7777) r.squawk = m.squawk;
            r.emergency = m.emergency || r.squawk == 7500 || r.squawk == 7600 || r.squawk == 7700;
        } else if (src_ == vibeavshare::kHfdl && m.squawk >= 2000 && m.squawk <= 30000) r.khz = m.squawk;   // the HF channel, kHz
        if (m.hasPos) {
            // ★ the position's own time when the writer gave a believable one (dumphfdl: the fix's time); else arrival
            int64_t at = nowMs;
            if (m.genMs > 0 && m.genMs <= nowMs + 60 * 1000 && nowMs - m.genMs <= posKeepMs()) at = std::min(m.genMs, nowMs);
            if (r.hasPos && at < r.posMs) return true;   // an older fix than the one held: keep the newer
            if (r.hasPos) {
                const double dtH = std::max(1.0, double(at - r.posMs) / 1000.0) / 3600.0;
                if (distNm(r.lat, r.lon, m.lat, m.lon) / dtH > 1200.0 && double(at - r.posMs) < 3600e3) { ++badPos_; return false; }
            }
            r.hasPos = true; r.lat = m.lat; r.lon = m.lon; r.posMs = at;
        }
        return true;
    }
    /** Forget rows heard too long ago. */
    void expire(int64_t nowMs) {
        for (auto it = rows_.begin(); it != rows_.end();) it = nowMs - it->second.heardMs > rowKeepMs() ? rows_.erase(it) : std::next(it);
    }
    /** The rows heard since `sinceMs`, as the shared table's updates (vibe_aircraft_share.h), tagged with this source's bit. */
    void shareSince(int64_t sinceMs, int64_t nowMs, std::vector<vibeavshare::Update>& out, const std::string& via) {
        for (auto& kv : rows_) {
            Row& r = kv.second;
            if (r.heardMs < sinceMs) continue;
            vibeavshare::Update u;
            u.icao = kv.first; u.flight = r.callsign;
            u.srcMask = uint8_t(1u << src_); u.srcMs[src_] = r.heardMs; u.heardMs = r.heardMs;
            if (src_ == vibeavshare::kHfdl && r.khz > 0) { char b[40]; std::snprintf(b, sizeof b, "HFDL %d", r.khz); u.via = b; }
            else u.via = via;
            if (r.hasPos && nowMs - r.posMs <= posKeepMs()) {
                u.hasPos = true; u.lat = r.lat; u.lon = r.lon; u.posMs = r.posMs; u.posSrc = src_;
                u.hasAlt = r.hasAlt; u.altFt = r.altFt; u.hasTrk = r.hasTrk; u.trk = r.trk; u.hasGs = r.hasGs; u.gs = r.gs;
                u.ground = r.ground;
            } else if (r.hasAlt) { u.hasAlt = true; u.altFt = r.altFt; }
            u.count = r.msgs - r.msgsShared; r.msgsShared = r.msgs;
            out.push_back(std::move(u));
            if (out.size() >= vibeavshare::kCap) break;
        }
    }
    size_t size() const { return rows_.size(); }
    /** Aircraft heard within `withinMs` (the status line's count). */
    size_t heardWithin(int64_t nowMs, int64_t withinMs) const {
        size_t n = 0; for (const auto& kv : rows_) if (nowMs - kv.second.heardMs <= withinMs) ++n; return n;
    }
    uint64_t badPositions() const { return badPos_; }
    void clear() { rows_.clear(); }
    struct Row {
        std::string callsign;
        bool hasAlt = false; int altFt = 0;
        bool hasGs = false; float gs = 0;
        bool hasTrk = false; float trk = 0;
        bool hasPos = false; double lat = 0, lon = 0; int64_t posMs = 0;
        bool ground = false, emergency = false;
        int squawk = -1, khz = 0;
        int64_t firstMs = 0, heardMs = 0;
        uint32_t msgs = 0, msgsShared = 0;
    };
    const std::unordered_map<uint32_t, Row>& rows() const { return rows_; }

private:
    static double distNm(double la1, double lo1, double la2, double lo2) {
        const double k = M_PI / 180.0;
        const double dla = (la2 - la1) * k, dlo = (lo2 - lo1) * k;
        const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * k) * std::cos(la2 * k) * std::sin(dlo / 2) * std::sin(dlo / 2);
        return 2 * 3440.065 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1 - a)));
    }
    void evictOldest_() {
        auto old = rows_.begin();
        for (auto it = rows_.begin(); it != rows_.end(); ++it) if (it->second.heardMs < old->second.heardMs) old = it;
        if (old != rows_.end()) rows_.erase(old);
    }
    std::string label_;
    int src_ = vibeavshare::kAdsb;
    std::unordered_map<uint32_t, Row> rows_;
    uint64_t badPos_ = 0;
};

}  // namespace sbs
}  // namespace vibe
