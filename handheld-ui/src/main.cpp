// vibe-hhui — the R36S's native receiver UI (docs/v12/R36S-NATIVE-UI.md). SDL2 on KMSDRM + OpenGL ES 3 on Panfrost,
// drawn straight to the 640×480 panel; a client of the handheld engine on 127.0.0.1:48000 like every VibeSDR client.
//
// ★★ CONTROLS — Stuart's scheme, BY POSITION (memory: gamepad_mapping), two-stick pads (the R36S):
//    D-pad = move the highlight · bottom face = enter · right face = back
//    LEFT stick turned like a jog wheel = tune (clockwise up) · RIGHT stick turned = zoom (clockwise in)
//    L / R shoulders = zoom out / in · L2 / R2 = tune down / up one step · SELECT = the menu
// ★★★ Hints use Nintendo Switch-style glyphs: four circles, the meant one solid (padFace) — never a letter.
// ★ Draws at most 30 frames a second (Stuart's cap on the handheld), nothing at all while the screen is off.
#include "engine.h"
#include "gfx.h"
#include "text.h"
#include "wf/waterfall.h"   // ★ the APP's waterfall + spectrum, ported (src/wf/README.md)

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace hh;

namespace {

// ── look ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
const Rgba kBg = rgb(0x070907), kPanel = rgb(0x10140f), kText = rgb(0xe8f0d8), kDim = rgb(0x8a9a80),
           kAccent = rgb(0xffb000), kHi = rgb(0x5cf2ff), kVfd = rgb(0x6cffb0), kWarn = rgb(0xff5040);

std::string fontPath(const char* name) {
    const char* dir = std::getenv("VIBE_HHUI_FONTS");
    return std::string(dir && *dir ? dir : "/usr/share/vibesdr-r36s/fonts") + "/" + name;
}

/** ★ Switch-style face glyph: a diamond of four circles, the meant one solid. pos: 0 top, 1 left, 2 right, 3 bottom. */
void padFace(Gfx& g, float cx, float cy, float size, int solid, Rgba c) {
    const float r = size * 0.16f, d = size * 0.30f;
    const float px[4] = {0, -d, d, 0}, py[4] = {-d, 0, 0, d};
    for (int i = 0; i < 4; i++) g.circle(cx + px[i], cy + py[i], r, i == solid ? 0.f : 1.2f, c);
}
enum { FACE_TOP = 0, FACE_LEFT = 1, FACE_RIGHT = 2, FACE_BOTTOM = 3 };

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

/** The frequency as the VFD shows it: MHz with the kHz digits grouped, the units in the segments (capitals). */
std::string vfdFreq(double hz) {
    char b[48];
    const long long h = std::llround(hz);
    std::snprintf(b, sizeof b, "%lld.%03lld.%03lld", h / 1000000, (h / 1000) % 1000, h % 1000);
    return b;
}
std::string bwText(int lo, int hi) {
    const int w = hi - lo;
    char b[32];
    if (w >= 10000) std::snprintf(b, sizeof b, "%.0fk", w / 1000.0);
    else if (w >= 1000) std::snprintf(b, sizeof b, "%.1fk", w / 1000.0);
    else std::snprintf(b, sizeof b, "%d", w);
    return b;
}
std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

// ── tuning steps (Hz) and the default per mode ───────────────────────────────────────────────────────────────────
const int kSteps[] = {10, 50, 100, 500, 1000, 2500, 5000, 9000, 10000, 12500, 25000, 100000};
int defaultStep(const std::string& m) {
    if (m == "usb" || m == "lsb") return 100;
    if (m == "cwu" || m == "cwl") return 10;
    if (m == "wfm") return 100000;
    if (m == "nfm" || m == "fm") return 12500;
    return 5000;   // AM family, DRM
}
const char* kModes[] = {"am", "sam", "usb", "lsb", "cwu", "nfm", "wfm"};

struct Ui {
    // focus: the top row of controls
    enum Focus { FREQ, MODE, BW, STEP, MENU, COUNT } focus = FREQ;
    bool editing = false;                 // enter on a control: the D-pad adjusts it
    int step = 5000;
    std::string stepMode;                 // the mode `step` was defaulted for
    // overlays
    double volUntil = 0; int vol = -1; bool muted = false;
    double toastUntil = 0; std::string toast;
    int battery = -1; bool charging = false;
    bool screenOff = false;
    bool menuOpen = false; int menuSel = 0; double armedPower = 0, armedReboot = 0;
    int brightness = 120;
    // spectrum averaging (display only)
    std::vector<float> trace;
    float loDb = -120, hiDb = -40;        // the waterfall's colour range (auto-tracked)
};

}  // namespace

int main(int argc, char** argv) {
    int port = 48000;
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
    const char* shot = std::getenv("VIBE_HHUI_SCREENSHOT");     // ★ tests: write a PPM of frame N and exit
    const int shotFrame = std::getenv("VIBE_HHUI_SHOT_FRAME") ? std::atoi(std::getenv("VIBE_HHUI_SHOT_FRAME")) : 90;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "vibe-hhui: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) { std::fprintf(stderr, "vibe-hhui: TTF_Init: %s\n", TTF_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_DisplayMode dm{};
    int W = 640, H = 480;
    if (!shot && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0) { W = dm.w; H = dm.h; }   // ★ tests: 640×480, the panel
    SDL_Window* win = SDL_CreateWindow("VibeSDR", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                                       SDL_WINDOW_OPENGL | (shot ? 0 : SDL_WINDOW_FULLSCREEN));
    if (!win) { std::fprintf(stderr, "vibe-hhui: window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { std::fprintf(stderr, "vibe-hhui: GL context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_DISABLE);
    std::fprintf(stderr, "vibe-hhui: %dx%d, %s / %s\n", W, H, (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));

    Gfx g; g.init(W, H);
    TextCache tc;
    Font fUi, fUiSmall, fVfd, fBig;
    fUi.load(fontPath("JetBrainsMono-Bold.ttf"), 16);
    fUiSmall.load(fontPath("JetBrainsMono-Regular.ttf"), 13);
    fBig.load(fontPath("JetBrainsMono-Bold.ttf"), 28);
    fVfd.load(fontPath("DSEG14Classic-BoldItalic.ttf"), 32);
    Font fVfdSmall; fVfdSmall.load(fontPath("DSEG14Classic-BoldItalic.ttf"), 14);
    // ★★★ THE APP'S WATERFALL (memory: feedback_waterfall_core_rules): per-row placement, the as-drawn zoom fan, every tune
    //     eased, the fixed output clock (DEFAULT = 30 rows/s, never 20), the app's palettes (Jet, its production default).
    wf::Waterfall wfp;
    if (!wfp.init()) std::fprintf(stderr, "vibe-hhui: the waterfall did not initialise\n");
    wfp.setPalette("Jet");
    wfp.setSpeed(wf::Scroll::Default);

    Engine eng(port);
    eng.start();
    Ui ui;

    // ── the pad ──
    SDL_GameController* pad = nullptr;
    SDL_Joystick* joy = nullptr;
    auto openPads = [&] {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) { if (!pad) { pad = SDL_GameControllerOpen(i); if (pad) std::fprintf(stderr, "vibe-hhui: pad %s\n", SDL_GameControllerName(pad)); } }
            else if (!joy) { joy = SDL_JoystickOpen(i); if (joy) std::fprintf(stderr, "vibe-hhui: joystick %s (no mapping)\n", SDL_JoystickName(joy)); }
        }
    };
    openPads();
    Jog tuneJog(30), zoomJog(45);

    auto now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    auto setToast = [&](const std::string& s) { ui.toast = s; ui.toastUntil = now() + 2.5; };

    // ── actions ──
    auto tuneSteps = [&](int n) {
        const RadioState& s = eng.state();
        if (s.session != "running" || n == 0) return;
        double f = s.vfoHz + (double)n * ui.step;
        f = std::round(f / ui.step) * ui.step;                // ★ on the step grid, like the app's dial
        if (f < 1000) f = 1000;
        eng.tune(f);
        // ★ the view moves with the dial before the server's frames catch up (the app's predicted view)
        wfp.setView(s.viewCentreHz + (f - s.vfoHz), s.spanHz);
        wfp.notifyInteraction(now());
    };
    auto zoomSteps = [&](int n) {
        const RadioState& s = eng.state();
        if (s.session != "running" || n == 0 || s.spanHz <= 0) return;
        // ★ ×√2 a press on VibeServer (the app's zoom keys, Stuart 2026-10-28)
        double span = s.spanHz * std::pow(std::sqrt(0.5), n);
        const double minSpan = 3000;
        if (span > s.maxSpanHz * 0.999 && s.maxSpanHz > 0) { eng.zoomReset(); wfp.notifyInteraction(now()); return; }
        if (span < minSpan) span = minSpan;
        eng.zoom(s.vfoHz, span);
        wfp.setView(s.vfoHz, span);
        wfp.notifyInteraction(now());
    };
    auto cycle = [&](int dir) {
        const RadioState& s = eng.state();
        switch (ui.focus) {
            case Ui::FREQ: tuneSteps(dir); break;
            case Ui::MODE: {
                int i = 0;
                const int n = (int)(sizeof kModes / sizeof *kModes);
                for (int k = 0; k < n; k++) if (s.mode == kModes[k]) i = k;
                for (int tries = 0; tries < n; tries++) {
                    i = (i + dir + n) % n;
                    if (std::find(s.blockedModes.begin(), s.blockedModes.end(), kModes[i]) == s.blockedModes.end()) break;
                }
                eng.setMode(kModes[i]);
                ui.step = defaultStep(kModes[i]); ui.stepMode = kModes[i];
                break;
            }
            case Ui::BW: {
                int lo = s.bwLow, hi = s.bwHigh;
                const double f = dir > 0 ? 1.25 : 0.8;
                if (lo < 0 && hi > 0) { const int h = std::max(100, (int)std::lround(hi * f)); lo = -h; hi = h; }
                else if (hi > 0) hi = std::max(lo + 100, (int)std::lround(hi * f));
                else lo = std::min(hi - 100, (int)std::lround(lo * f));
                eng.setBandwidth(lo, hi);
                break;
            }
            case Ui::STEP: {
                const int n = (int)(sizeof kSteps / sizeof *kSteps);
                int i = 0;
                for (int k = 0; k < n; k++) if (kSteps[k] == ui.step) i = k;
                ui.step = kSteps[std::max(0, std::min(n - 1, i + dir))];
                break;
            }
            default: break;
        }
    };
    // the menu
    struct Item { const char* label; };
    const Item menuItems[] = {{"BRIGHTNESS"}, {"SPECTRUM RATE"}, {"ZOOM OUT FULLY"}, {"POWER OFF"}, {"RESTART"}};
    const int menuN = (int)(sizeof menuItems / sizeof *menuItems);
    int specRate = 20;
    auto menuAdjust = [&](int dir) {
        if (ui.menuSel == 0) { ui.brightness = std::max(10, std::min(255, ui.brightness + dir * 15)); eng.device("{\"set\":\"brightness\",\"value\":" + std::to_string(ui.brightness) + "}"); }
        if (ui.menuSel == 1) { const int r[] = {5, 10, 20}; int i = specRate == 5 ? 0 : specRate == 10 ? 1 : 2; i = std::max(0, std::min(2, i + dir)); specRate = r[i]; eng.fftRate(specRate); }
    };
    auto menuEnter = [&] {
        const double t = now();
        if (ui.menuSel == 2) { eng.zoomReset(); ui.menuOpen = false; }
        if (ui.menuSel == 3) { if (t - ui.armedPower < 3) eng.device("{\"action\":\"poweroff\"}"), setToast("Switching off"); else { ui.armedPower = t; setToast("Press again to switch off"); } }
        if (ui.menuSel == 4) { if (t - ui.armedReboot < 3) eng.device("{\"action\":\"reboot\"}"), setToast("Restarting"); else { ui.armedReboot = t; setToast("Press again to restart"); } }
    };

    enum Btn { B_ENTER, B_BACK, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_L1, B_R1, B_L2, B_R2, B_SELECT, B_START, B_TOPFACE, B_LEFTFACE, B_NONE };
    auto press = [&](Btn b) {
        if (ui.screenOff) { ui.screenOff = false; eng.device("{\"action\":\"screen-on\"}"); return; }
        if (ui.menuOpen) {
            if (b == B_UP) ui.menuSel = (ui.menuSel + menuN - 1) % menuN;
            else if (b == B_DOWN) ui.menuSel = (ui.menuSel + 1) % menuN;
            else if (b == B_LEFT) menuAdjust(-1);
            else if (b == B_RIGHT) menuAdjust(1);
            else if (b == B_ENTER) menuEnter();
            else if (b == B_BACK || b == B_SELECT) ui.menuOpen = false;
            return;
        }
        switch (b) {
            case B_LEFT:  if (ui.editing || ui.focus == Ui::FREQ) cycle(-1); else ui.focus = (Ui::Focus)((ui.focus + Ui::COUNT - 1) % Ui::COUNT); break;
            case B_RIGHT: if (ui.editing || ui.focus == Ui::FREQ) cycle(1); else ui.focus = (Ui::Focus)((ui.focus + 1) % Ui::COUNT); break;
            case B_UP:    if (ui.editing) cycle(1); else ui.focus = (Ui::Focus)((ui.focus + Ui::COUNT - 1) % Ui::COUNT); break;
            case B_DOWN:  if (ui.editing) cycle(-1); else ui.focus = (Ui::Focus)((ui.focus + 1) % Ui::COUNT); break;
            case B_ENTER: if (ui.focus == Ui::MENU) { ui.menuOpen = true; ui.menuSel = 0; } else ui.editing = !ui.editing; break;
            case B_BACK:  ui.editing = false; break;
            case B_L1: zoomSteps(-1); break;
            case B_R1: zoomSteps(1); break;
            case B_L2: tuneSteps(-1); break;
            case B_R2: tuneSteps(1); break;
            case B_SELECT: ui.menuOpen = true; ui.menuSel = 0; break;
            default: break;
        }
    };
    auto fromController = [](int b) -> Btn {
        switch (b) {
            case SDL_CONTROLLER_BUTTON_A: return B_ENTER;          // ★ SDL's A is the BOTTOM position
            case SDL_CONTROLLER_BUTTON_B: return B_BACK;           //   …and B the RIGHT one, whatever the printing
            case SDL_CONTROLLER_BUTTON_Y: return B_TOPFACE;
            case SDL_CONTROLLER_BUTTON_X: return B_LEFTFACE;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return B_UP;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return B_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return B_LEFT;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return B_RIGHT;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return B_L1;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return B_R1;
            case SDL_CONTROLLER_BUTTON_BACK: return B_SELECT;
            case SDL_CONTROLLER_BUTTON_START: return B_START;
            default: return B_NONE;
        }
    };
    // ★ a pad SDL has no mapping for: the R36S joypad's evdev order (BTN_SOUTH 0x130 … THUMBR 0x13e, then the D-pad)
    auto fromJoystick = [](int b) -> Btn {
        switch (b) {
            case 0: return B_ENTER;  case 1: return B_BACK;  case 2: return B_TOPFACE; case 3: return B_LEFTFACE;
            case 4: return B_L1;     case 5: return B_R1;    case 6: return B_L2;      case 7: return B_R2;
            case 8: return B_SELECT; case 9: return B_START;
            case 13: return B_UP;    case 14: return B_DOWN; case 15: return B_LEFT;   case 16: return B_RIGHT;
            default: return B_NONE;
        }
    };
    bool l2Down = false, r2Down = false;
    // ★ tests: VIBE_HHUI_SCRIPT="frame:BUTTON,…" presses buttons at drawn frames (ENTER BACK UP DOWN LEFT RIGHT L1 R1 L2 R2 SELECT)
    std::vector<std::pair<int, Btn>> script;
    if (const char* sc = std::getenv("VIBE_HHUI_SCRIPT")) {
        const std::pair<const char*, Btn> names[] = {{"ENTER", B_ENTER}, {"BACK", B_BACK}, {"UP", B_UP}, {"DOWN", B_DOWN}, {"LEFT", B_LEFT},
            {"RIGHT", B_RIGHT}, {"L1", B_L1}, {"R1", B_R1}, {"L2", B_L2}, {"R2", B_R2}, {"SELECT", B_SELECT}};
        std::string all(sc), tok;
        for (size_t i = 0; i <= all.size(); i++) {
            if (i < all.size() && all[i] != ',') { tok += all[i]; continue; }
            const size_t c = tok.find(':');
            if (c != std::string::npos) for (auto& n : names) if (tok.substr(c + 1) == n.first) script.push_back({std::atoi(tok.c_str()), n.second});
            tok.clear();
        }
    }

    // ── the loop ──
    uint64_t frame = 0;
    double lastDraw = 0, lastDevPoll = 0;
    bool quit = false;
    std::vector<DeviceEvent> devEvents;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
            else if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_JOYDEVICEADDED) openPads();
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) press(fromController(e.cbutton.button));
            else if (e.type == SDL_JOYBUTTONDOWN && joy && !pad) press(fromJoystick(e.jbutton.button));
            else if (e.type == SDL_JOYHATMOTION && joy && !pad) {
                if (e.jhat.value & SDL_HAT_UP) press(B_UP);
                if (e.jhat.value & SDL_HAT_DOWN) press(B_DOWN);
                if (e.jhat.value & SDL_HAT_LEFT) press(B_LEFT);
                if (e.jhat.value & SDL_HAT_RIGHT) press(B_RIGHT);
            } else if (e.type == SDL_CONTROLLERAXISMOTION && pad) {
                // the analogue triggers as buttons (L2 / R2)
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) { const bool d = e.caxis.value > 16000; if (d && !l2Down) press(B_L2); l2Down = d; }
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) { const bool d = e.caxis.value > 16000; if (d && !r2Down) press(B_R2); r2Down = d; }
            } else if (e.type == SDL_KEYDOWN) {           // ★ a USB keyboard: the keyboard scheme (arrows, Enter, Backspace/Esc, < > tune, - + zoom)
                switch (e.key.keysym.sym) {
                    case SDLK_UP: press(B_UP); break; case SDLK_DOWN: press(B_DOWN); break;
                    case SDLK_LEFT: press(B_LEFT); break; case SDLK_RIGHT: press(B_RIGHT); break;
                    case SDLK_RETURN: press(B_ENTER); break; case SDLK_BACKSPACE: case SDLK_ESCAPE: press(B_BACK); break;
                    case SDLK_COMMA: tuneSteps(-1); break; case SDLK_PERIOD: tuneSteps(1); break;
                    case SDLK_MINUS: zoomSteps(-1); break; case SDLK_EQUALS: case SDLK_PLUS: zoomSteps(1); break;
                    case SDLK_TAB: press(B_SELECT); break;
                    default: break;
                }
            }
        }
        // the sticks: jog wheels (sampled every loop, as the web page samples its pad each frame)
        {
            const double tMs = now() * 1000.0;
            double lx = 0, ly = 0, rx = 0, ry = 0;
            if (pad) {
                lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0;
                ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0;
                rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0;
                ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0;
            } else if (joy && SDL_JoystickNumAxes(joy) >= 4) {
                lx = SDL_JoystickGetAxis(joy, 0) / 32767.0; ly = SDL_JoystickGetAxis(joy, 1) / 32767.0;
                rx = SDL_JoystickGetAxis(joy, 2) / 32767.0; ry = SDL_JoystickGetAxis(joy, 3) / 32767.0;
            }
            if (!ui.menuOpen && !ui.screenOff) {
                tuneSteps(tuneJog.feed(lx, ly, tMs));
                zoomSteps(zoomJog.feed(rx, ry, tMs));
            }
        }

        // the engine
        devEvents.clear();
        bool newRow = false;
        eng.pump([&](const uint8_t* bins, int n, double centreHz, double) {
            // ★ bins are low → high already (engine.h rotated them); the span is this view's (config.totalBandwidth)
            wfp.pushFrame(bins, n, false, centreHz, eng.state().spanHz, now());
            newRow = true;
        }, devEvents);
        for (auto& de : devEvents) {
            if (de.kind == "volume") { ui.vol = (int)de.body["volume"].num(-1); ui.muted = de.body["muted"].boolean(); ui.volUntil = now() + 1.5; }
            else if (de.kind == "battery") { ui.battery = (int)de.body["pct"].num(ui.battery); ui.charging = de.body["charging"].boolean(); }
            else if (de.kind == "battery_low") setToast("Battery low — " + std::to_string((int)de.body["pct"].num()) + " %");
            else if (de.kind == "output") setToast(de.body["output"].str() == "headphones" ? "Headphones" : "Speaker");
            else if (de.kind == "screen") ui.screenOff = de.body["screen"].str() == "off";
            else if (de.kind == "shutdown") setToast("Switching off");
        }
        const RadioState& s = eng.state();
        if (!s.mode.empty() && s.mode != ui.stepMode) { ui.step = defaultStep(s.mode); ui.stepMode = s.mode; }
        // the battery, once a minute (and at the start)
        if (now() - lastDevPoll > 60 || lastDevPoll == 0) {
            lastDevPoll = now();
            std::thread([&ui, p = eng.port()] {
                const HttpResult r = httpRequest(p, "GET", "/handheld/device", "", 4);
                if (r.status == 200) { const Json j = Json::parse(r.body)["state"]; if (j.has("battery")) { ui.battery = (int)j["battery"]["pct"].num(-1); ui.charging = j["battery"]["charging"].boolean(); }
                    if (j.has("brightness")) ui.brightness = (int)j["brightness"].num(ui.brightness); }
            }).detach();
        }

        // ── draw (≤ 30 fps; nothing while the screen is off) ──
        const double t = now();
        if (ui.screenOff || t - lastDraw < 1.0 / 30.0 - 0.002) { SDL_Delay(4); continue; }
        lastDraw = t;
        frame++;
        for (auto& sp : script) if (sp.first == (int)frame) { std::fprintf(stderr, "vibe-hhui: script press %d at frame %d\n", (int)sp.second, sp.first); press(sp.second); }
        tc.tick(frame);
        glViewport(0, 0, W, H);
        glClearColor(kBg.r, kBg.g, kBg.b, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        const float topH = 24, freqH = 56, specY = topH + freqH, specH = 110, wfY = specY + specH, hintH = 24;
        const float wfH = H - wfY - hintH;

        // status row
        g.rect(0, 0, (float)W, topH, kPanel);
        tc.draw(g, fUiSmall, s.radioName.empty() ? "VibeSDR" : s.radioName, 8, 4, kDim);
        {
            char clk[32];
            const std::time_t tt = std::time(nullptr);
            std::tm u{}; gmtime_r(&tt, &u);
            std::snprintf(clk, sizeof clk, "%02d:%02d UTC", u.tm_hour, u.tm_min);
            std::string right = clk;
            if (ui.battery >= 0) right = std::to_string(ui.battery) + " %" + (ui.charging ? " +" : "") + "   " + right;
            tc.draw(g, fUiSmall, right, (float)W - 8, 4, ui.battery >= 0 && ui.battery <= 10 && !ui.charging ? kWarn : kDim, 2);
        }

        if (s.session != "running") {
            // ── no radio / starting ──
            const char* msg = s.session == "none" ? "Please connect an SDR" : s.session == "starting" ? "Starting the radio…" : "Waiting for the receiver…";
            tc.draw(g, fBig, msg, W / 2.f, H / 2.f - 30, kText, 1);
            if (s.session == "starting" && !s.radioName.empty()) tc.draw(g, fUi, s.radioName, W / 2.f, H / 2.f + 10, kDim, 1);
        } else {
            // ── the readout row: FREQ  MODE  BW  STEP  MENU ──
            struct Cell { Ui::Focus f; float x, w; };
            const Cell cells[] = {{Ui::FREQ, 6, 300}, {Ui::MODE, 312, 72}, {Ui::BW, 388, 78}, {Ui::STEP, 470, 86}, {Ui::MENU, 560, 74}};
            for (auto& c : cells) {
                const bool f = ui.focus == c.f;
                if (f) g.frame(c.x, topH + 4, c.w, freqH - 8, 2, ui.editing ? kAccent : kHi);
            }
            {   // ★ the VFD: MHz, the units IN THE SEGMENTS, capitals (memory: vfd_units_in_segments)
                const int fw = tc.draw(g, fVfd, vfdFreq(s.vfoHz), 14, topH + 10, kVfd);
                tc.draw(g, fVfdSmall, "MHZ", 14 + fw + 8, topH + 24, kVfd);
            }
            tc.draw(g, fUiSmall, "MODE", 348, topH + 8, kDim, 1);
            tc.draw(g, fUi, upper(s.mode), 348, topH + 26, kText, 1);
            tc.draw(g, fUiSmall, "BW", 427, topH + 8, kDim, 1);
            tc.draw(g, fUi, bwText(s.bwLow, s.bwHigh), 427, topH + 26, kText, 1);
            tc.draw(g, fUiSmall, "STEP", 513, topH + 8, kDim, 1);
            tc.draw(g, fUi, ui.step >= 1000 ? (ui.step % 1000 ? std::to_string(ui.step / 1000) + "." + std::to_string(ui.step % 1000 / 100) + "k" : std::to_string(ui.step / 1000) + "k") : std::to_string(ui.step), 513, topH + 26, kText, 1);
            tc.draw(g, fUi, "MENU", 597, topH + 18, kText, 1);

            // ── spectrum + waterfall: the app's (waterfall FIRST — it runs the clock and the pace) ──
            g.flush();
            wfp.draw(0, (int)wfY, W, (int)wfH, W, H, t);
            wfp.drawSpectrum(0, (int)specY, W, (int)specH, W, H);
            glEnable(GL_BLEND);
            const double lo = s.viewCentreHz - s.spanHz / 2, hi = s.viewCentreHz + s.spanHz / 2;
            auto xOf = [&](double hz) { return (float)((hz - lo) / (hi - lo) * W); };
            if (s.spanHz > 0) {
                // the passband
                const float px0 = xOf(s.vfoHz + s.bwLow), px1 = xOf(s.vfoHz + s.bwHigh);
                g.rect(px0, specY, std::max(1.f, px1 - px0), specH, rgb(0xffb000, 0.12f));
                const float vx = xOf(s.vfoHz);
                g.rect(vx - 0.5f, specY, 1.f, specH, rgb(0xff4040, 0.9f));
                // signal level
                char sl[48];
                std::snprintf(sl, sizeof sl, "%.0f dB", s.chanDb);
                tc.draw(g, fUiSmall, sl, (float)W - 6, specY + 4, kDim, 2);
                if (!s.rdsPs.empty()) tc.draw(g, fUi, s.rdsPs + (s.rdsStereo ? "  STEREO" : ""), 8, specY + 4, kAccent);
            }
            if (s.spanHz > 0) { const float vx = xOf(s.vfoHz); g.rect(vx - 0.5f, wfY, 1.f, wfH, rgb(0xff4040, 0.35f)); }
        }

        // ── the hint bar (Switch-style glyphs) ──
        g.rect(0, (float)H - hintH, (float)W, hintH, kPanel);
        {
            float x = 10;
            const float y = H - hintH / 2.f;
            auto hint = [&](int face, const std::string& what) {
                padFace(g, x + 9, y, 18, face, kText);
                x += 22;
                x += tc.draw(g, fUiSmall, what, x, y - 8, kDim) + 16;
            };
            if (ui.menuOpen) { hint(FACE_BOTTOM, "select"); hint(FACE_RIGHT, "close"); tc.draw(g, fUiSmall, "D-pad left / right adjust", x, y - 8, kDim); }
            else if (ui.editing) { hint(FACE_BOTTOM, "done"); hint(FACE_RIGHT, "done"); tc.draw(g, fUiSmall, "D-pad adjust · stick tune · L/R zoom", x, y - 8, kDim); }
            else { hint(FACE_BOTTOM, "adjust"); tc.draw(g, fUiSmall, "D-pad move · left stick tune · right stick / L R zoom", x, y - 8, kDim); }
        }

        // ── overlays ──
        if (ui.menuOpen) {
            const float mw = 360, mh = 40 + menuN * 34.f, mx = (W - mw) / 2, my = (H - mh) / 2;
            g.rect(0, 0, (float)W, (float)H, rgb(0x000000, 0.55f));
            g.rect(mx, my, mw, mh, kPanel);
            g.frame(mx, my, mw, mh, 2, kAccent);
            tc.draw(g, fUi, "R36S HARDWARE", mx + mw / 2, my + 10, kAccent, 1);
            for (int i = 0; i < menuN; i++) {
                const float iy = my + 38 + i * 34.f;
                if (i == ui.menuSel) g.frame(mx + 8, iy - 2, mw - 16, 30, 2, kHi);
                tc.draw(g, fUi, menuItems[i].label, mx + 20, iy + 4, kText);
                std::string v;
                if (i == 0) v = "< " + std::to_string(ui.brightness * 100 / 255) + " % >";
                if (i == 1) v = "< " + std::to_string(specRate) + " fps >";
                if (i == 3 && now() - ui.armedPower < 3) v = "PRESS AGAIN";
                if (i == 4 && now() - ui.armedReboot < 3) v = "PRESS AGAIN";
                if (!v.empty()) tc.draw(g, fUi, v, mx + mw - 20, iy + 4, kDim, 2);
            }
        }
        if (t < ui.volUntil && ui.vol >= 0) {
            const float bw = 220, bh = 54, bx = (W - bw) / 2, by = 120;
            g.rect(bx, by, bw, bh, rgb(0x000000, 0.8f));
            g.frame(bx, by, bw, bh, 2, kAccent);
            tc.draw(g, fUi, ui.muted ? "MUTED" : "VOL " + std::to_string(ui.vol), bx + bw / 2, by + 6, kText, 1);
            g.rect(bx + 14, by + 34, (bw - 28) * ui.vol / 100.f, 8, kAccent);
            g.frame(bx + 14, by + 34, bw - 28, 8, 1, kDim);
        }
        if (t < ui.toastUntil) {
            const float bw = (float)tc.width(fUi, ui.toast) + 40, bh = 36, bx = (W - bw) / 2, by = H - hintH - 60;
            g.rect(bx, by, bw, bh, rgb(0x000000, 0.85f));
            g.frame(bx, by, bw, bh, 1, kAccent);
            tc.draw(g, fUi, ui.toast, W / 2.f, by + 8, kText, 1);
        }
        g.flush();

        if (shot && (int)frame == shotFrame) {
            std::vector<uint8_t> px((size_t)W * H * 4);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            if (FILE* f = std::fopen(shot, "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (int y = H - 1; y >= 0; y--) for (int x = 0; x < W; x++) std::fwrite(&px[((size_t)y * W + x) * 4], 1, 3, f);
                std::fclose(f);
            }
            quit = true;
        }
        SDL_GL_SwapWindow(win);
        (void)newRow;
    }
    eng.stop();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
