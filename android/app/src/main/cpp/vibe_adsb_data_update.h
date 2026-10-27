// vibe_adsb_data_update.h — the server keeps its own aircraft data fresh: a weekly, jittered, CONDITIONAL download
// of the two upstream sources into its data directory, rebuilt with vibe_adsb_data_build.h and swapped in
// atomically. Design: docs/v12/ADSB-DATA-RESEARCH.md §4.1–4.2 ("seed + weekly download").
//
// ★★★ A FAILURE NEVER BREAKS THE SERVER. Nothing here runs on a radio's path: one detached thread at the lowest
//     priority (vibeBulkThread — below decoders), and every failure (no network, a 500, a corrupt archive, a
//     licence that changed) keeps the tables the server already has and says why on the admin page. The old
//     file is replaced only by a rename of a COMPLETE, VALIDATED new one.
// ★★★ POLITE BY DESIGN (the upstreams are one person's GitHub repositories): once a week, at a time of week that
//     is random per server (so a fleet does not arrive together), with If-None-Match / If-Modified-Since so an
//     unchanged source costs a 304 and a few hundred bytes, a User-Agent that says who we are, and a back-off
//     after a failure (6 h, doubling to 2 days). ~10 MB a week when both changed.
// ★★ ONE MACHINE, ONE DOWNLOAD. Several radio processes share /var/lib/vibeserver: an flock() on adsb/.lock means
//    one of them fetches and the others simply see the new files (vibe_adsb_data.h re-maps a replaced file).
// ★★ THE OWNER CAN SWITCH IT OFF (admin page): the switch is a line in adsb/state.txt, so every process on the
//    machine reads the same answer and it survives restarts — no config schema change on four platforms.
// ★ TRANSPORT IS THE PLATFORM'S: curl on Linux/macOS (vibeserver/adsb_data_curl.h), OkHttp on Android
//   (VibeMapGL.fetchIf through JNI). This file owns the policy, the state, the build and the swap.
#pragma once
#include "vibe_adsb_data_build.h"
#include "vibe_thread.h"      // vibeBulkThread — below the decoders
#include <atomic>
#include <condition_variable>
#include <functional>
#include <random>
#include <sys/file.h>
#include <thread>

namespace vibeadsbdata {

constexpr const char* kVrsUrl = "https://codeload.github.com/vradarserver/standing-data/tar.gz/refs/heads/main";
constexpr const char* kMicUrl = "https://raw.githubusercontent.com/Mictronics/aircraft-database/main/indexedDB.zip";

/** status: 200 (body written to dest), 304 (unchanged, dest untouched), 0 = failed (error says why). */
struct FetchResult { int status = 0; std::string etag, lastModified, error; };
using Fetcher = std::function<FetchResult(const std::string& url, const std::string& dest,
                                          const std::string& ifNoneMatch, const std::string& ifModifiedSince)>;

/** adsb/state.txt — "key=value" lines. Small, human-readable, and every process on the machine reads it. */
class State {
public:
    bool load(const std::string& path) {
        kv_.clear();
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return false;
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const size_t eq = s.find('=');
            if (eq != std::string::npos) kv_[s.substr(0, eq)] = s.substr(eq + 1);
        }
        std::fclose(f);
        return true;
    }
    bool save(const std::string& path) const {
        std::string o, err;
        for (const auto& kv : kv_) o += kv.first + "=" + kv.second + "\n";
        return writeAtomic(path, o, err);
    }
    std::string get(const std::string& k, const std::string& d = "") const { auto it = kv_.find(k); return it == kv_.end() ? d : it->second; }
    int64_t num(const std::string& k, int64_t d = 0) const { const std::string v = get(k); return v.empty() ? d : std::strtoll(v.c_str(), nullptr, 10); }
    void set(const std::string& k, const std::string& v) {
        std::string c; for (char ch : v) if (ch != '\n' && ch != '\r') c += ch;   // ★ one value, one line
        kv_[k] = c;
    }
    void set(const std::string& k, int64_t v) { kv_[k] = std::to_string(v); }
private:
    std::map<std::string, std::string> kv_;
};

class Updater {
public:
    /** ★★★ NEVER DESTROYED (Stuart's Pi 2, 2026-10-26 23:53 UTC: SIGSEGV in `vibe-adsbdata` during a settings save's
     *  restart). The job runs on a DETACHED thread, and a settings save ends the server with `return 0` from main —
     *  exit() then runs the static destructors while that thread is still waiting on cv_ or working inside runOnce():
     *  it locked a destroyed mutex and read freed strings. A heap object that is never deleted outlives every thread
     *  that can touch it; the process ending reclaims it. test-adsb-data-exit. */
    static Updater& get() { static Updater* u = new Updater; return *u; }

    /** `<data dir>/adsb`. Creates it; also points the reader there. */
    void setDir(const std::string& d) {
        {
            std::lock_guard<std::mutex> lk(m_);
            dir_ = d;
        }
        ::mkdir(d.c_str(), 0755);
        Db::get().setDir(d);
    }
    void setFetcher(Fetcher f) { std::lock_guard<std::mutex> lk(m_); fetch_ = std::move(f); }
    void setUserAgent(const std::string& ua) { std::lock_guard<std::mutex> lk(m_); ua_ = ua; }
    std::string userAgent() { std::lock_guard<std::mutex> lk(m_); return ua_; }
    bool canFetch() { std::lock_guard<std::mutex> lk(m_); return (bool)fetch_ && !dir_.empty(); }

    /** The background job. Idempotent: a second call (a server restart in the same process) does nothing. */
    void start() {
        if (started_.exchange(true)) return;
        std::thread([this] { loop(); }).detach();
    }

    bool updatesOn() { State s; s.load(statePath()); return s.get("updates", "1") != "0"; }
    void setUpdatesOn(bool on) {
        std::lock_guard<std::mutex> lk(run_);
        State s; s.load(statePath());
        s.set("updates", on ? "1" : "0");
        if (on && s.num("next") == 0) s.set("next", int64_t(0));
        s.save(statePath());
        cv_.notify_all();
    }
    /** The admin page's "check now": the next wake runs at once, whatever the schedule says. */
    void checkNow() { now_ = true; cv_.notify_all(); }
    bool busy() const { return busy_.load(); }

    /** One check of both sources. `force`: even when updates are off (the admin asked). Returns a summary line. */
    std::string runOnce(bool force = false) {
        std::lock_guard<std::mutex> rl(run_);
        const std::string dir = dirCopy();
        Fetcher fetch; { std::lock_guard<std::mutex> lk(m_); fetch = fetch_; }
        if (dir.empty() || !fetch) return "no downloader on this server";
        State s; s.load(statePath());
        if (!force && s.get("updates", "1") == "0") return "updates are switched off";
        // ★ one process per machine — the others see the new files when they appear
        const int lf = ::open((dir + "/.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (lf < 0) return "cannot open the lock file";
        if (::flock(lf, LOCK_EX | LOCK_NB) != 0) { ::close(lf); return "another server process is updating"; }
        busy_ = true;
        const int64_t t = int64_t(time(nullptr));
        std::string summary;
        bool failed = false;
        for (int src = 0; src < 2; ++src) {
            const bool vrs = src == 0;
            const std::string tag = vrs ? "vrs" : "mic";
            const std::string out = dir + "/" + tag + ".vad", dl = dir + "/" + tag + ".download";
            struct stat sb{};
            const bool have = ::stat(out.c_str(), &sb) == 0;
            // ★ conditional only when we HAVE the file the validator describes; a missing file asks afresh
            const FetchResult r = fetch(vrs ? kVrsUrl : kMicUrl, dl, have ? s.get(tag + ".etag") : "", have ? s.get(tag + ".lastmod") : "");
            s.set(tag + ".checked", t);
            if (r.status == 304) { s.set(tag + ".status", "unchanged"); summary += tag + " unchanged; "; continue; }
            if (r.status != 200) {
                ::unlink(dl.c_str());
                s.set(tag + ".status", "download failed: " + (r.error.empty() ? std::string("no answer") : r.error));
                summary += tag + " download failed; "; failed = true; continue;
            }
            Mapped in;
            if (!in.open(dl)) { s.set(tag + ".status", "update failed: the download is empty or unreadable"); ::unlink(dl.c_str()); failed = true; continue; }
            std::map<std::string, std::string> meta = {{"retrieved", isoDate(t)}, {"url", vrs ? kVrsUrl : kMicUrl}};
            if (!r.etag.empty()) meta["etag"] = r.etag;
            BuildResult b = vrs ? buildVrs(in.data(), in.size(), false, meta) : buildMic(in.data(), in.size(), meta);
            in.close();
            ::unlink(dl.c_str());
            if (!b.error.empty()) {
                // ★★ a licence change is NOT a transient failure: it stays held until a person moves the pin
                s.set(tag + ".status", (b.licenceError.empty() ? "update failed: " : "held: ") + b.error);
                summary += tag + " " + b.error + "; "; failed = true;
                continue;
            }
            Vad check;
            std::string err;
            if (!check.open((const uint8_t*)b.bytes.data(), b.bytes.size(), &err) || !writeAtomic(out, b.bytes, err)) {
                s.set(tag + ".status", "could not install: " + err); failed = true; continue;
            }
            // ★ the licence travels with the data it covers (ODC-By §4.2), beside the table, in the owner's data dir
            writeAtomic(dir + (vrs ? "/LICENSE-VRS-standing-data-CC0-1.0.txt" : "/LICENSE-Mictronics-ODC-By-1.0.txt"), b.licenceText, err);
            s.set(tag + ".etag", r.etag); s.set(tag + ".lastmod", r.lastModified);
            s.set(tag + ".updated", t); s.set(tag + ".version", b.version);
            s.set(tag + ".status", "updated");
            char line[160];
            if (vrs) std::snprintf(line, sizeof line, "routes %zu, airports %zu; ", b.routes, b.airports);
            else std::snprintf(line, sizeof line, "aircraft %zu, operators %zu; ", b.aircraft, b.operators);
            summary += line;
        }
        s.set("checked", t);
        const int64_t fails = failed ? s.num("fails") + 1 : 0;
        s.set("fails", fails);
        // ★ next: a week on, at THIS server's own hour of the week; after a failure, 6 h doubling to 2 days
        s.set("next", failed ? t + std::min<int64_t>(6 * 3600LL << std::min<int64_t>(fails - 1, 3), 48 * 3600) : nextWeekly(s, t));
        s.save(statePath());
        ::flock(lf, LOCK_UN); ::close(lf);
        busy_ = false;
        Db::get().refresh(true);
        return summary;
    }

    /** The admin page's line: data dates, sources, the schedule and the switch. JSON. */
    std::string statusJson() {
        State s; s.load(statePath());
        const Db::Status d = Db::get().status();
        std::string o = "{";
        auto str = [&](const char* k, const std::string& v) { o += "\""; o += k; o += "\":\""; Info::esc(o, v); o += "\","; };
        auto num = [&](const char* k, int64_t v) { o += "\""; o += k; o += "\":" + std::to_string(v) + ","; };
        o += std::string("\"available\":") + (canFetch() ? "true" : "false") + ",";
        o += std::string("\"updates\":") + (s.get("updates", "1") != "0" ? "true" : "false") + ",";
        o += std::string("\"busy\":") + (busy() ? "true" : "false") + ",";
        num("checked", s.num("checked")); num("next", s.num("next"));
        num("vrsBuilt", d.vrsBuilt); num("micBuilt", d.micBuilt); num("starterBuilt", d.starterBuilt);
        o += std::string("\"vrsFile\":") + (d.vrsFile ? "true" : "false") + ",";
        o += std::string("\"micFile\":") + (d.micFile ? "true" : "false") + ",";
        num("aircraft", int64_t(d.aircraft)); num("routes", int64_t(d.routes)); num("airports", int64_t(d.airports));
        num("operators", int64_t(d.operators)); num("types", int64_t(d.types));
        str("vrsStatus", s.get("vrs.status")); str("micStatus", s.get("mic.status"));
        str("vrsVersion", s.get("vrs.version")); str("micVersion", s.get("mic.version"));
        num("logos", int64_t(Db::images().count(0))); num("silhouettes", int64_t(Db::images().count(1)));
        o += "\"credits\":[" + creditsJson() + "]}";
        return o;
    }

    /** Every source, its licence and where it lives — shown wherever the data is (research §3.3: ODC-By §4.3). */
    static std::string creditsJson() {
        return "{\"name\":\"Virtual Radar Server standing data\",\"by\":\"Andrew Whewell and the VRS users who submit it\","
               "\"licence\":\"CC0 1.0\",\"url\":\"https://github.com/vradarserver/standing-data\","
               "\"what\":\"routes, airports, airlines, aircraft types, address blocks, countries\"},"
               "{\"name\":\"Mictronics aircraft database\",\"by\":\"Mictronics\",\"licence\":\"ODC-By 1.0\","
               "\"url\":\"https://github.com/Mictronics/aircraft-database\","
               "\"licenceUrl\":\"https://opendatacommons.org/licenses/by/1-0/\","
               "\"notice\":\"Contains information from Mictronics aircraft database, which is made available under the ODC Attribution License.\","
               "\"what\":\"registrations, types, operators\"},"
               "{\"name\":\"ICAO Annex 10 Vol. III address allocations\",\"by\":\"VibeSDR (our own table)\",\"licence\":\"facts\","
               "\"what\":\"country of registry\"}";
    }

private:
    std::string dirCopy() { std::lock_guard<std::mutex> lk(m_); return dir_; }
    std::string statePath() { return dirCopy() + "/state.txt"; }

    static std::string isoDate(int64_t t) {
        char b[16]; const time_t tt = time_t(t); struct tm tm{}; gmtime_r(&tt, &tm);
        std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
        return b;
    }
    /** This server's slot in the week: chosen at random once and kept, so the fleet spreads across 168 hours. */
    static int64_t nextWeekly(State& s, int64_t now) {
        int64_t slot = s.num("slot", -1);
        if (slot < 0 || slot >= 7 * 86400) {
            std::random_device rd;
            slot = int64_t(std::uniform_int_distribution<int64_t>(0, 7 * 86400 - 1)(rd));
            s.set("slot", slot);
        }
        const int64_t week = 7 * 86400;
        int64_t next = now - (now % week) + slot;
        while (next <= now + 86400) next += week;          // ★ at least a day away, at most eight
        return next;
    }

    void loop() {
        vibeBulkThread("vibe-adsbdata");
        // ★ not during start-up: the radio comes up first. A random 1.5–4 minutes, so a fleet restarted by an
        //   apt upgrade does not arrive at GitHub in the same second.
        std::mt19937 rng{std::random_device{}()};
        wait(std::chrono::seconds(90 + std::uniform_int_distribution<int>(0, 150)(rng)));
        for (;;) {
            if (canFetch()) {
                State s; s.load(statePath());
                const int64_t t = int64_t(time(nullptr));
                const bool on = s.get("updates", "1") != "0";
                const bool forced = now_.exchange(false);
                if (forced || (on && t >= s.num("next"))) {
                    const std::string r = runOnce(forced);
                    std::fprintf(stderr, "VibeServer: aircraft data check: %s\n", r.c_str());
                }
            }
            // ★ hourly wake, weekly work: the decision comes from the state file's `next`, never from uptime (the
            //   EiBi lesson — a server restarted every few days would otherwise never refresh)
            wait(std::chrono::hours(1));
        }
    }
    template <class D> void wait(D d) {
        std::unique_lock<std::mutex> lk(cvm_);
        cv_.wait_for(lk, d, [this] { return now_.load(); });
    }

    std::mutex m_, run_, cvm_;
    std::condition_variable cv_;
    std::string dir_, ua_ = "VibeServer (+https://vibesdr.net)";
    Fetcher fetch_;
    std::atomic<bool> started_{false}, now_{false}, busy_{false};
};

} // namespace vibeadsbdata
