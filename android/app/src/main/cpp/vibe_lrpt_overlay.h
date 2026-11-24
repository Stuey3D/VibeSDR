// vibe_lrpt_overlay.h — METEOR-M LRPT: the MAP OUTLINE drawn over a pass's picture (V12, 2026-11-24).
//
// ★★★ WHAT IT MAKES. Per pass, a small JSON of polylines IN THE PICTURE'S OWN PIXELS — coastlines, big lakes, country
//     borders and a 5° latitude / longitude grid with its labels — plus the picture's corners and centre as lat / lon. The
//     clients only DRAW it (a toggle over the plain picture; the saved PNG is never altered). Georeferenced by
//     vibe_lrpt_geo.h from the satellite's own line times and our SGP4 — nothing set by hand.
//
// ★★★ FAIL CLOSED, WITH A REASON. No outline unless every check passes (checkClock, chooseSatellite): the reason string
//     is plain English for the picture's caption — "no map outline: the satellite's clock reads 3 h 0 min ahead of ours".
//
// ★★ THE CHECKS
//    · the strips' stamps sit on ONE line: each within kMaxStampResidualMs of the least-squares fit (whole-ms stamps of a
//      ~1231.1 ms period step 1228 / 1232: a residual of ≤ 3 ms is normal), at least kMinStrips heard;
//    · the period is MSU-MR's (kPeriodMinMs … kPeriodMaxMs for 8 lines);
//    · the satellite's time agrees with OURS when the frames arrived, within kMaxClockSkewSec (the receiver's own queue
//      is ≤ 2 s; METEOR-M2 3 sent wrong on-board times in Oct 2023 — that is what this catches);
//    · the orbit: an element set no older than kMaxElementDays, and — with this server's location known — the satellite
//      above our horizon at the picture's middle time. With two METEORs up at once and no pick naming one: refused.
//
// ★ BOUNDED: the coast table is read-only data in the binary (vibe_lrpt_coast.h, culled by each record's box); the output
//   is capped at kMaxJsonBytes; the row table is the picture's rows. Run once at a pass's end, and for the live preview at
//   most every kLiveEverySec while it grows (vibe_lrpt_run.h).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "vibe_lrpt_coast.h"
#include "vibe_lrpt_geo.h"

namespace vibelrpt {
namespace geo {

constexpr int kMinStrips = 3;
constexpr double kMaxStampResidualMs = 8.0;
constexpr double kPeriodMinMs = 1225.0, kPeriodMaxMs = 1238.0;
constexpr double kMaxClockSkewSec = 5.0;
constexpr double kMaxElementDays = 5.0;
constexpr size_t kMaxJsonBytes = 1u << 20;
constexpr double kGridDeg = 5.0;


/** ★ The pass's clock: stamp(strip) = a + b · strip (ms, unwrapped past midnight), the UTC day it is on, how far the
 *  satellite's time was from ours. */
struct Clock {
    double dayStartUnix = 0, aMs = 0, bMs = 0, skewSec = 0, maxResidualMs = 0;
    int heard = 0;
    double unixAt(double stripF) const { return dayStartUnix + (aMs + bMs * stripF) / 1000.0; }
};

/** Plain words for a clock difference ("3 h 0 min", "41 s"). */
inline std::string spanWords(double sec) {
    sec = std::fabs(sec);
    char b[48];
    if (sec >= 3600) std::snprintf(b, sizeof b, "%d h %d min", int(sec / 3600), int(std::fmod(sec, 3600) / 60));
    else if (sec >= 60) std::snprintf(b, sizeof b, "%d min %d s", int(sec / 60), int(std::fmod(sec, 60)));
    else std::snprintf(b, sizeof b, "%.1f s", sec);
    return b;
}

/** ★★ The clock checks. `stripMs[i]` = strip i's stamp (ms of day, −1 = not heard); `wallUnix` = OUR clock (UTC) when
 *  the frame carrying stamp `wallStampMs` was decoded. */
inline bool checkClock(const std::vector<int32_t>& stripMs, double wallUnix, int32_t wallStampMs, Clock& c, std::string& why) {
    c = Clock{};
    std::vector<std::pair<double, double>> pts;
    double first = -1;
    for (size_t i = 0; i < stripMs.size(); i++) {
        if (stripMs[i] < 0) continue;
        double ms = stripMs[i];
        if (first < 0) first = ms;
        else if (ms < first - 43200000.0) ms += 86400000.0;   // ★ midnight inside the pass
        pts.push_back({ double(i), ms });
    }
    c.heard = int(pts.size());
    if (c.heard < kMinStrips) { why = "too little of the pass to place it on the map"; return false; }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (auto& p : pts) { sx += p.first; sy += p.second; sxx += p.first * p.first; sxy += p.first * p.second; }
    const double n = double(pts.size()), den = n * sxx - sx * sx;
    if (!(den > 0)) { why = "too little of the pass to place it on the map"; return false; }
    c.bMs = (n * sxy - sx * sy) / den; c.aMs = (sy - c.bMs * sx) / n;
    for (auto& p : pts) c.maxResidualMs = std::max(c.maxResidualMs, std::fabs(p.second - (c.aMs + c.bMs * p.first)));
    if (c.bMs < kPeriodMinMs || c.bMs > kPeriodMaxMs) {
        char b[160]; std::snprintf(b, sizeof b, "the satellite's line times step %.0f ms, not MSU-MR's 1231 ms", c.bMs);
        why = b; return false;
    }
    if (c.maxResidualMs > kMaxStampResidualMs) {
        char b[160]; std::snprintf(b, sizeof b, "the satellite's line times jump (one is %.0f ms off the rest)", c.maxResidualMs);
        why = b; return false;
    }
    if (!(wallUnix > 0) || wallStampMs < 0) { why = "this server's clock was not known for the pass"; return false; }
    // the UTC day: the one that puts the stamp nearest our clock
    const double day0 = std::floor(wallUnix / 86400.0) * 86400.0;
    double best = 1e18;
    for (int d = -1; d <= 1; d++) {
        const double s = day0 + d * 86400.0 + wallStampMs / 1000.0 - wallUnix;
        if (std::fabs(s) < std::fabs(best)) { best = s; c.dayStartUnix = day0 + d * 86400.0; }
    }
    c.skewSec = best;
    if (std::fabs(best) > kMaxClockSkewSec) {
        why = "the satellite's clock reads " + spanWords(best) + (best > 0 ? " ahead of" : " behind") + " ours";
        return false;
    }
    return true;
}

/** ★★ Which satellite took the picture. `obs` may be null (no location set). Returns the index, −1 + why if unsure. */
inline int chooseSatellite(const std::vector<Candidate>& cs, const vibe::sat::Observer* obs, double tMid, std::string& why) {
    if (cs.empty()) { why = "no orbit data for the METEOR satellites on this server"; return -1; }
    std::vector<int> up; int picked = -1; bool stale = false;
    for (size_t i = 0; i < cs.size(); i++) {
        const double age = std::fabs(tMid - cs[i].el.epochUnix()) / 86400.0;
        if (age > kMaxElementDays) { stale = true; continue; }
        bool visible = true;
        if (obs && obs->valid) {
            vibe::sat::Sgp4 s; if (!s.init(cs[i].el)) continue;
            const vibe::sat::Look L = vibe::sat::look(s, *obs, tMid);
            visible = L.ok && L.elDeg > -2.0;
        }
        if (!visible) continue;
        up.push_back(int(i));
        if (cs[i].picked) picked = int(i);
    }
    if (picked >= 0) return picked;
    if (up.size() == 1 && (obs && obs->valid)) return up[0];
    if (up.size() == 1 && cs.size() == 1) return up[0];
    if (up.empty()) {
        why = stale ? "the orbit data is more than " + std::to_string(int(kMaxElementDays)) + " days old"
                    : "no METEOR satellite was above this server's horizon at the picture's time";
        return -1;
    }
    why = up.size() > 1 ? "two METEOR satellites were up at once and nothing says which one this was"
                        : "this server's location is not set, so the satellite cannot be confirmed";
    return -1;
}

/** The output: `ok` + the JSON, or why not. */
struct Overlay { bool ok = false; std::string why, json; double latC = 0, lonC = 0; int points = 0; };

namespace detail {
inline void num(std::string& o, double v) { char b[24]; std::snprintf(b, sizeof b, "%.4f", v); o += b; }
inline std::string gridLabel(double deg, bool lat) {
    const int d = int(std::lround(std::fabs(deg)));
    std::string s = std::to_string(d) + "\xc2\xb0";
    if (d == 0) return s;
    return s + (lat ? (deg > 0 ? "N" : "S") : (deg > 0 ? "E" : "W"));
}
} // namespace detail

/** ★★ Build the outline for a picture of `rows` rows whose strip i (rows 8i … 8i+7) has stamp stripMs[i]. `scale`
 *  shrinks the output coordinates (the live preview is a quarter size: 0.25). */
inline Overlay makeOverlay(const std::vector<int32_t>& stripMs, int rows, double wallUnix, int32_t wallStampMs,
                           const std::vector<Candidate>& cands, const vibe::sat::Observer* obs, double scale = 1.0,
                           const Instrument& ins = Instrument{}) {
    Overlay ov;
    if (rows < 16) { ov.why = "too little of the pass to place it on the map"; return ov; }
    Clock clk;
    if (!checkClock(stripMs, wallUnix, wallStampMs, clk, ov.why)) return ov;
    const double tMid = clk.unixAt(rows / 16.0);
    const int si = chooseSatellite(cands, obs, tMid, ov.why);
    if (si < 0) return ov;
    std::vector<double> t(static_cast<size_t>(rows));
    for (int r = 0; r < rows; r++) t[size_t(r)] = clk.unixAt(r / 8.0);
    Georef g; g.ins = ins;
    if (!g.build(cands[size_t(si)].el, t, ov.why)) return ov;
    const int W = ins.width;

    // ── the footprint, for culling the coast table: lat range and lon range around the centre ──
    double cLat, cLon;
    if (!g.toGround(W / 2.0, rows / 2.0, cLat, cLon)) { ov.why = "the picture's centre is not on the Earth"; return ov; }
    double laMin = cLat, laMax = cLat, dlMin = 0, dlMax = 0;
    auto take = [&](double x, double y) {
        double la, lo; if (!g.toGround(x, y, la, lo)) return;
        double d = lo - cLon; while (d > 180) d -= 360; while (d < -180) d += 360;
        laMin = std::min(laMin, la); laMax = std::max(laMax, la); dlMin = std::min(dlMin, d); dlMax = std::max(dlMax, d);
    };
    for (int y = 0; y <= rows; y += 16) { take(0, y); take(W - 1, y); }
    for (int x = 0; x <= W; x += 98) { take(x, 0); take(x, rows - 1); }
    laMin -= 1; laMax += 1; dlMin -= 2; dlMax += 2;
    const bool allLon = laMax > 88 || laMin < -88 || dlMax - dlMin > 300;
    auto lonIn = [&](double lo0, double lo1) {   // a record's lon range against the footprint's (wrap-aware)
        if (allLon) return true;
        for (int k = -1; k <= 1; k++) { const double a = lo0 + 360.0 * k - cLon, b = lo1 + 360.0 * k - cLon; if (b >= dlMin && a <= dlMax) return true; }
        return false;
    };

    // ── polylines ──
    std::string js[4];               // coast, lake, border, grid
    std::string labels;
    size_t bytes = 0; bool full = false;
    const double minStep = 1.0;      // px (full size) between kept points
    std::vector<std::pair<double, double>> run;
    auto flush = [&](int kind) {
        if (run.size() >= 2 && !full) {
            std::string& o = js[kind];
            o += o.empty() ? "[" : ",[";
            for (size_t i = 0; i < run.size(); i++) {
                if (i) o += ',';
                char b[32]; std::snprintf(b, sizeof b, "%.0f,%.0f", run[i].first * scale, run[i].second * scale); o += b;
            }
            o += ']';
            ov.points += int(run.size());
            bytes += run.size() * 9;
            if (bytes > kMaxJsonBytes) full = true;
        }
        run.clear();
    };
    auto add = [&](int kind, double la, double lo) {
        double x, y;
        const bool in = g.toImage(la, lo, x, y, 2.0) && x >= -2 && x <= W + 2 && y >= -2 && y <= rows + 2;
        if (!in) { flush(kind); return; }
        if (!run.empty()) {
            const double dx = x - run.back().first, dy = y - run.back().second;
            if (dx * dx + dy * dy > 120.0 * 120.0) flush(kind);   // a jump (the far side, the antimeridian): break
            else if (dx * dx + dy * dy < minStep * minStep) return;
        }
        run.push_back({ x, y });
    };
    // a segment densified to ≤ 0.05° steps (the scan's curvature over a long straight coast)
    auto seg = [&](int kind, double la0, double lo0, double la1, double lo1, bool first) {
        if (first) { add(kind, la0, lo0); }
        const int k = std::max(1, int(std::ceil(std::max(std::fabs(la1 - la0), std::fabs(lo1 - lo0)) / 0.05)));
        for (int j = 1; j <= k; j++) add(kind, la0 + (la1 - la0) * j / k, lo0 + (lo1 - lo0) * j / k);
    };
    const double inv = 1.0 / coast::kScale;
    const int laLo = int(std::floor(laMin * coast::kScale)), laHi = int(std::ceil(laMax * coast::kScale));
    for (unsigned r = 0; r < coast::kRecCount && !full; r++) {
        const coast::Rec& rec = coast::kRecs[r];
        if (rec.latMax < laLo || rec.latMin > laHi) continue;
        if (!lonIn(rec.lonMin * inv, rec.lonMax * inv)) continue;
        const int kind = rec.kind;
        int la = rec.lat0, lo = rec.lon0;
        const int8_t* d = coast::kDelta + rec.first;
        for (unsigned i = 1; i < rec.n; i++) {
            int dy, dx;
            if (*d == -128) {
                dy = int16_t(uint16_t(uint8_t(d[1])) | (uint16_t(uint8_t(d[2])) << 8));
                dx = int16_t(uint16_t(uint8_t(d[3])) | (uint16_t(uint8_t(d[4])) << 8)); d += 5;
            } else { dy = d[0]; dx = d[1]; d += 2; }
            seg(kind, la * inv, lo * inv, (la + dy) * inv, (lo + dx) * inv, i == 1);
            la += dy; lo += dx;
        }
        flush(kind);
    }
    // ── the grid: every kGridDeg of latitude and longitude over the footprint ──
    auto label = [&](double x, double y, const std::string& text) {
        if (!labels.empty()) labels += ',';
        char b[48]; std::snprintf(b, sizeof b, "[%.0f,%.0f,\"", x * scale, y * scale);
        labels += b; labels += text; labels += "\"]";
    };
    for (double la = std::ceil(laMin / kGridDeg) * kGridDeg; la <= laMax && !full; la += kGridDeg) {
        if (std::fabs(la) > 89) continue;
        const size_t before = js[3].size();
        bool first = true;
        double lo0 = allLon ? -180 : cLon + dlMin, lo1 = allLon ? 180 : cLon + dlMax;
        for (double lo = lo0; lo + 0.25 <= lo1; lo += 0.25) { seg(3, la, lo, la, lo + 0.25, first); first = false; }
        flush(3);
        if (js[3].size() != before) {   // label where the parallel is nearest the picture's left edge
            double bx = 1e9, by = 0;
            for (double lo = lo0; lo <= lo1; lo += 0.25) { double x, y; if (g.toImage(la, lo, x, y) && x >= 0 && x < W && y >= 0 && y < rows && x < bx) { bx = x; by = y; } }
            if (bx < 1e9) label(bx, by, detail::gridLabel(la, true));
        }
    }
    {
        const double lo0 = allLon ? -180 : cLon + dlMin, lo1 = allLon ? 180 : cLon + dlMax;
        for (double lo = std::ceil(lo0 / kGridDeg) * kGridDeg; lo <= lo1 && !full; lo += kGridDeg) {
            double lon = lo; while (lon > 180) lon -= 360; while (lon <= -180) lon += 360;
            const size_t before = js[3].size();
            bool first = true;
            for (double la = std::max(-89.0, laMin); la + 0.25 <= std::min(89.0, laMax); la += 0.25) { seg(3, la, lon, la + 0.25, lon, first); first = false; }
            flush(3);
            if (js[3].size() != before) {   // label where the meridian is nearest the picture's top
                double bx = 0, by = 1e9;
                for (double la = std::max(-89.0, laMin); la <= std::min(89.0, laMax); la += 0.25) { double x, y; if (g.toImage(la, lon, x, y) && x >= 0 && x < W && y >= 0 && y < rows && y < by) { bx = x; by = y; } }
                if (by < 1e9) label(bx, by, detail::gridLabel(lon, false));
            }
        }
    }
    if (full) { ov.why = "the outline is too large to draw"; return ov; }

    // ── the JSON ──
    const Candidate& c = cands[size_t(si)];
    std::string o = "{\"v\":1,\"w\":";
    o += std::to_string(int(std::lround(W * scale))) + ",\"h\":" + std::to_string(int(std::lround(rows * scale)));
    o += ",\"scale\":"; detail::num(o, scale);
    std::string nm; for (char ch : c.name) if (ch != '"' && ch != '\\' && (unsigned char)ch >= 0x20) nm += ch;
    o += ",\"sat\":\"" + nm + "\",\"norad\":" + std::to_string(c.norad);
    char b[200];
    std::snprintf(b, sizeof b, ",\"start\":%.3f,\"end\":%.3f,\"periodMs\":%.2f,\"clockSkewSec\":%.2f,\"stampResidualMs\":%.1f,\"elementsAgeDays\":%.2f,\"halfScanDeg\":%.2f",
                  clk.unixAt(0), clk.unixAt(rows / 8.0), clk.bMs, clk.skewSec, clk.maxResidualMs, (tMid - c.el.epochUnix()) / 86400.0, ins.halfScanDeg);
    o += b;
    // corners (top-left, top-right, bottom-left, bottom-right) and the centre
    o += ",\"corners\":[";
    const double cx[4] = { 0, double(W - 1), 0, double(W - 1) }, cy[4] = { 0, 0, double(rows - 1), double(rows - 1) };
    for (int k = 0; k < 4; k++) {
        double la = 0, lo = 0; g.toGround(cx[k], cy[k], la, lo);
        if (k) o += ',';
        o += '['; detail::num(o, la); o += ','; detail::num(o, lo); o += ']';
    }
    o += "],\"centre\":["; detail::num(o, cLat); o += ','; detail::num(o, cLon); o += ']';
    const char* names[4] = { "coast", "lake", "border", "grid" };
    for (int k = 0; k < 4; k++) { o += ",\""; o += names[k]; o += "\":["; o += js[k]; o += ']'; }
    o += ",\"labels\":[" + labels + "]}";
    ov.ok = true; ov.json = std::move(o); ov.latC = cLat; ov.lonC = cLon;
    return ov;
}

} // namespace geo
} // namespace vibelrpt
