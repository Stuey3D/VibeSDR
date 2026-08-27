// VibeSDR V5 — clean-room, GPL-free TCP socket wrapper.
//
// Replaces the SDR++ Brown `utils/net.h` (GPLv3) the local-SDR shim used for its
// localhost HTTP/WebSocket server and the RTL-TCP client. This is original
// VibeSDR code over plain POSIX sockets (works on Android + iOS/darwin). It
// implements ONLY the TCP subset the shim calls — Socket {send/sendstr/recv/
// recvline/isOpen/close}, Listener {accept/stop}, net::listen, net::connect —
// keeping the same signatures so the shim's call sites are unchanged.
#pragma once
#include <atomic>
#include <chrono>
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
    explicit Socket(int fd) : fd_(fd) {
        // ★ Stamped at construction so ageSecs() is the socket's WHOLE life, including the time
        //   before it was promoted to a websocket — a connection that never got that far is
        //   exactly the case worth being able to see.
        openedAt_ = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
    }
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    void close();
    /** ★★★ TELL THEM, THEN HANG UP — AND ACTUALLY TELL THEM. close() does shutdown(SHUT_RDWR) and
     *  closes at once, which ABORTS the connection: the frame just handed to the kernel is
     *  discarded, and any unread inbound data turns the close into an RST that destroys it for
     *  certain. Every "explain why, then close" path was therefore explaining to nobody —
     *  `evicted`, `busy` and `session_expired` alike — so a listener displaced by an admin saw the
     *  generic "someone else is listening" card instead of being told they had been taken over
     *  (Stuart, 2026-08-15, in both directions between the app and the browser).
     *  ★★ Half-close the WRITE side first: that flushes what is queued and sends FIN after it.
     *     Then drain briefly, because it is UNREAD INBOUND data that turns a close into a reset.
     *  ★ Bounded hard (a few ms, a few KB). This runs on a server thread holding a lock; it is
     *    long enough for a frame already in the buffer, not a wait for the peer to do anything. */
    void closeAfterFlush();
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

    /* ★★★ WHAT THIS SOCKET ACTUALLY DELIVERED, and how long it lived.
     *
     * The server counted bytes only SERVER-WIDE (vsSpecBytes / vsAudioBytes), which cannot answer
     * the question anyone actually asks about a visitor: did THEY get anything? A nine-second
     * connection is completely ordinary if 40 KB of waterfall went down it, and a fault if zero
     * did — and until now those two logged identically.
     *
     * ★★ send() is the ONE choke point every byte passes through, whatever the socket carries, so
     * counting here needs no bookkeeping at the call sites and cannot miss a path someone adds
     * later. Relaxed: it is a diagnostic total, never read for control flow.
     *
     * ★ Counting AUDIO and SPECTRUM separately is the point rather than a nicety. A listener who
     * minimises the page does not close the spectrum socket — it stays open and simply stops
     * receiving frames — so "audio 47 MB, spectrum 3 KB over the same 90 minutes" is a
     * fingerprint that duration alone cannot show (Stuart, 2026-08-27). Each socket carries one
     * kind, so per-socket totals give that for free. */
    uint64_t txBytes()   const { return tx_.load(std::memory_order_relaxed); }
    double   ageSecs()   const;

    /* ★★★ WHY A CONNECTION ENDED — the one thing the disconnect line never said.
     *
     *   "spectrum WS disconnected — 1s, 18 KB delivered" is the same sentence whether the peer
     *   hung up, the network broke mid-send, or we dropped them ourselves. On 2026-08-27 a
     *   spectrum socket flapped six times in twenty-five seconds while the audio socket beside it
     *   ran for twelve minutes, and the log could not say which end let go — so the question
     *   "is it us or the tunnel?" stayed open, as it has before (see the tunnel-drops note).
     *
     * ★★ ATTRIBUTION IS THE WHOLE POINT. A server that cannot say who closed a connection cannot
     *    be cleared of closing it, and every future report of this shape starts from zero again.
     * ★ Set at the moment it happens, read at teardown. Defaults to "peer" because that is what an
     *   ordinary, healthy disconnect is: the reader hit EOF and unwound. */
    enum class Why : uint8_t { Peer, SendFailed, Local, Timeout };
    void     noteWhy(Why w) { why_ = w; }
    Why      why() const { return why_; }
    const char* whyText() const {
        switch (why_) {
            case Why::SendFailed: return "send failed";
            case Why::Local:      return "closed by server";
            case Why::Timeout:    return "timed out";
            default:              return "closed by peer";
        }
    }

private:
    int  recvRaw(uint8_t* data, size_t maxLen, int timeout);
    std::atomic<uint64_t> tx_{0};
    Why why_ = Why::Peer;
    double openedAt_ = 0.0;
    int  fd_;
    bool open_ = true;
    std::string effAddr_;   // set only for connections arriving via a trusted proxy
    /** The peer's address, asked for ONCE. getpeername() fails after the peer disconnects, so a
     *  short-lived connection would otherwise be recorded with no address at all. */
    std::string cachedAddr_;
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
