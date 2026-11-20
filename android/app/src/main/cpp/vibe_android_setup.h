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

/** ★★ THE CONTROLS THE PAGE KEEPS ON A PHONE — every other control is hidden by the page (androidPrune). Each id here has
 *  its mapping in BOTH functions below and its mirror on the phone's screen (ServerModeScreen, remoteKeys); adding one
 *  means all three, and test-android-setup.cpp checks the two here agree. */
inline const std::vector<std::string>& supportedControls() {
    static const std::vector<std::string> ids = {
        // the server tab
        "name", "mdns", "landingMessage", "landingLinkUrl", "landingLinkLabel", "uncompressed", "trustedProxies",
        "maxRadiosPerIp", "decoderMax", "galleryMb", "galleryPublic", "gallerySaveLive", "batteryPauseAt",
        // the radio tab
        "radioName", "antenna", "users", "askFirst", "zoomSpectrum", "landingFreq", "demodMode", "sessionLimit",
        "sessionLimitMode", "idleKick", "nbWide", "vfoB", "idleGrace", "radioPin", "radioPinClear",
    };
    return ids;
}

/** The demodulator names the phone's server lands on — the page's own list is wider (it serves every radio). */
inline bool knownMode(const std::string& m) {
    static const std::set<std::string> ok = { "am", "sam", "nfm", "wfm", "usb", "lsb", "cw", "cwu", "cwl", "dsb", "drm", "dab", "iq", "raw" };
    return ok.count(m) > 0;
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
    out.set("galleryMb", J::num(numOr(S("galleryMb"), 100)));
    out.set("galleryPublic", J::boolean(boolOr(S("galleryPublic"), true)));
    out.set("gallerySaveLive", J::boolean(boolOr(S("gallerySaveLive"), true)));
    out.set("batteryPauseAt", J::num(numOr(S("batteryPauseAt"), 0)));
    out.set("batteryResumeAt", J::num(numOr(S("batteryResumeAt"), 40)));
    out.set("port", J::num(0));

    J r = J::obj();
    r.set("serial", J::str("android"));
    r.set("enabled", J::boolean(true));
    r.set("configured", J::boolean(true));
    r.set("driver", J::str(rad ? strOr(rad->get("driver"), "") : ""));
    r.set("model", J::str(rad ? strOr(rad->get("model"), "") : ""));
    r.set("mode", J::str("shared"));
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
    J radios = J::arr(); radios.a.push_back(std::move(r));
    out.set("radios", std::move(radios));

    J android = J::obj();
    J sup = J::arr();
    for (const auto& id : supportedControls()) sup.a.push_back(J::str(id));
    android.set("supported", std::move(sup));
    // ★★ The phone-only facts, shown READ-ONLY on the page with "change it on the phone" — the page can see them, never set
    //    them: battery optimisation, start when power returns and the USB grant are Android's own dialogs.
    android.set("device", dev && dev->t == J::Obj ? *dev : J::obj());
    out.set("android", std::move(android));
    return dump(out);
}

/**
 * The page's save (`body`, the page's config shape) → a patch of Android keys (`patchOut`, a JSON object; a key whose
 * value is null is REMOVED from the settings). Only the supported controls are read. `restart` says whether the page
 * asked for the server to restart (its main Save does; "Save radio settings" does not). False + `err` on a bad body.
 */
inline bool settingsPatch(const std::string& body, std::string& patchOut, bool& restart, std::string& err) {
    J b;
    if (!parse(body, b) || b.t != J::Obj) { err = "the settings were not valid JSON"; return false; }
    restart = boolOr(b.get("restart"), false);
    J p = J::obj();
    auto str = [&](const char* pageKey, const char* key, size_t maxLen) {
        const J* v = b.get(pageKey);
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

    // the server tab
    if (const J* v = b.get("name"); v && v->t == J::Str) {
        std::string n = v->s.substr(0, 60);
        if (n.find_first_not_of(" \t") == std::string::npos) { err = "the server needs a name"; return false; }
        p.set("name", J::str(n));
    }
    flag(&b, "mdnsAdvertise", "advertise");
    str("landingMessage", "landingMessage", 500);
    str("landingLinkUrl", "landingLinkUrl", 300);
    str("landingLinkLabel", "landingLinkLabel", 60);
    num(&b, "uncompressed", "uncompressedAudio", 0, 2);
    str("trustedProxies", "trustedProxies", 400);
    if (const J* v = b.get("maxRadiosPerIp"); v && v->t == J::Num) p.set("oneRadioPerIp", J::boolean(v->n != 0));
    num(&b, "decoderMax", "decoderMax", 0, 64);
    num(&b, "galleryMb", "galleryMb", 0, 100000);
    flag(&b, "galleryPublic", "galleryPublic");
    flag(&b, "gallerySaveLive", "gallerySaveLive");
    if (const J* v = b.get("batteryPauseAt"); v && v->t == J::Num) {
        double at = std::round(std::max(0.0, std::min(95.0, v->n)));
        p.set("batteryPauseAt", J::num(at));
        // ★ The phone's own rule (ServerModeScreen: resume 20 points above the pause, 40 when there is none) — one truth.
        p.set("batteryResumeAt", J::num(at > 0 ? at + 20 : 40));
    }

    // the radio tab — the phone has exactly one
    const J* radios = b.get("radios");
    const J* r = (radios && radios->t == J::Arr && !radios->a.empty() && radios->a[0].t == J::Obj) ? &radios->a[0] : nullptr;
    if (r) {
        if (const J* v = r->get("label"); v && v->t == J::Str) p.set("radioLabel", J::str(v->s.substr(0, 60)));
        if (const J* v = r->get("antenna"); v && v->t == J::Str) p.set("antenna", J::str(v->s.substr(0, 120)));
        num(r, "users", "maxUsers", 1, 50);
        flag(r, "askBeforeTuning", "askBeforeTuning");
        flag(r, "zoomSpectrum", "zoomSpectrum");
        if (const J* v = r->get("landingFreq"); v && v->t == J::Num) {
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
        num(r, "nbWide", "nbWide", 0, 2);
        num(r, "vfoB", "vfoB", 0, 1);
        num(r, "idleGrace", "idleGraceSec", 0, 24 * 3600);
        // ★ Sent only when typed or deliberately cleared (collectRadio) — an absent key leaves the PIN alone.
        if (const J* v = r->get("pin"); v && v->t == J::Str) {
            std::string d;
            for (char c : v->s) if (c >= '0' && c <= '9') d += c;
            // ★ The phone's own rule (ServerModeScreen: digits only, up to 12) — the page must not set one the phone refuses.
            if (!v->s.empty() && (d.empty() || d.size() > 12)) { err = "a PIN is digits only, up to 12 of them"; return false; }
            p.set("pin", J::str(d));
        }
    }
    patchOut = dump(p);
    return true;
}

}  // namespace androidsetup
}  // namespace vibe
