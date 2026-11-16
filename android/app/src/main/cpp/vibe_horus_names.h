// vibe_horus_names.h — V12 balloons: Horus Binary payload IDs → callsigns (docs/v12/BALLOONS.md §Horus payload names).
//
// A Horus v1/v2 packet carries a number, not a name (v1: 0–255, v2: 256–65535). Project Horus publishes the list that
// gives each number its callsign — payload_id_list.txt in the horusdemodlib repository (LGPL-2.1; the list itself is
// factual data, "ID, CALLSIGN" per line, '#' comments, new IDs requested by issue / pull request). horusdemodlib fetches
// it at run time in the same way; nothing of theirs is compiled in here, and the parser is our own.
//
// ★★ THE SERVER FETCHES, NEVER THE CLIENT — and only when it is needed: the first Horus packet a server decodes asks for a
//    name, and that starts a download if the copy on disk is missing or more than a day old. AT MOST ONCE A DAY, across
//    restarts too (the time of the last attempt is kept beside the copy, successful or not). A server that never hears
//    Horus never downloads anything.
// ★★ BOUNDED (256 MB boxes): the download is refused past kMaxBytes, at most kMaxIds names are kept, each at most
//    kMaxName characters of A–Z a–z 0–9 - _ . — anything else on a line and the line is skipped.
// ★ No list (no network, no transport — iOS, a fresh Android with no downloader — or a payload not in it): "HORUS-<id>",
//   as before.
// ★ The transport is the ADS-B aircraft data's (curl on the desktop servers, OkHttp on Android — vibe_adsb_data_update.h
//   Updater::fetcher()), handed over where that one is installed (main.cpp, vibeserver_api.cpp, the Android JNI). This
//   header itself depends on nothing, so every decoder host can include it.
#pragma once
#include <functional>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>

namespace vibehorus {

constexpr const char* kListUrl = "https://raw.githubusercontent.com/projecthorus/horusdemodlib/master/payload_id_list.txt";
constexpr size_t kMaxBytes = 512 * 1024;   ///< today's list is ~15 KB
constexpr size_t kMaxIds = 65536;
constexpr size_t kMaxName = 20;
constexpr int64_t kDaySec = 86400;
/** GET `url` into the file `dest`; the HTTP status back (0 = it failed). */
using Fetch = std::function<int(const std::string& url, const std::string& dest)>;

/** "ID, CALLSIGN" lines → the map (bounded). Comments ('#'), blank lines and anything malformed are skipped. */
inline size_t parseList(const std::string& text, std::map<int, std::string>& out) {
    std::istringstream in(text);
    std::string line;
    size_t n = 0;
    while (std::getline(in, line) && out.size() < kMaxIds) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = 0; while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
        if (a >= line.size() || line[a] == '#') continue;
        const size_t comma = line.find(',', a);
        if (comma == std::string::npos) continue;
        const std::string ids = line.substr(a, comma - a);
        bool digits = !ids.empty() && ids.size() <= 5;
        for (char c : ids) if (c != ' ' && (c < '0' || c > '9')) digits = false;
        if (!digits) continue;
        const long id = std::strtol(ids.c_str(), nullptr, 10);
        if (id < 0 || id > 65535) continue;
        std::string nm = line.substr(comma + 1);
        while (!nm.empty() && (nm.front() == ' ' || nm.front() == '\t')) nm.erase(0, 1);
        while (!nm.empty() && (nm.back() == ' ' || nm.back() == '\t')) nm.pop_back();
        bool ok = !nm.empty() && nm.size() <= kMaxName;
        for (char c : nm) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) ok = false;
        if (!ok) continue;
        out[(int)id] = nm;
        ++n;
    }
    return n;
}

class Names {
public:
    /** Never destroyed (the ADS-B updater's reason: a detached download thread may outlive main's statics). */
    static Names& get() { static Names* n = new Names; return *n; }

    /** `<data dir>/horus`: the copy on disk (read now) and the time of the last attempt. */
    void setDir(const std::string& d) {
        ::mkdir(d.c_str(), 0755);
        { std::lock_guard<std::mutex> lk(m_); dir_ = d; }
        load_();
    }
    void setFetcher(Fetch f) { std::lock_guard<std::mutex> lk(m_); fetch_ = std::move(f); }

    /** The callsign for a payload ID, or "" — and, the first time it is needed each day, a background refresh. */
    std::string name(int id) {
        maybeRefresh_();
        std::lock_guard<std::mutex> lk(m_);
        auto it = names_.find(id);
        return it == names_.end() ? std::string() : it->second;
    }
    /** "HORUS-<id>" when the list does not name it. */
    std::string display(int id) {
        const std::string n = name(id);
        return n.empty() ? "HORUS-" + std::to_string(id) : n;
    }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return names_.size(); }
    bool busy() const { return busy_.load(); }

    /** For tests: the current time (seconds). */
    std::function<int64_t()> clock = [] { return (int64_t)std::time(nullptr); };
    /** Run the refresh on the calling thread (tests); the server's is a detached thread. */
    bool inlineRefresh = false;

private:
    std::string listPath_() const { return dir_ + "/payload_id_list.txt"; }
    std::string checkedPath_() const { return dir_ + "/checked"; }
    void load_() {
        std::string path; { std::lock_guard<std::mutex> lk(m_); if (dir_.empty()) return; path = listPath_(); }
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        std::stringstream ss; ss << f.rdbuf();
        std::string t = ss.str();
        if (t.size() > kMaxBytes) return;
        std::map<int, std::string> m;
        parseList(t, m);
        std::lock_guard<std::mutex> lk(m_);
        names_.swap(m);
    }
    int64_t lastChecked_() {
        std::string p; { std::lock_guard<std::mutex> lk(m_); p = checkedPath_(); }
        std::ifstream f(p); int64_t t = 0; f >> t; return t;
    }
    void maybeRefresh_() {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (dir_.empty() || !fetch_) return;
        }
        const int64_t now = clock();
        if (now - lastTry_.load() < kDaySec) return;          // ★ this process already tried today
        if (now - lastChecked_() < kDaySec) { lastTry_ = now; return; }   // ★ …or another process / an earlier run did
        if (busy_.exchange(true)) return;
        lastTry_ = now;
        auto job = [this, now] {
            std::string dir; Fetch f;
            { std::lock_guard<std::mutex> lk(m_); dir = dir_; f = fetch_; }
            { std::ofstream c(dir + "/checked"); c << now; }   // ★ the attempt counts, success or not: at most once a day
            const std::string dl = dir + "/payload_id_list.download";
            const int status = f(kListUrl, dl);
            struct stat sb{};
            bool good = status == 200 && ::stat(dl.c_str(), &sb) == 0 && sb.st_size > 0 && (size_t)sb.st_size <= kMaxBytes;
            if (good) {
                std::ifstream in(dl, std::ios::binary); std::stringstream ss; ss << in.rdbuf();
                std::map<int, std::string> m;
                good = parseList(ss.str(), m) > 0;
                if (good) good = std::rename(dl.c_str(), (dir + "/payload_id_list.txt").c_str()) == 0;
                if (good) { std::lock_guard<std::mutex> lk(m_); names_.swap(m); }
            }
            if (!good) std::remove(dl.c_str());
            busy_ = false;
        };
        if (inlineRefresh) job(); else std::thread(job).detach();
    }

    std::mutex m_;
    std::string dir_;
    Fetch fetch_;
    std::map<int, std::string> names_;
    std::atomic<bool> busy_{false};
    std::atomic<int64_t> lastTry_{-kDaySec};
};

} // namespace vibehorus
