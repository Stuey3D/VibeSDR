// test-adsb-feed.cpp — V12 beta96 SHARE ADS-B WITH FLIGHT TRACKERS (vibe_beast.h, docs/v12/ADSB-FEED.md).
//   1. Beast encoding: a hand-worked frame byte for byte (every 0x1A doubled), the type bytes, the 12 MHz counter, the
//      signal byte; then thousands of random frames (0x1A-heavy) round-tripped through the test's own Reader, with junk
//      and a broken frame in the stream.
//   2. The owner's settings: off by default, every refusal, the round trip, the UUID rule.
//   3. The local server on 127.0.0.1: a test client reads exactly the frames fed, in order; the LAN rule (peerAllowed).
//   4. A push link to a LOCAL test endpoint (never a real aggregator): the UUID first, the reduce, reconnect with
//      backoff after the far end drops, retry while nothing listens, the bounded queue on a link that stops reading,
//      and ADS-B stopping takes the link down after its grace.
//   5. The ADS-B service end to end: modulated IQ → believed replies → the sink, a repaired bit sent corrected, the
//      sample-clock counter's spacing.
#include "vibe_beast.h"
#include "vibe_adsb_service.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <atomic>
#include <functional>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace vibe::beast;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

static double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static std::vector<uint8_t> hexBytes(const char* h) {
    std::vector<uint8_t> o;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) { unsigned v; std::sscanf(h + i, "%2x", &v); o.push_back(uint8_t(v)); }
    return o;
}
static std::string field(const std::string& j, const std::string& key) {   // first "key":value (number/bool/string)
    const size_t a = j.find("\"" + key + "\":");
    if (a == std::string::npos) return "";
    size_t b = a + key.size() + 3;
    if (j[b] == '"') { const size_t e = j.find('"', b + 1); return j.substr(b + 1, e - b - 1); }
    size_t e = b; while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != ']') ++e;
    return j.substr(b, e - b);
}

// ── tiny sockets for the test endpoints ──
static int listenLocal(int& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(port));
    if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 4) != 0) { ::close(fd); return -1; }
    socklen_t l = sizeof a; getsockname(fd, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
    return fd;
}
static int freePort() { int p = 0; const int fd = listenLocal(p); ::close(fd); return p; }
static int acceptWithin(int lfd, double secs) {
    const double end = nowS() + secs;
    while (nowS() < end) {
        pollfd p{lfd, POLLIN, 0};
        if (::poll(&p, 1, 50) > 0) return ::accept(lfd, nullptr, nullptr);
    }
    return -1;
}
static int dialLocal(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(port));
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return -1; }
    return fd;
}
/** Read for up to `secs` (or until `want` frames), decoding Beast as it comes. */
static std::vector<Decoded> readFrames(int fd, double secs, size_t want, std::vector<uint8_t>* raw = nullptr) {
    Reader r; std::vector<Decoded> out;
    const double end = nowS() + secs;
    uint8_t buf[4096];
    while (nowS() < end && out.size() < want) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0) continue;
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        if (raw) raw->insert(raw->end(), buf, buf + n);
        r.feed(buf, size_t(n), out);
    }
    return out;
}
static bool waitFor(const std::function<bool()>& f, double secs) {
    const double end = nowS() + secs;
    while (nowS() < end) { if (f()) return true; sleepMs(20); }
    return f();
}

// The Riddle's DF17 identification example (KLM1023) and a DF11 all-call.
static const char* kId = "8D4840D6202CC371C32CE0576098";
static const char* kDf11 = "5D484FDEA248F5";

static void testEncoding() {
    std::printf("1. Beast encoding\n");
    CHECK(ts12FromFine(0) == 0 && ts12FromFine(2) == 3 && ts12FromFine(8) == 12, "125 ns units → 12 MHz ticks (×1.5)");
    CHECK(ts12FromFine(uint64_t(1) << 50) == ((uint64_t(3) << 49) & kTsMask), "the counter wraps at 48 bits");
    CHECK(signalByte(0) == 255 && signalByte(10) == 255, "0 dBFS (and above) = 255");
    CHECK(std::abs(int(signalByte(-6.0206f)) - 128) <= 1, "−6 dBFS (half amplitude) ≈ 128 (got %d)", signalByte(-6.0206f));
    CHECK(signalByte(-80) == 0 && signalByte(NAN) == 0, "very weak / unknown = 0");
    CHECK(typeFor(2) == '1' && typeFor(7) == '2' && typeFor(14) == '3' && typeFor(5) == 0, "type bytes '1' '2' '3'");

    // Hand-worked: timestamp 0x00001A00001A, signal 0x1A, the DF11 frame — every 0x1A doubled.
    std::vector<uint8_t> o;
    const auto m = hexBytes(kDf11);
    CHECK(appendFrame(o, m.data(), 7, 0x00001A00001Aull, 0x1A), "a 7-byte frame encodes");
    const auto want = hexBytes("1a32" "00001a1a00001a1a" "1a1a" "5d484fdea248f5");
    CHECK(o == want, "the bytes, 0x1A escaped in the timestamp and the signal (%zu vs %zu bytes)", o.size(), want.size());
    std::vector<uint8_t> bad;
    CHECK(!appendFrame(bad, m.data(), 5, 0, 0) && bad.empty(), "a 5-byte message is refused and nothing written");
    // a message containing 0x1A
    const uint8_t withEsc[14] = {0x8D, 0x1A, 0x1A, 0x1A, 0, 0, 0, 0, 0, 0, 0, 0x1A, 0, 0x1A};
    o.clear(); appendFrame(o, withEsc, 14, 0, 0);
    CHECK(o.size() == 2 + 6 + 1 + 14 + 5, "five 0x1A in the message → five extra bytes (%zu)", o.size());

    // Round trip: random frames, many 0x1A, with junk before, a broken frame in the middle.
    std::mt19937 rng(42);
    std::vector<uint8_t> stream = {0x00, 0x1A, 0x1A, 0x55, 0x1A, 0x99};   // junk: an escaped byte, an unknown type
    struct Want { int len; uint64_t ts; uint8_t sig; std::vector<uint8_t> m; };
    std::vector<Want> sent;
    for (int i = 0; i < 4000; ++i) {
        const int len = (i % 3 == 0) ? 7 : (i % 3 == 1) ? 14 : 2;
        Want w; w.len = len; w.ts = (uint64_t(rng()) << 16 ^ rng()) & kTsMask; w.sig = uint8_t(rng() % 4 ? rng() : 0x1A);
        for (int k = 0; k < len; ++k) w.m.push_back(uint8_t(rng() % 5 == 0 ? 0x1A : rng()));
        if (i % 7 == 0) w.ts = 0x1A1A1A1A1A1Aull;
        appendFrame(stream, w.m.data(), len, w.ts, w.sig);
        sent.push_back(w);
        if (i == 2000) { const uint8_t brk[] = {0x1A, '3', 1, 2, 3}; stream.insert(stream.end(), brk, brk + 5); }   // cut short
    }
    Reader rd; std::vector<Decoded> got;
    for (size_t at = 0; at < stream.size();) {   // in odd-sized pieces, as TCP hands them over
        const size_t n = std::min<size_t>(stream.size() - at, 1 + rng() % 97);
        rd.feed(stream.data() + at, n, got); at += n;
    }
    bool same = got.size() == sent.size();
    for (size_t i = 0; same && i < sent.size(); ++i)
        same = got[i].type == typeFor(sent[i].len) && got[i].ts == sent[i].ts && got[i].sig == sent[i].sig && got[i].msg == sent[i].m;
    CHECK(same, "4000 random frames (0x1A-heavy) round-trip exactly (%zu of %zu back)", got.size(), sent.size());
    CHECK(rd.bad() >= 1, "the broken frame and the junk are counted (%llu)", (unsigned long long)rd.bad());

    // reduce keys
    const auto id = hexBytes(kId);
    uint8_t pe[14] = {0x8D, 0x48, 0x40, 0xD6, 0x58, 0xC3, 0x82, 0xD6, 0x90, 0xC8, 0xAC, 0, 0, 0};   // TC 11, F=0
    uint8_t po[14]; std::memcpy(po, pe, 14); po[6] |= 0x04;                                           // F=1
    CHECK(reduceKey(pe, 14, 0x4840D6) != reduceKey(po, 14, 0x4840D6), "a position's odd and even are different keys");
    CHECK(reduceKey(id.data(), 14, 0x4840D6) != reduceKey(pe, 14, 0x4840D6), "identification and position differ");
    CHECK(reduceKey(pe, 14, 0x4840D6) != reduceKey(pe, 14, 0x4840D7), "two aircraft differ");
    CHECK(cleanUuid("0F3C3A2B-1234-4abc-9def-0123456789AB") == "0f3c3a2b-1234-4abc-9def-0123456789ab" && cleanUuid("nope").empty()
          && cleanUuid("0f3c3a2b-1234-4abc-9def-0123456789az").empty(), "UUIDs: 8-4-4-4-12 hex only, lower-cased");
    const auto uf = uuidFrame("0f3c3a2b-1234-4abc-9def-0123456789ab");
    CHECK(uf.size() == 38 && uf[0] == 0x1A && uf[1] == 0xE4 && std::memcmp(uf.data() + 2, "0f3c3a2b", 8) == 0, "the UUID announcement: 0x1A 0xE4 + 36 characters");
}

static void testConfig() {
    std::printf("2. settings\n");
    Config c; std::string e;
    CHECK(parseConfig("", c, e) && !c.serve && !c.push && c.feeds.empty() && c.port == 30005 && !c.open && !c.any(),
          "empty = everything off, port 30005, LAN only (every radio before this existed)");
    CHECK(parseConfig("{}", c, e) && !c.any(), "{} = off");
    CHECK(parseConfig(R"({"serve":true,"port":30105,"open":true,"push":true,"feeds":[{"host":"feed.adsb.lol","port":30004},
          {"host":"10.0.0.2","port":30004,"on":false,"uuid":"0F3C3A2B-1234-4abc-9def-0123456789AB"}]})", c, e)
          && c.serve && c.port == 30105 && c.open && c.push && c.feeds.size() == 2 && c.feeds[0].on && !c.feeds[1].on
          && c.feeds[1].uuid == "0f3c3a2b-1234-4abc-9def-0123456789ab", "a full blob reads (%s)", e.c_str());
    Config back; e.clear();
    CHECK(parseConfig(toJson(c), back, e) && toJson(back) == toJson(c), "toJson round-trips: %s", toJson(c).c_str());
    CHECK(!parseConfig(R"({"port":80})", c, e) && e.find("1024") != std::string::npos, "port under 1024 refused in words");
    e.clear(); CHECK(!parseConfig(R"({"port":30005.5})", c, e), "a fractional port refused");
    e.clear(); CHECK(!parseConfig(R"({"feeds":[{"host":"a b","port":1}]})", c, e) && !e.empty(), "a host with a space refused");
    e.clear(); CHECK(!parseConfig(R"({"feeds":[{"host":"x.org","port":0}]})", c, e), "port 0 refused");
    e.clear(); CHECK(!parseConfig(R"({"feeds":[{"host":"x.org","port":1,"uuid":"abc"}]})", c, e) && e.find("UUID") != std::string::npos, "a bad UUID refused");
    std::string many = R"({"feeds":[)";
    for (int i = 0; i < 9; ++i) many += std::string(i ? "," : "") + R"({"host":"h)" + std::to_string(i) + R"(.org","port":1})";
    e.clear(); CHECK(!parseConfig(many + "]}", c, e), "nine destinations refused (eight at most)");
    e.clear(); CHECK(!parseConfig("[1]", c, e), "not an object: refused");
    CHECK(peerAllowed("127.0.0.1", false) && peerAllowed("::1", false) && peerAllowed("192.168.1.20", false)
          && peerAllowed("10.1.2.3", false) && peerAllowed("::ffff:172.16.0.9", false) && peerAllowed("fe80::1", false)
          && peerAllowed("fd00::5", false), "loopback, private IPv4, link-local and ULA are the LAN");
    CHECK(!peerAllowed("8.8.8.8", false) && !peerAllowed("100.64.1.1", false) && !peerAllowed("", false), "a public address is not (LAN only)");
    CHECK(peerAllowed("8.8.8.8", true), "…unless the owner opened it to any address");
}

static void testServer() {
    std::printf("3. the local Beast server\n");
    Hub h; Timing t; t.listenRetry = 0.2; h.setTiming(t);
    const int port = freePort();
    Config c; c.serve = true; c.port = port;
    h.configure(c);
    CHECK(waitFor([&] { return field(h.statusJson(), "listening") == "true"; }, 3), "it listens on %d: %s", port, h.statusJson().c_str());
    const int fd = dialLocal(port);
    CHECK(fd >= 0, "a reader on 127.0.0.1 connects");
    CHECK(waitFor([&] { return h.statusJson().find("\"peer\":\"127.0.0.1\"") != std::string::npos; }, 3), "and is listed by address");
    const auto id = hexBytes(kId), d11 = hexBytes(kDf11);
    const double t0 = nowS();
    for (int i = 0; i < 300; ++i) {
        if (i % 2) h.frame(id.data(), 14, uint64_t(i) * 1000, -10.0f, 0x4840D6, t0);
        else h.frame(d11.data(), 7, uint64_t(i) * 1000, -20.0f, 0x484FDE, t0);
    }
    const auto got = readFrames(fd, 3, 300);
    bool ok = got.size() == 300;
    for (size_t i = 0; ok && i < got.size(); ++i)
        ok = got[i].ts == uint64_t(i) * 1000 && got[i].msg == (i % 2 ? id : d11) && got[i].type == (i % 2 ? '3' : '2');
    size_t bad = 0; for (size_t i = 0; i < got.size(); ++i) if (!(got[i].ts == uint64_t(i) * 1000 && got[i].msg == (i % 2 ? id : d11))) { bad = i; break; }
    CHECK(ok, "every frame fed arrives once, in order, unreduced (%zu of 300; first odd one %zu ts %llu type %c len %zu)", got.size(), bad,
          got.empty() ? 0ull : (unsigned long long)got[bad].ts, got.empty() ? '?' : got[bad].type, got.empty() ? 0 : got[bad].msg.size());
    CHECK(waitFor([&] { return h.statusJson().find("\"msgs\":300") != std::string::npos; }, 2), "the reader's count says 300");
    // A second server on the same port must say so in words.
    Hub h2; h2.setTiming(t); h2.configure(c);
    CHECK(waitFor([&] { return h2.statusJson().find("in use") != std::string::npos; }, 3), "a taken port is reported: %s", field(h2.statusJson(), "error").c_str());
    h2.stop();
    // Switched off: the reader is closed.
    h.configure(Config{});
    std::vector<uint8_t> raw; uint8_t b; ssize_t n = 0;
    { const double end = nowS() + 3; while (nowS() < end) { pollfd p{fd, POLLIN, 0}; if (::poll(&p, 1, 50) > 0) { n = ::recv(fd, &b, 1, 0); if (n <= 0) break; } } }
    CHECK(n == 0, "switching the server off closes its readers");
    ::close(fd);
    CHECK(field(h.statusJson(), "on") == "false", "and the status says off");
}

static void testPush() {
    std::printf("4. a push feed to a local test endpoint\n");
    int port = 0; int lfd = listenLocal(port);
    Hub h; Timing t; t.backoffMin = 0.2; t.backoffMax = 0.8; t.stableAfter = 60; t.activeGrace = 0.4; t.connectTimeout = 2;
    h.setTiming(t);
    Config c; c.push = true; c.feeds.push_back({"127.0.0.1", port, true, "0f3c3a2b-1234-4abc-9def-0123456789ab"});
    h.configure(c);
    sleepMs(400);
    pollfd p0{lfd, POLLIN, 0};
    CHECK(::poll(&p0, 1, 0) == 0, "no connection while ADS-B is not running here");
    h.setActive(true);
    int a = acceptWithin(lfd, 3);
    CHECK(a >= 0, "ADS-B running → the feed connects");
    CHECK(waitFor([&] { return h.statusJson().find("\"state\":\"connected\"") != std::string::npos; }, 3), "status: connected");
    const auto id = hexBytes(kId), d11 = hexBytes(kDf11);
    // the same identification 20 times inside 0.5 s → one; then 0.6 s later → a second; the DF11 is its own key
    const double t0 = 1000.0;
    for (int i = 0; i < 20; ++i) h.frame(id.data(), 14, uint64_t(i), -10, 0x4840D6, t0 + i * 0.02);
    h.frame(d11.data(), 7, 50, -10, 0x484FDE, t0 + 0.1);
    h.frame(id.data(), 14, 99, -10, 0x4840D6, t0 + 1.0);
    std::vector<uint8_t> raw;
    auto got = readFrames(a, 2, 3, &raw);
    CHECK(raw.size() >= 38 && raw[0] == 0x1A && raw[1] == 0xE4 && std::memcmp(raw.data() + 2, "0f3c3a2b-1234-4abc-9def-0123456789ab", 36) == 0,
          "the station UUID is the first thing sent");
    CHECK(got.size() == 3 && got[0].ts == 0 && got[1].msg == d11 && got[2].ts == 99,
          "reduced: 20 repeats in 0.5 s → 1, a different kind passes, the same again after 0.5 s passes (%zu frames; ts %llu %llu %llu)", got.size(),
          got.size() > 0 ? (unsigned long long)got[0].ts : 0ull, got.size() > 1 ? (unsigned long long)got[1].ts : 0ull, got.size() > 2 ? (unsigned long long)got[2].ts : 0ull);

    // the far end drops the link → reconnect after the backoff
    ::close(a);
    CHECK(waitFor([&] { return h.statusJson().find("\"state\":\"waiting\"") != std::string::npos
                             || h.statusJson().find("\"connects\":2") != std::string::npos; }, 3), "a dropped link waits, then…");
    a = acceptWithin(lfd, 3);
    CHECK(a >= 0 && field(h.statusJson(), "connects") == "2", "…reconnects (connects %s)", field(h.statusJson(), "connects").c_str());
    std::vector<uint8_t> raw2; readFrames(a, 0.5, 1, &raw2);
    CHECK(raw2.size() >= 2 && raw2[1] == 0xE4, "the UUID is sent again on the new connection");
    ::close(a);
    // nothing listening → retries back off (0.2 → 0.4 → 0.8 cap), the reason in words
    ::close(lfd);
    std::vector<int> waits; double lastConnects = 0;
    const double end = nowS() + 3.5;
    while (nowS() < end) {
        const std::string s = h.statusJson();
        const double cn = std::atof(field(s, "connects").c_str());
        if (cn != lastConnects) { lastConnects = cn; waits.push_back(int(cn)); }
        sleepMs(20);
    }
    const std::string st = h.statusJson();
    CHECK(st.find("\"state\":\"waiting\"") != std::string::npos || st.find("\"state\":\"connecting\"") != std::string::npos, "status says waiting");
    const std::string fst = st.substr(st.find("\"feeds\""));
    CHECK(!field(fst, "error").empty(), "with the reason: %s", field(fst, "error").c_str());
    CHECK(lastConnects >= 4 && lastConnects <= 10, "about 0.2+0.4+0.8+0.8… s apart: %g attempts in 3.5 s (not hammering)", lastConnects);
    CHECK(field(st, "line").find("retrying in") != std::string::npos || field(st, "line").find("connecting") != std::string::npos,
          "the admin line says so: %s", field(st, "line").c_str());

    // a link that stops reading: the queue stays bounded, the oldest goes, counted
    port = 0; lfd = listenLocal(port);
    Timing t2 = t; t2.linkCap = 16 * 1024; t2.reduceSec = 0; h.setTiming(t2);
    c.feeds[0] = {"127.0.0.1", port, true, ""};
    h.configure(c);
    a = acceptWithin(lfd, 3);
    CHECK(a >= 0, "connected to the slow endpoint");
    { int small = 4096; setsockopt(a, SOL_SOCKET, SO_RCVBUF, &small, sizeof small); }
    for (int i = 0; i < 60000; ++i) { h.frame(id.data(), 14, uint64_t(i), -10, 0x4840D6, 0); if (i % 2000 == 0) sleepMs(5); }
    CHECK(waitFor([&] { return std::atoll(field(h.statusJson().substr(h.statusJson().find("\"feeds\"")), "dropped").c_str()) > 0; }, 3),
          "a stalled link drops its oldest frames, counted: %s", h.statusJson().substr(h.statusJson().find("\"feeds\"")).c_str());
    CHECK(field(h.statusJson(), "line").find("dropped") != std::string::npos, "and the admin line says so");
    // ADS-B stops: the link goes after the grace
    h.setActive(false);
    CHECK(waitFor([&] { return h.statusJson().find("\"state\":\"off\"") != std::string::npos; }, 3), "ADS-B stopped → the feed closes after its grace");
    CHECK(field(h.statusJson(), "line").find("not running") != std::string::npos, "and the line says ADS-B is not running");
    ::close(a); ::close(lfd);
    h.stop();
}

// ── 5. the ADS-B service end to end ──
static std::vector<std::complex<float>> modulate(const std::vector<std::vector<uint8_t>>& frames, double rate, double amp,
                                                 double noise, uint32_t seed, double gapUs, std::vector<double>& startsUs) {
    std::vector<std::pair<double, double>> pulses;
    double t = 50.25;
    for (const auto& f : frames) {
        startsUs.push_back(t);
        for (double p : {0.0, 1.0, 3.5, 4.5}) pulses.push_back({t + p, t + p + 0.5});
        for (size_t i = 0; i < f.size() * 8; ++i) {
            const bool one = (f[i >> 3] >> (7 - (i & 7))) & 1;
            const double s = t + 8 + double(i) + (one ? 0 : 0.5);
            pulses.push_back({s, s + 0.5});
        }
        t += 8 + double(f.size()) * 8 + gapUs;
    }
    const size_t n = size_t((t + 50) * 1e-6 * rate);
    std::vector<double> env(n, 0.0);
    const double dt = 1e6 / rate;
    for (auto& p : pulses)
        for (size_t k = size_t(p.first / dt); k <= size_t(p.second / dt) && k < n; ++k) {
            const double s0 = double(k) * dt, s1 = s0 + dt;
            env[k] += std::max(0.0, std::min(s1, p.second) - std::max(s0, p.first)) / dt;
        }
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, noise / std::sqrt(2.0));
    std::vector<std::complex<float>> iq(n);
    for (size_t k = 0; k < n; ++k) iq[k] = {float(amp * env[k] + g(rng)), float(g(rng))};
    return iq;
}

static void testService() {
    std::printf("5. the ADS-B service → the sink\n");
    vibeadsb::Service svc;
    std::mutex m; std::vector<std::vector<uint8_t>> got; std::vector<uint64_t> fine; std::vector<uint32_t> addrs; int runs = 0, stops = 0;
    svc.setFeed([&](const uint8_t* b, int len, uint64_t f, float, uint32_t addr, double) {
                    std::lock_guard<std::mutex> lk(m); got.emplace_back(b, b + len); fine.push_back(f); addrs.push_back(addr); },
                [&](bool on) { std::lock_guard<std::mutex> lk(m); (on ? runs : stops)++; });
    svc.start(2.4e6, NAN, NAN, true);
    const auto id = hexBytes(kId);
    auto flipped = id; flipped[6] ^= 0x10;                  // one bit wrong: the Tracker repairs it
    auto noiseFrame = hexBytes("8D4840D6202CC371C32CE0576099");   // two bits off parity: never believed
    noiseFrame[5] ^= 0x01;
    std::vector<std::vector<uint8_t>> frames = {id, flipped, noiseFrame, id};
    std::vector<double> starts;
    const auto iq = modulate(frames, 2.4e6, 0.5, 0.01, 7, 300, starts);
    svc.feed(iq.data(), iq.size());
    waitFor([&] { std::lock_guard<std::mutex> lk(m); return got.size() >= 3; }, 3);
    svc.stop();
    std::lock_guard<std::mutex> lk(m);
    CHECK(runs == 1 && stops == 1, "the run hook hears start and stop");
    CHECK(got.size() == 3, "three believed replies reach the sink, the corrupt one does not (%zu)", got.size());
    bool allId = got.size() == 3;
    for (auto& g : got) allId = allId && g == id;
    CHECK(allId, "the one-bit error is sent CORRECTED (as the Tracker believed it)");
    CHECK(addrs.size() == 3 && addrs[0] == 0x4840D6, "the aircraft's address comes with it");
    if (fine.size() == 3) {
        // reply k starts at starts[k] µs; 125 ns units → 8 per µs. Frames 0 and 3 are 3 replies apart.
        const double d01 = (double(fine[1]) - double(fine[0])) / 8.0, want01 = starts[1] - starts[0];
        const double d03 = (double(fine[2]) - double(fine[0])) / 8.0, want03 = starts[3] - starts[0];
        CHECK(std::fabs(d01 - want01) <= 0.25 && std::fabs(d03 - want03) <= 0.25,
              "the sample-clock counter spaces them as sent: %.3f µs (want %.3f), %.3f µs (want %.3f)", d01, want01, d03, want03);
    }
}


// ── 6-9. THE BEAST INPUT (a radio with no SDR: another receiver's Beast output, Stuart 11-26) ──
static std::vector<uint8_t> posBytes(uint32_t icao, double lat, double lon, int altFt, bool odd) {
    auto mod = [](double x, double y) { return x - y * std::floor(x / y); };
    const double i = odd ? 1 : 0, dLat = 360.0 / (60 - i);
    const double yz = std::floor(131072.0 * mod(lat, dLat) / dLat + 0.5);
    const double rLat = dLat * (yz / 131072.0 + std::floor(lat / dLat));
    const double dLon = 360.0 / std::max(1.0, double(vibeadsb::cprNL(rLat)) - i);
    const double xz = std::floor(131072.0 * mod(lon, dLon) / dLon + 0.5);
    const uint32_t la = uint32_t(yz) & 0x1FFFF, lo = uint32_t(xz) & 0x1FFFF;
    const uint32_t n = uint32_t((altFt + 1000) / 25);
    const uint32_t alt = ((n >> 4) << 5) | 0x10 | (n & 0xF);
    uint64_t me = (uint64_t(11) << 51) | (uint64_t(alt) << 36) | (uint64_t(odd) << 34) | (uint64_t(la) << 17) | lo;
    std::vector<uint8_t> f(14, 0);
    f[0] = 0x8D; f[1] = uint8_t(icao >> 16); f[2] = uint8_t(icao >> 8); f[3] = uint8_t(icao);
    for (int k = 0; k < 7; ++k) f[4 + k] = uint8_t(me >> (48 - 8 * k));
    const uint32_t p = vibeadsb::syndrome(f.data(), 14);
    f[11] = uint8_t(p >> 16); f[12] = uint8_t(p >> 8); f[13] = uint8_t(p);
    return f;
}

static void testHostPort() {
    std::printf("6. the Beast feed radio's address\n");
    std::string h, e; int p = 0;
    CHECK(parseHostPort("192.168.86.12:30005", h, p, e) && h == "192.168.86.12" && p == 30005, "IPv4:port");
    CHECK(parseHostPort(" 192.168.86.12 ", h, p, e) && p == 30005, "a bare address → port 30005");
    CHECK(parseHostPort("piaware.local:30005", h, p, e) && h == "piaware.local", "a host name");
    CHECK(parseHostPort("[fd00::12]:30005", h, p, e) && h == "fd00::12" && p == 30005, "[IPv6]:port");
    CHECK(parseHostPort("fe80::1", h, p, e) && h == "fe80::1" && p == 30005, "a bare IPv6 address");
    e.clear(); CHECK(!parseHostPort("192.168.86:30005", h, p, e) && e.find("IPv4") != std::string::npos, "192.168.86 refused: %s", e.c_str());
    e.clear(); CHECK(!parseHostPort("300.1.1.1", h, p, e), "300.1.1.1 refused");
    e.clear(); CHECK(!parseHostPort("pi:99999", h, p, e) && !parseHostPort("pi:abc", h, p, e), "bad ports refused");
    e.clear(); CHECK(!parseHostPort("", h, p, e) && e.find("e.g.") != std::string::npos, "empty: says what to give");
    e.clear(); CHECK(!parseHostPort("[fd00::1", h, p, e) && !parseHostPort("my host", h, p, e), "a broken [v6] and a space refused");
    CHECK(hostPortText("fd00::1", 30005) == "[fd00::1]:30005", "an IPv6 address is shown in [ ]");
}

/** A synthetic dump1090: listens, and on each connection sends `perSec` replies a second (two aircraft flying, ids,
 *  a DF11, now and then a broken frame or line noise) until told to stop. */
struct Feeder {
    int lfd = -1, port = 0; std::atomic<bool> run{true}; std::thread th; std::atomic<int> conns{0}; int perSec;
    bool junk;
    explicit Feeder(int rate, bool withJunk = true, int fixedPort = 0) : perSec(rate), junk(withJunk) {
        port = fixedPort; lfd = listenLocal(port);
        th = std::thread([this] { loop(); });
    }
    ~Feeder() { stop(); }
    void stop() { run = false; if (th.joinable()) th.join(); if (lfd >= 0) { ::close(lfd); lfd = -1; } }
    void loop() {
        while (run) {
            const int c = acceptWithin(lfd, 0.2);
            if (c < 0) continue;
            ++conns;
            serve(c);
            ::close(c);
        }
    }
    void serve(int c) {
        const auto id = hexBytes(kId);
        uint64_t ts = 0; int k = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (run) {
            std::vector<uint8_t> o;
            for (int i = 0; i < perSec / 50; ++i, ++k) {
                ts += 12000 * 3 + (k % 7);
                const double lat = 52.0 + 0.0001 * (k / 2), lon = -1.0;
                if (k % 10 == 0) appendFrame(o, id.data(), 14, ts, 120);
                else if (k % 10 == 1) { auto d = hexBytes(kDf11); appendFrame(o, d.data(), 7, ts, 90); }
                else { auto f = posBytes(k % 4 < 2 ? 0x4840D6 : 0x400F01, lat, lon, 35000, k % 2); appendFrame(o, f.data(), 14, ts, 0x1A); }
                if (junk && k % 97 == 0) { const uint8_t g[] = {0x55, 0x1A, 0x99, 0x00, 0x1A, '3', 1, 2}; o.insert(o.end(), g, g + 8); }
                if (junk && k % 89 == 0) { auto f = posBytes(0x4840D6, 52, -1, 35000, 0); f[9] ^= 0x18; appendFrame(o, f.data(), 14, ts, 50); }   // 2 bits: bad CRC
            }
            if (::send(c, o.data(), o.size(), 0) < 0) return;
            const auto due = t0 + std::chrono::milliseconds(20 * (k / std::max(1, perSec / 50)));
            std::this_thread::sleep_until(due);
        }
    }
};

static std::vector<vibeadsb::Frame> toFrames(const std::vector<Decoded>& ds) {
    std::vector<vibeadsb::Frame> fs;
    for (const auto& d : ds) { auto f = vibeadsb::frameFromBeast(d.msg.data(), int(d.msg.size()), d.ts, d.sig); if (f.len) fs.push_back(f); }
    return fs;
}

static void testInput() {
    std::printf("7. the Beast input against a local synthetic dump1090\n");
    // nothing listening → the owner's words, and backoff
    {
        const int port = freePort();
        BeastInput in; InputTiming t; t.backoffMin = 0.2; t.backoffMax = 0.4; in.setTiming(t);
        in.start("127.0.0.1", port, [](const std::vector<Decoded>&) {});
        CHECK(waitFor([&] { return in.statusJson().find("\"state\":\"reconnecting\"") != std::string::npos; }, 3), "nothing listening → reconnecting");
        const std::string st = in.statusJson();
        CHECK(field(st, "error") == "nothing is listening on 127.0.0.1:" + std::to_string(port) + " \xe2\x80\x94 is dump1090's Beast output on?",
              "the error says what to check: %s", field(st, "error").c_str());
        CHECK(field(st, "line").find("trying again in") != std::string::npos, "and when it tries again");
        in.stop();
    }
    // the feed through the Service: aircraft appear, bad CRC and junk dropped and counted
    {
        Feeder fd(1500);
        vibeadsb::Service svc;
        std::atomic<uint64_t> sunk{0};
        svc.setFeed([&](const uint8_t*, int, uint64_t, float, uint32_t, double) { ++sunk; }, [](bool) {});
        svc.start(0, 52.0, -1.0, true);
        CHECK(svc.inputOnly(), "rate 0 = an input with no radio (no demodulator)");
        BeastInput in;
        in.start("127.0.0.1", fd.port, [&](const std::vector<Decoded>& ds) { svc.injectFrames(toFrames(ds)); });
        CHECK(waitFor([&] { return in.connected(); }, 3), "connected");
        sleepMs(2600);
        const std::string rep = svc.report();
        CHECK(rep.find("\"hex\":\"4840d6\"") != std::string::npos && rep.find("\"hex\":\"400f01\"") != std::string::npos, "both aircraft in the report");
        CHECK(rep.find("\"lat\":52") != std::string::npos, "with a position (CPR pairs from the feed)");
        CHECK(rep.find("KLM1023") != std::string::npos, "and the callsign");
        const std::string st = in.statusJson();
        const double ps = std::atof(field(st, "perSec").c_str());
        CHECK(ps > 1200 && ps < 1900, "about 1,500 messages a second counted (%.0f)", ps);
        CHECK(std::atoll(field(st, "bad").c_str()) > 0, "line noise resynced and counted (%s)", field(st, "bad").c_str());
        CHECK(svc.rejected() > 0, "bad-CRC replies dropped by our own check (%llu)", (unsigned long long)svc.rejected());
        CHECK(sunk.load() > 2000, "believed replies reach the share sink like a local decode (%llu)", (unsigned long long)sunk.load());
        // the feeder restarts (a Pi rebooting): the input comes back on its own
        const int port = fd.port;
        fd.stop();
        CHECK(waitFor([&] { return in.statusJson().find("\"state\":\"reconnecting\"") != std::string::npos; }, 3), "the far end gone (mid-stream close) → reconnecting: %s",
              field(in.statusJson(), "error").c_str());
        Feeder again(500, false, port);
        CHECK(again.port == port, "(the feeder is back on the same port)");
        CHECK(waitFor([&] { return in.connected(); }, 6), "…and reconnected when it came back");
        in.stop(); svc.stop(); again.stop();
    }
    // silence on an open link (a half-open TCP after the far end lost power) → reconnect
    {
        int port = 0; const int lfd = listenLocal(port);
        BeastInput in; InputTiming t; t.silentAfter = 0.6; t.backoffMin = 0.2; in.setTiming(t);
        in.start("127.0.0.1", port, [](const std::vector<Decoded>&) {});
        const int c = acceptWithin(lfd, 3);
        CHECK(waitFor([&] { return field(in.statusJson(), "error").find("sent nothing") != std::string::npos; }, 3), "a silent link is dropped: %s",
              field(in.statusJson(), "error").c_str());
        in.stop(); if (c >= 0) ::close(c); ::close(lfd);
    }
}

static void testLoopGuard() {
    std::printf("8. the loop guard\n");
    int port = 0; const int lfd = listenLocal(port);
    Hub h; Timing t; t.backoffMin = 0.2; h.setTiming(t);
    h.setInputHost("127.0.0.1");
    Config c; c.push = true; c.feeds.push_back({"127.0.0.1", port, true, ""});
    h.configure(c); h.setActive(true);
    sleepMs(600);
    pollfd p{lfd, POLLIN, 0};
    CHECK(::poll(&p, 1, 0) == 0, "a push feed to the host this radio's ADS-B comes from is never connected");
    CHECK(waitFor([&] { return h.statusJson().find("\"state\":\"skipped\"") != std::string::npos; }, 2), "status: skipped");
    CHECK(field(h.statusJson(), "line").find("from there") != std::string::npos, "and the line says why");
    h.setInputHost("");
    CHECK(acceptWithin(lfd, 3) >= 0, "another source: the same feed connects");
    h.stop(); ::close(lfd);
}

static long rssKb() { rusage u{}; getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return u.ru_maxrss / 1024;
#else
    return u.ru_maxrss;
#endif
}
static double cpuSec() { rusage u{}; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6; }

static void testBudget() {
    std::printf("9. the cost at ~1,500 messages a second (feeder in a child process; this process = input + tracker + Beast server + push)\n");
    int fds[2]; if (::pipe(fds) != 0) return;
    const pid_t kid = fork();
    if (kid == 0) {   // the synthetic dump1090, in its own process so its cost is not counted
        ::close(fds[0]);
        Feeder f(1500);
        const int pt = f.port; (void)!::write(fds[1], &pt, sizeof pt);
        sleepMs(14000);
        f.stop(); _exit(0);
    }
    ::close(fds[1]);
    int port = 0; (void)!::read(fds[0], &port, sizeof port); ::close(fds[0]);
    int sink = 0; const int sfd = listenLocal(sink);
    Hub& hub = Hub::shared();
    Config c; c.serve = true; c.port = freePort(); c.push = true; c.feeds.push_back({"127.0.0.1", sink, true, ""});
    hub.configure(c);
    vibeadsb::Service svc;
    svc.setFeed([&](const uint8_t* m, int len, uint64_t fine, float db, uint32_t a, double t) { hub.frame(m, len, ts12FromFine(fine), db, a, t); },
                [&](bool on) { hub.setActive(on); });
    svc.start(0, 52.0, -1.0, true);
    BeastInput in;
    in.start("127.0.0.1", port, [&](const std::vector<Decoded>& ds) { svc.injectFrames(toFrames(ds)); });
    const int reader = dialLocal(c.port);
    const int agg = acceptWithin(sfd, 3);
    std::atomic<bool> drainRun{true};
    std::thread drainer([&] { uint8_t b[8192]; while (drainRun) { pollfd p[2] = {{reader, POLLIN, 0}, {agg, POLLIN, 0}};
        if (::poll(p, 2, 100) > 0) { if (p[0].revents) (void)::recv(reader, b, sizeof b, 0); if (p[1].revents) (void)::recv(agg, b, sizeof b, 0); } } });
    sleepMs(2000);   // warm up
    const double c0 = cpuSec(); const auto w0 = std::chrono::steady_clock::now();
    std::string rep;
    for (int i = 0; i < 10; ++i) { sleepMs(1000); rep = svc.report(); }   // the 1 fps report a listener would cause
    const double cpu = cpuSec() - c0;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    const double ps = std::atof(field(in.statusJson(), "perSec").c_str());
    std::printf("   measured: %.0f msg/s in, %.2f %% of one core on this Mac, peak RSS %ld MB (whole test process)\n",
                ps, 100.0 * cpu / wall, rssKb() / 1024);
    CHECK(ps > 1200, "the feed ran at ~1,500 msg/s (%.0f)", ps);
    CHECK(100.0 * cpu / wall < 5.0, "under 5 %% of a core here (%.2f %%) — a Pi 3 core is roughly 5-8x slower", 100.0 * cpu / wall);
    drainRun = false; drainer.join();
    in.stop(); svc.stop(); hub.configure(Config{});
    ::close(reader); ::close(agg); ::close(sfd);
    int stv = 0; waitpid(kid, &stv, 0);
}


static void testOutage() {
    std::printf("10. a long outage on a fake clock (Stuart's PiAware drops for hours)\n");
    Outage o; InputTiming tm; std::string log;
    std::vector<std::string> logs;
    double t = 1000, d = o.fail(t, "nothing is listening on 192.168.86.12:30005", tm, 0.5, log);
    logs.push_back(log);
    CHECK(log.find("lost:") == 0 && std::fabs(d - 2.0) < 1e-9, "the loss is logged once; first retry after 2 s (%s)", log.c_str());
    double maxDelay = 0, minDelay = 1e9; int tries = 1;
    std::mt19937 r(3); std::uniform_real_distribution<double> j(0, 1);
    while (t < 1000 + 5 * 3600 + 7 * 60) {   // five hours and seven minutes down
        t += d;
        d = o.fail(t, "no answer", tm, j(r), log); ++tries;
        if (!log.empty()) logs.push_back(log);
        if (tries > 8) { maxDelay = std::max(maxDelay, d); minDelay = std::min(minDelay, d); }
    }
    CHECK(maxDelay <= 72.0 + 1e-9 && minDelay >= 48.0 - 1e-9, "capped at 60 s ±20 %% jitter (%.1f–%.1f s), retried forever (%d tries)", minDelay, maxDelay, tries);
    CHECK(logs.size() >= 30 && logs.size() <= 33, "no log flood: %zu lines in 5 h (one at the loss, one per 10 min)", logs.size());
    CHECK(logs.back().find("still reconnecting, down 5 h") == 0, "the reminder says how long: %s", logs.back().c_str());
    o.up(t, log);
    CHECK(log.find("back after 5 h 7 min") == 0, "recovery logged once, with the outage: %s", log.c_str());
    CHECK(!o.down() && o.backoff == 0, "a reconnect resets the backoff");
    d = o.fail(t + 10, "closed", tm, 0.5, log);
    CHECK(std::fabs(d - 2.0) < 1e-9 && log.find("lost:") == 0, "the next loss starts again at 2 s");
    CHECK(spanWords(8040) == "2 h 14 min" && spanWords(90) == "1 min" && spanWords(200000) == "2 d 7 h", "the words for a span");
}

static void testStallState() {
    std::printf("11. a stalled feed is shown as stalled, then reconnected; logs not flooded\n");
    int port = 0; const int lfd = listenLocal(port);
    std::vector<std::string> logs; std::mutex lm;
    BeastInput in; InputTiming t; t.silentAfter = 2.5; t.backoffMin = 0.2; in.setTiming(t);
    in.setLog([&](const std::string& l) { std::lock_guard<std::mutex> g(lm); logs.push_back(l); });
    in.start("127.0.0.1", port, [](const std::vector<Decoded>&) {});
    const int c = acceptWithin(lfd, 3);
    const auto id = hexBytes(kId); std::vector<uint8_t> o; appendFrame(o, id.data(), 14, 1, 1);
    (void)::send(c, o.data(), o.size(), 0);
    sleepMs(300);
    CHECK(field(in.statusJson(), "state") == "connected" && in.downSinceUtc() == 0, "data flowing: connected, not down");
    // (kQuietSec is 20 s in the server; this test does not wait that long — the stall → reconnect at silentAfter is the rule)
    CHECK(waitFor([&] { return field(in.statusJson(), "state") == "reconnecting"; }, 5), "silent past the stall limit → reconnecting: %s",
          field(in.statusJson(), "error").c_str());
    CHECK(in.downSinceUtc() > 0 && std::atoll(field(in.statusJson(), "downSince").c_str()) > 0, "down-since is set (the listeners' banner)");
    const int c2 = acceptWithin(lfd, 3);
    (void)::send(c2, o.data(), o.size(), 0);
    CHECK(waitFor([&] { return in.downSinceUtc() == 0; }, 3), "data again: the outage is over (the banner clears)");
    { std::lock_guard<std::mutex> g(lm);
      CHECK(logs.size() == 2 && logs[0].find("lost:") != std::string::npos && logs[1].find("back after") != std::string::npos,
            "two log lines for the whole outage (%zu): %s | %s", logs.size(), logs.empty() ? "" : logs[0].c_str(), logs.size() > 1 ? logs[1].c_str() : ""); }
    in.stop(); ::close(c); ::close(c2); ::close(lfd);
}

int main() {
    testEncoding();
    testConfig();
    testServer();
    testPush();
    testService();
    testHostPort();
    testInput();
    testLoopGuard();
    testBudget();
    testOutage();
    testStallState();
    std::printf("test-adsb-feed: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
