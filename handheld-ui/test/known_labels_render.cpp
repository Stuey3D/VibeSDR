// known_labels_render.cpp — the SIGNAL LABELS drawn offscreen at the R36S's 640×480, three spectra stacked, from the
// same synthetic schedule and bookmarks as the golden test (known_labels_inputs.json): 40 m's digital dials beside the
// dial, a crowded FM band (26 names → the cap of 8, the rest culled), and 4.6 MHz with the dial on Northwood's WEFAX.
// The spectrum under them is a drawn stand-in (noise and a bump per signal); the labels, the needle, the dB axis and the
// scale are the real code's geometry and fonts. Also times the per-frame part (KnownLabels::draw on a cached layout).
// Usage (arm64 image, SDL offscreen): known_labels_render <inputs.json> <out.png>      Silent: no audio.
#include "../src/json.h"
#include "../src/known_labels.h"

#include <SDL2/SDL_image.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace hh;

static std::string fontPath(const char* name) {
    const char* dir = std::getenv("VIBE_HHUI_FONTS");
    return std::string(dir && *dir ? dir : "/usr/share/vibesdr-r36s/fonts") + "/" + name;
}
static std::string readAll(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

struct Panel { const char* caption; double lo, span, dial; int bwLo, bwHi; };

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: known_labels_render <inputs.json> <out.png>\n"); return 2; }
    const int W = 640, H = 480;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) { std::fprintf(stderr, "init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("labels", 0, 0, W, H, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    Gfx g; g.init(W, H);
    TextCache tc;
    Font tiny, small, bodyB;
    tiny.load(fontPath("AtkinsonHyperlegible-Bold.ttf"), 13);      // Fonts::tiny (app_impl.h)
    small.load(fontPath("AtkinsonHyperlegible-Regular.ttf"), 16);  // Fonts::small
    bodyB.load(fontPath("AtkinsonHyperlegible-Bold.ttf"), 15);

    // the list: the same inputs as the golden test, built as the device builds it (known_labels_app.h)
    const Json in = Json::parse(readAll(argv[1]));
    std::vector<EibiIn> eibi;
    for (auto& r : in["eibi"].a) eibi.push_back({r["name"].str(), r["comment"].str(), r["frequency"].num()});
    std::vector<BookmarkIn> bms;
    for (auto& b : in["bookmarks"].a) {
        BookmarkIn o;
        o.name = b["name"].str(); o.mode = b["mode"].str(); o.frequency = b["frequency"].num();
        o.manual = b["manual"].kind == Json::Bool ? (b["manual"].b ? 1 : 0) : -1;
        o.nameSrc = b["nameSrc"].kind == Json::Num ? (int)b["nameSrc"].n : -1;
        o.sid = b["sid"].kind == Json::Num ? (int)b["sid"].n : -1;
        o.repeater = b["repeater"].boolean();
        bms.push_back(o);
    }
    const std::vector<KnownSig> list = knownSignalList(eibiKnownSignals(eibi, 750), bookmarkSignals(bms));

    const Panel panels[3] = {
        {"40 m, dial on FT8: FT8 · FT4 · JS8 · WSPR + an EiBi CW row", 7.000e6, 100e3, 7.074e6, 200, 2800},
        {"FM, 26 names in view: the cap (8) and collision culling", 87.5e6, 20.5e6, 93.7e6, -100000, 100000},
        {"4.6 MHz, dial on Northwood 4608.1: never over the needle", 4.550e6, 100e3, 4.6081e6, 200, 2800},
    };
    KnownLabels kl[3];
    for (auto& k : kl) k.setList(std::vector<KnownSig>(list));
    const float panelH = 160, capH = 20, scaleH = 22, specH = 112;

    auto drawFrame = [&](bool timeIt, double& drawUs) {
        glViewport(0, 0, W, H);
        glClearColor(th::bg.r, th::bg.g, th::bg.b, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int i = 0; i < 3; i++) {
            const Panel& p = panels[i];
            const float y0 = i * panelH;
            const Rect spec{0, y0 + capH + scaleH, (float)W, specH};
            auto xOf = [&](double hz) { return (float)((hz - p.lo) / p.span) * W; };
            tc.draw(g, small, p.caption, 6, y0 + 1, th::white68);
            // the scale (drawScale's look)
            const Rect sc{0, y0 + capH, (float)W, scaleH};
            g.rect(sc.x, sc.y, sc.w, sc.h, th::bg);
            const double step = p.span >= 10e6 ? 2e6 : 20e3;
            for (double f = std::ceil(p.lo / step) * step; f <= p.lo + p.span; f += step) {
                const float x = xOf(f);
                g.rect(x - 0.5f, sc.b() - 5, 1, 5, th::dim);
                char b[32]; std::snprintf(b, sizeof b, p.span >= 10e6 ? "%.0fM" : "%.2fM", f / 1e6);
                const int tw = tc.width(small, b);
                tc.draw(g, small, b, std::max(2 + tw / 2.f, std::min(W - 2 - tw / 2.f, x)), sc.y + 2, rgb(0xffb43c, 0.9f), 1);
            }
            // a stand-in spectrum: a noise floor and a bump at every signal in view
            g.rect(spec.x, spec.y, spec.w, spec.h, rgb(0x05070c));
            uint32_t seed = 12345 + i;
            for (int x = 0; x < W; x++) {
                seed = seed * 1103515245u + 12345u;
                double lvl = 0.16 + ((seed >> 16) & 255) / 255.0 * 0.07;
                const double hz = p.lo + (x + 0.5) / W * p.span;
                for (const KnownSig& s : list) {
                    const double d = std::fabs(s.hz - hz) / p.span * W;
                    if (d < 6) lvl = std::max(lvl, 0.55 + 0.25 * std::cos(d / 6 * 1.57) - ((s.src == "learnt" ? 0.1 : 0)));
                }
                const float h = (float)(lvl * spec.h);
                g.rect((float)x, spec.b() - h, 1, h, rgb(0x2a6fd6, 0.55f));
                g.rect((float)x, spec.b() - h, 1, 1, rgb(0x9cc8ff, 0.9f));
            }
            // the dB grid (drawOverlays)
            for (int k = 0; k < 5; k++) {
                const float t = k / 4.f, y = spec.y + t * spec.h;
                g.rect(spec.x, y, spec.w, 1, rgb(0xffb43c, 0.10f));
                char b[16]; std::snprintf(b, sizeof b, "%d", -20 - k * 25);
                const float ly = std::max(spec.y + 1.f, std::min(spec.b() - small.lineHeight() - 1.f, y - small.lineHeight() / 2.f));
                tc.draw(g, small, b, spec.x + 5, ly + 1, rgb(0, 0.8f));
                tc.draw(g, small, b, spec.x + 4, ly, rgb(0xffb43c, 0.9f));
            }
            // passband + needle (drawOverlays' colours; the default needle is red)
            const float px0 = xOf(p.dial + p.bwLo), px1 = xOf(p.dial + p.bwHi), vx = xOf(p.dial);
            g.rect(px0, spec.y, std::max(1.f, px1 - px0), spec.h, rgb(0xffb000, 0.13f));
            g.rect(vx - 1, spec.y, 2, spec.h, rgb(0xff2020, 0.9f));
            // ★ the code under test: layout (cached) + draw
            kl[i].layout(tc, tiny, spec, p.lo, p.span, p.dial, vx);
            const auto t0 = std::chrono::steady_clock::now();
            kl[i].draw(g, tc, tiny, spec, p.lo, p.span);
            if (timeIt) drawUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        }
        g.flush();
    };

    double us = 0;
    uint64_t frame = 0;
    tc.tick(++frame);
    drawFrame(false, us);   // the first frame makes the glyphs
    const int made0 = tc.glyphsMade();
    const int frames = 300;
    for (int f = 0; f < frames; f++) { tc.tick(++frame); drawFrame(true, us); }
    size_t placed = 0;
    for (auto& k : kl) placed += k.placed().size();
    std::printf("  render: %zu labels on 3 spectra · draw %.1f us a frame for all three (M-series core; layout cached: %d %d %d layouts in %d frames) · "
                "glyphs made after the first frame: %d · atlas clears: %d\n",
                placed, us / frames, kl[0].layouts(), kl[1].layouts(), kl[2].layouts(), frames + 1, tc.glyphsMade() - made0, tc.atlasClears());
    for (int i = 0; i < 3; i++) {
        std::printf("  panel %d:", i);
        for (auto& p : kl[i].placed()) std::printf(" [r%d %s]", p.row, p.sig->label.c_str());
        std::printf("\n");
    }

    std::vector<uint8_t> px((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ABGR8888);
    for (int y = 0; y < H; y++) std::memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, &px[(size_t)(H - 1 - y) * W * 4], (size_t)W * 4);
    for (int i = 0; i < W * H; i++) ((uint8_t*)s->pixels)[(size_t)(i / W) * s->pitch + (size_t)(i % W) * 4 + 3] = 255;
    const int ok = IMG_SavePNG(s, argv[2]);
    SDL_FreeSurface(s);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    return ok == 0 ? 0 : 1;
}
