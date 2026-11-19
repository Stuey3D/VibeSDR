// V12 SATELLITE ENGINE — the satellite data a headless VibeServer keeps for itself (docs/v12/SATELLITE-ENGINE.md §6):
// sats.json from vibesdr.net's mirror (directory/src/sats.js — the ONLY CelesTrak client in VibeSDR), synced like the
// EiBi schedule: daily, jittered, a conditional GET, the last good copy kept, one shared fetch for every radio process
// on the machine, backed off on errors. ★★★ A VibeServer NEVER asks CelesTrak itself (brief §6.1 TRAP: carrier CGNAT —
// thousands of servers asking directly would get whole carriers firewalled). See satcat.cpp for the rules.
#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>

namespace vssat {

/** The machine's data directory; the copy is <dir>/sat/sats.json. Shared by every radio process on the box. */
void setDir(const std::string& dir);
/** Where sats.json goes (the shim's engine). */
void setPublisher(std::function<void(const std::string& satsJson)> fn);
/** ★ Old caches from v12 builds that asked CelesTrak directly (<dir>/sat/gp-<group>.json, OMM JSON): published at start
 *  ONLY when there is no sats.json yet, so an upgraded server is not left with nothing while it first syncs. */
void setLegacyPublisher(std::function<void(const std::string& group, const std::string& json)> fn);
/** A dry parse of a download (the engine's parseSats): a 200 that fails it never replaces the copy. Empty = structural. */
void setValidator(std::function<bool(const std::string& body, std::string& err)> fn);

/** Publish the copy (any age — the engine shows how old). Returns 1 if published, else the legacy groups published. */
int loadFromCache();
/** ★ Another radio process refreshed the shared copy: publish it here too. true = published. */
bool reloadIfChanged();
/** Should we ask? Never inside a hold; never within kMinAttemptSec of ANY process's attempt; else when the copy is
 *  missing or its last check is older than a day less this machine's own jitter. */
bool needsRefresh();
/** One conditional GET. Returns 1 = new data published, 0 = unchanged (304) or failed (`err` = the owner message).
 *  ★ Machine-wide lock: a second process finds it held and asks nothing. */
int refresh(std::string& err);
/** For the setup page: {"source":"vibesdr.net","url":…,"have":…,"checked":…,"nextTry":…,"code":…,"failures":…,
 *  "message":"…"} — `message` is "" when all is well. */
std::string statusJson();
/** The owner's words for the last failure ("" = none): what happened, the HTTP code, and when VibeServer asks again. */
std::string ownerMessage();
/** The few words a listener's greyed SAT button carries when there is no data at all ("" = not downloaded yet). */
std::string listenerNote();

constexpr const char* kUrl = "https://vibeserver.vibesdr.net/api/sats";
constexpr long kCheckSec = 24 * 3600;          // daily, like EiBi (the mirror rebuilds every 6 h; elements < 3 d = fresh)
constexpr long kCheckJitterSec = 4 * 3600;     // ★ per machine, stable: a fleet does not check in at the same hour
constexpr long kMinAttemptSec = 3600;          // never more often than this from one machine, success or not
constexpr long kBackoffSec = 3600;             // after a failure, doubling to a day
constexpr long kHoldFloorSec = 15 * 60;        // ★★★ no hold is shorter, whatever the configuration (CelesTrak's rule)
constexpr long kResumeJitterSec = 3600;        // ★★ + a random 0–60 min on every hold: a fleet does not return together
constexpr size_t kMaxBytes = 4u * 1024 * 1024; // sats.json is ~20 KB; 4 MB is not one

// ── Test seams ──
struct Request { std::string url, dest, ifNoneMatch, ifModifiedSince; };
struct Response { int code = 0; std::string etag, lastModified, msg; };   // code 0 = no answer (network, timeout)
using Fetcher = std::function<void(const Request&, Response&)>;
void setFetcher(Fetcher fn);                    // empty = curl
void setClock(std::function<std::time_t()> fn); // empty = the real clock
void setRandom(std::function<uint32_t()> fn);   // empty = std::random_device
std::string userAgent();

}  // namespace vssat
