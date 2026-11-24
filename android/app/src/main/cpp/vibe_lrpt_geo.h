// vibe_lrpt_geo.h — METEOR-M LRPT: where each pixel is on the Earth (V12, 2026-11-24), for the automatic map overlay.
//
// ★★★ FROM OUR OWN DATA, NO HAND-SET OFFSETS. Every input is something the receiver or the satellite engine already has:
//     · the TIME of each picture line — the packet time the satellite stamped on its scan period (Receiver::stripTimeMs,
//       ms of day, UTC on the METEOR-M N2-4 passes measured here) plus the line's place in the period;
//     · the satellite's POSITION and VELOCITY at that time — our SGP4 (vibe_sat_orbit.h) from the element set the
//       satellite engine holds (CelesTrak via the vibesdr.net mirror);
//     · MSU-MR's published GEOMETRY — a nadir-pointing cross-track scanner, 1 km at nadir, a ~2800 km swath (WMO OSCAR
//       "MSU-MR"; ±54° — the half-angle below is from the published swath and checked on real passes, LRPT-MAP-OVERLAY.md);
//     · the Earth — WGS-84, rotating (TEME → Earth-fixed by GMST, as the satellite engine's look() does).
//     There is NO per-satellite "Alfa / Delta" correction table: if the model needs one to fit, it is wrong and the
//     overlay is refused, not nudged.
//
// ★★★ FAIL CLOSED. A wrong outline is worse than none (a coast drawn 30 km off reads as "the picture is wrong"). The pass's
//     clock is checked before anything is drawn: every heard strip's stamp must sit on ONE straight line (the 1.231 s
//     scan period) within a few ms, the period itself must be MSU-MR's, the stamps must agree with OUR clock when they
//     were received (within kMaxClockSkewSec), and the satellite must have been above the horizon then. Any miss → no
//     overlay and a plain-English reason (`why`) the clients show. (METEOR-M2 3 sent wrong on-board times in Oct 2023.)
//
// ★★ HOW (forward and inverse). Each image row r has a time t(r); SGP4 gives the satellite's Earth-fixed position p and its
//    orbital frame — z down the ellipsoid's normal under it (GEODETIC nadir: the geocentric one put every coast ~2.5 km
//    too far north on the 23 Nov pass, LRPT-MAP-OVERLAY.md), y = z × v (across the track; v the inertial velocity: the
//    spacecraft is not yaw-steered for the Earth's turning), x = y × z (along it). Pixel column c is
//    a look angle θ(c) = (c + ½ − W/2) · Δθ · sign in the y–z plane; the ray meets the WGS-84 ellipsoid at the pixel's
//    ground point. The overlay goes the OTHER way, for every coastline vertex: the row is where the vertex crosses the
//    scan plane ((P − p(r)) · x(r) = 0: a binary search over the row table, linear between rows), the column from the
//    angle of P − p in that plane. A vertex hidden behind the Earth or beyond the scan edge is dropped and the line
//    broken there. Cost: one SGP4 call per picture row (≤ 6400, ~1 ms) and ~log2(rows) dot products per vertex.
//
// ★ Memory: the row table is 13 doubles a row (≤ 6400 rows = 666 KB at the cap; a typical 900-row pass ≈ 94 KB), held
//   only while the overlay is built.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "vibe_sat_orbit.h"

namespace vibelrpt {
namespace geo {

constexpr double kDeg = 3.14159265358979323846 / 180.0;
constexpr double kWgsA = 6378.137;                          ///< km
constexpr double kWgsF = 1.0 / 298.257223563;
constexpr double kWgsB = kWgsA * (1.0 - kWgsF);
constexpr double kWgsE2 = kWgsF * (2.0 - kWgsF);
constexpr double kEarthRate = 7.292115146706979e-5;         ///< rad/s (IERS)

/** ★★ MSU-MR as LRPT carries it. */
struct Instrument {
    int width = 1568;                 ///< pixels a line (196 MCUs × 8)
    /** ★★ Half the scan, degrees, from nadir to the outer edge of the edge pixel (pixels are equal steps of angle: a
     *  rotating-mirror scanner). Published figures bracket it — a 2800 km swath (WMO OSCAR, "±54°") to 2950 km (the
     *  instrument makers' paper), i.e. 54.4°–55.6° from an ~830 km orbit — and the coastline fit on Stuart's 23 Nov
     *  METEOR-M2-4 pass (LRPT-MAP-OVERLAY.md) puts it at 55.2°: the same for every MSU-MR, NOT a per-satellite value. */
    double halfScanDeg = 55.2;
    /** −1: column 0 looks to the satellite's RIGHT (east on a northbound pass — the picture's top is its first line, so a
     *  northbound picture is south-up with east on the left). +1 drew Scotland mirrored on the 23 Nov pass. */
    int scanSign = -1;
    double degPerPixel() const { return 2.0 * halfScanDeg / width; }
    double angleOf(double col) const { return scanSign * (col + 0.5 - width * 0.5) * degPerPixel() * kDeg; }
    double colOf(double angleRad) const { return angleRad / (scanSign * degPerPixel() * kDeg) - 0.5 + width * 0.5; }
};

/** One satellite a picture may have come from: the pick's (named by the satellite engine), or every METEOR with LRPT
 *  and an element set (vibe_sat_engine.h lrptCandidates). */
struct Candidate { std::string name; long long norad = 0; vibe::sat::Elements el; bool picked = false; };

/** One picture row: when it was scanned, where the satellite was, its orbital frame (Earth-fixed, km). */
struct Row { double t = 0; double p[3] = {0, 0, 0}, x[3] = {0, 0, 0}, y[3] = {0, 0, 0}, z[3] = {0, 0, 0}; };

inline double dot(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross(const double* a, const double* b, double* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
inline void unit(double* a) { const double n = std::sqrt(dot(a, a)); if (n > 0) { a[0] /= n; a[1] /= n; a[2] /= n; } }

inline void llaToEcef(double latDeg, double lonDeg, double hKm, double* out) {
    const double sl = std::sin(latDeg * kDeg), cl = std::cos(latDeg * kDeg);
    const double n = kWgsA / std::sqrt(1 - kWgsE2 * sl * sl);
    out[0] = (n + hKm) * cl * std::cos(lonDeg * kDeg);
    out[1] = (n + hKm) * cl * std::sin(lonDeg * kDeg);
    out[2] = (n * (1 - kWgsE2) + hKm) * sl;
}
inline void ecefToLatLon(const double* p, double& latDeg, double& lonDeg) {
    const double r = std::sqrt(p[0] * p[0] + p[1] * p[1]);
    double lat = std::atan2(p[2], r * (1 - kWgsE2));
    for (int i = 0; i < 4; ++i) {
        const double sl = std::sin(lat), n = kWgsA / std::sqrt(1 - kWgsE2 * sl * sl);
        lat = std::atan2(p[2] + n * kWgsE2 * sl, r);
    }
    latDeg = lat / kDeg; lonDeg = std::atan2(p[1], p[0]) / kDeg;
}
/** The ray p + s·d meets the WGS-84 ellipsoid first at s (km); false if it misses (beyond the limb). */
inline bool hitEllipsoid(const double* p, const double* d, double& s) {
    const double q[3] = { p[0] / kWgsA, p[1] / kWgsA, p[2] / kWgsB }, e[3] = { d[0] / kWgsA, d[1] / kWgsA, d[2] / kWgsB };
    const double a = dot(e, e), b = 2 * dot(q, e), c = dot(q, q) - 1;
    const double disc = b * b - 4 * a * c;
    if (disc < 0) return false;
    s = (-b - std::sqrt(disc)) / (2 * a);
    return s > 0;
}

/** ★ The satellite's state at UTC Unix time t → a Row (Earth-fixed, the orbital frame from the INERTIAL velocity: the
 *  spacecraft holds its attitude to the orbit, not to the ground under it). */
inline bool rowAt(vibe::sat::Sgp4& s, double t, Row& out, bool geodetic = true) {
    double r[3], v[3];
    if (s.propagateUnix(t, r, v) != 0) return false;
    const double th = vibe::sat::gmst(vibe::sat::jdFromUnix(t)), c = std::cos(th), sn = std::sin(th);
    out.t = t;
    out.p[0] = c * r[0] + sn * r[1]; out.p[1] = -sn * r[0] + c * r[1]; out.p[2] = r[2];
    const double vi[3] = { c * v[0] + sn * v[1], -sn * v[0] + c * v[1], v[2] };
    if (geodetic) {
        double la, lo; ecefToLatLon(out.p, la, lo);
        out.z[0] = -std::cos(la * kDeg) * std::cos(lo * kDeg); out.z[1] = -std::cos(la * kDeg) * std::sin(lo * kDeg); out.z[2] = -std::sin(la * kDeg);
    } else { out.z[0] = -out.p[0]; out.z[1] = -out.p[1]; out.z[2] = -out.p[2]; unit(out.z); }
    cross(out.z, vi, out.y); unit(out.y);
    cross(out.y, out.z, out.x); unit(out.x);
    return true;
}

/** ★★ The georeference of one picture: a row table, then pixel ↔ ground both ways. */
class Georef {
public:
    Instrument ins;
    /** ★ DIAGNOSTICS ONLY (the residual study in LRPT-MAP-OVERLAY.md): a constant attitude error to try. Always 0 in the
     *  product — a constant that "fixes" a pass is a question, not a setting. Roll turns z toward y, pitch z toward x,
     *  yaw x toward y. */
    double rollDeg = 0, pitchDeg = 0, yawDeg = 0;
    bool geodeticNadir = true;
    /** Rows from their UTC times (one per picture row). False (and why) when SGP4 cannot place the satellite. */
    bool build(const vibe::sat::Elements& el, const std::vector<double>& rowUnix, std::string& why) {
        rows_.clear();
        vibe::sat::Sgp4 s;
        if (!s.init(el)) { why = "the satellite's orbit could not be computed"; return false; }
        rows_.resize(rowUnix.size());
        for (size_t i = 0; i < rowUnix.size(); i++) if (!rowAt(s, rowUnix[i], rows_[i], geodeticNadir)) { why = "the satellite's orbit could not be computed"; rows_.clear(); return false; }
        if (rollDeg != 0 || pitchDeg != 0 || yawDeg != 0) for (auto& r : rows_) tilt(r);
        return !rows_.empty();
    }
    size_t rows() const { return rows_.size(); }
    const Row& row(size_t i) const { return rows_[i]; }

    /** Pixel (column, row; may be fractional) → latitude / longitude. False beyond the limb. */
    bool toGround(double col, double rowF, double& latDeg, double& lonDeg) const {
        if (rows_.empty()) return false;
        const double rc = std::max(0.0, std::min(double(rows_.size() - 1), rowF));
        const size_t i = std::min(rows_.size() - 1, size_t(rc)); const size_t j = std::min(rows_.size() - 1, i + 1);
        const double f = rc - double(i);
        Row r; lerp(rows_[i], rows_[j], f, r);
        const double th = ins.angleOf(col);
        double d[3]; for (int k = 0; k < 3; k++) d[k] = std::cos(th) * r.z[k] + std::sin(th) * r.y[k];
        double s; if (!hitEllipsoid(r.p, d, s)) return false;
        double g[3]; for (int k = 0; k < 3; k++) g[k] = r.p[k] + s * d[k];
        ecefToLatLon(g, latDeg, lonDeg);
        return true;
    }
    /** Latitude / longitude (on the ellipsoid) → pixel. False when the place is not in the picture: before the first or
     *  after the last row (by more than `rowSlack`), beyond the scan's edge, or hidden behind the Earth. */
    bool toImage(double latDeg, double lonDeg, double& col, double& rowF, double rowSlack = 0.0) const {
        double P[3]; llaToEcef(latDeg, lonDeg, 0, P);
        return toImageEcef(P, col, rowF, rowSlack);
    }
    bool toImageEcef(const double* P, double& col, double& rowF, double rowSlack = 0.0) const {
        const size_t n = rows_.size();
        if (n < 2) return false;
        auto f = [&](size_t i) { const Row& r = rows_[i]; const double d[3] = { P[0] - r.p[0], P[1] - r.p[1], P[2] - r.p[2] }; return dot(d, r.x); };
        // ★ quick rejection: in front of the satellite at all (the near hemisphere) — the far side has the same sign pattern
        const Row& mid = rows_[n / 2];
        { const double d[3] = { P[0] - mid.p[0], P[1] - mid.p[1], P[2] - mid.p[2] }; if (dot(d, mid.z) <= 0) return false; }
        double f0 = f(0), f1 = f(n - 1);
        // along the track the satellite moves +x: a point ahead has f > 0; it is crossed between the rows where f changes sign
        double rf;
        if (f0 < 0) {                     // before the first row
            const double step = f0 - f(1);
            if (!(step > 0) || -f0 / step > rowSlack) return false;
            rf = f0 / step;               // negative, within slack
        } else if (f1 > 0) {              // after the last row
            const double step = f(n - 2) - f1;
            if (!(step > 0) || f1 / step > rowSlack) return false;
            rf = double(n - 1) + f1 / step;
        } else {
            size_t lo = 0, hi = n - 1;    // f(lo) >= 0 > f(hi)… (f(hi) <= 0)
            while (hi - lo > 1) { const size_t m = (lo + hi) / 2; if (f(m) >= 0) lo = m; else hi = m; }
            const double a = f(lo), b = f(hi);
            rf = double(lo) + (a - b != 0 ? a / (a - b) : 0.0);
        }
        // the angle across the track, from the (interpolated) frame at that row
        const double rc = std::max(0.0, std::min(double(n - 1), rf));
        const size_t i = std::min(n - 2, size_t(rc));
        Row r; lerp(rows_[i], rows_[i + 1], rc - double(i), r);
        const double d[3] = { P[0] - r.p[0], P[1] - r.p[1], P[2] - r.p[2] };
        const double dz = dot(d, r.z), dy = dot(d, r.y);
        if (dz <= 0) return false;
        const double th = std::atan2(dy, dz);
        if (std::fabs(th) > (ins.halfScanDeg + 0.5) * kDeg) return false;
        // ★ hidden behind the Earth: the ray at that angle must meet the ground at THIS point, not before it
        double dir[3]; for (int k = 0; k < 3; k++) dir[k] = std::cos(th) * r.z[k] + std::sin(th) * r.y[k];
        double s; if (!hitEllipsoid(r.p, dir, s)) return false;
        const double dist = std::sqrt(dz * dz + dy * dy);
        if (std::fabs(s - dist) > 0.02 * dist + 2.0) return false;
        col = ins.colOf(th); rowF = rf;
        return true;
    }
private:
    std::vector<Row> rows_;
    static void turn(double* a, double* b, double ang) {   // rotate the pair (a, b) by ang: a → a cos + b sin
        const double c = std::cos(ang), sn = std::sin(ang);
        for (int k = 0; k < 3; k++) { const double na = c * a[k] + sn * b[k], nb = -sn * a[k] + c * b[k]; a[k] = na; b[k] = nb; }
    }
    void tilt(Row& r) const { turn(r.z, r.y, rollDeg * kDeg); turn(r.z, r.x, pitchDeg * kDeg); turn(r.x, r.y, yawDeg * kDeg); }
    static void lerp(const Row& a, const Row& b, double f, Row& o) {
        o.t = a.t + (b.t - a.t) * f;
        for (int k = 0; k < 3; k++) { o.p[k] = a.p[k] + (b.p[k] - a.p[k]) * f; o.x[k] = a.x[k] + (b.x[k] - a.x[k]) * f; o.y[k] = a.y[k] + (b.y[k] - a.y[k]) * f; o.z[k] = a.z[k] + (b.z[k] - a.z[k]) * f; }
    }
};

} // namespace geo
} // namespace vibelrpt
