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
//    60 m is left out (national channels, not one segment); 6 m has CW only (beacons and CW, 50.000–50.100 MHz in all
//    three plans; its digimodes are all-mode spectrum shared with SSB).
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
        {"160m", "160 m", k(1810), k(1838)}, {"80m", "80 m", k(3500), k(3570)}, {"40m", "40 m", k(7000), k(7040)},
        {"30m", "30 m", k(10100), k(10130)}, {"20m", "20 m", k(14000), k(14070)}, {"17m", "17 m", k(18068), k(18095)},
        {"15m", "15 m", k(21000), k(21070)}, {"12m", "12 m", k(24890), k(24915)}, {"10m", "10 m", k(28000), k(28070)},
        {"6m", "6 m", k(50000), k(50100)} };
    static const std::vector<Seg> rtty = {
        {"160m", "160 m", k(1838), k(1840)}, {"80m", "80 m", k(3570), k(3600)}, {"40m", "40 m", k(7040), k(7050)},
        {"30m", "30 m", k(10130), k(10150)}, {"20m", "20 m", k(14070), k(14099)}, {"17m", "17 m", k(18095), k(18109)},
        {"15m", "15 m", k(21070), k(21110)}, {"12m", "12 m", k(24915), k(24929)}, {"10m", "10 m", k(28070), k(28190)} };
    static const std::vector<Seg> psk = {
        {"160m", "160 m", k(1838), k(1840)}, {"80m", "80 m", k(3580), k(3600)}, {"40m", "40 m", k(7040), k(7050)},
        {"30m", "30 m", k(10140), k(10150)}, {"20m", "20 m", k(14070), k(14099)}, {"17m", "17 m", k(18100), k(18109)},
        {"15m", "15 m", k(21070), k(21110)}, {"12m", "12 m", k(24920), k(24929)}, {"10m", "10 m", k(28120), k(28190)} };
    return kd == Kind::Cw ? cw : kd == Kind::Rtty ? rtty : psk;
}
// ── Region 2 — the same as Region 1 but on 160 m (CW 1810–1840, digimodes 1800–1810) and 15 m (digimodes to 21120) ──
inline const std::vector<Seg>& r2(Kind kd) {
    static const std::vector<Seg> cw = {
        {"160m", "160 m", k(1810), k(1840)}, {"80m", "80 m", k(3500), k(3570)}, {"40m", "40 m", k(7000), k(7040)},
        {"30m", "30 m", k(10100), k(10130)}, {"20m", "20 m", k(14000), k(14070)}, {"17m", "17 m", k(18068), k(18095)},
        {"15m", "15 m", k(21000), k(21070)}, {"12m", "12 m", k(24890), k(24915)}, {"10m", "10 m", k(28000), k(28070)},
        {"6m", "6 m", k(50000), k(50100)} };
    static const std::vector<Seg> rtty = {
        {"160m", "160 m", k(1800), k(1810)}, {"80m", "80 m", k(3570), k(3600)}, {"40m", "40 m", k(7040), k(7050)},
        {"30m", "30 m", k(10130), k(10150)}, {"20m", "20 m", k(14070), k(14099)}, {"17m", "17 m", k(18095), k(18109)},
        {"15m", "15 m", k(21070), k(21120)}, {"12m", "12 m", k(24915), k(24929)}, {"10m", "10 m", k(28070), k(28190)} };
    static const std::vector<Seg> psk = {
        {"160m", "160 m", k(1838), k(1843)}, {"80m", "80 m", k(3580), k(3600)}, {"40m", "40 m", k(7040), k(7050)},
        {"30m", "30 m", k(10140), k(10150)}, {"20m", "20 m", k(14070), k(14099)}, {"17m", "17 m", k(18100), k(18109)},
        {"15m", "15 m", k(21070), k(21120)}, {"12m", "12 m", k(24920), k(24929)}, {"10m", "10 m", k(28120), k(28190)} };
    return kd == Kind::Cw ? cw : kd == Kind::Rtty ? rtty : psk;
}
// ── Region 3 — CW 1800–1830, 3500–3535, 7000–7030, 10110–10130; narrowband from 1830, 3535, 7030 (RTTY DX 7040) ──
inline const std::vector<Seg>& r3(Kind kd) {
    static const std::vector<Seg> cw = {
        {"160m", "160 m", k(1800), k(1830)}, {"80m", "80 m", k(3500), k(3535)}, {"40m", "40 m", k(7000), k(7030)},
        {"30m", "30 m", k(10110), k(10130)}, {"20m", "20 m", k(14000), k(14070)}, {"17m", "17 m", k(18068), k(18095)},
        {"15m", "15 m", k(21000), k(21070)}, {"12m", "12 m", k(24890), k(24915)}, {"10m", "10 m", k(28000), k(28070)},
        {"6m", "6 m", k(50000), k(50100)} };
    static const std::vector<Seg> rtty = {
        {"160m", "160 m", k(1830), k(1840)}, {"80m", "80 m", k(3535), k(3600)}, {"40m", "40 m", k(7030), k(7060)},
        {"30m", "30 m", k(10130), k(10150)}, {"20m", "20 m", k(14070), k(14112)}, {"17m", "17 m", k(18095), k(18110)},
        {"15m", "15 m", k(21070), k(21125)}, {"12m", "12 m", k(24915), k(24930)}, {"10m", "10 m", k(28070), k(28190)} };
    static const std::vector<Seg> psk = {
        {"160m", "160 m", k(1838), k(1840)}, {"80m", "80 m", k(3580), k(3600)}, {"40m", "40 m", k(7040), k(7060)},
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

/** ★★ THE RULE: the window starts at the segment's bottom and is `widthHz` wide. */
inline double windowCentre(const Seg& s, double widthHz) { return s.loHz + widthHz / 2.0; }

/** The band whose computed centre `centreHz` is (to the hertz), or null — an owner's own centre names no band. */
inline const Seg* bandAtCentre(Kind kind, int region, double widthHz, double centreHz) {
    for (const auto& s : segments(kind, region))
        if (centreHz > 0 && std::abs(windowCentre(s, widthHz) - centreHz) < 1.0) return &s;
    return nullptr;
}

/** ★ Does a window of `widthHz` at `centreHz` fit inside a capture? `halfHz` is how far either side of the capture's
 *  centre a whole signal may sit (vibe::bg::halfWindowHz + 3 kHz for background decoding — what its save checks). */
inline bool windowFits(double centreHz, double widthHz, double captureCentreHz, double halfHz) {
    if (!(captureCentreHz > 0) || !(halfHz > 0)) return true;   // ★ unknown capture: the radio's own save decides
    return std::abs(centreHz - captureCentreHz) <= halfHz - widthHz / 2.0;
}

/** ★★ ONE WRITER: `,"skimBands":{"region":R,"cw":[{id,name,lo,hi}…],"rtty":[…],"psk":[…]}` — every page applies the
 *  rule (centre = lo + width / 2) to these. `tunable(lo, hi)` = whether this radio can tune the segment at all (the
 *  owner's allow/block ranges, its hardware); empty = every band. */
template <class Tunable>
inline std::string skimBandsJson(int region, Tunable tunable) {
    std::string j = ",\"skimBands\":{\"region\":" + std::to_string(region == 2 || region == 3 ? region : 1);
    for (Kind kd : { Kind::Cw, Kind::Rtty, Kind::Psk }) {
        j += std::string(",\"") + kindName(kd) + "\":[";
        bool first = true;
        for (const auto& s : segments(kd, region)) {
            if (!tunable(s.loHz, s.hiHz)) continue;
            char e[160];
            std::snprintf(e, sizeof e, "%s{\"id\":\"%s\",\"name\":\"%s\",\"lo\":%.0f,\"hi\":%.0f}", first ? "" : ",", s.id, s.name, s.loHz, s.hiHz);
            j += e; first = false;
        }
        j += "]";
    }
    return j + "}";
}
inline std::string skimBandsJson(int region) { return skimBandsJson(region, [](double, double) { return true; }); }

} // namespace skimbands
} // namespace vibe
