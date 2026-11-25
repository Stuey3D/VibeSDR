// test-bg-heavy.cpp — WHEN BACKGROUND DECODING IS ON THE HEALTH PILL (beta94, the Lenovo, Stuart 11-25).
//
// What happened: an 8-core i5 at load 0.05, Stuart listening to an ORBCOMM pass at 137.66 MHz on the RSP1A, and the
// pill said "Background decoding: TOO HEAVY" — "not even background decoding right now either, as I'm tuned away from
// the background decoders". The radio's own log, the minutes before:
//     17:36:48 background decoding: profile 1, RF centre 1200.0 kHz, 6 VFO(s), 6 decoder(s) running
//     17:37:48 background decoding: 0.5 s of IQ dropped in the last minute   (Band: decode 0.00 s behind, IQ blocks dropped 0)
//     17:38:48 background decoding: 0.5 s of IQ dropped in the last minute
//     17:39:41 background decoding stopped — no background VFO is inside the radio's window now   (a listener arrived)
//     17:39:47 [sat] the shared dial picked ORBCOMM FM116 (137.663 MHz nfm, Doppler on, decoder orbcomm)
// Two faults: (1) the steady ~0.5 s a minute the DSP thread gives way (it never waits for background decoding's queue)
// named it whenever nobody was listening — any drop counted; (2) once it STOPPED the name stayed up for the next minute
// (vibehealth::BehindWatch holds a name a minute after its last drop), so the arriving listener saw TOO HEAVY for work
// that had just ended. Stuart's rule: "the pill is to inform users why they might be having a sluggish connection,
// lagging, broken audio etc." — background decoding is on it only when its load is what the LISTENER feels
// (vibe_bg_ease.h PillWatch); idle, paused, tuned away is the admin page's. This drives a real BgRunner through the
// Lenovo's sequence, with the old rule (BehindWatch fed the same numbers) beside it to show the fault, then a real
// shortfall (the listener's own IQ dropping) through the governor's easeTick.
#include "vibe_bgdecode.h"
#include "vibe_health.h"

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); c ? passes++ : fails++; }

static constexpr double kRate = 3000000;   // the Lenovo's RSP1A
static constexpr int64_t kTick = 2000;     // the health tick

/** Lose `sec` seconds of capture the way the DSP thread gives way: the worker holds its queue, the chunk is not taken. */
static void giveWay(bg::BgRunner& r, double sec) {
    std::vector<cf32> v((size_t)std::lround(sec * kRate), cf32(0, 0));
    std::atomic<bool> held{false}, done{false};
    std::thread worker([&] {   // ★ the worker's side, on its own thread (a std::mutex is never try-locked by its holder)
        auto hold = r.holdQueueForTest();
        held.store(true);
        while (!done.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    r.feedIq(v.data(), (int)v.size());
    done.store(true);
    worker.join();
}

/** The pill, both ways, fed the same numbers every tick: the old rule (BehindWatch while running) and the new one. */
struct Pill {
    vibehealth::BehindWatch old;
    bg::ease::PillWatch now;
    bool oldSays = false, newSays = false;
    void tick(bg::BgRunner& r, int64_t t, bool listenerOn) {
        if (r.running()) old.note("bg", "Background decoding", r.droppedSamples(), t, r.dropsExcused());
        oldSays = !old.behind(t).empty();
        newSays = now.tick(t, r.running(), r.decodingNow(), listenerOn, r.degradingListener());
        // ★ and what goes on the wire: no background entry at all unless the new rule says so
        vibehealth::Health h;
        if (newSays) h.warns.push_back("Server CPU overloaded \xe2\x80\x94 audio or waterfall may stutter");
        json = vibehealth::json(h);
    }
    std::string json;
};

int main() {
    std::printf("── the Lenovo's sequence: background decoding with nobody on, then a listener tunes to 137.66 MHz ──\n");
    FileSlots slots("", [] { return 4; });
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = kRate; p.centreHz = 1200000; p.fftSize = 65536; p.slots = &slots;
    p.profile.centreHz = 1200000;
    p.profile.vfos = { bg::Vfo{489500, "usb", "navtex"}, bg::Vfo{517500, "usb", "navtex"} };
    p.log = [](const std::string&) {};
    r.start(p);
    r.slotTickForTest();
    ok(r.running() && r.decodingNow() == 2, "running at 1200 kHz, nobody on: both NAVTEX VFOs decoding (decodingNow 2)");

    Pill pill;
    int64_t t = 0;
    pill.tick(r, t, false);
    // ★ two minutes of the steady give-way: 0.5 s a minute = 1/60 s every 2 s tick (what the Lenovo logged, every minute)
    bool oldNamed = false, newNamed = false;
    for (int i = 0; i < 60; i++) {
        t += kTick; giveWay(r, 0.5 / 30.0); pill.tick(r, t, false);
        oldNamed = oldNamed || pill.oldSays; newNamed = newNamed || pill.newSays;
    }
    ok(r.droppedSamples() > 0, "the give-way really dropped IQ (" + std::to_string((double)r.droppedSamples() / kRate).substr(0, 4) + " s)");
    ok(oldNamed, "the OLD rule names background decoding on the pill for the steady give-way (the fault reproduced)");
    ok(!newNamed, "the new rule does not: losing a little of its own IQ costs a listener nothing");

    // ★ the listener arrives and picks ORBCOMM at 137.66 MHz: every VFO leaves the window and the shim stops the runner
    t += kTick; giveWay(r, 0.5 / 30.0);
    r.retarget(137660000, 0, true);
    r.slotTickForTest();
    ok(r.decodingNow() == 0, "tuned to 137.66 MHz: nothing decoding (decodingNow 0)");
    pill.tick(r, t, true);
    ok(!pill.newSays, "…nothing on the pill at once");
    r.stop(false);
    ok(!r.running() && r.decodingNow() == 0 && !r.degradingListener(), "stopped (\"no background VFO is inside the radio's window now\")");
    bool oldAfter = false, newAfter = false, wireClean = true;
    for (int i = 0; i < 20; i++) {
        t += kTick; pill.tick(r, t, true);
        oldAfter = oldAfter || pill.oldSays; newAfter = newAfter || pill.newSays;
        wireClean = wireClean && pill.json.find("warn") == std::string::npos && pill.json.find("ackground") == std::string::npos;
    }
    // ★ BehindWatch itself now forgets a key the tick it is not fed (its decoder stopped) — the hold was the second fault
    ok(!oldAfter, "BehindWatch (the old path) no longer holds a STOPPED decoder's name either");
    ok(!newAfter && wireClean, "the new rule: stopped = no background entry in the health JSON at all");

    std::printf("\n── a real shortfall the listener feels still says so ──\n");
    p.listenerOn = true;
    r.start(p);
    r.slotTickForTest();
    ok(r.decodingNow() == 2, "running in range with a listener on: 2 decoding");
    Pill p2;
    uint64_t lisDrops = 0;
    bg::BgRunner::EaseInputs in;
    in.machineBusyPct = 95; in.cores = 4;
    in.nowMs = t; in.listenerIqDrops = lisDrops; r.easeTick(in);
    t += kTick; p2.tick(r, t, true);
    ok(!p2.newSays, "the listener keeping up: nothing on the pill");
    // ★ the LISTENER's own IQ drops (the DSP thread short of CPU) while background decoding runs
    t += kTick; lisDrops += 3; in.nowMs = t; in.listenerIqDrops = lisDrops; r.easeTick(in);
    p2.tick(r, t, true);
    ok(r.degradingListener() && p2.newSays, "the listener's IQ dropping while it decodes: on the pill");
    ok(p2.json.find("\"warn\":[\"Server CPU overloaded") != std::string::npos, "…in the listener's words: " + p2.json.substr(p2.json.find("\"warn\"")));
    // ★ the listener leaves: gone at once
    p2.tick(r, t + kTick, false);
    ok(!p2.newSays, "the listener left: gone at once");
    r.stop(false);

    std::printf("\n── PillWatch alone ──\n");
    bg::ease::PillWatch w;
    ok(w.tick(0, true, 3, true, true), "running, decoding, a listener, their receiver behind: shown");
    ok(w.tick(10000, true, 3, true, false), "one calm tick does not blink it (held 30 s)");
    ok(!w.tick(40001, true, 3, true, false), "calm 30 s: gone");
    ok(!w.tick(42000, true, 0, true, true), "nothing decoding: never (background decoding is not what they feel)");
    ok(!w.tick(44000, false, 3, true, true), "stopped: never");
    ok(!w.tick(46000, true, 3, false, true), "nobody listening: never (nobody to tell)");

    std::printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
