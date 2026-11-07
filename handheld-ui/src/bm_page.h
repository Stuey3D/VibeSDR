// bm_page.h — the BOOKMARKS page (the web's #bookmarksPanel, docs/v12/R36S-NATIVE-MENU-INVENTORY.md §5.1) and the two
// keys under the frequency pad that open it and SEARCH (§2.5: Stuart, 2026-11-06 — "at the bottom of the frequency input
// screen a bookmarks button which switches to a bookmarks page where you can add or view bookmarks, import too from the
// exfat partition. Search button to take you to the search page … a 2nd import button specifically for the EiBi import").
//
// ★★ ONE LIST: THE RECEIVER'S. The web page keeps two — YOUR BOOKMARKS (the browser's localStorage) and SAVED ON THE
//    RECEIVER (the engine's file) — because a browser on a phone and the receiver are different machines. Here they are
//    the same machine: the engine's list IS this handheld's, one file whichever radio is plugged in (vibeserver
//    main.cpp), and it is the list R36S HARDWARE → FILES → IMPORT already fills (hw_files.h) and EXPORT BOOKMARKS writes.
//    A second, UI-local list would be a second place for a bookmark to hide. So + ADD HERE saves on the receiver
//    (POST /bookmarks, the web's + ADD TO SERVER), and the page shows the receiver's two sections:
//    SAVED ON THE RECEIVER (manual) and HEARD BY THIS RECEIVER (learnt over RDS; expires).
// ★ DAB rows are not offered: a DAB bookmark is a block + a service (main.ts dabGoTo), and this UI has no DAB — a row
//   that cannot play is a dead control (AGENTS.md).
// ★ A row press = the web's tuneTo (bmTuneTo): its mode (or the band's), its passband, the landing band's step.
#pragma once
#include "bm_state.h"
#include "hw_files.h"

namespace hh {

/** The page's own state (UI thread only). */
struct BmUi {
    int view = 0;                 // 0 the list, 1 EDIT one bookmark
    BmRow edit;                   // the bookmark being edited (its frequency + sid name the server's row)
    double armedDelete = 0;
    Scroller scroll;
    bool importWasBusy = false;
    std::string focusNext;        // ★ a highlight for the NEXT frame: the view it names is not drawn until then (Nav.end
                                  //   would otherwise fall back to the first control, CLOSE)
    std::vector<BmRow> rows; uint64_t seq = ~0ull;   // the receiver's list, copied only when it changes
    std::map<std::string, std::string> fit;   // a name cut to its column, by "<width>|<name>"
};
inline BmUi& bmUi() { static BmUi u; return u; }

/** Text cut to `maxW` pixels with "…", measured without making textures (each try would be one). */
inline std::string bmFit(const Font& f, const std::string& s, int maxW) {
    BmUi& u = bmUi();
    const std::string key = std::to_string(maxW) + "|" + std::to_string((uintptr_t)&f) + "|" + s;
    auto it = u.fit.find(key);
    if (it != u.fit.end()) return it->second;
    if (u.fit.size() > 4000) u.fit.clear();
    auto w = [&](const std::string& t) { int x = 0, y = 0; TTF_SizeUTF8(f.get(), t.c_str(), &x, &y); return x; };
    std::string out = s;
    if (w(s) > maxW) {
        size_t n = s.size();
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
            out = s.substr(0, n) + "…";
            if (w(out) <= maxW) break;
        }
    }
    u.fit[key] = out;
    return out;
}

/** main.ts tuneTo(r): the result's mode, else the band's, else the current one; tune through clampTune; the result's
 *  own passband when it has one; then the landing band's step (applyBandStep). */
inline void bmTuneTo(App& a, double hz, const std::string& mode, bool hasBw, int lo, int hi) {
    const RadioState& s = a.eng_->state();
    if (s.session != "running" || !(hz > 0)) return;
    const int reg = a.region();
    const BandDefaults d = bandTuneDefaults(hz, reg);
    std::string m = !mode.empty() ? mode : !d.mode.empty() ? d.mode : s.mode;
    int plo = 0, phi = 0;
    if (!modePassband(m, plo, phi)) m = s.mode;              // a mode this UI cannot demodulate keeps the current one
    a.gapNudgeDir_ = 0;
    const double got = a.clampTune(hz);
    a.tuneTo(got);
    a.eng_->setMode(m);                                      // ★ always: the mode's passband, then the bookmark's own
    a.num_["bw.sync"] = -1;
    if (hasBw && hi > lo) a.eng_->setBandwidth(lo, hi);
    if (d.step > 0) a.setStep(stepOnLadder(got, d.step));
}

/** Open the BOOKMARKS page (from the pad, or anywhere). */
inline void bmOpen(App& a) {
    BmUi& u = bmUi();
    u.view = 0; u.armedDelete = 0; u.scroll = {};
    a.nav_.forget("bm");
    a.nav_.setFocus("bm", "bm.add");
    a.open(App::S_BOOKMARKS);
    if (a.eng_) { bmRefresh(a.eng_->port()); bmLoadStations(a.eng_->port()); }
}
inline void searchOpen(App& a);   // search_page.h

// ── the frequency pad's two keys ────────────────────────────────────────────────────────────────────────────────────
// ★ The pad is the keyboard (S_KB), which takes every button itself (kbInput) — there is no Nav there. So the two keys
//   are reached the way the pad's own keys are: DOWN from the bottom row (". 0 ↵ Back") lands on them, LEFT / RIGHT
//   go between them, UP goes back to the keys, ENTER opens the page. num_["pad.link"]: 0 keys · 1 BOOKMARKS · 2 SEARCH.
inline bool bmPadIsFreq(const App& a) {
    return !a.sheets_.empty() && a.sheets_.back() == App::S_KB && a.kbReq_.opts.mode == kb::KbMode::frequency && a.kbReq_.title == "FREQUENCY";
}

inline bool App::padLinkFocused() const {
    if (!bmPadIsFreq(*this)) return false;
    auto it = num_.find("pad.link");
    return it != num_.end() && it->second > 0;
}

inline bool App::padLinksInput(Btn b) {
    if (!bmPadIsFreq(*this)) return false;
    double& link = num_["pad.link"];
    if (link <= 0) {
        if (b != B_DOWN) return false;
        const kb::KbKey* k = kbd_.focusedKey();
        double bottom = 0;
        for (auto& key : kbd_.keys()) bottom = std::max(bottom, key.y + key.h);
        if (!k || k->y + k->h < bottom - 1e-6) return false;      // not the bottom row: the pad's own move
        link = k->x < 2.0 ? 1 : 2;                                // under ". 0" → BOOKMARKS, under "↵ Back" → SEARCH
        return true;
    }
    switch (b) {
        case B_LEFT: case B_RIGHT: link = link == 1 ? 2 : 1; return true;
        case B_UP: link = 0; return true;
        case B_DOWN: return true;
        case B_ENTER: {
            const bool bm = link == 1;
            link = 0;
            for (size_t i = sheets_.size(); i-- > 0;) if (sheets_[i] == S_KB) { sheets_.erase(sheets_.begin() + (long)i); break; }
            if (bm) bmOpen(*this); else searchOpen(*this);
            return true;
        }
        case B_BACK: link = 0; return false;                       // the pad's own Back (a layer, then cancel)
        default: return false;                                    // ⌫, the unit, START: still the pad's
    }
}

inline void App::drawPadLinks(Ctx& c, Rect r) {
    // ★ a pad that was closed with a key highlighted opens on its keys again
    if (nowSec() - num_["pad.drawnAt"] > 0.25) num_["pad.link"] = 0;
    num_["pad.drawnAt"] = nowSec();
    const int link = (int)num_["pad.link"];
    const float gap = 12, w = (r.w - gap) / 2;
    const Rect bb{r.x, r.y, w, r.h}, sb{r.x + w + gap, r.y, w, r.h};
    auto key = [&](Rect k, const char* label, bool focused, int glyph) {
        c.g.rrect(k.x, k.y, k.w, k.h, 9, rgb(0x140a00));
        c.g.rframe(k.x, k.y, k.w, k.h, 9, 1, th::btnBorder);
        const float tw = (float)c.tc.width(c.f.btn, label);
        const float gx = k.cx() - (tw + 30) / 2, cy = k.cy();
        if (glyph == 0) {
            // a bookmark ribbon
            c.g.rect(gx + 2, cy - 10, 14, 14, th::btnText);
            c.g.triangle(gx + 2, cy + 4, gx + 9, cy + 4, gx + 2, cy + 11, th::btnText);
            c.g.triangle(gx + 16, cy + 4, gx + 9, cy + 4, gx + 16, cy + 11, th::btnText);
        } else {
            // a magnifier
            c.g.circle(gx + 8, cy - 3, 7, 2.2f, th::btnText);
            c.g.line(gx + 13, cy + 2, gx + 19, cy + 9, 2.6f, th::btnText);
        }
        c.tc.draw(c.g, c.f.btn, label, gx + 30, cy - c.f.btn.lineHeight() / 2.f, th::btnText);
        if (focused) c.g.rframe(k.x - 4, k.y - 4, k.w + 8, k.h + 8, 12, 3, th::focus);
    };
    key(bb, "BOOKMARKS", link == 1, 0);
    key(sb, "SEARCH", link == 2, 1);
}

// ── the page ────────────────────────────────────────────────────────────────────────────────────────────────────────

/** "<MHz> MHz <MODE>" — the web's default name (main.ts addNow); the RDS name first when there is one (+ ADD TO SERVER). */
inline std::string bmDefaultName(const RadioState& s) {
    const std::string ps = bmClean(s.rdsPs, 64);
    if (!ps.empty()) return ps;
    return bmMhz(s.vfoHz) + " MHz " + modeLabel(s.mode);
}

inline void bmAskName(App& a, const std::string& title, const std::string& value, const std::string& placeholder,
                      std::function<std::string(const std::string&)> done) {
    KbRequest r;
    r.title = title;
    r.opts.mode = kb::KbMode::text;
    r.opts.value = value;
    r.placeholder = placeholder;
    r.commit = [done, placeholder](const kb::KbState& st) -> std::string {
        std::string v = bmClean(st.valueUtf8(), 60);
        if (v.empty()) v = placeholder;
        if (v.empty()) return "Enter a name";
        return done(v);
    };
    a.openKeyboard(r);
}

inline void bmEditQuery(App& a, const BmRow& old, double hz, const std::string& name, const std::string& mode,
                        bool hasBw, int lo, int hi, const std::string& okText) {
    std::string q = "was=" + std::to_string(std::llround(old.frequency));
    if (old.sid > 0) q += "&was_sid=" + std::to_string(old.sid);
    q += "&frequency=" + std::to_string(std::llround(hz)) + "&name=" + bmUrlEnc(name) + "&mode=" + bmUrlEnc(mode) + bmBwQuery(hasBw, lo, hi);
    bmWrite(a.eng_->port(), "POST", q, okText);
}

inline void App::drawBookmarks(Ctx& c, const RadioState& s) {
    BmUi& u = bmUi();
    const int port = eng_->port();
    bool closed = false;
    const Rect panel{12, 10, 616, 460};
    const Rect area = sheetPanel(c, panel, u.view == 1 ? "EDIT BOOKMARK" : "BOOKMARKS", "bm.close", closed);
    if (closed) { closeTop(); return; }
    if (!u.focusNext.empty()) { nav_.setFocus("bm", u.focusNext); u.focusNext.clear(); }

    bool known = false, busy = false, eibiBusy = false; int eibiN = -1; std::string eibiAt;
    {
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        if (u.seq != b.seq) { u.rows = b.rows; u.seq = b.seq; }
        known = b.known; busy = b.writeBusy; eibiBusy = b.eibiBusy; eibiN = b.eibiEntries; eibiAt = b.eibiUpdated;
    }
    bool importBusy = false;
    { HwShared& h = hwShared(); std::lock_guard<std::mutex> lk(h.m); importBusy = h.importBusy; }
    if (u.importWasBusy && !importBusy) bmRefresh(port, true);    // the card's import landed in the receiver's list
    u.importWasBusy = importBusy;

    // ══ EDIT one bookmark (the web's #bmEdit, as keys: the name, the dial, delete) ══
    if (u.view == 1) {
        const BmRow& e = u.edit;
        float y = area.y;
        c.tc.draw(c.g, c.f.title, bmFit(c.f.title, e.name, (int)area.w - 12), area.x + 6, y, th::white);
        y += c.f.title.lineHeight() + 2;
        std::string line = bmMhz(e.frequency) + " MHz · " + modeLabel(e.mode);
        if (e.hasBw) line += " · " + std::to_string(e.bwLo) + " … " + std::to_string(e.bwHi) + " Hz";
        line += e.manual ? " · saved on the receiver" : " · heard by this receiver";
        c.tc.draw(c.g, c.f.small, line, area.x + 6, y, th::dim);
        y += c.f.small.lineHeight() + 14;
        BtnStyle st; st.font = &c.f.btn; st.disabled = busy;
        const float bh = 50;
        if (button(c, "bm.e.tune", {area.x, y, area.w, bh}, "TUNE", st)) { bmTuneTo(*this, e.frequency, e.mode, e.hasBw, e.bwLo, e.bwHi); closeTop(); return; }
        y += bh + 10;
        if (button(c, "bm.e.name", {area.x, y, area.w, bh}, "RENAME", st)) {
            const BmRow old = e;
            bmAskName(*this, "BOOKMARK NAME", old.name, old.name, [this, old](const std::string& v) -> std::string {
                bmEditQuery(*this, old, old.frequency, v, old.mode, old.hasBw, old.bwLo, old.bwHi, "Renamed \"" + v + "\"");
                bmUi().edit.name = v;
                return "";
            });
        }
        y += bh + 10;
        if (button(c, "bm.e.dial", {area.x, y, area.w, bh}, "USE WHAT I'M LISTENING TO", st) && s.session == "running") {
            const BmRow old = e;
            bmEditQuery(*this, old, s.vfoHz, old.name, s.mode, true, s.bwLow, s.bwHigh, "Saved \"" + old.name + "\" at " + bmMhz(s.vfoHz) + " MHz");
            u.edit.frequency = std::round(s.vfoHz); u.edit.mode = s.mode; u.edit.hasBw = true; u.edit.bwLo = s.bwLow; u.edit.bwHi = s.bwHigh;
        }
        y += bh + 2;
        c.tc.draw(c.g, c.f.small, "Frequency, mode and passband from the dial; the name stays", area.x + 6, y, th::dim);
        y += c.f.small.lineHeight() + 10;
        {
            const double t = nowSec();
            BtnStyle ds = st; ds.on = t - u.armedDelete < 3; ds.text = th::red;
            if (button(c, "bm.e.del", {area.x, y, area.w, bh}, ds.on ? "PRESS AGAIN TO DELETE" : "DELETE", ds)) {
                if (t - u.armedDelete < 3) {
                    std::string q = "frequency=" + std::to_string(std::llround(e.frequency));
                    if (e.sid >= 0) q += "&sid=" + std::to_string(e.sid);
                    bmWrite(port, "DELETE", q, "Deleted \"" + e.name + "\"");
                    u.view = 0; u.armedDelete = 0;
                    u.focusNext = "bm.add";
                    return;
                }
                u.armedDelete = t;
            }
        }
        // ★ Back: to the list, not out of the page
        if (c.nav.input().back && c.nav.backPressed()) { u.view = 0; u.focusNext = "bm.r." + std::to_string(std::llround(e.frequency)) + ".e"; }
        return;
    }

    // ══ the list ══
    float y = area.y;
    {
        const float gap = 10, bw = (area.w - 2 * gap) / 3, bh = 48;
        BtnStyle st; st.font = &c.f.btn;
        BtnStyle as = st; as.disabled = busy || s.session != "running";
        if (button(c, "bm.add", {area.x, y, bw, bh}, "+ ADD HERE", as)) {
            const std::string def = bmDefaultName(s);
            bmAskName(*this, "BOOKMARK NAME", "", def, [this](const std::string& name) -> std::string {
                const RadioState& st2 = eng_->state();
                if (st2.session != "running") return "No radio — nothing to save";
                const std::string q = "frequency=" + std::to_string(std::llround(st2.vfoHz)) + "&name=" + bmUrlEnc(name) + "&mode=" + bmUrlEnc(st2.mode)
                                    + bmBwQuery(true, st2.bwLow, st2.bwHigh);
                bmWrite(eng_->port(), "POST", q, "Saved \"" + name + "\" on the receiver");
                return "";
            });
        }
        BtnStyle is = st; is.on = importBusy;
        if (button(c, "bm.import", {area.x + bw + gap, y, bw, bh}, importBusy ? "WORKING…" : "IMPORT", is) && !importBusy) hwRunImport(port, true);
        BtnStyle es = st; es.on = eibiBusy;
        if (button(c, "bm.eibi", {area.x + 2 * (bw + gap), y, bw, bh}, eibiBusy ? "WORKING…" : "IMPORT EIBI", es) && !eibiBusy) bmImportEibi(port);
        y += bh + 8;
        // what the two imports read, and the schedule SEARCH has now
        c.tc.draw(c.g, c.f.small, "IMPORT: bookmark backups from the card's import folder", area.x + 2, y, th::dim);
        y += c.f.small.lineHeight();
        c.tc.draw(c.g, c.f.small, "IMPORT EIBI: sked-b26.csv from its EiBi folder — the CSV, not the freq-/time- .txt",
                  area.x + 2, y, th::dim);
        y += c.f.small.lineHeight();
        std::string ei;
        if (eibiN > 0) ei = "EiBi schedule: " + std::to_string(eibiN) + " stations" + (eibiAt.empty() ? "" : " · " + eibiAt);
        else if (eibiN == 0) ei = "No EiBi schedule yet — put sked-b26.csv from eibispace.de in the card's EiBi folder";
        if (!ei.empty()) c.tc.draw(c.g, c.f.small, bmFit(c.f.small, ei, (int)area.w - 4), area.x + 2, y, eibiN > 0 ? th::dimmer : th::amber);
        y += c.f.small.lineHeight() + 8;
    }

    const Rect view{area.x - 8, y, area.w + 16, panel.b() - 8 - y};
    c.g.clip(view.x, view.y, view.w, view.h);
    float ly = view.y - u.scroll.y;
    const float rowH = 46, rowGap = 4;
    // the two sections, by frequency; DAB rows are not offered (this UI has no DAB)
    std::vector<const BmRow*> saved, heard;
    for (auto& r : u.rows) { if (r.mode == "dab") continue; (r.manual ? saved : heard).push_back(&r); }
    auto byF = [](const BmRow* a2, const BmRow* b2) { return a2->frequency < b2->frequency; };
    std::sort(saved.begin(), saved.end(), byF);
    std::sort(heard.begin(), heard.end(), byF);
    const float freqW = (float)c.tc.widthTab(c.f.body, "8888.888") + 8;
    const float editW = 86, modeW = 70;
    auto visible = [&](float top, float h) { return top + h > view.y - 2 * rowH && top < view.b() + 2 * rowH; };
    auto section = [&](const char* title, const char* hint, const std::vector<const BmRow*>& list) -> bool {
        if (list.empty()) return true;
        if (visible(ly, 44)) {
            float tx = area.x + 4;
            tx += c.tc.draw(c.g, c.f.bodyB, title, tx, ly + 2, th::btnText) + 10;
            c.tc.draw(c.g, c.f.small, std::to_string(list.size()), tx, ly + 4, th::dim);
            c.tc.draw(c.g, c.f.small, hint, area.x + 4, ly + 2 + c.f.bodyB.lineHeight(), th::dimmer);
        }
        ly += c.f.bodyB.lineHeight() + c.f.small.lineHeight() + 8;
        for (const BmRow* r : list) {
            if (visible(ly, rowH)) {
                const std::string id = "bm.r." + std::to_string(std::llround(r->frequency));
                const Rect row{area.x, ly, area.w, rowH};
                const Nav::Hit h = c.nav.item(id, row);
                if (ly + rowH > view.y && ly < view.b()) {
                    c.g.rrect(row.x, row.y, row.w - editW - 8, row.h, 7, h.focused ? th::activeBg : th::btnBg);
                    c.g.rframe(row.x, row.y, row.w - editW - 8, row.h, 7, 1, h.focused ? th::activeBdr : th::btnBorder);
                    c.tc.drawTab(c.g, c.f.body, bmMhz(r->frequency), row.x + 10, row.cy() - c.f.body.lineHeight() / 2.f, th::white);
                    const int nameW = (int)(row.w - editW - 8 - freqW - modeW - 24);
                    c.tc.draw(c.g, c.f.body, bmFit(c.f.body, r->name, nameW), row.x + 10 + freqW, row.cy() - c.f.body.lineHeight() / 2.f, th::white);
                    c.tc.draw(c.g, c.f.small, modeLabel(r->mode), row.x + row.w - editW - 18, row.cy() - c.f.small.lineHeight() / 2.f, th::dim, 2);
                    if (h.focused) focusRing(c, {row.x, row.y, row.w - editW - 8, row.h}, 7);
                }
                if (h.pressed) { bmTuneTo(*this, r->frequency, r->mode, r->hasBw, r->bwLo, r->bwHi); c.g.unclip(); closeTop(); return false; }
                BtnStyle es; es.font = &c.f.bodyB; es.radius = 7; es.disabled = busy;
                if (button(c, id + ".e", {row.r() - editW, row.y, editW, rowH}, "EDIT", es)) {
                    u.edit = *r; u.view = 1; u.armedDelete = 0;
                    u.focusNext = "bm.e.tune";
                }
            }
            ly += rowH + rowGap;
        }
        ly += 10;
        return true;
    };
    // ★ false = a row tuned and the page closed itself
    if (!section("SAVED ON THE RECEIVER", "Saved on this handheld — the same list whichever radio is plugged in", saved)) return;
    if (!section("HEARD BY THIS RECEIVER", "Found automatically over RDS — expires if it stops being heard", heard)) return;
    if (saved.empty() && heard.empty()) {
        const char* msg = !known ? "Loading the receiver's bookmarks…"
                                 : "No bookmarks yet — tune something and press + ADD HERE. Stations heard over RDS are added here automatically.";
        for (auto& l : c.tc.wrap(c.f.body, msg, (int)area.w - 12)) { c.tc.draw(c.g, c.f.body, l, area.x + 6, ly, th::dim); ly += c.f.body.lineHeight(); }
    }
    c.g.unclip();
    u.scroll.contentH = ly + u.scroll.y - view.y;
    u.scroll.follow(c.nav.focusedRect(), view);
    if (nav_.focus("bm").empty()) nav_.setFocus("bm", "bm.add");
}

}  // namespace hh
