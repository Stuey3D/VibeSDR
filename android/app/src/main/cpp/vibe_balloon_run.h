// vibe_balloon_run.h — V12 balloons inside a decoder host (vibe_decoder_host.h, ext "balloon"): a LISTENER's decoder on
// their USB audio, as RTTY is. The receiver is decoders/balloon_rx.h; this turns what it finds into what the clients are
// sent. docs/v12/BALLOONS.md.
//
// ★★ WHY A LISTENER'S DECODER, NOT A WHOLE-RADIO MODE (as SONDES is): a balloon payload is one narrow signal a person
//    tunes — RTTY a few hundred hertz wide, Horus 4-FSK about 1 kHz, both inside a USB passband — and payloads sit on
//    different frequencies across 434.0–434.8 MHz (and elsewhere), so each listener follows their own. Radiosondes are
//    the opposite: many on a 2 MHz band at once, found by scanning it, which needs the radio's whole capture.
//
// Wire (on the decoder socket):
//   0x01 | u64 BE ts | u32 BE len | UTF-8   one line per report: "10:00:05z  VIBE1 #2  52.20601, 0.11950  31050 m
//                                           · UKHAS 300 Bd · 8N1 · 600 Hz" — and a line when a picture completes
//   0x03 | u8 state                         0 listening · 1 a signal (tones found) · 3 decoding (a report in the last 30 s)
//   0x0B | u8 keyLen | key | u16 BE have | u16 BE expected (0 = not known yet) | u8 complete | u16 BE w | u16 BE h | JPEG
//                                           an SSDV picture so far — at most every 4 s while it grows, and at once
//                                           when it completes; the key is "CALL#image"
//   {"type":"balloon","data":{"now":UTC s,"state":0|1|3,
//        "rtty":{"centre":Hz,"shift":Hz,"settings":"300 Bd · 8N1 · 600 Hz"|"","candidates":n,"rejected":n},
//        "horus":{"tones":[Hz×4]|null,"baud":100,"packets":n},
//        "payloads":[{"call","src":"ukhas"|"horus","count","time":"hh:mm:ss"|"","lat","lon","alt","speed","sats","temp",
//                     "batt","heard":UTC s,"n":reports,"fec":bits,"sentence","extra":[…]}…],   (newest first, ≤ 12)
//        "images":[{"key","call","image","have","expected","complete","mcus","total"}…]}}
//                                           on every report and every 5 s
// ★ Bounded: ≤ 12 payloads kept (the oldest heard goes), the receiver's own bounds (balloon_rx.h), one JPEG built at a time.
#pragma once
#include "decoders/balloon_rx.h"
#include "vibe_horus_names.h"   // ★ V12 Horus payload IDs → callsigns (Project Horus's list, fetched by the server)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class BalloonRun {
public:
    struct Out {
        std::function<void(const std::string&)> text;                  ///< 0x01 line
        std::function<void(uint8_t)> state;                            ///< 0x03
        std::function<void(const std::string&)> json;                  ///< a JSON text frame
        std::function<void(const std::vector<uint8_t>&)> frame;        ///< a binary frame (0x0B pictures)
    };
    static constexpr size_t kMaxPayloads = 12;
    static constexpr double kImageEverySec = 4;

    BalloonRun(const balloon::Config& cfg, Out out) : out_(std::move(out)), rx_(new balloon::Receiver(cfg)) {
        rx_->onTelemetry = [this](const balloon::Telemetry& t) { report_(t); };
        rx_->onImagePacket = [this](balloon::ssdv::Image& im, const balloon::ssdv::Packet&) { picture_(im, false); };
    }
    /** 48 kHz mono float, ±1. */
    void feedAudio(const float* x, int n) {
        if (n <= 0) return;
        rx_->feed48k(x, n);
        const double now = nowS();
        const int st = rx_->state();
        if (st != state_) { state_ = st; if (out_.state) out_.state((uint8_t)st); }
        if (now - lastStatus_ >= 5) sendJson_(now);
        // a growing picture's next draw, when one is due
        for (auto& kv : rx_->pictures().images()) {
            auto it = pendingDraw_.find(kv.first);
            if (it != pendingDraw_.end() && it->second && now - lastDraw_[kv.first] >= kImageEverySec) picture_(*kv.second, true);
        }
    }
    static double nowS() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    const balloon::Receiver& receiver() const { return *rx_; }

    /** ★ Settings from the attach message (the client's AUTO or its manual pins). Unknown / out-of-range = AUTO. */
    static balloon::Config configFrom(double baud, double bits, double reverse, double shift, double horusBaud, double horusSpacing,
                                      bool rtty, bool horus) {
        balloon::Config c;
        c.rtty = rtty; c.horus = horus;
        if (baud >= 45 && baud <= 1200) c.rttyBaud = baud;
        if (bits == 7 || bits == 8) c.rttyBits = (int)bits;
        if (reverse == 0 || reverse == 1) c.rttyReverse = (int)reverse;
        if (shift >= 150 && shift <= 1200) c.rttyShift = shift;
        if (horusBaud >= 25 && horusBaud <= 300) c.horusBaud = horusBaud;
        if (horusSpacing >= 80 && horusSpacing <= 1500) c.horusSpacing = horusSpacing;
        return c;
    }

private:
    struct Payload { balloon::Telemetry t; double heard = 0; unsigned long n = 0; };
    static std::string esc(const std::string& s) {
        std::string o;
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
            else if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
        return o;
    }
    static std::string num(double v, const char* fmt = "%.5f") { if (!std::isfinite(v)) return "null"; char b[32]; std::snprintf(b, sizeof b, fmt, v); return b; }
    void report_(const balloon::Telemetry& tIn) {
        /* ★ A Horus payload by its callsign where Project Horus's list names it (vibe_horus_names.h — the server's own
         *  copy, refreshed at most once a day); "HORUS-<id>" otherwise, as the decoder wrote it. */
        balloon::Telemetry named;
        const balloon::Telemetry* tp = &tIn;
        if (tIn.source == "horus") {
            const std::string nm = vibehorus::Names::get().name(tIn.horusId);
            if (!nm.empty()) { named = tIn; named.callsign = nm; tp = &named; }
        }
        const balloon::Telemetry& t = *tp;
        const double now = nowS();
        Payload& p = payloads_[t.callsign];
        p.t = t; p.heard = now; ++p.n;
        if (payloads_.size() > kMaxPayloads) {
            auto old = payloads_.begin();
            for (auto j = payloads_.begin(); j != payloads_.end(); ++j) if (j->second.heard < old->second.heard) old = j;
            payloads_.erase(old);
        }
        if (out_.text) {
            const time_t tt = (time_t)now; struct tm u{}; gmtime_r(&tt, &u);
            char b[320];
            std::string pos = std::isfinite(t.lat) ? num(t.lat) + ", " + num(t.lon) : std::string("no fix");
            std::string alt = std::isfinite(t.alt) ? num(t.alt, "%.0f") + " m" : std::string("");
            std::string how = t.source == "horus" ? std::string("Horus v") + std::to_string(t.horusVersion) : "UKHAS " + rx_->rttySettings();
            std::snprintf(b, sizeof b, "%02d:%02d:%02dz  %s #%ld  %s  %s \xc2\xb7 %s\n", u.tm_hour, u.tm_min, u.tm_sec, t.callsign.c_str(), t.count,
                          pos.c_str(), alt.c_str(), how.c_str());
            out_.text(b);
        }
        sendJson_(now);
    }
    void picture_(balloon::ssdv::Image& im, bool dueDraw) {
        const std::string key = im.call + "#" + std::to_string(im.imageId);
        const double now = nowS();
        // ★ bounded: a flight sends picture after picture — forget the ones the receiver no longer holds
        if (lastDraw_.size() > 2 * balloon::ssdv::kMaxImages + 2) {
            const auto& live = rx_->pictures().images();
            for (auto* m : { &pendingDraw_, &doneSent_ }) for (auto it = m->begin(); it != m->end();) it = live.count(it->first) ? std::next(it) : m->erase(it);
            for (auto it = lastDraw_.begin(); it != lastDraw_.end();) it = live.count(it->first) ? std::next(it) : lastDraw_.erase(it);
        }
        const bool done = im.complete();
        if (!dueDraw && !done && now - lastDraw_[key] < kImageEverySec) { pendingDraw_[key] = true; return; }
        pendingDraw_[key] = false;
        if (done && doneSent_[key]) return;
        lastDraw_[key] = now;
        const std::vector<uint8_t> jpg = im.jpeg();
        std::vector<uint8_t> f;
        f.reserve(jpg.size() + 32);
        f.push_back(0x0B);
        const std::string k = key.substr(0, 40);
        f.push_back((uint8_t)k.size()); f.insert(f.end(), k.begin(), k.end());
        auto u16 = [&](int v) { f.push_back((uint8_t)(v >> 8)); f.push_back((uint8_t)v); };
        u16((int)im.packets.size()); u16(im.expected()); f.push_back(done ? 1 : 0); u16(im.w16 * 16); u16(im.h16 * 16);
        f.insert(f.end(), jpg.begin(), jpg.end());
        if (out_.frame) out_.frame(f);
        if (done) {
            doneSent_[key] = true;
            if (out_.text) {
                char b[160];
                std::snprintf(b, sizeof b, "SSDV  %s picture %d complete \xc2\xb7 %d\xc3\x97%d \xc2\xb7 %zu packets\n", im.call.c_str(), im.imageId,
                              im.w16 * 16, im.h16 * 16, im.packets.size());
                out_.text(b);
            }
        }
        sendJson_(now);
    }
    void sendJson_(double now) {
        lastStatus_ = now;
        if (!out_.json) return;
        std::string j = "{\"type\":\"balloon\",\"data\":{\"now\":" + num(now, "%.0f") + ",\"state\":" + std::to_string(rx_->state());
        j += ",\"rtty\":{\"centre\":" + num(rx_->rttyCentre(), "%.0f") + ",\"shift\":" + num(rx_->rttyShift(), "%.0f") + ",\"settings\":\"" + esc(rx_->rttySettings())
           + "\",\"candidates\":" + std::to_string(rx_->candidates()) + ",\"rejected\":" + std::to_string(rx_->crcFails()) + "}";
        double t4[4];
        j += ",\"horus\":{\"tones\":";
        if (rx_->horusTones(t4)) j += "[" + num(t4[0], "%.0f") + "," + num(t4[1], "%.0f") + "," + num(t4[2], "%.0f") + "," + num(t4[3], "%.0f") + "]";
        else j += "null";
        j += ",\"baud\":" + num(rx_->config().horusBaud, "%.0f") + ",\"packets\":" + std::to_string(rx_->horus().packets) + "}";
        // payloads, newest first
        std::vector<const std::pair<const std::string, Payload>*> v;
        for (auto& kv : payloads_) v.push_back(&kv);
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->second.heard > b->second.heard; });
        j += ",\"payloads\":[";
        for (size_t i = 0; i < v.size(); ++i) {
            const balloon::Telemetry& t = v[i]->second.t;
            char tm[16] = "";
            if (t.hh >= 0) std::snprintf(tm, sizeof tm, "%02d:%02d:%02d", t.hh, t.mm, t.ss);
            j += i ? "," : "";
            j += "{\"call\":\"" + esc(t.callsign) + "\",\"src\":\"" + t.source + "\",\"count\":" + std::to_string(t.count) + ",\"time\":\"" + tm + "\""
               + ",\"lat\":" + num(t.lat) + ",\"lon\":" + num(t.lon) + ",\"alt\":" + num(t.alt, "%.0f") + ",\"speed\":" + num(t.speedKmh, "%.0f")
               + ",\"sats\":" + (t.sats >= 0 ? std::to_string(t.sats) : std::string("null")) + ",\"temp\":" + num(t.tempC, "%.0f")
               + ",\"batt\":" + num(t.battV, "%.2f") + ",\"heard\":" + num(v[i]->second.heard, "%.0f") + ",\"n\":" + std::to_string(v[i]->second.n)
               + ",\"fec\":" + std::to_string(t.fecFixed) + ",\"sentence\":\"" + esc(t.sentence.substr(0, 200)) + "\",\"extra\":[";
            for (size_t e = 0; e < t.extra.size() && e < 16; ++e) j += (e ? ",\"" : "\"") + esc(t.extra[e].substr(0, 24)) + "\"";
            j += "]}";
        }
        j += "],\"images\":[";
        bool first = true;
        for (auto& kv : rx_->pictures().images()) {
            const auto& im = *kv.second;
            j += first ? "" : ","; first = false;
            j += "{\"key\":\"" + esc(kv.first) + "\",\"call\":\"" + esc(im.call) + "\",\"image\":" + std::to_string(im.imageId) + ",\"have\":" + std::to_string(im.packets.size())
               + ",\"expected\":" + std::to_string(im.expected()) + ",\"complete\":" + (im.complete() ? "true" : "false") + ",\"mcus\":" + std::to_string(im.mcusDecoded)
               + ",\"total\":" + std::to_string(im.mcuCount()) + "}";
        }
        j += "]}}";
        out_.json(j);
    }

    Out out_;
    std::unique_ptr<balloon::Receiver> rx_;
    std::map<std::string, Payload> payloads_;
    std::map<std::string, double> lastDraw_;
    std::map<std::string, bool> pendingDraw_, doneSent_;
    double lastStatus_ = 0;
    int state_ = -1;
};

} // namespace vibe
