// sec_hardware.h — MENU → R36S HARDWARE (handheld.ts buildHandheldMenu) (one of the menu's parts; see app.h for the App's state and helpers, ui.h for the widgets).
#pragma once
// (included by app.h after App is declared)

namespace hh {

inline float App::secHardware(Ctx& c, const RadioState& s, Rect a, float y) {
    (void)s;
    // VOLUME
    float y0 = y;
    y = menuRow(c, a, y, "VOLUME", "");
    {
        double v = volume_ < 0 ? 60 : volume_;
        if (slider(c, "hw.vol", {a.x + 150, y0 + 2, a.w - 230, 40}, v, 0, 100, 5)) { volume_ = (int)v; eng_->device("{\"set\":\"volume\",\"value\":" + std::to_string(volume_) + "}"); }
        c.tc.draw(c.g, c.f.body, std::to_string(volume_ < 0 ? 0 : volume_), a.r() - 6, y0 + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
    }
    // BRIGHTNESS
    y0 = y;
    y = menuRow(c, a, y, "BRIGHTNESS", "");
    {
        double v = brightness_;
        if (slider(c, "hw.bri", {a.x + 150, y0 + 2, a.w - 230, 40}, v, 10, 255, 15)) { brightness_ = (int)v; eng_->device("{\"set\":\"brightness\",\"value\":" + std::to_string(brightness_) + "}"); }
        c.tc.draw(c.g, c.f.body, std::to_string(brightness_ * 100 / 255) + " %", a.r() - 6, y0 + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
    }
    // BATTERY
    y0 = y;
    y = menuRow(c, a, y, "BATTERY", "");
    c.tc.draw(c.g, c.f.body, battery_ < 0 ? "—" : std::to_string(battery_) + " %" + (charging_ ? " · charging" : ""), a.r() - 6, y0 + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
    // CLOCK
    y0 = y;
    y = menuRow(c, a, y, "CLOCK", "");
    {
        char clk[48];
        const std::time_t tt = std::time(nullptr);
        std::tm u{}; gmtime_r(&tt, &u);
        std::snprintf(clk, sizeof clk, "%04d-%02d-%02d %02d:%02d:%02d UTC", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
        c.tc.drawTab(c.g, c.f.body, clk, a.r() - 6, y0 + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
    }
    // POWER OFF · RESTART (press twice — a stray press never switches the radio off)
    const double t = nowSec();
    const float bw = (a.w - 12 - 12) / 2;
    BtnStyle st; st.font = &c.f.btn;
    st.on = t - armedPower_ < 3;
    if (button(c, "hw.off", {a.x + 6, y, bw, 52}, st.on ? "PRESS AGAIN" : "POWER OFF", st)) {
        if (t - armedPower_ < 3) { eng_->device("{\"action\":\"poweroff\"}"); toast("Switching off"); }
        else { armedPower_ = t; toast("Press again to switch off"); }
    }
    st.on = t - armedReboot_ < 3;
    if (button(c, "hw.reboot", {a.x + 18 + bw, y, bw, 52}, st.on ? "PRESS AGAIN" : "RESTART", st)) {
        if (t - armedReboot_ < 3) { eng_->device("{\"action\":\"reboot\"}"); toast("Restarting"); }
        else { armedReboot_ = t; toast("Press again to restart"); }
    }
    return y + 64;
}


}  // namespace hh
