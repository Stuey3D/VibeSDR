// test-station-store.cpp — V12 beta90 THE STATION STORE (vibe_station_store.h) for the DIGI MODES MAP and the PSK Reporter
// upload, docs/v12/DIGI-MODES-MAP.md.
//
//  1. Validation: only real callsigns and real grids (FT8 words, RR73, AA00, junk and markup never become stations).
//  2. RAM only (no gallery): spots merge per call+mode+band; the map answer is one entry per call with its modes and bands.
//  3. The disk: a flush writes the day's file; a second store (another radio's PROCESS) sees it; counts add as deltas, so
//     two radios and repeated flushes never double-count; the unflushed part is laid over the file.
//  4. FOUR PROCESSES (fork) flushing one day at once: one valid file, every station once, the counts exact.
//  5. Days and windows: 1 h / 24 h / 7 d / 30 d / all; a spot yesterday is in 24 h, not in 1 h; changedSince = live poll.
//  6. BOUNDED: kDayCap rows a day (196 KB), the rest counted dropped; maxOut calls in an answer; days pruned to the budget.
//  7. subscribe(): every accepted reception, newToday once per call+mode+band; unsubscribe stops it; forEachSince.
//  8. Cost: spots per second into the RAM table; a flush of a full day; a 30-day map answer (cached past days).
//  `--write <galleryDir>` writes a small fixture (today's stations) for the web/app map tests and exits.
#include "vibe_station_store.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace vs = vibestations;

static std::string tmpDir(const char* tag) {
    char b[256]; std::snprintf(b, sizeof b, "/tmp/vibe-station-store-%s-%d", tag, (int)::getpid());
    std::string cmd = std::string("rm -rf ") + b; if (std::system(cmd.c_str())) {}
    ::mkdir(b, 0755);
    return b;
}
static void rmDir(const std::string& d) { std::string cmd = "rm -rf " + d; if (std::system(cmd.c_str())) {} }
static uint64_t fsize(const std::string& p) { struct stat s{}; return ::stat(p.c_str(), &s) == 0 ? (uint64_t)s.st_size : 0; }
static int count(const std::string& hay, const std::string& needle) { int n = 0; size_t p = 0; while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); } return n; }
/** The number after "key": in a flat JSON answer (the first one). */
static long num(const std::string& j, const std::string& key) { const size_t p = j.find("\"" + key + "\":"); return p == std::string::npos ? -1 : std::atol(j.c_str() + p + key.size() + 3); }

static vs::Reception rx(const char* call, const char* grid, const char* mode, const char* band, int snr, int64_t utc, uint8_t origin = vs::kLive, double hz = 14075000) {
    vs::Reception r; r.call = call; r.grid = grid; r.mode = mode; r.band = band; r.snr = snr; r.utc = utc; r.origin = origin; r.rfHz = hz;
    return r;
}

static const int64_t T0 = 1795000000;   // 2026-11-17-ish, a fixed day for the tests
static int64_t g_now = T0;

static void testValidation() {
    std::printf("── validation ──\n");
    std::string o;
    CHECK(vs::validCall("g4abc", &o) && o == "G4ABC", "lower case call upper-cased");
    CHECK(vs::validCall("EA8/G4ABC/P", &o), "portable call");
    for (const char* bad : { "CQ", "DE", "RR73", "73", "TEST", "AB", "ABCDEF", "123456", "G4AB1", "<script>", "K1ABC\"", "", "A\001B1C" })
        CHECK(!vs::validCall(bad), "'%s' is not a call", bad);
    CHECK(vs::validGrid("io91", &o) && o == "IO91", "4-char grid");
    CHECK(vs::validGrid("IO91wm", &o) && o == "IO91WM", "6-char grid");
    for (const char* bad : { "RR73", "AA00", "SS00", "IO9", "IO91W", "IO91ZZ", "+05", "-12" }) CHECK(!vs::validGrid(bad), "'%s' is not a grid", bad);
    double la, lo;
    CHECK(vs::gridCentre("IO91", la, lo) && std::fabs(la - 51.5) < 1e-9 && std::fabs(lo + 1.0) < 1e-9, "IO91 centre 51.5,-1");
    CHECK(vs::cleanMode("bpsk31") == "PSK31", "BPSK31 → PSK31");
    CHECK(vs::cleanMode("ft8\"x") == "FT8X", "mode allow-list");
    vs::Store s([] { return g_now; }, false);
    CHECK(!s.spot(rx("CQ", "IO91", "FT8", "20m", -10, T0)), "a word is refused");
    CHECK(s.spot(rx("G4ABC", "RR73", "FT8", "20m", -10, T0)), "a call with a non-grid is kept, ungridded");
    const std::string j = s.mapJson(vs::kDaySec, 0, NAN, NAN);
    CHECK(j.find("\"grid\":\"\"") != std::string::npos && j.find("\"lat\"") == std::string::npos, "no grid = no position: %s", j.c_str());
}

static void testRamOnly() {
    std::printf("── RAM only ──\n");
    g_now = T0 + 3600;
    vs::Store s([] { return g_now; }, false);
    s.spot(rx("G4ABC", "IO91", "FT8", "20m", -10, g_now - 100));
    s.spot(rx("G4ABC", "IO91", "FT8", "20m", -3, g_now - 50));
    s.spot(rx("G4ABC", "", "FT4", "20m", -15, g_now - 20));
    s.spot(rx("G4ABC", "", "CW", "40m", 12, g_now - 10, vs::kBackground, 7020000));
    s.spot(rx("K1ABC", "FN42", "WSPR", "30m", -25, g_now - 5, vs::kBackground, 10140100));
    const std::string j = s.mapJson(vs::kDaySec, 0, 51.5, -1.0);
    CHECK(num(j, "total") == 2, "two calls: %s", j.c_str());
    CHECK(count(j, "\"call\":\"G4ABC\"") == 1, "G4ABC once");
    CHECK(j.find("\"modes\":[\"CW\",\"FT4\",\"FT8\"]") != std::string::npos || (j.find("\"FT8\"") != std::string::npos && j.find("\"CW\"") != std::string::npos && j.find("\"FT4\"") != std::string::npos), "its modes listed");
    CHECK(j.find("\"bands\":[") != std::string::npos && j.find("\"40m\"") != std::string::npos, "its bands listed");
    // G4ABC: count 4, best SNR 12 (the CW one), latest mode CW, grid from FT8
    const size_t g = j.find("\"call\":\"G4ABC\"");
    const std::string row = j.substr(g, j.find('}', g) - g);
    CHECK(num(row, "count") == 4 && num(row, "snr") == 12, "count 4, best 12: %s", row.c_str());
    CHECK(row.find("\"mode\":\"CW\"") != std::string::npos && row.find("\"grid\":\"IO91\"") != std::string::npos, "latest mode, the known grid");
    CHECK(row.find("\"km\":0") != std::string::npos, "km from the server");
    CHECK(num(row, "src") == 3, "both origins");
    CHECK(j.find("\"country\":\"England\"") != std::string::npos && j.find("\"country\":\"United States\"") != std::string::npos, "countries");
    CHECK(j.find("\"byMode\":{\"CW\":1,\"WSPR\":1}") != std::string::npos, "byMode by each call's latest mode: %s", j.substr(0, 200).c_str());
}

static void testDisk() {
    std::printf("── the disk, two processes' stores ──\n");
    const std::string d = tmpDir("disk");
    g_now = T0 + 7200;
    vs::Store a([] { return g_now; }, false), b([] { return g_now; }, false), reader([] { return g_now; }, false);
    for (auto* s : { &a, &b, &reader }) s->configure([d] { return d; }, [] { return 100ull << 20; });
    a.spot(rx("G4ABC", "IO91", "FT8", "20m", -10, g_now - 60));
    a.spot(rx("G4ABC", "IO91", "FT8", "20m", -8, g_now - 45));
    b.spot(rx("G4ABC", "IO91", "FT8", "20m", -2, g_now - 30, vs::kBackground));
    b.spot(rx("DL1XYZ", "JO62", "FT8", "20m", -20, g_now - 30, vs::kBackground));
    a.flush(); b.flush();
    const std::string path = a.dayPath(vs::dayOf(g_now));
    CHECK(fsize(path) == vs::kHeader + 2 * vs::kRecSize, "one file, two rows: %llu", (unsigned long long)fsize(path));
    std::string j = reader.mapJson(vs::kDaySec, 0, NAN, NAN);
    size_t g = j.find("\"call\":\"G4ABC\"");
    CHECK(g != std::string::npos && num(j.substr(g), "count") == 3 && num(j.substr(g), "snr") == -2, "merged across processes: %s", j.c_str());
    CHECK(num(j.substr(g), "src") == 3, "both origins on disk");
    // flushing again with nothing new changes nothing; a new spot adds exactly one
    a.flush(); b.flush();
    a.spot(rx("G4ABC", "IO91", "FT8", "20m", -9, g_now - 10));
    j = a.mapJson(vs::kDaySec, 0, NAN, NAN);   // ★ unflushed: laid over the file
    g = j.find("\"call\":\"G4ABC\"");
    CHECK(num(j.substr(g), "count") == 4, "unflushed overlay counted once: %s", j.substr(g, 80).c_str());
    a.flush();
    j = reader.mapJson(vs::kDaySec, 0, NAN, NAN);
    g = j.find("\"call\":\"G4ABC\"");
    CHECK(num(j.substr(g), "count") == 4, "after the flush, still 4");
    j = a.mapJson(vs::kDaySec, 0, NAN, NAN);
    g = j.find("\"call\":\"G4ABC\"");
    CHECK(num(j.substr(g), "count") == 4, "the writer's own view: 4 (no double count after its flush)");
    CHECK(vs::Store::bytesIn(d) == fsize(path), "bytesIn counts the day files");
    // a corrupt file is started again, never trusted
    { FILE* f = std::fopen((d + "/stations/" + vs::dayName(vs::dayOf(g_now) - vs::kDaySec)).c_str(), "wb"); std::fputs("rubbish rubbish rubbish", f); std::fclose(f); }
    j = reader.mapJson(7 * vs::kDaySec, 0, NAN, NAN);
    CHECK(num(j, "total") == 2, "a corrupt day is ignored: %s", j.substr(0, 120).c_str());
    rmDir(d);
}

static void testProcesses() {
    std::printf("── four processes flushing one day ──\n");
    const std::string d = tmpDir("procs");
    std::vector<pid_t> kids;
    for (int p = 0; p < 4; ++p) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            vs::Store s([] { return T0 + 100; }, false);
            s.configure([d] { return d; }, [] { return 100ull << 20; });
            for (int round = 0; round < 20; ++round) {
                for (int i = 0; i < 50; ++i) {
                    char c[16]; std::snprintf(c, sizeof c, "G%dAB%c", i % 10, 'A' + i / 10);
                    s.spot(rx(c, "IO91", "FT8", "20m", -10 + p, T0 + 50, p == 0 ? vs::kLive : vs::kBackground));
                }
                s.flush();
            }
            std::_Exit(0);
        }
        kids.push_back(pid);
    }
    for (pid_t k : kids) { int st = 0; ::waitpid(k, &st, 0); }
    g_now = T0 + 100;
    vs::Store r([] { return g_now; }, false);
    r.configure([d] { return d; }, [] { return 100ull << 20; });
    const std::string j = r.mapJson(vs::kDaySec, 0, NAN, NAN);
    CHECK(num(j, "total") == 50, "50 stations once each: %ld", num(j, "total"));
    const std::string path = r.dayPath(vs::dayOf(T0));
    CHECK(fsize(path) == vs::kHeader + 50 * vs::kRecSize, "50 rows on disk");
    // every station heard 4 processes × 20 rounds = 80 times
    bool all80 = true; size_t p = j.find("\"stations\"");
    while ((p = j.find("\"count\":", p)) != std::string::npos) { if (std::atol(j.c_str() + p + 8) != 80) all80 = false; p += 8; }
    CHECK(all80, "every count exactly 80 (deltas under the lock)");
    CHECK(j.find("\"snr\":-7") != std::string::npos, "best SNR across the processes");
    rmDir(d);
}

static void testWindows() {
    std::printf("── days and windows ──\n");
    const std::string d = tmpDir("win");
    const int64_t today = vs::dayOf(T0) + 5 * vs::kDaySec;
    g_now = today + 12 * 3600;   // noon
    vs::Store s([] { return g_now; }, false);
    s.configure([d] { return d; }, [] { return 100ull << 20; });
    s.spot(rx("G1AAA", "IO91", "FT8", "20m", -1, g_now - 600));                  // 10 min ago
    s.spot(rx("G2BBB", "IO92", "FT8", "20m", -1, g_now - 5 * 3600));             // today 07:00
    s.flush();
    // yesterday and 3 days ago and 20 days ago and 100 days ago: written by a store whose clock is then
    for (int64_t back : { (int64_t)vs::kDaySec, 3 * vs::kDaySec, 20 * vs::kDaySec, 100 * vs::kDaySec }) {
        const int64_t then = g_now - back;
        vs::Store o([then] { return then; }, false);
        o.configure([d] { return d; }, [] { return 100ull << 20; });
        char c[16]; std::snprintf(c, sizeof c, "M%lldX", (long long)(back / vs::kDaySec) % 10);
        std::string call = std::string("M0") + (char)('A' + (back / vs::kDaySec) % 26) + "BC";
        o.spot(rx(call.c_str(), "IO80", "JS8", "40m", -5, then, vs::kBackground, 7078000));
        o.flush();
    }
    auto total = [&](const char* w) { return num(s.mapJson(vs::windowSec(w), 0, NAN, NAN), "total"); };
    CHECK(total("1h") == 1, "1 h: one (%ld)", total("1h"));
    CHECK(total("24h") == 3, "24 h: today's two + yesterday's (%ld)", total("24h"));
    CHECK(total("7d") == 4, "7 d: + 3 days ago (%ld)", total("7d"));
    CHECK(total("30d") == 5, "30 d: + 20 days ago (%ld)", total("30d"));
    CHECK(total("all") == 6, "all: + 100 days ago (%ld)", total("all"));
    // the cached past days: a second answer reads only today's file
    (void)s.mapJson(vs::windowSec("30d"), 0, NAN, NAN);   // (the cache holds one window: this one fills it)
    (void)s.mapJson(vs::windowSec("30d"), 0, NAN, NAN);
    CHECK(s.lastRowsRead() == 2, "a repeat 30 d answer re-reads only today (%zu rows)", s.lastRowsRead());
    // a live poll: only what changed since
    const std::string live = s.mapJson(vs::windowSec("30d"), g_now - 900, NAN, NAN);
    CHECK(num(live, "count") == 1 && num(live, "total") == 5, "changedSince: 1 of 5: %s", live.substr(0, 120).c_str());
    // forEachSince: rows since yesterday's start
    int n = 0; bool order = true; int64_t lastDay = INT64_MAX;
    s.forEachSince(g_now - 2 * vs::kDaySec, [&](const vs::Station& st) { ++n; if (st.day > lastDay) order = false; lastDay = st.day; return true; });
    CHECK(n == 3 && order, "forEachSince 2 days: 3 rows, newest day first (%d)", n);
    n = 0;
    s.forEachSince(0, [&](const vs::Station&) { return ++n < 2; });
    CHECK(n == 2, "forEachSince stops when told");
    rmDir(d);
}

static void testBounds() {
    std::printf("── bounded ──\n");
    const std::string d = tmpDir("bound");
    g_now = T0 + 3600;
    vs::Store s([] { return g_now; }, false);
    s.configure([d] { return d; }, [] { return 100ull << 20; });
    int refused = 0;
    for (uint32_t i = 0; i < vs::dayCap() + 500; ++i) {
        char c[16]; std::snprintf(c, sizeof c, "K%uA%c%c", i % 10, 'A' + (i / 10) % 26, 'A' + (i / 260) % 26);
        std::string call = c; call += (char)('A' + (i / 6760) % 26);
        if (!s.spot(rx(call.c_str(), "FN42", "FT8", "20m", -10, g_now - 10))) ++refused;
    }
    s.flush();
    const std::string path = s.dayPath(vs::dayOf(g_now));
    CHECK(refused == 500, "the RAM table refuses past the day's cap (%d)", refused);
    CHECK(fsize(path) <= vs::kHeader + (uint64_t)vs::dayCap() * vs::kRecSize, "the day's file ≤ 196 KB (%llu)", (unsigned long long)fsize(path));
    // a second process's new stations: dropped on disk, counted
    vs::Store b([] { return g_now; }, false);
    b.configure([d] { return d; }, [] { return 100ull << 20; });
    b.spot(rx("ZL1ZZZ", "RF70", "FT8", "20m", -10, g_now - 5));
    b.flush();
    CHECK(fsize(path) == vs::kHeader + (uint64_t)vs::dayCap() * vs::kRecSize, "still full, never past the cap");
    std::vector<vs::Station> v; uint32_t dropped = 0;
    vs::readDay(path, vs::dayOf(g_now), v, &dropped);
    CHECK(v.size() == vs::dayCap() && dropped == 1, "the header counts the dropped one (%u)", dropped);
    const std::string j = s.mapJson(vs::kDaySec, 0, NAN, NAN, 1000);
    CHECK(num(j, "count") == 1000 && num(j, "truncated") == 1, "maxOut caps the answer");
    // pruning: a gallery of 16 MB → a 1 MB budget; 10 full days (~1.9 MB) prune down
    rmDir(d); ::mkdir(d.c_str(), 0755);
    for (int day = 10; day >= 1; --day) {
        const int64_t then = vs::dayOf(T0) - day * vs::kDaySec + 3600;
        vs::Store o([then] { return then; }, false);
        o.configure([d] { return d; }, [] { return 16ull << 20; });
        for (uint32_t i = 0; i < vs::dayCap(); ++i) { char c[16]; std::snprintf(c, sizeof c, "W%uA%c%c", i % 10, 'A' + (i / 10) % 26, 'A' + (i / 260) % 26); o.spot(rx(c, "", "CW", "40m", 5, then)); }
        o.flush();
    }
    CHECK(vs::Store::bytesIn(d) <= vs::budgetFor(16ull << 20), "pruned to the budget (%llu ≤ %llu)", (unsigned long long)vs::Store::bytesIn(d), (unsigned long long)vs::budgetFor(16ull << 20));
    CHECK(vs::Store::bytesIn(d) > 0, "the newest days kept");
    rmDir(d);
}

static void testSubscribe() {
    std::printf("── subscribe (the PSK Reporter hook) ──\n");
    g_now = T0 + 3600;
    vs::Store s([] { return g_now; }, false);
    std::vector<vs::Reception> got;
    const int id = s.subscribe([&](const vs::Reception& r) { got.push_back(r); });
    s.spot(rx("g4abc", "io91", "FT8", "20m", -10, g_now - 15));
    s.spot(rx("G4ABC", "IO91", "FT8", "20m", -11, g_now));
    s.spot(rx("G4ABC", "IO91", "FT4", "20m", -11, g_now));
    s.spot(rx("CQ", "IO91", "FT8", "20m", -11, g_now));
    CHECK(got.size() == 3, "three accepted receptions (%zu)", got.size());
    CHECK(got.size() == 3 && got[0].newToday && !got[1].newToday && got[2].newToday, "newToday once per call+mode+band");
    CHECK(got.size() == 3 && got[0].call == "G4ABC" && got[0].grid == "IO91" && got[0].utc == g_now - 15 && got[0].rfHz == 14075000, "cleaned fields, the slot time, the RF");
    s.unsubscribe(id);
    s.spot(rx("G4ABC", "IO91", "FT8", "20m", -11, g_now));
    CHECK(got.size() == 3, "unsubscribed");
    s.setEnabled(false);
    CHECK(!s.spot(rx("K1ABC", "FN42", "FT8", "20m", -11, g_now)), "switched off: nothing kept");
    CHECK(s.mapJson(3600, 0, NAN, NAN).find("\"off\":1") != std::string::npos, "switched off: the map says so");
}

static void testCost() {
    std::printf("── cost ──\n");
    const std::string d = tmpDir("cost");
    g_now = T0 + 3600;
    vs::Store s([] { return g_now; }, false);
    s.configure([d] { return d; }, [] { return 100ull << 20; });
    const auto t0 = std::chrono::steady_clock::now();
    const int N = 200000;
    for (int i = 0; i < N; ++i) {
        char c[16]; std::snprintf(c, sizeof c, "K%dA%c%c", i % 10, 'A' + (i / 10) % 26, 'A' + (i / 260) % 6);
        s.spot(rx(c, "FN42", (i & 1) ? "FT8" : "FT4", "20m", -10 + i % 20, g_now - 10));
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    std::printf("   %d spots in %.1f ms (%.2f µs a spot)\n", N, us / 1000, us / N);
    CHECK(us / N < 50, "a spot costs < 50 µs");
    const auto t1 = std::chrono::steady_clock::now();
    s.flush();
    const double fms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("   flush of %u rows: %.1f ms\n", vs::dayCap(), fms);
    const auto t2 = std::chrono::steady_clock::now();
    const std::string j = s.mapJson(30 * vs::kDaySec, 0, 51.5, -1.0);
    const double mms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
    std::printf("   30-day map answer: %zu bytes, %.1f ms\n", j.size(), mms);
    CHECK(fms < 500 && mms < 500, "flush and answer well under half a second on this machine");
    rmDir(d);
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--write") {
        // ★ the map tests' fixture: today's stations in <galleryDir>/stations, as a server's store keeps them
        vs::Store s(nullptr, false);
        s.configure([argv] { return std::string(argv[2]); }, [] { return 100ull << 20; });
        const int64_t now = (int64_t)std::time(nullptr);
        s.spot(rx("K1ABC", "FN42", "FT8", "20m", -12, now - 300, vs::kBackground, 14075600));
        s.spot(rx("VK2XYZ", "QF56", "FT8", "20m", -21, now - 200, vs::kBackground, 14076100));
        s.spot(rx("JA1ZZZ", "PM95", "WSPR", "30m", -27, now - 1800, vs::kBackground, 10140150));
        s.spot(rx("DL1ABC", "JO62", "CW", "40m", 18, now - 60, vs::kLive, 7021000));
        s.spot(rx("DL1ABC", "JO62", "FT4", "40m", -5, now - 90, vs::kLive, 7047500));
        s.spot(rx("EA8ZZ", "IL18", "PSK31", "20m", 9, now - 120, vs::kLive, 14070500));
        s.flush();
        std::printf("wrote 6 receptions (5 stations) to %s/stations\n", argv[2]);
        return 0;
    }
    std::printf("test-station-store — the station store for the DIGI MODES MAP\n");
    // ★★ beta91: the cap follows the machine's memory (Stuart's Pi 500 filled 3072 by mid-morning and refused 49,331 spots)
    CHECK(vs::dayCapForRam(256ull << 20) == 3072 && vs::dayCapForRam(512ull << 20) == 3072, "256/512 MB boards keep 3072 rows a day");
    CHECK(vs::dayCapForRam(1ull << 30) == 8192 && vs::dayCapForRam(2ull << 30) == 16384, "1 GB → 8192, 2 GB → 16384");
    CHECK(vs::dayCapForRam(8ull << 30) == 32768, "a Pi 500's 8 GB → 32768 rows (2 MB a day)");
    CHECK(vs::dayCap() >= 3072, "this machine's cap (%u)", vs::dayCap());
    vs::setDayCapForTest(3072);   // the bounds tests below model the smallest boards
    testValidation();
    testRamOnly();
    testDisk();
    testProcesses();
    testWindows();
    testBounds();
    testSubscribe();
    testCost();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
