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
/** ★ V12 SATELLITE MODE: the last good sync — Unix s of the last 200 / 304 from the mirror; 0 = never. */
long long lastSynced();
/** Is there a copy on this machine at all? */
bool haveCopy();
/** ★ Some process on the machine holds the fetch lock right now (it is asking). */
bool lockBusy();

// ── ★★ V12 SATELLITE MODE (docs/v12/SATELLITE-MODE.md item 9): the daily runner + THE ON-OPEN FETCH ─────────────────
/** What the runner needs from its host (main.cpp, or the Android app's own server through JNI). */
struct RunnerHooks {
    std::function<bool()> wanted;                    // a radio here reaches a listed downlink (else nothing is asked)
    std::function<void(const std::string&)> note;    // listenerNote() after every round (the greyed control's words)
    std::function<void(const std::string&)> log;     // one line per new outcome
};
/** Start the ONE background thread: `firstDelaySec` of start-up jitter (a fleet restarted together), then a round every
 *  10 min — a round asks only when needsRefresh() says so (daily, the holds). kick() cuts any wait short.
 *  A second call only replaces the hooks. */
void startDaily(RunnerHooks hooks, int firstDelaySec);
/** ★★ THE ON-OPEN FETCH: a listener opened SATELLITE on a server with no orbit data. Wakes the runner at once — only when
 *  a request is allowed now (needsRefresh: no hold, not too soon) or another process is asking already, so the holds and
 *  the one-fetch-per-machine lock still rule. true = a fetch is under way (the list says `fetching:true`); false = held,
 *  no runner, or nothing to do. */
bool kick();
/** A fetch round is running now (kicked or daily). */
bool fetching();
/** The Retry-After header ("120" or an HTTP date) → seconds from `now`; 0 = absent / unreadable / already past. */
long parseRetryAfter(const std::string& v, std::time_t now);

constexpr const char* kUrl = "https://vibeserver.vibesdr.net/api/sats";
constexpr long kCheckSec = 24 * 3600;          // daily, like EiBi (the mirror rebuilds every 6 h; elements < 3 d = fresh)
constexpr long kCheckJitterSec = 4 * 3600;     // ★ per machine, stable: a fleet does not check in at the same hour
constexpr long kMinAttemptSec = 3600;          // never more often than this from one machine, success or not
constexpr long kBackoffSec = 3600;             // after a failure, doubling to a day
constexpr long kHoldFloorSec = 15 * 60;        // ★★★ no hold is shorter, whatever the configuration (CelesTrak's rule)
constexpr long kResumeJitterSec = 3600;        // ★★ + a random 0–60 min on every hold: a fleet does not return together
// ★★ V12 SATELLITE MODE item 9 — a machine with NO copy at all (nothing to show; the on-open fetch): the hold after a
//    failure starts at the floor (15 min) and doubles to the usual hour, + a random 0–5 min; and its attempts may come
//    this close together. A machine WITH a copy keeps the rules above unchanged (1 h doubling to a day, + 0–60 min).
constexpr long kNoCopyMinAttemptSec = kHoldFloorSec;
constexpr long kNoCopyJitterSec = 5 * 60;
constexpr long kMaxRetryAfterSec = 86400;      // ★ a server's Retry-After is honoured (never shorter than the floor), up to a day
constexpr size_t kMaxBytes = 4u * 1024 * 1024; // sats.json is ~20 KB; 4 MB is not one

// ── Test seams ──
struct Request { std::string url, dest, ifNoneMatch, ifModifiedSince; };
/** code 0 = no answer (network, timeout). retryAfter: seconds the server asked for (Retry-After), 0 = none. */
struct Response { int code = 0; std::string etag, lastModified, msg; long retryAfter = 0; };
using Fetcher = std::function<void(const Request&, Response&)>;
void setFetcher(Fetcher fn);                    // empty = curl
void setClock(std::function<std::time_t()> fn); // empty = the real clock
void setRandom(std::function<uint32_t()> fn);   // empty = std::random_device
std::string userAgent();

}  // namespace vssat
