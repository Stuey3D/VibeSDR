// map_test.cpp — THE NATIVE MAP, OFFSCREEN: vibe-hhui's own MapStore / MapRender / MapView (map_*.h, unchanged) over
// the real map files (assets/mapdata/v1), served the way the engine serves them, drawn by Mesa llvmpipe through SDL's
// offscreen driver. No engine, no radio, no sound.
//
// Scenarios (argv[1]):
//   zoom   — the zoom sequence Stuart used (z10 down to z2 a step at a time, then back in), and at each zoom the
//            picture is checked for TWO DRAWINGS OF ONE THING: every layer from one tier only, and no land border
//            drawn twice (a country outline along a border AND tier2-borders — two lines a few pixels apart).
//   spots  — the DIGITAL SPOTS map: a spot stream (~1 a second, as FT8 on a busy band) drawn as the sheet draws its
//            markers, with a zoom / pan script; counts the basemap re-renders BY CAUSE, their cost, the frame time.
//            Spots must never re-render the basemap.
//   shot   — one view (lat lon z) once everything has loaded, to a PNG.
// Run: test/run_map_test.sh (Docker linux/arm64, vibe-hh-map:trixie). Exit status 0 = every check passed.
// (app.h's three things map_view.h leans on — the rest of the app is not built here)
#include <chrono>
#include <cstdlib>
#include <string>
namespace hh {
enum Btn { B_ENTER, B_BACK, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_L1, B_R1, B_L2, B_R2, B_SELECT, B_START, B_TOPFACE, B_LEFTFACE, B_NONE };
inline double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline std::string fontPath(const char* name) { const char* d = std::getenv("VIBE_HHUI_FONTS"); return std::string(d ? d : ".") + "/" + name; }
}  // namespace hh
#include "map_view.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace hh;

// ── the engine's /mapdata/v1/, served from a directory (throttled like the R36S's card + A35 parse) ─────────────────
static std::string gDir;
static double gBytesPerSec = 1.5e6;   // ★ the device's pace: 22 files landed in ~25 s of a fresh map (h66 log, 1918–1948 s)
static void serveOne(int c) {
    char buf[2048];
    const ssize_t n = ::recv(c, buf, sizeof buf - 1, 0);
    if (n <= 0) { ::close(c); return; }
    buf[n] = 0;
    std::string req(buf), path;
    const size_t a = req.find(' '), b = req.find(' ', a + 1);
    if (a != std::string::npos && b != std::string::npos) path = req.substr(a + 1, b - a - 1);
    const std::string pre = "/mapdata/v1/";
    std::string body; int status = 404;
    if (path.compare(0, pre.size(), pre) == 0 && path.find("..") == std::string::npos) {
        std::ifstream f(gDir + "/" + path.substr(pre.size()), std::ios::binary);
        if (f) { std::stringstream ss; ss << f.rdbuf(); body = ss.str(); status = 200; }
    }
    if (status == 200 && gBytesPerSec > 0) std::this_thread::sleep_for(std::chrono::milliseconds((int)(20 + body.size() / gBytesPerSec * 1e3)));
    const std::string head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found") + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    ::send(c, head.data(), head.size(), MSG_NOSIGNAL);
    size_t off = 0;
    while (off < body.size()) { const ssize_t w = ::send(c, body.data() + off, body.size() - off, MSG_NOSIGNAL); if (w <= 0) break; off += (size_t)w; }
    ::close(c);
}
static int startServer() {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (::bind(s, (sockaddr*)&a, sizeof a) != 0 || ::listen(s, 16) != 0) { std::perror("bind"); std::exit(2); }
    socklen_t l = sizeof a; getsockname(s, (sockaddr*)&a, &l);
    std::thread([s] { for (;;) { const int c = ::accept(s, nullptr, nullptr); if (c < 0) continue; std::thread(serveOne, c).detach(); } }).detach();
    return ntohs(a.sin_port);
}

// ── the harness ──────────────────────────────────────────────────────────────────────────────────────────────────────
struct H {
    SDL_Window* win = nullptr;
    Gfx g; TextCache tc; Fonts f; Nav nav;
    MapView v;
    uint64_t frame = 0;
    double t0 = 0;
    double frameCpu = 0; long frames = 0;      // ms of the map's own drawing per frame (view.draw + markers + flush + finish)
};

static double wall() { return nowSec(); }

static void savePng(const std::string& path) {
    std::vector<uint8_t> px(640 * 480 * 4);
    glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_ABGR8888);
    for (int y = 0; y < 480; y++) std::memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, px.data() + (size_t)(479 - y) * 640 * 4, 640 * 4);
    IMG_SavePNG(s, path.c_str());
    SDL_FreeSurface(s);
}

/** A spot as the sheet draws it (map_page.h's marker loop, MG_DOT): a dark disc, the mode's colour, its call at z ≥ 5. */
struct Spot { double lat, lon; Rgba col; std::string call; };

/** ★ A WASTED RENDER: one that drew exactly what the render before it drew, for the same view — nothing new was
 *  drawable (a shard of a tier that is not complete yet lands; the old tier is drawn again, alone). */
static std::string gLastSig; static long gWasted = 0, gWastedData = 0;
static void noteRender(H& h) {
    MapRender& R = mapRender();
    std::string sig = std::to_string(R.fz) + "|" + std::to_string(R.fcx) + "|" + std::to_string(R.fcy) + "|" + std::to_string(R.vertsLast) + "|" + std::to_string(R.labelsLast);
    for (auto& d : R.drawn) sig += "|" + *d.file + d.what;
    if (sig == gLastSig) {
        gWasted++; if (R.lastCause == MapRender::RC_DATA) gWastedData++;
        if (std::getenv("MAP_TEST_DEBUG")) std::fprintf(stderr, "wasted render (cause %s) at z%d: %s\n", MapRender::causeName(R.lastCause), R.fz, sig.c_str());
    } else if (std::getenv("MAP_TEST_DEBUG")) std::fprintf(stderr, "render (cause %s) z%d %ld idx\n", MapRender::causeName(R.lastCause), R.fz, R.vertsLast);
    gLastSig = sig;
}
/** One frame: the basemap (MapView::draw — the code under test), then the overlay, as mapDrawSheet does. */
static void frameOnce(H& h, const std::vector<Spot>& spots, bool finish = true) {
    const double now = wall();
    const double c0 = wall();
    h.tc.tick(h.frame);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 640, 480);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    Ctx c{h.g, h.tc, h.f, h.nav, now};
    const long before = mapRender().renders;
    h.v.draw(c, now, h.frame);
    if (mapRender().renders != before) noteRender(h);
    c.g.clip(h.v.area.x, h.v.area.y, h.v.area.w, h.v.area.h);
    const Font& lf = mapFonts().bold13;
    for (auto& s : spots) {
        float x, y;
        if (!h.v.toScreen(s.lat, s.lon, x, y)) continue;
        c.g.circle(x, y, 6, 0, rgb(0, 0.8f)); c.g.circle(x, y, 4.5f, 0, s.col);
        if (h.v.z >= 5) { c.tc.draw(c.g, lf, s.call, x + 12, y - 8, rgb(0, 0.8f)); c.tc.draw(c.g, lf, s.call, x + 11, y - 9, s.col); }
    }
    h.v.drawPointer(c, now);
    c.g.unclip();
    h.v.drawAttribution(c, h.v.area.b());
    c.g.flush();
    if (finish) glFinish();   // ★ llvmpipe renders on flush: the GPU's share counted in the frame
    h.frameCpu += (wall() - c0) * 1e3; h.frames++;
    SDL_GL_SwapWindow(h.win);
    h.frame++;
}
/** Frames at ~30 a second for `sec` seconds of wall time; `each` runs before each frame. */
template <class F> static void run(H& h, double sec, std::vector<Spot>& spots, F each) {
    const double end = wall() + sec;
    while (wall() < end) {
        const double t = wall();
        each(t);
        frameOnce(h, spots);
        const double left = 1.0 / 30 - (wall() - t);
        if (left > 0) std::this_thread::sleep_for(std::chrono::microseconds((int)(left * 1e6)));
    }
}
static void settle(H& h, std::vector<Spot>& spots, double maxSec = 60) {
    const double end = wall() + maxSec;
    double quietSince = wall();
    while (wall() < end) {
        frameOnce(h, spots, false);
        if (mapStore().pending() > 0) quietSince = wall();
        if (wall() - quietSince > 2.2) break;   // the loads done and the repair render (1.5 s after) made
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    frameOnce(h, spots);
}

static int fails = 0;
static void check(bool ok, const std::string& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }

static void perfLine(const char* tag, H& h, double sec) {
    MapRender& R = mapRender();
    std::string causes;
    for (int k = 0; k < MapRender::RC_N; k++) if (R.byCause[k]) causes += std::string(causes.empty() ? "" : " ") + MapRender::causeName(k) + " " + std::to_string(R.byCause[k]);
    std::printf("  %s: %ld re-renders in %.0f s (%.1f a minute) [%s], %ld drew what the one before drew (%ld of them data), %.1f ms each (max %.1f), %ld drew nothing · frame %.2f ms (%ld frames) · %ld uploads · last %ld indices in %ld draws, %ld labels\n",
                tag, R.renders, sec, R.renders * 60.0 / sec, causes.c_str(), gWasted, gWastedData, R.renders ? R.renderMs / R.renders : 0.0, R.maxMs, R.emptyRenders,
                h.frameCpu / std::max(1L, h.frames), h.frames, mapStore().uploads, R.vertsLast, R.drawsLast, R.labelsLast);
}
static void resetPerf(H& h) { gWasted = gWastedData = 0; mapRender().resetPerf(); mapStore().uploads = 0; mapStore().uploadMs = 0; h.frameCpu = 0; h.frames = 0; }

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: map_test <zoom|spots|shot> <mapdataDir> <outDir> [lat lon z]\n"); return 2; }
    const std::string what = argv[1], out = argv[3];
    gDir = argv[2];
    if (const char* bps = std::getenv("MAP_TEST_BPS")) gBytesPerSec = std::atof(bps);
    const int port = startServer();

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
    TTF_Init();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    H h;
    h.win = SDL_CreateWindow("map", 0, 0, 640, 480, SDL_WINDOW_OPENGL);
    if (!h.win || !SDL_GL_CreateContext(h.win)) { std::fprintf(stderr, "GL: %s\n", SDL_GetError()); return 2; }
    std::printf("  GL: %s / %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    h.g.init(640, 480);
    const std::string R = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    h.f.small.load(R, 16); h.f.tiny.load(B, 13);
    mapFonts().load(R, B);
    mapStore().start(port);
    mapRender().init(640, 480, 128);
    h.v.area = {0, 36, 404, 416};   // the sheet with its list open (map_page.h mapl)
    std::vector<Spot> none;

    if (what == "shot") {
        const double lat = argc > 4 ? std::atof(argv[4]) : 52.24, lon = argc > 5 ? std::atof(argv[5]) : -0.9;
        const int z = argc > 6 ? std::atoi(argv[6]) : 4;
        h.v.area = {0, 0, 640, 480};
        h.v.profile = argc > 7 ? argv[7] : "spots";
        h.v.centreOn(lat, lon, z);
        settle(h, none);
        savePng(out);
        std::printf("  shot %s (%.2f, %.2f, z%d): %ld indices in %ld draws, %ld labels\n", out.c_str(), lat, lon, z, mapRender().vertsLast, mapRender().drawsLast, mapRender().labelsLast);
        return 0;
    }

    if (what == "bench") {
        // ★ ONE RE-RENDER'S COST, view by view: settled (everything loaded), then re-rendered 15 times, each finished
        //   (glFinish — llvmpipe draws on the CPU, so this is the whole render, CPU + "GPU").
        h.v.profile = "spots";
        struct V { double lat, lon; int z; } views[] = {{30, 0, 2}, {52.24, -0.9, 3}, {50, 12, 4}, {49.5, 16, 6}, {51.5, -0.5, 8}, {50.85, 4.3, 10}};
        double total = 0; int n = 0;
        for (auto& w : views) {
            h.v.centreOn(w.lat, w.lon, w.z);
            settle(h, none);
            double ms = 0, best = 1e9;
            for (int i = 0; i < 15; i++) {
                glFinish();
                const double a = wall();
                mapRender().render(mapStore(), h.tc, mapFonts(), h.v.cx, h.v.cy, h.v.z, mapProfile(h.v.profile), h.frame++, MapRender::RC_OPEN);
                glFinish();
                const double d = (wall() - a) * 1e3; ms += d; best = std::min(best, d);
            }
            std::printf("  bench z%-2d (%.2f, %.2f): %.1f ms a render (best %.1f) · %ld indices in %ld draws · %ld labels\n", w.z, w.lat, w.lon, ms / 15, best, mapRender().vertsLast, mapRender().drawsLast, mapRender().labelsLast);
            if (std::getenv("MAP_TEST_DEBUG")) { std::map<std::string, long> by; for (auto& d : mapRender().drawn) by[*d.file + " " + d.what] += d.indices; for (auto& b : by) if (b.second > 2000) std::printf("      %7ld %s\n", b.second, b.first.c_str()); }
            total += ms / 15; n++;
        }
        std::printf("  bench mean %.1f ms a render · buffer %.1f MB%s\n", total / n, mapRender().gpuBytes / 1048576.0, mapRender().msaa ? " (MSAA)" : "");
        return 0;
    }

    if (what == "lod") {
        // ★ the world-view fans (z ≤ 4) must look like the full ones: the same view both ways, pixel by pixel
        h.v.area = {0, 0, 640, 480};
        h.v.profile = "spots";
        struct V { double lat, lon; int z; } views[] = {{30, 0, 2}, {52.24, -0.9, 3}, {50, 12, 4}, {-35, 150, 4}, {60, -150, 3}};
        for (auto& w : views) {
            h.v.centreOn(w.lat, w.lon, w.z);
            settle(h, none);
            std::vector<uint8_t> a(640 * 480 * 4), b(640 * 480 * 4);
            long ia, ib;
            mapRender().setLod(true); h.v.dirty = true; frameOnce(h, none); glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, a.data()); ia = mapRender().vertsLast;
            mapRender().setLod(false); h.v.dirty = true; frameOnce(h, none); glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, b.data()); ib = mapRender().vertsLast;
            mapRender().setLod(true);
            long diff = 0, any = 0; int mx = 0;
            for (size_t i = 0; i < a.size(); i += 4) { int m = 0; for (int c = 0; c < 3; c++) m = std::max(m, std::abs((int)a[i + c] - (int)b[i + c])); if (m > 40) diff++; if (m > 0) any++; mx = std::max(mx, m); }
            const double pct = diff * 100.0 / (640 * 480);
            char t[200]; std::snprintf(t, sizeof t, "z%d (%.0f, %.0f): world-view fans %ld indices against %ld — %.3f %% of pixels differ visibly (> 40), %.2f %% at all, most by %d",
                                       w.z, w.lat, w.lon, ia, ib, pct, any * 100.0 / (640 * 480), mx);
            check(pct < 0.3, t);
        }
        return fails ? 1 : 0;
    }

    if (what == "zoom") {
        // ★ Stuart's sequence: in on central Europe, then out a step at a time, then back in — each step held long
        //   enough for some of its files to land (the R36S pace), the way a person zooms while looking.
        h.v.profile = "spots";
        h.v.centreOn(50.5, 10.0, 10);
        h.v.px = h.v.area.cx(); h.v.py = h.v.area.cy();
        settle(h, none);
        for (int z = 9; z >= 2; z--) { h.v.zoomBy(-1); run(h, 0.7, none, [](double) {}); }
        for (int z = 3; z <= 8; z++) {
            h.v.zoomBy(1);
            run(h, 0.7, none, [](double) {});
        }
        // each zoom on its own, settled: the checks
        for (int z : {2, 3, 4, 5, 6, 7, 8, 9, 10}) {
            h.v.centreOn(50.5, 10.0, z);
            settle(h, none);
            const auto& D = mapRender().drawn;
            std::map<std::string, std::set<int>> tiers;
            for (auto& d : D) tiers[d.layer].insert(d.tier);
            std::string multi;
            for (auto& t : tiers) if (t.second.size() > 1) multi += " " + t.first;
            check(multi.empty(), "z" + std::to_string(z) + ": every layer from one tier" + (multi.empty() ? "" : " — MIXED:" + multi));
            // ★ a land border has ONE source: the country polygons' own outline (the country tiers) OR tier2-borders
            //   (the coast tier, z10+) — both together are two lines a few pixels apart (110 m / 50 m against 10 m)
            std::set<int> outlineTiers; bool borders = false;
            for (auto& d : D) { if (std::string(d.what) == "outline" && d.layer == "countries") outlineTiers.insert(d.tier); if (d.layer == "borders") borders = true; }
            check(!(borders && !outlineTiers.empty()), "z" + std::to_string(z) + ": no land border drawn twice" +
                  (borders && !outlineTiers.empty() ? " — tier" + std::to_string(*outlineTiers.begin()) + " country outlines AND tier2-borders" : ""));
            if (z == 4) savePng(out + "/zoom_z4.png");
            if (z == 8) savePng(out + "/zoom_z8.png");
        }
        std::printf("  zoom: %s\n", fails ? "FAILED" : "passed");
        return fails ? 1 : 0;
    }

    if (what == "spots") {
        // ★ the DIGITAL SPOTS map (startZ 3) on the receiver, a spot ~every 1.3 s (h66 log: 46 in the last minute)
        h.v.profile = "spots";
        h.v.centreOn(52.24, -0.9, 3);
        std::vector<Spot> spots;
        uint32_t rnd = 12345;
        auto next = [&] { rnd = rnd * 1103515245u + 12345u; return (rnd >> 8) / 16777216.0; };
        // ★ a box that has been listening a while: 250 spots already held (it keeps 300, one per call · band · mode)
        for (int i = 0; i < 250; i++) spots.push_back({-50 + next() * 115, -170 + next() * 340, rgb(next() < 0.8 ? 0x5be36b : 0x46e0ff), "G" + std::to_string(i) + "ABC"});
        double nextSpot = wall();
        auto feed = [&](double t) {
            if (t < nextSpot) return;
            nextSpot = t + 1.3;
            spots.push_back({-50 + next() * 115, -170 + next() * 340, rgb(next() < 0.8 ? 0x5be36b : 0x46e0ff), "G" + std::to_string((int)(next() * 9)) + "ABC"});
            if (spots.size() > 300) spots.erase(spots.begin());
        };
        // 1) spots alone on a settled map: NO re-render at all
        settle(h, spots);
        resetPerf(h);
        run(h, 20, spots, feed);
        perfLine("spots only, settled map, 20 s", h, 20);
        check(mapRender().renders == 0, "spots never re-render the basemap (" + std::to_string(mapRender().renders) + " renders)");
        // 2) Stuart's session: the map opened fresh (everything to load) at z3, zoom to z4, a pan, a zoom out and in,
        //    spots arriving all the while — 60 s
        mapStore().flush(); mapRender().release(); mapRender().init(640, 480, 128);
        h.v.centreOn(52.24, -0.9, 3); h.v.dirty = true;
        resetPerf(h);
        const double start = wall();
        bool z4 = false, panned = false, out1 = false, in1 = false;
        run(h, 60, spots, [&](double t) {
            feed(t);
            const double e = t - start;
            if (!z4 && e > 3) { h.v.zoomBy(1); z4 = true; }
            if (e > 15 && e < 16.5) { h.v.sticks(0, 0, 0.8, 0, t); panned = true; } else if (panned) h.v.sticks(0, 0, 0, 0, t);
            if (!out1 && e > 30) { h.v.zoomBy(-1); out1 = true; }
            if (!in1 && e > 38) { h.v.zoomBy(1); in1 = true; }
        });
        perfLine("open + zoom/pan with spots, 60 s", h, 60);
        const long renders = mapRender().renders;
        savePng(out + "/spots_z4.png");
        // ★ bounded: an open (everything loads), 3 zooms, 1 pan, their data — not one render per file
        const long cap = std::getenv("MAP_TEST_RENDER_CAP") ? std::atol(std::getenv("MAP_TEST_RENDER_CAP")) : 24;
        check(renders <= cap, "re-renders bounded (" + std::to_string(renders) + " ≤ " + std::to_string(cap) + " in 60 s)");
        check(gWastedData == 0, "no data re-render that draws nothing new (" + std::to_string(gWastedData) + ")");
        std::printf("  spots: %s\n", fails ? "FAILED" : "passed");
        return fails ? 1 : 0;
    }
    return 2;
}
