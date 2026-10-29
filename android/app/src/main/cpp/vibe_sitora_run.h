// vibe_sitora_run.h — one SITOR-A (AMTOR mode A, ARQ) listener inside a decoder host (vibe_decoder_host.h, ext
// "sitora"): a listener's decoder on their audio, or a background VFO's. The engine is decoders/sitor_a.h; this is what
// turns what it prints into what the clients and the gallery are sent.
//
// Wire (on the decoder socket, beside RTTY's frames — see web/client/src/decoders.ts):
//   0x01 | u64 BE ts | u32 BE len | UTF-8   the printed text. When the sending station changes, a new line starts
//                                           "[2 · QCXT] " (its station number, and its identity once heard), so any
//                                           client that shows decoder text — and the gallery — shows who sent what.
//                                           Protocol events (the call, the identities, the end) are lines in [ ] too.
//   0x03 | u8 state                         the link: 0 no signal · 1 searching · 2 phasing (the call) ·
//                                           3 identification · 4 traffic · 5 idle · 6 change-over · 7 ended
//   {"type":"sitora_status","data":{...}}   link, iss, master, ids, numbers, idChecked, blocks, repeats, lost,
//                                           combined, snr[2], offset[2], inverted — at most twice a second
// ★ Nothing free-form goes into the JSON: identities are the 20 identification letters and digits.
#pragma once
#include "decoders/sitor_a.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace vibe {

class SitorARun {
public:
    struct Out {
        std::function<void(const std::string&)> text;   ///< 0x01
        std::function<void(uint8_t)> state;             ///< 0x03
        std::function<void(const std::string&)> json;   ///< a JSON text frame
    };
    explicit SitorARun(double centreHz, Out out, double rate = 48000) : out_(std::move(out)) {
        rx_.reset(new sitora::Receiver(rate, centreHz));
        rx_->onChar = [this](int side, char32_t c) { onChar(side, c); };
        rx_->onEvent = [this](const std::string& e) { line("[" + e + "]"); };
        rx_->onStatus = [this](const sitora::Status& s) {
            if ((int)s.link != lastLink_) { lastLink_ = (int)s.link; if (out_.state) out_.state((uint8_t)s.link); statusDue_ = true; }
            dirty_ = true;
        };
    }
    void feedAudio(const float* x, int n) {
        if (!rx_ || n <= 0) return;
        rx_->process(x, n);
        flush();
        const int64_t now = nowMs();
        if (dirty_ && (statusDue_ || now - lastStatus_ >= 500)) { sendStatus(); lastStatus_ = now; dirty_ = statusDue_ = false; }
    }
    const sitora::Status& status() const { return rx_->status(); }
    static std::string statusJson(const sitora::Status& s) {
        char b[512];
        std::snprintf(b, sizeof b,
            "{\"link\":%d,\"linkName\":\"%s\",\"iss\":%d,\"master\":%d,\"heard\":[%s,%s],\"ids\":[\"%s\",\"%s\"],\"numbers\":[\"%s\",\"%s\"],"
            "\"idChecked\":%s,\"blocks\":%lu,\"repeats\":%lu,\"lost\":%lu,\"combined\":%lu,\"snr\":[%.1f,%.1f],\"offset\":[%.1f,%.1f],\"inverted\":%s}",
            (int)s.link, sitora::linkName(s.link), s.iss, s.master, s.heard[0] ? "true" : "false", s.heard[1] ? "true" : "false",
            clean(s.ids[0]).c_str(), clean(s.ids[1]).c_str(), clean(s.numbers[0]).c_str(), clean(s.numbers[1]).c_str(),
            s.idChecked ? "true" : "false", s.blocks, s.repeats, s.lost, s.combined, s.snrDb[0], s.snrDb[1], s.offsetHz[0], s.offsetHz[1],
            s.inverted ? "true" : "false");
        return b;
    }

private:
    static int64_t nowMs() {
        return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    /** Identities are letters and digits only — anything else is dropped (they go into JSON and text unescaped). */
    static std::string clean(const std::string& s) {
        std::string o; for (char c : s) if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) o += c; return o;
    }
    std::string sideLabel(int side) const {
        const auto& s = rx_->status();
        std::string l = "[" + std::to_string(side + 1);
        if (side >= 0 && side < 2 && !s.ids[side].empty()) l += " · " + clean(s.ids[side]);
        return l + "] ";
    }
    void onChar(int side, char32_t c) {
        if (side != side_) {
            if (!buf_.empty() && buf_.back() != '\n') buf_ += '\n';
            buf_ += sideLabel(side);
            side_ = side;
        }
        if (c < 0x80) buf_ += (char)c;
        else buf_ += '?';
        atLineStart_ = c == U'\n';
    }
    void line(const std::string& l) {
        if (!buf_.empty() && buf_.back() != '\n') buf_ += '\n';
        else if (buf_.empty() && !atLineStart_ && side_ >= 0) buf_ += '\n';
        buf_ += l + "\n";
        side_ = -1;           // the next character starts a fresh labelled line
        atLineStart_ = true;
    }
    void flush() { if (!buf_.empty() && out_.text) { out_.text(buf_); buf_.clear(); } }
    void sendStatus() { if (out_.json) out_.json("{\"type\":\"sitora_status\",\"data\":" + statusJson(rx_->status()) + "}"); }

    Out out_;
    std::unique_ptr<sitora::Receiver> rx_;
    std::string buf_;
    int side_ = -1, lastLink_ = -1;
    bool dirty_ = false, statusDue_ = false, atLineStart_ = true;
    int64_t lastStatus_ = 0;
};

} // namespace vibe
