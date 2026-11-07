// bm_state.h — what the BOOKMARKS and SEARCH pages learn from the engine, fetched on WORKER threads (http.h: never from
// the draw loop) and handed to the UI thread under one lock — the same split as hw_state.h.
//
//   GET /bookmarks                      the RECEIVER's list (search.ts loadServerBookmarks): saved by the owner
//                                       (manual:true) and heard over RDS / on a DAB multiplex (manual:false)
//   POST /bookmarks?frequency=&name=&mode=&bandwidth_low=&bandwidth_high=     + ADD HERE (search.ts saveToServer)
//   POST /bookmarks?was=<Hz>[&was_sid=]&frequency=&name=&mode=[&bandwidth_…]  EDIT (bookmarkEdit.ts serverEditQuery)
//   DELETE /bookmarks?frequency=<Hz>[&sid=]                                    ✕ (search.ts removeFromServer)
//   GET /stations                       the EiBi schedule the engine holds (search.ts loadStations)
//   GET /vibeserver/eibi                how many EiBi entries, and when the file was written
//   POST /handheld/eibi/import          IMPORT EIBI: the card's EiBi/sked-*.csv becomes the engine's schedule
//
// ★ Every write answers with the whole list, which replaces ours (as search.ts does with the array it gets back).
// ★★ Names from the server are UNTRUSTED (search.ts safeList): a real, finite frequency or the row is dropped; control
//    characters are removed and the name capped. Nothing downstream sees raw server text.
#pragma once
#include "hw_state.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace hh {

struct BmRow {
    std::string name, mode, lname;   // lname: the name in lower case (SEARCH matches case-blind)
    double frequency = 0;
    bool manual = false;          // saved by the owner (else heard by this receiver)
    bool hasBw = false; int bwLo = 0, bwHi = 0;
    int sid = -1;                 // a DAB service (mode "dab") — the native UI has no DAB, so these are not offered
};

/** One EiBi entry (GET /stations), kept compact: ~9000 of them. */
struct StationRow {
    std::string name, comment, itu, mode, lname, lcomment;   // l…: lower case, made once (SEARCH runs per key press)
    double frequency = 0;
};

struct BmShared {
    std::mutex m;
    // the receiver's list
    std::vector<BmRow> rows;
    bool known = false, loading = false;
    double fetchedAt = -1e9;
    uint64_t seq = 0;             // bumps on every new list (the pages rebuild their views)
    // the EiBi schedule
    std::vector<StationRow> stations;
    bool stationsLoading = false;
    double stationsAt = -1e9;
    uint64_t stationsSeq = 0;
    int eibiEntries = -1;         // GET /vibeserver/eibi
    std::string eibiUpdated;
    // in flight
    bool writeBusy = false, eibiBusy = false;
};
inline BmShared& bmShared() { static BmShared s; return s; }

// ── text ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** src/utils/safeText cleanText, the parts that matter here: no control characters, trimmed, capped (UTF-8 safe). */
inline std::string bmClean(const std::string& in, size_t cap = 64) {
    std::string o;
    for (unsigned char ch : in) { if (ch < 0x20 || ch == 0x7f) continue; o += (char)ch; }
    size_t a = 0, b = o.size();
    while (a < b && o[a] == ' ') a++;
    while (b > a && o[b - 1] == ' ') b--;
    o = o.substr(a, b - a);
    if (o.size() > cap) { size_t n = cap; while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80) n--; o.resize(n); }
    return o;
}
inline std::string bmLowerMode(const std::string& m) {
    std::string o;
    for (char ch : m) if (std::isalnum((unsigned char)ch) && o.size() < 16) o += (char)std::tolower((unsigned char)ch);
    return o;
}
/** ASCII lower case (search.ts toLowerCase; accented letters are left as they are on both sides). */
inline std::string bmLower(const std::string& s) { std::string o = s; for (auto& ch : o) ch = (char)std::tolower((unsigned char)ch); return o; }
/** encodeURIComponent. */
inline std::string bmUrlEnc(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '!' || ch == '~' || ch == '*' || ch == '\'' || ch == '(' || ch == ')') o += (char)ch;
        else { o += '%'; o += hx[ch >> 4]; o += hx[ch & 15]; }
    }
    return o;
}
inline std::string bmMhz(double hz) { char b[32]; std::snprintf(b, sizeof b, "%.3f", hz / 1e6); return b; }

/** The receiver's answer (an array) → rows. False when it was not a list. */
inline bool bmParseList(const std::string& body, std::vector<BmRow>& out) {
    const Json j = Json::parse(body);
    if (j.kind != Json::Arr) return false;
    out.clear();
    out.reserve(j.a.size());
    for (auto& b : j.a) {
        if (b.kind != Json::Obj) continue;
        const double f = b["frequency"].num(0);
        if (!(f > 0) || !std::isfinite(f)) continue;
        BmRow r;
        r.frequency = f;
        r.name = bmClean(b["name"].str());
        if (r.name.empty()) continue;
        r.lname = bmLower(r.name);
        r.mode = bmLowerMode(b["mode"].str());
        if (r.mode.empty()) r.mode = "wfm";                 // search.ts: `b.mode ?? 'wfm'`
        r.manual = b["manual"].boolean();
        if (b["bandwidth_low"].kind == Json::Num && b["bandwidth_high"].kind == Json::Num) {
            r.hasBw = true; r.bwLo = (int)b["bandwidth_low"].n; r.bwHi = (int)b["bandwidth_high"].n;
            if (r.bwHi <= r.bwLo) r.hasBw = false;
        }
        r.sid = b["sid"].kind == Json::Num ? (int)b["sid"].n : -1;
        out.push_back(std::move(r));
    }
    return true;
}
inline void bmTakeList(std::vector<BmRow>&& rows) {
    BmShared& b = bmShared();
    std::lock_guard<std::mutex> lk(b.m);
    b.rows = std::move(rows); b.known = true; b.fetchedAt = hwNow(); b.seq++;
}

// ── workers ─────────────────────────────────────────────────────────────────────────────────────────────────────────
/** refreshServerBookmarks (main.ts): at most every 10 s unless `force`. */
inline void bmRefresh(int port, bool force = false) {
    {
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        if (b.loading || (!force && hwNow() - b.fetchedAt < 10)) return;
        b.loading = true;
    }
    hwAsync([port] {
        const HttpResult r = httpRequest(port, "GET", "/bookmarks", "", 10);
        std::vector<BmRow> rows;
        const bool ok = r.status == 200 && bmParseList(r.body, rows);
        if (ok) bmTakeList(std::move(rows));
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        b.loading = false;
        if (!ok) b.fetchedAt = hwNow() - 7;                  // try again in ~3 s
    });
}

/** A write (add / edit / delete): the answer is the whole list. `okText` toasted on success. */
inline void bmWrite(int port, const std::string& method, const std::string& query, const std::string& okText) {
    {
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        if (b.writeBusy) { hwToast("Still saving the last change…"); return; }
        b.writeBusy = true;
    }
    hwAsync([port, method, query, okText] {
        const HttpResult r = httpRequest(port, method, "/bookmarks?" + query, "", 10);
        std::vector<BmRow> rows;
        if (r.status == 200 && bmParseList(r.body, rows)) { bmTakeList(std::move(rows)); hwToast(okText); }
        else if (r.status == 0) hwToast("Not saved — the receiver did not answer");
        else {
            // bookmarkEdit.ts serverEditError: the server's own words when it sent some
            const Json j = Json::parse(r.body);
            const std::string e = j["error"].str();
            hwToast(e.empty() ? "Not saved — the receiver refused it (" + std::to_string(r.status) + ")" : "Not saved — " + e.substr(0, 200));
        }
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        b.writeBusy = false;
    });
}
inline std::string bmBwQuery(bool has, int lo, int hi) {
    if (!has || hi <= lo) return "";
    return "&bandwidth_low=" + std::to_string(lo) + "&bandwidth_high=" + std::to_string(hi);
}

/** The EiBi schedule (GET /stations) and its status (GET /vibeserver/eibi): when the search page opens, at most every
 *  10 minutes unless `force` (after IMPORT EIBI). The JSON (~1.1 MB) is parsed here and only the rows kept. */
inline void bmLoadStations(int port, bool force = false) {
    {
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        if (b.stationsLoading || (!force && hwNow() - b.stationsAt < 600)) return;
        b.stationsLoading = true;
    }
    hwAsync([port] {
        int entries = -1; std::string updated;
        const HttpResult e = httpRequest(port, "GET", "/vibeserver/eibi", "", 10);
        if (e.status == 200) { const Json j = Json::parse(e.body); entries = (int)j["entries"].num(-1); updated = j["updated"].str(); }
        std::vector<StationRow> st;
        bool ok = false;
        {
            HttpResult r = httpRequest(port, "GET", "/stations", "", 15);
            if (r.status == 200) {
                const Json j = Json::parse(r.body);
                r.body.clear(); r.body.shrink_to_fit();
                if (j.kind == Json::Arr) {
                    ok = true;
                    st.reserve(j.a.size());
                    for (auto& s : j.a) {
                        const double f = s["frequency"].num(0);
                        if (!(f > 0) || !std::isfinite(f)) continue;
                        StationRow o;
                        o.name = bmClean(s["name"].str());
                        if (o.name.empty()) continue;
                        o.frequency = f;
                        o.mode = bmLowerMode(s["mode"].str());
                        o.comment = bmClean(s["comment"].str(), 80);
                        if (o.comment.empty()) o.comment = bmClean(s["group"].str(), 80);
                        o.itu = bmClean(s["itu"].str(), 8);
                        o.lname = bmLower(o.name); o.lcomment = bmLower(o.comment);
                        st.push_back(std::move(o));
                    }
                }
            }
        }
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        b.stationsLoading = false;
        if (entries >= 0) { b.eibiEntries = entries; b.eibiUpdated = updated; }
        if (ok) { b.stations = std::move(st); b.stationsAt = hwNow(); b.stationsSeq++; }
        else b.stationsAt = hwNow() - 570;                   // try again in ~30 s
    });
}

/** IMPORT EIBI: the supervisor installs the card's newest EiBi/sked-*.csv, then we re-read the list SEARCH uses. */
inline void bmImportEibi(int port) {
    {
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        if (b.eibiBusy) return;
        b.eibiBusy = true;
    }
    hwAsync([port] {
        const HttpResult r = httpRequest(port, "POST", "/handheld/eibi/import", "", 60);
        const Json j = Json::parse(r.body);
        if (r.status == 0) hwToast("EiBi not imported — the engine did not answer");
        else if (r.status == 404 && (j.isNull() || j["error"].str() == "no such handheld route")) hwToast("EiBi not imported — this engine is too old for IMPORT EIBI");
        else if (!j["ok"].boolean()) hwToast("EiBi not imported — " + (j["error"].str().empty() ? "the engine refused it (" + std::to_string(r.status) + ")" : j["error"].str()));
        else {
            const int n = (int)j["entries"].num(0);
            const std::string season = j["season"].str();
            hwToast("EiBi imported · " + std::to_string(n) + " stations" + (season.empty() ? "" : " · " + season) + " — SEARCH finds them now");
        }
        { BmShared& b = bmShared(); std::lock_guard<std::mutex> lk(b.m); b.eibiBusy = false; }
        if (j["ok"].boolean()) bmLoadStations(port, true);
    });
}

}  // namespace hh
