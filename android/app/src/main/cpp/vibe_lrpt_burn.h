// vibe_lrpt_burn.h — a METEOR picture WITH its map outline burnt in, made on request (V12, 2026-11-24).
//
// ★★ WHAT FOR. The gallery keeps the plain picture (the PNG is never altered) and the outline as vectors beside it
//    (vibe_lrpt_overlay.h). A viewer with the MAP OUTLINE toggle on who presses DOWNLOAD / OPEN / SAVE gets what is on
//    their screen: /vibeserver/gallery/file/<picture>.png?outline=1 — this decodes the saved PNG, draws the outline's
//    lines and grid labels into it and encodes a new PNG, there and then. Nothing is stored. Done on the SERVER so the
//    web page, the app (iOS and Android: OPEN hands a URL to the phone, which cannot share a file it made itself
//    without another library) and anything else get the same picture from one implementation.
// ★ BOUNDED. Only our own PNGs are read (8-bit grey or RGB, not interlaced — what vibe_png.h writes; every filter type
//   is handled anyway) up to kMaxPixels; a whole pass at the cap (1568 × 6400 RGB) is 30 MB while it is drawn, a
//   typical pass (1568 × 900) 4 MB. One request at a time per connection thread, nothing kept.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vibe_inflate.h"
#include "vibe_png.h"

namespace vibelrpt {
namespace burn {

constexpr size_t kMaxPixels = size_t(1568) * 6400;

inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

/** A PNG (8-bit grey or RGB, not interlaced) → RGB pixels. "" on success, else what is wrong. */
inline std::string decodeRgb(const std::string& png, int& w, int& h, std::vector<uint8_t>& rgb) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(png.data());
    const size_t n = png.size();
    if (n < 33 || std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) != 0) return "not a PNG";
    size_t o = 8; int ct = -1; std::string idat;
    while (o + 12 <= n) {
        const uint32_t len = be32(p + o); const char* type = reinterpret_cast<const char*>(p + o + 4);
        if (o + 12 + len > n) return "truncated";
        const uint8_t* d = p + o + 8;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len < 13) return "bad header";
            w = int(be32(d)); h = int(be32(d + 4));
            if (d[8] != 8 || (d[9] != 0 && d[9] != 2) || d[12] != 0) return "not an 8-bit grey or RGB picture";
            ct = d[9];
        } else if (!std::memcmp(type, "IDAT", 4)) idat.append(reinterpret_cast<const char*>(d), len);
        else if (!std::memcmp(type, "IEND", 4)) break;
        o += 12 + len;
    }
    if (ct < 0 || w <= 0 || h <= 0 || size_t(w) * size_t(h) > kMaxPixels) return "too large or no header";
    if (idat.size() < 6) return "no picture data";
    const int ch = ct == 2 ? 3 : 1;
    const size_t row = size_t(w) * ch;
    std::vector<uint8_t> raw; raw.reserve((row + 1) * size_t(h));
    const size_t want = (row + 1) * size_t(h);
    vibeinflate::Inflater inf;
    const vibeinflate::Sink sink = [&](const uint8_t* b, size_t k) { if (raw.size() + k > want) k = want - raw.size(); raw.insert(raw.end(), b, b + k); return raw.size() < want; };
    const std::string e = inf.run(reinterpret_cast<const uint8_t*>(idat.data()) + 2, idat.size() - 2, sink);
    if (!e.empty()) return e;
    if (raw.size() < want) return "short picture data";
    std::vector<uint8_t> cur(row), prev(row, 0);
    rgb.assign(size_t(w) * size_t(h) * 3, 0);
    for (int y = 0; y < h; y++) {
        const uint8_t f = raw[size_t(y) * (row + 1)];
        const uint8_t* s = &raw[size_t(y) * (row + 1) + 1];
        for (size_t i = 0; i < row; i++) {
            const int a = i >= size_t(ch) ? cur[i - ch] : 0, b = prev[i], c = i >= size_t(ch) ? prev[i - ch] : 0;
            int v = s[i];
            if (f == 1) v += a; else if (f == 2) v += b; else if (f == 3) v += (a + b) / 2;
            else if (f == 4) { const int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
            else if (f != 0) return "bad row filter";
            cur[i] = uint8_t(v);
        }
        uint8_t* d = &rgb[size_t(y) * size_t(w) * 3];
        if (ch == 3) std::memcpy(d, cur.data(), row);
        else for (int x = 0; x < w; x++) d[x * 3] = d[x * 3 + 1] = d[x * 3 + 2] = cur[size_t(x)];
        std::swap(cur, prev);
    }
    return {};
}

/** The few glyphs a grid label needs (0-9, N S E W, °), 5 × 7. */
inline const uint8_t* glyph(char c) {
    static const uint8_t d[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}};
    static const uint8_t N[7] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
                         E[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, W[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
                         DEG[7] = {0x0C,0x12,0x12,0x0C,0x00,0x00,0x00}, SP[7] = {0, 0, 0, 0, 0, 0, 0};
    if (c >= '0' && c <= '9') return d[c - '0'];
    return c == 'N' ? N : c == 'S' ? S : c == 'E' ? E : c == 'W' ? W : c == 'o' ? DEG : SP;
}

/** Draw the outline JSON (vibe_lrpt_overlay.h's) into RGB pixels: coast and lakes yellow, borders orange, the grid pale
 *  blue with its labels — the clients' colours. */
inline void draw(std::vector<uint8_t>& rgb, int w, int h, const std::string& js) {
    auto dot = [&](int x, int y, const uint8_t* c) { if (x >= 0 && y >= 0 && x < w && y < h) { uint8_t* p = &rgb[(size_t(y) * w + x) * 3]; p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; } };
    auto line = [&](int x0, int y0, int x1, int y1, const uint8_t* c) {
        if (std::abs(x1 - x0) + std::abs(y1 - y0) > 4 * (w + h)) return;
        int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
        for (;;) {
            dot(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * e;
            if (e2 >= dy) { e += dy; x0 += sx; }
            if (e2 <= dx) { e += dx; y0 += sy; }
        }
    };
    static const uint8_t coast[3] = { 255, 214, 0 }, border[3] = { 255, 140, 60 }, grid[3] = { 150, 205, 255 }, shade[3] = { 0, 0, 0 };
    struct L { const char* key; const uint8_t* c; } layers[] = { { "\"grid\":[", grid }, { "\"border\":[", border }, { "\"lake\":[", coast }, { "\"coast\":[", coast } };
    for (const auto& ly : layers) {
        size_t p = js.find(ly.key);
        if (p == std::string::npos) continue;
        p += std::strlen(ly.key);
        while (p < js.size() && js[p] == '[') {
            const size_t e = js.find(']', p);
            if (e == std::string::npos) return;
            std::vector<int> v; const char* q = js.c_str() + p + 1; const char* end = js.c_str() + e;
            while (q < end) { char* nx; const long x = std::strtol(q, &nx, 10); if (nx == q) break; v.push_back(int(x)); q = nx; if (*q == ',') q++; }
            for (size_t i = 2; i + 1 < v.size(); i += 2) line(v[i - 2], v[i - 1], v[i], v[i + 1], ly.c);
            p = e + 1;
            if (p < js.size() && js[p] == ',') p++;
        }
    }
    // the grid labels: [x,y,"55°N"], drawn at twice size with a dark edge so they read on cloud and on sea
    size_t p = js.find("\"labels\":[");
    if (p == std::string::npos) return;
    p += 10;
    while (p < js.size() && js[p] == '[') {
        const size_t e = js.find(']', p);
        if (e == std::string::npos) return;
        const std::string item = js.substr(p + 1, e - p - 1);
        char* nx; const long x = std::strtol(item.c_str(), &nx, 10); long y = 0;
        if (*nx == ',') y = std::strtol(nx + 1, &nx, 10);
        const size_t q1 = item.find('"'), q2 = item.rfind('"');
        std::string t;
        if (q1 != std::string::npos && q2 > q1) for (size_t i = q1 + 1; i < q2; i++) {
            const unsigned char c = (unsigned char)item[i];
            if (c == 0xC2 && i + 1 < q2 && (unsigned char)item[i + 1] == 0xB0) { t += 'o'; i++; } else t += char(c);
        }
        const int s = 2, tx = int(x) + 4, ty = int(y) + 4;
        for (int pass = 0; pass < 2; pass++) {
            int cx = tx;
            for (char c : t) {
                const uint8_t* g = glyph(c);
                for (int r = 0; r < 7; r++) for (int k = 0; k < 5; k++) if (g[r] & (0x10 >> k))
                    for (int a = 0; a < s; a++) for (int b = 0; b < s; b++) {
                        if (pass == 0) { for (int ox = -1; ox <= 1; ox++) for (int oy = -1; oy <= 1; oy++) dot(cx + k * s + a + ox, ty + r * s + b + oy, shade); }
                        else dot(cx + k * s + a, ty + r * s + b, grid);
                    }
                cx += 6 * s;
            }
        }
        p = e + 1;
        if (p < js.size() && js[p] == ',') p++;
    }
}

/** The saved PNG + its outline → a new PNG with the outline burnt in. Empty + err when it cannot be made. */
inline std::string withOutline(const std::string& png, const std::string& outlineJson, std::string& err) {
    int w = 0, h = 0; std::vector<uint8_t> rgb;
    err = decodeRgb(png, w, h, rgb);
    if (!err.empty()) return {};
    draw(rgb, w, h, outlineJson);
    std::string out = vibe::png::encode(w, h, 3, rgb.data());
    if (out.empty()) err = "could not encode";
    return out;
}

} // namespace burn
} // namespace vibelrpt
