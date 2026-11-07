// app.h — vibe-hhui's screens: the main receiver (scale, spectrum, waterfall, the portrait control island) and the
// web handheld page's sheets (mode, step, frequency entry, audio, the menu). See main.cpp for the scheme.
#pragma once
#include "engine.h"
#include "gfx.h"
#include "settings.h"
#include "text.h"
#include "tuning.h"
#include "ui.h"
#include "kb/controller_keyboard.h"
#include "kb/kb_layouts.h"
#include "wf/waterfall.h"   // ★ the APP's waterfall + spectrum, ported (src/wf/README.md)

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <time.h>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hh {

inline std::string fontPath(const char* name) {
    const char* dir = std::getenv("VIBE_HHUI_FONTS");
    return std::string(dir && *dir ? dir : "/usr/share/vibesdr-r36s/fonts") + "/" + name;
}
inline double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

/** The stick jog wheel — src/utils/gamepadNav.ts Jog, ported (same thresholds and speed curve). */
class Jog {
public:
    explicit Jog(double degPerStep) : per_(degPerStep) {}
    int feed(double x, double y, double tMs) {
        const double mag = std::hypot(x, y);
        if (mag < (has_ ? kRelease : kEngage)) { has_ = false; acc_ = 0; speed_ = 0; return 0; }
        const double a = std::atan2(y, x) * 180.0 / M_PI;
        if (!has_) { has_ = true; last_ = a; lastT_ = tMs; return 0; }
        double d = a - last_;
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        last_ = a;
        const double dt = std::max(1.0, tMs - lastT_) / 1000.0;
        const double v = std::fabs(d) / dt;
        if (speed_ == 0) speed_ = v;
        else speed_ += (v - speed_) * std::min(1.0, dt / (v < speed_ ? 0.04 : 0.12));
        lastT_ = tMs;
        if (mag < kCount) { acc_ = 0; return 0; }
        const double per = degPerStep(speed_);
        acc_ += d;
        const int steps = acc_ > 0 ? (int)std::floor(acc_ / per) : (int)std::ceil(acc_ / per);
        acc_ -= steps * per;
        return steps;
    }
private:
    static constexpr double kEngage = 0.6, kRelease = 0.35, kCount = 0.7;
    double per_, last_ = 0, lastT_ = 0, acc_ = 0, speed_ = 0;
    bool has_ = false;
    double degPerStep(double speed) const {
        const double slow = 120, normal = 300, fast = 450;
        if (speed <= slow) return per_ * 1.5;
        if (speed < normal) return per_ * (1.5 - 0.5 * (speed - slow) / (normal - slow));
        if (speed <= fast) return per_;
        return std::max(per_ / 3, per_ * fast / speed);
    }
};

/** A pad button, by POSITION. */
enum Btn { B_ENTER, B_BACK, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_L1, B_R1, B_L2, B_R2, B_SELECT, B_START, B_TOPFACE, B_LEFTFACE, B_NONE };

/** The demodulators the picker offers, in the web picker's order (web/client/src/main.ts MODES + amSyncOffered +
 *  drm, minus hwinfo blocked[]; a blocked "cw" removes both cwu and cwl). `srvModes` = vibeserver.json modes[]. */
struct ModeDef { std::string id, label; };
inline std::string modeLabel(const std::string& id) {
    // src/services/amSync.ts modeLegend
    static const char* sync[][2] = {{"sam", "SAM"}, {"sau", "SAM-U"}, {"sal", "SAM-L"}, {"ecssu", "ECSS-U"}, {"ecssl", "ECSS-L"}, {"amst", "AM-ST"}};
    for (auto& p : sync) if (id == p[0]) return p[1];
    return upper(id);
}
inline std::vector<ModeDef> offeredModes(const std::vector<std::string>& srvModes, const std::vector<std::string>& blocked) {
    auto has = [&](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    std::vector<std::string> ids = {"wfm", "nfm", "am"};
    for (const char* sy : {"sam", "sau", "sal", "ecssu", "ecssl", "amst"}) if (has(srvModes, sy)) ids.push_back(sy);
    for (const char* m : {"usb", "lsb", "cwu", "cwl"}) ids.push_back(m);
    if (has(srvModes, "drm")) ids.push_back("drm");
    if (has(srvModes, "dab")) ids.push_back("dab");   // (vibeserver.json dab === true, added to srvModes at the fetch)
    std::vector<ModeDef> out;
    for (auto& id : ids) {
        if (has(blocked, id)) continue;
        if ((id == "cwu" || id == "cwl") && has(blocked, "cw")) continue;
        out.push_back({id, modeLabel(id)});
    }
    return out;
}

/** The on-screen keyboard, asked for by any field (src/kb — the app's controller keyboard, ported). */
struct KbRequest {
    kb::SessionOpts opts;
    std::string title;                                      // e.g. "FREQUENCY", "SET UTC", "BOOKMARK NAME"
    std::function<std::string(const kb::KbState&)> commit;  // ↵: act; return a refusal to keep the keyboard open
    /** ★ Shown dimmed while the field is empty: the web SELECTS the field's value when the pad opens, so the first
     *  key replaces it. The pad opens empty with the old value here as the hint (↵ on an empty field changes nothing). */
    std::string placeholder;
};

class App;
inline void dispApply(App& a);

// ★ Everything is public: the menu's parts (sec_*.h) are App members in their own files, and read its state directly.
class App {
public:
    int run(int argc, char** argv);

    // ── platform ──
    int W = 640, H = 480;
    SDL_Window* win_ = nullptr;
    SDL_GameController* pad_ = nullptr;
    SDL_Joystick* joy_ = nullptr;
    Gfx g_;
    TextCache tc_;
    Fonts f_;
    Nav nav_;
    wf::Waterfall wfp_;
    Engine* eng_ = nullptr;
    Settings set_;
    bool quit_ = false;

    // ── state ──
    enum Sheet { S_NONE, S_MODE, S_STEP, S_MENU, S_AUDIO, S_KB, S_BOOKMARKS, S_SEARCH, S_DAB };
    std::vector<Sheet> sheets_;
    std::deque<Btn> presses_;
    Jog tuneJog_{30}, zoomJog_{45};
    bool l2Down_ = false, r2Down_ = false;
    double sigSmooth_ = 0, snrSmooth_ = 0, lastSigDb_ = -160;
    int battery_ = -1; bool charging_ = false;
    int volume_ = -1; bool muted_ = false; double volUntil_ = 0;
    int brightness_ = 120;
    std::atomic<double> cpuMaxSeen_{0};   // the CPU SPEED cap the hardware service reported (kHz), handed to num_ on the UI thread
    bool screenOff_ = false;
    double lastInput_ = 0;
    std::string toast_; double toastUntil_ = 0;
    double armedPower_ = 0, armedReboot_ = 0;
    // the menu
    std::string menuOpen_;           // the open accordion ("" = all folded)
    Scroller menuScroll_;
    // the view follows the dial (VFO CENTRE: LOCKED) — paced like spectrum.ts _sendView (≤ 1 per 33 ms, trailing edge)
    bool viewPending_ = false; double viewF_ = 0, viewSpan_ = 0, viewSentAt_ = 0;
    // the on-screen keyboard (one at a time, over everything)
    kb::Session kbd_;
    KbRequest kbReq_;
    // ★ scratch state for the menu's parts (sec_*.h): keyed by the part's own prefix ("audio.sqlAuto", "sdr.rate" …)
    std::map<std::string, double> num_;
    std::map<std::string, std::string> str_;
    Scroller audioScroll_;
    // the server's offer (GET /vibeserver.json) and the device's location (GET /handheld/location) — fetched off-thread
    std::mutex srvMtx_;
    std::vector<std::string> srvModes_;
    bool haveLon_ = false; double lon_ = 0;
    int region() { std::lock_guard<std::mutex> lk(srvMtx_); return ituRegion(haveLon_, lon_); }
    bool settingsDirty_ = false; double settingsAt_ = 0;

    // ── actions ──
    void tuneTo(double f);
    void tuneSteps(int n);
    void zoomSteps(int n);
    void setStep(int s) { set_.step = s; dirty(); }
    void dirty() { settingsDirty_ = true; settingsAt_ = nowSec(); }
    void toast(const std::string& s) { toast_ = s; toastUntil_ = nowSec() + 2.5; }
    void toastLong(const std::string& s) { toast_ = s; toastUntil_ = nowSec() + 5.0; }
    void open(Sheet s) { sheets_.push_back(s); }
    void closeTop() { if (!sheets_.empty()) { nav_.forget(layerName(sheets_.back())); sheets_.pop_back(); } }
    static const char* layerName(Sheet s) {
        switch (s) { case S_MODE: return "mode"; case S_STEP: return "step"; case S_MENU: return "menu"; case S_AUDIO: return "audio";
                     case S_KB: return "kb"; case S_BOOKMARKS: return "bm"; case S_SEARCH: return "search"; case S_DAB: return "dab"; default: return "deck"; }
    }
    void applyLook() {
        wfp_.setPalette(set_.palette);
        wfp_.setSpeed((wf::Scroll)std::max(0, std::min(5, set_.speed)));
        wfp_.setContrast(set_.contrast);
        wfp_.setSharpness(set_.sharpness);
        wfp_.setBrightness(set_.wfBrightness);
        dispApply(*this);   // DISPLAY's own look settings + the custom colour map (disp_settings.h)
    }
    void press(Btn b);
    void pollInput();
    void updateSignal(const uint8_t* bins, int n, double centreHz, double spanHz);

    // ── drawing ──
    void drawBands(Ctx& c, const RadioState& s, Rect r);
    void drawVts(Ctx& c, const RadioState& s, Rect r);   // vts.h — the app's VTS bar
    void drawScale(Ctx& c, const RadioState& s, Rect r);
    void drawOverlays(Ctx& c, const RadioState& s, Rect spec, Rect wf);
    void drawDeck(Ctx& c, const RadioState& s, Rect r);
    void drawModeSheet(Ctx& c, const RadioState& s);
    void drawStepSheet(Ctx& c, const RadioState& s);
    void drawKeyboard(Ctx& c, const RadioState& s);
    void kbInput(Btn b);
    // BOOKMARKS + SEARCH (bm_page.h, search_page.h) — reached from the frequency pad's two keys
    void drawBookmarks(Ctx& c, const RadioState& s);
    void drawSearch(Ctx& c, const RadioState& s);
    void drawSearchLive(Ctx& c, Rect r);
    void drawPadLinks(Ctx& c, Rect r);
    bool padLinksInput(Btn b);
    bool padLinkFocused() const;
    void openKeyboard(const KbRequest& r) { kbReq_ = r; kbd_.open(r.opts); open(S_KB); }
    void openFreqEntry();
    /** Tune to a typed frequency: the radio's ranges, then tune. Returns a refusal ("" = done). */
    std::string tuneTyped(double hz);
    /** web main.ts clampTune: inside a range = as asked; in a GAP the first nudge parks on the edge and says why, a
     *  second nudge the same way jumps the gap (so a fast scroll never flings you into another band by accident). */
    double clampTune(double hz);
    int gapNudgeDir_ = 0;
    // the whole device's CPU (/proc/stat, once a second) for the stats row
    int cpuPct_ = -1; long cpuKHz_ = 0; double cpuAt_ = 0; unsigned long long cpuBusy_ = 0, cpuTotal_ = 0;
    void sampleCpu();
    // ★ HOLD THE LEFT STICK LEFT / RIGHT = SWEEP TUNE (Stuart, 2026-11-06: spinning is great for small precise
    //   adjustments, "large tunes with them hurt"). The web's hold-sweep (main.ts attachHoldSweep): 350 ms hold, then
    //   3 → 15 steps a second over 3.5 s, each step waiting for the receiver's echo (700 ms timeout).
    double lx_ = 0, ly_ = 0, testStick_ = 0;
    int sweepDir_ = 0; double sweepZoneAt_ = 0, sweepZoneAng_ = 0, sweepStart_ = 0, sweepNext_ = 0, sweepEchoAt_ = 0;
    bool sweepAwaiting_ = false; uint64_t sweepSeq_ = 0;
    void stickSweep();
    // ★ PROFILE: the UI thread's own CPU per stage (CLOCK_THREAD_CPUTIME_ID), summed and logged every 30 s — the
    //   handheld13 log had vibe-hhui at 77–85 % of a core on FM, far more than 30 fps of drawing should cost
    struct Prof { double pre = 0, wf = 0, ui = 0, swap = 0, idle = 0; long loops = 0, frames = 0, batches = 0; double since = 0; } prof_;
    static double threadCpu() { timespec ts{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
    void drawAudioSheet(Ctx& c, const RadioState& s);
    void drawMenu(Ctx& c, const RadioState& s);
    void drawNoRadio(Ctx& c, const RadioState& s);
    void drawToasts(Ctx& c);
    // the menu's parts
    float menuRow(Ctx& c, Rect area, float y, const std::string& label, const std::string& help);
    float secHardware(Ctx& c, const RadioState& s, Rect area, float y);
    float secSdr(Ctx& c, const RadioState& s, Rect area, float y);
    float secDisplay(Ctx& c, const RadioState& s, Rect area, float y);
    float secController(Ctx& c, const RadioState& s, Rect area, float y);
};

// the AUDIO sheet's per-frame work (sec_audio.h): auto squelch, the recording tap, re-sends on reconnect
void audioTick(App&, const RadioState&);
void audioSpecFrame(App&, const uint8_t*, int, double, double);
// DISPLAY RATE (display_rate.h): the panel's modes, live, with keep-or-revert
inline bool dispRatePress(App&, Btn);
inline void dispRateTick(App&);
inline void dispRateDraw(App&, Ctx&);
inline void dispRateStartup(App&);
inline float dispRateRow(App&, Ctx&, Rect, float);
// per-radio memory (memory.h): restore on plug-in, save what changed
inline void memTick(App&, const RadioState&);
// the deck's REC key (sec_audio.h): recording now? / start or stop it
bool deckRecording(App&);
void deckToggleRec(App&, const RadioState&);

inline int runApp(int argc, char** argv) { App a; return a.run(argc, argv); }

}  // namespace hh

#include "hw_pump.h"
#include "dab_box.h"
#include "app_impl.h"
#include "sec_audio.h"
#include "sec_display.h"
#include "sec_hardware.h"
#include "sec_sdr.h"
#include "sec_controller.h"
#include "bm_page.h"
#include "search_page.h"
#include "vts.h"
#include "memory.h"
#include "display_rate.h"
