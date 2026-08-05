#include "vibeserver_config.h"

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
    S("name", c.name); S("place", c.place); S("country", c.country);
    S("locator", c.locator); S("lat", c.lat); S("lon", c.lon);
    B("mdnsAdvertise", c.mdnsAdvertise);
    S("mdnsName", c.mdnsName);
    S("pin", c.pin); S("adminPass", c.adminPass);
    N("sessionLimitMin", c.sessionLimitMin);
    N("freq", c.freq); N("rate", c.rate); N("lockFreq", c.lockFreq); N("lockRate", c.lockRate);
    N("gain", c.gain);
    S("demodMode", c.demodMode);
    N("landingFreq", c.landingFreq);
    N("users", c.users); N("maxBw", c.maxBw); N("maxFps", c.maxFps); N("fftRate", c.fftRate);
    N("uncompressed", c.uncompressed);
    B("forceIdleSaver", c.forceIdleSaver);
    N("idleGrace", c.idleGrace);
    B("rfNotch", c.rfNotch); B("dabNotch", c.dabNotch); B("zoomSpectrum", c.zoomSpectrum);
    N("port", c.port);
    j += "  \"web\": " + std::string(c.web ? "true" : "false") + "\n}\n";
    return j;
}

bool fromJson(const std::string& s, Config& c, std::string& err) {
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
    getStr(s, "name", c.name);     getStr(s, "place", c.place);
    getStr(s, "country", c.country); getStr(s, "locator", c.locator);
    getStr(s, "lat", c.lat);       getStr(s, "lon", c.lon);
    getBool(s, "mdnsAdvertise", c.mdnsAdvertise);
    getStr(s, "mdnsName", c.mdnsName);
    getStr(s, "pin", c.pin);       getStr(s, "adminPass", c.adminPass);
    if (getNum(s, "sessionLimitMin", d)) c.sessionLimitMin = (int)d;
    if (getNum(s, "freq", d))        c.freq = d;
    if (getNum(s, "rate", d))        c.rate = d;
    if (getNum(s, "lockFreq", d))    c.lockFreq = d;
    if (getNum(s, "lockRate", d))    c.lockRate = d;
    if (getNum(s, "gain", d))        c.gain = (int)d;
    getStr(s, "demodMode", c.demodMode);
    if (getNum(s, "landingFreq", d)) c.landingFreq = d;
    if (getNum(s, "users", d))       c.users = (int)d;
    if (getNum(s, "maxBw", d))       c.maxBw = d;
    if (getNum(s, "maxFps", d))      c.maxFps = d;
    if (getNum(s, "fftRate", d))     c.fftRate = d;
    if (getNum(s, "uncompressed", d)) c.uncompressed = (int)d;
    getBool(s, "forceIdleSaver", c.forceIdleSaver);
    if (getNum(s, "idleGrace", d))   c.idleGrace = d;
    getBool(s, "rfNotch", c.rfNotch);
    getBool(s, "dabNotch", c.dabNotch);
    getBool(s, "zoomSpectrum", c.zoomSpectrum);
    if (getNum(s, "port", d))        c.port = (int)d;
    getBool(s, "web", c.web);

    // ★★ VALIDATE WHAT THE SERVER CANNOT SURVIVE, and only that. A config that would refuse to
    //    start is worse than one with an odd value in it, so the rule is: clamp the recoverable,
    //    reject only the contradictory.
    if (c.users < 1) c.users = 1;
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

} // namespace vsconfig
