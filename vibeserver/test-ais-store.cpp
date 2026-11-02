// test-ais-store.cpp — V12 2026-11-02 THE SHIP STORE (vibe_ais_store.h) for the SHIPPING MAP, docs/v12/SHIPPING-MAP.md.
//
//  1. A ship round trip: the state and the track through the 512-byte record, the JSON in the live report's shape.
//  2. The merge: static fields kept from older reports, the newest position wins, two radios' tracks in time order.
//  3. BOUNDED: the slots a gallery size affords; a full store gives the longest-unheard slot to a newcomer; the file
//     never grows past header + directory + cap records; a smaller gallery starts the store again smaller; size 0 removes it.
//  4. The read filters: since, range from the receiver, limit (newest first), tracks only when asked.
//  5. FOUR PROCESSES (four radios) writing at once, overlapping ships, under the gallery's flock: one valid file, every
//     MMSI once, the shared ships carrying every radio's points.
//  6. Inside the owner's gallery size: usedBytes() counts the store; fit() evicts items to make room for it.
//  7. The AIS service hands over only what changed, once, and again only when the ship is heard again.
#include "vibe_ais_store.h"
#include "vibe_gallery.h"
#include "vibe_bgdecode_config.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace st = vibeais::store;

static std::string tmpDir(const char* tag) {
    char b[256]; std::snprintf(b, sizeof b, "/tmp/vibe-ais-store-%s-%d", tag, (int)::getpid());
    std::string cmd = std::string("rm -rf ") + b; if (std::system(cmd.c_str())) {}
    ::mkdir(b, 0755);
    return b;
}
static void rmDir(const std::string& d) { std::string cmd = "rm -rf " + d; if (std::system(cmd.c_str())) {} }

static st::Rec ship(uint32_t mmsi, uint32_t seen, double lat, double lon, const char* name = "") {
    st::Rec r; r.mmsi = mmsi; r.seen = seen; r.first = seen; r.posAt = seen; r.lat = st::e7(lat); r.lon = st::e7(lon);
    r.sog10 = 105; r.cog10 = 2450; r.hdg = 246; r.stype = 70; r.kind = 0;
    st::putStr(r.name, sizeof r.name, name);
    r.nTrk = 1; r.trk[0] = { seen, st::e7(lat), st::e7(lon) };
    return r;
}
static uint64_t fsize(const std::string& p) { struct stat s{}; return ::stat(p.c_str(), &s) == 0 ? (uint64_t)s.st_size : 0; }

static void testRoundTrip() {
    std::printf("── a ship round trip ──\n");
    const std::string d = tmpDir("rt");
    vibeais::Vessel v; v.mmsi = 235012345; v.kind = "A"; v.name = "WILSON GOOLE"; v.callsign = "MXYZ7"; v.dest = "ROTTERDAM";
    v.shipType = 70; v.nav = 0; v.heading = 246; v.lat = 50.8; v.lon = -1.3; v.sog = 12.3; v.cog = 245.6; v.bow = 100; v.stern = 20; v.port = 10; v.stbd = 10;
    v.first = 10; v.last = 100; v.posAt = 100; v.count = 7; v.etaMonth = 11; v.etaDay = 3; v.etaHour = 14; v.etaMin = 30;
    for (int i = 0; i < 100; ++i) v.trail.push_back({ 10.0 + i * 60.0 / 1.0 * 0.5, 50.7 + i * 0.001, -1.3 });   // every 30 s, 111 m apart
    const int64_t wall = 1'800'000'000;
    st::Rec r = st::fromVessel(v, 100.0, wall);
    CHECK(r.seen == wall && r.first == wall - 90, "times to UTC: seen %u first %u", r.seen, r.first);
    CHECK(r.nTrk > 0 && r.nTrk <= st::kTrack, "the track thinned to at most %d points (%d)", st::kTrack, r.nTrk);
    bool spaced = true;
    for (int i = 1; i < r.nTrk; ++i) if (r.trk[i].t - r.trk[i - 1].t < st::kTrackMinSec && st::metres(r.trk[i - 1], st::de7(r.trk[i].lat), st::de7(r.trk[i].lon)) < st::kTrackMinM) spaced = false;
    CHECK(spaced, "every kept point is 2 min or 300 m from the last");
    CHECK(st::write(d, 100ull << 20, { r }) == 1, "written");
    st::Query q; q.tracks = true;
    size_t total = 0;
    const auto got = st::read(d, q, &total);
    CHECK(got.size() == 1 && total == 1 && got[0].mmsi == 235012345, "read back one ship");
    const std::string j = st::toJson(got, wall + 60, true, total);
    CHECK(j.find("\"name\":\"WILSON GOOLE\"") != std::string::npos && j.find("\"dest\":\"ROTTERDAM\"") != std::string::npos
          && j.find("\"family\":\"cargo\"") != std::string::npos && j.find("\"flag\":\"GB\"") != std::string::npos, "the live report's fields: %s", j.substr(0, 300).c_str());
    CHECK(j.find("\"seen\":60") != std::string::npos && j.find("\"lat\":50.80000") != std::string::npos && j.find("\"len\":120") != std::string::npos, "ages from now, position, size");
    CHECK(j.find("\"eta\":\"11-03 14:30\"") != std::string::npos && j.find("\"stored\":1") != std::string::npos && j.find("\"pts\":[[") != std::string::npos, "ETA, stored, the track");
    CHECK(j.find("WILSON GOOLE \xc2\xb7 cargo \xc2\xb7 12 kn SW \xe2\x86\x92 ROTTERDAM") != std::string::npos, "the readable line, as the live list");
    q.tracks = false;
    CHECK(st::toJson(st::read(d, q), wall, false, 1).find("\"pts\"") == std::string::npos, "no track unless asked");
    rmDir(d);
}

static void testMerge() {
    std::printf("── the merge ──\n");
    st::Rec a = ship(1, 100, 50, -1, "OLD NAME");
    st::putStr(a.dest, sizeof a.dest, "LE HAVRE"); a.imo = 9134270; a.len = 120;
    st::Rec b = ship(1, 200, 50.1, -1.1);      // a position report: no name
    b.trk[0] = { 200, st::e7(50.1), st::e7(-1.1) };
    st::merge(a, b);
    CHECK(a.seen == 200 && st::getStr(a.name, sizeof a.name) == "OLD NAME" && st::getStr(a.dest, sizeof a.dest) == "LE HAVRE" && a.imo == 9134270 && a.len == 120,
          "the newer report wins, the static fields it lacks are kept");
    CHECK(std::fabs(st::de7(a.lat) - 50.1) < 1e-6, "the newest position");
    CHECK(a.nTrk == 2 && a.trk[0].t == 100 && a.trk[1].t == 200, "both points, in time order");
    st::Rec c = ship(1, 150, 49, 0);           // an OLDER report from another radio
    c.trk[0] = { 150, st::e7(49.9), st::e7(-1.05) };
    st::merge(a, c);
    CHECK(a.seen == 200 && std::fabs(st::de7(a.lat) - 50.1) < 1e-6, "an older report does not move the ship back");
    CHECK(a.nTrk == 3 && a.trk[1].t == 150, "…but its track point goes in between");
    st::Rec dup = ship(1, 210, 50.1, -1.1); dup.trk[0] = { 210, st::e7(50.1), st::e7(-1.1) };
    st::merge(a, dup);
    CHECK(a.nTrk == 3, "the same fix 10 s later (another radio) is one point");
}

static void testBounded() {
    std::printf("── bounded ──\n");
    CHECK(st::capFor(100ull << 20) == 2048 && st::capFor(10ull << 20) == 640 && st::capFor(1ull << 20) == 64 && st::capFor(1) == st::kMinCap,
          "slots: 100 MB → 2048, 10 MB → 640, 1 MB → 64, tiny → %u", st::kMinCap);
    const std::string d = tmpDir("bound");
    const uint64_t lim = 1ull << 20;   // 64 slots
    for (uint32_t i = 0; i < 200; ++i) st::write(d, lim, { ship(1000 + i, 1000 + i, 50 + i * 0.001, -1) });
    st::Query q;
    size_t total = 0;
    auto all = st::read(d, q, &total);
    CHECK(all.size() == 64 && total == 64, "200 ships into 64 slots: 64 kept (%zu)", all.size());
    std::set<uint32_t> m; for (auto& r : all) m.insert(r.mmsi);
    CHECK(*m.begin() == 1136 && *m.rbegin() == 1199, "the newest 64 kept, the longest unheard gave way (%u…%u)", *m.begin(), *m.rbegin());
    CHECK(fsize(d + "/" + st::kFile) == st::fileBytesFor(64, 64), "the file is header + directory + 64 records exactly (%llu)", (unsigned long long)fsize(d + "/" + st::kFile));
    // a batch bigger than the store: never more than cap
    std::vector<st::Rec> big; for (uint32_t i = 0; i < 500; ++i) big.push_back(ship(5000 + i, 9000 + i, 51, 1));
    st::write(d, lim, big);
    CHECK(st::read(d, q).size() == 64 && fsize(d + "/" + st::kFile) == st::fileBytesFor(64, 64), "a 500-ship batch: still 64 records, the same file size");
    // a ship heard again is not a new slot
    const auto before = fsize(d + "/" + st::kFile);
    for (int k = 0; k < 50; ++k) st::write(d, lim, { ship(5499, 20000 + k, 51, 1) });
    CHECK(fsize(d + "/" + st::kFile) == before && st::read(d, q).size() == 64, "the same ship 50 times: one slot");
    // the owner shrinks the gallery: the store starts again smaller
    st::write(d, 64ull << 10, { ship(7, 30000, 50, 0) });   // 64 KB → 32 slots
    CHECK(fsize(d + "/" + st::kFile) == st::fileBytesFor(32, 1) && st::read(d, q).size() == 1, "a smaller gallery: the store restarts at 32 slots");
    // the gallery keeps nothing: the store goes
    st::write(d, 0, { ship(8, 30001, 50, 0) });
    CHECK(fsize(d + "/" + st::kFile) == 0 && st::read(d, q).empty(), "gallery size 0: the store is removed and nothing is written");
    // a corrupt file is started again, never trusted
    { FILE* f = std::fopen((d + "/" + st::kFile).c_str(), "w"); std::fputs("garbage garbage garbage garbage garbage garbage garbage garbage garbage", f); std::fclose(f); }
    CHECK(st::read(d, q).empty(), "a corrupt file reads as empty");
    st::write(d, lim, { ship(9, 30002, 50, 0) });
    CHECK(st::read(d, q).size() == 1, "…and the next write starts it again");
    rmDir(d);
}

static void testFilters() {
    std::printf("── the read filters ──\n");
    const std::string d = tmpDir("filt");
    std::vector<st::Rec> v;
    v.push_back(ship(1, 1000, 50.80, -1.30, "NEAR NEW"));    // ~0 km
    v.push_back(ship(2, 400, 50.80, -1.30, "NEAR OLD"));
    v.push_back(ship(3, 990, 51.80, -1.30, "FAR"));          // ~111 km
    st::Rec np = ship(4, 995, 0, 0, "NO POSITION"); np.lat = st::kNoPos; np.lon = st::kNoPos; np.nTrk = 0; v.push_back(np);
    st::write(d, 100ull << 20, v);
    st::Query q; q.sinceUtc = 900;
    auto r = st::read(d, q);
    CHECK(r.size() == 3 && r[0].mmsi == 1 && r[1].mmsi == 4 && r[2].mmsi == 3, "since: the three heard after 900, newest first");
    q.rangeKm = 50; q.rxLat = 50.8; q.rxLon = -1.3;
    r = st::read(d, q);
    CHECK(r.size() == 1 && r[0].mmsi == 1, "range 50 km: only the near ship (no position = not in range)");
    q = st::Query(); q.limit = 2;
    size_t total = 0;
    r = st::read(d, q, &total);
    CHECK(r.size() == 2 && total == 4 && r[0].mmsi == 1, "limit 2 of 4, total counted");
    rmDir(d);
}

static void testProcesses() {
    std::printf("── four radios (processes) at once, under the gallery's lock ──\n");
    const std::string d = tmpDir("proc");
    const uint64_t lim = 100ull << 20;
    const int kProcs = 4, kRounds = 25;
    std::vector<pid_t> kids;
    for (int p = 0; p < kProcs; ++p) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            for (int round = 0; round < kRounds; ++round) {
                std::vector<st::Rec> batch;
                // ships 0..99 heard by every radio (the shared coast), 100 more each its own
                for (uint32_t i = 0; i < 100; ++i) {
                    st::Rec r = ship(200000 + i, 10000 + round * 600 + p * 7, 50 + i * 0.01, -1 + round * 0.01);
                    r.trk[0] = { r.seen, r.lat, r.lon };
                    batch.push_back(r);
                }
                for (uint32_t i = 0; i < 100; ++i) batch.push_back(ship(300000 + p * 1000 + i, 10000 + round * 600, 52, 1));
                st::write(d, lim, batch);
            }
            ::_exit(0);
        }
        kids.push_back(pid);
    }
    int okKids = 0;
    for (pid_t k : kids) { int stt = 0; ::waitpid(k, &stt, 0); if (WIFEXITED(stt) && WEXITSTATUS(stt) == 0) ++okKids; }
    CHECK(okKids == kProcs, "all %d writers finished", kProcs);
    st::Query q; q.tracks = true;
    size_t total = 0;
    const auto all = st::read(d, q, &total);
    std::set<uint32_t> m; for (auto& r : all) m.insert(r.mmsi);
    CHECK(all.size() == 100 + 100 * kProcs && m.size() == all.size(), "%zu ships, each MMSI once (want %d)", all.size(), 100 + 100 * kProcs);
    bool merged = true; int minPts = 99;
    for (auto& r : all) if (r.mmsi < 300000) { minPts = std::min(minPts, (int)r.nTrk); if (r.nTrk < 10) merged = false; }
    CHECK(merged, "a shared ship carries points from every round and radio (fewest %d)", minPts);
    bool ordered = true;
    for (auto& r : all) for (int i = 1; i < r.nTrk; ++i) if (r.trk[i].t <= r.trk[i - 1].t) ordered = false;
    CHECK(ordered, "every track in time order");
    CHECK(fsize(d + "/" + st::kFile) == st::fileBytesFor(2048, 100 + 100 * kProcs), "the file is exactly header + directory + %d records", 100 + 100 * kProcs);
    rmDir(d);
}

static void testGallery() {
    std::printf("── inside the owner's gallery size ──\n");
    const std::string d = tmpDir("gal");
    vibe::gallery::Store g;
    g.setDir(d);
    const uint64_t lim = 1ull << 20;   // 1 MB: 64 ship slots = 32 KB + directory
    g.setLimitBytes(lim);
    vibe::gallery::Meta m; m.kind = "navtex"; m.title = "test";
    for (int i = 0; i < 80; ++i) g.add(m, "txt", std::string(15000, 'x'));   // the gallery full of NAVTEX
    const uint64_t before = g.usedBytes();
    std::vector<st::Rec> v; for (uint32_t i = 0; i < 64; ++i) v.push_back(ship(1 + i, 100 + i, 50, 0));
    st::write(d, g.limitBytes(), v);
    const uint64_t side = st::bytesOnDisk(d);
    CHECK(vibe::gallery::Store::sideBytes(d) == side && side == st::fileBytesFor(64, 64), "sideBytes = the store's file (%llu)", (unsigned long long)side);
    CHECK(g.usedBytes() >= before + side - 1, "usedBytes counts the store");
    const size_t itemsBefore = g.itemCount();
    g.fit();
    CHECK(g.itemCount() < itemsBefore, "fit(): the oldest items make room for the store (%zu → %zu)", itemsBefore, g.itemCount());
    CHECK(g.usedBytes() <= lim, "…and the gallery fits the owner's space — %llu of %llu", (unsigned long long)g.usedBytes(), (unsigned long long)lim);
    CHECK(st::read(d, st::Query()).size() == 64, "…and the ships are all still there");
    rmDir(d);
}

static void testService() {
    std::printf("── the AIS service hands over what changed ──\n");
    vibeais::Service s;
    std::vector<std::vector<vibeais::Vessel>> got;
    s.setStore([&](std::vector<vibeais::Vessel>&& v, double) { got.push_back(std::move(v)); });
    // a frame needs the decoder: build the bytes of a type 18 (class B position) by hand, M.1371 Annex 8
    auto type18 = [](uint32_t mmsi, double lat, double lon) {
        std::vector<int> b;
        auto u = [&](uint64_t v, int len) { for (int i = len - 1; i >= 0; --i) b.push_back(int((v >> i) & 1)); };
        auto sg = [&](int64_t v, int len) { u(uint64_t(v) & ((1ull << len) - 1), len); };
        u(18, 6); u(0, 2); u(mmsi, 30); u(0, 8); u(52, 10); u(1, 1);
        sg(std::lround(lon * 600000), 28); sg(std::lround(lat * 600000), 27); u(900, 12); u(511, 9); u(30, 6); u(0, 2);
        u(1, 1); u(0, 1); u(1, 1); u(1, 1); u(1, 1); u(0, 1); u(1, 1); u(0, 20);
        std::vector<uint8_t> o((b.size() + 7) / 8, 0);
        for (size_t k = 0; k < b.size(); ++k) if (b[k]) o[k / 8] |= uint8_t(1 << (7 - k % 8));
        vibeais::RawFrame f; f.bytes = o; f.levelDb = -30; f.snrDb = 20;
        return f;
    };
    s.acceptForTest(0, type18(235000001, 50.8, -1.3));
    s.acceptForTest(1, type18(235000002, 50.9, -1.2));
    s.flushStoreForTest();
    CHECK(got.size() == 1 && got[0].size() == 2, "two ships handed over (%zu)", got.empty() ? 0 : got[0].size());
    s.flushStoreForTest();
    CHECK(got.size() == 1, "nothing changed: nothing handed over");
    s.acceptForTest(0, type18(235000001, 50.801, -1.301));
    s.flushStoreForTest();
    CHECK(got.size() == 2 && got[1].size() == 1 && got[1][0].mmsi == 235000001 && got[1][0].trail.size() == 1, "heard again: that one, with only its new point");
    // the whole path: into a store and back
    const std::string d = tmpDir("svc");
    const int64_t wall = vibe::gallery::nowUtc();
    std::vector<st::Rec> recs; for (auto& v : got[0]) recs.push_back(st::fromVessel(v, 0, wall));
    st::write(d, 100ull << 20, recs);
    CHECK(st::read(d, st::Query()).size() == 2, "into the store: two ships");
    rmDir(d);
}

static void testProfile() {
    std::printf("── an AIS background profile (vibe_bgdecode_config.h) ──\n");
    namespace bg = vibe::bg;
    bg::Config c; std::string err;
    const std::string js = "{\"on\":true,\"waitMin\":5,\"profiles\":[{\"centre\":0,\"from\":\"00:00\",\"to\":\"00:00\",\"vfos\":[],\"ais\":true}]}";
    CHECK(bg::parse(js, c, err) && c.profiles.size() == 1 && c.profiles[0].isAis() && c.profiles[0].isWhole() && !c.profiles[0].isIsm(), "parses: profile 1 is AIS");
    CHECK(bg::toJson(c) == js, "round-trips: %s", bg::toJson(c).c_str());
    CHECK(bg::validate(c, 2.4e6, err), "valid on an RTL at 2.4 MS/s (no centre, no VFOs): %s", err.c_str());
    CHECK(!bg::validate(c, 2.4e6, err, 0, 0, true, true, /*aisOk=*/false) && err.find("cannot tune to 162 MHz") != std::string::npos, "an HF+ / Icom: refused — %s", err.c_str());
    CHECK(!bg::validate(c, 2.4e6, err, 0, 162e6) && err.find("locked") != std::string::npos, "a locked radio: refused — %s", err.c_str());
    CHECK(!bg::validate(c, 48e3, err) && err.find("96 kS/s") != std::string::npos, "48 kS/s: refused — %s", err.c_str());
    // Stuart's case: NAVTEX by day on one profile, AIS by night on another — and "ism" wins over a stray "ais"
    bg::Config two;
    bg::parse("{\"on\":true,\"profiles\":[{\"centre\":500000,\"from\":\"06:00\",\"to\":\"22:00\",\"vfos\":[{\"hz\":517500,\"mode\":\"usb\",\"dec\":\"navtex\"}]},"
              "{\"from\":\"22:00\",\"to\":\"06:00\",\"ais\":true}]}", two, err);
    CHECK(bg::validate(two, 2.4e6, err) && bg::activeProfile(two, 23 * 60) == 1 && two.profiles[1].isAis(), "NAVTEX by day + AIS by night: valid, AIS at 23:00");
    bg::Config both; bg::parse("{\"on\":true,\"profiles\":[{\"ism\":\"433\",\"ais\":true}]}", both, err);
    CHECK(both.profiles[0].isIsm() && !both.profiles[0].isAis() && bg::toJson(both).find("\"ais\"") == std::string::npos, "a profile cannot be ISM and AIS: ISM it is");
    bg::Config plain; bg::parse("{\"on\":true,\"waitMin\":5,\"profiles\":[{\"centre\":500000,\"from\":\"00:00\",\"to\":\"00:00\",\"vfos\":[{\"hz\":489500,\"mode\":\"usb\",\"dec\":\"navtex\"}]}]}", plain, err);
    CHECK(bg::toJson(plain).find("ais") == std::string::npos, "an ordinary profile writes no \"ais\" key (older settings read back unchanged)");
}

int main() {
    std::printf("test-ais-store — the ship store for the SHIPPING MAP\n");
    testProfile();
    testRoundTrip();
    testMerge();
    testBounded();
    testFilters();
    testProcesses();
    testGallery();
    testService();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
