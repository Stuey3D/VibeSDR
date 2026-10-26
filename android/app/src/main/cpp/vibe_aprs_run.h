// vibe_aprs_run.h — V12 APRS: one decoder run (a listener's, on their NFM/SSB audio, or a background VFO's) — the
// demodulator, the station list, the messages, and what is sent to the clients and the gallery. docs/v12/AIS-APRS.md.
//
// ★★ RUNS ON THE DECODER HOST'S OWN THREAD (vibe_decoder_host.h), fed 48 kHz audio; never on the audio thread.
// ★★ ONE PACKET, HEARD THREE TIMES (direct, then from two digipeaters) IS ONE PACKET: the APRS 1.0.1 duplicate rule —
//    the same source and the same information field within 30 s — counts it once; the paths it came by are kept for the
//    card ("via MB7UH*"), so a listener sees which digipeater carried it.
// ★ What goes out:
//     {"type":"aprs_packet","data":{t, from, kind, line, raw, lat?, lon?, key, sym, msg?}}  — each new packet
//     {"type":"aprs_list","data":{baud, frames, repaired, dupes, stations:[…], messages:[…]}} — every 2 s when changed,
//       and at once to a joiner (with each station's track; the clients add points from the packets after that).
#pragma once
#include "vibe_aprs.h"
#include "vibe_station_log.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vibeaprs {

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}
/** ★ Text off the air into JSON: invalid UTF-8 replaced byte by byte with '?', so one bad byte never breaks a client's
 *  JSON.parse (an APRS comment is whatever a TNC sent — Latin-1 is common). */
inline std::string utf8Clean(const std::string& s) {
    std::string o; size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + size_t(n) <= s.size();
        for (int k = 1; ok && k < n; ++k) ok = ((unsigned char)s[i + size_t(k)] >> 6) == 2;
        if (!ok) { o += '?'; ++i; continue; }
        o.append(s, i, size_t(n)); i += size_t(n);
    }
    return o;
}

struct Station {
    std::string key, from, name, kind;
    char symTable = '/', symCode = '/';
    double lat = NAN, lon = NAN, course = NAN, speedKn = NAN, altFt = NAN;
    std::string comment, status, micE, path, raw, line, via;
    Weather wx;
    bool killed = false, messaging = false;
    double first = 0, last = 0, posAt = -1;
    int count = 0;
    struct Pt { double t, lat, lon; };
    std::deque<Pt> trail;
};
struct Message { double t; std::string from, to, text, id, kind; };

class Run {
public:
    struct Opts {
        std::function<void(const std::string&)> json;          ///< a JSON text frame to every decoder socket
        std::function<void(const vibestation::Heard&)> heard;  ///< the gallery's copy (null = none)
        std::function<double()> rfHz;                          ///< the dial (for the gallery line), may be empty
        std::function<double()> clock;                         ///< seconds (tests); empty = steady clock
    };
    static constexpr size_t kMaxStations = 500, kTrailMax = 120, kMsgMax = 60;
    static constexpr double kKeepSec = 6 * 3600, kDupeSec = 30;

    Run(int baud, Opts o) : baud_(baud), o_(std::move(o)), demod_(48000, baud) {}
    int baud() const { return baud_; }

    /** 48 kHz audio from the decoder thread. */
    void feedAudio(const float* x, int n) {
        demod_.feed(x, n, [&](const Frame& f) { onFrame(f); });
        const double t = now();
        if (dirty_ && t - lastList_ >= 2.0) { lastList_ = t; dirty_ = false; if (o_.json) o_.json(listJson(false)); }
    }
    /** ★ For tests and the offline tool: one frame, as the demodulator hands it over. */
    void onFrame(const Frame& f) {
        Ax25 ax;
        if (!parseAx25(f.bytes, ax)) return;
        ++frames_; if (f.repaired) ++repaired_;
        const double t = now();
        // ★ the duplicate rule (source + information field within 30 s)
        const std::string dk = addrText(ax.src) + "|" + ax.info;
        for (auto it = dupes_.begin(); it != dupes_.end();) { if (t - it->second > kDupeSec) it = dupes_.erase(it); else ++it; }
        Packet p = parsePacket(ax);
        auto d = dupes_.find(dk);
        if (d != dupes_.end()) {
            ++dupes_n_;
            // a digipeated copy: remember the path it came by, nothing else
            auto st = stations_.find(keyOf(p));
            if (st != stations_.end() && !p.path.empty() && p.path.find('*') != std::string::npos) st->second.via = viaOf(p.path);
            return;
        }
        dupes_[dk] = t;
        accept(p, t);
    }
    /** The whole list (to a joiner, with tracks; every 2 s, without). */
    std::string listJson(bool withTrails) {
        const double t = now();
        prune(t);
        std::string o = "{\"type\":\"aprs_list\",\"data\":{\"baud\":" + std::to_string(baud_) + ",\"frames\":" + std::to_string(frames_)
                      + ",\"repaired\":" + std::to_string(repaired_) + ",\"dupes\":" + std::to_string(dupes_n_) + ",\"stations\":[";
        std::vector<const Station*> v;
        for (const auto& kv : stations_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Station* a, const Station* b) { return a->last > b->last; });
        bool first = true;
        for (const Station* s : v) { if (!first) o += ','; first = false; stationJson(*s, t, withTrails, o); }
        o += "],\"messages\":[";
        for (size_t i = 0; i < msgs_.size(); ++i) {
            const Message& m = msgs_[i];
            if (i) o += ',';
            o += "{\"age\":" + fmt(t - m.t, 0) + ",\"from\":\"" + jsonEsc(m.from) + "\",\"to\":\"" + jsonEsc(m.to) + "\",\"text\":\""
               + jsonEsc(utf8Clean(m.text)) + "\",\"kind\":\"" + m.kind + "\"" + (m.id.empty() ? "" : ",\"id\":\"" + jsonEsc(m.id) + "\"") + "}";
        }
        return o + "]}}";
    }
    size_t stationCount() const { return stations_.size(); }
    uint64_t frames() const { return frames_; }
    uint64_t repaired() const { return repaired_; }

private:
    double now() const {
        if (o_.clock) return o_.clock();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    }
    static std::string keyOf(const Packet& p) { return p.name.empty() ? p.from : "obj:" + p.name; }
    static std::string viaOf(const std::string& path) {
        // the last digipeater that marked the packet (the one with '*'); generic aliases (WIDEn-N) are not a station
        const size_t star = path.find('*');
        if (star == std::string::npos) return "";
        size_t b = path.rfind(',', star); b = b == std::string::npos ? 0 : b + 1;
        std::string v = path.substr(b, star - b);
        if (v.compare(0, 4, "WIDE") == 0 || v.compare(0, 5, "TRACE") == 0 || v.compare(0, 5, "RELAY") == 0) {
            // the station before the alias is the one that used it
            if (b >= 2) { size_t b2 = path.rfind(',', b - 2); b2 = b2 == std::string::npos ? 0 : b2 + 1; v = path.substr(b2, b - 1 - b2); }
            else v.clear();
        }
        return v;
    }

    void accept(const Packet& p, double t) {
        if (p.kind == "message" || p.kind == "bulletin" || p.kind == "ack") {
            msgs_.push_back({ t, p.from, p.to, p.text, p.msgId, p.kind });
            while (msgs_.size() > kMsgMax) msgs_.pop_front();
        }
        Station& s = stations_[keyOf(p)];
        if (s.count == 0) { s.key = keyOf(p); s.first = t; }
        ++s.count; s.last = t;
        s.from = p.from; s.name = p.name; s.raw = p.raw; s.path = p.path;
        if (!p.path.empty()) s.via = viaOf(p.path);
        if (p.kind != "message" && p.kind != "ack" && p.kind != "bulletin") s.kind = s.kind.empty() || p.hasPos || p.kind == "weather" ? p.kind : s.kind;
        else if (s.kind.empty()) s.kind = "station";
        if (p.hasPos) {
            s.symTable = p.symTable; s.symCode = p.symCode; s.lat = p.lat; s.lon = p.lon; s.posAt = t;
            s.course = p.course; s.speedKn = p.speedKn; s.messaging = p.messaging;
            if (p.altFt == p.altFt) s.altFt = p.altFt;
            if (!p.comment.empty()) s.comment = p.comment;
            if (!p.micE.empty()) s.micE = p.micE;
            bool add = s.trail.empty();
            if (!add) {
                const auto& q = s.trail.back();
                const double dy = (p.lat - q.lat) * 111320.0, dx = (p.lon - q.lon) * 111320.0 * std::cos(p.lat * M_PI / 180);
                add = dx * dx + dy * dy > 400;   // ★ 20 m: a parked tracker's GPS wander is not a track
            }
            if (add) { s.trail.push_back({ t, p.lat, p.lon }); while (s.trail.size() > kTrailMax) s.trail.pop_front(); }
        }
        if (p.wx.any()) s.wx = p.wx;
        if (!p.status.empty() && (p.kind == "status" || p.kind == "other")) s.status = p.status;
        s.killed = p.killed;
        s.line = readable(p);
        dirty_ = true;
        prune(t);
        if (o_.json) {
            std::string j = "{\"type\":\"aprs_packet\",\"data\":{\"age\":0,\"key\":\"" + jsonEsc(utf8Clean(s.key)) + "\",\"from\":\"" + jsonEsc(utf8Clean(p.from))
                          + "\",\"kind\":\"" + p.kind + "\",\"line\":\"" + jsonEsc(utf8Clean(s.line)) + "\",\"raw\":\"" + jsonEsc(utf8Clean(p.raw)) + "\"";
            if (p.hasPos) j += ",\"lat\":" + fmt(p.lat, 5) + ",\"lon\":" + fmt(p.lon, 5);
            if (!p.to.empty()) j += ",\"to\":\"" + jsonEsc(p.to) + "\",\"text\":\"" + jsonEsc(utf8Clean(p.text)) + "\"";
            o_.json(j + "}}");
        }
        if (o_.heard) {
            vibestation::Heard h;
            h.key = s.key; h.kind = p.kind; h.name = s.key; h.freqHz = o_.rfHz ? o_.rfHz() : 0;
            h.line = utf8Clean(s.line);
            if (p.hasPos) h.line += " \xc2\xb7 " + fmt(p.lat, 4) + " " + fmt(p.lon, 4);
            if (!s.via.empty()) h.line += " \xc2\xb7 via " + s.via;
            o_.heard(h);
        }
    }
    void prune(double t) {
        for (auto it = stations_.begin(); it != stations_.end();) { if (t - it->second.last > kKeepSec) it = stations_.erase(it); else ++it; }
        while (stations_.size() > kMaxStations) {
            auto old = stations_.begin();
            for (auto it = stations_.begin(); it != stations_.end(); ++it) if (it->second.last < old->second.last) old = it;
            stations_.erase(old);
        }
    }
    static void stationJson(const Station& s, double t, bool trails, std::string& o) {
        o += "{\"key\":\"" + jsonEsc(utf8Clean(s.key)) + "\",\"from\":\"" + jsonEsc(utf8Clean(s.from)) + "\",\"kind\":\"" + s.kind + "\"";
        if (!s.name.empty()) o += ",\"name\":\"" + jsonEsc(utf8Clean(s.name)) + "\"";
        const char st[3] = { s.symTable, s.symCode, 0 };
        o += ",\"sym\":\"" + jsonEsc(st) + "\"";
        const std::string sn = symbolName(s.symTable, s.symCode);
        if (!sn.empty()) o += ",\"symName\":\"" + jsonEsc(sn) + "\"";
        auto num = [&](const char* k, double x, int dp) { if (x == x) { o += ",\""; o += k; o += "\":" + fmt(x, dp); } };
        num("lat", s.lat, 5); num("lon", s.lon, 5); num("course", s.course, 0); num("speed", s.speedKn == s.speedKn ? s.speedKn * 1.852 : NAN, 1);
        num("alt", s.altFt == s.altFt ? s.altFt * 0.3048 : NAN, 0);
        auto str = [&](const char* k, const std::string& v) { if (!v.empty()) { o += ",\""; o += k; o += "\":\"" + jsonEsc(utf8Clean(v)) + "\""; } };
        str("comment", s.comment); str("status", s.status); str("micE", s.micE); str("path", s.path); str("via", s.via); str("raw", s.raw);
        str("line", s.line);
        if (s.wx.any()) {
            o += ",\"wx\":{";
            bool f = true;
            auto w = [&](const char* k, double v, int dp) { if (v == v) { if (!f) o += ','; f = false; o += "\""; o += k; o += "\":" + fmt(v, dp); } };
            w("tempC", s.wx.tempF == s.wx.tempF ? (s.wx.tempF - 32) * 5 / 9 : NAN, 1); w("hum", s.wx.hum, 0);
            w("windDir", s.wx.windDir, 0); w("windKmh", s.wx.windMph == s.wx.windMph ? s.wx.windMph * 1.609344 : NAN, 1);
            w("gustKmh", s.wx.gustMph == s.wx.gustMph ? s.wx.gustMph * 1.609344 : NAN, 1);
            w("rain1hMm", s.wx.rain1hIn == s.wx.rain1hIn ? s.wx.rain1hIn * 25.4 : NAN, 1);
            w("rainTodayMm", s.wx.rainMidIn == s.wx.rainMidIn ? s.wx.rainMidIn * 25.4 : NAN, 1);
            w("hPa", s.wx.baroMbar, 1);
            o += "}";
        }
        if (s.killed) o += ",\"killed\":1";
        if (s.messaging) o += ",\"msg\":1";
        o += ",\"seen\":" + fmt(t - s.last, 0) + ",\"age\":" + fmt(t - s.first, 0) + ",\"count\":" + std::to_string(s.count)
           + ",\"posAge\":" + fmt(s.posAt >= 0 ? t - s.posAt : -1, 0);
        if (trails && s.trail.size() > 1) {
            o += ",\"trail\":[";
            for (size_t i = 0; i < s.trail.size(); ++i) { if (i) o += ','; o += "[" + fmt(s.trail[i].lat, 5) + "," + fmt(s.trail[i].lon, 5) + "]"; }
            o += "]";
        }
        o += "}";
    }

    int baud_;
    Opts o_;
    AfskDemod demod_;
    std::map<std::string, Station> stations_;
    std::map<std::string, double> dupes_;
    std::deque<Message> msgs_;
    uint64_t frames_ = 0, repaired_ = 0, dupes_n_ = 0;
    bool dirty_ = false;
    double lastList_ = -10;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
};

} // namespace vibeaprs
