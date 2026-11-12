// wide_vfo_render_test.cpp — THE DAB VFO ON THE R36S, drawn by the real App::drawOverlays offscreen (Stuart, 2026-11-12: "we
// need an extra wide VFO for things like DAB"). In DAB on 7D (194.064 MHz) at the 640-px spectrum:
//   • a 7 MHz span (the B210 view he saw): the amber passband spans the 1.536 MHz block (~140 px) with its two edges drawn,
//     and nothing either side of it;
//   • an RTL's DAB view (2.048 MHz): the block is ¾ of the view — still filled, edges at 12.5 % / 87.5 %;
//   • a 1.2 MHz view, narrower than the block: NO wash over the whole view — the block's edges are off screen, so only the
//     needle is drawn;
//   • out of DAB: the ordinary WFM passband (±100 kHz) exactly as before.
// Pixels are read back from the frame; PNGs written to $WIDE_VFO_OUT (if set) for a look. Silent: no audio.
// Run in the arm64 test image: handheld-ui/test/run_offscreen_tests.sh wide_vfo_render_test
#include "../src/app.h"

#include <GLES3/gl3.h>
#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace hh;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

static App* gA;
static const Rect kSpec{0, 40, 640, 140}, kWf{0, 180, 640, 200};
static std::vector<uint8_t> px;

static void render(const char* name, double spanHz, bool dab, const char* block = "7D") {
    App& a = *gA;
    RadioState s;
    s.mode = dab ? "wfm" : "wfm";   // the listener's own mode under DAB (the audio filter is ±100 kHz)
    s.bwLow = -100000; s.bwHigh = 100000;
    const int bi = dabBlockIndex(block);
    s.vfoHz = kDabBlocks[bi].hz;
    s.viewCentreHz = s.vfoHz;
    s.spanHz = spanHz;
    dabUi().on = dab;
    dabUi().serverBlock = dab ? bi : -1;
    a.nav_.begin("t", Input{});
    Ctx c{a.g_, a.tc_, a.f_, a.nav_, nowSec()};
    a.g_.rect(0, 0, (float)a.W, (float)a.H, rgb(0x000000));
    a.drawOverlays(c, s, kSpec, kWf);
    a.nav_.end();
    a.g_.flush();
    px.assign((size_t)a.W * a.H * 4, 0);
    glReadPixels(0, 0, a.W, a.H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (const char* out = std::getenv("WIDE_VFO_OUT")) {
        std::vector<uint8_t> flip(px.size());
        for (int y = 0; y < a.H; y++) std::memcpy(&flip[(size_t)y * a.W * 4], &px[(size_t)(a.H - 1 - y) * a.W * 4], (size_t)a.W * 4);
        SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormatFrom(flip.data(), a.W, a.H, 32, a.W * 4, SDL_PIXELFORMAT_ABGR8888);
        IMG_SavePNG(sf, (std::string(out) + "/r36s-dab-vfo-" + name + ".png").c_str());
        SDL_FreeSurface(sf);
    }
}
/** Red channel at (x, y) in screen coordinates (the readback is bottom-up). */
static int R(int x, int y) { return px[((size_t)(gA->H - 1 - y) * gA->W + x) * 4]; }
static int G(int x, int y) { return px[((size_t)(gA->H - 1 - y) * gA->W + x) * 4 + 1]; }
/** Amber tint (the passband's 0xffb000): green well above zero, in the waterfall rows below the dB grid's lines. */
static bool tinted(int x) { return G(x, kWf.y + 100) > 4; }
static int edgeRow() { return (int)kSpec.y + 70; }

int main() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    static App a;
    gA = &a;
    a.win_ = SDL_CreateWindow("widevfo", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    const std::string Rg = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    a.f_.tiny.load(B, 13); a.f_.small.load(Rg, 16); a.f_.body.load(Rg, 19); a.f_.bodyB.load(B, 19);
    const int cx = 320;

    // ── 7 MHz span (the B210): the block is 1.536/7 of 640 = ~140 px, centred ──
    render("7mhz", 7e6, true);
    const int half = (int)(768000.0 / 7e6 * 640);   // ~70 px
    CHECK(tinted(cx - half + 8) && tinted(cx + half - 8), "7 MHz: inside the block is tinted (%d / %d)", G(cx - half + 8, kWf.y + 100), G(cx + half - 8, kWf.y + 100));
    CHECK(tinted(cx - 30) && tinted(cx + 30), "7 MHz: well beyond the old ±100 kHz (±9 px) passband is tinted");
    CHECK(!tinted(cx - half - 8) && !tinted(cx + half + 8), "7 MHz: outside the block is not");
    CHECK(G(cx - half, edgeRow()) > G(cx - half + 4, edgeRow()) || G(cx - half - 1, edgeRow()) > G(cx - half + 4, edgeRow()), "7 MHz: the low edge is drawn");
    CHECK(G(cx + half, edgeRow()) > G(cx + half - 4, edgeRow()) || G(cx + half - 1, edgeRow()) > G(cx + half - 4, edgeRow()), "7 MHz: the high edge is drawn");

    // ── an RTL's DAB view, 2.048 MHz: ¾ of the view, still filled ──
    render("2048k", 2.048e6, true);
    CHECK(tinted(100) && tinted(540), "2.048 MHz: the block (x 80–560) is tinted");
    CHECK(!tinted(40) && !tinted(600), "2.048 MHz: the quarter outside it is not");

    // ── 1.2 MHz, narrower than the block: no wash, only the needle ──
    render("1200k", 1.2e6, true);
    int washed = 0;
    for (int x = 0; x < 640; x += 16) if (std::abs(x - cx) > 4 && tinted(x)) washed++;
    CHECK(washed == 0, "1.2 MHz: no full-view wash (%d sampled columns tinted)", washed);
    CHECK(R(cx, kWf.y + 100) > 40 || R(cx - 1, kWf.y + 100) > 40, "1.2 MHz: the needle is still drawn");

    // ── out of DAB: the WFM passband as ever (±100 kHz at 2.048 MHz = ±31 px) ──
    render("wfm", 2.048e6, false);
    CHECK(tinted(cx + 20) && !tinted(cx + 45) && !tinted(100), "WFM: the ordinary ±100 kHz passband, unchanged");

    std::printf("wide_vfo_render_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
