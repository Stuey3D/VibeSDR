// sec_sdr.h — MENU → SDR HARDWARE, per radio driver (one of the menu's parts; see app.h for the App's state and helpers, ui.h for the widgets).
#pragma once
// (included by app.h after App is declared)

namespace hh {

inline float App::secSdr(Ctx& c, const RadioState& s, Rect a, float y) {
    float y0 = y;
    y = menuRow(c, a, y, "RECEIVER", "");
    c.tc.draw(c.g, c.f.body, s.radioName.empty() ? "—" : s.radioName, a.r() - 6, y0 + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
    if (!s.gains.empty()) {
        y0 = y;
        y = menuRow(c, a, y, "GAIN", "");
        int idx = 0;
        for (size_t i = 0; i < s.gains.size(); i++) if (std::abs(s.gains[i] - s.gainNow) < std::abs(s.gains[(size_t)idx] - s.gainNow)) idx = (int)i;
        double v = idx;
        if (slider(c, "sdr.gain", {a.x + 150, y0 + 2, a.w - 260, 40}, v, 0, (double)s.gains.size() - 1, 1)) eng_->gain(s.gains[(size_t)v]);
        char b[24]; std::snprintf(b, sizeof b, "%.1f dB", s.gainNow / 10.0);
        c.tc.draw(c.g, c.f.body, b, a.r() - 6, y0 + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
        y0 = y;
        y = menuRow(c, a, y, "AGC", "");
        if (toggle(c, "sdr.agc", {a.r() - 96, y0 + 4, 90, 38}, s.agc, "AUTO", "OFF")) eng_->agc(!s.agc);
    }
    return y;
}


}  // namespace hh
