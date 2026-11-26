// vibe_sat_engine.h — V12 SATELLITE ENGINE (docs/v12/SATELLITE-ENGINE.md): the curated satellite list, element sets
// from CelesTrak's OMM JSON, the server's own sky (passes, look angles), and the DOPPLER LOCK — a listener's (or the
// shared dial's) channel following the predicted Doppler, trimmed by a slow closed loop on FM repeaters.
//
// ★★★ COMPLEXITY IN THE ENGINE, NOT THE SETTINGS (brief §1): one button. The dial shows the NOMINAL (published)
//     frequency; the channel sits at nominal + shift. The shift is applied by the caller inside the channel's own NCO
//     (RxPipeline::setTune on the same mode and width = a phase-continuous NCO move, no rebuild, no AGC reset) — never by
//     retuning the radio, never through the tune pacer, never as a "user tune" (the SHARED-DIAL CONTRACT: the server's
//     own moves are not tunes; brief §4.2 TRAP).
// ★★★ SMOOTH BY CONSTRUCTION: the model is a continuous function of time evaluated every tick (25 Hz → at most ~9 Hz a
//     step at 70 cm overhead, under the FM channel's own noise); the loop's trim is slew-limited (kTrimSlewHzS) and
//     clamped (kTrimClampHz — the capture-range TRAP: without it the loop locks to the steady carrier beside ISS).
// ★★ THE LOOP ONLY WHERE IT HELPS: `fll` (FM repeaters — every voice shares the satellite's one Doppler curve), never on
//    a linear transponder (`none` — each uplinker brings their own error; the loop would chase whoever talks), never on
//    `costas` (a digital demodulator does its own fine tracking; the engine is its feed-forward).
// ★★ HOOK FOR DECODERS (LRPT, AX.25 9600): Engine::selected(key) / Engine::dopplerFor(norad, hz, t) — see RESUME HERE.
// ★ Bounded: the curated list is ~20 birds; element sets are kept only for listed birds (plus none other); passes are
//   cached per bird for kPassCacheSec, ≤ 8 a bird.
#pragma once

#include "vibe_sat_orbit.h"
#include "vibe_lrpt_geo.h"   // ★ V12 LRPT map outline: lrptCandidates()

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if __has_include("vibedsp/simd_internal.h")
#include "vibedsp/simd_internal.h"
#endif
#ifndef VIBE_NEON
#define VIBE_NEON 0
#endif
#ifndef VIBE_SSE
#define VIBE_SSE 0
#endif

namespace vibe { namespace sat {

// ── The curated list ──────────────────────────────────────────────────────────────────────────────────────────────
/** One downlink. `hiHz` > 0 = a passband (a linear transponder), else a single channel at `hz`. */
struct Transmitter {
    std::string label;
    double hz = 0, hiHz = 0;
    std::string mode;        // the demodulator the dial should be on: "nfm", "usb", "cw" — or a decoder's name
    std::string loop;        // "fll" | "none" | "costas" (brief §6.2)
    std::string consumer;    // "" = listening; "lrpt", "ax25-9600", "ax25-1200", "fc-bpsk" (FUNcube telemetry, decoded),
                             // "fox-bpsk" (Fox-format telemetry — heard, NOT decoded) = a decoder's
    double baud = 0;
    std::string modeName;    // ★ beta90 1.4: the catalogue's own name for a transponder ("Mode A", "Mode B"), else ""
    double upHz = 0, ctcss = 0;
    std::string note;
    std::string status;      // sats.json: "active" | "inactive" (SatNOGS DB's word) | "" (nobody said)
    bool passband() const { return hiHz > hz; }
    double centre() const { return passband() ? 0.5 * (hz + hiHz) : hz; }
    /** ★★ beta91 SATELLITE LABELS: mode "label" = a downlink that is only ever NAMED on the waterfall (ORBCOMM — commercial
     *  messaging): never offered in SATELLITE mode, never locked, never decoded. */
    bool labelOnly() const { return mode == "label"; }
    /** ★★ V12 2026-11-24 ORBCOMM (docs/v12/ORBCOMM.md): a downlink the ORBCOMM decoder reads (vibe_orbcomm.h) — the
     *  satellite's own broadcast housekeeping decoded, subscriber messages shown RAW (never decoded). */
    bool orbcomm() const { return consumer == "orbcomm" || mode == "orbcomm"; }
    /** ★★★ beta94 THE TRANSPONDER SIGNAL FINDER (docs/v12/TRANSPONDER-FINDER.md — Stuart 11-24, RS-44 with CW and SSB
     *  contacts all over its passband: "open the satellite in iq mode and as soon as a carrier is detected it gives you a
     *  button to listen to it"). A linear transponder and the bird's CW beacon beside it are ONE slice (linkXpdr): the
     *  satellite-frame span both cover (xpdrLo…xpdrHi), the beacon in it (beaconHz, 0 = none), and whether the
     *  transponder inverts (an LSB uplink comes down USB — every linear bird we list does; a note saying "non-inverting"
     *  says otherwise). Set on BOTH the transponder and its beacon: picking either runs the finder over the whole span. */
    double xpdrLo = 0, xpdrHi = 0, beaconHz = 0;
    /** ★★★ beta94 TRANSPONDER + TELEMETRY (Stuart 11-25: "the transponder and telemetry are close together so they can also
     *  be handled as one"): on a TRANSPONDER only — the bird's FUNcube telemetry downlink (fc-bpsk) inside the span, 0 =
     *  none. JO-97 145.840 below 145.855–145.875, AO-73 145.935 below 145.950–145.970. The telemetry downlink itself keeps
     *  its own decoder (an unattended pass of it still runs fctlm); a PICK of it opens the transponder (xpdrOwnerOf). */
    double tlmHz = 0;
    bool inverting = true;
    bool xpdr() const { return xpdrHi > xpdrLo; }
    double xpdrCentre() const { return 0.5 * (xpdrLo + xpdrHi); }
    /** ★★★ beta94 where a transponder lock's NOMINAL (the dial, satellite frame) may sit: its passband — or, for a finder's
     *  span, the span ± kXpdrNominalEdgeHz, so LISTEN can put the dial on the beacon side or 1.5 kHz below the telemetry's
     *  carrier without the lock clamping it back into the passband (the listener's tune there would never "land"). */
    static constexpr double kXpdrNominalEdgeHz = 6000.0;
    double nominalLo() const { return xpdr() ? std::min(hz, xpdrLo - kXpdrNominalEdgeHz) : hz; }
    double nominalHi() const { return xpdr() ? std::max(hiHz, xpdrHi + kXpdrNominalEdgeHz) : hiHz; }
};
struct SatDef {
    std::string id, name;
    long long norad = 0;
    std::string group;       // CelesTrak GP group its elements come from
    std::string note;        // what the listener should know (schedule, sunlight only …)
    std::vector<Transmitter> tx;
};

/** ★★ THE DEFAULT SET — checked 2026-11-19 against AMSAT's live FM / linear lists (FM page; "Live Linear Satellites",
 *  updated 2026-02-21), CelesTrak's current amateur + weather groups (every NORAD number below has a current element
 *  set there) and SatNOGS. Left OUT on purpose: NOAA-15/18/19 (decommissioned 6 June, 13 and 19 August 2025 — NOAA's
 *  POES archive; their 137 MHz APT is off), AO-91 (end of mission), SO-125 (status disputed: SatNOGS says re-entered),
 *  TEVEL-2 (downlinks unconfirmed), CAS-4A / XW-2 (re-entered), QO-100 (geostationary, no Doppler to correct).
 *  ★ Meteor frequencies and modes CHANGE ON ORBIT (brief §6.2 TRAP) — these are today's; a vibesdr.net sats.json
 *  (phase 2) replaces this list. */
inline void linkXpdr(std::vector<SatDef>& defs);   // ★★★ beta94 (below): each transponder + its beacon = one finder span
inline std::vector<SatDef> curatedCatalogue() {
    auto fm = [](const char* label, double hz, const char* note = "", double up = 0, double ctcss = 0) {
        Transmitter t; t.label = label; t.hz = hz; t.mode = "nfm"; t.loop = "fll"; t.upHz = up; t.ctcss = ctcss; t.note = note; return t; };
    auto lin = [](const char* label, double lo, double hi, double upLo = 0) {
        Transmitter t; t.label = label; t.hz = lo; t.hiHz = hi; t.mode = "usb"; t.loop = "none"; t.upHz = upLo; return t; };
    auto dig = [](const char* label, double hz, const char* mode, const char* consumer, double baud, const char* loop) {
        Transmitter t; t.label = label; t.hz = hz; t.mode = mode; t.consumer = consumer; t.baud = baud; t.loop = loop; return t; };
    auto cw = [](const char* label, double hz, const char* note = "") {
        Transmitter t; t.label = label; t.hz = hz; t.mode = "cw"; t.loop = "none"; t.note = note; return t; };
    auto named = [](Transmitter t, const char* modeName) { t.modeName = modeName; return t; };
    std::vector<SatDef> v;
    v.push_back({"ISS", "ISS", 25544, "amateur", "Crew contacts, events and the repeater when ARISS has it on",
                 {fm("FM repeater", 437800000, "Cross-band repeater, uplink 145.990 MHz (67 Hz)", 145990000, 67.0),
                  fm("Voice / SSTV", 145800000, "School contacts and SSTV events"),
                  dig("APRS", 145825000, "nfm", "ax25-1200", 1200, "fll"),
                  fm("SSTV events", 437550000, "Recent ARISS SSTV events (Robot 36) were sent here")}});
    v.push_back({"SO-50", "SO-50 (SaudiSat-1C)", 27607, "amateur", "FM repeater; armed by a 74.4 Hz tone, 10 minute timer",
                 {fm("FM repeater", 436795000, "Uplink 145.850 MHz (67 Hz)", 145850000, 67.0)}});
    v.push_back({"AO-123", "AO-123 (ASRTU-1)", 61781, "amateur", "FM repeater",
                 {fm("FM repeater", 435400000, "Uplink 145.850 MHz (67 Hz)", 145850000, 67.0)}});
    v.push_back({"PO-101", "PO-101 (Diwata-2)", 43678, "amateur", "FM repeater on a schedule",
                 {fm("FM repeater", 145900000, "Uplink 437.500 MHz (141.3 Hz)", 437500000, 141.3)}});
    v.push_back({"RS-44", "RS-44", 44909, "amateur", "Linear transponder, inverting",
                 {lin("Transponder", 435610000, 435670000, 145935000), cw("CW beacon", 435605000)}});
    v.push_back({"FO-29", "FO-29 (JAS-2)", 24278, "amateur", "Linear transponder on a schedule (power permitting)",
                 {lin("Transponder", 435800000, 435900000, 145900000), cw("CW beacon", 435795000)}});
    v.push_back({"AO-7", "AO-7", 7530, "amateur", "Sunlight only; mode A or B by its own timer",
                 {named(lin("Mode B transponder", 145925000, 145975000, 432125000), "Mode B"),
                  named(lin("Mode A transponder", 29400000, 29500000, 145850000), "Mode A"),
                  named(cw("Mode A beacon", 29502000), "Mode A"),
                  named(cw("Mode B beacon", 145977500, "AMSAT gives 145.9775 MHz; it has drifted to about 145.973"), "Mode B")}});
    v.push_back({"AO-73", "AO-73 (FUNcube-1)", 39444, "amateur", "Transponder in eclipse; telemetry in sunlight",
                 {lin("Transponder", 145950000, 145970000, 435130000), dig("Telemetry", 145935000, "usb", "fc-bpsk", 1200, "none")}});
    // ★★ 2026-11-24 + its TELEMETRY BEACON (Stuart heard it at ~145.841 MHz on the Pi 500's V4, the FUNcube 5 s frame rhythm
    //   plain on the waterfall): JY1Sat is built on the FUNcube design — 1200 bit/s BPSK, the same AO-40 FEC — so the fctlm
    //   decoder frames it. ★★ beta94 its OWN layout is read (vibe_fctlm.h parseJy1 — AMSAT-UK's "JY1SAT data budget with
    //   tlm equations 2F"): the extended satellite id (16 header bits), then JY1Sat's 432-bit block, in its units.
    {
        Transmitter t = dig("Telemetry", 145840000, "usb", "fc-bpsk", 1200, "none");
        t.note = "FUNcube-format beacon; JY1Sat's own telemetry layout and units";
        v.push_back({"JO-97", "JO-97 (JY1Sat)", 43803, "amateur", "Linear transponder; FUNcube-format telemetry beacon",
                     {lin("Transponder", 145855000, 145875000, 435100000), t}});
    }
    v.push_back({"MO-122", "MO-122 (MESAT1)", 60209, "amateur", "Linear transponder",
                 {lin("Transponder", 435810000, 435840000, 145910000), dig("Telemetry", 435800000, "usb", "fox-bpsk", 1200, "none")}});
    v.push_back({"SONATE-2", "SONATE-2", 59112, "amateur", "Telemetry, APRS and SSTV",
                 {[&] { Transmitter t = dig("Telemetry", 437025000, "nfm", "ax25-9600", 9600, "none"); t.note = "Germany passes only"; return t; }(),
                  dig("APRS", 145825000, "nfm", "ax25-1200", 1200, "fll"),
                  fm("SSTV", 145880000, "Martin M1 pictures on the university's published schedule")}});
    v.push_back({"TO-108", "TO-108 (CAS-6)", 44881, "amateur", "Linear transponder, inverting; on intermittently",
                 {lin("Transponder", 145915000, 145935000, 435280000)}});
    v.push_back({"METEOR-M2-3", "Meteor-M2-3", 57166, "weather", "Weather images (LRPT); mode and frequency can change",
                 {dig("LRPT", 137900000, "lrpt", "lrpt", 72000, "costas")}});
    v.push_back({"METEOR-M2-4", "Meteor-M2-4", 59051, "weather", "Weather images (LRPT); mode and frequency can change",
                 {dig("LRPT", 137900000, "lrpt", "lrpt", 72000, "costas")}});
    linkXpdr(v);   // ★★★ beta94 each transponder and its beacon: one finder span
    return v;
}

/** ★★★ beta94 TRANSPONDER FINDER: join each linear transponder with its bird's CW beacon (the same band, within
 *  kXpdrBeaconReachHz of the passband, the same catalogue mode name when both have one — AO-7's Mode B beacon goes with
 *  Mode B) into one satellite-frame span, set on both (Transmitter::xpdrLo/xpdrHi/beaconHz). A transponder with no beacon
 *  near it is a span of its own passband. "non-inverting" in the bird's or the downlink's note clears `inverting`. Called
 *  on the built-in list and on every sats.json the mirror sends (the mirror knows nothing of it). */
constexpr double kXpdrBeaconReachHz = 30000.0;
inline void linkXpdr(std::vector<SatDef>& defs) {
    auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    for (auto& d : defs) {
        const bool nonInv = lower(d.note).find("non-inverting") != std::string::npos;
        for (auto& t : d.tx) { t.xpdrLo = t.xpdrHi = t.beaconHz = t.tlmHz = 0; }
        for (auto& t : d.tx) {
            if (!t.passband() || !(t.mode == "usb" || t.mode == "lsb")) continue;
            t.inverting = !(nonInv || lower(t.note).find("non-inverting") != std::string::npos);
            double lo = t.hz, hi = t.hiHz;
            Transmitter* beacon = nullptr;
            for (auto& b : d.tx) {
                if (&b == &t || b.passband() || b.mode != "cw" || !b.consumer.empty()) continue;
                if (!t.modeName.empty() && !b.modeName.empty() && t.modeName != b.modeName) continue;
                if (b.hz < t.hz - kXpdrBeaconReachHz || b.hz > t.hiHz + kXpdrBeaconReachHz) continue;
                if (b.xpdr()) continue;   // ★ already another transponder's
                beacon = &b; break;
            }
            if (beacon) { lo = std::min(lo, beacon->hz); hi = std::max(hi, beacon->hz); }
            // ★★★ beta94 the bird's FUNcube telemetry within reach: one more signal in the span (its own decoder rides along)
            const Transmitter* tlm = nullptr;
            for (auto& b : d.tx) {
                if (&b == &t || b.passband() || b.consumer != "fc-bpsk") continue;
                if (b.hz < t.hz - kXpdrBeaconReachHz || b.hz > t.hiHz + kXpdrBeaconReachHz) continue;
                bool taken = false;
                for (auto& o : d.tx) if (&o != &t && o.tlmHz == b.hz) taken = true;
                if (taken) continue;
                tlm = &b; break;
            }
            if (tlm) { lo = std::min(lo, tlm->hz); hi = std::max(hi, tlm->hz); }
            t.xpdrLo = lo; t.xpdrHi = hi; t.beaconHz = beacon ? beacon->hz : 0; t.tlmHz = tlm ? tlm->hz : 0;
            if (beacon) { beacon->xpdrLo = lo; beacon->xpdrHi = hi; beacon->beaconHz = beacon->hz; beacon->inverting = t.inverting; }
        }
    }
}
/** The transponder a linked beacon belongs to (its index), or −1 — a pick of the beacon opens the transponder's finder.
 *  ★★★ beta94 so does a linked FUNcube telemetry downlink (Transmitter::tlmHz): its pick opens the transponder, whose
 *  finder decodes the telemetry too. */
inline int xpdrOwnerOf(const SatDef& d, int txi) {
    if (txi < 0 || txi >= (int)d.tx.size()) return -1;
    const Transmitter& b = d.tx[(size_t)txi];
    if (b.consumer == "fc-bpsk" && !b.passband()) {
        for (size_t k = 0; k < d.tx.size(); ++k)
            if (d.tx[k].passband() && d.tx[k].xpdr() && d.tx[k].tlmHz > 0 && d.tx[k].tlmHz == b.hz) return (int)k;
        return -1;
    }
    if (!b.xpdr() || b.passband()) return -1;
    for (size_t k = 0; k < d.tx.size(); ++k)
        if (d.tx[k].passband() && d.tx[k].xpdr() && d.tx[k].beaconHz == b.hz && d.tx[k].xpdrLo == b.xpdrLo) return (int)k;
    return -1;
}

/** ★★ V12 2026-11-24 ORBCOMM: a bird of the ORBCOMM constellation (by name — the mirror's "ORBCOMM FM114"). */
inline bool orbcommBird(const SatDef& d) {
    std::string n; for (char c : d.name.substr(0, 7)) n += (char)std::toupper((unsigned char)c);
    std::string i; for (char c : d.id.substr(0, 7)) i += (char)std::toupper((unsigned char)c);
    return n == "ORBCOMM" || i == "ORBCOMM";
}
/** A label-only ORBCOMM downlink → the decoder's downlink (the same frequency or band; NFM to listen with). */
inline void orbcommTx(Transmitter& t) {
    t.mode = "orbcomm"; t.consumer = "orbcomm"; t.loop = "none"; t.baud = 4800;
    if (t.label.empty() || t.label == "label") t.label = "Downlink";
    if (t.note.empty()) t.note = "Housekeeping decoded; subscriber messages are not decoded (shown raw)";
}
/** What sats.json says about itself — the picker's credit line ("Orbits: CelesTrak · 5 h old"). */
struct SatsMeta {
    double generated = 0;     // the mirror built it (Unix)
    double orbitsAt = 0;      // the OLDEST group's CelesTrak fetch (Unix) — the data's age
    std::string orbitsCredit = "Orbits: CelesTrak";
    std::string txCredit;
    bool fromMirror = false;
};
inline double isoToUnix(const std::string& s) { double jd = 0; return parseIsoUtc(s, jd) ? unixFromJd(jd) : 0.0; }

/** ★★ beta91 SATELLITE LABELS (docs/v12/SAT-LABELS.md): the LABEL-ONLY birds of a sats.json — kept apart from the list
 *  (Engine::labelDefs_), so nothing that offers, locks or decodes a satellite ever sees them. */
struct LabelSet {
    std::vector<SatDef> defs; std::vector<Elements> els; std::vector<bool> have;
    void clear() { defs.clear(); els.clear(); have.clear(); }
};

/** ★★ sats.json (vibesdr.net's mirror, docs/v12/SATELLITE-ENGINE.md §6.2) → the list, the element sets, the credit.
 *  Strict: an unknown format, no satellites, a bad id / catalogue number / frequency / mode = refused (false) and the
 *  caller keeps what it has. A transmitter with an unknown mode or loop is skipped, not the whole bird.
 *  ★ Catalogue numbers are integers 1 … 999 999 999 (six digits are normal now) — never a 5-digit TLE field. */
/*  ★★ beta91: a bird marked "labelOnly", or whose every downlink is mode "label", goes to `labels` (when given — the
 *  validators pass none and just check it); a "label" downlink on an ordinary bird is dropped. A server before beta91
 *  skips "label" (an unknown mode), so such a bird has no usable downlink there and is not offered. ≤ 200 birds in all
 *  (the old servers' limit — the mirror keeps under it). */
inline bool parseSats(const std::string& text, std::vector<SatDef>& defs, std::vector<Elements>& els, std::vector<bool>& haveEl,
                      SatsMeta& meta, std::string& err, LabelSet* labels = nullptr) {
    json::Value doc;
    if (text.size() > 4u * 1024 * 1024 || !json::parseValue(text, doc) || doc.t != json::Value::Obj) { err = "not JSON"; return false; }
    if (doc.num("format") != 1) { err = "an unknown sats.json format"; return false; }
    const json::Value* sats = doc.get("satellites");
    if (!sats || sats->t != json::Value::Arr || sats->a.empty() || sats->a.size() > 200) { err = "no satellites"; return false; }
    defs.clear(); els.clear(); haveEl.clear();
    if (labels) labels->clear();
    meta = SatsMeta{};
    meta.fromMirror = true;
    meta.generated = isoToUnix(doc.str("generated"));
    if (const json::Value* at = doc.get("attribution")) {
        if (!at->str("orbits").empty()) meta.orbitsCredit = at->str("orbits").substr(0, 120);
        meta.txCredit = at->str("transmitters").substr(0, 160);
    }
    if (const json::Value* og = doc.get("orbits"); og && og->t == json::Value::Obj)
        for (auto& kv : og->o) {
            const double t = isoToUnix(kv.second.str("fetched"));
            if (t > 0 && (meta.orbitsAt <= 0 || t < meta.orbitsAt)) meta.orbitsAt = t;
        }
    auto okId = [](const std::string& id) {
        if (id.empty() || id.size() > 32) return false;
        for (char c : id) if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
        return true;
    };
    for (const auto& x : sats->a) {
        if (x.t != json::Value::Obj) { err = "a satellite is not an object"; return false; }
        SatDef d;
        d.id = x.str("id"); d.name = x.str("name").substr(0, 64); d.group = x.str("group").substr(0, 32); d.note = x.str("note").substr(0, 240);
        const double nd = x.num("norad");
        if (!okId(d.id) || !(nd >= 1 && nd <= 999999999.0) || nd != std::floor(nd)) { err = "a bad satellite id or catalogue number"; return false; }
        d.norad = (long long)nd;
        if (d.name.empty()) d.name = d.id;
        const json::Value* txs = x.get("transmitters");
        if (txs && txs->t == json::Value::Arr)
            for (const auto& t : txs->a) {
                if (t.t != json::Value::Obj || d.tx.size() >= 16) continue;
                Transmitter tr;
                tr.label = t.str("label").substr(0, 48); tr.hz = t.num("downlink"); tr.hiHz = t.num("downlinkHigh");
                tr.mode = t.str("mode"); tr.loop = t.str("loop"); tr.consumer = t.str("consumer"); tr.baud = t.num("baud");
                tr.upHz = t.num("uplink"); tr.ctcss = t.num("ctcss"); tr.note = t.str("note").substr(0, 160); tr.status = t.str("status");
                tr.modeName = t.str("modeName").substr(0, 24);
                static const char* kModes[] = {"nfm", "usb", "lsb", "cw", "am", "lrpt", "label", "orbcomm"};
                bool modeOk = false; for (auto m : kModes) if (tr.mode == m) modeOk = true;
                const bool loopOk = tr.loop == "fll" || tr.loop == "none" || tr.loop == "costas";
                if (!modeOk || !loopOk || !(tr.hz >= 1e6 && tr.hz <= 1e10) || (tr.hiHz != 0 && !(tr.hiHz > tr.hz && tr.hiHz - tr.hz <= 2e6))) continue;
                if (tr.status != "active" && tr.status != "inactive") tr.status.clear();
                if (tr.consumer.size() > 24) tr.consumer.clear();
                if (tr.label.empty()) tr.label = tr.mode;
                d.tx.push_back(tr);
            }
        // ★★ beta91: label-only birds apart; a stray "label" downlink on an ordinary bird is not listened to
        bool allLabel = !d.tx.empty();
        for (auto& t : d.tx) if (!t.labelOnly()) allLabel = false;
        const json::Value* lo = x.get("labelOnly");
        bool labelBird = allLabel || (lo && lo->t == json::Value::Bool && lo->b);
        /* ★★★ V12 2026-11-24 ORBCOMM IS A REGULAR SATELLITE NOW (Stuart: "we can treat orbcomm as a regular satellite, just
         *  put the caveat that encrypted messages not decoded but we can show the raw data"). The directory's mirror still
         *  sends the beta91 shape — an "ORBCOMM …" bird, label-only, one downlink of mode "label" (its own channel, or the
         *  137.2–137.8 MHz band) — so THIS server turns it into an ordinary pick: listed with passes, Doppler-tracked, the
         *  ORBCOMM decoder attached (orbcommBird). A mirror that one day sends mode "orbcomm" itself needs nothing here. */
        if (labelBird && orbcommBird(d)) {
            for (auto& t : d.tx) if (t.labelOnly()) orbcommTx(t);
            labelBird = false;
        }
        if (labelBird) {
            d.tx.erase(std::remove_if(d.tx.begin(), d.tx.end(), [](const Transmitter& t) { return !t.labelOnly(); }), d.tx.end());
            for (auto& t : d.tx) { t.loop = "none"; t.consumer.clear(); t.upHz = 0; t.ctcss = 0; }   // ★ a name, nothing else
        } else d.tx.erase(std::remove_if(d.tx.begin(), d.tx.end(), [](const Transmitter& t) { return t.labelOnly(); }), d.tx.end());
        if (d.tx.empty()) continue;   // nothing to listen to: not offered
        Elements e; bool have = false;
        if (const json::Value* omm = x.get("omm"); omm && omm->t == json::Value::Obj) {
            std::string e2;
            have = fromOmm(omm->flat(), e, e2) && e.norad == d.norad;
        }
        if (labelBird) {
            if (labels && labels->defs.size() < 200) { labels->defs.push_back(std::move(d)); labels->els.push_back(e); labels->have.push_back(have); }
            continue;
        }
        defs.push_back(std::move(d)); els.push_back(e); haveEl.push_back(have);
    }
    if (defs.empty()) { err = "no satellite with a usable downlink"; return false; }
    linkXpdr(defs);   // ★★★ beta94 each transponder and its beacon: one finder span (the mirror knows nothing of it)
    return true;
}

/** The CelesTrak GP groups the list needs. */
inline std::vector<std::string> catalogueGroups() { return {"amateur", "weather"}; }

// ── ★★ V12 SATELLITE MODE (docs/v12/SATELLITE-MODE.md "THE CONTRACT"): what a downlink is FOR, and what a pick runs ────
/** ISS-style "Voice / SSTV" downlinks: an FM channel whose label or note names SSTV — the picture events go out on it,
 *  so a pick there runs the SSTV decoder (auto-sync) under the voice. Read from the data, never a hard-coded number. */
inline bool sstvOf(const Transmitter& t) {
    if (t.mode != "nfm") return false;
    auto has = [](const std::string& s) {
        for (size_t i = 0; i + 4 <= s.size(); ++i)
            if (std::toupper((unsigned char)s[i]) == 'S' && std::toupper((unsigned char)s[i + 1]) == 'S'
                && std::toupper((unsigned char)s[i + 2]) == 'T' && std::toupper((unsigned char)s[i + 3]) == 'V') return true;
        return false;
    };
    return has(t.label) || has(t.note);
}
/** "image" (a picture decoder: Meteor LRPT) · "packet" (text lines: AX.25 / APRS, a CW beacon's decoded text (beta90
 *  1.1), FUNcube telemetry (1.2)) · "voice" (listening: FM repeaters, linear transponders — and telemetry with no
 *  decoder of ours, e.g. Fox-format "fox-bpsk", which is heard, not decoded). */
inline const char* useOf(const Transmitter& t) {
    if (t.consumer == "lrpt" || t.mode == "lrpt") return "image";
    if (t.consumer.rfind("ax25", 0) == 0) return "packet";
    if (t.consumer == "fc-bpsk") return "packet";
    if (t.orbcomm()) return "packet";   // ★ 2026-11-24 ORBCOMM: housekeeping lines + raw subscriber frames
    if (t.mode == "cw" && t.consumer.empty()) return "packet";
    return "voice";
}
/** ★ beta90 1.4: AMSAT's band letter for a frequency (A 10 m · V 2 m · U 70 cm · L 23 cm · S 13 cm · X 3 cm), or 0. */
inline char bandLetter(double hz) {
    if (hz >= 21.0e6 && hz <= 30.0e6) return 'A';
    if (hz >= 144.0e6 && hz <= 148.0e6) return 'V';
    if (hz >= 420.0e6 && hz <= 450.0e6) return 'U';
    if (hz >= 1240.0e6 && hz <= 1300.0e6) return 'L';
    if (hz >= 2300.0e6 && hz <= 2450.0e6) return 'S';
    if (hz >= 10.0e9 && hz <= 10.5e9) return 'X';
    return 0;
}
/** ★ beta90 1.4: "U/v" — uplink band UPPER case, downlink lower case (AMSAT's notation). "" when the downlink has no
 *  uplink (a beacon, telemetry, a weather satellite) or either band has no letter. We only receive: it is how people
 *  find the bird. */
inline std::string bandOf(const Transmitter& t) {
    if (!(t.upHz > 0)) return "";
    const char u = bandLetter(t.upHz), d = bandLetter(t.hz);
    if (!u || !d) return "";
    return std::string(1, u) + "/" + std::string(1, (char)std::tolower((unsigned char)d));
}
/** The demodulator a downlink is listened with (a decoder's own mode → its carrier's) — the same answer as the clients'
 *  txListenMode (src/services/satellites.ts). */
inline std::string listenModeOf(const Transmitter& t) {
    if (t.mode == "nfm" || t.mode == "usb" || t.mode == "lsb" || t.mode == "cw" || t.mode == "am") return t.mode;
    return "nfm";   // ★ lrpt: the LRPT decoder cuts its own slice; NFM is its listening mode
}
/* ★★★ beta92 SATELLITE TELEMETRY LISTENS WIDE (Stuart 11-23, AO-73 on the Pi 500's RTL-SDR V4: the bird sat 1.2–1.4 kHz
 *  ABOVE the Doppler-corrected prediction all pass — a dongle's ppm, the listed figure or the transmitter's own drift —
 *  so at the old 1200 Hz centre it landed at ~2.4 kHz audio: outside the decoder's ±900 Hz hunt, and its top cut by the
 *  2.7 kHz USB edge. 0 frames on a strong signal). A BPSK telemetry pick now listens in a 100–7600 Hz USB passband
 *  with the decoder's centre in the middle (3800 Hz) and its hunt ±2600 Hz — the signal's own ±0.9 kHz plus ±2.5 kHz
 *  of prediction error fits. The dial sits kTlmCentreHz below the downlink. Once the decoder is locked, its measured
 *  offset becomes the lock's TRIM (Engine::setDecoderTrim), so the channel follows the real signal back to the centre
 *  ("a wider usb passband and audio correction too that way it compensates for transmitter drift too").
 * ★★★ beta93 (11-24) THE CARRIER AT 1.5 kHz, NOT 3.8 (Stuart: the pick's telemetry "sounds thin and high-pitched"; tuned
 *  by hand it sounded fuller). The dial now sits 1500 Hz below the downlink, so the BPSK lands where a hand-tuned USB
 *  signal does, and the passband stops at 5 kHz (the hunt's top, 1500 + 2600, plus the signal's own ~0.9 kHz). The hunt
 *  keeps its 2.6 kHz UPWARDS; downwards it now reaches 1.2 kHz (a carrier at 300 Hz — vibebpsk::RxCfg::minCarrierHz:
 *  below 0 Hz there is no USB audio, and the window must not hold the carrier's mirror). On air so far the bird has
 *  been +1.3 kHz (11-23) and −0.6 kHz (11-24) from the prediction — both inside. A signal more than ~1.2 kHz BELOW the
 *  prediction is no longer found (at 3800 it was, to −2.6 kHz): the trade for a natural-sounding pick. */
/* ★★★ beta94 (11-25) THE CARRIER AT 3.0 kHz, IN 100–6500 Hz (Stuart, the Pi 500 V4L, a JO-97 pass at 28°: "i think the
 *  telemetry signal is just outside our passband"). The bird sat ~0.9 kHz BELOW the prediction, so at the 1500 Hz centre
 *  its carrier landed at ~550 Hz audio: inside the hunt (≥ 300 Hz) but with the lower half of the BPSK (carrier
 *  −0.9 kHz, raised cosine α 0.5 at 1200 Bd) below the dial, where USB has no audio — the real audio's mirror sits on top
 *  of it. "1 frame · 0 good · 1 failed". He nudged the dial 500 Hz down (carrier ~1.05 kHz) and 3 of 4 frames decoded.
 *  On air so far: +1.3 kHz (AO-73 11-23), −0.6 kHz (AO-73 11-24), −0.9 kHz (JO-97 11-25) — a dongle's ppm, the listed
 *  figure, the orbit. The rule now: a ±2 kHz error keeps the WHOLE signal (carrier ±0.9 kHz) inside the passband —
 *  3000 − 2000 − 900 = 100 Hz, the low edge; 3000 + 2600 (the hunt's top) + 900 = 6500 Hz, the high edge. Synthetic
 *  frames through the pick's path (test-fctlm §11, Eb/N0 8 dB): the 1500 Hz pick 0 / 12 at −0.9, −1.5 and −2.0 kHz;
 *  this one 12 / 12 at every offset from −2.0 to +2.0 kHz. The cost: the carrier is heard at 3 kHz, not 1.5 — higher
 *  than beta93, lower than beta92's 3.8 kHz that Stuart called thin. Opening the low edge to 0 Hz would not help: what
 *  is below the dial is not in USB audio at all — only a higher carrier keeps it. */
/* ★★★ beta94 AND THE DECODER READS IQ (Stuart 11-25: "or again use iq"). The pick's decoder no longer depends on the
 *  audio at all: it reads a Doppler-corrected IQ slice (kTlmIqWidthHz, cut near the lock's aim as the CW beacon
 *  skimmer's is — DecoderHost setIqAim) with the prediction mixed to 0 Hz, and hunts ±kTlmAfcHz around it — no USB edge,
 *  no mirror, the carrier's sign known (vibefc::Run::feedIq). Its offset is the satellite's against the prediction, so
 *  the drift follow trims the lock exactly as before. The audio above is now only what the listener hears (and the box
 *  on the waterfall): the whole signal for a ±2 kHz error. The decoder's reach is wider than the box: ±4 kHz. */
constexpr double kTlmCentreHz = 3000.0;   ///< where the carrier lands in the listener's audio (the dial this far below the downlink)
constexpr double kTlmAfcHz = 4000.0;      ///< the decoder's coarse hunt on IQ (vibebpsk::RxCfg::afcRangeHz) — ±4 kHz of the prediction
constexpr double kTlmIqWidthHz = 24000.0; ///< the IQ slice the decoder reads (the dial's aim −9 … +15 kHz holds the hunt and the signal)
constexpr double kTlmAudioLoHz = 100.0;   ///< the USB passband a telemetry pick listens with (the clients' edges)
constexpr double kTlmAudioHiHz = 6500.0;
/** ★★ beta92: a downlink whose pick runs the BPSK telemetry decoder on the wide passband (FUNcube "fc-bpsk" — the one
 *  BPSK telemetry format we decode; Fox "fox-bpsk" is heard, not decoded, so it keeps the voice passband). */
inline bool tlmWide(const Transmitter& t) { return t.consumer == "fc-bpsk" && t.mode == "usb"; }
/** ★ beta90 1.2: where the DIAL sits relative to the downlink for its decoder — FUNcube BPSK is listened kTlmCentreHz
 *  LOW in USB (beta90–91: 1200 Hz), so the signal lands at the fctlm decoder's centre; everything else on the
 *  frequency itself. */
inline double listenOffsetHz(const Transmitter& t) {
    if (tlmWide(t)) return -kTlmCentreHz;
    return 0.0;
}
/** Where a pick tunes: a single channel's frequency, a passband's centre (the clients' txTuneHz) — plus the decoder's
 *  listening offset (listenOffsetHz). */
inline double tuneHzOf(const Transmitter& t) { return std::round(t.centre() + listenOffsetHz(t)); }
/** ★★ beta92: the CW skimmer's window for a satellite CW beacon (Hz) — wide enough for a prediction a few kHz out at UHF. */
constexpr double kBeaconSkimHz = 24000.0;
/** ★★ 2026-11-24 ORBCOMM: the slice around ONE listed channel (±3.4 kHz signal, ±3.3 kHz Doppler, a dongle's ppm and the
 *  channel's neighbours to find), and the most a band pick asks for (137.2–137.8 MHz with Doppler room). */
constexpr double kOrbNarrowHz = 40000.0;
constexpr double kOrbMaxSliceHz = 640000.0;
constexpr double kOrbBandLoHz = 137.2e6, kOrbBandHiHz = 137.8e6;   ///< the channels in use (the directory's band listing)
/** The band an ORBCOMM pick decodes: the listed band when the downlink is one, else the channels in use — its centre. */
inline double orbcommBandCentreHz(const Transmitter& t) { return t.passband() ? t.centre() : 0.5 * (kOrbBandLoHz + kOrbBandHiHz); }
/** ★★★ beta94 THE TRANSPONDER FINDER'S SLICE: the span (passband + beacon) and, either side, the whole Doppler swing (the
 *  slice is cut FIXED on the span's satellite-frame centre — DecoderHost iqFixed — and the finder turns the Doppler out
 *  itself, so every signal keeps its satellite-frame place all pass) plus kXpdrMarginHz for a dongle's error and a
 *  station at the very edge. RS-44 (65 kHz with its beacon, 435 MHz): ~99 kHz. */
constexpr double kXpdrMarginHz = 5000.0;
constexpr double kXpdrDopplerFrac = 2.7e-5;   ///< vibe_sat_if.h kLeoDopplerFrac (the largest a low orbit gives)
inline double xpdrSliceHz(const Transmitter& t) {
    return (t.xpdrHi - t.xpdrLo) + 2.0 * (std::fabs(t.xpdrCentre()) * kXpdrDopplerFrac + kXpdrMarginHz);
}
/** ★★ beta92: half the CW passband a beacon pick listens with, centred on the prediction (so ±3 kHz is heard). */
constexpr double kBeaconAudioHalfHz = 3000.0;
/** ★★ beta92: a downlink whose pick runs the CW skimmer over kBeaconSkimHz (a satellite CW beacon). */
inline bool beaconSkimmed(const Transmitter& t) { return t.mode == "cw" && t.consumer.empty(); }
/** The decoder a pick attaches and its attach parameters (the JSON body after the extension name, "" = none) — the
 *  same parameters the clients' own keys send, so a client that later attaches the same decoder JOINS it (DecoderHost
 *  start: the same ext + paramsOf = Joined, nothing restarted).
 *   · image / lrpt → "lrpt", AUTO modulation and coding, the symbol rate from the satellite's entry (72k / 80k)
 *   · packet → "aprs" at the downlink's baud ("packet":"1200" | "9600")
 *   · voice with sstv → "sstv" (auto-sync; it waits quietly between pictures)
 *   · a CW beacon → "cwskim" over 24 kHz (beta92; beta90–91 ran "cw") · FUNcube telemetry (fc-bpsk) → "fctlm" (beta90)
 *   · voice → "" (nothing to decode). */
inline std::string decoderOf(const Transmitter& t, std::string& params) {
    params.clear();
    if (t.consumer == "lrpt" || t.mode == "lrpt") {
        params = "\"modulation\":\"auto\",\"diff\":\"auto\"";
        if (t.baud > 0) params += std::string(",\"symbol_rate\":") + (t.baud >= 76000 ? "80000" : "72000");
        return "lrpt";
    }
    if (t.consumer == "ax25-9600") { params = "\"packet\":\"9600\",\"baud_rate\":1200"; return "aprs"; }
    if (t.consumer == "ax25-1200" || t.consumer.rfind("ax25", 0) == 0) { params = "\"packet\":\"1200\",\"baud_rate\":1200"; return "aprs"; }
    // ★ beta90 1.2: FUNcube BPSK 1200, AUTO. ★★ beta92 its centre and hunt on the wide passband (kTlmCentreHz, kTlmAfcHz)
    if (t.consumer == "fc-bpsk") {
        // ★★★ beta94 "iq":true — on the IQ slice, the carrier "centre" Hz above the dial (DecoderHost buildFctlm)
        if (tlmWide(t)) params = "\"centre\":" + std::to_string((int)kTlmCentreHz) + ",\"afc_range\":" + std::to_string((int)kTlmAfcHz) + ",\"iq\":true";
        return "fctlm";
    }
    /* ★★ beta92 THE CW BEACON IS SKIMMED (Stuart 11-23, RS-44 on the Pi 500's RTL-SDR V4: the beacon sat ~2 kHz below the
     *  prediction — catalogue figure, dongle error, orbit age — outside the one-signal CW passband, and nothing decoded:
     *  "treat it like the cw skimmer and have a wider filter of say 24k"). The CW SKIMMER over kBeaconSkimHz of IQ centred
     *  on the Doppler-corrected prediction (the host's IQ aim, DecoderHost::setIqAim — the window follows the pass), so a
     *  beacon a few kHz off the figure is still read, and its offset from the prediction is shown. */
    /* ★★★ beta94 THE TRANSPONDER FINDER (docs/v12/TRANSPONDER-FINDER.md): a linear transponder — and its beacon, which is
     *  one more CW row in it, not a pick of its own — runs "xpdr": every CW signal in the span skimmed and decoded, every
     *  voice found and offered to LISTEN, on one Doppler-corrected slice (xpdrSliceHz) FIXED on the span's centre. */
    if (t.xpdr()) {
        char b[320];
        std::snprintf(b, sizeof b, "\"width\":%d,\"centre\":%lld,\"lo\":%lld,\"hi\":%lld,\"beacon\":%lld,\"up\":%lld,\"inv\":%d,\"sb\":\"%s\"",
                      (int)std::lround(xpdrSliceHz(t)), (long long)std::llround(t.xpdrCentre()), (long long)std::llround(t.xpdrLo),
                      (long long)std::llround(t.xpdrHi), (long long)std::llround(t.beaconHz), (long long)std::llround(t.upHz),
                      t.inverting ? 1 : 0, t.mode == "lsb" ? "lsb" : "usb");
        params = b;
        // ★★★ beta94 the bird's FUNcube telemetry in the span: the finder decodes it too (vibe_xpdr_run.h — sat_tlm, fctlm_status)
        if (t.tlmHz > 0) params += ",\"tlm\":" + std::to_string((long long)std::llround(t.tlmHz));
        return "xpdr";
    }
    if (beaconSkimmed(t)) { params ="\"width\":" + std::to_string((int)kBeaconSkimHz); return "cwskim"; }
    /* ★★ 2026-11-24 ORBCOMM: the decoder cuts its own slice at the dial — the bird's listed channel ± Doppler
     *  (kOrbNarrowHz), or the whole listed band (the mirror's 137.2–137.8 MHz when no single channel is known) — and finds
     *  every carrier in it; the one that is this bird is named by its own position report (vibe_orbcomm_id.h). */
    /* ★★★ 2026-11-24 (Stuart: "why is it not the full 137.2 – 137.8?"): EVERY ORBCOMM pick decodes the WHOLE band — a bird
     *  with one listed channel too (FM110's own packets listed 137.7125 / 137.800 the night its listing said 137.290: published
     *  channels go stale). The slice is the band + kOrbNarrowHz, FIXED on the band's centre ("centre"), never following the
     *  dial's Doppler steps (the receiver tracks each carrier's Doppler itself; a slice that moved each second jolted the
     *  carriers — the live FM110 pass lost lock at its highest). The listed channel only places the listening dial. */
    if (t.orbcomm()) {
        params = "\"width\":" + std::to_string((int)kOrbMaxSliceHz) + ",\"centre\":" + std::to_string((long long)std::llround(orbcommBandCentreHz(t)));
        return "orbcomm";
    }
    if (sstvOf(t)) return "sstv";
    return "";
}
/** ★ beta90 UNATTENDED PASSES: the downlinks a pass can be KEPT from — the ones with a decoder whose output goes to the
 *  gallery (pictures, packets, telemetry). A voice / transponder downlink has nothing to keep, so it is not offered. */
inline bool passKeepable(const Transmitter& t) {
    std::string p;
    const std::string d = decoderOf(t, p);
    return d == "lrpt" || d == "sstv" || d == "aprs" || d == "fctlm" || d == "orbcomm";
}
/** ★★★ beta96 SINGLE MODE: SATELLITES — THE LINEAR-TRANSPONDER HYBRID (docs/v12/SAT-SINGLE.md). A downlink a SATELLITES
 *  radio may run a pass of although it has nothing to keep: a linear transponder's passband (SSB / CW contacts all over
 *  it). For its pass the radio becomes a locked range over the passband and every listener tunes their own VFO across it
 *  with their own AUTO DOPPLER. Only on a single-mode SATELLITES radio (the scheduler asks), never an ordinary one. */
inline bool passHybrid(const Transmitter& t) {
    return t.passband() && (t.mode == "usb" || t.mode == "lsb") && t.consumer.empty() && !t.labelOnly() && !t.orbcomm();
}
/** The Doppler margin either side of a hybrid's passband: the largest LEO Doppler at its frequency (~2.7e-5·f — the
 *  per-listener lock's own figure) + the lock's trim clamp. */
inline double hybridMarginHz(const Transmitter& t) { return t.hiHz * 2.7e-5 + 2500.0; }
/** The attach message a pick hands the decoder host — the shape a client's audio_extension_attach has. */
inline std::string decoderAttachMsg(const std::string& ext, const std::string& params) {
    return "{\"type\":\"audio_extension_attach\",\"extension_name\":\"" + ext + "\"" + (params.empty() ? "" : "," + params) + "}";
}
/** What sat_list says about the satellite DATA SUPPLY (item 10): the last good sync and a fetch in progress. */
struct SupplyInfo {
    bool known = false; double synced = 0; bool fetching = false;
    int pickPrivate = -1;   // ★ beta90 1.6: 1 = a pick here disturbs nobody (single user / per-listener locked range), 0 = shared, -1 = not said
};
/** Why a downlink is greyed (the reach rule, in words) — "" = this radio receives it. */
using ReachWhyFn = std::function<std::string(double lo, double hi)>;

// ── Small helpers ─────────────────────────────────────────────────────────────────────────────────────────────────
inline std::string jsonEsc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
inline std::string fmtNum(double v, int dp) { char b[48]; std::snprintf(b, sizeof b, "%.*f", dp, std::isfinite(v) ? v : 0.0); return b; }

/** Element-set age → what the brief's §6.3 table says to do. */
enum class Age { Fresh, Stale, Old, Expired, None };
inline Age ageOf(double days) {
    if (!(days >= -2)) return Age::None;     // NaN, or an epoch far in the future = a wrong clock or a bad set
    if (days < 3) return Age::Fresh;
    if (days < 7) return Age::Stale;
    if (days < 14) return Age::Old;
    return Age::Expired;
}
inline const char* ageName(Age a) {
    switch (a) { case Age::Fresh: return "fresh"; case Age::Stale: return "stale"; case Age::Old: return "old";
                 case Age::Expired: return "expired"; default: return "none"; }
}

// ── The closed loop's measurement (SIMD) ──────────────────────────────────────────────────────────────────────────
/** Σ x[i]·conj(x[i−1]), Σ|x[i]|² and Σ|x[i]|⁴ over a block of interleaved float IQ, x[−1] = `prev` (updated to the
 *  last sample). The angle of the first sum over a window is the power-weighted mean frequency of the channel — a
 *  carrier offset estimator with no arctangent per sample; the second and fourth moments give the window's SNR (M2M4,
 *  below). ★ NEON / SSE2 / scalar give the SAME sums: four lanes in the same order (lane k takes i ≡ k+1 mod 4 from
 *  i = 1), the tail and the first product sequentially, lanes added ((0+1)+(2+3)) into double; the scalar products go
 *  through sumOfProducts() so no compiler fuses them into an FMA the vector lanes do not use. */
struct LagSums { double re = 0, im = 0, pw = 0, p4 = 0; long long n = 0; };
inline float sumOfProducts(float a, float b, float c, float d) { volatile float x = a * b, y = c * d; return x + y; }
inline float diffOfProducts(float a, float b, float c, float d) { volatile float x = a * b, y = c * d; return x - y; }
inline float squared(float a) { volatile float x = a * a; return x; }
inline void lagAccumulate(const float* iq, int n, float prev[2], LagSums& acc) {
    if (n <= 0) return;
    float hr = sumOfProducts(iq[0], prev[0], iq[1], prev[1]);
    float hi = diffOfProducts(iq[1], prev[0], iq[0], prev[1]);
    float hp = sumOfProducts(iq[0], iq[0], iq[1], iq[1]);
    float h4 = squared(hp);
    float lr[4] = {0, 0, 0, 0}, li[4] = {0, 0, 0, 0}, lp[4] = {0, 0, 0, 0}, l4[4] = {0, 0, 0, 0};
    int i = 1;
    const int full = 1 + ((n - 1) / 4) * 4;
#if VIBE_NEON
    float32x4_t ar = vdupq_n_f32(0), ai = vdupq_n_f32(0), ap = vdupq_n_f32(0), a4 = vdupq_n_f32(0);
    for (; i < full; i += 4) {
        const float32x4x2_t c = vld2q_f32(iq + 2 * i), p = vld2q_f32(iq + 2 * (i - 1));
        ar = vaddq_f32(ar, vaddq_f32(vmulq_f32(c.val[0], p.val[0]), vmulq_f32(c.val[1], p.val[1])));
        ai = vaddq_f32(ai, vsubq_f32(vmulq_f32(c.val[1], p.val[0]), vmulq_f32(c.val[0], p.val[1])));
        const float32x4_t q = vaddq_f32(vmulq_f32(c.val[0], c.val[0]), vmulq_f32(c.val[1], c.val[1]));
        ap = vaddq_f32(ap, q);
        a4 = vaddq_f32(a4, vmulq_f32(q, q));
    }
    vst1q_f32(lr, ar); vst1q_f32(li, ai); vst1q_f32(lp, ap); vst1q_f32(l4, a4);
#elif VIBE_SSE
    __m128 ar = _mm_setzero_ps(), ai = _mm_setzero_ps(), ap = _mm_setzero_ps(), a4 = _mm_setzero_ps();
    for (; i < full; i += 4) {
        const __m128 c0 = _mm_loadu_ps(iq + 2 * i), c1 = _mm_loadu_ps(iq + 2 * i + 4);
        const __m128 p0 = _mm_loadu_ps(iq + 2 * (i - 1)), p1 = _mm_loadu_ps(iq + 2 * (i - 1) + 4);
        const __m128 cr = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(2, 0, 2, 0)), ci = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(3, 1, 3, 1));
        const __m128 pr = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(2, 0, 2, 0)), pi = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(3, 1, 3, 1));
        ar = _mm_add_ps(ar, _mm_add_ps(_mm_mul_ps(cr, pr), _mm_mul_ps(ci, pi)));
        ai = _mm_add_ps(ai, _mm_sub_ps(_mm_mul_ps(ci, pr), _mm_mul_ps(cr, pi)));
        const __m128 q = _mm_add_ps(_mm_mul_ps(cr, cr), _mm_mul_ps(ci, ci));
        ap = _mm_add_ps(ap, q);
        a4 = _mm_add_ps(a4, _mm_mul_ps(q, q));
    }
    _mm_storeu_ps(lr, ar); _mm_storeu_ps(li, ai); _mm_storeu_ps(lp, ap); _mm_storeu_ps(l4, a4);
#else
    for (; i < full; i += 4)
        for (int k = 0; k < 4; ++k) {
            const float* c = iq + 2 * (i + k); const float* p = c - 2;
            lr[k] = lr[k] + sumOfProducts(c[0], p[0], c[1], p[1]);
            li[k] = li[k] + diffOfProducts(c[1], p[0], c[0], p[1]);
            const float q = sumOfProducts(c[0], c[0], c[1], c[1]);
            lp[k] = lp[k] + q;
            l4[k] = l4[k] + squared(q);
        }
#endif
    for (; i < n; ++i) {
        const float* c = iq + 2 * i; const float* p = c - 2;
        hr = hr + sumOfProducts(c[0], p[0], c[1], p[1]);
        hi = hi + diffOfProducts(c[1], p[0], c[0], p[1]);
        const float q = sumOfProducts(c[0], c[0], c[1], c[1]);
        hp = hp + q;
        h4 = h4 + squared(q);
    }
    acc.re += (double)((lr[0] + lr[1]) + (lr[2] + lr[3])) + (double)hr;
    acc.im += (double)((li[0] + li[1]) + (li[2] + li[3])) + (double)hi;
    acc.pw += (double)((lp[0] + lp[1]) + (lp[2] + lp[3])) + (double)hp;
    acc.p4 += (double)((l4[0] + l4[1]) + (l4[2] + l4[3])) + (double)h4;
    acc.n += n;
    prev[0] = iq[2 * (n - 1)]; prev[1] = iq[2 * (n - 1) + 1];
}

/** The FLL's front end: windows of ~0.4 s → a residual (Hz) when the signal is clearly up, else nothing (COAST).
 *  ★ "Update only when squelch open AND SNR above threshold" (brief §4.2). The gate is the window's own SNR by the
 *    M2M4 moments estimator — for a constant-envelope signal (FM) in Gaussian noise, S = √(2·M2² − M4), N = M2 − S —
 *    so it needs no noise reference, no learning period and no squelch setting: noise alone gives S ≈ 0. A window
 *    under kGateDb never reaches the loop (its lag-sum angle is meaningless). */
class ResidualMeter {
public:
    static constexpr double kWindowSec = 0.4, kGateDb = 3.0;
    /** Feed channel IQ (the dial at DC). Returns true with `hz` when a window closed with the gate open. */
    bool feed(const float* iq, int n, double rateHz, double& hz) {
        if (rateHz <= 0 || n <= 0) return false;
        if (rateHz != rate_) { rate_ = rateHz; acc_ = LagSums{}; prev_[0] = prev_[1] = 0; }
        lagAccumulate(iq, n, prev_, acc_);
        if ((double)acc_.n < kWindowSec * rateHz) return false;
        const double cnt = (double)std::max<long long>(1, acc_.n);
        const double m2 = acc_.pw / cnt, m4 = acc_.p4 / cnt, re = acc_.re, im = acc_.im;
        acc_ = LagSums{};
        if (!(m2 > 0)) { lastSnrDb_ = -99; return false; }
        const double s2 = 2.0 * m2 * m2 - m4;
        const double S = s2 > 0 ? std::sqrt(s2) : 0.0, N = std::max(m2 - S, m2 * 1e-6);
        lastSnrDb_ = S > 0 ? 10.0 * std::log10(S / N) : -99.0;
        if (lastSnrDb_ < kGateDb) return false;
        hz = std::atan2(im, re) * rate_ / kTwoPi;
        return std::isfinite(hz);
    }
    double lastSnrDb() const { return lastSnrDb_; }
    void reset() { acc_ = LagSums{}; rate_ = 0; }
private:
    LagSums acc_; double rate_ = 0, lastSnrDb_ = 0; float prev_[2] = {0, 0};
};

// ── The engine ────────────────────────────────────────────────────────────────────────────────────────────────────
enum class LockState { Off, Armed, Track, Coast, Ended };
inline const char* lockName(LockState s) {
    switch (s) { case LockState::Armed: return "ARMED"; case LockState::Track: return "TRACK"; case LockState::Coast: return "COAST";
                 case LockState::Ended: return "ENDED"; default: return "OFF"; }
}

/** The clock the engine reads. ★ The shim passes vibeUtcNow() and clockMeasured() (brief §5: the same corrected time
 *  FT8 uses, never the raw system clock). */
struct Clock { double unix = 0; bool measured = false; };

/** ★★ beta91 SATELLITE LABELS (docs/v12/SAT-LABELS.md): one downlink of a bird above the horizon, where it is RECEIVED
 *  now (the published frequency + Doppler) — what the waterfall names. */
struct SkyLabel {
    std::string id, name, tx, mode;   // mode: the listening demodulator ("nfm" for a label-only or LRPT downlink)
    long long norad = 0;
    double hz = 0;                    // received centre (a passband's centre)
    double loHz = 0, hiHz = 0;        // a passband's received edges (0 = a single channel)
    double rateHzS = 0;               // the Doppler's rate here (Hz/s) — a client may carry the mark between messages
    double elDeg = 0;
    bool labelOnly = false;           // ORBCOMM-style: a name only (never offered, locked or decoded)
};

/** What a lock is doing right now — what the caller applies (shiftHz) and what the clients draw. */
struct Track {
    LockState state = LockState::Off;
    std::string why;               // a refusal or an ending, in words
    std::string satId, satName, txLabel, mode, loop, consumer;
    long long norad = 0;
    double nominalHz = 0;          // the dial
    double shiftHz = 0;            // applied: model + trim (0 unless TRACK/COAST)
    double modelHz = 0, trimHz = 0, rateHzS = 0;
    double followHz = 0;           // ★★★ beta94 the transponder finder's follow (part of shiftHz): where the person is
    double azDeg = 0, elDeg = -90, rangeKm = 0;
    double aos = 0, los = 0, maxElDeg = 0;   // the pass being tracked / armed for
    double ageDays = -1;
    std::string ageState = "none";
    std::string time = "ok";       // "ok" | "system" (clock unmeasured but plausible) | "none"
    bool approaching() const { return shiftHz > 0; }   // ▲ approaching = received high, ▼ receding
};

class Engine {
public:
    static constexpr double kMatchHz = 12000.0;      // brief §8: a downlink within ±12 kHz of the dial
    static constexpr double kTrimClampHz = 2500.0;   // brief §4.2: ±2–3 kHz around the model
    static constexpr double kTrimSlewHzS = 100.0;    // the applied trim moves at most this fast: 4 Hz a 25 Hz tick
    /** ★★ beta92 a telemetry decoder's trim (setDecoderTrim) moves slower still: 2 Hz a tick — a ramp its Costas loop
     *  follows with a degree or two of phase error, so a frame being read is not lost while the channel moves. */
    static constexpr double kDecTrimSlewHzS = 50.0;
    static constexpr double kEndTuneHz = 15000.0;
    /** ★★★ beta94 the finder's follow: at most this far from the nominal (a station's own uplink drift over a pass at
     *  2 m is ~±3.5 kHz on a 70 cm downlink, plus their error), moved at most this fast (a few Hz a 25 Hz tick). */
    static constexpr double kFollowClampHz = 6000.0;
    static constexpr double kFollowSlewHzS = 150.0;    // brief §9: a tune further than this ends the lock (single channels)
    static constexpr double kPassbandSlackHz = 3000.0;   // beta90 1.4: a passband's edge allowance beyond the Doppler
    static constexpr double kPassCacheSec = 300.0;
    static constexpr double kGeoMarginSec = 600.0;   // beta90 1.7: the dotted path runs 10 min either side of the pass
    static constexpr double kLosHoldSec = 3.0;       // below the horizon this long = LOS (no flicker at the edge)
    static constexpr double kOrphanArmSec = 1800.0;  // brief §9 TRAP: an unattended arm more than 30 min from AOS goes

    Engine() : defs_(curatedCatalogue()) { sats_.resize(defs_.size()); }

    // ── inputs ──
    void setObserver(double lat, double lon, double altM = 0) {
        std::lock_guard<std::mutex> lk(m_);
        obs_.set(lat, lon, altM);
        for (auto& s : sats_) s.passesAt = 0;
        skyAt_ = 0;
    }
    /** ★ beta94 No observer any more (Local Hardware's CLEAR with no phone position — vibe_sat_observer.h): the sky goes,
     *  "no location" comes back. Passes are dropped as setObserver does. */
    void clearObserver() {
        std::lock_guard<std::mutex> lk(m_);
        obs_ = Observer();
        for (auto& s : sats_) s.passesAt = 0;
        skyAt_ = 0;
    }
    bool haveObserver() const { std::lock_guard<std::mutex> lk(m_); return obs_.valid; }
    /** ★ This server's place (false = not set) — LRPT's map outline checks the satellite was above our horizon. */
    bool observer(Observer& o) const { std::lock_guard<std::mutex> lk(m_); o = obs_; return obs_.valid; }
    /** ★★ sats.json (vibesdr.net's mirror) → the LIST and the element sets, replacing the built-in list. Returns how
     *  many listed birds have elements, -1 = refused (the current list and elements stay). A bird kept from the old
     *  list keeps its elements if the new file has none or older ones; a running lock follows its bird and transmitter
     *  (by catalogue number and label) or ends when the new list no longer has them. */
    int loadSats(const std::string& text, std::string* errOut = nullptr) {
        std::vector<SatDef> defs; std::vector<Elements> els; std::vector<bool> have; SatsMeta meta; std::string err;
        LabelSet labels;
        if (!parseSats(text, defs, els, have, meta, err, &labels)) { if (errOut) *errOut = err; return -1; }
        std::lock_guard<std::mutex> lk(m_);
        {   // ★★ beta91 the label-only birds: their own rows, never in defs_ (nothing that offers or locks sees them)
            std::vector<SatRow> lr(labels.defs.size());
            for (size_t i = 0; i < labels.defs.size(); ++i) {
                Sgp4 sg;
                if (labels.have[i] && sg.init(labels.els[i])) { lr[i].have = true; lr[i].el = labels.els[i]; lr[i].sgp = sg; }
            }
            labelDefs_ = std::move(labels.defs); labelSats_ = std::move(lr);
            skyAt_ = 0;
        }
        std::vector<SatRow> rows(defs.size());
        for (size_t i = 0; i < defs.size(); ++i) {
            const SatRow* old = nullptr;
            for (size_t k = 0; k < defs_.size(); ++k) if (defs_[k].norad == defs[i].norad && sats_[k].have) old = &sats_[k];
            if (have[i]) {
                Sgp4 sg;
                if (sg.init(els[i]) && !(old && old->el.epochJd > els[i].epochJd)) { rows[i].have = true; rows[i].el = els[i]; rows[i].sgp = sg; }
            }
            if (!rows[i].have && old) { rows[i].have = true; rows[i].el = old->el; rows[i].sgp = old->sgp; }
        }
        for (auto it = locks_.begin(); it != locks_.end();) {
            Lock& L = it->second;
            const long long nd = defs_[L.s].norad; const std::string lab = defs_[L.s].tx[L.t].label;
            int ns = -1, nt = -1;
            for (size_t i = 0; i < defs.size() && ns < 0; ++i)
                if (defs[i].norad == nd)
                    for (size_t k = 0; k < defs[i].tx.size(); ++k) if (defs[i].tx[k].label == lab) { ns = (int)i; nt = (int)k; break; }
            if (ns < 0 || !rows[ns].have) { it = locks_.erase(it); continue; }
            L.s = ns; L.t = nt; ++it;
        }
        defs_ = std::move(defs); sats_ = std::move(rows); meta_ = meta;
        int n = 0; for (auto& r : sats_) if (r.have) ++n;
        return n;
    }
    /** ★ beta91: how many label-only birds are held (and how many have elements). */
    int labelCount(int* withElements = nullptr) const {
        std::lock_guard<std::mutex> lk(m_);
        int n = 0; for (auto& r : labelSats_) if (r.have) ++n;
        if (withElements) *withElements = n;
        return (int)labelDefs_.size();
    }

    // ── ★★ beta91 SATELLITE LABELS (docs/v12/SAT-LABELS.md — Stuart 2026-11-22: "we dont even need to be in satellite
    //    mode either, i could simply be listening to 2m band and if it appears we can label it") ──
    static constexpr double kSkyEverySec = 4.0;      // the sky is looked at once in this long, whoever asks
    static constexpr size_t kSkyLabelMax = 24;       // the most labels one message carries (highest birds first)
    /** Every listed downlink (and every label-only bird's) of a bird ABOVE THE HORIZON whose received signal falls in
     *  [lo, hi] Hz — at its Doppler-shifted frequency. Highest elevation first, ≤ `max`. Nothing without a location,
     *  without a trustworthy clock ("none"), or for a bird whose elements are expired. A downlink SatNOGS marks
     *  inactive is not named (it is not there to see).
     *  ★ COST: one look per bird (and one 1 s ahead for the rate) every kSkyEverySec, shared by every caller — ~100
     *    birds = ~200 SGP4 steps (0.3 µs each on the M5, test-sat-orbit §4); between refreshes a call only filters. */
    std::vector<SkyLabel> skyLabels(const Clock& clk, double lo, double hi, size_t max = kSkyLabelMax) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<SkyLabel> out;
        if (!obs_.valid || !(hi > lo) || timeStateLocked(clk) == "none") return out;
        if (skyAt_ <= 0 || std::fabs(clk.unix - skyAt_) >= kSkyEverySec) refreshSkyLocked(clk);
        for (const SkyBird& b : sky_) {
            const SatDef& d = b.label ? labelDefs_[b.i] : defs_[b.i];
            for (const Transmitter& t : d.tx) {
                if (t.status == "inactive") continue;
                const double c = t.centre();
                const double dop = -c * (b.rrKmS * 1000.0) / kC;
                SkyLabel L;
                L.id = d.id; L.name = d.name; L.tx = t.label; L.norad = d.norad; L.labelOnly = b.label; L.elDeg = b.elDeg;
                /* ★ 2026-11-24: an ORBCOMM bird named over the whole BAND (no single channel known) keeps the beta91 "b" flag on
                 *  its LABEL — so the clients still merge the birds up at once into ONE band label ("ORBCOMM FM114 +2") rather
                 *  than four labels over the same 600 kHz. A tap tunes there, as before; picking it is the SATELLITE list's job. */
                if (t.orbcomm() && t.passband()) L.labelOnly = true;
                L.mode = b.label ? "nfm" : listenModeOf(t);
                L.hz = c + dop;
                L.rateHzS = -c * (b.rrRateKmS2 * 1000.0) / kC;
                if (t.passband()) {
                    L.loHz = t.hz - t.hz * (b.rrKmS * 1000.0) / kC;
                    L.hiHz = t.hiHz - t.hiHz * (b.rrKmS * 1000.0) / kC;
                    if (L.hiHz < lo || L.loHz > hi) continue;
                } else if (L.hz < lo || L.hz > hi) continue;
                out.push_back(std::move(L));
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const SkyLabel& a, const SkyLabel& b) { return a.elDeg > b.elDeg; });
        if (out.size() > max) out.resize(max);
        return out;
    }
    /** The wire message: {"type":"sat_labels","v":1,"t":unix,"sats":[{"id","n","norad","tx","hz","lo"?,"hi"?,"r","el","m","b"?}]}
     *  (`b`:true = label-only). An empty list is a valid message (the client clears). */
    static std::string skyLabelsJson(const std::vector<SkyLabel>& v, double unixNow) {
        std::string j = "{\"type\":\"sat_labels\",\"v\":1,\"t\":" + fmtNum(unixNow, 0) + ",\"sats\":[";
        for (size_t k = 0; k < v.size(); ++k) {
            const SkyLabel& L = v[k];
            if (k) j += ",";
            j += "{\"id\":\"" + jsonEsc(L.id) + "\",\"n\":\"" + jsonEsc(L.name) + "\",\"norad\":" + std::to_string(L.norad)
               + ",\"tx\":\"" + jsonEsc(L.tx) + "\",\"hz\":" + fmtNum(L.hz, 0);
            if (L.hiHz > L.loHz) j += ",\"lo\":" + fmtNum(L.loHz, 0) + ",\"hi\":" + fmtNum(L.hiHz, 0);
            j += ",\"r\":" + fmtNum(L.rateHzS, 1) + ",\"el\":" + fmtNum(L.elDeg, 1) + ",\"m\":\"" + jsonEsc(L.mode) + "\"";
            if (L.labelOnly) j += ",\"b\":true";
            j += "}";
        }
        return j + "]}";
    }
    SatsMeta meta() const { std::lock_guard<std::mutex> lk(m_); return meta_; }
    /** CelesTrak OMM JSON (one group) → element sets for the listed birds. Returns how many listed birds it updated;
     *  a newer epoch replaces an older one, an older never replaces a newer. */
    int loadOmm(const std::string& jsonText) {
        std::vector<json::Object> objs;
        if (!json::parseArray(jsonText, objs)) return -1;
        std::lock_guard<std::mutex> lk(m_);
        int n = 0;
        for (auto& o : objs) {
            double id = 0;
            if (!json::num(o, "NORAD_CAT_ID", id)) continue;
            for (size_t i = 0; i < defs_.size(); ++i) {
                if (defs_[i].norad != (long long)id) continue;
                Elements e; std::string err;
                if (!fromOmm(o, e, err)) continue;
                if (sats_[i].have && sats_[i].el.epochJd >= e.epochJd) continue;
                Sgp4 s;
                if (!s.init(e)) continue;
                sats_[i].have = true; sats_[i].el = e; sats_[i].sgp = s; sats_[i].passesAt = 0;
                ++n;
            }
        }
        return n;
    }
    /** How many listed birds have elements, and the newest epoch (Unix). */
    int elementCount(double* newestEpoch = nullptr) const {
        std::lock_guard<std::mutex> lk(m_);
        int n = 0; double best = 0;
        for (auto& s : sats_) if (s.have) { ++n; best = std::max(best, s.el.epochUnix()); }
        if (newestEpoch) *newestEpoch = best;
        return n;
    }
    /** ★★ V12 LRPT's MAP OUTLINE (vibe_lrpt_overlay.h): every listed bird with an LRPT downlink and an element set — the
     *  METEORs a picture may have come from. `pickedNorad` (the listener's lock, 0 = none) is marked. */
    std::vector<vibelrpt::geo::Candidate> lrptCandidates(long long pickedNorad = 0) const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<vibelrpt::geo::Candidate> out;
        for (size_t i = 0; i < defs_.size() && i < sats_.size(); ++i) {
            if (!sats_[i].have) continue;
            bool lrpt = false;
            for (const auto& t : defs_[i].tx) if (t.consumer == "lrpt" || t.mode == "lrpt") lrpt = true;
            if (!lrpt) continue;
            vibelrpt::geo::Candidate c; c.name = defs_[i].name; c.norad = defs_[i].norad; c.el = sats_[i].el;
            c.picked = pickedNorad > 0 && c.norad == pickedNorad;
            out.push_back(std::move(c));
        }
        return out;
    }
    /** A copy: sats.json can replace the list at any time (loadSats). */
    std::vector<SatDef> defs() const { std::lock_guard<std::mutex> lk(m_); return defs_; }
    /** ★★ 2026-11-24 ORBCOMM: every ORBCOMM bird held (listed, or label-only on an older shape) WITH elements — the
     *  decoder names carriers by them (vibe_orbcomm_id.h) — and the station's location (false = none set). */
    struct ElBird { long long norad = 0; std::string name; Elements el; };
    std::vector<ElBird> orbcommElements() const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<ElBird> v;
        for (size_t i = 0; i < defs_.size() && i < sats_.size(); ++i)
            if (sats_[i].have && orbcommBird(defs_[i])) v.push_back({ defs_[i].norad, defs_[i].name, sats_[i].el });
        for (size_t i = 0; i < labelDefs_.size() && i < labelSats_.size(); ++i)
            if (labelSats_[i].have && orbcommBird(labelDefs_[i])) v.push_back({ labelDefs_[i].norad, labelDefs_[i].name, labelSats_[i].el });
        return v;
    }
    /** ★ SATELLITE MODE's pick: bird `norad`'s downlink number `txIndex` (the index in sat_list's tx[]). false = no such. */
    bool txAt(long long norad, int txIndex, SatDef& sat, Transmitter& tx) const {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& d : defs_)
            if (d.norad == norad) {
                if (txIndex < 0 || txIndex >= (int)d.tx.size()) return false;
                sat = d; tx = d.tx[(size_t)txIndex];
                return true;
            }
        return false;
    }

    /** ★ TIME (brief §5 TRAPs): measured by SNTP = "ok"; unmeasured but within a plausible window of the element sets
     *  (a desktop whose own NTP is fine but whose SNTP to us is blocked) = "system", tracked with the word shown; a
     *  clock before the newest epoch − 1 day or more than 60 days after it (a Pi with no RTC booted offline) = "none":
     *  NO TIME, nothing tracks. */
    std::string timeState(const Clock& c) const {
        if (c.measured) return "ok";
        double newest = 0; elementCount(&newest);
        if (newest <= 0) return "none";
        if (c.unix < newest - 86400.0 || c.unix > newest + 60 * 86400.0) return "none";
        return "system";
    }

    /** "Can this radio receive [lo, hi] Hz?" — the radio's tuning ranges (and a locked window), from the caller. */
    using ReachFn = std::function<bool(double lo, double hi)>;
    static ReachFn rangeReach(double lo, double hi) { return [lo, hi](double a, double b) { return a >= lo && b <= hi; }; }
    /** Is any listed downlink receivable? — the reach gate (no dead controls). */
    bool reaches(const ReachFn& r) const {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& d : defs_) for (auto& t : d.tx)
            if (r(t.hz, t.passband() ? t.hiHz : t.hz)) return true;
        return false;
    }
    bool reaches(double lo, double hi) const { return reaches(rangeReach(lo, hi)); }

    // ── the lock ──
    /** Press Doppler at `dialHz` (brief §8). Matches the listed downlinks within ±12 kHz; one up → TRACK; several →
     *  the highest; none up but one will rise → ARMED for its AOS; else refused (Track.state Off, why set).
     *  ★ `wantNorad` > 0: the listener chose a bird in the picker — only that bird's downlinks match. */
    Track start(const std::string& key, const std::string& owner, double dialHz, const std::string& mode, const Clock& clk,
                long long wantNorad = 0, int wantTx = -1) {
        std::lock_guard<std::mutex> lk(m_);
        Track out;
        out.time = timeStateLocked(clk);
        if (!obs_.valid) { out.why = "This server has no location set, so it cannot work out where satellites are"; return out; }
        if (out.time == "none") { out.why = "NO TIME — this server's clock has not been checked yet"; return out; }
        struct Cand { int s, t; double el, aos; Pass pass; };
        std::vector<Cand> cands;
        bool anyElements = false, anyExpired = false;
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (wantNorad > 0 && defs_[i].norad != wantNorad) continue;
            for (size_t k = 0; k < defs_[i].tx.size(); ++k) {
                if (wantNorad > 0 && wantTx >= 0 && (int)k != wantTx) continue;   // ★ SATELLITE MODE's pick: that downlink only
                const Transmitter& t = defs_[i].tx[k];
                const bool near = t.passband() ? (dialHz >= t.hz - kMatchHz && dialHz <= t.hiHz + kMatchHz)
                                               : std::fabs(dialHz - t.hz) <= kMatchHz;
                if (!near) continue;
                SatRow& r = sats_[i];
                if (!r.have) continue;
                anyElements = true;
                const double age = (clk.unix - r.el.epochUnix()) / 86400.0;
                if (ageOf(age) == Age::Expired || ageOf(age) == Age::None) { anyExpired = true; continue; }
                Look L = look(r.sgp, obs_, clk.unix);
                Cand c{(int)i, (int)k, L.ok ? L.elDeg : -90, 0, Pass{}};
                auto ps = passesLocked(i, clk.unix);
                for (auto& p : ps) if (p.los > clk.unix) { c.pass = p; c.aos = p.aos; break; }
                cands.push_back(c);
            }
        }
        if (cands.empty()) {
            out.why = anyExpired ? "The satellite's orbit data is too old to track (over 14 days)"
                    : anyElements || wantNorad > 0 ? "No listed satellite transmits within 12 kHz of this frequency"
                    : "NO SAT — no listed satellite downlink near this frequency";
            if (!anyElements && wantNorad <= 0) {
                bool listed = false;
                for (auto& d : defs_) for (auto& t : d.tx)
                    if (t.passband() ? (dialHz >= t.hz - kMatchHz && dialHz <= t.hiHz + kMatchHz) : std::fabs(dialHz - t.hz) <= kMatchHz) listed = true;
                if (listed) out.why = "NO SAT DATA — the orbit data has not been downloaded yet";
            }
            return out;
        }
        // highest elevation first; among those below the horizon, the soonest AOS
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            const bool ua = a.el >= 0, ub = b.el >= 0;
            if (ua != ub) return ua;
            if (ua) return a.el > b.el;
            const double xa = a.aos > 0 ? a.aos : 1e300, xb = b.aos > 0 ? b.aos : 1e300;
            return xa < xb;
        });
        const Cand& c = cands.front();
        if (c.el < 0 && c.aos <= 0) { out.why = "That satellite does not rise here in the next day"; return out; }
        Lock L;
        L.s = c.s; L.t = c.t; L.owner = owner; L.mode = mode; L.pass = c.pass; L.startedAt = clk.unix;
        L.lastMeasure = clk.unix;   // ★ TRACK, not COAST, for the first moment: nothing has been heard yet either way
        const Transmitter& t = defs_[c.s].tx[c.t];
        L.nominal = t.hz + listenOffsetHz(t);   // ★ beta90: FUNcube BPSK's dial 1200 Hz low (its audio centre)
        if (t.passband()) {
            // ★ A linear transponder: the dial the listener is on stays where the signal is — the nominal is the
            //   satellite-frame frequency that puts the channel exactly there now (no jump on engagement).
            double nom = dialHz;
            if (c.el >= 0) { Look lk2 = look(sats_[c.s].sgp, obs_, clk.unix); if (lk2.ok) nom = dialHz - lk2.dopplerHz(dialHz); }
            L.nominal = std::max(t.nominalLo(), std::min(t.nominalHi(), nom));   // ★★★ beta94 a finder's span (nominalLo/Hi)
        }
        L.state = c.el >= 0 ? LockState::Track : LockState::Armed;
        locks_[key] = L;
        return trackLocked(key, clk);
    }
    /** End it (the button again, a deliberate tune, LOS). The caller freezes the channel at nominal + shift. */
    void stop(const std::string& key) { std::lock_guard<std::mutex> lk(m_); locks_.erase(key); }
    bool active(const std::string& key) const { std::lock_guard<std::mutex> lk(m_); return locks_.count(key) != 0; }
    std::vector<std::string> keys() const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::string> v; for (auto& kv : locks_) v.push_back(kv.first); return v;
    }
    /** ★ A small tune while locked (brief §9: "small fine-tune nudges adjust trim rather than cancel") — the listener
     *  moved the dial by less than 15 kHz: the nominal follows their dial, the lock goes on. Returns false when the
     *  move is a deliberate tune away (the caller ends the lock).
     *  ★★ beta90 1.4 THE PASSBAND RULE (Stuart's "still open" 1, approved): on a linear transponder ANY tune that lands
     *     inside the Doppler-shifted passband (lo − |shift| − kPassbandSlackHz … hi + |shift| + slack) re-bases the
     *     lock there — FO-29's 100 kHz and RS-44's 60 kHz could not be tuned across under the 15 kHz rule. Only a tune
     *     outside it ends the lock. The nominal is clamped to the passband (the satellite frame). */
    bool nudge(const std::string& key, double newDialHz) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        if (it == locks_.end()) return false;
        Lock& L = it->second;
        const Transmitter& t = defs_[L.s].tx[L.t];
        ++L.seq;                                          // ★★★ beta94 the listener tuned: a follow ends, the dial is theirs
        L.following = false; L.follow = L.followTarget = 0;
        if (t.passband()) {
            const double edge = std::fabs(L.last.shiftHz) + std::fabs(L.last.modelHz - L.last.shiftHz) + kPassbandSlackHz;
            if (newDialHz < t.nominalLo() - edge || newDialHz > t.nominalHi() + edge) return false;
            L.nominal = std::max(t.nominalLo(), std::min(t.nominalHi(), newDialHz));   // ★★★ beta94 a finder's span: beacon / telemetry side too
            return true;
        }
        if (std::fabs(newDialHz - L.nominal) > kEndTuneHz) return false;
        L.nominal = newDialHz;
        return true;
    }
    /** The closed loop's input: a residual (Hz, signal above channel centre = positive) measured with the gate open. */
    void noteResidual(const std::string& key, double hz, double now) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        if (it == locks_.end()) return;
        Lock& L = it->second;
        if (defs_[L.s].tx[L.t].loop != "fll" || (L.state != LockState::Track && L.state != LockState::Coast)) return;
        if (!std::isfinite(hz) || std::fabs(hz) > kTrimClampHz * 2) return;   // ★ a far-off figure is another carrier
        // ★ type-2 on 0.4 s windows. The target is set from the trim APPLIED NOW plus a share of the error — never by
        //   accumulating errors the slew has not caught up with yet (that winds up and rings: the first version did,
        //   ±500 Hz around the answer). The rate term follows a residual ramp (a clock error grows with the Doppler
        //   rate near TCA) with no steady-state lag.
        L.trimTarget = std::max(-kTrimClampHz, std::min(kTrimClampHz, L.trim + 0.5 * hz));
        L.trimRate = std::max(-50.0, std::min(50.0, L.trimRate + 0.08 * hz));
        L.lastMeasure = now;
    }
    /** ★★★ beta92 THE DECODER'S TRIM — a BPSK telemetry pick (tlmWide) whose decoder is locked says where the signal
     *  really is; the lock's trim takes it there (the channel follows the satellite's real frequency: ppm, the listed
     *  figure, transmitter drift). Only on such a downlink, only while TRACK / COAST. The trim is clamped to
     *  ±kTrimClampHz and slewed at kDecTrimSlewHzS by tick(). The POLICY (when to ask, how far) is the decoder's
     *  (vibefc::TrimFollow): it asks only once the trim has arrived and its own estimate is steady, so the two never
     *  chase each other. `applied` is the trim in the channel now, `target` where it is going. */
    bool decoderTrim(const std::string& key, double& applied, double& target) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        if (it == locks_.end()) return false;
        const Lock& L = it->second;
        if (!tlmWide(defs_[L.s].tx[L.t]) || (L.state != LockState::Track && L.state != LockState::Coast)) return false;
        applied = L.trim; target = L.trimTarget;
        return true;
    }
    bool setDecoderTrim(const std::string& key, double targetHz) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        if (it == locks_.end() || !std::isfinite(targetHz)) return false;
        Lock& L = it->second;
        if (!tlmWide(defs_[L.s].tx[L.t]) || (L.state != LockState::Track && L.state != LockState::Coast)) return false;
        L.trimTarget = std::max(-kTrimClampHz, std::min(kTrimClampHz, targetHz));
        return true;
    }
    /** ★★★ beta94 THE TRANSPONDER FINDER'S FOLLOW (docs/v12/TRANSPONDER-FINDER.md): VFO A's channel goes to `satHz` (a
     *  satellite-frame frequency — where the station the listener chose with LISTEN really is), as an offset from the
     *  nominal clamped to ±kFollowClampHz and slewed by tick(). Only on a linear transponder, only while TRACK / COAST.
     *  The listener's own tune (nudge) ends it and bumps `seq`. */
    bool setFollow(const std::string& key, double satHz) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        if (it == locks_.end() || !std::isfinite(satHz)) return false;
        Lock& L = it->second;
        if (!defs_[L.s].tx[L.t].passband() || (L.state != LockState::Track && L.state != LockState::Coast)) return false;
        L.followTarget = std::max(-kFollowClampHz, std::min(kFollowClampHz, satHz - L.nominal));
        L.following = true;
        return true;
    }
    /** Stop following (the dial's channel eases back to the nominal). */
    void clearFollow(const std::string& key) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        if (it == locks_.end()) return;
        it->second.following = false; it->second.followTarget = 0;
    }
    /** The follow's state: the nominal (the dial), whether it follows, the offset applied now and the tune count. */
    struct FollowState { bool lock = false, passband = false, following = false; double nominal = 0, follow = 0; unsigned seq = 0; };
    FollowState followState(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_);
        FollowState f;
        auto it = locks_.find(key);
        if (it == locks_.end()) return f;
        const Lock& L = it->second;
        f.lock = L.state == LockState::Track || L.state == LockState::Coast;
        f.passband = defs_[L.s].tx[L.t].passband();
        f.following = L.following; f.nominal = L.nominal; f.follow = L.follow; f.seq = L.seq;
        return f;
    }
    /** This tick's state for `key` — what to apply and what to show. ★ Call at the tracker's rate (25 Hz); it advances
     *  the slew-limited trim, the ARMED → TRACK step at AOS and LOS. */
    Track tick(const std::string& key, const Clock& clk) {
        std::lock_guard<std::mutex> lk(m_);
        return trackLocked(key, clk);
    }
    /** ★ HOOK FOR DECODERS: the bird `key`'s lock is on (or nothing), without advancing anything. */
    Track selected(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = locks_.find(key);
        return it == locks_.end() ? Track{} : it->second.last;
    }
    /** ★ HOOK FOR DECODERS: Doppler (Hz, received − transmitted) and its rate (Hz/s) for `norad` at `hz` and time `t`,
     *  from the model alone. false when the bird is not listed / has no elements / no location. */
    bool dopplerFor(long long norad, double hz, double t, double& dopplerHz, double& rateHzS, double* elDeg = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        if (!obs_.valid) return false;
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].norad != norad || !sats_[i].have) continue;
            Look a = look(sats_[i].sgp, obs_, t), b = look(sats_[i].sgp, obs_, t + 0.5);
            if (!a.ok || !b.ok) return false;
            dopplerHz = a.dopplerHz(hz);
            rateHzS = (b.dopplerHz(hz) - dopplerHz) / 0.5;
            if (elDeg) *elDeg = a.elDeg;
            return true;
        }
        return false;
    }
    /** ★ beta90 (Decided 4): the LOS of the pass `norad` is in now (AOS − kPassActiveLeadSec … LOS), else 0 — a
     *  listener on a picked pass is not idle-timed-out until then (as WEFAX). */
    double activeUntil(long long norad, double now) {
        std::lock_guard<std::mutex> lk(m_);
        if (!obs_.valid) return 0;
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].norad != norad || !sats_[i].have) continue;
            for (auto& p : passesLocked(i, now)) if (p.aos - kPassActiveLeadSec <= now && p.los > now) return p.los;
        }
        return 0;
    }
    static constexpr double kPassActiveLeadSec = 120.0;   // picked a couple of minutes before AOS: waiting counts
    /** ★ beta90 unattended passes: the downlink of `norad` labelled `label` → its index (−1 = not listed now). */
    int txIndex(long long norad, const std::string& label) const {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& d : defs_) if (d.norad == norad)
            for (size_t k = 0; k < d.tx.size(); ++k) if (d.tx[k].label == label) return (int)k;
        return -1;
    }
    /** ★ beta90: the pass of `norad` in progress at `now` (AOS ≤ now + lead, LOS > now) — false when none. */
    bool passAt(long long norad, double now, Pass& out, double leadSec = 0) {
        std::lock_guard<std::mutex> lk(m_);
        if (!obs_.valid) return false;
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].norad != norad || !sats_[i].have) continue;
            for (auto& p : passesLocked(i, now)) if (p.aos - leadSec <= now && p.los > now) { out = p; return true; }
        }
        return false;
    }
    /** ★★★ beta94: how many ORBCOMM birds are above the horizon at `t` (one SGP4 look each — ~60 birds, every 2 s on the
     *  housekeeping thread: unattended ORBCOMM monitoring runs while this is > 0). 0 with no location. */
    int orbcommUp(double t) {
        std::lock_guard<std::mutex> lk(m_);
        if (!obs_.valid) return 0;
        int n = 0;
        for (size_t i = 0; i < defs_.size() && i < sats_.size(); ++i)
            if (sats_[i].have && orbcommBird(defs_[i])) { const Look L = look(sats_[i].sgp, obs_, t); if (L.ok && L.elDeg > 0) n++; }
        for (size_t i = 0; i < labelDefs_.size() && i < labelSats_.size(); ++i)
            if (labelSats_[i].have && orbcommBird(labelDefs_[i])) { const Look L = look(labelSats_[i].sgp, obs_, t); if (L.ok && L.elDeg > 0) n++; }
        return n;
    }
    /** ★★★ beta94: the next pass of `norad` (in progress, or to come in the next day) peaking at ≥ `minElDeg` — the admin
     *  line's "no Meteor pass for 1 h 12 min". false = none. */
    bool nextPass(long long norad, double now, double minElDeg, Pass& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (!obs_.valid) return false;
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].norad != norad || !sats_[i].have) continue;
            for (auto& p : passesLocked(i, now)) if (p.los > now && p.maxElDeg >= minElDeg) { out = p; return true; }
        }
        return false;
    }
    /** ★ The brief's orphan rule: with nobody connected, an ARMED lock more than 30 minutes from its AOS goes. Returns
     *  the keys removed. Active locks run to LOS unattended. */
    std::vector<std::string> dropOrphanArms(const Clock& clk) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::string> gone;
        for (auto it = locks_.begin(); it != locks_.end();) {
            if (it->second.state == LockState::Armed && it->second.pass.aos - clk.unix > kOrphanArmSec) { gone.push_back(it->first); it = locks_.erase(it); }
            else ++it;
        }
        return gone;
    }

    /** The picker's list: every listed bird, its elements' age, where it is now and its next passes; `lo`/`hi` = the
     *  radio's tuning range (a downlink outside it is marked "reach":false — the client does not offer it).
     *  ★ Bounded: ≤ 4 passes a bird in the next 24 h. */
    std::string listJson(const Clock& clk, double lo, double hi) { return listJson(clk, rangeReach(lo, hi)); }
    std::string listJson(const Clock& clk, const ReachFn& reachFn) {
        return listJson(clk, ReachWhyFn([reachFn](double lo, double hi) {
            return reachFn(lo, hi) ? std::string() : std::string("This receiver cannot tune to it"); }), SupplyInfo{});
    }
    /** ★★ V12 SATELLITE MODE (THE CONTRACT): + `source.synced` / `source.fetching` (when `supply.known`), per bird
     *  `inRangeFor` (s to LOS while up), per tx `use` (voice | image | packet), `sstv:true` on a Voice / SSTV downlink,
     *  and `why` beside every `reach:false` (the clients grey it and show this). */
    std::string listJson(const Clock& clk, const ReachWhyFn& whyFn, const SupplyInfo& supply) {
        std::lock_guard<std::mutex> lk(m_);
        std::string j = "{\"type\":\"sat_list\",\"time\":\"" + timeStateLocked(clk) + "\",\"located\":" + (obs_.valid ? "true" : "false");
        j += ",\"now\":" + fmtNum(clk.unix, 1);
        {
            // ★ the credit (CelesTrak's data, SatNOGS's where used) and the data's age — the picker shows both
            double newest = 0; for (auto& r : sats_) if (r.have) newest = std::max(newest, r.el.epochUnix());
            j += ",\"source\":{\"orbits\":\"" + jsonEsc(meta_.orbitsCredit) + "\"";
            if (!meta_.txCredit.empty()) j += ",\"transmitters\":\"" + jsonEsc(meta_.txCredit) + "\"";
            j += ",\"via\":\"" + std::string(meta_.fromMirror ? "vibesdr.net" : "built-in") + "\"";
            if (meta_.orbitsAt > 0) j += ",\"fetched\":" + fmtNum(meta_.orbitsAt, 0);
            if (newest > 0) j += ",\"newestEpoch\":" + fmtNum(newest, 0);
            if (supply.known) j += ",\"synced\":" + fmtNum(supply.synced, 0) + ",\"fetching\":" + (supply.fetching ? "true" : "false");
            if (supply.pickPrivate >= 0) j += std::string(",\"pickPrivate\":") + (supply.pickPrivate ? "true" : "false");
            j += "}";
        }
        j += ",\"sats\":[";
        bool first = true;
        for (size_t i = 0; i < defs_.size(); ++i) {
            const SatDef& d = defs_[i];
            SatRow& r = sats_[i];
            if (!first) j += ",";
            first = false;
            j += "{\"id\":\"" + jsonEsc(d.id) + "\",\"name\":\"" + jsonEsc(d.name) + "\",\"norad\":" + std::to_string(d.norad);
            j += ",\"note\":\"" + jsonEsc(d.note) + "\"";
            const double age = r.have ? (clk.unix - r.el.epochUnix()) / 86400.0 : -1;
            j += ",\"age\":" + fmtNum(age, 2) + ",\"ageState\":\"" + std::string(r.have ? ageName(ageOf(age)) : "none") + "\"";
            j += ",\"tx\":[";
            for (size_t k = 0; k < d.tx.size(); ++k) {
                const Transmitter& t = d.tx[k];
                if (k) j += ",";
                const std::string why = whyFn ? whyFn(t.hz, t.passband() ? t.hiHz : t.hz) : std::string();
                const bool reach = why.empty();
                j += "{\"label\":\"" + jsonEsc(t.label) + "\",\"hz\":" + fmtNum(t.hz, 0);
                if (t.passband()) j += ",\"hiHz\":" + fmtNum(t.hiHz, 0);
                j += ",\"mode\":\"" + t.mode + "\",\"loop\":\"" + t.loop + "\"";
                if (!t.consumer.empty()) j += ",\"consumer\":\"" + t.consumer + "\"";
                if (t.upHz > 0) j += ",\"upHz\":" + fmtNum(t.upHz, 0);
                if (t.ctcss > 0) j += ",\"ctcss\":" + fmtNum(t.ctcss, 1);
                if (!t.note.empty()) j += ",\"note\":\"" + jsonEsc(t.note) + "\"";
                if (!t.status.empty()) j += ",\"status\":\"" + t.status + "\"";
                j += std::string(",\"use\":\"") + useOf(t) + "\"";
                if (sstvOf(t)) j += ",\"sstv\":true";
                {   // ★ beta90: the band label, the catalogue's mode name, the decoder a pick attaches
                    const std::string b = bandOf(t);
                    if (!b.empty()) j += ",\"band\":\"" + b + "\"";
                    if (!t.modeName.empty()) j += ",\"modeName\":\"" + jsonEsc(t.modeName) + "\"";
                    std::string prm;
                    j += ",\"decoder\":\"" + decoderOf(t, prm) + "\"";
                }
                j += std::string(",\"reach\":") + (reach ? "true" : "false");
                if (!reach) j += ",\"why\":\"" + jsonEsc(why) + "\"";
                j += "}";
            }
            j += "]";
            if (r.have && obs_.valid) {
                Look L = look(r.sgp, obs_, clk.unix);
                if (L.ok) j += ",\"az\":" + fmtNum(L.azDeg, 1) + ",\"el\":" + fmtNum(L.elDeg, 1) + ",\"rangeKm\":" + fmtNum(L.rangeKm, 0)
                            + ",\"lat\":" + fmtNum(L.subLatDeg, 2) + ",\"lon\":" + fmtNum(L.subLonDeg, 2);
                auto ps = passesLocked(i, clk.unix);
                // ★ "in range for 09:55": while it is up, the seconds to this pass's LOS (absent below the horizon)
                if (L.ok && L.elDeg >= 0)
                    for (auto& p : ps)
                        if (p.aos <= clk.unix + 5 && p.los > clk.unix) { j += ",\"inRangeFor\":" + fmtNum(p.los - clk.unix, 0); break; }
                j += ",\"passes\":[";
                int np = 0;
                for (auto& p : ps) {
                    if (p.los < clk.unix || np >= 4) continue;
                    if (np++) j += ",";
                    j += "{\"aos\":" + fmtNum(p.aos, 0) + ",\"tca\":" + fmtNum(p.tca, 0) + ",\"los\":" + fmtNum(p.los, 0)
                       + ",\"maxEl\":" + fmtNum(p.maxElDeg, 1) + ",\"aosAz\":" + fmtNum(p.aosAzDeg, 0) + ",\"losAz\":" + fmtNum(p.losAzDeg, 0) + "}";
                }
                j += "]";
            }
            j += "}";
        }
        j += "]}";
        return j;
    }

    /** The sky track for one pass of `norad` (the picker's polar plot): az/el every `stepSec` from AOS to LOS, ≤ 120
     *  points. Empty when unknown. */
    std::string trackJson(long long norad, double aos, double los, double stepSec = 15) {
        std::lock_guard<std::mutex> lk(m_);
        std::string j = "[";
        if (!obs_.valid || los <= aos) return "[]";
        stepSec = std::max(stepSec, (los - aos) / 120.0);
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].norad != norad || !sats_[i].have) continue;
            bool first = true;
            for (double t = aos; t <= los + 0.01; t += stepSec) {
                Look L = look(sats_[i].sgp, obs_, std::min(t, los));
                if (!L.ok) continue;
                if (!first) j += ",";
                first = false;
                j += "[" + fmtNum(L.azDeg, 1) + "," + fmtNum(std::max(0.0, L.elDeg), 1) + "]";
            }
        }
        return j + "]";
    }

    /** ★ beta90 1.7: the pass's ground track for the half-globe — the sub-satellite point [lat, lon, altKm] from
     *  AOS − 10 min to LOS + 10 min, ≤ 120 points evenly in time. Fills t0/t1/step; "" when unknown. Also the pass's
     *  TCA (the highest point) if a cached pass overlaps [aos, los]. */
    std::string geoJson(long long norad, double aos, double los, double& t0, double& t1, double& step, double& tca) {
        std::lock_guard<std::mutex> lk(m_);
        t0 = t1 = step = tca = 0;
        if (!obs_.valid || !(los > aos) || los - aos > 6 * 3600) return "";
        for (size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].norad != norad || !sats_[i].have) continue;
            t0 = aos - kGeoMarginSec; t1 = los + kGeoMarginSec;
            step = std::max(10.0, (t1 - t0) / 119.0);
            std::string j = "[";
            int n = 0;
            for (double t = t0; t <= t1 + 0.01 && n < 120; t += step, ++n) {
                Look L = look(sats_[i].sgp, obs_, std::min(t, t1));
                if (n) j += ",";
                if (!L.ok) { j += "null"; continue; }   // ★ keeps geo[i] at t0 + i·step
                j += "[" + fmtNum(L.subLatDeg, 3) + "," + fmtNum(L.subLonDeg, 3) + "," + fmtNum(L.altKm, 0) + "]";
            }
            for (auto& p : sats_[i].passes) if (p.aos < los && p.los > aos) { tca = p.tca; break; }
            if (tca <= 0) {   // ★ not cached: the highest sample of the window inside AOS…LOS
                double best = -91;
                for (double t = aos; t <= los; t += std::max(5.0, (los - aos) / 120.0)) {
                    Look L = look(sats_[i].sgp, obs_, t);
                    if (L.ok && L.elDeg > best) { best = L.elDeg; tca = t; }
                }
            }
            return j + "]";
        }
        return "";
    }

    /** The per-listener readout (`{"type":"sat", …}`), from a Track. */
    static std::string trackJsonMsg(const Track& t) {
        std::string j = "{\"type\":\"sat\",\"state\":\"" + std::string(lockName(t.state)) + "\"";
        if (!t.why.empty()) j += ",\"why\":\"" + jsonEsc(t.why) + "\"";
        if (t.state != LockState::Off) {
            j += ",\"id\":\"" + jsonEsc(t.satId) + "\",\"name\":\"" + jsonEsc(t.satName) + "\",\"norad\":" + std::to_string(t.norad);
            j += ",\"tx\":\"" + jsonEsc(t.txLabel) + "\",\"loop\":\"" + t.loop + "\"";
            if (!t.consumer.empty()) j += ",\"consumer\":\"" + t.consumer + "\"";
            j += ",\"nominal\":" + fmtNum(t.nominalHz, 0) + ",\"offset\":" + fmtNum(t.shiftHz, 1) + ",\"rate\":" + fmtNum(t.rateHzS, 1);
            if (t.followHz != 0) j += ",\"follow\":" + fmtNum(t.followHz, 1);   // ★★★ beta94 part of offset: the finder's follow
            j += ",\"trim\":" + fmtNum(t.trimHz, 1) + ",\"az\":" + fmtNum(t.azDeg, 1) + ",\"el\":" + fmtNum(t.elDeg, 1);
            j += ",\"aos\":" + fmtNum(t.aos, 0) + ",\"los\":" + fmtNum(t.los, 0) + ",\"maxEl\":" + fmtNum(t.maxElDeg, 1);
            j += ",\"age\":" + fmtNum(t.ageDays, 2) + ",\"ageState\":\"" + t.ageState + "\"";
        }
        j += ",\"time\":\"" + t.time + "\"}";
        return j;
    }

private:
    struct SatRow {
        bool have = false; Elements el; Sgp4 sgp;
        std::vector<Pass> passes; double passesAt = 0;
    };
    struct Lock {
        int s = 0, t = 0;
        std::string owner, mode;
        double nominal = 0, startedAt = 0;
        LockState state = LockState::Track;
        Pass pass;
        double trimTarget = 0, trim = 0, trimRate = 0, lastMeasure = 0, lastTick = 0, belowSince = 0;
        // ★★★ beta94 VFO A FOLLOWS THE PERSON (setFollow): the channel's offset from the nominal towards where the followed
        //   station really is (their own uplink drift), slewed; `seq` counts the listener's own tunes (nudge) — the finder
        //   stops following the moment it changes. A manual nudge always wins.
        double follow = 0, followTarget = 0;
        bool following = false;
        unsigned seq = 0;
        Track last;
    };

    std::string timeStateLocked(const Clock& c) const {
        if (c.measured) return "ok";
        double newest = 0;
        for (auto& s : sats_) if (s.have) newest = std::max(newest, s.el.epochUnix());
        if (newest <= 0) return "none";
        if (c.unix < newest - 86400.0 || c.unix > newest + 60 * 86400.0) return "none";
        return "system";
    }

    const std::vector<Pass>& passesLocked(size_t i, double now) {
        SatRow& r = sats_[i];
        if (r.passesAt <= 0 || std::fabs(now - r.passesAt) > kPassCacheSec || (!r.passes.empty() && r.passes.front().los < now - 1)) {
            r.passes = r.have && obs_.valid ? predictPasses(r.sgp, obs_, now - 900, 86400 + 900, 0.0, 8) : std::vector<Pass>{};
            // ★ the search started 15 min back so a pass already in progress has its true AOS
            r.passes.erase(std::remove_if(r.passes.begin(), r.passes.end(), [&](const Pass& p) { return p.los < now; }), r.passes.end());
            r.passesAt = now;
        }
        return r.passes;
    }

    Track trackLocked(const std::string& key, const Clock& clk) {
        Track out;
        out.time = timeStateLocked(clk);
        auto it = locks_.find(key);
        if (it == locks_.end()) return out;
        Lock& L = it->second;
        const SatDef& d = defs_[L.s];
        const Transmitter& tx = d.tx[L.t];
        SatRow& r = sats_[L.s];
        out.satId = d.id; out.satName = d.name; out.norad = d.norad; out.txLabel = tx.label;
        out.mode = L.mode; out.loop = tx.loop; out.consumer = tx.consumer; out.nominalHz = L.nominal;
        out.ageDays = (clk.unix - r.el.epochUnix()) / 86400.0;
        out.ageState = ageName(ageOf(out.ageDays));
        const double dt = L.lastTick > 0 ? std::max(0.0, std::min(1.0, clk.unix - L.lastTick)) : 0.0;
        L.lastTick = clk.unix;
        Look now = look(r.sgp, obs_, clk.unix);
        out.azDeg = now.azDeg; out.elDeg = now.elDeg; out.rangeKm = now.rangeKm;
        // the pass: refresh once it has ended (an ARMED lock waits for the next one)
        if (L.pass.los < clk.unix) {
            auto ps = passesLocked(L.s, clk.unix);
            for (auto& p : ps) if (p.los > clk.unix) { L.pass = p; break; }
        }
        out.aos = L.pass.aos; out.los = L.pass.los; out.maxElDeg = L.pass.maxElDeg;
        if (L.state == LockState::Armed) {
            if (now.ok && now.elDeg >= 0) { L.state = LockState::Track; L.trim = L.trimTarget = L.trimRate = 0; L.lastMeasure = clk.unix; }
        } else if (L.state == LockState::Track || L.state == LockState::Coast) {
            if (!now.ok || now.elDeg < 0.0) {
                if (L.belowSince <= 0) L.belowSince = clk.unix;
                if (clk.unix - L.belowSince >= kLosHoldSec) { L.state = LockState::Ended; }
            } else L.belowSince = 0;
        }
        if (L.state == LockState::Track || L.state == LockState::Coast || L.state == LockState::Ended) {
            // model at the actual listening frequency (nominal); the loop's trim, slew-limited
            const double m = now.ok ? now.dopplerHz(L.nominal) : L.last.modelHz;
            Look ahead = look(r.sgp, obs_, clk.unix + 0.5);
            out.rateHzS = (now.ok && ahead.ok) ? (ahead.dopplerHz(L.nominal) - m) / 0.5 : 0;
            if (L.state != LockState::Ended) {
                // COAST: the loop has heard nothing for over a second — the model carries on alone, the trim FROZEN
                //   (brief §3: "on signal loss the loop freezes trim and the model keeps moving")
                const bool fll = tx.loop == "fll";
                L.state = (fll && (L.lastMeasure <= 0 || clk.unix - L.lastMeasure > 1.2)) ? LockState::Coast : LockState::Track;
                if (L.state == LockState::Track && fll) {
                    L.trimTarget += L.trimRate * dt;
                    L.trimTarget = std::max(-kTrimClampHz, std::min(kTrimClampHz, L.trimTarget));
                    const double step = kTrimSlewHzS * dt;
                    L.trim += std::max(-step, std::min(step, L.trimTarget - L.trim));
                }
                // ★★ beta92 a telemetry decoder's trim (setDecoderTrim): no loop of the engine's own — slewed only
                if (L.state == LockState::Track && !fll && tlmWide(tx)) {
                    const double step = kDecTrimSlewHzS * dt;
                    L.trim += std::max(-step, std::min(step, L.trimTarget - L.trim));
                }
                // ★★★ beta94 the finder's follow (passband only): slewed at kFollowSlewHzS, part of the shift like the trim
                if (tx.passband()) {
                    const double step = kFollowSlewHzS * dt;
                    L.follow += std::max(-step, std::min(step, L.followTarget - L.follow));
                }
                out.modelHz = m; out.trimHz = L.trim; out.followHz = L.follow; out.shiftHz = m + L.trim + L.follow;
            } else {
                // ★ ENDED (LOS): frozen where it was — the caller moves the dial there (brief §9: never snap back)
                out.modelHz = L.last.modelHz; out.trimHz = L.last.trimHz; out.shiftHz = L.last.shiftHz; out.rateHzS = 0;
                out.why = "Below the horizon — Doppler correction ended";
            }
        }
        out.state = L.state;
        L.last = out;
        return out;
    }

    /** ★ beta91: one bird above the horizon at skyAt_ — its range rate and that rate's change (for the Doppler's rate). */
    struct SkyBird { bool label = false; size_t i = 0; double elDeg = 0, rrKmS = 0, rrRateKmS2 = 0; };
    void refreshSkyLocked(const Clock& clk) {
        sky_.clear();
        skyAt_ = clk.unix;
        auto scan = [&](std::vector<SatRow>& rows, bool label) {
            for (size_t i = 0; i < rows.size(); ++i) {
                SatRow& r = rows[i];
                if (!r.have) continue;
                const Age a = ageOf((clk.unix - r.el.epochUnix()) / 86400.0);
                if (a == Age::Expired || a == Age::None) continue;
                Look now = look(r.sgp, obs_, clk.unix);
                if (!now.ok || now.elDeg < 0) continue;
                Look ahead = look(r.sgp, obs_, clk.unix + 1.0);
                sky_.push_back({label, i, now.elDeg, now.rangeRateKmS, ahead.ok ? ahead.rangeRateKmS - now.rangeRateKmS : 0.0});
            }
        };
        scan(sats_, false);
        scan(labelSats_, true);
    }

    mutable std::mutex m_;
    std::vector<SatDef> labelDefs_;   // ★★ beta91 label-only birds (ORBCOMM) — never offered, locked or decoded
    std::vector<SatRow> labelSats_;
    std::vector<SkyBird> sky_;
    double skyAt_ = 0;
    std::vector<SatDef> defs_;
    std::vector<SatRow> sats_;
    SatsMeta meta_;
    Observer obs_;
    std::map<std::string, Lock> locks_;
};

// ── ★★ THE HOOK FOR DECODERS (docs/v12/SATELLITE-ENGINE.md "RESUME HERE — the decoder hook") ─────────────────────────
/** The process's one engine (the shim's lock, the server's sky). Any translation unit may call it. */
inline Engine& engine() { static Engine e; return e; }
/** The shared dial's lock key (a session id never starts with \x01). Per-listener locks are keyed by the session id. */
constexpr const char* kSharedKey = "\x01shared";

/** What a decoder needs to know about the satellite its listener is on.
 *  ★★ TWO DIFFERENT DOPPLERS — read the right one:
 *   · `inChannelHz` is ALREADY APPLIED in the listener's channel NCO (the lock moves it 25 times a second). A decoder fed
 *     from that listener's CHANNEL IQ (clientIqCb — "the dial at DC") sees the signal at DC ± the residual only: it must
 *     NOT correct again. (On `costas` downlinks the engine's trim is 0, so inChannelHz == modelHz: the demodulator's own
 *     carrier loop takes the residual.)
 *   · `modelHz` (+ `rateHzS`) is the prediction for `downlinkHz` now. A decoder cutting its OWN slice of the capture at
 *     the nominal (feedSkimFromBins-style, off the shared forward FFT) must apply it itself — feed-forward into its NCO,
 *     with the rate for its timing loop. */
struct DecoderView {
    bool on = false;                 // a lock (TRACK / COAST) is running for this listener or the shared dial
    std::string state;               // "TRACK" | "COAST" | "ARMED" | "ENDED" | "" (none)
    long long norad = 0;
    std::string satId, satName, txLabel, consumer, loop;   // consumer: "lrpt", "ax25-9600", "ax25-1200", "bpsk-telemetry", ""
    double downlinkHz = 0;           // the nominal (the dial)
    double inChannelHz = 0;          // the shift the channel already carries (see above)
    double trimHz = 0;               // ★ beta92: the part of inChannelHz that is the trim (a loop's, or a decoder's)
    double modelHz = 0, rateHzS = 0; // the model's Doppler at downlinkHz now, and its rate
    double elDeg = -90, azDeg = 0, rangeKm = 0;
    double aos = 0, los = 0;         // the pass (UTC Unix seconds)
    double ageDays = -1;             // the element set's age (the model is refused past 14 days)
};
/** The listener `session`'s lock, else the shared dial's, else nothing (`on` false). ★ Cheap (a map look-up under the
 *  engine's mutex, no SGP4): call it per block if you like. The values are the tracker's last tick (≤ 40 ms old). */
inline DecoderView decoderView(const std::string& session) {
    Track t = engine().selected(session);
    if (t.state == LockState::Off) t = engine().selected(kSharedKey);
    DecoderView v;
    if (t.state == LockState::Off) return v;
    v.state = lockName(t.state);
    v.on = t.state == LockState::Track || t.state == LockState::Coast;
    v.norad = t.norad; v.satId = t.satId; v.satName = t.satName; v.txLabel = t.txLabel; v.consumer = t.consumer; v.loop = t.loop;
    v.downlinkHz = t.nominalHz; v.inChannelHz = v.on ? t.shiftHz : 0.0; v.trimHz = v.on ? t.trimHz : 0.0; v.modelHz = t.modelHz; v.rateHzS = t.rateHzS;
    v.elDeg = t.elDeg; v.azDeg = t.azDeg; v.rangeKm = t.rangeKm; v.aos = t.aos; v.los = t.los; v.ageDays = t.ageDays;
    return v;
}
/** The model alone, for any listed bird at any frequency and time — a decoder that runs WITHOUT a listener's lock (a
 *  gallery capture of a Meteor pass, a background decoder). false = not listed / no elements / no location. */
inline bool dopplerNow(long long norad, double hz, double unixUtc, double& dopplerHz, double& rateHzS, double* elDeg = nullptr) {
    return engine().dopplerFor(norad, hz, unixUtc, dopplerHz, rateHzS, elDeg);
}

/** ★★ The LRPT decoder's two questions of a pick (beta91, the live Meteor-M2-4 pass 2026-11-22): the symbol rate the
 *  satellite's entry names (72000 / 80000; 0 = no pick, not an LRPT downlink, or the entry says nothing) — so AUTO does
 *  not spend the pass trying the other rate — and whether the satellite is above the horizon (1 TRACK / COAST, 0 armed
 *  for a later pass or ENDED, −1 no pick at all: a hand-tuned dial). */
inline double lrptSymbolRateOf(const DecoderView& v) {
    if (v.norad == 0) return 0;
    for (const SatDef& d : engine().defs()) {
        if (d.norad != v.norad) continue;
        for (const Transmitter& t : d.tx)
            if ((t.consumer == "lrpt" || t.mode == "lrpt") && t.baud > 0 && (v.downlinkHz <= 0 || std::fabs(t.hz - v.downlinkHz) < 20000))
                return t.baud >= 76000 ? 80000 : 72000;
    }
    return 0;
}
inline int inRangeOf(const DecoderView& v) {
    if (v.state.empty() || v.state == "OFF") return -1;
    return v.on ? 1 : 0;
}

}}  // namespace vibe::sat
