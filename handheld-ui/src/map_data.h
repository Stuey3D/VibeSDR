// map_data.h — the map's DATA: /mapdata/v1/ from the engine (the supervisor serves it itself, radio or none —
// handheld.cpp "THE MAP IS THE DEVICE'S"), read on ONE worker thread, parsed straight into GPU-ready arrays.
//
// ★★★ THE FORMAT IS OURS (scripts/gen-vibemap-source.mjs and scripts/mapdata/ build it; web/mapkit/vibemap.js reads it):
//     index.json — files{} (what is INSTALLED: the basic pack always, the detail pack's tier2 when present), sharded{}
//     (a layer cut by area: each shard with its own data box), relief{} (the global image + relief-tiles.json), and
//     licences{} (the attribution the CC BY / ODbL layers REQUIRE). Each layer file is plain JSON:
//       rings     [[[lon,lat],…],…]                 countries {"FJ":[ring,…],…} · cover {"ice":[ring,…],…}
//       lines     [[[lon,lat],…],…]                 geolines  [{"name":…,"lines":[line,…]},…]
//       rows      [["name",lon,lat,rank,…],…]       (places, airports, ports, countrylabels, regions, runways)
// ★★ NO JSON DOM FOR GEOMETRY. tier2-countries is 9 MB and 546 k points: json.h's tree would be ~120 bytes a number.
//    The scanner below reads the text once into float pairs (8 bytes a point) and the fan / edge indices the GPU draws.
// ★★ BOUNDED (no OOM, ever — 730 MB device, ~500 MB free with the radio): one file in flight; a body over 12 MB is
//    refused; what is resident (GPU buffers + point rows + images) is capped at kMapCacheBytes and the least recently
//    drawn file goes first (tier0 — the world view — is never evicted). An evicted file is read again from the card.
// ★ A MISS IS NOT KEPT (vibemap.js fetchJson): a failed read is retried after 3 s, never remembered as "absent".
// ★ Threads: the worker only fetches and parses (no GL, no App). The UI thread uploads (≤ 2 files a frame) and evicts.
#pragma once
#include "json.h"
#include "map_geo.h"

#include <GLES3/gl3.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hh {

/* ★★ 96 MB, not 56 (Stuart, 2026-11-08: "we definitely need the bigger ram cache"). At town-label zooms the picture alone
 *  held 51–59 MB (handheld40/43 logs), so the 56 MB cap kept evicting and re-fetching the surrounding area ("loading map
 *  data…"). With the map and the radio up the card had 168–250 MB available of 730 MB; 96 MB leaves > 100 MB. */
constexpr size_t kMapCacheBytes = 96u << 20;   // GPU buffers + rows + relief textures
constexpr size_t kMapBodyMax = 12u << 20;       // the largest shard is tier2-countries, 9.05 MB

/** GET a whole file from the engine (worker threads only). 0 = no answer; the body is refused past `maxBytes`. */
inline int mapHttpGet(int port, const std::string& path, std::string& body, size_t maxBytes = kMapBodyMax) {
    body.clear();
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{8, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return 0; }
    // ★ no Accept-Encoding: the files go out raw (a .gz sibling would need inflating here)
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) != (ssize_t)req.size()) { ::close(fd); return 0; }
    std::string head;
    char buf[16384];
    int status = 0;
    size_t want = 0;
    bool inBody = false;
    for (;;) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        if (!inBody) {
            head.append(buf, (size_t)r);
            const size_t he = head.find("\r\n\r\n");
            if (he == std::string::npos) { if (head.size() > 16384) break; continue; }
            inBody = true;
            const size_t sp = head.find(' ');
            if (sp != std::string::npos) status = std::atoi(head.c_str() + sp + 1);
            std::string lower = head.substr(0, he);
            for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
            const size_t cl = lower.find("content-length:");
            if (cl != std::string::npos) want = std::strtoul(lower.c_str() + cl + 15, nullptr, 10);
            if (want > maxBytes) { status = 413; break; }
            if (want) body.reserve(want);
            body.append(head, he + 4, std::string::npos);
            head.clear();
        } else body.append(buf, (size_t)r);
        if (body.size() > maxBytes) { status = 413; break; }
        if (want && body.size() >= want) break;
    }
    ::close(fd);
    if (status == 200 && want && body.size() < want) status = 0;   // ★ cut short (a busy engine, the 8 s timeout): a miss, retried
    if (status != 200) body.clear();
    return status;
}

// ── the parsed forms ─────────────────────────────────────────────────────────────────────────────────────────────────

/** One drawable group of a geometry file (a cover kind, a geoline, or the whole file). Vertices are Mercator world
 *  coordinates; fills carry (x, y), lines carry (x, y, distance along the line in world units — the dashes). */
struct MapGroup {
    std::string key;
    std::vector<float> v;                 // staged until upload, then freed
    std::vector<uint32_t> fan, edge;      // fills: fan triangles (stencil); edges: GL_LINES pairs (outlines / lines)
    std::vector<float> boxes;             // one (x0, y0, x1, y1) per ring / line — the CPU's own culling + hit tests
    std::vector<uint32_t> fanAt, edgeAt;  // ★ each fill PIECE's / ring's first index (+ an end sentinel): only those in view are drawn
    std::vector<float> fanBoxes;          // ★ one (x0, y0, x1, y1) per fill piece (mapSplitFill) — fanAt's culling boxes
    std::vector<uint32_t> rv;             // parse-time: (first vertex, count) per ring, until mapFinish
    // GPU
    GLuint vbo = 0, ibo = 0;
    GLsizei fanN = 0, edgeN = 0;          // index counts (fan first in the ibo, then edges)
    float box[4] = {1, 1, 0, 0};          // the group's whole extent
};
/** One point row (places, airports, ports, labels, regions, runways): numbers and strings in the order they came. */
struct MapRow {
    float x = 0, y = 0;                   // Mercator of the first lon / lat pair
    float n[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
    uint32_t s[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};   // offsets into MapFile::pool
};
enum MapFileKind { MF_FILL, MF_LINE, MF_ROWS, MF_IMAGE };

struct MapFile {
    std::string name;
    MapFileKind kind = MF_FILL;
    bool outline = false;                 // fills that are also drawn as lines (countries, coast, islands)
    std::vector<MapGroup> groups;
    std::vector<MapRow> rows;
    std::string pool;                     // the rows' strings, '\0'-separated
    SDL_Surface* img = nullptr;           // decoded image, until upload
    GLuint tex = 0;
    int texW = 0, texH = 0;
    size_t bytes = 0;                     // resident cost (the cache's measure)
    bool uploaded = false;
    uint64_t used = 0;                    // the last frame it was drawn / asked for
    const char* str(const MapRow& r, int i) const { return r.s[i] == UINT32_MAX ? "" : pool.c_str() + r.s[i]; }
    ~MapFile() { if (img) SDL_FreeSurface(img); }
};

// ── the scanner: geometry JSON → arrays, no tree ─────────────────────────────────────────────────────────────────────

class MapScan {
public:
    explicit MapScan(const std::string& t) : p_(t.data()), e_(t.data() + t.size()) {}
    bool ok() const { return ok_; }
    void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) p_++; }
    bool peek(char c) { ws(); return p_ < e_ && *p_ == c; }
    bool eat(char c) { ws(); if (p_ < e_ && *p_ == c) { p_++; return true; } return false; }
    void need(char c) { if (!eat(c)) ok_ = false; }
    double num() {
        ws();
        char* end = nullptr;
        const double d = std::strtod(p_, &end);
        if (end == p_) { ok_ = false; return 0; }
        p_ = end;
        return d;
    }
    std::string str() {
        ws();
        std::string out;
        if (p_ >= e_ || *p_ != '"') { ok_ = false; return out; }
        p_++;
        while (p_ < e_ && *p_ != '"') {
            if (*p_ == '\\' && p_ + 1 < e_) {
                const char c = p_[1]; p_ += 2;
                if (c == 'u' && p_ + 4 <= e_) {
                    const uint32_t cp = (uint32_t)std::strtoul(std::string(p_, 4).c_str(), nullptr, 16); p_ += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
                    else { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
                } else out += c == 'n' ? ' ' : c == 't' ? ' ' : c;
            } else out += *p_++;
        }
        if (p_ < e_) p_++;
        return out;
    }
    /** Skip one value of any kind. */
    void skip() {
        ws();
        if (p_ >= e_) { ok_ = false; return; }
        if (*p_ == '"') { str(); return; }
        if (*p_ == '[' || *p_ == '{') {
            int depth = 0;
            bool inStr = false;
            for (; p_ < e_; p_++) {
                const char c = *p_;
                if (inStr) { if (c == '\\') p_++; else if (c == '"') inStr = false; continue; }
                if (c == '"') inStr = true;
                else if (c == '[' || c == '{') depth++;
                else if (c == ']' || c == '}') { if (--depth == 0) { p_++; return; } }
            }
            ok_ = false;
            return;
        }
        while (p_ < e_ && *p_ != ',' && *p_ != ']' && *p_ != '}') p_++;
    }
    /** One ring / line: [[lon,lat],…] appended to g (a fill: fan + optional edges; a line: edges + distance). */
    void path(MapGroup& g, bool fill, bool edges) {
        need('[');
        const uint32_t base = (uint32_t)(g.v.size() / (fill ? 2 : 3));
        uint32_t n = 0;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, px = 0, py = 0;
        double dist = 0;
        while (ok_ && !peek(']')) {
            need('[');
            const double lon = num(); need(',');
            const double lat = num();
            while (ok_ && eat(',')) num();          // (a third coordinate, if a file ever carries one)
            need(']');
            const float x = (float)mercX(lon), y = (float)mercY(lat);
            if (n && !fill) dist += std::hypot((double)x - px, (double)y - py);
            g.v.push_back(x); g.v.push_back(y);
            if (!fill) g.v.push_back((float)dist);
            px = x; py = y;
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            n++;
            eat(',');
        }
        need(']');
        if (n < 2 || (fill && n < 3)) { g.v.resize((size_t)base * (fill ? 2 : 3)); return; }
        (void)edges;
        g.rv.push_back(base); g.rv.push_back(n);
        g.boxes.insert(g.boxes.end(), {x0, y0, x1, y1});
        g.box[0] = std::min(g.box[0], x0); g.box[1] = std::min(g.box[1], y0);
        g.box[2] = std::max(g.box[2], x1); g.box[3] = std::max(g.box[3], y1);
    }
    /** [path, path, …] */
    void paths(MapGroup& g, bool fill, bool edges) {
        need('[');
        while (ok_ && !peek(']')) { path(g, fill, edges); eat(','); }
        need(']');
    }
    /** One row: ["s", n, n, …] */
    void row(MapFile& f) {
        need('[');
        MapRow r;
        int ni = 0, si = 0;
        while (ok_ && !peek(']')) {
            ws();
            if (p_ < e_ && *p_ == '"') {
                const std::string s = str();
                if (si < 3) { r.s[si++] = (uint32_t)f.pool.size(); f.pool += s; f.pool += '\0'; }
            } else if (p_ < e_ && (*p_ == '[' || *p_ == '{' || *p_ == 'n' || *p_ == 't' || *p_ == 'f')) skip();
            else { const double d = num(); if (ni < 6) r.n[ni++] = (float)d; }
            eat(',');
        }
        need(']');
        if (ni >= 2 && std::isfinite(r.n[0]) && std::isfinite(r.n[1])) {
            r.x = (float)mercX(r.n[0]); r.y = (float)mercY(r.n[1]);
            f.rows.push_back(r);
        }
    }
private:
    const char* p_;
    const char* e_;
    bool ok_ = true;
};

/* ★★★ FILLS CUT INTO CELLS AT LOAD (2026-11-08 — the R36S map's GPU resets, at last). A fill is drawn by stencil-then-
 *  cover as a fan from its ring's FIRST point. Zoomed in, that point is usually far off the screen, so a coast ring of
 *  tens of thousands of points became tens of thousands of slivers that EACH cross the whole buffer — on the Mali-G31
 *  the tiler bins every one into every tile it crosses, and one fragment job ran past Panfrost's 500 ms limit: the
 *  "gpu sched timeout"s (handheld46: 14 of them in a re-open with nothing loading, 486 k indices), the reset that left
 *  "a zoomed in UK over a zoomed out USA", and why ZOOMED IN was slow while the world view was fine.
 *  So a big ring is cut into cells (Sutherland–Hodgman against each quadrant, recursively) until a piece has ≤ 64 points
 *  or its cell is ~2 km; each piece is fanned from its own first point, so no sliver is bigger than its cell, and each
 *  piece carries its own box, so only the cells in view are drawn at all. Inside a convex cell the clipped polygon has
 *  the same winding as the ring, so the even-odd stencil gives the same picture; the bridges SH adds along a cell edge
 *  are drawn twice and cancel. Interior cells stop early as four-point squares. Cost: once, on the loader thread. */
struct MapP2 { float x, y; };
inline void mapClipSide(const std::vector<MapP2>& in, std::vector<MapP2>& out, int side, float c) {
    out.clear();
    const size_t n = in.size();
    if (n < 3) return;
    auto inside = [&](const MapP2& p) { return side == 0 ? p.x >= c : side == 1 ? p.x <= c : side == 2 ? p.y >= c : p.y <= c; };
    auto cross = [&](const MapP2& a, const MapP2& b) {
        if (side < 2) { const float t = (c - a.x) / (b.x - a.x); return MapP2{c, a.y + t * (b.y - a.y)}; }
        const float t = (c - a.y) / (b.y - a.y); return MapP2{a.x + t * (b.x - a.x), c};
    };
    MapP2 prev = in[n - 1]; bool pin = inside(prev);
    for (size_t i = 0; i < n; i++) {
        const MapP2& cur = in[i]; const bool cin = inside(cur);
        if (cin) { if (!pin) out.push_back(cross(prev, cur)); out.push_back(cur); }
        else if (pin) out.push_back(cross(prev, cur));
        prev = cur; pin = cin;
    }
}
inline void mapSplitFill(const std::vector<MapP2>& poly, float x0, float y0, float x1, float y1, std::vector<float>& v, MapGroup& g, std::vector<MapP2>& t1, std::vector<MapP2>& t2) {
    if (poly.size() < 3) return;
    if (poly.size() <= 64 || (x1 - x0 < 1.f / 16384 && y1 - y0 < 1.f / 16384)) {
        const uint32_t base = (uint32_t)(v.size() / 2);
        float b0 = 1e9f, b1 = 1e9f, b2 = -1e9f, b3 = -1e9f;
        for (auto& p : poly) { v.push_back(p.x); v.push_back(p.y); b0 = std::min(b0, p.x); b1 = std::min(b1, p.y); b2 = std::max(b2, p.x); b3 = std::max(b3, p.y); }
        g.fanAt.push_back((uint32_t)g.fan.size());
        for (uint32_t k = 1; k + 1 < poly.size(); k++) { g.fan.push_back(base); g.fan.push_back(base + k); g.fan.push_back(base + k + 1); }
        g.fanBoxes.insert(g.fanBoxes.end(), {b0, b1, b2, b3});
        return;
    }
    // ★ split the longer side only once it is far the longer (a tall ring is not cut into flat strips)
    const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    const bool sx = x1 - x0 >= (y1 - y0) * 0.5f, sy = y1 - y0 >= (x1 - x0) * 0.5f;
    for (int qx = 0; qx < (sx ? 2 : 1); qx++)
        for (int qy = 0; qy < (sy ? 2 : 1); qy++) {
            const float cx0 = sx ? (qx ? mx : x0) : x0, cx1 = sx ? (qx ? x1 : mx) : x1;
            const float cy0 = sy ? (qy ? my : y0) : y0, cy1 = sy ? (qy ? y1 : my) : y1;
            std::vector<MapP2> a, b;
            mapClipSide(poly, a, 0, cx0); mapClipSide(a, b, 1, cx1); mapClipSide(b, a, 2, cy0); mapClipSide(a, b, 3, cy1);
            mapSplitFill(b, cx0, cy0, cx1, cy1, v, g, t1, t2);
        }
}

/** ★★ RINGS IN SPACE ORDER. After the parse, a group's rings are re-laid in Morton (Z-curve) order of their centres and
 *  their indices made: the rings in any one view then sit in a few contiguous runs of the index buffer, so a view draws
 *  ONLY what it can see in a handful of calls (tier2 lakes are 13 000 rings a shard; drawing a whole shard for the
 *  twenty in view was 4 M indices a render). */
inline void mapFinish(MapGroup& g, bool fill, bool edges) {
    const size_t nr = g.rv.size() / 2, stride = fill ? 2 : 3;
    if (!nr) { g.v.clear(); return; }
    auto spread = [](uint32_t x) { x &= 0xffff; x = (x | (x << 8)) & 0x00FF00FF; x = (x | (x << 4)) & 0x0F0F0F0F; x = (x | (x << 2)) & 0x33333333; x = (x | (x << 1)) & 0x55555555; return x; };
    std::vector<std::pair<uint32_t, uint32_t>> order(nr);
    for (size_t i = 0; i < nr; i++) {
        const float* b = &g.boxes[i * 4];
        const uint32_t mx = (uint32_t)std::max(0.f, std::min(65535.f, (b[0] + b[2]) * 0.5f * 65535.f));
        const uint32_t my = (uint32_t)std::max(0.f, std::min(65535.f, (b[1] + b[3]) * 0.5f * 65535.f));
        order[i] = {spread(mx) | spread(my) << 1, (uint32_t)i};
    }
    std::sort(order.begin(), order.end());
    std::vector<float> v, boxes;
    v.reserve(g.v.size()); boxes.reserve(g.boxes.size());
    g.fan.clear(); g.edge.clear(); g.fanAt.clear(); g.edgeAt.clear(); g.fanBoxes.clear();
    std::vector<MapP2> poly, t1, t2;
    for (auto& o : order) {
        const uint32_t i = o.second, first = g.rv[2 * i], n = g.rv[2 * i + 1];
        const uint32_t base = (uint32_t)(v.size() / stride);
        v.insert(v.end(), g.v.begin() + (long)(first * stride), g.v.begin() + (long)((first + n) * stride));
        boxes.insert(boxes.end(), g.boxes.begin() + (long)(i * 4), g.boxes.begin() + (long)(i * 4 + 4));
        g.edgeAt.push_back((uint32_t)g.edge.size());
        if (fill) {
            if (n <= 64) {   // ★ a small ring: its own fan, its own box
                g.fanAt.push_back((uint32_t)g.fan.size());
                for (uint32_t k = 1; k + 1 < n; k++) { g.fan.push_back(base); g.fan.push_back(base + k); g.fan.push_back(base + k + 1); }
                g.fanBoxes.insert(g.fanBoxes.end(), g.boxes.begin() + (long)(i * 4), g.boxes.begin() + (long)(i * 4 + 4));
            } else {          // ★ a big one: cut into cells (mapSplitFill) — its pieces' vertices follow the plain ones
                poly.clear();
                for (uint32_t k = 0; k < n; k++) poly.push_back({g.v[(first + k) * 2], g.v[(first + k) * 2 + 1]});
                const float* bb = &g.boxes[i * 4];
                mapSplitFill(poly, bb[0], bb[1], bb[2], bb[3], v, g, t1, t2);
            }
        }
        if (!fill || edges) for (uint32_t k = 0; k + 1 < n; k++) { g.edge.push_back(base + k); g.edge.push_back(base + k + 1); }
    }
    g.fanAt.push_back((uint32_t)g.fan.size()); g.edgeAt.push_back((uint32_t)g.edge.size());
    g.v.swap(v); g.boxes.swap(boxes);
    std::vector<uint32_t>().swap(g.rv);
}

/** The file's layer name: "tier2-lakes.s13.json" → "lakes". */
inline std::string mapLayerOf(const std::string& file) {
    const size_t d = file.find('-');
    std::string l = d == std::string::npos ? file : file.substr(d + 1);
    const size_t dot = l.find('.');
    return dot == std::string::npos ? l : l.substr(0, dot);
}

/** Parse a fetched body into `f` (worker thread). */
inline bool mapParse(MapFile& f, const std::string& body) {
    const std::string layer = mapLayerOf(f.name);
    MapScan s(body);
    static const std::set<std::string> rowLayers = {"places", "airports", "ports", "countrylabels", "regions", "runways"};
    static const std::set<std::string> lineLayers = {"rivers", "roads", "rail", "admin1", "borders", "reefs"};
    if (rowLayers.count(layer)) {
        f.kind = MF_ROWS;
        s.need('[');
        while (s.ok() && !s.peek(']')) { s.row(f); s.eat(','); }
        f.rows.shrink_to_fit(); f.pool.shrink_to_fit();
        f.bytes = f.rows.size() * sizeof(MapRow) + f.pool.size();
        return s.ok();
    }
    if (layer == "geolines") {
        f.kind = MF_LINE;
        s.need('[');
        while (s.ok() && !s.peek(']')) {
            MapGroup g;
            s.need('{');
            while (s.ok() && !s.peek('}')) {
                const std::string k = s.str(); s.need(':');
                if (k == "name") g.key = s.str();
                else if (k == "lines") s.paths(g, false, true);
                else s.skip();
                s.eat(',');
            }
            s.need('}');
            f.groups.push_back(std::move(g));
            s.eat(',');
        }
        for (auto& g : f.groups) mapFinish(g, false, true);
        return s.ok();
    }
    const bool lines = lineLayers.count(layer) > 0;
    f.kind = lines ? MF_LINE : MF_FILL;
    f.outline = layer == "countries" || layer == "coast" || layer == "islands";
    if (s.peek('{')) {
        // countries {"FJ":[ring…]} → one group; cover {"ice":[ring…]} → a group a kind
        s.need('{');
        MapGroup one;
        while (s.ok() && !s.peek('}')) {
            const std::string k = s.str(); s.need(':');
            if (layer == "cover") { MapGroup g; g.key = k; s.paths(g, !lines, f.outline); f.groups.push_back(std::move(g)); }
            else s.paths(one, !lines, f.outline);
            s.eat(',');
        }
        s.need('}');
        if (layer != "cover") f.groups.push_back(std::move(one));
    } else {
        MapGroup g;
        s.paths(g, !lines, f.outline);
        f.groups.push_back(std::move(g));
    }
    for (auto& g : f.groups) mapFinish(g, !lines, f.outline);
    return s.ok();
}

// ── the index ────────────────────────────────────────────────────────────────────────────────────────────────────────

struct MapShard { std::string file; float box[4]; };   // box in Mercator (x0, y0, x1, y1)
struct MapReliefTile { std::string file; float box[4]; };
struct MapIndex {
    bool loaded = false;
    std::set<std::string> files;
    std::map<std::string, std::vector<MapShard>> sharded;   // "tier2-lakes" → shards
    std::vector<MapReliefTile> reliefTiles;
    std::string reliefBasic = "relief.png";
    std::vector<std::string> attribution;                   // the licences' required credits
    bool has(const std::string& f) const { return files.count(f) > 0; }
    bool detail() const { return has("tier2-countries.json"); }
};

inline void mapBoxFromLonLat(const Json& b, float out[4]) {
    // [west, south, east, north] → Mercator x0, y0 (north), x1, y1 (south)
    out[0] = (float)mercX(b[0].num()); out[2] = (float)mercX(b[2].num());
    out[1] = (float)mercY(b[3].num()); out[3] = (float)mercY(b[1].num());
}

// ── the store: the worker, the queue, the cache ──────────────────────────────────────────────────────────────────────

class MapStore {
public:
    ~MapStore() { stop(); }
    void start(int port) {
        if (run_) return;
        port_ = port; run_ = true;
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        if (!run_.exchange(false)) return;
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    const MapIndex& index() const { return ix_; }
    bool indexReady() const { return ix_.loaded; }
    /** Something new is drawable (an upload happened) — the map re-renders. */
    uint64_t generation() const { return gen_; }

    /** UI thread: the file if it is on the GPU, else nullptr (and it is asked for). `frame` stamps its use. */
    MapFile* want(const std::string& name, uint64_t frame, int prio = 0) {
        auto it = files_.find(name);
        if (it != files_.end()) { it->second->used = frame; return it->second.get(); }
        ask(name, prio);
        return nullptr;
    }
    /** Is it resident (no request)? */
    MapFile* peekFile(const std::string& name) { auto it = files_.find(name); return it == files_.end() ? nullptr : it->second.get(); }

    /** UI thread, once a frame while the map is up: take the worker's results, upload ≤ 2, evict past the cap. */
    void pump(uint64_t frame) {
        frame_ = frame;
        std::vector<std::shared_ptr<MapFile>> got;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ixFresh_) { ix_ = ixNext_; ixFresh_ = false; gen_++; }
            for (int i = 0; i < 2 && !done_.empty(); i++) { got.push_back(done_.front()); asked_.erase(done_.front()->name); done_.pop_front(); }
        }
        for (auto& f : got) {
            upload(*f);
            f->used = frame;
            files_[f->name] = f;
            total_ += f->bytes;
            gen_++;
        }
        if (total_ > kMapCacheBytes) evict();
    }
    /** The frame of the last basemap render (MapRender::render): the files it touched carry this `used`, and they are
     *  the picture on screen — evict() never takes one (see there). */
    void noteRender(uint64_t frame) { lastRender_ = frame; }
    size_t residentBytes() const { return total_; }
    int residentFiles() const { return (int)files_.size(); }
    std::string lastError() { std::lock_guard<std::mutex> lk(m_); return err_; }
    /** Everything off the GPU (the map closed for good / memory pressure). UI thread. */
    void dropAll() {
        for (auto& kv : files_) release(*kv.second);
        files_.clear(); total_ = 0; gen_++;
    }
    /** ★ Everything gone, the loader's queue too (the map closed — mapClosePage). A file in flight lands in done_ and is
     *  uploaded fresh the next time the map is up. UI thread. */
    void flush() {
        { std::lock_guard<std::mutex> lk(m_); q_.clear(); done_.clear(); asked_.clear(); }
        dropAll();
        lastRender_ = 0;
    }
    /** Files the loader is still working through (the "loading" hint). */
    int pending() { std::lock_guard<std::mutex> lk(m_); return (int)q_.size() + (busy_ ? 1 : 0) + (int)done_.size(); }

private:
    int port_ = 48000;
    uint64_t lastRender_ = 0;
    std::atomic<bool> run_{false};
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, int>> q_;            // (file, priority) — newest asks first within a priority
    std::set<std::string> asked_;                          // queued or in flight
    std::map<std::string, double> failedAt_;               // a miss: retried after 3 s
    std::deque<std::shared_ptr<MapFile>> done_;
    bool busy_ = false, ixFresh_ = false, ixAsked_ = false;
    std::string err_;
    MapIndex ix_, ixNext_;
    std::map<std::string, std::shared_ptr<MapFile>> files_;  // UI thread only
    size_t total_ = 0;
    uint64_t gen_ = 0, frame_ = 0;

    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    void ask(const std::string& name, int prio) {
        std::lock_guard<std::mutex> lk(m_);
        if (asked_.count(name)) {
            // ★ asked again: to the front of its priority (the file the view needs NOW goes first)
            for (auto it = q_.begin(); it != q_.end(); ++it) if (it->first == name) { auto e = *it; q_.erase(it); q_.push_front({e.first, std::max(e.second, prio)}); break; }
            return;
        }
        auto f = failedAt_.find(name);
        if (f != failedAt_.end() && now() - f->second < 3.0) return;
        asked_.insert(name);
        q_.push_front({name, prio});
        // ★ bounded: a long pan can ask for more than will ever be drawn — the oldest asks fall off
        while (q_.size() > 48) { asked_.erase(q_.back().first); q_.pop_back(); }
        cv_.notify_one();
    }

    void loop() {
        while (run_) {
            if (!ixAsked_) { loadIndex(); continue; }
            std::string name;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return !run_ || !q_.empty(); });
                if (!run_) return;
                if (q_.empty()) continue;
                // the highest priority, newest first
                auto best = q_.begin();
                for (auto it = q_.begin(); it != q_.end(); ++it) if (it->second > best->second) best = it;
                name = best->first; q_.erase(best);
                busy_ = true;
            }
            auto f = std::make_shared<MapFile>();
            f->name = name;
            bool ok = false;
            std::string body;
            const double t0 = now();
            const int st = mapHttpGet(port_, "/mapdata/v1/" + name, body);
            const double t1 = now();
            const size_t got = body.size();
            if (st == 200) {
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0) ok = decodeImage(*f, body);
                else { ok = mapParse(*f, body); }
                std::string().swap(body);
            }
            static const bool logIt = std::getenv("VIBE_HHUI_MAP_LOG") != nullptr;
            if (logIt) std::fprintf(stderr, "vibe-hhui: map read %s — %zu bytes, fetch %.0f ms, parse %.0f ms, %zu resident\n", name.c_str(), got, (t1 - t0) * 1e3, (now() - t1) * 1e3, f->bytes);
            std::lock_guard<std::mutex> lk(m_);
            busy_ = false;
            if (ok) { done_.push_back(f); failedAt_.erase(name); }   // ★ still "asked" until pump() takes it (no second read)
            else {
                asked_.erase(name);
                failedAt_[name] = now();
                char b[160]; std::snprintf(b, sizeof b, "%s: %s", name.c_str(), st == 0 ? "the engine did not answer" : st == 200 ? ("unreadable (" + std::to_string(got) + " bytes)").c_str() : ("HTTP " + std::to_string(st)).c_str());
                err_ = b;
                std::fprintf(stderr, "vibe-hhui: map %s\n", b);
            }
        }
    }

    void loadIndex() {
        std::string body;
        const int st = mapHttpGet(port_, "/mapdata/v1/index.json", body, 1u << 20);
        if (st != 200) {
            { std::lock_guard<std::mutex> lk(m_); err_ = st == 0 ? "the engine did not answer" : "no map data on this device (HTTP " + std::to_string(st) + ")"; }
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::seconds(2), [this] { return !run_.load(); });
            return;
        }
        MapIndex ix;
        {
            const Json j = Json::parse(body);
            for (auto& kv : j["files"].o) ix.files.insert(kv.first);
            for (auto& kv : j["sharded"].o) {
                auto& v = ix.sharded[kv.first];
                for (auto& sh : kv.second.a) { MapShard s; s.file = sh["file"].str(); mapBoxFromLonLat(sh["box"], s.box); v.push_back(s); }
            }
            if (!j["relief"]["basic"].str().empty()) ix.reliefBasic = j["relief"]["basic"].str();
            // ★★★ THE CREDIT TRAVELS WITH THE DATA (vibemap.js MAP_ATTRIBUTION): every licence that names one
            for (auto& kv : j["licences"].o) if (!kv.second["attribution"].str().empty()) ix.attribution.push_back(kv.second["attribution"].str());
            const std::string tiles = j["relief"]["tiles"].str();
            if (!tiles.empty() && ix.has(tiles)) {
                std::string tb;
                if (mapHttpGet(port_, "/mapdata/v1/" + tiles, tb, 1u << 20) == 200) {
                    const Json t = Json::parse(tb);
                    for (auto& e : t.a) { MapReliefTile r; r.file = e["file"].str(); mapBoxFromLonLat(e["bounds"], r.box); if (ix.has(r.file)) ix.reliefTiles.push_back(r); }
                }
            }
        }
        ix.loaded = true;
        std::fprintf(stderr, "vibe-hhui: map index — %zu files, detail pack %s, %zu relief tiles\n", ix.files.size(), ix.detail() ? "installed" : "absent", ix.reliefTiles.size());
        std::lock_guard<std::mutex> lk(m_);
        ixNext_ = std::move(ix); ixFresh_ = true; ixAsked_ = true; err_.clear();
    }

    /** PNG → RGBA surface; the global relief (2700²) is halved — a 29 MB texture would be the whole budget. */
    static bool decodeImage(MapFile& f, const std::string& body) {
        f.kind = MF_IMAGE;
        SDL_RWops* rw = SDL_RWFromConstMem(body.data(), (int)body.size());
        SDL_Surface* s = rw ? IMG_Load_RW(rw, 1) : nullptr;
        if (!s) return false;
        SDL_Surface* rgba = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
        SDL_FreeSurface(s);
        if (!rgba) return false;
        if (rgba->w > 2048 || rgba->h > 2048) {
            const int w = rgba->w / 2, h = rgba->h / 2;
            SDL_Surface* half = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
            if (!half) { SDL_FreeSurface(rgba); return false; }
            for (int y = 0; y < h; y++) {
                const uint8_t* r0 = (const uint8_t*)rgba->pixels + (size_t)(2 * y) * rgba->pitch;
                const uint8_t* r1 = r0 + rgba->pitch;
                uint8_t* o = (uint8_t*)half->pixels + (size_t)y * half->pitch;
                for (int x = 0; x < w; x++)
                    for (int c = 0; c < 4; c++) o[x * 4 + c] = (uint8_t)((r0[x * 8 + c] + r0[x * 8 + 4 + c] + r1[x * 8 + c] + r1[x * 8 + 4 + c] + 2) / 4);
            }
            SDL_FreeSurface(rgba);
            rgba = half;
        }
        f.img = rgba;
        f.bytes = (size_t)rgba->w * rgba->h * 4;
        return true;
    }

    void upload(MapFile& f) {
        if (f.kind == MF_IMAGE) {
            if (!f.img) return;
            glGenTextures(1, &f.tex);
            glBindTexture(GL_TEXTURE_2D, f.tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, f.img->pitch / 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f.img->w, f.img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.img->pixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            f.texW = f.img->w; f.texH = f.img->h;
            // ★ the log's side of the GPU-timeout hunt (handheld44): each image put on the GPU, against the kernel's times
            std::fprintf(stderr, "vibe-hhui: map upload image %s %dx%d (%.1f MB)\n", f.name.c_str(), f.img->w, f.img->h, f.img->w * f.img->h * 4 / 1048576.0);
            SDL_FreeSurface(f.img); f.img = nullptr;
            f.uploaded = true;
            return;
        }
        size_t gpu = 0;
        for (auto& g : f.groups) {
            if (g.v.empty()) continue;
            glGenBuffers(1, &g.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g.v.size() * sizeof(float)), g.v.data(), GL_STATIC_DRAW);
            std::vector<uint32_t> all;
            all.reserve(g.fan.size() + g.edge.size());
            all.insert(all.end(), g.fan.begin(), g.fan.end());
            all.insert(all.end(), g.edge.begin(), g.edge.end());
            glGenBuffers(1, &g.ibo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(all.size() * sizeof(uint32_t)), all.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            g.fanN = (GLsizei)g.fan.size(); g.edgeN = (GLsizei)g.edge.size();
            gpu += g.v.size() * sizeof(float) + all.size() * sizeof(uint32_t);
            // ★ the CPU copies go: only the per-ring boxes stay (culling, hit tests)
            std::vector<float>().swap(g.v); std::vector<uint32_t>().swap(g.fan); std::vector<uint32_t>().swap(g.edge);
            g.boxes.shrink_to_fit();
            g.fanBoxes.shrink_to_fit();
            gpu += (g.boxes.size() + g.fanBoxes.size() + g.fanAt.size() + g.edgeAt.size()) * sizeof(float);
        }
        f.bytes += gpu;
        if (gpu > (2u << 20)) std::fprintf(stderr, "vibe-hhui: map upload geometry %s %.1f MB\n", f.name.c_str(), gpu / 1048576.0);
        f.uploaded = true;
    }
    static void release(MapFile& f) {
        for (auto& g : f.groups) { if (g.vbo) glDeleteBuffers(1, &g.vbo); if (g.ibo) glDeleteBuffers(1, &g.ibo); g.vbo = g.ibo = 0; }
        if (f.tex) glDeleteTextures(1, &f.tex);
        f.tex = 0;
    }
    /* ★★★ NEVER EVICT THE PICTURE ON SCREEN (Stuart, 2026-11-08: "when you zoom into the map at the point the town names
     *  appear it constantly says loading and seems to flicker lots"). The basemap is rendered into a buffer only when
     *  something changes, so a file's `used` is the frame of the LAST RENDER, not of every frame. The old test
     *  (used + 2 < frame) therefore offered the whole visible set for eviction two frames after each render; past the cap
     *  — and at town-label zooms the visible files alone are near it — eviction took them, bumped gen_, the map
     *  re-rendered, found them missing ("loading map data…"), fetched them, went over the cap, evicted them: a loop.
     *  Now the files of the last render (used ≥ lastRender_) are never candidates, and evicting a file that was NOT in
     *  the picture does not bump gen_ (nothing on screen changed, so no re-render). When the picture alone is over the
     *  cap the cache simply holds it — it falls back under the cap as soon as the view moves on. */
    void evict() {
        std::vector<std::pair<uint64_t, std::string>> c;
        for (auto& kv : files_)
            if (kv.first.compare(0, 6, "tier0-") != 0 && kv.second->used + 2 < frame_ && kv.second->used < lastRender_)
                c.push_back({kv.second->used, kv.first});
        std::sort(c.begin(), c.end());
        for (auto& e : c) {
            if (total_ <= kMapCacheBytes * 9 / 10) break;
            auto it = files_.find(e.second);
            total_ -= std::min(total_, it->second->bytes);
            release(*it->second);
            files_.erase(it);
        }
    }
};

inline MapStore& mapStore() { static MapStore s; return s; }

}  // namespace hh
