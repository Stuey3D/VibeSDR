// ★★★ WHICH DECODERS A RADIO CAN USE, AND WHICH ARE OFF HERE BY DEFAULT (V12, 2026-11-03) — vibe_decoder_reach.h held to
// fake radios and regions. No radio, no DSP: the decisions only.
//   Stuart, on a Dutch server whose USRP is locked to 88–96 MHz offering SAME: "what is SAME?" … "dont grey it out let a
//   user enable it if they want but disable it by default".
//   build: c++ -std=c++17 -I android/app/src/main/cpp -o /tmp/t vibeserver/test-decoder-reach.cpp && /tmp/t
#include "vibe_decoder_reach.h"
#include <cstdio>

using namespace vibereach;
static int fails = 0, passes = 0;
static void ok(bool c, const char* what) {
    if (c) ++passes; else { ++fails; std::printf("FAIL  %s\n", what); }
}
static bool live(const char* id, const Reach& r) { return whyNot(id, r).empty(); }
static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

int main() {
    // ── REGION DEFAULTS ──────────────────────────────────────────────────────────────────────────────────────────
    {
        ok(regionOff("same", 2) == nullptr, "Region 2: SAME is on by default");
        ok(regionOff("same", 1) != nullptr && regionOff("same", 3) != nullptr, "Regions 1 and 3: SAME is off by default");
        ok(regionOff("dab", 2) != nullptr, "Region 2: DAB is off by default");
        ok(regionOff("dab", 1) == nullptr && regionOff("dab", 3) == nullptr, "Regions 1 and 3: DAB is on by default");
        ok(regionOff("same", 0) == nullptr && regionOff("dab", 0) == nullptr, "region unknown: no region default at all");
        ok(regionOff("rtty", 1) == nullptr && regionOff("rtty", 2) == nullptr, "a decoder not in the table is never region-off");
        ok(has(regionOff("same", 1)->why, "North America"), "SAME's words name North America");
        ok(has(regionOff("dab", 2)->why, "Americas"), "DAB's words name the Americas");

        ok(effectiveBlocked("", 1) == "same", "Region 1, a fresh config: SAME off");
        ok(effectiveBlocked("", 2) == "dab", "Region 2, a fresh config: DAB off");
        ok(effectiveBlocked("", 0) == "", "region unknown, a fresh config: nothing off");
        ok(effectiveBlocked("", 3) == "same", "Region 3, a fresh config: SAME off");
        ok(effectiveBlocked("+same", 1) == "", "Region 1, the owner switched SAME ON: on, and the + token is not published");
        ok(effectiveBlocked("same", 2) == "same,dab", "Region 2, the owner switched SAME OFF: off (and DAB's default)");
        ok(effectiveBlocked("+dab,rtty", 2) == "rtty", "Region 2, the owner switched DAB ON: on; their own list kept");
        ok(effectiveBlocked("dab", 1) == "dab,same", "Region 1, the owner switched DAB off: off — never overwritten");
        ok(effectiveBlocked("lora+all,wefax", 1) == "lora+all,wefax,same", "a mid-token + (lora+all) is the owner's option, kept");
        ok(effectiveBlocked("SAME", 1) == "same", "case-insensitive, no doubled entry");

        ok(regionJson(0).empty(), "region unknown: no region JSON at all");
        const std::string j1 = regionJson(1), j2 = regionJson(2);
        ok(has(j1, "\"region\":1") && has(j1, "\"id\":\"same\"") && !has(j1, "\"id\":\"dab\""), "Region 1 JSON: SAME's default only");
        ok(has(j2, "\"region\":2") && has(j2, "\"id\":\"dab\"") && !has(j2, "\"id\":\"same\""), "Region 2 JSON: DAB's default only");
    }

    // ── REACH ────────────────────────────────────────────────────────────────────────────────────────────────────
    const char* fixed[] = { "same", "dsc", "aprs", "navtex", "selcal", "cw", "psk", "jtty" };
    // A full-coverage radio (an SDRplay RSP: 1 kHz – 2 GHz)
    {
        Reach r{ reachRanges("sdrplay"), false };
        for (const char* id : fixed) ok(live(id, r), "RSP (1 kHz – 2 GHz): every fixed-frequency decoder is live");
        ok(live("rtty", r) && live("wefax", r), "decoders not in the table are never greyed");
    }
    // Airspy HF+ (500 Hz – 31 MHz, 60 – 260 MHz): reaches 162 MHz and HF — all live
    {
        Reach r{ reachRanges("airspyhf"), false };
        for (const char* id : fixed) ok(live(id, r), "HF+: every fixed-frequency decoder is live (HF, and 60–260 MHz)");
    }
    // USRP (50 MHz – 6 GHz): no HF
    {
        Reach r{ reachRanges("usrp"), false };
        ok(live("same", r) && live("aprs", r) && live("dsc", r) && live("selcal", r), "USRP: SAME, APRS, DSC (ch 70), SELCAL (airband) live");
        ok(!live("navtex", r), "USRP: NAVTEX greyed (no HF)");
        ok(has(whyNot("navtex", r), "Out of this radio's reach (50.0 MHz\xe2\x80\x93" "6.00 GHz)") && has(whyNot("navtex", r), "518"),
           "USRP: NAVTEX's reason gives the radio's range and NAVTEX's frequencies");
        ok(live("cw", r), "USRP: CW live (6 m and up)");
    }
    // An RTL without direct sampling (no HF): tuner from 24 MHz
    {
        Reach r{ reachRanges("rtl", 0, 0, 0, 0), false };
        ok(!live("navtex", r), "RTL, no direct sampling: NAVTEX greyed");
        ok(live("dsc", r) && live("same", r) && live("aprs", r) && live("selcal", r), "RTL, no DS: DSC (ch 70), SAME, APRS, SELCAL (airband) live");
        ok(live("cw", r) && live("psk", r), "RTL, no DS: CW / PSK live (10 m and up)");
        Reach ds{ reachRanges("rtl", 0, 0, 0, 2), false };
        ok(live("navtex", ds), "RTL with direct sampling: NAVTEX live (0–14.4 MHz)");
        Reach v4{ reachRanges("rtl", 0, 0, 0, 1), false };
        ok(live("navtex", v4), "RTL Blog V4 (its own upconverter): NAVTEX live");
    }
    // A LOCKED RANGE 88–96 MHz (the Dutch USRP)
    {
        Reach r{ { { 88e6, 96e6 } }, true };
        for (const char* id : fixed) ok(!live(id, r), "locked 88–96 MHz: every fixed-frequency decoder is greyed");
        const std::string w = whyNot("same", r);
        ok(w == "Outside this radio's locked range (88.0\xe2\x80\x93" "96.0 MHz) \xe2\x80\x94 SAME is at 162.400\xe2\x80\x93" "162.550 MHz",
           "locked 88–96 MHz: SAME's reason, word for word");
        ok(live("rtty", r), "locked 88–96 MHz: RTTY (not in the table) is not greyed by reach");
    }
    // A locked window that holds ONE SAME channel only (162.475 ± 0.1 MHz): live
    {
        Reach r{ { { 162.375e6, 162.475e6 + 6.25e3 } }, true };
        ok(live("same", r), "a locked window holding one SAME channel: live");
        Reach part{ { { 162.400e6 + 1e3, 162.4e6 + 20e3 } }, true };
        ok(!live("same", part), "a window cutting a SAME channel in half: greyed (a channel must fit whole)");
    }
    // SELCAL's airband is a BAND: any part of it will do
    {
        Reach r{ { { 125e6, 127e6 } }, true };
        ok(live("selcal", r), "locked 125–127 MHz: SELCAL live (part of the airband)");
        ok(!live("dsc", r), "locked 125–127 MHz: DSC greyed");
    }
    // An upconverter: an RTL behind a Ham It Up (LO 125 MHz, signed −125 MHz, passing 0–65 MHz)
    {
        Reach r{ reachRanges("rtl", -125e6, 0, 65e6, 0), false };
        ok(live("navtex", r), "RTL + Ham It Up: NAVTEX live (HF through the converter)");
        ok(!live("same", r), "RTL + Ham It Up: SAME greyed");
        ok(live("aprs", r), "RTL + Ham It Up: APRS live (HF APRS on 10.1476 MHz)");
        ok(live("dsc", r), "RTL + Ham It Up: DSC live (MF/HF channels)");
    }
    // The owner's allow list: FM broadcast only
    {
        Reach r{ reachRanges("rtl", 0, 0, 0, 1, "87.5-108MHz"), false };
        ok(!live("same", r) && !live("navtex", r) && !live("cw", r), "allow list FM only: SAME, NAVTEX, CW greyed");
        ok(has(whyNot("same", r), "87.5\xe2\x80\x93" "108.0 MHz"), "allow list FM only: the reason gives the range");
    }
    // Unknown reach: never grey
    {
        Reach r;
        for (const char* id : fixed) ok(live(id, r), "reach unknown: nothing greyed");
    }
    // DSC is available if ANY of its channels is reachable (an HF-only Icom range)
    {
        Reach r{ { { 30e3, 30e6 } }, false };
        ok(live("dsc", r) && live("navtex", r) && !live("same", r), "HF only: DSC (MF/HF) and NAVTEX live, SAME greyed");
    }

    std::printf("%s  %d passed, %d failed\n", fails ? "FAIL" : "ok", passes, fails);
    return fails ? 1 : 0;
}
