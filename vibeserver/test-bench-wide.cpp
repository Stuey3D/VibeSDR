// ★★★ THE QUICK / WIDE BENCHMARK'S RULES (V12, 2026-11-18) — vibe_bench_profiles.h, held to made-up figures. No radio, no
// DSP: which radios get the WIDE BENCHMARK, the rate each profile is measured at, how many Band III blocks a rate holds, the
// users maths (the cores, the RAM, the hottest thread), the USB bus verdict, and benchmark.json's two halves kept apart.
//   Stuart: "the cap should be based on the hardware running the server, so build it into the benchmark".
//   build: c++ -std=c++17 -I android/app/src/main/cpp -o /tmp/t vibeserver/test-bench-wide.cpp && /tmp/t
#include "vibe_bench_profiles.h"
#include <cstdio>

using namespace vibe;
using benchhw::Hw;
using benchhw::Need;
static int fails = 0, passes = 0;
static void ok(bool c, const char* what) {
    if (c) ++passes; else { ++fails; std::printf("FAIL  %s\n", what); }
}
static bool near(double a, double b) { return std::fabs(a - b) < 1.0; }
static bool hasRate(const std::vector<double>& v, double r) { for (double x : v) if (near(x, r)) return true; return false; }

int main() {
    // ── which radios get the WIDE BENCHMARK ──
    ok(benchprof::wideCapable("usrp") && benchprof::wideCapable("hackrf") && benchprof::wideCapable("sdrplay")
       && benchprof::wideCapable("airspy"), "wide: USRP, HackRF, SDRplay, Airspy R2/Mini");
    ok(!benchprof::wideCapable("rtl") && !benchprof::wideCapable("rtlsdr") && !benchprof::wideCapable("airspyhf")
       && !benchprof::wideCapable("icom") && !benchprof::wideCapable(""), "not wide: RTL, Airspy HF+, Icom, nothing");

    // ── each driver's own rates (the list the WIDE BENCHMARK offers before the radio has been opened) ──
    ok(hasRate(benchhw::offlineRates("sdrplay"), 8e6) && !hasRate(benchhw::offlineRates("sdrplay"), 10e6), "RSP: up to 8 MS/s");
    ok(hasRate(benchhw::offlineRates("hackrf"), 10e6), "HackRF: 10 MS/s offered");
    ok(hasRate(benchhw::offlineRates("airspy"), 10e6) && hasRate(benchhw::offlineRates("airspy"), 6e6), "Airspy R2/Mini: 6 and 10 MS/s");
    { auto u3 = benchhw::offlineRates("usrp", 3), u2 = benchhw::offlineRates("usrp", 2);
      auto c = benchhw::wideCandidates("usrp", 3);
      ok(hasRate(u3, 16e6) && !hasRate(u2, 16e6) && hasRate(u2, 8e6), "USRP: 16 MS/s on USB 3, 8 on USB 2");
      ok(hasRate(c, 56e6) && hasRate(c, 30.72e6) && benchhw::wideCandidates("usrp", 2).empty(),
         "USRP: 30.72 … 56 MS/s are the gate's candidates on USB 3, none on USB 2"); }

    // ── the QUICK BENCHMARK's profile rate ──
    { Hw none;
      ok(near(benchprof::profileRate(none, Need::None), 2048000) && near(benchprof::profileRate(none, Need::Wfm), 2048000)
         && near(benchprof::profileRate(none, Need::Dab), 2048000), "no radio configured: every profile at 2.048 MS/s"); }
    { Hw rtl; rtl.radios.push_back(benchhw::radioFor("rtlsdr"));
      ok(near(benchprof::profileRate(rtl, Need::None), 2400000) && near(benchprof::profileRate(rtl, Need::Wfm), 2400000)
         && near(benchprof::profileRate(rtl, Need::Dab), 2400000), "RTL: every profile at its 2.4 MS/s (a Pi 2's run stays short)"); }
    { Hw hf; hf.radios.push_back(benchhw::radioFor("airspyhf"));
      ok(near(benchprof::profileRate(hf, Need::None), 912000), "HF+: the narrow profile at its 912 kS/s");
      ok(near(benchprof::profileRate(hf, Need::Wfm), 912000), "HF+: FM at 912 kS/s (it reaches 60–260 MHz)");
      ok(benchprof::profileRate(hf, Need::Dab) == 0, "HF+: no DAB line (912 kS/s cannot hold an ensemble)"); }
    { Hw rsp; rsp.radios.push_back(benchhw::radioFor("sdrplay"));
      ok(near(benchprof::profileRate(rsp, Need::Wfm), 8e6) && near(benchprof::profileRate(rsp, Need::Dab), 8e6), "RSP: 8 MS/s"); }
    { Hw a; a.radios.push_back(benchhw::radioFor("airspy"));
      ok(near(benchprof::profileRate(a, Need::None), 6e6), "Airspy R2: 6 MS/s (its 10 is past the quick run's 8)"); }
    { Hw u; u.radios.push_back(benchhw::radioFor("usrp", 0, 0, 0, false, 3, true));
      ok(near(benchprof::profileRate(u, Need::Wfm), 8e6), "USRP USB 3: 8 MS/s (the wider rates are the WIDE BENCHMARK's)"); }
    { Hw mix; mix.radios.push_back(benchhw::radioFor("airspyhf")); mix.radios.push_back(benchhw::radioFor("rtl"));
      ok(near(benchprof::profileRate(mix, Need::Wfm), 2400000) && near(benchprof::profileRate(mix, Need::None), 2400000),
         "HF+ and RTL: the RTL's 2.4 MS/s"); }
    { Hw ic; ic.radios.push_back(benchhw::radioFor("icom"));
      ok(benchprof::profileRate(ic, Need::None) == 0, "an Icom alone: no profile (no IQ capture)"); }

    // ── Band III blocks a rate holds ──
    ok(benchprof::dabBlocksAt(1.0e6) == 0 && benchprof::dabBlocksAt(912000) == 0, "below 2.048 MS/s: no DAB block");
    ok(benchprof::dabBlocksAt(2048000) == 1 && benchprof::dabBlocksAt(2400000) == 1 && benchprof::dabBlocksAt(3.2e6) == 1,
       "2.048 – 3.2 MS/s: the ordinary DAB mode, one block");
    {
        const double ladder[] = { 4e6, 5e6, 6e6, 8e6, 10e6, 16e6, 20e6, 30.72e6, 40e6, 56e6 };
        int prev = 1; bool mono = true;
        for (double r : ladder) { const int b = benchprof::dabBlocksAt(r); if (b < prev) mono = false; prev = b; }
        ok(mono, "more rate never holds fewer blocks");
        const int b8 = benchprof::dabBlocksAt(8e6), b56 = benchprof::dabBlocksAt(56e6);
        std::printf("   blocks: 4 MS/s %d, 8 MS/s %d, 16 MS/s %d, 56 MS/s %d\n", benchprof::dabBlocksAt(4e6), b8, benchprof::dabBlocksAt(16e6), b56);
        ok(benchprof::dabBlocksAt(4e6) == 2, "4 MS/s: two blocks (vibe_dab_wide.h's floor)");
        ok(b8 >= 3 && b8 <= 4, "8 MS/s: three or four blocks (the RSP1A round 215 MHz holds 10C, 10D, 11A)");
        ok(b56 >= 20, "56 MS/s: most of Band III");
        // ★ The figure is the server's own rule: a centre that holds that many must exist.
        bool found = false;
        for (size_t i = 0; i < vibedab::kBandIIICount && !found; ++i)
            for (double c = vibedab::kBandIII[i].centreHz - 4e6; c <= vibedab::kBandIII[i].centreHz + 4e6 && !found; c += 8000.0)
                if ((int)vibedab::wide::blocksIn(c, 8e6).size() == b8) found = true;
        ok(found, "8 MS/s: the count is one vibedab::wide::blocksIn gives at a real centre");
    }

    // ── the users maths ──
    using benchprof::ProfileCost;
    {
        ProfileCost c; c.perPct = 5; c.perHottestPct = 4; c.fixedPct = 20; c.fixedHottestPct = 12; c.perKB = 2000; c.fixedKB = 20000;
        auto u = benchprof::profileUsers(c, 4, 4.0 * 1024 * 1024);   // 4 cores, 4 GB free
        ok(u.cpuN == 38 && u.n == 38 && u.by == "cpu", "CPU bound: (70×4 − 70 − 20) / 5 = 38");
        ok(u.ramN == (int)std::floor((4.0 * 1024 * 1024 - 65536 - 20000) / 2000), "…the RAM bound beside it (free − 64 MB floor − buffers)");
        auto r = benchprof::profileUsers(c, 4, 65536 + 20000 + 10 * 2000 + 500);
        ok(r.n == 10 && r.by == "ram" && r.cpuN == 38, "RAM bound: 10 fit in what is left above the floor — said as ram");
        auto t = benchprof::profileUsers(c, 4, 60000);
        ok(t.n == 0 && t.ramN == 0 && t.by == "ram", "less free than the floor: none, by RAM (NO OOM)");
        auto n = benchprof::profileUsers(c, 4, -1);
        ok(n.n == 38 && n.ramN == -1 && n.by == "cpu", "RAM not readable: the cores alone");
        ProfileCost h = c; h.fixedHottestPct = 71;
        ok(benchprof::profileUsers(h, 16, -1).n == 0 && benchprof::profileUsers(h, 16, -1).by == "capture",
           "HOTTEST THREAD: the front end past 70 % of a core = none, however many cores");
        ProfileCost d = c; d.dropped = 1;
        ok(benchprof::profileUsers(d, 16, -1).n == 0 && benchprof::profileUsers(d, 16, -1).by == "capture", "the capture dropped IQ = none");
        ProfileCost l = c; l.perHottestPct = 75;
        ok(benchprof::profileUsers(l, 16, -1).n == 0 && benchprof::profileUsers(l, 16, -1).by == "cpu", "one listener's thread past 70 % = none");
        ProfileCost ld = c; ld.listenerDropped = 3;
        ok(benchprof::profileUsers(ld, 16, -1).n == 0, "a listener that dropped blocks = none");
        ProfileCost z; ok(benchprof::profileUsers(z, 4, -1).by == "none" && benchprof::profileUsers(z, 4, -1).n == 0, "not measured = none");
        ProfileCost one = c; one.fixedPct = 0;
        ok(benchprof::profileUsers(one, 1, -1).n == 0, "one core: it is the network's and the audio clock's — none");
        ok(benchprof::profileUsers(c, 16, -1).n == (int)std::floor((70.0 * 16 - 70 - 20) / 5), "16 cores: never capped at 20");
    }
    // ── DAB's cost ──
    {
        benchprof::CaptureCost cap; cap.rate = 8e6; cap.intakePct = 3; cap.frontPct = 10; cap.fixedKB = 30000;
        auto one = benchprof::dabCost(4, 20, 30, 6, nullptr, 2048000, 1, cap, 5000);
        ok(near(one.fixedPct, 20) && near(one.fixedHottestPct, 30) && one.perPct == 4 && one.fixedKB == 0,
           "one block: its own front end only — no cutting, no channeliser");
        auto three = benchprof::dabCost(4, 20, 30, 6, nullptr, 8e6, 3, cap, 5000);
        ok(near(three.fixedPct, 3 * 20 + 3 * 6 + 3 + 10), "three blocks: 3 fronts + 3 DDCs + the capture's intake and front end");
        ok(near(three.fixedHottestPct, 30) && near(three.fixedKB, 30000), "…hottest = a multiplex's own thread; the capture's buffers");
        vibedab::wide::BankCost bank; bank.rates = { 8e6, 16e6 }; bank.fwd = { 5, 10 }; bank.slice = { 0.5, 1 };
        auto b = benchprof::dabCost(4, 20, 30, 6, &bank, 8e6, 3, cap, 5000);
        ok(near(b.fixedPct, 3 * 20 + 5 + 1.5 + 13), "the filter bank where it is cheaper (cutCostPct — the server's own choice)");
        auto hot = benchprof::dabCost(4, 20, 30, 30, nullptr, 56e6, 20, cap, 5000);
        ok(hot.fixedHottestPct > 70 && benchprof::profileUsers(hot, 32, -1).by == "capture",
           "56 MS/s by DDCs: one DDC thread past 70 % = none, said as the capture");
        auto unk = benchprof::dabCost(4, 20, 30, 6, nullptr, 2048000, 1, cap, -1);
        ok(near(unk.perKB, benchprof::kDabStationKBFallback), "station memory not measured: the top of the measured range");
        ok(!(benchprof::dabCost(-1, 20, 30, 6, nullptr, 8e6, 3, cap, 5000).perPct > 0), "no station figure: no DAB line");
        ok(!(benchprof::dabCost(4, 20, 30, -1, nullptr, 8e6, 3, cap, 5000).perPct > 0), "several blocks, no cutting measured: no DAB line");
    }

    // ── the USB bus verdict ──
    {
        benchprof::UsbCounters a, b;
        a.t = 100; b.t = 120; a.samples = 1000; b.samples = 1000 + (unsigned long long)(56e6 * 20);
        auto v = benchprof::usbVerdict(a, b, 56e6);
        ok(v.ok && near(v.deliveredPct * 1000, 100000) && v.why.empty(), "every sample, no hole: kept up");
        b.samples = 1000 + (unsigned long long)(56e6 * 20 * 0.9995);
        ok(benchprof::usbVerdict(a, b, 56e6).ok, "99.95 % delivered: kept up (≥ 99.9 %)");
        b.samples = 1000 + (unsigned long long)(56e6 * 20 * 0.998);
        v = benchprof::usbVerdict(a, b, 56e6);
        ok(!v.ok && v.why.find("99.80 %") != std::string::npos, "99.8 % delivered: could not keep up, and says how much arrived");
        b.samples = 1000 + (unsigned long long)(56e6 * 20); b.usbEvents = 2; b.usbLost = 65536;
        v = benchprof::usbVerdict(a, b, 56e6);
        ok(!v.ok && v.usbEvents == 2 && v.why.find("2 USB drops") != std::string::npos, "the radio reported drops: could not keep up");
        b.usbEvents = 0; b.usbLost = 0; a.iqDrops = 5; b.iqDrops = 6;
        v = benchprof::usbVerdict(a, b, 56e6);
        ok(!v.ok && v.iqDrops == 1 && v.why.find("1 IQ buffer dropped") != std::string::npos, "our IQ queue dropped one: could not keep up");
        b.iqDrops = 5; b.gaps = 1;
        ok(!benchprof::usbVerdict(a, b, 56e6).ok, "a gap reached the DSP: could not keep up");
        b.gaps = 0;
        ok(benchprof::usbVerdict(a, b, 56e6).ok, "drops before the settle (the start counters) do not count");
        b.t = a.t;
        ok(!benchprof::usbVerdict(a, b, 56e6).ok && benchprof::usbVerdict(a, b, 56e6).why == "not measured", "no time measured: not a verdict");
        b.t = 120;
        const std::string j = benchprof::usbJson(benchprof::usbVerdict(a, b, 56e6), 4);
        ok(j.find("\"ok\":true") != std::string::npos && j.find("\"deliveredPct\":100.000") != std::string::npos
           && j.find("\"settleSec\":4.0") != std::string::npos, "the verdict's JSON");
    }

    // ── benchmark.json: the quick half and the wide half kept apart ──
    {
        const std::string quick1 = "{\"v\":1,\"at\":5,\"rows\":[{\"id\":\"dab\",\"label\":\"DAB+ \\\"x\\\" {}\",\"pct\":31.5}],\"capture\":{\"lockedMaxHz\":30720000}}";
        ok(benchprof::jsonTop(quick1, "at") == "5" && benchprof::jsonTop(quick1, "capture") == "{\"lockedMaxHz\":30720000}",
           "a top-level value, past a string holding quotes and braces");
        ok(benchprof::jsonTop(quick1, "wide").empty() && benchprof::jsonTop(quick1, "lockedMaxHz").empty(), "absent / not top-level: empty");
        const std::string e1 = "{\"v\":1,\"rate\":56000000,\"usb\":{\"ok\":true}}", e2 = "{\"v\":1,\"rate\":8000000}";
        std::string saved = benchprof::benchWithWide(quick1, "30C1234", e1);
        ok(benchprof::jsonTop(saved, "rows") == benchprof::jsonTop(quick1, "rows") && benchprof::jsonTop(saved, "at") == "5",
           "wide keeps the quick result whole");
        ok(benchprof::jsonTop(benchprof::jsonTop(saved, "wide"), "30C1234") == e1, "…and holds the radio's entry");
        saved = benchprof::benchWithWide(saved, "RSP99", e2);
        saved = benchprof::benchWithWide(saved, "30C1234", e2);
        const std::string w = benchprof::jsonTop(saved, "wide");
        ok(benchprof::jsonTop(w, "30C1234") == e2 && benchprof::jsonTop(w, "RSP99") == e2, "a second radio beside it; a re-run replaces only its own");
        const std::string quick2 = "{\"v\":1,\"at\":9,\"rows\":[]}";
        const std::string kept = benchprof::benchKeepWide(quick2, saved);
        ok(benchprof::jsonTop(kept, "at") == "9" && benchprof::jsonTop(kept, "wide") == w, "a new quick run carries the wide block across");
        ok(benchprof::benchKeepWide(quick2, "") == quick2 && benchprof::benchKeepWide(quick2, quick1) == quick2, "…and adds nothing where there was none");
        const std::string fresh = benchprof::benchWithWide("", "radio0", e2);
        ok(benchprof::jsonTop(fresh, "v") == "0" && benchprof::jsonTop(benchprof::jsonTop(fresh, "wide"), "radio0") == e2,
           "no saved file: a {\"v\":0} holder with the wide entry");
        ok(benchprof::jsonWithTop("{}", "a", "1") == "{\"a\":1}" && benchprof::jsonWithTop("{\"a\":1}", "a", "[2]") == "{\"a\":[2]}",
           "set: into an empty object, and replacing");
    }
    // ── DAB's inputs from a saved quick result ──
    {
        const std::string q = "{\"v\":1,\"rows\":[{\"id\":\"dab_scan\",\"pct\":9},{\"id\":\"dab\",\"label\":\"DAB+\",\"rate\":2048000,\"pct\":31.5,\"threads\":{\"a\":1}}],"
                              "\"dabStations\":{\"n\":4,\"perStationPct\":3.25,\"frontPct\":20,\"memKB\":4100,\"muxFrontPct\":18.5,\"ddc8Pct\":6.0,\"cores\":4,"
                              "\"bank\":{\"rates\":[8000000,16000000],\"fwd\":[2.1,4.4],\"slice\":[0.300,0.610]}}}";
        const auto d = benchprof::dabInputsFromJson(q);
        ok(d.ok() && near(d.perStation * 100, 325) && near(d.muxFront * 10, 185) && near(d.ddc8, 6) && near(d.stationKB, 4100),
           "the dabStations figures");
        ok(near(d.muxHottest * 10, 315), "the dab row's hottest thread (not dab_scan's)");
        ok(d.bank.ok() && d.bank.rates.size() == 2 && near(d.bank.fwd[1] * 10, 44) && near(d.bank.slice[0] * 1000, 300), "the filter bank's costs");
        const auto old = benchprof::dabInputsFromJson("{\"v\":1,\"dabStations\":{\"n\":3,\"perStationPct\":3.0,\"frontPct\":20}}");
        ok(!old.ok(), "a result from before the multiplex row: not usable (run the quick benchmark again)");
        ok(!benchprof::dabInputsFromJson("").ok() && !benchprof::dabInputsFromJson("{\"v\":0}").ok(), "no quick result: not usable");
    }
    // ── one profile's JSON ──
    {
        ProfileCost c; c.perPct = 5; c.perHottestPct = 4; c.fixedPct = 20; c.fixedHottestPct = 12; c.perKB = 2000;
        const auto u = benchprof::profileUsers(c, 4, -1);
        const std::string j = benchprof::profileJson(u, c, 8e6, ",\"mode\":\"nfm\"");
        ok(benchprof::jsonTop(j, "n") == "38" && benchprof::jsonTop(j, "rate") == "8000000" && benchprof::jsonTop(j, "by") == "\"cpu\""
           && benchprof::jsonTop(j, "mode") == "\"nfm\"" && benchprof::jsonTop(j, "ramN") == "-1", "n, rate, by, the extra keys");
    }

    std::printf("test-bench-wide: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
