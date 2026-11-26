// test-sattrack-restart.cpp — ★★★ beta96 THE SATELLITE TRACKER STOPS WITH THE SERVER (the XCover / Sony crashes, 11-25/26).
//
// On a phone the server runs INSIDE the app: a stop / restart (a settings save, a rate change, the app's own stop) deletes
// the shim's Impl and makes a new one in the same process. The Doppler tracker ("vibe-sat-track") was a DETACHED thread
// holding the Impl, so it kept ticking into freed memory — XCover SIGABRT "pthread_mutex_lock called on a destroyed mutex",
// Sony SIGSEGV, every one in that thread, while using SATELLITE (ORBCOMM in particular).
//
// This runs the REAL shim in this process against fake-rtl-tcp, START → an ORBCOMM pick (Doppler on, the decoder attached:
// the tracker runs) → STOP → START … with the tracker live at every stop, on the SHARED dial and then with a LISTENER's own
// lock (a locked centre: per-listener channels). After every start it checks the tracker thread exists; after every stop
// that NO tracker thread is left, and that the stop returned in bounded time (a join must not hang). Built under ASan
// (scripts/test-sattrack-restart.sh SAN=address) a tracker outliving its Impl is a heap-use-after-free report.
//
//   usage: test-sattrack-restart <fake-rtl-tcp port> <sats.json fixture> [starts per phase = 12]
#include "local_sdr_shim.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#else
#include <dirent.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using vibe::LocalSdrShim;
static int fails = 0, checks = 0;
static void ok(bool c, const std::string& what) { ++checks; if (!c) ++fails; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str()); std::fflush(stdout); }

/** How many of this process's threads carry `name` (the tracker names itself "vibe-sat-track"). */
static int threadsNamed(const char* name) {
    int c = 0;
#if defined(__APPLE__)
    thread_act_array_t th = nullptr; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return -1;
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
        if (pthread_t pt = pthread_from_mach_thread_np(th[i])) {
            char b[64] = {0};
            if (pthread_getname_np(pt, b, sizeof b) == 0 && std::strcmp(b, name) == 0) ++c;
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
#else
    if (DIR* d = ::opendir("/proc/self/task")) {
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::ifstream f(std::string("/proc/self/task/") + e->d_name + "/comm");
            std::string s; std::getline(f, s);
            if (s == name) ++c;
        }
        ::closedir(d);
    }
#endif
    return c;
}
template <class F> static bool until(F f, int ms) {
    for (int i = 0; i < ms / 50; ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    return f();
}

/** A minimal WebSocket client (text frames in, masked text frames out) — what a browser's spectrum socket does. */
struct Ws {
    int fd = -1;
    std::thread rd;
    std::mutex m;
    std::vector<std::string> txt;
    std::atomic<bool> open{false};
    bool connect(int port, const std::string& path) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); fd = -1; return false; }
        const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        (void)!::send(fd, req.data(), req.size(), 0);
        rd = std::thread([this] {
            std::string buf; char b[65536]; bool hs = false;
            for (;;) {
                const ssize_t n = ::recv(fd, b, sizeof b, 0);
                if (n <= 0) break;
                buf.append(b, (size_t)n);
                if (!hs) { const size_t j = buf.find("\r\n\r\n"); if (j == std::string::npos) continue; hs = true; open = true; buf.erase(0, j + 4); }
                for (;;) {
                    if (buf.size() < 2) break;
                    const int op = (unsigned char)buf[0] & 0x0f; uint64_t len = (unsigned char)buf[1] & 0x7f; size_t off = 2;
                    if (len == 126) { if (buf.size() < 4) break; len = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3]; off = 4; }
                    else if (len == 127) { if (buf.size() < 10) break; len = 0; for (int k = 0; k < 8; ++k) len = (len << 8) | (unsigned char)buf[2 + k]; off = 10; }
                    if (buf.size() < off + len) break;
                    if (op == 1) { std::lock_guard<std::mutex> lk(m); txt.emplace_back(buf.substr(off, (size_t)len)); if (txt.size() > 4000) txt.erase(txt.begin(), txt.begin() + 2000); }
                    buf.erase(0, off + (size_t)len);
                }
            }
            open = false;
        });
        return until([this] { return open.load(); }, 3000);
    }
    void send(const std::string& s) {
        std::string f; f += (char)0x81;
        if (s.size() < 126) f += (char)(0x80 | s.size());
        else { f += (char)(0x80 | 126); f += (char)((s.size() >> 8) & 0xff); f += (char)(s.size() & 0xff); }
        f.append(4, '\0');   // mask key 0
        f += s;
        (void)!::send(fd, f.data(), f.size(), 0);
    }
    std::string find(const std::string& needle) { std::lock_guard<std::mutex> lk(m); for (auto it = txt.rbegin(); it != txt.rend(); ++it) if (it->find(needle) != std::string::npos) return *it; return ""; }
    ~Ws() { if (fd >= 0) ::shutdown(fd, SHUT_RDWR); if (rd.joinable()) rd.join(); if (fd >= 0) ::close(fd); }
};

/** sat_list → the first ORBCOMM bird and the index of its orbcomm downlink (the picker's TRACK). */
static bool orbcommPick(const std::string& list, long long& norad, int& txi) {
    size_t p = 0;
    while ((p = list.find("\"name\":\"ORBCOMM", p)) != std::string::npos) {
        const size_t np = list.find("\"norad\":", p);
        const size_t tp = list.find("\"tx\":[", p);
        if (np == std::string::npos || tp == std::string::npos) return false;
        norad = std::atoll(list.c_str() + np + 8);
        // walk the tx array: count top-level objects until the one carrying "decoder":"orbcomm" and "reach":true
        int depth = 0, idx = -1; size_t objStart = 0;
        for (size_t i = tp + 6; i < list.size(); ++i) {
            const char ch = list[i];
            if (ch == '"') { for (++i; i < list.size() && list[i] != '"'; ++i) if (list[i] == '\\') ++i; continue; }
            if (ch == '{') { if (depth++ == 0) { ++idx; objStart = i; } }
            else if (ch == '}') {
                if (--depth == 0) {
                    const std::string o = list.substr(objStart, i - objStart + 1);
                    if (o.find("\"decoder\":\"orbcomm\"") != std::string::npos && o.find("\"reach\":true") != std::string::npos) { txi = idx; return true; }
                }
            } else if (ch == ']' && depth == 0) break;
        }
        p += 10;
    }
    return false;
}

static std::string slurp(const std::string& path) { std::ifstream f(path, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string isoAgo(double secs) {
    const std::time_t t = std::time(nullptr) - (std::time_t)secs;
    std::tm g{}; gmtime_r(&t, &g);
    char b[40]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &g);
    return b;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <fake port> <sats.json> [starts per phase]\n", argv[0]); return 2; }
    const int fakePort = std::atoi(argv[1]);
    const int starts = argc > 3 ? std::max(2, std::atoi(argv[3])) : 12;
    // ★ the whole run is bounded: a stop that hangs on the tracker's join is a FAIL, never a wedged suite
    std::thread([] { std::this_thread::sleep_for(std::chrono::minutes(8)); std::printf("   FAIL the run took over 8 minutes (a stop hung?)\n"); std::fflush(stdout); std::_Exit(4); }).detach();

    // ── the satellite data: vibesdr.net's sats.json fixture WITH the ORBCOMM birds, every epoch moved to an hour ago ──
    std::string sats = slurp(argv[2]);
    if (sats.empty()) { std::printf("   FAIL cannot read %s\n", argv[2]); return 1; }
    sats = std::regex_replace(sats, std::regex("\"EPOCH\"\\s*:\\s*\"[^\"]*\""), "\"EPOCH\":\"" + isoAgo(3600) + "\"");
    sats = std::regex_replace(sats, std::regex("\"fetched\"\\s*:\\s*\"[^\"]*\""), "\"fetched\":\"" + isoAgo(7200) + "Z\"");
    std::string err;
    const int nSats = LocalSdrShim::satLoadSats(sats, &err);
    ok(nSats > 0, "the fixture's satellites loaded (" + std::to_string(nSats) + " with elements" + (err.empty() ? "" : ", " + err) + ")");
    LocalSdrShim::setSatObserver(true, 52.27, -1.04);   // IO92ng — the server tests' locator

    char tmpl[] = "/tmp/vs-sattrack-XXXXXX";
    const std::string dir = ::mkdtemp(tmpl) ? tmpl : "/tmp";
    LocalSdrShim::setRateGuardBase(dir + "/rate-guard");
    LocalSdrShim::setVibeServerLanding(137.5e6, "nfm");
    LocalSdrShim::setVibeServerIdleGrace(0.0);
    LocalSdrShim::setVibeServerMaxUsers(4);

    struct Phase { const char* name; double lockedCentre; };
    const Phase phases[] = { { "shared dial", 0.0 }, { "a listener's own lock (locked centre)", 137.5e6 } };
    int stopsClean = 0, stopsTotal = 0, trackerUp = 0;
    double worstStop = 0;
    for (const Phase& ph : phases) {
        std::printf("── %s ──\n", ph.name);
        if (ph.lockedCentre > 0) { LocalSdrShim::setVibeServerLockedRate(2.4e6); LocalSdrShim::setVibeServerLockedCentre(ph.lockedCentre); }
        for (int n = 1; n <= starts; ++n) {
            const int port = LocalSdrShim::instance().startTcp("127.0.0.1", fakePort, 137.5e6, 2.4e6, -1, 1024, 10.0, "nfm", err);
            if (port <= 0) { ok(false, std::string(ph.name) + " start " + std::to_string(n) + ": " + err); break; }
            bool up = false; std::string why;
            {
                auto sp = std::make_unique<Ws>();
                char sess[32]; std::snprintf(sess, sizeof sess, "stk%02d%04d", (int)(ph.lockedCentre > 0), n);
                if (!sp->connect(port, std::string("/ws/user-spectrum?user_session_id=") + sess + "&bins=256&proto=1")) why = "spectrum socket did not open";
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                    sp->send("{\"type\":\"sat_list\"}");
                    std::string list;
                    until([&] { list = sp->find("\"type\":\"sat_list\""); return !list.empty(); }, 4000);
                    long long norad = 0; int txi = -1;
                    if (list.empty()) why = "no sat_list";
                    else if (!orbcommPick(list, norad, txi)) why = "no ORBCOMM bird with a reachable orbcomm downlink in sat_list";
                    else {
                        sp->send("{\"type\":\"sat\",\"op\":\"pick\",\"norad\":" + std::to_string(norad) + ",\"tx\":" + std::to_string(txi) + ",\"doppler\":true}");
                        up = until([] { return threadsNamed("vibe-sat-track") >= 1; }, 4000);
                        if (!up) why = "pick sent (" + std::to_string(norad) + " tx " + std::to_string(txi) + ") but no tracker thread; last sat: " + sp->find("\"type\":\"sat\"").substr(0, 160);
                    }
                }
                trackerUp += up ? 1 : 0;
                ok(up, std::string(ph.name) + ", start " + std::to_string(n) + ": an ORBCOMM pick with Doppler — the tracker runs" + (up ? "" : " (" + why + ")"));
                // ★ vary when the stop lands: at once, mid-tick, after a few hundred ms of tracking
                std::this_thread::sleep_for(std::chrono::milliseconds((n % 4) * 170));
                // ★ the socket stays OPEN through the stop (the tracker is still sending to it), as on a phone
                const auto t0 = std::chrono::steady_clock::now();
                LocalSdrShim::instance().stop();
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                worstStop = std::max(worstStop, secs);
                const bool gone = until([] { return threadsNamed("vibe-sat-track") == 0; }, 1000);
                ++stopsTotal; stopsClean += gone && secs < 20.0 ? 1 : 0;
                char line[200];
                std::snprintf(line, sizeof line, "%s, stop %d: no tracker thread outlives the stop (%d left), the stop took %.2f s", ph.name, n,
                              threadsNamed("vibe-sat-track"), secs);
                ok(gone && secs < 20.0, line);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100 + 60 * (n % 3)));
        }
        if (ph.lockedCentre > 0) { LocalSdrShim::setVibeServerLockedCentre(0.0); LocalSdrShim::setVibeServerLockedRate(0.0); }
    }
    std::printf("\ntracker ran in %d starts; %d of %d stops left no tracker behind; slowest stop %.2f s\n%d/%d checks passed\n",
                trackerUp, stopsClean, stopsTotal, worstStop, checks - fails, checks);
    std::string rm = "rm -rf '" + dir + "'"; (void)!std::system(rm.c_str());
    std::fflush(stdout);
    std::_Exit(fails ? 1 : 0);
}
