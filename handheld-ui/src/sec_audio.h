// sec_audio.h — the AUDIO sheet (#audioPanel) (one of the menu's parts; see app.h for the App's state and helpers, ui.h for the widgets).
#pragma once
// (included by app.h after App is declared)

namespace hh {

inline void App::drawAudioSheet(Ctx& c, const RadioState& s) {
    bool closed = false;
    const Rect area = sheetPanel(c, {12, 10, 616, 460}, "AUDIO", "a.close", closed);
    if (closed) { closeTop(); return; }
    float y = area.y + 4;
    // VOLUME — the device's (the hardware service owns it; the volume keys move the same figure)
    y = menuRow(c, area, y, "VOLUME", "");
    {
        double v = volume_ < 0 ? 60 : volume_;
        const Rect sr{area.x + 150, y - 40, area.w - 230, 40};
        if (slider(c, "a.vol", sr, v, 0, 100, 5)) { volume_ = (int)v; eng_->device("{\"set\":\"volume\",\"value\":" + std::to_string(volume_) + "}"); }
        c.tc.draw(c.g, c.f.body, std::to_string(volume_ < 0 ? 0 : volume_), area.r() - 6, y - 32, th::dim, 2);
    }
    (void)s;
}


}  // namespace hh
