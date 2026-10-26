// decoders/digimodes/ft2.h — FT2 (IU8LMC's fast mode) as a digital-spots mode (decoders/digi_mode.h): Ft2Decoder
// (decoders/ft2_decoder.h — FT4's frame at 288 samples a symbol, 3.75 s slots) behind the Engine interface, mapped
// exactly as FT8's engine is (digimodes/ft8.h). docs/v12/JS8-FT2.md.
// ★ Cost (measured): ≈ 0.9× one FT8 engine. On real FT8 and FT4 audio (Single mode at those dials runs every Single
//   mode) it decodes nothing — the live decoder runs FT2 without OSD, which on that audio found only false codewords.
#pragma once
#include "../digi_mode.h"
#include "../ft2_decoder.h"

#include <memory>

namespace vibe {
namespace digi {

class Ft2Engine : public Engine {
public:
    Ft2Engine() : d_(12000) {
        d_.onSpot = [this](const std::string& to, const std::string& de, const std::string& grid, int snr, float hz) {
            (void)to;
            if (!onSpot) return;
            Spot s; s.mode = "FT2"; s.call = de; s.grid = grid; s.snr = snr; s.audioHz = hz;
            s.slotUtc = d_.decodingSlotUtc();   // ★ the slot's start
            onSpot(s);
        };
        d_.loadProbe = [this] { return loaded ? loaded() : false; };
    }
    void process(const float* audio, int n, double captureUtc) override { d_.process(audio, n, captureUtc); }
    uint64_t slotsSkipped() const override { return d_.slotsSkipped.load(); }
    uint64_t slotsDecoded() const override { return d_.slotsDecoded.load(); }
    Ft2Decoder& decoder() { return d_; }
private:
    Ft2Decoder d_;
};

inline ModeDef vibeDigiMode_ft2() {
    ModeDef m;
    m.name = "FT2";
    m.slotSec = 3.75;
    m.sampleRate = 12000;
    m.audioLoHz = 100; m.audioHiHz = 3500;   // ★ the FT8/FT4 monitor's search (f_min / f_max)
    m.costVsFt8 = 0.9;
    m.lightCostVsFt8 = 0.6;
    m.priority = 50;
    m.make = [] { return std::unique_ptr<Engine>(new Ft2Engine()); };
    return m;                          // dials: digi_bands.h's FT2 rows
}

} // namespace digi
} // namespace vibe
