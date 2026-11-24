// test-bg-ease.cpp — BACKGROUND DECODING EASES OFF FOR LISTENERS, ON PURPOSE, BEFORE IT HURTS (V12, 2026-11-24).
//   Stuart, 24 Nov, on the Pi 500's RSP1B: "Background decoding: TOO HEAVY" while he listened — and "the sdr feels
//   responsive enough, just the warning that came up". So:
//     · the order steps are taken in (chips furthest from the listener → pictures → text → whole skimmers → DSC/NAVTEX
//       LAST, and those only for the listener's own harm);
//     · the governor: the Pi 500's own give-way (~0.4 s of IQ a minute) is NEVER acted on; real trouble is, one step at a
//       time, waiting to see the effect; steps come back in reverse only after 3 calm minutes and only with room for
//       them; a step that has to be taken again soon after coming back waits twice as long next time; a noisy signal
//       never makes it flap; the listener leaving gives everything back;
//     · the health card: planned give-way is not named "TOO HEAVY" (BehindWatch's excused drops), the eased state is a
//       line of information ("notes");
//     · the admin figure's thread count and busiest thread (Linux: /proc/self/task) — elsewhere it says nothing.
// Silent: no audio, no network. Pure — the governor is driven with its own clock.
#include "vibe_bg_ease.h"
#include "vibe_health.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#endif

using namespace vibe::bg::ease;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

// ── the Pi 500's profile, 24 Nov (radios.0.bgDecode): 4 Band skimmers, 3 DSC, RTTY weather, WEFAX stations ────────────
struct V { const char* dec; double hz; };
static const V kPi500[] = {
    { "spots-band", 3573000 }, { "spots-band", 5357000 }, { "spots-band", 7074000 }, { "spots-band", 10136000 },
    { "dsc", 4207500 }, { "dsc", 6312000 }, { "dsc", 8414500 }, { "rtty-weather", 10100800 }, { "wefax-stations", 7880000 },
};
static std::vector<Candidate> candidates(double listenerHz, const std::vector<bool>& chipsOff, const std::vector<bool>& paused) {
    std::vector<Candidate> c;
    for (int i = 0; i < 9; i++) {
        if (paused[(size_t)i]) continue;
        Candidate k; k.vfo = i; k.rank = i + 1; k.distHz = std::fabs(kPi500[i].hz - listenerHz);
        if (std::string(kPi500[i].dec) == "spots-band" && !chipsOff[(size_t)i]) { k.kind = Kind::Chips; k.tier = kTierChips; c.push_back(k); }
        k.kind = Kind::Pause; k.tier = tierOf(kPi500[i].dec); c.push_back(k);
    }
    return c;
}

static void order() {
    std::printf("── the order steps are taken in ──\n");
    ok(tierOf("dsc") == kTierSafety && tierOf("dsc-vhf") == kTierSafety && tierOf("navtex") == kTierSafety, "DSC (MF/HF, VHF) and NAVTEX are safety");
    ok(tierOf("wefax-stations") == kTierPictures && tierOf("wefax") == kTierPictures && tierOf("sstv") == kTierPictures, "WEFAX stations, WEFAX, SSTV are pictures");
    ok(tierOf("rtty-weather") == kTierText && tierOf("sitora") == kTierText && tierOf("aprs") == kTierText, "RTTY weather, SITOR-A, APRS are text");
    ok(tierOf("spots-band") == kTierSkimmers && tierOf("ft8") == kTierSkimmers && tierOf("cw-skim-48") == kTierSkimmers
       && tierOf("rtty-skim-24") == kTierSkimmers && tierOf("psk-skim-6") == kTierSkimmers && tierOf("ndb-skim") == kTierSkimmers,
       "Band / FT8 / CW / RTTY / PSK / NDB skimmers are skimmers");

    // The listener on 40 m FT8 (7.074 MHz): walk the whole ladder.
    std::vector<bool> chips(9, false), paused(9, false);
    std::vector<std::string> seq;
    for (int step = 0; step < 20; step++) {
        const auto c = candidates(7074000, chips, paused);
        const int k = pickNext(c, false);
        if (k < 0) break;
        const Candidate& x = c[(size_t)k];
        char b[64]; std::snprintf(b, sizeof b, "%s%d", x.kind == Kind::Chips ? "chips" : "pause", x.vfo + 1);
        seq.push_back(b);
        if (x.kind == Kind::Chips) chips[(size_t)x.vfo] = true; else paused[(size_t)x.vfo] = true;
    }
    std::string got; for (auto& s : seq) got += s + " ";
    // chips: 80 m (3.50 MHz away), 30 m (3.06), 60 m (1.72), 40 m (0) · WEFAX stations (9) · RTTY weather (8) ·
    // Band skimmers 80, 30, 60, 40 m · and NOT the DSC watches.
    ok(got == "chips1 chips4 chips2 chips3 pause9 pause8 pause1 pause4 pause2 pause3 ",
       "★ listener on 40 m: chips 80→30→60→40 m, WEFAX stations, RTTY weather, Band 80→30→60→40 m — DSC never: " + got);
    const auto rest = candidates(7074000, chips, paused);
    ok(pickNext(rest, false) < 0, "★ only the three DSC watches left: nothing more for background decoding's own drops");
    const int k = pickNext(rest, true);
    ok(k >= 0 && rest[(size_t)k].vfo == 6, "...for the LISTENER's harm, the lowest-row DSC (VFO 7, 8414.5 kHz) goes first");

    // The listener on 80 m: 40 m's chips are not first any more — the band furthest away is (30 m).
    std::vector<bool> c0(9, false), p0(9, false);
    const auto a = candidates(3573000, c0, p0);
    const int f = pickNext(a, false);
    ok(f >= 0 && a[(size_t)f].kind == Kind::Chips && a[(size_t)f].vfo == 3, "listener on 80 m: 30 m's chips go first (furthest)");

    // An idle picture decoder goes before one part-way through a chart.
    std::vector<Candidate> pics = { { 0, Kind::Pause, kTierPictures, 0, 1, true }, { 1, Kind::Pause, kTierPictures, 0, 2, false } };
    ok(pickNext(pics, false) == 1, "two WEFAX VFOs: the idle one before the one mid-chart (even though it ranks higher)");
    pics[1].busy = true;
    ok(pickNext(pics, false) == 1, "both mid-chart: the owner's lower row (VFO 2) goes first");
}

// ── the governor ─────────────────────────────────────────────────────────────────────────────────────────────────
struct Sim {
    Governor g;
    int64_t t = 0;
    double bg = 0;            // seconds dropped so far
    uint64_t drops = 0;
    int taken = 0, nonSafety = 6, safety = 3;
    std::vector<Act> acts;
    double busy = 50, cost = 5; int cores = 4;
    Act step(double bgPerMin, double lag = 0.1, bool listener = true, uint64_t newDrops = 0) {
        t += 2000; bg += bgPerMin * 2.0 / 60.0; drops += newDrops;
        Signals s; s.nowMs = t; s.listenerOn = listener; s.bgDroppedSec = bg; s.listenerIqDrops = drops; s.listenerLag = lag;
        s.machineBusyPct = busy; s.cores = cores;
        const Act a = g.tick(s, taken, taken < nonSafety, taken >= nonSafety && taken < nonSafety + safety, cost);
        if (a == Act::Shed || a == Act::ShedSafety) taken++;
        if (a == Act::Restore) taken--;
        if (a == Act::RestoreAll) taken = 0;
        if (a != Act::Hold) acts.push_back(a);
        return a;
    }
    int run(int secs, double bgPerMin, double lag = 0.1) { int n = 0; for (int i = 0; i < secs / 2; i++) if (step(bgPerMin, lag) != Act::Hold) n++; return n; }
};

static void governor() {
    std::printf("── the governor ──\n");
    {
        Sim s;
        const int n = s.run(20 * 60, 0.4);
        ok(n == 0 && s.taken == 0, "★★ the Pi 500's own give-way, 0.4 s of IQ a minute for 20 min with a listener on: NOTHING paused");
        ok(!s.g.exhausted(), "...and it is not 'TOO HEAVY' (the drops are excused)");
    }
    {
        Sim s;
        s.run(120, 0.0);                                   // a listener on for two quiet minutes
        int firstShed = -1;
        for (int i = 0; i < 60 && firstShed < 0; i++) if (s.step(3.0) == Act::Shed) firstShed = i;
        ok(firstShed >= 0 && s.taken == 1, "background decoding dropping 3 s a minute: ONE step taken");
        ok(firstShed >= 14, "...only after 30 s of it measured (" + std::to_string(firstShed * 2 + 2) + " s)");
        ok(s.g.why().find("s of IQ a minute (more than the 1.5 s it may give way)") != std::string::npos, "...said in words: " + s.g.why());
        int more = 0, at = -1;
        for (int i = 0; i < 20 && at < 0; i++) if (s.step(3.0) == Act::Shed) { more++; at = i; }
        ok(more == 1 && at >= 14, "still dropping: the next step only after 30 s more of its effect (" + std::to_string(at * 2 + 2) + " s)");
        // the steps work: 0.3 s a minute. Nothing comes back for 3 minutes; then one; then 3 minutes for the next.
        int restoreAt = -1;
        for (int i = 0; i < 150 && restoreAt < 0; i++) if (s.step(0.3) == Act::Restore) restoreAt = i;
        ok(restoreAt >= 89 && s.taken == 1, "★ calm again: the LAST step comes back only after 3 calm minutes (" + std::to_string(restoreAt * 2 + 2) + " s)");
        int r2 = -1;
        for (int i = 0; i < 150 && r2 < 0; i++) if (s.step(0.3) == Act::Restore) r2 = i;
        ok(r2 >= 89 && s.taken == 0, "...and the first one, 3 minutes after that (" + std::to_string(r2 * 2 + 2) + " s) — in reverse, one at a time");
    }
    {
        Sim s;
        s.run(20, 0.0);
        for (int i = 0; i < 40 && s.taken == 0; i++) s.step(3.0);
        for (int i = 0; i < 150 && s.taken == 1; i++) s.step(0.3);
        ok(s.taken == 0 && s.g.calmWaitMs() == 180000, "a step taken and given back (wait 3 min)");
        for (int i = 0; i < 40 && s.taken == 0; i++) s.step(3.0);   // ★ the box could not take it back
        ok(s.taken == 1 && s.g.calmWaitMs() == 360000, "★ needed again within 10 min of coming back: next time it waits 6 min (no flapping)");
        int at = -1;
        for (int i = 0; i < 400 && at < 0; i++) if (s.step(0.3) == Act::Restore) at = i;
        ok(at >= 179, "...and does (" + std::to_string(at * 2 + 2) + " s)");
    }
    {
        Sim s;
        s.run(20, 0.0);
        int sheds = 0, restores = 0;
        s.taken = 2;   // two steps already in effect
        unsigned r = 12345;
        for (int i = 0; i < 900; i++) {   // 30 min of a NOISY signal around 1 s a minute, lag spikes one at a time
            r = r * 1103515245u + 12345u;
            const double bg = 0.6 + 0.8 * ((r >> 8) % 1000) / 1000.0;
            const double lag = (i % 7 == 0) ? 0.7 : 0.2;
            const Act a = s.step(bg, lag);
            if (a == Act::Shed) sheds++;
            if (a == Act::Restore) restores++;
        }
        ok(sheds == 0 && restores == 0, "★ 30 min of a noisy signal (0.6–1.4 s/min, lone lag spikes): no step taken, none given back — between "
           "the two thresholds nothing moves (" + std::to_string(sheds) + " / " + std::to_string(restores) + ")");
    }
    {
        Sim s;
        s.run(20, 0.0);
        int at = -1;
        for (int i = 0; i < 10 && at < 0; i++) if (s.step(0.0, 0.75) == Act::Shed) at = i;
        ok(at >= 0 && at <= 1, "★ the LISTENER's IQ queue 75 % full twice: a step at once (" + std::to_string(at * 2 + 2) + " s)");
        ok(s.g.listenerHarm() && s.g.why().find("listener") != std::string::npos, "...said: " + s.g.why());
        int next = -1;
        for (int i = 0; i < 10 && next < 0; i++) if (s.step(0.0, 0.75) == Act::Shed) next = i;
        ok(next >= 3, "still lagging: the next step 10 s later, not on every tick (" + std::to_string(next * 2 + 2) + " s)");
        Sim d; d.run(20, 0.0);
        d.step(0.0, 0.1, true, 1);
        ok(d.step(0.0, 0.1, true, 0) == Act::Shed || d.taken == 1, "a dropped listener buffer: a step");
    }
    {
        Sim s; s.nonSafety = 0; s.safety = 3;   // only DSC left
        s.run(20, 0.0);
        int safe = 0;
        for (int i = 0; i < 60; i++) if (s.step(3.0) != Act::Hold) safe++;
        ok(safe == 0 && s.g.exhausted(), "★ only DSC left and background decoding still dropping 3 s/min: DSC is NOT paused — TOO HEAVY instead");
        Sim h; h.nonSafety = 0; h.safety = 3; h.run(20, 0.0);
        Act a = Act::Hold;
        for (int i = 0; i < 10 && a == Act::Hold; i++) a = h.step(0.0, 0.8);
        ok(a == Act::ShedSafety, "...but the LISTENER's own receiver falling behind with nothing else left: a DSC watch, last of all");
        Sim z; z.nonSafety = 0; z.safety = 0; z.run(20, 0.0);
        for (int i = 0; i < 10; i++) z.step(0.0, 0.8);
        ok(z.g.exhausted(), "nothing left at all and the listener still lagging: exhausted");
    }
    {
        Sim s; s.run(20, 0.0);
        for (int i = 0; i < 40 && s.taken < 1; i++) s.step(3.0);
        const Act a = s.step(0.0, 0.1, false);
        ok(a == Act::RestoreAll && s.taken == 0, "★ the listener leaves: every eased step given back at once (" + s.g.why() + ")");
        ok(s.step(5.0, 0.1, false) == Act::Hold && !s.g.exhausted(), "nobody on: the governor never sheds (drops are then real overload — TOO HEAVY)");
    }
    {
        Sim s; s.run(20, 0.0);
        for (int i = 0; i < 40 && s.taken < 1; i++) s.step(3.0);
        s.busy = 90;
        int r = 0; for (int i = 0; i < 300; i++) if (s.step(0.2) == Act::Restore) r++;
        ok(r == 0 && s.taken == 1, "calm, but the machine 90 % busy: nothing comes back");
        s.busy = 80; s.cost = 50;   // 4 cores × 20 % idle = 80 % of one core free; the step cost 50
        for (int i = 0; i < 300; i++) if (s.step(0.2) == Act::Restore) r++;
        ok(r == 0, "★ a step that cost 50 % of a core, 80 % of a core free: it waits (twice its cost must be free)");
        s.busy = 70;  // 120 % of a core free
        for (int i = 0; i < 10; i++) if (s.step(0.2) == Act::Restore) r++;
        ok(r == 1 && s.taken == 0, "...120 % free: it comes back");
    }
}

// ── the health card ──────────────────────────────────────────────────────────────────────────────────────────────
static void health() {
    std::printf("── the health card ──\n");
    vibehealth::BehindWatch w;
    w.note("bg", "Background decoding", 0, 0);
    w.note("bg", "Background decoding", 12000, 2000, true);
    ok(w.behind(2000).empty(), "★★ drops EXCUSED (a listener on, the easing has room): not named — no TOO HEAVY");
    w.note("bg", "Background decoding", 24000, 4000, false);
    ok(w.behind(4000).size() == 1 && w.behind(4000)[0] == "Background decoding", "drops not excused (nobody on, or everything eased): named");
    w.note("bg", "Background decoding", 24000, 70000, true);
    ok(w.behind(70000).empty(), "...for a minute after its last unexcused drop, then gone");
    w.note("bg", "Background decoding", 99000, 72000, true);
    ok(w.behind(72000).empty(), "an excused drop moves the baseline: no stale unexcused drop surfaces later");
    w.note("bg", "Background decoding", 99000, 74000, false);
    ok(w.behind(74000).empty(), "...(the next unexcused note with no new drops names nothing)");

    vibehealth::Health a, b;
    b.notes.push_back("Background decoding: eased for listeners (3 of 9 paused)");
    ok(vibehealth::differs(a, b), "a new note is a change (sent on change)");
    const std::string j = vibehealth::json(b);
    ok(j.find("\"notes\":[\"Background decoding: eased for listeners (3 of 9 paused)\"]") != std::string::npos, "★ sent as notes: " + j);
    ok(vibehealth::json(a).find("notes") == std::string::npos, "...and omitted when there are none (older clients unchanged)");
}

// ── the admin figure: background threads counted, the busiest named (Linux) ─────────────────────────────────────
static void threads() {
    std::printf("── the admin figure's threads ──\n");
#if defined(__linux__)
    vibeadmin::BgThreadSampler smp;
    std::atomic<bool> stop{false};
    auto spin = [&](const char* nm) {
        pthread_setname_np(pthread_self(), nm);
        (void)setpriority(PRIO_PROCESS, 0, 19);
        volatile double x = 0; while (!stop.load()) x = x + 1.0;
    };
    std::thread t1([&] { spin("vibe-bgtest1"); }), t2([&] { spin("vibe-bgtest2"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    smp.sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    const vibeadmin::BgThreadCpu r = smp.sample();
    stop.store(true); t1.join(); t2.join();
    ok(r.threads >= 2, "two background-rung threads counted (" + std::to_string(r.threads) + ")");
    ok(r.hottestPct > 40 && r.hottestName.rfind("vibe-bgtest", 0) == 0, "the busiest named: " + r.hottestName + " " + std::to_string((int)r.hottestPct) + " %");
#else
    vibeadmin::BgThreadSampler smp;
    ok(smp.sample().threads == 0, "not Linux: no per-thread figure (the page says nothing new)");
#endif
}

int main() {
    order();
    governor();
    health();
    threads();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
