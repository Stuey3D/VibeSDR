// vibe_dsc_run.h — one DSC receiver inside a decoder host (vibe_decoder_host.h, ext "dsc"): a listener's decoder on their
// audio, or a background VFO's. The engine is decoders/dsc_decoder.h; this is what turns its calls into what the clients
// and the gallery are sent.
//
// Wire (on the decoder socket, beside RTTY's frames — see web/client/src/decoders.ts):
//   0x01 | u64 BE ts | u32 BE len | UTF-8     "14:32:07  2187.5 kHz  DISTRESS · MMSI …\n" — one line per call, as RTTY's
//                                             text frame (any client that shows decoder text shows these)
//   0x03 | u8 state                           0 listening · 1 a call coming in
//   {"type":"dsc_call","data":{...}}          the call, field by field (dsc::toJson) + id, utc (ms), rf (Hz), repeat
//
// ★★ A TRANSMISSION'S REPEATS ARE ONE CALL. A distress alert is sent five times running on one frequency, a test call
//    twice if unanswered: the same symbols again within two minutes update the first call's "repeat" count (the same
//    id) instead of listing it five times. A different call — even from the same ship — is a new one.
// ★ Everything sent is built from the call's digits and our own tables (dsc::describe) — nothing free-form comes off the
//   air in DSC, but it is escaped for JSON all the same.
#pragma once
#include "decoders/dsc_decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class DscRun {
public:
    struct Out {
        std::function<void(const std::string&)> text;       ///< 0x01: the readable line
        std::function<void(uint8_t)> state;                 ///< 0x03
        std::function<void(const std::string&)> json;       ///< a JSON text frame
        /** The gallery's copy: the line, the JSON, the call, and whether it is a REPEAT of one already given. */
        std::function<void(const dsc::Call&, const std::string& line, const std::string& json, bool repeat)> call;
    };
    /** `vhf`: channel 70 on an FM demodulator's audio, else MF/HF on SSB audio with its tones around `centreHz`.
     *  `rfHz`: the RF frequency the call was heard on (the dial and mode decide it — the caller knows them). */
    DscRun(bool vhf, double centreHz, std::function<double()> rfHz, Out out) : vhf_(vhf), rfHz_(std::move(rfHz)), out_(std::move(out)) {
        dsc::RxConfig c; c.vhf = vhf; c.rate = 48000; c.centreHz = centreHz;
        rx_.reset(new dsc::Receiver(c));
        rx_->onCall = [this](const dsc::Call& k) { onCall(k); };
        rx_->onState = [this](int s) { if (out_.state) out_.state((uint8_t)s); };
    }
    bool vhf() const { return vhf_; }
    void feedAudio(const float* x, int n) { if (rx_ && n > 0) rx_->process(x, n); }
    const dsc::Receiver::Stats& stats() const { return rx_->stats(); }
    unsigned long callsSent() const { return sent_; }

private:
    struct Seen { std::vector<int> syms; int64_t at; uint32_t id; int repeat; };
    static int64_t nowMs() {
        return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void onCall(const dsc::Call& k) {
        const int64_t now = nowMs();
        while (!seen_.empty() && now - seen_.front().at > kRepeatMs) seen_.pop_front();
        Seen* same = nullptr;
        for (auto& s : seen_) if (s.syms == k.symbols) { same = &s; break; }
        const double rf = rfHz_ ? rfHz_() : 0.0;
        uint32_t id; int repeat;
        if (same) { same->at = now; id = same->id; repeat = ++same->repeat; }
        else { id = ++nextId_; repeat = 1; seen_.push_back({ k.symbols, now, id, 1 }); if (seen_.size() > 64) seen_.pop_front(); }
        char extra[160];
        std::snprintf(extra, sizeof extra, "\"id\":%u,\"utc\":%lld,\"rf\":%.0f,\"repeat\":%d", id, (long long)now, rf, repeat);
        const std::string j = dsc::toJson(k, extra);
        const std::string line = dsc::describe(k);
        if (out_.json) out_.json("{\"type\":\"dsc_call\",\"data\":" + j + "}");
        if (!same && out_.text) {
            const time_t t = (time_t)(now / 1000);
            struct tm u{}; gmtime_r(&t, &u);
            char head[64];
            std::snprintf(head, sizeof head, "%02d:%02d:%02dz  %s  ", u.tm_hour, u.tm_min, u.tm_sec, freqText(rf).c_str());
            out_.text(std::string(head) + line + "\n");
        }
        if (out_.call) out_.call(k, line, j, same != nullptr);
        ++sent_;
    }
public:
    /** "2187.5 kHz", "156.525 MHz" — a DSC frequency as a mariner writes it. */
    static std::string freqText(double hz) {
        char b[32];
        if (hz >= 30e6) std::snprintf(b, sizeof b, "%.3f MHz", hz / 1e6);
        else if (hz > 0) std::snprintf(b, sizeof b, "%.1f kHz", hz / 1e3);
        else b[0] = 0;
        return b;
    }
private:
    static constexpr int64_t kRepeatMs = 120000;
    bool vhf_;
    std::function<double()> rfHz_;
    Out out_;
    std::unique_ptr<dsc::Receiver> rx_;
    std::deque<Seen> seen_;
    uint32_t nextId_ = 0;
    unsigned long sent_ = 0;
};

} // namespace vibe
