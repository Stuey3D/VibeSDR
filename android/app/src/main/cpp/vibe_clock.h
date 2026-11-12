// VibeSDR — the server's own idea of UTC, corrected by SNTP without touching the system clock.
//
// ★★★ WHY (Sony TV, 2026-09-19): the TV's clock read 2.64 s slow and Android left it there — it corrects
//     only errors above 5 s, once a day — and FT8 needs its 15 s slots within about a second: Stuart saw
//     ONE decode where the Pi next to it had dozens. An app may not set Android's clock, and a Pi with no
//     RTC can be just as wrong after a reboot, so the server measures its own offset and the decoders that
//     align to UTC read vibeUtcNow() instead of CLOCK_REALTIME.
// ★ Never steers the machine: the offset is ours alone, so nothing else on the box is surprised.
#pragma once
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace vibe {

/** Microseconds to ADD to the system clock to get UTC. 0 until the first good answer. */
inline std::atomic<int64_t>& clockOffsetUs() { static std::atomic<int64_t> v{0}; return v; }
/** Whether any SNTP answer has been accepted yet (so a log can say "unmeasured" rather than "exact"). */
inline std::atomic<bool>& clockMeasured() { static std::atomic<bool> v{false}; return v; }

/** ★★ THE SYSTEM CLOCK HAS JUST BEEN SET (V12, the R36S handheld, 2026-11-05) — by its owner, from a decoded time
 *  station or from FT8's DT, through the root hardware service. An offset measured against the OLD clock would now
 *  be added to a clock that is already right, so it goes: the decoders read the system clock as it stands. Called from
 *  a signal handler (SIGUSR2, sent by the handheld supervisor after a set) — two lock-free atomic stores, nothing else. */
inline void clockWasSet() {
    clockOffsetUs().store(0, std::memory_order_relaxed);
    clockMeasured().store(true, std::memory_order_relaxed);
}

/** UTC in seconds, corrected. The decoders' slot clock. */
inline double vibeUtcNow() {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9 + clockOffsetUs().load(std::memory_order_relaxed) / 1e6;
}

/** One SNTP (RFC 4330) exchange. Returns true and the offset (s, to ADD to local) + round trip (s).
 *  ★ Standard four-timestamp offset: ((t1 - t0) + (t2 - t3)) / 2. 2 s timeout; answers with a round
 *    trip over 0.5 s are refused by the caller, since half the trip is the error bound. */
inline bool sntpQuery(const char* host, double& offset, double& rtt) {
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, "123", &hints, &res) != 0 || !res) return false;
    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }
    timeval tv{2, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    auto now = [] { timespec t; clock_gettime(CLOCK_REALTIME, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; };
    constexpr double kNtpEpoch = 2208988800.0;   // 1900 -> 1970
    uint8_t pkt[48]{}; pkt[0] = 0x23;            // LI 0, version 4, mode 3 (client)
    const double t0 = now();
    { const double n = t0 + kNtpEpoch; const uint32_t s = (uint32_t)n, f = (uint32_t)((n - s) * 4294967296.0);
      const uint32_t be[2] = { htonl(s), htonl(f) }; memcpy(pkt + 40, be, 8); }   // transmit timestamp
    bool ok = ::sendto(fd, pkt, sizeof pkt, 0, res->ai_addr, res->ai_addrlen) == (ssize_t)sizeof pkt;
    freeaddrinfo(res);
    uint8_t r[48]{};
    ok = ok && ::recv(fd, r, sizeof r, 0) == (ssize_t)sizeof r;
    const double t3 = now();
    ::close(fd);
    if (!ok) return false;
    if ((r[0] & 0x07) != 4 || r[1] == 0 || r[1] > 15) return false;   // not a server reply / unsynchronised
    auto ts = [&](int o) { uint32_t s, f; memcpy(&s, r + o, 4); memcpy(&f, r + o + 4, 4);
                           return (double)ntohl(s) + ntohl(f) / 4294967296.0 - kNtpEpoch; };
    const double t1 = ts(32), t2 = ts(40);
    if (t1 <= 0 || t2 <= 0) return false;
    offset = ((t1 - t0) + (t2 - t3)) / 2.0;
    rtt = (t3 - t0) - (t2 - t1);
    return true;
}

/** Measure against a few public servers, keep the answer with the shortest round trip. */
inline bool refreshClockOffset(std::string& report) {
    static const char* const kHosts[] = { "time.cloudflare.com", "pool.ntp.org", "time.google.com" };
    double best = 0, bestRtt = 1e9; const char* from = nullptr;
    for (const char* h : kHosts) {
        double o, r;
        if (sntpQuery(h, o, r) && r >= 0 && r < 0.5 && r < bestRtt) { best = o; bestRtt = r; from = h; }
    }
    if (!from) { report = "no time server answered"; return false; }
    clockOffsetUs().store((int64_t)(best * 1e6), std::memory_order_relaxed);
    clockMeasured().store(true, std::memory_order_relaxed);
    char buf[160];
    snprintf(buf, sizeof buf, "this machine's clock is %+.3f s from UTC (%s, round trip %.0f ms) — decoders use corrected time",
             -best, from, bestRtt * 1e3);
    report = buf;
    return true;
}

/** Start the periodic measurement once per process; `log` is told each result. Detached — it only ever
 *  sleeps and sends a few UDP packets, and the process owns it for its whole life. */
inline void startClockWatch(std::function<void(const std::string&)> log) {
    static std::once_flag once;
    std::call_once(once, [log] {
        std::thread([log] {
            for (;;) {
                std::string rep;
                const bool ok = refreshClockOffset(rep);
                if (log) log(rep);
                // ★★ EVERY TEN MINUTES, not hourly: the Sony TV's clock DRIFTS about 0.7 s an hour (-2.66 s at
                //    15:21, -5.26 s at 18:58, 2026-09-19), so an hourly offset let FT8 wander ~0.7 s — most of
                //    its margin — between checks. One UDP exchange; every two minutes until the first answer.
                std::this_thread::sleep_for(std::chrono::seconds(ok ? 600 : 120));
            }
        }).detach();
    });
}

} // namespace vibe
