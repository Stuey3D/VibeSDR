// decoders/digimodes/pskw.h — WIDE PSK: PSK31 / PSK63 / PSK125 / QPSK31 / QPSK63 across the band's whole narrow-band digimode segment, as a Band chip
// (V12 beta91, docs/v12/BAND-SKIM-CW-RTTY.md). Stuart (2026-11-22, his first PSK63): "he wants more of it".
//
// ★★ The original PSK chip (psk.h) reads 200–3600 Hz above the conventional PSK31 dial (7040, 14070 …): where PSK31 is
//    called, and where it runs with the tuners like every audio mode. This chip reads the SAME segment the RTTY chip does
//    (vibe_skim_bands.h, the owner's IARU region — 40 m R1 7040–7050, 20 m 14070–14099) straight off the slice, so a PSK31
//    or PSK63 QSO anywhere in it is spotted. Engine: the PSK skimmer's own psk::Skimmer (one FFT per hop across the
//    window, five lanes per carrier — PSK31, PSK63, PSK125, QPSK31, QPSK63 — the clean one wins), on its own worker.
// ★★ NO DOUBLE SPOTS: the original PSK chip's windows (every published PSK dial + 150 … 3650 Hz that falls inside this
//    window) are LEFT TO IT — a station heard there by this engine is not spotted again. Both chips run; the original keeps
//    its window whether or not this one is on, shed or paused.
// ★ beta91: PSK125, QPSK31 and QPSK63 too — psk::Skimmer's lanes (a QPSK lane Viterbi-decodes the PSK31 spec's K=5
//   code); spot modes PSK31 / PSK63 / PSK125 / QPSK31 / QPSK63, the ADIF names PSK Reporter takes (psk.h spotModeOf).
// ★ Shed and paused LAST of the three IQ chips (CW, then RTTY, then this) — Stuart's order.
#pragma once
#include "iq_worker.h"
#include "psk.h"
#include "../psk_skimmer.h"
#include "../digi_bands.h"
#include "../../vibe_skim_bands.h"

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace vibe {
namespace digi {

class PskWideEngine : public IqWorkerEngine {
public:
    /** `skip`: RF stretches left to the original PSK chip (its windows inside this one). */
    PskWideEngine(double loHz, double hiHz, std::vector<std::pair<double, double>> skip)
        : IqWorkerEngine(loHz, hiHz), skip_(std::move(skip)) { startWorker("vibe-bandpsk"); }
    ~PskWideEngine() override { stopWorker(); }
    /** The original PSK chip's windows inside [lo, hi] — every published PSK dial's 150 … 3650 Hz (psk.h's passband). */
    static std::vector<std::pair<double, double>> originalWindows(double lo, double hi) {
        std::vector<std::pair<double, double>> o;
        for (const Dial& d : publishedDials())
            if (sameName("PSK", d.mode)) {
                const double a = d.hz + 150, b = d.hz + 3650;
                if (b > lo && a < hi) o.push_back({ a, b });
            }
        return o;
    }
protected:
    void consume(const cf32* x, int n, double rate, double centreHz, double captureUtc) override {
        if (!sk_ || std::fabs(rate - rate_) > 0.5 || std::fabs(centreHz - centre_) > 0.5) {
            psk::Skimmer::Config c;
            c.rate = rate; c.complexIn = true;
            c.loHz = lo_ - centreHz; c.hiHz = hi_ - centreHz;
            c.maxSignals = std::max(32, std::min(128, (int)((hi_ - lo_) / 150.0)));
            sk_.reset(new psk::Skimmer(c));
            rate_ = rate; centre_ = centreHz; anchored_ = false;
            sk_->onSpot = [this](const psk::Spot& p) {
                if (!onSpot || p.call.empty()) return;
                const double rf = centre_ + p.offsetHz;
                for (const auto& s : skip_) if (rf >= s.first && rf <= s.second) return;   // ★ the original chip's
                Spot s;
                s.mode = spotModeOf(p.mode);
                s.call = p.call; s.how = p.how;
                s.text = p.how + " " + p.call;
                s.textMade = true;   // ★ beta94 a summary, not the message sent — never the spot's "msg"
                s.snr = (int)std::lround(p.snrDb - es0To2500Db(p.mode));   // ★ Es/N0 → dB in 2500 Hz, as psk.h
                s.rfHz = rf;
                s.slotUtc = p.utcMs / 1000.0;
                onSpot(s);
            };
        }
        sk_->feedIq(x, n);
        if (std::isfinite(captureUtc)) {
            const int64_t cap = (int64_t)std::llround(captureUtc * 1000.0), now = sk_->nowMs();
            if (!anchored_ || cap < now || cap - now > 1000) { sk_->setStartUtcMs(cap); anchored_ = true; }
        }
    }
private:
    std::vector<std::pair<double, double>> skip_;
    std::unique_ptr<psk::Skimmer> sk_;
    double rate_ = 0, centre_ = 0;
    bool anchored_ = false;
};

inline ModeDef vibeDigiMode_pskw() {
    ModeDef m;
    m.name = "PSKW";                 // ★ "wide PSK" — the chip and its toggle ("spotsband-pskw"); spots say PSK31 … QPSK63
    m.iq = true;
    m.single = false;
    m.slotSec = 0;
    m.sampleRate = 0;
    // ★ per 48 kHz of window (at least half) — MEASURED (BAND-SKIM-CW-RTTY.md): 0.276 % at 48 kHz with 19 signals vs FT8's 0.083 %
    //   (3.3) with two lanes; beta91's five lanes cost ×1.38 on a busy window (0.160 → 0.220 %, noise unchanged): 4.5
    m.costVsFt8 = 4.5;
    m.lightCostVsFt8 = 4.5;
    m.priority = 105;                // ★ shed after CW (120) and RTTY (110), before every audio mode
    m.pauseRank = 3;                 // ★ paused last of the three
    m.gateSec = 4.0;
    m.windows = [](const std::string& band) { return skimbands::bandWindows(skimbands::Kind::Rtty, skimbands::bandRegion().load(), band); };
    m.makeWin = [](double lo, double hi) { return std::unique_ptr<Engine>(new PskWideEngine(lo, hi, PskWideEngine::originalWindows(lo, hi))); };
    m.make = [] { return std::unique_ptr<Engine>(); };
    return m;
}

} // namespace digi
} // namespace vibe
