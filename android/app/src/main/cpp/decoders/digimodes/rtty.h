// decoders/digimodes/rtty.h — THE RTTY SKIMMER AS A BAND CHIP (V12 beta91, docs/v12/BAND-SKIM-CW-RTTY.md).
//
// ★★ The band's RTTY / narrow-band digimode segment (vibe_skim_bands.h, the owner's IARU region — 40 m R1 7040–7050, 20 m
//    14070–14099; Region 2 also the US RTTY stretch 7080–7125) as an IQ window of the band decode (ModeDef::iq). Its engine
//    is the RTTY skimmer VFO's own rttysk::Skimmer (decoders/rtty_skimmer.h: two tones a standard shift apart that TAKE
//    TURNS, then each signal's own settings searched for), fed on its own worker thread (iq_worker.h), with ONE search
//    worker of its own beside it (the engine queues each new signal's parameter search; a search runs much faster than
//    real time on a copy of the stored channel).
// ★★ SPOTS ONLY WHERE THE DECODE IDENTIFIES A CALL: the skimmer's own rule — text that READS AS TEXT, then a callsign in
//    CQ / DE / TEST (cw::identify) heard twice on that signal. Noise, FT8's crowd, CW and PSK carriers spot nothing (the
//    tone-alternation test lists none of them; test-bg-band checks noise and an FT8 crowd inside the window). Mode "RTTY"
//    (ADIF RTTY) whatever the settings — PSK Reporter and the map know RTTY, not "RTTY 45/170".
#pragma once
#include "iq_worker.h"
#include "../rtty_skimmer.h"
#include "../../vibe_skim_bands.h"

#include <cmath>
#include <memory>

namespace vibe {
namespace digi {

class RttyBandEngine : public IqWorkerEngine {
public:
    RttyBandEngine(double loHz, double hiHz) : IqWorkerEngine(loHz, hiHz) {
        search_ = std::thread([this] { searcher(); });
        startWorker("vibe-bandrtty");
    }
    ~RttyBandEngine() override {
        stopWorker();   // ★ the feeder first: nothing queues a search after this
        { std::lock_guard<std::mutex> lk(sm_); sstop_ = true; }
        scv_.notify_all();
        if (search_.joinable()) search_.join();
    }
protected:
    void consume(const cf32* x, int n, double rate, double centreHz, double captureUtc) override {
        if (!sk_ || std::fabs(rate - rate_) > 0.5 || std::fabs(centreHz - centre_) > 0.5) {
            rttysk::Skimmer::Config c;
            c.rate = rate; c.loHz = lo_ - centreHz; c.hiHz = hi_ - centreHz;
            c.workers = 1;                                                       // ★ the searches run on searcher()
            c.maxSignals = std::max(16, std::min(64, (int)((hi_ - lo_) / 400.0)));   // ★ RTTY at 170 Hz needs ≈ 400 Hz
            std::shared_ptr<rttysk::Skimmer> s(new rttysk::Skimmer(c));
            const double centre = centreHz;
            s->onSpot = [this, centre](const rttysk::Spot& p) {
                if (!onSpot || p.call.empty()) return;
                Spot o;
                o.mode = "RTTY";
                o.call = p.call; o.how = p.how;
                o.text = p.how + " " + p.call + (p.mode.empty() ? std::string() : " (" + p.mode + ")");
                o.snr = std::isfinite(p.snrDb) ? (int)std::lround(p.snrDb) : 0;
                o.rfHz = centre + p.offsetHz;
                o.slotUtc = p.utcMs / 1000.0;
                onSpot(o);
            };
            s->onWork = [this] { { std::lock_guard<std::mutex> lk(sm_); ++wake_; } scv_.notify_one(); };
            { std::lock_guard<std::mutex> lk(sm_); sk_ = s; }   // ★ the searcher holds its own reference: the old one dies with its job
            rate_ = rate; centre_ = centreHz; anchored_ = false;
        }
        sk_->feedIq(x, n);
        if (std::isfinite(captureUtc)) {
            const int64_t cap = (int64_t)std::llround(captureUtc * 1000.0), now = sk_->nowMs();
            if (!anchored_ || cap < now || cap - now > 1000) { sk_->setStartUtcMs(cap); anchored_ = true; }
        }
    }
private:
    void searcher() {
        vibeDecoderLagThread("vibe-bandrttys");
        for (;;) {
            std::shared_ptr<rttysk::Skimmer> s;
            {
                std::unique_lock<std::mutex> lk(sm_);
                scv_.wait(lk, [&] { return sstop_ || wake_ > 0; });
                if (sstop_) return;
                --wake_;
                s = sk_;
            }
            if (!s) continue;
            const uint64_t c0 = threadCpuNs();
            for (;;) {
                { std::lock_guard<std::mutex> lk(sm_); if (sstop_) break; }
                if (!s->workOnce()) break;
            }
            if (onCpu) onCpu(threadCpuNs() - c0);
        }
    }
    std::shared_ptr<rttysk::Skimmer> sk_;
    double rate_ = 0, centre_ = 0;
    bool anchored_ = false;
    std::mutex sm_;
    std::condition_variable scv_;
    int wake_ = 0;
    bool sstop_ = false;
    std::thread search_;
};

inline ModeDef vibeDigiMode_rtty() {
    ModeDef m;
    m.name = "RTTY";
    m.iq = true;
    m.single = false;                // ★ Band only (a listener's own RTTY decoder is DECODERS → RTTY)
    m.slotSec = 0;
    m.sampleRate = 0;
    // ★ per 48 kHz of window (at least half) — MEASURED (BAND-SKIM-CW-RTTY.md): 0.331 % at 48 kHz with 19 signals vs FT8's 0.083 %
    m.costVsFt8 = 4.0;
    m.lightCostVsFt8 = 4.0;
    m.priority = 110;                // ★ shed after CW, before every audio mode
    m.pauseRank = 2;                 // ★ paused after CW when the band decode falls behind
    m.gateSec = 4.0;
    m.windows = [](const std::string& band) { return skimbands::bandWindows(skimbands::Kind::Rtty, skimbands::bandRegion().load(), band); };
    m.makeWin = [](double lo, double hi) { return std::unique_ptr<Engine>(new RttyBandEngine(lo, hi)); };
    m.make = [] { return std::unique_ptr<Engine>(); };
    return m;
}

} // namespace digi
} // namespace vibe
