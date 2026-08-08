// VibeSDR V5 — clean-room, GPL-free TCP socket wrapper.
//
// Replaces the SDR++ Brown `utils/net.h` (GPLv3) the local-SDR shim used for its
// localhost HTTP/WebSocket server and the RTL-TCP client. This is original
// VibeSDR code over plain POSIX sockets (works on Android + iOS/darwin). It
// implements ONLY the TCP subset the shim calls — Socket {send/sendstr/recv/
// recvline/isOpen/close}, Listener {accept/stop}, net::listen, net::connect —
// keeping the same signatures so the shim's call sites are unchanged.
#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <unistd.h>

namespace net {

enum { NO_TIMEOUT = -1 };

// Present only so the default-argument signatures match the old API; the shim
// never passes one (TCP, address ignored).
struct Address;

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    void close();
    bool isOpen() { return open_; }

    /** ★★★ LET GO OF THE DESCRIPTOR WITHOUT KILLING THE CONNECTION.
     *
     *  close() calls shutdown(), which acts on the SOCKET — every descriptor pointing at it,
     *  including one another process now holds. After handing a connection on (SCM_RIGHTS) that is
     *  precisely wrong: the front door "closed its copy" and tore down the live TCP connection the
     *  radio had just adopted, so the listener got an empty reply and both sides looked healthy.
     *  ★ Here we drop OUR reference only. The connection lives on for as long as anyone still
     *    holds one, which is exactly the semantics a hand-off needs.
     */
    void releaseFd() { open_ = false; if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

    /** ★ The raw descriptor. Needed for two things and nothing else: peeking at a request without
     *  consuming it (MSG_PEEK), and handing the whole connection to another process
     *  (SCM_RIGHTS — see fd_passing.h). Deliberately not a general escape hatch. */
    int rawFd() const { return fd_; }

    // Peer IP address ("a.b.c.d"), or "" if unavailable. For UI/logging, bans and geo.
    // ★★★ THIS IS THE *EFFECTIVE* CLIENT ADDRESS. Behind a reverse proxy the TCP peer is the
    //     PROXY, so every listener looks like one address: bans hit everyone at once, the country
    //     flags all show the proxy's country, and one wrong admin password locks out the world.
    //     When the connection arrives from a TRUSTED proxy the real client is taken from
    //     X-Forwarded-For and set here, so the 30-odd call sites below need no changes.
    // ★★ Untrusted peers are never overridden — the header is attacker-controlled, and honouring
    //    it from anyone would let a stranger forge any address and walk straight through the ban
    //    list. Trust is opt-in, per proxy address, from the owner's config.
    std::string peerAddress();

    /** Override the reported address (see peerAddress()). Only ever called after the peer has been
     *  checked against the owner's trusted-proxy list. */
    void setEffectiveAddress(const std::string& ip) { effAddr_ = ip; }
    /** The real TCP peer, ignoring any override — what the trust check itself must use. */
    std::string socketPeerAddress();

    // Kernel socket buffer sizing. A large send buffer lets a high-rate IQ stream
    // ride out brief WiFi radio stalls before the userspace queue starts dropping;
    // a large receive buffer does the same on the client side. The kernel may clamp
    // the request — these return false on outright failure, never fatal.
    bool setSendBufferSize(int bytes);
    bool setRecvBufferSize(int bytes);

    // Send all `len` bytes; returns len on success, -1 on error.
    int send(const uint8_t* data, size_t len, const Address* dest = nullptr);
    int sendstr(const std::string& str, const Address* dest = nullptr);

    // Receive up to `maxLen` bytes. forceLen=true blocks until exactly maxLen are
    // read (or error/timeout). `timeout` is milliseconds, or NO_TIMEOUT to block.
    // Returns bytes read (>0), 0 on close/timeout-with-nothing, -1 on error.
    int recv(uint8_t* data, size_t maxLen, bool forceLen = false,
             int timeout = NO_TIMEOUT, Address* dest = nullptr);

    // Read one '\n'-terminated line into `str` (newline stripped, any trailing
    // '\r' kept). Returns line length (0 for a blank line), -1 on error/close.
    int recvline(std::string& str, int maxLen = 0, int timeout = NO_TIMEOUT,
                 Address* dest = nullptr);

private:
    int  recvRaw(uint8_t* data, size_t maxLen, int timeout);
    int  fd_;
    bool open_ = true;
    std::string effAddr_;   // set only for connections arriving via a trusted proxy
};

class Listener {
public:
    explicit Listener(int fd) : fd_(fd) {}
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Accept a connection, waiting up to `timeout` ms (NO_TIMEOUT = block).
    // Returns the new Socket, or nullptr on timeout / would-block / closed.
    std::shared_ptr<Socket> accept(Address* dest = nullptr, int timeout = NO_TIMEOUT);
    void stop();
    bool listening() { return open_; }

private:
    int  fd_;
    bool open_ = true;
};

// Bind + listen on host:port. Throws std::runtime_error on failure.
std::shared_ptr<Listener> listen(const std::string& host, int port);
// Connect to host:port. Throws std::runtime_error on failure.
// `timeoutMs` > 0 bounds the TCP handshake. The default blocking ::connect() waits
// for the OS timeout (~75 s on an unreachable host), which is far too long to hold
// a UI — and, worse, to hold a lifecycle lock behind.
std::shared_ptr<Socket> connect(const std::string& host, int port, int timeoutMs = 0);

} // namespace net
