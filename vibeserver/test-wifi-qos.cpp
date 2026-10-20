// ★★★ WHICH WI-FI QUEUE EACH SOCKET GOES OUT IN (vibe_wifi_qos.h, docs/BRIEF-portable-wifi-realtime.md, 2026-10-20).
//     The audio socket is WMM voice (SO_PRIORITY 6 + DSCP EF); the waterfall and the map NEVER are. Checked on REAL
//     sockets (a loopback TCP pair, IPv4 and IPv6) by reading the options back — on Linux that includes the trap where
//     setting IP_TOS rewrites SO_PRIORITY, so the order inside apply() is what is under test.
#include "vibe_wifi_qos.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) {
    std::printf("  %s %s\n", c ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", w.c_str()); if (!c) fails++;
}
static void skip(const std::string& w) { std::printf("  \033[2mSKIP\033[0m %s\n", w.c_str()); }

/** A connected loopback TCP pair; returns the ACCEPTED end (what the server marks), -1 if the family is unavailable. */
static int acceptedSocket(int family, int& client, int& listener) {
    client = listener = -1;
    listener = ::socket(family, SOCK_STREAM, 0);
    if (listener < 0) return -1;
    sockaddr_storage ss{}; socklen_t len;
    if (family == AF_INET) {
        auto* a = (sockaddr_in*)&ss; a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); len = sizeof *a;
    } else {
        auto* a = (sockaddr_in6*)&ss; a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback; len = sizeof *a;
    }
    if (::bind(listener, (sockaddr*)&ss, len) != 0 || ::listen(listener, 1) != 0) return -1;
    len = sizeof ss;
    ::getsockname(listener, (sockaddr*)&ss, &len);
    client = ::socket(family, SOCK_STREAM, 0);
    if (client < 0 || ::connect(client, (sockaddr*)&ss, len) != 0) return -1;
    return ::accept(listener, nullptr, nullptr);
}

static int getInt(int fd, int level, int opt) {
    int v = -1; socklen_t l = sizeof v;
    return ::getsockopt(fd, level, opt, &v, &l) == 0 ? v : -1;
}

int main() {
    std::printf("wifi qos — audio is voice, the waterfall and the map never are\n");

    // ── the table ──
    using vibeqos::Class;
    const auto au = vibeqos::marksFor(Class::Audio), wf = vibeqos::marksFor(Class::Waterfall), bk = vibeqos::marksFor(Class::Bulk);
    ok(au.priority == 6 && au.tos == 0xB8 && au.nodelay, "audio: SO_PRIORITY 6 (802.1d voice), DSCP EF (0xB8), TCP_NODELAY");
    ok(wf.priority != 6 && wf.tos != 0xB8 && wf.priority <= 1 && (wf.tos == 0x00 || wf.tos == 0x20),
       "★★ TRAP: the waterfall is NEVER voice — priority 0, CS0");
    ok(bk.priority == 1 && bk.tos == 0x20, "map / bulk: priority 1 (background), CS1 (0x20)");
    ok((au.tos >> 2) == 46 && (bk.tos >> 2) == 8, "the TOS bytes are the DSCPs shifted into place (EF = 46, CS1 = 8)");

    for (int family : {AF_INET, AF_INET6}) {
        const char* fam = family == AF_INET ? "IPv4" : "IPv6";
        int cl, li;
        const int fd = acceptedSocket(family, cl, li);
        if (fd < 0) { skip(std::string(fam) + ": no loopback socket of this family here"); if (cl >= 0) ::close(cl); if (li >= 0) ::close(li); continue; }

        const auto a = vibeqos::apply(fd, Class::Audio);
        ok(a.family == family, std::string(fam) + ": the family is read off the socket itself");
        if (family == AF_INET) {
            ok(a.tos && getInt(fd, IPPROTO_IP, IP_TOS) == 0xB8, "IPv4 audio: IP_TOS reads back EF (0xB8)");
        } else {
#if defined(IPV6_TCLASS)
            ok(a.tclass && getInt(fd, IPPROTO_IPV6, IPV6_TCLASS) == 0xB8,
               "★ TRAP: IPv6 audio gets IPV6_TCLASS = EF (IP_TOS alone does nothing for a native v6 packet)");
#else
            skip("IPV6_TCLASS not defined on this platform");
#endif
        }
        ok(a.nodelay && getInt(fd, IPPROTO_TCP, TCP_NODELAY) != 0, std::string(fam) + " audio: TCP_NODELAY on");
#if defined(SO_PRIORITY)
        ok(a.priority && getInt(fd, SOL_SOCKET, SO_PRIORITY) == 6,
           std::string("★★ TRAP: ") + fam + " audio: SO_PRIORITY reads back 6 AFTER the TOS was set (Linux's IP_TOS rewrites "
           "it to 4 — the order in apply() is what keeps it; got " + std::to_string(getInt(fd, SOL_SOCKET, SO_PRIORITY)) + ")");
#else
        ok(!a.priority, std::string(fam) + ": no SO_PRIORITY on this platform — reported as not applied, not faked");
        skip("SO_PRIORITY readback (Linux only)");
#endif
        // re-marking the same socket as the waterfall really takes it OUT of voice
        const auto w = vibeqos::apply(fd, Class::Waterfall);
        if (family == AF_INET) ok(w.tos && getInt(fd, IPPROTO_IP, IP_TOS) == 0x00, "IPv4 waterfall: IP_TOS 0 (best effort)");
#if defined(IPV6_TCLASS)
        else ok(w.tclass && getInt(fd, IPPROTO_IPV6, IPV6_TCLASS) == 0x00, "IPv6 waterfall: IPV6_TCLASS 0 (best effort)");
#endif
#if defined(SO_PRIORITY)
        ok(getInt(fd, SOL_SOCKET, SO_PRIORITY) == 0, std::string(fam) + " waterfall: SO_PRIORITY 0 — never voice");
#endif
        const auto b = vibeqos::apply(fd, Class::Bulk);
        if (family == AF_INET) ok(b.tos && getInt(fd, IPPROTO_IP, IP_TOS) == 0x20, "IPv4 bulk: IP_TOS CS1 (0x20)");
#if defined(SO_PRIORITY)
        ok(getInt(fd, SOL_SOCKET, SO_PRIORITY) == 1, std::string(fam) + " bulk: SO_PRIORITY 1 (background)");
#endif
        ::close(fd); ::close(cl); ::close(li);
    }
    {
        const auto a = vibeqos::apply(-1, Class::Audio);
        ok(!a.tos && !a.tclass && !a.priority && !a.nodelay, "a closed descriptor: nothing applied, nothing thrown");
    }

    std::printf(fails ? "\033[31m%d failed\033[0m\n" : "\033[32mall passed\033[0m\n", fails);
    return fails ? 1 : 0;
}
