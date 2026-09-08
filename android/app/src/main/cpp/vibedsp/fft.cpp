// VibeSDR V5 — RealFFT + windows. Original VibeSDR code; FFT kernel = PFFFT, KissFFT as reference.
//
// ★★★ THE ONE HOT KERNEL WITH NO SIMD BEHIND IT. The windowing and the dB conversion in this file
//     have been NEON since V5; the transform between them was a bare kiss_fft(), which is a scalar
//     reference implementation. On a shared receiver the channelizer's forward FFT is the single
//     largest line in the DSP budget — the server's own split reports "forward FFT 43%" of real
//     time with nobody even listening.
//
// ★★★ MEASURED ON THE PI, SAME BOX, SAME LOAD, N = 32768 COMPLEX:
//         KissFFT   1582 us      PFFFT   366 us      = 4.3x
//         at N=8192:  276 us              43 us      = 6.4x
//     PFFFT is NEON on aarch64 and SSE on x86-64 — the same two targets simd_internal.h already
//     covers, and the same argument: no runtime dispatch, no fat binary, and it reaches the old
//     machines VibeServer is meant to run on.
//
// ★★ KISSFFT STAYS, AND NOT ONLY AS A FALLBACK. PFFFT's complex transform needs a size that is a
//    multiple of 32 built from factors 2, 3 and 5; a narrow channel's inverse can be smaller than
//    that, so kiss still runs those. It is also the REFERENCE the new path is checked against
//    (test_cfft compares the two bin for bin), which is the same discipline VIBE_FORCE_SCALAR
//    gives the NEON kernels.
//
// ★ Licence: PFFFT carries the FFTPACK/NCAR licence — three-clause BSD in substance, permissive,
//   and compatible both with GPL3 and with the app-store exception this project ships under.
#include "vibedsp.h"
#include "simd_internal.h"   // mulComplexReal (NEON window), powerToDb (fast log)
#include <cmath>
#include <cstring>

// KissFFT transforms (BSD-3). kiss_fft_scalar defaults to float.
#include "third_party/kissfft/kiss_fft.h"
#include "third_party/kissfft/kiss_fftr.h"
#include "third_party/pffft/pffft.h"
#include <cstdint>

namespace vibedsp {

// ── ComplexFFT (IQ waterfall) ────────────────────────────────────────────--
/** ★ PFFFT's complex transform is defined for N a multiple of 32 whose only factors are 2, 3
 *  and 5. Every size this engine uses for the waterfall and the channelizer's forward transform
 *  qualifies; a narrow channel's INVERSE can be smaller, and those keep the kiss path. */
static bool pffftCanDo(int n) {
    if (n < 32 || (n % 32) != 0) return false;
    int m = n;
    while ((m % 2) == 0) m /= 2;
    while ((m % 3) == 0) m /= 3;
    while ((m % 5) == 0) m /= 5;
    return m == 1;
}

ComplexFFT::ComplexFFT(int size, bool inverse) : n_(size), inverse_(inverse) {
    if (pffftCanDo(n_)) {
        pf_ = pffft_new_setup(n_, PFFFT_COMPLEX);
        if (pf_) {
            /* ★ PFFFT reads and writes through SIMD loads, so every buffer it touches must be
             *  16-byte aligned. A caller's std::vector base is, but `out` may be an INTERIOR
             *  pointer (the channelizer writes each channel into a slice of a larger buffer), so
             *  the alignment is TESTED per call and these are the landing ground when it fails.
             *  ★ The work buffer is mandatory and must not be shared between instances — one per
             *    ComplexFFT, like the kiss config it replaces. */
            work_ = pffft_aligned_malloc((size_t)n_ * 2 * sizeof(float));
            alignedIn_  = pffft_aligned_malloc((size_t)n_ * 2 * sizeof(float));
            alignedOut_ = pffft_aligned_malloc((size_t)n_ * 2 * sizeof(float));
            if (!work_ || !alignedIn_ || !alignedOut_) { freePffft(); }
        }
    }
    if (!pf_) cfg_ = kiss_fft_alloc(n_, inverse ? 1 : 0, nullptr, nullptr);
    in_.resize(n_);
    out_.resize(n_);
}

void ComplexFFT::freePffft() {
    if (work_)       { pffft_aligned_free(work_);       work_ = nullptr; }
    if (alignedIn_)  { pffft_aligned_free(alignedIn_);  alignedIn_ = nullptr; }
    if (alignedOut_) { pffft_aligned_free(alignedOut_); alignedOut_ = nullptr; }
    if (pf_)         { pffft_destroy_setup((PFFFT_Setup*)pf_); pf_ = nullptr; }
}

ComplexFFT::~ComplexFFT() {
    if (cfg_) kiss_fft_free((kiss_fft_cfg)cfg_);
    freePffft();
}

void ComplexFFT::forward(const cf32* in, cf32* out) {
    if (pf_) {
        /* ★ Neither transform normalises its inverse — kiss and pffft agree on that — so callers
         *  that already scale by 1/N keep working unchanged. */
        const float* src = reinterpret_cast<const float*>(in);
        float*       dst = reinterpret_cast<float*>(out);
        const size_t bytes = (size_t)n_ * 2 * sizeof(float);
        const bool inOk  = ((uintptr_t)src & 15u) == 0;
        const bool outOk = ((uintptr_t)dst & 15u) == 0;
        const float* s = src;
        if (!inOk) { std::memcpy(alignedIn_, src, bytes); s = (const float*)alignedIn_; }
        float* d = outOk ? dst : (float*)alignedOut_;
        pffft_transform_ordered((PFFFT_Setup*)pf_, s, d, (float*)work_,
                                inverse_ ? PFFFT_BACKWARD : PFFFT_FORWARD);
        if (!outOk) std::memcpy(dst, alignedOut_, bytes);
        return;
    }
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
