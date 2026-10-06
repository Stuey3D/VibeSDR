// ★★★ THE AGC-INITIALISING INDICATOR CANNOT WEDGE (vibe_rsp_init_gate.h, 2026-10-06).
// Saber's RSP1: RF AGC off, IF AGC on, gain stage dead — "AGC initialising" in the VTS for good,
// over a perfect RDS decode. These drive the gate through the shim's own tick shape with a fake
// radio, including one that IGNORES every gain write (our measured floor and peak never move).
//   build: c++ -std=c++17 -I ../android/app/src/main/cpp -o /tmp/tgate test-rsp-init-gate.cpp
#include "vibe_rsp_init_gate.h"
#include <cstdio>
#include <cmath>

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}

using namespace vibersp;

/** A fake RSP as the gate sees it: our band-edge floor and ADC peak, given the gain the kick set.
 *  `responds` = the gain reaches the samples. `rfLoop` = automatic RF gain on (placement follows). */
struct FakeRsp {
    bool responds = true;
    bool rfLoop   = false;
    bool kickStuck = false;     // the kick never reaches step 6 (AGC off mid-kick, failure loop)
    bool agcPulls  = true;      // false = the IF AGC is the known-stuck kind and moves nothing
    double gainDb = 20.0;       // whatever the front end is doing
    double floor() const { return responds ? -95.0 + gainDb : -80.0; }
    double peak()  const { return responds ? -50.0 + gainDb : -35.0; }
};

struct Result { InitEnd end = InitEnd::None; double at = -1; bool noResp = false; };

/** The shim's sequence at ~20 ticks/s: kick steps 1 s apart, handover, the IF AGC pulling gain in,
 *  the RF loop's placement (grace 12 s + 4 s + 6 s) when it is on. Runs for `secs`. */
static Result run(InitGate& g, FakeRsp r, double t0, double secs = 120.0) {
    Result res;
    const double dt = 0.05;
    int kick = 0; double lastStep = t0 - 1.0, handAt = -1;
    for (double t = t0; t < t0 + secs; t += dt) {
        if (kick < 6 && !(r.kickStuck && kick >= 2) && t - lastStep >= 1.0) {
            ++kick; lastStep = t;
            switch (kick) {
                case 1: g.begin(t, r.floor(), r.peak()); r.gainDb = 5.0;  break;  // LNA to position 1
                case 3: r.gainDb -= 0.0; break;                                     // ifgr 59 (already)
                case 4: r.gainDb += 4.0; break;                                     // 59 -> 55
                case 5: r.gainDb -= 4.0; break;                                     // 55 -> 59
                case 6: g.handover(t); handAt = t; break;
                default: break;
            }
        }
        if (r.agcPulls && handAt > 0 && t - handAt < 5.0) r.gainDb += 2.0 * dt;   // the IF AGC pulls ~10 dB in
        const bool settling = kick < 6;
        const bool complete = r.rfLoop && handAt > 0 && t - handAt >= 12.0 + 4.0 + 6.0;
        g.sample(r.floor(), r.peak());
        const InitEnd e = g.tick(t, settling, r.rfLoop, complete);
        if (e != InitEnd::None && res.end == InitEnd::None) { res.end = e; res.at = t - t0; }
    }
    res.noResp = g.gainNoResponse;
    return res;
}

int main() {
    printf("\nWorking radio, RF loop ON — the original ending still wins\n");
    { InitGate g; FakeRsp r; r.rfLoop = true;
      Result x = run(g, r, 1000.0);
      ok(x.end == InitEnd::Complete, "ends as Complete (coarse placement + 6 s)");
      ok(x.at > 25.0 && x.at < kInitMaxSec, "after the placement, inside the 60 s ceiling");
      ok(!x.noResp, "a moving floor is never called a dead gain stage"); }

    printf("\nWorking radio, RF loop OFF (the default) — was wedged for ever\n");
    { InitGate g; FakeRsp r;
      Result x = run(g, r, 2000.0);
      ok(x.end == InitEnd::NoPlacement, "ends as NoPlacement — nothing further was coming");
      ok(x.at < 20.0, "within seconds of the kick, not 60 s and certainly not never");
      ok(!g.showing, "indicator is down (VTS free for RDS / bookmarks)");
      ok(!x.noResp, "no false diagnostic"); }

    printf("\nWorst working case: LNA already at position 1, IF AGC stuck — only the 4 dB IF steps\n");
    { InitGate g; FakeRsp r; r.gainDb = 5.0; r.agcPulls = false;
      Result x = run(g, r, 2500.0);
      ok(!x.noResp, "the kick's own 59->55->59 is enough: a working radio is never called dead");
      ok(x.end == InitEnd::NoPlacement, "and still ends promptly"); }

    printf("\nSaber's RSP1: gain writes change NOTHING we measure, RF loop off\n");
    { InitGate g; FakeRsp r; r.responds = false;
      Result x = run(g, r, 3000.0);
      ok(x.end == InitEnd::GainNoResponse, "ends as GainNoResponse");
      ok(x.at < 15.0, "about six seconds after the handover");
      ok(x.noResp, "owner diagnostic raised");
      ok(g.lastFloorSwing < kMinFloorSwingDb && g.lastPeakSwing < kMinPeakSwingDb, "both swings flat"); }

    printf("\nDead gain AND the RF loop on — the placement never confirms anything\n");
    { InitGate g; FakeRsp r; r.responds = false; r.rfLoop = true;
      Result x = run(g, r, 4000.0);
      ok(x.end == InitEnd::GainNoResponse, "does not wait for the placement");
      ok(x.at < kInitMaxSec, "inside the ceiling"); }

    printf("\nThe kick never finishes (AGC off mid-kick / API failure loop)\n");
    { InitGate g; FakeRsp r; r.kickStuck = true;
      Result x = run(g, r, 5000.0);
      ok(x.end == InitEnd::TimedOut, "ends as TimedOut");
      ok(std::fabs(x.at - kInitMaxSec) < 0.2, "at the 60 s ceiling, not never"); }

    printf("\nA stream that keeps restarting after a give-up does not re-raise it at once\n");
    { InitGate g; FakeRsp r; r.responds = false;
      run(g, r, 6000.0, 30.0);
      const bool shown = g.begin(6040.0, -80.0, -35.0);
      ok(!shown, "40 s later: the re-kick runs silently");
      ok(!g.begin(6000.0 + kRetryQuietSec - 1.0, -80.0, -35.0), "still silent inside the quiet spell");
      ok(g.begin(6000.0 + kRetryQuietSec + 20.0, -80.0, -35.0), "after it: may show again — never permanent"); }

    printf("\nThe diagnostic clears itself when a later cycle does respond\n");
    { InitGate g; FakeRsp dead; dead.responds = false;
      run(g, dead, 7000.0, 30.0);
      ok(g.gainNoResponse, "raised by the dead cycle");
      FakeRsp alive;
      run(g, alive, 7000.0 + kRetryQuietSec + 10.0, 40.0);
      ok(!g.gainNoResponse, "cleared by a cycle in which the floor moved"); }

    printf("\nManual gain: no kick, no begin() — nothing ever shows\n");
    { InitGate g;
      for (double t = 0; t < 60; t += 0.05) g.tick(t, false, false, false);
      ok(!g.showing, "never raised"); }

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
