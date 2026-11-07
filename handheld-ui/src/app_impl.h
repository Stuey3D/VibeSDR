// app_impl.h — App's bodies (included once, from app.h). Layout in pixels on the 640×480 panel.
#pragma once

namespace hh {

// ── layout (the web handheld page at 640×480, a little larger: no tune / zoom row) ─────────────────────────────────────
namespace lay {
constexpr float bandsH = 18;                        // the band plan bar (the web's #bands, 16 px)
constexpr float scaleY = 18, scaleH = 22;           // the frequency scale
constexpr float specY = 40, specH = 112;            // the spectrum
constexpr float wfY = specY + specH;                // the waterfall runs to the foot, behind the island
constexpr float islandX = 8, islandW = 624, islandY = 314, islandH = 160;
constexpr float pillX = 18, pillY = 324, pillW = 604, pillH = 58;
constexpr float btnY = 392, btnH = 52;
constexpr float statusY = 448, statusH = 22;
}  // namespace lay

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// actions
// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

inline void App::tuneTo(double f) {
    const RadioState& s = eng_->state();
    if (s.session != "running") return;
    if (f < 1000) f = 1000;
    eng_->tune(f);
    // ★★★ THE VIEW FOLLOWS THE DIAL (VFO CENTRE: LOCKED, the web's default — spectrum.ts tune(): `if (this.followVfo)
    //     this.zoom(this.frequency, binBandwidth)`). Without it a long tune walked the dial off the side of the view and
    //     the waterfall stood still until a zoom recentred it (Stuart, 2026-11-06, HF → FM on the jog wheel).
    if (set_.followVfo && s.spanHz > 0) {
        viewPending_ = true; viewF_ = f; viewSpan_ = s.spanHz;
        wfp_.setView(f, s.spanHz);
    } else {
        wfp_.setView(s.viewCentreHz, s.spanHz);
    }
    wfp_.notifyInteraction(nowSec());
}

inline void App::tuneSteps(int n) {
    if (dabTune(*this, n)) return;   // in DAB the arrows step the block (L2/R2, the jog, the hold-sweep)
    const RadioState& s = eng_->state();
    if (s.session != "running" || n == 0) return;
    const int step = stepOnLadder(s.vfoHz, set_.step);
    const double want = stepFrom(s.vfoHz, step, n);
    const double got = clampTune(want);
    if (std::fabs(got - s.vfoHz) < 0.5) return;            // parked on the edge already: the message said why
    tuneTo(got);
}

inline void App::sampleCpu() {
    const double t = nowSec();
    if (t - cpuAt_ < 1.0) return;
    cpuAt_ = t;
    FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return;
    unsigned long long v[10] = {0};
    const int n = std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    std::fclose(f);
    if (n < 4) return;
    unsigned long long total = 0;
    for (int i = 0; i < 8 && i < n; i++) total += v[i];
    const unsigned long long idle = v[3] + (n > 4 ? v[4] : 0), busy = total - idle;
    if (cpuTotal_ && total > cpuTotal_) cpuPct_ = (int)std::lround(100.0 * (double)(busy - cpuBusy_) / (double)(total - cpuTotal_));
    cpuBusy_ = busy; cpuTotal_ = total;
}

inline void App::stickSweep() {
    const double t = nowSec();
    const bool inKb = !sheets_.empty() && sheets_.back() == S_KB;
    const double mag = std::hypot(lx_, ly_);
    const int dir = (!screenOff_ && !inKb && mag > 0.8 && std::fabs(ly_) < 0.45 * mag) ? (lx_ > 0 ? 1 : -1) : 0;
    const double ang = std::atan2(ly_, lx_) * 180.0 / M_PI;
    if (dir == 0 || (sweepDir_ != 0 && dir != sweepDir_)) { sweepDir_ = 0; sweepStart_ = 0; sweepAwaiting_ = false; return; }
    if (sweepDir_ == 0) { sweepDir_ = dir; sweepZoneAt_ = t; sweepZoneAng_ = ang; sweepStart_ = 0; return; }
    double dA = std::fabs(ang - sweepZoneAng_); if (dA > 180) dA = 360 - dA;
    if (sweepStart_ == 0) {
        // ★ a spin passing through left / right is not a hold: the angle must stay put for the hold time
        if (dA > 20) { sweepZoneAt_ = t; sweepZoneAng_ = ang; return; }
        if (t - sweepZoneAt_ < 0.35) return;          // HOLD_MS
        sweepStart_ = t; sweepNext_ = t + 1.0 / 3;    // the first step after 1 / SWEEP_LO
        return;
    }
    if (t < sweepNext_) return;
    const RadioState& s = eng_->state();
    // wait for the receiver's answer to the last step (a config per tune), but never more than 700 ms
    if (sweepAwaiting_ && s.configSeq == sweepSeq_ && t - sweepEchoAt_ < 0.7) return;
    sweepAwaiting_ = true; sweepEchoAt_ = t; sweepSeq_ = s.configSeq;
    lastInput_ = t;
    tuneSteps(sweepDir_);
    const double k = std::min(1.0, (t - sweepStart_) / 3.5);
    sweepNext_ = t + 1.0 / (3 + (15 - 3) * k);
}

inline double App::clampTune(double hz) {
    const RadioState& s = eng_->state();
    const double want = std::round(hz);
    std::vector<std::pair<double, double>> ranges = s.ranges;
    if (ranges.empty()) { gapNudgeDir_ = 0; return std::max(10000.0, std::min(1.8e9, want)); }
    for (auto& r : ranges) if (want >= r.first && want <= r.second) { gapNudgeDir_ = 0; return want; }
    // in a gap: which way we were going, and the edges either side
    const double cur = s.vfoHz > 0 ? s.vfoHz : want;
    const int dir = want >= cur ? 1 : -1;
    double below = -1e300, above = 1e300;
    for (auto& r : ranges) {
        if (r.second < want && r.second > below) below = r.second;
        if (r.first > want && r.first < above) above = r.first;
    }
    char b[200];
    if (gapNudgeDir_ == dir) {
        gapNudgeDir_ = 0;
        const double target = dir > 0 ? above : below;
        if (std::fabs(target) < 1e299) { std::snprintf(b, sizeof b, "Jumped to %.3f MHz", target / 1e6); toast(b); return target; }
    }
    gapNudgeDir_ = dir;
    const double edge = dir > 0 ? below : above;
    if (std::fabs(edge) > 1e299) return ranges.front().first > want ? ranges.front().first : ranges.back().second;
    const double other = dir > 0 ? above : below;
    // ★ the owner's limit (radio.allowed) or the hardware's own coverage (radio.ranges)?
    bool hwReach = false;
    if (s.hw("allowed").kind == Json::Arr) for (auto& r : s.hw("ranges").a) if (r.size() == 2 && want >= r[0].num() && want <= r[1].num()) hwReach = true;
    const char* why = hwReach ? "the server owner does not allow tuning here" : "this radio cannot receive here";
    if (std::fabs(other) < 1e299)
        std::snprintf(b, sizeof b, "%.3f MHz — %s. Tune %s again to jump to %.3f MHz", edge / 1e6, why, dir > 0 ? "up" : "down", other / 1e6);
    else std::snprintf(b, sizeof b, "%.3f MHz — %s", edge / 1e6, why);
    toastLong(b);
    return edge;
}

inline void App::zoomSteps(int n) {
    if (dabZoom(*this, n)) return;   // zoom is fixed in DAB
    const RadioState& s = eng_->state();
    if (s.session != "running" || n == 0 || s.spanHz <= 0) return;
    // ★ ×√2 a press on VibeServer (the app's zoom keys, Stuart 2026-10-28)
    double span = s.spanHz * std::pow(std::sqrt(0.5), n);
    const double minSpan = 3000;
    if (span > s.maxSpanHz * 0.999 && s.maxSpanHz > 0) { eng_->zoomReset(); wfp_.notifyInteraction(nowSec()); return; }
    if (span < minSpan) span = minSpan;
    const double centre = set_.followVfo ? s.vfoHz : s.viewCentreHz;
    eng_->zoom(centre, span, !set_.followVfo);
    viewPending_ = false;
    wfp_.setView(centre, span);
    wfp_.notifyInteraction(nowSec());
}

inline void App::updateSignal(const uint8_t* bins, int n, double centreHz, double spanHz) {
    // ★ web/client/src/main.ts updateSignal: the passband's peak against the 25th-percentile floor, or the server's own
    //   channel reading when it sends one; the same smoothing (0.30 up, 0.12 down) and the same −121…−13 dBFS scale.
    const RadioState& s = eng_->state();
    double sigDb = -160, noiseDb = -120;
    if (s.chanDb > -199) { sigDb = s.chanDb; noiseDb = s.floorDb; }
    else if (n > 0 && spanHz > 0) {
        const double hzPerBin = spanHz / n, lo = centreHz - spanHz / 2;
        const int b0 = std::max(0, (int)std::floor((s.vfoHz + s.bwLow - lo) / hzPerBin));
        const int b1 = std::min(n - 1, (int)std::ceil((s.vfoHz + s.bwHigh - lo) / hzPerBin));
        for (int i = b0; i <= b1; i++) if (bins[i]) sigDb = std::max(sigDb, (double)bins[i] - 256);
        std::vector<double> sample;
        for (int i = 0; i < n; i += 8) if (bins[i]) sample.push_back((double)bins[i] - 256);
        if (!sample.empty()) { std::sort(sample.begin(), sample.end()); noiseDb = sample[(size_t)(sample.size() * 0.25)]; }
    }
    const double snr = std::max(0.0, sigDb - noiseDb);
    snrSmooth_ += (snr - snrSmooth_) * (snr > snrSmooth_ ? 0.30 : 0.12);
    lastSigDb_ = sigDb;
    const double norm = std::max(0.0, std::min(1.0, (sigDb + 121) / (121 - 13)));
    sigSmooth_ += (norm - sigSmooth_) * (norm > sigSmooth_ ? 0.30 : 0.12);
}

inline void App::press(Btn b) {
    lastInput_ = nowSec();
    if (screenOff_) { screenOff_ = false; eng_->device("{\"action\":\"screen-on\"}"); return; }
    if (!sheets_.empty() && sheets_.back() == S_KB) { kbInput(b); return; }   // ★ the keyboard takes every button while it is up
    const bool inFreq = !sheets_.empty() && sheets_.back() == S_KB;
    switch (b) {
        // ★ the dial and the zoom work under every sheet but the keypad (the web page keeps them live too)
        case B_L1: if (!inFreq) zoomSteps(-1); return;
        case B_R1: if (!inFreq) zoomSteps(1); return;
        case B_L2: if (!inFreq) tuneSteps(-1); return;
        case B_R2: if (!inFreq) tuneSteps(1); return;
        case B_SELECT:
            if (!sheets_.empty() && sheets_.back() == S_MENU) closeTop();
            else { sheets_.clear(); menuOpen_.clear(); menuScroll_ = {}; open(S_MENU); }
            return;
        default: break;
    }
    presses_.push_back(b);
}

inline void App::pollInput() {
    SDL_Event e;
    auto fromController = [](int b) -> Btn {
        switch (b) {
            case SDL_CONTROLLER_BUTTON_A: return B_ENTER;          // ★ SDL's A is the BOTTOM position
            case SDL_CONTROLLER_BUTTON_B: return B_BACK;           //   …and B the RIGHT one, whatever the printing
            case SDL_CONTROLLER_BUTTON_Y: return B_TOPFACE;
            case SDL_CONTROLLER_BUTTON_X: return B_LEFTFACE;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return B_UP;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return B_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return B_LEFT;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return B_RIGHT;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return B_L1;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return B_R1;
            case SDL_CONTROLLER_BUTTON_BACK: return B_SELECT;
            case SDL_CONTROLLER_BUTTON_START: return B_START;
            default: return B_NONE;
        }
    };
    // ★ a pad SDL has no mapping for: the R36S joypad's evdev order (BTN_SOUTH 0x130 … THUMBR 0x13e, then the D-pad)
    auto fromJoystick = [](int b) -> Btn {
        switch (b) {
            case 0: return B_ENTER;  case 1: return B_BACK;  case 2: return B_TOPFACE; case 3: return B_LEFTFACE;
            case 4: return B_L1;     case 5: return B_R1;    case 6: return B_L2;      case 7: return B_R2;
            case 8: return B_SELECT; case 9: return B_START;
            case 13: return B_UP;    case 14: return B_DOWN; case 15: return B_LEFT;   case 16: return B_RIGHT;
            default: return B_NONE;
        }
    };
    auto openPads = [&] {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) { if (!pad_) { pad_ = SDL_GameControllerOpen(i); if (pad_) std::fprintf(stderr, "vibe-hhui: pad %s\n", SDL_GameControllerName(pad_)); } }
            else if (!joy_) { joy_ = SDL_JoystickOpen(i); if (joy_) std::fprintf(stderr, "vibe-hhui: joystick %s (no mapping)\n", SDL_JoystickName(joy_)); }
        }
    };
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { presses_.clear(); quit_ = true; }
        else if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_JOYDEVICEADDED) openPads();
        else if (e.type == SDL_CONTROLLERBUTTONDOWN) { const Btn b = fromController(e.cbutton.button); if (b != B_NONE) press(b); }
        else if (e.type == SDL_JOYBUTTONDOWN && joy_ && !pad_) { const Btn b = fromJoystick(e.jbutton.button); if (b != B_NONE) press(b); }
        else if (e.type == SDL_JOYHATMOTION && joy_ && !pad_) {
            if (e.jhat.value & SDL_HAT_UP) press(B_UP);
            if (e.jhat.value & SDL_HAT_DOWN) press(B_DOWN);
            if (e.jhat.value & SDL_HAT_LEFT) press(B_LEFT);
            if (e.jhat.value & SDL_HAT_RIGHT) press(B_RIGHT);
        } else if (e.type == SDL_CONTROLLERAXISMOTION && pad_) {
            // the analogue triggers as buttons (L2 / R2)
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) { const bool d = e.caxis.value > 16000; if (d && !l2Down_) press(B_L2); l2Down_ = d; }
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) { const bool d = e.caxis.value > 16000; if (d && !r2Down_) press(B_R2); r2Down_ = d; }
        } else if (e.type == SDL_KEYDOWN) {
            // ★ a USB keyboard: the ONE keyboard scheme (memory gamepad_mapping 10-18): arrows + WASD move, Enter select,
            //   Backspace AND Escape back, < > (, .) tune, - + (=) zoom; Tab = the menu
            switch (e.key.keysym.sym) {
                case SDLK_UP: case SDLK_w: press(B_UP); break;
                case SDLK_DOWN: case SDLK_s: press(B_DOWN); break;
                case SDLK_LEFT: case SDLK_a: press(B_LEFT); break;
                case SDLK_RIGHT: case SDLK_d: press(B_RIGHT); break;
                case SDLK_RETURN: press(B_ENTER); break;
                case SDLK_BACKSPACE: case SDLK_ESCAPE: press(B_BACK); break;
                case SDLK_COMMA: press(B_L2); break; case SDLK_PERIOD: press(B_R2); break;
                case SDLK_MINUS: press(B_L1); break; case SDLK_EQUALS: case SDLK_PLUS: press(B_R1); break;
                case SDLK_TAB: press(B_SELECT); break;
                default: break;
            }
        }
    }
    if (!pad_ && !joy_) openPads();
    // the sticks: jog wheels (sampled every loop, as the web page samples its pad each frame)
    const double tMs = nowSec() * 1000.0;
    double lx = 0, ly = 0, rx = 0, ry = 0;
    if (pad_) {
        lx = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0;
        ly = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0;
        rx = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0;
        ry = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0;
    } else if (joy_ && SDL_JoystickNumAxes(joy_) >= 4) {
        lx = SDL_JoystickGetAxis(joy_, 0) / 32767.0; ly = SDL_JoystickGetAxis(joy_, 1) / 32767.0;
        rx = SDL_JoystickGetAxis(joy_, 2) / 32767.0; ry = SDL_JoystickGetAxis(joy_, 3) / 32767.0;
    }
    if (testStick_ != 0) { lx = testStick_; ly = 0; }   // ★ tests: SCRIPT STICKL / STICKR / STICK0 hold the left stick
    lx_ = lx; ly_ = ly;
    const bool inFreq = !sheets_.empty() && sheets_.back() == S_KB;
    if (!screenOff_ && inFreq) {
        // ★ the keyboard: the left stick turned moves the caret (padKeyboard.ts)
        const int t = tuneJog_.feed(lx, ly, tMs);
        for (int i = 0; i < std::abs(t); i++) kbd_.edit(t > 0 ? kb::EditAct::right : kb::EditAct::left);
        zoomJog_.feed(rx, ry, tMs);
    } else if (!screenOff_) {
        const int t = tuneJog_.feed(lx, ly, tMs), z = zoomJog_.feed(rx, ry, tMs);
        if (t || z) lastInput_ = nowSec();
        tuneSteps(t);
        zoomSteps(z);
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// drawing — the main screen
// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

/** The frequency scale: ~5 labels across, on a 1-2-5 ladder, "9.5M" like the web's scale. */
inline void App::drawScale(Ctx& c, const RadioState& s, Rect r) {
    c.g.rect(r.x, r.y, r.w, r.h, th::bg);
    if (s.spanHz <= 0) return;
    const double lo = s.viewCentreHz - s.spanHz / 2, hi = s.viewCentreHz + s.spanHz / 2;
    const double raw = s.spanHz / 5;
    const double p10 = std::pow(10.0, std::floor(std::log10(raw)));
    double step = p10;
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) if (p10 * m >= raw) { step = p10 * m; break; }
    int dec = 0;
    for (double u = step / 1e6; u < 0.999 && dec < 6; u *= 10) dec++;
    for (double f = std::ceil(lo / step) * step; f <= hi; f += step) {
        const float x = r.x + (float)((f - lo) / (hi - lo)) * r.w;
        c.g.rect(x - 0.5f, r.b() - 5, 1, 5, th::dim);
        char b[32];
        std::snprintf(b, sizeof b, "%.*fM", std::max(1, dec), f / 1e6);
        const int tw = c.tc.width(c.f.small, b);
        const float tx = std::max(r.x + 2 + tw / 2.f, std::min(r.r() - 2 - tw / 2.f, x));
        c.tc.draw(c.g, c.f.small, b, tx, r.y + 2, rgb(0xffb43c, 0.9f), 1);
    }
}

/** The band plan bar (web main.ts drawBands, the app's BAND_COLS): widest first so a narrower band sits on top; the
 *  full name when it fits, else the short label, clipped to its own segment. Region-scoped. */
inline void App::drawBands(Ctx& c, const RadioState& s, Rect r) {
    c.g.rect(r.x, r.y, r.w, r.h, rgb(0));
    if (s.spanHz <= 0) return;
    static std::vector<int> order;
    if (order.empty()) {
        for (int i = 0; i < kBandPlanN; i++) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [](int a, int b) { return (kBandPlan[a].hi - kBandPlan[a].lo) > (kBandPlan[b].hi - kBandPlan[b].lo); });
    }
    const double lo = s.viewCentreHz - s.spanHz / 2, hi = lo + s.spanHz;
    const int reg = region();
    for (int i : order) {
        const BandDef& b = kBandPlan[i];
        if (b.hi < lo || b.lo > hi) continue;
        if (b.regions && !(b.regions & (1 << reg))) continue;
        const float x0 = std::max(r.x, r.x + (float)((b.lo - lo) / s.spanHz) * r.w), x1 = std::min(r.r(), r.x + (float)((b.hi - lo) / s.spanHz) * r.w);
        if (x1 - x0 < 1) continue;
        const bool cb = std::strstr(b.name, "CB") != nullptr;
        const Rgba col = cb ? rgb(0xff7700, 0.92f) : b.type == 0 ? rgb(0xcf0000, 0.92f) : b.type == 1 ? rgb(0x0900ff, 0.92f) : rgb(0x07bd00, 0.92f);
        c.g.rect(x0, r.y, x1 - x0, r.h, col);
        const std::string full = b.name, shortL = *b.label ? b.label : b.name;
        const int wf = c.tc.width(c.f.tiny, full), ws = c.tc.width(c.f.tiny, shortL);
        std::string label; int tw = 0;
        if (x1 - x0 > wf + 8) { label = full; tw = wf; } else if (x1 - x0 > ws + 8) { label = shortL; tw = ws; }
        if (!label.empty()) {
            c.g.clip(x0, r.y, x1 - x0, r.h);
            const float ty = r.cy() - c.f.tiny.lineHeight() / 2.f;
            c.tc.draw(c.g, c.f.tiny, label, (x0 + x1) / 2 - tw / 2.f + 1, ty + 1, rgb(0, 0.55f));
            c.tc.draw(c.g, c.f.tiny, label, (x0 + x1) / 2 - tw / 2.f, ty, rgb(0xffffff, 0.95f));
            c.g.unclip();
        }
    }
}

inline void App::drawOverlays(Ctx& c, const RadioState& s, Rect spec, Rect wfr) {
    if (s.spanHz <= 0) return;
    const double lo = s.viewCentreHz - s.spanHz / 2, hi = s.viewCentreHz + s.spanHz / 2;
    auto xOf = [&](double hz) { return spec.x + (float)((hz - lo) / (hi - lo)) * spec.w; };
    // the dB grid + labels (web drawDbScale: 5 stops, faint lines, amber labels with a shadow)
    const double dbMax = wfp_.dbMax(), dbMin = wfp_.dbMin();
    for (int i = 0; i < 5 && spec.h >= 60; i++) {   // (no dB grid on the DAB page's strip)
        const float t = i / 4.f, y = spec.y + t * spec.h;
        c.g.rect(spec.x, y, spec.w, 1, rgb(0xffb43c, 0.10f));
        char b[16];
        std::snprintf(b, sizeof b, "%.0f", dbMax - t * (dbMax - dbMin));
        const float ly = std::max(spec.y + 1.f, std::min(spec.b() - c.f.small.lineHeight() - 1.f, y - c.f.small.lineHeight() / 2.f));
        c.tc.draw(c.g, c.f.small, b, spec.x + 5, ly + 1, rgb(0, 0.8f));
        c.tc.draw(c.g, c.f.small, b, spec.x + 4, ly, rgb(0xffb43c, 0.9f));
    }
    // the passband and the dial
    const float px0 = xOf(s.vfoHz + s.bwLow), px1 = xOf(s.vfoHz + s.bwHigh);
    c.g.rect(px0, spec.y, std::max(1.f, px1 - px0), spec.h, rgb(0xffb000, 0.13f));
    c.g.rect(px0, wfr.y, std::max(1.f, px1 - px0), wfr.h, rgb(0xffb000, 0.06f));
    const float vx = xOf(s.vfoHz);
    c.g.rect(vx - 1, spec.y, 2, spec.h, dispNeedle(0.9f));
    c.g.rect(vx - 0.5f, wfr.y, 1, wfr.h, dispNeedle(0.45f));
    // the radio's name (top right, as the web page) and the station's RDS name (top left)
    const std::string name = s.radioName.empty() ? "VibeSDR" : s.radioName;
    c.tc.draw(c.g, c.f.title, name, spec.r() - 10, spec.y + 6, rgb(0, 0.8f), 2);
    c.tc.draw(c.g, c.f.title, name, spec.r() - 11, spec.y + 5, th::amber, 2);
    // (the station's RDS name is the VTS bar's now — vts.h)
}

/** The portrait control island: the pill (signal gradient · frequency · mode · reading), the four buttons, the status line. */
inline void App::drawDeck(Ctx& c, const RadioState& s, Rect r) {
    c.g.rrect(r.x, r.y, r.w, r.h, 14, rgb(0x0e0700, 0.90f));
    c.g.rframe(r.x, r.y, r.w, r.h, 14, 1, th::barBorder);

    // ── the pill ──
    const Rect pill{lay::pillX, lay::pillY, lay::pillW, lay::pillH};
    if (!dabDrawPill(*this, c, s, pill)) {   // ★ in DAB the pill is the DAB reception meter (dab_box.h)
    c.g.rrect(pill.x, pill.y, pill.w, pill.h, 9, th::pillBg);
    {
        // ★ the meter IS the pill's background (#mSig): red → amber → green, its width the smoothed level
        const float w = (float)std::max(0.0, std::min(1.0, sigSmooth_)) * (pill.w - 2);
        if (w > 1) {
            const Rgba cols[3] = {rgb(0xe8462a), rgb(0xe8a02a), rgb(0x35e08a)};
            const float pos[3] = {0, 0.38f, 1};
            c.g.clip(pill.x + 1, pill.y + 1, w, pill.h - 2);
            c.g.hgrad(pill.x + 1, pill.y + 1, pill.w - 2, pill.h - 2, cols, pos, 3);
            c.g.unclip();
        }
    }
    c.g.rframe(pill.x, pill.y, pill.w, pill.h, 9, 1, th::btnBorder);
    // the squelch threshold on the meter's own scale (#mSqlLine: amber, below the readouts)
    {
        const double sq = num_.count("audio.sqlDb") ? num_["audio.sqlDb"] : -999;
        if (sq > -998) {
            const float x = pill.x + 1 + (float)std::max(0.0, std::min(1.0, (sq + 121) / 108.0)) * (pill.w - 2);
            c.g.rect(x - 2, pill.y + 1, 4, pill.h - 2, rgb(0, 0.6f));
            c.g.rect(x - 1, pill.y + 1, 2, pill.h - 2, th::amber);
        }
    }

    // the readouts: one centred group — frequency box · mode chip · reading chip
    const bool live = s.session == "running" && s.vfoHz > 0;
    std::string fmain = "—", ffine;
    const char* unitTxt = set_.freqUnit == 0 ? "Hz" : set_.freqUnit == 1 ? "kHz" : "MHz";
    if (live) {
        // web main.ts cardFreqPlain: MHz = 3 decimals + the dimmed 100 Hz / 10 Hz pair; kHz = 3 decimals (1 Hz); Hz = whole
        const long long hz = std::llround(s.vfoHz);
        char b[32];
        if (set_.freqUnit == 0) { std::snprintf(b, sizeof b, "%lld", hz); fmain = b; }
        else if (set_.freqUnit == 1) { std::snprintf(b, sizeof b, "%.3f", hz / 1e3); fmain = b; }
        else {
            std::snprintf(b, sizeof b, "%.3f", std::floor(hz / 1e3) / 1e3);
            fmain = b;
            std::snprintf(b, sizeof b, "%02lld", (std::llabs(hz) % 1000) / 10);
            ffine = b;
        }
    }
    const std::string mode = live ? modeLabel(s.mode) : "—";
    std::string reading;
    if (set_.meterMode == 1) reading = toSUnit(lastSigDb_);
    else if (set_.meterMode == 2) { char b[24]; std::snprintf(b, sizeof b, "%d dBFS", (int)std::lround(lastSigDb_)); reading = b; }
    else { char b[24]; std::snprintf(b, sizeof b, "SNR %d dB", (int)std::lround(snrSmooth_)); reading = b; }
    if (!live) reading = "—";
    // ★ while the squelch gate is shut the chip says SQL (mobile.ts paintSignal)
    const double sqlDb = num_.count("audio.sqlDb") ? num_["audio.sqlDb"] : -999;
    const bool sqlOn = sqlDb > -998;
    if (live && sqlOn && lastSigDb_ < sqlDb) reading = "SQL";
    // widths reserved for the widest reading (the web's 8ch rule: nothing moves as the numbers change)
    const float fMainW = (float)c.tc.widthTab(c.f.freq, "8888.888"), fFineW = (float)c.tc.widthTab(c.f.freqFine, "88");
    const float unitW = (float)c.tc.width(c.f.unit, "MHz");
    const float fMainW2 = set_.freqUnit == 2 ? 0.f : (float)c.tc.widthTab(c.f.freq, "88888888");
    const float boxW = (set_.freqUnit == 2 ? fMainW + 6 + fFineW : fMainW2) + 6 + unitW + 24;
    // ★ + room for the STEREO badge (#mStereo), reserved always so nothing moves when the pilot comes and goes
    const float stW = 30;
    const float modeW = std::max(84.f, (float)c.tc.width(c.f.btn, "ECSS-U") + 24) + stW;
    const float rdW = std::max(118.f, (float)c.tc.width(c.f.small, "SNR 99 dB") + 22);
    const float gap = 10, total = boxW + gap + modeW + gap + rdW;
    float x = pill.cx() - total / 2;
    const float bh = pill.h - 12, by = pill.y + 6;
    // the frequency box (#mFreqBox: 62 % black, a white hairline)
    const Rect fb{x, by, boxW, bh};
    const Nav::Hit hf = c.nav.item("freq", fb);
    c.g.rrect(fb.x, fb.y, fb.w, fb.h, 7, rgb(0, 0.62f));
    c.g.rframe(fb.x, fb.y, fb.w, fb.h, 7, 1, rgb(0xffffff, 0.38f));
    {
        const float base = fb.cy() + c.f.freq.lineHeight() * 0.5f;   // a shared baseline-ish line for the three parts
        const float mx = fb.x + 12;
        if (set_.freqUnit == 2) {
            c.tc.drawTab(c.g, c.f.freq, fmain, mx + fMainW / 2, base - c.f.freq.lineHeight(), th::white, 1);
            c.tc.drawTab(c.g, c.f.freqFine, ffine, mx + fMainW + 6, base - c.f.freqFine.lineHeight() - 3, th::white68);
            c.tc.draw(c.g, c.f.unit, unitTxt, mx + fMainW + 6 + fFineW + 6, base - c.f.unit.lineHeight() - 4, th::white);
        } else {
            c.tc.drawTab(c.g, c.f.freq, fmain, mx + fMainW2 / 2, base - c.f.freq.lineHeight(), th::white, 1);
            c.tc.draw(c.g, c.f.unit, unitTxt, mx + fMainW2 + 6, base - c.f.unit.lineHeight() - 4, th::white);
        }
    }
    if (hf.focused) focusRing(c, fb, 7);
    if (hf.pressed) openFreqEntry();
    x += boxW + gap;
    const Rect mb{x, by, modeW, bh};
    const Nav::Hit hm = c.nav.item("mode", mb);
    c.g.rrect(mb.x, mb.y, mb.w, mb.h, 7, rgb(0, 0.62f));
    c.g.rframe(mb.x, mb.y, mb.w, mb.h, 7, 1, rgb(0xffffff, 0.38f));
    textIn(c, c.f.btn, mode, {mb.x, mb.y, mb.w - stW, mb.h}, th::white);
    // ★★ THE STEREO BADGE (web syncStereoLight: the RDS/pilot `stereo` flag) — two overlapping rings, white like the
    //    mode, drawn only while the pilot is locked; its absence on a weak station is information
    if (live && s.mode == "wfm" && s.rdsStereo) {   // (the web clears it on a mode change)
        const float cx = mb.r() - stW / 2 - 6, cy = mb.cy();
        c.g.circle(cx - 5, cy, 7, 1.6f, th::white);
        c.g.circle(cx + 5, cy, 7, 1.6f, th::white);
    }
    if (hm.focused) focusRing(c, mb, 7);
    if (hm.pressed) open(S_MODE);
    x += modeW + gap;
    const Rect rb{x, pill.cy() - 15, rdW, 30};
    const Nav::Hit hr = c.nav.item("snr", rb);
    c.g.rrect(rb.x, rb.y, rb.w, rb.h, 15, rgb(0, 0.62f));
    c.g.rframe(rb.x, rb.y, rb.w, rb.h, 15, 1, rgb(0xffffff, 0.38f));
    textIn(c, c.f.small, reading, rb, th::white);
    if (hr.focused) focusRing(c, rb, 15);
    if (hr.pressed) { set_.meterMode = (set_.meterMode + 1) % 3; dirty(); }   // ★ the web's meter cycle: SNR → S → dBFS

    }

    // ── the four buttons ──
    const float bw = (pill.w - 3 * 8) / 4;
    auto bx = [&](int i) { return pill.x + i * (bw + 8); };
    BtnStyle st; st.font = &c.f.btn;
    if (!dabDrawStepKey(*this, c, {bx(0), lay::btnY, bw, lay::btnH}, st))
        if (button(c, "step", {bx(0), lay::btnY, bw, lay::btnH}, formatStep(stepOnLadder(s.vfoHz, set_.step)), st)) open(S_STEP);
    if (button(c, "menu", {bx(1), lay::btnY, bw, lay::btnH}, "MENU", st)) { menuOpen_.clear(); menuScroll_ = {}; open(S_MENU); }
    {
        const Rect ab{bx(2), lay::btnY, bw, lay::btnH};
        if (button(c, "audio", ab, "", st)) open(S_AUDIO);
        // the speaker glyph (#mAudio): a filled cone + two arcs; struck through when muted
        const float cx = ab.cx() - 6, cy = ab.cy();
        const Rgba col = th::btnText;
        c.g.rect(cx - 9, cy - 5, 6, 10, col);
        c.g.triangle(cx - 3, cy - 5, cx + 5, cy - 12, cx + 5, cy + 12, col);
        c.g.triangle(cx - 3, cy - 5, cx + 5, cy + 12, cx - 3, cy + 5, col);
        for (int k = 0; k < 2; k++) {
            const float rr = 8 + k * 6;
            for (int i = -3; i < 3; i++) {
                const float a0 = i * 0.25f, a1 = (i + 1) * 0.25f;
                c.g.line(cx + 6 + std::cos(a0) * rr, cy + std::sin(a0) * rr, cx + 6 + std::cos(a1) * rr, cy + std::sin(a1) * rr, 2, col);
            }
        }
        // ★ the web's #mAudio follows the ENGINE speaker's mute (the AUDIO sheet), not the hardware's
        if (num_.count("audio.mute") && num_["audio.mute"] > 0) c.g.line(cx - 12, cy + 12, cx + 22, cy - 12, 2.5f, th::red);
    }
    {
        // ★★ REC, where DEVICE was (Stuart, 2026-11-06: MENU and DEVICE were the same place twice). Press to record to
        //    the card, press again to stop; the time runs on the key itself.
        const Rect rb4{bx(3), lay::btnY, bw, lay::btnH};
        const bool rec = deckRecording(*this);
        BtnStyle rs = st; rs.on = rec; rs.disabled = !rec && !s.connected;
        std::string lbl = "REC";
        if (rec && num_.count("audio.recSec")) { const int sec = (int)num_["audio.recSec"]; char b[24]; std::snprintf(b, sizeof b, "%02d:%02d", sec / 60, sec % 60); lbl = b; }
        if (button(c, "rec", rb4, "", rs)) deckToggleRec(*this, s);
        const float tw = (float)c.tc.width(c.f.btn, lbl);
        const float gx = rb4.cx() - (tw + 22) / 2;
        const float a = rec ? 0.55f + 0.45f * (float)std::sin(c.t * 4.0) : 1.f;
        if (rec) c.g.circle(gx + 7, rb4.cy(), 7, 0, rgb(0xff3030, a));
        else c.g.circle(gx + 7, rb4.cy(), 7, 2, th::btnText);
        c.tc.draw(c.g, c.f.btn, lbl, gx + 22, rb4.cy() - c.f.btn.lineHeight() / 2.f, rec ? th::red : th::btnText);
    }

    // ── the status line: UTC · hints · battery ──
    {
        char clk[32];
        const std::time_t tt = std::time(nullptr);
        std::tm u{}; gmtime_r(&tt, &u);
        std::snprintf(clk, sizeof clk, "%02d:%02d UTC", u.tm_hour, u.tm_min);
        const float ty = lay::statusY + lay::statusH / 2;
        // ★★ THE STATS ROW (Stuart, 2026-11-06: "the status row doesnt need to show the control hint all the time … have
        //    the system stats there: Gn: 43db (A) IF: 1400K (A) CPU: 53% Bat: 65%"). Full words when they fit ("if you can
        //    fit the full words in then do it"), his abbreviations when not; the UTC clock at the right only if there is
        //    room left. (A) = automatic: the radio's AGC / an IF filter that follows the zoom.
        const Json& h = s.msg("hwinfo");
        struct Part { std::string full, abbr; Rgba col = th::dim; };
        std::vector<Part> parts;
        if (s.session == "running") {
            char a1[48], a2[48];
            const long gdb = std::lround(s.gainNow / 10.0);
            std::snprintf(a1, sizeof a1, "Gain %ld dB%s", gdb, s.agc ? " (auto)" : "");
            std::snprintf(a2, sizeof a2, "Gn: %lddB%s", gdb, s.agc ? " (A)" : "");
            parts.push_back({a1, a2});
            const Json& hr = h["radio"].has("tunerBw") || h["radio"].has("tunerBwAuto") ? h["radio"] : h;
            if (hr.has("tunerBw") || hr.has("tunerBwAuto")) {
                const int bwHz = (int)hr["tunerBw"].num(0);
                const bool au = hr["tunerBwAuto"].boolean();
                std::string v = bwHz > 0 ? std::to_string(bwHz / 1000) : "wide";
                parts.push_back({"IF filter " + (bwHz > 0 ? v + " kHz" : v) + (au ? " (auto)" : ""),
                                 "IF: " + (bwHz > 0 ? v + "K" : v) + (au ? " (A)" : "")});
            }
        }
        if (cpuPct_ >= 0) parts.push_back({"CPU " + std::to_string(cpuPct_) + "%", "CPU: " + std::to_string(cpuPct_) + "%"});
        if (battery_ >= 0) {
            Part p{"Battery " + std::to_string(battery_) + "%" + (charging_ ? " +" : ""), "Bat: " + std::to_string(battery_) + "%" + (charging_ ? "+" : "")};
            if (battery_ <= 10 && !charging_) p.col = th::red;
            parts.push_back(p);
        }
        // ★ Stuart, 2026-11-06: "Time left, record time right stats middle"
        const float lyT = ty - c.f.small.lineHeight() / 2.f;
        const float leftW = (float)c.tc.width(c.f.small, clk);
        c.tc.draw(c.g, c.f.small, clk, pill.x + 2, lyT, th::dim);
        float rightW = 0;
        if (num_.count("audio.recSec")) {
            const int sec = (int)num_["audio.recSec"];
            char rb[24]; std::snprintf(rb, sizeof rb, "REC %02d:%02d", sec / 60, sec % 60);
            rightW = (float)c.tc.width(c.f.small, rb) + 16;
            const float a = 0.55f + 0.45f * (float)std::sin(c.t * 4.0);
            c.g.circle(pill.r() - rightW + 5, ty, 5, 0, rgb(0xff3030, a));
            c.tc.draw(c.g, c.f.small, rb, pill.r() - 2, lyT, th::red, 2);
        }
        // the stats centred in the room between the clock and the recording time
        const float gapW = 18;
        const float room = pill.w - leftW - rightW - 2 * gapW;
        auto widthOf = [&](bool full) { float w = 0; for (auto& p : parts) w += c.tc.width(c.f.small, full ? p.full : p.abbr) + gapW; return w - gapW; };
        const bool full = widthOf(true) <= room;
        const float tw = widthOf(full);
        // centred on the row; nudged right of the clock if it would overlap
        float x = std::max(pill.x + 2 + leftW + gapW, pill.cx() - tw / 2);
        if (x + tw > pill.r() - rightW - gapW) x = pill.r() - rightW - gapW - tw;
        for (auto& p : parts) x += c.tc.draw(c.g, c.f.small, full ? p.full : p.abbr, x, lyT, p.col) + gapW;
    }
}

inline void App::drawNoRadio(Ctx& c, const RadioState& s) {
    c.g.rect(0, 0, (float)W, (float)H, th::bg);
    const char* msg = s.session == "none" ? "Please connect an SDR" : s.session == "starting" ? "Starting the radio…" : "Waiting for the receiver…";
    c.tc.draw(c.g, c.f.big, msg, W / 2.f, H / 2.f - 40, th::amber, 1);
    if (s.session == "starting" && !s.radioName.empty()) c.tc.draw(c.g, c.f.body, s.radioName, W / 2.f, H / 2.f + 4, th::dim, 1);
    if (s.session == "none") c.tc.draw(c.g, c.f.body, "Plug a radio into the USB-C OTG port", W / 2.f, H / 2.f + 4, th::dim, 1);
    BtnStyle st; st.font = &c.f.btn;
    if (button(c, "menu", {W / 2.f - 100, H - 110.f, 200, 52}, "MENU", st)) { menuOpen_ = "hw"; menuScroll_ = {}; open(S_MENU); }
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// sheets
// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

/** A sheet's panel: the web's dark glass with the amber hairline, a spaced title and CLOSE. Returns the content box. */
inline Rect sheetPanel(Ctx& c, Rect r, const std::string& title, const std::string& closeId, bool& closed) {
    c.g.rect(0, 0, 640, 480, th::shade);
    c.g.rrect(r.x, r.y, r.w, r.h, 12, th::sheetBg);
    c.g.rframe(r.x, r.y, r.w, r.h, 12, 1, th::btnBorder);
    float tx = r.x + 20;
    for (char ch : title) { const std::string one(1, ch); tx += c.tc.draw(c.g, c.f.bodyB, one, tx, r.y + 22, th::btnText) + 4; }
    BtnStyle st; st.font = &c.f.btn;
    closed = button(c, closeId, {r.r() - 118, r.y + 12, 102, 44}, "CLOSE", st);
    return {r.x + 16, r.y + 66, r.w - 32, r.h - 78};
}

inline void App::drawModeSheet(Ctx& c, const RadioState& s) {
    // the web's picker: a tall panel at the left, two columns, the current mode ringed in amber; the BANDWIDTH rows
    // (the web's .bwRow, moved under the grid) at the right, where the 640-wide screen has the room
    const Rect r{12, 8, 420, 464};
    c.g.rect(0, 0, 640, 480, th::shade);
    c.g.rrect(r.x, r.y, r.w, r.h, 10, rgb(0x101010, 0.97f));
    std::vector<ModeDef> ms;
    { std::lock_guard<std::mutex> lk(srvMtx_); ms = offeredModes(srvModes_, s.blockedModes); }
    const float cw = (r.w - 24) / 2, rh = std::min(50.f, (r.h - 16) / (float)((ms.size() + 1) / 2) - 4);
    for (size_t i = 0; i < ms.size(); i++) {
        const Rect b{r.x + 8 + (i % 2) * (cw + 8), r.y + 8 + (i / 2) * (rh + 4), cw, rh};
        const bool cur = dabActive() ? ms[i].id == "dab" : s.mode == ms[i].id;
        const Nav::Hit h = c.nav.item("m." + ms[i].id, b);
        if (cur) c.g.rframe(b.x, b.y, b.w, b.h, 6, 1.5f, th::amber);
        textIn(c, c.f.btn, ms[i].label, b, cur ? th::amber : rgb(0xdddddd));
        if (h.focused) focusRing(c, b, 6);
        if (h.pressed) {
            if (dabModePick(*this, s, ms[i].id)) { closeTop(); return; }   // DAB in / out (dab_state.h)
            eng_->setMode(ms[i].id);
            num_["bw.sync"] = -1;   // the new mode's own SYNC default
            closeTop();
            return;
        }
    }
    if (nav_.focus("mode").empty()) nav_.setFocus("mode", dabActive() ? std::string("m.dab") : "m." + s.mode);

    // ── BANDWIDTH (hidden in DRM, as the web's syncBw) ──
    if (s.mode == "drm" || s.mode.empty() || dabActive()) return;
    const Rect p{444, 8, 184, 464};
    c.g.rrect(p.x, p.y, p.w, p.h, 10, rgb(0x101010, 0.97f));
    c.tc.draw(c.g, c.f.bodyB, "BANDWIDTH", p.x + 14, p.y + 14, th::btnText);
    // main.ts BW_EDGE_MAX: the per-edge magnitude ceiling; the slider step follows it; WFM's minimum per edge is 20 kHz
    const std::string& m = s.mode;
    const int edgeMax = (m == "usb" || m == "lsb" || m == "ecssu" || m == "ecssl") ? 6000
                      : (m == "am" || m == "sam" || m == "sau" || m == "sal" || m == "amst") ? 20000
                      : (m == "cwu" || m == "cwl") ? 2000 : (m == "fm" || m == "nfm") ? 30000 : m == "wfm" ? 250000 : 20000;
    const int stepHz = edgeMax > 50000 ? 1000 : edgeMax > 10000 ? 100 : 10;
    const int edgeMin = m == "wfm" ? 20000 : 0;
    const bool lowSide = s.bwLow < 0, highSide = s.bwHigh > 0;
    // SYNC (mirror both edges): default ON for the AM family and FM, off for SSB / CW
    double& sync = num_["bw.sync"];
    if (sync < 0 || !num_.count("bw.sync.set")) {
        const bool on = m == "am" || m == "sam" || m == "sau" || m == "sal" || m == "amst" || m == "fm" || m == "nfm" || m == "wfm";
        sync = on ? 1 : 0; num_["bw.sync.set"] = 1;
    }
    const bool autoOk = [&] { const Json& a = s.hw("autoBwModes"); for (auto& x : a.a) if (x.str() == m) return true; return false; }();
    const bool autoOn = s.hw("autoBw").boolean() || s.msg("config")["autoBw"].boolean();
    auto edgeLabel = [](int hz) { char b[24]; std::snprintf(b, sizeof b, "%s%.1fk", hz < 0 ? "−" : "+", std::fabs(hz) / 1000.0); return std::string(b); };
    float y = p.y + 50;
    int lo = s.bwLow, hi = s.bwHigh;
    bool changed = false;
    if (lowSide) {
        c.tc.draw(c.g, c.f.small, "LOW  " + edgeLabel(lo), p.x + 14, y, th::dim);
        double v = -lo;
        if (slider(c, "bw.lo", {p.x + 18, y + 22, p.w - 36, 36}, v, edgeMin, edgeMax, stepHz)) {
            lo = -(int)v; if (sync > 0 && highSide) hi = (int)v; changed = true;
        }
        y += 72;
    }
    if (highSide) {
        c.tc.draw(c.g, c.f.small, "HIGH  " + edgeLabel(hi), p.x + 14, y, th::dim);
        double v = hi;
        if (slider(c, "bw.hi", {p.x + 18, y + 22, p.w - 36, 36}, v, edgeMin, edgeMax, stepHz)) {
            hi = (int)v; if (sync > 0 && lowSide) lo = -(int)v; changed = true;
        }
        y += 72;
    }
    if (changed) {
        eng_->setBandwidth(lo, hi);
        if (autoOn) eng_->send("{\"type\":\"autobw\",\"on\":false}");   // ★ a drag switches AUTO off: your width wins
    }
    if (lowSide && highSide) {
        if (toggle(c, "bw.sync", {p.x + 14, y, p.w - 28, 40}, sync > 0, "SYNC ON", "SYNC OFF")) {
            sync = sync > 0 ? 0 : 1;
            if (sync > 0) { const int w = std::max(-lo, hi); eng_->setBandwidth(-w, w); }   // mirrors at once to the wider edge
        }
        y += 50;
    }
    if (autoOk) {
        if (toggle(c, "bw.auto", {p.x + 14, y, p.w - 28, 40}, autoOn, "AUTO ON", "AUTO OFF"))
            eng_->send(std::string("{\"type\":\"autobw\",\"on\":") + (autoOn ? "false" : "true") + "}");
        y += 50;
    }
}

inline void App::drawStepSheet(Ctx& c, const RadioState& s) {
    const auto& steps = stepsForFreq(s.vfoHz);
    const float rh = 40, w = 220, h = steps.size() * rh + 16;
    const Rect r{lay::pillX, lay::btnY - 8 - h, w, h};
    c.g.rect(0, 0, 640, 480, rgb(0, 0.35f));
    c.g.rrect(r.x, r.y, r.w, r.h, 8, rgb(0x0d0d0d, 0.98f));
    c.g.rframe(r.x, r.y, r.w, r.h, 8, 1, rgb(0xffa000));
    const int cur = stepOnLadder(s.vfoHz, set_.step);
    for (size_t i = 0; i < steps.size(); i++) {
        const Rect b{r.x + 6, r.y + 8 + i * rh, r.w - 12, rh - 2};
        const Nav::Hit h = c.nav.item("st." + std::to_string(steps[i]), b);
        c.tc.draw(c.g, c.f.btn, formatStep(steps[i]), b.r() - 12, b.cy() - c.f.btn.lineHeight() / 2.f, steps[i] == cur ? th::activeTx : rgb(0xdddddd), 2);
        if (h.focused) focusRing(c, b, 5);
        if (h.pressed) { setStep(steps[i]); closeTop(); return; }
    }
    if (nav_.focus("step").empty()) nav_.setFocus("step", "st." + std::to_string(cur));
}

// ── the on-screen keyboard (padKeyboard.ts, drawn as the web page draws it) ─────────────────────────────────────────

inline void App::openFreqEntry() {
    const RadioState& s = eng_->state();
    KbRequest r;
    r.title = "FREQUENCY";
    r.opts.mode = kb::KbMode::frequency;
    char b[32];
    // ★ in the unit last used (Stuart: "stuck in MHz mode") — the web's freqUnit, device-wide
    const int u = set_.freqUnit;
    if (u == 0) std::snprintf(b, sizeof b, "%.0f", s.vfoHz);
    else std::snprintf(b, sizeof b, "%.3f", u == 1 ? s.vfoHz / 1e3 : s.vfoHz / 1e6);
    r.opts.value = "";
    r.placeholder = b;
    r.opts.unit = u == 0 ? kb::KbUnit::hz : u == 1 ? kb::KbUnit::khz : kb::KbUnit::mhz;
    r.opts.unitSettable = true;
    r.commit = [this](const kb::KbState& st) -> std::string {
        if (st.valueUtf8().empty()) return "";            // ↵ on the untouched pad: nothing changes
        const double v = kb::kbParseNumber(st.valueUtf8());
        if (!(v > 0)) return "Enter a frequency";
        const kb::KbUnit u = st.unit ? *st.unit : kb::KbUnit::mhz;
        return tuneTyped(std::round(v * (u == kb::KbUnit::mhz ? 1e6 : u == kb::KbUnit::khz ? 1e3 : 1)));
    };
    openKeyboard(r);
}

inline std::string App::tuneTyped(double hz) {
    const RadioState& s = eng_->state();
    // ★ the web's go(): through clampTune — a typed frequency in a gap parks on the edge and says how to cross it
    gapNudgeDir_ = 0;
    const double got = clampTune(hz);
    // ★ A JUMP takes the landing band's step (and mode, when it changes band) — bandPlan.ts bandJumpDefaults
    const BandDefaults d = bandJumpDefaults(s.vfoHz, got, s.mode, region());
    tuneTo(got);
    if (!d.mode.empty()) eng_->setMode(d.mode);
    if (d.step > 0) setStep(stepOnLadder(got, d.step));
    return "";
}

inline void App::kbInput(Btn b) {
    if (padLinksInput(b)) return;   // the pad's BOOKMARKS / SEARCH keys
    const bool numeric = kbd_.layout() == kb::KbLayoutName::numeric;
    kb::Outcome o = kb::Outcome::none;
    const auto commit = kbReq_.commit ? kbReq_.commit : kb::Commit{};
    switch (b) {
        case B_UP: o = kbd_.move(kb::Dir::up); break;
        case B_DOWN: o = kbd_.move(kb::Dir::down); break;
        case B_LEFT: o = kbd_.move(kb::Dir::left); break;
        case B_RIGHT: o = kbd_.move(kb::Dir::right); break;
        case B_ENTER: o = kbd_.press(commit); break;
        case B_BACK: o = kbd_.back(); break;
        case B_LEFTFACE: o = kbd_.edit(kb::EditAct::bs); break;      // □ ⌫
        case B_TOPFACE: o = kbd_.edit(kb::EditAct::space); break;    // △ space
        case B_L1: o = kbd_.edit(numeric ? kb::EditAct::unit : kb::EditAct::shift); break;
        case B_R1: o = kbd_.edit(numeric ? kb::EditAct::unit : kb::EditAct::symbols); break;
        case B_START: o = kbd_.submit(commit); break;
        default: break;
    }
    // ★ the frequency pad's unit is remembered as soon as it changes (the web's unit seg saves freqUnit at once)
    if (kbReq_.title == "FREQUENCY" && kbd_.state().unit) {
        const kb::KbUnit u = *kbd_.state().unit;
        const int ui = u == kb::KbUnit::hz ? 0 : u == kb::KbUnit::khz ? 1 : 2;
        if (ui != set_.freqUnit) { set_.freqUnit = ui; dirty(); }
    }
    if (o == kb::Outcome::submit || o == kb::Outcome::cancel) {
        for (size_t i = sheets_.size(); i-- > 0;) if (sheets_[i] == S_KB) { sheets_.erase(sheets_.begin() + (long)i); break; }
    }
}

/** A system key's glyph, drawn (the font has no ⌫ ↵ ⇧): bs, enter, shift. */
inline void kbGlyph(Ctx& c, kb::KeyActT t, Rect b, Rgba col, bool caps) {
    const float cx = b.cx(), cy = b.cy();
    if (t == kb::KeyActT::enter) {
        c.g.line(cx + 10, cy - 9, cx + 10, cy + 3, 2.2f, col); c.g.line(cx + 10, cy + 3, cx - 10, cy + 3, 2.2f, col);
        c.g.line(cx - 10, cy + 3, cx - 4, cy - 3, 2.2f, col); c.g.line(cx - 10, cy + 3, cx - 4, cy + 9, 2.2f, col);
    } else if (t == kb::KeyActT::bs) {
        c.g.line(cx - 14, cy, cx - 6, cy - 8, 1.8f, col); c.g.line(cx - 14, cy, cx - 6, cy + 8, 1.8f, col);
        c.g.line(cx - 6, cy - 8, cx + 14, cy - 8, 1.8f, col); c.g.line(cx - 6, cy + 8, cx + 14, cy + 8, 1.8f, col);
        c.g.line(cx + 14, cy - 8, cx + 14, cy + 8, 1.8f, col);
        c.g.line(cx - 1, cy - 4, cx + 7, cy + 4, 1.8f, col); c.g.line(cx - 1, cy + 4, cx + 7, cy - 4, 1.8f, col);
    } else if (t == kb::KeyActT::shift) {
        c.g.line(cx, cy - 11, cx - 10, cy, 1.8f, col); c.g.line(cx, cy - 11, cx + 10, cy, 1.8f, col);
        c.g.line(cx - 10, cy, cx - 5, cy, 1.8f, col); c.g.line(cx + 10, cy, cx + 5, cy, 1.8f, col);
        c.g.line(cx - 5, cy, cx - 5, cy + 8, 1.8f, col); c.g.line(cx + 5, cy, cx + 5, cy + 8, 1.8f, col);
        c.g.line(cx - 5, cy + 8, cx + 5, cy + 8, 1.8f, col);
        if (caps) c.g.rect(cx - 5, cy + 11, 10, 2, col);
    }
}

inline void App::drawKeyboard(Ctx& c, const RadioState& s) {
    (void)s;
    c.g.rect(0, 0, 640, 480, rgb(0x050402, 0.97f));
    const bool numeric = kbd_.layout() == kb::KbLayoutName::numeric;
    const auto& keys = kbd_.keys();
    double uw = 1, uh = 1;
    for (auto& k : keys) { uw = std::max(uw, k.x + k.w); uh = std::max(uh, k.y + k.h); }
    // the value box (title · text with caret · unit · the refusal under it)
    const Rect rd = numeric ? Rect{16, 168, 270, 84} : Rect{16, 20, 608, 84};
    c.g.rrect(rd.x, rd.y, rd.w, rd.h, 10, rgb(0x140a00));
    c.g.rframe(rd.x, rd.y, rd.w, rd.h, 10, 1, th::btnBorder);
    c.tc.draw(c.g, c.f.small, kbReq_.title, rd.x + 16, rd.y + 10, th::dim);
    {
        const std::string before = kbd_.displayBefore(), after = kbd_.displayAfter();
        const Font& vf = numeric ? c.f.big : c.f.title;
        const float ty = rd.y + 36 + (numeric ? 0 : 6);
        float x = rd.x + 16;
        const bool ph = before.empty() && after.empty() && !kbReq_.placeholder.empty();
        float phW = 0;
        if (ph) phW = (float)c.tc.draw(c.g, vf, kbReq_.placeholder, x + 6, ty, rgb(0xffffff, 0.35f)) + 6;
        x += c.tc.draw(c.g, vf, before, x, ty, th::white);
        if (std::fmod(c.t, 1.0) < 0.6) c.g.rect(x + 1, ty + 3, 2, vf.lineHeight() - 6, th::white);
        x += 4;
        x += c.tc.draw(c.g, vf, after, x, ty, th::white);
        const std::string u = kbd_.unitText();
        if (!u.empty()) c.tc.draw(c.g, c.f.body, u, std::max(x, rd.x + 16 + phW) + 8, ty + (vf.lineHeight() - c.f.body.lineHeight()) - 4, th::unit);
    }
    if (!kbd_.err().empty()) {
        float y = rd.b() + 10;
        for (auto& l : c.tc.wrap(c.f.small, kbd_.err(), numeric ? (int)rd.w : 600)) { c.tc.draw(c.g, c.f.small, l, rd.x, y, th::red); y += c.f.small.lineHeight(); }
    }
    // the keys
    const Rect kr = numeric ? Rect{300, 106, 324, 324 * 4 / 4.75f} : Rect{8, 200, 624, 268};
    const float ux = kr.w / (float)uw, uy = kr.h / (float)uh, gp = 4;
    for (size_t i = 0; i < keys.size(); i++) {
        const auto& k = keys[i];
        const Rect b{kr.x + (float)k.x * ux + gp, kr.y + (float)k.y * uy + gp, (float)k.w * ux - 2 * gp, (float)k.h * uy - 2 * gp};
        const bool digit = k.act.t == kb::KeyActT::ch;
        const bool lit = kbd_.keyLit((int)i);
        if (lit) { c.g.rrect(b.x, b.y, b.w, b.h, 8, rgb(0x2a2000)); c.g.rframe(b.x, b.y, b.w, b.h, 8, 1, rgb(0xffe566, 0.55f)); }
        else if (digit) { c.g.rrect(b.x, b.y, b.w, b.h, 8, rgb(0x000000)); c.g.rframe(b.x, b.y, b.w, b.h, 8, 1, rgb(0xffffff, 0.5f)); }
        else { c.g.rrect(b.x, b.y, b.w, b.h, 8, rgb(0x140a00)); c.g.rframe(b.x, b.y, b.w, b.h, 8, 1, th::btnBorder); }
        const Rgba col = lit ? th::activeTx : digit ? th::white : th::btnText;
        if (k.act.t == kb::KeyActT::enter || k.act.t == kb::KeyActT::bs || k.act.t == kb::KeyActT::shift)
            kbGlyph(c, k.act.t, b, col, kbd_.state().layer == kb::KbLayer::caps);
        else textIn(c, digit ? (numeric ? c.f.big : c.f.title) : c.f.btn, k.label, b, col);
        if ((int)i == kbd_.focus() && !padLinkFocused()) c.g.rframe(b.x - 4, b.y - 4, b.w + 8, b.h + 8, 11, 3, kbd_.secure() ? rgb(0xaaaaaa) : th::focus);
    }
    if (numeric && kbReq_.title == "FREQUENCY") drawPadLinks(c, {16, 392, 608, 52});
    if (!numeric) drawSearchLive(c, {16, 112, 608, 84});
    hints(c, 16, 464, numeric ? std::vector<std::pair<int, std::string>>{{FACE_BOTTOM, "press"}, {FACE_RIGHT, "back"}, {FACE_LEFT, "delete"}, {-1, "L R unit · START enter"}}
                              : std::vector<std::pair<int, std::string>>{{FACE_BOTTOM, "press"}, {FACE_RIGHT, "back"}, {FACE_LEFT, "delete"}, {FACE_TOP, "space"}, {-1, "L shift · R symbols"}});
}

inline float App::menuRow(Ctx& c, Rect area, float y, const std::string& label, const std::string& help) {
    c.tc.draw(c.g, c.f.bodyB, label, area.x + 6, y + 22 - c.f.bodyB.lineHeight() / 2.f, th::btnText);
    y += 44;
    if (!help.empty()) {
        for (auto& l : c.tc.wrap(c.f.small, help, (int)area.w - 12)) { c.tc.draw(c.g, c.f.small, l, area.x + 6, y, th::dim); y += c.f.small.lineHeight(); }
        y += 4;
    }
    return y + 6;
}

inline void App::drawMenu(Ctx& c, const RadioState& s) {
    bool closed = false;
    const Rect panel{12, 10, 616, 460};
    const Rect area = sheetPanel(c, panel, "MENU", "menu.close", closed);
    if (closed) { closeTop(); return; }
    const Rect view{area.x - 8, area.y, area.w + 16, area.h};
    c.g.clip(view.x, view.y, view.w, view.h);
    float y = area.y - menuScroll_.y;
    struct Sec { const char* id; const char* title; };
    const Sec secs[] = {{"hw", "R36S HARDWARE"}, {"sdr", "SDR HARDWARE"}, {"disp", "DISPLAY"}, {"pad", "GAME CONTROLLER"}};
    for (auto& sec : secs) {
        const bool openNow = menuOpen_ == sec.id;
        const Rect hr{area.x, y, area.w, 52};
        BtnStyle st; st.font = &c.f.title; st.on = openNow;
        const Nav::Hit h = c.nav.item(std::string("sec.") + sec.id, hr);
        c.g.rrect(hr.x, hr.y, hr.w, hr.h, 8, openNow ? th::activeBg : th::btnBg);
        c.g.rframe(hr.x, hr.y, hr.w, hr.h, 8, 1, openNow ? th::activeBdr : th::btnBorder);
        float tx = hr.x + 16;
        for (char ch : std::string(sec.title)) { const std::string one(1, ch); tx += c.tc.draw(c.g, c.f.title, one, tx, hr.cy() - c.f.title.lineHeight() / 2.f, th::btnText) + 2; }
        // ▸ / ▾
        if (openNow) c.g.triangle(hr.r() - 30, hr.cy() - 4, hr.r() - 18, hr.cy() - 4, hr.r() - 24, hr.cy() + 4, th::btnText);
        else c.g.triangle(hr.r() - 28, hr.cy() - 6, hr.r() - 28, hr.cy() + 6, hr.r() - 20, hr.cy(), th::btnText);
        if (h.focused) focusRing(c, hr, 8);
        if (h.pressed) { menuOpen_ = openNow ? "" : sec.id; }
        y += 60;
        if (openNow) {
            const Rect inner{area.x + 12, y, area.w - 12, 0};
            c.g.rect(area.x + 4, y, 2, 1, th::btnBorder);
            const float y1 = sec.id[0] == 'h' ? secHardware(c, s, inner, y) : sec.id[0] == 's' ? secSdr(c, s, inner, y) : sec.id[0] == 'p' ? secController(c, s, inner, y) : secDisplay(c, s, inner, y);
            c.g.rect(area.x + 4, y, 2, y1 - y, th::btnBorder);
            y = y1 + 8;
        }
    }
    c.g.unclip();
    menuScroll_.contentH = y + menuScroll_.y - area.y;
    menuScroll_.follow(c.nav.focusedRect(), view);
    // ★ the back button folds an open section first, then closes the menu (the web's accordion: B collapses)
    if (!menuOpen_.empty() && c.nav.input().back && c.nav.backPressed()) {
        nav_.setFocus("menu", "sec." + menuOpen_);
        menuOpen_.clear();
    }
    if (nav_.focus("menu").empty()) nav_.setFocus("menu", menuOpen_.empty() ? "sec.hw" : "sec." + menuOpen_);
}

inline void App::drawToasts(Ctx& c) {
    const double t = c.t;
    if (t < volUntil_ && volume_ >= 0) {
        const float bw = 300, bh = 74, bx = (W - bw) / 2, by = 110;
        c.g.rrect(bx, by, bw, bh, 12, rgb(0, 0.85f));
        c.g.rframe(bx, by, bw, bh, 12, 1.5f, th::btnBorder);
        c.tc.draw(c.g, c.f.title, muted_ ? "MUTED" : "VOLUME " + std::to_string(volume_), bx + bw / 2, by + 10, th::btnText, 1);
        c.g.rrect(bx + 20, by + 48, bw - 40, 10, 5, th::meterTrack);
        c.g.rrect(bx + 20, by + 48, std::max(10.f, (bw - 40) * volume_ / 100.f), 10, 5, th::amber);
    }
    if (t < toastUntil_) {
        // ★ wrapped: the gap message ("31.000 MHz — this radio cannot receive here. Tune up again …") is long
        const auto& lines = c.tc.wrap(c.f.btn, toast_, 540);
        float tw = 0;
        for (auto& l : lines) tw = std::max(tw, (float)c.tc.width(c.f.btn, l));
        const float lh = (float)c.f.btn.lineHeight();
        const float bw = tw + 48, bh = lines.size() * lh + 24, bx = (W - bw) / 2, by = 270 - bh / 2;
        c.g.rrect(bx, by, bw, bh, 12, rgb(0, 0.9f));
        c.g.rframe(bx, by, bw, bh, 12, 1.5f, th::btnBorder);
        float y = by + 12;
        for (auto& l : lines) { c.tc.draw(c.g, c.f.btn, l, W / 2.f, y, th::white, 1); y += lh; }
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// the loop
// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

inline int App::run(int argc, char** argv) {
    int port = 48000;
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
    const char* shot = std::getenv("VIBE_HHUI_SCREENSHOT");     // ★ tests: write a PPM of frame N and exit
    const int shotFrame = std::getenv("VIBE_HHUI_SHOT_FRAME") ? std::atoi(std::getenv("VIBE_HHUI_SHOT_FRAME")) : 90;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "vibe-hhui: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) { std::fprintf(stderr, "vibe-hhui: TTF_Init: %s\n", TTF_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_DisplayMode dm{};
    if (!shot && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0) { W = dm.w; H = dm.h; }   // ★ tests: 640×480, the panel
    // ★ 4× MSAA for the rounded corners and the rings: on a tile-based Mali it resolves in the tile, nearly free.
    //   Falls back to none if the driver refuses.
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    const Uint32 wflags = SDL_WINDOW_OPENGL | (shot ? 0 : SDL_WINDOW_FULLSCREEN);
    win_ = SDL_CreateWindow("VibeSDR", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H, wflags);
    SDL_GLContext ctx = win_ ? SDL_GL_CreateContext(win_) : nullptr;
    if (!ctx) {
        if (win_) SDL_DestroyWindow(win_);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        win_ = SDL_CreateWindow("VibeSDR", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H, wflags);
        if (!win_) { std::fprintf(stderr, "vibe-hhui: window: %s\n", SDL_GetError()); return 1; }
        ctx = SDL_GL_CreateContext(win_);
        if (!ctx) { std::fprintf(stderr, "vibe-hhui: GL context: %s\n", SDL_GetError()); return 1; }
        std::fprintf(stderr, "vibe-hhui: no MSAA\n");
    }
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_DISABLE);
    std::fprintf(stderr, "vibe-hhui: %dx%d, %s / %s\n", W, H, (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));

    g_.init(W, H);
    // ★★ ATKINSON HYPERLEGIBLE EVERYWHERE (Stuart, 2026-11-06) — the Braille Institute's face, made for low-vision reading
    const std::string R = fontPath("AtkinsonHyperlegible-Regular.ttf"), B = fontPath("AtkinsonHyperlegible-Bold.ttf");
    f_.tiny.load(B, 13); f_.small.load(R, 16); f_.body.load(R, 19); f_.bodyB.load(B, 19); f_.btn.load(B, 21); f_.title.load(B, 22);
    f_.big.load(B, 34); f_.freq.load(B, 38); f_.freqFine.load(B, 22); f_.unit.load(R, 18);

    set_.load();
    if (!wfp_.init()) std::fprintf(stderr, "vibe-hhui: the waterfall did not initialise\n");
    // ★★★ THE APP'S WATERFALL (memory: feedback_waterfall_core_rules): per-row placement, the as-drawn zoom fan, every tune
    //     eased, the fixed output clock (DEFAULT = 30 rows/s, never 20), the app's palettes (Jet, its production default).
    applyLook();

    Engine eng(port);
    eng_ = &eng;
    eng.start();
    lastInput_ = nowSec();

    // ★ tests: VIBE_HHUI_SCRIPT="frame:BUTTON,…" presses buttons at drawn frames
    std::vector<std::pair<int, Btn>> script;
    std::vector<std::pair<int, double>> stickScript;
    if (const char* sc = std::getenv("VIBE_HHUI_SCRIPT")) {
        const std::pair<const char*, Btn> names[] = {{"ENTER", B_ENTER}, {"BACK", B_BACK}, {"UP", B_UP}, {"DOWN", B_DOWN}, {"LEFT", B_LEFT},
            {"RIGHT", B_RIGHT}, {"L1", B_L1}, {"R1", B_R1}, {"L2", B_L2}, {"R2", B_R2}, {"SELECT", B_SELECT},
            {"START", B_START}, {"TOPFACE", B_TOPFACE}, {"LEFTFACE", B_LEFTFACE},
            {"STICKL", B_NONE}, {"STICKR", B_NONE}, {"STICK0", B_NONE}};
        std::string all(sc), tok;
        for (size_t i = 0; i <= all.size(); i++) {
            if (i < all.size() && all[i] != ',') { tok += all[i]; continue; }
            const size_t cpos = tok.find(':');
            if (cpos != std::string::npos) {
                const std::string nm = tok.substr(cpos + 1);
                if (nm == "STICKL" || nm == "STICKR" || nm == "STICK0") stickScript.push_back({std::atoi(tok.c_str()), nm == "STICKL" ? -1.0 : nm == "STICKR" ? 1.0 : 0.0});
                else for (auto& n : names) if (nm == n.first) script.push_back({std::atoi(tok.c_str()), n.second});
            }
            tok.clear();
        }
    }

    uint64_t frame = 0;
    double lastDraw = 0, lastDevPoll = 0;
    int sentRate = -1;
    std::string lastSession;
    std::vector<DeviceEvent> devEvents;
    prof_.since = nowSec();
    while (!quit_) {
        double pc = threadCpu();
        prof_.loops++;
        pollInput();
        hwPump(*this);
        sampleCpu();
        stickSweep();

        // the engine
        devEvents.clear();
        eng.pump([&](const uint8_t* bins, int n, double centreHz, double) {
            // ★ bins are low → high already (engine.h rotated them); the span is this view's (config.totalBandwidth)
            wfp_.pushFrame(bins, n, false, centreHz, eng.state().spanHz, nowSec());
            updateSignal(bins, n, centreHz, eng.state().spanHz);
            audioSpecFrame(*this, bins, n, centreHz, eng.state().spanHz);
        }, devEvents, [&](const uint8_t* d, size_t n) {
            // ★ the full-capture layer: the edges a glide or a zoom-in exposes are filled from real capture (wf_wide.h)
            wfp_.pushWide(d, n, nowSec());
        });
        for (auto& de : devEvents) {
            if (de.kind == "volume") { volume_ = (int)de.body["volume"].num(-1); muted_ = de.body["muted"].boolean(); volUntil_ = nowSec() + 1.5; }
            else if (de.kind == "battery") { battery_ = (int)de.body["pct"].num(battery_); charging_ = de.body["charging"].boolean(); }
            else if (de.kind == "battery_low") toast("Battery low — " + std::to_string((int)de.body["pct"].num()) + " %");
            else if (de.kind == "output") toast(de.body["output"].str() == "headphones" ? "Headphones" : "Speaker");
            else if (de.kind == "screen") { screenOff_ = de.body["screen"].str() == "off"; if (!screenOff_) lastInput_ = nowSec(); }
            else if (de.kind == "shutdown") toast("Switching off");
        }
        const RadioState& s = eng.state();
        audioTick(*this, s);
        dabTick(*this, s);
        if (s.session != lastSession) { if (s.session == "running") { nav_.setFocus("deck", "freq"); lastDevPoll = 0; } lastSession = s.session; }
        if (s.connected && sentRate != set_.specRate) { eng.fftRate(set_.specRate); sentRate = set_.specRate; }
        if (!s.connected) sentRate = -1;
        // the view follows the dial: ≤ 1 zoom per 33 ms, the last one always sent (spectrum.ts _sendView)
        if (viewPending_ && nowSec() - viewSentAt_ > 0.033) {
            eng.zoom(viewF_, viewSpan_);
            viewSentAt_ = nowSec();
            viewPending_ = false;
        }
        // the device's state, once a minute (and at the start)
        if (nowSec() - lastDevPoll > 60 || lastDevPoll == 0) {
            lastDevPoll = nowSec();
            std::thread([this, p = eng.port()] {
                const HttpResult r = httpRequest(p, "GET", "/handheld/device", "", 4);
                if (r.status == 200) {
                    const Json j = Json::parse(r.body)["state"];
                    if (j.has("battery")) { battery_ = (int)j["battery"]["pct"].num(-1); charging_ = j["battery"]["charging"].boolean(); }
                    if (j.has("brightness")) brightness_ = (int)j["brightness"].num(brightness_);
                    if (j.has("volume")) volume_ = (int)j["volume"].num(volume_);
                }
                const HttpResult v = httpRequest(p, "GET", "/vibeserver.json", "", 4);
                if (v.status == 200) {
                    std::vector<std::string> ms;
                    const Json vj = Json::parse(v.body);
                    for (auto& m : vj["modes"].a) ms.push_back(m.str());
                    if (vj["dab"].boolean()) ms.push_back("dab");
                    std::lock_guard<std::mutex> lk(srvMtx_); srvModes_ = ms;
                }
                const HttpResult l = httpRequest(p, "GET", "/handheld/location", "", 4);
                if (l.status == 200) {
                    const Json lj = Json::parse(l.body);
                    std::lock_guard<std::mutex> lk(srvMtx_);
                    haveLon_ = lj["set"].boolean() || lj.has("lon"); lon_ = lj["lon"].num(0);
                    if (!lj.has("lon")) haveLon_ = false;
                }
            }).detach();
        }
        // the screen-off timer (the hardware service wakes it on any button)
        if (set_.screenOffMin > 0 && !screenOff_ && nowSec() - lastInput_ > set_.screenOffMin * 60.0) {
            eng.device("{\"action\":\"screen-off\"}");
            screenOff_ = true;
        }
        if (settingsDirty_ && nowSec() - settingsAt_ > 1.0) { set_.save(); settingsDirty_ = false; }

        // ── draw (≤ 30 fps; nothing while the screen is off) ──
        const double t = nowSec();
        {
            const double c1 = threadCpu(); prof_.pre += c1 - pc; pc = c1;
            if (t - prof_.since >= 30) {
                const double fr = std::max(1L, prof_.frames), el = t - prof_.since;
                std::fprintf(stderr, "vibe-hhui: perf %.0f s — %ld frames (%.1f fps), %ld loops/s · UI-thread CPU per frame: "
                             "ticks %.2f ms · waterfall %.2f ms · drawing %.2f ms · swap %.2f ms · total %.0f %% of a core\n",
                             el, prof_.frames, prof_.frames / el, (long)(prof_.loops / el), prof_.pre * 1e3 / fr, prof_.wf * 1e3 / fr,
                             prof_.ui * 1e3 / fr, prof_.swap * 1e3 / fr, 100 * (prof_.pre + prof_.wf + prof_.ui + prof_.swap) / el);
                prof_ = Prof{}; prof_.since = t;
            }
        }
        // ★ between frames, SLEEP until the next one is due (≤ 16 ms, so the sticks are still read ~60 times a second —
        //   the web samples its pad once an animation frame); it used to wake every 4 ms and redo every tick
        if (screenOff_) { SDL_Delay(30); continue; }
        if (t - lastDraw < 1.0 / 30.0 - 0.002) {
            const double wait = (1.0 / 30.0 - 0.002) - (t - lastDraw);
            SDL_Delay((Uint32)std::max(1.0, std::min(16.0, wait * 1000)));
            continue;
        }
        prof_.frames++;
        lastDraw = t;
        frame++;
        for (auto& sp : stickScript) if (sp.first == (int)frame) testStick_ = sp.second;
        for (auto& sp : script) if (sp.first == (int)frame) { std::fprintf(stderr, "vibe-hhui: script press %d at frame %d\n", (int)sp.second, sp.first); press(sp.second); }
        tc_.tick(frame);
        glViewport(0, 0, W, H);
        glClearColor(th::bg.r, th::bg.g, th::bg.b, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        // one press per frame for the highlight (a fast D-pad still lands every press: 30 a second)
        Input in;
        if (!presses_.empty()) {
            const Btn b = presses_.front(); presses_.pop_front();
            if (b == B_UP) in.nav = D_UP; else if (b == B_DOWN) in.nav = D_DOWN; else if (b == B_LEFT) in.nav = D_LEFT; else if (b == B_RIGHT) in.nav = D_RIGHT;
            else if (b == B_ENTER) in.enter = true; else if (b == B_BACK) in.back = true;
        }
        const std::string top = sheets_.empty() ? "deck" : layerName(sheets_.back());
        nav_.begin(top, in);
        Ctx c{g_, tc_, f_, nav_, t};

        nav_.layer("deck");
        if (s.session != "running") {
            drawNoRadio(c, s);
        } else {
            // spectrum + waterfall: the app's (waterfall FIRST — it runs the clock and the pace)
            g_.flush();
            Rect spec{0, lay::specY, (float)W, lay::specH}, wfr{0, lay::wfY, (float)W, (float)H - lay::wfY};
            Rect scaleR{0, lay::scaleY, (float)W, lay::scaleH};
            DabScreenRects dr;
            const bool dabScr = dabScreenRects(*this, dr);   // ★ the DAB page (layout C): the spectrum as a strip
            if (dabScr) { spec = dr.spec; wfr = dr.wf; scaleR = dr.scale; }
            { const double c0 = threadCpu(); prof_.ui += c0 - pc; pc = c0; }
            wfp_.draw(0, (int)wfr.y, W, (int)wfr.h, W, H, t);
            wfp_.drawSpectrum(0, (int)spec.y, W, (int)spec.h, W, H);
            { const double c0 = threadCpu(); prof_.wf += c0 - pc; pc = c0; }
            glEnable(GL_BLEND);
            if (!dabScr || dr.bands) drawBands(c, s, {0, 0, (float)W, lay::bandsH});
            drawScale(c, s, scaleR);
            drawOverlays(c, s, spec, wfr);
            dabDrawBox(*this, c, s);
            drawDeck(c, s, {lay::islandX, lay::islandY, lay::islandW, lay::islandH});
            drawVts(c, s, {lay::islandX, lay::islandY - 40, lay::islandW, 34});
        }
        for (size_t i = 0; i < sheets_.size(); i++) {
            const Sheet sh = sheets_[i];
            nav_.layer(layerName(sh));
            const size_t before = sheets_.size();
            switch (sh) {
                case S_MODE: drawModeSheet(c, s); break;
                case S_STEP: drawStepSheet(c, s); break;
                case S_KB: drawKeyboard(c, s); break;
                case S_BOOKMARKS: drawBookmarks(c, s); break;
                case S_SEARCH: drawSearch(c, s); break;
                case S_DAB: dabDrawFull(*this, c, s); break;
                case S_AUDIO: drawAudioSheet(c, s); break;
                case S_MENU: drawMenu(c, s); break;
                default: break;
            }
            if (sheets_.size() < before) break;     // it closed itself
        }
        if (nav_.backPressed() && !sheets_.empty()) closeTop();
        nav_.end();
        drawToasts(c);
        hwDrawPerf(c);
        g_.flush();

        if (shot && (int)frame == shotFrame) {
            std::vector<uint8_t> px((size_t)W * H * 4);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            if (FILE* f = std::fopen(shot, "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (int y = H - 1; y >= 0; y--) for (int x = 0; x < W; x++) std::fwrite(&px[((size_t)y * W + x) * 4], 1, 3, f);
                std::fclose(f);
            }
            quit_ = true;
        }
        { const double c0 = threadCpu(); prof_.ui += c0 - pc; pc = c0; }
        SDL_GL_SwapWindow(win_);
        { const double c0 = threadCpu(); prof_.swap += c0 - pc; pc = c0; }
    }
    if (settingsDirty_) set_.save();
    eng.stop();
    eng_ = nullptr;
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win_);
    TTF_Quit();
    SDL_Quit();
    return 0;
}

}  // namespace hh
