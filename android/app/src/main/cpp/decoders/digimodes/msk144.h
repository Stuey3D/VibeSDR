// decoders/digimodes/msk144.h — MSK144 (meteor scatter, 6 m / 4 m / 2 m / 70 cm) as a digital-spots mode
// (decoders/digi_mode.h): our own msk144::Receiver (digimodes/msk144_decoder.h, clean room — docs/v12/DECODERS-WAVE2.md
// "MSK144") behind the Engine interface. V12, 2026-10-31.
//
// ★★ FREE-RUNNING. A meteor ping can come at any instant and last 20 ms or 2 s, so every block goes straight into the
//    receiver (no slots); it analyses 1.5 s windows a second apart. A spot is stamped with the ping's own time.
// ★ One report per message per 30 s (a QSO repeats its message every period; a long ping straddles two windows).
// ★ MSK40 short messages (hashed calls, mid-QSO) are not decoded — they carry no callsign to spot.
// ★ COST, measured (msk144-bench, M-series Mac, noise — the search runs on every window, decodes only on candidates):
//   ≈ 0.45 ms of analysis per second of audio plus the mix and decimation, ≈ 35 ms a minute in all → costVsFt8 0.8.
//   Pi 2: docs/v12/DECODERS-WAVE2.md "MSK144" (qemu instruction count). Loaded: half the candidates.
// ★ THE GATE. Band mode's activity gate judges a 2 s period's energy; a lone short weak ping does not light it. During a
//   shower the owner can set MSK144 to Always (no gate) — docs/v12/DIGITAL-SPOTS.md "The Band governor".
#pragma once
#include "../digi_mode.h"
#include "msk144_decoder.h"

#include <cmath>
#include <memory>

namespace vibe {
namespace digi {

class Msk144Engine : public Engine {
public:
    Msk144Engine() {
        rx_.onResult = [this](const msk144::Result& r) {
            if (!onSpot || r.call.empty()) return;          // ★ a spot is a call heard in full
            Spot s;
            s.mode = "MSK144"; s.call = r.call; s.grid = r.grid; s.text = r.text;
            s.snr = r.snr; s.audioHz = r.f0; s.dt = 0; s.slotUtc = r.utc;
            onSpot(s);
        };
    }
    void process(const float* audio, int n, double captureUtc) override {
        if (n <= 0) return;
        rx_.opts.loaded = loaded ? loaded() : false;
        /* ★ The receiver's clock is its sample count; utc0 says what UTC its sample 0 was. Re-said only when the capture
         *  clock moves a second away from it (Band's gate did not feed a stretch) or runs earlier. */
        if (std::isfinite(captureUtc)) {
            const double u0 = captureUtc - (rx_.audioSec() + (double)n / msk144::kRate);
            if (!std::isfinite(rx_.utc0) || u0 < rx_.utc0 - 0.05 || u0 - rx_.utc0 > 1.0) rx_.utc0 = u0;
        }
        rx_.push(audio, n);
        ++blocks_;
    }
    uint64_t slotsDecoded() const override { return rx_.stats.windows; }
    msk144::Receiver& receiver() { return rx_; }
private:
    msk144::Receiver rx_;
    uint64_t blocks_ = 0;
};

inline ModeDef vibeDigiMode_msk144() {
    ModeDef m;
    m.name = "MSK144";
    m.slotSec = 0;                    // ★ free-running
    m.sampleRate = 12000;
    m.audioLoHz = 200; m.audioHiHz = 2800;   // ★ 1500 ± (the main lobe ±1.2 kHz + the ±200 Hz search)
    m.costVsFt8 = 0.8;
    m.lightCostVsFt8 = 0.6;
    m.priority = 100;
    m.gateSec = 2.0;
    m.make = [] { return std::unique_ptr<Engine>(new Msk144Engine()); };
    return m;                         // dials: digi_bands.h's "MSK144" rows
}

} // namespace digi
} // namespace vibe
