// vibe_skim_bands.h — WHERE A SKIMMER SITS ON EACH AMATEUR BAND (V12 beta90, Stuart 2026-11-21).
//
// Stuart: "cw skimmer and rtty skimmer should be per band rather than entering a centre frequency" and "48k should default
// to 7024 for 40m 96k 7048 etc so the bands cw allocation is fully covered".
//
// ★★ THE PLACEMENT RULE — ONE PLACE (windowCentre): a skimmer's window starts at the BOTTOM of the band's segment for its
//    mode and extends by the skimmer's width, so centre = segment bottom + width / 2. 40 m CW in Region 1 starts at 7000:
//    24 kHz → 7012, 48 kHz → 7024, 96 kHz → 7048. The same for RTTY and PSK on their own segments. Every client
//    (the Setup page, the Android server screen, the web and app decoder boxes) gets the segments from the server's JSON
//    (skimBandsJson) and applies this one rule; the server applies it in skimWindow().
//
// ★★ THE SEGMENTS, PER IARU REGION, from the published band plans:
//    • Region 1 — "IARU Region 1 HF Band Plan, effective from 16 Oct 2020" (Novi Sad 2020 virtual General Conference);
//      6 m CW from the Region 1 VHF plan (50.000–50.100 MHz telegraphy).
//    • Region 2 — "IARU Region 2 Band Plan, September 2020".
//    • Region 3 — "IARU Region 3 Band Plan, Document R3-004, revised 3 September 2019".
//    CW = the plan's CW-only segment. RTTY = the plan's narrow-band digimode segment ("Narrow band modes / Digimodes",
//    "CW, DM", "CW, Narrowband modes" — the plans name RTTY as one of these and give it no sub-segment of its own).
//    PSK = the same digimode segment, but from where PSK31 is conventionally found (the plans do not name a PSK
//    frequency): 1838, 3580, 7040, 10140, 14070, 18100, 21070, 24920, 28120 kHz — a PSK skimmer is 6 or 12 kHz wide, and
//    from the bottom of 80 m's digimodes (3570) it would miss 3580 altogether.
//    6 m has CW only (beacons and CW, 50.000–50.100 MHz in all three plans; its digimodes are all-mode spectrum shared with
//    SSB).
//    ★ 60 m (V12 beta90 follow-up, Stuart: "maybe list multiple centre points per band"): the WRC-15 secondary allocation
//    5351.5–5366.5 kHz, which all three plans divide the same way — 5351.5–5354 CW / narrow-band, 5354–5366 all modes
//    (FT8 5357), 5366–5366.5 weak signal. CW and RTTY take the whole allocation (one 24 kHz window holds it), PSK the
//    narrow-band part. NATIONAL rules differ (some licences give channels only, some nothing): every 60 m point carries
//    the note "check your licence", and a receiver hearing it costs nobody anything.
//
// ★★ SEVERAL POINTS PER BAND (V12 beta90 follow-up) — points(): a band offers
//    • TILES of its segment at the skimmer's width, from the bottom (the rule above — the first tile is the preset):
//      centre_k = lo + width/2 + k·width while the tile starts below the segment's top. 40 m CW R1 at 24 kHz: 7012
//      (7000–7024), 7036 (7024–7048); at 48 kHz 7024 alone; at 96 kHz 7048 alone;
//    • PRACTICE segments outside the plan where activity really sits, for the region where it matters, tiled by the SAME
//      rule and labelled (kPractice — each with its source).
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vibe {
namespace skimbands {

enum class Kind { Cw = 0, Rtty = 1, Psk = 2 };
inline const char* kindName(Kind k) { return k == Kind::Cw ? "cw" : k == Kind::Rtty ? "rtty" : "psk"; }
inline const char* kindLabel(Kind k) { return k == Kind::Cw ? "CW" : k == Kind::Rtty ? "RTTY" : "PSK"; }

/** One band's segment for one mode, Hz. */
struct Seg { const char* id; const char* name; double loHz, hiHz; };

namespace detail {
// kHz → Hz, so the tables read as the plans print them
constexpr double k(double khz) { return khz * 1000.0; }
// ── Region 1 ──
inline const std::vector<Seg>& r1(Kind kd) {
    static const std::vector<Seg> cw = {
        {"160m", "160 m", k(1810), k(1838)}, {"80m", "80 m", k(3500), k(3570)}, {"60m", "60 m", k(5351.5), k(5366.5)}, {"40m", "40 m", k(7000), k(7040)},
        {"30m", "30 m", k(10100), k(10130)}, {"20m", "20 m", k(14000), k(14070)}, {"17m", "17 m", k(18068), k(18095)},
        {"15m", "15 m", k(21000), k(21070)}, {"12m", "12 m", k(24890), k(24915)}, {"10m", "10 m", k(28000), k(28070)},
        {"6m", "6 m", k(50000), k(50100)} };
    static const std::vector<Seg> rtty = {
        {"160m", "160 m", k(1838), k(1840)}, {"80m", "80 m", k(3570), k(3600)}, {"60m", "60 m", k(5351.5), k(5366.5)}, {"40m", "40 m", k(7040), k(7050)},
        {"30m", "30 m", k(10130), k(10150)}, {"20m", "20 m", k(14070), k(14099)}, {"17m", "17 m", k(18095), k(18109)},
        {"15m", "15 m", k(21070), k(21110)}, {"12m", "12 m", k(24915), k(24929)}, {"10m", "10 m", k(28070), k(28190)} };
    static const std::vector<Seg> psk = {
        {"160m", "160 m", k(1838), k(1840)}, {"80m", "80 m", k(3580), k(3600)}, {"60m", "60 m", k(5351.5), k(5354)}, {"40m", "40 m", k(7040), k(7050)},
        {"30m", "30 m", k(10140), k(10150)}, {"20m", "20 m", k(14070), k(14099)}, {"17m", "17 m", k(18100), k(18109)},
        {"15m", "15 m", k(21070), k(21110)}, {"12m", "12 m", k(24920), k(24929)}, {"10m", "10 m", k(28120), k(28190)} };
    return kd == Kind::Cw ? cw : kd == Kind::Rtty ? rtty : psk;
}
// ── Region 2 — the same as Region 1 but on 160 m (CW 1810–1840, digimodes 1800–1810) and 15 m (digimodes to 21120) ──
inline const std::vector<Seg>& r2(Kind kd) {
    static const std::vector<Seg> cw = {
        {"160m", "160 m", k(1810), k(1840)}, {"80m", "80 m", k(3500), k(3570)}, {"60m", "60 m", k(5351.5), k(5366.5)}, {"40m", "40 m", k(7000), k(7040)},
        {"30m", "30 m", k(10100), k(10130)}, {"20m", "20 m", k(14000), k(14070)}, {"17m", "17 m", k(18068), k(18095)},
        {"15m", "15 m", k(21000), k(21070)}, {"12m", "12 m", k(24890), k(24915)}, {"10m", "10 m", k(28000), k(28070)},
        {"6m", "6 m", k(50000), k(50100)} };
    static const std::vector<Seg> rtty = {
        {"160m", "160 m", k(1800), k(1810)}, {"80m", "80 m", k(3570), k(3600)}, {"60m", "60 m", k(5351.5), k(5366.5)}, {"40m", "40 m", k(7040), k(7050)},
        {"30m", "30 m", k(10130), k(10150)}, {"20m", "20 m", k(14070), k(14099)}, {"17m", "17 m", k(18095), k(18109)},
        {"15m", "15 m", k(21070), k(21120)}, {"12m", "12 m", k(24915), k(24929)}, {"10m", "10 m", k(28070), k(28190)} };
    static const std::vector<Seg> psk = {
        {"160m", "160 m", k(1838), k(1843)}, {"80m", "80 m", k(3580), k(3600)}, {"60m", "60 m", k(5351.5), k(5354)}, {"40m", "40 m", k(7040), k(7050)},
        {"30m", "30 m", k(10140), k(10150)}, {"20m", "20 m", k(14070), k(14099)}, {"17m", "17 m", k(18100), k(18109)},
        {"15m", "15 m", k(21070), k(21120)}, {"12m", "12 m", k(24920), k(24929)}, {"10m", "10 m", k(28120), k(28190)} };
    return kd == Kind::Cw ? cw : kd == Kind::Rtty ? rtty : psk;
}
// ── Region 3 — CW 1800–1830, 3500–3535, 7000–7030, 10110–10130; narrowband from 1830, 3535, 7030 (RTTY DX 7040) ──
inline const std::vector<Seg>& r3(Kind kd) {
    static const std::vector<Seg> cw = {
        {"160m", "160 m", k(1800), k(1830)}, {"80m", "80 m", k(3500), k(3535)}, {"60m", "60 m", k(5351.5), k(5366.5)}, {"40m", "40 m", k(7000), k(7030)},
        {"30m", "30 m", k(10110), k(10130)}, {"20m", "20 m", k(14000), k(14070)}, {"17m", "17 m", k(18068), k(18095)},
        {"15m", "15 m", k(21000), k(21070)}, {"12m", "12 m", k(24890), k(24915)}, {"10m", "10 m", k(28000), k(28070)},
        {"6m", "6 m", k(50000), k(50100)} };
    static const std::vector<Seg> rtty = {
        {"160m", "160 m", k(1830), k(1840)}, {"80m", "80 m", k(3535), k(3600)}, {"60m", "60 m", k(5351.5), k(5366.5)}, {"40m", "40 m", k(7030), k(7060)},
        {"30m", "30 m", k(10130), k(10150)}, {"20m", "20 m", k(14070), k(14112)}, {"17m", "17 m", k(18095), k(18110)},
        {"15m", "15 m", k(21070), k(21125)}, {"12m", "12 m", k(24915), k(24930)}, {"10m", "10 m", k(28070), k(28190)} };
    static const std::vector<Seg> psk = {
        {"160m", "160 m", k(1838), k(1840)}, {"80m", "80 m", k(3580), k(3600)}, {"60m", "60 m", k(5351.5), k(5354)}, {"40m", "40 m", k(7040), k(7060)},
        {"30m", "30 m", k(10140), k(10150)}, {"20m", "20 m", k(14070), k(14112)}, {"17m", "17 m", k(18100), k(18110)},
        {"15m", "15 m", k(21070), k(21125)}, {"12m", "12 m", k(24920), k(24930)}, {"10m", "10 m", k(28120), k(28190)} };
    return kd == Kind::Cw ? cw : kd == Kind::Rtty ? rtty : psk;
}
} // namespace detail

/** The segments for `kind` in IARU `region` (1–3; anything else = Region 1, as vibebands::ituRegion's default). */
inline const std::vector<Seg>& segments(Kind kind, int region) {
    return region == 2 ? detail::r2(kind) : region == 3 ? detail::r3(kind) : detail::r1(kind);
}
inline const Seg* segment(Kind kind, int region, const std::string& id) {
    for (const auto& s : segments(kind, region)) if (id == s.id) return &s;
    return nullptr;
}

/** ★★ A PRACTICE segment (V12 beta90 follow-up): where activity sits OUTSIDE the plan's segment, for one region. */
struct Practice { Kind kind; int region; const char* band; double loHz, hiHz; const char* note; };
inline const std::vector<Practice>& practice() {
    static const std::vector<Practice> p = {
        // ★ Region 2, 40 m RTTY: the ARRL's "Considerate Operator's Frequency Guide" lists 7.080–7.125 MHz as RTTY/data
        //   (the R2 plan's own digimode segment, 7040–7050, is kept as the first entries).
        { Kind::Rtty, 2, "40m", detail::k(7080), detail::k(7125), "US RTTY" },
        // ★ Region 2, 40 m PSK31 round 7.070 MHz: common practice in North America (PSK31 software's default list) — no plan
        //   names it, so it is kept to the one 5 kHz stretch.
        { Kind::Psk, 2, "40m", detail::k(7070), detail::k(7075), "US PSK31" },
    };
    return p;
}
/** One window a skimmer may sit at: its centre and edges (Hz), and a note ("US RTTY", "check your licence") or "". */
struct Point { double hz, loHz, hiHz; std::string note; };
inline void tile(std::vector<Point>& out, double lo, double hi, double widthHz, const std::string& note) {
    if (!(widthHz > 0) || !(hi > lo)) return;
    for (int k = 0; k < 64; ++k) {
        const double a = lo + k * widthHz;
        if (a >= hi) break;
        out.push_back(Point{ a + widthHz / 2.0, a, a + widthHz, note });
    }
}
/** ★★ The windows a band offers at `widthHz`: its segment's tiles from the bottom, then the region's practice segments'. */
inline std::vector<Point> points(Kind kind, int region, const Seg& s, double widthHz) {
    std::vector<Point> v;
    const std::string sixty = std::string(s.id) == "60m" ? "check your licence" : "";
    tile(v, s.loHz, s.hiHz, widthHz, sixty);
    for (const auto& p : practice())
        if (p.kind == kind && p.region == region && std::string(p.band) == s.id) tile(v, p.loHz, p.hiHz, widthHz, p.note);
    return v;
}
/** The widths each skimmer offers (the decoder host's and the clients' options) — the JSON carries points for these. */
inline std::vector<double> widths(Kind k) {
    return k == Kind::Psk ? std::vector<double>{ 6000, 12000, 24000 } : std::vector<double>{ 24000, 48000, 96000 };
}

/** ★★ THE RULE: the window starts at the segment's bottom and is `widthHz` wide. */
inline double windowCentre(const Seg& s, double widthHz) { return s.loHz + widthHz / 2.0; }

/** The band one of whose points `centreHz` is (to the hertz), or null — an owner's own centre names no band. */
inline const Seg* bandAtCentre(Kind kind, int region, double widthHz, double centreHz) {
    for (const auto& s : segments(kind, region))
        for (const auto& p : points(kind, region, s, widthHz))
            if (centreHz > 0 && std::abs(p.hz - centreHz) < 1.0) return &s;
    return nullptr;
}

/** ★ Does a window of `widthHz` at `centreHz` fit inside a capture? `halfHz` is how far either side of the capture's
 *  centre a whole signal may sit (vibe::bg::halfWindowHz + 3 kHz for background decoding — what its save checks). */
inline bool windowFits(double centreHz, double widthHz, double captureCentreHz, double halfHz) {
    if (!(captureCentreHz > 0) || !(halfHz > 0)) return true;   // ★ unknown capture: the radio's own save decides
    return std::abs(centreHz - captureCentreHz) <= halfHz - widthHz / 2.0;
}

/** ★★ ONE WRITER: `,"skimBands":{"region":R,"cw":[{id,name,lo,hi,"points":{"24000":[{"hz":…,"note":…}…],…}}…],
 *  "rtty":[…],"psk":[…]}` — each band's windows at every width its skimmer offers (points(): the tiles and the practice
 *  segments), so the Setup page, the Android server screen and the listener's BAND row list exactly the same windows
 *  and compute nothing. `tunable(lo, hi)` = whether this radio can tune a window end to end (its range, the owner's
 *  allow/block ranges, a lock); a window it cannot is left out, a band with none at any width is left out. */
template <class Tunable>
inline std::string skimBandsJson(int region, Tunable tunable) {
    std::string j = ",\"skimBands\":{\"region\":" + std::to_string(region == 2 || region == 3 ? region : 1);
    for (Kind kd : { Kind::Cw, Kind::Rtty, Kind::Psk }) {
        j += std::string(",\"") + kindName(kd) + "\":[";
        bool first = true;
        for (const auto& s : segments(kd, region)) {
            std::string pts; bool any = false;
            for (double w : widths(kd)) {
                std::string one; bool f1 = true;
                for (const auto& p : points(kd, region, s, w)) {
                    if (!tunable(p.loHz, p.hiHz)) continue;
                    char e[96];
                    std::snprintf(e, sizeof e, "%s{\"hz\":%.0f", f1 ? "" : ",", p.hz);
                    one += e;
                    if (!p.note.empty()) one += ",\"note\":\"" + p.note + "\"";
                    one += "}"; f1 = false;
                }
                if (f1) continue;
                char k[32]; std::snprintf(k, sizeof k, "%s\"%.0f\":[", any ? "," : "", w);
                pts += k + one + "]"; any = true;
            }
            if (!any) continue;
            char e[160];
            std::snprintf(e, sizeof e, "%s{\"id\":\"%s\",\"name\":\"%s\",\"lo\":%.0f,\"hi\":%.0f,\"points\":{", first ? "" : ",", s.id, s.name, s.loHz, s.hiHz);
            j += e + pts + "}}"; first = false;
        }
        j += "]";
    }
    return j + "}";
}
inline std::string skimBandsJson(int region) { return skimBandsJson(region, [](double, double) { return true; }); }

} // namespace skimbands
} // namespace vibe
