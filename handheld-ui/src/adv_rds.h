// adv_rds.h — ADVANCED RDS on the R36S: the app's AdvRdsPanel (src/components/AdvRdsPanel.tsx) as a box over the
// waterfall, pad-driven. The readings are adv_rds_core.h; this file asks the engine for them, and draws them.
//
// ★★★ THE PANEL BEING ON SCREEN IS THE SWITCH (the app's rule, SDRScreen setAdvRds): `{"type":"rdsx","on":1}` goes to the
//     engine only while the box is open, the radio is in WFM, and nothing covers it — the analyser costs vibe-dsp real
//     CPU (the engine's own note: ~16 % of a Cortex-A7 for the eye + deviation block) and ~6 messages a second. A sheet
//     over it for more than 2 s, the screen going off, leaving WFM, entering DAB or closing the box sends `on:0`.
//     ★ OFF BY DEFAULT and never hidden (memory: benchmark sets the default, never hides): offered in WFM wherever the
//     owner has not blocked "rds" (hwinfo blocked[] — the engine refuses it then too, so the entry would be a no-op).
// ★★ THE LIGHTER EYE STREAM (the web's rdsxLight, spectrum.ts): `eyeEvery` asks for the three eye grids (~8.4 of the
//    ~9 kB a frame) in every Nth message only — 2 while PLOTS is showing (the web's figure), 12 otherwise, where nothing
//    draws them. The last grids are kept (core.eye) — ABSENT = UNCHANGED, as the app's parse.
// ★★ THREE VIEWS, the app's SMALL / BIG on a 640×480 screen:
//      SUMMARY — the box over the waterfall (y 156 →), the seven readings you watch while tuning, two columns;
//      ALL     — the tall panel (y 44 →), every reading, scrolled with UP / DOWN;
//      PLOTS   — the tall panel: constellation + its verdict, the symbol trace, the MPX spectrum, the deviation meter
//                with L / R and MPX power, and the three-band SCOPE VIEW.
//    PI, the station name and RadioText are NOT repeated: the VTS bar under the box shows them (vts.h). The PI's decimal
//    and meaning are in PI detail.
// ★★ ONE STOP FOR THE D-PAD (the dec / DAB boxes' rule): ENTER grabs (App::S_ARDS, layer "ards"), BACK releases. Grabbed:
//    RIGHT / UP reach the header's keys (SUMMARY · ALL · PLOTS · CLOSE), ENTER on the body steps to the next view, UP /
//    DOWN scroll ALL. ★ The box keeps its readings live while released — that is the point of SUMMARY.
// ★ Entry point: the mode picker's ADV RDS (in WFM only) — where the APP opens it (ModeSelect onAdvRds). The web has it
//   in its decoders panel (VHF · BROADCAST); adv_rds_dec.h registers it with the decoder box for when that is wired.
// ★ The CPU cost shown where it is paid: the header carries the device's CPU % (the status row's figure) beside the
//   group rate, so opening PLOTS on a weak box shows what it costs at once.
// ★ Cheap per frame: the rows are rebuilt only when an rdsx arrives (≤ 6 Hz); wrapping is cached per row; the plots'
//   coordinates are recomputed at most every 320 ms (the app's useThrottledPoints) and the eye grids are ONE texture,
//   re-uploaded only when a new grid arrives — the frame draws ~2 × N dots, one polyline, three textured quads.
#pragma once
#include "adv_rds_core.h"
#include "vts.h"

#include <GLES3/gl3.h>

#include <functional>

namespace hh {

struct ArdsUi {
    enum View { V_SUMMARY, V_ALL, V_PLOTS };
    bool open = false;
    int view = V_SUMMARY;
    bool grabNext = false;
    ardr::Core core;
    uint64_t seenSeq = 0;
    double lastAt = 0;                  // the last rdsx's arrival (the LED)
    // the subscription
    bool subscribed = false; int sentEvery = 0; double coveredSince = 0;
    double logAt = 0; long logN = 0; size_t logBytes = 0;
    // ALL's scroll (pixels) and the rows' grow-only line counts (the app's minHeight memory: a row never shrinks)
    float scrollY = 0, contentH = 0;
    std::map<std::string, int> maxLines;
    // the plots: normalised once per 320 ms
    double plotAt = 0;
    std::vector<float> cPts;            // the constellation, de-rotated + scaled: (x, y) per point, in box units (0..1)
    std::vector<float> tPts;            // the symbol trace: y per symbol, in box units
    std::vector<float> mPts;            // the MPX curve: y per bin, 0 (top) .. 1 (floor)
    GLuint eyeTex = 0; uint64_t eyeUp = 0; int texW = 0, texH = 0; bool eyeOk[3] = {false, false, false};
    // a fit cache for the header's status line
    std::map<std::string, std::string> fit;
    // ★ THE HOST: the standalone box (layer "ards", App::S_ARDS) or the decoder box (layer "dec", adv_rds_dec.h)
    std::string layer = "ards";
    std::function<void()> onEnded;      // the host's "it stopped by itself" (the decoder box's decExternalEnded)
};
inline ArdsUi& ardsUi() { static ArdsUi u; return u; }

namespace ardr {
inline Rgba toneCol(Tone t) {
    switch (t) {
        case T_GOOD: return rgb(0x7dff9a);
        case T_WARN: return rgb(0xffd479);
        case T_BAD: return rgb(0xff8a7d);
        case T_MUTED: return rgb(0xffffff, 0.55f);
        default: return th::white;
    }
}
inline const Rgba kLabel = rgb(0xffb833, 0.85f);
inline const Rgba kPlotBg = rgb(0x000000, 0.55f);
inline const Rgba kAxis = rgb(0xffa000, 0.22f);
inline const Rgba kAxisStrong = rgb(0xffa000, 0.45f);
inline const Rgba kDot = rgb(0xffe566);
}  // namespace ardr

/** ★ App::S_ARDS when the standalone box is wired (app.h's Sheet enum), else S_NONE — so this file builds either way
 *  (hosted by the decoder box alone, adv_rds_dec.h, it needs no sheet of its own). */
template <class A> constexpr auto ardsSheetOf(int) -> decltype(A::S_ARDS) { return A::S_ARDS; }
template <class A> constexpr typename A::Sheet ardsSheetOf(long) { return A::S_NONE; }
inline constexpr App::Sheet kArdsSheet = ardsSheetOf<App>(0);
inline bool ardsGrabbed(const App& a) { if (kArdsSheet == App::S_NONE) return false; for (auto sh : a.sheets_) if (sh == kArdsSheet) return true; return false; }
inline void ardsRelease(App& a) {
    for (size_t i = 0; i < a.sheets_.size();) { if (a.sheets_[i] == kArdsSheet && kArdsSheet != App::S_NONE) a.sheets_.erase(a.sheets_.begin() + (long)i); else i++; }
    a.nav_.forget("ards");
}
/** Offered: WFM (not DAB), a running radio, and the owner has not blocked "rds" (the engine's vsModeBlocked gate). */
inline bool ardsOffered(const RadioState& s) {
    if (s.session != "running" || s.mode != "wfm" || dabActive()) return false;
    for (auto& b : s.blockedModes) if (b == "rds") return false;
    return true;
}
inline void ardsOpen(App& a) {
    ArdsUi& u = ardsUi();
    if (u.open) return;
    u.open = true; u.view = ArdsUi::V_SUMMARY; u.grabNext = true;
    u.core.reset();
    if (a.eng_) { auto it = a.eng_->state().seq.find("rdsx"); u.seenSeq = it == a.eng_->state().seq.end() ? 0 : it->second; }   // ★ a frame from before is not this panel's
    u.lastAt = 0; u.scrollY = 0; u.maxLines.clear(); u.plotAt = 0; u.cPts.clear(); u.tPts.clear(); u.mPts.clear(); u.eyeUp = 0;
}
inline void ardsClose(App& a) {
    ArdsUi& u = ardsUi();
    u.open = false;
    if (ardsGrabbed(a)) ardsRelease(a);
    // ★ off AT ONCE: a host that stops ticking a closed decoder (the decoder box) must not leave the analyser running
    if (u.subscribed && a.eng_) { a.eng_->send("{\"type\":\"rdsx\",\"on\":0}"); std::fprintf(stderr, "vibe-hhui: adv rds off\n"); }
    u.subscribed = false; u.sentEvery = 0;
}

/** Once a loop (after the engine's pump): the subscription, and a new frame into the core. */
inline void ardsTick(App& a, const RadioState& s) {
    ArdsUi& u = ardsUi();
    const double now = nowSec();
    if (u.open && !ardsOffered(s) && s.session == "running" && !s.mode.empty()) {   // ★ left WFM / into DAB: closed, honestly
        ardsClose(a);
        if (u.onEnded) u.onEnded();
    }
    // covered: any sheet but the host's own grab (the mode picker, the menu …) — after 2 s, so a quick look costs nothing
    bool covered = false;
    for (auto sh : a.sheets_) { const std::string l = App::layerName(sh); if (l != "ards" && l != "dec") covered = true; }
    if (!covered) u.coveredSince = 0; else if (u.coveredSince == 0) u.coveredSince = now;
    const bool want = u.open && ardsOffered(s) && s.connected && !a.screenOff_ && !(covered && now - u.coveredSince > 2.0);
    const int every = u.view == ArdsUi::V_PLOTS ? 2 : 12;
    if (!s.connected) u.subscribed = false;            // ★ the engine drops a socket's subscription with the socket
    if (want && (!u.subscribed || u.sentEvery != every)) {
        a.eng_->send("{\"type\":\"rdsx\",\"on\":1,\"eyeEvery\":" + std::to_string(every) + "}");
        if (!u.subscribed) std::fprintf(stderr, "vibe-hhui: adv rds on (eyeEvery %d)\n", every);
        u.subscribed = true; u.sentEvery = every;
    } else if (!want && u.subscribed) {
        a.eng_->send("{\"type\":\"rdsx\",\"on\":0}");
        std::fprintf(stderr, "vibe-hhui: adv rds off\n");
        u.subscribed = false; u.sentEvery = 0;
    }
    auto it = s.seq.find("rdsx");
    const uint64_t sq = it == s.seq.end() ? 0 : it->second;
    if (sq != u.seenSeq) {
        u.seenSeq = sq;
        if (!u.open) return;
        const Json& x = s.msg("rdsx");
        const Json& rds = s.msg("rds");
        const Json& hw = s.msg("hwinfo");
        const int wsp = hw.has("wsp") && (hw["wsp"].kind == Json::Bool || hw["wsp"].kind == Json::Num) ? (hw["wsp"].boolean() ? 1 : 0) : -1;
        const int pi = (int)ardr::num(rds, "pi", 0), ecc = (int)ardr::num(rds, "ecc", 0);
        const std::string iso = pi > 0 ? vts::eccPiToIso(ecc, ardr::hex(pi, 4)) : "";
        u.core.update(x, rds, wsp, iso, now);
        u.lastAt = now;
        u.logN++; u.logBytes += x["eyeP"].s.size() + x["eyeS"].s.size() + x["eyeR"].s.size();
    }
    if (u.subscribed && now - u.logAt >= 10) {
        if (u.logAt > 0) std::fprintf(stderr, "vibe-hhui: adv rds %.1f rdsx/s · eye grids %.1f kB/s · view %d · device CPU %d %%\n",
                                      u.logN / (now - u.logAt), u.logBytes / 1024.0 / (now - u.logAt), u.view, a.cpuPct_);
        u.logAt = now; u.logN = 0; u.logBytes = 0;
    }
}

// ── the mode picker's entry (beside dabModePick) ─────────────────────────────────────────────────────────────────────
/** ★ Only while the standalone box is the host: with the decoder box hosting it (adv_rds_dec.h), DECODERS… is the door. */
inline void ardsModeEntries(const RadioState& s, std::vector<ModeDef>& ms) {
    if (kArdsSheet != App::S_NONE && ardsUi().layer == "ards" && ardsOffered(s)) ms.push_back({"ards", "ADV RDS"});
}
inline bool ardsModeLit(const std::string& id) { return id == "ards" && ardsUi().open; }
/** Toggles the box (the app's setAdvRdsOpen(o => !o)). True when it handled the press (the caller closes the sheet). */
inline bool ardsModePick(App& a, const RadioState&, const std::string& id) {
    if (id != "ards") return false;
    if (ardsUi().open) ardsClose(a); else ardsOpen(a);
    return true;
}

// ── drawing ──────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace ardr {
inline std::string fitText(Ctx& c, const Font& f, const std::string& s, int maxW) {
    auto& m = ardsUi().fit;
    if (maxW <= 0) return "";
    const std::string key = std::to_string(maxW) + "|" + s;
    auto it = m.find(key);
    if (it != m.end()) return it->second;
    if (m.size() > 200) m.clear();
    std::string out = s;
    if (c.tc.width(f, s) > maxW) {
        size_t n = s.size();
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
            out = s.substr(0, n) + "\xE2\x80\xA6";
            if (c.tc.width(f, out) <= maxW) break;
        }
    }
    return m[key] = out;
}
/** One label / value row: the value wraps under itself (never cut — the app's rule), and the row keeps the most lines
 *  it has ever needed (grow-only: a value that gets shorter never pulls the rows below up under the reader's eye).
 *  Returns the row's height. `draw` false = measure only. */
inline float drawRow(Ctx& c, const Row& r, float x, float y, float labelW, float w, const std::string& key, bool draw, float clipTop, float clipBot) {
    ArdsUi& u = ardsUi();
    const Font& f = c.f.small;
    const float lh = (float)f.lineHeight();
    const auto& lines = c.tc.wrap(f, r.text, (int)(w - labelW));
    int& ml = u.maxLines[key];
    ml = std::max(ml, (int)std::max<size_t>(1, lines.size()));
    const float h = ml * lh;
    if (draw && y + h >= clipTop && y <= clipBot) {
        c.tc.draw(c.g, f, r.label, x, y, kLabel);
        const Rgba col = toneCol(r.tone);
        for (size_t i = 0; i < lines.size(); i++) c.tc.draw(c.g, f, lines[i], x + labelW, y + i * lh, col);
    }
    return h;
}
}  // namespace ardr

/** SUMMARY: two columns — the station's flags on the left, the three readings you tune by on the right. */
inline void ardsDrawSummary(Ctx& c, Rect body) {
    ArdsUi& u = ardsUi();
    const float colW = (body.w - 12) / 2;
    const float lw = (float)c.tc.width(c.f.small, "Pilot dev") + 12;
    static const char* left[] = {"Type", "Flags", "Errors", "Rate"};
    static const char* right[] = {"Pilot dev", "RDS dev", "MPX S/N"};
    c.g.clip(body.x, body.y, body.w, body.h);
    float y = body.y;
    for (const char* l : left) if (const ardr::Row* r = u.core.row(l)) y += ardr::drawRow(c, *r, body.x + 6, y, lw, colW, std::string("s.") + l, true, body.y, body.b());
    y = body.y;
    const float rx = body.x + 6 + colW + 6;
    c.g.rect(rx - 7, body.y + 2, 1, body.h - 4, th::barBorder);
    for (const char* l : right) if (const ardr::Row* r = u.core.row(l)) y += ardr::drawRow(c, *r, rx, y, lw, colW - 6, std::string("s.") + l, true, body.y, body.b());
    c.g.unclip();
}

/** ALL: every reading, scrolled. The scroll stops above and below the body are the decoder log's trick (dec_box.h). */
inline void ardsDrawAll(App& a, Ctx& c, Rect body, bool grabbed) {
    ArdsUi& u = ardsUi();
    const float lw = (float)c.tc.width(c.f.small, "Other networks") + 14;
    const float lh = (float)c.f.small.lineHeight();
    // measure, then clamp the scroll
    float h = 0;
    for (auto& r : u.core.rows) h += ardr::drawRow(c, r, 0, 0, lw, body.w - 22, "a." + r.label, false, 0, 0);
    u.contentH = h;
    const float maxS = std::max(0.f, h - body.h);
    if (grabbed) {
        static std::string prev;
        const std::string f = a.nav_.focus(u.layer);
        const bool fromBody = prev == "ards.body";
        if (f == "ards.up") { if (fromBody) u.scrollY -= 3 * lh; a.nav_.setFocus(u.layer, "ards.body"); }
        else if (f == "ards.dn") { if (fromBody) u.scrollY += 3 * lh; a.nav_.setFocus(u.layer, "ards.body"); }
        prev = a.nav_.focus(u.layer);
    }
    u.scrollY = std::max(0.f, std::min(u.scrollY, maxS));
    c.g.clip(body.x, body.y, body.w, body.h);
    float y = body.y - u.scrollY;
    for (auto& r : u.core.rows) y += ardr::drawRow(c, r, body.x + 6, y, lw, body.w - 22, "a." + r.label, true, body.y, body.b());
    c.g.unclip();
    if (h > body.h + 1) {
        const float frac = body.h / h, top = u.scrollY / h;
        c.g.rrect(body.r() - 6, body.y + top * body.h, 3, std::max(10.f, frac * body.h), 1.5f, th::dim);
    }
    if (grabbed) {
        if (u.scrollY > 0.5f) c.nav.item("ards.up", {body.x + 10, body.y, body.w - 20, 2});
        if (u.scrollY < maxS - 0.5f) c.nav.item("ards.dn", {body.x + 10, body.b() - 2, body.w - 20, 2});
    }
}

namespace ardr {
/** The plots' coordinates from the latest arrays — at most every 320 ms (the app's useThrottledPoints: 3 Hz is the same
 *  picture by eye, and halves the work). */
inline void plotPrep(double now) {
    ArdsUi& u = ardsUi();
    if (now - u.plotAt < 0.32) return;
    u.plotAt = now;
    const Core& k = u.core;
    const std::vector<float>& xy = k.xy;
    const double rot = constellationAngle(xy), cr = std::cos(rot), sr = std::sin(rot);
    const double s1 = constellationScale(xy, 1.0);   // ★ linear in the box, so one scale serves every size
    u.cPts.clear(); u.tPts.clear();
    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        const double x = xy[i] * cr - xy[i + 1] * sr, y = xy[i] * sr + xy[i + 1] * cr;
        u.cPts.push_back((float)(0.5 + x * s1)); u.cPts.push_back((float)(0.5 - y * s1));
        u.tPts.push_back((float)(0.5 - x * s1 * 0.9));   // the symbol trace: the wanted axis against time
    }
    // ★ the MPX curve AUTO-RANGES on what is present (injection levels vary)
    u.mPts.clear();
    if (!k.mpx.empty()) {
        float lo = 999, hi = -999;
        for (float v : k.mpx) { lo = std::min(lo, v); hi = std::max(hi, v); }
        if (hi - lo < 12) hi = lo + 12;
        for (float v : k.mpx) u.mPts.push_back(1.f - (v - lo) / (hi - lo));
    }
}
/** The three eye grids as ONE texture (stacked), white with the cell's alpha — the band's VERDICT colour is the draw's
 *  tint, so a colour change never re-uploads. Only when a new grid arrived. */
inline void eyeUpload(Gfx& g) {
    ArdsUi& u = ardsUi();
    const Core& k = u.core;
    if (u.eyeUp == k.eyeGen || k.eyeW <= 0 || k.eyeH <= 0 || k.eyeW * k.eyeH > 1 << 16) return;
    u.eyeUp = k.eyeGen;
    const int w = k.eyeW, h = k.eyeH, n = w * h;
    std::vector<uint8_t> px((size_t)n * 3 * 4, 0), cells;
    for (int b = 0; b < 3; b++) {
        u.eyeOk[b] = decodeEye(k.eye[b], n, cells);
        if (!u.eyeOk[b]) continue;
        for (int i = 0; i < n; i++) {
            const int v = cells[(size_t)i];
            if (v <= 0) continue;
            const double al = 0.10 + 0.90 * std::pow(v / 63.0, 0.55);
            uint8_t* p = &px[((size_t)b * n + i) * 4];
            p[0] = p[1] = p[2] = 255; p[3] = (uint8_t)std::min(255.0, std::round(255 * al));
        }
    }
    g.flush();
    if (!u.eyeTex) glGenTextures(1, &u.eyeTex);
    glBindTexture(GL_TEXTURE_2D, u.eyeTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h * 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    u.texW = w; u.texH = h;
}
inline void plotBox(Ctx& c, Rect r) { c.g.rrect(r.x, r.y, r.w, r.h, 3, kPlotBg); }
inline void plotLabel(Ctx& c, const std::string& t, float x, float y) { c.tc.draw(c.g, c.f.tiny, t, x, y, rgb(0xffffff, 0.6f)); }
/** A verdict line (or two), the live text over a fixed height so the plots below never move. */
inline void verdictText(Ctx& c, const Font& f, const std::string& t, Tone tone, float x, float y, float w, int maxLines, int align = 0) {
    const auto& ln = c.tc.wrap(f, t, (int)w);
    const float lh = (float)f.lineHeight();
    for (size_t i = 0; i < ln.size() && (int)i < maxLines; i++)
        c.tc.draw(c.g, f, ln[i], align == 1 ? x + w / 2 : x, y + i * lh, toneCol(tone), align);
}
inline void devBar(Ctx& c, Rect r, double pct, Tone tone, double avgPct, double hold, bool marks) {
    c.g.rect(r.x, r.y, r.w, r.h, rgb(0xffa000, 0.12f));
    c.g.frame(r.x, r.y, r.w, r.h, 1, kAxis);
    c.g.rect(r.x, r.y, r.w * (float)(pct / 100), r.h, toneCol(tone));
    if (marks && avgPct > 0.5) c.g.rect(r.x + r.w * (float)(avgPct / 100) - 1, r.y, 2, r.h, rgb(0xffffff, 0.45f));
    if (marks) c.g.rect(r.x + r.w * (float)(hold / 100) - 1, r.y, 2, r.h, th::white);
    c.g.rect(r.x + r.w * 0.75f, r.y, 1, r.h, rgb(0xffffff, 0.55f));   // the 75 kHz limit
}
}  // namespace ardr

/** PLOTS: three columns — the constellation over the symbol trace · MPX + the deviation meter · the SCOPE VIEW.
 *  ★ Laid out against the box's own height (≈ 195 px with the VTS bar up, 233 without): every block has a FIXED
 *    height (verdicts reserve their lines), so a reading that changes length never moves a plot. */
inline void ardsDrawPlots(Ctx& c, Rect body) {
    using namespace ardr;
    ArdsUi& u = ardsUi();
    const Core& k = u.core;
    plotPrep(c.t);
    eyeUpload(c.g);
    const Font& tf = c.f.tiny;
    const float tl = (float)tf.lineHeight(), sl = (float)c.f.small.lineHeight();
    const float aw = 166, bw = 258, gap = 11;
    const float ax = body.x + 2, bx = ax + aw + gap, cx = bx + bw + gap, cw = body.r() - cx - 2;
    // ── column A: CONSTELLATION + its verdict, SYMBOL TRACE ──
    {
        const float traceMin = 30;
        const float cs = std::max(70.f, std::min(aw - 20, body.h - 2 - tl - 2 - 2 * tl - tl - traceMin));
        float y = body.y + 2;
        plotLabel(c, "CONSTELLATION", ax, y); y += tl;
        const Rect cb{ax + (aw - cs) / 2, y, cs, cs};
        plotBox(c, cb);
        c.g.rect(cb.cx() - 0.5f, cb.y, 1, cb.h, kAxis);
        c.g.rect(cb.x, cb.cy() - 0.5f, cb.w, 1, kAxis);
        // ★ out-of-box points skipped, not scissored: a clip is a batch break (two GL draws) every frame
        for (size_t i = 0; i + 1 < u.cPts.size(); i += 2) {
            const float px = u.cPts[i], py = u.cPts[i + 1];
            if (px < 0.01f || px > 0.99f || py < 0.01f || py > 0.99f) continue;
            c.g.rect(cb.x + px * cs - 1.2f, cb.y + py * cs - 1.2f, 2.4f, 2.4f, kDot);
        }
        y += cs + 2;
        verdictText(c, tf, k.constV.first, k.constV.second, ax, y, aw, 2, 1); y += 2 * tl;   // ★ two lines reserved (the rotating verdict)
        plotLabel(c, "SYMBOL TRACE", ax, y); y += tl;
        const Rect tb{ax, y, aw, std::max(10.f, body.b() - y - 1)};
        plotBox(c, tb);
        c.g.rect(tb.x, tb.cy() - 0.5f, tb.w, 1, kAxisStrong);
        const size_t nt = u.tPts.size();
        for (size_t i = 0; nt > 1 && i < nt; i++) {
            const float py = u.tPts[i];
            if (py < 0.02f || py > 0.98f) continue;
            c.g.rect(tb.x + 1 + (float)i / (nt - 1) * (tb.w - 2) - 0.9f, tb.y + py * tb.h - 0.9f, 1.8f, 1.8f, kDot);
        }
    }
    // ── column B: MPX, the deviation meter (bar + figures + verdict), L / R, MPX power ──
    {
        float y = body.y + 2;
        plotLabel(c, "MPX", bx, y); y += tl;
        const float mh = std::max(40.f, std::min(80.f, body.h - 2 - tl - 4 - 12 - sl - 2 * tl - 26 - sl - 1));   // (the blocks below, summed)
        const Rect mb{bx, y, bw, mh};
        plotBox(c, mb);
        const float top = mb.y + tl - 1;                       // the landmarks' strip
        c.g.rect(mb.x, top, mb.w * 0.15f, mb.b() - top, rgb(0xffa000, 0.10f));   // L+R: DC..15 kHz is a band, not a line
        c.tc.draw(c.g, tf, "L+R", mb.x + 2, mb.y, rgb(0xffffff, 0.6f));
        static const std::pair<double, const char*> marks[] = {{0.19, "PILOT"}, {0.38, "L\xE2\x88\x92R"}, {0.57, "RDS"}};
        for (auto& m : marks) {
            const float mx = mb.x + mb.w * (float)m.first;
            c.g.rect(mx - 0.5f, top, 1, mb.b() - top, kAxisStrong);
            c.tc.draw(c.g, tf, m.second, mx + 2, mb.y, rgb(0xffffff, 0.6f));
        }
        const size_t n = u.mPts.size();
        const float ph = mb.b() - top - 2;
        for (size_t i = 1; i < n; i++)
            c.g.line(mb.x + (float)(i - 1) / (n - 1) * mb.w, top + 1 + u.mPts[i - 1] * ph, mb.x + (float)i / (n - 1) * mb.w, top + 1 + u.mPts[i] * ph, 1.3f, th::amber);
        y = mb.b() + 4;
        // ★ the FILL = the fast true peak, the white TICK = the 6 s hold (what the digits quote), the line = 75 kHz
        devBar(c, {bx, y, bw, 8}, k.dev.pct, k.dev.tone, k.dev.avgPct, k.dev.hold, true);
        y += 12;
        // ★★ FIXED CELLS: average and peak in their own columns (they move independently, several times a second)
        const Rgba col = toneCol(k.dev.tone);
        const float half = bw / 2;
        c.tc.draw(c.g, c.f.small, "Dev avg", bx, y, kLabel);
        c.tc.drawTab(c.g, c.f.small, k.dev.avg, bx + half - 10, y, col, 2);
        c.tc.draw(c.g, c.f.small, "peak", bx + half + 4, y, kLabel);
        c.tc.drawTab(c.g, c.f.small, k.dev.peak, bx + bw, y, col, 2);
        y += sl;
        verdictText(c, tf, k.dev.verdict, k.dev.tone, bx, y, bw, 2, 1);   // ★ two lines reserved: nothing below jumps
        y += 2 * tl;
        if (k.lr.show) {
            const LrSide* sd[2] = {&k.lr.l, &k.lr.r};
            for (int i = 0; i < 2; i++) {
                const Rgba lc = sd[i]->tone == T_MUTED ? kLabel : toneCol(sd[i]->tone);
                c.tc.draw(c.g, tf, i ? "R" : "L", bx, y - 2, kLabel);
                const Rect lb{bx + 14, y + 2, bw - 14 - 60, 7};
                c.g.rect(lb.x, lb.y, lb.w, lb.h, rgb(0xffa000, 0.12f));
                c.g.rect(lb.x, lb.y, lb.w * (float)(sd[i]->pct / 100), lb.h, lc);
                c.g.rect(lb.x + lb.w * 0.75f, lb.y, 1, lb.h, rgb(0xffffff, 0.55f));
                c.tc.drawTab(c.g, tf, sd[i]->text, bx + bw, y - 2, lc, 2);
                y += 13;
            }
        } else y += 26;
        const std::string pw = k.powSettling.empty() ? k.powValue : k.powValue + " \xC2\xB7 " + k.powSettling;
        c.tc.draw(c.g, c.f.small, "MPX power", bx, y, kLabel);
        c.tc.draw(c.g, c.f.small, pw, bx + bw, y, rgb(0xffffff, 0.75f), 2);
    }
    // ── column C: SCOPE VIEW — each band ALONE, in the colour of its verdict ──
    {
        float y = body.y + 2;
        plotLabel(c, "SCOPE VIEW", cx, y); y += tl;
        const float bh = std::min(44.f, std::floor((body.b() - y - 3 * tl - 2) / 3) - 3);
        // ★ the three pictures in one pass (one texture: one batch), then the marks and labels over them
        for (int b = 0; b < 3; b++) {
            const Rect sb{cx, y + b * (bh + 3), cw, bh};
            plotBox(c, sb);
            if (u.eyeTex && u.eyeOk[b] && u.texH > 0) {
                const float v0 = (b * u.texH + 0.5f) / (3.f * u.texH), v1 = ((b + 1) * u.texH - 0.5f) / (3.f * u.texH);
                c.g.imageUV(u.eyeTex, sb.x, sb.y, sb.w, sb.h, 0, v0, 1, v1, toneCol(k.bands[b].tone));
            }
        }
        for (int b = 0; b < 3; b++) {
            const Rect sb{cx, y, cw, bh};
            c.g.rect(sb.x, sb.cy() - 0.5f, sb.w, 1, kAxisStrong);
            c.g.rect(sb.cx() - 0.5f, sb.y, 1, sb.h, kAxis);
            std::string nm = k.bands[b].name;
            if (k.bands[b].khz > 0.05) nm += "  " + fx(k.bands[b].khz, 1) + " kHz";
            c.tc.draw(c.g, tf, nm, sb.x + 3, sb.y + 1, toneCol(k.bands[b].tone));
            y += bh + 3;
        }
        verdictText(c, tf, k.eyeV.first, k.eyeV.second, cx, y, cw, 3);
    }
}

/** The body for any host (the standalone box, or the decoder box via adv_rds_dec.h). */
inline void ardsDrawBody(App& a, Ctx& c, const RadioState& s, Rect body, bool grabbed) {
    ArdsUi& u = ardsUi();
    // ★ the body is the first stop (a fresh grab lands on it, not on a key); ENTER on it steps to the next view
    if (grabbed) {
        const Nav::Hit h = c.nav.item("ards.body", body);
        if (h.pressed) { u.view = (u.view + 1) % 3; u.scrollY = 0; }
        if (h.focused) c.g.rframe(body.x - 1, body.y - 1, body.w + 2, body.h + 2, 7, 3, th::focus);
    }
    if (!u.core.have) {
        const std::string t = u.subscribed ? "waiting for the analyser\xE2\x80\xA6 (the first frame takes about a second)"
                                           : "paused while covered \xE2\x80\x94 the analyser runs only while this box is on screen";
        const auto& ln = c.tc.wrap(c.f.small, t, (int)body.w - 16);
        float y = body.y + 4;
        for (size_t i = 0; i < ln.size() && y + c.f.small.lineHeight() <= body.b() + 2; i++, y += c.f.small.lineHeight())
            c.tc.draw(c.g, c.f.small, ln[i], body.x + 8, y, th::dim);
        return;
    }
    if (u.view == ArdsUi::V_SUMMARY) ardsDrawSummary(c, body);
    else if (u.view == ArdsUi::V_ALL) ardsDrawAll(a, c, body, grabbed);
    else ardsDrawPlots(c, body);
}
/** The header's status: the group rate and the device's CPU — the cost, where it is paid. */
inline std::string ardsStatus(const App& a) {
    ArdsUi& u = ardsUi();
    std::string st;
    if (u.core.rate > 0) st = ardr::fx(u.core.rate, 1) + " groups/s";
    else if (u.core.have) st = "no groups yet";
    if (a.cpuPct_ >= 0) st += (st.empty() ? "" : " \xC2\xB7 ") + std::string("CPU ") + std::to_string(a.cpuPct_) + " %";
    return st;
}

/** The standalone box (call after dabDrawBox in the deck pass). */
inline void ardsDrawBox(App& a, Ctx& c, const RadioState& s) {
    ArdsUi& u = ardsUi();
    const bool grabbed = ardsGrabbed(a);
    if (kArdsSheet == App::S_NONE || u.layer != "ards") return;   // (not wired standalone, or the decoder box hosts it)
    if (!u.open || s.session != "running") { if (grabbed) ardsRelease(a); return; }
    const vts::State& vs = vts::vtsState();
    const bool vtsUp = vs.bar.shown() && vs.bar.opacity() > 0.004;
    const float top = 156, bot = vtsUp ? 270.f : 308.f;
    const bool tall = u.view != ArdsUi::V_SUMMARY;
    const Rect box = tall ? Rect{6, 44, 628, bot - 44} : Rect{6, top, 628, bot - top};
    c.nav.layer("ards");
    c.g.rrect(box.x, box.y, box.w, box.h, 12, rgb(0x0c0802, tall ? 0.94f : 0.88f));
    c.g.rframe(box.x, box.y, box.w, box.h, 12, 1, th::btnBorder);
    const Rect head{box.x, box.y, box.w, 27};
    const Rect body{box.x + 4, head.b() + 2, box.w - 10, box.b() - head.b() - 4};
    ardsDrawBody(a, c, s, body, grabbed);   // (FIRST: its stop is the one a fresh grab lands on)
    // the header: LED · title · status · [keys]
    const bool live = u.lastAt > 0 && nowSec() - u.lastAt < 1.5;
    c.g.circle(head.x + 14, head.cy(), 5, 0, live ? rgb(0x35e08a) : rgb(0x696252));
    const float tw = (float)c.tc.draw(c.g, c.f.bodyB, "ADVANCED RDS", head.x + 26, head.cy() - c.f.bodyB.lineHeight() / 2.f, th::btnText);
    struct K { const char* id; const char* label; int act; };
    static const K ks[] = {{"ards.k.sum", "SUMMARY", 0}, {"ards.k.all", "ALL", 1}, {"ards.k.plots", "PLOTS", 2}, {"ards.k.close", "CLOSE", 3}};
    float kx = head.r() - 6;
    Rect kr[4];
    if (grabbed) for (int i = 3; i >= 0; i--) { const float w = (float)c.tc.width(c.f.tiny, ks[i].label) + 18; kx -= w; kr[i] = {kx, head.y + 4, w, head.h - 6}; kx -= 5; }
    const float sx = head.x + 26 + tw + 10;
    c.tc.draw(c.g, c.f.small, ardr::fitText(c, c.f.small, ardsStatus(a), (int)((grabbed ? kx : head.r() - 8) - sx - 4)), sx,
              head.cy() - c.f.small.lineHeight() / 2.f, th::white68);
    c.g.rect(box.x + 8, head.b(), box.w - 16, 1, th::barBorder);
    if (grabbed) {
        int act = -1;
        for (int i = 0; i < 4; i++) {
            BtnStyle st; st.font = &c.f.tiny; st.radius = 6; st.on = ks[i].act == u.view;
            if (button(c, ks[i].id, kr[i], ks[i].label, st)) act = ks[i].act;
        }
        if (act == 3) { ardsClose(a); c.nav.layer("deck"); return; }
        if (act >= 0 && act != u.view) { u.view = act; u.scrollY = 0; }
    }
    c.nav.layer("deck");
    // released: the box is ONE stop on the island's layer — ENTER grabs
    if (!grabbed) {
        const Nav::Hit h = c.nav.item("ards.box", box);
        if (h.focused) {
            c.g.rframe(box.x + 1, box.y + 1, box.w - 2, box.h - 2, 12, 3, th::focus);
            const std::string t = "views \xC2\xB7 scroll";
            const float hw = 24 + (float)c.tc.width(c.f.small, t) + 20;
            const Rect hb{box.r() - hw - 8, box.y + 2, hw, 23};   // ★ in the header's empty right (the keys' place), never over a reading
            c.g.rrect(hb.x, hb.y, hb.w, hb.h, 13, rgb(0, 0.85f));
            c.g.rframe(hb.x, hb.y, hb.w, hb.h, 13, 1, th::focus);
            hints(c, hb.x + 8, hb.cy(), {{FACE_BOTTOM, t}}, th::focus);
        }
        if (h.pressed) { a.open(kArdsSheet); a.nav_.forget("ards"); }
    }
    if (u.grabNext && !grabbed) { u.grabNext = false; a.open(kArdsSheet); a.nav_.forget("ards"); }
}

}  // namespace hh
