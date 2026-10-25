// vibe_cw_run.h — one CW run inside a decoder host (vibe_decoder_host.h): a LISTENER's CW decoder on their audio, or a
// SKIMMER on a complex IQ segment. Both are the same engine (decoders/cw_skimmer.h); this is what turns it into what the
// clients are sent — the text of one signal, the list of signals, and spots.
//
// ★★ What a listener sees (Stuart's "readable" rule): the PRIMARY signal's text in the decoder box, as RTTY's is; and a
//    list of every signal the engine is decoding — frequency, callsign when one has been spotted, speed, SNR, quality,
//    last heard and its recent text — the skimmer view. On a listener's audio the list is their passband (one carrier on
//    CW, a few on a wide USB filter); on a skimmer it is the whole segment.
//
// Wire (all on the decoder socket, beside RTTY's frames — see web/client/src/decoders.ts):
//   0x01 | u64 BE ts | u32 BE len | UTF-8     the primary signal's text (listener only), as RTTY's text frame
//   0x03 | u8 state                           0 nothing heard · 1 carriers, none decoding yet · 3 decoding
//   {"type":"cw_signals","data":{...}}        the list, at most once a second and only when it changed
//   {"type":"cw_spot","data":{...}}           a callsign heard identifying itself (twice) — as FT8's digital_spot
//
// ★ Everything sent is built HERE from what the engine decoded, and decoded text comes off the air: it is whitelisted to
//   printable ASCII before it goes anywhere near JSON (the RDS lesson: "sandbox everything").
#pragma once
#include "decoders/cw_skimmer.h"
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

class CwRun {
public:
    using cf32 = std::complex<float>;
    struct Out {
        std::function<void(const std::string&)> text;       ///< the primary signal's text (0x01)
        std::function<void(uint8_t)> state;                 ///< 0x03
        std::function<void(const std::string&)> json;       ///< a JSON text frame
        /** A spot, with its RF frequency — for the gallery's summary (DecoderTap::spot). */
        std::function<void(const cw::Spot&, double rfHz)> spot;
    };
    /** `skimmer`: IQ in (feedIq), else 48 kHz listener audio (feedAudio). `widthHz`: the skimmer's segment (clamped to
     *  what the IQ carries). `rfOfAudio`: a listener's audio Hz → RF Hz (their dial and mode); null = audio only. */
    CwRun(bool skimmer, double widthHz, std::function<double(double)> rfOfAudio, Out out)
        : skim_(skimmer), width_(widthHz), rfOf_(std::move(rfOfAudio)), out_(std::move(out)) {
        if (!skim_) {
            cw::Skimmer::Config c;
            c.rate = 12000; c.complexIn = false; c.loHz = 150; c.hiHz = 3600; c.maxSignals = 24;
            build(c);
        }
    }
    bool skimmer() const { return skim_; }

    /** ★ 48 kHz → 12 kHz by a box of four (CW audio is under 3 kHz, and the listener's own filter has already removed
     *  what would fold back — the same decimation FT8 takes). */
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
    /** A skimmer's IQ: `rate` Hz, centred on `centreRfHz`. A different rate rebuilds the engine (a radio rate change). */
    void feedIq(const cf32* x, int n, double rate, double centreRfHz) {
        if (!skim_ || n <= 0 || !(rate > 0)) return;
        if (!sk_ || std::fabs(rate - rate_) > 0.5) {
            cw::Skimmer::Config c;
            c.rate = rate; c.complexIn = true;
            const double half = std::min(width_, rate * 0.94) / 2.0;   // ★ the slice's outer 3 % is its filter's roll-off
            c.loHz = -half; c.hiHz = half;
            c.maxSignals = std::max(32, std::min(240, (int)(2.0 * half / 300.0)));   // ★ a signal every 300 Hz at most
            rate_ = rate;
            build(c);
        }
        centre_ = centreRfHz;
        sk_->feedIq(x, n);
        after();
    }
    /** Hz across the segment actually searched (the admin line). */
    double searchedHz() const { return sk_ ? sk_->binsSearched() * sk_->binHzActual() : 0.0; }
    int confirmed() const { return sk_ ? sk_->confirmed() : 0; }
    unsigned long spots() const { return spots_; }

private:
    void build(const cw::Skimmer::Config& c) {
        sk_.reset(new cw::Skimmer(c));
        sk_->onText = [this](int id, double off, const std::string& t) { onText(id, off, t); };
        sk_->onSpot = [this](const cw::Spot& s) { onSpot(s); };
        primary_ = 0; primaryOff_ = 0; lastList_.clear();
    }
    double rf(double off) const {
        if (skim_) return centre_ + off;
        return rfOf_ ? rfOf_(off) : 0.0;
    }
    static std::string clean(const std::string& in, size_t cap) {
        // ★ Printable ASCII only — the engine emits A–Z, 0–9, punctuation, spaces and <SK>-style prosigns.
        std::string o;
        for (char ch : in) {
            const unsigned char u = (unsigned char)ch;
            if (u >= 0x20 && u < 0x7f && u != '"' && u != '\\') o.push_back(ch);
        }
        if (o.size() > cap) o.erase(0, o.size() - cap);
        return o;
    }
    // ── the primary signal (a listener's decoder box) ──
    void onText(int id, double off, const std::string& t) {
        if (skim_) return;
        if (!primary_) { primary_ = id; primaryOff_ = off; }
        if (id != primary_) return;
        primaryOff_ = off;
        pend_ += t;
    }
    /** ★ Sticky: the box follows one signal; another takes it only when the first has gone, or is clearly (6 dB)
     *  stronger for two seconds — so two stations in a wide passband do not interleave mid-word. */
    void choosePrimary(const std::vector<cw::SignalView>& v) {
        const cw::SignalView* cur = nullptr; const cw::SignalView* best = nullptr;
        for (auto& s : v) {
            if (!s.active) continue;
            if (s.id == primary_) cur = &s;
            if (!best || s.snrDb > best->snrDb) best = &s;
        }
        const int64_t now = nowMs();
        if (!best) return;
        if (!cur) { switchTo(*best); return; }
        if (best->id != cur->id && best->snrDb > cur->snrDb + 6.0f) {
            if (!challenger_ || challenger_ != best->id) { challenger_ = best->id; challengeAt_ = now; }
            else if (now - challengeAt_ > 2000) switchTo(*best);
        } else challenger_ = 0;
    }
    void switchTo(const cw::SignalView& s) {
        if (primary_ == s.id) return;
        const bool had = primary_ != 0;
        primary_ = s.id; primaryOff_ = s.offsetHz; challenger_ = 0;
        // ★ A new signal starts on a new line, with the text it already decoded (it was decoding all along).
        pend_ += (had ? "\n" : "") + s.text;
    }
    void onSpot(const cw::Spot& s) {
        ++spots_;
        const double hz = rf(s.offsetHz);
        if (out_.spot) out_.spot(s, hz);
        char b[512];
        const std::string call = clean(s.call, 16), how = clean(s.how, 8), ctry = clean(callsign::country(s.call), 40);
        const int n = std::snprintf(b, sizeof b,
            "{\"type\":\"cw_spot\",\"data\":{\"callsign\":\"%s\",\"frequency\":%.0f,\"audio\":%.0f,\"wpm\":%.0f,\"snr\":%.0f,"
            "\"how\":\"%s\",\"country\":\"%s\",\"timestamp\":%lld}}",
            call.c_str(), hz, skim_ ? 0.0 : s.offsetHz, s.wpm, s.snrDb, how.c_str(), ctry.c_str(), (long long)s.utcMs);
        if (n > 0 && out_.json) out_.json(std::string(b, (size_t)std::min(n, (int)sizeof b - 1)));
    }
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void after() {
        const int64_t now = nowMs();
        if (now - lastTick_ < 250) { flushText(); return; }
        lastTick_ = now;
        const auto v = sk_->signals(true);
        if (!skim_) choosePrimary(v);
        flushText();
        // ★ State: 3 while anything is decoding, 1 while carriers are being looked at, 0 otherwise.
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
    void sendList(const std::vector<cw::SignalView>& v) {
        // ★ Active signals (strongest first), then the recently gone — at most 60 rows, each with its last 120 characters.
        std::string j = std::string("{\"type\":\"cw_signals\",\"data\":{\"skimmer\":") + (skim_ ? "true" : "false");
        char b[256];
        if (skim_) { std::snprintf(b, sizeof b, ",\"centre\":%.0f,\"width\":%.0f", centre_, searchedHz()); j += b; }
        std::snprintf(b, sizeof b, ",\"primary\":%d,\"signals\":[", skim_ ? 0 : primary_); j += b;
        int n = 0;
        for (auto& s : v) {
            if (n >= 60) break;
            if (n) j += ',';
            ++n;
            std::snprintf(b, sizeof b, "{\"id\":%d,\"hz\":%.0f,\"audio\":%.0f,\"wpm\":%.0f,\"snr\":%.0f,\"q\":\"%s\",\"active\":%s,\"last\":%lld,\"first\":%lld,",
                          s.id, rf(s.offsetHz), skim_ ? 0.0 : s.offsetHz, s.wpm, s.snrDb, cw::qualityWord(s.quality),
                          s.active ? "true" : "false", (long long)s.lastMs, (long long)s.firstMs);
            j += b;
            j += "\"call\":\"" + clean(s.call, 16) + "\",\"country\":\"" + clean(s.call.empty() ? std::string() : callsign::country(s.call), 40)
               + "\",\"text\":\"" + clean(s.text, 120) + "\"}";
        }
        j += "]}}";
        // ★ Only when something changed — an idle band sends nothing after its first empty list.
        if (j == lastList_) return;
        lastList_ = j;
        if (out_.json) out_.json(j);
    }

    bool skim_;
    double width_;
    std::function<double(double)> rfOf_;
    Out out_;
    std::unique_ptr<cw::Skimmer> sk_;
    std::vector<float> buf_;
    float acc_ = 0; int dec_ = 0;
    double rate_ = 0, centre_ = 0;
    int primary_ = 0, challenger_ = 0; double primaryOff_ = 0; int64_t challengeAt_ = 0;
    std::string pend_, lastList_;
    int64_t lastTick_ = 0, lastListAt_ = 0;
    uint8_t state_ = 255;
    unsigned long spots_ = 0;
};

} // namespace vibe
