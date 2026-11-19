// vibe_lrpt_run.h — METEOR-M LRPT as a listener's decoder (V12, 2026-11-19): the DecoderHost's "lrpt" extension.
//
// ★★★ WHERE IT SITS. A listener tunes the satellite's downlink (137.100 / 137.900 MHz — per satellite, sats.json) and
//     starts LRPT; the host asks the DSP thread for an IQ SLICE of kSliceWidthHz centred on the dial (the CW / RTTY
//     skimmers' path, vibe_band_slice.h) and hands each block to this on its own decode thread (decoder priority:
//     Network > Audio > Spectrum > Decoders). Nothing here touches the radio.
//
// ★★★ THE DOPPLER CONTRACT with the satellite engine (branch v12-sat, docs/v12/SATELLITE-ENGINE.md): EITHER the engine
//     moves the listener's dial with the pass (the slice follows the dial, nothing to do here), OR it leaves the dial on
//     the published frequency and supplies Env::satDopplerHz — the shift to remove now, Hz, positive = received above
//     nominal. This reads it once per IQ block (≈ 20 times a second) and feeds it forward to the carrier loop; the loop
//     tracks the remainder. With neither, the receiver's own x⁴ acquisition and Costas loop follow ±3.1 kHz / 65 Hz/s
//     unaided (test-lrpt: −2.8 kHz with a +65 Hz/s slope decodes every block) — the engine only makes it faster.
//
// ★★ WHAT CLIENTS SEE: the SSTV picture frames (vibe_decoder_host.h: 0x07 start w h · 0x01 line y w rgb · 0x05 complete),
//    a QUARTER-SIZE colour preview (392 px wide; the composite when channels 1 and 2 are live, else the first live channel
//    as grey) growing a strip at a time, plus {"type":"lrpt_status","data":{…}} once a second. WHAT THE GALLERY KEEPS: the
//    full-size PNGs — every live channel and the composite — when the pass ends (30 s without a frame after pictures)
//    or the decoder stops (then marked partial).
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vibe_lrpt.h"

namespace vibelrpt {

class Run {
public:
    static constexpr double kSliceWidthHz = 140000;    ///< flat width asked for: 72k × 1.6 = 115 kHz occupied, + Doppler
    static constexpr double kPassEndSec = 30;          ///< no frame this long after pictures = the pass is over
    static constexpr int kPreviewW = kLineWidth / 4;   ///< 392
    static constexpr int kPreviewRowsPerStrip = 2;
    static constexpr int kPreviewMaxH = kMaxStrips * kPreviewRowsPerStrip;   ///< 1600

    struct Opts {
        double symbolRate = 72000;
        ReceiverConfig::Mod mod = ReceiverConfig::Auto;
        ReceiverConfig::Diff diff = ReceiverConfig::DiffAuto;
        bool interleaved = false;
        bool rateAuto = false;                                           ///< no satellite entry named the rate: 72k and 80k tried
        std::function<void(const std::string&)> json;                    ///< text frames (lrpt_status)
        std::function<void(const uint8_t*, size_t)> frame;               ///< binary picture frames
        std::function<double()> dopplerHz;                               ///< the satellite engine's feed-forward; null = none
        /** The pass's pictures (full size) — the gallery's copy. `partial` = cut off by a stop, not the pass ending. */
        std::function<void(const std::vector<Picture>&, bool partial)> pictures;
        std::function<void(const std::string&)> log;
    };
    explicit Run(Opts o) : o_(std::move(o)) {}
    ~Run() { finish(true); }

    /** One IQ block from the host's queue (decode thread). */
    void feedIq(const std::complex<float>* iq, int n, double rate, double /*centreRfHz*/) {
        if (n <= 0 || !(rate > 0)) return;
        if (!rx_ || std::fabs(rate - rate_) > 0.5) {
            const double sMax = o_.rateAuto ? 80000 : o_.symbolRate, sMin = o_.rateAuto ? 72000 : o_.symbolRate;
            if (rate < sMax * 1.8 || rate > sMin * 8.5) {
                if (!rateWarned_) { rateWarned_ = true; status("the IQ slice runs at " + std::to_string(int(rate)) + " S/s \xe2\x80\x94 LRPT needs 1.8 to 8 times the symbol rate"); }
                return;
            }
            if (rx_) { savePass(true); }
            rate_ = rate; newReceiver();
        }
        if (o_.dopplerHz) rx_->setDopplerHz(o_.dopplerHz());
        rx_->feedIq(reinterpret_cast<const float*>(iq), size_t(n));
        fedSec_ += n / rate;
        // ── progressive preview: every new strip that has landed ──
        if (rx_->version() != seenVersion_) { seenVersion_ = rx_->version(); lastFrameSec_ = fedSec_; }
        // ★ at most once a second: the version moves with every packet, and each preview resends its last strips
        if (seenVersion_ != previewVersion_ && fedSec_ - lastPreview_ >= 1.0) { previewVersion_ = seenVersion_; lastPreview_ = fedSec_; preview(); }
        if (fedSec_ - lastStatus_ >= 1.0) { lastStatus_ = fedSec_; status(""); }
        if (havePictures_ && fedSec_ - lastFrameSec_ > kPassEndSec) {
            if (o_.log) o_.log("lrpt: pass over (no frame for 30 s) \xe2\x80\x94 pictures to the gallery");
            savePass(false);
            newReceiver();
        }
    }
    /** The decoder stops (a replace, the listener leaving, a shutdown): keep what the pass got, marked partial. */
    void finish(bool interrupted) {
        if (done_) return;
        done_ = true;
        if (rx_) savePass(interrupted);
    }
    const Receiver* receiver() const { return rx_.get(); }

private:
    void newReceiver() {
        ReceiverConfig c; c.sampleRate = rate_; c.symbolRate = o_.symbolRate; c.mod = o_.mod; c.diff = o_.diff; c.interleaved = o_.interleaved; c.rateAuto = o_.rateAuto;
        rx_ = std::make_unique<Receiver>(c);
        seenVersion_ = 0; previewVersion_ = 0; havePictures_ = false; sentRows_ = 0; previewOpen_ = false; lastFrameSec_ = fedSec_;
        if (o_.log) o_.log("lrpt: receiver at " + std::to_string(int(rate_)) + " S/s, "
                           + (o_.rateAuto ? std::string("72k / 80k AUTO") : std::to_string(int(o_.symbolRate)) + " sym/s" + (o_.interleaved ? " (80k interleaved)" : "")));
    }
    void savePass(bool partial) {
        if (!rx_ || !havePictures_) return;
        preview(true);
        if (sentRows_ > 0 && o_.frame) { std::vector<uint8_t> m{ 0x05, 0, 0, 0, 0 }; o_.frame(m.data(), m.size()); }
        auto pics = renderPictures(*rx_);
        if (!pics.empty() && o_.pictures) o_.pictures(pics, partial);
        havePictures_ = false;
    }
    static void put32(std::vector<uint8_t>& m, uint32_t v) { m.push_back(uint8_t(v >> 24)); m.push_back(uint8_t(v >> 16)); m.push_back(uint8_t(v >> 8)); m.push_back(uint8_t(v)); }
    /** ★★ THE PREVIEW: a QUARTER-SIZE picture (392 px, two rows a strip), each row sent ONCE, in order, a strip only once
     *  the next has begun (by then its period's packets — every channel's — are in). Never a row re-sent: both clients
     *  read a line number going back as a NEW picture (the WEFAX rule), and a re-send every second was 28 kB/s anyway.
     *  Quarter size because the app pre-sizes an SSTV canvas from 0x07 (392 × 1600 RGBA = 2.5 MB on a 1 GB phone,
     *  not 10 MB at half size); the full pictures are the gallery's. `flush`: the pass is over — the last strip too. */
    void preview(bool flush = false) {
        int s0, s1; rx_->stripRange(s0, s1);
        if (s1 <= s0) return;
        const auto live = rx_->liveApids();
        if (live.empty()) return;
        havePictures_ = true;
        if (!o_.frame) return;
        if (!previewOpen_) {
            previewOpen_ = true; previewS0_ = s0;
            std::vector<uint8_t> m{ 0x07 }; put32(m, kPreviewW); put32(m, kPreviewMaxH);
            o_.frame(m.data(), m.size());
            const std::string nm = "METEOR LRPT";
            std::vector<uint8_t> n{ 0x02, uint8_t(nm.size() >> 8), uint8_t(nm.size()) }; n.insert(n.end(), nm.begin(), nm.end());
            o_.frame(n.data(), n.size());
        }
        const bool rgb = std::find(live.begin(), live.end(), 64) != live.end() && std::find(live.begin(), live.end(), 65) != live.end();
        const Channel* a = &rx_->channels().at(rgb ? 65 : live.front());
        const Channel* b = rgb ? &rx_->channels().at(64) : a;
        int lo1, hi1, lo2, hi2; a->levels(lo1, hi1); b->levels(lo2, hi2);
        const bool inv = !rgb && live.front() >= 67;
        const int upto = flush ? s1 : s1 - 1;
        std::vector<uint8_t> line(size_t(kPreviewW) * 3);
        auto lv = [](int v, int lo, int hi) { const int o = (v - lo) * 255 / std::max(1, hi - lo); return uint8_t(o < 0 ? 0 : o > 255 ? 255 : o); };
        for (int y = sentRows_; ; y++) {
            const int s = previewS0_ + y / kPreviewRowsPerStrip, r = (y % kPreviewRowsPerStrip) * (8 / kPreviewRowsPerStrip);
            if (s >= upto || y >= kPreviewMaxH) break;
            for (int x = 0; x < kPreviewW; x++) {
                const int px = x * (kLineWidth / kPreviewW), col = px / 8;
                if (!a->has(s, col) || !b->has(s, col)) { line[size_t(x) * 3] = line[size_t(x) * 3 + 1] = line[size_t(x) * 3 + 2] = 0; continue; }
                const size_t pi = (size_t(s) * 8 + size_t(r)) * kLineWidth + size_t(px);
                uint8_t R = lv(a->pixels()[pi], lo1, hi1), B = lv(b->pixels()[pi], lo2, hi2);
                if (inv) R = B = uint8_t(255 - R);
                line[size_t(x) * 3] = R; line[size_t(x) * 3 + 1] = R; line[size_t(x) * 3 + 2] = rgb ? B : R;
            }
            std::vector<uint8_t> m; m.reserve(9 + line.size()); m.push_back(0x01); put32(m, uint32_t(y)); put32(m, kPreviewW);
            m.insert(m.end(), line.begin(), line.end());
            o_.frame(m.data(), m.size());
            sentRows_ = y + 1;
        }
    }
    void status(const std::string& why) {
        if (!o_.json) return;
        std::string j = "{\"type\":\"lrpt_status\",\"data\":{\"on\":true";
        if (rx_) {
            const Stats s = rx_->stats();
            int s0, s1; rx_->stripRange(s0, s1);
            j += ",\"carrier\":" + std::string(rx_->demodLocked() ? "true" : "false") + ",\"sync\":" + (rx_->syncLocked() ? "true" : "false")
               + ",\"mode\":\"" + rx_->modeName() + "\",\"diff\":" + std::to_string(rx_->diffWinner())
               + ",\"symbolRate\":" + std::to_string(int(rx_->symbolRate())) + ",\"lock\":" + std::to_string(int(std::lround(rx_->lockMetric() * 100)))
               + ",\"offsetHz\":" + std::to_string(int(std::lround(rx_->carrierHz())))
               + ",\"frames\":" + std::to_string((unsigned long long)s.rsOk) + ",\"bad\":" + std::to_string((unsigned long long)s.rsFail)
               + ",\"packets\":" + std::to_string((unsigned long long)s.packets) + ",\"lines\":" + std::to_string((s1 - s0) * 8) + ",\"channels\":[";
            bool first = true;
            for (int a : rx_->liveApids()) { j += std::string(first ? "" : ",") + "\"" + channelName(a) + "\""; first = false; }
            j += "]";
        }
        if (!why.empty()) j += ",\"why\":\"" + why + "\"";
        j += "}}";
        o_.json(j);
    }

    Opts o_;
    std::unique_ptr<Receiver> rx_;
    double rate_ = 0, fedSec_ = 0, lastStatus_ = -1, lastFrameSec_ = 0;
    uint64_t seenVersion_ = 0, previewVersion_ = 0;
    double lastPreview_ = -1;
    bool havePictures_ = false, previewOpen_ = false, done_ = false, rateWarned_ = false;
    int sentRows_ = 0, previewS0_ = 0;
};

} // namespace vibelrpt
