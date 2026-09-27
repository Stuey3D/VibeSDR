// test-health-snail.cpp — the throttle snail (vibe_health.h), driven through a FAKE sysfs tree.
//
// ★ Stuart's rule (2026-09-27): the snail shows when the machine is fully loaded but running below its
//   all-core maximum; fire for heat, lightning for power, plain when the cause cannot be read. On x86 the
//   all-core maximum is CALIBRATED once at setup (a boost chip cannot hold cpuinfo_max_freq with every core
//   busy); on ARM cpuinfo_max_freq IS the all-core maximum.
//
// Built twice: once as this machine's arch (the ARM path on Apple Silicon) and once with
// VIBE_HEALTH_TEST_BOOST, which forces the x86 path — see CMakeLists.
#include "vibe_health.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static int fails = 0;
static void ok(bool c, const char* what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what); if (!c) fails++; }

static std::string g_root;
static void put(const std::string& rel, long v) {
    fs::create_directories(fs::path(g_root + rel).parent_path());
    std::ofstream(g_root + rel) << v << "\n";
}
static void cores(int n, long maxKHz) {
    for (int i = 0; i < n; i++) put("/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq", maxKHz);
}
static std::map<int, long> clocks(int n, long kHz) { std::map<int, long> m; for (int i = 0; i < n; i++) m[i] = kHz; return m; }
/** Tick `secs` times and report whether the snail ended up showing. */
static bool run(vibehealth::Sampler& s, int secs, bool loaded, long kHz, int n = 4) {
    for (int i = 0; i < secs; i++) s.snailTick(loaded, clocks(n, kHz));
    return s.slowNow;
}

int main() {
    g_root = (fs::temp_directory_path() / ("vibe-snail-" + std::to_string((long)getpid()))).string();
    fs::remove_all(g_root);
    vibehealth::detail::sysRoot() = g_root;
    vibehealth::detail::peakFile() = g_root + "/cpu-allcore-peak.txt";
    std::printf("-- snail, %s path --\n", vibehealth::detail::kBoostClocks ? "x86 (boost, calibrated)" : "ARM (cpuinfo_max)");

    if (!vibehealth::detail::kBoostClocks) {
        cores(4, 2400000);
        vibehealth::Sampler s;
        ok(!run(s, 10, true, 2400000), "full clock under load: no snail");
        ok(!run(s, 4, true, 1500000), "slowed under load for 4 s: not yet");
        ok(run(s, 1, true, 1500000), "slowed under load for 5 s: SNAIL");
        ok(run(s, 4, true, 2400000), "recovered for 4 s: still showing (hysteresis)");
        ok(!run(s, 1, true, 2400000), "recovered for 5 s: cleared");
        ok(!run(s, 20, false, 600000), "idle at minimum clock: never a snail");
        ok(!run(s, 20, true, 2300000), "96 % of max under load: inside the 7 % margin, no snail");
        // Cause: the CPU's own thermal counter rising.
        put("/devices/system/cpu/cpu0/thermal_throttle/core_throttle_count", 3);
        put("/devices/system/cpu/cpu0/thermal_throttle/package_throttle_count", 0);
        s.snailTick(true, clocks(4, 1500000));
        put("/devices/system/cpu/cpu0/thermal_throttle/core_throttle_count", 7);
        s.snailTick(true, clocks(4, 1500000));
        ok(s.thermalHold > 0, "thermal counter rose: cause = heat (fire)");
        ok(s.powerHold == 0, "no power counter on this kernel: power NOT inferred");
        // ★ Idle cores sit at their minimum under a dynamic governor — only BUSY cores are judged.
        {
            vibehealth::Sampler g;
            std::map<int, long> mixed = {{0, 2400000}, {1, 2400000}, {2, 600000}, {3, 600000}};
            std::map<int, double> busy = {{0, 99}, {1, 98}, {2, 5}, {3, 3}};
            for (int i = 0; i < 10; i++) g.snailTick(true, mixed, busy);
            ok(!g.slowNow, "two busy cores at max, two idle at min: NO snail (idle cores ignored)");
            std::map<int, long> held = {{0, 1500000}, {1, 1500000}, {2, 600000}, {3, 600000}};
            for (int i = 0; i < 5; i++) g.snailTick(true, held, busy);
            ok(g.slowNow, "busy cores held at 1.5 GHz for 5 s: SNAIL");
        }
        // ★ A governor that pins the clock is configuration, not throttling.
        {
            std::ofstream(g_root + "/devices/system/cpu/cpu0/cpufreq/scaling_governor") << "powersave\n";
            std::ofstream(g_root + "/devices/system/cpu/cpu0/cpufreq/scaling_driver") << "cpufreq-dt\n";
            vibehealth::Sampler g;
            ok(!run(g, 10, true, 600000), "powersave on cpufreq-dt (pinned minimum) under load: NO snail");
            std::ofstream(g_root + "/devices/system/cpu/cpu0/cpufreq/scaling_governor") << "ondemand\n";
            ok(run(g, 5, true, 600000), "same clocks under ondemand (should have boosted): SNAIL");
        }
    } else {
        // An Intel-like chip: 3.9 GHz single-core boost advertised, 3.1 GHz real all-core.
        cores(4, 3900000);
        for (int i = 0; i < 4; i++) put("/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq", 3100000);
        ok(!vibehealth::detail::calibrated(), "fresh machine: not calibrated");
        const double mhz = vibehealth::detail::calibrateAllCore(0.5);
        ok(mhz > 3099 && mhz < 3101, "calibration measured the all-core clock (3100 MHz)");
        ok(vibehealth::detail::calibrated(), "and marked itself done");
        std::ifstream f(vibehealth::detail::peakFile()); std::string first; std::getline(f, first);
        ok(first == "calibrated 1", "saved to disk with the calibrated flag");
        vibehealth::Sampler s;
        ok(!run(s, 10, true, 3000000), "97 % of all-core peak (77 % of single-core boost): NO snail");
        ok(run(s, 5, true, 2600000), "84 % of all-core peak for 5 s: SNAIL");
        ok(!run(s, 5, true, 3100000), "back at the all-core peak: cleared");
        ok(!run(s, 20, false, 800000), "idle: never a snail");
        // ★ intel_pstate's "powersave" is DYNAMIC (the Lenovo reached 4.0 GHz on it) — must NOT be
        //   mistaken for a pinned clock.
        std::ofstream(g_root + "/devices/system/cpu/cpu0/cpufreq/scaling_governor") << "powersave\n";
        std::ofstream(g_root + "/devices/system/cpu/cpu0/cpufreq/scaling_driver") << "intel_pstate\n";
        vibehealth::Sampler p;
        ok(run(p, 5, true, 2600000), "intel_pstate powersave, slowed under load: SNAIL (not treated as pinned)");
        std::ofstream(g_root + "/devices/system/cpu/cpu0/cpufreq/scaling_driver") << "intel_cpufreq\n";
        vibehealth::Sampler q;
        ok(!run(q, 10, true, 800000), "intel_cpufreq (passive) powersave = pinned: NO snail");
    }
    fs::remove_all(g_root);
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
