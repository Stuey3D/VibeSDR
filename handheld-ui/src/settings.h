// settings.h — the handheld UI's own settings (the DEVICE's look and habits, never a radio's), kept in
// $HOME/.vibe-hhui.json, written with a temp file + rename (Stuart: "every write = temp file + rename").
#pragma once
#include "json.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace hh {

struct Settings {
    int step = 1000;               // Hz (the web's default `step`)
    std::string palette = "Jet";   // ★ the app's production default
    // wf::Scroll — ★★ Stuart, 2026-11-07: three choices over the engine's fixed 5 frames a second: NATIVE (2, the real 5
    //   rows), 10 (Eco, 3) and 30 (Smooth, 5 — Smooth's rule gives 30 from a 5 fps feed; Default would give 15). No 20 (it
    //   judders on other devices — feedback_waterfall_core_rules). 30 by default.
    int speed = 5;
    double contrast = 0, sharpness = 5, wfBrightness = 0;
    int specRate = 10;             // real spectrum frames a second asked of the engine: 5 / 10 / 20 — ★ 10, as every server's
                                   //   default (Stuart, 2026-11-07); the waterfall's output clock stays 30 rows/s regardless
    int screenOffMin = 0;          // 0 = never
    /** ★★ IDLE POWER SAVE (Stuart, 2026-11-07: "slowing the waterfall data rate to the minimum when idle … and then
     *  sharpen it up and speed it up when in use") — 1 = on (the default). See App::idleNow_. */
    int idleSave = 1;
    bool followVfo = true;         // VFO CENTRE: LOCKED (the view follows the dial)
    int meterMode = 0;             // 0 SNR · 1 S-units · 2 dBFS (the web's meter cycle)
    int volume = -1;               // last known (the hardware service owns it)
    int dispClock = 0, dispProven = 1;   // the KEPT display mode (pixel clock kHz; 0 = the panel's default) — display_rate.h
    int stickDeg = 30;             // ★ the TUNING STICK FEEL: degrees of turn a step (Stuart, 2026-11-07: "more of a turn before
                                   //   it steps so a user can dial in the feel they want") — 30 = the web's Jog
    int freqUnit = 2;              // the readout's and the pad's unit: 0 Hz · 1 kHz · 2 MHz (the web's freqUnit, device-wide)

    static std::string path() {
        const char* home = std::getenv("HOME");
        return std::string(home && *home ? home : "/tmp") + "/.vibe-hhui.json";
    }
    void load() {
        FILE* f = std::fopen(path().c_str(), "r");
        if (!f) return;
        std::string s;
        char b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        std::fclose(f);
        const Json j = Json::parse(s);
        if (j.isNull()) return;
        step = (int)j["step"].num(step);
        if (!j["palette"].str().empty()) palette = j["palette"].str();
        speed = (int)j["speed"].num(speed);
        if (speed != 2 && speed != 3) speed = 5;   // ★ LOW / DEFAULT / older choices → 30 (the three left are NATIVE, 10, 30)
        contrast = j["contrast"].num(contrast);
        sharpness = j["sharpness"].num(sharpness);
        wfBrightness = j["wfBrightness"].num(wfBrightness);
        specRate = (int)j["specRate"].num(specRate);
        screenOffMin = (int)j["screenOffMin"].num(screenOffMin);
        idleSave = (int)j["idleSave"].num(idleSave) ? 1 : 0;
        followVfo = j["followVfo"].boolean(followVfo);
        meterMode = (int)j["meterMode"].num(meterMode);
        freqUnit = std::max(0, std::min(2, (int)j["freqUnit"].num(freqUnit)));
        dispClock = (int)j["dispClock"].num(dispClock);
        dispProven = (int)j["dispProven"].num(dispProven);
        stickDeg = std::max(10, std::min(180, (int)j["stickDeg"].num(stickDeg)));
    }
    void save() const {
        const std::string p = path(), tmp = p + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "{\"step\":%d,\"palette\":%s,\"speed\":%d,\"contrast\":%g,\"sharpness\":%g,\"wfBrightness\":%g,"
                        "\"specRate\":%d,\"screenOffMin\":%d,\"followVfo\":%s,\"meterMode\":%d,\"freqUnit\":%d,\"dispClock\":%d,\"dispProven\":%d,\"stickDeg\":%d,\"idleSave\":%d}\n",
                     step, jsonQuote(palette).c_str(), speed, contrast, sharpness, wfBrightness, specRate, screenOffMin,
                     followVfo ? "true" : "false", meterMode, freqUnit, dispClock, dispProven, stickDeg, idleSave);
        std::fflush(f);
        std::fclose(f);
        std::rename(tmp.c_str(), p.c_str());
    }
};

}  // namespace hh
