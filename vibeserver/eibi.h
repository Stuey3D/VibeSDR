// EiBi shortwave schedule for a HEADLESS VibeServer — see eibi.cpp for why the server fetches it
// rather than the browser, and why nothing supplied it before.
#pragma once
#include <string>

namespace vseibi {

/** Parse the cached CSV and publish it. Returns the number of entries (0 = no usable cache).
 *  ★ Call at start-up BEFORE any refresh: a receiver with yesterday's schedule is useful
 *    immediately, and waiting on the network to serve a search box is not. */
int loadFromCache();

/** Download this season's schedule, parse, cache and publish. Returns entries, 0 on failure with
 *  `err` set. ★ Never destroys a good cache on a bad download — see the note in the .cpp. */
int refresh(std::string& err);

/** Date the cache was last written (YYYY-MM-DD), or empty when there is none. */
std::string status();

/** Should we fetch? True when there is no cache, when it is more than a day old, or when the
 *  SEASON has rolled over (EiBi publishes a new file twice a year and the old one stops being
 *  what is on air).
 *  ★★★ AGE, NOT UPTIME. The refresh loop originally slept 24 h before its first attempt, so the
 *      timer restarted every time the server did — and saving anything on the setup page restarts
 *      the server. A receiver reconfigured every few days would have refreshed NEVER while
 *      appearing to have a daily refresh. Deciding from the cache's own age cannot be defeated by
 *      restarts. */
bool needsRefresh();

}  // namespace vseibi
