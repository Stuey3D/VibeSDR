// VibeServer — the GPU map's files, served FROM DISK at /mapgl/<path>.
//
// The listener's BROWSER renders the map (MapLibre GL, vector tiles in PMTiles archives); this
// server only hands it the files. Nothing comes from a CDN: a receiver on a LAN with no outbound
// internet must still draw its admin map and its digital-spots map. See
// briefs/BRIEF-server-gpu-maps.md — §1 is the URL contract this file implements, §3 its API.
//
// ★★★ TWO DIRECTORIES, AND THEY ARE DIFFERENT PROMISES.
//     The BUNDLE dir (read-only) is what the package installed: the renderer, the style, the
//     glyphs, the icons, and the basic + relief packs. It is always complete or absent.
//     The DATA dir (writable) holds ONE optional file — vibemap-detail.pmtiles, ~169 MB — which an
//     owner downloads from the admin page, and which can be removed again. So it is `no-cache`,
//     while the bundle is cached for a day.
//
// ★★★ HTTP RANGE IS THE WHOLE POINT. A PMTiles archive is one file the browser reads a few KB at a
//     time — the header, a directory, the tiles in view. Without 206 the browser would pull all
//     169 MB to draw one country. So every file here answers a single `bytes=` range; anything we
//     cannot honour honestly (several ranges, a unit that is not bytes, a malformed header) is
//     IGNORED and the whole file is sent, which is what RFC 9110 says a server may do. An offset
//     past the end is 416, never a silent full body.
//
// ★★ HALF A DOWNLOAD IS NEVER "INSTALLED". The detail pack is written to `.part`, then checked for
//    the exact size and the 7-byte "PMTiles" magic, and only THEN renamed into place. rename() is
//    atomic, so a reader sees either no detail pack or a whole one.
//
// ★★ SEVERAL PROCESSES, ONE FILE. On a multi-radio Linux machine every radio is its own process,
//    and the admin page may poll any of them. So "someone is downloading" is an flock on
//    `vibemap-detail.pmtiles.lock` (which also stops two processes downloading at once), progress
//    is the size of `.part`, and the last failure is a small `.error` file — all visible to every
//    process, not just the one that was asked.
//
// ★ Path safety is decided on the DECODED path, before anything is joined to a directory:
//   no "..", no backslash, no NUL, no leading '/', no dot-files, and only the subtrees in §1.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>   // _NSGetExecutablePath — macOS's /proc/self/exe
#endif

#ifndef VIBE_MAPGL_NO_NET
#include "net_shim.h"
#include "vibe_bulk_pace.h"
#endif

namespace vibemapgl {

// ★ One release asset, shared with the app (src/services/mapglDetail.ts). Change both or neither.
inline constexpr const char* DETAIL_URL =
    "https://github.com/Stuey3D/VibeSDR/releases/download/mapgl-detail-v1/vibemap-detail.pmtiles";
inline constexpr int64_t DETAIL_BYTES = 177024426;
inline constexpr const char* DETAIL_NAME = "vibemap-detail.pmtiles";

using Downloader = std::function<bool(const std::string&, const std::string&,
                                      std::function<void(int64_t, int64_t)>)>;

// ── state ──────────────────────────────────────────────────────────────────────────────────────
struct State {
    std::mutex           mtx;
    std::string          bundleDir;       // explicit; empty = search (see bundleDir())
    bool                 bundleSearched = false;
    std::string          bundleFound;
    std::string          dataDir;
    Downloader           downloader;
    std::string          error;           // this process's last failure
    std::atomic<bool>    downloading{false};
    std::atomic<int64_t> written{0};
    std::atomic<int64_t> total{0};
};
inline State& st() { static State s; return s; }

inline bool isDir(const std::string& p) {
    struct stat sb{};
    return !p.empty() && ::stat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}
inline bool isFile(const std::string& p, int64_t* size = nullptr) {
    struct stat sb{};
    if (p.empty() || ::stat(p.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
    if (size) *size = (int64_t)sb.st_size;
    return true;
}

inline void setBundleDir(const std::string& d) {
    std::lock_guard<std::mutex> lk(st().mtx);
    st().bundleDir = d;
}
inline void setDataDir(const std::string& d) {
    std::lock_guard<std::mutex> lk(st().mtx);
    st().dataDir = d;
}
inline void setDownloader(Downloader fn) {
    std::lock_guard<std::mutex> lk(st().mtx);
    st().downloader = std::move(fn);
}
inline std::string dataDir() {
    std::lock_guard<std::mutex> lk(st().mtx);
    return st().dataDir;
}

inline std::string exeDir() {
    char buf[4096];
#if defined(__APPLE__)
    uint32_t n = sizeof(buf);
    if (_NSGetExecutablePath(buf, &n) != 0) return {};
    std::string exe(buf);
#else
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    std::string exe(buf);
#endif
    const size_t slash = exe.rfind('/');
    return slash == std::string::npos ? std::string() : exe.substr(0, slash);
}

/** The read-only bundle. An explicit setBundleDir() wins; otherwise VIBESERVER_MAPGL_DIR, then
 *  relative to the binary (the .deb's $exe/../lib/vibeserver/mapgl, the Mac app's
 *  Contents/Resources/mapgl), then the fixed Linux locations — the same order vibe_mapdata.h uses,
 *  and resolved ONCE because it is on the request path. */
inline std::string bundleDir() {
    std::lock_guard<std::mutex> lk(st().mtx);
    if (!st().bundleDir.empty()) return st().bundleDir;
    if (st().bundleSearched) return st().bundleFound;
    st().bundleSearched = true;
    if (const char* e = getenv("VIBESERVER_MAPGL_DIR"); e && *e && isDir(e)) {
        st().bundleFound = e;
        return st().bundleFound;
    }
    const std::string ed = exeDir();
    const std::string candidates[] = {
        ed.empty() ? std::string() : ed + "/../lib/vibeserver/mapgl",
        ed.empty() ? std::string() : ed + "/../Resources/mapgl",     // macOS .app
        ed.empty() ? std::string() : ed + "/mapgl",
        "/usr/lib/vibeserver/mapgl",                                  // ★ also the armv6 binary's
        "/usr/share/vibeserver/mapgl",
    };
    for (const std::string& c : candidates)
        if (isDir(c)) { st().bundleFound = c; return c; }
    return {};
}

// ── path safety ────────────────────────────────────────────────────────────────────────────────
inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/** %XX-decode. False on a malformed escape. ('+' is NOT a space in a path.) */
inline bool urlDecode(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') { out += in[i]; continue; }
        if (i + 2 >= in.size()) return false;
        const int h = hexVal(in[i + 1]), l = hexVal(in[i + 2]);
        if (h < 0 || l < 0) return false;
        out += (char)(h * 16 + l);
        i += 2;
    }
    return true;
}
inline bool endsWith(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
/** A single path segment made of the characters our files use. `spaces` for font names. */
inline bool segOk(const std::string& s, bool spaces) {
    if (s.empty() || s.size() > 128 || s[0] == '.') return false;   // "..", ".", dot-files
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
                        (spaces && c == ' ');
        if (!ok) return false;
    }
    return s.find("..") == std::string::npos;
}

enum class Where { None, Bundle, Data };

/** Map the part of the URL after "/mapgl/" (still percent-encoded, query already stripped) to a
 *  RELATIVE file path and the directory it lives in. Where::None = refuse (404). */
inline Where resolve(const std::string& encoded, std::string& rel) {
    rel.clear();
    std::string p;
    if (encoded.empty() || encoded.size() > 512 || !urlDecode(encoded, p)) return Where::None;
    // ★ Refused outright, before any shape test: these are the traversal vocabulary.
    if (p.empty() || p[0] == '/' || p.find('\\') != std::string::npos ||
        p.find('\0') != std::string::npos || p.find("..") != std::string::npos)
        return Where::None;
    std::vector<std::string> seg;
    size_t a = 0;
    for (;;) {
        const size_t b = p.find('/', a);
        seg.push_back(p.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    if (seg.size() == 1) {
        const std::string& f = seg[0];
        if (f == DETAIL_NAME) { rel = f; return Where::Data; }
        if (f == "vibemap-basic.pmtiles" || f == "vibemap-relief.pmtiles" || f == "vibemap-runways.pmtiles" ||
            f == "vibemap-style.json") { rel = f; return Where::Bundle; }
        return Where::None;
    }
    if (seg.size() == 2 && seg[0] == "vendor" && segOk(seg[1], false) &&
        (endsWith(seg[1], ".js") || endsWith(seg[1], ".css"))) {
        rel = p; return Where::Bundle;
    }
    if (seg.size() == 2 && seg[0] == "icons" && segOk(seg[1], false) && endsWith(seg[1], ".png")) {
        rel = p; return Where::Bundle;
    }
    if (seg.size() == 3 && seg[0] == "fonts" && segOk(seg[1], true) && segOk(seg[2], false) &&
        endsWith(seg[2], ".pbf")) {
        rel = p; return Where::Bundle;
    }
    return Where::None;
}

inline const char* contentTypeFor(const std::string& name) {
    if (endsWith(name, ".js"))      return "text/javascript";
    if (endsWith(name, ".css"))     return "text/css";
    if (endsWith(name, ".json"))    return "application/json";
    if (endsWith(name, ".pbf"))     return "application/x-protobuf";
    if (endsWith(name, ".png"))     return "image/png";
    return "application/octet-stream";   // .pmtiles
}

// ── Range ──────────────────────────────────────────────────────────────────────────────────────
enum class Range { Full, Partial, Unsatisfiable };

/** Parse a Range header value against a file of `size` bytes. Partial → [first, last] inclusive.
 *  Anything we will not honour (not `bytes=`, several ranges, malformed) is Full — ignoring a
 *  Range header is always permitted; answering a different range than was asked is not. */
inline Range parseRange(const std::string& header, int64_t size, int64_t& first, int64_t& last) {
    first = 0; last = size > 0 ? size - 1 : -1;
    std::string h;
    for (char c : header) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') h += c;
    if (h.empty()) return Range::Full;
    if (h.size() < 6) return Range::Full;
    std::string unit = h.substr(0, 6);
    for (auto& c : unit) c = (char)std::tolower((unsigned char)c);
    if (unit != "bytes=") return Range::Full;
    const std::string spec = h.substr(6);
    if (spec.empty() || spec.find(',') != std::string::npos) return Range::Full;
    const size_t dash = spec.find('-');
    if (dash == std::string::npos) return Range::Full;
    const std::string sa = spec.substr(0, dash), sb = spec.substr(dash + 1);
    auto num = [](const std::string& s, int64_t& v) {
        if (s.empty() || s.size() > 18) return false;
        v = 0;
        for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + (c - '0'); }
        return true;
    };
    int64_t va = 0, vb = 0;
    if (sa.empty()) {                              // bytes=-n : the last n bytes
        if (!num(sb, vb)) return Range::Full;
        if (vb == 0 || size <= 0) return Range::Unsatisfiable;
        first = vb >= size ? 0 : size - vb;
        last = size - 1;
        return Range::Partial;
    }
    if (!num(sa, va)) return Range::Full;
    if (sb.empty()) {                              // bytes=a-
        if (va >= size) return Range::Unsatisfiable;
        first = va; last = size - 1;
        return Range::Partial;
    }
    if (!num(sb, vb) || vb < va) return Range::Full;   // bytes=a-b, invalid when b < a
    if (va >= size) return Range::Unsatisfiable;
    first = va; last = vb >= size ? size - 1 : vb;
    return Range::Partial;
}

// ── detail pack: status / install / remove ─────────────────────────────────────────────────────
inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}
inline std::string readSmall(const std::string& path) {
    std::string out;
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[512];
    const size_t n = ::fread(buf, 1, sizeof buf, f);
    ::fclose(f);
    out.assign(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
inline void writeSmall(const std::string& path, const std::string& text) {
    FILE* f = ::fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "VibeServer: mapgl: cannot write %s (%s)\n", path.c_str(), strerror(errno)); return; }
    ::fputs(text.c_str(), f);
    ::fclose(f);
}

/** Is another process holding the download lock? (This process's own download is tracked in
 *  State::downloading, and flock is per open file, so we must not probe our own.) */
inline bool lockHeldElsewhere(const std::string& dd) {
    if (dd.empty()) return false;
    const std::string lp = dd + "/" + DETAIL_NAME + ".lock";
    const int fd = ::open(lp.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    if (!held) ::flock(fd, LOCK_UN);
    ::close(fd);
    return held;
}

inline bool available() {
    std::lock_guard<std::mutex> lk(st().mtx);
    return (bool)st().downloader && !st().dataDir.empty();
}

/** {"installed":bool,"bytes":N,"downloading":bool,"written":N,"total":N,"error":"","available":bool} */
inline std::string statusJson() {
    const std::string dd = dataDir();
    int64_t bytes = 0;
    const bool installed = !dd.empty() && isFile(dd + "/" + DETAIL_NAME, &bytes) && bytes > 0;
    bool downloading = st().downloading.load();
    int64_t written = st().written.load(), total = st().total.load();
    std::string error;
    { std::lock_guard<std::mutex> lk(st().mtx); error = st().error; }
    if (!downloading && lockHeldElsewhere(dd)) {
        // ★ Another radio process on this machine is downloading: its .part is the progress.
        downloading = true;
        written = 0;
        isFile(dd + "/" + DETAIL_NAME + ".part", &written);
        total = DETAIL_BYTES;
    }
    if (downloading) error.clear();
    else if (error.empty() && !dd.empty()) error = readSmall(dd + "/" + DETAIL_NAME + ".error");
    if (!downloading) { written = installed ? bytes : 0; total = installed ? bytes : DETAIL_BYTES; }
    std::string j = "{\"installed\":";
    j += installed ? "true" : "false";
    j += ",\"bytes\":" + std::to_string(installed ? bytes : 0);
    j += std::string(",\"downloading\":") + (downloading ? "true" : "false");
    j += ",\"written\":" + std::to_string(written);
    j += ",\"total\":" + std::to_string(total);
    j += ",\"error\":\"" + jsonEsc(error) + "\"";
    j += std::string(",\"available\":") + (available() ? "true" : "false");
    return j + "}";
}

/** Check a finished .part: exact size, then the 7-byte magic. Empty string = good. */
inline std::string verifyPart(const std::string& part) {
    int64_t size = 0;
    if (!isFile(part, &size)) return "the download produced no file";
    if (size != DETAIL_BYTES)
        return "the download was incomplete (" + std::to_string(size) + " of " +
               std::to_string(DETAIL_BYTES) + " bytes)";
    char magic[7] = {0};
    FILE* f = ::fopen(part.c_str(), "rb");
    if (!f) return std::string("could not read the download back (") + strerror(errno) + ")";
    const size_t n = ::fread(magic, 1, 7, f);
    ::fclose(f);
    if (n != 7 || std::memcmp(magic, "PMTiles", 7) != 0)
        return "the downloaded file is not a map archive";
    return {};
}

inline bool startDetailInstall(std::string& err) {
    Downloader dl;
    std::string dd;
    { std::lock_guard<std::mutex> lk(st().mtx); dl = st().downloader; dd = st().dataDir; }
    if (!dl) { err = "this server cannot download map packs"; return false; }
    if (dd.empty()) { err = "this server has nowhere to keep the map pack"; return false; }
    ::mkdir(dd.c_str(), 0755);
    if (!isDir(dd)) { err = "cannot create " + dd + " (" + strerror(errno) + ")"; return false; }
    bool expected = false;
    if (!st().downloading.compare_exchange_strong(expected, true)) {
        err = "the High Detail Maps are already downloading";
        return false;
    }
    // ★ The machine-wide lock: taken HERE, synchronously, so a second request (from any radio
    //   process) is refused with a sentence rather than starting a second 169 MB download.
    const std::string final_ = dd + "/" + DETAIL_NAME;
    const std::string part = final_ + ".part", errPath = final_ + ".error";
    const int lockFd = ::open((final_ + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lockFd < 0) {
        err = "cannot create the download lock in " + dd + " (" + strerror(errno) + ")";
        st().downloading = false;
        return false;
    }
    if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        ::close(lockFd);
        err = "the High Detail Maps are already downloading";
        st().downloading = false;
        return false;
    }
    ::unlink(part.c_str());
    ::unlink(errPath.c_str());
    st().written = 0;
    st().total = DETAIL_BYTES;
    { std::lock_guard<std::mutex> lk(st().mtx); st().error.clear(); }
    fprintf(stderr, "VibeServer: mapgl: downloading High Detail Maps to %s\n", final_.c_str());
    // ★★ A WORKER THREAD, NEVER THE CALLER'S. The caller is a connection handler; 169 MB takes
    //    minutes on a Pi's link. Detached: the state it reports through lives in State.
    std::thread([dl, final_, part, errPath, lockFd]() {
        std::string fail;
        bool ok = false;
        try {
            ok = dl(DETAIL_URL, part, [](int64_t w, int64_t t) {
                st().written = w;
                if (t > 0) st().total = t;
            });
        } catch (const std::exception& e) {
            fail = std::string("the download failed: ") + e.what();
        } catch (...) {
            fail = "the download failed";
        }
        if (fail.empty() && !ok) fail = "the download failed — check this server can reach github.com";
        if (fail.empty()) fail = verifyPart(part);
        if (fail.empty() && ::rename(part.c_str(), final_.c_str()) != 0)
            fail = std::string("could not move the download into place (") + strerror(errno) + ")";
        if (!fail.empty()) {
            ::unlink(part.c_str());
            writeSmall(errPath, fail);
            fprintf(stderr, "VibeServer: mapgl: High Detail Maps install FAILED — %s\n", fail.c_str());
        } else {
            fprintf(stderr, "VibeServer: mapgl: High Detail Maps installed (%lld bytes)\n",
                    (long long)DETAIL_BYTES);
        }
        { std::lock_guard<std::mutex> lk(st().mtx); st().error = fail; }
        st().downloading = false;
        ::flock(lockFd, LOCK_UN);
        ::close(lockFd);
    }).detach();
    return true;
}
inline bool startDetailInstall() { std::string e; return startDetailInstall(e); }

inline bool removeDetail(std::string& err) {
    const std::string dd = dataDir();
    if (dd.empty()) { err = "this server has no map pack to remove"; return false; }
    if (st().downloading.load() || lockHeldElsewhere(dd)) {
        err = "the High Detail Maps are downloading — wait for it to finish";
        return false;
    }
    const std::string final_ = dd + "/" + DETAIL_NAME;
    // ★ A listener mid-read keeps its open descriptor; unlink only removes the name.
    if (::unlink(final_.c_str()) != 0 && errno != ENOENT) {
        err = "could not remove " + final_ + " (" + strerror(errno) + ")";
        fprintf(stderr, "VibeServer: mapgl: %s\n", err.c_str());
        return false;
    }
    ::unlink((final_ + ".part").c_str());
    ::unlink((final_ + ".error").c_str());
    { std::lock_guard<std::mutex> lk(st().mtx); st().error.clear(); }
    fprintf(stderr, "VibeServer: mapgl: High Detail Maps removed\n");
    return true;
}
inline bool removeDetail() { std::string e; return removeDetail(e); }

// ── serving ────────────────────────────────────────────────────────────────────────────────────
#ifndef VIBE_MAPGL_NO_NET
inline void sendPlain(const std::shared_ptr<net::Socket>& sock, const char* status,
                      const std::string& body, const std::string& extra = {}) {
    sock->sendstr(std::string("HTTP/1.1 ") + status +
                  "\r\nContent-Type: text/plain; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\n" +
                  extra + "Connection: close\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\n\r\n" + body);
    sock->close();
}

/** Serve GET/HEAD /mapgl/<encoded>. `range` is the raw Range header value ("" when absent).
 *  Always answers and closes. Streams in chunks; never holds a file in memory.
 *  ★★ `paced`: somebody is LISTENING, so the body goes out through vibebulk's shared budget and
 *     never ahead of their audio (see vibe_bulk_pace.h). With nobody listening there is nothing
 *     to protect and the map loads at full speed. Required, not defaulted — the caller decides. */
inline void serve(const std::shared_ptr<net::Socket>& sock, const std::string& encoded,
                  const std::string& range, bool head, bool paced) {
    std::string rel;
    const Where w = resolve(encoded, rel);
    const std::string base = w == Where::Bundle ? bundleDir() : w == Where::Data ? dataDir() : "";
    if (w == Where::None) { sendPlain(sock, "404 Not Found", "no such map file\n"); return; }
    if (base.empty()) {
        sendPlain(sock, "404 Not Found", w == Where::Data
            ? "the High Detail Maps are not installed on this server\n"
            : "no GPU map files are installed on this server\n");
        return;
    }
    const std::string path = base + "/" + rel;
    FILE* f = ::fopen(path.c_str(), "rb");
    struct stat sb{};
    if (!f || ::fstat(fileno(f), &sb) != 0 || !S_ISREG(sb.st_mode)) {
        if (f) ::fclose(f);
        sendPlain(sock, "404 Not Found", w == Where::Data
            ? "the High Detail Maps are not installed on this server\n"
            : "this server does not carry that map file\n");
        return;
    }
    const int64_t size = (int64_t)sb.st_size;
    int64_t first = 0, last = size - 1;
    const Range r = parseRange(range, size, first, last);
    if (r == Range::Unsatisfiable) {
        ::fclose(f);
        sendPlain(sock, "416 Range Not Satisfiable", "",
                  "Content-Range: bytes */" + std::to_string(size) + "\r\nAccept-Ranges: bytes\r\n");
        return;
    }
    const int64_t len = r == Range::Partial ? last - first + 1 : size;
    std::string hdr = r == Range::Partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    hdr += "Content-Type: ";
    hdr += contentTypeFor(rel);
    hdr += "\r\nAccess-Control-Allow-Origin: *\r\n"
           "Access-Control-Expose-Headers: Content-Range, Content-Length, Accept-Ranges\r\n"
           "Accept-Ranges: bytes\r\n";
    // ★ The bundle only changes with the package; the detail pack can be removed or replaced.
    hdr += w == Where::Data ? "Cache-Control: no-cache\r\n" : "Cache-Control: public, max-age=86400\r\n";
    if (r == Range::Partial)
        hdr += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
               std::to_string(size) + "\r\n";
    hdr += "Connection: close\r\nContent-Length: " + std::to_string(len) + "\r\n\r\n";
    if (sock->sendstr(hdr) < 0 || head || len == 0) { ::fclose(f); sock->close(); return; }
    if (first > 0 && ::fseeko(f, (off_t)first, SEEK_SET) != 0) { ::fclose(f); sock->close(); return; }
    std::vector<uint8_t> buf(paced ? vibebulk::kChunk : 64 * 1024);
    int64_t left = len;
    while (left > 0) {
        const size_t want = (size_t)std::min<int64_t>(left, (int64_t)buf.size());
        const size_t n = ::fread(buf.data(), 1, want, f);
        if (n == 0) break;
        if (paced) vibebulk::pace(n);
        if (sock->send(buf.data(), n) < 0) break;   // ★ peer went away (a pan cancels reads)
        left -= (int64_t)n;
    }
    ::fclose(f);
    sock->close();
}
#endif  // VIBE_MAPGL_NO_NET

}  // namespace vibemapgl
