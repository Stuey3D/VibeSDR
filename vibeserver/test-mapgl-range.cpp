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
#include <string>
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
    ok(part("bytes=177024000-", 177024426, 177024000, 177024425), "offsets past 2^31 (the detail pack)");

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
       s.find("\"bytes\":177024426") != std::string::npos && s.find("\"available\":true") != std::string::npos,
       "status: installed, bytes, no error, available");
    printf("    %s\n", s.c_str());
    ok(vibemapgl::removeDetail(err), "remove");
    ok(vibemapgl::statusJson().find("\"installed\":false") != std::string::npos, "status: not installed");

    // A downloader that fails.
    vibemapgl::setDownloader([](const std::string&, const std::string&, std::function<void(int64_t, int64_t)>) { return false; });
    vibemapgl::startDetailInstall(err); waitIdle();
    ok(vibemapgl::statusJson().find("github.com") != std::string::npos, "a failed download reports a reason");

    unlink((fin + ".lock").c_str()); unlink((fin + ".error").c_str()); rmdir(dir.c_str());
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
