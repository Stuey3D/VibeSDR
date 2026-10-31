// decoders/digimodes/q65.h — Q65 as a digital-spots mode (decoders/digi_mode.h): our own q65::LiveDecoder
// (digimodes/q65_decoder.h, clean room — docs/v12/DECODERS-WAVE2.md "Q65") behind the Engine interface. V12, 2026-10-31.
//
// ★★ ONE ENGINE, THREE PERIODS. Q65 has no single T/R period: EME on 2 m and 70 cm is mostly Q65-60 (A–D by the band's
//    Doppler spread), ionoscatter and 6 m Q65-30A, and Q65-15 is used for quick scatter contacts. The engine decodes the
//    15, 30 AND 60 s periods of the same audio (15 s added 2026-10-31 — Stuart: "make sure we cover everything"), every
//    submode A–D, and the spot's mode names what it was ("Q65-60B") — the map and the list file it under Q65
//    (src/utils/digiModes.ts digiModeFamily). 120/300 s and submode E (10 GHz and up, outside our bands) are built in the
//    engine but not run here.
// ★★ DIALS (digi_bands.h's "Q65" rows): only where the IARU Region 1 plans allow MGM AND PSKReporter shows Q65 being
//    used — see the table's note. The audio window is 200–4800 Hz, wider than the other modes', because EME stations
//    spread across their segment rather than sitting on one dial.
// ★ COST, measured (q65-bench on noise — every candidate tried, the worst case): M-series Mac ≈ 42 ms per 60 s window
//   and ≈ 34 ms per 30 s window, ≈ 110 ms a minute; FT8's engine ≈ 42 ms a minute (10.5 ms per 15 s slot) → costVsFt8
//   ≈ 2.6. Pi 2 (qemu-arm instruction count, NEON build): ≈ 0.84 G instructions per 60 s window, ≈ 0.68 G per 30 s →
//   ≈ 2.2 G a minute → ≈ 3–4 s of one 900 MHz core a minute at 0.6–0.8 IPC (≈ 5–7 %, ESTIMATED). The scalar build is
//   2.3× that (belief propagation is 4/5 of it unvectorised). Loaded: one submode, half the candidates, no a-priori.
// ★★ Q65-15 (2026-10-31) DOUBLES IT: Mac 23.9 ms per 15 s window (× 4 a minute = 96 ms) beside 30 s 31.1 + 60 s 39.0 ms
//    (101 ms a minute); Pi 2 count ≈ 0.55 G instructions a 15 s window → +2.2 G a minute, ≈ 4.4 G a minute in all →
//    ≈ 6–8 s of a 900 MHz core a minute per dial (≈ 10–14 %, ESTIMATE). costVsFt8 2.6 → 4.7. The governor sheds Q65
//    first (priority 100).
#pragma once
#include "../digi_mode.h"
#include "q65_decoder.h"

#include <cmath>
#include <memory>

namespace vibe {
namespace digi {

class Q65Engine : public Engine {
public:
    Q65Engine() : d_(std::vector<int>{15, 30, 60}) {
        d_.opts.loHz = 200; d_.opts.hiHz = 4800;
        d_.opts.submodes = 0x0F;                       // A–D
        d_.loadProbe = [this] { return loaded ? loaded() : false; };
        d_.onResult = [this](const q65::Result& r) {
            if (!onSpot || r.call.empty()) return;     // ★ a spot is a call heard in full; free text never is
            Spot s;
            s.mode = r.mode(); s.call = r.call; s.grid = r.grid; s.text = r.text;
            s.snr = r.snr; s.audioHz = r.f0; s.dt = r.dt; s.slotUtc = r.slotUtc;
            onSpot(s);
        };
    }
    void process(const float* audio, int n, double captureUtc) override { d_.process(audio, n, captureUtc); }
    uint64_t slotsSkipped() const override { return d_.slotsSkipped.load(); }
    uint64_t slotsDecoded() const override { return d_.slotsDecoded.load(); }
    q65::LiveDecoder& decoder() { return d_; }
private:
    q65::LiveDecoder d_;
};

inline ModeDef vibeDigiMode_q65() {
    ModeDef m;
    m.name = "Q65";
    m.slotSec = 60.0;              // ★ the activity gate's period: a minute (the 30 s periods within it ride along)
    m.sampleRate = 12000;
    m.audioLoHz = 200; m.audioHiHz = 4800;
    m.costVsFt8 = 4.7;             // measured, see the file note (2.6 before Q65-15 joined, 2026-10-31)
    m.lightCostVsFt8 = 2.2;        // loaded: one submode, half the candidates, no a-priori pass (scaled with the 15 s period)
    m.priority = 100;              // ★ the governor sheds it before every established mode
    m.make = [] { return std::unique_ptr<Engine>(new Q65Engine()); };
    return m;                      // dials: digi_bands.h's "Q65" rows
}

} // namespace digi
} // namespace vibe
