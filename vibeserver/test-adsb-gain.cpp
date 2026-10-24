// test-adsb-gain.cpp — the ADS-B gain rule (vibe_adsb_service.h GainRule), after Stuart's FlightAware box (dump1090-fa
// --gain 60 --adaptive-range, ADAPTIVE_BURST=no): the top of the list, down only for dynamic range, never for strong
// aircraft; an SDRplay (levelHeld) only down, and only for replies clipping.
#include "vibe_adsb_service.h"
#include <algorithm>
#include <cstdio>
#include <utility>

static int fails = 0, passes = 0;
static void ok(bool c, const char* what) { if (c) passes++; else { fails++; std::printf("FAIL %s\n", what); } }
static vibeadsb::Health H(double noise, double strong, bool valid = true) {
    vibeadsb::Health h; h.noiseDbfs = noise; h.strongFrac = strong; h.valid = valid; h.msgsPerSec = 300; return h;
}

int main() {
    vibeadsb::GainRule r;
    ok(r.decide(H(-25, 0)) == -1, "noise -25 dBFS (25 dB of range, under 30) -> down");
    ok(r.decide(H(-30.5, 0)) == 0, "noise -30.5 (the FlightAware box's settled -30.6) -> stay");
    ok(r.decide(H(-31.7, 0.08)) == 0, "the Pi 500 V4L's -31.7 with 8 % strong -> stay (strong is no reason to cut)");
    ok(r.decide(H(-42, 0.11)) == +1, "the Pi 2's -42 at 11 % strong -> up (the shim clamps at the top)");
    ok(r.decide(H(-32, 0)) == 0, "-32: inside the 3 dB of spare -> stay, no climb");
    ok(r.decide(H(-25, 0, false)) == 0, "no evidence yet -> stay");
    ok(r.decide(H(-20, 0.60)) == -1, "a hot front end is cut for its noise floor, whatever the strong share");

    vibeadsb::GainRule s; s.levelHeld = true;
    ok(s.decide(H(-43, 0.02)) == 0, "SDRplay: quiet and clean -> stay (never climbs)");
    ok(s.decide(H(-43, 0.10)) == -1, "SDRplay: 10 % clipping -> down");
    ok(s.decide(H(-20, 0.0)) == 0, "SDRplay: its noise floor is its AGC's, not ours -> stay");

    // ★★★ GainTrial — a proposed step is kept only for more messages (the Pi 500 V4L: 250 -> 170 msg/s after a cut)
    {
        // runs a trial at a 2 s tick; rateAt(idx) = messages a second at that gain index
        auto run = [](int cur, int prop, double rHi, double rLo, int& setsSeen) {
            vibeadsb::GainTrial g; double t = 100; uint64_t total = 0; setsSeen = 0;
            int at = g.start(cur, prop, t);
            int last = at;
            for (int i = 0; i < 200 && g.active(); ++i) {
                const double r = at == std::max(cur, prop) ? rHi : rLo;
                t += 2; total += uint64_t(r * 2);
                at = g.step(t, total);
                if (at != last) { ++setsSeen; last = at; }
            }
            return std::make_pair(g.active() ? -99 : at, g);
        };
        int sets = 0;
        auto a = run(28, 27, 250, 170, sets);
        ok(a.first == 28, "a cut that loses messages (250 -> 170) is refused, the top kept");
        ok(!a.second.proposedWon(), "  ...and says so");
        ok(sets == 5, "  ...after six alternating legs (lower, top, lower, top, lower, top)");
        ok(a.second.rate(0) > 240 && a.second.rate(1) < 180, "  ...each side measured on its own legs");
        auto b = run(28, 27, 200, 230, sets);
        ok(b.first == 27 && b.second.proposedWon(), "a cut that gains messages (a front end overloading) is taken");
        auto c = run(28, 27, 200, 200, sets);
        ok(c.first == 28, "a tie on a cut keeps the higher gain");
        auto d = run(20, 21, 200, 200, sets);
        ok(d.first == 21, "a tie on a climb takes the higher gain");
        auto e = run(20, 21, 150, 200, sets);
        ok(e.first == 20, "a climb that loses messages is refused");
        vibeadsb::GainTrial f; f.start(5, 4, 0); f.cancel();
        ok(!f.active(), "a listener taking the gain voids the trial");
    }

    std::printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", passes + fails, fails);
    return fails ? 1 : 0;
}
