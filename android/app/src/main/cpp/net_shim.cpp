// VibeSDR V5 — clean-room, GPL-free TCP socket wrapper (POSIX). See net_shim.h.
#include "net_shim.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace net {

// ── Socket ──────────────────────────────────────────────────────────────────
Socket::~Socket() { close(); }

void Socket::closeAfterFlush() {
    if (!open_) return;
    open_ = false;
    if (fd_ < 0) return;
    ::shutdown(fd_, SHUT_WR);            // flush what is queued, then FIN — not an abort
    // Drain whatever the peer had already sent, so the close cannot become a reset. Bounded:
    // 8 KB or ~20 ms, whichever comes first.
    char sink[1024];
    const auto start = std::chrono::steady_clock::now();
    int drained = 0;
    for (;;) {
        struct pollfd pf { fd_, POLLIN, 0 };
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count();
        if (ms >= 20 || drained >= 8192) break;
        const int pr = ::poll(&pf, 1, (int)(20 - ms));
        if (pr <= 0) break;
        const ssize_t n = ::recv(fd_, sink, sizeof sink, MSG_DONTWAIT);
        if (n <= 0) break;               // EOF or would-block: nothing left worth draining
        drained += (int)n;
    }
    ::close(fd_); fd_ = -1;
}

void Socket::close() {
    if (!open_) return;
    open_ = false;
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
}

std::string Socket::peerAddress() {
    if (!effAddr_.empty()) return effAddr_;   // real client, via a trusted proxy
    return socketPeerAddress();
}

std::string Socket::socketPeerAddress() {
    // ★★★ ASKED ONCE AND REMEMBERED. This called getpeername() EVERY time, and getpeername fails
    //     with ENOTCONN the moment the peer has gone — so a connection that closed quickly was
    //     logged with NO ADDRESS AT ALL. The owner's connection history filled with rows reading
    //     "—", 0s, no client: unattributable entries on a public receiver, where the whole point
    //     of the log is being able to see who was here and block them by name (Stuart, 2026-08-15,
    //     looking at his own admin page: "look at all these with no IP address").
    // ★★ The address is a FACT ABOUT THE CONNECTION, fixed the moment it is accepted, so caching
    //    it is not an optimisation — it is the difference between recording what happened and
    //    recording what was still true when we got round to asking.
    // ★ IPv6-aware: a v4-mapped address (::ffff:a.b.c.d) is unwrapped to its IPv4 form, which is
    //   what the ban list, the geo lookup and the loopback test all expect. Reading a v6 peer
    //   through a sockaddr_in used to yield a plausible-looking WRONG address.
    if (!cachedAddr_.empty()) return cachedAddr_;
    if (fd_ < 0) return "";
    struct sockaddr_storage ss {};
    socklen_t len = sizeof(ss);
    if (::getpeername(fd_, (struct sockaddr*)&ss, &len) != 0) return "";
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        const auto* a4 = reinterpret_cast<const sockaddr_in*>(&ss);
        if (!::inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof buf)) return "";
    } else if (ss.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            struct in_addr v4 {};
            std::memcpy(&v4, reinterpret_cast<const uint8_t*>(&a6->sin6_addr) + 12, 4);
            if (!::inet_ntop(AF_INET, &v4, buf, sizeof buf)) return "";
        } else if (!::inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof buf)) {
            return "";
        }
    } else {
        return "";
    }
    cachedAddr_ = buf;
    return cachedAddr_;
}

bool Socket::setSendBufferSize(int bytes) {
    if (fd_ < 0) return false;
    return ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0;
}

bool Socket::setRecvBufferSize(int bytes) {
    if (fd_ < 0) return false;
    return ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
}

int Socket::send(const uint8_t* data, size_t len, const Address*) {
    if (!open_ || fd_ < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = ::send(fd_, data + off, len - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            open_ = false;
            return -1;
        }
        if (w == 0) { open_ = false; return -1; }
        off += (size_t)w;
    }
    return (int)len;
}

int Socket::sendstr(const std::string& str, const Address* dest) {
    return send((const uint8_t*)str.data(), str.size(), dest);
}

// One recv() call, honouring a millisecond timeout via poll().
int Socket::recvRaw(uint8_t* data, size_t maxLen, int timeout) {
    if (!open_ || fd_ < 0) return -1;
    if (timeout != NO_TIMEOUT) {
        struct pollfd pfd { fd_, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, timeout);
        if (pr == 0) return 0;                      // timed out, nothing ready
        if (pr < 0) { if (errno == EINTR) return 0; open_ = false; return -1; }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { open_ = false; return -1; }
    }
    ssize_t r = ::recv(fd_, data, maxLen, 0);
    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        open_ = false;
        return -1;
    }
    if (r == 0) { open_ = false; return 0; }        // peer closed
    return (int)r;
}

int Socket::recv(uint8_t* data, size_t maxLen, bool forceLen, int timeout, Address*) {
    if (!forceLen) return recvRaw(data, maxLen, timeout);
    // forceLen: loop until exactly maxLen bytes (or error/close).
    size_t got = 0;
    while (got < maxLen) {
        int r = recvRaw(data + got, maxLen - got, timeout);
        if (r < 0) return -1;
        if (r == 0) {
            if (!open_) return (got > 0) ? (int)got : 0;   // closed
            continue;                                       // timeout slice; keep waiting
        }
        got += (size_t)r;
    }
    return (int)got;
}

int Socket::recvline(std::string& str, int maxLen, int timeout, Address*) {
    str.clear();
    uint8_t c;
    while (maxLen <= 0 || (int)str.size() < maxLen) {
        int r = recvRaw(&c, 1, timeout);
        if (r < 0) return -1;
        if (r == 0) { if (!open_) return str.empty() ? -1 : (int)str.size(); else return -1; }
        if (c == '\n') return (int)str.size();      // newline stripped, '\r' kept
        str.push_back((char)c);
    }
    return (int)str.size();
}

// ── Listener ────────────────────────────────────────────────────────────────
Listener::~Listener() { stop(); }

void Listener::stop() {
    if (!open_) return;
    open_ = false;
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

std::shared_ptr<Socket> Listener::accept(Address*, int timeout) {
    if (!open_ || fd_ < 0) return nullptr;
    if (timeout != NO_TIMEOUT) {
        struct pollfd pfd { fd_, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, timeout);
        if (pr <= 0) return nullptr;                // timeout, would-block, or error
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return nullptr;
    }
    int cfd = ::accept(fd_, nullptr, nullptr);
    if (cfd < 0) return nullptr;
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // ★ AGGRESSIVE TCP KEEPALIVE — so a client that VANISHES is noticed in ~10s, not the OS
    // default of ~2 HOURS. The audio socket is server→client only: the client never sends on it,
    // so our recv loop blocks there, and TCP happily buffers our outbound audio to a silently-dead
    // peer (no clean FIN over a flaky watch link) — so `send()` doesn't error either. The server
    // then sat on a phantom "1 connection" long after the listener had gone (Stuart, 2026-07-23).
    // Keepalive probes make the OS surface ECONNRESET on both recv and send, flipping the socket
    // closed and freeing the single-occupant slot promptly. Idle 5s + 2s×2 probes ≈ 9s to detect.
    ::setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 5, intvl = 2, cnt = 2;
#if defined(TCP_KEEPIDLE)          // Linux / Android
    ::setsockopt(cfd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
#elif defined(TCP_KEEPALIVE)       // macOS/BSD: seconds of idle before the first probe
    ::setsockopt(cfd, IPPROTO_TCP, TCP_KEEPALIVE, &idle,  sizeof(idle));
#endif
#if defined(TCP_KEEPINTVL)
    ::setsockopt(cfd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#if defined(TCP_KEEPCNT)
    ::setsockopt(cfd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
#endif
    // Keepalive only probes an IDLE connection — but the audio socket is CONTINUOUSLY SENDING to
    // the client, so when that client dies the OS retransmits our unacked audio instead, and the
    // default retransmit budget is minutes. Bound it so a dead active peer is dropped in ~10s too.
#if defined(TCP_USER_TIMEOUT)      // Linux / Android: ms of unacked data before forced close
    int uto = 10000;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_USER_TIMEOUT, &uto, sizeof(uto));
#elif defined(TCP_RXT_CONNDROPTIME) // macOS/BSD: seconds of retransmit before dropping
    int rxt = 10;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_RXT_CONNDROPTIME, &rxt, sizeof(rxt));
#endif
    return std::make_shared<Socket>(cfd);
}

// ── Factories ───────────────────────────────────────────────────────────────
std::shared_ptr<Listener> listen(const std::string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("bind() failed on " + host + ":" + std::to_string(port));
    }
    if (::listen(fd, 8) < 0) {
        ::close(fd);
        throw std::runtime_error("listen() failed");
    }
    return std::make_shared<Listener>(fd);
}

std::shared_ptr<Socket> connect(const std::string& host, int port, int timeoutMs) {
    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        throw std::runtime_error("getaddrinfo() failed for " + host);
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { ::freeaddrinfo(res); throw std::runtime_error("socket() failed"); }

    if (timeoutMs > 0) {
        // Non-blocking connect + poll, so an unreachable host fails in `timeoutMs`
        // instead of after the kernel's ~75 s. Restore blocking mode afterwards —
        // the rest of Socket assumes it.
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int r = ::connect(fd, res->ai_addr, res->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            struct pollfd pfd { fd, POLLOUT, 0 };
            const int pr = ::poll(&pfd, 1, timeoutMs);
            if (pr <= 0) {
                ::close(fd); ::freeaddrinfo(res);
                throw std::runtime_error("connect() timed out to " + host + ":" + std::to_string(port));
            }
            int soErr = 0; socklen_t len = sizeof(soErr);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len);
            if (soErr != 0) {
                ::close(fd); ::freeaddrinfo(res);
                throw std::runtime_error("connect() failed to " + host + ":" + std::to_string(port));
            }
        } else if (r < 0) {
            ::close(fd); ::freeaddrinfo(res);
            throw std::runtime_error("connect() failed to " + host + ":" + std::to_string(port));
        }
        ::fcntl(fd, F_SETFL, flags);
    } else if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(fd); ::freeaddrinfo(res);
        throw std::runtime_error("connect() failed to " + host + ":" + std::to_string(port));
    }
    ::freeaddrinfo(res);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return std::make_shared<Socket>(fd);
}

} // namespace net
