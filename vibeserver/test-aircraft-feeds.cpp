// test-aircraft-feeds.cpp — V12 beta96 SERVER-LEVEL AIRCRAFT FEEDS (vibe_feeds.h, vibe_sbs.h; docs/v12/ADSB-FEED.md).
//   1. BaseStation parsing: dumphfdl's own MSG,3 line (as its formatter writes it), dump1090-style MSG 1/3/4 lines, the
//      flag spellings, CRLF, and junk: binary bytes, an over-long line, a line split across reads, '~' addresses, (0,0),
//      out-of-range numbers, other record kinds.
//   2. The per-source table: labels (HFDL never ADS-B), freq-as-squawk, the fix's own time, an impossible jump refused.
//   3. The owner's list: defaults, every refusal, Beast forced to ADS-B, the round trip.
//   4. LISTEN: a test sender connects to a listen source and streams; reconnects; an address outside the rule refused
//      (and logged once); the status line; the shared table row tagged HFDL with a datalink position.
//   5. TWO SOURCES ON ONE MACHINE: a Beast connect source (a synthetic dump1090) + an SBS listen source (HFDL) merge into
//      ONE row per aircraft with both sources; the ADS-B one goes away → the banner names it and says HFDL is still live.
//   6. THE HUB AS A MIDDLE MAN: two Beast sources + our own radio's frames → one destination gets each reply ONCE; a
//      foreign reply arrives byte-identical (timestamp and signal byte kept); HFDL never reaches it; a destination on a
//      source's host is skipped; an own-only destination gets only our radio's.
// 127.0.0.1 sockets only. No radio, no audio.
#include "vibe_feeds.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); ++fails; } } while (0)
static double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static bool waitFor(const std::function<bool()>& f, double secs) {
    const double end = nowS() + secs;
    while (nowS() < end) { if (f()) return true; sleepMs(20); }
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
static int freePort() { int p = 0; const int fd = listenLocal(p); ::close(fd); return p; }
static int dialLocal(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(port));
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return -1; }
    return fd;
}
static void sendAll(int fd, const std::string& s) { size_t o = 0; while (o < s.size()) { ssize_t n = ::send(fd, s.data() + o, s.size() - o, 0); if (n <= 0) return; o += size_t(n); } }
static std::string field(const std::string& j, const std::string& key) {
    const size_t a = j.find("\"" + key + "\":");
    if (a == std::string::npos) return "";
    size_t b = a + key.size() + 3;
    if (j[b] == '"') { const size_t e = j.find('"', b + 1); return j.substr(b + 1, e - b - 1); }
    size_t e = b; while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != ']') ++e;
    return j.substr(b, e - b);
}
/** A valid DF17 identification for `icao` (parity computed), so the Tracker believes it. */
static std::vector<uint8_t> df17(uint32_t icao, uint8_t salt) {
    std::vector<uint8_t> m(14, 0);
    m[0] = 0x8D; m[1] = uint8_t(icao >> 16); m[2] = uint8_t(icao >> 8); m[3] = uint8_t(icao);
    m[4] = 0x20; m[5] = salt; m[6] = 0x2C; m[7] = 0xC3; m[8] = 0x71; m[9] = 0xC3; m[10] = 0x2C;
    const uint32_t s = vibeadsb::syndrome(m.data(), 14);
    m[11] = uint8_t(s >> 16); m[12] = uint8_t(s >> 8); m[13] = uint8_t(s);
    return m;
}
// The published worked example: DF17 even + odd airborne positions of 40621D (52.2572 N, 3.9194 E).
static const char* kPosE = "8D40621D58C382D690C8AC2863A7";
static const char* kPosO = "8D40621D58C386435CC412692AD6";
static const char* kId = "8D4840D6202CC371C32CE0576098";   // KLM1023

/** A synthetic dump1090: accepts readers on 127.0.0.1 and writes `frames` (Beast) to each, every 200 ms, until stopped. */
struct FakeDump {
    int port = 0, lfd = -1; std::atomic<bool> run{true}; std::thread th;
    std::vector<std::vector<uint8_t>> frames; std::atomic<int> rounds{0};
    std::mutex m; bool once = false; std::atomic<bool> release{false};
    void start() {
        lfd = listenLocal(port);
        th = std::thread([this] {
            std::vector<int> cs; size_t sentTo = 0;
            while (run) {
                pollfd p{lfd, POLLIN, 0};
                std::vector<uint8_t> out;
                { std::lock_guard<std::mutex> lk(m); for (auto& f : frames) beast::appendFrame(out, f.data(), int(f.size()), once ? 0x1A0000001A00ull : 0x1A0000001A00ull + rounds, 0x1A); }
                if (::poll(&p, 1, 20) > 0) { const int c = ::accept(lfd, nullptr, nullptr); if (c >= 0) cs.push_back(c); }
                if (!once) for (int c : cs) ::send(c, out.data(), out.size(), 0);
                else if (release && sentTo < cs.size()) { for (; sentTo < cs.size(); ++sentTo) ::send(cs[sentTo], out.data(), out.size(), 0); }   // ★ once, when released
                ++rounds;
                for (int i = 0; i < 10 && run; ++i) sleepMs(20);
            }
            for (int c : cs) ::close(c);
            ::close(lfd);
        });
    }
    void stop() { run = false; if (th.joinable()) th.join(); }
};

static void testParse() {
    std::printf("1. BaseStation parsing\n");
    sbs::Msg m;
    // dumphfdl's formatter (fmtr-basestation.c, read 2026-11-26): MSG,3 with only callsign, lat/lon, squawk 0, ground 0
    const std::string hf = "MSG,3,1,1,AB12CD,1,2026/11/26,10:00:00.000,2026/11/26,10:00:00.000,BAW123,,,,51.500000,-0.100000,,0,,,,0";
    CHECK(sbs::parseLine(hf, m) && m.type == 3 && m.icao == 0xAB12CD && m.callsign == "BAW123", "dumphfdl's line: type, address, callsign");
    CHECK(m.hasPos && std::fabs(m.lat - 51.5) < 1e-9 && std::fabs(m.lon + 0.1) < 1e-9 && !m.hasAlt && !m.hasGs && !m.hasTrk, "…the position, and no altitude/speed/track");
    CHECK(m.squawk == 0 && !m.ground && m.hasGround && !m.emergency, "…squawk 0, on-ground 0");
    CHECK(m.genMs == sbs::utcMs("2026/11/26", "10:00:00.000") && m.genMs > 0, "…the fix's time read as UTC");
    CHECK(sbs::utcMs("2000/03/01", "00:00:00") - sbs::utcMs("2000/02/28", "00:00:00") == 2 * 86400000LL && sbs::utcMs("2000/01/01", "00:00:00") == 946684800000LL, "the date maths (2000 is a leap year; the epoch offset)");
    // dump1090 style
    CHECK(sbs::parseLine("MSG,1,111,11111,4840D6,111111,2026/11/26,10:00:00.000,2026/11/26,10:00:00.000,KLM1023 ,,,,,,,,,,,", m) && m.callsign == "KLM1023" && m.type == 1, "MSG,1 identification (trailing space trimmed)");
    CHECK(sbs::parseLine("MSG,3,111,11111,40621D,111111,2026/11/26,10:00:00.000,2026/11/26,10:00:00.000,,38000,,,52.25720,3.91937,,,0,0,0,-1\r", m)
          && m.hasAlt && m.altFt == 38000 && m.hasPos && m.ground, "MSG,3 with altitude, CR at the end, '-1' = true");
    CHECK(sbs::parseLine("MSG,4,1,1,40621D,1,,,,,,,450,180.5,,,-832,,,,,", m) && m.hasGs && m.gs == 450 && m.hasTrk && std::fabs(m.trk - 180.5) < 1e-6 && m.hasVr && m.vr == -832, "MSG,4 speed, track, vertical rate");
    CHECK(sbs::parseLine("MSG,6,1,1,40621D,1,,,,,,,,,,,,7700,0,1,0,0", m) && m.squawk == 7700 && m.emergency, "MSG,6 squawk + emergency (1 = true)");
    // junk
    bool other = false;
    CHECK(!sbs::parseLine("MSG,3,1,1,~ABCDEF,1,,,,,,,,,51.0,0.1,,,,,,", m), "a non-ICAO ('~') address is skipped");
    CHECK(!sbs::parseLine("MSG,3,1,1,000000,1,,,,,,,,,51.0,0.1,,,,,,", m), "address 000000 is skipped");
    CHECK(!sbs::parseLine("MSG,9,1,1,ABCDEF,1,,,,,,,,,51.0,0.1,,,,,,", m), "transmission type 9 refused");
    CHECK(!sbs::parseLine("MSG,3,1,1,ABCDEG,1,,,,,,,,,51.0,0.1,,,,,,", m), "a bad hex digit refused");
    CHECK(!sbs::parseLine("hello world", m) && !sbs::parseLine("", m) && !sbs::parseLine("MSG", m), "plain junk refused");
    CHECK(!sbs::parseLine("SEL,,496,2286,4CA4E5,27215,2010/02/19,18:06:07.710,2010/02/19,18:06:07.710,RYR1427", m, &other) && other, "SEL: skipped as another kind");
    CHECK(sbs::parseLine("MSG,3,1,1,ABCDEF,1,,,,,,,,,0.000000,0.000000,,,,,,", m) && !m.hasPos, "(0,0) is not a position");
    CHECK(sbs::parseLine("MSG,3,1,1,ABCDEF,1,,,,,,,,,95.0,10.0,,,,,,", m) && !m.hasPos, "latitude 95 is not a position");
    CHECK(sbs::parseLine("MSG,3,1,1,ABCDEF,1,,,,,,,nan,inf,1e400,0x10,,,,,,", m) && !m.hasPos && !m.hasGs && !m.hasTrk, "nan / inf / hex numbers ignored");
    CHECK(sbs::parseLine("MSG,3,1,1,ABCDEF,1,,,,,BAW1", m) && m.callsign == "BAW1", "a line cut short after the callsign still reads");
    // the splitter
    sbs::LineSplitter sp; std::vector<std::string> got;
    auto take = [&](const std::string& l) { got.push_back(l); };
    const std::string a = "MSG,3,1,1,AB12CD,1,,,,,BAW1", b = "23,,,,51.5,-0.1,,0,,,,0\nGARB\x01\x02GE\n";
    sp.feed((const uint8_t*)a.data(), a.size(), take);
    sp.feed((const uint8_t*)b.data(), b.size(), take);
    CHECK(got.size() == 1 && got[0] == "MSG,3,1,1,AB12CD,1,,,,,BAW123,,,,51.5,-0.1,,0,,,,0", "a line split across two reads is whole; a binary line is dropped");
    CHECK(sp.junkLines() == 1, "…and counted (%llu)", (unsigned long long)sp.junkLines());
    std::string longL(5000, 'x'); longL += "\nMSG,3,1,1,AB12CD,1,,,,,X\n";
    got.clear(); sp.feed((const uint8_t*)longL.data(), longL.size(), take);
    CHECK(got.size() == 1 && got[0].rfind("MSG,3", 0) == 0 && sp.longLines() == 1, "a 5000-byte line is dropped up to its LF and counted; the next line reads");
    // a random-byte storm never throws or yields a record with an address of 0
    uint32_t r = 12345; size_t okN = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string s; const int n = int(r % 80); for (int k = 0; k < n; ++k) { r = r * 1103515245u + 12345u; s += char("MSG,0123456789ABCDEF.-~ \r"[r % 25]); }
        r = r * 1103515245u + 12345u;
        if (sbs::parseLine(s, m)) { ++okN; if (!m.icao) { CHECK(false, "a record with address 0"); break; } }
    }
    CHECK(true, "20,000 random lines parsed without a fault (%zu happened to be records)", okN);
}

static void testTable() {
    std::printf("2. the per-source table and its labels\n");
    const int64_t now = vibeavshare::wallMs();
    sbs::Table t("hfdl");
    sbs::Msg m;
    sbs::parseLine("MSG,3,1,1,AB12CD,1,,,,,BAW123,,,,51.500000,-0.100000,,8977,,,,0", m);
    CHECK(t.apply(m, now), "applied");
    std::vector<vibeavshare::Update> u;
    t.shareSince(0, now, u, "HFDL feed");
    CHECK(u.size() == 1 && u[0].srcMask == (1u << vibeavshare::kHfdl) && !(u[0].srcMask & (1u << vibeavshare::kAdsb)), "an HFDL source's row carries the HFDL bit and NOT ADS-B");
    CHECK(u[0].hasPos && u[0].posSrc == vibeavshare::kHfdl && u[0].via == "HFDL 8977", "…its position is an HFDL one; freq-as-squawk shows as the channel (%s)", u[0].via.c_str());
    CHECK(t.posKeepMs() == 30 * 60 * 1000, "an HFDL position is kept 30 min (the community feed's datalink window)");
    // the fix's own time
    sbs::Table t2("hfdl");
    sbs::parseLine("MSG,3,1,1,ABCDEF,1,2026/11/26,10:00:00.000,2026/11/26,10:00:00.000,,,,,51.0,0.0,,0,,,,0", m);
    const int64_t at = sbs::utcMs("2026/11/26", "10:02:00.000");
    t2.apply(m, at);
    CHECK(t2.rows().at(0xABCDEF).posMs == sbs::utcMs("2026/11/26", "10:00:00.000"), "the position keeps the fix's time (2 min before arrival)");
    // a jump no aircraft flies
    sbs::parseLine("MSG,3,1,1,ABCDEF,1,2026/11/26,10:02:30.000,,,,,,,10.0,100.0,,0,,,,0", m);
    CHECK(!t2.apply(m, at + 30000) && t2.rows().at(0xABCDEF).lat == 51.0 && t2.badPositions() == 1, "a 6,000 NM jump in 2.5 min is refused, counted, the old fix kept");
    // ADS-B label: squawk and emergency kept; 1-min position window
    sbs::Table ta("adsb");
    sbs::parseLine("MSG,3,1,1,4840D6,1,,,,,KLM1023,38000,,,52.0,4.0,,7700,,,,0", m); ta.apply(m, now);
    u.clear(); ta.shareSince(0, now, u, "ADS-B feed");
    CHECK(u.size() == 1 && u[0].srcMask == 1u && u[0].hasAlt && u[0].altFt == 38000 && ta.rows().at(0x4840D6).emergency, "an ADS-B source: the ADS-B bit, altitude, squawk 7700 = emergency");
    CHECK(ta.posKeepMs() == 60000, "…and ADS-B's own one-minute position window");
    // bounded
    sbs::Table tb("hfdl");
    for (uint32_t i = 1; i <= sbs::kMaxRows + 50; ++i) { sbs::Msg x; x.icao = i; x.type = 3; tb.apply(x, now + i); }
    CHECK(tb.size() == sbs::kMaxRows && !tb.rows().count(1) && tb.rows().count(sbs::kMaxRows + 50), "at most %zu aircraft; the oldest gives way", sbs::kMaxRows);
    tb.expire(now + 2 * 3600 * 1000);
    CHECK(tb.size() == 0, "rows unheard for an hour are forgotten");
}

static void testConfig() {
    std::printf("3. the owner's list\n");
    std::vector<feeds::SourceCfg> v; std::string e;
    CHECK(feeds::parseSources("", v, e) && v.empty(), "\"\" = no feeds");
    CHECK(feeds::parseSources(R"([{"fmt":"beast","dir":"connect","host":"192.168.86.12","port":30005,"name":"PiAware"},{"fmt":"sbs","dir":"listen","port":32010,"label":"hfdl"}])", v, e)
          && v.size() == 2 && v[0].label == "adsb" && v[0].host == "192.168.86.12" && v[1].label == "hfdl" && v[1].port == 32010, "Stuart's two sources: %s", e.c_str());
    std::vector<feeds::SourceCfg> back;
    CHECK(feeds::parseSources(feeds::sourcesJson(v), back, e) && feeds::sourcesJson(back) == feeds::sourcesJson(v), "the round trip: %s", feeds::sourcesJson(v).c_str());
    CHECK(feeds::parseSources(R"([{"fmt":"sbs","dir":"listen"}])", v, e) && v[0].port == feeds::kSbsPort && v[0].label == "hfdl", "SBS defaults: port 32010, label HFDL");
    CHECK(feeds::parseSources(R"([{"host":"pi.local"}])", v, e) && v[0].fmt == "beast" && v[0].dir == "connect" && v[0].port == 30005 && v[0].label == "adsb", "Beast defaults: connect, 30005, ADS-B");
    CHECK(feeds::parseSources(R"([{"fmt":"beast","host":"pi.local","label":"hfdl"}])", v, e) && v[0].label == "adsb", "Beast is always ADS-B");
    e.clear(); CHECK(!feeds::parseSources(R"([{"fmt":"json","host":"x"}])", v, e) && !e.empty(), "an unknown format refused: %s", e.c_str());
    e.clear(); CHECK(!feeds::parseSources(R"([{"fmt":"sbs","dir":"listen","label":"other"}])", v, e) && e.find("HFDL") != std::string::npos, "an unknown label refused: %s", e.c_str());
    e.clear(); CHECK(!feeds::parseSources(R"([{"fmt":"sbs","dir":"listen","port":80}])", v, e), "listening below 1024 refused");
    e.clear(); CHECK(!feeds::parseSources(R"([{"fmt":"sbs","dir":"listen","port":32010},{"fmt":"beast","dir":"listen","port":32010}])", v, e) && e.find("same port") != std::string::npos, "two on one port refused");
    e.clear(); CHECK(!feeds::parseSources(R"([{"host":"a b"}])", v, e), "a bad host refused");
    e.clear(); CHECK(!feeds::parseSources(R"({"host":"x"})", v, e), "not a list: refused");
    std::string many = "["; for (int i = 0; i < 9; ++i) many += std::string(i ? "," : "") + "{\"host\":\"h" + std::to_string(i) + "\"}";
    e.clear(); CHECK(!feeds::parseSources(many + "]", v, e), "nine sources refused (eight at most)");
}

static vibeavshare::Row rowFor(uint32_t icao) {
    const int64_t now = vibeavshare::wallMs();
    for (const auto& w : vibeavshare::table().read(now, 0)) if (w.icao == icao) return w;
    return vibeavshare::Row();
}

static void testListen() {
    std::printf("4. a LISTEN source (dumphfdl connects to us)\n");
    feeds::stallSec() = 1.0;
    auto& fs = feeds::FeedSet::shared();
    std::vector<std::string> logs; std::mutex lm;
    fs.setLog([&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); logs.push_back(l); });
    const int port = freePort();
    std::vector<feeds::SourceCfg> v; std::string e;
    feeds::parseSources("[{\"fmt\":\"sbs\",\"dir\":\"listen\",\"port\":" + std::to_string(port) + ",\"label\":\"hfdl\",\"name\":\"UberSDR HFDL\"}]", v, e);
    fs.configure(v);
    CHECK(waitFor([&] { return fs.statusJson().find("listening on") != std::string::npos; }, 3), "listening: %s", fs.statusJson().c_str());
    int c = dialLocal(port);
    CHECK(c >= 0, "a sender on 127.0.0.1 connects");
    sendAll(c, "MSG,3,1,1,AB12CD,1,2026/11/26,10:00:00.000,2026/11/26,10:00:00.000,BAW123,,,,51.500000,-0.100000,,0,,,,0\r\njunk line\nMSG,3,1,1,AB12CE,1,,,,,DLH4");
    sendAll(c, "AB,,,,48.1,11.5,,0,,,,0\n");
    CHECK(waitFor([&] { return field(fs.statusJson(), "aircraft") == "2"; }, 3), "two aircraft (one line split across sends): %s", fs.statusJson().c_str());
    CHECK(field(fs.statusJson(), "state") == "connected" && fs.statusJson().find("connected from 127.0.0.1") != std::string::npos, "the line names who connected");
    vibeavshare::sharer().flushNow();
    vibeavshare::Row w = rowFor(0xAB12CD);
    CHECK(w.used && w.srcMask == (1u << vibeavshare::kHfdl) && w.posSrc == vibeavshare::kHfdl && w.flight == "BAW123", "the shared table: HFDL only, an HFDL position, the callsign (mask %u)", w.srcMask);
    CHECK(!vibeavshare::adsbHeardNow(w, vibeavshare::wallMs(), vibeavshare::wallMs()), "never counted as ADS-B heard now");
    // ★★ beta96 (coordinator, b96-mapchat's OTHER RADIOS switch): a feed's aircraft are written AS the radio "feed", in a write of
    //    their own — never mixed into this process's own radio's updates
    {
        std::vector<std::pair<std::string, std::vector<uint32_t>>> calls;
        vibeavshare::sharer().writerForTest([&](const std::vector<vibeavshare::Update>& u, const std::vector<vibeavshare::GsHeard>& g, int64_t now2, bool live, const std::string& as) {
            std::vector<uint32_t> ids; for (const auto& x : u) ids.push_back(x.icao);
            calls.push_back({ as, ids });
            vibeavshare::table().write(u, g, now2, live, as);   // ★ and on into the table, so the rest of the test is unchanged
        });
        sendAll(c, "MSG,3,1,1,AB12CF,1,,,,,EZY77,,,,50.0,-25.0,,0,,,,0\n");
        waitFor([&] { return field(fs.statusJson(), "aircraft") == "3"; }, 3);
        vibeavshare::sharer().provide("testown", [](int64_t, std::vector<vibeavshare::Update>& out, std::vector<vibeavshare::GsHeard>&) {
            vibeavshare::Update x; x.icao = 0x777777; x.srcMask = 1u << vibeavshare::kAdsb; x.heardMs = x.srcMs[vibeavshare::kAdsb] = vibeavshare::wallMs(); out.push_back(x); });
        vibeavshare::sharer().flushNow();
        vibeavshare::sharer().remove("testown");
        vibeavshare::sharer().writerForTest(nullptr);
        bool feedTagged = false, ownMixed = false, ownSeparate = false;
        for (const auto& cl : calls) {
            const bool hasFeed = std::find(cl.second.begin(), cl.second.end(), 0xAB12CFu) != cl.second.end();
            const bool hasOwn = std::find(cl.second.begin(), cl.second.end(), 0x777777u) != cl.second.end();
            if (hasFeed && cl.first == feeds::kFeedRadioId) feedTagged = true;
            if (hasFeed && hasOwn) ownMixed = true;
            if (hasOwn && cl.first.empty()) ownSeparate = true;
        }
        CHECK(feedTagged && std::string(feeds::kFeedRadioId) == "feed", "a feed's aircraft are written as the radio \"feed\" (%zu writes)", calls.size());
        CHECK(!ownMixed && ownSeparate, "…in a write of their own: this process's own radio's aircraft go untagged, never mixed with the feed's");
    }
    CHECK(fs.publicJson().find("127.0.0.1") == std::string::npos && fs.publicJson().find("\"label\":\"HFDL\"") != std::string::npos, "the public status has the label and no address");
    // the sender goes away → after the stall time the banner names it
    ::close(c);
    CHECK(waitFor([&] { return !fs.banner().empty(); }, 4), "nobody connected for the stall time → banner: %s", fs.banner().c_str());
    CHECK(fs.banner().find("UberSDR HFDL lost") != std::string::npos, "…naming the source");
    c = dialLocal(port);
    sendAll(c, "MSG,3,1,1,AB12CF,1,,,,,EZY1,,,,50.0,1.0,,0,,,,0\n");
    CHECK(waitFor([&] { return fs.banner().empty() && field(fs.statusJson(), "aircraft") == "3"; }, 3), "it reconnects; the banner clears; data flows");
    ::close(c);
    // an address outside the rule (the test's rule refuses everything) is refused and logged once
    fs.setAllow([](const std::string&) { return false; });
    fs.configure(v);
    waitFor([&] { return fs.statusJson().find("listening on") != std::string::npos; }, 3);
    sleepMs(100);
    { std::lock_guard<std::mutex> lk(lm); logs.clear(); }
    for (int i = 0; i < 5; ++i) { int x = dialLocal(port); if (x >= 0) { char b; ::recv(x, &b, 1, 0); ::close(x); } }
    CHECK(waitFor([&] { return fs.statusJson().find("5 refused") != std::string::npos; }, 3), "five refused, counted: %s", fs.statusJson().c_str());
    size_t nLog = 0; { std::lock_guard<std::mutex> lk(lm); for (auto& l : logs) if (l.find("refused") != std::string::npos) ++nLog; }
    CHECK(nLog == 1, "…and logged once, not five times (%zu)", nLog);
    fs.setAllow(nullptr);
    fs.configure({});
    fs.setLog(nullptr);   // ★ the lambda above holds this test's locals
    CHECK(fs.size() == 0 && fs.statusJson() == "[]", "an empty list stops everything");
}

static void testTwo() {
    std::printf("5. two sources on one machine: Beast (ADS-B) + SBS (HFDL)\n");
    feeds::stallSec() = 1.0;
    FakeDump d;
    d.frames = { hexBytes(kPosE), hexBytes(kPosO), hexBytes(kId) };
    d.start();
    auto& fs = feeds::FeedSet::shared();
    const int lp = freePort();
    std::vector<feeds::SourceCfg> v; std::string e;
    CHECK(feeds::parseSources("[{\"fmt\":\"beast\",\"dir\":\"connect\",\"host\":\"127.0.0.1\",\"port\":" + std::to_string(d.port) + ",\"name\":\"PiAware\"},"
                              "{\"fmt\":\"sbs\",\"dir\":\"listen\",\"port\":" + std::to_string(lp) + ",\"label\":\"hfdl\"}]", v, e), "config: %s", e.c_str());
    fs.configure(v);
    waitFor([&] { return fs.statusJson().find("listening on") != std::string::npos; }, 3);
    const int c = dialLocal(lp);
    // the same aircraft as the Beast position, heard on HFDL too
    sendAll(c, "MSG,3,1,1,40621D,1,,,,,KLM10,,,,52.250000,3.910000,,0,,,,0\n");
    CHECK(waitFor([&] { vibeavshare::sharer().flushNow(); const auto w = rowFor(0x40621D); return w.used && w.srcMask == ((1u << vibeavshare::kAdsb) | (1u << vibeavshare::kHfdl)); }, 5),
          "ONE row for 40621D with both sources (mask %u)", rowFor(0x40621D).srcMask);
    const auto w = rowFor(0x40621D);
    CHECK((w.flags & 1) && std::fabs(w.lat - 52.2572) < 0.01, "…its position (the newest fix) %.4f %.4f from %s", w.lat, w.lon, vibeavshare::srcName(w.posSrc));
    CHECK(rowFor(0x4840D6).used && rowFor(0x4840D6).srcMask == 1u && rowFor(0x4840D6).flight == "KLM1023", "the Beast-only aircraft: ADS-B only, its callsign");
    CHECK(fs.adsbLive(), "an ADS-B source with aircraft counts as ADS-B decoding now");
    CHECK(fs.banner().empty(), "both live: no banner");
    d.stop();   // PiAware goes away
    CHECK(waitFor([&] { return fs.banner().find("PiAware lost") != std::string::npos; }, 6), "PiAware gone → banner: %s", fs.banner().c_str());
    CHECK(fs.banner().find("HFDL source still live") != std::string::npos, "…and HFDL still live: %s", fs.banner().c_str());
    CHECK(!fs.adsbLive(), "the ADS-B source down → no longer 'ADS-B now'");
    // the status file other processes read
    const std::string dir = "/tmp";
    fs.setDir(dir); fs.writeStatusFile();
    const std::string sf = feeds::readStatusFile(dir, vibeavshare::wallMs() / 1000);
    CHECK(sf.find("PiAware lost") != std::string::npos && sf.find("\"feeds\":[") != std::string::npos, "the status file carries the feeds and the banner");
    CHECK(feeds::readStatusFile(dir, vibeavshare::wallMs() / 1000 + 60).empty(), "…and a stale one reads as nothing");
    ::close(c);
    fs.configure({});
    fs.setDir("");
    ::unlink((dir + "/" + feeds::kStatusFile).c_str());
    ::unlink((dir + "/" + feeds::kIngestPortFile).c_str());
}

static std::vector<beast::Decoded> readFrames(int fd, double secs, std::vector<uint8_t>* raw = nullptr) {
    beast::Reader r; std::vector<beast::Decoded> out;
    const double end = nowS() + secs;
    uint8_t buf[4096];
    while (nowS() < end) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0) continue;
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        if (raw) raw->insert(raw->end(), buf, buf + n);
        r.feed(buf, size_t(n), out);
    }
    return out;
}
static int acceptWithin(int lfd, double secs) {
    const double end = nowS() + secs;
    while (nowS() < end) { pollfd p{lfd, POLLIN, 0}; if (::poll(&p, 1, 50) > 0) return ::accept(lfd, nullptr, nullptr); }
    return -1;
}

static void testHub() {
    std::printf("6. the hub as a middle man\n");
    beast::Hub hub;
    beast::Timing tm; tm.reduceSec = 0; tm.backoffMin = 0.2; hub.setTiming(tm);
    // two Beast sources sending the SAME reply (A) plus one each of their own (B, C) — ONCE each round
    const auto A = df17(0xA00001, 1), B = df17(0xA00002, 2), C = df17(0xA00003, 3);
    FakeDump d1, d2;
    d1.frames = { A, B }; d2.frames = { A, C }; d1.once = d2.once = true;
    // destinations: 127.0.0.1 (everything), an own-only one, and one on the sources' host ("localhost") = the loop guard
    int pAll = 0, pOwn = 0, pLoop = 0;
    const int lAll = listenLocal(pAll), lOwn = listenLocal(pOwn), lLoop = listenLocal(pLoop);
    beast::Config hc; hc.push = true;
    hc.feeds = { {"127.0.0.1", pAll, true, "", false}, {"127.0.0.1", pOwn, true, "", true}, {"localhost", pLoop, true, "", false} };
    hub.setInputHosts({"localhost"});   // ★ as FeedSet::configure sets it, before any destination connects
    hub.configure(hc);
    hub.setActive(true);
    auto& fs = feeds::FeedSet::shared();
    fs.setHub(&hub);
    const int sp = freePort();
    d1.start(); d2.start();
    std::vector<feeds::SourceCfg> v; std::string e;
    feeds::parseSources("[{\"host\":\"localhost\",\"port\":" + std::to_string(d1.port) + "},{\"host\":\"localhost\",\"port\":" + std::to_string(d2.port) + "},"
                        "{\"fmt\":\"sbs\",\"dir\":\"listen\",\"port\":" + std::to_string(sp) + ",\"label\":\"hfdl\"}]", v, e);
    // the sources must not start sending before the destinations are up: hold the fake receivers' frames until connected
    const int fAll = acceptWithin(lAll, 3), fOwn = acceptWithin(lOwn, 3);
    CHECK(fAll >= 0 && fOwn >= 0, "the two destinations connect");
    fs.configure(v);
    waitFor([&] { return fs.statusJson().find("listening on") != std::string::npos; }, 3);
    const int sc = dialLocal(sp);
    sendAll(sc, "MSG,3,1,1,ABCDEF,1,,,,,HFD1,,,,51.0,0.0,,0,,,,0\n");
    CHECK(waitFor([&] { const std::string j = fs.statusJson(); size_t n = 0, at = 0; while ((at = j.find("\"state\":\"connected\"", at)) != std::string::npos) { ++n; ++at; } return n == 3; }, 5),
          "both Beast sources and the HFDL sender are connected: %s", fs.statusJson().c_str());
    d1.release = true; d2.release = true;
    // our own radio's reply
    const auto O = df17(0xA00009, 9);
    sleepMs(300);
    hub.frame(O.data(), 14, 12345, -10.0f, 0xA00009, nowS());
    std::vector<uint8_t> rawAll;
    const auto got = readFrames(fAll, 2.5, &rawAll);
    auto count = [&](const std::vector<beast::Decoded>& g, const std::vector<uint8_t>& m) { int n = 0; for (auto& x : g) if (x.msg == m) ++n; return n; };
    CHECK(count(got, A) == 1, "the reply both sources heard arrives ONCE (%d)", count(got, A));
    CHECK(count(got, B) == 1 && count(got, C) == 1 && count(got, O) == 1, "each source's own reply and our radio's arrive (B %d, C %d, own %d)", count(got, B), count(got, C), count(got, O));
    CHECK(got.size() == 4, "nothing else — no HFDL, no Mode A/C (%zu frames)", got.size());
    CHECK(hub.duplicates() >= 1, "the duplicate is counted (%llu)", (unsigned long long)hub.duplicates());
    // byte-faithful: the upstream's escaped bytes, its timestamp and its signal byte (0x1A, escaped) exactly
    bool faithful = false;
    for (auto& x : got) if (x.msg == B) faithful = (x.sig == 0x1A) && ((x.ts & 0xFFFFFF000000ull) == 0x1A0000000000ull);
    std::vector<uint8_t> want; beast::appendFrame(want, B.data(), 14, 0x1A0000001A00ull, 0x1A);
    bool bytesIn = std::search(rawAll.begin(), rawAll.end(), want.begin(), want.end()) != rawAll.end();
    CHECK(faithful && bytesIn, "a source's reply goes on byte-identical (timestamp and signal byte as received)");
    const auto gotOwn = readFrames(fOwn, 0.8);
    CHECK(gotOwn.size() == 1 && gotOwn[0].msg == O, "the own-only destination gets only our radio's reply (%zu)", gotOwn.size());
    const int fLoop = acceptWithin(lLoop, 0.5);
    CHECK(fLoop < 0, "a destination on a source's host is never connected (the loop guard)");
    CHECK(hub.statusJson().find("\"state\":\"skipped\"") != std::string::npos, "…and shows as skipped");
    ::close(sc); ::close(fAll); ::close(fOwn); if (fLoop >= 0) ::close(fLoop);
    ::close(lAll); ::close(lOwn); ::close(lLoop);
    fs.configure({}); fs.setHub(nullptr);
    hub.stop();
    d1.stop(); d2.stop();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testParse();
    testTable();
    testConfig();
    testListen();
    testTwo();
    testHub();
    std::printf("\ntest-aircraft-feeds: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
