// test-satcat — V12 satellite engine: the CelesTrak GP cache (satcat.cpp; docs/v12/SATELLITE-ENGINE.md §6). A fake
// fetcher and a fake clock: nothing touches the network.
//  · first start with no cache fetches every group, caches and publishes; a second process sees the fresh cache
//  · the cache's AGE decides — a restart does not fetch; 12 h later it does
//  · one attempt per machine per 2 h, success or not (the marker is shared)
//  · a non-200 STOPS the round (later groups not asked) and backs off 6 h → 12 h → 24 h; the cache is kept
//  · a 200 that is not GP JSON (a captive portal, "No GP data found") never replaces the cache
//  · an oversized answer is refused; the cache is published at start-up, offline
#include "satcat.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::time_t g_now = 1795600000;
static std::map<std::string, int> g_asked;
static std::string g_mode = "ok";   // ok | refuse | portal | huge | refuse-weather
static std::map<std::string, std::string> g_pub;
static std::string body(const std::string& g) {
    return "[{\"OBJECT_NAME\":\"X-" + g + "\",\"NORAD_CAT_ID\":25544,\"MEAN_MOTION\":15.5,\"EPOCH\":\"2026-11-19T06:28:00\"}]";
}

static vssat::Fetch fake(const std::string& url, const std::string& dest, std::string& msg) {
    const std::string g = url.substr(url.find("GROUP=") + 6, url.find('&', url.find("GROUP=")) - url.find("GROUP=") - 6);
    g_asked[g]++;
    if (g_mode == "refuse" || (g_mode == "refuse-weather" && g == "weather")) { msg = "curl: (22) The requested URL returned error: 403"; return vssat::Fetch::Refused; }
    std::ofstream f(dest, std::ios::binary);
    if (g_mode == "portal") f << "<html>Please sign in to the hotel Wi-Fi</html>";
    else if (g_mode == "huge") { std::string s(3u * 1024 * 1024, ' '); s[0] = '['; f << s << "\"NORAD_CAT_ID\" \"MEAN_MOTION\"]"; }
    else f << body(g);
    return vssat::Fetch::Ok;
}

int main() {
    char tmpl[] = "/tmp/test-satcat-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) return 3;
    g_now = std::time(nullptr);   // ★ the cache's age is its file time, so the fake clock starts at the real one
    vssat::setDir(dir);
    vssat::setFetcher(fake);
    vssat::setClock([] { return g_now; });
    vssat::setPublisher([](const std::string& g, const std::string& j) { g_pub[g] = j; });

    std::printf("§1 first start, no cache\n");
    CHECK(vssat::loadFromCache() == 0, "nothing cached");
    CHECK(vssat::needsRefresh(), "no cache → fetch");
    std::string err;
    CHECK(vssat::refresh(err) == 2 && err.empty(), "both groups fetched (%s)", err.c_str());
    CHECK(g_pub.count("amateur") && g_pub.count("weather"), "both published");
    CHECK(!vssat::needsRefresh(), "fresh cache → no fetch");
    CHECK(vssat::urlFor("amateur") == "https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=json", "the GP URL (OMM JSON)");

    std::printf("§2 restart, age, the machine-wide marker\n");
    g_pub.clear();
    CHECK(vssat::loadFromCache() == 2 && g_pub.size() == 2, "a restart publishes the cache, offline");
    g_now += 3 * 3600;
    CHECK(!vssat::needsRefresh(), "3 h later: the cache is young, no fetch");
    g_now += 10 * 3600;   // 13 h after the fetch
    CHECK(vssat::needsRefresh(), "13 h old → fetch");

    std::printf("§3 a refusal stops the round and backs off\n");
    g_mode = "refuse"; g_asked.clear();
    CHECK(vssat::refresh(err) == 0 && err.find("403") != std::string::npos, "refused (%s)", err.c_str());
    CHECK(g_asked["amateur"] == 1 && g_asked["weather"] == 0, "the round STOPPED at the first refusal");
    std::string a1; { std::ifstream f(std::string(dir) + "/sat/gp-amateur.json"); std::stringstream ss; ss << f.rdbuf(); a1 = ss.str(); }
    CHECK(a1 == body("amateur"), "the cache kept");
    g_now += 3 * 3600;
    CHECK(!vssat::needsRefresh(), "3 h after a refusal: still backing off");
    g_now += 4 * 3600;
    CHECK(vssat::needsRefresh(), "7 h after: allowed again");
    vssat::refresh(err);
    g_now += 7 * 3600;
    CHECK(!vssat::needsRefresh(), "second refusal: 12 h backoff (7 h is not enough)");
    g_now += 6 * 3600;
    CHECK(vssat::needsRefresh(), "13 h after the second: allowed");
    CHECK(vssat::statusJson().find("\"failures\":2") != std::string::npos, "status counts failures: %s", vssat::statusJson().c_str());

    std::printf("§4 a 200 that is not the data\n");
    g_mode = "portal";
    CHECK(vssat::refresh(err) == 0 && !err.empty(), "a captive portal page is refused (%s)", err.c_str());
    { std::ifstream f(std::string(dir) + "/sat/gp-amateur.json"); std::stringstream ss; ss << f.rdbuf(); CHECK(ss.str() == body("amateur"), "cache kept after the portal"); }
    g_now += 25 * 3600; g_mode = "huge";
    CHECK(vssat::refresh(err) == 0, "an oversized answer is refused (%s)", err.c_str());
    g_now += 25 * 3600; g_mode = "ok"; g_pub.clear();
    CHECK(vssat::refresh(err) == 2 && g_pub.size() == 2, "recovers (%s)", err.c_str());
    CHECK(vssat::statusJson().find("\"failures\":0") != std::string::npos, "a success clears the backoff");
    g_now += 600;
    CHECK(!vssat::needsRefresh(), "and the 2 h minimum holds after a success");

    std::string cmd = std::string("rm -rf '") + dir + "'";
    if (std::system(cmd.c_str()) != 0) {}
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
