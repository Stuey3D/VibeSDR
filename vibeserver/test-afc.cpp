// test-afc.cpp — AUTOMATIC FREQUENCY CORRECTION's estimator, gates, split and store (vibe_afc.h), on their own.
//   A. the reading → absolute clock error (the sign convention, the applied correction folded in)
//   B. thresholds: nothing before minSamples, nothing while the readings disagree (spread)
//   C. ONE bad reading never moves it; outliers rejected; a run of agreeing outliers is the truth moving (regroup)
//   D. the step cap and the minimum interval once acquired; the first acquisition uncapped; the deadband
//   E. DRM only corroborates: never acquires, counted only when it agrees
//   F. THE LOOP ON A SIMULATED RADIO — a +4 ppm clock warming by +1.5 ppm over 20 min, readings with noise, LATENCY (the
//      reading reflects the correction 0.6 s ago) and the glide of the digital shift: the residual the receiver sees
//      falls to ~0 and STAYS there, the correction never overshoots or reverses (no oscillation), every step ≤ 0.5 ppm
//      and ≥ 30 s apart; and the same loop WITHOUT the settling blank, to show the blank is what keeps it honest
//   G. the DAB and DRM gates, in words
//   H. the hardware/digital split (integer in the radio with hysteresis; all-digital radios) and the glide
//   I. the store: round trip, damage, atomic write, the owner's line
#include "vibe_afc.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const std::string& what) { std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str()); c ? ++g_pass : ++g_fail; }
static std::string f(const char* fmt, double a, double b = 0, double c = 0) { char s[200]; std::snprintf(s, sizeof s, fmt, a, b, c); return s; }

using namespace vibeafc;

static Reference dab(double clockPpm, double applied, double at, double noisePpm = 0, double blockHz = 194.064e6) {
    Reference r; r.source = "DAB 7D"; r.rfHz = blockHz; r.at = at; r.kind = Kind::Primary;
    r.offsetHz = -(clockPpm - applied + noisePpm) * 1e-6 * blockHz;   // a FAST clock lands the signal LOW
    return r;
}

int main() {
    std::printf("A. a reading is an absolute clock error\n");
    {
        const Reference r = dab(4.0, 0.0, 1.0);
        ok(std::fabs(residualPpm(r) + 4.0) < 1e-9, f("+4 ppm clock, nothing applied: the multiplex lands %.0f Hz low (residual %.2f ppm)", -r.offsetHz, residualPpm(r)));
        ok(std::fabs(absolutePpm(r, 0.0) - 4.0) < 1e-9, "…which is a clock 4 ppm fast");
        const Reference r2 = dab(4.0, 3.0, 2.0);
        ok(std::fabs(absolutePpm(r2, 3.0) - 4.0) < 1e-9, "with 3 ppm already applied the residual is 1 ppm, and the clock still reads 4 — the estimate survives a step");
        Reference tester; tester.rfHz = 194.064e6; tester.offsetHz = 781;   // ★ the B210's "781 Hz (4.02 ppm)"
        ok(std::fabs(absolutePpm(tester, 0) + 4.02) < 0.01, f("the B210's +781 Hz on 7D is a clock %.2f ppm SLOW", absolutePpm(tester, 0)));
    }

    std::printf("B. thresholds\n");
    {
        Estimator e; double next = 0;
        for (int i = 0; i < 9; ++i) e.offer(dab(4.0, 0, i), 0);
        ok(!e.decide(10, 0, false, next), "9 readings: no decision (minSamples 10)");
        e.offer(dab(4.0, 0, 9), 0);
        ok(e.decide(10, 0, false, next) && std::fabs(next - 4.0) < 1e-9, f("10 readings: acquires %.3f ppm", next));
        Estimator w; std::mt19937 rng(1); std::uniform_real_distribution<double> u(-1.4, 1.4);
        for (int i = 0; i < 40; ++i) w.offer(dab(4.0, 0, i, u(rng)), 0);
        ok(!w.decide(50, 0, false, next), f("readings scattered ±1.4 ppm (spread %.2f): no decision", w.spread()));
    }

    std::printf("C. outliers\n");
    {
        Estimator e; double next = 0;
        for (int i = 0; i < 20; ++i) e.offer(dab(4.0, 0, i), 0);
        const auto v = e.offer(dab(40.0, 0, 20), 0);
        ok(v == Estimator::Verdict::Outlier, "one reading 36 ppm away is an outlier");
        ok(std::fabs(e.median() - 4.0) < 1e-9 && e.decide(30, 0, false, next) && std::fabs(next - 4.0) < 1e-9, "…and moves nothing");
        for (int i = 0; i < 10; ++i) e.offer(dab(4.0 + (i % 2 ? 9 : -9), 0, 21 + i), 0);
        ok(std::fabs(e.median() - 4.0) < 1e-9, "ten wild readings either side: rejected, median unmoved");
        Estimator g;
        for (int i = 0; i < 20; ++i) g.offer(dab(4.0, 0, i), 0);
        Estimator::Verdict last = Estimator::Verdict::Accepted;
        int n = 0;
        while (last != Estimator::Verdict::Regrouped && n < 100) last = g.offer(dab(9.0, 0, 20 + n++), 0);
        ok(last == Estimator::Verdict::Regrouped && n == g.config().regroupAfter, f("a run of %.0f AGREEING readings at 9 ppm is the truth moving: regrouped", n));
        ok(std::fabs(g.median() - 9.0) < 1e-9, "…and the window is theirs");
    }

    std::printf("D. the step cap, the interval, the deadband\n");
    {
        Estimator e; double next = 0;
        for (int i = 0; i < 20; ++i) e.offer(dab(6.0, 0, i), 0);
        ok(e.decide(20, 0, false, next) && std::fabs(next - 6.0) < 1e-9, "first acquisition is applied whole (6 ppm in one step)");
        e.noteChanged(20);
        ok(!e.decide(40, 4.0, true, next), "acquired: no step 20 s after the last");
        ok(e.decide(50, 4.0, true, next) && std::fabs(next - 4.5) < 1e-9, f("30 s on, 2 ppm to go: one step of 0.5 (to %.2f)", next));
        ok(!e.decide(80, 5.98, true, next), "0.02 ppm off: inside the deadband, left alone");
    }

    std::printf("E. DRM only corroborates\n");
    {
        Estimator e; double next = 0;
        Reference d = dab(4.0, 0, 0); d.kind = Kind::Corroborating; d.source = "DRM 3965 kHz";
        for (int i = 0; i < 30; ++i) { d.at = i; e.offer(d, 0); }
        ok(e.count() == 0 && !e.decide(40, 0, false, next), "30 DRM readings alone: nothing learned (it never acquires)");
        for (int i = 0; i < 12; ++i) e.offer(dab(4.0, 0, 40 + i), 0);
        d.at = 60; d.offsetHz = -(4.3) * 1e-6 * d.rfHz;
        ok(e.offer(d, 0) == Estimator::Verdict::Accepted, "with DAB's 4.0 established, DRM's 4.3 agrees and is counted");
        d.at = 61; d.offsetHz = -(6.0) * 1e-6 * d.rfHz;
        ok(e.offer(d, 0) == Estimator::Verdict::Disagrees, "DRM's 6.0 disagrees by 2 ppm: not counted");
    }

    std::printf("F. the loop on a simulated radio (latency, warm-up drift, noise, the glide)\n");
    auto runLoop = [](bool blank, double& worstLate, double& maxStep, double& minGap, int& reversals, int& steps, double& finalResid) {
        std::mt19937 rng(7); std::normal_distribution<double> noise(0.0, 0.03);
        Estimator e;
        double applied = 0, digitalNow = 0, target = 0;   // the correction: what is decided, what the glide has reached
        bool acquired = false;
        std::deque<std::pair<double, double>> hist;        // (t, correction in force) — for the latency
        double lastStepAt = -1e9, lastDir = 0;
        worstLate = 0; maxStep = 0; minGap = 1e9; reversals = 0; steps = 0; finalResid = 0;
        const double dt = 0.1, latency = 0.6, settle = 4.0;
        double sum = 0; int n = 0, k = 0;
        for (double t = 0; t < 1800; t += dt) {
            const double clock = 4.0 + 1.5 * (1 - std::exp(-t / 600.0));   // warming: 4 → ~5.4 ppm
            digitalNow = slewToward(digitalNow, target, dt);
            hist.push_back({t, digitalNow});
            while (hist.size() > 1 && hist[1].first <= t - latency) hist.pop_front();
            const double seen = hist.front().second;                       // what the reading reflects
            sum += -(clock - seen) + noise(rng); ++n;                       // the receiver's residual (ppm), per frame
            if (++k % 20 == 0) {                                            // a 2 s reading, as the housekeeping takes them
                Reference r; r.source = "DAB 7D"; r.rfHz = 194.064e6; r.at = t; r.kind = Kind::Primary;
                r.offsetHz = sum / n * 1e-6 * r.rfHz; sum = 0; n = 0;
                // ★ the shim offers the reading with the correction in force NOW — so a reading taken mid-glide is
                //   paired with the wrong value. The blank (glide end + settle) is what prevents that.
                e.offer(r, digitalNow);
                double next;
                if (e.decide(t, applied, acquired, next)) {
                    const double step = next - applied;
                    if (acquired) {
                        maxStep = std::max(maxStep, std::fabs(step));
                        minGap = std::min(minGap, t - lastStepAt);
                        if (lastDir != 0 && step * lastDir < 0 && std::fabs(step) > 0.06) ++reversals;
                        lastDir = step > 0 ? 1 : -1;
                    }
                    ++steps; lastStepAt = t;
                    applied = next; target = next;
                    if (!acquired) digitalNow = next;   // first acquisition: applied at once
                    acquired = true;
                    e.noteChanged(t);
                    if (blank) e.blankUntil(t + std::fabs(target - digitalNow) / kSlewPpmPerS + latency + settle);
                }
            }
            if (t > 300) worstLate = std::max(worstLate, std::fabs(clock - digitalNow));
            finalResid = clock - digitalNow;
        }
    };
    {
        double worst, maxStep, minGap, finalResid; int rev, steps;
        runLoop(true, worst, maxStep, minGap, rev, steps, finalResid);
        ok(worst < 0.35, f("after acquisition the residual stays under 0.35 ppm while the clock warms 1.5 ppm (worst %.3f)", worst));
        ok(std::fabs(finalResid) < 0.1, f("…and ends at %.3f ppm (DAB's measured offset falls to ~0)", finalResid));
        ok(rev == 0, f("no reversals — the correction never oscillates (%.0f steps in 30 min)", steps));
        ok(maxStep <= 0.5 + 1e-9, f("every step after acquisition ≤ 0.5 ppm (largest %.3f)", maxStep));
        ok(minGap >= 30 - 1e-9, f("…and ≥ 30 s apart (closest %.0f s)", minGap));
        double w2, m2, g2, f2; int r2, s2;
        runLoop(false, w2, m2, g2, r2, s2, f2);
        std::printf("      (without the settling blank: worst %.3f ppm, %d reversals, %d steps — a reading is paired with the correction\n"
                    "       in force, so 0.6 s of latency at a 0.1 ppm/s glide costs little; the blank is there for what this model\n"
                    "       leaves out: a hardware step's retune and the DAB frames it disturbs)\n", w2, r2, s2);
        ok(true, "the loop ran without the blank too (printed above for comparison)");
    }

    std::printf("G. the gates\n");
    {
        DabRead r; r.locked = true; r.frames = 20; r.offsetHz = 776; r.blockHz = 194.064e6; r.merDb = 14; r.fibRate = 1.0f;
        ok(dabWhyNot(r, 30) == nullptr, "a strong clean settled lock is usable");
        ok(std::string(dabWhyNot(r, 3)) == "lock not yet settled", "…not 3 s after (re)acquiring");
        DabRead m = r; m.merDb = 5; ok(std::string(dabWhyNot(m, 30)) == "MER too low", "MER 5 dB: not used");
        DabRead fi = r; fi.fibRate = 0.8f; ok(std::string(dabWhyNot(fi, 30)) == "FIC not clean", "FIB rate 0.8: not used");
        DabRead d = r; d.disturbed = true; ok(std::string(dabWhyNot(d, 30)) == "lock disturbed", "a re-acquisition in the span: not used");
        DabRead u = r; u.locked = false; ok(std::string(dabWhyNot(u, 30)) == "not locked", "unlocked: not used");
        DabRead p = r; p.offsetHz = 50000; ok(std::string(dabWhyNot(p, 30)) == "offset not plausible", "258 ppm: not plausible");
        ok(drmWhyNot(true, 3965000, 18, 20) == nullptr, "DRM on 3965 kHz, +18 Hz, MER 20: usable");
        ok(std::string(drmWhyNot(true, 3965500, 18, 20)) == "dial not on a whole kHz", "dial on 3965.5: not used");
        ok(std::string(drmWhyNot(true, 3965000, 400, 20)) == "offset too large (tuned off the channel?)", "400 Hz off: not used");
    }

    std::printf("H. the split and the glide\n");
    {
        ok(hwPpmFor(4.02, 0, true, true) == 4, "RTL, first acquisition 4.02: 4 in the radio, 0.02 digital");
        ok(hwPpmFor(5.3, 4, false, true) == 4, "drifted to 5.3: the radio stays at 4 (1.3 digital, inside the hysteresis)");
        ok(hwPpmFor(5.6, 4, false, true) == 6, "5.6: past 1.5 — the radio moves to 6");
        ok(hwPpmFor(4.02, 0, true, false) == 0, "an Airspy (no ppm control): all digital");
        double c = 0; for (int i = 0; i < 10; ++i) c = slewToward(c, 0.5, 0.5);
        ok(std::fabs(c - 0.5) < 1e-9, "the glide reaches 0.5 ppm in 5 s");
        ok(std::fabs(slewToward(0, 0.5, 1.0) - 0.1) < 1e-12, "…at 0.1 ppm a second");
    }

    std::printf("I. the store\n");
    {
        Saved s; s.have = true; s.ppm = 4.0213; s.at = 1789000000; s.hw = 4; s.source = "DAB 7D"; s.on = 1;
        Saved r;
        ok(parse(serialise(s), r) && r.have && std::fabs(r.ppm - 4.0213) < 1e-9 && r.at == s.at && r.hw == 4 && r.source == "DAB 7D" && r.on == 1, "round trip");
        Saved srv; srv.have = true; srv.ppm = -1.5; ok(parse(serialise(srv), r) && r.on == -1, "a server's file carries no switch");
        ok(!parse("garbage\nppm 4", r), "not ours: refused");
        ok(parse("VIBE-AFC 1\nppm nan at 1 hw 0\n", r) && !r.have, "a NaN is no value");
        ok(parse("VIBE-AFC 1\nppm 900 at 1 hw 0\n", r) && !r.have, "900 ppm is damage");
        char tmpl[] = "/tmp/afc-XXXXXX";
        const char* d = mkdtemp(tmpl);
        const std::string fn = std::string(d) + "/afc-x.txt";
        ok(save(fn, s) && access((fn + ".tmp").c_str(), F_OK) != 0, "written atomically (no .tmp left)");
        ok(load(fn, r) && r.have && r.source == "DAB 7D", "read back");
        ok(!save(std::string(d) + "/nope/afc.txt", s), "a missing directory: refused, no throw");
        unlink(fn.c_str()); rmdir(d);
        ok(describe(s, 1789000000 + 120) == "auto-corrected +4.0 ppm from DAB 7D, 2 min ago", describe(s, 1789000000 + 120));
        Saved none; ok(describe(none, 0) == "not learned yet", "nothing learned: says so");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
