// fd_passing.h — hand a live TCP connection to another process, over a unix socket.
//
// ★★★ WHY, AND WHY NOT A PROXY. Several radios on one machine means several processes, and an
//     owner should have to forward ONE port from their router, not one per radio. The obvious
//     answer is to proxy: accept on the public port, open a second connection to the right radio,
//     and copy bytes between them. That would work, and it would put every listener's audio and
//     waterfall through a single process.
//
//     This codebase has been bitten TWICE by exactly that shape — a blocking broadcast under a
//     global mutex, and one slow listener freezing all twenty. Passing the DESCRIPTOR instead
//     removes the failure mode rather than mitigating it: after the hand-off the listener talks
//     straight to the radio process, the front door goes back to idle, and a wedged radio cannot
//     stall anyone else.
//
// ★★ THE FRONT NEVER CONSUMES THE REQUEST. It PEEKs (MSG_PEEK) just far enough to read the
//    request line and decide who it belongs to, then passes the socket on untouched — so the
//    receiving process reads a completely ordinary HTTP request from byte zero. The alternative,
//    reading the line and replaying it on the far side, means carrying a "bytes already consumed"
//    buffer through every read path, and the first thing to get it wrong would corrupt a
//    WebSocket handshake in a way that looks like a client bug.
#pragma once
#include <string>

namespace vibe {

/** Send an open socket to another process. `path` is a unix socket it is listening on.
 *  Returns false (with `err` set) if the peer is not there — which is normal: it means that
 *  radio's process is not running, and the caller answers for itself instead. */
bool sendFdTo(const std::string& path, int fd, std::string& err);

/** Listen for descriptors. Returns a listening unix socket fd, or -1 with `err` set.
 *  ★ Removes a stale socket file first: a process killed with SIGKILL leaves one behind, and
 *    bind() then fails for ever with "address in use" on something that nothing is using. */
int fdListen(const std::string& path, std::string& err);

/** Wait for one descriptor on `listenFd`. Returns the received fd, or -1 on error/timeout. */
int fdAccept(int listenFd, int timeoutMs);

}  // namespace vibe
