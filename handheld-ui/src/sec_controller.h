// sec_controller.h — MENU → GAME CONTROLLER (inventory §4b; index.html #btnPadGuide) (one of the menu's parts; see app.h
// for the App's state and helpers, ui.h for the widgets).
//
// GUIDE: CONTROLLER GUIDE opens the guide inline — what every button does in THIS UI (app_impl.h press / pollInput /
// kbInput). ★★ If a button moves there, fix this table (AGENTS.md: "when a control moves, fix the copy that says where
// it is").
// Left out: TUCK AWAY TUNE AND ZOOM KEYS — the native UI has no ‹ › / − + keys to tuck away. The web's guide panel
// (two tabs, the SELECT shortcut) is the web's; here SELECT opens the menu.
#pragma once
// (included by app.h after App is declared)

namespace hh {

inline float App::secController(Ctx& c, const RadioState& s, Rect a, float y) {
    (void)s;
    const float y0 = y;
    y = menuRow(c, a, y, "GUIDE", "A game controller or a handheld drives the receiver: the D-pad moves a highlight, and the guide shows "
                                  "what every button does. Pressing a text box opens an on-screen keyboard for the controller.");
    double& on = num_["pad.guide"];
    {
        BtnStyle st; st.font = &c.f.body; st.radius = 7; st.on = on != 0;
        if (hwRowButton(c, "pad.guide", {a.x, y0, a.w, 44}, {a.r() - 226, y0 + 3, 220, 38}, on != 0 ? "HIDE GUIDE" : "CONTROLLER GUIDE", st)) on = on != 0 ? 0 : 1;
    }
    if (on == 0) return y;
    struct G { int face; const char* key; const char* what; };
    // face: −1 = a named key; 0..3 = a face button by position (top, left, right, bottom)
    const G receiver[] = {
        {-1, "D-pad", "move the highlight · left / right adjust a slider"},
        {FACE_BOTTOM, "", "press the highlighted control"},
        {FACE_RIGHT, "", "back · fold a menu section · close"},
        {-1, "L1 · R1", "zoom out · zoom in"},
        {-1, "L2 · R2", "tune down · tune up, one step"},
        {-1, "Left stick", "turn it like a dial to tune — faster turns, faster tuning"},
        {-1, "Right stick", "turn it like a dial to zoom"},
        {-1, "SELECT", "the menu (again to close it)"},
    };
    const G keyboard[] = {
        {FACE_BOTTOM, "", "type the highlighted key"},
        {FACE_LEFT, "", "delete"},
        {FACE_TOP, "", "space"},
        {FACE_RIGHT, "", "back a layer, then close"},
        {-1, "L1 · R1", "shift · symbols (on the number pad: the unit)"},
        {-1, "Left stick", "turned: move the caret"},
        {-1, "START", "enter"},
    };
    auto table = [&](const char* head, const G* g, int n) {
        y = dispHead(c, a, y, head);
        for (int i = 0; i < n; i++) {
            const float cy = y + c.f.body.lineHeight() / 2.f;
            // ★ each line is a stop for the highlight (pressing does nothing) — the menu scrolls to the highlight, so
            //   without these the guide below the button could never be scrolled into view
            const float lines = (float)c.tc.wrap(c.f.body, g[i].what, (int)a.w - 150).size();
            const Nav::Hit hl = c.nav.item(std::string("pad.g.") + head + std::to_string(i), {a.x, y - 2, a.w, lines * c.f.body.lineHeight() + 4});
            if (hl.focused) c.g.rframe(a.x + 2, y - 3, a.w - 4, lines * c.f.body.lineHeight() + 6, 6, 2, th::focus);
            if (g[i].face >= 0) padFace(c.g, a.x + 26, cy, 24, g[i].face, th::btnText);
            else c.tc.draw(c.g, c.f.bodyB, g[i].key, a.x + 6, y, th::btnText);
            for (auto& l : c.tc.wrap(c.f.body, g[i].what, (int)a.w - 150)) { c.tc.draw(c.g, c.f.body, l, a.x + 140, y, th::white68); y += c.f.body.lineHeight(); }
            y += 6;
        }
    };
    table("THE RECEIVER", receiver, (int)(sizeof receiver / sizeof receiver[0]));
    table("THE KEYBOARD", keyboard, (int)(sizeof keyboard / sizeof keyboard[0]));
    for (auto& l : c.tc.wrap(c.f.small, "The face buttons go by POSITION, whatever is printed on them. A USB keyboard works too: arrows move, "
                                        "Enter presses, Esc goes back, < > tune, − + zoom, Tab opens the menu.", (int)a.w - 12)) {
        c.tc.draw(c.g, c.f.small, l, a.x + 6, y, th::dim); y += c.f.small.lineHeight();
    }
    return y + 8;
}

}  // namespace hh
