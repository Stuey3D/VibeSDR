// battery_math.h — the Battery block's words and sums (MENU → R36S HARDWARE, sec_hardware.h; the calibration screen,
// battery_cal.h). PURE: no SDL, no App — test/battery_math_test.cpp runs it on the Mac (test/run_battery_math_test.sh).
//
// ★★ WHAT IS MEASURED AND WHAT IS NOT. vibe-hwd sends the battery's own voltage and current (voltage_avg / current_avg,
//    rk817) and the charge port's voltage. The rk817 driver reports NO input current and NO system consumption, so on
//    the charger the console's own use is the last thing that WAS measured — the discharge power from before the cord
//    went in, smoothed (vibe-hwd UseSmoother) — and the charger's current is worked out from it:
//        input A ≈ (the unit's use + the power into the battery) / the port's voltage   (losses not counted)
//    Both are marked ≈ wherever they are shown. Measured figures never are.
// ★ CALIBRATION (vibe-hwd BatteryCal): every unit's cell differs, so the capacity is MEASURED on a discharge from a
//   GENUINE full (vibe-hwd FullJudge — the charger's physics, never the gauge's %) to a safe empty — never a constant
//   here. Until then the % is the gauge's own. Started ON THE CHARGER; the count begins at the unplug.
// ★ The R36S shows these at 40 characters a line: every fixed line here keeps within it (battery_math_test checks).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace hh {

/** vibe-hwd's state.power.charge — is the battery REALLY full (vibe-hwd FullJudge, 2026-11-16). Judged from the
 *  charger's physics (its own CHARGE_FINISH, or the battery's current tapered under the PMIC's termination current
 *  near its CV voltage, held 5 min) — NEVER from the gauge's %. Empty `state` = an older service (no judge). */
struct BattCharge {
    std::string state, by;         // cc · cv · hold · full · paused · off;  by "charger" | "taper"
    double cvV = -1, termA = -1, vlimV = -1;
    bool nearVlim = false, limited = false;
    int holdLeftS = -1;
};
/** vibe-hwd's {"get":"state"}.power (−1 = not reported). batA / batW are magnitudes; `dir` says which way. */
struct BattLive {
    bool known = false, online = false;
    std::string status, dir;       // "Charging" / "Discharging" / "Full" / "Not charging"; "charge" / "discharge"
    double batV = -1, batA = -1, batW = -1, portV = -1, useW = -1;
    int chargeMah = -1, fullMah = -1;
    int useAgeS = -1;              // how long ago the unit's use was last measured (on battery)
    int gaugePct = -1;             // the gauge's own % — shown beside "not full yet", never the judge
    BattCharge charge;
};
/** vibe-hwd's {"get":"state"}.calib. */
struct BattCal {
    std::string phase = "idle", mode = "normal", note;   // idle · charging · armed · running
    bool plugged = true;           // charging: false = the cord came out before full
    int countedMah = -1;
    bool hasResult = false; int mah = 0, design = 0; long long utc = 0; double qEmptyMah = 0;
};

inline std::string battF(const char* fmt, double v) { char b[48]; std::snprintf(b, sizeof b, fmt, v); return b; }
/** Characters (UTF-8 code points) — the R36S's 40-a-line budget (battery_math_test holds every line to it). */
inline size_t battChars(const std::string& s) { size_t n = 0; for (unsigned char ch : s) n += (ch & 0xC0) != 0x80; return n; }

/** ≈ the current the unit itself draws: its smoothed power at the battery's voltage (−1 unknown). */
inline double battUnitA(double useW, double batV) { return useW > 0 && batV > 0 ? useW / batV : -1; }
/** ≈ the charger's current: (the unit's use + the power INTO the battery, negative when it is giving) / the port's
 *  voltage. −1 when the use was never measured or the port reads nothing. */
inline double battInputA(double useW, double intoBatW, double portV) {
    if (useW <= 0 || portV <= 0.5) return -1;
    return std::max(0.0, (useW + intoBatW) / portV);
}
/** ★★ The ≈ input current rests on the unit's use from BEFORE the cord went in — stale once it has been in a while
 *  (Stuart 2026-11-16: ≈1.7 A shown where his bank measured 1.4 A, half an hour after plug-in: the console was doing
 *  less than when it was last measured). Shown only this long after the last measurement. */
constexpr int kUseStaleS = 600;

/** "3 min" / "45 s" */
inline std::string battLeftText(int s) {
    if (s < 0) return "";
    if (s < 60) return std::to_string(s) + " s";
    return std::to_string((s + 30) / 60) + " min";
}

/** The live block — every line within 40 characters.
 *    on battery   "On battery · 4.10 V · 0.80 A · 3.28 W"
 *    on the cord  the charge's progress (battChargeLines) · "Input 4.84 V · ≈1.0 A" · "Unit used ≈0.79 A (3.3 W) on battery"
 *                 · the input-limit hint when the port is near the charger's floor */
inline std::vector<std::string> battLiveLines(const BattLive& b);

/** On the cord: how far the charge has got, judged by vibe-hwd's FullJudge — both numbers while it tops up
 *  (the voltage against CV, the current against the termination current), and NEVER "full" from the gauge's %.
 *    cc     "Charging 4.15 V of 4.20 V · 0.96 A"    + "Full once under 0.20 A near 4.20 V"
 *    cv     "Topping up 4.18 V of 4.20 V · 0.35 A"  + the same
 *    hold   "Topping up 4.19 V of 4.20 V · 0.15 A"  + "Under 0.20 A — full in 3 min"
 *    full   "Full — charge finished · 4.19 V"
 *    paused "On the charger · 4.05 V · not charging" + "The battery is giving 0.10 A"
 *  + "Gauge says 100 % — not full yet" whenever the gauge claims 100 and the charger disagrees. */
inline std::vector<std::string> battChargeLines(const BattLive& b) {
    std::vector<std::string> out;
    const std::string v = battF("%.2f V", b.batV), a = b.batA >= 0 ? battF("%.2f A", b.batA) : std::string("—");
    const BattCharge& c = b.charge;
    const std::string st = !c.state.empty() ? c.state
                         : b.status == "Full" ? "full" : (b.dir == "charge" && b.status == "Charging") ? "cc" : "paused";
    const std::string of = c.cvV > 0 ? v + " of " + battF("%.2f V", c.cvV) : v;
    const std::string rule = c.termA > 0 && c.cvV > 0
        ? "Full once under " + battF("%.2f A", c.termA) + " near " + battF("%.2f V", c.cvV) : std::string();
    if (st == "full") out.push_back("Full — charge finished · " + v);
    else if (st == "paused") {
        out.push_back("On the charger · " + v + " · not charging");
        if (b.batA > 0.005) out.push_back((b.dir == "discharge" ? "The battery is giving " : "The battery is taking ") + a);
    } else if (st == "hold") {
        out.push_back("Topping up " + of + " · " + a);
        out.push_back("Under " + battF("%.2f A", c.termA) + " — full in " + battLeftText(c.holdLeftS));
    } else {
        out.push_back((st == "cv" ? "Topping up " : "Charging ") + of + " · " + a);
        if (!rule.empty()) out.push_back(rule);
    }
    if (st != "full" && b.gaugePct >= 100) out.push_back("Gauge says 100 % — not full yet");
    return out;
}

inline std::vector<std::string> battLiveLines(const BattLive& b) {
    std::vector<std::string> out;
    if (!b.known || b.batV <= 0) return out;
    if (!b.online) {
        std::string l = "On battery · " + battF("%.2f V", b.batV);
        if (b.batA >= 0) l += " · " + battF("%.2f A", b.batA);
        if (b.batW >= 0) l += " · " + battF("%.2f W", b.batW);
        out.push_back(l);
        return out;
    }
    out = battChargeLines(b);
    // the power into the battery (negative: the battery is helping the charger)
    const double into = b.batW > 0 ? (b.dir == "discharge" ? -b.batW : b.batW) : 0;
    const bool fresh = b.useAgeS >= 0 && b.useAgeS <= kUseStaleS;
    std::string in = "Input " + (b.portV > 0 ? battF("%.2f V", b.portV) : std::string("—"));
    const double ia = fresh ? battInputA(b.useW, into, b.portV) : -1, ua = battUnitA(b.useW, b.batV);
    if (ia >= 0) in += " · ≈" + battF("%.1f A", ia);
    out.push_back(in);
    if (ua > 0) out.push_back("Unit used ≈" + battF("%.2f A", ua) + " (" + battF("%.1f W", b.useW) + ") on battery");
    else out.push_back("Unplug a minute to measure the unit's use");
    // ★ the port near the charger's own input floor (VLIM): factual, never "faulty" — a long cable's drop is normal
    if (b.charge.nearVlim && b.charge.vlimV > 0) {
        out.push_back("Input near the charger's " + battF("%.2f V", b.charge.vlimV) + " limit");
        out.push_back("A shorter cable charges faster");
    } else if (b.charge.limited) out.push_back("The charger is held back by its input");
    return out;
}

/** Minutes until `remainingMah` runs out at `amps` (−1 when it cannot be said). */
inline int battEtaMin(double remainingMah, double amps) {
    if (remainingMah <= 0 || amps < 0.02) return -1;
    return (int)std::lround(remainingMah / 1000.0 / amps * 60.0);
}
inline std::string battDurText(int minutes) {
    if (minutes < 0) return "—";
    char b[40]; std::snprintf(b, sizeof b, "%d h %02d min", minutes / 60, minutes % 60);
    return b;
}
/** The charge still to come out before the empty point: past the last calibration's empty when there is one, else
 *  the gauge's own remaining figure. */
inline double battRemainingMah(const BattLive& b, const BattCal& c) {
    if (b.chargeMah < 0) return -1;
    return c.hasResult ? b.chargeMah - c.qEmptyMah : b.chargeMah;
}

/** "15/11/26" — the owner's own date order (DD/MM/YY), the device's local time. */
inline std::string battDateDMY(long long utc) {
    const std::time_t t = (std::time_t)utc;
    std::tm l{}; localtime_r(&t, &l);
    char b[16]; std::snprintf(b, sizeof b, "%02d/%02d/%02d", l.tm_mday, l.tm_mon + 1, l.tm_year % 100);
    return b;
}

/** The calibration's standing lines: the last result, or what the % is based on until there is one. */
inline std::vector<std::string> battCalResultLines(const BattCal& c, int gaugeFullMah) {
    if (c.hasResult && c.mah > 0) {
        std::string l2 = std::to_string(c.mah) + " mAh measured";
        const int d = c.design > 0 ? c.design : gaugeFullMah;
        if (d > 0) l2 += " · " + std::to_string(d) + " design (" + std::to_string((int)std::lround(100.0 * c.mah / d)) + " %)";
        return {"Battery last calibrated " + battDateDMY(c.utc), l2};
    }
    if (gaugeFullMah > 0) return {"Not calibrated — gauge assumes " + std::to_string(gaugeFullMah) + " mAh"};
    return {"Not calibrated — the % is the gauge's own"};
}
/** While a calibration is under way: what to do now (drawn wrapped). ★★ It STARTS ON THE CHARGER (2026-11-16): it waits
 *  for a genuine full charge (vibe-hwd FullJudge — not the gauge's 100 %), then asks for the unplug; the count begins
 *  at the unplug. Pulled out before full, nothing starts and it says so. */
inline std::string battCalPhaseLine(const BattCal& c) {
    if (c.phase == "charging")
        return c.plugged ? std::string("Calibration: waiting for a full charge — leave the charger in. You will be asked to unplug.")
                         : std::string("Not full yet — plug the charger back in to carry on, or CANCEL.");
    if (c.phase == "armed") return "Full — unplug the charger to start the calibration";
    if (c.phase == "running") {
        std::string l = c.mode == "stress" ? "Calibrating fast" : "Calibrating: use the radio as normal until it switches itself off";
        if (c.countedMah >= 0) l += " — " + std::to_string(c.countedMah) + " mAh counted";
        return l + ". Charging before then cancels it.";
    }
    return "";
}

}  // namespace hh
