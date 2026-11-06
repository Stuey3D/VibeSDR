// ui.h — vibe-hhui's controls: the web handheld page's look (web/client/index.html's design tokens, amber on black) drawn
// with Gfx, and the D-pad highlight that moves between them the way the web page's gamepad layer does.
//
// ★★ IMMEDIATE MODE. Each frame the screen code calls the widgets in order; each one registers its box with the Nav
//    and draws itself. The Nav knows which box has the highlight, reports a press on it, and — after the frame — moves
//    the highlight for a D-pad press to the nearest box that way (src/utils/gamepadNav.ts pickNext, ported).
// ★★ LAYERS: one per sheet (the deck, the menu, the audio sheet, the keypad …). Only the TOP layer takes the pad; each
//    layer remembers its own highlight, so closing a sheet puts the highlight back where it was underneath.
// ★ The highlight is the web's .gpFocus: a 3 px #7cff9b ring 2 px outside the control; AMBER while a slider is held
//   (.gpAdjust). Text is Atkinson Hyperlegible everywhere (Stuart, 2026-11-06: "that resolution and screen size will need it").
#pragma once
#include "gfx.h"
#include "text.h"

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hh {

// ── the web's design tokens (web/client/index.html :root) ──────────────────────────────────────────────────────────────
namespace th {
inline const Rgba bg        = rgb(0x080601);
inline const Rgba barBorder = rgb(0xffa000, 0.22f);
inline const Rgba btnBg     = rgb(0x140a00, 0.75f);
inline const Rgba pillBg    = rgb(0x140a00);
inline const Rgba btnBorder = rgb(0xffa000, 0.35f);
inline const Rgba btnText   = rgb(0xffb833);
inline const Rgba activeBg  = rgb(0xffc800, 0.10f);
inline const Rgba activeBdr = rgb(0xffe566, 0.55f);
inline const Rgba activeTx  = rgb(0xffe566);
inline const Rgba amber     = rgb(0xffb833);
inline const Rgba unit      = rgb(0x886600);
inline const Rgba dim       = rgb(0xffa000, 0.45f);
inline const Rgba dimmer    = rgb(0xffa000, 0.30f);
inline const Rgba meterTrack= rgb(0x696252, 0.30f);
inline const Rgba red       = rgb(0xe05050);
inline const Rgba green     = rgb(0x28a745);
inline const Rgba white     = rgb(0xffffff);
inline const Rgba white68   = rgb(0xffffff, 0.68f);
inline const Rgba focus     = rgb(0x7cff9b);
inline const Rgba sheetBg   = rgb(0x0c0802, 0.96f);
inline const Rgba onBg      = rgb(0xffe566);          // a lit segment / ON chip: the web's yellow fill, dark text
inline const Rgba onText    = rgb(0x1a1200);
inline const Rgba shade     = rgb(0x000000, 0.55f);
}  // namespace th

struct Fonts {
    Font tiny, small, body, bodyB, btn, title, big, freq, freqFine, unit;   // Atkinson Hyperlegible
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
    float r() const { return x + w; }
    float b() const { return y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

enum Dir { D_NONE, D_UP, D_DOWN, D_LEFT, D_RIGHT };

/** One frame's input for the top layer. */
struct Input {
    Dir nav = D_NONE;
    bool enter = false, back = false;
};

class Nav {
public:
    struct Hit { bool focused = false, pressed = false; int adjust = 0; };

    /** Start a frame: `top` is the layer that takes the pad this frame. */
    void begin(const std::string& top, const Input& in) {
        top_ = top; in_ = in; items_.clear(); consumed_ = false; layerNow_.clear();
    }
    /** The layer the following items belong to. */
    void layer(const std::string& l) { layerNow_ = l; }
    bool topLayer() const { return layerNow_ == top_; }

    /** Register a control. `takesLR`: while it has the highlight, D-pad left / right adjust it (a slider, a stepper)
     *  instead of moving — the web's range inputs do the same. Returns whether it has the highlight, was pressed, and
     *  any adjustment this frame. */
    Hit item(const std::string& id, Rect r, bool takesLR = false) {
        Hit h;
        if (layerNow_ != top_) {
            // not the top layer: drawn, never highlighted
            return h;
        }
        items_.push_back({id, r, takesLR});
        std::string& f = focus_[top_];
        if (f.empty() && !pendingDefault_.count(top_)) pendingDefault_[top_] = id;
        if (f == id) {
            h.focused = true;
            if (in_.enter && !consumed_) { h.pressed = true; consumed_ = true; }
            if (takesLR && !consumed_ && (in_.nav == D_LEFT || in_.nav == D_RIGHT)) { h.adjust = in_.nav == D_LEFT ? -1 : 1; consumed_ = true; }
        }
        return h;
    }
    /** Give a control the highlight (the next frame draws it). */
    void setFocus(const std::string& layer, const std::string& id) { focus_[layer] = id; }
    std::string focus(const std::string& layer) const { auto it = focus_.find(layer); return it == focus_.end() ? "" : it->second; }
    void forget(const std::string& layer) { focus_.erase(layer); pendingDefault_.erase(layer); }
    bool backPressed() { if (in_.back && !consumed_) { consumed_ = true; return true; } return false; }
    const Input& input() const { return in_; }
    /** The highlighted control's box this frame (for scrolling it into view), or nullptr. */
    const Rect* focusedRect() const {
        auto it = focus_.find(top_);
        if (it == focus_.end()) return nullptr;
        for (auto& i : items_) if (i.id == it->second) return &i.r;
        return nullptr;
    }

    /** After the frame: move the highlight for an unused D-pad press; fix a highlight that no longer exists. */
    void end() {
        std::string& f = focus_[top_];
        bool present = false;
        for (auto& i : items_) if (i.id == f) present = true;
        if (!present) {
            auto d = pendingDefault_.find(top_);
            f = d != pendingDefault_.end() ? d->second : (items_.empty() ? "" : items_.front().id);
            if (!items_.empty()) { bool ok = false; for (auto& i : items_) if (i.id == f) ok = true; if (!ok) f = items_.front().id; }
        }
        pendingDefault_.erase(top_);
        if (consumed_ || in_.nav == D_NONE || items_.empty()) return;
        const Item* cur = nullptr;
        for (auto& i : items_) if (i.id == f) cur = &i;
        const int k = pickNext(cur ? &cur->r : nullptr, in_.nav);
        if (k >= 0) f = items_[(size_t)k].id;
    }

private:
    struct Item { std::string id; Rect r; bool takesLR; };
    std::string top_, layerNow_;
    Input in_;
    bool consumed_ = false;
    std::vector<Item> items_;
    std::map<std::string, std::string> focus_, pendingDefault_;

    // src/utils/gamepadNav.ts pickNext — the nearest box that way; across-distance penalised ×2.5
    int pickNext(const Rect* cur, Dir dir) const {
        if (!cur) {
            int best = 0;
            for (size_t i = 1; i < items_.size(); i++)
                if (items_[i].r.y + items_[i].r.x * 0.5f < items_[(size_t)best].r.y + items_[(size_t)best].r.x * 0.5f) best = (int)i;
            return best;
        }
        int best = -1;
        double bestScore = 1e30;
        for (size_t i = 0; i < items_.size(); i++) {
            const Rect& b = items_[i].r;
            if (b.x == cur->x && b.y == cur->y && b.w == cur->w && b.h == cur->h) continue;
            const double dx = b.cx() - cur->cx(), dy = b.cy() - cur->cy();
            const double along = dir == D_RIGHT ? dx : dir == D_LEFT ? -dx : dir == D_DOWN ? dy : -dy;
            const double across = (dir == D_LEFT || dir == D_RIGHT) ? std::fabs(dy) : std::fabs(dx);
            if (along <= 1) continue;
            const double score = along + across * 2.5;
            if (score < bestScore) { bestScore = score; best = (int)i; }
        }
        return best;
    }
};

/** The drawing context every widget gets. */
struct Ctx {
    Gfx& g;
    TextCache& tc;
    Fonts& f;
    Nav& nav;
    double t = 0;
};

// ── drawing helpers ───────────────────────────────────────────────────────────────────────────────────────────────────
inline void focusRing(Ctx& c, Rect r, float radius, bool adjust = false) {
    c.g.rframe(r.x - 5, r.y - 5, r.w + 10, r.h + 10, radius + 4, 3, adjust ? th::amber : th::focus);
}
/** Text vertically centred in r (align 0 left from r.x+pad, 1 centre, 2 right). */
inline void textIn(Ctx& c, const Font& f, const std::string& s, Rect r, Rgba col, int align = 1, float pad = 0) {
    const float y = r.cy() - f.lineHeight() / 2.f;
    const float x = align == 1 ? r.cx() : align == 2 ? r.r() - pad : r.x + pad;
    c.tc.draw(c.g, f, s, x, y, col, align);
}

// ── widgets ──────────────────────────────────────────────────────────────────────────────────────────────────────────
struct BtnStyle { bool on = false, disabled = false; const Font* font = nullptr; Rgba text = th::btnText; float radius = 9; bool lit = false; };

/** The web's .mBtn / .btn: dark glass, amber hairline, amber text. `on` = the web's .on (active border + text);
 *  `lit` = a selected segment (yellow fill, dark text). Returns true when pressed. */
inline bool button(Ctx& c, const std::string& id, Rect r, const std::string& label, BtnStyle st = {}) {
    const Nav::Hit h = st.disabled ? Nav::Hit{} : c.nav.item(id, r);
    const float a = st.disabled ? 0.38f : 1.f;
    if (st.lit) c.g.rrect(r.x, r.y, r.w, r.h, st.radius, th::onBg);
    else c.g.rrect(r.x, r.y, r.w, r.h, st.radius, st.on ? th::activeBg : th::btnBg);
    Rgba bd = st.lit ? th::onBg : st.on ? th::activeBdr : th::btnBorder; bd.a *= a;
    c.g.rframe(r.x, r.y, r.w, r.h, st.radius, 1, bd);
    Rgba tx = st.lit ? th::onText : st.on ? th::activeTx : st.text; tx.a *= a;
    if (!label.empty()) textIn(c, st.font ? *st.font : c.f.btn, label, r, tx);
    if (h.focused) focusRing(c, r, st.radius);
    return h.pressed;
}

/** A row of choices (the web's .seg); returns the index pressed, or -1. `sel` is drawn lit. */
inline int segmented(Ctx& c, const std::string& id, Rect r, const std::vector<std::string>& opts, int sel, float gap = 6,
                     const Font* font = nullptr) {
    if (opts.empty()) return -1;
    const float w = (r.w - gap * (opts.size() - 1)) / opts.size();
    int hit = -1;
    for (size_t i = 0; i < opts.size(); i++) {
        BtnStyle st; st.lit = (int)i == sel; st.font = font ? font : &c.f.body; st.radius = 7;
        if (button(c, id + "#" + std::to_string(i), {r.x + i * (w + gap), r.y, w, r.h}, opts[i], st)) hit = (int)i;
    }
    return hit;
}

/** The web's range input: amber track + ball. While highlighted, D-pad left / right step it (and the ring turns
 *  amber, the web's .gpAdjust). Returns true when the value changed. */
inline bool slider(Ctx& c, const std::string& id, Rect r, double& v, double lo, double hi, double step) {
    const Nav::Hit h = c.nav.item(id, r, true);
    bool changed = false;
    if (h.adjust) {
        const double nv = std::max(lo, std::min(hi, std::round((v + h.adjust * step - lo) / step) * step + lo));
        if (nv != v) { v = nv; changed = true; }
    }
    const float ty = r.cy() - 3, th_ = 6;
    const float frac = hi > lo ? (float)((v - lo) / (hi - lo)) : 0.f;
    c.g.rrect(r.x, ty, r.w, th_, 3, th::meterTrack);
    c.g.rrect(r.x, ty, std::max(6.f, r.w * frac), th_, 3, th::amber);
    c.g.circle(r.x + r.w * frac, r.cy(), 10, 0, th::amber);
    if (h.focused) focusRing(c, r, 8, true);
    return changed;
}

/** ON / OFF chip (the web's small toggle buttons). Returns true when pressed (the caller flips the state). */
inline bool toggle(Ctx& c, const std::string& id, Rect r, bool on, const std::string& onText = "ON", const std::string& offText = "OFF") {
    BtnStyle st; st.lit = on; st.font = &c.f.bodyB; st.radius = 7;
    return button(c, id, r, on ? onText : offText, st);
}

/** A scrolling sheet's state: the content scrolls so the highlight stays in view. */
struct Scroller {
    float y = 0;        // current offset (pixels scrolled)
    float target = 0;
    float contentH = 0;
    /** Call after the content was laid out: items were drawn at view.y + contentY − y. Keeps the highlighted box in
     *  view, easing there. */
    void follow(const Rect* focused, Rect view, float margin = 16) {
        if (focused) {
            const float top = focused->y - view.y + y, bot = focused->b() - view.y + y;   // content coordinates
            if (top - margin < target) target = top - margin;
            if (bot + margin > target + view.h) target = bot + margin - view.h;
        }
        target = std::max(0.f, std::min(target, std::max(0.f, contentH - view.h)));
        y += (target - y) * 0.45f;
        if (std::fabs(target - y) < 0.5f) y = target;
    }
};

/** Switch-style face glyph: a diamond of four circles, the meant one solid (memory: gamepad_mapping, 2026-11-06).
 *  pos: 0 top, 1 left, 2 right, 3 bottom. */
inline void padFace(Gfx& g, float cx, float cy, float size, int solid, Rgba c) {
    const float r = size * 0.16f, d = size * 0.30f;
    const float px[4] = {0, -d, d, 0}, py[4] = {-d, 0, 0, d};
    for (int i = 0; i < 4; i++) g.circle(cx + px[i], cy + py[i], r, i == solid ? 0.f : 1.4f, c);
}
enum { FACE_TOP = 0, FACE_LEFT = 1, FACE_RIGHT = 2, FACE_BOTTOM = 3 };

/** A hint line: glyph + word pairs, left to right from x. Returns the end x. */
inline float hints(Ctx& c, float x, float cy, const std::vector<std::pair<int, std::string>>& hs, Rgba col = th::dim) {
    for (auto& h : hs) {
        if (h.first >= 0) { padFace(c.g, x + 9, cy, 20, h.first, col); x += 24; }
        x += c.tc.draw(c.g, c.f.small, h.second, x, cy - c.f.small.lineHeight() / 2.f, col) + 14;
    }
    return x;
}

}  // namespace hh
