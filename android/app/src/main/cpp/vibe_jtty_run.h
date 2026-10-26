// vibe_jtty_run.h — one JTTY receiver inside a decoder host (vibe_decoder_host.h, ext "jtty"): a listener's decoder on
// their audio. The engine is decoders/jtty_decoder.h — EVERY JTTY signal in the passband at once, no time slots; this is
// what turns its frames into what the clients are sent.
//
// Wire (on the decoder socket, beside RTTY's frames — see web/client/src/decoders.ts):
//   0x01 | u64 BE ts | u32 BE len | UTF-8     "14:32:07z  14.0915  CQ K1ABC CQ\n" — one line per finished message, as
//                                             RTTY's text frame (any client that shows decoder text shows these)
//   0x03 | u8 state                           0 listening · 3 signals being decoded
//   {"type":"jtty_signals","data":{"signals":[{id,hz,audio,snr,q,active,last,first,call,country,text,frames}]}}
//                                             every station heard, at most once a second and only when it changed
//   {"type":"jtty_spot","data":{callsign,frequency,audio,snr,how,country,timestamp}}
//                                             a call heard in a call atom: how "CQ" (calling CQ / TU … CQ) or "QSO"
//
// ★ Everything sent is built HERE from decoded frames — and every frame passed the CRC, the reserved bit, the whole
//   grammar and the coherence gate — but TEXT5 carries free punctuation off the air, so text is whitelisted to printable
//   ASCII and escaped before it goes anywhere near JSON (the RDS lesson: "sandbox everything").
#pragma once
#include "decoders/jtty_decoder.h"
#include "vibe_callsign_country.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class JttyRun {
public:
    struct Out {
        std::function<void(const std::string&)> text;       ///< 0x01: one line per finished message
        std::function<void(uint8_t)> state;                 ///< 0x03
        std::function<void(const std::string&)> json;       ///< a JSON text frame
        /** A spot, with its RF — for the gallery's summary, when it has one. */
        std::function<void(const jtty::Spot&, double rfHz)> spot;
    };
    /** `rfOfAudio`: a listener's audio Hz → RF Hz (their dial and mode); null = audio only. */
    JttyRun(std::function<double(double)> rfOfAudio, Out out) : rfOf_(std::move(rfOfAudio)), out_(std::move(out)) {
        jtty::Decoder::Config c;
        c.rate = 12000; c.loHz = 150; c.hiHz = 3300;
        rx_.reset(new jtty::Decoder(c));
        rx_->onMessage = [this](int id, double hz, const std::string& t, bool complete) { onMessage(id, hz, t, complete); };
        rx_->onSpot = [this](const jtty::Spot& s) { onSpot(s); };
    }

    /** ★ 48 kHz → 12 kHz by a box of four (the listener's SSB filter has already removed what would fold back — the
     *  same decimation FT8 and CW take). */
    void feedAudio(const float* x, int n) {
        if (!rx_ || n <= 0) return;
        buf_.clear(); buf_.reserve((size_t)n / 4 + 1);
        for (int i = 0; i < n; ++i) {
            acc_ += x[i];
            if (++dec_ >= 4) { buf_.push_back(acc_ * 0.25f); acc_ = 0; dec_ = 0; }
        }
        if (!buf_.empty()) rx_->feed(buf_.data(), (int)buf_.size());
        after();
    }
    uint64_t frames() const { return rx_ ? rx_->framesDecoded() : 0; }
    uint64_t candidates() const { return rx_ ? rx_->candidates() : 0; }
    unsigned long spots() const { return spots_; }

    /** For JSON: printable ASCII escaped, UTF-8 (the engine's "…") kept, control characters dropped; the LAST `cap` bytes
     *  of the raw text, cut on a character boundary BEFORE escaping, so a cut never splits an escape or a character. */
    static std::string clean(const std::string& in, size_t cap) {
        size_t from = in.size() > cap ? in.size() - cap : 0;
        while (from < in.size() && ((unsigned char)in[from] & 0xC0) == 0x80) ++from;
        std::string o;
        for (size_t i = from; i < in.size(); ++i) {
            const unsigned char u = (unsigned char)in[i];
            if (u == '"' || u == '\\') { o.push_back('\\'); o.push_back((char)u); }
            else if ((u >= 0x20 && u < 0x7f) || (u & 0x80)) o.push_back((char)u);
        }
        return o;
    }

private:
    double rf(double audio) const { return rfOf_ ? rfOf_(audio) : 0.0; }
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void onMessage(int, double hz, const std::string& t, bool complete) {
        if (!complete || !out_.text) return;
        const int64_t now = nowMs();
        const time_t tt = (time_t)(now / 1000);
        struct tm u{}; gmtime_r(&tt, &u);
        char head[64];
        const double r = rf(hz);
        if (r > 0) std::snprintf(head, sizeof head, "%02d:%02d:%02dz  %.4f  ", u.tm_hour, u.tm_min, u.tm_sec, r / 1e6);
        else std::snprintf(head, sizeof head, "%02d:%02d:%02dz  %4.0f Hz  ", u.tm_hour, u.tm_min, u.tm_sec, hz);
        std::string body;
        for (char ch : t) { const unsigned char c = (unsigned char)ch; if (c >= 0x20 || (c & 0x80)) body.push_back(ch); }
        out_.text(std::string(head) + body + "\n");
    }
    void onSpot(const jtty::Spot& s) {
        ++spots_;
        const double hz = rf(s.audioHz);
        if (out_.spot) out_.spot(s, hz);
        char b[512];
        const std::string call = clean(s.call, 16), how = clean(s.how, 8), ctry = clean(callsign::country(s.call), 40);
        const int n = std::snprintf(b, sizeof b,
            "{\"type\":\"jtty_spot\",\"data\":{\"callsign\":\"%s\",\"frequency\":%.0f,\"audio\":%.0f,\"snr\":%.0f,"
            "\"how\":\"%s\",\"country\":\"%s\",\"timestamp\":%lld}}",
            call.c_str(), hz, s.audioHz, s.snrDb, how.c_str(), ctry.c_str(), (long long)nowMs());
        if (n > 0 && out_.json) out_.json(std::string(b, (size_t)std::min(n, (int)sizeof b - 1)));
    }
    void after() {
        const int64_t now = nowMs();
        if (now - lastTick_ < 250) return;
        lastTick_ = now;
        const uint8_t st = rx_->activeSignals() > 0 ? 3 : 0;
        if (st != state_) { state_ = st; if (out_.state) out_.state(st); }
        if (now - lastListAt_ >= 1000) { lastListAt_ = now; sendList(); }
    }
    void sendList() {
        // ★ Lowest frequency first (the band's order, as the spectrum above); at most 60 rows, each its recent messages.
        const auto v = rx_->signals(true);
        // ★ The engine's clock is the stream's; a row's "last heard" goes out as wall-clock ms like every other list.
        const int64_t now = nowMs(), eng = rx_->nowMs();
        std::string j = "{\"type\":\"jtty_signals\",\"data\":{\"signals\":[";
        char b[320];
        int n = 0;
        for (auto& s : v) {
            if (n >= 60) break;
            if (n) j += ',';
            ++n;
            std::snprintf(b, sizeof b, "{\"id\":%d,\"hz\":%.0f,\"audio\":%.1f,\"snr\":%.0f,\"q\":\"%s\",\"active\":%s,\"last\":%lld,\"first\":%lld,\"frames\":%u,",
                          s.id, rf(s.audioHz), s.audioHz, s.snrDb, jtty::qualityWord(s.quality), s.active ? "true" : "false",
                          (long long)(now - (eng - s.lastMs)), (long long)(now - (eng - s.firstMs)), s.frames);
            j += b;
            j += "\"call\":\"" + clean(s.call, 16) + "\",\"country\":\"" + clean(s.call.empty() ? std::string() : callsign::country(s.call), 40)
               + "\",\"text\":\"" + clean(s.text, 400) + "\"}";
        }
        j += "]}}";
        if (j == lastList_) return;   // ★ only when something changed — an idle band sends nothing after its first list
        lastList_ = j;
        if (out_.json) out_.json(j);
    }

    std::function<double(double)> rfOf_;
    Out out_;
    std::unique_ptr<jtty::Decoder> rx_;
    std::vector<float> buf_;
    float acc_ = 0; int dec_ = 0;
    std::string lastList_;
    int64_t lastTick_ = 0, lastListAt_ = 0;
    uint8_t state_ = 255;
    unsigned long spots_ = 0;
};

} // namespace vibe
