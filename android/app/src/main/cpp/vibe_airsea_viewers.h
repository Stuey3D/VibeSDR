// vibe_airsea_viewers.h — V12 beta96 WHO HAS THE STANDALONE AIR & SEA MAP OPEN, and the owner's limits on it
// (docs/v12/ADSB-FEED.md §The standalone map's limits).
//
// ★★ Stuart, 11-26: "so people on limited or sketchy connections can host it". The standalone map takes no radio, so the
//    radios' listener limits never saw it: a link on a forum could put a hundred maps on a 4G uplink. The owner sets:
//      · "Map viewers" — how many may have it open at once (0 = no limit; 20 by default);
//      · "Map time limit" — minutes one address may keep it open (0 = none, the default).
//
// ── Counting by TICKET, not by address ────────────────────────────────────────────────────────────────────────────────
//   The viewer asks for a ticket when it opens (GET /vibeserver/airsea/join) and sends it with every poll (`vt=`). A ticket
//   not seen for kIdleMs (30 s — ten missed polls) has gone. So two people behind one router are two viewers, and a tab
//   closed without a word stops counting within 30 s. ★ A poll WITHOUT a ticket (an older page or app, a radio's own map)
//   is not refused and not counted — the listener on a radio is already counted as a listener. Honest gap: someone who
//   writes their own poller can read aircraft.json without a ticket; the limit is for the map, not a lock on the data.
//
// ── The time limit is per ADDRESS, as a radio's (memory: soft_session_limits.md) ─────────────────────────────────────
//   A session id is born with the page, so a reload would buy a fresh turn: the clock is the address's. A turn lapses after
//   an absence as long as the limit itself. When it is used up the viewer is told ("ended") and stops polling.
//   The viewer is warned before the end (the client counts `left` down; kWarnSec before the end it says so).
//
// ⚖ Addresses only, in memory, pruned — what the connection log already keeps. Nothing about what was looked at.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>

namespace vibeasview {

static constexpr int64_t kIdleMs = 30000;          ///< a ticket not polled for 30 s has gone
static constexpr int kWarnSec = 60;                ///< the viewer says "1 minute left" from here
static constexpr int kDefaultMax = 20;
static constexpr size_t kMaxTickets = 2000;        ///< bounded whatever the limit says (0 = "no limit" still means ≤ 2000)
static constexpr size_t kTodayMax = 2000;

enum class Poll { Ok, Unknown, Ended };

struct Join {
    bool ok = false;        ///< a ticket was given
    bool full = false;      ///< refused: the map is full (n of max)
    bool ended = false;     ///< refused: this address has used its time (back after `back` s)
    std::string ticket;
    int n = 0, max = 0;     ///< viewers now (including this one when ok), the limit (0 = none)
    int limitSec = 0;       ///< the time limit (0 = none)
    int left = -1;          ///< seconds left of this address's turn (-1 = no limit)
    int back = 0;           ///< ended: seconds until the turn lapses
};

class Gate {
public:
    /** The owner's limits (live). max 0 = no limit; minutes 0 = no time limit. */
    void set(int maxViewers, int minutes) {
        std::lock_guard<std::mutex> lk(m_);
        max_ = std::max(0, std::min(1000, maxViewers));
        limitMs_ = (int64_t)std::max(0, std::min(1440, minutes)) * 60000;
    }
    int max() { std::lock_guard<std::mutex> lk(m_); return max_; }
    int minutes() { std::lock_guard<std::mutex> lk(m_); return (int)(limitMs_ / 60000); }

    /** A viewer opens the map. `owner`: the admin's own browser/app — never refused (still counted). */
    Join join(const std::string& ip, int64_t nowMs, bool owner = false) {
        std::lock_guard<std::mutex> lk(m_);
        prune_(nowMs);
        Join r;
        r.max = max_; r.limitSec = (int)(limitMs_ / 1000);
        r.n = (int)tickets_.size();
        Turn& t = turn_(ip, nowMs);
        if (limitMs_ > 0 && !owner) {
            const int64_t used = nowMs - t.start;
            if (used >= limitMs_) {
                r.ended = true;
                r.back = (int)std::max<int64_t>(1, (t.last + limitMs_ - nowMs) / 1000);
                return r;
            }
        }
        if (!owner && ((max_ > 0 && r.n >= max_) || tickets_.size() >= kMaxTickets)) { r.full = true; return r; }
        std::string id = newId_();
        while (tickets_.count(id)) id = newId_();
        tickets_[id] = { ip, nowMs, nowMs };
        t.last = nowMs;
        r.ok = true; r.ticket = id; r.n = (int)tickets_.size();
        r.left = limitMs_ > 0 && !owner ? (int)((t.start + limitMs_ - nowMs) / 1000) : -1;
        if (owner) owners_.insert(id);
        return r;
    }
    /** A poll with ticket `id`. `left`: seconds left of the turn (-1 = no limit). Unknown = expired or never given (the
     *  viewer joins again — a server restart, a 30 s gap). Ended = the time limit: the viewer stops. */
    Poll poll(const std::string& id, const std::string& ip, int64_t nowMs, int* left = nullptr, int* n = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        prune_(nowMs);
        if (left) *left = -1;
        if (n) *n = (int)tickets_.size();
        auto it = tickets_.find(id);
        if (it == tickets_.end()) return Poll::Unknown;
        it->second.last = nowMs;
        Turn& t = turn_(it->second.ip.empty() ? ip : it->second.ip, nowMs);
        t.last = nowMs;
        if (limitMs_ > 0 && !owners_.count(id)) {
            const int64_t rem = t.start + limitMs_ - nowMs;
            if (rem <= 0) { end_(it, nowMs); return Poll::Ended; }
            if (left) *left = (int)(rem / 1000);
        }
        return Poll::Ok;
    }
    /** The viewer closed the map (CLOSE MAP, the page going away). */
    void leave(const std::string& id, int64_t nowMs) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = tickets_.find(id);
        if (it != tickets_.end()) end_(it, nowMs);
    }
    int count(int64_t nowMs) { std::lock_guard<std::mutex> lk(m_); prune_(nowMs); return (int)tickets_.size(); }

    /** The admin page's view: {"now":n,"max":m,"minutes":x,"viewers":[{"ip","cc","forSecs","secs"}],"today":[{"ip","cc","at","end"}]}.
     *  `cc(ip)` — the country code (the shim's geoip), may be null. */
    std::string json(int64_t nowMs, const std::function<std::string(const std::string&)>& cc = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        prune_(nowMs);
        std::string o = "{\"now\":" + std::to_string(tickets_.size()) + ",\"max\":" + std::to_string(max_)
                      + ",\"minutes\":" + std::to_string(limitMs_ / 60000) + ",\"viewers\":[";
        bool first = true;
        for (const auto& kv : tickets_) {
            o += first ? "{" : ",{"; first = false;
            o += "\"ip\":\"" + esc_(kv.second.ip) + "\",\"cc\":\"" + esc_(cc ? cc(kv.second.ip) : std::string())
               + "\",\"forSecs\":" + std::to_string((long long)((nowMs - kv.second.since) / 1000))
               + ",\"secs\":" + std::to_string((long long)((nowMs - kv.second.last) / 1000)) + "}";
        }
        o += "],\"today\":[";
        first = true;
        // ★ the visitor stats (admin.ts machineStats): each visit as a connection-log row — the open ones too
        auto row = [&](const std::string& ip, int64_t at, int64_t end) {
            o += first ? "{" : ",{"; first = false;
            o += "\"ip\":\"" + esc_(ip) + "\",\"cc\":\"" + esc_(cc ? cc(ip) : std::string()) + "\",\"at\":" + std::to_string((long long)(at / 1000))
               + ",\"end\":" + std::to_string((long long)(end / 1000)) + "}";
        };
        for (const auto& v : today_) if (nowMs - v.end <= 24 * 3600 * 1000LL) row(v.ip, v.at, v.end);
        for (const auto& kv : tickets_) row(kv.second.ip, kv.second.since, nowMs);
        return o + "]}";
    }
    /** Tests. */
    void reset() { std::lock_guard<std::mutex> lk(m_); tickets_.clear(); turns_.clear(); today_.clear(); owners_.clear(); }

private:
    struct Ticket { std::string ip; int64_t since = 0, last = 0; };
    struct Turn { int64_t start = 0, last = 0; };
    struct Visit { std::string ip; int64_t at = 0, end = 0; };
    void end_(std::map<std::string, Ticket>::iterator it, int64_t nowMs) {
        today_.push_back({ it->second.ip, it->second.since, std::min(nowMs, std::max(it->second.last, it->second.since)) });
        while (today_.size() > kTodayMax) today_.pop_front();
        owners_.erase(it->first);
        tickets_.erase(it);
    }
    void prune_(int64_t nowMs) {
        for (auto it = tickets_.begin(); it != tickets_.end();) {
            if (nowMs - it->second.last > kIdleMs) { auto d = it++; end_(d, nowMs); } else ++it;
        }
        while (!today_.empty() && nowMs - today_.front().end > 24 * 3600 * 1000LL) today_.pop_front();
        // ★ a turn lapses after an absence as long as the limit (no limit: after the idle time — nothing to remember)
        const int64_t lapse = std::max<int64_t>(limitMs_, kIdleMs);
        for (auto it = turns_.begin(); it != turns_.end();) {
            bool open = false;
            for (const auto& kv : tickets_) if (kv.second.ip == it->first) { open = true; break; }
            if (!open && nowMs - it->second.last > lapse) it = turns_.erase(it); else ++it;
        }
    }
    Turn& turn_(const std::string& ip, int64_t nowMs) {
        auto it = turns_.find(ip);
        if (it == turns_.end()) it = turns_.emplace(ip, Turn{ nowMs, nowMs }).first;
        return it->second;
    }
    std::string newId_() {
        static const char* hx = "0123456789abcdef";
        std::string s(20, '0');
        for (auto& ch : s) ch = hx[rng_() & 15];
        return s;
    }
    static std::string esc_(const std::string& s) {
        std::string o;
        for (unsigned char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += char(c); } else if (c >= 0x20 && c < 0x7F) o += char(c); }
        return o;
    }
    std::mutex m_;
    int max_ = kDefaultMax;
    int64_t limitMs_ = 0;
    std::map<std::string, Ticket> tickets_;
    std::map<std::string, Turn> turns_;
    std::deque<Visit> today_;
    std::set<std::string> owners_;   ///< tickets the admin holds: counted, never refused or timed
    std::mt19937_64 rng_{ std::random_device{}() };
};
inline Gate& gate() { static Gate g; return g; }

}  // namespace vibeasview
