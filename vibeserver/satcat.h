// V12 SATELLITE ENGINE — the orbit data a headless VibeServer fetches for itself (docs/v12/SATELLITE-ENGINE.md §6):
// CelesTrak's public GP data, OMM JSON, the groups the curated list needs (vibe_sat_engine.h catalogueGroups()), cached
// on disk with its age, bounded, offline-safe. See satcat.cpp for the etiquette rules.
#pragma once
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace vssat {

/** The machine's data directory; the cache is <dir>/sat/gp-<group>.json. Shared by every radio process on the box. */
void setDir(const std::string& dir);
/** Where a group's JSON goes (the shim's engine). A hook so the test needs no shim. */
void setPublisher(std::function<void(const std::string& group, const std::string& json)> fn);
/** The groups to keep (default: amateur, weather). */
void setGroups(const std::vector<std::string>& groups);

/** Publish every cached group. Returns how many were published. ★ Before any refresh: yesterday's orbits are good for
 *  days (the engine shows the age), and a radio must not wait on the network. */
int loadFromCache();
/** Should we fetch? A group's cache older than kRefreshSec AND the last attempt (by ANY process on the machine — the
 *  marker is in the shared cache dir) older than the backoff. */
bool needsRefresh();
/** Fetch every group, validate, cache, publish. Returns the groups refreshed; `err` says why one failed.
 *  ★ Stops at the FIRST failure (CelesTrak: "stop immediately on non-200") and backs off. */
int refresh(std::string& err);
/** {"groups":[{"group":"amateur","age":3600,"bytes":41858}],"lastAttempt":…,"lastError":"…","backoff":…} */
std::string statusJson();

constexpr long kRefreshSec = 12 * 3600;        // orbits move ~1 km a day off; twice a day is plenty (CelesTrak updates ~2 h)
constexpr long kMinAttemptSec = 2 * 3600;      // never more often than this from one machine, success or not
constexpr long kBackoffFailSec = 6 * 3600;     // after a refusal / error, then doubling to a day
constexpr size_t kMaxGroupBytes = 2u * 1024 * 1024;   // a group is ~40 KB; a 2 MB "group" is not one

// ── Test seams ──
enum class Fetch { Ok, Refused, Failed };       // Refused = a non-200 answer (rate limit, block, missing)
using Fetcher = std::function<Fetch(const std::string& url, const std::string& dest, std::string& msg)>;
void setFetcher(Fetcher fn);                    // empty = curl
void setClock(std::function<std::time_t()> fn); // empty = the real clock
std::string urlFor(const std::string& group);

}  // namespace vssat
