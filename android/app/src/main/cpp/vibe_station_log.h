// vibe_station_log.h — V12: AIS and APRS in the gallery — the day's messages as a plain .txt and the day's SUMMARY card of
// every station heard (docs/v12/AIS-APRS.md "The gallery"). ISM's day log (vibe_ism_gallery.h) is the model, without its
// privacy machinery: everything AIS and APRS carry is a public broadcast by design (a ship's name and MMSI, a callsign).
//
// ★★ NEVER A WRITE PER MESSAGE (Stuart's rule for continuous decoders, 2026-10-22): a busy coast is a message a second all
//    day, on a Pi 2's SD card. Lines are held in RAM and APPENDED every minute or 8 KB, at UTC midnight, and when the
//    decoder stops. The card is rewritten in place (Store::replace) at most once a minute, only when something new came.
// ★ What reaches the log is what the station list counts as NEW: a ship's position report repeats every 2–10 s, so the
//   service hands over a line only when it CHANGES the readable line (moved on, a new name, a new destination) and at
//   most once a minute per station for a moving one. The log reads as a log, not a firehose.
#pragma once
#include "vibe_gallery.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vibestation {

/** One new line for the log: the station's key (MMSI, callsign), what it is, its name and its readable line. */
struct Heard {
    std::string key;
    std::string kind;     ///< "A", "B", "base", "aton", "sar" (AIS) · "position", "weather", "object", "message" … (APRS)
    std::string name;
    std::string line;
    double freqHz = 0;
};
/** ★ Called on the decoder's OWN thread: heard() per new line, tick() about once a second, finish() once at the stop. */
class Sink {
public:
    virtual ~Sink() = default;
    virtual void heard(const Heard& h) = 0;
    virtual void tick() {}
    virtual void finish() {}
};

class DayLog : public Sink {
public:
    static constexpr size_t   kFlushBytes   = 8192;
    static constexpr int64_t  kFlushSec     = 60;
    static constexpr int64_t  kCardSec      = 60;
    static constexpr uint64_t kPartMaxBytes = 4ull << 20;
    static constexpr size_t   kMaxRows      = 3000;

    struct Options {
        std::string kind;        ///< the gallery kind: "ais" | "aprs"
        std::string label;       ///< "AIS" | "APRS"
        std::string what;        ///< the card's noun: "ships and stations" / "stations"
        double      freqHz = 0;
        std::string mode;        ///< "" (AIS, a whole-radio mode) | "nfm" | "usb" …
        std::string origin;      ///< "background" | "live"
        std::string profile;
        std::function<int64_t()> clock;   ///< UTC seconds (tests); empty = the wall clock
        /** "A" → "class A ship" for the card's breakdown; empty = the kind as it is. */
        std::function<std::string(const std::string&, int)> kindWord;
    };
    DayLog(vibe::gallery::Store& store, Options o) : store_(store), o_(std::move(o)) {
        std::lock_guard<std::mutex> lk(m_);
        day_ = now() / 86400; lastFlush_ = lastCard_ = now();
    }
    ~DayLog() override { finish(); }

    void heard(const Heard& h) override {
        if (h.line.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        if (t / 86400 != day_) { flushLocked(true); writeCardLocked(t); newDayLocked(t); }
        char ts[16]; const time_t tt = time_t(t); struct tm g{}; gmtime_r(&tt, &g);
        std::snprintf(ts, sizeof ts, "%02d:%02d:%02d  ", g.tm_hour, g.tm_min, g.tm_sec);
        buf_ += ts; buf_ += h.line; buf_ += '\n';
        Row& r = rows_[h.key];
        if (r.count == 0) { r.first = t; r.kind = h.kind; }
        if (!h.kind.empty()) r.kind = h.kind;
        r.name = h.name; r.line = h.line; r.last = t; ++r.count; ++lines_;
        if (rows_.size() > kMaxRows) {
            auto old = rows_.begin();
            for (auto it = rows_.begin(); it != rows_.end(); ++it) if (it->second.last < old->second.last) old = it;
            rows_.erase(old); ++dropped_;
        }
        dirty_ = true;
        if (buf_.size() >= kFlushBytes) flushLocked(false);
    }
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        if (t / 86400 != day_) { flushLocked(true); writeCardLocked(t); newDayLocked(t); return; }
        if (!buf_.empty() && t - lastFlush_ >= kFlushSec) flushLocked(false);
        if (dirty_ && t - lastCard_ >= kCardSec) writeCardLocked(t);
    }
    void finish() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        flushLocked(true);
        if (dirty_ || !rows_.empty()) writeCardLocked(now());
        if (!logName_.empty()) store_.close(logName_);
        if (!cardName_.empty()) store_.close(cardName_);
    }
    size_t stations() { std::lock_guard<std::mutex> lk(m_); return rows_.size(); }

private:
    struct Row { std::string kind, name, line; int64_t first = 0, last = 0; int count = 0; };
    int64_t now() const { return o_.clock ? o_.clock() : int64_t(::time(nullptr)); }
    static std::string date(int64_t day) {
        const time_t tt = time_t(day * 86400); struct tm g{}; gmtime_r(&tt, &g);
        char b[16]; std::snprintf(b, sizeof b, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday); return b;
    }
    static std::string hhmm(int64_t t) {
        const time_t tt = time_t(t); struct tm g{}; gmtime_r(&tt, &g);
        char b[8]; std::snprintf(b, sizeof b, "%02d:%02d", g.tm_hour, g.tm_min); return b;
    }
    vibe::gallery::Meta meta(const std::string& slot) const {
        vibe::gallery::Meta m;
        m.kind = o_.kind; m.origin = o_.origin; m.freqHz = o_.freqHz; m.mode = o_.mode; m.decoder = o_.kind;
        m.profile = o_.profile; m.utc = day_ * 86400; m.slot = slot;
        return m;
    }
    std::string slotBase() const { char b[48]; std::snprintf(b, sizeof b, "%s-%lld-%.0f", o_.kind.c_str(), (long long)day_, o_.freqHz / 1e3); return b; }
    void newDayLocked(int64_t t) {
        if (!logName_.empty()) store_.close(logName_);
        if (!cardName_.empty()) store_.close(cardName_);
        logName_.clear(); cardName_.clear(); rows_.clear(); part_ = 1; lines_ = 0; dropped_ = 0; dirty_ = false;
        day_ = t / 86400;
    }
    void flushLocked(bool /*final*/) {
        lastFlush_ = now();
        if (buf_.empty()) return;
        if (!logName_.empty() && store_.sizeOf(logName_) + buf_.size() > kPartMaxBytes) { store_.close(logName_); logName_.clear(); ++part_; }
        if (logName_.empty()) logName_ = store_.adoptSlot(meta(slotBase() + "-log" + std::to_string(part_)), true);
        bool ok = false;
        if (!logName_.empty()) { ok = store_.append(logName_, buf_); if (!ok) { store_.close(logName_); logName_.clear(); } }
        if (!ok) {
            vibe::gallery::Meta m = meta(slotBase() + "-log" + std::to_string(part_));
            m.title = o_.label + " " + date(day_) + " \xc2\xb7 every message" + (part_ > 1 ? " (part " + std::to_string(part_) + ")" : "");
            m.dlName = vibe::gallery::downloadSafe(o_.label + " " + date(day_) + (part_ > 1 ? " (" + std::to_string(part_) + ")" : "") + ".txt");
            const std::string head = o_.label + " \xc2\xb7 " + date(day_) + " (UTC)" + (o_.profile.empty() ? "" : " \xc2\xb7 " + o_.profile) + "\n\n";
            logName_ = store_.create(m, "txt", head + buf_);
        }
        buf_.clear();
    }
    std::string cardTitle() const {
        std::map<std::string, int> by;
        for (const auto& kv : rows_) by[kv.second.kind]++;
        std::string t = o_.label + " " + date(day_) + " \xc2\xb7 " + std::to_string(rows_.size()) + " " + o_.what;
        std::string parts;
        for (const auto& kv : by) {
            if (!parts.empty()) parts += ", ";
            parts += std::to_string(kv.second) + " " + (o_.kindWord ? o_.kindWord(kv.first, kv.second) : kv.first);
        }
        if (!parts.empty() && by.size() > 1) t += ": " + parts;
        return t;
    }
    void writeCardLocked(int64_t t) {
        lastCard_ = t; dirty_ = false;
        if (rows_.empty()) return;
        std::vector<const std::pair<const std::string, Row>*> v;
        for (const auto& kv : rows_) v.push_back(&kv);
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->second.last > b->second.last; });
        std::string body = cardTitle() + "\n" + std::to_string(lines_) + " messages logged" + (o_.profile.empty() ? "" : " \xc2\xb7 " + o_.profile)
                         + " \xc2\xb7 updated " + hhmm(t) + " UTC\n\n";
        const Row* most = nullptr;
        for (auto* p : v) if (!most || p->second.count > most->count) most = &p->second;
        if (most) body += "Most heard: " + most->name + " (" + std::to_string(most->count) + ")\n";
        body += "Newest: " + v.front()->second.name + "\n\n";
        body += "last   first  count  station\n";
        for (auto* p : v) {
            char b[40]; std::snprintf(b, sizeof b, "%s  %s  %5d  ", hhmm(p->second.last).c_str(), hhmm(p->second.first).c_str(), p->second.count);
            body += b; body += p->second.line; body += '\n';
        }
        if (dropped_) body += "\n(" + std::to_string(dropped_) + " stations heard earlier not listed)\n";
        vibe::gallery::Meta m = meta(slotBase() + "-card");
        m.title = cardTitle();
        m.dlName = vibe::gallery::downloadSafe(o_.label + " " + date(day_) + " summary.txt");
        if (cardName_.empty()) cardName_ = store_.adoptSlot(m, true);
        if (cardName_.empty() || !store_.replace(cardName_, m, body, /*toNewest=*/true)) {
            if (!cardName_.empty()) store_.close(cardName_);
            cardName_ = store_.create(m, "txt", body);
        }
    }

    vibe::gallery::Store& store_;
    Options o_;
    std::mutex m_;
    std::map<std::string, Row> rows_;
    std::string buf_, logName_, cardName_;
    int64_t day_ = 0, lastFlush_ = 0, lastCard_ = 0;
    int part_ = 1;
    uint64_t lines_ = 0, dropped_ = 0;
    bool dirty_ = false, done_ = false;
};

} // namespace vibestation
