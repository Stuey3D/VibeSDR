// dec_pic.h — THE PICTURE DECODERS on the R36S: WEFAX, SSTV and FELD HELL (web/client/src/main.ts startDecImage /
// drawDecLine / drawHellLine / clearDecImage / the WEFAX ADJ keys; decoders.ts _handleBinary for the frames). The
// state, the frames, the GL textures and the WEFAX worker; dec_pic_box.h draws them, dec_pic_gallery.h is the gallery.
//
// ★★★ THE DECODERS RUN IN THE ENGINE (vibe_decoder_host.h); this only draws what /ws/dxcluster sends (dec_state.h owns
//     the socket). Frames are read against the decoder ATTACHED — 0x01 is WEFAX's grey line, SSTV's RGB line and
//     HELL's strip row (never sniffed).
// ★★★ NO OOM, EVER — every picture is bounded before a byte is spent:
//     · a picture lives in GL TEXTURES, tiles of kPicTileRows rows, added as it grows: never an RGBA copy in RAM;
//     · WEFAX keeps its received lines (1 byte a pixel) for re-align / re-render — at most kWefaxMaxLines (4096 × 1809 =
//       7.4 MB; 34 min at 120 lpm), then the chart is banked and the next line starts a new one. The PREV chart keeps
//       its textures only: its lines are dropped once its final rendering lands;
//     · SSTV ≤ 1024 × 1024, HELL a page of 224 rows; widths ≤ 2048 (HELL 4096). Live + PREV ≈ 2 × 7.4 MB of GPU at worst.
// ★★ THREADS: the frames arrive on the UI thread (decTick drains the socket) and a line is uploaded at once (a few KB).
//    WEFAX's heavy work — the per-chart auto-align LOOK (wefaxAlign.ts findChartAlign, up to ~100 passes over the chart)
//    and a whole-chart RE-RENDER after a margin / slant change — runs on ONE low-priority worker (nice 10) on a snapshot
//    of the line pointers (lines are immutable once received, so the worker reads them without a lock). The UI never
//    waits for it: until a re-render lands the old rendering is shown slid by the new margin (the web's drag preview).
// ★★ WEFAX: PHASED ≠ ALIGNED (memory, Stuart 2026-10-06) — the auto-align judges the CONTENT (dec_pic_align.h, the
//    app's code), never the phasing state. Its manual fallback (Stuart's rule: AUTO decoders get manual controls) is
//    ADJ: MARGIN · SLANT · AUTO · RAW, per chart, the slant remembered per kHz ($HOME/.vibe-hhui-wefax.json).
// ★ SSTV 0x08 (a slant-corrected repaint) is NOT a new picture: its lines land in place (decoders.ts's audit note).
#pragma once
#include "dec_pic_align.h"
#include "dec_state.h"

#include <GLES3/gl3.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace hh {

enum PicKind { PK_WEFAX, PK_SSTV, PK_HELL };
constexpr int kPicTileRows = 512, kWefaxMaxLines = 4096, kSstvMaxDim = 1024, kPicMaxW = 2048, kHellMaxW = 4096;
constexpr int kHellStripRows = 28, kHellPageStrips = 8, kHellPageRows = kHellStripRows * kHellPageStrips;
inline const char* const kWefaxPhaseText[] = {"standing by", "start tone", "phasing", "receiving chart", "stop tone"};
inline const char* kHellHint = "FELD HELL (Hellschreiber): drawn, not read — the text is painted as it is heard, a strip at a time, and you read it. Every column is printed twice, one copy above the other, so a slant or a fade that cuts one copy leaves the other whole. Tune USB so the signal is in your passband (the tone is found by itself); each new page puts the last one under PREV.";

typedef std::shared_ptr<const std::vector<uint8_t>> PicRow;

struct PicTile { GLuint tex = 0; int y0 = 0; bool dirty = false; };
/** One picture — the live one or PREV. */
struct Pic {
    uint64_t id = 0;
    PicKind kind = PK_WEFAX;
    int w = 0, h = 0;                // h: the picture's expected height (SSTV's, HELL's page); WEFAX grows
    int maxY = -1;                   // the highest line drawn
    bool complete = false;
    std::vector<PicTile> tiles;
    double mipAt = 0;
    std::string mode;                // SSTV's mode name, for the title
    // ── WEFAX ──
    std::vector<PicRow> raw;         // the lines as received (nullptr = never came)
    wfx::Hist hist;
    wfx::AlignState st;              // this chart's own auto-align (chartAlignStep)
    bool lookPending = false;
    bool hasManual = false; double manualShift = 0;   // MARGIN set by hand on THIS chart
    bool rawMode = false;            // RAW: drawn exactly as received
    bool slantTouched = false;       // SLANT set by hand on THIS chart
    wfx::Align tex;                  // the geometry the texture's rows were rendered with
    uint64_t paintSeq = 0; bool paintPending = false; double paintDue = 0;
    bool dropRawAfterPaint = false;
};

/** The worker's jobs (WEFAX only): a look, or a whole-chart render. */
struct PicJob { int type = 0; uint64_t pic = 0, seq = 0; std::vector<PicRow> rows; int w = 0; wfx::LookReq q; double station = 0; wfx::Format fmt;
                wfx::Align al; wfx::Levels lv; };
struct PicDone { int type = 0; uint64_t pic = 0, seq = 0; wfx::LookReq q; wfx::LookRes res; int n = 0, w = 0; wfx::Align al; std::vector<uint8_t> px; };

// ── the rendering (any thread): the drawn row = the received one moved by the geometry, lost lines filled ──────────────
/** Line j as DRAWN under `a` — a lost line repeats the last received one above when the run is ≤ WEFAX_FILL_MAX
 *  (wefaxCrisp fillLostLines), else nullptr. `cache`: 8 rows (a render walks down, needing j−2 … j+2). */
struct PicAlCache { int y[8] = {-1, -1, -1, -1, -1, -1, -1, -1}; std::vector<uint8_t> px[8]; };
inline const uint8_t* picAlignedRow(const PicRow* rows, int n, int w, int j, const wfx::Align& a, PicAlCache& cc) {
    if (j < 0 || j >= n) return nullptr;
    int k = j;
    if (!rows[j]) {
        k = -1;
        for (int i = j - 1; i >= 0 && i >= j - wfx::WEFAX_FILL_MAX - 1; i--) if (rows[i]) { k = i; break; }
        if (k < 0) return nullptr;
        int e = -1;
        for (int i = j + 1; i < n && i <= k + wfx::WEFAX_FILL_MAX + 1; i++) if (rows[i]) { e = i; break; }
        if (e < 0 || e - k - 1 > wfx::WEFAX_FILL_MAX) return nullptr;
    }
    const int slot = k & 7;
    if (cc.y[slot] == k) return cc.px[slot].data();
    std::vector<uint8_t>& o = cc.px[slot];
    o.resize((size_t)w);
    const uint8_t* r = rows[k]->data();
    const int off = wfx::offset(a, k, w);
    if (!off) std::memcpy(o.data(), r, (size_t)w);
    else for (int x = 0; x < w; x++) o[(size_t)x] = r[(x + off) % w];
    cc.y[slot] = k;
    return o.data();
}
/** Rows y0 … y1 rendered (wefaxCrisp, the gold-standard look) into `out` ((y1 − y0 + 1) × w; a row that is not there = 0). */
inline void picRenderRows(const PicRow* rows, int n, int w, const wfx::Align& a, const wfx::Levels& lv, int y0, int y1, uint8_t* out) {
    PicAlCache cc;
    std::vector<float> v;
    auto rowF = [&](int j) { return picAlignedRow(rows, n, w, j, a, cc); };
    for (int y = y0; y <= y1; y++) {
        uint8_t* o = out + (size_t)(y - y0) * w;
        const uint8_t* al = rowF(y);
        if (!al) { std::memset(o, 0, (size_t)w); continue; }
        if (lv.ok) wfx::crispLine(rowF, y, w, lv, o, v); else std::memcpy(o, al, (size_t)w);
    }
}

// ── the worker ────────────────────────────────────────────────────────────────────────────────────────────────────────
class PicWorker {
public:
    void post(PicJob j) {
        std::lock_guard<std::mutex> lk(m_);
        // ★ one look and one render per picture queued at most: a newer render replaces an older one
        for (auto it = q_.begin(); it != q_.end(); ++it) if (it->pic == j.pic && it->type == j.type) { *it = std::move(j); cv_.notify_one(); return; }
        q_.push_back(std::move(j));
        if (!started_) { started_ = true; std::thread([this] { run(); }).detach(); }
        cv_.notify_one();
    }
    void take(std::vector<PicDone>& out) { std::lock_guard<std::mutex> lk(m_); out.swap(done_); }
private:
    std::mutex m_; std::condition_variable cv_;
    std::deque<PicJob> q_; std::vector<PicDone> done_;
    bool started_ = false;
    void run() {
#if defined(__linux__)
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   // ★ below the network, the audio and the drawing
#endif
        for (;;) {
            PicJob j;
            { std::unique_lock<std::mutex> lk(m_); cv_.wait(lk, [this] { return !q_.empty(); }); j = std::move(q_.front()); q_.pop_front(); }
            PicDone d; d.type = j.type; d.pic = j.pic; d.seq = j.seq; d.q = j.q; d.w = j.w; d.al = j.al; d.n = (int)j.rows.size();
            if (j.type == 1) {
                wfx::Rows rs(j.rows.size(), nullptr);
                for (size_t i = 0; i < j.rows.size(); i++) if (j.rows[i]) rs[i] = j.rows[i]->data();
                const double t0 = nowSec();
                d.res = wfx::alignLook(rs, j.w, j.station, j.fmt, j.q);
                std::fprintf(stderr, "vibe-hhui: wefax look at %d lines (%s) took %.0f ms: %s\n", j.q.n, j.q.kind == wfx::LK_FIRST ? "first" : j.q.kind == wfx::LK_RETRY ? "retry" : "second",
                             (nowSec() - t0) * 1e3, d.res.have ? (std::to_string((int)d.res.a.shift) + " px, slant " + std::to_string(d.res.a.slant)).c_str() : "nothing to move");
            } else if (d.n > 0) {
                d.px.resize((size_t)d.n * j.w);
                picRenderRows(j.rows.data(), d.n, j.w, j.al, j.lv, 0, d.n - 1, d.px.data());
            }
            std::lock_guard<std::mutex> lk(m_);
            done_.push_back(std::move(d));
        }
    }
};
inline PicWorker& picWorker() { static PicWorker* w = new PicWorker; return *w; }   // ★ never destroyed: its thread outlives main

// ── the state (UI thread) ─────────────────────────────────────────────────────────────────────────────────────────────
struct PicUi {
    std::unique_ptr<Pic> live, prev;
    bool viewingPrev = false;
    uint64_t nextId = 1;
    int lpm = 120;                   // WEFAX's LPM (THIS SESSION only, as the web page)
    // WEFAX geometry: the SLANT per kHz (the listener's own, else the station's); shift / RAW per chart (in Pic)
    std::map<int, double> saved; bool savedLoaded = false;
    int alignKey = INT32_MIN; bool alignSaved = false; wfx::Align align;
    bool phaseKnown = false;
    int lineBase = 0; bool rebase = false;
    double chartHz = 0;
    // HELL's pager (feldHell.ts HellPager)
    long hellBase = -1;
    // SSTV: a repaint (0x08) is under way — its lines land in place
    bool redrawing = false;
    std::string pendingMode;         // the last SSTV mode named (0x02)
    // the box
    bool adjOpen = false, adjWantFocus = false;
    int adjDir = 0, adjRun = 0; double adjAt = 0;   // a held key steps ×5 after 8 (useHoldRepeat)
    double lineAt = 0;               // the LED: a line in the last 2.5 s (until the server reports WEFAX phases)
    float scrollBack = 0;            // HELL in the box: rows scrolled up from the newest (WEFAX fits whole)
    // full screen: 0 closed, 1 the live picture (or PREV), 2 a gallery picture
    int full = 0; int zoomI = 0; float panX = 0.5f, panY = 1.f;
    double fullOpenedAt = 0, fullDrawnAt = 0;   // ★ the full-screen view is drawn from the sheet loop (decDrawFull) — see dec_box.h
};
inline PicUi& picUi() { static PicUi u; return u; }

/** A key in the box's header (dec_pic_box.h decPicKeys). */
struct PicKey { std::string id, label; int act; bool on; };

inline bool decIsPicture(const std::string& id) { return id == "wefax" || id == "sstv" || id == "hell"; }

// ── GL ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool picRgb(const Pic& p) { return p.kind == PK_SSTV; }
inline void picFree(Pic& p) { for (auto& t : p.tiles) if (t.tex) glDeleteTextures(1, &t.tex); p.tiles.clear(); }
/** Tiles enough for `rows` rows (each made at once, filled with black — or HELL's white paper). */
inline void picEnsureRows(Pic& p, int rows) {
    if (p.w <= 0) return;
    const int bpp = picRgb(p) ? 3 : 1;
    while ((int)p.tiles.size() * kPicTileRows < rows) {
        PicTile t; t.y0 = (int)p.tiles.size() * kPicTileRows;
        std::vector<uint8_t> fill((size_t)p.w * kPicTileRows * bpp, p.kind == PK_HELL ? 255 : 0);
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, bpp == 3 ? GL_RGB8 : GL_R8, p.w, kPicTileRows, 0, bpp == 3 ? GL_RGB : GL_RED, GL_UNSIGNED_BYTE, fill.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // ★ WEFAX wraps sideways: a margin change is shown at once by sliding the rendering (u offset) until it is redone
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, p.kind == PK_WEFAX ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (bpp == 1) {   // ★ grey: R8 read as R, R, R, 1 (gfx.h multiplies colour × texture)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        }
        glGenerateMipmap(GL_TEXTURE_2D);
        p.tiles.push_back(t);
    }
}
/** Upload rows y0 … y0 + n − 1 (tightly packed, w × bpp each). */
inline void picUpload(Pic& p, int y0, int n, const uint8_t* px) {
    if (n <= 0 || p.w <= 0) return;
    picEnsureRows(p, y0 + n);
    const int bpp = picRgb(p) ? 3 : 1;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    int y = y0;
    while (y < y0 + n) {
        PicTile& t = p.tiles[(size_t)(y / kPicTileRows)];
        const int k = std::min(y0 + n - y, t.y0 + kPicTileRows - y);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y - t.y0, p.w, k, bpp == 3 ? GL_RGB : GL_RED, GL_UNSIGNED_BYTE, px + (size_t)(y - y0) * p.w * bpp);
        t.dirty = true;
        y += k;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}
/** The mipmaps of the tiles drawn into, at most twice a second (a 1809-px chart is drawn ~3× smaller in the box). */
inline void picMips(Pic& p, double now) {
    if (now - p.mipAt < 0.5) return;
    p.mipAt = now;
    for (auto& t : p.tiles) if (t.dirty) { glBindTexture(GL_TEXTURE_2D, t.tex); glGenerateMipmap(GL_TEXTURE_2D); t.dirty = false; }
}
/** Draw picture rows [sy0, sy1) into `dst` (full width), slid left by `u` of the width (WEFAX's margin preview). */
inline void picDraw(Gfx& g, const Pic& p, Rect dst, float sy0, float sy1, float u = 0, float alpha = 1) {
    if (sy1 <= sy0 || p.tiles.empty()) return;
    const float k = dst.h / (sy1 - sy0);
    u -= std::floor(u);
    for (auto& t : p.tiles) {
        const float a = std::max(sy0, (float)t.y0), b = std::min(sy1, (float)(t.y0 + kPicTileRows));
        if (b <= a) continue;
        g.imageUV(t.tex, dst.x, dst.y + (a - sy0) * k, dst.w, (b - a) * k, u, (a - t.y0) / kPicTileRows, u + 1, (b - t.y0) / kPicTileRows, {1, 1, 1, alpha});
    }
}
/** Draw columns [sx0, sx1) × rows [sy0, sy1) into dst (the full-screen zoom). */
inline void picDrawPart(Gfx& g, const Pic& p, Rect dst, float sx0, float sx1, float sy0, float sy1, float u = 0) {
    if (sy1 <= sy0 || sx1 <= sx0 || p.tiles.empty() || p.w <= 0) return;
    const float k = dst.h / (sy1 - sy0);
    u -= std::floor(u);
    for (auto& t : p.tiles) {
        const float a = std::max(sy0, (float)t.y0), b = std::min(sy1, (float)(t.y0 + kPicTileRows));
        if (b <= a) continue;
        g.imageUV(t.tex, dst.x, dst.y + (a - sy0) * k, dst.w, (b - a) * k, u + sx0 / p.w, (a - t.y0) / kPicTileRows, u + sx1 / p.w, (b - t.y0) / kPicTileRows, {1, 1, 1, 1});
    }
}
/** The rows a picture shows: SSTV its whole frame, HELL the strips drawn so far, WEFAX the lines so far. */
inline int picShownRows(const Pic& p) {
    if (p.kind == PK_SSTV) return std::max(p.h, p.maxY + 1);
    if (p.kind == PK_HELL) return p.maxY < 0 ? 0 : (p.maxY / kHellStripRows + 1) * kHellStripRows;
    return p.maxY + 1;
}

// ── WEFAX geometry ────────────────────────────────────────────────────────────────────────────────────────────────────
inline std::string picAlignPath() { const char* h = std::getenv("HOME"); return std::string(h && *h ? h : "/tmp") + "/.vibe-hhui-wefax.json"; }
inline void picLoadSaved() {
    PicUi& u = picUi();
    if (u.savedLoaded) return;
    u.savedLoaded = true;
    FILE* f = std::fopen(picAlignPath().c_str(), "r");
    if (!f) return;
    std::string s; char b[4096]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0 && s.size() < 65536) s.append(b, n);
    std::fclose(f);
    const Json j = Json::parse(s);
    for (auto& kv : j.o) if (kv.second.kind == Json::Num && u.saved.size() < 300) u.saved[std::atoi(kv.first.c_str())] = std::max(-2.0, std::min(2.0, kv.second.n));
}
inline void picWriteSaved() {
    PicUi& u = picUi();
    std::string s = "{";
    for (auto& kv : u.saved) { char b[64]; std::snprintf(b, sizeof b, "%s\"%d\":%.3f", s.size() > 1 ? "," : "", kv.first, kv.second); s += b; }
    s += "}\n";
    const std::string p = picAlignPath(), tmp = p + ".tmp";   // ★ temp + rename (Stuart: "the safest option")
    if (FILE* f = std::fopen(tmp.c_str(), "w")) { std::fwrite(s.data(), 1, s.size(), f); std::fclose(f); std::rename(tmp.c_str(), p.c_str()); }
}
/** decCorrAlign: this chart's shift (by hand, else its own auto) and the slant (measured on it unless set by hand on
 *  it, else the listener's saved one for this kHz, else the station's). */
inline wfx::Align picCorr(const Pic& p) {
    const PicUi& u = picUi();
    const double shift = p.hasManual ? p.manualShift : p.st.hasAl ? p.st.al.shift : 0;
    const double slant = !p.slantTouched && p.st.hasSlant ? p.st.slant : u.align.slant;
    return {shift, slant};
}
inline wfx::Align picEff(const Pic& p) { return wfx::drawnAlign(picCorr(p), p.rawMode); }
inline bool picSlantManual(const Pic& p) { return p.slantTouched || (picUi().alignSaved && !p.st.hasSlant); }
/** The whole chart rendered again (on the worker) with its geometry now, `delay` s from now (a burst of presses = one). */
inline void picRepaintSoon(Pic& p, double delay = 0) { if (p.kind != PK_WEFAX) return; p.paintPending = true; p.paintDue = nowSec() + delay; }
inline void picRepaintNow(Pic& p) {
    if (p.kind != PK_WEFAX || p.maxY < 0 || p.w <= 0) { p.paintPending = false; return; }
    PicJob j; j.type = 2; j.pic = p.id; j.seq = ++p.paintSeq; j.w = p.w; j.al = picEff(p); j.lv = wfx::crispLevels(p.hist);
    j.rows.assign(p.raw.begin(), p.raw.begin() + std::min((size_t)p.maxY + 1, p.raw.size()));
    picWorker().post(std::move(j));
    p.paintPending = false;
}
/** Rows y0 … y1 rendered here (a new line and the two above it — the cheap case) with the texture's geometry. */
inline void picPaintRows(Pic& p, int y0, int y1) {
    y0 = std::max(0, y0); y1 = std::min(y1, p.maxY);
    if (y1 < y0 || p.w <= 0) return;
    static std::vector<uint8_t> buf;
    buf.resize((size_t)(y1 - y0 + 1) * p.w);
    picRenderRows(p.raw.data(), p.maxY + 1, p.w, p.tex, wfx::crispLevels(p.hist), y0, y1, buf.data());
    picUpload(p, y0, y1 - y0 + 1, buf.data());
}
inline void picSetAlign(bool have, wfx::Align a, bool save) {
    PicUi& u = picUi();
    picLoadSaved();
    if (!have) { u.saved.erase(u.alignKey); picWriteSaved(); u.alignSaved = false; a = wfx::preset((double)u.alignKey * 1000); }
    else if (save) {
        a = {0, wfx::jsRound(a.slant * 1000) / 1000};   // ★ only the slant is the station's
        u.saved[u.alignKey] = a.slant; picWriteSaved(); u.alignSaved = true;
    }
    u.align = a;
    if (u.live && u.live->kind == PK_WEFAX) picRepaintSoon(*u.live, 0.15);
}
/** loadDecAlign: this kHz's slant (the listener's own, else the station's) — a retune picks it up. */
inline void picLoadAlign(double hz) {
    PicUi& u = picUi();
    const int key = wfx::alignKey(hz);
    if (key == u.alignKey) return;
    u.alignKey = key;
    picLoadSaved();
    auto it = u.saved.find(key);
    u.alignSaved = it != u.saved.end();
    u.align = u.alignSaved ? wfx::Align{0, it->second} : wfx::preset(hz);
    if (u.live && u.live->kind == PK_WEFAX) picRepaintSoon(*u.live, 0);
}

// ── pictures start, bank, clear ───────────────────────────────────────────────────────────────────────────────────────
/** startDecImage: a new picture. The one on screen, if it has anything in it, goes to PREV (never thrown away). */
inline Pic& picStart(PicKind kind, int w, int h) {
    PicUi& u = picUi();
    if (u.live && (u.live->complete || u.live->maxY > 0 || (u.live->kind == PK_HELL && u.live->maxY >= 0))) {
        if (u.prev) picFree(*u.prev);
        Pic& old = *u.live;
        old.complete = true;
        if (old.kind == PK_WEFAX) { old.dropRawAfterPaint = true; picRepaintNow(old); }   // ★ its final levels, then its lines go
        u.prev = std::move(u.live);
    } else if (u.live) picFree(*u.live);
    u.live.reset(new Pic);
    Pic& p = *u.live;
    p.id = u.nextId++; p.kind = kind; p.w = w; p.h = h;
    p.tex = kind == PK_WEFAX ? picEff(p) : wfx::Align{};
    u.scrollBack = 0;
    if (w > 0) picEnsureRows(p, std::max(1, kind == PK_WEFAX ? 1 : h));
    return p;
}
inline void picReset() {
    PicUi& u = picUi();
    if (u.live) picFree(*u.live);
    if (u.prev) picFree(*u.prev);
    u.live.reset(); u.prev.reset();
    u.viewingPrev = false; u.phaseKnown = false; u.lineBase = 0; u.rebase = false; u.chartHz = 0; u.hellBase = -1; u.redrawing = false;
    u.adjOpen = false; u.alignKey = INT32_MIN; u.scrollBack = 0; u.full = 0;
}
/** CLR on a picture (clearDecImage / clearHellPage): the picture so far goes under PREV, the next line starts a fresh one. */
inline void picClear(const std::string& why = "") {
    PicUi& u = picUi(); DecUi& d = decUi();
    if (d.active == "hell") {
        u.hellBase = -1;
        if (u.live && u.live->maxY >= 0) { u.viewingPrev = false; picStart(PK_HELL, u.live->w, kHellPageRows); }
        d.status = "cleared — the last page is under PREV";
        return;
    }
    if (!u.live) return;
    const PicKind k = u.live->kind;
    const int w = u.live->w, h = k == PK_SSTV ? u.live->h : 0;
    u.viewingPrev = false;
    picStart(k, w, h);
    if (k == PK_WEFAX) u.rebase = true;
    d.status = why.empty() ? "cleared — the last picture is under PREV" : why;
}
inline const Pic* picShown() { const PicUi& u = picUi(); return u.viewingPrev && u.prev ? u.prev.get() : u.live.get(); }

// ── the frames ───────────────────────────────────────────────────────────────────────────────────────────────────────
inline void picLive() { DecUi& d = decUi(); d.live = true; d.liveAt = nowSec(); picUi().lineAt = nowSec(); }
inline void picWefaxLine(const RadioState& s, uint32_t yIn, uint32_t w, const uint8_t* px, size_t n) {
    PicUi& u = picUi();
    if (w == 0 || w > (uint32_t)kPicMaxW) return;
    // ★★ A RETUNE STARTS A NEW CHART (3 kHz or more: a different transmission, joined mid-chart); fine-tuning does not
    const double f = s.vfoHz;
    if (u.chartHz && f && std::fabs(f - u.chartHz) >= 3000 && u.live && u.live->maxY > 0) picClear("new frequency — the last chart is under PREV");
    if (f) u.chartHz = f;
    long y = (long)yIn;
    if (u.rebase) { u.rebase = false; u.lineBase = (int)y; }
    if (y < u.lineBase) u.lineBase = 0;
    y -= u.lineBase;
    // ★★★ THE LINE COUNT GOING BACK IS A NEW PICTURE (a step back of ≤ 2 lines is tolerated)
    if (u.live && u.live->kind == PK_WEFAX && u.live->maxY > 0 && (y == 0 || y < u.live->maxY - 2)) { u.live->complete = true; picStart(PK_WEFAX, (int)w, 0); }
    if (!u.live || u.live->kind != PK_WEFAX || u.live->w != (int)w) picStart(PK_WEFAX, (int)w, 0);
    if (y >= kWefaxMaxLines) {   // ★ bounded: a chart this long is banked and the next line starts the next one
        std::fprintf(stderr, "vibe-hhui: wefax chart reached %d lines — banked under PREV, a new one starts\n", kWefaxMaxLines);
        u.lineBase += (int)y; y = 0;
        u.live->complete = true;
        picStart(PK_WEFAX, (int)w, 0);
    }
    Pic& p = *u.live;
    picLoadAlign(f);
    auto row = std::make_shared<std::vector<uint8_t>>((size_t)w, 255);
    std::memcpy(row->data(), px, std::min<size_t>(n, w));
    const int prevMax = p.maxY;
    if ((long)p.raw.size() <= y) p.raw.resize((size_t)y + 1);
    p.raw[(size_t)y] = row;
    wfx::addToHist(p.hist, row->data(), (int)w);
    if (y > p.maxY) p.maxY = (int)y;
    // the auto-align: counted here, LOOKED at on the worker
    if (!p.hasManual) {
        const wfx::LookReq q = wfx::alignStepPre(p.st, row->data(), (int)w, p.lookPending);
        if (q.kind != wfx::LK_NONE) {
            PicJob j; j.type = 1; j.pic = p.id; j.w = (int)w; j.q = q; j.station = wfx::preset(f).slant; j.fmt = wfx::format(f);
            j.rows.assign(p.raw.begin(), p.raw.end());
            picWorker().post(std::move(j));
            p.lookPending = true;
        }
    }
    // paint it and the two above it (a lost run under it re-filled); line 40: the levels have settled — the top again
    const int g0 = std::min(prevMax + 1, (int)y + 1);
    if (y == 40) picPaintRows(p, 0, (int)y); else picPaintRows(p, std::min((int)y, g0) - 2, p.maxY);
    if (!u.phaseKnown) picLive();
}
inline void picSstvLine(uint32_t y, uint32_t w, const uint8_t* px, size_t n) {
    PicUi& u = picUi();
    if (w == 0 || w > (uint32_t)kSstvMaxDim || y >= (uint32_t)kSstvMaxDim) return;
    if (!u.redrawing && u.live && u.live->kind == PK_SSTV && u.live->maxY > 0 && (y == 0 || (int)y < u.live->maxY - 2)) {
        u.live->complete = true; picStart(PK_SSTV, (int)w, u.live->h);
    }
    if (!u.live || u.live->kind != PK_SSTV || u.live->w != (int)w) picStart(PK_SSTV, (int)w, 256);
    Pic& p = *u.live;
    std::vector<uint8_t> line((size_t)w * 3, 0);
    std::memcpy(line.data(), px, std::min<size_t>(n, line.size()));
    picUpload(p, (int)y, 1, line.data());
    if ((int)y > p.maxY) p.maxY = (int)y;
    picLive();
}
/** feldHell.ts HellPager.place */
inline int picHellPlace(long line, bool& newPage) {
    PicUi& u = picUi();
    const long stripStart = line - (line % kHellStripRows);
    newPage = false;
    if (u.hellBase < 0 || line < u.hellBase || line - u.hellBase >= kHellPageRows) { u.hellBase = stripStart; newPage = true; }
    return (int)(line - u.hellBase);
}
inline void picHellLine(uint32_t line, uint32_t w, const uint8_t* px, size_t n) {
    PicUi& u = picUi();
    if (w == 0 || w > (uint32_t)kHellMaxW) return;
    bool np;
    const int y = picHellPlace((long)line, np);
    if (np || !u.live || u.live->kind != PK_HELL || u.live->w != (int)w) {
        if (u.live && u.live->maxY >= 0) u.live->complete = true;
        picStart(PK_HELL, (int)w, kHellPageRows);
    }
    if (y < 0 || y >= kHellPageRows) return;
    Pic& p = *u.live;
    std::vector<uint8_t> row((size_t)w, 255);
    std::memcpy(row.data(), px, std::min<size_t>(n, w));
    picUpload(p, y, 1, row.data());
    if (y > p.maxY) p.maxY = y;
    picLive();
}

inline void decPicBinary(App& a, const RadioState& s, const std::string& b) {
    (void)a;
    DecUi& d = decUi(); PicUi& u = picUi();
    const uint8_t op = (uint8_t)b[0];
    const uint8_t* px = (const uint8_t*)b.data();
    if (d.active == "wefax") {
        if (op == 0x01 && b.size() >= 9) picWefaxLine(s, decU32(b, 1), decU32(b, 5), px + 9, b.size() - 9);
        else if (op == 0x02) { if (u.live && u.live->maxY > 0) picStart(PK_WEFAX, u.live->w, 0); }
        else if (op == 0x03) {
            d.status = "image complete";
            if (u.live) { u.live->complete = true; picRepaintNow(*u.live); }
        } else if (op == 0x04 && b.size() >= 2) {
            // ★★ what part of the transmission is arriving (Stuart: "show the part … such as the phasing lines")
            u.phaseKnown = true;
            const int ph = (uint8_t)b[1];
            d.status = ph <= 4 ? kWefaxPhaseText[ph] : "receiving";
            d.live = ph >= 1 && ph <= 3; d.liveAt = nowSec();
        } else if (op == 0x08) std::fprintf(stderr, "vibe-hhui: [wefax] %s\n", b.substr(1, 300).c_str());
    } else if (d.active == "sstv") {
        if (op == 0x07 && b.size() >= 9) {
            const uint32_t w = decU32(b, 1), h = decU32(b, 5);
            if (w > 0 && w <= (uint32_t)kSstvMaxDim && h > 0 && h <= (uint32_t)kSstvMaxDim) {
                // ★ the mode (0x02) comes just before the start: it is the NEW picture's
                const std::string md = u.live && u.live->maxY < 0 ? u.live->mode : u.pendingMode;
                u.redrawing = false;
                Pic& p = picStart(PK_SSTV, (int)w, (int)h);
                p.mode = md.empty() ? u.pendingMode : md;
            }
        } else if (op == 0x01 && b.size() >= 9) picSstvLine(decU32(b, 1), decU32(b, 5), px + 9, b.size() - 9);
        else if (op == 0x02 && b.size() >= 3) {
            const size_t len = std::min<size_t>(((size_t)(uint8_t)b[1] << 8 | (uint8_t)b[2]), b.size() - 3);
            d.status = decClip(b.substr(3, len), 40);
            u.pendingMode = d.status;
            if (u.live && u.live->maxY < 0) u.live->mode = d.status;
        } else if (op == 0x03 && b.size() >= 4) {
            const size_t len = std::min<size_t>(((size_t)(uint8_t)b[2] << 8 | (uint8_t)b[3]), b.size() - 4);
            d.status = decClip(b.substr(4, len), 120);
        } else if (op == 0x05) { d.status = "image complete"; u.redrawing = false; if (u.live) u.live->complete = true; }
        else if (op == 0x08) { u.redrawing = true; if (u.live) u.live->complete = false; }   // ★ the same picture, repainted in place
    } else if (d.active == "hell") {
        if (op == 0x01 && b.size() >= 9) picHellLine(decU32(b, 1), decU32(b, 5), px + 9, b.size() - 9);
        else if (op == 0x02) u.hellBase = -1;
    }
}
/** feldHell.ts hellStatusText */
inline std::string picHellStatus(bool have, int hz, bool signal) {
    if (have && signal) return "drawing · " + std::to_string(hz) + " Hz";
    return have && hz ? "listening — last tone " + std::to_string(hz) + " Hz" : "listening for a Hell tone";
}
inline bool decPicJson(const std::string& type, const Json& d) {
    if (type != "hell_status") return false;
    DecUi& u = decUi();
    if (u.active != "hell" || d.kind != Json::Obj) return true;
    const int hz = (int)std::max(0.0, std::min(5000.0, std::round(d["hz"].num(0))));
    const bool sig = d["signal"].kind == Json::Bool ? d["signal"].b : d["signal"].num(0) == 1;
    u.status = picHellStatus(true, hz, sig);
    u.live = sig; u.liveAt = nowSec();
    return true;
}
/** A picture decoder opens (showDecBox / resetDecImages): a clean box, ALIGN closed, this kHz's slant loaded. */
inline void decPicOpen(const RadioState& s, const std::string& id) {
    picReset();
    if (id == "wefax") picLoadAlign(s.vfoHz);
    if (id == "hell") decUi().status = picHellStatus(false, 0, false);
}
/** The attach parameters (main.ts decParams: wefax). */
inline std::string decPicParams(const std::string& id) {
    if (id == "wefax") return ",\"lpm\":" + std::to_string(picUi().lpm) + ",\"carrier\":1900,\"deviation\":400,\"image_width\":1809";
    return "";
}

// ── once a loop (decTick) ────────────────────────────────────────────────────────────────────────────────────────────
inline Pic* picById(uint64_t id) {
    PicUi& u = picUi();
    if (u.live && u.live->id == id) return u.live.get();
    if (u.prev && u.prev->id == id) return u.prev.get();
    return nullptr;
}
inline void decPicTick(App& a, const RadioState& s) {
    (void)a; (void)s;
    PicUi& u = picUi(); DecUi& d = decUi();
    const double now = nowSec();
    std::vector<PicDone> got;
    picWorker().take(got);
    for (auto& r : got) {
        Pic* p = picById(r.pic);
        if (!p) continue;
        if (r.type == 1) {
            p->lookPending = false;
            if (p->hasManual) continue;
            const bool hadSlant = p->st.hasSlant; const double slantBefore = p->st.slant;
            const bool moved = wfx::alignStepApply(p->st, r.q, r.res, p->w, wfx::preset(u.chartHz).slant);
            const bool slantChanged = p->st.hasSlant != hadSlant || (p->st.hasSlant && p->st.slant != slantBefore);
            if (moved || (slantChanged && !p->slantTouched)) picRepaintNow(*p);
        } else if (r.seq == p->paintSeq && r.n > 0 && r.w == p->w) {
            picUpload(*p, 0, r.n, r.px.data());
            p->tex = r.al;
            if (p == u.live.get()) picPaintRows(*p, r.n - 3, p->maxY);   // ★ the lines that came in meanwhile, in the new geometry
            if (p->dropRawAfterPaint) { p->raw.clear(); p->raw.shrink_to_fit(); }
        }
    }
    if (u.live && u.live->paintPending && now >= u.live->paintDue) picRepaintNow(*u.live);
    if (u.live) picMips(*u.live, now);
    if (u.prev) picMips(*u.prev, now);
    // the LED: lines in the last 2.5 s (once the server reports WEFAX phases, the phase says it instead)
    if (decIsPicture(d.active) && !(d.active == "wefax" && u.phaseKnown) && d.active != "hell" && d.live && now - u.lineAt > 2.5) d.live = false;
}

// ── the WEFAX ADJ actions (main.ts nudgeShift / nudgeSlant / AUTO / RAW) ──────────────────────────────────────────────
/** A held key steps ×5 from its 8th repeat (useHoldRepeat); presses ≤ 0.25 s apart in one direction are a hold. */
inline int picHoldMult(int dir) {
    PicUi& u = picUi();
    const double now = nowSec();
    if (dir == u.adjDir && now - u.adjAt < 0.25) u.adjRun++; else u.adjRun = 0;
    u.adjDir = dir; u.adjAt = now;
    return u.adjRun >= 8 ? 5 : 1;
}
inline void picNudgeShift(int d) {
    PicUi& u = picUi();
    if (!u.live || u.live->kind != PK_WEFAX || u.viewingPrev) return;
    Pic& p = *u.live;
    if (p.rawMode) { p.rawMode = false; }
    p.manualShift = picCorr(p).shift + d; p.hasManual = true;
    picRepaintSoon(p, 0.35);   // ★ shown at once, slid; rendered again when the presses stop
}
inline void picNudgeSlant(double dk) {
    PicUi& u = picUi();
    if (!u.live || u.live->kind != PK_WEFAX || u.viewingPrev) return;
    Pic& p = *u.live;
    if (p.rawMode) p.rawMode = false;
    const double k = wfx::jsRound((picCorr(p).slant + dk) * 1000) / 1000;
    p.slantTouched = true;
    picSetAlign(true, {0, k}, true);
    picRepaintSoon(p, 0.35);
}
inline void picAuto() {
    PicUi& u = picUi();
    if (!u.live || u.live->kind != PK_WEFAX) return;
    Pic& p = *u.live;
    p.hasManual = false; p.rawMode = false; p.slantTouched = false;
    picSetAlign(false, {}, false);
}
inline void picToggleRaw() { PicUi& u = picUi(); if (!u.live || u.live->kind != PK_WEFAX) return; u.live->rawMode = !u.live->rawMode; picRepaintSoon(*u.live, 0); }
/** decSignedPx: the shift as the listener reads it — px, signed, the short way round. */
inline std::string picSignedPx(double shift) {
    const PicUi& u = picUi();
    const double W = u.live && u.live->w ? u.live->w : 1809;
    const double s = wfx::jsRound(wfx::jsMod(wfx::jsMod(shift, W) + W + W / 2, W) - W / 2);
    char b[32]; std::snprintf(b, sizeof b, "%s%d", s > 0 ? "+" : s < 0 ? "-" : "", (int)std::fabs(s)); return b;
}
inline std::string picSignedSlant(double k) { char b[32]; std::snprintf(b, sizeof b, "%s%.3f", k > 0 ? "+" : k < 0 ? "-" : "", std::fabs(k)); return b; }

// ── tests: canned pictures through the real frame path (VIBE_HHUI_DEC_CANNED=1) ─────────────────────────────────────
/** A synthetic NWS-style chart (decoders' world corpus 'black5'): a speckled page with isobars and a coast, the
 *  1728-px picture with 81 px of black strip after it — received ROLLED 700 px (joined without phasing), so the
 *  auto-align has a strip to find and cut down its middle. */
inline void picCannedWefaxRow(int y, std::vector<uint8_t>& o) {
    const int W = 1809, roll = 700;
    std::vector<uint8_t> t((size_t)W);
    uint32_t sd = 2166136261u ^ (uint32_t)(y * 7919);
    for (int x = 0; x < W; x++) {
        sd = sd * 1664525u + 1013904223u;
        int v = 225 + (int)(sd >> 27);                                     // paper
        if (x >= 1728) v = 8;                                               // the black strip
        t[(size_t)x] = (uint8_t)v;
    }
    for (int c = 0; c < 5; c++) {                                           // isobars
        const double cx = 300 + c * 290 + 120 * std::sin((y + c * 70) / 110.0);
        for (int k = -1; k <= 1; k++) { const int x = (int)cx + k; if (x >= 0 && x < 1728) t[(size_t)x] = 30; }
    }
    for (int r = 1; r <= 3; r++) {                                          // a low's rings
        const double dy = y - 450.0, rr = 60.0 * r;
        if (std::fabs(dy) < rr) { const int dx = (int)std::sqrt(rr * rr - dy * dy); for (int sg = -1; sg <= 1; sg += 2) { const int x = 900 + sg * dx; for (int k = 0; k < 3; k++) if (x + k < 1728) t[(size_t)(x + k)] = 20; } }
    }
    { const int cx = 200 + (int)(80 * std::sin(y / 60.0)) + y / 4; for (int x = std::max(0, cx - 400); x < std::min(1728, cx); x += 9) t[(size_t)x] = 60; }   // land
    if (y % 100 < 6) for (int x = 40; x < 1690; x++) if ((x / 8) % 3 == 0) t[(size_t)x] = 40;   // a text line
    o.resize((size_t)W);
    for (int x = 0; x < W; x++) o[(size_t)x] = t[(size_t)(((x - roll) % W + W) % W)];
}
inline void picCannedSstvRow(int pic, int y, std::vector<uint8_t>& o) {
    const int W = 320; o.resize(W * 3);
    static const uint8_t bars[8][3] = {{235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16}, {235, 16, 235}, {235, 16, 16}, {16, 16, 235}, {16, 16, 16}};
    for (int x = 0; x < W; x++) {
        uint8_t* p = &o[(size_t)x * 3];
        if (y < 120) { const uint8_t* c = bars[(x * 8 / W + pic) % 8]; p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; }
        else if (y < 160) { p[0] = p[1] = p[2] = (uint8_t)(x * 255 / W); }
        else {
            const double dx = x - 160, dy = y - 208, r = std::sqrt(dx * dx + dy * dy);
            const bool ring = std::fabs(r - 40) < 4;
            p[0] = ring ? 255 : (uint8_t)(40 + pic * 80); p[1] = ring ? 200 : 60; p[2] = ring ? 40 : (uint8_t)(160 - pic * 60);
        }
    }
}
/** A 5 × 7 font for the canned HELL text. */
inline const uint8_t* picCannedGlyph(char c) {
    static const struct { char c; uint8_t r[7]; } g[] = {
        {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
        {'E', {31, 16, 16, 30, 16, 16, 31}}, {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
        {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}}, {'N', {17, 25, 21, 19, 17, 17, 17}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
        {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}}, {'T', {31, 4, 4, 4, 4, 4, 4}},
        {'V', {17, 17, 17, 17, 17, 10, 4}}, {'4', {2, 6, 10, 18, 31, 2, 2}}, {'O', {14, 17, 17, 17, 17, 17, 14}}, {'M', {17, 27, 21, 21, 17, 17, 17}}};
    for (auto& x : g) if (x.c == c) return x.r;
    return nullptr;
}
inline void decPicCanned(App& a, const RadioState& s, int k) {
    DecUi& d = decUi();
    auto line = [&](uint8_t op, uint32_t y, uint32_t w, const std::vector<uint8_t>& px) {
        std::string b(9, '\0'); b[0] = (char)op;
        for (int i = 0; i < 4; i++) { b[1 + i] = (char)(y >> (24 - 8 * i)); b[5 + i] = (char)(w >> (24 - 8 * i)); }
        b.append((const char*)px.data(), px.size());
        decOnBinary(a, s, b);
    };
    std::vector<uint8_t> row;
    if (d.active == "wefax") {
        const int len = 760, per = 12;               // ★ 48 lines a second: the first look (300 lines) comes in ~6 s
        const int cyc = len / per + 30, c = k % cyc, chart = k / cyc;
        if (c == 0) decOnBinary(a, s, std::string("\x04\x02", 2));
        if (c == 2) decOnBinary(a, s, std::string("\x04\x03", 2));
        if (c < len / per) for (int i = 0; i < per; i++) {
            const int y = c * per + i;
            if (chart == 0 && y >= 520 && y < 523) continue;   // ★ three lost lines (filled from the one above)
            picCannedWefaxRow(y, row); line(0x01, (uint32_t)y, 1809, row);
        }
        if (c == len / per) { decOnBinary(a, s, std::string("\x03", 1)); decOnBinary(a, s, std::string("\x04\x00", 2)); }
    } else if (d.active == "sstv") {
        const int cyc = 256 / 16 + 12, c = k % cyc, pic = (k / cyc) % 3;
        if (c == 0) {
            const std::string nm = "Martin 1";
            std::string m(3, '\0'); m[0] = 0x02; m[2] = (char)nm.size(); decOnBinary(a, s, m + nm);
            std::string st(9, '\0'); st[0] = 0x07; st[3] = 1; st[4] = 64; st[7] = 1; st[8] = 0; decOnBinary(a, s, st);   // 320 × 256
        }
        if (c < 16) for (int i = 0; i < 16; i++) { picCannedSstvRow(pic, c * 16 + i, row); line(0x01, (uint32_t)(c * 16 + i), 320, row); }
        if (c == 16) { std::string m(5, '\0'); m[0] = 0x05; decOnBinary(a, s, m); }
    } else if (d.active == "hell") {
        static const std::string txt = "CQ CQ DE G4ABC G4ABC PSE K   VIBESDR HANDHELD TEST   CQ CQ DE G4ABC K   ";
        const int ch = k;                             // a character a tick (the engine: 2.5 a second)
        if (k % 40 == 0) { std::string j = "{\"type\":\"hell_status\",\"data\":{\"hz\":980,\"signal\":1}}"; decOnJson(a, s, j); }
        const int strip = ch / 30, inStrip = ch % 30 + 1;
        std::vector<uint8_t> r(210);
        for (int rr = 0; rr < 28; rr++) {
            std::fill(r.begin(), r.end(), 255);
            const int gr = (rr % 14) / 2;             // the glyph row (each printed twice, two half-elements each)
            for (int cIdx = 0; cIdx < inStrip; cIdx++) {
                const uint8_t* gl = picCannedGlyph(txt[(size_t)((strip * 30 + cIdx) % txt.size())]);
                if (!gl) continue;
                for (int col = 0; col < 5; col++) if (gl[gr] & (16 >> col)) r[(size_t)(cIdx * 7 + col)] = 0;
            }
            line(0x01, (uint32_t)(strip * 28 + rr), 210, r);
        }
    }
}

}  // namespace hh
