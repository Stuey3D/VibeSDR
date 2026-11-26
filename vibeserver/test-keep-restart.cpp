// test-keep-restart.cpp — KEPT VDL2, KEPT ISM 868 AND KEPT AIS + DSC (beta96 b96-maritime) MUST COME BACK AFTER AN IN-PROCESS
// RESTART (b96-keeprun, after b96-adsbstart).
//
// ★★★ Android's Setup save (and the Mac app) restart the engine IN THE SAME PROCESS; before endWholeRadioModesForStop the
//     stop left the whole-radio mode flags set, so a kept mode read as "already on" and was never re-entered — the radio sat
//     on the landing frequency (the Sony's ADS-B case, test-adsb-restart). The landing map's kept modes (mapKeepTick) rely on
//     the same fix: this runs the real shim, in this process, against fake-rtl-tcp, START → kept mode → STOP → START …, and
//     after EVERY start asks the fake's own log that THIS start tuned the radio to the kept mode's band (VDL2 136.86 MHz,
//     then ISM 868.6 MHz on the Amateur map) — not the landing's 96.1.
//
//   usage: test-keep-restart <fake-rtl-tcp port> <fake log file> [starts=6]   (scripts/test-keep-restart.sh runs it)
#include "local_sdr_shim.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using vibe::LocalSdrShim;
static int fails = 0, checks = 0;
static void ok(bool c, const std::string& what) { ++checks; if (!c) ++fails; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str()); std::fflush(stdout); }

static std::string httpGet(int port, const std::string& path) {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return "";
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{2, 0}; ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (::connect(s, (sockaddr*)&a, sizeof a) != 0) { ::close(s); return ""; }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    (void)!::send(s, req.data(), req.size(), 0);
    std::string out; char b[8192];
    for (;;) { const ssize_t n = ::recv(s, b, sizeof b, 0); if (n <= 0) break; out.append(b, (size_t)n); if (out.size() > (4u << 20)) break; }
    ::close(s);
    const size_t h = out.find("\r\n\r\n");
    return h == std::string::npos ? "" : out.substr(h + 4);
}
static long long jsonNum(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = j.find(k);
    return p == std::string::npos ? -1 : std::atoll(j.c_str() + p + k.size());
}
static std::string fileFrom(const std::string& path, std::streamoff from) {
    std::ifstream f(path, std::ios::binary); if (!f) return "";
    f.seekg(0, std::ios::end); const std::streamoff end = f.tellg(); if (end <= from) return "";
    f.seekg(from); std::string s((size_t)(end - from), '\0'); f.read(&s[0], end - from); return s;
}
static std::streamoff fileSize(const std::string& path) { std::ifstream f(path, std::ios::binary | std::ios::ate); return f ? (std::streamoff)f.tellg() : 0; }

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <fake port> <fake log> [starts]\n", argv[0]); return 2; }
    const int fakePort = std::atoi(argv[1]);
    const std::string fakeLog = argv[2];
    const int starts = argc > 3 ? std::max(2, std::atoi(argv[3])) : 6;
    char tmpl[] = "/tmp/vs-keeprestart-XXXXXX";
    const std::string dir = ::mkdtemp(tmpl) ? tmpl : "/tmp";
    LocalSdrShim::setRateGuardBase(dir + "/rate-guard");
    LocalSdrShim::setVibeServerLanding(96.1e6, "wfm");
    LocalSdrShim::setVibeServerIdleGrace(0.0);
    int good = 0, total = 0;
    struct Phase { const char* map; const char* modes; const char* tune; const char* name; double rate = 2.4e6; const char* bg = nullptr; };
    /* ★★★ beta96 + the MARITIME combination kept (AIS + DSC, b96-maritime) on a radio listing 8 MS/s (the fake rtl_tcp given an
     *  RSP's rate list — VIBE_TEST_TCP_RATES, tests only) and started at 8 MS/s: every start back between channel 70 and AIS. */
    const Phase phases[] = { { "avmar", "vdl2", "tune 136.8", "kept VDL2" }, { "amateur", "ism:868", "tune 868.", "kept ISM 868" },
                             { "avmar", "aisdsc", "tune 159.", "kept AIS + DSC (8 MS/s)", 8.0e6 },
                             /* ★★★ b96-bgsplit BACKGROUND AIS with the PLAIN landing (96.1 WFM — Listen-Amateur's shape): no map at all,
                              *  the background choice alone (setVibeServerBackground, authoritative from here on — so this phase is LAST) */
                             { "", "", "tune 16", "background AIS, FM landing", 2.4e6, "ais" } };
    for (const Phase& ph : phases) {
        LocalSdrShim::setVibeServerMap(ph.map, ph.modes, !ph.bg);   // ★ set once per phase, as the boot does; the statics outlive every start
        if (ph.bg) LocalSdrShim::setVibeServerBackground(ph.bg);
        const int n0 = std::string(ph.modes) == "vdl2" || ph.bg ? starts : 2;
        if (ph.rate > 3e6) ::setenv("VIBE_TEST_TCP_RATES", "8000000,6000000,5000000,4000000,3000000,2048000,2000000", 1);
        else ::unsetenv("VIBE_TEST_TCP_RATES");
        for (int n = 1; n <= n0; ++n, ++total) {
            const std::streamoff mark = fileSize(fakeLog);
            std::string err;
            const int port = LocalSdrShim::instance().startTcp("127.0.0.1", fakePort, 96.1e6, ph.rate, -1, 1024, 10.0, "wfm", err);
            if (port <= 0) { ok(false, std::string(ph.name) + " start " + std::to_string(n) + ": " + err); break; }
            bool tuned = false;
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(12)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                tuned = fileFrom(fakeLog, mark).find(ph.tune) != std::string::npos;
                if (tuned) break;
            }
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            good += tuned ? 1 : 0;
            char line[200];
            std::snprintf(line, sizeof line, "%s, start %d: this start tuned the radio to the kept mode's band %s (%.1f s)", ph.name, n, tuned ? "yes" : "NO", secs);
            ok(tuned, line);
            LocalSdrShim::instance().stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(200 + 150 * (n % 4)));
        }
    }
    std::printf("\n%d of %d starts came up in their kept mode\n%d/%d checks passed\n", good, total, checks - fails, checks);
    std::string rm = "rm -rf '" + dir + "'"; (void)!std::system(rm.c_str());
    std::fflush(stdout);
    std::_Exit(fails ? 1 : 0);
}
