#include "vibeserver_config.h"
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace vsconfig {
namespace {

// ── A small, dependency-free JSON reader for a FLAT object ────────────────────
// ★ Deliberately not a general parser. The config schema is one flat object of strings, numbers,
//   booleans and one array of strings, and keeping the reader to exactly that is what makes it
//   small enough to be obviously correct. If the schema ever needs nesting, take a real parser —
//   do not grow this one.

/** Find `"key"` at the top level and return the index just past its colon, or npos. */
size_t findKey(const std::string& s, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t i = 0;
    while ((i = s.find(pat, i)) != std::string::npos) {
        size_t j = i + pat.size();
        while (j < s.size() && isspace((unsigned char)s[j])) j++;
        if (j < s.size() && s[j] == ':') return j + 1;
        i = j;
    }
    return std::string::npos;
}

std::string unescape(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
        switch (in[++i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            default: out += in[i];
        }
    }
    return out;
}

bool getStr(const std::string& s, const std::string& key, std::string& out) {
    size_t p = findKey(s, key);
    if (p == std::string::npos) return false;
    while (p < s.size() && isspace((unsigned char)s[p])) p++;
    if (p >= s.size() || s[p] != '"') return false;
    std::string raw;
    for (size_t i = p + 1; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { raw += s[i]; raw += s[i+1]; i++; continue; }
        if (s[i] == '"') { out = unescape(raw); return true; }
        raw += s[i];
    }
    return false;
}

bool getNum(const std::string& s, const std::string& key, double& out) {
    size_t p = findKey(s, key);
    if (p == std::string::npos) return false;
    while (p < s.size() && isspace((unsigned char)s[p])) p++;
    char* end = nullptr;
    const double v = strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    out = v;
    return true;
}

bool getBool(const std::string& s, const std::string& key, bool& out) {
    size_t p = findKey(s, key);
    if (p == std::string::npos) return false;
    while (p < s.size() && isspace((unsigned char)s[p])) p++;
    if (s.compare(p, 4, "true") == 0)  { out = true;  return true; }
    if (s.compare(p, 5, "false") == 0) { out = false; return true; }
    return false;
}

/** ★★★ SPLIT A JSON ARRAY OF OBJECTS. The rest of this file is a FLAT key scanner — it searches
 *  the whole document for `"key"` and reads what follows — which is why it never needed nesting.
 *  A `radios` array does, and getting this wrong is not a parse error, it is one radio reading
 *  another radio's settings.
 *
 *  ★★ IT TRACKS STRINGS AND ESCAPES, not just braces. A label of `Loft { HF }` or a Windows-style
 *     path in any future field would otherwise unbalance the count and silently split one radio's
 *     object in half — and the flat getters would then happily answer from the wrong fragment
 *     rather than fail. Depth counting alone looks correct and is not.
 *
 *  Returns false only when the key is absent or the array is malformed; an EMPTY array is a
 *  perfectly good answer and means "this machine serves no radios". */
bool getObjectArray(const std::string& s, const std::string& key,
                    std::vector<std::string>& out) {
    size_t p = findKey(s, key);
    if (p == std::string::npos) return false;
    while (p < s.size() && isspace((unsigned char)s[p])) p++;
    if (p >= s.size() || s[p] != '[') return false;

    out.clear();
    int depth = 0;
    bool inStr = false, esc = false;
    size_t start = 0;
    for (size_t i = p + 1; i < s.size(); i++) {
        const char c = s[i];
        if (inStr) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') { if (depth++ == 0) start = i; continue; }
        if (c == '}') {
            if (--depth < 0) return false;              // a closing brace with nothing open
            if (depth == 0) out.push_back(s.substr(start, i - start + 1));
            continue;
        }
        if (c == ']' && depth == 0) return true;        // the array ended cleanly
    }
    return false;                                        // ran off the end: unterminated
}

std::string esc(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

std::string num(double v) {
    char b[64];
    // Integers print as integers — a config a human may open should not be full of ".000000".
    if (v == (long long)v && v < 1e15 && v > -1e15) snprintf(b, sizeof b, "%lld", (long long)v);
    else snprintf(b, sizeof b, "%.6f", v);
    return b;
}

} // namespace

std::string mdnsLabel(const std::string& friendly) {
    std::string out;
    bool lastDash = false;
    for (unsigned char c : friendly) {
        if (isalnum(c)) { out += (char)tolower(c); lastDash = false; }
        else if (!out.empty() && !lastDash) { out += '-'; lastDash = true; }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "vibeserver";
    if (out.size() > 63) out.resize(63);          // one DNS label
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

const char* defaultPath() { return "/etc/vibeserver/config.json"; }

std::string toJson(const Config& c) {
    std::string j = "{\n";
    auto S = [&](const char* k, const std::string& v, bool last = false) {
        j += "  \"" + std::string(k) + "\": \"" + esc(v) + "\"" + (last ? "\n" : ",\n"); };
    auto N = [&](const char* k, double v) {
        j += "  \"" + std::string(k) + "\": " + num(v) + ",\n"; };
    auto B = [&](const char* k, bool v) {
        j += "  \"" + std::string(k) + "\": " + (v ? "true" : "false") + ",\n"; };

    B("configured", c.configured);
    S("mode", c.mode == Mode::LockedRange ? "locked" : "single");
    S("sharing", c.sharing == Sharing::Public ? "public" : "local");
    S("name", c.name); S("place", c.place); S("country", c.country);
    S("locator", c.locator); S("lat", c.lat); S("lon", c.lon);
    B("mdnsAdvertise", c.mdnsAdvertise);
    S("mdnsName", c.mdnsName);
    S("pin", c.pin); S("adminPass", c.adminPass);
    N("sessionLimitMin", c.sessionLimitMin);
    N("adminIdleMin", c.adminIdleMin);
    N("updateSrvHour", c.updateSrvHour);
    N("updateSrvDay",  c.updateSrvDay);
    N("updateAllHour", c.updateAllHour);
    N("updateAllDay",  c.updateAllDay);
    N("freq", c.freq); N("rate", c.rate); N("lockFreq", c.lockFreq); N("lockRate", c.lockRate);
    N("gain", c.gain);
    N("lnaState", c.lnaState);
    N("ifGr", c.ifGr);
    N("ifAgc", c.ifAgc);
    S("demodMode", c.demodMode);
    N("landingFreq", c.landingFreq);
    N("users", c.users); N("maxBw", c.maxBw); N("maxFps", c.maxFps); N("fftRate", c.fftRate);
    N("uncompressed", c.uncompressed);
    B("forceIdleSaver", c.forceIdleSaver);
    B("releaseWhenIdle", c.releaseWhenIdle);
    N("idleGrace", c.idleGrace);
    B("rfNotch", c.rfNotch); B("dabNotch", c.dabNotch); B("zoomSpectrum", c.zoomSpectrum);
    S("cpuGovernor", c.cpuGovernor);
    S("trustedProxies", c.trustedProxies);
    N("port", c.port);
    j += "  \"web\": " + std::string(c.web ? "true" : "false") + "\n}\n";
    return j;
}

bool fromJson(const std::string& s, Config& c, std::string& err, bool validate) {
    if (s.find('{') == std::string::npos) { err = "not a JSON object"; return false; }
    double d;
    std::string t;
    // ★★ COUNT WHAT WE ACTUALLY RECOGNISED. A file containing a stray "{" parses as an EMPTY
    //    config: every getter quietly fails, validation passes, and the server starts on defaults
    //    having silently discarded every setting the owner wrote. That is the worst failure this
    //    file can have — not a refusal, which is visible, but a silent reversion nobody notices
    //    until they wonder why the receiver is on 9.41 MHz. So: if a non-empty file yielded
    //    nothing we understood, say so.
    size_t seen = 0;
    auto S2 = [&](const char* k, std::string& dst){ if (getStr(s, k, dst)) seen++; };
    auto B2 = [&](const char* k, bool& dst){ if (getBool(s, k, dst)) seen++; };
    auto N2 = [&](const char* k, double& dst){ if (getNum(s, k, dst)) { seen++; } };
    (void)S2; (void)B2; (void)N2;
    if (getBool(s, "configured", c.configured)) seen++;
    if (getStr(s, "mode", t)) c.mode = (t == "locked") ? Mode::LockedRange : Mode::SingleUser;
    // ★ Anything that is not exactly "public" reads as Local, including a missing field — so an
    //   existing config from before this setting behaves exactly as it did.
    if (getStr(s, "sharing", t)) c.sharing = (t == "public") ? Sharing::Public : Sharing::Local;
    getStr(s, "name", c.name);     getStr(s, "place", c.place);
    getStr(s, "country", c.country); getStr(s, "locator", c.locator);
    getStr(s, "lat", c.lat);       getStr(s, "lon", c.lon);
    getBool(s, "mdnsAdvertise", c.mdnsAdvertise);
    getStr(s, "mdnsName", c.mdnsName);
    getStr(s, "pin", c.pin);       getStr(s, "adminPass", c.adminPass);
    if (getNum(s, "sessionLimitMin", d)) c.sessionLimitMin = (int)d;
    if (getNum(s, "adminIdleMin", d)) c.adminIdleMin = (int)d;
    if (getNum(s, "updateSrvHour", d)) c.updateSrvHour = (int)d;
    if (getNum(s, "updateSrvDay",  d)) c.updateSrvDay  = (int)d;
    if (getNum(s, "updateAllHour", d)) c.updateAllHour = (int)d;
    if (getNum(s, "updateAllDay",  d)) c.updateAllDay  = (int)d;
    // ★ Read the SHORT-LIVED earlier shape too (one schedule + an updateAll flag). It only ever
    //   reached one machine, but a config we wrote is a config we must keep reading — silently
    //   dropping a setting the owner made is the worst kind of upgrade.
    {
        double h = -1, dd = -1; bool all = false;
        if (getNum(s, "updateHour", h)) {
            getNum(s, "updateDay", dd);
            getBool(s, "updateAll", all);
            if (all) { c.updateAllHour = (int)h; c.updateAllDay = (int)dd; }
            else     { c.updateSrvHour = (int)h; c.updateSrvDay = (int)dd; }
        }
    }
    if (getNum(s, "freq", d))        c.freq = d;
    if (getNum(s, "rate", d))        c.rate = d;
    if (getNum(s, "lockFreq", d))    c.lockFreq = d;
    if (getNum(s, "lockRate", d))    c.lockRate = d;
    if (getNum(s, "gain", d))        c.gain = (int)d;
    if (getNum(s, "lnaState", d))    c.lnaState = (int)d;
    if (getNum(s, "ifGr", d))        c.ifGr = (int)d;
    if (getNum(s, "ifAgc", d))       c.ifAgc = (int)d;
    getStr(s, "demodMode", c.demodMode);
    if (getNum(s, "landingFreq", d)) c.landingFreq = d;
    if (getNum(s, "users", d))       c.users = (int)d;
    if (getNum(s, "maxBw", d))       c.maxBw = d;
    if (getNum(s, "maxFps", d))      c.maxFps = d;
    if (getNum(s, "fftRate", d))     c.fftRate = d;
    if (getNum(s, "uncompressed", d)) c.uncompressed = (int)d;
    getBool(s, "forceIdleSaver", c.forceIdleSaver);
    getBool(s, "releaseWhenIdle", c.releaseWhenIdle);
    if (getNum(s, "idleGrace", d))   c.idleGrace = d;
    getBool(s, "rfNotch", c.rfNotch);
    getBool(s, "dabNotch", c.dabNotch);
    getBool(s, "zoomSpectrum", c.zoomSpectrum);
    getStr(s, "cpuGovernor", c.cpuGovernor);
    getStr(s, "trustedProxies", c.trustedProxies);
    if (getNum(s, "uncompressed", d)) c.uncompressed = (int)d;
    if (getNum(s, "port", d))        c.port = (int)d;
    getBool(s, "web", c.web);

    // ★★ VALIDATE WHAT THE SERVER CANNOT SURVIVE, and only that. A config that would refuse to
    //    start is worse than one with an odd value in it, so the rule is: clamp the recoverable,
    //    reject only the contradictory.
    if (c.users < 1) c.users = 1;
    if (!validate) return true;      // a live patch — see the note in the header
    if (c.users > 1 && c.lockFreq <= 0) {
        err = "multi-user needs a locked centre frequency (lockFreq)";
        return false;
    }
    if (c.configured && c.adminPass.empty()) {
        err = "a configured server must have an admin password";
        return false;
    }
    // Did anything at all land? Count the fields most likely to be present in a real config.
    if (!c.name.empty() || !c.adminPass.empty() || c.lockFreq > 0 || c.users > 1
        || !c.pin.empty() || c.port != 0) seen++;
    if (seen == 0 && s.find_first_not_of(" \t\r\n{}") != std::string::npos) {
        err = "no settings recognised — is the JSON valid?";
        return false;
    }
    return true;
}

bool load(const std::string& path, Config& cfg, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;                       // absent is not an error: it means "not set up"
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    Config parsed = cfg;                        // start from defaults, apply the file over them
    if (!fromJson(s, parsed, err)) return false;
    cfg = parsed;                               // ★ all-or-nothing: never half-apply a bad file
    return true;
}

bool save(const std::string& path, const Config& cfg, std::string& err) {
    // ★★★ ATOMIC OR NOT AT ALL. This file is read at every boot, so a torn write is a receiver
    //     that will not start — and it would be written by a browser click, i.e. by someone who
    //     is not at the machine and cannot fix it. Temp file in the SAME directory (rename is
    //     only atomic within a filesystem), fsync, then rename.
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = "cannot write " + tmp + ": " + strerror(errno); return false; }
    const std::string j = toJson(cfg);
    if (fwrite(j.data(), 1, j.size(), f) != j.size()) {
        err = "short write to " + tmp; fclose(f); unlink(tmp.c_str()); return false;
    }
    fflush(f);
    if (fsync(fileno(f)) != 0) { err = "fsync failed"; fclose(f); unlink(tmp.c_str()); return false; }
    // ★★★ 0600 BEFORE THE RENAME, NOT AFTER. This file contains the ADMIN PASSWORD and the PIN in
    //     clear, and on a public receiver it is the one file that must not be world-readable.
    //     Setting the mode after the rename would leave a window where it is not.
    fchmod(fileno(f), 0600);
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        err = std::string("rename failed: ") + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// ── Several radios on one machine ────────────────────────────────────────────────────────────

namespace {

/** One radio's object. Shares the flat getters with everything else — each entry IS flat. */
void radioFromJson(const std::string& j, RadioConfig& r) {
    double n = 0; bool b = false; std::string t;
    auto S = [&](const char* k, std::string& dst) { if (getStr(j, k, t)) dst = t; };
    auto N = [&](const char* k, double& dst)      { if (getNum(j, k, n)) dst = n; };
    auto I = [&](const char* k, int& dst)         { if (getNum(j, k, n)) dst = (int)n; };
    auto B = [&](const char* k, bool& dst)        { if (getBool(j, k, b)) dst = b; };

    S("serial", r.serial); S("driver", r.driver); S("usbPath", r.usbPath); S("label", r.label);
    B("enabled", r.enabled); B("configured", r.configured);
    I("port", r.port);
    // ★ SAME VOCABULARY AS THE TOP-LEVEL FIELD — "locked"/"single", not a second spelling.
    //   Mode is about the radio's WINDOW (does the owner pin it); how many people may listen is
    //   `users`. Stuart's RSP is locked + users 30; the RTL and Airspy are single + users 1.
    if (getStr(j, "mode", t)) r.mode = (t == "locked") ? Mode::LockedRange : Mode::SingleUser;
    N("freq", r.freq); N("rate", r.rate); N("lockFreq", r.lockFreq); N("lockRate", r.lockRate);
    I("gain", r.gain); I("lnaState", r.lnaState); I("ifGr", r.ifGr); I("ifAgc", r.ifAgc);
    S("demodMode", r.demodMode); N("landingFreq", r.landingFreq);
    S("allowRanges", r.allowRanges); S("blockRanges", r.blockRanges);
    I("users", r.users); N("maxBw", r.maxBw); N("maxFps", r.maxFps); N("fftRate", r.fftRate);
    I("uncompressed", r.uncompressed);
    B("forceIdleSaver", r.forceIdleSaver); B("releaseWhenIdle", r.releaseWhenIdle);
    N("idleGrace", r.idleGrace);
    B("rfNotch", r.rfNotch);
    S("allowRanges", r.allowRanges);
    S("blockRanges", r.blockRanges); B("dabNotch", r.dabNotch); B("zoomSpectrum", r.zoomSpectrum);
    B("biasT", r.biasT);
    I("ppm", r.ppm); I("ppb", r.ppb); I("directSampling", r.directSampling);
    B("spectrogram", r.spectrogram);
    I("sessionLimitMin", r.sessionLimitMin);
}

std::string radioToJson(const RadioConfig& r) {
    std::string o = "{";
    auto S = [&](const char* k, const std::string& v) { o += "\"" + std::string(k) + "\":\"" + esc(v) + "\","; };
    auto N = [&](const char* k, double v) { o += "\"" + std::string(k) + "\":" + num(v) + ","; };
    auto B = [&](const char* k, bool v)   { o += "\"" + std::string(k) + "\":" + (v ? "true" : "false") + ","; };
    S("serial", r.serial); S("driver", r.driver); S("usbPath", r.usbPath); S("label", r.label);
    B("enabled", r.enabled); B("configured", r.configured);
    N("port", r.port);
    S("mode", r.mode == Mode::LockedRange ? "locked" : "single");
    N("freq", r.freq); N("rate", r.rate); N("lockFreq", r.lockFreq); N("lockRate", r.lockRate);
    N("gain", r.gain); N("lnaState", r.lnaState); N("ifGr", r.ifGr); N("ifAgc", r.ifAgc);
    S("demodMode", r.demodMode); N("landingFreq", r.landingFreq);
    // ★★★ THE BAND LISTS MUST TRAVEL. There are TWO radio writers — one for the file and one for
    //     the config API — and only the file one had these. The setup page reads the API, so the
    //     lists it showed were always empty and the ones it saved were always blank: an owner set
    //     "FM and AM broadcast only" and got a receiver that tuned anywhere (Stuart, 2026-08-08).
    //     A field added to one writer and not the other is invisible until someone trusts it.
    S("allowRanges", r.allowRanges); S("blockRanges", r.blockRanges);
    N("users", r.users); N("maxBw", r.maxBw); N("maxFps", r.maxFps); N("fftRate", r.fftRate);
    N("uncompressed", r.uncompressed);
    B("forceIdleSaver", r.forceIdleSaver); B("releaseWhenIdle", r.releaseWhenIdle);
    N("idleGrace", r.idleGrace);
    B("rfNotch", r.rfNotch); B("dabNotch", r.dabNotch); B("zoomSpectrum", r.zoomSpectrum);
    B("biasT", r.biasT);
    N("ppm", r.ppm); N("ppb", r.ppb); N("directSampling", r.directSampling);
    B("spectrogram", r.spectrogram);
    N("sessionLimitMin", r.sessionLimitMin);
    if (!o.empty() && o.back() == ',') o.pop_back();
    return o + "}";
}

/** ★★★ TODAY'S FILE, READ AS A ONE-RADIO MACHINE. Every install in the field has this shape, and
 *  the upgrade must be invisible to them.
 *  ★★ enabled AND configured both TRUE — never defaulted. These gates are NEW, and a receiver that
 *     has been working for months must not go dark because it has not ticked a box that did not
 *     exist when it was set up. Exactly the fault that took the demo off the air when config.json
 *     itself was introduced: a test on a FRESH server can never catch it. */
void migrateSingleRadio(const std::string& json, ServerConfig& out) {
    Config one;
    std::string ignored;
    fromJson(json, one, ignored, /*validate=*/false);

    out.configured   = one.configured;
    // ★★★ DECIDE THE MODE FROM WHAT THE SERVER WAS ALREADY DOING. The switch did not exist when
    //     this file was written, so it cannot have been chosen — and defaulting every install to
    //     one answer would change what half of them do. A pinned window, several listeners, or a
    //     public listing are all things a SIMPLE receiver never has.
    out.fullMode     = (one.mode == Mode::LockedRange) || (one.users > 1)
                       || (one.sharing == Sharing::Public);
    out.sharing      = one.sharing;
    out.name         = one.name;
    out.place        = one.place;   out.country = one.country;
    out.locator      = one.locator; out.lat = one.lat; out.lon = one.lon;
    out.mdnsAdvertise= one.mdnsAdvertise; out.mdnsName = one.mdnsName;
    out.pin          = one.pin;     out.adminPass = one.adminPass;
    out.sessionLimitMin = one.sessionLimitMin;
    out.updateSrvHour= one.updateSrvHour; out.updateSrvDay = one.updateSrvDay;
    out.updateAllHour= one.updateAllHour; out.updateAllDay = one.updateAllDay;
    out.adminIdleMin = one.adminIdleMin;
    out.cpuGovernor  = one.cpuGovernor;
    out.trustedProxies = one.trustedProxies;
    out.port         = one.port;
    out.web          = one.web;

    RadioConfig r;
    r.label = one.name;
    r.enabled = true;
    r.configured = one.configured;   // ★ it was serving, so it is configured
    r.mode = one.mode;
    r.freq = one.freq; r.rate = one.rate; r.lockFreq = one.lockFreq; r.lockRate = one.lockRate;
    r.gain = one.gain; r.lnaState = one.lnaState; r.ifGr = one.ifGr; r.ifAgc = one.ifAgc;
    r.demodMode = one.demodMode; r.landingFreq = one.landingFreq;
    r.users = one.users; r.maxBw = one.maxBw; r.maxFps = one.maxFps; r.fftRate = one.fftRate;
    r.uncompressed = one.uncompressed;
    r.forceIdleSaver = one.forceIdleSaver; r.releaseWhenIdle = one.releaseWhenIdle;
    r.idleGrace = one.idleGrace;
    r.rfNotch = one.rfNotch; r.dabNotch = one.dabNotch; r.zoomSpectrum = one.zoomSpectrum;
    r.allowRanges = one.allowRanges; r.blockRanges = one.blockRanges;
    r.sessionLimitMin = one.sessionLimitMin;
    r.biasT = one.biasT;
    r.ppm = one.ppm; r.ppb = one.ppb; r.directSampling = one.directSampling;
    r.port = one.port;
    out.radios.push_back(r);
}

} // namespace

std::string toJson(const ServerConfig& c) {
    std::string o = "{\n";
    auto S = [&](const char* k, const std::string& v) { o += "  \"" + std::string(k) + "\": \"" + esc(v) + "\",\n"; };
    auto N = [&](const char* k, double v) { o += "  \"" + std::string(k) + "\": " + num(v) + ",\n"; };
    auto B = [&](const char* k, bool v)   { o += "  \"" + std::string(k) + "\": " + (v ? "true" : "false") + ",\n"; };
    B("configured", c.configured);
    B("fullMode", c.fullMode);
    S("sharing", c.sharing == Sharing::Public ? "public" : "local");
    S("name", c.name); S("place", c.place); S("country", c.country);
    S("locator", c.locator); S("lat", c.lat); S("lon", c.lon);
    B("mdnsAdvertise", c.mdnsAdvertise); S("mdnsName", c.mdnsName);
    S("pin", c.pin); S("adminPass", c.adminPass);
    N("sessionLimitMin", c.sessionLimitMin);
    N("updateSrvHour", c.updateSrvHour); N("updateSrvDay", c.updateSrvDay);
    N("updateAllHour", c.updateAllHour); N("updateAllDay", c.updateAllDay);
    N("adminIdleMin", c.adminIdleMin);
    S("cpuGovernor", c.cpuGovernor);
    S("trustedProxies", c.trustedProxies);
    N("port", c.port); B("web", c.web);
    o += "  \"radios\": [";
    for (size_t i = 0; i < c.radios.size(); i++) {
        o += (i ? ",\n    " : "\n    ");
        o += radioToJson(c.radios[i]);
    }
    o += c.radios.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

bool fromJson(const std::string& j, ServerConfig& c, std::string& err) {
    std::vector<std::string> objs;
    const bool haveRadios = getObjectArray(j, "radios", objs);
    if (!haveRadios && c.radios.empty()) {
        // ★ No array, and we hold none: this is today's single-radio FILE. See migrateSingleRadio.
        migrateSingleRadio(j, c);
        return true;
    }
    // ★★★ NO RADIOS IN THE DOCUMENT, BUT WE ALREADY HAVE SOME — THIS IS A PATCH, NOT A FILE.
    //
    //     The server persists live changes as fragments: an admin nudges the gain and the daemon
    //     writes {"gain":123}. That has no radios array, and treating it as an old-format config
    //     ran the migration over a machine that already had three — inventing a radio from the
    //     fragment's DEFAULTS and marking every radio `configured`, which put two receivers that
    //     had never been set up onto the landing page claiming to be serving.
    //
    // ★★ A PATCH MUST NOT HAVE AN OPINION ABOUT FIELDS IT DOES NOT MENTION. Exactly the lesson
    //    the live-persist path learned from the other side, and the same one the TUI learned:
    //    re-read, change only what you were told, write back.
    double n = 0; bool b = false; std::string t;
    auto S = [&](const char* k, std::string& dst) { if (getStr(j, k, t)) dst = t; };
    auto I = [&](const char* k, int& dst)         { if (getNum(j, k, n)) dst = (int)n; };
    auto B = [&](const char* k, bool& dst)        { if (getBool(j, k, b)) dst = b; };
    B("configured", c.configured);
    B("fullMode", c.fullMode);
    if (getStr(j, "sharing", t)) c.sharing = (t == "public") ? Sharing::Public : Sharing::Local;
    S("name", c.name); S("place", c.place); S("country", c.country);
    S("locator", c.locator); S("lat", c.lat); S("lon", c.lon);
    B("mdnsAdvertise", c.mdnsAdvertise); S("mdnsName", c.mdnsName);
    S("pin", c.pin); S("adminPass", c.adminPass);
    I("sessionLimitMin", c.sessionLimitMin);
    I("updateSrvHour", c.updateSrvHour); I("updateSrvDay", c.updateSrvDay);
    I("updateAllHour", c.updateAllHour); I("updateAllDay", c.updateAllDay);
    I("adminIdleMin", c.adminIdleMin);
    S("cpuGovernor", c.cpuGovernor);
    S("trustedProxies", c.trustedProxies);
    I("uncompressed", c.uncompressed);
    I("port", c.port); B("web", c.web);

    if (haveRadios) {
        // ★★★ MERGE BY SERIAL — NEVER REPLACE THE LIST. Replacing it meant one bad save could
        //     delete every radio on the machine, and one did: a page holding a stale (or empty)
        //     view posted `radios: []` and the config came back with NOTHING in it. The receiver
        //     then had no radios at all and the landing page had nothing to show.
        //
        // ★★ A CONFIG API UPDATES RADIOS; IT DOES NOT DEFINE WHICH EXIST. What exists is decided by
        //    the hardware and by the owner in the setup screen. Removal is a deliberate act there,
        //    not a side effect of a settings save that happened to omit one.
        // ★ An entry we have never seen is still ADDED — that is how a newly attached radio
        //   arrives — so this is not "ignore the client", it is "do not let it delete".
        for (const auto& o : objs) {
            RadioConfig incoming; radioFromJson(o, incoming);
            if (incoming.serial.empty()) continue;      // cannot be matched, cannot be trusted
            bool merged = false;
            for (auto& existing : c.radios)
                if (existing.serial == incoming.serial) { existing = incoming; merged = true; break; }
            if (!merged) c.radios.push_back(incoming);
        }
    }
    (void)err;
    return true;
}

bool needsFrontDoor(const ServerConfig& cfg) { return cfg.fullMode; }

bool canDrawSpectrogram(const RadioConfig& r) {
    // ★ Fixed window, kept, and actually served. Any one of those missing and the picture would
    //   have holes the viewer cannot account for — which is worse than no picture.
    return r.enabled && r.configured
        && r.mode == Mode::LockedRange
        && !r.releaseWhenIdle;
}

int spectrogramRadio(const ServerConfig& cfg) {
    for (size_t i = 0; i < cfg.radios.size(); i++)
        if (cfg.radios[i].spectrogram && canDrawSpectrogram(cfg.radios[i])) return (int)i;
    return -1;   // ★ A real answer: no radio can draw one, so the page must not pretend.
}

int primaryRadio(const ServerConfig& cfg) {
    for (size_t i = 0; i < cfg.radios.size(); i++)
        if (cfg.radios[i].enabled && cfg.radios[i].configured) return (int)i;
    return -1;
}

int portForRadio(const ServerConfig& cfg, size_t index) {
    const int base = cfg.port > 0 ? cfg.port : 48000;
    if (index >= cfg.radios.size()) return base;
    // ★ An explicit per-radio port always wins — an owner who pinned one did it for a router rule.
    if (cfg.radios[index].port > 0) return cfg.radios[index].port;

    // ★★ WITH A FRONT DOOR, NO RADIO TAKES THE PUBLIC PORT. The front door has it, and every radio
    //    queues behind: 48001, 48002, 48003. There is no "primary" to be special about, which is
    //    the whole reason this is cleaner — one shape for every radio.
    if (needsFrontDoor(cfg)) {
        int offset = 1;
        for (size_t i = 0; i < index && i < cfg.radios.size(); i++)
            if (cfg.radios[i].enabled && cfg.radios[i].configured) offset++;
        return base + offset;
    }

    const int primary = primaryRadio(cfg);
    if ((int)index == primary) return base;
    // Everyone else takes base+1, base+2 … in ARRAY order, skipping the primary. Counting only
    // the radios before this one keeps a receiver's port stable when a LATER radio is switched off.
    int offset = 1;
    for (size_t i = 0; i < index && i < cfg.radios.size(); i++)
        if ((int)i != primary) offset++;
    return base + offset;
}

Config effectiveFor(const ServerConfig& s, const RadioConfig& r) {
    Config c;
    c.configured = s.configured && r.configured;
    c.sharing = s.sharing;
    // ★ The radio's own label is what listeners see; the machine's name heads the landing page.
    c.name = r.label.empty() ? s.name : r.label;
    c.place = s.place; c.country = s.country;
    c.locator = s.locator; c.lat = s.lat; c.lon = s.lon;
    c.mdnsAdvertise = s.mdnsAdvertise; c.mdnsName = s.mdnsName;
    c.pin = s.pin; c.adminPass = s.adminPass;
    // ★ The radio's own limit, falling back to the machine-wide one a pre-per-radio
    //   config would have carried — so an existing server keeps the limit it had.
    c.sessionLimitMin = r.sessionLimitMin > 0 ? r.sessionLimitMin : s.sessionLimitMin;
    c.updateSrvHour = s.updateSrvHour; c.updateSrvDay = s.updateSrvDay;
    c.updateAllHour = s.updateAllHour; c.updateAllDay = s.updateAllDay;
    c.adminIdleMin = s.adminIdleMin;
    c.cpuGovernor = s.cpuGovernor;
    c.trustedProxies = s.trustedProxies;
    c.web = s.web;

    c.mode = r.mode;
    c.freq = r.freq; c.rate = r.rate; c.lockFreq = r.lockFreq; c.lockRate = r.lockRate;
    c.gain = r.gain; c.lnaState = r.lnaState; c.ifGr = r.ifGr; c.ifAgc = r.ifAgc;
    c.demodMode = r.demodMode; c.landingFreq = r.landingFreq;
    // ★★★ AN UNLOCKED RADIO STARTS WHERE ITS LISTENERS WILL. A locked radio's centre is the
    //     owner's fixed window and must not move. An unlocked one has no window to protect, and
    //     leaving the capture on `freq` meant the radio sat on a band nobody was going to use —
    //     an RTL with a landing of 648 kHz still opened at 145 MHz, so before anyone connected the
    //     waterfall, the spectrogram and the landing page all showed the wrong band, and the first
    //     listener paid for a retune across 144 MHz to get where the owner had already said.
    // ★ Only when the owner actually set one: 0 means "same as freq", which is already true.
    if (r.mode != Mode::LockedRange && r.landingFreq > 0) c.freq = r.landingFreq;
    c.users = r.users; c.maxBw = r.maxBw; c.maxFps = r.maxFps; c.fftRate = r.fftRate;
    // ★ The MACHINE's choice wins — one uplink, one answer. A file that only carries the old
    //   per-radio value still works and is migrated up on the next save.
    c.uncompressed = s.uncompressed ? s.uncompressed : r.uncompressed;
    c.forceIdleSaver = r.forceIdleSaver; c.releaseWhenIdle = r.releaseWhenIdle;
    c.idleGrace = r.idleGrace;
    c.rfNotch = r.rfNotch; c.dabNotch = r.dabNotch; c.zoomSpectrum = r.zoomSpectrum;
    c.allowRanges = r.allowRanges; c.blockRanges = r.blockRanges;
    c.biasT = r.biasT;
    c.ppm = r.ppm; c.ppb = r.ppb; c.directSampling = r.directSampling;
    c.port = r.port;
    return c;
}

bool loadServer(const std::string& path, ServerConfig& cfg, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string body;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
    fclose(f);
    return fromJson(body, cfg, err);
}

bool saveServer(const std::string& path, const ServerConfig& cfg, std::string& err) {
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = std::string("cannot write ") + tmp + ": " + strerror(errno); return false; }
    const std::string j = toJson(cfg);
    if (fwrite(j.data(), 1, j.size(), f) != j.size()) {
        err = "short write to " + tmp; fclose(f); unlink(tmp.c_str()); return false;
    }
    fflush(f);
    if (fsync(fileno(f)) != 0) { err = "fsync failed"; fclose(f); unlink(tmp.c_str()); return false; }
    fchmod(fileno(f), 0600);       // ★ before the rename — it holds the admin password in clear
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        err = std::string("rename failed: ") + strerror(errno);
        unlink(tmp.c_str()); return false;
    }
    return true;
}

} // namespace vsconfig
