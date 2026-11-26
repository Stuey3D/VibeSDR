// vibe_station_log.h — V12: AIS and APRS in the gallery — the day's messages as a plain .txt and the day's SUMMARY card of
// every station heard (docs/v12/AIS-APRS.md "The gallery"). ISM's day log (vibe_ism_gallery.h) is the model, without its
// privacy machinery: everything AIS and APRS carry is a public broadcast by design (a ship's name and MMSI, a callsign).
//
// ★★ NEVER A WRITE PER MESSAGE (Stuart's rule for continuous decoders, 2026-10-22): a busy coast is a message a second all
//    day, on a Pi 2's SD card. Lines are held in RAM and APPENDED every minute or 8 KB, at UTC midnight, and when the
//    decoder stops. The card is rewritten in place (Store::replace) at most once a minute, only when something new came.
// ★★★ V12 beta92: ONE log and ONE card per day per channel for the whole SERVER, every radio feeding them, each message
//     once (vibe_gallery_shared.h; DayLog's private notes). Older per-radio items stay as they were.
// ★ What reaches the log is what the station list counts as NEW: a ship's position report repeats every 2–10 s, so the
//   service hands over a line only when it CHANGES the readable line (moved on, a new name, a new destination) and at
//   most once a minute per station for a moving one. The log reads as a log, not a firehose.
#pragma once
#include "vibe_gallery.h"
#include "vibe_gallery_shared.h"
#include "vibe_station_sink.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vibestation {

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
        if (t / 86400 != day_) { flushLocked(true); newDayLocked(t); }
        char ts[16]; const time_t tt = time_t(t); struct tm g{}; gmtime_r(&tt, &g);
        std::snprintf(ts, sizeof ts, "%02d:%02d:%02d  ", g.tm_hour, g.tm_min, g.tm_sec);
        Pending p; p.text = std::string(ts) + h.line + snrToken(h.snr); p.key = h.key; p.kind = h.kind; p.name = h.name; p.line = h.line; p.t = t;
        bufBytes_ += p.text.size() + 1;
        pending_.push_back(std::move(p));
        seen_.insert(h.key);
        if (pending_.size() >= 4096 || bufBytes_ >= kFlushBytes) flushLocked(false);
    }
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        if (t / 86400 != day_) { flushLocked(true); newDayLocked(t); return; }
        if (!pending_.empty() && t - lastFlush_ >= kFlushSec) flushLocked(false);
        if (cardDirty_ && t - lastCard_ >= kCardSec) cardFromState(t);
    }
    void finish() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        flushLocked(true);
        if (!logName_.empty()) store_.close(logName_);
        if (!cardName_.empty()) store_.close(cardName_);
    }
    /** Stations THIS process heard today (the admin line / tests); the card counts the whole server's. */
    size_t stations() { std::lock_guard<std::mutex> lk(m_); return seen_.size(); }

private:
    /* ★★★ V12 beta92 ONE LOG AND ONE CARD PER SERVER (vibe_gallery_shared.h; Stuart 11-22 "all radios on same server should
     *  feed the same log"). Every radio process's flush, under the shared Lock: the day's log found by its SLOT alone
     *  (kind, day, kHz, part — never the radio, profile or origin), this process's lines DEDUPED against its tail (the same
     *  message another radio heard within 20 s is one line), the survivors appended; the card's rows kept in the shared
     *  STATE (<gallery>/.shared/<slot>.state — every station, its count of DISTINCT messages, first and last) and the
     *  card drawn from it, so it is the whole server's. */
    struct Pending { std::string text, key, kind, name, line; int64_t t = 0; };
    struct Row { std::string kind, name, line; int64_t first = 0, last = 0; long count = 0; };
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
    vibe::gallery::Meta logMeta() const {
        vibe::gallery::Meta m = meta(slotBase() + "-log" + std::to_string(part_));
        m.title = o_.label + " " + date(day_) + " \xc2\xb7 every message" + (part_ > 1 ? " (part " + std::to_string(part_) + ")" : "");
        m.dlName = vibe::gallery::downloadSafe(o_.label + " " + date(day_) + (part_ > 1 ? " (" + std::to_string(part_) + ")" : "") + ".txt");
        return m;
    }
    void newDayLocked(int64_t t) {
        if (!logName_.empty()) store_.close(logName_);
        if (!cardName_.empty()) store_.close(cardName_);
        logName_.clear(); cardName_.clear(); seen_.clear(); part_ = 1;
        day_ = t / 86400;
        vibe::gallery::sharedday::pruneStates(store_, t);
    }
    /** The day's log: this process's pending lines, deduped, appended (or the item made). Caller holds the shared Lock.
     *  @return which pending entries were NEW (written). */
    std::vector<bool> writeLog() {
        namespace sh = vibe::gallery::sharedday;
        std::vector<std::string> texts; texts.reserve(pending_.size());
        for (const auto& p : pending_) texts.push_back(p.text);
        for (int guard = 0; guard < 64; ++guard) {
            if (logName_.empty()) logName_ = sh::adopt(store_, logMeta());
            if (!logName_.empty() && store_.sizeOf(logName_) + bufBytes_ > kPartMaxBytes) { store_.close(logName_); logName_.clear(); ++part_; continue; }
            break;
        }
        std::string tail;
        std::vector<bool> keep;
        const int64_t t = now();
        own_.prune(t);
        const std::vector<std::string> own = own_.list();
        auto remember = [&] { for (size_t i = 0; i < texts.size(); i++) if (keep[i]) own_.add(texts[i], t); };
        if (!logName_.empty() && sh::readTail(store_, logName_, tail)) {
            keep = sh::newLines(tail, texts, sh::kDedupeSec, &own);
            std::string add;
            for (size_t i = 0; i < texts.size(); i++) if (keep[i]) { add += texts[i]; add += '\n'; }
            if (add.empty() || store_.append(logName_, add)) { store_.retag(logName_, logMeta()); remember(); return keep; }
            store_.close(logName_); logName_.clear();
        }
        keep = sh::newLines(std::string(), texts);
        remember();
        std::string body;
        for (size_t i = 0; i < texts.size(); i++) if (keep[i]) { body += texts[i]; body += '\n'; }
        const std::string head = o_.label + " \xc2\xb7 " + date(day_) + " (UTC) \xc2\xb7 every radio on this server, each message once\n\n";
        logName_ = store_.create(logMeta(), "txt", head + body);
        return keep;
    }
    void flushLocked(bool final) {
        namespace sh = vibe::gallery::sharedday;
        const int64_t t = now();
        lastFlush_ = t;
        if (pending_.empty()) return;
        if (store_.dir().empty() || !store_.enabled()) { pending_.clear(); bufBytes_ = 0; return; }
        sh::Lock fl(store_.dir());
        const std::vector<bool> keep = writeLog();
        // ★ the card's rows: the shared state, with this process's NEW messages merged in
        const std::string slot = slotBase() + "-card";
        std::map<std::string, Row> rows;
        uint64_t lines = 0, dropped = 0;
        readRows(sh::readState(store_, slot), rows, lines, dropped);
        for (size_t i = 0; i < pending_.size(); i++) {
            if (!keep[i]) continue;   // ★ another radio already logged (and counted) this message
            const Pending& p = pending_[i];
            Row& r = rows[p.key];
            if (r.first == 0 || p.t < r.first) r.first = p.t;
            if (p.t >= r.last) { r.last = p.t; r.line = p.line; r.name = p.name; if (!p.kind.empty()) r.kind = p.kind; }
            if (r.kind.empty()) r.kind = p.kind;
            ++r.count; ++lines;
        }
        while (rows.size() > kMaxRows) {
            auto old = rows.begin();
            for (auto it = rows.begin(); it != rows.end(); ++it) if (it->second.last < old->second.last) old = it;
            rows.erase(old); ++dropped;
        }
        sh::writeState(store_, slot, rowsText(rows, lines, dropped));
        pending_.clear(); bufBytes_ = 0;
        // ★ the card at most once a kCardSec (a busy coast flushes on 8 KB sooner), and always at the end
        if (final || t - lastCard_ >= kCardSec) { writeCard(rows, lines, dropped, t); lastCard_ = t; cardDirty_ = false; }
        else cardDirty_ = true;
    }
    /** The card from the shared state alone (its last change was not yet drawn). */
    void cardFromState(int64_t t) {
        namespace sh = vibe::gallery::sharedday;
        lastCard_ = t; cardDirty_ = false;
        if (store_.dir().empty() || !store_.enabled()) return;
        sh::Lock fl(store_.dir());
        std::map<std::string, Row> rows; uint64_t lines = 0, dropped = 0;
        readRows(sh::readState(store_, slotBase() + "-card"), rows, lines, dropped);
        writeCard(rows, lines, dropped, t);
    }
    static void readRows(const std::string& text, std::map<std::string, Row>& rows, uint64_t& lines, uint64_t& dropped) {
        namespace sh = vibe::gallery::sharedday;
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
            const std::vector<std::string> f = sh::splitTabs(text.substr(p, e - p)); p = e + 1;
            if (f.size() == 3 && f[0] == "#v1") { lines = std::strtoull(f[1].c_str(), nullptr, 10); dropped = std::strtoull(f[2].c_str(), nullptr, 10); continue; }
            if (f.size() != 7 || f[0].empty()) continue;
            Row r; r.kind = f[1]; r.first = std::atoll(f[2].c_str()); r.last = std::atoll(f[3].c_str()); r.count = std::atol(f[4].c_str());
            r.name = f[5]; r.line = f[6];
            rows[f[0]] = r;
        }
    }
    static std::string rowsText(const std::map<std::string, Row>& rows, uint64_t lines, uint64_t dropped) {
        namespace sh = vibe::gallery::sharedday;
        std::string o = "#v1\t" + std::to_string((unsigned long long)lines) + "\t" + std::to_string((unsigned long long)dropped) + "\n";
        for (const auto& kv : rows)
            o += sh::field(kv.first) + "\t" + sh::field(kv.second.kind) + "\t" + std::to_string((long long)kv.second.first) + "\t"
               + std::to_string((long long)kv.second.last) + "\t" + std::to_string(kv.second.count) + "\t" + sh::field(kv.second.name) + "\t"
               + sh::field(kv.second.line) + "\n";
        return o;
    }
    std::string cardTitle(const std::map<std::string, Row>& rows) const {
        std::map<std::string, int> by;
        for (const auto& kv : rows) by[kv.second.kind]++;
        std::string t = o_.label + " " + date(day_) + " \xc2\xb7 " + std::to_string(rows.size()) + " " + o_.what;
        std::string parts;
        for (const auto& kv : by) {
            if (!parts.empty()) parts += ", ";
            parts += std::to_string(kv.second) + " " + (o_.kindWord ? o_.kindWord(kv.first, kv.second) : kv.first);
        }
        if (!parts.empty() && by.size() > 1) t += ": " + parts;
        return t;
    }
    void writeCard(const std::map<std::string, Row>& rows, uint64_t lines, uint64_t dropped, int64_t t) {
        if (rows.empty()) return;
        std::vector<const std::pair<const std::string, Row>*> v;
        for (const auto& kv : rows) v.push_back(&kv);
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->second.last > b->second.last; });
        std::string body = cardTitle(rows) + "\n" + std::to_string(lines) + " messages logged \xc2\xb7 every radio on this server"
                         + " \xc2\xb7 updated " + hhmm(t) + " UTC\n\n";
        const Row* most = nullptr;
        for (auto* p : v) if (!most || p->second.count > most->count) most = &p->second;
        if (most) body += "Most heard: " + most->name + " (" + std::to_string(most->count) + ")\n";
        body += "Newest: " + v.front()->second.name + "\n\n";
        body += "last   first  count  station\n";
        for (auto* p : v) {
            char b[40]; std::snprintf(b, sizeof b, "%s  %s  %5ld  ", hhmm(p->second.last).c_str(), hhmm(p->second.first).c_str(), p->second.count);
            body += b; body += p->second.line; body += '\n';
        }
        if (dropped) body += "\n(" + std::to_string(dropped) + " stations heard earlier not listed)\n";
        vibe::gallery::Meta m = meta(slotBase() + "-card");
        m.title = cardTitle(rows);
        m.dlName = vibe::gallery::downloadSafe(o_.label + " " + date(day_) + " summary.txt");
        if (cardName_.empty()) cardName_ = vibe::gallery::sharedday::adopt(store_, m);
        if (cardName_.empty() || !store_.replace(cardName_, m, body, /*toNewest=*/true)) {
            if (!cardName_.empty()) store_.close(cardName_);
            cardName_ = store_.create(m, "txt", body);
        }
    }

    vibe::gallery::Store& store_;
    Options o_;
    std::mutex m_;
    std::vector<Pending> pending_;
    vibe::gallery::sharedday::OwnLines own_;   ///< ★ the lines this radio wrote lately (never taken for another radio's)
    std::set<std::string> seen_;
    size_t bufBytes_ = 0;
    std::string logName_, cardName_;
    int64_t day_ = 0, lastFlush_ = 0, lastCard_ = 0;
    int part_ = 1;
    bool done_ = false, cardDirty_ = false;
};

} // namespace vibestation
