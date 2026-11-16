// vibe_hfdl_run.h — one listener's HFDL decoder inside a decoder host (vibe_decoder_host.h, ext "hfdl"): the
// listener tunes the HFDL frequency in USB (the published "dial", the carrier 1440 Hz above it — ITU-R M.1458) and
// this decodes the burst out of their audio (vibe_hfdl.h Demod). docs/v12/AVIATION.md §HFDL, RESUME HERE step 4.
//
// ★ The audio is real (48 kS/s); the carrier sits at 1440 Hz in it with ±1.2 kHz of signal. Mixed down by 1440 Hz its
//   own image lands at −2880 ± 1.2 kHz, only ~500 Hz beyond the signal's edge, so the channel is cut in two stages:
//   a short FIR to 12 kS/s (nothing above ~4.5 kHz is in USB audio), then a sharp one at 12 kS/s (flat to 1.25 kHz,
//   shut by 1.65 kHz) before the Demod (which resamples to its 7200 S/s itself). Both are vibedsp FirDecimators
//   (NEON / SSE2 / scalar). About 3 % of a Pi 2 core with the Demod (AVIATION.md §Cost), on the host's decoder thread.
// ★ Every message goes to the radio's aircraft table (vibeavi::sharedTracker()) — the HFDL MAP's /addon/hfdl/* is drawn
//   from it — and a one-line summary to the listener's decoder box. ★★ NEVER A MESSAGE BODY in the box: ACARS free text
//   is the owner's opt-in and shown to the owner only (DECODER-RESEARCH.md §4.3); the line carries the flight, the
//   ground station, the position and the label's plain name.
//
// Wire (the decoder socket, beside RTTY's frames — web/client/src/decoders.ts):
//   0x01 | u64 BE ts | u32 BE len | UTF-8     one line per message
//   {"type":"hfdl_status","data":{"msgs":N,"bursts":N,"detections":N}}   at most every 2 s, when it changed
#pragma once
#include "vibe_avi_service.h"
#include "vibedsp/vibedsp.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {

class HfdlRun {
public:
    struct Out {
        std::function<void(const std::string&)> text;   ///< 0x01
        std::function<void(const std::string&)> json;   ///< a JSON text frame
    };
    /** `dialHz()`: the listener's USB dial now (the message's frequency), may be empty. */
    HfdlRun(vibeavi::Tracker& tr, Out out, std::function<double()> dialHz, double audioRate = 48000)
        : tr_(tr), out_(std::move(out)), dial_(std::move(dialHz)), rate_(audioRate) {
        dec1_ = std::max(1, int(std::lround(audioRate / 12000.0)));
        const double r1 = audioRate / dec1_;
        // stage 1: pass 1.7 kHz, shut by r1 − 4.5 kHz (what folds onto the channel is above that, and USB audio has none)
        st1_.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(((1700.0 + (r1 - 4500.0)) / 2) / audioRate, ((r1 - 4500.0) - 1700.0) / audioRate), dec1_));
        // stage 2: flat to 1.25 kHz, shut by 1.65 kHz — the image's inner edge is 1.70 kHz below the carrier
        st2_.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(1450.0 / r1, 400.0 / r1, true), 1));
        dm_.reset(new vibehfdl::Demod(r1));
        dm_->setChannel(0, dial_ ? dial_() : 0.0);
        rot_ = std::polar(1.0, -2.0 * M_PI * vibehfdl::kCarrierOffsetHz / audioRate);
    }
    void feedAudio(const float* x, int n) {
        if (n <= 0) return;
        z_.resize(size_t(n));
        std::complex<double> p = ph_;
        for (int i = 0; i < n; ++i) { z_[size_t(i)] = vibedsp::cf32(float(x[i] * p.real()), float(x[i] * p.imag())); p *= rot_; }
        ph_ = p / std::abs(p);
        a_.resize(size_t(st1_->maxOut(n)));
        const int k1 = st1_->process(z_.data(), n, a_.data());
        b_.resize(size_t(k1 + 1));
        const int k2 = st2_->process(a_.data(), k1, b_.data());
        if (dial_) dm_->setChannel(0, dial_());
        const double now = nowS();
        dm_->feed(b_.data(), k2, [&](const vibehfdl::Message& m) {
            ++msgs_;
            tr_.addHfdl(m, now);
            if (out_.text) out_.text(line(m) + "\n");
        });
        const auto& st = dm_->stats();
        if (out_.json && now - lastStatus_ >= 2.0 && (st.bursts != lastBursts_ || msgs_ != lastMsgs_ || lastStatus_ == 0)) {
            lastStatus_ = now; lastBursts_ = st.bursts; lastMsgs_ = msgs_;
            char b[160];
            std::snprintf(b, sizeof b, "{\"type\":\"hfdl_status\",\"data\":{\"msgs\":%llu,\"bursts\":%llu,\"detections\":%llu}}",
                          (unsigned long long)msgs_, (unsigned long long)st.bursts, (unsigned long long)st.detections);
            out_.json(b);
        }
    }
    uint64_t messages() const { return msgs_; }

    /** The box's line: frequency, direction and station, who, where, what — never the body. */
    static std::string line(const vibehfdl::Message& m) {
        std::string s;
        char b[160];
        std::snprintf(b, sizeof b, "HFDL %.0f kHz %d bps", m.hz / 1e3, vibehfdl::bitRate(m.mode));
        s = b;
        const int gs = m.downlink ? m.dstId : m.srcId;
        const char* gn = vibehfdl::gsName(gs);
        s += m.downlink ? " \xc2\xb7 aircraft to " : " \xc2\xb7 ground station ";
        s += *gn ? std::string(gn) : "station " + std::to_string(gs);
        if (!m.flight.empty()) s += " \xc2\xb7 " + clean(m.flight);
        if (m.hasAcars && m.acarsCrcOk && !m.acars.reg.empty()) s += " \xc2\xb7 " + clean(m.acars.reg);
        if (m.icao) { std::snprintf(b, sizeof b, " \xc2\xb7 %06X", m.icao); s += b; }
        if (!std::isnan(m.lat)) {
            std::snprintf(b, sizeof b, " \xc2\xb7 %.2f %c %.2f %c", std::fabs(m.lat), m.lat >= 0 ? 'N' : 'S', std::fabs(m.lon), m.lon >= 0 ? 'E' : 'W');
            s += b;
        }
        if (m.hasAcars && m.acarsCrcOk) {
            const char* ln = vibeavi::labelName(m.acars.label);
            s += " \xc2\xb7 ACARS " + clean(m.acars.label) + (*ln ? std::string(" (") + ln + ")" : std::string());
        } else if (!m.typeName.empty()) s += " \xc2\xb7 " + m.typeName;
        return s;
    }

private:
    static double nowS() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    /** Flight IDs and registrations: printable ASCII only (they reach text frames and JSON). */
    static std::string clean(const std::string& x) { std::string o; for (char c : x) if (c > 0x20 && c < 0x7F) o += c; return o; }

    vibeavi::Tracker& tr_;
    Out out_;
    std::function<double()> dial_;
    double rate_;
    int dec1_ = 4;
    std::unique_ptr<vibedsp::FirDecimator> st1_, st2_;
    std::unique_ptr<vibehfdl::Demod> dm_;
    std::complex<double> ph_{1.0, 0.0}, rot_{1.0, 0.0};
    std::vector<vibedsp::cf32> z_, a_, b_;
    uint64_t msgs_ = 0, lastBursts_ = 0, lastMsgs_ = 0;
    double lastStatus_ = 0;
};

} // namespace vibe
