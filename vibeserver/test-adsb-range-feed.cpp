// test-adsb-range-feed.cpp — b96-rangefeed: THE RANGE OUTLINE COUNTS EVERY ADS-B POSITION THIS SERVER HOLDS (vibe_adsb_range_feed.h,
// docs/v12/RANGE-FEED.md). Stuart 11-27, the Lenovo: its radio has no ADS-B, its aircraft come from a server-level Beast feed,
// and GET /adsb/range answered every bin 0.
//   1. The follower's rule on hand-made table rows: the second consistent ADS-B fix counts (the first proves nothing); a
//      datalink position (VDL2 / HFDL / ADS-C) never; a jump that is not flight never; a stale fix never; the same fix twice
//      once; a row heard ONLY by this radio is left to its own service; a feed row and another radio's row count; bounded.
//   2. END TO END, one machine: a synthetic dump1090 (Beast on 127.0.0.1 — never Stuart's Pi) → the FeedSet (the front door's
//      job) → the shared table in a runtime directory, tagged "feed" → this radio process's follower → its outline's JSON
//      (/adsb/range's body) has the feed aircraft's bearing filled, around the server's location. No ADS-B live → not read.
// Silent: no radio, no audio.
#include "vibe_feeds.h"
#include "vibe_adsb_range_feed.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); ++fails; } else { std::printf("  ok    "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static bool waitFor(const std::function<bool()>& f, double secs) {
    const double end = nowS() + secs;
    while (nowS() < end) { if (f()) return true; sleepMs(50); }
    return f();
}
static std::vector<uint8_t> hexBytes(const char* h) {
    std::vector<uint8_t> o;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) { unsigned v; std::sscanf(h + i, "%2x", &v); o.push_back(uint8_t(v)); }
    return o;
}
static int listenLocal(int& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(port));
    if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 4) != 0) { ::close(fd); return -1; }
    socklen_t l = sizeof a; getsockname(fd, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
    return fd;
}
// The published worked example: DF17 even + odd airborne positions of 40621D (52.2572 N, 3.9194 E).
static const char* kPosE = "8D40621D58C382D690C8AC2863A7";
static const char* kPosO = "8D40621D58C386435CC412692AD6";

/** A synthetic dump1090: accepts readers on 127.0.0.1 and writes the even + odd pair (Beast) to each every 200 ms. */
struct FakeDump {
    int port = 0, lfd = -1; std::atomic<bool> run{true}; std::thread th; uint64_t ts = 0x1A0000001A00ull;
    void start() {
        lfd = listenLocal(port);
        th = std::thread([this] {
            std::vector<int> cs;
            while (run) {
                std::vector<uint8_t> out;
                for (const char* h : { kPosE, kPosO }) { const auto f = hexBytes(h); ts += 12000000 / 10; beast::appendFrame(out, f.data(), int(f.size()), ts, 0x40); }
                pollfd p{lfd, POLLIN, 0};
                if (::poll(&p, 1, 20) > 0) { const int c = ::accept(lfd, nullptr, nullptr); if (c >= 0) cs.push_back(c); }
                for (int c : cs) ::send(c, out.data(), out.size(), 0);
                for (int i = 0; i < 10 && run; ++i) sleepMs(20);
            }
            for (int c : cs) ::close(c);
            ::close(lfd);
        });
    }
    void stop() { run = false; if (th.joinable()) th.join(); }
};

static vibeavshare::Row adsbRow(uint32_t icao, double lat, double lon, int64_t posMs, uint32_t radios, int posSrc = vibeavshare::kAdsb) {
    vibeavshare::Row w;
    w.used = true; w.icao = icao; w.flags = 1; w.posSrc = posSrc; w.lat = lat; w.lon = lon; w.posMs = posMs; w.lastMs = posMs; w.radios = radios;
    w.srcMask = uint8_t(1u << posSrc);
    return w;
}
static int binOf(double cLat, double cLon, double lat, double lon) { return (int)std::floor(vibeadsb::RangeOutline::bearingDeg(cLat, cLon, lat, lon)); }
/** The km of bin `b` in /adsb/range's body (km array). */
static double kmAt(const std::string& j, int b) {
    size_t p = j.find("\"km\":[");
    if (p == std::string::npos) return -1;
    p += 6;
    for (int i = 0; i < b; ++i) { p = j.find(',', p); if (p == std::string::npos) return -1; ++p; }
    return std::atof(j.c_str() + p);
}

static void testRule() {
    std::printf("1. the follower's rule (hand-made table rows)\n");
    const double cLat = 52.3125, cLon = -0.875;   // the Lenovo's location (IO92nh)
    vibeadsb::RangeStore st;
    st.setCentre(cLat, cLon, 1795770000);
    vibeadsb::TableRangeFollower f;
    const uint32_t own = 1u << 0, feed = 1u << 1, other = 1u << 2;
    const int64_t t0 = 1795770000000LL;
    // VKG520 over the Humber, from the feed; 2 s later 0.25 NM on (450 kt)
    const double la = 53.70, lo = -0.30;
    CHECK(f.tick({ adsbRow(0x406A99, la, lo, t0, feed) }, own, t0 + 500, st, vibeadsb::kRangeAdsb) == 0, "a feed aircraft's FIRST fix proves nothing");
    int grew = 0;
    CHECK(f.tick({ adsbRow(0x406A99, la + 0.004, lo, t0 + 2000, feed) }, own, t0 + 2500, st, vibeadsb::kRangeAdsb, &grew) == 1 && grew == 1, "…its second consistent fix counts and grows a bearing");
    const int b = binOf(cLat, cLon, la + 0.004, lo);
    const double want = vibeadsb::RangeOutline::distKm(cLat, cLon, la + 0.004, lo);
    CHECK(std::fabs(kmAt(st.json(vibeadsb::kRangeAdsb, t0 / 1000), b) - want) < 0.11, "/adsb/range: bin %d = %.1f km (want %.1f)", b, kmAt(st.json(vibeadsb::kRangeAdsb, t0 / 1000), b), want);
    CHECK(f.tick({ adsbRow(0x406A99, la + 0.004, lo, t0 + 2000, feed) }, own, t0 + 4000, st, vibeadsb::kRangeAdsb) == 0, "the same fix read again (the table rewritten, no new position) is not offered twice");
    // another radio on this server
    f.tick({ adsbRow(0x4CA97B, 51.0, 1.0, t0, other) }, own, t0 + 100, st, vibeadsb::kRangeAdsb);
    CHECK(f.tick({ adsbRow(0x4CA97B, 51.001, 1.0, t0 + 1000, other) }, own, t0 + 1100, st, vibeadsb::kRangeAdsb) == 1, "another radio's ADS-B counts (the server's range, at the server's site)");
    // heard by this radio AND the feed (the door's loopback ingest tags its own replies "feed" too): counts, harmlessly
    f.tick({ adsbRow(0x400001, 52.0, 0.0, t0, own | feed) }, own, t0 + 100, st, vibeadsb::kRangeAdsb);
    CHECK(f.tick({ adsbRow(0x400001, 52.001, 0.0, t0 + 1000, own | feed) }, own, t0 + 1100, st, vibeadsb::kRangeAdsb) == 1, "own + feed: counted (a maximum: a repeat is harmless)");
    // ONLY this radio: its own service already offered it
    f.tick({ adsbRow(0x400002, 52.0, 1.0, t0, own) }, own, t0 + 100, st, vibeadsb::kRangeAdsb);
    CHECK(f.tick({ adsbRow(0x400002, 52.001, 1.0, t0 + 1000, own) }, own, t0 + 1100, st, vibeadsb::kRangeAdsb) == 0, "a row heard ONLY by this radio is left to its own service");
    // unknown radios (an older writer, no runtime directory): counted
    f.tick({ adsbRow(0x400003, 52.0, 2.0, t0, 0) }, own, t0 + 100, st, vibeadsb::kRangeAdsb);
    CHECK(f.tick({ adsbRow(0x400003, 52.001, 2.0, t0 + 1000, 0) }, own, t0 + 1100, st, vibeadsb::kRangeAdsb) == 1, "radios unknown (0): counted");
    // datalink positions never
    for (int s : { (int)vibeavshare::kVdl2, (int)vibeavshare::kHfdl, (int)vibeavshare::kAdsc, (int)vibeavshare::kAcars }) {
        vibeadsb::TableRangeFollower g;
        g.tick({ adsbRow(0x500000 + s, 54.0, -3.0, t0, feed, s) }, own, t0 + 100, st, vibeadsb::kRangeAdsb);
        CHECK(g.tick({ adsbRow(0x500000 + s, 54.001, -3.0, t0 + 1000, feed, s) }, own, t0 + 1100, st, vibeadsb::kRangeAdsb) == 0, "a %s position never counts (not Mode S)", vibeavshare::srcName(s));
    }
    // a jump that is not flight; a stale fix
    f.tick({ adsbRow(0x400004, 52.0, -2.0, t0, feed) }, own, t0 + 100, st, vibeadsb::kRangeAdsb);
    CHECK(f.tick({ adsbRow(0x400004, 54.0, -2.0, t0 + 2000, feed) }, own, t0 + 2100, st, vibeadsb::kRangeAdsb) == 0, "120 NM in 2 s is a bad fix, not range");
    CHECK(f.tick({ adsbRow(0x400004, 54.002, -2.0, t0 + 3000, feed) }, own, t0 + 3100, st, vibeadsb::kRangeAdsb) == 1, "…and the next good fix pairs with the last one (as the Tracker's posOk)");
    vibeadsb::TableRangeFollower h;
    h.tick({ adsbRow(0x400005, 52.0, -1.0, t0, feed) }, own, t0 + 100, st, vibeadsb::kRangeAdsb);
    CHECK(h.tick({ adsbRow(0x400005, 52.001, -1.0, t0 + 1000, feed) }, own, t0 + 1000 + 11000, st, vibeadsb::kRangeAdsb) == 0, "a fix more than %lld s old when read is not offered", (long long)vibeadsb::TableRangeFollower::kFreshSec);
    // bounded
    vibeadsb::TableRangeFollower big;
    std::vector<vibeavshare::Row> many;
    for (uint32_t i = 0; i < 3000; ++i) many.push_back(adsbRow(0x600000 + i, 52.0 + i * 1e-4, 0.5, t0, feed));
    big.tick(many, own, t0 + 100, st, vibeadsb::kRangeAdsb);
    CHECK(big.tracked() <= vibeadsb::TableRangeFollower::kMaxTracked, "bounded: %zu aircraft tracked of 3000 rows (≤ %zu)", big.tracked(), vibeadsb::TableRangeFollower::kMaxTracked);
    big.tick({}, own, t0 + 200 * 1000, st, vibeadsb::kRangeAdsb);
    CHECK(big.tracked() == 0, "an aircraft unheard for %lld s is forgotten", (long long)vibeadsb::TableRangeFollower::kForgetSec);
}

static void testEndToEnd() {
    std::printf("2. end to end: a Beast feed (the front door's) → the shared table → this radio's outline\n");
    char tmpl[] = "/tmp/vs-rangefeed-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    CHECK(dir != nullptr, "a runtime directory");
    if (!dir) return;
    vibeavshare::table().setDir(dir);
    vibe::logradio::setMine("59a1a7d0");   // this process is a radio (the Lenovo's RSP1A id)
    const double cLat = 52.3125, cLon = -0.875;
    vibeadsb::rangeStore().setFiles(std::string(dir) + "/adsb-range");
    vibeadsb::TableRangeFollower f;
    CHECK(vibeadsb::followTableOnce(f, cLat, cLon, vibeavshare::wallMs()) == -1, "no table yet: not read (and not made)");

    FakeDump d; d.start();
    auto& fs = feeds::FeedSet::shared();
    fs.setDir(dir);
    std::vector<feeds::SourceCfg> v; std::string e;
    CHECK(feeds::parseSources("[{\"fmt\":\"beast\",\"dir\":\"connect\",\"host\":\"127.0.0.1\",\"port\":" + std::to_string(d.port) + ",\"name\":\"PiAware\"}]", v, e), "the feed's config: %s", e.c_str());
    fs.configure(v);
    const int b = binOf(cLat, cLon, 52.2572, 3.9194);
    const double want = vibeadsb::RangeOutline::distKm(cLat, cLon, 52.2572, 3.9194);
    CHECK(kmAt(vibeadsb::rangeStore().json(vibeadsb::kRangeAdsb, std::time(nullptr)), b) == 0, "before: /adsb/range's bin %d is empty (the Lenovo's every-bin-0)", b);
    // the radio process's follower, as its thread runs it (every 2 s): the feed's aircraft's second fix fills the bearing
    const bool filled = waitFor([&] {
        vibeavshare::sharer().flushNow();
        vibeadsb::followTableOnce(f, cLat, cLon, vibeavshare::wallMs());
        return kmAt(vibeadsb::rangeStore().json(vibeadsb::kRangeAdsb, std::time(nullptr)), b) > 0;
    }, 12);
    const std::string j = vibeadsb::rangeStore().json(vibeadsb::kRangeAdsb, std::time(nullptr));
    vibeavshare::Row w; for (const auto& r : vibeavshare::table().read(vibeavshare::wallMs(), 0)) if (r.icao == 0x40621D) w = r;
    const double got = vibeadsb::RangeOutline::distKm(cLat, cLon, w.lat, w.lon);
    CHECK(filled && std::fabs(kmAt(j, b) - got) < 0.11 && std::fabs(got - want) < 3, "after: the feed aircraft (40621D at %.4f N %.4f E) fills bin %d at %.1f km (its fix: %.1f), within seconds", w.lat, w.lon, b, kmAt(j, b), got);
    CHECK(j.find("\"lat\":52.31250,\"lon\":-0.87500") != std::string::npos, "…around the server's location");
    int others = 0; for (int i = 0; i < 360; ++i) if (i != b && kmAt(j, i) > 0) ++others;
    CHECK(others == 0, "…and nothing else (%d other bearings)", others);
    CHECK(w.used && w.radios && vibe::logradio::idsOf(vibeavshare::table().radioRegistry(), w.radios) == std::vector<std::string>{ "feed" }, "the row is tagged as the feed's, not this radio's");
    // the feed going away: ADS-B is no longer live on this machine (after kAdsbLiveSec) → the table is not read
    fs.configure({});
    d.stop();
    sleepMs(int(vibeavshare::kAdsbLiveSec * 1000) + 1500);
    CHECK(vibeadsb::followTableOnce(f, cLat, cLon, vibeavshare::wallMs()) == -1, "no ADS-B live on the machine: the table is not read");
    vibeadsb::rangeStore().flush(std::time(nullptr));
    vibeadsb::RangeOutline saved;
    CHECK(saved.load(std::string(dir) + "/adsb-range-adsb.bin") && !saved.empty(), "the outline was saved to the radio's file (it survives a restart)");
    std::string rm = std::string("rm -rf '") + dir + "'";
    if (std::system(rm.c_str()) != 0) std::printf("  (could not remove %s)\n", dir);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testRule();
    testEndToEnd();
    std::printf("\ntest-adsb-range-feed: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
