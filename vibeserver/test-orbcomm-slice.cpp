// test-orbcomm-slice — ORBCOMM through the SERVER'S OWN SLICE PATHS at an Airspy HF+'s rates (V12 2026-11-24: beta93 on
// Stuart's XCover, HF+ at 912 kS/s, float IQ — a pick of FM27 showed "0 good · 0 bad" and no carrier at all, while the RTL
// at 1.024 / 2.4 MS/s decoded). FM116/FM27-like carriers at 137.6625 MHz and at 137.800 MHz (+3 kHz Doppler: half of it
// above the listed 137.2–137.8 MHz band) in a capture centred on 137.500 MHz, cut exactly as the shim cuts them:
//   · the SHARED path (feedSkimFromBins): the capture's forward FFT (fftSizeForRate), the slice = bandSliceBins of
//     min(iqWidth, 0.8 × rate), extracted with the sixteenth's roll-off;
//   · the ONE-PIPELINE path (a personal receiver — the Android app's own radio): vibe::WideTap with Band's flat sizing.
// at 912 / 768 / 456 kS/s and 1.024 MS/s (the RTL reference), the dial on 137.500 and moved to 137.800, at RTL-like and
// HF+-like (tiny float) amplitudes. Silent.
#include "vibe_orbcomm_tx.h"
#include "vibe_band_slice.h"
#include "vibe_wide_tap.h"

#include <chrono>
#include <cstdio>
#include <thread>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibeorb;
using cf32 = vibedsp::cf32;

static int fftSizeForRate(double rate) { double want = rate / 75.0; int s = 4096; while (s < (int)want && s < 32768) s *= 2; return s; }

struct Res { int ok137662 = 0, ok137800 = 0, after = 0; };

static std::vector<std::complex<float>> capture(double fs, double centre, double scale, int frames) {
    std::vector<uint8_t> a, b;
    for (int f = 0; f < frames; f++) {
        auto fa = tx::minorFrame(0x2E, 0x09, f, {}); a.insert(a.end(), fa.begin(), fa.end());
        auto fb = tx::minorFrame(0x2E, 0x40, f, {}); b.insert(b.end(), fb.begin(), fb.end());
    }
    tx::Signal s1; s1.fs = fs; s1.offsetHz = 137.6625e6 - 1500 - centre; s1.slopeHzS = -25; s1.esn0Db = 11; s1.seed = 3; s1.clockPpm = 20;
    tx::Signal s2 = s1; s2.offsetHz = 137.800e6 + 3000 - centre; s2.seed = 4; s2.esn0Db = 50; s2.amp = 0.3f;   // the noise comes once (s1)
    auto x = tx::modulate(a, s1), y = tx::modulate(b, s2);
    for (size_t i = 0; i < x.size() && i < y.size(); i++) x[i] = (x[i] + y[i]) * float(scale);
    return x;
}

/** The shared path: what feedSkimFromBins hands the decoder host (dial = `dial`, capture centre 137.5 MHz). */
static Res viaShared(const std::vector<std::complex<float>>& x, double fs, double dial, double widthHz, double dial2 = 0) {
    const double hwC = 137.5e6;
    const int fft = fftSizeForRate(fs);
    vibedsp::Channelizer ch(fft);
    vibedsp::Channelizer::ExtractCtx ctx;
    const double w = std::min(widthHz, fs * 0.8);
    const int b = vibe::bandSliceBins(fs, fft, w);
    const double binHz = fs / fft;
    int cb = int(std::lround((dial - hwC) / binHz));
    bool moved = false;
    std::vector<cf32> slice((size_t)b);
    Receiver::Cfg cfg; cfg.windowHz = std::max(10000.0, widthHz / 2); Receiver r(cfg);
    Res res; long long blk = 0;
    r.onPacket = [&](const Rx& p) { if (!p.p.ok) return; if (moved) res.after++; if (std::fabs(p.rfHz - 137.6625e6) < 6000) res.ok137662++; if (std::fabs(p.rfHz - 137.800e6) < 6000) res.ok137800++; };
    double t = 0;
    for (size_t i = 0; i < x.size(); i += 8192) {
        const int n = int(std::min<size_t>(8192, x.size() - i));
        if (dial2 > 0 && !moved && i > x.size() / 2) { moved = true; cb = int(std::lround((dial2 - hwC) / binHz)); }   // ★ the listener tunes mid-pick
        ch.feed(reinterpret_cast<const cf32*>(&x[i]), n, [&](const cf32* bins, int) {
            const int gs = ch.extract(bins, cb, b, slice.data(), ctx, blk++, vibe::kBandTaperDiv);
            const double rate = fs * (double)b / fft;
            if (gs > 0) { r.feed(reinterpret_cast<const std::complex<float>*>(slice.data()), gs, rate, hwC + cb * binHz, t); t += gs / rate; }
        });
    }
    return res;
}

/** The one-pipeline path: vibe::WideTap with the shim's skimTapFeed geometry. */
static Res viaTap(const std::vector<std::complex<float>>& x, double fs, double dial, double widthHz, double dial2 = 0) {
    vibe::WideTap tap;
    Receiver::Cfg cfg; cfg.windowHz = std::max(10000.0, widthHz / 2); Receiver r(cfg);
    Res res; double t = 0;
    std::atomic<bool> moved{false};
    r.onPacket = [&](const Rx& p) { if (!p.p.ok) return; if (moved) res.after++; if (std::fabs(p.rfHz - 137.6625e6) < 6000) res.ok137662++; if (std::fabs(p.rfHz - 137.800e6) < 6000) res.ok137800++; };
    tap.start(vibe::WideTap::BandSink([&](const cf32* s, int k, double rate, double centre, const float*, int, double, double, int slice) {
        if (slice != 0 || !s) return;
        r.feed(reinterpret_cast<const std::complex<float>*>(s), k, rate, centre, t); t += k / rate;
    }));
    vibe::WideTap::Geometry g;
    g.sampleRate = fs; g.hwCentreHz = 137.5e6; g.wantCentreHz = dial; g.widthHz = std::min(widthHz, fs * 0.8); g.fftSize = fftSizeForRate(fs); g.band = true;
    tap.aim(g);
    for (size_t i = 0; i < x.size(); i += 8192) {
        if (dial2 > 0 && !moved && i > x.size() / 2) {   // ★ the listener tunes mid-pick: the tap re-aims at the new dial
            while (tap.queuedSamples() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            moved = true; g.wantCentreHz = dial2; tap.aim(g);
        }
        tap.feed(reinterpret_cast<const cf32*>(&x[i]), int(std::min<size_t>(8192, x.size() - i)), 1.0e9 + double(i) / fs);
        while (tap.queuedSamples() > size_t(fs * 0.2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (tap.queuedSamples() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tap.stop();
    return res;
}

/** ★★ THE BAND PICK ON A ONE-PIPELINE RADIO WITH THE PASS'S DOPPLER STEPS (the XCover, 2026-11-24: a 640 kHz band pick opened
 *  nothing while a 40 kHz one decoded). Every second the satellite lock moves the dial by the Doppler step and the hardware
 *  LO follows it (the worst case: the capture itself shifts, as a retune does) — the tap re-plans, the slice's centre moves.
 *  Decoding must keep flowing all through and both carriers lock. */
static Res viaTapSteps(const std::vector<std::complex<float>>& x0, double fs, double widthHz, double stepHz) {
    vibe::WideTap tap;
    Receiver::Cfg cfg; cfg.windowHz = std::max(10000.0, widthHz / 2); Receiver r(cfg);
    Res res; double t = 0; int sec = 0; std::vector<int> perSec(200, 0);
    r.onPacket = [&](const Rx& p) { if (!p.p.ok) return; if (sec < 200) perSec[size_t(sec)]++; if (std::fabs(p.rfHz - 137.6625e6) < 6000) res.ok137662++; if (std::fabs(p.rfHz - 137.800e6) < 6000) res.ok137800++; };
    tap.start(vibe::WideTap::BandSink([&](const cf32* s, int k, double rate, double centre, const float*, int, double, double, int slice) {
        if (slice != 0 || !s) return;
        r.feed(reinterpret_cast<const std::complex<float>*>(s), k, rate, centre, t); t += k / rate;
    }));
    vibe::WideTap::Geometry g;
    g.sampleRate = fs; g.hwCentreHz = 137.5e6; g.wantCentreHz = 137.5e6; g.widthHz = std::min(widthHz, fs * 0.8); g.fftSize = fftSizeForRate(fs); g.band = true;
    tap.aim(g);
    std::vector<std::complex<float>> buf(8192);
    double lo = 0, ph = 0;   // the LO's move from 137.5 MHz: the capture is the RF seen from the moved LO
    for (size_t i = 0; i < x0.size(); i += 8192) {
        const int n = int(std::min<size_t>(8192, x0.size() - i));
        const int nowSec = int(double(i) / fs);
        if (nowSec != sec) {   // the lock's step: dial and LO move together
            sec = nowSec; lo += stepHz;
            while (tap.queuedSamples() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            g.hwCentreHz = 137.5e6 + lo; g.wantCentreHz = 137.5e6 + lo; tap.aim(g);
        }
        const double w = -2 * M_PI * lo / fs;
        for (int k = 0; k < n; k++) { buf[size_t(k)] = x0[i + size_t(k)] * std::complex<float>(float(std::cos(ph)), float(std::sin(ph))); ph += w; }
        ph = std::fmod(ph, 2 * M_PI);
        tap.feed(reinterpret_cast<const cf32*>(buf.data()), n, 1.0e9 + double(i) / fs);
        while (tap.queuedSamples() > size_t(fs * 0.2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (tap.queuedSamples() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tap.stop();
    int quiet = 0;   // whole seconds after the first 3 with no packet at all
    for (int k = 3; k < sec; k++) if (perSec[size_t(k)] == 0) quiet++;
    res.after = quiet;
    return res;
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    const int frames = quick ? 8 : 12;
    const double width = 640000;   // vibe_sat_engine.h: a band pick's slice (137.2–137.8 + kOrbNarrowHz)
    for (double fs : { 1024000.0, 912000.0, 768000.0, 456000.0 }) {
        for (double scale : { 1.0, 1e-4 }) {
            auto x = capture(fs, 137.5e6, scale, frames);
            for (double dial : { 137.5e6, 137.8e6 }) {
                const Res a = viaShared(x, fs, dial, width), b = viaTap(x, fs, dial, width);
                std::printf("   %4.0f kS/s scale %-6g dial %.3f: shared 137.6625 %4d / 137.800 %4d · tap %4d / %4d\n", fs / 1000, scale, dial / 1e6,
                            a.ok137662, a.ok137800, b.ok137662, b.ok137800);
                const int need = frames * 50 / 3;
                // what the capture holds: both carriers are within ±0.4 of the rate of 137.5 at ≥ 768 kS/s; at 456 kS/s only
                // what the dial's slice reaches inside the capture
                const bool holds662 = std::fabs(137.6625e6 - 137.5e6) < fs * 0.42, holds800 = std::fabs(137.803e6 - 137.5e6) < fs * 0.42;
                if (holds662) CHECK(a.ok137662 > need && b.ok137662 > need, "%.0f k scale %g dial %.3f: 137.6625 decodes (shared %d, tap %d)", fs / 1000, scale, dial / 1e6, a.ok137662, b.ok137662);
                if (holds800) CHECK(a.ok137800 > need && b.ok137800 > need, "%.0f k scale %g dial %.3f: 137.800 (+3 kHz) decodes (shared %d, tap %d)", fs / 1000, scale, dial / 1e6, a.ok137800, b.ok137800);
            }
        }
    }
    {   // ★★ a band pick (640 kHz) at 912 kS/s, one-pipeline, the dial + LO stepping by the Doppler every second
        const int secs = quick ? 12 : 30;
        auto x = capture(912000, 137.5e6, 1e-4, secs);
        const Res st = viaTapSteps(x, 912000, width, -30);
        std::printf("   912 kS/s band pick, LO + dial stepping \u221230 Hz/s: 137.6625 %d / 137.800 %d packets, %d silent seconds\n", st.ok137662, st.ok137800, st.after);
        CHECK(st.ok137662 > secs * 50 * 2 / 3 && st.ok137800 > secs * 50 * 2 / 3, "band pick with Doppler steps: both carriers decode (%d / %d)", st.ok137662, st.ok137800);
        CHECK(st.after == 0, "IQ keeps flowing: no silent second after the first 3 (%d)", st.after);
    }
    // ★★ A MANUAL DIAL MOVE MID-PICK (Stuart tuned to 137.800 by hand): the slice follows the dial; decoding must go on
    for (double fs : { 912000.0, 1024000.0 }) {
        auto x = capture(fs, 137.5e6, 1e-4, frames);
        const Res a = viaShared(x, fs, 137.5e6, width, 137.8e6), b = viaTap(x, fs, 137.5e6, width, 137.8e6);
        std::printf("   %4.0f kS/s dial 137.500 \u2192 137.800 mid-pick: after the move shared %d, tap %d packets\n", fs / 1000, a.after, b.after);
        const int need = frames * 50 / 3;
        CHECK(a.after > need && b.after > need, "%.0f k: decoding continues after a manual dial move (shared %d, tap %d)", fs / 1000, a.after, b.after);
    }
    std::printf("test-orbcomm-slice: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
