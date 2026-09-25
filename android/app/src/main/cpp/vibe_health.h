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
            if (dt > 0) worst = std::max(worst, 100.0 * (double)db / (double)dt);
        }
        last[cpu] = { busy, total };
    }
    fclose(f);
    return worst;
}

}  // namespace detail

/** One sample. Call about once a second; it is cheap (readSys plus at most a few small sysfs reads).
 *  `prev` carries the hysteresis and the smoothing between calls. */
struct Sampler {
    double cpuEwma = -1, ramEwma = -1;
    int    iqDropRun = 0;          // consecutive seconds with IQ thrown away
    Health last;
    int    critHoldCpu = 0, critHoldRam = 0, critHoldTemp = 0;
    bool   started = false;

    /** ★ EWMA at 0.3: fast enough that a real spike shows within a couple of seconds, slow enough
     *  that one busy frame does not repaint the pill. */
    static double ewma(double prev, double v) { return prev < 0 ? v : prev * 0.7 + v * 0.3; }

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
        } else h.ram = last.ram;

        // ── TEMP, or a throttle, or nothing ────────────────────────────────────────────────────
        /* ★★ HEADROOM, NOT TEMPERATURE. 70 °C is fine on a chip that throttles at 100 and serious on
         *    one that throttles at 80, so the level is "how far from the limit", never the reading.
         *    readSys() has no trip point, so 80 °C is assumed — the figure the admin page has always
         *    used for its own warning. */
        static const double HEAD_T[3] = { 20, 10, 5 };      // lower headroom is worse
        if (s.haveTemp) {
            const double head = 80.0 - s.tempC;
            h.temp = detail::settle(detail::bucket(head, HEAD_T, false), last.temp, head, HEAD_T, 5, false);
            h.tempKind = TempKind::Sensor;
            /* ★ A machine that is BOTH hot and capped is at least High, whatever the headroom says —
             *  the cap is the hardware telling us the reading is optimistic. */
            if (capped && h.temp < HIGH) h.temp = HIGH;
        } else if (capped) {
            /* ★★★ THE CAUSE CHOOSES THE SNAIL, and only a real cap gets here (see capObserved).
             *  Flames = heat, bolt = power. Two causes, opposite fixes: cool it down, or find a
             *  better supply. Stuart, 2026-09-25: "snail on fire thermal throttle, snail with a
             *  lightning bolt power limit throttled."
             *  ★ under-voltage is the only power evidence this server collects (readSys reads the
             *    rpi_volt alarm, NOT vcgencmd — the service user cannot open /dev/vcio). */
            h.tempKind = (s.haveVolt && s.underVoltageNow) ? TempKind::ThrottlePower
                                                           : TempKind::ThrottleUnknown;
            h.temp = HIGH;
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
    std::string j = "{\"type\":\"health\",\"v\":1,\"cpu\":" + std::to_string((int)h.cpu)
                  + ",\"ram\":" + std::to_string((int)h.ram)
                  + ",\"temp\":{\"kind\":\"" + kindName(h.tempKind) + "\",\"level\":"
                  + std::to_string((int)h.temp) + "}";
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
