// ★★★ IPv6 CLIENTS (2026-10-25). The listener is dual-stack now (net::listen: one AF_INET6 socket, IPV6_V6ONLY=0),
//     because Nick's Orange Pi One was reachable on IPv6 only by name and "site not found" followed. This proves:
//       · an IPv4 client and an IPv6 client BOTH connect to the same "0.0.0.0" listener;
//       · the IPv4 client's address reads EXACTLY as it did on the old IPv4 listener ("127.0.0.1", never
//         "::ffff:127.0.0.1"), so bans, the connection log, the LAN and loopback tests see what they always saw;
//       · the IPv6 client reads as "::1";
//       · a named "127.0.0.1" stays IPv4 loopback only; a taken port still FAILS (the port scans depend on it);
//       · vibe_ipclass.h: link-local/ULA always LAN, a global address LAN only inside the box's own prefixes,
//         loopback never "on-link", and the brute-force key is the /64 for a REMOTE IPv6 client only.
#include "net_shim.h"
#include "vibe_ipclass.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static void ok(bool c, const char* what, const std::string& extra = "") {
    std::printf("   %s %s%s\n", c ? "ok  " : "FAIL", what, c ? "" : ("  — " + extra).c_str());
    if (!c) failures++;
}

static int dial(int family, int port) {
    const int fd = ::socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int r;
    if (family == AF_INET) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        r = ::connect(fd, (sockaddr*)&a, sizeof a);
    } else {
        sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_port = htons((uint16_t)port); a.sin6_addr = in6addr_loopback;
        r = ::connect(fd, (sockaddr*)&a, sizeof a);
    }
    if (r != 0) { ::close(fd); return -1; }
    return fd;
}

static bool hostHasIpv6() {
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_addr = in6addr_loopback;
    const bool okb = ::bind(fd, (sockaddr*)&a, sizeof a) == 0;
    ::close(fd);
    return okb;
}

static vibeip::Net6 net6(const char* addr, int bits) {
    vibeip::Net6 n{};
    in6_addr a{}; ::inet_pton(AF_INET6, addr, &a);
    std::memcpy(n.addr, &a, 16);
    for (int k = 0; k < 16; k++) { const int b = bits - k * 8; n.mask[k] = b >= 8 ? 0xff : b <= 0 ? 0 : (uint8_t)(0xff << (8 - b)); }
    return n;
}

int main() {
    std::printf("\nOne listener, both families\n");
    std::shared_ptr<net::Listener> lis;
    int port = 0;
    for (int p = 47311; p < 47400 && !lis; p++) { try { lis = net::listen("0.0.0.0", p); port = p; } catch (...) {} }
    ok((bool)lis, "an \"any address\" listener opens");
    if (lis) {
        const int c4 = dial(AF_INET, port);
        ok(c4 >= 0, "an IPv4 client connects (127.0.0.1)");
        auto s4 = lis->accept(nullptr, 2000);
        ok((bool)s4, "…and is accepted");
        if (s4) ok(s4->socketPeerAddress() == "127.0.0.1", "★ the IPv4 client reads as \"127.0.0.1\" — exactly as before",
                   s4->socketPeerAddress());
        if (s4) ok(s4->peerAddress() == "127.0.0.1", "…and peerAddress() agrees", s4->peerAddress());
        if (c4 >= 0) ::close(c4);
        if (hostHasIpv6()) {
            const int c6 = dial(AF_INET6, port);
            ok(c6 >= 0, "an IPv6 client connects ([::1]) to the SAME listener");
            auto s6 = lis->accept(nullptr, 2000);
            ok((bool)s6, "…and is accepted");
            if (s6) ok(s6->socketPeerAddress() == "::1", "the IPv6 client reads as \"::1\"", s6->socketPeerAddress());
            if (c6 >= 0) ::close(c6);
        } else {
            std::printf("   --   this host has no IPv6 loopback: the IPv6 client half is skipped\n");
        }
        bool threw = false;
        try { auto again = net::listen("0.0.0.0", port); } catch (...) { threw = true; }
        ok(threw, "★ a port already taken still FAILS (the 48000-48049 scan depends on it)");
#if defined(__linux__)
        // ★ Linux only: BSD/macOS SO_REUSEADDR lets a SPECIFIC address share a port with a wildcard listener (it did
        //   with the old IPv4 0.0.0.0 listener too), so this is a platform rule, not ours.
        threw = false;
        try { auto again = net::listen("127.0.0.1", port); } catch (...) { threw = true; }
        ok(threw, "…for an IPv4-loopback listener on it too");
#endif
    }
    std::printf("\nA named loopback stays loopback\n");
    {
        std::shared_ptr<net::Listener> lo; int lp = 0;
        for (int p = 47400; p < 47480 && !lo; p++) { try { lo = net::listen("127.0.0.1", p); lp = p; } catch (...) {} }
        ok((bool)lo, "a 127.0.0.1 listener opens");
        if (lo && hostHasIpv6()) {
            const int c6 = dial(AF_INET6, lp);
            ok(c6 < 0, "★ an IPv6 client CANNOT reach a 127.0.0.1 listener (local-only stays IPv4 loopback)");
            if (c6 >= 0) ::close(c6);
        }
    }

    std::printf("\nWhich IPv6 addresses are the local network\n");
    using namespace vibeip;
    ok(isLinkLocalOrUla("fe80::1") && isLinkLocalOrUla("febf:ffff::1"), "fe80::/10 is link-local");
    ok(!isLinkLocalOrUla("fec0::1"), "fec0:: is not");
    ok(isLinkLocalOrUla("fc00::1") && isLinkLocalOrUla("fdff::1"), "fc00::/7 is ULA");
    ok(!isLinkLocalOrUla("2a02:c7c:1234:5600::9") && !isLinkLocalOrUla("192.168.1.2") && !isLinkLocalOrUla("::ffff:10.0.0.1"),
       "a global address, an IPv4 address, a v4-mapped one: none of them");
    ok(isLinkLocalOrUla("fe80::1%eth0"), "a zone suffix is ignored");
    const std::vector<Net6> nets = { net6("2a02:c7c:1234:5600::1", 64), net6("fd00:1::1", 64) };
    ok(onLinkV6("2a02:c7c:1234:5600:a1b2:c3d4:e5f6:789", nets), "★ a global address in the box's own /64 IS on-link (the laptop beside it)");
    ok(!onLinkV6("2a02:c7c:1234:5601::9", nets), "the next /64 is not");
    ok(!onLinkV6("2001:db8::1", nets), "a stranger's address is not");
    ok(!onLinkV6("192.168.1.5", nets) && !onLinkV6("::ffff:192.168.1.5", nets), "IPv4 is never on-link here (IPv4 keeps its own rules)");
    ok(!onLinkV6("::1", localNets6()), "★ ::1 is never on-link (loopback interfaces are skipped)");

    std::printf("\nThe brute-force lockout key\n");
    ok(lockKey("203.0.113.9", nets) == "203.0.113.9", "IPv4: the address, as before");
    ok(lockKey("b:abcdef", nets) == "b:abcdef" && lockKey("", nets).empty(), "a non-address key is untouched");
    ok(lockKey("2001:db8:aa:bb:1:2:3:4", nets) == "2001:db8:aa:bb::/64", "★ a remote IPv6 client counts under its /64",
       lockKey("2001:db8:aa:bb:1:2:3:4", nets));
    ok(lockKey("2001:db8:aa:bb:ffff::1", nets) == lockKey("2001:db8:aa:bb:1:2:3:4", nets), "…so a new address per guess is the same key");
    ok(lockKey("2a02:c7c:1234:5600::77", nets) == "2a02:c7c:1234:5600::77", "a LAN global address keys on itself (one bad PIN locks out one device)");
    ok(lockKey("fe80::5", nets) == "fe80::5" && lockKey("fd00:1::5", nets) == "fd00:1::5", "link-local / ULA key on themselves");
    ok(lockKey("::ffff:10.0.0.1", nets) == "::ffff:10.0.0.1", "a v4-mapped text is left alone");

    std::printf("\ntest-ipclass: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
