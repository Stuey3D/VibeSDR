// ws.h — a minimal WebSocket CLIENT for vibe-hhui: loopback to the handheld's own engine only.
//
// ★ One reader thread per socket; received messages go to a queue the UI drains once a frame (no callbacks into the
//   UI from another thread). Sends are serialised by a mutex. Client frames are masked (RFC 6455 §5.3); the server's
//   are not. Fragmented messages are reassembled; pings answered; a close or an error marks the socket dead and the
//   owner reconnects.
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace hh {

struct WsMessage {
    bool text = false;
    std::string data;   // text, or the binary payload
};

class WsClient {
public:
    ~WsClient() { close(); }

    /** Connect to 127.0.0.1:port and upgrade on `path` (with its query). False on any failure. */
    bool open(int port, const std::string& path) {
        close();
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, (sockaddr*)&a, sizeof a) != 0) { ::close(fd_); fd_ = -1; return false; }
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        timeval tv{5, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port)
            + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
            + key() + "\r\nUser-Agent: vibe-hhui (R36S native)\r\n\r\n";
        if (!writeAll(req.data(), req.size())) { ::close(fd_); fd_ = -1; return false; }
        // the response head
        std::string head;
        char c;
        while (head.size() < 8192) {
            const ssize_t n = ::recv(fd_, &c, 1, 0);
            if (n != 1) { ::close(fd_); fd_ = -1; return false; }
            head.push_back(c);
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        if (head.compare(0, 12, "HTTP/1.1 101") != 0) { ::close(fd_); fd_ = -1; return false; }
        tv = {0, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);   // blocking reads from here on
        alive_ = true;
        th_ = std::thread([this] { readLoop(); });
        return true;
    }

    bool alive() const { return alive_.load(); }

    void close() {
        alive_ = false;
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
        if (th_.joinable()) th_.join();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        std::lock_guard<std::mutex> lk(qm_);
        q_.clear();
    }

    bool sendText(const std::string& s) { return sendFrame(0x1, s.data(), s.size()); }

    /** Everything received since the last call (oldest first). */
    void drain(std::vector<WsMessage>& out) {
        std::lock_guard<std::mutex> lk(qm_);
        out.assign(std::make_move_iterator(q_.begin()), std::make_move_iterator(q_.end()));
        q_.clear();
    }

    /** ★ Binary frames are kept to at most this many queued (the newest win): a UI that stalls must not grow memory. */
    size_t maxBinaryQueued = 64;

private:
    int fd_ = -1;
    std::atomic<bool> alive_{false};
    std::thread th_;
    std::mutex sm_, qm_;
    std::deque<WsMessage> q_;
    std::mt19937 rng_{std::random_device{}()};

    std::string key() {
        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        uint8_t k[16];
        for (auto& b : k) b = (uint8_t)rng_();
        std::string s;
        for (int i = 0; i < 16; i += 3) {
            const uint32_t v = (uint32_t)k[i] << 16 | (i + 1 < 16 ? (uint32_t)k[i + 1] << 8 : 0) | (i + 2 < 16 ? k[i + 2] : 0);
            s += b64[(v >> 18) & 63]; s += b64[(v >> 12) & 63];
            s += i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
            s += i + 2 < 16 ? b64[v & 63] : '=';
        }
        return s;
    }

    bool writeAll(const void* p, size_t n) {
        const char* c = (const char*)p;
        while (n) {
            const ssize_t w = ::send(fd_, c, n, MSG_NOSIGNAL);
            if (w <= 0) return false;
            c += w; n -= (size_t)w;
        }
        return true;
    }
    bool readAll(void* p, size_t n) {
        char* c = (char*)p;
        while (n) {
            const ssize_t r = ::recv(fd_, c, n, 0);
            if (r <= 0) return false;
            c += r; n -= (size_t)r;
        }
        return true;
    }

    bool sendFrame(uint8_t op, const void* data, size_t len) {
        if (!alive_) return false;
        std::vector<uint8_t> f;
        f.reserve(len + 14);
        f.push_back(0x80 | op);
        if (len < 126) f.push_back(0x80 | (uint8_t)len);
        else if (len < 65536) { f.push_back(0x80 | 126); f.push_back((uint8_t)(len >> 8)); f.push_back((uint8_t)len); }
        else { f.push_back(0x80 | 127); for (int i = 7; i >= 0; i--) f.push_back((uint8_t)(len >> (i * 8))); }
        uint8_t m[4];
        { std::lock_guard<std::mutex> lk(sm_); for (auto& b : m) b = (uint8_t)rng_(); }
        f.insert(f.end(), m, m + 4);
        const uint8_t* d = (const uint8_t*)data;
        for (size_t i = 0; i < len; i++) f.push_back(d[i] ^ m[i & 3]);
        std::lock_guard<std::mutex> lk(sm_);
        if (!writeAll(f.data(), f.size())) { alive_ = false; return false; }
        return true;
    }

    void readLoop() {
        std::string msg;
        uint8_t msgOp = 0;
        while (alive_) {
            uint8_t h[2];
            if (!readAll(h, 2)) break;
            const bool fin = h[0] & 0x80;
            const uint8_t op = h[0] & 0x0F;
            uint64_t len = h[1] & 0x7F;
            if (len == 126) { uint8_t e[2]; if (!readAll(e, 2)) break; len = (uint64_t)e[0] << 8 | e[1]; }
            else if (len == 127) { uint8_t e[8]; if (!readAll(e, 8)) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | e[i]; }
            if (len > (8u << 20)) break;                      // ★ nothing the engine sends is this big
            uint8_t mask[4] = {0, 0, 0, 0};
            if (h[1] & 0x80) { if (!readAll(mask, 4)) break; }
            std::string payload((size_t)len, '\0');
            if (len && !readAll(&payload[0], (size_t)len)) break;
            if (h[1] & 0x80) for (size_t i = 0; i < payload.size(); i++) payload[i] ^= (char)mask[i & 3];
            if (op == 0x8) break;                                                 // close
            if (op == 0x9) { sendFrame(0xA, payload.data(), payload.size()); continue; }   // ping → pong
            if (op == 0xA) continue;
            if (op == 0x1 || op == 0x2) { msg = std::move(payload); msgOp = op; }
            else if (op == 0x0) msg += payload;                                   // continuation
            else continue;
            if (!fin) continue;
            WsMessage m{msgOp == 0x1, std::move(msg)};
            msg.clear();
            std::lock_guard<std::mutex> lk(qm_);
            if (!m.text) {
                size_t nb = 0;
                for (auto& x : q_) nb += !x.text;
                if (nb >= maxBinaryQueued)
                    for (auto it = q_.begin(); it != q_.end(); ++it) if (!it->text) { q_.erase(it); break; }
            }
            q_.push_back(std::move(m));
        }
        alive_ = false;
    }
};

}  // namespace hh
