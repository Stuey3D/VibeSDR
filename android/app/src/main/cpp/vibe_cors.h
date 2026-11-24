// vibe_cors.h — who may read this receiver from another origin, decided once per request (beta93, 2026-11-24).
//
// ★★★ WHY. A listener who opens <slug>.vibeserver.vibesdr.net gets the page through the directory's Cloudflare Worker
//     (a stable origin, so localStorage survives the Quick Tunnel's rotating hostname). Until beta93 every HTTP read
//     that page made — the gallery, the DX Log, telemetry charts, logos, map data — went back through that Worker, one
//     billed request each; on 2026-11-24 one page's runaway DX Log poll spent the whole account's daily allowance and
//     took every listed tunnel down. Stuart: "the gallery etc should all be handled via the free tunnel not the main
//     workers — they should only establish the connection." So the page now reads THIS server directly (web/client/
//     src/origin.ts dataUrl) — cross-origin, from https://<slug>.vibeserver.vibesdr.net — and this file says who may.
//
// ★★ THE RULE.
//   · Origin https://vibeserver.vibesdr.net or https://<one label>.vibeserver.vibesdr.net → the answer carries
//     `Access-Control-Allow-Origin: <that origin>` + `Vary: Origin`, whatever the route wrote (net_shim Socket::
//     setCorsHead rewrites the head). Never `*` for these.
//   · Any other Origin on a request that CARRIES A CREDENTIAL (the PIN proof, an admin proof or ticket, a raw-IQ token)
//     or reaches an owner page (admin, setup, config) → every Access-Control-Allow-Origin line is REMOVED. A foreign
//     page may still send the request (CORS never stopped that), but it can no longer read the answer.
//   · Anything else (no Origin, same-origin, a foreign origin on a public read) → the bytes go out exactly as the route
//     wrote them — the public `*` on /vibeserver.json and friends is unchanged, so nothing that reads those today breaks.
// ★★ WHY ECHOING THE SLUG ORIGINS IS SAFE. VibeServer has no cookies and no HTTP auth — every credential travels in the
//    URL query (readAuthUrl, the admin ticket), so a browser attaches nothing ambient to these requests, and a page on
//    some OTHER server's slug cannot use a credential it was never given (the victim page's storage is its own
//    origin's). The only ambient authority VibeServer grants is LOOPBACK, and a loopback request with a foreign Origin
//    is refused before any of this (the cross-site check in handleConnection). Echoing therefore grants a slug page no
//    more than `*` already granted anyone — it is narrower — and `*` is never sent where a credential is in play.
#pragma once

#include <cctype>
#include <string>

namespace vibecors {

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/** `origin` exactly as the browser sent it, when it is the directory or one of its server addresses; else "". */
inline std::string allowedOrigin(const std::string& origin) {
    static const std::string kPre = "https://", kZone = "vibeserver.vibesdr.net";
    if (origin.size() > 200) return "";
    const std::string o = lower(origin);
    if (o.compare(0, kPre.size(), kPre) != 0) return "";
    std::string host = o.substr(kPre.size());
    /* ★ A port is allowed: the NAME is what makes it ours (only our Worker answers under vibeserver.vibesdr.net, on any
     *   port Cloudflare proxies), and the end-to-end test serves the stand-in Worker on a high port. Digits only. */
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        const std::string port = host.substr(colon + 1);
        if (port.empty() || port.size() > 5) return "";
        for (char c : port) if (!std::isdigit((unsigned char)c)) return "";
        host.resize(colon);
    }
    if (host == kZone) return origin;
    if (host.size() <= kZone.size() + 1) return "";
    if (host.compare(host.size() - kZone.size(), kZone.size(), kZone) != 0) return "";
    if (host[host.size() - kZone.size() - 1] != '.') return "";
    const std::string label = host.substr(0, host.size() - kZone.size() - 1);
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return "";
    for (char c : label) if (!(std::isalnum((unsigned char)c) || c == '-')) return "";   // ★ one label: no dot, no port, no path
    return origin;
}

/** True when the request line carries a credential, or reaches a page only an owner uses. */
inline bool credentialed(const std::string& reqLine) {
    const size_t sp = reqLine.find(' ');
    if (sp == std::string::npos) return false;
    size_t e = reqLine.find(' ', sp + 1);
    if (e == std::string::npos) e = reqLine.size();
    const std::string target = reqLine.substr(sp + 1, e - sp - 1);
    const size_t q = target.find('?');
    std::string path = target.substr(0, q);
    if (path.rfind("/r/", 0) == 0) {                       // ★ judged by the radio's own path
        const size_t s = path.find('/', 3);
        path = s == std::string::npos ? "/" : path.substr(s);
    }
    if (path.rfind("/vibeserver/admin", 0) == 0 || path.rfind("/vibeserver/config", 0) == 0
        || path.rfind("/setup", 0) == 0 || path.rfind("/vibeserver/unlock", 0) == 0) return true;
    if (q == std::string::npos) return false;
    const std::string qs = "&" + target.substr(q + 1);
    for (const char* k : { "vs_nonce=", "vs_auth=", "vs_admin_nonce=", "vs_admin_auth=", "vs_admin_ticket=", "tok=" })
        if (qs.find(std::string("&") + k) != std::string::npos) return true;
    return false;
}

/** What Socket::setCorsHead should be given for this request, and whether to give it anything at all.
 *  @return true with `line` set (possibly empty = strip only), or false = leave the answer as the route writes it. */
inline bool headFor(const std::string& originHeader, const std::string& reqLine, std::string& line) {
    const std::string ok = allowedOrigin(originHeader);
    if (!ok.empty()) { line = "Access-Control-Allow-Origin: " + ok + "\r\nVary: Origin\r\n"; return true; }
    if (!originHeader.empty() && credentialed(reqLine)) { line.clear(); return true; }
    return false;
}

/** The whole answer to a CORS preflight (OPTIONS) from an allowed origin. Asked for by a direct read that sends a
 *  header outside the CORS-safelisted few — the GPU map's PMTiles Range reads, a JSON POST. Only the request headers
 *  the page actually uses are allowed back. */
inline std::string preflight(const std::string& origin) {
    return "HTTP/1.1 204 No Content\r\n"
           "Access-Control-Allow-Origin: " + origin + "\r\nVary: Origin\r\n"
           "Access-Control-Allow-Methods: GET, HEAD, POST, PUT, DELETE\r\n"
           "Access-Control-Allow-Headers: Content-Type, Range, If-None-Match\r\n"
           "Access-Control-Max-Age: 600\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
}

/** ★★ The receiver's own public host, for the PAGE's Content-Security-Policy, when the page was proxied by the
 *  directory: `https://<host>` or "". The Worker names it (x-vibesdr-direct-host); an older Worker does not, and then
 *  the Host the tunnel delivered is that same name. Only a bare DNS name is believed — anything else, nothing is added.
 *  ★ Forging these headers changes only the policy of the answer the forger receives (the page is no-store). */
inline std::string cspDirect(const std::string& via, const std::string& directHeader, const std::string& hostHeader) {
    if (via != "directory") return "";
    std::string h = lower(!directHeader.empty() ? directHeader : hostHeader);
    if (h.empty() || h.size() > 253) return "";
    for (char c : h) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '.' || c == ':')) return "";
    const size_t colon = h.find(':');
    const std::string name = h.substr(0, colon);
    if (name.find('.') == std::string::npos) return "";     // ★ a public name, never "localhost" or a bare word
    if (name == "vibeserver.vibesdr.net" || (name.size() > 23 && name.compare(name.size() - 23, 23, ".vibeserver.vibesdr.net") == 0))
        return "";                                          // ★ the slug itself is 'self' already
    if (colon != std::string::npos) {
        const std::string port = h.substr(colon + 1);
        if (port.empty() || port.size() > 5) return "";
        for (char c : port) if (!std::isdigit((unsigned char)c)) return "";
    }
    return "https://" + h;
}

}  // namespace vibecors
