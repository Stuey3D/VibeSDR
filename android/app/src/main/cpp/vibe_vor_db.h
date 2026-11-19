// vibe_vor_db.h — V12 the VOR / ILS station list (vibe_vor_navaids.h, generated from OurAirports, public domain): the stations on
// a channel nearest the server, and the great-circle helpers the radial line needs. docs/v12/VOR-ILS.md.
#pragma once
#include "vibe_vor_navaids.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace vibevor {

struct Station {
    const StationRec* r = nullptr;
    double km = NAN, brg = NAN;   ///< from the server (NAN without a position)
    const char* name() const { return r ? kStationNames + r->name : ""; }
};
inline double rad(double d) { return d * M_PI / 180.0; }
inline double deg(double r) { return r * 180.0 / M_PI; }
/** Great-circle distance (km) and initial bearing (deg true) from (la1, lo1) to (la2, lo2). */
inline void distBearing(double la1, double lo1, double la2, double lo2, double& km, double& brg) {
    const double p1 = rad(la1), p2 = rad(la2), dl = rad(lo2 - lo1);
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    km = 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a)));
    brg = std::fmod(deg(std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl))) + 360.0, 360.0);
}
/** The point `km` along the great circle from (la, lo) at initial bearing `brg` (deg true). */
inline void destination(double la, double lo, double brg, double km, double& la2, double& lo2) {
    const double d = km / 6371.0, p1 = rad(la), l1 = rad(lo), t = rad(brg);
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(t));
    const double l2 = l1 + std::atan2(std::sin(t) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    la2 = deg(p2); lo2 = std::fmod(deg(l2) + 540.0, 360.0) - 180.0;
}
/** How far (km) the point (la, lo) is from the great circle leaving (sla, slo) at `brg`, and whether it is AHEAD (along the
 *  radial, not behind the station). */
inline double crossTrackKm(double sla, double slo, double brg, double la, double lo, bool& ahead) {
    double d13, b13; distBearing(sla, slo, la, lo, d13, b13);
    const double dx = std::asin(std::sin(d13 / 6371.0) * std::sin(rad(b13 - brg))) * 6371.0;
    ahead = std::cos(rad(b13 - brg)) > 0;
    return std::fabs(dx);
}
/** The stations of `kind` (0 VOR, 1 localiser) on the 50 kHz channel `khz`, nearest first (by name when no position). */
inline std::vector<Station> on(int khz, int kind, bool havePos, double lat, double lon) {
    std::vector<Station> v;
    const StationRec* b = kStations;
    const StationRec* e = kStations + kStationCount;
    const StationRec* p = std::lower_bound(b, e, (uint32_t)(khz - 10), [](const StationRec& r, uint32_t k) { return r.khz < k; });
    for (; p != e && p->khz <= (uint32_t)(khz + 10); ++p) {
        if (p->kind != kind) continue;
        Station s; s.r = p;
        if (havePos) distBearing(lat, lon, p->lat, p->lon, s.km, s.brg);
        v.push_back(s);
    }
    if (havePos) std::sort(v.begin(), v.end(), [](const Station& a, const Station& c) { return a.km < c.km; });
    return v;
}

} // namespace vibevor
