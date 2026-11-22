// test-satcat — V12 satellite engine: sats.json synced from vibesdr.net like the EiBi schedule (satcat.cpp;
// docs/v12/SATELLITE-ENGINE.md §6). A fake fetcher, a fake clock and a fake random source: nothing touches the network.
//  · the URL is vibesdr.net's (never CelesTrak), a plain VibeServer User-Agent; the copy is published at start-up
//  · daily by the copy's last check (never uptime), with this machine's own 0–4 h offset; a conditional GET → 304
//  · ★★★ a 5xx / a network failure / a timeout ENDS the round: ONE request, no retry; the hold (≥ 15 min, 1 h doubling
//    to a day, + a random 0–60 min) is on disk BEFORE the request returns and after it; the owner's words
//  · another process on the machine: the shared state stops it; the lock held = it asks nothing; it picks the new copy up
//  · a 200 that is not sats.json (the engine's own parse) never replaces the copy; a 4xx is reported and held
//  · old direct-CelesTrak caches (gp-<group>.json) are published only while there is no sats.json
//  · a clock that jumped back does not hold for ever
#include "satcat.h"
#include "vibe_sat_engine.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::time_t g_now = 0;
static uint32_t g_rand = 0;
static int g_calls = 0, g_published = 0, g_legacy = 0;
static std::string g_mode = "ok";   // ok | 304 | 503 | 500 | net | timeout | portal | 404 | bad
static vssat::Request g_last;
static std::string g_dir, g_pub;
static std::string g_onDiskDuring;   // the state file as the fetcher saw it (the hold must be there already)

static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static long long stateNum(const std::string& st, const char* k) {
    const std::string key = std::string("\n") + k + " ";
    const std::string s = "\n" + st;
    const size_t a = s.find(key);
    return a == std::string::npos ? -1 : std::atoll(s.c_str() + a + key.size());
}
static long g_dataAge = 3600;   // ★ beta91 how old the fake mirror's orbits are when it answers (its "fetched" times)
static bool g_noOrbitTimes = false;   // ★ an older mirror's copy: no "orbits" block, so its age is unknown
static std::string satsBody() {
    std::string b = readFile("vibeserver/testdata-sat/sats-2026-11-19.json");
    char iso[40]; const std::time_t t = g_now - g_dataAge; std::tm tm{}; gmtime_r(&t, &tm);
    std::strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    for (size_t at = b.find("2026-11-19T18:00:00.000Z"); at != std::string::npos; at = b.find("2026-11-19T18:00:00.000Z", at + 1))
        if (at < b.find("\"satellites\"")) b.replace(at, 24, iso);
    if (g_noOrbitTimes) for (size_t o = b.find("\"orbits\""); o != std::string::npos; o = b.find("\"orbits\"", o + 8)) b.replace(o, 8, "\"orbitz\"");
    return b;
}
static long g_retryAfter = 0;   // ★ the fake mirror's Retry-After (s), 0 = none
static void fake(const vssat::Request& rq, vssat::Response& rs) {
    ++g_calls; g_last = rq;
    g_onDiskDuring = readFile(g_dir + "/sat/sync-state");
    rs.retryAfter = g_retryAfter;
    if (g_mode == "503" || g_mode == "500" || g_mode == "404") { rs.code = std::atoi(g_mode.c_str()); return; }
    if (g_mode == "net") { rs.code = 0; rs.msg = "curl: (7) Failed to connect to vibeserver.vibesdr.net port 443"; return; }
    if (g_mode == "timeout") { rs.code = 0; rs.msg = "curl: (28) Operation timed out after 45001 milliseconds"; return; }
    if (g_mode == "304") { rs.code = rq.ifNoneMatch == "\"abc\"" ? 304 : 200; if (rs.code == 304) return; }
    std::ofstream f(rq.dest, std::ios::binary);
    if (g_mode == "portal") f << "<html>Please sign in to the hotel Wi-Fi</html>";
    else if (g_mode == "bad") f << "{\"format\":2,\"satellites\":[]}";
    else f << satsBody();
    rs.code = 200; rs.etag = "\"abc\""; rs.lastModified = "Thu, 19 Nov 2026 18:00:00 GMT";
}

int main() {
    char tmpl[] = "/tmp/test-satcat-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) return 3;
    g_dir = dir;
    g_now = std::time(nullptr);   // ★ the copy's mtime is real, so the fake clock starts at the real one
    vssat::setDir(dir);
    vssat::setFetcher(fake);
    vssat::setClock([] { return g_now; });
    vssat::setRandom([] { return g_rand; });
    vssat::setPublisher([](const std::string& j) { ++g_published; g_pub = j; });
    vssat::setLegacyPublisher([](const std::string&, const std::string&) { ++g_legacy; });
    vssat::setValidator([](const std::string& body, std::string& err) {
        std::vector<vibe::sat::SatDef> d; std::vector<vibe::sat::Elements> e; std::vector<bool> h; vibe::sat::SatsMeta m;
        return vibe::sat::parseSats(body, d, e, h, m, err);
    });
    static_assert(vssat::kHoldFloorSec >= 15 * 60, "CelesTrak's floor: never under 15 minutes");
    static_assert(vssat::kBackoffSec >= vssat::kHoldFloorSec, "the first back-off is above the floor");
    static_assert(vssat::kResumeJitterSec >= 3600, "a fleet's return spread over at least an hour");

    {   // ★★ beta91: a sats.json with ORBCOMM's label-only birds (mode "label") passes the same dry parse
        std::vector<vibe::sat::SatDef> d; std::vector<vibe::sat::Elements> e; std::vector<bool> h; vibe::sat::SatsMeta m; std::string why;
        vibe::sat::LabelSet ls;
        const std::string orb = readFile("vibeserver/testdata-sat/sats-orbcomm-SYNTHETIC-2026-11-22.json");
        CHECK(vibe::sat::parseSats(orb, d, e, h, m, why) && !d.empty(), "beta91: the validator accepts a sats.json with ORBCOMM label birds (%s)", why.c_str());
        CHECK(vibe::sat::parseSats(orb, d, e, h, m, why, &ls) && ls.defs.size() == 8, "…and the engine's parse sets them apart (%zu)", ls.defs.size());
    }
    std::printf("§1 first start, no copy\n");
    CHECK(vssat::loadFromCache() == 0 && g_published == 0, "nothing cached");
    CHECK(vssat::needsRefresh(), "no copy → ask");
    std::string err;
    CHECK(vssat::refresh(err) == 1 && err.empty() && g_published == 1, "fetched and published (%s)", err.c_str());
    CHECK(g_last.url == "https://vibeserver.vibesdr.net/api/sats" && g_last.url.find("celestrak") == std::string::npos, "vibesdr.net's mirror — never CelesTrak");
    CHECK(g_last.ifNoneMatch.empty() && g_last.ifModifiedSince.empty(), "no conditional headers without a copy");
    CHECK(vssat::userAgent().rfind("VibeServer", 0) == 0 && vssat::userAgent().find('@') == std::string::npos, "User-Agent: %s", vssat::userAgent().c_str());
    CHECK(!vssat::needsRefresh(), "fresh copy → no ask");

    std::printf("§2 restart, daily, the machine's offset\n");
    g_published = 0;
    CHECK(vssat::loadFromCache() == 1 && g_published == 1, "a restart publishes the copy, offline");
    const long long seed = stateNum(readFile(g_dir + "/sat/sync-state"), "seed");
    CHECK(seed >= 0 && seed < vssat::kCheckJitterSec, "this machine's offset chosen and kept (%lld s)", seed);
    g_now += 3 * 3600;
    CHECK(!vssat::needsRefresh(), "3 h later: no ask");
    // ★ beta91: a copy whose orbits pass 12 h old is re-checked then (the mirror rebuilds every 6 h), not at its daily time
    g_now += 12 * 3600 - 3 * 3600 - g_dataAge - 60;
    CHECK(!vssat::needsRefresh(), "a minute before its orbits are 12 h old: no ask");
    g_now += 120;
    CHECK(vssat::needsRefresh(), "its orbits 12 h old: ask (not wait the day)");
    {   // ★ a copy that does not say how old its orbits are (an older mirror): the daily time, less this machine's offset
        g_noOrbitTimes = true; std::string e2;
        CHECK(vssat::refresh(e2) == 1, "a copy with no orbit times arrives");
        g_now += 24 * 3600 - seed - 60;
        CHECK(!vssat::needsRefresh(), "no orbit times: a minute before its daily time, no ask");
        g_now += 120;
        CHECK(vssat::needsRefresh(), "no orbit times: its daily time, ask");
        g_noOrbitTimes = false;
        CHECK(vssat::refresh(e2) == 1, "(a copy with orbit times again)");
        g_now += 12 * 3600;   // ...and its orbits past 12 h old, so §3 starts due, as before
        CHECK(vssat::needsRefresh(), "(due again)");
    }

    std::printf("§3 a conditional GET → 304\n");
    g_mode = "304"; g_calls = 0; g_published = 0;
    CHECK(vssat::refresh(err) == 0 && err.empty() && g_calls == 1, "304: not modified, no error (%s)", err.c_str());
    CHECK(g_last.ifNoneMatch == "\"abc\"" && g_last.ifModifiedSince == "Thu, 19 Nov 2026 18:00:00 GMT", "If-None-Match + If-Modified-Since sent");
    CHECK(g_published == 0 && !vssat::needsRefresh(), "nothing republished; checked again = a day's rest");

    std::printf("§4 ★★★ 503 — stop, hold ≥ 15 min, the hold on disk first, a fleet spread\n");
    g_now += 25 * 3600; g_mode = "503"; g_calls = 0; g_rand = 0;
    const std::string before = readFile(g_dir + "/sat/sats.json");
    CHECK(vssat::refresh(err) == 0 && g_calls == 1, "ONE request — no retry (%d)", g_calls);
    CHECK(stateNum(g_onDiskDuring, "until") >= g_now + vssat::kHoldFloorSec, "a provisional ≥ 15-min hold was on disk BEFORE the answer came back");
    long long until = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
    CHECK(until - g_now >= vssat::kHoldFloorSec && until - g_now == 3600, "hold ≥ 15 min (%lld s)", until - g_now);
    CHECK(err.find("overloaded (HTTP 503)") != std::string::npos && err.find("has stopped asking and will try again after 20") != std::string::npos, "the owner's words: %s", err.c_str());
    CHECK(vssat::ownerMessage() == err && vssat::statusJson().find("\"code\":503") != std::string::npos
          && vssat::statusJson().find("\"nextTry\":" + std::to_string(until)) != std::string::npos, "status: %s", vssat::statusJson().c_str());
    CHECK(readFile(g_dir + "/sat/sats.json") == before, "the copy kept");
    g_now += 14 * 60;
    g_calls = 0;
    CHECK(!vssat::needsRefresh() && vssat::refresh(err) == 0 && g_calls == 0, "14 min later: nothing asked, by this process or any other (the state is shared)");
    // ★ a fleet: the same failure on two machines with different random draws resumes up to an hour apart
    g_now = until + 1; g_mode = "500"; g_rand = 3599;
    vssat::refresh(err);
    until = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
    CHECK(until - g_now == 2 * 3600 + 3599, "second failure: 2 h + this machine's random 0–60 min (%lld s)", until - g_now);
    CHECK(err.find("HTTP 500") != std::string::npos, "500 the same");

    std::printf("§5 a network failure / timeout = overload\n");
    g_now = until + 1; g_mode = "timeout"; g_calls = 0; g_rand = 0;
    CHECK(vssat::refresh(err) == 0 && g_calls == 1, "one request");
    until = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
    CHECK(until - g_now == 4 * 3600 && err.find("could not be reached") != std::string::npos && err.find("timed out") != std::string::npos, "held 4 h: %s", err.c_str());
    CHECK(vssat::listenerNote() == "this server could not reach vibesdr.net for the satellite data", "the listener's few words: %s", vssat::listenerNote().c_str());
    g_now = until + 1; g_mode = "net";
    vssat::refresh(err);
    until = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
    CHECK(until - g_now == 8 * 3600 && err.find("Failed to connect") != std::string::npos, "a refused connection: 8 h (%s)", err.c_str());
    for (int k = 0; k < 6; ++k) { g_now = until + 1; vssat::refresh(err); until = stateNum(readFile(g_dir + "/sat/sync-state"), "until"); }
    CHECK(until - g_now == 86400, "doubling caps at a day (%lld s)", until - g_now);

    std::printf("§6 another radio process on the machine\n");
    g_now = until + 1; g_mode = "ok"; g_calls = 0;
    {
        const int fd = ::open((g_dir + "/sat/sync.lock").c_str(), O_CREAT | O_RDWR, 0644);
        // ★ flock is per open file description: a second descriptor in this process stands in for the other process
        CHECK(fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0, "the other process holds the lock");
        CHECK(vssat::refresh(err) == 0 && g_calls == 0, "…so this one asks nothing");
        ::flock(fd, LOCK_UN); ::close(fd);
    }
    g_published = 0;
    CHECK(vssat::refresh(err) == 1 && g_calls == 1 && g_published == 1 && err.empty(), "lock free: one request, published, the error cleared");
    CHECK(vssat::ownerMessage().empty() && vssat::statusJson().find("\"message\":\"\"") != std::string::npos, "the message clears on success");
    // the other process fetched a new copy: this one picks it up by mtime
    g_published = 0;
    CHECK(!vssat::reloadIfChanged() && g_published == 0, "unchanged: nothing republished");
    { struct utimbuf t{}; t.actime = t.modtime = std::time(nullptr) + 5; utime((g_dir + "/sat/sats.json").c_str(), &t); }
    CHECK(vssat::reloadIfChanged() && g_published == 1, "another process's new copy is published here");

    std::printf("§7 a 200 that is not sats.json; a 4xx\n");
    g_now += 25 * 3600; g_mode = "portal";
    const std::string good = readFile(g_dir + "/sat/sats.json");
    CHECK(vssat::refresh(err) == 0 && err.find("not satellite data") != std::string::npos, "a captive portal page refused (%s)", err.c_str());
    CHECK(readFile(g_dir + "/sat/sats.json") == good, "the copy kept");
    g_now += 3 * 3600; g_mode = "bad";
    CHECK(vssat::refresh(err) == 0 && err.find("unknown sats.json format") != std::string::npos, "a sats.json the engine refuses is refused (%s)", err.c_str());
    g_now += 5 * 3600; g_mode = "404";
    CHECK(vssat::refresh(err) == 0 && err.find("answered HTTP 404") != std::string::npos && err.find("will not ask again until") != std::string::npos, "%s", err.c_str());
    CHECK(readFile(g_dir + "/sat/sats.json") == good, "still kept");

    std::printf("§8 a clock that jumped back\n");
    {
        std::string st = readFile(g_dir + "/sat/sync-state");
        g_now -= 40 * 86400;   // an RTC-less Pi booted with an old clock: every stored time is far in its future
        CHECK(vssat::needsRefresh(), "a hold 40 days ahead is not honoured for ever");
        g_now += 40 * 86400;
    }

    std::printf("§9 old direct-CelesTrak caches\n");
    {
        char t2[] = "/tmp/test-satcat-legacy-XXXXXX";
        const char* d2 = mkdtemp(t2);
        std::string sd = std::string(d2) + "/sat";
        ::mkdir(sd.c_str(), 0755);
        { std::ofstream f(sd + "/gp-amateur.json"); f << "[{\"NORAD_CAT_ID\":25544,\"MEAN_MOTION\":15.5}]"; }
        vssat::setDir(d2); g_legacy = 0; g_published = 0;
        CHECK(vssat::loadFromCache() == 1 && g_legacy == 1 && g_published == 0, "no sats.json yet: the old group cache is published");
        { std::ofstream f(sd + "/sats.json"); f << satsBody(); }
        g_legacy = 0;
        CHECK(vssat::loadFromCache() == 1 && g_legacy == 0 && g_published == 1, "with sats.json: the old caches are not");
        std::system((std::string("rm -rf '") + d2 + "'").c_str());
        vssat::setDir(dir);
    }

    std::printf("§10 ★★ SATELLITE MODE item 9: NO copy at all — 15 min, 30, then hourly; Retry-After honoured\n");
    {
        char t3[] = "/tmp/test-satcat-nocopy-XXXXXX";
        const char* d3 = mkdtemp(t3);
        vssat::setDir(d3); g_dir = d3;
        g_mode = "503"; g_calls = 0; g_rand = 0; g_retryAfter = 0;
        CHECK(!vssat::haveCopy() && vssat::lastSynced() == 0, "nothing here, never synced");
        CHECK(vssat::needsRefresh() && vssat::refresh(err) == 0 && g_calls == 1, "a 503: ONE request");
        long long u = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
        CHECK(u - g_now == vssat::kHoldFloorSec, "first hold with no copy: 15 min exactly (%lld s)", u - g_now);
        CHECK(err.find("overloaded (HTTP 503)") != std::string::npos, "the owner's words: %s", err.c_str());
        g_now += 14 * 60;
        CHECK(!vssat::needsRefresh(), "14 min later: not yet");
        g_now = u + 1;
        CHECK(vssat::needsRefresh(), "after the 15 min: allowed (attempts 15 min apart with no copy, not hourly)");
        g_rand = 299;
        vssat::refresh(err);
        u = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
        CHECK(u - g_now == 30 * 60 + 299, "second: 30 min + this machine's 0–5 min (%lld s)", u - g_now);
        g_rand = 0;
        for (int k = 0; k < 3; ++k) { g_now = u + 1; vssat::refresh(err); u = stateNum(readFile(g_dir + "/sat/sync-state"), "until"); }
        CHECK(u - g_now == 3600, "then hourly, never longer while there is nothing (%lld s)", u - g_now);
        g_now = u + 1; g_retryAfter = 7200; g_calls = 0;
        vssat::refresh(err);
        u = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
        CHECK(g_calls == 1 && u - g_now == 7200, "★ the mirror's Retry-After: 2 h asked = 2 h held (%lld s)", u - g_now);
        g_now = u + 1; g_retryAfter = 60;
        vssat::refresh(err);
        u = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
        CHECK(u - g_now >= vssat::kHoldFloorSec && u - g_now == 3600, "a Retry-After under the hold never shortens it (%lld s)", u - g_now);
        g_now = u + 1; g_retryAfter = 0; g_mode = "ok"; g_published = 0;
        CHECK(vssat::refresh(err) == 1 && g_published == 1 && vssat::haveCopy() && vssat::lastSynced() == (long long)g_now,
              "then a 200: published, synced %lld", vssat::lastSynced());
        CHECK(!vssat::needsRefresh(), "and the daily cadence after it");
        // ★ with a copy the old rules stand: a 503 holds 1 h + 0–60 min, as §4
        g_now += 25 * 3600; g_mode = "503"; g_rand = 0;
        vssat::refresh(err);
        u = stateNum(readFile(g_dir + "/sat/sync-state"), "until");
        CHECK(u - g_now == 3600 && vssat::lastSynced() > 0, "with a copy: 1 h as before; the last good sync kept (%lld s)", u - g_now);
        std::system((std::string("rm -rf '") + d3 + "'").c_str());
    }

    std::printf("§11 Retry-After\n");
    CHECK(vssat::parseRetryAfter("120", 1000) == 120 && vssat::parseRetryAfter(" 900\r", 1000) == 900, "seconds");
    {
        std::tm t{}; t.tm_year = 2026 - 1900; t.tm_mon = 10; t.tm_mday = 20; t.tm_hour = 16; t.tm_min = 8;
        const std::time_t at = timegm(&t);
        CHECK(vssat::parseRetryAfter("Fri, 20 Nov 2026 16:08:00 GMT", at - 600) == 600, "an HTTP date (%ld)", vssat::parseRetryAfter("Fri, 20 Nov 2026 16:08:00 GMT", at - 600));
        CHECK(vssat::parseRetryAfter("Fri, 20 Nov 2026 16:08:00 GMT", at + 5) == 0, "a date already past: 0");
    }
    CHECK(vssat::parseRetryAfter("", 0) == 0 && vssat::parseRetryAfter("soon", 0) == 0, "absent / unreadable: 0");

    std::printf("§12 ★★ the runner and THE ON-OPEN FETCH (a real thread; waits have deadlines)\n");
    {
        char t4[] = "/tmp/test-satcat-runner-XXXXXX";
        const char* d4 = mkdtemp(t4);
        vssat::setDir(d4); g_dir = d4;
        CHECK(!vssat::kick(), "no runner yet: kick says no");
        static std::atomic<int> notes{0};
        vssat::RunnerHooks h;
        h.wanted = [] { return true; };
        h.note = [](const std::string&) { notes.fetch_add(1); };
        vssat::startDaily(h, 3600);   // ★ an hour's start-up delay: only a kick can make it ask now
        auto settle = [] { for (int i = 0; i < 100 && vssat::fetching(); ++i) usleep(50 * 1000); return !vssat::fetching(); };
        g_mode = "503"; g_calls = 0; g_rand = 0; g_retryAfter = 900;
        usleep(200 * 1000);
        CHECK(g_calls == 0, "nothing asked during the start-up delay");
        CHECK(vssat::kick() && vssat::fetching(), "kick on an empty server: fetching at once");
        CHECK(settle() && g_calls == 1 && vssat::lastSynced() == 0 && notes.load() >= 1, "the 503: one request, still never synced (%d calls)", g_calls);
        CHECK(!vssat::kick() && g_calls == 1, "inside the 15-min hold: a second open asks nothing");
        g_now = stateNum(readFile(g_dir + "/sat/sync-state"), "until") + 1; g_mode = "ok"; g_published = 0; g_retryAfter = 0;
        CHECK(vssat::kick(), "the hold over: the next open fetches");
        CHECK(settle() && g_calls == 2 && g_published == 1 && vssat::lastSynced() == (long long)g_now, "the 200: published, synced");
        CHECK(!vssat::kick() && g_calls == 2, "synced: an open asks nothing (the daily cadence rules)");
        std::system((std::string("rm -rf '") + d4 + "'").c_str());
        vssat::setDir(dir); g_dir = dir;
    }

    // ★★ beta91 AS CURRENT AS WE CAN (Stuart 11-22): data already old when it arrives → re-check in 6 h, not a day
    {
        char t5[] = "/tmp/test-satcat-stale-XXXXXX";
        const char* d5 = mkdtemp(t5);
        vssat::setDir(d5); g_dir = d5;
        g_mode = "ok"; g_calls = 0; g_rand = 0; g_retryAfter = 0; g_dataAge = 21 * 3600;   // the mirror stalled: 21 h old
        std::string err;
        CHECK(vssat::refresh(err) == 1 && stateNum(readFile(g_dir + "/sat/sync-state"), "stale") == 1, "a 200 with orbits 21 h old: kept, marked old");
        g_now += 5 * 3600 + 3500;
        CHECK(!vssat::needsRefresh(), "5 h 58 min later: not yet");
        g_now += 200;
        CHECK(vssat::needsRefresh(), "6 h later: due again (not a day)");
        g_dataAge = 3600;   // the mirror caught up
        CHECK(vssat::refresh(err) == 1 && stateNum(readFile(g_dir + "/sat/sync-state"), "stale") == 0, "fresh data (1 h old): no longer old");
        g_now += 6 * 3600 + 60;
        CHECK(!vssat::needsRefresh(), "fresh: back to the daily cadence (not due 6 h later)");
        g_now += 18 * 3600;
        CHECK(vssat::needsRefresh(), "...due a day after (less this machine's offset, here 0)");
        // ★ beta91 (Stuart 11-22, "orbits are 31h old"): "old" is judged from the copy NOW, not only the stored flag
        g_dataAge = 3600; CHECK(vssat::refresh(err) == 1 && stateNum(readFile(g_dir + "/sat/sync-state"), "stale") == 0, "a fresh copy (1 h old) arrives");
        g_now += 10 * 3600 + 3000;
        CHECK(!vssat::needsRefresh(), "10 h 50 min on (the data 11 h 50 min old): not old yet, not due");
        g_now += 1200;
        CHECK(vssat::needsRefresh(), "the copy passes 12 h old: due (6 h since the check has long gone), not a day after");
        {   // a state file written by an OLDER server: no `stale` line at all, the copy's data 31 h old, checked 1 h ago
            g_dataAge = 30 * 3600; CHECK(vssat::refresh(err) == 1, "a copy 30 h old arrives");
            std::string st = readFile(g_dir + "/sat/sync-state"), out;
            for (size_t a = 0; a < st.size();) { size_t e = st.find('\n', a); if (e == std::string::npos) e = st.size();
                const std::string ln = st.substr(a, e - a); if (ln.rfind("stale", 0) != 0) out += ln + "\n"; a = e + 1; }
            { FILE* f = std::fopen((g_dir + "/sat/sync-state").c_str(), "w"); std::fputs(out.c_str(), f); std::fclose(f); }
            CHECK(readFile(g_dir + "/sat/sync-state").find("stale") == std::string::npos, "(the state file now has no stale line, as beta90 wrote it)");
            g_now += 3600;
            CHECK(!vssat::needsRefresh(), "1 h after that check: not yet (the 6 h recheck still applies)");
            g_now += 5 * 3600 + 60;
            CHECK(vssat::needsRefresh(), "★ an older server's state file, the copy 36 h old: due 6 h after the check, not a day");
        }
        // a 304 while the copy is old: still old, so still 6 h
        g_dataAge = 15 * 3600; CHECK(vssat::refresh(err) == 1, "a copy 15 h old arrives");
        g_mode = "304"; g_now += 6 * 3600 + 60;
        CHECK(vssat::needsRefresh() && vssat::refresh(err) == 0 && stateNum(readFile(g_dir + "/sat/sync-state"), "code") == 304
              && stateNum(readFile(g_dir + "/sat/sync-state"), "stale") == 1, "6 h on, a 304: the copy is now 21 h old, still marked old");
        g_now += 6 * 3600 + 60;
        CHECK(vssat::needsRefresh(), "...so asked again 6 h after that");
        std::system((std::string("rm -rf '") + d5 + "'").c_str());
        vssat::setDir(dir); g_dir = dir; g_mode = "ok"; g_dataAge = 3600;
    }

    std::string cmd = std::string("rm -rf '") + dir + "'";
    if (std::system(cmd.c_str()) != 0) {}
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
