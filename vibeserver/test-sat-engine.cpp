// test-sat-engine — V12 satellite engine: the curated list + element sets, the reach gate, and the DOPPLER LOCK
// (vibe_sat_engine.h; docs/v12/SATELLITE-ENGINE.md). Synthetic only — the element sets are the 2026-11-19 CelesTrak
// fixtures, the times are the passes Skyfield found for them (testdata-sat/reference-passes.txt).
//
// §1 catalogue: every listed bird gets elements from the fixtures; ages; the reach gate per radio range.
// §2 lock on an ISS FM pass: snaps to the nominal, the shift follows the model, NO STEP over 10 Hz a 25 Hz tick
//    anywhere in the pass (the smoothness rule), LOS ends it within the hold and freezes the shift.
// §3 ARMED before AOS → TRACK at AOS; the orphan rule.
// §4 the closed loop on a synthetic FM signal with a clock error and a radio ppm error: converges, COASTs through a gap
//    with the trim frozen, refuses a carrier outside the clamp, never steps.
// §5 a linear transponder (RS-44): model only, no jump on engagement, the loop ignored.
// §6 nudges and deliberate tunes; refusals (no location, NO TIME, old elements, nothing near).
// §7 the loop's lag sums: the SIMD build equals the scalar lane order exactly; cost.
#include "vibe_sat_engine.h"

#include <cmath>
#include <cstdlib>
#include <complex>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace vibe::sat;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; } else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

static const double kSaoLat = -23.5, kSaoLon = -46.6, kIoLat = 52.270833, kIoLon = -0.875;
// from reference-passes.txt
static const double kIssAos = 1795609400.724, kIssTca = 1795609729.166, kIssLos = 1795610051.907;   // São Paulo, 56.5°
static const double kRsTca = 1795604402.281, kRsLos = 1795604993.252;      // RS-44, IO92ng, 47.7°

static Engine* makeEngine(const std::string& dir, double lat, double lon) {
    auto* e = new Engine();
    e->loadOmm(readFile(dir + "/celestrak-amateur-2026-11-19.json"));
    e->loadOmm(readFile(dir + "/celestrak-weather-2026-11-19.json"));
    e->setObserver(lat, lon);
    return e;
}

static void s1(const std::string& dir) {
    std::printf("§1 catalogue\n");
    Engine e;
    const int a = e.loadOmm(readFile(dir + "/celestrak-amateur-2026-11-19.json"));
    const int w = e.loadOmm(readFile(dir + "/celestrak-weather-2026-11-19.json"));
    std::printf("   %d amateur + %d weather element sets for %zu listed birds\n", a, w, e.defs().size());
    CHECK(e.elementCount() == (int)e.defs().size(), "every listed bird has elements (%d of %zu)", e.elementCount(), e.defs().size());
    CHECK(e.loadOmm(readFile(dir + "/celestrak-amateur-2026-11-19.json")) == 0, "the same sets again replace nothing");
    CHECK(e.loadOmm("<html>blocked</html>") < 0, "an HTML page is refused, the sets kept");
    CHECK(e.elementCount() == (int)e.defs().size(), "still every bird after the bad page");
    CHECK(e.reaches(24e6, 1766e6), "an RTL (24–1766 MHz) reaches the list");
    CHECK(e.reaches(60e6, 260e6), "an Airspy HF+'s VHF range reaches 2 m / 137 MHz");
    CHECK(e.reaches(0, 31e6), "an HF radio reaches AO-7 mode A (29.4 MHz)");
    CHECK(!e.reaches(0, 28e6), "a radio below 28 MHz reaches nothing — no dead control");
    CHECK(!e.reaches(400e6, 406e6), "a 400–406 MHz window reaches nothing");
    CHECK(ageOf(1) == Age::Fresh && ageOf(5) == Age::Stale && ageOf(10) == Age::Old && ageOf(20) == Age::Expired, "the §6.3 age table");
    Clock c{kIssTca, true};
    e.setObserver(kSaoLat, kSaoLon);
    const std::string j = e.listJson(c, 24e6, 1766e6);
    CHECK(j.find("\"id\":\"ISS\"") != std::string::npos && j.find("\"id\":\"METEOR-M2-4\"") != std::string::npos, "the list names the birds");
    CHECK(j.find("\"passes\":[{") != std::string::npos, "the list carries passes");
    CHECK(j.size() < 16384, "the list is small (%zu bytes)", j.size());
    const std::string tr = e.trackJson(25544, kIssAos, kIssLos);
    CHECK(tr.size() > 20 && tr.front() == '[', "a sky track for the pass (%zu bytes)", tr.size());
}

static void s2(const std::string& dir) {
    std::printf("§2 ISS FM pass: snap, follow, smooth, LOS\n");
    std::unique_ptr<Engine> e(makeEngine(dir, kSaoLat, kSaoLon));
    const double t0 = kIssAos + 60;
    double d0 = 0, r0 = 0;
    CHECK(e->dopplerFor(25544, 437.8e6, t0, d0, r0), "the hook gives a Doppler");
    // the listener hand-tuned to where the signal is now, to the nearest 100 Hz (Kiko's way)
    const double dial = std::round((437.8e6 + d0) / 100.0) * 100.0;
    Track t = e->start("L1", "owner", dial, "nfm", Clock{t0, true});
    CHECK(t.state == LockState::Track, "locks (%s %s)", lockName(t.state), t.why.c_str());
    CHECK(t.nominalHz == 437.8e6 && t.satId == "ISS", "snaps to 437.800 ISS (%.0f %s)", t.nominalHz, t.satId.c_str());
    CHECK(std::fabs(t.nominalHz + t.shiftHz - dial) < 60, "no jump on engagement (%.1f Hz)", t.nominalHz + t.shiftHz - dial);
    CHECK(t.state == LockState::Track || t.state == LockState::Coast, "tracking");
    double last = t.shiftHz, worstStep = 0, worstModel = 0, maxAbs = 0, maxRate = 0;
    double endedAt = 0;
    for (double tt = t0 + 0.04; tt < kIssLos + 30; tt += 0.04) {
        Track k = e->tick("L1", Clock{tt, true});
        if (k.state == LockState::Ended) { if (endedAt == 0) endedAt = tt; continue; }
        worstStep = std::max(worstStep, std::fabs(k.shiftHz - last));
        double dm, rm; e->dopplerFor(25544, 437.8e6, tt, dm, rm);
        worstModel = std::max(worstModel, std::fabs(k.shiftHz - dm));
        maxAbs = std::max(maxAbs, std::fabs(k.shiftHz)); maxRate = std::max(maxRate, std::fabs(k.rateHzS));
        last = k.shiftHz;
    }
    std::printf("   max Doppler %.0f Hz, max rate %.0f Hz/s, worst 40 ms step %.2f Hz, model error %.2f Hz, ended %.1f s after LOS\n",
                maxAbs, maxRate, worstStep, worstModel, endedAt - kIssLos);
    CHECK(maxAbs > 8000 && maxAbs < 11000, "a 56° pass at 70 cm: about ±10 kHz (%.0f)", maxAbs);
    CHECK(maxRate > 80 && maxRate < 260, "rate peaks near TCA, under the brief's 215 Hz/s + margin (%.0f)", maxRate);
    CHECK(worstStep < 10.0, "no step over 10 Hz between 25 Hz ticks (%.2f)", worstStep);
    CHECK(worstModel < 1e-6, "no loop input = the model exactly (%.3g)", worstModel);
    CHECK(endedAt > kIssLos && endedAt - kIssLos < 5, "LOS ends it within the hold (%.1f s)", endedAt - kIssLos);
    Track f = e->tick("L1", Clock{kIssLos + 40, true});
    CHECK(f.state == LockState::Ended && std::fabs(f.shiftHz) > 1000, "ENDED keeps the last shift for the freeze (%.0f)", f.shiftHz);
    e->stop("L1");
    CHECK(!e->active("L1"), "stopped");
}

static void s3(const std::string& dir) {
    std::printf("§3 ARMED → TRACK, orphans\n");
    std::unique_ptr<Engine> e(makeEngine(dir, kSaoLat, kSaoLon));
    Track a = e->start("L1", "o", 437.8e6, "nfm", Clock{kIssAos - 600, true}, 25544);
    CHECK(a.state == LockState::Armed && std::fabs(a.aos - kIssAos) < 2, "armed for the AOS (%s, aos %+.1f s)", lockName(a.state), a.aos - kIssAos);
    CHECK(a.shiftHz == 0, "no shift while armed");
    Track b = e->tick("L1", Clock{kIssAos - 1, true});
    CHECK(b.state == LockState::Armed, "still armed a second before AOS");
    Track c = e->tick("L1", Clock{kIssAos + 2, true});
    CHECK(c.state == LockState::Track || c.state == LockState::Coast, "tracks at AOS (%s)", lockName(c.state));
    CHECK(c.shiftHz > 5000, "approaching: received high (%.0f)", c.shiftHz);
    e->start("L2", "o", 437.8e6, "nfm", Clock{kIssAos - 600, true}, 25544);   // 10 min: kept
    e->start("L3", "o", 437.8e6, "nfm", Clock{kIssAos - 3600, true}, 25544);  // an hour: an orphan when nobody is left
    auto gone = e->dropOrphanArms(Clock{kIssAos - 3600, true});
    CHECK(gone.size() == 2 && e->active("L1"), "arms over 30 min from AOS go; the active lock stays (%zu)", gone.size());
}

/** A synthetic FM-ish signal: a carrier at `offHz` in the channel, with a 1 kHz tone at ±3 kHz deviation, noise. */
static void synth(std::vector<float>& iq, int n, double rate, double offHz, double& ph, double& mph, bool on, std::mt19937& rng) {
    std::normal_distribution<float> g(0.0f, 0.05f);
    iq.resize(2 * (size_t)n);
    for (int i = 0; i < n; ++i) {
        mph += kTwoPi * 1000.0 / rate;
        const double f = offHz + 3000.0 * std::sin(mph);
        ph += kTwoPi * f / rate;
        const float a = on ? 1.0f : 0.0f;
        iq[2 * i] = a * (float)std::cos(ph) + g(rng);
        iq[2 * i + 1] = a * (float)std::sin(ph) + g(rng);
    }
    ph = std::fmod(ph, kTwoPi); mph = std::fmod(mph, kTwoPi);
}

static void s4(const std::string& dir, double t0, const char* where) {
    std::printf("§4 the closed loop (clock 1.5 s late + radio 1.6 ppm), from %s\n", where);
    std::unique_ptr<Engine> e(makeEngine(dir, kSaoLat, kSaoLon));
    const double rate = 48000;
    const double ppmHz = 1.6e-6 * 437.8e6;   // ≈ 700 Hz
    e->start("L1", "o", 437.8e6, "nfm", Clock{t0, true}, 25544);
    ResidualMeter meter;
    std::mt19937 rng(7);
    std::vector<float> iq;
    double ph = 0, mph = 0, applied = 0, lastApplied = 0, worstStep = 0;
    double errAt20 = 1e9, errAfterGap = 1e9, trimMax = 0;
    bool coastSeen = false, trimFrozen = true;
    double trimAtGap = 0;
    const double blk = 0.02;   // 20 ms blocks; the tracker ticks every 40 ms
    int ticks = 0;
    for (double tt = t0; tt < t0 + 60; tt += blk) {
        // the truth: the model evaluated 1.5 s late (the server clock is 1.5 s behind), plus the radio's error
        double truth, r; e->dopplerFor(25544, 437.8e6, tt + 1.5, truth, r);
        truth += ppmHz;
        const bool on = !(tt > t0 + 30 && tt < t0 + 36);     // a 6 s gap between overs
        if ((ticks++ % 2) == 0) {
            Track k = e->tick("L1", Clock{tt, true});
            applied = k.shiftHz;
            if (ticks > 2) worstStep = std::max(worstStep, std::fabs(applied - lastApplied) - std::fabs(r) * 0.04);
            lastApplied = applied;
            trimMax = std::max(trimMax, std::fabs(k.trimHz));
            if (tt > t0 + 30 && tt < t0 + 35.5 && k.state == LockState::Coast) {
                if (!coastSeen) trimAtGap = k.trimHz;
                coastSeen = true;
                if (std::fabs(k.trimHz - trimAtGap) > 1e-6) trimFrozen = false;
            }
            if (std::getenv("SAT_TRACE") && (ticks % 25) == 1) std::printf("   t=%5.1f truth %8.1f applied %8.1f err %7.1f trim %7.1f %s\n", tt - t0, truth, applied, truth - applied, k.trimHz, lockName(k.state));
        }
        synth(iq, (int)(rate * blk), rate, truth - applied, ph, mph, on, rng);
        double hz;
        if (meter.feed(iq.data(), (int)(rate * blk), rate, hz)) e->noteResidual("L1", hz, tt);
        if (std::fabs(tt - (t0 + 20)) < blk / 2) errAt20 = std::fabs(truth - applied);
        if (std::fabs(tt - (t0 + 45)) < blk / 2) errAfterGap = std::fabs(truth - applied);
    }
    std::printf("   error after 20 s %.1f Hz, after the gap %.1f Hz, worst step beyond the model's own %.2f Hz, max trim %.0f Hz\n",
                errAt20, errAfterGap, worstStep, trimMax);
    CHECK(errAt20 < 40, "converged within 40 Hz in 20 s (%.1f)", errAt20);
    CHECK(errAfterGap < 40, "still centred after the gap (%.1f)", errAfterGap);
    CHECK(coastSeen, "COAST during the gap");
    CHECK(trimFrozen, "the trim frozen in COAST");
    CHECK(worstStep < 4.5, "the trim never steps (slew-limited: %.2f Hz a tick)", worstStep);
    CHECK(trimMax <= Engine::kTrimClampHz + 1e-9, "trim inside the clamp");
    if (t0 > kIssAos + 100) return;   // the refusals below once
    // a carrier 6 kHz away (the steady carrier beside ISS in Kiko's waterfall) never drags the lock there
    std::unique_ptr<Engine> e2(makeEngine(dir, kSaoLat, kSaoLon));
    e2->start("L1", "o", 437.8e6, "nfm", Clock{t0, true}, 25544);
    for (int i = 0; i < 200; ++i) e2->noteResidual("L1", 6000.0, t0 + i * 0.4);
    Track k2 = e2->tick("L1", Clock{t0 + 80, true});
    CHECK(std::fabs(k2.trimHz) < 1e-9, "a 6 kHz residual is refused outright (trim %.1f)", k2.trimHz);
    for (int i = 0; i < 400; ++i) { e2->noteResidual("L1", 2400.0, t0 + 80 + i * 0.4); e2->tick("L1", Clock{t0 + 80 + i * 0.4, true}); }
    Track k3 = e2->tick("L1", Clock{t0 + 240, true});
    CHECK(std::fabs(k3.trimHz) <= Engine::kTrimClampHz + 1e-9, "a persistent pull stops at the clamp (%.1f)", k3.trimHz);
    // noise only: the gate keeps the loop shut
    ResidualMeter m2; int opened = 0; double ph2 = 0, mph2 = 0;
    for (int i = 0; i < 1500; ++i) { synth(iq, 960, rate, 0, ph2, mph2, false, rng); double hz; if (m2.feed(iq.data(), 960, rate, hz)) ++opened; }
    CHECK(opened == 0, "30 s of noise never opens the gate (%d)", opened);
}

static void s5(const std::string& dir) {
    std::printf("§5 RS-44 linear transponder\n");
    std::unique_ptr<Engine> e(makeEngine(dir, kIoLat, kIoLon));
    const double t0 = kRsTca - 120;
    const double dial = 435640000;
    Track t = e->start("L1", "o", dial, "usb", Clock{t0, true});
    CHECK(t.state == LockState::Track && t.satId == "RS-44", "locks RS-44 (%s %s)", lockName(t.state), t.satId.c_str());
    CHECK(std::fabs(t.nominalHz + t.shiftHz - dial) < 1.0, "no jump: the channel stays where the listener was (%.2f Hz)", t.nominalHz + t.shiftHz - dial);
    CHECK(t.loop == "none", "model only");
    e->noteResidual("L1", 300, t0);
    Track k = e->tick("L1", Clock{t0 + 1, true});
    CHECK(k.trimHz == 0 && k.state == LockState::Track, "the loop's input is ignored on a linear transponder (no COAST either)");
    double last = k.shiftHz, worst = 0;
    for (double tt = t0 + 1.04; tt < kRsLos - 10; tt += 0.04) { Track x = e->tick("L1", Clock{tt, true}); worst = std::max(worst, std::fabs(x.shiftHz - last)); last = x.shiftHz; }
    CHECK(worst < 10, "smooth through TCA (%.2f Hz a tick)", worst);
}

static void s6(const std::string& dir) {
    std::printf("§6 nudges, tunes, refusals\n");
    std::unique_ptr<Engine> e(makeEngine(dir, kSaoLat, kSaoLon));
    const double t0 = kIssTca;
    e->start("L1", "o", 437.8e6, "nfm", Clock{t0, true});
    CHECK(e->nudge("L1", 437.8005e6), "a 500 Hz nudge keeps the lock");
    CHECK(e->tick("L1", Clock{t0 + 0.04, true}).nominalHz == 437.8005e6, "the nominal follows the nudge");
    CHECK(!e->nudge("L1", 437.82e6), "a 20 kHz tune is deliberate — the caller ends the lock");
    Engine none;
    Track r1 = none.start("L1", "o", 437.8e6, "nfm", Clock{t0, true});
    CHECK(r1.state == LockState::Off && r1.why.find("location") != std::string::npos, "no location: refused in words (%s)", r1.why.c_str());
    std::unique_ptr<Engine> e2(makeEngine(dir, kSaoLat, kSaoLon));
    Track r2 = e2->start("L1", "o", 437.8e6, "nfm", Clock{1.0e9, false});
    CHECK(r2.state == LockState::Off && r2.time == "none" && r2.why.find("NO TIME") != std::string::npos, "a 2001 clock, unmeasured: NO TIME (%s)", r2.why.c_str());
    Track r2b = e2->start("L1", "o", 437.8e6, "nfm", Clock{kIssTca, false});
    CHECK(r2b.time == "system" && r2b.state != LockState::Off, "an unmeasured but plausible clock tracks, marked SYSTEM (%s)", r2b.time.c_str());
    e2->stop("L1");
    Track r3 = e2->start("L1", "o", 437.8e6, "nfm", Clock{kIssTca + 20 * 86400.0, true});
    CHECK(r3.state == LockState::Off && r3.why.find("too old") != std::string::npos, "20-day-old elements: refused (%s)", r3.why.c_str());
    Track r4 = e2->start("L1", "o", 100.0e6, "wfm", Clock{kIssTca, true});
    CHECK(r4.state == LockState::Off && !r4.why.empty(), "100 MHz: nothing there (%s)", r4.why.c_str());
    Track stale = e2->start("L5", "o", 437.8e6, "nfm", Clock{kIssTca + 5 * 86400.0, true});
    CHECK(stale.ageState == "stale" || stale.state != LockState::Track, "5-day-old elements show STALE (%s)", stale.ageState.c_str());
    const std::string m = Engine::trackJsonMsg(e->tick("L1", Clock{t0 + 1, true}));
    CHECK(m.find("\"type\":\"sat\"") == 1 && m.find("\"nominal\":437800500") != std::string::npos && m.find("\"offset\":") != std::string::npos,
          "the readout message (%s)", m.substr(0, 120).c_str());
    double dz, rz, el;
    CHECK(e->dopplerFor(57166, 137.9e6, t0, dz, rz, &el) && std::fabs(dz) < 4000, "hook: Meteor-M2-3 at 137.9 MHz (%.0f Hz, el %.1f)", dz, el);
    CHECK(!e->dopplerFor(12345, 137.9e6, t0, dz, rz), "hook: an unlisted bird is refused");
}

static void refLag(const float* iq, int n, float prev[2], LagSums& acc) {
    float hr = sumOfProducts(iq[0], prev[0], iq[1], prev[1]), hi = diffOfProducts(iq[1], prev[0], iq[0], prev[1]);
    float hp = sumOfProducts(iq[0], iq[0], iq[1], iq[1]), h4 = squared(hp);
    float lr[4] = {0, 0, 0, 0}, li[4] = {0, 0, 0, 0}, lp[4] = {0, 0, 0, 0}, l4[4] = {0, 0, 0, 0};
    int i = 1; const int full = 1 + ((n - 1) / 4) * 4;
    for (; i < full; i += 4) for (int k = 0; k < 4; ++k) {
        const float* c = iq + 2 * (i + k); const float* p = c - 2;
        lr[k] = lr[k] + sumOfProducts(c[0], p[0], c[1], p[1]); li[k] = li[k] + diffOfProducts(c[1], p[0], c[0], p[1]);
        const float q = sumOfProducts(c[0], c[0], c[1], c[1]); lp[k] = lp[k] + q; l4[k] = l4[k] + squared(q);
    }
    for (; i < n; ++i) { const float* c = iq + 2 * i; const float* p = c - 2;
        hr = hr + sumOfProducts(c[0], p[0], c[1], p[1]); hi = hi + diffOfProducts(c[1], p[0], c[0], p[1]);
        const float q = sumOfProducts(c[0], c[0], c[1], c[1]); hp = hp + q; h4 = h4 + squared(q); }
    acc.re += (double)((lr[0] + lr[1]) + (lr[2] + lr[3])) + (double)hr;
    acc.im += (double)((li[0] + li[1]) + (li[2] + li[3])) + (double)hi;
    acc.pw += (double)((lp[0] + lp[1]) + (lp[2] + lp[3])) + (double)hp;
    acc.p4 += (double)((l4[0] + l4[1]) + (l4[2] + l4[3])) + (double)h4;
    acc.n += n; prev[0] = iq[2 * (n - 1)]; prev[1] = iq[2 * (n - 1) + 1];
}

static void s7() {
    std::printf("§7 lag sums: SIMD (%s) vs the scalar lane order; cost\n", VIBE_NEON ? "NEON" : VIBE_SSE ? "SSE2" : "scalar");
    std::mt19937 rng(3); std::normal_distribution<float> g(0, 1);
    bool same = true;
    for (int n : {1, 2, 3, 4, 5, 7, 8, 9, 63, 64, 65, 960, 4097}) {
        std::vector<float> v(2 * n); for (auto& x : v) x = g(rng);
        float p1[2] = {0.3f, -0.2f}, p2[2] = {0.3f, -0.2f};
        LagSums a, b; lagAccumulate(v.data(), n, p1, a); refLag(v.data(), n, p2, b);
        if (a.re != b.re || a.im != b.im || a.pw != b.pw || a.p4 != b.p4 || p1[0] != p2[0]) { same = false; std::printf("   n=%d differs: %.9g vs %.9g\n", n, a.re, b.re); }
    }
    CHECK(same, "identical sums at every length (lanes, tail, carry)");
    std::vector<float> v(2 * 48000); for (auto& x : v) x = g(rng);
    float p[2] = {0, 0}; LagSums s;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int k = 0; k < 200; ++k) lagAccumulate(v.data(), 48000, p, s);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / 200;
    std::printf("   one second of a 48 kS/s channel: %.1f µs (%.4f %% of a core) [%g]\n", us, us / 1e4, s.re * 0);
    CHECK(us < 2000, "cheap");
}

int main(int argc, char** argv) {
    std::string dir = "vibeserver/testdata-sat";
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--data" && i + 1 < argc) dir = argv[++i];
    if (readFile(dir + "/celestrak-amateur-2026-11-19.json").empty()) { std::printf("data missing\n"); return 3; }
    s1(dir); s2(dir); s3(dir); s4(dir, kIssAos + 30, "AOS + 30 s"); s4(dir, kIssTca - 40, "TCA − 40 s (the fastest Doppler)"); s5(dir); s6(dir); s7();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
