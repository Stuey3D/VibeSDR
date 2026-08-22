// IP -> ISO-3166 country code, from the RIRs' own delegated-extended statistics.
// See geoip.cpp for why this source and not a geolocation API or MaxMind.
#pragma once

#include <string>
#include <vector>

namespace geoip {

/** Where the cache lives. Default /var/lib/vibeserver. */
void setDir(const std::string& dir);

/** Load the parsed cache from disk. False when there is none (a fresh install). */
bool load();

/** Download all five registry files and rebuild the cache. Slow (tens of MB) — call it off the
 *  DSP thread and never on a request. */
bool refresh(std::string& err);
/** Rebuild from files ALREADY on disk — the app downloads them where curl does not exist.
 *  See geoip.cpp. */
bool ingest(const std::vector<std::string>& files, std::string& err);

/** The registry URLs refresh() uses, for a host that downloads them itself. */
std::vector<std::string> sources();


/** Is the cache missing or older than `maxAgeDays`? */
bool stale(int maxAgeDays);

/** Two-letter code, or empty when unknown — which is a legitimate answer for unallocated space,
 *  a private address, or a fresh install with no data yet. The caller must render empty as
 *  "no flag", never as a guess. */
std::string lookup(const std::string& ip);

/** How many ranges are loaded, and when the cache was built (epoch seconds). */
int count();
long long updated();

}  // namespace geoip
