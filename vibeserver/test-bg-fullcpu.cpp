// test-bg-fullcpu.cpp — NOBODY LISTENING: background decoding may use the whole machine (beta94, vibe_bg_fullcpu.h).
// Stuart, 2026-11-25: "if nobody is using the pi then the full cpu can be allowed for background use, soon as a listener
// joins and starts using then the background gets cut back as needed".
//   · no listeners → normal through the calm period (the owner's idle grace, clamped 30 s … 10 min), then FULL;
//   · a listener joins → normal AT ONCE, said in the log; a HELD session is not streaming;
//   · the last leaves → full again only after the calm period;
//   · many radios on one box: each idle radio's share = cores ÷ idle radios; the allowance inside it follows the MEASURED
//     spare CPU (up one core while ≥ 1.25 free, down one while < 0.4 free, one step per 10 s);
//   · unknown load (Android) never gets every core; hot / throttled / on battery never goes full (phones, the R36S);
//   · the RTTY workers never fall below the owner's / benchmark's figure;
//   · the decoder threads' view (Live): the Band cap's scale, whether a skip counts, the backlog hold;
//   · the box board: idle radios counted through flock-held files, a closed one gone.
#include "vibe_bg_fullcpu.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace vibe::bg::fullcpu;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

static Inputs idleIn(int64_t t, double busy = 50.0, int idleRadios = 1) {
    Inputs in;
    in.nowMs = t; in.streaming = 0; in.bgRunning = true; in.cores = 4; in.machineBusyPct = busy; in.idleRadios = idleRadios;
    in.graceSec = 300; in.rttyFloor = 2;
    return in;
}

static void states() {
    std::printf("── nobody listening → FULL after the calm period; a listener → normal at once; the last leaves → calm, then FULL ──\n");
    Policy p;
    int64_t t = 0;
    Budget b = p.tick(idleIn(t, 10));
    ok(!b.full && b.rttyWorkers == 2 && b.extraSlots == 0 && b.cores == 1, "nobody listening, but inside the calm period: today's budget (2 RTTY workers, no extra slots)");
    for (t = 2000; t < 298000; t += 2000) b = p.tick(idleIn(t, 10));
    ok(!b.full, "299 s of nobody: still normal (the owner's 300 s grace)");
    b = p.tick(idleIn(t = 300000, 10));
    ok(b.full && b.share == 4 && b.cores == 3, "300 s: FULL — this radio alone on 4 cores, 3.6 cores free → starts at 3 (" + std::to_string(b.cores) + ")");
    ok(p.said().find("background decoding: nobody listening \xe2\x80\x94 using the whole CPU (4 cores)") == 0, "logged: " + p.said());
    ok(b.rttyWorkers == 3 && b.extraSlots == 3, "3 RTTY search workers, 3 decoder slots over the box's limit");
    for (int i = 0; i < 5; i++) b = p.tick(idleIn(t += 2000, 10));
    ok(b.cores == 4 && b.rttyWorkers == 4, "10 s later, still 3.6 free: spreads to all 4 cores");
    ok(p.said().empty() || p.said().find("spreading to 4 cores") != std::string::npos, "…said: spreading to 4 cores");
    Live L; Policy::store(L, b);
    ok(L.full && L.budgetScale() == 4.0 && L.extraSlots == 4 && L.behindHoldSec() == kFullBehindHoldSec, "the threads' view: Band cap × 4, backlog must last 10 s");
    ok(!L.skipCounts(), "…and with 3.6 cores spare a skipped slot does not count");

    // a listener joins
    Inputs in = idleIn(t += 2000, 10); in.streaming = 1;
    b = p.tick(in);
    ok(!b.full && b.cores == 1 && b.rttyWorkers == 2 && b.extraSlots == 0, "★ a listener joins: today's budget AT ONCE (2 workers, no extras)");
    ok(p.said().find("background decoding: a listener joined \xe2\x80\x94 background cut back to the normal budget (2 RTTY search workers") == 0, "logged: " + p.said());
    Policy::store(L, b);
    ok(!L.full && L.budgetScale() == 1.0 && L.skipCounts() && L.behindHoldSec() == 0.0, "the threads' view: today's cap, a skip pauses CW, no hold");
    b = p.tick(in);
    ok(p.said().empty(), "said once, not every tick");

    // the last listener leaves
    for (int i = 0; i < 150; i++) b = p.tick(idleIn(t += 2000, 10));
    ok(!b.full, "the last listener left 298 s ago: still the calm period");
    b = p.tick(idleIn(t += 2000, 10));
    ok(b.full, "300 s after: FULL again");

    // held session = not streaming (the shim's count), and no background running = nothing to give
    Policy q;
    Inputs n2 = idleIn(0, 10); n2.bgRunning = false;
    for (int64_t k = 0; k < 400000; k += 2000) { n2.nowMs = k; b = q.tick(n2); }
    ok(!b.full, "background not running: never FULL (there is nothing to give the CPU to)");

    // the calm period clamps
    Policy c1; Inputs g = idleIn(0, 10); g.graceSec = 5;
    for (int64_t k = 0; k <= 28000; k += 2000) { g.nowMs = k; b = c1.tick(g); }
    ok(!b.full, "an owner's 5 s grace: the calm period is at least 30 s (a page reload is not a leave)");
    g.nowMs = 30000; b = c1.tick(g);
    ok(b.full, "…FULL at 30 s");
    Policy c2; g = idleIn(0, 10); g.graceSec = 3600;
    for (int64_t k = 0; k <= 600000; k += 2000) { g.nowMs = k; b = c2.tick(g); }
    ok(b.full, "an owner's 1 h grace: at most 10 minutes");
}

static void share() {
    std::printf("── many radios on one box: each idle radio's share, the allowance from MEASURED spare CPU ──\n");
    Policy p;
    int64_t t = 0;
    Budget b;
    // the Pi 500: 4 cores, 4 radios idle with background running, 47 % idle (1.88 cores spare)
    for (; t <= 300000; t += 2000) b = p.tick(idleIn(t, 53, 4));
    ok(b.full && b.share == 1 && b.cores == 1, "4 idle radios on 4 cores: each may use 1 core — never every core each");
    ok(b.rttyWorkers == 2, "…the RTTY workers stay at the floor (2), never below it");
    for (int i = 0; i < 20; i++) b = p.tick(idleIn(t += 2000, 53, 2));
    ok(b.share == 2 && b.cores == 2, "two of them get listeners: the two idle ones' share is 2 cores each, and 1.88 free → up to 2");
    for (int i = 0; i < 20; i++) b = p.tick(idleIn(t += 2000, 75, 1));
    ok(b.share == 4 && b.cores == 2, "alone: share 4, but only 1.0 core is spare — it stays at 2 (spare CPU only while it is spare)");
    b = p.tick(idleIn(t += 2000, 20, 1));
    ok(b.cores == 3, "the box goes quiet (3.2 free): one more core (the last step was long ago)");
    for (int i = 0; i < 4; i++) b = p.tick(idleIn(t += 2000, 20, 1));
    ok(b.cores == 3, "…8 s later still 3: one step per 10 s, never a jump");
    b = p.tick(idleIn(t += 2000, 20, 1));
    ok(b.cores == 4, "10 s: the fourth core");
    for (int i = 0; i < 5; i++) b = p.tick(idleIn(t += 2000, 92, 1));
    ok(b.cores == 3, "another radio's listener loads the box to 92 % (0.32 free): one core given back");
    ok(p.said().find("easing back to 3 cores") != std::string::npos, "logged: " + p.said());
    for (int i = 0; i < 30; i++) b = p.tick(idleIn(t += 2000, 92, 1));
    ok(b.cores == 1 && b.full, "still 92 %: down to 1 core, never below");
    for (int i = 0; i < 30; i++) b = p.tick(idleIn(t += 2000, 70, 1));
    ok(b.cores == 1, "70 % busy (1.2 free, under 1.25): holds — no flapping between the thresholds");
    Live L; b.spare = 0.32; Policy::store(L, b);
    ok(L.full && L.skipCounts(), "no headroom (0.32 cores free): a skipped slot counts again — CW/RTTY pause first, as today");
    b.spare = -1; Policy::store(L, b);
    ok(L.skipCounts(), "headroom unknown: a skip counts (today's rule)");
}

static void phones() {
    std::printf("── phones and the handheld: unknown load, heat, battery ──\n");
    Policy p;
    Budget b;
    Inputs in = idleIn(0, -1); in.cores = 8;
    for (int64_t t = 0; t <= 300000; t += 2000) { in.nowMs = t; b = p.tick(in); }
    ok(b.full && b.share == 4 && b.cores == 4, "an 8-core phone (load unknown): FULL, but half the cores at most (" + std::to_string(b.cores) + ")");
    Live L; Policy::store(L, b);
    ok(L.skipCounts(), "unknown spare: a skipped slot still pauses CW/RTTY");
    in.hot = true; in.nowMs += 2000; b = p.tick(in);
    ok(!b.full && b.cores == 1, "★ the phone gets hot (throttled / temperature HIGH): back to the normal budget at once");
    ok(p.said().find("nobody listening, but the CPU is hot or throttled \xe2\x80\x94 staying at the normal budget") != std::string::npos, "logged: " + p.said());
    in.nowMs += 2000; b = p.tick(in);
    ok(p.said().empty(), "said once");
    in.hot = false; in.onBattery = true;
    for (int i = 0; i < 200; i++) { in.nowMs += 2000; b = p.tick(in); }
    ok(!b.full, "on battery (not charging, not on mains): never FULL");
    in.onBattery = false;
    in.nowMs += 2000; b = p.tick(in);
    ok(b.full, "charging again (the calm period long passed): FULL");
    Policy r; Inputs h = idleIn(0, 10); h.hot = true;
    for (int64_t t = 0; t <= 400000; t += 2000) { h.nowMs = t; b = r.tick(h); }
    ok(!b.full, "a Pi throttling for heat or under-voltage: never FULL (more work would only make it worse)");
}

static void board() {
    std::printf("── the box board: idle radios counted, a closed one gone ──\n");
    char tpl[256]; std::snprintf(tpl, sizeof tpl, "%s/bgcpu-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    const std::string dir = mkdtemp(tpl) ? std::string(tpl) : std::string();
    ok(!dir.empty(), "a temporary slot directory");
    Board a, b, c;
    a.open(dir, "100"); b.open(dir, "200"); c.open(dir, "300");
    ok(a.isOpen() && b.isOpen() && c.isOpen(), "three radios' files");
    ok(Board::countIdle(dir) == 0, "nobody idle yet");
    a.publish(true); b.publish(true); c.publish(false);
    ok(Board::countIdle(dir) == 2, "two idle with background running, one with a listener: 2");
    b.close();
    ok(Board::countIdle(dir) == 1, "one radio's process goes: its file goes with it");
    Board dup; dup.open(dir, "100");
    ok(!dup.isOpen(), "a second open of a live radio's file is refused (the flock)");
    a.publish(false);
    ok(Board::countIdle(dir) == 0, "a listener joins the last idle one: 0");
    ok(Board::countIdle(std::string()) == 0, "no directory (Android): nothing to count — the shim counts this radio alone");
    a.close(); c.close();
    ::rmdir(dir.c_str());
}

int main() {
    states();
    share();
    phones();
    board();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
