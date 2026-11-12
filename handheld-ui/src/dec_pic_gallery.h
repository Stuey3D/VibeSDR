// dec_pic_gallery.h — THE GALLERY on the R36S: the pictures the engine kept (web/client/src/main.ts renderGallery /
// openGalleryItem; the engine's vibe_gallery.h — on the handheld the items are the card's Gallery/<DECODER>/… files,
// "2026-11-07 1432 - 4608.100 kHz WEFAX.png"). The engine writes a WEFAX chart or an SSTV picture there when it
// COMPLETES (vibe_bgdecode.h GalleryImageTap); this lists them, newest first, and shows one full screen.
//
// ★★ A SHEET, full screen (drawn from decDrawFull while the decoder box holds the pad): a grid of thumbnails, 4 × 3 a
//    page (GET /vibeserver/gallery.json?kind=…&limit=12&offset=…), WEFAX | SSTV, the pager under it. D-pad = the
//    highlight, ENTER = the picture full screen (ENTER again zooms 1 → 1.5 → 2 → 3 → 4 ×, the D-pad pans; LEFT / RIGHT
//    at FIT step through the page), DELETE under it — ★ a SECOND press within 4 s deletes (the first says so).
// ★★★ NO OOM, EVER: a body over 4 MB is refused (http.h stops there); the image's size is read from its header BEFORE it
//     is decoded and anything over 2048 × 4096 is refused; a thumbnail is decoded, shrunk to 144 × 96 at once and only
//     that is kept (an LRU of 36 ≈ 2 MB of GPU); the open picture is kept as grey (1 byte a pixel) when it is grey —
//     a chart is — in the picture tiles (dec_pic.h), never as RGBA; one decode at a time, on one worker.
// ★★ ONLY PNG AND JPEG, by their magic bytes (dab_image.h's rule: IMG_LoadPNG_RW / IMG_LoadJPG_RW, never IMG_Load).
// ★ Delete is the supervisor's: DELETE /handheld/gallery/<FOLDER>/<file> (handheld.cpp). An engine without it answers
//   404 "no such handheld route" — then the key says so and nothing is pretended.
#pragma once
#include "dab_image.h"
#include "dec_pic.h"

namespace hh {

struct GalItem { std::string f, k, m, title, o, p; double t = 0, hz = 0, bytes = 0; bool partial = false; };
struct GalThumb { GLuint tex = 0; int w = 0, h = 0; int state = 0; double used = 0; };   // state 0 none · 1 asked · 2 ok · 3 failed

struct GalUi {
    bool open = false;
    int tab = 0;                     // 0 WEFAX · 1 SSTV
    int page = 0, pages = 1, total = 0;
    std::vector<GalItem> items;
    std::string note;                // "Nothing from this decoder yet." / an error, in words
    bool loading = false, enabledKnown = false, enabled = false, probing = false;
    uint64_t listSeq = 0;
    std::map<std::string, GalThumb> thumbs;
    // the open picture
    int viewing = -1;                // index in items, -1 = the grid
    std::unique_ptr<Pic> pic;        // its tiles
    std::string picFor; bool picLoading = false; std::string picErr;
    double armedAt = 0;              // DELETE pressed once
    bool deleting = false;
    std::string focusAfter;          // a grid cell to land on (BACK from a picture)
};
inline GalUi& galUi() { static GalUi g; return g; }
inline bool galIsOpen() { return galUi().open; }
inline const char* galKind(int tab) { return tab == 1 ? "sstv" : "wefax"; }
constexpr int kGalPer = 12, kGalThumbW = 144, kGalThumbH = 96, kGalThumbCache = 36;
constexpr size_t kGalMaxBytes = 4u << 20;
constexpr int kGalMaxW = 2048, kGalMaxH = 4096;

/** encodeURIComponent */
inline std::string galPct(const std::string& s) {
    std::string o; char b[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

// ── the worker: the list, thumbnails, the picture, delete ────────────────────────────────────────────────────────────
struct GalReq { int type = 0; std::string path, f; uint64_t seq = 0; };   // 1 list · 2 thumb · 3 picture · 4 delete · 5 probe
struct GalDone { int type = 0; uint64_t seq = 0; std::string f; int status = 0; std::string body; std::vector<uint8_t> px; int w = 0, h = 0, ch = 0; std::string err; };
class GalWorker {
public:
    void post(GalReq r, int port) {
        std::lock_guard<std::mutex> lk(m_);
        port_ = port;
        if (r.type == 1 || r.type == 3) for (auto it = q_.begin(); it != q_.end();) { if (it->type == r.type) it = q_.erase(it); else ++it; }   // ★ only the newest list / picture
        if (r.type == 2 && q_.size() > 40) return;
        q_.push_back(std::move(r));
        if (!started_) { started_ = true; std::thread([this] { run(); }).detach(); }
        cv_.notify_one();
    }
    void dropThumbs() { std::lock_guard<std::mutex> lk(m_); for (auto it = q_.begin(); it != q_.end();) { if (it->type == 2) it = q_.erase(it); else ++it; } }
    void take(std::vector<GalDone>& out) { std::lock_guard<std::mutex> lk(m_); out.swap(done_); }
private:
    std::mutex m_; std::condition_variable cv_;
    std::deque<GalReq> q_; std::vector<GalDone> done_;
    bool started_ = false; int port_ = 48000;
    /** Decode (PNG / JPEG only, its size checked first); `maxW/maxH` > 0 = shrink to fit at once (RGBA out). Else the
     *  whole picture: grey (1 channel) when every pixel is grey, else RGB. */
    static bool decode(const std::string& bytes, int maxW, int maxH, GalDone& d) {
        int w = 0, h = 0; bool png = false;
        if (bytes.empty() || bytes.size() >= kGalMaxBytes) { d.err = "too big for this receiver to show"; return false; }
        if (!dabimg::peekSize(bytes, w, h, png)) { d.err = "not a picture this receiver shows"; return false; }
        if (w > kGalMaxW || h > kGalMaxH) { d.err = "too big for this receiver to show (" + std::to_string(w) + " × " + std::to_string(h) + ")"; return false; }
        SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
        if (!rw) return false;
        SDL_Surface* src = png ? IMG_LoadPNG_RW(rw) : IMG_LoadJPG_RW(rw);
        SDL_RWclose(rw);
        if (!src) { d.err = "the picture could not be read"; return false; }
        SDL_Surface* rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0);
        SDL_FreeSurface(src);
        if (!rgba) return false;
        if (maxW > 0) {
            const double k = std::min(1.0, std::min((double)maxW / rgba->w, (double)maxH / rgba->h));
            const int ow = std::max(1, (int)(rgba->w * k + 0.5)), oh = std::max(1, (int)(rgba->h * k + 0.5));
            SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ABGR8888);
            if (!out || (SDL_SoftStretchLinear(rgba, nullptr, out, nullptr) != 0 && SDL_BlitScaled(rgba, nullptr, out, nullptr) != 0)) {
                if (out) SDL_FreeSurface(out);
                SDL_FreeSurface(rgba);
                return false;
            }
            SDL_FreeSurface(rgba);
            d.w = ow; d.h = oh; d.ch = 4; d.px.resize((size_t)ow * oh * 4);
            for (int y = 0; y < oh; y++) std::memcpy(&d.px[(size_t)y * ow * 4], (const uint8_t*)out->pixels + (size_t)y * out->pitch, (size_t)ow * 4);
            SDL_FreeSurface(out);
            return true;
        }
        const int W = rgba->w, H = rgba->h;
        bool grey = true;
        for (int y = 0; y < H && grey; y++) {
            const uint8_t* r = (const uint8_t*)rgba->pixels + (size_t)y * rgba->pitch;
            for (int x = 0; x < W; x++) if (r[x * 4] != r[x * 4 + 1] || r[x * 4] != r[x * 4 + 2]) { grey = false; break; }
        }
        d.w = W; d.h = H; d.ch = grey ? 1 : 3;
        d.px.resize((size_t)W * H * d.ch);
        for (int y = 0; y < H; y++) {
            const uint8_t* r = (const uint8_t*)rgba->pixels + (size_t)y * rgba->pitch;
            uint8_t* o = &d.px[(size_t)y * W * d.ch];
            for (int x = 0; x < W; x++) { if (grey) o[x] = r[x * 4]; else { o[x * 3] = r[x * 4]; o[x * 3 + 1] = r[x * 4 + 1]; o[x * 3 + 2] = r[x * 4 + 2]; } }
        }
        SDL_FreeSurface(rgba);
        return true;
    }
    void run() {
#if defined(__linux__)
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
        IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
        for (;;) {
            GalReq r; int port;
            { std::unique_lock<std::mutex> lk(m_); cv_.wait(lk, [this] { return !q_.empty(); }); r = std::move(q_.front()); q_.pop_front(); port = port_; }
            GalDone d; d.type = r.type; d.seq = r.seq; d.f = r.f;
            const HttpResult h = httpRequest(port, r.type == 4 ? "DELETE" : "GET", r.path, "", r.type == 3 || r.type == 2 ? 10 : 6);
            d.status = h.status;
            if (r.type == 2 || r.type == 3) {
                if (h.status == 200) { if (!decode(h.body, r.type == 2 ? kGalThumbW : 0, kGalThumbH, d)) d.status = 0; }
                else d.err = h.status == 404 ? "This picture is no longer on the card." : "The engine did not answer.";
            } else d.body = h.body.substr(0, 512 * 1024);
            std::lock_guard<std::mutex> lk(m_);
            done_.push_back(std::move(d));
        }
    }
};
inline GalWorker& galWorker() { static GalWorker* w = new GalWorker; return *w; }

/** Does this engine keep a gallery at all (asked once a session, off the UI thread)? Draws GALLERY only where it does. */
inline bool galEnabled(int port) {
    GalUi& g = galUi();
    if (!g.enabledKnown && !g.probing) { g.probing = true; GalReq r; r.type = 5; r.path = "/vibeserver/gallery.json?limit=1&offset=0"; galWorker().post(r, port); }
    return g.enabled;
}
inline void galList(int port) {
    GalUi& g = galUi();
    GalReq r; r.type = 1; r.seq = ++g.listSeq;
    r.path = std::string("/vibeserver/gallery.json?kind=") + galKind(g.tab) + "&limit=" + std::to_string(kGalPer) + "&offset=" + std::to_string(g.page * kGalPer);
    g.loading = true;
    galWorker().post(r, port);
}
inline void galOpen(App& a, int tab) {
    GalUi& g = galUi();
    g.open = true; g.tab = tab; g.page = 0; g.viewing = -1; g.items.clear(); g.note.clear(); g.armedAt = 0;
    a.nav_.forget("dec");
    galList(a.eng_->port());
}
inline void galClose(App& a) {
    GalUi& g = galUi();
    g.open = false; g.viewing = -1;
    if (g.pic) { picFree(*g.pic); g.pic.reset(); }
    galWorker().dropThumbs();
    a.nav_.forget("dec");
}
inline void galOpenItem(App& a, int idx) {
    GalUi& g = galUi();
    if (idx < 0 || idx >= (int)g.items.size()) return;
    g.viewing = idx; g.armedAt = 0;
    PicUi& u = picUi(); u.zoomI = 0; u.panX = 0.5f; u.panY = 0.f;
    if (g.picFor != g.items[(size_t)idx].f) {
        if (g.pic) { picFree(*g.pic); g.pic.reset(); }
        g.picFor = g.items[(size_t)idx].f; g.picLoading = true; g.picErr.clear();
        GalReq r; r.type = 3; r.f = g.picFor; r.path = "/vibeserver/gallery/file/" + galPct(g.picFor);
        galWorker().post(r, a.eng_->port());
    }
    a.nav_.forget("dec");
}

/** UI thread, once a loop (from decPicTick's neighbour, galTick): what the worker finished — the list, the pixels. */
inline void galTick(App& a) {
    GalUi& g = galUi();
    std::vector<GalDone> got;
    galWorker().take(got);
    const double now = nowSec();
    for (auto& d : got) {
        if (d.type == 5) {
            g.probing = false; g.enabledKnown = true;
            const Json j = Json::parse(d.body);
            g.enabled = d.status == 200 && !(j["enabled"].kind == Json::Bool && !j["enabled"].b);
        } else if (d.type == 1) {
            if (d.seq != g.listSeq) continue;
            g.loading = false;
            g.items.clear();
            const Json j = Json::parse(d.body);
            if (d.status != 200) { g.note = d.status == 403 ? "This gallery is private." : "The gallery did not load — press the page key to try again."; continue; }
            if (j["enabled"].kind == Json::Bool && !j["enabled"].b) { g.note = "This receiver keeps no gallery."; continue; }
            for (auto& x : j["items"].a) {
                if (g.items.size() >= kGalPer) break;
                GalItem it;
                it.f = x["f"].str().substr(0, 260); it.k = decClip(x["k"].str(), 16); it.m = decClip(x["m"].str(), 8);
                it.title = decClip(x["title"].str(), 120); it.o = decClip(x["o"].str(), 16); it.p = decClip(x["p"].str(), 40);
                it.t = x["t"].num(0); it.hz = x["hz"].num(0); it.bytes = x["bytes"].num(0); it.partial = x["partial"].boolean();
                if (it.f.empty() || it.f.size() < 5 || it.f.compare(it.f.size() - 4, 4, ".png") != 0) continue;   // ★ pictures only
                g.items.push_back(std::move(it));
            }
            g.total = (int)j["total"].num((double)g.items.size());
            g.pages = std::max(1, (g.total + kGalPer - 1) / kGalPer);
            if (g.page >= g.pages && g.page > 0) { g.page = g.pages - 1; galList(a.eng_->port()); continue; }   // ★ the gallery shrank under us
            g.note = g.items.empty() ? (g.tab == 1 ? "No SSTV pictures yet — each finished picture is kept here." : "No WEFAX charts yet — each finished chart is kept here.") : "";
        } else if (d.type == 2) {
            auto it = g.thumbs.find(d.f);
            if (it == g.thumbs.end()) continue;
            GalThumb& t = it->second;
            if (d.status != 200 || d.px.empty()) { t.state = 3; continue; }
            if (!t.tex) glGenTextures(1, &t.tex);
            glBindTexture(GL_TEXTURE_2D, t.tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d.w, d.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.px.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            t.w = d.w; t.h = d.h; t.state = 2; t.used = now;
        } else if (d.type == 3) {
            if (d.f != g.picFor) continue;
            g.picLoading = false;
            if (d.status != 200 || d.px.empty()) { g.picErr = d.err.empty() ? "The picture could not be shown." : d.err; continue; }
            g.pic.reset(new Pic);
            Pic& p = *g.pic;
            p.id = 0; p.kind = d.ch == 1 ? PK_WEFAX : PK_SSTV; p.w = d.w; p.h = d.h; p.maxY = d.h - 1; p.complete = true;
            if (d.ch == 4) {   // (never — the whole picture is grey or RGB) kept for safety
                std::vector<uint8_t> rgb((size_t)d.w * d.h * 3);
                for (size_t i = 0, n = (size_t)d.w * d.h; i < n; i++) { rgb[i * 3] = d.px[i * 4]; rgb[i * 3 + 1] = d.px[i * 4 + 1]; rgb[i * 3 + 2] = d.px[i * 4 + 2]; }
                d.px.swap(rgb);
            }
            picUpload(p, 0, d.h, d.px.data());
            p.mipAt = 0; picMips(p, now + 1);
        } else if (d.type == 4) {
            g.deleting = false;
            if (d.status == 200) {
                a.toast("Deleted");
                for (auto& [f, t] : g.thumbs) if (f == d.f && t.tex) { glDeleteTextures(1, &t.tex); t.tex = 0; }
                g.thumbs.erase(d.f);
                g.viewing = -1;
                if (g.pic) { picFree(*g.pic); g.pic.reset(); }
                g.picFor.clear();
                galList(a.eng_->port());
            } else if (d.status == 404 && d.body.find("no such handheld route") != std::string::npos) a.toastLong("This engine cannot delete from the gallery yet — the card's Gallery folder can be tidied on a computer.");
            else a.toastLong(d.status == 404 ? "Already gone from the card." : "Not deleted — the card did not answer.");
        }
    }
    // the thumbnail LRU
    size_t n = 0; for (auto& t : g.thumbs) if (t.second.tex) n++;
    while (n > (size_t)kGalThumbCache) {
        auto old = g.thumbs.end();
        for (auto it = g.thumbs.begin(); it != g.thumbs.end(); ++it) if (it->second.tex && (old == g.thumbs.end() || it->second.used < old->second.used)) old = it;
        if (old == g.thumbs.end()) break;
        glDeleteTextures(1, &old->second.tex); g.thumbs.erase(old); n--;
    }
}
inline GalThumb* galThumb(App& a, const std::string& f) {
    GalUi& g = galUi();
    GalThumb& t = g.thumbs[f];
    t.used = nowSec();
    if (t.state == 0) { t.state = 1; GalReq r; r.type = 2; r.f = f; r.path = "/vibeserver/gallery/file/" + galPct(f); galWorker().post(r, a.eng_->port()); }
    return &t;
}
inline std::string galWhen(double t) {
    const time_t tt = (time_t)t; struct tm g{}; gmtime_r(&tt, &g);
    char b[40]; std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d UTC", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min); return b;
}
inline std::string galFreq(double hz) { char b[32]; if (hz < 30e6) std::snprintf(b, sizeof b, "%.1f kHz", hz / 1e3); else std::snprintf(b, sizeof b, "%.4f MHz", hz / 1e6); return b; }
inline std::string galSize(double b) { char s[32]; if (b >= 1048576) std::snprintf(s, sizeof s, "%.1f MB", b / 1048576); else std::snprintf(s, sizeof s, "%.0f kB", b / 1024); return s; }
inline std::string galOrigin(const GalItem& it) {   // galleryOrigin.ts galleryOriginWords
    if (it.o == "both") return "background decoding and listeners";
    return it.o == "background" ? "background decoding" + (it.p.empty() ? std::string() : " (" + it.p + ")") : "your decoder";
}

}  // namespace hh
