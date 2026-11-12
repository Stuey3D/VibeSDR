// handheld.cpp — the R36S handheld's engine supervisor. See handheld.h for the why, and
// docs/v12/R36S-HANDHELD-IMAGE.md §0.2 for the hardware service's side of the conversation.
//
// ── What runs where ────────────────────────────────────────────────────────────────────────────────────────────────
//  supervisor (this file)            127.0.0.1:48000, the USB watch, the radio process, per-radio memory, the card,
//                                    the hardware-service client (/run/vibe-hwd.sock), /handheld/* for the page
//  radio process                     `vibeserver --handheld-radio …` — the ordinary radio process in its handheld
//                                    profile (main.cpp g_handheldChild): loopback only, every control the owner's
//  hardware service (the image's)    root: backlight, volume, battery, power button, the clock
//
// ── The routes the page uses (all answered here, never handed to the radio) ────────────────────────────────────────
//  GET  /handheld/session            {"handheld":true,"radio":{…}|null,"state":…,"memory":{…}|null,"resetSeq":N,…}
//  POST /handheld/memory             {"serial":"…","prefs":{…}} — the page's per-radio settings, in RAM; on disk every
//                                    ~30 s while in use, on unplug and on shutdown (temp + rename)
//  GET  /handheld/events?since=N     long-poll (≤ 25 s): the hardware service's events and the radio's arrivals
//  GET  /handheld/device · POST /handheld/device   the hardware service, relayed (a fixed list of requests)
//  POST /handheld/clock              {"utcMs":…} or {"deltaMs":…} — set through the service, then the radio process
//                                    is told its old NTP offset no longer applies (SIGUSR2 → vibe::clockWasSet)
//  GET  /handheld/perf               CPU %, temperature, RAM, battery + time left, the engine's own CPU
//  GET  /handheld/card · GET/POST/DELETE /handheld/recordings[/<name>] · GET /handheld/import ·
//  POST /handheld/import/result · POST /handheld/export · POST /handheld/diagnostics
//  DELETE /handheld/gallery/<FOLDER/file>  a decoded picture on the card
//  POST /handheld/eibi/import        the newest sked-*.csv in the card's EiBi/ (or import/) becomes the schedule
#include "handheld.h"
#include "handheld_card.h"
#include "handheld_json.h"

#include "fd_passing.h"
#include "local_sdr_shim.h"
#include "radios.h"
#include "vibeserver_config.h"
#include "diag_scrub.h"
#include "eibi.h"
#include "vibe_mapgl.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace handheld {
namespace {

using hhjson::quote;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
double monoS() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string envOr(const char* k, const std::string& d) { const char* v = std::getenv(k); return v && *v ? v : d; }

// ── Options ─────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Opts {
    int port = 48000;
    std::string dataDir, hhDir, runDir, cardDir, hwdSock, resetFlag, fakeRadios, diagScript, self;
};
Opts g_o;

std::atomic<bool> g_stop{false};
void onSig(int) { g_stop = true; }

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fputs("VibeServer handheld: ", stdout);
    std::vprintf(fmt, ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    va_end(ap);
}

// ── Events, for the page (long-poll) ────────────────────────────────────────────────────────────────────────────────
// ★ A ring of the last 64, each with a sequence number: a page that was away for a moment catches up, one that was
//   away for long simply starts from now. Bounded RAM whatever happens.
std::mutex g_evMtx;
std::condition_variable g_evCv;
std::deque<std::pair<long long, std::string>> g_events;
long long g_evSeq = 0;
void pushEvent(const std::string& json) {
    {
        std::lock_guard<std::mutex> lk(g_evMtx);
        g_events.emplace_back(++g_evSeq, json);
        while (g_events.size() > 64) g_events.pop_front();
    }
    g_evCv.notify_all();
}

// ── The hardware service (vibe-hwd) ─────────────────────────────────────────────────────────────────────────────────
int unixConnect(const std::string& path, int timeoutMs) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fd < 0) return -1;
    sockaddr_un a{}; a.sun_family = AF_UNIX; std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return -1; }
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}
bool sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
#endif
        if (n < 0) { if (errno == EINTR) continue; return false; }
        off += (size_t)n;
    }
    return true;
}
/** One line, up to 64 KB; false on EOF/timeout. `carry` keeps what came after the newline. */
bool readLine(int fd, std::string& carry, std::string& line, int timeoutMs) {
    const double until = monoS() + timeoutMs / 1000.0;
    for (;;) {
        const size_t nl = carry.find('\n');
        if (nl != std::string::npos) { line = carry.substr(0, nl); carry.erase(0, nl + 1); return true; }
        if (carry.size() > 65536) return false;
        const int left = (int)((until - monoS()) * 1000);
        if (left <= 0) return false;
        pollfd p{ fd, POLLIN, 0 };
        const int r = ::poll(&p, 1, std::min(left, 1000));
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) { if (g_stop) return false; continue; }
        if (r < 0) return false;
        char b[4096];
        const ssize_t n = ::recv(fd, b, sizeof b, 0);
        if (n <= 0) return false;
        carry.append(b, (size_t)n);
    }
}

/** ★ One request, one reply, on a connection of its own — so a reply can never be mistaken for an event, and a slow
 *  service cannot hold the page's request longer than `timeoutMs`. */
bool hwdRequest(const std::string& line, std::string& reply, int timeoutMs = 4000) {
    const int fd = unixConnect(g_o.hwdSock, timeoutMs);
    if (fd < 0) { reply = "{\"ok\":false,\"error\":\"the hardware service is not running\"}"; return false; }
    std::string carry;
    const bool ok = sendAll(fd, line + "\n") && readLine(fd, carry, reply, timeoutMs);
    ::close(fd);
    if (!ok) reply = "{\"ok\":false,\"error\":\"the hardware service did not answer\"}";
    return ok;
}

// The service's last known state, for /handheld/perf and the battery estimate.
std::mutex g_hwMtx;
std::atomic<bool> g_hwdUp{false};
int g_batPct = -1; bool g_batChg = false;
std::deque<std::pair<double, int>> g_batHist;   // (mono s, pct) while discharging — the time-left estimate
void noteBattery(int pct, bool chg) {
    std::lock_guard<std::mutex> lk(g_hwMtx);
    if (pct < 0 || pct > 100) return;
    if (chg || pct > g_batPct + 1) g_batHist.clear();   // ★ charging, or a jump up: the slope starts again
    g_batPct = pct; g_batChg = chg;
    if (!chg) { const double t = monoS(); if (g_batHist.empty() || g_batHist.back().second != pct) g_batHist.emplace_back(t, pct);
                while (g_batHist.size() > 2 && t - g_batHist.front().first > 3600) g_batHist.pop_front(); }
}
/** ★ Minutes left at the slope of the last hour's discharge, -1 until it has fallen 2 % (a guess on less is noise). */
int batteryMinutesLeft() {
    std::lock_guard<std::mutex> lk(g_hwMtx);
    if (g_batChg || g_batPct < 0 || g_batHist.size() < 2) return -1;
    const auto& a = g_batHist.front(); const auto& b = g_batHist.back();
    const int drop = a.second - b.second;
    const double mins = (b.first - a.first) / 60.0;
    if (drop < 2 || mins <= 0) return -1;
    return (int)(b.second / (drop / mins));
}

void onShutdownEvent(const std::string& reason);   // below
void afterShutdownAck();

/** ★★ The event connection: subscribe, then read lines for as long as the service is up; reconnect every 2 s. The
 *  shutdown ack goes back on THIS connection (§0.2) — the service waits ≤ 5 s for it. */
void hwdEventLoop() {
    while (!g_stop) {
        const int fd = unixConnect(g_o.hwdSock, 2000);
        if (fd < 0) { if (g_hwdUp.exchange(false)) pushEvent("{\"event\":\"hwd\",\"up\":false}");
                      for (int i = 0; i < 20 && !g_stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        if (!sendAll(fd, "{\"subscribe\":true}\n")) { ::close(fd); continue; }
        if (!g_hwdUp.exchange(true)) { logf("hardware service connected (%s)", g_o.hwdSock.c_str()); pushEvent("{\"event\":\"hwd\",\"up\":true}"); }
        {   // ★ the battery figure from the start, not only from the first change
            std::string st; hhjson::Value v;
            if (hwdRequest("{\"get\":\"state\"}", st) && hhjson::parse(st, v)) {
                if (const auto* b = v.get("battery")) noteBattery((int)b->num("pct", -1), b->boolean("charging"));
            }
        }
        std::string carry, line;
        for (;;) {
            // ★ a long wait: events are rare. readLine returns false on EOF, so a dead service is noticed at once.
            if (!readLine(fd, carry, line, 3600 * 1000)) { if (g_stop) break; if (carry.size() > 65536) carry.clear(); else break; }
            hhjson::Value v;
            if (!hhjson::parse(line, v) || v.kind != hhjson::Value::Obj) continue;
            const std::string ev = v.str("event");
            if (ev.empty()) continue;                                   // the subscribe's own {"ok":true}
            if (ev == "battery" || ev == "battery_low") noteBattery((int)v.num("pct", -1), v.boolean("charging", ev == "battery" ? false : g_batChg));
            if (ev == "shutdown") {
                logf("shutdown from the hardware service (%s) — saving every radio's memory", v.str("reason").c_str());
                pushEvent(line);
                onShutdownEvent(v.str("reason"));
                sendAll(fd, "{\"ack\":\"shutdown\"}\n");
                afterShutdownAck();
                continue;
            }
            pushEvent(line);                                            // ★ as the service sent it — the page reads §0.2's fields
        }
        ::close(fd);
        if (g_hwdUp.exchange(false)) { logf("hardware service went away — reconnecting"); pushEvent("{\"event\":\"hwd\",\"up\":false}"); }
    }
}

// ── Per-radio memory ────────────────────────────────────────────────────────────────────────────────────────────────
struct Memory {
    std::string serial, driver, name;
    std::string prefs = "null";          // ★ the page's own JSON, raw — never re-serialised
    bool dirty = false, loaded = false;
};
std::mutex g_memMtx;
std::map<std::string, Memory> g_mem;     // by serial

std::string memPath(const std::string& serial)    { return g_o.hhDir + "/radio-" + hhcard::serialFile(serial) + ".json"; }
std::string enginePath(const std::string& serial) { return g_o.hhDir + "/radio-" + hhcard::serialFile(serial) + ".engine.json"; }

Memory& memLocked(const std::string& serial) {
    Memory& m = g_mem[serial];
    if (!m.loaded) {
        m.loaded = true; m.serial = serial;
        std::string text; hhjson::Value v;
        if (hhcard::readFile(memPath(serial), text, 4u << 20) && hhjson::parse(text, v) && v.kind == hhjson::Value::Obj) {
            if (const auto* p = v.get("prefs")) if (p->kind == hhjson::Value::Obj) m.prefs = p->raw;
            m.driver = v.str("driver"); m.name = v.str("name");
        }
    }
    return m;
}
bool saveMemLocked(Memory& m) {
    if (!m.dirty) return true;
    const std::string body = "{\"serial\":" + quote(m.serial) + ",\"driver\":" + quote(m.driver) + ",\"name\":" + quote(m.name)
                           + ",\"savedUtc\":" + std::to_string((long long)(nowMs() / 1000)) + ",\"prefs\":" + m.prefs + "}\n";
    std::string err;
    if (!hhcard::atomicWrite(memPath(m.serial), body, &err)) { logf("could not save %s's memory — %s", m.serial.c_str(), err.c_str()); return false; }
    m.dirty = false;
    return true;
}
void saveAllMemory(const char* why) {
    std::lock_guard<std::mutex> lk(g_memMtx);
    int n = 0;
    for (auto& kv : g_mem) if (kv.second.dirty && saveMemLocked(kv.second)) n++;
    if (n) logf("saved %d radio memor%s (%s)", n, n == 1 ? "y" : "ies", why);
}

// ── The radio process ───────────────────────────────────────────────────────────────────────────────────────────────
struct Radio { std::string serial, driver, name, tcp; int index = -1; };
bool sameRadio(const Radio& a, const Radio& b) { return a.serial == b.serial && a.driver == b.driver && a.tcp == b.tcp; }

std::mutex g_rMtx;
std::condition_variable g_rCv;
enum class St { None, Starting, Running } g_state = St::None;
Radio g_cur;                     // valid while state != None
pid_t g_child = -1;
std::string g_childSock;
double g_childSince = 0;
int g_fails = 0;
double g_retryAt = 0;
// ★ Start+Select: armed at start, spent on the radio that connects next; the toast's number moves when it is spent.
bool g_resetArmed = false;
long long g_resetSeq = 0;
std::string g_resetSerial;

const char* stName(St s) { return s == St::None ? "none" : s == St::Starting ? "starting" : "running"; }

std::string radioJsonLocked() {
    if (g_state == St::None) return "null";
    return "{\"serial\":" + quote(g_cur.serial) + ",\"driver\":" + quote(g_cur.driver) + ",\"name\":" + quote(g_cur.name) + "}";
}
void announceLocked() {
    pushEvent("{\"event\":\"radio\",\"state\":\"" + std::string(stName(g_state)) + "\",\"radio\":" + radioJsonLocked() + "}");
}

/** ★★ The radio process's own settings file: ONE radio, enabled and configured, on a machine that lists nowhere,
 *  advertises nowhere and never touches the CPU governor. Written once per radio and then the radio process's own —
 *  it persists its gain, the DAB block and station it was left on there, as every server's radio does. */
// ── Where the device is (no GPS, no internet — the owner says) ──────────────────────────────────────────────────────
/* ★★ THE DEVICE'S LOCATION, NOT A RADIO'S (Stuart 2026-11-05). One file, $DATA/handheld/location.json — a Maidenhead
 *  locator or exact lat/lon, or nothing (a fresh card). It is written into every radio's engine settings as the
 *  machine's "Where this receiver is" (locator / lat / lon, the same fields a server's setup page writes), so the radio
 *  process reads it exactly as a server does: the map centre, DAB transmitter distances, NDB / NAVTEX ranges, the ITU
 *  region, the locator in logs. A change is applied LIVE: the radio process is told (SIGUSR1) and republishes it. */
struct Location { std::string locator, lat, lon; };
std::string locPath() { return g_o.hhDir + "/location.json"; }
Location loadLocation() {
    Location l; std::string t; hhjson::Value v;
    if (hhcard::readFile(locPath(), t, 4096) && hhjson::parse(t, v) && v.kind == hhjson::Value::Obj) {
        l.locator = v.str("locator"); l.lat = v.str("lat"); l.lon = v.str("lon");
    }
    return l;
}
/** ★ A Maidenhead square of 4 or 6 characters ("IO92" / "IO92nh"), normalised "IO92nh"; "" if it is not one. */
std::string normLocator(const std::string& in) {
    std::string u; for (char c : in) if (c != ' ') u += c;
    if (u.size() != 4 && u.size() != 6) return "";
    for (int i = 0; i < 2; i++) { const char c = (char)std::toupper((unsigned char)u[i]); if (c < 'A' || c > 'R') return ""; u[i] = c; }
    for (int i = 2; i < 4; i++) if (!std::isdigit((unsigned char)u[i])) return "";
    if (u.size() == 6) for (int i = 4; i < 6; i++) { const char c = (char)std::tolower((unsigned char)u[i]); if (c < 'a' || c > 'x') return ""; u[i] = c; }
    return u;
}
/** The location into a radio's engine settings (the machine fields). True when the file changed. */
bool applyLocationTo(vsconfig::ServerConfig& srv, const Location& l) {
    if (srv.locator == l.locator && srv.lat == l.lat && srv.lon == l.lon) return false;
    srv.locator = l.locator; srv.lat = l.lat; srv.lon = l.lon;
    return true;
}

void ensureEngineConfig(const Radio& r) {
    const std::string path = enginePath(r.serial);
    vsconfig::ServerConfig srv; std::string err;
    if (hhcard::exists(path) && vsconfig::loadServer(path, srv, err) && !srv.radios.empty()) {
        // ★ an existing radio picks up a location set while it was unplugged
        if (applyLocationTo(srv, loadLocation()) && !vsconfig::saveServer(path, srv, err))
            logf("could not record the location for %s — %s", r.serial.c_str(), err.c_str());
        return;
    }
    srv = vsconfig::ServerConfig{};
    srv.configured = true;
    srv.fullMode = true;
    srv.sharing = vsconfig::Sharing::Local;
    srv.dirList = false; srv.tunnelResume = 0; srv.mdnsAdvertise = false;
    srv.cpuGovernor = "";                         // ★ schedutil is the image's choice (§0); the engine leaves it alone
    srv.name = "VibeSDR handheld";
    applyLocationTo(srv, loadLocation());
    srv.batteryPauseAt = 0;                       // ★ the hardware service owns the battery floor (3 %, §0.1)
    srv.maxRadiosPerIp = 0;                       // ★ one owner on 127.0.0.1, radio after radio (main.cpp says why)
    if (!r.serial.empty()) {
        vsconfig::RadioConfig rc;
        rc.serial = r.serial; rc.driver = r.driver; rc.label = r.name;
        rc.enabled = true; rc.configured = true;
        // ★ The page's SPECTRUM RATE goes to 20 real frames (DISPLAY); a server's radio defaults to a 10 fps ceiling.
        rc.maxFps = 20; rc.fftRate = 20;
        /* ★★ IT SIMPLY WORKS FROM A FRESH CARD (Stuart, 2026-11-08: "I would like the decoder rate boost to be default and
         *  VibeAGC also being default so that when a user fires a radio up from fresh install it simply works. Obviously
         *  Airspy handles its own gain"). A server keeps both opt-in — a weak shared box may not sustain the borrowed rate,
         *  and an AGC that can raise the gain above the owner's setting must be asked for there. The handheld is one
         *  owner's own radio on a known CPU, so both start ON (the HARDWARE/SDR pages still turn them off):
         *    DECODER RATE BOOST (dabRateBoost) — DAB 2.048 / ADS-B 2.4 MS/s borrowed while they run; without it ADS-B
         *      refused with "enable the boost" on a fresh card.
         *    VibeAGC on an RTL (rtlAgc). An SDRplay keeps rspVibeAgc 0 = automatic, which IS VibeAGC with the OpenRSP
         *      driver and keeps SDRplay's own API (which wedged under VibeAGC's write load) on its IF AGC. Airspy and
         *      HackRF: unchanged. */
        rc.dabRateBoost = true;
        if (r.driver == "rtlsdr") rc.rtlAgc = true;
        srv.radios.push_back(rc);
    }
    if (!vsconfig::saveServer(path, srv, err)) logf("could not write %s — %s", path.c_str(), err.c_str());
}

void applyResetLocked(const Radio& r) {
    if (!g_resetArmed) return;
    g_resetArmed = false;
    {
        std::lock_guard<std::mutex> lk(g_memMtx);
        g_mem.erase(r.serial);
    }
    ::unlink(memPath(r.serial).c_str());
    ::unlink(enginePath(r.serial).c_str());
    ::unlink((enginePath(r.serial) + ".bak").c_str());
    ::unlink((g_o.hhDir + "/radio-" + hhcard::serialFile(r.serial) + ".dab-scan.txt").c_str());   // ★ its last DAB scan (main.cpp)
    ::unlink((g_o.hhDir + "/radio-" + hhcard::serialFile(r.serial) + ".dab-last.txt").c_str());   // ★ and its last station per multiplex
    g_resetSeq = nowMs();
    g_resetSerial = r.serial;
    logf("Start+Select: %s (%s) back to its defaults", r.name.c_str(), r.serial.c_str());
    pushEvent("{\"event\":\"reset\",\"serial\":" + quote(r.serial) + ",\"seq\":" + std::to_string(g_resetSeq) + "}");
}

void startChildLocked(const Radio& r) {
    applyResetLocked(r);
    ensureEngineConfig(r);
    {   // ★ the memory record carries the radio's names, so a file read back on a PC says whose it is
        std::lock_guard<std::mutex> lk(g_memMtx);
        Memory& m = memLocked(r.serial);
        if (m.driver != r.driver || m.name != r.name) { m.driver = r.driver; m.name = r.name; }
    }
    const std::string sockName = hhcard::serialFile(r.serial.empty() ? r.driver : r.serial);
    g_childSock = g_o.runDir + "/" + sockName + ".sock";
    ::unlink(g_childSock.c_str());
    // ★ The radio process must bind A port (the shim always serves one) but nobody uses it — every connection comes
    //   through the hand-off. A free loopback port, asked of the kernel now: 48001 may well be someone else's.
    int childPort = g_o.port + 1;
    {
        const int t = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        socklen_t al = sizeof a;
        if (t >= 0 && ::bind(t, (sockaddr*)&a, sizeof a) == 0 && ::getsockname(t, (sockaddr*)&a, &al) == 0) childPort = ntohs(a.sin_port);
        if (t >= 0) ::close(t);
    }
    std::vector<std::string> args = { g_o.self, "--handheld-radio", "--port", std::to_string(childPort) };
    if (!r.tcp.empty()) { args.push_back("--tcp"); args.push_back(r.tcp); }
    else if (!r.serial.empty()) { args.push_back("--radio-serial"); args.push_back(r.serial); }
    else { args.push_back("--radio"); args.push_back(std::to_string(r.index)); }
    // ★★ 2.048 MS/s, not the server's 2.4 (Stuart, 2026-11-07, the battery): the lowest rate that still carries a DAB
    //    multiplex (1.536 MHz wide — 1.2 MS/s cannot), and ~15 % less of every per-sample job than 2.4. The radio's
    //    memory (POST /handheld/memory, vibe-hhui memory.h) still puts back the rate a person chose for it.
    args.push_back("--rate"); args.push_back("2048000");
    std::vector<std::string> env;
    for (char** e = environ; e && *e; e++) {
        const std::string kv = *e;
        if (kv.rfind("VIBESERVER_CONFIG=", 0) == 0 || kv.rfind("VIBESERVER_RUNTIME_DIR=", 0) == 0
            || kv.rfind("VIBESERVER_PARENT_PID=", 0) == 0 || kv.rfind("VIBE_HANDHELD_SERIAL=", 0) == 0
            || kv.rfind("VIBE_HANDHELD_SOCK=", 0) == 0 || kv.rfind("VIBESERVER_DATA_DIR=", 0) == 0
            || kv.rfind("VIBE_HANDHELD_RECDIR=", 0) == 0) continue;
        env.push_back(kv);
    }
    env.push_back("VIBESERVER_CONFIG=" + enginePath(r.serial));
    env.push_back("VIBESERVER_RUNTIME_DIR=" + g_o.runDir);
    env.push_back("VIBESERVER_DATA_DIR=" + g_o.dataDir);
    env.push_back("VIBESERVER_PARENT_PID=" + std::to_string((long)::getpid()));   // ★ macOS: dies with us (kqueue)
    env.push_back("VIBE_HANDHELD_SERIAL=" + r.serial);
    env.push_back("VIBE_HANDHELD_SOCK=" + g_childSock);                             // ★ where it takes our hand-offs
    // ★★ the card's Recordings/ — the radio plays a recording from there on the speaker it owns ({"type":"playRec"})
    if (!g_o.cardDir.empty()) env.push_back("VIBE_HANDHELD_RECDIR=" + g_o.cardDir + "/Recordings");
    std::vector<char*> av, ev;
    for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    for (auto& e : env) ev.push_back(const_cast<char*>(e.c_str()));
    ev.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
#if defined(__linux__)
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);       // ★ the radio never outlives the supervisor that owns its port
#endif
        ::execve(av[0], av.data(), ev.data());
        std::fprintf(stderr, "VibeServer handheld: could not start the radio process (%s)\n", std::strerror(errno));
        ::_exit(127);
    }
    if (pid < 0) { logf("fork failed — %s", std::strerror(errno)); return; }
    g_child = pid; g_cur = r; g_state = St::Starting; g_childSince = monoS();
    logf("%s %s (%s) plugged in — starting it (pid %d)", r.driver.c_str(), r.name.c_str(), r.serial.c_str(), (int)pid);
    announceLocked();
}

/** ★ Stop the radio process: SIGTERM (it saves what it keeps itself), up to `waitS`, then SIGKILL. Caller holds g_rMtx. */
void stopChildLocked(const char* why, double waitS) {
    if (g_child > 0) {
        ::kill(g_child, SIGTERM);
        const double until = monoS() + waitS;
        int st = 0;
        while (::waitpid(g_child, &st, WNOHANG) == 0) {
            if (monoS() > until) { ::kill(g_child, SIGKILL); ::waitpid(g_child, &st, 0); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        logf("radio process stopped (%s)", why);
    }
    g_child = -1;
    g_state = St::None;
    g_cur = Radio{};
    announceLocked();
    g_rCv.notify_all();
}

std::atomic<bool> g_shuttingDown{false};
/** ★★ THE SHUTDOWN EVENT: every radio's memory to disk (each file fsync'd), the file systems flushed — and THEN the
 *  ack (§0.2), well inside the service's 5 s. The radio process is stopped AFTER the ack: what it keeps itself (gain,
 *  the DAB block) it writes as it changes, not at exit, so nothing the owner needs waits on its teardown — and a slow
 *  radio library (the SDRplay API has hung on release before, main.cpp) must never cost the card its clean shutdown. */
void onShutdownEvent(const std::string& reason) {
    g_shuttingDown = true;
    saveAllMemory(("shutdown: " + reason).c_str());
    // ★ Every file we wrote is already fsync'd (atomicWrite). The card and the data partition are then flushed WHOLE —
    //   syncfs, not sync(): one file system each, bounded, where sync() waits on every disk the machine has.
#if defined(__linux__)
    for (const std::string& d : { g_o.hhDir, g_o.cardDir }) {
        if (d.empty()) continue;
        const int fd = ::open(d.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { ::syncfs(fd); ::close(fd); }
    }
#endif
}
void afterShutdownAck() {
    std::thread([] {
        std::lock_guard<std::mutex> lk(g_rMtx);
        stopChildLocked("shutdown", 2.0);
    }).detach();
}

// ── What is plugged in ──────────────────────────────────────────────────────────────────────────────────────────────
bool supportedDriver(const std::string& d) {
    return d == "rtlsdr" || d == "airspyhf" || d == "airspy" || d == "sdrplay" || d == "hackrf" || d == "usrp";
}
/** ★ Test seam (VIBE_HANDHELD_FAKE_RADIOS): a file whose first line "serial driver host:port" IS the plugged-in radio
 *  (an rtl_tcp server — vibeserver/fake-rtl-tcp.mjs), and an empty file is "nothing plugged in". */
bool fakeRadio(Radio& out) {
    std::string text;
    if (!hhcard::readFile(g_o.fakeRadios, text, 4096)) return false;
    const size_t nl = text.find('\n');
    const std::string line = text.substr(0, nl);
    char s[128] = {0}, d[32] = {0}, t[128] = {0};
    if (std::sscanf(line.c_str(), "%127s %31s %127s", s, d, t) != 3) return false;
    out.serial = s; out.driver = d; out.tcp = t; out.name = std::string("Test ") + d;
    return true;
}

#if defined(__linux__)
/** ★★ CHEAP, AND IT TOUCHES NO DEVICE: the names in /sys/bus/usb/devices change when anything is plugged or pulled, so
 *  the bus is only counted (usbBusCounts — descriptors, never an open) when they do. "Repeated USB probing has
 *  disturbed running radios before" (main.cpp) — this never opens anything. */
std::string usbSignature() {
    std::string sig;
    DIR* d = ::opendir("/sys/bus/usb/devices");
    if (!d) return "?";
    std::vector<std::string> n;
    while (dirent* e = ::readdir(d)) if (e->d_name[0] != '.') n.push_back(e->d_name);
    ::closedir(d);
    std::sort(n.begin(), n.end());
    for (auto& x : n) sig += x + ",";
    return sig;
}
#endif

int countSupported(const vibe::UsbBusCounts& c) {
    return c.ok ? c.rtlsdr + c.sdrplay + c.airspyhf + c.hackrf + c.airspy + c.usrp : 0;
}

// ── The watch: one loop decides what should be running ──────────────────────────────────────────────────────────────
void watchLoop() {
    std::string lastSig;
    double lastCount = 0, lastSave = monoS();
    bool busPresent = false;
    vibe::UsbBusCounts counts;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const double now = monoS();
        // ★ 1) A radio process that ended by itself.
        {
            std::lock_guard<std::mutex> lk(g_rMtx);
            if (g_child > 0) {
                int st = 0;
                if (::waitpid(g_child, &st, WNOHANG) == g_child) {
                    const double ran = now - g_childSince;
                    logf("radio process ended (%s %d) after %.0f s", WIFEXITED(st) ? "exit" : "signal",
                         WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st), ran);
                    g_child = -1; g_state = St::None; g_cur = Radio{};
                    g_fails = ran > 60 ? 1 : g_fails + 1;
                    g_retryAt = now + std::min(30.0, 2.0 * (1 << std::min(g_fails - 1, 4)));
                    announceLocked();
                    g_rCv.notify_all();
                    saveAllMemory("the radio process ended");   // ★ g_memMtx only — taken after g_rMtx, as everywhere
                }
            }
            // ★ 2) Starting → running, once its hand-off socket answers.
            if (g_state == St::Starting) {
                const int fd = unixConnect(g_childSock, 200);
                if (fd >= 0) { ::close(fd); g_state = St::Running; g_fails = 0; logf("%s is running", g_cur.name.c_str()); announceLocked(); g_rCv.notify_all(); }
                else if (now - g_childSince > 40) logf("the radio process has not opened its socket after 40 s");
            }
        }
        // ★ 3) What is on the bus.
        Radio want; bool have = false;
        if (!g_o.fakeRadios.empty()) {
            have = fakeRadio(want);
        } else {
#if defined(__linux__)
            const std::string sig = usbSignature();
            if (sig != lastSig || now - lastCount > 10) { lastSig = sig; lastCount = now; counts = vibe::usbBusCounts(); }
#else
            if (now - lastCount > 2) { lastCount = now; counts = vibe::usbBusCounts(); }
#endif
            busPresent = countSupported(counts) > 0;
            std::lock_guard<std::mutex> lk(g_rMtx);
            if (g_state != St::None) {
                // ★ The running radio's kind gone from the bus = it was pulled. (An unknown count is not "gone".)
                const int n = counts.forDriver(g_cur.driver);
                have = n != 0; want = g_cur;
            } else have = busPresent;
        }
        std::unique_lock<std::mutex> lk(g_rMtx);
        if (g_state != St::None) {
            if (!have || (!g_o.fakeRadios.empty() && !sameRadio(want, g_cur))) {
                // ★★ UNPLUGGED: save first, then "Please connect an SDR".
                const std::string was = g_cur.name;
                lk.unlock();
                saveAllMemory("unplugged");
                lk.lock();
                stopChildLocked(("unplugged: " + was).c_str(), 4.0);
                g_fails = 0; g_retryAt = 0;
            }
        } else if (have && now >= g_retryAt && !g_shuttingDown) {
            if (g_o.fakeRadios.empty()) {
                // ★ Only now is a radio NAMED — one enumeration, the same one --radio uses.
                lk.unlock();
                const auto found = vibe::detectRadios();
                lk.lock();
                bool picked = false;
                for (const auto& d : found) {
                    if (!supportedDriver(d.driver)) continue;
                    want = Radio{}; want.driver = d.driver; want.serial = d.serial; want.name = d.name; want.index = d.index;
                    picked = true; break;
                }
                if (!picked) { g_retryAt = now + 2; continue; }
            }
            if (g_state == St::None) startChildLocked(want);
        }
        lk.unlock();
        // ★ 4) Every ~30 s while in use, the page's latest memory goes to disk (only if it changed).
        if (now - lastSave >= 30) { lastSave = now; saveAllMemory("every 30 s"); }
    }
}

// ── Perf figures (Linux /proc and /sys; elsewhere what we can) ──────────────────────────────────────────────────────
struct Perf { double cpu = -1, tempC = -1, engineCpu = -1; long long ramUsedMb = -1, ramTotalMb = -1; int cores = 1; };
std::mutex g_perfMtx;
Perf g_perf;
void perfLoop() {
#if defined(__linux__)
    unsigned long long lastTot = 0, lastIdle = 0, lastChild = 0; pid_t lastPid = -1;
    const long tck = ::sysconf(_SC_CLK_TCK);
    while (!g_stop) {
        Perf p;
        p.cores = (int)std::max(1L, ::sysconf(_SC_NPROCESSORS_ONLN));
        unsigned long long tot = 0, idle = 0;
        if (FILE* f = std::fopen("/proc/stat", "r")) {
            unsigned long long v[10] = {0};
            if (std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
                for (int i = 0; i < 8; i++) tot += v[i];
                idle = v[3] + v[4];
            }
            std::fclose(f);
        }
        unsigned long long childT = 0; pid_t pid;
        { std::lock_guard<std::mutex> lk(g_rMtx); pid = g_child; }
        if (pid > 0) {
            // ★★ NOT hhcard::readFile: it sizes the read by fstat, and every /proc file reports st_size 0 — the line came
            //    back empty, utime+stime read 0, and DSP showed 0 % on the R36S for ever (Stuart, 2026-11-07).
            std::string st;
            if (FILE* sf = std::fopen(("/proc/" + std::to_string((int)pid) + "/stat").c_str(), "r")) {
                char line[1024];
                if (std::fgets(line, sizeof line, sf)) st = line;
                std::fclose(sf);
            }
            if (!st.empty()) {
                const size_t rp = st.rfind(')');
                if (rp != std::string::npos) {
                    unsigned long long ut = 0, stt = 0;
                    // fields after ')': state(3) ... utime(14) stime(15)
                    std::sscanf(st.c_str() + rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &stt);
                    childT = ut + stt;
                }
            }
        }
        if (lastTot && tot > lastTot) {
            const double dt = (double)(tot - lastTot);
            p.cpu = 100.0 * (1.0 - (double)(idle - lastIdle) / dt);
            if (pid > 0 && pid == lastPid && childT >= lastChild) p.engineCpu = 100.0 * (double)(childT - lastChild) / dt;
        }
        lastTot = tot; lastIdle = idle; lastChild = childT; lastPid = pid;
        // temperature: the hottest thermal zone
        for (int z = 0; z < 8; z++) {
            std::string t;
            if (!hhcard::readFile("/sys/class/thermal/thermal_zone" + std::to_string(z) + "/temp", t, 64)) continue;
            const double c = std::atof(t.c_str()) / 1000.0;
            if (c > p.tempC && c < 150) p.tempC = c;
        }
        if (FILE* f = std::fopen("/proc/meminfo", "r")) {
            char k[64]; long long v; long long tot2 = -1, avail = -1;
            while (std::fscanf(f, "%63s %lld kB\n", k, &v) == 2) {
                if (!std::strcmp(k, "MemTotal:")) tot2 = v; else if (!std::strcmp(k, "MemAvailable:")) avail = v;
            }
            std::fclose(f);
            if (tot2 > 0 && avail >= 0) { p.ramTotalMb = tot2 / 1024; p.ramUsedMb = (tot2 - avail) / 1024; }
        }
        (void)tck;
        { std::lock_guard<std::mutex> lk(g_perfMtx); g_perf = p; }
        for (int i = 0; i < 10 && !g_stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#else
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(500));
#endif
}

// ── HTTP ────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Req {
    int fd = -1;
    std::string method, target, path, query, head;   // head = everything up to the blank line
    long long clen = 0;
    std::string body;          // already read (for small bodies) — or the leftover after the headers (big uploads)
};

std::string qv(const std::string& query, const std::string& key) {
    size_t p = 0;
    while (p <= query.size()) {
        size_t amp = query.find('&', p); if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(p, amp - p);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) {
            const std::string v = eq == std::string::npos ? "" : kv.substr(eq + 1);
            std::string o;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == '+') o += ' ';
                else if (v[i] == '%' && i + 2 < v.size() && std::isxdigit((unsigned char)v[i + 1]) && std::isxdigit((unsigned char)v[i + 2])) {
                    o += (char)std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
                } else o += v[i];
            }
            return o;
        }
        p = amp + 1;
    }
    return "";
}
std::string pctDecode(const std::string& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '%' && i + 2 < v.size() && std::isxdigit((unsigned char)v[i + 1]) && std::isxdigit((unsigned char)v[i + 2])) {
            o += (char)std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
        } else o += v[i];
    }
    return o;
}
std::string header(const std::string& head, const char* name) {
    std::string lower = head; for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    std::string key = std::string("\n") + name + ":";
    for (auto& c : key) c = (char)std::tolower((unsigned char)c);
    const size_t p = lower.find(key);
    if (p == std::string::npos) return "";
    size_t a = p + key.size(); while (a < head.size() && (head[a] == ' ' || head[a] == '\t')) a++;
    const size_t e = head.find('\r', a);
    return head.substr(a, e == std::string::npos ? std::string::npos : e - a);
}

void reply(int fd, int code, const char* status, const std::string& type, const std::string& body, const std::string& extra = "") {
    const std::string h = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\nContent-Type: " + type
        + "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n";
    sendAll(fd, h);
    sendAll(fd, body);
}
void json(int fd, const std::string& body, int code = 200) {
    reply(fd, code, code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : code == 413 ? "Payload Too Large"
          : code == 503 ? "Service Unavailable" : "Error", "application/json", body);
}
std::string errJson(const std::string& e) { return "{\"ok\":false,\"error\":" + quote(e) + "}"; }

/** Read the rest of the request head (after the peek) and a body of up to `cap` bytes. */
bool readRequest(Req& r, size_t cap) {
    std::string buf;
    char b[8192];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        if (buf.size() > 32768) return false;
        pollfd p{ r.fd, POLLIN, 0 };
        if (::poll(&p, 1, 8000) <= 0) return false;
        const ssize_t n = ::recv(r.fd, b, sizeof b, 0);
        if (n <= 0) return false;
        buf.append(b, (size_t)n);
    }
    const size_t he = buf.find("\r\n\r\n");
    r.head = buf.substr(0, he + 2);
    r.body = buf.substr(he + 4);
    const std::string cl = header(r.head, "Content-Length");
    r.clen = cl.empty() ? 0 : std::atoll(cl.c_str());
    if (r.clen < 0) return false;
    if ((size_t)r.clen > cap) return true;                        // ★ the caller streams it (or refuses)
    while ((long long)r.body.size() < r.clen) {
        pollfd p{ r.fd, POLLIN, 0 };
        if (::poll(&p, 1, 8000) <= 0) return false;
        const ssize_t n = ::recv(r.fd, b, sizeof b, 0);
        if (n <= 0) return false;
        r.body.append(b, (size_t)n);
    }
    if ((long long)r.body.size() > r.clen) r.body.resize((size_t)r.clen);
    return true;
}

std::string cardSub(const char* sub) { return g_o.cardDir.empty() ? std::string() : g_o.cardDir + "/" + sub; }

// ── Route handlers ──────────────────────────────────────────────────────────────────────────────────────────────────
void hSession(Req& r) {
    std::string radio, state, memory = "null", resetSerial; long long resetSeq;
    {
        std::lock_guard<std::mutex> lk(g_rMtx);
        radio = radioJsonLocked(); state = stName(g_state); resetSeq = g_resetSeq; resetSerial = g_resetSerial;
        if (g_state != St::None) {
            std::lock_guard<std::mutex> mk(g_memMtx);
            memory = memLocked(g_cur.serial).prefs;
        }
    }
    long long seq; { std::lock_guard<std::mutex> lk(g_evMtx); seq = g_evSeq; }
    const auto sp = hhcard::spaceAt(g_o.cardDir);
    json(r.fd, "{\"handheld\":true,\"state\":\"" + state + "\",\"radio\":" + radio + ",\"memory\":" + memory
         + ",\"resetSeq\":" + std::to_string(resetSeq) + ",\"resetSerial\":" + quote(resetSerial)
         + ",\"eventSeq\":" + std::to_string(seq) + ",\"hwd\":" + (g_hwdUp.load() ? "true" : "false")
         + ",\"card\":" + (sp.ok && !g_o.cardDir.empty() ? "true" : "false") + "}");
}

void hMemory(Req& r) {
    hhjson::Value v;
    if (!hhjson::parse(r.body, v) || v.kind != hhjson::Value::Obj) { json(r.fd, errJson("not JSON"), 400); return; }
    const std::string serial = v.str("serial");
    const hhjson::Value* p = v.get("prefs");
    if (!p || p->kind != hhjson::Value::Obj) { json(r.fd, errJson("prefs must be an object"), 400); return; }
    {
        // ★★ ONLY FOR THE RADIO THAT IS PLUGGED IN. A page posting late, after its radio was pulled and another
        //    plugged in, must not write the first radio's settings into the second's memory.
        std::lock_guard<std::mutex> lk(g_rMtx);
        if (g_state == St::None || serial != g_cur.serial) { json(r.fd, errJson("that radio is not plugged in"), 409); return; }
        std::lock_guard<std::mutex> mk(g_memMtx);
        Memory& m = memLocked(serial);
        if (m.prefs != p->raw) { m.prefs = p->raw; m.dirty = true; }
    }
    json(r.fd, "{\"ok\":true}");
}

void hEvents(Req& r) {
    const long long since = std::atoll(qv(r.query, "since").c_str());
    std::string out;
    long long seq;
    {
        std::unique_lock<std::mutex> lk(g_evMtx);
        g_evCv.wait_for(lk, std::chrono::seconds(25), [&] { return g_evSeq > since || g_stop.load(); });
        seq = g_evSeq;
        for (auto& e : g_events) if (e.first > since) out += (out.empty() ? "" : ",") + e.second;
    }
    json(r.fd, "{\"seq\":" + std::to_string(seq) + ",\"events\":[" + out + "]}");
}

void hDeviceGet(Req& r) {
    std::string st;
    hwdRequest("{\"get\":\"state\"}", st);
    hhjson::Value v; if (!hhjson::parse(st, v)) st = errJson("bad reply from the hardware service");
    json(r.fd, "{\"state\":" + st + "}");
}

/** ★★ A FIXED LIST, relayed as JSON we BUILD — never the page's line passed through.
 *  ★ No SSH (Stuart, 2026-11-05: "no SSH on the handheld at all") — there is nothing to switch on and no password. */
void hDevicePost(Req& r) {
    hhjson::Value v;
    if (!hhjson::parse(r.body, v) || v.kind != hhjson::Value::Obj) { json(r.fd, errJson("not JSON"), 400); return; }
    std::string line;
    const std::string set = v.str("set"), action = v.str("action");
    if (set == "brightness" || set == "volume") {
        const hhjson::Value* x = v.get("value");
        if (!x || x->kind != hhjson::Value::Num) { json(r.fd, errJson("value must be a number"), 400); return; }
        long n = (long)x->n;
        if (set == "volume") n = std::max(0L, std::min(100L, n));
        else n = std::max(1L, std::min(255L, n));                     // ★ a floor above 0: never a black screen by slider
        line = "{\"set\":\"" + set + "\",\"value\":" + std::to_string(n) + "}";
    } else if (set == "boost") {
        // ★★ the map's performance boost (vibe-hwd set_boost): every cap lifted while the map is up — 1 / 0, never saved
        const hhjson::Value* x = v.get("value");
        if (!x || x->kind != hhjson::Value::Num) { json(r.fd, errJson("value must be a number"), 400); return; }
        line = std::string("{\"set\":\"boost\",\"value\":") + (x->n != 0 ? "1" : "0") + "}";
    } else if (action == "poweroff" || action == "reboot" || action == "screen-off" || action == "screen-on") {
        if (action == "poweroff" || action == "reboot") saveAllMemory(action.c_str());
        line = "{\"action\":\"" + action + "\"}";
    } else { json(r.fd, errJson("not a request the hardware service takes"), 400); return; }
    std::string rep;
    hwdRequest(line, rep);
    hhjson::Value rv; if (!hhjson::parse(rep, rv)) rep = errJson("bad reply from the hardware service");
    json(r.fd, rep);
}

void hClock(Req& r) {
    hhjson::Value v;
    if (!hhjson::parse(r.body, v) || v.kind != hhjson::Value::Obj) { json(r.fd, errJson("not JSON"), 400); return; }
    long long target;
    if (v.has("utcMs")) target = (long long)v.num("utcMs");
    else if (v.has("deltaMs")) target = nowMs() + (long long)v.num("deltaMs");     // ★ now + the offset, at THIS moment
    else { json(r.fd, errJson("give utcMs or deltaMs"), 400); return; }
    if (target < 1704067200000LL || target > 4102444800000LL) { json(r.fd, errJson("that time is not plausible"), 400); return; }
    std::string rep;
    hwdRequest("{\"set\":\"time\",\"utcMs\":" + std::to_string(target) + "}", rep);
    hhjson::Value rv;
    const bool ok = hhjson::parse(rep, rv) && rv.boolean("ok");
    if (ok) {
        // ★★ The radio process's NTP offset was measured against the OLD clock — it goes (vibe::clockWasSet).
        std::lock_guard<std::mutex> lk(g_rMtx);
        if (g_child > 0) ::kill(g_child, SIGUSR2);
        logf("clock set (%s, %+lld ms)", v.str("source", "by hand").c_str(), target - nowMs());
    }
    json(r.fd, ok ? "{\"ok\":true,\"utcMs\":" + std::to_string(nowMs()) + "}" : rep);
}

void hPerf(Req& r) {
    Perf p; { std::lock_guard<std::mutex> lk(g_perfMtx); p = g_perf; }
    int pct; bool chg; { std::lock_guard<std::mutex> lk(g_hwMtx); pct = g_batPct; chg = g_batChg; }
    char b[512];
    std::snprintf(b, sizeof b, "{\"cpu\":%.1f,\"tempC\":%.1f,\"engineCpu\":%.1f,\"cores\":%d,\"ramUsedMb\":%lld,\"ramTotalMb\":%lld,"
                  "\"battery\":{\"pct\":%d,\"charging\":%s,\"minutesLeft\":%d}}",
                  p.cpu, p.tempC, p.engineCpu, p.cores, p.ramUsedMb, p.ramTotalMb, pct, chg ? "true" : "false", batteryMinutesLeft());
    json(r.fd, b);
}

void hLocationGet(Req& r) {
    const Location l = loadLocation();
    json(r.fd, "{\"locator\":" + quote(l.locator) + ",\"lat\":" + quote(l.lat) + ",\"lon\":" + quote(l.lon)
         + ",\"set\":" + (l.locator.empty() && l.lat.empty() ? "false" : "true") + "}");
}
/** {"locator":"IO92nh"} · {"lat":52.2,"lon":-0.9} · {"clear":true}. Saved, written into the running radio's settings,
 *  and the radio process told to republish it (SIGUSR1) — no restart. */
void hLocationPost(Req& r) {
    hhjson::Value v;
    if (!hhjson::parse(r.body, v) || v.kind != hhjson::Value::Obj) { json(r.fd, errJson("not JSON"), 400); return; }
    Location l;
    if (v.boolean("clear")) { /* unset: an empty file */ }
    else if (v.has("locator")) {
        l.locator = normLocator(v.str("locator"));
        if (l.locator.empty()) { json(r.fd, errJson("a locator is 4 or 6 characters, e.g. IO92 or IO92nh"), 400); return; }
    } else if (v.has("lat") && v.has("lon")) {
        const hhjson::Value* a = v.get("lat"); const hhjson::Value* b = v.get("lon");
        const double la = a->kind == hhjson::Value::Num ? a->n : std::atof(a->s.c_str());
        const double lo = b->kind == hhjson::Value::Num ? b->n : std::atof(b->s.c_str());
        if (!(la >= -90 && la <= 90 && lo >= -180 && lo <= 180) || (la == 0 && lo == 0)) {
            json(r.fd, errJson("latitude -90 to 90, longitude -180 to 180"), 400); return;
        }
        char x[32]; std::snprintf(x, sizeof x, "%.5f", la); l.lat = x;
        std::snprintf(x, sizeof x, "%.5f", lo); l.lon = x;
    } else { json(r.fd, errJson("give locator, or lat and lon, or clear"), 400); return; }
    std::string err;
    if (!hhcard::atomicWrite(locPath(), "{\"locator\":" + quote(l.locator) + ",\"lat\":" + quote(l.lat) + ",\"lon\":" + quote(l.lon) + "}\n", &err)) {
        json(r.fd, errJson(err), 500); return;
    }
    {
        std::lock_guard<std::mutex> lk(g_rMtx);
        if (g_state != St::None) {
            const std::string path = enginePath(g_cur.serial);
            vsconfig::ServerConfig srv; std::string e2;
            if (vsconfig::loadServer(path, srv, e2) && applyLocationTo(srv, l) && !vsconfig::saveServer(path, srv, e2))
                logf("could not record the location — %s", e2.c_str());
            if (g_child > 0) ::kill(g_child, SIGUSR1);
        }
    }
    logf("location %s", l.locator.empty() && l.lat.empty() ? "cleared" : (l.locator.empty() ? (l.lat + ", " + l.lon).c_str() : l.locator.c_str()));
    hLocationGet(r);
}

void hCard(Req& r) {
    const auto sp = hhcard::spaceAt(g_o.cardDir);
    json(r.fd, std::string("{\"present\":") + (sp.ok && !g_o.cardDir.empty() ? "true" : "false") + ",\"freeBytes\":" + std::to_string(sp.freeBytes)
         + ",\"totalBytes\":" + std::to_string(sp.totalBytes) + "}");
}

std::string listJson(const std::string& dir) {
    std::string out;
    for (const auto& e : hhcard::listFiles(dir))
        out += (out.empty() ? "" : ",") + std::string("{\"name\":") + quote(e.name) + ",\"bytes\":" + std::to_string(e.bytes)
             + ",\"mtime\":" + std::to_string(e.mtime) + "}";
    return "[" + out + "]";
}

void hRecordingsGet(Req& r, const std::string& name) {
    const std::string dir = cardSub("Recordings");
    if (dir.empty()) { json(r.fd, errJson("no card"), 503); return; }
    if (name.empty()) { json(r.fd, "{\"recordings\":" + listJson(dir) + "}"); return; }
    if (!hhcard::safeSegment(name)) { json(r.fd, errJson("no such recording"), 404); return; }
    const std::string path = dir + "/" + name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { if (fd >= 0) ::close(fd); json(r.fd, errJson("no such recording"), 404); return; }
    // ★ A Range answer, because an <audio> element seeks with one (WebKit will not play without it on some builds).
    long long a = 0, b = (long long)st.st_size - 1;
    const std::string rg = header(r.head, "Range");
    bool partial = false;
    if (rg.rfind("bytes=", 0) == 0) {
        const std::string s = rg.substr(6); const size_t dash = s.find('-');
        if (dash != std::string::npos) {
            if (dash > 0) a = std::atoll(s.substr(0, dash).c_str());
            if (dash + 1 < s.size()) b = std::min(b, std::atoll(s.substr(dash + 1).c_str()));
            partial = a <= b && a < (long long)st.st_size;
            if (!partial) { a = 0; b = (long long)st.st_size - 1; }
        }
    }
    const long long len = st.st_size ? b - a + 1 : 0;
    std::string h = std::string("HTTP/1.1 ") + (partial ? "206 Partial Content" : "200 OK") + "\r\nContent-Type: "
        + (name.size() > 4 && name.compare(name.size() - 4, 4, ".wav") == 0 ? "audio/wav" : "application/octet-stream")
        + "\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(len) + "\r\n";
    if (partial) h += "Content-Range: bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" + std::to_string((long long)st.st_size) + "\r\n";
    h += "\r\n";
    sendAll(r.fd, h);
    if (r.method != "HEAD") {
        ::lseek(fd, a, SEEK_SET);
        char buf[65536]; long long left = len;
        while (left > 0) {
            const ssize_t n = ::read(fd, buf, (size_t)std::min<long long>(left, sizeof buf));
            if (n <= 0 || !sendAll(r.fd, std::string(buf, (size_t)n))) break;
            left -= n;
        }
    }
    ::close(fd);
}

/** ★★★ THE PAGE HANDS US ITS WAV (Cog has no downloads, §0.1). Streamed to a hidden ".part" beside its final name and
 *  renamed only when every byte has arrived — a pulled card or a flat battery mid-upload leaves no half recording. */
void hRecordingsPost(Req& r) {
    const std::string dir = cardSub("Recordings");
    if (dir.empty() || !hhcard::ensureDir(dir)) { json(r.fd, errJson("no card to save to"), 503); return; }
    if (r.clen <= 44 || r.clen > (2LL << 30)) { json(r.fd, errJson("a recording must be a WAV of up to 2 GB"), 413); return; }
    const auto sp = hhcard::spaceAt(dir);
    if (sp.ok && (uint64_t)r.clen + (16u << 20) > sp.freeBytes) { json(r.fd, errJson("the card is full"), 507); return; }
    const double hz = std::atof(qv(r.query, "hz").c_str());
    long long start = std::atoll(qv(r.query, "start").c_str());
    if (start <= 0) start = nowMs() / 1000;
    if (start > 4102444800LL) start /= 1000;                            // ★ accept ms as well as s
    const std::string name = hhcard::uniqueIn(dir, hhcard::itemName(start, hz, qv(r.query, "mode"), "wav"));
    const std::string path = dir + "/" + name, tmp = hhcard::tempFor(path);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) { json(r.fd, errJson("cannot write to the card"), 500); return; }
    long long got = 0; bool ok = true;
    auto put = [&](const char* d, size_t n) { size_t off = 0; while (off < n) { const ssize_t w = ::write(fd, d + off, n - off); if (w < 0) { if (errno == EINTR) continue; return false; } off += (size_t)w; } return true; };
    if (!r.body.empty()) { const size_t n = (size_t)std::min<long long>((long long)r.body.size(), r.clen); ok = put(r.body.data(), n); got = (long long)n; }
    char b[65536];
    while (ok && got < r.clen) {
        pollfd p{ r.fd, POLLIN, 0 };
        if (::poll(&p, 1, 15000) <= 0) { ok = false; break; }
        const ssize_t n = ::recv(r.fd, b, (size_t)std::min<long long>(sizeof b, r.clen - got), 0);
        if (n <= 0) { ok = false; break; }
        ok = put(b, (size_t)n); got += n;
    }
    ::close(fd);
    std::string err;
    if (!ok || got != r.clen || !hhcard::commitTemp(tmp, path, &err)) {
        ::unlink(tmp.c_str());
        json(r.fd, errJson(err.empty() ? "the recording did not arrive whole" : err), 500);
        return;
    }
    logf("recording saved: Recordings/%s (%lld bytes)", name.c_str(), got);
    json(r.fd, "{\"ok\":true,\"name\":" + quote(name) + "}");
}

/** DELETE /handheld/gallery/<FOLDER/file> — a decoded picture on the card (Gallery/WEFAX/…, Gallery/SSTV/…). The
 *  listing already skips an item whose file is gone, so unlinking the file removes it. FOLDER = [A-Z0-9-]{1,24}. */
void hGalleryDelete(Req& r, const std::string& name) {
    const std::string dir = cardSub("Gallery");
    const size_t sl = name.find('/');
    bool ok = !dir.empty() && sl != std::string::npos && sl > 0 && sl <= 24 && name.find('/', sl + 1) == std::string::npos;
    for (size_t i = 0; ok && i < sl; i++) { const char c = name[i]; ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; }
    if (ok) ok = hhcard::safeSegment(name.substr(sl + 1));
    if (!ok || ::unlink((dir + "/" + name).c_str()) != 0) { json(r.fd, errJson("no such picture"), 404); return; }
    logf("gallery item deleted: Gallery/%s", name.c_str());
    json(r.fd, "{\"ok\":true}");
}

void hRecordingsDelete(Req& r, const std::string& name) {
    const std::string dir = cardSub("Recordings");
    if (dir.empty() || !hhcard::safeSegment(name)) { json(r.fd, errJson("no such recording"), 404); return; }
    if (::unlink((dir + "/" + name).c_str()) != 0) { json(r.fd, errJson("no such recording"), 404); return; }
    logf("recording deleted: Recordings/%s", name.c_str());
    json(r.fd, "{\"ok\":true}");
}

/** ★ import/: the files waiting, with their text, for the PAGE to read — it already parses every bookmark format there
 *  is (UberSDR's YAML, the app's JSON — parseBookmarksAny), and a second parser here would be a second set of rules. */
void hImportGet(Req& r) {
    const std::string dir = cardSub("import");
    if (dir.empty()) { json(r.fd, "{\"files\":[]}"); return; }
    hhcard::ensureDir(dir);
    std::string out; int n = 0;
    for (const auto& e : hhcard::listFiles(dir)) {
        if (n >= 20) break;
        if (!vseibi::seasonOfName(e.name).empty()) continue;   // ★ an EiBi schedule is not a bookmark backup (IMPORT EIBI)
        std::string text;
        if (e.bytes > (1u << 20) || !hhcard::readFile(dir + "/" + e.name, text, 1u << 20)) {
            out += (out.empty() ? "" : ",") + std::string("{\"name\":") + quote(e.name) + ",\"tooBig\":true}";
        } else out += (out.empty() ? "" : ",") + std::string("{\"name\":") + quote(e.name) + ",\"text\":" + quote(text) + "}";
        n++;
    }
    json(r.fd, "{\"files\":[" + out + "]}");
}
/** The page's verdict on one file: it moves to import/done/, a "<name>.result.txt" beside it says what happened. */
void hImportResult(Req& r) {
    hhjson::Value v;
    if (!hhjson::parse(r.body, v) || v.kind != hhjson::Value::Obj) { json(r.fd, errJson("not JSON"), 400); return; }
    const std::string name = v.str("name");
    const std::string dir = cardSub("import"), done = dir + "/done";
    if (dir.empty() || !hhcard::safeSegment(name) || !hhcard::exists(dir + "/" + name)) { json(r.fd, errJson("no such file"), 404); return; }
    hhcard::ensureDir(done);
    const std::string dest = hhcard::uniqueIn(done, name);
    if (::rename((dir + "/" + name).c_str(), (done + "/" + dest).c_str()) != 0) { json(r.fd, errJson("could not move it"), 500); return; }
    const bool ok = v.boolean("ok");
    const int count = (int)v.num("imported", 0);
    std::string text = std::string(ok ? "Imported" : "NOT imported") + " — " + hhcard::utcStamp(nowMs() / 1000) + " UTC\r\n"
        + "File: " + name + "\r\n" + (ok ? "Bookmarks imported: " + std::to_string(count) + "\r\n" : std::string())
        + (v.str("message").empty() ? "" : v.str("message").substr(0, 2000) + "\r\n");
    hhcard::atomicWrite(done + "/" + dest + ".result.txt", text);
    logf("import: %s → import/done/%s (%s, %d)", name.c_str(), dest.c_str(), ok ? "ok" : "failed", count);
    json(r.fd, "{\"ok\":true,\"movedTo\":" + quote("done/" + dest) + "}");
}

/** Ask the radio process to re-read the schedule: GET /vibeserver/eibi through our own door (it is handed over like any
 *  page request) re-parses the cache when the file changed (vseibi::loadedCount). Its entry count, or -1 when no radio
 *  answered — the next radio to start loads the file anyway (main.cpp, loadFromCache at start-up). */
int askRadioToReloadEibi() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    timeval tv{ 25, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#if defined(__APPLE__)
    { int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_o.port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return -1; }
    std::string got;
    if (sendAll(fd, "GET /vibeserver/eibi HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")) {
        char b[4096];
        for (;;) { const ssize_t n = ::recv(fd, b, sizeof b, 0); if (n <= 0) break; got.append(b, (size_t)n); if (got.size() > 65536) break; }
    }
    ::close(fd);
    if (got.compare(0, 12, "HTTP/1.1 200") != 0) return -1;
    const size_t at = got.find("\"entries\":");
    return at == std::string::npos ? -1 : std::atoi(got.c_str() + at + 10);
}

/** ★★ V12 IMPORT EIBI (Stuart, 2026-11-06: "EiBi is not going to be able to be auto downloaded"). The handheld has no
 *  internet, so the owner downloads EiBi's schedule (sked-b26.csv … from eibispace.de) on a computer and puts it on the
 *  card in EiBi\ (import\ is looked in too). The NEWEST sked-*.csv there is checked and installed as the machine's
 *  schedule — $DATA/eibi.csv, the very file the radio process loads at start and a networked server downloads to
 *  (vseibi::importFile: capped, parsed, refused when empty, temp + rename) — then the radio process re-reads it, so
 *  SEARCH finds the stations at once and after every reboot. The file stays on the card: it is the owner's copy. */
void hEibiImport(Req& r) {
    if (g_o.cardDir.empty()) { json(r.fd, errJson("no card"), 503); return; }
    std::string pick, from;
    int64_t newest = -1;
    for (const char* sub : { "EiBi", "import" }) {
        const std::string dir = cardSub(sub);
        if (std::string(sub) == "EiBi") hhcard::ensureDir(dir);
        for (const auto& e : hhcard::listFiles(dir)) {
            if (vseibi::seasonOfName(e.name).empty() || e.name.size() < 4) continue;
            std::string ext = e.name.substr(e.name.size() - 4);
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext != ".csv") continue;
            if (e.mtime > newest) { newest = e.mtime; pick = dir + "/" + e.name; from = std::string(sub) + "/" + e.name; }
        }
    }
    if (pick.empty()) {
        // ★ eibispace.de also offers the SAME data as printable lists (freq-b26.txt, time-b26.txt: fixed-width, a banner
        //   header). Only the CSV is read — so when that is what the owner brought, say exactly which file to fetch.
        for (const char* sub : { "EiBi", "import" }) {
            for (const auto& e : hhcard::listFiles(cardSub(sub))) {
                std::string lo = e.name;
                for (auto& c : lo) c = (char)std::tolower((unsigned char)c);
                if (lo.size() < 9 || (lo.compare(0, 5, "freq-") != 0 && lo.compare(0, 5, "time-") != 0)) continue;
                const std::string code = vseibi::seasonOfName("sked-" + lo.substr(5));
                const std::string want = "sked-" + (code.empty() ? std::string("b26") : code) + ".csv";
                json(r.fd, errJson("found " + e.name + " — that is the printable list; download " + want
                                   + " from eibispace.de instead (the CSV, not the freq-/time- .txt versions)"), 404);
                return;
            }
        }
        json(r.fd, errJson("No EiBi file on the card — download sked-b26.csv from eibispace.de (the CSV, not the "
                           "freq-/time- .txt versions) and put it in the card's EiBi folder"), 404);
        return;
    }
    vseibi::setDir(g_o.dataDir);
    std::string err, season;
    const int n = vseibi::importFile(pick, err, &season);
    if (n <= 0) { logf("EiBi import: %s refused — %s", from.c_str(), err.c_str()); json(r.fd, errJson(from + ": " + err), 400); return; }
    const int loaded = askRadioToReloadEibi();
    logf("EiBi import: %s — %d entries%s%s", from.c_str(), n, season.empty() ? "" : (", " + season).c_str(),
         loaded < 0 ? " (no radio running: loaded when one starts)" : "");
    json(r.fd, "{\"ok\":true,\"file\":" + quote(from) + ",\"entries\":" + std::to_string(n) + ",\"season\":" + quote(season)
             + ",\"loaded\":" + std::to_string(loaded) + "}");
}

void hExport(Req& r) {
    const std::string kind = qv(r.query, "kind");
    const std::string dir = cardSub("export");
    if (dir.empty() || !hhcard::ensureDir(dir)) { json(r.fd, errJson("no card to save to"), 503); return; }
    if (kind != "bookmarks" && kind != "dxlog") { json(r.fd, errJson("kind must be bookmarks or dxlog"), 400); return; }
    if (r.body.empty()) { json(r.fd, errJson("nothing to export"), 400); return; }
    std::string ext = qv(r.query, "ext"); if (ext != "csv" && ext != "adi" && ext != "txt") ext = "json";
    const std::string name = hhcard::uniqueIn(dir, hhcard::utcStampSec(nowMs() / 1000) + " - " + (kind == "dxlog" ? "DX log" : "bookmarks") + "." + ext);
    std::string err;
    if (!hhcard::atomicWrite(dir + "/" + name, r.body, &err)) { json(r.fd, errJson(err), 500); return; }
    logf("export: export/%s", name.c_str());
    json(r.fd, "{\"ok\":true,\"name\":" + quote("export/" + name) + "}");
}

/** ★ EXPORT DIAGNOSTICS: the servers' own vibeserver-diagnostics, run as this user (what it cannot read, it says so),
 *  scrubbed again of what we know (diag_scrub.h), named by UTC time, onto the card. */
void hDiagnostics(Req& r) {
    const std::string dir = cardSub("Diagnostic logs");
    if (dir.empty() || !hhcard::ensureDir(dir)) { json(r.fd, errJson("no card to save to"), 503); return; }
    std::string out;
    const std::string cmd = "timeout 180 python3 -I '" + g_o.diagScript + "' 2>&1";
    if (FILE* p = ::popen(cmd.c_str(), "r")) {
        char b[65536]; size_t n;
        while ((n = std::fread(b, 1, sizeof b, p)) > 0) { out.append(b, n); if (out.size() > (16u << 20)) break; }
        ::pclose(p);
    }
    if (out.empty()) out = "vibeserver-diagnostics produced nothing (" + g_o.diagScript + ")\n";
    // ★ The handheld's own state on the end: what the supervisor knows that the script cannot.
    {
        std::lock_guard<std::mutex> lk(g_rMtx);
        out += "\n== handheld supervisor ==\nstate: " + std::string(stName(g_state)) + "\nradio: " + radioJsonLocked()
             + "\nhardware service: " + (g_hwdUp.load() ? "connected" : "not connected") + " (" + g_o.hwdSock + ")\ncard: " + g_o.cardDir + "\n";
    }
    std::vector<std::string> secrets;   // ★ the handheld has no admin password or PIN; the scrub's patterns still run
    out = vsdiag::scrubSecrets(std::move(out), secrets);
    const std::string name = hhcard::uniqueIn(dir, hhcard::utcStampSec(nowMs() / 1000) + " UTC - VibeSDR diagnostics.txt");
    std::string err;
    if (!hhcard::atomicWrite(dir + "/" + name, out, &err)) { json(r.fd, errJson(err), 500); return; }
    logf("diagnostics: Diagnostic logs/%s (%zu bytes)", name.c_str(), out.size());
    json(r.fd, "{\"ok\":true,\"name\":" + quote("Diagnostic logs/" + name) + "}");
}

/** ★ No radio: the page itself, its scripts, and a /vibeserver.json that says so. Everything else is a plain 503. */
void answerNoRadio(Req& r) {
    if (!readRequest(r, 64 * 1024)) return;
    if (r.path == "/vibeserver.json") {
        json(r.fd, "{\"handheld\":true,\"radio\":null,\"state\":\"none\"}");
        return;
    }
    std::string body, type, enc;
    const std::string ae = header(r.head, "Accept-Encoding");
    if ((r.path == "/" || r.path.rfind("/vs/", 0) == 0)
        && vibe::LocalSdrShim::webAssetFor(r.path, ae.find("br") != std::string::npos, ae.find("gzip") != std::string::npos, body, type, enc)) {
        std::string extra = "Vary: Accept-Encoding\r\n";
        if (!enc.empty()) extra += "Content-Encoding: " + enc + "\r\n";
        reply(r.fd, 200, "OK", type, body, extra);
        return;
    }
    json(r.fd, errJson("no radio is plugged in"), 503);
}

void routeHandheld(Req& r) {
    const std::string& p = r.path;
    const size_t cap = (p == "/handheld/recordings" && r.method == "POST") ? 0 : 2u << 20;
    if (!readRequest(r, cap)) return;
    if (cap && (size_t)r.clen > cap) { json(r.fd, errJson("too big"), 413); return; }
    if (p == "/handheld/session")                                  hSession(r);
    else if (p == "/handheld/memory" && r.method == "POST")         hMemory(r);
    else if (p == "/handheld/events")                              hEvents(r);
    else if (p == "/handheld/device" && r.method == "POST")         hDevicePost(r);
    else if (p == "/handheld/device")                              hDeviceGet(r);
    else if (p == "/handheld/clock" && r.method == "POST")          hClock(r);
    else if (p == "/handheld/perf")                                hPerf(r);
    else if (p == "/handheld/card")                                hCard(r);
    else if (p == "/handheld/location" && r.method == "POST")       hLocationPost(r);
    else if (p == "/handheld/location")                            hLocationGet(r);
    else if (p == "/handheld/recordings" && r.method == "POST")     hRecordingsPost(r);
    else if (p == "/handheld/recordings")                          hRecordingsGet(r, "");
    else if (p.rfind("/handheld/recordings/", 0) == 0) {
        const std::string name = pctDecode(p.substr(21));
        if (r.method == "DELETE") hRecordingsDelete(r, name); else hRecordingsGet(r, name);
    }
    else if (p.rfind("/handheld/gallery/", 0) == 0 && r.method == "DELETE") hGalleryDelete(r, pctDecode(p.substr(18)));
    else if (p == "/handheld/import" && r.method == "GET")          hImportGet(r);
    else if (p == "/handheld/import/result" && r.method == "POST")  hImportResult(r);
    else if (p == "/handheld/eibi/import" && r.method == "POST")    hEibiImport(r);
    else if (p == "/handheld/export" && r.method == "POST")         hExport(r);
    else if (p == "/handheld/diagnostics" && r.method == "POST")    hDiagnostics(r);
    else json(r.fd, errJson("no such handheld route"), 404);
}

/** ★★ One connection: peek the request line, then answer it here or hand it whole to the radio process. */
void connection(int fd) {
    Req r; r.fd = fd;
    char buf[2048];
    pollfd p{ fd, POLLIN, 0 };
    if (::poll(&p, 1, 8000) <= 0) { ::close(fd); return; }
    const ssize_t n = ::recv(fd, buf, sizeof buf - 1, MSG_PEEK);
    if (n <= 0) { ::close(fd); return; }
    const std::string head(buf, (size_t)n);
    const size_t sp = head.find(' '), sp2 = sp == std::string::npos ? std::string::npos : head.find(' ', sp + 1);
    if (sp == std::string::npos || sp2 == std::string::npos) { ::close(fd); return; }
    r.method = head.substr(0, sp);
    r.target = head.substr(sp + 1, sp2 - sp - 1);
    const size_t q = r.target.find('?');
    r.path = r.target.substr(0, q);
    r.query = q == std::string::npos ? "" : r.target.substr(q + 1);
    if (r.path.rfind("/handheld/", 0) == 0) { routeHandheld(r); ::close(fd); return; }
    /* ★★ THE MAP IS THE DEVICE'S, NOT THE RADIO'S: static files on the card's image (the basic pack and the high-detail
     *  one, vibeserver-mapdata-detail), so the supervisor answers /mapdata/v1/ and /mapgl/ itself, radio or none — the
     *  image agent's boot test found every map file a 503 "no radio is plugged in". Same lookup as a server. */
    if (r.path.rfind("/mapdata/v1/", 0) == 0 || r.path.rfind("/mapgl/", 0) == 0) {
        if (readRequest(r, 0)) {
            const std::string ae = header(r.head, "Accept-Encoding");
            vibe::LocalSdrShim::serveMapAsset(fd, r.method, r.path, ae.find("gzip") != std::string::npos, header(r.head, "Range"));
        }
        ::close(fd); return;
    }
    // ★ A radio that is starting is waited for (≤ 15 s) — the page's first request after a plug-in is not an error.
    std::string dest;
    {
        std::unique_lock<std::mutex> lk(g_rMtx);
        if (g_state == St::Starting) g_rCv.wait_for(lk, std::chrono::seconds(15), [] { return g_state != St::Starting; });
        if (g_state == St::Running) dest = g_childSock;
    }
    if (!dest.empty()) {
        std::string err;
        if (vibe::sendFdTo(dest, fd, err)) { ::close(fd); return; }   // ★ our copy only — the radio process has its own
        logf("hand-off to the radio process failed (%s) — answering here", err.c_str());
    }
    answerNoRadio(r);
    ::close(fd);
}

int listenLoopback(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);   // ★ never inherited by the radio process
    if (fd < 0) return -1;
    int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 64) != 0) { ::close(fd); return -1; }
    return fd;
}

std::string selfPath(const char* argv0) {
#if defined(__linux__)
    char b[4096]; const ssize_t n = ::readlink("/proc/self/exe", b, sizeof b - 1);
    if (n > 0) { b[n] = 0; return b; }
#elif defined(__APPLE__)
    char b[4096]; uint32_t sz = sizeof b;
    if (_NSGetExecutablePath(b, &sz) == 0) { char rp[4096]; if (::realpath(b, rp)) return rp; return b; }
#endif
    return argv0;
}

}  // namespace

bool wanted(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--handheld") return true;
    for (int i = 1; i < argc; i++) if (std::string(argv[i]).rfind("--handheld-", 0) == 0) return false;   // ★ the radio process
    // ★ The config form: "profile":"handheld" in the file the server reads.
    const char* c = std::getenv("VIBESERVER_CONFIG");
    const std::string path = c && *c ? c : vsconfig::defaultPath();
    std::string text; hhjson::Value v;
    return hhcard::readFile(path, text, 1u << 20) && hhjson::parse(text, v) && v.str("profile") == "handheld";
}

int runSupervisor(int argc, char** argv) {
    g_o.self = selfPath(argv[0]);
    for (int i = 1; i + 1 < argc; i++) if (std::string(argv[i]) == "--port") g_o.port = std::atoi(argv[i + 1]);
    g_o.dataDir   = envOr("VIBESERVER_DATA_DIR", "/var/lib/vibeserver");
    g_o.cardDir   = envOr("VIBESERVER_CARD_DIR", "");
    g_o.hwdSock   = envOr("VIBE_HWD_SOCK", "/run/vibe-hwd.sock");
    g_o.resetFlag = envOr("VIBE_HANDHELD_RESET_FLAG", "/run/vibe-handheld/reset-radio");
    g_o.fakeRadios = envOr("VIBE_HANDHELD_FAKE_RADIOS", "");
    g_o.diagScript = envOr("VIBESERVER_DIAG_SCRIPT", "/usr/lib/vibeserver/vibeserver-diagnostics");
    g_o.hhDir  = g_o.dataDir + "/handheld";
    g_o.runDir = g_o.hhDir + "/run";
    if (!hhcard::ensureDir(g_o.hhDir) || !hhcard::ensureDir(g_o.runDir)) {
        std::fprintf(stderr, "VibeServer handheld: cannot write %s\n", g_o.hhDir.c_str());
        return 1;
    }
    ::chmod(g_o.runDir.c_str(), 0700);
    vibemapgl::setDataDir(g_o.dataDir + "/mapgl");   // ★ the GPU map's detail pack, where a server keeps a downloaded one
    std::signal(SIGINT, onSig); std::signal(SIGTERM, onSig); std::signal(SIGPIPE, SIG_IGN);
    // ★ Start+Select (§0.2): read once, acted on when the next radio connects, then the flag goes.
    if (hhcard::exists(g_o.resetFlag)) {
        g_resetArmed = true;
        if (::unlink(g_o.resetFlag.c_str()) != 0) logf("the reset flag %s could not be removed (%s) — acting on it for this run only",
                                                     g_o.resetFlag.c_str(), std::strerror(errno));
        logf("Start+Select held at power-on: the next radio goes back to its defaults");
    }
    // ★ What a cut-off write left on the card, swept before anything new is written.
    if (!g_o.cardDir.empty()) {
        int n = 0;
        for (const char* sub : { "Recordings", "Gallery", "export", "Diagnostic logs", "import/done" }) n += hhcard::sweepParts(cardSub(sub));
        if (n) logf("removed %d unfinished write%s from the card", n, n == 1 ? "" : "s");
    }
    const int lfd = listenLoopback(g_o.port);
    if (lfd < 0) { std::fprintf(stderr, "VibeServer handheld: port %d on 127.0.0.1 is taken\n", g_o.port); return 1; }
    logf("listening on 127.0.0.1:%d — data %s, card %s, hardware service %s%s", g_o.port, g_o.hhDir.c_str(),
         g_o.cardDir.empty() ? "(none)" : g_o.cardDir.c_str(), g_o.hwdSock.c_str(), g_o.fakeRadios.empty() ? "" : " [TEST RADIOS]");
    std::thread(hwdEventLoop).detach();
    std::thread(perfLoop).detach();
    std::thread watch(watchLoop);
    while (!g_stop) {
        pollfd p{ lfd, POLLIN, 0 };
        const int r = ::poll(&p, 1, 500);
        if (r <= 0) continue;
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
#if defined(__APPLE__)
        { int one = 1; ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
        std::thread(connection, cfd).detach();
    }
    ::close(lfd);
    watch.join();
    logf("stopping — saving every radio's memory");
    saveAllMemory("stopping");
    {
        std::lock_guard<std::mutex> lk(g_rMtx);
        stopChildLocked("the supervisor is stopping", 5.0);
    }
    g_evCv.notify_all();
    return 0;
}

}  // namespace handheld
