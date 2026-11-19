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
static std::string satsBody() {
    std::string b = readFile("vibeserver/testdata-sat/sats-2026-11-19.json");
    return b;
}
static void fake(const vssat::Request& rq, vssat::Response& rs) {
    ++g_calls; g_last = rq;
    g_onDiskDuring = readFile(g_dir + "/sat/sync-state");
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
    g_now += 24 * 3600 - 3 * 3600 - seed - 60;
    CHECK(!vssat::needsRefresh(), "a minute before its daily time: no ask");
    g_now += 120;
    CHECK(vssat::needsRefresh(), "its daily time: ask");

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

    std::string cmd = std::string("rm -rf '") + dir + "'";
    if (std::system(cmd.c_str()) != 0) {}
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
