// Space weather from NOAA SWPC — and PREDICTED band conditions computed here from it.
//
// ★★★ WHY NOAA AND NOT THE HAM SOURCES. hamqsl.com (N0NBH) publishes ready-made per-band
// Good/Fair/Poor, which is tempting and wrong for a SHIPPED PRODUCT. His own page says: "these
// feeds will last as long as I do not hear grief from my ISP provider. If I do they are gone!!"
// That is one volunteer paying for the bandwidth — and every VibeServer we ship would hit him
// hourly, forever. Building a product feature on that is both unkind and unreliable.
//   NOAA's Space Weather Prediction Center is a funded government service designed for
// programmatic access, and its data is explicitly PUBLIC DOMAIN ("may be used without charge for
// any lawful purpose"). No licence, no attribution obligation, nothing for an app store to
// question, and it will not fall over.
//
// ★★ SO WE COMPUTE THE BAND VERDICT OURSELVES, from the flux and the K index. That is not a
// workaround, it is the honest version: the numbers are facts from NOAA, the interpretation is
// ours and is labelled as ours. Nobody's terms are being stretched.
//
// ★ COST: one HTTPS request an hour, via curl, on a detached thread. NOAA publishes F10.7 daily
//   and Kp every minute, so hourly is already finer than the data changes for HF purposes.
#include "solar.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace vssolar {
namespace {

std::mutex g_mtx;
Solar      g_cur;
double     g_fetchedAt = 0;      // CLOCK_MONOTONIC seconds; 0 = never

double monoNow() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec;
}

std::string runCmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

/** Value of `key` in the LAST JSON object of a flat array — these feeds are chronological, so the
 *  last entry is the newest. ★ Not a JSON parser on purpose: two known feeds, two numbers. */
bool lastNumber(const std::string& json, const std::string& key, double& out) {
    const std::string pat = "\"" + key + "\"";
    const size_t at = json.rfind(pat);
    if (at == std::string::npos) return false;
    size_t p = json.find(':', at);
    if (p == std::string::npos) return false;
    p++;
    while (p < json.size() && (isspace((unsigned char)json[p]) || json[p] == '"')) p++;
    char* end = nullptr;
    const double v = strtod(json.c_str() + p, &end);
    if (end == json.c_str() + p) return false;
    out = v;
    return true;
}

std::string lastString(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    const size_t at = json.rfind(pat);
    if (at == std::string::npos) return "";
    size_t p = json.find(':', at);
    if (p == std::string::npos) return "";
    p = json.find('"', p);
    if (p == std::string::npos) return "";
    const size_t e = json.find('"', p + 1);
    return e == std::string::npos ? "" : json.substr(p + 1, e - p - 1);
}

}  // namespace

bool fetch(std::string& err) {
    // ★ -f so an error PAGE is a failure rather than something we "parse" into zeroes and then
    //   publish as though it were a reading.
    const std::string flux = runCmd(
        "curl -fsSL --max-time 25 https://services.swpc.noaa.gov/json/f107_cm_flux.json 2>/dev/null");
    const std::string kp = runCmd(
        "curl -fsSL --max-time 25 https://services.swpc.noaa.gov/json/planetary_k_index_1m.json 2>/dev/null");

    Solar s;
    double v = 0;
    if (lastNumber(flux, "flux", v) && v > 0 && v < 500) s.sfi = v;
    if (lastNumber(kp, "estimated_kp", v) && v >= 0 && v <= 9) s.kp = v;
    s.updated = lastString(kp, "time_tag");

    if (!s.valid()) {
        // ★★ KEEP THE LAST GOOD READING. A network blip must not blank a panel that was correct
        //    a minute ago — stale-but-labelled beats empty, and the UI shows the timestamp.
        err = "no usable reading from NOAA SWPC";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_cur = s;
        // ★ MONOTONIC, never wall clock. A Pi has no RTC: its clock jumps years the moment NTP
        //   lands, and a wall-clock age would then read as "hours in the future" and either
        //   hammer NOAA or never refresh again.
        g_fetchedAt = monoNow();
    }
    return true;
}

bool needsRefresh() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_fetchedAt <= 0) return true;
    return (monoNow() - g_fetchedAt) > 3600.0;
}

Solar current() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_cur;
}

bool gridToLatLon(const std::string& g, double& lat, double& lon) {
    if (g.size() < 4) return false;
    auto up = [](char c){ return (char)toupper((unsigned char)c); };
    const int A = up(g[0]) - 'A', B = up(g[1]) - 'A';
    if (A < 0 || A > 17 || B < 0 || B > 17) return false;
    if (!isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3])) return false;
    lon = A * 20.0 - 180.0 + (g[2] - '0') * 2.0;
    lat = B * 10.0 -  90.0 + (g[3] - '0') * 1.0;
    if (g.size() >= 6) {                       // sub-square: 5 min lon, 2.5 min lat
        const int a = up(g[4]) - 'A', b = up(g[5]) - 'A';
        if (a >= 0 && a < 24 && b >= 0 && b < 24) {
            lon += a * (2.0 / 24.0);
            lat += b * (1.0 / 24.0);
        }
    }
    lon += 1.0;    // centre of the square rather than its corner
    lat += 0.5;
    return true;
}

bool sunUp(double lat, double lon, std::time_t when) {
    std::tm g{};
    gmtime_r(&when, &g);
    const double dayFrac = g.tm_hour + g.tm_min / 60.0 + g.tm_sec / 3600.0;
    // Days since J2000, at the current instant.
    const double n = (double)(when / 86400.0) - 10957.5;
    // ★ The low-precision solar position from the Astronomical Almanac: good to about a minute of
    //   arc, which is far beyond what a day/night verdict needs. Anything more elaborate would be
    //   precision we then throw away in a four-way Good/Fair/Poor bucket.
    const double D2R = M_PI / 180.0;
    const double L = std::fmod(280.460 + 0.9856474 * n, 360.0);          // mean longitude
    const double gAnom = std::fmod(357.528 + 0.9856003 * n, 360.0) * D2R; // mean anomaly
    const double lambda = (L + 1.915 * std::sin(gAnom) + 0.020 * std::sin(2 * gAnom)) * D2R;
    const double eps = (23.439 - 0.0000004 * n) * D2R;                    // obliquity
    const double decl = std::asin(std::sin(eps) * std::sin(lambda));      // declination
    // Greenwich hour angle of the sun, then local.
    const double eqTime = 4.0 * (L - 0.0057183 - std::atan2(std::cos(eps) * std::sin(lambda),
                                                            std::cos(lambda)) / D2R);
    const double trueSolarMin = dayFrac * 60.0 + eqTime + 4.0 * lon;
    const double ha = (trueSolarMin / 4.0 - 180.0) * D2R;                 // hour angle
    const double elev = std::asin(std::sin(lat * D2R) * std::sin(decl) +
                                  std::cos(lat * D2R) * std::cos(decl) * std::cos(ha));
    // ★ Above the horizon = day. The D layer forms and decays with the sun, so this is the line
    //   that actually matters to HF rather than any clock hour.
    return elev > 0.0;
}

std::string bandVerdict(const Solar& s, const std::string& band, bool day) {
    if (!s.valid()) return "";
    const double sfi = s.sfi >= 0 ? s.sfi : 100.0;
    const double kp  = s.kp  >= 0 ? s.kp  : 2.0;

    double mhz = 0;
    if      (band == "160m") mhz = 1.8;
    else if (band == "80m")  mhz = 3.6;
    else if (band == "60m")  mhz = 5.3;
    else if (band == "40m")  mhz = 7.1;
    else if (band == "30m")  mhz = 10.1;
    else if (band == "20m")  mhz = 14.1;
    else if (band == "17m")  mhz = 18.1;
    else if (band == "15m")  mhz = 21.1;
    else if (band == "12m")  mhz = 24.9;
    else if (band == "10m")  mhz = 28.1;
    else return "";

    // ★★★ CALIBRATED AGAINST UberSDR, NOT INVENTED. The first cut of this was my own guess and it
    //     read a band or two PESSIMISTIC — 160/80/60m came out "Good" at night where every
    //     operator (and UberSDR) says "Excellent" (Stuart, 2026-08-06: "ours may be reporting good
    //     and excellent a bit low"). So the shape below is fitted to UberSDR's own published
    //     table at SFI 158 / K 1, all twenty day+night verdicts, and there is a test that checks
    //     it still reproduces them: tools/bandmodel-check.
    //     ★ Fitting to a reference is not the same as copying one: the DATA is NOAA's public
    //       domain, the model is ours, and the reference only told us where our thresholds sat.
    double score;
    if (day) {
        // ★ Daytime is a fight between D-layer ABSORPTION (kills the low bands, falls off fast
        //   with frequency) and the MUF (nothing propagates far above it). The result peaks in
        //   the middle — 20m at this flux — which is exactly what every HF operator expects.
        const double mufDay = 12.0 + (sfi - 70.0) * 0.13;
        score = 3.0 - 7.0 / std::pow(mhz, 1.3);
        score += 0.45 * std::exp(-std::pow((mhz - 15.0) / 4.0, 2.0));   // the mid-band peak
        score -= std::max(0.0, mhz - 0.5 * mufDay) * 0.05;              // thinning above it
    } else {
        // ★ After dark the D layer lifts, so the low bands are as good as they get — and the MUF
        //   falls, so the high bands simply close. One cutoff describes the whole night.
        const double mufNight = 7.0 + (sfi - 70.0) * 0.006;
        score = 3.0 - std::max(0.0, mhz - mufNight) * 0.25;
    }
    // ★ Geomagnetic activity hurts everything and hurts the low bands most. Below Kp 3 it is not
    //   worth mentioning; Kp 5 is a storm.
    if (kp > 3.0) score -= (kp - 3.0) * (mhz < 10.0 ? 0.55 : 0.40);

    if (score >= 2.9) return "Excellent";
    if (score >= 1.8) return "Good";
    if (score >= 0.9) return "Fair";
    return "Poor";
}

}  // namespace vssolar
