// test-mapgl-range.cpp — the GPU map's file server: Range parsing, path safety, and the rule that
// half a download is never "installed" (android/app/src/main/cpp/vibe_mapgl.h).
//
// ★★★ WHY RANGE MATTERS. A PMTiles archive is read a few KB at a time. A wrong 206 (the wrong bytes,
//     or a Content-Range that disagrees with the body) corrupts the tile silently; a 200 where a 206
//     was asked for pulls the whole 169 MB detail pack to draw one country.
// ★★ No socket and no radio: the parsing and path rules are pure functions.
#define VIBE_MAPGL_NO_NET 1
#include "vibe_mapgl.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;
static void ok(bool c, const char* what) { printf("  %s  %s\n", c ? "ok  " : "FAIL", what); if (!c) fails++; }

static bool part(const char* h, int64_t size, int64_t a, int64_t b) {
    int64_t f = -9, l = -9;
    return vibemapgl::parseRange(h, size, f, l) == vibemapgl::Range::Partial && f == a && l == b;
}
static vibemapgl::Range kind(const char* h, int64_t size) {
    int64_t f, l;
    return vibemapgl::parseRange(h, size, f, l);
}
static bool refused(const char* p) { std::string r; return vibemapgl::resolve(p, r) == vibemapgl::Where::None; }
static bool bundled(const char* p, const char* want) {
    std::string r;
    return vibemapgl::resolve(p, r) == vibemapgl::Where::Bundle && r == want;
}

/* ── beta96: bundle files served from INSIDE an APK (setBundleSlice) ─────────────────────────────────────────────
 * A real zip with one STORED pack entry (and one DEFLATE-flagged entry), written here so the test needs no zip tool;
 * its entry offset is found the way Android's AssetFileDescriptor reports it (local header + name + extra). */
struct Rec {   // what serveTo() wrote
    std::string data; bool closed = false;
    int sendstr(const std::string& x) { data += x; return (int)x.size(); }
    int send(const uint8_t* d, size_t n) { data.append((const char*)d, n); return (int)n; }
    void close() { closed = true; }
    std::string head() const { const size_t e = data.find("\r\n\r\n"); return e == std::string::npos ? data : data.substr(0, e + 2); }
    std::string body() const { const size_t e = data.find("\r\n\r\n"); return e == std::string::npos ? "" : data.substr(e + 4); }
};
static void le16(std::string& o, unsigned v) { o += (char)(v & 255); o += (char)((v >> 8) & 255); }
static void le32(std::string& o, unsigned v) { le16(o, v & 0xffff); le16(o, v >> 16); }
/** Minimal zip: each entry's local header (method 0 = stored or 8 = deflate flag; CRC left 0 — nothing here checks it),
 *  an extra field like aapt's alignment padding, the central directory and the end record. Returns data offsets. */
static std::vector<long> writeZip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& ents,
                                  const std::vector<int>& methods) {
    std::string z, cd; std::vector<long> offs;
    for (size_t i = 0; i < ents.size(); ++i) {
        const auto& [name, body] = ents[i];
        const unsigned lho = (unsigned)z.size();
        const std::string extra(5 + i, '\0');   // ★ odd-sized padding: the offset is NOT just header + name
        z += "PK\x03\x04"; le16(z, 10); le16(z, 0); le16(z, (unsigned)methods[i]); le16(z, 0); le16(z, 0);
        le32(z, 0); le32(z, (unsigned)body.size()); le32(z, (unsigned)body.size());
        le16(z, (unsigned)name.size()); le16(z, (unsigned)extra.size()); z += name; z += extra;
        offs.push_back((long)z.size());
        z += body;
        cd += "PK\x01\x02"; le16(cd, 20); le16(cd, 10); le16(cd, 0); le16(cd, (unsigned)methods[i]); le16(cd, 0); le16(cd, 0);
        le32(cd, 0); le32(cd, (unsigned)body.size()); le32(cd, (unsigned)body.size());
        le16(cd, (unsigned)name.size()); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, lho);
        cd += name;
    }
    const unsigned cdo = (unsigned)z.size();
    z += cd;
    z += "PK\x05\x06"; le16(z, 0); le16(z, 0); le16(z, (unsigned)ents.size()); le16(z, (unsigned)ents.size());
    le32(z, (unsigned)cd.size()); le32(z, cdo); le16(z, 0);
    FILE* f = fopen(path.c_str(), "wb"); fwrite(z.data(), 1, z.size(), f); fclose(f);
    return offs;
}
/** Local-header walk, as AssetManager.openFd reports startOffset: 30 + name length + extra length. */
static long entryOffset(const std::string& zip, const std::string& want) {
    FILE* f = fopen(zip.c_str(), "rb"); std::string z; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) z.append(b, n);
    fclose(f);
    size_t p = 0;
    while (p + 30 <= z.size() && z.compare(p, 4, "PK\x03\x04") == 0) {
        auto u16 = [&](size_t o) { return (unsigned)(unsigned char)z[o] | ((unsigned)(unsigned char)z[o + 1] << 8); };
        auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
        const unsigned csz = u32(p + 18), nl = u16(p + 26), xl = u16(p + 28);
        if (z.compare(p + 30, nl, want) == 0) return (long)(p + 30 + nl + xl);
        p += 30 + nl + xl + csz;
    }
    return -1;
}

int main() {
    using vibemapgl::Range;
    printf("Range:\n");
    ok(part("bytes=0-99", 1000, 0, 99), "bytes=a-b");
    ok(part("bytes=0-0", 1000, 0, 0), "bytes=0-0 is one byte");
    ok(part("bytes=900-2000", 1000, 900, 999), "bytes=a-b past the end is clamped to the last byte");
    ok(part("bytes=500-", 1000, 500, 999), "bytes=a- runs to the end");
    ok(part("bytes=-100", 1000, 900, 999), "bytes=-n is the last n bytes");
    ok(part("bytes=-5000", 1000, 0, 999), "bytes=-n larger than the file is the whole file");
    ok(part("Bytes = 10 - 20", 1000, 10, 20), "unit case and spaces tolerated");
    ok(kind("bytes=1000-", 1000) == Range::Unsatisfiable, "offset == size is 416");
    ok(kind("bytes=5000-6000", 1000) == Range::Unsatisfiable, "offset past the end is 416");
    ok(kind("bytes=-0", 1000) == Range::Unsatisfiable, "bytes=-0 is 416");
    ok(kind("bytes=0-", 0) == Range::Unsatisfiable, "any range of an empty file is 416");
    ok(kind("", 1000) == Range::Full, "no header is a full 200");
    ok(kind("bytes=0-10,20-30", 1000) == Range::Full, "several ranges are IGNORED (200), never half-answered");
    ok(kind("items=0-10", 1000) == Range::Full, "a unit that is not bytes is ignored");
    ok(kind("bytes=20-10", 1000) == Range::Full, "b < a is malformed and ignored");
    ok(kind("bytes=abc-", 1000) == Range::Full, "garbage is ignored");
    ok(kind("bytes=", 1000) == Range::Full, "an empty spec is ignored");
    ok(part("bytes=177024000-", 177024426, 177024000, 177024425), "offsets past 2^31 (a detail-pack-sized file)");

    printf("Paths:\n");
    ok(bundled("vendor/maplibre-gl.js", "vendor/maplibre-gl.js"), "vendor .js");
    ok(bundled("vendor/maplibre-gl.css", "vendor/maplibre-gl.css"), "vendor .css");
    ok(bundled("vibemap-style.json", "vibemap-style.json"), "the style");
    ok(bundled("vibemap-basic.pmtiles", "vibemap-basic.pmtiles"), "the basic pack");
    ok(bundled("vibemap-relief.pmtiles", "vibemap-relief.pmtiles"), "the relief pack");
    ok(bundled("vibemap-runways.pmtiles", "vibemap-runways.pmtiles"), "the runways pack");
    ok(bundled("fonts/JetBrains%20Mono%20Bold/0-255.pbf", "fonts/JetBrains Mono Bold/0-255.pbf"),
       "★ a font name with %20 decodes to its directory");
    ok(bundled("icons/vs-aircraft.png", "icons/vs-aircraft.png"), "icons");
    { std::string r; ok(vibemapgl::resolve("vibemap-detail.pmtiles", r) == vibemapgl::Where::Data,
                        "the detail pack comes from the DATA dir"); }
    ok(refused("../etc/passwd"), "../ refused");
    ok(refused("vendor/../../etc/passwd"), "vendor/../.. refused");
    ok(refused("%2e%2e/etc/passwd"), "encoded .. refused");
    ok(refused("vendor/%2e%2e%2fsecret.js"), "encoded ../ inside a segment refused");
    ok(refused("/etc/passwd"), "absolute path refused");
    ok(refused("%2fetc/passwd"), "encoded absolute path refused");
    ok(refused("vendor\\maplibre-gl.js"), "backslash refused");
    ok(refused("vendor%5cmaplibre-gl.js"), "encoded backslash refused");
    ok(refused("vendor/maplibre-gl.js%00.css"), "NUL refused");
    ok(refused("vendor/.hidden.js"), "dot-file refused");
    ok(refused("vendor/LICENSE-maplibre-gl.txt"), "a vendor file that is not .js/.css refused");
    ok(refused("vibemap-style.js"), "names outside the list refused");
    ok(refused("fonts/JetBrains Mono Bold/../0-255.pbf"), "font traversal refused");
    ok(refused("fonts/a/b/0-255.pbf"), "too deep refused");
    ok(refused("icons/x.svg"), "wrong extension refused");
    ok(refused("vendor/x.js%zz"), "a malformed escape refused");
    ok(refused(""), "empty refused");

    printf("Bundle files inside an APK (setBundleSlice):\n");
    {
        char t2[] = "/tmp/test-mapgl-apk-XXXXXX";
        const std::string ad = ::mkdtemp(t2);
        const std::string apk = ad + "/base.apk", bdir = ad + "/bundled";
        ::mkdir(bdir.c_str(), 0755);
        { FILE* f = fopen((bdir + "/vibemap-style.json").c_str(), "wb"); fputs("{\"version\":8}", f); fclose(f); }
        vibemapgl::setBundleDir(bdir);
        // A pack-shaped body: the magic, then bytes whose value is their own position (mod 251) — any off-by-one shows.
        std::string pack = "PMTiles\x03";
        while (pack.size() < 300000) pack += (char)(pack.size() % 251);
        const std::string relief = "PMTiles-relief-body";
        writeZip(apk, {{"AndroidManifest.xml", "<manifest/>"}, {"assets/mapgl/vibemap-basic.pmtiles", pack},
                       {"assets/mapgl/vibemap-relief.pmtiles", relief}}, {8, 0, 8});
        const long off = entryOffset(apk, "assets/mapgl/vibemap-basic.pmtiles");
        const long offR = entryOffset(apk, "assets/mapgl/vibemap-relief.pmtiles");
        ok(off > 0 && offR > 0, "the zip's stored entry offsets are found");
        std::string why;
        ok(vibemapgl::setBundleSlice("vibemap-basic.pmtiles", apk, off, (int64_t)pack.size(), &why), "a stored pack entry is accepted");
        ok(!vibemapgl::setBundleSlice("vibemap-runways.pmtiles", apk, off + 1, 1000, &why) && why.find("magic") != std::string::npos,
           "★★ a run that does not start with the PMTiles magic (a compressed entry) is refused — the caller unpacks");
        ok(!vibemapgl::setBundleSlice("vibemap-relief.pmtiles", apk, offR, 1 << 30, &why), "a run past the end of the APK is refused");
        ok(!vibemapgl::setBundleSlice("../etc/passwd", apk, off, 10, &why), "a name that is not a bundle file is refused");
        ok(!vibemapgl::setBundleSlice("vibemap-detail.pmtiles", apk, off, 10, &why), "the DATA pack can never be a slice");
        ok(!vibemapgl::setBundleSlice("vibemap-relief.pmtiles", ad + "/missing.apk", 0, 10, &why), "a missing APK is refused");

        Rec a; vibemapgl::serveTo(a, "vibemap-basic.pmtiles", "bytes=0-6", false);
        ok(a.data.rfind("HTTP/1.1 206", 0) == 0 && a.body() == "PMTiles" && a.closed, "206 with the magic from inside the APK");
        ok(a.head().find("Content-Range: bytes 0-6/" + std::to_string(pack.size()) + "\r\n") != std::string::npos,
           "★★ Content-Range total is the ENTRY's length, not the APK's");
        ok(a.head().find("Content-Length: 7\r\n") != std::string::npos && a.head().find("Accept-Ranges: bytes") != std::string::npos,
           "Content-Length 7, Accept-Ranges");
        Rec b; vibemapgl::serveTo(b, "vibemap-basic.pmtiles", "bytes=123456-123999", false);
        ok(b.body() == pack.substr(123456, 544), "a range in the middle is exactly the entry's bytes");
        Rec b2; vibemapgl::serveTo(b2, "vibemap-basic.pmtiles", "bytes=-10", false);
        ok(b2.body() == pack.substr(pack.size() - 10), "bytes=-n is the END OF THE ENTRY, never the APK's central directory");
        Rec c; vibemapgl::serveTo(c, "vibemap-basic.pmtiles", "", false, 4096);
        ok(c.data.rfind("HTTP/1.1 200", 0) == 0 && c.body() == pack, "no Range: the whole entry, 200, in small chunks");
        Rec d; vibemapgl::serveTo(d, "vibemap-basic.pmtiles", "", true);
        ok(d.body().empty() && d.head().find("Content-Length: " + std::to_string(pack.size()) + "\r\n") != std::string::npos,
           "HEAD: the entry's length, no body");
        Rec e; vibemapgl::serveTo(e, "vibemap-basic.pmtiles", "bytes=" + std::to_string(pack.size()) + "-", false);
        ok(e.data.rfind("HTTP/1.1 416", 0) == 0 && e.head().find("bytes */" + std::to_string(pack.size())) != std::string::npos,
           "past the entry's end is 416 with the entry's size");
        Rec f; vibemapgl::serveTo(f, "vibemap-style.json", "", false);
        ok(f.data.rfind("HTTP/1.1 200", 0) == 0 && f.body() == "{\"version\":8}", "a file with no slice still comes from the bundle dir");
        Rec g; vibemapgl::serveTo(g, "vibemap-relief.pmtiles", "", false);
        ok(g.data.rfind("HTTP/1.1 404", 0) == 0, "a pack with neither a slice nor an unpacked file is 404");
        // ★ Re-registering (an app update moves the APK) replaces the slice.
        ok(vibemapgl::setBundleSlice("vibemap-basic.pmtiles", apk, offR, (int64_t)relief.size(), &why), "re-register");
        Rec h; vibemapgl::serveTo(h, "vibemap-basic.pmtiles", "", false);
        ok(h.body() == relief, "the newer slice answers");
        // ★ The APK vanishes under a registered slice (uninstall mid-request): 404, not garbage.
        unlink(apk.c_str());
        Rec k; vibemapgl::serveTo(k, "vibemap-basic.pmtiles", "bytes=0-6", false);
        ok(k.data.rfind("HTTP/1.1 404", 0) == 0, "the APK gone: 404");
        vibemapgl::clearBundleSlices();
        vibemapgl::setBundleDir("");
        unlink((bdir + "/vibemap-style.json").c_str()); rmdir(bdir.c_str()); rmdir(ad.c_str());
    }

    printf("Detail pack install (atomic):\n");
    char tmpl[] = "/tmp/test-mapgl-XXXXXX";
    const std::string dir = ::mkdtemp(tmpl);
    const std::string fin = dir + "/vibemap-detail.pmtiles";
    vibemapgl::setDataDir(dir);
    ok(vibemapgl::statusJson().find("\"available\":false") != std::string::npos,
       "no downloader: available is false");
    std::string err;
    ok(!vibemapgl::startDetailInstall(err) && !err.empty(), "no downloader: install refused with a reason");
    auto waitIdle = [] { for (int i = 0; i < 400 && vibemapgl::st().downloading.load(); ++i) usleep(10000); };

    // A short download: must NOT be installed.
    vibemapgl::setDownloader([](const std::string&, const std::string& dest, std::function<void(int64_t, int64_t)> p) {
        FILE* f = fopen(dest.c_str(), "wb"); fputs("PMTiles", f); fclose(f); p(7, 7); return true; });
    ok(vibemapgl::startDetailInstall(err), "install starts");
    waitIdle();
    ok(access(fin.c_str(), F_OK) != 0, "★★ a short download is never renamed into place");
    ok(access((fin + ".part").c_str(), F_OK) != 0, "and its .part is cleaned up");
    ok(vibemapgl::statusJson().find("incomplete") != std::string::npos, "and the status says why");

    // Right size, wrong magic.
    vibemapgl::setDownloader([](const std::string&, const std::string& dest, std::function<void(int64_t, int64_t)>) {
        FILE* f = fopen(dest.c_str(), "wb"); fputs("NOTMAPS", f); fclose(f);
        truncate(dest.c_str(), vibemapgl::DETAIL_BYTES); return true; });
    vibemapgl::startDetailInstall(err); waitIdle();
    ok(access(fin.c_str(), F_OK) != 0, "★★ the right size with the wrong magic is refused");

    // Good (sparse, so it costs no disk).
    vibemapgl::setDownloader([](const std::string&, const std::string& dest, std::function<void(int64_t, int64_t)>) {
        FILE* f = fopen(dest.c_str(), "wb"); fputs("PMTiles", f); fclose(f);
        truncate(dest.c_str(), vibemapgl::DETAIL_BYTES); return true; });
    vibemapgl::startDetailInstall(err); waitIdle();
    ok(access(fin.c_str(), F_OK) == 0, "a whole, valid download is installed");
    const std::string s = vibemapgl::statusJson();
    ok(s.find("\"installed\":true") != std::string::npos && s.find("\"error\":\"\"") != std::string::npos &&
       s.find("\"bytes\":" + std::to_string(vibemapgl::DETAIL_BYTES)) != std::string::npos &&
       s.find("\"available\":true") != std::string::npos && s.find("\"outdated\":false") != std::string::npos,
       "status: installed, bytes, no error, available, not outdated");
    printf("    %s\n", s.c_str());

    // ★★ An OLDER pack (v1's size) is reported outdated and a server replaces it by itself.
    truncate(fin.c_str(), 177024426);
    ok(vibemapgl::statusJson().find("\"outdated\":true") != std::string::npos, "an older pack's size: outdated");
    vibemapgl::refreshOutdated(0);
    for (int i = 0; i < 300; ++i) {
        struct stat sb {};
        if (!vibemapgl::st().downloading.load() && ::stat(fin.c_str(), &sb) == 0 && sb.st_size == vibemapgl::DETAIL_BYTES) break;
        usleep(10000);
    }
    waitIdle();
    { struct stat sb {}; ok(::stat(fin.c_str(), &sb) == 0 && sb.st_size == vibemapgl::DETAIL_BYTES,
                            "★★ refreshOutdated replaced the older pack with the published one"); }
    ok(vibemapgl::statusJson().find("\"outdated\":false") != std::string::npos, "and it is no longer outdated");

    ok(vibemapgl::removeDetail(err), "remove");
    ok(vibemapgl::statusJson().find("\"installed\":false") != std::string::npos, "status: not installed");
    // ★★ No pack installed: refreshOutdated never gives a server one it did not ask for.
    vibemapgl::refreshOutdated(0);
    usleep(300000); waitIdle();
    ok(access(fin.c_str(), F_OK) != 0, "★★ no pack installed: refreshOutdated downloads nothing");

    // A downloader that fails.
    vibemapgl::setDownloader([](const std::string&, const std::string&, std::function<void(int64_t, int64_t)>) { return false; });
    vibemapgl::startDetailInstall(err); waitIdle();
    ok(vibemapgl::statusJson().find("github.com") != std::string::npos, "a failed download reports a reason");

    unlink((fin + ".lock").c_str()); unlink((fin + ".error").c_str()); rmdir(dir.c_str());
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
