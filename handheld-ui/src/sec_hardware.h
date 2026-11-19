// sec_hardware.h — MENU → R36S HARDWARE (handheld.ts buildHandheldMenu, inventory §4c.2) (one of the menu's parts; see
// app.h for the App's state and helpers, ui.h for the widgets).
//
// Rows, in the web's order: VOLUME · BATTERY (+ the live block and CALIBRATE, R36S only) · CLOCK · NUDGE · SET UTC · SET CLOCK FROM <station> / ALIGN TO FT8 (only
// with fresh evidence) · the note · LOCATION · SET LOCATION · CARD · FILES ·
// POWER. ★ BRIGHTNESS is DISPLAY's first row, as on the web page (sec_display.h).
// Left out: TOUR / VIEW TUTORIAL (no native
// tutorial yet — a key that opened nothing would be a dead control).
#pragma once
// (included by app.h after App is declared)
#include "hw_files.h"
#include "hw_pump.h"
#include "hw_state.h"

#include <regex>

namespace hh {

inline float App::secHardware(Ctx& c, const RadioState& s, Rect a, float y) {
    (void)s;
    static double lastDrawn = 0;
    hwOnOpen(*this, lastDrawn);
    hwPumpUi(*this);
    const int port = eng_ ? eng_->port() : 0;
    const float valY = 22 - c.f.body.lineHeight() / 2.f;   // a value on the label's line
    HwShared& sh = hwShared();

    // VOLUME (the ALSA master, through the hardware service — not the AUDIO sheet's speaker level)
    float y0 = y;
    y = menuRow(c, a, y, "VOLUME", "");
    {
        double v = volume_ < 0 ? 60 : volume_;
        if (hwRowSlider(c, "hw.vol", {a.x, y0, a.w, 44}, {a.x + 150, y0 + 2, a.w - 230, 40}, v, 0, 100, 1)) { volume_ = (int)v; const std::string m = "{\"set\":\"volume\",\"value\":" + std::to_string(volume_) + "}"; sendLatest("hw.vol", [this, m] { eng_->device(m); }); }   // ★ a held slider: ≤ 10 POSTs a second, the last always
        c.tc.draw(c.g, c.f.body, std::to_string(volume_ < 0 ? 60 : volume_), a.r() - 6, y0 + valY, th::dim, 2);
    }
    // BATTERY
    y0 = y;
    y = menuRow(c, a, y, "BATTERY", "");
    {
        std::string t = "—";
        bool ok, full; int mins;
        { std::lock_guard<std::mutex> lk(sh.m); ok = sh.hwdOk; mins = sh.minutesLeft; full = sh.bat.online && sh.bat.charge.state == "full"; }
        if (battery_ >= 0) {
            // ★ "full" only when the CHARGER says so (vibe-hwd FullJudge) — a gauge's 100 % on the cord is still charging
            t = std::to_string(battery_) + " %" + (charging_ ? (full ? " · full" : " · charging") : "");
            if (!charging_ && mins >= 0) t += " · about " + std::to_string(mins / 60) + " h " + std::to_string(mins % 60) + " min left";
        } else if (!ok) t = "the hardware service is not running";
        c.tc.draw(c.g, c.f.body, t, a.r() - 6, y0 + valY, th::dim, 2);
    }
    // ★★★ THE BATTERY BLOCK (Stuart, 2026-11-15): what the battery is doing NOW, once a second — measured on battery;
    //     on the charger the unit's own use and the charger's current are worked out (≈, battery_math.h) — and the
    //     CALIBRATION beside it: this cell's real capacity, measured on a discharge from a GENUINE full (vibe-hwd
    //     BatteryCal + FullJudge, 2026-11-16: "Full — charge finished" comes from the charger, never the gauge's 100 %).
    {
        static double lastPoll = 0;
        if (nowSec() - lastPoll >= 1.0) { lastPoll = nowSec(); if (port) hwRefreshDevice(port); }
        BattLive b; BattCal k; bool busy;
        { std::lock_guard<std::mutex> lk(sh.m); b = sh.bat; k = sh.cal; busy = sh.calBusy; }
        for (auto& l : battLiveLines(b)) {
            // ★ the verdicts in colour: full green, "not full yet" / the input-limit hint amber; the figures plain
            const bool full = l.rfind("Full —", 0) == 0, warn = l.find("not full yet") != std::string::npos
                || l.rfind("Input near", 0) == 0 || l.rfind("A shorter cable", 0) == 0 || l.rfind("The charger is held", 0) == 0;
            for (auto& w : c.tc.wrap(c.f.small, l, (int)a.w - 12)) {
                c.tc.draw(c.g, c.f.small, w, a.x + 6, y - 6, full ? th::green : warn ? th::amber : th::white68); y += c.f.small.lineHeight();
            }
        }
        y += 4;
        for (auto& l : battCalResultLines(k, b.fullMah))
            for (auto& w : c.tc.wrap(c.f.small, l, (int)a.w - 12)) { c.tc.draw(c.g, c.f.small, w, a.x + 6, y - 6, k.hasResult ? th::green : th::dim); y += c.f.small.lineHeight(); }
        const std::string ph = battCalPhaseLine(k);
        std::string say = !ph.empty() ? ph : k.note;   // (the last run's note: "calibration cancelled — …", "… failed …")
        if (!say.empty()) say[0] = (char)std::toupper((unsigned char)say[0]);
        if (!say.empty()) for (auto& l : c.tc.wrap(c.f.small, say, (int)a.w - 12)) { c.tc.draw(c.g, c.f.small, l, a.x + 6, y - 6, th::amber); y += c.f.small.lineHeight(); }
        y += 8;
        // CALIBRATE — NORMAL · STRESS (press twice: a stray press must not start a run that ends in a power-off);
        // started ON THE CHARGER; while it waits for full, or for the unplug, CANCEL. Running: no key — charging
        // before empty is what cancels it.
        y0 = y;
        y = menuRow(c, a, y, "CALIBRATE", k.phase == "idle"
            ? "Measures this battery's real capacity. With the charger in, press NORMAL (use the radio until it switches "
              "itself off) or STRESS (the fastest: full brightness, every core busy, the radio at its top sample rate, a "
              "screen of its own — hold POWER to stop it). It waits for a genuine full charge, then asks you to unplug; the count starts then. A power-off "
              "part-way does not spoil it."
            : "");
        static double armedN = 0, armedS = 0;
        const double t = nowSec();
        const float x0 = a.x + 150, gap = 8, bw = (a.r() - 6 - x0 - gap) / 2;
        BtnStyle st; st.font = &c.f.body; st.radius = 7;
        if (k.phase == "idle") {
            st.disabled = busy;
            st.on = t - armedN < 3;
            if (hwRowButton(c, "hw.calN", {a.x, y0, a.w, 44}, {x0, y0 + 3, bw, 38}, st.on ? "PRESS AGAIN" : "NORMAL", st)) {
                if (st.on) { armedN = 0; hwCalibrate(port, "normal"); } else { armedN = t; armedS = 0; toast("Press again to calibrate as you use it"); }
            }
            st.on = t - armedS < 3;
            if (button(c, "hw.calS", {x0 + bw + gap, y0 + 3, bw, 38}, st.on ? "PRESS AGAIN" : "STRESS (FASTEST)", st)) {
                if (st.on) { armedS = 0; hwCalibrate(port, "stress"); } else { armedS = t; armedN = 0; toast("Press again for the fast calibration"); }
            }
        } else if (k.phase == "running") {
            c.tc.draw(c.g, c.f.body, k.mode == "stress" ? "under way (fast)" : "under way", a.r() - 6, y0 + valY, th::amber, 2);
        } else if (k.phase == "armed" || k.phase == "charging") {
            st.disabled = busy;
            if (hwRowButton(c, "hw.calX", {a.x, y0, a.w, 44}, {x0, y0 + 3, bw, 38}, "CANCEL", st)) hwCalibrate(port, "");
        }
    }
    // ★★ INPUT FLOOR (TEST) — h80, 2026-11-19: the charge port's input voltage floor (rk817 CHRG_IN VLIM). On the
    //    charger near full the RTL-SDR dropped off USB as it started (Stuart's h79 log, the port at 4.84–4.89 V); the
    //    suspicion is its start-up surge pulling the input under the 4.5 V floor. A TEST setting, said so on the row;
    //    only drawn when the service reports it (an older vibe-hwd has none — no dead control).
    {
        int vl; bool vb;
        { std::lock_guard<std::mutex> lk(sh.m); vl = sh.vlimMv; vb = sh.vlimBusy; }
        if (vl > 0) {
            y = menuRow(c, a, y, "INPUT FLOOR (TEST)", "A test setting for the RTL-SDR dropping off on the charger. The "
                "lowest charger voltage the console will take power at: 4.5 V is normal. Lower lets a weak charger or a "
                "long cable keep going under the radio's start-up surge. Kept until you change it; every change is in the "
                "Diagnostic logs.");
            const int mvs[3] = {4500, 4300, 4000};
            int sel = -1;
            for (int i = 0; i < 3; i++) if (mvs[i] == vl) sel = i;
            const int hit = segmented(c, "hw.vlim", {a.x + 6, y - 4, a.w - 12, 38}, {"4.5 V (NORMAL)", "4.3 V", "4.0 V"}, sel);
            if (hit >= 0 && hit != sel && !vb) hwSetVlim(port, mvs[hit]);
            y += 46;
        }
    }
    // CLOCK (UTC — the device's clock, a tenth of a second)
    y0 = y;
    y = menuRow(c, a, y, "CLOCK", "");
    c.tc.draw(c.g, c.f.small, "UTC", a.x + 6, y0 + 36, th::dim);
    c.tc.drawTab(c.g, c.f.body, hwUtcText(hwUtcMsNow()), a.r() - 6, y0 + valY, th::dim, 2);
    y += 8;
    // ★ the latest successful sync, one line (hw_state.h hwClockLastLine) — Stuart, 2026-11-08
    {
        bool warn = false;
        const std::string last = hwClockLastLine(warn);
        if (warn) {   // ★ the clock is not known (flat battery): ask for a source, wrapped, in amber
            for (auto& l : c.tc.wrap(c.f.small, last, (int)a.w - 12)) { c.tc.draw(c.g, c.f.small, l, a.x + 6, y - 6, th::amber); y += c.f.small.lineHeight(); }
            y += 4;
        } else if (!last.empty()) { c.tc.draw(c.g, c.f.small, last, a.r() - 6, y - 6, th::green, 2); y += c.f.small.lineHeight() + 2; }
    }
    // NUDGE
    y0 = y;
    y = menuRow(c, a, y, "NUDGE", "");
    {
        struct N { const char* t; int ms; };
        const N ns[6] = {{"−1 m", -60000}, {"−1 s", -1000}, {"−0.1 s", -100}, {"+0.1 s", 100}, {"+1 s", 1000}, {"+1 m", 60000}};
        const float x0 = a.x + 110, gap = 6, w = (a.r() - 6 - x0 - gap * 5) / 6;
        for (int i = 0; i < 6; i++) {
            BtnStyle st; st.font = &c.f.body; st.radius = 7;
            if (button(c, std::string("hw.nudge") + std::to_string(i), {x0 + i * (w + gap), y0 + 3, w, 38}, ns[i].t, st))
                hwSetClock(port, "{\"deltaMs\":" + std::to_string(ns[i].ms) + ",\"source\":" + jsonQuote(std::string("nudge ") + ns[i].t) + "}",
                           std::string("(") + ns[i].t + ")");
        }
    }
    // SET UTC (the controller keyboard; it opens on the clock as it reads now, so a correction is a digit or two)
    y0 = y;
    y = menuRow(c, a, y, "SET UTC", "");
    if (hwField(c, "hw.setutc", {a.x, y0, a.w, 44}, {a.x + 110, y0 + 3, a.w - 116, 38}, "", "YYYY-MM-DD HH:MM:SS")) {
        KbRequest r;
        r.title = "SET UTC";
        r.opts.mode = kb::KbMode::text;
        r.opts.value = hwUtcText(hwUtcMsNow(), false);
        r.commit = [port](const kb::KbState& st) -> std::string {
            static const std::regex re(R"(^(\d{4})-(\d\d)-(\d\d)[ T](\d\d):(\d\d)(?::(\d\d(?:\.\d+)?))?$)");
            std::smatch m;
            const std::string v = hwTrim(st.valueUtf8());
            if (!std::regex_match(v, m, re)) return "Not a time — type it as YYYY-MM-DD HH:MM:SS, in UTC";
            std::tm t{};
            t.tm_year = std::stoi(m[1]) - 1900; t.tm_mon = std::stoi(m[2]) - 1; t.tm_mday = std::stoi(m[3]);
            t.tm_hour = std::stoi(m[4]); t.tm_min = std::stoi(m[5]);
            const long long ms = (long long)timegm(&t) * 1000 + (long long)std::llround((m[6].matched ? std::stod(m[6]) : 0.0) * 1000);
            hwSetClock(port, "{\"utcMs\":" + std::to_string(ms) + ",\"source\":\"by hand\"}", "by hand");
            return "";
        };
        openKeyboard(r);
    }
    // CLOCK AUTO-SET (clock_rds.h clockAutoTick) — Stuart, 2026-11-08
    y = menuRow(c, a, y, "CLOCK AUTO-SET", set_.clockAuto
        ? "ON: the clock sets itself from a time station, an FM station's RDS clock or a DAB multiplex once it has agreed "
          "with itself twice, or from FT8 after three cycles of decodes that agree — and only when it is further out than "
          "that source can be trusted to (DAB: 4 s, RDS: 1.5 s, FT8 and time stations: 0.2 s)"
        : "OFF: the clock changes only when you press SET CLOCK FROM below");
    {
        const int hit = segmented(c, "hw.clkauto", {a.x + 6, y - 4, a.w - 12, 38}, {"OFF", "ON"}, set_.clockAuto ? 1 : 0);
        if (hit >= 0) { set_.clockAuto = hit; dirty(); }
        y += 46;
    }
    // SET CLOCK FROM <station> · ALIGN TO FT8 — only while there is fresh evidence (a station minute under 90 s old;
    // ≥ 3 FT8 decodes in 2 min). Fed by hwNoteTimeSync / hwNoteFt8Dt.
    {
        const HwTimeSync& ls = hwLastSync();
        const bool fresh = !ls.station.empty() && hwNow() - ls.at < 90;
        double dt = 0; int n = 0;
        const bool ft8 = hwFt8Median(dt, n);
        if (fresh || ft8) {
            float x = a.x + 6;
            BtnStyle st; st.font = &c.f.body; st.radius = 7;
            if (fresh) {
                const std::string t = "SET CLOCK FROM " + ls.station;
                const float w = (float)c.tc.width(c.f.body, t) + 32;
                // ★ (utcMs + rxDelayMs) − hostMs is the engine clock's error at the boundary — still the error now
                if (button(c, "hw.clkst", {x, y, w, 40}, t, st)) {
                    hwSetClock(port, "{\"deltaMs\":" + std::to_string(std::llround(ls.utcMs + ls.rxDelayMs - ls.hostMs)) + ",\"source\":" + jsonQuote(ls.station) + "}", "from " + ls.station);
                    hwFt8ClockForget(25);   // ★ the FT8 DTs so far were against the old clock
                }
                x += w + 8;
            }
            if (ft8) {
                char t[64]; std::snprintf(t, sizeof t, "ALIGN TO FT8 (DT %s%.2f s, %d)", dt >= 0 ? "+" : "", dt, n);
                const float w = (float)c.tc.width(c.f.body, t) + 32;
                // ★ a positive DT = signals arrive late against our clock = our clock is FAST: set it back
                if (button(c, "hw.clkft8", {x, y, w, 40}, t, st)) {
                    hwSetClock(port, "{\"deltaMs\":" + std::to_string(std::llround(-dt * 1000)) + ",\"source\":\"FT8\"}", "to FT8 (" + std::to_string(n) + " decodes)");
                    hwFt8ClockForget(25);
                }
            }
            y += 50;
        }
    }
    for (auto& l : c.tc.wrap(c.f.small, "\"Set clock from\" appears after a time station (MSF, DCF77, WWV, WWVB, JJY) has given two consecutive "
                                        "parity-checked minutes that agree — or, without one, after an FM station's RDS clock or a DAB "
                                        "multiplex's time has agreed with itself twice (to about a second); \"Align to FT8\" after three "
                                        "or more FT8 decodes.", (int)a.w - 12)) {
        c.tc.draw(c.g, c.f.small, l, a.x + 6, y, th::dim); y += c.f.small.lineHeight();
    }
    y += 10;
    // LOCATION
    y0 = y;
    y = menuRow(c, a, y, "LOCATION", "Where this receiver is — for distances, ranges and the map");
    {
        std::string t = "—";
        { std::lock_guard<std::mutex> lk(sh.m); if (sh.locKnown) t = !sh.locSet ? "not set" : !sh.locator.empty() ? sh.locator : sh.lat + ", " + sh.lon; }
        c.tc.draw(c.g, c.f.body, t, a.r() - 6, y0 + valY, th::dim, 2);
    }
    // SET LOCATION (a locator, or lat, lon) · CLEAR
    y0 = y;
    y = menuRow(c, a, y, "SET LOCATION", "");
    {
        const float cw = 100;
        if (hwField(c, "hw.loc", {a.x, y0, a.w, 44}, {a.x + 150, y0 + 3, a.w - 156 - cw - 8, 38}, "", "IO92nh  or  52.25, -0.89")) {
            KbRequest r;
            r.title = "SET LOCATION";
            r.opts.mode = kb::KbMode::text;
            r.commit = [port](const kb::KbState& st) -> std::string {
                const std::string t = hwTrim(st.valueUtf8());
                static const std::regex ll(R"(^(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)$)");
                static const std::regex loc(R"(^[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?$)");
                std::smatch m;
                if (std::regex_match(t, m, ll)) {
                    const double la = std::stod(m[1]), lo = std::stod(m[2]);
                    // ★ the supervisor's own refusals, checked here first so the keyboard stays open on a slip
                    if (la < -90 || la > 90 || lo < -180 || lo > 180 || (la == 0 && lo == 0)) return "latitude -90 to 90, longitude -180 to 180";
                    char b[96]; std::snprintf(b, sizeof b, "{\"lat\":%.6f,\"lon\":%.6f}", la, lo);
                    hwPostLocation(port, b);
                    return "";
                }
                if (!std::regex_match(t, loc)) return "a locator is 4 or 6 characters, e.g. IO92 or IO92nh";
                hwPostLocation(port, "{\"locator\":" + jsonQuote(t) + "}");
                return "";
            };
            openKeyboard(r);
        }
        BtnStyle st; st.font = &c.f.body; st.radius = 7;
        if (button(c, "hw.locclr", {a.r() - 6 - cw, y0 + 3, cw, 38}, "CLEAR", st)) hwPostLocation(port, "{\"clear\":true}");
    }
    // SET FROM MAP (handheldMap.ts openMapPick): the native map, the pointer's point set with the bottom face
    y0 = y;
    y = menuRow(c, a, y, "", "");
    { BtnStyle st; st.font = &c.f.body; st.radius = 7;
      if (button(c, "hw.locmap", {a.x + 150, y0 + 3, a.w - 156, 38}, "SET FROM MAP", st)) mapOpenPicker(*this); }
    // CARD
    y0 = y;
    y = menuRow(c, a, y, "CARD", "");
    { std::string t; { std::lock_guard<std::mutex> lk(sh.m); t = sh.card; } c.tc.draw(c.g, c.f.body, t, a.r() - 6, y0 + valY, th::dim, 2); }
    // FILES — IMPORT · EXPORT BOOKMARKS · EXPORT DIAGNOSTICS
    y = menuRow(c, a, y, "FILES", "");
    {
        bool ib, eb, db;
        { std::lock_guard<std::mutex> lk(sh.m); ib = sh.importBusy; eb = sh.exportBusy; db = sh.diagBusy; }
        const float gap = 6, w0 = 136, w1 = 196, w2 = a.w - 12 - w0 - w1 - 2 * gap;
        BtnStyle st; st.font = &c.f.body; st.radius = 7;
        st.disabled = ib;
        if (button(c, "hw.imp", {a.x + 6, y - 6, w0, 40}, ib ? "WORKING…" : "IMPORT", st)) hwRunImport(port, true);
        st.disabled = eb;
        // ★ the middle key's highlight box is the whole row, so DOWN from the row above lands here (hwRowButton)
        if (hwRowButton(c, "hw.exp", {a.x, y - 6, a.w, 40}, {a.x + 6 + w0 + gap, y - 6, w1, 40}, eb ? "WORKING…" : "EXPORT BOOKMARKS", st)) hwExportBookmarks(port);
        st.disabled = db;
        if (button(c, "hw.diag", {a.x + 6 + w0 + w1 + 2 * gap, y - 6, w2, 40}, db ? "WORKING…" : "EXPORT DIAGNOSTICS", st)) hwExportDiagnostics(port);
        y += 46;
    }
    y += 10;
    // POWER — POWER OFF · RESTART (press twice — a stray press never switches the radio off)
    y = menuRow(c, a, y, "POWER", "");
    {
        const double t = nowSec();
        const float bw = (a.w - 12 - 12) / 2;
        BtnStyle st; st.font = &c.f.btn;
        st.on = t - armedPower_ < 3;
        // ★ POWER OFF's highlight box is the row (DOWN from the row above skipped two half-width keys); RIGHT reaches RESTART
        if (hwRowButton(c, "hw.off", {a.x, y - 6, a.w, 52}, {a.x + 6, y - 6, bw, 52}, st.on ? "PRESS AGAIN" : "POWER OFF", st)) {
            if (t - armedPower_ < 3) { eng_->device("{\"action\":\"poweroff\"}"); toast("Switching off"); }
            else { armedPower_ = t; toast("Press again to switch off"); }
        }
        st.on = t - armedReboot_ < 3;
        if (button(c, "hw.reboot", {a.x + 18 + bw, y - 6, bw, 52}, st.on ? "PRESS AGAIN" : "RESTART", st)) {
            if (t - armedReboot_ < 3) { eng_->device("{\"action\":\"reboot\"}"); toast("Restarting"); }
            else { armedReboot_ = t; toast("Press again to restart"); }
        }
        y += 58;
    }
    return y;
}


}  // namespace hh
