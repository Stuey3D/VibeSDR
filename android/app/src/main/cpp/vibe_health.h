// VibeServer — the public health signal: LEVELS ONLY, never figures.
//
// ★★★ WHY LEVELS AND NOT NUMBERS. Listeners asked for the server's CPU. The raw figures stay on the
//     admin page, where the owner is: a stranger does not need to know this machine is at 83 % of
//     800 %, and publishing it invites both misreading ("it's overloaded!") and fingerprinting. What
//     a listener actually wants to know is whether the receiver is healthy — four levels answer that
//     and nothing else. See BRIEF-server-health-pill.md §6.
//
// ★★ EVERYTHING HERE IS DERIVED FROM vibeadmin::readSys(), which already gathers CPU, RAM,
//    temperature, clock and the Pi's under-voltage alarm for the admin page. This header adds only
//    the smoothing, the thresholds and the hysteresis — it does not open a single new file, except
//    the per-core maximum clock the cap test needs and readSys() does not collect.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "vibe_admin.h"

namespace vibehealth {

enum Level : int { OK = 0, WARM = 1, HIGH = 2, CRIT = 3 };

/** What, if anything, the TEMP slot is showing. */
enum class TempKind { None, Sensor, ThrottleThermal, ThrottlePower, ThrottleUnknown };

struct Health {
    Level cpu = OK, ram = OK, temp = OK;
    /** ★★★ WHERE ON THE LADDER, NOT WHICH RUNG — so the icon can BLEND instead of snapping between
     *  three colours (Stuart, 2026-09-26: "can the icons blend between colours rather than snap
     *  between green amber red"). 0.0 = bottom of OK, 1.0 = exactly on the first threshold, 3.0 =
     *  fully critical. The LEVEL above is unchanged and still decides the words, the title text and
     *  the critical animation; this only tints.
     *  ★★ IT DOES NOT RE-EXPOSE THE FIGURE. The pill's whole rule is "LEVELS, NOT FIGURES — the raw
     *     numbers stay on the admin page" so a stranger cannot misread 800 % as an overload. A
     *     position on a ladder is not the reading: it carries no units and no scale, and 2.4 tells
     *     you nothing about how many cores this machine has.
     *  ★ Computed from the SAME already-EWMA'd values the level uses, so it inherits that smoothing
     *    and cannot shimmer on its own. -1 = not measured; the client then falls back to the rung. */
    float cpuPos = -1.0f, ramPos = -1.0f, tempPos = -1.0f;
    TempKind tempKind = TempKind::None;
    bool  batPresent = false, batCharging = false;
    int   batPct = -1;
    Level bat = OK;
};

namespace detail {

/** ★ A level that RISES at once and FALLS late.
 *
 *  A value sitting on a threshold would otherwise flip the colour several times a second, which is
 *  worse than either state: an icon that flickers reads as a fault in the icon. Rising immediately
 *  is deliberate — a receiver going critical should say so on the first sample, not three seconds
 *  later; it is coming BACK that is made slow. `back` is in the metric's own units (points of
 *  percent, or degrees of headroom). */
inline Level settle(Level now, Level was, double v, const double thr[3], double back, bool higherIsWorse) {
    if (now > was) return now;                       // worse: take it immediately
    // Not worse — only step down once clear of the threshold we are currently sitting above.
    if (was == OK) return OK;
    const double t = thr[was - 1];
    const bool clear = higherIsWorse ? (v < t - back) : (v > t + back);
    return clear ? (Level)(was - 1) : was;
}

/** ★★★ THE SAME LADDER bucket() WALKS, READ AS A CONTINUOUS POSITION — see Health::cpuPos.
 *
 *  bucket() answers "which of the four rungs"; this answers "how far up", by interpolating linearly
 *  INSIDE the band the value currently sits in. The two can never disagree, because they read the
 *  same thresholds: floor(pos) is always bucket()'s rung.
 *  ★★ The bottom band has no lower threshold to interpolate from, so it is measured from zero (or,
 *     for a headroom metric where LOWER is worse, from twice the first threshold — far enough away
 *     to be "comfortable" without inventing a scale the caller did not supply).
 *  ★ Clamped to the top rung: past the critical threshold there is nothing further to say, and an
 *    unbounded number would let one wild sample drag the colour somewhere it cannot come back from.
 */
inline float ladderPos(double v, const double thr[3], bool higherIsWorse) {
    auto span = [](double a, double b, double x) -> double {
        if (b == a) return 0.0;
        const double f = (x - a) / (b - a);
        return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
    };
    if (higherIsWorse) {
        if (v >= thr[2]) return 3.0f;
        if (v >= thr[1]) return (float)(2.0 + span(thr[1], thr[2], v));
        if (v >= thr[0]) return (float)(1.0 + span(thr[0], thr[1], v));
        return (float)span(0.0, thr[0], v);
    }
    // Headroom: SMALLER is worse, so the ladder runs downwards.
    if (v <= thr[2]) return 3.0f;
    if (v <= thr[1]) return (float)(2.0 + span(thr[1], thr[2], v));
    if (v <= thr[0]) return (float)(1.0 + span(thr[0], thr[1], v));
    return (float)span(thr[0] * 2.0, thr[0], v);
}

inline Level bucket(double v, const double thr[3], bool higherIsWorse) {
    auto over = [&](double t) { return higherIsWorse ? v >= t : v <= t; };
    if (over(thr[2])) return CRIT;
    if (over(thr[1])) return HIGH;
    if (over(thr[0])) return WARM;
    return OK;
}

inline long readLong(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return -1;
    long v = -1;
    if (std::fscanf(f, "%ld", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}

/** ★★★ IS THERE MAINS POWER? — because "not charging" is not the same as "running out".
 *
 *  MEASURED ON THE LENOVO, 2026-09-25: `BAT1` reports 55 % with status **"Not charging"** while
 *  `ACAD` (type Mains) is online — a laptop plugged in with a charge limiter holding it at 55. The
 *  shim's `charging` flag is false there, because it means literally "the battery is filling". Judge
 *  the battery LEVEL on that alone and this machine starts warning at 50 %, then orange, then red,
 *  about a battery that is on mains and in no danger whatever.
 *  ★★ A warning nobody can act on is the fault this whole pill exists to avoid — the same shape as
 *     the Pi 500's permanent under-voltage alarm.
 *  ★ Read from sysfs rather than inferred from the status string: "Not charging", "Full", "Unknown"
 *    and vendor spellings all mean different things, and the mains device says the one thing that
 *    matters plainly. Absent (a phone, a Pi) returns false and nothing changes. */
inline bool onMains() {
    // ★ A fixed list rather than a directory walk: this runs once a second on every server, and the
    //   names are standard (ACAD/AC/AC0 and ADP0/ADP1 on laptops).
    static const char* kNames[] = { "ACAD", "AC", "AC0", "AC1", "ADP0", "ADP1" };
    for (const char* n : kNames)
        if (readLong(std::string("/sys/class/power_supply/") + n + "/online") == 1) return true;
    return false;
}

/** ★★★ IS THE MACHINE ACTUALLY CAPPED? — the only thing allowed to raise a throttle icon.
 *
 *  MEASURED ON THE PI 500, 2026-09-25: `vcgencmd get_throttled` returned 0x50005 — under-voltage NOW
 *  and THROTTLED NOW — while every core sat at its full 2400 MHz, and four seconds later the live
 *  bits had cleared with the clock unchanged. Stuart: "My Pi500 always reports under voltage but
 *  from what I can see it is hitting the 2.4GHz it should all the time and isnt throttled."
 *  So the bits are NOT evidence of a cap. An observed cap is.
 *  ✗ CORRECTED 2026-09-30: "every core sat at its full 2400 MHz" was sysfs, which reports the REQUEST.
 *    The firmware's MEASURED clock was flipping 2400 ↔ 1000 in step with those bits. On a Pi the
 *    snail now reads the mailbox (vibe_vcio.h, Sampler::fwTick); this ceiling test is unchanged.
 *
 *  ★★ PER CORE AGAINST ITS OWN MAXIMUM, never a global one: on big.LITTLE the little cluster's
 *     maximum is legitimately lower, and comparing it to the big cores' would report every phone as
 *     permanently throttled.
 *  ★ A low clock on its own means nothing — governors idle at the minimum, and an XCover sits at
 *    800 MHz at 10 % load quite happily. Only a CAP (the ceiling itself lowered) counts here; the
 *    "slow under load" case is judged by the caller, which knows the CPU figure. */
inline bool capObserved() {
    for (int i = 0; i < 64; i++) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
        const long hw = readLong(base + "cpuinfo_max_freq");
        if (hw <= 0) { if (i == 0) return false; break; }      // no cpufreq at all → cannot tell
        const long cap = readLong(base + "scaling_max_freq");
        if (cap > 0 && cap < (long)(hw * 0.9)) return true;    // a ceiling has been imposed
    }
    return false;
}


/* ══ THE SNAIL: LOADED, AND SLOWER THAN THIS MACHINE CAN RUN WITH EVERY CORE BUSY ═════════════════
 * ★★★ Stuart, 2026-09-27: "the snail should appear when the system is fully loaded but not using its
 *     full CPU clock indicating throttling is happening. Snail on fire for thermal, snail [with] a
 *     lightning bolt indicates power" — and, for Intel: "it should detect the all core max speed and
 *     if system is loaded and the all core speed is anything below its maximum it should indicate".
 *     Until then the snail could only come from capObserved() — a LOWERED CEILING — which Intel
 *     never does (it drops the actual clock under an unchanged ceiling), and only on a machine with
 *     no temperature sensor. Kiko's fanless N4000 ran 15 min at 100 %, load 5.45, 76.8 °C and could
 *     never have shown one.
 * ★★ THE ALL-CORE MAXIMUM IS LEARNED, BECAUSE NOTHING PUBLISHES IT. cpuinfo_max_freq on Intel is the
 *    SINGLE-core boost — a healthy chip with every core busy runs below it by design, so comparing
 *    against it would call every Intel box throttled. The real all-core figure lives in an MSR that
 *    needs root. So: whenever the machine is fully loaded, each core's clock is recorded and the
 *    HIGHEST is kept. A chip runs its best all-core clock in the first seconds of load, before the
 *    heat builds, so even a fanless box captures it on its first busy spell.
 * ★ PER CORE, against each core's own learned peak, then averaged — so a big.LITTLE phone's slow
 *   cluster is judged against itself, never against the fast one.
 * ★ Kept on disk (peakFile) so a restart does not forget what the silicon can do. */
inline std::string& peakFile() { static std::string f; return f; }
/** ★ The sysfs root, "/sys" in life. A test points it at a fake tree so the snail can be driven
 *  through every case on a machine that has no cpufreq at all (see test-health-snail.cpp). */
inline std::string& sysRoot() { static std::string r = "/sys"; return r; }
/** ★★★ ONLY x86 NEEDS CALIBRATING. Stuart, 2026-09-27: "hardware where we can read the throttle status
 *  like Raspberry Pi etc don't need a throttle indication calibration." The reason is the CLOCK, not the
 *  flags: Intel/AMD advertise a single-core BOOST as cpuinfo_max_freq, which a healthy chip cannot hold
 *  with every core busy, so the all-core figure must be measured. ARM (Pis, phones, TV boxes) has no
 *  such boost — its cpuinfo_max_freq IS the all-core maximum, and is used directly.
 *  ★ (The Pi's firmware flags are not the trigger — the firmware's MEASURED clock is, where the mailbox
 *    answers; the flags only ever name the cause. See Sampler::fwTick and the correction at capObserved.) */
#if defined(__x86_64__) || defined(__i386__) || defined(VIBE_HEALTH_TEST_BOOST)
constexpr bool kBoostClocks = true;
#else
constexpr bool kBoostClocks = false;
#endif
/** Has the benchmark's all-core stress run on this machine? Stuart: "Only ever needs to be done on
 *  initial setup" — so it is stored with the peaks and checked before the stress ever runs again. */
inline bool& calibrated() { static bool c = false; return c; }
inline std::map<int, long>& corePeaks() {
    static std::map<int, long> m; static long long seenMtime = -1;
    /* ★★ RE-READ WHEN THE FILE CHANGES, not once. On Linux the benchmark runs in one process and every
     *  radio is its own process with its own sampler; loaded once, a radio that started before the
     *  calibration would never see it. A stat a second is nothing. Merged by MAX, so a peak this
     *  process learned itself is never lowered by an older file. */
    if (!peakFile().empty()) {
        struct stat st;
        if (stat(peakFile().c_str(), &st) == 0 && (long long)st.st_mtime != seenMtime) {
            seenMtime = (long long)st.st_mtime;
            if (FILE* f = fopen(peakFile().c_str(), "r")) {
                int cal = 0;
                if (fscanf(f, " calibrated %d", &cal) == 1 && cal) calibrated() = true;
                int c; long k;
                while (fscanf(f, "%d %ld", &c, &k) == 2) if (k > m[c]) m[c] = k;
                fclose(f);
            }
        }
    }
    return m;
}
inline void savePeaks() {
    if (peakFile().empty()) return;
    // ★ Write-then-rename: several radio processes on one machine may learn at once, and a reader
    //   must never see half a file.
    const std::string tmp = peakFile() + "." + std::to_string((long)getpid());
    if (FILE* f = fopen(tmp.c_str(), "w")) {
        fprintf(f, "calibrated %d\n", calibrated() ? 1 : 0);
        for (const auto& kv : corePeaks()) fprintf(f, "%d %ld\n", kv.first, kv.second);
        fclose(f);
        std::rename(tmp.c_str(), peakFile().c_str());
    }
}
/** Each core's current clock (kHz). Empty when cpufreq is absent — a Mac, a container.
 *  (Declared ahead of calibrateAllCore, which samples it.) */
inline std::map<int, long> coreClocks();
inline bool governorPinsClock();
/** ★★★ CALIBRATE THE ALL-CORE MAXIMUM ON PURPOSE — Stuart, 2026-09-27: "On initial benchmark run a
 *  quick CPU stress test on all cores." Waiting for load to happen by accident meant a fresh install
 *  had no reference at all, and a box whose first busy spell began hot learned a THROTTLED figure.
 *  The benchmark is when a burst is expected, the machine is cool and nobody is listening.
 *  ★ SHORT ON PURPOSE: long enough for the governor to ramp and the boost to engage, not long enough
 *    to heat-soak the chip — which would calibrate against the very throttling it exists to detect.
 *    Every core spins; each core's clock is sampled every 100 ms and its best kept.
 *  ★ Only ever RAISES a stored peak (merged by max), so a later cooler run still refines it.
 *  Returns the calibrated all-core average in MHz, or -1 where cpufreq is absent. */
inline double calibrateAllCore(double seconds = 3.0) {
    // ★ Under a governor that pins the clock this would record the pinned MINIMUM as the maximum.
    //   Skip, and stay uncalibrated, so the next benchmark under a normal governor does it properly.
    if (governorPinsClock()) return -1;
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<bool> stop{false};
    std::vector<std::thread> spin;
    for (unsigned i = 0; i < n; i++)
        spin.emplace_back([&stop] {
            volatile double x = 1.0000001;
            while (!stop.load(std::memory_order_relaxed))
                for (int k = 0; k < 20000; k++) x = x * 1.0000001 + 1e-12;
        });
    std::map<int, long> best;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (const auto& kv : coreClocks()) if (kv.second > best[kv.first]) best[kv.first] = kv.second;
    }
    stop.store(true);
    for (auto& t : spin) t.join();
    if (best.empty()) return -1;
    auto& peaks = corePeaks();
    calibrated() = true;
    double sum = 0;
    for (const auto& kv : best) { if (kv.second > peaks[kv.first]) peaks[kv.first] = kv.second; sum += kv.second; }
    savePeaks();
    return sum / best.size() / 1000.0;
}
inline std::map<int, long> coreClocks() {
    std::map<int, long> out;
    for (int i = 0; i < 256; i++) {
        const std::string cpuDir = sysRoot() + "/devices/system/cpu/cpu" + std::to_string(i);
        const long cur = readLong(cpuDir + "/cpufreq/scaling_cur_freq");
        if (cur > 0) { out[i] = cur; continue; }
        if (access(cpuDir.c_str(), F_OK) != 0) break;
    }
    return out;
}
/** ★★★ IS THE GOVERNOR HOLDING THE CLOCK DOWN ON PURPOSE? Stuart, 2026-09-27: "we also have to be
 *  concious of if set to On Demand or PowerSave our processes might not ever push the core to its
 *  maximum clock anyway". A pinned clock is the OWNER'S CONFIGURATION, not throttling — a fire or
 *  lightning snail there would send them chasing the wrong fault, and calibration would learn the pinned
 *  minimum as the "maximum". So the snail stands down.
 *  ★★ THE NAME IS A TRAP: "powersave" PINS the minimum on the generic drivers (acpi-cpufreq, cpufreq-dt,
 *     intel_cpufreq = intel_pstate PASSIVE, amd-pstate passive) — but on intel_pstate ACTIVE and
 *     amd-pstate-epp it is DYNAMIC and boosts under load (the Lenovo reached 4.0 GHz on it). "userspace"
 *     is a fixed clock. The dynamic governors (ondemand, schedutil, conservative, performance) reach the
 *     maximum under load, which is handled by judging BUSY cores only (see snailTick). */
inline bool governorPinsClock() {
    auto rd = [](const std::string& path) {
        std::string v;
        if (FILE* f = fopen(path.c_str(), "r")) { char b[64] = {0}; if (fgets(b, sizeof b, f)) v = b; fclose(f); }
        while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
        return v;
    };
    const std::string base = sysRoot() + "/devices/system/cpu/cpu0/cpufreq/";
    const std::string gov = rd(base + "scaling_governor"), drv = rd(base + "scaling_driver");
    if (gov == "userspace") return true;
    if (gov == "powersave") return !(drv == "intel_pstate" || drv == "amd-pstate-epp");
    return false;
}
/** The CPU's OWN throttle report, where it gives one (Intel): how many times it throttled for heat,
 *  and how many for a power limit. -1 = this machine does not say. Summed over cores. */
inline long throttleCount(const char* name) {
    long sum = -1;
    for (int i = 0; i < 256; i++) {
        const long v = readLong(sysRoot() + "/devices/system/cpu/cpu" + std::to_string(i) + "/thermal_throttle/" + name);
        if (v < 0) { if (i == 0) return -1; break; }
        sum = (sum < 0 ? 0 : sum) + v;
    }
    return sum;
}
}  // namespace detail

namespace detail {

/** ★★★ THE BUSIEST CORE, AND WHETHER ANYTHING CAN MOVE OFF IT.
 *
 *  Stuart, 2026-09-25, refining this twice: first "we cant have it green when some audio is breaking
 *  up due to a thread taking up an entire core, like we encountered when first building the Pi2
 *  build", then the sharper version — "its when that thread, regardless of if itself has grown or if
 *  other threads are running with it on the same core, starts getting starved that is the issue;
 *  that 1 core could be 100% and the other cores could be 75 50 60 but that 100% core will be the
 *  one causing us the issues."
 *
 *  ★★ SO THE MEASURE IS THE CORE, NOT OUR THREAD. What starves the audio is the core it happens to
 *     be on being saturated — by our work or anyone's — and that is invisible in a machine average
 *     (100/75/50/60 averages to a comfortable 71) and equally invisible in our own thread's figure,
 *     which says nothing about who else is on that core.
 *  ★★★ AND A SATURATED CORE IS ONLY A PROBLEM WHEN THERE IS NOWHERE TO GO. One core pegged with
 *      three idle ones is a stable system — the scheduler simply moves the rest away, which is
 *      exactly the case he called "happy all day". The same core pegged while the others sit at
 *      50-75 % means nothing can migrate and whatever is on it waits. That is why the busiest core
 *      is weighted by the machine's own load below rather than read on its own.
 *  ★ Straight from /proc/stat's per-cpu lines, so it counts every process on the machine and not
 *    just ours. Linux only; macOS keeps the machine total alone rather than a guess. */
/** Each core's busy % from the last busiestCorePct() call — the snail judges only BUSY cores. */
inline std::map<int, double>& coreBusy() { static std::map<int, double> m; return m; }
inline double busiestCorePct(double) {
    static std::map<int, std::pair<long, long>> last;   // cpu -> (busy, total)
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[512];
    double worst = -1;
    while (fgets(line, sizeof line, f)) {
        int cpu = -1;
        long u = 0, n = 0, sy = 0, id = 0, io = 0, irq = 0, sirq = 0, st = 0;
        if (sscanf(line, "cpu%d %ld %ld %ld %ld %ld %ld %ld %ld",
                   &cpu, &u, &n, &sy, &id, &io, &irq, &sirq, &st) != 9) continue;
        const long busy  = u + n + sy + irq + sirq + st;      // ★ iowait is NOT busy
        const long total = busy + id + io;
        auto it = last.find(cpu);
        if (it != last.end()) {
            const long db = busy - it->second.first, dt = total - it->second.second;
            if (dt > 0) {
                const double pct = 100.0 * (double)db / (double)dt;
                worst = std::max(worst, pct);
                coreBusy()[cpu] = pct;
            }
        }
        last[cpu] = { busy, total };
    }
    fclose(f);
    return worst;
}

/** ★★★ A SUSTAINED RUN QUEUE COUNTS AS "LOADED" — not only a machine average of 90 %.
 *
 *  MEASURED ON THE PI 500, 2026-09-30: ten listeners and their decoders held the machine at ~75 %
 *  on average with a load average of 12 on 4 cores — bursty, every core taking turns at 100 % and
 *  never all four at once for long. The old gate (≥ 90 % machine-wide, every core ≥ 85 %) almost
 *  never held, so the snail could not appear through a whole night of real firmware throttling.
 *  ★★ The load average IS the sustained measure: it is a one-minute exponential mean of the
 *     threads ready to run, so 12 on 4 cores means three are waiting behind every core, continuously,
 *     whatever the instantaneous percentages say. On a machine like that a slowed clock is lost
 *     throughput somebody is queueing for — which is exactly the case the snail exists to name.
 *  ★ 1.5× the core count: at parity every thread gets a core when it asks, so a queue only starts
 *    to mean waiting well above it. AND ≥ 60 % CPU, because Linux counts threads in uninterruptible
 *    sleep (USB and disk I/O) in the load average — a box with a high load and idle cores is
 *    waiting on I/O, not on its clock, and must not raise a clock verdict.
 *  ★ Android refuses /proc/loadavg, so there haveLoad is false and nothing changes. */
inline bool queueSaturated(bool haveLoad, double load1, int cores, double cpuPct) {
    return haveLoad && cores > 0 && cpuPct >= 60.0 && load1 >= 1.5 * cores;
}

}  // namespace detail

/** ★ The Pi's firmware clock for the snail — see vibe_vcio.h. ok=false everywhere else. */
struct FwClock {
    bool ok = false;
    long long kHz = 0, maxKHz = 0;
    long long throttled = -1;          ///< get_throttled bits; -1 = not readable
};

/** One sample. Call about once a second; it is cheap (readSys plus at most a few small sysfs reads).
 *  `prev` carries the hysteresis and the smoothing between calls. */
struct Sampler {
    double cpuEwma = -1, ramEwma = -1;
    int    iqDropRun = 0;          // consecutive seconds with IQ thrown away
    Health last;
    int    critHoldCpu = 0, critHoldRam = 0, critHoldTemp = 0;
    int    slowRun = 0, fastRun = 0;   // consecutive seconds below / back above the all-core peak
    bool   slowNow = false;
    long   lastCoreThr = -1, lastPkgPwr = -1;
    int    thermalHold = 0, powerHold = 0;   // seconds left on the CPU's own cause report
    uint32_t fwWin = 0;            // the firmware path's last 10 seconds, one bit each (1 = below)
    bool   started = false;

    /** ★ EWMA at 0.3: fast enough that a real spike shows within a couple of seconds, slow enough
     *  that one busy frame does not repaint the pill. */
    static double ewma(double prev, double v) { return prev < 0 ? v : prev * 0.7 + v * 0.3; }
    /** ★ One second of the snail's judgement, given whether the machine is fully loaded and each
     *  core's clock. Separate from sample() so it can be driven directly (test-health-snail.cpp) —
     *  sample() reads live load from /proc, which a test machine may not even have. Updates slowNow
     *  and the cause holds.
     *  @param busyPct  the per-core load a core must reach to be judged — 85 normally, lower when
     *                  the run queue says every core has work waiting (see queueSaturated).
     *  @param fw       the Pi's firmware clock, when the mailbox answers. It REPLACES the per-core
     *                  sysfs clocks, which on a Pi report the request and not the result. */
    void snailTick(bool loaded, const std::map<int, long>& clocks,
                   const std::map<int, double>& busy = detail::coreBusy(),
                   double busyPct = 85.0, const FwClock* fw = nullptr) {
        // ★★ A GOVERNOR THAT PINS THE CLOCK IS CONFIGURATION, NOT THROTTLING — see governorPinsClock.
        if (detail::governorPinsClock()) loaded = false;
        if (fw && fw->ok && fw->maxKHz > 0) { fwTick(loaded, *fw); return; }
        auto& peaks = detail::corePeaks();
        double ratioSum = 0; int ratioN = 0; bool learned = false;
        for (const auto& kv : clocks) {
            /* ★★★ ONLY BUSY CORES ARE JUDGED. An idle core under ondemand/schedutil sits at its minimum
             *  BY DESIGN; averaging it in would drag a healthy machine under the margin. A core that is
             *  saturated under a dynamic governor MUST be at its maximum — if it is not, something is
             *  holding it back. Where per-core load is unknown (no /proc/stat, e.g. Android), every core
             *  is judged, as before. */
            if (!busy.empty()) {
                const auto b = busy.find(kv.first);
                if (b == busy.end() || b->second < busyPct) continue;
            }
            long ref;
            if (detail::kBoostClocks) {
                long& pk = peaks[kv.first];
                if (loaded && kv.second > pk) { pk = kv.second; learned = true; }
                ref = pk;
            } else {
                // ★ ARM: the advertised maximum IS the all-core maximum — no learning needed.
                ref = detail::readLong(detail::sysRoot() + "/devices/system/cpu/cpu"
                                       + std::to_string(kv.first) + "/cpufreq/cpuinfo_max_freq");
            }
            if (ref > 0) { ratioSum += (double)kv.second / (double)ref; ratioN++; }
        }
        if (learned) detail::savePeaks();
        // ★ 7 % margin, sustained 5 s: the clock wobbles a few percent on its own, and a snail that
        //   flickers is noise. It clears only after 5 s back above, for the same reason.
        const bool below = loaded && ratioN > 0 && (ratioSum / ratioN) < 0.93;
        slowRun = below ? slowRun + 1 : 0;
        fastRun = below ? 0 : fastRun + 1;
        if (slowRun >= 5) slowNow = true;
        if (fastRun >= 5) slowNow = false;
        /* ★★ ONLY WHAT THE HARDWARE WILL TELL AN UNPRIVILEGED PROCESS — MEASURED, 2026-09-27:
         *    Intel (Lenovo, kernel 6.8): core_ and package_throttle_count ARE readable — the CPU's own
         *      report of throttling for HEAT. The power-limit counters no longer exist in this kernel,
         *      and RAPL shows the limit (200 W) but not the draw (energy_uj is root-only). So Intel can
         *      PROVE thermal and cannot read power. (HWiNFO reads MSR_CORE_PERF_LIMIT_REASONS through a
         *      root driver; granting a network service raw MSR access is not worth it.)
         *    Pi 500: the firmware's own flags, through the mailbox, when the service user is in
         *      `video` — see fwTick. Without it, the rpi_volt under-voltage alarm is the power evidence.
         *  ✗ "Slow and not hot, so it must be power" would be an INFERRED hardware readout. A slowed
         *    machine with no readable cause gets the plain snail. package_power_limit_count is still
         *    read for older kernels that have it. A rise within the last 10 s counts. */
        const long thrC = detail::throttleCount("core_throttle_count");
        const long thrP = detail::throttleCount("package_throttle_count");
        const long thr = (thrC < 0 && thrP < 0) ? -1 : std::max(0L, thrC) + std::max(0L, thrP);
        const long pwr = detail::throttleCount("package_power_limit_count");
        thermalHold = (thr >= 0 && lastCoreThr >= 0 && thr > lastCoreThr) ? 10 : std::max(0, thermalHold - 1);
        powerHold   = (pwr >= 0 && lastPkgPwr  >= 0 && pwr > lastPkgPwr)  ? 10 : std::max(0, powerHold - 1);
        lastCoreThr = thr; lastPkgPwr = pwr;
    }

    /** ★★★ THE PI: THE FIRMWARE'S MEASURED CLOCK AGAINST ITS MAXIMUM, AND ITS OWN REASON WHY.
     *
     *  The Pi 500, 2026-09-30, at full load on a sagging supply: `measure_clock arm` flipped
     *  2400 ↔ 1000 MHz in step with get_throttled 0x50005 ↔ 0x50000, while sysfs said 2400
     *  throughout. So on a Pi the clock is the FIRMWARE's, never sysfs's.
     *  ★ One ARM clock for the whole cluster, so there is no per-core judgement to make: a busy
     *    machine below its maximum is being held down, whichever core is busiest.
     *  ★★ AGAINST THE MAXIMUM, which on a Pi is the all-core figure (no boost) and which the
     *     governor asks for under load — VibeServer sets `performance` by default, and a governor
     *     that pins the minimum is already excluded above.
     *  ★★★ A WINDOW, NOT A RUN. The clock FLIPS: the firmware drops to 1000 MHz, the supply recovers,
     *      it returns to 2400, and round again — so "five consecutive seconds below" (the sysfs
     *      rule, which suits a clock that sags and stays) almost never holds, and a machine losing a
     *      quarter of its throughput would never be told. Here: below in ≥ 4 of the last 10 loaded
     *      seconds raises the snail; five clean seconds in a row clear it (the same exit as sysfs).
     *      4 of 10 at 1000/2400 is ≥ 23 % of the machine's work gone — well past a wobble.
     *  ★★ THE CAUSE IS READ, NEVER INFERRED: under-voltage NOW (bit 0) is the lightning; the soft
     *     temperature limit NOW (bit 3) is the fire. Each is held 10 s, like the Intel counters, so a
     *     flag that flickers with the clock does not flicker the icon. A snail with neither bit set
     *     stays plain — "slow and not hot, so it must be power" is exactly the guess we refuse. */
    void fwTick(bool loaded, const FwClock& fw) {
        const bool below = loaded && (double)fw.kHz / (double)fw.maxKHz < 0.93;
        fwWin = ((fwWin << 1) | (below ? 1u : 0u)) & 0x3ffu;          // the last 10 seconds
        int n = 0; for (uint32_t w = fwWin; w; w &= w - 1) n++;
        slowRun = below ? slowRun + 1 : 0;
        fastRun = below ? 0 : fastRun + 1;
        if (n >= 4) slowNow = true;
        if (fastRun >= 5) slowNow = false;
        const bool uv  = fw.throttled >= 0 && (fw.throttled & 0x1);
        const bool hot = fw.throttled >= 0 && (fw.throttled & 0x8);
        powerHold   = uv  ? 10 : std::max(0, powerHold - 1);
        thermalHold = hot ? 10 : std::max(0, thermalHold - 1);
    }

    Health sample(int batPct, bool batCharging, double dtSec = 1.0,
                  uint64_t iqDroppedDelta = 0) {
        const vibeadmin::SysStats s = vibeadmin::readSys();
        Health h;

        // ── CPU ────────────────────────────────────────────────────────────────────────────────
        /* ★★★ NORMALISED BY CORE COUNT. The admin card shows a per-core SUM ("104% / 800%"); feeding
         *     that straight in would paint a single busy core red on an 8-core phone. */
        /* ★★★ NORMALISE BY CORE COUNT IN *BOTH* CASES — this branch was inverted and it painted a
         *  perfectly healthy Android server red. On Android /proc/stat is unreadable, so readSys()
         *  falls back to THIS PROCESS's usage, where 100 % means one core: the Sony TV reported
         *  140 %, meaning vibeserver was using 1.4 of its 4 cores. Used raw against thresholds of
         *  50/75/90 that is instantly critical, while the machine was in fact about a third busy
         *  and the stream was flawless (Stuart, 2026-09-25: "not sure what is causing the sony to
         *  complain about its health as the stream was working perfect ... just listening to wfm
         *  with no decoders open").
         *  ★★ BOTH FORMS ARE PER-CORE SUMS — the machine-wide one and the process one alike — so
         *     both need dividing. The brief says exactly this and I applied it to the wrong half:
         *     "Always normalise by core count, or a single busy core would show red on an 8-core
         *     phone." The admin page prints it honestly as "140% / 400%"; this needed the same
         *     denominator and did not have it.
         *  ★ The process figure is still the right input where it is all we have: it is what OUR
         *    work costs, and this badge is about whether the RECEIVER is coping. */
        double cpu = -1;
        if (s.cpuPct >= 0) cpu = s.cores > 0 ? s.cpuPct / s.cores : s.cpuPct;
        else if (s.haveLoad && s.cores > 0) cpu = 100.0 * s.load1 / s.cores;   // fallback: load average
        static const double CPU_T[3] = { 50, 75, 90 };
        /* ★★★ THE TWO FIGURES ARE COMBINED, NOT RACED. Taking the worse of them called a busy core
         *  a problem on an idle machine, and it is not one. Stuart, 2026-09-25: "85% thread may be
         *  happy all day until the core itself gets slammed at 100%, but if that 85% is on its own
         *  core doing its own thing and the other processes are spread out like they are now then
         *  that is a stable system even though that thread is high."
         *
         *  ★★ SO THE RISK IS CONTENTION, NOT BUSYNESS. A thread pegged at 85 % with three idle cores
         *     around it owns its core and will keep owning it. The SAME thread on a machine that is
         *     itself at 80 % is competing for that core, and the moment it loses, the audio it
         *     carries stutters. The thread figure therefore counts in PROPORTION to how loaded the
         *     machine is: barely at all when there is room, fully when there is none.
         *  ★ At an idle machine the thread contributes 40 % of its value; at a saturated one, 100 %.
         *    An 85 % thread reads 34 (green) on an idle box and 75 (high) on a busy one — which is
         *    the distinction he drew, expressed as one number instead of two verdicts.
         *  ★ Still the MAX against the machine total, so a machine in trouble is reported whatever
         *    its threads are doing individually. */
        const bool capped = detail::capObserved();
        const double hottest = detail::busiestCorePct(dtSec);
        /* ★★★ THE RUN QUEUE — threads that are READY and waiting for a core. This is the most
         *  direct proxy there is for "something is being starved": a load average above the core
         *  count means somebody is always queueing, whatever the percentages look like. */
        const double loadRatio = (s.haveLoad && s.cores > 0) ? 100.0 * s.load1 / s.cores : -1;
        double pressure = cpu;
        if (cpu >= 0 && hottest >= 0) {
            /* ★ How little room there is to move work off that core. At an idle machine the hottest
             *  core contributes 40 % of its value (a lone busy core is fine); at a saturated one,
             *  all of it. 100/75/50/60 gives a machine total of 71 and a pressure of 83 — high,
             *  which is the verdict he wanted; one core at 85 with the rest idle gives 45 — green. */
            const double contention = 0.4 + 0.6 * std::min(1.0, cpu / 100.0);
            /* ★★★ AND A KNEE AT SATURATION, which is what reconciles two cases that look like they
             *  contradict each other. Stuart's own figures, 2026-09-25:
             *      100 % of 400 %, cores 50/25/15/10  -> fine
             *      100 % of 400 %, cores 95/ 3/ 1/ 1  -> "Bad, one hiccup away from issues"
             *  Identical machine totals; the difference is entirely that one core is nearly full.
             *  And earlier: 85 % on its own core with the rest idle is "happy all day".
             *  ★★ So below the knee a lone busy core really is fine — there is slack, and the
             *     scheduler has somewhere to put everything else. Above it there is no slack left
             *     ON THAT CORE, and the work sitting there cannot be split however idle the rest of
             *     the machine is. 88 % is where 85 stays green and 95 does not.
             *  ★ 95 % scores 89.6 — high, and deliberately just short of critical. That is what
             *    "one hiccup away from issues" means: not broken, but with nothing left in hand. */
            static constexpr double kKnee = 88.0;
            const double sat = hottest < kKnee ? 0.0
                             : 75.0 + (hottest - kKnee) * (25.0 / (100.0 - kKnee));
            pressure = std::max({ cpu, std::min(100.0, hottest) * contention, sat });
        }
        /* ★★★ A COMBINATION DETECTOR — every factor that makes a server hiccup, in one colour.
         *
         *  Stuart, 2026-09-25: "we make a combination detector for the CPU meter, that takes all of
         *  the factors that can cause a server to have hiccups and issues and make the colour react
         *  accordingly."
         *
         *  ★★★ THE STRONGEST FACTOR IS NOT A PROXY AT ALL. Dropped IQ is not a PREDICTION that the
         *      receiver might struggle — it is the radio's own samples being thrown on the floor
         *      because nothing collected them in time. When that is happening the pill must not be
         *      green whatever the percentages say, so it sets a FLOOR rather than joining the
         *      average.
         *  ★★★ AND SPECTRUM FRAMES DROPPING IS DELIBERATELY IGNORED. The thread priority is
         *      NETWORK > AUDIO > SPECTRUM > DECODERS, so shedding spectrum under load is the design
         *      WORKING — the Pi 2 holds a steady waterfall at a reduced rate with the audio intact,
         *      and Stuart's own words were "that is the process priority working as it should".
         *      Colouring the pill for it would report correct behaviour as a fault, which is the
         *      same mistake the app's link meter made this morning.
         *  ★★ A frequency CAP counts too: a throttled machine is a slower machine, and the work has
         *     not got any smaller. It is worth a nudge, not an alarm, because the hardware is
         *     protecting itself — which is what it is supposed to do.
         *  ★ Proxies take the MAX, not a sum: these describe the same shortage from different angles
         *    and adding them would double-count one busy machine into a crisis. */
        static const double LOAD_T[3] = { 90, 130, 200 };   // load1 as a % of core count
        double score = pressure;
        /* ★ The run queue only says anything once there are MORE runnable threads than cores —
         *  below that everything gets a core when it asks and the queue is not a shortage. The
         *  earlier form (ratio/2 + 25) contributed 25 at zero load, which is a score for a machine
         *  doing nothing at all, and reached critical on an idle Android box whose load average
         *  counts sleepers. Mapped from 100 % (parity) to 250 % (badly oversubscribed). */
        if (loadRatio > 100.0)
            score = std::max(score, 50.0 + (std::min(250.0, loadRatio) - 100.0) * (40.0 / 150.0));
        if (score >= 0) {
            cpuEwma = ewma(cpuEwma, std::min(100.0, score));
            h.cpu = detail::settle(detail::bucket(cpuEwma, CPU_T, true), last.cpu, cpuEwma, CPU_T, 5, true);
            h.cpuPos = detail::ladderPos(cpuEwma, CPU_T, true);
        } else h.cpu = last.cpu;
        if (loadRatio >= LOAD_T[2] && h.cpu < HIGH) h.cpu = HIGH;   // the queue never clears

        /* ★ A cap is a nudge. ★★ Dropped IQ is a floor: HIGH the moment it happens, CRITICAL if it
         *  is still happening a few seconds later — by then it is not a blip, it is the state of the
         *  machine. Counted in whole samples, so any non-zero delta is real. */
        if (capped && h.cpu < WARM) h.cpu = WARM;
        if (iqDroppedDelta > 0) {
            iqDropRun++;
            if (h.cpu < HIGH) h.cpu = HIGH;
            if (iqDropRun >= 3) h.cpu = CRIT;
        } else iqDropRun = 0;

        // ── RAM ────────────────────────────────────────────────────────────────────────────────
        static const double RAM_T[3] = { 70, 85, 95 };
        if (s.haveMem && s.memTotalKB > 0) {
            const double used = 100.0 * (double)(s.memTotalKB - s.memAvailKB) / (double)s.memTotalKB;
            ramEwma = ewma(ramEwma, used);
            h.ram = detail::settle(detail::bucket(ramEwma, RAM_T, true), last.ram, ramEwma, RAM_T, 5, true);
            h.ramPos = detail::ladderPos(ramEwma, RAM_T, true);
        } else h.ram = last.ram;

        // ── THE SNAIL: loaded and below the all-core maximum (see snailTick) ───────────────────
        {
            // ★ Loaded = a saturated machine OR a sustained run queue (see queueSaturated); when it
            //   is the queue, every core has work waiting, so a core at 60 % is not idling by design.
            const bool queued = detail::queueSaturated(s.haveLoad, s.load1, s.cores, cpu);
            FwClock fw;
            if (s.haveFw) { fw.ok = true; fw.kHz = s.cpuKHz; fw.maxKHz = s.fwMaxKHz; fw.throttled = s.fwThrottled; }
            snailTick(cpu >= 90.0 || queued, detail::coreClocks(), detail::coreBusy(),
                      queued ? 60.0 : 85.0, &fw);
        }
        const bool slowed = slowNow || capped;
        // ── TEMP, or a throttle, or nothing ────────────────────────────────────────────────────
        /* ★★ HEADROOM, NOT TEMPERATURE. 70 °C is fine on a chip that throttles at 100 and serious on
         *    one that throttles at 80, so the level is "how far from the limit", never the reading.
         *    readSys() has no trip point, so 80 °C is assumed — the figure the admin page has always
         *    used for its own warning. */
        static const double HEAD_T[3] = { 20, 10, 5 };      // lower headroom is worse
        if (slowed) {
            /* ★★★ THROTTLED WINS THE SLOT, SENSOR OR NOT. This used to be reachable only on a machine
             *  WITHOUT a temperature sensor, so a thermometer on a throttling machine said "warm" while
             *  the real message was "running slow". The cause picks the snail: the CPU's own counters
             *  first, then what we can see — within 10 °C of the limit is heat, under-voltage is power. */
            const bool hot   = thermalHold > 0 || (s.haveTemp && 80.0 - s.tempC <= 10.0);
            const bool power = powerHold > 0 || (s.haveVolt && s.underVoltageNow);
            h.tempKind = (power && !hot) ? TempKind::ThrottlePower
                       : hot             ? TempKind::ThrottleThermal
                                         : TempKind::ThrottleUnknown;
            h.temp = HIGH;
        } else if (s.haveTemp) {
            const double head = 80.0 - s.tempC;
            h.temp = detail::settle(detail::bucket(head, HEAD_T, false), last.temp, head, HEAD_T, 5, false);
            h.tempPos = detail::ladderPos(head, HEAD_T, false);
            h.tempKind = TempKind::Sensor;
        } else {
            h.tempKind = TempKind::None;                   // ★ slot omitted; the pill gets narrower
            h.temp = OK;
        }

        // ── Battery ────────────────────────────────────────────────────────────────────────────
        static const double BAT_T[3] = { 50, 20, 10 };      // lower is worse
        h.batPresent = batPct >= 0;
        h.batPct = batPct;
        /* ★ Charging OR ON MAINS is always OK. Both halves are needed: a phone at 8 % on a charger
         *  is not a problem, and neither is a laptop pinned at 55 % by a charge limiter — which
         *  reports "Not charging", not "Charging". See onMains(). */
        const bool powered = batCharging || detail::onMains();
        h.bat = (!h.batPresent || powered) ? OK : detail::bucket(batPct, BAT_T, false);
        h.batCharging = powered;      // ★ the client draws a bolt for "you are not running down"


        /* ★★ CRITICAL COSTS THREE SECONDS. It is the only level that animates, and a one-sample
         *  spike that starts the pill breathing then stops is worse than a slightly late warning. */
        auto hold = [](Level& l, int& n) { if (l == CRIT) { if (++n < 3) l = HIGH; } else n = 0; };
        hold(h.cpu, critHoldCpu); hold(h.ram, critHoldRam); hold(h.temp, critHoldTemp);

        last = h; started = true;
        return h;
    }
};

inline const char* kindName(TempKind k) {
    switch (k) {
        case TempKind::Sensor:           return "sensor";
        case TempKind::ThrottleThermal:  return "thermal";
        case TempKind::ThrottlePower:    return "power";
        case TempKind::ThrottleUnknown:  return "throttle";
        default:                         return "none";
    }
}

/** The public message. ★ Levels and the battery percentage only — no °C, no MHz, no RAM figure. */
inline std::string json(const Health& h) {
    /* ★★ ONE DECIMAL IS ENOUGH AND IS THE POINT. The colour is interpolated from this, and the eye
     *  cannot resolve a thirtieth of a band — but two decimals would make the field look like a
     *  measurement, which is exactly what this pill refuses to publish. */
    auto pos1 = [](float v) {
        char b2[16]; std::snprintf(b2, sizeof b2, "%.1f", v < 0.0f ? 0.0f : (v > 3.0f ? 3.0f : v));
        return std::string(b2);
    };
    std::string j = "{\"type\":\"health\",\"v\":1,\"cpu\":" + std::to_string((int)h.cpu)
                  + ",\"ram\":" + std::to_string((int)h.ram)
                  + ",\"temp\":{\"kind\":\"" + kindName(h.tempKind) + "\",\"level\":"
                  + std::to_string((int)h.temp)
                  + (h.tempPos >= 0.0f ? ",\"pos\":" + pos1(h.tempPos) : std::string()) + "}";
    /* ★ ADDITIVE, and omitted when not measured: a client older than 5.6.58 ignores these and keeps
     *  drawing the four fixed colours, and a newer one falls back to the rung when they are absent
     *  rather than guessing a position. */
    if (h.cpuPos >= 0.0f) j += ",\"cpuPos\":" + pos1(h.cpuPos);
    if (h.ramPos >= 0.0f) j += ",\"ramPos\":" + pos1(h.ramPos);
    if (h.batPresent) {
        j += ",\"bat\":{\"present\":true,\"pct\":" + std::to_string(h.batPct)
           + ",\"charging\":" + (h.batCharging ? "true" : "false")
           + ",\"level\":" + std::to_string((int)h.bat) + "}";
    } else {
        j += ",\"bat\":{\"present\":false}";
    }
    return j + "}";
}

/** True when anything a client draws has changed — the message is sent on change, not on a timer. */
inline bool differs(const Health& a, const Health& b) {
    return a.cpu != b.cpu || a.ram != b.ram || a.temp != b.temp || a.tempKind != b.tempKind
        || a.bat != b.bat || a.batPresent != b.batPresent || a.batCharging != b.batCharging
        || a.batPct != b.batPct;
}

}  // namespace vibehealth
