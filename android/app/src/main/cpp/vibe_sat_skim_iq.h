// vibe_sat_skim_iq.h — ★★★ beta97 "KEEP THE CORRECTED IQ OF THIS PASS" (the satellite skimmer's option, off by default —
// docs/v12/SAT-SKIM.md): the finder's Doppler-TURNED stream (0 Hz = the span's satellite-frame centre, the pass's Doppler
// already out) written to the gallery as a 2-channel 16-bit WAV (I left, Q right) at the slice's rate — what anyone can
// re-skim later (WSJT-X off an SDR player, or VibeSDR's own tools) or share, without the Doppler to undo.
//
// ★★ BOUNDED: at most `maxBytes` (the host passes min(64 MB, a quarter of the gallery's size) — a 48 kS/s slice is
//    192 KB/s, so ~5.5 min of a JO-97 pass in 64 MB); past it nothing more is written and the log says so. Written in
//    ~1 s pieces (append), never per block; the header's sizes are patched in when it closes (Store::patch). A gallery
//    that keeps nothing, or the handheld's card (every write a temp file + rename), keeps no IQ — said in the log.
// ★ The level: one gain, chosen from the first second (RMS to ~ −24 dBFS) and HELD — a moving gain would be a modulation of
//   its own on a recording meant for decoding.
#pragma once
#include "vibe_gallery.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vibesatskim {

class IqKeep {
public:
    using cf32 = std::complex<float>;
    struct Opts {
        vibe::gallery::Meta meta;          ///< kind "satiq", origin, title … (the caller fills it)
        uint64_t maxBytes = 64ull << 20;
        std::function<void(const std::string&)> log;
    };
    explicit IqKeep(Opts o) : o_(std::move(o)) {}
    ~IqKeep() { close(); }

    /** The turned stream (the decode thread). */
    void feed(const cf32* x, int n, double rate) {
        if (dead_ || n <= 0 || !(rate > 0)) return;
        if (rate_ == 0) rate_ = rate;
        if (std::fabs(rate - rate_) > 0.5) { stop_("the slice's rate changed"); return; }
        if (gain_ <= 0) {   // ★ the first second decides the level, once
            for (int i = 0; i < n; ++i) { pow_ += std::norm(x[i]); ++powN_; }
            pre_.insert(pre_.end(), x, x + n);
            if ((double)powN_ < rate_) return;
            const double rms = std::sqrt(pow_ / std::max<uint64_t>(1, powN_) / 2.0);
            gain_ = rms > 0 ? (float)std::min(1e9, 2000.0 / rms) : 1.0f;
            std::vector<cf32> p; p.swap(pre_);
            put_(p.data(), (int)p.size());
            return;
        }
        put_(x, n);
    }
    void close() {
        if (closed_) return;
        closed_ = true;
        if (!pre_.empty() && !dead_) { if (gain_ <= 0) gain_ = 1.0f; std::vector<cf32> p; p.swap(pre_); put_(p.data(), (int)p.size()); }
        flush_();
        if (!name_.empty()) {
            std::string h(8, '\0');
            le32_(h, 0, (uint32_t)std::min<uint64_t>(0xFFFFFFFFull, 36 + data_));
            vibe::gallery::shared().patch(name_, 4, h.substr(0, 4));
            std::string d(4, '\0'); le32_(d, 0, (uint32_t)std::min<uint64_t>(0xFFFFFFFFull, data_));
            vibe::gallery::shared().patch(name_, 40, d);
            vibe::gallery::shared().close(name_);   // ★ no longer an open run: the size limit may evict it like any item
            if (o_.log) { char b[200]; std::snprintf(b, sizeof b, "SAT SKIM: corrected IQ kept \xe2\x80\x94 %s, %.1f MB, %.0f s", name_.c_str(), data_ / 1048576.0, rate_ > 0 ? data_ / 4.0 / rate_ : 0.0); o_.log(b); }
        }
    }
    const std::string& name() const { return name_; }
    uint64_t bytes() const { return data_; }
    bool stopped() const { return dead_; }

private:
    static void le32_(std::string& s, size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) s[at + (size_t)k] = (char)((v >> (8 * k)) & 0xff); }
    static void le16_(std::string& s, size_t at, uint16_t v) { s[at] = (char)(v & 0xff); s[at + 1] = (char)(v >> 8); }
    std::string header_() const {
        std::string h(44, '\0');
        std::memcpy(&h[0], "RIFF", 4); le32_(h, 4, 0xFFFFFFFFu);   // ★ "streaming" sizes until close() patches them
        std::memcpy(&h[8], "WAVEfmt ", 8); le32_(h, 16, 16); le16_(h, 20, 1); le16_(h, 22, 2);
        const uint32_t sr = (uint32_t)std::lround(rate_);
        le32_(h, 24, sr); le32_(h, 28, sr * 4); le16_(h, 32, 4); le16_(h, 34, 16);
        std::memcpy(&h[36], "data", 4); le32_(h, 40, 0xFFFFFFFFu);
        return h;
    }
    void stop_(const std::string& why) {
        if (dead_) return;
        dead_ = true;
        if (o_.log) o_.log("SAT SKIM: corrected IQ no longer kept \xe2\x80\x94 " + why);
    }
    void put_(const cf32* x, int n) {
        for (int i = 0; i < n; ++i) {
            if (data_ + buf_.size() + 4 > o_.maxBytes) { flush_(); stop_("it reached its size (" + std::to_string(o_.maxBytes >> 20) + " MB)"); return; }
            const float re = std::max(-32768.f, std::min(32767.f, x[i].real() * gain_));
            const float im = std::max(-32768.f, std::min(32767.f, x[i].imag() * gain_));
            const int16_t a = (int16_t)std::lrint(re), b = (int16_t)std::lrint(im);
            buf_.push_back((char)(a & 0xff)); buf_.push_back((char)((uint16_t)a >> 8));
            buf_.push_back((char)(b & 0xff)); buf_.push_back((char)((uint16_t)b >> 8));
        }
        if ((double)buf_.size() >= rate_ * 4.0) flush_();   // ★ about a second a write
    }
    void flush_() {
        if (buf_.empty() || dead_) { buf_.clear(); return; }
        if (name_.empty()) {
            if (vibe::gallery::cardLayout().on) { stop_("not on the handheld's card"); buf_.clear(); return; }
            name_ = vibe::gallery::shared().create(o_.meta, "wav", header_() + buf_);
            if (name_.empty()) { stop_("the gallery keeps nothing (Setup \xe2\x86\x92 Server: its size)"); buf_.clear(); return; }
        } else if (!vibe::gallery::shared().append(name_, buf_)) { stop_("the gallery item went"); buf_.clear(); return; }
        data_ += buf_.size();
        buf_.clear();
    }
    Opts o_;
    double rate_ = 0;
    float gain_ = 0;
    double pow_ = 0; uint64_t powN_ = 0;
    std::vector<cf32> pre_;
    std::string buf_, name_;
    uint64_t data_ = 0;
    bool dead_ = false, closed_ = false;
};

} // namespace vibesatskim
