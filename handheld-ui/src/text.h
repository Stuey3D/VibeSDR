// text.h — text for vibe-hhui through ONE GLYPH ATLAS: SDL_ttf renders each glyph (font, code point) once into a
// 1024×1024 texture, and a string is laid out from those glyphs (advance + kerning) as textured quads.
//
// ★★★ WHY AN ATLAS (handheld14 log, 2026-11-07): with a texture per string every string was its own batch — a hundred
//     glBufferData + draws a frame — and on Panfrost that cost 20–40 ms of the UI thread's CPU per frame (80–95 % of a
//     core; 0.5 ms on the Mac's driver). Now text and shapes share one texture (solids use uv −1: no sample, see
//     gfx.h), so the whole UI is a single batch a frame.
// ★ White glyphs, tinted by the quad colour — one glyph serves every colour. The atlas is never evicted mid-frame:
//   if it ever fills (it holds every face and size the UI uses many times over), it is cleared at the next frame start.
#pragma once
#include "gfx.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hh {

class Font {
public:
    bool load(const std::string& path, int px) {
        f_ = TTF_OpenFont(path.c_str(), px);
        if (!f_) { std::fprintf(stderr, "vibe-hhui: font %s: %s\n", path.c_str(), TTF_GetError()); return false; }
        px_ = px;
        return true;
    }
    TTF_Font* get() const { return f_; }
    int px() const { return px_; }
    int lineHeight() const { return f_ ? TTF_FontHeight(f_) : px_; }
private:
    TTF_Font* f_ = nullptr;
    int px_ = 0;
};

class TextCache {
public:
    /** Draw `s` with its top-left at (x, y). align: 0 left, 1 centre, 2 right of x. Returns the width drawn. */
    int draw(Gfx& g, const Font& f, const std::string& s, float x, float y, Rgba c, int align = 0) {
        if (s.empty() || !f.get()) return 0;
        const int w = width(f, s);
        float pen = std::round(align == 1 ? x - w / 2.f : align == 2 ? x - w : x);
        const float top = std::round(y);
        Uint32 prev = 0;
        for (Uint32 cp : decode(s)) {
            if (prev) pen += kern(f, prev, cp);
            const Glyph& gl = glyph(f, cp);
            if (gl.w > 0) g.imageUV(atlas_, pen, top, (float)gl.w, (float)gl.h, gl.u0, gl.v0, gl.u1, gl.v1, c);
            pen += gl.adv;
            prev = cp;
        }
        return w;
    }
    int width(const Font& f, const std::string& s) {
        if (s.empty() || !f.get()) return 0;
        int w = 0;
        Uint32 prev = 0;
        for (Uint32 cp : decode(s)) { if (prev) w += kern(f, prev, cp); w += glyph(f, cp).adv; prev = cp; }
        return w;
    }
    /** Like draw(), but digits sit in cells of one width (the font's widest digit) — a readout that changes never
     *  moves sideways (the web's `font-variant-numeric: tabular-nums`). Returns the width drawn. */
    int drawTab(Gfx& g, const Font& f, const std::string& s, float x, float y, Rgba c, int align = 0) {
        if (s.empty() || !f.get()) return 0;
        const int w = widthTab(f, s);
        float cx = std::round(align == 1 ? x - w / 2.f : align == 2 ? x - w : x);
        const float top = std::round(y);
        const int dw = digitW(f);
        for (Uint32 cp : decode(s)) {
            const Glyph& gl = glyph(f, cp);
            if (cp >= '0' && cp <= '9') {
                if (gl.w > 0) g.imageUV(atlas_, std::round(cx + (dw - gl.adv) / 2.f), top, (float)gl.w, (float)gl.h, gl.u0, gl.v0, gl.u1, gl.v1, c);
                cx += dw;
            } else {
                if (gl.w > 0) g.imageUV(atlas_, cx, top, (float)gl.w, (float)gl.h, gl.u0, gl.v0, gl.u1, gl.v1, c);
                cx += gl.adv;
            }
        }
        return w;
    }
    int widthTab(const Font& f, const std::string& s) {
        int w = 0;
        const int dw = digitW(f);
        for (Uint32 cp : decode(s)) w += (cp >= '0' && cp <= '9') ? dw : glyph(f, cp).adv;
        return w;
    }
    /** Word-wrap `s` to `maxW` pixels (cached). Explicit '\n' breaks a line. */
    const std::vector<std::string>& wrap(const Font& f, const std::string& s, int maxW) {
        auto key = std::make_tuple((const void*)f.get(), s, maxW);
        auto it = wraps_.find(key);
        if (it != wraps_.end()) return it->second;
        std::vector<std::string> out;
        std::string line, word;
        auto flushWord = [&] {
            if (word.empty()) return;
            const std::string tryLine = line.empty() ? word : line + " " + word;
            if (!line.empty() && width(f, tryLine) > maxW) { out.push_back(line); line = word; }
            else line = tryLine;
            word.clear();
        };
        for (size_t i = 0; i <= s.size(); i++) {
            const char ch = i < s.size() ? s[i] : '\n';
            if (ch == ' ') flushWord();
            else if (ch == '\n') { flushWord(); out.push_back(line); line.clear(); }
            else word += ch;
        }
        if (wraps_.size() > 400) wraps_.clear();
        return wraps_[key] = out;
    }
    /** Once a frame, before anything is drawn: a full atlas is cleared here (never mid-frame). */
    void tick(uint64_t frame) {
        frame_ = frame;
        if (full_) { glyphs_.clear(); penX_ = 1; penY_ = 1; rowH_ = 0; full_ = false; ++clears_; }
    }
    int glyphsMade() const { return made_; }
    int atlasClears() const { return clears_; }

private:
    struct Glyph { float u0 = 0, v0 = 0, u1 = 0, v1 = 0; int w = 0, h = 0, adv = 0; };
    static constexpr int AW = 1024, AH = 1024;
    GLuint atlas_ = 0;
    int penX_ = 1, penY_ = 1, rowH_ = 0, made_ = 0, clears_ = 0;
    bool full_ = false;
    std::map<std::pair<const void*, Uint32>, Glyph> glyphs_;
    std::map<std::tuple<const void*, Uint32, Uint32>, int> kerns_;
    std::map<std::tuple<const void*, std::string, int>, std::vector<std::string>> wraps_;
    std::map<const void*, int> digitW_;
    uint64_t frame_ = 0;

    static std::vector<Uint32> decode(const std::string& s) {
        std::vector<Uint32> out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = (unsigned char)s[i];
            Uint32 cp = c; size_t n = 1;
            if (c >= 0xF0 && i + 3 < s.size()) { cp = ((c & 7u) << 18) | ((s[i + 1] & 63u) << 12) | ((s[i + 2] & 63u) << 6) | (s[i + 3] & 63u); n = 4; }
            else if (c >= 0xE0 && i + 2 < s.size()) { cp = ((c & 15u) << 12) | ((s[i + 1] & 63u) << 6) | (s[i + 2] & 63u); n = 3; }
            else if (c >= 0xC0 && i + 1 < s.size()) { cp = ((c & 31u) << 6) | (s[i + 1] & 63u); n = 2; }
            out.push_back(cp);
            i += n;
        }
        return out;
    }
    int kern(const Font& f, Uint32 a, Uint32 b) {
        auto key = std::make_tuple((const void*)f.get(), a, b);
        auto it = kerns_.find(key);
        if (it != kerns_.end()) return it->second;
        int k = TTF_GetFontKerningSizeGlyphs32(f.get(), a, b);
        if (kerns_.size() > 20000) kerns_.clear();
        return kerns_[key] = k;
    }
    int digitW(const Font& f) {
        auto it = digitW_.find(f.get());
        if (it != digitW_.end()) return it->second;
        int m = 0;
        for (Uint32 d = '0'; d <= '9'; d++) m = std::max(m, glyph(f, d).adv);
        return digitW_[f.get()] = m;
    }
    void ensureAtlas() {
        if (atlas_) return;
        glGenTextures(1, &atlas_);
        glBindTexture(GL_TEXTURE_2D, atlas_);
        std::vector<uint8_t> zero((size_t)AW * AH * 4, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, AW, AH, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    const Glyph& glyph(const Font& f, Uint32 cp) {
        auto key = std::make_pair((const void*)f.get(), cp);
        auto it = glyphs_.find(key);
        if (it != glyphs_.end()) return it->second;
        ensureAtlas();
        Glyph gl;
        int minx = 0, maxx = 0, miny = 0, maxy = 0, adv = 0;
        if (TTF_GlyphMetrics32(f.get(), cp, &minx, &maxx, &miny, &maxy, &adv) == 0) gl.adv = adv;
        SDL_Surface* src = (cp > ' ') ? TTF_RenderGlyph32_Blended(f.get(), cp, SDL_Color{255, 255, 255, 255}) : nullptr;
        if (src) {
            SDL_Surface* rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0);   // ★ bytes R,G,B,A in memory
            SDL_FreeSurface(src);
            if (rgba) {
                const int w = rgba->w, h = rgba->h;
                if (penX_ + w + 1 > AW) { penX_ = 1; penY_ += rowH_ + 1; rowH_ = 0; }
                if (penY_ + h + 1 > AH) {
                    // ★ full: draw nothing for this glyph now, clear at the next frame start (tick) and re-make then
                    full_ = true;
                    SDL_FreeSurface(rgba);
                    static Glyph none; none = gl; return none;
                }
                glBindTexture(GL_TEXTURE_2D, atlas_);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
                glTexSubImage2D(GL_TEXTURE_2D, 0, penX_, penY_, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                gl.w = w; gl.h = h;
                gl.u0 = (float)penX_ / AW; gl.v0 = (float)penY_ / AH;
                gl.u1 = (float)(penX_ + w) / AW; gl.v1 = (float)(penY_ + h) / AH;
                penX_ += w + 1;
                rowH_ = std::max(rowH_, h);
                made_++;
                SDL_FreeSurface(rgba);
            }
        }
        return glyphs_[key] = gl;
    }
};

}  // namespace hh
