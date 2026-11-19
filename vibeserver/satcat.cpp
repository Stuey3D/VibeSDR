// V12 SATELLITE ENGINE — CelesTrak GP data for a headless VibeServer (docs/v12/SATELLITE-ENGINE.md §6).
//
// ★★★ CELESTRAK'S ETIQUETTE IS THE DESIGN (brief §6.1 TRAP). They ask for no more than one download of the same data
//     every couple of hours, an immediate stop on any non-200 answer, and they firewall repeat offenders by IP — and
//     Brazilian hosts on mobile data share carrier CGNAT addresses, so one badly behaved server could get a whole
//     carrier blocked. So:
//       · the cache's AGE decides (12 h), never uptime — a restart does not fetch;
//       · ONE attempt marker per MACHINE, in the shared data dir: every radio process on the box sees the others' tries,
//         and none fetches within 2 h of any attempt, good or bad;
//       · a non-200 stops the round at once (the other groups are not tried) and backs off 6 h, doubling to a day;
//       · a few minutes' random jitter before the first fetch after start-up, so a fleet restarted together (an apt
//         upgrade) does not arrive together.
// ★★ OFFLINE-SAFE: a failed, refused, truncated or non-JSON download never touches the cache; the cache is published at
//    start-up whatever its age (the engine shows STALE / the age, and refuses past 14 days).
// ★ OMM JSON, not TLE: six-digit catalogue numbers (brief §6.1 TRAP).
// ★ Phase 2 (brief §6.2): a vibesdr.net mirror joining SatNOGS transmitters into sats.json, synced like EiBi. Until it
//   exists the server asks CelesTrak itself with the rules above — an open decision for Stuart (SATELLITE-ENGINE.md).
#include "satcat.h"
#include "proc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <algorithm>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace vssat {
namespace {

std::string g_dir = "/var/lib/vibeserver";
std::function<void(const std::string&, const std::string&)> g_publish;
std::vector<std::string> g_groups = {"amateur", "weather"};
Fetcher g_fetcher;
std::function<std::time_t()> g_clock;
std::mutex g_mtx;
std::string g_lastError;

std::time_t nowT() { return g_clock ? g_clock() : std::time(nullptr); }
std::string satDir() { return g_dir + "/sat"; }
std::string cachePath(const std::string& g) { return satDir() + "/gp-" + g + ".json"; }
std::string markerPath() { return satDir() + "/last-attempt"; }

void ensureDir() {
    ::mkdir(g_dir.c_str(), 0755);
    ::mkdir(satDir().c_str(), 0755);
}

/** "<unix> <backoffSec> <failures>" */
void readMarker(std::time_t& at, long& backoff, int& fails) {
    at = 0; backoff = kMinAttemptSec; fails = 0;
    FILE* f = std::fopen(markerPath().c_str(), "rb");
    if (!f) return;
    long long a = 0; long b = 0; int c = 0;
    if (std::fscanf(f, "%lld %ld %d", &a, &b, &c) >= 1) { at = (std::time_t)a; if (b > 0) backoff = b; fails = c; }
    std::fclose(f);
}
void writeMarker(std::time_t at, long backoff, int fails) {
    ensureDir();
    const std::string tmp = markerPath() + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "%lld %ld %d\n", (long long)at, backoff, fails);
    std::fclose(f);
    std::rename(tmp.c_str(), markerPath().c_str());
}

bool readCapped(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16384]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > kMaxGroupBytes) { std::fclose(f); out.clear(); return false; }
    }
    std::fclose(f);
    return true;
}

/** A CelesTrak GP JSON answer looks like `[{"OBJECT_NAME":…,"NORAD_CAT_ID":…}, …]` — and an error is plain text ("No
 *  GP data found") or an HTML page. Cheap structural check; the engine's parser does the real work. */
bool looksLikeGp(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    if (i >= s.size() || s[i] != '[') return false;
    return s.find("\"NORAD_CAT_ID\"") != std::string::npos && s.find("\"MEAN_MOTION\"") != std::string::npos;
}

Fetch curlFetch(const std::string& url, const std::string& dest, std::string& msg) {
    std::string out;
    // ★ -f: an HTTP error is exit 22, never a saved error page. A plain product name as the agent (no address, no
    //   owner details — the privacy rule).
    vibeproc::Child c = vibeproc::spawn({"curl", "-fsSL", "--max-time", "45", "-A", "VibeServer", "-o", dest, url}, true);
    if (c.ok()) { char buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, c.out)) > 0) out.append(buf, n); }
    const int rc = vibeproc::reap(c);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (rc == 0) return Fetch::Ok;
    msg = out.empty() ? (rc < 0 ? "curl could not be run" : "curl exited " + std::to_string(rc)) : out;
    return rc == 22 ? Fetch::Refused : Fetch::Failed;
}

long ageOf(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return (long)std::difftime(nowT(), st.st_mtime);
}

}  // namespace

void setDir(const std::string& dir) { if (!dir.empty()) g_dir = dir; }
void setPublisher(std::function<void(const std::string&, const std::string&)> fn) { g_publish = std::move(fn); }
void setGroups(const std::vector<std::string>& groups) { if (!groups.empty()) g_groups = groups; }
void setFetcher(Fetcher fn) { g_fetcher = std::move(fn); }
void setClock(std::function<std::time_t()> fn) { g_clock = std::move(fn); }
std::string urlFor(const std::string& group) { return "https://celestrak.org/NORAD/elements/gp.php?GROUP=" + group + "&FORMAT=json"; }

int loadFromCache() {
    std::lock_guard<std::mutex> lk(g_mtx);
    int n = 0;
    for (const auto& g : g_groups) {
        std::string s;
        if (!readCapped(cachePath(g), s) || !looksLikeGp(s)) continue;
        if (g_publish) g_publish(g, s);
        ++n;
    }
    return n;
}

bool needsRefresh() {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::time_t at; long backoff; int fails;
    readMarker(at, backoff, fails);
    const std::time_t now = nowT();
    if (at > 0 && now >= at && now - at < std::max(backoff, kMinAttemptSec)) return false;
    // ★ a marker in the FUTURE is a clock that jumped back (an RTC-less Pi): honour it no longer than a day
    if (at > now && at - now < 86400) return false;
    for (const auto& g : g_groups) {
        const long a = ageOf(cachePath(g));
        if (a < 0 || a > kRefreshSec) return true;
    }
    return false;
}

int refresh(std::string& err) {
    std::lock_guard<std::mutex> lk(g_mtx);
    ensureDir();
    std::time_t at; long backoff; int fails;
    readMarker(at, backoff, fails);
    const std::time_t now = nowT();
    int ok = 0;
    for (const auto& g : g_groups) {
        const std::string dest = cachePath(g), tmp = dest + ".part";
        std::string msg;
        std::remove(tmp.c_str());
        const Fetch r = (g_fetcher ? g_fetcher : Fetcher(curlFetch))(urlFor(g), tmp, msg);
        std::string body;
        if (r == Fetch::Ok && (!readCapped(tmp, body) || !looksLikeGp(body))) {
            msg = body.empty() ? "the answer was empty or larger than any GP group" : "the answer was not GP JSON";
            std::remove(tmp.c_str());
            err = "CelesTrak " + g + ": " + msg;
            // ★ a 200 that is not the data (a captive portal, "No GP data found") is treated as a refusal
            fails += 1; backoff = std::min<long>(86400, kBackoffFailSec << std::min(fails - 1, 2));
            writeMarker(now, backoff, fails);
            g_lastError = err;
            return ok;
        }
        if (r != Fetch::Ok) {
            std::remove(tmp.c_str());
            err = "CelesTrak " + g + ": " + msg;
            fails += 1; backoff = std::min<long>(86400, kBackoffFailSec << std::min(fails - 1, 2));
            writeMarker(now, backoff, fails);
            g_lastError = err;
            return ok;   // ★ STOP: the other groups are not asked for
        }
        if (std::rename(tmp.c_str(), dest.c_str()) != 0) { std::remove(tmp.c_str()); err = "could not write the cache"; continue; }
        if (g_publish) g_publish(g, body);
        ++ok;
    }
    writeMarker(now, kMinAttemptSec, 0);
    g_lastError.clear();
    return ok;
}

std::string statusJson() {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::time_t at; long backoff; int fails;
    readMarker(at, backoff, fails);
    std::string j = "{\"groups\":[";
    bool first = true;
    for (const auto& g : g_groups) {
        struct stat st{};
        const bool have = ::stat(cachePath(g).c_str(), &st) == 0;
        if (!first) j += ",";
        first = false;
        j += "{\"group\":\"" + g + "\",\"age\":" + std::to_string(have ? ageOf(cachePath(g)) : -1)
           + ",\"bytes\":" + std::to_string(have ? (long long)st.st_size : 0LL) + "}";
    }
    std::string e;
    for (char c : g_lastError) { if (c == '"' || c == '\\') e += '\\'; if ((unsigned char)c >= 0x20) e += c; }
    j += "],\"lastAttempt\":" + std::to_string((long long)at) + ",\"backoff\":" + std::to_string(backoff)
       + ",\"failures\":" + std::to_string(fails) + ",\"lastError\":\"" + e + "\"}";
    return j;
}

}  // namespace vssat
