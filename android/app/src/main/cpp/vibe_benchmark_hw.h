// vibe_benchmark_hw.h — THE BENCHMARK TAILORED TO THE RADIOS ON THIS MACHINE (V12, Stuart, 2026-11-03).
//
// ★★★ WHY. "On the benchmark we need to tailor it for the hardware installed, no point testing DAB if the user only has
//     an AirSpy HF for instance, and no super wide sample rates for users with only an RTL." The run measures what the
//     radios HERE can use, and nothing they cannot: DAB only where a radio reaches Band III at 2.048 MS/s, ADS-B only
//     where one reaches 1090 MHz at 2.4 MS/s, the HF decoders only with HF coverage, each rate row only at a rate a radio
//     here offers — and the WIDE rows (8 MS/s and up) only where a radio offers, or could be offered, such a rate.
// ★★★ NEVER SILENTLY DROPPED. A row that does not run is listed under "notRun" with the reason in words ("no radio here
//     reaches Band III (174–240 MHz)"), and what it would have set stays at its existing default — the benchmark SETS
//     THE DEFAULT, NEVER HIDES (memory: benchmark sets the default, never hides).
// ★★ NO RADIO CONFIGURED (a fresh install before any radio) = an EMPTY set = today's full run, every row. The owner who
//    adds a radio later re-runs it.
// ★★ ONE RULE PER QUESTION, AND IT IS THE SERVER'S OWN. Coverage comes from vibebands::driverCoverage (the table hwinfo
//    and the directory publish), DAB's test is vibedab::radioCanDab, AIS leaves out the HF+ as vsAisReach does, NDB on an
//    RTL needs direct sampling as vsNdbReach does, and the offline rate lists are the Setup page's DRIVER_HW (a running
//    radio's own list replaces them — main.cpp passes it when it has one).
// ★ Pure and header-only: (radios) → (what runs, and why not) — vibeserver/test-bench-hw.cpp holds it to fake radio sets.
#pragma once
#include "vibe_bands.h"
#include "vibe_dab_channels.h"
#include "vibe_usrp_caps.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vibe {
namespace benchhw {

/** One radio as the benchmark sees it. `ranges` in true RF Hz (a converter already applied); `rates` the capture rates
 *  it offers now; `wideCandidates` rates ABOVE that list which this machine's benchmark may unlock (the rate gate);
 *  `locked` = a locked-range, multi-listener radio (the channeliser path); `ds` = an RTL with direct sampling on. */
struct Radio {
    std::string driver;
    std::vector<std::pair<double, double>> ranges;
    std::vector<double> rates;
    std::vector<double> wideCandidates;
    bool locked = false;
    bool ds = false;
};
/** The machine. Empty = no radio configured: the full run. */
struct Hw { std::vector<Radio> radios; };

// ── the offline tables ───────────────────────────────────────────────────────────────────────────────────────────

/** ★ The rates a driver offers before its radio has been opened — the Setup page's DRIVER_HW, restated (the page is
 *  JavaScript; this is C++). A running radio's own list (supportedRates) replaces it: main.cpp passes it when known. */
inline std::vector<double> offlineRates(const std::string& driver, int usbSpeed = 0) {
    if (driver == "rtl" || driver == "rtlsdr") return { 250000, 1024000, 1536000, 1792000, 1920000, 2048000, 2160000, 2400000 };
    if (driver == "airspyhf") return { 912000 };
    if (driver == "sdrplay")  return { 2000000, 2048000, 3000000, 4000000, 5000000, 6000000, 8000000 };
    if (driver == "hackrf")   return { 2000000, 2400000, 4000000, 5000000, 8000000, 10000000 };
    if (driver == "airspy")   return { 2500000, 3000000, 6000000, 10000000 };
    if (driver == "usrp") {
        std::vector<double> o;
        for (uint32_t r : usrpcaps::ratesFor(usbSpeed)) o.push_back(r);
        return o;
    }
    if (driver == "icom") return {};   // a transceiver's audio: no IQ capture to measure
    return {};
}

/** ★★ THE RATES ABOVE A RADIO'S LIST THAT A BENCHMARK MAY UNLOCK (the rate gate — vibe_usrp_caps.h ratesFor). Only the
 *  USRP on a USB 3 link (or a link not yet known: the measurement costs only time, the gate still needs USB 3 to offer
 *  them). Measured so the verdict exists BEFORE the owner asks for the rate — a gate can only open on a number. */
inline std::vector<double> wideCandidates(const std::string& driver, int usbSpeed = 0) {
    if (driver == "usrp" && usbSpeed != 2) {
        std::vector<double> o;
        for (uint32_t r : usrpcaps::gatedRates()) o.push_back(r);
        return o;
    }
    return {};
}

/** A radio from its config: the driver's coverage, shifted by a converter and clipped to what the converter passes
 *  (vsconfig RadioConfig converterOffsetHz / converterInputLoHz / HiHz — hardware = wanted − offset). */
inline Radio radioFor(const std::string& driver, double converterOffsetHz = 0, double convLo = 0, double convHi = 0,
                      bool directSampling = false, int usbSpeed = 0, bool locked = false,
                      const std::vector<double>& liveRates = {}) {
    Radio r;
    r.driver = driver == "rtlsdr" ? "rtl" : driver;
    for (const auto& x : vibebands::driverCoverage(r.driver)) {
        double lo = x.lo + converterOffsetHz, hi = x.hi + converterOffsetHz;
        if (converterOffsetHz != 0 && convHi > convLo) { lo = std::max(lo, convLo); hi = std::min(hi, convHi); }
        if (lo < 0) lo = 0;
        if (hi > lo) r.ranges.push_back({ lo, hi });
    }
    // ★ A USRP whose link is not known yet (it is read when the radio opens) is MEASURED at the USB 3 rates — the
    //   measurement costs only time; what is OFFERED still waits for the link (vibe_usrp_caps.h ratesFor).
    r.rates = liveRates.empty() ? offlineRates(r.driver, (r.driver == "usrp" && usbSpeed == 0) ? 3 : usbSpeed) : liveRates;
    r.wideCandidates = wideCandidates(r.driver, usbSpeed);
    r.locked = locked;
    r.ds = directSampling;
    return r;
}

// ── the questions ─────────────────────────────────────────────────────────────────────────────────────────────────

inline bool reaches(const Radio& r, double lo, double hi) {
    for (const auto& x : r.ranges) if (x.first <= lo && x.second >= hi) return true;
    return false;
}
inline bool touches(const Radio& r, double lo, double hi) {
    for (const auto& x : r.ranges) if (x.first < hi && x.second > lo) return true;
    return false;
}
inline double maxRate(const Radio& r) {
    double m = 0;
    for (double v : r.rates) m = std::max(m, v);
    return m;
}
inline bool offers(const Radio& r, double hz) {
    for (double v : r.rates) if (std::fabs(v - hz) < 1.0) return true;
    return false;
}
/** ★ HF for the HF decoders: below 24 MHz, where RTTY, WEFAX, NAVTEX, SITOR and the HF skimmers live (an R820T tuner
 *  starts at 24 MHz, so an R2 or a bare dongle hears only 10 m — that counts for digital spots, below). An RTL reaches it
 *  by its coverage table (a V4 has its own upconverter; a V3 direct sampling). */
inline bool hasHf(const Radio& r) { return touches(r, 100e3, 24e6); }

/** Every requirement a row can have. */
enum class Need {
    None,        // runs everywhere a capture exists
    Wfm,         // 88–108 MHz broadcast FM
    Dab,         // a Band III channel at >= 2.048 MS/s (vibedab::radioCanDab)
    Adsb,        // 1089–1091 MHz at >= 2.4 MS/s (vsAdsbWhyNot)
    Ais,         // 161.95–162.05 MHz, not an HF+ (vsAisReach)
    Hf,          // any coverage below 30 MHz
    Navtex,      // 518 kHz (and 490)
    Ndb,         // the NDB band (190–535 kHz); an RTL only with direct sampling (vsNdbReach)
    TimeSig,     // MSF 60 kHz / DCF77 77.5 kHz / WWV 2.5–20 MHz — HF in practice
    Same,        // NOAA weather radio 162.400–162.550 MHz
    Aprs,        // 144–146 MHz (144.39 / 144.80)
    Paging,      // POCSAG / FLEX: 138–470 MHz in practice
    Ism,         // an ISM band (433 / 868 / 915 MHz — vibeism's own; here: 433.05–434.79 or 863–870 or 902–928)
    Spots,       // digital spots: HF, 6 m or 2 m
};

struct Gate { bool run = true; std::string why; };

inline Gate gateNeed(const Hw& hw, Need n) {
    Gate g;
    if (hw.radios.empty() || n == Need::None) return g;   // ★ no radio configured: today's full run
    auto any = [&](const auto& f) { for (const auto& r : hw.radios) if (f(r)) return true; return false; };
    auto no = [&](const char* why) { g.run = false; g.why = std::string("not run — ") + why; return g; };
    switch (n) {
        case Need::None: break;
        case Need::Wfm:
            if (!any([](const Radio& r) { return touches(r, 88e6, 108e6); })) return no("no radio here reaches broadcast FM (88–108 MHz)");
            break;
        case Need::Dab:
            if (!any([](const Radio& r) {
                    std::vector<vibedab::Range> v;
                    for (const auto& x : r.ranges) v.push_back({ (uint32_t)std::min(4.0e9, x.first), (uint32_t)std::min(4.0e9, x.second) });
                    return vibedab::radioCanDab(v.data(), v.size(), (uint32_t)maxRate(r)); }))
                return no("no radio here reaches Band III (174–240 MHz) at 2.048 MS/s");
            break;
        case Need::Adsb:
            if (!any([](const Radio& r) { return reaches(r, 1089e6, 1091e6) && maxRate(r) >= 2.4e6 - 1000; }))
                return no("no radio here reaches 1090 MHz at 2.4 MS/s");
            break;
        case Need::Ais:
            if (!any([](const Radio& r) { return r.driver != "airspyhf" && r.driver != "icom" && reaches(r, 161.95e6, 162.05e6); }))
                return no("no radio here reaches AIS (162 MHz)");
            break;
        case Need::Hf:
            if (!any([](const Radio& r) { return hasHf(r); })) return no("no radio here reaches HF (below 24 MHz)");
            break;
        case Need::Navtex:
            if (!any([](const Radio& r) { return touches(r, 489e3, 519e3) && (r.driver != "rtl" || r.ds); }))
                return no("no radio here reaches NAVTEX (518 kHz)");
            break;
        case Need::Ndb:
            if (!any([](const Radio& r) { return touches(r, 190e3, 535e3) && (r.driver != "rtl" || r.ds); }))
                return no("no radio here reaches the NDB band (190–535 kHz)");
            break;
        case Need::TimeSig:
            if (!any([](const Radio& r) { return touches(r, 40e3, 25e6) && (r.driver != "rtl" || r.ds || touches(r, 2.4e6, 25e6)); }))
                return no("no radio here reaches the time signals (60 kHz – 20 MHz)");
            break;
        case Need::Same:
            if (!any([](const Radio& r) { return touches(r, 162.4e6, 162.55e6); })) return no("no radio here reaches NOAA weather radio (162.4 MHz)");
            break;
        case Need::Aprs:
            if (!any([](const Radio& r) { return touches(r, 144e6, 146e6); })) return no("no radio here reaches APRS (144 MHz)");
            break;
        case Need::Paging:
            if (!any([](const Radio& r) { return touches(r, 138e6, 470e6); })) return no("no radio here reaches the paging bands (138–470 MHz)");
            break;
        case Need::Ism:
            if (!any([](const Radio& r) { return touches(r, 433.05e6, 434.79e6) || touches(r, 863e6, 870e6) || touches(r, 902e6, 928e6); }))
                return no("no radio here reaches an ISM band (433 / 868 / 915 MHz)");
            break;
        case Need::Spots:
            if (!any([](const Radio& r) { return hasHf(r) || touches(r, 28e6, 29.7e6) || touches(r, 50e6, 54e6) || touches(r, 144e6, 148e6); }))
                return no("no radio here reaches a digital-spots band (HF, 6 m or 2 m)");
            break;
    }
    return g;
}

/** ★★ WHAT EACH ROW NEEDS, by its id — every row the benchmark has (vibe_benchmark.h, _decoders.h, _more.h, _dab.h,
 *  _adsb.h). An id not listed needs nothing: a new row runs everywhere until someone gives it a need. */
inline Need needFor(const std::string& id) {
    auto is = [&](const char* p) { return id.rfind(p, 0) == 0; };
    if (is("wfm_") || is("rdsx_") || is("zoom_") || id == "lk_wfm" || id == "auto_wfm") return Need::Wfm;
    if (id == "dec_dxscan") return Need::Wfm;   // ★★★ V12 the DX scanner's FM sweep: only where a radio reaches Broadcast FM
    if (id == "dab" || is("dab_")) return Need::Dab;
    if (is("adsb")) return Need::Adsb;
    if (id == "dec_ais") return Need::Ais;
    if (id == "dec_navtex") return Need::Navtex;
    if (id == "dec_ndb") return Need::Ndb;
    if (id == "dec_time") return Need::TimeSig;
    if (id == "dec_same") return Need::Same;
    if (id == "dec_aprs") return Need::Aprs;
    if (id == "dec_paging") return Need::Paging;
    if (id == "dec_ism" || id == "dec_wmbus" || id == "dec_lora" || id == "dec_lora_all") return Need::Ism;
    if (id == "dec_ft8" || id == "dec_ft8band" || is("spot_")) return Need::Spots;
    // ★ HF-only decoders: RTTY (and AUTO), WEFAX, the CW / PSK skimmers (HF bands), JTTY, SITOR-A, DSC (the MF/HF row),
    //   SELCAL (HF aviation), MFSK / Olivia / Contestia / THOR, Feld Hell. A listener's CW, PSK31 and SSTV run on any band.
    if (id == "dec_rtty" || id == "dec_rttyauto" || id == "dec_wefax" || id == "dec_cwskim" || id == "dec_pskskim"
        || id == "dec_jtty" || id == "dec_sitora" || id == "dec_dsc" || id == "dec_selcal" || id == "dec_mfsk"
        || id == "dec_hell")
        return Need::Hf;
    return Need::None;
}
inline Gate gateRow(const Hw& hw, const std::string& id) { return gateNeed(hw, needFor(id)); }

// ── the rate rows ─────────────────────────────────────────────────────────────────────────────────────────────────

/** ★ A rate row runs where a radio here offers that rate (or, for a wide row, could be offered it by the gate).
 *  No radio configured = the standard rows only (today's run). */
inline bool rateOffered(const Hw& hw, double hz, bool includeCandidates) {
    if (hw.radios.empty()) return false;
    for (const auto& r : hw.radios) {
        if (offers(r, hz)) return true;
        if (includeCandidates) for (double c : r.wideCandidates) if (std::fabs(c - hz) < 1.0) return true;
    }
    return false;
}
/** The nearest rate any radio here offers to `hz` — where a feature row (Advanced RDS, the keep-sharp zoom, NFM / AM /
 *  SSB) is measured when no radio offers its standard 2.048 MS/s (an RSP: 2.0; an HF+: 912 kHz). 0 = no radio. */
inline double nearestOffered(const Hw& hw, double hz, Need need = Need::None) {
    double best = 0, bd = 1e300;
    for (const auto& r : hw.radios) {
        if (need == Need::Wfm && !touches(r, 88e6, 108e6)) continue;
        for (double v : r.rates) { const double d = std::fabs(v - hz); if (d < bd) { bd = d; best = v; } }
    }
    return best;
}
/** ★★ THE WIDE RATES TO MEASURE: every rate of 8 MS/s and up a radio here offers, plus the gate's candidates, ascending.
 *  None on a machine whose radios top out below 8 MS/s (an RTL, an HF+) — "no super wide sample rates for users with
 *  only an RTL". None with no radio configured (today's run had none, and a Pi 2's run time stays where it was). */
inline std::vector<double> wideRates(const Hw& hw) {
    std::vector<double> o;
    auto add = [&](double v) { if (v >= 8e6 - 1 && std::find_if(o.begin(), o.end(), [&](double x) { return std::fabs(x - v) < 1; }) == o.end()) o.push_back(v); };
    for (const auto& r : hw.radios) { for (double v : r.rates) add(v); for (double v : r.wideCandidates) add(v); }
    std::sort(o.begin(), o.end());
    return o;
}
/** Any radio here set to locked range (the channeliser path — the gate judges such a radio by the locked-range rows). */
inline bool anyLocked(const Hw& hw) { for (const auto& r : hw.radios) if (r.locked) return true; return false; }

/** ★★ THE GATE'S INPUT, read back from a saved benchmark (benchmark.json beside the config): the "capture" block's
 *  lockedMaxHz / fullMaxHz. false (both 0) = no such block — never measured wide, so nothing above the base list. */
inline bool gateFromBenchJson(const std::string& j, double& lockedHz, double& fullHz) {
    lockedHz = fullHz = 0;
    const size_t c = j.find("\"capture\":{");
    if (c == std::string::npos) return false;
    auto num = [&](const char* key, double& out) {
        const size_t k = j.find(key, c);
        if (k == std::string::npos) return false;
        out = std::atof(j.c_str() + k + std::strlen(key));
        return true;
    };
    const bool a = num("\"lockedMaxHz\":", lockedHz), b = num("\"fullMaxHz\":", fullHz);
    if (!(lockedHz > 0)) lockedHz = 0;
    if (!(fullHz > 0)) fullHz = 0;
    return a && b;
}

/** "28 MS/s" / "30.72 MS/s" / "912 kS/s" — a rate as the owner reads it. */
inline std::string rateWords(double hz) {
    char b[32];
    if (hz < 1e6) { std::snprintf(b, sizeof b, "%.0f kS/s", hz / 1e3); return b; }
    std::snprintf(b, sizeof b, "%.3f", hz / 1e6);
    std::string s(b);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s + " MS/s";
}
/** "16000" — a rate as a row id's suffix, in kS/s (30.72 MS/s = 30720). */
inline std::string rateId(double hz) { return std::to_string((long long)std::llround(hz / 1e3)); }

}  // namespace benchhw
}  // namespace vibe
