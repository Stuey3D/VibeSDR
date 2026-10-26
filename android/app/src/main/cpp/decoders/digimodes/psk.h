// decoders/digimodes/psk.h — PSK31 / PSK63 SPOTS as a digital-spots mode (decoders/digi_mode.h): our own psk::Skimmer
// (decoders/psk_skimmer.h, docs/v12/PSK.md) behind the Engine interface. V12, 2026-10-27.
//
// ★★ SPOTS ONLY. The skimmer reads every PSK31 and PSK63 signal in the passband; here only its SPOTS are used — a
//    callsign heard identifying itself twice ("CQ CQ DE G4ABC G4ABC K", "TEST G4ABC", "… DE G4ABC") — never the text.
//    The listener's own PSK decoder (DECODERS → PSK) is where the text is read. The spot's mode is the lane that
//    decoded it, "PSK31" or "PSK63" (ModeDef::name "PSK" is the family: one engine, one map colour and letter).
// ★ Free-running (no slots): every block goes straight in. Text and spots are gated on decoded-text quality inside the
//   skimmer, so noise and FT8's crowd spot nothing (docs/v12/PSK.md: 0 characters in 10 min of noise, 0 on FT8/RTTY).
// ★ SNR: the skimmer measures Es/N0 (signal over noise in the baud rate's bandwidth); a spot carries WSJT-X's 2500 Hz
//   convention like every other mode's: −19 dB for PSK31, −16 dB for PSK63.
// ★ COST: one FFT per hop over the passband and a demodulator per carrier — ≈ 0.03 % of an M5 core on a busy 3 kHz
//   (≈ 0.6 of one FT8 engine). No lighter setting.
#pragma once
#include "../digi_mode.h"
#include "../psk_skimmer.h"

#include <cmath>
#include <memory>

namespace vibe {
namespace digi {

class PskEngine : public Engine {
public:
    PskEngine() {
        psk::Skimmer::Config c;
        c.rate = 12000; c.complexIn = false; c.loHz = 200; c.hiHz = 3600; c.maxSignals = 32;
        d_.reset(new psk::Skimmer(c));
        d_->onSpot = [this](const psk::Spot& p) {
            if (!onSpot || p.call.empty()) return;
            const bool p63 = p.mode == "BPSK63";
            Spot s;
            s.mode = p63 ? "PSK63" : "PSK31";
            s.call = p.call; s.how = p.how;
            s.text = p.how + " " + p.call;
            s.snr = (int)std::lround(p.snrDb - (p63 ? 16.0f : 19.0f));   // ★ Es/N0 → dB in 2500 Hz
            s.audioHz = (float)p.offsetHz;
            s.slotUtc = p.utcMs / 1000.0;
            onSpot(s);
        };
    }
    void process(const float* audio, int n, double captureUtc) override {
        if (n <= 0) return;
        d_->feedReal(audio, n);
        /* ★ The skimmer's clock is its sample count; setStartUtcMs says what NOW is. Re-said only when the capture clock
         *  moved a second away from it (audio Band's gate did not feed) or runs earlier (a stamp is late, never early). */
        if (std::isfinite(captureUtc)) {
            const int64_t cap = (int64_t)std::llround(captureUtc * 1000.0), now = d_->nowMs();
            if (!anchored_ || cap < now || cap - now > 1000) { d_->setStartUtcMs(cap); anchored_ = true; }
        }
    }
    psk::Skimmer& skimmer() { return *d_; }
private:
    std::unique_ptr<psk::Skimmer> d_;
    bool anchored_ = false;
};

inline ModeDef vibeDigiMode_psk() {
    ModeDef m;
    m.name = "PSK";                  // ★ the family; spots say "PSK31" / "PSK63"
    m.slotSec = 0;                   // ★ free-running
    m.sampleRate = 12000;
    m.audioLoHz = 150; m.audioHiHz = 3650;   // ★ the skimmer searches 200–3600 Hz; ±50 Hz more so its edges are flat
    m.costVsFt8 = 0.6;
    m.lightCostVsFt8 = 0.6;
    m.priority = 70;                 // ★ the governor's order: last of the modes built today (spots only)
    m.gateSec = 4.0;
    m.make = [] { return std::unique_ptr<Engine>(new PskEngine()); };
    return m;                        // dials: digi_bands.h's "PSK" rows
}

} // namespace digi
} // namespace vibe
