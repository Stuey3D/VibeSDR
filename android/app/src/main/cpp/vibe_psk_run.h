// vibe_psk_run.h — one PSK run inside a decoder host (vibe_decoder_host.h): a LISTENER's PSK31/63 decoder on their
// audio, or a SKIMMER on a complex IQ segment. Both are the same engine (decoders/psk_skimmer.h); this turns it into what
// the clients are sent — the text of one signal, the list of signals, and spots. The shape is CW's (vibe_cw_run.h), so
// the clients draw both with one list.
//
// ★★ Which signal is ONE SIGNAL's: the one nearest the middle of the listener's passband ("centre", audio Hz, sent with
//    the attach — a PSK program's cursor). Sticky: another takes over only when it is nearer by 15 Hz for a second, or
//    the first has gone — so a retune moves the box to the signal now in the middle, and two signals never interleave.
//
// Wire (all on the decoder socket — see web/client/src/decoders.ts):
//   0x01 | u64 BE ts | u32 BE len | UTF-8     the primary signal's text (listener only), as RTTY's text frame
//   0x03 | u8 state                           0 nothing heard · 1 carriers, none decoding yet · 3 decoding
//   {"type":"psk_signals","data":{...}}       the list, at most once a second and only when it changed
//   {"type":"psk_spot","data":{...}}          a callsign heard identifying itself (twice) — as CW's cw_spot
//
// ★ Everything sent is built HERE from what the engine decoded, and decoded text comes off the air: it is whitelisted to
//   printable ASCII (and the primary's newlines) and JSON-escaped before it goes anywhere near a client.
#pragma once
#include "decoders/psk_skimmer.h"
#include "vibe_callsign_country.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class PskRun {
public:
    using cf32 = std::complex<float>;
    struct Out {
        std::function<void(const std::string&)> text;       ///< the primary signal's text (0x01)
        std::function<void(uint8_t)> state;                 ///< 0x03
        std::function<void(const std::string&)> json;       ///< a JSON text frame
        std::function<void(const psk::Spot&, double rfHz)> spot;
    };
    /** `skimmer`: IQ in (feedIq), else 48 kHz listener audio (feedAudio). `widthHz`: the skimmer's segment. `centreAudio`:
     *  the middle of the listener's passband (ONE SIGNAL's cursor). `rfOfAudio`: audio Hz → RF Hz; null = audio only. */
    PskRun(bool skimmer, double widthHz, double centreAudio, std::function<double(double)> rfOfAudio, Out out)
        : skim_(skimmer), width_(widthHz), cursor_(centreAudio), rfOf_(std::move(rfOfAudio)), out_(std::move(out)) {
        if (!skim_) {
            psk::Skimmer::Config c;
            c.rate = 12000; c.complexIn = false; c.loHz = 200; c.hiHz = 3600; c.maxSignals = 32;
            build(c);
        }
    }
    bool skimmer() const { return skim_; }

    /** ★ 48 kHz → 12 kHz by a box of four (PSK is under 3.5 kHz, and the listener's own filter has removed what would
     *  fold back — the decimation CW and FT8 take). */
    void feedAudio(const float* x, int n) {
        if (skim_ || !sk_) return;
        buf_.clear(); buf_.reserve((size_t)n / 4 + 1);
        for (int i = 0; i < n; ++i) {
            acc_ += x[i];
            if (++dec_ >= 4) { buf_.push_back(acc_ * 0.25f); acc_ = 0; dec_ = 0; }
        }
        if (!buf_.empty()) sk_->feedReal(buf_.data(), (int)buf_.size());
        after();
    }
    /** A skimmer's IQ: `rate` Hz, centred on `centreRfHz`. A different rate rebuilds the engine. */
    void feedIq(const cf32* x, int n, double rate, double centreRfHz) {
        if (!skim_ || n <= 0 || !(rate > 0)) return;
        if (!sk_ || std::fabs(rate - rate_) > 0.5) {
            psk::Skimmer::Config c;
            c.rate = rate; c.complexIn = true;
            const double half = std::min(width_, rate * 0.94) / 2.0;   // ★ the slice's outer 3 % is its filter's roll-off
            c.loHz = -half; c.hiHz = half;
            c.maxSignals = std::max(32, std::min(160, (int)(2.0 * half / 60.0)));   // ★ a PSK31 signal every 60 Hz at most
            rate_ = rate;
            build(c);
        }
        centre_ = centreRfHz;
        sk_->feedIq(x, n);
        after();
    }
    double searchedHz() const { return sk_ ? sk_->binsSearched() * sk_->binHz() : 0.0; }
    int confirmed() const { return sk_ ? sk_->confirmed() : 0; }
    unsigned long spots() const { return spots_; }

    /** ★ JSON string body: printable ASCII, " and \ escaped; `newlines` keeps \n as an escape, else it is a space. */
    static std::string jsonText(const std::string& in, size_t cap, bool newlines = false) {
        std::string o;
        const size_t from = in.size() > cap ? in.size() - cap : 0;
        for (size_t i = from; i < in.size(); ++i) {
            const unsigned char u = (unsigned char)in[i];
            if (u == '\n') { o += newlines ? "\\n" : " "; continue; }
            if (u < 0x20 || u >= 0x7f) continue;
            if (u == '"' || u == '\\') o.push_back('\\');
            o.push_back((char)u);
        }
        return o;
    }

private:
    void build(const psk::Skimmer::Config& c) {
        sk_.reset(new psk::Skimmer(c));
        sk_->onText = [this](int id, double off, const std::string& t) { onText(id, off, t); };
        sk_->onSpot = [this](const psk::Spot& s) { onSpot(s); };
        primary_ = 0; lastList_.clear();
    }
    double rf(double off) const {
        if (skim_) return centre_ + off;
        return rfOf_ ? rfOf_(off) : 0.0;
    }
    static std::string clean(const std::string& in, size_t cap) {
        std::string o;
        for (char ch : in) {
            const unsigned char u = (unsigned char)ch;
            if ((u >= 0x20 && u < 0x7f) || u == '\n') o.push_back(ch);
        }
        if (o.size() > cap) o.erase(0, o.size() - cap);
        return o;
    }
    void onText(int id, double /*off*/, const std::string& t) {
        if (skim_ || id != primary_) return;
        pend_ += t;
    }
    void choosePrimary(const std::vector<psk::SignalView>& v) {
        const psk::SignalView* cur = nullptr; const psk::SignalView* best = nullptr;
        for (auto& s : v) {
            if (!s.active) continue;
            if (s.id == primary_) cur = &s;
            if (!best || std::fabs(s.offsetHz - cursor_) < std::fabs(best->offsetHz - cursor_)) best = &s;
        }
        const int64_t now = sk_->nowMs();
        if (!best) return;
        if (!cur) { switchTo(*best); return; }
        if (best->id != cur->id && std::fabs(best->offsetHz - cursor_) + 15.0 < std::fabs(cur->offsetHz - cursor_)) {
            if (challenger_ != best->id) { challenger_ = best->id; challengeAt_ = now; }
            else if (now - challengeAt_ > 1000) switchTo(*best);
        } else challenger_ = 0;
    }
    void switchTo(const psk::SignalView& s) {
        if (primary_ == s.id) return;
        const bool had = primary_ != 0;
        primary_ = s.id; challenger_ = 0;
        // ★ a new signal starts on a new line, with the text it has already decoded (it was decoding all along)
        std::string t = s.text;
        if (t.size() > 300) t.erase(0, t.size() - 300);
        pend_ += (had ? "\n" : "") + t;
    }
    void onSpot(const psk::Spot& s) {
        ++spots_;
        const double hz = rf(s.offsetHz);
        if (out_.spot) out_.spot(s, hz);
        char b[512];
        const std::string call = jsonText(s.call, 16), how = jsonText(s.how, 8), mode = jsonText(s.mode, 8),
                          ctry = jsonText(callsign::country(s.call), 40);
        const int n = std::snprintf(b, sizeof b,
            "{\"type\":\"psk_spot\",\"data\":{\"callsign\":\"%s\",\"frequency\":%.0f,\"audio\":%.0f,\"mode\":\"%s\",\"snr\":%.0f,"
            "\"how\":\"%s\",\"country\":\"%s\",\"timestamp\":%lld}}",
            call.c_str(), hz, skim_ ? 0.0 : s.offsetHz, mode.c_str(), s.snrDb, how.c_str(), ctry.c_str(), (long long)s.utcMs);
        if (n > 0 && out_.json) out_.json(std::string(b, (size_t)std::min(n, (int)sizeof b - 1)));
    }
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    /** ★ Ticks on the STREAM's clock (samples / rate), not the wall's: the same live, and a test feeding faster than
     *  real time still sees a list a second and a primary chosen — not one list for its whole run. */
    void after() {
        const int64_t now = sk_->nowMs();
        if (now - lastTick_ < 250) { flushText(); return; }
        lastTick_ = now;
        const auto v = sk_->signals(true);
        if (!skim_) choosePrimary(v);
        flushText();
        const int conf = sk_->confirmed();
        const uint8_t st = conf > 0 ? 3 : sk_->tracked() > 0 ? 1 : 0;
        if (st != state_) { state_ = st; if (out_.state) out_.state(st); }
        if (now - lastListAt_ >= 1000) { lastListAt_ = now; sendList(v); }
    }
    void flushText() {
        if (pend_.empty()) return;
        if (out_.text) out_.text(clean(pend_, 4096));
        pend_.clear();
    }
    void sendList(const std::vector<psk::SignalView>& v) {
        // ★ Active signals, then the recently gone — at most 60 rows, each with its last 160 characters.
        std::string j = std::string("{\"type\":\"psk_signals\",\"data\":{\"skimmer\":") + (skim_ ? "true" : "false");
        char b[320];
        if (skim_) { std::snprintf(b, sizeof b, ",\"centre\":%.0f,\"width\":%.0f", centre_, searchedHz()); j += b; }
        std::snprintf(b, sizeof b, ",\"primary\":%d,\"signals\":[", skim_ ? 0 : primary_); j += b;
        int n = 0;
        for (auto& s : v) {
            if (n >= 60) break;
            if (n) j += ',';
            ++n;
            std::snprintf(b, sizeof b, "{\"id\":%d,\"hz\":%.0f,\"audio\":%.0f,\"baud\":%d,\"snr\":%.0f,\"imd\":%.0f,\"q\":\"%s\",\"active\":%s,"
                          "\"last\":%lld,\"first\":%lld,",
                          s.id, rf(s.offsetHz), skim_ ? 0.0 : s.offsetHz, s.baud, s.snrDb, s.imdDb, psk::qualityWord(s.quality),
                          s.active ? "true" : "false", (long long)s.lastMs, (long long)s.firstMs);
            j += b;
            j += "\"call\":\"" + jsonText(s.call, 16) + "\",\"country\":\""
               + jsonText(s.call.empty() ? std::string() : callsign::country(s.call), 40)
               + "\",\"text\":\"" + jsonText(s.text, 160) + "\"}";
        }
        j += "]}}";
        if (j == lastList_) return;
        lastList_ = j;
        if (out_.json) out_.json(j);
    }

    bool skim_;
    double width_, cursor_;
    std::function<double(double)> rfOf_;
    Out out_;
    std::unique_ptr<psk::Skimmer> sk_;
    std::vector<float> buf_;
    float acc_ = 0; int dec_ = 0;
    double rate_ = 0, centre_ = 0;
    int primary_ = 0, challenger_ = 0; int64_t challengeAt_ = 0;
    std::string pend_, lastList_;
    int64_t lastTick_ = 0, lastListAt_ = 0;
    uint8_t state_ = 255;
    unsigned long spots_ = 0;
};

} // namespace vibe
