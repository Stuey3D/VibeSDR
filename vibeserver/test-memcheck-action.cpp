// test-memcheck-action.cpp — the daemon's half of "Check this radio's memory" (vibe_memcheck.h, 2026-11-03).
// ★★ The endpoint, the daemon and the root helper each check the serial; this is the first two's rule, which must agree
//    with the helper's mc_valid_serial (test_vibeserver_memcheck.py runs the same cases there), plus the listener count
//    read from a radio's /vibeserver.json and the "is something else running?" test on the helper's log.
#include "vibe_memcheck.h"
#include <cstdio>
#include <string>

static int fails = 0, checks = 0;
static void ok(bool c, const char* what) { checks++; std::printf("  %s %s\n", c ? "ok  " : "FAIL", what); if (!c) fails++; }

int main() {
    using namespace vibememcheck;
    std::puts("serialOk — the serial alphabet");
    for (const char* s : { "31A0B7C", "0000000001", "3b52ab5dac9d3f4b", "4", "2405XY:01", "A_b-c.d" })
        ok(serialOk(s), (std::string("accepts ") + s).c_str());
    for (const char* s : { "", "-rf", "--config", ".hidden", "../etc/passwd", "a/b", "a b", "x;reboot", "a$(id)", "`id`",
                           "vibeserver@x", "a\\b", "*", "a\nb", "31A0B7C\r" })
        ok(!serialOk(s), (std::string("refuses '") + s + "'").c_str());
    ok(!serialOk(std::string(65, 'A')) && serialOk(std::string(64, 'A')), "64 characters at most");

    std::puts("parseAction — \"memcheck <serial>\" only");
    std::string s;
    ok(parseAction("memcheck 31A0B7C", s) && s == "31A0B7C", "memcheck 31A0B7C → 31A0B7C");
    ok(!parseAction("memcheck", s), "no serial: refused");
    ok(!parseAction("memcheck ", s), "an empty serial: refused");
    ok(!parseAction("memcheck ../x", s), "a path: refused");
    ok(!parseAction("memcheck 31A0B7C extra", s), "a second word: refused");
    ok(!parseAction("restart 31A0B7C", s), "another action's name: not a memcheck");
    ok(!parseAction("memcheckx 31A0B7C", s), "a longer name: not a memcheck");

    std::puts("listenersIn — a radio's /vibeserver.json");
    ok(listenersIn("HTTP/1.1 200 OK\r\n\r\n{\"busy\":false,\"listeners\":3,\"maxUsers\":10}") == 3, "3 listeners");
    ok(listenersIn("{\"listeners\":0}") == 0, "0 listeners");
    ok(listenersIn("{\"listeners\": 12}") == 12, "a space after the colon");
    ok(listenersIn("") == -1, "nothing answered: -1 (no field), which the daemon treats as nobody listening");
    ok(listenersIn("{\"listeners\":\"x\"}") == -1, "not a number: -1");

    std::puts("logShowsRunning — the helper's end marker");
    ok(!logShowsRunning(""), "an empty log: nothing running");
    ok(logShowsRunning("installing updates\n"), "output, no marker: running");
    ok(!logShowsRunning("done\n__VIBESERVER_DONE__ 0\n"), "the marker: finished");
    std::printf("%s (%d checks)\n", fails ? "FAILED" : "ALL PASS", checks);
    return fails ? 1 : 0;
}
