// hw_pump.h — the UI-thread side of R36S HARDWARE / DISPLAY: worker results into App, the SCREEN OFF AFTER dim, the
// first silent import, and DISPLAY's save. Wire-up (app_impl.h's loop):
//     hwPump(*this);        once a loop iteration (before drawing)
// ★ Without hwPump the sections still drain their own results while the menu is open (hwPumpUi) — but the dim at half
//   the SCREEN OFF time and the start-up import need it.
// ★ The PERFORMANCE overlay was removed (Stuart, 2026-11-09: "the performance overlay isnt needed either"); vibe-hhui's
//   "perf 30 s" line in the debug log still measures the same things.
#pragma once
#include "disp_settings.h"
#include "hw_files.h"
#include "hw_state.h"

namespace hh {

struct HwPumpState {
    bool dimmed = false; int savedBright = -1; double dimAt = 0;
    bool importedAtStart = false;
};
inline HwPumpState& hwPumpState() { static HwPumpState s; return s; }

/** Results from the workers → App (UI thread): the device's figures and one toast at a time. */
inline void hwPumpUi(App& a) {
    HwShared& h = hwShared();
    std::lock_guard<std::mutex> lk(h.m);
    if (h.devKnown) {
        if (h.brightness >= 0) a.brightness_ = h.brightness;
        if (h.volume >= 0) a.volume_ = h.volume;
        if (h.batt >= 0) { a.battery_ = h.batt; a.charging_ = h.charging; }
        h.devKnown = false;
        h.brightness = h.volume = -1;
    }
    if (!h.toasts.empty() && nowSec() > a.toastUntil_ - 0.6) { a.toast(h.toasts.front()); a.toastUntil_ = nowSec() + 3.0; h.toasts.pop_front(); }
}

inline void hwPump(App& a) {
    hwPumpUi(a);
    HwPumpState& p = hwPumpState();
    const double now = nowSec();
    dispSaveTick(now);
    // ★ the start-up import (handheld.ts: runImport once at start, silently) — once a radio is running
    if (!p.importedAtStart && a.eng_ && a.eng_->state().session == "running") { p.importedAtStart = true; hwRunImport(a.eng_->port(), false); }
    // ★ SCREEN OFF AFTER: DIM at half the time (handheld.ts:351-373) — to min(saved, 20); any input puts it back
    //   (the full-time switch-off is the loop's own)
    if (a.eng_) {
        const double idle = now - a.lastInput_;
        if (a.set_.screenOffMin > 0 && !p.dimmed && !a.screenOff_ && idle > a.set_.screenOffMin * 30.0) {
            p.dimmed = true; p.dimAt = now; p.savedBright = a.brightness_;
            a.eng_->device("{\"set\":\"brightness\",\"value\":" + std::to_string(std::min(a.brightness_, 20)) + "}");
        } else if (p.dimmed && a.lastInput_ > p.dimAt) {
            p.dimmed = false;
            if (p.savedBright > 0) a.eng_->device("{\"set\":\"brightness\",\"value\":" + std::to_string(p.savedBright) + "}");
        }
    }
}

/** refreshDeviceRows on opening (the web runs it whenever an accordion opens): a gap in the section's drawing = opened. */
inline void hwOnOpen(App& a, double& lastDrawn) {
    const double now = nowSec();
    if (now - lastDrawn > 0.4 && a.eng_) hwRefresh(a.eng_->port());
    lastDrawn = now;
}

/** A field that opens the controller keyboard (the web's text box + SET): its value, or the placeholder dimmed. */
inline bool hwField(Ctx& c, const std::string& id, Rect row, Rect r, const std::string& value, const std::string& placeholder) {
    const Nav::Hit h = c.nav.item(id, row);   // ★ row-wide highlight box (hwRowToggle)
    c.g.rrect(r.x, r.y, r.w, r.h, 7, rgb(0x000000, 0.55f));
    c.g.rframe(r.x, r.y, r.w, r.h, 7, 1, rgb(0xffffff, 0.45f));
    textIn(c, c.f.body, value.empty() ? placeholder : value, r, value.empty() ? th::dimmer : th::white, 0, 10);
    if (h.focused) focusRing(c, r, 7);
    return h.pressed;
}

/** ★ ROW-WIDE HIGHLIGHT BOXES. The D-pad's next-control pick (gamepadNav pickNext) penalises sideways distance ×2.5,
 *  so a chip at the far right under a slider in the middle was skipped — DOWN jumped two rows. These register the
 *  WHOLE ROW with the Nav (so every row sits on the same centre line and DOWN visits each in turn) but draw, and ring,
 *  only the control itself. */
inline bool hwRowToggle(Ctx& c, const std::string& id, Rect row, Rect chip, bool on, const std::string& onT = "ON", const std::string& offT = "OFF") {
    const Nav::Hit h = c.nav.item(id, row);
    c.g.rrect(chip.x, chip.y, chip.w, chip.h, 7, on ? th::onBg : th::btnBg);
    c.g.rframe(chip.x, chip.y, chip.w, chip.h, 7, 1, on ? th::onBg : th::btnBorder);
    textIn(c, c.f.bodyB, on ? onT : offT, chip, on ? th::onText : th::btnText);
    if (h.focused) focusRing(c, chip, 7);
    return h.pressed;
}
inline bool hwRowButton(Ctx& c, const std::string& id, Rect row, Rect btn, const std::string& label, BtnStyle st = {}) {
    const Nav::Hit h = st.disabled ? Nav::Hit{} : c.nav.item(id, row);
    const float al = st.disabled ? 0.38f : 1.f;
    c.g.rrect(btn.x, btn.y, btn.w, btn.h, st.radius, st.on ? th::activeBg : th::btnBg);
    Rgba bd = st.on ? th::activeBdr : th::btnBorder; bd.a *= al;
    c.g.rframe(btn.x, btn.y, btn.w, btn.h, st.radius, 1, bd);
    Rgba tx = st.on ? th::activeTx : st.text; tx.a *= al;
    textIn(c, st.font ? *st.font : c.f.btn, label, btn, tx);
    if (h.focused) focusRing(c, btn, st.radius);
    return h.pressed;
}

/** ui.h's slider() with a ROW-WIDE highlight box (see above): the track where it sits, the amber ring round the track. */
inline bool hwRowSlider(Ctx& c, const std::string& id, Rect row, Rect r, double& v, double lo, double hi, double step) {
    const Nav::Hit h = c.nav.item(id, row, true);
    bool changed = false;
    if (h.adjust) {
        const double nv = std::max(lo, std::min(hi, std::round((v + h.adjust * step - lo) / step) * step + lo));
        if (nv != v) { v = nv; changed = true; }
    }
    const float ty = r.cy() - 3;
    const float frac = hi > lo ? (float)((v - lo) / (hi - lo)) : 0.f;
    c.g.rrect(r.x, ty, r.w, 6, 3, th::meterTrack);
    c.g.rrect(r.x, ty, std::max(6.f, r.w * frac), 6, 3, th::amber);
    c.g.circle(r.x + r.w * frac, r.cy(), 10, 0, th::amber);
    if (h.focused) focusRing(c, r, 8, true);
    return changed;
}

}  // namespace hh
