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

int main() {
    testEncoding();
    testConfig();
    testServer();
    testPush();
    testService();
    std::printf("test-adsb-feed: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
