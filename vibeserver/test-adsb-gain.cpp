// test-adsb-gain.cpp — the ADS-B gain rule (vibe_adsb_service.h): GainControl (the noise ceiling, its hysteresis,
// strong-reply pressure, the plateau/cliff stops, the SDRplay's look-down-only rule, the "hearing little" warning) on
// model feeds built from real measurements — Stuart's FlightAware Pi (filtered LNA, the 10-24 clip's floor), the Pi 500
// V4L (wideband LaNA, its beta13 message curve), the Pi 2's bare antenna, the May–June 2025 antenna fault — then
// GainTrial and GainSearch on their own.
#include "vibe_adsb_service.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>
#include <utility>

static int fails = 0, passes = 0;
static void ok(bool c, const char* what) { if (c) passes++; else { fails++; std::printf("FAIL %s\n", what); } }

// the R820T's list, tenths of a dB — what an RTL reports
static const std::vector<int> kRtl = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338,
                                      364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
static int idxOf(double db) { for (size_t i = 0; i < kRtl.size(); ++i) if (std::fabs(kRtl[i] / 10.0 - db) < 0.05) return int(i); return -1; }

/** A model feed: at gain g dB, the floor (dBFS on Demod's scale), believed msg/s, the strong share, the furthest NM. */
struct Feed {
    std::function<double(double)> floor, msgs, strong = [](double) { return 0.08; }, far = [](double) { return 180.0; };
    double floorJitter = 0.3;   ///< dB, per tick
    bool poisson = true;
};
struct Run { std::vector<int> idx; int changes = 0; int settledChanges = 0; std::vector<std::string> whys; std::string warn; int maxIdx = 0; int lastSettled = -1; };
/** Run the rule for `secs` at the shim's 2 s tick. `from`: begin() at t = 0. */
static Run sim(vibeadsb::GainControl& c, const Feed& f, double secs, bool held = false, bool begin = true, double t0 = 0,
               std::function<void(double, vibeadsb::GainControl&)> each = nullptr) {
    static uint64_t total = 0;
    std::mt19937 rng(1234);
    std::normal_distribution<double> nd(0, 1);
    Run r;
    int at = begin ? c.begin(kRtl, held, t0) : c.index();
    r.maxIdx = at;
    for (double t = t0 + 2; t <= t0 + secs; t += 2) {
        const double g = kRtl[size_t(at)] / 10.0;
        const double rate = std::max(0.0, f.msgs(g));
        double got = rate * 2;
        if (f.poisson && got > 0) got = std::max(0.0, got + nd(rng) * std::sqrt(got));
        total += uint64_t(std::lround(got));
        vibeadsb::Health h;
        h.valid = true; h.msgsTotal = total; h.msgsPerSec = got / 2;
        h.noiseDbfs = f.floor(g) + nd(rng) * f.floorJitter;
        h.strongFrac = f.strong(g); h.farNm = f.far(g); h.aircraft = int(rate / 8);
        const int nx = c.tick(t, h);
        if (nx != at) ++r.changes;
        if (!c.busy()) { if (r.lastSettled >= 0 && nx != r.lastSettled && t > t0 + 120) ++r.settledChanges; r.lastSettled = nx; }
        at = nx; r.idx.push_back(at); r.maxIdx = std::max(r.maxIdx, at);
        if (r.whys.empty() || r.whys.back() != c.why()) r.whys.push_back(c.why());
        if (each) each(t, c);
    }
    r.warn = c.warning();
    return r;
}

int main() {
    // ★★★ THE FLIGHTAWARE PI: filtered LNA feed. Floor from the 10-24 clip (−21.6 dBFS at 49.6, dB for dB with the gain);
    //     messages CREEP UP with gain to the top (the A/B: +5.8 % at 49.6) — the shape that pinned the old rule there.
    {
        Feed fa;
        fa.floor = [](double g) { return -21.6 + (g - 49.6); };
        fa.msgs = [](double g) { return 1250 + 4.0 * g; };
        fa.strong = [](double g) { return g > 45 ? 0.5 : 0.11; };
        vibeadsb::GainControl c;
        Run r = sim(c, fa, 3600);
        const double settled = kRtl[size_t(r.lastSettled)] / 10.0;
        std::printf("   FlightAware Pi model: settled %.1f dB (dump1090-fa: 38.6-40.2), %d settled changes after the ramp in an hour, why: %s\n", settled, r.settledChanges, c.why().c_str());
        ok(settled >= 38.5 && settled <= 40.3, "the FlightAware Pi's filtered LNA feed settles at 38.6-40.2 dB, where dump1090-fa sits — not 49.6");
        ok(r.maxIdx <= idxOf(43.4), "  ...the ramp never goes past the first rung over the ceiling (43.4)");
        ok(c.why().find("noise floor at its target") != std::string::npos, "  ...and says why: the noise floor is at its target");
        ok(r.warn.empty(), "  ...and no feed warning");
        ok(r.settledChanges <= 1, "  ...and holds: the settled gain does not move after the ramp (the ten-minute looks each return)");
        // the old message-only climb on the same feed (GainSearch, no ceiling) — the bug, measured
        vibeadsb::GainSearch old; double t = 0; uint64_t tot = 0;
        std::vector<int> up; for (int i = 14; i < 29; i += 2) up.push_back(i);
        int at = old.start(up, t, true);
        for (int i = 0; i < 400 && old.active(); ++i) { t += 2; tot += uint64_t(fa.msgs(kRtl[size_t(at)] / 10.0) * 2); at = old.step(t, tot); }
        ok(old.result() == 28, "  (the old rule, messages only, on this feed: 49.6 dB — the A/B's finding reproduced)");
    }
    // ★★★ THE PI 500 V4L: YouLoop + a WIDEBAND LaNA, no 1090 filter — the beta13 curve with its cliff at 44.5 dB
    {
        auto v4lMsgs = [](double g) {
            const double pts[][2] = {{25.4, 52}, {29.7, 85}, {33.8, 155}, {37.2, 182}, {40.2, 196}, {43.4, 196}, {44.5, 52}, {49.6, 8}};
            if (g <= pts[0][0]) return pts[0][1] * g / 25.4;
            for (int i = 0; i < 7; ++i) if (g <= pts[i + 1][0]) {
                const double u = (g - pts[i][0]) / (pts[i + 1][0] - pts[i][0]); return pts[i][1] + u * (pts[i + 1][1] - pts[i][1]); }
            return 8.0;
        };
        Feed v; v.msgs = v4lMsgs;
        v.floor = [](double g) { return -31.7 + 1.5 * (g - 40.2); };   // the noted −31.7 near 40 dB, −16.9 at the top
        v.far = [](double) { return 63.0; };
        vibeadsb::GainControl c;
        Run r = sim(c, v, 3600);
        const double settled = kRtl[size_t(r.lastSettled)] / 10.0;
        std::printf("   Pi 500 V4L model (floor -31.7 @ 40.2): settled %.1f dB (Stuart's screen: 40.2), why: %s\n", settled, c.why().c_str());
        ok(std::fabs(settled - 40.2) < 0.05, "the V4L on its general aerial settles at 40.2 dB, as it does today");
        ok(r.maxIdx < idxOf(44.5), "  ...and never tries the cliff at 44.5 (nor the dead top)");
        ok(r.warn.empty(), "  ...no warning: it is not at full gain (63 NM is that aerial, not a fault)");
        Feed q = v; q.floor = [](double g) { return -38 + (g - 40.2); };   // a quieter floor: the cliff alone must stop it
        vibeadsb::GainControl c2;
        Run r2 = sim(c2, q, 3600);
        ok(std::fabs(kRtl[size_t(r2.lastSettled)] / 10.0 - 40.2) < 0.05, "  ...and with a quiet floor (no ceiling), the message cliff alone still lands it at 40.2");
        ok(c2.why().find("decodes the most") != std::string::npos || c2.why().find("fell away") != std::string::npos
           || c2.why().find("checked one step") != std::string::npos, "  ...saying it is the messages that chose");
        // ★★ a V4L whose floor is a little HIGHER (−29 at 40.2: over the ceiling) — the ceiling would cut it to 37.2
        //    (182 msg/s against 196); one rung over is kept because the sky clearly says so
        Feed hi = v; hi.floor = [](double g) { return -29.0 + 1.5 * (g - 40.2); };
        vibeadsb::GainControl c3;
        Run r3 = sim(c3, hi, 3600);
        std::printf("   V4L with its floor at -29 dBFS @ 40.2: settled %.1f dB, why: %s\n", kRtl[size_t(r3.lastSettled)] / 10.0, c3.why().c_str());
        ok(std::fabs(kRtl[size_t(r3.lastSettled)] / 10.0 - 40.2) < 0.05, "  ...a V4L whose floor sits over the ceiling at 40.2 still keeps 40.2: one rung over, earned in messages");
        ok(r3.settledChanges <= 1, "  ...and the noise guard does not take it back down (the ceiling moved with it)");
        ok(r3.maxIdx < idxOf(44.5), "  ...and still never the cliff");
    }
    // ★★ THE PI 2: a bare antenna, no LNA — the floor never reaches the ceiling, more gain hears more
    {
        Feed p; p.floor = [](double g) { return -42 - 0.5 * (49.6 - g); }; p.msgs = [](double g) { return 100 + 4 * g; };
        p.far = [](double) { return 150.0; };
        vibeadsb::GainControl c;
        Run r = sim(c, p, 3600);
        ok(r.lastSettled == 28, "the Pi 2's bare antenna: the top (49.6)");
        ok(c.why().find("top of the radio") != std::string::npos || c.why().find("checked one step") != std::string::npos, "  ...because it is the top and the floor has room");
        ok(r.warn.empty(), "  ...hearing 150 NM: no warning");
    }
    // ★★★ THE MAY–JUNE 2025 FAULT: full gain, the floor 8 dB under the ceiling, 17–41 aircraft out to 41–70 NM
    {
        Feed d; d.floor = [](double g) { return -38 + (g - 49.6); }; d.msgs = [](double g) { return 60 + 3 * g; };
        d.far = [](double) { return 55.0; }; d.strong = [](double) { return 0.0; };
        vibeadsb::GainControl c;
        bool earlyWarn = false;
        Run r = sim(c, d, 1800, false, true, 0, [&](double t, vibeadsb::GainControl& k) { if (t < 300 && !k.warning().empty()) earlyWarn = true; });
        ok(r.lastSettled == 28, "the faulty antenna: the rule climbs to the top (there is nothing else to do)");
        ok(!r.warn.empty() && r.warn.find("hearing little") != std::string::npos && r.warn.find("55 NM") != std::string::npos,
           "  ...and after ten minutes says so plainly: full gain, hearing little, nothing beyond 55 NM");
        std::printf("   warning: %s\n", r.warn.c_str());
        ok(!earlyWarn, "  ...but not in its first minutes (the ramp, then 10 min at the top)");
        // repaired: the floor comes back to normal → the rule comes down, the warning clears
        Feed fixed; fixed.floor = [](double g) { return -21.6 + (g - 49.6); }; fixed.msgs = [](double g) { return 1250 + 4 * g; };
        Run r2 = sim(c, fixed, 1200, false, false, 1800);
        ok(r2.warn.empty(), "  ...repaired: the warning clears");
        ok(kRtl[size_t(r2.lastSettled)] / 10.0 <= 40.3, "  ...and the noise guard brings the gain down to the target without a new ramp");
        // no position: judged by the counts
        Feed np = d; np.far = [](double) { return -1.0; };
        vibeadsb::GainControl c3; Run r3 = sim(c3, np, 1800);
        ok(!r3.warn.empty() && r3.warn.find("NM") == std::string::npos, "  ...a server with no position of its own warns on the counts (under 40 aircraft, 400 msg/s)");
        Feed busy = np; busy.msgs = [](double) { return 900.0; };
        vibeadsb::GainControl c4; Run r4 = sim(c4, busy, 1800);
        ok(r4.warn.empty(), "  ...but not with 900 msg/s coming in (a busy sky is no dead feed)");
    }
    // ★★ A HOT FEED: the middle is already over the ceiling — it steps DOWN until it is under
    {
        Feed h; h.floor = [](double g) { return -20 + (g - 25.4); }; h.msgs = [](double) { return 300.0; };
        vibeadsb::GainControl c;
        Run r = sim(c, h, 600);
        const double f = -20 + (kRtl[size_t(r.lastSettled)] / 10.0 - 25.4);
        ok(f <= -29.5 && f > -34, "a hot feed (the middle at -20 dBFS): descends until the floor is under -30 (as measured, ±0.3 dB)");
    }
    // ★★ HYSTERESIS — no hunting on a floor that sits on the line
    {
        Feed e; e.floor = [](double g) { return -30.4 + (g - 40.2); }; e.msgs = [](double) { return 800.0; }; e.floorJitter = 0.8;
        vibeadsb::GainControl c;
        Run r = sim(c, e, 3 * 3600);
        std::printf("   on the line (-30.4 at 40.2, +-0.8 dB jitter): %d settled changes in 3 h after the ramp\n", r.settledChanges);
        ok(kRtl[size_t(r.lastSettled)] / 10.0 == 40.2, "a floor 0.4 dB under the ceiling with 0.8 dB of jitter: stays at 40.2");
        ok(r.settledChanges <= 1, "  ...three hours with no hunting (the ten-minute looks each return)");
        // over by 0.5 dB (inside the 1 dB hysteresis): stays; over by 2 dB: one rung down after 10 s
        vibeadsb::GainControl d;
        Feed in = e; in.floorJitter = 0.1;
        sim(d, in, 600);
        const int at = d.index();
        Feed over = in; over.floor = [](double g) { return -29.5 + (g - 40.2); };
        Run r2 = sim(d, over, 120, false, false, 600);
        ok(r2.lastSettled == at, "  over the target by 0.5 dB (inside the hysteresis): stays");
        Feed over2 = in; over2.floor = [](double g) { return -28.0 + (g - 40.2); };
        int firstMove = -1;
        Run r3 = sim(d, over2, 60, false, false, 720, [&](double t, vibeadsb::GainControl& k) { if (firstMove < 0 && k.index() != at) firstMove = int(t - 720); });
        ok(r3.lastSettled == at - 1, "  over by 2 dB: one rung down");
        ok(firstMove >= 10 && firstMove <= 16, "  ...after the 10 s change delay, not at once");
    }
    // ★★ STRONG-REPLY PRESSURE
    {
        Feed s; s.floor = [](double g) { return -45 + 0.3 * (g - 49.6); }; s.msgs = [](double) { return 600.0; };
        s.strong = [](double g) { return g > 45 ? 0.45 : 0.1; };
        vibeadsb::GainControl c;
        Run r = sim(c, s, 2400);
        ok(kRtl[size_t(r.lastSettled)] / 10.0 <= 44.5, "45 % of replies clipping at the top: backed off below it");
        bool said = false; for (auto& w : r.whys) if (w.find("strong replies") != std::string::npos) said = true;
        ok(said, "  ...and said why (strong replies clipping)");
        Feed mild = s; mild.strong = [](double) { return 0.12; };
        vibeadsb::GainControl c2; Run r2 = sim(c2, mild, 2400);
        ok(r2.lastSettled == 28, "  12 % strong (the FlightAware Pi's healthy share) is no reason to come down");
        Feed lose = s; lose.msgs = [](double g) { return g > 49 ? 600.0 : 400.0; };
        vibeadsb::GainControl c3; Run r3 = sim(c3, lose, 2400);
        ok(r3.lastSettled == 28, "  ...and a cut that clearly loses messages is refused even under pressure");
    }
    // ★ AN EMPTY SKY: the noise rule alone
    {
        Feed q; q.floor = [](double g) { return -21.6 + (g - 49.6); }; q.msgs = [](double) { return 0.3; };
        vibeadsb::GainControl c; Run r = sim(c, q, 600);
        ok(std::fabs(kRtl[size_t(r.lastSettled)] / 10.0 - 40.2) < 0.05, "an empty sky on the FlightAware feed: the noise rule alone still picks 40.2");
        ok(c.why().find("too few aircraft") != std::string::npos, "  ...and says that is why");
    }
    // ★★ AN SDRplay (levelHeld): its IF AGC holds the floor; it never climbs above where it starts, and only looks down
    {
        std::vector<int> rsp; for (int g = 0; g <= 490; g += 10) rsp.push_back(g);
        (void)rsp;
        Feed r; r.floor = [](double) { return -43.0; }; r.msgs = [](double) { return 400.0; };
        vibeadsb::GainControl c;
        Run x = sim(c, r, 3 * 3600, true);
        const int start = int(kRtl.size()) * 3 / 4;
        ok(x.maxIdx <= start, "SDRplay: three hours, never above where it started (three-quarters up)");
        std::printf("   SDRplay flat curve, 3 h: settled at index %d (start %d)\n", x.lastSettled, start);
        ok(x.lastSettled >= start - 2, "  ...a flat curve stays at (or within counting noise of) the start — no ratchet down");
        ok(c.why().find("holds its own level") != std::string::npos || c.why().find("checked one step") != std::string::npos, "  ...and says the radio holds its own level");
        ok(c.warning().empty(), "  ...never the feed warning (its floor is its AGC's)");
        Feed hot = r; hot.strong = [](double) { return 0.5; };
        vibeadsb::GainControl c2; Run y = sim(c2, hot, 1200, true);
        ok(y.lastSettled < start, "  ...replies clipping: steps the LNA down");
        Feed low = r; low.msgs = [start](double g) { return g < kRtl[size_t(start)] / 10.0 - 3 ? 500.0 : 400.0; };
        vibeadsb::GainControl c3; Run z = sim(c3, low, 1200, true);
        ok(z.lastSettled < start, "  ...a lower LNA state that clearly decodes more is found by the look down");
    }
    // ★ the up-look is blocked by the ceiling's prediction: at 40.2 (−31 dBFS), 42.1 would be −29.1
    {
        Feed fa; fa.floor = [](double g) { return -21.6 + (g - 49.6); }; fa.msgs = [](double g) { return g > 41 ? 2000.0 : 1300.0; };
        fa.floorJitter = 0.05;
        vibeadsb::GainControl c; Run r = sim(c, fa, 3 * 3600);
        std::printf("   a feed whose messages jump +54 %% one ramp step over the ceiling: settled %.1f dB, why: %s\n", kRtl[size_t(r.lastSettled)] / 10.0, c.why().c_str());
        ok(r.maxIdx <= idxOf(43.4), "a feed that decodes far more one step over the ceiling: never further than that one step, in 3 h");
        ok(kRtl[size_t(r.lastSettled)] / 10.0 <= 43.5 && c.why().find("one step over") != std::string::npos,
           "  ...and when it keeps that step it says why (one step over the target, earned in messages)");
        // the FlightAware shape (+~0.3 %% a rung) never earns it — checked above (settles 40.2)
    }

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
        // ★★ Stuart's real V4L curve (beta13 log, 23:39): 25.4=52 29.7=85 33.8=155 37.2=182 40.2=196 43.4=196 44.5=52 49.6=8
        //    — the ramp's rungs 14 16 … 28 of the R820T list, by index
        std::vector<double> real(29, 0);
        real[14] = 52; real[16] = 85; real[18] = 155; real[20] = 182; real[22] = 196; real[24] = 196; real[26] = 52; real[28] = 8;
        auto rr = search(up, real, legs, true);
        ok(rr.found() && rr.result() == 22, "the real V4L curve: settles one rung back from the cliff's edge (22 = 40.2, not 24 = 43.4)");
        ok(rr.rates()[7] < 0, "  ...and never tries the dead top: the cliff at 44.5 stops the ramp at once");
        std::vector<double> sharp(29, 0);
        for (int i = 0; i < 29; ++i) sharp[size_t(i)] = i <= 24 ? 50 + i * 6.0 : 10;
        auto rs = search(up, sharp, legs, true);
        ok(rs.found() && rs.result() == 24, "a rising curve that then falls off a cliff: the best stays (the rung before is not on a plateau)");
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
