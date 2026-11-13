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
    // wf::Scroll — ★★ Stuart, 2026-11-07: four choices over the engine's fixed 5 frames a second, the app's and the web's
    //   own rules for a 5 fps feed: NATIVE (2, the real 5 rows), 10 (Eco, 3), 15 (Default, 4 — 3 × the feed) and 30
    //   (Smooth, 5 — 6 × the feed). No 20 (it judders on other devices — feedback_waterfall_core_rules). 15 by default:
    //   "that would be the setting I would use it at the most as it matches the settings i use on the web client".
    int speed = 4;
    double contrast = 0, sharpness = 5, wfBrightness = 0;
    int specRate = 10;             // real spectrum frames a second asked of the engine: 5 / 10 / 20 — ★ 10, as every server's
                                   //   default (Stuart, 2026-11-07); the waterfall's output clock stays 30 rows/s regardless
    int screenOffMin = 0;          // 0 = never
    /** ★★ IDLE POWER SAVE (Stuart, 2026-11-07: "slowing the waterfall data rate to the minimum when idle … and then
     *  sharpen it up and speed it up when in use") — 1 = on (the default). See App::idleNow_. */
    int idleSave = 1;
    /** ★★ CLOCK AUTO-SET (Stuart, 2026-11-08: "there should be an option for the clock to autoset from any compatible
     *  source RDS/DAB then a user doesnt have to press an option button") — 1 = on (the default). See clockAutoTick. */
    int clockAuto = 1;
    bool followVfo = true;         // VFO CENTRE: LOCKED (the view follows the dial)
    int meterMode = 0;             // 0 SNR · 1 S-units · 2 dBFS (the web's meter cycle)
    int volume = -1;               // last known (the hardware service owns it)
    int dispClock = 0, dispProven = 1;   // ✗ IGNORED since 2026-11-13 (the display-rate switcher was removed): still read so an old settings file loads
    int stickDeg = 30;             // ★ the TUNING STICK FEEL: degrees of turn a step (Stuart, 2026-11-07: "more of a turn before
                                   //   it steps so a user can dial in the feel they want") — 30 = the web's Jog
    /** ★★ REC BUFFER seconds: 0 / 30 / 60 — 30 by default and KEPT through a restart (Stuart, 2026-11-11: "make 30
     *  second record buffer the default … I enable the 60 second buffer and then when i turn the device off and on again
     *  it has reset back to 0" — it lived only in the audio sheet's memory). */
    int recBuffer = 30;
    /** ★ CW's EXPLAIN ↔ RAW (the decoder box's key): 1 = CW read in plain English (cw_plain.h), kept like the app's
     *  vibe_cw_plain. */
    int cwPlain = 0;
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
        if (speed < 2 || speed > 5) speed = 4;   // ★ LOW / MINIMUM (gone) → 15
        contrast = j["contrast"].num(contrast);
        sharpness = j["sharpness"].num(sharpness);
        wfBrightness = j["wfBrightness"].num(wfBrightness);
        specRate = (int)j["specRate"].num(specRate);
        screenOffMin = (int)j["screenOffMin"].num(screenOffMin);
        idleSave = (int)j["idleSave"].num(idleSave) ? 1 : 0;
        clockAuto = (int)j["clockAuto"].num(clockAuto) ? 1 : 0;
        followVfo = j["followVfo"].boolean(followVfo);
        meterMode = (int)j["meterMode"].num(meterMode);
        freqUnit = std::max(0, std::min(2, (int)j["freqUnit"].num(freqUnit)));
        dispClock = (int)j["dispClock"].num(dispClock);
        dispProven = (int)j["dispProven"].num(dispProven);
        stickDeg = std::max(10, std::min(180, (int)j["stickDeg"].num(stickDeg)));
        recBuffer = (int)j["recBuffer"].num(recBuffer);
        if (recBuffer != 0 && recBuffer != 30 && recBuffer != 60) recBuffer = 30;
        cwPlain = (int)j["cwPlain"].num(cwPlain) ? 1 : 0;
    }
    void save() const {
        const std::string p = path(), tmp = p + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "{\"step\":%d,\"palette\":%s,\"speed\":%d,\"contrast\":%g,\"sharpness\":%g,\"wfBrightness\":%g,"
                        "\"specRate\":%d,\"screenOffMin\":%d,\"followVfo\":%s,\"meterMode\":%d,\"freqUnit\":%d,\"dispClock\":%d,\"dispProven\":%d,\"stickDeg\":%d,\"idleSave\":%d,\"clockAuto\":%d,\"recBuffer\":%d,\"cwPlain\":%d}\n",
                     step, jsonQuote(palette).c_str(), speed, contrast, sharpness, wfBrightness, specRate, screenOffMin,
                     followVfo ? "true" : "false", meterMode, freqUnit, dispClock, dispProven, stickDeg, idleSave, clockAuto, recBuffer, cwPlain);
        std::fflush(f);
        std::fclose(f);
        std::rename(tmp.c_str(), p.c_str());
    }
};

}  // namespace hh
