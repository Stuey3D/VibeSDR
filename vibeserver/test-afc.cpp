// test-afc.cpp — AUTOMATIC FREQUENCY CORRECTION's estimator, gates, split and store (vibe_afc.h), on their own.
//   A. the reading → absolute clock error (the sign convention, the applied correction folded in)
//   B. thresholds: nothing before minSamples, nothing while the readings disagree (spread)
//   C. ONE bad reading never moves it; outliers rejected; a run of agreeing outliers is the truth moving (regroup)
//   D. the step cap and the minimum interval once acquired; the first acquisition uncapped; the deadband
//   E. DRM only corroborates: never acquires, counted only when it agrees
//   F. THE LOOP ON A SIMULATED RADIO — a +4 ppm clock warming by +1.5 ppm over 20 min, readings with noise, LATENCY (the
//      reading reflects the correction 0.6 s ago) and the glide of the digital shift: the residual the receiver sees
//      falls to ~0 and STAYS there, the correction never overshoots or reverses (no oscillation), every step ≤ 0.5 ppm
//      and ≥ 30 s apart; and the same loop WITHOUT the settling blank, printed for comparison
//   G. the DAB and DRM gates, in words
//   H. the per-driver caps and the glide
//   I. the store: round trip, damage, atomic write
//   J. THE SAFETY RULES (Stuart, 2026-11-12) on the Controller: defaults (on), an upgrade with nothing saved applies
//      nothing until confident (agreement over time or across blocks), a wrong 1 kHz carrier shift on one block
//      disagrees with another and nothing is applied, a reading beyond the cap is rejected, auto-revert on a step that
//      made things worse, suspension after the second, Reset and Turn off back to the manual ppm exactly, a saved value
//      learned on another manual ppm is not restored, and the owner's line
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
        for (int i = 0; i < 9; ++i) e.offer(dab(4.0, 0, 15 * i), 0);
        ok(!e.decide(200, 0, false, next), "9 readings: no decision (minSamples 10)");
        e.offer(dab(4.0, 0, 135), 0);
        ok(e.decide(200, 0, false, next) && std::fabs(next - 4.0) < 1e-9, f("10 readings over 135 s: acquires %.3f ppm", next));
        Estimator q;
        for (int i = 0; i < 30; ++i) q.offer(dab(4.0, 0, 2 * i), 0);
        ok(!q.decide(60, 0, false, next), "30 clean readings over only 58 s on ONE block: not yet (agreement over time: 120 s)");
        Estimator w; std::mt19937 rng(1); std::uniform_real_distribution<double> u(-1.4, 1.4);
        for (int i = 0; i < 40; ++i) w.offer(dab(4.0, 0, i, u(rng)), 0);
        ok(!w.decide(50, 0, false, next), f("readings scattered ±1.4 ppm (spread %.2f): no decision", w.spread()));
    }

    std::printf("C. outliers\n");
    {
        Estimator e; double next = 0;
        for (int i = 0; i < 20; ++i) e.offer(dab(4.0, 0, 10 * i), 0);
        const auto v = e.offer(dab(40.0, 0, 200), 0);
        ok(v == Estimator::Verdict::Outlier, "one reading 36 ppm away is an outlier");
        ok(std::fabs(e.median() - 4.0) < 1e-9 && e.decide(210, 0, false, next) && std::fabs(next - 4.0) < 1e-9, "…and moves nothing");
        for (int i = 0; i < 10; ++i) e.offer(dab(4.0 + (i % 2 ? 9 : -9), 0, 201 + i), 0);
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
        for (int i = 0; i < 20; ++i) e.offer(dab(6.0, 0, 10 * i), 0);
        ok(e.decide(200, 0, false, next) && std::fabs(next - 6.0) < 1e-9, "first acquisition is decided whole (6 ppm; the caller glides it in)");
        e.noteChanged(200);
        ok(!e.decide(220, 4.0, true, next), "acquired: no step 20 s after the last");
        ok(e.decide(230, 4.0, true, next) && std::fabs(next - 4.5) < 1e-9, f("30 s on, 2 ppm to go: one step of 0.5 (to %.2f)", next));
        ok(!e.decide(260, 5.98, true, next), "0.02 ppm off: inside the deadband, left alone");
    }

    std::printf("E. DRM only corroborates\n");
    {
        Estimator e; double next = 0;
        Reference d = dab(4.0, 0, 0); d.kind = Kind::Corroborating; d.source = "DRM 3965 kHz";
        for (int i = 0; i < 30; ++i) { d.at = i; e.offer(d, 0); }
        ok(e.count() == 0 && !e.decide(40, 0, false, next), "30 DRM readings alone: nothing learned (it never acquires)");
        for (int i = 0; i < 12; ++i) e.offer(dab(4.0, 0, 40 + 15 * i), 0);
        d.at = 300; d.offsetHz = -(4.3) * 1e-6 * d.rfHz;
        ok(e.offer(d, 0) == Estimator::Verdict::Accepted, "with DAB's 4.0 established, DRM's 4.3 agrees and is counted");
        d.at = 301; d.offsetHz = -(6.0) * 1e-6 * d.rfHz;
        ok(e.offer(d, 0) == Estimator::Verdict::Disagrees, "DRM's 6.0 disagrees by 2 ppm: not counted");
    }

    std::printf("F. the loop on a simulated radio (latency, warm-up drift, noise, the glide)\n");
    // ★ THE WHOLE CONTROLLER (estimator, caps, self-check) in the loop, as the shim runs it every 2 s.
    auto runLoop = [](bool blank, double& worstLate, double& maxStep, double& minGap, int& reversals, int& steps, double& finalResid,
                      int& reverts) {
        std::mt19937 rng(7); std::normal_distribution<double> noise(0.0, 0.03);
        Controller c(100);
        double digitalNow = 0, target = 0;                 // the correction: what is decided, what the glide has reached
        std::deque<std::pair<double, double>> hist;        // (t, correction in force) — for the latency
        double lastStepAt = -1e9, lastDir = 0;
        worstLate = 0; maxStep = 0; minGap = 1e9; reversals = 0; steps = 0; finalResid = 0; reverts = 0;
        const double dt = 0.1, latency = 0.6, settle = 4.0;
        double sum = 0; int n = 0, k = 0;
        bool acquired = false;
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
                const Action a = c.offer(r, 14.0f, 11, digitalNow, t, 1789000000 + int64_t(t));
                if (a.kind == Action::Revert || a.kind == Action::Suspend) ++reverts;
                if (a.kind == Action::Apply) {
                    const double step = a.to - a.from;
                    if (acquired) {
                        maxStep = std::max(maxStep, std::fabs(step));
                        minGap = std::min(minGap, t - lastStepAt);
                        if (lastDir != 0 && step * lastDir < 0 && std::fabs(step) > 0.06) ++reversals;
                        lastDir = step > 0 ? 1 : -1;
                    }
                    acquired = true;
                    ++steps; lastStepAt = t;
                }
                if (a.kind != Action::None) {
                    target = c.target();
                    if (blank) c.settleUntil(t + std::fabs(target - digitalNow) / kSlewPpmPerS + latency + settle);
                }
            }
            if (t > 600) worstLate = std::max(worstLate, std::fabs(clock - digitalNow));
            finalResid = clock - digitalNow;
        }
    };
    {
        double worst, maxStep, minGap, finalResid; int rev, steps, reverts;
        runLoop(true, worst, maxStep, minGap, rev, steps, finalResid, reverts);
        ok(reverts == 0, "the self-check never fires on an honest radio (no reverts)");
        ok(worst < 0.35, f("from 10 min on (acquired, glided in, confirmed) the residual stays under 0.35 ppm while the clock warms 1.5 ppm (worst %.3f)", worst));
        ok(std::fabs(finalResid) < 0.1, f("…and ends at %.3f ppm (DAB's measured offset falls to ~0)", finalResid));
        ok(rev == 0, f("no reversals — the correction never oscillates (%.0f steps in 30 min)", steps));
        ok(maxStep <= 0.5 + 1e-9, f("every step after acquisition ≤ 0.5 ppm (largest %.3f)", maxStep));
        ok(minGap >= 30 - 1e-9, f("…and ≥ 30 s apart (closest %.0f s)", minGap));
        double w2, m2, g2, f2; int r2, s2, v2;
        runLoop(false, w2, m2, g2, r2, s2, f2, v2);
        std::printf("      (without the settling blank: worst %.3f ppm, %d reversals, %d steps, %d reverts — the self-check would be\n"
                    "       judging readings taken mid-glide)\n", w2, r2, s2, v2);
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

    std::printf("H. the caps and the glide\n");
    {
        ok(capFor("rtl") == 100 && capFor("hackrf") == 30 && capFor("usrp") == 10 && capFor("sdrplay") == 10
           && capFor("airspy") == 10 && capFor("airspyhf") == 10, "caps: RTL 100, HackRF 30, the TCXO radios 10 ppm");
        double c = 0; for (int i = 0; i < 10; ++i) c = slewToward(c, 0.5, 0.5);
        ok(std::fabs(c - 0.5) < 1e-9, "the glide reaches 0.5 ppm in 5 s");
        ok(std::fabs(slewToward(0, 0.5, 1.0) - 0.1) < 1e-12, "…at 0.1 ppm a second");
    }

    std::printf("I. the store\n");
    {
        Saved s; s.have = true; s.ppm = 4.0213; s.at = 1789000000; s.hw = 2; s.source = "DAB 7D"; s.on = 1; s.reverts = 1;
        Saved r;
        ok(parse(serialise(s), r) && r.have && std::fabs(r.ppm - 4.0213) < 1e-9 && r.at == s.at && r.hw == 2 && r.source == "DAB 7D"
           && r.on == 1 && r.reverts == 1 && !r.suspended, "round trip");
        Saved su; su.suspended = true; su.reverts = 2;
        ok(parse(serialise(su), r) && r.suspended && r.reverts == 2 && !r.have, "a suspension survives a restart");
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
    }

    std::printf("J. the safety rules, on the Controller\n");
    {
        const int64_t U = 1789000000;
        // feed: a clock of `clock` ppm on top of the manual, `n` readings `dt` apart from t0 on one block, the correction in force `inForce`
        auto feed = [&](Controller& c, double clock, double inForce, double t0, int n, double dt, double blockHz, const char* src,
                        float mer, int block, std::vector<Action>* acts = nullptr) {
            for (int i = 0; i < n; ++i) {
                Reference r = dab(clock, inForce, t0 + i * dt, 0, blockHz); r.source = src;
                const Action a = c.offer(r, mer, block, inForce, t0 + i * dt, U + int64_t(t0 + i * dt));
                if (a.kind != Action::None && acts) acts->push_back(a);
                if (a.kind != Action::None) return a;
            }
            return Action{};
        };
        Controller def;
        ok(def.on() && def.target() == 0 && !def.acquired(), "default: ON, and nothing applied (the radio exactly as set)");
        ok(def.describe(U).find("nothing applied yet") != std::string::npos, "…and says so: " + def.describe(U));

        // ── an upgrade: no saved value, a manual ppm of 2 — nothing until confident ──
        Controller up(100); up.restore(Saved{}, 2);
        Action a = feed(up, 3.0, 0, 0, 30, 2, 194.064e6, "DAB 7D", 14, 11);
        ok(a.kind == Action::None && up.target() == 0, "upgrade: 60 s of clean lock on one block — still nothing applied");
        a = feed(up, 3.0, 0, 60, 40, 2, 194.064e6, "DAB 7D", 14, 11);
        ok(a.kind == Action::Apply && std::fabs(a.to - 3.0) < 1e-9, f("…past 120 s it applies %+.2f ppm ON TOP of the manual 2", a.to));
        ok(up.manual() == 2, "the manual ppm is untouched");

        // ── across blocks: two blocks agreeing acquire sooner ──
        Controller two(100);
        feed(two, 3.0, 0, 0, 8, 2, 194.064e6, "DAB 7D", 14, 11);
        a = feed(two, 3.0, 0, 16, 8, 2, 225.648e6, "DAB 12B", 14, 30);
        ok(a.kind == Action::Apply && std::fabs(a.to - 3.0) < 1e-9, "two blocks that agree: acquired in 32 s");

        // ── rule 2: a wrong integer carrier shift (exactly 1 kHz) on one block disagrees with the other ──
        Controller slip(100);
        for (int i = 0; i < 40; ++i) {
            Reference r1 = dab(3.0, 0, 4 * i, 0, 194.064e6); r1.source = "DAB 7D";
            Reference r2 = dab(3.0, 0, 4 * i + 2, 0, 225.648e6); r2.source = "DAB 12B"; r2.offsetHz += 1000;   // ★ a 1 kHz slip
            const Action x1 = slip.offer(r1, 14, 11, 0, 4 * i, U), x2 = slip.offer(r2, 14, 30, 0, 4 * i + 2, U);
            if (x1.kind != Action::None || x2.kind != Action::None) { a = x1.kind != Action::None ? x1 : x2; break; }
            a = Action{};
        }
        ok(a.kind == Action::None && slip.target() == 0 && slip.estimator().sourcesDisagree(),
           "a 1 kHz carrier slip on 12B (3.0 vs 7.4 ppm): the references disagree, NOTHING applied in 160 s");
        ok(slip.lastWhyNot().find("disagree") != std::string::npos, "…and it says why: " + slip.lastWhyNot());

        // ── rule 1: beyond the cap ──
        Controller tc(capFor("usrp"));
        a = feed(tc, 14.0, 0, 0, 80, 2, 194.064e6, "DAB 7D", 14, 11);
        ok(a.kind == Action::None && tc.target() == 0 && tc.capRejects() == 80, "a USRP reading 14 ppm (cap 10): every reading rejected, nothing applied");
        ok(tc.lastWhyNot().find("beyond this radio") != std::string::npos, "…logged: " + tc.lastWhyNot());
        Controller m(100); m.setManual(95);
        a = feed(m, 8.0, 0, 0, 80, 2, 194.064e6, "DAB 7D", 14, 11);
        ok(a.kind == Action::None, "an RTL with a manual 95: +8 more would be 103 in all — rejected");

        // ── rule 3: auto-revert, then suspension ──
        Controller rv(100);
        a = feed(rv, 3.0, 0, 0, 70, 2, 194.064e6, "DAB 7D", 14, 11);
        ok(a.kind == Action::Apply, "acquired 3.0");
        rv.settleUntil(140 + 40);
        // the radio does NOT behave as the readings said (a false reference): after the change the residual is 6 ppm
        Action b = feed(rv, 9.0, 3.0, 182, 10, 2, 194.064e6, "DAB 7D", 14, 11);
        ok(b.kind == Action::Revert && std::fabs(b.to) < 1e-9 && rv.target() == 0, "the residual grew after the change: REVERTED at once to 0");
        ok(b.why.find("reverted") != std::string::npos, "…logged: " + b.why);
        a = feed(rv, 3.0, 0, 300, 70, 2, 194.064e6, "DAB 7D", 14, 11);
        ok(a.kind == Action::Apply, "learns again");
        rv.settleUntil(440 + 40);
        b = feed(rv, 3.0, 3.0, 482, 10, 2, 194.064e6, "DAB 7D", 6, 11);   // residual 0 but the MER fell 8 dB
        ok(b.kind == Action::Suspend && rv.suspended() && rv.target() == 0, "worse a second time (the MER fell): SUSPENDED, back to the manual setting");
        ok(rv.describe(U).find("suspended") != std::string::npos, "…and it says so: " + rv.describe(U));
        a = feed(rv, 3.0, 0, 600, 80, 2, 194.064e6, "DAB 7D", 14, 11);
        ok(a.kind == Action::None && rv.target() == 0, "suspended: no more learning");
        ok(rv.saved(0).suspended, "…which is saved (a restart does not undo it)");
        rv.reset();
        ok(!rv.suspended() && rv.target() == 0 && !rv.acquired() && rv.reverts() == 0, "Reset: unsuspended, nothing learned, back to the manual ppm exactly");

        // ── a good step is confirmed ──
        Controller good(100);
        feed(good, 3.0, 0, 0, 70, 2, 194.064e6, "DAB 7D", 12, 11);
        good.settleUntil(180);
        b = feed(good, 3.0, 3.0, 182, 10, 2, 194.064e6, "DAB 7D", 13, 11);
        ok(b.kind == Action::None && !good.verifying() && std::fabs(good.target() - 3.0) < 1e-9, "a step that helped is confirmed and kept");

        // ── Turn off ──
        good.setOn(false);
        ok(good.target() == 0 && good.describe(U) == "Automatic frequency correction is off.", "Turn off: the correction is 0 — the manual ppm exactly");
        good.setOn(true);
        ok(good.target() == 0, "on again: nothing in force until it is confident again");

        // ── a saved value learned on another manual ppm is not restored ──
        Saved sv; sv.have = true; sv.ppm = 3.0; sv.hw = 0; sv.at = U; sv.source = "DAB 7D";
        Controller r1; r1.restore(sv, 0);
        ok(std::fabs(r1.target() - 3.0) < 1e-9, "restored at start: +3.0 on the same manual ppm");
        Controller r2; r2.restore(sv, 5);
        ok(r2.target() == 0, "…but not once the owner has changed the manual ppm (5): the radio was re-trimmed");
        ok(r1.describe(U + 120) == "Auto-corrected +3.0 ppm from DAB 7D, 2 min ago", r1.describe(U + 120));
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
