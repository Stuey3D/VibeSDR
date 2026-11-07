// map_geo.h — the map's arithmetic: Web Mercator (the projection every VibeSDR map uses — Leaflet's, and the relief
// image was reprojected with its exact cut-off latitude), the Maidenhead locator (web/client/src/handheldMap.ts
// latLonToLocator, ported), range and bearing (web/client/src/tracks.ts rangeBearing).
// ★ World coordinates are Mercator in [0, 1] × [0, 1] (x east from −180°, y SOUTH from +85.05°): one float pair a point
//   on the GPU. At z14 the world is 4.2 M px wide and float32 still resolves 0.25 px — the map stops at z14.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace hh {

constexpr double kMercLat = 85.0511287798066;   // index.json relief.mercatorLat — the image's own constant
constexpr int kMapMinZ = 1, kMapMaxZ = 14;

inline double mercX(double lon) { return (lon + 180.0) / 360.0; }
inline double mercY(double lat) {
    lat = std::max(-kMercLat, std::min(kMercLat, lat));
    const double s = std::sin(lat * M_PI / 180.0);
    return 0.5 - std::log((1 + s) / (1 - s)) / (4 * M_PI);
}
inline double mercLon(double x) { return x * 360.0 - 180.0; }
inline double mercLat(double y) { return std::atan(std::sinh(M_PI * (1 - 2 * y))) * 180.0 / M_PI; }
/** World size in pixels at Leaflet zoom z (256 · 2^z). */
inline double worldPx(double z) { return 256.0 * std::pow(2.0, z); }
/** Longitude wrapped into [−180, 180). */
inline double wrapLon(double lon) { return std::fmod(std::fmod(lon + 180.0, 360.0) + 360.0, 360.0) - 180.0; }

/** The 6-character locator ("IO92nh"), the subsquare in lower case — handheldMap.ts latLonToLocator. */
inline std::string maidenhead(double lat, double lon) {
    const double la = std::min(89.999999, std::max(-90.0, lat)) + 90;
    const double lo = std::fmod(std::fmod(lon + 180, 360) + 360, 360);
    const char* A = "ABCDEFGHIJKLMNOPQRSTUVWX";
    const int f1 = (int)std::floor(lo / 20), f2 = (int)std::floor(la / 10);
    const int s1 = (int)std::floor(std::fmod(lo, 20) / 2), s2 = (int)std::floor(std::fmod(la, 10));
    const int u1 = (int)std::floor(std::fmod(lo, 2) * 60 / 5), u2 = (int)std::floor(std::fmod(la, 1) * 60 / 2.5);
    std::string s;
    s += A[f1 % 18]; s += A[f2 % 18]; s += (char)('0' + s1); s += (char)('0' + s2);
    s += (char)(A[u1 % 24] + 32); s += (char)(A[u2 % 24] + 32);
    return s;
}

/** Great-circle range (km) and initial bearing (°) — tracks.ts rangeBearing. */
inline void rangeBearing(double lat1, double lon1, double lat2, double lon2, double& km, double& brg) {
    const double D = M_PI / 180, p1 = lat1 * D, p2 = lat2 * D, dp = (lat2 - lat1) * D, dl = (lon2 - lon1) * D;
    const double h = std::pow(std::sin(dp / 2), 2) + std::cos(p1) * std::cos(p2) * std::pow(std::sin(dl / 2), 2);
    km = 2 * 6371.0088 * std::asin(std::min(1.0, std::sqrt(h)));
    const double y = std::sin(dl) * std::cos(p2), x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    brg = std::fmod(std::atan2(y, x) / D + 360, 360);
}
inline const char* compass8(double d) {
    static const char* c[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return c[(int)std::lround(std::fmod(std::fmod(d, 360) + 360, 360) / 45) % 8];
}
/** "94 NM SW" — distances in nautical miles, as on the web's ADS-B / NDB maps. */
inline std::string nmText(double km, double brg) {
    if (!(km >= 0)) return "";
    const double nm = km / 1.852;
    char b[40];
    if (nm < 10) std::snprintf(b, sizeof b, "%.1f NM %s", nm, compass8(brg));
    else std::snprintf(b, sizeof b, "%.0f NM %s", nm, compass8(brg));
    return b;
}
/** "now" / "12 s" / "3 min" / "1 h 4 min" — tracks.ts ago. */
inline std::string agoText(double s) {
    const long v = std::max(0L, std::lround(s));
    char b[32];
    if (v < 5) return "now";
    if (v < 60) std::snprintf(b, sizeof b, "%ld s", v);
    else if (v < 3600) std::snprintf(b, sizeof b, "%ld min", (long)std::lround(v / 60.0));
    else std::snprintf(b, sizeof b, "%ld h %ld min", v / 3600, (long)std::lround((v % 3600) / 60.0));
    return b;
}

}  // namespace hh
