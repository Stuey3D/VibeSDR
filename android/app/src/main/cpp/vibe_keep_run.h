// vibe_keep_run.h — THE LANDING MAP AND ITS KEEP-RUNNING MODES (V12 beta96, Stuart 11-25/26; docs/v12/KEEP-RUNNING.md).
//
// ★★★ STUART (the Sony's Setup page, 11-26): "I thought I'd be able to set VDL2 as the Sony's background mode". beta95 had one
//     answer: "Use ADS-B as the landing page" + "Keep ADS-B running when nobody is connected". Generalised (memory
//     map_landing_toggles.md):
//   · The owner picks a LANDING MAP per radio — the AVIATION & MARITIME map (the client maps' b96 merge) — and which of its
//     modes the radio sits in: ADS-B, VDL2, ACARS, the VOR / ILS map, AIS. "Keep it running when nobody is connected" keeps
//     that mode on with nobody here, so a newcomer finds a populated map.
//   · GATED BY THE RADIO'S RATE, AS THE SERVER SAYS (feedback_app_no_hardcoded_settings): at an ordinary rate ONE mode at a
//     time ("for a one radio server you can only choose 1 at a time"), the others greyed WITH THE REASON; a combination only
//     where one capture holds both. The one combination a single whole-radio mode decodes today is ACARS + VDL2 (the
//     datalink's wide band, ~6.1 MS/s — vibe_avi_mode.h). ✗ AIS + DSC is NOT offered: DSC is a VFO decoder (background
//     decoding's), not a whole-radio mode, so ticking both could only half-work (AGENTS.md: no dead controls).
//   · Picking VDL2 is THE WHOLE SETUP: the centre and every 25 kHz channel come from the datalink's own table
//     (vibeavi::bands) — no frequency, no VFO typed by the owner.
//   · Each radio of a multi-radio server has its own (the config is per radio).
//   · MIGRATION: a radio with beta95's landingAdsb / adsbKeepRunning and no landingMap becomes map + "adsb" + keep exactly
//     as it was; the writer mirrors the ADS-B case back into the old two fields, so an older build reads the same thing.
//
// ★★★ THE SHARED 137 MHz CAPTURE (Stuart 11-25 22:57–23:30): a VDL2 radio's capture at 2.4 / 2.56 MS/s also holds ORBCOMM
//     (137.2–137.8) and a Meteor LRPT / NOAA APT downlink (137.1 / 137.9 + Doppler). shareFit says where the tuner sits so
//     that the kept VDL2 channels AND the satellite work fit one capture with the DC clear of every signal that is not
//     ORBCOMM's — the same fitCapture the beta94 pass scheduler uses (vibe_sat_pass_sched.h). The shim acts on it.
//
// Pure: no radio, no clock — vibeserver/test-keep-run.cpp drives every rule here, and the Setup page's keepGate() is the
// same rule in JavaScript (scripts/test-setup-keep-run.mjs runs the same case table).
#pragma once

#include "vibe_avi_mode.h"
#include "vibe_keep_store.h"
#include "vibe_sat_pass_sched.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vibekeep {

/** The rate the combination needs (Hz) — the wide band's own figure, so the page and the mode cannot disagree. */
inline double comboMinRateHz() { const vibeavi::Band* b = vibeavi::bandById("acars+vdl2"); return b ? b->minRateHz() : 6.1e6; }

// ══ THE RATE GATE — what the page may tick, and why not ═════════════════════════════════════════════════════════════════
/** What the SERVER says of one mode on this radio: may it run here with these settings ("" = yes), and its cost. */
struct Avail {
    std::string id;
    bool hw = true;          ///< false = this radio can never do it (an HF+ at 1090 MHz): the row is not drawn at all
    std::string why;         ///< hw but cannot now (blocked, the rate): said beside the greyed row
    double benchPct = -1;    ///< % of one core from the setup benchmark (−1 = not measured)
};
struct Offer {
    std::string id, name, group;
    bool drawn = true;       ///< false = not offered on this radio (hw false)
    bool enabled = true;     ///< may be ticked now
    bool on = false;         ///< ticked
    std::string why;         ///< why it is greyed
    double benchPct = -1;
};
inline std::string rateWords(double hz) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", hz / 1e6);   // 2.048 · 2.4 · 2.56 · 8
    std::string s = b;
    if (s.find('.') != std::string::npos) { while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); }
    return s + " MS/s";
}
/** ★★★ THE RULE. `rateHz` the capture the radio will run at; `comboOk` the combination's capture fits (the running rate,
 *  or the owner's 8 MS/s switch — the server's vsAvdataReach on the wide band); `comboWhy` its words when not. With
 *  nothing ticked every mode the radio can run is enabled. With one ticked, the others are greyed — "2.4 MS/s: one mode
 *  at a time on this radio" — except the other half of ACARS + VDL2 where the capture holds both. */
inline std::vector<Offer> gate(const std::vector<Avail>& avail, bool comboOk, const std::string& comboWhy, double rateHz,
                               const std::vector<std::string>& chosenIn) {
    std::vector<std::string> chosen;
    for (const auto& id : parseCsv(toCsv(chosenIn))) {
        for (const auto& a : avail) if (a.id == id && a.hw && a.why.empty()) chosen.push_back(id);
    }
    if (chosen.size() > 1 && !(isCombo(chosen) && comboOk)) chosen.resize(1);
    std::vector<Offer> out;
    for (const auto& m : modes()) {
        Offer o; o.id = m.id; o.name = m.name; o.group = m.group;
        const Avail* a = nullptr;
        for (const auto& x : avail) if (x.id == m.id) a = &x;
        if (!a || !a->hw) { o.drawn = false; o.enabled = false; out.push_back(o); continue; }
        o.benchPct = a->benchPct;
        o.on = std::find(chosen.begin(), chosen.end(), o.id) != chosen.end();
        if (!a->why.empty()) { o.enabled = false; o.why = a->why; out.push_back(o); continue; }
        if (o.on || chosen.empty()) { out.push_back(o); continue; }
        // something else is ticked
        const bool partner = chosen.size() == 1
            && ((chosen[0] == acarsVdl2().a && o.id == acarsVdl2().b) || (chosen[0] == acarsVdl2().b && o.id == acarsVdl2().a));
        if (partner && comboOk) { out.push_back(o); continue; }
        o.enabled = false;
        const Mode* c0 = modeById(chosen[0]);
        if (partner) o.why = comboWhy.empty() ? std::string("ACARS and VDL2 together need a capture of ")
                                                + rateWords(std::ceil(comboMinRateHz() / 1e5) * 1e5) + " or more" : comboWhy;
        else if (chosen.size() == 2) o.why = "ACARS + VDL2 already take this radio's capture";
        else o.why = (rateHz > 0 ? rateWords(rateHz) + ": " : std::string()) + "one mode at a time on this radio \xe2\x80\x94 "
                   + (c0 ? c0->name : chosen[0].c_str()) + " is ticked";
        out.push_back(o);
    }
    return out;
}
/** The set a save may store: ticked AND runnable, one mode or the combination (where it fits) — the first otherwise. */
inline std::vector<std::string> normalise(const std::vector<std::string>& chosen, const std::vector<Avail>& avail, bool comboOk) {
    std::vector<std::string> v;
    for (const auto& o : gate(avail, comboOk, "", 0, chosen)) if (o.on) v.push_back(o.id);
    return v;
}

// ══ THE SHARED 137 MHz CAPTURE ══════════════════════════════════════════════════════════════════════════════════════════
/** The kept VDL2 channels as a region the capture must hold — every channel ± 12.5 kHz, the DC never inside (the band's own
 *  centre sits BETWEEN channels, but a shared capture puts it outside the block altogether: simpler, and no channel near it). */
inline vibe::satsched::Region vdl2Region() {
    vibe::satsched::Region r;
    const vibeavi::Band* b = vibeavi::bandById("vdl2");
    r.lo = b ? b->lo() : 136.6875e6; r.hi = b ? b->hi() : 136.9875e6; r.dcOk = false;
    return r;
}
struct Share {
    bool ok = false;
    double centreHz = 0;      ///< the tuner's centre for everything held
    double lo = 0, hi = 0;    ///< the span held (for the IF filter)
    std::string why;          ///< when !ok
};
/** Where one capture of `usableHz` holds kept VDL2 plus ORBCOMM (`orbcomm`) plus a pass (`pass`, may be null) — fitCapture's
 *  rule (DC 10 kHz clear of every non-ORBCOMM signal, nearest the middle). */
inline Share shareFit(double usableHz, bool orbcomm, const vibe::satsched::Region* pass) {
    std::vector<vibe::satsched::Region> rs = { vdl2Region() };
    if (orbcomm) rs.push_back(vibe::satsched::orbcommRegion());
    if (pass) rs.push_back(*pass);
    const vibe::satsched::Fit f = vibe::satsched::fitCapture(usableHz, rs);
    Share s;
    s.ok = f.ok; s.centreHz = f.centreHz; s.why = f.why;
    s.lo = 1e18; s.hi = -1e18;
    for (const auto& r : rs) { s.lo = std::min(s.lo, r.lo); s.hi = std::max(s.hi, r.hi); }
    return s;
}
/** Half the IF filter width a capture centred at `centreHz` needs to hold [lo, hi] (and no less than `floorHalf`). */
inline double ifHalfFor(double centreHz, double lo, double hi, double floorHalf = 250000.0) {
    return std::max(floorHalf, std::max(std::fabs(centreHz - lo), std::fabs(hi - centreHz)));
}

// ══ THE COST LINE (from the setup benchmark — it sets the default, it never hides) ═══════════════════════════════════════
/** "about 9 % of one core (setup benchmark)" · "" when not measured. `heavy` = more than half a core: the page warns. */
inline std::string costWords(double pct, bool* heavy = nullptr) {
    if (heavy) *heavy = pct > 50.0;
    if (!(pct >= 0)) return std::string();
    char b[96];
    if (pct < 1.0) std::snprintf(b, sizeof b, "under 1 %% of one core (setup benchmark)");
    else std::snprintf(b, sizeof b, "about %.0f %% of one core (setup benchmark)", pct);
    return b;
}

}  // namespace vibekeep
