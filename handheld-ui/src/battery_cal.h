// battery_cal.h — the STRESS battery calibration's own screen ("Battery calibration mode"), drawn in place of the radio
// while vibe-hwd runs a stress calibration (MENU → R36S HARDWARE → CALIBRATE → STRESS; vibe-hwd BatteryCal / Stress).
// Wire-up (app_impl.h's loop, the deck pass): `if (calScreenUp()) calDrawScreen(*this, c); else …`.
//
// ★★ WHY A SCREEN OF ITS OWN (Stuart, 2026-11-15): STRESS is the fastest way from Full to a safe empty — the service
//    holds the backlight at full, the CPU at the image's top step and every core busy, the speaker muted. ★★ The radio
//    is part of the load (Stuart's design, built h80 — StressSdr below): while the run is RUNNING this UI puts it on
//    the top sample rate the engine offers that radio, decoding and streaming continuously, and puts its own rate back
//    when the run ends or is cancelled. The waterfall is not drawn: what matters for the next hour is the battery, so the
//    page says what it is doing — live V / A / W, the charge counted so far and an ETA (the charge left to the empty
//    point / the smoothed current) — and how to stop it: HOLD POWER (the normal power-off; the next start is the
//    ordinary radio, still counting). Charging before empty cancels the run, and the screen goes with it.
// ★★ STARTED ON THE CHARGER (2026-11-16): first this page waits for a GENUINE full charge (vibe-hwd FullJudge — the
//    charger's physics, never the gauge's 100 %) and shows how far the charge has got; NO stress load yet (it would only
//    slow the charge). Then "Full — unplug the charger to start"; the load and the count begin at the unplug. Pulled
//    out before full: nothing starts, the page says so.
// ★ Before the unplug (charging / armed) BACK, or CANCEL, calls it off — nothing has been measured yet.
#pragma once
// (included by app.h after App is declared)
#include "battery_math.h"
#include "hw_state.h"

namespace hh {

// ── STRESS: the radio as part of the load (h80, 2026-11-19) ─────────────────────────────────────────────────────────
// ★★ Stuart 2026-11-15: "SDR at max sample rate with audio muted and no waterfall". While a STRESS run is RUNNING (after
//    the unplug) the radio goes to the TOP rate the engine offers it (hwinfo rates, under the server's lockedRate; on an
//    RTL that is the engine's top step, the same list the SAMPLE RATE row offers), keeps decoding and streaming (vibe-hwd
//    mutes the speaker; this screen draws no waterfall), and its previous rate goes back when the run ends, is
//    cancelled, or the console switches off. ★ Per-radio memory saves the PREVIOUS rate while the run drives it
//    (memory.h memBody), so a HOLD-POWER stop or the empty power-off never leaves the radio on the stress rate.
//    ★ Where the rate is not this UI's to change (DAB, a decoder mode that runs at the rate it found, an HF+, a server
//    lock) the radio stays where it is and the screen says why.
struct StressSdr {
    bool active = false;            // the run is driving the radio
    std::string driver;             // the radio it is driving (a different one plugged mid-run starts afresh)
    double savedRate = 0;           // what it ran before (0 = nothing to put back)
    double sentRate = 0, sentAt = 0;
    std::string why;                // why it is not on the top rate ("" = it is, or it is on its way)
};
inline StressSdr& stressSdr() { static StressSdr k; return k; }

inline std::string stressRateText(double r) {
    char b[32];
    if (r >= 1e6) { std::snprintf(b, sizeof b, "%.3f", r / 1e6); std::string t = b; while (t.back() == '0') t.pop_back(); if (t.back() == '.') t.pop_back(); return t + " MS/s"; }
    std::snprintf(b, sizeof b, "%.0f kS/s", r / 1e3); return b;
}

/** What the radio runs now: hwinfo rateNow, or config's full span when it names one of the offered rates (as rateRow). */
inline double stressRateNow(const RadioState& s) {
    double r = s.rateNow;
    for (double x : s.rates) if (s.maxSpanHz > 0 && std::fabs(x - s.maxSpanHz) < x * 0.001) r = x;
    return r;
}

/** The top rate this run may put the radio on, or 0 with `why` when it is not this UI's to change. */
inline double stressTopRate(const RadioState& s, std::string& why) {
    const Json& h = s.msg("hwinfo");
    why.clear();
    if (h["lockedCentre"].num(0) > 0) { why = "the server sets the rate"; return 0; }
    if (s.hwDriver == "airspyhf") { why = "an HF+ keeps the rate it opened at"; return 0; }
    if (s.mode == "dab") { why = "DAB sets the rate"; return 0; }
    const double cap = h["lockedRate"].num(0) > 0 ? h["lockedRate"].num(0) : 1e30;
    double top = 0;
    for (double r : s.rates) if (r <= cap) top = std::max(top, r);
    if (top <= 0) why = "this radio offers no rate choice";
    return top;
}

/** A STRESS run is running (the service's state, read within the last 30 s). */
inline bool calStressRunning() {
    HwShared& h = hwShared();
    std::lock_guard<std::mutex> lk(h.m);
    return h.cal.mode == "stress" && h.cal.phase == "running" && hwNow() - h.batAt < 30;
}

/** The run is over (ended, cancelled, switching off): the radio's own rate back. */
inline void calStressSdrEnd(App& a, const char* why) {
    StressSdr& k = stressSdr();
    if (!k.active) return;
    if (k.savedRate > 0 && a.eng_) {
        const RadioState& s = a.eng_->state();
        if (s.session == "running" && s.hwDriver == k.driver && std::fabs(stressRateNow(s) - k.savedRate) > 1)
            a.eng_->send("{\"type\":\"sampleRate\",\"value\":" + std::to_string(std::llround(k.savedRate)) + "}");
    }
    std::fprintf(stderr, "vibe-hhui: stress: SDR back to %s (%s)\n", k.savedRate > 0 ? stressRateText(k.savedRate).c_str() : "its own rate", why);
    k = StressSdr{};
}

/** Once a frame (before memTick): drive the radio while a STRESS run is running; put it back after. */
inline void calStressSdrTick(App& a, const RadioState& s) {
    StressSdr& k = stressSdr();
    if (!calStressRunning()) { if (k.active) calStressSdrEnd(a, "the run is over"); return; }
    if (s.session != "running" || !a.eng_) return;            // ★ no radio: waits (a replug carries on)
    const double now = stressRateNow(s);
    if (!k.active || k.driver != s.hwDriver) {
        k = StressSdr{}; k.active = true; k.driver = s.hwDriver; k.savedRate = now;
        std::fprintf(stderr, "vibe-hhui: stress: driving the SDR (%s at %s)\n", s.hwDriver.c_str(), stressRateText(now).c_str());
    }
    std::string why;
    const double top = stressTopRate(s, why);
    if (top <= 0) { k.why = why; return; }
    if (std::fabs(now - top) <= 1) { k.why.clear(); return; }
    if (k.sentRate != top) {
        a.eng_->send("{\"type\":\"sampleRate\",\"value\":" + std::to_string(std::llround(top)) + "}");
        k.sentRate = top; k.sentAt = nowSec(); k.why.clear();
        std::fprintf(stderr, "vibe-hhui: stress: SDR %s -> %s (the top rate offered)\n", stressRateText(now).c_str(), stressRateText(top).c_str());
    } else if (nowSec() - k.sentAt > 4) k.why = "this mode keeps its rate";   // ★ asked once; the engine kept its own
}

/** "SDR 3.2 MS/s" (+ why it is not the top rate) for the stress screen. */
inline std::string calStressSdrLine(App& a) {
    if (!a.eng_) return "";
    const RadioState& s = a.eng_->state();
    if (s.session != "running") return "SDR — no radio plugged in";
    const StressSdr& k = stressSdr();
    std::string l = "SDR " + stressRateText(stressRateNow(s));
    if (!k.why.empty()) l += " (" + k.why + ")";
    return l + " \u00b7 speaker muted \u00b7 no waterfall";
}

/** A stress calibration is waiting on the charger, armed or running (the service's state, read within the last 30 s). */
inline bool calScreenUp() {
    HwShared& h = hwShared();
    std::lock_guard<std::mutex> lk(h.m);
    return h.cal.mode == "stress" && (h.cal.phase == "charging" || h.cal.phase == "armed" || h.cal.phase == "running")
        && hwNow() - h.batAt < 30;
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
    if (k.phase == "charging") {
        // ★ waiting for a genuine full: the charge's progress (both numbers — battery_math.h battChargeLines)
        const bool out = !k.plugged || !b.online;
        c.tc.draw(c.g, c.f.big, out ? "Not full yet" : "Charging to full first", W / 2, 120, th::amber, 1);
        float y = 174;
        if (out) {
            c.tc.draw(c.g, c.f.body, "Plug the charger back in to carry on", W / 2, y, th::white, 1); y += c.f.body.lineHeight();
        } else {
            for (auto& l : battChargeLines(b)) { c.tc.draw(c.g, c.f.body, l, W / 2, y, th::white, 1); y += c.f.body.lineHeight(); }
        }
        y += 10;
        for (auto& l : c.tc.wrap(c.f.body, "When the charger has really finished you will be asked to unplug. The fast load "
                                           "starts then — not now, it would only slow the charge.", (int)W - 100)) {
            c.tc.draw(c.g, c.f.body, l, W / 2, y, th::dim, 1); y += c.f.body.lineHeight();
        }
        BtnStyle st; st.font = &c.f.btn; st.disabled = busy;
        if (button(c, "cal.cancel", {W / 2 - 110, H - 96, 220, 52}, "CANCEL", st) || c.nav.backPressed()) hwCalibrate(a.eng_ ? a.eng_->port() : 0, "");
        return;
    }
    if (k.phase == "armed") {
        c.tc.draw(c.g, c.f.big, "Full — unplug the charger", W / 2, 150, th::green, 1);
        float y = 214;
        for (auto& l : c.tc.wrap(c.f.body, "The count starts when the charger comes out. Then the screen stays at full brightness, "
                                           "every core is kept busy and the radio runs at its top sample rate until the battery reaches a safe empty; the radio then "
                                           "switches itself off and the measured capacity is kept.", (int)W - 100)) {
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
    {   // ★ h80: what the load is — the busy loops (and why fewer: the temperature) and the radio
        int loops, of; std::string why;
        { HwShared& h = hwShared(); std::lock_guard<std::mutex> lk(h.m); loops = h.stressLoops; of = h.stressOf; why = h.stressWhy; }
        if (loops >= 0) {
            std::string l = std::to_string(loops) + " of " + std::to_string(of) + " busy loops" + (why.empty() ? "" : " \u2014 " + why);
            c.tc.draw(c.g, c.f.body, l, W / 2, y, why.empty() ? th::white68 : th::amber, 1); y += c.f.body.lineHeight();
        }
        const std::string sl = calStressSdrLine(a);
        if (!sl.empty()) { c.tc.draw(c.g, c.f.body, sl, W / 2, y, th::white68, 1); y += c.f.body.lineHeight(); }
    }
    y = H - 110;
    for (auto& l : c.tc.wrap(c.f.body, "It switches itself off at a safe empty and keeps the result. HOLD POWER to stop it — the next start is "
                                       "the ordinary radio, still counting. Plugging in the charger cancels it.", (int)W - 80)) {
        c.tc.draw(c.g, c.f.body, l, W / 2, y, th::dim, 1); y += c.f.body.lineHeight();
    }
    c.nav.item("cal.screen", {0, 0, W, H});   // ★ the pad's presses land here, on nothing that does anything
    c.nav.backPressed();                      //   and BACK does not reach the radio underneath
}

}  // namespace hh
