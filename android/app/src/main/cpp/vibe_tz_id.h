// ★★★ THE SERVER'S IANA ZONE NAME ("America/Sao_Paulo"), sent beside tzOffsetMin/tzAbbr (2026-11-12).
//
// ★★ WHY: Kiko's receiver is an old Android TV box in Paraná. Brazil abolished DST in 2019, but the
//    box's pre-2019 tzdata still springs forward in November, so the server said "-120 / BRST" and
//    every listener saw 20:20 when it was 19:20 at the aerial. The offset is computed from the
//    DEVICE's rules, and the device is the one most likely to be years out of date. The zone NAME is
//    not stale — Sao_Paulo is still Sao_Paulo — so we send it and let a client with current rules
//    (a browser, a phone) work the offset out itself. tzOffsetMin/tzAbbr stay for older clients.
//
// ★ Sources, first that yields a plausible name wins:
//     TZ env (":Europe/London", "Europe/London", or a path under zoneinfo/)
//     Android:   persist.sys.timezone — the property bionic's own localtime() reads
//     Apple:     CFTimeZoneCopySystem() — honours TZ, works inside the iOS sandbox
//     Linux:     the /etc/localtime symlink target after "zoneinfo/" (all `timedatectl` reads — no
//                process spawned, no new dependency), then /etc/timezone (Debian's older file, which
//                newer systemd no longer updates — so the symlink wins when both exist)
// ★ Empty when nothing says — the field is then simply not sent and clients behave exactly as before.
// ★ A POSIX rule string ("GMT0BST,M3.5.0/1,M10.5.0") is NOT a zone name and is refused by the
//   charset check (the comma). Anything else odd that slips through is harmless: a client whose Intl
//   does not know the name falls back to tzOffsetMin/tzAbbr.
#pragma once

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#if defined(__ANDROID__)
  #include <sys/system_properties.h>
#elif defined(__APPLE__)
  #include <CoreFoundation/CoreFoundation.h>
#endif

/** ★ Is this shaped like an IANA name? Letters, digits, _ + - / only; no "..", no leading '/',
 *  at most 64 characters. Not a lookup — the client's Intl is the judge of whether it exists. */
inline bool vsTzIdPlausible(const std::string& s) {
    if (s.empty() || s.size() > 64 || s[0] == '/' || s[0] == '-' || s.find("..") != std::string::npos)
        return false;
    for (char c : s) {
        const bool okc = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         c == '_' || c == '+' || c == '-' || c == '/';
        if (!okc) return false;
    }
    return true;
}

/** ★ Turn one raw source value into a zone name, or "". Accepts a bare name, a ':'-prefixed TZ value,
 *  or any path containing "zoneinfo/" (/usr/share/zoneinfo/X, /var/db/timezone/zoneinfo/X,
 *  ../usr/share/zoneinfo/X, zoneinfo/posix/X, zoneinfo/right/X). Trailing whitespace is trimmed. */
inline std::string vsTzIdNormalise(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    const size_t z = s.rfind("zoneinfo/");
    if (z != std::string::npos) {
        s = s.substr(z + 9);
        if (s.compare(0, 6, "posix/") == 0) s.erase(0, 6);
        else if (s.compare(0, 6, "right/") == 0) s.erase(0, 6);
    } else if (!s.empty() && s[0] == '/') {
        return {};   // a path, but not into a zone database — not a name
    }
    return vsTzIdPlausible(s) ? s : std::string();
}

/** ★ The pure chooser — every source handed in, so the test can drive it without touching the
 *  machine's real zone. Pass nullptr / "" for a source that said nothing. */
inline std::string vsTzIdFrom(const char* tzEnv, const std::string& platformId,
                              const std::string& etcTimezone, const std::string& localtimeLink) {
    if (tzEnv && *tzEnv) {
        const std::string t = vsTzIdNormalise(tzEnv);
        if (!t.empty()) return t;
    }
    for (const std::string* src : {&platformId, &localtimeLink, &etcTimezone}) {
        const std::string t = vsTzIdNormalise(*src);
        if (!t.empty()) return t;
    }
    return {};
}

/** ★ This machine's zone name, or "" — read fresh each call (a few hundred ns: a getenv, one small
 *  file, one readlink) so a zone changed in Settings shows up without a restart. */
inline std::string vsZoneId() {
    std::string platformId, etcTz, link;
#if defined(__ANDROID__)
    char prop[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.sys.timezone", prop) > 0) platformId = prop;
#elif defined(__APPLE__)
    if (CFTimeZoneRef tz = CFTimeZoneCopySystem()) {
        char buf[96] = {0};
        if (CFStringGetCString(CFTimeZoneGetName(tz), buf, sizeof buf, kCFStringEncodingUTF8)) platformId = buf;
        CFRelease(tz);
    }
#else
    if (FILE* f = std::fopen("/etc/timezone", "r")) {
        char buf[96] = {0};
        if (std::fgets(buf, sizeof buf, f)) etcTz = buf;
        std::fclose(f);
    }
    char lb[PATH_MAX] = {0};
    const ssize_t n = readlink("/etc/localtime", lb, sizeof lb - 1);
    if (n > 0) { lb[n] = 0; link = lb; }
#endif
    return vsTzIdFrom(std::getenv("TZ"), platformId, etcTz, link);
}
