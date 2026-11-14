// hw_state.h — what MENU → R36S HARDWARE (and DISPLAY's backlight) learns from the supervisor's /handheld/* endpoints,
// fetched on WORKER threads (http.h: never from the draw loop) and handed to the UI thread under one lock.
// ★ Workers never touch App: they fill HwShared; the UI thread (hwPump / the sections) copies what it needs into App
//   and turns queued messages into toasts.
#pragma once
#include "http.h"
#include "json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hh {

struct HwShared {
    std::mutex m;
    // GET /handheld/device
    bool devKnown = false, hwdOk = true;
    int brightness = -1, volume = -1, batt = -1; bool charging = false;
    // GET /handheld/perf
    bool perfKnown = false;
    double cpu = -1, tempC = -1, engineCpu = -1; int ramUsed = -1, ramTotal = -1, minutesLeft = -1;
    // GET /handheld/card
    std::string card = "—";
    // GET /handheld/location
    bool locKnown = false, locSet = false; std::string locator, lat, lon;
    // in flight (a button shows WORKING…)
    bool diagBusy = false, importBusy = false, exportBusy = false;
    // toasts for the UI thread
    std::deque<std::string> toasts;
};
inline HwShared& hwShared() { static HwShared s; return s; }
inline void hwToast(const std::string& t) { HwShared& h = hwShared(); std::lock_guard<std::mutex> lk(h.m); h.toasts.push_back(t); }
inline double hwNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/** Run `fn` on a worker thread (the engine's port captured). */
template <class F> inline void hwAsync(F fn) { std::thread(std::move(fn)).detach(); }

inline void hwParseLocation(const Json& j) {
    HwShared& h = hwShared();
    std::lock_guard<std::mutex> lk(h.m);
    h.locKnown = true; h.locSet = j["set"].boolean();
    h.locator = j["locator"].str();
    // lat / lon arrive as strings (the supervisor's own formatting) — accept numbers too
    auto s = [](const Json& v) { if (v.kind == Json::Str) return v.s; if (v.kind == Json::Num) { char b[32]; std::snprintf(b, sizeof b, "%g", v.n); return std::string(b); } return std::string(); };
    h.lat = s(j["lat"]); h.lon = s(j["lon"]);
}

/** refreshDeviceRows (handheld.ts): /handheld/device, /card, /perf, /location — every time a section opens. */
inline void hwRefresh(int port) {
    hwAsync([port] {
        const HttpResult d = httpRequest(port, "GET", "/handheld/device", "", 4);
        {
            HwShared& h = hwShared();
            std::lock_guard<std::mutex> lk(h.m);
            if (d.status == 200) {
                const Json st = Json::parse(d.body)["state"];
                h.devKnown = true;
                h.hwdOk = !(st.has("ok") && !st["ok"].boolean(true));
                if (st["brightness"].kind == Json::Num) h.brightness = (int)st["brightness"].n;
                if (st["volume"].kind == Json::Num) h.volume = (int)st["volume"].n;
                if (st["battery"]["pct"].kind == Json::Num) { h.batt = (int)st["battery"]["pct"].n; h.charging = st["battery"]["charging"].boolean(); }
                else h.batt = -1;
            }
        }
        const HttpResult c = httpRequest(port, "GET", "/handheld/card", "", 4);
        {
            HwShared& h = hwShared();
            std::lock_guard<std::mutex> lk(h.m);
            if (c.status == 200) {
                const Json j = Json::parse(c.body);
                if (j["present"].boolean()) {
                    char b[80];
                    std::snprintf(b, sizeof b, "%.1f GB free of %.1f GB", j["freeBytes"].num() / 1073741824.0, j["totalBytes"].num() / 1073741824.0);
                    h.card = b;
                } else h.card = "no card";
            } else h.card = "—";
        }
        const HttpResult p = httpRequest(port, "GET", "/handheld/perf", "", 4);
        if (p.status == 200) {
            const Json j = Json::parse(p.body);
            HwShared& h = hwShared();
            std::lock_guard<std::mutex> lk(h.m);
            h.perfKnown = true;
            h.cpu = j["cpu"].num(-1); h.tempC = j["tempC"].num(-1); h.engineCpu = j["engineCpu"].num(-1);
            h.ramUsed = (int)j["ramUsedMb"].num(-1); h.ramTotal = (int)j["ramTotalMb"].num(-1);
            h.minutesLeft = (int)j["battery"]["minutesLeft"].num(-1);
        }
        const HttpResult l = httpRequest(port, "GET", "/handheld/location", "", 4);
        if (l.status == 200) hwParseLocation(Json::parse(l.body));
    });
}

/** /handheld/perf alone (the status row's battery time-left, once a minute). */
inline void hwRefreshPerf(int port) {
    hwAsync([port] {
        const HttpResult p = httpRequest(port, "GET", "/handheld/perf", "", 3);
        if (p.status != 200) return;
        const Json j = Json::parse(p.body);
        HwShared& h = hwShared();
        std::lock_guard<std::mutex> lk(h.m);
        h.perfKnown = true;
        h.cpu = j["cpu"].num(-1); h.tempC = j["tempC"].num(-1); h.engineCpu = j["engineCpu"].num(-1);
        h.ramUsed = (int)j["ramUsedMb"].num(-1); h.ramTotal = (int)j["ramTotalMb"].num(-1);
        h.minutesLeft = (int)j["battery"]["minutesLeft"].num(-1);
        if (j["battery"]["pct"].kind == Json::Num) { h.batt = (int)j["battery"]["pct"].n; h.charging = j["battery"]["charging"].boolean(); }
    });
}

/** "2026-11-06 21:11:18.0" from UTC milliseconds. */
inline std::string hwUtcText(long long ms, bool tenths = true) {
    const std::time_t tt = (std::time_t)(ms / 1000);
    std::tm u{}; gmtime_r(&tt, &u);
    char b[48];
    if (tenths) std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d.%d", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, (int)((ms % 1000) / 100));
    else std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
    return b;
}
inline long long hwUtcMsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/* ★★ THE LAST GOOD SYNC (Stuart, 2026-11-08: "all it needs to be is one line showing the latest successful sync" —
 *  "System Clock set by FT8 @ 18:17:20 08/Nov/26"). Every set the engine confirms is kept: who set it (FT8 · RDS · DAB ·
 *  a time station · by hand) and the clock as it read just after. Kept in ~/.vibe-hhui-clock.json so it survives a
 *  power-off (a reflash starts it blank). */
struct HwClockLast { std::mutex m; std::string by; long long utcMs = 0; bool loaded = false; };
inline HwClockLast& hwClockLast() { static HwClockLast c; return c; }
inline std::string hwClockLastPath() { const char* home = std::getenv("HOME"); return std::string(home && *home ? home : "/tmp") + "/.vibe-hhui-clock.json"; }
/** "FT8" / "RDS" / "DAB" / "MSF" / "hand" from the source the set carried. */
inline std::string hwClockBy(std::string src) {
    if (src.rfind("auto · ", 0) == 0) src = src.substr(8);   // ("auto · " is 8 bytes: "·" is two)
    if (src.find("FT8") != std::string::npos) return "FT8";
    if (src.rfind("RDS", 0) == 0) return "RDS";
    if (src.rfind("DAB", 0) == 0) return "DAB";
    if (src.empty() || src == "by hand" || src.rfind("nudge", 0) == 0) return "hand";
    return src;   // a time station: MSF, DCF77, WWVB …
}
/** The clock at boot was only a GUESS (vibe-clock wrote "guess": the RTC lost the time — a flat or pulled battery). */
inline bool hwClockBootGuess() {
    static int g = -1;
    if (g < 0) {
        g = 0;
        if (FILE* f = std::fopen("/run/vibe-clock.boot", "r")) { char b[16] = {0}; if (std::fgets(b, sizeof b, f)) g = std::string(b).rfind("guess", 0) == 0; std::fclose(f); }
    }
    return g == 1;
}
/** When this boot began, on the clock as it reads now (a set since then moved both together). */
inline long long hwBootUtcMs() {
    double up = 0;
    if (FILE* f = std::fopen("/proc/uptime", "r")) { if (std::fscanf(f, "%lf", &up) != 1) up = 0; std::fclose(f); }
    return hwUtcMsNow() - (long long)(up * 1000);
}
/** "System Clock set by FT8 @ 18:17:20 08/Nov/26" (UTC), or "" before the first. warn: the clock is NOT known — the
 *  RTC lost the time at boot and nothing has set it since — and the line asks for a source instead. */
inline std::string hwClockLastLine(bool& warn) {
    warn = false;
    HwClockLast& c = hwClockLast();
    std::lock_guard<std::mutex> lk(c.m);
    if (!c.loaded) {
        c.loaded = true;
        if (FILE* f = std::fopen(hwClockLastPath().c_str(), "r")) {
            char buf[512]; const size_t n = std::fread(buf, 1, sizeof buf - 1, f); std::fclose(f); buf[n] = 0;
            const Json j = Json::parse(buf);
            c.by = j["by"].str(); c.utcMs = (long long)j["utcMs"].num(0);
        }
    }
    // ★ a guessed boot clock stays unset until a set made THIS boot (on the corrected clock, after the boot began)
    if (hwClockBootGuess() && !(c.utcMs > 0 && c.utcMs >= hwBootUtcMs())) {
        warn = true;
        return "System clock not set — it lost the time while the battery was flat or out. Tune to a time signal (MSF, DCF77, WWVB), FT8, "
               "an FM station with RDS or a DAB multiplex to set it";
    }
    if (c.by.empty() || c.utcMs <= 0) return "";
    static const char* mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t tt = (std::time_t)(c.utcMs / 1000);
    std::tm u{}; gmtime_r(&tt, &u);
    char b[96];
    std::snprintf(b, sizeof b, "System Clock set by %s @ %02d:%02d:%02d %02d/%s/%02d", c.by.c_str(), u.tm_hour, u.tm_min, u.tm_sec, u.tm_mday, mon[u.tm_mon], u.tm_year % 100);
    return b;
}
inline void hwClockNoteSet(const std::string& by, long long utcMs) {
    HwClockLast& c = hwClockLast();
    std::lock_guard<std::mutex> lk(c.m);
    c.loaded = true; c.by = by; c.utcMs = utcMs;
    const std::string p = hwClockLastPath(), tmp = p + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
        std::fprintf(f, "{\"by\":%s,\"utcMs\":%lld}\n", jsonQuote(by).c_str(), utcMs);
        std::fclose(f);
        std::rename(tmp.c_str(), p.c_str());
    }
}

/** setClock (handheld.ts): POST /handheld/clock, the answer toasted — and a confirmed set kept (hwClockLastLine). */
inline void hwSetClock(int port, const std::string& body, const std::string& what) {
    hwAsync([port, body, what] {
        const HttpResult r = httpRequest(port, "POST", "/handheld/clock", body, 5);
        const Json j = Json::parse(r.body);
        if (r.status == 0) hwToast("The clock was not set — the engine did not answer");
        else if (j["ok"].boolean()) {
            hwToast("Clock set " + what + " · " + hwUtcText((long long)j["utcMs"].num()) + " UTC");
            hwClockNoteSet(hwClockBy(Json::parse(body)["source"].str()), j["utcMs"].kind == Json::Num ? (long long)j["utcMs"].n : hwUtcMsNow());
        }
        else hwToast("The clock was not set" + (j["error"].str().empty() ? std::string() : " — " + j["error"].str()));
    });
}

/** postLocation (handheld.ts): the ONE way a location is set — typed or cleared. */
inline void hwPostLocation(int port, const std::string& body) {
    hwAsync([port, body] {
        const HttpResult r = httpRequest(port, "POST", "/handheld/location", body, 5);
        const Json j = Json::parse(r.body);
        if (r.status == 0) { hwToast("Location not set — the engine did not answer"); return; }
        if (r.status != 200) { hwToast("Location not set" + (j["error"].str().empty() ? std::string() : " — " + j["error"].str())); return; }
        hwParseLocation(j);
        if (j["set"].boolean()) hwToast("Location set · " + (j["locator"].str().empty() ? j["lat"].str() + ", " + j["lon"].str() : j["locator"].str()));
        else hwToast("Location cleared");
    });
}

// ── the clock keys' evidence (fed by whoever reads the decoder socket — none yet in the native UI) ─────────────────────
struct HwTimeSync { std::string station; double utcMs = 0, rxDelayMs = 0, hostMs = 0; double at = 0; };
inline HwTimeSync& hwLastSync() { static HwTimeSync s; return s; }
inline std::deque<std::pair<double, double>>& hwFt8Dts() { static std::deque<std::pair<double, double>> d; return d; }   // (dt, at)
/** A corroborated time-station minute (main.ts onTimeSync → handheld.ts noteTimeSync). UI thread. */
inline void hwNoteTimeSync(const std::string& station, double utcMs, double rxDelayMs, double hostMs) {
    hwLastSync() = {station, utcMs, rxDelayMs, hostMs, hwNow()};
}
/** ★★ FT8 CLOCK AUTO-SET's evidence (Stuart, 2026-11-08) — the DTs grouped by the 15 s cycle they were decoded in
 *  (their arrival's cycle: every decode of one slot lands in the same one). See clockAutoFt8 (clock_rds.h). */
struct HwFt8Clock {
    struct Cycle { long long slot = 0; std::vector<double> dts; };
    std::deque<Cycle> cycles;
    long long evalSlot = -1;          // the newest cycle already judged
    double holdUntil = 0;             // hwNow(): DTs before this were measured against the clock before a set — ignored
    double syncedAt = -1e18;          // hwNow(): the last cycle that found the clock right (or set it)
    double setAt = -1e18, setMs = 0;  // the last auto-set and how far it moved the clock
    int agreeing = 0;                 // how many of the last 3 complete cycles agree (the box's "cycle N of 3")
};
inline HwFt8Clock& hwFt8Clock() { static HwFt8Clock k; return k; }
/** An FT8 spot's DT (handheld.ts noteSpot). UI thread. */
inline void hwNoteFt8Dt(double dt) {
    auto& d = hwFt8Dts();
    d.push_back({dt, hwNow()});
    while (d.size() > 200) d.pop_front();
    HwFt8Clock& k = hwFt8Clock();
    if (hwNow() < k.holdUntil || std::fabs(dt) >= 7.5) return;   // ★ half a slot: the decoder's clock hunt reports DTs out to ±7 s (2026-11-14)
    const long long slot = hwUtcMsNow() / 15000;
    if (k.cycles.empty() || k.cycles.back().slot != slot) k.cycles.push_back({slot, {}});
    k.cycles.back().dts.push_back(dt);
    while (k.cycles.size() > 8) k.cycles.pop_front();
}
/** After any clock set: the DTs so far were measured against the old clock. */
inline void hwFt8ClockForget(double holdSec) {
    HwFt8Clock& k = hwFt8Clock();
    k.cycles.clear(); k.agreeing = 0; k.holdUntil = hwNow() + holdSec;
    hwFt8Dts().clear();
}
/** ft8Median: ≥ 3 decodes in 2 min with |dt| < 7.5 s (half a slot — the clock hunt finds errors that large). */
inline bool hwFt8Median(double& dt, int& n) {
    std::vector<double> v;
    const double now = hwNow();
    for (auto& x : hwFt8Dts()) if (now - x.second < 120 && std::fabs(x.first) < 7.5) v.push_back(x.first);
    if (v.size() < 3) return false;
    std::sort(v.begin(), v.end());
    const size_t k = v.size();
    dt = k % 2 ? v[(k - 1) / 2] : (v[k / 2 - 1] + v[k / 2]) / 2;
    n = (int)k;
    return true;
}

}  // namespace hh
