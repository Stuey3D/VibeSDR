// vibe_rs41_service.h — V12 RS41 radiosondes: the SONDES whole-radio mode's bands, the service thread, the sonde table
// and the JSON the clients draw (docs/v12/RS41.md). The receiver is vibe_rs41_rx.h.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS (Network > Audio > Spectrum > Decoders): feed() copies and
//     returns; more than half a second queued loses the OLDEST block and counts it.
// ★★★ ONE ROW PER SONDE, by serial — never by frequency (two sondes can share a channel in turn; one sonde keeps its
//     serial whatever the channel). A row exists only once a frame proved it (Reed–Solomon + the status block's CRC +
//     a plausible serial). Bounded: 64 rows, 400 track points each.
// ★★★ NEVER A GUESSED PTU: the temperature appears only once all 51 calibration subframes are in; until then the row
//     says how many ("calibration 17 of 51"). Humidity and pressure are not decoded (RS41.md says why).
#pragma once
#include "vibe_rs41_rx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vibe_thread.h"

namespace vibers41 {

// ── The bands ───────────────────────────────────────────────────────────────────────────────────────────────────────
/** 400–406 MHz is wider than an RTL's capture, so three 2 MHz bands (the tune keys step them, ISM's pattern) and — on a
 *  radio already running ≥ 6.7 MS/s — the whole allocation at once. Each centre is 5 kHz off the 10 kHz raster sondes
 *  use, so the DC remnant sits between channels. A capture narrower than a band covers its middle (`Receiver::lo/hi`
 *  say what). */
struct Band {
    const char* id;
    const char* name;
    double centreHz, loHz, hiHz;
    double minRateHz;            ///< the capture this band needs to be offered
};
inline const std::vector<Band>& bands() {
    static const std::vector<Band> v = {
        { "lo",  "400\xe2\x80\x93" "402 MHz", 401.005e6, 400.0e6, 402.0e6, 0.9e6 },
        { "mid", "402\xe2\x80\x93" "404 MHz", 403.005e6, 402.0e6, 404.0e6, 0.9e6 },
        { "hi",  "404\xe2\x80\x93" "406 MHz", 405.005e6, 404.0e6, 406.0e6, 0.9e6 },
        { "all", "400\xe2\x80\x93" "406 MHz", 403.005e6, 400.0e6, 406.0e6, 6.7e6 },
    };
    return v;
}
inline const Band* bandById(const std::string& id) { for (const auto& b : bands()) if (id == b.id) return &b; return nullptr; }

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20 || c == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}

// ── The sonde table ─────────────────────────────────────────────────────────────────────────────────────────────────
struct TrackPt { float lat, lon, alt; double t; };
struct SondeRow {
    std::string serial;
    const char* kind = "RS41";      ///< ★ V12 the row's key is (kind, serial): a DFM and an M10 never share a row
    std::string model;
    int calN = -1, calOf = -1;      ///< non-RS41: the type's own calibration progress (calOf 0 = no temperature)
    double hz = 0, first = 0, last = 0;
    int frames = 0, frameNo = -1, rsFixed = 0;
    double lat = NAN, lon = NAN, alt = NAN, vE = NAN, vN = NAN, vU = NAN, posAt = -1, maxAlt = NAN;
    int sats = 0;
    double snr = 0, battery = NAN, gpsUnix = 0;
    bool encrypted = false;
    Calibration cal;
    double temp = NAN, tempAt = -1;
    std::vector<TrackPt> track;
};

class Tracker {
public:
    static constexpr size_t kMaxRows = 64, kMaxTrack = 400;
    void add(const Frame& f, double now) {
        std::lock_guard<std::mutex> lk(m_);
        ++frames_;
        SondeRow* r = nullptr;
        for (auto& x : rows_) if (x.serial == f.serial && !std::strcmp(x.kind, f.kind)) { r = &x; break; }
        if (!r) {
            if (rows_.size() >= kMaxRows) {
                auto old = std::min_element(rows_.begin(), rows_.end(), [](const SondeRow& a, const SondeRow& b) { return a.last < b.last; });
                rows_.erase(old);
            }
            rows_.emplace_back();
            r = &rows_.back();
            r->serial = f.serial; r->kind = f.kind; r->first = now;
        }
        r->last = now; ++r->frames; r->frameNo = f.frameNo; r->hz = f.hz; r->snr = f.snrDb; r->rsFixed = f.rsFixed;
        if (std::isfinite(f.batteryV)) r->battery = f.batteryV;
        r->encrypted = r->encrypted || f.encrypted;
        if (!f.model.empty()) r->model = f.model;
        if (f.calIndex >= 0) r->cal.add(f.calIndex, f.cal);
        if (f.timeOk) r->gpsUnix = gpsToUnix(f.gpsWeek, f.towMs);
        if (f.unixTime > 0) r->gpsUnix = f.unixTime;
        if (std::strcmp(f.kind, "RS41")) {
            // ★ another type: its decoder judged the calibration; the temperature only beside a complete one
            r->calN = f.calN; r->calOf = f.calOf;
            if (std::isfinite(f.tempC) && f.calOf > 0 && f.calN >= f.calOf) { r->temp = f.tempC; r->tempAt = now; }
        } else if (f.ptuOk) {
            const double t = temperature(r->cal, f.meas);
            if (std::isfinite(t)) { r->temp = t; r->tempAt = now; }
        }
        if (f.posOk) {
            r->lat = f.lat; r->lon = f.lon; r->alt = f.alt; r->vE = f.vE; r->vN = f.vN; r->vU = f.vU; r->sats = f.sats; r->posAt = now;
            if (!std::isfinite(r->maxAlt) || f.alt > r->maxAlt) r->maxAlt = f.alt;
            // ★ a track point every 10 s, or on a 30 m move / 50 m climb; halved (every other point) when full
            bool put = r->track.empty();
            if (!put) {
                const TrackPt& p = r->track.back();
                const double dn = (f.lat - p.lat) * 111320.0, de = (f.lon - p.lon) * 111320.0 * std::cos(f.lat * M_PI / 180);
                put = now - p.t >= 10 || std::hypot(dn, de) > 30 || std::fabs(f.alt - p.alt) > 50;
            }
            if (put) {
                if (r->track.size() >= kMaxTrack) {
                    std::vector<TrackPt> h;
                    for (size_t i = 0; i < r->track.size(); i += 2) h.push_back(r->track[i]);
                    r->track.swap(h);
                }
                r->track.push_back({ float(f.lat), float(f.lon), float(f.alt), now });
            }
        }
    }
    void clear() { std::lock_guard<std::mutex> lk(m_); rows_.clear(); frames_ = 0; }
    uint64_t frames() { std::lock_guard<std::mutex> lk(m_); return frames_; }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return rows_.size(); }
    /** ★★ V12 beta94 THE COMMUNITY MAP (vibe_community.h): the sondes with a position, heard within `maxAge` s, newest first,
     *  as compact rows — [serial, type, lat, lon, alt m, climb m/s, last heard (UTC s), max alt m, temp °C|null, [[lat,lon]…]] — the
     *  track thinned to ≤ `maxPts` points (first and last kept), 4 decimals. Comma-joined ELEMENTS, no brackets. */
    std::string communityRows(double now, double maxAge, size_t maxRows, size_t maxPts) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<const SondeRow*> v;
        for (const auto& r : rows_) if (now - r.last <= maxAge && std::isfinite(r.lat) && std::isfinite(r.lon)) v.push_back(&r);
        std::sort(v.begin(), v.end(), [](const SondeRow* a, const SondeRow* b) { return a->last > b->last; });
        std::string o;
        char b[320];
        auto f = [](double x, const char* fmt) { char t[32]; if (!std::isfinite(x)) return std::string("null"); std::snprintf(t, sizeof t, fmt, x); return std::string(t); };
        for (size_t i = 0; i < v.size() && i < maxRows; ++i) {
            const SondeRow* r = v[i];
            std::snprintf(b, sizeof b, "%s[\"%s\",\"%s\",%.4f,%.4f,%s,%s,%.0f,%s,%s,[", i ? "," : "", jsonEsc(r->serial).c_str(),
                          jsonEsc(r->model.empty() ? std::string(r->kind) : r->model).c_str(), r->lat, r->lon, f(r->alt, "%.0f").c_str(), f(r->vU, "%.1f").c_str(),
                          r->last, f(r->maxAlt, "%.0f").c_str(), f(r->temp, "%.1f").c_str());
            o += b;
            const size_t n = r->track.size(), step = n > maxPts && maxPts > 1 ? (n + maxPts - 2) / (maxPts - 1) : 1;
            bool first = true;
            for (size_t k = 0; k < n; k += step) {
                const size_t j = (k + step >= n) ? n - 1 : k;
                std::snprintf(b, sizeof b, "%s[%.4f,%.4f]", first ? "" : ",", r->track[j].lat, r->track[j].lon); o += b; first = false;
                if (j == n - 1) break;
            }
            o += "]]";
        }
        return o;
    }
    /** `"frames":N,"sondes":[…]` (no braces) — heard within `maxAge` s, the newest first; each track ≤ `maxPts` points. */
    std::string json(double now, double maxAge = 3 * 3600, size_t maxPts = 120) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<const SondeRow*> v;
        for (const auto& r : rows_) if (now - r.last <= maxAge) v.push_back(&r);
        std::sort(v.begin(), v.end(), [](const SondeRow* a, const SondeRow* b) { return a->last > b->last; });
        char b[512];
        std::snprintf(b, sizeof b, "\"frames\":%llu,\"sondes\":[", (unsigned long long)frames_);
        std::string o = b;
        bool first = true;
        for (const SondeRow* r : v) {
            o += first ? "{" : ",{"; first = false;
            const bool rs = !std::strcmp(r->kind, "RS41");
            const int calN = rs ? r->cal.count() : std::max(0, r->calN), calOf = rs ? kCalSubframes : std::max(0, r->calOf);
            std::snprintf(b, sizeof b, "\"serial\":\"%s\",\"kind\":\"%s\",\"hz\":%.0f,\"age\":%.1f,\"n\":%d,\"frame\":%d,\"snr\":%.1f,\"cal\":%d,\"calOf\":%d",
                          jsonEsc(r->serial).c_str(), r->kind, r->hz, now - r->last, r->frames, r->frameNo, r->snr, calN, calOf);
            o += b;
            if (!r->model.empty()) { o += ",\"model\":\""; o += jsonEsc(r->model); o += "\""; }
            const double tx = r->cal.txHz();
            if (tx > 0) { std::snprintf(b, sizeof b, ",\"txHz\":%.0f", tx); o += b; }
            if (r->encrypted) o += ",\"enc\":true";
            if (std::isfinite(r->battery)) { std::snprintf(b, sizeof b, ",\"batt\":%.1f", r->battery); o += b; }
            if (r->gpsUnix > 0) { std::snprintf(b, sizeof b, ",\"gpsTime\":%.0f", r->gpsUnix); o += b; }
            if (r->posAt >= 0) {
                const double spd = std::hypot(r->vE, r->vN);
                double hd = std::atan2(r->vE, r->vN) * 180 / M_PI; if (hd < 0) hd += 360;
                std::snprintf(b, sizeof b, ",\"lat\":%.5f,\"lon\":%.5f,\"alt\":%.0f,\"climb\":%.1f,\"speed\":%.1f,\"heading\":%.0f,\"sats\":%d,\"posAge\":%.1f,\"maxAlt\":%.0f",
                              r->lat, r->lon, r->alt, r->vU, spd, hd, r->sats, now - r->posAt, r->maxAlt);
                o += b;
            }
            if (r->tempAt >= 0 && (rs ? r->cal.complete() : (calOf > 0 && calN >= calOf))) { std::snprintf(b, sizeof b, ",\"temp\":%.1f", r->temp); o += b; }
            if (!r->track.empty()) {
                o += ",\"track\":[";
                const size_t n = r->track.size(), step = (n + maxPts - 1) / std::max<size_t>(1, maxPts);
                bool f2 = true;
                for (size_t i = 0; i < n; i += step) {
                    const size_t j = (i + step >= n) ? n - 1 : i;   // ★ the last point is always the newest
                    std::snprintf(b, sizeof b, "%s[%.5f,%.5f,%.0f]", f2 ? "" : ",", r->track[j].lat, r->track[j].lon, r->track[j].alt);
                    o += b; f2 = false;
                    if (j == n - 1) break;
                }
                o += "]";
            }
            o += "}";
        }
        o += "]";
        return o;
    }
private:
    std::mutex m_;
    std::vector<SondeRow> rows_;
    uint64_t frames_ = 0;
};

// ── The service ─────────────────────────────────────────────────────────────────────────────────────────────────────
class Service {
public:
    Service() = default;
    ~Service() { stop(); }
    void start(const Band& b, double hwHz, double rateHz, int maxChannels = 6) {
        stop();
        rate_ = rateHz;
        rx_.reset(new Receiver(rateHz, hwHz, b.loHz, b.hiHz, maxChannels));
        { std::lock_guard<std::mutex> lk(mtx_); queue_.clear(); queued_ = 0; dropped_ = 0; run_ = true; health_ = Health(); clipN_ = clipSeen_ = 0; winStart_ = 0; chans_.clear(); }
        lo_ = rx_->lo(); hi_ = rx_->hi();
        worker_ = std::thread([this] { vibeDecoderThread("vibe-rs41"); loop_(); });   // ★ decoder priority (vibe_thread.h)
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); run_ = false; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        rx_.reset();
    }
    bool running() { std::lock_guard<std::mutex> lk(mtx_); return run_; }
    /** From the DSP thread: copy and return. Over half a second queued (at most 2 M samples), the OLDEST block goes. */
    void feed(const cf32* iq, size_t n) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!run_) return;
            queue_.emplace_back(iq, iq + n);
            queued_ += n;
            const size_t cap = std::min<size_t>(size_t(rate_ * 0.5), 2u << 20);
            while (queued_ > cap && queue_.size() > 1) { queued_ -= queue_.front().size(); queue_.pop_front(); ++dropped_; }
        }
        cv_.notify_one();
    }
    uint64_t dropped() { std::lock_guard<std::mutex> lk(mtx_); return dropped_; }
    size_t queued() { std::lock_guard<std::mutex> lk(mtx_); return queued_ + (busy_ ? 1 : 0); }
    Tracker& tracker() { return tr_; }
    double coverLo() const { return lo_; }
    double coverHi() const { return hi_; }
    struct Health { double clipFrac = 0; uint64_t msgs = 0; bool valid = false; };
    Health health() { std::lock_guard<std::mutex> lk(mtx_); return health_; }
    /** The channels being demodulated now (the scan's candidates), for the list's "listening on" line. */
    std::vector<Receiver::ChanInfo> channels() { std::lock_guard<std::mutex> lk(mtx_); return chans_; }

    /** {"type":"sondes", band, bandName, hz, lo, hi, dropped, chans:[…], frames, sondes:[…]} */
    std::string report(const Band& b, double now) {
        char h[256];
        std::snprintf(h, sizeof h, "{\"type\":\"sondes\",\"band\":\"%s\",\"bandName\":\"%s\",\"hz\":%.0f,\"lo\":%.0f,\"hi\":%.0f,\"dropped\":%llu,\"chans\":[",
                      b.id, b.name, b.centreHz, lo_, hi_, (unsigned long long)dropped());
        std::string o = h;
        bool first = true;
        for (const auto& c : channels()) {
            std::snprintf(h, sizeof h, "%s{\"hz\":%.0f,\"db\":%.1f,\"n\":%d%s%s%s}", first ? "" : ",", c.hz, c.levelDb, c.frames,
                          c.kind ? ",\"kind\":\"" : "", c.kind ? c.kind : "", c.kind ? "\"" : "");
            o += h; first = false;
        }
        o += "],";
        o += tr_.json(now);
        o += "}";
        return o;
    }

private:
    static double now_() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    void loop_() {
        std::unique_lock<std::mutex> lk(mtx_);
        while (run_) {
            cv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !queue_.empty(); });
            while (run_ && !queue_.empty()) {
                std::vector<cf32> b = std::move(queue_.front());
                queue_.pop_front();
                queued_ -= b.size();
                busy_ = true;
                lk.unlock();
                size_t clip = 0, seen = 0;
                for (size_t i = 0; i < b.size(); i += 4, ++seen) if (std::fabs(b[i].real()) > 0.93f || std::fabs(b[i].imag()) > 0.93f) ++clip;
                const double t = now_();
                rx_->feed(b.data(), b.size(), [&](const Frame& f) { tr_.add(f, t); ++msgs_; });
                std::vector<Receiver::ChanInfo> ch = rx_->channels();
                lk.lock();
                chans_.swap(ch);
                busy_ = false;
                clipN_ += clip; clipSeen_ += seen;
                if (t - winStart_ >= 1.0) {
                    if (winStart_ > 0) { health_.clipFrac = clipSeen_ ? double(clipN_) / double(clipSeen_) : 0; health_.msgs = msgs_.load(); health_.valid = true; }
                    winStart_ = t; clipN_ = clipSeen_ = 0;
                }
            }
        }
    }
    Tracker tr_;
    std::unique_ptr<Receiver> rx_;
    double rate_ = 0, lo_ = 0, hi_ = 0;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::vector<cf32>> queue_;
    size_t queued_ = 0;
    uint64_t dropped_ = 0, clipN_ = 0, clipSeen_ = 0;
    double winStart_ = 0;
    Health health_;
    bool run_ = false, busy_ = false;
    std::atomic<uint64_t> msgs_{0};
    std::thread worker_;
    std::vector<Receiver::ChanInfo> chans_;
};

}  // namespace vibers41
