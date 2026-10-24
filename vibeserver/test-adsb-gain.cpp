// test-adsb-gain.cpp — the ADS-B gain rule (vibe_adsb_service.h GainRule), after Stuart's FlightAware box (dump1090-fa
// --gain 60 --adaptive-range, ADAPTIVE_BURST=no): the top of the list, down only for dynamic range, never for strong
// aircraft; an SDRplay (levelHeld) only down, and only for replies clipping.
#include "vibe_adsb_service.h"
#include <algorithm>
#include <cstdio>
#include <vector>
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
        auto run = [](int cur, int prop, double rHi, double rLo, int& setsSeen, bool breaksTie = true) {
            vibeadsb::GainTrial g; double t = 100; uint64_t total = 0; setsSeen = 0;
            int at = g.start(cur, prop, t, breaksTie);
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
        ok(c.first == 27, "a tie on the noise rule's cut takes the cut (messages first, the noise rule breaks ties)");
        auto c0 = run(28, 27, 0.5, 0.5, sets);
        ok(c0.first == 27, "the V4L + LaNA at the top: nothing decodes either side -> the cut is taken, not stuck at the top");
        auto c2 = run(28, 27, 200, 195, sets);
        ok(c2.first == 27, "2.5 % down is inside the noise of the counts -> a tie -> the cut");
        auto d = run(20, 21, 200, 200, sets);
        ok(d.first == 21, "a tie on a climb takes the climb");
        auto x = run(20, 21, 200, 205, sets, false);
        ok(x.first == 20, "an exploring look must clearly WIN: a near-tie stays where it is");
        auto x2 = run(20, 21, 260, 200, sets, false);
        ok(x2.first == 21, "an exploring look that clearly wins (260 vs 200) is taken");
        auto e = run(20, 21, 150, 200, sets);
        ok(e.first == 20, "a climb that loses messages is refused");
        vibeadsb::GainTrial f; f.start(5, 4, 0); f.cancel();
        ok(!f.active(), "a listener taking the gain voids the trial");
    }

    // ★★★ GainSearch — the look at entry (the V4L + LaNA on a YouLoop: 0 msg/s at the top, 40 four rungs down)
    {
        auto search = [](const std::vector<int>& order, const std::vector<double>& rateAt /* by index */, int& legs, bool climbing = false) {
            vibeadsb::GainSearch g; double t = 0; uint64_t total = 0; legs = 0;
            int at = g.start(order, t, climbing), last = at;
            for (int i = 0; i < 400 && g.active(); ++i) {
                t += 2; total += uint64_t(rateAt[size_t(at)] * 2);
                at = g.step(t, total);
                if (at != last) { ++legs; last = at; }
            }
            return g;
        };
        std::vector<int> order = {28, 26, 24, 22, 20, 18, 16, 14, 12, 10};
        std::vector<double> v4l(29, 0);
        v4l[28] = 0.5; v4l[26] = 40; v4l[24] = 120; v4l[22] = 160; v4l[20] = 150; v4l[18] = 90; v4l[16] = 40; v4l[14] = 10;
        int legs = 0;
        auto g = search(order, v4l, legs);
        ok(g.found() && g.result() == 22, "an overloaded top: the search lands on the peak four rungs down (22)");
        ok(g.rates()[0] >= 0 && g.rates()[0] < 2, "  ...having measured the top as nearly nothing");
        ok(g.rates().back() < 0, "  ...and stopped once well past the peak (not every rung tried)");
        std::vector<double> fa(29, 0);
        for (int i = 0; i < 29; ++i) fa[size_t(i)] = 1300 - (28 - i) * 60;   // Stuart's FlightAware feed: best at the top
        auto f = search(order, fa, legs);
        ok(f.found() && f.result() == 28, "a clean feed that peaks at the top stays at the top");
        ok(f.rates()[3] < 0, "  ...and stops early (a few rungs, not ten)");
        std::vector<double> flat(29, 1351);
        flat[22] = 1379; flat[16] = 1390;
        auto fl = search(order, flat, legs);
        ok(fl.found() && fl.result() == 28, "a flat curve (the recording: 1351..1390, all within counting noise) stays at the top");
        // ★★ THE RAMP — middle up (Stuart: "start in the middle and ramp up"), the R820T's 29 rungs: 14, 16 … 28
        std::vector<int> up = {14, 16, 18, 20, 22, 24, 26, 28};
        std::vector<double> v4lr(29, 0);
        for (int i = 0; i < 29; ++i) v4lr[size_t(i)] = i <= 25 ? 20 + i * 6.0 : 170 - (i - 25) * 55.0;   // peak at 42.1 (25)
        auto ru = search(up, v4lr, legs, true);
        ok(ru.found() && ru.result() == 24, "the V4L + LaNA: the ramp climbs to the peak's neighbourhood (24 of 22/24/26) and stops");
        ok(ru.rates()[0] >= 0 && ru.rates()[0] < ru.rates()[5], "  ...having measured the middle below the peak (8 s at the dead top at most)");
        std::vector<double> pi2(29, 0);
        for (int i = 0; i < 29; ++i) pi2[size_t(i)] = 100 + i * 10.0;   // a bare antenna: more gain, more aircraft
        auto rp = search(up, pi2, legs, true);
        ok(rp.found() && rp.result() == 28, "the Pi 2's bare antenna: the ramp climbs all the way to the top");
        std::vector<double> plat(29, 300);
        auto rpl = search(up, plat, legs, true);
        ok(rpl.found() && rpl.result() == 28, "a plateau CLIMBS on (the distant aircraft are what gain buys)");
        std::vector<double> quiet(29, 0.3);
        auto q = search(order, quiet, legs);
        ok(!q.active() && !q.found() && q.result() == 28, "an empty sky proves nothing: not found, the top, the noise rule from there");
    }

    std::printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", passes + fails, fails);
    return fails ? 1 : 0;
}
