#include "fd_passing.h"

#include <cstring>
#include <cerrno>

#ifdef _WIN32
/* ★★★ WINDOWS: THE SAME HAND-OFF WITHOUT SCM_RIGHTS (docs/WINDOWS-PORT-PLAN.md §4.2).
 *  Windows has AF_UNIX (10 1803+) but cannot carry a descriptor over it. What it has instead is
 *  WSADuplicateSocketW: the OWNER of a socket fills a WSAPROTOCOL_INFOW for one named target process,
 *  and that process — and only it — turns the blob back into a live socket with WSASocketW. The TCP
 *  connection, its unread bytes (the request the front door MSG_PEEK'd) and its state carry over, as
 *  with SCM_RIGHTS.
 *  ★★ So the conversation gains one step: the RECEIVER speaks first, with its process id, because the
 *     sender cannot duplicate for a process it cannot name.
 *        receiver → sender   4 bytes  its pid (little-endian DWORD)
 *        sender   → receiver sizeof(WSAPROTOCOL_INFOW) bytes
 *        receiver → sender   'K' once the socket exists — only then may the sender drop its copy
 *  ★ The same three functions and the same meaning, so the front door and the radios call them
 *    unchanged. The socket file lives where the POSIX one does; a stale one is removed before bind. */
#include <afunix.h>

namespace vibe {
namespace {

bool fillAddr(sockaddr_un& a, const std::string& path, std::string& err) {
    if (path.size() >= sizeof(a.sun_path)) { err = "socket path too long: " + path; return false; }
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, path.c_str(), path.size());
    return true;
}
std::string wsaErr(const char* what) {
    return std::string(what) + ": Winsock error " + std::to_string(WSAGetLastError());
}
/** Exactly n bytes, or false — bounded by `ms` per wait so a dead peer cannot hang the front door. */
bool readAll(SOCKET s, char* p, int n, int ms) {
    while (n > 0) {
        WSAPOLLFD pf{}; pf.fd = s; pf.events = POLLRDNORM;
        if (WSAPoll(&pf, 1, ms) <= 0) return false;
        const int r = ::recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}
bool writeAll(SOCKET s, const char* p, int n) {
    while (n > 0) {
        const int r = ::send(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

}  // namespace

bool sendFdTo(const std::string& path, int fd, std::string& err) {
    sockaddr_un addr;
    if (!fillAddr(addr, path, err)) return false;
    SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { err = wsaErr("socket(AF_UNIX)"); return false; }
    if (::connect(s, (sockaddr*)&addr, sizeof addr) != 0) {
        err = wsaErr(("connect " + path).c_str()); closesocket(s); return false;
    }
    bool ok = false;
    unsigned char pidb[4];
    if (!readAll(s, (char*)pidb, 4, 2000)) {
        err = "the radio did not say who it is (no pid from " + path + ")";
    } else {
        const DWORD pid = (DWORD)pidb[0] | ((DWORD)pidb[1] << 8) | ((DWORD)pidb[2] << 16) | ((DWORD)pidb[3] << 24);
        WSAPROTOCOL_INFOW info;
        if (WSADuplicateSocketW((SOCKET)(intptr_t)fd, pid, &info) != 0) {
            err = wsaErr("WSADuplicateSocketW");
        } else if (!writeAll(s, (const char*)&info, (int)sizeof info)) {
            err = wsaErr("send(protocol info)");
        } else {
            // ★★★ WAIT FOR THE FAR SIDE TO TAKE IT BEFORE CLOSING — as on POSIX, and more so here: the
            //     duplicate exists only once the receiver has called WSASocketW.
            char ack = 0;
            ok = readAll(s, &ack, 1, 2000) && ack == 'K';
            if (!ok) err = "the radio did not confirm it took the connection";
        }
    }
    closesocket(s);
    return ok;
}

int fdListen(const std::string& path, std::string& err) {
    sockaddr_un addr;
    if (!fillAddr(addr, path, err)) return -1;
    ::DeleteFileA(path.c_str());   // ★ a killed process leaves its socket file behind
    SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { err = wsaErr("socket(AF_UNIX)"); return -1; }
    if (::bind(s, (sockaddr*)&addr, sizeof addr) != 0) {
        err = wsaErr(("bind " + path).c_str()); closesocket(s); return -1;
    }
    if (::listen(s, 16) != 0) { err = wsaErr("listen"); closesocket(s); return -1; }
    return (int)s;
}

int fdAccept(int listenFd, int timeoutMs) {
    WSAPOLLFD p{}; p.fd = (SOCKET)(intptr_t)listenFd; p.events = POLLRDNORM;
    if (WSAPoll(&p, 1, timeoutMs) <= 0) return -1;
    SOCKET c = ::accept((SOCKET)(intptr_t)listenFd, nullptr, nullptr);
    if (c == INVALID_SOCKET) return -1;
    int got = -1;
    const DWORD pid = GetCurrentProcessId();
    const unsigned char pidb[4] = { (unsigned char)pid, (unsigned char)(pid >> 8),
                                    (unsigned char)(pid >> 16), (unsigned char)(pid >> 24) };
    WSAPROTOCOL_INFOW info;
    if (writeAll(c, (const char*)pidb, 4) && readAll(c, (char*)&info, (int)sizeof info, 2000)) {
        SOCKET ns = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                               WSA_FLAG_OVERLAPPED);
        if (ns != INVALID_SOCKET) {
            got = (int)ns;
            const char ack = 'K';
            (void)writeAll(c, &ack, 1);
        }
    }
    closesocket(c);
    return got;
}

}  // namespace vibe

#else  // ── POSIX: SCM_RIGHTS ───────────────────────────────────────────────────────────────────
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace vibe {
namespace {

/** ★ sun_path is a FIXED 108 bytes and is NOT null-terminated for you. Silently truncating a long
 *  path produces a socket at an address neither side agrees on, and the symptom is "the other
 *  process is never there" — so refuse instead. */
bool fillAddr(sockaddr_un& a, const std::string& path, std::string& err) {
    if (path.size() >= sizeof(a.sun_path)) {
        err = "socket path too long: " + path;
        return false;
    }
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, path.c_str(), path.size());
    return true;
}

}  // namespace

bool sendFdTo(const std::string& path, int fd, std::string& err) {
    sockaddr_un addr;
    if (!fillAddr(addr, path, err)) return false;

    const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { err = std::string("socket: ") + strerror(errno); return false; }

    if (::connect(s, (sockaddr*)&addr, sizeof addr) != 0) {
        err = std::string("connect ") + path + ": " + strerror(errno);
        ::close(s);
        return false;
    }

    // ★★ A DESCRIPTOR CANNOT TRAVEL ALONE. sendmsg() with SCM_RIGHTS requires at least one byte of
    //    ordinary data or the ancillary data is silently dropped on some kernels — the fd simply
    //    never arrives and neither side reports an error. One byte, and it is never read for its
    //    value.
    char byte = 'F';
    iovec iov{};
    iov.iov_base = &byte;
    iov.iov_len  = 1;

    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof control);

    msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof control;

    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof fd);

    ssize_t n;
    do { n = ::sendmsg(s, &msg, 0); } while (n < 0 && errno == EINTR);
    const bool ok = (n == 1);
    if (!ok) err = std::string("sendmsg: ") + strerror(errno);

    // ★★★ WAIT FOR THE FAR SIDE TO TAKE IT BEFORE CLOSING. Closing this unix socket immediately
    //     can destroy the message still in flight, and the connection is then lost with no error
    //     anywhere — the listener's browser simply hangs. One byte back means "I have it".
    if (ok) {
        pollfd p{ s, POLLIN, 0 };
        if (::poll(&p, 1, 2000) > 0) { char ack; (void)::recv(s, &ack, 1, 0); }
    }
    ::close(s);
    return ok;
}

int fdListen(const std::string& path, std::string& err) {
    sockaddr_un addr;
    if (!fillAddr(addr, path, err)) return -1;

    // ★ A process killed with SIGKILL leaves its socket file behind, and bind() then fails for
    //   ever with EADDRINUSE on something nothing is using. Unlink first — this path is ours by
    //   construction (it is named after our own radio).
    ::unlink(path.c_str());

    const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { err = std::string("socket: ") + strerror(errno); return -1; }
    if (::bind(s, (sockaddr*)&addr, sizeof addr) != 0) {
        err = std::string("bind ") + path + ": " + strerror(errno);
        ::close(s); return -1;
    }
    // ★ 0600 would be wrong and 0666 would be dangerous: every VibeServer process runs as the SAME
    //   service user, so owner-only is exactly right and keeps other local users out of a channel
    //   that hands over live client connections.
    ::chmod(path.c_str(), 0600);
    if (::listen(s, 16) != 0) {
        err = std::string("listen: ") + strerror(errno);
        ::close(s); return -1;
    }
    return s;
}

int fdAccept(int listenFd, int timeoutMs) {
    pollfd p{ listenFd, POLLIN, 0 };
    const int r = ::poll(&p, 1, timeoutMs);
    if (r <= 0) return -1;

    const int c = ::accept(listenFd, nullptr, nullptr);
    if (c < 0) return -1;

    char byte = 0;
    iovec iov{};
    iov.iov_base = &byte;
    iov.iov_len  = 1;

    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof control);

    msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof control;

    ssize_t n;
    do { n = ::recvmsg(c, &msg, 0); } while (n < 0 && errno == EINTR);

    int got = -1;
    if (n == 1) {
        for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                memcpy(&got, CMSG_DATA(cm), sizeof got);
                break;
            }
        }
    }
    // Acknowledge, so the sender knows the descriptor landed before it drops its own copy.
    if (got >= 0) { const char ack = 'K'; (void)::send(c, &ack, 1, 0); }
    ::close(c);
    return got;
}

}  // namespace vibe

#endif  // _WIN32
