// decoders/digimodes/wspr.h — WSPR-2 as a digital-spots mode (decoders/digi_mode.h): our own WsprDecoder
// (digimodes/wspr_decoder.h) behind the Engine interface, as digimodes/ft8.h does for FT8. The registry picks this file
// up by __has_include; the dials are digi_bands.h's "WSPR" rows; the map draws it "W" in reddish purple (digiModes.ts).
#pragma once
#include "../digi_mode.h"
#include "wspr_decoder.h"

#include <memory>

namespace vibe {
namespace digi {

/** One WsprDecoder: it decimates as audio arrives, cuts two-minute slots by the capture clock and decodes each on its
 *  own worker thread; a slot still decoding when the next completes is skipped and counted, never waited for. */
class WsprEngine : public Engine {
public:
    WsprEngine() : d_(12000) {
        d_.onSpot = [this](const WsprSpot& w) {
            // ★ an unresolved type-3 hash never reaches here (the engine drops it); the call has no brackets
            if (!onSpot || w.call.empty()) return;
            Spot s;
            s.mode = "WSPR"; s.call = w.call; s.grid = w.grid; s.text = w.message;
            s.snr = w.snr;                 // ★ the WSPRnet scale — what other receivers' reports of the station read
            s.audioHz = (float)w.audioHz; s.dt = w.dt;
            onSpot(s);
        };
        d_.loadProbe = [this] { return loaded ? loaded() : false; };
    }
    void process(const float* audio, int n, double captureUtc) override { d_.process(audio, n, captureUtc); }
    uint64_t slotsSkipped() const override { return d_.slotsSkipped.load(); }
    uint64_t slotsDecoded() const override { return d_.slotsDecoded.load(); }
    WsprDecoder& decoder() { return d_; }
private:
    WsprDecoder d_;
};

inline ModeDef vibeDigiMode_wspr() {
    ModeDef m;
    m.name = "WSPR"; m.slotSec = 120.0; m.sampleRate = 12000;
    /* ★ The engine searches 1380–1620 Hz and measures the WSPRnet-scale noise across ±150 Hz of 1500: Band mode's tuner
     *  is asked for 1350–1650 so all of that is flat passband, not the tuner's roll-off. */
    m.audioLoHz = 1350; m.audioHiHz = 1650;
    /* ★ Measured on the Mac on real busy 40 m, per two minutes: WSPR ≈ 480 ms (3 passes; Pi 2 ≈ 11–12 s, estimated from
     *  its instruction count), FT8 ≈ 84 ms (eight 15 s slots of 10.5 ms, ft8-bench, 3 passes). On a quiet band WSPR is
     *  ~20–170 ms. Most of it is the Fano search on candidates that are not signals. */
    m.costVsFt8 = 5.0;
    /* ★ Single mode feeds it only while the listener sits on a WSPR dial (±150 Hz): on the FT8 dial its 1400–1600 Hz
     *  is FT8 traffic — nothing to decode, all cost. */
    m.singleDialTolHz = 150;
    m.make = [] { return std::unique_ptr<Engine>(new WsprEngine()); };
    return m;   // dials: digi_bands.h's "WSPR" rows
}

} // namespace digi
} // namespace vibe
