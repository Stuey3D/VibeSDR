// vibe_sat_orbit.h — V12 SATELLITE ENGINE, the orbit core (docs/v12/SATELLITE-ENGINE.md): element sets (TLE and
// CelesTrak OMM JSON), SGP4/SDP4 propagation, the observer's view (azimuth, elevation, range, range rate), Doppler at
// any frequency, and pass prediction (AOS / TCA / LOS, maximum elevation).
//
// ★★★ OUR OWN CODE, FROM THE PUBLISHED MODEL. SGP4/SDP4 is defined by Spacetrack Report #3 (Hoots & Roehrich, 1980) as
//     revised in "Revisiting Spacetrack Report #3" (Vallado, Crawford, Hujsak & Kelso, AIAA 2006-6753). The equations,
//     constants and the order of operations are the paper's — they have to be, because the element sets are FITTED to
//     that exact model and any "improvement" makes every prediction worse. The code is written here from the paper's
//     description; it is validated against the paper's own verification set (SGP4-VER.TLE → tcppver.out, every case:
//     near-Earth, deep space, 12 h / 24 h resonance, Lyddane, decay and the error cases) by vibeserver/test-sat-orbit.cpp.
//     Gravity model WGS-72 and the paper's "improved" operations mode, as the verification output uses.
// ★★ OMM, NOT ONLY TLE. Five-digit catalogue numbers ran out in 2026 (CelesTrak's amateur group already carries
//     101095 ERMINAZ-2V), and TLE cannot hold a six-digit number. The brief's rule: parse OMM JSON straight into SGP4.
//     TLE is kept (Alpha-5 too) because owners paste them and the verification set is TLE.
// ★ TIME: every function takes UTC as a Unix time in seconds (double). The callers pass vibe::vibeUtcNow() — the same
//   corrected clock FT8 uses — never a raw system clock (brief §5 TRAP). UT1 is taken as UTC (|UT1−UTC| < 0.9 s moves a
//   LEO's Doppler by < 2 Hz at 70 cm, far inside the closed loop's trim).
// ★ No heap in the propagator; one Sgp4 is ~1 KB. Header-only, no dependencies beyond <cmath>.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vibe { namespace sat {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kDeg = kPi / 180.0;
constexpr double kMinPerDay = 1440.0;
constexpr double kC = 299792458.0;                 // m/s
constexpr double kJdUnix = 2440587.5;              // JD of 1970-01-01T00:00Z
constexpr double kJd1950 = 2433281.5;              // SGP4's epoch origin (1949 December 31, 0h)

// ── WGS-72 (the model's gravity constants; the element sets are fitted with these) ───────────────────────────────
struct Grav {
    double mu = 398600.8, re = 6378.135, xke = 0, tumin = 0;
    double j2 = 0.001082616, j3 = -0.00000253881, j4 = -0.00000165597, j3oj2 = 0;
    Grav() { xke = 60.0 / std::sqrt(re * re * re / mu); tumin = 1.0 / xke; j3oj2 = j3 / j2; }
};
inline const Grav& wgs72() { static const Grav g; return g; }

// ── Time ──────────────────────────────────────────────────────────────────────────────────────────────────────────
inline double jdFromUnix(double unixSec) { return unixSec / 86400.0 + kJdUnix; }
inline double unixFromJd(double jd) { return (jd - kJdUnix) * 86400.0; }
/** Julian date of a civil UTC date (proleptic Gregorian, valid 1900–2100 as the model needs). */
inline double julianDay(int y, int mon, int d, int h, int mi, double s) {
    return 367.0 * y - std::floor(7 * (y + std::floor((mon + 9) / 12.0)) * 0.25) + std::floor(275 * mon / 9.0) + d
         + 1721013.5 + ((s / 60.0 + mi) / 60.0 + h) / 24.0;
}
/** Greenwich mean sidereal time (IAU 1982), radians — the rotation TEME is defined against. */
inline double gmst(double jdut1) {
    const double t = (jdut1 - 2451545.0) / 36525.0;
    double g = -6.2e-6 * t * t * t + 0.093104 * t * t + (876600.0 * 3600.0 + 8640184.812866) * t + 67310.54841;   // s
    g = std::fmod(g * kDeg / 240.0, kTwoPi);
    if (g < 0) g += kTwoPi;
    return g;
}

// ── Elements ──────────────────────────────────────────────────────────────────────────────────────────────────────
/** One mean element set, in the units SGP4 wants. */
struct Elements {
    long long norad = 0;
    std::string name, intlDes;
    double epochJd = 0;          // UTC Julian date of the epoch (whole + fraction in one double: ~20 µs resolution)
    double bstar = 0;            // 1/earth radii
    double ndot = 0, nddot = 0;  // rad/min², rad/min³ (as the TLE fields, after unit conversion)
    double incl = 0, raan = 0, ecc = 0, argp = 0, ma = 0;   // rad
    double noKozai = 0;          // rad/min
    double epochUnix() const { return unixFromJd(epochJd); }
};

namespace detail {
inline bool fieldNum(const std::string& s, size_t at, size_t len, double& out) {
    if (s.size() < at + len) return false;
    std::string f = s.substr(at, len);
    size_t a = 0, b = f.size();
    while (a < b && f[a] == ' ') ++a;
    while (b > a && f[b - 1] == ' ') --b;
    f = f.substr(a, b - a);
    if (f.empty()) { out = 0; return true; }
    char* end = nullptr;
    out = std::strtod(f.c_str(), &end);
    return end && *end == 0;
}
/** "-11606-4" style assumed-decimal exponent field (BSTAR, n̈). */
inline bool fieldExp(const std::string& s, size_t at, size_t len, double& out) {
    if (s.size() < at + len) return false;
    std::string f = s.substr(at, len);
    std::string t;
    for (char c : f) if (c != ' ') t += c;
    if (t.empty()) { out = 0; return true; }
    double sign = 1;
    if (t[0] == '-' || t[0] == '+') { if (t[0] == '-') sign = -1; t = t.substr(1); }
    const size_t e = t.find_last_of("+-");
    if (e == std::string::npos || e == 0) return false;
    const std::string mant = t.substr(0, e), ex = t.substr(e);
    for (char c : mant) if (!std::isdigit((unsigned char)c)) return false;
    const int expv = std::atoi(ex.c_str());
    out = sign * std::atof(("0." + mant).c_str()) * std::pow(10.0, expv);
    return true;
}
inline int tleChecksum(const std::string& l) {
    int s = 0;
    for (size_t i = 0; i < 68 && i < l.size(); ++i) {
        const char c = l[i];
        if (c >= '0' && c <= '9') s += c - '0';
        else if (c == '-') s += 1;
    }
    return s % 10;
}
/** Catalogue number, five characters: digits, or Alpha-5 (a letter for the leading two digits, I and O skipped). */
inline bool alpha5(const std::string& f, long long& out) {
    std::string t;
    for (char c : f) if (c != ' ') t += c;
    if (t.empty() || t.size() > 5) return false;
    if (std::isalpha((unsigned char)t[0])) {
        const char c = (char)std::toupper((unsigned char)t[0]);
        if (c == 'I' || c == 'O') return false;
        int v = c - 'A' + 10;
        if (c > 'I') --v;
        if (c > 'O') --v;
        for (size_t i = 1; i < t.size(); ++i) if (!std::isdigit((unsigned char)t[i])) return false;
        out = (long long)v * 10000 + std::atoll(t.c_str() + 1);
        return true;
    }
    for (char c : t) if (!std::isdigit((unsigned char)c)) return false;
    out = std::atoll(t.c_str());
    return true;
}
/** Shared by TLE and OMM: TLE units (rev/day, deg, rev/day² …) → SGP4's. */
inline void toSgp4Units(Elements& e, double nRevDay, double ndotHalf, double nddotSixth, double inclDeg, double raanDeg,
                        double argpDeg, double maDeg) {
    const double xpdotp = kMinPerDay / kTwoPi;      // rev/day → rad/min
    e.noKozai = nRevDay / xpdotp;
    e.ndot = ndotHalf / (xpdotp * kMinPerDay);
    e.nddot = nddotSixth / (xpdotp * kMinPerDay * kMinPerDay);
    e.incl = inclDeg * kDeg; e.raan = raanDeg * kDeg; e.argp = argpDeg * kDeg; e.ma = maDeg * kDeg;
}
}  // namespace detail

/** Two-line element set → Elements. `name` is the optional title line. Checks lengths, the line numbers, the
 *  catalogue numbers agreeing, and both checksums (a corrupted digit in a TLE moves a satellite by kilometres).
 *  ★ `checksums` false only for the paper's verification set, whose synthetic error cases carry wrong ones. */
inline bool parseTle(const std::string& line1In, const std::string& line2In, Elements& e, std::string& err,
                     const std::string& name = std::string(), bool checksums = true) {
    auto trimR = [](std::string s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back(); return s; };
    const std::string l1 = trimR(line1In), l2 = trimR(line2In);
    if (l1.size() < 68 || l2.size() < 68) { err = "a TLE line is shorter than 68 characters"; return false; }
    if (l1[0] != '1' || l2[0] != '2') { err = "not a TLE (lines must start 1 and 2)"; return false; }
    for (unsigned char c : l1.substr(0, 69)) if (c >= 0x80) { err = "a TLE line holds a non-ASCII character"; return false; }
    for (unsigned char c : l2.substr(0, 69)) if (c >= 0x80) { err = "a TLE line holds a non-ASCII character"; return false; }
    if (checksums && l1.size() >= 69 && std::isdigit((unsigned char)l1[68]) && detail::tleChecksum(l1) != l1[68] - '0') { err = "line 1 checksum is wrong"; return false; }
    if (checksums && l2.size() >= 69 && std::isdigit((unsigned char)l2[68]) && detail::tleChecksum(l2) != l2[68] - '0') { err = "line 2 checksum is wrong"; return false; }
    long long n1 = 0, n2 = 0;
    if (!detail::alpha5(l1.substr(2, 5), n1) || !detail::alpha5(l2.substr(2, 5), n2)) { err = "the catalogue number is not readable"; return false; }
    if (n1 != n2) { err = "the two lines are for different objects"; return false; }
    e = Elements{};
    e.norad = n1;
    { std::string t = l1.substr(9, 8); while (!t.empty() && t.back() == ' ') t.pop_back(); while (!t.empty() && t[0] == ' ') t.erase(0, 1); e.intlDes = t; }
    e.name = name;
    while (!e.name.empty() && (e.name.back() == ' ' || e.name.back() == '\r')) e.name.pop_back();
    if (e.name.size() > 2 && e.name[0] == '0' && e.name[1] == ' ') e.name = e.name.substr(2);   // "0 NAME" 3LE style
    double yy = 0, days = 0, ndot = 0, nddot = 0, bstar = 0, incl = 0, raan = 0, ecc = 0, argp = 0, ma = 0, n = 0;
    if (!detail::fieldNum(l1, 18, 2, yy) || !detail::fieldNum(l1, 20, 12, days) || !detail::fieldNum(l1, 33, 10, ndot)
        || !detail::fieldExp(l1, 44, 8, nddot) || !detail::fieldExp(l1, 53, 8, bstar)
        || !detail::fieldNum(l2, 8, 8, incl) || !detail::fieldNum(l2, 17, 8, raan) || !detail::fieldNum(l2, 34, 8, argp)
        || !detail::fieldNum(l2, 43, 8, ma) || !detail::fieldNum(l2, 52, 11, n)) { err = "a TLE field is not a number"; return false; }
    { std::string ef = "0." + l2.substr(26, 7); for (char& c : ef) if (c == ' ') c = '0'; ecc = std::atof(ef.c_str()); }
    const int year = (int)yy < 57 ? 2000 + (int)yy : 1900 + (int)yy;
    if (days < 1 || days >= 367 || n <= 0) { err = "the epoch or mean motion is out of range"; return false; }
    e.epochJd = julianDay(year, 1, 1, 0, 0, 0.0) - 1.0 + days;
    e.ecc = ecc; e.bstar = bstar;
    detail::toSgp4Units(e, n, ndot, nddot, incl, raan, argp, ma);
    return true;
}

/** "2026-11-19T12:42:23.431104" (UTC, optional Z) → Julian date. */
inline bool parseIsoUtc(const std::string& s, double& jd) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0; double sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) < 5) {
        if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) < 5) return false;
    }
    if (y < 1957 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec >= 61) return false;
    jd = julianDay(y, mo, d, h, mi, sec);
    return true;
}

// ── A small JSON reader for OMM arrays — flat objects of strings and numbers, bounded ───────────────────────────────
namespace json {
struct Field { std::string key, str; double num = 0; bool isNum = false; };
using Object = std::vector<Field>;
inline const Field* find(const Object& o, const char* k) { for (auto& f : o) if (f.key == k) return &f; return nullptr; }
inline bool num(const Object& o, const char* k, double& v) {
    const Field* f = find(o, k); if (!f) return false;
    if (f->isNum) { v = f->num; return true; }
    char* end = nullptr; v = std::strtod(f->str.c_str(), &end); return end && end != f->str.c_str();
}
inline std::string str(const Object& o, const char* k) { const Field* f = find(o, k); return f ? (f->isNum ? std::string() : f->str) : std::string(); }

/** Parses `[ {..}, {..} ]` (or one object). Nested arrays/objects inside an object are skipped. Returns false on a
 *  malformed document. ★ Bounded: at most `maxObjects` objects kept, strings capped at 256 bytes. */
inline bool parseArray(const std::string& s, std::vector<Object>& out, size_t maxObjects = 4096) {
    size_t i = 0; const size_t n = s.size();
    auto ws = [&] { while (i < n && std::isspace((unsigned char)s[i])) ++i; };
    auto readStr = [&](std::string& v) -> bool {
        if (i >= n || s[i] != '"') return false;
        ++i; v.clear();
        while (i < n && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < n) {
                const char x = s[i++];
                if (x == 'u') { if (i + 4 > n) return false; i += 4; c = '?'; }
                else c = (x == 'n') ? '\n' : (x == 't') ? '\t' : x;
            }
            if (v.size() < 256) v += c;
        }
        if (i >= n) return false;
        ++i; return true;
    };
    std::function<bool()> skipValue;   // forward
    skipValue = [&]() -> bool {
        ws();
        if (i >= n) return false;
        if (s[i] == '"') { std::string t; return readStr(t); }
        if (s[i] == '{' || s[i] == '[') {
            const char open = s[i], close = open == '{' ? '}' : ']';
            ++i; ws();
            if (i < n && s[i] == close) { ++i; return true; }
            for (;;) {
                if (open == '{') { ws(); std::string k; if (!readStr(k)) return false; ws(); if (i >= n || s[i] != ':') return false; ++i; }
                if (!skipValue()) return false;
                ws();
                if (i < n && s[i] == ',') { ++i; continue; }
                if (i < n && s[i] == close) { ++i; return true; }
                return false;
            }
        }
        while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && !std::isspace((unsigned char)s[i])) ++i;
        return true;
    };
    auto readObject = [&](Object& o) -> bool {
        ws(); if (i >= n || s[i] != '{') return false;
        ++i; ws();
        if (i < n && s[i] == '}') { ++i; return true; }
        for (;;) {
            ws(); Field f; if (!readStr(f.key)) return false;
            ws(); if (i >= n || s[i] != ':') return false;
            ++i; ws();
            if (i < n && s[i] == '"') { if (!readStr(f.str)) return false; o.push_back(std::move(f)); }
            else if (i < n && (s[i] == '{' || s[i] == '[')) { if (!skipValue()) return false; }
            else {
                const size_t a = i;
                while (i < n && s[i] != ',' && s[i] != '}' && !std::isspace((unsigned char)s[i])) ++i;
                const std::string lit = s.substr(a, i - a);
                if (lit != "null" && lit != "true" && lit != "false") {
                    char* end = nullptr; f.num = std::strtod(lit.c_str(), &end);
                    if (!end || *end) return false;
                    f.isNum = true; o.push_back(std::move(f));
                }
            }
            ws();
            if (i < n && s[i] == ',') { ++i; continue; }
            if (i < n && s[i] == '}') { ++i; return true; }
            return false;
        }
    };
    out.clear();
    ws();
    if (i < n && s[i] == '{') { Object o; if (!readObject(o)) return false; out.push_back(std::move(o)); return true; }
    if (i >= n || s[i] != '[') return false;
    ++i; ws();
    if (i < n && s[i] == ']') return true;
    for (;;) {
        Object o; if (!readObject(o)) return false;
        if (out.size() < maxObjects) out.push_back(std::move(o));
        ws();
        if (i < n && s[i] == ',') { ++i; continue; }
        if (i < n && s[i] == ']') return true;
        return false;
    }
}
}  // namespace json

/** One CelesTrak OMM object (FORMAT=json) → Elements. Units as CCSDS OMM / TLE-derived: MEAN_MOTION rev/day,
 *  MEAN_MOTION_DOT rev/day² (the TLE's ṅ/2), MEAN_MOTION_DDOT rev/day³ (n̈/6), angles in degrees. */
inline bool fromOmm(const json::Object& o, Elements& e, std::string& err) {
    double norad = 0, n = 0, ecc = 0, incl = 0, raan = 0, argp = 0, ma = 0, bstar = 0, ndot = 0, nddot = 0, jd = 0;
    if (!json::num(o, "NORAD_CAT_ID", norad) || norad <= 0) { err = "no NORAD_CAT_ID"; return false; }
    if (!json::num(o, "MEAN_MOTION", n) || !json::num(o, "ECCENTRICITY", ecc) || !json::num(o, "INCLINATION", incl)
        || !json::num(o, "RA_OF_ASC_NODE", raan) || !json::num(o, "ARG_OF_PERICENTER", argp) || !json::num(o, "MEAN_ANOMALY", ma)) {
        err = "an orbital element is missing"; return false;
    }
    json::num(o, "BSTAR", bstar); json::num(o, "MEAN_MOTION_DOT", ndot); json::num(o, "MEAN_MOTION_DDOT", nddot);
    if (!parseIsoUtc(json::str(o, "EPOCH"), jd)) { err = "the EPOCH is not a UTC date"; return false; }
    if (n <= 0 || n > 20 || ecc < 0 || ecc >= 1 || !std::isfinite(bstar)) { err = "an element is out of range"; return false; }
    double et = 0;
    if (json::num(o, "EPHEMERIS_TYPE", et) && et != 0 && et != 2) { err = "not an SGP4 element set (EPHEMERIS_TYPE)"; return false; }
    e = Elements{};
    e.norad = (long long)norad;
    e.name = json::str(o, "OBJECT_NAME");
    e.intlDes = json::str(o, "OBJECT_ID");
    e.epochJd = jd; e.ecc = ecc; e.bstar = bstar;
    detail::toSgp4Units(e, n, ndot, nddot, incl, raan, argp, ma);
    return true;
}

// ── SGP4 / SDP4 ───────────────────────────────────────────────────────────────────────────────────────────────────
/** Error codes as the paper numbers them: 1 mean eccentricity out of range, 2 mean motion ≤ 0, 3 perturbed eccentricity
 *  out of range, 4 semi-latus rectum < 0, 6 the satellite has decayed (radius < 1 earth radius). */
class Sgp4 {
public:
    Sgp4() = default;
    explicit Sgp4(const Elements& el) { init(el); }

    /** Sets up the model for these elements. Returns false (and error()) when the elements cannot be propagated. */
    bool init(const Elements& el) {
        *this = Sgp4{};
        el_ = el;
        const Grav& g = wgs72();
        const double x2o3 = 2.0 / 3.0, temp4 = 1.5e-12;
        bstar = el.bstar; ecco = el.ecc; argpo = el.argp; inclo = el.incl; mo = el.ma; nodeo = el.raan;
        const double epoch = el.epochJd - kJd1950;
        const double ss = 78.0 / g.re + 1.0;
        const double qzms2t = std::pow((120.0 - 78.0) / g.re, 4);
        // initl
        const double eccsq = ecco * ecco, omeosq = 1.0 - eccsq, rteosq = std::sqrt(omeosq);
        const double cosio = std::cos(inclo), cosio2 = cosio * cosio;
        {
            const double ak = std::pow(g.xke / el.noKozai, x2o3);
            const double d1 = 0.75 * g.j2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
            double del = d1 / (ak * ak);
            const double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
            del = d1 / (adel * adel);
            no = el.noKozai / (1.0 + del);
        }
        const double ao = std::pow(g.xke / no, x2o3);
        const double sinio = std::sin(inclo);
        const double po = ao * omeosq;
        const double con42 = 1.0 - 5.0 * cosio2;
        con41 = -con42 - cosio2 - cosio2;
        const double posq = po * po, rp = ao * (1.0 - ecco);
        gsto = gmst(epoch + kJd1950);
        error_ = 0;
        if (omeosq >= 0.0 || no >= 0.0) {
            isimp = (rp < (220.0 / g.re + 1.0));
            double sfour = ss, qzms24 = qzms2t;
            const double perige = (rp - 1.0) * g.re;
            if (perige < 156.0) {
                sfour = perige - 78.0;
                if (perige < 98.0) sfour = 20.0;
                qzms24 = std::pow((120.0 - sfour) / g.re, 4);
                sfour = sfour / g.re + 1.0;
            }
            const double pinvsq = 1.0 / posq;
            const double tsi = 1.0 / (ao - sfour);
            eta = ao * ecco * tsi;
            const double etasq = eta * eta, eeta = ecco * eta, psisq = std::fabs(1.0 - etasq);
            const double coef = qzms24 * std::pow(tsi, 4), coef1 = coef / std::pow(psisq, 3.5);
            const double cc2 = coef1 * no * (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq))
                             + 0.375 * g.j2 * tsi / psisq * con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
            cc1 = bstar * cc2;
            double cc3 = 0.0;
            if (ecco > 1.0e-4) cc3 = -2.0 * coef * tsi * g.j3oj2 * no * sinio / ecco;
            x1mth2 = 1.0 - cosio2;
            cc4 = 2.0 * no * coef1 * ao * omeosq * (eta * (2.0 + 0.5 * etasq) + ecco * (0.5 + 2.0 * etasq)
                  - g.j2 * tsi / (ao * psisq) * (-3.0 * con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta))
                  + 0.75 * x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * std::cos(2.0 * argpo)));
            cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
            const double cosio4 = cosio2 * cosio2;
            const double temp1 = 1.5 * g.j2 * pinvsq * no;
            const double temp2 = 0.5 * temp1 * g.j2 * pinvsq;
            const double temp3 = -0.46875 * g.j4 * pinvsq * pinvsq * no;
            mdot = no + 0.5 * temp1 * rteosq * con41 + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
            argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4)
                    + temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
            const double xhdot1 = -temp1 * cosio;
            nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
            const double xpidot = argpdot + nodedot;
            omgcof = bstar * cc3 * std::cos(argpo);
            xmcof = 0.0;
            if (ecco > 1.0e-4) xmcof = -x2o3 * coef * bstar / eeta;
            nodecf = 3.5 * omeosq * xhdot1 * cc1;
            t2cof = 1.5 * cc1;
            if (std::fabs(cosio + 1.0) > 1.5e-12) xlcof = -0.25 * g.j3oj2 * sinio * (3.0 + 5.0 * cosio) / (1.0 + cosio);
            else                                    xlcof = -0.25 * g.j3oj2 * sinio * (3.0 + 5.0 * cosio) / temp4;
            aycof = -0.5 * g.j3oj2 * sinio;
            delmo = std::pow(1.0 + eta * std::cos(mo), 3);
            sinmao = std::sin(mo);
            x7thm1 = 7.0 * cosio2 - 1.0;

            // ★ Deep space: a period of 225 minutes or more (SDP4).
            if (kTwoPi / no >= 225.0) {
                deep = true; isimp = true;
                const double tc = 0.0;
                double inclm = inclo;
                DsCom c; dscom(epoch, ecco, argpo, tc, inclo, nodeo, no, c);
                // (dpper at init changes nothing — the periodics are measured from epoch)
                double argpm = 0.0, nodem = 0.0, mm = 0.0, em = c.em, nm = c.nm;
                dsinit(c, cosio /*unused*/, tc, xpidot, eccsq, em, argpm, inclm, mm, nm, nodem);
                (void)em; (void)argpm; (void)inclm; (void)mm; (void)nm; (void)nodem;
            }
            if (!isimp) {
                const double cc1sq = cc1 * cc1;
                d2 = 4.0 * ao * tsi * cc1sq;
                const double temp = d2 * tsi * cc1 / 3.0;
                d3 = (17.0 * ao + sfour) * temp;
                d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * cc1;
                t3cof = d2 + 2.0 * cc1sq;
                t4cof = 0.25 * (3.0 * d3 + cc1 * (12.0 * d2 + 10.0 * cc1sq));
                t5cof = 0.2 * (3.0 * d4 + 12.0 * cc1 * d3 + 6.0 * d2 * d2 + 15.0 * cc1sq * (2.0 * d2 + cc1sq));
            }
        }
        double r[3], v[3];
        ok_ = true;
        propagate(0.0, r, v);
        ok_ = (error_ == 0);
        return ok_;
    }

    /** Position (km) and velocity (km/s) in TEME, `tsince` minutes from the epoch. Returns the error code (0 = good).
     *  ★ Error 6 (decayed) still fills r/v, as the paper does; callers treat any non-zero as "no position". */
    int propagate(double tsince, double r[3], double v[3]) {
        const Grav& g = wgs72();
        const double x2o3 = 2.0 / 3.0, temp4 = 1.5e-12;
        const double vkmpersec = g.re * g.xke / 60.0;
        error_ = 0;
        const double t = tsince;
        const double xmdf = mo + mdot * t;
        const double argpdf = argpo + argpdot * t;
        const double nodedf = nodeo + nodedot * t;
        double argpm = argpdf, mm = xmdf;
        const double t2 = t * t;
        double nodem = nodedf + nodecf * t2;
        double tempa = 1.0 - cc1 * t, tempe = bstar * cc4 * t, templ = t2cof * t2;
        if (!isimp) {
            const double delomg = omgcof * t;
            const double delmtemp = 1.0 + eta * std::cos(xmdf);
            const double delm = xmcof * (delmtemp * delmtemp * delmtemp - delmo);
            const double temp = delomg + delm;
            mm = xmdf + temp;
            argpm = argpdf - temp;
            const double t3 = t2 * t, t4 = t3 * t;
            tempa = tempa - d2 * t2 - d3 * t3 - d4 * t4;
            tempe = tempe + bstar * cc5 * (std::sin(mm) - sinmao);
            templ = templ + t3cof * t3 + t4 * (t4cof + t * t5cof);
        }
        double nm = no, em = ecco, inclm = inclo;
        if (deep) dspace(t, em, argpm, inclm, mm, nodem, nm);
        if (nm <= 0.0) { error_ = 2; return error_; }
        const double am = std::pow(g.xke / nm, x2o3) * tempa * tempa;
        nm = g.xke / std::pow(am, 1.5);
        em = em - tempe;
        if (em >= 1.0 || em < -0.001) { error_ = 1; return error_; }
        if (em < 1.0e-6) em = 1.0e-6;
        mm = mm + no * templ;
        double xlm = mm + argpm + nodem;
        nodem = std::fmod(nodem, kTwoPi);
        argpm = std::fmod(argpm, kTwoPi);
        xlm = std::fmod(xlm, kTwoPi);
        mm = std::fmod(xlm - argpm - nodem, kTwoPi);
        double sinim = std::sin(inclm), cosim = std::cos(inclm);
        double ep = em, xincp = inclm, argpp = argpm, nodep = nodem, mp = mm;
        double sinip = sinim, cosip = cosim;
        double lxlcof = xlcof, laycof = aycof, lcon41 = con41, lx1mth2 = x1mth2, lx7thm1 = x7thm1;
        if (deep) {
            dpper(t, false, ep, xincp, nodep, argpp, mp);
            if (xincp < 0.0) { xincp = -xincp; nodep += kPi; argpp -= kPi; }
            if (ep < 0.0 || ep > 1.0) { error_ = 3; return error_; }
            sinip = std::sin(xincp); cosip = std::cos(xincp);
            laycof = -0.5 * g.j3oj2 * sinip;
            if (std::fabs(cosip + 1.0) > 1.5e-12) lxlcof = -0.25 * g.j3oj2 * sinip * (3.0 + 5.0 * cosip) / (1.0 + cosip);
            else                                    lxlcof = -0.25 * g.j3oj2 * sinip * (3.0 + 5.0 * cosip) / temp4;
        }
        const double axnl = ep * std::cos(argpp);
        double temp = 1.0 / (am * (1.0 - ep * ep));
        const double aynl = ep * std::sin(argpp) + temp * laycof;
        const double xl = mp + argpp + nodep + temp * lxlcof * axnl;
        // Kepler's equation, the paper's iteration (≤ 10 steps, step capped at 0.95)
        const double u = std::fmod(xl - nodep, kTwoPi);
        double eo1 = u, tem5 = 9999.9, sineo1 = 0, coseo1 = 0;
        int ktr = 1;
        while (std::fabs(tem5) >= 1.0e-12 && ktr <= 10) {
            sineo1 = std::sin(eo1); coseo1 = std::cos(eo1);
            tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
            tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
            if (std::fabs(tem5) >= 0.95) tem5 = tem5 > 0.0 ? 0.95 : -0.95;
            eo1 += tem5;
            ++ktr;
        }
        const double ecose = axnl * coseo1 + aynl * sineo1;
        const double esine = axnl * sineo1 - aynl * coseo1;
        const double el2 = axnl * axnl + aynl * aynl;
        const double pl = am * (1.0 - el2);
        if (pl < 0.0) { error_ = 4; return error_; }
        const double rl = am * (1.0 - ecose);
        const double rdotl = std::sqrt(am) * esine / rl;
        const double rvdotl = std::sqrt(pl) / rl;
        const double betal = std::sqrt(1.0 - el2);
        temp = esine / (1.0 + betal);
        const double sinu = am / rl * (sineo1 - aynl - axnl * temp);
        const double cosu = am / rl * (coseo1 - axnl + aynl * temp);
        double su = std::atan2(sinu, cosu);
        const double sin2u = (cosu + cosu) * sinu;
        const double cos2u = 1.0 - 2.0 * sinu * sinu;
        temp = 1.0 / pl;
        const double temp1 = 0.5 * g.j2 * temp;
        const double temp2 = temp1 * temp;
        if (deep) {
            const double cosisq = cosip * cosip;
            lcon41 = 3.0 * cosisq - 1.0;
            lx1mth2 = 1.0 - cosisq;
            lx7thm1 = 7.0 * cosisq - 1.0;
        }
        const double mrt = rl * (1.0 - 1.5 * temp2 * betal * lcon41) + 0.5 * temp1 * lx1mth2 * cos2u;
        su = su - 0.25 * temp2 * lx7thm1 * sin2u;
        const double xnode = nodep + 1.5 * temp2 * cosip * sin2u;
        const double xinc = xincp + 1.5 * temp2 * cosip * sinip * cos2u;
        const double mvt = rdotl - nm * temp1 * lx1mth2 * sin2u / g.xke;
        const double rvdot = rvdotl + nm * temp1 * (lx1mth2 * cos2u + 1.5 * lcon41) / g.xke;
        const double sinsu = std::sin(su), cossu = std::cos(su), snod = std::sin(xnode), cnod = std::cos(xnode);
        const double sini = std::sin(xinc), cosi = std::cos(xinc);
        const double xmx = -snod * cosi, xmy = cnod * cosi;
        const double ux = xmx * sinsu + cnod * cossu, uy = xmy * sinsu + snod * cossu, uz = sini * sinsu;
        const double vx = xmx * cossu - cnod * sinsu, vy = xmy * cossu - snod * sinsu, vz = sini * cossu;
        const double mr = mrt * g.re;
        r[0] = mr * ux; r[1] = mr * uy; r[2] = mr * uz;
        v[0] = (mvt * ux + rvdot * vx) * vkmpersec;
        v[1] = (mvt * uy + rvdot * vy) * vkmpersec;
        v[2] = (mvt * uz + rvdot * vz) * vkmpersec;
        if (mrt < 1.0) error_ = 6;
        return error_;
    }

    /** The same at a UTC Unix time. */
    int propagateUnix(double unixSec, double r[3], double v[3]) {
        return propagate((jdFromUnix(unixSec) - el_.epochJd) * kMinPerDay, r, v);
    }

    bool ok() const { return ok_; }
    int error() const { return error_; }
    bool deepSpace() const { return deep; }
    const Elements& elements() const { return el_; }
    /** Orbital period, minutes (from the un-Kozai'd mean motion). */
    double periodMin() const { return no > 0 ? kTwoPi / no : 0; }

private:
    struct DsCom {
        double snodm, cnodm, sinim, cosim, sinomm, cosomm, day, e3, ee2, em, emsq, gam, rtemsq;
        double se2, se3, sgh2, sgh3, sgh4, sh2, sh3, si2, si3, sl2, sl3, sl4;
        double s1, s2, s3, s4, s5, s6, s7, ss1, ss2, ss3, ss4, ss5, ss6, ss7;
        double sz1, sz2, sz3, sz11, sz12, sz13, sz21, sz22, sz23, sz31, sz32, sz33;
        double xgh2, xgh3, xgh4, xh2, xh3, xi2, xi3, xl2, xl3, xl4, nm;
        double z1, z2, z3, z11, z12, z13, z21, z22, z23, z31, z32, z33, zmol, zmos;
    };

    /** Deep-space common terms: the lunar and solar perturbation coefficients at epoch. */
    static void dscom(double epoch, double ep, double argpp, double tc, double inclp, double nodep, double np, DsCom& o) {
        const double zes = 0.01675, zel = 0.05490, c1ss = 2.9864797e-6, c1l = 4.7968065e-7;
        const double zsinis = 0.39785416, zcosis = 0.91744867, zcosgs = 0.1945905, zsings = -0.98088458;
        o.nm = np; o.em = ep;
        o.snodm = std::sin(nodep); o.cnodm = std::cos(nodep);
        o.sinomm = std::sin(argpp); o.cosomm = std::cos(argpp);
        o.sinim = std::sin(inclp); o.cosim = std::cos(inclp);
        o.emsq = o.em * o.em;
        const double betasq = 1.0 - o.emsq;
        o.rtemsq = std::sqrt(betasq);
        o.day = epoch + 18261.5 + tc / 1440.0;
        const double xnodce = std::fmod(4.5236020 - 9.2422029e-4 * o.day, kTwoPi);
        const double stem = std::sin(xnodce), ctem = std::cos(xnodce);
        const double zcosil = 0.91375164 - 0.03568096 * ctem;
        const double zsinil = std::sqrt(1.0 - zcosil * zcosil);
        const double zsinhl = 0.089683511 * stem / zsinil;
        const double zcoshl = std::sqrt(1.0 - zsinhl * zsinhl);
        o.gam = 5.8351514 + 0.0019443680 * o.day;
        double zx = 0.39785416 * stem / zsinil;
        const double zy = zcoshl * ctem + 0.91744867 * zsinhl * stem;
        zx = std::atan2(zx, zy);
        zx = o.gam + zx - xnodce;
        const double zcosgl = std::cos(zx), zsingl = std::sin(zx);
        double zcosg = zcosgs, zsing = zsings, zcosi = zcosis, zsini = zsinis, zcosh = o.cnodm, zsinh = o.snodm, cc = c1ss;
        const double xnoi = 1.0 / o.nm;
        for (int lsflg = 1; lsflg <= 2; ++lsflg) {
            const double a1 = zcosg * zcosh + zsing * zcosi * zsinh;
            const double a3 = -zsing * zcosh + zcosg * zcosi * zsinh;
            const double a7 = -zcosg * zsinh + zsing * zcosi * zcosh;
            const double a8 = zsing * zsini;
            const double a9 = zsing * zsinh + zcosg * zcosi * zcosh;
            const double a10 = zcosg * zsini;
            const double a2 = o.cosim * a7 + o.sinim * a8;
            const double a4 = o.cosim * a9 + o.sinim * a10;
            const double a5 = -o.sinim * a7 + o.cosim * a8;
            const double a6 = -o.sinim * a9 + o.cosim * a10;
            const double x1 = a1 * o.cosomm + a2 * o.sinomm;
            const double x2 = a3 * o.cosomm + a4 * o.sinomm;
            const double x3 = -a1 * o.sinomm + a2 * o.cosomm;
            const double x4 = -a3 * o.sinomm + a4 * o.cosomm;
            const double x5 = a5 * o.sinomm, x6 = a6 * o.sinomm, x7 = a5 * o.cosomm, x8 = a6 * o.cosomm;
            const double z31 = 12.0 * x1 * x1 - 3.0 * x3 * x3;
            const double z32 = 24.0 * x1 * x2 - 6.0 * x3 * x4;
            const double z33 = 12.0 * x2 * x2 - 3.0 * x4 * x4;
            double z1 = 3.0 * (a1 * a1 + a2 * a2) + z31 * o.emsq;
            double z2 = 6.0 * (a1 * a3 + a2 * a4) + z32 * o.emsq;
            double z3 = 3.0 * (a3 * a3 + a4 * a4) + z33 * o.emsq;
            const double z11 = -6.0 * a1 * a5 + o.emsq * (-24.0 * x1 * x7 - 6.0 * x3 * x5);
            const double z12 = -6.0 * (a1 * a6 + a3 * a5) + o.emsq * (-24.0 * (x2 * x7 + x1 * x8) - 6.0 * (x3 * x6 + x4 * x5));
            const double z13 = -6.0 * a3 * a6 + o.emsq * (-24.0 * x2 * x8 - 6.0 * x4 * x6);
            const double z21 = 6.0 * a2 * a5 + o.emsq * (24.0 * x1 * x5 - 6.0 * x3 * x7);
            const double z22 = 6.0 * (a4 * a5 + a2 * a6) + o.emsq * (24.0 * (x2 * x5 + x1 * x6) - 6.0 * (x4 * x7 + x3 * x8));
            const double z23 = 6.0 * a4 * a6 + o.emsq * (24.0 * x2 * x6 - 6.0 * x4 * x8);
            z1 = z1 + z1 + betasq * z31;
            z2 = z2 + z2 + betasq * z32;
            z3 = z3 + z3 + betasq * z33;
            const double s3 = cc * xnoi;
            const double s2 = -0.5 * s3 / o.rtemsq;
            const double s4 = s3 * o.rtemsq;
            const double s1 = -15.0 * o.em * s4;
            const double s5 = x1 * x3 + x2 * x4;
            const double s6 = x2 * x3 + x1 * x4;
            const double s7 = x2 * x4 - x1 * x3;
            if (lsflg == 1) {
                o.ss1 = s1; o.ss2 = s2; o.ss3 = s3; o.ss4 = s4; o.ss5 = s5; o.ss6 = s6; o.ss7 = s7;
                o.sz1 = z1; o.sz2 = z2; o.sz3 = z3; o.sz11 = z11; o.sz12 = z12; o.sz13 = z13;
                o.sz21 = z21; o.sz22 = z22; o.sz23 = z23; o.sz31 = z31; o.sz32 = z32; o.sz33 = z33;
                zcosg = zcosgl; zsing = zsingl; zcosi = zcosil; zsini = zsinil;
                zcosh = zcoshl * o.cnodm + zsinhl * o.snodm;
                zsinh = o.snodm * zcoshl - o.cnodm * zsinhl;
                cc = c1l;
            } else {
                o.s1 = s1; o.s2 = s2; o.s3 = s3; o.s4 = s4; o.s5 = s5; o.s6 = s6; o.s7 = s7;
                o.z1 = z1; o.z2 = z2; o.z3 = z3; o.z11 = z11; o.z12 = z12; o.z13 = z13;
                o.z21 = z21; o.z22 = z22; o.z23 = z23; o.z31 = z31; o.z32 = z32; o.z33 = z33;
            }
        }
        o.zmol = std::fmod(4.7199672 + 0.22997150 * o.day - o.gam, kTwoPi);
        o.zmos = std::fmod(6.2565837 + 0.017201977 * o.day, kTwoPi);
        // solar terms
        o.se2 = 2.0 * o.ss1 * o.ss6;
        o.se3 = 2.0 * o.ss1 * o.ss7;
        o.si2 = 2.0 * o.ss2 * o.sz12;
        o.si3 = 2.0 * o.ss2 * (o.sz13 - o.sz11);
        o.sl2 = -2.0 * o.ss3 * o.sz2;
        o.sl3 = -2.0 * o.ss3 * (o.sz3 - o.sz1);
        o.sl4 = -2.0 * o.ss3 * (-21.0 - 9.0 * o.emsq) * zes;
        o.sgh2 = 2.0 * o.ss4 * o.sz32;
        o.sgh3 = 2.0 * o.ss4 * (o.sz33 - o.sz31);
        o.sgh4 = -18.0 * o.ss4 * zes;
        o.sh2 = -2.0 * o.ss2 * o.sz22;
        o.sh3 = -2.0 * o.ss2 * (o.sz23 - o.sz21);
        // lunar terms
        o.ee2 = 2.0 * o.s1 * o.s6;
        o.e3 = 2.0 * o.s1 * o.s7;
        o.xi2 = 2.0 * o.s2 * o.z12;
        o.xi3 = 2.0 * o.s2 * (o.z13 - o.z11);
        o.xl2 = -2.0 * o.s3 * o.z2;
        o.xl3 = -2.0 * o.s3 * (o.z3 - o.z1);
        o.xl4 = -2.0 * o.s3 * (-21.0 - 9.0 * o.emsq) * zel;
        o.xgh2 = 2.0 * o.s4 * o.z32;
        o.xgh3 = 2.0 * o.s4 * (o.z33 - o.z31);
        o.xgh4 = -18.0 * o.s4 * zel;
        o.xh2 = -2.0 * o.s2 * o.z22;
        o.xh3 = -2.0 * o.s2 * (o.z23 - o.z21);
    }

    /** Deep-space set-up: secular rates and the resonance terms (12 h and 24 h orbits). */
    void dsinit(const DsCom& c, double /*cosio*/, double tc, double xpidot, double eccsq, double& em, double& argpm,
                double& inclm, double& mm, double& nm, double& nodem) {
        // keep the periodic coefficients for dpper
        e3 = c.e3; ee2 = c.ee2; se2 = c.se2; se3 = c.se3; sgh2 = c.sgh2; sgh3 = c.sgh3; sgh4 = c.sgh4;
        sh2 = c.sh2; sh3 = c.sh3; si2 = c.si2; si3 = c.si3; sl2 = c.sl2; sl3 = c.sl3; sl4 = c.sl4;
        xgh2 = c.xgh2; xgh3 = c.xgh3; xgh4 = c.xgh4; xh2 = c.xh2; xh3 = c.xh3; xi2 = c.xi2; xi3 = c.xi3;
        xl2 = c.xl2; xl3 = c.xl3; xl4 = c.xl4; zmol = c.zmol; zmos = c.zmos;
        const Grav& g = wgs72();
        const double q22 = 1.7891679e-6, q31 = 2.1460748e-6, q33 = 2.2123015e-7;
        const double root22 = 1.7891679e-6, root44 = 7.3636953e-9, root54 = 2.1765803e-9;
        const double rptim = 4.37526908801129966e-3, root32 = 3.7393792e-7, root52 = 1.1428639e-7;
        const double x2o3 = 2.0 / 3.0, znl = 1.5835218e-4, zns = 1.19459e-5;
        const double cosim = c.cosim, sinim = c.sinim;
        double emsq = c.emsq;
        em = c.em; nm = c.nm;
        irez = 0;
        if (nm < 0.0052359877 && nm > 0.0034906585) irez = 1;
        if (nm >= 8.26e-3 && nm <= 9.24e-3 && em >= 0.5) irez = 2;
        // solar
        const double ses = c.ss1 * zns * c.ss5;
        const double sis = c.ss2 * zns * (c.sz11 + c.sz13);
        const double sls = -zns * c.ss3 * (c.sz1 + c.sz3 - 14.0 - 6.0 * emsq);
        const double sghs = c.ss4 * zns * (c.sz31 + c.sz33 - 6.0);
        double shs = -zns * c.ss2 * (c.sz21 + c.sz23);
        if (inclm < 5.2359877e-2 || inclm > kPi - 5.2359877e-2) shs = 0.0;
        if (sinim != 0.0) shs = shs / sinim;
        const double sgs = sghs - cosim * shs;
        // lunar
        dedt = ses + c.s1 * znl * c.s5;
        didt = sis + c.s2 * znl * (c.z11 + c.z13);
        dmdt = sls - znl * c.s3 * (c.z1 + c.z3 - 14.0 - 6.0 * emsq);
        const double sghl = c.s4 * znl * (c.z31 + c.z33 - 6.0);
        double shll = -znl * c.s2 * (c.z21 + c.z23);
        if (inclm < 5.2359877e-2 || inclm > kPi - 5.2359877e-2) shll = 0.0;
        domdt = sgs + sghl;
        dnodt = shs;
        if (sinim != 0.0) { domdt = domdt - cosim / sinim * shll; dnodt = dnodt + shll / sinim; }
        // resonance
        double dndt = 0.0;
        const double theta = std::fmod(gsto + tc * rptim, kTwoPi);
        const double t = 0.0;
        em = em + dedt * t; inclm = inclm + didt * t; argpm = argpm + domdt * t; nodem = nodem + dnodt * t; mm = mm + dmdt * t;
        if (irez != 0) {
            const double aonv = std::pow(nm / g.xke, x2o3);
            if (irez == 2) {
                const double cosisq = cosim * cosim;
                const double emo = em; em = ecco;
                const double emsqo = emsq; emsq = eccsq;
                const double eoc = em * emsq;
                const double g201 = -0.306 - (em - 0.64) * 0.440;
                double g211, g310, g322, g410, g422, g520, g521, g532, g533;
                if (em <= 0.65) {
                    g211 = 3.616 - 13.2470 * em + 16.2900 * emsq;
                    g310 = -19.302 + 117.3900 * em - 228.4190 * emsq + 156.5910 * eoc;
                    g322 = -18.9068 + 109.7927 * em - 214.6334 * emsq + 146.5816 * eoc;
                    g410 = -41.122 + 242.6940 * em - 471.0940 * emsq + 313.9530 * eoc;
                    g422 = -146.407 + 841.8800 * em - 1629.014 * emsq + 1083.4350 * eoc;
                    g520 = -532.114 + 3017.977 * em - 5740.032 * emsq + 3708.2760 * eoc;
                } else {
                    g211 = -72.099 + 331.819 * em - 508.738 * emsq + 266.724 * eoc;
                    g310 = -346.844 + 1582.851 * em - 2415.925 * emsq + 1246.113 * eoc;
                    g322 = -342.585 + 1554.908 * em - 2366.899 * emsq + 1215.972 * eoc;
                    g410 = -1052.797 + 4758.686 * em - 7193.992 * emsq + 3651.957 * eoc;
                    g422 = -3581.690 + 16178.110 * em - 24462.770 * emsq + 12422.520 * eoc;
                    if (em > 0.715) g520 = -5149.66 + 29936.92 * em - 54087.36 * emsq + 31324.56 * eoc;
                    else            g520 = 1464.74 - 4664.75 * em + 3763.64 * emsq;
                }
                if (em < 0.7) {
                    g533 = -919.22770 + 4988.6100 * em - 9064.7700 * emsq + 5542.21 * eoc;
                    g521 = -822.71072 + 4568.6173 * em - 8491.4146 * emsq + 5337.524 * eoc;
                    g532 = -853.66600 + 4690.2500 * em - 8624.7700 * emsq + 5341.4 * eoc;
                } else {
                    g533 = -37995.780 + 161616.52 * em - 229838.20 * emsq + 109377.94 * eoc;
                    g521 = -51752.104 + 218913.95 * em - 309468.16 * emsq + 146349.42 * eoc;
                    g532 = -40023.880 + 170470.89 * em - 242699.48 * emsq + 115605.82 * eoc;
                }
                const double sini2 = sinim * sinim;
                const double f220 = 0.75 * (1.0 + 2.0 * cosim + cosisq);
                const double f221 = 1.5 * sini2;
                const double f321 = 1.875 * sinim * (1.0 - 2.0 * cosim - 3.0 * cosisq);
                const double f322 = -1.875 * sinim * (1.0 + 2.0 * cosim - 3.0 * cosisq);
                const double f441 = 35.0 * sini2 * f220;
                const double f442 = 39.3750 * sini2 * sini2;
                const double f522 = 9.84375 * sinim * (sini2 * (1.0 - 2.0 * cosim - 5.0 * cosisq) + 0.33333333 * (-2.0 + 4.0 * cosim + 6.0 * cosisq));
                const double f523 = sinim * (4.92187512 * sini2 * (-2.0 - 4.0 * cosim + 10.0 * cosisq) + 6.56250012 * (1.0 + 2.0 * cosim - 3.0 * cosisq));
                const double f542 = 29.53125 * sinim * (2.0 - 8.0 * cosim + cosisq * (-12.0 + 8.0 * cosim + 10.0 * cosisq));
                const double f543 = 29.53125 * sinim * (-2.0 - 8.0 * cosim + cosisq * (12.0 + 8.0 * cosim - 10.0 * cosisq));
                const double xno2 = nm * nm, ainv2 = aonv * aonv;
                double temp1 = 3.0 * xno2 * ainv2;
                double temp = temp1 * root22;
                d2201 = temp * f220 * g201;
                d2211 = temp * f221 * g211;
                temp1 = temp1 * aonv;
                temp = temp1 * root32;
                d3210 = temp * f321 * g310;
                d3222 = temp * f322 * g322;
                temp1 = temp1 * aonv;
                temp = 2.0 * temp1 * root44;
                d4410 = temp * f441 * g410;
                d4422 = temp * f442 * g422;
                temp1 = temp1 * aonv;
                temp = temp1 * root52;
                d5220 = temp * f522 * g520;
                d5232 = temp * f523 * g532;
                temp = 2.0 * temp1 * root54;
                d5421 = temp * f542 * g521;
                d5433 = temp * f543 * g533;
                xlamo = std::fmod(mo + nodeo + nodeo - theta - theta, kTwoPi);
                xfact = mdot + dmdt + 2.0 * (nodedot + dnodt - rptim) - no;
                em = emo; emsq = emsqo;
            }
            if (irez == 1) {
                const double g200 = 1.0 + emsq * (-2.5 + 0.8125 * emsq);
                const double g310 = 1.0 + 2.0 * emsq;
                const double g300 = 1.0 + emsq * (-6.0 + 6.60937 * emsq);
                const double f220 = 0.75 * (1.0 + cosim) * (1.0 + cosim);
                const double f311 = 0.9375 * sinim * sinim * (1.0 + 3.0 * cosim) - 0.75 * (1.0 + cosim);
                double f330 = 1.0 + cosim;
                f330 = 1.875 * f330 * f330 * f330;
                del1 = 3.0 * nm * nm * aonv * aonv;
                del2 = 2.0 * del1 * f220 * g200 * q22;
                del3 = 3.0 * del1 * f330 * g300 * q33 * aonv;
                del1 = del1 * f311 * g310 * q31 * aonv;
                xlamo = std::fmod(mo + nodeo + argpo - theta, kTwoPi);
                xfact = mdot + xpidot - rptim + dmdt + domdt + dnodt - no;
            }
            xli = xlamo;
            xni = no;
            atime = 0.0;
            nm = no + dndt;
        }
    }

    /** Deep-space secular effects and the resonance integration (fixed 720-minute steps from epoch). */
    void dspace(double t, double& em, double& argpm, double& inclm, double& mm, double& nodem, double& nm) {
        const double fasx2 = 0.13130908, fasx4 = 2.8843198, fasx6 = 0.37448087;
        const double g22 = 5.7686396, g32 = 0.95240898, g44 = 1.8014998, g52 = 1.0508330, g54 = 4.4108898;
        const double rptim = 4.37526908801129966e-3, stepp = 720.0, stepn = -720.0, step2 = 259200.0;
        double dndt = 0.0;
        const double tc = t;
        const double theta = std::fmod(gsto + tc * rptim, kTwoPi);
        em = em + dedt * t;
        inclm = inclm + didt * t;
        argpm = argpm + domdt * t;
        nodem = nodem + dnodt * t;
        mm = mm + dmdt * t;
        double ft = 0.0;
        if (irez != 0) {
            if (atime == 0.0 || t * atime <= 0.0 || std::fabs(t) < std::fabs(atime)) { atime = 0.0; xni = no; xli = xlamo; }
            const double delt = t > 0.0 ? stepp : stepn;
            double xndt = 0, xldot = 0, xnddt = 0;
            for (int guard = 0; guard < 2000000; ++guard) {
                if (irez != 2) {
                    xndt = del1 * std::sin(xli - fasx2) + del2 * std::sin(2.0 * (xli - fasx4)) + del3 * std::sin(3.0 * (xli - fasx6));
                    xldot = xni + xfact;
                    xnddt = del1 * std::cos(xli - fasx2) + 2.0 * del2 * std::cos(2.0 * (xli - fasx4)) + 3.0 * del3 * std::cos(3.0 * (xli - fasx6));
                    xnddt = xnddt * xldot;
                } else {
                    const double xomi = argpo + argpdot * atime;
                    const double x2omi = xomi + xomi, x2li = xli + xli;
                    xndt = d2201 * std::sin(x2omi + xli - g22) + d2211 * std::sin(xli - g22) + d3210 * std::sin(xomi + xli - g32)
                         + d3222 * std::sin(-xomi + xli - g32) + d4410 * std::sin(x2omi + x2li - g44) + d4422 * std::sin(x2li - g44)
                         + d5220 * std::sin(xomi + xli - g52) + d5232 * std::sin(-xomi + xli - g52) + d5421 * std::sin(xomi + x2li - g54)
                         + d5433 * std::sin(-xomi + x2li - g54);
                    xldot = xni + xfact;
                    xnddt = d2201 * std::cos(x2omi + xli - g22) + d2211 * std::cos(xli - g22) + d3210 * std::cos(xomi + xli - g32)
                          + d3222 * std::cos(-xomi + xli - g32) + d5220 * std::cos(xomi + xli - g52) + d5232 * std::cos(-xomi + xli - g52)
                          + 2.0 * (d4410 * std::cos(x2omi + x2li - g44) + d4422 * std::cos(x2li - g44) + d5421 * std::cos(xomi + x2li - g54)
                          + d5433 * std::cos(-xomi + x2li - g54));
                    xnddt = xnddt * xldot;
                }
                if (std::fabs(t - atime) >= stepp) {
                    xli = xli + xldot * delt + xndt * step2;
                    xni = xni + xndt * delt + xnddt * step2;
                    atime = atime + delt;
                } else { ft = t - atime; break; }
            }
            nm = xni + xndt * ft + xnddt * ft * ft * 0.5;
            const double xl = xli + xldot * ft + xndt * ft * ft * 0.5;
            if (irez != 1) mm = xl - 2.0 * nodem + 2.0 * theta;
            else           mm = xl - nodem - argpm + theta;
            dndt = nm - no;
            nm = no + dndt;
        }
    }

    /** Deep-space long-period lunar–solar periodics (with Lyddane's modification at low inclination). */
    void dpper(double t, bool init, double& ep, double& inclp, double& nodep, double& argpp, double& mp) const {
        const double zns = 1.19459e-5, zes = 0.01675, znl = 1.5835218e-4, zel = 0.05490;
        double zm = init ? zmos : zmos + zns * t;
        double zf = zm + 2.0 * zes * std::sin(zm);
        double sinzf = std::sin(zf);
        double f2 = 0.5 * sinzf * sinzf - 0.25, f3 = -0.5 * sinzf * std::cos(zf);
        const double ses = se2 * f2 + se3 * f3, sis = si2 * f2 + si3 * f3;
        const double sls = sl2 * f2 + sl3 * f3 + sl4 * sinzf, sghs = sgh2 * f2 + sgh3 * f3 + sgh4 * sinzf;
        const double shs = sh2 * f2 + sh3 * f3;
        zm = init ? zmol : zmol + znl * t;
        zf = zm + 2.0 * zel * std::sin(zm);
        sinzf = std::sin(zf);
        f2 = 0.5 * sinzf * sinzf - 0.25; f3 = -0.5 * sinzf * std::cos(zf);
        const double sel = ee2 * f2 + e3 * f3, sil = xi2 * f2 + xi3 * f3;
        const double sll = xl2 * f2 + xl3 * f3 + xl4 * sinzf, sghl = xgh2 * f2 + xgh3 * f3 + xgh4 * sinzf;
        const double shll = xh2 * f2 + xh3 * f3;
        double pe = ses + sel, pinc = sis + sil, pl = sls + sll, pgh = sghs + sghl, ph = shs + shll;
        if (init) return;
        // (peo … pho are zero: the periodics are measured from their value at epoch, which the model sets to 0)
        inclp = inclp + pinc;
        ep = ep + pe;
        const double sinip = std::sin(inclp), cosip = std::cos(inclp);
        if (inclp >= 0.2) {
            ph = ph / sinip;
            pgh = pgh - cosip * ph;
            argpp = argpp + pgh;
            nodep = nodep + ph;
            mp = mp + pl;
        } else {
            const double sinop = std::sin(nodep), cosop = std::cos(nodep);
            double alfdp = sinip * sinop, betdp = sinip * cosop;
            const double dalf = ph * cosop + pinc * cosip * sinop;
            const double dbet = -ph * sinop + pinc * cosip * cosop;
            alfdp = alfdp + dalf; betdp = betdp + dbet;
            nodep = std::fmod(nodep, kTwoPi);
            double xls = mp + argpp + cosip * nodep;
            const double dls = pl + pgh - pinc * nodep * sinip;
            xls = xls + dls;
            const double xnoh = nodep;
            nodep = std::atan2(alfdp, betdp);
            if (std::fabs(xnoh - nodep) > kPi) { if (nodep < xnoh) nodep += kTwoPi; else nodep -= kTwoPi; }
            mp = mp + pl;
            argpp = xls - mp - cosip * nodep;
        }
    }

    Elements el_;
    bool ok_ = false, deep = false, isimp = false;
    int error_ = 0, irez = 0;
    double bstar = 0, ecco = 0, argpo = 0, inclo = 0, mo = 0, nodeo = 0, no = 0, gsto = 0;
    double con41 = 0, cc1 = 0, cc4 = 0, cc5 = 0, d2 = 0, d3 = 0, d4 = 0, delmo = 0, eta = 0, argpdot = 0, omgcof = 0;
    double sinmao = 0, t2cof = 0, t3cof = 0, t4cof = 0, t5cof = 0, x1mth2 = 0, x7thm1 = 0, mdot = 0, nodedot = 0;
    double xlcof = 0, xmcof = 0, nodecf = 0, aycof = 0;
    // deep space
    double e3 = 0, ee2 = 0, se2 = 0, se3 = 0, sgh2 = 0, sgh3 = 0, sgh4 = 0, sh2 = 0, sh3 = 0, si2 = 0, si3 = 0;
    double sl2 = 0, sl3 = 0, sl4 = 0, xgh2 = 0, xgh3 = 0, xgh4 = 0, xh2 = 0, xh3 = 0, xi2 = 0, xi3 = 0;
    double xl2 = 0, xl3 = 0, xl4 = 0, zmol = 0, zmos = 0;
    double d2201 = 0, d2211 = 0, d3210 = 0, d3222 = 0, d4410 = 0, d4422 = 0, d5220 = 0, d5232 = 0, d5421 = 0, d5433 = 0;
    double dedt = 0, didt = 0, dmdt = 0, dnodt = 0, domdt = 0, del1 = 0, del2 = 0, del3 = 0;
    double xfact = 0, xlamo = 0, xli = 0, xni = 0, atime = 0;
};

// ── The observer ──────────────────────────────────────────────────────────────────────────────────────────────────
/** A place on the WGS-84 ellipsoid. ★ The server's published grid-square centre is accurate enough (6-character
 *  square ≈ 5 km: AOS/LOS shift by about a second, Doppler by a few hertz — brief §7). */
struct Observer {
    double latDeg = 0, lonDeg = 0, altM = 0;
    bool valid = false;
    double ecef[3] = {0, 0, 0};      // km
    double sinLat = 0, cosLat = 0, sinLon = 0, cosLon = 0;
    Observer() = default;
    Observer(double la, double lo, double altMeters = 0) { set(la, lo, altMeters); }
    void set(double la, double lo, double altMeters = 0) {
        latDeg = la; lonDeg = lo; altM = altMeters;
        valid = std::isfinite(la) && std::isfinite(lo) && la >= -90 && la <= 90 && lo >= -180 && lo <= 180;
        const double a = 6378.137, f = 1.0 / 298.257223563, e2 = f * (2 - f);
        sinLat = std::sin(la * kDeg); cosLat = std::cos(la * kDeg); sinLon = std::sin(lo * kDeg); cosLon = std::cos(lo * kDeg);
        const double nrad = a / std::sqrt(1 - e2 * sinLat * sinLat), h = altMeters / 1000.0;
        ecef[0] = (nrad + h) * cosLat * cosLon;
        ecef[1] = (nrad + h) * cosLat * sinLon;
        ecef[2] = (nrad * (1 - e2) + h) * sinLat;
    }
};

/** Maidenhead square (4 or 6 characters) → its centre. */
inline bool gridCentre(const std::string& g, double& lat, double& lon) {
    std::string u; for (char c : g) if (c != ' ') u += (char)std::toupper((unsigned char)c);
    if (u.size() != 4 && u.size() != 6) return false;
    const int f1 = u[0] - 'A', f2 = u[1] - 'A';
    if (f1 < 0 || f1 >= 18 || f2 < 0 || f2 >= 18 || !std::isdigit((unsigned char)u[2]) || !std::isdigit((unsigned char)u[3])) return false;
    lon = f1 * 20.0 + (u[2] - '0') * 2.0 - 180.0;
    lat = f2 * 10.0 + (u[3] - '0') * 1.0 - 90.0;
    if (u.size() == 6) {
        const int t1 = u[4] - 'A', t2 = u[5] - 'A';
        if (t1 < 0 || t1 >= 24 || t2 < 0 || t2 >= 24) return false;
        lon += t1 * (2.0 / 24.0) + 1.0 / 24.0;
        lat += t2 * (1.0 / 24.0) + 0.5 / 24.0;
    } else { lon += 1.0; lat += 0.5; }
    return true;
}

/** What the observer sees. */
struct Look {
    bool ok = false;
    double azDeg = 0, elDeg = -90, rangeKm = 0;
    double rangeRateKmS = 0;          // + receding
    double subLatDeg = 0, subLonDeg = 0, altKm = 0;
    /** Received minus transmitted frequency for a downlink at `hz` (negative while receding). First order — the
     *  second-order (relativistic) term is under 0.01 Hz at 437 MHz. */
    double dopplerHz(double hz) const { return -hz * (rangeRateKmS * 1000.0) / kC; }
};

/** Satellite position + velocity (TEME) at `unixSec` → what the observer sees. */
inline Look look(Sgp4& s, const Observer& o, double unixSec) {
    Look L;
    double r[3], v[3];
    const int e = s.propagateUnix(unixSec, r, v);
    if (e != 0) return L;
    const double th = gmst(jdFromUnix(unixSec));
    const double c = std::cos(th), sn = std::sin(th);
    // TEME → Earth-fixed (pseudo-Earth-fixed; polar motion is metres)
    const double re[3] = { c * r[0] + sn * r[1], -sn * r[0] + c * r[1], r[2] };
    const double we = 7.292115146706979e-5;   // rad/s (IERS)
    double ve[3] = { c * v[0] + sn * v[1], -sn * v[0] + c * v[1], v[2] };
    ve[0] += we * re[1]; ve[1] -= we * re[0];  // − ω × r
    const double rx = re[0] - o.ecef[0], ry = re[1] - o.ecef[1], rz = re[2] - o.ecef[2];
    const double rng = std::sqrt(rx * rx + ry * ry + rz * rz);
    // topocentric south-east-zenith
    const double s_ = o.sinLat * o.cosLon * rx + o.sinLat * o.sinLon * ry - o.cosLat * rz;
    const double e_ = -o.sinLon * rx + o.cosLon * ry;
    const double z_ = o.cosLat * o.cosLon * rx + o.cosLat * o.sinLon * ry + o.sinLat * rz;
    L.elDeg = std::asin(std::max(-1.0, std::min(1.0, z_ / rng))) / kDeg;
    double az = std::atan2(e_, -s_) / kDeg;
    if (az < 0) az += 360.0;
    L.azDeg = az;
    L.rangeKm = rng;
    L.rangeRateKmS = (rx * ve[0] + ry * ve[1] + rz * ve[2]) / rng;
    // sub-satellite point (geodetic, Bowring's one-step is plenty for a map)
    const double p = std::sqrt(re[0] * re[0] + re[1] * re[1]);
    const double a = 6378.137, f = 1.0 / 298.257223563, e2 = f * (2 - f);
    double lat = std::atan2(re[2], p * (1 - e2));
    for (int i = 0; i < 4; ++i) {
        const double sl = std::sin(lat), nrad = a / std::sqrt(1 - e2 * sl * sl);
        lat = std::atan2(re[2] + nrad * e2 * sl, p);
    }
    const double sl = std::sin(lat), nrad = a / std::sqrt(1 - e2 * sl * sl);
    L.subLatDeg = lat / kDeg;
    L.subLonDeg = std::atan2(re[1], re[0]) / kDeg;
    L.altKm = p / std::cos(lat) - nrad;
    L.ok = true;
    return L;
}

// ── Passes ────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Pass {
    double aos = 0, tca = 0, los = 0;   // UTC Unix seconds
    double maxElDeg = 0, aosAzDeg = 0, losAzDeg = 0;
    bool risingAtStart = false;          // ★ the pass was already up at the search start (aos = the start)
};

/** The passes over `o` between `fromUnix` and `fromUnix + spanSec`, above `minElDeg`. Coarse step from the period
 *  (a LEO: 20 s), edges refined by bisection to 0.5 s, TCA by golden section. ★ Bounded: at most `maxPasses`. */
inline std::vector<Pass> predictPasses(Sgp4& s, const Observer& o, double fromUnix, double spanSec,
                                       double minElDeg = 0.0, size_t maxPasses = 16) {
    std::vector<Pass> out;
    if (!s.ok() || !o.valid || spanSec <= 0) return out;
    const double period = s.periodMin() * 60.0;
    double step = period > 0 ? std::min(120.0, std::max(10.0, period / 300.0)) : 30.0;
    if (period >= 600 * 60.0) step = 300.0;                         // ★ a high orbit moves slowly across the sky
    auto el = [&](double t) { Look L = look(s, o, t); return L.ok ? L.elDeg - minElDeg : -999.0; };
    auto edge = [&](double a, double b) {                            // el(a) and el(b) on opposite sides
        double fa = el(a);
        for (int i = 0; i < 40 && b - a > 0.5; ++i) {
            const double m = 0.5 * (a + b), fm = el(m);
            if ((fm >= 0) == (fa >= 0)) { a = m; fa = fm; } else b = m;
        }
        return 0.5 * (a + b);
    };
    auto peak = [&](double a, double b) {
        const double gr = 0.6180339887498949;
        double c = b - gr * (b - a), d = a + gr * (b - a);
        double fc = el(c), fd = el(d);
        for (int i = 0; i < 60 && b - a > 0.5; ++i) {
            if (fc > fd) { b = d; d = c; fd = fc; c = b - gr * (b - a); fc = el(c); }
            else         { a = c; c = d; fc = fd; d = a + gr * (b - a); fd = el(d); }
        }
        return 0.5 * (a + b);
    };
    const double end = fromUnix + spanSec;
    double t = fromUnix, prev = el(t);
    double aos = prev >= 0 ? fromUnix : 0;
    bool up = prev >= 0, startedUp = up;
    double peakT = t, peakV = prev;
    while (t < end && out.size() < maxPasses) {
        const double tn = std::min(end, t + step);
        const double v = el(tn);
        if (v <= -900) { t = tn; prev = v; continue; }
        if (!up && v >= 0) { aos = edge(t, tn); up = true; peakT = tn; peakV = v; startedUp = false; }
        else if (up && v < 0) {
            const double los = edge(t, tn);
            Pass p; p.aos = aos; p.los = los; p.risingAtStart = startedUp;
            double a = std::max(aos, peakT - step), b = std::min(los, peakT + step);
            p.tca = b > a ? peak(a, b) : peakT;
            Look L = look(s, o, p.tca); p.maxElDeg = L.elDeg;
            p.aosAzDeg = look(s, o, aos).azDeg; p.losAzDeg = look(s, o, los).azDeg;
            out.push_back(p);
            up = false; startedUp = false;
        }
        if (up && v > peakV) { peakV = v; peakT = tn; }
        prev = v; t = tn;
    }
    (void)prev;
    return out;
}

}}  // namespace vibe::sat
