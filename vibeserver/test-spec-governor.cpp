// ★★★ THE WATERFALL GIVES WAY TO THE CPU (vibespec::Governor, vibe_spec_governor.h) — on a fake clock and a fake load,
//     so it says the same thing on any machine. The Pi 2 case (2026-10-17): DAB starving at 20 fps must drop to 10 and
//     then 5 within seconds; SSB on HF with room to spare must climb back to 20 — slowly; a box on the edge must SETTLE
//     on the lower rung, not see-saw; the owner's top and a listener's lower ask are never exceeded. And composed with the
//     audio-first gate (vibeqos::AudioFirst), each listener gets the LOWER of the two.
#include "vibe_spec_governor.h"
#include "vibe_wifi_qos.h"

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) {
    std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}

using vibespec::Governor;
using vibespec::Load;

static const Load kStarvedDab{97, true, 40, 3};   // the Pi 2 on DAB at 20 fps: dspCpu 97, demod queue full, CRIT
static const Load kBusy{75, false, 20, 2};        // coping, not comfortable — hold
static const Load kCalm{30, false, 5, 0};         // SSB on HF

/** Tick every 2 s (the hotplug thread's cadence) from `t` for `ms`, with `l`. Returns the new time. */
static double run(Governor& g, double t, double ms, const Load& l, double want, int* changes = nullptr) {
    for (double end = t + ms; t < end; t += 2000) if (g.feed(t, l, want) && changes) ++*changes;
    return t;
}

int main() {
    std::printf("── fast down: starved at 20 fps ──\n");
    {
        Governor g;
        double t = 0;
        ok(g.ceiling() == 0 && g.effective(20) == 20, "fresh: no ceiling, 20 runs at 20");
        t = run(g, t, 2000, kStarvedDab, 20);
        ok(g.ceiling() == 0, "one starved tick is not enough (a single bad window is not starvation)");
        t = run(g, t, 4000, kStarvedDab, 20);
        ok(g.ceiling() == 10, "starved ~4 s → 10 fps");
        const double at10 = t;
        t = run(g, t, 6000, kStarvedDab, 20);
        ok(g.ceiling() == 10, "…and holds 10 through the settle (the load figure must see the new rate first)");
        t = run(g, t, 4000, kStarvedDab, 20);
        ok(g.ceiling() == 5, "still starved after the settle → 5 fps (" + std::to_string((int)((t - at10) / 1000)) + " s later)");
        t = run(g, t, 60000, kStarvedDab, 20);
        ok(g.ceiling() == 5, "never below 5: the waterfall slows, it does not stop");
        ok(g.downs() == 2, "two steps, one rung at a time");
    }

    std::printf("── slow up: the load goes away (DAB → SSB on HF) ──\n");
    {
        Governor g;
        double t = run(g, 0, 20000, kStarvedDab, 20);
        ok(g.ceiling() == 5, "down at 5");
        t = run(g, t, 50000, kCalm, 20);
        ok(g.ceiling() == 5, "50 s calm: still 5 (slow up)");
        t = run(g, t, 12000, kCalm, 20);
        ok(g.ceiling() == 10, "~60 s calm → 10");
        t = run(g, t, 58000, kCalm, 20);
        ok(g.ceiling() == 10, "…and another full calm minute before the next rung");
        t = run(g, t, 4000, kCalm, 20);
        ok(g.ceiling() == 0 && g.effective(20) == 20, "→ unlimited: back to the owner's 20");
    }

    std::printf("── busy is not starved: hold, don't climb, don't fall ──\n");
    {
        Governor g;
        double t = run(g, 0, 8000, kStarvedDab, 20);
        ok(g.ceiling() == 10, "down at 10");
        int ch = 0;
        t = run(g, t, 30 * 60000.0, kBusy, 20, &ch);
        ok(g.ceiling() == 10 && ch == 0, "30 min busy (dspCpu 75, CPU HIGH): stays at 10, no changes");
        // A busy tick in the middle of a calm spell restarts the calm count.
        t = run(g, t, 50000, kCalm, 20);
        t = run(g, t, 2000, kBusy, 20);
        t = run(g, t, 50000, kCalm, 20);
        ok(g.ceiling() == 10, "calm must be CONTINUOUS: 50 s + a busy blip + 50 s does not climb");
    }

    std::printf("── no hunting: a box on the edge settles on the lower rung ──\n");
    {
        // A fake box: starved whenever the engine runs above 10 fps, calm at 10 or below.
        Governor g;
        double t = 0; int changes = 0;
        for (double end = 3 * 3600 * 1000.0; t < end; t += 2000) {
            const Load l = g.effective(20) > 10 ? kStarvedDab : kCalm;
            if (g.feed(t, l, 20)) ++changes;
        }
        ok(g.ceiling() == 10, "after 3 h it sits at 10");
        ok(changes <= 17, "and changed only " + std::to_string(changes) + " times in 3 hours (≤ 17: each failed climb doubles the wait)");
        ok(g.upWaitMs() == Governor::kUpMaxMs, "the wait before a climb has backed off to 64 min");
        // Count the changes in the NEXT hour: at the cap, at most one try an hour (a try is up + down = 2).
        int lastHour = 0;
        for (double end = t + 3600 * 1000.0; t < end; t += 2000) {
            const Load l = g.effective(20) > 10 ? kStarvedDab : kCalm;
            if (g.feed(t, l, 20)) ++lastHour;
        }
        ok(lastHour <= 2, "steady state: " + std::to_string(lastHour) + " changes in an hour (≤ 2: one ~4 s probe)");
        // A mode change (DAB → SSB): the lesson no longer applies, so the next climb waits only the base minute.
        g.newCircumstances();
        for (double end = t + 64000; t < end; t += 2000) g.feed(t, kCalm, 20);
        ok(g.ceiling() == 0, "new circumstances + calm: climbs again after ~60 s, not 64 min");
    }

    std::printf("── the penalty is forgiven ──\n");
    {
        Governor g;
        double t = run(g, 0, 8000, kStarvedDab, 20);           // → 10
        t = run(g, t, 62000, kCalm, 20);                       // → unlimited (on probation)
        ok(g.ceiling() == 0, "climbed to unlimited");
        t = run(g, t, 10000, kStarvedDab, 20);                 // starves inside 2 min → back to 10, wait doubled
        ok(g.ceiling() == 10 && g.upWaitMs() == 2 * Governor::kUpAfterMs, "a climb that starves inside 2 min: back down, wait doubled");
        t = run(g, t, 121 * 60000.0, kBusy, 20);               // two hours with no failed try
        t = run(g, t, 2000, kBusy, 20);
        ok(g.upWaitMs() == Governor::kUpAfterMs, "after 2 h without a failed try the wait is back to 60 s");
        // A starve LONG after a climb is a new event, not a failed try.
        Governor h;
        double u = run(h, 0, 8000, kStarvedDab, 20);
        u = run(h, u, 62000, kCalm, 20);
        u = run(h, u, 10 * 60000.0, kCalm, 20);
        u = run(h, u, 8000, kStarvedDab, 20);
        ok(h.ceiling() == 10 && h.upWaitMs() == Governor::kUpAfterMs, "starved 10 min after a climb: down, but no penalty");
    }

    std::printf("── the owner's top and a listener's ask are never exceeded ──\n");
    {
        Governor g;
        ok(g.effective(5) == 5 && g.effective(10) == 10, "no ceiling: the want passes through untouched");
        run(g, 0, 8000, kStarvedDab, 20);
        ok(g.effective(20) == 10 && g.effective(8) == 8, "ceiling 10: 20 → 10, a listener's 8 stays 8 (never raised)");
        Governor o;                                            // owner capped at 15 (an odd hand-set value)
        run(o, 0, 8000, kStarvedDab, 15);
        ok(o.effective(15) == 10, "owner 15, starved → 10");
        Governor f;                                            // fastest listener asked 10: the first cut is 5
        run(f, 0, 8000, kStarvedDab, 10);
        ok(f.ceiling() == 5 && f.effective(10) == 5, "engine already at 10: the first cut goes straight to 5 (a cut that cuts)");
        // ★ THE NEW SERVER DEFAULT (2026-10-20): owner Default · 10 — the top is 10, so the ladder is 10 → 5 → back to 10.
        Governor d; int dch = 0;
        run(d, 0, 8000, kStarvedDab, 10, &dch);
        ok(d.effective(10) == 5 && dch == 1, "owner Default · 10, starved: one step, 10 → 5");
        run(d, 8000, 8 * 60000, kCalm, 10);
        ok(d.effective(10) == 10 && d.effective(20) <= 20, "…and calm again: back to the owner's 10, never above it");
        ok(d.effective(10) == 10, "the climb tops out at the owner's 10 (effective never raises a want)");
        Governor q;                                            // already at 5 (owner Half · 5): nothing to cut
        int ch = 0;
        run(q, 0, 60000, kStarvedDab, 5, &ch);
        ok(q.ceiling() == 0 && ch == 0, "owner at 5: starved changes nothing — no rung below");
        Governor i;                                            // nobody listening: the 2 fps idle floor
        run(i, 0, 60000, kStarvedDab, 2, &ch);
        ok(i.effective(2) == 2, "the 2 fps idle floor is never touched");
    }

    std::printf("── which loads count ──\n");
    {
        ok(vibespec::starved({95, false, 0, 0}), "dspCpu 95 = starved");
        ok(vibespec::starved({70, true, 0, 0}), "the demod queue blocking under load = starved (the worst block)");
        ok(vibespec::starved({40, true, 150, 0}), "the demod queue blocking with the IQ backlog growing = starved");
        ok(!vibespec::starved({40, true, 0, 0}), "blocking at 40 % with no backlog = the consumer pacing itself, not starved");
        ok(vibespec::starved({40, false, 300, 0}), "300 ms of IQ backlog = starved");
        ok(vibespec::starved({40, false, 0, 3}), "the whole machine CRITICAL = starved (the Pi 2's DAB case)");
        ok(!vibespec::starved({85, false, 100, 2}), "dspCpu 85 + CPU HIGH = busy, not starved");
        ok(!vibespec::calm({65, false, 0, 0}), "dspCpu 65 is not calm");
        ok(!vibespec::calm({30, false, 0, 2}), "CPU HIGH is not calm");
        ok(vibespec::calm({30, false, 50, 1}), "dspCpu 30, CPU WARM = calm");
    }

    std::printf("── composed with the audio-first gate: each listener gets the LOWER ──\n");
    {
        // Engine frames at the governor's rate; each one offered to the listener's gate, as onSpectrum → queueOut does.
        auto delivered = [](double engineFps, bool audioQueued) {
            vibeqos::AudioFirst gate;
            int sent = 0;
            const double frameMs = 1000.0 / engineFps;
            for (double t = 0; t < 20000; t += 10) {
                if (audioQueued) gate.noteAudio(t, 20000, 16000);   // 1.25 s of Opus unsent: a real backlog
                else             gate.noteAudio(t, 100, 16000);
                if (std::fmod(t, frameMs) < 10 && t >= 10000 && gate.waterfallMaySend(t)) ++sent;   // count the last 10 s
            }
            return sent / 10.0;
        };
        Governor g;
        run(g, 0, 8000, kStarvedDab, 20);
        const double eng = g.effective(20);
        ok(std::fabs(delivered(eng, false) - 10) < 0.6, "governor 10, audio clear: the listener gets 10 fps");
        ok(std::fabs(delivered(eng, true) - 2) < 0.6, "governor 10, audio queued: the gate's 2 fps wins");
        ok(std::fabs(delivered(5, false) - 5) < 0.6, "governor 5, audio clear: 5 fps");
        ok(std::fabs(delivered(5, true) - 2) < 0.6, "governor 5, audio queued: 2 fps — still the lower");
        ok(std::fabs(delivered(20, false) - 20) < 0.6, "no governor, audio clear: the full 20");
    }

    std::printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    std::printf("── an idle box whose demod queue reports full is NOT starved (2026-10-20) ──\n");
    {
        // The Mac on a network radio: dspCpu 2 %, demod waits ~30/s, no backlog — the consumer pacing itself.
        const Load idleBlocked{2, true, 0, 0};
        Governor g;
        double t = 0; int changes = 0;
        t = run(g, t, 60000, idleBlocked, 20, &changes);
        ok(g.ceiling() == 0 && changes == 0, "a minute of idle-but-blocked never lowers the waterfall");
        // …and from a lowered ceiling it still counts as calm, so the waterfall climbs back.
        Governor h; double u = 0;
        u = run(h, u, 8000, kStarvedDab, 20);
        ok(h.ceiling() == 10, "(setup) starved → 10");
        u = run(h, u, 70000, idleBlocked, 20);
        ok(h.effective(20) == 20, "idle-but-blocked is calm: back to 20 after the calm spell");
        // A blocked queue WITH load is still starvation.
        Governor b; double v = 0;
        v = run(b, v, 8000, Load{70, true, 20, 1}, 20);
        ok(b.ceiling() == 10, "blocked under load (dspCpu 70) still steps down");
    }
    return fails ? 1 : 0;
}
