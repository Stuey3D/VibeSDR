// vibe_bgdecode_config.h — BACKGROUND DECODING's settings: the profiles, what the save refuses, which profile runs when.
// Split from vibe_bgdecode.h so the daemon's save path (vibeserver/main.cpp) can validate without pulling in the DSP and
// the decoders. See vibe_bgdecode.h for the design.
#pragma once
#include "vibe_antswitch.h"      // ★ the strict little JSON reader — reused, not re-written
#include "vibe_gallery.h"        // jsonEsc
#include "vibe_ism_bands.h"      // ★ an ISM profile names one of these bands

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
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
    /** ★★ V12 2026-10-25 — AN ISM PROFILE (Stuart: ISM "selectable as a decoder in a background-decoding profile"). ISM
     *  is a WHOLE-CAPTURE decoder: every sensor in a sub-band at once, with the radio centred for the band
     *  (local_sdr_shim.cpp ismHwCentre) — so such a profile picks a BAND ("315" / "433" / "868" / "915",
     *  vibe_ism_bands.h) instead of an RF centre and VFOs, and runs the listener's own ISM mode with nobody here.
     *  Empty = an ordinary profile. Stored as "ism":"433"; its "centre" and "vfos" are ignored. */
    std::string ism;
    /* ★ V12 2026-10-27 — there is no whole-radio NDB profile any more (Stuart chose the VFO list: the NDB skimmer is a VFO
     *  decoder, "ndb-skim", running beside NAVTEX and FT8 on the same capture). An older saved "ndb":true profile is
     *  CONVERTED on parse into a VFO profile with that one entry, centred on the band (kNdbLegacyCentreHz) — see parse(). */
    bool allDay() const { return fromMin == toMin; }
    bool isIsm() const { return !ism.empty(); }
    bool isWhole() const { return isIsm(); }   ///< a whole-radio mode (ISM), not VFOs
};
struct Config {
    bool on = false;
    int  waitSec = 300;            ///< after the last listener leaves (the owner's, default 5 minutes)
    /** ★★ "Keep decoding stations still in range while listeners are on" (Stuart, 2026-10-22, default ON): when a listener
     *  arrives the radio goes back to its own dial as ever, and every VFO still inside the window it now captures keeps
     *  decoding — the radio is never moved for them. Off: background decoding stops entirely while anyone is here. */
    bool keepInRange = true;
    std::vector<Profile> profiles;
};

constexpr size_t kMaxProfiles = 4;
constexpr size_t kMaxVfos = 16;   // ★ a sensible ceiling; a SINGLE-radio server is capped lower, at its decoder slots

/** ★ What a background VFO may decode, which demodulators it works on, and what it is told. `toneHz` is where the
 *  signal sits in the AUDIO (USB: RF = dial + tone; LSB: RF = dial − tone): NAVTEX on 518 kHz is dial 517.5 USB or
 *  518.5 LSB; a WEFAX carrier is the dial + 1.9 kHz; SSTV and FT8 frequencies are quoted as the dial itself (tone 0).
 *  `inverted` is the preset's polarity on USB — LSB mirrors the audio, so the decoder is told the OPPOSITE there
 *  (★★ NickB, 2026-10-22: LSB to dodge local interference; the mark/space swap is ours to make).
 *  ★ `modes` lists only what works: WEFAX and SSTV are NOT offered on LSB — mirrored audio is a negative chart and a
 *    scrambled picture, and neither decoder can be told otherwise — and SSTV on NFM is the ISS on 145.800.
 *  ★ `ext` "" = the FT8/FT4 spotter (DecoderHost::startSpots), kept as a run's SUMMARY, not as spots. */
struct DecoderDef { const char* id; const char* label; const char* ext; const char* kind; const char* modes;
                    double toneHz; double shift; double baud; bool inverted; bool autoRtty;
                    /** ★ V12 2026-10-25: a CW SKIMMER's segment, Hz (0 = an ordinary audio decoder). Its VFO frequency is
                     *  the segment's CENTRE, and it is fed the channel's IQ, not a demodulator's audio. */
                    double widthHz; };
inline const std::vector<DecoderDef>& decoders() {
    static const std::vector<DecoderDef> d = {
        { "navtex",       "NAVTEX",                      "navtex", "navtex", "usb,lsb", 500,  170, 100,   false, false, 0 },
        { "rtty",         "RTTY (auto)",                 "fsk",    "rtty",   "usb,lsb", 1000, 170, 45.45, false, true,  0 },
        { "rtty-ham",     "RTTY 45.45 Bd 170 Hz",        "fsk",    "rtty",   "usb,lsb", 1000, 170, 45.45, false, false, 0 },
        { "rtty-weather", "RTTY 50 Bd 450 Hz (weather)", "fsk",    "rtty",   "usb,lsb", 1000, 450, 50,    true,  false, 0 },
        { "wefax",        "WEFAX (120 LPM)",             "wefax",  "wefax",  "usb",     1900, 0,   0,     false, false, 0 },
        { "sstv",         "SSTV",                        "sstv",   "sstv",   "usb,nfm", 0,    0,   0,     false, false, 0 },
        { "ft8",          "FT8 / FT4 (summary)",         "",       "ft8",    "usb",     0,    0,   0,     false, false, 0 },
        // ★★ CW SKIMMERS (Stuart, 2026-10-25: "CW skimming … lightweight") — one FFT over the segment, a decoder per signal
        //    found; kept as a SUMMARY of the spots, as FT8 is. The VFO is the segment's centre (e.g. 7.020 MHz for
        //    7.008–7.032 at 24 kHz). "usb" only because a VFO row has a demodulator — none runs for a skimmer.
        { "cw-skim-24",   "CW skimmer, 24 kHz (summary)", "cwskim", "cw",    "usb",     0,    0,   0,     false, false, 24000 },
        { "cw-skim-48",   "CW skimmer, 48 kHz (summary)", "cwskim", "cw",    "usb",     0,    0,   0,     false, false, 48000 },
        { "cw-skim-96",   "CW skimmer, 96 kHz (summary)", "cwskim", "cw",    "usb",     0,    0,   0,     false, false, 96000 },
        // ★★ PSK SKIMMERS (V12 2026-10-26) — the same shape: the VFO is the segment's centre (e.g. 14.0715 MHz for
        //    14.0655–14.0775 at 12 kHz — the PSK segment and its neighbours), a summary of the spots.
        { "psk-skim-6",   "PSK skimmer, 6 kHz (summary)",  "pskskim", "psk", "usb",     0,    0,   0,     false, false, 6000 },
        { "psk-skim-12",  "PSK skimmer, 12 kHz (summary)", "pskskim", "psk", "usb",     0,    0,   0,     false, false, 12000 },
        // ★★ V12 APRS (docs/v12/AIS-APRS.md): 1200 Bd on an NFM VFO (the region's channel), or HF 300 Bd on USB/LSB —
        //    kept as the day's packets + a station card. NRZI does not care which way round the tones are, so LSB is fine.
        { "aprs",         "APRS (1200 Bd, VHF)",          "aprs",   "aprs",  "nfm",     0,    0,   1200,  false, false, 0 },
        { "aprs-hf",      "APRS HF (300 Bd)",             "aprs",   "aprs",  "usb,lsb", 0,    0,   300,   false, false, 0 },
        // ★★ DSC (V12, 2026-10-26): MF/HF on SSB, the tones 1.7 kHz from the dial (2185.8 USB for 2187.5), either sense
        //    found by the decoder; VHF channel 70 on NFM. Kept as the day's log + an item per distress/urgency call.
        { "dsc",          "DSC (MF/HF)",                  "dsc",    "dsc",   "usb,lsb", 1700, 170, 100,   false, false, 0 },
        { "dsc-vhf",      "DSC (VHF channel 70)",         "dsc",    "dsc",   "nfm",     0,    0,   1200,  false, false, 0 },
        // ★★ V12 2026-10-29: SITOR-A (AMTOR mode A, ARQ) — both stations of a link, the tones 1 kHz from the dial as for
        //    RTTY; either sense is found by the decoder. Kept as an RTTY-style run of text.
        { "sitora",       "SITOR-A (ARQ)",                "sitora", "sitora", "usb,lsb", 1000, 170, 100,  false, false, 0 },
        // ★★ V12 2026-10-27 — DIGITAL SPOTS: BAND (Stuart: "Also full band digital modes too"). Every digital mode on the
        //    band the VFO's frequency is in — FT8, FT4, WSPR, JS8, FT2, JTTY, PSK on their own dials, the listener's own
        //    Band mode (decoders/digi_spotter.h) with its CPU governor — kept as ONE summary for the band, as FT8's is.
        //    The frequency only NAMES the band (7074 = 40 m): the slice is cut where the band's modes are (bandSpan).
        //    "usb" only because a VFO row has a demodulator — none runs (it is fed IQ, as a skimmer is).
        { "spots-band",   "Digital spots: Band (summary)", "",      "ft8",   "usb",     0,    0,   0,     false, false, 0 },
        // ★★ V12 2026-10-27 — THE NDB SKIMMER (Stuart: NDB "must just run beside them" — the Lenovo's RSP at 1200 kHz,
        //    3 MS/s, with NAVTEX 489.5 + 517.5 and 160 m FT8). Every beacon in as much of the LF beacon band (190–535 kHz)
        //    as the profile's capture holds: the listener's own NDB skimmer on ONE IQ slice of the runner's channeliser,
        //    into the same gallery cards a listener's NDB mode writes (the day's heard log and map, last 10 min, hourly).
        //    ★ ITS FREQUENCY MEANS NOTHING — the decoder sets it (ndbSegment): the setup page greys the box out and the row
        //      stores hz 0. "usb" only because a VFO row has a demodulator — none runs (it is fed IQ).
        { "ndb-skim",     "NDB skimmer (summary)",        "",       "ndb",   "usb",     0,    0,   0,     false, false, 0 },
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
inline double passbandHz(const Vfo& v) {
    const DecoderDef* d = decoderDef(v.dec);
    if (d && d->widthHz > 0) return d->widthHz;     // ★ a skimmer's whole segment
    return v.mode == "nfm" ? 12500.0 : v.dec == "ft8" ? 3000.0 : 2700.0;
}
/** ★ A CW skimmer VFO (fed IQ, no demodulator). */
inline bool isSkimmer(const Vfo& v) { const DecoderDef* d = decoderDef(v.dec); return d && d->widthHz > 0; }
/** ★ A Band digital-spots VFO (V12 2026-10-27): fed the band's IQ slices, no demodulator. */
inline bool isBandSpots(const Vfo& v) { return v.dec == "spots-band"; }
/** ★ An NDB skimmer VFO (V12 2026-10-27): fed one IQ slice over the NDB band, no demodulator, its frequency ignored. */
inline bool isNdbSkim(const Vfo& v) { return v.dec == "ndb-skim"; }
/** ★★ THE NDB BAND, as the listener's NDB mode has it (vibe_ndb_service.h kBandLoHz / kBandHiHz / kMinSpanHz — the runner
 *  static_asserts they agree; this header is read by the daemon's save path without the decoder). */
constexpr double kNdbBandLoHz = 190e3, kNdbBandHiHz = 535e3, kNdbMinSpanHz = 100e3;
/** ★ Where an older whole-radio NDB profile's capture is put when it is converted to a VFO profile: the band's middle,
 *  a quarter-kHz off the 0.5 kHz raster so the DC remnant lands between beacons (vibendb::plan's rule). */
constexpr double kNdbLegacyCentreHz = 362750;

/** ★★ WHERE A BAND VFO'S SLICE GOES — the registered modes' span of the band `hz` is in (digi::spanOf: its RF edges and
 *  centre — the main slice's; the extra slices are the host's to cut where the capture holds them). The registry of
 *  modes lives with the decoders (decoders/digi_modes.cpp), and this header is read by the daemon's save path without
 *  them — so the span comes through this hook, installed by vibe_bgdecode.h wherever the decoders are linked (the
 *  server, the tests that run the runner). Unset (a test of the config alone): a band VFO is checked by its frequency
 *  only. Returns false when `hz` is on no band a registered mode uses. */
struct BandSpan { double loHz = 0, hiHz = 0, centreHz = 0; std::string name; };
inline std::function<bool(double hz, BandSpan& out)>& bandSpanHook() { static std::function<bool(double, BandSpan&)> f; return f; }
inline bool bandSpan(double hz, BandSpan& out) { auto& f = bandSpanHook(); return f && f(hz, out); }

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
    else if (id == "aprs" || id == "aprs-hf") std::snprintf(b, sizeof b, "{\"baud_rate\":%.0f}", d->baud);
    else if (id == "dsc") std::snprintf(b, sizeof b, "{\"band\":\"hf\",\"center_frequency\":%.0f}", d->toneHz);
    else if (id == "dsc-vhf") std::snprintf(b, sizeof b, "{\"band\":\"vhf\"}");
    else if (id == "sitora") std::snprintf(b, sizeof b, "{\"center_frequency\":%.0f}", d->toneHz);
    else if (d->widthHz > 0) std::snprintf(b, sizeof b, "{\"width\":%.0f}", d->widthHz);
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
    if (const auto* v = j.get("keepInRange")) if (v->kind == antswitch::Json::Bool) out.keepInRange = v->b;
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
            p.ism = pj.str("ism", "");
            bool legacyNdb = false;
            if (const auto* nd = pj.get("ndb")) legacyNdb = nd->kind == antswitch::Json::Bool && nd->b;
            if (const auto* vs = pj.get("vfos"); vs && vs->kind == antswitch::Json::Arr)
                for (const auto& vj : vs->a) {
                    if (vj.kind != antswitch::Json::Obj) continue;
                    Vfo v;
                    if (const auto* h = vj.get("hz")) if (h->kind == antswitch::Json::Num) v.hz = h->n;
                    v.mode = vj.str("mode", "usb");
                    v.dec  = vj.str("dec", "navtex");
                    p.vfos.push_back(v);
                }
            /* ★★ BACKWARD COMPATIBILITY (V12 2026-10-27): a profile saved while NDB was a whole-radio profile kind
             *  ("ndb":true, its centre and VFOs ignored) becomes the VFO profile it now is — the band's middle as its RF
             *  centre and ONE NDB skimmer VFO. It decodes what it did (every beacon), and saves back in the new shape. */
            if (legacyNdb && p.ism.empty()) {
                p.centreHz = kNdbLegacyCentreHz;
                Vfo v; v.hz = 0; v.mode = "usb"; v.dec = "ndb-skim";
                p.vfos.assign(1, v);
            }
            out.profiles.push_back(p);
        }
    }
    return true;
}
inline std::string toJson(const Config& c) {
    std::string o = std::string("{\"on\":") + (c.on ? "true" : "false") + ",\"waitMin\":";
    char b[64]; std::snprintf(b, sizeof b, "%g", c.waitSec / 60.0); o += b;
    if (!c.keepInRange) o += ",\"keepInRange\":false";   // ★ absent = on (the default), so older settings read back unchanged
    o += ",\"profiles\":[";
    for (size_t i = 0; i < c.profiles.size(); i++) {
        const auto& p = c.profiles[i];
        if (i) o += ',';
        std::snprintf(b, sizeof b, "%.0f", p.centreHz);
        o += std::string("{\"centre\":") + b + ",\"from\":\"" + hhmm(p.fromMin) + "\",\"to\":\"" + hhmm(p.toMin) + "\"";
        o += ",\"vfos\":[";
        for (size_t k = 0; k < p.vfos.size(); k++) {
            const auto& v = p.vfos[k];
            if (k) o += ',';
            std::snprintf(b, sizeof b, "%.0f", v.hz);
            o += std::string("{\"hz\":") + b + ",\"mode\":\"" + gallery::jsonEsc(v.mode) + "\",\"dec\":\"" + gallery::jsonEsc(v.dec) + "\"}";
        }
        o += "]";
        // ★ V12 2026-10-25: absent on an ordinary profile, so older settings read back unchanged (the page's own order)
        if (p.isIsm()) o += ",\"ism\":\"" + gallery::jsonEsc(p.ism) + "\"";
        o += "}";
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

/** ★★ WHAT AN NDB SKIMMER VFO COVERS: as much of 190–535 kHz as the capture at `centre` and `rate` holds — inside the same
 *  guard every background VFO keeps from the capture's edge (halfWindowHz), clipped to what the radio reaches
 *  ([reachLo, reachHi]; 0 = no limit). False when under kNdbMinSpanHz of the band is there (or the rate is unknown): the
 *  skimmer would hear too few beacons to be worth a decoder slot. */
inline bool ndbSegment(double centre, double rate, double& lo, double& hi, double reachLo = 0, double reachHi = 0) {
    const double half = rate > 0 ? halfWindowHz(rate) : 0;
    lo = std::max(kNdbBandLoHz, centre - half); hi = std::min(kNdbBandHiHz, centre + half);
    if (reachLo > 0) lo = std::max(lo, reachLo);
    if (reachHi > 0) hi = std::min(hi, reachHi);
    return rate > 0 && hi - lo >= kNdbMinSpanHz;
}

static inline std::string khz(double hz) { char b[32]; std::snprintf(b, sizeof b, "%.1f kHz", hz / 1e3); return b; }

/** ★★ WHAT THE SAVE REFUSES, IN WORDS. `rate` is the radio's sample rate (0 = unknown: the window is not checked).
 *  `slotCap` > 0: a SINGLE-radio server (Stuart, 2026-10-22) — a profile may have no more VFOs than the box has decoder
 *  slots, because nothing else competes for them there (a listener arriving stops background decoding completely).
 *  On a multi-radio server the cap is 0: background takes whatever slots are free, in its own priority order. */
/** `lockCentre` > 0: a LOCKED-RANGE radio (Stuart, 2026-10-22) — its window never moves, a profile has no RF centre of
 *  its own, and every VFO must sit inside the locked window. */
/** `ismOk` false: this radio reaches no ISM band at all (an Airspy HF+ tops out near 260 MHz; an Icom is a transceiver's
 *  audio) — an ISM profile is refused there, in words (AGENTS.md: no control that cannot work on this radio). Whether it
 *  reaches the CHOSEN band through the owner's own allow/block ranges is the radio's to say when the profile runs.
 *  `ndbOk` false: this radio reaches no LF (an RTL without direct sampling, a HackRF, an Airspy R2/Mini, an Icom) — an
 *  NDB skimmer VFO is refused there, in words. */
inline bool validate(const Config& c, double rate, std::string& err, int slotCap = 0, double lockCentre = 0, bool ismOk = true, bool ndbOk = true) {
    if (c.profiles.size() > kMaxProfiles) { err = "background decoding: at most " + std::to_string(kMaxProfiles) + " profiles"; return false; }
    if (c.on && c.profiles.empty()) { err = "background decoding is on but has no profile — add one, or switch it off"; return false; }
    for (size_t i = 0; i < c.profiles.size(); i++) {
        const auto& p = c.profiles[i];
        const std::string name = "Profile " + std::to_string(i + 1);
        // ★★ AN ISM PROFILE: a band, not a centre and VFOs. It MOVES the radio onto the band, so never on a locked radio
        //    (background decoding never moves one), and it needs the capture ISM mode needs (≥ 0.9 MS/s).
        if (p.isIsm()) {
            const vibeism::Band* b = vibeism::bandById(p.ism);
            if (!b) { err = name + ": choose an ISM band (315, 433.92, 868 or 915 MHz)"; return false; }
            if (!ismOk) { err = name + ": this radio cannot tune to an ISM band"; return false; }
            if (lockCentre > 0) {
                err = name + ": ISM puts the radio on the " + std::string(b->name) + " band, and this radio's range is locked \xe2\x80\x94 "
                      "background decoding never moves a locked radio. Unlock it, or choose VFOs inside its range.";
                return false;
            }
            if (rate > 0 && rate < vibeism::kMinRateHz) {
                char t[200];
                std::snprintf(t, sizeof t, "%s: ISM needs a capture of 0.9 MS/s or more \xe2\x80\x94 this radio runs at %.3f MS/s", name.c_str(), rate / 1e6);
                err = t; return false;
            }
            continue;
        }
        if (!(p.centreHz > 0) && !(lockCentre > 0)) { err = name + ": set an RF centre"; return false; }
        const double centre = lockCentre > 0 ? lockCentre : p.centreHz;
        if (p.vfos.empty()) { err = name + ": add at least one VFO"; return false; }
        if (slotCap > 0 && p.vfos.size() > (size_t)slotCap) {
            err = name + " has " + std::to_string(p.vfos.size()) + " VFOs \xe2\x80\x94 this server can run " + std::to_string(slotCap)
                + (slotCap == 1 ? " decoder" : " decoders") + " at once, and that is the most VFOs a profile can have. Remove "
                + std::to_string(p.vfos.size() - (size_t)slotCap) + ", or raise \"Decoders at once\" on the Server tab.";
            return false;
        }
        if (p.vfos.size() > kMaxVfos) { err = name + ": at most " + std::to_string(kMaxVfos) + " VFOs"; return false; }
        for (size_t k = 0; k < p.vfos.size(); k++) {
            const auto& v = p.vfos[k];
            const std::string vn = name + ", VFO " + std::to_string(k + 1);
            /* ★★ AN NDB SKIMMER VFO (V12 2026-10-27): no frequency of its own — it covers what the capture holds of the NDB
             *  band. Refused, in words, on a radio that reaches no LF at all (an RTL without direct sampling, a HackRF, an
             *  Airspy R2/Mini, an Icom — the page does not even offer it there), where the capture misses the band, and a
             *  second time in one profile (it would hear the same beacons twice). */
            if (isNdbSkim(v)) {
                if (!ndbOk) { err = vn + ": this radio cannot reach the NDB band (190\xe2\x80\x93" "535 kHz) \xe2\x80\x94 an RTL needs direct sampling, and a HackRF, an Airspy R2/Mini or an Icom cannot do it at all"; return false; }
                for (size_t q = 0; q < k; q++)
                    if (isNdbSkim(p.vfos[q])) { err = vn + ": this profile already has an NDB skimmer (VFO " + std::to_string(q + 1) + ") \xe2\x80\x94 one covers every beacon its capture holds"; return false; }
                double lo = 0, hi = 0;
                if (rate > 0 && !ndbSegment(centre, rate, lo, hi)) {
                    const double half = halfWindowHz(rate);
                    char b[420];
                    std::snprintf(b, sizeof b, "%s: the NDB skimmer needs at least %.0f kHz of the NDB band (190\xe2\x80\x93" "535 kHz) inside the capture "
                                  "\xe2\x80\x94 at %.0f kS/s around %s this radio sees %.1f\xe2\x80\x93%.1f kHz%s. %s",
                                  vn.c_str(), kNdbMinSpanHz / 1e3, rate / 1e3, khz(centre).c_str(), std::max(0.0, centre - half) / 1e3, (centre + half) / 1e3,
                                  hi > lo ? (std::string(", only ") + khz(hi - lo) + " of the band").c_str() : ", none of the band",
                                  lockCentre > 0 ? "On a locked radio the lock must cover the band." : "Move the RF centre nearer the band (e.g. 362.75 kHz), or remove the NDB skimmer.");
                    err = b; return false;
                }
                continue;
            }
            // ★ V12 2026-10-28: a Band row PICKS its band on the setup page (its dial is what is stored) — say so
            if (!(v.hz > 0)) { err = vn + (isBandSpots(v) ? ": choose its band" : ": set a frequency"); return false; }
            if (!demodOk(v.mode)) { err = vn + ": background decoding runs on USB, LSB or NFM"; return false; }
            if (!decoderDef(v.dec)) { err = vn + ": choose a decoder"; return false; }
            if (!decoderFitsDemod(v.dec, v.mode)) {
                std::string need = decoderDef(v.dec)->modes, m = v.mode;
                for (auto& c : need) c = c == ',' ? '/' : (char)std::toupper((unsigned char)c);
                for (auto& c : m) c = (char)std::toupper((unsigned char)c);
                err = vn + ": " + std::string(decoderDef(v.dec)->label) + " does not work on " + m + " \xe2\x80\x94 it needs " + need;
                return false;
            }
            /* ★★ A BAND VFO: its frequency must name a band the digital modes use, and the band's main slice (the cluster
             *  round FT8, up to 48 kHz) must sit inside the window — checked like a skimmer's segment, from the span's own
             *  centre. Its extra slices (40 m's JTTY at 7.090 …) are cut only where the capture holds them, as for a
             *  listener's Band; one beyond it is simply not decoded. */
            if (isBandSpots(v) && bandSpanHook()) {
                BandSpan sp;
                if (!bandSpan(v.hz, sp)) {
                    err = vn + ": " + khz(v.hz) + " is not in a band the digital modes use \xe2\x80\x94 choose a band, "
                          "e.g. 40 m or 20 m";
                    return false;
                }
                if (rate > 0) {
                    const double half = halfWindowHz(rate) + 3000.0;
                    const double off = std::max(std::fabs(sp.loHz - centre), std::fabs(sp.hiHz - centre));
                    if (off > half) {
                        char b[400];
                        std::snprintf(b, sizeof b, "%s: the %s band's digital modes (%.4f\xe2\x80\x93%.4f MHz) are not all inside this radio's "
                                      "window \xe2\x80\x94 at %.0f kS/s it sees about \xc2\xb1%s around %s. %s",
                                      vn.c_str(), sp.name.c_str(), sp.loHz / 1e6, sp.hiHz / 1e6, rate / 1e3, khz(half).c_str(),
                                      khz(centre).c_str(), lockCentre > 0 ? "On a locked radio they must sit inside it." : "Move the RF centre nearer the band.");
                        err = b; return false;
                    }
                }
                continue;
            }
            if (rate > 0) {
                // ★ A skimmer needs its whole segment inside the window, not a 3 kHz passband.
                const double half = halfWindowHz(rate) + 3000.0 - (isSkimmer(v) ? passbandHz(v) / 2.0 : 3000.0);
                const double off = std::fabs(v.hz - centre);
                if (off > half && lockCentre > 0) {
                    char b[300];
                    std::snprintf(b, sizeof b, "%s (%s) is outside this radio's locked range (%.3f\xe2\x80\x93%.3f MHz) \xe2\x80\x94 on a locked "
                                  "radio background VFOs must sit inside it.", vn.c_str(), khz(v.hz).c_str(),
                                  std::max(0.0, centre - half) / 1e6, (centre + half) / 1e6);
                    err = b; return false;
                }
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
