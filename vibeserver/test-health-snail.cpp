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

/* ★ A FAKE FIRMWARE. A fake FILE cannot answer an ioctl, so the mailbox transport itself is swapped
 *   (vibevcio::mboxOverride) for this, which answers property messages the way the VideoCore does:
 *   response code 0x80000000, bit 31 on each tag it understood, values in place. */
struct FakeFw {
    bool present = true, measSupported = true;
    uint32_t maxHz = 2400000000u, setHz = 2400000000u, measHz = 2400000000u, thr = 0;
};
static FakeFw g_fw;
static long long g_thrRequest = -1;
static int fakeMbox(uint32_t* buf) {
    if (!g_fw.present) return -1;
    const uint32_t tag = buf[2];
    uint32_t* v = buf + 5;
    auto answer = [&](int words) { buf[1] = 0x80000000u; buf[4] = 0x80000000u | (uint32_t)(words * 4); };
    switch (tag) {
        case vibevcio::TAG_GET_MAX_CLOCK_RATE:      if (v[0] != 3) return -1; v[1] = g_fw.maxHz; answer(2); break;
        case vibevcio::TAG_GET_CLOCK_RATE:          if (v[0] != 3) return -1; v[1] = g_fw.setHz; answer(2); break;
        case vibevcio::TAG_GET_CLOCK_RATE_MEASURED:
            buf[1] = 0x80000000u;
            if (!g_fw.measSupported) { buf[4] = 8; break; }          // unknown tag: bit 31 clear
            v[1] = g_fw.measHz; answer(2); break;
        case vibevcio::TAG_GET_THROTTLED:           g_thrRequest = v[0]; v[0] = g_fw.thr; answer(1); break;
        default: return -1;
    }
    return 0;
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

    // ══ THE PI'S FIRMWARE CLOCK (vibe_vcio.h) — both builds, it does not depend on the boost path ══
    std::ofstream(g_root + "/devices/system/cpu/cpu0/cpufreq/scaling_governor") << "performance\n";
    std::ofstream(g_root + "/devices/system/cpu/cpu0/cpufreq/scaling_driver") << "cpufreq-dt\n";
    std::printf("-- firmware clock (mailbox) --\n");
    {
        // ★ The REAL transport against a path that is not there: silent, and ok=false.
        vibevcio::devPath() = g_root + "/no-such-vcio";
        const auto r = vibevcio::read();
        ok(!r.ok, "no /dev/vcio: read() says so and nothing else happens");
        vibehealth::detail::sysRoot() = g_root;   // (unchanged — the mailbox has its own path)
    }
    vibevcio::mboxOverride() = &fakeMbox;
    {
        g_fw = FakeFw{};   // Pi 500 at rest: 2400 max, 2400 measured, no flags
        const auto r = vibevcio::read();
        ok(r.ok && r.armHz == 2400000000LL && r.armMaxHz == 2400000000LL && r.measured,
           "fake firmware: measured 2400 MHz of 2400");
        ok(r.throttled == 0, "throttle flags read (0)");
        ok(g_thrRequest == 0, "get_throttled asked to clear NO sticky bits (the kernel's rpi_volt owns them)");
        g_fw.measHz = 1000000000; g_fw.thr = 0x50005;
        const auto t = vibevcio::read();
        ok(t.armHz == 1000000000LL && t.underVoltNow() && t.cappedNow(), "sagging: measured 1000 MHz, under-voltage + throttled NOW");
        g_fw.measSupported = false;
        const auto u = vibevcio::read();
        ok(u.ok && !u.measured && u.armHz == 2400000000LL, "older firmware without MEASURED: falls back to the set rate");
        g_fw.measSupported = true;
        g_fw.present = false;
        ok(!vibevcio::read().ok, "mailbox refuses: ok=false (silent fallback)");
        g_fw.present = true;

        // ★ The admin figure: the measured clock REPLACES sysfs's request.
        vibeadmin::SysStats s; s.cpuKHz = 2400000;
        g_fw.measHz = 1000000000; g_fw.thr = 0x50005;
        vibeadmin::applyFirmwareClock(s, vibevcio::read());
        ok(s.haveFw && s.cpuKHz == 1000000 && s.fwMaxKHz == 2400000 && s.fwAskedKHz == 2400000,
           "admin CPU CLOCK: 1000 MHz (firmware), asked 2400, max 2400");
        const std::string j = vibeadmin::sysJson(s);
        ok(j.find("\"cpuKHz\":1000000") != std::string::npos && j.find("\"cpuFirmware\":true") != std::string::npos
           && j.find("\"cpuMaxKHz\":2400000") != std::string::npos && j.find("\"throttled\":327685") != std::string::npos,
           "sysJson carries the firmware figure, the maximum and the flags");
        vibeadmin::SysStats plain; plain.cpuKHz = 2400000;
        vibeadmin::applyFirmwareClock(plain, vibevcio::Reading{});
        ok(!plain.haveFw && plain.cpuKHz == 2400000 && vibeadmin::sysJson(plain).find("cpuFirmware") == std::string::npos,
           "no firmware: the sysfs figure and the old JSON, unchanged");
    }
    vibevcio::mboxOverride() = nullptr;
    {
        // ★★★ THE PI 500 NIGHT: sysfs says 2400 on every core while the firmware flips 2400 ↔ 1000.
        vibehealth::FwClock up;   up.ok = true;   up.kHz = 2400000;   up.maxKHz = 2400000; up.throttled = 0x50000;
        vibehealth::FwClock down; down.ok = true; down.kHz = 1000000; down.maxKHz = 2400000; down.throttled = 0x50005;
        const auto sysfs = clocks(4, 2400000);
        vibehealth::Sampler s;
        for (int i = 0; i < 10; i++) s.snailTick(true, sysfs, {}, 85.0, &up);
        ok(!s.slowNow, "firmware at 2400 under load: no snail");
        bool shown = false; int at = -1;
        for (int i = 0; i < 10 && !shown; i++) { s.snailTick(true, sysfs, {}, 85.0, (i % 2) ? &up : &down); if (s.slowNow) { shown = true; at = i + 1; } }
        ok(shown && at <= 8, "firmware FLIPPING 2400/1000 under load: SNAIL within 8 s (sysfs said 2400 throughout)");
        ok(s.powerHold > 0 && s.thermalHold == 0, "under-voltage bit read NOW: lightning cause (power), not heat");
        for (int i = 0; i < 4; i++) s.snailTick(true, sysfs, {}, 85.0, &up);
        ok(s.slowNow, "4 s back at 2400: still showing (hysteresis)");
        s.snailTick(true, sysfs, {}, 85.0, &up);
        ok(!s.slowNow, "5 s back at 2400: cleared");
        // ★ The same flipping read the OLD way (five consecutive seconds) would never have shown.
        vibehealth::Sampler old;
        const long hi = vibehealth::detail::kBoostClocks ? 3100000 : 2400000;   // each path's reference
        for (int i = 0; i < 30; i++) old.snailTick(true, clocks(4, (i % 2) ? hi : 1000000), {});
        ok(!old.slowNow, "(why the window) a 5-s-consecutive rule on the same flipping NEVER fires");
        // Idle: the firmware idles low by design — never a snail.
        vibehealth::Sampler idle;
        for (int i = 0; i < 20; i++) idle.snailTick(false, sysfs, {}, 85.0, &down);
        ok(!idle.slowNow, "firmware low while idle: never a snail");
        // Cause read, never inferred.
        vibehealth::Sampler hot;
        vibehealth::FwClock soft = down; soft.throttled = 0x8;
        for (int i = 0; i < 6; i++) hot.snailTick(true, sysfs, {}, 85.0, &soft);
        ok(hot.slowNow && hot.thermalHold > 0 && hot.powerHold == 0, "soft temperature limit bit: fire (heat), not power");
        vibehealth::Sampler bare;
        vibehealth::FwClock none = down; none.throttled = 0;
        for (int i = 0; i < 6; i++) bare.snailTick(true, sysfs, {}, 85.0, &none);
        ok(bare.slowNow && bare.thermalHold == 0 && bare.powerHold == 0, "slow with no flag set: plain snail, NO inferred cause");
    }
    {
        // ★★ "BUSY", RECONSIDERED: the Pi 500's night was ~75 % average with load 12 on 4 cores.
        using vibehealth::detail::queueSaturated;
        ok(queueSaturated(true, 12.0, 4, 75.0), "75 % with load 12 on 4 cores: LOADED (sustained run queue)");
        ok(!queueSaturated(true, 3.0, 4, 75.0), "75 % with load 3 on 4 cores: not loaded (no queue)");
        ok(!queueSaturated(true, 12.0, 4, 30.0), "load 12 but 30 % CPU (I/O wait in the load average): not loaded");
        ok(!queueSaturated(false, 0.0, 4, 95.0), "no /proc/loadavg (Android): the queue says nothing");
        // And on the sysfs path the queued threshold lets bursty ~75 % cores be judged at all.
        vibehealth::Sampler a, b;
        std::map<int, double> bursty = {{0, 78}, {1, 72}, {2, 80}, {3, 70}};
        const long slow = vibehealth::detail::kBoostClocks ? 1000 : 1500000;   // well under any reference
        for (int i = 0; i < 6; i++) { a.snailTick(true, clocks(4, slow), bursty, 85.0); b.snailTick(true, clocks(4, slow), bursty, 60.0); }
        ok(!a.slowNow, "bursty 70-80 % cores at the old 85 % gate: nothing judged, no snail (the blind spot)");
        ok(b.slowNow, "same cores with the run-queue gate (60 %): SNAIL");
    }
    fs::remove_all(g_root);
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
