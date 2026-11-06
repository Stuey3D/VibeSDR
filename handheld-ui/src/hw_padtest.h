// hw_padtest.h — R36S HARDWARE → CONTROLLER → TEST BUTTONS (handheldPadTest.ts): each button and stick as the receiver
// sees it, live — clones are wired differently, and the image has no SSH. Drawn inline under the row (the menu stays
// usable: the D-pad still moves the highlight, B still folds the section — they light up here as they do).
#pragma once
#include <SDL2/SDL.h>

#include <string>

namespace hh {

/** What is held now, by the receiver's own reading of the pad (SDL's mapping, else the raw joystick). */
struct HwPadNow {
    std::string name = "no controller";
    bool mapped = false;
    bool up = false, down = false, left = false, right = false;
    bool fTop = false, fLeft = false, fRight = false, fBottom = false;
    bool l1 = false, r1 = false, l2 = false, r2 = false, sel = false, start = false, l3 = false, r3 = false;
    double lx = 0, ly = 0, rx = 0, ry = 0;
    std::string raw;   // raw buttons held (an unmapped pad: its own numbers)
};

inline HwPadNow hwReadPad(SDL_GameController* pad, SDL_Joystick* joy) {
    HwPadNow p;
    if (pad) {
        p.mapped = true;
        const char* n = SDL_GameControllerName(pad);
        p.name = n ? n : "controller";
        auto b = [&](SDL_GameControllerButton k) { return SDL_GameControllerGetButton(pad, k) != 0; };
        p.up = b(SDL_CONTROLLER_BUTTON_DPAD_UP); p.down = b(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        p.left = b(SDL_CONTROLLER_BUTTON_DPAD_LEFT); p.right = b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        p.fBottom = b(SDL_CONTROLLER_BUTTON_A); p.fRight = b(SDL_CONTROLLER_BUTTON_B);
        p.fLeft = b(SDL_CONTROLLER_BUTTON_X); p.fTop = b(SDL_CONTROLLER_BUTTON_Y);
        p.l1 = b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER); p.r1 = b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        p.sel = b(SDL_CONTROLLER_BUTTON_BACK); p.start = b(SDL_CONTROLLER_BUTTON_START);
        p.l3 = b(SDL_CONTROLLER_BUTTON_LEFTSTICK); p.r3 = b(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        p.l2 = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
        p.r2 = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
        p.lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0;
        p.ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0;
        p.rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0;
        p.ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0;
        if (SDL_Joystick* j = SDL_GameControllerGetJoystick(pad))
            for (int i = 0; i < SDL_JoystickNumButtons(j); i++) if (SDL_JoystickGetButton(j, i)) p.raw += (p.raw.empty() ? "" : " ") + std::to_string(i);
    } else if (joy) {
        const char* n = SDL_JoystickName(joy);
        p.name = std::string(n ? n : "joystick") + " (no mapping)";
        // ★ the same evdev order the loop reads (app_impl.h fromJoystick)
        auto b = [&](int i) { return i < SDL_JoystickNumButtons(joy) && SDL_JoystickGetButton(joy, i); };
        p.fBottom = b(0); p.fRight = b(1); p.fTop = b(2); p.fLeft = b(3);
        p.l1 = b(4); p.r1 = b(5); p.l2 = b(6); p.r2 = b(7); p.sel = b(8); p.start = b(9);
        p.l3 = b(11); p.r3 = b(12);
        p.up = b(13); p.down = b(14); p.left = b(15); p.right = b(16);
        if (SDL_JoystickNumHats(joy) > 0) {
            const Uint8 h = SDL_JoystickGetHat(joy, 0);
            p.up |= (h & SDL_HAT_UP) != 0; p.down |= (h & SDL_HAT_DOWN) != 0; p.left |= (h & SDL_HAT_LEFT) != 0; p.right |= (h & SDL_HAT_RIGHT) != 0;
        }
        if (SDL_JoystickNumAxes(joy) >= 4) {
            p.lx = SDL_JoystickGetAxis(joy, 0) / 32767.0; p.ly = SDL_JoystickGetAxis(joy, 1) / 32767.0;
            p.rx = SDL_JoystickGetAxis(joy, 2) / 32767.0; p.ry = SDL_JoystickGetAxis(joy, 3) / 32767.0;
        }
        for (int i = 0; i < SDL_JoystickNumButtons(joy); i++) if (SDL_JoystickGetButton(joy, i)) p.raw += (p.raw.empty() ? "" : " ") + std::to_string(i);
    }
    return p;
}

/** The figure: shoulders on top, the D-pad left, the face diamond right, SELECT / START between, the two sticks below.
 *  Returns the height used. */
inline float hwDrawPadTest(Ctx& c, Rect a, float y, SDL_GameController* pad, SDL_Joystick* joy) {
    const HwPadNow p = hwReadPad(pad, joy);
    const float h = 236;
    c.g.rrect(a.x + 6, y, a.w - 12, h, 10, rgb(0, 0.45f));
    c.g.rframe(a.x + 6, y, a.w - 12, h, 10, 1, th::btnBorder);
    c.tc.draw(c.g, c.f.small, p.name, a.x + 18, y + 8, th::dim);
    auto key = [&](float x, float yy, float w, float hh, bool on, const std::string& t) {
        c.g.rrect(x, yy, w, hh, 7, on ? th::onBg : th::btnBg);
        c.g.rframe(x, yy, w, hh, 7, 1, on ? th::onBg : th::btnBorder);
        textIn(c, c.f.small, t, {x, yy, w, hh}, on ? th::onText : th::btnText);
    };
    const float cx = a.x + a.w / 2, top = y + 34;
    // shoulders
    key(a.x + 22, top, 70, 28, p.l2, "L2"); key(a.x + 98, top, 70, 28, p.l1, "L1");
    key(a.r() - 168, top, 70, 28, p.r1, "R1"); key(a.r() - 92, top, 70, 28, p.r2, "R2");
    // D-pad
    const float dx = a.x + 92, dy = top + 92, k = 34;
    key(dx - k / 2, dy - k * 1.5f, k, k, p.up, "");
    key(dx - k / 2, dy + k * 0.5f, k, k, p.down, "");
    key(dx - k * 1.5f, dy - k / 2, k, k, p.left, "");
    key(dx + k * 0.5f, dy - k / 2, k, k, p.right, "");
    // face diamond (by POSITION: the printing differs between clones)
    const float fx = a.r() - 92, fy = dy, r = 16;
    auto face = [&](float x, float yy, bool on) { c.g.circle(x, yy, r, on ? 0.f : 1.5f, on ? th::onBg : th::btnText); };
    face(fx, fy - 34, p.fTop); face(fx - 34, fy, p.fLeft); face(fx + 34, fy, p.fRight); face(fx, fy + 34, p.fBottom);
    // SELECT / START
    key(cx - 96, top + 50, 88, 28, p.sel, "SELECT"); key(cx + 8, top + 50, 88, 28, p.start, "START");
    // sticks
    auto stick = [&](float x, float yy, double sx, double sy, bool click, const char* t) {
        c.g.circle(x, yy, 30, 1.5f, click ? th::onBg : th::btnBorder);
        c.g.circle(x + (float)sx * 24, yy + (float)sy * 24, 9, 0, click ? th::onBg : th::amber);
        c.tc.draw(c.g, c.f.small, t, x, yy + 34, th::dim, 1);
    };
    stick(cx - 60, top + 130, p.lx, p.ly, p.l3, "LEFT");
    stick(cx + 60, top + 130, p.rx, p.ry, p.r3, "RIGHT");
    c.tc.draw(c.g, c.f.small, "held: " + (p.raw.empty() ? std::string("—") : p.raw), a.r() - 18, y + 8, th::dim, 2);
    return y + h + 10;
}

}  // namespace hh
