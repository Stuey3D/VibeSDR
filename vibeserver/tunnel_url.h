// tunnel_url.h — which cloudflared output line is a Quick Tunnel's public address, and which is not (2026-10-27).
//
// ★★★ AN ERROR LINE WAS READ AS THE ADDRESS (Kiko, BTV B9 TV box, Android 6.0.1, Lite 11.0.13). When cloudflared
//     cannot create the tunnel it logs
//         failed to request quick Tunnel: Post "https://api.trycloudflare.com/tunnel": context deadline exceeded …
//     and the old scrape — "https:// followed somewhere by .trycloudflare.com" — took https://api.trycloudflare.com
//     from it. That is Cloudflare's provisioning API, not a tunnel: the self-check could never reach our server through
//     it, so the tunnel was recycled every 15 s for ever and the owner was told it "stopped before it was given an
//     address". Same flaw on Linux/Mac (directory.cpp) and Android (VibeTunnel.kt); the Kotlin twin is
//     VibeQuickTunnelUrl.kt, and the two must agree.
//
// ★★ WHAT cloudflared ACTUALLY PRINTS (cmd/cloudflared/tunnel/quick_tunnel.go → cliutil.LogTable, tag 2026.10.4):
//         INF +--------------------------------------------------------------------------------------------+
//         INF |  Your quick Tunnel has been created! Visit it at (it may take some time to be reachable):  |
//         INF |  https://printed-attacks-postposted-hong.trycloudflare.com                                 |
//         INF +--------------------------------------------------------------------------------------------+
//     So an address is accepted ONLY when it is the whole of a box row, or follows "Visit it at" on the same line —
//     never a URL that merely appears in a message. And never from a line that is an error or a warning.
// ★★ NO HYPHEN RULE. Real names are four words joined by hyphens, but cloudflared's own test uses
//    "example.trycloudflare.com" and the format is Cloudflare's to change — so the label is checked for shape and
//    against the names Cloudflare reserves for itself, not for how many words it has.
#pragma once
#include <cctype>
#include <cstring>
#include <string>

namespace vstunnel {

/** ★ Labels under trycloudflare.com that are Cloudflare's services, never an assigned tunnel. `api` is the one that
 *  bit (the provisioning endpoint in every failure line); `www` is the site itself. */
inline bool reservedLabel(const std::string& l) { return l == "api" || l == "www"; }

/** ★ A DNS label of the shape the edge assigns: 1–63 of [a-z0-9-], not starting or ending with '-'. */
inline bool assignableLabel(const std::string& l) {
    if (l.empty() || l.size() > 63 || l.front() == '-' || l.back() == '-') return false;
    for (char c : l) if (!(std::islower((unsigned char)c) || std::isdigit((unsigned char)c) || c == '-')) return false;
    return !reservedLabel(l);
}

/** ★★ Does this line read as a failure? Checked on the line WITH THE CANDIDATE URL CUT OUT: the edge picks random
 *  words, and a perfectly good tunnel called https://error-tomato-fail-fox.trycloudflare.com must not be refused for
 *  its own name. Levels are cloudflared's three-letter ones (ERR, WRN, FTL) as whole words. */
inline bool looksLikeFailure(const std::string& rest) {
    std::string low(rest);
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.find("fail") != std::string::npos || low.find("error") != std::string::npos) return true;
    for (const char* lvl : {"ERR", "WRN", "FTL"}) {
        for (size_t at = rest.find(lvl); at != std::string::npos; at = rest.find(lvl, at + 1)) {
            const bool l = at == 0 || !std::isalnum((unsigned char)rest[at - 1]);
            const bool r = at + 3 >= rest.size() || !std::isalnum((unsigned char)rest[at + 3]);
            if (l && r) return true;
        }
    }
    return false;
}

/** The public address in one line of cloudflared output ("https://<name>.trycloudflare.com"), or "" when the line
 *  does not announce one. Case is folded to lower; a trailing CR/LF is ignored. */
inline std::string quickTunnelUrl(const std::string& lineIn) {
    std::string line(lineIn);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    static const std::string kScheme = "https://", kZone = ".trycloudflare.com";
    for (size_t at = line.find(kScheme); at != std::string::npos; at = line.find(kScheme, at + 1)) {
        // ★ The hostname runs to the first character a hostname cannot contain.
        size_t end = at + kScheme.size();
        while (end < line.size() && (std::isalnum((unsigned char)line[end]) || line[end] == '-' || line[end] == '.')) end++;
        std::string host = line.substr(at + kScheme.size(), end - at - kScheme.size());
        for (char& c : host) c = (char)std::tolower((unsigned char)c);
        if (host.size() <= kZone.size() || host.compare(host.size() - kZone.size(), kZone.size(), kZone) != 0) continue;
        const std::string label = host.substr(0, host.size() - kZone.size());
        if (label.find('.') != std::string::npos || !assignableLabel(label)) continue;
        // ★★ STANDALONE: what follows must end the address — not "/tunnel", not a quote, not ":443".
        if (end < line.size() && !(line[end] == ' ' || line[end] == '\t' || line[end] == '|')) continue;
        if (at > 0 && !(line[at - 1] == ' ' || line[at - 1] == '\t' || line[at - 1] == '|' || line[at - 1] == ':')) continue;
        const std::string before = line.substr(0, at), after = line.substr(end);
        if (looksLikeFailure(before + " " + after)) return {};
        // ★★ AND ANNOUNCED: the whole of a box row, or the same line as "Visit it at".
        const size_t bar = before.find_last_not_of(" \t");
        const size_t bar2 = after.find_first_not_of(" \t");
        const bool boxed = bar != std::string::npos && before[bar] == '|' && bar2 != std::string::npos && after[bar2] == '|';
        std::string lowBefore(before);
        for (char& c : lowBefore) c = (char)std::tolower((unsigned char)c);
        const bool visit = lowBefore.find("visit it at") != std::string::npos;
        if (!boxed && !visit) continue;
        return kScheme + host;
    }
    return {};
}

}  // namespace vstunnel
