// vibe_power.h — supply dips on a Raspberry Pi, COUNTED rather than sampled.
//
// ★★★ WHY (Stuart's Pi 500, 2026-10-16). The admin page's POWER card read the rpi_volt alarm at the
//     instant the page polled and turned RED — "UNDER-VOLTAGE — the supply cannot keep up — expect USB
//     devices to drop out" — whenever that instant fell in a dip. That supply sags for a second or two
//     (~4.65 V) many times an hour under bursty load (schedutil + Advanced RDS), with clean audio and
//     NOT ONE USB dropout. A red card that cries wolf is how the real fault gets ignored, so:
//       · brief dips are AMBER, with how many in the last hour;
//       · RED only when it is serious — a dip SUSTAINED ≥ 10 s, a USB device actually lost during a
//         dip, a USB over-current event, or dips so frequent they are effectively continuous.
//
// ★★ WHAT THE ALARM ACTUALLY IS. /sys/class/hwmon/hwmonN/in0_lcrit_alarm (name = "rpi_volt") is NOT a
//    live voltage comparator. The kernel driver (drivers/hwmon/raspberrypi-hwmon.c) asks the firmware
//    for get_throttled every 2 s, CLEARING the sticky bits as it does, and reports bit 16 — "under-
//    voltage happened at some point in the last poll interval". So:
//      · a 1-2 s dip reads as 1 for one or two 2-s windows (2-4 s), never as nothing;
//      · reading it once a second cannot miss a window (each value stands for ~2 s);
//      · the kernel calls hwmon_notify_event on every change, which is sysfs_notify — so poll()
//        with POLLPRI wakes on the edge itself. The 1 s timeout is the belt to that pair of braces.
//    Readable by any user on Raspberry Pi OS (0444), no root, no `video`, no dmesg.
//
// ★★ USB LOSS without root. dmesg is restricted (kernel.dmesg_restrict) and the journal needs a group
//    the service user is not in, so neither is read. Instead:
//      · /dev/bus/usb/BBB/DDD — one file per enumerated device. A device that drops and comes back
//        returns with a NEW device number, so "an entry vanished" catches the bounce too, not only a
//        device that stays gone. One readdir per bus, every 2 s.
//      · /sys/bus/usb/devices/*:*/*-port*/over_current_count — the hub's own per-port count (kernel
//        ≥ 4.20). Cumulative, so a 10 s cadence misses nothing.
//    A vanished device only counts against the SUPPLY when it lands within 10 s of a dip (the alarm
//    trails the dip by up to 2 s, so the window looks both ways) — somebody unplugging a dongle on a
//    healthy supply is not a power fault. Over-current counts whenever it happens: that IS the fault
//    (2026-08-08: three SDRs against a 600 mA budget, 66 over-current events, a radio that would not
//    stay up).
//
// ★ Linux only, and only where rpi_volt exists. Everywhere else start() finds nothing and returns
//   false, readSys() keeps its old fields and the page shows nothing new.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(__linux__) || defined(VIBE_POWER_TEST)
  #include <dirent.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <unistd.h>
#endif

namespace vibepower {

/** ★★ THE THRESHOLDS, and why each sits where it does.
 *  kSustainedSec 10 — the alarm is quantised to the kernel's 2-s poll, so a 1-2 s dip reads 2-4 s and
 *    two dips back to back can read 6. 10 s is FIVE consecutive polls each finding the supply below
 *    the PMIC's ~4.63 V threshold: not a burst of load, a supply that is under. Well clear of the
 *    quantisation, well short of the time an SD write or a USB device needs to come to harm.
 *  kRecentSec / kFrequentFrac 300 s, 50 % — dips that leave the supply low for half of five minutes
 *    are not "brief" whatever each one measured; that is a continuous fault with gaps in it.
 *  kUsbCoincideSec 10 — a device lost within 10 s of a dip (either side; the alarm trails by ≤ 2 s,
 *    the USB scan by ≤ 2 s) is blamed on the supply; outside it, on whoever pulled the plug. */
constexpr double kHourSec        = 3600.0;
constexpr double kSustainedSec   = 10.0;
constexpr double kRecentSec      = 300.0;
constexpr double kFrequentFrac   = 0.5;
constexpr double kUsbCoincideSec = 10.0;

enum class Verdict { Ok, Dipped, Brief, Frequent, Sustained, OverCurrent, UsbLost };

inline const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::Ok:          return "ok";
        case Verdict::Dipped:      return "dipped";      // not in the last hour, but since start
        case Verdict::Brief:       return "brief";
        case Verdict::Frequent:    return "frequent";
        case Verdict::Sustained:   return "sustained";
        case Verdict::OverCurrent: return "overcurrent";
        case Verdict::UsbLost:     return "usblost";
    }
    return "ok";
}
/** "ok" | "warning" | "critical" — the card's colour, decided here, never on the page. */
inline const char* verdictStatus(Verdict v) {
    switch (v) {
        case Verdict::Ok:     return "ok";
        case Verdict::Dipped:
        case Verdict::Brief:  return "warning";
        default:              return "critical";
    }
}

struct Summary {
    bool   now = false;              ///< the alarm is up at this moment
    bool   ever = false;             ///< seen since watching began
    int    dipsHour = 0;             ///< episodes STARTED in the last hour (the current one included)
    double longestHourSec = 0;       ///< longest episode touching the last hour (an open one, so far)
    double nowSec = 0;               ///< the open episode so far; 0 when the supply is fine
    double recentSec = 0;            ///< seconds under-voltage in the last kRecentSec
    int    usbLostHour = 0;          ///< USB devices lost within kUsbCoincideSec of a dip, last hour
    int    overCurrentHour = 0;      ///< USB over-current events, last hour
    double lastDipAgoSec = -1;       ///< since the last episode ENDED (0 while one is open), -1 none
    Verdict verdict = Verdict::Ok;
};

/** ★★★ THE CLASSIFICATION — pure, so it is tested case by case (test-power-dips.cpp).
 *  Worst first: a device actually lost outranks everything, because it is the harm the rest only
 *  predicts. */
inline Verdict classify(const Summary& s) {
    if (s.usbLostHour > 0)                                         return Verdict::UsbLost;
    if (s.overCurrentHour > 0)                                     return Verdict::OverCurrent;
    if (s.nowSec >= kSustainedSec || s.longestHourSec >= kSustainedSec) return Verdict::Sustained;
    if (s.recentSec >= kFrequentFrac * kRecentSec)                 return Verdict::Frequent;
    if (s.dipsHour > 0 || s.now)                                   return Verdict::Brief;
    if (s.ever)                                                    return Verdict::Dipped;
    return Verdict::Ok;
}

/** Episodes and USB events on a MONOTONIC clock (seconds). Thread-safe: the watcher feeds it, the
 *  status request reads it. ★ Steady, not wall time — a Pi has no RTC and NTP steps the wall clock
 *  minutes at a time after boot, which would invent or erase an hour of dips. */
class Tracker {
public:
    /** One alarm reading. A rising edge opens an episode; a falling edge closes it. */
    void alarm(double t, bool on) {
        std::lock_guard<std::mutex> lk(m_);
        if (on && !on_) { eps_.push_back({t, -1}); ever_ = true; }
        if (!on && on_ && !eps_.empty() && eps_.back().end < 0) eps_.back().end = t;
        on_ = on;
        prune(t);
    }
    /** A USB device disappeared (or re-enumerated) at t. Judged against the dips in summary(). */
    void usbLost(double t) { std::lock_guard<std::mutex> lk(m_); usb_.push_back(t); prune(t); }
    void overCurrent(double t, long n) {
        std::lock_guard<std::mutex> lk(m_);
        for (long i = 0; i < n && i < 1000; i++) oc_.push_back(t);
        prune(t);
    }

    Summary summary(double now) const {
        std::lock_guard<std::mutex> lk(m_);
        Summary s;
        s.now = on_; s.ever = ever_;
        const double hourAgo = now - kHourSec, recentAgo = now - kRecentSec;
        for (const auto& e : eps_) {
            const double end = e.end < 0 ? now : e.end;
            if (end < hourAgo) continue;
            if (e.start >= hourAgo) s.dipsHour++;
            s.longestHourSec = std::max(s.longestHourSec, end - e.start);
            const double a = std::max(e.start, recentAgo), b = end;
            if (b > a) s.recentSec += b - a;
        }
        if (on_ && !eps_.empty()) s.nowSec = now - eps_.back().start;
        if (on_) s.lastDipAgoSec = 0;
        else if (!eps_.empty() && eps_.back().end >= 0) s.lastDipAgoSec = now - eps_.back().end;
        for (double u : usb_) {
            if (u < hourAgo) continue;
            for (const auto& e : eps_) {
                const double end = e.end < 0 ? now : e.end;
                if (u >= e.start - kUsbCoincideSec && u <= end + kUsbCoincideSec) { s.usbLostHour++; break; }
            }
        }
        for (double o : oc_) if (o >= hourAgo) s.overCurrentHour++;
        s.verdict = classify(s);
        return s;
    }

    /** The last hour's episodes as [seconds ago it started, seconds ago it ended] (0 = still open),
     *  newest last, at most `max` of them — for the dip marks on the page's graphs. */
    std::vector<std::pair<double, double>> recent(double now, size_t max = 60) const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::pair<double, double>> v;
        for (const auto& e : eps_) {
            const double end = e.end < 0 ? now : e.end;
            if (end < now - kHourSec) continue;
            v.push_back({now - e.start, now - end});
        }
        if (v.size() > max) v.erase(v.begin(), v.end() - (long)max);
        return v;
    }

private:
    struct Ep { double start, end; };   // end < 0 = open
    void prune(double now) {
        // ★ An hour and a bit — the coincidence window reaches back past the hour's edge.
        const double keep = now - kHourSec - kUsbCoincideSec - 1;
        while (eps_.size() > 1 && eps_.front().end >= 0 && eps_.front().end < keep) eps_.pop_front();
        while (!usb_.empty() && usb_.front() < keep) usb_.pop_front();
        while (!oc_.empty()  && oc_.front()  < keep) oc_.pop_front();
        // ★ A hard ceiling, whatever the supply does: one episode per 2-s poll is 1800 an hour.
        while (eps_.size() > 4000) eps_.pop_front();
        while (usb_.size() > 4000) usb_.pop_front();
        while (oc_.size()  > 4000) oc_.pop_front();
    }
    mutable std::mutex m_;
    std::deque<Ep> eps_;
    std::deque<double> usb_, oc_;
    bool on_ = false, ever_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  The sources — each takes its root, so a test can stand up a fake /sys and /dev.
// ─────────────────────────────────────────────────────────────────────────────────────────────

inline std::string readSmall(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char b[64] = {0};
    const size_t n = std::fread(b, 1, sizeof b - 1, f);
    std::fclose(f);
    std::string s(b, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) s.pop_back();
    return s;
}

#if defined(__linux__) || defined(VIBE_POWER_TEST)
/** <sys>/class/hwmon/hwmonN/in0_lcrit_alarm of the hwmon named rpi_volt, or "" — found by NAME, the
 *  number moves between kernels. */
inline std::string findAlarm(const std::string& sysRoot) {
    for (int i = 0; i < 16; i++) {
        const std::string d = sysRoot + "/class/hwmon/hwmon" + std::to_string(i);
        if (readSmall(d + "/name") == "rpi_volt") return d + "/in0_lcrit_alarm";
    }
    return {};
}

/** Every enumerated USB device as "BBB/DDD". A bounce comes back with a new DDD. */
inline std::set<std::string> usbDevices(const std::string& devRoot) {
    std::set<std::string> out;
    const std::string base = devRoot + "/bus/usb";
    if (DIR* d = opendir(base.c_str())) {
        while (dirent* bus = readdir(d)) {
            if (bus->d_name[0] == '.') continue;
            const std::string bd = base + "/" + bus->d_name;
            if (DIR* e = opendir(bd.c_str())) {
                while (dirent* dev = readdir(e))
                    if (dev->d_name[0] != '.') out.insert(std::string(bus->d_name) + "/" + dev->d_name);
                closedir(e);
            }
        }
        closedir(d);
    }
    return out;
}

/** The sum of every hub port's over_current_count; -1 where the kernel has none. */
inline long overCurrentTotal(const std::string& sysRoot) {
    long total = -1;
    const std::string base = sysRoot + "/bus/usb/devices";
    if (DIR* d = opendir(base.c_str())) {
        while (dirent* it = readdir(d)) {
            if (!std::strchr(it->d_name, ':')) continue;          // hub INTERFACES hold the ports
            const std::string id = base + "/" + it->d_name;
            if (DIR* e = opendir(id.c_str())) {
                while (dirent* p = readdir(e)) {
                    if (!std::strstr(p->d_name, "-port")) continue;
                    const std::string v = readSmall(id + "/" + p->d_name + "/over_current_count");
                    if (v.empty()) continue;
                    total = std::max(0L, total) + std::atol(v.c_str());
                }
                closedir(e);
            }
        }
        closedir(d);
    }
    return total;
}

/** ★ ONE STEP OF THE WATCHER, separate from its thread so a test can drive it against a fake root on
 *  a fake clock. Holds the previous USB set and over-current total between steps. */
struct Watch {
    std::string sysRoot = "/sys", devRoot = "/dev", alarmPath;
    Tracker tracker;
    std::set<std::string> lastUsb;
    bool haveUsb = false;
    long lastOc = -1;
    double lastUsbAt = -1e18, lastOcAt = -1e18;

    bool find() { alarmPath = findAlarm(sysRoot); return !alarmPath.empty(); }

    /** @param alarmText what the alarm file read ("" = unreadable this time; the reading is skipped). */
    void step(double now, const std::string& alarmText) {
        if (!alarmText.empty()) tracker.alarm(now, std::atoi(alarmText.c_str()) != 0);
        if (now - lastUsbAt >= 2.0) {
            lastUsbAt = now;
            std::set<std::string> cur = usbDevices(devRoot);
            if (haveUsb) {
                // ★ One event per scan however many went: a hub dropping takes its children with it.
                for (const auto& k : lastUsb) if (!cur.count(k)) { tracker.usbLost(now); break; }
            }
            lastUsb.swap(cur); haveUsb = true;
        }
        if (now - lastOcAt >= 10.0) {
            lastOcAt = now;
            const long oc = overCurrentTotal(sysRoot);
            if (oc >= 0 && lastOc >= 0 && oc > lastOc) tracker.overCurrent(now, oc - lastOc);
            if (oc >= 0) lastOc = oc;
        }
    }
    void step(double now) { step(now, readSmall(alarmPath)); }
};
#endif

inline double monoNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** The process's watcher, or nullptr where there is nothing to watch (not a Pi, not Linux).
 *  ★ Started on first call; heap-allocated and never freed, so a detached thread cannot outlive it at
 *    exit. Cheap: one sysfs read a second (a cached value — no firmware call), a readdir per USB bus
 *    every 2 s, the hub ports every 10 s. */
inline Tracker* instance() {
#if defined(__linux__) && !defined(VIBE_POWER_TEST)
    static Tracker* t = []() -> Tracker* {
        Watch* w = new Watch();
        if (!w->find()) { delete w; return nullptr; }
        w->step(monoNow());
        std::thread([w] {
            const int fd = ::open(w->alarmPath.c_str(), O_RDONLY | O_CLOEXEC);
            char b[16];
            for (;;) {
                if (fd >= 0) {
                    // ★ sysfs_notify from hwmon_notify_event wakes this on the edge; otherwise 1 s.
                    pollfd p{fd, POLLPRI | POLLERR, 0};
                    (void)::poll(&p, 1, 1000);
                    const ssize_t n = ::pread(fd, b, sizeof b - 1, 0);
                    w->step(monoNow(), n > 0 ? std::string(b, (size_t)n) : std::string());
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    w->step(monoNow());
                }
            }
        }).detach();
        return &w->tracker;
    }();
    return t;
#else
    return nullptr;
#endif
}

}  // namespace vibepower
