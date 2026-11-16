// vibe_acars_apps.h — V12 aviation datalink: what an ACARS message SAYS, where its layout is published and checkable.
// docs/v12/AVIATION.md §Applications. The block itself (address, label, flight, text) is vibe_acars.h; this reads the
// text of a BCS-good block (VHF ACARS, ACARS over VDL2, ACARS over HFDL) for the few structures that carry flight data:
//
//   ★ ARINC 622 (labels A6 / AA / B6 / BA / H1): "/<ground address>.<IMI>.<registration><hex>" with its own CRC-16. The
//     IMI says which ATS application it is — ADS-C ("ADS", "DIS") or FANS-1/A CPDLC ("AT1", "CR1", "CC1", "DR1"). ADS-C
//     reports are decoded for the aircraft's POSITION, ALTITUDE and TIME (a basic report or an event report: the same
//     10-byte group) and its flight ID. CPDLC is only NAMED: its content is controller–pilot conversation (DECODER-
//     RESEARCH.md §4.3) and is never decoded here.
//   ★ ARINC 702-style FMS downlinks (H1, "#M1B" sublabel): "<IMI>/<IEI>…" ending in a 4-hex-digit CRC. Accepted ONLY when
//     that CRC is right; then the position (IMI POS, IEI PS), its time and flight level, and the airports (IEI AF).
//   ★ The fixed-layout airline reports with a published sample set: label 16 "N 51.123,W  1.234,…" positions, label 15
//     "(2N51123W  1234" positions (+ the OFF time), label 44 POS / OFF / ON / IN, labels QP / QQ / QR / QS (OUT / OFF /
//     ON / IN with the airports), label H2 "02E" weather reports (their observation points ARE positions).
//
// ★★★ NEVER A GUESSED FIELD. Every format is matched character class by character class over its whole fixed part, every
//     number range-checked (minutes < 60, |lat| ≤ 90, an hour < 24); a near miss is NOT parsed — it is counted as
//     "unparsed", never shown as a best guess. A label we have no layout for is counted the same way. Where public
//     samples disagree about a layout (label 20's "N38160": decimal degrees in one sample set, degrees and minutes in
//     another), the label is left out until a real capture settles it.
// ★★★ PRIVACY (DECODER-RESEARCH.md §4.3, the UK WT Act s.48): the fields this file returns — position, altitude, the time
//     of the fix, flight ID, airports, the OOOI event — are FLIGHT DATA, shown as the registration and flight are. Nothing
//     here returns message content: no free text, no CPDLC, no weather values, no fuel. Bodies stay behind the owner's
//     opt-in in vibe_avi_service.h.
// ★★ SOURCES: libacars (MIT) and its documentation for the ARINC 622 envelope, its CRC and the ADS-C groups; the
//    acars-decoder-typescript sample set (MIT) for the airline layouts and their sample messages — both READ for the
//    facts, no code taken; our own code throughout. The CRCs were checked on those published samples before use.
// ★ Bounded: no allocation beyond the message's own length; the reassembler holds ≤ 32 messages of ≤ 8 blocks.
#pragma once
#include "vibe_acars.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace vibeacarsapps {

/** What was read out of one message. `kind` empty = nothing recognised (the caller counts it as unparsed). */
struct Result {
    std::string kind;          ///< plain words: "ADS-C position", "FMS position", "position report", "OOOI", "CPDLC", …
    std::string app;           ///< the counter: "pos" | "oooi" | "adsc" | "cpdlc" | "route"
    bool hasPos = false;
    double lat = NAN, lon = NAN;
    bool hasAlt = false;
    int altFt = 0;
    int fixSec = -1;           ///< the time of the fix as REPORTED: seconds past midnight UTC, or -1
    int fixPastHour = -1;      ///< ADS-C reports seconds past the hour only (×8: 1/8 s units), or -1
    std::string from, to;      ///< ICAO airports (four letters)
    std::string oooi;          ///< "out" | "off" | "on" | "in"
    int oooiSec = -1;          ///< seconds past midnight UTC of the event, or -1
    std::string flight;        ///< ADS-C flight ID group
};

// ── small strict helpers ─────────────────────────────────────────────────────────────────────────────────────────────
inline bool isDig(char c) { return c >= '0' && c <= '9'; }
inline bool isUp(char c) { return c >= 'A' && c <= 'Z'; }
inline bool isHex(char c) { return isDig(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
inline int hexVal(char c) { return isDig(c) ? c - '0' : (c >= 'a' ? c - 'a' + 10 : c - 'A' + 10); }
inline bool allDig(const std::string& s, size_t p, size_t n) {
    if (p + n > s.size()) return false;
    for (size_t i = 0; i < n; ++i) if (!isDig(s[p + i])) return false;
    return true;
}
inline int num(const std::string& s, size_t p, size_t n) { int v = 0; for (size_t i = 0; i < n; ++i) v = v * 10 + (s[p + i] - '0'); return v; }
/** An ICAO airport: four capital letters. */
inline bool icaoAt(const std::string& s, size_t p) { return p + 4 <= s.size() && isUp(s[p]) && isUp(s[p + 1]) && isUp(s[p + 2]) && isUp(s[p + 3]); }
/** "HHMM" or "HHMMSS" at p (n = 4 | 6), a real time of day → seconds past midnight, else -1. */
inline int hms(const std::string& s, size_t p, size_t n) {
    if (!allDig(s, p, n)) return -1;
    const int h = num(s, p, 2), m = num(s, p + 2, 2), sec = n == 6 ? num(s, p + 4, 2) : 0;
    if (h > 23 || m > 59 || sec > 59) return -1;
    return h * 3600 + m * 60 + sec;
}
inline bool latLonOk(double la, double lo) { return std::isfinite(la) && std::isfinite(lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180; }

/** "N51286W000380" / "N51286 W000380" at p: degrees, minutes and tenths (5 + 6 digits; a space may pad the longitude's
 *  hundreds). Returns the characters used, 0 = not one. */
inline size_t degMinTenths(const std::string& s, size_t p, double& la, double& lo) {
    if (p + 12 > s.size() || (s[p] != 'N' && s[p] != 'S') || !allDig(s, p + 1, 5)) return 0;
    size_t q = p + 6;
    if (q < s.size() && s[q] == ' ') ++q;
    if (q + 7 > s.size() || (s[q] != 'E' && s[q] != 'W')) return 0;
    std::string lon = s.substr(q + 1, 6);
    for (char& c : lon) if (c == ' ') c = '0';           // "W 77216" — the hundreds as a space
    if (!allDig(lon, 0, 6)) return 0;
    const int ld = num(s, p + 1, 2), lm = num(s, p + 3, 3), od = num(lon, 0, 3), om = num(lon, 3, 3);
    if (lm >= 600 || om >= 600 || ld > 90 || od > 180) return 0;
    la = (ld + lm / 600.0) * (s[p] == 'S' ? -1 : 1);
    lo = (od + om / 600.0) * (s[q] == 'W' ? -1 : 1);
    if (!latLonOk(la, lo)) return 0;
    return q + 7 - p;
}

// ── the CRCs (checked on the published samples: test-acars-apps §1) ──────────────────────────────────────────────────
/** CRC-16, polynomial 0x1021 MSB first, preset 0xFFFF; a message run through it WITH its CRC leaves 0x1D0F (ARINC 622). */
inline uint16_t crc622(const uint8_t* d, size_t n, uint16_t c = 0xFFFF) {
    for (size_t i = 0; i < n; ++i) {
        c ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return c;
}
/** The FMS downlinks' CRC: CRC-16 reflected 0x1021 (0x8408), preset and final XOR 0xFFFF, written as four hex digits in
 *  REVERSED nibble order. */
inline uint16_t crc702(const char* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= uint8_t(d[i]);
        for (int b = 0; b < 8; ++b) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
    }
    c ^= 0xFFFF;
    return uint16_t(((c & 0xF) << 12) | (((c >> 4) & 0xF) << 8) | (((c >> 8) & 0xF) << 4) | (c >> 12));
}

// ── label H1: the sublabel and MFI in front of the application ───────────────────────────────────────────────────────
/** Downlink "#M1B" (+ "/B6 "), uplink "- #M1" (+ "/B6 "): how many characters to skip to reach the application. */
inline size_t h1Prefix(const std::string& t, bool downlink) {
    size_t p = 0;
    if (downlink) { if (t.size() >= 4 && t[0] == '#' && t[3] == 'B') p = 4; else return 0; }
    else { if (t.size() >= 5 && t.compare(0, 3, "- #") == 0) p = 5; else return 0; }
    if (t.size() >= p + 4 && t[p] == '/' && t[p + 3] == ' ') p += 4;
    return p;
}

// ── ADS-C (inside ARINC 622) ─────────────────────────────────────────────────────────────────────────────────────────
struct Bits {
    const uint8_t* b; size_t n, pos = 0;
    Bits(const uint8_t* d, size_t len) : b(d), n(len * 8) {}
    bool read(int k, uint32_t& v) {
        if (pos + size_t(k) > n) return false;
        v = 0;
        for (int i = 0; i < k; ++i, ++pos) v = (v << 1) | ((b[pos >> 3] >> (7 - (pos & 7))) & 1u);
        return true;
    }
};
inline int32_t sext(uint32_t v, int bits) { const uint32_t m = 1u << (bits - 1); return int32_t((v ^ m) - m); }
/** A 21-bit coordinate: MSB = 90° … the field spans ±180° (r × (180 − 90/2¹⁹) / (2²⁰ − 1)). */
inline double adsCoord(uint32_t v) { return double(sext(v, 21)) * (180.0 - 90.0 / 524288.0) / double(0xFFFFF); }

/** The ADS-C groups of a DOWNLINK report. Position from the first basic-shaped group (basic report 7, emergency 9, event
 *  reports 10 / 18 / 19 / 20 — the same 10 bytes), flight ID from group 12. An unknown group ends the walk (its length is
 *  unknown); what was read before it stands (the message CRC already passed). */
inline void adsC(const uint8_t* d, size_t n, Result& r) {
    size_t p = 0;
    while (p < n) {
        const uint8_t tag = d[p++];
        const size_t left = n - p;
        size_t len = 0;
        switch (tag) {
            case 3: len = 1; break;                              // acknowledgement
            case 4: len = (left >= 2 && (d[p + 1] == 1 || d[p + 1] == 2 || d[p + 1] == 7)) ? 3 : 2; break;   // negative ack
            case 5: {                                            // non-compliance: groups of 2 + their parameter nibbles
                if (left < 2) return;
                size_t q = p + 2;
                for (int g = 0; g < d[p + 1]; ++g) {
                    if (q + 2 > n) return;
                    const uint8_t f = d[q + 1];
                    const int cnt = (f & 0xC0) ? 0 : (f & 0x0F);
                    q += 2 + size_t(cnt / 2 + cnt % 2);
                }
                len = q - p; break;
            }
            case 6: len = 0; break;                              // cancel emergency
            case 7: case 9: case 10: case 18: case 19: case 20: {
                len = 10;
                if (left < len) return;
                if (!r.hasPos) {
                    Bits bs(d + p, 10);
                    uint32_t la, lo, al, ts, fl;
                    bs.read(21, la); bs.read(21, lo); bs.read(16, al); bs.read(15, ts); bs.read(7, fl);
                    const int accuracy = int((fl >> 1) & 7);     // 0 = the navigation unit lost its position
                    const double lat = adsCoord(la), lon = adsCoord(lo);
                    const int alt = sext(al, 16) * 4;
                    if (accuracy > 0 && latLonOk(lat, lon) && alt > -2000 && alt < 70000) {
                        r.hasPos = true; r.lat = lat; r.lon = lon; r.hasAlt = true; r.altFt = alt;
                        if (ts < 3600u * 8u) r.fixPastHour = int(ts);   // 1/8 s past the hour
                    }
                }
                break;
            }
            case 12: {                                           // flight ID: 8 × 6-bit characters
                len = 6;
                if (left < len) return;
                Bits bs(d + p, 6);
                std::string f;
                bool good = true;
                for (int i = 0; i < 8; ++i) {
                    uint32_t c; bs.read(6, c);
                    const char ch = char((c & 0x20) ? c : c + 0x40);
                    if (!(isUp(ch) || isDig(ch) || ch == ' ')) good = false;
                    f += ch;
                }
                while (!f.empty() && f.back() == ' ') f.pop_back();
                while (!f.empty() && f.front() == ' ') f.erase(f.begin());
                if (good && f.size() >= 2) r.flight = f;
                break;
            }
            case 13: len = 17; break;   // predicted route
            case 14: case 15: len = 5; break;   // earth / air reference
            case 16: len = 4; break;    // meteo (never decoded: weather is not flight data)
            case 17: len = 3; break;    // airframe ID
            case 22: len = 8; break;    // intermediate projection
            case 23: len = 9; break;    // fixed projection
            default: return;
        }
        if (left < len) return;
        p += len;
    }
}

/** ARINC 622 at `t` (sublabel already skipped). True when the envelope was recognised AND its CRC is right. */
inline bool arinc622(const std::string& t, bool downlink, Result& r) {
    size_t p = 0;
    if (p < t.size() && t[p] == '/') ++p;
    // the ground address: 7 or 4 capitals/digits, then ".IMI"
    size_t g = 0;
    for (size_t k : { size_t(7), size_t(4) }) {
        if (p + k + 4 > t.size() || t[p + k] != '.') continue;
        bool ok = true;
        for (size_t i = 0; i < k; ++i) if (!(isUp(t[p + i]) || isDig(t[p + i]))) ok = false;
        if (ok) { g = k; break; }
    }
    if (!g) return false;
    const size_t imi = p + g + 1;
    if (imi + 3 + 7 + 4 > t.size()) return false;
    const std::string id = t.substr(imi, 3);
    const bool ads = id == "ADS", dis = id == "DIS";
    const bool cpdlc = id == "AT1" || id == "CR1" || id == "CC1" || id == "DR1";
    if (!ads && !dis && !cpdlc) return false;
    const size_t hx = imi + 3 + 7;                  // IMI, the 7-character registration (dot-padded), then hex
    size_t e = hx;
    while (e < t.size() && isHex(t[e])) ++e;
    const size_t nh = e - hx;
    if (nh < 4 || (nh & 1) || nh > 2 * 1100) return false;
    std::vector<uint8_t> buf;
    buf.reserve(10 + nh / 2);
    for (size_t i = imi; i < hx; ++i) buf.push_back(uint8_t(t[i]));
    for (size_t i = hx; i < e; i += 2) buf.push_back(uint8_t(hexVal(t[i]) << 4 | hexVal(t[i + 1])));
    if (crc622(buf.data(), buf.size()) != 0x1D0F) return false;
    const uint8_t* body = buf.data() + 10;
    const size_t nb = buf.size() - 10 - 2;          // the CRC off
    if (cpdlc) { r.kind = "CPDLC"; r.app = "cpdlc"; return true; }    // ★ named, never decoded (§4.3)
    r.app = "adsc";
    if (dis) { r.kind = "ADS-C disconnect"; return true; }
    if (!downlink) { r.kind = "ADS-C contract request"; return true; }
    adsC(body, nb, r);
    r.kind = r.hasPos ? "ADS-C position" : "ADS-C report";
    return true;
}

// ── ARINC 702-style FMS downlinks (H1) ───────────────────────────────────────────────────────────────────────────────
/** The comma fields of a POS / PS element: [0] the position, then a time "HHMMSS" at [1] or [2], the flight level right
 *  after it. Returns true when the position was good. */
inline bool fmsPosFields(const std::string& s, Result& r) {
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) { const size_t c = s.find(',', a); f.push_back(s.substr(a, c == std::string::npos ? std::string::npos : c - a)); if (c == std::string::npos) break; a = c + 1; if (f.size() > 40) break; }
    double la, lo;
    if (f.empty() || degMinTenths(f[0], 0, la, lo) != f[0].size()) return false;
    r.hasPos = true; r.lat = la; r.lon = lo;
    for (size_t ti : { size_t(1), size_t(2) }) {
        if (ti + 1 < f.size() && f[ti].size() == 6 && hms(f[ti], 0, 6) >= 0) {
            r.fixSec = hms(f[ti], 0, 6);
            if (f[ti + 1].size() == 3 && allDig(f[ti + 1], 0, 3)) { r.hasAlt = true; r.altFt = num(f[ti + 1], 0, 3) * 100; }
            break;
        }
    }
    return true;
}
inline bool arinc702(const std::string& t0, Result& r) {
    std::string t = t0;
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) t.pop_back();
    std::string clean; clean.reserve(t.size());
    for (char c : t) if (c != '\r' && c != '\n') clean += c;
    t = clean;
    if (!t.empty() && t[0] == '/') { const size_t d = t.find('.'); if (d == std::string::npos || d > 9) return false; t = t.substr(d + 1); }
    if (t.size() < 8 || !isUp(t[0]) || !isUp(t[1]) || !isUp(t[2])) return false;
    for (size_t i = t.size() - 4; i < t.size(); ++i) if (!isHex(t[i]) || (t[i] >= 'a' && t[i] <= 'f')) return false;
    const uint16_t want = uint16_t(hexVal(t[t.size() - 4]) << 12 | hexVal(t[t.size() - 3]) << 8 | hexVal(t[t.size() - 2]) << 4 | hexVal(t[t.size() - 1]));
    if (crc702(t.data(), t.size() - 4) != want) return false;
    const std::string data = t.substr(0, t.size() - 4);
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) { const size_t c = data.find('/', a); f.push_back(data.substr(a, c == std::string::npos ? std::string::npos : c - a)); if (c == std::string::npos) break; a = c + 1; if (f.size() > 60) break; }
    const std::string imi = f[0].substr(0, 3);
    if (imi == "POS" && f[0].size() > 3) fmsPosFields(f[0].substr(3), r);
    for (size_t i = 1; i < f.size(); ++i) {
        if (f[i].size() < 2) continue;
        const std::string iei = f[i].substr(0, 2), v = f[i].substr(2);
        if (iei == "PS" && !r.hasPos) fmsPosFields(v, r);
        else if (iei == "AF" && v.size() == 9 && icaoAt(v, 0) && v[4] == ',' && icaoAt(v, 5)) { r.from = v.substr(0, 4); r.to = v.substr(5, 4); }
    }
    r.app = r.hasPos ? "pos" : (!r.from.empty() ? "route" : "fms");
    r.kind = r.hasPos ? "FMS position" : (!r.from.empty() ? "FMS route" : "FMS report " + imi);
    return true;
}

// ── fixed-layout airline reports ─────────────────────────────────────────────────────────────────────────────────────
/** Label 16 "N 44.203,W 86.546,31965,…" or "N 28.177/W 96.055": decimal degrees (three decimals), then the altitude. */
inline bool label16(const std::string& t, Result& r) {
    size_t p = 0;
    auto deg = [&](char a, char b, int maxDeg, double& out) -> bool {
        if (p >= t.size() || (t[p] != a && t[p] != b)) return false;
        const double sg = t[p] == a ? 1 : -1;
        ++p;
        while (p < t.size() && t[p] == ' ') ++p;
        const size_t s0 = p;
        while (p < t.size() && isDig(t[p])) ++p;
        const size_t nd = p - s0;
        if (nd < 1 || nd > 3 || p + 4 > t.size() || t[p] != '.' || !allDig(t, p + 1, 3)) return false;
        const int d = num(t, s0, nd), frac = num(t, p + 1, 3);
        p += 4;
        if (d > maxDeg) return false;
        out = sg * (d + frac / 1000.0);
        return std::fabs(out) <= maxDeg;
    };
    double la, lo;
    if (!deg('N', 'S', 90, la)) return false;
    if (p >= t.size() || (t[p] != ',' && t[p] != '/')) return false;
    const char sep = t[p++];
    if (!deg('E', 'W', 180, lo)) return false;
    if (sep == '/') { if (p != t.size()) return false; }
    else {
        if (p >= t.size() || t[p] != ',') return false;
        ++p;
        size_t q = p;
        while (q < t.size() && isDig(t[q])) ++q;
        if (q >= t.size() || t[q] != ',') return false;      // variant 1: "…,alt,x,y"
        if (q - p >= 1 && q - p <= 5) { r.hasAlt = true; r.altFt = num(t, p, q - p); }
    }
    r.hasPos = true; r.lat = la; r.lon = lo;
    r.kind = "position report"; r.app = "pos";
    return true;
}
/** Label 15 "(2N38448W 77216…(Z": degrees, minutes and tenths; "OFF" then six date characters and HHMM = the take-off. */
inline bool label15(const std::string& t, Result& r) {
    if (t.size() < 16 || t.compare(0, 2, "(2") != 0 || t.compare(t.size() - 2, 2, "(Z") != 0) return false;
    double la, lo;
    const size_t n = degMinTenths(t, 2, la, lo);
    if (n != 13) return false;
    r.hasPos = true; r.lat = la; r.lon = lo;
    r.kind = "position report"; r.app = "pos";
    const size_t o = 2 + n;
    if (t.compare(o, 3, "OFF") == 0 && o + 13 <= t.size()) {
        const int s = hms(t, o + 9, 4);
        if (s >= 0) { r.oooi = "off"; r.oooiSec = s; }
    }
    return true;
}
/** Label 44 "POS02,N38171W077507,319,KJFK,KUZA,…", "OFF02,N…,KFDK,KSNA,1106,2124,…", "ON01,…,1106,004023", "IN01,…". */
inline bool label44(const std::string& t, Result& r) {
    std::string ev;
    size_t p = 0;
    if (t.compare(0, 3, "POS") == 0) { ev = "pos"; p = 3; }
    else if (t.compare(0, 3, "OFF") == 0) { ev = "off"; p = 3; }
    else if (t.compare(0, 2, "ON") == 0) { ev = "on"; p = 2; }
    else if (t.compare(0, 2, "IN") == 0) { ev = "in"; p = 2; }
    else return false;
    if (!allDig(t, p, 2) || p + 3 > t.size() || t[p + 2] != ',') return false;
    p += 3;
    double la, lo;
    if (degMinTenths(t, p, la, lo) != 13 || p + 13 >= t.size() || t[p + 13] != ',') return false;
    p += 14;
    if (ev == "pos") {
        if (!allDig(t, p, 3) || p + 3 >= t.size() || t[p + 3] != ',') return false;
        const int fl = num(t, p, 3);
        p += 4;
        if (!icaoAt(t, p) || p + 9 > t.size() || t[p + 4] != ',' || !icaoAt(t, p + 5)) return false;
        r.hasAlt = true; r.altFt = fl * 100;
    } else {
        if (!icaoAt(t, p) || p + 9 > t.size() || t[p + 4] != ',' || !icaoAt(t, p + 5)) return false;
        const size_t d = p + 9;                                   // ",MMDD,HHMM[SS]"
        if (d + 10 > t.size() || t[d] != ',' || !allDig(t, d + 1, 4) || t[d + 5] != ',') return false;
        size_t e = d + 6; while (e < t.size() && isDig(t[e])) ++e;
        const int s = (e - d - 6 == 4 || e - d - 6 == 6) ? hms(t, d + 6, e - d - 6) : -1;
        if (s < 0) return false;
        r.oooi = ev; r.oooiSec = s;
    }
    r.from = t.substr(p, 4); r.to = t.substr(p + 5, 4);
    r.hasPos = true; r.lat = la; r.lon = lo;
    r.kind = ev == "pos" ? "position report" : "OOOI"; r.app = ev == "pos" ? "pos" : "oooi";
    return true;
}
/** QP / QQ / QR / QS: "KSFOKLAX1234" — OUT / OFF / ON / IN, the airports, the time (QQ's long form adds a position). */
inline bool labelQ(char l2, const std::string& t, Result& r) {
    if (t.size() < 12 || !icaoAt(t, 0) || !icaoAt(t, 4)) return false;
    static const char* ev[] = { "out", "off", "on", "in" };
    const int s = hms(t, 8, 4);
    const bool longQQ = l2 == 'Q' && t.size() >= 42 && t.compare(12, 7, "\r\n001FE") == 0;
    if (s < 0 && !longQQ) return false;
    r.oooi = ev[l2 - 'P']; r.from = t.substr(0, 4); r.to = t.substr(4, 4);
    r.kind = "OOOI"; r.app = "oooi";
    if (longQQ) {
        // "…\r\n001FE" DD HHMMSS N DDMM.M W DDDMM.M
        const int s2 = hms(t, 21, 6);
        r.oooiSec = s2 >= 0 ? s2 : s;
        const size_t q = 27;
        if (q + 15 <= t.size() && (t[q] == 'N' || t[q] == 'S') && allDig(t, q + 1, 4) && t[q + 5] == '.' && isDig(t[q + 6])
            && (t[q + 7] == 'E' || t[q + 7] == 'W') && allDig(t, q + 8, 5) && t[q + 13] == '.' && isDig(t[q + 14])) {
            const int ld = num(t, q + 1, 2), lm = num(t, q + 3, 2) * 10 + (t[q + 6] - '0');
            const int od = num(t, q + 8, 3), om = num(t, q + 11, 2) * 10 + (t[q + 14] - '0');
            const double la = (ld + lm / 600.0) * (t[q] == 'S' ? -1 : 1), lo = (od + om / 600.0) * (t[q + 7] == 'W' ? -1 : 1);
            if (lm < 600 && om < 600 && latLonOk(la, lo)) { r.hasPos = true; r.lat = la; r.lon = lo; r.fixSec = r.oooiSec; }
        }
    } else r.oooiSec = s;
    return true;
}
/** H2 "02E" weather reports: "02E" DD ORIG DEST then 32-character observations (position, HHMM, flight level + a digit,
 *  temperature, wind, 'G'), the later ones after "Q". Only the airports and the LAST observation's place, time and level
 *  are taken — the weather itself is not flight data and is not decoded. */
inline bool labelH2(const std::string& t, Result& r) {
    if (t.size() < 45 || t.compare(0, 3, "02E") != 0 || !allDig(t, 3, 2) || !icaoAt(t, 5) || !icaoAt(t, 9)) return false;
    auto obs = [&](size_t p, Result& o) -> bool {
        double la, lo;
        if (p + 32 > t.size() || degMinTenths(t, p, la, lo) != 13) return false;
        const int s = hms(t, p + 13, 4);
        if (s < 0 || !allDig(t, p + 17, 4) || (t[p + 21] != 'M' && t[p + 21] != 'P') || !allDig(t, p + 22, 3)
            || !allDig(t, p + 25, 6) || t[p + 31] != 'G') return false;
        o.hasPos = true; o.lat = la; o.lon = lo; o.fixSec = s; o.hasAlt = true; o.altFt = num(t, p + 17, 3) * 100;
        return true;
    };
    Result last;
    if (!obs(13, last)) return false;
    size_t p = 45;
    for (;;) {
        while (p < t.size() && t[p] == ' ') ++p;
        if (p >= t.size() || t[p] != 'Q') break;
        Result o;
        if (!obs(p + 1, o)) break;
        last = o; p += 33;
    }
    r.hasPos = true; r.lat = last.lat; r.lon = last.lon; r.fixSec = last.fixSec; r.hasAlt = true; r.altFt = last.altFt;
    r.from = t.substr(5, 4); r.to = t.substr(9, 4);
    r.kind = "weather report position"; r.app = "pos";
    return true;
}

/** ★ The one entry point: a BCS-good block's label and text (downlink text WITHOUT the message number and flight —
 *  vibe_acars.h's Block::text). An empty `kind` = not recognised. */
inline Result parse(const std::string& label, const std::string& text, bool downlink) {
    Result r;
    if (label.size() != 2 || text.empty() || text.size() > 4000) return r;
    const char a = label[0], b = label[1];
    if (a == 'H' && b == '1') {
        const std::string t = text.substr(h1Prefix(text, downlink));
        if (arinc622(t, downlink, r)) return r;
        r = Result();
        if (downlink && arinc702(t, r)) return r;
        return Result();
    }
    if ((a == 'A' || a == 'B') && (b == '6' || b == 'A')) { if (arinc622(text, downlink, r)) return r; return Result(); }
    if (!downlink) return r;          // the airline layouts below are downlinks
    bool ok = false;
    if (a == '1' && b == '6') ok = label16(text, r);
    else if (a == '1' && b == '5') ok = label15(text, r);
    else if (a == '4' && b == '4') ok = label44(text, r);
    else if (a == 'Q' && b >= 'P' && b <= 'S') ok = labelQ(b, text, r);
    else if (a == 'H' && b == '2') ok = labelH2(text, r);
    return ok ? r : Result();
}

/** "21:21" / "21:21:36" / ":53:27" (past the hour) — the fix time as reported, for the list. Empty = none. */
inline std::string fixTimeText(const Result& r) {
    char b[16];
    if (r.fixSec >= 0) { std::snprintf(b, sizeof b, "%02d:%02d:%02d", r.fixSec / 3600, r.fixSec / 60 % 60, r.fixSec % 60); return b; }
    if (r.fixPastHour >= 0) { const int s = r.fixPastHour / 8; std::snprintf(b, sizeof b, ":%02d:%02d", s / 60, s % 60); return b; }
    return "";
}

/** ★ Multi-block downlinks (ETB): the blocks of one message (same registration, label and message number's first three
 *  characters; the fourth is the block's letter) are joined in arrival order until the last (ETX) block. Bounded: 32
 *  messages pending, 8 blocks / 2000 characters each, 60 s to complete. A single block passes straight through. */
class Reassembler {
public:
    /** Returns true with `out` = the whole text when the message is complete. */
    bool add(const vibeacars::Block& blk, double now, std::string& out) {
        if (!blk.downlink || blk.msgNo.size() != 4) { if (blk.more) return false; out = blk.text; return true; }
        const std::string key = blk.reg + '|' + blk.label + '|' + blk.msgNo.substr(0, 3);
        while (!pend_.empty() && now - pend_.front().t0 > 60) pend_.pop_front();
        Pend* p = nullptr;
        for (auto& x : pend_) if (x.key == key) { p = &x; break; }
        if (!p) {
            if (!blk.more) { out = blk.text; return true; }
            if (pend_.size() >= 32) pend_.pop_front();
            pend_.push_back(Pend{ key, now, std::string(), std::string() });
            p = &pend_.back();
        }
        const char seq = blk.msgNo[3];
        if (p->seen.find(seq) == std::string::npos) {     // a repeated block (an ARQ resend) counts once
            p->seen += seq;
            if (p->text.size() + blk.text.size() > 2000 || p->seen.size() > 8) { drop_(key); return false; }
            p->text += blk.text;
        }
        if (blk.more) return false;
        out = p->text;
        drop_(key);
        return true;
    }
    size_t pending() const { return pend_.size(); }
private:
    struct Pend { std::string key; double t0; std::string text; std::string seen; };
    void drop_(const std::string& k) { for (auto it = pend_.begin(); it != pend_.end(); ++it) if (it->key == k) { pend_.erase(it); return; } }
    std::deque<Pend> pend_;
};

}  // namespace vibeacarsapps
