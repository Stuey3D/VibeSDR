// vibe_radio_caps.h — WHAT THIS RADIO CAN DO: its modes, its decoders and the settings that mean anything on it (V12,
// Stuart, 2026-11-05).
//
// ★★★ WHY. "I've looked at the icom page and there are loads of options that dont apply to it, Fixed range multiple vfo's
//     keep spectrum sharp the demodulators and decoders etc … people will think that they are setting up a standard USB
//     port" — then: "that settings audit needs to apply to all radios, we shouldnt be offering modes etc that cannot be
//     run on that radio". Every radio's Setup tab showed the SAME forty switches: an Airspy HF+ was offered DAB, ADS-B and
//     LoRa, an Icom the usage chooser, Raw IQ out and VFO A/B. AGENTS.md: a control that cannot work on this radio is
//     removed, not left there dead.
//
// ★★★ ONE ANSWER, TWO ASKERS.
//     • The Setup page (vibe_setup_page.h) — every radio's tab draws only what `modes` and `settings` allow. The front
//       door answers it for radios that are PAUSED OR NOT RUNNING, so this works from the radio's CONFIG alone: driver,
//       converter, the RTL's HF path. main.cpp folds one per radio into GET /vibeserver/config as `radioCaps`.
//     • The receiver — the running radio's own answer (its live model, coverage and widest rate) rides on GET
//       /vibeserver/hardware as `caps` (the page prefers it), and the shim's `modes` offer in vibeserver.json passes
//       through it, so the directory, the listener's clients and the Setup page can never list a mode this says cannot run.
//
// ★★ CAN, NOT MAY. This is the HARDWARE's question. The owner's own switches (blockedModes), the allow / block lists and a
//    locked window are the owner's choices — they are answered where they always were (vsModeBlocked, vibe_decoder_reach.h
//    whyNot, vsDabCapable …) and never change what the Setup page OFFERS: an owner who narrows the allow list to FM must
//    still find the WEFAX switch when they widen it again. Hiding ≠ clearing: a saved value for a hidden switch stays in
//    the config untouched.
//
// ★★ THE RULES ARE THE SERVER'S OWN, restated nowhere else: coverage = vibereach::reachRanges (driverCoverage through the
//    converter, an RTL's HF only through direct sampling or a V4), the fixed-frequency decoders = vibereach's table, DAB =
//    vibedab::radioCanDab, ADS-B = 1090 MHz at 2.4 MS/s (vsAdsbHwWhyNot), AIS = 162 MHz (vsAisReach — the HF+ too, since 2026-11-24), NDB =
//    an RTL only in direct sampling (vsNdbReach), ISM = a whole vibeism band at 0.9 MS/s (vsIsmReach), the offline rates =
//    vibe_benchmark_hw.h offlineRates (the Setup page's DRIVER_HW). An Icom = one demodulated audio channel: its own modes
//    and the decoders that read audio (vibe_icom_decode.h — no skimmer, no Band, no DRM, no whole-radio mode).
//
// ★ Pure and header-only: (facts) → (caps) — vibeserver/test-radio-caps.cpp holds it to every driver.
#pragma once
#include "vibe_bands.h"
#include "vibe_benchmark_hw.h"
#include "vibe_dab_channels.h"
#include "vibe_decoder_reach.h"
#include "vibe_ism_bands.h"
#include <algorithm>
#include <string>
#include <vector>

namespace vibecaps {

/** The facts. From the config (factsFromConfig), or from the running radio (the shim fills `ranges` and `maxRateHz`
 *  from what the device reports). */
struct Facts {
    std::string driver;           ///< "rtl" | "sdrplay" | "airspyhf" | "airspy" | "hackrf" | "usrp" | "icom"
    vibebands::Ranges ranges;     ///< RF reach in true Hz, converter applied — NOT the owner's allow / block lists
    double maxRateHz = 0;         ///< the widest capture it offers (0 = unknown: rate rules are not applied)
    bool converter = false;       ///< an up/down-converter is set in front of it
    int rtlHf = 1;                ///< RTL only: 0 tuner only (24 MHz up), 1 its table (a V4, or unknown), 2 direct sampling
};

/** ★★ Every switch the Setup page's "Modes and decoders" draws, in its order (BLOCKABLE in vibe_setup_page.h).
 *  ★ Derived rows follow their parent and are not listed: each digital-spots mode (`spots-<mode>`, under "spots"), LoRa's
 *    "every channel" (`lora+all`), the DAB label scan and Band's per-mode list. */
inline const std::vector<const char*>& allModes() {
    static const std::vector<const char*> v = {
        "wfm", "rds", "nfm", "tone", "am", "usb", "sam", "sau", "sal", "ecssu", "ecssl", "amst", "lsb", "cwu", "cwl",
        "drm", "freedv", "rade", "m17", "dvsig", "dab", "rtty", "navtex", "wefax", "sstv", "ft8", "time", "spots", "spotsband", "ism", "lora", "cw", "psk",
        "dsc", "sitora", "same", "selcal", "mfsk", "hell", "ndb", "jtty", "ais", "aisstore", "aprs", "adsb", "commb", "loraaprs", "fanet", "meshcom",
        "hfdl", "avdata",   // ★ V12 aviation (docs/v12/AVIATION.md): HFDL on the dial, Aircraft datalink (ACARS / VDL2)
        "avdata+8m",        // ★ V12 2026-11-24 …its option: raise the capture to 8 MS/s for ACARS + VDL2 together (a "+" token)
        "sondes",           // ★ V12 RS41 radiosondes (docs/v12/RS41.md): a whole-radio mode on 400–406 MHz
        "balloon",          // ★ V12 balloons (docs/v12/BALLOONS.md): a listener's decoder on USB, 70 cm
        "tones",            // ★ V12 DTMF + 5-tone selcall (docs/v12/TONES.md): a listener's decoder on NFM / AM, any frequency
        "lrpt",             // ★ V12 METEOR-M LRPT pictures (docs/v12/LRPT.md): a listener's decoder on a 140 kHz IQ slice, 137 MHz
        "fctlm",            // ★ V12 beta90 FUNcube telemetry (AO-73): a listener's decoder on USB, 145.935 MHz (a SATELLITE pick attaches it)
        "orbcomm",          // ★ V12 ORBCOMM (docs/v12/ORBCOMM.md): a listener's decoder on an IQ slice, 137.2–137.8 MHz (a SATELLITE pick attaches it)
        "vor",              // ★ V12 VOR / ILS (docs/v12/VOR-ILS.md): a listener's decoder on the channel IQ, 108–118 / 329–335 MHz
        "vormap",           // ★ V12 the VOR / ILS MAP (docs/v12/VOR-MAP.md): a whole-radio mode sweeping 108–118 MHz, NDB's rules
        "sarsat",           // ★ V12 406 MHz beacons (docs/v12/SARSAT.md): a listener's decoder on the channel IQ, 406.0–406.1 MHz
        "ale",              // ★ V12 2G HF ALE (docs/v12/HF-DATA.md): a listener's decoder on USB, HF
    };
    return v;
}

/** The settings that exist on one kind of radio and not another. Absent from the JSON = an older server: shown. */
struct Settings {
    bool usage = true;        ///< "How will it be used?" — unlocked dial / locked centre with its own VFOs (and its Range card)
    bool vfoB = true;         ///< "Let listeners use VFO A/B" — a second channel in the capture
    bool rawIq = true;        ///< "Raw IQ out"
    bool nbWide = true;       ///< the IQ-domain impulse noise blanker
    bool landing = true;      ///< "Where new listeners start" (an Icom is where its owner left it)
    bool gain = true;         ///< the gain-limits card
    bool hardware = true;     ///< "This radio's hardware": span, converter, background decoding …
    bool fmDefaults = true;   ///< "Broadcast FM defaults" — only where WFM can run
    bool dabLanding = true;   ///< a DAB station as the landing
    bool adsbLanding = true;  ///< ADS-B as the landing
    bool dxScan = true;       ///< the DX scanner (it sweeps Broadcast FM or DAB)
    bool bgDecode = true;     ///< background decoding profiles
    bool converter = true;    ///< an up/down-converter in front of it
    bool directSampling = false;  ///< the RTL's automatic direct sampling
    bool ppm = false;         ///< crystal correction in ppm (RTL, RSP)
    bool ppb = false;         ///< calibration in ppb (HF+)
    bool biasT = false;       ///< DC on the aerial socket (an RSP's is per model — the page asks rspCaps)
    /** ★★ V12 automatic frequency correction (vibe_afc.h) — it LEARNS from DAB only (DRM merely confirms), so a radio that
     *  cannot reach Band III could never use it: the switch is not drawn there (AGENTS.md, one-radio controls). */
    bool autoPpm = false;
};

struct Caps {
    bool iq = true;                     ///< false = an Icom: one demodulated audio channel
    std::vector<std::string> modes;     ///< the allModes() ids that can run here, in that order
    Settings s;
    std::vector<std::string> hints;     ///< one line each: what the OWNER could change to get more (never a list of greys)
    bool has(const std::string& id) const { return std::find(modes.begin(), modes.end(), id) != modes.end(); }
};

inline std::string normDriver(const std::string& d) { return d == "rtlsdr" ? "rtl" : d; }

/** ★ The facts from a radio's config, the radio absent (the front door, a paused radio). `rtlHf`: direct sampling set
 *  (automatic, or forced to the Q branch) = 2; a name that says V4 (its own upconverter) = 1; otherwise 0 — a dongle's
 *  tuner starts at 24 MHz. The running radio knows its model better and says so through its own `caps`. */
inline Facts factsFromConfig(const std::string& driverIn, double convOffsetHz, double convLo, double convHi,
                             bool autoDs, int directSampling, const std::string& label) {
    Facts f;
    f.driver = normDriver(driverIn);
    f.converter = convOffsetHz != 0;
    if (f.driver == "rtl") {
        const bool v4 = label.find("V4") != std::string::npos || label.find("v4") != std::string::npos;
        f.rtlHf = (autoDs || directSampling == 2 || directSampling == 1) ? 2 : v4 ? 1 : 0;
    }
    f.ranges = vibereach::reachRanges(f.driver, convOffsetHz, convLo, convHi, f.rtlHf);
    double m = 0;
    for (double r : vibe::benchhw::offlineRates(f.driver)) m = std::max(m, r);
    f.maxRateHz = m;
    return f;
}

inline bool touches(const vibebands::Ranges& r, double lo, double hi) {
    for (const auto& x : r) if (x.lo < hi && x.hi > lo) return true;
    return false;
}
inline bool holds(const vibebands::Ranges& r, double lo, double hi) {
    for (const auto& x : r) if (x.lo <= lo && x.hi >= hi) return true;
    return false;
}

/** ★★ THE ANSWER. */
inline Caps compute(const Facts& f) {
    Caps c;
    const std::string& d = f.driver;
    /* ★★ V12 beta96 AN "ADS-B BEAST FEED (from another receiver)" RADIO: no hardware — its ADS-B comes from another
     *  receiver's Beast output. ADS-B is its ONLY mode; none of an SDR's settings mean anything for it (AGENTS.md: no dead
     *  controls) — no landing, gain, hardware, usage, raw IQ, VFO B, background decoding. */
    if (d == "beast") {
        c.iq = false;
        c.modes = {"adsb"};
        Settings& s = c.s;
        s.usage = s.vfoB = s.rawIq = s.nbWide = s.landing = s.gain = s.hardware = s.fmDefaults = s.dabLanding = false;
        s.adsbLanding = s.dxScan = s.bgDecode = s.converter = false;
        c.hints.push_back("This radio reads another receiver's ADS-B (Beast) output, so the aircraft map is all it offers.");
        return c;
    }
    const vibebands::Ranges& R = f.ranges;
    const bool icom = d == "icom";
    c.iq = !icom;
    const bool rateKnown = f.maxRateHz > 0;
    auto rateAtLeast = [&](double hz) { return !rateKnown || f.maxRateHz >= hz; };
    vibereach::Reach reach; reach.ranges = R;   // ★ unlocked: the hardware's question, never the owner's window
    auto tableOk = [&](const char* id) { return vibereach::whyNot(id, reach).empty(); };
    // ★ The decoders that read whatever the listener tunes but live, in practice, on HF (below 30 MHz).
    const bool hf = touches(R, 1.6e6, 30e6);
    const bool anyAmateur = [&] { for (const auto& b : vibereach::amateurBands()) if (touches(R, b.lo, b.hi)) return true; return false; }();

    auto can = [&](const std::string& id) -> bool {
        // ── an Icom: its own modes (LSB USB AM CW RTTY FM) and the decoders that read its audio ──
        if (icom) {
            static const char* kIcom[] = { "nfm", "am", "usb", "lsb", "cwu", "cwl", "rtty", "navtex", "wefax", "sstv",
                                           "ft8", "time", "spots", "cw", "psk", "dsc", "sitora", "same", "selcal",
                                           "mfsk", "hell", "jtty", "aprs",
                                           "hfdl",     // ★ HFDL reads the USB audio: an Icom's does
                                           "balloon",     // ★ …and so do balloons (an IC-9700 / IC-705 on 70 cm; reach decides)
                                           "tones",       // ★ …and DTMF / 5-tone on the radio's FM or AM audio
                                           "ale" };       // ★ …and ALE (an HF rig's USB audio)
            bool listed = false;
            for (const char* k : kIcom) if (id == k) { listed = true; break; }
            if (!listed) return false;   // ★ WFM, SAM/ECSS/C-QUAM, DRM, CTCSS (it reads channel IQ), every whole-radio mode
        }
        if (id == "nfm" || id == "am" || id == "usb" || id == "lsb" || id == "cwu" || id == "cwl"
            || id == "tone" || id == "sam" || id == "sau" || id == "sal" || id == "ecssu" || id == "ecssl") return true;
        if (id == "wfm" || id == "rds") return touches(R, 76e6, 108e6);
        if (id == "amst") return touches(R, 520e3, 1710e3);            // ★ C-QUAM is medium wave
        if (id == "drm") return touches(R, 148.5e3, 24e6);             // ★ DRM30: LW, MW and SW (not an R820T's 24 MHz up)
        if (id == "freedv") return hf;                                  // ★ V12 FreeDV digital voice: the HF amateur bands
        if (id == "rade") return hf;                                    // ★ V12 FreeDV's RADE V1, an option under it
        /* ★ V12 M17 digital voice: the 2 m and 70 cm amateur bands, where M17 lives — on every SDR that reaches one. The
         *  HF+ gets it on 2 m (it stops at 260 MHz, so 70 cm is never in its ranges; touches() decides). An Icom never: its
         *  FM audio is demodulated and de-emphasised in the radio, and the 4-FSK receiver reads IQ (not in kIcom above). */
        if (id == "m17") return touches(R, 144e6, 148e6) || touches(R, 420e6, 450e6);
        /* ★ V12 DV signalling (DMR, D-STAR, YSF, NXDN, dPMR identities — docs/v12/DV-SIGNALLING.md): VHF 136–174 MHz (2 m
         *  and the VHF PMR / business band) or UHF 400–470 MHz (70 cm, UHF PMR, PMR446), where those systems live — on every
         *  SDR that reaches one. Never an Icom: the receiver reads IQ (not in kIcom above). */
        if (id == "dvsig") return touches(R, 136e6, 174e6) || touches(R, 400e6, 470e6);
        if (id == "dab") {
            std::vector<vibedab::Range> v;
            for (const auto& x : R) v.push_back({ (uint32_t)std::min(4.0e9, x.lo), (uint32_t)std::min(4.0e9, x.hi) });
            return vibedab::radioCanDab(v.data(), v.size(), rateKnown ? (uint32_t)f.maxRateHz : vibedab::kMinCaptureHz);
        }
        if (id == "adsb" || id == "commb") return holds(R, 1089e6, 1091e6) && rateAtLeast(2.4e6 - 1000);
        /* ★★ AIS and the aircraft datalink on the HF+ too (2026-11-24, Stuart's XCover): left out "for its fixed-gain front
         *  end", but the modes step no gain there (vibe_mode_gain.h) and its 912 kS/s holds every band they use. */
        if (id == "ais" || id == "aisstore") return holds(R, 161.95e6, 162.05e6);
        // ★ V12 Aircraft datalink: ACARS (131.525–131.825) or VDL2 (136.700–136.975) whole
        if (id == "avdata") return holds(R, 131.50e6, 131.85e6) || holds(R, 136.68e6, 136.99e6);
        /* ★★ …and its 8 MS/s switch for ACARS + VDL2 together: only where BOTH sets are in reach and the radio lists a rate
         *  of 8 MS/s or more (an RSP, an Airspy R2, a HackRF, a USRP) — never an RTL or the HF+ (AGENTS.md: no dead switch). */
        if (id == "avdata+8m") return rateKnown && f.maxRateHz >= 8.0e6 - 1000 && holds(R, 131.50e6, 136.99e6);
        // ★ V12 Radiosondes: one 2 MHz band of 400–406 MHz whole — not the HF+ (it stops short of 400 MHz), not an Icom (no IQ)
        if (id == "sondes") return d != "airspyhf" && (holds(R, 400.0e6, 402.0e6) || holds(R, 402.0e6, 404.0e6) || holds(R, 404.0e6, 406.0e6));
        /* ★ V12 the VOR / ILS map: the whole navaid band (108–117.95 MHz and its windows' edges) at ≥ 700 kS/s (vibevormap::kMinRateHz —
         *  20 windows at most); the HF+ included (14 windows, a slower sweep); not an Icom (no IQ) */
        if (id == "vormap") return holds(R, 107.9e6, 118.05e6) && rateAtLeast(0.70e6);   // ★★ 2026-11-24 the HF+ in: slower, not dead (Stuart)
        if (id == "ism" || id == "lora") {
            if (!rateAtLeast(vibeism::kMinRateHz)) return false;
            for (const auto& b : vibeism::bands()) if (holds(R, b.centreHz - b.needHz, b.centreHz + b.needHz)) return true;
            return false;
        }
        /* ★ V12 the LoRa networks with their own switches: only where the radio reaches their channels (433.175–433.775 MHz;
         *  FANET 868.2, or zone 2's 920.8) at an ISM capture rate */
        if (id == "loraaprs" || id == "meshcom")   // ★ 433, or the UK's 439.9125 (V12 2026-11-16)
            return rateAtLeast(vibeism::kMinRateHz) && (holds(R, 433.05e6, 434.79e6) || holds(R, 439.85e6, 439.975e6));
        if (id == "fanet") return rateAtLeast(vibeism::kMinRateHz) && (holds(R, 868.05e6, 868.35e6) || holds(R, 920.5e6, 921.1e6));
        if (id == "ndb") {
            if (d == "rtl" && !f.converter && f.rtlHf != 2) return false;   // ★ vsNdbReach: the Q branch, or nothing
            double best = 0;
            for (const auto& x : R) best = std::max(best, std::min(x.hi, 535e3) - std::max(x.lo, 190e3));
            return best >= 100e3;                                           // vibendb::kMinSpanHz
        }
        if (id == "time") {
            static const double kHz[] = { 40e3, 60e3, 77.5e3, 2.5e6, 4.996e6, 5e6, 9.996e6, 10e6, 14.996e6, 15e6, 20e6 };
            for (double h : kHz) if (holds(R, h - 1e3, h + 1e3)) return true;
            return false;
        }
        if (id == "wefax") return touches(R, 2e6, 23e6);
        if (id == "sitora") return touches(R, 1.6e6, 23e6);           // ★ the maritime HF bands in use
        if (id == "rtty" || id == "mfsk" || id == "hell" || id == "ale") return hf;   // ★ ALE: 2G nets live 2–30 MHz
        if (id == "sstv" || id == "ft8" || id == "spots") return anyAmateur;
        if (id == "spotsband") return !icom && anyAmateur;                 // ★ Band reads IQ across a band
        if (id == "lrpt") return !icom && tableOk("lrpt");                // ★ V12 an IQ slice at 137.1 / 137.9 MHz (an Icom: no IQ)
        if (vibereach::entry(id)) return tableOk(id.c_str());             // navtex dsc same selcal aprs cw psk jtty balloon vor sarsat
        return true;
    };
    for (const char* id : allModes()) if (can(id)) c.modes.push_back(id);
    // ★ A child is never live under a parent that cannot run.
    auto drop = [&](const char* child, const char* parent) {
        if (!c.has(parent)) c.modes.erase(std::remove(c.modes.begin(), c.modes.end(), std::string(child)), c.modes.end());
    };
    drop("rds", "wfm"); drop("tone", "nfm"); drop("lora", "ism"); drop("aisstore", "ais"); drop("spotsband", "spots"); drop("commb", "adsb");
    drop("loraaprs", "lora"); drop("fanet", "lora"); drop("meshcom", "lora"); drop("avdata+8m", "avdata");

    // ── the settings ──
    Settings& s = c.s;
    s.usage = s.vfoB = s.rawIq = s.nbWide = s.landing = s.gain = s.hardware = s.bgDecode = s.converter = !icom;
    s.fmDefaults = c.has("wfm");
    s.dabLanding = c.has("dab");
    s.adsbLanding = c.has("adsb");
    s.dxScan = !icom && (c.has("wfm") || c.has("dab"));
    s.directSampling = d == "rtl";
    s.ppm = d == "rtl" || d == "sdrplay";
    s.ppb = d == "airspyhf";
    s.biasT = d == "rtl" || d == "sdrplay" || d == "hackrf" || d == "airspy";
    s.autoPpm = !icom && c.has("dab");

    // ── what the owner could change (one line each) ──
    if (icom)
        c.hints.push_back("An Icom gives VibeServer one demodulated audio channel, so only the radio's own modes and the "
                          "decoders that read audio are listed here.");
    else if (d == "rtl" && !f.converter && f.rtlHf == 0)
        c.hints.push_back("HF (below 24 MHz) needs direct sampling on this dongle: tick \xe2\x80\x9c" "Automatic Direct Sampling for HF\xe2\x80\x9d "
                          "under This radio\xe2\x80\x99s hardware (an RTL-SDR Blog V3 and its kin), or fit an upconverter, and "
                          "save \xe2\x80\x94 the HF modes and decoders then appear here.");
    else if ((d == "airspy" || d == "usrp") && !f.converter)
        c.hints.push_back(std::string("This radio starts at ") + (d == "usrp" ? "50" : "24") + " MHz: an upconverter (This "
                          "radio\xe2\x80\x99s hardware \xe2\x86\x92 Converter LO) brings HF, and its modes and decoders, here.");
    return c;
}

inline std::string toJson(const Caps& c) {
    std::string j = std::string("{\"iq\":") + (c.iq ? "true" : "false") + ",\"modes\":[";
    for (size_t i = 0; i < c.modes.size(); ++i) j += std::string(i ? "," : "") + "\"" + c.modes[i] + "\"";
    j += "],\"settings\":{";
    const Settings& s = c.s;
    const std::pair<const char*, bool> kv[] = {
        { "usage", s.usage }, { "vfoB", s.vfoB }, { "rawIq", s.rawIq }, { "nbWide", s.nbWide }, { "landing", s.landing },
        { "gain", s.gain }, { "hardware", s.hardware }, { "fmDefaults", s.fmDefaults }, { "dabLanding", s.dabLanding },
        { "adsbLanding", s.adsbLanding }, { "dxScan", s.dxScan }, { "bgDecode", s.bgDecode }, { "converter", s.converter },
        { "directSampling", s.directSampling }, { "ppm", s.ppm }, { "ppb", s.ppb }, { "biasT", s.biasT }, { "autoPpm", s.autoPpm },
    };
    bool first = true;
    for (const auto& p : kv) { j += std::string(first ? "" : ",") + "\"" + p.first + "\":" + (p.second ? "true" : "false"); first = false; }
    j += "},\"hints\":[";
    for (size_t i = 0; i < c.hints.size(); ++i) j += std::string(i ? "," : "") + "\"" + vibereach::jsonEsc(c.hints[i]) + "\"";
    return j + "]}";
}

}  // namespace vibecaps
