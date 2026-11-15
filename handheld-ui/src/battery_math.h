// battery_math.h — the Battery block's words and sums (MENU → R36S HARDWARE, sec_hardware.h; the calibration screen,
// battery_cal.h). PURE: no SDL, no App — test/battery_math_test.cpp runs it on the Mac (test/run_battery_math_test.sh).
//
// ★★ WHAT IS MEASURED AND WHAT IS NOT. vibe-hwd sends the battery's own voltage and current (voltage_avg / current_avg,
//    rk817) and the charge port's voltage. The rk817 driver reports NO input current and NO system consumption, so on
//    the charger the console's own use is the last thing that WAS measured — the discharge power from before the cord
//    went in, smoothed (vibe-hwd UseSmoother) — and the charger's current is worked out from it:
//        input A ≈ (the unit's use + the power into the battery) / the port's voltage   (losses not counted)
//    Both are marked ≈ wherever they are shown. Measured figures never are.
// ★ CALIBRATION (vibe-hwd BatteryCal): every unit's cell differs, so the capacity is MEASURED on a discharge from Full
//   to a safe empty — never a constant here. Until then the % is the gauge's own.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace hh {

/** vibe-hwd's {"get":"state"}.power (−1 = not reported). batA / batW are magnitudes; `dir` says which way. */
struct BattLive {
    bool known = false, online = false;
    std::string status, dir;       // "Charging" / "Discharging" / "Full" / "Not charging"; "charge" / "discharge"
    double batV = -1, batA = -1, batW = -1, portV = -1, useW = -1;
    int chargeMah = -1, fullMah = -1;
};
/** vibe-hwd's {"get":"state"}.calib. */
struct BattCal {
    std::string phase = "idle", mode = "normal", note;
    int countedMah = -1;
    bool hasResult = false; int mah = 0, design = 0; long long utc = 0; double qEmptyMah = 0;
};

inline std::string battF(const char* fmt, double v) { char b[48]; std::snprintf(b, sizeof b, fmt, v); return b; }

/** ≈ the current the unit itself draws: its smoothed power at the battery's voltage (−1 unknown). */
inline double battUnitA(double useW, double batV) { return useW > 0 && batV > 0 ? useW / batV : -1; }
/** ≈ the charger's current: (the unit's use + the power INTO the battery, negative when it is giving) / the port's
 *  voltage. −1 when the use was never measured or the port reads nothing. */
inline double battInputA(double useW, double intoBatW, double portV) {
    if (useW <= 0 || portV <= 0.5) return -1;
    return std::max(0.0, (useW + intoBatW) / portV);
}

/** The live block: one line on battery, two on the charger.
 *    on battery   "Battery  Discharging  4.10 V  0.80 A  3.28 W"
 *    charging     "Battery  4.17 V  Charging at 0.28 A"   +  "Input  4.84 V  ≈1.0 A   Unit using ≈0.80 A  3.3 W"
 *    full         "Battery  Full  4.20 V"                 +  the same Input line */
inline std::vector<std::string> battLiveLines(const BattLive& b) {
    std::vector<std::string> out;
    if (!b.known || b.batV <= 0) return out;
    const std::string v = battF("%.2f V", b.batV);
    if (!b.online) {
        std::string l = "Battery  " + (b.status.empty() || b.status == "Unknown" ? std::string("Discharging") : b.status) + "  " + v;
        if (b.batA >= 0) l += "  " + battF("%.2f A", b.batA);
        if (b.batW >= 0) l += "  " + battF("%.2f W", b.batW);
        out.push_back(l);
        return out;
    }
    double into = 0;   // the power into the battery (negative: the battery is helping the charger)
    if (b.status == "Full") out.push_back("Battery  Full  " + v);
    else if (b.dir == "charge" && b.status == "Charging") {
        out.push_back("Battery  " + v + "  Charging at " + battF("%.2f A", std::max(0.0, b.batA)));
        into = b.batW > 0 ? b.batW : 0;
    } else {
        // ★ on the cord but not charging (the LED's flicker): the battery may even be giving — say which way
        std::string l = "Battery  " + v + "  " + (b.status.empty() ? std::string("Not charging") : b.status);
        if (b.batA > 0.005) l += (b.dir == "discharge" ? "  giving " : "  taking ") + battF("%.2f A", b.batA);
        out.push_back(l);
        into = b.batW > 0 ? (b.dir == "discharge" ? -b.batW : b.batW) : 0;
    }
    std::string in = "Input  " + (b.portV > 0 ? battF("%.2f V", b.portV) : std::string("—"));
    const double ia = battInputA(b.useW, into, b.portV), ua = battUnitA(b.useW, b.batV);
    if (ia >= 0) in += "  ≈" + battF("%.1f A", ia);
    if (ua > 0) in += "   Unit using ≈" + battF("%.2f A", ua) + "  " + battF("%.1f W", b.useW);
    else in += "   Unit's own use: unplug for a minute to measure it";
    out.push_back(in);
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
    if (gaugeFullMah > 0) return {"Never calibrated — the % is the gauge's own, on " + std::to_string(gaugeFullMah) + " mAh"};
    return {"Never calibrated — the % is the gauge's own"};
}
/** While a calibration is under way: what to do now. */
inline std::string battCalPhaseLine(const BattCal& c) {
    if (c.phase == "armed") return "Calibrating: unplug the charger to start";
    if (c.phase == "running") {
        std::string l = c.mode == "stress" ? "Calibrating fast" : "Calibrating: use the radio as normal until it switches itself off";
        if (c.countedMah >= 0) l += " — " + std::to_string(c.countedMah) + " mAh counted";
        return l + ". Charging before then cancels it.";
    }
    return "";
}

}  // namespace hh
