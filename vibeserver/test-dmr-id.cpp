// test-dmr-id.cpp — V12 beta90 DMR ID → CALLSIGN (android/app/src/main/cpp/vibe_dmr_id.h, docs/v12/DMR-ID.md).
// ★ No network: the RadioID answers are a FAKE fetcher (one real answer's shape, captured 2026-11-21, below).
//   · RadioID's per-ID answer read for the callsign only (whitelisted); another ID's result is not taken
//   · OFF by default; off = no lookups; each NEW ID asked ONCE; at most one request every 2 s; misses kept a day
//   · the cache is BOUNDED (1024, least recently used out) and so is the queue (32) — no 270 k list in RAM
//   · a failing service is backed off (1 min doubling); a block page is not an answer
//   · the DV report gains "srcCall" / "dstCall" on DMR entries only (a talkgroup is never looked up)
#include "vibe_dmr_id.h"

#include <cstdio>
#include <string>

using namespace vibe::dmrid;
static int g_pass = 0, g_fail = 0;
#define CHECK(c, what) do { if (c) { ++g_pass; std::printf("   ok   %s\n", what); } else { ++g_fail; std::printf("   FAIL %s\n", what); } } while (0)

// The shape of https://radioid.net/api/dmr/user/?id=2355254 (2026-11-21) — personal fields replaced with placeholders.
static const char* kAnswer =
    "{\"count\":1,\"page\":1,\"pages\":1,\"per_page\":200,\"results\":[{\"account_unvalidated\":0,"
    "\"account_validation_status\":\"validated\",\"callsign\":\"GI4SZW\",\"city\":\"Town\",\"country\":\"United Kingdom\","
    "\"fname\":\"Name\",\"has_valid_callsign\":\"1\",\"has_valid_email\":1,\"id\":2355254,\"lastheard\":\"Sat, 21 Nov 2026 15:48:41 GMT\","
    "\"lastmaster\":\"3180\",\"lastsource\":\"X\",\"lasttg\":\"2351\",\"name\":\"Name\",\"radio_id\":2355254,\"state\":\"Northern Ireland\",\"surname\":\"\"}]}";

int main() {
    std::printf("── RadioID's answer ──\n");
    CHECK(parseCallsign(kAnswer, 2355254) == "GI4SZW", "the callsign of THIS id");
    CHECK(parseCallsign(kAnswer, 235525).empty() && parseCallsign(kAnswer, 23552540).empty(), "another id (a prefix, a longer one) is not taken");
    CHECK(parseCallsign("{\"count\":0,\"results\":[]}", 2355254).empty(), "not registered: no callsign");
    CHECK(parseCallsign("{\"results\":[{\"callsign\":\"G4<b>\",\"id\":2355254}]}", 2355254).empty(), "a callsign that is not A–Z 0–9 / is refused");
    CHECK(parseCallsign("<html>Checking your browser…</html>", 2355254).empty(), "a block page is not an answer");
    CHECK(lookupable(2355254) && !lookupable(235101) && !lookupable(16777215) && !lookupable(0), "only 7-digit user IDs are asked (not repeaters, all-call, 0)");

    std::printf("── the lookup ──\n");
    long long now = 1'800'000'000'000LL;
    Lookup lk; lk.setManual(true); lk.setClock([&] { return now; });
    int asked = 0; std::string lastUrl; std::string reply = kAnswer;
    lk.setFetcher([&](const std::string& u) { ++asked; lastUrl = u; return reply; });
    CHECK(lk.callFor(2355254).empty() && lk.queued() == 0, "OFF by default: nothing queued");
    lk.setEnabled(true);
    CHECK(lk.callFor(2355254).empty() && lk.queued() == 1, "a new ID: not known yet, queued");
    lk.callFor(2355254); lk.callFor(2355254);
    CHECK(lk.queued() == 1, "…once, however often it is heard");
    const uint64_t g0 = lk.generation();
    CHECK(lk.step() && asked == 1 && lastUrl == "https://radioid.net/api/dmr/user/?id=2355254", "one per-ID request to RadioID's API");
    CHECK(lk.callFor(2355254) == "GI4SZW" && lk.generation() == g0 + 1, "answered: the callsign, and the report's version moves");
    lk.callFor(2355254); CHECK(!lk.step() && asked == 1, "known: never asked again (within the week)");
    // the gap
    reply = "{\"count\":0,\"results\":[]}";
    lk.callFor(2340001); lk.callFor(2340002);
    CHECK(!lk.step(), "the next request waits for the 2 s gap");
    now += kMinGapMs; CHECK(lk.step() && asked == 2, "…then goes");
    now += kMinGapMs; lk.step();
    now += 3600 * 1000LL; lk.callFor(2340001);
    CHECK(lk.queued() == 0, "an unregistered ID is not asked again for a day");
    now += 86400 * 1000LL; lk.callFor(2340001);
    CHECK(lk.queued() == 1, "…and is after a day");
    // backoff
    reply = "";
    now += kMinGapMs; lk.step();
    lk.callFor(2340003);
    now += kMinGapMs; CHECK(!lk.step(), "after a failure the service is backed off (not after 2 s)");
    now += kBackoffMinSec * 1000LL; CHECK(lk.step(), "…a minute");
    now += kBackoffMinSec * 1000LL; CHECK(!lk.step(), "…then two");
    // bounds
    reply = "{\"count\":0,\"results\":[]}";
    for (uint32_t i = 0; i < 200; ++i) lk.callFor(3000000 + i);
    CHECK(lk.queued() <= kMaxQueue, "the queue is bounded (32)");
    Lookup big; big.setManual(true); long long t2 = 0; big.setClock([&] { return t2; });
    big.setFetcher([&](const std::string& u) { const uint32_t id = (uint32_t)std::atol(u.c_str() + u.rfind('=') + 1);
        return "{\"results\":[{\"callsign\":\"M" + std::to_string(id % 10) + "AA\",\"id\":" + std::to_string(id) + "}]}"; });
    big.setEnabled(true);
    for (uint32_t i = 0; i < 3000; ++i) { big.callFor(4000000 + i); t2 += kMinGapMs; big.step(); }
    CHECK(big.size() == kMaxEntries, "the cache is bounded (1024 answers, 3000 IDs heard)");
    CHECK(big.callFor(4000000 + 2999) == "M9AA", "the most recent are kept");
    CHECK(big.callFor(4000000).empty(), "the oldest went");
    lk.setEnabled(false);
    CHECK(lk.queued() == 0 && lk.callFor(2355254).empty(), "switched off: nothing shown, nothing queued");

    std::printf("── the DV report ──\n");
    Lookup& sh = Lookup::shared(); sh.setManual(true); long long t3 = 0; sh.setClock([&] { return t3; });
    sh.setFetcher([&](const std::string&) { return std::string(kAnswer); });
    const std::string rep = "{\"type\":\"dvsig\",\"state\":\"sync\",\"pin\":\"auto\",\"amateur\":true,\"band\":\"70 cm amateur\",\"ids\":true,\"sys\":\"DMR\",\"log\":["
        "{\"id\":2,\"t\":1,\"t1\":2,\"sys\":\"DMR\",\"kind\":\"voice\",\"open\":true,\"enc\":false,\"slot\":2,\"cc\":1,\"src\":2355254,\"dst\":2351,\"grp\":true,\"gps\":{\"lat\":1.0,\"lon\":2.0}},"
        "{\"id\":1,\"t\":1,\"t1\":2,\"sys\":\"NXDN\",\"kind\":\"voice\",\"open\":false,\"enc\":false,\"ran\":1,\"mode\":\"NXDN48\",\"src\":2355254,\"dst\":77,\"grp\":true}]}";
    CHECK(sh.annotate(rep) == rep, "off: the report unchanged");
    sh.setEnabled(true);
    std::string a = sh.annotate(rep);
    CHECK(a == rep && sh.queued() == 1, "on, first sight: unchanged, the ID queued (the talkgroup never)");
    sh.step();
    a = sh.annotate(rep);
    CHECK(a.find("\"grp\":true,\"gps\":{\"lat\":1.0,\"lon\":2.0},\"srcCall\":\"GI4SZW\"}") != std::string::npos, "the DMR entry gains srcCall (after its gps object)");
    CHECK(a.find("\"NXDN48\",\"src\":2355254,\"dst\":77,\"grp\":true}") != std::string::npos, "an NXDN entry (another ID namespace) is not touched");
    CHECK(a.find("dstCall") == std::string::npos, "a talkgroup is never looked up");
    sh.setEnabled(false);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
