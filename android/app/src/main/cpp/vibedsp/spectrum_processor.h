// VibeSDR — SpectrumProcessor: the waterfall/spectrum display mapping, in C++.
//
// ★★★ A LINE-BY-LINE PORT OF src/assets/signalProcessor.ts (M9PSY's compression maths + UberSDR's
//   updateAutoRange), so the native spectrum engine (docs/NATIVE-SPECTRUM-ENGINE.md) draws EXACTLY what
//   the JS did. The why behind every constant and step lives in the TypeScript, beside the code it explains
//   — read it there; this file keeps only the notes a port needs.
// ★★ BIT-FOR-BIT WITH THE JS, which is what the host test (test/test_spectrum_processor.cpp, fed the same
//   frames as the TS) checks:
//   · buffers are float (a JS Float32Array) but every expression is evaluated in DOUBLE and rounded to float
//     on the store, exactly as JS numbers do;
//   · Math.round is floor(x + 0.5); `x | 0` truncates toward zero; Math.sign(0) is 0;
//   · the clock is passed IN (nowMs) — the JS read Date.now() — so a test can replay frames deterministically.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

namespace vibedsp {

struct SpectrumProcessorSettings {
    double autoContrast    = 5;      // dB, floor +N / ceiling −N
    double minRangeDb      = 30;     // the display window never shrinks below this
    bool   manual          = false;  // manual range (wfCoarse='manual'): auto-range bypassed
    double manualMinDb     = -120;
    double manualMaxDb     = -20;
    double specFloor       = 0;      // −20…+20 dB, spectrum trace only
    double specPeakScale   = 10;     // ×0.1
    double smoothingFrames = 5;      // 1 = instant
    double avgFrames       = 0;      // OWRX-style EMA weight 0…0.9
    bool   spatialSmooth   = true;
    bool   peakHold        = true;
    double wfBrightness    = 0;      // dB, waterfall only
    double wfContrast      = 0;      // range-affecting (flush) only — applied in the shader
    double wfSharpness     = 0;      // range-affecting (flush) only — applied in the shader
};

/** One processed frame. Views into the processor's own buffers — valid until the next process(). */
struct SpectrumFrameOut {
    const uint8_t* row  = nullptr;   // normalised intensity 0–255 (Gray_8 waterfall texture)
    const float*   spec = nullptr;   // normalised [0,1] trace
    const float*   peak = nullptr;   // normalised [0,1] peak hold (zeros when off)
    int            n    = 0;
    double         dbMin = -120, dbMax = -20;
};

class SpectrumProcessor {
public:
    /** Patch settings. A change to a range-affecting setting flushes history (as applySettings in the TS). */
    void applySettings(const SpectrumProcessorSettings& s);
    const SpectrumProcessorSettings& settings() const { return s_; }

    /** Process one raw dB frame (any length; it may change between frames). */
    SpectrumFrameOut process(const float* bins, int n, double centerHz, double bwHz, bool interacting, double nowMs);

    void resetPeakHold() { peakLine_.clear(); hasPeak_ = false; }
    void reset();
    double dbMin() const { return actualMinDb_; }
    double dbMax() const { return actualMaxDb_; }

private:
    double slewFloor(double want, double now);

    SpectrumProcessorSettings s_;
    std::vector<float> dbAvg_, specSmooth_, peakLine_, tmp_, normRow_, outSpec_, outPeak_;
    std::vector<uint8_t> outRow_;
    bool haveBuffers_ = false;   // the TS's `this.dbAvg !== null`
    bool hasPeak_ = false;       // the TS's `this.peakLine !== null`

    struct Sample { double value, ts; };
    std::deque<Sample> minHistory_, maxHistory_;
    double lastMaxAt_ = 0, lastMinAt_ = 0;
    bool adoptRange_ = false;
    double lastCenterHz_ = 0, lastBwHz_ = 0;
    bool haveFrozenFloor_ = false;
    double frozenFloorDb_ = 0;
    double actualMinDb_ = -120, actualMaxDb_ = -20;
    uint32_t dbHist_[300] = {};

    double lastFrameMs_ = 0, prevCenterHz_ = 0;
    int settingsVer_ = 0, settingsVerApp_ = 0;
};

}  // namespace vibedsp
