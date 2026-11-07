// dab_box.h — the DAB DECODER BOX on a 640×480 handheld, the pill's DAB meter, and the keys DAB changes on the island.
// State and actions are dab_state.h; this file only draws, and registers what the D-pad can land on.
//
// ★★★ THE DECODER BOX RULE (the web's [data-navbox], gamepad.ts): the box is ONE stop for the D-pad. ENTER (bottom face)
//     hands it the pad — the station rows, the pane keys — and BACK (right face) gives it back to the island. In DAB the
//     box takes the pad by itself on entry (the web's autoGrab: "the station list IS the tuning"). Grabbed = App::S_DAB
//     on the sheet stack (layer "dab"); the box is drawn in the deck pass either way, so the island stays on screen.
//     Inside the box BACK first returns a sub-pane (SIGNAL, BLOCKS, SCAN) to the station list, then releases.
// ★★ THREE LAYOUTS, for Stuart to choose (VIBE_HHUI_DAB_LAYOUT=A|B|C, default C):
//     A  SHEET   — the box takes the waterfall above the island (y 156–270); spectrum, scale and bands untouched.
//                  Now playing at the left, three station rows at the right.
//     B  STRIP   — a one-line now-playing strip over the waterfall; the list opens ON DEMAND (ENTER) as a tall panel
//                  over the spectrum and waterfall, and goes away again on BACK. The most waterfall when not browsing.
//     C  PAGE    — a DAB page: the spectrum a thin strip at the top (bands hidden), then the slideshow + now playing at
//                  the left and six station rows at the right. ★ The default (why: see the report / docs).
//     All three leave the strip directly above the island (y ~274–310) to the VTS bar, which carries the radio text.
// ★★ ONLY THE PLAYING STATION'S RADIO TEXT (Stuart, 2026-11-07) — the rows never show another service's `dls`.
// ★ Text: Atkinson Hyperlegible (the Fonts), as everywhere on the R36S.
// ★ Logos and the MOT slideshow: dab_image.h (SDL2_image, JPEG + PNG only, fetched and decoded off the UI thread, bounded).
//   The slide sits in layout C's slide area; pressing the station that is already playing (or the slide itself) shows it
//   full screen (dabDrawFull). A station without a picture gets its initials on a tile.
// ★ The SCOPES (constellation + impulse response) sit at the left of the SIGNAL pane; `dab_scopes` on/off follows the
//   pane being on screen.
#pragma once
#include "dab_state.h"

namespace hh {

// ── small helpers ─────────────────────────────────────────────────────────────────────────────────────────────────────

/** `s` cut to `maxW` pixels with "…" (cached: each try would otherwise make a texture). */
inline std::string dabFit(Ctx& c, const Font& f, const std::string& s, int maxW) {
    DabUi& u = dabUi();
    const std::string key = std::to_string((uintptr_t)f.get()) + "|" + std::to_string(maxW) + "|" + s;
    auto it = u.fit.find(key);
    if (it != u.fit.end()) return it->second;
    if (u.fit.size() > 3000) u.fit.clear();
    std::string out = s;
    if (c.tc.width(f, s) > maxW) {
        size_t n = s.size();
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
            out = s.substr(0, n) + "…";
            if (c.tc.width(f, out) <= maxW) break;
        }
    }
    return u.fit[key] = out;
}
inline std::string dabTrim(std::string s) {
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
    size_t i = 0; while (i < s.size() && (unsigned char)s[i] <= ' ') i++;
    return s.substr(i);
}
/** A station's picture: the logo the multiplex sent (dab_image.h), else its initials on a colour of its own. */
inline void dabLogoTile(Ctx& c, Rect r, const std::string& label, double sid) {
    const float rad = std::min(8.f, r.w / 5);
    if (const DabTex* t = dabImages().logo((uint32_t)sid, c.t)) {
        c.g.rrect(r.x, r.y, r.w, r.h, rad, rgb(0xffffff, 0.92f));   // ★ logos are drawn for light backgrounds
        dabDrawTex(c.g, *t, r.x + 2, r.y + 2, r.w - 4, r.h - 4);
        return;
    }
    static const unsigned pal[] = {0x7a3b00, 0x00507a, 0x5a007a, 0x2f6a00, 0x7a0030, 0x006a5a, 0x6a5a00, 0x30307a};
    const unsigned h = (unsigned)sid * 2654435761u;
    c.g.rrect(r.x, r.y, r.w, r.h, rad, rgb(pal[(h >> 16) % 8]));
    c.g.rframe(r.x, r.y, r.w, r.h, rad, 1, rgb(0xffffff, 0.22f));
    std::string ini;
    const std::string t = dabTrim(label);
    bool start = true;
    for (size_t i = 0; i < t.size() && ini.size() < 2; i++) {
        const unsigned char ch = (unsigned char)t[i];
        if (ch == ' ') { start = true; continue; }
        if (start && ch < 0x80 && std::isalnum(ch)) ini += (char)std::toupper(ch);
        start = false;
    }
    if (ini.empty()) ini = "D";
    const Font& f = r.h >= 56 ? c.f.big : r.h >= 34 ? c.f.bodyB : c.f.tiny;
    textIn(c, f, ini, r, th::white);
}
/** The MOT slideshow (TS 101 499): the picture the playing service is sending, fitted to `r`; while none has arrived,
 *  the station's logo and "no slideshow". */
inline void dabSlide(Ctx& c, Rect r, const Json* rep, const Json* play, bool focused) {
    c.g.rrect(r.x, r.y, r.w, r.h, 8, rgb(0x000000, 0.85f));
    const DabTex* t = dabImages().slide();
    if (t && play) {
        c.g.flush();
        c.g.clip(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
        dabDrawTex(c.g, *t, r.x + 1, r.y + 1, r.w - 2, r.h - 2);
        c.g.unclip();
    } else if (play) {
        const bool coming = rep && (*rep)["slide"].kind == Json::Obj;
        dabLogoTile(c, {r.cx() - 36, r.cy() - 44, 72, 72}, (*play)["label"].str(), (*play)["sid"].num(0));
        textIn(c, c.f.tiny, coming ? "slideshow…" : "no slideshow", {r.x, r.b() - 24, r.w, 16}, th::dimmer);
    } else {
        textIn(c, c.f.small, "DAB", r, th::dimmer);
    }
    c.g.rframe(r.x, r.y, r.w, r.h, 8, 1, th::btnBorder);
    if (focused) focusRing(c, r, 8);
}
/** ★ THE SCOPES (the web's dabDrawScopes): the constellation — the carriers' differential phases, four clouds when the
 *  multiplex is clean — and the impulse response of the channel (the PRS correlation, 128 bins, the main path at bin 8;
 *  everything right of it is an echo). Points and bars, a few hundred quads. Both come in every report this client reads. */
inline void dabScopes(Ctx& c, const Json* rep, Rect cr, Rect ir) {
    c.g.rrect(cr.x, cr.y, cr.w, cr.h, 6, rgb(0, 0.8f));
    c.g.rframe(cr.x, cr.y, cr.w, cr.h, 6, 1, th::btnBorder);
    c.g.rect(cr.cx(), cr.y + 3, 1, cr.h - 6, rgb(0xffffff, 0.15f));
    c.g.rect(cr.x + 3, cr.cy(), cr.w - 6, 1, rgb(0xffffff, 0.15f));
    c.g.rrect(ir.x, ir.y, ir.w, ir.h, 6, rgb(0, 0.8f));
    c.g.rframe(ir.x, ir.y, ir.w, ir.h, 6, 1, th::btnBorder);
    c.tc.draw(c.g, c.f.tiny, "CONSTELLATION", cr.x + 5, cr.y + 2, th::dimmer);
    c.tc.draw(c.g, c.f.tiny, "IMPULSE", ir.x + 5, ir.y + 2, th::dimmer);
    if (!rep) return;
    const auto& iq = (*rep)["iq"].a;
    const float k = std::min(cr.w, cr.h) / 2 / 70.f;   // the web: ±70 counts fill a 120 px box (×0.85)
    const Rgba dot = rgb(0xffe566, 0.9f);
    for (size_t i = 0; i + 1 < iq.size(); i += 2) {
        const float x = cr.cx() + (float)iq[i].num(0) * k, y = cr.cy() - (float)iq[i + 1].num(0) * k;
        if (x > cr.x && x < cr.r() && y > cr.y && y < cr.b()) c.g.rect(x - 1, y - 1, 2, 2, dot);
    }
    const auto& v = (*rep)["ir"].a;
    if (!v.empty()) {
        const float bw = (ir.w - 8) / (float)v.size(), base = ir.b() - 4, hh = ir.h - 20;
        for (float gx = 0; gx <= v.size(); gx += 16) c.g.rect(ir.x + 4 + gx * bw, ir.y + 16, 1, hh, rgb(0xffffff, 0.10f));
        for (size_t i = 0; i < v.size(); i++) {
            const float h = std::max(0.f, std::min(1.f, (float)v[i].num(0) / 255.f)) * hh;
            if (h >= 0.5f) c.g.rect(ir.x + 4 + i * bw, base - h, std::max(1.f, bw), h, rgb(0xffa000));
        }
        c.g.rect(ir.x + 4 + 8 * bw, ir.y + 16, 1, hh, rgb(0xffffff, 0.5f));   // the main path: right of it, echoes
    }
}
/** The three bars (signal-strength style), `level` of them lit, coloured by the verdict. */
inline void dabBars(Ctx& c, float x, float yBot, float h, int level) {
    const Rgba on = level >= 3 ? rgb(0x35e08a) : level == 2 ? rgb(0xe8a02a) : rgb(0xe8462a);
    for (int i = 0; i < 3; i++) {
        const float bh = h * (0.45f + 0.275f * i);
        c.g.rrect(x + i * 9, yBot - bh, 6, bh, 2, i < level ? on : th::meterTrack);
    }
}
/** The multiplex in words: "12D · VibeSDR Bench · 6 stations" (or the block alone while it is being read). */
inline std::string dabHeadLine(const RadioState& s) {
    const Json* r = dabReport(s);
    const int b = dabShownBlock();
    std::string blk = b >= 0 ? kDabBlocks[b].name : "DAB";
    if (!r) return blk + (dabUi().stepPending >= 0 ? "" : " · tuning…");
    if (!(*r)["locked"].boolean()) return blk + " · searching…";
    const std::string lbl = dabTrim((*r)["label"].str());
    const size_t n = (*r)["services"].size();
    if (!n) return blk + (lbl.empty() ? "" : " · " + lbl) + " · reading the multiplex…";
    return blk + (lbl.empty() ? "" : " · " + lbl) + " · " + std::to_string(n) + (n == 1 ? " station" : " stations");
}

// ── the island: the pill's DAB meter, the BLOCK key ──────────────────────────────────────────────────────────────────

/** ★★★ THE PILL IN DAB (mobile.ts paintDab): the gradient and the squelch line go — power in the passband is not whether
 *  a multiplex can be HEARD. The frequency box reads the block's centre and name (pressing it opens BLOCKS), the mode chip
 *  says DAB with the SERVICE's stereo light, and the reading becomes the meter: three bars, the verdict, what you will
 *  hear, and MER (DAB's SNR) — "Multiplex weak · No or heavily broken audio · 14 % frames lost". Draws the whole pill and
 *  registers freq / mode / snr like the ordinary pill, so the highlight moves the same. Returns false outside DAB. */
inline bool dabDrawPill(App& a, Ctx& c, const RadioState& s, Rect pill) {
    DabUi& u = dabUi();
    if (!u.on) return false;
    c.g.rrect(pill.x, pill.y, pill.w, pill.h, 9, th::pillBg);
    c.g.rframe(pill.x, pill.y, pill.w, pill.h, 9, 1, th::btnBorder);
    const float bh = pill.h - 12, by = pill.y + 6;
    const int b = dabShownBlock();
    // the frequency box: the block centre (the web's readout follows the multiplex) + its name
    const float unitW = (float)c.tc.width(c.f.unit, "MHz");
    const float fW = (float)c.tc.widthTab(c.f.freq, "888.888");
    const float blkW = (float)c.tc.width(c.f.bodyB, "13F") + 12;
    // ★ the unit and the block name stacked at the right of the number, so the meter keeps its room
    const Rect fb{pill.x + 8, by, 12 + fW + 8 + std::max(unitW, blkW) + 8, bh};
    const Nav::Hit hf = c.nav.item("freq", fb);
    c.g.rrect(fb.x, fb.y, fb.w, fb.h, 7, rgb(0, 0.62f));
    c.g.rframe(fb.x, fb.y, fb.w, fb.h, 7, 1, rgb(0xffffff, 0.38f));
    {
        const float base = fb.cy() + c.f.freq.lineHeight() * 0.5f;
        char f[24];
        if (b >= 0) std::snprintf(f, sizeof f, "%.3f", kDabBlocks[b].hz / 1e6); else std::snprintf(f, sizeof f, "—");
        c.tc.drawTab(c.g, c.f.freq, f, fb.x + 12 + fW / 2, base - c.f.freq.lineHeight(), th::white, 1);
        const float rx = fb.x + 12 + fW + 8, rw = std::max(unitW, blkW);
        c.tc.draw(c.g, c.f.unit, "MHz", rx + rw / 2, fb.y + 1, th::white, 1);
        const Rect bc{rx, fb.y + fb.h / 2, rw, fb.h / 2 - 3};
        // ★ lit while a burst of presses is settling: the label has moved, the radio has not yet
        if (u.stepPending >= 0) c.g.rrect(bc.x, bc.y, bc.w, bc.h, 5, th::onBg);
        textIn(c, c.f.bodyB, b >= 0 ? kDabBlocks[b].name : "—", bc, u.stepPending >= 0 ? th::onText : th::activeTx);
    }
    if (hf.focused) focusRing(c, fb, 7);
    if (hf.pressed) { u.pane = DabUi::P_BLOCKS; u.blkScroll = {}; u.grabNext = true; a.nav_.forget("dab"); }
    // the mode chip: DAB + the playing service's stereo light
    const Json* r = dabReport(s);
    const float stW = 30, modeW = std::max(84.f, (float)c.tc.width(c.f.btn, "DAB") + 24) + stW;
    const Rect mb{fb.r() + 10, by, modeW, bh};
    const Nav::Hit hm = c.nav.item("mode", mb);
    c.g.rrect(mb.x, mb.y, mb.w, mb.h, 7, rgb(0, 0.62f));
    c.g.rframe(mb.x, mb.y, mb.w, mb.h, 7, 1, rgb(0xffffff, 0.38f));
    textIn(c, c.f.btn, "DAB", {mb.x, mb.y, mb.w - stW, mb.h}, th::white);
    if (dabServiceStereo(r)) {
        const float cx = mb.r() - stW / 2 - 6, cy = mb.cy();
        c.g.circle(cx - 5, cy, 7, 1.6f, th::white);
        c.g.circle(cx + 5, cy, 7, 1.6f, th::white);
    }
    if (hm.focused) focusRing(c, mb, 7);
    if (hm.pressed) a.open(App::S_MODE);
    // the meter
    const DabQuality q = u.meter.has() && r ? u.meter.current() : DabQuality{};
    const Rect qb{mb.r() + 10, by, pill.r() - 8 - (mb.r() + 10), bh};
    const Nav::Hit hq = c.nav.item("snr", qb);
    c.g.rrect(qb.x, qb.y, qb.w, qb.h, 7, rgb(0, 0.62f));
    c.g.rframe(qb.x, qb.y, qb.w, qb.h, 7, 1, rgb(0xffffff, 0.38f));
    dabBars(c, qb.x + 10, qb.b() - 9, qb.h - 16, q.level);
    const float tx = qb.x + 44, tw = qb.r() - 8 - tx;
    std::string mer;
    if (q.hasMer) { char m[24]; std::snprintf(m, sizeof m, "MER %d dB", (int)std::lround(q.merDb)); mer = m; }
    const float merW = mer.empty() ? 0.f : (float)c.tc.width(c.f.small, mer) + 8;
    const Rgba lc = q.level >= 3 ? rgb(0x7dffb0) : q.level == 2 ? rgb(0xffc966) : q.level == 1 ? rgb(0xff8a6a) : th::white68;
    const std::string verdict = (float)c.tc.width(c.f.bodyB, q.label) <= tw - merW ? q.label : q.shortL;
    c.tc.draw(c.g, c.f.bodyB, dabFit(c, c.f.bodyB, verdict, (int)(tw - merW)), tx, qb.y + 2, lc);
    if (!mer.empty()) c.tc.draw(c.g, c.f.small, mer, qb.r() - 8, qb.y + 4, th::white, 2);
    const std::string line2 = q.detail.empty() ? q.advice : q.advice + " · " + q.detail;
    c.tc.draw(c.g, c.f.small, dabFit(c, c.f.small, line2, (int)tw), tx, qb.y + 2 + c.f.bodyB.lineHeight() - 2, th::white68);
    if (hq.focused) focusRing(c, qb, 7);
    if (hq.pressed) { u.pane = DabUi::P_SIGNAL; u.sigScroll = {}; u.grabNext = true; a.nav_.forget("dab"); }
    return true;
}

/** STEP means nothing in DAB (the dial steps blocks): the key becomes BLOCK, opening the block picker. Returns false
 *  outside DAB (draw the STEP key as usual). */
inline bool dabDrawStepKey(App& a, Ctx& c, Rect r, BtnStyle st) {
    DabUi& u = dabUi();
    if (!u.on) return false;
    if (button(c, "step", r, "BLOCK", st)) { u.pane = DabUi::P_BLOCKS; u.blkScroll = {}; u.grabNext = true; a.nav_.forget("dab"); }
    return true;
}

// ── the screen: what DAB changes about the spectrum / waterfall rectangles ───────────────────────────────────────────

/** Layout C shrinks the spectrum to a strip and hides the band bar (a DAB block is one flat slab, and zoom is fixed).
 *  Returns false when the ordinary layout stands. */
struct DabScreenRects { bool bands = true; Rect scale, spec, wf; };
inline bool dabScreenRects(const App& a, DabScreenRects& o) {
    const DabUi& u = dabUi();
    if (!u.on || u.layout != DAB_C_PAGE) return false;
    o.bands = false;
    o.scale = {0, 0, (float)a.W, 20};
    o.spec = {0, 20, (float)a.W, 42};
    o.wf = {0, 62, (float)a.W, (float)a.H - 62};
    return true;
}

// ── the box ──────────────────────────────────────────────────────────────────────────────────────────────────────────

inline bool dabGrabbed(const App& a) { for (auto sh : a.sheets_) if (sh == App::S_DAB) return true; return false; }

/** A small triangle (Atkinson Hyperlegible has no ▸ ◂ ▾ ▶, so they are drawn): dir 0 right, 1 left, 2 down. */
inline void dabArrow(Ctx& c, float cx, float cy, float sz, int dir, Rgba col) {
    if (dir == 0) c.g.triangle(cx - sz * 0.4f, cy - sz * 0.5f, cx - sz * 0.4f, cy + sz * 0.5f, cx + sz * 0.5f, cy, col);
    else if (dir == 1) c.g.triangle(cx + sz * 0.4f, cy - sz * 0.5f, cx + sz * 0.4f, cy + sz * 0.5f, cx - sz * 0.5f, cy, col);
    else c.g.triangle(cx - sz * 0.5f, cy - sz * 0.4f, cx + sz * 0.5f, cy - sz * 0.4f, cx, cy + sz * 0.5f, col);
}
/** A pane key in the header; `arrow` 0 = a ► after the label, 1 = a ◄ before it, -1 none. Returns true when pressed. */
inline float dabKeyW(Ctx& c, const std::string& label, int arrow) { return (float)c.tc.width(c.f.tiny, label) + 18 + (arrow >= 0 ? 12 : 0); }
inline bool dabKey(Ctx& c, const std::string& id, Rect r, const std::string& label, bool on = false, int arrow = -1) {
    BtnStyle st; st.font = &c.f.tiny; st.radius = 6; st.on = on;
    if (arrow < 0) return button(c, id, r, label, st);
    const bool hit = button(c, id, r, "", st);
    const Rgba col = on ? th::activeTx : th::btnText;
    const float tw = (float)c.tc.width(c.f.tiny, label), x0 = r.cx() - (tw + 12) / 2;
    if (arrow == 1) { dabArrow(c, x0 + 4, r.cy(), 8, 1, col); c.tc.draw(c.g, c.f.tiny, label, x0 + 12, r.cy() - c.f.tiny.lineHeight() / 2.f, col); }
    else { c.tc.draw(c.g, c.f.tiny, label, x0, r.cy() - c.f.tiny.lineHeight() / 2.f, col); dabArrow(c, x0 + tw + 8, r.cy(), 8, 0, col); }
    return hit;
}

/** The header: the multiplex line at the left, the pane keys at the right. Returns where the keys begin. */
inline void dabHeader(App& a, Ctx& c, const RadioState& s, Rect h, bool withNow) {
    DabUi& u = dabUi();
    const bool mayScan = s.hw("dabScan").boolean();
    const Json& scan = s.msg("dab_scan");
    const bool haveList = scan["found"].size() > 0;
    const bool scanRunning = scan["running"].boolean();
    // keys, right to left: EXIT · SCAN|MULTIPLEXES · BLOCK · SIGNAL|STATIONS
    struct K { std::string id, label; int act; bool on; int arrow; };
    std::vector<K> ks;
    // ★ the label names where the key GOES, so the pane up is readable without pressing it (the web's rule)
    ks.push_back({"dab.k.pane", u.pane == DabUi::P_SIGNAL ? "STATIONS" : "SIGNAL", 0, u.pane == DabUi::P_SIGNAL, u.pane == DabUi::P_SIGNAL ? 1 : 0});
    ks.push_back({"dab.k.blk", "BLOCK", 1, u.pane == DabUi::P_BLOCKS, -1});
    if (!scanRunning && (haveList || mayScan)) ks.push_back({"dab.k.scan", haveList ? "MULTIPLEXES" : "SCAN", 2, u.pane == DabUi::P_SCAN, -1});
    ks.push_back({"dab.k.exit", "EXIT DAB", 3, false, -1});
    float x = h.r() - 4;
    const float kh = h.h - 6;
    std::vector<std::pair<Rect, size_t>> placed;
    for (size_t i = ks.size(); i-- > 0;) {
        const float w = dabKeyW(c, ks[i].label, ks[i].arrow);
        x -= w;
        placed.push_back({{x, h.y + 3, w, kh}, i});
        x -= 6;
    }
    // the line
    c.tc.draw(c.g, c.f.bodyB, "DAB", h.x + 10, h.cy() - c.f.bodyB.lineHeight() / 2.f, th::btnText);
    const float lx = h.x + 10 + c.tc.width(c.f.bodyB, "DAB") + 10;
    c.tc.draw(c.g, c.f.small, dabFit(c, c.f.small, dabHeadLine(s), (int)(x - lx - 4)), lx, h.cy() - c.f.small.lineHeight() / 2.f, th::white68);
    std::reverse(placed.begin(), placed.end());   // ★ registered left to right: a fresh layer never defaults to EXIT
    for (auto& p : placed) {
        const K& k = ks[p.second];
        if (dabKey(c, k.id, p.first, k.label, k.on, k.arrow)) {
            if (k.act == 0) { u.pane = u.pane == DabUi::P_SIGNAL ? DabUi::P_STATIONS : DabUi::P_SIGNAL; u.sigScroll = {}; }
            else if (k.act == 1) { u.pane = u.pane == DabUi::P_BLOCKS ? DabUi::P_STATIONS : DabUi::P_BLOCKS; u.blkScroll = {}; }
            else if (k.act == 2) {
                if (!haveList && mayScan) { u.scanMine = true; a.eng_->send("{\"type\":\"dab_scan\",\"on\":1}"); }
                u.pane = DabUi::P_SCAN; u.scanScroll = {};
            } else if (k.act == 3) { dabLeave(a, s); return; }
        }
    }
    (void)withNow;
}

/** Now playing: the station's tile, its name, the codec line and the PLAYING service's radio text (wrapped). */
inline void dabNowPlayingBlock(Ctx& c, const RadioState& s, Rect r, float tile, int dlsLines) {
    const Json* rep = dabReport(s);
    const Json* p = rep ? dabPlaying(*rep) : nullptr;
    if (!p) {
        const char* t = !rep ? "Tuning the multiplex…" : !(*rep)["locked"].boolean() ? "Searching for a multiplex…"
                      : (*rep)["services"].size() ? "Pick a station" : "Reading the multiplex…";
        c.tc.draw(c.g, c.f.body, t, r.x, r.y + 4, th::dim);
        return;
    }
    float x = r.x;
    if (tile > 0) { dabLogoTile(c, {r.x, r.y, tile, tile}, (*p)["label"].str(), (*p)["sid"].num(0)); x += tile + 10; }
    const int w = (int)(r.r() - x);
    c.tc.draw(c.g, c.f.bodyB, dabFit(c, c.f.bodyB, dabTrim((*p)["label"].str()), w), x, r.y, th::white);
    float y = r.y + c.f.bodyB.lineHeight();
    {
        std::string cod = (*p)["codec"].str();
        if ((*p)["kbps"].num(0) > 0) cod += " " + std::to_string((int)(*p)["kbps"].num(0)) + " kbit/s";
        const char* pty = dabPty((int)(*p)["pty"].num(0));
        if (*pty && std::strcmp(pty, "None") != 0) cod += std::string(" · ") + pty;
        c.tc.draw(c.g, c.f.small, dabFit(c, c.f.small, cod, w), x, y, th::dim);
        y += c.f.small.lineHeight() + 2;
    }
    // ★★ THE RADIO TEXT IS THE VTS BAR'S (as in the app): the box shows only the "tuning in" line, never a second copy
    const std::string dls = dabUi().waiting;
    if (dls.empty() || dlsLines <= 0) return;
    const Rgba col = !dabUi().waiting.empty() ? th::amber : th::white68;
    const float maxW = tile > 0 && y > r.y + tile ? r.w : (float)w;
    const float lx = tile > 0 && y > r.y + tile ? r.x : x;
    const auto& lines = c.tc.wrap(c.f.small, dls, (int)maxW);
    for (int i = 0; i < (int)lines.size() && i < dlsLines && y + c.f.small.lineHeight() <= r.b() + 2; i++) {
        const bool last = i == dlsLines - 1 && (int)lines.size() > dlsLines;
        c.tc.draw(c.g, c.f.small, last ? dabFit(c, c.f.small, lines[(size_t)i] + " …", (int)maxW) : lines[(size_t)i], lx, y, col);
        y += c.f.small.lineHeight();
    }
}

/** A scrolling column of full-width rows: draws row k with `drawRow(k, rect, focused)` and registers it under
 *  `idPrefix + key(k)`. Returns the pressed row, or -1. ★ Full-width boxes, so the D-pad walks the rows (ui.h pickNext). */
template <class KeyF, class DrawF>
inline int dabRows(Ctx& c, Scroller& sc, Rect view, int n, float rowH, const std::string& idPrefix, KeyF key, DrawF drawRow) {
    int hit = -1;
    sc.contentH = n * rowH;
    c.g.clip(view.x - 1, view.y, view.w + 2, view.h);
    const Rect* fr = nullptr;
    Rect focusR{};
    for (int k = 0; k < n; k++) {
        const Rect rr{view.x, view.y + k * rowH - sc.y, view.w, rowH - 3};
        const Nav::Hit h = c.nav.item(idPrefix + key(k), rr);
        if (rr.b() >= view.y && rr.y <= view.b()) drawRow(k, rr, h.focused);
        if (h.focused) { focusR = rr; fr = &focusR; }
        if (h.pressed) hit = k;
    }
    c.g.unclip();
    if (fr && fr->b() > view.y && fr->y < view.b()) c.g.rframe(fr->x - 1, std::max(view.y, fr->y) - 1, fr->w + 2, std::min(view.b(), fr->b()) - std::max(view.y, fr->y) + 2, 7, 3, th::focus);
    sc.follow(c.nav.focusedRect(), view, 4);
    // a scroll hint: more below / above
    if (sc.contentH > view.h + 1) {
        const float frac = view.h / sc.contentH, top = sc.y / sc.contentH;
        c.g.rrect(view.r() + 2, view.y + top * view.h, 3, std::max(10.f, frac * view.h), 1.5f, th::dim);
    }
    return hit;
}

/** STATIONS: one row per service the receiver can play (the server already drops data / CA / incomplete ones). */
inline void dabPaneStations(App& a, Ctx& c, const RadioState& s, Rect view, float rowH, bool tiles) {
    DabUi& u = dabUi();
    const Json* rep = dabReport(s);
    if (!rep || !(*rep)["services"].size()) {
        const char* t = !rep ? "Tuning…" : !(*rep)["locked"].boolean() ? "Searching for a multiplex…" : "Reading the multiplex…";
        textIn(c, c.f.body, t, view, th::dim);
        c.nav.item("dab.none", view);   // ★ something to hold the highlight, so BACK still releases a box with no rows
        return;
    }
    const auto& svcs = (*rep)["services"].a;
    const double playing = (*rep)["sid"].num(0);
    const int hit = dabRows(c, u.listScroll, view, (int)svcs.size(), rowH, "dab.row.",
        [&](int k) { return std::to_string((long long)svcs[(size_t)k]["sid"].num(0)); },
        [&](int k, Rect rr, bool) {
            const Json& sv = svcs[(size_t)k];
            const bool on = sv["sid"].num(0) == playing && playing > 0;
            c.g.rrect(rr.x, rr.y, rr.w, rr.h, 7, on ? th::activeBg : th::btnBg);
            c.g.rframe(rr.x, rr.y, rr.w, rr.h, 7, 1, on ? th::activeBdr : th::btnBorder);
            float x = rr.x + 8;
            if (tiles) { const float t = rr.h - 8; dabLogoTile(c, {x, rr.y + 4, t, t}, sv["label"].str(), sv["sid"].num(0)); x += t + 8; }
            std::string cod = sv["codec"].str();
            if (sv["kbps"].num(0) > 0) cod += " " + std::to_string((int)sv["kbps"].num(0)) + "k";
            const float cw = (float)c.tc.width(c.f.small, cod);
            c.tc.draw(c.g, c.f.small, cod, rr.r() - 10, rr.cy() - c.f.small.lineHeight() / 2.f, on ? th::activeTx : th::dim, 2);
            if (on) { c.g.triangle(x, rr.cy() - 6, x, rr.cy() + 6, x + 9, rr.cy(), th::activeTx); x += 14; }
            const std::string nm = dabTrim(sv["label"].str());
            c.tc.draw(c.g, c.f.body, dabFit(c, c.f.body, nm.empty() ? "(unnamed)" : nm, (int)(rr.r() - 22 - cw - x)), x,
                      rr.cy() - c.f.body.lineHeight() / 2.f, on ? th::activeTx : th::btnText);
        });
    if (hit >= 0) {
        const double sid = svcs[(size_t)hit]["sid"].num(0);
        // ★ the station already playing, pressed again: its slideshow full screen (when it has one) — re-asking for the
        //   service it is already on would do nothing a listener could hear
        if (sid == playing && playing > 0 && dabImages().slide()) u.slideFull = true;
        else dabPickService(a, sid);
    }
}

/** SIGNAL: the advanced figures, one row each (the web's signal pane, without the scopes — see the report). */
inline void dabPaneSignal(App& a, Ctx& c, const RadioState& s, Rect view) {
    DabUi& u = dabUi();
    const Json* rep = dabReport(s);
    std::vector<std::pair<std::string, std::string>> rows;
    auto f1 = [](const char* fmt, double v) { char b[64]; std::snprintf(b, sizeof b, fmt, v); return std::string(b); };
    if (!rep) rows.push_back({"Receiver", "tuning…"});
    else {
        const Json& r = *rep;
        const Json* p = dabPlaying(r);
        const DabQuality& q = u.meter.current();
        rows.push_back({"Reception", q.label + (q.detail.empty() ? "" : " · " + q.detail)});
        const std::string ens = dabTrim(r["label"].str());
        char eid[48]; std::snprintf(eid, sizeof eid, "EId %04X", (unsigned)r["eid"].num(0));
        rows.push_back({"Ensemble", (ens.empty() ? std::string("—") : ens) + (r["eid"].num(0) > 0 ? std::string(" · ") + eid : "")});
        rows.push_back({"Block", r["channel"].str() + " · " + f1("%.3f MHz", r["centreHz"].num(0) / 1e6)});
        rows.push_back({"Lock", r["locked"].boolean() ? "locked" + f1(" · offset %.0f Hz", r["offsetHz"].num(0)) : "searching"});
        if (r.has("mer")) rows.push_back({"MER", f1("%.1f dB", r["mer"].num(0))});
        if (r.has("fibRate")) rows.push_back({"FIC", f1("%.1f %% good", r["fibRate"].num(0) * 100) + " (" + std::to_string((long)r["fibOk"].num(0)) + " / " + std::to_string((long)r["fibTotal"].num(0)) + ")"});
        if (r["sid"].num(0) > 0 && r.has("mscBer")) rows.push_back({"Errors before Viterbi", f1("%.2f %%", r["mscBer"].num(0) * 100)});
        rows.push_back({"Null depth", f1("%.1f dB", r["nullDepthDb"].num(0))});
        if (p) {
            char sid[24]; std::snprintf(sid, sizeof sid, "SId %04X", (unsigned)(*p)["sid"].num(0));
            rows.push_back({"Service", dabTrim((*p)["label"].str()) + " · " + sid});
            rows.push_back({"Codec", r["codecDetail"].str().empty() ? (*p)["codec"].str() : r["codecDetail"].str()});
            rows.push_back({"Sub-channel", std::to_string((int)(*p)["subch"].num(0)) + " · " + (*p)["prot"].str() + " · CU " +
                                           std::to_string((int)(*p)["cuStart"].num(0)) + "+" + std::to_string((int)(*p)["cuSize"].num(0))});
            const char* pty = dabPty((int)(*p)["pty"].num(0));
            if (*pty) rows.push_back({"Programme type", pty});
            if ((*p)["codec"].str() == "DAB+") {
                rows.push_back({"Super frames", std::to_string((long)r["sfOk"].num(0)) + " / " + std::to_string((long)r["sfTried"].num(0)) + " OK"});
                if (r.has("auIn")) rows.push_back({"Audio units lost", std::to_string((long)r["auBad"].num(0)) + " / " + std::to_string((long)r["auIn"].num(0))});
                rows.push_back({"Reed-Solomon", std::to_string((long)r["rsFixed"].num(0)) + " fixed · " + std::to_string((long)r["rsLost"].num(0)) + " lost"});
            } else {
                rows.push_back({"Layer II frames", std::to_string((long)r["mp2Bad"].num(0)) + " bad / " + std::to_string((long)r["mp2In"].num(0))});
                if (r.has("scfConcealed")) rows.push_back({"Concealed groups", std::to_string((long)r["scfConcealed"].num(0))});
            }
        }
        rows.push_back({"Frames erased", std::to_string((long)r["erased"].num(0)) + " / " + std::to_string((long)r["frames"].num(0))});
        for (auto& t : r["tii"].a) {
            std::string v = t["site"].str().empty() ? f1("TII %.0f", t["main"].num(0)) + f1("/%.0f", t["sub"].num(0)) : t["site"].str();
            if (!t["area"].str().empty()) v += ", " + t["area"].str();
            if (t.has("km")) v += f1(" · %.0f km", t["km"].num(0));
            v += f1(" · %.1f dB", t["db"].num(0));
            rows.push_back({"Transmitter", v});
        }
        if (r.has("utc")) rows.push_back({"Multiplex time", r["utc"].str() + " UTC"});
        if (r["announce"].size()) {
            std::string an;
            for (auto& x : r["announce"].a) for (auto& t : x["types"].a) an += (an.empty() ? "" : ", ") + t.str();
            if (!an.empty()) rows.push_back({"Announcement", an + " on air (the radio does not switch)"});
        }
    }
    // ★ the scopes at the left of the figures (stacked when the pane is tall, side by side when it is short)
    Rect rowsV = view;
    if (view.h >= 150) {
        const float sq = std::min(view.h - 52, 140.f);
        dabScopes(c, rep, {view.x, view.y, sq, sq}, {view.x, view.y + sq + 4, std::max(sq, 168.f), view.h - sq - 4});
        rowsV = {view.x + std::max(sq, 168.f) + 10, view.y, view.w - std::max(sq, 168.f) - 10, view.h};
    } else {
        const float sq = view.h;
        dabScopes(c, rep, {view.x, view.y, sq, sq}, {view.x + sq + 4, view.y, 140, sq});
        rowsV = {view.x + sq + 154, view.y, view.w - sq - 154, view.h};
    }
    const float kw = std::min(210.f, rowsV.w * 0.42f);
    view = rowsV;
    dabRows(c, u.sigScroll, view, (int)rows.size(), 26, "dab.sig.", [](int k) { return std::to_string(k); },
        [&](int k, Rect rr, bool focused) {
            if (k % 2 == 0) c.g.rect(rr.x, rr.y, rr.w, rr.h + 3, rgb(0xffa000, 0.05f));
            c.tc.draw(c.g, c.f.small, rows[(size_t)k].first, rr.x + 8, rr.cy() - c.f.small.lineHeight() / 2.f, th::dim);
            c.tc.draw(c.g, c.f.small, dabFit(c, c.f.small, rows[(size_t)k].second, (int)(rr.w - kw - 16)), rr.x + kw, rr.cy() - c.f.small.lineHeight() / 2.f, th::white);
        });
}

/** BLOCKS: every Band III block, the one on air lit; a press tunes there at once (dabGoBlock). */
inline void dabPaneBlocks(App& a, Ctx& c, const RadioState& s, Rect view) {
    DabUi& u = dabUi();
    const int cols = view.w > 500 ? 7 : 6;
    const float gap = 6, cw = (view.w - 8 - gap * (cols - 1)) / cols, ch = 30;
    const int rowsN = (kDabBlockN + cols - 1) / cols;
    u.blkScroll.contentH = rowsN * (ch + gap);
    const int cur = dabShownBlock();
    c.g.clip(view.x - 6, view.y, view.w + 12, view.h);
    Rect fr{}; bool haveF = false;
    for (int i = 0; i < kDabBlockN; i++) {
        const Rect b{view.x + (i % cols) * (cw + gap), view.y + (i / cols) * (ch + gap) - u.blkScroll.y, cw, ch};
        BtnStyle st; st.font = &c.f.bodyB; st.radius = 6; st.lit = i == cur;
        const Nav::Hit h = c.nav.item(std::string("dab.blk.") + kDabBlocks[i].name, b);
        c.g.rrect(b.x, b.y, b.w, b.h, 6, st.lit ? th::onBg : th::btnBg);
        c.g.rframe(b.x, b.y, b.w, b.h, 6, 1, st.lit ? th::onBg : th::btnBorder);
        textIn(c, c.f.bodyB, kDabBlocks[i].name, b, st.lit ? th::onText : th::btnText);
        if (h.focused) { fr = b; haveF = true; }
        if (h.pressed) { dabGoBlock(a, i); u.pane = DabUi::P_STATIONS; }
    }
    c.g.unclip();
    if (haveF) {
        if (fr.b() > view.y && fr.y < view.b()) c.g.rframe(fr.x - 4, fr.y - 4, fr.w + 8, fr.h + 8, 8, 3, th::focus);
        // the highlighted block's frequency, where the person is looking
    }
    if (a.nav_.focus("dab").empty() && cur >= 0) a.nav_.setFocus("dab", std::string("dab.blk.") + kDabBlocks[cur].name);
    u.blkScroll.follow(c.nav.focusedRect(), view, 4);
}

/** SCAN / MULTIPLEXES: the band scan (dabBandScan.ts): the progress line, then one row per ensemble found; a row opens
 *  to "Tune to <block>" + its stations; a press tunes (which also ends a running scan on the server). */
inline void dabPaneScan(App& a, Ctx& c, const RadioState& s, Rect view) {
    DabUi& u = dabUi();
    const Json& j = s.msg("dab_scan");
    const bool running = j["running"].boolean();
    const bool mayScan = s.hw("dabScan").boolean();
    // the line (dabScanLine)
    std::string line;
    const size_t ens = j["found"].size();
    size_t st = 0; for (auto& h : j["found"].a) st += h["services"].size();
    auto plural = [](size_t n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); };
    if (j.isNull()) line = "No band scan yet.";
    else if (j.has("error")) line = j["error"].str();
    else if (running) line = "Scanning " + std::to_string((int)j["done"].num(0)) + " / " + std::to_string((int)j["total"].num(0)) + " blocks"
                           + (j["current"].str().empty() ? "" : " · " + j["current"].str()) + (ens ? " — " + plural(ens, "ensemble") + " so far" : "");
    else if (j["finished"].boolean() || ens) line = ens ? plural(ens, "ensemble") + ", " + plural(st, "station") : "No DAB found on any block — check the aerial.";
    float y = view.y;
    const float btnW = 120;
    const bool showStop = running && mayScan, showAgain = !running && mayScan;
    c.tc.draw(c.g, c.f.small, dabFit(c, c.f.small, line, (int)(view.w - ((showStop || showAgain) ? btnW + 10 : 0))), view.x + 4, y + 4, th::white68);
    if (showStop && dabKey(c, "dab.scan.stop", {view.r() - btnW, y, btnW, 26}, "CANCEL")) a.eng_->send("{\"type\":\"dab_scan\",\"on\":0}");
    if (showAgain && dabKey(c, "dab.scan.again", {view.r() - btnW, y, btnW, 26}, "SCAN AGAIN")) { u.scanMine = true; a.eng_->send("{\"type\":\"dab_scan\",\"on\":1}"); }
    y += 30;
    if (running && j["total"].num(0) > 0) {
        c.g.rrect(view.x + 4, y, view.w - 8, 6, 3, th::meterTrack);
        c.g.rrect(view.x + 4, y, (view.w - 8) * (float)(j["done"].num(0) / j["total"].num(1)), 6, 3, th::amber);
        y += 12;
    }
    // the rows: ensembles, and the opened one's "Tune to" + stations, as one flat list
    struct R { int kind; const Json* h; const Json* sv; };
    std::vector<const Json*> hits;
    for (auto& h : j["found"].a) hits.push_back(&h);
    std::sort(hits.begin(), hits.end(), [](const Json* x, const Json* y2) { return (*x)["channel"].num(0) < (*y2)["channel"].num(0); });
    std::vector<R> rows;
    const bool locked = running && j["lock"].boolean();   // ★ an owner's scan holds the radio: the list is read-only
    for (auto* h : hits) {
        rows.push_back({0, h, nullptr});
        if (u.scanOpen == (*h)["block"].str()) {
            if (!locked) rows.push_back({1, h, nullptr});
            for (auto& sv : (*h)["services"].a) rows.push_back({2, h, &sv});
        }
    }
    const Rect lv{view.x, y, view.w - 6, view.b() - y};
    if (rows.empty()) { c.nav.item("dab.none", lv); return; }
    const int hit = dabRows(c, u.scanScroll, lv, (int)rows.size(), 30, "dab.scan.",
        [&](int k) { const R& r = rows[(size_t)k]; return (*r.h)["block"].str() + "." + std::to_string(r.kind) + "." + (r.sv ? std::to_string((long long)(*r.sv)["sid"].num(0)) : ""); },
        [&](int k, Rect rr, bool) {
            const R& r = rows[(size_t)k];
            const Json& h = *r.h;
            if (r.kind == 0) {
                c.g.rrect(rr.x, rr.y, rr.w, rr.h, 7, th::btnBg);
                c.g.rframe(rr.x, rr.y, rr.w, rr.h, 7, 1, th::btnBorder);
                const double fib = std::max(0.0, std::min(1.0, h["fib"].num(0))), mer = h["mer"].num(0);
                const int byF = fib >= dabq::FIB_STRONG ? 3 : fib >= dabq::FIB_WEAK ? 2 : 1;
                const int byM = mer > 0 ? (mer >= dabq::MER_STRONG ? 3 : mer >= dabq::MER_WEAK ? 2 : 1) : 3;
                const int lvl = std::min(byF, byM);
                const char* word = lvl == 3 ? "STRONG" : lvl == 2 ? "MODERATE" : "WEAK";
                c.tc.draw(c.g, c.f.bodyB, h["block"].str(), rr.x + 10, rr.cy() - c.f.bodyB.lineHeight() / 2.f, th::activeTx);
                const float qw = (float)c.tc.width(c.f.tiny, word);
                c.tc.draw(c.g, c.f.tiny, word, rr.r() - 30, rr.cy() - c.f.tiny.lineHeight() / 2.f, lvl == 3 ? rgb(0x7dffb0) : lvl == 2 ? rgb(0xffc966) : rgb(0xff8a6a), 2);
                dabArrow(c, rr.r() - 16, rr.cy(), 9, u.scanOpen == h["block"].str() ? 2 : 0, th::dim);
                char sub[64]; std::snprintf(sub, sizeof sub, "  %.3f MHz · %zu station%s", h["hz"].num(0) / 1e6, h["services"].size(), h["services"].size() == 1 ? "" : "s");
                const std::string nm = dabTrim(h["ensemble"].str());
                const float nx = rr.x + 62;
                const std::string full = (nm.empty() ? std::string("Unnamed ensemble") : nm);
                const int room = (int)(rr.r() - 40 - qw - nx);
                const std::string fitN = dabFit(c, c.f.body, full, room - c.tc.width(c.f.small, sub));
                const float w1 = (float)c.tc.draw(c.g, c.f.body, fitN, nx, rr.cy() - c.f.body.lineHeight() / 2.f, th::btnText);
                c.tc.draw(c.g, c.f.small, sub, nx + w1, rr.cy() - c.f.small.lineHeight() / 2.f, th::dim);
            } else {
                c.g.rrect(rr.x + 24, rr.y, rr.w - 24, rr.h, 6, rgb(0xffa000, 0.06f));
                const std::string t = r.kind == 1 ? "Tune to " + h["block"].str() : dabTrim((*r.sv)["label"].str());
                if (r.kind == 1) dabArrow(c, rr.x + 30, rr.cy(), 10, 0, th::activeTx);
                c.tc.draw(c.g, c.f.body, dabFit(c, c.f.body, t, (int)rr.w - 50), rr.x + (r.kind == 1 ? 42 : 36), rr.cy() - c.f.body.lineHeight() / 2.f, r.kind == 1 ? th::activeTx : th::btnText);
            }
        });
    if (hit >= 0) {
        const R& r = rows[(size_t)hit];
        if (r.kind == 0) { const std::string b = (*r.h)["block"].str(); u.scanOpen = u.scanOpen == b ? "" : b; }
        else if (!locked) {
            const int ch = (int)(*r.h)["channel"].num(-1);
            const double sid = r.kind == 2 ? (*r.sv)["sid"].num(0) : 0;
            u.scanMine = false;
            dabGoBlock(a, ch, sid);
            u.pane = DabUi::P_STATIONS;
        }
    }
}

/** The pane in `body` (the stations pane may be given a narrower list rect by the layout). */
inline void dabPane(App& a, Ctx& c, const RadioState& s, Rect body, Rect list, float rowH, bool tiles) {
    switch (dabUi().pane) {
        case DabUi::P_SIGNAL: dabPaneSignal(a, c, s, body); break;
        case DabUi::P_BLOCKS: dabPaneBlocks(a, c, s, body); break;
        case DabUi::P_SCAN: dabPaneScan(a, c, s, body); break;
        default: dabPaneStations(a, c, s, list, rowH, tiles); break;
    }
}

inline void dabPanel(Ctx& c, Rect r, float radius = 12) {
    c.g.rrect(r.x, r.y, r.w, r.h, radius, rgb(0x0c0802, 0.94f));
    c.g.rframe(r.x, r.y, r.w, r.h, radius, 1, th::btnBorder);
}

/** ★★ THE BOX — call once a frame in the deck pass, after the overlays and before the VTS bar and the island, while
 *  DAB is on (it draws nothing otherwise). */
inline void dabDrawBox(App& a, Ctx& c, const RadioState& s) {
    DabUi& u = dabUi();
    if (!u.on) return;
    const bool grabbed = dabGrabbed(a);
    // ★ BACK inside the box: a sub-pane goes back to the stations first (the release is the run loop's closeTop)
    if (grabbed && !a.sheets_.empty() && a.sheets_.back() == App::S_DAB && u.pane != DabUi::P_STATIONS && c.nav.input().back) {
        if (c.nav.backPressed()) { u.pane = DabUi::P_STATIONS; a.nav_.forget("dab"); }
    }
    Rect box;
    const Json* rep = dabReport(s);
    const Json* play = rep ? dabPlaying(*rep) : nullptr;
    if (u.slideFull && !(dabImages().slide() && play && grabbed)) u.slideFull = false;
    c.nav.layer(u.slideFull ? "dab.under" : "dab");   // ★ under the full-screen slide nothing in the box takes the pad
    if (u.layout == DAB_A_SHEET) {
        box = {6, 156, 628, 114};
        dabPanel(c, box);
        const Rect head{box.x, box.y, box.w, 30};
        dabHeader(a, c, s, head, true);
        c.g.rect(box.x + 8, head.b(), box.w - 16, 1, th::barBorder);
        const Rect body{box.x + 8, head.b() + 4, box.w - 22, box.b() - head.b() - 8};
        const Rect now{body.x, body.y + 2, 232, body.h - 2};
        const Rect list{body.x + 244, body.y, body.r() - (body.x + 244), body.h};
        if (u.pane == DabUi::P_STATIONS) dabNowPlayingBlock(c, s, now, 44, 1);
        dabPane(a, c, s, body, list, 26, false);
    } else if (u.layout == DAB_B_STRIP) {
        if (!grabbed) {
            box = {6, 222, 628, 48};
            dabPanel(c, box, 10);
            c.tc.draw(c.g, c.f.tiny, "DAB", box.x + 10, box.y + 4, th::btnText);
            dabNowPlayingBlock(c, s, {box.x + 46, box.y + 5, box.w - 200, box.h - 8}, 38, 0);
            hints(c, box.r() - 150, box.cy(), {{FACE_BOTTOM, "STATIONS"}});
        } else {
            box = {6, 44, 628, 226};
            dabPanel(c, box);
            const Rect head{box.x, box.y, box.w, 32};
            dabHeader(a, c, s, head, false);
            c.g.rect(box.x + 8, head.b(), box.w - 16, 1, th::barBorder);
            const Rect npr{box.x + 10, head.b() + 6, box.w - 20, 46};
            float by = head.b() + 4;
            if (u.pane == DabUi::P_STATIONS) { dabNowPlayingBlock(c, s, npr, 40, 1); by = npr.b() + 4; c.g.rect(box.x + 8, by, box.w - 16, 1, th::barBorder); by += 4; }
            const Rect body{box.x + 8, by, box.w - 22, box.b() - by - 6};
            dabPane(a, c, s, body, body, 30, true);
        }
    } else {
        box = {0, 64, (float)a.W, 208};
        c.g.rect(box.x, box.y, box.w, box.h, rgb(0x0c0802, 0.97f));
        c.g.rect(box.x, box.y, box.w, 1, th::btnBorder);
        c.g.rect(box.x, box.b() - 1, box.w, 1, th::barBorder);
        const Rect head{box.x + 2, box.y + 1, box.w - 6, 30};
        dabHeader(a, c, s, head, true);
        c.g.rect(box.x + 8, head.b(), box.w - 16, 1, th::barBorder);
        const Rect slide{10, head.b() + 6, 168, 126};
        const Rect now{10, slide.b() + 4, 172, box.b() - slide.b() - 6};
        const Rect list{192, head.b() + 6, box.w - 192 - 14, box.b() - head.b() - 10};
        const Rect body{10, head.b() + 6, box.w - 24, box.b() - head.b() - 10};
        if (u.pane == DabUi::P_STATIONS) {
            // ★ the slide is a stop of its own when there is one: ENTER shows it full screen
            bool fsl = false;
            if (dabImages().slide() && play) {
                const Nav::Hit hs = c.nav.item("dab.slide", slide);
                fsl = hs.focused;
                if (hs.pressed) u.slideFull = true;
            }
            dabSlide(c, slide, rep, play, fsl);
            if (play) {
                c.tc.draw(c.g, c.f.bodyB, dabFit(c, c.f.bodyB, dabTrim((*play)["label"].str()), (int)now.w), now.x, now.y, th::white);
                // ★ the radio text is the VTS bar's; here the codec line, or the "tuning in" line while it starts
                std::string cod = (*play)["codec"].str();
                if ((*play)["kbps"].num(0) > 0) cod += " " + std::to_string((int)(*play)["kbps"].num(0)) + " kbit/s";
                const char* pty = dabPty((int)(*play)["pty"].num(0));
                if (*pty && std::strcmp(pty, "None") != 0) cod += std::string(" · ") + pty;
                const std::string t = u.waiting.empty() ? cod : u.waiting;
                const auto& ln = c.tc.wrap(c.f.tiny, t, (int)now.w);
                float y = now.y + c.f.bodyB.lineHeight();
                for (size_t i = 0; i < ln.size() && i < 2; i++, y += c.f.tiny.lineHeight())
                    c.tc.draw(c.g, c.f.tiny, i == 1 && ln.size() > 2 ? dabFit(c, c.f.tiny, ln[i] + " …", (int)now.w) : ln[i], now.x, y,
                              u.waiting.empty() ? th::dim : th::amber);
            }
        }
        dabPane(a, c, s, body, list, 29, true);
    }
    c.nav.layer("deck");
    // the box as ONE stop on the island's layer (the web's [data-navbox]): ENTER grabs
    if (!grabbed) {
        const Nav::Hit h = c.nav.item("dab.box", box);
        if (h.focused) c.g.rframe(box.x + 1, box.y + 1, box.w - 2, box.h - 2, 12, 3, th::focus);
        if (h.focused && u.layout != DAB_B_STRIP) {
            const float hw = 24 + (float)c.tc.width(c.f.small, "browse the stations") + 20;
            const Rect hb{box.r() - hw - 10, box.b() - 34, hw, 26};
            c.g.rrect(hb.x, hb.y, hb.w, hb.h, 13, rgb(0, 0.85f));
            c.g.rframe(hb.x, hb.y, hb.w, hb.h, 13, 1, th::focus);
            hints(c, hb.x + 8, hb.cy(), {{FACE_BOTTOM, "browse the stations"}}, th::focus);
        }
        if (h.pressed) { a.open(App::S_DAB); u.wantRowFocus = true; }
    } else if (u.wantRowFocus && u.pane == DabUi::P_SIGNAL) {
        u.wantRowFocus = false; a.nav_.setFocus("dab", "dab.sig.0");
    } else if (u.wantRowFocus && u.pane == DabUi::P_BLOCKS) {
        u.wantRowFocus = false;
        const int cur = dabShownBlock();
        if (cur >= 0) a.nav_.setFocus("dab", std::string("dab.blk.") + kDabBlocks[cur].name);
    } else if ((u.wantRowFocus || a.nav_.focus("dab").empty()) && u.pane == DabUi::P_STATIONS && rep && (*rep)["services"].size()) {
        u.wantRowFocus = false;
        // ★ land on the station playing, else the first one — never on EXIT
        const double sid = play ? (*play)["sid"].num(0) : (*rep)["services"][0]["sid"].num(0);
        a.nav_.setFocus("dab", "dab.row." + std::to_string((long long)sid));
    }
    // ★ the scopes travel only while they are on screen (the web's dabSetPane → spec.dabScopes). This client reads the
    //   legacy report, which carries them anyway; the message also makes the engine send a report AT ONCE, so the pane
    //   never opens empty — and it is right the day this client moves to dab=2.
    const bool wantScopes = u.pane == DabUi::P_SIGNAL && (u.layout != DAB_B_STRIP || grabbed) && !u.slideFull;
    if (wantScopes != u.scopesOn) { u.scopesOn = wantScopes; a.eng_->send(std::string("{\"type\":\"dab_scopes\",\"on\":") + (wantScopes ? "1" : "0") + "}"); }
    // ★ an open request from the island (BLOCK, the frequency box, the meter) grabs too
    if (u.grabNext && !grabbed) { u.grabNext = false; a.open(App::S_DAB); u.wantRowFocus = true; }
}

}  // namespace hh

namespace hh {
/** ★ THE SLIDE, FULL SCREEN — draw it from the sheet loop (`case S_DAB: dabDrawFull(*this, c, s); break;`). A 320 × 240
 *  MOT slide is exactly half the panel, so it doubles cleanly. ENTER or BACK closes it; the station, its radio text and
 *  the slide's name ride along the foot. Nothing while it is not open. */
inline void dabDrawFull(App& a, Ctx& c, const RadioState& s) {
    DabUi& u = dabUi();
    if (!u.slideFull) return;
    const DabTex* t = dabImages().slide();
    const Json* rep = dabReport(s);
    const Json* play = rep ? dabPlaying(*rep) : nullptr;
    if (!t || !play) { u.slideFull = false; return; }
    c.g.rect(0, 0, (float)a.W, (float)a.H, rgb(0));
    dabDrawTex(c.g, *t, 0, 0, (float)a.W, (float)a.H);
    const float fh = 54;
    c.g.rect(0, a.H - fh, (float)a.W, fh, rgb(0, 0.72f));
    c.tc.draw(c.g, c.f.bodyB, dabFit(c, c.f.bodyB, dabTrim((*play)["label"].str()), a.W - 220), 14, a.H - fh + 4, th::white);
    const std::string dls = dabTrim((*rep)["dls"].str());
    c.tc.draw(c.g, c.f.small, dabFit(c, c.f.small, dls, a.W - 28), 14, a.H - fh + 4 + c.f.bodyB.lineHeight(), th::white68);
    hints(c, a.W - 200.f, a.H - fh + 15, {{FACE_RIGHT, "close"}}, th::white68);
    const Nav::Hit h = c.nav.item("dab.full", {0, 0, (float)a.W, (float)a.H});
    if (h.pressed || c.nav.backPressed()) { u.slideFull = false; a.nav_.setFocus("dab", "dab.slide"); }
}
}  // namespace hh
