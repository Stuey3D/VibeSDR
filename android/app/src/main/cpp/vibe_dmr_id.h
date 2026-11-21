// vibe_dmr_id.h — V12 beta90: DMR ID → CALLSIGN for the DV ID box (docs/v12/DMR-ID.md).
//
// Stuart (2026-11-21): "no callsigns on this dmr just weird numbers". A DMR transmission carries a 24-bit source ID and a
// destination (a talkgroup or another ID) — the numbers are RadioID.net registrations. The only callsign DMR itself
// carries is the OPTIONAL talker alias (vibe_dvsig.h decodes it), and most radios never send one.
//
// ★★★ RADIOID.NET'S TERMS DECIDED THE SHAPE (its "API and Data Use Policy", radioid.net/api, read 2026-11-21):
//     "Normal lookup use is allowed." · "Do not mirror or re-publish RadioID as a public feed, bulk export, reusable
//     database, competing directory" · "Commercial, bulk, mirrored, scraped, or public-facing redistributions require
//     approval." · "Automated clients should identify themselves with a clear User-Agent and contact address." ·
//     "Be gentle." So there is NO MIRROR in the directory Worker: each server looks up each NEW ID ONCE through the
//     per-ID lookup (https://radioid.net/api/dmr/user/?id=N), behind the OWNER'S SWITCH, OFF BY DEFAULT, until RadioID
//     says yes to the use (a public-facing server shows the answer to its listeners — "may require approval").
//
// ★★ BOUNDED: never the 270 k list in a Pi's RAM. At most kMaxEntries answers (least recently used out), a queue of
//    kMaxQueue IDs waiting, ONE request at a time at most every kMinGapMs, misses remembered for a day, a failing
//    service backed off (1 min doubling to an hour). Only the CALLSIGN is kept — never the name, city or e-mail state
//    the answer also carries.
// ★★ AMATEUR BANDS ONLY: the identities copy of the DV report is only ever written on an amateur band (vibe_dvsig.h, the
//    existing UK WTA s.48 rule), and this only annotates that copy.
// ★ The talkgroup NAME: RadioID publishes no talkgroup list to look up, so "TG 2351" stays a number (DMR-ID.md).
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace vibe {
namespace dmrid {

constexpr size_t kMaxEntries = 1024;        ///< answers kept (a busy repeater hears a few hundred IDs a day)
constexpr size_t kMaxQueue = 32;            ///< IDs waiting to be asked; more are simply asked on a later sighting
constexpr long   kMinGapMs = 2000;          ///< "be gentle": one request at a time, at most one every 2 s
constexpr long   kHitSec = 7 * 86400;       ///< a found callsign is asked again after a week
constexpr long   kMissSec = 86400;          ///< an ID RadioID does not know: not asked again for a day
constexpr long   kBackoffMinSec = 60, kBackoffMaxSec = 3600;

/** A DMR subscriber ID worth asking about: RadioID's user IDs are 7 digits (an MCC-prefixed number); 6-digit IDs are
 *  repeaters, and anything else (0, the all-call 16777215, the private ranges) is never a person. */
inline bool lookupable(uint32_t id) { return id >= 1000000 && id <= 9999999; }

/** RadioID's per-ID answer → the callsign ("" = not found / not that answer). Reads only `"callsign":"…"` of the result
 *  whose `"id"` is this ID; the callsign is whitelisted (A–Z 0–9 /, ≤ 12), since it is shown to listeners. */
inline std::string parseCallsign(const std::string& body, uint32_t id) {
    if (body.size() > 65536 || body.find("\"results\"") == std::string::npos) return "";
    const std::string idKey = "\"id\":" + std::to_string(id);
    size_t p = 0;
    while ((p = body.find('{', p + 1)) != std::string::npos) {
        const size_t e = body.find('}', p);
        if (e == std::string::npos) break;
        const std::string obj = body.substr(p, e - p + 1);
        const size_t ik = obj.find(idKey);
        if (ik == std::string::npos || (ik + idKey.size() < obj.size() && std::isdigit((unsigned char)obj[ik + idKey.size()]))) continue;
        const size_t c = obj.find("\"callsign\":\"");
        if (c == std::string::npos) continue;
        std::string call;
        for (size_t i = c + 12; i < obj.size() && obj[i] != '"' && call.size() < 12; ++i) {
            const char u = (char)std::toupper((unsigned char)obj[i]);
            if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '/') call += u; else return "";
        }
        return call.size() >= 3 ? call : "";
    }
    return "";
}

class Lookup {
public:
    static Lookup& shared() { static Lookup l; return l; }
    ~Lookup() { stop(); }

    using Fetch = std::function<std::string(const std::string& url)>;
    void setFetcher(Fetch f) { std::lock_guard<std::mutex> l(m_); fetch_ = std::move(f); }
    void setEnabled(bool on) {
        { std::lock_guard<std::mutex> l(m_); on_ = on; if (!on) queue_.clear(); }
        if (on) start();
    }
    bool enabled() const { std::lock_guard<std::mutex> l(m_); return on_; }
    // ── test seams ──
    void setClock(std::function<long long()> ms) { std::lock_guard<std::mutex> l(m_); clock_ = std::move(ms); }
    void setManual(bool manual) { std::lock_guard<std::mutex> l(m_); manual_ = manual; }   ///< tests: no thread, step()

    /** Non-blocking: the callsign if known; else "" and the ID is queued (once) to be asked. */
    std::string callFor(uint32_t id) {
        if (!lookupable(id)) return "";
        std::lock_guard<std::mutex> l(m_);
        if (!on_) return "";
        const long long now = nowMs();
        auto it = map_.find(id);
        if (it != map_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.pos);   // most recently used
            if (now < it->second.until) return it->second.call;
        }
        if (queued_(id)) return it != map_.end() ? it->second.call : "";
        if (queue_.size() < kMaxQueue) { queue_.push_back(id); cv_.notify_one(); }
        return it != map_.end() ? it->second.call : "";
    }
    /** Bumps each time an answer lands — the DV report folds it into its version so the box redraws. */
    uint64_t generation() const { std::lock_guard<std::mutex> l(m_); return gen_; }
    size_t size() const { std::lock_guard<std::mutex> l(m_); return map_.size(); }
    size_t queued() const { std::lock_guard<std::mutex> l(m_); return queue_.size(); }
    long long requests() const { std::lock_guard<std::mutex> l(m_); return requests_; }

    /** Ask about ONE queued ID if allowed now (the thread loops on it; tests call it). True = a request was made. */
    bool step() {
        std::unique_lock<std::mutex> l(m_);
        const long long now = nowMs();
        if (!on_ || !fetch_ || queue_.empty() || now < nextOk_) return false;
        const uint32_t id = queue_.front(); queue_.pop_front();
        Fetch f = fetch_;
        ++requests_;
        nextOk_ = now + kMinGapMs;
        l.unlock();
        const std::string body = f("https://radioid.net/api/dmr/user/?id=" + std::to_string(id));
        l.lock();
        const long long t = nowMs();
        if (body.empty() || body.find("\"results\"") == std::string::npos) {
            // ★ no answer / not the API's JSON (a block page, a timeout): back off the whole service, ask this ID later
            backoffSec_ = backoffSec_ ? std::min(kBackoffMaxSec, backoffSec_ * 2) : kBackoffMinSec;
            nextOk_ = t + backoffSec_ * 1000;
            return true;
        }
        backoffSec_ = 0;
        const std::string call = parseCallsign(body, id);
        put_(id, call, t + (call.empty() ? kMissSec : kHitSec) * 1000);
        if (!call.empty()) ++gen_;
        return true;
    }

    /** ★ The DV report's identities copy → the same with "srcCall" / "dstCall" after each DMR entry's "src" / private
     *  "dst" when the callsign is known (and queues the unknown ones). A DMR entry's talker alias stays as it is — the
     *  clients show the alias first when there is one. */
    std::string annotate(const std::string& j) {
        if (!enabled() || j.find("\"sys\":\"DMR\"") == std::string::npos) return j;
        std::string o; o.reserve(j.size() + 64);
        size_t i = 0;
        const size_t logAt = j.find("\"log\":[");
        if (logAt == std::string::npos) return j;
        o.append(j, 0, logAt + 7); i = logAt + 7;
        while (i < j.size()) {
            if (j[i] != '{') { o += j[i++]; continue; }
            // one log entry (its only nested object is "gps":{…})
            int depth = 0; size_t e = i;
            for (; e < j.size(); ++e) { if (j[e] == '{') ++depth; else if (j[e] == '}' && --depth == 0) break; }
            if (e >= j.size()) { o.append(j, i, std::string::npos); break; }
            std::string obj = j.substr(i, e - i + 1);
            if (obj.find("\"sys\":\"DMR\"") != std::string::npos) {
                auto num = [&](const char* k) -> long long {
                    const std::string key = std::string("\"") + k + "\":"; const size_t p = obj.find(key);
                    return p == std::string::npos ? -1 : std::atoll(obj.c_str() + p + key.size());
                };
                const long long src = num("src"), dst = num("dst");
                const bool grp = obj.find("\"grp\":true") != std::string::npos;
                std::string add;
                if (src > 0) { const std::string c = callFor((uint32_t)src); if (!c.empty()) add += ",\"srcCall\":\"" + c + "\""; }
                if (dst > 0 && !grp) { const std::string c = callFor((uint32_t)dst); if (!c.empty()) add += ",\"dstCall\":\"" + c + "\""; }
                if (!add.empty()) obj.insert(obj.size() - 1, add);
            }
            o += obj; i = e + 1;
        }
        return o;
    }

    void start() {
        std::lock_guard<std::mutex> l(m_);
        if (manual_ || th_.joinable()) return;
        stop_ = false;
        th_ = std::thread([this] {
            std::unique_lock<std::mutex> l(m_);
            while (!stop_) {
                cv_.wait_for(l, std::chrono::milliseconds(500), [this] { return stop_ || (!queue_.empty() && on_); });
                if (stop_) break;
                const long long wait = nextOk_ - nowMs();
                if (wait > 0) { cv_.wait_for(l, std::chrono::milliseconds(std::min<long long>(wait, 1000)), [this] { return stop_; }); continue; }
                l.unlock(); step(); l.lock();
            }
        });
    }
    void stop() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

private:
    struct E { std::string call; long long until; std::list<uint32_t>::iterator pos; };
    long long nowMs() const {
        return clock_ ? clock_() : (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    bool queued_(uint32_t id) const { return std::find(queue_.begin(), queue_.end(), id) != queue_.end(); }
    void put_(uint32_t id, const std::string& call, long long until) {
        auto it = map_.find(id);
        if (it != map_.end()) { it->second.call = call; it->second.until = until; lru_.splice(lru_.begin(), lru_, it->second.pos); return; }
        lru_.push_front(id);
        map_[id] = E{call, until, lru_.begin()};
        while (map_.size() > kMaxEntries) { map_.erase(lru_.back()); lru_.pop_back(); }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    bool on_ = false, stop_ = false, manual_ = false;
    Fetch fetch_;
    std::function<long long()> clock_;
    std::unordered_map<uint32_t, E> map_;
    std::list<uint32_t> lru_;
    std::deque<uint32_t> queue_;
    long long nextOk_ = 0, requests_ = 0;
    long backoffSec_ = 0;
    uint64_t gen_ = 0;
    std::thread th_;
};

}  // namespace dmrid
}  // namespace vibe
