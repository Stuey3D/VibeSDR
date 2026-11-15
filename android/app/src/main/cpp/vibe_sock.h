// vibe_sock.h — the four places a socket is NOT a file descriptor, spelled once for every platform.
//
// ★★★ WHY THIS EXISTS (the Windows port, 2026-10-26). On POSIX a socket is a file descriptor; on Windows
//     it is a SOCKET handle, and four things the core does to sockets mean something different there:
//       · close()   — a CRT call that does not know the handle: the connection would stay open for ever
//                     (and a socket handed to a radio would never be released by the front door).
//       · errno     — Winsock never sets it. `errno == EINTR` after a failed recv() is a stale value.
//       · EINPROGRESS — a non-blocking connect() on Windows reports WSAEWOULDBLOCK instead.
//       · SO_REUSEADDR — on Windows it lets a SECOND process bind a port that is already listening, so the
//                     port scans (48000-48049, 50001-50100) would never see "in use" and two servers
//                     would share 48000. SO_EXCLUSIVEADDRUSE is the Windows meaning of the POSIX flag.
// ★ Everything else (send, recv, poll, setsockopt, shutdown) has the same meaning and stays as written.
// ★ Header-only; Android, iOS, macOS and Linux compile exactly what they compiled before.
#pragma once
#include <cerrno>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vibesock {

/** Close a SOCKET (never a file). */
inline int close(int fd) {
#ifdef _WIN32
    return ::closesocket((SOCKET)(intptr_t)fd) == 0 ? 0 : -1;
#else
    return ::close(fd);
#endif
}

/** The last socket error, as an errno value (and errno is set to it, so strerror(errno) reads right). */
inline int lastError() {
#ifdef _WIN32
    return vibe_sock_errno_win();
#else
    return errno;
#endif
}

/** A non-blocking connect() that has started rather than failed. */
inline bool connectPending(int e) {
    return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN;
}

/** Make a LISTENING socket's bind behave as POSIX SO_REUSEADDR does: rebind straight after a restart
 *  (TIME_WAIT) — and still fail with EADDRINUSE while another process is listening there. */
inline void reuseListenAddr(int fd) {
    int one = 1;
#ifdef _WIN32
    // ★ Windows binds over TIME_WAIT without being asked; EXCLUSIVEADDRUSE refuses a live listener.
    ::setsockopt((SOCKET)(intptr_t)fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
#else
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
}

}  // namespace vibesock
