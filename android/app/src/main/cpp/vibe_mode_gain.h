// vibe_mode_gain.h — the gain of the whole-radio BURST modes (AIS, the aircraft datalink), per radio (V12 2026-11-24).
//
// ★★★ WHY (AGENTS.md: "a control that only works on one radio should not be there"). AIS and the datalink put the radio's
//     gain 85 % up its own list and step it down when bursts clip (vibeism::GainRule), and their reports carry the figure
//     ("gain 43.4 dB auto"). That is an RTL's / an RSP's / an R2's gain model. The Airspy HF+ has NO variable gain: a
//     preamp switch, an 8-step attenuator and its own AGC, all the OWNER's settings (LocalHardwarePanel draws them). So on
//     the HF+ the modes step nothing (no gain list — getTunerGains is empty there — and no setGain on the way out), and the
//     report says what IS true — "HF+ AGC · attenuator 12 dB" — instead of a dB figure the radio never had.
// ★ Pure, header-only: vibeserver/test-avi-service.cpp holds every radio to it.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace vibemodegain {

/** Does a burst mode's gain rule step this radio's gain? Not on the HF+ (no variable gain), nor where there is no list. */
inline bool steps(const std::string& driver, const std::vector<int>& gains) {
    return driver != "airspyhf" && !gains.empty();
}
/** Where the rule starts: 85 % up the radio's own list (AIS's figure, 2026-10-29). -1 = it does not step here. */
inline int startIdx(const std::string& driver, const std::vector<int>& gains) {
    if (!steps(driver, gains)) return -1;
    return std::min(int(gains.size()) - 1, int(gains.size() * 85 / 100));
}
/** What the HF+'s front end is doing, as the owner set it: "HF+ AGC · attenuator 12 dB · preamp". `attSteps` 6 dB each. */
inline std::string hfNote(bool agc, bool agcHigh, int attSteps, bool lna) {
    std::string s = agc ? (agcHigh ? "HF+ AGC (high threshold)" : "HF+ AGC") : "HF+ AGC off";
    if (attSteps > 0) s += " \xc2\xb7 attenuator " + std::to_string(attSteps * 6) + " dB";
    if (lna) s += " \xc2\xb7 preamp";
    return s;
}
/** The report's gain fields, added before its closing brace: `"gain":43.4,"gainAuto":1` where the mode sets the gain, or
 *  `"gainNote":"HF+ AGC"` on the HF+ (never a dB figure there). Nothing when neither is known. */
inline void appendGain(std::string& r, bool hfplus, int shownTenthDb, bool autoG, const std::string& note) {
    if (r.empty() || r.back() != '}') return;
    if (hfplus) {
        if (note.empty()) return;
        r.pop_back();
        r += ",\"gainNote\":\"";
        for (char c : note) { if (c == '"' || c == '\\') r += '\\'; if ((unsigned char)c >= 0x20) r += c; }
        r += "\"}";
        return;
    }
    if (shownTenthDb < 0) return;
    char g[80];
    std::snprintf(g, sizeof g, ",\"gain\":%.1f,\"gainAuto\":%d}", shownTenthDb / 10.0, autoG ? 1 : 0);
    r.pop_back(); r += g;
}

}  // namespace vibemodegain
