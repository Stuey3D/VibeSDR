// vibe_beast.h — V12 beta96: SHARE ADS-B WITH FLIGHT TRACKERS (docs/v12/ADSB-FEED.md). Stuart 2026-11-26: built "like we
// did for PSKReporter" — an owner switch per radio, OFF by default.
//
// Three pieces, header-only like vibe_spot_report.h (the daemon, the Android app's server and the tests compile it as is):
//   1. THE BEAST ENCODING of the Mode S replies our own decoder (vibe_adsb.h) believed — and a small reader for it, which
//      the tests use to prove a round trip.
//   2. A LOCAL BEAST TCP SERVER (default port 30005, the usual one) that a feeder program on this machine or the LAN reads
//      — FlightAware's PiAware with receiver-type "other" (installed by the owner from FlightAware, never bundled by us).
//   3. PUSH FEEDS: our Beast stream sent out to one or more aggregators' published feed ports (or any host:port).
//
// ★★★ THE FORMAT, OWN CODE. The Mode S "Beast" binary framing is the one the Mode-S Beast receiver introduced and every
//     ADS-B feeder program reads: 0x1A, a type byte ('1' Mode A/C, 2 bytes; '2' Mode S short, 7; '3' Mode S long, 14), a
//     6-byte big-endian timestamp in 12 MHz ticks, one signal byte, the message — and every 0x1A inside the timestamp,
//     signal or message sent twice. Written from that description; nothing copied.
// ★★★ WHAT IS SENT: only replies our decoder accepted (vibe_adsb.h Tracker::accept: DF17/18 with a CRC remainder of 0 or
//     one repaired bit, DF11 all-calls, and the address/parity replies DF0/4/5/16/20/21 of an aircraft already heard), plus
//     CRC-clean DF18 TIS-B / ADS-R relays (the Tracker leaves those off the map because they are not the aircraft's own,
//     but aggregators read them). The bytes are the corrected ones. Mode A/C is not demodulated, so no type '1' is sent.
//     What aircraft broadcast and nothing else: no listener, no position of this receiver, no names.
// ★★ THE TIMESTAMP, HONESTLY. Our demodulator places a reply on a 125 ns grid counted in samples from the start of the
//    ADS-B run (Frame::sample). We send that as the 12 MHz counter (× 1.5). It is a SAMPLE CLOCK, not GPS time, and it
//    jumps when the decoder's queue drops a block on a slow box, and restarts with each ADS-B run. MLAT (multilateration)
//    needs a clean, continuous counter with sub-sample precision; ours has not been verified for it, so the docs and the
//    Setup card say MLAT is NOT supported. The counter is filled so feeder programs that expect one see one.
// ★★★ THREADS (memory feedback_thread_priority_order: Network > Audio > Spectrum > Decoders). frame() is called on the
//     ADS-B decoder's own thread: it appends to a bounded buffer under a short lock and returns. Every socket lives on
//     this file's OWN network thread — never the audio or DSP threads. A link that stalls loses its OLDEST data
//     (counted), never makes anything wait.
// ★★★ BOUNDED (memory feedback_memory_floor_256mb): at most 8 local readers and 8 push feeds, 256 KB queued per link
//     (~10 s of a busy sky), 64 KB between the decoder and the network thread. Worst case ~4.2 MB.
// ★★ LAN ONLY BY DEFAULT: the local server accepts loopback and the local network (vibe_ipclass.h — the rules RAW IQ OUT's
//    "Local only" uses) unless the owner opens it to any address.
// ★★ PUSH FEEDS ARE "REDUCED", LOCAL READERS GET EVERYTHING. The community aggregators' own feed scripts send a reduced
//    Beast stream (readsb's "beast_reduce_plus_out": ordinary Beast frames, the same data not repeated within 0.5 s) and
//    identify the station by a UUID sent once after connecting (0x1A 0xE4 + the UUID as 36 ASCII characters). We do the
//    same from those published facts, in our own code: per aircraft, per kind of message (DF, the extended squitter's type
//    code, and a position's odd/even bit, so a CPR pair is never split), at most one every 0.5 s. A UUID is sent only
//    where the owner pasted one for that destination.
// ★ Reconnects back off: 5 s doubling to 5 min (±20 % jitter, so a fleet of servers never knocks in step), back to 5 s
//   after a link has stayed up for a minute. Push links connect only while ADS-B is running here (and for 2 minutes after,
//   to ride over a quick exit and re-entry), so an aggregator never sees a silent feeder from us.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vibe_antswitch.h"   // ★ its small strict JSON reader (Json / parseJson) — as vibe_dxscan.h uses it
#include "vibe_ipclass.h"     // ★ LAN-only: the same address rules as RAW IQ OUT's "Local only"

namespace vibe {
namespace beast {

// ══ 1. THE ENCODING ════════════════════════════════════════════════════════════════════════════════════════════════
constexpr uint8_t kEsc = 0x1A;
constexpr uint64_t kTsMask = 0xFFFFFFFFFFFFull;   ///< 48 bits

/** vibe_adsb.h Frame::sample (125 ns units) → the 12 MHz counter: 1 unit = 1.5 ticks. Wraps at 48 bits. */
inline uint64_t ts12FromFine(uint64_t fine) { return (fine * 3 / 2) & kTsMask; }

/** The signal byte: the reply's amplitude relative to full scale, 0–255 (255 = full scale). Our level is a POWER in dBFS
 *  (|IQ|² of 1.0 = 0 dBFS), so the amplitude is 10^(dB/20). */
inline uint8_t signalByte(float dbfs) {
    if (!(dbfs == dbfs)) return 0;
    const double a = std::pow(10.0, double(dbfs) / 20.0) * 255.0;
    return uint8_t(std::max(0.0, std::min(255.0, std::round(a))));
}

/** The type byte for a message length, or 0 for a length Beast does not carry. */
inline uint8_t typeFor(int len) { return len == 2 ? '1' : len == 7 ? '2' : len == 14 ? '3' : 0; }
inline int lenFor(uint8_t type) { return type == '1' ? 2 : type == '2' ? 7 : type == '3' ? 14 : 0; }

/** Append one Beast frame. Returns false (and appends nothing) for a length Beast does not carry. */
inline bool appendFrame(std::vector<uint8_t>& o, const uint8_t* msg, int len, uint64_t ts12, uint8_t sig) {
    const uint8_t t = typeFor(len);
    if (!t || !msg) return false;
    o.push_back(kEsc); o.push_back(t);
    auto put = [&](uint8_t b) { o.push_back(b); if (b == kEsc) o.push_back(kEsc); };
    for (int i = 5; i >= 0; --i) put(uint8_t((ts12 & kTsMask) >> (8 * i)));
    put(sig);
    for (int i = 0; i < len; ++i) put(msg[i]);
    return true;
}

/** A Beast stream reader: the tests' independent check of the encoder, and nothing else in the server reads Beast. */
struct Decoded { uint8_t type = 0; uint64_t ts = 0; uint8_t sig = 0; std::vector<uint8_t> msg; };
class Reader {
public:
    /** Feed bytes; complete frames are appended to `out`. A broken frame (an unescaped 0x1A inside it) is counted and
     *  reading starts again at that 0x1A. */
    void feed(const uint8_t* p, size_t n, std::vector<Decoded>& out) {
        for (size_t i = 0; i < n; ++i) step(p[i], out);
    }
    uint64_t bad() const { return bad_; }
private:
    enum St { Hunt, Type, Body } st_ = Hunt;
    bool esc_ = false;
    int need_ = 0;
    std::vector<uint8_t> body_;
    uint8_t type_ = 0;
    uint64_t bad_ = 0;
    void begin() { st_ = Type; esc_ = false; body_.clear(); }
    void step(uint8_t b, std::vector<Decoded>& out) {
        switch (st_) {
        case Hunt: if (b == kEsc) begin(); return;
        case Type:
            if (lenFor(b)) { type_ = b; need_ = 7 + lenFor(b); st_ = Body; esc_ = false; return; }
            // 0x1A 0x1A outside a frame is an escaped data byte, not a start; anything else is not ours
            st_ = Hunt; if (b != kEsc) ++bad_; return;
        case Body:
            if (esc_) {
                esc_ = false;
                if (b == kEsc) { push(b, out); return; }
                // an unescaped 0x1A was a new frame's start: the one in hand is broken
                ++bad_;
                if (lenFor(b)) { body_.clear(); type_ = b; need_ = 7 + lenFor(b); st_ = Body; return; }
                st_ = Hunt; return;
            }
            if (b == kEsc) { esc_ = true; return; }
            push(b, out);
            return;
        }
    }
    void push(uint8_t b, std::vector<Decoded>& out) {
        body_.push_back(b);
        if ((int)body_.size() < need_) return;
        Decoded d; d.type = type_;
        for (int i = 0; i < 6; ++i) d.ts = (d.ts << 8) | body_[size_t(i)];
        d.sig = body_[6];
        d.msg.assign(body_.begin() + 7, body_.end());
        out.push_back(std::move(d));
        st_ = Hunt;
    }
};

// ══ THE OWNER'S SETTINGS (RadioConfig::adsbFeed, one JSON blob like dxScan) ═════════════════════════════════════════
constexpr int kMaxFeeds = 8;
constexpr int kMaxReaders = 8;
constexpr int kDefaultPort = 30005;

struct Dest { std::string host; int port = 0; bool on = true; std::string uuid; };

/** A station UUID as the aggregators issue them (8-4-4-4-12 hex), lower-cased; "" if it is not one. */
inline std::string cleanUuid(const std::string& u) {
    if (u.size() != 36) return "";
    std::string o;
    for (size_t i = 0; i < 36; ++i) {
        const char c = u[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return ""; o += c; continue; }
        if (!std::isxdigit((unsigned char)c)) return "";
        o += char(std::tolower((unsigned char)c));
    }
    return o;
}
/** The UUID announcement sent once, first thing after a push link connects: 0x1A 0xE4 then the 36 characters. */
inline std::vector<uint8_t> uuidFrame(const std::string& uuid) {
    std::vector<uint8_t> o;
    if (uuid.size() != 36) return o;
    o.push_back(kEsc); o.push_back(0xE4);
    for (char c : uuid) o.push_back(uint8_t(c));   // ASCII: never 0x1A
    return o;
}

/** The reduce key of a believed reply: its aircraft, its DF, and for an extended squitter the type code and (for a
 *  position) the CPR odd/even bit. `addr` = the aircraft's address (the Tracker's). */
inline uint64_t reduceKey(const uint8_t* m, int len, uint32_t addr) {
    const int df = m[0] >> 3;
    uint64_t k = (uint64_t(addr & 0xFFFFFF) << 16) | (uint64_t(df) << 8);
    if ((df == 17 || df == 18) && len == 14) {
        const int tc = m[4] >> 3;
        k |= uint64_t(tc) << 1;
        if ((tc >= 5 && tc <= 18) || (tc >= 20 && tc <= 22)) k |= (m[6] >> 2) & 1;   // ★ the F bit: odd and even both go
        if (tc == 28 || tc == 29 || tc == 31) k |= uint64_t(m[4] & 7) << 40;          // sub-types of status/target/op-status
    }
    return k;
}
struct Config {
    bool serve = false;           ///< the local Beast server
    int  port = kDefaultPort;     ///< its port
    bool open = false;            ///< false = loopback + LAN only (the default); true = any address
    bool push = false;            ///< send to the destinations below
    std::vector<Dest> feeds;
    bool any() const { return serve || (push && std::any_of(feeds.begin(), feeds.end(), [](const Dest& d) { return d.on; })); }
};

/** A host the owner typed: a name or an address. Letters, digits, '.', '-', ':' (IPv6), at most 253. */
inline bool hostOk(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    for (char c : h) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':')) return false;
    return h.front() != '-' && h.front() != '.';
}

/** Read the blob. "" = everything off (every radio before this existed). A wrong entry is refused in words, never
 *  half-applied — the Setup page's save shows the reason. */
inline bool parseConfig(const std::string& text, Config& out, std::string& err) {
    out = Config{};
    if (text.empty()) return true;
    vibe::antswitch::Json j;
    if (!vibe::antswitch::parseJson(text, j) || j.kind != vibe::antswitch::Json::Obj) {
        err = "the flight-tracker sharing settings are not a JSON object"; return false;
    }
    using J = vibe::antswitch::Json;
    auto bo = [&](const J& o, const char* k, bool d) { const J* v = o.get(k); return v && v->kind == J::Bool ? v->b : d; };
    auto num = [&](const J& o, const char* k, double d) { const J* v = o.get(k); return v && v->kind == J::Num ? v->n : d; };
    out.serve = bo(j, "serve", false);
    out.open = bo(j, "open", false);
    out.push = bo(j, "push", false);
    const double p = num(j, "port", kDefaultPort);
    if (p < 1024 || p > 65535 || p != std::floor(p)) { err = "the Beast port must be a whole number from 1024 to 65535"; return false; }
    out.port = int(p);
    if (const J* f = j.get("feeds")) {
        if (f->kind != J::Arr) { err = "the flight-tracker list is not a list"; return false; }
        if ((int)f->a.size() > kMaxFeeds) { err = "at most " + std::to_string(kMaxFeeds) + " flight trackers"; return false; }
        for (const J& e : f->a) {
            if (e.kind != J::Obj) { err = "a flight-tracker entry is not an object"; return false; }
            Dest d;
            d.host = e.str("host");
            const double dp = num(e, "port", 0);
            d.on = bo(e, "on", true);
            const std::string u = e.str("uuid");
            if (!u.empty()) { d.uuid = cleanUuid(u); if (d.uuid.empty()) { err = "the station UUID for " + e.str("host").substr(0, 60) + " is not a UUID (8-4-4-4-12 hex)"; return false; } }
            if (!hostOk(d.host)) { err = "\"" + d.host.substr(0, 60) + "\" is not a host name or address"; return false; }
            if (dp < 1 || dp > 65535 || dp != std::floor(dp)) { err = "the port for " + d.host + " must be 1 to 65535"; return false; }
            d.port = int(dp);
            out.feeds.push_back(d);
        }
    }
    return true;
}

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

inline std::string toJson(const Config& c) {
    std::string o = std::string("{\"serve\":") + (c.serve ? "true" : "false") + ",\"port\":" + std::to_string(c.port)
                  + ",\"open\":" + (c.open ? "true" : "false") + ",\"push\":" + (c.push ? "true" : "false") + ",\"feeds\":[";
    for (size_t i = 0; i < c.feeds.size(); ++i) {
        if (i) o += ',';
        o += "{\"host\":\"" + jsonEsc(c.feeds[i].host) + "\",\"port\":" + std::to_string(c.feeds[i].port)
           + ",\"on\":" + (c.feeds[i].on ? "true" : "false")
           + (c.feeds[i].uuid.empty() ? std::string() : ",\"uuid\":\"" + c.feeds[i].uuid + "\"") + "}";
    }
    return o + "]}";
}

/** May a reader at `peer` use the local server? Loopback always; the LAN unless… ; anyone only when the owner opened it. */
inline bool peerAllowed(const std::string& peer, bool open) {
    if (open) return true;
    std::string p = peer.rfind("::ffff:", 0) == 0 ? peer.substr(7) : peer;
    if (p == "::1" || p.rfind("127.", 0) == 0) return true;
    if (vibeip::isPrivateV4(p)) return true;
    if (vibeip::isLinkLocalOrUla(p)) return true;
    if (p.find(':') != std::string::npos && vibeip::onLinkV6(p)) return true;
    return false;
}

// ══ 2 + 3. THE HUB: the local server and the push feeds, on one network thread ══════════════════════════════════════
struct Timing {
    double backoffMin = 5, backoffMax = 300;   ///< seconds
    double stableAfter = 60;                   ///< up this long → the next failure starts again at backoffMin
    double connectTimeout = 10;
    double activeGrace = 120;                  ///< push links stay up this long after ADS-B stops
    double listenRetry = 30;                   ///< a port that would not bind is tried again after this
    size_t linkCap = 256 * 1024;               ///< bytes queued per link before the oldest goes
    size_t pendingCap = 64 * 1024;             ///< bytes between the decoder and the network thread
    double reduceSec = 0.5;                    ///< push feeds: the same aircraft + kind at most this often (0 = everything)
};

class Hub {
public:
    static Hub& shared() { static Hub* h = new Hub(); return *h; }   // ★ never destroyed: a detached decoder may still call
    Hub() {
        int fds[2];
        if (::pipe(fds) == 0) {
            wakeR_ = fds[0]; wakeW_ = fds[1];
            for (int fd : fds) { const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC); }
        }
    }
    ~Hub() { stop(); if (wakeR_ >= 0) ::close(wakeR_); if (wakeW_ >= 0) ::close(wakeW_); }

    void setTiming(const Timing& t) { std::lock_guard<std::mutex> lk(mtx_); timing_ = t; }
    /** For a log line, the radio's name in the status. */
    void setLabel(const std::string& s) { std::lock_guard<std::mutex> lk(mtx_); label_ = s; }

    /** The owner's settings. Starts the network thread when something is on, stops it (closing everything) when not. */
    void configure(const Config& c) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cfg_ = c;
            ++gen_;
            wanted_.store(c.any(), std::memory_order_relaxed);
        }
        if (c.any()) startThread(); else stop();
        wake();
    }
    Config config() const { std::lock_guard<std::mutex> lk(mtx_); return cfg_; }

    /** ADS-B started (true) or stopped (false) on this radio. Push links follow it (with the grace above). */
    void setActive(bool on) {
        active_.store(on, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(mtx_); if (!on) lastActive_ = mono(); }
        wake();
    }

    /** ★ ON THE DECODER THREAD: encode, append, return. Never blocks on a socket. */
    /** `addr` = the aircraft's address (for the push feeds' reduce); `now` = seconds on any steady clock (the frame's). */
    void frame(const uint8_t* msg, int len, uint64_t ts12, float dbfs, uint32_t addr, double now) {
        if (!wanted_.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lk(mtx_);
        ++framesIn_;
        if (pending_.size() >= timing_.pendingCap) { ++pendingDropped_; return; }
        const uint8_t sig = signalByte(dbfs);
        if (!appendFrame(pending_, msg, len, ts12, sig)) return;
        ++pendingMsgs_;
        // ★ the reduced copy for the push feeds (built only while one is wanted)
        if (cfg_.push && pendingR_.size() < timing_.pendingCap) {
            bool send = timing_.reduceSec <= 0;
            if (!send) {
                const uint64_t k = reduceKey(msg, len, addr);
                auto it = lastSent_.find(k);
                if (it == lastSent_.end() || now - it->second >= timing_.reduceSec || now < it->second) {
                    send = true;
                    if (lastSent_.size() >= 20000) {   // ★ bounded: forget everything older than 5 s, else start again
                        for (auto i = lastSent_.begin(); i != lastSent_.end();) i = now - i->second > 5 ? lastSent_.erase(i) : std::next(i);
                        if (lastSent_.size() >= 20000) lastSent_.clear();
                    }
                    lastSent_[k] = now;
                }
            }
            if (send && appendFrame(pendingR_, msg, len, ts12, sig)) ++pendingRMsgs_;
        }
        if (pending_.size() >= 4096) wakeLocked();
    }

    /** {"on","adsb","frames","serve":{…},"push","feeds":[…],"line"} — the admin page's and the Setup card's status. */
    std::string statusJson() const { std::lock_guard<std::mutex> lk(mtx_); return status_.empty() ? idleStatus() : status_; }

    void stop() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!run_) return;
            run_ = false;
            t = std::move(thread_);
        }
        wake();
        if (t.joinable()) t.join();
        std::lock_guard<std::mutex> lk(mtx_);
        status_.clear();
    }

private:
    // ── a connection: a reader of the local server, or a push link ──
    struct Chunk { std::shared_ptr<const std::vector<uint8_t>> b; uint32_t msgs = 0; size_t off = 0; };
    struct Link {
        int fd = -1;
        enum St { Off, Connecting, Up, Wait } st = Off;
        std::string peer;                 // a reader's address
        Dest d;                           // a push link's destination
        std::deque<Chunk> q; size_t qBytes = 0;
        uint64_t bytes = 0, msgs = 0, dropped = 0, connects = 0;
        std::string err;
        double since = 0, nextTry = 0, backoff = 0, deadline = 0;
        long sinceUtc = 0;
    };

    static double mono() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void wake() { std::lock_guard<std::mutex> lk(mtx_); wakeLocked(); }
    void wakeLocked() { if (wakeW_ >= 0) { const char c = 1; ssize_t r = ::write(wakeW_, &c, 1); (void)r; } }

    void startThread() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (run_) return;
        run_ = true;
        thread_ = std::thread([this] { loop(); });
    }

    static void closeFd(int& fd) { if (fd >= 0) { ::close(fd); fd = -1; } }
    static void noSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
        (void)fd;
#endif
    }
    static void tuneSocket(int fd) {
        const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);   // ★ a dead link is found without sending made-up frames
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        noSigpipe(fd);
    }
    static ssize_t sendSome(int fd, const uint8_t* p, size_t n) {
#ifdef MSG_NOSIGNAL
        return ::send(fd, p, n, MSG_NOSIGNAL);
#else
        return ::send(fd, p, n, 0);
#endif
    }

    /** Queue a chunk on a link, then trim it to the cap from the oldest end. The chunk being sent (off > 0) is never cut
     *  — a half-sent frame would break the stream's framing for the reader. */
    void enqueue(Link& l, const std::shared_ptr<const std::vector<uint8_t>>& b, uint32_t msgs, size_t cap) {
        l.q.push_back(Chunk{b, msgs, 0});
        l.qBytes += b->size();
        while (l.qBytes > cap && l.q.size() > 1) {
            auto victim = l.q.begin();
            if (victim->off > 0) ++victim;
            if (victim == l.q.end()) break;
            l.qBytes -= victim->b->size() - victim->off;
            l.dropped += victim->msgs;
            l.q.erase(victim);
        }
    }
    /** Send what the socket takes. False = the link failed (err set). */
    bool flush(Link& l) {
        while (!l.q.empty()) {
            Chunk& c = l.q.front();
            const ssize_t n = sendSome(l.fd, c.b->data() + c.off, c.b->size() - c.off);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
                l.err = std::strerror(errno); return false;
            }
            c.off += size_t(n); l.qBytes -= size_t(n); l.bytes += uint64_t(n);
            if (c.off < c.b->size()) return true;
            l.msgs += c.msgs;
            l.q.pop_front();
        }
        return true;
    }
    /** Read and throw away whatever the far end says (a reader's commands, an aggregator's nothing). False = closed. */
    static bool drain(Link& l) {
        uint8_t buf[512];
        for (int k = 0; k < 8; ++k) {
            const ssize_t n = ::recv(l.fd, buf, sizeof buf, 0);
            if (n > 0) continue;
            if (n == 0) { l.err = "closed by the far end"; return false; }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
            l.err = std::strerror(errno); return false;
        }
        return true;
    }

    void failPush(Link& l, double now, const Timing& t) {
        closeFd(l.fd);
        l.q.clear(); l.qBytes = 0;
        if (l.st == Link::Up && now - l.since >= t.stableAfter) l.backoff = 0;
        l.backoff = l.backoff <= 0 ? t.backoffMin : std::min(t.backoffMax, l.backoff * 2);
        std::uniform_real_distribution<double> jit(0.8, 1.2);
        l.nextTry = now + l.backoff * jit(rng_);
        l.st = Link::Wait;
    }

    /** Resolve and start a non-blocking connect. ★ getaddrinfo can take a moment: it runs here, on this file's own
     *  thread, where the cost is a slightly late frame for the readers (their queues absorb it), never audio. */
    void startConnect(Link& l, double now, const Timing& t) {
        addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string port = std::to_string(l.d.port);
        const int rc = ::getaddrinfo(l.d.host.c_str(), port.c_str(), &hints, &res);
        if (rc != 0 || !res) {
            l.err = std::string("cannot find ") + l.d.host + " (" + gai_strerror(rc) + ")";
            if (res) ::freeaddrinfo(res);
            l.st = Link::Connecting; failPush(l, now, t); return;
        }
        int fd = -1;
        for (addrinfo* a = res; a; a = a->ai_next) {
            fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) continue;
            tuneSocket(fd);
            if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS) break;
            l.err = std::strerror(errno);
            ::close(fd); fd = -1;
        }
        ::freeaddrinfo(res);
        ++l.connects;
        if (fd < 0) { l.st = Link::Connecting; failPush(l, now, t); return; }
        l.fd = fd; l.st = Link::Connecting; l.deadline = now + t.connectTimeout;
    }

    void openListener(const Config& c, double now, const Timing& t) {
        listenErr_.clear();
        int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        bool v6 = fd >= 0;
        if (v6) { int off = 0; setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off); }
        else fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { listenErr_ = std::strerror(errno); listenRetryAt_ = now + t.listenRetry; return; }
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        int rc;
        if (v6) { sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_addr = in6addr_any; a.sin6_port = htons(uint16_t(c.port));
                  rc = ::bind(fd, (sockaddr*)&a, sizeof a); }
        else    { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(uint16_t(c.port));
                  rc = ::bind(fd, (sockaddr*)&a, sizeof a); }
        if (rc != 0 || ::listen(fd, 8) != 0) {
            listenErr_ = errno == EADDRINUSE ? "port " + std::to_string(c.port) + " is in use by another program (or another radio)"
                                             : std::string(std::strerror(errno));
            ::close(fd); listenRetryAt_ = now + t.listenRetry; return;
        }
        const int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC);
        listenFd_ = fd; listenPort_ = c.port;
    }

    static std::string peerOf(const sockaddr_storage& ss) {
        char b[INET6_ADDRSTRLEN + 1] = {0};
        if (ss.ss_family == AF_INET6) {
            const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
            ::inet_ntop(AF_INET6, &a->sin6_addr, b, sizeof b);
            std::string s = b;
            return s.rfind("::ffff:", 0) == 0 ? s.substr(7) : s;
        }
        const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
        ::inet_ntop(AF_INET, &a->sin_addr, b, sizeof b);
        return b;
    }

    void loop() {
        uint64_t seenGen = 0;
        Config c; Timing t;
        std::vector<Link> readers, pushes;
        double lastStatus = 0;
        for (;;) {
            const double now = mono();
            std::shared_ptr<const std::vector<uint8_t>> chunk, chunkR; uint32_t chunkMsgs = 0, chunkRMsgs = 0;
            bool active; double lastActive;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (!run_) break;
                t = timing_;
                if (gen_ != seenGen) {
                    const Config was = c;
                    c = cfg_; seenGen = gen_;
                    // the server: closed when switched off or moved
                    if (listenFd_ >= 0 && (!c.serve || c.port != listenPort_)) { closeFd(listenFd_); for (auto& r : readers) closeFd(r.fd); readers.clear(); }
                    if (listenFd_ >= 0 && was.open && !c.open)   // ★ narrowed to the LAN: anyone outside it goes now
                        for (auto& r : readers) if (!peerAllowed(r.peer, false)) closeFd(r.fd);
                    listenRetryAt_ = 0; listenErr_.clear();
                    // the push links: rebuilt, keeping a link (and its counters) whose destination is unchanged
                    std::vector<Link> next;
                    for (const Dest& d : c.feeds) {
                        Link n; n.d = d;
                        for (auto& o : pushes) if (o.d.host == d.host && o.d.port == d.port && o.d.uuid == d.uuid && o.fd != -2) { n = std::move(o); o.fd = -2; n.d = d; break; }
                        next.push_back(std::move(n));
                    }
                    for (auto& o : pushes) if (o.fd >= 0) ::close(o.fd);
                    pushes = std::move(next);
                }
                if (!pending_.empty()) {
                    chunk = std::make_shared<const std::vector<uint8_t>>(std::move(pending_));
                    pending_ = std::vector<uint8_t>();
                    pending_.reserve(4096);
                    chunkMsgs = pendingMsgs_; pendingMsgs_ = 0;
                }
                if (!pendingR_.empty()) {
                    chunkR = std::make_shared<const std::vector<uint8_t>>(std::move(pendingR_));
                    pendingR_ = std::vector<uint8_t>();
                    chunkRMsgs = pendingRMsgs_; pendingRMsgs_ = 0;
                }
                active = active_.load(std::memory_order_relaxed);
                lastActive = lastActive_;
            }
            // ── hand the new frames to every connected link ──
            if (chunk) for (auto& r : readers) if (r.fd >= 0) enqueue(r, chunk, chunkMsgs, t.linkCap);
            if (chunkR) for (auto& p : pushes) if (p.st == Link::Up) enqueue(p, chunkR, chunkRMsgs, t.linkCap);
            // ── the server ──
            if (c.serve && listenFd_ < 0 && now >= listenRetryAt_) openListener(c, now, t);
            // ── the push links ──
            const bool pushWanted = c.push && (active || (lastActive > 0 && now - lastActive < t.activeGrace));
            for (auto& p : pushes) {
                const bool want = pushWanted && p.d.on;
                if (!want) { if (p.st != Link::Off) { closeFd(p.fd); p.q.clear(); p.qBytes = 0; p.st = Link::Off; p.backoff = 0; p.err.clear(); } continue; }
                if ((p.st == Link::Off || p.st == Link::Wait) && now >= p.nextTry) startConnect(p, now, t);
                if (p.st == Link::Connecting && p.fd >= 0 && now > p.deadline) { p.err = "no answer (timed out)"; failPush(p, now, t); }
            }
            // ── wait for something to do ──
            std::vector<pollfd> pf;
            std::vector<std::pair<int, size_t>> who;   // 0 wake, 1 listener, 2 reader i, 3 push i
            pf.push_back({wakeR_, POLLIN, 0}); who.push_back({0, 0});
            if (listenFd_ >= 0) { pf.push_back({listenFd_, POLLIN, 0}); who.push_back({1, 0}); }
            for (size_t i = 0; i < readers.size(); ++i) if (readers[i].fd >= 0) {
                pf.push_back({readers[i].fd, short(POLLIN | (readers[i].q.empty() ? 0 : POLLOUT)), 0}); who.push_back({2, i}); }
            for (size_t i = 0; i < pushes.size(); ++i) if (pushes[i].fd >= 0) {
                const short ev = pushes[i].st == Link::Connecting ? POLLOUT : short(POLLIN | (pushes[i].q.empty() ? 0 : POLLOUT));
                pf.push_back({pushes[i].fd, ev, 0}); who.push_back({3, i}); }
            ::poll(pf.data(), nfds_t(pf.size()), 100);
            const double now2 = mono();
            for (size_t k = 0; k < pf.size(); ++k) {
                const short re = pf[k].revents;
                if (!re) continue;
                const int kind = who[k].first; const size_t i = who[k].second;
                if (kind == 0) { char b[64]; while (::read(wakeR_, b, sizeof b) > 0) {} continue; }
                if (kind == 1) {
                    for (int n = 0; n < 4; ++n) {
                        sockaddr_storage ss{}; socklen_t sl = sizeof ss;
                        const int fd = ::accept(listenFd_, (sockaddr*)&ss, &sl);
                        if (fd < 0) break;
                        const std::string peer = peerOf(ss);
                        size_t live = 0; for (auto& r : readers) if (r.fd >= 0) ++live;
                        if (!peerAllowed(peer, c.open)) { ::close(fd); ++refused_; lastRefused_ = peer + " (not on this network)"; continue; }
                        if (live >= size_t(kMaxReaders)) { ::close(fd); ++refused_; lastRefused_ = peer + " (8 readers already)"; continue; }
                        tuneSocket(fd);
                        Link r; r.fd = fd; r.peer = peer; r.st = Link::Up; r.since = now2; r.sinceUtc = (long)std::time(nullptr);
                        readers.push_back(std::move(r));
                    }
                    continue;
                }
                Link& l = kind == 2 ? readers[i] : pushes[i];
                if (l.fd < 0) continue;
                if (kind == 3 && l.st == Link::Connecting) {
                    int e = 0; socklen_t el = sizeof e;
                    getsockopt(l.fd, SOL_SOCKET, SO_ERROR, &e, &el);
                    if (e != 0 || (re & (POLLERR | POLLHUP))) { l.err = e ? std::strerror(e) : "refused"; failPush(l, now2, t); continue; }
                    l.st = Link::Up; l.since = now2; l.sinceUtc = (long)std::time(nullptr); l.err.clear();
                    // ★ the station's UUID first, where the owner gave one (counted as bytes, not as a message)
                    if (!l.d.uuid.empty()) enqueue(l, std::make_shared<const std::vector<uint8_t>>(uuidFrame(l.d.uuid)), 0, t.linkCap);
                    continue;
                }
                bool okay = true;
                if (re & (POLLIN | POLLHUP | POLLERR)) okay = drain(l);
                if (okay && (re & POLLOUT)) okay = flush(l);
                if (!okay) { if (kind == 2) closeFd(l.fd); else failPush(l, now2, t); }
            }
            readers.erase(std::remove_if(readers.begin(), readers.end(), [](const Link& r) { return r.fd < 0; }), readers.end());
            // a link with something queued is written at once (not only when poll says writable next time round)
            for (auto& r : readers) if (r.fd >= 0 && !r.q.empty() && !flush(r)) closeFd(r.fd);
            for (auto& p : pushes) if (p.st == Link::Up && p.fd >= 0 && !p.q.empty() && !flush(p)) failPush(p, now2, t);
            if (now2 - lastStatus >= 0.5) { lastStatus = now2; publish(c, readers, pushes, active, now2); }
        }
        // stopped: close everything
        closeFd(listenFd_);
        for (auto& r : readers) closeFd(r.fd);
        for (auto& p : pushes) closeFd(p.fd);
    }

    void publish(const Config& c, const std::vector<Link>& readers, const std::vector<Link>& pushes, bool active, double now) {
        const long utc = (long)std::time(nullptr);
        std::string j = std::string("{\"on\":") + (c.any() ? "true" : "false") + ",\"adsb\":" + (active ? "true" : "false");
        uint64_t frames, pdrop;
        { std::lock_guard<std::mutex> lk(mtx_); frames = framesIn_; pdrop = pendingDropped_; }
        j += ",\"frames\":" + std::to_string(frames) + ",\"behind\":" + std::to_string(pdrop);
        j += std::string(",\"serve\":{\"on\":") + (c.serve ? "true" : "false") + ",\"port\":" + std::to_string(c.port)
           + ",\"open\":" + (c.open ? "true" : "false") + ",\"listening\":" + (listenFd_ >= 0 ? "true" : "false")
           + ",\"error\":\"" + jsonEsc(listenErr_) + "\",\"refused\":" + std::to_string(refused_)
           + ",\"lastRefused\":\"" + jsonEsc(lastRefused_) + "\",\"readers\":[";
        std::string words;
        size_t nr = 0;
        for (const auto& r : readers) {
            if (r.fd < 0) continue;
            if (nr++) j += ',';
            j += "{\"peer\":\"" + jsonEsc(r.peer) + "\",\"since\":" + std::to_string(r.sinceUtc) + ",\"bytes\":" + std::to_string(r.bytes)
               + ",\"msgs\":" + std::to_string(r.msgs) + ",\"dropped\":" + std::to_string(r.dropped) + "}";
        }
        j += "]},\"push\":" + std::string(c.push ? "true" : "false") + ",\"feeds\":[";
        if (c.serve) {
            words = listenFd_ >= 0 ? "Beast on port " + std::to_string(c.port) + " (" + std::to_string(nr) + (nr == 1 ? " reader)" : " readers)")
                                   : "Beast port " + std::to_string(c.port) + ": " + (listenErr_.empty() ? std::string("opening") : listenErr_);
        }
        for (size_t i = 0; i < pushes.size(); ++i) {
            const Link& p = pushes[i];
            const char* st = p.st == Link::Up ? "connected" : p.st == Link::Connecting ? "connecting" : p.st == Link::Wait ? "waiting" : "off";
            if (i) j += ',';
            j += "{\"host\":\"" + jsonEsc(p.d.host) + "\",\"port\":" + std::to_string(p.d.port) + ",\"on\":" + (p.d.on ? "true" : "false")
               + ",\"uuid\":" + (p.d.uuid.empty() ? "false" : "true") + ",\"state\":\"" + st + "\",\"since\":" + std::to_string(p.st == Link::Up ? p.sinceUtc : 0)
               + ",\"bytes\":" + std::to_string(p.bytes) + ",\"msgs\":" + std::to_string(p.msgs) + ",\"dropped\":" + std::to_string(p.dropped)
               + ",\"connects\":" + std::to_string(p.connects) + ",\"error\":\"" + jsonEsc(p.err) + "\""
               + ",\"retryIn\":" + std::to_string(p.st == Link::Wait ? std::max(0L, std::lround(p.nextTry - now)) : 0L) + "}";
            if (!c.push || !p.d.on) continue;
            std::string w = p.d.host + ": ";
            if (p.st == Link::Up) w += "connected, " + std::to_string(p.msgs) + " messages";
            else if (p.st == Link::Connecting) w += "connecting";
            else if (p.st == Link::Wait) w += "retrying in " + std::to_string(std::max(0L, std::lround(p.nextTry - now))) + " s (" + p.err + ")";
            else w += "waits for ADS-B";
            if (p.dropped) w += ", " + std::to_string(p.dropped) + " dropped (link too slow)";
            words += (words.empty() ? "" : " · ") + w;
        }
        if (words.empty()) words = c.any() ? "" : "off";
        if (c.any() && !active) words += (words.empty() ? "" : " · ") + std::string("ADS-B is not running on this radio now, so nothing is being sent");
        j += "],\"line\":\"" + jsonEsc(words) + "\",\"at\":" + std::to_string(utc) + "}";
        std::lock_guard<std::mutex> lk(mtx_);
        status_ = j;
    }
    std::string idleStatus() const {
        return "{\"on\":false,\"adsb\":" + std::string(active_.load() ? "true" : "false")
             + ",\"frames\":0,\"behind\":0,\"serve\":{\"on\":false,\"port\":" + std::to_string(cfg_.port)
             + ",\"open\":false,\"listening\":false,\"error\":\"\",\"refused\":0,\"lastRefused\":\"\",\"readers\":[]},"
               "\"push\":false,\"feeds\":[],\"line\":\"off\",\"at\":" + std::to_string((long)std::time(nullptr)) + "}";
    }

    mutable std::mutex mtx_;
    Config cfg_; uint64_t gen_ = 0; Timing timing_; std::string label_;
    std::atomic<bool> wanted_{false}, active_{false};
    double lastActive_ = 0;
    std::vector<uint8_t> pending_, pendingR_; uint32_t pendingMsgs_ = 0, pendingRMsgs_ = 0;
    std::unordered_map<uint64_t, double> lastSent_;   // the reduce's memory (bounded in frame())
    uint64_t framesIn_ = 0, pendingDropped_ = 0;
    std::string status_;
    bool run_ = false; std::thread thread_;
    int wakeR_ = -1, wakeW_ = -1;
    // network-thread state
    int listenFd_ = -1, listenPort_ = 0; double listenRetryAt_ = 0; std::string listenErr_;
    uint64_t refused_ = 0; std::string lastRefused_;
    std::mt19937 rng_{std::random_device{}()};
};

}  // namespace beast
}  // namespace vibe
