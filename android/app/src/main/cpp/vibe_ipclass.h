// vibe_ipclass.h — what kind of address a client has, for IPv6 as well as IPv4 (2026-10-25).
//
// ★★★ WHY THIS EXISTS. Until 11.0.11 the server listened on IPv4 only, so every client address was an IPv4 address
//     (or ::ffff:-wrapped one, unwrapped where it is first read — net::Socket::socketPeerAddress). Nick (G4IRX) on a
//     real Orange Pi One, 2026-10-25: "vibeserversetup.local resolves to a link local ipv6, then a public ipv6, but no
//     ipv4 … 'site not found'". The listener is dual-stack now, so a real IPv6 client arrives — and "is this client on
//     the LAN?" can no longer be answered from the address text alone:
//       · fe80::/10 (link-local) and fc00::/7 (ULA) are always the local network;
//       · a GLOBAL IPv6 address (2a02:…) is the local network when it is inside one of the box's OWN on-link
//         prefixes — on IPv6 there is no NAT, so the laptop beside the box has a public address in the same /64.
//         Without this, the person setting up a box over IPv6 would be told "the first password can only be set from
//         the box's own network" while standing next to it.
//     ★ IPv4 is NOT changed by any of this: callers keep their own IPv4 rules exactly as they were.
// ★★ THE BRUTE-FORCE LOCKOUT KEY. One IPv6 subscriber holds a whole /64 (2^64 addresses) and a client can pick a new
//    one per attempt, so a lockout keyed on the full address would never bite. lockKey() turns a global IPv6
//    address into its /64; IPv4 (and everything else) keys exactly as before.
// Header-only: pocket.cpp, the shim and the tests use it without pulling in the shim.
#pragma once

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#if !(defined(__ANDROID__) && __ANDROID_API__ < 24)
#include <ifaddrs.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vibeip {

struct Net6 { uint8_t addr[16]; uint8_t mask[16]; };

/** Parse an IPv6 text address (a "%zone" suffix is dropped). False for IPv4 or rubbish. */
inline bool parse6(const std::string& s, uint8_t out[16]) {
    if (s.find(':') == std::string::npos) return false;
    std::string t = s.substr(0, s.find('%'));
    in6_addr a{};
    if (::inet_pton(AF_INET6, t.c_str(), &a) != 1) return false;
    std::memcpy(out, &a, 16);
    return true;
}

inline bool isV4Mapped(const uint8_t a[16]) {
    static const uint8_t pre[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::memcmp(a, pre, 12) == 0;
}

/** fe80::/10 link-local or fc00::/7 unique-local — the local network by definition. */
inline bool isLinkLocalOrUla(const std::string& ip) {
    uint8_t a[16];
    if (!parse6(ip, a) || isV4Mapped(a)) return false;
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return true;   // fe80::/10
    if ((a[0] & 0xfe) == 0xfc) return true;                   // fc00::/7
    return false;
}

/** Is `ip` inside one of `nets`? (An IPv4 or v4-mapped address never is.) */
inline bool onLinkV6(const std::string& ip, const std::vector<Net6>& nets) {
    uint8_t a[16];
    if (!parse6(ip, a) || isV4Mapped(a)) return false;
    for (const auto& n : nets) {
        bool in = true;
        bool any = false;
        for (int k = 0; k < 16 && in; k++) {
            if (n.mask[k]) any = true;
            if ((a[k] & n.mask[k]) != (n.addr[k] & n.mask[k])) in = false;
        }
        if (in && any) return true;
    }
    return false;
}

/** The box's own IPv6 prefixes, loopback excluded (::1 is "the box itself", not "the LAN" — callers that want it
 *  ask isLoopback). Cached for 5 s: it is asked per request, and the interfaces change rarely. */
inline std::vector<Net6> localNets6() {
    static std::mutex mtx;
    static std::vector<Net6> cache;
    static std::chrono::steady_clock::time_point at{};
    std::lock_guard<std::mutex> lk(mtx);
    const auto now = std::chrono::steady_clock::now();
    if (at.time_since_epoch().count() != 0 && now - at < std::chrono::seconds(5)) return cache;
    std::vector<Net6> out;
#if !(defined(__ANDROID__) && __ANDROID_API__ < 24)
    struct ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) == 0) {
        for (auto* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET6) continue;
            if (p->ifa_flags & IFF_LOOPBACK) continue;
            Net6 n{};
            std::memcpy(n.addr, &reinterpret_cast<const sockaddr_in6*>(p->ifa_addr)->sin6_addr, 16);
            std::memcpy(n.mask, &reinterpret_cast<const sockaddr_in6*>(p->ifa_netmask)->sin6_addr, 16);
            out.push_back(n);
        }
        ::freeifaddrs(ifa);
    }
#endif
    cache = out;
    at = now;
    return cache;
}

inline bool onLinkV6(const std::string& ip) { return onLinkV6(ip, localNets6()); }

/** The key a per-client lockout should count under: a REMOTE global IPv6 address → its /64 ("2a02:1:2:3::/64").
 *  ★ A LAN address (link-local, ULA, or in one of the box's own prefixes) keys on the whole address, as an IPv4
 *    LAN address does: every device in the house shares the /64, and one mistyped PIN must not lock them all out.
 *  Anything else (IPv4, a browser key, "") unchanged. `nets` = localNets6() unless a test supplies its own. */
inline std::string lockKey(const std::string& ip, const std::vector<Net6>& nets) {
    uint8_t a[16];
    if (!parse6(ip, a) || isV4Mapped(a)) return ip;
    if (isLinkLocalOrUla(ip) || onLinkV6(ip, nets)) return ip;
    std::memset(a + 8, 0, 8);
    char buf[INET6_ADDRSTRLEN + 4] = {0};
    if (!::inet_ntop(AF_INET6, a, buf, INET6_ADDRSTRLEN)) return ip;
    return std::string(buf) + "/64";
}
inline std::string lockKey(const std::string& ip) {
    if (ip.find(':') == std::string::npos) return ip;      // ★ IPv4: no interface walk at all
    return lockKey(ip, localNets6());
}

} // namespace vibeip
