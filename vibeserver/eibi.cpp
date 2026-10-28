// EiBi shortwave schedule, fetched and parsed BY THE SERVER.
//
// ★★★ WHY THIS EXISTS. The station list served at GET /stations is supplied by whoever embeds the
// shim, and until now that was always an APP — the phone and the Mac already download and cache
// EiBi, so they simply hand it over. A headless VibeServer embeds nothing: the Pi has no app, so
// `g_stationsJson` stayed empty and the web client's search had no schedule at all. Every listener
// on the demo has been searching a list with nothing in it (Stuart, 2026-08-05: "EiBi in the
// initial setup screen, same as it is on the Mac and Android").
//
// ★★ AND THE BROWSER CANNOT DO IT ITSELF. eibispace.de sends no CORS headers, so a page served
// from the receiver cannot fetch it — which is the whole reason the list is served BY the server
// rather than fetched by the client. Same model as UberSDR: the server presents the stations.
//
// ★ curl rather than a linked HTTP client. The daemon has no TLS stack of its own and this is a
//   once-a-day fetch of a plain-HTTP CSV; adding libcurl (or worse, hand-rolling a client) to the
//   build for that would be a poor trade. If the fetch fails we keep whatever cache we have, which
//   is the behaviour the app already has and the only one that keeps a receiver useful offline.
//
// ★★★ A SEASON EiBi HAS NOT PUBLISHED YET IS NOT AN ERROR (NickB, 2026-10-27: "curl: (22) The
//     requested URL returned error: 404"). The season switch below is a DATE GUESS — 25 March and
//     25 October — and EiBi publishes the new file when it is ready, which can be days or weeks
//     later. B26 was still a 404 two days after we switched to it. Before this, the server kept its
//     old cache but told the owner a raw curl error, asked again on every hourly wake, and a
//     server with NO cache had no schedule at all. Now: ask for this season; if it is not there,
//     use the PREVIOUS season's file (which is what EiBi itself still has up), say so in plain
//     words, and ask for the new one again every few hours until it appears.
//     ★ The same rule is in src/services/eibi.ts (app) and vibeserver/mac/EibiStations.swift —
//       keep all three in step.
#include "eibi.h"
#include "proc.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace vseibi {
namespace {

std::function<void(const std::string&)> g_publish;
Fetcher g_fetcher;
std::function<std::time_t()> g_clock;

std::time_t nowT() { return g_clock ? g_clock() : std::time(nullptr); }
void publish(const std::string& json) { if (g_publish) g_publish(json); }

std::string fileFor(const std::string& code) { return "sked-" + code + ".csv"; }
/** "sked-a26.csv" → "a26". ★ The season marker has always held the FILE name, so a cache written
 *  by an older server still reads correctly. */
std::string codeOfFile(const std::string& file) {
    if (file.size() == 12 && file.compare(0, 5, "sked-") == 0) return file.substr(5, 3);
    return "";
}

/** The real fetch: curl, no shell, to `dest`. ★ -f makes an HTTP error an exit status (22) rather
 *  than an HTML error page saved as the schedule; the status code in curl's own message tells a
 *  file that is not there yet (404/410) from a server or network that is broken. */
Fetch curlFetch(const std::string& url, const std::string& dest, std::string& msg) {
    std::string out;
    vibeproc::Child c = vibeproc::spawn(
        {"curl", "-fsSL", "--max-time", "45", "-o", dest, url}, true);
    if (c.ok()) {
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, c.out)) > 0) out.append(buf, n);
    }
    const int rc = vibeproc::reap(c);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (rc == 0) return Fetch::Ok;
    if (rc == 22) {                              // HTTP error; curl says "... returned error: 404"
        const size_t at = out.rfind("error: ");
        const int code = at == std::string::npos ? 0 : atoi(out.c_str() + at + 7);
        if (code == 404 || code == 410) return Fetch::NotPublished;
    }
    msg = out.empty() ? (rc < 0 ? "curl could not be run" : "curl exited " + std::to_string(rc)) : out;
    return Fetch::Failed;
}

std::string esc(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) continue;
        else out += c;
    }
    return out;
}

/** Windows-1252 → UTF-8. ★ NOT optional: EiBi is full of accented station names, and pushing
 *  raw 8-bit bytes into JSON produces invalid UTF-8 that a browser's JSON.parse rejects OUTRIGHT —
 *  so one accented name would cost the entire station list, not one entry. */
std::string toUtf8(const std::string& in) {
    static const unsigned short kCp1252[32] = {
        0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,
        0x0152,0x008D,0x017D,0x008F,0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
        0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178 };
    std::string out;
    out.reserve(in.size() + in.size() / 8);
    for (unsigned char c : in) {
        unsigned int cp = c;
        if (c >= 0x80 && c <= 0x9F) cp = kCp1252[c - 0x80];
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
               out += (char)(0x80 | (cp & 0x3F)); }
    }
    return out;
}

/** CSV → the JSON shape the web client's search already reads (ServerStation).
 *  ★ Fields and column order match `parse()` in src/services/eibi.ts. Deriving the same data two
 *    different ways is how the app and the web client end up disagreeing about a station. */
std::string toStationsJson(const std::string& csv, int& count) {
    std::string j = "[";
    count = 0;
    size_t pos = 0;
    while (pos <= csv.size()) {
        size_t nl = csv.find('\n', pos);
        if (nl == std::string::npos) nl = csv.size();
        std::string line = csv.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) { if (pos > csv.size()) break; else continue; }

        std::vector<std::string> f;
        size_t p = 0;
        while (true) {
            size_t sc = line.find(';', p);
            if (sc == std::string::npos) { f.push_back(line.substr(p)); break; }
            f.push_back(line.substr(p, sc - p));
            p = sc + 1;
        }
        if (f.size() < 6) continue;
        const double khz = atof(f[0].c_str());
        if (!(khz > 0)) continue;                 // skips the header row too
        // ★★ FINITE AND BOUNDED before the integer cast. atof accepts "inf" and "1e400", and
        //    (long long)inf is undefined behaviour — this file comes off the network over plain
        //    http, so a hostile or corrupted line must be skipped, not trusted. 1 GHz is far above
        //    anything EiBi lists (it is a shortwave schedule) and far below the cast's limit.
        if (!std::isfinite(khz) || khz > 1.0e6) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && isspace((unsigned char)s.back()))  s.pop_back();
            return s;
        };
        const std::string station = trim(f[4]);
        if (station.empty()) continue;
        const std::string itu  = trim(f[3]);
        const std::string lang = trim(f[5]);
        const std::string time = trim(f[1]);

        if (count) j += ',';
        j += "{\"name\":\"" + esc(toUtf8(station)) + "\",\"frequency\":"
           + std::to_string((long long)(khz * 1000.0 + 0.5))
           + ",\"source\":\"eibi\"";
        if (!itu.empty())  j += ",\"itu\":\"" + esc(toUtf8(itu)) + "\"";
        // ★ The comment carries the time and language, which is what makes two entries on the same
        //   frequency tellable apart — without it a busy channel is a list of identical rows.
        std::string comment = time;
        if (!lang.empty()) comment += (comment.empty() ? "" : " · ") + lang;
        if (!comment.empty()) j += ",\"comment\":\"" + esc(toUtf8(comment)) + "\"";
        j += ",\"mode\":\"am\"}";
        count++;
        if (nl == csv.size()) break;
    }
    j += "]";
    return j;
}


/** ★ Settable, for the same reason geoip's and asndb's are: /var/lib does not exist on a Mac, so
 *  a hardcoded path there means the schedule can never download. See vsDataDir() in main.cpp. */
std::string g_dir = "/var/lib/vibeserver";
std::string cachePath() { return g_dir + "/eibi.csv"; }
/** Which season the cache holds. ★ Stored, because the FILE cannot tell us: sked-a26.csv and
 *  sked-b26.csv are the same shape, and a cache fetched yesterday is still stale the moment the
 *  season turns over. Without this a rollover would wait for the age check to expire. */
std::string seasonPath() { return g_dir + "/eibi.season"; }
/** "sked-b26.csv <unix time>" — the season EiBi last told us it has not published, and when.
 *  ★ What keeps a fallback from re-asking on every wake, and what lets note() explain it. */
std::string triedPath() { return g_dir + "/eibi.tried"; }

std::string readSmall(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    char b[64] = {0};
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    std::string s(b, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
void writeSmall(const std::string& path, const std::string& s) {
    if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
}

std::string cachedSeason() { return codeOfFile(readSmall(seasonPath())); }

/** The unpublished season last asked for, and when (0 = never). */
std::string triedSeason(std::time_t& at) {
    const std::string s = readSmall(triedPath());
    const size_t sp = s.find(' ');
    at = sp == std::string::npos ? 0 : (std::time_t)atoll(s.c_str() + sp + 1);
    return codeOfFile(s.substr(0, sp));
}

bool cacheAge(double& ageSec) {
    struct stat st{};
    if (stat(cachePath().c_str(), &st) != 0) return false;
    ageSec = difftime(nowT(), st.st_mtime);
    return true;
}
constexpr double kDailySec = 20 * 3600;            // a day old, near enough

/** Fetch one season's file to the temp file and parse it. Ok = `json`/`count` hold a usable
 *  schedule and the temp file is waiting to be committed; anything else leaves no temp file.
 *  ★ A file that downloads but parses to nothing counts as NOT PUBLISHED: some servers answer a
 *    missing file with a 200 and an HTML page, and that must never replace a working schedule. */
Fetch fetchSeason(const std::string& code, const std::string& tmp, std::string& json, int& count,
                  std::string& msg) {
    // ★★ PLAIN http, ON PURPOSE AND FOR NOW (checked 2026-10-03): https://www.eibispace.de presents
    //    an EXPIRED certificate, so an https URL here fails every fetch. The content is treated as
    //    untrusted either way (the parser above bounds every number it casts). Move to https the
    //    day the site's certificate is valid again — and keep the Mac app's copy in step
    //    (vibeserver/mac/EibiStations.swift).
    const std::string url = "http://www.eibispace.de/dx/" + fileFor(code);
    unlink(tmp.c_str());
    const Fetch r = (g_fetcher ? g_fetcher : Fetcher(curlFetch))(url, tmp, msg);
    if (r != Fetch::Ok) { unlink(tmp.c_str()); return r; }
    FILE* f = fopen(tmp.c_str(), "rb");
    if (!f) { msg = "the download left no file"; return Fetch::Failed; }
    std::string csv;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) csv.append(buf, n);
    fclose(f);
    count = 0;
    json = toStationsJson(csv, count);
    if (count <= 0) { unlink(tmp.c_str()); return Fetch::NotPublished; }
    return Fetch::Ok;
}

}  // namespace

void setDir(const std::string& dir) { if (!dir.empty()) g_dir = dir; }
void setPublisher(std::function<void(const std::string&)> fn) { g_publish = std::move(fn); }
void setFetcher(Fetcher fn) { g_fetcher = std::move(fn); }
void setClock(std::function<std::time_t()> fn) { g_clock = std::move(fn); }

std::string seasonCodeAt(std::time_t t) {
    std::tm g{};
    gmtime_r(&t, &g);
    const int m = g.tm_mon + 1, day = g.tm_mday, y = g.tm_year + 1900;
    bool summer;
    if (m > 3 && m < 10)      summer = true;
    else if (m < 3 || m > 10) summer = false;
    else if (m == 3)          summer = day >= 25;   // ~last Sunday of March
    else                      summer = day < 25;    // ~last Sunday of October
    int yy = y;
    // ★★ m <= 3, NOT m <= 2: 1–24 March is still the winter that started the previous October.
    //    "<= 2" asked for sked-b27.csv on 1 March 2027 — a file that will never exist.
    if (!summer && m <= 3) yy = y - 1;
    char s[8];
    snprintf(s, sizeof s, "%c%02d", summer ? 'a' : 'b', ((yy % 100) + 100) % 100);
    return s;
}

std::string previousSeason(const std::string& code) {
    if (code.size() != 3) return "";
    const int yy = atoi(code.c_str() + 1);
    char s[8];
    // ★ b26 → a26 (the same year's summer); a27 → b26 (the winter that STARTED the year before).
    if (code[0] == 'b') snprintf(s, sizeof s, "a%02d", yy);
    else                snprintf(s, sizeof s, "b%02d", (yy + 99) % 100);
    return s;
}

std::string seasonLabel(const std::string& code) {
    if (code.size() != 3) return code;
    std::string s = code;
    s[0] = (char)toupper((unsigned char)s[0]);
    return s + (code[0] == 'a' ? " (summer)" : " (winter)");
}

int loadFromCache() {
    FILE* f = fopen(cachePath().c_str(), "rb");
    if (!f) return 0;
    std::string csv;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) csv.append(buf, n);
    fclose(f);
    if (csv.empty()) return 0;
    int count = 0;
    const std::string json = toStationsJson(csv, count);
    if (count > 0) publish(json);
    return count;
}

bool needsRefresh() {
    double age = 0;
    if (!cacheAge(age)) return true;                                // nothing cached
    const std::time_t now = nowT();
    const std::string cur = seasonCodeAt(now);
    if (cachedSeason() == cur) return age > kDailySec;             // this season: daily
    // ★★ The season turned over. Ask — unless EiBi told us "not yet" for this very season within
    //    the last few hours, in which case the fallback we hold is the best there is.
    std::time_t at = 0;
    if (triedSeason(at) == cur && now >= at && now - at < kRetryUnpublishedSec) return false;
    return true;
}

int refresh(std::string& err) {
    err.clear();
    const std::time_t now = nowT();
    const std::string cur = seasonCodeAt(now), prev = previousSeason(cur);
    // ★ To a temp file first, then parse, then move. A half-downloaded CSV overwriting a good
    //   cache would leave the receiver with a truncated schedule until the next successful fetch —
    //   the same reasoning as the atomic config write.
    // ★★★ THE TEMP FILE MUST SHARE A FILESYSTEM WITH THE DESTINATION. rename() cannot cross one
    //     (EXDEV), and this daemon runs under PrivateTmp — so a /tmp staging file would download
    //     perfectly, parse perfectly, and then silently fail to save, leaving the button reporting
    //     success and the cache empty. Same rule the atomic config write already follows: stage
    //     NEXT TO the target, not somewhere convenient.
    mkdir(g_dir.c_str(), 0755);
    const std::string tmp = g_dir + "/eibi.csv.tmp";
    /* ★★★ NO SHELL — see vibeserver/proc.h. The file name is composed from the season and year
     *  here, so this one was not exploitable, but it is the same shape as the RadioDNS path that
     *  WAS, and leaving one shell command line in the daemon is how the next one gets written. */
    auto commit = [&](const std::string& code, const std::string& json) {
        rename(tmp.c_str(), cachePath().c_str());
        writeSmall(seasonPath(), fileFor(code));
        publish(json);
    };

    std::string json, msg;
    int count = 0;
    Fetch r = fetchSeason(cur, tmp, json, count, msg);
    if (r == Fetch::Ok) {
        commit(cur, json);
        unlink(triedPath().c_str());
        return count;
    }
    if (r == Fetch::Failed) {
        // ★ A REAL failure (no network, DNS, a 5xx) says so — that is the owner's to fix or wait
        //   out. The caller keeps serving the cache it has.
        err = "couldn't reach eibispace.de — " + msg;
        return 0;
    }

    // ── EiBi has not published this season yet: fall back to the one before ──────────────────
    writeSmall(triedPath(), fileFor(cur) + " " + std::to_string((long long)now));
    double age = 0;
    if (cachedSeason() == prev && cacheAge(age) && age <= kDailySec) {
        const int n = loadFromCache();                  // already holding it, and it is fresh
        if (n > 0) return n;
    }
    json.clear(); msg.clear(); count = 0;
    r = fetchSeason(prev, tmp, json, count, msg);
    if (r == Fetch::Ok) { commit(prev, json); return count; }
    // ★ Neither file: keep whatever cache there is (an older season beats an empty search).
    const int n = loadFromCache();
    if (n > 0) return n;
    err = r == Fetch::Failed
        ? "EiBi hasn't published the " + seasonLabel(cur) + " schedule yet, and the "
          + seasonLabel(prev) + " one could not be fetched — couldn't reach eibispace.de — " + msg
        : "EiBi hasn't published the " + seasonLabel(cur) + " schedule yet, and the "
          + seasonLabel(prev) + " file has no entries either";
    return 0;
}

std::string status() {
    struct stat st{};
    if (stat(cachePath().c_str(), &st) != 0) return "";
    char when[64];
    std::tm g{};
    gmtime_r(&st.st_mtime, &g);
    strftime(when, sizeof when, "%Y-%m-%d", &g);
    return when;
}

std::string note() {
    struct stat st{};
    if (stat(cachePath().c_str(), &st) != 0) return "";
    const std::string cur = seasonCodeAt(nowT()), cached = cachedSeason();
    if (cached.empty() || cached == cur) return "";
    std::time_t at = 0;
    if (triedSeason(at) != cur) return "";
    std::string using_ = cached;
    using_[0] = (char)toupper((unsigned char)using_[0]);
    return "EiBi hasn't published the " + seasonLabel(cur) + " schedule yet — using "
         + using_ + " until it appears.";
}

}  // namespace vseibi
