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
//  9. beta92 WHICH RADIO (vibe_log_radio.h): two PROCESSES (fork), two radio ids, one station heard by both → one row
//     listing both; a station only one heard lists one; the registry gives each id one bit; the radio= filter; an OLD
//     file (pad bytes 0) reads as unknown and passes every filter; the "radios" list names the radios as the machine does.
// 10. ★ beta92 THE FALSE-DECODE FILTER (vibe_callsign_entity.h, docs/v12/DIGI-MAP-FILTER.md): Stuart's Pi 500 false
//     decodes — no administration → never kept, counted "falseToday"; an impossible grid → kept, unplaced, counted
//     "unlocatedToday"; across a flush and a second process; a day file written BEFORE the filter (ver 1) is cleaned on
//     read and on its next write.
//  8. Cost: spots per second into the RAM table; a flush of a full day; a 30-day map answer (cached past days).
//  `--write <galleryDir>` writes a small fixture (today's stations) for the web/app map tests and exits.
#include "vibe_station_store.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
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

/* ★★ beta92 SPOTS SENT (Stuart 11-23: "15000 spots from 3200 transmitters"): reports sent to PSK Reporter, per window,
 *  shared by every radio's process through stations/sent.dat. */
static void testSpotsSent() {
    std::printf("── spots sent (PSK Reporter reports, per window, every radio) ──\n");
    const std::string d = tmpDir("sent");
    std::vector<pid_t> kids;
    for (int p = 0; p < 3; ++p) {   // ★ three radios' processes, each sending batches, flushing into one file
        const pid_t pid = ::fork();
        if (pid == 0) {
            vs::Store s([] { return T0 + 7200; }, false);
            s.configure([d] { return d; }, [] { return 100ull << 20; });
            for (int b = 0; b < 10; ++b) { s.noteSent(T0 + 7200 - 30 * 60 + b * 60, 10); s.flush(); }   // last half hour: 100
            s.noteSent(T0 + 7200 - 3 * 3600, 7);                     // 3 h ago (outside 1H, inside 24H)
            s.noteSent(T0 + 7200 - 3 * vs::kDaySec, 5);              // 3 days ago (7D and ALL only)
            s.flush();
            std::_Exit(0);
        }
        kids.push_back(pid);
    }
    for (pid_t k : kids) { int st = 0; ::waitpid(k, &st, 0); }
    g_now = T0 + 7200;
    vs::Store r([] { return g_now; }, false);
    r.configure([d] { return d; }, [] { return 100ull << 20; });
    CHECK(r.sentSince(g_now - 3600) == 300, "1H: 3 radios x 100 = 300 (%llu)", (unsigned long long)r.sentSince(g_now - 3600));
    CHECK(r.sentSince(g_now - vs::kDaySec) == 321, "24H: + 3 x 7 = 321 (%llu)", (unsigned long long)r.sentSince(g_now - vs::kDaySec));
    CHECK(r.sentSince(vs::dayOf(g_now - 7 * vs::kDaySec)) == 336, "7D: + 3 x 5 = 336 (%llu)", (unsigned long long)r.sentSince(vs::dayOf(g_now - 7 * vs::kDaySec)));
    CHECK(r.sentSince(0) == 336, "ALL: 336");
    r.noteSent(g_now - 60, 4);   // this process's, not yet flushed
    CHECK(r.sentSince(g_now - 3600) == 304, "an unflushed batch counts at once (304)");
    const std::string j = r.mapJson(3600, 0, NAN, NAN);
    CHECK(num(j, "spotsSent") == 304, "the map's answer carries spotsSent for its window: %ld", num(j, "spotsSent"));
    CHECK(fsize(d + "/stations/sent.dat") > 0 && fsize(d + "/stations/sent.dat") < 8192, "sent.dat is small and fixed (%lld B)", (long long)fsize(d + "/stations/sent.dat"));
    // ★ the ring reuses a slot: a bucket two days and more later replaces the old one, never adds to it
    r.flush();
    g_now = T0 + 7200 + 400 * 600;   // 400 buckets later (the fine ring is 288)
    r.noteSent(g_now - 60, 1); r.flush();
    CHECK(r.sentSince(g_now - 3600) == 1, "two days on, the 1H window holds only the new batch (%llu)", (unsigned long long)r.sentSince(g_now - 3600));
    // ★ no gallery (RAM only): still counted, still bounded
    vs::Store m([] { return g_now; }, false);
    for (int i = 0; i < 5000; ++i) m.noteSent(g_now - (int64_t)i * 600, 1);
    { const uint64_t h = m.sentSince(g_now - 3600);
      CHECK(h == 6 || h == 7, "RAM only: the last hour's batches, to the 10-minute bucket (%llu)", (unsigned long long)h); }
    CHECK(m.sentSince(0) <= 5000 && m.sentSince(0) >= 280, "RAM only: bounded — the fine ring keeps two days, the days kKeepDays");
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

static void testRadios() {
    std::printf("── beta92: which radio heard it (two processes) ──\n");
    namespace lr = vibe::logradio;
    const std::string d = tmpDir("radios");
    const char* ids[2] = { "aa11bb22", "cc33dd44" };
    std::vector<pid_t> kids;
    for (int p = 0; p < 2; ++p) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            lr::setMine(ids[p]);
            vs::Store s([] { return T0 + 100; }, false);
            s.configure([d] { return d; }, [] { return 100ull << 20; });
            for (int round = 0; round < 10; ++round) {
                s.spot(rx("G4BOTH", "IO91", "FT8", "20m", -10 + p, T0 + 50));       // both radios hear it
                s.spot(rx(p == 0 ? "G4ONEA" : "M0TWOB", "IO92", "FT8", "20m", -5, T0 + 60));   // one each
                s.flush();
            }
            std::_Exit(0);
        }
        kids.push_back(pid);
    }
    for (pid_t k : kids) { int st = 0; ::waitpid(k, &st, 0); }
    const std::vector<std::string> reg = lr::readRegistry(d);
    CHECK(reg.size() == 2 && reg[0] != reg[1], "the registry: two ids, one bit each (%zu)", reg.size());
    lr::setMine("");
    lr::setMachineRadios([] { return std::vector<lr::Named>{ { "aa11bb22", "RSP1A HF" }, { "cc33dd44", "RTL VHF" } }; });
    g_now = T0 + 100;
    vs::Store r([] { return g_now; }, false);
    r.configure([d] { return d; }, [] { return 100ull << 20; });
    std::string j = r.mapJson(vs::kDaySec, 0, NAN, NAN);
    CHECK(num(j, "total") == 3, "three stations, one row each (%ld)", num(j, "total"));
    size_t g = j.find("\"call\":\"G4BOTH\"");
    std::string row = g == std::string::npos ? "" : j.substr(g, j.find('}', g) - g);
    CHECK(num(row, "count") == 20, "G4BOTH counted 20 (both processes): %s", row.c_str());
    CHECK(row.find("aa11bb22") != std::string::npos && row.find("cc33dd44") != std::string::npos, "G4BOTH lists both radios: %s", row.c_str());
    g = j.find("\"call\":\"G4ONEA\"");
    row = g == std::string::npos ? "" : j.substr(g, j.find('}', g) - g);
    CHECK(row.find("\"radios\":[\"aa11bb22\"]") != std::string::npos, "G4ONEA lists only its radio: %s", row.c_str());
    CHECK(j.find("{\"id\":\"aa11bb22\",\"name\":\"RSP1A HF\",\"n\":2}") != std::string::npos
          && j.find("{\"id\":\"cc33dd44\",\"name\":\"RTL VHF\",\"n\":2}") != std::string::npos, "the radios list, named, with counts: %s", j.substr(0, 400).c_str());
    std::string f = r.mapJson(vs::kDaySec, 0, NAN, NAN, vs::kMaxOut, "cc33dd44");
    CHECK(count(f, "\"call\":") == 2 && f.find("G4ONEA") == std::string::npos && f.find("M0TWOB") != std::string::npos && f.find("G4BOTH") != std::string::npos,
          "radio=cc33dd44: G4BOTH + M0TWOB only: %s", f.c_str());
    CHECK(f.find("\"radio\":\"cc33dd44\"") != std::string::npos, "the answer says the filter");
    f = r.mapJson(vs::kDaySec, 0, NAN, NAN, vs::kMaxOut, "../etc");
    CHECK(count(f, "\"call\":") == 3, "a junk radio= is ignored (all)");
    // ★ the OLD FILE: a beta90/91 record has 0 in the mask's bytes → unknown, shown under every radio
    {
        const std::string path = r.dayPath(vs::dayOf(T0));
        const int fd = ::open(path.c_str(), O_RDWR);
        vs::Hdr h{}; CHECK(::pread(fd, &h, sizeof h, 0) == (ssize_t)sizeof h, "read header");
        vs::Rec rec{}; std::memset(&rec, 0, sizeof rec);
        vs::putStr(rec.call, 16, "OH2OLD"); vs::putStr(rec.grid, 8, "KP20"); vs::putStr(rec.mode, 8, "FT8"); vs::putStr(rec.band, 8, "20m");
        rec.first = rec.last = (uint32_t)(T0 + 70); rec.count = 3; rec.best = 1; rec.lastSnr = 1; rec.origins = 1; rec.ver = 1;
        CHECK(::pwrite(fd, &rec, sizeof rec, vs::kHeader + (off_t)h.n * vs::kRecSize) == (ssize_t)sizeof rec, "append an old-style row");
        h.n += 1; CHECK(::pwrite(fd, &h, sizeof h, 0) == (ssize_t)sizeof h, "header");
        ::close(fd);
    }
    j = r.mapJson(vs::kDaySec, 0, NAN, NAN);
    g = j.find("\"call\":\"OH2OLD\"");
    row = g == std::string::npos ? "" : j.substr(g, j.find('}', g) - g);
    CHECK(!row.empty() && row.find("\"radios\"") == std::string::npos, "an old row reads, with no radios (unknown): %s", row.c_str());
    CHECK(num(j, "radioUnknown") == 1, "the unknown are counted: %ld", num(j, "radioUnknown"));
    f = r.mapJson(vs::kDaySec, 0, NAN, NAN, vs::kMaxOut, "aa11bb22");
    CHECK(f.find("OH2OLD") != std::string::npos && f.find("M0TWOB") == std::string::npos, "an unknown row passes every filter");
    // ★ one radio only: the list has one entry (the clients then draw no filter)
    lr::setMachineRadios(nullptr);
    rmDir(d);
    {
        const std::string d1 = tmpDir("radio1");
        lr::setMine("aa11bb22");
        vs::Store s([] { return T0 + 100; }, false);
        s.configure([d1] { return d1; }, [] { return 100ull << 20; });
        s.spot(rx("G4ABC", "IO91", "FT8", "20m", -1, T0 + 10));
        const std::string before = s.mapJson(vs::kDaySec, 0, NAN, NAN);   // unflushed, bit not yet resolved
        s.flush();
        const std::string after = s.mapJson(vs::kDaySec, 0, NAN, NAN);
        CHECK(after.find("\"radios\":[{\"id\":\"aa11bb22\",\"name\":\"aa11bb22\",\"n\":1}]") != std::string::npos, "one radio listed (by id, no names known): %s", after.substr(0, 300).c_str());
        CHECK(after.find("\"radios\":[\"aa11bb22\"]") != std::string::npos, "a row spotted before the bit was known says who heard it");
        (void)before;
        lr::setMine("");
        rmDir(d1);
    }
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
    // ★★ beta91: every reception today is counted, kept or not — 3072 kept + 500 refused in RAM + 1 refused on disk
    {
        uint32_t ds = 0; std::vector<vs::Station> w; vs::readDay(path, vs::dayOf(g_now), w, nullptr, &ds);
        CHECK(ds == 501, "the header counts the refused RECEPTIONS, RAM and disk (%u)", ds);
        CHECK(num(j, "notKeptToday") == 501, "the map says how many were not kept (%ld)", num(j, "notKeptToday"));
        CHECK(num(j, "decodesToday") == (double)vs::dayCap() + 501, "…and every decode today, kept or not (%ld)", num(j, "decodesToday"));
        s.spot(rx("K1NEW", "FN42", "FT8", "20m", -10, g_now - 1));   // one more refused, not yet flushed: counted at once
        const std::string j2 = s.mapJson(vs::kDaySec, 0, NAN, NAN, 1000);
        CHECK(num(j2, "notKeptToday") == 502 && num(j2, "decodesToday") == (double)vs::dayCap() + 502, "an unflushed refusal counts too");
        s.flush();
        const std::string j3 = s.mapJson(vs::kDaySec, 0, NAN, NAN, 1000);
        CHECK(num(j3, "notKeptToday") == 502, "…once, after its flush (%ld)", num(j3, "notKeptToday"));
    }
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

static void testFilter() {
    std::printf("── the false-decode filter (beta92) ──\n");
    const std::string d = tmpDir("filter");
    g_now = T0 + 3600;
    vs::Store a([] { return g_now; }, false), b([] { return g_now; }, false);
    for (auto* s : { &a, &b }) s->configure([d] { return d; }, [] { return 100ull << 20; });
    std::vector<vs::Reception> heard;
    a.subscribe([&](const vs::Reception& r) { heard.push_back(r); });
    // Stuart's Pi 500 map, 2026-11-22
    CHECK(!a.spot(rx("132XQA", "PC46", "FT8", "20m", -20, g_now - 60, vs::kBackground)), "132XQA is not a station");
    CHECK(!a.spot(rx("1H7IJH", "EB72", "FT8", "20m", -20, g_now - 60, vs::kBackground)), "1H7IJH is not a station");
    CHECK(a.spot(rx("7Q8FAZ", "OB45", "FT8", "20m", -20, g_now - 50, vs::kBackground)), "7Q8FAZ is kept…");
    CHECK(a.spot(rx("YE9HMP", "PC24", "FT8", "20m", -20, g_now - 50, vs::kBackground)), "YE9HMP is kept…");
    CHECK(b.spot(rx("N6LCO", "OC08", "FT8", "20m", -20, g_now - 40, vs::kBackground)), "N6LCO is kept…");
    CHECK(b.spot(rx("FQ2HMC", "PD93", "FT8", "20m", -20, g_now - 40, vs::kBackground)), "FQ2HMC is kept…");
    // real ones, untouched
    CHECK(a.spot(rx("VK0EK", "MD66", "FT8", "20m", -18, g_now - 30)) && a.spot(rx("KC4AAA", "RB32", "FT8", "20m", -18, g_now - 30)), "Heard Island and McMurdo");
    CHECK(b.spot(rx("3Y0J", "JD15", "FT8", "20m", -18, g_now - 30)) && b.spot(rx("G4ABC/MM", "GG00", "FT8", "20m", -18, g_now - 30)), "Bouvet and a /MM mid-ocean");
    CHECK(heard.size() == 4, "the PSK Reporter hook hears only the kept (%zu)", heard.size());
    CHECK(heard.size() >= 2 && heard[0].call == "7Q8FAZ" && heard[0].grid.empty() && heard[1].grid.empty(), "…and without the impossible grid");
    auto rowOf = [](const std::string& j, const char* call) {
        const size_t g = j.find(std::string("\"call\":\"") + call + "\"");
        return g == std::string::npos ? std::string() : j.substr(g, j.find('}', g) - g);
    };
    auto checkMap = [&](const std::string& j, const char* when) {
        CHECK(j.find("132XQA") == std::string::npos && j.find("1H7IJH") == std::string::npos, "%s: no false call on the map", when);
        for (const char* c : { "7Q8FAZ", "YE9HMP", "N6LCO", "FQ2HMC" }) {
            const std::string r = rowOf(j, c);
            CHECK(!r.empty() && r.find("\"grid\":\"\"") != std::string::npos && r.find("\"lat\"") == std::string::npos, "%s: %s kept, unplaced: %s", when, c, r.c_str());
        }
        for (const char* c : { "VK0EK", "KC4AAA", "3Y0J", "G4ABC/MM" }) CHECK(rowOf(j, c).find("\"lat\"") != std::string::npos, "%s: %s placed", when, c);
        CHECK(rowOf(j, "7Q8FAZ").find("\"country\":\"Malawi\"") != std::string::npos, "%s: the country", when);
        CHECK(rowOf(j, "3Y0J").find("\"country\":\"Norway\"") != std::string::npos, "%s: a country the display table lacks, from the ITU table", when);
    };
    std::string j = a.mapJson(vs::kDaySec, 0, NAN, NAN);   // a's RAM + nothing on disk yet (b's rows not visible: unflushed)
    CHECK(num(j, "falseToday") == 2 && num(j, "unlocatedToday") == 2, "unflushed counts: false %ld, unlocated %ld", num(j, "falseToday"), num(j, "unlocatedToday"));
    a.flush(); b.flush();
    vs::Store r([] { return g_now; }, false);
    r.configure([d] { return d; }, [] { return 100ull << 20; });
    j = r.mapJson(vs::kDaySec, 0, NAN, NAN);
    checkMap(j, "after the flush");
    CHECK(num(j, "falseToday") == 2 && num(j, "unlocatedToday") == 4, "on disk, both radios: false %ld, unlocated %ld", num(j, "falseToday"), num(j, "unlocatedToday"));
    CHECK(num(j, "total") == 8 && num(j, "decodesToday") == 8, "8 stations, 8 decodes (the false ones are not decodes of anyone): %ld / %ld", num(j, "total"), num(j, "decodesToday"));
    // ★ a day file written BEFORE the filter (ver 1 rows, beta90/91): cleaned on read, and on its next write
    {
        const int64_t day = vs::dayOf(g_now) - vs::kDaySec;
        const std::string path = d + "/stations/" + vs::dayName(day);
        vs::Hdr h{}; h.magic = vs::kMagic; h.recSize = vs::kRecSize; h.day = day;
        std::vector<vs::Rec> recs;
        auto old = [&](const char* call, const char* grid) {
            vs::Station s; s.call = call; s.grid = grid; s.mode = "FT8"; s.band = "20m"; s.count = 3; s.first = day + 100; s.last = day + 200;
            vs::Rec rr = vs::toRec(s); rr.ver = 1; recs.push_back(rr);
        };
        old("132XQA", "PC46"); old("1H7IJH", "EB72"); old("7Q8FAZ", "OB45"); old("YE9HMP", "PC24"); old("N6LCO", "OC08"); old("FQ2HMC", "PD93");
        old("DL1ABC", "JO62"); old("ZL1ABC", "RF73");
        h.n = (uint32_t)recs.size();
        FILE* f = std::fopen(path.c_str(), "wb");
        std::fwrite(&h, sizeof h, 1, f); std::fwrite(recs.data(), sizeof(vs::Rec), recs.size(), f); std::fclose(f);
        std::vector<vs::Station> v;
        CHECK(vs::readDay(path, day, v) && v.size() == 6, "an old file reads 6 of its 8 rows (%zu)", v.size());
        vs::Store o([] { return g_now; }, false);
        o.configure([d] { return d; }, [] { return 100ull << 20; });
        const std::string jw = o.mapJson(2 * vs::kDaySec, 0, NAN, NAN);
        CHECK(jw.find("132XQA") == std::string::npos && rowOf(jw, "DL1ABC").find("\"lat\"") != std::string::npos, "the 48 h map: old false rows gone, real ones placed");
        CHECK(rowOf(jw, "N6LCO").find("\"lat\"") == std::string::npos, "the 48 h map: N6LCO unplaced on both days: %s", rowOf(jw, "N6LCO").c_str());
        // its next write (a late spot for yesterday from a radio) keeps the old row's grid off
        const int64_t keep = g_now; g_now = day + vs::kDaySec - 10;
        vs::Store late([] { return g_now; }, false);
        late.configure([d] { return d; }, [] { return 100ull << 20; });
        late.spot(rx("N6LCO", "", "FT8", "20m", -10, day + 300));
        late.flush();
        g_now = keep;
        std::vector<vs::Rec> raw(8); vs::Hdr h2{};
        f = std::fopen(path.c_str(), "rb"); CHECK(f && std::fread(&h2, sizeof h2, 1, f) == 1 && std::fread(raw.data(), sizeof(vs::Rec), 8, f) == 8, "re-read");
        if (f) std::fclose(f);
        bool found = false;
        for (const auto& rr : raw) if (std::string(rr.call) == "N6LCO") { found = true; CHECK(rr.grid[0] == 0 && rr.ver == 2 && rr.count == 4, "rewritten row: grid dropped, ver 2, count 4 (%s, %u, %u)", rr.grid, rr.ver, rr.count); }
        CHECK(found, "N6LCO row on disk");
    }
    rmDir(d);
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
    testSpotsSent();
    testRadios();
    testWindows();
    testBounds();
    testSubscribe();
    testFilter();
    testCost();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
