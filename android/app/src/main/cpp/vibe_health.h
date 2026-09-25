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
#include <string>
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

/** One sample. Call about once a second; it is cheap (readSys plus at most a few small sysfs reads).
 *  `prev` carries the hysteresis and the smoothing between calls. */
struct Sampler {
    double cpuEwma = -1, ramEwma = -1;
    Health last;
    int    critHoldCpu = 0, critHoldRam = 0, critHoldTemp = 0;
    bool   started = false;

    /** ★ EWMA at 0.3: fast enough that a real spike shows within a couple of seconds, slow enough
     *  that one busy frame does not repaint the pill. */
    static double ewma(double prev, double v) { return prev < 0 ? v : prev * 0.7 + v * 0.3; }

    Health sample(int batPct, bool batCharging) {
        const vibeadmin::SysStats s = vibeadmin::readSys();
        Health h;

        // ── CPU ────────────────────────────────────────────────────────────────────────────────
        /* ★★★ NORMALISED BY CORE COUNT. The admin card shows a per-core SUM ("104% / 800%"); feeding
         *     that straight in would paint a single busy core red on an 8-core phone. */
        double cpu = -1;
        if (s.cpuPct >= 0) cpu = s.cpuIsProcess ? s.cpuPct : (s.cores > 0 ? s.cpuPct / s.cores : s.cpuPct);
        else if (s.haveLoad && s.cores > 0) cpu = 100.0 * s.load1 / s.cores;   // fallback: load average
        static const double CPU_T[3] = { 50, 75, 90 };
        if (cpu >= 0) {
            cpuEwma = ewma(cpuEwma, std::min(100.0, cpu));
            h.cpu = detail::settle(detail::bucket(cpuEwma, CPU_T, true), last.cpu, cpuEwma, CPU_T, 5, true);
        } else h.cpu = last.cpu;

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
        const bool capped = detail::capObserved();
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
