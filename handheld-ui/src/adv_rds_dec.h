// adv_rds_dec.h — ADVANCED RDS hosted by the DECODER BOX (dec_state.h's registry, dec_box.h): the web's home for it —
// its decoders panel, VHF · BROADCAST (src/utils/decoderBands.ts). The readings, the subscription and the drawing are
// adv_rds.h's; this file only plugs them into the box: the key in the picker, the box's body, its header keys
// (SUMMARY · ALL · PLOTS beside the box's own DECODERS · CLR · STOP), and the tall panel for ALL / PLOTS.
// ★ Include after dec_box.h and adv_rds.h; call ardsRegisterDecoder(*this) once in App::run before the loop. Then the
//   standalone wiring (App::S_ARDS, the mode picker's ADV RDS, ardsDrawBox) is not needed: with the layer set to "dec"
//   the mode-picker entry and the standalone box both stand down by themselves.
// ★ Exclusive with the text decoders (the registry's rule), and off at once on STOP (ardsClose sends rdsx on:0 itself —
//   the box stops ticking a decoder that is not running).
#pragma once
#include "adv_rds.h"
#include "dec_box.h"

namespace hh {

inline void ardsRegisterDecoder(App& app) {
    ArdsUi& u = ardsUi();
    u.layer = "dec";
    u.onEnded = [] { decExternalEnded("rds"); };
    DecExternal e;
    e.id = "rds"; e.label = "ADV RDS"; e.band = 1; e.group = "BROADCAST";
    // ★ where it works: WFM on a running radio the owner has not blocked it on (the app offers it in WFM only)
    e.offered = [](const RadioState& s) { return ardsOffered(s); };
    e.setOn = [](App& a, const RadioState&, bool on) {
        ArdsUi& v = ardsUi();
        if (on) { ardsOpen(a); v.grabNext = false; }   // (the decoder box does its own grabbing)
        else ardsClose(a);
    };
    e.tick = [](App& a, const RadioState& s) {
        ardsTick(a, s);
        if (DecExternal* x = decExt("rds")) x->tall = ardsUi().view != ArdsUi::V_SUMMARY;   // ALL / PLOTS = the tall panel
    };
    e.status = [&app] { return ardsStatus(app); };
    e.live = [] { const ArdsUi& v = ardsUi(); return v.lastAt > 0 && nowSec() - v.lastAt < 1.5; };
    e.draw = [](App& a, Ctx& c, const RadioState& s, Rect body) { ardsDrawBody(a, c, s, body, decGrabbed(a)); };
    e.keys = [](App&, const RadioState&, std::vector<std::string>& labels, std::vector<std::function<void()>>& press) {
        static const char* names[] = {"SUMMARY", "ALL", "PLOTS"};
        for (int i = 0; i < 3; i++) {
            if (i == ardsUi().view) continue;   // (the view you are on needs no key: the box has little header room)
            labels.push_back(names[i]);
            press.push_back([i] { ArdsUi& v = ardsUi(); v.view = i; v.scrollY = 0; });
        }
    };
    e.hint = "the engine's analyser runs only while this is on \xC2\xB7 CPU shown in the box's header";
    decRegister(std::move(e));
}

}  // namespace hh
