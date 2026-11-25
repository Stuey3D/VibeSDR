// test-sat-pass-sched — WHICH UNATTENDED SATELLITE WORK RUNS (vibe_sat_pass_sched.h, beta94): a Meteor pass, ORBCOMM sky-
// plot monitoring, or both on one capture; the priority only where the capture cannot hold both.
//
// §1 the capture fit per radio: an RTL at 2.4 MS/s and an RSP at 2 MS/s hold METEOR (137.1 and 137.9) + the ORBCOMM band
//    with the DC clear of the Meteor signal; an Airspy HF+ at 912 / 768 kS/s does not.
// §2 both together on a wide capture — a loaded box too (never dropped for CPU).
// §3 the priority on a narrow capture, both ways; ORBCOMM first never takes a pass.
// §4 a Meteor pass interrupts ORBCOMM at AOS and hands back at LOS (narrow, Meteor first); on a wide capture ORBCOMM runs on.
// §5 ORBCOMM switched off, out of the radio's reach, no bird up; a listener; a blocked radio.
// §6 the admin lines ("ORBCOMM monitoring (sky plot) — no Meteor pass for 1 h 12 min", "… — ORBCOMM paused (priority…").
// §7 the IF filter (vibe_sat_if.h): with both running, the regions hold the whole ORBCOMM band AND the Meteor signal round
//    the chosen centre — the pass alone would have cut the band.
// Silent. Run: scripts/run-tests.sh (VIBE_TESTS="test-sat-pass-sched").
#include "vibe_sat_engine.h"
#include "vibe_sat_if.h"
#include "vibe_sat_pass_sched.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibe::satsched;

static vibe::sat::Transmitter meteorTx(double hz) { vibe::sat::Transmitter t; t.hz = hz; t.consumer = "lrpt"; t.mode = "lrpt"; t.baud = 72000; t.label = "LRPT"; return t; }
static Region regionOfTx(const vibe::sat::Transmitter& t) {
    const vibesatif::Region r = vibesatif::regionOf(t);
    Region o; o.lo = r.centreHz - r.halfHz; o.hi = r.centreHz + r.halfHz; return o;
}
// ★ usable spans as the shim computes them: min(usableSpan — the capture minus an HF+'s cropped edges — and the flat part
//   a slice is cut from, vibe_band_slice.h kBandFlatFrac). An RTL / RSP / Airspy R2 crop nothing; the HF+ crops to 768 kHz
//   at 912 kS/s and 60 kHz a side at 768.
static const double kFlat = 1.0 - 2.0 / 16.0 - 0.01;   // vibe::kBandFlatFrac
static const double kRtl24 = 2.4e6 * kFlat, kRsp2 = 2.0e6 * kFlat, kHfp912 = 768e3, kHfp768 = 768e3 - 120e3;

static In base(double usable) {
    In in; in.meteorOn = true; in.orbcommOn = true; in.orbcommUp = true; in.usableHz = usable; in.now = 1e9;
    in.passUp = true; in.passName = "METEOR-M2 4 LRPT"; in.passRegion = regionOfTx(meteorTx(137.9e6));
    return in;
}

int main() {
    std::printf("§1 the capture fit\n");
    for (double hz : { 137.1e6, 137.9e6 }) {
        const Region m = regionOfTx(meteorTx(hz));
        for (auto rc : { std::make_pair("RTL 2.4 MS/s", kRtl24), std::make_pair("RSP 2 MS/s", kRsp2) }) {
            const Fit f = fitCapture(rc.second, { m, orbcommRegion() });
            const bool holds = f.ok && f.centreHz - rc.second / 2 <= std::min(m.lo, kOrbLoHz) && f.centreHz + rc.second / 2 >= std::max(m.hi, kOrbHiHz);
            const bool dcClear = f.ok && (f.centreHz < m.lo - kDcGuardHz || f.centreHz > m.hi + kDcGuardHz);
            CHECK(holds && dcClear, "%s holds METEOR %.1f + ORBCOMM, DC clear of the Meteor signal (centre %.4f MHz, %s)", rc.first, hz / 1e6, f.centreHz / 1e6, f.why.c_str());
        }
        for (auto rc : { std::make_pair("HF+ 912 kS/s", kHfp912), std::make_pair("HF+ 768 kS/s", kHfp768) }) {
            const Fit f = fitCapture(rc.second, { m, orbcommRegion() });
            CHECK(!f.ok && f.why.find("too narrow for both") != std::string::npos, "%s cannot hold METEOR %.1f + ORBCOMM (%s)", rc.first, hz / 1e6, f.why.c_str());
        }
    }
    { const Fit f = fitCapture(kHfp912, { orbcommRegion() }); CHECK(f.ok && f.centreHz > kOrbLoHz && f.centreHz < kOrbHiHz, "an HF+ at 912 kS/s holds the ORBCOMM band alone, its DC inside (blanked by the watcher) — %.4f MHz", f.centreHz / 1e6); }
    { CHECK(std::fabs(orbcommAloneCentre(kHfp768) - 137.5e6) < 1, "an HF+ at 768 kS/s (narrower than the band) watches it from its middle"); }
    { const Fit f = fitCapture(kRtl24, { orbcommRegion() });
      CHECK(f.ok && (f.centreHz < kOrbLoHz || f.centreHz > kOrbHiHz), "an RTL holds the ORBCOMM band alone with its DC outside it (%.4f MHz)", f.centreHz / 1e6); }

    std::printf("§2 both together on a wide capture\n");
    for (bool orbFirst : { false, true }) {
        In in = base(kRtl24); in.orbcommFirst = orbFirst;
        const Out o = decide(in);
        CHECK(o.runPass && o.runOrbcomm && o.both && o.placeCentre, "RTL, %s first: both run together", orbFirst ? "ORBCOMM" : "Meteor");
    }
    {
        In in = base(kRsp2); in.loaded = true;
        const Out o = decide(in);
        CHECK(o.runPass && o.runOrbcomm, "a loaded box on a wide capture still runs both (never dropped for CPU)");
    }

    std::printf("§3 the priority on a narrow capture\n");
    {
        In in = base(kHfp912);
        const Out o = decide(in);
        CHECK(o.runPass && !o.runOrbcomm && !o.both, "HF+, Meteor first: the pass runs, ORBCOMM pauses");
        CHECK(o.orbcommWhyNot.find("paused for the METEOR-M2 4 LRPT pass") != std::string::npos, "…and says why (%s)", o.orbcommWhyNot.c_str());
        in.orbcommFirst = true;
        const Out p = decide(in);
        CHECK(!p.runPass && p.runOrbcomm, "HF+, ORBCOMM first: ORBCOMM is not interrupted for the pass");
        CHECK(p.state.find("pass not taken (ORBCOMM first") != std::string::npos, "…and says so (%s)", p.state.c_str());
    }

    std::printf("§4 a Meteor pass interrupts ORBCOMM and hands back at LOS\n");
    {
        // narrow (HF+), Meteor first: before AOS · during · after LOS
        In in = base(kHfp912);
        in.passUp = false; in.nextPassAos = in.now + 600;
        Out a = decide(in);
        in.passUp = true;
        Out b = decide(in);
        in.passUp = false; in.nextPassAos = in.now + 6000;
        Out c = decide(in);
        CHECK(a.runOrbcomm && !a.runPass, "before AOS: ORBCOMM monitoring");
        CHECK(b.runPass && !b.runOrbcomm, "AOS: the pass takes the radio");
        CHECK(c.runOrbcomm && !c.runPass, "LOS: back to ORBCOMM");
        // wide (RTL): ORBCOMM never stops
        In w = base(kRtl24); w.passUp = false;
        const Out wa = decide(w); w.passUp = true; const Out wb = decide(w); w.passUp = false; const Out wc = decide(w);
        CHECK(wa.runOrbcomm && wb.runOrbcomm && wb.runPass && wc.runOrbcomm, "on a wide capture ORBCOMM runs through the pass");
    }

    std::printf("§5 switched off, out of reach, nothing up, a listener, blocked\n");
    {
        In in = base(kRtl24); in.passUp = false;
        in.orbcommOn = false; CHECK(!decide(in).runOrbcomm, "ORBCOMM switched off: not run");
        in.orbcommOn = true; in.orbcommReachWhy = "This receiver cannot tune to it";
        Out o = decide(in);
        CHECK(!o.runOrbcomm && o.orbcommWhyNot == "This receiver cannot tune to it", "a radio that cannot reach 137 MHz: not run (%s)", o.orbcommWhyNot.c_str());
        in.orbcommReachWhy.clear(); in.orbcommUp = false;
        o = decide(in);
        CHECK(!o.runOrbcomm && o.orbcommWhyNot == "no ORBCOMM satellite above the horizon", "no bird up: not run");
        In l = base(kRtl24); l.listener = true;
        o = decide(l);
        CHECK(!o.runPass && !o.runOrbcomm && o.state == "a listener has the radio", "a listener: the radio is theirs");
        In bl = base(kRtl24); bl.blocked = "ADS-B is kept running on this radio when nobody is connected";
        o = decide(bl);
        CHECK(!o.runPass && !o.runOrbcomm && o.state == bl.blocked, "blocked: neither, with the reason");
        In mo = base(kRtl24); mo.orbcommOn = false;
        o = decide(mo);
        CHECK(o.runPass && !o.runOrbcomm && !o.placeCentre, "Meteor alone: the pass as before (its own placement)");
    }

    std::printf("§6 the admin lines\n");
    {
        In in = base(kRtl24); in.passUp = false; in.nextPassAos = in.now + 72 * 60 + 5;
        Out o = decide(in);
        CHECK(o.state == "ORBCOMM monitoring (sky plot) \xe2\x80\x94 no Meteor pass for 1 h 12 min", "%s", o.state.c_str());
        In n = base(kHfp912);
        o = decide(n);
        CHECK(o.state.rfind("METEOR-M2 4 LRPT pass \xe2\x80\x94 ORBCOMM paused (priority; this radio's capture is too narrow for both", 0) == 0, "%s", o.state.c_str());
        In w = base(kRtl24);
        o = decide(w);
        CHECK(o.state == "METEOR-M2 4 LRPT pass + ORBCOMM monitoring (sky plot) together \xe2\x80\x94 one capture holds both", "%s", o.state.c_str());
        In none = base(kRtl24); none.passUp = false; none.meteorOn = false; none.anyPasses = false;
        o = decide(none);
        CHECK(o.state == "ORBCOMM monitoring (sky plot) \xe2\x80\x94 no passes set up", "%s", o.state.c_str());
        CHECK(dur(30) == "under a minute" && dur(14 * 60) == "14 min" && dur(3 * 3600) == "3 h 0 min", "durations");
    }

    std::printf("§7 the IF filter holds both\n");
    for (double hz : { 137.1e6, 137.9e6 }) {
        const auto t = meteorTx(hz);
        const Fit f = fitCapture(kRtl24, { regionOfTx(t), orbcommRegion() });
        const std::vector<vibesatif::Region> passOnly{ vibesatif::regionOf(t) };
        const std::vector<vibesatif::Region> both{ vibesatif::regionOf(t), vibesatif::orbcommSkyRegion() };
        const double hPass = vibesatif::halfForPass(passOnly, f.centreHz), hBoth = vibesatif::halfForPass(both, f.centreHz);
        const bool holdsOrb = f.centreHz - hBoth <= kOrbLoHz && f.centreHz + hBoth >= kOrbHiHz;
        const Region m = regionOfTx(t);
        const bool holdsMeteor = f.centreHz - hBoth <= m.lo && f.centreHz + hBoth >= m.hi;
        const bool passCuts = f.centreHz - hPass > kOrbLoHz || f.centreHz + hPass < kOrbHiHz;
        CHECK(holdsOrb && holdsMeteor, "METEOR %.1f: the filter (±%.0f kHz round %.4f MHz) holds the ORBCOMM band and the Meteor signal", hz / 1e6, hBoth / 1e3, f.centreHz / 1e6);
        CHECK(passCuts, "METEOR %.1f: the pass's region alone (±%.0f kHz) would have cut the band", hz / 1e6, hPass / 1e3);
        const int rung = vibesatif::narrowestRung(2 * hBoth, vibesatif::rtlRungs(), 2.4e6);
        CHECK(rung >= 2 * hBoth || rung == 0, "an RTL rung that holds it (%d kHz)", rung / 1000);
    }

    std::printf("§8 the pages' promise = the server's rule\n");
    {
        double need = 0;
        for (double hz : { 137.1e6, 137.9e6 }) need = std::max(need, bothNeedHz(regionOfTx(meteorTx(hz))));
        const double minRate = bothMinRate(need, kFlat);
        std::FILE* f = std::fopen("android/app/src/main/cpp/vibe_setup_page.h", "rb");
        std::string page; if (f) { char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) page.append(b, n); std::fclose(f); }
        const size_t at = page.find("const SAT_BOTH_MIN_RATE = ");
        const double onPage = at == std::string::npos ? -1 : std::atof(page.c_str() + at + 26);
        CHECK(onPage == minRate, "the setup page's SAT_BOTH_MIN_RATE (%.0f) = bothMinRate (%.0f; %.0f kHz needed)", onPage, minRate, need / 1e3);
        // and the rule holds at that rate on a radio that crops nothing (an RTL), fails just under it
        for (double hz : { 137.1e6, 137.9e6 }) {
            CHECK(fitCapture(minRate * kFlat, { regionOfTx(meteorTx(hz)), orbcommRegion() }).ok, "at %.0f kS/s both fit (METEOR %.1f)", minRate / 1e3, hz / 1e6);
        }
        CHECK(!fitCapture((minRate - 30000) * kFlat, { regionOfTx(meteorTx(137.9e6)), orbcommRegion() }).ok, "30 kS/s under it they do not");
        std::printf("   per radio: RTL 2.4 MS/s %s · RTL 1.024 MS/s %s · RSP 2 MS/s %s · Airspy R2 2.5 MS/s %s · HF+ 912 kS/s %s · HF+ 768 kS/s %s\n",
                    fitCapture(2.4e6 * kFlat, { regionOfTx(meteorTx(137.9e6)), orbcommRegion() }).ok ? "both" : "priority",
                    fitCapture(1.024e6 * kFlat, { regionOfTx(meteorTx(137.9e6)), orbcommRegion() }).ok ? "both" : "priority",
                    fitCapture(2.0e6 * kFlat, { regionOfTx(meteorTx(137.9e6)), orbcommRegion() }).ok ? "both" : "priority",
                    fitCapture(2.5e6 * kFlat, { regionOfTx(meteorTx(137.9e6)), orbcommRegion() }).ok ? "both" : "priority",
                    fitCapture(kHfp912, { regionOfTx(meteorTx(137.9e6)), orbcommRegion() }).ok ? "both" : "priority",
                    fitCapture(kHfp768, { regionOfTx(meteorTx(137.9e6)), orbcommRegion() }).ok ? "both" : "priority");
    }

    std::printf("test-sat-pass-sched: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
