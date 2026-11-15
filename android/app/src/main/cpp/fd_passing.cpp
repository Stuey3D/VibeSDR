#include "fd_passing.h"

#include <cstring>
#include <cerrno>

#ifdef _WIN32
/* ★★★ WINDOWS: THE SAME HAND-OFF WITHOUT SCM_RIGHTS (docs/WINDOWS-PORT-PLAN.md §4.2).
 *  Windows cannot carry a descriptor between processes. What it has instead is WSADuplicateSocketW: the
 *  OWNER of a socket fills a WSAPROTOCOL_INFOW for one named target process, and that process — and only
 *  it — turns the blob back into a live socket with WSASocketW. The TCP connection, its unread bytes (the
 *  request the front door MSG_PEEK'd) and its state carry over, exactly as with SCM_RIGHTS.
 *  ★★ The channel is a NAMED PIPE, not AF_UNIX: pipes exist on every Windows the server runs on (AF_UNIX
 *     only from 10 1803), are what Windows' own services use for this, and can be tested under Wine,
 *     which has no AF_UNIX. `path` (the POSIX socket path) names the pipe, so callers are unchanged.
 *  ★★ The conversation gains one step — the RECEIVER speaks first, because the sender cannot duplicate
 *     for a process it cannot name:
 *        receiver → sender   4 bytes  its pid (little-endian)
 *        sender   → receiver sizeof(WSAPROTOCOL_INFOW) bytes
 *        receiver → sender   'K' once the socket exists — only then may the sender drop its copy
 *  ★★ LOCAL ONLY AND NOT SQUATTABLE: PIPE_REJECT_REMOTE_CLIENTS (a pipe is otherwise reachable over SMB),
 *     and FILE_FLAG_FIRST_PIPE_INSTANCE, so a process that created the name first makes fdListen FAIL
 *     rather than receive our listeners' connections.
 *  ★ Every wait is bounded (2 s), as on POSIX: a dead peer must never hang the front door. */
#include <map>
#include <mutex>

namespace vibe {
namespace {

std::wstring pipeName(const std::string& path) {
    std::wstring n = L"\\\\.\\pipe\\vibeserver-";
    for (unsigned char c : path) n += (wchar_t)(isalnum(c) || c == '.' || c == '-' ? c : '_');
    return n;
}
std::string winErr(const char* what) {
    return std::string(what) + ": Windows error " + std::to_string(GetLastError());
}

/** One overlapped read or write of exactly n bytes, or false after `ms`. */
bool pipeIo(HANDLE h, bool write, void* p, DWORD n, DWORD ms) {
    char* b = (char*)p;
    while (n > 0) {
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD got = 0;
        BOOL ok = write ? WriteFile(h, b, n, &got, &ov) : ReadFile(h, b, n, &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0) { CancelIoEx(h, &ov); GetOverlappedResult(h, &ov, &got, TRUE); CloseHandle(ov.hEvent); return false; }
            ok = GetOverlappedResult(h, &ov, &got, FALSE);
        }
        CloseHandle(ov.hEvent);
        if (!ok || got == 0) return false;
        b += got; n -= got;
    }
    return true;
}

/** A listening pipe: the instance waiting for the next sender. */
struct PipeListener {
    std::wstring name;
    HANDLE       inst = INVALID_HANDLE_VALUE;
    OVERLAPPED   ov{};
    bool         pending = false;   // ConnectNamedPipe is in flight on `inst`
};
std::mutex g_mtx;
std::map<int, PipeListener*> g_listeners;
int g_nextId = 0x7F000000;          // ★ far from any socket handle value, so the two can never be confused

HANDLE newInstance(const std::wstring& name, bool first) {
    return CreateNamedPipeW(name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
}
bool arm(PipeListener* L) {
    if (L->ov.hEvent == nullptr) L->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(L->ov.hEvent);
    if (ConnectNamedPipe(L->inst, &L->ov)) { SetEvent(L->ov.hEvent); L->pending = false; return true; }
    const DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) { L->pending = true; return true; }
    if (e == ERROR_PIPE_CONNECTED) { SetEvent(L->ov.hEvent); L->pending = false; return true; }
    return false;
}

}  // namespace

bool sendFdTo(const std::string& path, int fd, std::string& err) {
    const std::wstring name = pipeName(path);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 2 && h == INVALID_HANDLE_VALUE; ++i) {
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), 2000)) break;
        }
    }
    if (h == INVALID_HANDLE_VALUE) { err = winErr(("connect " + path).c_str()); return false; }
    bool ok = false;
    unsigned char pidb[4];
    if (!pipeIo(h, false, pidb, 4, 2000)) {
        err = "the radio did not say who it is (no pid on " + path + ")";
    } else {
        const DWORD pid = (DWORD)pidb[0] | ((DWORD)pidb[1] << 8) | ((DWORD)pidb[2] << 16) | ((DWORD)pidb[3] << 24);
        WSAPROTOCOL_INFOW info;
        if (WSADuplicateSocketW((SOCKET)(intptr_t)fd, pid, &info) != 0) {
            err = "WSADuplicateSocketW: Winsock error " + std::to_string(WSAGetLastError());
        } else if (!pipeIo(h, true, &info, (DWORD)sizeof info, 2000)) {
            err = winErr("send(protocol info)");
        } else {
            // ★★★ WAIT FOR THE FAR SIDE TO TAKE IT BEFORE CLOSING — the duplicate exists only once the
            //     receiver has called WSASocketW; dropping our copy sooner would end the connection.
            char ack = 0;
            ok = pipeIo(h, false, &ack, 1, 2000) && ack == 'K';
            if (!ok) err = "the radio did not confirm it took the connection";
        }
    }
    CloseHandle(h);
    return ok;
}

int fdListen(const std::string& path, std::string& err) {
    auto* L = new PipeListener();
    L->name = pipeName(path);
    L->inst = newInstance(L->name, true);
    if (L->inst == INVALID_HANDLE_VALUE) {
        err = winErr(("CreateNamedPipe " + path + " (is another copy of this radio running?)").c_str());
        delete L; return -1;
    }
    if (!arm(L)) { err = winErr("ConnectNamedPipe"); CloseHandle(L->inst); delete L; return -1; }
    std::lock_guard<std::mutex> lk(g_mtx);
    const int id = ++g_nextId;
    g_listeners[id] = L;
    return id;
}

int fdAccept(int listenFd, int timeoutMs) {
    PipeListener* L = nullptr;
    { std::lock_guard<std::mutex> lk(g_mtx); auto it = g_listeners.find(listenFd); if (it != g_listeners.end()) L = it->second; }
    if (!L) return -1;
    if (WaitForSingleObject(L->ov.hEvent, timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs) != WAIT_OBJECT_0) return -1;
    if (L->pending) { DWORD x = 0; if (!GetOverlappedResult(L->inst, &L->ov, &x, FALSE)) { /* a sender that gave up */ } }
    HANDLE c = L->inst;
    // ★ The next sender gets a fresh instance at once, before this one's conversation even starts.
    L->inst = newInstance(L->name, false);
    if (L->inst != INVALID_HANDLE_VALUE) arm(L); else ResetEvent(L->ov.hEvent);
    int got = -1;
    const DWORD pid = GetCurrentProcessId();
    unsigned char pidb[4] = { (unsigned char)pid, (unsigned char)(pid >> 8), (unsigned char)(pid >> 16), (unsigned char)(pid >> 24) };
    WSAPROTOCOL_INFOW info;
    if (pipeIo(c, true, pidb, 4, 2000) && pipeIo(c, false, &info, (DWORD)sizeof info, 2000)) {
        SOCKET ns = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
        if (ns != INVALID_SOCKET) {
            got = (int)ns;
            char ack = 'K';
            (void)pipeIo(c, true, &ack, 1, 2000);
            FlushFileBuffers(c);
        }
    }
    DisconnectNamedPipe(c);
    CloseHandle(c);
    return got;
}

void fdListenClose(int listenFd) {
    PipeListener* L = nullptr;
    { std::lock_guard<std::mutex> lk(g_mtx); auto it = g_listeners.find(listenFd); if (it != g_listeners.end()) { L = it->second; g_listeners.erase(it); } }
    if (!L) return;
    if (L->inst != INVALID_HANDLE_VALUE) { CancelIoEx(L->inst, &L->ov); CloseHandle(L->inst); }
    if (L->ov.hEvent) CloseHandle(L->ov.hEvent);
    delete L;
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

void fdListenClose(int listenFd) { if (listenFd >= 0) ::close(listenFd); }

}  // namespace vibe

#endif  // _WIN32
