// battery_math_test.cpp — battery_math.h (the R36S Battery block's words and sums): the live lines on battery / on the
// charger / full, the ≈ input and unit figures, the ETA, the DD/MM/YY date, the calibration result line.
// Run: test/run_battery_math_test.sh (native, the Mac).
#include "../src/battery_math.h"
#include <cstdio>
#include <cstdlib>

using namespace hh;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)
#define EQ(got, want) CHECK((got) == (want), "got [%s] want [%s]", std::string(got).c_str(), std::string(want).c_str())

int main() {
    setenv("TZ", "UTC", 1); tzset();
    BattLive b; b.known = true;
    // ── on battery ──
    b.online = false; b.status = "Discharging"; b.dir = "discharge"; b.batV = 4.10; b.batA = 0.80; b.batW = 3.28; b.useW = 3.28;
    auto L = battLiveLines(b);
    CHECK(L.size() == 1, "one line on battery (%zu)", L.size());
    EQ(L[0], "Battery  Discharging  4.10 V  0.80 A  3.28 W");
    // ── charging: the unit's use is the remembered discharge power; the input current worked out from it ──
    b.online = true; b.status = "Charging"; b.dir = "charge"; b.batV = 4.17; b.batA = 0.28; b.batW = 4.17 * 0.28; b.portV = 4.84; b.useW = 3.3;
    L = battLiveLines(b);
    CHECK(L.size() == 2, "two lines on the charger (%zu)", L.size());
    EQ(L[0], "Battery  4.17 V  Charging at 0.28 A");
    // (3.3 + 1.1676) / 4.84 = 0.92 → ≈0.9 A; 3.3 / 4.17 = 0.79 A
    EQ(L[1], "Input  4.84 V  ≈0.9 A   Unit using ≈0.79 A  3.3 W");
    CHECK(std::fabs(battInputA(3.3, 1.1676, 4.84) - 0.923) < 0.001, "input ≈ (use + charge power) / port V");
    CHECK(battInputA(-1, 1, 4.8) < 0 && battInputA(3, 1, 0) < 0, "no input figure without a measured use or a port voltage");
    // ── full ──
    b.status = "Full"; b.batV = 4.20; b.batA = 0; b.batW = 0;
    L = battLiveLines(b);
    EQ(L[0], "Battery  Full  4.20 V");
    EQ(L[1], "Input  4.84 V  ≈0.7 A   Unit using ≈0.79 A  3.3 W");
    // ── on the cord, not charging, the battery helping (the LED symptom) ──
    b.status = "Not charging"; b.dir = "discharge"; b.batV = 4.05; b.batA = 0.10; b.batW = 0.405;
    L = battLiveLines(b);
    EQ(L[0], "Battery  4.05 V  Not charging  giving 0.10 A");
    // (3.3 − 0.405) / 4.84 = 0.598 → ≈0.6 A
    EQ(L[1], "Input  4.84 V  ≈0.6 A   Unit using ≈0.81 A  3.3 W");
    // ── never measured off the cord ──
    b.useW = -1; b.status = "Charging"; b.dir = "charge"; b.batA = 0.5; b.batW = 2;
    L = battLiveLines(b);
    EQ(L[1], "Input  4.84 V   Unit's own use: unplug for a minute to measure it");
    BattLive none; CHECK(battLiveLines(none).empty(), "nothing known: no lines");
    // ── ETA ──
    CHECK(battEtaMin(2000, 1.0) == 120, "2000 mAh at 1 A = 120 min");
    CHECK(battEtaMin(100, 0.01) == -1 && battEtaMin(-5, 1) == -1, "no ETA on no current / nothing left");
    EQ(battDurText(125), "2 h 05 min");
    BattCal c;
    BattLive r; r.chargeMah = 1500;
    CHECK(battRemainingMah(r, c) == 1500, "uncalibrated: the gauge's remaining");
    c.hasResult = true; c.qEmptyMah = 188;
    CHECK(battRemainingMah(r, c) == 1312, "calibrated: down to this cell's own empty");
    // ── the date and the result ──
    EQ(battDateDMY(1794700800), "15/11/26");   // 2026-11-15 00:00 UTC
    c.mah = 2620; c.design = 2808; c.utc = 1794700800;
    auto R = battCalResultLines(c, 2808);
    CHECK(R.size() == 2, "two result lines");
    EQ(R[0], "Battery last calibrated 15/11/26");
    EQ(R[1], "2620 mAh measured · 2808 design (93 %)");
    BattCal n0;
    EQ(battCalResultLines(n0, 2808)[0], "Never calibrated — the % is the gauge's own, on 2808 mAh");
    // ── under way ──
    BattCal run; run.phase = "running"; run.countedMah = 812;
    EQ(battCalPhaseLine(run), "Calibrating: use the radio as normal until it switches itself off — 812 mAh counted. Charging before then cancels it.");
    run.phase = "armed"; EQ(battCalPhaseLine(run), "Calibrating: unplug the charger to start");
    run.phase = "idle"; EQ(battCalPhaseLine(run), "");
    std::printf("battery_math_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
