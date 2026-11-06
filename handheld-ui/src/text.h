// text.h — text for vibe-hhui: SDL_ttf renders a string once into a GL texture, cached by (font, text); the cache
// keeps what was drawn in the last few seconds and frees the rest, so a ticking clock or a moving frequency never
// grows memory. ★ White glyphs, tinted by the quad colour — one texture serves every colour.
#pragma once
#include "gfx.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <map>
#include <string>
#include <utility>

namespace hh {

struct TextTex { GLuint tex = 0; int w = 0, h = 0; uint64_t lastUse = 0; };

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
        TextTex& t = get(f, s);
        if (!t.tex) return 0;
        const float ox = align == 1 ? x - t.w / 2.f : align == 2 ? x - t.w : x;
        g.image(t.tex, ox, y, (float)t.w, (float)t.h, c);
        return t.w;
    }
    int width(const Font& f, const std::string& s) {
        if (s.empty() || !f.get()) return 0;
        int w = 0, h = 0;
        TTF_SizeUTF8(f.get(), s.c_str(), &w, &h);
        return w;
    }
    /** Once a frame: forget what has not been drawn for ~5 s. */
    void tick(uint64_t frame) {
        frame_ = frame;
        if (frame % 60) return;
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (frame - it->second.lastUse > 150) { glDeleteTextures(1, &it->second.tex); it = cache_.erase(it); }
            else ++it;
        }
    }
private:
    std::map<std::pair<const void*, std::string>, TextTex> cache_;
    uint64_t frame_ = 0;

    TextTex& get(const Font& f, const std::string& s) {
        auto key = std::make_pair((const void*)f.get(), s);
        auto it = cache_.find(key);
        if (it != cache_.end()) { it->second.lastUse = frame_; return it->second; }
        TextTex t;
        t.lastUse = frame_;
        SDL_Surface* src = TTF_RenderUTF8_Blended(f.get(), s.c_str(), SDL_Color{255, 255, 255, 255});
        if (src) {
            SDL_Surface* rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0);   // ★ bytes R,G,B,A in memory
            SDL_FreeSurface(src);
            if (rgba) {
                glGenTextures(1, &t.tex);
                glBindTexture(GL_TEXTURE_2D, t.tex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                t.w = rgba->w; t.h = rgba->h;
                SDL_FreeSurface(rgba);
            }
        }
        return cache_[key] = t;
    }
};

}  // namespace hh
