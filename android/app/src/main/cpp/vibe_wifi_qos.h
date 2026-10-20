// vibe_wifi_qos.h — which Wi-Fi queue each of our sockets goes out in, and when the waterfall gives way to the audio.
//
// docs/BRIEF-portable-wifi-realtime.md (2026-10-20). A Portable and the listener's phone were both on a 2.4 GHz phone
// hotspot; the phone's own browsing made the Portable's audio stutter. Two halves, both here so they can be tested:
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
// ── 2. THE WATERFALL YIELDS TO THE AUDIO (Phase 2.1) ─────────────────────────────────────────────────────────────
// ★★★ WHEN A LISTENER'S AUDIO IS QUEUED, THEIR WATERFALL SLOWS. The audio and the waterfall are separate sockets, and
//     until now nothing decided between them: on a link short of airtime the waterfall's large frames sat in the same
//     driver queue in front of the audio's small ones. AudioFirst is the rule, per listener (session):
//       · the audio socket's writer reports, after every frame, how much audio is still unsent (our queue + the kernel's
//         send queue, SIOCOUTQ) in MILLISECONDS of audio — so the same rule fits 16 KB/s Opus and 192 KB/s PCM;
//       · above kYieldMs, continuously for kSustainMs (a burst from the encoder is not a backlog), the waterfall
//         YIELDS: whole frames are dropped, one let through every kYieldFrameMs so the picture visibly slows rather than freezing (a frozen waterfall reads as a dead server);
//       · it resumes at full rate only once the audio has been clear for kClearMs (~1 s), so it does not flap.
//     Pure logic on a caller-supplied clock: deterministic, tested in vibeserver/test-wifi-qos.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <linux/sockios.h>      // SIOCOUTQ
#endif

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

/** Bytes written to this socket that the peer has not yet acknowledged, sent or not (Linux SIOCOUTQ; macOS
 *  SO_NWRITE). 0 where neither exists — the gate then sees only our own queue. */
inline int unsentBytes(int fd) {
    if (fd < 0) return 0;
#if defined(__linux__) && defined(SIOCOUTQ)
    int n = 0;
    if (::ioctl(fd, SIOCOUTQ, &n) == 0 && n > 0) return n;
#elif defined(SO_NWRITE)
    int n = 0; socklen_t l = sizeof n;
    if (::getsockopt(fd, SOL_SOCKET, SO_NWRITE, &n, &l) == 0 && n > 0) return n;
#endif
    return 0;
}

/** ★ Hold the socket's UNSENT kernel bytes to `bytes` (TCP_NOTSENT_LOWAT, Linux 3.12+ and macOS). For the waterfall:
 *  newest-wins happens in our outbox, and an autotuned send buffer would otherwise park seconds of stale rows ahead of
 *  it — still contending for airtime after the audio-first gate has said stop. In-flight data is not limited, so
 *  throughput on a long internet path is unchanged. Returns whether it took. */
inline bool limitUnsent(int fd, int bytes = 32 * 1024) {
#if defined(TCP_NOTSENT_LOWAT)
    return fd >= 0 && ::setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &bytes, sizeof bytes) == 0;
#else
    (void)fd; (void)bytes; return false;
#endif
}

/** ★★★ The audio-first rule for ONE listener. Thread-safe: the audio writer reports, the spectrum path asks. */
class AudioFirst {
public:
    static constexpr double kYieldMs      = 150.0;   // audio queued beyond this ⇒ the waterfall yields
    static constexpr double kClearMs      = 1000.0;  // …until the audio has been clear this long
    static constexpr double kYieldFrameMs = 500.0;   // while yielding, one whole frame every this often (2 fps)
    /** ★★ …and it must STAY over for this long. The encoder hands the writer its packets in BURSTS (one DSP block = up
     *  to ~8 Opus frames written back to back), so for a moment right after a burst the kernel holds 160 ms of audio
     *  on a perfectly healthy link. Measured on the live server (test-server-audio-first.mjs): a one-report rule
     *  yielded on loopback with nothing wrong at all. A real backlog persists; a burst drains within a few ms. */
    static constexpr double kSustainMs    = 150.0;

    /** The audio writer, after each frame: `queuedBytes` still unsent (ours + the kernel's), `bytesPerSec` the audio's
     *  own rate (so the threshold is in time, not bytes). A rate of 0 (not known yet) never yields. */
    void noteAudio(double nowMs, double queuedBytes, double bytesPerSec) {
        std::lock_guard<std::mutex> lk(m_);
        const double ms = bytesPerSec > 0 ? queuedBytes * 1000.0 / bytesPerSec : 0.0;
        lastQueuedMs_ = ms;
        if (ms <= kYieldMs) { overSinceMs_ = -1; return; }
        if (overSinceMs_ < 0) overSinceMs_ = nowMs;
        if (nowMs - overSinceMs_ < kSustainMs) return;        // a burst, not (yet) a backlog
        if (!yielding_) { yielding_ = true; ++episodes_; lastLetMs_ = -1e18; }
        busySinceClearMs_ = nowMs;
    }

    /** ★★ The audio writer around each send. A send that BLOCKS (the kernel buffer full — the worst backlog there is)
     *  reports nothing until it returns, so a send under way for longer than kYieldMs counts as audio queued. Without
     *  this the waterfall would resume ~1 s into the very stall it should be yielding to. */
    void noteSendStart(double nowMs) { std::lock_guard<std::mutex> lk(m_); sending_ = true; sendStartMs_ = nowMs; }
    void noteSendEnd() { std::lock_guard<std::mutex> lk(m_); sending_ = false; }

    /** The spectrum path, per whole frame: send it (true) or drop it (false). */
    bool waterfallMaySend(double nowMs) {
        std::lock_guard<std::mutex> lk(m_);
        if (sending_ && nowMs - sendStartMs_ > kYieldMs) {          // an audio send stuck in the kernel
            if (!yielding_) { yielding_ = true; ++episodes_; lastLetMs_ = -1e18; }
            busySinceClearMs_ = nowMs;
            lastQueuedMs_ = nowMs - sendStartMs_;
        }
        if (yielding_ && nowMs - busySinceClearMs_ >= kClearMs) yielding_ = false;   // clear for ~1 s: resume
        if (!yielding_) return true;
        if (nowMs - lastLetMs_ >= kYieldFrameMs) { lastLetMs_ = nowMs; return true; }
        ++dropped_;
        return false;
    }

    bool yielding() const { std::lock_guard<std::mutex> lk(m_); return yielding_; }
    uint64_t dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    uint64_t episodes() const { std::lock_guard<std::mutex> lk(m_); return episodes_; }
    double lastQueuedMs() const { std::lock_guard<std::mutex> lk(m_); return lastQueuedMs_; }

private:
    mutable std::mutex m_;
    bool yielding_ = false;
    double overSinceMs_ = -1;       // the audio has been over kYieldMs continuously since (-1 = it is not)
    bool sending_ = false;          // an audio send is under way…
    double sendStartMs_ = 0;        //   …since
    double busySinceClearMs_ = 0;   // the last time the audio was over the threshold
    double lastLetMs_ = -1e18;      // the last frame let through while yielding
    double lastQueuedMs_ = 0;
    uint64_t dropped_ = 0, episodes_ = 0;
};

/** The audio's byte rate, smoothed — what turns "bytes queued" into "milliseconds queued". Fed by the writer. */
class RateMeter {
public:
    void note(double nowMs, double bytes) {
        if (t0_ < 0) { t0_ = nowMs; acc_ = 0; }
        acc_ += bytes;
        const double dt = nowMs - t0_;
        if (dt >= 1000.0) {
            const double r = acc_ * 1000.0 / dt;
            rate_ = rate_ <= 0 ? r : rate_ * 0.7 + r * 0.3;
            t0_ = nowMs; acc_ = 0;
        }
    }
    double bytesPerSec() const { return rate_; }
private:
    double t0_ = -1, acc_ = 0, rate_ = 0;
};

}  // namespace vibeqos
