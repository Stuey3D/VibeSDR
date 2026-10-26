// decoders/digimodes/ft8.h — FT8 and FT4 as digital-spots modes (decoders/digi_mode.h): the existing Ft8Decoder, unchanged,
// behind the Engine interface. ★ The pattern a new mode copies — see docs/v12/DIGITAL-SPOTS.md.
#pragma once
#include "../digi_mode.h"
#include "../ft8_decoder.h"

#include <memory>

namespace vibe {
namespace digi {

/** One Ft8Decoder (its own worker thread; a slot still decoding when the next completes is skipped and counted). */
class Ft8Engine : public Engine {
public:
    explicit Ft8Engine(bool ft4) : d_(12000, ft4), ft4_(ft4) {
        d_.onSpot = [this](const std::string& to, const std::string& de, const std::string& grid, int snr, float hz) {
            (void)to;
            if (!onSpot) return;
            Spot s; s.mode = ft4_ ? "FT4" : "FT8"; s.call = de; s.grid = grid; s.snr = snr; s.audioHz = hz;
            s.slotUtc = d_.decodingSlotUtc();   // ★ the slot's start (onSpot runs inside that slot's decode)
            onSpot(s);
        };
        d_.loadProbe = [this] { return loaded ? loaded() : false; };
    }
    void process(const float* audio, int n, double captureUtc) override { d_.process(audio, n, captureUtc); }
    uint64_t slotsSkipped() const override { return d_.slotsSkipped.load(); }
    uint64_t slotsDecoded() const override { return d_.slotsDecoded.load(); }
    Ft8Decoder& decoder() { return d_; }
private:
    Ft8Decoder d_;
    bool ft4_;
};

inline ModeDef vibeDigiMode_ft8() {
    ModeDef m;
    m.name = "FT8"; m.slotSec = 15.0; m.sampleRate = 12000;
    m.audioLoHz = 100; m.audioHiHz = 3500;          // ★ what Ft8SlotDecoder's monitor searches (f_min / f_max)
    m.costVsFt8 = 1.0;
    m.lightCostVsFt8 = 0.6;                          // ★ one pass, no OSD (measured, docs/v12/DIGITAL-SPOTS.md)
    m.priority = 10;                                 // ★ the governor's first: most of the traffic on every band
    m.singleFallback = true;                         // ★ Single, off every dial: FT8 + FT4, as it always was
    m.make = [] { return std::unique_ptr<Engine>(new Ft8Engine(false)); };
    return m;
}
inline ModeDef vibeDigiMode_ft4() {
    ModeDef m;
    m.name = "FT4"; m.slotSec = 7.5; m.sampleRate = 12000;
    m.audioLoHz = 100; m.audioHiHz = 3500;
    m.costVsFt8 = 1.0;   // ★ half the window, twice the slots: about FT8's average (the benchmark's FT8+FT4 row)
    m.lightCostVsFt8 = 0.6;
    m.priority = 20;
    m.singleFallback = true;
    m.make = [] { return std::unique_ptr<Engine>(new Ft8Engine(true)); };
    return m;
}

} // namespace digi
} // namespace vibe
