// battery_screen_test.cpp — the R36S Battery block and the STRESS calibration page drawn offscreen through the
// handheld's own code with the real fonts (2026-11-16: calibration started ON THE CHARGER, "full" from the charger's
// physics — vibe-hwd FullJudge — never the gauge's 100 %):
//   • MENU → R36S HARDWARE in every charge state (Stuart's gauge 100 % with 0.96 A going in, topping up, the hold,
//     full, paused, the input-limit hint) and every calibration phase (waiting on the charger, pulled out before full,
//     full → unplug, counting): every Battery-block line fits the menu's width unwrapped (40 characters is the budget);
//     the keys under CALIBRATE are the right ones (NORMAL + STRESS idle; CANCEL while waiting or armed);
//   • the STRESS page waiting on the charger (its progress lines fit the screen), pulled out early, full → unplug;
//   • BATT_PNG_DIR set: each screen saved as a PNG to look at.
// Silent (no audio). Run in the arm64 test image: handheld-ui/test/run_offscreen_tests.sh battery_screen_test
#include "../src/app.h"

#include <SDL2/SDL_image.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

using namespace hh;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

static App* gA;
static const char* gOut = nullptr;

static void savePng(const std::string& name) {
    if (!gOut) return;
    std::vector<uint8_t> px(640 * 480 * 4);
    glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_ABGR8888);
    for (int y = 0; y < 480; y++) std::memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, px.data() + (size_t)(479 - y) * 640 * 4, 640 * 4);
    IMG_SavePNG(s, (std::string(gOut) + "/" + name + ".png").c_str());
    SDL_FreeSurface(s);
}

static void setState(const BattLive& b, const BattCal& k) {
    HwShared& h = hwShared();
    std::lock_guard<std::mutex> lk(h.m);
    h.bat = b; h.cal = k; h.batAt = hwNow(); h.hwdOk = true; h.calBusy = false;
}

/** MENU → R36S HARDWARE's top, as the menu draws it (the section's inner rect); the frame read back. */
static void drawHardware(const std::string& png) {
    App& a = *gA;
    static RadioState s;
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    a.nav_.begin("menu", {});
    Ctx c{a.g_, a.tc_, a.f_, a.nav_, nowSec()};
    c.nav.layer("menu");
    c.g.rect(0, 0, (float)a.W, (float)a.H, th::sheetBg);
    a.secHardware(c, s, Rect{12, 8, 616, 0}, 8);
    a.nav_.end();
    a.g_.flush();
    savePng(png);
}
static bool hasItem(const std::string& id) {
    // a key exists when the Nav saw it this frame
    return gA->nav_.has(id);
}
static void drawCalPage(const std::string& png) {
    App& a = *gA;
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    a.nav_.begin("cal", {});
    Ctx c{a.g_, a.tc_, a.f_, a.nav_, nowSec()};
    c.nav.layer("cal");
    CHECK(calScreenUp(), "%s: the stress page is up", png.c_str());
    calDrawScreen(a, c);
    a.nav_.end();
    a.g_.flush();
    savePng(png);
}
/** Every Battery-block line fits the menu's width without wrapping. */
static void fitsMenu(const BattLive& b, const char* what) {
    for (auto& l : battLiveLines(b)) {
        CHECK(battChars(l) <= 40, "%s: over 40 characters: [%s]", what, l.c_str());
        CHECK(gA->tc_.width(gA->f_.small, l) <= 616 - 12, "%s: wider than the menu (%d px): [%s]", what, gA->tc_.width(gA->f_.small, l), l.c_str());
    }
}

int main() {
    char tmpl[] = "/tmp/battscr-XXXXXX";
    const std::string home = mkdtemp(tmpl);
    setenv("HOME", home.c_str(), 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    if (chdir(home.c_str()) != 0) { std::fprintf(stderr, "chdir failed\n"); return 1; }
    gOut = std::getenv("BATT_PNG_DIR");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    static App a;
    gA = &a;
    a.win_ = SDL_CreateWindow("battery", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    const std::string R = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    a.f_.tiny.load(B, 13); a.f_.small.load(R, 16); a.f_.body.load(R, 19); a.f_.bodyB.load(B, 19); a.f_.btn.load(B, 21);
    a.f_.title.load(B, 22); a.f_.big.load(B, 30); a.f_.freq.load(B, 34);
    a.eng_ = nullptr;   // ★ no engine: nothing is fetched — the state below is what is drawn
    a.battery_ = 100; a.charging_ = true;

    // ── ★★ Stuart's unit: gauge 100 %, 4.15 V, 0.96 A in, port 4.63 V near the 4.5 V floor; never calibrated ──
    BattLive b; b.known = true; b.online = true; b.status = "Charging"; b.dir = "charge"; b.batV = 4.15; b.batA = 0.96;
    b.batW = 3.98; b.portV = 4.63; b.useW = 4.0; b.useAgeS = 1800; b.gaugePct = 100; b.fullMah = 2886;
    b.charge.state = "cc"; b.charge.cvV = 4.20; b.charge.termA = 0.20; b.charge.vlimV = 4.50; b.charge.nearVlim = true;
    BattCal k;
    setState(b, k); fitsMenu(b, "gauge 100 %, CC"); drawHardware("1-gauge100-cc");
    CHECK(hasItem("hw.calN") && hasItem("hw.calS") && !hasItem("hw.calX"), "idle: NORMAL + STRESS, no CANCEL");
    // waiting on the charger (NORMAL)
    k.phase = "charging"; setState(b, k); drawHardware("2-cal-waiting");
    CHECK(hasItem("hw.calX") && !hasItem("hw.calN"), "★ waiting on the charger: CANCEL, no START keys");
    // topping up, the hold
    b.charge.state = "cv"; b.batV = 4.18; b.batA = 0.35; b.charge.nearVlim = false; b.portV = 4.95; b.useAgeS = 40;
    setState(b, k); fitsMenu(b, "topping up"); drawHardware("3-topping-up");
    b.charge.state = "hold"; b.batV = 4.19; b.batA = 0.15; b.charge.holdLeftS = 170;
    setState(b, k); fitsMenu(b, "hold"); drawHardware("4-hold");
    // full → unplug
    b.charge.state = "full"; b.charge.by = "taper"; b.batA = 0.11; k.phase = "armed";
    setState(b, k); fitsMenu(b, "full"); drawHardware("5-full-unplug");
    CHECK(hasItem("hw.calX"), "armed: CANCEL");
    // pulled out before full
    BattLive off = b; off.online = false; off.status = "Discharging"; off.dir = "discharge"; off.batV = 4.05; off.batA = 0.9; off.batW = 3.6;
    off.charge = BattCharge{};
    k.phase = "charging"; k.plugged = false; a.charging_ = false; a.battery_ = 98;
    setState(off, k); fitsMenu(off, "unplugged early"); drawHardware("6-notfull-unplugged");
    CHECK(hasItem("hw.calX"), "pulled out before full: still CANCEL");
    // paused + the input clamp
    BattLive p = b; p.charge.state = "paused"; p.status = "Not charging"; p.dir = "discharge"; p.batV = 4.05; p.batA = 0.10; p.batW = 0.4;
    p.charge.limited = true; p.charge.nearVlim = false; p.gaugePct = 88;
    fitsMenu(p, "paused");
    // counting
    k.phase = "running"; k.countedMah = 812; k.plugged = true; setState(off, k); drawHardware("7-counting");
    CHECK(!hasItem("hw.calX") && !hasItem("hw.calN"), "running: no key (charging before empty cancels it)");

    // ── the STRESS page ──
    k = BattCal{}; k.mode = "stress"; k.phase = "charging"; a.charging_ = true;
    b.charge.state = "cc"; b.batV = 4.15; b.batA = 0.96; b.charge.nearVlim = true; b.portV = 4.63;
    setState(b, k); drawCalPage("8-stress-waiting");
    for (auto& l : battChargeLines(b)) CHECK(a.tc_.width(a.f_.body, l) <= a.W - 40, "stress page: a progress line too wide: [%s]", l.c_str());
    CHECK(hasItem("cal.cancel"), "stress page waiting: CANCEL");
    k.plugged = false; setState(off, k); drawCalPage("9-stress-notfull");
    k.plugged = true; k.phase = "armed"; b.charge.state = "full"; setState(b, k); drawCalPage("10-stress-unplug");
    CHECK(hasItem("cal.cancel"), "stress page armed: CANCEL");
    // ── h80: the STRESS run's load line (busy loops + why fewer) on the running page ──
    k.phase = "running"; k.countedMah = 300; b.online = false; b.status = "Discharging"; b.dir = "discharge"; b.batA = 1.6; setState(b, k);
    { HwShared& h = hwShared(); std::lock_guard<std::mutex> lk(h.m); h.stressLoops = 2; h.stressOf = 4; h.stressWhy = "76 \u00b0C"; }
    drawCalPage("11-stress-running-hot");
    CHECK(a.tc_.width(a.f_.body, "2 of 4 busy loops \u2014 76 \u00b0C") <= a.W - 40, "the busy-loop line fits");
    CHECK(a.tc_.width(a.f_.body, "SDR 3.2 MS/s (this mode keeps its rate) \u00b7 speaker muted \u00b7 no waterfall") <= a.W - 20, "the SDR line fits");
    CHECK(stressRateText(3200000) == "3.2 MS/s" && stressRateText(2048000) == "2.048 MS/s" && stressRateText(912000) == "912 kS/s", "rate text");
    // ── h80: INPUT FLOOR (TEST) — drawn only when the service reports it ──
    k = BattCal{}; setState(off, k);
    { HwShared& h = hwShared(); std::lock_guard<std::mutex> lk(h.m); h.vlimMv = -1; }
    drawHardware("12-no-vlim");
    CHECK(!hasItem("hw.vlim#0"), "an older service (no vlimMv): no INPUT FLOOR row");
    { HwShared& h = hwShared(); std::lock_guard<std::mutex> lk(h.m); h.vlimMv = 4300; }
    drawHardware("13-vlim-43");
    CHECK(hasItem("hw.vlim#0") && hasItem("hw.vlim#1") && hasItem("hw.vlim#2"), "★ INPUT FLOOR: 4.5 / 4.3 / 4.0 V keys");
    CHECK(a.tc_.width(a.f_.body, "4.5 V (NORMAL)") <= (616 - 12 - 12) / 3 - 10, "the 4.5 V (NORMAL) key's label fits a third");

    std::printf("battery_screen_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
