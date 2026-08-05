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
#include "eibi.h"

#include "../android/app/src/main/cpp/local_sdr_shim.h"

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

std::string runCmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

/** EiBi labels a season by the year it STARTED, so Jan/Feb belong to the previous year's winter.
 *  ★ Derived exactly as `src/services/eibi.ts` does — the app and the server must ask for the same
 *    file, or a listener's app and the receiver it is listening to disagree about what is on air. */
std::string seasonFile() {
    const std::time_t t = std::time(nullptr);
    std::tm g{};
    gmtime_r(&t, &g);
    const int m = g.tm_mon + 1, day = g.tm_mday, y = g.tm_year + 1900;
    bool summer;
    if (m > 3 && m < 10)      summer = true;
    else if (m < 3 || m > 10) summer = false;
    else if (m == 3)          summer = day >= 25;   // ~last Sunday of March
    else                      summer = day < 25;    // ~last Sunday of October
    int yy = y;
    if (!summer && m <= 2) yy = y - 1;
    char s[32];
    snprintf(s, sizeof s, "sked-%c%02d.csv", summer ? 'a' : 'b', yy % 100);
    return s;
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

std::string cachePath() { return "/var/lib/vibeserver/eibi.csv"; }
/** Which season the cache holds. ★ Stored, because the FILE cannot tell us: sked-a26.csv and
 *  sked-b26.csv are the same shape, and a cache fetched yesterday is still stale the moment the
 *  season turns over. Without this a rollover would wait for the age check to expire. */
std::string seasonPath() { return "/var/lib/vibeserver/eibi.season"; }

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

}  // namespace

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
    if (count > 0) vibe::LocalSdrShim::instance().setStationsJson(json);
    return count;
}

bool needsRefresh() {
    struct stat st{};
    if (stat(cachePath().c_str(), &st) != 0) return true;          // nothing cached
    if (readSmall(seasonPath()) != seasonFile()) return true;      // the season turned over
    const double ageSec = difftime(std::time(nullptr), st.st_mtime);
    return ageSec > 20 * 3600;                                     // a day old, near enough
}

int refresh(std::string& err) {
    const std::string file = seasonFile();
    // ★ To a temp file first, then parse, then move. A half-downloaded CSV overwriting a good
    //   cache would leave the receiver with a truncated schedule until the next successful fetch —
    //   the same reasoning as the atomic config write.
    // ★★★ THE TEMP FILE MUST SHARE A FILESYSTEM WITH THE DESTINATION. rename() cannot cross one
    //     (EXDEV), and this daemon runs under PrivateTmp — so a /tmp staging file would download
    //     perfectly, parse perfectly, and then silently fail to save, leaving the button reporting
    //     success and the cache empty. Same rule the atomic config write already follows: stage
    //     NEXT TO the target, not somewhere convenient.
    mkdir("/var/lib/vibeserver", 0755);
    const std::string tmp = "/var/lib/vibeserver/eibi.csv.tmp";
    const std::string cmd = "curl -fsSL --max-time 45 -o " + tmp +
                            " http://www.eibispace.de/dx/" + file + " 2>&1";
    const std::string out = runCmd(cmd);
    FILE* f = fopen(tmp.c_str(), "rb");
    if (!f) { err = out.empty() ? "could not reach eibispace.de" : out; return 0; }
    std::string csv;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) csv.append(buf, n);
    fclose(f);

    int count = 0;
    const std::string json = toStationsJson(csv, count);
    if (count <= 0) {
        // ★★ KEEP THE OLD CACHE. A season file that has not been published yet returns a short
        //    404 page, which parses to zero entries — and replacing a working schedule with
        //    nothing because the new season is a week away is the worst outcome available.
        unlink(tmp.c_str());
        err = "downloaded file had no usable entries (" + std::to_string(csv.size()) + " bytes)";
        return 0;
    }
    rename(tmp.c_str(), cachePath().c_str());
    if (FILE* f = fopen(seasonPath().c_str(), "wb")) { fwrite(file.data(), 1, file.size(), f); fclose(f); }
    vibe::LocalSdrShim::instance().setStationsJson(json);
    return count;
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

}  // namespace vseibi
