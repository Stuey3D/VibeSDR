// vibe_feeds.h — V12 beta96: SERVER-LEVEL AIRCRAFT FEED SOURCES (docs/v12/ADSB-FEED.md). Stuart 2026-11-26: "feeds are
// NOT radios". Machine-wide sources of aircraft — another receiver's Beast output (PiAware's dump1090 on a Pi), or
// BaseStation (SBS-1) text (dumphfdl on an UberSDR) — feeding the SHARED aircraft table (vibe_aircraft_share.h) directly,
// so every radio's AIR & SEA map and the community map show them. No radio entry, no tab, no landing of their own.
//
// ★★★ ONE OWNER PER MACHINE: on Linux / the Mac, the FRONT DOOR process (or the one process of a SIMPLE server); on
//     Android, the single server. Radio processes behind a door run none of this — they read the table and the status
//     file this writes (kStatusFile in the runtime directory).
// ★★★ EACH SOURCE: FORMAT (Beast | SBS) × DIRECTION (connect to host:port | listen on port N) × LABEL (ADS-B | HFDL | VDL2 |
//     ACARS — which bit of the shared table its aircraft carry; an HFDL source is never counted as ADS-B).
//       · Beast is Mode S: always ADS-B. Its replies go through our own Tracker (vibe_adsb.h: CRC, CPR) and, when
//         believed, on to the hub (vibe_beast.h Hub) BYTE-FAITHFUL — the upstream receiver's timestamp and signal byte as
//         received — for the server-level flight-tracker destinations and the local Beast output.
//       · SBS is text with absolute positions (vibe_sbs.h): straight into the table. Never sent to an ADS-B aggregator:
//         its positions are not Mode S frames (and dumphfdl asks that its feed is not sent to public ADS-B networks).
// ★★ LISTEN = LAN ONLY, the same rule as the Beast server and RAW IQ OUT's "Local only" (beast::peerAllowed with open =
//    false): anyone else is refused at accept and logged (one line per address, then at most one every 10 minutes). At
//    most kMaxClients connected at once.
// ★★ RESILIENCE, as the Beast client (beast::BeastInput, beast::Outage): connect sources reconnect for ever (2 s → 60 s,
//    ±20 %); a listen source accepts reconnects for ever. A source is DOWN when its link has been gone (connect) or nobody
//    has been connected (listen) for kStallSec, or — an ADS-B source only — nothing has arrived for kStallSec (a busy sky
//    never goes quiet for 90 s; a datalink source reports minutes apart, so silence alone never makes it down). The AIR &
//    SEA map's banner names the source: "ADS-B source lost — reconnecting… · HFDL still live".
// ★★ BOUNDED: kMaxSources sources, kMaxClients per listener, one 16 KB read at a time, vibe_sbs.h's line and row caps, the
//    Tracker's own aging. Every socket on its source's own thread (Network), never audio or DSP.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vibe_beast.h"
#include "vibe_sbs.h"
#include "vibe_adsb.h"
#include "vibe_aircraft_share.h"

namespace vibe {
namespace feeds {

constexpr int kMaxSources = 8;
/** ★★ beta96 the "radio" every feed source's aircraft are written as in the shared table (vibeavshare::Table::write's asId —
 *  b96-mapchat's registry, radios.idx): a map's OTHER RADIOS switch can hide them; they never count as a radio's own. */
inline constexpr const char* kFeedRadioId = "feed";
constexpr int kMaxClients = 4;
constexpr int kSbsPort = 32010;                       ///< the port Stuart's dumphfdl sends to (any free port works)
constexpr const char* kStatusFile = "aircraft-feeds.json";
constexpr const char* kIngestPortFile = "adsb-ingest.port";
constexpr double kStatusEverySec = 2.0;
/** How long without a link (or, ADS-B, without data) makes a source DOWN: beast::kStallSec (90 s). A test shortens it. */
inline double& stallSec() { static double s = beast::kStallSec; return s; }

/** One source as the owner set it. */
struct SourceCfg {
    std::string fmt = "beast";     ///< "beast" | "sbs"
    std::string dir = "connect";   ///< "connect" | "listen"
    std::string host;              ///< connect only
    int port = beast::kDefaultPort;
    std::string label = "adsb";    ///< "adsb" | "hfdl" | "vdl2" | "acars"
    std::string name;              ///< the owner's name for it ("PiAware", "UberSDR HFDL"), optional
    bool internal = false;         ///< ★ the radios' own ADS-B coming in over loopback (never configured, never shown)
};
inline bool labelOk(const std::string& l) { return l == "adsb" || l == "hfdl" || l == "vdl2" || l == "acars"; }
inline std::string key(const SourceCfg& c) { return c.fmt + "|" + c.dir + "|" + c.host + "|" + std::to_string(c.port) + "|" + c.label + "|" + c.name; }
/** What a source is called in a status line or banner: its name, else its label ("HFDL source"). */
inline std::string title(const SourceCfg& c) { return c.name.empty() ? std::string(sbs::labelWords(c.label)) + " source" : c.name; }

/** Read the owner's list (ServerConfig::aircraftFeeds). "" = none. Refused in words, never half-applied. */
inline bool parseSources(const std::string& text, std::vector<SourceCfg>& out, std::string& err) {
    out.clear();
    if (text.empty()) return true;
    vibe::antswitch::Json j;
    if (!vibe::antswitch::parseJson(text, j) || j.kind != vibe::antswitch::Json::Arr) { err = "the aircraft feed list is not a JSON list"; return false; }
    using J = vibe::antswitch::Json;
    if ((int)j.a.size() > kMaxSources) { err = "at most " + std::to_string(kMaxSources) + " aircraft feeds"; return false; }
    std::set<int> listenPorts;
    for (const J& e : j.a) {
        if (e.kind != J::Obj) { err = "an aircraft feed entry is not an object"; return false; }
        SourceCfg c;
        c.fmt = e.str("fmt", "beast"); c.dir = e.str("dir", "connect"); c.label = e.str("label", c.fmt == "sbs" ? "hfdl" : "adsb");
        c.name = sbs::trim(e.str("name"));
        if (c.name.size() > 40) c.name.resize(40);
        for (char& ch : c.name) if ((unsigned char)ch < 0x20 || ch == '"' || ch == '\\') ch = ' ';
        if (c.fmt != "beast" && c.fmt != "sbs") { err = "a feed's format is Beast or BaseStation (SBS)"; return false; }
        if (c.dir != "connect" && c.dir != "listen") { err = "a feed either connects to an address or listens on a port"; return false; }
        if (!labelOk(c.label)) { err = "a feed's label is ADS-B, HFDL, VDL2 or ACARS"; return false; }
        if (c.fmt == "beast") c.label = "adsb";   // ★ Beast is Mode S: nothing else
        const J* p = e.get("port");
        const double pd = p && p->kind == J::Num ? p->n : (c.fmt == "sbs" ? kSbsPort : beast::kDefaultPort);
        if (c.dir == "connect") {
            std::string h; int pt = 0;
            if (!beast::parseHostPort(beast::hostPortText(e.str("host"), int(pd)), h, pt, err)) return false;
            if (pd < 1 || pd > 65535 || pd != std::floor(pd)) { err = "the port for " + h + " must be 1 to 65535"; return false; }
            c.host = h; c.port = int(pd);
        } else {
            if (pd < 1024 || pd > 65535 || pd != std::floor(pd)) { err = "a feed listens on a port from 1024 to 65535"; return false; }
            c.port = int(pd);
            if (!listenPorts.insert(c.port).second) { err = "two feeds cannot listen on the same port (" + std::to_string(c.port) + ")"; return false; }
        }
        out.push_back(c);
    }
    return true;
}
inline std::string sourcesJson(const std::vector<SourceCfg>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        const SourceCfg& c = v[i];
        if (c.internal) continue;
        if (o.size() > 1) o += ',';
        o += "{\"fmt\":\"" + c.fmt + "\",\"dir\":\"" + c.dir + "\"";
        if (c.dir == "connect") o += ",\"host\":\"" + beast::jsonEsc(c.host) + "\"";
        o += ",\"port\":" + std::to_string(c.port) + ",\"label\":\"" + c.label + "\"";
        if (!c.name.empty()) o += ",\"name\":\"" + beast::jsonEsc(c.name) + "\"";
        o += "}";
    }
    return o + "]";
}

// ══ THE LISTENER: a source that waits for the far end to connect (dumphfdl's tcp output connects OUT) ═════════════════
class Listener {
public:
    /** Bytes from client `id` (a new id per connection). Returns how many messages they completed. */
    using Sink = std::function<size_t(int id, const uint8_t*, size_t)>;
    ~Listener() { stop(); }
    void setLog(std::function<void(const std::string&)> f) { std::lock_guard<std::mutex> lk(m_); log_ = std::move(f); }
    /** A test's own rule in place of the LAN rule (nullptr = beast::peerAllowed, LAN only). */
    void setAllow(std::function<bool(const std::string&)> f) { std::lock_guard<std::mutex> lk(m_); allow_ = std::move(f); }
    /** `loopbackOnly`: the radios' own ADS-B ingest (127.0.0.1 only). `port` 0 = any free port (the ingest). */
    void start(int port, bool loopbackOnly, Sink sink, const std::string& name) {
        stop();
        std::lock_guard<std::mutex> lk(m_);
        port_ = port; loop_ = loopbackOnly; sink_ = std::move(sink); name_ = name;
        run_ = true; err_.clear(); startMono_ = mono();
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        std::thread t;
        { std::lock_guard<std::mutex> lk(m_); if (!run_) return; run_ = false; t = std::move(th_); }
        if (t.joinable()) t.join();
    }
    int boundPort() const { std::lock_guard<std::mutex> lk(m_); return bound_; }
    size_t clients() const { std::lock_guard<std::mutex> lk(m_); return peers_.size(); }
    std::vector<std::string> peers() const { std::lock_guard<std::mutex> lk(m_); return peers_; }
    /** Seconds since a client was last connected (0 while one is); since the start when none has ever been. */
    double secsWithoutClient() const { std::lock_guard<std::mutex> lk(m_); return peers_.empty() ? mono() - (lastClient_ > 0 ? lastClient_ : startMono_) : 0; }
    double secsSinceData() const { std::lock_guard<std::mutex> lk(m_); return mono() - (lastData_ > 0 ? lastData_ : startMono_); }
    bool everData() const { std::lock_guard<std::mutex> lk(m_); return lastData_ > 0; }
    uint64_t refused() const { std::lock_guard<std::mutex> lk(m_); return refused_; }
    std::string lastRefused() const { std::lock_guard<std::mutex> lk(m_); return lastRefused_; }
    std::string error() const { std::lock_guard<std::mutex> lk(m_); return err_; }
    uint64_t msgs() const { std::lock_guard<std::mutex> lk(m_); return msgs_; }
    double perSec() const { std::lock_guard<std::mutex> lk(m_); return perSec_; }
    uint64_t connects() const { std::lock_guard<std::mutex> lk(m_); return connects_; }

private:
    static double mono() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void logLine(const std::string& l) {
        std::function<void(const std::string&)> f; { std::lock_guard<std::mutex> lk(m_); f = log_; }
        if (f && !l.empty()) f(l);
    }
    int openSock(std::string& err) {
        int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        const bool v6 = fd >= 0;
        if (v6) { int off = 0; setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off); }
        else fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { err = std::strerror(errno); return -1; }
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        int rc;
        if (v6) { sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_addr = loop_ ? in6addr_loopback : in6addr_any; a.sin6_port = htons(uint16_t(port_));
                  rc = ::bind(fd, (sockaddr*)&a, sizeof a); }
        else    { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(loop_ ? INADDR_LOOPBACK : INADDR_ANY); a.sin_port = htons(uint16_t(port_));
                  rc = ::bind(fd, (sockaddr*)&a, sizeof a); }
        if (rc != 0 && v6 && loop_) {   // ★ no IPv6 loopback on this box: plain 127.0.0.1
            ::close(fd); fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { err = std::strerror(errno); return -1; }
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(port_));
            rc = ::bind(fd, (sockaddr*)&a, sizeof a);
        }
        if (rc != 0 || ::listen(fd, 4) != 0) {
            err = errno == EADDRINUSE ? "port " + std::to_string(port_) + " is in use by another program" : std::string(std::strerror(errno));
            ::close(fd); return -1;
        }
        const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC);
        sockaddr_storage ss{}; socklen_t sl = sizeof ss;
        if (::getsockname(fd, (sockaddr*)&ss, &sl) == 0)
            bound_ = ntohs(ss.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
        return fd;
    }
    static std::string peerOf(const sockaddr_storage& ss) {
        char b[INET6_ADDRSTRLEN + 1] = {0};
        if (ss.ss_family == AF_INET6) {
            ::inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr, b, sizeof b);
            std::string s = b; return s.rfind("::ffff:", 0) == 0 ? s.substr(7) : s;
        }
        ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr, b, sizeof b);
        return b;
    }
    void loop() {
        struct C { int fd; int id; std::string peer; };
        std::vector<C> cs;
        int lfd = -1; double retryAt = 0, rateT = mono(); uint64_t rateN = 0;
        std::vector<uint8_t> buf(16 * 1024);
        int nextId = 1;
        std::map<std::string, double> refusedLog;   // ★ one line per address, then one per 10 min (bounded below)
        for (;;) {
            { std::lock_guard<std::mutex> lk(m_); if (!run_) break; }
            const double now = mono();
            if (lfd < 0 && now >= retryAt) {
                std::string e; lfd = openSock(e);
                std::lock_guard<std::mutex> lk(m_);
                err_ = lfd < 0 ? e : std::string();
                if (lfd < 0) retryAt = now + 30;
            }
            if (now - rateT >= 2.0) { std::lock_guard<std::mutex> lk(m_); perSec_ = rateN / (now - rateT); rateN = 0; rateT = now; }
            std::vector<pollfd> pf;
            if (lfd >= 0) pf.push_back({lfd, POLLIN, 0});
            for (auto& c : cs) pf.push_back({c.fd, POLLIN, 0});
            if (pf.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            if (::poll(pf.data(), nfds_t(pf.size()), 200) <= 0) continue;
            size_t k = 0;
            const size_t polled = cs.size();   // ★ clients accepted below were not in this poll
            if (lfd >= 0) {
                if (pf[0].revents & POLLIN) {
                    for (int n = 0; n < 4; ++n) {
                        sockaddr_storage ss{}; socklen_t sl = sizeof ss;
                        const int fd = ::accept(lfd, (sockaddr*)&ss, &sl);
                        if (fd < 0) break;
                        const std::string peer = peerOf(ss);
                        std::function<bool(const std::string&)> al; { std::lock_guard<std::mutex> lk(m_); al = allow_; }
                        const bool ok = al ? al(peer) : loop_ ? (peer == "::1" || peer.rfind("127.", 0) == 0) : beast::peerAllowed(peer, false);
                        if (!ok || (int)cs.size() >= kMaxClients) {
                            ::close(fd);
                            std::string why = !ok ? peer + " is not on this network \xe2\x80\x94 refused" : peer + " refused: " + std::to_string(kMaxClients) + " connected already";
                            { std::lock_guard<std::mutex> lk(m_); ++refused_; lastRefused_ = why; }
                            auto it = refusedLog.find(peer);
                            if (it == refusedLog.end() || now - it->second >= beast::kDownLogEverySec) {
                                if (refusedLog.size() > 256) refusedLog.clear();
                                refusedLog[peer] = now;
                                logLine(why);
                            }
                            continue;
                        }
                        const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC);
                        int one = 1; setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef SO_NOSIGPIPE
                        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#ifdef TCP_KEEPIDLE
                        { int idle = 30, intvl = 10, cnt = 3;
                          setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
                          setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
                          setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt); }
#elif defined(TCP_KEEPALIVE)
                        { int idle = 30; setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle); }
#endif
                        const bool first = cs.empty();
                        cs.push_back({fd, nextId++, peer});
                        double was;
                        { std::lock_guard<std::mutex> lk(m_); peers_.push_back(peer); ++connects_; was = lastClient_ > 0 ? now - lastClient_ : -1; lastClient_ = 0; }
                        if (first && !loop_) logLine("connected from " + peer + (was >= beast::kStallSec ? " (back after " + beast::spanWords(was) + ")" : ""));
                    }
                }
                k = 1;
            }
            for (size_t i = 0; i < polled; ++i, ++k) {
                if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                const ssize_t n = ::recv(cs[i].fd, buf.data(), buf.size(), 0);
                if (n > 0) {
                    Sink s; { std::lock_guard<std::mutex> lk(m_); s = sink_; lastData_ = mono(); }
                    const size_t m = s ? s(cs[i].id, buf.data(), size_t(n)) : 0;
                    if (m) { std::lock_guard<std::mutex> lk(m_); msgs_ += m; }
                    rateN += m;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                ::close(cs[i].fd); cs[i].fd = -1;
            }
            const size_t before = cs.size();
            cs.erase(std::remove_if(cs.begin(), cs.end(), [](const C& c) { return c.fd < 0; }), cs.end());
            if (cs.size() != before) {
                std::lock_guard<std::mutex> lk(m_);
                peers_.clear(); for (auto& c : cs) peers_.push_back(c.peer);
                if (cs.empty()) lastClient_ = mono();
            }
            if (cs.size() != before && cs.empty() && !loop_) logLine("the sender disconnected \xe2\x80\x94 waiting for it to connect again");
        }
        for (auto& c : cs) ::close(c.fd);
        if (lfd >= 0) ::close(lfd);
        std::lock_guard<std::mutex> lk(m_);
        peers_.clear(); bound_ = 0;
    }
    mutable std::mutex m_;
    int port_ = 0, bound_ = 0; bool loop_ = false, run_ = false;
    Sink sink_; std::string name_, err_, lastRefused_;
    std::function<void(const std::string&)> log_;
    std::function<bool(const std::string&)> allow_;
    std::thread th_;
    std::vector<std::string> peers_;
    double startMono_ = 0, lastClient_ = 0, lastData_ = 0, perSec_ = 0;
    uint64_t refused_ = 0, msgs_ = 0, connects_ = 0;
};

// ══ ONE SOURCE ═══════════════════════════════════════════════════════════════════════════════════════════════════════
class Source {
public:
    /** `hub`: where believed Beast replies go (the server-level flight-tracker hub), or nullptr. */
    Source(const SourceCfg& c, beast::Hub* hub, std::function<void(const std::string&)> log) : c_(c), hub_(hub), log_(std::move(log)), tbl_(c.label) {}
    ~Source() { stop(); }
    const SourceCfg& cfg() const { return c_; }
    void start(double rxLat = NAN, double rxLon = NAN) {
        tr_ = vibeadsb::Tracker();
        tr_.setMaxRangeNm(360);
        if (rxLat == rxLat && rxLon == rxLon) tr_.setReceiver(rxLat, rxLon);
        t0_ = std::chrono::steady_clock::now();
        startWall_ = vibeavshare::wallMs();
        auto pre = [this](const std::string& l) { if (log_) log_("[aircraft feeds] " + title(c_) + ": " + l); };
        const bool adsb = c_.label == "adsb";
        if (c_.dir == "connect") {
            in_.setLog([this](const std::string& l) { if (log_) log_("[aircraft feeds] " + title(c_) + " \xe2\x80\x94 " + l); });
            if (c_.fmt == "beast")
                in_.start(c_.host, c_.port, [this](const std::vector<beast::Decoded>& ds) { onBeast(ds); });
            else
                in_.startRaw(c_.host, c_.port, [this](const uint8_t* p, size_t n) { return onSbs(0, p, n); }, "BaseStation feed",
                             "is the sender's BaseStation (SBS) output on? (dumphfdl connects OUT \xe2\x80\x94 choose \"listen\" for it)",
                             adsb ? beast::kStallSec : 0);
        } else {
            ln_.setLog(pre);
            ln_.start(c_.port, c_.internal, [this](int id, const uint8_t* p, size_t n) {
                return c_.fmt == "beast" ? onBeastBytes(id, p, n) : onSbs(id, p, n);
            }, title(c_));
        }
    }
    void stop() { in_.stop(); ln_.stop(); }
    int boundPort() const { return ln_.boundPort(); }
    Listener& listener() { return ln_; }

    /** ★ The shared table's provider: rows heard since `sinceMs`, tagged with this source's label. */
    void shareSince(int64_t sinceMs, std::vector<vibeavshare::Update>& out) {
        std::lock_guard<std::mutex> lk(m_);
        const int64_t nowMs = vibeavshare::wallMs();
        if (c_.fmt == "sbs") { tbl_.shareSince(sinceMs, nowMs, out, std::string(sbs::labelWords(c_.label)) + " feed" + (c_.name.empty() ? "" : " (" + c_.name + ")")); return; }
        const double tn = now();
        auto wall = [&](double t) { return nowMs - (int64_t)std::llround((tn - t) * 1000); };
        for (const auto& kv : tr_.aircraft()) {
            const vibeadsb::Aircraft& a = kv.second;
            if (a.nonIcao) continue;
            const int64_t heard = wall(a.seen);
            if (heard < sinceMs) continue;
            vibeavshare::Update u;
            u.icao = a.icao; u.flight = a.callsign;
            u.srcMask = 1u << vibeavshare::kAdsb; u.srcMs[vibeavshare::kAdsb] = heard; u.heardMs = heard;
            if (a.hasPos && tn - a.seenPos <= 60) {
                u.hasPos = true; u.lat = a.lat; u.lon = a.lon; u.posMs = wall(a.seenPos); u.posSrc = vibeavshare::kAdsb;
                u.hasAlt = a.hasAlt; u.altFt = a.altBaro; u.hasTrk = a.hasTrack; u.trk = (float)a.track; u.hasGs = a.hasGs; u.gs = (float)a.gs;
                u.ground = a.onGround;
            } else if (a.hasAlt) { u.hasAlt = true; u.altFt = a.altBaro; }
            const uint32_t c = a.messages; auto& sent = shareSent_[a.icao];
            u.count = c >= sent ? c - sent : c; sent = c;
            out.push_back(std::move(u));
            if (out.size() >= vibeavshare::kCap) break;
        }
        if (shareSent_.size() > 4 * vibeavshare::kCap) shareSent_.clear();
    }
    /** Age out rows (once a second from the set's housekeeping). */
    void housekeep() {
        std::lock_guard<std::mutex> lk(m_);
        tr_.expire(now(), 60.0);
        tbl_.expire(vibeavshare::wallMs());
    }
    size_t aircraft() {
        std::lock_guard<std::mutex> lk(m_);
        return c_.fmt == "sbs" ? tbl_.heardWithin(vibeavshare::wallMs(), tbl_.posKeepMs()) : tr_.aircraft().size();
    }
    /** ★ Is the source DOWN (see the header)? `secs` = for how long. */
    bool down(double& secs) const {
        const bool adsb = c_.label == "adsb";
        if (c_.dir == "connect") {
            const long ds = in_.downSinceUtc();
            if (ds > 0) { secs = double(std::time(nullptr) - ds); return true; }
            if (adsb && in_.connected() && in_.secsSinceData() > stallSec()) { secs = in_.secsSinceData(); return true; }
            return false;
        }
        const double noClient = ln_.secsWithoutClient();
        if (noClient > stallSec()) { secs = noClient; return true; }
        if (adsb && ln_.clients() && ln_.secsSinceData() > stallSec()) { secs = ln_.secsSinceData(); return true; }
        return false;
    }
    /** The hosts this source's data comes from (the hub's loop guards): its upstream, or who connected to it. */
    std::vector<std::string> inputHosts() const {
        if (c_.internal) return {};
        if (c_.dir == "connect") return {c_.host};
        return ln_.peers();
    }
    /** {"title","fmt","dir","label","state","line","aircraft","perSec","msgs",…} — `withAddr`: the owner's view. */
    std::string statusJson(bool withAddr) {
        const size_t ac = aircraft();
        double ds = 0; const bool dn = down(ds);
        std::string st, line, err;
        double perSec = 0; uint64_t msgs = 0;
        const std::string where = c_.dir == "connect" ? beast::hostPortText(c_.host, c_.port) : "port " + std::to_string(c_.port);
        if (c_.dir == "connect") {
            vibe::antswitch::Json j;
            const std::string s = in_.statusJson();
            if (vibe::antswitch::parseJson(s, j)) {
                st = j.str("state"); line = j.str("line"); err = j.str("error");
                if (const auto* v = j.get("perSec")) perSec = v->n;
                if (const auto* v = j.get("msgs")) msgs = uint64_t(v->n);
            }
        } else {
            perSec = ln_.perSec(); msgs = ln_.msgs(); err = ln_.error();
            const auto ps = ln_.peers();
            if (!err.empty()) { st = "error"; line = "cannot listen on " + where + ": " + err; }
            else if (ln_.boundPort() <= 0) { st = "opening"; line = "opening " + where; }
            else if (ps.empty()) { st = "listening"; line = "listening on " + where + " \xe2\x80\x94 nothing connected" + (ln_.connects() ? " (the sender left " + beast::spanWords(ln_.secsWithoutClient()) + " ago)" : " yet"); }
            else {
                st = c_.label == "adsb" && ln_.secsSinceData() > beast::kQuietSec ? "stalled" : "connected";
                char b[64]; std::snprintf(b, sizeof b, ", %.1f messages a second", perSec);
                line = "connected from " + ps.front() + (ps.size() > 1 ? " (+" + std::to_string(ps.size() - 1) + ")" : "") + b;
            }
            if (ln_.refused()) line += " \xc2\xb7 " + std::to_string(ln_.refused()) + " refused (last: " + ln_.lastRefused() + ")";
        }
        if (dn) st = "down";
        line += " \xc2\xb7 " + std::to_string(ac) + " aircraft";
        std::string o = "{\"title\":\"" + beast::jsonEsc(title(c_)) + "\",\"fmt\":\"" + c_.fmt + "\",\"dir\":\"" + c_.dir + "\",\"label\":\""
                      + sbs::labelWords(c_.label) + "\",\"state\":\"" + st + "\",\"down\":" + (dn ? std::to_string(long(ds)) : std::string("0"))
                      + ",\"aircraft\":" + std::to_string(ac);
        char b[96]; std::snprintf(b, sizeof b, ",\"perSec\":%.1f,\"msgs\":%llu", perSec, (unsigned long long)msgs); o += b;
        if (withAddr) o += ",\"where\":\"" + beast::jsonEsc(where) + "\",\"line\":\"" + beast::jsonEsc(line) + "\",\"error\":\"" + beast::jsonEsc(err) + "\"";
        return o + "}";
    }

private:
    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }
    void onBeast(const std::vector<beast::Decoded>& ds) {
        const double t = now();
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& d : ds) {
            if (d.type == '1') continue;   // Mode A/C: not ADS-B
            const int len = int(d.msg.size());
            if (len != 7 && len != 14) continue;
            vibeadsb::Frame f;
            std::memcpy(f.b, d.msg.data(), size_t(len)); f.len = len;
            f.sample = d.ts * 2 / 3;
            f.level = d.sig ? float(20.0 * std::log10(d.sig / 255.0)) : -60.0f;
            if (!tr_.accept(f, t)) continue;
            if (hub_) {   // ★★ byte-faithful: the upstream's own bytes, timestamp and signal byte
                const uint8_t* m = d.msg.data();
                const int df = m[0] >> 3;
                const uint32_t addr = (df == 11 || df == 17 || df == 18) ? (uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3]) : vibeadsb::syndrome(m, len);
                hub_->frameRaw(m, len, d.ts, d.sig, addr, t, c_.internal, uintptr_t(this));
            }
        }
    }
    size_t onBeastBytes(int id, const uint8_t* p, size_t n) {
        std::vector<beast::Decoded> out;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (id != beastId_) { reader_ = beast::Reader(); beastId_ = id; }   // ★ a new connection starts a clean frame
            reader_.feed(p, n, out);
        }
        onBeast(out);
        return out.size();
    }
    size_t onSbs(int id, const uint8_t* p, size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        if (id != sbsId_) { split_.reset(); sbsId_ = id; }
        size_t k = 0;
        const int64_t nowMs = vibeavshare::wallMs();
        split_.feed(p, n, [&](const std::string& line) {
            ++st_.lines;
            sbs::Msg m; bool other = false;
            if (sbs::parseLine(line, m, &other)) { if (tbl_.apply(m, nowMs)) ++st_.msgs; ++k; }
            else if (other) ++st_.skipped; else ++st_.bad;
        });
        return k;
    }
    SourceCfg c_;
    beast::Hub* hub_;
    std::function<void(const std::string&)> log_;
    mutable std::mutex m_;
    vibeadsb::Tracker tr_;
    sbs::Table tbl_;
    sbs::LineSplitter split_;
    sbs::Stats st_;
    beast::Reader reader_;
    int beastId_ = -1, sbsId_ = -1;
    std::unordered_map<uint32_t, uint32_t> shareSent_;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
    int64_t startWall_ = 0;
    beast::BeastInput in_;
    Listener ln_;
};

// ══ THE SET: every source of this machine, the shared-table provider, the banner, the status file ════════════════════
class FeedSet {
public:
    static FeedSet& shared() { static FeedSet* f = new FeedSet(); return *f; }   // ★ never destroyed (threads may still call)
    ~FeedSet() { stop(); }
    void setLog(std::function<void(const std::string&)> f) { std::lock_guard<std::mutex> lk(m_); log_ = std::move(f); }
    void setHub(beast::Hub* h) { std::lock_guard<std::mutex> lk(m_); hub_ = h; }
    void setReceiver(double lat, double lon) { std::lock_guard<std::mutex> lk(m_); rxLat_ = lat; rxLon_ = lon; }
    /** The runtime directory (the shared table's): the status file other processes read, and the ingest port file. */
    void setDir(const std::string& d) { std::lock_guard<std::mutex> lk(m_); dir_ = d; }
    /** ★ Full layout: the radios' own ADS-B comes in on a loopback port (their hub pushes there) — the server-level
     *  destinations get every radio's decodes plus the feeds. */
    void setIngest(bool on) { std::lock_guard<std::mutex> lk(m_); ingest_ = on; }
    /** A test's address rule for every listen source (nullptr = the LAN rule). */
    void setAllow(std::function<bool(const std::string&)> f) { std::lock_guard<std::mutex> lk(m_); allow_ = std::move(f); key_ = "\x01"; }

    /** Apply the owner's list (restarts the sources only when it changed). */
    bool configure(const std::vector<SourceCfg>& list) {
        std::vector<SourceCfg> want = list;
        bool ingest;
        { std::lock_guard<std::mutex> lk(m_); ingest = ingest_; }
        if (ingest) { SourceCfg in; in.fmt = "beast"; in.dir = "listen"; in.port = 0; in.label = "adsb"; in.internal = true; in.name = "this server's radios"; want.push_back(in); }
        std::string k; for (const auto& c : want) k += key(c) + ";";
        bool keyStale;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (k == key_) return true;
            keyStale = key_ == "\x01";   // ★ setAllow (a test) changed every listener's rule: start them all again
            key_ = k;
        }
        // ★ a source whose settings did not change keeps running (its connection, its aircraft) — only the changed go
        std::vector<std::unique_ptr<Source>> old;
        { std::lock_guard<std::mutex> lk(m_); old = std::move(src_); }
        std::vector<std::unique_ptr<Source>> made;
        beast::Hub* hub; std::function<void(const std::string&)> log; double la, lo;
        { std::lock_guard<std::mutex> lk(m_); hub = hub_; log = log_; la = rxLat_; lo = rxLon_; }
        std::function<bool(const std::string&)> allow; { std::lock_guard<std::mutex> lk(m_); allow = allow_; }
        const bool keep = !keyStale;
        std::vector<std::unique_ptr<Source>> slot(want.size());
        for (size_t i = 0; i < want.size(); ++i)
            if (keep) for (auto& o : old) if (o && key(o->cfg()) == key(want[i])) { slot[i] = std::move(o); break; }
        for (auto& o : old) if (o) o->stop();   // ★ the ones going close first: a listener's port is free for its successor
        old.clear();
        for (size_t i = 0; i < want.size(); ++i) {
            if (!slot[i]) {
                slot[i].reset(new Source(want[i], hub, log));
                if (allow && !want[i].internal) slot[i]->listener().setAllow(allow);
                slot[i]->start(la, lo);
            }
            made.push_back(std::move(slot[i]));
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            src_ = std::move(made);
        }
        if (hub) hub->setInputHosts(inputHosts());   // ★ the loop guards before any destination can connect
        if (!want.empty()) {
            vibeavshare::sharer().provide("feeds", [this](int64_t since, std::vector<vibeavshare::Update>& out, std::vector<vibeavshare::GsHeard>&) {
                std::lock_guard<std::mutex> lk(m_);
                for (auto& s : src_) { std::vector<vibeavshare::Update> u; s->shareSince(since, u); out.insert(out.end(), u.begin(), u.end()); if (out.size() >= vibeavshare::kCap) break; }
            }, kFeedRadioId);   // ★★ beta96 written as the radio "feed" (b96-mapchat's per-client OTHER RADIOS switch hides them)
            vibeavshare::sharer().adsbLiveExtra("feeds", [this] { return adsbLive(); });
            startHousekeeping();
        } else {
            vibeavshare::sharer().remove("feeds");
            vibeavshare::sharer().adsbLiveExtra("feeds", nullptr);
            stop();
            writeStatusFile();   // ★ an empty list says so to the other processes at once
        }
        return true;
    }
    size_t size() { std::lock_guard<std::mutex> lk(m_); size_t n = 0; for (auto& s : src_) if (!s->cfg().internal) ++n; return n; }
    /** The loopback ingest port (0 = none yet). */
    int ingestPort() { std::lock_guard<std::mutex> lk(m_); for (auto& s : src_) if (s->cfg().internal) return s->boundPort(); return 0; }

    /** ★ An ADS-B source is live (stamps the shared table's "ADS-B decoding now", so its rows count as heard now). */
    bool adsbLive() {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& s : src_) { double d; if (s->cfg().label == "adsb" && !s->cfg().internal && !s->down(d) && s->aircraft() > 0) return true; }
        return false;
    }
    /** The owner's status: every source with its address and line. */
    std::string statusJson() { return json(true); }
    /** For anyone (aircraft.json): labels, states, counts — no addresses. */
    std::string publicJson() { return json(false); }
    /** ★ The AIR & SEA map's banner: "" when every source is live. "ADS-B source lost — reconnecting… (2 min) · HFDL still live". */
    std::string banner() {
        std::lock_guard<std::mutex> lk(m_);
        return bannerOf(src_);
    }
    template <class V> static std::string bannerOf(const V& src) {
        std::string lost, live;
        for (auto& s : src) {
            if (s->cfg().internal) continue;
            double d = 0;
            if (s->down(d)) lost += (lost.empty() ? "" : " \xc2\xb7 ") + title(s->cfg()) + " lost \xe2\x80\x94 reconnecting\xe2\x80\xa6 (" + beast::spanWords(d) + ")";
            else live += (live.empty() ? "" : ", ") + std::string(title(s->cfg()));
        }
        if (lost.empty()) return "";
        return live.empty() ? lost : lost + " \xc2\xb7 " + live + " still live";
    }
    void stop() {
        std::thread t;
        { std::lock_guard<std::mutex> lk(m_); run_ = false; t = std::move(hk_); }
        if (t.joinable()) t.join();
    }
    /** The hub's loop guards: every host a source reads from. */
    std::vector<std::string> inputHosts() {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::string> v;
        for (auto& s : src_) for (auto& h : s->inputHosts()) v.push_back(h);
        return v;
    }
    void writeStatusFile() {
        std::string dir; { std::lock_guard<std::mutex> lk(m_); dir = dir_; }
        if (dir.empty()) return;
        const std::string body = "{\"at\":" + std::to_string((long long)(vibeavshare::wallMs() / 1000)) + ",\"feeds\":" + publicJson()
                               + ",\"warn\":\"" + beast::jsonEsc(banner()) + "\"}";
        const std::string p = dir + "/" + kStatusFile, tmp = p + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "w")) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); ::rename(tmp.c_str(), p.c_str()); }
        const int ip = ingestPort();
        const std::string pp = dir + "/" + kIngestPortFile;
        if (ip > 0) { if (FILE* f = std::fopen((pp + ".tmp").c_str(), "w")) { std::fprintf(f, "%d\n", ip); std::fclose(f); ::rename((pp + ".tmp").c_str(), pp.c_str()); } }
        else ::unlink(pp.c_str());
    }

private:
    std::string json(bool owner) {
        std::lock_guard<std::mutex> lk(m_);
        std::string o = "[";
        for (auto& s : src_) { if (s->cfg().internal) continue; if (o.size() > 1) o += ','; o += s->statusJson(owner); }
        return o + "]";
    }
    void startHousekeeping() {
        std::lock_guard<std::mutex> lk(m_);
        if (run_) return;
        run_ = true;
        hk_ = std::thread([this] {
            double lastFile = 0;
            for (;;) {
                { std::lock_guard<std::mutex> lk2(m_); if (!run_) break; for (auto& s : src_) s->housekeep(); }
                beast::Hub* hub; { std::lock_guard<std::mutex> lk2(m_); hub = hub_; }
                if (hub) hub->setInputHosts(inputHosts());
                const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (t - lastFile >= kStatusEverySec) { lastFile = t; writeStatusFile(); }
                for (int i = 0; i < 10; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    std::lock_guard<std::mutex> lk2(m_); if (!run_) break;
                }
            }
        });
    }
    std::mutex m_;
    std::vector<std::unique_ptr<Source>> src_;
    std::string key_, dir_;   // ★ key_ "" = configured empty (the start)
    beast::Hub* hub_ = nullptr;
    std::function<void(const std::string&)> log_;
    double rxLat_ = NAN, rxLon_ = NAN;
    std::function<bool(const std::string&)> allow_;
    bool ingest_ = false, run_ = false;
    std::thread hk_;
};

/** ★ A radio process behind the door (or any reader): the owner's feed status as the owning process last wrote it —
 *  {"feeds":[…],"warn":"…"} — or "" when there is none or it is stale (the owner stopped). */
inline std::string readStatusFile(const std::string& dir, int64_t nowS) {
    if (dir.empty()) return "";
    FILE* f = std::fopen((dir + "/" + kStatusFile).c_str(), "r");
    if (!f) return "";
    char b[16384]; const size_t n = std::fread(b, 1, sizeof b - 1, f); std::fclose(f);
    std::string s(b, n);
    vibe::antswitch::Json j;
    if (!vibe::antswitch::parseJson(s, j) || j.kind != vibe::antswitch::Json::Obj) return "";
    const auto* at = j.get("at");
    if (!at || at->kind != vibe::antswitch::Json::Num || nowS - int64_t(at->n) > 15) return "";
    const auto* fs = j.get("feeds");
    if (!fs || fs->kind != vibe::antswitch::Json::Arr || fs->a.empty()) return "";
    return s;
}
/** The loopback ingest port the owner wrote (0 = none). */
inline int readIngestPort(const std::string& dir) {
    if (dir.empty()) return 0;
    FILE* f = std::fopen((dir + "/" + kIngestPortFile).c_str(), "r");
    if (!f) return 0;
    int p = 0; if (std::fscanf(f, "%d", &p) != 1) p = 0; std::fclose(f);
    return p > 0 && p < 65536 ? p : 0;
}

}  // namespace feeds
}  // namespace vibe
