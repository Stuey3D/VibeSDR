// decoders/digimodes/cw.h — THE CW SKIMMER AS A BAND CHIP (V12 beta91, docs/v12/BAND-SKIM-CW-RTTY.md).
//
// Stuart (2026-11-22): "is it worth adding cw skimmer and rtty skimmer to the full band skimming? so one decoder for all
// digi psk cw rtty modes?" … "yes please, i want to get this working as a pskr beast".
//
// ★★ ONE BAND VFO, ONE SLOT, EVERY MODE. The band decode (decoders/digi_spotter.h) already reads FT8 … PSK off one slice
//    and its extras; this adds the band's whole CW SEGMENT (vibe_skim_bands.h, the owner's IARU region — 40 m R1 7000–7040)
//    as an IQ window (ModeDef::iq). Its engine is the same cw::Skimmer the CW skimmer VFO and the listener's CW decoder run
//    (decoders/cw_skimmer.h — one FFT per hop across the window, a timing decoder only on keyed carriers), on its own
//    worker thread (iq_worker.h). Spots: a callsign heard identifying itself TWICE (CQ … CALL, DE CALL, TEST CALL) — the
//    skimmer's own rule — mode "CW", into the band decode's one spot path (the summary card, the stations store, PSK
//    Reporter as ADIF "CW", the digital_spot frame), exactly as FT8's.
// ★ SNR: the skimmer's mark over noise in its own ≈ 47 Hz bin, as the standalone CW skimmer reports it (no 2500 Hz
//   conversion — PSK Reporter's CW receivers report this way, and the two paths must agree).
// ★ COST: per 48 kHz of window (costVsFt8), measured in test-bg-band (BAND-SKIM-CW-RTTY.md has the figures).
#pragma once
#include "iq_worker.h"
#include "../cw_skimmer.h"
#include "../../vibe_skim_bands.h"

#include <cmath>
#include <memory>

namespace vibe {
namespace digi {

class CwBandEngine : public IqWorkerEngine {
public:
    CwBandEngine(double loHz, double hiHz) : IqWorkerEngine(loHz, hiHz) { startWorker("vibe-bandcw"); }
    ~CwBandEngine() override { stopWorker(); }
    /** Confirmed signals now (tests, the admin line). Worker-thread state: read only after the engine has stopped being fed. */
    int confirmedForTest() const { return sk_ ? sk_->confirmed() : 0; }
protected:
    void consume(const cf32* x, int n, double rate, double centreHz, double captureUtc) override {
        if (!sk_ || std::fabs(rate - rate_) > 0.5 || std::fabs(centreHz - centre_) > 0.5) {
            // ★ a new slice (rate) or a moved one: the window's offsets change — start afresh (CwRun does the same on a jump)
            cw::Skimmer::Config c;
            c.rate = rate; c.complexIn = true;
            c.loHz = lo_ - centreHz; c.hiHz = hi_ - centreHz;
            c.maxSignals = std::max(32, std::min(240, (int)((hi_ - lo_) / 300.0)));   // ★ a signal every 300 Hz at most
            sk_.reset(new cw::Skimmer(c));
            rate_ = rate; centre_ = centreHz; anchored_ = false;
            sk_->onSpot = [this](const cw::Spot& p) {
                if (!onSpot || p.call.empty()) return;
                Spot s;
                s.mode = "CW";
                s.call = p.call; s.how = p.how;
                s.text = p.how + " " + p.call;
                s.textMade = true;   // ★ beta94 a summary, not the message sent — never the spot's "msg"
                s.snr = std::isfinite(p.snrDb) ? (int)std::lround(p.snrDb) : 0;
                s.rfHz = centre_ + p.offsetHz;
                s.slotUtc = p.utcMs / 1000.0;
                onSpot(s);
            };
        }
        sk_->feedIq(x, n);
        if (std::isfinite(captureUtc)) {
            const int64_t cap = (int64_t)std::llround(captureUtc * 1000.0), now = sk_->nowMs();
            // ★ the skimmer's clock is its sample count; setStartUtcMs says what NOW is (as PskEngine): re-said when the
            //   capture clock is a second away, or earlier (a stamp is late, never early)
            if (!anchored_ || cap < now || cap - now > 1000) { sk_->setStartUtcMs(cap); anchored_ = true; }
        }
        /* ★★ beta92 BAND SKIMMER (Stuart: "when expanded we should also be able to read the cw message like we do in the
         *  cw skimmer"): once a second, the signals the skimmer follows — callsign or not ("waiting for callsign"), each
         *  with its last 120 characters, exactly what the CW skimmer's own list carries (vibe_cw_run.h sendList). */
        if (rowsDue(sk_->nowMs())) {
            std::vector<SkimRow> rows;
            for (const cw::SignalView& v : sk_->signals(true)) {
                if ((int)rows.size() >= kSkimRowsMax) break;
                SkimRow r;
                r.mode = "CW"; r.id = v.id; r.hz = centre_ + v.offsetHz; r.snr = v.snrDb;
                r.wpm = v.wpm; r.wpmLo = v.wpmLo; r.wpmHi = v.wpmHi; r.quality = v.quality;
                r.active = v.active; r.lastMs = v.lastMs; r.call = v.call;
                r.text = v.text.size() > kSkimTextMax ? v.text.substr(v.text.size() - kSkimTextMax) : v.text;
                rows.push_back(std::move(r));
            }
            publishRows(std::move(rows));
        }
    }
private:
    std::unique_ptr<cw::Skimmer> sk_;
    double rate_ = 0, centre_ = 0;
    bool anchored_ = false;
};

inline ModeDef vibeDigiMode_cw() {
    ModeDef m;
    m.name = "CW";
    m.iq = true;
    m.single = false;                // ★ Band only (a listener's own CW decoder is DECODERS → CW)
    m.slotSec = 0;                   // ★ free-running
    m.sampleRate = 0;                // ★ no audio: the slice's own IQ
    // ★ per 48 kHz of window (at least half that: a 10 kHz window costs half a 48's) — MEASURED on the M5 Pro
    //   (chip bench, BAND-SKIM-CW-RTTY.md): 0.136 % of a core at 48 kHz against one FT8 engine's 0.083 %
    m.costVsFt8 = 1.6;
    m.lightCostVsFt8 = 1.6;
    m.priority = 120;                // ★ shed before every slotted and audio mode (they never lose a slot to it)…
    m.pauseRank = 1;                 // ★ …and the FIRST paused when the band decode falls behind (Stuart: "CW then RTTY")
    m.gateSec = 4.0;
    m.windows = [](const std::string& band) { return skimbands::bandWindows(skimbands::Kind::Cw, skimbands::bandRegion().load(), band); };
    m.makeWin = [](double lo, double hi) { return std::unique_ptr<Engine>(new CwBandEngine(lo, hi)); };
    m.make = [] { return std::unique_ptr<Engine>(); };   // ★ never made without a window (Spotter uses makeWin)
    return m;
}

} // namespace digi
} // namespace vibe
