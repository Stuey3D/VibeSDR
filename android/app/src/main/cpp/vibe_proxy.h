// vibe_proxy.h — work out who the client really is when a reverse proxy is in the way.
//
// ★★★ WHY THIS EXISTS. Put VibeServer behind nginx/Caddy/a tunnel and every connection arrives
//     from the proxy, so `getpeername()` returns 127.0.0.1 for everybody. Everything keyed on the
//     address then collapses onto one identity: banning an abuser bans the whole audience, the
//     country flags and the top-countries chart all show the proxy, the connection log cannot tell
//     two listeners apart, and the admin brute-force lockout can be tripped for everyone at once
//     by a single scanner. (Saber, 2026-08-08: "the logs spammed to me 127.0.0.1 connected".)
//
// ★★★ AND WHY IT IS OPT-IN. X-Forwarded-For is a header — text the client types. A server that
//     believes it from anyone lets a stranger claim any address they like, which walks straight
//     through the ban list and the ASN blocks and makes the connection log actively misleading.
//     That is WORSE than having no proxy support at all, so the owner must name the proxies they
//     trust; with an empty list (the default) the header is never read.
#pragma once

#include "vibe_admin.h"

#include <string>
#include <vector>

namespace vibeproxy {

/** The owner's trusted-proxy list, compiled once. Entries are addresses or CIDRs, same syntax as
 *  the ban list — they go through the same parser, so there is one place where "192.168.1.0/24"
 *  is understood rather than two that can disagree. */
class TrustedProxies {
public:
    void set(const std::vector<std::string>& entries) {
        list_.clear();
        for (const auto& e : entries) {
            vibeadmin::Ban b;
            b.cidr = e;
            // ★ A malformed entry is DROPPED, not fatal: one typo in the config must not disable
            //   the proxies that were spelled correctly (and silently un-trust the real one).
            if (vibeadmin::compile(b) && !b.asn) list_.push_back(b);
        }
    }
    bool empty() const { return list_.empty(); }
    bool trusted(const std::string& ip) const {
        if (ip.empty()) return false;
        for (const auto& b : list_) if (vibeadmin::matches(b, ip)) return true;
        return false;
    }

private:
    std::vector<vibeadmin::Ban> list_;
};

/** True if `s` is a syntactically valid bare IP address (v4 or v6, no prefix). */
inline bool isPlainAddress(const std::string& s) {
    if (s.empty() || s.size() > 45) return false;         // 45 = longest possible v6 text form
    if (s.find('/') != std::string::npos) return false;   // a CIDR is not a client address
    vibeadmin::Ban b;
    b.cidr = s;
    if (!vibeadmin::compile(b) || b.asn) return false;
    // compile() accepts a bare address as a full-length prefix; anything shorter came from a "/n"
    // we already rejected, so this is belt and braces.
    return b.bits == (b.v6 ? 128 : 32);
}

/**
 * The address to treat as the client's.
 *
 * @param tp        the owner's trusted-proxy list
 * @param peer      the real TCP peer (never trust the caller to have substituted anything)
 * @param xff       raw X-Forwarded-For header value ("" if absent)
 * @param xRealIp   raw X-Real-IP header value ("" if absent)
 *
 * Returns `peer` unchanged unless the peer is a trusted proxy AND the headers yield a valid
 * address — so every failure mode lands on the truth we can actually observe.
 */
inline std::string clientAddress(const TrustedProxies& tp, const std::string& peer,
                                 const std::string& xff, const std::string& xRealIp) {
    if (!tp.trusted(peer)) return peer;

    // ★ The header is unbounded input that ends up in the connection log and possibly the ban
    //   list. Cap it before doing any work — a megabyte of commas is not a proxy chain.
    if (xff.size() <= 1024 && !xff.empty()) {
        std::vector<std::string> hops;
        size_t start = 0;
        while (start <= xff.size()) {
            const size_t comma = xff.find(',', start);
            const size_t end = comma == std::string::npos ? xff.size() : comma;
            std::string h = xff.substr(start, end - start);
            const size_t a = h.find_first_not_of(" \t");
            const size_t b = h.find_last_not_of(" \t\r\n");
            if (a != std::string::npos) hops.push_back(h.substr(a, b - a + 1));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        // ★★ RIGHT TO LEFT, PAST OUR OWN INFRASTRUCTURE. The client can prepend whatever it likes,
        //    so the LEFTMOST entry is forgeable even through a trusted proxy. Each trusted hop
        //    appended what IT saw, so the first non-trusted address from the right is the earliest
        //    one our own equipment actually observed.
        for (size_t i = hops.size(); i-- > 0;) {
            if (!isPlainAddress(hops[i])) continue;
            if (tp.trusted(hops[i])) continue;      // still inside our own chain
            return hops[i];
        }
    }

    if (xRealIp.size() <= 64) {
        const size_t a = xRealIp.find_first_not_of(" \t");
        const size_t b = xRealIp.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) {
            const std::string one = xRealIp.substr(a, b - a + 1);
            if (isPlainAddress(one)) return one;
        }
    }
    return peer;
}

}  // namespace vibeproxy
