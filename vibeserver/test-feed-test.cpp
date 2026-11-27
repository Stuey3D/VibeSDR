// test-feed-test.cpp — V12 beta96 follow-up: TEST A FEED BEFORE SAVING IT, AUTO-DETECT, and the UBERSDR HFDL source
// (vibe_feed_test.h, vibe_uberhfdl.h; docs/v12/FEED-TEST.md). 127.0.0.1 sockets only; no radio, no audio.
//   1. One source through the SAVE's parser (the same refusals, the same words).
//   2. CONNECT tests against fakes: a Beast server (CRC-good frames), a BaseStation server, a closed port (on THIS machine:
//      the "did you mean listen" hint), the wrong format both ways, a silent server (Beast: no data; HFDL: quiet but ok).
//   3. LISTEN tests: a free port, a port another program holds, the port this feed already owns.
//   4. UBERSDR HFDL: the captured fixture (test/fixtures/uberhfdl) parsed exactly; a fake UberSDR over HTTP (the fixture,
//      a 404, not-JSON, chunked); the polite request (HTTP/1.0, the plain User-Agent, the path).
//   5. AUTO-DETECT: Beast + UberSDR + BaseStation + closed + silent ports probed at once, about 5 s, ONE request per port.
//   6. The Tester: one at a time ("busy"), cancel ends a running test at once.
//   7. The POLLER as a feed source: the fixture's aircraft into the source's table as HFDL rows ("HFDL 8942", the
//      registration), its status line, never faster than its floor, and a stop that joins at once.
#include "vibe_feed_test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
using namespace vibe::feeds;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); ++fails; } } while (0)
static double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static bool has(const std::string& s, const std::string& t) { return s.find(t) != std::string::npos; }

static std::vector<uint8_t> hexBytes(const char* h) {
    std::vector<uint8_t> v; for (size_t i = 0; h[i] && h[i + 1]; i += 2) { unsigned x; std::sscanf(h + i, "%2x", &x); v.push_back(uint8_t(x)); } return v;
}
static std::vector<uint8_t> beastFrame(const char* hex, uint64_t ts, uint8_t sig) {
    const auto m = hexBytes(hex);
    std::vector<uint8_t> raw;
    for (int i = 5; i >= 0; --i) raw.push_back(uint8_t(ts >> (8 * i)));
    raw.push_back(sig); raw.insert(raw.end(), m.begin(), m.end());
    std::vector<uint8_t> out{0x1a, uint8_t(m.size() == 7 ? '2' : '3')};
    for (uint8_t b : raw) { out.push_back(b); if (b == 0x1a) out.push_back(0x1a); }
    return out;
}
static const char* kFrames[] = {"8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7", "8D40621D58C386435CC412692AD6", "5D484FDEA248F5"};

/** A tiny TCP server on 127.0.0.1: `serve(fd)` runs per connection on the server's own thread (one at a time). */
struct Fake {
    int lfd = -1, port = 0; std::atomic<bool> run{true}; std::thread th; std::atomic<int> conns{0};
    std::function<void(int)> serve;
    explicit Fake(std::function<void(int)> f) : serve(std::move(f)) {
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        ::bind(lfd, (sockaddr*)&a, sizeof a); ::listen(lfd, 8);
        socklen_t sl = sizeof a; ::getsockname(lfd, (sockaddr*)&a, &sl); port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (run) {
                pollfd p{lfd, POLLIN, 0};
                if (::poll(&p, 1, 100) <= 0) continue;
                const int c = ::accept(lfd, nullptr, nullptr);
                if (c < 0) continue;
                ++conns;
                serve(c);
                ::close(c);
            }
        });
    }
    ~Fake() { run = false; th.join(); ::close(lfd); }
};
static int closedPort() {   // a port nothing listens on (bound then closed)
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    ::bind(fd, (sockaddr*)&a, sizeof a); socklen_t sl = sizeof a; ::getsockname(fd, (sockaddr*)&a, &sl);
    const int p = ntohs(a.sin_port); ::close(fd); return p;
}
static void sendAll(int fd, const std::string& s) { size_t o = 0; while (o < s.size()) { const ssize_t n = ::send(fd, s.data() + o, s.size() - o, 0); if (n <= 0) return; o += size_t(n); } }
static void sendAll(int fd, const std::vector<uint8_t>& s) { sendAll(fd, std::string(s.begin(), s.end())); }
/** Stream for `secs` (or until the peer goes): Beast frames every 50 ms. */
static void serveBeast(int fd, double secs) {
    uint64_t ts = 1000; int k = 0; const double end = nowS() + secs;
    while (nowS() < end) {
        std::vector<uint8_t> b;
        for (int i = 0; i < 10; ++i, ++k) { ts += 300000; auto f = beastFrame(kFrames[k % 4], ts, 0x1a); b.insert(b.end(), f.begin(), f.end()); }
        if (::send(fd, b.data(), b.size(), 0) <= 0) return;
        sleepMs(50);
    }
}
static void serveSbs(int fd, double secs) {
    const double end = nowS() + secs; int k = 0;
    while (nowS() < end) {
        char l[256];
        std::snprintf(l, sizeof l, "MSG,3,1,1,%06X,1,2026/11/27,08:55:16.000,2026/11/27,08:55:16.000,BAW%d,37000,,,51.5,-0.1,,,,,,0\n", 0x400000 + (k % 7), k % 7);
        ++k;
        if (::send(fd, l, std::strlen(l), 0) <= 0) return;
        sleepMs(20);
    }
}
/** Read one HTTP request head (to \r\n\r\n), at most 2 s. */
static std::string readHead(int fd) {
    std::string h; char b[1024]; const double end = nowS() + 2;
    while (nowS() < end && h.find("\r\n\r\n") == std::string::npos) {
        pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 100) <= 0) continue;
        const ssize_t n = ::recv(fd, b, sizeof b, 0); if (n <= 0) break; h.append(b, size_t(n));
    }
    return h;
}
static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
/** The fixture with every non-zero last_seen moved by `delta` seconds (so the ages are the capture's, today). */
static std::string shifted(const std::string& j, long long delta) {
    std::string o; size_t i = 0; const std::string k = "\"last_seen\":";
    for (;;) {
        const size_t p = j.find(k, i);
        if (p == std::string::npos) { o.append(j, i, std::string::npos); break; }
        o.append(j, i, p + k.size() - i);
        size_t e = p + k.size(); long long v = 0; while (e < j.size() && j[e] >= '0' && j[e] <= '9') v = v * 10 + (j[e++] - '0');
        o += std::to_string(v > 0 ? v + delta : 0); i = e;
    }
    return o;
}

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);   // ★ the fakes write to sockets the tests close (the server ignores SIGPIPE too)
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string fixture = readFile(root + "/test/fixtures/uberhfdl/aircraft-2026-11-27.json");
    if (fixture.size() < 1000) { std::printf("cannot read the fixture (run from the repo root, or give it as argv[1])\n"); return 2; }
    const long long kCapture = 1795769725;
    probe::Opts O; O.lanIp = "192.168.86.13"; O.connectSec = 1.5; O.readSec = 1.5; O.silentSec = 2.5;
    auto never = [] { return false; };

    std::printf("1. one source through the save's parser\n");
    {
        SourceCfg c; std::string e;
        CHECK(probe::parseOne("{\"fmt\":\"beast\",\"dir\":\"connect\",\"host\":\"192.168.86.12\",\"port\":30005}", c, e) && c.host == "192.168.86.12" && c.port == 30005, "beast parsed (%s)", e.c_str());
        CHECK(!probe::parseOne("{\"fmt\":\"sbs\",\"dir\":\"listen\",\"port\":80}", c, e) && e == "a feed listens on a port from 1024 to 65535", "a low listen port refused in the save's words (%s)", e.c_str());
        CHECK(!probe::parseOne("{\"fmt\":\"beast\",\"dir\":\"connect\",\"host\":\"300.1.1.1\"}", c, e) && has(e, "not an IPv4 address"), "a bad address refused (%s)", e.c_str());
        CHECK(probe::parseOne("{\"fmt\":\"uberhfdl\",\"host\":\"192.168.86.11\"}", c, e) && c.port == 8080 && c.label == "hfdl" && c.dir == "connect", "uberhfdl: port 8080, HFDL, connect by default (%s)", e.c_str());
        CHECK(!probe::parseOne("{\"fmt\":\"uberhfdl\",\"dir\":\"listen\",\"port\":9000}", c, e) && has(e, "read from the UberSDR"), "uberhfdl cannot listen (%s)", e.c_str());
        CHECK(!probe::parseOne("[{}]", c, e), "a list is not one source");
        std::vector<SourceCfg> v;
        CHECK(parseSources("[{\"fmt\":\"uberhfdl\",\"host\":\"ubersdr.local\",\"port\":8080,\"label\":\"adsb\"}]", v, e) && v.size() == 1 && v[0].label == "hfdl", "a saved uberhfdl source is HFDL whatever it says");
        CHECK(sourcesJson(v) == "[{\"fmt\":\"uberhfdl\",\"dir\":\"connect\",\"host\":\"ubersdr.local\",\"port\":8080,\"label\":\"hfdl\"}]", "…and round-trips: %s", sourcesJson(v).c_str());
    }

    std::printf("2. connect tests\n");
    {
        Fake beastSrv([](int fd) { serveBeast(fd, 3); });
        SourceCfg c; c.fmt = "beast"; c.host = "127.0.0.1"; c.port = beastSrv.port;
        const double t0 = nowS();
        auto r = probe::run(c, O, never);
        CHECK(r.ok && r.verdict == "ok" && r.msgs > 50 && r.aircraft >= 2, "Beast: %s (%llu, %zu)", r.line.c_str(), (unsigned long long)r.msgs, r.aircraft);
        CHECK(has(r.line, "Connected \xe2\x80\x94 ") && has(r.line, " Beast messages in 2 s, 2 aircraft"), "the words: %s", r.line.c_str());
        CHECK(nowS() - t0 < 3.0, "bounded (%.1f s)", nowS() - t0);
        CHECK(r.thisMachine, "127.0.0.1 is this machine");
        // ★ SBS text tested as Beast: wrong format, say so
        Fake sbsSrv([](int fd) { serveSbs(fd, 3); });
        c.port = sbsSrv.port;
        r = probe::run(c, O, never);
        CHECK(!r.ok && r.verdict == "wrongformat" && r.line == "Connected, but this is not Beast data \xe2\x80\x94 it looks like BaseStation (SBS) text (try BaseStation)", "SBS as Beast: %s", r.line.c_str());
        SourceCfg s; s.fmt = "sbs"; s.label = "adsb"; s.host = "127.0.0.1"; s.port = sbsSrv.port;
        r = probe::run(s, O, never);
        CHECK(r.ok && r.msgs > 10 && r.aircraft == 7 && has(r.line, "BaseStation messages"), "SBS: %s", r.line.c_str());
        s.port = beastSrv.port;
        r = probe::run(s, O, never);
        CHECK(!r.ok && r.verdict == "wrongformat" && has(r.line, "not BaseStation data") && has(r.line, "try Beast"), "Beast as SBS: %s", r.line.c_str());
        // closed port on this machine — Stuart's HFDL mistake
        s.port = closedPort(); s.label = "hfdl";
        r = probe::run(s, O, never);
        CHECK(!r.ok && r.verdict == "refused" && r.line == "Nothing is listening on 127.0.0.1:" + std::to_string(s.port), "closed: %s", r.line.c_str());
        CHECK(has(r.hint, "That address is this server") && has(r.hint, "listen on a port"), "…and the hint says listen: %s", r.hint.c_str());
        // silent
        Fake silent([](int fd) { const double e = nowS() + 4; while (nowS() < e) { char b[64]; pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 100) > 0 && ::recv(fd, b, sizeof b, 0) <= 0) return; } });
        c.port = silent.port;
        r = probe::run(c, O, never);
        CHECK(!r.ok && r.verdict == "nodata" && r.line == "Connected, but no data in 3 s", "silent Beast: %s", r.line.c_str());
        s.port = silent.port; s.label = "hfdl";
        r = probe::run(s, O, never);
        CHECK(r.ok && r.verdict == "quiet" && has(r.line, "normal for HFDL"), "silent HFDL is fine: %s", r.line.c_str());
        // a name that does not exist (.invalid never resolves, RFC 6761)
        c.host = "no-such-host.invalid"; c.port = 30005;
        r = probe::run(c, O, never);
        CHECK(!r.ok && r.verdict == "noname" && has(r.line, "Name not found: no-such-host.invalid"), "no name: %s", r.line.c_str());
        // an address nobody answers (TEST-NET-1, RFC 5737)
        c.host = "192.0.2.1";
        const double t1 = nowS();
        r = probe::run(c, O, never);
        CHECK(!r.ok && (r.verdict == "unreachable") && has(r.line, "No route to 192.0.2.1"), "no answer: %s", r.line.c_str());
        CHECK(nowS() - t1 < 2.5, "…within the connect deadline (%.1f s)", nowS() - t1);
    }

    std::printf("3. listen tests\n");
    {
        SourceCfg l; l.fmt = "sbs"; l.dir = "listen"; l.label = "hfdl"; l.port = closedPort();
        auto r = probe::run(l, O, never);
        CHECK(r.ok && r.verdict == "free" && has(r.hint, "must connect to 192.168.86.13:" + std::to_string(l.port)), "free: %s / %s", r.line.c_str(), r.hint.c_str());
        // another program holding the port on every address (as a real one would)
        const int hfd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in ha{}; ha.sin_family = AF_INET; ha.sin_addr.s_addr = htonl(INADDR_ANY); ha.sin_port = 0;
        ::bind(hfd, (sockaddr*)&ha, sizeof ha); ::listen(hfd, 1);
        socklen_t hsl = sizeof ha; ::getsockname(hfd, (sockaddr*)&ha, &hsl);
        struct { int port; } holder{ntohs(ha.sin_port)};
        l.port = holder.port;
        r = probe::run(l, O, never);
        CHECK(!r.ok && r.verdict == "inuse" && has(r.line, "in use by another program"), "held: %s", r.line.c_str());
        probe::Opts O2 = O;
        O2.ownedListen = [&](int p, std::string& t, std::string& f, std::string& line, bool& in) { if (p != holder.port) return false; t = "UberSDR HFDL"; f = "sbs"; line = "connected from 192.168.86.11"; in = false; return true; };
        r = probe::run(l, O2, never);
        CHECK(r.ok && r.verdict == "owned" && has(r.line, "already listens") && has(r.line, "connected from 192.168.86.11"), "owned: %s", r.line.c_str());
        l.fmt = "beast";
        r = probe::run(l, O2, never);
        CHECK(!r.ok && r.verdict == "inuse" && has(r.line, "UberSDR HFDL"), "owned by a feed of another format: %s", r.line.c_str());
        ::close(hfd);
    }

    std::printf("4. UberSDR HFDL\n");
    {
        uberhfdl::Parsed p; std::string e;
        CHECK(uberhfdl::parse(fixture, kCapture * 1000, 30 * 60 * 1000, p, e), "the fixture parses (%s)", e.c_str());
        CHECK(p.rows == 261 && p.withIcao == 253 && p.withPos == 243, "261 rows, 253 with an ICAO address, 243 with a fresh position (%zu %zu %zu)", p.rows, p.withIcao, p.withPos);
        const sbs::Msg* m = nullptr; for (auto& x : p.msgs) if (x.icao == 0x4521BF) m = &x;
        CHECK(m && m->callsign == "H60252" && m->reg == "LZ-LAN" && m->squawk == 8942 && m->hasPos && std::fabs(m->lat - 39.19998) < 1e-6 && m->genMs == 1795769725000LL,
              "4521BF: flight, reg, channel, position, the fix's own time");
        const sbs::Msg* alt = nullptr; for (auto& x : p.msgs) if (x.icao == 0xAB8B7F) alt = &x;
        CHECK(alt && alt->hasAlt && alt->altFt == 37000, "alt_ft when alt_valid");
        uberhfdl::Parsed q;
        CHECK(!uberhfdl::parse("{\"a\":1}", 0, 0, q, e) && e == "not UberSDR HFDL JSON", "an object is not it");
        CHECK(!uberhfdl::parse("[{\"name\":\"x\",\"value\":2}]", 0, 0, q, e), "another JSON list is not it");
        CHECK(uberhfdl::parse("[]", 0, 0, q, e) && q.rows == 0, "an empty list is (nobody heard yet)");
        // the fake UberSDR
        const long long delta = (long long)std::time(nullptr) - kCapture;
        const std::string body = shifted(fixture, delta);
        std::vector<std::string> heads; std::mutex hm;
        Fake uber([&](int fd) {
            const std::string h = readHead(fd);
            { std::lock_guard<std::mutex> lk(hm); heads.push_back(h); }
            if (h.rfind("GET /addon/hfdl/aircraft ", 0) == 0) sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
            else sendAll(fd, std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 18\r\n\r\n404 page not found"));
        });
        SourceCfg u; u.fmt = "uberhfdl"; u.label = "hfdl"; u.host = "127.0.0.1"; u.port = uber.port;
        auto r = probe::run(u, O, never);
        CHECK(r.ok && r.aircraft == 253 && has(r.line, "Connected \xe2\x80\x94 UberSDR HFDL, 253 aircraft (24"), "uberhfdl: %s", r.line.c_str());
        {
            std::lock_guard<std::mutex> lk(hm);
            CHECK(heads.size() == 1 && heads[0].rfind("GET /addon/hfdl/aircraft HTTP/1.0\r\n", 0) == 0, "one plain HTTP/1.0 GET of the add-on's path");
            CHECK(!heads.empty() && has(heads[0], "User-Agent: VibeServer (aircraft feed; +https://vibesdr.net)\r\n") && has(heads[0], "Accept-Encoding: identity"), "the polite User-Agent, no compression");
        }
        Fake nohfdl([](int fd) { readHead(fd); sendAll(fd, std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 18\r\n\r\n404 page not found")); });
        u.port = nohfdl.port;
        r = probe::run(u, O, never);
        CHECK(!r.ok && r.line == "Connected, but 404: this UberSDR has no HFDL add-on", "404: %s", r.line.c_str());
        Fake html([](int fd) { readHead(fd); sendAll(fd, std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\n<html>\r\n0\r\n\r\n")); });
        u.port = html.port;
        r = probe::run(u, O, never);
        CHECK(!r.ok && r.verdict == "wrongformat" && r.line == "Connected, but this is not UberSDR HFDL JSON", "a web page (chunked): %s", r.line.c_str());
        u.port = closedPort();
        r = probe::run(u, O, never);
        CHECK(!r.ok && r.verdict == "refused" && has(r.line, "Nothing is listening on 127.0.0.1:"), "nothing listening: %s", r.line.c_str());

        std::printf("5. auto-detect\n");
        Fake beastSrv([](int fd) { serveBeast(fd, 2.5); });
        Fake sbsSrv([](int fd) { serveSbs(fd, 2.5); });
        Fake silent([](int fd) { const double e = nowS() + 5; while (nowS() < e) { char b[256]; pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 100) > 0 && ::recv(fd, b, sizeof b, 0) <= 0) return; } });
        const int dead = closedPort();
        // ★ a second port on the same UberSDR (live: 8080 and 80 both answer) — one find, not two
        Fake uber2([&](int fd) { readHead(fd); sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body); });
        probe::Opts D = O; D.candidates = {beastSrv.port, sbsSrv.port, uber.port, dead, silent.port, uber2.port};
        const int uberBefore = uber.conns.load();
        const double t0 = nowS();
        auto d = probe::detect("127.0.0.1", 0, D, never);
        const double took = nowS() - t0;
        CHECK(took < 5.5, "about 5 s in all (%.1f s)", took);
        CHECK(d.hits.size() == 2 && d.hits[0].cfg.fmt == "beast" && d.hits[0].cfg.port == beastSrv.port && d.hits[0].name == "PiAware"
              && d.hits[1].cfg.fmt == "uberhfdl" && d.hits[1].name == "UberSDR HFDL", "Beast + UberSDR found (BaseStation dropped: the same aircraft as Beast) — %s", d.line.c_str());
        CHECK(d.line.rfind("Found: PiAware ADS-B (Beast, port " + std::to_string(beastSrv.port) + ") \xe2\x80\x94 2 aircraft", 0) == 0 && has(d.line, "also UberSDR HFDL (port"), "the line: %s", d.line.c_str());
        CHECK(uber.conns.load() - uberBefore == 1, "ONE request to the UberSDR's port per press (%d)", uber.conns.load() - uberBefore);
        CHECK(d.tried.size() == 6 && d.tried[5] == std::to_string(uber2.port) + ": UberSDR HFDL", "the second UberSDR port is tried, not listed twice");
        CHECK(d.tried.size() == 6 && d.tried[3] == std::to_string(dead) + ": nothing listening" && d.tried[4] == std::to_string(silent.port) + ": connected, but silent", "tried: %s · %s", d.tried.size() > 3 ? d.tried[3].c_str() : "", d.tried.size() > 4 ? d.tried[4].c_str() : "");
        const std::string dj = probe::toJson(d);
        CHECK(has(dj, "\"found\":true") && has(dj, "{\"src\":{\"fmt\":\"beast\",\"dir\":\"connect\",\"host\":\"127.0.0.1\",\"port\":" + std::to_string(beastSrv.port) + ",\"label\":\"adsb\",\"name\":\"PiAware\"}"), "the JSON carries a ready-to-add source: %s", dj.substr(0, 200).c_str());
        probe::Opts D2 = O; D2.candidates = {sbsSrv.port, dead};
        d = probe::detect("127.0.0.1:" + std::to_string(sbsSrv.port), 0, D2, never);
        CHECK(d.hits.size() == 1 && d.hits[0].cfg.fmt == "sbs" && d.hits[0].cfg.label == "adsb", "BaseStation alone: %s", d.line.c_str());
        probe::Opts D3 = O; D3.candidates = {dead};
        d = probe::detect("127.0.0.1", 0, D3, never);
        CHECK(d.hits.empty() && d.line == "Nothing found on 127.0.0.1 \xe2\x80\x94 tried " + std::to_string(dead) + ": nothing listening" && has(d.hint, "listen on a port"), "nothing: %s / %s", d.line.c_str(), d.hint.c_str());
        CHECK(probe::detectPorts(0) == std::vector<int>({30005, 30003, 8080, 80}) && probe::detectPorts(8073) == std::vector<int>({8073, 30005, 30003, 8080, 80}) && probe::detectPorts(8080) == std::vector<int>({8080, 30005, 30003, 80}), "the default ports, the owner's first");
        d = probe::detect("no-such-host.invalid", 0, O, never);
        CHECK(d.hits.empty() && d.line == "Name not found: no-such-host.invalid", "no name: %s", d.line.c_str());

        std::printf("6. the Tester: one at a time, cancel\n");
        {
            SourceCfg c; c.fmt = "beast"; c.host = "127.0.0.1"; c.port = silent.port;
            probe::Opts S = O; S.silentSec = 4;
            probe::Result a, b;
            std::thread t1([&] { a = probe::Tester::shared().test(c, S); });
            sleepMs(300);
            b = probe::Tester::shared().test(c, S);
            CHECK(b.verdict == "busy" && has(b.line, "Another feed test is running"), "a second test while one runs: busy (%s)", b.verdict.c_str());
            const double tc = nowS();
            probe::Tester::shared().cancel();
            t1.join();
            CHECK(a.verdict == "cancelled" && nowS() - tc < 0.6, "cancel ends it at once (%s, %.2f s)", a.verdict.c_str(), nowS() - tc);
            probe::Detected dd;
            CHECK(probe::Tester::shared().find("127.0.0.1", 0, D3, dd) && dd.hits.empty(), "and the next one runs");
        }

        std::printf("7. the poller as a feed source\n");
        {
            ::setenv("VIBE_TEST_UBERHFDL_POLL_SEC", "0.3", 1);
            SourceCfg c; std::string e;
            probe::parseOne("{\"fmt\":\"uberhfdl\",\"host\":\"127.0.0.1\",\"port\":" + std::to_string(uber.port) + ",\"name\":\"UberSDR HFDL\"}", c, e);
            std::vector<std::string> logs; std::mutex lm;
            const int before = uber.conns.load();
            Source src(c, nullptr, [&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); logs.push_back(l); });
            src.start();
            const double t0s = nowS();
            while (nowS() - t0s < 3 && src.aircraft() < 200) sleepMs(50);
            CHECK(src.aircraft() == 253, "253 aircraft in the source's table (%zu)", src.aircraft());
            std::vector<vibeavshare::Update> out;
            src.shareSince(0, out);
            const vibeavshare::Update* u = nullptr; for (auto& x : out) if (x.icao == 0x4521BF) u = &x;
            CHECK(u && u->srcMask == (1u << vibeavshare::kHfdl) && u->via == "HFDL 8942" && u->reg == "LZ-LAN" && u->flight == "H60252" && u->hasPos && u->posSrc == vibeavshare::kHfdl,
                  "a row: HFDL only, the channel, the registration, an HFDL position");
            size_t withPos = 0; for (auto& x : out) if (x.hasPos) ++withPos;
            CHECK(withPos >= 240 && withPos <= 243, "the fresh positions only (%zu)", withPos);
            const std::string st = src.statusJson(true);
            CHECK(has(st, "\"state\":\"connected\"") && has(st, "with positions \u00b7 253 aircraft") && has(st, "\"fmt\":\"uberhfdl\"") && has(st, "\"label\":\"HFDL\""), "status: %s", st.substr(0, 300).c_str());
            double ds; CHECK(!src.down(ds), "not down");
            sleepMs(1000);
            const int polls = uber.conns.load() - before;
            CHECK(polls >= 3 && polls <= 6, "polls at its (test) interval, not faster (%d in ~%.1f s)", polls, nowS() - t0s);
            const double ts = nowS();
            src.stop();
            CHECK(nowS() - ts < 0.5, "stop joins at once (%.2f s)", nowS() - ts);
            { std::lock_guard<std::mutex> lk(lm); CHECK(logs.size() == 1 && has(logs[0], "reading 127.0.0.1:") && has(logs[0], "/addon/hfdl/aircraft"), "one log line, not one per poll (%zu)", logs.size()); }
            ::unsetenv("VIBE_TEST_UBERHFDL_POLL_SEC");
            uberhfdl::Poller pl;   // ★ the real floor: never faster than 5 s
            const int b2 = uber.conns.load();
            pl.start("127.0.0.1", uber.port, 1800000, nullptr, 0.1);
            sleepMs(1500);
            CHECK(uber.conns.load() - b2 == 1, "asked for 0.1 s, it polls every 5 s (%d in 1.5 s)", uber.conns.load() - b2);
            pl.stop();
            // a lost UberSDR: down after the stall time, the banner names it
            stallSec() = 1.0;
            ::setenv("VIBE_TEST_UBERHFDL_POLL_SEC", "0.3", 1);
            SourceCfg gone = c; gone.port = closedPort();
            Source s2(gone, nullptr, nullptr);
            s2.start();
            sleepMs(1600);
            CHECK(s2.down(ds) && has(s2.statusJson(true), "Nothing is listening on 127.0.0.1:"), "an UberSDR that is not there: down, and why (%s)", s2.statusJson(true).substr(0, 260).c_str());
            s2.stop();
            ::unsetenv("VIBE_TEST_UBERHFDL_POLL_SEC");
            stallSec() = beast::kStallSec;
        }
    }

    std::printf("\ntest-feed-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
