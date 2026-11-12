// sdr_ui.h — MENU → SDR HARDWARE's row helpers (sec_sdr.h and its sdr_*.h parts). Each draws one labelled menu row
// the way sec_display.h does (menuRow for the label + wrapped help, the control in the label band at the right or on
// its own line below), and reports what the person did. Nothing here keeps radio state: the rows are handed the value
// the radio REPORTED (hwinfo / rspstat), and `held` only bridges the moment between a press and the radio's answer.
#pragma once
// (included by sec_sdr.h, after App is declared)

namespace hh {
namespace sdr {

/** One frame of the section: the App (its helpers and scratch maps), the drawing context, the radio, the area. */
struct S {
    App& A;
    Ctx& c;
    const RadioState& s;
    Rect a;
    double now;
    const Json& hwi() const { return s.msg("hwinfo"); }
    const Json& radio() const { return s.msg("hwinfo")["radio"]; }
};

inline std::string fmt(const char* f, double v) { char b[96]; std::snprintf(b, sizeof b, f, v); return b; }
inline std::string fmtI(const char* f, int v) { char b[96]; std::snprintf(b, sizeof b, f, v); return b; }
/** The web's `(x / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '')`. */
inline std::string msps(double hz) {
    std::string t = fmt("%.3f", hz / 1e6);
    while (!t.empty() && t.back() == '0') t.pop_back();
    if (!t.empty() && t.back() == '.') t.pop_back();
    return t + " MS/s";
}
/** dBFS the way rspAgcReadout.ts writes it: a real minus sign, rounded. */
inline std::string dbfs(double v) { return std::string(v < 0 ? "−" : "") + std::to_string((int)std::lround(std::fabs(v))); }

// ── the press → answer bridge ───────────────────────────────────────────────────────────────────────────────────────
// ★ The shared-dial contract: the radio's report is the truth. After a press the row shows what was asked for until
//   the radio's report agrees, until the radio reports something NEW (a refusal, another listener), or for `holdS` at
//   most (the web's setToggleTo ignores a report for 4 s after a press, the same idea) — so a control never flickers
//   back for the beat before the answer, and never claims a state for longer than it must.
// ★ `kSilent` (pass as holdS): for the engine handlers that act but send no fresh hwinfo afterwards (`agc` — the
//   dongle's digital AGC — and `ppm`), the old report would win after any timeout and the row would flip back to a
//   state the radio is no longer in; those hold until a report disagrees with what was there at the press.
constexpr double kSilent = 1e12;
inline double held(S& x, const std::string& k, double reported, double holdS = 3.0) {
    auto& n = x.A.num_;
    auto it = n.find("sdr.pend." + k);
    if (it == n.end()) { n["sdr.rep." + k] = reported; return reported; }
    const double v = it->second, t = n["sdr.pendT." + k], was = n["sdr.pendR." + k];
    if (x.now - t > holdS || std::fabs(reported - v) < 1e-6 || std::fabs(reported - was) > 1e-6) {
        n.erase("sdr.pend." + k); n.erase("sdr.pendT." + k); n.erase("sdr.pendR." + k);
        n["sdr.rep." + k] = reported;
        return reported;
    }
    return v;
}
/** After a press: show `v` (see held — which has already noted what the radio said this frame). */
inline void hold(S& x, const std::string& k, double v) {
    auto& n = x.A.num_;
    if (!n.count("sdr.pendR." + k)) n["sdr.pendR." + k] = n.count("sdr.rep." + k) ? n["sdr.rep." + k] : v;
    n["sdr.pend." + k] = v; n["sdr.pendT." + k] = x.now;
}

inline void send(S& x, const std::string& json) { if (x.A.eng_) x.A.eng_->send(json); }
/** {"type":"gain","value":<tenths>} — the generic gain (RTL, Airspy R2 preset, USRP). */
inline void sendGain(S& x, int tenths) { if (x.A.eng_) x.A.eng_->gain(tenths); }
/** {"type":<type>,"<key>":<int>} — every driver's *_control message has this shape. */
inline void sendKV(S& x, const char* type, const char* key, long v) {
    send(x, std::string("{\"type\":\"") + type + "\",\"" + key + "\":" + std::to_string(v) + "}");
}
/** ★ A SLIDER's message (key_repeat.h LatestSender): at once, or while it is HELD at most 10 a second, the latest value
 *  and never the last one lost — a held gain slider would otherwise send the radio 30 gain changes a second. */
inline void sendSlider(S& x, const std::string& json) {
    App* A = &x.A;
    const size_t c = json.rfind(':');
    A->sendLatest("sdr|" + json.substr(0, c == std::string::npos ? json.size() : c), [A, json] { if (A->eng_) A->eng_->send(json); });
}
inline void sendKVSlider(S& x, const char* type, const char* key, long v) {
    sendSlider(x, std::string("{\"type\":\"") + type + "\",\"" + key + "\":" + std::to_string(v) + "}");
}

// ── drawing ─────────────────────────────────────────────────────────────────────────────────────────────────────────
inline float textW(S& x, const Font& f, const std::string& t) { return (float)x.c.tc.width(f, t); }
/** A value at the right of a row's label band. */
inline void valueAt(S& x, float y0, const std::string& t, Rgba col = th::dim) {
    x.c.tc.draw(x.c.g, x.c.f.body, t, x.a.r() - 6, y0 + 22 - x.c.f.body.lineHeight() / 2.f, col, 2);
}
/** Wrapped small text under a row (a note, a status line). */
inline float note(S& x, float y, const std::string& t, Rgba col = th::dim) {
    if (t.empty()) return y;
    for (auto& l : x.c.tc.wrap(x.c.f.small, t, (int)x.a.w - 12)) { x.c.tc.draw(x.c.g, x.c.f.small, l, x.a.x + 6, y, col); y += x.c.f.small.lineHeight(); }
    return y + 8;
}
/** A sub-heading inside the section (the web's <h3>, e.g. ANTENNA). */
inline float heading(S& x, float y, const std::string& t) {
    x.c.g.rect(x.a.x + 6, y + 8, x.a.w - 12, 1, th::btnBorder);
    x.c.tc.draw(x.c.g, x.c.f.small, t, x.a.x + 6, y + 14, th::dim);
    return y + 14 + x.c.f.small.lineHeight() + 6;
}

/** A read-only row: label, help, a value at the right. */
inline void textRow(S& x, float& y, const std::string& label, const std::string& help, const std::string& value, Rgba col = th::dim) {
    const float y0 = y;
    y = x.A.menuRow(x.c, x.a, y, label, help);
    valueAt(x, y0, value, col);
}

// ★★ THE D-PAD BOX IS THE WHOLE ROW. ui.h's widgets register their own drawn box with the Nav, and the Nav picks the
//    nearest box that way with the across-distance penalised x2.5 (gamepadNav.ts pickNext). A chip at the far right of
//    a row is then further "across" from a full-width slider above it than the row after it is "along" — so DOWN from
//    GAIN jumped straight past VIBE AGC and it could not be reached at all. Every row here registers {area.x, row, area.w}
//    and draws its control (and the ring) where it sits, so UP / DOWN walk the rows in order.
inline Nav::Hit rowItem(S& x, const std::string& id, float top, float h, bool takesLR, bool repeats = false) {
    return x.c.nav.item(id, {x.a.x, top, x.a.w, h}, takesLR, repeats);
}

/** The slider, or — when the radio owns the value — the same track drawn dim and out of the D-pad's reach (a control
 *  that cannot be moved must not be highlightable: the press would do nothing). `navTop/navH`: the row's box. */
inline bool sliderAt(S& x, const std::string& id, Rect r, double& v, double lo, double hi, double step, bool enabled,
                     float navTop, float navH, bool* pressed = nullptr) {
    const float ty = r.cy() - 3;
    if (!enabled) {
        const float frac = hi > lo ? (float)((std::max(lo, std::min(hi, v)) - lo) / (hi - lo)) : 0.f;
        x.c.g.rrect(r.x, ty, r.w, 6, 3, th::meterTrack);
        x.c.g.rrect(r.x, ty, std::max(6.f, r.w * frac), 6, 3, th::dimmer);
        x.c.g.circle(r.x + r.w * frac, r.cy(), 9, 0, th::dimmer);
        return false;
    }
    const Nav::Hit h = rowItem(x, id, navTop, navH, true, true);   // ★ held LEFT / RIGHT slide it
    if (pressed) *pressed = h.pressed;
    bool changed = false;
    if (h.adjust) {
        const double nv = std::max(lo, std::min(hi, std::round((v + h.adjust * step - lo) / step) * step + lo));
        if (nv != v) { v = nv; changed = true; }
    }
    const float frac = hi > lo ? (float)((std::max(lo, std::min(hi, v)) - lo) / (hi - lo)) : 0.f;
    x.c.g.rrect(r.x, ty, r.w, 6, 3, th::meterTrack);
    x.c.g.rrect(r.x, ty, std::max(6.f, r.w * frac), 6, 3, th::amber);
    x.c.g.circle(r.x + r.w * frac, r.cy(), 10, 0, th::amber);
    if (h.focused) focusRing(x.c, r, 8, true);
    return changed;
}

/** Label + help + a slider; `valueW` is the room kept for the value text (fixed, so the slider never jumps).
 *  `pressed` (optional) reports ENTER on it. */
inline bool rowSlider(S& x, float& y, const std::string& id, const std::string& label, const std::string& help,
                      double& v, double lo, double hi, double step, const std::string& valueText, bool enabled = true,
                      float valueW = 96, bool* pressed = nullptr) {
    const float y0 = y;
    y = x.A.menuRow(x.c, x.a, y, label, help);
    const float sx = x.a.x + std::max(150.f, textW(x, x.c.f.bodyB, label) + 28);
    const float sw = x.a.r() - 6 - valueW - 16 - sx;
    bool changed;
    if (sw >= 150) {
        changed = sliderAt(x, id, {sx, y0 + 2, sw, 40}, v, lo, hi, step, enabled, y0, 44, pressed);
    } else {
        // a long label and a long value: the slider takes its own line under the help
        changed = sliderAt(x, id, {x.a.x + 16, y, x.a.w - 32, 40}, v, lo, hi, step, enabled, y, 40, pressed);
        y += 50;
    }
    valueAt(x, y0, valueText, enabled ? th::dim : th::dimmer);
    return changed;
}

/** Label + help + an ON/OFF chip at the right. Returns true when pressed (the caller sends and holds). */
inline bool rowToggle(S& x, float& y, const std::string& id, const std::string& label, const std::string& help, bool on,
                      const std::string& onT = "ON", const std::string& offT = "OFF", bool enabled = true) {
    const float y0 = y;
    y = x.A.menuRow(x.c, x.a, y, label, help);
    const float w = std::max(90.f, std::max(textW(x, x.c.f.bodyB, onT), textW(x, x.c.f.bodyB, offT)) + 30);
    const Rect r{x.a.r() - 6 - w, y0 + 4, w, 38};
    const Nav::Hit h = enabled ? rowItem(x, id, y0, 44, false) : Nav::Hit{};
    const float al = enabled ? 1.f : 0.38f;
    x.c.g.rrect(r.x, r.y, r.w, r.h, 7, on ? th::onBg : th::btnBg);
    Rgba bd = on ? th::onBg : th::btnBorder; bd.a *= al;
    x.c.g.rframe(r.x, r.y, r.w, r.h, 7, 1, bd);
    Rgba tx = on ? th::onText : th::btnText; tx.a *= al;
    textIn(x.c, x.c.f.bodyB, on ? onT : offT, r, tx);
    if (h.focused) focusRing(x.c, r, 7);
    return h.pressed;
}

/** A row of choices, on its own line under the label and help (the web's block rows; a choice row squeezed in at the
 *  right of its label would be skipped by the D-pad, as above). `enabled` false draws it dim and out of reach.
 *  Returns the index pressed, or -1. */
inline int rowSeg(S& x, float& y, const std::string& id, const std::string& label, const std::string& help,
                  const std::vector<std::string>& opts, int sel, bool enabled = true) {
    y = x.A.menuRow(x.c, x.a, y, label, help);
    const Rect r{x.a.x + 6, y, x.a.w - 12, 40};
    y += 52;
    if (enabled) return segmented(x.c, id, r, opts, sel, 6);
    const float gap = 6, w = (r.w - gap * (opts.size() - 1)) / opts.size();
    for (size_t i = 0; i < opts.size(); i++) {
        BtnStyle st; st.lit = (int)i == sel; st.font = &x.c.f.body; st.radius = 7; st.disabled = true;
        button(x.c, id + "#" + std::to_string(i), {r.x + i * (w + gap), r.y, w, r.h}, opts[i], st);
    }
    return -1;
}

/** The web's <select>: a box showing the current choice; with the highlight, D-pad left / right step through the
 *  options (skipping ones `ok` rules out — the web's disabled <option>s), and ENTER steps forward, wrapping. Returns
 *  the new index, or -1. `enabled` false shows the choice dim and out of reach (DAB holds the rate). */
inline int rowSelect(S& x, float& y, const std::string& id, const std::string& label, const std::string& help,
                     const std::vector<std::string>& opts, int sel, const std::vector<bool>& ok = {}, bool enabled = true) {
    const float y0 = y;
    y = x.A.menuRow(x.c, x.a, y, label, help);
    const float bx = x.a.x + std::max(150.f, textW(x, x.c.f.bodyB, label) + 28);
    const Rect r{bx, y0 + 4, x.a.r() - 6 - bx, 38};
    auto allowed = [&](int i) { return i >= 0 && i < (int)opts.size() && (ok.empty() || ok[(size_t)i]); };
    int out = -1;
    Nav::Hit h;
    if (enabled && opts.size() > 1) {
        h = rowItem(x, id, y0, 44, true);
        if (h.adjust) {
            for (int i = sel + h.adjust; i >= 0 && i < (int)opts.size(); i += h.adjust) if (allowed(i)) { out = i; break; }
        } else if (h.pressed) {
            for (int k = 1; k <= (int)opts.size(); k++) { const int i = (sel + k) % (int)opts.size(); if (allowed(i)) { out = i; break; } }
            if (out == sel) out = -1;
        }
    }
    const bool live = enabled && opts.size() > 1;
    const float a = live ? 1.f : 0.45f;
    x.c.g.rrect(r.x, r.y, r.w, r.h, 7, th::btnBg);
    Rgba bd = th::btnBorder; bd.a *= a;
    x.c.g.rframe(r.x, r.y, r.w, r.h, 7, 1, bd);
    Rgba tx = th::btnText; tx.a *= a;
    const int show = out >= 0 ? out : sel;
    textIn(x.c, x.c.f.body, show >= 0 && show < (int)opts.size() ? opts[(size_t)show] : "—", r, tx);
    if (live) {
        bool canL = false, canR = false;
        for (int i = 0; i < (int)opts.size(); i++) if (allowed(i)) { if (i < show) canL = true; if (i > show) canR = true; }
        Rgba l = th::btnText, rr = th::btnText;
        l.a *= canL ? 1.f : 0.3f; rr.a *= canR ? 1.f : 0.3f;
        textIn(x.c, x.c.f.body, "‹", r, l, 0, 10);
        textIn(x.c, x.c.f.body, "›", r, rr, 2, 10);
    }
    if (h.focused) focusRing(x.c, r, 7, true);
    return out;
}

/** A grid of labelled ON/OFF buttons (the RSP's FILTERS group, the antenna ports). Returns the index pressed, or -1.
 *  `focusedOut` gets the highlighted index (for its explanation under the grid), or -1. */
inline int buttonGrid(S& x, float& y, const std::string& id, const std::vector<std::string>& labels, const std::vector<bool>& lit,
                      const std::vector<bool>& enabled, int& focusedOut, int maxCols = 3) {
    focusedOut = -1;
    if (labels.empty()) return -1;
    float ow = 0;
    for (auto& l : labels) ow = std::max(ow, textW(x, x.c.f.body, l) + 30);
    const float gap = 8, full = x.a.w - 12;
    int cols = std::min<int>((int)labels.size(), maxCols);
    while (cols > 1 && ow * cols + gap * (cols - 1) > full) cols--;
    const float w = (full - gap * (cols - 1)) / cols;
    int hit = -1;
    const std::string f = x.A.nav_.focus("menu");
    for (size_t i = 0; i < labels.size(); i++) {
        const int r = (int)i / cols, col = (int)i % cols;
        const std::string bid = id + "#" + std::to_string(i);
        BtnStyle st; st.lit = lit[i]; st.font = &x.c.f.body; st.radius = 7; st.disabled = !enabled[i];
        if (button(x.c, bid, {x.a.x + 6 + col * (w + gap), y + r * 48, w, 40}, labels[i], st)) hit = (int)i;
        if (enabled[i] && f == bid) focusedOut = (int)i;
    }
    y += ((int)labels.size() + cols - 1) / cols * 48 + 6;
    return hit;
}

}  // namespace sdr
}  // namespace hh
