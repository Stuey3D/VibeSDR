#pragma once
// ★★★ A SMALL MQTT 3.1.1 CLIENT — for the external antenna switch (2026-10-09, docs/v12/ANTENNA-SWITCH-ROTATOR.md).
//
// Stuart: "build it … this can be pushed to [Chicopee] as a test". Chicopee runs VibeServer Lite on Android — no GPIO — so
// the switch is reached over the NETWORK, and MQTT is what Tasmota and most ESP32 relay boards already speak. This header
// is shared by Linux and Lite (the shim runs in both), header-only and dependency-free like the rest of vibe_*.h.
//
// ★ Plain TCP (port 1883), QoS 0 only, clean session. That is the whole of what a relay switch on the owner's LAN needs;
//   TLS and QoS 1/2 would bring a dependency (the daemon has no TLS stack of its own) for no gain on a LAN.
// ★ The CODEC is free functions so test-mqtt.cpp can check the bytes against the spec without a broker; the Client wraps
//   a socket and a reader thread, and reconnects on its own with backoff.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibe {
namespace mqtt {

// ── codec (MQTT 3.1.1, OASIS standard) ────────────────────────────────────────────────────────

/** The variable-length "remaining length" (§2.2.3): 7 bits a byte, high bit = more. Max 268,435,455. */
inline void putLength(std::string& out, size_t n) {
    do {
        uint8_t b = n % 128; n /= 128;
        if (n) b |= 0x80;
        out.push_back(char(b));
    } while (n);
}
/** Read a remaining length from `p` (len `avail`). Returns bytes used, 0 = need more, -1 = malformed (> 4 bytes). */
inline int getLength(const uint8_t* p, size_t avail, size_t& n) {
    n = 0; size_t mult = 1;
    for (int i = 0; i < 4; ++i) {
        if (size_t(i) >= avail) return 0;
        n += size_t(p[i] & 0x7F) * mult;
        if (!(p[i] & 0x80)) return i + 1;
        mult *= 128;
    }
    return -1;
}
inline void putStr(std::string& out, const std::string& s) {
    const size_t n = s.size() > 65535 ? 65535 : s.size();
    out.push_back(char(n >> 8)); out.push_back(char(n & 0xFF));
    out.append(s, 0, n);
}
inline std::string packet(uint8_t header, const std::string& body) {
    std::string p(1, char(header)); putLength(p, body.size()); p += body; return p;
}
/** CONNECT (§3.1): protocol "MQTT" level 4, clean session, optional username/password, keepalive in seconds. */
inline std::string connectPacket(const std::string& clientId, uint16_t keepAlive,
                                 const std::string& user = "", const std::string& pass = "") {
    std::string b;
    putStr(b, "MQTT");
    b.push_back(char(4));                                   // protocol level 3.1.1
    uint8_t flags = 0x02;                                    // clean session
    if (!user.empty()) flags |= 0x80;
    if (!user.empty() && !pass.empty()) flags |= 0x40;
    b.push_back(char(flags));
    b.push_back(char(keepAlive >> 8)); b.push_back(char(keepAlive & 0xFF));
    putStr(b, clientId);
    if (!user.empty()) putStr(b, user);
    if (!user.empty() && !pass.empty()) putStr(b, pass);
    return packet(0x10, b);
}
/** PUBLISH QoS 0 (§3.3): no packet id. `retain` only for the owner's own state topics — never for a command. */
inline std::string publishPacket(const std::string& topic, const std::string& payload, bool retain = false) {
    std::string b; putStr(b, topic); b += payload;
    return packet(uint8_t(0x30 | (retain ? 1 : 0)), b);
}
/** SUBSCRIBE (§3.8): flags must be 0010; one filter, requested QoS 0. */
inline std::string subscribePacket(uint16_t id, const std::string& filter) {
    std::string b; b.push_back(char(id >> 8)); b.push_back(char(id & 0xFF));
    putStr(b, filter); b.push_back(char(0));
    return packet(0x82, b);
}
inline std::string pingPacket() { return std::string("\xC0\x00", 2); }
inline std::string disconnectPacket() { return std::string("\xE0\x00", 2); }

/** One decoded control packet. For PUBLISH, topic + payload are filled. */
struct Packet { uint8_t type = 0; uint8_t flags = 0; std::string body; std::string topic, payload; };

/** Take one whole packet off the front of `buf`. true = one taken; false = need more (or malformed: `bad` set). */
inline bool takePacket(std::string& buf, Packet& out, bool& bad) {
    bad = false;
    if (buf.size() < 2) return false;
    size_t len = 0;
    const int used = getLength(reinterpret_cast<const uint8_t*>(buf.data()) + 1, buf.size() - 1, len);
    if (used < 0) { bad = true; return false; }
    if (used == 0 || buf.size() < size_t(1 + used) + len) return false;
    const uint8_t h = uint8_t(buf[0]);
    out = Packet{};
    out.type = h >> 4; out.flags = h & 0x0F;
    out.body = buf.substr(size_t(1 + used), len);
    buf.erase(0, size_t(1 + used) + len);
    if (out.type == 3) {                                     // PUBLISH
        const std::string& b = out.body;
        if (b.size() < 2) { bad = true; return false; }
        const size_t tl = (size_t(uint8_t(b[0])) << 8) | uint8_t(b[1]);
        if (b.size() < 2 + tl) { bad = true; return false; }
        out.topic = b.substr(2, tl);
        size_t at = 2 + tl;
        if (((out.flags >> 1) & 3) > 0) at += 2;             // QoS 1/2 carry a packet id
        out.payload = at <= b.size() ? b.substr(at) : std::string();
    }
    return true;
}

/** MQTT topic-filter match (§4.7): '+' one level, '#' the rest (last only). */
inline bool topicMatches(const std::string& filter, const std::string& topic) {
    size_t f = 0, t = 0;
    while (f < filter.size()) {
        if (filter[f] == '#') return true;
        const size_t fe = filter.find('/', f), te = topic.find('/', t);
        const std::string fl = filter.substr(f, fe == std::string::npos ? std::string::npos : fe - f);
        if (t > topic.size()) return false;
        const std::string tl = topic.substr(t, te == std::string::npos ? std::string::npos : te - t);
        if (fl != "+" && fl != tl) return false;
        if (fe == std::string::npos) return te == std::string::npos;
        if (te == std::string::npos) return filter.compare(fe + 1, std::string::npos, "#") == 0;
        f = fe + 1; t = te + 1;
    }
    return t >= topic.size();
}

// ── a blocking TCP connect with a timeout (for the client and the LAN search) ────────────────

inline int tcpConnect(const std::string& host, int port, int timeoutMs) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -1;
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, timeoutMs) != 1) { ::close(fd); return -1; }
        int err = 0; socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { ::close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fl);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}
inline bool sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
#endif
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

// ── the client ────────────────────────────────────────────────────────────────────────────────

/**
 * ★★ ONE CONNECTION TO THE OWNER'S BROKER, KEPT UP BY ITSELF. start() spawns a thread that connects, (re)subscribes to
 * every filter it was given, delivers each PUBLISH to `onMessage`, pings at half the keepalive, and on any failure waits
 * (1 s doubling to 30 s) and tries again — a broker rebooting must not need the server restarted.
 * ★ publish() is safe from any thread; it is dropped (returns false) while disconnected — a switch command must never be
 *   queued and fired minutes later, when it would move an antenna somebody has since chosen differently.
 */
class Client {
public:
    using OnMessage = std::function<void(const std::string& topic, const std::string& payload)>;
    using OnState   = std::function<void(bool connected, const std::string& why)>;

    ~Client() { stop(); }

    void start(const std::string& host, int port, const std::string& clientId,
               const std::string& user, const std::string& pass,
               std::vector<std::string> filters, OnMessage onMsg, OnState onState = nullptr) {
        stop();
        host_ = host; port_ = port; id_ = clientId; user_ = user; pass_ = pass;
        filters_ = std::move(filters); onMsg_ = std::move(onMsg); onState_ = std::move(onState);
        run_ = true;
        thread_ = std::thread([this] { loop(); });
    }
    void stop() {
        run_ = false;
        { std::lock_guard<std::mutex> lk(m_); if (fd_ >= 0) { sendAll(fd_, disconnectPacket()); ::shutdown(fd_, SHUT_RDWR); } }
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lk(m_);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        connected_ = false;
    }
    bool connected() const { return connected_.load(); }
    bool publish(const std::string& topic, const std::string& payload, bool retain = false) {
        std::lock_guard<std::mutex> lk(m_);
        if (fd_ < 0 || !connected_) return false;
        return sendAll(fd_, publishPacket(topic, payload, retain));
    }

private:
    void setState(bool on, const std::string& why) {
        if (connected_.exchange(on) != on || !on) { if (onState_) onState_(on, why); }
    }
    void loop() {
        int backoffMs = 1000;
        while (run_) {
            std::string why;
            if (session(why)) backoffMs = 1000;
            setState(false, why);
            for (int waited = 0; run_ && waited < backoffMs; waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoffMs = backoffMs * 2 > 30000 ? 30000 : backoffMs * 2;
        }
    }
    /** One connection, start to end. true = it got as far as CONNACK (so the next try starts from a short wait). */
    bool session(std::string& why) {
        const int fd = tcpConnect(host_, port_, 4000);
        if (fd < 0) { why = "cannot reach the broker at " + host_ + ":" + std::to_string(port_); return false; }
        { std::lock_guard<std::mutex> lk(m_); fd_ = fd; }
        const uint16_t keep = 30;
        bool ok = sendAll(fd, connectPacket(id_, keep, user_, pass_));
        std::string buf; bool acked = false; uint16_t pid = 1;
        auto lastTx = std::chrono::steady_clock::now();
        char tmp[4096];
        while (run_ && ok) {
            pollfd p{fd, POLLIN, 0};
            const int pr = ::poll(&p, 1, 500);
            if (pr < 0) { why = "poll failed"; break; }
            if (pr == 1) {
                const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
                if (n <= 0) { why = acked ? "the broker closed the connection" : "the broker refused the connection"; break; }
                buf.append(tmp, size_t(n));
                Packet pk; bool bad = false;
                while (takePacket(buf, pk, bad)) {
                    if (pk.type == 2) {                       // CONNACK: byte 2 is the return code
                        const int rc = pk.body.size() >= 2 ? uint8_t(pk.body[1]) : 255;
                        if (rc != 0) {
                            why = rc == 4 || rc == 5 ? "the broker refused the username/password"
                                                     : "the broker refused the connection (code " + std::to_string(rc) + ")";
                            ok = false; break;
                        }
                        acked = true;
                        std::lock_guard<std::mutex> lk(m_);
                        for (const auto& f : filters_) { sendAll(fd, subscribePacket(pid++, f)); if (!pid) pid = 1; }
                        connected_ = true;
                        if (onState_) onState_(true, "");
                    } else if (pk.type == 3) {
                        if (onMsg_) onMsg_(pk.topic, pk.payload);
                    }
                }
                if (bad) { why = "malformed data from the broker"; break; }
            }
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastTx).count() >= keep / 2) {
                std::lock_guard<std::mutex> lk(m_);
                ok = sendAll(fd, pingPacket());
                lastTx = now;
                if (!ok) why = "the connection to the broker dropped";
            }
        }
        std::lock_guard<std::mutex> lk(m_);
        ::close(fd); fd_ = -1;
        return acked;
    }

    std::string host_, id_, user_, pass_;
    int port_ = 1883;
    std::vector<std::string> filters_;
    OnMessage onMsg_;
    OnState onState_;
    std::atomic<bool> run_{false}, connected_{false};
    std::thread thread_;
    std::mutex m_;
    int fd_ = -1;
};

}  // namespace mqtt
}  // namespace vibe
