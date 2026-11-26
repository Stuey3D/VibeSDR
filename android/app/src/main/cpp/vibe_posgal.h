// vibe_posgal.h — ★★★ beta96 maps2: STORED POSITIONS IN THE GALLERY — the day's radiosondes (SONDES) and VOR stations
// (the VOR MAP) as a standing day card each, so the AIR & SEA map can draw them, faded by age, when the mode is NOT running
// (docs/v12/ONE-MAP.md: "Every other layer shows what this server has STORED"). The ISM nodes card's pattern exactly
// (vibe_ism_gallery.h "STATIONS RECEIVED"): one card per kind per UTC day, a readable .txt whose table the clients parse
// (src/utils/storedLayers.ts galPosParse — keep the two in step), found again and read back after a restart.
//
// ★★ SD WEAR (Stuart's rule for continuous decoders): never a write per report. Rows are held in RAM and the card is
//    rewritten in place (tmp + rename, Store::replace) at most every kCardSec and only when something changed, once more
//    when the mode stops (finish) and at a UTC midnight. A power cut loses at most kCardSec of positions.
// ★★ BOUNDED: kMaxRows rows (the least recently heard go, counted on the card); a row is a few fields — no track.
// ★ Public data only: a radiosonde's serial, type and position (meteorological broadcast, published by every tracker);
//   a VOR's published ident, name and site. Nothing else.
#pragma once
#include "vibe_gallery.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vibe {
namespace posgal {

static const char* const kDot = " \xc2\xb7 ";   // " · "
/** ★ beta96 the heading of the card's SNR block (body / restore; src/services/storedLayers.ts galPosParse reads it too). */
static const char* const kSnrHead = "SNR, dB (each one's last frame, in the receiver's scan box):";

/** One thing heard at a place. `id` keys the row (a sonde's serial; a VOR's ident and channel). */
struct Row {
    std::string id, name, type, cc;
    double lat = NAN, lon = NAN, altM = NAN, topM = NAN, mhz = 0;
    int64_t first = 0, last = 0;
    double snr = NAN;   ///< ★ beta96 the last frame's SNR, dB (sondes) — the card's own block below the table, NAN = none
};

/** What the card is: the gallery kind, the slot's prefix (the clients know a card by it), the words. */
struct Spec {
    std::string kind;       ///< the gallery kind: "sondes" | "vormap"
    std::string slot;       ///< "sondes" → "sondes-2026-11-26"; "vor" → "vor-2026-11-26"
    std::string heading;    ///< "Radiosondes heard"
    std::string one, many;  ///< "sonde" / "sondes"
    std::string mode;       ///< "SONDES" — "while SONDES runs"
    std::string by;         ///< "SONDES mode" — "Heard by this server's SONDES mode."
    std::string dl;         ///< the download's name before the date: "Radiosondes"
};
inline Spec sondesSpec() { return { "sondes", "sondes", "Radiosondes heard", "sonde", "sondes", "SONDES", "SONDES mode", "Radiosondes" }; }
inline Spec vorSpec() { return { "vormap", "vor", "VOR stations heard", "station", "stations", "the VOR map", "VOR map", "VOR stations" }; }

/** A name as it may stand in one field: printable, no " · " inside, "-" when empty. */
inline std::string field(const std::string& s, size_t max = 40) {
    std::string o = gallery::printable(s, max), r;
    for (size_t i = 0; i < o.size(); ++i) {
        if ((unsigned char)o[i] == 0xC2 && i + 1 < o.size() && (unsigned char)o[i + 1] == 0xB7) { r += '-'; ++i; continue; }
        r += o[i];
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    while (!r.empty() && r.front() == ' ') r.erase(r.begin());
    return r.empty() ? std::string("-") : r;
}

class DayCard {
public:
    static constexpr int64_t kCardSec = 300;    // ★ rewritten at most this often (and at once when the mode stops)
    static constexpr size_t  kMaxRows = 300;    // ★ the table (the least recently heard go)

    struct Options {
        Spec spec;
        std::function<int64_t()> clock;   ///< UTC seconds (tests); empty = the wall clock
    };
    DayCard(gallery::Store& store, Options o) : store_(store), o_(std::move(o)) {
        std::lock_guard<std::mutex> lk(m_);
        openDay(now() / 86400);
    }
    ~DayCard() { finish(); }

    /** A row seen (its newest position wins; first heard kept). Never a write here: tick() writes. */
    void seen(const Row& r) {
        if (r.id.empty() || !std::isfinite(r.lat) || !std::isfinite(r.lon) || std::fabs(r.lat) > 90 || std::fabs(r.lon) > 180) return;
        if (r.lat == 0 && r.lon == 0) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        rollIfNewDay(t);
        const int64_t last = r.last > 0 ? std::min(r.last, t) : t;
        if (last / 86400 != day_) return;   // ★ a row last heard on another day belongs on that day's card
        Row& w = rows_[field(r.id, 24)];   // ★ keyed as written, so a card read back after a restart finds the same row
        const bool fresh = w.id.empty();
        if (!fresh && last <= w.last && r.lat == w.lat && r.lon == w.lon) return;
        const int64_t first = fresh ? (r.first > 0 && r.first / 86400 == day_ ? std::min(r.first, last) : last) : w.first;
        const double top = std::isfinite(w.topM) ? (std::isfinite(r.topM) ? std::max(w.topM, r.topM) : w.topM) : r.topM;
        w = r;
        w.id = field(r.id, 24); w.name = field(r.name); w.type = field(r.type, 24); w.cc = field(r.cc, 4);
        w.first = first; w.last = std::max(last, w.first); w.topM = top;
        dirty_ = true;
        while (rows_.size() > kMaxRows) {
            auto old = rows_.begin();
            for (auto it = rows_.begin(); it != rows_.end(); ++it) if (it->second.last < old->second.last) old = it;
            rows_.erase(old);
            dropped_++;
        }
    }
    void tick() {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        rollIfNewDay(t);
        if (dirty_ && t - written_ >= kCardSec) write(t);
    }
    void finish() {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        if (dirty_) write(now());
        if (!name_.empty()) store_.close(name_);
    }
    // ── test seams ──
    std::string text() const { std::lock_guard<std::mutex> lk(m_); return body(now()); }
    std::string itemName() const { std::lock_guard<std::mutex> lk(m_); return name_; }
    size_t rows() const { std::lock_guard<std::mutex> lk(m_); return rows_.size(); }
    std::string titleNow() const { std::lock_guard<std::mutex> lk(m_); return title(); }

private:
    int64_t now() const { return o_.clock ? o_.clock() : gallery::nowUtc(); }
    std::string date(int64_t day) const { return gallery::utcStamp(day * 86400, "%Y-%m-%d"); }
    std::string slot() const { return o_.spec.slot + "-" + date(day_); }
    gallery::Meta meta() const {
        gallery::Meta m;
        m.kind = o_.spec.kind; m.origin = "live"; m.decoder = o_.spec.kind; m.mode = "";
        m.freqHz = 0;   // ★ a day's card of many channels — and the slot is found again by it (Store::adoptSlot matches the dial)
        m.utc = day_ * 86400;   // ★ a DAY's item: listed at the day it covers
        m.slot = slot();
        return m;
    }
    void openDay(int64_t day) {
        day_ = day;
        rows_.clear(); dropped_ = 0; dirty_ = false; name_.clear();
        written_ = now();
        name_ = store_.adoptSlot(meta(), /*anyOrigin=*/true);
        if (!name_.empty()) { std::string b; if (store_.readItem(name_, b)) restore(b); }
    }
    void rollIfNewDay(int64_t t) {
        if (t / 86400 == day_) return;
        if (dirty_) write(t);
        if (!name_.empty()) store_.close(name_);
        openDay(t / 86400);
    }
    std::string title() const {
        const size_t n = rows_.size() + dropped_;
        std::string t = o_.spec.heading + kDot + date(day_) + ": " + std::to_string(n) + " " + (n == 1 ? o_.spec.one : o_.spec.many);
        double top = NAN;
        for (const auto& kv : rows_) if (std::isfinite(kv.second.topM) && !(kv.second.topM <= top)) top = kv.second.topM;
        if (std::isfinite(top)) { char b[48]; std::snprintf(b, sizeof b, "highest %.0f m", top); if (t.size() + 4 + std::strlen(b) <= 120) t += kDot + std::string(b); }
        return t;
    }
    static std::string num(double v, const char* fmt) { if (!std::isfinite(v)) return "-"; char b[32]; std::snprintf(b, sizeof b, fmt, v); return b; }
    /** ★ The table the clients read: 10 fields, " · " between (storedLayers.ts galPosParse). */
    std::string body(int64_t t) const {
        std::string o = o_.spec.heading + kDot + date(day_) + " (UTC)\n";
        o += "Heard by this server's " + o_.spec.by + ".\n";
        o += "Updated " + gallery::utcStamp(t, "%H:%M UTC") + " \xe2\x80\x94 at most every 5 minutes while " + o_.spec.mode
           + " runs. The AIR & SEA map shows them, faded by age, when " + o_.spec.mode + " is not running.\n";
        if (dropped_) o += "Not listed:   " + std::to_string((unsigned long long)dropped_) + " (this card keeps the " + std::to_string(kMaxRows) + " most recently heard)\n";
        o += "\nEach, most recent first (last heard \xc2\xb7 first heard \xc2\xb7 id \xc2\xb7 name \xc2\xb7 type \xc2\xb7 position \xc2\xb7 altitude m \xc2\xb7 highest m \xc2\xb7 MHz \xc2\xb7 country):\n";
        std::vector<const Row*> v;
        for (const auto& kv : rows_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Row* a, const Row* b) { return a->last != b->last ? a->last > b->last : a->id < b->id; });
        for (const Row* r : v) {
            char pos[64];
            std::snprintf(pos, sizeof pos, "%.4f, %.4f", r->lat, r->lon);
            o += gallery::utcStamp(r->last, "%H:%M:%S") + kDot + gallery::utcStamp(r->first, "%H:%M:%S") + kDot + r->id + kDot + r->name + kDot
               + r->type + kDot + pos + kDot + num(r->altM, "%.0f") + kDot + num(r->topM, "%.0f") + kDot + num(r->mhz > 0 ? r->mhz : NAN, "%.3f")
               + kDot + r->cc + "\n";
        }
        /* ★★ beta96 (Stuart 11-26: "yes save snr history") EACH ROW'S LAST SNR — a block of its own BELOW the table, two
         *  fields a line ("id · dB"), so a reader of the 10-field table (an older client's galPosParse, an older server's
         *  restore) skips these lines as it skips any line that is not a row. Only rows that have one. */
        bool any = false;
        for (const Row* r : v) if (std::isfinite(r->snr)) {
            if (!any) { o += "\n" + std::string(kSnrHead) + "\n"; any = true; }
            char b[24]; std::snprintf(b, sizeof b, "%.1f", r->snr);
            o += r->id + kDot + b + "\n";
        }
        return o;
    }
    /** ★ The card is its own record: a damaged row costs that row, never the process. */
    void restore(const std::string& b) {
        const size_t at = b.find("Each, most recent first");
        if (at == std::string::npos) return;
        auto hms = [&](const std::string& s, int64_t& out) {
            int h, m, sec;
            if (s.size() != 8 || std::sscanf(s.c_str(), "%2d:%2d:%2d", &h, &m, &sec) != 3) return false;
            out = day_ * 86400 + h * 3600 + m * 60 + sec;
            return true;
        };
        auto dbl = [](const std::string& s) { return s == "-" ? NAN : std::atof(s.c_str()); };
        const std::string dot = kDot;
        size_t p = b.find('\n', at);
        while (p != std::string::npos && p + 1 < b.size()) {
            const size_t e = b.find('\n', p + 1);
            const std::string row = b.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
            p = e;
            std::vector<std::string> f; size_t q = 0;
            for (;;) { const size_t d = row.find(dot, q); if (d == std::string::npos) { f.push_back(row.substr(q)); break; } f.push_back(row.substr(q, d - q)); q = d + dot.size(); }
            if (f.size() != 10 || f[2].empty() || f[2] == "-") continue;
            Row r;
            if (!hms(f[0], r.last) || !hms(f[1], r.first)) continue;
            if (std::sscanf(f[5].c_str(), "%lf, %lf", &r.lat, &r.lon) != 2 || std::fabs(r.lat) > 90 || std::fabs(r.lon) > 180) continue;
            r.id = f[2]; r.name = f[3]; r.type = f[4]; r.altM = dbl(f[6]); r.topM = dbl(f[7]);
            const double mhz = dbl(f[8]); r.mhz = std::isfinite(mhz) ? mhz : 0; r.cc = f[9];
            if (rows_.size() < kMaxRows) rows_[r.id] = r;
        }
        // ★ beta96 the SNR block (body): "id · dB" lines after its heading
        if (const size_t sp = b.find(kSnrHead); sp != std::string::npos) {
            size_t p2 = b.find('\n', sp);
            while (p2 != std::string::npos && p2 + 1 < b.size()) {
                const size_t e = b.find('\n', p2 + 1);
                const std::string ln = b.substr(p2 + 1, e == std::string::npos ? std::string::npos : e - p2 - 1);
                p2 = e;
                const size_t d = ln.find(dot);
                if (d == std::string::npos || ln.find(dot, d + dot.size()) != std::string::npos) continue;
                auto it = rows_.find(ln.substr(0, d));
                const double v = std::atof(ln.c_str() + d + dot.size());
                if (it != rows_.end() && std::isfinite(v) && v > -100 && v < 200) it->second.snr = v;
            }
        }
        if (const size_t np = b.find("Not listed:"); np != std::string::npos) { unsigned long long d = 0; if (std::sscanf(b.c_str() + np, "Not listed: %llu", &d) == 1) dropped_ = d; }
    }
    void write(int64_t t) {
        written_ = t; dirty_ = false;
        if (rows_.empty()) return;
        gallery::Meta m = meta();
        m.title = title();
        m.dlName = o_.spec.dl + " " + date(day_) + ".txt";
        const std::string b = body(t);
        if (name_.empty() || !store_.replace(name_, m, b, /*toNewest=*/true)) {
            if (!name_.empty()) store_.close(name_);
            name_ = store_.create(m, "txt", b);
        }
    }

    gallery::Store& store_;
    Options o_;
    mutable std::mutex m_;
    bool done_ = false, dirty_ = false;
    int64_t day_ = 0, written_ = 0;
    std::map<std::string, Row> rows_;
    uint64_t dropped_ = 0;
    std::string name_;
};

} // namespace posgal
} // namespace vibe
