// vibe_ndb_gallery.h — NDB IN THE GALLERY: each UTC day's HEARD LOG (a card, rewritten in place) and its MAP (a picture),
// whoever was listening — a listener in NDB mode or background decoding. docs/v12/NDB-SKIMMER.md.
//
//  • THE DAY'S LOG — "NDB 2026-10-26 heard.txt" when downloaded: every beacon identified that day, most recently heard
//    first, with its frequency, best SNR, distance and bearing, country and station (from the published list):
//        08:40:12 · 07:30:05 · WOD · 351.999 kHz · 26 dB · 94 km · 181° S · GB · Woodley
//  • THE DAY'S MAP — a picture: the receiver at the centre (azimuthal equidistant, so every distance and bearing is
//    true from it), range rings, the country outlines from the server's own map pack when it is installed, and every
//    beacon heard that day as a dot and its ident. Written with the card, at most every ten minutes, and when NDB stops.
//
//  • ★★ V12 2026-10-27 — THE FT8 SUMMARY'S TWO CARDS, FOR NDB (Stuart: "I'd like to add NDB to the background decoding
//    as my SDRPlay on the Lenovo already decodes Navtex and 160M FT8 in the background"): beside the day's log,
//      – LAST 10 MINUTES — ONE standing card ("NDB last 10 min · 23 beacons · furthest KER 412 km"), the rolling last ten
//        minutes, rewritten at most once a minute and only when the window changed, moved to the top, found again by the
//        next run (Store::adoptSlot); when the run ends it is written once more "to 20:20 UTC" and kept;
//      – HOURLY — one card per UTC clock hour ("NDB 19:00–20:00 UTC · 41 beacons · furthest KER 412 km · best 31 dB WOD"),
//        written when the hour closes and NEVER rewritten; a run that began or ended inside the hour writes the part it
//        heard, marked partial, with its minutes. Evicted oldest first like everything else.
//    Each lists the beacons heard with their idents, kHz, how many minutes each was heard, best SNR, distance and
//    bearing, country and station. Fed by the SAME heard() the day's log is (the service's confirmed beacons, at most
//    once a minute each) — no second store of NDB readings. The cost, stated as FT8's: each last-10 rewrite appends one
//    index line (never the index rewritten), at most once a minute; the hourly cards are one small append each.
//
// ★★ NEVER A WRITE PER BEACON (the RAM-buffered rule of every continuous decoder here, ISM's included): the card is
//    rewritten at most once a minute and only when something changed; the map at most every ten minutes. A power cut
//    loses at most that. ★ A RESTART reads the day's card back (Store::adoptSlot + readItem), so the day carries on.
#pragma once
#include "vibe_gallery.h"
#include "vibe_mapdata.h"
#include "vibe_ndb_service.h"
#include "vibe_png.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace vibe {
namespace ndbgal {

static const char* const kDot = " \xc2\xb7 ";   // " · "

// ── A 5×7 bitmap font for the map's labels (A–Z, 0–9 and a few marks) — drawn here, no font file ────────────────────
inline const uint8_t* glyph(char c) {
    static const uint8_t k[][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},                                                                   // 0-9
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
        {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
        {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x11,0x19,0x15,0x13,0x11,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x15,0x0A},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},                             // A-Z
        {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},{0x00,0x00,0x00,0x1F,0x00,0x00,0x00},{0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // . - space
    };
    if (c >= '0' && c <= '9') return k[c - '0'];
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    if (c >= 'A' && c <= 'Z') return k[10 + c - 'A'];
    if (c == '.') return k[36];
    if (c == '-') return k[37];
    return k[38];
}

/** An RGB canvas with the few primitives the map needs. */
struct Canvas {
    int w, h; std::vector<uint8_t> px;
    Canvas(int W, int H, uint8_t r, uint8_t g, uint8_t b) : w(W), h(H), px((size_t)W * H * 3) {
        for (size_t i = 0; i < px.size(); i += 3) { px[i] = r; px[i + 1] = g; px[i + 2] = b; }
    }
    void dot(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = &px[((size_t)y * w + x) * 3]; p[0] = r; p[1] = g; p[2] = b;
    }
    void line(double x0, double y0, double x1, double y1, uint8_t r, uint8_t g, uint8_t b) {
        const int n = (int)std::ceil(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)));
        if (n > 4 * (w + h)) return;   // ★ a segment that wraps the world: not drawn
        for (int i = 0; i <= n; ++i) { const double t = n ? (double)i / n : 0; dot((int)std::lround(x0 + (x1 - x0) * t), (int)std::lround(y0 + (y1 - y0) * t), r, g, b); }
    }
    void disc(int cx, int cy, int rad, uint8_t r, uint8_t g, uint8_t b) {
        for (int y = -rad; y <= rad; ++y) for (int x = -rad; x <= rad; ++x) if (x * x + y * y <= rad * rad) dot(cx + x, cy + y, r, g, b);
    }
    void text(int x, int y, const std::string& s, uint8_t r, uint8_t g, uint8_t b, int scale = 1) {
        for (char c : s) {
            const uint8_t* gl = glyph(c);
            for (int row = 0; row < 7; ++row) for (int col = 0; col < 5; ++col)
                if (gl[row] & (0x10 >> col)) for (int a = 0; a < scale; ++a) for (int bb = 0; bb < scale; ++bb) dot(x + col * scale + a, y + row * scale + bb, r, g, b);
            x += 6 * scale;
        }
    }
};

/** Azimuthal equidistant about (lat0, lon0): x east, y north, in km. */
inline void project(double lat0, double lon0, double lat, double lon, double& x, double& y) {
    double km, deg; vibendb::distBearing(lat0, lon0, lat, lon, km, deg);
    x = km * std::sin(deg * M_PI / 180.0); y = km * std::cos(deg * M_PI / 180.0);
}

/** The country outlines of a map-pack file ({"CC":[[[lon,lat],…],…],…}) as rings of points — parsed by hand (one pass,
 *  no JSON library), only once per picture. Empty when the pack is not installed: the map then has no coast. */
inline std::vector<std::vector<std::pair<float, float>>> outlines(const std::string& file) {
    std::vector<std::vector<std::pair<float, float>>> rings;
    const std::string& base = vibemap::dir();
    if (base.empty()) return rings;
    std::ifstream f(base + "/" + file, std::ios::binary);
    if (!f) return rings;
    std::stringstream ss; ss << f.rdbuf();
    const std::string s = ss.str();
    int depth = 0; std::vector<std::pair<float, float>> ring; double num[2]; int nn = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        // ★ depth 1 = a country's polygons, 2 = one polygon's ring, 3 = a [lon,lat] point
        if (c == '[') { ++depth; if (depth == 3) nn = 0; }
        else if (c == ']') {
            if (depth == 3 && nn == 2) ring.push_back({ (float)num[0], (float)num[1] });
            if (depth == 2) { if (ring.size() > 1) rings.push_back(std::move(ring)); ring.clear(); }
            --depth;
        } else if (depth == 3 && (c == '-' || (c >= '0' && c <= '9'))) {
            char* end = nullptr; const double v = std::strtod(s.c_str() + i, &end);
            if (nn < 2) num[nn++] = v;
            i = (size_t)(end - s.c_str()) - 1;
        } else if (c == '{' || c == '}') { /* the object around it */ }
    }
    return rings;
}

class DayLog : public vibendb::Sink {
public:
    static constexpr int64_t kCardSec = 60, kMapSec = 600;
    static constexpr double kSlotHz = 362500;   // ★ the items' "frequency": one day's card whatever the capture was
    struct Options {
        std::string origin;     ///< "background" | "live"
        std::string profile;
        bool havePos = false; double lat = 0, lon = 0;
        std::function<int64_t()> clock;
    };
    static constexpr int64_t kWindowSec = 10 * 60, kRefreshSec = 60, kHourSec = 3600;
    static constexpr size_t kMaxRecent = 20000;   // ★ heard events held for the window (each beacon at most once a minute)
    DayLog(gallery::Store& store, Options o) : store_(store), o_(std::move(o)) {
        std::lock_guard<std::mutex> lk(m_);
        openDay(now() / 86400);
        start_ = hourFrom_ = rollWrite_ = now();
    }
    ~DayLog() override { finish(); }

    void heard(const vibendb::HeardBeacon& b, int64_t t) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_ || b.ident.empty()) return;
        rollIfNewDay(t);
        const std::string key = b.ident + "@" + std::to_string((long long)std::lround(b.hz / 500.0));
        Row& r = rows_[key];
        if (r.first == 0) { r.first = t; newBeacon_ = true; }
        r.last = t; r.ident = b.ident; r.hz = b.hz; r.best = r.first == t ? b.snrDb : std::max(r.best, b.snrDb);
        if (b.listed) { r.name = b.name; r.cc = b.cc; r.km = b.km; r.deg = b.deg; r.lat = b.lat; r.lon = b.lon; }
        dirty_ = true;
        // ★★ …and the hour's card and the last 10 minutes (the FT8 summary's two cards, V12 2026-10-27)
        closeHourIfDue(t);
        Heard h; h.key = key; h.ident = b.ident; h.hz = b.hz; h.snr = b.snrDb; h.t = t;
        if (b.listed) { h.name = b.name; h.cc = b.cc; h.km = b.km; h.deg = b.deg; }
        HourRow& hr = hour_[key];
        if (hr.n == 0) { hr.first = t; hr.best = h.snr; }
        hr.info = h; hr.best = std::max(hr.best, h.snr); hr.last = t; ++hr.n;
        recent_.push_back(h);
        if (recent_.size() > kMaxRecent) recent_.pop_front();
        lastHeard_ = t; rollDirty_ = true;
    }
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        rollIfNewDay(t);
        if (dirty_ && t - cardAt_ >= kCardSec) writeCard(t);
        if (newBeacon_ && t - mapAt_ >= kMapSec) writeMap(t);
        closeHourIfDue(t);
        trim(t);
        if (rollDirty_ && t - rollWrite_ >= kRefreshSec) writeRolling(t, false);
    }
    void finish() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        const int64_t t = now();
        if (dirty_) writeCard(t);
        if (newBeacon_ || (mapName_.empty() && !rows_.empty())) writeMap(t);
        if (!cardName_.empty()) store_.close(cardName_);
        if (!mapName_.empty()) store_.close(mapName_);
        // ★ the hour so far (partial, with its minutes) and the last 10 minutes "to HH:MM", kept
        closeHourIfDue(t);
        writeHour(t);
        trim(t);
        writeRolling(t, true);
        if (!rollName_.empty()) store_.close(rollName_);
    }
    // ── test seams for the two summary cards ──
    std::string rollingName() const { std::lock_guard<std::mutex> lk(m_); return rollName_; }
    std::string hourText(int64_t to) const { std::lock_guard<std::mutex> lk(m_); return hourBody(to); }
    // ── test seams / the admin line ──
    size_t beacons() const { std::lock_guard<std::mutex> lk(m_); return rows_.size(); }
    std::string cardText() const { std::lock_guard<std::mutex> lk(m_); return body(now()); }
    std::string cardName() const { std::lock_guard<std::mutex> lk(m_); return cardName_; }
    std::string mapName() const { std::lock_guard<std::mutex> lk(m_); return mapName_; }
    std::string mapPng() const { std::lock_guard<std::mutex> lk(m_); return renderMap(now()); }

private:
    struct Row { std::string ident, name, cc; double hz = 0, km = -1, deg = 0, lat = NAN, lon = NAN; float best = -99; int64_t first = 0, last = 0; };
    int64_t now() const { return o_.clock ? o_.clock() : gallery::nowUtc(); }
    std::string date(int64_t day) const { return gallery::utcStamp(day * 86400, "%Y-%m-%d"); }
    gallery::Meta meta(const std::string& slot) const {
        gallery::Meta m;
        m.kind = "ndb"; m.origin = o_.origin; m.freqHz = kSlotHz; m.mode = ""; m.decoder = "ndb"; m.profile = o_.profile;
        m.utc = day_ * 86400; m.slot = slot;
        return m;
    }
    void openDay(int64_t day) {
        day_ = day; rows_.clear(); dirty_ = newBeacon_ = false; cardName_.clear(); mapName_.clear();
        cardAt_ = mapAt_ = 0;
        cardName_ = store_.adoptSlot(meta("ndbsum-" + date(day_)), /*anyOrigin=*/true);
        mapName_ = store_.adoptSlot(meta("ndbmap-" + date(day_)), /*anyOrigin=*/true);
        if (!cardName_.empty()) { std::string b; if (store_.readItem(cardName_, b)) restore(b); }
    }
    void rollIfNewDay(int64_t t) {
        if (t / 86400 == day_) return;
        if (dirty_) writeCard(t);
        if (newBeacon_) writeMap(t);
        if (!cardName_.empty()) store_.close(cardName_);
        if (!mapName_.empty()) store_.close(mapName_);
        openDay(t / 86400);
    }
    std::vector<const Row*> byLast() const {
        std::vector<const Row*> v; for (const auto& kv : rows_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Row* a, const Row* b) { return a->last != b->last ? a->last > b->last : a->ident < b->ident; });
        return v;
    }
    const Row* furthest() const { const Row* f = nullptr; for (const auto& kv : rows_) if (kv.second.km >= 0 && (!f || kv.second.km > f->km)) f = &kv.second; return f; }
    std::string title() const {
        std::string t = "NDB" + std::string(kDot) + date(day_) + kDot + std::to_string(rows_.size()) + (rows_.size() == 1 ? " beacon heard" : " beacons heard");
        if (const Row* f = furthest()) { char b[96]; std::snprintf(b, sizeof b, "%sfurthest %s %.0f km", kDot, f->ident.c_str(), f->km); t += b; }
        return t;
    }
    std::string body(int64_t t) const {
        std::string o = "NDB heard log" + std::string(kDot) + date(day_) + " (UTC)\n";
        o += std::string("Heard by ") + (o_.origin == "background" ? "background decoding" + (o_.profile.empty() ? std::string() : " (" + o_.profile + ")") : "a listener in NDB mode") + ".\n";
        o += "Updated " + gallery::utcStamp(t, "%H:%M UTC") + ". An ident is listed only once several of its repeats agreed. Stations, countries and "
             "positions: " + std::string(vibendb::kNavaidCredit) + "; a beacon not in that list shows its ident alone.\n\n";
        o += std::to_string(rows_.size()) + (rows_.size() == 1 ? " beacon" : " beacons") + " identified";
        if (const Row* f = furthest()) { char b[160]; std::snprintf(b, sizeof b, "%sfurthest: %s %s %.0f km", kDot, f->ident.c_str(), f->name.c_str(), f->km); o += b; }
        o += "\n\nEach beacon, most recently heard first (last heard \xc2\xb7 first heard \xc2\xb7 ident \xc2\xb7 kHz \xc2\xb7 best SNR \xc2\xb7 distance \xc2\xb7 bearing \xc2\xb7 country \xc2\xb7 station):\n";
        for (const Row* r : byLast()) {
            char b[400];
            std::string dist = r->km >= 0 ? std::to_string((long long)std::lround(r->km)) + " km" : std::string("-");
            std::string brg = r->km >= 0 ? std::to_string((long long)std::lround(r->deg)) + "\xc2\xb0 " + vibendb::compass(r->deg) : std::string("-");
            std::snprintf(b, sizeof b, "%s%s%s%s%s%s%.3f kHz%s%.0f dB%s%s%s%s%s%s%s%s", gallery::utcStamp(r->last, "%H:%M:%S").c_str(), kDot,
                          gallery::utcStamp(r->first, "%H:%M:%S").c_str(), kDot, r->ident.c_str(), kDot, r->hz / 1e3, kDot, r->best, kDot,
                          dist.c_str(), kDot, brg.c_str(), kDot, r->cc.empty() ? "-" : r->cc.c_str(), kDot, r->name.empty() ? "(not in the published list)" : r->name.c_str());
            o += b; o += "\n";
        }
        return o;
    }
    /** ★ The card is its own record: read the day's rows back after a restart. A row that does not parse is skipped. */
    void restore(const std::string& b) {
        const size_t at = b.find("Each beacon, most recently heard first");
        if (at == std::string::npos) return;
        auto hms = [&](const std::string& s, int64_t& out) { int h, m, sec; if (std::sscanf(s.c_str(), "%2d:%2d:%2d", &h, &m, &sec) != 3) return false; out = day_ * 86400 + h * 3600 + m * 60 + sec; return true; };
        size_t p = b.find('\n', at);
        while (p != std::string::npos && p + 1 < b.size()) {
            const size_t e = b.find('\n', p + 1);
            const std::string line = b.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
            p = e;
            std::vector<std::string> f; size_t q = 0;
            for (;;) { const size_t d = line.find(kDot, q); if (d == std::string::npos) { f.push_back(line.substr(q)); break; } f.push_back(line.substr(q, d - q)); q = d + std::strlen(kDot); }
            if (f.size() != 9) continue;
            Row r;
            if (!hms(f[0], r.last) || !hms(f[1], r.first)) continue;
            r.ident = vibendb::cleanText(f[2], 6); r.hz = std::atof(f[3].c_str()) * 1e3; r.best = (float)std::atof(f[4].c_str());
            if (r.ident.empty() || r.hz <= 0) continue;
            if (f[5] != "-") { r.km = std::atof(f[5].c_str()); r.deg = std::atof(f[6].c_str()); }
            r.cc = f[7] == "-" ? std::string() : f[7];
            r.name = f[8].rfind("(not in", 0) == 0 ? std::string() : f[8];
            // ★ the position comes back from the published list (the card does not carry it)
            for (const auto& n : vibendb::onFrequency(r.hz)) if (n.ident() == r.ident) { r.lat = n.lat(); r.lon = n.lon(); break; }
            rows_[r.ident + "@" + std::to_string((long long)std::lround(r.hz / 500.0))] = r;
        }
    }
    void writeCard(int64_t t) {
        cardAt_ = t; dirty_ = false;
        if (rows_.empty()) return;
        gallery::Meta m = meta("ndbsum-" + date(day_));
        m.title = title();
        m.dlName = "NDB " + date(day_) + " heard.txt";
        const std::string b = body(t);
        if (cardName_.empty() || !store_.replace(cardName_, m, b, /*toNewest=*/true)) {
            if (!cardName_.empty()) store_.close(cardName_);
            cardName_ = store_.create(m, "txt", b);
        }
    }
    void writeMap(int64_t t) {
        mapAt_ = t; newBeacon_ = false;
        if (rows_.empty() || !o_.havePos) return;
        const std::string png = renderMap(t);
        if (png.empty()) return;
        gallery::Meta m = meta("ndbmap-" + date(day_));
        m.title = "NDB" + std::string(kDot) + date(day_) + kDot + "map of the " + std::to_string(rows_.size()) + (rows_.size() == 1 ? " beacon heard" : " beacons heard");
        m.dlName = "NDB " + date(day_) + " map.png";
        if (mapName_.empty() || !store_.replace(mapName_, m, png, /*toNewest=*/true)) {
            if (!mapName_.empty()) store_.close(mapName_);
            mapName_ = store_.create(m, "png", png);
        }
    }
    /** ★ The picture: 960×720, the receiver at the centre, the scale set by the furthest beacon heard (≥ 150 km). */
    std::string renderMap(int64_t t) const {
        if (!o_.havePos) return "";
        const int W = 960, H = 720;
        Canvas c(W, H, 12, 16, 22);
        double far = 150;
        for (const auto& kv : rows_) if (kv.second.km > far) far = kv.second.km;
        const double R = std::min(W, H) * 0.44, k = R / (far * 1.08);
        const double cx = W / 2.0, cy = H / 2.0 + 10;
        auto toPx = [&](double lat, double lon, double& x, double& y) { double ex, ny; project(o_.lat, o_.lon, lat, lon, ex, ny); x = cx + ex * k; y = cy - ny * k; };
        // coast / borders: the regional pack for a local map, the world one beyond
        const auto rings = outlines(far < 2500 ? "tier1-countries.json" : "tier0-countries.json");
        for (const auto& r : rings) {
            double px = 0, py = 0; bool have = false;
            for (const auto& pt : r) {
                double km, deg; vibendb::distBearing(o_.lat, o_.lon, pt.second, pt.first, km, deg);
                if (km > far * 2.2) { have = false; continue; }
                double x, y; toPx(pt.second, pt.first, x, y);
                if (have) c.line(px, py, x, y, 70, 82, 96);
                px = x; py = y; have = true;
            }
        }
        // range rings, at a round step
        const double steps[] = { 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000 };
        double step = 1000; for (double s : steps) if (far / s <= 5) { step = s; break; }
        for (double rk = step; rk <= far * 1.08; rk += step) {
            const double rp = rk * k;
            for (int i = 0; i < 720; ++i) { const double a = i * M_PI / 360.0; c.dot((int)std::lround(cx + rp * std::cos(a)), (int)std::lround(cy + rp * std::sin(a)), 60, 70, 40); }
            c.text((int)std::lround(cx + 4), (int)std::lround(cy - rp - 9), std::to_string((long long)rk) + " KM", 150, 130, 70);
        }
        // the receiver
        c.line(cx - 6, cy, cx + 6, cy, 255, 255, 255); c.line(cx, cy - 6, cx, cy + 6, 255, 255, 255);
        // the beacons
        for (const auto& kv : rows_) {
            const Row& r = kv.second;
            if (!std::isfinite(r.lat) || !std::isfinite(r.lon)) continue;
            double x, y; toPx(r.lat, r.lon, x, y);
            c.line(cx, cy, x, y, 90, 70, 20);
            c.disc((int)std::lround(x), (int)std::lround(y), 4, 255, 176, 32);
            c.text((int)std::lround(x) + 7, (int)std::lround(y) - 3, r.ident, 255, 214, 120);
        }
        c.text(12, 10, "NDB HEARD " + date(day_), 255, 214, 120, 2);
        c.text(12, 32, std::to_string(rows_.size()) + " BEACONS - " + gallery::utcStamp(t, "%H.%M") + " UTC", 200, 190, 160);
        c.text(12, H - 14, "BEACON DATA OURAIRPORTS - PUBLIC DOMAIN", 120, 120, 120);
        return png::encode(W, H, 3, c.px.data());
    }

    // ── ★★ the hour's card and the last 10 minutes (V12 2026-10-27) ──────────────────────────────────────────────────
    /** One heard() — what the cards need of it, copied (the day's rows are cleared at midnight; an hour or a window is
     *  not cut there). */
    struct Heard { std::string key, ident, name, cc; double hz = 0, km = -1, deg = 0; float snr = 0; int64_t t = 0; };
    struct HourRow { Heard info; float best = -99; int n = 0; int64_t first = 0, last = 0; };
    /** One beacon's line on a summary card: ident · kHz · heard N min · best SNR · distance · bearing · country · station. */
    static std::string line(const HourRow& r) {
        char b[400];
        const Heard& h = r.info;
        const std::string dist = h.km >= 0 ? std::to_string((long long)std::lround(h.km)) + " km" : std::string("-");
        const std::string brg = h.km >= 0 ? std::to_string((long long)std::lround(h.deg)) + "\xc2\xb0 " + vibendb::compass(h.deg) : std::string("-");
        std::snprintf(b, sizeof b, "%-6s%s%.3f kHz%sheard %d min%sbest %.0f dB%s%s%s%s%s%s%s%s", h.ident.c_str(), kDot, h.hz / 1e3, kDot, r.n, kDot,
                      r.best, kDot, dist.c_str(), kDot, brg.c_str(), kDot, h.cc.empty() ? "-" : h.cc.c_str(), kDot,
                      h.name.empty() ? "(not in the published list)" : h.name.c_str());
        return b;
    }
    /** The figures and the list for a set of beacons (an hour's, or the window's). */
    std::string figures(const std::map<std::string, HourRow>& rows) const {
        std::string o;
        const HourRow* far = nullptr; const HourRow* best = nullptr;
        for (const auto& kv : rows) {
            if (kv.second.info.km >= 0 && (!far || kv.second.info.km > far->info.km)) far = &kv.second;
            if (!best || kv.second.best > best->best) best = &kv.second;
        }
        char b[300];
        std::snprintf(b, sizeof b, "Beacons heard:     %zu\n", rows.size()); o += b;
        if (far) { std::snprintf(b, sizeof b, "Furthest:          %s %s \xe2\x80\x94 %.0f km %s\n", far->info.ident.c_str(), far->info.name.c_str(), far->info.km, vibendb::compass(far->info.deg)); o += b; }
        else if (!o_.havePos) o += "Furthest:          (this server has no position set \xe2\x80\x94 add a locator in Setup)\n";
        if (best) { std::snprintf(b, sizeof b, "Best SNR:          %s %.0f dB\n", best->info.ident.c_str(), best->best); o += b; }
        std::vector<const HourRow*> v; for (const auto& kv : rows) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const HourRow* a, const HourRow* c) { return a->info.hz != c->info.hz ? a->info.hz < c->info.hz : a->info.ident < c->info.ident; });
        o += "\nEach beacon, lowest frequency first (ident \xc2\xb7 kHz \xc2\xb7 minutes heard \xc2\xb7 best SNR \xc2\xb7 distance \xc2\xb7 bearing \xc2\xb7 country \xc2\xb7 station):\n";
        for (const HourRow* r : v) { o += line(*r); o += "\n"; }
        return o;
    }
    /** "41 beacons · furthest KER 412 km · best WOD 31 dB" */
    static std::string headline(const std::map<std::string, HourRow>& rows) {
        const HourRow* far = nullptr; const HourRow* best = nullptr;
        for (const auto& kv : rows) {
            if (kv.second.info.km >= 0 && (!far || kv.second.info.km > far->info.km)) far = &kv.second;
            if (!best || kv.second.best > best->best) best = &kv.second;
        }
        std::string t = std::to_string(rows.size()) + (rows.size() == 1 ? " beacon" : " beacons");
        char b[120];
        if (far) { std::snprintf(b, sizeof b, "%sfurthest %s %.0f km", kDot, far->info.ident.c_str(), far->info.km); t += b; }
        if (best) { std::snprintf(b, sizeof b, "%sbest %s %.0f dB", kDot, best->info.ident.c_str(), best->best); t += b; }
        return t;
    }
    std::string where() const {
        return std::string("NDB summary") + kDot + "190\xe2\x80\x93" "535 kHz" + kDot
             + (o_.origin == "background" ? "background decoding" + (o_.profile.empty() ? std::string() : " (" + o_.profile + ")") : std::string("a listener in NDB mode")) + "\n";
    }
    gallery::Meta cardMeta(int64_t utc) const {
        gallery::Meta m;
        m.kind = "ndb"; m.origin = o_.origin; m.freqHz = kSlotHz; m.mode = ""; m.decoder = "ndb"; m.profile = o_.profile; m.utc = utc;
        return m;
    }
    std::string hourBody(int64_t to) const {
        const int64_t mins = (to - hourFrom_ + 30) / 60;
        const bool whole = hourFrom_ % kHourSec == 0 && to - hourFrom_ >= kHourSec;
        std::string o = where() + gallery::utcStamp(hourFrom_, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + gallery::utcStamp(to, "%H:%M UTC");
        o += whole ? std::string(" \xc2\xb7 the hour\n\n") : " \xc2\xb7 " + std::to_string((long long)mins) + " of the hour's 60 minutes\n\n";
        return o + figures(hour_);
    }
    /** The clock hour closed: its card (never rewritten), and a fresh tally from the hour's start. */
    void closeHourIfDue(int64_t t) {
        if (t / kHourSec == hourFrom_ / kHourSec) return;
        writeHour((hourFrom_ / kHourSec + 1) * kHourSec);
        hour_.clear();
        hourFrom_ = t / kHourSec * kHourSec;
    }
    void writeHour(int64_t to) {
        if (hour_.empty()) return;   // ★ nothing written for a stretch that heard nothing
        gallery::Meta m = cardMeta(hourFrom_);
        m.partial = !(hourFrom_ % kHourSec == 0 && to - hourFrom_ >= kHourSec);
        m.title = "NDB " + gallery::utcStamp(hourFrom_, "%H:%M") + "\xe2\x80\x93" + gallery::utcStamp(to, "%H:%M UTC") + kDot + headline(hour_);
        m.dlName = "NDB " + gallery::utcStamp(hourFrom_, "%Y-%m-%d %H%M") + " UTC.txt";
        store_.add(m, "txt", hourBody(to));
    }
    void trim(int64_t t) {
        while (!recent_.empty() && recent_.front().t <= t - kWindowSec) { recent_.pop_front(); rollDirty_ = true; }
    }
    /** The standing card, rewritten whole (or created, or adopted from an earlier run). `last`: the run is over. */
    void writeRolling(int64_t t, bool last) {
        rollWrite_ = t; rollDirty_ = false;
        if (recent_.empty() && rollName_.empty() && lastHeard_ == 0) return;   // ★ never heard anything: no card
        std::map<std::string, HourRow> w;
        for (const Heard& h : recent_) {
            HourRow& r = w[h.key];
            if (r.n == 0) { r.first = h.t; r.best = h.snr; }
            r.info = h; r.best = std::max(r.best, h.snr); r.last = h.t; ++r.n;
        }
        const int64_t from = std::max(start_, t - kWindowSec);
        gallery::Meta m = cardMeta(from);
        m.slot = "ndblast10";
        m.dlName = "NDB last 10 min.txt";
        const std::string to = gallery::utcStamp(t, "%H:%M UTC");
        std::string body = where();
        if (last) body += "The last 10 minutes of the run, to " + to + " (" + gallery::utcStamp(from, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + to + ")\n\n";
        else body += "The last 10 minutes: " + gallery::utcStamp(from, "%Y-%m-%d %H:%M") + " \xe2\x80\x93 " + to + " (refreshed every minute; the hourly cards are the record)\n\n";
        if (!w.empty()) {
            body += figures(w);
            m.title = std::string("NDB last 10 min") + (last ? " to " + to : std::string()) + kDot + headline(w);
        } else {
            const std::string ls = lastHeard_ ? gallery::utcStamp(lastHeard_, "%H:%M UTC") : std::string();
            body += "Nothing heard in the last 10 minutes" + (ls.empty() ? std::string() : " (the last beacon was at " + ls + ")") + ".\n";
            m.title = std::string("NDB last 10 min") + (last ? " to " + to : std::string()) + kDot + "nothing heard"
                    + (ls.empty() ? std::string() : " since " + gallery::utcStamp(lastHeard_, "%H:%M"));
        }
        if (rollName_.empty()) rollName_ = store_.adoptSlot(m);
        if (rollName_.empty() || !store_.replace(rollName_, m, body, /*toNewest=*/true)) {
            if (!rollName_.empty()) store_.close(rollName_);
            rollName_ = store_.create(m, "txt", body);
        }
    }

    gallery::Store& store_;
    Options o_;
    mutable std::mutex m_;
    bool done_ = false, dirty_ = false, newBeacon_ = false;
    int64_t day_ = 0, cardAt_ = 0, mapAt_ = 0;
    std::string cardName_, mapName_;
    std::map<std::string, Row> rows_;
    // ★ the summary cards
    int64_t start_ = 0, hourFrom_ = 0, rollWrite_ = 0, lastHeard_ = 0;
    std::map<std::string, HourRow> hour_;
    std::deque<Heard> recent_;
    bool rollDirty_ = false;
    std::string rollName_;
};

} // namespace ndbgal
} // namespace vibe
