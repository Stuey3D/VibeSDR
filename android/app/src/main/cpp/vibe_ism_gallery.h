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
#include <cmath>
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
        { "meter",      "meter",             "meters" },           // ★ V12 wM-Bus: water, heat, gas … (vibe_wmbus.h)
        // ★ V12 LoRa (vibe_lora_track.h, docs/v12/LORA-MESH.md): the mesh nodes, LoRaWAN devices, unnamed LoRa, and the
        //   mesh networks' PUBLIC messages (each its own row — "meshmsg")
        { "meshtastic", "Meshtastic node",   "Meshtastic nodes" },
        { "meshcore",   "MeshCore node",     "MeshCore nodes" },
        { "lorawan",    "LoRaWAN device",    "LoRaWAN devices" },
        { "lora",       "LoRa transmitter",  "LoRa transmitters" },
        { "meshmsg",    "public message",    "public messages" },
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
/** ★ V12 meters: "Water meter · Sensus · 12345678 · …" → its first three fields — the type, the maker and the serial
 *  ARE the meter (the plain deviceKey would take "Water meter" alone, and every water meter would be one row). */
inline std::string meterKey(const std::string& line) {
    size_t p = 0;
    for (int i = 0; i < 3; ++i) { const size_t d = line.find(kDot, p); if (d == std::string::npos) return line; p = d + 4; }
    return line.substr(0, p - 4);
}
/** ★ V12 LoRa: "Meshtastic · Northampton Base (NBAS) · …" → its first two fields (the network and the node); a public
 *  message is its own row (the whole line). */
inline std::string loraKey(const std::string& line) {
    const size_t a = line.find(kDot);
    if (a == std::string::npos) return line;
    const size_t b = line.find(kDot, a + 4);
    return b == std::string::npos ? line : line.substr(0, b);
}
inline std::string keyOf(const std::string& kind, const std::string& line) {
    if (kind == "meter") return meterKey(line);
    if (kind == "meshmsg") return line;
    if (kind == "meshtastic" || kind == "meshcore" || kind == "lorawan" || kind == "lora") return loraKey(line);
    return deviceKey(line);
}
/** "Water meter" → "water", "Cold water meter" → "cold water", "Heat cost allocator" → "heat cost allocator". */
inline std::string meterSort(const std::string& name) {
    std::string t = name.substr(0, name.find(kDot));
    const std::string suf = " meter";
    if (t.size() > suf.size() && t.compare(t.size() - suf.size(), suf.size(), suf) == 0) t.resize(t.size() - suf.size());
    else if (t == "Meter" || t.compare(0, 6, "Meter ") == 0) t = "other";
    if (!t.empty() && t[0] >= 'A' && t[0] <= 'Z') t[0] = char(t[0] - 'A' + 'a');
    return t;
}

/** "TPMS #12" → 12; anything else → 0. */
inline int nickOf(const std::string& name) {
    const size_t h = name.rfind(" #");
    if (h == std::string::npos) return 0;
    const int n = std::atoi(name.c_str() + h + 2);
    return n > 0 ? n : 0;
}

// ── ★★ V12 2026-11-01: "STATIONS RECEIVED" — the day's positioned mesh nodes, for the gallery's MAP ──────────────────────
// Stuart: "being able to open up the map in the Gallery so that if I have this set as the background profile we can see
// which stations we received". A third standing item per band per day, beside the log and the summary — the summary's
// rules exactly: RAM only between writes, rewritten in place (tmp + rename) at most once a minute and only when something
// was heard, once more when ISM stops; found again and read back after a restart, a band change, a listener arriving
// over background decoding or a profile switch (each a new DayLog adopting the same slot). Bounded: kMaxNodes, the least
// recently heard go. It counts against the gallery's size cap like any item (Store).
// ★★★ PRIVACY: only what NodeFix carries — the PUBLISHED position (a companion's rounded to ~1 km and marked "approx"),
//     the node's advertised name and type. No message, ever.
// The body is a readable .txt whose table the clients parse for the map (web/client/src/ism.ts ismNodesParse — keep the
// two in step): one node a line, " · " between fields.
struct NodeRow {
    std::string net, type, name, node, origin;
    double lat = 0, lon = 0; bool approx = false; int hops = -1; float snr = 0; double freqHz = 0;
    int64_t first = 0, last = 0; uint64_t count = 0;
};
inline const char* netWord(const std::string& n) { return n == "meshcore" ? "MeshCore" : n == "meshtastic" ? "Meshtastic" : "LoRa"; }
inline std::string netFromWord(const std::string& w) { return w == "MeshCore" ? "meshcore" : w == "Meshtastic" ? "meshtastic" : ""; }
inline const char* typeWord(const std::string& t, int n) {
    if (t == "repeater") return n == 1 ? "repeater" : "repeaters";
    if (t == "room") return n == 1 ? "room server" : "room servers";
    if (t == "sensor") return n == 1 ? "sensor" : "sensors";
    return n == 1 ? "companion" : "companions";
}
inline std::string typeFromWord(const std::string& w) {
    if (w == "repeater") return "repeater";
    if (w == "room server") return "room";
    if (w == "sensor") return "sensor";
    if (w == "companion") return "companion";
    return "";
}
/** A name as it may stand in one field of the table: printable, no " · " (the separator) inside it. */
inline std::string fieldSafe(const std::string& s) {
    std::string o = gallery::printable(s, 40), r;
    for (size_t i = 0; i < o.size(); ++i) {
        if ((unsigned char)o[i] == 0xC2 && i + 1 < o.size() && (unsigned char)o[i + 1] == 0xB7) { r += '-'; ++i; continue; }
        r += o[i];
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r.empty() ? std::string("-") : r;
}
inline double kmBetween(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180, p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 12742.0 * std::asin(std::min(1.0, std::sqrt(a)));
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
    /** ★★ V12 METERS ARE THROTTLED: a meter sends every 16 s, so a street of 50 would write 270 000 lines a day. Its line
     *  reaches the log the first time it is heard in the day, when its reading or status changes (the line without
     *  signal and interval — Heard::core), or once per this many seconds otherwise. */
    static constexpr int64_t  kMeterEverySec = 15 * 60;
    static constexpr size_t   kMaxNodes      = 500;            // ★ the "stations received" card's table (the least recently heard go)

    struct Options {
        std::string bandId;        ///< "433"
        std::string bandName;      ///< "433.92 MHz ISM"
        double      bandHz = 0;    ///< the band's middle — the items' frequency
        std::string origin;        ///< "background" | "live"
        std::string profile;       ///< "Profile 1" (background), "" (a listener) — said in the card, not in the index
        std::function<int64_t()> clock;     ///< UTC seconds (tests); empty = the wall clock
        double      rxLat = NAN, rxLon = NAN;   ///< ★ this server's own position (the card's "furthest"); NaN = not set
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
        const std::string key = keyOf(h.kind, h.line);
        Dev& d = devs_[key];
        if (h.kind == "meter") {
            const bool first = d.count == 0;
            if (first) { d.first = t; d.kind = h.kind; d.name = h.name.empty() ? key : h.name; }
            d.count++; d.last = t; d.line = h.line;
            const bool news = first || d.written == 0 || h.core != d.core || t - d.written >= kMeterEverySec;
            d.core = h.core;
            cardDirty_ = true;
            if (!news) return;
            d.written = t;
            readings_++;
            char lead[48];
            std::snprintf(lead, sizeof lead, "%s  %.3f  ", gallery::utcStamp(t, "%H:%M:%S").c_str(), h.freqHz / 1e6);
            buf_ += lead + gallery::printable(h.line, 400) + "\n";
            trimRows();
            if (buf_.size() >= kFlushBytes) flushLog(t);
            return;
        }
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
    /** ★ V12: a positioned mesh node — the "stations received" card. Never a write here: the tick writes. */
    void node(const vibeism::NodeFix& f) override {
        if (!std::isfinite(f.lat) || !std::isfinite(f.lon) || std::fabs(f.lat) > 90 || std::fabs(f.lon) > 180) return;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        rollIfNewDay(t);
        NodeRow& n = nodes_[f.net + "|" + (f.node.empty() ? f.name : f.node)];
        if (n.count == 0) { n.first = t; n.net = f.net; n.node = f.node; }
        n.count++; n.last = t;
        n.type = f.type; n.name = f.name; n.lat = f.lat; n.lon = f.lon; n.approx = f.approx;   // ★ newest wins
        if (f.hops >= 0) n.hops = f.hops;
        n.snr = f.snr; n.freqHz = f.freqHz;
        n.origin = n.origin.empty() || n.origin == o_.origin ? o_.origin : "both";
        nodesDirty_ = true;
        while (nodes_.size() > kMaxNodes) {
            auto old = nodes_.begin();
            for (auto it = nodes_.begin(); it != nodes_.end(); ++it) if (it->second.last < old->second.last) old = it;
            nodes_.erase(old);
            nodesDropped_++;
        }
    }
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now();
        rollIfNewDay(t);
        if (!buf_.empty() && t - lastFlush_ >= kFlushSec) flushLog(t);
        if (cardDirty_ && t - cardWrite_ >= kCardSec) writeCard(t, false);
        if (nodesDirty_ && t - nodesWrite_ >= kCardSec) writeNodes(t);
    }
    void finish() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        const int64_t t = now();
        flushLog(t);
        if (cardDirty_ || !cardName_.empty()) writeCard(t, true);
        if (nodesDirty_) writeNodes(t);
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
    std::string nodesName() const { std::lock_guard<std::mutex> lk(m_); return nodesName_; }
    size_t nodes() const { std::lock_guard<std::mutex> lk(m_); return nodes_.size(); }
    std::string nodesText() const { std::lock_guard<std::mutex> lk(m_); return nodesBody(now()); }
    std::string nodesTitleNow() const { std::lock_guard<std::mutex> lk(m_); return nodesTitle(); }
    /** For the admin line: "14 devices · 212 readings today". */
    std::string brief() const {
        std::lock_guard<std::mutex> lk(m_);
        return std::to_string(devs_.size()) + (devs_.size() == 1 ? " device" : " devices") + kDot
             + std::to_string((unsigned long long)readings_) + (readings_ == 1 ? " reading" : " readings") + " today";
    }

private:
    struct Dev { std::string kind, name, line, core; int64_t first = 0, last = 0, written = 0; uint64_t count = 0; };
    int64_t now() const { return o_.clock ? o_.clock() : gallery::nowUtc(); }
    std::string date(int64_t day) const { return gallery::utcStamp(day * 86400, "%Y-%m-%d"); }
    /** "433.92" from "433.92 MHz ISM", "868" from "868 MHz SRD". */
    std::string bandShort() const { const size_t sp = o_.bandName.find(' '); return sp == std::string::npos ? o_.bandName : o_.bandName.substr(0, sp); }
    std::string bandMhz() const { return bandShort() + " MHz"; }
    std::string logSlot(int part) const { return "ismlog-" + o_.bandId + "-" + date(day_) + (part > 1 ? "-" + std::to_string(part) : ""); }
    std::string cardSlot() const { return "ismsum-" + o_.bandId + "-" + date(day_); }
    /** ★ The clients know a "stations received" card by this slot's prefix ("ismnodes-") and draw its MAP key. */
    std::string nodesSlot() const { return "ismnodes-" + o_.bandId + "-" + date(day_); }
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
        nodes_.clear(); nodesDirty_ = false; nodesDropped_ = 0; nodesName_.clear(); nodesOrigin_.clear();
        lastFlush_ = cardWrite_ = nodesWrite_ = now();
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
        nodesName_ = store_.adoptSlot(meta(nodesSlot(), o_.origin), /*anyOrigin=*/true);
        if (!nodesName_.empty()) {
            nodesOrigin_ = store_.field(nodesName_, "o");
            std::string body;
            if (store_.readItem(nodesName_, body)) restoreNodes(body);
        }
    }
    void closeAll() {
        if (!logName_.empty()) store_.close(logName_);
        if (!cardName_.empty()) store_.close(cardName_);
        if (!nodesName_.empty()) store_.close(nodesName_);
    }
    void rollIfNewDay(int64_t t) {
        if (t / 86400 == day_) return;
        // ★ UTC midnight: yesterday's buffer into yesterday's file, its card written for the last time, then a new day
        flushLog(t);
        if (cardDirty_ || !cardName_.empty()) writeCard(t, true);
        if (nodesDirty_) writeNodes(t);
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
                  "Tyre sensors, key fobs and remotes are only ever their nicknames here. A meter is written when its reading "
                  "or status changes, and otherwise once every 15 minutes.\n\n";
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
        for (const auto& e : v) {
            if (!o.empty()) o += ", ";
            o += std::to_string(e.first) + " " + kindMany(e.second, e.first);
            if (e.second == "meter") o += " (" + meterMix() + ")";   // ★ "58 meters (51 water, 4 heat, 3 gas)"
        }
        return o;
    }
    std::string meterMix() const {
        std::map<std::string, int> n;
        for (const auto& kv : devs_) if (kv.second.kind == "meter") n[meterSort(kv.second.name)]++;
        std::vector<std::pair<int, std::string>> v;
        for (const auto& kv : n) v.push_back({ kv.second, kv.first });
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        std::string o;
        for (const auto& e : v) { if (!o.empty()) o += ", "; o += std::to_string(e.first) + " " + e.second; }
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
            d.name = d.kind == "meter" ? meterKey(d.line) : (d.kind == "meshtastic" || d.kind == "meshcore" || d.kind == "lorawan" || d.kind == "lora") ? loraKey(d.line)
                   : n == std::string::npos ? d.line : d.line.substr(0, n);
            devs_[keyOf(d.kind, d.line)] = d;
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

    // ── ★ V12 "stations received" (see NodeRow) ──
    std::string networks() const {
        bool mc = false, mt = false;
        for (const auto& kv : nodes_) { mc |= kv.second.net == "meshcore"; mt |= kv.second.net == "meshtastic"; }
        return mc && mt ? "MeshCore + Meshtastic" : mc ? "MeshCore" : mt ? "Meshtastic" : "mesh";
    }
    const NodeRow* furthest(double& km) const {
        km = 0;
        if (!std::isfinite(o_.rxLat) || !std::isfinite(o_.rxLon)) return nullptr;
        const NodeRow* best = nullptr;
        for (const auto& kv : nodes_) { const double k = kmBetween(o_.rxLat, o_.rxLon, kv.second.lat, kv.second.lon); if (k > km) { km = k; best = &kv.second; } }
        return best;
    }
    /** "23 nodes heard · 9 repeaters · 2 room servers · 12 companions" */
    std::string nodesMix() const {
        std::map<std::string, int> n;
        for (const auto& kv : nodes_) n[kv.second.type]++;
        std::string o = std::to_string(nodes_.size() + nodesDropped_) + (nodes_.size() + nodesDropped_ == 1 ? " node heard" : " nodes heard");
        for (const char* t : {"repeater", "room", "sensor", "companion"}) if (n[t]) o += std::string(kDot) + std::to_string(n[t]) + " " + typeWord(t, n[t]);
        return o;
    }
    static std::string kmText(double km) { char b[32]; std::snprintf(b, sizeof b, km < 10 ? "%.1f km" : "%.0f km", km); return b; }
    /** "LoRa / MeshCore 868 MHz · 2026-11-01: 23 nodes heard · 9 repeaters · furthest 84 km" — what fits 120 bytes. */
    std::string nodesTitle() const {
        std::map<std::string, int> n;
        for (const auto& kv : nodes_) n[kv.second.type]++;
        std::string t = "LoRa / " + networks() + " " + bandMhz() + kDot + date(day_) + ": " + std::to_string(nodes_.size() + nodesDropped_)
                      + (nodes_.size() + nodesDropped_ == 1 ? " node heard" : " nodes heard");
        auto add = [&](const std::string& part) { if (t.size() + 4 + part.size() <= 120) t += kDot + part; };
        if (n["repeater"]) add(std::to_string(n["repeater"]) + " " + typeWord("repeater", n["repeater"]));
        double km; if (furthest(km)) add("furthest " + kmText(km));
        return t;
    }
    static std::string posText(const NodeRow& n) {
        char b[64];
        if (n.approx) std::snprintf(b, sizeof b, "%.2f, %.2f approx", n.lat, n.lon);
        else std::snprintf(b, sizeof b, "%.6f, %.6f", n.lat, n.lon);
        return b;
    }
    std::string nodesBody(int64_t t) const {
        const std::string origin = nodesOrigin_.empty() ? o_.origin : merged(nodesOrigin_);
        std::string o = "LoRa nodes heard" + std::string(kDot) + o_.bandName + kDot + date(day_) + " (UTC)\n";
        o += "Heard by " + heardBy(origin) + ".\n";
        o += "Updated " + gallery::utcStamp(t, "%H:%M UTC") + " \xe2\x80\x94 at most once a minute while ISM runs. MAP shows them on the map.\n";
        if (std::isfinite(o_.rxLat) && std::isfinite(o_.rxLon)) { char b[80]; std::snprintf(b, sizeof b, "Receiver:     %.4f, %.4f\n", o_.rxLat, o_.rxLon); o += b; }
        o += "Repeaters, room servers and sensors are shown where they advertise. Companion nodes are people: their positions are "
             "rounded to about 1 km and marked approx. No message is ever kept here.\n\n";
        o += nodesMix();
        double km; if (const NodeRow* f = furthest(km)) o += std::string(kDot) + "furthest " + kmText(km) + " (" + fieldSafe(f->name) + ")";
        o += "\n";
        if (nodesDropped_) o += "Not listed:   " + std::to_string((unsigned long long)nodesDropped_) + " (this card keeps the " + std::to_string(kMaxNodes) + " most recently heard)\n";
        o += "\nEach node, most recent first (last heard \xc2\xb7 first heard \xc2\xb7 times heard \xc2\xb7 heard by \xc2\xb7 network \xc2\xb7 type \xc2\xb7 name \xc2\xb7 id \xc2\xb7 position \xc2\xb7 hops \xc2\xb7 SNR dB \xc2\xb7 MHz):\n";
        std::vector<const NodeRow*> v;
        for (const auto& kv : nodes_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const NodeRow* a, const NodeRow* b) { return a->last != b->last ? a->last > b->last : a->name < b->name; });
        for (const NodeRow* n : v) {
            char tail[64];
            std::snprintf(tail, sizeof tail, "%s%.1f%s%.3f", kDot, double(n->snr), kDot, n->freqHz / 1e6);
            o += gallery::utcStamp(n->last, "%H:%M:%S") + kDot + gallery::utcStamp(n->first, "%H:%M:%S") + kDot
               + std::to_string((unsigned long long)n->count) + kTimes + kDot + (n->origin.empty() ? o_.origin : n->origin) + kDot
               + netWord(n->net) + kDot + typeWord(n->type, 1) + kDot + fieldSafe(n->name) + kDot + fieldSafe(n->node) + kDot
               + posText(*n) + kDot + (n->hops >= 0 ? std::to_string(n->hops) : std::string("-")) + tail + "\n";
        }
        return o;
    }
    /** ★ The card is its own record (the summary's rule): a damaged row costs that row, never the process. */
    void restoreNodes(const std::string& body) {
        const size_t at = body.find("Each node, most recent first");
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
            for (;;) { const size_t d = row.find(dot, q); if (d == std::string::npos) { f.push_back(row.substr(q)); break; } f.push_back(row.substr(q, d - q)); q = d + dot.size(); }
            if (f.size() != 12) continue;
            NodeRow n;
            if (!hms(f[0], n.last) || !hms(f[1], n.first)) continue;
            n.count = std::strtoull(f[2].c_str(), nullptr, 10);
            if (!n.count) continue;
            n.origin = f[3] == "background" || f[3] == "live" || f[3] == "both" ? f[3] : o_.origin;
            n.net = netFromWord(f[4]); n.type = typeFromWord(f[5]);
            if (n.net.empty() || n.type.empty()) continue;
            n.name = f[6]; n.node = f[7] == "-" ? std::string() : f[7];
            n.approx = f[8].find("approx") != std::string::npos;
            if (std::sscanf(f[8].c_str(), "%lf, %lf", &n.lat, &n.lon) != 2 || std::fabs(n.lat) > 90 || std::fabs(n.lon) > 180) continue;
            if (!n.approx && n.type == "companion") {   // ★ belt and braces: a person's node is never exact, even from a file
                n.lat = std::round(n.lat * 100) / 100; n.lon = std::round(n.lon * 100) / 100; n.approx = true;
            }
            n.hops = f[9] == "-" ? -1 : std::atoi(f[9].c_str());
            n.snr = float(std::atof(f[10].c_str())); n.freqHz = std::atof(f[11].c_str()) * 1e6;
            nodes_[n.net + "|" + (n.node.empty() ? n.name : n.node)] = n;
        }
        if (const size_t np = body.find("Not listed:"); np != std::string::npos) { unsigned long long d = 0; if (std::sscanf(body.c_str() + np, "Not listed: %llu", &d) == 1) nodesDropped_ = d; }
    }
    void writeNodes(int64_t t) {
        nodesWrite_ = t; nodesDirty_ = false;
        if (nodes_.empty()) return;
        const std::string origin = nodesOrigin_.empty() ? o_.origin : merged(nodesOrigin_);
        gallery::Meta m = meta(nodesSlot(), origin);
        m.title = nodesTitle();
        m.dlName = "LoRa nodes " + bandShort() + " " + date(day_) + ".txt";
        const std::string body = nodesBody(t);
        if (nodesName_.empty() || !store_.replace(nodesName_, m, body, /*toNewest=*/true)) {
            if (!nodesName_.empty()) store_.close(nodesName_);
            nodesName_ = store_.create(m, "txt", body);
        }
        nodesOrigin_ = origin;
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
    std::map<std::string, NodeRow> nodes_;
    std::string nodesName_, nodesOrigin_;
    uint64_t nodesDropped_ = 0;
    int64_t nodesWrite_ = 0;
    bool nodesDirty_ = false;
};

} // namespace ismgal
} // namespace vibe
