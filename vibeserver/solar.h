// Space weather (NOAA, public domain) — see solar.cpp for why this source and not the ham ones.
#pragma once
#include <ctime>
#include <string>

namespace vssolar {

struct Solar {
    double sfi = -1;        // 10.7 cm solar flux
    double kp  = -1;        // planetary K index, 0-9
    std::string updated;    // ISO time of the newest reading we used
    bool valid() const { return sfi >= 0 || kp >= 0; }
};

/** Is the sun above the horizon at this position, now?
 *  ★★★ A REAL SUN CALCULATION, NOT AN OFFICE-HOURS WINDOW. This began as "day = 07:00-19:00
 *  local", which is wrong by hours for most of the year and most latitudes: at 52°N in August the
 *  sun is up at 04:30 and still up at 20:30, so the server reported NIGHT through a summer morning
 *  and predicted 80m "Excellent" on a band that was plainly a daytime Fair (Stuart, 2026-08-06).
 *  D-layer absorption follows the SUN, so the model must too.
 *  @param lat,lon degrees, north/east positive. */
bool sunUp(double lat, double lon, std::time_t when);

/** Latitude/longitude from a Maidenhead locator, e.g. "IO92nh". False if it does not parse.
 *  ★ The locator is the position field owners actually fill in — lat/lon are optional on the
 *    setup page and were empty on the very server this was written for. */
bool gridToLatLon(const std::string& grid, double& lat, double& lon);

/** Fetch now. False + `err` on failure; a failure never destroys the last good reading. */
bool fetch(std::string& err);
/** True when the cache is older than an hour (or empty). Cheap. */
bool needsRefresh();
/** The last good reading. */
Solar current();

/** Our own day/night verdict for one amateur band, from the flux and the K index.
 *  ★★★ COMPUTED HERE, AND THAT IS THE POINT. Ready-made per-band verdicts exist (N0NBH publishes
 *  them) but come from a volunteer's server with informal terms; NOAA gives the raw indices as
 *  public-domain data and asks nothing. So the FACTS are NOAA's and the INTERPRETATION is ours,
 *  which leaves nobody's terms stretched and nothing for an app store to question.
 *  ★★ It is a rule of thumb, not a propagation model, and the UI must not imply otherwise: the
 *     honest reading is "what the solar numbers suggest", sitting beside what this receiver can
 *     actually hear — which is the measurement that really answers the question.
 *  @param bandLabel "80m", "40m", ...  @param day true for daylight at the receiver.
 *  Empty when we have no reading. */
std::string bandVerdict(const Solar& s, const std::string& bandLabel, bool day);

}  // namespace vssolar
