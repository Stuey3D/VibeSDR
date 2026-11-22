// vibe_aircraft_info.h — V12 beta91 WHAT EVERY AIRCRAFT ON THE AVIATION MAP IS, whichever source heard it: the ADS-B aircraft
// data (vibe_adsb_data.h — registration, type, operator, country, military, route, the owner's logo / silhouette keys)
// looked up for the shared table's rows (vibe_aircraft_share.h), so a VDL2-, ACARS-, HFDL- or ADS-C-only aircraft gets the
// same type silhouette, operator badge, logo and flag an ADS-B one does. docs/v12/AVIATION.md §Aircraft data for every source.
//
// ★★★ Stuart (2026-11-22): "our adsb logos and silhouettes could apply to vdl2 acars hfdl cant it, they are the same
//     aircraft after all". Before this only the ADS-B decoder looked aircraft up (vibe_adsb_service.h enrich), so a row
//     ADS-B never heard showed just its address.
//
// ★★ THE SAME TABLES, THE SAME LOOKUPS: Db::lookupHex (by the ICAO address), Db::lookupCallsign (the operator from the
//    flight's airline prefix — an ACARS "BA0117" is IATA BA → BAW, the same normalisation the ADS-B path uses — and the
//    VRS overlay's operator by address), Db::judge (the leg, where the row has a position), Db::resolveImages. Nothing is
//    duplicated: the tables stay memory-mapped in vibe_adsb_data.h.
// ★★★ NEVER ON THE NETWORK, DSP OR AUDIO THREAD (Network > Audio > Spectrum > Decoders). A first lookup can fault a page
//     in from an SD card. GET /vibeserver/aircraft.json (a network thread) only READS this cache — a map lookup under a
//     mutex — and queues the rows it has no answer for; ONE worker thread at decoder priority looks them up. A row's
//     first poll goes without (the map polls every 3 s; the next one has it).
// ★★ ONCE PER AIRCRAFT, AGAIN WHEN IT CHANGES: the answer is kept per row key with the flight and registration it was
//    made from; a new flight or registration (or a first position, for a route's leg) queues it again; an answer an hour
//    old is redone (the weekly data update may have replaced the tables).
// ★★ BOUNDED (the 256 MB board, the 1 GB 32-bit phone): at most kMax answers (the shared table's own kCap) — the least
//    recently asked go first — and at most kMax rows waiting. Each answer is a few hundred bytes: < 1 MB when full.
#pragma once
#include "vibe_adsb_data.h"
#include "vibe_aircraft_share.h"
#include "vibe_thread.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vibeavinfo {

static constexpr size_t kMax = vibeavshare::kCap;
static constexpr int64_t kRedoMs = 3600 * 1000;

/** The row's identity for the cache: the shared table's own key (the address, else r:REG, else f:FLIGHT). */
inline std::string rowKey(const vibeavshare::Row& w) {
    char b[16];
    if (w.icao) { std::snprintf(b, sizeof b, "%06X", w.icao); return b; }
    if (!w.reg.empty()) return "r:" + vibeavshare::regKey(w.reg);
    return "f:" + vibeavshare::flightKey(w.flight);
}

/** ★ One lookup — what the ADS-B decoder's enrich() does for an aircraft, for a shared-table row. Public for the tests. */
inline vibeavshare::Extra lookup(uint32_t icao, const std::string& flight, const std::string& reg, bool hasPos, double lat, double lon) {
    vibeadsbdata::Db& db = vibeadsbdata::Db::get();
    vibeadsbdata::Info i;
    if (icao) db.lookupHex(icao, i);
    // ★ the registration the row heard (ACARS) keeps a registration-as-flight ("GABCD") from reading as an airline,
    //   exactly as the ADS-B path passes the database's
    const std::string regForCs = i.r.empty() ? reg : i.r;
    if (icao || !flight.empty()) db.lookupCallsign(flight, regForCs, icao, i);
    if (hasPos && i.route.size() >= 2) vibeadsbdata::Db::judge(i, lat, lon);
    char hu[8] = "";
    if (icao) std::snprintf(hu, sizeof hu, "%06X", icao);
    if (i.r.empty() && !reg.empty()) i.r = reg;   // ★ an owner's logo named by the registration ACARS heard
    vibeadsbdata::Db::resolveImages(hu, i);
    vibeavshare::Extra ex;
    ex.reg = i.r; ex.type = i.t;
    // ★★ the ADS-B aircraft data's OWN JSON (Info::json — the same names the ADS-B layer reads), split: the members
    //    without r / t (the row's own reg / type win), and the route object on its own (the row's airports win)
    vibeadsbdata::Info f = i;
    f.r.clear(); f.t.clear(); f.route.clear();
    const std::string j = f.json();
    if (j.size() > 2) ex.fields = "," + j.substr(1, j.size() - 2);
    if (i.route.size() >= 2) {
        vibeadsbdata::Info r;
        r.route = i.route; r.leg = i.leg; r.plausible = i.plausible;
        const std::string rj = r.json();                 // {"route":{…}}
        const size_t at = rj.find(':');
        if (at != std::string::npos && rj.size() > at + 2) ex.route = rj.substr(at + 1, rj.size() - at - 2);
    }
    return ex;
}

class Cache {
public:
    /** ★ Never destroyed (as vibeadsbdata::Db::get): the worker may still be looking up while exit() runs. */
    static Cache& get() { static Cache* c = new Cache; return *c; }

    /** ★ THE NETWORK THREAD's call (aircraft.json, per row): what is known — at once, never a lookup. A row with no
     *  answer yet, or one whose flight / registration changed, is queued for the worker; the old answer stands meanwhile. */
    void extra(const vibeavshare::Row& w, vibeavshare::Extra& out) {
        if (!w.icao && w.flight.empty()) return;          // ★ a registration alone: the data has no index by it
        const std::string key = rowKey(w);
        const bool pos = (w.flags & 1) != 0;
        std::lock_guard<std::mutex> lk(m_);
        const int64_t now = vibeavshare::wallMs();
        auto it = map_.find(key);
        bool want = it == map_.end();
        if (!want) {
            E& e = it->second;
            e.used = now;
            out = e.ex;
            want = e.flight != w.flight || e.reg != w.reg || now - e.made > kRedoMs || (pos && !e.pos && !e.ex.route.empty());
        }
        if (want && !queued_.count(key) && q_.size() < kMax) {
            q_.push_back({ key, w.icao, w.flight, w.reg, pos, w.lat, w.lon });
            queued_.insert(key);
            ++queuedTotal_;
            if (async_) { start_(); cv_.notify_one(); }
        }
    }
    /** Look up everything queued, on the CALLER's thread (the tests; the worker's loop body). */
    size_t runPending() {
        size_t n = 0;
        for (;;) {
            Want x;
            { std::lock_guard<std::mutex> lk(m_); if (q_.empty()) break; x = std::move(q_.front()); q_.pop_front(); }
            vibeavshare::Extra ex = lookup(x.icao, x.flight, x.reg, x.pos, x.lat, x.lon);   // ★ the lock released
            std::lock_guard<std::mutex> lk(m_);
            queued_.erase(x.key);
            E& e = map_[x.key];
            e.ex = std::move(ex); e.flight = x.flight; e.reg = x.reg; e.pos = x.pos; e.made = e.used = vibeavshare::wallMs();
            ++lookups_; ++n;
            if (map_.size() > kMax) evictLocked_();
        }
        return n;
    }
    /** Tests: false = nothing runs until runPending() (no thread). */
    void setAsync(bool on) { std::lock_guard<std::mutex> lk(m_); async_ = on; }
    void clear() { std::lock_guard<std::mutex> lk(m_); map_.clear(); q_.clear(); queued_.clear(); }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return map_.size(); }
    size_t waiting() { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    uint64_t lookups() { std::lock_guard<std::mutex> lk(m_); return lookups_; }

private:
    struct Want { std::string key; uint32_t icao = 0; std::string flight, reg; bool pos = false; double lat = 0, lon = 0; };
    struct E { vibeavshare::Extra ex; std::string flight, reg; bool pos = false; int64_t made = 0, used = 0; };

    /** ★ Full: the least recently asked eighth goes (one pass, rare — never on every insert). */
    void evictLocked_() {
        std::vector<std::pair<int64_t, std::string>> by;
        by.reserve(map_.size());
        for (const auto& kv : map_) by.push_back({ kv.second.used, kv.first });
        const size_t drop = map_.size() - kMax * 7 / 8;
        std::nth_element(by.begin(), by.begin() + (std::ptrdiff_t)drop, by.end());
        for (size_t k = 0; k < drop; ++k) map_.erase(by[k].second);
    }
    void start_() {
        if (started_) return;
        started_ = true;
        std::thread([this] {
            vibeDecoderThread("vibe-avinfo");   // ★ decoder priority: below network, audio and spectrum
            for (;;) {
                { std::unique_lock<std::mutex> lk(m_); cv_.wait(lk, [this] { return !q_.empty(); }); }
                runPending();
            }
        }).detach();
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::unordered_map<std::string, E> map_;
    std::deque<Want> q_;
    std::unordered_set<std::string> queued_;
    bool async_ = true, started_ = false;
    uint64_t lookups_ = 0, queuedTotal_ = 0;
};

/** The hook for vibeavshare::toJson. */
inline vibeavshare::ExtraFn extraFn() {
    return [](const vibeavshare::Row& w, vibeavshare::Extra& ex) { Cache::get().extra(w, ex); };
}

}  // namespace vibeavinfo
