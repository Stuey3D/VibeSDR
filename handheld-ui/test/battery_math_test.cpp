// battery_math_test.cpp — battery_math.h (the R36S Battery block's words and sums): the live lines on battery / on the
// charger, the charge's progress judged by vibe-hwd's FullJudge (2026-11-16: "100 %" from the gauge is never "full"),
// the ≈ input and unit figures (dropped when stale), the input-limit hint, the ETA, the DD/MM/YY date, the calibration
// lines (started on the charger: waiting / not full yet / unplug / counting). GOLDEN: every screen state's exact text,
// and every line within the R36S's 40 characters.
// Run: test/run_battery_math_test.sh (native, the Mac).
#include "../src/battery_math.h"
#include <cstdio>
#include <cstdlib>

using namespace hh;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)
#define EQ(got, want) CHECK((got) == (want), "got [%s] want [%s]", std::string(got).c_str(), std::string(want).c_str())
/** The whole block, golden: these lines, in this order, each within 40 characters. */
static void GOLD(const std::vector<std::string>& got, const std::vector<std::string>& want, const char* what) {
    bool same = got == want;
    CHECK(same, "%s: %zu lines, want %zu", what, got.size(), want.size());
    if (!same) for (size_t i = 0; i < std::max(got.size(), want.size()); i++)
        std::fprintf(stderr, "   [%s] | [%s]\n", i < got.size() ? got[i].c_str() : "", i < want.size() ? want[i].c_str() : "");
    for (auto& l : got) CHECK(battChars(l) <= 40, "%s: over 40 characters (%zu): [%s]", what, battChars(l), l.c_str());
}

int main() {
    setenv("TZ", "UTC", 1); tzset();
    BattLive b; b.known = true;
    // ── on battery ──
    b.online = false; b.status = "Discharging"; b.dir = "discharge"; b.batV = 4.10; b.batA = 0.80; b.batW = 3.28; b.useW = 3.28; b.useAgeS = 0;
    GOLD(battLiveLines(b), {"On battery · 4.10 V · 0.80 A · 3.28 W"}, "on battery");
    // ── ★★ Stuart's unit, 2026-11-16: the gauge 100 %, 4.15 V, 0.96 A in, port 4.63 V (bank 5.1 V), half an hour in ──
    b.online = true; b.status = "Charging"; b.dir = "charge"; b.batV = 4.15; b.batA = 0.96; b.batW = 4.15 * 0.96; b.portV = 4.63;
    b.useW = 4.0; b.useAgeS = 1800; b.gaugePct = 100;
    b.charge.state = "cc"; b.charge.cvV = 4.20; b.charge.termA = 0.20; b.charge.vlimV = 4.50; b.charge.nearVlim = true;
    GOLD(battLiveLines(b), {"Charging 4.15 V of 4.20 V · 0.96 A", "Full once under 0.20 A near 4.20 V",
                            "Gauge says 100 % — not full yet", "Input 4.63 V",
                            "Unit used ≈0.96 A (4.0 W) on battery",
                            "Input near the charger's 4.50 V limit", "A shorter cable charges faster"},
         "★★ gauge 100 % with 0.96 A going in: charging, NOT full; the stale ≈ input dropped; the input-limit hint");
    // fresh (just plugged in): the ≈ input is shown — (use + charge power) / port V
    b.useAgeS = 30; b.gaugePct = 97; b.charge.nearVlim = false; b.portV = 4.95;
    GOLD(battLiveLines(b), {"Charging 4.15 V of 4.20 V · 0.96 A", "Full once under 0.20 A near 4.20 V", "Input 4.95 V · ≈1.6 A",
                            "Unit used ≈0.96 A (4.0 W) on battery"}, "just plugged in: the ≈ input (use still fresh)");
    CHECK(std::fabs(battInputA(3.3, 1.1676, 4.84) - 0.923) < 0.001, "input ≈ (use + charge power) / port V");
    CHECK(battInputA(-1, 1, 4.8) < 0 && battInputA(3, 1, 0) < 0, "no input figure without a measured use or a port voltage");
    // ── topping up (CV), the hold, full ──
    b.charge.state = "cv"; b.batV = 4.18; b.batA = 0.35; b.batW = 4.18 * 0.35; b.useAgeS = 1200;
    GOLD(battLiveLines(b), {"Topping up 4.18 V of 4.20 V · 0.35 A", "Full once under 0.20 A near 4.20 V", "Input 4.95 V",
                            "Unit used ≈0.96 A (4.0 W) on battery"}, "topping up (CV)");
    b.charge.state = "hold"; b.batV = 4.19; b.batA = 0.15; b.charge.holdLeftS = 170;
    EQ(battChargeLines(b)[0], "Topping up 4.19 V of 4.20 V · 0.15 A");
    EQ(battChargeLines(b)[1], "Under 0.20 A — full in 3 min");
    b.charge.holdLeftS = 40; EQ(battChargeLines(b)[1], "Under 0.20 A — full in 40 s");
    b.charge.state = "full"; b.charge.by = "taper"; b.gaugePct = 100; b.batA = 0.12;
    GOLD(battChargeLines(b), {"Full — charge finished · 4.19 V"}, "★ full (by the taper): no 'not full yet', whatever the gauge");
    // ── on the cord, not charging, the battery helping (the LED symptom) ──
    b.charge.state = "paused"; b.status = "Not charging"; b.dir = "discharge"; b.batV = 4.05; b.batA = 0.10; b.batW = 0.405; b.gaugePct = 88;
    GOLD(battChargeLines(b), {"On the charger · 4.05 V · not charging", "The battery is giving 0.10 A"}, "paused");
    // the PMIC's input clamp without the port near its floor
    b.charge.limited = true; b.useW = -1; b.useAgeS = -1;
    auto L = battLiveLines(b);
    EQ(L.back(), "The charger is held back by its input");
    EQ(L[L.size() - 2], "Unplug a minute to measure the unit's use");
    // ── an older service (no judge): the status decides, never the gauge ──
    BattLive o; o.known = true; o.online = true; o.status = "Charging"; o.dir = "charge"; o.batV = 4.17; o.batA = 0.28; o.gaugePct = 100;
    EQ(battChargeLines(o)[0], "Charging 4.17 V · 0.28 A");
    EQ(battChargeLines(o)[1], "Gauge says 100 % — not full yet");
    o.status = "Full"; EQ(battChargeLines(o)[0], "Full — charge finished · 4.17 V");
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
    GOLD(battCalResultLines(c, 2808), {"Battery last calibrated 15/11/26", "2620 mAh measured · 2808 design (93 %)"}, "calibrated");
    BattCal n0;
    GOLD(battCalResultLines(n0, 2886), {"Not calibrated — gauge assumes 2886 mAh"}, "never calibrated");
    // ── the calibration, started on the charger ──
    BattCal run; run.phase = "charging";
    EQ(battCalPhaseLine(run), "Calibration: waiting for a full charge — leave the charger in. You will be asked to unplug.");
    run.plugged = false;
    EQ(battCalPhaseLine(run), "Not full yet — plug the charger back in to carry on, or CANCEL.");
    run.phase = "armed"; run.plugged = true; EQ(battCalPhaseLine(run), "Full — unplug the charger to start the calibration");
    run.phase = "running"; run.countedMah = 812;
    EQ(battCalPhaseLine(run), "Calibrating: use the radio as normal until it switches itself off — 812 mAh counted. Charging before then cancels it.");
    run.phase = "idle"; EQ(battCalPhaseLine(run), "");
    std::printf("battery_math_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
