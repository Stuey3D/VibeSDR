// test-power-dips.cpp — the POWER card's verdict (vibe_power.h): brief dips amber, serious ones red.
//
// ★ Stuart's Pi 500, 2026-10-16: the supply sags for 1-2 s many times an hour under bursty load, with
//   clean audio and no USB dropouts — and the card went RED every time the page polled inside one.
//   These are the cases that decide the colour, first as the pure classification, then end to end
//   through a FAKE /sys and /dev (the kernel's rpi_volt alarm toggled by hand, USB devices removed).
#define VIBE_POWER_TEST 1
#include "vibe_power.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace vibepower;
static int fails = 0;
static void ok(bool c, const char* what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what); if (!c) fails++; }

/** The kernel's alarm as the watcher sees it: a 1-2 s dip is reported for ONE 2-s poll window, and
 *  sampled once a second that is two readings of 1. */
static void dip(Tracker& t, double at, double readSecs) {
    t.alarm(at - 1, false);
    for (double s = 0; s < readSecs; s += 1) t.alarm(at + s, true);
    t.alarm(at + readSecs, false);
}

static std::string g_root;
static void put(const std::string& rel, const std::string& v) {
    fs::create_directories(fs::path(g_root + rel).parent_path());
    std::ofstream(g_root + rel) << v << "\n";
}

int main() {
    std::printf("── classification (pure) ──\n");
    {
        Summary s;
        ok(classify(s) == Verdict::Ok, "nothing ever: OK");
        s.ever = true;
        ok(classify(s) == Verdict::Dipped, "a dip since start but none this hour: DIPPED (amber, the quiet case kept)");
        s.dipsHour = 25; s.longestHourSec = 4; s.recentSec = 8;
        ok(classify(s) == Verdict::Brief, "25 short dips this hour, longest 4 s: BRIEF (amber)");
        ok(std::string(verdictStatus(Verdict::Brief)) == "warning", "BRIEF is a warning, not critical");
        s.now = true; s.nowSec = 2;
        ok(classify(s) == Verdict::Brief, "polled INSIDE a 2 s dip: still BRIEF — the instant no longer decides");
        s.nowSec = 10;
        ok(classify(s) == Verdict::Sustained, "the open dip reaches 10 s: SUSTAINED (red)");
        s.now = false; s.nowSec = 0; s.longestHourSec = 12;
        ok(classify(s) == Verdict::Sustained, "a 12 s dip earlier this hour: still SUSTAINED");
        s.longestHourSec = 4; s.recentSec = 150;
        ok(classify(s) == Verdict::Frequent, "low for half of the last 5 minutes in short dips: FREQUENT (red)");
        s.recentSec = 149;
        ok(classify(s) == Verdict::Brief, "just under half: BRIEF");
        s.overCurrentHour = 1;
        ok(classify(s) == Verdict::OverCurrent, "a USB over-current event: OVERCURRENT (red)");
        s.usbLostHour = 1;
        ok(classify(s) == Verdict::UsbLost, "a USB device lost in a dip outranks everything: USBLOST");
        ok(std::string(verdictStatus(Verdict::UsbLost)) == "critical", "USBLOST is critical");
    }

    std::printf("── episodes ──\n");
    {
        Tracker t;
        // Stuart's Pi 500: 1-2 s sags every couple of minutes for an hour.
        for (int i = 0; i < 30; i++) dip(t, 100 + i * 120, 2);
        const double now = 100 + 30 * 120;
        Summary s = t.summary(now);
        ok(s.dipsHour == 30, "30 dips counted as 30 episodes (rising edges, not readings)");
        ok(s.longestHourSec <= 2.01, "longest is the 2-s reading, not the sum");
        ok(s.verdict == Verdict::Brief, "Pi 500 pattern: BRIEF, amber");
        ok(!s.now && s.nowSec == 0, "not dipping now");
        Summary later = t.summary(now + 2 * kHourSec);
        ok(later.dipsHour == 0 && later.verdict == Verdict::Dipped, "two hours later: no dips this hour, DIPPED");
    }
    {
        Tracker t;
        t.alarm(0, false);
        for (int s = 1; s <= 11; s++) t.alarm(s, true);   // held low 1..11 s
        Summary s = t.summary(11);
        ok(s.now && s.nowSec >= 10, "an open dip of 10 s is reported as it runs");
        ok(s.verdict == Verdict::Sustained, "SUSTAINED while still low");
        t.alarm(12, false);
        ok(t.summary(600).verdict == Verdict::Sustained, "and stays red for the hour after it ends");
        ok(t.summary(12 + kHourSec + 5).verdict == Verdict::Dipped, "an hour on, back to DIPPED");
    }
    {
        Tracker t;
        // One second off between 3-s readings for 5 minutes: low 75 % of the time, no single dip ≥ 10 s.
        double at = 0;
        while (at < 300) { dip(t, at + 1, 3); at += 4; }
        Summary s = t.summary(300);
        ok(s.longestHourSec < kSustainedSec, "no single dip reaches 10 s");
        ok(s.verdict == Verdict::Frequent, "but effectively continuous: FREQUENT");
    }
    {
        Tracker t;
        dip(t, 1000, 2);
        t.usbLost(1003);                       // the scan noticed a device gone 1 s after the reading cleared
        ok(t.summary(1100).verdict == Verdict::UsbLost, "a USB device lost during/just after a dip: USBLOST");
        Tracker u;
        dip(u, 1000, 2);
        u.usbLost(990);                        // the scan can see the loss before the 2-s alarm does
        ok(u.summary(1100).usbLostHour == 1, "a loss just BEFORE the alarm rises still counts");
        Tracker v;
        dip(v, 1000, 2);
        v.usbLost(1500);                       // somebody unplugged a dongle on a healthy supply
        ok(v.summary(1600).verdict == Verdict::Brief, "a USB unplug far from any dip is NOT blamed on the supply");
    }
    {
        Tracker t;
        auto r = t.recent(100);
        ok(r.empty(), "no dips: no marks");
        dip(t, 50, 2);
        r = t.recent(100);
        ok(r.size() == 1 && r[0].first == 50 && r[0].second == 48, "a mark is [ago start, ago end]");
    }

    std::printf("── through a fake /sys and /dev ──\n");
    {
        char tmpl[] = "/tmp/vibe-power-XXXXXX";
        g_root = mkdtemp(tmpl);
        put("/sys/class/hwmon/hwmon0/name", "cpu_thermal");
        put("/sys/class/hwmon/hwmon2/name", "rpi_volt");
        put("/sys/class/hwmon/hwmon2/in0_lcrit_alarm", "0");
        put("/dev/bus/usb/001/001", "");
        put("/dev/bus/usb/001/002", "");
        put("/dev/bus/usb/001/003", "");        // the dongle
        put("/sys/bus/usb/devices/1-0:1.0/usb1-port1/over_current_count", "0");
        put("/sys/bus/usb/devices/1-1:1.0/1-1-port1/over_current_count", "0");
        put("/sys/bus/usb/devices/1-1:1.0/1-1-port2/over_current_count", "0");

        Watch w; w.sysRoot = g_root + "/sys"; w.devRoot = g_root + "/dev";
        ok(w.find() && w.alarmPath.find("hwmon2/in0_lcrit_alarm") != std::string::npos, "found rpi_volt by NAME (hwmon2)");
        ok(usbDevices(w.devRoot).size() == 3, "three USB devices enumerated");
        ok(overCurrentTotal(w.sysRoot) == 0, "over-current ports read (0)");

        double now = 0;
        auto tick = [&](int secs) { for (int i = 0; i < secs; i++) w.step(now += 1); };
        tick(5);
        put("/sys/class/hwmon/hwmon2/in0_lcrit_alarm", "1"); tick(2);   // a brief dip
        put("/sys/class/hwmon/hwmon2/in0_lcrit_alarm", "0"); tick(30);
        Summary s = w.tracker.summary(now);
        ok(s.dipsHour == 1 && s.verdict == Verdict::Brief, "one 2-s alarm edge: one BRIEF dip");

        put("/sys/class/hwmon/hwmon2/in0_lcrit_alarm", "1"); tick(2);
        fs::remove(g_root + "/dev/bus/usb/001/003");                    // the dongle drops in the dip …
        put("/dev/bus/usb/001/004", "");                                // … and comes back as device 4
        tick(2);
        put("/sys/class/hwmon/hwmon2/in0_lcrit_alarm", "0"); tick(5);
        s = w.tracker.summary(now);
        ok(s.dipsHour == 2, "second dip counted");
        ok(s.usbLostHour == 1 && s.verdict == Verdict::UsbLost, "the dongle re-enumerating in the dip: USBLOST (red)");

        Watch oc; oc.sysRoot = g_root + "/sys"; oc.devRoot = g_root + "/dev";
        oc.find();
        now = 0; double t2 = 0;
        for (int i = 0; i < 12; i++) oc.step(t2 += 1);
        put("/sys/bus/usb/devices/1-1:1.0/1-1-port2/over_current_count", "3");
        for (int i = 0; i < 12; i++) oc.step(t2 += 1);
        Summary o = oc.tracker.summary(t2);
        ok(o.overCurrentHour == 3 && o.verdict == Verdict::OverCurrent, "a hub port's over_current_count rose by 3: OVERCURRENT");

        Watch none; none.sysRoot = g_root + "/nowhere";
        ok(!none.find(), "no rpi_volt: nothing to watch (every non-Pi)");
        fs::remove_all(g_root);
    }

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
