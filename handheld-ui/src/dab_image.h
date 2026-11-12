// dab_image.h — the DAB pictures on the R36S: the MOT slideshow (TS 101 499, GET /vibeserver/dabslide) and the station
// logos the multiplex sent (GET /vibeserver/dablogoair?sid=, else the kept slide /vibeserver/dabslide?sid=), decoded
// with SDL2_image and drawn as GL textures.
//
// ★★ THREADS: ONE worker fetches and decodes (HTTP off the UI thread, as hw_state.h); the UI thread only uploads the
//    finished pixels (GL lives there) — dabImagesTick, called from dabTick.
// ★★★ NO OOM, EVER (memory feedback_memory_floor_256mb): every size is bounded before it is spent —
//     · a body over 1 MB is refused (MOT slides are ~50 kB; the engine's own ceiling is far below this);
//     · the image's DIMENSIONS are read from its header BEFORE decoding, and anything over 2048 × 2048 is refused (so a
//       crafted 30 000 × 30 000 PNG never asks for 3.6 GB);
//     · the decode is then scaled DOWN at once — the slide to fit 640 × 480 (the full-screen view; a 320 × 240 MOT slide
//       stays as sent), a logo to 96 × 96 — so what is kept is ≤ 1.2 MB for the slide plus ≤ 24 logos × 36 kB;
//     · the request queue is bounded (one slide, ≤ 32 logos) and the logo cache is an LRU of 24 textures.
// ★★ ONLY JPEG AND PNG, chosen by their magic bytes (IMG_LoadJPG_RW / IMG_LoadPNG_RW), never IMG_Load's sniff-everything:
//    these bytes come off the air from anybody with a transmitter.
// ★ With no live slide the slide area shows the playing service's KEPT picture (?sid=, the engine's slide store) — the
//   web's pane does the same (slide, else the air logo, else the kept one).
// ★ When a slide is NEW: the legacy report's slide.seq changes (the web's dabSlideQuery uses `h` on a dab=2 socket and
//   `seq` otherwise — this client reads the legacy report). A new playing service clears the old slide at once.
#pragma once
#include "gfx.h"
#include "http.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hh {

namespace dabimg {
constexpr size_t kMaxBytes = 1u << 20;
constexpr int kMaxDim = 2048;
constexpr int kSlideW = 640, kSlideH = 480, kLogo = 96;
constexpr size_t kLogoCache = 24, kLogoQueue = 32;

/** Width / height from a PNG's IHDR or a JPEG's SOFn, without decoding. false = not a JPEG / PNG we will touch. */
inline bool peekSize(const std::string& b, int& w, int& h, bool& png) {
    const unsigned char* d = (const unsigned char*)b.data();
    const size_t n = b.size();
    if (n >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' && std::memcmp(d + 12, "IHDR", 4) == 0) {
        w = (int)((d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19]);
        h = (int)((d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23]);
        png = true;
        return w > 0 && h > 0;
    }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) { i++; continue; }
            const unsigned char m = d[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
            const size_t len = (size_t)((d[i + 2] << 8) | d[i + 3]);
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = (d[i + 5] << 8) | d[i + 6];
                w = (d[i + 7] << 8) | d[i + 8];
                png = false;
                return w > 0 && h > 0;
            }
            if (len < 2) return false;
            i += 2 + len;
        }
    }
    return false;
}

/** Decode `bytes` and scale to fit maxW × maxH (never up). RGBA8 rows, tightly packed. */
inline bool decode(const std::string& bytes, int maxW, int maxH, std::vector<uint8_t>& px, int& ow, int& oh) {
    int w = 0, h = 0; bool png = false;
    if (bytes.empty() || bytes.size() > kMaxBytes || !peekSize(bytes, w, h, png)) return false;
    if (w > kMaxDim || h > kMaxDim) return false;
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
    if (!rw) return false;
    SDL_Surface* src = png ? IMG_LoadPNG_RW(rw) : IMG_LoadJPG_RW(rw);
    SDL_RWclose(rw);
    if (!src) return false;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0);   // ★ bytes R,G,B,A in memory
    SDL_FreeSurface(src);
    if (!rgba) return false;
    const double k = std::min(1.0, std::min((double)maxW / rgba->w, (double)maxH / rgba->h));
    ow = std::max(1, (int)(rgba->w * k + 0.5)); oh = std::max(1, (int)(rgba->h * k + 0.5));
    SDL_Surface* out = rgba;
    if (ow != rgba->w || oh != rgba->h) {
        out = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ABGR8888);
        if (!out || SDL_SoftStretchLinear(rgba, nullptr, out, nullptr) != 0) {
            if (out) SDL_FreeSurface(out);
            out = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ABGR8888);
            if (!out || SDL_BlitScaled(rgba, nullptr, out, nullptr) != 0) { if (out) SDL_FreeSurface(out); SDL_FreeSurface(rgba); return false; }
        }
        SDL_FreeSurface(rgba);
    }
    px.resize((size_t)ow * oh * 4);
    for (int y = 0; y < oh; y++) std::memcpy(&px[(size_t)y * ow * 4], (const uint8_t*)out->pixels + (size_t)y * out->pitch, (size_t)ow * 4);
    SDL_FreeSurface(out);
    return true;
}
}  // namespace dabimg

struct DabTex { GLuint tex = 0; int w = 0, h = 0; };

/** The pictures: the worker's queue, the finished pixels, the textures. */
class DabImages {
public:
    /** The slide area's picture: the live slide (key = its seq, path /vibeserver/dabslide) or, while the playing service is
     *  sending none, the one it sent before and the engine kept (path ?sid=). Ask once per key. */
    void wantSlide(int port, uint64_t seq, const std::string& path) {
        std::lock_guard<std::mutex> lk(m_);
        port_ = port;
        slideReq_ = seq; slidePath_ = path;
        haveSlideReq_ = true;
        cv_.notify_one();
        start();
    }
    /** A station's logo: `path` is where to get it ("" = none known); asked again only when the path changes, or a
     *  minute after a miss. */
    void wantLogo(int port, uint32_t sid, const std::string& path, double now) {
        auto it = logos_.find(sid);
        if (it != logos_.end()) {
            Logo& l = it->second;
            if (l.path == path && (l.tex.tex || l.pending || now - l.missAt < 60)) return;
            if (path.empty()) return;
        } else if (path.empty()) return;
        Logo& l = logos_[sid];
        l.path = path; l.pending = true; l.missAt = 0;
        std::lock_guard<std::mutex> lk(m_);
        port_ = port;
        for (auto& q : logoQ_) if (q.first == sid) { q.second = path; return; }
        if (logoQ_.size() >= dabimg::kLogoQueue) { logos_[logoQ_.front().first].pending = false; logoQ_.pop_front(); }
        logoQ_.push_back({sid, path});
        cv_.notify_one();
        start();
    }
    /** UI thread, once a frame: upload what the worker finished; keep the logo LRU in bounds. */
    void tick(double now) {
        std::vector<Done> got;
        { std::lock_guard<std::mutex> lk(m_); got.swap(done_); }
        for (auto& d : got) {
            if (d.slide) {
                if (d.seq != slideSeqWanted_) continue;   // ★ a newer slide was asked for meanwhile
                if (!d.ok) continue;                       // keep the old picture rather than a blank (the next seq retries)
                upload(slide_, d);
                slideSeqShown_ = d.seq;
            } else {
                auto it = logos_.find(d.sid);
                if (it == logos_.end() || it->second.path != d.path) continue;
                Logo& l = it->second;
                l.pending = false;
                if (!d.ok) { l.missAt = now; continue; }
                upload(l.tex, d);
                l.used = now;
            }
        }
        // the LRU: the least recently drawn logo goes once there are more than the cap
        while (countTex() > dabimg::kLogoCache) {
            auto old = logos_.end();
            for (auto it = logos_.begin(); it != logos_.end(); ++it)
                if (it->second.tex.tex && (old == logos_.end() || it->second.used < old->second.used)) old = it;
            if (old == logos_.end()) break;
            glDeleteTextures(1, &old->second.tex.tex);
            logos_.erase(old);
        }
    }
    /** The slide for `seq` if it has arrived (else the last one, until clearSlide). */
    const DabTex* slide() const { return slide_.tex ? &slide_ : nullptr; }
    uint64_t slideSeqShown() const { return slideSeqShown_; }
    void setSlideWanted(uint64_t seq) { slideSeqWanted_ = seq; }
    void clearSlide() { if (slide_.tex) glDeleteTextures(1, &slide_.tex); slide_ = {}; slideSeqShown_ = 0; slideSeqWanted_ = 0; }
    const DabTex* logo(uint32_t sid, double now) {
        auto it = logos_.find(sid);
        if (it == logos_.end() || !it->second.tex.tex) return nullptr;
        it->second.used = now;
        return &it->second.tex;
    }

private:
    struct Logo { std::string path; DabTex tex; bool pending = false; double missAt = 0, used = 0; };
    struct Done { bool slide = false, ok = false; uint64_t seq = 0; uint32_t sid = 0; std::string path; std::vector<uint8_t> px; int w = 0, h = 0; };
    std::mutex m_;
    std::condition_variable cv_;
    bool started_ = false;
    int port_ = 48000;
    bool haveSlideReq_ = false; uint64_t slideReq_ = 0; std::string slidePath_;
    std::deque<std::pair<uint32_t, std::string>> logoQ_;
    std::vector<Done> done_;
    // UI thread only
    std::map<uint32_t, Logo> logos_;
    DabTex slide_;
    uint64_t slideSeqShown_ = 0, slideSeqWanted_ = 0;

    size_t countTex() const { size_t n = 0; for (auto& l : logos_) if (l.second.tex.tex) n++; return n; }
    static void upload(DabTex& t, const Done& d) {
        if (!t.tex) glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d.w, d.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.w = d.w; t.h = d.h;
    }
    void start() {   // caller holds m_
        if (started_) return;
        started_ = true;
        std::thread([this] { worker(); }).detach();   // ★ lives for the process; it sleeps on the queue
    }
    void worker() {
        IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
        for (;;) {
            Done d;
            int port;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return haveSlideReq_ || !logoQ_.empty(); });
                port = port_;
                if (haveSlideReq_) { d.slide = true; d.seq = slideReq_; haveSlideReq_ = false; d.path = slidePath_; }
                else { d.sid = logoQ_.front().first; d.path = logoQ_.front().second; logoQ_.pop_front(); }
            }
            const HttpResult r = httpRequest(port, "GET", d.path, "", 5);
            if (r.status == 200)
                d.ok = d.slide ? dabimg::decode(r.body, dabimg::kSlideW, dabimg::kSlideH, d.px, d.w, d.h)
                               : dabimg::decode(r.body, dabimg::kLogo, dabimg::kLogo, d.px, d.w, d.h);
            std::lock_guard<std::mutex> lk(m_);
            done_.push_back(std::move(d));
        }
    }
};
inline DabImages& dabImages() { static DabImages* i = new DabImages; return *i; }   // ★ never destroyed: its worker outlives main

/** Draw `t` fitted inside `r` (aspect kept, centred). */
inline void dabDrawTex(Gfx& g, const DabTex& t, float x, float y, float w, float h, float alpha = 1.f) {
    if (!t.tex || t.w <= 0 || t.h <= 0) return;
    const float k = std::min(w / t.w, h / t.h);
    const float dw = t.w * k, dh = t.h * k;
    g.image(t.tex, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, {1, 1, 1, alpha});
}

}  // namespace hh
