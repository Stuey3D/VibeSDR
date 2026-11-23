// test-log-radio.cpp — V12 beta92 WHICH RADIO HEARD IT in the gallery's index (vibe_log_radio.h, vibe_gallery.h),
// docs/v12/LOG-RADIO-FILTER.md. The station store's half is in test-station-store.cpp (testRadios).
//
// ★★★ WHY (Stuart, 2026-11-22): the logs stay ONE PER SERVER ("we dont separate the digital spots, navtex etc"); each
//     entry only remembers which radio(s) heard it, so a log view can "filter by sdr if multiple picked up the same signal".
//  1. The helpers: ids validated (never a path, never markup), split/join/union bounded; the registry gives two
//     processes' ids one bit each, the same bit every time, and survives a torn last line.
//  2. TWO PROCESSES (fork) on one gallery folder, two radio ids: each writes its own NAVTEX items, and both rewrite ONE
//     shared item (FUNcube's day log — Store::replace, adoptSlot anyWriter) → its "ri" lists both.
//  3. listJson: "radios" names the radios the list holds (the machine's names, counts per kind); radio= filters (a view —
//     nothing is deleted); the shared item shows under both; "total" is the view's; a junk radio= is ignored.
//  4. OLD LINES (no "ri", an older build) read as unknown and show under every radio; a one-radio gallery lists one
//     radio (the clients then draw no filter); retag / rewrite keep the radios.
//  5. ★★ THE TIME RANGE (listJson sinceSec, Stuart 2026-11-23 "last hour last day last week last month"): items at
//     now−30 min, −5 h, −3 d, −20 d, −60 d → 1 H / 24 H / 7 DAYS / 30 DAYS / ALL show 1 / 2 / 3 / 4 / 5; pages and "total"
//     follow; with kind= and radio=; "kinds" stays the whole store's; an old request is answered byte for byte as before.
#include "vibe_gallery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace vibe;
static int g_pass = 0, g_fail = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (c) g_pass++; else g_fail++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/logradio-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static void rmTree(const std::string& d) { std::string c = "rm -rf '" + d + "'"; if (std::system(c.c_str())) {} }
static long num(const std::string& j, const std::string& k) {
    const size_t p = j.find("\"" + k + "\":"); return p == std::string::npos ? -1L : std::atol(j.c_str() + p + k.size() + 3);
}
static size_t countOf(const std::string& s, const std::string& n) { size_t c = 0; for (size_t at = 0; (at = s.find(n, at)) != std::string::npos; at += n.size()) c++; return c; }
static gallery::Meta meta(const char* kind, int i) {
    gallery::Meta m; m.kind = kind; m.origin = "background"; m.freqHz = 518000; m.mode = "usb"; m.decoder = kind;
    m.utc = 1792700000 + i; m.title = std::string(kind) + " " + std::to_string(i); return m;
}

static void testHelpers() {
    std::printf("── the helpers ──\n");
    namespace lr = logradio;
    ok(lr::validId("a1b2c3d4") && !lr::validId("") && !lr::validId("../x") && !lr::validId("A1B2") && !lr::validId("a\"b")
       && !lr::validId(std::string(17, 'a')), "ids: [0-9a-z]{1,16} only");
    ok(lr::joinIds(lr::splitIds("aa,bb,aa,,../x,cc")) == "aa,bb,cc", "split drops junk and repeats");
    ok(lr::unionIds("aa,bb", "bb,cc") == "aa,bb,cc" && lr::unionIds("", "aa") == "aa" && lr::unionIds("aa", "") == "aa", "union keeps order");
    std::string many; for (int i = 0; i < 20; i++) { if (i) many += ','; many += "r" + std::to_string(i); }
    ok(lr::splitIds(many).size() == lr::kMaxIdsPerEntry, "at most 8 ids on one entry");
    const std::string d = tmpDir("reg");
    ok(lr::bitFor(d, "aa11") == 0 && lr::bitFor(d, "bb22") == 1 && lr::bitFor(d, "aa11") == 0, "registry: first come, first bit; the same id the same bit");
    { FILE* f = std::fopen((d + "/radios.idx").c_str(), "a"); std::fputs("cc3", f); std::fclose(f); }   // a torn line (a crash mid-write)
    const int b = lr::bitFor(d, "dd44");
    const auto reg = lr::readRegistry(d);
    ok(b == 3 && reg.size() == 4 && reg[3] == "dd44" && reg[2] == "cc3", "a torn last line is closed, the next id gets its own line");
    ok(lr::bitFor(d, "") == -1 && lr::bitFor("", "aa11") == -1 && lr::bitFor(d, "../etc") == -1, "no dir / no id / a bad id → no bit");
    ok(lr::idsOf(reg, 0b1001) == std::vector<std::string>({ "aa11", "dd44" }), "a mask → its ids");
    rmTree(d);
    lr::setMachineRadios([] { return std::vector<lr::Named>{ { "bb22", "Second" }, { "aa11", "First \"quoted\"" }, { "BAD!", "x" } }; });
    const std::string j = lr::radiosJson({ { "aa11", 3 }, { "bb22", 0 }, { "ee55", 1 } }, 2);
    ok(j == "[{\"id\":\"aa11\",\"name\":\"First \\\"quoted\\\"\",\"n\":3},{\"id\":\"ee55\",\"name\":\"ee55\",\"n\":1}],\"radioUnknown\":2",
       "radiosJson: the machine's order and names (escaped), a radio with none left out, a removed radio by its id: " + j);
    lr::setMachineRadios(nullptr);
}

static void testTwoProcesses() {
    std::printf("── two processes, two radios, one gallery ──\n");
    namespace lr = logradio;
    const std::string d = tmpDir("procs");
    // the shared item (FUNcube's day log): created once, before the radios start
    std::string shared;
    {
        lr::setMine("aa11bb22");
        gallery::Store s; s.setDir(d);
        gallery::Meta m = meta("fctlm", 0); m.slot = "fc-day-AO73"; m.origin = "live";
        shared = s.create(m, "txt", "day log\n");
        s.close(shared);
        lr::setMine("");
    }
    ok(!shared.empty(), "the shared item is made");
    const char* ids[2] = { "aa11bb22", "cc33dd44" };
    std::vector<pid_t> kids;
    for (int p = 0; p < 2; p++) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            lr::setMine(ids[p]);
            gallery::Store s; s.setDir(d);
            for (int i = 0; i < 10; i++) s.add(meta("navtex", p * 100 + i), "txt", "ZCZC " + std::to_string(i) + "\nNNNN\n");
            gallery::Meta m = meta("fctlm", 0); m.slot = "fc-day-AO73"; m.origin = "live";
            const std::string n = s.adoptSlot(m, true, true);
            if (n.empty() || !s.replace(n, m, "day log, radio " + std::to_string(p) + "\n")) std::_Exit(3);
            s.close(n);
            std::_Exit(0);
        }
        kids.push_back(pid);
    }
    bool clean = true;
    for (pid_t k : kids) { int st = 0; ::waitpid(k, &st, 0); if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) clean = false; }
    ok(clean, "both processes wrote (and found the shared item by its slot)");

    lr::setMachineRadios([] { return std::vector<lr::Named>{ { "aa11bb22", "RSP1A HF" }, { "cc33dd44", "RTL VHF" } }; });
    gallery::Store r; r.setDir(d);   // a third process reading: the front door, say
    const std::string all = r.listJson("", 100, 0);
    ok(num(all, "total") == 21, "21 items: 10 + 10 NAVTEX + the shared one (" + std::to_string(num(all, "total")) + ")");
    ok(all.find("{\"id\":\"aa11bb22\",\"name\":\"RSP1A HF\",\"n\":11}") != std::string::npos
       && all.find("{\"id\":\"cc33dd44\",\"name\":\"RTL VHF\",\"n\":11}") != std::string::npos, "radios: both named, 11 each (10 + the shared)");
    ok(r.field(shared, "ri").find("aa11bb22") != std::string::npos && r.field(shared, "ri").find("cc33dd44") != std::string::npos,
       "the shared item lists both radios: " + r.field(shared, "ri"));
    const std::string one = r.listJson("", 100, 0, "cc33dd44");
    ok(num(one, "total") == 11 && countOf(one, "\"ri\":\"aa11bb22\"") == 0 && one.find(shared) != std::string::npos,
       "radio=cc33dd44: its 10 + the shared item, none of the other's (" + std::to_string(num(one, "total")) + ")");
    ok(one.find("\"radio\":\"cc33dd44\"") != std::string::npos, "the answer says the filter");
    const std::string page = r.listJson("navtex", 4, 4, "aa11bb22");
    ok(countOf(page, "\"f\":") == 4 && num(page, "total") == 10 && countOf(page, "\"ri\":\"aa11bb22\"") == 4, "a filtered PAGE: 4 of its 10");
    const std::string nav = r.listJson("navtex", 100, 0);
    ok(nav.find("\"n\":10}") != std::string::npos && nav.find("\"n\":11}") == std::string::npos, "radios counts follow the kind (NAVTEX: 10 each)");
    ok(num(r.listJson("", 100, 0, "../etc"), "total") == 21, "a junk radio= is ignored");
    ok(num(r.listJson("", 100, 0, "ffffffff"), "total") == 0, "a radio with nothing: an empty view");
    ok(num(r.listJson("", 100, 0), "total") == 21, "nothing was deleted by filtering");

    // ── 4. an OLD line (no "ri") ──
    {
        FILE* f = std::fopen((d + "/old-navtex.txt").c_str(), "w"); std::fputs("ZCZC old\nNNNN\n", f); std::fclose(f);
        f = std::fopen((d + "/index.jsonl").c_str(), "a");
        std::fputs("{\"f\":\"old-navtex.txt\",\"k\":\"navtex\",\"o\":\"background\",\"t\":1792600000,\"hz\":518000,\"m\":\"usb\",\"d\":\"navtex\",\"p\":\"\",\"r\":\"Pi\",\"title\":\"old\"}\n", f);
        std::fclose(f);
    }
    const std::string withOld = r.listJson("navtex", 100, 0);
    ok(num(withOld, "total") == 21 && num(withOld, "radioUnknown") == 1, "an old line reads, counted as unknown");
    ok(num(r.listJson("navtex", 100, 0, "aa11bb22"), "total") == 11 && num(r.listJson("navtex", 100, 0, "cc33dd44"), "total") == 11,
       "the unknown item shows under every radio");
    // retag keeps the radios (and adds this process's)
    {
        lr::setMine("aa11bb22");
        gallery::Meta m = meta("fctlm", 0); m.slot = "fc-day-AO73"; m.origin = "both";
        ok(r.retag(shared, m), "retag");
        ok(r.field(shared, "ri") == "aa11bb22,cc33dd44" || r.field(shared, "ri") == "cc33dd44,aa11bb22", "retag keeps both radios: " + r.field(shared, "ri"));
        ok(r.rewrite(shared, "AO-73 day", "new text\n") && r.field(shared, "ri").size() == 17, "rewrite keeps the radios");
        lr::setMine("");
    }
    lr::setMachineRadios(nullptr);
    rmTree(d);
}

static void testOneRadio() {
    std::printf("── one radio ──\n");
    namespace lr = logradio;
    const std::string d = tmpDir("one");
    lr::setMine("aa11bb22");
    gallery::Store s; s.setDir(d);
    for (int i = 0; i < 3; i++) s.add(meta("navtex", i), "txt", "x\n");
    const std::string j = s.listJson("", 25, 0);
    ok(countOf(j, "{\"id\":") == 1 && j.find("radioUnknown") == std::string::npos, "one radio listed: the clients draw no filter");
    lr::setMine("");
    s.add(meta("navtex", 9), "txt", "y\n");   // the phone's own radio: no id
    const std::string k = s.listJson("", 25, 0);
    ok(num(k, "radioUnknown") == 1 && k.find(",\"ri\":") != std::string::npos, "a writer with no id writes no ri (unknown)");
    rmTree(d);
}

static void testTimeRange() {
    std::printf("── the time range (since=) ──\n");
    namespace lr = logradio;
    const std::string d = tmpDir("since");
    lr::setMine("aa11bb22");
    gallery::Store s; s.setDir(d);
    const int64_t now = gallery::nowUtc();
    const long back[5] = { 1800, 5 * 3600, 3 * 86400, 20 * 86400, 60 * 86400 };
    for (int i = 4; i >= 0; i--) {   // oldest first, as a gallery fills
        gallery::Meta m = meta("navtex", i); m.utc = now - back[i];
        s.add(m, "txt", "ZCZC " + std::to_string(i) + "\nNNNN\n");
        gallery::Meta w = meta("wefax", i); w.utc = now - back[i];
        s.add(w, "txt", "chart " + std::to_string(i) + "\n");
    }
    lr::setMine("cc33dd44");
    { gallery::Meta m = meta("navtex", 9); m.utc = now - 600; s.add(m, "txt", "ZCZC other radio\nNNNN\n"); }
    lr::setMine("");
    const long ranges[5] = { 3600, 86400, 7 * 86400, 30 * 86400, 0 };
    const long wantNav[5] = { 2, 3, 4, 5, 6 };   // + the other radio's, 10 min ago
    const char* names[5] = { "1 H", "24 H", "7 DAYS", "30 DAYS", "ALL" };
    for (int r = 0; r < 5; r++) {
        const std::string j = s.listJson("navtex", 25, 0, "", ranges[r]);
        ok(num(j, "total") == wantNav[r] && (long)countOf(j, "\"f\":") == wantNav[r],
           std::string("NAVTEX ") + names[r] + ": " + std::to_string(wantNav[r]) + " (" + std::to_string(num(j, "total")) + ")");
        ok(num(s.listJson("", 25, 0, "", ranges[r]), "total") == wantNav[r] + (r + 1), std::string("ALL kinds ") + names[r]);
        ok(num(s.listJson("navtex", 25, 0, "aa11bb22", ranges[r]), "total") == r + 1, std::string("radio aa11bb22 ") + names[r]);
    }
    const std::string day = s.listJson("navtex", 25, 0, "", 86400);
    ok(day.find("\"since\":86400") != std::string::npos, "the answer says the range");
    ok(num(day, "count") == 11 && day.find("\"navtex\":{\"n\":6,") != std::string::npos && day.find("\"wefax\":{\"n\":5,") != std::string::npos,
       "\"count\" and \"kinds\" stay the whole store's");
    ok(day.find("\"id\":\"aa11bb22\"") != std::string::npos && day.find("\"n\":5}") != std::string::npos,
       "\"radios\" stays the kind's whole list (the chips never vanish in a quiet hour): " + day.substr(day.find("\"radios\"")));
    // pages in a range: 30 DAYS of ALL kinds = 9, pages of 4 → 4 + 4 + 1
    const std::string p3 = s.listJson("", 4, 8, "", 30 * 86400);
    ok(countOf(p3, "\"f\":") == 1 && num(p3, "total") == 9, "a ranged PAGE: page 3 of 30 DAYS holds the 9th");
    ok(num(s.listJson("", 4, 0, "", 3600), "total") == 3, "1 H over ALL kinds: the two NAVTEX and the WEFAX");
    // old requests: exactly as before
    ok(s.listJson("navtex", 25, 0, "", 0) == s.listJson("navtex", 25, 0), "since 0 = the old request, byte for byte");
    ok(s.listJson("", 300, -1, "", 0) == s.listJson("", 300), "the oldest request (no offset) unchanged");
    ok(s.listJson("", 25, 0, "", 401L * 86400) == s.listJson("", 25, 0), "past 400 days = all");
    ok(s.listJson("", 25, 0, "", -5) == s.listJson("", 25, 0), "a negative range = all");
    ok(s.listJson("", 300, -1).find("\"since\"") == std::string::npos, "no range: no \"since\" in the answer");
    // an old line with no "t" shows under ALL only
    {
        FILE* f = std::fopen((d + "/old-not.txt").c_str(), "w"); std::fputs("x\n", f); std::fclose(f);
        f = std::fopen((d + "/index.jsonl").c_str(), "a");
        std::fputs("{\"f\":\"old-not.txt\",\"k\":\"navtex\",\"o\":\"background\",\"hz\":518000,\"m\":\"usb\",\"d\":\"navtex\",\"p\":\"\",\"r\":\"Pi\",\"title\":\"no time\"}\n", f);
        std::fclose(f);
    }
    ok(num(s.listJson("navtex", 25, 0), "total") == 7 && num(s.listJson("navtex", 25, 0, "", 30 * 86400), "total") == 5,
       "a line with no time: under ALL, never in a range");
    rmTree(d);
}

int main() {
    std::printf("test-log-radio — which radio heard each gallery entry (beta92)\n");
    testHelpers();
    testTwoProcesses();
    testOneRadio();
    testTimeRange();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
