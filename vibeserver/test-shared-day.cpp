// test-shared-day.cpp — V12 beta92 ONE DAY ITEM PER SERVER (vibe_gallery_shared.h; docs/v12/LOG-RADIO-FILTER.md § Shared
// day items). Stuart, 2026-11-22: "all radios on same server should feed the same log"; only the DX Log is per radio.
//
// For each decoder's day item, TWO PROCESSES (fork) — two radios, two radio ids, one gallery folder — write the same day:
//  · one item, not two; the message both radios heard is in it ONCE (dedupe), each radio's own messages are in it;
//  · its index line's "ri" lists both radios (vibe_log_radio.h); the card / title counts are the server's, deduped.
//  0. the helpers: newLines / dedupeLines (same text within 20 s = the same reception; later = a new one), readTail.
//  1. AIS / APRS (vibe_station_log.h DayLog): the day's log and the summary card from the shared state.
//  2. DSC (vibe_dsc_gallery.h): the day's log per frequency; a distress alert both radios decoded is ONE item.
//  3. NDB (vibe_ndb_gallery.h): the day's heard log merges both radios' beacons (the best SNR, the first and last).
#include "vibe_gallery_shared.h"
#include "vibe_station_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace vibe;
static int g_pass = 0, g_fail = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (c) g_pass++; else g_fail++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/shday-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static void rmTree(const std::string& d) { std::string c = "rm -rf '" + d + "'"; if (std::system(c.c_str())) {} }
static size_t countOf(const std::string& s, const std::string& n) { size_t c = 0; for (size_t at = 0; (at = s.find(n, at)) != std::string::npos; at += n.size()) c++; return c; }
static const char* const kIds[2] = { "aa11bb22", "cc33dd44" };
static const int64_t T0 = 1795000000 / 86400 * 86400 + 12 * 3600;   // a noon, UTC

/** Run `fn(p)` in two forked processes (radio ids kIds[p]) and wait. False if either failed. */
static bool twoRadios(const std::function<void(int)>& fn) {
    std::vector<pid_t> kids;
    for (int p = 0; p < 2; p++) {
        const pid_t pid = ::fork();
        if (pid == 0) { logradio::setMine(kIds[p]); fn(p); std::_Exit(0); }
        kids.push_back(pid);
    }
    bool clean = true;
    for (pid_t k : kids) { int st = 0; ::waitpid(k, &st, 0); if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) clean = false; }
    return clean;
}
/** Every live item of `kind`: name → text. */
static std::vector<std::pair<std::string, std::string>> items(gallery::Store& s, const std::string& kind) {
    std::vector<std::pair<std::string, std::string>> v;
    for (const auto& n : s.names(kind)) { std::string b; s.readItem(n, b); v.push_back({ n, b }); }
    return v;
}
static bool bothRadios(gallery::Store& s, const std::string& name) {
    const std::string ri = s.field(name, "ri");
    return ri.find(kIds[0]) != std::string::npos && ri.find(kIds[1]) != std::string::npos;
}

static void testHelpers() {
    std::printf("── the helpers ──\n");
    namespace sh = gallery::sharedday;
    const std::string tail = "AIS · head\n\n12:00:05  ship A at 50N\n12:00:30  ship B\n";
    const auto k = sh::newLines(tail, { "12:00:06  ship A at 50N", "12:00:40  ship A at 50N", "12:00:31  ship B", "12:00:55  ship B",
                                         "12:01:00  ship C", "12:01:02  ship C", "no time here" });
    ok(k == std::vector<bool>({ false, true, false, true, true, true, true }),
       "the same text within 20 s of another radio's line is one reception; later is new; one batch never dedupes itself; untimed kept");
    const std::vector<std::string> own = { "12:00:05  ship A at 50N" };
    ok(sh::newLines(tail, { "12:00:06  ship A at 50N" }, 20, &own) == std::vector<bool>({ true }),
       "a line THIS radio wrote is not another radio's: a real repeat is kept");
    ok(sh::newLines("12:00:05  x\n12:00:07  x\n", { "12:00:06  x", "12:00:08  x", "12:00:09  x" }) == std::vector<bool>({ false, false, true }),
       "each line of the tail answers for one of ours (two polls both radios heard = two lines)");
    size_t gone = 0;
    const std::string d = sh::dedupeLines(tail, "12:00:04  ship A at 50N\n12:02:00  ship D\n", 20, &gone);
    ok(d == "12:02:00  ship D\n" && gone == 1, "dedupeLines: " + d);
    ok(sh::lineSecs("23:59:59  x") == 86399 && sh::lineSecs("2x:00:00 x") == -1, "line times");
}

static void testDayLog() {
    std::printf("── AIS: two radios, one day's log and one card ──\n");
    const std::string dir = tmpDir("ais");
    const bool clean = twoRadios([&](int p) {
        gallery::Store st; st.setDir(dir); st.setLimitBytes(100ull << 20);
        st.setRadioLabel(p == 0 ? "RSP1A" : "RTL");
        int64_t now = T0 + p;   // ★ the second radio's clock a second on — the same burst, heard by both
        vibestation::DayLog::Options o;
        o.kind = "ais"; o.label = "AIS"; o.what = "ships and stations"; o.freqHz = 162000000; o.origin = p == 0 ? "background" : "live";
        o.profile = p == 0 ? "Profile 1" : ""; o.clock = [&now] { return now; };
        vibestation::DayLog log(st, o);
        auto heard = [&](const char* key, const char* name, const std::string& line) {
            vibestation::Heard h; h.key = key; h.kind = "A"; h.name = name; h.line = line; log.heard(h);
        };
        heard("235000001", "BOTH SEEN", "BOTH SEEN · 50.1N 1.2W · 12 kn");          // both radios hear this one
        heard(p == 0 ? "235000002" : "235000003", p == 0 ? "ONLY HF" : "ONLY VHF", p == 0 ? "ONLY HF · moored" : "ONLY VHF · 3 kn");
        now += 120;
        heard("235000001", "BOTH SEEN", "BOTH SEEN · 50.2N 1.2W · 12 kn");          // and its next position
        log.tick();
        now += 120; log.tick();
        log.finish();
    });
    ok(clean, "both processes ran");
    logradio::setMine("");
    gallery::Store r; r.setDir(dir); r.setLimitBytes(100ull << 20);
    const auto all = items(r, "ais");
    std::string logN, logT, cardN, cardT;
    for (const auto& it : all) {
        if (r.field(it.first, "s").find("-log") != std::string::npos) { logN = it.first; logT = it.second; }
        if (r.field(it.first, "s").find("-card") != std::string::npos) { cardN = it.first; cardT = it.second; }
    }
    ok(all.size() == 2, "ONE log + ONE card for the server (" + std::to_string(all.size()) + " items)");
    ok(countOf(logT, "BOTH SEEN · 50.1N") == 1 && countOf(logT, "BOTH SEEN · 50.2N") == 1, "the message both heard: once each\n" + logT);
    ok(countOf(logT, "ONLY HF") == 1 && countOf(logT, "ONLY VHF") == 1, "each radio's own message is in it");
    ok(bothRadios(r, logN) && bothRadios(r, cardN), "log and card list both radios (ri " + r.field(logN, "ri") + ")");
    ok(cardT.find("3 ships and stations") != std::string::npos && cardT.find("4 messages logged") != std::string::npos,
       "the card is the server's: 3 stations, 4 distinct messages\n" + cardT);
    ok(cardT.find("    2  BOTH SEEN") != std::string::npos, "BOTH SEEN counted twice (two positions), not four");
    // a second day goes to a new item; the old day's stay as they are
    rmTree(dir);
}

int main() {
    std::printf("test-shared-day — one day item per server, every radio feeding it (beta92)\n");
    testHelpers();
    testDayLog();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
