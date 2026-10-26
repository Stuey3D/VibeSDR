// decoders/digimodes/js8.h — JS8 as a digital-spots mode (decoders/digi_mode.h): Js8Decoder (js8_decoder.h) behind the
// Engine interface, all four submodes (Normal, Fast, Turbo, Slow) on one dial at once. docs/v12/JS8-FT2.md.
//
// ★★ A SPOT ONLY FOR A SENDER. JS8 frames are heartbeats, CQs, directed frames ("G4ABC: M0XYZ SNR -12") and pieces of
//    text; only a frame that carries its sender's real callsign becomes a spot (Js8Spot::isSpot: heartbeat, CQ,
//    compound, directed — never a group such as @ALLCALL, never a text fragment). The grid only from a heartbeat/CQ or a
//    compound frame, which carry the sender's own; a directed frame's addressee is the other station, never spotted.
// ★ The mode is "JS8" for every submode (one colour, one letter on the map); the submode is in the spot's text.
// ★ Cost (measured, docs/v12/JS8-FT2.md): all four submodes ≈ 1.8× one FT8 engine at full depth on a quiet band,
//   ≈ 0.6× when the server is loaded (one pass, no coherent stage, no OSD — `loaded`). On FT8's own busy audio
//   (Single mode at an FT8 dial runs every Single mode) it decodes nothing — 21 minutes of real FT8, 0 frames.
#pragma once
#include "../digi_mode.h"
#include "js8_decoder.h"

#include <memory>

namespace vibe {
namespace digi {

class Js8Engine : public Engine {
public:
    Js8Engine() : d_(12000, kJs8AllSubmodes) {
        d_.onDecode = [this](const Js8Spot& j) {
            if (!onSpot || !j.isSpot()) return;
            Spot s;
            s.mode = "JS8";
            s.call = j.frame.from;
            const bool ownGrid = j.frame.kind == js8::FrameKind::Heartbeat || j.frame.kind == js8::FrameKind::Cq
                              || j.frame.kind == js8::FrameKind::Compound;
            if (ownGrid) s.grid = j.frame.grid;
            std::string sub = js8Info(j.submode).name;                  // "NORMAL" → "Normal"
            for (size_t i = 1; i < sub.size(); ++i) sub[i] = (char)(sub[i] - 'A' + 'a');
            s.text = j.frame.text + " (" + sub + ")";
            s.snr = j.snr; s.audioHz = j.audioHz; s.dt = j.dt;
            s.slotUtc = j.slotUtc;              // ★ its submode's slot start
            onSpot(s);
        };
        d_.loadProbe = [this] { return loaded ? loaded() : false; };
    }
    void process(const float* audio, int n, double captureUtc) override { d_.process(audio, n, captureUtc); }
    uint64_t slotsSkipped() const override { return d_.slotsSkipped.load(); }
    uint64_t slotsDecoded() const override { return d_.slotsDecoded.load(); }
    Js8Decoder& decoder() { return d_; }
private:
    Js8Decoder d_;
};

inline ModeDef vibeDigiMode_js8() {
    ModeDef m;
    m.name = "JS8";
    m.slotSec = 15.0;                  // ★ Normal's; the engine cuts Fast (10 s), Turbo (6 s) and Slow (30 s) itself
    m.sampleRate = 12000;
    m.audioLoHz = 100; m.audioHiHz = 3300;   // ★ what Js8SlotDecoder's monitor searches (f_min / f_max)
    m.costVsFt8 = 1.8;
    m.make = [] { return std::unique_ptr<Engine>(new Js8Engine()); };
    return m;                          // dials: digi_bands.h's JS8 rows (JS8Call's defaults)
}

} // namespace digi
} // namespace vibe
