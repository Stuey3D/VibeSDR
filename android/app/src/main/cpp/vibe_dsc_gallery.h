// vibe_dsc_gallery.h — DSC IN THE GALLERY (V12, 2026-10-26): the day's calls as one log per frequency, and every distress
// or urgency call as an item of its own, with its position for the map.
//
// ★★★ WHY TWO KINDS OF ITEM. DSC is a few bursts an hour on each frequency — mostly test calls and their
//     acknowledgements, coast stations' safety calls — and, rarely, the one that matters. So:
//  • THE DAY'S LOG — "DSC 8414.5 2026-10-26.txt" when downloaded: one line per call, the readable line exactly as the
//    decoder box shows it, prefixed with the UTC time and the frequency it was heard on:
//        14:32:07  2187.5 kHz  DISTRESS · MMSI 235012345 (United Kingdom) · Sinking · 50°12'N 004°10'W · 14:32 UTC · …
//    One item per frequency per UTC day, whoever was listening (background decoding and a listener's decoder write into
//    the same day's log — Store::adoptSlot with anyOrigin, as ISM's). Its title counts the day: "DSC 8414.5 kHz ·
//    26 Oct · 23 calls · 1 distress".
//  • A DISTRESS OR URGENCY CALL — an item of its own the moment it is decoded, titled with its line, marked "lv" 3 or 2
//    (the clients colour it), and pinned ("plat"/"plon") where a position was sent, so the gallery can put it on a map.
//    A transmission's repeats (a distress alert goes five times) are ONE item.
//
//  • ★★ V12 beta88 ANY CALL THAT SENT A POSITION — a routine or safety call too (Stuart, 2026-11-20, background decoding
//    on the Pi 500's locked-range RSP1B: "i have no map button to view them from the gallery"). On HF the common case is a
//    POSITION REPLY (a coast station asked a ship where it is); it was only a line in the day's log, so never on a map.
//    Now it is an item of its own, pinned, marked with its level ("lv" 1 safety, 0 routine — the map colours them), so
//    the SHIPPING MAP's DSC layer has it. Test calls and their acknowledgements never carry a position, so never one.
//
// ★★ NEVER A WRITE PER CALL for the log (the RTTY rule): held in RAM and appended once a minute, at a UTC midnight and
//    when the decoder stops. A distress item is written at once — it is the one thing worth the write. A positioned
//    routine / safety call is written at once too: they are rare (a few a day on a busy HF channel), and
//    ★ BOUNDED, beside the gallery's own size limit (the owner's space, vibe_gallery.h — the oldest item goes first):
//      · the same ship at the same position within kPinSameSec is ONE item (a coast station that polls a ship at anchor
//        gets the same answer each time — the log still has every line);
//      · at most kPinPerHour positioned routine / safety items per decoder per UTC hour — beyond that (a fault, a
//        malicious transmitter) the calls go only into the log, and the log says how many were not pinned.
//    Distress and urgency are never capped: they are the reason the gallery keeps DSC at all.
#pragma once
#include "vibe_gallery.h"
#include "decoders/dsc_decoder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vibe {
namespace bg {

class DscGalleryTap : public DecoderTap {
public:
    DscGalleryTap(gallery::Store& store, gallery::Meta meta, std::function<double()> dial = nullptr)
        : store_(store), meta_(std::move(meta)), dial_(std::move(dial)) {
        lastFlush_ = gallery::nowUtc();
    }
    ~DscGalleryTap() override { finish(true); }

    void dscCall(const dsc::Call& c, const std::string& line, const std::string& json, bool repeat) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_ || repeat) return;
        followDial();
        // ★ The log is named for the DSC FREQUENCY the call was heard on (2187.5), not the dial (2185.8 USB).
        const size_t rp = json.find("\"rf\":");
        if (rp != std::string::npos) { const double rf = std::strtod(json.c_str() + rp + 5, nullptr); if (rf > 0 && std::fabs(rf - rfHz_) > 50) {
            if (rfHz_ > 0 && !log_.empty()) { flush(gallery::nowUtc()); store_.close(log_); log_.clear(); calls_ = distress_ = urgency_ = 0; }
            rfHz_ = rf; } }
        const int64_t now = gallery::nowUtc();
        rollDay(now);
        pending_ += gallery::utcStamp(now, "%H:%M:%S") + "  " + freqText() + "  " + line + "\n";
        ++calls_;
        if (c.level() >= 3) ++distress_;
        else if (c.level() == 2) ++urgency_;
        if (c.level() >= 2) saveAlert(c, line, now);
        else if (positionOf(c).known) savePinned(c, line, now);   // ★ beta88 a routine / safety call with a position
        // ★ Distress and urgency go into the log at once too, not a minute later.
        if (c.level() >= 2 || now - lastFlush_ >= 60) flush(now);
    }
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t now = gallery::nowUtc();
        rollDay(now);
        if (!pending_.empty() && now - lastFlush_ >= 60) flush(now);
    }
    void finish(bool) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        flush(gallery::nowUtc());
        if (!log_.empty()) store_.close(log_);
    }
    size_t saved() const { std::lock_guard<std::mutex> lk(m_); return items_; }
    std::string logName() const { std::lock_guard<std::mutex> lk(m_); return log_; }

private:
    std::string freqText() const {
        char b[32];
        const double hz = rfHz_ > 0 ? rfHz_ : meta_.freqHz;
        if (hz >= 30e6) std::snprintf(b, sizeof b, "%.3f MHz", hz / 1e6);
        else std::snprintf(b, sizeof b, "%.1f kHz", hz / 1e3);
        return b;
    }
    std::string freqShort() const {
        char b[32];
        const double hz = rfHz_ > 0 ? rfHz_ : meta_.freqHz;
        if (hz >= 30e6) std::snprintf(b, sizeof b, "%.3f", hz / 1e6); else std::snprintf(b, sizeof b, "%.1f", hz / 1e3);
        return b;
    }
    /** A listener who retunes is on another frequency: its own day's log. */
    void followDial() {
        if (!dial_) return;
        const double d = dial_();
        if (std::fabs(d - meta_.freqHz) < 100.0) return;
        flush(gallery::nowUtc());
        if (!log_.empty()) store_.close(log_);
        log_.clear(); calls_ = distress_ = urgency_ = 0;
        meta_.freqHz = d; rfHz_ = 0;
    }
    void rollDay(int64_t now) {
        const int64_t day = now / 86400;
        if (day == day_) return;
        if (!log_.empty()) { flush(now); store_.close(log_); log_.clear(); }
        day_ = day; calls_ = distress_ = urgency_ = 0;
    }
    gallery::Meta logMeta(int64_t now) const {
        gallery::Meta m = meta_;
        // ★ One log per DSC frequency per day: the frequency is part of what finds it again (with the dial, in "hz").
        char fs[24]; std::snprintf(fs, sizeof fs, "%.0f", rfHz_ > 0 ? rfHz_ : meta_.freqHz);
        m.slot = "day-" + gallery::utcStamp(now, "%Y%m%d") + "-" + fs;
        m.utc = (now / 86400) * 86400;
        std::string t = "DSC " + freqText() + " \xc2\xb7 " + gallery::utcStamp(now, "%d %b");
        t += " \xc2\xb7 " + std::to_string(calls_) + (calls_ == 1 ? " call" : " calls");
        if (distress_) t += " \xc2\xb7 " + std::to_string(distress_) + " distress";
        if (urgency_) t += " \xc2\xb7 " + std::to_string(urgency_) + " urgency";
        m.title = t;
        m.level = distress_ ? 3 : urgency_ ? 2 : -1;
        m.dlName = "DSC " + freqShort() + " " + gallery::utcStamp(now, "%Y-%m-%d") + ".txt";
        return m;
    }
    void flush(int64_t now) {
        if (pending_.empty()) { lastFlush_ = now; return; }
        gallery::Meta m = logMeta(now);
        if (log_.empty()) {
            log_ = store_.adoptSlot(m, /*anyOrigin=*/true);
            if (!log_.empty()) {
                // ★ The day's log after a restart: its calls carry on from what it holds.
                std::string body;
                if (store_.readItem(log_, body)) {
                    unsigned long n = 0, d = 0, u = 0;
                    size_t p = 0;
                    while (p < body.size()) {
                        const size_t e = body.find('\n', p);
                        const std::string l = body.substr(p, e == std::string::npos ? std::string::npos : e - p);
                        if (l.size() > 10 && l[2] == ':' && l[5] == ':') {
                            ++n;
                            if (l.find("  DISTRESS") != std::string::npos) ++d;
                            else if (l.find("  URGENCY") != std::string::npos) ++u;
                        }
                        if (e == std::string::npos) break;
                        p = e + 1;
                    }
                    // the pending lines are already counted in calls_ — add the ones the file holds
                    calls_ += n; distress_ += d; urgency_ += u;
                    m = logMeta(now);
                }
            }
        }
        if (!log_.empty() && store_.append(log_, pending_)) {
            store_.retag(log_, m);
        } else {
            if (!log_.empty()) store_.close(log_);
            const std::string head = "DSC \xc2\xb7 " + freqText() + " \xc2\xb7 " + gallery::utcStamp(now, "%Y-%m-%d") + " (UTC)\n"
                + "Digital Selective Calling (ITU-R M.493), one line per call as decoded. Times are UTC.\n\n";
            log_ = store_.create(m, "txt", head + pending_);
            if (!log_.empty()) ++items_;
        }
        pending_.clear();
        lastFlush_ = now;
    }
    /** The position a call is ABOUT: its distress position, else a position reply's. */
    static const dsc::Position& positionOf(const dsc::Call& c) { return c.pos.known ? c.pos : c.posReply; }
    /** ★★ beta88 a routine or safety call that sent a position: its own pinned item, bounded (see the top). */
    void savePinned(const dsc::Call& c, const std::string& line, int64_t now) {
        const dsc::Position& p = positionOf(c);
        const std::string who = !c.distressId.empty() ? c.distressId : c.self;
        // the same ship, the same position, within kPinSameSec: one item
        for (const auto& r : recentPins_)
            if (r.who == who && std::fabs(r.lat - p.lat) < 1e-4 && std::fabs(r.lon - p.lon) < 1e-4 && now - r.t <= kPinSameSec) return;
        const int64_t hour = now / 3600;
        if (hour != pinHour_) { pinHour_ = hour; pinsThisHour_ = 0; pinsSkipped_ = 0; }
        if (pinsThisHour_ >= kPinPerHour) {
            if (++pinsSkipped_ == 1 || pinsSkipped_ % 100 == 0)
                pending_ += gallery::utcStamp(now, "%H:%M:%S") + "  (" + std::to_string(pinsSkipped_)
                    + " positioned calls this hour not pinned on the map: more than " + std::to_string(kPinPerHour) + " an hour)\n";
            return;
        }
        ++pinsThisHour_;
        recentPins_.push_back({ who, p.lat, p.lon, now });
        while (recentPins_.size() > 64 || (!recentPins_.empty() && now - recentPins_.front().t > kPinSameSec)) recentPins_.erase(recentPins_.begin());
        saveAlert(c, line, now);
    }
    void saveAlert(const dsc::Call& c, const std::string& line, int64_t now) {
        gallery::Meta m = meta_;
        m.utc = now;
        m.title = line;
        m.level = c.level();
        const dsc::Position& p = positionOf(c);
        if (p.known) { m.pinLat = p.lat; m.pinLon = p.lon; }
        const std::string who = !c.distressId.empty() ? c.distressId : c.self;
        const int lv = c.level();
        const char* word = lv >= 3 ? "DISTRESS" : lv == 2 ? "URGENCY" : lv == 1 ? "SAFETY" : "POSITION";
        m.dlName = std::string("DSC ") + word + " " + who + " " + gallery::utcStamp(now, "%Y-%m-%d %H%M") + ".txt";
        std::string body = std::string("DSC ") + (lv >= 2 ? word : lv == 1 ? "SAFETY CALL WITH A POSITION" : "CALL WITH A POSITION") + " \xc2\xb7 heard "
            + gallery::utcStamp(now, "%Y-%m-%d %H:%M:%S UTC") + " on " + freqText() + " ("
            + (meta_.origin == "background" ? "background decoding" : "a listener's decoder") + ")\n\n" + line + "\n";
        if (p.known) {
            char b[96]; std::snprintf(b, sizeof b, "\nPosition %s (%.4f, %.4f)\n", dsc::positionText(p).c_str(), p.lat, p.lon);
            body += b;
        }
        if (lv >= 2)
            body += "\nDSC is the GMDSS alerting system. A distress call heard here may already have been answered by a coast\n"
                    "station; this is a receiver's log, not an alerting service.\n";
        if (!store_.add(m, "txt", body).empty()) ++items_;
    }

    gallery::Store& store_;
    gallery::Meta meta_;
    std::function<double()> dial_;
    mutable std::mutex m_;
    bool done_ = false;
    std::string pending_, log_;
    int64_t lastFlush_ = 0, day_ = -1;
    unsigned long calls_ = 0, distress_ = 0, urgency_ = 0;
    size_t items_ = 0;
    double rfHz_ = 0;     // the DSC frequency the calls come from (from the call's JSON); 0 = the dial
    // ★ beta88 the positioned routine / safety items' bounds (see the top)
    static constexpr int kPinPerHour = 60;
    static constexpr int64_t kPinSameSec = 600;
    struct RecentPin { std::string who; double lat, lon; int64_t t; };
    std::vector<RecentPin> recentPins_;
    int64_t pinHour_ = -1;
    int pinsThisHour_ = 0;
    unsigned long pinsSkipped_ = 0;
};

} // namespace bg
} // namespace vibe
