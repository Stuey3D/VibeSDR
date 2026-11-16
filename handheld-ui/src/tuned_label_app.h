// tuned_label_app.h — the TUNED STATION'S LABEL wired into the R36S's main screen (tuned_label.h is the port of the app's
// src/utils/tunedStation.ts; this is the plumbing and the drawing): which lists it reads, when it is worked out again, and
// the one call drawOverlays makes.
//
// ★★★ STUART (2026-11-15): "When the VTS disappears can we have a label on the VFO with the current station name" —
//     "[User] Flex FM", "[Server] Radio Caroline", "[EiBi] China Radio Int", CENTRED on the dial's needle at the top of
//     the waterfall (Stuart, 2026-11-16: "centre the frequency and bookmark reading on the VFO"; shifted inward near an
//     edge). The R36S draws no LISTEN readout (the frequency is the pill's) and its signal labels sit on the spectrum's
//     rows above, so nothing here covers either.
// ★ THE LISTS are the VTS's own (vts.h vtsStations): the rows the owner SAVED on this receiver = [User] (the handheld is
//   its listener's own radio — the app's "your bookmarks"), the stations it HEARD = [Server], the EiBi schedule the
//   owner imported = [EiBi], only while on air by the hours in its comment. Nothing extra is fetched: the VTS keeps them.
// ★ WHEN: the dial has rested tl::kSettleMs (a stick sweep never flickers a name per channel), the VTS bar has let the
//   name go and no live RDS / DAB / DRM name is held (vts.h vtsTunedInputs — the bar's own Screen). Worked out only when
//   one of those, a list or the minute changes; a frame that changes none costs one box and its glyphs.
// ★★ A SUPPLEMENT, NEVER A REPEAT (Stuart, 2026-11-15 — this file's first render showed "Flex FM" on the spectrum and
//    "[User] Flex FM" under it): the SIGNAL LABELS drawn this frame (known_labels_app.h, knownLabelsUi().k.placed() —
//    empty when they are off) are handed over (tunedPickNow, called between their layout and their drawing). A station
//    they already name gets NO tagged label and no other name in its place: THAT label's name is drawn here, centred on
//    the needle in the tuned colour, and not on the spectrum as well (2026-11-16, as the app and the web do).
// ★ RECEIVED, not only dead on: matched inside the receive passband (s.bwLow / s.bwHigh, the demodulator's filter).
#pragma once
#include "bm_state.h"
#include "disp_settings.h"
#include "known_labels_app.h"
#include "tuned_label.h"

#include <ctime>
#include <mutex>

namespace hh {

/** The VTS's side of it (defined in vts.h, after the bar): the live station name it holds, and whether its bar is showing
 *  a station's name right now. */
inline void vtsTunedInputs(std::string& liveName, bool& showing);

struct TunedLabelUi {
    double hz = -1, restAt = 0;      // the dial last seen, and when it counts as rested (s)
    std::string key, text;           // what `text` was worked out for
    int highlight = -1;              // the placed SIGNAL LABEL naming the tuned station (drawn in the tuned colour), −1 none
    double pickedAt = -1;            // the frame (Ctx::t) the pick was last made for
};
inline TunedLabelUi& tunedLabelUi() { static TunedLabelUi u; return u; }

/** The candidates inside the passband only (the schedule is ~9000 rows; only the few near the dial are copied). */
inline std::vector<tl::Row> tunedRowsNear(double hz, const tl::Passband& pb) {
    std::vector<tl::Row> out;
    BmShared& b = bmShared();
    std::lock_guard<std::mutex> lk(b.m);
    auto near = [&](double f) { return tl::inPassband(f, hz, pb); };
    for (const BmRow& r : b.rows) if (r.manual && near(r.frequency)) out.push_back({r.frequency, r.name, "user", "", r.mode});
    for (const BmRow& r : b.rows) if (!r.manual && near(r.frequency)) out.push_back({r.frequency, r.name, "server", "", r.mode});
    for (const StationRow& r : b.stations) if (near(r.frequency)) out.push_back({r.frequency, r.name, "eibi", r.comment, r.mode});
    return out;
}

/** ★★ This frame's choice (tl::tunedPick) against the SIGNAL LABELS placed this frame: the tuned label's text, or the
 *  index of the placed label to draw in the tuned colour (never both). Called by knownLabelsDraw between its layout and
 *  its drawing (so the highlight lands on the right label in the same frame), else by tunedLabelDraw with none placed.
 *  Returns the highlight (−1 none). */
inline int tunedPickNow(Ctx& c, const RadioState& s, const std::vector<PlacedLabel>& placed) {
    TunedLabelUi& u = tunedLabelUi();
    const double now = c.t;
    u.pickedAt = now;
    if (s.vfoHz != u.hz) { u.hz = s.vfoHz; u.restAt = now + tl::kSettleMs / 1000.0; }
    if (!(s.vfoHz > 0) || !(s.spanHz > 0) || now < u.restAt) { u.text.clear(); u.highlight = -1; u.key.clear(); return -1; }
    std::string live; bool showing = false;
    vtsTunedInputs(live, showing);
    uint64_t bs, ss;
    { BmShared& b = bmShared(); std::lock_guard<std::mutex> lk(b.m); bs = b.seq; ss = b.stationsSeq; }
    const std::time_t tt = std::time(nullptr);
    std::tm g{};
    gmtime_r(&tt, &g);
    const int nowMin = g.tm_hour * 60 + g.tm_min;
    const tl::Passband pb = tl::tunedPassband(s.mode, s.bwLow, s.bwHigh);
    std::vector<tl::Shown> shown;
    std::string shownKey;
    for (const PlacedLabel& p : placed) if (p.sig) {
        shown.push_back({p.sig->hz, p.sig->dialHz, p.sig->src});
        shownKey += std::to_string((long long)std::llround(p.sig->hz)) + p.sig->src + ",";
    }
    const std::string key = std::to_string((long long)std::llround(s.vfoHz)) + "|" + std::to_string(bs) + "|" + std::to_string(ss) + "|"
                          + std::to_string(nowMin) + "|" + live + "|" + (showing ? "1" : "0") + "|" + std::to_string((int)pb.lo) + ":"
                          + std::to_string((int)pb.hi) + "|" + shownKey;
    if (key != u.key) {
        u.key = key;
        tl::Pick pk;
        if (!showing && tl::trim(live).empty()) pk = tl::tunedPick(tunedRowsNear(s.vfoHz, pb), s.vfoHz, nowMin, live, showing, true, pb, &shown);
        u.text = pk.label; u.highlight = pk.highlight;
    }
    return u.highlight;
}

/** Called from drawOverlays with the waterfall's rect: the label centred on the needle at its top, or nothing. */
inline void tunedLabelDraw(Ctx& c, const RadioState& s, Rect wfr, const unsigned char* lut) {
    TunedLabelUi& u = tunedLabelUi();
    if (u.pickedAt != c.t) tunedPickNow(c, s, {});   // the SIGNAL LABELS are off (or not drawn): nothing placed
    // ★★ a signal label already names the station: ITS name is drawn here, centred on the needle, and not on the
    //    spectrum (known_labels.h draw skips it) — once, on the VFO, as the app and the web draw it
    std::string text = u.text;
    if (text.empty() && u.highlight >= 0) {
        const std::vector<PlacedLabel>& pl = knownLabelsUi().k.placed();
        if ((size_t)u.highlight < pl.size() && pl[u.highlight].sig) text = pl[u.highlight].sig->label;
    }
    if (text.empty() || !c.f.small.get()) return;
    const double lo = s.viewCentreHz - s.spanHz / 2;
    const float nx = wfr.x + (float)((s.vfoHz - lo) / s.spanHz) * wfr.w;
    if (nx < wfr.x || nx > wfr.r()) return;
    const Font& f = c.f.small;
    const float pad = 4, bh = (float)f.lineHeight() + 2;
    const float bw = std::min(wfr.w - 4, (float)c.tc.width(f, text) + 2 * pad);
    const float bx = (float)tl::labelX(nx - wfr.x, bw, wfr.w) + wfr.x, by = wfr.y + 3;
    const unsigned col = tl::stampColour(dispSet().needle, lut);
    c.g.rect(bx, by, bw, bh, rgb(0x000000, 0.6f));
    c.g.clipPush(bx, by, bw, bh);
    c.tc.draw(c.g, f, text, bx + pad + 1, by + 2, rgb(0x000000, 0.95f));   // the dark shadow, one pixel down-right
    c.tc.draw(c.g, f, text, bx + pad, by + 1, rgb(col));
    c.g.clipPop();
}

}  // namespace hh
