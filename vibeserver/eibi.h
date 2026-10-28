// EiBi shortwave schedule for a HEADLESS VibeServer — see eibi.cpp for why the server fetches it
// rather than the browser, and why nothing supplied it before.
#pragma once
#include <ctime>
#include <functional>
#include <string>

namespace vseibi {

/** Where the cache lives. ★ Settable because /var/lib does not exist on macOS, so a hardcoded path
 *  there means the schedule can never download — see vsDataDir() in main.cpp. */
void setDir(const std::string& dir);

/** Where a parsed schedule goes (the shim's GET /stations list). ★ A hook rather than a direct
 *  call so this file does not drag the whole shim into its test — main.cpp points it at
 *  LocalSdrShim::setStationsJson before the first load. */
void setPublisher(std::function<void(const std::string& json)> fn);

/** Parse the cached CSV and publish it. Returns the number of entries (0 = no usable cache).
 *  ★ Call at start-up BEFORE any refresh: a receiver with yesterday's schedule is useful
 *    immediately, and waiting on the network to serve a search box is not. */
int loadFromCache();

/** Download this season's schedule, parse, cache and publish. Returns entries, 0 on failure with
 *  `err` set. ★ Never destroys a good cache on a bad download — see the note in the .cpp.
 *  ★★ When EiBi has not published this season's file yet (404, or a page with no entries) it
 *     falls back to the PREVIOUS season's file and returns its entries with `err` EMPTY: that is
 *     not a failure, it is the normal state of the first days of a season. note() says so. */
int refresh(std::string& err);

/** Date the cache was last written (YYYY-MM-DD), or empty when there is none. */
std::string status();

/** A plain sentence for the owner when the cache is NOT this season's file because EiBi has not
 *  published it yet — "EiBi hasn't published the B26 (winter) schedule yet — using A26 until it
 *  appears." Empty when the cache is current (or when there is nothing to explain). */
std::string note();

/** Should we fetch? True when there is no cache, when it is more than a day old, or when the
 *  SEASON has rolled over (EiBi publishes a new file twice a year and the old one stops being
 *  what is on air).
 *  ★★★ AGE, NOT UPTIME. The refresh loop originally slept 24 h before its first attempt, so the
 *      timer restarted every time the server did — and saving anything on the setup page restarts
 *      the server. A receiver reconfigured every few days would have refreshed NEVER while
 *      appearing to have a daily refresh. Deciding from the cache's own age cannot be defeated by
 *      restarts.
 *  ★★ A SEASON EiBi HAS NOT PUBLISHED is asked for again every kRetryUnpublishedSec, not on every
 *     wake — our season switch is a date guess, and EiBi can be days or weeks behind it
 *     (B26 was still 404 on 2026-10-27, NickB). */
bool needsRefresh();

// ── Season arithmetic, exposed for the test ───────────────────────────────────────────────────
/** "a26" / "b26" for the moment `t`. A = summer (~last Sun Mar → ~last Sun Oct), B = winter;
 *  EiBi labels a season by the year it STARTED, so Jan/Feb belong to the previous year's B. */
std::string seasonCodeAt(std::time_t t);
/** The season before `code`: b26 → a26, a27 → b26. */
std::string previousSeason(const std::string& code);
/** "B26 (winter)" / "A26 (summer)". */
std::string seasonLabel(const std::string& code);
/** How long after EiBi said "not yet" before asking again: 6 hours. */
constexpr long kRetryUnpublishedSec = 6 * 3600;

// ── Test seams ────────────────────────────────────────────────────────────────────────────────
enum class Fetch { Ok, NotPublished, Failed };
/** Download `url` to `dest`. NotPublished = the server answered "no such file" (404/410);
 *  Failed = anything else, with `msg` saying what. The default runs curl (no shell). */
using Fetcher = std::function<Fetch(const std::string& url, const std::string& dest, std::string& msg)>;
void setFetcher(Fetcher fn);                      // empty = back to curl
void setClock(std::function<std::time_t()> fn);   // empty = the real clock

}  // namespace vseibi
