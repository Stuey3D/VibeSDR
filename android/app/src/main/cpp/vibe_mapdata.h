// VibeServer — the bundled vector map, served FROM DISK at /mapdata/v1/<file>.
//
// ★★★ FILES ON DISK, NEVER COMPILED IN. Everything else this server sends a browser — the client
//     itself, the icon, the favicon — is base64 inside the executable, because a PHONE has nowhere
//     to serve files from (see vibe_web_page.h). The map is the first asset where that trick is
//     actively wrong: the basic pack alone is ~22 MB and the optional detail pack is ~221 MB, and a
//     compiled-in blob is resident for the life of the process. A Pi 2 has 1 GB for the whole
//     machine, radios included. So these are ordinary files, opened per request, streamed, and
//     never held.
//
// ★★★ NO MAPS IS A DEGRADED MAP, NOT A BROKEN SERVER. The directory may be absent entirely — an
//     upgrade from a package that predates it, a build from a tree with no assets, a hand-copied
//     binary — and every one of those must still serve radio. So a missing directory and a missing
//     file are the same clean 404, and nothing here can abort the process or throw into the
//     connection handler.
//
// ★★ WHY A LONG CACHE IS SAFE HERE AND NOWHERE ELSE IN THIS SERVER. The path carries the dataset
//    version (/mapdata/v1/), and the generator writes a NEW version rather than changing files
//    under an old one — so the bytes at a given URL never change and `immutable` is the truth. The
//    web client and the favicon are deliberately no-store/one-hour for the opposite reason: they
//    change with the server they are served from.
//
// ★ Flat filenames only, and that is a security property, not a simplification: the dataset is one
//   flat directory, so anything containing '/' or ".." is a traversal attempt and is refused before
//   a path is ever built.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "net_shim.h"

namespace vibemap {

inline std::mutex& dirMtx() { static std::mutex m; return m; }
inline std::string& dirRef() { static std::string d; return d; }

/** Override where the map data lives. Empty restores the automatic search below. */
inline void setDir(const std::string& d) {
    std::lock_guard<std::mutex> lk(dirMtx());
    dirRef() = d;
}

inline bool isDir(const std::string& p) {
    struct stat st{};
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/** ★★★ FOUND RELATIVE TO THE BINARY FIRST, so the package works wherever it is unpacked.
 *
 *  The .deb installs the binary to /usr/bin/vibeserver and the data to
 *  /usr/lib/vibeserver/mapdata — the same "../lib/vibeserver" relation the bundled librtlsdr uses
 *  for its RPATH, and for the same reason: a prefix other than /usr (a staged install, a container
 *  with /opt) must not silently serve nothing.
 *  ★★ Resolved ONCE and cached. This is on the request path, and a per-request stat() storm over
 *     four candidate directories for every one of the ~160 files a map load asks for is a real cost
 *     on a Pi.
 *  ★ VIBESERVER_MAPDATA_DIR overrides everything — how a developer points a server at the repo's
 *    own assets/mapdata/v1 without installing anything, and the only escape hatch an owner needs
 *    if they keep the detail pack on another disk.
 */
inline const std::string& dir() {
    static std::string cached;
    static bool done = false;
    {
        std::lock_guard<std::mutex> lk(dirMtx());
        if (!dirRef().empty()) return dirRef();
    }
    if (done) return cached;
    done = true;
    if (const char* e = getenv("VIBESERVER_MAPDATA_DIR"); e && *e && isDir(e)) { cached = e; return cached; }
    // Where are we? /proc/self/exe on Linux; the Mac app sets the environment variable above.
    std::string exeDir;
    {
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            const std::string exe(buf);
            const size_t slash = exe.rfind('/');
            if (slash != std::string::npos) exeDir = exe.substr(0, slash);
        }
    }
    const std::string candidates[] = {
        exeDir.empty() ? std::string() : exeDir + "/../lib/vibeserver/mapdata",
        exeDir.empty() ? std::string() : exeDir + "/mapdata",
        "/usr/lib/vibeserver/mapdata",
        "/usr/share/vibeserver/mapdata",
        // ★ Last: the machine's own data directory, so an owner who cannot write /usr (or who is
        //   running a binary they built themselves) has somewhere to unpack a pack by hand.
        "/var/lib/vibeserver/mapdata",
    };
    for (const std::string& c : candidates) if (isDir(c)) { cached = c; return cached; }
    cached.clear();
    return cached;
}

/** ★ A name we are willing to open: the flat filenames the generator emits and nothing else.
 *  Rejecting '/' and ".." here is what makes the concatenation below safe; a check further down,
 *  after a path has been assembled, is the version of this that gets bypassed. */
inline bool nameOk(const std::string& n) {
    if (n.empty() || n.size() > 128) return false;
    if (n[0] == '.') return false;                       // ".." and dotfiles alike
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return n.find("..") == std::string::npos;
}

inline const char* contentTypeFor(const std::string& name) {
    auto ends = [&](const char* s) {
        const size_t n = std::strlen(s);
        return name.size() >= n && name.compare(name.size() - n, n, s) == 0;
    };
    if (ends(".json")) return "application/json";
    if (ends(".png"))  return "image/png";
    if (ends(".webp")) return "image/webp";
    return "application/octet-stream";
}

/** ★★ THE OPTIONAL PRE-COMPRESSED SIBLING. The dataset is 221 MB raw and 82 MB gzipped, and these
 *  are GeoJSON — the single most compressible thing this server sends. We do not gzip on the fly:
 *  that would put a deflate of a 20 MB file on a connection thread on a Pi, for a file that never
 *  changes. Instead, if `<file>.gz` sits beside it, that is what goes out with Content-Encoding.
 *  The package ships the files raw; an owner tight on disk (or bandwidth) can `gzip -k` the tree
 *  and the server picks it up with no configuration.
 *  ★ Never served to a client that did not ask: a browser that omits Accept-Encoding gets the raw
 *    file, and if only the .gz exists for it there is nothing we can do but 404 — which is why the
 *    packaged form is the raw one. */
struct Chosen {
    std::string path;
    bool        gzip = false;
    long long   size = 0;
};

inline bool choose(const std::string& name, bool acceptGzip, Chosen& out) {
    const std::string& base = dir();
    if (base.empty() || !nameOk(name)) return false;
    auto tryOne = [&](const std::string& p, bool gz) {
        struct stat st{};
        if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
        out.path = p; out.gzip = gz; out.size = (long long)st.st_size;
        return true;
    };
    const std::string raw = base + "/" + name;
    if (acceptGzip && tryOne(raw + ".gz", true)) return true;
    return tryOne(raw, false);
}

/** Serve GET /mapdata/v1/<name>. Returns having answered the request either way — a 200 with the
 *  file, or a 404 with one plain sentence. `head` sends the headers only.
 *  ★★ STREAMED IN CHUNKS, NEVER SLURPED. A single detail shard runs to tens of MB; reading one
 *     into a std::string to measure it would double it in RAM and, on the Pi 2, do that once per
 *     concurrent listener. The size comes from stat(), which is what Content-Length wants anyway.
 *  ★ 64 KB: comfortably more than a socket write can absorb in one go, small enough that the
 *    buffer is a stack-free heap block we can afford per connection. */
inline void serve(const std::shared_ptr<net::Socket>& sock, const std::string& name,
                  bool acceptGzip, bool head = false) {
    Chosen c;
    if (!choose(name, acceptGzip, c)) {
        // ★★ A SENTENCE, NOT AN EMPTY 404 — AND IT DISTINGUISHES THE TWO CASES, because they have
        //    different cures and only one of them is the owner's business. No DIRECTORY means the
        //    data was never installed; a missing FILE inside one that exists means the client asked
        //    for a layer this pack does not carry, which is the detail pack nine times out of ten.
        //    A single "not found" for both is how "install the optional pack" gets read as a bug in
        //    the client.
        const std::string body = dir().empty()
            ? std::string("no map data installed on this server\n")
            : std::string("this server does not carry that map layer — the optional detail pack "
                          "(vibeserver-mapdata-detail) may not be installed\n");
        sock->sendstr("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\n"
                      "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: "
                      + std::to_string(body.size()) + "\r\n\r\n" + body);
        sock->close();
        return;
    }
    FILE* f = ::fopen(c.path.c_str(), "rb");
    if (!f) {   // ★ Raced with an uninstall, or unreadable. Same answer as absent.
        sock->sendstr("HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\nContent-Length: 0\r\n\r\n");
        sock->close();
        return;
    }
    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: ";
    hdr += contentTypeFor(name);
    hdr += "\r\nAccess-Control-Allow-Origin: *\r\n";
    // ★★ A YEAR, AND `immutable`. See the version note at the top of this file: the URL names the
    //    dataset version, so a re-fetch can only ever return the same bytes. Without this a map
    //    pan re-validates ~160 files against the server on every page load.
    hdr += "Cache-Control: public, max-age=31536000, immutable\r\n";
    if (c.gzip) hdr += "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n";
    hdr += "Connection: close\r\nContent-Length: " + std::to_string(c.size) + "\r\n\r\n";
    if (sock->sendstr(hdr) < 0 || head) { ::fclose(f); sock->close(); return; }
    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
        const size_t n = ::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        if (sock->send(buf.data(), n) < 0) break;   // ★ Peer went away mid-file: nothing to say.
    }
    ::fclose(f);
    sock->close();
}

/** Is any map data installed? For the server's own status, so "no map" is reportable rather than
 *  something a listener discovers as a blank rectangle. */
inline bool installed() { return !dir().empty(); }

}  // namespace vibemap
