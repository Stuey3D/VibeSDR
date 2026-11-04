// ★★★ THE BENCHMARK TAILORED TO THE RADIOS HERE, AND THE USRP's RATE GATE (V12, 2026-11-03) — vibe_benchmark_hw.h and
// vibe_usrp_caps.h ratesFor, held to fake radio sets. No radio, no DSP: the decisions only.
//   Stuart: "no point testing DAB if the user only has an AirSpy HF for instance, and no super wide sample rates for
//   users with only an RTL".
//   build: c++ -std=c++17 -I android/app/src/main/cpp -o /tmp/t vibeserver/test-bench-hw.cpp && /tmp/t
#include "vibe_benchmark_hw.h"
#include <cstdio>

using namespace vibe;
using benchhw::Hw;
static int fails = 0, passes = 0;
static void ok(bool c, const char* what) {
    if (c) ++passes; else { ++fails; std::printf("FAIL  %s\n", what); }
}
static bool runs(const Hw& hw, const char* id) { return benchhw::gateRow(hw, id).run; }
static bool hasRate(const std::vector<uint32_t>& v, uint32_t r) { for (uint32_t x : v) if (x == r) return true; return false; }
static bool hasWide(const Hw& hw, double r) { for (double x : benchhw::wideRates(hw)) if (std::fabs(x - r) < 1) return true; return false; }

int main() {
    // ── no radio configured: today's full run ──
    {
        Hw hw;
        for (const char* id : { "wfm_2400", "rdsx_2048", "dab", "adsb", "dec_ais", "dec_rtty", "dec_ndb", "dec_navtex", "lk_wfm", "dec_lora" })
            ok(runs(hw, id), "no radio configured: every row runs");
        ok(benchhw::wideRates(hw).empty(), "no radio configured: no wide rows (the run is what it was)");
    }
    // ── an Airspy HF+ alone ──
    {
        Hw hw; hw.radios.push_back(benchhw::radioFor("airspyhf"));
        ok(!runs(hw, "dab"), "HF+ only: no DAB (912 kS/s cannot carry a 1.536 MHz ensemble)");
        ok(benchhw::gateRow(hw, "dab").why.find("Band III") != std::string::npos, "HF+ only: DAB's reason names Band III");
        ok(!runs(hw, "adsb") && !runs(hw, "adsb_2400"), "HF+ only: no ADS-B");
        ok(!runs(hw, "dec_ais"), "HF+ only: no AIS (vsAisReach leaves the HF+ out)");
        ok(!benchhw::rateOffered(hw, 2400000, false) && !benchhw::rateOffered(hw, 2048000, false) && !benchhw::rateOffered(hw, 1024000, false),
           "HF+ only: none of the WFM rate rows (2.4 / 2.048 / 1.024 MS/s) is offered");
        ok(benchhw::wideRates(hw).empty(), "HF+ only: no wide rows");
        ok(runs(hw, "dec_rtty") && runs(hw, "dec_navtex") && runs(hw, "dec_ndb") && runs(hw, "dec_wefax") && runs(hw, "dec_time"),
           "HF+ only: the HF decoders run");
        ok(benchhw::nearestOffered(hw, 2048000, benchhw::Need::Wfm) == 912000, "HF+ only: a feature row runs at its 912 kS/s");
        ok(!runs(hw, "dec_ism") && !runs(hw, "dec_lora"), "HF+ only: no ISM / LoRa (it tops out near 260 MHz)");
    }
    // ── an RTL dongle alone ──
    {
        Hw hw; hw.radios.push_back(benchhw::radioFor("rtlsdr"));
        ok(benchhw::wideRates(hw).empty(), "RTL only: no wide rates");
        ok(runs(hw, "dab") && runs(hw, "adsb") && runs(hw, "dec_ais") && runs(hw, "rdsx_2048"), "RTL only: DAB, ADS-B, AIS, WFM run");
        ok(benchhw::rateOffered(hw, 2400000, false) && benchhw::rateOffered(hw, 2048000, false) && benchhw::rateOffered(hw, 1024000, false),
           "RTL only: the three WFM rate rows run");
        ok(!runs(hw, "dec_ndb") && !runs(hw, "dec_navtex"), "RTL without direct sampling: no NDB / NAVTEX (vsNdbReach)");
        ok(runs(hw, "dec_rtty"), "RTL: HF decoders run (its coverage table reaches HF — a V4, or direct sampling)");
        Hw ds; ds.radios.push_back(benchhw::radioFor("rtl", 0, 0, 0, true));
        ok(runs(ds, "dec_ndb") && runs(ds, "dec_navtex"), "RTL with direct sampling: NDB and NAVTEX run");
    }
    // ── a USRP B210 on USB 3, locked range ──
    {
        Hw hw; hw.radios.push_back(benchhw::radioFor("usrp", 0, 0, 0, false, 3, true));
        ok(hasWide(hw, 8e6) && hasWide(hw, 16e6) && hasWide(hw, 20e6) && hasWide(hw, 28e6) && hasWide(hw, 30.72e6) && hasWide(hw, 32e6),
           "USRP USB 3: wide rows 8 … 32 MS/s, 30.72 among them");
        ok(benchhw::anyLocked(hw), "USRP locked: judged by the locked-range rows");
        ok(!runs(hw, "dec_rtty") && !runs(hw, "dec_ndb") && !runs(hw, "dec_wefax"), "USRP: no HF decoders (50 MHz floor)");
        ok(runs(hw, "dab") && runs(hw, "adsb") && runs(hw, "dec_ais") && runs(hw, "lk_wfm"), "USRP: DAB, ADS-B, AIS, WFM run");
        // ★ V12 2026-11-04 (v12-dab-wide): the USRP offers 2.048 MS/s now — DAB's own rate, so a block change never re-rates it.
        ok(benchhw::rateOffered(hw, 2400000, false) && benchhw::rateOffered(hw, 2048000, false), "USRP: wfm_2400 and wfm_2048 both run (2.048 offered since the DAB work)");
        Hw u2; u2.radios.push_back(benchhw::radioFor("usrp", 0, 0, 0, false, 2));
        ok(!hasWide(u2, 20e6) && hasWide(u2, 8e6), "USRP on USB 2: 8 MS/s measured, nothing gated above it");
        Hw u0; u0.radios.push_back(benchhw::radioFor("usrp"));
        ok(hasWide(u0, 16e6) && hasWide(u0, 30.72e6), "USRP, link not known yet: measured at the USB 3 rates");
    }
    // ── mixed: an HF+ and an RTL — each row runs if ANY radio can use it ──
    {
        Hw hw; hw.radios.push_back(benchhw::radioFor("airspyhf")); hw.radios.push_back(benchhw::radioFor("rtl"));
        ok(runs(hw, "dab") && runs(hw, "dec_ndb") && benchhw::wideRates(hw).empty(), "HF+ + RTL: DAB (the RTL), NDB (the HF+), no wide");
    }
    // ── an Airspy R2 behind an up-converter: HF ──
    {
        Hw bare; bare.radios.push_back(benchhw::radioFor("airspy"));
        ok(!runs(bare, "dec_rtty"), "Airspy R2 alone: no HF (24 MHz floor)");
        ok(hasWide(bare, 10e6) && !hasWide(bare, 20e6), "Airspy R2: its own 10 MS/s is measured, no gated rates");
        Hw conv; conv.radios.push_back(benchhw::radioFor("airspy", -120e6, 0, 30e6));
        ok(runs(conv, "dec_rtty") && runs(conv, "dec_ndb"), "Airspy R2 + a 120 MHz up-converter: HF and NDB run");
        ok(!runs(conv, "dab"), "Airspy R2 + converter (input 0–30 MHz): no Band III");
    }
    // ── an RSP: rates 2–8 MS/s ──
    {
        Hw hw; hw.radios.push_back(benchhw::radioFor("sdrplay"));
        ok(benchhw::rateOffered(hw, 2048000, false) && !benchhw::rateOffered(hw, 2400000, false), "RSP: wfm_2048 runs, wfm_2400 does not");
        ok(benchhw::wideRates(hw).size() == 1 && hasWide(hw, 8e6), "RSP: 8 MS/s is its one wide row");
    }
    // ── the USRP's rate gate ──
    {
        using usrpcaps::ratesFor;
        ok(!hasRate(ratesFor(3), 20000000) && hasRate(ratesFor(3), 16000000), "gate shut (never measured): USB 3 stops at 16 MS/s");
        const auto g28 = ratesFor(3, 28e6);
        ok(hasRate(g28, 20000000) && hasRate(g28, 25000000) && hasRate(g28, 28000000) && !hasRate(g28, 30720000),
           "verdict 28 MS/s: 20, 25, 28 offered, 30.72 not");
        const auto g32 = ratesFor(3, 32e6);
        ok(hasRate(g32, 30720000) && hasRate(g32, 32000000), "verdict 32 MS/s: 30.72 and 32 offered");
        ok(!hasRate(ratesFor(2, 32e6), 10000000) && !hasRate(ratesFor(2, 32e6), 20000000), "USB 2: no wide rate whatever the verdict");
        ok(!hasRate(ratesFor(0, 32e6), 20000000), "link unknown: treated as USB 2");
        const auto keep = ratesFor(3, 0, 30.72e6);
        ok(hasRate(keep, 30720000) && !hasRate(keep, 28000000), "a rate in use stays offered (default, never hide)");
        usrpcaps::RateGate g; g.lockedHz = 30.72e6; g.fullHz = 16e6;
        g.locked = true;  ok(usrpcaps::gateHzFor(g) == 30.72e6, "a locked-range radio is judged by the locked-range figure");
        g.locked = false; ok(usrpcaps::gateHzFor(g) == 16e6, "a full radio is judged by the full-radio figure");
        double l = -1, f = -1;
        ok(benchhw::gateFromBenchJson("{\"v\":1,\"rows\":[],\"capture\":{\"lockedMaxHz\":30720000,\"fullMaxHz\":28000000,\"rates\":[]}}", l, f)
           && l == 30720000 && f == 28000000, "the verdict reads back from a saved benchmark");
        ok(!benchhw::gateFromBenchJson("{\"v\":1,\"rows\":[]}", l, f) && l == 0 && f == 0, "no capture block: no verdict, gate shut");
    }
    // ── Stuart's target: a locked range of 82–108 MHz ──
    {
        // ★ A locked radio tunes lockFreq ± lockRate / 2 (vsTunableRanges). Centred at 95 MHz:
        const double c = 95e6;
        ok(c - 30.72e6 / 2 <= 82e6 && c + 30.72e6 / 2 >= 108e6, "82–108 MHz fits a 30.72 MS/s lock centred on 95 MHz");
        ok(c - 28e6 / 2 <= 82e6 && c + 28e6 / 2 >= 108e6, "…and a 28 MS/s one (with 1 MHz either side)");
        ok(!(c - 25e6 / 2 <= 82e6), "…but not 25 MS/s");
    }
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
