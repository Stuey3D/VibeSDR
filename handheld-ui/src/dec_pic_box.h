// dec_pic_box.h — the PICTURE decoders drawn (dec_pic.h is their state): the picture in the decoder box, WEFAX's ADJ
// row, the picture FULL SCREEN, and the GALLERY sheet (dec_pic_gallery.h). Included at the end of dec_box.h.
//
// ★★ IN THE BOX (over the waterfall, dec_box.h): the picture takes the box's whole body. WEFAX at the box's width with
//    the NEWEST lines at the foot (a fax machine's view; grabbed, UP / DOWN scroll back up the chart), SSTV whole at the
//    box's height with its mode and the last picture beside it, HELL's newest strips at the box's width (the text big
//    enough to read). Grabbed, the picture is a stop: ENTER opens it FULL SCREEN (dab_box.h dabDrawFull's way).
// ★★ FULL SCREEN (decDrawFull — drawn from the sheet loop, `case S_DEC`): the picture fitted; ENTER zooms 1 → 1.5 → 2 →
//    3 → 4 → FIT, the D-pad pans while zoomed; at FIT DOWN reaches the keys under it (PREV / LIVE · GALLERY), LEFT /
//    RIGHT step PREV ⇄ LIVE. BACK closes it.
// ★★ WEFAX's ADJ (main.ts decAdjRow, the app's DecoderPanel): MARGIN · SLANT · AUTO · RAW — MARGIN and SLANT are
//    full-width-style adjusters (LEFT / RIGHT move the chart 1 px / the slant 0.001; held, ×5 from the 8th step), the
//    figures shown are the ones APPLIED (AUTO until set by hand). AUTO and RAW are the mode pair.
#pragma once
#include "dec_pic_gallery.h"

namespace hh {

inline const std::vector<float>& picZooms() { static const std::vector<float> z = {1, 1.5f, 2, 3, 4}; return z; }

/** The idle words in the box before the first line. */
inline std::string picIdle(const std::string& id) {
    if (id == "wefax") return "standing by — a chart draws here line by line as it is received. Tune USB 1.9 kHz below the station's frequency (Northwood 4608.1, DWD 7878.1 kHz).";
    if (id == "sstv") return "listening — a picture draws here as it arrives; the mode is found by itself. 14.230 MHz USB is the 20 m calling frequency.";
    return "listening for a Hell tone — tune USB with the signal in your passband; the text is painted here as it is heard.";
}
/** WEFAX's u offset: the margin set since the rendering (shown slid until it is rendered again). */
inline float picUOff(const Pic& p) {
    if (p.kind != PK_WEFAX || p.w <= 0 || &p != picUi().live.get()) return 0;
    return (float)((picEff(p).shift - p.tex.shift) / p.w);
}

// ── the header's keys (dec_box.h asks; act codes 60 +) ───────────────────────────────────────────────────────────────
inline void decPicKeys(App& a, const std::string& view, std::vector<PicKey>& out) {
    const PicUi& u = picUi();
    if (u.prev) out.push_back({"dec.k.prev", u.viewingPrev ? "LIVE" : "PREV", 60, u.viewingPrev});
    if (view == "wefax" && u.live && !u.viewingPrev) {
        std::string l = "ADJ";
        const Pic& p = *u.live;
        const wfx::Align c = picCorr(p);
        // ★★ a SAVED slant says so on the key (2026-10-08: "I didn't realise it had saved a slant")
        if (!p.rawMode && !p.slantTouched && u.alignSaved && !p.st.hasSlant && std::fabs(c.slant) >= 0.0005) l = "ADJ · SLANT " + picSignedSlant(c.slant) + " SAVED";
        out.push_back({"dec.k.adj", l, 61, u.adjOpen});
    }
    if (galEnabled(a.eng_->port())) out.push_back({"dec.k.gal", "GALLERY", 62, false});
}
inline void decPicKeyPress(App& a, int act, const std::string& view) {
    PicUi& u = picUi();
    if (act == 60) { if (u.prev) u.viewingPrev = !u.viewingPrev; u.scrollBack = 0; }
    else if (act == 61) { u.adjOpen = !u.adjOpen; u.adjWantFocus = u.adjOpen; }
    else if (act == 62) galOpen(a, view == "sstv" ? 1 : 0);
}

// ── the ADJ row ──────────────────────────────────────────────────────────────────────────────────────────────────────
inline void picAdjRow(App& a, Ctx& c, Rect r) {
    PicUi& u = picUi();
    if (!u.live || u.live->kind != PK_WEFAX) return;
    Pic& p = *u.live;
    // ★ the highlight lands on MARGIN as the row opens (set before it registers, so the Nav keeps it)
    if (u.adjWantFocus) { a.nav_.setFocus("dec", "dec.adj.m"); u.adjWantFocus = false; }
    const wfx::Align al = picCorr(p);
    const std::string m = p.rawMode ? "MARGIN RAW" : std::string("MARGIN ") + (p.hasManual ? "MANUAL " : "AUTO ") + picSignedPx(al.shift) + " px";
    const std::string sl = p.rawMode ? "SLANT RAW" : std::string("SLANT ") + (picSlantManual(p) ? "MANUAL " : "AUTO ") + picSignedSlant(al.slant);
    const float wm = 196, ws = 188, wa = 74, wr = 66, gap = 6;
    float x = r.x;
    auto adjuster = [&](const std::string& id, float w, const std::string& label, bool lit) -> int {
        const Rect b{x, r.y, w, r.h};
        const Nav::Hit h = c.nav.item(id, b, true);
        c.g.rrect(b.x, b.y, b.w, b.h, 6, lit ? th::activeBg : th::btnBg);
        c.g.rframe(b.x, b.y, b.w, b.h, 6, 1, lit ? th::activeBdr : th::btnBorder);
        c.g.triangle(b.x + 12, b.cy() - 5, b.x + 12, b.cy() + 5, b.x + 5, b.cy(), th::btnText);
        c.g.triangle(b.r() - 12, b.cy() - 5, b.r() - 12, b.cy() + 5, b.r() - 5, b.cy(), th::btnText);
        textIn(c, c.f.tiny, label, b, lit ? th::activeTx : th::btnText);
        if (h.focused) focusRing(c, b, 6, true);
        x += w + gap;
        return h.adjust;
    };
    const int dm = adjuster("dec.adj.m", wm, m, !p.rawMode && p.hasManual);
    if (dm) picNudgeShift(-dm * picHoldMult(dm));   // ★ ◀ moves the chart left (the shift grows)
    const int ds = adjuster("dec.adj.s", ws, sl, !p.rawMode && picSlantManual(p));
    if (ds) picNudgeSlant(ds * wfx::SLANT_STEP * picHoldMult(10 + ds));
    BtnStyle st; st.font = &c.f.tiny; st.radius = 6;
    st.lit = !p.rawMode && !picSlantManual(p) && !p.hasManual;
    if (button(c, "dec.adj.auto", {x, r.y, wa, r.h}, "AUTO", st)) picAuto();
    x += wa + gap;
    st.lit = p.rawMode;
    if (button(c, "dec.adj.raw", {x, r.y, wr, r.h}, "RAW", st)) picToggleRaw();
}

// ── the box's body ───────────────────────────────────────────────────────────────────────────────────────────────────
inline void decPicPane(App& a, Ctx& c, const RadioState& s, Rect body, bool grabbed) {
    (void)s;
    PicUi& u = picUi(); DecUi& d = decUi();
    if (u.adjOpen && d.view == "wefax" && u.live && !u.viewingPrev && grabbed) {
        picAdjRow(a, c, {body.x + 4, body.y + 2, body.w - 8, 28});
        body.y += 34; body.h -= 34;
    }
    const Pic* p = picShown();
    if (!p || p->maxY < 0 || p->w <= 0) {
        // ★ the last picture, while the next is awaited, is under PREV — say so rather than look empty
        decNone(c, body, picIdle(d.view) + (u.prev ? "  The last one is under PREV." : ""));
        return;
    }
    const Nav::Hit h = c.nav.item("dec.pic", body);
    if (h.pressed) { u.full = 1; u.zoomI = 0; u.panX = 0.5f; u.panY = 1.f; u.fullOpenedAt = nowSec(); a.nav_.forget("dec"); }
    const float rows = (float)picShownRows(*p);
    if (p->kind == PK_SSTV) {
        // whole, at the box's height; beside it: the mode, the state, and the last picture
        const float k = std::min(body.h / rows, (body.w * 0.55f) / p->w);
        const Rect pr{body.x + 4, body.y + (body.h - rows * k) / 2, p->w * k, rows * k};
        picDraw(c.g, *p, pr, 0, rows);
        const float tx = pr.r() + 14;
        float y = body.y + 4;
        c.tc.draw(c.g, c.f.bodyB, decFit(c, c.f.bodyB, p->mode.empty() ? std::string("SSTV") : p->mode, (int)(body.r() - tx - 8)), tx, y, th::white); y += c.f.bodyB.lineHeight();
        c.tc.draw(c.g, c.f.small, u.viewingPrev ? "the last picture (PREV)" : p->complete ? "complete — kept in the gallery" : std::to_string(p->maxY + 1) + " of " + std::to_string((int)rows) + " lines", tx, y, th::white68);
        y += c.f.small.lineHeight() + 6;
        if (!u.viewingPrev && u.prev && u.prev->kind == PK_SSTV && u.prev->w > 0) {
            const float pk = std::min((body.b() - y - 4) / (float)picShownRows(*u.prev), 150.f / u.prev->w);
            if (pk > 0.05f) {
                const Rect qr{tx, y, u.prev->w * pk, picShownRows(*u.prev) * pk};
                picDraw(c.g, *u.prev, qr, 0, (float)picShownRows(*u.prev), 0, 0.85f);
                c.tc.draw(c.g, c.f.tiny, "PREV", qr.r() + 6, qr.y, th::dim);
            }
        }
    } else {
        // WEFAX / HELL: at the box's width, the newest at the foot
        const float k = body.w / p->w;
        const float vis = body.h / k;
        if (p->kind == PK_HELL) c.g.rect(body.x, body.y, body.w, std::min(body.h, rows * k), rgb(0xffffff));
        // grabbed: UP / DOWN scroll back up a long chart (the log's thin stops — dec_box.h decDrawLog)
        const float maxBack = std::max(0.f, rows - vis);
        static std::string prevFocus;
        const std::string fnow = a.nav_.focus("dec");
        const bool fromPic = prevFocus == "dec.pic";
        const float step = std::max(8.f, vis * 0.3f);
        if (fnow == "dec.pic.up") { if (fromPic) u.scrollBack += step; a.nav_.setFocus("dec", "dec.pic"); }
        else if (fnow == "dec.pic.dn") { if (fromPic) u.scrollBack -= step; a.nav_.setFocus("dec", "dec.pic"); }
        prevFocus = a.nav_.focus("dec");
        u.scrollBack = std::max(0.f, std::min(u.scrollBack, maxBack));
        if (grabbed && u.scrollBack < maxBack - 0.5f) c.nav.item("dec.pic.up", {body.x + 10, body.y, body.w - 20, 2});
        if (grabbed && u.scrollBack > 0.5f) c.nav.item("dec.pic.dn", {body.x + 10, body.b() - 2, body.w - 20, 2});
        const float y1 = rows - u.scrollBack, y0 = std::max(0.f, y1 - vis);
        const float hgt = (y1 - y0) * k;
        c.g.clip(body.x, body.y, body.w, body.h);
        picDraw(c.g, *p, {body.x, body.b() - hgt, body.w, hgt}, y0, y1, picUOff(*p));
        c.g.unclip();
        if (rows > vis) {
            const float frac = vis / rows, top = y0 / rows;
            c.g.rrect(body.r() - 6, body.y + top * body.h, 3, std::max(10.f, frac * body.h), 1.5f, th::dim);
        }
        if (u.viewingPrev) {
            const float w = (float)c.tc.width(c.f.tiny, "PREV") + 18;
            c.g.rrect(body.x + 6, body.y + 4, w, 18, 9, rgb(0, 0.8f));
            c.tc.draw(c.g, c.f.tiny, "PREV", body.x + 15, body.y + 5, th::amber);
        }
    }
    if (h.focused) {
        c.g.rframe(body.x - 1, body.y - 1, body.w + 2, body.h + 2, 7, 3, th::focus);
        const std::string t = "full screen";
        const float hw = 24 + (float)c.tc.width(c.f.small, t) + 20;
        const Rect hb{body.r() - hw - 10, body.b() - 30, hw, 26};
        c.g.rrect(hb.x, hb.y, hb.w, hb.h, 13, rgb(0, 0.85f));
        hints(c, hb.x + 8, hb.cy(), {{FACE_BOTTOM, t}}, th::focus);
    }
}

// ── full screen ──────────────────────────────────────────────────────────────────────────────────────────────────────
/** Draw `p` full screen at the zoom and pan; returns the screen rect it took. D-pad pans while zoomed (picUi's state). */
inline void picFullDraw(App& a, Ctx& c, const Pic& p, float uOff) {
    PicUi& u = picUi();
    const float W = (float)a.W, H = (float)a.H;
    c.g.rect(0, 0, W, H, rgb(0));
    const float rows = (float)std::max(1, picShownRows(p));
    if (p.w <= 0) return;
    const float k0 = std::min(W / p.w, H / rows), k = k0 * picZooms()[(size_t)u.zoomI];
    const float vw = std::min((float)p.w, W / k), vh = std::min(rows, H / k);
    const float sx0 = (p.w - vw) * u.panX, sy0 = (rows - vh) * u.panY;
    const float dw = vw * k, dh = vh * k;
    const Rect dst{(W - dw) / 2, (H - dh) / 2, dw, dh};
    if (p.kind == PK_HELL) c.g.rect(dst.x, dst.y, dst.w, dst.h, rgb(0xffffff));
    picDrawPart(c.g, p, dst, sx0, sx0 + vw, sy0, sy0 + vh, uOff);
}
/** The pad on a full-screen picture: ENTER zooms, the D-pad pans while zoomed. `lr` gets LEFT / RIGHT at FIT. */
inline void picFullNav(Ctx& c, const std::string& id, int& lr, bool& pressed) {
    PicUi& u = picUi();
    const Nav::Hit h = c.nav.item(id, {0, 0, 640, 440}, true);
    lr = 0; pressed = h.pressed;
    if (h.pressed) { u.zoomI = (u.zoomI + 1) % (int)picZooms().size(); }
    if (u.zoomI > 0 && h.focused) {
        const float st = 0.15f / picZooms()[(size_t)u.zoomI];
        if (h.adjust) u.panX = std::max(0.f, std::min(1.f, u.panX + h.adjust * st));
        const Dir dn = c.nav.input().nav;
        if (dn == D_UP) u.panY = std::max(0.f, u.panY - st);
        if (dn == D_DOWN) u.panY = std::min(1.f, u.panY + st);
    } else if (h.adjust) lr = h.adjust;
}
inline void picFoot(Ctx& c, App& a, const std::string& l1, const std::string& l2, bool keysToo) {
    const float fh = 54, W = (float)a.W, H = (float)a.H;
    c.g.rect(0, H - fh, W, fh, rgb(0, 0.74f));
    c.tc.draw(c.g, c.f.bodyB, decFit(c, c.f.bodyB, l1, keysToo ? 300 : (int)W - 200), 12, H - fh + 4, th::white);
    c.tc.draw(c.g, c.f.small, decFit(c, c.f.small, l2, keysToo ? 300 : (int)W - 24), 12, H - fh + 4 + c.f.bodyB.lineHeight(), th::white68);
    if (!keysToo) hints(c, W - 190, H - fh + 15, {{FACE_BOTTOM, "zoom"}, {FACE_RIGHT, "close"}}, th::white68);
}

inline void galDrawSheet(App& a, Ctx& c);
/** A full-screen view or the gallery is up (the box then draws nothing and registers no stops). */
inline bool decPicFullOpen() { return picUi().full != 0 || galUi().open; }
/** ★ THE FULL-SCREEN VIEWS — from the sheet loop: `case S_DEC: decDrawFull(*this, c, s); break;` (app_impl.h). Draws
 *  nothing unless a picture or the gallery is open. */
inline void decDrawFull(App& a, Ctx& c, const RadioState& s) {
    (void)s;
    PicUi& u = picUi();
    GalUi& g = galUi();
    if (!u.full && !g.open) return;
    u.fullDrawnAt = nowSec();
    if (g.open) { galDrawSheet(a, c); return; }
    const Pic* p = picShown();
    if (!p || p->maxY < 0) { u.full = 0; a.nav_.forget("dec"); return; }
    picFullDraw(a, c, *p, picUOff(*p));
    int lr; bool pr;
    picFullNav(c, "dec.full", lr, pr);
    if (lr < 0 && u.prev) u.viewingPrev = true;
    if (lr > 0) u.viewingPrev = false;
    const DecUi& d = decUi();
    std::string l1 = decTitle(d.view) + (u.viewingPrev ? "  ·  PREV" : "  ·  LIVE");
    if (p->kind == PK_SSTV && !p->mode.empty()) l1 += "  ·  " + p->mode;
    std::string l2 = u.viewingPrev ? "the last picture — RIGHT for the live one" : d.status;
    if (u.zoomI > 0) l2 = std::string("zoom ") + (u.zoomI == 1 ? "1.5" : std::to_string((int)picZooms()[(size_t)u.zoomI])) + "×  ·  D-pad to look around  ·  press to zoom on";
    const bool keys = u.zoomI == 0;
    picFoot(c, a, l1, l2, keys);
    if (keys) {
        float x = (float)a.W - 12;
        std::vector<PicKey> ks;
        if (u.prev) ks.push_back({"dec.full.prev", u.viewingPrev ? "LIVE" : "PREV", 60, u.viewingPrev});
        if (galEnabled(a.eng_->port())) ks.push_back({"dec.full.gal", "GALLERY", 62, false});
        for (size_t i = ks.size(); i-- > 0;) {
            const float w = decKeyW(c, ks[i].label) + 6;
            x -= w;
            if (decKey(c, ks[i].id, {x, (float)a.H - 44, w, 32}, ks[i].label, ks[i].on)) {
                if (ks[i].act == 62) { u.full = 0; galOpen(a, d.view == "sstv" ? 1 : 0); return; }
                decPicKeyPress(a, ks[i].act, d.view);
            }
            x -= 6;
        }
    }
    if (c.nav.backPressed()) { u.full = 0; a.nav_.setFocus("dec", "dec.pic"); }
}

// ── the gallery sheet ────────────────────────────────────────────────────────────────────────────────────────────────
inline void galDrawViewer(App& a, Ctx& c) {
    GalUi& g = galUi(); PicUi& u = picUi();
    const GalItem& it = g.items[(size_t)g.viewing];
    // ★ LEFT / RIGHT at FIT step through the page: say where in it this one is
    const std::string l1 = (it.title.empty() ? upper(it.k) : it.title) + (g.items.size() > 1 ? "  ·  " + std::to_string(g.viewing + 1) + " of " + std::to_string(g.items.size()) : "");
    const std::string meta = galFreq(it.hz) + (it.m.empty() ? "" : " " + upper(it.m)) + " · " + galWhen(it.t) + " · " + galOrigin(it) + (it.partial ? " · PARTIAL" : "");
    if (g.pic) {
        picFullDraw(a, c, *g.pic, 0);
        int lr; bool pr;
        picFullNav(c, "gal.full", lr, pr);
        if (lr && g.viewing + lr >= 0 && g.viewing + lr < (int)g.items.size()) { galOpenItem(a, g.viewing + lr); return; }
    } else {
        c.g.rect(0, 0, (float)a.W, (float)a.H, rgb(0));
        textIn(c, c.f.body, g.picLoading ? "Loading the picture…" : g.picErr, {0, 0, (float)a.W, (float)a.H - 60}, g.picLoading ? th::dim : th::white68);
        c.nav.item("gal.full", {0, 0, 640, 440});
    }
    const bool keys = u.zoomI == 0;
    std::string l2 = meta;
    if (!keys) l2 = "zoomed  ·  D-pad to look around  ·  press to zoom on";
    picFoot(c, a, l1, l2, keys);
    if (keys) {
        // ★ DELETE: a second press within 4 s deletes (the first says so) — the card's file goes
        const bool armed = g.armedAt > 0 && nowSec() - g.armedAt < 4;
        const std::string dl = g.deleting ? "DELETING…" : armed ? "PRESS AGAIN TO DELETE" : "DELETE";
        const float w = decKeyW(c, dl) + 10;
        const Rect r{(float)a.W - 12 - w, (float)a.H - 44, w, 32};
        if (armed) c.g.rrect(r.x, r.y, r.w, r.h, 6, rgb(0xe05050, 0.35f));
        if (decKey(c, "gal.del", r, dl, armed) && !g.deleting) {
            if (armed) {
                g.deleting = true; g.armedAt = 0;
                GalReq q; q.type = 4; q.f = it.f; q.path = "/handheld/gallery/" + galPct(it.f);
                galWorker().post(q, a.eng_->port());
            } else g.armedAt = nowSec();
        }
    }
    if (c.nav.backPressed()) { g.focusAfter = "gal.i." + std::to_string(g.viewing); g.viewing = -1; u.zoomI = 0; a.nav_.forget("dec"); }
}
inline void galDrawSheet(App& a, Ctx& c) {
    GalUi& g = galUi();
    if (g.viewing >= 0 && g.viewing < (int)g.items.size()) { galDrawViewer(a, c); return; }
    const float W = (float)a.W, H = (float)a.H;
    c.g.rect(0, 0, W, H, rgb(0x0c0802));
    // the head: GALLERY · WEFAX | SSTV · the page
    c.tc.draw(c.g, c.f.bodyB, "GALLERY", 14, 10, th::btnText);
    const float tx = 30 + (float)c.tc.width(c.f.bodyB, "GALLERY");
    const char* tabs[] = {"WEFAX", "SSTV"};
    float x = tx;
    for (int i = 0; i < 2; i++) {
        const float w = decKeyW(c, tabs[i]) + 8;
        BtnStyle st; st.font = &c.f.tiny; st.radius = 6; st.lit = g.tab == i;
        if (button(c, std::string("gal.tab.") + tabs[i], {x, 7, w, 28}, tabs[i], st) && g.tab != i) { g.tab = i; g.page = 0; galList(a.eng_->port()); }
        x += w + 6;
    }
    const std::string pg = g.loading ? "loading…" : g.total ? std::to_string(g.total) + (g.total == 1 ? " picture" : " pictures") + (g.pages > 1 ? " · page " + std::to_string(g.page + 1) + " of " + std::to_string(g.pages) : "") : "";
    c.tc.draw(c.g, c.f.small, pg, x + 8, 21 - c.f.small.lineHeight() / 2.f, th::dim);
    hints(c, W - 96, 21, {{FACE_RIGHT, "close"}});
    c.g.rect(8, 42, W - 16, 1, th::barBorder);
    // the grid
    const float gx = 12, gy = 50, cw = 154, chh = 122;
    if (g.items.empty()) {
        decNone(c, {gx, gy, W - 24, 200}, g.loading ? "loading…" : g.note);
    }
    if (!g.focusAfter.empty()) { a.nav_.setFocus("dec", g.focusAfter); g.focusAfter.clear(); }
    for (size_t i = 0; i < g.items.size(); i++) {
        const GalItem& it = g.items[i];
        const int col = (int)i % 4, row = (int)i / 4;
        const Rect cell{gx + col * cw, gy + row * chh, cw - 8, chh - 8};
        const Nav::Hit h = c.nav.item("gal.i." + std::to_string(i), cell);
        c.g.rrect(cell.x, cell.y, cell.w, cell.h, 8, h.focused ? th::activeBg : th::btnBg);
        c.g.rframe(cell.x, cell.y, cell.w, cell.h, 8, 1, th::btnBorder);
        const Rect tr{cell.x + 3, cell.y + 3, cell.w - 6, (float)kGalThumbH};
        GalThumb* t = galThumb(a, it.f);
        if (t->state == 2 && t->tex) dabDrawTex(c.g, DabTex{t->tex, t->w, t->h}, tr.x, tr.y, tr.w, tr.h);
        else textIn(c, c.f.tiny, t->state == 3 ? "no preview" : "…", tr, th::dimmer);
        char when[32]; { const time_t tt = (time_t)it.t; struct tm gm{}; gmtime_r(&tt, &gm); std::snprintf(when, sizeof when, "%02d-%02d %02d:%02d", gm.tm_mon + 1, gm.tm_mday, gm.tm_hour, gm.tm_min); }
        // the caption: when (UTC) and the dial, kHz (the unit is the page's — every item here is HF)
        char fq[24]; std::snprintf(fq, sizeof fq, it.hz < 30e6 ? "%.1f" : "%.3f", it.hz < 30e6 ? it.hz / 1e3 : it.hz / 1e6);
        const float fw = (float)c.tc.width(c.f.tiny, fq);
        c.tc.draw(c.g, c.f.tiny, decFit(c, c.f.tiny, when, (int)(cell.w - fw - 18)), cell.x + 6, tr.b() + 1, th::white68);
        c.tc.draw(c.g, c.f.tiny, fq, cell.r() - 6, tr.b() + 1, th::dim, 2);
        if (h.focused) focusRing(c, cell, 8);
        if (h.pressed) galOpenItem(a, (int)i);
    }
    // the pager
    if (g.pages > 1) {
        BtnStyle st; st.font = &c.f.small; st.radius = 7;
        st.disabled = g.page == 0;
        if (button(c, "gal.pg.prev", {W / 2 - 160, H - 40, 150, 32}, "< NEWER", st)) { g.page--; galList(a.eng_->port()); a.nav_.setFocus("dec", "gal.pg.prev"); }
        st.disabled = g.page + 1 >= g.pages;
        if (button(c, "gal.pg.next", {W / 2 + 10, H - 40, 150, 32}, "OLDER >", st)) { g.page++; galList(a.eng_->port()); a.nav_.setFocus("dec", "gal.pg.next"); }
    } else if (g.items.empty()) c.nav.item("gal.none", {gx, gy, W - 24, 200});   // ★ something to hold the highlight
    if (c.nav.backPressed()) galClose(a);
}

}  // namespace hh
