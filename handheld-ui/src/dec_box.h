// dec_box.h — the DECODER BOX on a 640×480 handheld (every decoder but DAB, which has its own page — dab_box.h), the
// decoder picker inside it, and the mode picker's DECODERS… entry. State and actions are dec_state.h; this file draws
// and registers what the D-pad can land on.
//
// ★★★ THE BOX (decided with Stuart, 2026-11-07): a box OVER THE WATERFALL — spectrum, scale and band bar stay; the box
//     sits between the spectrum and the island (y 156 →), like DAB's layout A. ★ The VTS bar (vts.h, the 34 px strip
//     just above the island) hides when it has nothing to say, and then the box GROWS DOWN INTO ITS STRIP (to y 308):
//     one more line of decoded text whenever the station strip is idle. Asked of vts.h every frame (decVtsShowing).
// ★★★ ONE STOP FOR THE D-PAD (the web's [data-navbox], gamepad.ts): ENTER grabs it (App::S_DEC on the sheet stack, layer
//     "dec"), BACK releases it. Grabbed, the D-pad scrolls the decoded text (UP / DOWN), RIGHT reaches the header's keys
//     (the decoder's views, DECODERS, CLR, STOP); in a list decoder the rows are the stops and ENTER tunes to one.
// ★★ THE PICKER (the web's #decodersPanel, opened from the mode picker's DECODERS…): the decoders grouped by use under a
//    BAND tab (src/utils/decoderBands.ts), EVERY DECODER A TOGGLE WITH ITS OPTIONS UNDER IT (Stuart's rule): switch one
//    on and its rows open beneath it — RTTY's AUTO + presets + the whole manual spec, MFSK's FAMILY then MODE, the time
//    station, CW / PSK's view and segment, APRS's packet rate, the DSC and JTTY channels. While the picker is open the
//    box is a tall panel over the spectrum too (it is a menu; the spectrum is not needed while choosing); BACK returns
//    to the decoded text (or releases the box when nothing runs).
// ★★ OPTION ROWS ARE FULL-WIDTH STOPS (ui.h: pickNext skips small right-hand controls under full-width rows): a choice
//    row takes LEFT / RIGHT and changes its value at once (ENTER steps on); a channel row moves a cursor with LEFT /
//    RIGHT and ENTER tunes the one under it.
// ★ Text: Atkinson Hyperlegible, the decoded text at 19 px (the web's box is 12–13 px) — readable at 3.5".
// ★ Cheap: only the visible rows are drawn; text is wrapped once when it arrives (dec_state.h DecLog), never per frame.
#pragma once
#include "dec_state.h"
#include "cw_plain.h"  // ★ CW's EXPLAIN reading (the app's plainEnglish, ported + held to it by test/cw_plain_test.cpp)
#include "dec_pic.h"   // ★ the picture decoders' state (WEFAX, SSTV, HELL); their drawing is dec_pic_box.h, included at the end

namespace hh {

// ★ dec_pic_box.h / dec_pic_gallery.h (included at the end of this file)
inline void decPicPane(App&, Ctx&, const RadioState&, Rect, bool);
inline void decPicKeys(App&, const std::string&, std::vector<PicKey>&);
inline void decPicKeyPress(App&, int, const std::string&);
inline bool decPicFullOpen();
inline void galOpen(App&, int);
inline bool galEnabled(int);
inline void galClose(App&);
struct GalUi; inline GalUi& galUi();
inline bool galIsOpen();

// ── small helpers ─────────────────────────────────────────────────────────────────────────────────────────────────────

/** ★ Is the VTS bar showing (vts.h)? The box takes its strip only while it is not. */
inline bool decVtsShowing() {
    const vts::State& st = vts::vtsState();
    return st.bar.shown() && st.bar.opacity() > 0.004;
}
/** `s` cut to `maxW` pixels with "…" (cached). */
inline std::string decFit(Ctx& c, const Font& f, const std::string& s, int maxW) {
    DecUi& u = decUi();
    if (maxW <= 0) return "";
    const std::string key = std::to_string((uintptr_t)f.get()) + "|" + std::to_string(maxW) + "|" + s;
    auto it = u.fit.find(key);
    if (it != u.fit.end()) return it->second;
    if (u.fit.size() > 2000) u.fit.clear();
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
/** ★ `s` cut to `maxW` pixels from the FRONT ("…" first): the newest of a signal's text, the web's #cwList .tx
 *  (direction: rtl + an ellipsis). The row's text runs on as it is decoded, as RTTY's foot does (cached). */
inline std::string decFitTail(Ctx& c, const Font& f, const std::string& s, int maxW) {
    DecUi& u = decUi();
    if (maxW <= 0) return "";
    const std::string key = "T|" + std::to_string((uintptr_t)f.get()) + "|" + std::to_string(maxW) + "|" + s;
    auto it = u.fit.find(key);
    if (it != u.fit.end()) return it->second;
    if (u.fit.size() > 2000) u.fit.clear();
    std::string out = s;
    if (c.tc.width(f, s) > maxW) {
        // a first cut by an average advance, then a character at a time (a row is redrawn once a second, not per frame)
        size_t i = 0;
        const int avg = std::max(1, c.tc.width(f, "abcdefghij ABCDE 0123") / 21);
        if ((int)s.size() * avg > maxW * 2) i = s.size() - (size_t)(maxW * 2 / avg);
        for (;;) {
            while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
            out = "…" + s.substr(i);
            if (i >= s.size() || c.tc.width(f, out) <= maxW) break;
            i++;
        }
    }
    return u.fit[key] = out;
}
inline bool decGrabbed(const App& a) { for (auto sh : a.sheets_) if (sh == App::S_DEC) return true; return false; }
inline void decRelease(App& a) {
    for (size_t i = 0; i < a.sheets_.size();) { if (a.sheets_[i] == App::S_DEC) a.sheets_.erase(a.sheets_.begin() + (long)i); else i++; }
    a.nav_.forget("dec");
}
inline std::string decMHz(double hz) { char b[32]; std::snprintf(b, sizeof b, hz >= 30e6 ? "%.4f" : "%.3f", hz / 1e6); return b; }
inline std::string decKHz(double hz) { char b[32]; std::snprintf(b, sizeof b, "%.2f", hz / 1e3); return b; }
inline std::string decHms(double t) {
    const time_t tt = (time_t)t; struct tm g{}; gmtime_r(&tt, &g);
    char b[16]; std::snprintf(b, sizeof b, "%02d:%02d", g.tm_hour, g.tm_min); return b;
}
inline bool decReach(const RadioState& s, double hz) {
    if (s.ranges.empty()) return hz >= 10000 && hz <= 1.8e9;
    for (auto& r : s.ranges) if (hz >= r.first && hz <= r.second) return true;
    return false;
}
/** The CW / PSK quality colours (green high · amber medium · orange low · red poor — the web's list). */
inline Rgba decQCol(const std::string& q) {
    return q == "high" ? rgb(0x7dffb0) : q == "medium" ? rgb(0xffc966) : q == "low" ? rgb(0xff9a5a) : rgb(0xff6a6a);
}

/** The box's title (main.ts showDecBox / spotsTitle). */
inline std::string decTitle(const std::string& view) {
    const DecUi& u = decUi();
    if (view == "spots") {
        std::lock_guard<std::mutex> lk(decUi().capMtx);
        if (!u.spotModes.empty() && u.spotModes.size() <= 2) return u.spotModes.size() == 1 ? u.spotModes[0] + " SPOTS" : u.spotModes[0] + " / " + u.spotModes[1] + " SPOTS";
        return "DIGITAL SPOTS";
    }
    if (view == "cw") return u.cwSkim ? "CW SKIMMER" : "CW";
    if (view == "psk") return u.pskSkim ? "PSK SKIMMER" : "PSK";
    if (view == "aprs") return u.aprsBaud == 300 ? "APRS · HF 300" : "APRS";
    if (view == "time") { for (auto& t : kTimeStations) if (u.timeStation == t.id) return std::string("TIME · ") + upper(t.id); }
    if (view == "refused") return "DECODER";
    if (view == "hell") return "FELD HELL";
    if (const DecExternal* e = decExt(view)) return e->label;
    const DecDef* d = decDef(view);
    return d ? d->label : "DECODERS";
}

/** A small key (the DAB header's). */
inline float decKeyW(Ctx& c, const std::string& label) { return (float)c.tc.width(c.f.tiny, label) + 18; }
inline bool decKey(Ctx& c, const std::string& id, Rect r, const std::string& label, bool on = false) {
    BtnStyle st; st.font = &c.f.tiny; st.radius = 6; st.on = on;
    return button(c, id, r, label, st);
}

/** ★★★ THE VIEW NEVER LEAVES THE ROW YOU ARE READING (the app's useSpotsPin, Stuart 2026-10-31; the R36S 2026-11-11):
 *  the rows at the top of the view (up to three, by key, with where each sat) are kept; next frame, the first of them
 *  still listed is put back at the same place on screen — so a signal arriving ABOVE it (rows are lowest frequency
 *  first) or leaving, or a row changing above it, does not move what is being read. Even at the very top: scroll up
 *  to see a new one. The highlight still brings its own row into view (Scroller::follow). */
struct DecAnchor { std::vector<std::pair<std::string, float>> rows; };

/** ★ CW (not PSK — typed, not abbreviated — nor JTTY) read in plain English: the EXPLAIN key (settings cwPlain). */
inline bool decCwPlain(const App& a) { return a.set_.cwPlain && decUi().active == "cw"; }

/** A scrolling column of full-width rows (registered under prefix + key(k)); returns the row pressed, or -1.
 *  `hold` (optional): keep the rows being read in place as the list changes (DecAnchor). */
template <class KeyF, class DrawF>
inline int decRows(Ctx& c, Scroller& sc, Rect view, int n, float rowH, const std::string& prefix, KeyF key, DrawF draw,
                   DecAnchor* hold = nullptr) {
    int hit = -1;
    sc.contentH = n * rowH;
    if (hold && !hold->rows.empty()) {
        bool done = false;
        for (auto& ar : hold->rows) {
            for (int k = 0; k < n && !done; k++) {
                if (key(k) != ar.first) continue;
                const float d = (k * rowH - ar.second) - sc.y;   // where it must be scrolled to sit where it sat
                sc.y += d; sc.target += d;
                done = true;
            }
            if (done) break;
        }
        const float mx = std::max(0.f, sc.contentH - view.h);
        sc.y = std::max(0.f, std::min(sc.y, mx)); sc.target = std::max(0.f, std::min(sc.target, mx));
    }
    c.g.clip(view.x - 1, view.y, view.w + 2, view.h);
    Rect fr{}; bool haveF = false;
    for (int k = 0; k < n; k++) {
        const Rect rr{view.x, view.y + k * rowH - sc.y, view.w, rowH - 3};
        const Nav::Hit h = c.nav.item(prefix + key(k), rr);
        if (rr.b() >= view.y && rr.y <= view.b()) draw(k, rr, h.focused);
        if (h.focused) { fr = rr; haveF = true; }
        if (h.pressed) hit = k;
    }
    c.g.unclip();
    if (haveF && fr.b() > view.y && fr.y < view.b())
        c.g.rframe(fr.x - 1, std::max(view.y, fr.y) - 1, fr.w + 2, std::min(view.b(), fr.b()) - std::max(view.y, fr.y) + 2, 7, 3, th::focus);
    sc.follow(c.nav.focusedRect(), view, 3);
    if (hold) {
        hold->rows.clear();
        for (int k = std::max(0, (int)std::floor(sc.y / rowH)); k < n && hold->rows.size() < 3; k++)
            hold->rows.push_back({key(k), k * rowH - sc.y});
    }
    if (sc.contentH > view.h + 1) {
        const float frac = view.h / sc.contentH, top = sc.y / sc.contentH;
        c.g.rrect(view.r() + 2, view.y + top * view.h, 3, std::max(10.f, frac * view.h), 1.5f, th::dim);
    }
    return hit;
}

// ── the output panes ─────────────────────────────────────────────────────────────────────────────────────────────────

/** The decoded text: the newest at the foot, following it unless the reader has scrolled up (then the place holds
 *  still as new text arrives — the web's keepReadingPlace). Grabbed, UP / DOWN scroll two rows a press. */
inline void decDrawLog(App& a, Ctx& c, Rect body, const std::string& idle, DecLog* log = nullptr) {
    DecUi& u = decUi();
    DecLog& L = log ? *log : u.log;
    L.setXform(decCwPlain(a) ? &cwPlainEnglish : nullptr);
    L.setCwBreaks(u.active == "cw");   // ★ CW's stream of characters is cut into lines (DecLog::setCwBreaks)
    const Font& f = c.f.body;
    const float lh = (float)f.lineHeight();
    const int n = std::max(1, (int)std::floor(body.h / lh));
    L.setView(n);
    L.layout(c.tc, f, (int)body.w - 22);
    const int rows = (int)L.rows();
    const long added = L.takeAdded();
    if (L.back > 0) L.back = (int)std::max(1L, L.back + added);   // ★ the place holds while text arrives below it
    // the scroll stops (see the header): a press that landed on one scrolls, and the highlight goes back to the text
    // ★ only a press FROM the text scrolls: DOWN from a header key lands on the upper stop too, and must just enter the text
    static std::string prevFocus;
    const std::string fnow = a.nav_.focus("dec");
    const bool fromLog = prevFocus == "dec.log";
    if (fnow == "dec.log.up") { if (fromLog) L.back += 2; a.nav_.setFocus("dec", "dec.log"); }
    else if (fnow == "dec.log.dn") { if (fromLog) L.back -= 2; a.nav_.setFocus("dec", "dec.log"); }
    prevFocus = a.nav_.focus("dec");
    L.back = std::max(0, std::min(L.back, std::max(0, rows - n)));
    const int first = std::max(0, rows - n - L.back);
    const Nav::Hit h = c.nav.item("dec.log", body);
    if (first > 0) c.nav.item("dec.log.up", {body.x + 10, body.y, body.w - 20, 2});
    if (L.back > 0) c.nav.item("dec.log.dn", {body.x + 10, body.b() - 2, body.w - 20, 2});
    if (h.pressed) L.back = 0;   // ★ ENTER on the text: back to the newest
    if (rows == 0 || (rows == 1 && L.row(0).empty())) {
        const auto& ln = c.tc.wrap(c.f.small, idle, (int)body.w - 16);
        float y = body.y + 4;
        for (size_t i = 0; i < ln.size() && y + c.f.small.lineHeight() <= body.b() + 2; i++, y += c.f.small.lineHeight())
            c.tc.draw(c.g, c.f.small, ln[i], body.x + 8, y, th::dim);
    } else {
        // ★ bottom-aligned: the newest row sits on the foot of the box
        float y = body.b() - std::min(n, rows) * lh;
        for (int k = first; k < std::min(rows, first + n); k++, y += lh) c.tc.draw(c.g, f, L.row((size_t)k), body.x + 8, y, th::white);
        if (rows > n) {
            const float frac = (float)n / rows, top = (float)first / rows;
            c.g.rrect(body.r() - 6, body.y + top * body.h, 3, std::max(10.f, frac * body.h), 1.5f, th::dim);
        }
        if (L.back > 0) {
            const float w = (float)c.tc.width(c.f.tiny, "NEWEST") + 30;
            const Rect nb{body.r() - w - 14, body.b() - 22, w, 20};
            c.g.rrect(nb.x, nb.y, nb.w, nb.h, 10, rgb(0, 0.85f));
            c.g.rframe(nb.x, nb.y, nb.w, nb.h, 10, 1, th::btnBorder);
            c.tc.draw(c.g, c.f.tiny, "NEWEST", nb.x + 9, nb.cy() - c.f.tiny.lineHeight() / 2.f, th::btnText);
            c.g.triangle(nb.r() - 18, nb.cy() - 3, nb.r() - 8, nb.cy() - 3, nb.r() - 13, nb.cy() + 4, th::btnText);
        }
    }
    if (h.focused) c.g.rframe(body.x - 1, body.y - 1, body.w + 2, body.h + 2, 7, 3, th::focus);
}

/** A two-part row: the left column (fixed width, amber) and the rest (white, cut to fit). */
inline void decRowText(Ctx& c, Rect rr, const std::string& left, float leftW, const std::string& right, Rgba lc, Rgba rc,
                       const std::string& tail = "", Rgba tc = th::dim) {
    const float cy = rr.cy();
    c.tc.draw(c.g, c.f.small, decFit(c, c.f.small, left, (int)leftW - 6), rr.x + 8, cy - c.f.small.lineHeight() / 2.f, lc);
    float tw = 0;
    if (!tail.empty()) { tw = (float)c.tc.width(c.f.small, tail) + 10; c.tc.draw(c.g, c.f.small, tail, rr.r() - 8, cy - c.f.small.lineHeight() / 2.f, tc, 2); }
    c.tc.draw(c.g, c.f.body, decFit(c, c.f.body, right, (int)(rr.w - leftW - 16 - tw)), rr.x + 8 + leftW, cy - c.f.body.lineHeight() / 2.f, rc);
}
inline void decRowBg(Ctx& c, Rect rr, bool on, Rgba tint = rgb(0, 0)) {
    c.g.rrect(rr.x, rr.y, rr.w, rr.h, 6, tint.a > 0 ? tint : on ? th::activeBg : rgb(0xffa000, 0.05f));
}
inline void decNone(Ctx& c, Rect view, const std::string& t) {
    const auto& ln = c.tc.wrap(c.f.small, t, (int)view.w - 16);
    float y = view.y + 4;
    for (size_t i = 0; i < ln.size() && y + c.f.small.lineHeight() <= view.b() + 2; i++, y += c.f.small.lineHeight())
        c.tc.draw(c.g, c.f.small, ln[i], view.x + 8, y, th::dim);
    c.nav.item("dec.none", view);   // ★ something to hold the highlight, so BACK still releases an empty box
}

inline Scroller& decListScroll() { static Scroller s; return s; }
inline DecAnchor& decSigAnchor() { static DecAnchor h; return h; }

/** CW / PSK / JTTY signals: frequency · speed or mode · SNR · call · text — press = tune to it. */
inline void decPaneSignals(App& a, Ctx& c, const RadioState& s, Rect view) {
    DecUi& u = decUi();
    const bool jtty = u.view == "jtty", psk = u.view == "psk";
    if (u.sigs.empty()) { decNone(c, view, jtty ? "listening — every JTTY signal in your passband is listed here as it starts" : psk ? "listening — every PSK signal in your passband is listed here" : "listening — every CW signal in your passband is listed here"); return; }
    const int hit = decRows(c, decListScroll(), view, (int)u.sigs.size(), 30, "dec.sig.", [&](int k) { return std::to_string((long long)u.sigs[(size_t)k].id); },
        [&](int k, Rect rr, bool) {
            const DecSig& g = u.sigs[(size_t)k];
            decRowBg(c, rr, g.active);
            std::string spd;
            if (jtty) spd = std::to_string((int)g.snr) + " dB";
            else if (psk) spd = (g.baud == 63 ? "BPSK63 " : "BPSK31 ") + std::to_string((int)g.snr) + " dB";
            else {
                const int lo = (int)std::lround(std::min(g.wpmLo, g.wpmHi)), hi = (int)std::lround(std::max(g.wpmLo, g.wpmHi));
                spd = g.wpmLo > 0 && g.wpmHi > 0 && lo != hi ? std::to_string(lo) + "–" + std::to_string(hi) + " wpm" : std::to_string((int)std::lround(g.wpm)) + " wpm";
            }
            const std::string fq = g.hz > 0 ? decKHz(g.hz) : std::to_string((int)g.audio) + " Hz";
            // ★★ the NEWEST of the text (decFitTail), after the call — it was cut at its END, so the row showed the
            //    oldest of the server's 120 characters and stood still while the signal was decoded (Stuart, 2026-11-12)
            const float cy = rr.cy();
            c.tc.draw(c.g, c.f.small, decFit(c, c.f.small, fq, 86), rr.x + 8, cy - c.f.small.lineHeight() / 2.f, decQCol(g.q));
            const float tw = (float)c.tc.width(c.f.small, spd) + 10;
            c.tc.draw(c.g, c.f.small, spd, rr.r() - 8, cy - c.f.small.lineHeight() / 2.f, th::dim, 2);
            float x = rr.x + 8 + 92;
            if (!g.call.empty()) x += (float)c.tc.draw(c.g, c.f.body, g.call, x, cy - c.f.body.lineHeight() / 2.f, g.active ? th::white : th::white68) + 12;
            const std::string tx = !psk && !jtty && decCwPlain(a) ? cwPlainEnglish(g.text) : g.text;
            c.tc.draw(c.g, c.f.body, decFitTail(c, c.f.body, tx, (int)(rr.r() - 8 - tw - x)), x, cy - c.f.body.lineHeight() / 2.f,
                      g.active ? th::white : th::white68);
        }, &decSigAnchor());
    if (hit >= 0) {
        const DecSig& g = u.sigs[(size_t)hit];
        // ★★ CW: a press READS the signal (its own history, RTTY's reading — decPaneRead); TUNE there tunes to it
        if (!psk && !jtty) { decReadSig((long long)g.id); a.nav_.forget("dec"); return; }
        if (!(g.hz > 0)) return;
        if (jtty) decTune(a, s, g.hz - 1500, "usb");          // jtty.ts jttyTuneFor: tone 0 mid-passband
        else if (psk) {
            const bool lsb = s.mode == "lsb";
            const int cc = lsb || s.mode == "usb" ? decPassbandCentre(s) : 1500;
            decTune(a, s, lsb ? g.hz + cc : g.hz - cc, lsb ? "lsb" : "usb");
        } else decTune(a, s, g.hz, "cwu");
    }
}
/** ★★ ONE CW SIGNAL READ (a SIGNALS row pressed): its decoded history, newest at the foot, following it — and, the
 *  box grabbed, UP / DOWN scroll back through it and the place holds as text arrives (decDrawLog, RTTY's). The header
 *  has TUNE (tune to it — what the row's press did before) and SIGNALS / BACK return to the list. */
inline void decPaneRead(App& a, Ctx& c, const RadioState& s, Rect view) {
    DecUi& u = decUi();
    const DecSig* g = nullptr;
    for (auto& x : u.sigs) if ((long long)x.id == u.readSig) g = &x;
    // the signal's line: frequency · call · speed (or "no longer heard")
    std::string head = g ? (g->hz > 0 ? decKHz(g->hz) + " kHz" : std::to_string((int)g->audio) + " Hz") : std::string("no longer heard");
    if (g && !g->call.empty()) head += "  " + g->call;
    if (g && g->wpm > 0) head += "  · " + std::to_string((int)std::lround(g->wpm)) + " wpm";
    const float hh = (float)c.f.small.lineHeight() + 2;
    c.tc.draw(c.g, c.f.small, decFit(c, c.f.small, head, (int)view.w - 16), view.x + 8, view.y, g ? decQCol(g->q) : th::dim);
    const Rect body{view.x, view.y + hh, view.w, view.h - hh};
    decDrawLog(a, c, body, "nothing decoded from this signal yet — its text appears here as it is heard", &u.sigLog);
    (void)s;
}
/** The callsigns heard (cw_spot / psk_spot / jtty_spot), newest first — press = tune. */
inline void decPaneCalls(App& a, Ctx& c, const RadioState& s, Rect view) {
    DecUi& u = decUi();
    if (u.calls.empty()) { decNone(c, view, "no callsigns heard yet — a station identifying itself appears here"); return; }
    const int n = std::min(80, (int)u.calls.size());
    const int hit = decRows(c, decListScroll(), view, n, 30, "dec.call.", [&](int k) { return u.calls[(size_t)k].call; },
        [&](int k, Rect rr, bool) {
            const DecCall& x = u.calls[(size_t)k];
            decRowBg(c, rr, false);
            decRowText(c, rr, decHms(x.at), 62, x.call + (x.how.empty() ? "" : "  " + x.how) + (x.country.empty() ? "" : "  · " + x.country),
                       th::dim, x.how == "CQ" ? rgb(0x7dffb0) : th::white, x.hz > 0 ? decKHz(x.hz) : "");
        });
    if (hit >= 0) {
        const DecCall& x = u.calls[(size_t)hit];
        if (u.view == "jtty") decTune(a, s, x.hz - 1500, "usb");
        else if (u.view == "psk") decTune(a, s, x.hz - decPassbandCentre(s), "usb");
        else decTune(a, s, x.hz, "cwu");
    }
}
/** DIGITAL SPOTS (renderSpots): newest per call + band, time · mode · band · SNR · call · country · grid — press = tune. */
/** ★★ THE FT8 CLOCK LINE (Stuart, 2026-11-08): with CLOCK AUTO-SET on, the top of DIGITAL SPOTS says what the clock is
 *  doing — measuring (amber: "decodes may be poor until then") or "System clock synchronised to FT8 signals" (green).
 *  Off, it says nothing unless FT8 shows the clock half a second or more out — then where the key is. */
inline float decFt8ClockLine(App& a, Ctx& c, Rect view) {
    const HwFt8Clock& k = hwFt8Clock();
    const double now = hwNow();
    std::string t; Rgba col = th::amber;
    if (a.set_.clockAuto) {
        if (now - k.setAt < 120) {
            char b[96]; std::snprintf(b, sizeof b, "System clock synchronised to FT8 signals (moved %+.2f s)", k.setMs / 1000.0);
            t = b; col = rgb(0x7dffb0);
        } else if (now - k.syncedAt < 600) { t = "System clock synchronised to FT8 signals"; col = rgb(0x7dffb0); }
        else if (now < k.holdUntil) t = "Clock set — measuring FT8 timing again from the next cycle";
        else t = "Clock auto-set: measuring FT8 timing (" + std::to_string(std::min(k.agreeing, 3)) + " of 3 cycles). "
                 "The clock adjusts after 3 cycles; until then decodes may be poor.";
    } else {
        double dt = 0; int n = 0;
        if (hwFt8Median(dt, n) && std::fabs(dt) >= 0.5) {
            char b[128]; std::snprintf(b, sizeof b, "FT8 shows the clock %.1f s %s — MENU › HARDWARE › ALIGN TO FT8 (or CLOCK AUTO-SET ON)",
                                       std::fabs(dt), dt > 0 ? "fast" : "slow");
            t = b;
        }
    }
    if (t.empty()) return 0;
    const auto lines = c.tc.wrap(c.f.small, t, (int)view.w - 16);
    float y = view.y + 2;
    for (auto& l : lines) { c.tc.draw(c.g, c.f.small, l, view.x + 8, y, col); y += c.f.small.lineHeight(); }
    return (float)lines.size() * c.f.small.lineHeight() + 6;
}
inline void decPaneSpots(App& a, Ctx& c, const RadioState& s, Rect view) {
    DecUi& u = decUi();
    { const float h = decFt8ClockLine(a, c, view); view.y += h; view.h -= h; }
    if (u.spots.empty()) { decNone(c, view, "listening — the spots arrive at the end of each 15 s slot (FT8) as the server decodes them. Tune a digital frequency in USB (14.074 MHz FT8)."); return; }
    std::vector<const DecSpot*> rows;
    for (auto& sp : u.spots) {
        bool dup = false;
        for (auto* r : rows) if (r->call == sp.call && r->band == sp.band) { dup = true; break; }
        if (!dup) rows.push_back(&sp);
        if (rows.size() >= 80) break;
    }
    const int hit = decRows(c, decListScroll(), view, (int)rows.size(), 30, "dec.spot.", [&](int k) { return rows[(size_t)k]->call + "|" + rows[(size_t)k]->band; },
        [&](int k, Rect rr, bool) {
            const DecSpot& sp = *rows[(size_t)k];
            decRowBg(c, rr, false);
            char snr[16]; std::snprintf(snr, sizeof snr, "%+d", (int)sp.snr);
            const float cy = rr.cy();
            float x = rr.x + 8;
            c.tc.draw(c.g, c.f.small, decHms(sp.at), x, cy - c.f.small.lineHeight() / 2.f, th::dim); x += 50;
            c.tc.draw(c.g, c.f.tiny, sp.mode, x, cy - c.f.tiny.lineHeight() / 2.f, th::btnText); x += 40;
            c.tc.draw(c.g, c.f.small, sp.band, x, cy - c.f.small.lineHeight() / 2.f, th::dim); x += 44;
            c.tc.drawTab(c.g, c.f.small, snr, x + 30, cy - c.f.small.lineHeight() / 2.f, sp.snr >= 0 ? rgb(0x7dffb0) : rgb(0xffc966), 2); x += 42;
            const float cw = (float)c.tc.draw(c.g, c.f.bodyB, sp.call, x, cy - c.f.bodyB.lineHeight() / 2.f, th::white) + 10;
            const std::string tail = sp.grid;
            if (!tail.empty()) c.tc.draw(c.g, c.f.small, tail, rr.r() - 8, cy - c.f.small.lineHeight() / 2.f, th::dim, 2);
            const float tw = tail.empty() ? 0.f : (float)c.tc.width(c.f.small, tail) + 12;
            c.tc.draw(c.g, c.f.small, decFit(c, c.f.small, sp.country, (int)(rr.r() - 8 - tw - x - cw)), x + cw, cy - c.f.small.lineHeight() / 2.f, th::white68);
        });
    if (hit >= 0) decTune(a, s, rows[(size_t)hit]->hz, "usb");   // ★ the web's row tap: the spot's RF in USB
}
/** DSC: the calls, newest first, coloured by what they are (distress red, urgency orange, safety amber), "×5" repeats. */
inline void decPaneDsc(App& a, Ctx& c, const RadioState& s, Rect view) {
    DecUi& u = decUi();
    (void)a; (void)s;
    if (u.dsc.empty()) { decNone(c, view, "listening — each DSC call appears here as it is decoded. A few an hour on each frequency; most are test calls to coast stations and their acknowledgements."); return; }
    const float rh = 2 * c.f.small.lineHeight() + 8;
    decRows(c, decListScroll(), view, (int)u.dsc.size(), rh, "dec.dsc.", [&](int k) { return std::to_string((long long)u.dsc[(size_t)k].id) + "." + std::to_string((long long)u.dsc[(size_t)k].rf); },
        [&](int k, Rect rr, bool) {
            const DecDsc& x = u.dsc[(size_t)k];
            static const unsigned cols[] = {0xffe9c0, 0xffc966, 0xff9a5a, 0xff6a6a};
            decRowBg(c, rr, false, x.level == 3 ? rgb(0xe05050, 0.22f) : x.level == 2 ? rgb(0xff8a3d, 0.14f) : rgb(0xffa000, 0.05f));
            const std::string t = x.line + (x.repeat > 1 ? "  ×" + std::to_string(x.repeat) : "");
            const auto& ln = c.tc.wrap(c.f.small, t, (int)rr.w - 16);
            float y = rr.y + 3;
            for (size_t i = 0; i < ln.size() && i < 2; i++, y += c.f.small.lineHeight())
                c.tc.draw(c.g, c.f.small, i == 1 && ln.size() > 2 ? decFit(c, c.f.small, ln[i] + " …", (int)rr.w - 16) : ln[i], rr.x + 8, y, rgb(cols[x.level]));
        });
}
/** APRS: the stations (name · what it sent · age) and the packets as they came. The map is the map pass's. */
inline void decPaneAprs(App& a, Ctx& c, const RadioState& s, Rect view) {
    DecUi& u = decUi();
    (void)a; (void)s;
    if (u.sub == 0) {
        if (u.aprs.empty()) { decNone(c, view, u.aprsBaud == 300 ? "listening — HF packet, 300 Bd; stations appear with their first packet" : "listening — APRS, 1200 Bd; stations appear with their first packet"); return; }
        decRows(c, decListScroll(), view, (int)u.aprs.size(), 30, "dec.ap.", [&](int k) { return u.aprs[(size_t)k].key; },
            [&](int k, Rect rr, bool) {
                const DecAprsSt& x = u.aprs[(size_t)k];
                decRowBg(c, rr, x.seen < 60);
                const double sn = x.seen;
                const std::string ago = sn < 60 ? std::to_string((int)sn) + "s" : sn < 3600 ? std::to_string((int)(sn / 60)) + "m" : std::to_string((int)(sn / 3600)) + "h";
                const Rgba kc = x.kind == "weather" ? rgb(0x39d0d8) : x.kind == "object" || x.kind == "item" ? rgb(0xffad42) : rgb(0x59c26b);
                decRowText(c, rr, x.name, 130, x.line, kc, th::white, ago);
            });
    } else {
        if (u.aprsLog.empty()) { decNone(c, view, "no packets yet"); return; }
        const float rh = 2 * c.f.small.lineHeight() + 8;
        decRows(c, decListScroll(), view, (int)u.aprsLog.size(), rh, "dec.pk.", [&](int k) { return std::to_string(k); },
            [&](int k, Rect rr, bool) {
                const DecAprsPkt& p = u.aprsLog[(size_t)k];
                decRowBg(c, rr, false);
                const auto& ln = c.tc.wrap(c.f.small, decHms(p.at) + "  " + p.line, (int)rr.w - 16);
                float y = rr.y + 3;
                for (size_t i = 0; i < ln.size() && i < 2; i++, y += c.f.small.lineHeight()) c.tc.draw(c.g, c.f.small, ln[i], rr.x + 8, y, th::white);
            });
    }
}

// ── the picker (the web's decoders panel) ────────────────────────────────────────────────────────────────────────────

/** One row of the picker. */
struct DecPRow {
    enum Kind { HEAD, TOGGLE, CHOICE, ACTION, HINT, TABS } kind;
    explicit DecPRow(Kind k) : kind(k) {}
    std::string id, label;
    std::vector<std::string> opts;
    int sel = -1;                         // CHOICE: the value; ACTION: unused
    std::vector<bool> lit;                // ACTION: the channel the dial is on
    std::function<void(int)> pick;
    bool on = false;                      // TOGGLE
    std::string why;                      // TOGGLE: out of this radio's reach (greyed)
};

inline std::map<std::string, int>& decCursor() { static std::map<std::string, int> m; return m; }
inline Scroller& decPickScroll() { static Scroller s; return s; }

/** The rows under a decoder that is ON — its options, the web's subPanel, in its order. */
inline void decOptionRows(App& a, const RadioState& s, const std::string& id, std::vector<DecPRow>& out) {
    DecUi& u = decUi();
    auto choice = [&](const std::string& rid, const std::string& label, std::vector<std::string> opts, int sel, std::function<void(int)> f) {
        DecPRow r{DecPRow::CHOICE}; r.id = rid; r.label = label; r.opts = std::move(opts); r.sel = sel; r.pick = std::move(f); out.push_back(std::move(r));
    };
    auto action = [&](const std::string& rid, const std::string& label, std::vector<std::string> opts, std::vector<bool> lit, std::function<void(int)> f) {
        if (opts.empty()) return;
        DecPRow r{DecPRow::ACTION}; r.id = rid; r.label = label; r.opts = std::move(opts); r.lit = std::move(lit); r.pick = std::move(f); out.push_back(std::move(r));
    };
    auto hint = [&](const std::string& t) { if (t.empty()) return; DecPRow r{DecPRow::HINT}; r.id = "h." + id; r.label = t; out.push_back(std::move(r)); };
    if (id == "rtty") {
        auto& R = u.rtty;
        // PRESET (RTTY_PRESETS — verbatim from the app): AUTO is the default, one press
        struct P { const char* k; double sh, bd; const char* enc; bool inv; };
        static const P pre[] = {{"HAM", 170, 45.45, "ITA2", false}, {"WEATHER", 450, 50, "ITA2", true}, {"SITOR-B", 170, 100, "CCIR476", false}};
        int psel = R.autoOn ? 0 : -1;
        if (!R.autoOn) for (int i = 0; i < 3; i++) if (R.shift == pre[i].sh && R.baud == pre[i].bd && R.enc == pre[i].enc && R.inv == pre[i].inv) psel = i + 1;
        choice("rtty.pre", "PRESET", {"AUTO", "HAM", "WEATHER", "SITOR-B"}, psel, [&R](int i) {
            R = DecUi::Rtty{};
            if (i > 0) { R.autoOn = false; R.shift = pre[i - 1].sh; R.baud = pre[i - 1].bd; R.enc = pre[i - 1].enc; R.inv = pre[i - 1].inv; }
            decReattachSoon("rtty");
        });
        // ★ a value picked by hand turns AUTO off (and the preset row then shows the preset it matches, or none)
        auto manual = [&R] { R.autoOn = false; decReattachSoon("rtty"); };
        static const double shifts[] = {85, 170, 200, 425, 450, 850};
        static const double bauds[] = {45.45, 50, 56.88, 75, 100, 110, 150, 200, 300};
        int ss = -1, bs = -1;
        for (int i = 0; i < 6; i++) if (!R.autoOn && R.shift == shifts[i]) ss = i;
        for (int i = 0; i < 9; i++) if (!R.autoOn && R.baud == bauds[i]) bs = i;
        choice("rtty.shift", "SHIFT", {"85", "170", "200", "425", "450", "850"}, ss, [&R, manual](int i) { R.shift = shifts[i]; manual(); });
        choice("rtty.baud", "BAUD", {"45.45", "50", "56.88", "75", "100", "110", "150", "200", "300"}, bs, [&R, manual](int i) { R.baud = bauds[i]; manual(); });
        static const char* encs[] = {"ITA2", "ASCII", "CCIR476"};
        int es = -1; for (int i = 0; i < 3; i++) if (!R.autoOn && R.enc == encs[i]) es = i;
        choice("rtty.enc", "ENCODING", {"ITA2", "ASCII", "SITOR-B"}, es, [&R, manual](int i) {
            R.enc = encs[i];
            if (R.enc == "ASCII" && R.stop == 1.5) R.stop = 1;   // 1.5 is the 5-bit Baudot case only
            manual();
        });
        if (R.enc == "ASCII") {
            choice("rtty.bits", "DATA BITS", {"7", "8"}, R.autoOn ? -1 : (R.bits == 8 ? 1 : 0), [&R, manual](int i) { R.bits = i ? 8 : 7; manual(); });
            static const char par[] = {'N', 'E', 'O', 'M', 'S'};
            int ps = -1; for (int i = 0; i < 5; i++) if (!R.autoOn && R.parity == par[i]) ps = i;
            choice("rtty.par", "PARITY", {"NONE", "EVEN", "ODD", "MARK", "SPACE"}, ps, [&R, manual](int i) { R.parity = par[i]; manual(); });
        }
        if (R.enc != "CCIR476") {   // rttyStops: none for SITOR-B, no 1.5 for ASCII
            const bool ascii = R.enc == "ASCII";
            std::vector<std::string> o = ascii ? std::vector<std::string>{"1", "2"} : std::vector<std::string>{"1", "1.5", "2"};
            int st = -1;
            if (!R.autoOn) st = ascii ? (R.stop == 2 ? 1 : 0) : (R.stop == 1 ? 0 : R.stop == 2 ? 2 : 1);
            choice("rtty.stop", "STOP BITS", o, st, [&R, manual, ascii](int i) { R.stop = ascii ? (i ? 2 : 1) : (i == 0 ? 1 : i == 2 ? 2 : 1.5); manual(); });
        }
        choice("rtty.inv", "INVERTED", {"OFF", "ON"}, R.autoOn ? -1 : (R.inv ? 1 : 0), [&R, manual](int i) { R.inv = i == 1; manual(); });
        if (R.enc == "ITA2") choice("rtty.usos", "UNSHIFT ON SPACE", {"OFF", "ON"}, R.autoOn ? -1 : (R.usos ? 1 : 0), [&R, manual](int i) { R.usos = i == 1; manual(); });
    } else if (id == "time") {
        std::vector<std::string> o; int sel = 0;
        for (size_t i = 0; i < sizeof kTimeStations / sizeof kTimeStations[0]; i++) { o.push_back(kTimeStations[i].key); if (u.timeStation == kTimeStations[i].id) sel = (int)i; }
        choice("time.st", "STATION", o, sel, [](int i) { decUi().timeStation = kTimeStations[i].id; decReattachSoon("time"); });
        // ★ the station's own frequency AND mode, one press (WWV in AM, the rest in CW)
        const DecTimeStation& t = kTimeStations[sel];
        if (decReach(s, t.hz)) {
            char lb[64]; std::snprintf(lb, sizeof lb, "%s %s", t.hz >= 1e6 ? (decMHz(t.hz) + " MHz").c_str() : (decKHz(t.hz) + " kHz").c_str(), std::string(t.mode) == "am" ? "AM" : "CW");
            action("time.tune", "TUNE", {lb}, {std::fabs(s.vfoHz - t.hz) < 50 && s.mode == t.mode}, [&a, &s, t](int) { decTune(a, s, t.hz, t.mode); });
        }
        hint(std::string(t.hint) + (std::string(t.id) == "rwm" ? " The fields fill in as they arrive." : " The fields fill in as they arrive; a reading is only shown once a second minute confirms it — then MENU › HARDWARE can set the clock from it."));
    } else if (id == "cw" || id == "psk") {
        const bool cw = id == "cw";
        bool skimOk; { std::lock_guard<std::mutex> lk(u.capMtx); skimOk = u.skimOk; }
        bool& skim = cw ? u.cwSkim : u.pskSkim;
        if (!skimOk && skim) { skim = false; decReattachSoon(id); }
        if (skimOk) choice(id + ".view", "VIEW", {"ONE SIGNAL", "SKIMMER"}, skim ? 1 : 0, [id](int i) {
            DecUi& x = decUi(); (id == "cw" ? x.cwSkim : x.pskSkim) = i == 1; x.sub = 0; decReadSig(0); decReattachSoon(id); });
        if (skimOk && skim) {
            int& w = cw ? u.cwWidth : u.pskWidth;
            const std::vector<int> ws = cw ? std::vector<int>{24000, 48000, 96000} : std::vector<int>{6000, 12000, 24000};
            int sel = 1; for (int i = 0; i < 3; i++) if (w == ws[(size_t)i]) sel = i;
            choice(id + ".w", "SEGMENT", cw ? std::vector<std::string>{"24 kHz", "48 kHz", "96 kHz"} : std::vector<std::string>{"6 kHz", "12 kHz", "24 kHz"}, sel,
                   [id, ws](int i) { DecUi& x = decUi(); (id == "cw" ? x.cwWidth : x.pskWidth) = ws[(size_t)i]; decReattachSoon(id); });
        }
    } else if (id == "aprs") {
        double hz; { std::lock_guard<std::mutex> lk(u.capMtx); hz = u.aprsHz; }
        choice("aprs.baud", "PACKET", {"VHF 1200", "HF 300"}, u.aprsBaud == 300 ? 1 : 0, [](int i) { decUi().aprsBaud = i ? 300 : 1200; decReattachSoon("aprs"); });
        const double th = u.aprsBaud == 300 ? 10147600 : hz;
        const std::string tm = u.aprsBaud == 300 ? "usb" : "nfm";
        if (decReach(s, th)) action("aprs.tune", "TUNE", {decMHz(th) + (u.aprsBaud == 300 ? " USB" : " NFM")}, {std::fabs(s.vfoHz - th) < 500 && s.mode == tm},
                                    [&a, &s, th, tm](int) { decTune(a, s, th, tm); });
        hint(u.aprsBaud == 300 ? "HF packet: 300 Bd on 10.1476 MHz USB (30 m; 14.105 on 20 m). The decoder tries a few tunings either side, so close is close enough. The speaker is muted while it listens (AUDIO unmutes it)."
                               : "VHF APRS: " + decMHz(hz) + " MHz NFM here. Every station heard is listed, with its position, weather, messages and the digipeater that carried it. The speaker is muted while it listens (AUDIO unmutes it).");
    } else if (id == "dsc") {
        std::vector<std::string> o; std::vector<bool> lit; std::vector<DecDscCh> chs;
        for (auto& ch : kDscDistress) {
            double hz; std::string m; decDscTuneFor(ch, hz, m);
            if (!decReach(s, hz)) continue;   // ★ only the channels this radio reaches (AGENTS.md: no dead controls)
            o.push_back(ch.label); lit.push_back(decDscOn(ch, s.vfoHz, s.mode)); chs.push_back(ch);
        }
        action("dsc.ch", "DISTRESS", o, lit, [&a, &s, chs](int i) {
            double hz; std::string m; decDscTuneFor(chs[(size_t)i], hz, m);
            decTune(a, s, hz, m);
            decReattachSoon("dsc");   // ★ the decoder's tones follow the dial (main.ts dscTune)
        });
    } else if (id == "jtty") {
        std::vector<std::string> o; std::vector<bool> lit; std::vector<double> hzs;
        for (auto& d : kJttyDials) { if (!decReach(s, d.hz)) continue; o.push_back(d.label); lit.push_back(std::fabs(s.vfoHz - d.hz) < 500 && s.mode == "usb"); hzs.push_back(d.hz); }
        action("jtty.band", "BAND", o, lit, [&a, &s, hzs](int i) { decTune(a, s, hzs[(size_t)i], "usb"); });
    } else if (id == "wefax") {
        // main.ts #wefaxLpm (60 · 120 · 240) — a re-attach; the chart's own ADJ (margin / slant / RAW) is in the box
        static const int lpms[] = {60, 120, 240};
        int sel = 1; for (int i = 0; i < 3; i++) if (picUi().lpm == lpms[i]) sel = i;
        choice("wefax.lpm", "LPM", {"60", "120", "240"}, sel, [](int i) { picUi().lpm = lpms[i]; decReattachSoon("wefax"); });
    } else if (id == "mfsk") {
        // ★★★ AUTO's fallback rows (autoModes.ts, RTTY's pattern): FAMILY (AUTO + the server's groups), then the family's MODE
        std::vector<DecGMode> list; { std::lock_guard<std::mutex> lk(u.capMtx); list = u.mfskModes; }
        std::vector<std::string> groups;
        for (auto& m : list) if (!m.group.empty() && std::find(groups.begin(), groups.end(), m.group) == groups.end()) groups.push_back(m.group);
        std::string g;
        for (auto& m : list) if (m.id == u.mfskMode) g = m.group;
        std::vector<std::string> o = {"AUTO"}; int sel = 0;
        for (size_t i = 0; i < groups.size(); i++) { o.push_back(upper(groups[i])); if (groups[i] == g) sel = (int)i + 1; }
        choice("mfsk.fam", "FAMILY", o, sel, [list, groups](int i) {
            DecUi& x = decUi();
            if (i == 0) x.mfskMode = "auto";
            else for (auto& m : list) if (m.group == groups[(size_t)i - 1]) { x.mfskMode = m.id; break; }   // the group's own "ANY"
            decReattachSoon("mfsk");
        });
        std::vector<const DecGMode*> in;
        for (auto& m : list) if (!g.empty() && m.group == g) in.push_back(&m);
        if (in.size() > 1) {   // ★ the second row only when the family has a real choice to make
            std::vector<std::string> mo; int ms = 0; std::vector<std::string> ids;
            for (size_t i = 0; i < in.size(); i++) { mo.push_back(in[i]->label); ids.push_back(in[i]->id); if (in[i]->id == u.mfskMode) ms = (int)i; }
            choice("mfsk.mode", "MODE", mo, ms, [ids](int i) { decUi().mfskMode = ids[(size_t)i]; decReattachSoon("mfsk"); });
        }
    }
    const DecDef* d = decDef(id);
    if (d && id != "time" && id != "aprs") hint(d->hint);
}

/** Draw one picker row at `rr`; returns its height (the caller lays them out). */
inline void decDrawPRow(App& a, Ctx& c, const RadioState& s, DecPRow& r, Rect rr) {
    (void)s;
    if (r.kind == DecPRow::HEAD) {
        c.tc.draw(c.g, c.f.tiny, r.label, rr.x + 4, rr.b() - c.f.tiny.lineHeight() - 2, th::dim);
        c.g.rect(rr.x + 4 + c.tc.width(c.f.tiny, r.label) + 8, rr.b() - c.f.tiny.lineHeight() / 2.f - 3, rr.w - 16 - c.tc.width(c.f.tiny, r.label), 1, th::barBorder);
        return;
    }
    if (r.kind == DecPRow::HINT) {
        const auto& ln = c.tc.wrap(c.f.small, r.label, (int)rr.w - 40);
        float y = rr.y + 2;
        for (auto& l : ln) { c.tc.draw(c.g, c.f.small, l, rr.x + 28, y, th::dim); y += c.f.small.lineHeight(); }
        return;
    }
    if (r.kind == DecPRow::TOGGLE) {
        const Nav::Hit h = c.nav.item("dec.p." + r.id, rr);
        const bool grey = !r.why.empty();
        c.g.rrect(rr.x, rr.y, rr.w, rr.h, 8, r.on ? th::activeBg : th::btnBg);
        c.g.rframe(rr.x, rr.y, rr.w, rr.h, 8, 1, r.on ? th::activeBdr : grey ? th::dimmer : th::btnBorder);
        const float tw = (float)c.tc.draw(c.g, c.f.bodyB, r.label, rr.x + 12, rr.cy() - c.f.bodyB.lineHeight() / 2.f, grey ? th::dimmer : r.on ? th::activeTx : th::btnText);
        if (grey) c.tc.draw(c.g, c.f.small, decFit(c, c.f.small, r.why, (int)(rr.w - tw - 110)), rr.x + 24 + tw, rr.cy() - c.f.small.lineHeight() / 2.f, th::dimmer);
        const Rect chip{rr.r() - 70, rr.y + 5, 60, rr.h - 10};
        c.g.rrect(chip.x, chip.y, chip.w, chip.h, 6, r.on ? th::onBg : th::btnBg);
        c.g.rframe(chip.x, chip.y, chip.w, chip.h, 6, 1, r.on ? th::onBg : th::btnBorder);
        textIn(c, c.f.tiny, r.on ? "ON" : "OFF", chip, r.on ? th::onText : grey ? th::dimmer : th::btnText);
        if (h.focused) focusRing(c, rr, 8);
        if (h.pressed && r.pick) r.pick(0);
        return;
    }
    // CHOICE / ACTION / TABS: one full-width stop; LEFT / RIGHT change it (CHOICE, TABS) or move the cursor (ACTION)
    const std::string nid = "dec.p." + r.id;
    const Nav::Hit h = c.nav.item(nid, rr, true);
    const bool action = r.kind == DecPRow::ACTION;
    int& cur = decCursor()[r.id];
    const int n = (int)r.opts.size();
    if (action) {
        if (cur < 0 || cur >= n) { cur = 0; for (int i = 0; i < n; i++) if (i < (int)r.lit.size() && r.lit[(size_t)i]) cur = i; }
        if (h.adjust) cur = std::max(0, std::min(n - 1, cur + h.adjust));
        if (h.pressed && r.pick && n) r.pick(cur);
    } else {
        int nv = r.sel;
        if (h.adjust) nv = r.sel < 0 ? (h.adjust > 0 ? 0 : n - 1) : std::max(0, std::min(n - 1, r.sel + h.adjust));
        if (h.pressed) nv = r.sel < 0 ? 0 : (r.sel + 1) % n;
        if (nv != r.sel && nv >= 0 && r.pick) { r.pick(nv); r.sel = nv; }
    }
    const bool indent = r.kind != DecPRow::TABS;
    const float lx = rr.x + (indent ? 22 : 4);
    c.g.rrect(lx - 6, rr.y, rr.r() - lx + 6, rr.h, 7, rgb(0xffa000, 0.04f));
    c.tc.draw(c.g, c.f.tiny, r.label, lx, rr.cy() - c.f.tiny.lineHeight() / 2.f, th::dim);
    const float ox = lx + 128, ow = rr.r() - 8 - ox;
    // the choices as chips; when they do not fit, the chosen one between arrows
    const Font* f = &c.f.small;
    float total = 0;
    for (auto& o : r.opts) total += c.tc.width(*f, o) + 16 + 5;
    if (total > ow) { f = &c.f.tiny; total = 0; for (auto& o : r.opts) total += c.tc.width(*f, o) + 14 + 4; }
    const int shown = action ? cur : r.sel;
    if (total <= ow) {
        float x = ox;
        const float pad = f == &c.f.tiny ? 14.f : 16.f, gap = f == &c.f.tiny ? 4.f : 5.f;
        for (int i = 0; i < n; i++) {
            const float w = (float)c.tc.width(*f, r.opts[(size_t)i]) + pad;
            const Rect b{x, rr.y + 4, w, rr.h - 8};
            const bool lit = action ? (i < (int)r.lit.size() && r.lit[(size_t)i]) : i == r.sel;
            c.g.rrect(b.x, b.y, b.w, b.h, 5, lit ? th::onBg : th::btnBg);
            c.g.rframe(b.x, b.y, b.w, b.h, 5, 1, lit ? th::onBg : th::btnBorder);
            if (action && h.focused && i == cur) c.g.rframe(b.x - 3, b.y - 3, b.w + 6, b.h + 6, 7, 2, th::amber);
            textIn(c, *f, r.opts[(size_t)i], b, lit ? th::onText : th::btnText);
            x += w + gap;
        }
    } else {
        const std::string t = shown >= 0 && shown < n ? r.opts[(size_t)shown] : "—";
        const Rect b{ox + 26, rr.y + 4, ow - 52, rr.h - 8};
        const bool lit = action ? (shown >= 0 && shown < (int)r.lit.size() && r.lit[(size_t)shown]) : shown >= 0;
        c.g.rrect(b.x, b.y, b.w, b.h, 5, lit ? th::onBg : th::btnBg);
        textIn(c, c.f.small, t + "  (" + std::to_string(shown + 1) + "/" + std::to_string(n) + ")", b, lit ? th::onText : th::btnText);
        c.g.triangle(ox + 18, rr.cy() - 7, ox + 18, rr.cy() + 7, ox + 6, rr.cy(), th::btnText);
        c.g.triangle(ox + ow - 18, rr.cy() - 7, ox + ow - 18, rr.cy() + 7, ox + ow - 6, rr.cy(), th::btnText);
    }
    if (h.focused) focusRing(c, rr, 7, true);
    (void)a;
}

/** The picker's rows for the band tab: BAND tabs, then each group's heading and its decoders, options under the ON one. */
inline void decPickRows(App& a, const RadioState& s, std::vector<DecPRow>& out) {
    DecUi& u = decUi();
    auto offeredIn = [&](int band) {
        for (auto& g : decPlaces(band)) for (auto* id : g.ids) { const DecDef* d = decDef(id); if (d && decOffered(s, *d)) return true; }
        for (auto& e : decExternals()) if (e.band == band && decExtOffered(s, e)) return true;
        return false;
    };
    const bool hf = offeredIn(0), vhf = offeredIn(1);
    if (u.pickBand < 0) u.pickBand = (s.vfoHz >= 30e6 && vhf) || !hf ? 1 : 0;   // decoderBands.ts openingBand: the dial's band
    if (hf && vhf) {
        DecPRow t{DecPRow::TABS}; t.id = "band"; t.label = "BAND"; t.opts = {"HF (with LF / MF)", "VHF"}; t.sel = u.pickBand;
        t.pick = [](int i) { decUi().pickBand = i; decPickScroll() = {}; };
        out.push_back(std::move(t));
    } else u.pickBand = vhf && !hf ? 1 : 0;
    // the registry's decoders (decRegister) join their group — or make it, after the built-in ones
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    for (auto& g : decPlaces(u.pickBand)) { std::vector<std::string> ids(g.ids.begin(), g.ids.end()); groups.push_back({g.group, ids}); }
    for (auto& e : decExternals()) {
        if (e.band != u.pickBand) continue;
        bool put = false;
        for (auto& g : groups) if (g.first == e.group) { g.second.push_back(e.id); put = true; }
        if (!put) groups.push_back({e.group, {e.id}});
    }
    for (auto& g : groups) {
        bool head = false;
        for (auto& id : g.second) {
            if (DecExternal* e = decExt(id)) {
                if (!decExtOffered(s, *e)) continue;
                if (!head) { DecPRow h{DecPRow::HEAD}; h.id = "g." + g.first; h.label = g.first; out.push_back(std::move(h)); head = true; }
                DecPRow r{DecPRow::TOGGLE};
                r.id = id; r.label = e->label; r.on = decIsOn(id);
                const std::string sid = id;
                r.pick = [&a, &s, sid](int) { decToggle(a, s, sid); decUi().pickFocus = "dec.p." + sid; };
                out.push_back(std::move(r));
                if (decIsOn(id) && !e->hint.empty()) { DecPRow h{DecPRow::HINT}; h.id = "h." + id; h.label = e->hint; out.push_back(std::move(h)); }
                continue;
            }
            const DecDef* d = decDef(id);
            if (!d || !decOffered(s, *d)) continue;
            if (!head) { DecPRow h{DecPRow::HEAD}; h.id = "g." + g.first; h.label = g.first; out.push_back(std::move(h)); head = true; }
            DecPRow r{DecPRow::TOGGLE};
            r.id = id; r.label = d->label; r.on = decIsOn(id); r.why = decWhyNot(id);
            const std::string sid = id;
            r.pick = [&a, &s, sid](int) {
                decToggle(a, s, sid);
                decUi().pickFocus = "dec.p." + sid;
            };
            out.push_back(std::move(r));
            if (decIsOn(id)) decOptionRows(a, s, id, out);
        }
    }
    // ★ THE GALLERY — the pictures the engine kept on the card; only where it keeps any (galEnabled asks once)
    if (galEnabled(a.eng_->port())) {
        DecPRow h{DecPRow::HEAD}; h.id = "g.gallery"; h.label = "SAVED ON THE CARD"; out.push_back(std::move(h));
        DecPRow r{DecPRow::ACTION}; r.id = "gallery"; r.label = "GALLERY"; r.opts = {"WEFAX CHARTS", "SSTV PICTURES"}; r.lit = {false, false};
        r.pick = [&a](int i) { galOpen(a, i); };
        out.push_back(std::move(r));
    }
}
inline float decPRowH(Ctx& c, const DecPRow& r, float w) {
    switch (r.kind) {
        case DecPRow::HEAD: return 22;
        case DecPRow::HINT: return (float)c.tc.wrap(c.f.small, r.label, (int)w - 40).size() * c.f.small.lineHeight() + 6;
        case DecPRow::TOGGLE: return 40;
        default: return 36;
    }
}
inline void decDrawPicker(App& a, Ctx& c, const RadioState& s, Rect panel) {
    DecUi& u = decUi();
    c.g.rrect(panel.x, panel.y, panel.w, panel.h, 12, rgb(0x0c0802));   // ★ opaque: the scale's lettering must not ghost through a menu
    c.g.rframe(panel.x, panel.y, panel.w, panel.h, 12, 1, th::btnBorder);
    const Rect head{panel.x, panel.y, panel.w, 30};
    c.tc.draw(c.g, c.f.bodyB, "DECODERS", head.x + 12, head.cy() - c.f.bodyB.lineHeight() / 2.f, th::btnText);
    c.tc.draw(c.g, c.f.small, "decoded on the receiver", head.x + 22 + c.tc.width(c.f.bodyB, "DECODERS"), head.cy() - c.f.small.lineHeight() / 2.f, th::dim);
    hints(c, head.r() - (decRunning() ? 168.f : 140.f), head.cy(), {{FACE_RIGHT, decRunning() ? "decoded text" : "close"}});
    c.g.rect(panel.x + 8, head.b(), panel.w - 16, 1, th::barBorder);
    const Rect view{panel.x + 10, head.b() + 6, panel.w - 26, panel.b() - head.b() - 12};
    std::vector<DecPRow> rows;
    decPickRows(a, s, rows);
    if (rows.empty()) { decNone(c, view, "No decoders on this receiver — the owner has switched them off."); return; }
    Scroller& sc = decPickScroll();
    float y = 0;
    std::vector<float> ys;
    for (auto& r : rows) { ys.push_back(y); y += decPRowH(c, r, view.w) + 4; }
    sc.contentH = y;
    if (!u.pickFocus.empty()) { a.nav_.setFocus("dec", u.pickFocus); u.pickFocus.clear(); }
    else if (a.nav_.focus("dec").empty()) for (auto& r : rows) if (r.kind == DecPRow::TOGGLE) { a.nav_.setFocus("dec", "dec.p." + r.id); break; }   // ★ land on a decoder, not the band tab
    c.g.clip(view.x - 6, view.y, view.w + 12, view.h);
    for (size_t i = 0; i < rows.size(); i++) {
        const float h = decPRowH(c, rows[i], view.w);
        const Rect rr{view.x, view.y + ys[i] - sc.y, view.w, h};
        // ★ every row registers (the highlight must be able to scroll onto it); only the visible ones draw ink
        if (rr.b() < view.y - 60 || rr.y > view.b() + 60) {
            if (rows[i].kind == DecPRow::TOGGLE) c.nav.item("dec.p." + rows[i].id, rr);
            else if (rows[i].kind == DecPRow::CHOICE || rows[i].kind == DecPRow::ACTION || rows[i].kind == DecPRow::TABS) c.nav.item("dec.p." + rows[i].id, rr, true);
            continue;
        }
        decDrawPRow(a, c, s, rows[i], rr);
    }
    c.g.unclip();
    sc.follow(c.nav.focusedRect(), view, 8);
    if (sc.contentH > view.h + 1) {
        const float frac = view.h / sc.contentH, top = sc.y / sc.contentH;
        c.g.rrect(view.r() + 6, view.y + top * view.h, 3, std::max(10.f, frac * view.h), 1.5f, th::dim);
    }
}

// ── the box ──────────────────────────────────────────────────────────────────────────────────────────────────────────

/** The sub-panes a list decoder has (their keys in the header). */
inline std::vector<std::string> decSubs(const std::string& view) {
    const DecUi& u = decUi();
    if (view == "cw") return u.cwSkim ? std::vector<std::string>{"SIGNALS", "CALLS"} : std::vector<std::string>{"TEXT", "SIGNALS", "CALLS"};
    if (view == "psk") return u.pskSkim ? std::vector<std::string>{"SIGNALS", "CALLS"} : std::vector<std::string>{"TEXT", "SIGNALS", "CALLS"};
    if (view == "jtty") return {"SIGNALS", "CALLS"};
    if (view == "aprs") return {"STATIONS", "PACKETS"};
    return {};
}

/** ★★ THE BOX — call once a frame in the deck pass, right after dabDrawBox and before drawDeck / drawVts. Draws nothing
 *  while no decoder runs and the picker is not open; never in DAB (its own page). */
/** ★ The TALL box was drawn this frame — it covers the spectrum and nearly all the waterfall (App::specCovered_). */
inline bool& decBoxTallShown() { static bool v = false; return v; }
inline void decDrawBox(App& a, Ctx& c, const RadioState& s) {
    DecUi& u = decUi();
    decBoxTallShown() = false;
    static bool wasGrabbed = false;
    bool grabbed = decGrabbed(a);
    if (wasGrabbed && !grabbed) {   // released (BACK, the menu): the picker folds; an empty box goes
        u.pane = DecUi::P_OUT;
        picUi().full = 0; if (galIsOpen()) galClose(a);
        if (!decRunning()) { u.boxOpen = false; u.view.clear(); u.refusal.clear(); }
    }
    wasGrabbed = grabbed;
    if (!u.boxOpen || dabActive() || s.session != "running") { if (grabbed) decRelease(a); wasGrabbed = false; return; }
    if (u.view.empty() && u.pane == DecUi::P_OUT && !decRunning() && u.refusal.empty()) { u.pane = DecUi::P_PICK; }
    // ★ BACK inside the box: the picker goes back to the decoded text first (when something runs); the release is the
    //   run loop's closeTop
    if (grabbed && !a.sheets_.empty() && a.sheets_.back() == App::S_DEC && u.pane == DecUi::P_PICK && decRunning() && c.nav.input().back) {
        if (c.nav.backPressed()) { u.pane = DecUi::P_OUT; a.nav_.forget("dec"); }
    }
    // ★ …and a CW signal being read goes back to the list first (its row keeps the highlight)
    if (grabbed && !a.sheets_.empty() && a.sheets_.back() == App::S_DEC && u.pane == DecUi::P_OUT && u.readSig && u.view == "cw" && c.nav.input().back) {
        if (c.nav.backPressed()) { const long long id = u.readSig; decReadSig(0); a.nav_.setFocus("dec", "dec.sig." + std::to_string(id)); }
    }
    // ★ a picture full screen, or the gallery: drawn from the sheet loop (decDrawFull) — the box draws nothing under it.
    //   Without that hook wired (app_impl.h `case S_DEC: decDrawFull(*this, c, s);`) it would be an invisible state:
    //   then it closes again and says so, rather than leave the pad on nothing.
    if (grabbed && decPicFullOpen()) {
        PicUi& pu = picUi();
        if (!pu.fullOpenedAt) pu.fullOpenedAt = nowSec();
        if (nowSec() - pu.fullOpenedAt > 0.5 && nowSec() - pu.fullDrawnAt > 0.5) {
            pu.full = 0; if (galIsOpen()) galClose(a);
            a.toast("Full screen is not wired in this build");
            std::fprintf(stderr, "vibe-hhui: decDrawFull is not called from the sheet loop (case S_DEC) — full screen closed\n");
        } else return;
    }
    picUi().fullOpenedAt = 0;
    const DecExternal* xv = decExt(u.view);
    const bool tall = u.pane == DecUi::P_PICK || (xv && xv->tall);
    const float top = 156, bot = decVtsShowing() ? 270.f : 308.f;
    Rect box = tall ? Rect{6, 44, 628, bot - 44} : Rect{6, top, 628, bot - top};
    decBoxTallShown() = tall;
    c.nav.layer("dec");
    if (u.pane == DecUi::P_PICK) decDrawPicker(a, c, s, box);
    else {
        c.g.rrect(box.x, box.y, box.w, box.h, 12, rgb(0x0c0802, 0.96f));
        c.g.rframe(box.x, box.y, box.w, box.h, 12, 1, th::btnBorder);
        const Rect head{box.x, box.y, box.w, 30};
        const std::string view = u.view.empty() ? "refused" : u.view;
        const DecDef* d = decDef(view);
        // keys, right to left: STOP · CLR · DECODERS · [the decoder's views] · [SPOTS ⇄ the decoder, both running]
        struct K { std::string id, label; int act; bool on; };
        std::vector<K> ks;
        std::vector<std::function<void()>> extPress;
        if (grabbed) {
            if (xv && xv->keys) {
                std::vector<std::string> labels;
                xv->keys(a, s, labels, extPress);
                for (size_t i = 0; i < labels.size() && i < extPress.size(); i++) ks.push_back({"dec.k.x" + std::to_string(i), labels[i], 40 + (int)i, false});
            }
            if (!u.active.empty() && u.spotsOn) ks.push_back({"dec.k.sw", view == "spots" ? decTitle(u.active) : std::string("SPOTS"), 10, false});
            const auto subs = decSubs(view);
            for (size_t i = 0; i < subs.size(); i++) ks.push_back({"dec.k.s" + std::to_string(i), subs[i], 20 + (int)i, u.sub == (int)i});
            if (view == "aprs" || view == "dsc" || view == "spots") ks.push_back({"dec.k.map", "MAP", 50, false});   // the APRS / DSC / SPOTS map (map_modes.h)
            if (d && d->kind == DK_PICTURE) { std::vector<PicKey> pk; decPicKeys(a, view, pk); for (auto& k : pk) ks.push_back({k.id, k.label, k.act, k.on}); }
            ks.push_back({"dec.k.pick", "DECODERS", 1, false});
            // ★★ EXPLAIN ↔ RAW (the app's key, Stuart 2026-10-30): the legend names what a press DOES; CW only
            if (view == "cw") ks.push_back({"dec.k.plain", a.set_.cwPlain ? "RAW" : "EXPLAIN", 4, false});
            if (view == "cw" && u.readSig && decSubs(view)[(size_t)std::max(0, std::min(u.sub, (int)decSubs(view).size() - 1))] == "SIGNALS")
                ks.push_back({"dec.k.tune", "TUNE", 5, false});
            // ★ CLR only where there is something it clears (an external decoder: only with its own clear action)
            if (view != "refused" && (!xv || xv->clear)) ks.push_back({"dec.k.clr", "CLR", 2, false});
            ks.push_back({"dec.k.stop", decRunning() ? "STOP" : "CLOSE", 3, false});
        }
        float kx = head.r() - 6;
        std::vector<std::pair<Rect, size_t>> placed;
        for (size_t i = ks.size(); i-- > 0;) { const float w = decKeyW(c, ks[i].label); kx -= w; placed.push_back({{kx, head.y + 4, w, head.h - 6}, i}); kx -= 5; }
        // the line: the LED, the title, the status (or RTTY AUTO's tuning guide — while up, it IS the status)
        const Rgba led = u.live ? rgb(0x35e08a) : rgb(0x696252);
        c.g.circle(head.x + 14, head.cy(), 5, 0, led);
        const std::string title = decTitle(view);
        const float tw = (float)c.tc.draw(c.g, c.f.bodyB, title, head.x + 26, head.cy() - c.f.bodyB.lineHeight() / 2.f, th::btnText);
        std::string st = !u.refusal.empty() && view != "spots" && (u.active.empty() || view == "refused") ? "not started" : !u.tuneHint.empty() && view == "rtty" ? u.tuneHint : view == "spots" ? (u.spots.empty() ? "listening…" : std::to_string(u.spots.size()) + " heard") : u.status;
        if (view == "time" && !u.timeSynced.empty() && nowSec() - u.timeSyncedAt < 90) st += " · clock ready: MENU › HARDWARE";
        const float sx = head.x + 26 + tw + 10;
        c.tc.draw(c.g, c.f.small, decFit(c, c.f.small, st, (int)((grabbed ? kx : head.r() - 8) - sx - 4)), sx, head.cy() - c.f.small.lineHeight() / 2.f,
                  !u.tuneHint.empty() && view == "rtty" ? th::amber : th::white68);
        c.g.rect(box.x + 8, head.b(), box.w - 16, 1, th::barBorder);
        const Rect body{box.x + 4, head.b() + 3, box.w - 10, box.b() - head.b() - 6};
        // the body
        if (view == "refused" || (!u.refusal.empty() && view != "spots" && u.active.empty())) decNone(c, body, u.refusal);
        else if (view == "spots") decPaneSpots(a, c, s, body);
        else if (xv) { if (xv->draw) xv->draw(a, c, s, body); else decNone(c, body, u.status); }
        else if (d && d->kind == DK_PICTURE) decPicPane(a, c, s, body, grabbed);
        else if (d && d->kind == DK_DSC) decPaneDsc(a, c, s, body);
        else if (d && d->kind == DK_APRS) decPaneAprs(a, c, s, body);
        else if (d && (d->kind == DK_KB || d->kind == DK_JTTY)) {
            const auto subs = decSubs(view);
            const std::string sp = u.sub >= 0 && u.sub < (int)subs.size() ? subs[(size_t)u.sub] : "SIGNALS";
            if (sp == "TEXT") decDrawLog(a, c, body, "listening — the signal in the middle of your passband is decoded here");
            else if (sp == "CALLS") decPaneCalls(a, c, s, body);
            else if (view == "cw" && u.readSig) decPaneRead(a, c, s, body);
            else decPaneSignals(a, c, s, body);
        } else {
            std::string idle = u.status;
            if (view == "time") for (auto& t : kTimeStations) if (u.timeStation == t.id) idle = std::string(t.hint) + " The fields fill in as they arrive.";
            if (view == "navtex") idle = "standing by — NAVTEX messages appear here as they are broadcast (each station's slot comes round every 4 hours; 518 kHz is the international channel).";
            decDrawLog(a, c, body, idle);
        }
        // the header's keys — registered AFTER the body, so a fresh grab lands on the text / the first row, not on a key
        std::reverse(placed.begin(), placed.end());
        int act = -1;
        for (auto& p : placed) if (decKey(c, ks[p.second].id, p.first, ks[p.second].label, ks[p.second].on)) act = ks[p.second].act;
        if (act == 1) { u.pane = DecUi::P_PICK; u.pickFocus = "dec.p." + (view == "refused" ? std::string("rtty") : view); decPickScroll() = {}; a.nav_.forget("dec"); }
        else if (act == 2) {
            if (xv) { if (xv->clear) xv->clear(); }
            else if (d && d->kind == DK_PICTURE) picClear();
            else if (view == "spots") { u.spots.clear(); u.spotsSeq++; }
            else { u.log.clear(); u.calls.clear(); u.dsc.clear(); u.aprsLog.clear(); u.aprs.clear(); decClearLists(); decListScroll() = {}; decSigAnchor() = {}; }
        } else if (act == 3) {
            if (decRunning()) decStop(a, view == "spots" ? "spots" : view);
            if (!decRunning()) { u.boxOpen = false; u.view.clear(); u.refusal.clear(); decRelease(a); }
            else a.nav_.forget("dec");
        } else if (act == 4) { a.set_.cwPlain = a.set_.cwPlain ? 0 : 1; a.dirty();
            a.toast(a.set_.cwPlain ? "CW in plain English — RAW shows it as sent" : "CW as sent");
        } else if (act == 5) {
            for (auto& g : u.sigs) if ((long long)g.id == u.readSig && g.hz > 0) decTune(a, s, g.hz, "cwu");
        } else if (act == 50) { mapOpenDecoder(a, view);
        } else if (act == 10) { u.view = view == "spots" ? u.active : "spots"; u.sub = 0; decListScroll() = {}; decSigAnchor() = {}; a.nav_.forget("dec"); }
        else if (act >= 60) decPicKeyPress(a, act, view);
        else if (act >= 40) { if ((size_t)(act - 40) < extPress.size()) extPress[(size_t)(act - 40)](); }
        else if (act >= 20) { u.sub = act - 20; decReadSig(0); decListScroll() = {}; decSigAnchor() = {}; a.nav_.forget("dec"); }
    }
    c.nav.layer("deck");
    // the box as ONE stop on the island's layer (the web's [data-navbox]): ENTER grabs
    if (!grabbed) {
        const Nav::Hit h = c.nav.item("dec.box", box);
        if (h.focused) {
            c.g.rframe(box.x + 1, box.y + 1, box.w - 2, box.h - 2, 12, 3, th::focus);
            const std::string t = "read · options";
            const float hw = 24 + (float)c.tc.width(c.f.small, t) + 20;
            const Rect hb{box.r() - hw - 10, box.b() - 32, hw, 26};
            c.g.rrect(hb.x, hb.y, hb.w, hb.h, 13, rgb(0, 0.85f));
            c.g.rframe(hb.x, hb.y, hb.w, hb.h, 13, 1, th::focus);
            hints(c, hb.x + 8, hb.cy(), {{FACE_BOTTOM, t}}, th::focus);
        }
        if (h.pressed) { a.open(App::S_DEC); a.nav_.forget("dec"); }
    }
    if (u.grabNext && !grabbed) { u.grabNext = false; a.open(App::S_DEC); a.nav_.forget("dec"); }
}

// ── the mode picker's entry ──────────────────────────────────────────────────────────────────────────────────────────

/** ★ The DECODERS… entry for drawModeSheet's list, after the whole-radio modes (the web's order): DECODERS… when there
 *  is anything behind it (anyDecoderLeft), the SOLE decoder itself when only one is left (soleDecoder), nothing when
 *  the owner has switched them all off (a door to an empty room is a control whose every use is a no-op). */
inline void decModeEntries(const RadioState& s, std::vector<ModeDef>& ms) {
    if (!decAnyOffered(s)) return;
    if (const DecDef* one = decSole(s)) { ms.push_back({std::string("dec:") + one->id, one->label}); return; }
    ms.push_back({"dec", "DECODERS…"});
}
/** The entry drawn lit: DECODERS… while any decoder runs, the sole decoder while it runs. */
inline bool decModeLit(const std::string& id) {
    if (id == "dec") return decRunning();
    if (id.compare(0, 4, "dec:") == 0) return decIsOn(id.substr(4));
    return false;
}
/** The mode picker's press (call it first, beside dabModePick): opens the picker (out of DAB first), or toggles the sole
 *  decoder. Returns true when it handled the press (the caller then closes the sheet). */
inline bool decModePick(App& a, const RadioState& s, const std::string& id) {
    DecUi& u = decUi();
    if (id == "dec") {
        if (dabActive()) dabLeave(a, s);
        u.boxOpen = true; u.pane = DecUi::P_PICK; u.grabNext = true; u.pickBand = -1;
        u.pickFocus = !u.active.empty() ? "dec.p." + u.active : u.spotsOn ? std::string("dec.p.spots") : "";
        decPickScroll() = {};
        decFetchCaps(a.eng_->port());
        return true;
    }
    if (id.compare(0, 4, "dec:") == 0) {
        if (dabActive()) dabLeave(a, s);
        decToggle(a, s, id.substr(4));
        if (!decRunning()) u.boxOpen = false;
        return true;
    }
    return false;
}

}  // namespace hh

#include "dec_pic_box.h"   // ★ the picture decoders drawn + the gallery (they use this file's helpers)
