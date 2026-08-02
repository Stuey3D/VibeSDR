// VibeSDR V5 — RealFFT + windows. Original VibeSDR code; FFT kernel = KissFFT.
#include "vibedsp.h"
#include "simd_internal.h"   // mulComplexReal (NEON window), powerToDb (fast log)
#include <cmath>
#include <cstring>

// KissFFT transforms (BSD-3). kiss_fft_scalar defaults to float.
#include "third_party/kissfft/kiss_fft.h"
#include "third_party/kissfft/kiss_fftr.h"

namespace vibedsp {

// ── ComplexFFT (IQ waterfall) ────────────────────────────────────────────--
ComplexFFT::ComplexFFT(int size, bool inverse) : n_(size) {
    cfg_ = kiss_fft_alloc(n_, inverse ? 1 : 0, nullptr, nullptr);
    in_.resize(n_);
    out_.resize(n_);
}

ComplexFFT::~ComplexFFT() {
    if (cfg_) kiss_fft_free((kiss_fft_cfg)cfg_);
}

void ComplexFFT::forward(const cf32* in, cf32* out) {
    kiss_fft((kiss_fft_cfg)cfg_,
             reinterpret_cast<const kiss_fft_cpx*>(in),
             reinterpret_cast<kiss_fft_cpx*>(out));
}

void ComplexFFT::powerDbShifted(const cf32* in, const float* win, float* outDb, float scale) {
    if (win) mulComplexReal(in, win, in_.data(), n_);          // NEON windowing
    else     std::copy(in, in + n_, in_.begin());
    forward(in_.data(), out_.data());
    // fftshift in two contiguous runs (no per-bin modulo): output bin j = raw bin
    // (j + n/2) % n. powerToDb uses a fast log2 (the waterfall is a uint8 display,
    // so the ~3e-4 error is invisible) instead of std::log10 — this runs over
    // millions of bins/sec across every mode.
    const int h = n_ / 2;
    const float* z = reinterpret_cast<const float*>(out_.data());
    for (int j = 0; j < h; ++j) {
        const int r = j + h;
        outDb[j] = powerToDb((z[2*r]*z[2*r] + z[2*r+1]*z[2*r+1]) * scale);
    }
    for (int j = h; j < n_; ++j) {
        const int r = j - h;
        outDb[j] = powerToDb((z[2*r]*z[2*r] + z[2*r+1]*z[2*r+1]) * scale);
    }
}

RealFFT::RealFFT(int size) : n_(size) {
    // inverse=0 (forward), no preallocated mem/lenmem -> KissFFT mallocs cfg.
    cfg_ = kiss_fftr_alloc(n_, 0, nullptr, nullptr);
    scratch_.resize(bins());
}

RealFFT::~RealFFT() {
    if (cfg_) kiss_fftr_free((kiss_fftr_cfg)cfg_);
}

void RealFFT::forward(const float* in, cf32* out) {
    // kiss_fft_cpx is {float r, i}; std::complex<float> is layout-compatible.
    kiss_fftr((kiss_fftr_cfg)cfg_,
              reinterpret_cast<const kiss_fft_scalar*>(in),
              reinterpret_cast<kiss_fft_cpx*>(out));
}

void RealFFT::powerDb(const float* in, float* outDb, float scale) {
    forward(in, scratch_.data());
    const int b = bins();
    for (int i = 0; i < b; ++i) {
        const float re = scratch_[i].real();
        const float im = scratch_[i].imag();
        outDb[i] = powerToDb((re * re + im * im) * scale);   // fast log2-based dB
    }
}

// 4-term Nuttall window (matches SDR++ IQFrontEnd NUTTALL).
void nuttallWindow(float* w, int n) {
    const double a0 = 0.355768, a1 = 0.487396, a2 = 0.144232, a3 = 0.012604;
    for (int i = 0; i < n; ++i) {
        const double t = 2.0 * M_PI * i / (n - 1);
        w[i] = (float)(a0 - a1 * std::cos(t) + a2 * std::cos(2 * t) - a3 * std::cos(3 * t));
    }
}

double windowCoherentGain(const float* w, int n) {
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += w[i];
    return s / n;
}

} // namespace vibedsp
