// ★★★ PHASE 2 — THE WATERFALL YIELDS TO THE AUDIO (vibeqos::AudioFirst), on a synthetic clock so it says the same
//     thing on any machine: yields while the audio is queued, one frame per 500 ms while it does, resumes only after
//     ~1 s clear, never flaps, treats a send stuck in the kernel as queued, one listener's audio never slows another's
//     waterfall.
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

#include <fcntl.h>

#include <cstdio>
#include <cstring>
#include <cmath>
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


// ── Phase 2 ──
/** Frames offered every `stepMs` from `from` to `to`; how many the gate let through. */
static int offered(vibeqos::AudioFirst& g, double from, double to, double stepMs = 50) {
    int n = 0;
    for (double t = from; t < to; t += stepMs) n += g.waterfallMaySend(t) ? 1 : 0;
    return n;
}

/** The audio writer reporting `ms` of audio queued every 20 ms from `from` to `to` (a frame per report). */
static void queued(vibeqos::AudioFirst& g, double from, double to, double ms, double rate) {
    for (double t = from; t < to; t += 20) g.noteAudio(t, ms / 1000.0 * rate, rate);
}

static void phase2() {
    std::printf("audio first — the waterfall yields while a listener's audio is queued\n");
    using vibeqos::AudioFirst;
    const double opus = 12000;          // 96 kbit/s stereo Opus, bytes/s
    const double pcm = 192000;          // 48 kHz stereo int16, bytes/s
    {
        AudioFirst g;
        for (double t = 0; t < 2000; t += 20) g.noteAudio(t, 0.03 * opus, opus);    // 30 ms in flight: a healthy link
        ok(offered(g, 0, 2000) == 40 && !g.yielding(), "a healthy link (30 ms of audio in flight): every waterfall frame goes (20 fps)");
    }
    {
        // ★★★ FOUND ON THE LIVE SERVER: the encoder hands over ~8 Opus frames per DSP block, written back to back, so the
        //     kernel briefly holds 160 ms of audio after every block on a perfectly healthy link. A one-report rule yielded
        //     on loopback with nothing wrong. Bursts every 160 ms, each draining within 5 ms:
        AudioFirst g;
        int let = 0;
        for (double b = 0; b < 3000; b += 160) {
            for (int k = 1; k <= 8; k++) g.noteAudio(b + k * 0.1, k * 0.02 * opus, opus);   // 20, 40 … 160 ms, within 1 ms
            g.noteAudio(b + 5, 0.01 * opus, opus);                                          // drained
        }
        for (double t = 0; t < 3000; t += 50) let += g.waterfallMaySend(t) ? 1 : 0;
        ok(!g.yielding() && g.episodes() == 0 && let == 60,
           "★★★ the encoder's bursts (8 frames = 160 ms written at once, drained in 5 ms): NEVER a yield — a burst is not a backlog");
    }
    {
        AudioFirst g;
        g.noteAudio(0, 0.20 * opus, opus);                                          // 200 ms queued…
        ok(!g.yielding(), "audio 200 ms queued for an instant: not yet (it must persist)");
        queued(g, 20, 200, 200, opus);                                              // …and still, 180 ms on
        ok(g.yielding() && g.episodes() == 1, "audio 200 ms queued and STAYING queued (≥ 150 ms): the waterfall YIELDS");
        double t = 0; int let = 0;
        for (; t < 3000; t += 50) { g.noteAudio(t, 0.25 * opus, opus); let += g.waterfallMaySend(t) ? 1 : 0; }
        ok(let == 6, "…while it stays queued, whole frames are dropped — one let through every 500 ms (6 in 3 s, not 60; got "
                     + std::to_string(let) + ")");
        ok(g.dropped() == 54, "…and the rest are dropped, counted (" + std::to_string(g.dropped()) + ")");
        // audio clears at 3000
        for (double u = 3000; u < 6000; u += 20) g.noteAudio(u, 0.02 * opus, opus);
        ok(offered(g, 3000, 3950, 50) <= 2 && g.yielding(), "★ clear for under a second: still yielding (no flap)");
        ok(g.waterfallMaySend(4001) && !g.yielding(), "clear for ~1 s: resumes");
        ok(offered(g, 4050, 5050) == 20, "…at FULL rate again (20 fps)");
    }
    {
        AudioFirst g;
        // busy 400 ms, clear 600 ms, busy, clear … — must not resume in the gaps
        int let = 0;
        for (int cycle = 0; cycle < 5; cycle++) {
            const double b = cycle * 1000.0;
            for (double t = b; t < b + 1000; t += 50) {
                g.noteAudio(t, (t < b + 400 ? 0.30 : 0.01) * opus, opus);
                let += g.waterfallMaySend(t) ? 1 : 0;
            }
        }
        ok(let <= 13 && let >= 9 && g.episodes() == 1, "★★ audio flapping (busy every second, clear 600 ms between): ONE yield episode, no "
                                          "resume in the gaps (" + std::to_string(let) + " frames in 5 s)");
    }
    {
        // the threshold is TIME, not bytes: the same 20 KB is a crisis for Opus and nothing for PCM
        AudioFirst a, b;
        for (double t = 0; t <= 200; t += 20) { a.noteAudio(t, 20000, opus); b.noteAudio(t, 20000, pcm); }
        ok(a.yielding() && !b.yielding(), "the threshold is in ms of audio, not bytes: 20 KB queued is 1.7 s of Opus (yield) "
                                         "but 104 ms of PCM (no yield)");
    }
    {
        AudioFirst a, b;
        queued(a, 0, 300, 100, pcm);         // 100 ms of PCM: under
        queued(b, 0, 300, 200, pcm);         // 200 ms of PCM: over
        ok(!a.yielding() && b.yielding(), "PCM at 192 KB/s: 100 ms queued goes on, 200 ms yields — same rule as Opus");
    }
    {
        AudioFirst g;
        for (double t = 0; t < 400; t += 20) g.noteAudio(t, 1e6, 0);
        ok(!g.yielding() && offered(g, 0, 1000) == 20, "no audio rate known yet: never yields (a rule cannot fire on a guess)");
    }
    {
        AudioFirst g;
        g.noteSendStart(1000);                                   // an audio send that blocks in the kernel
        ok(g.waterfallMaySend(1100), "an audio send under way for 100 ms: the waterfall still goes");
        ok(g.waterfallMaySend(1200) && g.yielding() && !g.waterfallMaySend(1250),
           "★★ …stuck for 200 ms with NO report (the writer is blocked): yields all the same");
        ok(offered(g, 1300, 3000) <= 4 && g.yielding(), "…and keeps yielding for as long as it stays stuck (1.7 s on)");
        g.noteSendEnd();
        g.noteAudio(3000, 0.01 * opus, opus);
        ok(offered(g, 3000, 3950) <= 2, "the send returns: still yielding for the clear second");
        ok(offered(g, 4050, 5050) == 20, "…then full rate");
    }
    {
        AudioFirst mine, theirs;
        queued(mine, 0, 300, 500, opus);
        ok(mine.yielding() && offered(theirs, 0, 1000) == 20, "one listener's queued audio never slows ANOTHER listener's waterfall");
    }
    {
        vibeqos::RateMeter r;
        for (double t = 0; t <= 5000; t += 20) r.note(t, 240);  // 240 B every 20 ms = 12 KB/s
        ok(std::abs(r.bytesPerSec() - 12000) < 300, "the audio's rate is measured (12 KB/s Opus → "
                                                    + std::to_string((int)r.bytesPerSec()) + ")");
        vibeqos::RateMeter z;
        z.note(0, 240);
        ok(z.bytesPerSec() == 0, "…and is 0 (never yields) until a full second has been seen");
    }
    // ── the kernel side, on real sockets ──
    {
        int cl, li;
        const int fd = acceptedSocket(AF_INET, cl, li);
        if (fd >= 0) {
#if defined(TCP_NOTSENT_LOWAT)
            ok(vibeqos::limitUnsent(fd) && getInt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT) == 32 * 1024,
               "the waterfall socket's unsent kernel queue is held to 32 KB (TCP_NOTSENT_LOWAT reads back)");
#else
            ok(!vibeqos::limitUnsent(fd), "no TCP_NOTSENT_LOWAT here: reported as not applied");
#endif
            // a peer that never reads: what we wrote and it has not acknowledged is visible to the gate
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            int rcv = 4096; ::setsockopt(cl, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
            char buf[8192]; std::memset(buf, 0, sizeof buf);
            for (int i = 0; i < 4096 && ::send(fd, buf, sizeof buf, 0) > 0; i++) {}
#if (defined(__linux__) && defined(SIOCOUTQ)) || defined(SO_NWRITE)
            ok(vibeqos::unsentBytes(fd) > 0, "a peer that stops reading: the kernel's queued bytes are seen ("
                                             + std::to_string(vibeqos::unsentBytes(fd)) + " B)");
#else
            ok(vibeqos::unsentBytes(fd) == 0, "no SIOCOUTQ / SO_NWRITE here: 0, the gate sees only our own queue");
#endif
            ok(vibeqos::unsentBytes(-1) == 0, "a closed descriptor: 0");
            ::close(fd); ::close(cl); ::close(li);
        } else skip("no loopback socket for the kernel-queue checks");
    }
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

    phase2();
    std::printf(fails ? "\033[31m%d failed\033[0m\n" : "\033[32mall passed\033[0m\n", fails);
    return fails ? 1 : 0;
}
