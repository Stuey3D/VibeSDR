// vibe_ism_gallery.h — ISM IN THE GALLERY: the day's readings as a plain .txt, and the day's SUMMARY card (V12, 2026-10-25).
//
// ★★★ WHY (Stuart, 2026-10-25): "ISM needs to be in the gallery too … same as OWRX does it but in our readable format."
//     Two items per band per UTC day, whoever was listening — a LISTENER in ISM mode or BACKGROUND decoding:
//  • THE DAY'S LOG — "ISM 433.92 2026-10-25.txt" when downloaded: one line per reading, the device's READABLE line exactly
//    as the ISM list shows it, prefixed with the UTC time and the frequency it was heard on:
//        17:01:12  433.920  Oregon Scientific THN132N · 10.5 °C · battery low · ch 1
//    ★ "What the list counts as a new reading": vibeism::Tracker folds the same line from the same device inside 3 s
//      (a sensor sends its frame 2-6 times in a row), and only what it counts reaches this file. A belt-and-braces fold
//      here does the same, so a second source can never double a line.
//  • THE SUMMARY CARD — FT8's background summary, mirrored (vibe_bgdecode.h Ft8SummaryTap's rolling card): ONE standing
//    card per band per day, rewritten in place (Store::replace — tmp + rename) at most once a minute and only when
//    something new was heard, moved to the top of the list; written once more when ISM stops. "14 devices: 6 weather,
//    5 TPMS, 2 key fobs, 1 doorbell / remote", the most heard, the newest, and the LAST READING OF EACH DEVICE.
//
// ★★ NEVER A WRITE PER READING (Stuart's rule for continuous text decoders, 2026-10-22, the RTTY one): a busy 433 MHz
//    band is a reading every few seconds all day, and the target hardware is a Pi 2 on an SD card. The log is held in RAM
//    and APPENDED every minute or 8 KB, at a UTC midnight, and at once when ISM stops (a band change, a listener leaving
//    ISM or arriving over background decoding, a profile switch, a clean shutdown). A power cut loses at most a minute.
//    ★ Bounded: a day's log is split into parts at kPartMaxBytes ("… (part 2)"), so none grows for ever.
//
// ★★★ PRIVACY (Stuart, 2026-10-25 — the hard rule vibe_ism_service.h enforces): a TPMS, a key fob, a fixed-code remote,
//     a "Suspected key fob" is only ever its anonymous per-server name ("TPMS #2") — in the log, the card, the index.
//     This file never sees anything else: it is handed the Tracker's own name and line (vibe_ism_sink.h), and writes
//     nothing it was not handed. No ID, no code, no bits, no raw fields.
//
// ★★ A RESTART, said plainly. The card is its own record: after a restart (an update, a reboot) the day's card is found
//    again (Store::adoptSlot) and its device table read back, so the day's counts carry on. The nicknames do NOT carry
//    over — the salt that makes them is drawn fresh at every start and never stored (that is the privacy rule) — so
//    a car heard before and after a restart is two rows. The new process numbers its devices above the ones the day
//    already named (nickFloor → Tracker::reserveNicks), so "TPMS #2" never means two cars on one card.
#pragma once
#include "vibe_gallery.h"
#include "vibe_ism_sink.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vibe {
namespace ismgal {

// ── The kinds, as the card names them ───────────────────────────────────────────────────────────────────────────────
struct KindWord { const char* kind; const char* one; const char* many; };
inline const std::vector<KindWord>& kindWords() {
    static const std::vector<KindWord> k = {
        { "weather",    "weather",           "weather" },
        { "tpms",       "TPMS",              "TPMS" },
        { "keyfob",     "key fob",           "key fobs" },
        { "suspectfob", "suspected key fob", "suspected key fobs" },
        { "doorbell",   "doorbell / remote", "doorbells / remotes" },
        { "sensor",     "sensor",            "sensors" },
        { "energy",     "energy meter",      "energy meters" },
    };
    return k;
}
inline std::string kindOne(const std::string& kind) { for (const auto& k : kindWords()) if (kind == k.kind) return k.one; return "other"; }
inline std::string kindMany(const std::string& kind, int n) {
    for (const auto& k : kindWords()) if (kind == k.kind) return n == 1 ? k.one : k.many;
    return "other";
}
inline std::string kindFromWord(const std::string& w) { for (const auto& k : kindWords()) if (w == k.one) return k.kind; return "other"; }

static const char* const kDot = " \xc2\xb7 ";   // " · "
static const char* const kTimes = "\xc3\x97";   // "×"

/** ★ What makes two readings ONE device on the card: the name, and for a sensor with a channel, its channel (the list
 *  draws "ch 2" last). Built from the LINE, so a card read back after a restart keys its rows the same way. */
inline std::string deviceKey(const std::string& line) {
    const size_t first = line.find(kDot);
    const std::string name = first == std::string::npos ? line : line.substr(0, first);
    const size_t last = line.rfind(kDot);
    if (last != std::string::npos && line.compare(last + 4, 3, "ch ") == 0) return name + "|" + line.substr(last + 4);
    return name;
}
/** "TPMS #12" → 12; anything else → 0. */
inline int nickOf(const std::string& name) {
    const size_t h = name.rfind(" #");
    if (h == std::string::npos) return 0;
    const int n = std::atoi(name.c_str() + h + 2);
    return n > 0 ? n : 0;
}

// ── One band, one day at a time ─────────────────────────────────────────────────────────────────────────────────────
class DayLog : public vibeism::Sink {
public:
    static constexpr size_t   kFlushBytes   = 8192;            // ★ the RAM buffer's size threshold
    static constexpr int64_t  kFlushSec     = 60;              // ★ …and its time threshold
    static constexpr int64_t  kCardSec      = 60;              // ★ the card is rewritten at most this often
    static constexpr uint64_t kPartMaxBytes = 4ull << 20;      // ★ a day's log splits into parts past this
    static constexpr size_t   kMaxRows      = 2000;            // ★ the card's device table (the least recently heard go)
    static constexpr double   kFoldSec      = 3.0;             // ★ Tracker's own fold, repeated here

    struct Options {
        std::string bandId;        ///< "433"
        std::string bandName;      ///< "433.92 MHz ISM"
        double      bandHz = 0;    ///< the band's middle — the items' frequency
        std::string origin;        ///< "background" | "live"
        std::string profile;       ///< "Profile 1" (background), "" (a listener) — said in the card, not in the index
        std::function<int64_t()> clock;     ///< UTC seconds (tests); empty = the wall clock
    };

    DayLog(gallery::Store& store, Options o) : store_(store), o_(std::move(o)) {
        std::lock_guard<std::mutex> lk(m_);
        openDay(now() / 86400);
    }
    ~DayLog() override { finish(); }

    void heard(const vibeism::Heard& h) override {
        if (h.line.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        rollIfNewDay(t);
        const std::string key = deviceKey(h.line);
        Dev& d = devs_[key];
        // ★ the fold: the same line from the same device inside 3 s is the same reading
        if (d.count > 0 && d.line == h.line && double(t) - double(d.last) < kFoldSec) { d.last = t; return; }
        if (d.count == 0) { d.first = t; d.kind = h.kind; d.name = h.name.empty() ? key : h.name; }
        d.count++; d.last = t; d.line = h.line;
        readings_++;
        char lead[48];
        std::snprintf(lead, sizeof lead, "%s  %.3f  ", gallery::utcStamp(t, "%H:%M:%S").c_str(), h.freqHz / 1e6);
        buf_ += lead + gallery::printable(h.line, 400) + "\n";
        cardDirty_ = true;
        trimRows();
        if (buf_.size() >= kFlushBytes) flushLog(t);
    }
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        rollIfNewDay(t);
        if (!buf_.empty() && t - lastFlush_ >= kFlushSec) flushLog(t);
        if (cardDirty_ && t - cardWrite_ >= kCardSec) writeCard(t, false);
    }
    void finish() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        const int64_t t = now();
        flushLog(t);
        if (cardDirty_ || !cardName_.empty()) writeCard(t, true);
        closeAll();
    }

    /** ★ The highest nickname of each secret kind the day's card already uses (see the header: "A RESTART"). */
    std::map<std::string, int> nickFloor() const {
        std::lock_guard<std::mutex> lk(m_);
        std::map<std::string, int> f;
        for (const auto& kv : devs_) { const int n = nickOf(kv.second.name); if (n > f[kv.second.kind]) f[kv.second.kind] = n; }
        return f;
    }
    // ── test seams ──
    std::string logName() const { std::lock_guard<std::mutex> lk(m_); return logName_; }
    std::string cardName() const { std::lock_guard<std::mutex> lk(m_); return cardName_; }
    size_t buffered() const { std::lock_guard<std::mutex> lk(m_); return buf_.size(); }
    uint64_t readings() const { std::lock_guard<std::mutex> lk(m_); return readings_; }
    size_t devices() const { std::lock_guard<std::mutex> lk(m_); return devs_.size(); }
    std::string cardText() const { std::lock_guard<std::mutex> lk(m_); return cardBody(now()); }
    std::string cardTitleNow() const { std::lock_guard<std::mutex> lk(m_); return cardTitle(); }
    /** For the admin line: "14 devices · 212 readings today". */
    std::string brief() const {
        std::lock_guard<std::mutex> lk(m_);
        return std::to_string(devs_.size()) + (devs_.size() == 1 ? " device" : " devices") + kDot
             + std::to_string((unsigned long long)readings_) + (readings_ == 1 ? " reading" : " readings") + " today";
    }

private:
    struct Dev { std::string kind, name, line; int64_t first = 0, last = 0; uint64_t count = 0; };
    int64_t now() const { return o_.clock ? o_.clock() : gallery::nowUtc(); }
    std::string date(int64_t day) const { return gallery::utcStamp(day * 86400, "%Y-%m-%d"); }
    /** "433.92" from "433.92 MHz ISM", "868" from "868 MHz SRD". */
    std::string bandShort() const { const size_t sp = o_.bandName.find(' '); return sp == std::string::npos ? o_.bandName : o_.bandName.substr(0, sp); }
    std::string bandMhz() const { return bandShort() + " MHz"; }
    std::string logSlot(int part) const { return "ismlog-" + o_.bandId + "-" + date(day_) + (part > 1 ? "-" + std::to_string(part) : ""); }
    std::string cardSlot() const { return "ismsum-" + o_.bandId + "-" + date(day_); }
    gallery::Meta meta(const std::string& slot, const std::string& origin) const {
        gallery::Meta m;
        m.kind = "ism"; m.origin = origin; m.freqHz = o_.bandHz; m.mode = ""; m.decoder = "ism";
        m.utc = day_ * 86400;          // ★ a DAY's item: listed at the day it covers
        m.slot = slot;
        return m;
    }
    std::string heardBy(const std::string& origin) const {
        if (origin == "both") return "background decoding and listeners in ISM mode";
        if (origin == "background") return "background decoding" + (o_.profile.empty() ? std::string() : " (" + o_.profile + ")");
        return "a listener in ISM mode";
    }
    /** The origin an item should carry once THIS writer has written into it. */
    std::string merged(const std::string& had) const { return had.empty() || had == o_.origin ? o_.origin : "both"; }

    /** Find the day's items again (after a restart, or a band coming back) — and read the card's table back. */
    void openDay(int64_t day) {
        day_ = day;
        devs_.clear(); readings_ = 0; dropped_ = 0; buf_.clear(); cardDirty_ = false;
        logName_.clear(); cardName_.clear(); logOrigin_.clear(); cardOrigin_.clear();
        lastFlush_ = cardWrite_ = now();
        part_ = 1;
        // ★ the log: the highest part of the day that exists
        for (int p = 1; p <= 64; ++p) {
            const std::string n = store_.adoptSlot(meta(logSlot(p), o_.origin), /*anyOrigin=*/true);
            if (n.empty()) break;
            if (!logName_.empty()) store_.close(logName_);
            logName_ = n; part_ = p;
        }
        if (!logName_.empty()) logOrigin_ = store_.field(logName_, "o");
        cardName_ = store_.adoptSlot(meta(cardSlot(), o_.origin), /*anyOrigin=*/true);
        if (!cardName_.empty()) {
            cardOrigin_ = store_.field(cardName_, "o");
            std::string body;
            if (store_.readItem(cardName_, body)) restore(body);
        }
    }
    void closeAll() {
        if (!logName_.empty()) store_.close(logName_);
        if (!cardName_.empty()) store_.close(cardName_);
    }
    void rollIfNewDay(int64_t t) {
        if (t / 86400 == day_) return;
        // ★ UTC midnight: yesterday's buffer into yesterday's file, its card written for the last time, then a new day
        flushLog(t);
        if (cardDirty_ || !cardName_.empty()) writeCard(t, true);
        closeAll();
        openDay(t / 86400);
    }
    void flushLog(int64_t t) {
        lastFlush_ = t;
        if (buf_.empty()) return;
        if (!logName_.empty() && store_.sizeOf(logName_) + buf_.size() > kPartMaxBytes) { store_.close(logName_); logName_.clear(); part_++; logOrigin_.clear(); }
        bool ok = false;
        if (!logName_.empty()) {
            ok = store_.append(logName_, buf_);
            if (ok && merged(logOrigin_) != logOrigin_) { logOrigin_ = merged(logOrigin_); store_.retag(logName_, logMeta()); }
            if (!ok) { store_.close(logName_); logName_.clear(); }   // ★ evicted or cleared: a new file
        }
        if (!ok) {
            logOrigin_ = o_.origin;
            const std::string head = "ISM " + bandMhz() + kDot + "every reading heard on " + date(day_) + " (UTC)"
                + (part_ > 1 ? kDot + std::string("part ") + std::to_string(part_) : std::string()) + "\n"
                + "Each line: the time (UTC), the frequency (MHz) and the reading exactly as the ISM list shows it. "
                  "Tyre sensors, key fobs and remotes are only ever their nicknames here.\n\n";
            logName_ = store_.create(logMeta(), "txt", head + buf_);
        }
        buf_.clear();
    }
    gallery::Meta logMeta() const {
        gallery::Meta m = meta(logSlot(part_), logOrigin_.empty() ? o_.origin : logOrigin_);
        m.title = "ISM " + bandMhz() + kDot + date(day_) + kDot + "every reading" + (part_ > 1 ? " (part " + std::to_string(part_) + ")" : "");
        m.dlName = "ISM " + bandShort() + " " + date(day_) + (part_ > 1 ? " (" + std::to_string(part_) + ")" : "") + ".txt";
        return m;
    }

    std::vector<const Dev*> byLast() const {
        std::vector<const Dev*> v;
        for (const auto& kv : devs_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Dev* a, const Dev* b) { return a->last != b->last ? a->last > b->last : a->name < b->name; });
        return v;
    }
    /** "6 weather, 5 TPMS, 2 key fobs, 1 doorbell / remote" — the most first. */
    std::string mix() const {
        std::map<std::string, int> n;
        for (const auto& kv : devs_) n[kv.second.kind]++;
        std::vector<std::pair<int, std::string>> v;
        for (const auto& kv : n) v.push_back({ kv.second, kv.first });
        // ★ the most first; a tie in the order kindWords() lists them (weather first, as Stuart wrote it)
        auto rank = [](const std::string& k) { const auto& w = kindWords(); for (size_t i = 0; i < w.size(); i++) if (k == w[i].kind) return (int)i; return 99; };
        std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : rank(a.second) < rank(b.second); });
        std::string o;
        for (const auto& e : v) { if (!o.empty()) o += ", "; o += std::to_string(e.first) + " " + kindMany(e.second, e.first); }
        return o;
    }
    std::string devicesWord() const { return std::to_string(devs_.size() + dropped_) + (devs_.size() + dropped_ == 1 ? " device" : " devices"); }
    const Dev* mostHeard() const {
        const Dev* best = nullptr;
        for (const auto& kv : devs_) if (!best || kv.second.count > best->count || (kv.second.count == best->count && kv.second.first < best->first)) best = &kv.second;
        return best;
    }
    const Dev* newest() const {
        const Dev* best = nullptr;
        for (const auto& kv : devs_) if (!best || kv.second.first > best->first) best = &kv.second;
        return best;
    }
    /** ★ The name a row is SHOWN by: the name, and the channel when it is what tells two sensors apart. */
    static std::string shown(const Dev& d) {
        const size_t bar = deviceKey(d.line).find('|');
        return bar == std::string::npos ? d.name : d.name + kDot + deviceKey(d.line).substr(bar + 1);
    }
    /** "ISM 433.92 MHz · 2026-10-25 · 14 devices: 6 weather, 5 TPMS … · most heard: … (212×) · newest: …" — the parts
     *  that fit the index's 120 bytes, in that order. */
    std::string cardTitle() const {
        std::string t = "ISM " + bandMhz() + kDot + date(day_) + kDot + devicesWord();
        const std::string m = mix();
        if (!m.empty()) t += ": " + m;
        auto add = [&](const std::string& part) { if (t.size() + 4 + part.size() <= 120) t += kDot + part; };
        if (const Dev* d = mostHeard(); d && d->count > 1) add("most heard: " + shown(*d) + " (" + std::to_string((unsigned long long)d->count) + kTimes + ")");
        if (const Dev* d = newest(); d && devs_.size() > 1) add("newest: " + shown(*d));
        return t;
    }
    std::string cardBody(int64_t t) const {
        std::string o = "ISM summary" + std::string(kDot) + o_.bandName + kDot + date(day_) + " (UTC)\n";
        o += "Heard by " + heardBy(cardOrigin_.empty() ? o_.origin : merged(cardOrigin_)) + ".\n";
        o += "Updated " + gallery::utcStamp(t, "%H:%M UTC") + " \xe2\x80\x94 at most once a minute while ISM runs. Every reading is in the day's log, \"ISM "
           + bandMhz() + kDot + date(day_) + kDot + "every reading\".\n\n";
        o += devicesWord() + (devs_.empty() ? std::string() : ": " + mix()) + "\n";
        char b[200];
        std::snprintf(b, sizeof b, "Readings:     %llu\n", (unsigned long long)readings_); o += b;
        // ★ only when one stands out — "most heard (1×)" out of a tie of ones would name a device at random
        if (const Dev* d = mostHeard(); d && d->count > 1) o += "Most heard:   " + shown(*d) + " (" + std::to_string((unsigned long long)d->count) + kTimes + ")\n";
        if (const Dev* d = newest()) o += "Newest:       " + shown(*d) + " (first heard " + gallery::utcStamp(d->first, "%H:%M:%S") + ")\n";
        if (dropped_) o += "Not listed:   " + std::to_string((unsigned long long)dropped_) + " (this card keeps the " + std::to_string(kMaxRows) + " most recently heard)\n";
        o += "\nThe last reading of each device, most recent first (last heard \xc2\xb7 first heard \xc2\xb7 readings \xc2\xb7 kind \xc2\xb7 reading):\n";
        for (const Dev* d : byLast())
            o += gallery::utcStamp(d->last, "%H:%M:%S") + kDot + gallery::utcStamp(d->first, "%H:%M:%S") + kDot
               + std::to_string((unsigned long long)d->count) + kTimes + kDot + kindOne(d->kind) + kDot + d->line + "\n";
        return o;
    }
    /** ★★ THE CARD IS ITS OWN RECORD: read the day's table back (see "A RESTART"). Anything that does not parse is
     *  skipped — a damaged card costs its rows, never the process. */
    void restore(const std::string& body) {
        const size_t at = body.find("The last reading of each device");
        if (at == std::string::npos) return;
        auto hms = [&](const std::string& s, int64_t& out) {
            int h, m, sec;
            if (s.size() != 8 || std::sscanf(s.c_str(), "%2d:%2d:%2d", &h, &m, &sec) != 3) return false;
            out = day_ * 86400 + h * 3600 + m * 60 + sec;
            return true;
        };
        const std::string dot = kDot;
        size_t p = body.find('\n', at);
        while (p != std::string::npos && p + 1 < body.size()) {
            const size_t e = body.find('\n', p + 1);
            const std::string row = body.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
            p = e;
            std::vector<std::string> f; size_t q = 0;
            for (int i = 0; i < 4; ++i) { const size_t d = row.find(dot, q); if (d == std::string::npos) break; f.push_back(row.substr(q, d - q)); q = d + dot.size(); }
            if (f.size() != 4) continue;
            Dev d;
            if (!hms(f[0], d.last) || !hms(f[1], d.first)) continue;
            d.count = std::strtoull(f[2].c_str(), nullptr, 10);
            if (d.count == 0) continue;
            d.kind = kindFromWord(f[3]);
            d.line = row.substr(q);
            if (d.line.empty()) continue;
            const size_t n = d.line.find(dot);
            d.name = n == std::string::npos ? d.line : d.line.substr(0, n);
            devs_[deviceKey(d.line)] = d;
        }
        // ★ the totals: the readings line, else the rows' own counts
        unsigned long long r = 0;
        if (const size_t rp = body.find("Readings:"); rp != std::string::npos && std::sscanf(body.c_str() + rp, "Readings: %llu", &r) == 1) readings_ = r;
        else for (const auto& kv : devs_) readings_ += kv.second.count;
        if (const size_t np = body.find("Not listed:"); np != std::string::npos) { unsigned long long n = 0; if (std::sscanf(body.c_str() + np, "Not listed: %llu", &n) == 1) dropped_ = n; }
    }
    void trimRows() {
        while (devs_.size() > kMaxRows) {
            auto old = devs_.begin();
            for (auto it = devs_.begin(); it != devs_.end(); ++it) if (it->second.last < old->second.last) old = it;
            devs_.erase(old);
            dropped_++;
        }
    }
    void writeCard(int64_t t, bool last) {
        (void)last;
        cardWrite_ = t; cardDirty_ = false;
        if (devs_.empty()) return;                                       // ★ nothing heard: no card
        const std::string origin = cardOrigin_.empty() ? o_.origin : merged(cardOrigin_);
        gallery::Meta m = meta(cardSlot(), origin);
        m.title = cardTitle();
        m.dlName = "ISM " + bandShort() + " " + date(day_) + " summary.txt";
        const std::string body = cardBody(t);
        if (cardName_.empty() || !store_.replace(cardName_, m, body, /*toNewest=*/true)) {
            if (!cardName_.empty()) store_.close(cardName_);
            cardName_ = store_.create(m, "txt", body);
        }
        cardOrigin_ = origin;
    }

    gallery::Store& store_;
    Options o_;
    mutable std::mutex m_;
    bool done_ = false;
    int64_t day_ = 0, lastFlush_ = 0, cardWrite_ = 0;
    int part_ = 1;
    std::string buf_, logName_, cardName_, logOrigin_, cardOrigin_;
    std::map<std::string, Dev> devs_;
    uint64_t readings_ = 0, dropped_ = 0;
    bool cardDirty_ = false;
};

} // namespace ismgal
} // namespace vibe
