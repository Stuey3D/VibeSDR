// vibe_bgdecode_config.h — BACKGROUND DECODING's settings: the profiles, what the save refuses, which profile runs when.
// Split from vibe_bgdecode.h so the daemon's save path (vibeserver/main.cpp) can validate without pulling in the DSP and
// the decoders. See vibe_bgdecode.h for the design.
#pragma once
#include "vibe_antswitch.h"      // ★ the strict little JSON reader — reused, not re-written
#include "vibe_gallery.h"        // jsonEsc

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vibe {
namespace bg {

// ── Config ──────────────────────────────────────────────────────────────────────────────────────────────────────
struct Vfo {
    double      hz = 0;            ///< the DIAL (carrier) — what a listener would type
    std::string mode = "usb";      ///< "usb" | "lsb"
    std::string dec = "navtex";    ///< see kDecoders
};
struct Profile {
    double centreHz = 500000;
    int    fromMin = 0, toMin = 0; ///< UTC minutes of the day; equal = all day
    std::vector<Vfo> vfos;
    bool allDay() const { return fromMin == toMin; }
};
struct Config {
    bool on = false;
    int  waitSec = 300;            ///< after the last listener leaves (the owner's, default 5 minutes)
    std::vector<Profile> profiles;
};

constexpr size_t kMaxProfiles = 4;
constexpr size_t kMaxVfos = 8;

/** ★ What a background VFO may decode, which demodulators it works on, and what it is told. `toneHz` is where the
 *  signal sits in the AUDIO (USB: RF = dial + tone; LSB: RF = dial − tone): NAVTEX on 518 kHz is dial 517.5 USB or
 *  518.5 LSB; a WEFAX carrier is the dial + 1.9 kHz; SSTV and FT8 frequencies are quoted as the dial itself (tone 0).
 *  `inverted` is the preset's polarity on USB — LSB mirrors the audio, so the decoder is told the OPPOSITE there
 *  (★★ NickB, 2026-10-22: LSB to dodge local interference; the mark/space swap is ours to make).
 *  ★ `modes` lists only what works: WEFAX and SSTV are NOT offered on LSB — mirrored audio is a negative chart and a
 *    scrambled picture, and neither decoder can be told otherwise — and SSTV on NFM is the ISS on 145.800.
 *  ★ `ext` "" = the FT8/FT4 spotter (DecoderHost::startSpots), kept as a run's SUMMARY, not as spots. */
struct DecoderDef { const char* id; const char* label; const char* ext; const char* kind; const char* modes;
                    double toneHz; double shift; double baud; bool inverted; bool autoRtty; };
inline const std::vector<DecoderDef>& decoders() {
    static const std::vector<DecoderDef> d = {
        { "navtex",       "NAVTEX",                      "navtex", "navtex", "usb,lsb", 500,  170, 100,   false, false },
        { "rtty",         "RTTY (auto)",                 "fsk",    "rtty",   "usb,lsb", 1000, 170, 45.45, false, true  },
        { "rtty-ham",     "RTTY 45.45 Bd 170 Hz",        "fsk",    "rtty",   "usb,lsb", 1000, 170, 45.45, false, false },
        { "rtty-weather", "RTTY 50 Bd 450 Hz (weather)", "fsk",    "rtty",   "usb,lsb", 1000, 450, 50,    true,  false },
        { "wefax",        "WEFAX (120 LPM)",             "wefax",  "wefax",  "usb",     1900, 0,   0,     false, false },
        { "sstv",         "SSTV",                        "sstv",   "sstv",   "usb,nfm", 0,    0,   0,     false, false },
        { "ft8",          "FT8 / FT4 (summary)",         "",       "ft8",    "usb",     0,    0,   0,     false, false },
    };
    return d;
}
inline const DecoderDef* decoderDef(const std::string& id) {
    for (const auto& d : decoders()) if (id == d.id) return &d;
    return nullptr;
}
/** The demodulators a background VFO offers — only those some decoder works on (AGENTS.md: no control whose every use
 *  is a no-op). */
inline bool demodOk(const std::string& m) { return m == "usb" || m == "lsb" || m == "nfm"; }
inline bool decoderFitsDemod(const std::string& dec, const std::string& mode) {
    const DecoderDef* d = decoderDef(dec);
    if (!d || !demodOk(mode)) return false;
    const std::string ms = std::string(",") + d->modes + ",";
    return ms.find("," + mode + ",") != std::string::npos;
}
/** The demodulator's passband for a background channel: SSB 2.7 kHz (FT8 3 kHz — its sub-band is 200-3000 Hz),
 *  NFM 12.5 kHz. */
inline double passbandHz(const Vfo& v) { return v.mode == "nfm" ? 12500.0 : v.dec == "ft8" ? 3000.0 : 2700.0; }

/** The signal frequency a VFO is listening for (NAVTEX at 517.5 USB → 518.0). */
inline double signalHz(const Vfo& v) {
    const DecoderDef* d = decoderDef(v.dec);
    const double tone = d && v.mode != "nfm" ? d->toneHz : 0;
    return v.mode == "lsb" ? v.hz - tone : v.hz + tone;
}

/** What DecoderHost::start is given for this VFO — the same shapes the web client's decParams() sends. */
inline std::string attachMsg(const Vfo& v) {
    const DecoderDef* d = decoderDef(v.dec);
    if (!d) return "{}";
    const std::string id = d->id;
    const bool inv = d->inverted != (v.mode == "lsb");      // ★ LSB mirrors the tones — tell the decoder
    char b[320];
    if (id == "navtex")
        std::snprintf(b, sizeof b, "{\"center_frequency\":%.0f,\"shift\":%.0f,\"baud_rate\":%.0f,\"inverted\":%s}",
                      d->toneHz, d->shift, d->baud, inv ? "true" : "false");
    // ★ WEFAX unattended: wait for the START TONE and end on the STOP TONE, so every chart is one item with its
    //   phasing and header bar — never a run-on strip (the UberSDR lesson, docs/v12/DECODERS-GALLERY.md).
    else if (id == "wefax")
        std::snprintf(b, sizeof b, "{\"lpm\":120,\"image_width\":1809,\"carrier\":%.0f,\"deviation\":400,"
                      "\"auto_start\":true,\"auto_stop\":true}", d->toneHz);
    else if (id == "sstv" || id == "ft8") std::snprintf(b, sizeof b, "{}");
    else
        std::snprintf(b, sizeof b, "{\"center_frequency\":%.0f,\"shift\":%.0f,\"baud_rate\":%.2f,\"encoding\":\"ITA2\","
                      "\"framing\":\"5N1.5\",\"inverted\":%s%s}",
                      d->toneHz, d->shift, d->baud, inv ? "true" : "false", d->autoRtty ? ",\"auto\":true" : "");
    return b;
}

inline int parseHhMm(const std::string& s) {
    int h = -1, m = -1;
    if (std::sscanf(s.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 24 || m < 0 || m > 59) return -1;
    const int v = h * 60 + m;
    return v == 1440 ? 0 : (v > 1440 ? -1 : v);
}
inline std::string hhmm(int min) { char b[8]; std::snprintf(b, sizeof b, "%02d:%02d", (min / 60) % 24, min % 60); return b; }

/** Parse the stored JSON. Missing or empty = off, which is every radio before this existed. */
inline bool parse(const std::string& json, Config& out, std::string& err) {
    out = Config{};
    if (json.find_first_not_of(" \t\r\n") == std::string::npos) return true;
    antswitch::Json j;
    if (!antswitch::Parser(json).parse(j) || j.kind != antswitch::Json::Obj) { err = "background decoding: not valid JSON"; return false; }
    if (const auto* v = j.get("on")) out.on = v->kind == antswitch::Json::Bool && v->b;
    if (const auto* v = j.get("waitMin")) if (v->kind == antswitch::Json::Num) out.waitSec = (int)std::lround(v->n * 60.0);
    if (out.waitSec < 0) out.waitSec = 0;
    if (out.waitSec > 24 * 3600) out.waitSec = 24 * 3600;
    const auto* ps = j.get("profiles");
    if (ps && ps->kind == antswitch::Json::Arr) {
        for (const auto& pj : ps->a) {
            if (pj.kind != antswitch::Json::Obj) continue;
            Profile p;
            if (const auto* c = pj.get("centre")) if (c->kind == antswitch::Json::Num) p.centreHz = c->n;
            const int f = parseHhMm(pj.str("from", "00:00")), t = parseHhMm(pj.str("to", "00:00"));
            if (f < 0 || t < 0) { err = "background decoding: a profile time is not HH:MM"; return false; }
            p.fromMin = f; p.toMin = t;
            if (const auto* vs = pj.get("vfos"); vs && vs->kind == antswitch::Json::Arr)
                for (const auto& vj : vs->a) {
                    if (vj.kind != antswitch::Json::Obj) continue;
                    Vfo v;
                    if (const auto* h = vj.get("hz")) if (h->kind == antswitch::Json::Num) v.hz = h->n;
                    v.mode = vj.str("mode", "usb");
                    v.dec  = vj.str("dec", "navtex");
                    p.vfos.push_back(v);
                }
            out.profiles.push_back(p);
        }
    }
    return true;
}
inline std::string toJson(const Config& c) {
    std::string o = std::string("{\"on\":") + (c.on ? "true" : "false") + ",\"waitMin\":";
    char b[64]; std::snprintf(b, sizeof b, "%g", c.waitSec / 60.0); o += b;
    o += ",\"profiles\":[";
    for (size_t i = 0; i < c.profiles.size(); i++) {
        const auto& p = c.profiles[i];
        if (i) o += ',';
        std::snprintf(b, sizeof b, "%.0f", p.centreHz);
        o += std::string("{\"centre\":") + b + ",\"from\":\"" + hhmm(p.fromMin) + "\",\"to\":\"" + hhmm(p.toMin) + "\",\"vfos\":[";
        for (size_t k = 0; k < p.vfos.size(); k++) {
            const auto& v = p.vfos[k];
            if (k) o += ',';
            std::snprintf(b, sizeof b, "%.0f", v.hz);
            o += std::string("{\"hz\":") + b + ",\"mode\":\"" + gallery::jsonEsc(v.mode) + "\",\"dec\":\"" + gallery::jsonEsc(v.dec) + "\"}";
        }
        o += "]}";
    }
    return o + "]}";
}

/** Does profile p run at minute-of-day m (UTC)? [from, to) — wrapping midnight when from > to. */
inline bool covers(const Profile& p, int m) {
    if (p.allDay()) return true;
    if (p.fromMin < p.toMin) return m >= p.fromMin && m < p.toMin;
    return m >= p.fromMin || m < p.toMin;
}
/** The profile in force at minute m, or -1. Profiles never overlap (validate), so there is at most one. */
inline int activeProfile(const Config& c, int m) {
    for (size_t i = 0; i < c.profiles.size(); i++) if (covers(c.profiles[i], m)) return (int)i;
    return -1;
}
/** ★ How far either side of the RF centre a VFO may sit at `rate` — the SAME guard retune() keeps from the edge of the
 *  capture (rate/2 − min(50 kHz, 15 %)), less a passband's worth (3 kHz) so the whole signal is inside. */
inline double halfWindowHz(double rate) { return rate / 2.0 - std::min(50000.0, rate * 0.15) - 3000.0; }

static inline std::string khz(double hz) { char b[32]; std::snprintf(b, sizeof b, "%.1f kHz", hz / 1e3); return b; }

/** ★★ WHAT THE SAVE REFUSES, IN WORDS. `rate` is the radio's sample rate (0 = unknown: the window is not checked). */
inline bool validate(const Config& c, double rate, std::string& err) {
    if (c.profiles.size() > kMaxProfiles) { err = "background decoding: at most " + std::to_string(kMaxProfiles) + " profiles"; return false; }
    if (c.on && c.profiles.empty()) { err = "background decoding is on but has no profile — add one, or switch it off"; return false; }
    for (size_t i = 0; i < c.profiles.size(); i++) {
        const auto& p = c.profiles[i];
        const std::string name = "Profile " + std::to_string(i + 1);
        if (!(p.centreHz > 0)) { err = name + ": set an RF centre"; return false; }
        if (p.vfos.empty()) { err = name + ": add at least one VFO"; return false; }
        if (p.vfos.size() > kMaxVfos) { err = name + ": at most " + std::to_string(kMaxVfos) + " VFOs"; return false; }
        for (size_t k = 0; k < p.vfos.size(); k++) {
            const auto& v = p.vfos[k];
            const std::string vn = name + ", VFO " + std::to_string(k + 1);
            if (!(v.hz > 0)) { err = vn + ": set a frequency"; return false; }
            if (!demodOk(v.mode)) { err = vn + ": background decoding runs on USB, LSB or NFM"; return false; }
            if (!decoderDef(v.dec)) { err = vn + ": choose a decoder"; return false; }
            if (!decoderFitsDemod(v.dec, v.mode)) {
                std::string need = decoderDef(v.dec)->modes, m = v.mode;
                for (auto& c : need) c = c == ',' ? '/' : (char)std::toupper((unsigned char)c);
                for (auto& c : m) c = (char)std::toupper((unsigned char)c);
                err = vn + ": " + std::string(decoderDef(v.dec)->label) + " does not work on " + m + " \xe2\x80\x94 it needs " + need;
                return false;
            }
            if (rate > 0) {
                const double half = halfWindowHz(rate);
                const double off = std::fabs(v.hz - p.centreHz);
                if (off > half) {
                    char b[300];
                    std::snprintf(b, sizeof b, "%s (%s) is %s from the RF centre %s — at %.0f kS/s this radio sees about "
                                  "\xc2\xb1%s around its centre. Move the centre, or the VFO.",
                                  vn.c_str(), khz(v.hz).c_str(), khz(off).c_str(), khz(p.centreHz).c_str(), rate / 1e3, khz(half).c_str());
                    err = b; return false;
                }
            }
        }
    }
    // ★★ OVERLAPS, NAMED. Minute by minute — 1,440 bits a profile, and no edge case of wrapping midnight can slip by.
    for (size_t i = 0; i < c.profiles.size(); i++)
        for (size_t k = i + 1; k < c.profiles.size(); k++)
            for (int m = 0; m < 1440; m++)
                if (covers(c.profiles[i], m) && covers(c.profiles[k], m)) {
                    const auto& a = c.profiles[i]; const auto& b = c.profiles[k];
                    auto when = [](const Profile& p) { return p.allDay() ? std::string("all day") : hhmm(p.fromMin) + "\xe2\x80\x93" + hhmm(p.toMin); };
                    err = "Profile " + std::to_string(i + 1) + " (" + when(a) + ") and Profile " + std::to_string(k + 1) + " ("
                        + when(b) + ") overlap at " + hhmm(m) + " UTC \xe2\x80\x94 only one profile can run at a time.";
                    return false;
                }
    return true;
}

} // namespace bg
} // namespace vibe
