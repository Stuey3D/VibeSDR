// VibeSDR V5 — FM stereo pilot PLL. Original VibeSDR code.
#include "vibedsp.h"
#include "simd_internal.h"
#include <cmath>

namespace vibedsp {

void StereoPLL::configure(double pilotHz, double rate) {
    w0_ = 2.0 * M_PI * pilotHz / rate;
    df_ = 0.0;
    phase_ = 0.0;
    // Second-order loop. Modest bandwidth (~one-thousandth of rate) for a clean
    // lock on the narrow pilot without excessive jitter.
    const double bw = w0_ * 0.01;            // loop bandwidth (rad/sample)
    const double zeta = 0.707;
    alpha_ = 2.0 * zeta * bw;
    beta_  = bw * bw;
    rotC_ = (float)std::cos(w0_);            // nominal per-sample rotation
    rotS_ = (float)std::sin(w0_);
    oscC_ = 1.0f; oscS_ = 0.0f; outC_ = 1.0f; outS_ = 0.0f; sinceNorm_ = 0;
    lockAmp_ = 0.0f;
    lockState_ = false; trackState_ = false;
    // ★★★ 110 ms, NOT 50 — AND THIS IS WHAT PAYS FOR THE LOWER ENGAGE THRESHOLD. A real pilot
    //     correlates COHERENTLY against the VCO, so its contribution to this average grows with N;
    //     noise correlates randomly and grows only as sqrt(N). Averaging longer therefore lowers
    //     the metric's noise floor faster than it lowers a weak pilot's reading, which is why a
    //     lower threshold here does NOT mean more stereo-on-static — the margin in sigmas is
    //     slightly better than it was. See kLockEngage for the arithmetic.
    // ★ The cost is acquisition time: 60 ms longer to declare stereo on a tune, which nobody can
    //   hear. The thing that WOULD be heard is a false lock, and that is what this protects.
    lockSmooth_ = (float)(1.0 / (0.11 * rate));
    // ★★★ ONE SECOND OF PATIENCE BEFORE DROPPING STEREO. Raising the engage threshold's sensitivity
    //     got H F M (4.7 kHz pilot) locking, but it still let go on brief dips: "spoke too soon,
    //     had a few drops in a row" (Stuart, 2026-08-14). On a fading signal the metric dips below
    //     the release level for a few hundred milliseconds at a time, and each dip is heard as a
    //     stutter between stereo and mono — far more objectionable than the noise it is avoiding.
    // ★★ SAFE BY CONSTRUCTION with respect to the static guarantee: this only makes UNLOCKING
    //    slower. Engaging still requires the full threshold and the full 110 ms averaging, and a
    //    signal that never locks cannot be held locked.
    releaseHold_ = (int)(rate * 1.0);
    belowRelease_ = 0;
}

// One PLL sample. Returns the oscillator's cos/sin via oscC_/oscS_ and keeps
// phase_/cycle_ in step for the RDS bit clock. No trigonometry: the oscillator
// is advanced by a complex multiply.
void StereoPLL::advance(float mpx) {
    const float s = oscS_, c = oscC_;
    outS_ = s; outC_ = c;               // references are taken at THIS phase
    // Phase detector: pilot * quadrature. When locked, cos(phase) aligns with the
    // pilot, so the error is mpx * (-sin) averaged.
    const double err = (double)mpx * (double)(-s);
    df_ += beta_ * err;
    const double d = df_ + alpha_ * err;     // this sample's deviation from w0_

    // Bookkeeping phase, integrated from the SAME increments as the rotator, so
    // the RDS bit clock stays exactly consistent with the oscillator without ever
    // needing an atan2 to recover the angle.
    phase_ += w0_ + d;
    if (phase_ >= 2.0 * M_PI) { phase_ -= 2.0 * M_PI; cycle_ = (cycle_ + 1) & 15; }
    else if (phase_ < 0.0)    { phase_ += 2.0 * M_PI; cycle_ = (cycle_ + 15) & 15; }

    // Rotate by exp(j*(w0 + d)) = exp(j*w0) * exp(j*d). |d| is tiny — the loop
    // bandwidth is 1% of the pilot frequency — so exp(j*d) is a second-order
    // small-angle expansion, well below float precision for any realistic error.
    const float dd = (float)d;
    const float cd = 1.0f - 0.5f * dd * dd, sd = dd;
    const float stepC = rotC_ * cd - rotS_ * sd;
    const float stepS = rotS_ * cd + rotC_ * sd;
    oscC_ = c * stepC - s * stepS;
    oscS_ = s * stepC + c * stepS;
    // Renormalise occasionally: one Newton step on 1/|osc|, no square root.
    if (++sinceNorm_ >= 512) {
        sinceNorm_ = 0;
        const float g = 1.5f - 0.5f * (oscC_ * oscC_ + oscS_ * oscS_);
        oscC_ *= g; oscS_ *= g;
    }

    // ★★★ A NaN HERE IS FOREVER. Every state in this loop is recursive, so one
    // non-finite sample — from anywhere upstream — multiplies through every
    // subsequent one and never leaves. The audible result is silence plus frozen
    // RDS while the spectrum (which does not pass through here) carries on
    // perfectly, and only a restart clears it. There are no other isnan guards in
    // the DSP, so this is the one place that can catch it cheaply: the check costs
    // a compare per pilot sample and turns a permanent failure into a glitch.
    //
    // ★ Re-seed rather than zero: a zeroed oscillator would sit dead, whereas this
    // re-acquires exactly as it does at start-up.
    if (!std::isfinite(oscC_) || !std::isfinite(oscS_) || !std::isfinite(lockAmp_)) {
        oscC_ = 1.0f; oscS_ = 0.0f; sinceNorm_ = 0;
        lockAmp_ = 0.0f; lockState_ = false; trackState_ = false; belowRelease_ = 0;
        return;
    }

    // Lock metric: slowly-smoothed in-phase pilot energy (mpx correlated with
    // cos), then a hysteretic state so static can't toggle stereo.
    lockAmp_ += lockSmooth_ * (mpx * c * 2.0f - lockAmp_);
    if (!lockState_ && lockAmp_ > kLockEngage) {
        lockState_ = true;
        belowRelease_ = 0;
    } else if (lockState_) {
        // ★ Count TIME under the release level, and only then let go — see releaseHold_.
        if (lockAmp_ < kLockRelease) {
            if (++belowRelease_ >= releaseHold_) { lockState_ = false; belowRelease_ = 0; }
        } else {
            belowRelease_ = 0;
        }
    }
    if (!trackState_ && lockAmp_ > kTrackEngage)      trackState_ = true;
    else if (trackState_ && lockAmp_ < kTrackRelease) trackState_ = false;
}

// Phase-coherent references from the oscillator's sin/cos via exact angle
// identities (no extra trig). The PLL locks cos(phase) onto the SINE pilot, so
// the L-R subcarrier — sin(2*pilot) per the FM stereo standard — is in phase with
// -sin(2*phase) = -2*s*c (NOT cos(2*phase), which is quadrature -> cancels the
// whole difference signal). RDS keeps cos(3*phase): its bi-phase decoder is
// phase-tolerant, so it already works.
void StereoPLL::step(float mpx, float* ref38, float* ref57, float* bitClk) {
    advance(mpx);
    if (ref38) *ref38 = -2.0f * outS_ * outC_;                    // -sin(2*phase)
    if (ref57) *ref57 = outC_ * (4.0f * outC_ * outC_ - 3.0f);    // cos(3*phase)
    // RDS bit clock (1187.5 Hz = pilot/16) derived directly from the pilot phase
    // + cycle counter, so it inherits the PLL's stability without integral drift.
    if (bitClk) *bitClk = (float)((cycle_ * 2.0 * M_PI + phase_) / 16.0);
}

void StereoPLL::processBlock(const float* mpx, int n, float* lmr,
                             float* ref57, float* ref57q, float* bitClk) {
    // The loop filter is a feedback path, so this cannot be vectorised across
    // samples — but it no longer has to be: with the trig gone each iteration is
    // a handful of multiplies. The vectorised work either side of it (the FM
    // discriminator feeding this, the stereo matrix consuming it) is where NEON
    // pays. Splitting the RDS references out of the loop keeps the common case —
    // stereo playing, no RDS subscriber — down to the L-R detection alone.
    if (ref57 && ref57q && bitClk) {
        for (int i = 0; i < n; ++i) {
            advance(mpx[i]);
            const float c = outC_, s = outS_;
            lmr[i]   = mpx[i] * (-2.0f * s * c) * 2.0f;   // detect + coherent gain comp
            // Triple-angle identities — the third harmonic of the pilot, still from the
            // one sin/cos pair, no extra trig.
            ref57[i]  = c * (4.0f * c * c - 3.0f);        // cos(3*phase)
            ref57q[i] = s * (3.0f - 4.0f * s * s);        // sin(3*phase)
            bitClk[i] = (float)((cycle_ * 2.0 * M_PI + phase_) / 16.0);
        }
    } else {
        for (int i = 0; i < n; ++i) {
            advance(mpx[i]);
            lmr[i] = mpx[i] * (-2.0f * outS_ * outC_) * 2.0f;
        }
    }
}

} // namespace vibedsp
