/* VibeServer on Windows — the POSIX surface the shared core uses, answered with Winsock2 and Win32.
 *
 * ★★★ FORCE-INCLUDED into every Windows translation unit (vibeserver/CMakeLists.txt, if(WIN32)), C and
 *     C++ alike, before anything else. Its job is to let the ONE core compile for Windows unchanged
 *     wherever the POSIX call and the Windows call MEAN THE SAME THING. Where they do not — fork/exec,
 *     SCM_RIGHTS, signals, /proc — the call site says so itself under #ifdef _WIN32; nothing here
 *     pretends a missing feature exists.
 * ★★ Everything is static inline: no object file, no link order, nothing that a static library
 *    could drop. A C file sees the C subset; the C++ overloads (mkdir(path, mode)) are C++ only.
 * ★ A no-op off Windows, so including it anywhere is harmless.
 */
#ifndef VIBE_WIN_COMPAT_H
#define VIBE_WIN_COMPAT_H
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* ★ Winsock2 BEFORE windows.h, or windows.h drags in the old winsock.h and the two collide. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <afunix.h>
#include <iphlpapi.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ★★ Macros from the Windows headers that are ordinary words in our sources (an HTTP method enum has
 *    DELETE; a log level is ERROR). Nothing in the core uses the Windows meanings. */
#undef DELETE
#undef ERROR
#undef IGNORE
#undef ABSOLUTE
#undef RELATIVE
#undef TRANSPARENT
#undef OPAQUE
#undef interface
#undef small
#undef near
#undef far
#undef min
#undef max

#ifdef __cplusplus
extern "C++" {
#include <cstdint>
}
#endif

/* ── sockets: the BSD names ─────────────────────────────────────────────────────────────────── */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0          /* ★ Windows has no SIGPIPE: a send to a closed peer is an error return */
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0          /* ★ not a Winsock flag; every caller that passes it also polls first */
#endif
#ifndef SHUT_RD
#define SHUT_RD   SD_RECEIVE
#define SHUT_WR   SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif
#ifndef F_GETFL
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0             /* ★ CRT handles are not inherited unless asked (CreateProcess bInheritHandles) */
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif

/* ★★ The socket error as an errno value. Winsock does not set errno, so a POSIX `errno == EAGAIN`
 *    after a recv() is false on Windows whatever happened. Call sites that act on the reason ask
 *    this instead (vibe_sock_errno() in net_shim.h is the portable spelling). */
static inline int vibe_wsa_to_errno(int w) {
    switch (w) {
        case WSAEWOULDBLOCK:  return EWOULDBLOCK;
        case WSAEINPROGRESS:  return EINPROGRESS;
        case WSAEALREADY:     return EALREADY;
        case WSAEINTR:        return EINTR;
        case WSAECONNRESET:   return ECONNRESET;
        case WSAECONNABORTED: return ECONNABORTED;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAETIMEDOUT:    return ETIMEDOUT;
        case WSAENOTCONN:     return ENOTCONN;
        case WSAEADDRINUSE:   return EADDRINUSE;
        case WSAEADDRNOTAVAIL:return EADDRNOTAVAIL;
        case WSAENETUNREACH:  return ENETUNREACH;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAENOTSOCK:     return ENOTSOCK;
        case WSAEMSGSIZE:     return EMSGSIZE;
        case WSAEINVAL:       return EINVAL;
        case WSAEACCES:       return EACCES;
        case WSAEMFILE:       return EMFILE;
        case WSAENOBUFS:      return ENOBUFS;
        case WSAESHUTDOWN:    return EPIPE;
        case 0:               return 0;
        default:              return EIO;
    }
}
static inline int vibe_sock_errno_win(void) {
    int e = vibe_wsa_to_errno(WSAGetLastError());
    errno = e;
    return e;
}

/* ★★ poll() → WSAPoll. Sockets only — which is all the core polls on Windows (a pipe to a child
 *    process is read by its own thread there; see vibeserver/proc.cpp). */
static inline int poll(struct pollfd* fds, unsigned long n, int timeoutMs) {
    if (n == 0) { Sleep(timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs); return 0; }
    int r = WSAPoll(fds, n, timeoutMs);
    if (r < 0) vibe_sock_errno_win();
    return r;
}
typedef unsigned long nfds_t;

/* ★ fcntl(): only the two questions the core asks — make a socket non-blocking, and close-on-exec.
 *   Asked of a CRT file descriptor it answers 0 and changes nothing (no caller depends on it).
 *   ★ A real variadic function, not a macro: `fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK)`
 *     nests it, and a macro cannot expand inside its own arguments. */
#include <stdarg.h>
static inline int fcntl(int fd, int cmd, ...) {
    if (cmd == F_GETFL || cmd == F_GETFD) return 0;
    va_list ap; va_start(ap, cmd); const long arg = va_arg(ap, long); va_end(ap);
    if (cmd == F_SETFL) {
        u_long nb = (arg & O_NONBLOCK) ? 1 : 0;
        if (ioctlsocket((SOCKET)(intptr_t)fd, FIONBIO, &nb) != 0) {
            if (WSAGetLastError() == WSAENOTSOCK) return 0;
            vibe_sock_errno_win(); return -1;
        }
        return 0;
    }
    if (cmd == F_SETFD) {
        SetHandleInformation((HANDLE)(intptr_t)fd, HANDLE_FLAG_INHERIT, 0);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* ★★ Closing a SOCKET. close() on a socket handle does nothing useful on Windows (it is not a CRT
 *    descriptor), so a socket must go through closesocket(). vibe_close_any() is for the few places that
 *    hold "a descriptor" without knowing which kind: it tries the socket first and falls back to the CRT.
 *    ★ The two namespaces are distinct (kernel handles are multiples of 4 and sockets are never among
 *      the first handles a process holds; CRT descriptors are small integers), and this server holds a
 *      handful of CRT descriptors at most. */
static inline int vibe_closesocket(int fd) { return closesocket((SOCKET)(intptr_t)fd) == 0 ? 0 : -1; }
static inline int vibe_close_any(int fd) {
    if (fd < 0) { errno = EBADF; return -1; }
    if (closesocket((SOCKET)(intptr_t)fd) == 0) return 0;
    return _close(fd);
}

/* ── environment, time, files ───────────────────────────────────────────────────────────────── */
static inline int setenv(const char* k, const char* v, int overwrite) {
    if (!overwrite && getenv(k)) return 0;
    return _putenv_s(k, v ? v : "") == 0 ? 0 : -1;
}
static inline int unsetenv(const char* k) { return _putenv_s(k, "") == 0 ? 0 : -1; }
#ifndef timegm
#define timegm _mkgmtime
#endif
static inline char* realpath(const char* path, char* resolved) {
    char* r = _fullpath(resolved, path, resolved ? _MAX_PATH : 0);
    if (r && GetFileAttributesA(r) == INVALID_FILE_ATTRIBUTES) {
        if (!resolved) free(r);
        errno = ENOENT;
        return NULL;
    }
    return r;
}
static inline int fsync(int fd) { return _commit(fd); }
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#ifndef S_IRWXU
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#endif
#ifndef lstat
#define lstat stat
#endif
static inline int vibe_win_pipe(int fds[2]) { return _pipe(fds, 65536, _O_BINARY); }
#ifndef pipe
#define pipe(f) vibe_win_pipe(f)
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0            /* ★ no symlinks in the paths the server writes (its own folder) */
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0           /* ★ open() of a folder fails on Windows — the callers treat that as "no fsync" */
#endif
typedef long suseconds_t;
static inline int fchmod(int fd, unsigned mode) { (void)fd; (void)mode; return 0; }   /* ★ ACLs, not modes */
static inline ssize_t readlink(const char* p, char* b, size_t n) { (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
static inline int symlink(const char* a, const char* b) { (void)a; (void)b; errno = EPERM; return -1; }
static inline int link(const char* from, const char* to) {
    return CreateHardLinkA(to, from, NULL) ? 0 : (errno = EPERM, -1);
}
/* ★★★ rename() MUST REPLACE, as POSIX's does. The CRT's refuses when the target exists, so every
 *     "write .tmp, rename over the real file" in the core — the config, the ban list, the maps —
 *     would fail on its SECOND save and keep the first version for ever. MoveFileEx with
 *     REPLACE_EXISTING is the atomic-replace Windows offers; WRITE_THROUGH returns after it is on disk. */
static inline int vibe_rename(const char* from, const char* to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    DWORD e = GetLastError();
    errno = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT
          : (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) ? EACCES : EIO;
    return -1;
}
#define rename(a, b) vibe_rename((a), (b))
#ifdef __cplusplus
namespace std { using ::vibe_rename; }   /* ★ std::rename(a, b) becomes std::vibe_rename(a, b) */
#endif
/* ★ flock() on the CRT descriptor's handle — whole-file, advisory between our own processes. */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
static inline int flock(int fd, int op) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED ov; memset(&ov, 0, sizeof ov);
    if (op & LOCK_UN) return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : (errno = EINVAL, -1);
    DWORD f = ((op & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0) | ((op & LOCK_NB) ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    if (LockFileEx(h, f, 0, MAXDWORD, MAXDWORD, &ov)) return 0;
    errno = (GetLastError() == ERROR_LOCK_VIOLATION) ? EWOULDBLOCK : EIO;
    return -1;
}
/* ★ pread(): read at an offset. Only the Android usbfs presence check calls it (never reached here). */
static inline ssize_t pread(int fd, void* buf, size_t n, off_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED ov; memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)((unsigned long long)off & 0xFFFFFFFFu); ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) { errno = EIO; return -1; }
    return (ssize_t)got;
}
static inline char* vibe_stpcpy(char* d, const char* s) { size_t n = strlen(s); memcpy(d, s, n + 1); return d + n; }
#define stpcpy vibe_stpcpy
/* ★ sysconf(): the CPU count and the page size, the two the core asks. */
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#define _SC_NPROCESSORS_CONF 83
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_CLK_TCK 2
#endif
static inline long sysconf(int name) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    switch (name) {
        case _SC_NPROCESSORS_ONLN: case _SC_NPROCESSORS_CONF: return (long)si.dwNumberOfProcessors;
        case _SC_PAGESIZE: return (long)si.dwPageSize;
        case _SC_CLK_TCK: return 100;
        default: errno = EINVAL; return -1;
    }
}

#ifdef __cplusplus
/* ★ mkdir(path, mode): POSIX takes a mode; the CRT's takes none. An OVERLOAD, not a macro, so a member
 *   or a call with one argument is untouched. Permissions on Windows come from the folder's ACL. */
static inline int mkdir(const char* p, int /*mode*/) { return _mkdir(p); }
#endif

#ifdef __cplusplus
/* ★★ The BSD buffer types. Winsock declares its buffers char* and its lengths int; POSIX takes void* and
 *    size_t, and the core passes uint8_t* and int* freely. These TEMPLATES accept whatever POSIX accepts
 *    and forward. ★ Templates, not plain overloads: (char*, size_t) would be a tie between Winsock's
 *    exact pointer and an overload's exact length — ambiguous. A template deduces both exactly, so it wins
 *    that case, and when Winsock's own signature matches exactly the non-template is preferred. */
template <class P, class N> static inline int setsockopt(SOCKET s, int l, int n, const P* v, N len) {
    return ::setsockopt(s, l, n, (const char*)v, (int)len); }
template <class P> static inline int getsockopt(SOCKET s, int l, int n, P* v, int* len) {
    return ::getsockopt(s, l, n, (char*)v, len); }
template <class P, class N> static inline int send(SOCKET s, const P* b, N n, int f) {
    return ::send(s, (const char*)b, (int)n, f); }
template <class P, class N> static inline int recv(SOCKET s, P* b, N n, int f) {
    return ::recv(s, (char*)b, (int)n, f); }
template <class P, class N> static inline int sendto(SOCKET s, const P* b, N n, int f, const struct sockaddr* a, int al) {
    return ::sendto(s, (const char*)b, (int)n, f, a, al); }
template <class P, class N> static inline int recvfrom(SOCKET s, P* b, N n, int f, struct sockaddr* a, int* al) {
    return ::recvfrom(s, (char*)b, (int)n, f, a, al); }
#endif

/* ★ One call per process, before the first socket — main() and the socket-using tests make it. */
static inline void vibe_win_startup(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
}

#endif /* _WIN32 */
#endif /* VIBE_WIN_COMPAT_H */
