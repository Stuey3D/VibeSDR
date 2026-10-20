// vibe_wifi_qos.h — which Wi-Fi queue each of our sockets goes out in, and when the waterfall gives way to the audio.
//
// docs/BRIEF-portable-wifi-realtime.md (2026-10-20). A Portable and the listener's phone were both on a 2.4 GHz phone
// hotspot; the phone's own browsing made the Portable's audio stutter.
//
// ── 1. MARKING (Phase 1) ─────────────────────────────────────────────────────────────────────────────────────────
// ★★★ THE AUDIO SOCKET GOES OUT AS VOICE (WMM AC_VO); THE WATERFALL AND THE MAP NEVER DO.
//     Set on the socket once its path is known — acceptWs() for /ws/audio and /ws/user-spectrum, the map routes for
//     bulk. Socket options belong to the SOCKET, not the descriptor, so a connection the front door handed over
//     (SCM_RIGHTS) is marked by the radio process that serves it, and one served in-process (a single-radio server,
//     Android) by the same line. Marking at the door instead would miss both of those.
//
//  ★★ WHO HONOURS WHICH MARK — measured from the kernel source, not assumed:
//     · brcmfmac (the Pi's onboard Wi-Fi, a FullMAC driver) uses skb->priority 1..7 AS the 802.1d tag and only
//       classifies by DSCP when it is 0 — so SO_PRIORITY 6 is TID 6 / AC_VO on a Pi whatever its kernel's DSCP table.
//     · mac80211 drivers (most USB dongles and PCIe cards) IGNORE SO_PRIORITY below 256 and classify by DSCP:
//       EF → UP 6 (AC_VO) with the RFC 8325 table, EF >> 3 = 5 (AC_VI) on older kernels. Either is above best effort.
//       (256+n would force it, but a priority above 6 needs CAP_NET_ADMIN, which the daemon does not have.)
//     · The NEXT hop — a phone's hotspot forwarding to a phone — sees only the DSCP. So both marks, always.
//  ★★ TRAP: ON LINUX, SETTING IP_TOS OVERWRITES sk_priority (ip_sock_set_tos → rt_tos2priority(EF) = 4). So the TOS
//     goes first and SO_PRIORITY second, or the priority we asked for is silently replaced. apply() does that order and
//     the test reads both back.
//  ★★ TRAP: IP_TOS does nothing for a native IPv6 packet — that needs IPV6_TCLASS. A v6 socket carrying v4-mapped
//     traffic takes IP_TOS on Linux. So an AF_INET6 socket gets BOTH; an AF_INET one gets IP_TOS only.
//  ★★ TRAP: NEVER the waterfall, a map tile or anything bulk as EF / priority 6. A high-volume flow in AC_VO wins
//     nearly every contention on the channel (the shortest AIFS and contention window) and starves everything else on it,
//     our own TCP ACKs included.
//  ★ Carriers and Cloudflare strip or ignore DSCP: no effect through the tunnel or across the internet. This is for the
//    local Wi-Fi hop(s), and harmless on Ethernet.
//
#pragma once
#include <algorithm>
#include <cstdint>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace vibeqos {

enum class Class { Audio, Waterfall, Bulk };

struct Marks {
    int priority;   // SO_PRIORITY (Linux); 6 = 802.1d "voice", 0 = classify by DSCP, 1 = background
    int tos;        // the whole TOS / traffic-class byte (DSCP << 2)
    bool nodelay;   // TCP_NODELAY (already set on every accepted socket by net_shim; restated for audio)
};

/** The brief's table. Control sockets are not marked at all (best effort, the default). */
inline Marks marksFor(Class c) {
    switch (c) {
        case Class::Audio:     return {6, 0xB8, true};   // EF (DSCP 46)
        case Class::Waterfall: return {0, 0x00, false};  // CS0 — best effort, explicitly
        case Class::Bulk:      return {1, 0x20, false};  // CS1 (DSCP 8) — background
    }
    return {0, 0, false};
}

/** What apply() managed, for the log and the test. */
struct Applied { bool tos = false, tclass = false, priority = false, nodelay = false; int family = 0; };

/** Mark one connected socket. Never fails the connection: every option is best effort (a platform without
 *  SO_PRIORITY, a socket that is not TCP), and the result only says which ones took. */
inline Applied apply(int fd, Class c) {
    Applied a;
    if (fd < 0) return a;
    const Marks m = marksFor(c);
    sockaddr_storage ss{}; socklen_t len = sizeof ss;
    if (::getsockname(fd, (sockaddr*)&ss, &len) == 0) a.family = ss.ss_family;
    const int tos = m.tos;
    // ★ TOS FIRST — see the TRAP above: on Linux it rewrites sk_priority, which SO_PRIORITY then sets properly.
    if (a.family == AF_INET || a.family == AF_INET6)
        a.tos = ::setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos) == 0;
#if defined(IPV6_TCLASS)
    if (a.family == AF_INET6)
        a.tclass = ::setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos) == 0;
#endif
#if defined(SO_PRIORITY)
    const int prio = m.priority;
    a.priority = ::setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof prio) == 0;
#endif
    if (m.nodelay) {
        const int one = 1;
        a.nodelay = ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0;
    }
    return a;
}

}  // namespace vibeqos
