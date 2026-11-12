// vts.h — the VTS bar on the R36S: the app's station strip (src/components/VTSBar.tsx), fed from the engine's messages
// the way the app feeds it (SDRScreen.tsx + VibeServerWsClient.ts), drawn in Atkinson Hyperlegible.
//
// What it shows, in the app's order of ownership (vts_core.h has the rules, line for line):
//   · a NOTICE owns the bar for its life — the server's explanations (`notice` why / vts, 7 s), the app's own
//     (gain at minimum 30 s, SDRplay AGC initialising ≤ 75 s, the RTL's "AGC: On  |  IF Filter: Auto" 8 s); one at a
//     time, the rest queue (≤ 3), the same words never twice;
//   · the LIVE STATION, held while it is on air: "PI: C363 / Name: RadioText" (RDS — the PS through the app's
//     stabiliser, US call letters where the PS is not a name) or "SId: C6D6 / Service: DLS" (DAB), with the RDS / DAB+
//     mark on the left, the transmitter's country, and TP · TA · AF on the right;
//   · a BAND CROSSING, 8 s: "BAND: 87.5 MHz–108 MHz · FM Broadcast Band" (+ the overlapping bands under it);
//   · the NFM channel's CTCSS / DCS tone, held while locked.
// The bar comes in over 180 ms, a held line updates IN PLACE, a timed one goes after its time over 300 ms; a line too
// long for its window slides: held = 1.2 s pause, 16 ms a pixel (≥ 2.2 s), 1.5 s pause, back to the start, for ever;
// timed = 0.9 s pause, one pass at reading speed.
//
// ★ Placement (R36S, 640×480): a full-width strip directly ABOVE the control island, over the oldest rows of the
//   waterfall — where the app floats it, above the controls pill. Drawn only while it has something to say.
// ★ Include AFTER the App class (it defines App::drawVts). State lives here (vtsState), not in App.
#pragma once
#include "vts_core.h"
#include "vts_marks.h"

#include <SDL2/SDL_ttf.h>
#include <cctype>
#include <cstdlib>

namespace hh {
namespace vts {

// ── the strip's colours (constants/faceplate.ts: TODAY_VTS + the default chassis — the default HYPER deck) ─────────
namespace col {
inline const Rgba onTune = rgb(0x50dc64, 0.95f);     // rgba(80,220,100,0.95) — also the RDS mark (`mark`)
inline const Rgba band   = rgb(0xffe566);            // band + notice lines
inline const Rgba dim    = rgb(0xffffff, 0.35f);     // an arrow that does not point anywhere
inline const Rgba sub    = rgb(0xffffff, 0.55f);     // the identity, the separators, the sub-line
inline const Rgba core   = rgb(0x3dff72);            // TP · TA · AF lit, the DAB mark (text.core, green)
inline const Rgba ghost  = rgb(0x3dff72, 0.16f);     // TP · TA · AF dark, the DAB "+]" on MP2
inline const Rgba bg     = rgb(0x080a0e, 0.94f);     // chassis vtsBg
inline const Rgba border = rgb(0xffffff, 0.22f);     // chassis vtsBorder
}  // namespace col

/** A message ARRIVED since the last frame. RadioState::last keeps only the newest message of each type, whole; a new one
 *  is a new parsed object, so its first map node is at a new address (the old one is freed only after the new one is
 *  allocated) — a repeat of the very same words still counts, as it does in the app. The content is compared as well. */
struct Arrival {
    const void* node = nullptr;
    std::string sig;
    bool operator()(const Json& j, const std::string& s) {
        const void* n = j.kind == Json::Obj && !j.o.empty() ? (const void*)&*j.o.begin() : nullptr;
        if (!n) { node = nullptr; return false; }
        const bool fresh = n != node || s != sig;
        node = n; sig = s;
        return fresh;
    }
};

/** Everything vts.h keeps between frames. */
struct State {
    Screen screen;
    Bar bar;
    Arrival rds, dab, dabOff, dabErr, drm, tone, notice, device;
    bool wasConnected = false;
    std::map<std::pair<const void*, char32_t>, bool> glyphs;   // TTF_GlyphIsProvided32, cached
};
inline State& vtsState() { static State s; return s; }

inline std::string numSig(const Json& j, const char* k) { char b[32]; std::snprintf(b, sizeof b, "%.3f", j[k].num(-9999)); return b; }

/** One frame's Inputs from the engine's state (the app's VibeServerWsClient → SDRScreen path). */
inline Inputs readInputs(State& st, const RadioState& s, double nowMs, int region) {
    Inputs in;
    in.now = nowMs;
    in.mode = s.mode;
    in.vfoHz = s.vfoHz;
    in.region = region;
    in.connectedEdge = s.connected && !st.wasConnected;
    st.wasConnected = s.connected;
    // rds — the shim sends the whole state on every change (ps, radiotext, pi, ecc, ber, sig, tp, ta, af, call)
    {
        const Json& j = s.msg("rds");
        const std::string sig = j["ps"].str() + "\x1f" + j["radiotext"].str() + "\x1f" + numSig(j, "pi") + numSig(j, "ecc")
            + numSig(j, "ber") + numSig(j, "sig") + numSig(j, "tp") + numSig(j, "ta") + numSig(j, "af") + j["call"].str()
            + (j["stereo"].boolean() ? "s" : "m");
        if (st.rds(j, sig)) {
            in.rdsNew = true;
            in.rdsPs = j["ps"].str(); in.rdsRt = j["radiotext"].str(); in.rdsCall = j["call"].str();
            in.rdsPi = j["pi"].kind == Json::Num ? (int)j["pi"].num() : -1;
            in.rdsEcc = j["ecc"].kind == Json::Num ? (int)j["ecc"].num() : 0;
            in.rdsHasFlags = j["tp"].kind == Json::Num && j["ta"].kind == Json::Num && j["af"].kind == Json::Num;
            in.rdsTp = (int)j["tp"].num(-1); in.rdsTa = (int)j["ta"].num(-1); in.rdsAf = (int)j["af"].num(-1);
        }
    }
    // dab — the playing service is the live station; dab_off / a refusal ends DAB
    {
        const Json& off = s.msg("dab_off");
        if (st.dabOff(off, "off")) in.dabOffNew = true;
        const Json& err = s.msg("dab_error");
        if (st.dabErr(err, err["why"].str())) in.dabOffNew = true;
        const Json& j = s.msg("dab");
        const double sid = j["sid"].num(0);
        const Json* play = nullptr;
        for (auto& sv : j["services"].a) if (sv["sid"].num(-1) == sid) { play = &sv; break; }
        const std::string sig = numSig(j, "sid") + j["dls"].str() + (play ? (*play)["label"].str() + (*play)["dls"].str() + (*play)["codec"].str() : "")
            + numSig(j, "frames") + numSig(j, "fibOk");
        if (st.dab(j, sig) && !in.dabOffNew) {
            in.dabNew = true;
            in.dabDls = j["dls"].str();
            if (play) { in.dabSvcLabel = (*play)["label"].str(); in.dabSvcDls = (*play)["dls"].str(); in.dabSvcCodec = (*play)["codec"].str(); in.dabSvcSid = (*play)["sid"].num(0); }
        }
    }
    // drm — the station's SDC label + text message, and the audio it sends ("AAC · 12 kHz | 8 kbit/s"); a report after
    // leaving DRM is a ghost (the web's onDrm), an unlocked one ends the line
    {
        const Json& j = s.msg("drm");
        const bool live = s.mode == "drm" && j["locked"].boolean() && !trim(j["label"].str()).empty();
        std::string audio;
        if (live && !j["codec"].str().empty()) {
            audio = j["codec"].str();
            char b[64];
            if (j["audioRate"].num(0) > 0) { std::snprintf(b, sizeof b, " \xc2\xb7 %g kHz", j["audioRate"].num() / 1000.0); audio += b; }
            if (j["bitrate"].num(0) > 0) {
                std::snprintf(b, sizeof b, "%.1f", j["bitrate"].num() / 1000.0);
                std::string br = b;
                if (br.size() > 2 && br.compare(br.size() - 2, 2, ".0") == 0) br.resize(br.size() - 2);
                audio += " | " + br + " kbit/s";
            }
        }
        const std::string sig = live ? j["label"].str() + "\x1f" + j["text"].str() + "\x1f" + j["serviceId"].str() + j["country"].str() + audio : "off";
        if (st.drm(j, sig)) {
            in.drmNew = true; in.drmLive = live;
            if (live) {
                in.drmLabel = j["label"].str(); in.drmText = j["text"].str(); in.drmAudio = audio;
                const std::string c = j["country"].str();
                in.drmCountry = c.size() == 2 ? std::string{(char)std::toupper((unsigned char)c[0]), (char)std::toupper((unsigned char)c[1])} : "";
                in.drmSid = (double)std::strtoul(j["serviceId"].str().c_str(), nullptr, 16);
            }
        }
    }
    // tone
    {
        const Json& j = s.msg("tone");
        if (st.tone(j, j["label"].str())) { in.toneNew = true; in.toneLabel = j["label"].str(); }
    }
    // notice: `why` = a refusal (an `ask` refusal is dropped), `vts` = a server line for the bar; `text` is the owner's
    // standing notice, which is not the bar's
    {
        const Json& j = s.msg("notice");
        const std::string why = j["why"].str(), vt = j["vts"].str();
        if (st.notice(j, why + "\x1f" + vt + "\x1f" + j["text"].str())) {
            if (!why.empty()) { if (!j["ask"].boolean()) { in.noticeNew = true; in.noticeText = why; } }
            else if (!vt.empty()) { in.noticeNew = true; in.noticeText = vt; }
        }
    }
    // device: the radio is lent out and another program still has it
    {
        const Json& j = s.msg("device");
        if (st.device(j, numSig(j, "present") + numSig(j, "inUseElsewhere") + j["radio"].str())
            && j["present"].kind == Json::Bool && !j["present"].b && j["inUseElsewhere"].boolean() && !in.noticeNew) {
            std::string nm = trim(j["radio"].str());
            if (nm.empty()) nm = "This radio";
            in.noticeNew = true;
            in.noticeText = nm + " is currently in use with another app on this server and is not available, please try again later.";
        }
    }
    // the radio, for the app's own notices
    const Json& hw = s.msg("hwinfo");
    in.driver = s.hwDriver;
    in.vibeAgc = s.agc;
    in.hasTunerBw = hw.has("tunerBw");
    in.tunerBwAuto = hw["tunerBwAuto"].kind == Json::Bool && hw["tunerBwAuto"].b;
    in.gains = s.gains;
    in.gainNow = s.gainNow;
    if (in.gainNow < 0) in.vibeAgc = true;     // ★ an old server's "-1 = auto" (onHwGainNow)
    in.hackrf = s.hwDriver == "hackrf";
    in.hackLna = (int)s.hw("lna").num(0); in.hackVga = (int)s.hw("vga").num(0);
    in.dsLive = hw.has("autoDs") ? (int)hw["ds"].num(0) : 0;
    in.rspAgcInit = s.msg("rspstat")["agcInit"].num(0) == 1;
    return in;
}

// ── drawing ─────────────────────────────────────────────────────────────────────────────────────────────────────────

inline Rgba fade(Rgba c, float a) { c.a *= a; return c; }

/** A glyph the font lacks is swapped for the nearest it has (Atkinson has no box-drawing bar). */
inline std::string fitGlyphs(State& st, const Font& f, const std::string& s) {
    if (!f.get()) return s;
    std::u32string u = u32(s);
    bool changed = false;
    for (auto& c : u) {
        if (c < 0x80) continue;
        auto key = std::make_pair((const void*)f.get(), c);
        auto it = st.glyphs.find(key);
        if (it == st.glyphs.end()) {
            if (st.glyphs.size() > 4096) st.glyphs.clear();
            it = st.glyphs.emplace(key, TTF_GlyphIsProvided32(f.get(), (Uint32)c) != 0).first;
        }
        if (it->second) continue;
        const char32_t sub = c == 0x2502 || c == 0x2503 ? U'|' : c == 0x2013 || c == 0x2014 ? U'-' : c == 0x2026 ? U'.' : 0;
        if (sub) { c = sub; changed = true; }
    }
    return changed ? u8(u) : s;
}

/** A coverage mask, tinted (built once per mask into a GL_ALPHA-as-RGBA texture). */
inline void drawMask(Gfx& g, const uint8_t* a, int w, int h, float x, float y, Rgba c) {
    static std::map<const uint8_t*, GLuint> texs;
    GLuint& t = texs[a];
    if (!t) {
        std::vector<uint8_t> px((size_t)w * h * 4);
        for (int i = 0; i < w * h; i++) { px[(size_t)i * 4] = px[(size_t)i * 4 + 1] = px[(size_t)i * 4 + 2] = 255; px[(size_t)i * 4 + 3] = a[i]; }
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    g.image(t, std::round(x), std::round(y), (float)w, (float)h, c);
}

/** AnnunciatorLegend.tsx's letters: strokes on a 10 × 14 cell, 2.5 wide, butt-ended. */
constexpr float kCellW = 10, kCellH = 14, kGap = 2.2f, kStroke = 2.5f;
inline float legendW(float h) { return (2 * kCellW + kGap) * h / kCellH; }
inline void drawLegend(Gfx& g, const char* nm, float x, float y, float h, Rgba c) {
    const float k = h / kCellH, sw = kStroke * k, hs = kStroke / 2;
    auto seg = [&](float ox, float x1, float y1, float x2, float y2) { g.line(x + (ox + x1) * k, y + y1 * k, x + (ox + x2) * k, y + y2 * k, sw, c); };
    auto quad = [&](float ox, float x0, float y0, float cx, float cy, float x1, float y1) {
        float px = x0, py = y0;
        for (int i = 1; i <= 6; i++) {
            const float t = i / 6.f, u = 1 - t;
            const float qx = u * u * x0 + 2 * u * t * cx + t * t * x1, qy = u * u * y0 + 2 * u * t * cy + t * t * y1;
            seg(ox, px, py, qx, qy); px = qx; py = qy;
        }
    };
    for (int i = 0; i < 2; i++) {
        const float ox = i * (kCellW + kGap);
        switch (nm[i]) {
            case 'T': seg(ox, 0, hs, kCellW, hs); seg(ox, kCellW / 2, hs, kCellW / 2, kCellH); break;
            case 'P':
                seg(ox, 1 + hs, 0, 1 + hs, kCellH);
                seg(ox, 1 + hs, hs, 6, hs);
                quad(ox, 6, hs, kCellW - hs, hs, kCellW - hs, 4.4f);
                quad(ox, kCellW - hs, 4.4f, kCellW - hs, 8.2f - hs, 6, 8.2f - hs);
                seg(ox, 6, 8.2f - hs, 1 + hs, 8.2f - hs);
                break;
            case 'A': seg(ox, hs, kCellH, kCellW / 2, hs); seg(ox, kCellW / 2, hs, kCellW - hs, kCellH); seg(ox, 2.6f, 9.6f, kCellW - 2.6f, 9.6f); break;
            case 'F': seg(ox, 1 + hs, 0, 1 + hs, kCellH); seg(ox, 1 + hs, hs, kCellW, hs); seg(ox, 1 + hs, 7, kCellW - 2, 7); break;
            default: break;
        }
    }
}

// sizes on the 640×480 glass (the app's points × ~1.15; text one step up, for the screen's size — Stuart's rule)
constexpr float kRadius = 10, kPadH = 10, kArrowW = 9, kArrowH = 11, kArrowPad = 4, kNameMargin = 6;
constexpr float kLegendH = 11, kLegendGap = 5, kAnnunML = 6, kAnnunMR = 2, kMarkMR = 5, kIsoMR = 3;

}  // namespace vts

/** The VTS bar in `r` (see the header for what and when). Call once a frame while the receiver screen is up, after the
 *  waterfall and before the sheets. Draws nothing while the bar has nothing to say. */
inline void App::drawVts(Ctx& c, const RadioState& s, Rect r) {
    using namespace vts;
    State& st = vtsState();
    const double now = c.t * 1000.0;
    const Inputs in = readInputs(st, s, now, region());
    st.screen.update(in);
    const uint64_t ended = st.bar.update(st.screen.notif(), now);
    if (ended) { st.screen.timedEnd(ended); st.bar.update(st.screen.notif(), now); }
    const auto& sh = st.bar.shown();
    if (!sh || !c.f.body.get() || !c.f.small.get()) return;
    const Notif& n = *sh;
    const float a = (float)std::max(0.0, std::min(1.0, st.bar.opacity()));
    if (a <= 0.004f) return;

    // the bar
    c.g.rrect(r.x, r.y, r.w, r.h, kRadius, fade(col::bg, a));
    c.g.rframe(r.x, r.y, r.w, r.h, kRadius, 1, fade(col::border, a));

    const bool onTune = n.kind == Notif::StationOn;
    const bool isBand = n.kind == Notif::Band || n.kind == Notif::Notice;
    const Rgba nameCol = isBand ? col::band : col::onTune;
    const Rgba arrowCol = onTune ? col::onTune : col::dim;     // no tune direction here: never a bookmark guide
    const float cy = r.cy();

    // ► mirrored, and ►
    float x = r.x + kPadH;
    c.g.triangle(x + kArrowPad, cy, x + kArrowPad + kArrowW, cy - kArrowH / 2, x + kArrowPad + kArrowW, cy + kArrowH / 2, fade(arrowCol, a));
    x += kArrowPad * 2 + kArrowW;
    float xr = r.r() - kPadH - (kArrowPad * 2 + kArrowW);
    c.g.triangle(xr + kArrowPad, cy - kArrowH / 2, xr + kArrowPad, cy + kArrowH / 2, xr + kArrowPad + kArrowW, cy, fade(arrowCol, a));

    // the side blocks: the left (mark · country) and the right (TP · TA · AF) reserve the wider one's width when the
    // cluster is there, so the line's window sits on the strip's centre (VTSBar centreSides)
    const Font& isoF = c.f.tiny;
    const std::string iso = n.iso;
    float leftW = 0;
    const bool rdsMark = n.badge == "RDS" && !n.dab;
    if (rdsMark) leftW += kRdsMarkW + kMarkMR;
    if (n.dab) leftW += kDabMarkW + kMarkMR;
    const float isoW = iso.empty() ? 0.f : (float)c.tc.width(isoF, iso) + 8;
    if (!iso.empty()) leftW += isoW + kIsoMR;
    const float rightW = n.annunciators ? kAnnunML + 3 * legendW(kLegendH) + 2 * kLegendGap + kAnnunMR : 0.f;
    const float sideW = std::max(leftW, rightW);
    const float leftBlock = n.annunciators ? sideW : leftW, rightBlock = n.annunciators ? sideW : 0.f;
    {
        float bx = x;
        if (rdsMark) { drawMask(c.g, kRdsMark, kRdsMarkW, kRdsMarkH, bx, cy - kRdsMarkH / 2.f, fade(col::onTune, a)); bx += kRdsMarkW + kMarkMR; }
        if (n.dab) {
            const float my = cy - kDabMarkH / 2.f;
            drawMask(c.g, kDabMarkMain, kDabMarkW, kDabMarkH, bx, my, fade(col::onTune, a));
            drawMask(c.g, kDabMarkPlus, kDabMarkW, kDabMarkH, bx, my, fade(n.dabPlus ? col::onTune : col::ghost, a));
            bx += kDabMarkW + kMarkMR;
        }
        if (!iso.empty()) {
            // ★ the app draws the country's FLAG here; this glass has no colour emoji, so the code — as the app's VFD strip
            const float h = (float)isoF.lineHeight() + 2;
            c.g.rframe(bx, cy - h / 2, isoW, h, 4, 1, fade(col::sub, a));
            c.tc.draw(c.g, isoF, iso, bx + isoW / 2, cy - isoF.lineHeight() / 2.f, fade(col::sub, a), 1);
        }
    }
    if (n.annunciators) {
        float lx = xr - rightBlock + (rightBlock - rightW) + kAnnunML;   // right-aligned in its block
        const char* nm[3] = {"TP", "TA", "AF"};
        const bool lit[3] = {n.tp, n.ta, n.af};
        for (int i = 0; i < 3; i++) {
            drawLegend(c.g, nm[i], lx, cy - kLegendH / 2, kLegendH, fade(lit[i] ? col::core : col::ghost, a));
            lx += legendW(kLegendH) + kLegendGap;
        }
    }

    // the line's window
    const Rect area{x + leftBlock + kNameMargin, r.y + 2, (xr - rightBlock - kNameMargin) - (x + leftBlock + kNameMargin), r.h - 4};
    if (area.w <= 8) return;
    struct Run { std::string s; const Font* f; Rgba c; float w; };
    std::vector<Run> runs;
    for (auto& sg : vtsLineSegments(n.id, n.idLabel, n.name, n.rt)) {
        const Font* f = sg.kind == Seg::Id ? &c.f.small : &c.f.body;
        const Rgba cc = sg.kind == Seg::Id || sg.kind == Seg::Sep ? col::sub : nameCol;
        runs.push_back({fitGlyphs(st, *f, sg.s), f, cc, 0});
    }
    if (!n.secondary.empty()) runs.push_back({fitGlyphs(st, c.f.small, "  \xE2\x94\x82  " + n.secondary), &c.f.small, col::sub, 0});
    float textW = 0;
    for (auto& rn : runs) { rn.w = (float)c.tc.width(*rn.f, rn.s); textW += rn.w; }
    if (textW <= 0) return;
    const double slide = st.bar.slideX(textW, area.w);
    float tx = textW <= area.w ? area.x + (area.w - textW) / 2 : area.x + (float)std::round(slide);
    // one baseline for every run (nested Text in the app)
    const float baseline = std::round(cy - c.f.body.lineHeight() / 2.f) + (float)TTF_FontAscent(c.f.body.get());
    c.g.clip(area.x, area.y, area.w, area.h);
    for (auto& rn : runs) {
        if (tx + rn.w >= area.x && tx <= area.r())
            c.tc.draw(c.g, *rn.f, rn.s, std::round(tx), baseline - (float)TTF_FontAscent(rn.f->get()), fade(rn.c, a));
        tx += rn.w;
    }
    c.g.unclip();
}

}  // namespace hh
