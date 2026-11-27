// vibe_feed_test.h — V12 beta96 follow-up: TEST AN AIRCRAFT FEED SOURCE BEFORE IT IS SAVED (docs/v12/FEED-TEST.md).
// Stuart, 2026-11-27: "we need to test the connection before saving". His HFDL feed was saved as "connect to
// 192.168.86.13:32010" — this server's own address, where nothing listens: dumphfdl connects OUT, so it should have been
// "listen". A test in the form says so before the save.
//
// ★★★ THE SAME RULES AS THE SAVE: one source, read by vibe_feeds.h parseSources (the list parser, given a list of one) — the
//     test refuses exactly what the save refuses, in the same words.
// ★★★ CONNECT: resolve (bounded), TCP connect (bounded), then READ for a few seconds and check the bytes really are that
//     format — Beast frames counted through beast::Reader and our own Tracker (CRC: an aircraft is counted only when a
//     reply proves it), BaseStation counted as MSG lines through vibe_sbs.h. Plain results:
//       "Connected — 1,214 Beast messages in 3 s, 87 aircraft" · "Connected, but no data in 5 s" ·
//       "Connected, but this is not Beast data — it looks like BaseStation (SBS) text (try BaseStation)" ·
//       "Nothing is listening on 192.168.86.13:32010" · "No route to …, or it timed out" · "Name not found: …"
//     and the warning when the address is THIS machine and nothing listens there (probably meant "listen").
// ★★★ UBERSDR HFDL (vibe_uberhfdl.h): one GET of /addon/hfdl/aircraft — "Connected — UberSDR HFDL, 23 aircraft (14 with
//     positions)", "404: this UberSDR has no HFDL add-on", "not UberSDR HFDL JSON", "Nothing is listening on …".
// ★★★ AUTO-DETECT (Stuart: "the easiest way possible please"): the owner types an address; detect() asks the owner's port
//     (if any), Beast 30005, BaseStation 30003 and the UberSDR on 8080 and 80 AT ONCE — ONE connection per port: what it
//     sends unasked (Beast / BaseStation), else, on the same connection, the HFDL add-on's GET — about 5 s in all, and
//     says what it found ("Found: PiAware ADS-B (Beast, port 30005) — 112 aircraft") with a ready-to-add source and a
//     name, or what it tried. Only the host the owner typed is asked; nothing is written to it but that one GET.
// ★★★ LISTEN: the port can be bound, or it is the port this feed already owns (FeedSet) — and the plain words that the far
//     end must connect to <this server's LAN address>:port.
// ★★ BOUNDED AND OUT OF THE WAY: never touches a running feed (its own sockets only; a listen test binds and closes at
//    once). ONE test at a time per process (Tester); each on its own short-lived thread at the network rung, joined by the
//    request that asked (so it never outlives the server's connection threads, which a stop joins); every step has a
//    deadline and polls a cancel flag in 100 ms slices — LocalSdrShim's stop cancels it. The one exception is a NAME lookup
//    (getaddrinfo cannot be interrupted): it runs on a detached helper that holds nothing but its own shared state — no
//    server object — and is abandoned at its deadline.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#if !(defined(__ANDROID__) && __ANDROID_API__ < 24)
#include <ifaddrs.h>
#else
#include <net/if.h>
#include <sys/ioctl.h>
#endif

#include "vibe_feeds.h"
#include "vibe_thread.h"

namespace vibe {
namespace feeds {
namespace probe {

/** What a test found. `verdict`: ok | quiet | nodata | wrongformat | closed | refused | unreachable | noname | inuse |
 *  owned | free | invalid | busy | cancelled. `ok` = it may be saved without asking ("quiet" and "free" are ok). */
struct Result {
    bool ok = false;
    std::string verdict, line, hint, looks, lanIp, where;
    uint64_t msgs = 0;
    size_t aircraft = 0;
    double secs = 0;
    bool thisMachine = false;
};

struct Opts {
    double resolveSec = 3, connectSec = 3, readSec = 3, silentSec = 5;
    std::string lanIp;                       ///< this server's LAN address, for the listen words ("" = say "this server")
    /** A listen port this server's feeds already use: true, its title, its format, its status line, internal (the ingest). */
    std::function<bool(int port, std::string& title, std::string& fmt, std::string& line, bool& internal)> ownedListen;
    std::function<std::vector<std::string>()> localAddrs;   ///< nullptr = localIpv4s() (a test may give its own)
    std::vector<int> candidates;             ///< auto-detect's ports (empty = detectPorts: the owner's, 30005, 30003, 8080, 80)
};

inline double mono() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
/** 1214 → "1,214". */
inline std::string thousands(uint64_t n) {
    std::string s = std::to_string(n), o;
    for (size_t i = 0; i < s.size(); ++i) { if (i && (s.size() - i) % 3 == 0) o += ','; o += s[i]; }
    return o;
}
inline std::string secsWord(double s) { char b[24]; std::snprintf(b, sizeof b, "%.0f s", std::max(1.0, std::round(s))); return b; }

/** One source from the form's JSON object, through the SAVE's parser (a list of one). */
inline bool parseOne(const std::string& body, SourceCfg& out, std::string& err) {
    std::vector<SourceCfg> v;
    std::string t = body;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\r')) t.pop_back();
    if (t.empty() || t.front() != '{') { err = "the feed to test is not a JSON object"; return false; }
    if (!parseSources("[" + t + "]", v, err)) return false;
    if (v.size() != 1) { err = "give one feed to test"; return false; }
    out = v[0];
    return true;
}

/** This machine's IPv4 addresses (loopback included). */
inline std::vector<std::string> localIpv4s() {
    std::vector<std::string> out{"127.0.0.1"};
#if !(defined(__ANDROID__) && __ANDROID_API__ < 24)
    struct ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) == 0) {
        for (auto* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            char b[INET_ADDRSTRLEN] = {0};
            if (::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr, b, sizeof b)) out.push_back(b);
        }
        ::freeifaddrs(ifa);
    }
#else
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct ifreq reqs[16]; struct ifconf conf; conf.ifc_len = sizeof reqs; conf.ifc_req = reqs;
        if (::ioctl(fd, SIOCGIFCONF, &conf) == 0)
            for (int i = 0; i < conf.ifc_len / int(sizeof(struct ifreq)); ++i) {
                if (reqs[i].ifr_addr.sa_family != AF_INET) continue;
                char b[INET_ADDRSTRLEN] = {0};
                if (::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&reqs[i].ifr_addr)->sin_addr, b, sizeof b)) out.push_back(b);
            }
        ::close(fd);
    }
#endif
    return out;
}
inline std::string addrText(const sockaddr* sa) {
    char b[INET6_ADDRSTRLEN + 1] = {0};
    if (sa->sa_family == AF_INET6) {
        ::inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, b, sizeof b);
        std::string s = b; return s.rfind("::ffff:", 0) == 0 ? s.substr(7) : s;
    }
    ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, b, sizeof b);
    return b;
}

// ══ WHAT THE BYTES ARE (pure: test-feed-test.cpp drives these directly) ════════════════════════════════════════════════
struct Count { uint64_t msgs = 0; size_t aircraft = 0; };
/** Beast: frames through beast::Reader, aircraft through our own Tracker (CRC-checked, as a feed source counts them). */
inline Count countBeast(const std::vector<uint8_t>& b) {
    beast::Reader r; std::vector<beast::Decoded> out;
    r.feed(b.data(), b.size(), out);
    vibeadsb::Tracker tr; tr.setMaxRangeNm(360);
    Count c;
    double t = 0;
    for (const auto& d : out) {
        ++c.msgs;
        const int len = int(d.msg.size());
        if (d.type == '1' || (len != 7 && len != 14)) continue;
        vibeadsb::Frame f; std::memcpy(f.b, d.msg.data(), size_t(len)); f.len = len; f.sample = d.ts * 2 / 3; f.level = -20;
        tr.accept(f, t += 0.001);
    }
    for (const auto& kv : tr.aircraft()) if (!kv.second.nonIcao) ++c.aircraft;
    return c;
}
/** BaseStation: MSG lines that parse (vibe_sbs.h), aircraft = distinct addresses. */
inline Count countSbs(const std::vector<uint8_t>& b) {
    sbs::LineSplitter sp; std::set<uint32_t> ac; Count c;
    sp.feed(b.data(), b.size(), [&](const std::string& line) { sbs::Msg m; if (sbs::parseLine(line, m)) { ++c.msgs; ac.insert(m.icao); } });
    c.aircraft = ac.size();
    return c;
}
/** What a sample looks like: "sbs" | "beast" | "avr" | "http" | "json" | "text" | "binary" | "" (nothing). */
inline std::string sniff(const std::vector<uint8_t>& b) {
    if (b.empty()) return "";
    const size_t n = std::min<size_t>(b.size(), 8192);
    // Beast: 0x1A then a frame type, several times, and frames the reader accepts
    int marks = 0;
    for (size_t i = 0; i + 1 < n; ++i) if (b[i] == 0x1a && (b[i + 1] == '1' || b[i + 1] == '2' || b[i + 1] == '3')) ++marks;
    if (marks >= 2) { std::vector<uint8_t> s(b.begin(), b.begin() + long(n)); if (countBeast(s).msgs >= 2) return "beast"; }
    size_t printable = 0;
    for (size_t i = 0; i < n; ++i) { const uint8_t c = b[i]; if ((c >= 0x20 && c < 0x7f) || c == '\n' || c == '\r' || c == '\t') ++printable; }
    if (printable * 10 < n * 9) return "binary";
    const std::string t(b.begin(), b.begin() + long(n));
    if (t.rfind("HTTP/", 0) == 0 || t.find("<html") != std::string::npos || t.find("<!DOCTYPE") != std::string::npos || t.find("<!doctype") != std::string::npos) return "http";
    // line by line
    int sbsL = 0, avrL = 0, lines = 0;
    size_t a = 0;
    while (a < t.size() && lines < 200) {
        size_t e = t.find('\n', a); if (e == std::string::npos) e = t.size();
        std::string l = sbs::trim(t.substr(a, e - a)); a = e + 1;
        if (l.empty()) continue;
        ++lines;
        if (l.rfind("MSG,", 0) == 0 || l.rfind("SEL,", 0) == 0 || l.rfind("ID,", 0) == 0 || l.rfind("AIR,", 0) == 0 || l.rfind("STA,", 0) == 0 || l.rfind("CLK,", 0) == 0) ++sbsL;
        else if ((l[0] == '*' || l[0] == '@') && l.back() == ';') ++avrL;
    }
    if (sbsL && sbsL * 2 >= lines) return "sbs";
    if (avrL && avrL * 2 >= lines) return "avr";
    const char f0 = t.find_first_not_of(" \r\n\t") == std::string::npos ? 0 : t[t.find_first_not_of(" \r\n\t")];
    if (f0 == '{' || f0 == '[') return "json";
    return "text";
}
inline const char* looksWords(const std::string& k) {
    if (k == "sbs") return "it looks like BaseStation (SBS) text (try BaseStation)";
    if (k == "beast") return "it looks like Beast data (try Beast)";
    if (k == "avr") return "it looks like AVR raw text (dump1090's port 30002; Beast is usually on 30005)";
    if (k == "http") return "it looks like a web page (that port is a web server)";
    if (k == "json") return "it looks like JSON (a web or data service, not a feed)";
    if (k == "text") return "it is text, but not BaseStation lines";
    return "the bytes are not a feed format we know";
}

/** Judge what a connect test read. `fmt`/`label` = the source's; `got` = every byte read; `secs` = how long it read. */
inline void judge(const SourceCfg& c, const std::vector<uint8_t>& got, double secs, bool closedEarly, Result& r) {
    r.secs = secs;
    const bool beastFmt = c.fmt == "beast";
    const std::string fmtWord = beastFmt ? "Beast" : "BaseStation";
    if (got.empty()) {
        if (closedEarly) { r.verdict = "closed"; r.line = "Connected, but the other end closed the connection without sending anything";
                           r.hint = "it may only accept certain addresses, or that port is not its " + fmtWord + " output"; return; }
        // ★ a datalink sender (HFDL, VDL2, ACARS) can go minutes between aircraft: connected is the test passed
        if (!beastFmt && c.label != "adsb") {
            r.ok = true; r.verdict = "quiet";
            r.line = "Connected, but nothing in " + secsWord(secs) + " \xe2\x80\x94 normal for " + sbs::labelWords(c.label) + ", which can go minutes between aircraft";
            return;
        }
        r.verdict = "nodata"; r.line = "Connected, but no data in " + secsWord(secs);
        r.hint = beastFmt ? "is this the Beast output port (30005 on dump1090-fa and readsb)?" : "is this the BaseStation output port (30003 on dump1090)?";
        return;
    }
    const Count n = beastFmt ? countBeast(got) : countSbs(got);
    const std::string k = sniff(got);
    r.looks = k;
    const bool right = beastFmt ? (n.msgs > 0 && (k == "beast" || k == "binary")) : (n.msgs > 0 && k == "sbs");
    if (!right) {
        r.verdict = "wrongformat";
        r.line = "Connected, but this is not " + fmtWord + " data \xe2\x80\x94 " + looksWords(k);
        return;
    }
    r.ok = true; r.verdict = "ok"; r.msgs = n.msgs; r.aircraft = n.aircraft;
    r.line = "Connected \xe2\x80\x94 " + thousands(n.msgs) + " " + fmtWord + " message" + (n.msgs == 1 ? "" : "s") + " in " + secsWord(secs)
           + ", " + thousands(n.aircraft) + " aircraft";
}

// ══ THE TEST ══════════════════════════════════════════════════════════════════════════════════════════════════════════
inline const char* stopped() { return "The test was stopped (the server is stopping)"; }
inline bool isMine(const std::vector<sockaddr_storage>& addrs, const Opts& o) {
    const std::vector<std::string> mine = o.localAddrs ? o.localAddrs() : localIpv4s();
    for (const auto& s : addrs) {
        const std::string a = addrText(reinterpret_cast<const sockaddr*>(&s));
        if (a == "::1" || a.rfind("127.", 0) == 0 || std::find(mine.begin(), mine.end(), a) != mine.end()) return true;
    }
    return false;
}
/** Read a stream socket: until `readSec` once bytes flow, else until `silentSec`; bounded at 4 MB. */
inline std::vector<uint8_t> readFor(int fd, double readSec, double silentSec, const std::function<bool()>& cancelled, bool& closed, double& secs) {
    std::vector<uint8_t> got, buf(16 * 1024);
    const double t0 = mono();
    closed = false;
    while (!cancelled()) {
        const double el = mono() - t0;
        if ((!got.empty() && el >= readSec) || el >= silentSec) break;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 100) <= 0) continue;
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n > 0) { if (got.size() < 4u * 1024 * 1024) got.insert(got.end(), buf.begin(), buf.begin() + n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        closed = true; break;
    }
    secs = mono() - t0;
    return got;
}
/** "Connected — UberSDR HFDL, 23 aircraft (14 with positions)", or why not. */
inline void judgeUber(const feednet::HttpResult& h, const std::string& where, Result& r) {
    std::string why = uberhfdl::failWords(h, where);
    if (why == "stopped") { r.verdict = "cancelled"; r.line = stopped(); return; }
    if (h.fail == "refused") { r.verdict = "refused"; r.line = why; r.hint = "is the UberSDR on, and is this its web port (8080)?"; return; }
    if (h.fail == "unreachable" || h.fail == "noname") { r.verdict = h.fail; r.line = why; return; }
    if (!why.empty()) { r.verdict = "wrongformat"; r.line = "Connected, but " + why; return; }
    uberhfdl::Parsed p; std::string e;
    const int64_t nowMs = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    if (!uberhfdl::parse(h.body, nowMs, 30 * 60 * 1000, p, e)) { r.verdict = "wrongformat"; r.line = "Connected, but this is " + e; return; }
    r.ok = true; r.verdict = "ok"; r.aircraft = p.withIcao; r.msgs = p.rows;
    r.line = "Connected \xe2\x80\x94 UberSDR HFDL, " + thousands(p.withIcao) + " aircraft (" + thousands(p.withPos) + " with positions)";
}

/** The test itself, synchronous and bounded (≈ resolveSec + connectSec + silentSec at most). `cancelled` polled each 100 ms. */
inline Result run(const SourceCfg& c, const Opts& o, const std::function<bool()>& cancelled) {
    Result r;
    r.lanIp = o.lanIp;
    const double t0 = mono();
    if (c.dir == "listen") {
        r.where = "port " + std::to_string(c.port);
        const std::string there = (o.lanIp.empty() ? std::string("this server's address") : o.lanIp) + ":" + std::to_string(c.port);
        const std::string who = c.fmt == "sbs" ? "The sender (dumphfdl, dumpvdl2)" : "The sender (readsb's net-connector)";
        r.hint = who + " must connect to " + there + (o.lanIp.empty() ? "" : " \xe2\x80\x94 this server's address on your network") + ".";
        std::string title, fmt, line; bool internal = false;
        if (o.ownedListen && o.ownedListen(c.port, title, fmt, line, internal)) {
            if (internal) { r.verdict = "inuse"; r.line = "Port " + std::to_string(c.port) + " is used by this server itself (its radios' own ADS-B)"; return r; }
            if (fmt != c.fmt) { r.verdict = "inuse"; r.line = "Port " + std::to_string(c.port) + " is already used by the feed \xe2\x80\x9c" + title + "\xe2\x80\x9d"; return r; }
            r.ok = true; r.verdict = "owned";
            r.line = "This feed already listens on port " + std::to_string(c.port) + (line.empty() ? "" : " \xe2\x80\x94 " + line);
            return r;
        }
        int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        const bool v6 = fd >= 0;
        if (v6) { int off = 0; setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off); }
        else fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { r.verdict = "inuse"; r.line = std::string("Cannot open a socket: ") + std::strerror(errno); return r; }
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   // ★ as the Listener binds
        int rc;
        if (v6) { sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_addr = in6addr_any; a.sin6_port = htons(uint16_t(c.port)); rc = ::bind(fd, (sockaddr*)&a, sizeof a); }
        else    { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(uint16_t(c.port)); rc = ::bind(fd, (sockaddr*)&a, sizeof a); }
        int e = errno;
        if (rc == 0 && ::listen(fd, 1) != 0) { rc = -1; e = errno; }
        ::close(fd);
        if (rc == 0 && v6) {   // ★ and on plain IPv4 too: some systems let a dual-stack bind sit beside an IPv4 listener
            const int f4 = ::socket(AF_INET, SOCK_STREAM, 0);
            if (f4 >= 0) {
                setsockopt(f4, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
                sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(uint16_t(c.port));
                if (::bind(f4, (sockaddr*)&a, sizeof a) != 0 || ::listen(f4, 1) != 0) { rc = -1; e = errno; }
                ::close(f4);
            }
        }
        r.secs = mono() - t0;
        if (rc != 0) {
            r.verdict = "inuse";
            r.line = e == EADDRINUSE ? "Port " + std::to_string(c.port) + " is in use by another program on this server"
                                     : "Cannot listen on port " + std::to_string(c.port) + ": " + std::strerror(e);
            return r;
        }
        r.ok = true; r.verdict = "free";
        r.line = "Port " + std::to_string(c.port) + " is free \xe2\x80\x94 this server can listen on it";
        return r;
    }
    // ── connect ──
    r.where = beast::hostPortText(c.host, c.port);
    std::vector<sockaddr_storage> addrs;
    const int grc = feednet::resolve(c.host, c.port, o.resolveSec, cancelled, addrs);
    if (cancelled()) { r.verdict = "cancelled"; r.line = stopped(); return r; }
    if (grc != 0 || addrs.empty()) {
        r.secs = mono() - t0; r.verdict = "noname";
        r.line = "Name not found: " + c.host + (grc == EAI_AGAIN ? " (no answer in " + secsWord(o.resolveSec) + ")" : "");
        return r;
    }
    r.thisMachine = isMine(addrs, o);
    int lastErr = 0;
    const int fd = feednet::connectTo(addrs, o.connectSec, cancelled, lastErr);
    if (cancelled()) { if (fd >= 0) ::close(fd); r.verdict = "cancelled"; r.line = stopped(); return r; }
    if (fd < 0) {
        r.secs = mono() - t0;
        if (lastErr == ECONNREFUSED) {
            r.verdict = "refused"; r.line = "Nothing is listening on " + r.where;
            if (r.thisMachine && c.fmt != "uberhfdl")
                r.hint = "That address is this server. " + std::string(c.fmt == "sbs" ? "dumphfdl and dumpvdl2 connect OUT to an address" : "A program that pushes its feed connects OUT")
                       + " \xe2\x80\x94 did you mean \xe2\x80\x9cIt connects to this server (listen on a port)\xe2\x80\x9d?";
            else r.hint = c.fmt == "beast" ? "is dump1090's Beast output on, on that port (30005)?" : c.fmt == "uberhfdl" ? "is the UberSDR on, and is this its web port (8080)?"
                                                                                                  : "is the sender's BaseStation output on, on that port?";
        } else {
            r.verdict = "unreachable";
            r.line = "No route to " + c.host + ", or it timed out (" + secsWord(o.connectSec) + ")";
            r.hint = "is that machine on, and on this network?";
        }
        return r;
    }
    if (c.fmt == "uberhfdl") {
        const double hs = mono();
        const auto h = feednet::httpOn(fd, c.host, c.port, uberhfdl::kPath, mono() + o.silentSec, uberhfdl::kMaxBody, cancelled);
        ::close(fd);
        r.secs = mono() - hs;
        judgeUber(h, r.where, r);
        return r;
    }
    bool closed = false; double secs = 0;
    const std::vector<uint8_t> got = readFor(fd, o.readSec, o.silentSec, cancelled, closed, secs);
    ::close(fd);
    if (cancelled()) { r.verdict = "cancelled"; r.line = stopped(); return r; }
    judge(c, got, got.empty() ? secs : std::max(secs, 0.5), closed, r);
    return r;
}

// ══ AUTO-DETECT: the owner types an address; we find what is there (Stuart: "the easiest way possible please") ══════════
/** One thing found: the source to add, a suggested name, the words. */
struct Hit { SourceCfg cfg; std::string name, line; uint64_t msgs = 0; size_t aircraft = 0; };
struct Detected {
    std::vector<Hit> hits;
    std::vector<std::string> tried;   ///< "30005: nothing listening" … (every port asked, in order)
    std::string line, hint, host, lanIp;
    bool thisMachine = false;
    double secs = 0;
};
/** The ports asked, in order: the owner's own first (if any), then Beast 30005, BaseStation 30003, UberSDR 8080 and 80. */
inline std::vector<int> detectPorts(int given) {
    std::vector<int> v;
    if (given > 0) v.push_back(given);
    for (int p : {beast::kDefaultPort, 30003, uberhfdl::kDefaultPort, 80}) if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
    return v;
}
/** ★ ONE CONNECTION PER PORT: read what it sends (Beast or BaseStation stream unasked); if it says nothing, ask the SAME
 *  connection for the UberSDR HFDL add-on's JSON (a web server waits for a request). Read-only; nothing else is sent. */
inline void probePort(const std::string& host, int port, const std::vector<sockaddr_storage>& addrs0, double connectSec,
                      double silentSec, double httpSec, const std::function<bool()>& cancelled, std::vector<Hit>& hits, std::string& tried) {
    const std::string ps = std::to_string(port);
    std::vector<sockaddr_storage> addrs = addrs0;
    for (auto& s : addrs) {   // the resolved addresses, at this port
        if (s.ss_family == AF_INET6) reinterpret_cast<sockaddr_in6*>(&s)->sin6_port = htons(uint16_t(port));
        else reinterpret_cast<sockaddr_in*>(&s)->sin_port = htons(uint16_t(port));
    }
    int err = 0;
    const int fd = feednet::connectTo(addrs, connectSec, cancelled, err);
    if (fd < 0) { tried = ps + ": " + (err == ECONNREFUSED ? "nothing listening" : "no answer"); return; }
    bool closed = false; double secs = 0;
    const std::vector<uint8_t> got = readFor(fd, 2.0, silentSec, cancelled, closed, secs);
    if (!got.empty()) {
        ::close(fd);
        const std::string k = sniff(got);
        if (k == "beast") {
            const Count n = countBeast(got);
            Hit h; h.cfg.fmt = "beast"; h.cfg.dir = "connect"; h.cfg.host = host; h.cfg.port = port; h.cfg.label = "adsb";
            h.name = "PiAware"; h.msgs = n.msgs; h.aircraft = n.aircraft;
            h.line = "PiAware ADS-B (Beast, port " + ps + ") \xe2\x80\x94 " + thousands(n.aircraft) + " aircraft";
            hits.push_back(h); tried = ps + ": Beast"; return;
        }
        if (k == "sbs") {
            const Count n = countSbs(got);
            Hit h; h.cfg.fmt = "sbs"; h.cfg.dir = "connect"; h.cfg.host = host; h.cfg.port = port; h.cfg.label = "adsb";
            h.name = "BaseStation feed"; h.msgs = n.msgs; h.aircraft = n.aircraft;
            h.line = "ADS-B as BaseStation text (port " + ps + ") \xe2\x80\x94 " + thousands(n.aircraft) + " aircraft";
            hits.push_back(h); tried = ps + ": BaseStation"; return;
        }
        tried = ps + ": " + std::string(k == "http" ? "a web server" : k == "avr" ? "AVR raw text (not supported)" : k == "json" ? "JSON, not a feed" : "not a feed format");
        return;
    }
    if (closed || cancelled()) { ::close(fd); tried = ps + ": closed at once"; return; }
    const auto hr = feednet::httpOn(fd, host, port, uberhfdl::kPath, mono() + httpSec, uberhfdl::kMaxBody, cancelled);
    ::close(fd);
    if (hr.fail.empty() && hr.status == 200) {
        uberhfdl::Parsed p; std::string e;
        const int64_t nowMs = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        if (uberhfdl::parse(hr.body, nowMs, 30 * 60 * 1000, p, e)) {
            Hit h; h.cfg.fmt = "uberhfdl"; h.cfg.dir = "connect"; h.cfg.host = host; h.cfg.port = port; h.cfg.label = "hfdl";
            h.name = "UberSDR HFDL"; h.msgs = p.rows; h.aircraft = p.withIcao;
            h.line = "UberSDR HFDL (port " + ps + ") \xe2\x80\x94 " + thousands(p.withIcao) + " aircraft (" + thousands(p.withPos) + " with positions)";
            hits.push_back(h); tried = ps + ": UberSDR HFDL"; return;
        }
        tried = ps + ": a web server, not UberSDR HFDL"; return;
    }
    if (hr.fail.empty() && hr.status == 404) { tried = ps + ": a web server with no HFDL add-on (404)"; return; }
    if (hr.fail.empty()) { tried = ps + ": a web server (HTTP " + std::to_string(hr.status) + ")"; return; }
    tried = ps + ": connected, but silent";
}
/** ★ Probe every candidate port of `host` AT ONCE (one thread each, joined), about 5 s in all. */
inline Detected detect(const std::string& hostIn, int given, const Opts& o, const std::function<bool()>& cancelled) {
    Detected d; d.lanIp = o.lanIp;
    const double t0 = mono();
    std::string host, err; int pp = 0;
    if (!beast::parseHostPort(hostIn, host, pp, err)) { d.line = err; return d; }
    if (given <= 0 && hostIn.find(':') != std::string::npos && (hostIn.find(']') != std::string::npos || std::count(hostIn.begin(), hostIn.end(), ':') == 1)) given = pp;
    d.host = host;
    std::vector<sockaddr_storage> addrs;
    const int grc = feednet::resolve(host, 1, std::min(2.0, o.resolveSec), cancelled, addrs);
    if (cancelled()) { d.line = stopped(); return d; }
    if (grc != 0 || addrs.empty()) { d.line = "Name not found: " + host; d.secs = mono() - t0; return d; }
    d.thisMachine = isMine(addrs, o);
    const std::vector<int> ports = o.candidates.empty() ? detectPorts(given) : o.candidates;
    std::vector<std::vector<Hit>> hits(ports.size());
    std::vector<std::string> tried(ports.size());
    std::vector<std::thread> th;
    for (size_t i = 0; i < ports.size(); ++i)
        th.emplace_back([&, i] { vibeNetThread("vibe-feedfind"); probePort(host, ports[i], addrs, 1.5, 1.2, 1.8, cancelled, hits[i], tried[i]); });
    for (auto& t : th) t.join();
    bool beastHit = false, uberKept = false;
    for (auto& v : hits) for (auto& h : v) if (h.cfg.fmt == "beast") beastHit = true;
    for (size_t i = 0; i < ports.size(); ++i) {
        d.tried.push_back(tried[i]);
        for (auto& h : hits[i]) {
            if (h.cfg.fmt == "sbs" && h.cfg.label == "adsb" && beastHit) continue;   // ★ the same aircraft: Beast is the better copy
            // ★ one UberSDR answers on 8080 AND 80 (seen live, 11-27): the same aircraft — keep the first port asked
            if (h.cfg.fmt == "uberhfdl") { if (uberKept) continue; uberKept = true; }
            d.hits.push_back(h);
        }
    }
    d.secs = mono() - t0;
    if (cancelled()) { d.hits.clear(); d.line = stopped(); return d; }
    std::string tr; for (auto& t : d.tried) tr += (tr.empty() ? "" : " \xc2\xb7 ") + t;
    if (d.hits.empty()) {
        d.line = "Nothing found on " + host + " \xe2\x80\x94 tried " + tr;
        d.hint = d.thisMachine ? "That address is this server. dumphfdl and dumpvdl2 connect OUT: open Advanced and choose \xe2\x80\x9cIt connects to this server (listen on a port)\xe2\x80\x9d."
                               : "Is it on? For a listening feed or an unusual port, open Advanced.";
    } else {
        d.line = "Found: " + d.hits[0].line;
        for (size_t i = 1; i < d.hits.size(); ++i) d.line += " \xc2\xb7 also " + d.hits[i].line;
    }
    return d;
}

inline std::string cfgJson(const SourceCfg& c) {
    std::string o = "{\"fmt\":\"" + c.fmt + "\",\"dir\":\"" + c.dir + "\"";
    if (c.dir == "connect") o += ",\"host\":\"" + beast::jsonEsc(c.host) + "\"";
    o += ",\"port\":" + std::to_string(c.port) + ",\"label\":\"" + c.label + "\"";
    if (!c.name.empty()) o += ",\"name\":\"" + beast::jsonEsc(c.name) + "\"";
    return o + "}";
}
inline std::string toJson(const Result& r) {
    char b[96];
    std::snprintf(b, sizeof b, ",\"msgs\":%llu,\"aircraft\":%zu,\"secs\":%.1f", (unsigned long long)r.msgs, r.aircraft, r.secs);
    return std::string("{\"ok\":") + (r.ok ? "true" : "false") + ",\"verdict\":\"" + r.verdict + "\",\"line\":\"" + beast::jsonEsc(r.line)
         + "\",\"hint\":\"" + beast::jsonEsc(r.hint) + "\",\"looks\":\"" + r.looks + "\",\"where\":\"" + beast::jsonEsc(r.where)
         + "\",\"lanIp\":\"" + beast::jsonEsc(r.lanIp) + "\",\"thisMachine\":" + (r.thisMachine ? "true" : "false") + b + "}";
}
inline std::string toJson(const Detected& d) {
    std::string o = std::string("{\"found\":") + (d.hits.empty() ? "false" : "true") + ",\"line\":\"" + beast::jsonEsc(d.line)
                  + "\",\"hint\":\"" + beast::jsonEsc(d.hint) + "\",\"host\":\"" + beast::jsonEsc(d.host) + "\",\"lanIp\":\"" + beast::jsonEsc(d.lanIp)
                  + "\",\"thisMachine\":" + (d.thisMachine ? "true" : "false") + ",\"hits\":[";
    for (size_t i = 0; i < d.hits.size(); ++i) {
        SourceCfg c = d.hits[i].cfg; c.name = d.hits[i].name;
        o += (i ? "," : "") + std::string("{\"src\":") + cfgJson(c) + ",\"name\":\"" + beast::jsonEsc(d.hits[i].name) + "\",\"line\":\""
           + beast::jsonEsc(d.hits[i].line) + "\",\"aircraft\":" + std::to_string(d.hits[i].aircraft) + "}";
    }
    o += "],\"tried\":[";
    for (size_t i = 0; i < d.tried.size(); ++i) o += (i ? ",\"" : "\"") + beast::jsonEsc(d.tried[i]) + "\"";
    char b[32]; std::snprintf(b, sizeof b, "],\"secs\":%.1f}", d.secs);
    return o + b;
}

/** ★★ ONE TEST AT A TIME PER PROCESS, each on its own short-lived thread at the network rung, joined before it returns.
 *  cancel() (LocalSdrShim's stop) ends a running one within ~100 ms. Never destroyed (a request may still be in it). */
class Tester {
public:
    static Tester& shared() { static Tester* t = new Tester(); return *t; }
    Result test(const SourceCfg& c, const Opts& o) {
        std::unique_lock<std::mutex> one(busy_, std::try_to_lock);
        if (!one.owns_lock()) { Result r; r.verdict = "busy"; r.line = busyWords(); return r; }
        const uint64_t gen = gen_.load();
        auto res = std::make_shared<Result>();
        std::thread th([this, c, o, res, gen] { vibeNetThread("vibe-feedtest"); *res = run(c, o, [this, gen] { return gen_.load() != gen; }); });
        th.join();
        ++tests_;
        return *res;
    }
    /** false = another test is running (`d.line` says so). */
    bool find(const std::string& host, int port, const Opts& o, Detected& d) {
        std::unique_lock<std::mutex> one(busy_, std::try_to_lock);
        if (!one.owns_lock()) { d.line = busyWords(); return false; }
        const uint64_t gen = gen_.load();
        auto res = std::make_shared<Detected>();
        std::thread th([this, host, port, o, res, gen] { vibeNetThread("vibe-feedtest"); *res = detect(host, port, o, [this, gen] { return gen_.load() != gen; }); });
        th.join();
        ++tests_;
        d = *res;
        return true;
    }
    void cancel() { ++gen_; }
    uint64_t tests() const { return tests_.load(); }
private:
    static std::string busyWords() { return "Another feed test is running \xe2\x80\x94 try again in a few seconds"; }
    std::mutex busy_;
    std::atomic<uint64_t> gen_{0}, tests_{0};
};

}  // namespace probe
}  // namespace feeds
}  // namespace vibe
