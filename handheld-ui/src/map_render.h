// map_render.h — the BASEMAP, drawn with GLES 3 into an off-screen buffer only when the view or the data changes:
// web/mapkit/vibemap.js's layers, palette, label ladder, tier rules and collision placer, ported (the GPU style,
// web/mapkit/vibemap-style.json, carries the same palette — "copied from vibemap.js").
//
// ★★★ DRAWN ON CHANGE, NOT EVERY FRAME (the A35s run the radio too). The basemap goes into a buffer LARGER than the
//     screen (a margin each side); the screen is a copy of part of it, so a pan inside the margin costs one textured
//     quad. Only a zoom, a pan past the margin, newly arrived data (≤ 4 a second) or a profile change re-renders.
// ★★★ POLYGONS WITHOUT TRIANGULATION. A tier2 coast ring is tens of thousands of points; ear-clipping it on an A35 is
//     seconds. Each ring is uploaded ONCE as a triangle fan from its first point and filled by stencil-then-cover
//     (even-odd in the stencil, then one quad in the colour) — the classic vector-fill trick, exact for any ring.
//     One draw a layer, whatever the ring count. The vertex buffers live on the GPU; the CPU keeps only ring boxes.
// ★★ 4× MSAA in the buffer when the driver offers it (Mali resolves in the tile), else none.
// ★★ LABELS ARE PLACED BY COLLISION, IN RANK ORDER (vibemap.js labelPlacer): the capital keeps its space, the village
//    drops. Atkinson Hyperlegible, as everywhere — larger than the web's 9 px (a 3.5" 640×480 panel needs it), so the
//    placer, not a count, decides how many fit.
#pragma once
#include "gfx.h"
#include "map_data.h"
#include "map_geo.h"
#include "text.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace hh {

// ── the palette (vibemap.js — "a set of nudges from MAP_LAND"; ✗ do not brighten to match a printed map) ──────────────
namespace mp {
inline const Rgba sea = rgb(0x123049), land = rgb(0x3c5a3f), coast = rgb(0x6fa37b, 0.8f), border = rgb(0xb5dcc0, 0.92f);
inline const Rgba shelf = rgb(0x194060, 0.58f), lake = rgb(0x1b4a6b), river = rgb(0x3f7ba0, 0.6f), admin = rgb(0x5a7a5f, 0.75f);
inline const Rgba urban = rgb(0x5a5d62, 0.62f), desert = rgb(0x6d6142, 0.85f), tundra = rgb(0x4a5750, 0.85f);
inline const Rgba wetland = rgb(0x33523f, 0.85f), ice = rgb(0x8d9aa2, 0.85f), road = rgb(0x7a7a58, 0.75f), reef = rgb(0x4e8f93, 0.6f);
inline const Rgba playa = rgb(0x8a8470, 0.8f), capital = rgb(0xf0c56a), town = rgb(0x9ec9a8), label = rgb(0xd2ebd7, 0.9f);
inline const Rgba grid = rgb(0xc98f2e), runway = rgb(0xd8d2c4, 0.8f), airport = rgb(0xe8b13a), heli = rgb(0x9fb3c8), port = rgb(0x4db2ff);
inline const Rgba country = rgb(0xbee1c8, 0.72f), region = rgb(0xcbb98f, 0.72f), seaLbl = rgb(0x8fb6cb, 0.62f);
}  // namespace mp

/** What the map is FOR decides what it draws (vibemap.js PROFILES) — each profile only NARROWS the default. */
struct MapProfile { bool airports, runways, ports, rail, grid, roads; int places; /* 0 all · 1 major · 2 countries */ };
inline MapProfile mapProfile(const std::string& name) {
    if (name == "aero") return {true, true, false, false, false, false, 1};
    if (name == "marine") return {false, false, true, false, false, false, 1};
    if (name == "spots") return {false, false, false, false, true, false, 0};
    return {true, true, true, true, true, true, 0};   // directory
}

/** The tier a layer wants at a zoom (vibemap.js TIER_STEP / tierFor) — chosen PER LAYER, not per zoom. */
inline int mapTierFor(const std::string& what, int z) {
    if (z <= 4) return 0;
    static const std::map<std::string, int> step = {{"countries", 10}, {"cover", 8}, {"urban", 8}, {"roads", 8}, {"rivers", 7}, {"lakes", 7},
        {"admin1", 8}, {"shelf", 8}, {"places", 8}, {"airports", 8}, {"islands", 7}, {"reefs", 7}, {"playas", 7}, {"rail", 8}};
    auto it = step.find(what);
    return z >= (it == step.end() ? 8 : it->second) ? 2 : 1;
}

/** The map's own faces: Atkinson Hyperlegible at the label sizes (loaded on first use). */
struct MapFonts {
    Font reg13, bold13, bold15, bold18, reg11, bold22;
    bool loaded = false;
    void load(const std::string& regular, const std::string& bold) {
        if (loaded) return;
        reg13.load(regular, 13); bold13.load(bold, 13); bold15.load(bold, 15); bold18.load(bold, 18); reg11.load(regular, 11); bold22.load(bold, 22);
        loaded = true;
    }
};

class MapRender {
public:
    // the view this buffer was drawn for
    double fcx = -1, fcy = -1; int fz = -1;
    int fboW = 0, fboH = 0, margin = 0;
    GLuint tex = 0;
    // measurements (the perf line)
    long renders = 0; double renderMs = 0, lastMs = 0, maxMs = 0; long vertsLast = 0, labelsLast = 0, drawsLast = 0;
    /* ★★ WHY EACH RE-RENDER HAPPENED (2026-11-12 — handheld66's log said "28 re-renders, 47 ms each" and could not say
     *  what asked for them). MapView::draw names a cause for every render; the perf line counts them by cause, and
     *  counts the renders that drew nothing at all. */
    enum Cause { RC_OPEN, RC_ZOOM, RC_PAN, RC_SETTLE, RC_DATA, RC_REPAIR, RC_N };
    static const char* causeName(int c) { static const char* n[RC_N] = {"open", "zoom", "pan", "settle", "data", "repair"}; return n[c]; }
    long byCause[RC_N] = {}; long emptyRenders = 0; int lastCause = RC_OPEN;
    void resetPerf() { renders = 0; renderMs = 0; maxMs = 0; emptyRenders = 0; for (auto& c : byCause) c = 0; }
    bool msaa = false;

    bool init(int screenW, int screenH, int marginPx) {
        if (prog_) return true;
        margin = marginPx; fboW = screenW + 2 * marginPx; fboH = screenH + 2 * marginPx;
        prog_ = linkProgram(
            "#version 300 es\nlayout(location=0) in vec3 a; uniform vec2 uOrigin; uniform float uWorld; uniform float uWrap; uniform vec2 uSize;\n"
            "out float vD;\n"
            "void main(){ vec2 p = (a.xy + vec2(uWrap, 0.0) - uOrigin) * uWorld; vD = a.z * uWorld;\n"
            "  gl_Position = vec4(p.x / uSize.x * 2.0 - 1.0, 1.0 - p.y / uSize.y * 2.0, 0.0, 1.0); }\n",
            "#version 300 es\nprecision highp float; in float vD; uniform vec4 uColor; uniform float uDash; out vec4 o;\n"
            "void main(){ if (uDash > 0.0 && fract(vD / uDash) > 0.55) discard; o = uColor; }\n");
        uOrigin_ = glGetUniformLocation(prog_, "uOrigin"); uWorld_ = glGetUniformLocation(prog_, "uWorld");
        uWrap_ = glGetUniformLocation(prog_, "uWrap"); uSize_ = glGetUniformLocation(prog_, "uSize");
        uColor_ = glGetUniformLocation(prog_, "uColor"); uDash_ = glGetUniformLocation(prog_, "uDash");
        glGenVertexArrays(1, &vao_);
        // the resolved texture (what the screen copies from)
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fboW, fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &resolveFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        // the drawing target: multisampled colour + depth-stencil if the driver allows, else the texture + a stencil
        GLint maxS = 0; glGetIntegerv(GL_MAX_SAMPLES, &maxS);
        const int samples = std::min(4, (int)maxS);
        bool ok = false;
        if (samples >= 2 && !std::getenv("VIBE_HHUI_MAP_NOMSAA")) {
            glGenFramebuffers(1, &drawFbo_);
            glBindFramebuffer(GL_FRAMEBUFFER, drawFbo_);
            glGenRenderbuffers(1, &colorRb_); glBindRenderbuffer(GL_RENDERBUFFER, colorRb_);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, fboW, fboH);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRb_);
            glGenRenderbuffers(1, &dsRb_); glBindRenderbuffer(GL_RENDERBUFFER, dsRb_);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, fboW, fboH);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, dsRb_);
            ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            if (ok) { msaa = true; gpuBytes = (size_t)fboW * fboH * 4 * (2 * samples + 1); }
            else { glDeleteFramebuffers(1, &drawFbo_); glDeleteRenderbuffers(1, &colorRb_); glDeleteRenderbuffers(1, &dsRb_); drawFbo_ = colorRb_ = dsRb_ = 0; }
        }
        if (!ok) {
            glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo_);
            glGenRenderbuffers(1, &dsRb_); glBindRenderbuffer(GL_RENDERBUFFER, dsRb_);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fboW, fboH);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, dsRb_);
            ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            gpuBytes = (size_t)fboW * fboH * 8;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        GLfloat lw[2] = {1, 1}; glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lw); maxLine_ = lw[1];
        if (!gInit_) { g_.init(fboW, fboH); gInit_ = true; }   // (Gfx has no teardown: made once)
        std::fprintf(stderr, "vibe-hhui: map buffer %dx%d%s (%.1f MB), line widths ≤ %.1f, %s\n", fboW, fboH, msaa ? " 4×MSAA" : "",
                     gpuBytes / 1048576.0, maxLine_, ok ? "ready" : "INCOMPLETE");
        ok_ = ok;
        return ok;
    }
    bool ready() const { return ok_; }
    /** Every GL object gone (the map shut for a while); init() makes them again. */
    void release() {
        if (!prog_) return;
        glDeleteProgram(prog_); glDeleteVertexArrays(1, &vao_); glDeleteTextures(1, &tex);
        if (drawFbo_) glDeleteFramebuffers(1, &drawFbo_);
        if (resolveFbo_) glDeleteFramebuffers(1, &resolveFbo_);
        if (colorRb_) glDeleteRenderbuffers(1, &colorRb_);
        if (dsRb_) glDeleteRenderbuffers(1, &dsRb_);
        prog_ = vao_ = tex = drawFbo_ = resolveFbo_ = colorRb_ = dsRb_ = 0;
        ok_ = false; msaa = false; fz = -1; gpuBytes = 0;
    }
    size_t gpuBytes = 0;

    /** Draw the basemap for centre (cx, cy) in world units at zoom z. `tc` / `mf` for the labels. */
    void render(MapStore& st, TextCache& tc, MapFonts& mf, double cx, double cy, int z, const MapProfile& P, uint64_t frame, int cause = RC_OPEN) {
        if (!ok_) return;
        if (cause >= 0 && cause < RC_N) byCause[cause]++;
        lastCause = cause;
        const double t0 = clk();
        fcx = cx; fcy = cy; fz = z;
        wpx_ = worldPx(z);
        ox_ = cx - fboW / 2.0 / wpx_; oy_ = cy - fboH / 2.0 / wpx_;
        box_[0] = (float)ox_; box_[1] = (float)oy_; box_[2] = (float)(ox_ + fboW / wpx_); box_[3] = (float)(oy_ + fboH / wpx_);
        st_ = &st; frame_ = frame; P_ = P; z_ = z; verts_ = 0; draws_ = 0; drawn.clear();
        st.noteRender(frame);   // ★ the files this render touches are the picture — evict() keeps them (map_data.h)
        glBindFramebuffer(GL_FRAMEBUFFER, drawFbo_ ? drawFbo_ : resolveFbo_);
        glViewport(0, 0, fboW, fboH);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(mp::sea.r, mp::sea.g, mp::sea.b, 1);
        glClearStencil(0);
        glStencilMask(0xff);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        layers();
        labels(tc, mf);
        g_.flush();
        st.endRender(frame);   // ★ purged on zoom: what this view did not ask for leaves the load queue (map_data.h)
        glDisable(GL_STENCIL_TEST);
        if (drawFbo_) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFbo_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo_);
            glBlitFramebuffer(0, 0, fboW, fboH, 0, 0, fboW, fboH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        renders++;
        lastMs = (clk() - t0) * 1e3; renderMs += lastMs; maxMs = std::max(maxMs, lastMs);
        vertsLast = verts_; drawsLast = draws_;
        if (!draws_) emptyRenders++;
    }

    /** What the last render drew, a row per file and use (the tests' check that no layer is drawn from two tiers). */
    struct Drawn { std::string layer; int tier; const char* what; const std::string* file; };
    std::vector<Drawn> drawn;

    /** The place labels / POIs that were drawn, for the pointer's hit test (FBO pixel boxes). */
    struct Placed { float x, y, w, h; std::string text, kind; double lat, lon; };
    std::vector<Placed> poi;

private:
    GLuint prog_ = 0, vao_ = 0, drawFbo_ = 0, resolveFbo_ = 0, colorRb_ = 0, dsRb_ = 0;
    GLint uOrigin_ = -1, uWorld_ = -1, uWrap_ = -1, uSize_ = -1, uColor_ = -1, uDash_ = -1;
    float maxLine_ = 1;
    bool ok_ = false, gInit_ = false;
    Gfx g_;                     // the buffer's own 2D batcher (text, dots, the cover quads)
    MapStore* st_ = nullptr;
    uint64_t frame_ = 0;
    MapProfile P_{};
    int z_ = 0;
    double wpx_ = 256, ox_ = 0, oy_ = 0;
    float box_[4] = {0, 0, 1, 1};
    long verts_ = 0, draws_ = 0;
    bool dbg_ = std::getenv("VIBE_HHUI_MAP_LOG") != nullptr;
    struct Box { float x, y, w, h; };
    std::vector<Box> placed_;

    static double clk() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    // ── geometry ──
    bool hits(const float* b) const {
        for (int k = -1; k <= 1; k++) if (!(b[2] + k < box_[0] || b[0] + k > box_[2] || b[3] < box_[1] || b[1] > box_[3])) return true;
        return false;
    }
    /** The world copies this view touches (vibemap.js worldOffsets). */
    std::vector<int> wraps(const float* b) const {
        std::vector<int> w;
        for (int k = (int)std::floor(box_[0]) - 1; k <= (int)std::floor(box_[2]) + 1 && w.size() < 4; k++)
            if (!(b[2] + k < box_[0] || b[0] + k > box_[2])) w.push_back(k);
        return w;
    }
    /** The files a layer needs at a tier, in this view: its shards, or its one file. Absent = empty. */
    std::vector<std::string> filesFor(const std::string& layer, int tier) const {
        const MapIndex& ix = st_->index();
        const std::string base = "tier" + std::to_string(tier) + "-" + layer;
        std::vector<std::string> out;
        auto sh = ix.sharded.find(base);
        if (sh != ix.sharded.end()) { for (auto& s : sh->second) if (ix.has(s.file) && hits(s.box)) out.push_back(s.file); return out; }
        if (ix.has(base + ".json")) out.push_back(base + ".json");
        return out;
    }
    /** ★★★ THE OLD LAND STAYS UP UNTIL THE NEW LAND HAS ARRIVED (vibemap.js drawBase): the wanted tier's files are
     *  asked for; until they are all here, the best tier already resident is drawn instead. */
    std::vector<MapFile*> resolve(const std::string& layer, int want, int prio = 1) {
        if (want == 2 && !st_->index().detail()) want = 1;
        // ★ a tier without this layer at all (tier0 has no roads): the next one down is the wanted one
        auto exists = [&](int t) { const std::string b = "tier" + std::to_string(t) + "-" + layer; return st_->index().sharded.count(b) || st_->index().has(b + ".json"); };
        while (want > 0 && !exists(want)) want--;
        if (!exists(want)) { int t = want + 1; while (t <= 2 && !exists(t)) t++; if (t > 2 || (t == 2 && !st_->index().detail())) return {}; want = t; }
        std::vector<MapFile*> got;
        const auto names = filesFor(layer, want);
        if (names.empty()) return {};                    // the layer exists, nothing of it in this view
        bool all = true;
        for (auto& n : names) { MapFile* f = st_->want(n, frame_, prio); if (f) got.push_back(f); else all = false; }
        if (all) return got;
        // ★★★ THE OLD LAND STAYS UP UNTIL THE NEW LAND HAS ARRIVED: the best lower tier already resident, ON ITS OWN.
        // ★★ Not with what has arrived of the wanted one on top (Stuart, 2026-11-11: "the tiles all seem to overlap
        //    different zoom levels"): two tiers of one layer drawn together is two coastlines and two sets of borders a
        //    few pixels apart. One complete tier at a time; the new one replaces it whole once all of it is here.
        for (int u = want - 1; u >= 0; u--) {
            if (!exists(u)) continue;
            bool ok = true; std::vector<MapFile*> lower;
            for (auto& n : filesFor(layer, u)) { MapFile* f = st_->peekFile(n); if (f) { f->used = frame_; lower.push_back(f); } else ok = false; }
            if (ok) return lower;
        }
        return got;
    }
    /** Draw the rings of `g` that meet the view, as few runs of its index buffer as the space order allows. `fan` picks
     *  the fan indices (fills) or the edges (lines); `first` is where that part starts in the ibo. */
    void runs(const MapGroup& g, bool fan, GLenum prim) {
        const std::vector<uint32_t>& at = fan ? g.fanAt : g.edgeAt;
        const std::vector<float>& boxes = fan ? g.fanBoxes : g.boxes;   // ★ a fill's pieces have their own boxes (mapSplitFill)
        const uint32_t first = fan ? 0 : (uint32_t)g.fanN;
        const size_t nr = boxes.size() / 4;
        if (at.size() != nr + 1) return;
        for (int k : wraps(g.box)) {
            glUniform1f(uWrap_, (float)k);
            uint32_t r0 = 0, r1 = 0; bool open = false;
            auto flushRun = [&] { if (open && r1 > r0) { glDrawElements(prim, (GLsizei)(r1 - r0), GL_UNSIGNED_INT, (void*)(sizeof(uint32_t) * (size_t)(first + r0))); verts_ += r1 - r0; draws_++; } open = false; };
            for (size_t i = 0; i < nr; i++) {
                const float* b = &boxes[i * 4];
                if (b[2] + k < box_[0] || b[0] + k > box_[2] || b[3] < box_[1] || b[1] > box_[3]) continue;
                const uint32_t s0 = at[i], s1 = at[i + 1];
                if (s1 == s0) continue;
                if (open && s0 - r1 < 4096) r1 = s1;   // ★ a short gap is cheaper drawn than another call
                else { flushRun(); r0 = s0; r1 = s1; open = true; }
            }
            flushRun();
        }
    }
    void note(const MapFile* f, const char* what) {
        const int tier = f->name.size() > 5 && f->name.compare(0, 4, "tier") == 0 ? f->name[4] - '0' : -1;
        drawn.push_back({mapLayerOf(f->name), tier, what, &f->name});
    }
    void use() {
        g_.flush();
        glUseProgram(prog_);
        glBindVertexArray(vao_);
        glUniform2f(uOrigin_, (float)ox_, (float)oy_);
        glUniform1f(uWorld_, (float)wpx_);
        glUniform2f(uSize_, (float)fboW, (float)fboH);
        glUniform1f(uDash_, 0);
    }
    void bindGroup(const MapGroup& g, bool line) {
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, line ? 3 : 2, GL_FLOAT, GL_FALSE, (line ? 3 : 2) * sizeof(float), (void*)0);
    }
    /** Stencil-then-cover: every ring's fan inverts the stencil (even-odd), then one quad paints where it is odd. */
    void fill(const std::vector<MapFile*>& fs, Rgba col, const std::string& key = "") {
        bool any = false;
        use();
        glEnable(GL_STENCIL_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilMask(0x01);
        glStencilFunc(GL_ALWAYS, 0, 0x01);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
        for (MapFile* f : fs) for (auto& g : f->groups) {
            if (!key.empty() && g.key != key) continue;
            if (!g.vbo || !g.fanN || !hits(g.box)) continue;
            bindGroup(g, f->kind == MF_LINE);
            runs(g, true, GL_TRIANGLES);
            any = true;
            note(f, "fill");
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (any) {
            glStencilFunc(GL_EQUAL, 0x01, 0x01);
            glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);   // ★ the cover clears as it paints: the next layer starts from zero
            g_.rect(0, 0, (float)fboW, (float)fboH, col);
            g_.flush();
        }
        glDisable(GL_STENCIL_TEST);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    /** Lines (or a fill's outline): GL_LINES from the edge indices; `dash` in screen pixels (0 = solid). */
    void lines(const std::vector<MapFile*>& fs, Rgba col, float width, float dash = 0, const std::string& skipKey = "") {
        use();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform4f(uColor_, col.r, col.g, col.b, col.a);
        glUniform1f(uDash_, dash);
        glLineWidth(std::max(1.f, std::min(maxLine_, width)));
        for (MapFile* f : fs) for (auto& g : f->groups) {
            if (!skipKey.empty() && g.key.find(skipKey) != std::string::npos) continue;
            if (!g.vbo || !g.edgeN || !hits(g.box)) continue;
            bindGroup(g, f->kind == MF_LINE);
            glUniform1f(uDash_, f->kind == MF_LINE ? dash : 0);
            runs(g, false, GL_LINES);
            note(f, f->kind == MF_LINE ? "line" : "outline");
        }
        glLineWidth(1);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    /** One geoline group, dashed or solid by its name (the tropics dashed, the circles solid; the Date Line not at all). */
    void geolines(MapFile* f) {
        if (!f) return;
        for (auto& g : f->groups) {
            if (g.key.find("Date Line") != std::string::npos) continue;
            const bool tropic = g.key.find("Tropic") != std::string::npos;
            use();
            glEnable(GL_BLEND);
            glUniform4f(uColor_, mp::grid.r, mp::grid.g, mp::grid.b, tropic ? 0.38f : 0.5f);
            glUniform1f(uDash_, tropic ? 10.f : 0.f);
            if (!g.vbo || !g.edgeN || !hits(g.box)) continue;
            bindGroup(g, true);
            runs(g, false, GL_LINES);
        }
    }
    /** A raster in world coordinates (the relief), drawn through the buffer's 2D batcher. */
    void image(GLuint tex, const float* b, float alpha) {
        for (int k : wraps(b)) {
            const float x0 = (float)((b[0] + k - ox_) * wpx_), y0 = (float)((b[1] - oy_) * wpx_);
            const float x1 = (float)((b[2] + k - ox_) * wpx_), y1 = (float)((b[3] - oy_) * wpx_);
            g_.imageUV(tex, x0, y0, x1 - x0, y1 - y0, 0, 0, 1, 1, rgb(0xffffff, alpha));
        }
        g_.flush();
    }

    void layers() {
        const int z = z_;
        const MapIndex& ix = st_->index();
        // shelf — UNDER the land (the 200 m contour: drawn above, it would flood every coast)
        fill(resolve("shelf", mapTierFor("shelf", z)), mp::shelf);
        // ★★★ the land: a coastline dataset from z10 (OSM, the runway platforms on land), country polygons below that
        bool coastDrawn = false;
        if (z >= 10 && ix.detail()) {
            const auto coast = resolve("coast", 2, 3);
            if (!coast.empty()) {
                /* ★ not every shard here yet: the country fill underneath, so a half-arrived view is never sea —
                 *  ★★ but ONLY over the shards still missing (a scissor a shard box). Under the whole view it was the
                 *  coarse land poking out past the fine coast wherever the coast HAD arrived: low detail under fine
                 *  detail, for as long as one shard of the view was still loading (or never came). */
                std::vector<const float*> missing;
                auto sh = ix.sharded.find("tier2-coast");
                if (sh != ix.sharded.end()) for (auto& s : sh->second) if (ix.has(s.file) && hits(s.box) && !st_->peekFile(s.file)) missing.push_back(s.box);
                if (!missing.empty()) {
                    const auto under = resolve("countries", 2, 2);
                    for (const float* b : missing) {
                        for (int k : wraps(b)) {
                            const float x0 = std::max(0.f, sx(b[0] + k)), x1 = std::min((float)fboW, sx(b[2] + k));
                            const float y0 = std::max(0.f, sy(b[1])), y1 = std::min((float)fboH, sy(b[3]));
                            if (x1 <= x0 || y1 <= y0) continue;
                            glEnable(GL_SCISSOR_TEST);
                            glScissor((GLint)std::floor(x0), (GLint)std::floor(fboH - y1), (GLsizei)std::ceil(x1 - x0) + 1, (GLsizei)std::ceil(y1 - y0) + 1);
                            fill(under, mp::land);
                            glDisable(GL_SCISSOR_TEST);
                        }
                    }
                }
                fill(coast, mp::land);
                lines(coast, mp::coast, 1);
                coastDrawn = true;
            }
        }
        std::vector<MapFile*> countries;
        if (!coastDrawn) {
            countries = resolve("countries", mapTierFor("countries", z), 3);
            fill(countries, mp::land);
        }
        const auto islands = resolve("islands", mapTierFor("islands", z));
        fill(islands, mp::land);
        lines(islands, mp::coast, 1);
        // relief: one global image far out, the 8192-wide tiles from z6 (detail pack)
        {
            bool tiles = false;
            if (z >= 6 && !ix.reliefTiles.empty()) {
                std::vector<std::pair<MapFile*, const float*>> got; bool all = true;
                for (auto& t : ix.reliefTiles) if (hits(t.box)) { MapFile* f = st_->want(t.file, frame_, 0); if (f) got.push_back({f, t.box}); else all = false; }
                if (!all) { static const float wb[4] = {0, 0, 1, 1}; if (MapFile* g = st_->peekFile(ix.reliefBasic)) image(g->tex, wb, 0.92f); }
                for (auto& p : got) image(p.first->tex, p.second, 0.92f);
                tiles = true;
            }
            if (!tiles) { static const float wb[4] = {0, 0, 1, 1}; if (MapFile* g = st_->want(ix.reliefBasic, frame_, 0)) image(g->tex, wb, 0.92f); }
        }
        // ground cover, then what was built on it, then what joins it
        {
            const auto cover = resolve("cover", mapTierFor("cover", z));
            fill(cover, mp::desert, "desert"); fill(cover, mp::tundra, "tundra"); fill(cover, mp::wetland, "wetland"); fill(cover, mp::ice, "ice");
        }
        if (z >= 4) fill(resolve("playas", std::max(1, mapTierFor("playas", z))), mp::playa);
        fill(resolve("urban", mapTierFor("urban", z)), mp::urban);
        fill(resolve("lakes", mapTierFor("lakes", z)), mp::lake);
        lines(resolve("rivers", mapTierFor("rivers", z)), mp::river, z >= 8 ? 1.4f : 1);
        if (z >= 4) lines(resolve("reefs", std::max(1, mapTierFor("reefs", z))), mp::reef, 1);
        // ★ admin-1 DASHED and dim: a solid line reads as a border between countries — a factual error, not a style one
        if (z >= 5) lines(resolve("admin1", mapTierFor("admin1", z)), mp::admin, 1, 7);
        // ★★★ THREE LINE KINDS, THREE LOOKS: coast · border (brighter, solid) · admin (dashed).
        // ★★★ A LAND BORDER HAS ONE SOURCE (Stuart, 2026-11-12, handheld66: "still overlapping … the maps draw different
        //     zoom levels or different countries merged into one"). At the country tiers (z ≤ 9) the outline of the
        //     country polygons IS the border — Natural Earth's countries share their border vertices exactly, so it
        //     sits where the two fills meet. tier2-borders on top of it (vibemap.js drawBorders does both) was the
        //     same border a second time from 10 m data over 110 m / 50 m: two lines a few pixels apart, the coarse
        //     one under the fine one, all across every land border (test/map_test.cpp zoom). tier2-borders is drawn
        //     at the COAST tier (z10+) only, where the fill is not political and the borders must come on their own.
        //     (The web's reason for both — a neighbour's fill painting over the stroke — cannot happen here: every
        //     fill is drawn before any line.) It also leaves 150 k border indices out of every world-view render.
        if (!countries.empty()) lines(countries, mp::border, 1);
        else if (coastDrawn) lines(resolve("borders", 2, 2), mp::border, z >= 6 ? 1.6f : 1.3f);
        if (z >= 2 && z <= 8) geolines(st_->want("tier0-geolines.json", frame_, 1));
        grid();
        if (z >= 5 && P_.roads) lines(resolve("roads", mapTierFor("roads", z)), mp::road, z >= 9 ? 1.4f : 1);
        if (z >= 7 && P_.rail) lines(resolve("rail", mapTierFor("rail", z)), mp::road, 1, 6);
        if (z >= 9 && P_.runways) runways();
        g_.flush();
    }

    // ── the Maidenhead grid: computed, not downloaded (vibemap.js drawGrid) ──
    void grid() {
        if (z_ < 3 || !P_.grid) return;
        const int level = z_ >= 12 ? 2 : z_ >= 6 ? 1 : 0;
        const double dLon = level == 0 ? 20 : level == 1 ? 2 : 2.0 / 24, dLat = level == 0 ? 10 : level == 1 ? 1 : 1.0 / 24;
        const double w = std::max(-180.0, std::floor(mercLon(box_[0]) / dLon) * dLon), e = std::min(180.0, std::ceil(mercLon(box_[2]) / dLon) * dLon);
        const double n = std::min(85.0, std::ceil(mercLat(box_[1]) / dLat) * dLat), s = std::max(-85.0, std::floor(mercLat(box_[3]) / dLat) * dLat);
        const double cols = (e - w) / dLon, rows = (n - s) / dLat;
        if (cols * rows > 900 || cols < 0 || rows < 0) return;
        const Rgba col = rgb(0xc98f2e, level == 2 ? 0.2f : level ? 0.26f : 0.36f);
        auto X = [&](double lon) { return (float)((mercX(lon) - ox_) * wpx_); };
        auto Y = [&](double lat) { return (float)((mercY(lat) - oy_) * wpx_); };
        for (double lon = w; lon <= e + 1e-9; lon += dLon) g_.rect(X(lon), Y(n), 1, Y(s) - Y(n), col);
        for (double lat = s; lat <= n + 1e-9; lat += dLat) g_.rect(X(w), Y(lat), X(e) - X(w), 1, col);
    }

    void runways() {
        const auto rw = resolve("runways", 2, 1);
        const float minFt = z_ >= 12 ? 2000 : z_ >= 11 ? 4000 : z_ >= 10 ? 7000 : 9000;
        const float w = z_ >= 14 ? 3.5f : z_ >= 12 ? 2.4f : 1.6f;
        for (MapFile* f : rw) for (auto& r : f->rows) {
            if (!(r.n[4] >= minFt)) continue;
            const float bx = (float)mercX(r.n[2]), by = (float)mercY(r.n[3]);
            const float b[4] = {std::min(r.x, bx), std::min(r.y, by), std::max(r.x, bx), std::max(r.y, by)};
            if (!hits(b)) continue;
            g_.line((float)((r.x - ox_) * wpx_), (float)((r.y - oy_) * wpx_), (float)((bx - ox_) * wpx_), (float)((by - oy_) * wpx_), w, mp::runway);
        }
    }

    // ── labels ──
    bool place(float x, float y, float w, float h) {
        if (x < 0 || y < 0 || x + w > fboW || y + h > fboH) return false;
        for (auto& b : placed_) if (x < b.x + b.w && x + w > b.x && y < b.y + b.h && y + h > b.y) return false;
        placed_.push_back({x, y, w, h});
        return true;
    }
    /** Text with a dark halo (the web's text-shadow) so it reads across terrain. */
    void halo(TextCache& tc, const Font& f, const std::string& s, float x, float y, Rgba col, int align = 0) {
        const Rgba sh = rgb(0x000000, 0.75f * col.a);
        tc.draw(g_, f, s, x + 1, y + 1, sh, align); tc.draw(g_, f, s, x - 1, y, sh, align);
        tc.draw(g_, f, s, x + 1, y, sh, align); tc.draw(g_, f, s, x, y - 1, sh, align);
        tc.draw(g_, f, s, x, y, col, align);
    }
    float sx(double wx) const { return (float)((wx - ox_) * wpx_); }
    float sy(double wy) const { return (float)((wy - oy_) * wpx_); }
    /** The screen x of a point on the world copy nearest the view centre. */
    float sxNear(double wx) const { const double c = ox_ + fboW / 2.0 / wpx_; double d = wx - c; d -= std::round(d); return (float)((c + d - ox_) * wpx_); }
    static bool latinOnly(const char* s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; p++) if (*p >= 0xC9) return false;   // ★ Atkinson covers Latin; others would be boxes
        return true;
    }
    static std::string up(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return s; }

    void labels(TextCache& tc, MapFonts& mf) {
        placed_.clear(); poi.clear();
        const int z = z_;
        const float lh = (float)mf.reg13.lineHeight();
        long n = 0;
        // ★ the ladder (vibemap.js LADDER): z0-3 continents · z3-6 regions, z2-6 seas · z4-7 countries · z5+ cities
        if (z <= 3) {
            static const struct { const char* n; double lon, lat; } C[] = {{"NORTH AMERICA", -100, 46}, {"SOUTH AMERICA", -60, -15},
                {"EUROPE", 18, 53}, {"AFRICA", 20, 3}, {"ASIA", 90, 45}, {"OCEANIA", 140, -25}, {"ANTARCTICA", 0, -78}};
            for (auto& c : C) {
                const float x = sxNear(mercX(c.lon)), y = sy(mercY(c.lat));
                const int w = tc.width(mf.bold15, c.n);
                if (place(x - w / 2.f - 4, y - 10, w + 8.f, 20)) { halo(tc, mf.bold15, c.n, x, y - 9, rgb(0xe6f0e1, 0.62f), 1); n++; }
            }
        }
        // physical regions and seas — stacked when long (a paper atlas's NORTH / ATLANTIC / OCEAN)
        if (z >= 2 && z <= 6) {
            if (MapFile* f = st_->want("tier0-regions.json", frame_, 1)) {
                const int cut = z <= 3 ? 2 : z <= 4 ? 3 : z <= 5 ? 4 : 6;
                std::vector<const MapRow*> c;
                for (auto& r : f->rows) {
                    const bool sea = std::string(f->str(r, 1)) == "sea";
                    if (r.n[2] > cut) continue;
                    if (sea ? z > 6 : (z < 3 || z > 6)) continue;
                    c.push_back(&r);
                }
                std::sort(c.begin(), c.end(), [](const MapRow* a, const MapRow* b) { return a->n[2] < b->n[2]; });
                for (const MapRow* r : c) {
                    const bool sea = std::string(f->str(*r, 1)) == "sea";
                    const std::string name = up(f->str(*r, 0));
                    std::vector<std::string> ls;
                    if (name.size() > 11 && name.find(' ') != std::string::npos) { std::string w; for (char ch : name + " ") { if (ch == ' ') { if (!w.empty()) ls.push_back(w); w.clear(); } else w += ch; } }
                    else ls.push_back(name);
                    const Font& font = sea ? mf.reg11 : mf.reg13;
                    int ww = 0; for (auto& l : ls) ww = std::max(ww, tc.width(font, l));
                    const float lineH = font.lineHeight() * 0.95f, h = lineH * ls.size();
                    const float x = sxNear(r->x), y = sy(r->y);
                    if (!place(x - ww / 2.f - 6, y - h / 2 - 4, ww + 12.f, h + 8)) continue;
                    for (size_t i = 0; i < ls.size(); i++) halo(tc, font, ls[i], x, y - h / 2 + i * lineH, sea ? mp::seaLbl : mp::region, 1);
                    n++;
                }
            }
        }
        // countries — Natural Earth's own label points, thinned by rank against zoom
        if (z >= 4 && z <= 7) {
            if (MapFile* f = st_->want("tier0-countrylabels.json", frame_, 1)) {
                const int cut = z <= 4 ? 3 : z <= 5 ? 5 : 7;
                std::vector<const MapRow*> c;
                for (auto& r : f->rows) if (r.n[2] <= cut) c.push_back(&r);
                std::sort(c.begin(), c.end(), [](const MapRow* a, const MapRow* b) { return a->n[2] < b->n[2]; });
                for (const MapRow* r : c) {
                    const std::string name = up(f->str(*r, 0));
                    const char* local = f->str(*r, 1);
                    const bool two = *local && latinOnly(local) && std::string(local) != f->str(*r, 0);
                    const int w = std::max(tc.width(mf.bold13, name), two ? tc.width(mf.reg11, local) : 0);
                    const float h = lh + (two ? mf.reg11.lineHeight() : 0);
                    const float x = sxNear(r->x), y = sy(r->y);
                    if (!place(x - w / 2.f - 4, y - h / 2 - 3, w + 8.f, h + 6)) continue;
                    halo(tc, mf.bold13, name, x, y - h / 2, mp::country, 1);
                    if (two) halo(tc, mf.reg11, local, x, y - h / 2 + lh, rgb(0xbee1c8, 0.6f), 1);
                    n++;
                }
            }
        }
        // ★ the host's points come next in importance: airports and ports compete with towns for the same pixels
        // ★ towns first: they are what a reader orients by; the airfields and ports take the room that is left (the web
        //   gives each its own placer and lets them overlap — on this screen's larger type that is a smear)
        if (z >= 5 && P_.places < 2) places(tc, mf, n);
        if (z >= 5 && P_.airports) airports(tc, mf, n);
        if (z >= 5 && P_.ports) ports(tc, mf, n);
        if (z >= 12 && P_.runways) runwayEnds(tc, mf);
        gridLabels(tc, mf);
        labelsLast = n;
    }

    /** Rows of a point layer in the view, best `cap` by rank. */
    template <class Keep, class Rank>
    std::vector<std::pair<MapFile*, const MapRow*>> pick(const std::vector<MapFile*>& fs, size_t cap, Keep keep, Rank rank) {
        std::vector<std::pair<MapFile*, const MapRow*>> c;
        for (MapFile* f : fs) for (auto& r : f->rows) {
            const float b[4] = {r.x, r.y, r.x, r.y};
            if (!hits(b) || !keep(*f, r)) continue;
            c.push_back({f, &r});
        }
        std::sort(c.begin(), c.end(), [&](const auto& a, const auto& b) { return rank(*a.first, *a.second) < rank(*b.first, *b.second); });
        if (c.size() > cap) c.resize(cap);
        return c;
    }

    void places(TextCache& tc, MapFonts& mf, long& n) {
        const auto fs = resolve("places", mapTierFor("places", z_), 2);
        const int z = z_;
        const int rankCut = P_.places == 1 ? 3 : z <= 5 ? 4 : z <= 6 ? 6 : 9;
        auto rk = [](const MapFile&, const MapRow& r) { return std::isfinite(r.n[2]) ? r.n[2] : 9.f; };
        auto keep = [&](const MapFile&, const MapRow& r) {
            const float rank = std::isfinite(r.n[2]) ? r.n[2] : 9;
            const int cap = std::isfinite(r.n[3]) ? (int)r.n[3] : 0;
            if (rank > rankCut && cap != 1) return false;
            if (cap == 1 && z <= 3 && rank > 3) return false;
            return true;
        };
        for (auto& pr : pick(fs, 700, keep, rk)) {
            const MapRow& r = *pr.second;
            const float rank = std::isfinite(r.n[2]) ? r.n[2] : 9;
            const int cap = std::isfinite(r.n[3]) ? (int)r.n[3] : 0;
            const bool major = rank <= 2, shout = cap == 1 || rank == 0, big = shout && z >= 7;
            const std::string name = big ? up(pr.first->str(r, 0)) : pr.first->str(r, 0);
            const Font& font = big ? (z >= 9 ? mf.bold22 : mf.bold18) : shout ? mf.bold15 : major ? mf.bold13 : mf.reg13;
            const int w = tc.width(font, name);
            const float fh = (float)font.lineHeight();
            const float x = sxNear(r.x), y = sy(r.y);
            const float pad = z <= 5 ? 5.f : 2.f;
            float bx, by;
            if (big) { bx = x - w / 2.f - pad; by = y - fh * 1.25f - pad; }
            else { bx = x + 4 - pad; by = y - fh / 2 - pad; }
            if (!place(bx, by, w + 2 * pad + (big ? 0 : 2), fh + 2 * pad)) continue;
            const Rgba col = cap == 1 ? mp::capital : cap == 2 ? rgb(0xd2ebd7, 0.8f) : mp::label;
            const float rad = cap == 1 ? 3.2f : major ? 2.8f : 2.1f;
            if (cap == 1) { g_.circle(x, y, rad + 0.6f, 1.2f, mp::capital); g_.circle(x, y, rad - 0.6f, 0, rgb(0xf0c56a, 0.55f)); }
            else g_.circle(x, y, rad, 0, rgb(0x9ec9a8, 0.92f));
            if (big) halo(tc, font, name, x, y - fh * 1.25f, col, 1);
            else halo(tc, font, name, x + 6, y - fh / 2, col);
            poi.push_back({bx, by, (float)w + 2 * pad, fh + 2 * pad, pr.first->str(r, 0), cap == 1 ? "capital" : "place", mercLat(r.y), mercLon(r.x)});
            n++;
        }
    }

    /** A little aeroplane (no ✈ in the face): a fuselage and two wings. */
    void planeGlyph(float x, float y, float s, Rgba c) {
        g_.rect(x - s * 0.12f, y - s * 0.5f, s * 0.24f, s, c);
        g_.triangle(x - s * 0.5f, y + s * 0.05f, x + s * 0.5f, y + s * 0.05f, x, y - s * 0.25f, c);
        g_.rect(x - s * 0.25f, y + s * 0.35f, s * 0.5f, s * 0.12f, c);
    }
    void anchorGlyph(float x, float y, float s, Rgba c) {
        g_.circle(x, y - s * 0.38f, s * 0.14f, 1.2f, c);
        g_.rect(x - 0.7f, y - s * 0.25f, 1.4f, s * 0.7f, c);
        g_.rect(x - s * 0.3f, y - s * 0.1f, s * 0.6f, 1.4f, c);
        g_.circle(x, y + s * 0.15f, s * 0.32f, 1.3f, c);
    }

    void airports(TextCache& tc, MapFonts& mf, long& n) {
        const auto fs = resolve("airports", mapTierFor("airports", z_), 2);
        const int aCut = z_ <= 7 ? 0 : z_ <= 9 ? 1 : 4;   // (the web's 6 / 8, one later: this type is larger)
        auto keep = [&](const MapFile& f, const MapRow& r) {
            if (!(r.n[2] <= aCut)) return false;
            // ★★★ AN AIRFIELD WITH NO REAL CODE GETS NO LABEL ("GB-1137" cannot be tuned for)
            const std::string icao = f.str(r, 0), iata = f.str(r, 1);
            if (!iata.empty()) return true;
            if (icao.empty()) return false;
            const size_t d = icao.find('-');
            if (d == 2 && icao.size() > 3 && std::isdigit((unsigned char)icao[3])) return false;
            return true;
        };
        auto rk = [](const MapFile&, const MapRow& r) { return r.n[2]; };
        for (auto& pr : pick(fs, 200, keep, rk)) {
            const MapRow& r = *pr.second;
            const int cls = (int)r.n[2];
            std::string code = pr.first->str(r, 1); if (code.empty()) code = pr.first->str(r, 0);
            const Font& font = cls == 0 ? mf.bold15 : cls == 1 ? mf.bold13 : mf.reg13;
            const Rgba col = cls >= 3 ? rgb(0x9fb3c8, 0.7f) : rgb(0xe8b13a, cls == 0 ? 1.f : cls == 1 ? 0.92f : 0.75f);
            const int w = tc.width(font, code);
            const float x = sxNear(r.x), y = sy(r.y), fh = (float)font.lineHeight();
            if (!place(x - 7, y - fh / 2 - 2, w + 22.f, fh + 4)) continue;
            if (cls >= 3) halo(tc, mf.bold13, "H", x, y - mf.bold13.lineHeight() / 2.f, col, 1);
            else { planeGlyph(x + 1, y + 1, 12, rgb(0, 0.7f)); planeGlyph(x, y, 12, col); }
            halo(tc, font, code, x + 9, y - fh / 2, col);
            poi.push_back({x - 7, y - fh / 2 - 2, w + 22.f, fh + 4, code + " · " + pr.first->str(r, 2), "airport", mercLat(r.y), mercLon(r.x)});
            n++;
        }
    }

    void ports(TextCache& tc, MapFonts& mf, long& n) {
        MapFile* f = st_->want("tier1-ports.json", frame_, 1);
        if (!f) return;
        static const int minZ[5] = {5, 6, 7, 8, 10};
        auto clsOf = [](const MapRow& r) { return std::isfinite(r.n[2]) ? std::max(0, std::min(4, (int)r.n[2])) : 4; };
        auto keep = [&](const MapFile&, const MapRow& r) { return z_ >= minZ[clsOf(r)]; };
        auto rk = [&](const MapFile&, const MapRow& r) { return clsOf(r) * 1000.f + (clsOf(r) == 0 && std::isfinite(r.n[3]) ? r.n[3] : 0.f); };
        for (auto& pr : pick({f}, 80, keep, rk)) {
            const MapRow& r = *pr.second;
            const int c = clsOf(r);
            const std::string name = f->str(r, 0);
            const Font& font = c == 0 ? mf.bold15 : c <= 2 ? mf.reg13 : mf.reg11;
            const Rgba col = rgb(0x4db2ff, c == 0 ? 1.f : c == 1 ? 0.9f : c == 2 ? 0.8f : 0.65f);
            const int w = tc.width(font, name);
            const float x = sxNear(r.x), y = sy(r.y), fh = (float)font.lineHeight();
            if (!place(x - 7, y - fh / 2 - 2, w + 22.f, fh + 4)) continue;
            anchorGlyph(x, y, 12, col);
            halo(tc, font, name, x + 9, y - fh / 2, col);
            poi.push_back({x - 7, y - fh / 2 - 2, w + 22.f, fh + 4, name, "port", mercLat(r.y), mercLon(r.x)});
            n++;
        }
    }

    void runwayEnds(TextCache& tc, MapFonts& mf) {
        for (MapFile* f : resolve("runways", 2, 1)) for (auto& r : f->rows) {
            if (!(r.n[4] >= 2000)) continue;
            const float bx = (float)mercX(r.n[2]), by = (float)mercY(r.n[3]);
            const float b[4] = {std::min(r.x, bx), std::min(r.y, by), std::max(r.x, bx), std::max(r.y, by)};
            if (!hits(b)) continue;
            const char* le = f->str(r, 1); const char* he = f->str(r, 2);
            if (*le) halo(tc, mf.reg11, le, sxNear(r.x), sy(r.y) - 6, rgb(0xe8e2d4, 0.9f), 1);
            if (*he) halo(tc, mf.reg11, he, sxNear(bx), sy(by) - 6, rgb(0xe8e2d4, 0.9f), 1);
        }
    }

    void gridLabels(TextCache& tc, MapFonts& mf) {
        if (z_ < 4 || !P_.grid) return;
        const int level = z_ >= 12 ? 2 : z_ >= 6 ? 1 : 0, chars = level == 0 ? 2 : level == 1 ? 4 : 6;
        const double dLon = level == 0 ? 20 : level == 1 ? 2 : 2.0 / 24, dLat = level == 0 ? 10 : level == 1 ? 1 : 1.0 / 24;
        const double w = std::max(-180.0, std::floor(mercLon(box_[0]) / dLon) * dLon), e = std::min(180.0, std::ceil(mercLon(box_[2]) / dLon) * dLon);
        const double nn = std::min(85.0, std::ceil(mercLat(box_[1]) / dLat) * dLat), s = std::max(-85.0, std::floor(mercLat(box_[3]) / dLat) * dLat);
        if ((e - w) / dLon * (nn - s) / dLat > 320) return;
        for (double lon = w; lon < e - 1e-9; lon += dLon)
            for (double lat = s; lat < nn - 1e-9; lat += dLat) {
                const std::string code = maidenhead(lat + dLat / 2, lon + dLon / 2).substr(0, (size_t)chars);
                tc.draw(g_, mf.reg11, code, sx(mercX(lon + dLon / 2)), sy(mercY(lat + dLat / 2)) - 6, rgb(0xc98f2e, 0.5f), 1);
            }
    }
};

}  // namespace hh
