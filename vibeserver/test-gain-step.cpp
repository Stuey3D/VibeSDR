// test-gain-step.cpp — the front end's own gain step must be inaudible, and stereo must survive it.
//
// ★★★ WHAT THIS IS FOR (Stuart, 2026-10-19, the Lenovo's RSP1A): "RF gain movement always caused a pop
//     to the signals even in MW and that pop is what kills stereo." Every LNA rung the RF AGC takes is
//     an abrupt level step in the IQ — up to ~21 dB on FM — on SDRplay's API and on OpenRSP alike, so
//     the fault is ours. MEASURED on the real RxPipeline before the fix (2026-10-19):
//       · AM −15 dB: a thump 7.6 dB OVER the programme, then a 1.3 s hole 14 dB down (the audio AGC
//         releasing); AM +15 dB: an 18 dB thump.
//       · WFM clean −15 dB: the multipath meter read 74 % — CEQ engaged on a perfect station and IMS
//         took the L−R corner 4.2 kHz down (8.5 kHz with CEQ off), back after 2.4-4 s.
//       · WFM marginal (~18 dB MPX S/N): CEQ held for 6 s; an UP-step clicked at 0 dB re programme
//         (the impulse blanker holding samples across the edge).
//       · The pilot PLL never unlocked. The meters, not the lock, are what "killed stereo".
//
// ★★ THE FIX UNDER TEST is the pair the shim runs: GainStepSmoother on the raw capture (level put back at
//    the step, then glided away) and RxPipeline::noteGainStep() while the correction is still gliding.
//    Each case is run TWICE with the same noise seed — with and without the step — and the difference
//    is the artefact, so nothing here depends on guessing what the audio "should" be.
//
// ★★ AND EVERY FIX ASSERTION IS PAIRED WITH THE FAULT ON THE SAME SIGNAL WITHOUT IT. A test that passes
//    with the fix but would also pass without it proves nothing (the project has been bitten by that:
//    see recurring_fault_shapes, "negative after positive").
//
// ★★★ 11.0.9 — case 9: the rung the hardware IF AGC cancels within milliseconds (a transient, not a step), seen
//     on the real RSP after 11.0.8. Before (no fix, and 11.0.8's handling of a flag), a click, a pilot dent and
//     CEQ engaging on a marginal FM station, a thump on AM; after (the flagged-transient hold), none of them.
//
// ★ Silent: no audio device is opened; the audio callback only stores samples.

#include "../android/app/src/main/cpp/vibedsp/vibedsp.h"
#include "../android/app/src/main/cpp/vibedsp/gainstep.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using vibedsp::RxPipeline;
using vibedsp::cf32;
using vibedsp::GainStepSmoother;

static int failures = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    if (cond) { std::printf("   ok   %s\n", what); return; }
    ++failures;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}
static std::string f1(double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return b; }

namespace {

constexpr double kFs = 1000000.0;
constexpr double kStepT = 3.0;              // the (first) step, seconds into the run
static int kBlk = 8192;                     // ★ steps land MID-block: the hint must not need alignment

struct Step { double t, db, phiDeg; };

/** Stereo WFM (1 kHz left only, pilot 10 %) or AM (1 kHz at 50 %) on IQ, with noise added BEFORE the
 *  front end's gain — antenna noise, which a gain step scales with the signal — and then the steps. */
struct Gen {
    bool am = false; double noise = 0.0;
    std::vector<Step> steps;
    double phase = 0.0, t = 0.0; long long n = 0;
    std::mt19937 rng{777}; std::normal_distribution<double> N{0.0, 1.0};
    void fill(std::vector<cf32>& o, int cnt) {
        o.resize((size_t)cnt);
        for (int i = 0; i < cnt; ++i, ++n) {
            double re, im;
            if (am) {
                re = 0.3 * (1.0 + 0.5 * std::sin(2.0 * M_PI * 1000.0 * t)); im = 0.0;
            } else {
                const double L = std::sin(2.0 * M_PI * 1000.0 * t);
                const double mpx = 0.45 * L + 0.10 * std::sin(2.0 * M_PI * 19000.0 * t)
                                 + 0.45 * L * std::sin(2.0 * M_PI * 38000.0 * t);
                phase += 2.0 * M_PI * 75000.0 * mpx / kFs;
                if (phase > 1e6) phase -= 2.0 * M_PI * std::floor(phase / (2.0 * M_PI));
                re = 0.3 * std::cos(phase); im = 0.3 * std::sin(phase);
            }
            if (noise > 0.0) { re += noise * 0.3 * N(rng); im += noise * 0.3 * N(rng); }
            double g = 0.0, ph = 0.0;
            for (const Step& s : steps) if (t >= s.t) { g += s.db; ph += s.phiDeg; }
            if (g != 0.0 || ph != 0.0) {
                const double a = std::pow(10.0, g / 20.0), c = a * std::cos(ph * M_PI / 180.0),
                             sn = a * std::sin(ph * M_PI / 180.0);
                const double r2 = re * c - im * sn, i2 = re * sn + im * c; re = r2; im = i2;
            }
            o[(size_t)i] = cf32((float)re, (float)im);
            t += 1.0 / kFs;
        }
    }
};

struct Sink {
    std::vector<float> L, R;
    static void audio(void* c, const float* p, int fr, int ch, int) {
        auto* s = (Sink*)c;
        for (int i = 0; i < fr; ++i) { s->L.push_back(p[i * ch]); s->R.push_back(ch > 1 ? p[i * ch + 1] : p[i * ch]); }
    }
};

/** Per-block state after the step. */
struct Run {
    Sink sink;
    std::vector<double> t; std::vector<int> lock, ceq; std::vector<float> blend, cut, mp, pilot;
    unsigned smSteps = 0, smHolds = 0; float smLastDb = 0.0f;
};

/** ★ Exact108 = flagged as 11.0.8 handled a flag: no hold for a rung that does not stay, and the estimators guarded
 *  only while a found step's correction glides — the "before" for the short transients of case 9. */
enum class Fix { None, Exact, Soon, Both, Exact108 };

/** ★ Fed exactly as the shim feeds it (smoothGainSteps): smoother on the raw capture, then noteGainStep() on every
 *  block that was FLAGGED, found a step, is still holding a transient, or whose correction is still more than 1 dB
 *  from unity. (11.0.8 guarded only the last two — Fix::Exact108.) */
void runOne(Run& r, Gen g, double secs, Fix fix, bool ceqOn = true) {
    RxPipeline::Callbacks cb{}; cb.ctx = &r.sink; cb.audio = &Sink::audio;
    RxPipeline rx; rx.start(kFs, 1024, 10.0, 48000, cb);
    if (!ceqOn) rx.setCeq(false);
    rx.setTune(0.0, g.am ? RxPipeline::Mode::AM : RxPipeline::Mode::WFM, g.am ? 10000.0 : 250000.0);
    GainStepSmoother sm; sm.configure(kFs);
    if (fix == Fix::Exact108) sm.setTransientHold(false);
    std::vector<cf32> b;
    const long long total = (long long)(kFs * secs);
    for (long long d = 0; d < total; d += kBlk) {
        g.fill(b, kBlk);
        bool flagged = false;
        if (fix != Fix::None) {
            for (const Step& s : g.steps) {
                const long long at = (long long)(s.t * kFs);
                // exact: the source flags the packet the change landed in (SDRplay grChanged);
                // soon:  we wrote the gain ~16 ms before it reached the samples (an RTL's USB queue).
                // both: an RSP — our setLnaState write first, then the packet the API flags.
                const long long early = (long long)(0.016 * kFs);
                const bool exact = fix == Fix::Exact || fix == Fix::Both || fix == Fix::Exact108;
                if (exact && at >= d && at < d + kBlk) { sm.noteStepAtStart(); flagged = true; }
                if ((fix == Fix::Soon || fix == Fix::Both) && at - early >= d && at - early < d + kBlk) sm.noteStepSoon(0.3);
            }
            const unsigned before = sm.steps();
            sm.process(b.data(), kBlk);
            if (fix == Fix::Exact108) { if (sm.steps() != before || sm.correcting(1.0)) rx.noteGainStep(0.1); }
            else if (flagged || sm.steps() != before || sm.correcting(1.0) || sm.holding()) rx.noteGainStep(0.1);
        }
        rx.feed(b.data(), kBlk);
        r.t.push_back((double)(d + kBlk) / kFs);
        r.lock.push_back(rx.pilotLocked()); r.ceq.push_back(rx.ceqEngaged());
        r.blend.push_back(rx.stereoBlend()); r.cut.push_back(rx.lmrHiCutHz()); r.mp.push_back(rx.multipathDepth());
        r.pilot.push_back(rx.pilotLockAmp());
    }
    r.smSteps = sm.steps(); r.smLastDb = sm.lastStepDb(); r.smHolds = sm.holds();
}

/** The step run against its no-step twin. */
struct Verdict {
    double clickDb = -200;            // largest |difference| near the step, dB re the programme peak
    double unlockedS = 0, extraCeqS = 0, maxCutDrop = 0, backAfterS = 0, maxMp = 0;
    double pilotDentPct = 0;          // the deepest dent in the pilot's lock amplitude against the twin, %
    double amPeakDb = 0, amMinDb = 0;  // AM: loudest 20 ms just after vs programme; quietest 100 ms-2 s
};

Verdict judge(const Run& a, const Run& b, bool am) {
    Verdict v;
    const size_t n = std::min(a.sink.L.size(), b.sink.L.size());
    const size_t s0 = (size_t)(kStepT * 48000.0);
    float ref = 0; for (size_t i = s0 - 48000; i < s0; ++i) ref = std::max(ref, std::fabs(a.sink.L[i]));
    if (!am) {
        float d = 0;
        // ★ −50…+120 ms: a step's click lands within a few ms of it (chain latency); the last step of
        //   case 4 is at +60 ms. Later than that the twins legitimately part company — the guard holds
        //   the blend S/N for 50 ms, so high-blend's own glides run a few blocks apart — and that is
        //   judged by the corner and CEQ figures, not mistaken for a click.
        for (size_t i = s0 - 2400; i < std::min(n, s0 + 5760); ++i)
            d = std::max(d, std::max(std::fabs(b.sink.L[i] - a.sink.L[i]), std::fabs(b.sink.R[i] - a.sink.R[i])));
        v.clickDb = 20.0 * std::log10(d / ref + 1e-12);
        const double dt = (double)kBlk / kFs;
        for (size_t k = 0; k < b.t.size(); ++k) {
            if (b.t[k] < kStepT) continue;
            if (!b.lock[k]) v.unlockedS += dt;
            if (b.ceq[k] && !a.ceq[k]) v.extraCeqS += dt;
            v.maxCutDrop = std::max(v.maxCutDrop, (double)(a.cut[k] - b.cut[k]));
            v.maxMp = std::max(v.maxMp, (double)b.mp[k]);
            if (a.pilot[k] > 0.0f) v.pilotDentPct = std::max(v.pilotDentPct, 100.0 * (a.pilot[k] - b.pilot[k]) / a.pilot[k]);
            const bool off = !b.lock[k] || b.blend[k] < 0.95f || std::fabs(b.cut[k] - a.cut[k]) > 600.0f
                          || b.ceq[k] != a.ceq[k];
            if (off) v.backAfterS = b.t[k] - kStepT;
        }
    } else {
        // ★ AM levels differ for a while by design (the glide), so the CLICK is judged as level: the
        //   loudest 20 ms after the step, and the deepest dip once the AGC has had to follow.
        auto peakIn = [&](size_t i0, size_t len) { float p = 0; for (size_t i = i0; i < std::min(n, i0 + len); ++i) p = std::max(p, std::fabs(b.sink.L[i])); return p; };
        v.amPeakDb = 20.0 * std::log10(peakIn(s0 - 480, 1920) / ref);
        float mn = 1e9f;
        for (size_t i0 = s0 + 4800; i0 + 960 < std::min(n, s0 + 96000); i0 += 960) mn = std::min(mn, peakIn(i0, 960));
        v.amMinDb = 20.0 * std::log10(mn / ref);
    }
    return v;
}

void print(const char* tag, const Verdict& v, bool am) {
    if (am) std::printf("   .. %-34s thump %+5.1f dB   deepest dip %+5.1f dB (re programme)\n", tag, v.amPeakDb, v.amMinDb);
    else std::printf("   .. %-34s click %6.1f dB   multipath %4.0f %%   CEQ +%.1f s   L-R -%4.0f Hz   unlocked %.1f s   back %.1f s   pilot -%.1f %%\n",
                     tag, v.clickDb, v.maxMp * 100.0, v.extraCeqS, v.maxCutDrop, v.unlockedS, v.backAfterS, v.pilotDentPct);
}

Verdict pair(Gen g, Fix fix, double secs, bool ceqOn = true, Run* bOut = nullptr) {
    Gen a = g; a.steps.clear();
    Run ra, rb;
    runOne(ra, a, secs, fix, ceqOn);
    runOne(rb, g, secs, fix, ceqOn);
    if (bOut) { bOut->smSteps = rb.smSteps; bOut->smLastDb = rb.smLastDb; bOut->smHolds = rb.smHolds; }
    return judge(ra, rb, g.am);
}

}  // namespace

int main() {
    std::printf("\nGain step — our own LNA rung must be inaudible, and stereo must ride it\n");
    const double secs = 8.0;

    // ── 1. WFM, a strong clean station, −15 dB (an RSP rung on FM) ──────────────────────────────────
    {
        Gen g; g.steps = {{kStepT, -15.0, 0.0}};
        const Verdict before = pair(g, Fix::None, secs);
        Run info; const Verdict after = pair(g, Fix::Exact, secs, true, &info);
        print("clean −15 dB, as it was:", before, false);
        print("clean −15 dB, smoothed + guarded:", after, false);
        ok(before.maxMp > 0.30 && before.extraCeqS > 0.5,
           "(the fault is there without the fix: the step reads as multipath and engages CEQ)",
           "multipath " + f1(before.maxMp * 100) + " %, CEQ " + f1(before.extraCeqS) + " s");
        ok(std::fabs(info.smLastDb + 15.0f) < 0.5f, "★ the step is FOUND and MEASURED (−15 dB)",
           "found " + std::to_string(info.smSteps) + ", measured " + f1(info.smLastDb) + " dB");
        ok(after.clickDb < -40.0, "★★ no click (< −40 dB re programme)", "click " + f1(after.clickDb) + " dB");
        ok(after.extraCeqS == 0.0, "★★ CEQ is not engaged by our own step", f1(after.extraCeqS) + " s");
        ok(after.maxCutDrop < 600.0, "★★ the L−R corner does not move (stereo image held)", f1(after.maxCutDrop) + " Hz");
        ok(after.unlockedS == 0.0 && after.backAfterS < 0.1, "★★ stereo holds throughout — nothing to recover from",
           "back after " + f1(after.backAfterS) + " s");
    }
    // ── 1b. …in RSP-sized packets (1008 samples, not a multiple of anything), soon AND exact ────────
    {
        // ★ An RSP hands over ~0.5 ms at a time, and the flag marks the packet the change landed in. Our
        //   own write reaches the smoother first (noteStepSoon from setLnaState), the flag after — the
        //   flag must win, and a packet boundary must not hide the step.
        kBlk = 1008;
        Gen g; g.steps = {{kStepT, -15.0, 0.0}};
        Run info; const Verdict after = pair(g, Fix::Both, 6.0, true, &info);
        const Verdict soon = pair(g, Fix::Soon, 6.0);
        kBlk = 8192;
        print("clean −15 dB, 1008-sample packets:", after, false);
        ok(info.smSteps == 1 && after.clickDb < -40.0 && soon.clickDb < -40.0 && after.extraCeqS == 0.0,
           "★★ small packets: found once, no click, flagged or searched",
           "found " + std::to_string(info.smSteps) + ", click " + f1(after.clickDb) + " / " + f1(soon.clickDb) + " dB");
    }
    // ── 2. …with CEQ off, where IMS was the one that narrowed ────────────────────────────────────────
    {
        Gen g; g.steps = {{kStepT, -15.0, 0.0}};
        const Verdict before = pair(g, Fix::None, secs, false);
        const Verdict after = pair(g, Fix::Exact, secs, false);
        print("clean −15 dB, CEQ off, as it was:", before, false);
        print("clean −15 dB, CEQ off, fixed:", after, false);
        ok(before.maxCutDrop > 3000.0, "(without the fix IMS throws the L−R corner kHz down)", f1(before.maxCutDrop) + " Hz");
        ok(after.maxCutDrop < 600.0 && after.clickDb < -40.0, "★★ with it, the corner and the audio are untouched",
           f1(after.maxCutDrop) + " Hz, click " + f1(after.clickDb) + " dB");
    }
    // ── 3. WFM, a MARGINAL station (~18 dB MPX S/N), stepping UP — where Stuart sees it stick ─────────
    {
        Gen g; g.noise = 0.65; g.steps = {{kStepT, +15.0, 0.0}};
        const Verdict before = pair(g, Fix::None, secs);
        const Verdict after = pair(g, Fix::Exact, secs);
        print("marginal +15 dB, as it was:", before, false);
        print("marginal +15 dB, fixed:", after, false);
        ok(before.clickDb > -10.0, "(without the fix an up-step is a loud click on a marginal station)", f1(before.clickDb) + " dB");
        ok(after.clickDb < -40.0, "★★ no click on the marginal station either", f1(after.clickDb) + " dB");
        ok(after.extraCeqS == 0.0 && after.maxCutDrop < 600.0 && after.unlockedS == 0.0,
           "★★ and stereo, blend and CEQ are exactly where they would have been",
           "CEQ " + f1(after.extraCeqS) + " s, corner " + f1(after.maxCutDrop) + " Hz");
    }
    // ── 4. A rung and the IF AGC answering it: −20 dB, then +3, +3 dB 30 ms apart, each flagged ───────
    {
        Gen g; g.noise = 0.5; g.steps = {{kStepT, -20.0, 0.0}, {kStepT + 0.03, +3.0, 0.0}, {kStepT + 0.06, +3.0, 0.0}};
        const Verdict before = pair(g, Fix::None, secs);
        const Verdict after = pair(g, Fix::Exact, secs);
        print("rung + IF AGC, as it was:", before, false);
        print("rung + IF AGC, fixed:", after, false);
        ok(after.clickDb < -40.0 && after.extraCeqS == 0.0 && after.maxCutDrop < 600.0,
           "★★ a SEQUENCE of steps (LNA then the IF AGC) is as quiet as one",
           "click " + f1(after.clickDb) + " dB, CEQ " + f1(after.extraCeqS) + " s, corner " + f1(after.maxCutDrop) + " Hz");
    }
    // ── 5. A driver that only knows WE wrote a gain (an RTL): searched for, latency unknown ──────────
    {
        Gen g; g.steps = {{kStepT, -12.0, 0.0}};
        Run info; const Verdict after = pair(g, Fix::Soon, secs, true, &info);
        print("clean −12 dB, our write ~16 ms early:", after, false);
        ok(info.smSteps == 1 && std::fabs(info.smLastDb + 12.0f) < 0.5f, "★ found by looking, from a hint 16 ms early",
           "found " + std::to_string(info.smSteps) + ", " + f1(info.smLastDb) + " dB");
        ok(after.clickDb < -40.0 && after.extraCeqS == 0.0 && after.maxCutDrop < 600.0, "★★ and as quiet as a flagged one",
           "click " + f1(after.clickDb) + " dB");
    }
    // ── 6. AM / MW — "even in MW" ──────────────────────────────────────────────────────────────────
    for (double db : {-15.0, +15.0, -6.0}) {
        Gen g; g.am = true; g.steps = {{kStepT, db, 0.0}};
        const Verdict before = pair(g, Fix::None, secs);
        const Verdict after = pair(g, Fix::Exact, secs);
        char lbl[16]; std::snprintf(lbl, sizeof lbl, "%+.0f", db);
        const std::string tb = std::string("AM ") + lbl + " dB, as it was:";
        const std::string ta = std::string("AM ") + lbl + " dB, fixed:";
        print(tb.c_str(), before, true); print(ta.c_str(), after, true);
        ok(before.amPeakDb > 3.0, "(without the fix the step is a thump over the programme)", f1(before.amPeakDb) + " dB");
        ok(after.amPeakDb < 1.5, "★★ no thump — the loudest moment is the programme itself", f1(after.amPeakDb) + " dB");
        ok(after.amMinDb > -6.0, "★★ and no hole while the AGC follows (never 6 dB down)", f1(after.amMinDb) + " dB");
    }
    // ── 7. A station that never steps is left exactly alone ─────────────────────────────────────────
    {
        // ★★ Bit-identical, not "close": with no step the smoother must be a pure delay, even with a hint
        //    open on a modulated AM carrier and on a noisy FM one — programme must never pass for a rung.
        // ★ kind 2 is DAB-shaped: wideband noise with a 1.3 ms NULL every 96 ms — sharp, deep, and back
        //   again, the one thing on air that looks most like a rung over 1.5 ms (see finalise()).
        for (int kind = 0; kind < 3; ++kind) {
            Gen g; g.am = (kind == 0); g.noise = g.am ? 0.0 : 0.65;
            GainStepSmoother sm; sm.configure(kFs);
            // ★ DAB runs with the flagged-transient hold OFF, as the shim does (a null symbol is a dip that comes
            //   back, and holding the level across one would fill it). Programme and noise run with it ON.
            if (kind == 2) sm.setTransientHold(false);
            std::vector<cf32> in, all, out;
            std::mt19937 rng(42); std::normal_distribution<float> N(0.0f, 0.3f);
            long long n = 0;
            for (int k = 0; k < 400; ++k) {
                g.fill(in, kBlk);
                if (kind == 2) for (auto& z : in) { const bool null = (n % 96000) < 1297; ++n;
                                                    z = null ? cf32(0.01f * N(rng), 0.01f * N(rng)) : cf32(N(rng), N(rng)); }
                all.insert(all.end(), in.begin(), in.end());
                if (k % 20 == 5) sm.noteStepAtStart();
                if (k % 50 == 7) sm.noteStepSoon(0.3);
                sm.process(in.data(), kBlk); out.insert(out.end(), in.begin(), in.end());
            }
            if (kind == 2) std::printf("   .. DAB nulls: %u looked like a rung over 1.5 ms and were thrown out at 4 ms\n", sm.rejected());
            const int D = sm.delaySamples();
            bool same = sm.steps() == 0;
            for (size_t i = (size_t)D; i < out.size() && same; ++i) same = (out[i] == all[i - (size_t)D]);
            ok(same, kind == 0 ? "★★★ no step, hints open, AM programme: output is the input, bit for bit (delayed 4 ms)"
                   : kind == 1 ? "★★★ no step, hints open, noisy FM: output is the input, bit for bit"
                               : "★★★ no step, hints open, DAB nulls every 96 ms: output is the input, bit for bit",
               "false steps " + std::to_string(sm.steps()));
        }
    }
    // ── 7b. …while a real rung on a DAB ensemble is still found, null symbols and all ───────────────
    // ★ Flagged, and searched for from a write 0.2 s early — so at least two nulls are met (and thrown
    //   out) before the rung, and the search must carry on past them.
    for (int soon = 0; soon < 2; ++soon) {
        GainStepSmoother sm; sm.configure(kFs); sm.setTransientHold(false);   // DAB: as the shim runs it
        std::mt19937 rng(43); std::normal_distribution<float> N(0.0f, 0.3f);
        std::vector<cf32> in((size_t)kBlk);
        long long n = 0; const long long at = (long long)(kStepT * kFs) + 5000;   // not on a null
        for (int k = 0; k < (int)(4.0 * kFs / kBlk); ++k) {
            for (auto& z : in) { const bool null = (n % 96000) < 1297; const float gs = n >= at ? 0.1778f : 1.0f; ++n;
                                 z = null ? cf32(0.01f * N(rng), 0.01f * N(rng)) * gs : cf32(N(rng), N(rng)) * gs; }
            if (!soon && at >= n - kBlk && at < n) sm.noteStepAtStart();
            const long long w = at - (long long)(0.2 * kFs);
            if (soon && w >= n - kBlk && w < n) sm.noteStepSoon(0.3);
            sm.process(in.data(), kBlk);
        }
        ok(sm.steps() == 1 && std::fabs(sm.lastStepDb() + 15.0f) < 1.0f && std::llabs(sm.lastStepAt() - at) <= 8,
           soon ? "★★ …and searched for past the nulls from a write 0.2 s early"
                : "★★ a −15 dB rung on a DAB ensemble is found, measured and placed to the sample",
           "found " + std::to_string(sm.steps()) + ", " + f1(sm.lastStepDb()) + " dB at " + std::to_string(sm.lastStepAt() - at));
    }
    // ── 9. THE RUNG THE IF AGC CANCELS WITHIN MILLISECONDS (11.0.9) ──────────────────────────────────────
    {
        // ★★★ The Lenovo's RSP1A, 2026-10-20: the steps that stay were caught, but some rungs (7→8, 6→7, IF AGC off
        //     its rail) come straight back — a transient, not a step. The "a step stays" test refuses it, so 11.0.8
        //     let it through: still a pop on AM, a click and CEQ engaging on a marginal FM station. Now a FLAGGED
        //     packet holds the level from the first sharp edge, and the estimators sit it out, whether or not the
        //     step stays. Each case: as it was (no fix), as 11.0.8 had it (Exact108), and now.
        struct T { const char* name; bool am; double noise; std::vector<Step> st; };
        const std::vector<T> ts = {
            {"marginal FM −15, back in 2 ms",   false, 0.65, {{kStepT, -15, 0}, {kStepT + 0.002, +15, 0}}},
            {"marginal FM +12, back in 3 ms",   false, 0.65, {{kStepT, +12, 0}, {kStepT + 0.003, -12, 0}}},
            {"clean FM −15, back in 3 × 5 dB",  false, 0.0,  {{kStepT, -15, 0}, {kStepT + 0.001, +5, 0},
                                                              {kStepT + 0.002, +5, 0}, {kStepT + 0.003, +5, 0}}},
            {"AM −15, back in 2 ms",            true,  0.0,  {{kStepT, -15, 0}, {kStepT + 0.002, +15, 0}}},
            {"AM +12, back in 3 ms",            true,  0.0,  {{kStepT, +12, 0}, {kStepT + 0.003, -12, 0}}},
        };
        for (const T& c : ts) {
            Gen g; g.am = c.am; g.noise = c.noise; g.steps = c.st;
            const Verdict none = pair(g, Fix::None, 6.0);
            const Verdict was = pair(g, Fix::Exact108, 6.0);
            Run info; const Verdict now = pair(g, Fix::Exact, 6.0, true, &info);
            const std::string n = c.name;
            print((n + ", as it was:").c_str(), none, c.am);
            print((n + ", 11.0.8:").c_str(), was, c.am);
            print((n + ", held:").c_str(), now, c.am);
            if (c.am) {
                const double worst = std::max(none.amPeakDb, was.amPeakDb);
                ok(worst > 3.0 || was.amMinDb < -6.0, ("(" + n + ": before, a thump or a hole)").c_str(),
                   "thump " + f1(none.amPeakDb) + " / " + f1(was.amPeakDb) + " dB, dip " + f1(was.amMinDb) + " dB");
                ok(now.amPeakDb < 1.5 && now.amMinDb > -6.0, ("★★★ " + n + ": no thump, no hole").c_str(),
                   "thump " + f1(now.amPeakDb) + " dB, dip " + f1(now.amMinDb) + " dB, holds " + std::to_string(info.smHolds));
            } else {
                const bool clickBefore = std::max(none.clickDb, was.clickDb) > -50.0;
                const bool upsetBefore = std::max(none.extraCeqS, was.extraCeqS) > 0.5 || std::max(none.backAfterS, was.backAfterS) > 0.5
                                      || std::max(none.maxCutDrop, was.maxCutDrop) > 400.0
                                      || std::max(none.pilotDentPct, was.pilotDentPct) > 2.0;
                if (c.noise > 0.0)
                    ok(clickBefore || upsetBefore, ("(" + n + ": before, a click or a stereo upset)").c_str(),
                       "click " + f1(none.clickDb) + " / " + f1(was.clickDb) + " dB, CEQ " + f1(none.extraCeqS) + " / " + f1(was.extraCeqS)
                       + " s, pilot -" + f1(none.pilotDentPct) + " / -" + f1(was.pilotDentPct) + " %");
                ok(now.clickDb < -50.0, ("★★★ " + n + ": no click (< −50 dB)").c_str(), "click " + f1(now.clickDb) + " dB");
                ok(now.extraCeqS == 0.0 && now.maxCutDrop < 400.0 && now.unlockedS == 0.0 && now.backAfterS < 0.1
                   && now.pilotDentPct < 2.0,
                   ("★★★ " + n + ": stereo, CEQ and the pilot ride it").c_str(),
                   "CEQ " + f1(now.extraCeqS) + " s, corner " + f1(now.maxCutDrop) + " Hz, back " + f1(now.backAfterS) + " s, pilot -" + f1(now.pilotDentPct) + " %");
            }
        }
    }
    // ── 8. And the pilot loop cannot be wedged: from the worst state it can reach ───────────────────
    {
        // ★★★ RECORDED BECAUSE IT RULES SOMETHING OUT. "Stereo does not come back until I retune" fits a
        //     PLL stuck outside its pull-in range — a retune re-seeds it. Measured: kicked to its ±200 Hz
        //     integrator clamp AND 180° out, at 0.8 noise, the loop pulls back inside its own 1 s release
        //     hold and stereo never drops. So a watchdog that re-seeds it would never fire, and none was
        //     added; this case keeps that claim true. The meters were the sticky part (cases 1-3).
        Sink sink; RxPipeline::Callbacks cb{}; cb.ctx = &sink; cb.audio = &Sink::audio;
        RxPipeline rx; rx.start(kFs, 1024, 10.0, 48000, cb); rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
        Gen g; g.noise = 0.8; std::vector<cf32> b;
        int unlocked = 0;
        for (int k = 0; k < (int)(8.0 * kFs / kBlk); ++k) {
            g.fill(b, kBlk); rx.feed(b.data(), kBlk);
            if (k == (int)(3.0 * kFs / kBlk)) rx.debugKickPilotLoop(200.0, M_PI);
            if (k > (int)(3.0 * kFs / kBlk) && !rx.pilotLocked()) ++unlocked;
        }
        ok(unlocked == 0, "★ the pilot loop recovers from its worst reachable state without dropping stereo",
           std::to_string(unlocked) + " unlocked blocks");
    }

    std::printf(failures ? "\nFAILED %d\n" : "\nall good\n", failures);
    return failures ? 1 : 0;
}
