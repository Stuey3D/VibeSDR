// vibe_decoder_reach.h — WHICH DECODERS THIS RADIO CAN USE, AND WHICH ARE OFF HERE BY DEFAULT (V12, 2026-11-03).
//
// Two different questions, kept apart on purpose (Stuart, 2026-11-03, a Dutch server whose USRP is locked to 88–96 MHz
// offering SAME: "what is SAME?" … "dont grey it out let a user enable it if they want but disable it by default"):
//
// ★★★ REACH — "can this radio tune the frequencies at all?" A decoder tied to fixed frequencies (SAME on 162.4 MHz,
//     NAVTEX on 518 kHz, DSC on channel 70 …) cannot work on a radio that cannot get there: a locked range that does not
//     hold them, a driver whose coverage stops short, an RTL with no HF path. Such a key is drawn GREYED WITH THE REASON
//     IN WORDS, numbers included — never hidden (a missing key reads as a missing feature) and never live (a live key
//     that can only ever decode silence reads as a broken one). AGENTS.md: a control that cannot work here is not live.
//     Decoders that work on whatever the listener tunes (RTTY, SSTV, WEFAX, Advanced RDS …) are not in the table.
//     CW, PSK and JTTY are, by the amateur bands — and only if NOT ONE band is reachable.
//
// ★★★ REGION DEFAULT — "is it worth having on, here?" SAME is North America's; DAB is not broadcast in the Americas.
//     That is not a reach question (a European radio CAN tune 162.4 MHz), so it never greys a key. It sets the DEFAULT
//     of the owner's own switch (Setup → Modes and decoders, the blocked-modes list): off by default where it is not
//     used, for a config that has never said. The owner's explicit choice always wins and is never overwritten —
//     off is the id in the list, as ever ("same"); ON against a region default is "+same", the owner saying so.
//     With the default applied it behaves exactly like any owner-blocked decoder. Region unknown (no position given):
//     no region default at all.
//
// ★ Pure and header-only: (ranges, region, CSV) → (answers) — vibeserver/test-decoder-reach.cpp holds it to fake radios.
#pragma once
#include "vibe_bands.h"
#include "decoders/digi_bands.h"
#include <cstdio>
#include <string>
#include <vector>

namespace vibereach {

// ── REACH ─────────────────────────────────────────────────────────────────────────────────────────────────────────

/** One place a decoder listens: [lo, hi] RF Hz. `part` = a BAND, reachable when any of it is (the airband, an amateur
 *  band); otherwise a CHANNEL, reachable only when the whole of it is. */
struct Chan { double lo, hi; bool part = false; };

/** A fixed-frequency decoder: its key id (the hwinfo / blocked-modes id), its name in words, where it lives in words,
 *  and its channels — available if ANY of them is reachable. */
struct Entry {
    const char* id;
    const char* name;
    const char* where;      // "is at …" / "is on …" — completes "<name> <where>"
    std::vector<Chan> chans;
};

inline Chan ch(double hz, double halfHz) { return { hz - halfHz, hz + halfHz, false }; }
inline Chan band(double lo, double hi) { return { lo, hi, true }; }

/** The amateur bands (digital spots' own list, decoders/digi_bands.h) as bands. */
inline std::vector<Chan> amateurBands() {
    std::vector<Chan> v;
    for (const auto& b : vibe::digi::bands()) v.push_back(band(b.loHz, b.hiHz));
    return v;
}

/** ★★ THE TABLE. A new fixed-frequency decoder joins here and every client greys it with no change of its own. */
inline const std::vector<Entry>& table() {
    static const std::vector<Entry> t = [] {
        std::vector<Entry> v;
        // EAS / SAME: NOAA Weather Radio and Weatheradio Canada, seven NFM channels 162.400–162.550 MHz (25 kHz apart).
        { Entry e{ "same", "SAME", "is at 162.400\xe2\x80\x93" "162.550 MHz", {} };
          for (int k = 0; k < 7; ++k) e.chans.push_back(ch(162.400e6 + 25e3 * k, 6.25e3));
          v.push_back(e); }
        // DSC: VHF channel 70, and the MF/HF distress and safety channels (ITU-R M.541) — any one of them will do.
        v.push_back({ "dsc", "DSC",
            "is on 156.525 MHz (VHF channel 70) and 2187.5, 4207.5, 6312, 8414.5, 12577 and 16804.5 kHz",
            { ch(156.525e6, 12.5e3), ch(2187.5e3, 1.5e3), ch(4207.5e3, 1.5e3), ch(6312.0e3, 1.5e3),
              ch(8414.5e3, 1.5e3), ch(12577.0e3, 1.5e3), ch(16804.5e3, 1.5e3) } });
        // APRS: every national 2 m channel vibeaprs::regionalHz knows, and HF APRS at 300 Bd on 30 m (the server reads
        // a baud below 600 as HF).
        v.push_back({ "aprs", "APRS",
            "is on 2 m (144.390\xe2\x80\x93" "145.570 MHz) and 10.1476 MHz",
            { ch(144.390e6, 8e3), ch(144.575e6, 8e3), ch(144.620e6, 8e3), ch(144.640e6, 8e3), ch(144.800e6, 8e3),
              ch(144.930e6, 8e3), ch(145.175e6, 8e3), ch(145.525e6, 8e3), ch(145.570e6, 8e3), ch(10.1476e6, 2e3) } });
        // NAVTEX: the international 518 kHz, the national 490 kHz and the HF 4209.5 kHz.
        v.push_back({ "navtex", "NAVTEX", "is on 518, 490 and 4209.5 kHz",
            { ch(518e3, 1e3), ch(490e3, 1e3), ch(4209.5e3, 1e3) } });
        // SELCAL: the HF aeronautical mobile (R) bands (ITU RR Appendix 27) and the VHF airband.
        v.push_back({ "selcal", "SELCAL",
            "is on the HF aeronautical bands (2.85\xe2\x80\x93" "22 MHz) and the VHF airband (118\xe2\x80\x93" "137 MHz)",
            { band(2850e3, 3155e3), band(3400e3, 3500e3), band(4650e3, 4750e3), band(5450e3, 5730e3),
              band(6525e3, 6765e3), band(8815e3, 9040e3), band(10005e3, 10100e3), band(11275e3, 11400e3),
              band(13260e3, 13360e3), band(17900e3, 18030e3), band(21924e3, 22000e3), band(118e6, 137e6) } });
        // CW, PSK, JTTY: the amateur bands — greyed only when NOT ONE of them is reachable.
        v.push_back({ "cw",   "CW",   "is on the amateur bands (136 kHz to 1.3 GHz)", amateurBands() });
        v.push_back({ "psk",  "PSK",  "is on the amateur bands (136 kHz to 1.3 GHz)", amateurBands() });
        v.push_back({ "jtty", "JTTY", "is on the amateur bands (136 kHz to 1.3 GHz)", amateurBands() });
        return v;
    }();
    return t;
}

inline const Entry* entry(const std::string& id) {
    for (const auto& e : table()) if (id == e.id) return &e;
    return nullptr;
}

/** What the radio can tune, for this question: the ranges, and whether they are an owner's LOCKED window (the words
 *  differ: "outside this radio's locked range" vs "out of this radio's reach"). Empty ranges = unknown: never grey. */
struct Reach {
    vibebands::Ranges ranges;
    bool locked = false;
};

/** ★★ A radio's RF reach from its driver (vibebands::driverCoverage — the table hwinfo and the directory publish), the
 *  converter in front of it (hardware = wanted − offset, clipped to what the converter passes — the benchmark's rule,
 *  vibe_benchmark_hw.h radioFor), the RTL's HF path, and the owner's allow / block lists.
 *  ★ `rtlHf`: an RTL reaches below 24 MHz only through direct sampling (auto or on: the Q branch, 0–14.4 MHz) or a
 *    V4's own upconverter. Without either its R820T tuner starts at 24 MHz (as vsNdbReach says for LF). */
inline vibebands::Ranges reachRanges(const std::string& driverIn, double convOffsetHz = 0, double convLo = 0,
                                     double convHi = 0, int rtlHf = 1, const std::string& allowCsv = "",
                                     const std::string& blockCsv = "") {
    const std::string driver = driverIn == "rtlsdr" ? "rtl" : driverIn;
    vibebands::Ranges hw;
    for (const auto& x : vibebands::driverCoverage(driver)) {
        double lo = x.lo, hi = x.hi;
        if (driver == "rtl" && convOffsetHz == 0) {
            if (rtlHf == 0) lo = std::max(lo, 24e6);                       // tuner only
            else if (rtlHf == 2) { hw.push_back({ 0.0, 14.4e6 }); lo = std::max(lo, 24e6); }   // direct sampling
        }
        lo += convOffsetHz; hi += convOffsetHz;
        if (convOffsetHz != 0 && convHi > convLo) { lo = std::max(lo, convLo); hi = std::min(hi, convHi); }
        if (lo < 0) lo = 0;
        if (hi > lo) hw.push_back({ lo, hi });
    }
    hw = vibebands::normalise(hw);
    if (allowCsv.empty() && blockCsv.empty()) return hw;
    return vibebands::permitted(hw, allowCsv, blockCsv);
}

inline bool reachable(const Chan& c, const vibebands::Ranges& r) {
    for (const auto& x : r) {
        if (c.part ? (x.lo < c.hi && x.hi > c.lo) : (x.lo <= c.lo && x.hi >= c.hi)) return true;
    }
    return false;
}

/** "88.0–96.0 MHz", "500 kHz–31.0 MHz" — a range as a person reads it. */
inline std::string fmtHz(double hz) {
    char b[40];
    if (hz >= 1e9) std::snprintf(b, sizeof b, "%.2f GHz", hz / 1e9);
    else if (hz >= 1e6) std::snprintf(b, sizeof b, "%.1f MHz", hz / 1e6);
    else std::snprintf(b, sizeof b, "%.0f kHz", hz / 1e3);
    return b;
}
inline std::string fmtRange(const vibebands::Range& x) {
    char b[80];
    if (x.lo >= 1e6 && x.hi < 1e9) std::snprintf(b, sizeof b, "%.1f\xe2\x80\x93%.1f MHz", x.lo / 1e6, x.hi / 1e6);
    else std::snprintf(b, sizeof b, "%s\xe2\x80\x93%s", fmtHz(x.lo).c_str(), fmtHz(x.hi).c_str());
    return b;
}
inline std::string fmtRanges(const vibebands::Ranges& r) {
    std::string s;
    for (size_t i = 0; i < r.size(); ++i) s += (i ? (i + 1 == r.size() ? " and " : ", ") : "") + fmtRange(r[i]);
    return s;
}

/** ★★ WHY decoder `id` cannot run on this radio, in words with the numbers — or "" when it can (or is not a
 *  fixed-frequency decoder, or the reach is unknown). The owner's block is NOT asked here: the caller asks it first,
 *  and it wins with its own wording. */
inline std::string whyNot(const std::string& id, const Reach& reach) {
    const Entry* e = entry(id);
    if (!e || reach.ranges.empty()) return {};
    for (const auto& c : e->chans) if (reachable(c, reach.ranges)) return {};
    std::string s;
    if (reach.locked) s = "Outside this radio's locked range (" + fmtRanges(reach.ranges) + ")";
    else if (reach.ranges.size() <= 3) s = "Out of this radio's reach (" + fmtRanges(reach.ranges) + ")";
    else s = "Out of this radio's reach";
    return s + " \xe2\x80\x94 " + e->name + " " + e->where;
}

// ── REGION DEFAULT ────────────────────────────────────────────────────────────────────────────────────────────────

/** A decoder that is OFF BY DEFAULT outside the ITU regions in `onIn` (bit 1 = Region 1, 2 = Region 2, 4 = Region 3),
 *  with the words the Setup page shows beside its switch. */
struct RegionDefault { const char* id; unsigned onIn; const char* why; };

inline const std::vector<RegionDefault>& regionDefaults() {
    static const std::vector<RegionDefault> t = {
        { "same", 2u,
          "Off by default here: SAME is North America's weather-alert system \xe2\x80\x94 nothing broadcasts it in this region" },
        { "dab", 1u | 4u,
          "Off by default in the Americas: there is no DAB broadcasting there" },
    };
    return t;
}

/** The region default that switches `id` off here, or nullptr. Region 0 / unknown = none. */
inline const RegionDefault* regionOff(const std::string& id, int region) {
    if (region < 1 || region > 3) return nullptr;
    for (const auto& d : regionDefaults())
        if (id == d.id && !(d.onIn & (1u << (region - 1)))) return &d;
    return nullptr;
}

inline std::vector<std::string> tokens(const std::string& csv) {
    std::vector<std::string> out;
    std::string tok;
    for (size_t i = 0; i <= csv.size(); ++i) {
        const char c = i < csv.size() ? csv[i] : ',';
        if (c == ',' || c == ' ' || c == ';' || c == '\t' || c == '\n') { if (!tok.empty()) out.push_back(tok); tok.clear(); }
        else tok += char(std::tolower((unsigned char)c));
    }
    return out;
}

/** ★★★ THE LIST THE SERVER ENFORCES: the owner's own CSV, plus each region default that is off here and that the owner
 *  has said nothing about (neither "same" nor "+same"). The "+id" tokens are the owner's word and are dropped from the
 *  result — they mean only "not off", and no reader of the effective list (hwinfo's `blocked`, every vsModeBlocked
 *  caller) has any use for them. */
inline std::string effectiveBlocked(const std::string& ownerCsv, int region) {
    const std::vector<std::string> t = tokens(ownerCsv);
    auto has = [&](const std::string& k) { for (const auto& x : t) if (x == k) return true; return false; };
    std::string out;
    for (const auto& x : t) {
        if (x[0] == '+') continue;
        out += (out.empty() ? "" : ",") + x;
    }
    for (const auto& d : regionDefaults()) {
        if (!regionOff(d.id, region)) continue;
        if (has(d.id) || has(std::string("+") + d.id)) continue;
        out += (out.empty() ? "" : ",") + std::string(d.id);
    }
    return out;
}

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

/** `,"region":N,"regionDefaults":[{"id":"same","why":"…"}]` — the defaults that are OFF here, for the Setup page's
 *  switches. Nothing at all when the region is unknown. */
inline std::string regionJson(int region) {
    if (region < 1 || region > 3) return {};
    std::string j = ",\"region\":" + std::to_string(region) + ",\"regionDefaults\":[";
    bool first = true;
    for (const auto& d : regionDefaults()) {
        if (!regionOff(d.id, region)) continue;
        j += std::string(first ? "" : ",") + "{\"id\":\"" + d.id + "\",\"why\":\"" + jsonEsc(d.why) + "\"}";
        first = false;
    }
    return j + "]";
}

} // namespace vibereach
