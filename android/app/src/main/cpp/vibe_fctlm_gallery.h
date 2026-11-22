// vibe_fctlm_gallery.h — V12 beta91 TELEMETRY CHARTS: FUNcube telemetry KEPT in the gallery, per pass and per day.
//
// ★★★ WHY (Stuart, 2026-11-22): "how will the funcube telemetry display? will it be saved as a text file to the gallery?"
//     → "that could be good to have" → "especially for those who background decode it every pass". The decoder
//     (vibe_fctlm.h) put ONE readable line a frame into an RTTY-style run — the battery's raw count only, a new file each
//     time the decoder attached. Nothing a chart could read, and nothing that built history across passes.
//
// ── What is written ────────────────────────────────────────────────────────────────────────────────────────────
//  • A PASS — one .csv item (kind "fctlm"), every channel the frame carries, a row a frame (one every 5 s):
//        # FUNcube telemetry · AO-73 · 145.9350 MHz USB
//        # Pass from 2026-11-22 14:02:16 UTC · AOS 14:01 · LOS 14:14 UTC
//        # [raw] = sent uncalibrated: the FUNcube-1 Downlink Data document publishes no calibration. Times are UTC.
//        utc,unix,frame,seq,mode,elevation [deg],rs fixed,Photo voltage 1 [raw],…,Reboot count,…
//        2026-11-22T14:02:16Z,1795442536,WO1,123456,Sunlight,23.4,0,512,…
//    A column's heading is the channel's NAME, then "[raw]" (no published calibration — never a guessed unit), "[unit]"
//    for a calibrated one, or nothing for a count / flag. Names carry no commas, so it opens as-is in a spreadsheet; the
//    charts (src/utils/tlmCharts.ts, both clients) read the same headings.
//    Titled "AO-73 telemetry · pass 14:02 UTC · 23 frames" (the clients find a satellite's passes by that prefix), and
//    downloaded as "AO-73 telemetry 2026-11-22 1402 UTC.csv".
//    ★ THE PASS: the satellite engine's AOS when the lock knows it (a SATELLITE pick, a background pass) — a new AOS is a
//      new pass; without one (a hand-tuned listener) a gap over kPassGapSec. A change of satellite or of the frame's
//      channel set starts a new file too, and so does kPassMaxBytes ("part 2") — none grows for ever.
//  • THE DAY — one .txt item per satellite per UTC day, a line a frame, whoever was decoding (background passes and a
//    listener's pick write into the SAME day's log — Store::adoptSlot with anyOrigin, as DSC's and ISM's):
//        14:02:16  AO-73  WO1 · seq 123456 · Sunlight · battery 1234 raw · el 23° · RS fixed 0
//    Titled "AO-73 telemetry log · 22 Nov · 180 frames"; a restart reads the day back and counts on.
//
// ★★ NEVER A WRITE PER FRAME (the RTTY rule, a Pi 2's SD card): both are held in RAM and appended once a minute or 8 KB,
//    when a pass ends, at a UTC midnight and when the decoder stops. A 12-minute AO-73 pass is ~150 frames ≈ 60 KB of
//    CSV and ~15 KB of log — inside the owner's gallery space, whose eviction (the oldest first) bounds the whole.
// ★ No position of the SERVER is written (the elevation is the bird's, as seen from it — the same the panel shows).
#pragma once
#include "vibe_gallery.h"
#include "vibe_decoder_host.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

namespace vibe {
namespace bg {
namespace fctlmlog {

inline constexpr int64_t  kPassGapSec   = 20 * 60;      ///< no AOS known: frames this far apart are another pass
inline constexpr uint64_t kPassMaxBytes = 256 * 1024;   ///< one pass's CSV — then "part 2"
inline constexpr size_t   kFlushBytes   = 8192;
inline constexpr int64_t  kFlushSec     = 60;

/** A channel's name as a CSV heading: printable, no commas or quotes, then "[raw]" / "[unit]" / nothing. */
inline std::string column(const vibefc::Value& v) {
    std::string n;
    for (unsigned char c : v.name) { if (n.size() >= 48) break; if (c >= 0x20 && c < 0x7F && c != ',' && c != '"' && c != '[' && c != ']') n += char(c); }
    if (n.empty()) n = v.key;
    if (v.raw) return n + " [raw]";
    if (!v.unit.empty()) {
        std::string u; for (unsigned char c : v.unit) if (c >= 0x20 && c < 0x7F && c != ',' && c != '"' && c != '[' && c != ']') u += char(c);
        if (!u.empty()) return n + " [" + u + "]";
    }
    return n;
}
/** The fixed columns, then one per value. */
inline std::string csvHeader(const vibefc::Decoded& d) {
    std::string h = "utc,unix,frame,seq,mode,elevation [deg],rs fixed";
    for (const auto& v : d.values) h += "," + column(v);
    return h + "\n";
}
inline std::string cell(const std::string& s) {   // a text cell: printable, no commas / quotes (frame names, modes)
    std::string o; for (unsigned char c : s) if (c >= 0x20 && c < 0x7F && c != ',' && c != '"') o += char(c);
    return o.substr(0, 24);
}
inline std::string csvRow(const vibefc::Decoded& d, const vibefc::TlmCtx& c, int64_t t) {
    std::string r = gallery::utcStamp(t, "%Y-%m-%dT%H:%M:%SZ") + "," + std::to_string((long long)t) + "," + cell(d.frameName) + ","
                  + (d.seq >= 0 ? std::to_string(d.seq) : std::string()) + "," + cell(d.mode) + ",";
    if (std::isfinite(c.elDeg)) { char b[16]; std::snprintf(b, sizeof b, "%.1f", c.elDeg); r += b; }
    r += "," + std::to_string(d.rsFixed);
    for (const auto& v : d.values) r += "," + vibefc::num(v.v);
    return r + "\n";
}
/** What the columns are: a frame whose channel set differs starts a new file (its header would not fit). */
inline std::string signature(const vibefc::Decoded& d) { std::string s; for (const auto& v : d.values) s += v.key + ";"; return s; }
/** "AO-73" → "AO-73"; anything else a file name or a slot would not like is dropped. */
inline std::string satToken(const std::string& sat) {
    std::string o; for (unsigned char c : sat) if (std::isalnum(c) || c == '-' || c == '_') o += char(c);
    return o.empty() ? "SAT" : o.substr(0, 24);
}
/** The day log's readable line (its time added by the writer). */
inline std::string dayLine(const vibefc::Decoded& d, const vibefc::TlmCtx& c, const std::string& sat) {
    double bat = 0; bool have = false;
    for (const auto& v : d.values) if (v.key == "eps.vbat") { bat = v.v; have = true; break; }
    std::string l = gallery::printable(sat, 24) + "  " + cell(d.frameName) + " \xc2\xb7 seq " + std::to_string(d.seq) + " \xc2\xb7 " + cell(d.mode);
    if (have) l += " \xc2\xb7 battery " + vibefc::num(bat) + " raw";
    if (std::isfinite(c.elDeg)) { char b[24]; std::snprintf(b, sizeof b, " \xc2\xb7 el %.0f\xc2\xb0", c.elDeg); l += b; }
    l += " \xc2\xb7 RS fixed " + std::to_string(d.rsFixed);
    if (!d.fitter.empty()) l += " \xc2\xb7 \"" + d.fitter.substr(0, 200) + "\"";
    return l;
}

} // namespace fctlmlog

class FcTlmGalleryTap : public DecoderTap {
public:
    FcTlmGalleryTap(gallery::Store& store, gallery::Meta meta, std::function<int64_t()> now = nullptr)
        : store_(store), meta_(std::move(meta)), now_(now ? std::move(now) : std::function<int64_t()>([] { return gallery::nowUtc(); })) {
        meta_.kind = "fctlm";
        lastFlush_ = now_();
    }
    ~FcTlmGalleryTap() override { finish(true); }

    void satTlm(const vibefc::Decoded& d, const vibefc::TlmCtx& c) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = c.unix > 0 ? (int64_t)c.unix : now_();
        const std::string sat = c.sat.empty() ? std::string("AO-73") : gallery::printable(c.sat, 24);
        const std::string sig = fctlmlog::signature(d);
        // ★ a new pass: another satellite, another AOS, another channel set, or (no AOS known) a long gap
        const bool aosNew = c.aos > 0 && (pass_.aos <= 0 || std::fabs(c.aos - pass_.aos) > 120);
        const bool gap = c.aos <= 0 && pass_.last > 0 && t - pass_.last > fctlmlog::kPassGapSec;
        if (pass_.frames == 0 || sat != pass_.sat || sig != pass_.sig || aosNew || gap) {
            endPass(t);
            pass_ = Pass{};
            pass_.sat = sat; pass_.sig = sig; pass_.aos = c.aos; pass_.los = c.los; pass_.start = t;
            pass_.header = fctlmlog::csvHeader(d);
        } else if ((pass_.name.empty() ? 0 : store_.sizeOf(pass_.name)) + pass_.rows.size() > fctlmlog::kPassMaxBytes) {
            flushPass(t);                       // ★ the part so far, then a new file for the rest of the pass
            if (!pass_.name.empty()) store_.close(pass_.name);
            pass_.name.clear(); ++pass_.part; pass_.partFrames = 0;
        }
        pass_.rows += fctlmlog::csvRow(d, c, t);
        ++pass_.frames; ++pass_.partFrames; pass_.last = t;
        if (std::isfinite(c.elDeg) && c.elDeg > pass_.maxEl) pass_.maxEl = c.elDeg;
        // the day's log
        rollDay(t, sat);
        day_.pending += gallery::utcStamp(t, "%H:%M:%S") + "  " + fctlmlog::dayLine(d, c, sat) + "\n";
        ++day_.newFrames;
        if (pass_.rows.size() + day_.pending.size() >= fctlmlog::kFlushBytes || t - lastFlush_ >= fctlmlog::kFlushSec) flushAll(t);
    }
    void tick() override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now_();
        if ((!pass_.rows.empty() || !day_.pending.empty()) && t - lastFlush_ >= fctlmlog::kFlushSec) flushAll(t);
    }
    void finish(bool) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        const int64_t t = pass_.last > 0 ? pass_.last : now_();
        endPass(t);
        flushDay(t);
        if (!day_.name.empty()) store_.close(day_.name);
    }
    size_t savedCount() const override { std::lock_guard<std::mutex> lk(m_); return saved_; }
    /** For the tests: the current pass's file ("" before its first flush) and the day log's. */
    std::string passName() const { std::lock_guard<std::mutex> lk(m_); return pass_.name; }
    std::string dayName() const { std::lock_guard<std::mutex> lk(m_); return day_.name; }

private:
    struct Pass {
        std::string sat, sig, header, rows, name;
        double aos = 0, los = 0, maxEl = -90;
        int64_t start = 0, last = 0;
        int frames = 0, partFrames = 0, part = 1;
    };
    struct Day { std::string sat, name, pending; int64_t day = -1; long frames = 0, newFrames = 0; };

    gallery::Meta passMeta() const {
        gallery::Meta m = meta_;
        m.utc = pass_.start;
        const std::string hm = gallery::utcStamp(pass_.start, "%H:%M");
        std::string t = pass_.sat + " telemetry \xc2\xb7 pass " + hm + " UTC";
        if (pass_.part > 1) t += " \xc2\xb7 part " + std::to_string(pass_.part);
        t += " \xc2\xb7 " + std::to_string(pass_.partFrames) + (pass_.partFrames == 1 ? " frame" : " frames");
        if (pass_.maxEl > -90) { char b[24]; std::snprintf(b, sizeof b, " \xc2\xb7 max el %.0f\xc2\xb0", pass_.maxEl); t += b; }
        m.title = t;
        m.dlName = gallery::downloadSafe(pass_.sat + " telemetry " + gallery::utcStamp(pass_.start, "%Y-%m-%d %H%M") + " UTC"
                                         + (pass_.part > 1 ? " part " + std::to_string(pass_.part) : std::string()) + ".csv");
        return m;
    }
    std::string passHead() const {
        std::string h = "# FUNcube telemetry \xc2\xb7 " + pass_.sat;
        if (meta_.freqHz > 0) h += " \xc2\xb7 " + gallery::freqText(meta_.freqHz) + (meta_.mode.empty() ? std::string() : " " + upperOf(meta_.mode));
        h += "\n# Pass from " + gallery::utcStamp(pass_.start, "%Y-%m-%d %H:%M:%S") + " UTC";
        if (pass_.aos > 0) h += " \xc2\xb7 AOS " + gallery::utcStamp((int64_t)pass_.aos, "%H:%M");
        if (pass_.los > 0) h += " \xc2\xb7 LOS " + gallery::utcStamp((int64_t)pass_.los, "%H:%M") + " UTC";
        if (pass_.part > 1) h += " \xc2\xb7 part " + std::to_string(pass_.part);
        h += "\n# [raw] = sent uncalibrated: the FUNcube-1 Downlink Data document publishes no calibration. Times are UTC.\n";
        return h + pass_.header;
    }
    static std::string upperOf(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

    void flushPass(int64_t) {
        if (pass_.rows.empty()) return;
        const gallery::Meta m = passMeta();
        if (!pass_.name.empty() && store_.append(pass_.name, pass_.rows)) {
            store_.retag(pass_.name, m);
        } else {
            if (!pass_.name.empty()) store_.close(pass_.name);
            pass_.name = store_.create(m, "csv", passHead() + pass_.rows);
            if (!pass_.name.empty()) ++saved_;
        }
        pass_.rows.clear();
    }
    void endPass(int64_t t) {
        flushPass(t);
        if (!pass_.name.empty()) store_.close(pass_.name);
        pass_.name.clear();
    }
    gallery::Meta dayMeta(int64_t now) const {
        gallery::Meta m = meta_;
        m.slot = "day-" + gallery::utcStamp(now, "%Y%m%d") + "-" + fctlmlog::satToken(day_.sat);
        m.utc = (now / 86400) * 86400;
        m.title = day_.sat + " telemetry log \xc2\xb7 " + gallery::utcStamp(now, "%d %b") + " \xc2\xb7 " + std::to_string(day_.frames)
                + (day_.frames == 1 ? " frame" : " frames");
        m.dlName = gallery::downloadSafe(day_.sat + " telemetry " + gallery::utcStamp(now, "%Y-%m-%d") + ".txt");
        return m;
    }
    void rollDay(int64_t t, const std::string& sat) {
        const int64_t d = t / 86400;
        if (d == day_.day && sat == day_.sat) return;
        flushDay(t - 1);
        if (!day_.name.empty()) store_.close(day_.name);
        day_ = Day{};
        day_.day = d; day_.sat = sat;
    }
    void flushDay(int64_t now) {
        if (day_.pending.empty()) return;
        const int64_t at = day_.day >= 0 ? std::max<int64_t>(day_.day * 86400, std::min<int64_t>(now, day_.day * 86400 + 86399)) : now;
        if (day_.name.empty()) {
            day_.name = store_.adoptSlot(dayMeta(at), /*anyOrigin=*/true);
            if (!day_.name.empty()) {
                // ★ the day's log after a restart: count the frames it holds and carry on
                std::string body;
                if (store_.readItem(day_.name, body)) {
                    long n = 0; size_t p = 0;
                    while (p < body.size()) {
                        const size_t e = body.find('\n', p);
                        if (e != std::string::npos && e - p > 10 && body[p + 2] == ':' && body[p + 5] == ':') ++n;
                        if (e == std::string::npos) break;
                        p = e + 1;
                    }
                    day_.frames += n;
                }
            }
        }
        day_.frames += day_.newFrames; day_.newFrames = 0;
        const gallery::Meta m = dayMeta(at);
        if (!day_.name.empty() && store_.append(day_.name, day_.pending)) {
            store_.retag(day_.name, m);
        } else {
            if (!day_.name.empty()) store_.close(day_.name);
            const std::string head = day_.sat + " \xc2\xb7 FUNcube telemetry \xc2\xb7 " + gallery::utcStamp(at, "%Y-%m-%d") + " (UTC)\n"
                + "One line a good frame, every pass this server decoded. Each pass's every channel is its own .csv item. "
                  "raw = sent uncalibrated (no published calibration).\n\n";
            day_.name = store_.create(m, "txt", head + day_.pending);
            if (!day_.name.empty()) ++saved_;
        }
        day_.pending.clear();
    }
    void flushAll(int64_t t) { flushPass(t); flushDay(t); lastFlush_ = t; }

    gallery::Store& store_;
    gallery::Meta meta_;
    std::function<int64_t()> now_;
    mutable std::mutex m_;
    bool done_ = false;
    Pass pass_;
    Day day_;
    int64_t lastFlush_ = 0;
    size_t saved_ = 0;
};

} // namespace bg
} // namespace vibe
