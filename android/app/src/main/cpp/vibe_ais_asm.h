// vibe_ais_asm.h — V12 2026-11-16 AIS BINARY MESSAGES (types 6 and 8): the application-specific messages a listener
// can read, written from the published tables. docs/v12/AIS-APRS.md "Binary messages".
//
// ★★ WHAT IS READ (international DAC 001 — IMO SN.1/Circ.289, and the SN/Circ.236 one it replaced):
//    · FI 31 meteorological and hydrographic data (Circ.289) — wind, gusts, air/dew/water temperature, humidity,
//      pressure and its trend, visibility, water level (tide) and its trend, surface current, waves, swell, sea state,
//      precipitation, salinity, ice. Sent by AtoN and port stations round the UK and Europe.
//    · FI 11 the same in the older Circ.236 layout (withdrawn, still on air from older stations).
//    · FI 22 (broadcast, type 8) / FI 23 (addressed, type 6) AREA NOTICE — what the area is (the Circ.289 notice table),
//      when it starts and how long it lasts, and its first shape (circle/point, rectangle, sector, line, polygon) as a
//      place and a size; any text sub-area is joined into the notice's words.
//    · FI 29 (broadcast) / FI 30 (addressed) TEXT DESCRIPTION — plain words.
//    Everything else (other DACs, other FIs) is counted, not shown: a regional DAC's layout is that region's own, and a
//    guess at it would put wrong numbers on a station's card.
// ★★ TYPE 21 EXTRAS: the AtoN status byte (IALA A-126 page 7: light on/off/fault, RACON, health alarm) — atonStatus()
//    below; vibe_ais.h decode() carries the raw byte.
// ★ "Not available" values (each field's own code in the tables) become kNone, never a number on the card.
// ★ PARSER ONLY: no allocation beyond a notice's text; a message is at most 1008 bits.
#pragma once
#include "vibe_ais.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vibeais {
namespace asmsg {

static constexpr int16_t kNone = INT16_MIN;
static constexpr int32_t kNoPos32 = INT32_MIN;

/** ★ ONE MET/HYDRO REPORT, 64 bytes, plain old data — the same bytes in RAM and in the met store on disk
 *  (vibe_ais_store.h). Values are scaled integers; kNone = not available. */
struct Met {
    int32_t lat = kNoPos32, lon = kNoPos32;   ///< 1e-7 degree: where the readings were taken (it may not be the sender)
    uint8_t day = 0, hour = 24, minute = 60;  ///< UTC of the observation (0 / 24 / 60 = not given)
    uint8_t fi = 0;                           ///< 31 (Circ.289) or 11 (Circ.236)
    int16_t windKn = kNone, gustKn = kNone, windDir = kNone, gustDir = kNone;   ///< knots; degrees true (from)
    int16_t air10 = kNone, hum = kNone, dew10 = kNone;                         ///< 0.1 °C; %; 0.1 °C
    int16_t hPa = kNone, pTrend = kNone;                                        ///< hPa; 0 steady · 1 falling · 2 rising
    int16_t vis10 = kNone; uint8_t visMore = 0, spare0 = 0;                     ///< 0.1 NM; "more than" (Circ.289's top bit)
    int16_t level100 = kNone, levelTrend = kNone;                               ///< 0.01 m above chart datum; trend as pTrend
    int16_t cur10 = kNone, curDir = kNone;                                      ///< surface current 0.1 kn; degrees
    int16_t wave10 = kNone, waveS = kNone, waveDir = kNone;                     ///< 0.1 m; s; degrees
    int16_t swell10 = kNone, swellS = kNone, swellDir = kNone;
    int16_t sea = kNone, water10 = kNone, precip = kNone, sal10 = kNone, ice = kNone;   ///< Beaufort; 0.1 °C; code; 0.1 ‰; 0/1
};
static_assert(sizeof(Met) == 64, "a met report is 64 bytes");

inline bool hasAny(const Met& m) {
    const int16_t v[] = { m.windKn, m.gustKn, m.windDir, m.gustDir, m.air10, m.hum, m.dew10, m.hPa, m.pTrend, m.vis10,
                          m.level100, m.levelTrend, m.cur10, m.curDir, m.wave10, m.waveS, m.waveDir, m.swell10, m.swellS,
                          m.swellDir, m.sea, m.water10, m.precip, m.sal10, m.ice };
    for (int16_t x : v) if (x != kNone) return true;
    return false;
}

/** Area notice / text description, for the notices strip and the gallery. */
struct Notice {
    int fi = 0;                 ///< 22/23 area notice · 29/30 text
    int what = -1;              ///< the notice description code (0–127), −1 for text
    int month = 0, day = 0, hour = 24, minute = 60;
    int durationMin = -1;       ///< minutes; −1 / 262143 not given; 0 = this notice is cancelled
    int shape = -1;             ///< 0 circle/point · 1 rectangle · 2 sector · 3 line · 4 polygon
    double lat = NAN, lon = NAN, sizeM = NAN;
    uint32_t to = 0;            ///< addressed: the destination MMSI
    std::string text;
};

/** What a binary message was, for the counters. */
struct Kind { int dac = -1, fi = -1; bool addressed = false; size_t dataAt = 0; };

/** The application identifier and where its data starts (M.1371-5 Annex 8, messages 6 and 8). False = too short. */
inline bool header(const Bits& B, int type, Kind& k, uint32_t* dest = nullptr) {
    if (type == 8) { if (B.n() < 56) return false; k.dac = int(B.u(40, 10)); k.fi = int(B.u(50, 6)); k.dataAt = 56; return true; }
    if (type == 6) {
        if (B.n() < 88) return false;
        if (dest) *dest = B.u(40, 30);
        k.addressed = true; k.dac = int(B.u(72, 10)); k.fi = int(B.u(82, 6)); k.dataAt = 88; return true;
    }
    return false;
}

// ── met/hydro ──────────────────────────────────────────────────────────────────────────────────────────────────────
/** Lon/lat in 1/1000 minute (Circ.289 / Circ.236): 181° / 91° = not available. */
inline void pos1000(int32_t rawLon, int32_t rawLat, Met& m) {
    const double lon = rawLon / 60000.0, lat = rawLat / 60000.0;
    if (std::fabs(lon) <= 180 && std::fabs(lat) <= 90 && !(rawLon == 0 && rawLat == 0)) {
        m.lon = int32_t(std::lround(lon * 1e7)); m.lat = int32_t(std::lround(lat * 1e7));
    }
}
inline void when(const Bits& B, size_t at, Met& m) {
    const uint32_t d = B.u(at, 5), h = B.u(at + 5, 5), mi = B.u(at + 10, 6);
    m.day = uint8_t(d); m.hour = uint8_t(h <= 23 ? h : 24); m.minute = uint8_t(mi <= 59 ? mi : 60);
}

/** ★ DAC 001 FI 31 — IMO SN.1/Circ.289 meteorological and hydrographic data (360 bits on a type 8; offsets below are
 *  from the start of the application data). */
inline bool metFi31(const Bits& B, size_t a, Met& m) {
    if (B.n() < a + 294) return false;   // ★ the table to salinity/ice; only the trailing spare may be missing
    m = Met(); m.fi = 31;
    pos1000(B.s(a + 0, 25), B.s(a + 25, 24), m);
    // a + 49: position accuracy (1 bit) — not shown
    when(B, a + 50, m);
    { const uint32_t v = B.u(a + 66, 7); if (v <= 126) m.windKn = int16_t(v); }       // 126 = 126 kn or more, 127 n/a
    { const uint32_t v = B.u(a + 73, 7); if (v <= 126) m.gustKn = int16_t(v); }
    { const uint32_t v = B.u(a + 80, 9); if (v <= 359) m.windDir = int16_t(v); }      // 360 n/a
    { const uint32_t v = B.u(a + 89, 9); if (v <= 359) m.gustDir = int16_t(v); }
    { const int32_t v = B.s(a + 98, 11); if (v >= -600 && v <= 600) m.air10 = int16_t(v); }   // −1024 n/a
    { const uint32_t v = B.u(a + 109, 7); if (v <= 100) m.hum = int16_t(v); }          // 101 n/a
    { const int32_t v = B.s(a + 116, 10); if (v >= -200 && v <= 500) m.dew10 = int16_t(v); }  // 501 n/a
    { const uint32_t v = B.u(a + 126, 9); if (v <= 402) m.hPa = int16_t(v + 799); }   // 0 ≤799 · 1–401 800–1200 · 402 ≥1201 · 511 n/a
    { const uint32_t v = B.u(a + 135, 2); if (v <= 2) m.pTrend = int16_t(v); }
    { const uint32_t more = B.u(a + 137, 1), v = B.u(a + 138, 7); if (v <= 126) { m.vis10 = int16_t(v); m.visMore = uint8_t(more); } }
    { const uint32_t v = B.u(a + 145, 12); if (v <= 4000) m.level100 = int16_t(long(v) - 1000); }  // −10.00…+30.00 m, 4001 n/a
    { const uint32_t v = B.u(a + 157, 2); if (v <= 2) m.levelTrend = int16_t(v); }
    { const uint32_t v = B.u(a + 159, 8); if (v <= 251) m.cur10 = int16_t(v); }        // 251 = more than 25 kn, 255 n/a
    { const uint32_t v = B.u(a + 167, 9); if (v <= 359) m.curDir = int16_t(v); }
    // a + 176 … 219: currents 2 and 3 with their depths — not shown (the surface one is what a listener wants)
    { const uint32_t v = B.u(a + 220, 8); if (v <= 251) m.wave10 = int16_t(v); }
    { const uint32_t v = B.u(a + 228, 6); if (v <= 60) m.waveS = int16_t(v); }         // 63 n/a
    { const uint32_t v = B.u(a + 234, 9); if (v <= 359) m.waveDir = int16_t(v); }
    { const uint32_t v = B.u(a + 243, 8); if (v <= 251) m.swell10 = int16_t(v); }
    { const uint32_t v = B.u(a + 251, 6); if (v <= 60) m.swellS = int16_t(v); }
    { const uint32_t v = B.u(a + 257, 9); if (v <= 359) m.swellDir = int16_t(v); }
    { const uint32_t v = B.u(a + 266, 4); if (v <= 12) m.sea = int16_t(v); }           // 13 n/a
    { const int32_t v = B.s(a + 270, 10); if (v >= -100 && v <= 500) m.water10 = int16_t(v); }   // 501 n/a
    { const uint32_t v = B.u(a + 280, 3); if (v >= 1 && v <= 6) m.precip = int16_t(v); }   // 7 n/a
    { const uint32_t v = B.u(a + 283, 9); if (v <= 501) m.sal10 = int16_t(v); }        // 501 = 50.1 ‰ or more, 510 n/a, 511 no sensor
    { const uint32_t v = B.u(a + 292, 2); if (v <= 1) m.ice = int16_t(v); }            // 3 n/a
    return true;
}

/** ★ DAC 001 FI 11 — the withdrawn IMO SN/Circ.236 met/hydro layout (352 bits). Offset values, lat before lon. */
inline bool metFi11(const Bits& B, size_t a, Met& m) {
    if (B.n() < a + 290) return false;
    m = Met(); m.fi = 11;
    pos1000(B.s(a + 24, 25), B.s(a + 0, 24), m);
    when(B, a + 49, m);
    { const uint32_t v = B.u(a + 65, 7); if (v <= 120) m.windKn = int16_t(v); }
    { const uint32_t v = B.u(a + 72, 7); if (v <= 120) m.gustKn = int16_t(v); }
    { const uint32_t v = B.u(a + 79, 9); if (v <= 359) m.windDir = int16_t(v); }
    { const uint32_t v = B.u(a + 88, 9); if (v <= 359) m.gustDir = int16_t(v); }
    { const uint32_t v = B.u(a + 97, 11); if (v <= 1200) m.air10 = int16_t(long(v) - 600); }   // −60.0…+60.0, 2047 n/a
    { const uint32_t v = B.u(a + 108, 7); if (v <= 100) m.hum = int16_t(v); }
    { const uint32_t v = B.u(a + 115, 10); if (v <= 700) m.dew10 = int16_t(long(v) - 200); }   // −20.0…+50.0
    { const uint32_t v = B.u(a + 125, 9); if (v <= 400) m.hPa = int16_t(v + 800); }
    { const uint32_t v = B.u(a + 134, 2); if (v <= 2) m.pTrend = int16_t(v); }
    { const uint32_t v = B.u(a + 136, 8); if (v <= 250) m.vis10 = int16_t(v); }
    { const uint32_t v = B.u(a + 144, 9); if (v <= 400) m.level100 = int16_t((long(v) - 100) * 10); }  // 0.1 m, −10.0…+30.0
    { const uint32_t v = B.u(a + 153, 2); if (v <= 2) m.levelTrend = int16_t(v); }
    { const uint32_t v = B.u(a + 155, 8); if (v <= 250) m.cur10 = int16_t(v); }
    { const uint32_t v = B.u(a + 163, 9); if (v <= 359) m.curDir = int16_t(v); }
    { const uint32_t v = B.u(a + 216, 8); if (v <= 250) m.wave10 = int16_t(v); }
    { const uint32_t v = B.u(a + 224, 6); if (v <= 60) m.waveS = int16_t(v); }
    { const uint32_t v = B.u(a + 230, 9); if (v <= 359) m.waveDir = int16_t(v); }
    { const uint32_t v = B.u(a + 239, 8); if (v <= 250) m.swell10 = int16_t(v); }
    { const uint32_t v = B.u(a + 247, 6); if (v <= 60) m.swellS = int16_t(v); }
    { const uint32_t v = B.u(a + 253, 9); if (v <= 359) m.swellDir = int16_t(v); }
    { const uint32_t v = B.u(a + 262, 4); if (v <= 12) m.sea = int16_t(v); }
    { const uint32_t v = B.u(a + 266, 10); if (v <= 600) m.water10 = int16_t(long(v) - 100); }   // −10.0…+50.0
    { const uint32_t v = B.u(a + 276, 3); if (v >= 1 && v <= 6) m.precip = int16_t(v); }
    { const uint32_t v = B.u(a + 279, 9); if (v <= 500) m.sal10 = int16_t(v); }
    { const uint32_t v = B.u(a + 288, 2); if (v <= 1) m.ice = int16_t(v); }
    return true;
}

// ── area notices and text ──────────────────────────────────────────────────────────────────────────────────────────
/** ★ The notice description (Circ.289 area notice table), in plain words. Empty = a reserved code. */
inline const char* noticeName(int c) {
    switch (c) {
        case 0: return "Caution: marine mammal habitat";
        case 1: return "Caution: marine mammals, reduce speed";
        case 2: return "Caution: marine mammals, stay clear";
        case 3: return "Caution: marine mammals, report sightings";
        case 4: return "Caution: protected habitat, reduce speed";
        case 5: return "Caution: protected habitat, stay clear";
        case 6: return "Caution: protected habitat, no fishing or anchoring";
        case 7: return "Caution: drifting objects";
        case 8: return "Caution: traffic congestion";
        case 9: return "Caution: marine event";
        case 10: return "Caution: divers down";
        case 11: return "Caution: swimming area";
        case 12: return "Caution: dredging";
        case 13: return "Caution: survey work";
        case 14: return "Caution: underwater work";
        case 15: return "Caution: seaplanes";
        case 16: return "Caution: fishing nets in the water";
        case 17: return "Caution: fishing vessels gathered";
        case 18: return "Caution: fairway closed";
        case 19: return "Caution: harbour closed";
        case 20: return "Caution: risk (see text)";
        case 21: return "Caution: underwater vehicle";
        case 23: return "Weather: storm front (line squall)";
        case 24: return "Weather: hazardous sea ice";
        case 25: return "Weather: storm warning";
        case 26: return "Weather: high wind";
        case 27: return "Weather: high waves";
        case 28: return "Weather: restricted visibility";
        case 29: return "Weather: strong currents";
        case 30: return "Weather: heavy icing";
        case 32: return "Restricted: fishing prohibited";
        case 33: return "Restricted: no anchoring";
        case 34: return "Restricted: entry needs approval";
        case 35: return "Restricted: entry prohibited";
        case 36: return "Restricted: military exercise area";
        case 37: return "Restricted: firing danger area";
        case 38: return "Restricted: drifting mines";
        case 40: return "Anchorage open";
        case 41: return "Anchorage closed";
        case 42: return "Anchoring prohibited";
        case 43: return "Deep-draught anchorage";
        case 44: return "Shallow-draught anchorage";
        case 45: return "Ship-to-ship transfer area";
        case 56: return "Security alert level 1";
        case 57: return "Security alert level 2";
        case 58: return "Security alert level 3";
        case 64: return "Distress: vessel disabled and adrift";
        case 65: return "Distress: vessel sinking";
        case 66: return "Distress: abandoning ship";
        case 67: return "Distress: medical help requested";
        case 68: return "Distress: vessel flooding";
        case 69: return "Distress: fire or explosion";
        case 70: return "Distress: vessel aground";
        case 71: return "Distress: collision";
        case 72: return "Distress: vessel listing or capsizing";
        case 73: return "Distress: vessel under attack";
        case 74: return "Distress: person overboard";
        case 75: return "Distress: search and rescue area";
        case 76: return "Distress: pollution response area";
        case 80: return "Instruction: call VTS here";
        case 81: return "Instruction: call the port here";
        case 82: return "Instruction: do not go beyond here";
        case 83: return "Instruction: wait for instructions here";
        case 84: return "Instruction: go here and wait";
        case 85: return "Instruction: cleared to berth";
        case 88: return "Pilot boarding position";
        case 89: return "Icebreaker waiting area";
        case 90: return "Place of refuge";
        case 91: return "Icebreakers here";
        case 92: return "Response units here";
        case 93: return "VTS target";
        case 94: return "Rogue or suspicious vessel";
        case 95: return "Vessel asking for help (not distress)";
        case 96: return "Chart: sunken vessel";
        case 97: return "Chart: submerged object";
        case 98: return "Chart: semi-submerged object";
        case 99: return "Chart: shoal";
        case 100: return "Chart: shoal to the north";
        case 101: return "Chart: shoal to the east";
        case 102: return "Chart: shoal to the south";
        case 103: return "Chart: shoal to the west";
        case 104: return "Chart: channel obstruction";
        case 105: return "Chart: reduced headroom";
        case 106: return "Chart: bridge closed";
        case 107: return "Chart: bridge partly open";
        case 108: return "Chart: bridge fully open";
        case 112: return "Ship report: icing";
        case 114: return "Ship report: see text";
        case 120: return "Recommended route";
        case 121: return "Alternative route";
        case 122: return "Recommended route through ice";
        case 125: return "Notice (see text)";
        case 126: return "Cancelled";
        case 127: return "Notice";
        default: return "";
    }
}
inline const char* shapeName(int s) {
    static const char* n[6] = { "circle", "rectangle", "sector", "line", "polygon", "text" };
    return s >= 0 && s < 6 ? n[s] : "";
}

/** ★ DAC 001 FI 22 / 23 — area notice (Circ.289): a 55-bit header from `a`, then 87-bit sub-areas. */
inline bool areaNotice(const Bits& B, size_t a, Notice& n) {
    if (B.n() < a + 55) return false;
    n = Notice();
    n.what = int(B.u(a + 10, 7));   // a + 0: 10-bit message link id
    n.month = int(B.u(a + 17, 4)); n.day = int(B.u(a + 21, 5)); n.hour = int(B.u(a + 26, 5)); n.minute = int(B.u(a + 31, 6));
    n.durationMin = int(B.u(a + 37, 18));
    std::string text;
    for (size_t s = a + 55; s + 87 <= B.n(); s += 87) {
        const int shape = int(B.u(s, 3));
        if (shape == 5) { const std::string t = B.text(s + 3, 14); if (!t.empty()) { if (!text.empty()) text += ' '; text += t; } continue; }
        if (shape > 4 || n.shape >= 0) continue;   // ★ the first shape is the notice's place; later ones only extend it
        const int scale = int(B.u(s + 3, 2));
        const double lon = B.s(s + 5, 25) / 60000.0, lat = B.s(s + 30, 24) / 60000.0;
        if (std::fabs(lon) > 180 || std::fabs(lat) > 90) continue;
        n.shape = shape; n.lat = lat; n.lon = lon;
        const double mul = std::pow(10.0, scale);
        if (shape == 0 || shape == 2) { const uint32_t r = B.u(s + 57, 12); if (r) n.sizeM = r * mul; }
        else if (shape == 1) { const uint32_t e = B.u(s + 57, 8), no = B.u(s + 65, 8); n.sizeM = std::max(e, no) * mul; }
        // a line or polygon: the first point is the place; its legs follow in the next sub-areas
    }
    n.text = text;
    return true;
}
/** ★ DAC 001 FI 29 / 30 — text description: a 10-bit link id, then six-bit text. */
inline bool textDescription(const Bits& B, size_t a, Notice& n) {
    if (B.n() < a + 16) return false;
    n = Notice();
    n.text = B.text(a + 10, int((B.n() - a - 10) / 6));
    return !n.text.empty();
}

/** One binary message → what we read from it. @return 1 met, 2 notice, 0 counted only. */
inline int parse(const std::vector<uint8_t>& bytes, Kind& k, Met& met, Notice& note) {
    Bits B{bytes};
    const int type = int(B.u(0, 6));
    uint32_t dest = 0;
    if (!header(B, type, k, &dest)) return 0;
    if (k.dac != 1) return 0;
    const size_t a = k.dataAt;
    switch (k.fi) {
        case 31: return metFi31(B, a, met) && hasAny(met) ? 1 : 0;
        case 11: return metFi11(B, a, met) && hasAny(met) ? 1 : 0;
        case 22: case 23: if (areaNotice(B, a, note)) { note.fi = k.fi; note.to = dest; return 2; } return 0;
        case 29: case 30: if (textDescription(B, a, note)) { note.fi = k.fi; note.to = dest; return 2; } return 0;
        default: return 0;
    }
}

// ── words ──────────────────────────────────────────────────────────────────────────────────────────────────────────
inline const char* trendWord(int t) { return t == 0 ? "steady" : t == 1 ? "falling" : t == 2 ? "rising" : ""; }
inline std::string fmt1(double v, int dp) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }
inline const char* compass16(int deg) {
    static const char* p[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    return p[int(std::floor(std::fmod(deg + 11.25 + 3600.0, 360.0) / 22.5)) & 15];
}
/** ★ The readable line: "wind SW 14 kn gusting 21 · 11.2 °C · 1012 hPa falling · visibility 4.5 NM · tide 3.21 m rising". */
inline std::string metLine(const Met& m) {
    static const char* const dot = " \xc2\xb7 ";
    std::string s;
    auto add = [&](const std::string& p) { if (p.empty()) return; if (!s.empty()) s += dot; s += p; };
    if (m.windKn != kNone) {
        std::string w = "wind ";
        if (m.windDir != kNone) w += std::string(compass16(m.windDir)) + " ";
        w += std::to_string(m.windKn) + " kn";
        if (m.gustKn != kNone && m.gustKn > m.windKn) w += " gusting " + std::to_string(m.gustKn);
        add(w);
    }
    if (m.air10 != kNone) add(fmt1(m.air10 / 10.0, 1) + " \xc2\xb0" "C");
    if (m.hPa != kNone) add(std::to_string(m.hPa) + " hPa" + (m.pTrend != kNone ? std::string(" ") + trendWord(m.pTrend) : ""));
    if (m.vis10 != kNone) add(std::string("visibility ") + (m.visMore ? "over " : "") + fmt1(m.vis10 / 10.0, 1) + " NM");
    if (m.level100 != kNone) add("tide " + fmt1(m.level100 / 100.0, 2) + " m" + (m.levelTrend != kNone ? std::string(" ") + trendWord(m.levelTrend) : ""));
    if (m.wave10 != kNone) add("waves " + fmt1(m.wave10 / 10.0, 1) + " m" + (m.waveS != kNone ? " " + std::to_string(m.waveS) + " s" : ""));
    if (m.water10 != kNone) add("sea " + fmt1(m.water10 / 10.0, 1) + " \xc2\xb0" "C");
    return s;
}
inline std::string noticeLine(const Notice& n) {
    static const char* const dot = " \xc2\xb7 ";
    std::string s = n.what >= 0 ? noticeName(n.what) : "";
    if (n.what >= 0 && s.empty()) s = "Area notice " + std::to_string(n.what);
    if (!n.text.empty()) s += (s.empty() ? "" : ": ") + n.text;
    if (n.shape >= 0) {
        s += dot; s += shapeName(n.shape);
        if (n.sizeM == n.sizeM && n.sizeM > 0) s += " " + (n.sizeM >= 1000 ? fmt1(n.sizeM / 1000, n.sizeM < 10000 ? 1 : 0) + " km" : std::to_string(long(n.sizeM)) + " m");
        s += " at " + fmt1(n.lat, 4) + " " + fmt1(n.lon, 4);
    }
    if (n.what >= 0 && n.day >= 1 && n.hour <= 23 && n.minute <= 59) {
        char b[48]; std::snprintf(b, sizeof b, "from %02d:%02d UTC %02d-%02d", n.hour, n.minute, n.day, n.month);
        s += dot; s += b;
        if (n.durationMin == 0) s += ", CANCELLED";
        else if (n.durationMin > 0 && n.durationMin < 262143) s += n.durationMin % 60 ? ", for " + std::to_string(n.durationMin) + " min" : ", for " + std::to_string(n.durationMin / 60) + " h";
    }
    return s;
}

/** ★ A met report as JSON (a vessel's "met" array; the met store's too). `age` in seconds. */
inline void metJson(const Met& m, double age, std::string& o) {
    o += "{\"fi\":" + std::to_string(m.fi) + ",\"age\":" + fmt1(age < 0 ? 0 : age, 0);
    auto i = [&](const char* k, int16_t v) { if (v != kNone) { o += ",\""; o += k; o += "\":" + std::to_string(v); } };
    auto d = [&](const char* k, int16_t v, double sc, int dp) { if (v != kNone) { o += ",\""; o += k; o += "\":" + fmt1(v / sc, dp); } };
    if (m.lat != kNoPos32) o += ",\"lat\":" + fmt1(m.lat / 1e7, 5) + ",\"lon\":" + fmt1(m.lon / 1e7, 5);
    if (m.day >= 1 && m.day <= 31 && m.hour <= 23 && m.minute <= 59) { char b[40]; std::snprintf(b, sizeof b, ",\"obs\":\"%02u %02u:%02u\"", unsigned(m.day), unsigned(m.hour), unsigned(m.minute)); o += b; }
    i("wind", m.windKn); i("gust", m.gustKn); i("windDir", m.windDir); i("gustDir", m.gustDir);
    d("airC", m.air10, 10, 1); i("hum", m.hum); d("dewC", m.dew10, 10, 1); i("hPa", m.hPa); i("pTrend", m.pTrend);
    d("vis", m.vis10, 10, 1); if (m.vis10 != kNone && m.visMore) o += ",\"visMore\":1";
    d("level", m.level100, 100, 2); i("levelTrend", m.levelTrend);
    d("cur", m.cur10, 10, 1); i("curDir", m.curDir);
    d("wave", m.wave10, 10, 1); i("waveS", m.waveS); i("waveDir", m.waveDir);
    d("swell", m.swell10, 10, 1); i("swellS", m.swellS); i("swellDir", m.swellDir);
    i("sea", m.sea); d("waterC", m.water10, 10, 1); i("precip", m.precip); d("sal", m.sal10, 10, 1); i("ice", m.ice);
    o += "}";
}

// ── type 21: the AtoN status byte (IALA A-126: page 7 = RACON / light / health) ───────────────────────────────────
struct AtonStatus { bool valid = false; int light = -1, racon = -1; bool alarm = false; };
/** `st` = bits 260–267 of a type 21. Page 7 (top three bits 111): RACON 2 bits · light 2 bits · health 1 bit.
 *  RACON: 0 none · 1 fitted, not monitored · 2 working · 3 fault. Light: 0 none · 1 on · 2 off · 3 fault. */
inline AtonStatus atonStatus(int st) {
    AtonStatus a;
    if (st < 0 || ((st >> 5) & 7) != 7) return a;
    a.valid = true; a.racon = (st >> 3) & 3; a.light = (st >> 1) & 3; a.alarm = (st & 1) != 0;
    return a;
}
inline const char* lightWord(int l) { return l == 1 ? "on" : l == 2 ? "off" : l == 3 ? "FAULT" : ""; }
inline const char* raconWord(int r) { return r == 1 ? "fitted, not monitored" : r == 2 ? "working" : r == 3 ? "FAULT" : ""; }

} // namespace asmsg
} // namespace vibeais
