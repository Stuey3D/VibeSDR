// vibe_wefax_stations.h — THE WORLD'S HF WEFAX (radiofacsimile) STATIONS and their frequencies (V12, 2026-11-22).
//
// ★★ THE ONE LIST. Background decoding's "WEFAX stations" VFO (vibe_bgdecode_config.h, decoder "wefax-stations") lets the
//    owner tick stations' frequencies and runs a WEFAX decoder on every ticked one inside the radio's capture, all in ONE
//    decoder slot (Stuart 2026-11-22: "i'd also like the other wefax frequencies covered"). The Setup page and the Android
//    server screen draw the ticks from THIS list as the server sends it (wefaxStationsJson — only what the radio can tune),
//    never from a copy of their own. The clients' spectrum hints (src/utils/decoderBands.ts WEFAX_KHZ, knownSignals.ts)
//    are held to it by scripts/test_wefax_stations.ts, which reads this header.
//
// ★ FREQUENCIES ARE THE ASSIGNED (CENTRE) FREQUENCY — the middle of the 1500–2300 Hz fax tones, what a dedicated fax
//   receiver is set to. The USB DIAL is 1.9 kHz below (Northwood 4610.0 → 4608.1 USB, Stuart's number), exactly as a
//   single-frequency WEFAX VFO is tuned (vibe_bgdecode_config.h: tone 1900).
//
// ★ SOURCE: U.S. National Weather Service, "Worldwide Marine Radiofacsimile Broadcast Schedules", March 7, 2025
//   (https://weather.gov/marine/media/rfax.pdf). Its tables give ASSIGNED frequencies ("for carrier frequency subtract
//   1.9 kHz") except where it says otherwise, and those are converted here:
//     · Athens SVJ4 lists 4481 / 8105 kHz marked "Center Frequency is 1.9 kHz higher" → 4482.9 / 8106.9.
//     · Bangkok HSW64 lists 7395.0 kHz marked "May refer to carrier frequency, for center frequency add 1.9 kHz" → 7396.9.
//     · Kodiak NOJ: the station table's 12412.5 kHz (the schedule page's footer says 12410.6; the table is used).
//     · Murmansk RBW: the table's four, which the document itself marks as uncertain (listener reports).
//   Northwood GYA and DWD Pinneberg (DDH3 / DDK3 / DDK6) are also Stuart's own off-air numbers (knownSignals.ts).
//   ✗ Not listed: Wellington ZKLF (stopped 1 July 2023, per the same document), the Canadian Arctic stations (discontinued
//   2019), Pevek (148 kHz, LF, ice charts at 90 LPM) and Kyodo News (press, not weather). A listener's own WEFAX key still
//   decodes whatever is tuned — this list is only what background decoding offers to tick.
// ★ Every station here sends 120 LPM / IOC 576 for its charts (the document's RPM/IOC column), the one mode the
//   background decoder runs.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace wefaxst {

struct Station {
    const char* id;        ///< stable key ("gya") — never shown
    const char* call;      ///< call sign(s) as the schedule gives them
    const char* name;      ///< where it is, as a person says it
    const char* country;
    std::vector<double> khz;   ///< assigned (centre) frequencies, kHz
};

/** The USB dial for a centre frequency (Hz): 1.9 kHz below. */
constexpr double kToneHz = 1900.0;
inline double dialOf(double centreHz) { return centreHz - kToneHz; }

inline const std::vector<Station>& stations() {
    static const std::vector<Station> s = {
        // Europe
        { "gya",   "GYA",            "Northwood",            "United Kingdom", { 2618.5, 4610.0, 8040.0, 11086.5 } },
        { "dwd",   "DDH3 / DDK3 / DDK6", "Pinneberg (DWD)",  "Germany", { 3855.0, 7880.0, 13882.5 } },
        { "svj4",  "SVJ4",           "Athens",               "Greece",         { 4482.9, 8106.9 } },
        { "rbw",   "RBW",            "Murmansk",             "Russia",         { 5336.0, 6445.5, 7908.8, 10130.0 } },
        // North America
        { "nmf",   "NMF",            "Boston",               "USA",            { 4235.0, 6340.5, 9110.0, 12750.0 } },
        { "nmg",   "NMG",            "New Orleans",          "USA",            { 4317.9, 8503.9, 12789.9, 17146.4 } },
        { "nmc",   "NMC",            "Point Reyes",          "USA",            { 4346.0, 8682.0, 12786.0, 17151.2, 22527.0 } },
        { "noj",   "NOJ",            "Kodiak",               "USA",            { 2054.0, 4298.0, 8459.0, 12412.5 } },
        { "vco",   "VCO",            "Sydney, Nova Scotia",  "Canada",         { 4416.0, 6915.1 } },
        // Pacific
        { "kvm70", "KVM70",          "Honolulu",             "USA",            { 9982.5, 11090.0, 16135.0 } },
        { "vmc",   "VMC",            "Charleville",          "Australia",      { 2628.0, 5100.0, 11030.0, 13920.0, 20469.0 } },
        { "vmw",   "VMW",            "Wiluna",               "Australia",      { 5755.0, 7535.0, 10555.0, 15615.0, 18060.0 } },
        // Asia
        { "jmh",   "JMH",            "Tokyo",                "Japan",          { 3622.5, 7795.0, 13988.5 } },
        { "jfx",   "JFX",            "Kagoshima",            "Japan",          { 4274.0, 8658.0, 13074.0, 16907.5, 22559.6 } },
        { "hll2",  "HLL2",           "Seoul",                "South Korea",    { 3585.0, 5857.5, 7433.5, 9165.0, 13570.0 } },
        { "hsw64", "HSW64",          "Bangkok",              "Thailand",       { 7396.9 } },
        { "xsq",   "XSQ",            "Guangzhou",            "China",          { 4199.75, 8412.5, 12629.25, 16826.25 } },
        // South America
        { "cbv",   "CBV",            "Valpara\xc3\xadso",    "Chile",          { 4228.0, 8677.0, 17146.4 } },
        { "cbm",   "CBM",            "Punta Arenas",         "Chile",          { 4322.0, 8696.0 } },
    };
    return s;
}

/** Same frequency (Hz) — the table's kHz carry at most two decimals. */
inline bool sameHz(double a, double b) { return std::fabs(a - b) < 5.0; }

/** Every station that lists `centreHz` — usually one; 17146.4 kHz is both New Orleans and Valparaíso. */
inline std::vector<const Station*> stationsOn(double centreHz) {
    std::vector<const Station*> v;
    for (const auto& s : stations()) for (double k : s.khz) if (sameHz(k * 1e3, centreHz)) { v.push_back(&s); break; }
    return v;
}
inline bool known(double centreHz) { return !stationsOn(centreHz).empty(); }

/** "4610.0" / "12629.25" — kHz as the schedule writes it. */
inline std::string khzText(double hz) {
    char b[32];
    const double k = hz / 1e3;
    if (std::fabs(k * 10 - std::round(k * 10)) < 1e-6) std::snprintf(b, sizeof b, "%.1f", k);
    else std::snprintf(b, sizeof b, "%.2f", k);
    return b;
}

/** ★ What a chart from `centreHz` is called in the gallery and the log: "Northwood GYA 4610.0 kHz" (two stations on one
 *  frequency: "New Orleans NMG / Valparaíso CBV 17146.4 kHz"). */
inline std::string label(double centreHz) {
    std::string s;
    // ★ a station whose call sign differs by frequency (DWD's DDH3 / DDK3 / DDK6) is named without it
    for (const Station* st : stationsOn(centreHz))
        s += (s.empty() ? "" : " / ") + std::string(st->name) + (std::strchr(st->call, '/') ? std::string() : " " + std::string(st->call));
    return (s.empty() ? std::string("WEFAX") : s) + " " + khzText(centreHz) + " kHz";
}

inline std::string jsonEsc(const std::string& in) {
    std::string o;
    for (char c : in) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

/** ★★ ONE WRITER: `,"wefaxStations":[{"id":"gya","call":"GYA","name":"Northwood","country":"United Kingdom",
 *  "freqs":[2618500,4610000,…]}…]` — only the frequencies `tunable(lo, hi)` says the radio can reach (its range, the
 *  owner's allow/block lists, a locked window: the reach gate's rule, vibe_decoder_reach.h — never offer what cannot
 *  work); a station with none left is not listed. Frequencies in Hz (centre); the dial is 1.9 kHz below. */
inline std::string stationsJson(const std::function<bool(double lo, double hi)>& tunable) {
    std::string j = ",\"wefaxStations\":[";
    bool first = true;
    for (const auto& s : stations()) {
        std::string f;
        for (double k : s.khz) {
            const double c = k * 1e3, d = dialOf(c);
            if (tunable && !tunable(d, d + 3000.0)) continue;   // ★ the whole USB passband must be reachable
            char b[32]; std::snprintf(b, sizeof b, "%.0f", c);
            f += (f.empty() ? "" : ",") + std::string(b);
        }
        if (f.empty()) continue;
        j += std::string(first ? "" : ",") + "{\"id\":\"" + s.id + "\",\"call\":\"" + jsonEsc(s.call) + "\",\"name\":\"" + jsonEsc(s.name)
           + "\",\"country\":\"" + jsonEsc(s.country) + "\",\"freqs\":[" + f + "]}";
        first = false;
    }
    return j + "]";
}

} // namespace wefaxst
} // namespace vibe
