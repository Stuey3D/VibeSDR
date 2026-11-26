// ★★★ "REMOTE ACCESS TO SERVER SETTINGS" ON ANDROID (beta88, 2026-11-20) — the /setup page for a phone's server.
//
// Stuart: "can we expose the android server settings via the /setup page like we do on linux servers. it would make it
// easier for admins to remote manage when updates add new features … When a setting is changed in the remote setup page
// the setting changes on the device menu too so its mirrored and both views show the current status."
//
// ★★ ONE PAGE, ONE API. The Linux page (vibe_setup_page.h) already talks to GET/POST /vibeserver/config behind the admin
//    password's nonce + HMAC challenge, with the brute-force lockout (local_sdr_shim.cpp). On a phone those two endpoints
//    were registered by nobody and answered 501. With the owner's switch on, the Android app registers handlers that go
//    through THIS file, so the same page, the same auth, the same lockout and the same CSRF-proof challenge serve it.
//
// ★★ ONE TRUTH. The phone's settings live in the server process (VibeRemoteSetup.kt) as the very config the server is
//    started and restored from — the keys VibeServerBoot reads. This file only TRANSLATES:
//      pageConfig()      Android settings → the page's config shape (one radio), plus the list of controls the phone's
//                        server actually has (`android.supported`) — the page hides everything else. A control the
//                        phone does not have is never drawn dead (AGENTS.md).
//      settingsPatch()   the page's save → a patch of Android keys, ONLY for the supported controls. Anything else the
//                        page posts (a governor, a Wi-Fi network, a second radio) is ignored, never stored.
//    The phone's screen reads and writes the same store, and the server tells it when the page changed something.
//
// ★ Header-only, no dependencies, so the desktop tests compile it as they are (vibeserver/test-android-setup.cpp).
// ★ The JSON reader below is small and strict: it accepts what JSON.stringify and org.json write, and refuses the rest.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vibe {
namespace androidsetup {

// ── A small JSON value ─────────────────────────────────────────────────────────────────────────────────────────────

struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    const J* get(const std::string& k) const {
        if (t != Obj) return nullptr;
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    J* getMut(const std::string& k) {
        if (t != Obj) return nullptr;
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    void set(const std::string& k, J v) {
        if (t != Obj) { t = Obj; o.clear(); }
        if (J* x = getMut(k)) { *x = std::move(v); return; }
        o.emplace_back(k, std::move(v));
    }
    static J num(double v) { J j; j.t = Num; j.n = v; return j; }
    static J str(const std::string& v) { J j; j.t = Str; j.s = v; return j; }
    static J boolean(bool v) { J j; j.t = Bool; j.b = v; return j; }
    static J obj() { J j; j.t = Obj; return j; }
    static J arr() { J j; j.t = Arr; return j; }
    static J null() { return J(); }
};

namespace detail {
struct P {
    const std::string& s; size_t i = 0; int depth = 0;
    explicit P(const std::string& x) : s(x) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool lit(const char* w) { size_t n = std::strlen(w); if (s.compare(i, n, w) != 0) return false; i += n; return true; }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;  case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp; if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo;
                        if (i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') { i += 2; if (!hex4(lo)) return false; }
                        else return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                    utf8(out, cp); break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool val(J& v) {
        if (++depth > 32) return false;   // ★ a hostile body cannot recurse us off the stack
        ws();
        if (i >= s.size()) return false;
        bool ok = true;
        char c = s[i];
        if (c == '{') {
            v = J::obj(); ++i; ws();
            if (i < s.size() && s[i] == '}') { ++i; }
            else for (;;) {
                ws(); std::string k; if (!str(k)) { ok = false; break; }
                ws(); if (i >= s.size() || s[i] != ':') { ok = false; break; } ++i;
                J x; if (!val(x)) { ok = false; break; }
                v.set(k, std::move(x));
                ws(); if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            v = J::arr(); ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; }
            else for (;;) {
                J x; if (!val(x)) { ok = false; break; }
                v.a.push_back(std::move(x));
                ws(); if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                ok = false; break;
            }
        } else if (c == '"') { v.t = J::Str; ok = str(v.s); }
        else if (lit("true")) { v = J::boolean(true); }
        else if (lit("false")) { v = J::boolean(false); }
        else if (lit("null")) { v = J::null(); }
        else {
            const char* b = s.c_str() + i; char* e = nullptr;
            double d = std::strtod(b, &e);
            if (e == b || !std::isfinite(d)) ok = false;
            else { v = J::num(d); i += size_t(e - b); }
        }
        --depth;
        return ok;
    }
};
}  // namespace detail

inline bool parse(const std::string& text, J& out) {
    detail::P p(text);
    if (!p.val(out)) return false;
    p.ws();
    return p.i == text.size();
}

inline void dumpTo(const J& v, std::string& out) {
    switch (v.t) {
        case J::Null: out += "null"; break;
        case J::Bool: out += v.b ? "true" : "false"; break;
        case J::Num: {
            char buf[40];
            if (std::fabs(v.n) < 1e15 && v.n == std::floor(v.n)) std::snprintf(buf, sizeof buf, "%.0f", v.n);
            else std::snprintf(buf, sizeof buf, "%.10g", v.n);
            out += buf; break;
        }
        case J::Str: {
            out += '"';
            for (unsigned char c : v.s) {
                if (c == '"') out += "\\\""; else if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n"; else if (c == '\r') out += "\\r"; else if (c == '\t') out += "\\t";
                else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += char(c);
            }
            out += '"'; break;
        }
        case J::Arr: {
            out += '[';
            for (size_t k = 0; k < v.a.size(); ++k) { if (k) out += ','; dumpTo(v.a[k], out); }
            out += ']'; break;
        }
        case J::Obj: {
            out += '{';
            for (size_t k = 0; k < v.o.size(); ++k) {
                if (k) out += ',';
                dumpTo(J::str(v.o[k].first), out); out += ':'; dumpTo(v.o[k].second, out);
            }
            out += '}'; break;
        }
    }
}
inline std::string dump(const J& v) { std::string s; dumpTo(v, s); return s; }

// ── Reading values the way the Android side writes them ──────────────────────────────────────────────────────────

inline double numOr(const J* v, double d) {
    if (!v) return d;
    if (v->t == J::Num) return v->n;
    if (v->t == J::Bool) return v->b ? 1 : 0;
    if (v->t == J::Str && !v->s.empty()) { char* e = nullptr; double x = std::strtod(v->s.c_str(), &e); if (e && *e == 0 && std::isfinite(x)) return x; }
    return d;
}
inline bool boolOr(const J* v, bool d) {
    if (!v) return d;
    if (v->t == J::Bool) return v->b;
    if (v->t == J::Num) return v->n != 0;
    if (v->t == J::Str) return v->s == "1" || v->s == "true";
    return d;
}
inline std::string strOr(const J* v, const std::string& d) {
    if (!v) return d;
    if (v->t == J::Str) return v->s;
    if (v->t == J::Num) { std::string o; dumpTo(*v, o); return o; }
    return d;
}

// ── What the phone's server has, as the page's control ids ───────────────────────────────────────────────────────

/* ★★★ EVERY SETTING ON THE PHONE'S SERVER SCREEN IS ON THE PAGE (Stuart, 2026-11-20: "all settings for android need to be
 *  on the remote setup page"). The first cut (beta88 morning) carried 28 controls; this table carries every setting
 *  ServerModeScreen has, except the ones only the phone can change (docs/v12/ANDROID-SERVER-PROCESS.md, "Device-only") and
 *  the admin password, which never travels to or from the page.
 *
 * ★★ A CONTROL THE RADIO CANNOT USE IS NEVER OFFERED (AGENTS.md). Each row says which radios it belongs to — the SAME rule
 *    ServerModeScreen draws by (isRtl = no radio or an RTL; hasHwSetup = not an HF+ and not an Icom; the SDRplay card only
 *    for an RSP; the HF+ calibration only for an HF+; an Icom's own settings only for an Icom) — and which builds/devices
 *    (raw IQ is not on Lite; the battery is not on a television; the public listing needs the tunnel binary). The page
 *    hides every control not in `android.supported`; a save of a control that is not supported here is IGNORED, never
 *    stored, whatever the page posts.
 * ★ P_ADV rows are the settings the phone's server applies only in Advanced mode (VibeServerBoot: `if (adv)`); the page
 *   hides them while its Simple/Advanced choice says Simple (android.advancedOnly), live, as the phone's screen does.
 * ★ Ids are the page's own control ids. A row may name a CONTAINER whose generated inputs have no id of their own (the
 *   modes tree, the background-decoding profiles, the per-band gain rows): the page keeps an id-less input when its
 *   nearest ancestor with an id is supported (androidPrune). */
enum : unsigned {
    D_RTL = 1u << 0, D_AHF = 1u << 1, D_ASP = 1u << 2, D_HRF = 1u << 3, D_RSP = 1u << 4, D_ICOM = 1u << 5,
    D_ANY = 0x3Fu,
    D_RADIO = D_ANY & ~D_ICOM,              // every radio but an Icom (an Icom serves its own one dial)
    D_HWSETUP = D_RTL | D_ASP | D_HRF | D_RSP,   // ServerModeScreen hasHwSetup: not an HF+, not an Icom
};
enum : unsigned { P_ADV = 1u, P_NOT_LITE = 2u, P_NOT_TV = 4u, P_TUNNEL = 8u, P_LITE = 16u };

struct Control { const char* ids; unsigned drivers; unsigned needs; };

/** ★★ THE TABLE — every remote control, its radios and its needs. Adding a setting to the phone's screen means a row here,
 *  its mapping in pageConfig() and settingsPatch() below, and its mirror in src/services/serverSettingsMirror.ts;
 *  scripts/check-remote-keys.mjs fails until all of them agree, and until every ServerModeScreen key is either here or in
 *  the documented device-only list. */
inline const std::vector<Control>& controlTable() {
    static const std::vector<Control> t = {
        // ── the server tab ──
        { "name mdns",                                   D_ANY,   0 },
        { "landingMessage landingLinkUrl landingLinkLabel", D_ANY, 0 },
        { "uncompressed",                                D_ANY,   0 },
        { "trustedProxies",                              D_ANY,   P_ADV },
        { "maxRadiosPerIp",                              D_ANY,   0 },
        { "decoderMax rttySkimWorkers",                  D_ANY,   0 },
        { "galleryMb galleryPublic gallerySaveLive",     D_ANY,   0 },
        // ★★ V12 beta94 the community map (vibe_community.h) — the owner's switch, off by default
        { "communityMap communityModes",                 D_ANY,   0 },
        // ★ V12 beta90 PSK Reporter + WSPRnet (every radio's digital spots); the DMR ID lookup only where DV ID can run
        { "pskrOn pskrCall pskrAntenna wsprnetOn wsprnetCall", D_ANY, 0 },
        { "dmrIdLookup",                                 D_RADIO, 0 },
        { "batteryPauseAt",                              D_ANY,   P_NOT_TV },
        { "place lat lon locFollow country locator",     D_ANY,   0 },
        { "dirList dirName dirShareSec",                 D_ANY,   P_TUNNEL },
        // ★ The phone's own three, which the Linux page never needed (androidSrvCard): the web client, Opus, Simple/Advanced.
        { "androidWebServer androidCompress androidAdvanced", D_ANY, 0 },
        // ── the radio tab ──
        { "radioName antenna",                           D_ANY,   0 },
        { "radioPin radioPinClear",                      D_ANY,   0 },
        { "users askFirst",                              D_ANY,   P_ADV },
        { "modeSingle modeLocked lockFreq",              D_RADIO, P_ADV },
        { "zoomSpectrum",                                D_RADIO, 0 },
        { "landingFreq demodMode",                       D_RADIO, 0 },
        { "sessionLimit sessionLimitMode idleKick",      D_ANY,   0 },
        { "nbWide vfoB",                                 D_RADIO, 0 },
        { "idleGrace",                                   D_ANY,   0 },
        { "spectrogram",                                 D_RADIO, P_ADV },
        { "rate rateLock maxFps",                        D_RADIO, 0 },
        { "modeBlockList bandModeList",                  D_ANY,   0 },   // the modes this aerial is offered (blockedModes)
        { "dabRateBoost",                                D_RADIO, 0 },
        { "adsb256 ism256",                              D_RTL,   0 },
        { "dabScanLabels",                               D_RADIO, P_LITE },
        { "gainRestSlider gainRest gainRestLock rtlAgc androidTunerBw", D_RTL, 0 },
        { "gainAgcLock",                                 D_RTL,   P_ADV },
        { "gainPick gainCurve gainMaxSlider gainMax gainIfSlider gainIfMax gainSplitSlider gainAspLock gainLock "
          "bandCopyFrom gainList gainAdd",               D_RADIO, P_ADV },
        { "gainIfBw",                                    D_RTL,   P_ADV },
        { "allowPick allowLo allowHi blockPick blockLo blockHi", D_ANY, P_ADV },
        { "dabLandOn dabLandBlock dabLandStation adsbLandOn adsbKeep", D_RADIO, 0 },
        { "fmDeemph fmStereo fmNr fmIms fmCeq fmAutoBw fmNb fmReturn fmRemember", D_RADIO, 0 },
        { "antRangePreset antFilterPreset antPortNames", D_ANY,   0 },
        { "swYes swNo swBroker swUser swPass swLocked swAntennas", D_ANY, 0 },
        { "antennaMap antennaPortLocked",                D_RSP,   0 },
        { "ppm",                                         D_RTL | D_RSP, 0 },
        { "ppb",                                         D_AHF,   0 },
        { "biasT",                                       D_HWSETUP, 0 },
        { "autoDs autoDsBelow androidDsFixed",           D_RTL,   0 },
        { "convLo convDown convHi androidConvInLo",      D_HWSETUP, 0 },
        { "icomBaud icomCiv icomScopeSpeed icomTuneLock icomPowerOn icomAutoOff icomOffDelayMin", D_ICOM, 0 },
        { "bgOn bgWait bgKeepInRange bgProfiles",        D_RADIO, 0 },
        { "dxOn dxWait dxBands dxRaster dxOirt dxExt dxViewAdmin", D_RADIO, 0 },
        { "afServe afPort afOpen afPush",                D_RADIO, 0 },   // ★★ V12 beta96 flight trackers (RadioConfig::adsbFeed)
        { "rawIqOn rawIqLan rawIqStreams rawIqLocalOnly", D_RADIO, P_NOT_LITE },
        { "rspVibeAgc rfNotch dabNotch rspAmNotch rspHdr rspExtRef rspDabDecim rfAgc autoNotch userNotch "
          "dabAgcOverride dabAgcTarget agcSet agcSetLock", D_RSP, 0 },
    };
    return t;
}

inline std::vector<std::string> splitIds(const char* s) {
    std::vector<std::string> out; std::string cur;
    for (const char* p = s; ; ++p) {
        if (*p == ' ' || *p == 0) { if (!cur.empty()) out.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return out;
}

/** Which radio, as ServerModeScreen reads it: no radio (or one it does not name) is treated as an RTL-SDR, exactly as the
 *  screen's `isRtl = !radio || radio.driver === 'rtl'` does. */
inline unsigned driverBit(const std::string& d) {
    if (d == "airspyhf") return D_AHF;
    if (d == "airspy") return D_ASP;
    if (d == "hackrf") return D_HRF;
    if (d == "sdrplay") return D_RSP;
    if (d == "icom") return D_ICOM;
    return D_RTL;
}

/** What this phone is, for the table's needs. */
struct Platform { unsigned driver = D_RTL; bool lite = false, tv = false, tunnel = false; };

inline bool rowApplies(const Control& c, const Platform& pf) {
    if (!(c.drivers & pf.driver)) return false;
    if ((c.needs & P_NOT_LITE) && pf.lite) return false;
    if ((c.needs & P_LITE) && !pf.lite) return false;
    if ((c.needs & P_NOT_TV) && pf.tv) return false;
    if ((c.needs & P_TUNNEL) && !pf.tunnel) return false;
    return true;
}

/** The ids the page keeps for THIS phone and radio (and, in `advOnly`, those of them shown only in Advanced). */
inline std::vector<std::string> supportedControls(const Platform& pf, std::vector<std::string>* advOnly = nullptr) {
    std::vector<std::string> ids;
    for (const auto& c : controlTable()) {
        if (!rowApplies(c, pf)) continue;
        for (auto& id : splitIds(c.ids)) { if (advOnly && (c.needs & P_ADV)) advOnly->push_back(id); ids.push_back(std::move(id)); }
    }
    return ids;
}
/** Every id in the table, whatever the radio — for the tests. */
inline std::vector<std::string> allControlIds() {
    std::vector<std::string> ids;
    for (const auto& c : controlTable()) for (auto& id : splitIds(c.ids)) ids.push_back(std::move(id));
    return ids;
}
/** Is control `id` offered on this phone? */
inline bool offered(const Platform& pf, const char* id) {
    for (const auto& c : controlTable()) {
        for (const auto& x : splitIds(c.ids)) if (x == id) return rowApplies(c, pf);
    }
    return false;
}

/** The demodulator names the phone's server lands on — the page's own list is wider (it serves every radio). */
inline bool knownMode(const std::string& m) {
    static const std::set<std::string> ok = { "am", "sam", "nfm", "wfm", "usb", "lsb", "cw", "cwu", "cwl", "dsb", "drm", "dab", "iq", "raw" };
    return ok.count(m) > 0;
}

/** ★★ THE CAPTURE WIDTHS THE PHONE'S SCREEN OFFERS, per radio (ServerModeScreen RATE_OPTIONS_*, without its 0 =
 *  "Client-controlled"). A page save snaps to the nearest of them — the same snap the engine makes to a pinned rate —
 *  so the phone's own picker can always show what the page chose. check-remote-keys.mjs holds these to the screen's. */
inline const std::vector<double>& ratesFor(unsigned drv) {
    static const std::vector<double> rtl = { 2400000, 2160000, 2048000, 1920000, 1792000, 1536000, 1200000, 1024000, 960000, 250000 };
    static const std::vector<double> hrf = { 2000000, 2400000, 4000000, 5000000, 8000000 };
    static const std::vector<double> rsp = { 2000000, 2048000, 3000000, 4000000, 5000000, 6000000, 8000000 };
    static const std::vector<double> ahf = { 912000, 768000, 384000 };
    static const std::vector<double> asp = { 10000000, 6000000, 3000000, 2500000 };
    switch (drv) { case D_HRF: return hrf; case D_RSP: return rsp; case D_AHF: return ahf; case D_ASP: return asp; default: return rtl; }
}
inline double snapTo(const std::vector<double>& list, double v) {
    double best = list.empty() ? v : list[0];
    for (double x : list) if (std::fabs(x - v) < std::fabs(best - v)) best = x;
    return best;
}

/** Maidenhead to 6 characters — the page shows it beside the latitude and longitude. */
inline std::string gridOf(double lat, double lon) {
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return "";
    double x = std::min(359.9999, lon + 180), y = std::min(179.9999, lat + 90);
    std::string g;
    g += char('A' + int(x / 20)); g += char('A' + int(y / 10));
    g += char('0' + int(std::fmod(x, 20) / 2)); g += char('0' + int(std::fmod(y, 10)));
    g += char('a' + int(std::fmod(x, 2) * 12)); g += char('a' + int(std::fmod(y, 1) * 24));
    return g;
}

inline Platform platformOf(const J& in) {
    Platform pf;
    const J* rad = in.get("radio");
    const J* dev = in.get("device");
    pf.driver = driverBit(rad ? strOr(rad->get("driver"), "") : "");
    if (dev) {
        pf.lite = boolOr(dev->get("lite"), false);
        pf.tv = boolOr(dev->get("tv"), false);
        pf.tunnel = boolOr(dev->get("tunnelSupported"), false);
    }
    return pf;
}

/**
 * Android settings → the page's config.
 * `raw` = {"settings":{…the server's config…}, "device":{…what only the phone knows…}, "radio":{"driver","model"}}.
 * Returns "" if `raw` is not JSON.
 */
inline std::string pageConfig(const std::string& raw) {
    J in; if (!parse(raw, in) || in.t != J::Obj) return "";
    J empty = J::obj();
    const J* st = in.get("settings"); if (!st || st->t != J::Obj) st = &empty;
    const J* dev = in.get("device");
    const J* rad = in.get("radio");
    const Platform pf = platformOf(in);
    auto S = [&](const char* k) { return st->get(k); };

    J out = J::obj();
    out.set("configured", J::boolean(true));
    out.set("host", J::str("android"));
    out.set("name", J::str(strOr(S("name"), "VibeSDR")));
    out.set("mdnsAdvertise", J::boolean(boolOr(S("advertise"), true)));
    out.set("landingMessage", J::str(strOr(S("landingMessage"), "")));
    out.set("landingLinkUrl", J::str(strOr(S("landingLinkUrl"), "")));
    out.set("landingLinkLabel", J::str(strOr(S("landingLinkLabel"), "")));
    out.set("uncompressed", J::num(numOr(S("uncompressedAudio"), 0)));
    out.set("trustedProxies", J::str(strOr(S("trustedProxies"), "")));
    const bool oneIp = boolOr(S("oneRadioPerIp"), true);
    out.set("oneRadioPerIp", J::boolean(oneIp));
    out.set("maxRadiosPerIp", J::num(oneIp ? 1 : 0));
    out.set("decoderMax", J::num(numOr(S("decoderMax"), 0)));
    out.set("rttySkimWorkers", J::num(numOr(S("rttySkimWorkers"), 0)));
    out.set("galleryMb", J::num(numOr(S("galleryMb"), 100)));
    out.set("galleryPublic", J::boolean(boolOr(S("galleryPublic"), true)));
    out.set("gallerySaveLive", J::boolean(boolOr(S("gallerySaveLive"), true)));
    out.set("communityMap", J::boolean(boolOr(S("communityMap"), false)));   // ★★ V12 beta94 absent = OFF
    out.set("communityOff", J::str(strOr(S("communityOff"), "")));
    // ★★ V12 beta90 PSK Reporter / WSPRnet / the DMR ID lookup — absent = OFF
    out.set("pskrOn", J::boolean(boolOr(S("pskrOn"), false)));
    out.set("pskrCall", J::str(strOr(S("pskrCall"), "")));
    out.set("pskrAntenna", J::str(strOr(S("pskrAntenna"), "")));
    out.set("wsprnetOn", J::boolean(boolOr(S("wsprnetOn"), false)));
    out.set("wsprnetCall", J::str(strOr(S("wsprnetCall"), "")));
    out.set("dmrIdLookup", J::boolean(boolOr(S("dmrIdLookup"), false)));
    out.set("batteryPauseAt", J::num(numOr(S("batteryPauseAt"), 0)));
    out.set("batteryResumeAt", J::num(numOr(S("batteryResumeAt"), 40)));
    out.set("port", J::num(0));
    // ★ Where the receiver is (the phone's RECEIVER LOCATION): "manual" = a place with a position; "off" = none published.
    {
        const J* loc = S("serverLocation");
        const bool manual = loc && strOr(loc->get("mode"), "off") == "manual";
        const double lat = loc ? numOr(loc->get("lat"), NAN) : NAN, lon = loc ? numOr(loc->get("lon"), NAN) : NAN;
        const bool pos = manual && std::isfinite(lat) && std::isfinite(lon);
        auto fix = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.5f", v); return std::string(b); };
        out.set("place", J::str(pos ? strOr(loc->get("label"), "") : ""));
        out.set("lat", J::str(pos ? fix(lat) : ""));
        out.set("lon", J::str(pos ? fix(lon) : ""));
        out.set("locator", J::str(pos ? gridOf(lat, lon) : ""));
        out.set("country", J::str(loc ? strOr(loc->get("country"), "") : ""));
        out.set("locFollow", J::boolean(loc ? boolOr(loc->get("moving"), false) : false));
    }
    // ★ The public listing: the switch is what the phone's tunnel is doing now (device.listing), the name the owner's.
    {
        const J* li = dev ? dev->get("listing") : nullptr;
        out.set("dirList", J::boolean(li ? boolOr(li->get("wanted"), false) : false));
        out.set("dirName", J::str(strOr(S("publicName"), "")));
        // ★ The page's own choices (Until I turn it off, 1 h, 6 h, 1 day, 1 week, 30 days) — the nearest, so the box is never blank.
        out.set("dirShareSec", J::num(snapTo({ 0, 3600, 21600, 86400, 604800, 2592000 }, numOr(S("publicShareSec"), 0))));
        out.set("dirStatus", li && li->t == J::Obj ? *li : J::obj());
    }

    J r = J::obj();
    r.set("serial", J::str("android"));
    r.set("enabled", J::boolean(true));
    r.set("configured", J::boolean(true));
    r.set("driver", J::str(rad ? strOr(rad->get("driver"), "") : ""));
    r.set("model", J::str(rad ? strOr(rad->get("model"), "") : ""));
    const bool locked = pf.driver != D_ICOM && strOr(S("radioUse"), "single") == "locked";
    r.set("mode", J::str(locked ? "locked" : "single"));
    r.set("label", J::str(strOr(S("radioLabel"), "")));
    r.set("antenna", J::str(strOr(S("antenna"), "")));
    r.set("users", J::num(std::max(1.0, numOr(S("maxUsers"), 1))));
    r.set("askBeforeTuning", J::boolean(boolOr(S("askBeforeTuning"), false)));
    r.set("zoomSpectrum", J::boolean(boolOr(S("zoomSpectrum"), true)));
    // ★ The landing: the phone sends centerFreq + mode only when the owner chose one (0 = "say nothing").
    r.set("landingFreq", J::num(numOr(S("centerFreq"), 0)));
    r.set("demodMode", J::str(strOr(S("mode"), "am")));
    r.set("sessionLimitMin", J::num(numOr(S("sessionLimitMin"), 0)));
    r.set("sessionLimitSoft", J::boolean(boolOr(S("sessionLimitSoft"), false)));
    r.set("idleKickMin", J::num(numOr(S("idleKickMin"), 0)));
    r.set("nbWide", J::num(numOr(S("nbWide"), 1)));
    r.set("vfoB", J::num(numOr(S("vfoB"), 1)));
    r.set("idleGrace", J::num(numOr(S("idleGraceSec"), 300)));
    // ★ The PIN comes down behind the admin password like the Linux file does; the page never renders it, only its state.
    r.set("pin", J::str(strOr(S("pin"), "")));
    // ── the capture: the phone's "rate 0 = the listener's choice" is the page's rate + an unticked pin box ──
    {
        const double lockedRate = numOr(S("lockedRate"), 0);
        r.set("rate", J::num(lockedRate > 0 ? lockedRate : snapTo(ratesFor(pf.driver), numOr(S("sampleRate"), 2400000))));
        r.set("rateLock", J::boolean(lockedRate > 0));
        const double fps = numOr(S("maxFftRate"), 10);
        r.set("maxFps", J::num(fps > 0 ? fps : 10)); r.set("fftRate", J::num(fps > 0 ? fps : 10));
    }
    r.set("lockFreq", J::num(numOr(S("lockedCentre"), 0)));
    r.set("spectrogram", J::boolean(boolOr(S("spectrogram"), false)));
    r.set("blockedModes", J::str(strOr(S("blockedModes"), "")));
    r.set("dabRateBoost", J::boolean(boolOr(S("dabRateBoost"), false)));
    r.set("adsb256", J::boolean(boolOr(S("adsb256"), false)));
    r.set("ism256", J::boolean(boolOr(S("ism256"), false)));
    r.set("dabScanLabels", J::num(numOr(S("dabScanLabels"), -1)));
    // ── gain ──
    r.set("gainLimits", J::str(strOr(S("gainLimits"), "")));
    r.set("gainLocks", J::str(strOr(S("gainLocks"), "")));
    r.set("gainSplits", J::str(strOr(S("gainSplits"), "")));
    r.set("gainCurves", J::str(strOr(S("gainCurves"), "")));
    r.set("ifBwLimits", J::str(strOr(S("ifBwLimits"), "")));
    r.set("restGain", J::num(numOr(S("restGain"), -1)));
    r.set("agcLock", J::num(boolOr(S("agcLock"), false) ? 1 : 0));
    r.set("rtlAgc", J::boolean(boolOr(S("rtlAgc"), false)));
    r.set("tunerBwAuto", J::boolean(boolOr(S("tunerBwAuto"), false)));
    r.set("allowRanges", J::str(strOr(S("allowRanges"), "")));
    r.set("blockRanges", J::str(strOr(S("blockRanges"), "")));
    // ── landings ──
    r.set("landingDabChannel", J::num(numOr(S("landingDabChannel"), -1)));
    r.set("landingDabSid", J::num(numOr(S("landingDabSid"), 0)));
    r.set("landingDabService", J::str(strOr(S("landingDabService"), "")));
    r.set("landingAdsb", J::boolean(boolOr(S("landingAdsb"), false)));
    r.set("adsbKeepRunning", J::boolean(boolOr(S("adsbKeepRunning"), false)));
    // ── Broadcast FM defaults (FM_DEFAULTS in services/fmDefaults.ts) ──
    r.set("fmDeemph", J::num(numOr(S("fmDeemph"), -1)));
    r.set("fmStereo", J::boolean(boolOr(S("fmStereo"), true)));
    r.set("fmNr", J::boolean(boolOr(S("fmNr"), false)));
    r.set("fmIms", J::boolean(boolOr(S("fmIms"), true)));
    r.set("fmCeq", J::boolean(boolOr(S("fmCeq"), true)));
    r.set("fmAutoBw", J::boolean(boolOr(S("fmAutoBw"), true)));
    r.set("fmNb", J::boolean(boolOr(S("fmNb"), true)));
    r.set("fmRemember", J::boolean(boolOr(S("fmRemember"), false)));
    // ── the aerial ──
    r.set("antennaIcon", J::str(strOr(S("antennaIcon"), "")));
    r.set("antennaRanges", J::str(strOr(S("antennaRanges"), "")));
    r.set("antennaFilters", J::str(strOr(S("antennaFilters"), "")));
    r.set("antennaNames", J::str(strOr(S("antennaNames"), "")));
    r.set("antennaSwitch", J::str(strOr(S("antennaSwitch"), "")));
    r.set("antennaMap", J::str(strOr(S("antennaMap"), "")));
    r.set("antennaPortLocked", J::boolean(boolOr(S("antennaLocked"), false)));
    // ── the front end ──
    r.set("ppm", J::num(numOr(S("ppm"), 0)));
    r.set("ppb", J::num(numOr(S("ppb"), 0)));
    r.set("biasT", J::boolean(boolOr(S("biasT"), false)));
    r.set("autoDirectSampling", J::boolean(boolOr(S("autoDirectSampling"), false)));
    r.set("directSamplingBelowHz", J::num(numOr(S("directSamplingBelowHz"), 24e6)));
    r.set("directSampling", J::num(numOr(S("directSampling"), 0)));
    r.set("converterOffsetHz", J::num(numOr(S("converterOffsetHz"), 0)));
    r.set("converterInputLoHz", J::num(numOr(S("converterInputLoHz"), 0)));
    r.set("converterInputHiHz", J::num(numOr(S("converterInputHiHz"), 0)));
    // ── an Icom's own (VibeIcom.prepareStart's defaults) ──
    r.set("icomBaud", J::num(numOr(S("icomBaud"), 115200)));
    r.set("icomCiv", J::num(numOr(S("icomCiv"), 0)));
    r.set("icomScopeSpeed", J::num(numOr(S("icomScopeSpeed"), 0)));
    r.set("icomTuneLock", J::boolean(boolOr(S("icomTuneLock"), false)));
    r.set("icomPowerOn", J::boolean(boolOr(S("icomPowerOn"), false)));
    r.set("icomAutoOff", J::boolean(boolOr(S("icomAutoOff"), true)));
    r.set("icomOffDelayMin", J::num(numOr(S("icomOffDelayMin"), 5)));
    // ── background decoding and the DX scanner, one JSON text each (RadioConfig::bgDecode / dxScan) ──
    r.set("bgDecode", J::str(strOr(S("bgDecode"), "")));
    r.set("dxScan", J::str(strOr(S("dxScan"), "")));
    r.set("adsbFeed", J::str(strOr(S("adsbFeed"), "")));   // ★★ V12 beta96
    // ── raw IQ out (the phone stores the LAN ceiling as text — see nativeServerConfig) ──
    r.set("rawIq", J::num(numOr(S("rawIq"), 0)));
    r.set("rawIqMax", J::num(numOr(S("rawIqMax"), 0)));
    r.set("rawIqLanMaxHz", J::num(numOr(S("rawIqLanMaxHz"), 0)));
    // ── the SDRplay card (rspCard.ts RSP_DEFAULTS) ──
    r.set("rspVibeAgc", J::num(numOr(S("rspVibeAgc"), 0)));
    for (const char* k : { "rfNotch", "dabNotch", "rspAmNotch", "rspHdr", "rspExtRef", "rspDabDecim", "rfAgc", "autoNotch", "agcSetLock" })
        r.set(k, J::boolean(boolOr(S(k), false)));
    r.set("userNotch", J::boolean(boolOr(S("userNotch"), true)));
    r.set("dabAgcOverride", J::boolean(boolOr(S("dabAgcOverride"), true)));
    r.set("dabAgcTarget", J::num(numOr(S("dabAgcTarget"), -40)));
    r.set("agcSet", J::num(numOr(S("agcSet"), -30)));
    J radios = J::arr(); radios.a.push_back(std::move(r));
    out.set("radios", std::move(radios));

    J android = J::obj();
    std::vector<std::string> adv;
    J sup = J::arr();
    for (const auto& id : supportedControls(pf, &adv)) sup.a.push_back(J::str(id));
    android.set("supported", std::move(sup));
    J advJ = J::arr();
    for (const auto& id : adv) advJ.a.push_back(J::str(id));
    android.set("advancedOnly", std::move(advJ));
    // ★ The phone's own three, for androidSrvCard.
    J srv = J::obj();
    srv.set("webServer", J::boolean(boolOr(S("webServer"), true)));
    srv.set("compressAudio", J::boolean(boolOr(S("compressAudio"), true)));
    srv.set("advanced", J::boolean(boolOr(S("advanced"), false)));
    android.set("server", std::move(srv));
    // ★★ The phone-only facts, shown READ-ONLY on the page with "change it on the phone" — the page can see them, never set
    //    them (docs/v12/ANDROID-SERVER-PROCESS.md, "Device-only").
    android.set("device", dev && dev->t == J::Obj ? *dev : J::obj());
    out.set("android", std::move(android));
    return dump(out);
}

/**
 * The page's save (`body`, the page's config shape) → a patch of Android keys (`patchOut`, a JSON object; a key whose
 * value is null is REMOVED from the settings). Only the controls offered for THIS phone and radio are read (`current` =
 * the same raw pageConfig() takes; it names the radio and the device). `restart` says whether the page asked for the
 * server to restart (its main Save does; "Save radio settings" does not). False + `err` on a bad body.
 * ★ Keys that are ACTIONS rather than settings travel in the patch too and are taken out by VibeRemoteSetup.kt:
 *   `publicListed` (list / unlist on vibesdr.net now).
 */
inline bool settingsPatch(const std::string& body, const std::string& current, std::string& patchOut, bool& restart, std::string& err) {
    J b;
    if (!parse(body, b) || b.t != J::Obj) { err = "the settings were not valid JSON"; return false; }
    J cur; if (!parse(current, cur) || cur.t != J::Obj) cur = J::obj();
    J noSettings = J::obj();
    const J* curS = cur.get("settings"); if (!curS || curS->t != J::Obj) curS = &noSettings;
    const Platform pf = platformOf(cur);
    auto on = [&](const char* id) { return offered(pf, id); };

    restart = boolOr(b.get("restart"), false);
    J p = J::obj();
    auto str = [&](const J* src, const char* pageKey, const char* key, size_t maxLen) {
        const J* v = src ? src->get(pageKey) : nullptr;
        if (v && v->t == J::Str) p.set(key, J::str(v->s.substr(0, maxLen)));
    };
    auto num = [&](const J* src, const char* pageKey, const char* key, double lo, double hi) {
        const J* v = src ? src->get(pageKey) : nullptr;
        if (!v || (v->t != J::Num && v->t != J::Str)) return;
        double x = numOr(v, NAN);
        if (!std::isfinite(x)) return;
        x = std::max(lo, std::min(hi, x));
        p.set(key, J::num(std::round(x)));
    };
    auto flag = [&](const J* src, const char* pageKey, const char* key) {
        const J* v = src ? src->get(pageKey) : nullptr;
        if (v && (v->t == J::Bool || v->t == J::Num)) p.set(key, J::boolean(boolOr(v, false)));
    };
    // ★ A JSON text carried as a string ("" = none): refused unless it is an object, so the server never boots on junk.
    auto jsonText = [&](const J* src, const char* pageKey, const char* key, size_t maxLen, const char* what) -> bool {
        const J* v = src ? src->get(pageKey) : nullptr;
        if (!v || v->t != J::Str) return true;
        if (v->s.empty()) { p.set(key, J::str("")); return true; }
        J chk;
        if (v->s.size() > maxLen || !parse(v->s, chk) || chk.t != J::Obj) { err = std::string(what) + " could not be read"; return false; }
        p.set(key, J::str(v->s));
        return true;
    };

    // ── the server tab ──
    if (const J* v = b.get("name"); v && v->t == J::Str) {
        std::string n = v->s.substr(0, 60);
        if (n.find_first_not_of(" \t") == std::string::npos) { err = "the server needs a name"; return false; }
        p.set("name", J::str(n));
    }
    flag(&b, "mdnsAdvertise", "advertise");
    str(&b, "landingMessage", "landingMessage", 500);
    str(&b, "landingLinkUrl", "landingLinkUrl", 300);
    str(&b, "landingLinkLabel", "landingLinkLabel", 60);
    num(&b, "uncompressed", "uncompressedAudio", 0, 2);
    str(&b, "trustedProxies", "trustedProxies", 400);
    if (const J* v = b.get("maxRadiosPerIp"); v && v->t == J::Num) p.set("oneRadioPerIp", J::boolean(v->n != 0));
    num(&b, "decoderMax", "decoderMax", 0, 64);
    num(&b, "rttySkimWorkers", "rttySkimWorkers", 0, 8);
    num(&b, "galleryMb", "galleryMb", 0, 100000);
    flag(&b, "galleryPublic", "galleryPublic");
    flag(&b, "gallerySaveLive", "gallerySaveLive");
    flag(&b, "communityMap", "communityMap");   // ★★ V12 beta94 the community map
    str(&b, "communityOff", "communityOff", 200);
    // ★★ V12 beta90 PSK Reporter / WSPRnet / the DMR ID lookup (the page refuses "on" without a callsign itself)
    flag(&b, "pskrOn", "pskrOn");
    str(&b, "pskrCall", "pskrCall", 20);   // ★ the receiver's NAME (vibe_spot_report.h kMaxReceiverName)
    str(&b, "pskrAntenna", "pskrAntenna", 64);
    flag(&b, "wsprnetOn", "wsprnetOn");
    str(&b, "wsprnetCall", "wsprnetCall", 15);   // ★ WSPRnet's own callsign (vibe_spot_report.h wsprCallOf)
    if (on("dmrIdLookup")) flag(&b, "dmrIdLookup", "dmrIdLookup");
    if (on("batteryPauseAt")) if (const J* v = b.get("batteryPauseAt"); v && v->t == J::Num) {
        double at = std::round(std::max(0.0, std::min(95.0, v->n)));
        p.set("batteryPauseAt", J::num(at));
        // ★ The phone's own rule (ServerModeScreen: resume 20 points above the pause, 40 when there is none) — one truth.
        p.set("batteryResumeAt", J::num(at > 0 ? at + 20 : 40));
    }
    // ★ RECEIVER LOCATION — a place needs a position (the page finds one for a typed place; the phone's screen does too).
    if (const J* la = b.get("lat"), *lo = b.get("lon"); la && lo && la->t == J::Str && lo->t == J::Str) {
        J loc = J::obj();
        const J* fol = b.get("locFollow");
        loc.set("moving", J::boolean(fol && boolOr(fol, false)));
        if (const J* c = b.get("country"); c && c->t == J::Str) {
            std::string cc;
            for (char ch : c->s.substr(0, 3)) if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) cc += char(ch & ~0x20);
            loc.set("country", J::str(cc));
        }
        const bool blank = la->s.find_first_not_of(" ") == std::string::npos && lo->s.find_first_not_of(" ") == std::string::npos;
        if (blank) {
            loc.set("mode", J::str("off"));
        } else {
            const double lat = numOr(la, NAN), lon = numOr(lo, NAN);
            if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) {
                err = "the receiver's location needs a latitude (-90 to 90) and a longitude (-180 to 180)"; return false;
            }
            loc.set("mode", J::str("manual"));
            loc.set("lat", J::num(std::round(lat * 1e5) / 1e5)); loc.set("lon", J::num(std::round(lon * 1e5) / 1e5));
            const J* pl = b.get("place");
            std::string label = pl && pl->t == J::Str ? pl->s.substr(0, 80) : "";
            if (label.find_first_not_of(" ") == std::string::npos) label = gridOf(lat, lon);
            loc.set("label", J::str(label));
        }
        p.set("serverLocation", std::move(loc));
    }
    if (on("dirList")) {
        str(&b, "dirName", "publicName", 48);
        num(&b, "dirShareSec", "publicShareSec", 0, 366.0 * 86400);
        flag(&b, "dirList", "publicListed");   // ★ an ACTION — VibeRemoteSetup lists or unlists, and does not store it
    }
    // ── the phone's own three (androidSrvCard) ──
    flag(&b, "webServer", "webServer");
    flag(&b, "compressAudio", "compressAudio");
    flag(&b, "advanced", "advanced");
    // ★ The screen's rule: no web client, no Advanced (the admin page IS the web client).
    if (const J* w = p.get("webServer"); w && !w->b) p.set("advanced", J::boolean(false));

    // ── the radio tab — the phone has exactly one ──
    const J* radios = b.get("radios");
    const J* r = (radios && radios->t == J::Arr && !radios->a.empty() && radios->a[0].t == J::Obj) ? &radios->a[0] : nullptr;
    if (r) {
        if (const J* v = r->get("label"); v && v->t == J::Str) p.set("radioLabel", J::str(v->s.substr(0, 60)));
        if (const J* v = r->get("antenna"); v && v->t == J::Str) p.set("antenna", J::str(v->s.substr(0, 120)));
        num(r, "users", "maxUsers", 1, 50);
        flag(r, "askBeforeTuning", "askBeforeTuning");
        if (on("zoomSpectrum")) flag(r, "zoomSpectrum", "zoomSpectrum");
        if (on("landingFreq")) if (const J* v = r->get("landingFreq"); v && v->t == J::Num) {
            if (v->n > 0) {
                p.set("centerFreq", J::num(std::round(v->n)));
                const J* m = r->get("demodMode");
                const std::string mode = (m && m->t == J::Str) ? m->s : "";
                if (!mode.empty()) {
                    if (!knownMode(mode)) { err = "unknown demodulator \"" + mode.substr(0, 12) + "\""; return false; }
                    p.set("mode", J::str(mode));
                }
            } else {
                // ★ 0 = "where the server keeps it" on the phone: absent, never a frequency of 0 Hz.
                p.set("centerFreq", J::null());
                p.set("mode", J::null());
            }
        }
        num(r, "sessionLimitMin", "sessionLimitMin", 0, 24 * 60);
        flag(r, "sessionLimitSoft", "sessionLimitSoft");
        num(r, "idleKickMin", "idleKickMin", 0, 24 * 60);
        if (on("nbWide")) num(r, "nbWide", "nbWide", 0, 2);
        if (on("vfoB")) num(r, "vfoB", "vfoB", 0, 1);
        num(r, "idleGrace", "idleGraceSec", 0, 24 * 3600);
        // ★ Sent only when typed or deliberately cleared (collectRadio) — an absent key leaves the PIN alone.
        if (const J* v = r->get("pin"); v && v->t == J::Str) {
            std::string d;
            for (char c : v->s) if (c >= '0' && c <= '9') d += c;
            // ★ The phone's own rule (ServerModeScreen: digits only, up to 12) — the page must not set one the phone refuses.
            if (!v->s.empty() && (d.empty() || d.size() > 12)) { err = "a PIN is digits only, up to 12 of them"; return false; }
            p.set("pin", J::str(d));
        }

        // ── how it is shared ──
        if (on("modeLocked")) {
            if (const J* v = r->get("mode"); v && v->t == J::Str) p.set("radioUse", J::str(v->s == "locked" ? "locked" : "single"));
            // ★ 0 is what the page sends for a radio that is not locked — never a real centre; the phone keeps its own.
            if (const J* v = r->get("lockFreq"); v && v->t == J::Num && v->n > 0) p.set("lockedCentre", J::num(std::round(std::min(v->n, 7e9))));
        }
        if (on("spectrogram")) flag(r, "spectrogram", "spectrogram");
        // ── the capture: the page's rate + its pin box → the phone's "0 = the listener's choice, else pinned" ──
        if (on("rate")) {
            const J* rv = r->get("rate"); const J* lk = r->get("rateLock");
            if (rv && rv->t == J::Num && rv->n > 0) {
                const bool pin = lk && boolOr(lk, false);
                const double hz = snapTo(ratesFor(pf.driver), rv->n);
                p.set("lockedRate", J::num(pin ? hz : 0));
                p.set("sampleRate", J::num(pin ? hz : 2400000));   // ★ the screen's `rate || 2_400_000`
                p.set("rateLock", J::boolean(pin));
            }
            if (const J* v = r->get("maxFps"); v && v->t == J::Num) p.set("maxFftRate", J::num(snapTo({ 5, 10, 20 }, v->n)));
        }
        if (on("modeBlockList")) str(r, "blockedModes", "blockedModes", 400);
        if (on("dabRateBoost")) flag(r, "dabRateBoost", "dabRateBoost");
        if (on("adsb256")) { flag(r, "adsb256", "adsb256"); flag(r, "ism256", "ism256"); }
        if (on("dabScanLabels")) num(r, "dabScanLabels", "dabScanLabels", -1, 1);
        // ── gain ──
        if (on("gainRest")) {
            num(r, "restGain", "restGain", -1, 500);
            flag(r, "rtlAgc", "rtlAgc");
            flag(r, "tunerBwAuto", "tunerBwAuto");
        }
        if (on("gainAgcLock")) flag(r, "agcLock", "agcLock");
        if (on("gainMax")) {
            str(r, "gainLimits", "gainLimits", 600); str(r, "gainLocks", "gainLocks", 600);
            str(r, "gainSplits", "gainSplits", 600); str(r, "gainCurves", "gainCurves", 600);
        }
        if (on("gainIfBw")) str(r, "ifBwLimits", "ifBwLimits", 600);
        if (on("allowPick")) { str(r, "allowRanges", "allowRanges", 600); str(r, "blockRanges", "blockRanges", 600); }
        // ── landings ──
        if (on("dabLandOn")) {
            const J* bm = p.get("blockedModes"); if (!bm) bm = curS->get("blockedModes");
            std::string bms = bm && bm->t == J::Str ? bm->s : "";
            for (auto& c : bms) c = char(std::tolower((unsigned char)c));
            bool dabBlocked = false;
            { std::string tok; for (size_t i = 0; i <= bms.size(); ++i) {
                const char c = i < bms.size() ? bms[i] : ',';
                if (c == ',' || c == ';' || c == ' ' || c == '\t') { if (tok == "dab") dabBlocked = true; tok.clear(); } else tok += c; } }
            if (const J* ch = r->get("landingDabChannel"); ch && ch->t == J::Num) {
                // ★★★ never a DAB landing beside a DAB block (the screen and the engine both refuse it)
                const bool none = ch->n < 0 || dabBlocked;
                p.set("landingDabChannel", J::num(none ? -1 : std::round(std::min(ch->n, 255.0))));
                p.set("landingDabSid", J::num(none ? 0 : std::max(0.0, std::min(4294967295.0, numOr(r->get("landingDabSid"), 0)))));
                p.set("landingDabService", J::str(none ? "" : strOr(r->get("landingDabService"), "").substr(0, 64)));
            }
            flag(r, "landingAdsb", "landingAdsb");
            if (const J* la = r->get("landingAdsb"); la && (la->t == J::Bool || la->t == J::Num))
                p.set("adsbKeepRunning", J::boolean(boolOr(la, false) && boolOr(r->get("adsbKeepRunning"), false)));
        }
        if (on("fmDeemph")) {
            if (const J* v = r->get("fmDeemph"); v && v->t == J::Num) {
                const int d = int(std::lround(v->n));
                p.set("fmDeemph", J::num(d == 0 || d == 50 || d == 75 ? d : -1));
            }
            for (const char* k : { "fmStereo", "fmNr", "fmIms", "fmCeq", "fmAutoBw", "fmNb", "fmRemember" }) flag(r, k, k);
        }
        // ── the aerial ──
        if (on("antRangePreset")) {
            if (const J* v = r->get("antennaIcon"); v && v->t == J::Str) {
                std::string ic; for (char c : v->s.substr(0, 40)) if (std::isalnum((unsigned char)c) || c == '-' || c == '_') ic += c;
                p.set("antennaIcon", J::str(ic));
            }
            str(r, "antennaRanges", "antennaRanges", 600); str(r, "antennaFilters", "antennaFilters", 600);
            str(r, "antennaNames", "antennaNames", 400);
        }
        if (on("swYes")) {
            if (!jsonText(r, "antennaSwitch", "antennaSwitch", 4000, "the antenna switch")) return false;
            // ★ With a switch, the per-band list is the switch's; without one, only an RSP has a per-band socket list.
            const J* sw = p.get("antennaSwitch");
            const bool hasSwitch = sw && !sw->s.empty();
            if (hasSwitch || on("antennaMap")) {
                str(r, "antennaMap", "antennaMap", 300);
                flag(r, "antennaPortLocked", "antennaLocked");
            }
        }
        // ── the front end ──
        if (on("ppm")) if (const J* v = r->get("ppm"); v && v->t == J::Num) {
            // ★ The phone sends ppm only when the owner gave one (absent = leave the radio alone); the page shows that as 0.
            const double x = std::round(std::max(-500.0, std::min(500.0, v->n)));
            if (x != 0 || curS->get("ppm")) p.set("ppm", J::num(x));
        }
        if (on("ppb")) num(r, "ppb", "ppb", -100000, 100000);
        if (on("biasT")) flag(r, "biasT", "biasT");
        if (on("autoDs")) {
            flag(r, "autoDirectSampling", "autoDirectSampling");
            num(r, "directSamplingBelowHz", "directSamplingBelowHz", 500000, 60e6);
            num(r, "directSampling", "directSampling", 0, 2);
        }
        if (on("convLo")) {
            num(r, "converterOffsetHz", "converterOffsetHz", -2e10, 2e10);
            num(r, "converterInputHiHz", "converterInputHiHz", 0, 2e10);
            // ★ The Linux page has no input-low box and always posts 0; the phone's comes from androidConvInLo, posted
            //   only on a phone (collectRadio) — and a 0 from an older page must not wipe the phone's.
            if (r->get("androidPage") || b.get("androidPage")) num(r, "converterInputLoHz", "converterInputLoHz", 0, 2e10);
        }
        // ── an Icom's own ──
        if (on("icomBaud")) {
            if (const J* v = r->get("icomBaud"); v && v->t == J::Num)
                p.set("icomBaud", J::num(snapTo({ 4800, 9600, 19200, 38400, 57600, 115200 }, v->n)));
            num(r, "icomCiv", "icomCiv", 0, 255);
            num(r, "icomScopeSpeed", "icomScopeSpeed", 0, 2);
            flag(r, "icomTuneLock", "icomTuneLock"); flag(r, "icomPowerOn", "icomPowerOn"); flag(r, "icomAutoOff", "icomAutoOff");
            if (const J* v = r->get("icomOffDelayMin"); v && v->t == J::Num)
                p.set("icomOffDelayMin", J::num(snapTo({ 0, 1, 5, 10, 15, 30, 60 }, v->n)));
        }
        // ── background decoding and the DX scanner ──
        if (on("bgOn") && !jsonText(r, "bgDecode", "bgDecode", 16000, "the background decoding profiles")) return false;
        if (on("dxOn") && !jsonText(r, "dxScan", "dxScan", 4000, "the DX scanner's settings")) return false;
        if (on("afServe") && !jsonText(r, "adsbFeed", "adsbFeed", 4000, "the flight-tracker sharing settings")) return false;   // ★★ beta96
        // ── raw IQ out ──
        if (on("rawIqOn")) {
            num(r, "rawIq", "rawIq", 0, 2);
            num(r, "rawIqMax", "rawIqMax", 0, 16);
            // ★ As TEXT, the way VibeServerBoot reads it (nativeServerConfig: "250000.0" would parse to nothing).
            if (const J* v = r->get("rawIqLanMaxHz"); v && v->t == J::Num) {
                char b2[24]; std::snprintf(b2, sizeof b2, "%.0f", std::max(0.0, std::min(1e8, std::round(v->n))));
                p.set("rawIqLanMaxHz", J::str(b2));
            }
        }
        // ── the SDRplay card ──
        if (on("rspVibeAgc")) {
            num(r, "rspVibeAgc", "rspVibeAgc", 0, 2);
            for (const char* k : { "rfNotch", "dabNotch", "rspAmNotch", "rspHdr", "rspExtRef", "rspDabDecim", "rfAgc",
                                   "autoNotch", "userNotch", "dabAgcOverride", "agcSetLock" }) flag(r, k, k);
            num(r, "dabAgcTarget", "dabAgcTarget", -60, -10);
            num(r, "agcSet", "agcSet", -72, -10);
        }
    }
    patchOut = dump(p);
    return true;
}
/** The first cut's signature (no current settings): the radio is then taken as an RTL-SDR, the screen's own default. */
inline bool settingsPatch(const std::string& body, std::string& patchOut, bool& restart, std::string& err) {
    return settingsPatch(body, std::string(), patchOut, restart, err);
}

}  // namespace androidsetup
}  // namespace vibe
