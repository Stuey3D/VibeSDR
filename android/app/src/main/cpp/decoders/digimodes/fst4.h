// decoders/digimodes/fst4.h — FST4 as a digital-spots mode (decoders/digi_mode.h): our own fst4::LiveDecoder
// (digimodes/fst4_decoder.h, clean room — docs/v12/DECODERS-WAVE2.md "FST4 / FST4W"). V12, 2026-10-31.
// ★ Periods 60, 120 and 300 s (the LF/MF QSO periods); the spot's mode names it ("FST4-120"). Audio 300–1500 Hz (on 630 m 474.5–475.7 kHz, where the activity is 475.1–475.5).
// ★ Dials: digi_bands.h's "FST4" rows (630 m and 2200 m, where PSKReporter shows FST4 in use).
// ★ COST: measured in docs/v12/DECODERS-WAVE2.md (three streams + a decode a period; small).
#pragma once
#include "../digi_mode.h"
#include "fst4_decoder.h"

#include <memory>

namespace vibe {
namespace digi {

class Fst4Engine : public Engine {
public:
    explicit Fst4Engine(bool w)
        : d_(w ? fst4::Mode::FST4W : fst4::Mode::FST4, w ? std::vector<int>{120, 300, 900, 1800} : std::vector<int>{60, 120, 300},
             1500.0, opts(w)) {
        d_.loadProbe = [this] { return loaded ? loaded() : false; };
        d_.onResult = [this](const fst4::Result& r) {
            if (!onSpot || r.call.empty()) return;
            Spot s;
            s.mode = r.modeName(); s.call = r.call; s.grid = r.grid; s.text = r.text;
            s.snr = r.snr; s.audioHz = r.f0; s.dt = r.dt; s.slotUtc = r.slotUtc;
            onSpot(s);
        };
    }
    void process(const float* audio, int n, double captureUtc) override { d_.process(audio, n, captureUtc); }
    uint64_t slotsSkipped() const override { return d_.slotsSkipped.load(); }
    uint64_t slotsDecoded() const override { return d_.slotsDecoded.load(); }
private:
    static fst4::SlotOptions opts(bool w) { fst4::SlotOptions o; if (w) { o.loHz = 1400; o.hiHz = 1600; } else { o.loHz = 300; o.hiHz = 1500; } return o; }
    fst4::LiveDecoder d_;
};

inline ModeDef vibeDigiMode_fst4() {
    ModeDef m;
    m.name = "FST4";
    m.slotSec = 60.0;                // ★ the activity gate's period (the 120/300 s periods ride along)
    m.sampleRate = 12000;
    m.audioLoHz = 250; m.audioHiHz = 1550;   // ★ 2200 m: 136.0 + 1.55 kHz + the tuner guard stays under the band top (137.8)
    m.costVsFt8 = 1.0; m.lightCostVsFt8 = 0.7;   // measured, see the doc
    m.priority = 100;
    m.make = [] { return std::unique_ptr<Engine>(new Fst4Engine(false)); };
    return m;
}

} // namespace digi
} // namespace vibe
