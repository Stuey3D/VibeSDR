// decoders/tones_rx.h — V12 tone decoders: ONE receiver on a listener's 48 kHz audio running DTMF and 5-tone selective
// calling at once (docs/v12/TONES.md). 48 → 8 kHz (tones_dsp.h Decim6), then a 300 Hz high-pass (a 2nd-order Butterworth
// biquad: a CTCSS sub-tone, 67–254 Hz, rides under most NFM speech and would otherwise take a share of every block's
// energy), then each decoder that is switched on. Bounded: the decimated block of one feed() call is all it holds.
#pragma once
#include "decoders/tones_dsp.h"
#include "decoders/tones_dtmf.h"
#include "decoders/tones_fivetone.h"

#include <cmath>
#include <vector>

namespace vibe {
namespace tones {

class Rx {
public:
    Dtmf dtmf;
    FiveTone five;
    bool dtmfOn = true, fiveOn = true;
    bool scalar = false;   ///< the test's scalar run: every hot loop on its scalar path

    Rx() {
        // RBJ cookbook high-pass, Q = 1/√2, 300 Hz at 8 kHz
        const double w = 2 * M_PI * 300.0 / 8000.0, cw = std::cos(w), al = std::sin(w) / (2 * M_SQRT1_2);
        const double a0 = 1 + al;
        b0_ = (float)((1 + cw) / 2 / a0); b1_ = (float)(-(1 + cw) / a0); b2_ = b0_;
        a1_ = (float)(-2 * cw / a0); a2_ = (float)((1 - al) / a0);
    }
    /** 48 kHz mono float. */
    void feed(const float* x, int n) {
        y_.clear();
        dec_.process(x, n, y_, scalar);
        for (float& v : y_) {
            volatile float p0 = b0_ * v, p1 = b1_ * x1_, p2 = b2_ * x2_, q1 = a1_ * y1_, q2 = a2_ * y2_;
            const float o = (((p0 + p1) + p2) - q1) - q2;
            x2_ = x1_; x1_ = v; y2_ = y1_; y1_ = o;
            v = o;
        }
        if (y_.empty()) return;
        dtmf.scalar = five.scalar = scalar;
        if (dtmfOn) dtmf.feed(y_.data(), (int)y_.size());
        if (fiveOn) five.feed(y_.data(), (int)y_.size());
    }
    void flush() { if (dtmfOn) dtmf.flush(); if (fiveOn) five.flush(); }

private:
    Decim6 dec_;
    std::vector<float> y_;
    float b0_, b1_, b2_, a1_, a2_, x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

} // namespace tones
} // namespace vibe
