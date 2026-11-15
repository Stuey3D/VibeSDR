// battery_cal.h — the STRESS battery calibration's own screen ("Battery calibration mode"), drawn in place of the radio
// while vibe-hwd runs a stress calibration (MENU → R36S HARDWARE → CALIBRATE → STRESS; vibe-hwd BatteryCal / Stress).
// Wire-up (app_impl.h's loop, the deck pass): `if (calScreenUp()) calDrawScreen(*this, c); else …`.
//
// ★★ WHY A SCREEN OF ITS OWN (Stuart, 2026-11-15): STRESS is the fastest way from Full to a safe empty — the service
//    holds the backlight at full, the CPU at the image's top step and every core busy, the speaker muted. The radio
//    keeps running underneath, but the waterfall is not drawn: what matters for the next hour is the battery, so the
//    page says what it is doing — live V / A / W, the charge counted so far and an ETA (the charge left to the empty
//    point / the smoothed current) — and how to stop it: HOLD POWER (the normal power-off; the next start is the
//    ordinary radio, still counting). Charging before empty cancels the run, and the screen goes with it.
// ★ Before the unplug (armed) BACK, or CANCEL, calls it off — nothing has been measured yet.
#pragma once
// (included by app.h after App is declared)
#include "battery_math.h"
#include "hw_state.h"

namespace hh {

/** A stress calibration is armed or running (the service's state, read within the last 30 s). */
inline bool calScreenUp() {
    HwShared& h = hwShared();
    std::lock_guard<std::mutex> lk(h.m);
    return h.cal.mode == "stress" && (h.cal.phase == "armed" || h.cal.phase == "running") && hwNow() - h.batAt < 30;
}

inline void calDrawScreen(App& a, Ctx& c) {
    // ★ started from the menu: the sheets close, so nothing is drawn over this page and the pad is its own
    while (!a.sheets_.empty()) a.closeTop();
    static double lastPoll = 0;
    if (nowSec() - lastPoll >= 1.0 && a.eng_) { lastPoll = nowSec(); hwRefreshDevice(a.eng_->port()); }
    BattLive b; BattCal k; bool busy;
    { HwShared& h = hwShared(); std::lock_guard<std::mutex> lk(h.m); b = h.bat; k = h.cal; busy = h.calBusy; }
    const float W = (float)a.W, H = (float)a.H;
    c.g.rect(0, 0, W, H, th::bg);
    c.tc.draw(c.g, c.f.title, "Battery calibration mode", W / 2, 22, th::btnText, 1);
    c.g.rect(40, 58, W - 80, 1, th::barBorder);
    if (k.phase == "armed") {
        c.tc.draw(c.g, c.f.big, "Unplug the charger to start", W / 2, 150, th::amber, 1);
        float y = 214;
        for (auto& l : c.tc.wrap(c.f.body, "Then the screen stays at full brightness and every core is kept busy until the battery reaches a safe "
                                           "empty; the radio then switches itself off and the measured capacity is kept.", (int)W - 100)) {
            c.tc.draw(c.g, c.f.body, l, W / 2, y, th::white68, 1); y += c.f.body.lineHeight();
        }
        BtnStyle st; st.font = &c.f.btn; st.disabled = busy;
        if (button(c, "cal.cancel", {W / 2 - 110, H - 96, 220, 52}, "CANCEL", st) || c.nav.backPressed()) hwCalibrate(a.eng_ ? a.eng_->port() : 0, "");
        return;
    }
    // running: the figures, big
    const double unitA = battUnitA(b.useW, b.batV);
    const double amps = unitA > 0 ? unitA : b.batA;      // ★ the smoothed figure for the ETA; the measured one shown
    const float cy = 130, colW = (W - 80) / 3;
    const std::string figs[3] = {b.batV > 0 ? battF("%.2f V", b.batV) : "—", b.batA >= 0 ? battF("%.2f A", b.batA) : "—",
                                 b.batW >= 0 ? battF("%.2f W", b.batW) : "—"};
    const char* names[3] = {"BATTERY", "CURRENT", "POWER"};
    for (int i = 0; i < 3; i++) {
        const float x = 40 + colW * i + colW / 2;
        c.tc.draw(c.g, c.f.small, names[i], x, cy - 34, th::dim, 1);
        c.tc.draw(c.g, c.f.freq, figs[i], x, cy, th::white, 1);
    }
    float y = 214;
    const int eta = battEtaMin(battRemainingMah(b, k), amps);
    c.tc.draw(c.g, c.f.title, eta >= 0 ? "About " + battDurText(eta) + " to empty" : "Working out the time to empty…", W / 2, y, th::amber, 1);
    y += c.f.title.lineHeight() + 10;
    if (k.countedMah >= 0) { c.tc.draw(c.g, c.f.body, std::to_string(k.countedMah) + " mAh counted since Full", W / 2, y, th::white68, 1); y += c.f.body.lineHeight(); }
    if (b.online) { c.tc.draw(c.g, c.f.body, "The charger is in — the calibration will be cancelled", W / 2, y, th::amber, 1); y += c.f.body.lineHeight(); }
    y = H - 110;
    for (auto& l : c.tc.wrap(c.f.body, "It switches itself off at a safe empty and keeps the result. HOLD POWER to stop it — the next start is "
                                       "the ordinary radio, still counting. Plugging in the charger cancels it.", (int)W - 80)) {
        c.tc.draw(c.g, c.f.body, l, W / 2, y, th::dim, 1); y += c.f.body.lineHeight();
    }
    c.nav.item("cal.screen", {0, 0, W, H});   // ★ the pad's presses land here, on nothing that does anything
    c.nav.backPressed();                      //   and BACK does not reach the radio underneath
}

}  // namespace hh
