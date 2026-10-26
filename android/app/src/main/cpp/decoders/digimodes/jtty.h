// decoders/digimodes/jtty.h — JTTY (the WSJT-X 3.2 keyboard mode) as a digital-spots mode (decoders/digi_mode.h): our own
// jtty::Decoder (decoders/jtty_decoder.h, docs/v12/JTTY.md) behind the Engine interface. V12, 2026-10-27.
//
// ★★ SLOT-LESS. JTTY has no UTC slots — a transmission of 1–16 frames (1.888 s each) starts at any moment — so this is a
//    free-running mode (ModeDef::slotSec 0): every block of audio goes straight into the decoder, which finds every
//    JTTY signal in the passband at once from ONE spectrogram. A spot is stamped with the moment its frame began.
// ★★ A SPOT IS A CALL ATOM. Only a frame that carries a call action (the FT8-family 28-bit standard call, which passed
//    the CRC-12, the reserved bit, the whole grammar and the coherence gate) becomes a spot: "CQ K1ABC CQ" and
//    "TU K1ABC CQ" are a station CALLING (how "CQ"), any other call atom is a station in a QSO (how "QSO"). A TEXT5
//    frame is free text and is never read for callsigns. JTTY carries no grid in its call atoms; a GRID4 atom is its
//    own frame and is not tied to a call here, so spots carry no grid (the country comes from the call — the host).
// ★ The dials are digi_bands.h's "JTTY" rows (the announced calling frequencies, checked against PSKReporter).
// ★ COST: the decoder runs a 768-point FFT every eighth of a symbol whatever is on air — measured ≈ 0.5–0.75 % of an M5
//   core (noise / a busy passband), about 10–16 FT8 engines' worth averaged over a slot. That is why Band's governor
//   (decoders/digi_governor.h) gates it on activity first: an empty JTTY window, most of the day on most bands, costs
//   only its tuner.
#pragma once
#include "../digi_mode.h"
#include "../jtty_decoder.h"

#include <cmath>
#include <memory>

namespace vibe {
namespace digi {

class JttyEngine : public Engine {
public:
    JttyEngine() {
        jtty::Decoder::Config c;
        c.rate = 12000; c.loHz = 150; c.hiHz = 3300;
        d_.reset(new jtty::Decoder(c));
        d_->onSpot = [this](const jtty::Spot& j) {
            if (!onSpot || j.call.empty()) return;
            Spot s;
            s.mode = "JTTY"; s.call = j.call; s.how = j.how;
            s.text = j.how == "CQ" ? "CQ " + j.call : j.call;
            s.snr = (int)std::lround(j.snrDb);
            s.audioHz = (float)j.audioHz;     // ★ tone 0, as WSJT-X reports a JTTY station (RN4AQ: 1088 Hz ↔ PSKReporter 14091088)
            s.slotUtc = j.utcMs / 1000.0;                           // ★ the frame's own start — no slots to name
            onSpot(s);
        };
    }
    void process(const float* audio, int n, double captureUtc) override {
        if (n <= 0) return;
        /* ★ The decoder's clock is its sample count from a start time. Anchor it to the capture clock: sample 0's UTC is
         *  this block's last sample's capture time less the samples counted so far. Lowest latency wins (a stamp is
         *  late, never early), and a jump of more than a second — audio that was not fed (Band's activity gate drops a
         *  quiet window's audio) — re-anchors, so a spot after a gap still carries its real time. */
        if (std::isfinite(captureUtc)) {
            const double est = captureUtc - (double)(d_->samples() + (uint64_t)n) / 12000.0;
            if (!anchored_ || est < utc0_ || est - utc0_ > 1.0) {
                utc0_ = est; anchored_ = true;
                d_->setStartUtcMs((int64_t)std::llround(utc0_ * 1000.0));
            }
        }
        d_->feed(audio, n);
    }
    // ★ Free-running: no slot to skip. Its "decoded" count is frames (tests, the admin page).
    uint64_t slotsDecoded() const override { return d_->framesDecoded(); }
    jtty::Decoder& decoder() { return *d_; }
private:
    std::unique_ptr<jtty::Decoder> d_;
    double utc0_ = 0; bool anchored_ = false;
};

inline ModeDef vibeDigiMode_jtty() {
    ModeDef m;
    m.name = "JTTY";
    m.slotSec = 0;                   // ★ free-running
    m.sampleRate = 12000;
    /* ★ tone 0 is searched over 150–3300 Hz and the top tone sits 94 Hz above it: Band's tuner is asked for 100–3450 so
     *  every tone of every signal it searches is flat passband. */
    m.audioLoHz = 100; m.audioHiHz = 3450;
    m.costVsFt8 = 12.0;              // ★ measured (test-digi-spots cpu, docs/v12/DIGITAL-SPOTS.md): ≈ 12× one FT8 engine
    m.lightCostVsFt8 = 12.0;         //   and no lighter setting — its depth is fixed (list Viterbi of 4, one coherent pass)
    m.priority = 60;                 // ★ the governor's order (digi_governor.h): after FT2, before PSK
    m.gateSec = 4.0;                 // ★ the activity gate's period: two frames' time
    m.make = [] { return std::unique_ptr<Engine>(new JttyEngine()); };
    return m;                        // dials: digi_bands.h's "JTTY" rows
}

} // namespace digi
} // namespace vibe
