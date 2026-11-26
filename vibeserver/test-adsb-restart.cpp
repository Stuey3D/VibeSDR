// test-adsb-restart.cpp — KEPT-RUNNING ADS-B AT 2.56 MS/s MUST COME BACK AFTER AN IN-PROCESS RESTART (b96, Stuart 11-26).
//
// ★★★ THE SONY (Android Lite, an RTL on ADS-B, "Keep ADS-B running when nobody is connected", adsb256, base 2.4): "I change
//     a setting and restart VibeServer, the 2.56 MS/s mode complains and says 94 % dropped" — and no aircraft. Android's
//     Setup save restarts the engine IN THE SAME PROCESS (VibeServerOps.restartFromSettings: stop, then applyAndStart), as
//     the Mac app does; the stop left g_adsbMode and the old ADS-B service (built for 2.56) behind, so the next start never
//     entered ADS-B (adsbKeepTick: "already in ADS-B"), the radio sat at 2.4 on the landing, and the stale service was fed
//     2.4 MS/s: capture 94 %, nothing decoded. See endWholeRadioModesForStop in local_sdr_shim.cpp.
//
// ★ WHAT THIS DOES: the real shim, in this process, against fake-rtl-tcp replaying a 2.56 MS/s 1090 MHz clip (three
//   aircraft). START → kept-running ADS-B enters → STOP → START … N times. After EVERY start it asks the server, not
//   the log: the radio's rate (captureSpanHz = 2 560 000), and fresh ADS-B aircraft in /vibeserver/aircraft.json
//   (?since=3: heard in the last 3 s, so a row left over from the previous start cannot pass); and from the fake's own
//   log, that THIS start tuned the radio to 1090 MHz and asked for 2.56 MS/s (entered through the listener's door).
//
//   usage: test-adsb-restart <fake-rtl-tcp port> <fake log file> [starts=10]   (scripts/test-adsb-restart.sh runs it)
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
    const int starts = argc > 3 ? std::max(2, std::atoi(argv[3])) : 10;
    char tmpl[] = "/tmp/vs-adsbrestart-XXXXXX";
    const std::string dir = ::mkdtemp(tmpl) ? tmpl : "/tmp";

    // ★ The Sony's config as VibeServerBoot.applyAndStart applies it: landing FM, ADS-B landing + keep running, the rate
    //   switch and ADS-B at 2.56 on, the guard beside the config — set ONCE, as the statics outlive every start.
    LocalSdrShim::setRateGuardBase(dir + "/rate-guard");
    LocalSdrShim::setVibeServerDabRateBoost(true);
    LocalSdrShim::setVibeServerAdsb256(true);
    LocalSdrShim::setVibeServerLanding(96.1e6, "wfm");
    LocalSdrShim::setVibeServerLandingAdsb(true, true);
    LocalSdrShim::setVibeServerIdleGrace(0.0);

    int good = 0;
    for (int n = 1; n <= starts; ++n) {
        const std::streamoff mark = fileSize(fakeLog);
        std::string err;
        const int port = LocalSdrShim::instance().startTcp("127.0.0.1", fakePort, 96.1e6, 2.4e6, -1, 1024, 10.0, "wfm", err);
        if (port <= 0) { ok(false, "start " + std::to_string(n) + ": " + err); break; }
        // ★ varied timing: the first keep tick is ~2 s after the open; wait for the server's own answer, up to 12 s
        long long rateNow = -1, count = 0;
        bool tuned = false, boosted = false;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(12)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            rateNow = (long long)LocalSdrShim::instance().captureSpanHz();   // ★ the rate the radio runs (p->sampleRate)
            count = jsonNum(httpGet(port, "/vibeserver/aircraft.json?since=3"), "count");
            const std::string fl = fileFrom(fakeLog, mark);
            tuned = fl.find("tune 1090.") != std::string::npos;
            boosted = fl.find("sample rate 2560000") != std::string::npos;
            if (rateNow == 2560000 && count > 0 && tuned && boosted) break;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const bool fine = rateNow == 2560000 && count > 0 && tuned && boosted;
        good += fine ? 1 : 0;
        char line[256];
        std::snprintf(line, sizeof line, "start %d: radio %.3f MS/s, %lld aircraft heard in the last 3 s, this start tuned 1090 %s, asked 2.56 %s (%.1f s)",
                      n, rateNow / 1e6, count, tuned ? "yes" : "NO", boosted ? "yes" : "NO", secs);
        ok(fine, line);
        LocalSdrShim::instance().stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(200 + 150 * (n % 4)));   // ★ varied gaps between starts
    }
    std::printf("\n%d of %d starts came up in ADS-B at 2.56 MS/s with aircraft\n", good, starts);
    std::printf("%d/%d checks passed\n", checks - fails, checks);
    std::string rm = "rm -rf '" + dir + "'"; (void)!std::system(rm.c_str());
    std::fflush(stdout);
    std::_Exit(fails ? 1 : 0);   // ★ statics with threads (decoder pools) are not torn down at exit — as main.cpp ends
}
