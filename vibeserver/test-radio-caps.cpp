// ★★★ WHAT EACH RADIO CAN DO (V12, 2026-11-05) — vibe_radio_caps.h held to every driver, from its CONFIG (the front door's
// answer for a radio that is paused or not running). Stuart: "that settings audit needs to apply to all radios, we shouldnt
// be offering modes etc that cannot be run on that radio".
//   build: c++ -std=c++17 -I android/app/src/main/cpp -I android/app/src/main/cpp/decoders -o /tmp/t vibeserver/test-radio-caps.cpp && /tmp/t
#include "vibe_radio_caps.h"
#include <cstdio>

using namespace vibecaps;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) {
    if (c) ++passes; else { ++fails; std::printf("FAIL  %s\n", what.c_str()); }
}
static Caps of(const std::string& drv, bool autoDs = false, const std::string& label = "", double conv = 0,
               double cLo = 0, double cHi = 0) {
    return compute(factsFromConfig(drv, conv, cLo, cHi, autoDs, -1, label));
}
static void yes(const Caps& c, const char* who, std::initializer_list<const char*> ids) {
    for (const char* id : ids) ok(c.has(id), std::string(who) + " offers " + id);
}
static void no(const Caps& c, const char* who, std::initializer_list<const char*> ids) {
    for (const char* id : ids) ok(!c.has(id), std::string(who) + " does NOT offer " + id);
}

int main(int argc, char** argv) {
    /* ★ --json: every fixture's caps as the door sends them — scripts/test-setup-radio-caps.mjs feeds them to the Setup
     *  page's own functions, so the page is tested against THIS function's answers, never a copy of them. */
    if (argc > 1 && std::string(argv[1]) == "--json") {
        const std::pair<const char*, Caps> fx[] = {
            { "icom", of("icom") }, { "rtl", of("rtl", false, "Generic RTL2832U OEM") }, { "rtlds", of("rtl", true) },
            { "rtlv4", of("rtl", false, "RTL-SDR Blog V4") }, { "airspyhf", of("airspyhf") }, { "sdrplay", of("sdrplay") },
            { "airspy", of("airspy") }, { "hackrf", of("hackrf") }, { "usrp", of("usrp") },
        };
        std::printf("{");
        for (size_t i = 0; i < sizeof fx / sizeof fx[0]; ++i)
            std::printf("%s\"%s\":%s", i ? "," : "", fx[i].first, toJson(fx[i].second).c_str());
        std::printf("}\n");
        return 0;
    }
    // ── ICOM (IC-7300: 30 kHz – 74.8 MHz, one demodulated audio channel) ──
    {
        const Caps c = of("icom");
        ok(!c.iq, "an Icom is not IQ");
        yes(c, "Icom", { "nfm", "am", "usb", "lsb", "cwu", "cwl", "rtty", "navtex", "wefax", "sstv", "ft8", "time", "spots",
                         "cw", "psk", "dsc", "sitora", "selcal", "mfsk", "hell", "jtty", "aprs" });
        no(c, "Icom", { "wfm", "rds", "tone", "sam", "sau", "sal", "ecssu", "ecssl", "amst", "drm", "dab", "spotsband", "ism",
                        "lora", "ndb", "ais", "aisstore", "adsb", "same" });
        ok(!c.s.usage && !c.s.vfoB && !c.s.rawIq && !c.s.nbWide && !c.s.landing && !c.s.gain && !c.s.hardware,
           "Icom: no usage chooser, VFO A/B, Raw IQ, impulse NB, landing, gain or hardware card");
        ok(!c.s.fmDefaults && !c.s.dabLanding && !c.s.adsbLanding && !c.s.dxScan && !c.s.bgDecode && !c.s.converter,
           "Icom: no FM defaults, DAB / ADS-B landing, DX scanner, background decoding or converter");
        ok(c.hints.size() == 1 && c.hints[0].find("one demodulated audio channel") != std::string::npos, "Icom: one hint, in words");
    }
    // ── RTL without direct sampling (24 MHz – 1.766 GHz, 2.4 MS/s) ──
    {
        const Caps c = of("rtl", false, "Generic RTL2832U OEM");
        yes(c, "RTL (no DS)", { "wfm", "rds", "nfm", "tone", "am", "usb", "lsb", "sam", "dab", "adsb", "ism", "lora", "ais",
                                "aisstore", "aprs", "same", "ft8", "spots", "spotsband", "cw", "psk", "sstv", "rtty" });
        no(c, "RTL (no DS)", { "drm", "amst", "navtex", "wefax", "sitora", "time", "ndb" });
        ok(c.s.usage && c.s.vfoB && c.s.rawIq && c.s.nbWide && c.s.fmDefaults && c.s.dabLanding && c.s.adsbLanding && c.s.dxScan,
           "RTL: the IQ settings, FM defaults, both landings and the DX scanner");
        ok(c.s.directSampling && c.s.ppm && c.s.biasT && !c.s.ppb, "RTL: direct sampling, ppm and bias-T; no ppb");
        ok(c.hints.size() == 1 && c.hints[0].find("direct sampling") != std::string::npos, "RTL (no DS): the direct-sampling hint");
    }
    // ── RTL with automatic direct sampling, and a V4 ──
    {
        const Caps ds = of("rtl", true);
        yes(ds, "RTL (DS)", { "drm", "amst", "navtex", "wefax", "sitora", "time", "ndb", "wfm", "dab", "adsb" });
        ok(ds.hints.empty(), "RTL (DS): no hint");
        const Caps v4 = of("rtl", false, "RTL-SDR Blog V4");
        yes(v4, "RTL V4", { "drm", "amst", "navtex", "wefax", "time", "wfm", "dab", "adsb" });
        no(v4, "RTL V4", { "ndb" });   // ★ vsNdbReach: an RTL reaches LF only on the Q branch
        ok(v4.hints.empty(), "RTL V4: no hint");
        const Caps up = of("rtl", false, "", -125e6, 0, 65e6);   // a Ham It Up
        yes(up, "RTL + upconverter", { "wefax", "time", "navtex", "drm" });
        no(up, "RTL + upconverter", { "wfm", "dab", "adsb", "ism" });
        ok(up.hints.empty(), "RTL + upconverter: no hint");
    }
    // ── AIRSPY HF+ (0.5 kHz – 31 MHz and 60 – 260 MHz, 912 kS/s) ──
    {
        const Caps c = of("airspyhf");
        yes(c, "HF+", { "wfm", "rds", "nfm", "am", "sam", "amst", "drm", "navtex", "wefax", "time", "ndb", "aprs", "same", "ft8" });
        no(c, "HF+", { "dab", "adsb", "ism", "lora", "ais", "aisstore" });
        ok(!c.s.dabLanding && !c.s.adsbLanding && c.s.fmDefaults && c.s.dxScan, "HF+: no DAB / ADS-B landing; FM defaults and DX scan");
        ok(c.s.ppb && !c.s.ppm && !c.s.biasT && !c.s.directSampling, "HF+: ppb, no ppm, bias-T or direct sampling");
        ok(c.hints.empty(), "HF+: no hint (nothing the owner could change)");
    }
    // ── SDRPLAY RSP (1 kHz – 2 GHz, 8 MS/s): everything ──
    {
        const Caps c = of("sdrplay");
        for (const char* id : allModes()) ok(c.has(id), std::string("RSP offers ") + id);
        ok(c.s.ppm && c.s.biasT && !c.s.ppb, "RSP: ppm and bias-T");
    }
    // ── AIRSPY R2 / MINI (24 MHz – 1.8 GHz, 10 MS/s) ──
    {
        const Caps c = of("airspy");
        yes(c, "R2", { "wfm", "dab", "adsb", "ism", "ais", "ft8", "spotsband", "rtty" });
        no(c, "R2", { "drm", "amst", "navtex", "wefax", "time", "ndb", "sitora" });
        ok(c.hints.size() == 1 && c.hints[0].find("24 MHz") != std::string::npos, "R2: the upconverter hint");
    }
    // ── HACKRF (1 MHz – 6 GHz) ──
    {
        const Caps c = of("hackrf");
        yes(c, "HackRF", { "wfm", "dab", "adsb", "ism", "ais", "drm", "wefax", "time", "navtex" });
        no(c, "HackRF", { "ndb" });   // ★ nothing below 1 MHz: no LF (the top of medium wave, 1–1.7 MHz, it reaches)
        yes(c, "HackRF", { "amst" });
        ok(c.hints.empty(), "HackRF: no hint");
    }
    // ── USRP B2xx (50 MHz – 6 GHz) ──
    {
        const Caps c = of("usrp");
        yes(c, "USRP", { "wfm", "dab", "adsb", "ism", "lora", "ais", "aprs", "same", "ft8", "spotsband" });
        no(c, "USRP", { "drm", "amst", "navtex", "wefax", "time", "ndb", "rtty", "sitora", "mfsk", "hell" });
        ok(c.has("dsc"), "USRP: DSC (VHF channel 70, 156.525 MHz)");
        ok(!c.s.biasT && !c.s.ppm, "USRP: no bias-T, no ppm");
        ok(c.hints.size() == 1 && c.hints[0].find("50 MHz") != std::string::npos, "USRP: the upconverter hint");
    }
    // ── the JSON ──
    {
        const std::string j = toJson(of("icom"));
        ok(j.find("\"iq\":false") != std::string::npos && j.find("\"usage\":false") != std::string::npos
           && j.find("\"modes\":[\"nfm\"") != std::string::npos, "the JSON carries iq, settings and modes");
    }
    std::printf("radio caps: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
