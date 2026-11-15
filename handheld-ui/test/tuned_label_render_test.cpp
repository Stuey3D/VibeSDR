// tuned_label_render_test.cpp — THE TUNED STATION'S LABEL ON THE R36S, drawn by the real App::drawOverlays offscreen at
// 640×480 (Stuart, 2026-11-15: "When the VTS disappears can we have a label on the VFO with the current station name").
//   • the dial at rest on a saved row: "[User] Flex FM" beside the needle at the waterfall's top, right of it;
//   • at the right edge, an EiBi row on air (no hours = on air): "[EiBi] …" flipped LEFT of the needle, on screen;
//   • the dial just moved: nothing (it waits tl::kSettleMs); a dial on nothing: nothing.
// Pixels are read back from the frame; PNGs written to $TUNED_LABEL_OUT (if set) for a look. Silent: no audio.
// Run in the arm64 test image: handheld-ui/test/run_offscreen_tests.sh tuned_label_render_test
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

static void render(const char* name, double vfoHz, double t) {
    App& a = *gA;
    RadioState s;
    s.mode = "am"; s.bwLow = -4500; s.bwHigh = 4500;
    s.vfoHz = vfoHz; s.viewCentreHz = 9600000; s.spanHz = 96000;
    a.nav_.begin("t", Input{});
    Ctx c{a.g_, a.tc_, a.f_, a.nav_, t};
    a.g_.rect(0, 0, (float)a.W, (float)a.H, rgb(0x000000));
    a.drawOverlays(c, s, kSpec, kWf);
    a.nav_.end();
    a.g_.flush();
    px.assign((size_t)a.W * a.H * 4, 0);
    glReadPixels(0, 0, a.W, a.H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (const char* out = std::getenv("TUNED_LABEL_OUT")) {
        std::vector<uint8_t> flip(px.size());
        for (int y = 0; y < a.H; y++) std::memcpy(&flip[(size_t)y * a.W * 4], &px[(size_t)(a.H - 1 - y) * a.W * 4], (size_t)a.W * 4);
        SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormatFrom(flip.data(), a.W, a.H, 32, a.W * 4, SDL_PIXELFORMAT_ABGR8888);
        IMG_SavePNG(sf, (std::string(out) + "/r36s-tuned-label-" + name + ".png").c_str());
        SDL_FreeSurface(sf);
    }
}
/** Bright text pixels (any channel > 180) in the band of rows the label sits in, between columns x0 and x1. */
static int bright(int x0, int x1) {
    int n = 0;
    for (int y = (int)kWf.y + 3; y < (int)kWf.y + 24; y++)
        for (int x = std::max(0, x0); x < std::min(gA->W, x1); x++) {
            const uint8_t* p = &px[((size_t)(gA->H - 1 - y) * gA->W + x) * 4];
            if (p[0] > 180 || p[1] > 180 || p[2] > 180) n++;
        }
    return n;
}
static int xOf(double hz) { return (int)std::lround((hz - (9600000 - 48000)) / 96000 * 640); }

int main() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init failed\n"); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    static App a;
    gA = &a;
    a.win_ = SDL_CreateWindow("tunedlabel", 0, 0, a.W, a.H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = a.win_ ? SDL_GL_CreateContext(a.win_) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    a.g_.init(a.W, a.H);
    const std::string Rg = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    a.f_.tiny.load(B, 13); a.f_.small.load(Rg, 16); a.f_.body.load(Rg, 19); a.f_.bodyB.load(B, 19);
    {
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        BmRow r; r.name = "Flex FM"; r.frequency = 9580000; r.manual = true; r.mode = "am";
        b.rows.push_back(r);
        BmRow h; h.name = "Radio Caroline"; h.frequency = 9580040; h.manual = false; h.mode = "am";
        b.rows.push_back(h);
        StationRow e; e.name = "China Radio International"; e.frequency = 9640000; e.comment = ""; e.mode = "am";
        b.stations.push_back(e);
        b.seq++; b.stationsSeq++;
    }

    // ── at rest on a saved row: [User], right of the needle ──
    const int nx = xOf(9580000);
    render("moving", 9580000, 100.0);          // the first frame on this dial: not rested yet
    CHECK(bright(nx + 4, nx + 200) == 0, "the dial just arrived: no label yet (%d bright px)", bright(nx + 4, nx + 200));
    render("user", 9580000, 100.7);
    CHECK(bright(nx + 4, nx + 200) > 40, "at rest on a saved row: the label right of the needle (%d bright px)", bright(nx + 4, nx + 200));
    CHECK(bright(nx - 200, nx - 4) == 0, "…and nothing left of it");

    // ── the right edge, EiBi: flipped left, on screen ──
    const int ex = xOf(9640000);
    render("eibi-moving", 9640000, 101.0);
    CHECK(bright(0, 640) == 0, "a new dial: nothing until it rests (%d)", bright(0, 640));
    render("eibi-right-edge", 9640000, 101.7);
    CHECK(bright(ex - 260, ex - 4) > 40, "EiBi at the right edge: flipped left of the needle (%d)", bright(ex - 260, ex - 4));
    CHECK(bright(ex + 4, 640) == 0, "…nothing right of it, off the edge");

    // ── on nothing ──
    render("nothing", 9600000, 102.0);
    render("nothing-rest", 9600000, 102.7);
    CHECK(bright(0, 640) == 0, "a dial on nothing: no label (%d)", bright(0, 640));

    std::printf("tuned_label_render_test: %d / %d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
