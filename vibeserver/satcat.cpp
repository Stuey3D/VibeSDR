// V12 SATELLITE ENGINE — sats.json from vibesdr.net, synced like the EiBi schedule (docs/v12/SATELLITE-ENGINE.md §6).
//
// ★★★ A VibeServer NEVER ASKS CELESTRAK. CelesTrak's usage policy (https://celestrak.org/usage-policy.php) asks for one
//     download per update, an immediate stop on any non-200, and firewalls offenders by IP — and Brazilian hosts on
//     mobile data share carrier CGNAT addresses, so many servers asking directly could get a whole carrier blocked. The
//     ONE client is vibesdr.net's mirror (directory/src/sats.js), which follows every point of the policy; servers sync
//     its sats.json. There is no fallback to CelesTrak: a server that cannot reach vibesdr.net keeps its last good copy
//     (the engine shows the age, STALE / ELEMENTS n d, and refuses past 14 days), then shows NO SAT DATA with the reason.
// ★★★ HARD RULE, kept here although the mirror is the one that meets CelesTrak: NO RETRY LOOPS, EVER. One request per
//     round; any answer other than 200 / 304 (and a network failure, a timeout, a reset — the far end may be overloaded)
//     ENDS the round and starts a hold of at least kHoldFloorSec (15 min — no configuration can lower it; ours is 1 h
//     doubling to a day) PLUS a random 0–60 min, so a fleet that lost the mirror together does not come back together.
//     The hold is written to the shared state BEFORE anything else happens, and a provisional hold is written BEFORE
//     the request, so a crash or restart mid-fetch still honours it. Nothing a listener does triggers a fetch.
// ★★ ONE FETCH PER MACHINE: the state file and a flock() are in the machine's data dir — every radio process sees the
//    others' attempts and holds, and a second process finds the lock held and asks nothing. The one that fetched
//    publishes; the others pick the new copy up by its mtime (reloadIfChanged).
// ★★ ONLY WHEN USED: main.cpp asks only while a radio in the process reaches a listed downlink (LocalSdrShim::satWanted).
// ★ Daily, a conditional GET (If-None-Match / If-Modified-Since — the mirror answers 304), --compressed, a plain
//   User-Agent naming VibeServer and its version (no owner details — the privacy rule). The copy's last check decides,
//   never uptime: a restart does not fetch. A start-up delay of 30–330 s (main.cpp) staggers a fleet restarted together.
// ★ OFFLINE-SAFE: a failed, refused, truncated or invalid download never touches the copy.
#include "satcat.h"
#include "proc.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <random>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vssat {
namespace {

std::string g_dir = "/var/lib/vibeserver";
std::function<void(const std::string&)> g_publish;
std::function<void(const std::string&, const std::string&)> g_legacy;
std::function<bool(const std::string&, std::string&)> g_validate;
Fetcher g_fetcher;
std::function<std::time_t()> g_clock;
std::function<uint32_t()> g_random;
std::mutex g_mtx;
long long g_loadedMtime = 0;

std::time_t nowT() { return g_clock ? g_clock() : std::time(nullptr); }
uint32_t rnd() { if (g_random) return g_random(); std::random_device rd; return rd(); }
std::string satDir() { return g_dir + "/sat"; }
std::string copyPath() { return satDir() + "/sats.json"; }
std::string statePath() { return satDir() + "/sync-state"; }
std::string lockPath() { return satDir() + "/sync.lock"; }
void ensureDir() { ::mkdir(g_dir.c_str(), 0755); ::mkdir(satDir().c_str(), 0755); }

/** The shared state: one "key value" a line. */
struct State {
    long long checked = 0, attempt = 0, until = 0; int fails = 0, code = 0; long seed = -1;
    int stale = 0;   // ★ beta91 the copy was already older than kStaleDataSec when last checked: re-check after kStaleRecheckSec
    std::string etag, lastmod, msg;
};
State readState() {
    State s;
    FILE* f = std::fopen(statePath().c_str(), "rb");
    if (!f) return s;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const size_t sp = l.find(' ');
        const std::string k = l.substr(0, sp), v = sp == std::string::npos ? "" : l.substr(sp + 1);
        if (k == "checked") s.checked = std::atoll(v.c_str());
        else if (k == "attempt") s.attempt = std::atoll(v.c_str());
        else if (k == "until") s.until = std::atoll(v.c_str());
        else if (k == "fails") s.fails = std::atoi(v.c_str());
        else if (k == "code") s.code = std::atoi(v.c_str());
        else if (k == "seed") s.seed = std::atol(v.c_str());
        else if (k == "stale") s.stale = std::atoi(v.c_str());
        else if (k == "etag") s.etag = v;
        else if (k == "lastmod") s.lastmod = v;
        else if (k == "msg") s.msg = v;
    }
    std::fclose(f);
    return s;
}
std::string oneLine(const std::string& v) {
    std::string o;
    for (char c : v) if ((unsigned char)c >= 0x20 || (unsigned char)c >= 0x80) o += c;
    return o.substr(0, 400);
}
void writeState(const State& s) {
    ensureDir();
    const std::string tmp = statePath() + ".tmp." + std::to_string((long)::getpid());
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "checked %lld\nattempt %lld\nuntil %lld\nfails %d\ncode %d\nseed %ld\nstale %d\netag %s\nlastmod %s\nmsg %s\n",
                 s.checked, s.attempt, s.until, s.fails, s.code, s.seed, s.stale, oneLine(s.etag).c_str(), oneLine(s.lastmod).c_str(),
                 oneLine(s.msg).c_str());
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    std::rename(tmp.c_str(), statePath().c_str());
}
/** ★ This machine's own offset into the day, chosen once and kept: the fleet's daily checks spread over 4 hours. */
long seedOf(State& s) {
    if (s.seed < 0 || s.seed > kCheckJitterSec) { s.seed = (long)(rnd() % (uint32_t)kCheckJitterSec); writeState(s); }
    return s.seed;
}

bool readCapped(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16384]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > kMaxBytes) { std::fclose(f); out.clear(); return false; }
    }
    std::fclose(f);
    return true;
}
long long mtimeOf(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0 ? (long long)st.st_mtime : 0; }

bool structural(const std::string& s, std::string& err) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    if (i >= s.size() || s[i] != '{' || s.find("\"satellites\"") == std::string::npos) { err = "not sats.json"; return false; }
    return true;
}
bool valid(const std::string& s, std::string& err) { return g_validate ? g_validate(s, err) : structural(s, err); }

std::string hhmm(long long t) {
    std::time_t tt = (std::time_t)t; std::tm g{}; gmtime_r(&tt, &g);
    char b[40]; std::strftime(b, sizeof b, "%Y-%m-%d %H:%M UTC", &g); return b;
}

std::string headerValue(const std::string& hdrs, const char* name) {
    // the LAST response's headers (no redirects are followed, but be safe), case-insensitive
    std::string out; size_t pos = 0; const size_t nl = std::strlen(name);
    while (pos < hdrs.size()) {
        size_t e = hdrs.find('\n', pos); if (e == std::string::npos) e = hdrs.size();
        std::string l = hdrs.substr(pos, e - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.size() > nl && strncasecmp(l.c_str(), name, nl) == 0 && l[nl] == ':') {
            out = l.substr(nl + 1); while (!out.empty() && out.front() == ' ') out.erase(0, 1);
        }
        pos = e + 1;
    }
    return out;
}

void curlFetch(const Request& rq, Response& rs) {
    const std::string hdr = rq.dest + ".hdr";
    std::remove(hdr.c_str());
    // ★ no -f (we want the code), no -L (an address that moves is a fault to report, not to follow), no --retry
    std::vector<std::string> a = {"curl", "-sS", "--compressed", "--max-time", "45", "--connect-timeout", "15",
                                  "-A", userAgent(), "-D", hdr, "-o", rq.dest, "-w", "\n%{http_code}"};
    if (!rq.ifNoneMatch.empty()) { a.push_back("-H"); a.push_back("If-None-Match: " + rq.ifNoneMatch); }
    if (!rq.ifModifiedSince.empty()) { a.push_back("-H"); a.push_back("If-Modified-Since: " + rq.ifModifiedSince); }
    a.push_back(rq.url);
    std::string out;
    vibeproc::Child c = vibeproc::spawn(a, true);
    if (c.ok()) { char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, c.out)) > 0 && out.size() < 65536) out.append(buf, n); }
    const int rc = vibeproc::reap(c);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    const size_t nl = out.rfind('\n');
    const std::string codeS = nl == std::string::npos ? out : out.substr(nl + 1);
    std::string msg = nl == std::string::npos ? "" : out.substr(0, nl);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    rs.code = std::atoi(codeS.c_str());
    if (rc != 0) { rs.code = 0; rs.msg = !msg.empty() ? msg : rc < 0 ? "curl could not be run" : "curl exited " + std::to_string(rc); }
    std::string h;
    if (readCapped(hdr, h)) {
        rs.etag = headerValue(h, "ETag"); rs.lastModified = headerValue(h, "Last-Modified");
        rs.retryAfter = parseRetryAfter(headerValue(h, "Retry-After"), nowT());
    }
    std::remove(hdr.c_str());
}

/** ★ beta91 how old the mirror's data is: now − the OLDEST group's CelesTrak fetch ("orbits":{"amateur":{"fetched":
 *  "2026-11-22T16:23:52.000Z"},…}). -1 = not stated (an old mirror, a hand-made file): never called stale. */
long long dataAgeSec(const std::string& body, std::time_t now) {
    const size_t o = body.find("\"orbits\"");
    if (o == std::string::npos) return -1;
    const size_t end = body.find("\"satellites\"", o);
    long long oldest = 0;
    for (size_t at = body.find("\"fetched\":\"", o); at != std::string::npos && (end == std::string::npos || at < end);
         at = body.find("\"fetched\":\"", at + 1)) {
        std::tm tm{}; int Y, M, D, h, m, sec;
        if (std::sscanf(body.c_str() + at + 11, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) continue;
        tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec;
        const long long t = (long long)timegm(&tm);
        if (t > 0 && (oldest == 0 || t < oldest)) oldest = t;
    }
    return oldest > 0 ? (long long)now - oldest : -1;
}
/** The wait after a good check: a day (less this machine's offset), or kStaleRecheckSec while the copy was old. */
long dueAfterCheck(const State& s) { return s.stale ? kStaleRecheckSec : kCheckSec - (s.seed >= 0 ? s.seed : 0); }

bool eligible(State& s, std::time_t now, bool haveCopy) {
    // ★ a time in the FUTURE beyond any hold we write = a clock that jumped back (an RTC-less Pi): not honoured
    const long long maxAhead = 86400 + kResumeJitterSec + 60;
    if (s.until > now && s.until - now <= maxAhead) return false;
    // ★★ item 9: with no copy at all (SATELLITE shows nothing), attempts may come every 15 min — the floor — not hourly
    if (s.attempt > 0 && s.attempt <= now && now - s.attempt < (haveCopy ? kMinAttemptSec : kNoCopyMinAttemptSec)) return false;
    if (s.attempt > now && s.attempt - now < 86400) return false;
    if (!haveCopy) return true;
    seedOf(s);
    /* ★★ beta91 "old" is judged NOW, from the copy itself, not only from the flag the last check stored (Stuart 11-22,
     *  "orbits are 31h old"): a state file written by an older server has no `stale` line, so the Pi 500's copy — fetched
     *  at 12:20 while the mirror was itself stalled, data from 21 Nov 16:08 — read as fresh and waited a day. And a copy
     *  that was fresh when checked becomes old with time: once it passes kStaleDataSec it is re-checked kStaleRecheckSec
     *  after the last check (a 304 costs the mirror nothing). */
    if (!s.stale) { std::string cur; if (readCapped(copyPath(), cur) && dataAgeSec(cur, now) > kStaleDataSec) s.stale = 1; }
    const long due = dueAfterCheck(s);
    return s.checked <= 0 || s.checked > now || now - s.checked >= due;
}

/** ★★ A failure: the hold FIRST, on disk, before the caller does anything else. */
void hold(State& s, std::time_t now, int code, const std::string& what, bool haveCopy = true, long retryAfter = 0) {
    s.fails += 1;
    long h;
    if (haveCopy) {
        h = std::min<long>(86400, kBackoffSec << std::min(s.fails - 1, 5));
    } else {
        // ★★ SATELLITE MODE item 9: nothing on this machine yet — 15 min, 30 min, then hourly (a 503 from the mirror
        //    while it builds its first copy is expected; the listener is waiting with an empty box)
        h = std::min<long>(kBackoffSec, kHoldFloorSec << std::min(s.fails - 1, 5));
    }
    // ★ the far end's Retry-After is honoured — it can lengthen a hold, never shorten it below the floor
    if (retryAfter > 0) h = std::max(h, std::min<long>(retryAfter, kMaxRetryAfterSec));
    h = std::max(h, kHoldFloorSec);
    h += (long)(rnd() % (uint32_t)(haveCopy ? kResumeJitterSec : kNoCopyJitterSec));
    s.until = now + h;
    s.code = code;
    const std::string when = hhmm(s.until);
    if (code == 0)
        s.msg = "vibesdr.net could not be reached for the satellite data (" + what + ") \xe2\x80\x94 VibeServer has stopped asking and will try again after " + when;
    else if (code >= 500)
        s.msg = "vibesdr.net's satellite mirror is overloaded (HTTP " + std::to_string(code) + ") \xe2\x80\x94 VibeServer has stopped asking and will try again after " + when;
    else if (code == 200)
        s.msg = "vibesdr.net's answer was not satellite data (" + what + ") \xe2\x80\x94 the last good copy is kept; VibeServer will try again after " + when;
    else
        s.msg = "vibesdr.net answered HTTP " + std::to_string(code) + " for the satellite data \xe2\x80\x94 VibeServer will not ask again until " + when;
    writeState(s);
}

struct FileLock {
    int fd = -1;
    bool take() {
        ensureDir();
        fd = ::open(lockPath().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0) return true;   // ★ a read-only data dir: the state file still guards
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); fd = -1; return false; }
        return true;
    }
    ~FileLock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
};

}  // namespace

void setDir(const std::string& dir) { if (!dir.empty()) g_dir = dir; }
void setPublisher(std::function<void(const std::string&)> fn) { g_publish = std::move(fn); }
void setLegacyPublisher(std::function<void(const std::string&, const std::string&)> fn) { g_legacy = std::move(fn); }
void setValidator(std::function<bool(const std::string&, std::string&)> fn) { g_validate = std::move(fn); }
void setFetcher(Fetcher fn) { g_fetcher = std::move(fn); }
void setClock(std::function<std::time_t()> fn) { g_clock = std::move(fn); }
void setRandom(std::function<uint32_t()> fn) { g_random = std::move(fn); }
std::string userAgent() {
#ifdef VIBESERVER_VERSION_STR
    return std::string("VibeServer/") + VIBESERVER_VERSION_STR + " (satellite data; +https://vibesdr.net)";
#else
    return "VibeServer (satellite data; +https://vibesdr.net)";
#endif
}

int loadFromCache() {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string s, e;
    if (readCapped(copyPath(), s) && valid(s, e)) {
        g_loadedMtime = mtimeOf(copyPath());
        if (g_publish) g_publish(s);
        return 1;
    }
    int n = 0;
    for (const char* g : {"amateur", "weather"}) {
        std::string j;
        if (!readCapped(satDir() + "/gp-" + g + ".json", j) || j.find("\"NORAD_CAT_ID\"") == std::string::npos) continue;
        if (g_legacy) g_legacy(g, j);
        ++n;
    }
    return n;
}

bool reloadIfChanged() {
    std::lock_guard<std::mutex> lk(g_mtx);
    const long long m = mtimeOf(copyPath());
    if (m == 0 || m == g_loadedMtime) return false;
    std::string s, e;
    if (!readCapped(copyPath(), s) || !valid(s, e)) return false;
    g_loadedMtime = m;
    if (g_publish) g_publish(s);
    return true;
}

bool needsRefresh() {
    std::lock_guard<std::mutex> lk(g_mtx);
    State s = readState();
    return eligible(s, nowT(), mtimeOf(copyPath()) != 0);
}

int refresh(std::string& err) {
    std::lock_guard<std::mutex> lk(g_mtx);
    err.clear();
    FileLock fl;
    if (!fl.take()) return 0;                          // ★ another radio process is asking right now
    State s = readState();
    const std::time_t now = nowT();
    const bool haveCopy = mtimeOf(copyPath()) != 0;
    if (!eligible(s, now, haveCopy)) return 0;         // ★ re-checked under the lock: the other process may just have
    seedOf(s);                                         // this machine's daily offset, chosen on its first attempt
    // ★★ the attempt and a provisional hold BEFORE the request: a crash mid-fetch still waits
    s.attempt = now;
    s.until = std::max<long long>(s.until, now + kHoldFloorSec);
    writeState(s);

    Request rq;
    rq.url = kUrl;
    // ★ TESTS ONLY: a fake mirror on THIS machine (scripts/test-server-sat.mjs). Loopback http only — the variable can
    //   never point a server anywhere else (and never at CelesTrak).
    if (const char* t = std::getenv("VIBESERVER_SATS_TEST_URL"))
        if (std::strncmp(t, "http://127.0.0.1:", 17) == 0 && std::strlen(t) < 200) rq.url = t;
    rq.dest = copyPath() + ".part." + std::to_string((long)::getpid());
    if (haveCopy) { rq.ifNoneMatch = s.etag; rq.ifModifiedSince = s.lastmod; }
    std::remove(rq.dest.c_str());
    Response rs;
    (g_fetcher ? g_fetcher : Fetcher(curlFetch))(rq, rs);   // ★ ONE request — no retry, whatever it answers

    if (rs.code == 304 && haveCopy) {
        std::remove(rq.dest.c_str());
        s.checked = now; s.fails = 0; s.code = 304; s.until = 0; s.msg.clear();
        { std::string cur; s.stale = readCapped(copyPath(), cur) && dataAgeSec(cur, now) > kStaleDataSec ? 1 : 0; }
        writeState(s);
        return 0;
    }
    if (rs.code != 200) {
        std::remove(rq.dest.c_str());
        hold(s, now, rs.code == 304 ? 304 : rs.code, rs.msg.empty() ? "no answer" : rs.msg, haveCopy, rs.retryAfter);
        err = s.msg;
        return 0;
    }
    std::string body, why;
    if (!readCapped(rq.dest, body) || body.empty() || !valid(body, why)) {
        std::remove(rq.dest.c_str());
        hold(s, now, 200, body.empty() ? "empty, or larger than any sats.json" : why, haveCopy);
        err = s.msg;
        return 0;
    }
    if (std::rename(rq.dest.c_str(), copyPath().c_str()) != 0) {
        std::remove(rq.dest.c_str());
        hold(s, now, 200, "the copy could not be written", haveCopy);
        err = s.msg;
        return 0;
    }
    s.checked = now; s.fails = 0; s.code = 200; s.until = 0; s.msg.clear();
    s.etag = rs.etag; s.lastmod = rs.lastModified;
    s.stale = dataAgeSec(body, now) > kStaleDataSec ? 1 : 0;
    writeState(s);
    g_loadedMtime = mtimeOf(copyPath());
    if (g_publish) g_publish(body);
    return 1;
}

std::string ownerMessage() { std::lock_guard<std::mutex> lk(g_mtx); return readState().msg; }

std::string listenerNote() {
    std::lock_guard<std::mutex> lk(g_mtx);
    const State s = readState();
    if (s.msg.empty()) return "";
    if (s.code == 0) return "this server could not reach vibesdr.net for the satellite data";
    return "vibesdr.net's satellite data could not be downloaded (HTTP " + std::to_string(s.code) + ")";
}

std::string statusJson() {
    std::lock_guard<std::mutex> lk(g_mtx);
    State s = readState();
    const std::time_t now = nowT();
    const long long m = mtimeOf(copyPath());
    long long next = 0;
    if (s.until > now) next = s.until;
    else if (m) next = std::max<long long>(s.checked + dueAfterCheck(s), s.attempt + kMinAttemptSec);
    std::string e;
    for (char c : s.msg) { if (c == '"' || c == '\\') e += '\\'; if ((unsigned char)c >= 0x20) e += c; }
    return std::string("{\"source\":\"vibesdr.net\",\"url\":\"") + kUrl + "\",\"have\":" + (m ? "true" : "false")
         + ",\"checked\":" + std::to_string(s.checked) + ",\"nextTry\":" + std::to_string(next)
         + ",\"code\":" + std::to_string(s.code) + ",\"failures\":" + std::to_string(s.fails)
         + ",\"message\":\"" + e + "\"}";
}

long long lastSynced() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (mtimeOf(copyPath()) == 0) return 0;
    return readState().checked;   // ★ set only by a 200 or a 304 — the last GOOD answer
}
bool haveCopy() { std::lock_guard<std::mutex> lk(g_mtx); return mtimeOf(copyPath()) != 0; }
bool lockBusy() {
    ensureDir();
    const int fd = ::open(lockPath().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const bool busy = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!busy) ::flock(fd, LOCK_UN);
    ::close(fd);
    return busy;
}

long parseRetryAfter(const std::string& v0, std::time_t now) {
    std::string v = v0;
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    if (v.empty()) return 0;
    bool digits = v.size() <= 9;
    for (char c : v) if (c < '0' || c > '9') digits = false;
    if (digits) return std::atol(v.c_str());
    // an HTTP date: "Fri, 20 Nov 2026 16:08:00 GMT"
    std::tm t{};
    const char* end = ::strptime(v.c_str(), "%a, %d %b %Y %H:%M:%S", &t);
    if (!end) return 0;
    const std::time_t at = ::timegm(&t);
    return at > now ? (long)(at - now) : 0;
}

// ── ★★ the runner ──────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
std::mutex g_runMtx;
std::condition_variable g_runCv;
bool g_kicked = false;           // under g_runMtx
RunnerHooks g_hooks;             // under g_runMtx
std::atomic<bool> g_runnerUp{false}, g_fetching{false};

RunnerHooks hooksCopy() { std::lock_guard<std::mutex> lk(g_runMtx); return g_hooks; }
/** Wait up to `sec`, or until kicked. Returns true when kicked (and clears it). */
bool waitOrKick(long sec) {
    std::unique_lock<std::mutex> lk(g_runMtx);
    g_runCv.wait_for(lk, std::chrono::seconds(sec), [] { return g_kicked; });
    const bool k = g_kicked;
    g_kicked = false;
    return k;
}
void round(bool kicked, std::string& lastLogged) {
    RunnerHooks h = hooksCopy();
    reloadIfChanged();   // ★ another radio process on this machine fetched it
    // ★★ ONLY WHEN USED: no radio here reaches a listed downlink = nothing is asked at all
    if (h.wanted && h.wanted()) {
        if (needsRefresh()) {
            g_fetching.store(true);
            std::string e;
            const int r = refresh(e);
            if (r > 0 && h.log) h.log("satellite data refreshed from vibesdr.net");
            // ★ the owner's words, in the log ONCE per new failure (the setup page shows them too)
            if (!e.empty() && e != lastLogged && h.log) h.log("satellite data not refreshed \xe2\x80\x94 " + e);
            lastLogged = e;
        }
        // ★ kicked while ANOTHER process on the machine is asking: wait for its copy (≤ 60 s), not for the next round
        if (kicked) {
            g_fetching.store(true);
            for (int i = 0; i < 30 && lockBusy(); ++i) std::this_thread::sleep_for(std::chrono::seconds(2));
            reloadIfChanged();
        }
    }
    g_fetching.store(false);
    if (h.note) h.note(listenerNote());
}
}  // namespace

void startDaily(RunnerHooks hooks, int firstDelaySec) {
    { std::lock_guard<std::mutex> lk(g_runMtx); g_hooks = std::move(hooks); }
    bool f = false;
    if (!g_runnerUp.compare_exchange_strong(f, true)) return;
    std::thread([firstDelaySec] {
        std::string lastLogged;
        // ★ a few minutes' jitter first (a fleet restarted together does not ask together) — unless a listener opens
        //   SATELLITE on a server with nothing: then at once (item 9)
        bool kicked = waitOrKick(std::max(0, firstDelaySec));
        for (;;) {
            round(kicked, lastLogged);
            kicked = waitOrKick(10 * 60);
        }
    }).detach();
}

bool kick() {
    if (!g_runnerUp.load()) return false;
    if (g_fetching.load()) return true;
    const bool go = needsRefresh() || lockBusy();
    if (!go) return false;
    g_fetching.store(true);   // ★ at once, so the reply that triggered it already says so
    { std::lock_guard<std::mutex> lk(g_runMtx); g_kicked = true; }
    g_runCv.notify_all();
    return true;
}
bool fetching() { return g_fetching.load(); }

}  // namespace vssat
