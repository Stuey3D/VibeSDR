// vibe_dab_fft.h — a radix-2 FFT sized for DAB.
//
// ★★ SELF-CONTAINED ON PURPOSE. Every vibe_dab_*.h is header-only and links nothing, which is what
//    has let each stage be tested on its own with a two-line g++ command. Pulling in vibedsp for
//    one transform would trade that for a build dependency; 40 lines of Cooley-Tukey does not.
//    ★ DAB's useful symbol is 2048 samples at the canonical rate — a power of two, which is not a
//      coincidence: the standard chose 2.048 MHz so the transform is radix-2.
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

/* ★★★ PFFFT WHERE THE BUILD LINKS IT, THE 40-LINE RADIX-2 WHERE IT DOES NOT.
 *
 *  simpleperf on the XCover decoding 7D, 2026-09-08: Fft::forward was 19.6 % of everything the
 *  two DAB threads did — the single largest symbol, above the Viterbi and above the rate
 *  converter after that was vectorised. A textbook radix-2 over std::complex<float> is exactly
 *  the transform PFFFT exists to replace: it is NEON/SSE throughout and 4.3x faster at N=32768
 *  on the Pi (fft.cpp's measurement), more at 2048 where the radix-2's twiddle stride hurts.
 *
 *  ★★ THE HEADER-ONLY RULE IS KEPT. Every vibe_dab_*.h still compiles with a two-line g++ and
 *     links nothing — the radix-2 stays as the default and as the REFERENCE. The app, the server
 *     and dab-offline define VIBE_DAB_PFFFT because they already compile pffft.c for vibedsp,
 *     so this adds no dependency they did not have. dab-offline decodes the same capture to
 *     the same frame counts either way, which is the check that the two agree.
 *  ★ Same convention both ways: forward = e^{-2πi kn/N}, natural order, unscaled. PFFFT wants
 *    16-byte-aligned buffers; a std::vector's data() is (max_align_t), and an unaligned caller
 *    is routed through the scratch rather than trusted. */
#if defined(VIBE_DAB_PFFFT)
#include "vibedsp/third_party/pffft/pffft.h"
#include <cstdint>
#include <cstring>
#endif

namespace vibedab {
class Fft {
public:
    explicit Fft(size_t n) : n_(n) {
        rev_.resize(n_);
        size_t bits = 0; while ((size_t(1) << bits) < n_) ++bits;
        for (size_t i = 0; i < n_; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b) if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
            rev_[i] = r;
        }
        tw_.resize(n_ / 2);
        for (size_t i = 0; i < n_ / 2; ++i) {
            const double a = -2.0 * M_PI * double(i) / double(n_);
            tw_[i] = std::complex<float>(float(std::cos(a)), float(std::sin(a)));
        }
#if defined(VIBE_DAB_PFFFT)
        // PFFFT needs N a multiple of 32 for complex transforms; DAB's 2048 (and the 256/512/1024
        // of Modes II-IV) all are. Anything else keeps the radix-2.
        if (n_ % 32 == 0) {
            setup_ = pffft_new_setup(int(n_), PFFFT_COMPLEX);
            work_  = static_cast<float*>(pffft_aligned_malloc(n_ * 2 * sizeof(float)));
            tmp_   = static_cast<float*>(pffft_aligned_malloc(n_ * 2 * sizeof(float)));
        }
#endif
    }
#if defined(VIBE_DAB_PFFFT)
    ~Fft() {
        if (setup_) pffft_destroy_setup(setup_);
        if (work_)  pffft_aligned_free(work_);
        if (tmp_)   pffft_aligned_free(tmp_);
    }
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;
#endif
    void forward(std::complex<float>* x) const {
#if defined(VIBE_DAB_PFFFT)
        if (setup_) {
            float* f = reinterpret_cast<float*>(x);
            if ((reinterpret_cast<uintptr_t>(f) & 15u) == 0) {
                pffft_transform_ordered(setup_, f, f, work_, PFFFT_FORWARD);     // in place
            } else {
                std::memcpy(tmp_, f, n_ * 2 * sizeof(float));
                pffft_transform_ordered(setup_, tmp_, tmp_, work_, PFFFT_FORWARD);
                std::memcpy(f, tmp_, n_ * 2 * sizeof(float));
            }
            return;
        }
#endif
        for (size_t i = 0; i < n_; ++i) if (i < rev_[i]) std::swap(x[i], x[rev_[i]]);
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t half = len >> 1, step = n_ / len;
            for (size_t i = 0; i < n_; i += len)
                for (size_t j = 0; j < half; ++j) {
                    const std::complex<float> u = x[i + j];
                    const std::complex<float> v = x[i + j + half] * tw_[j * step];
                    x[i + j]        = u + v;
                    x[i + j + half] = u - v;
                }
        }
    }
    size_t size() const { return n_; }
private:
    size_t n_;
    std::vector<size_t> rev_;
    std::vector<std::complex<float>> tw_;
#if defined(VIBE_DAB_PFFFT)
    PFFFT_Setup* setup_ = nullptr;
    float* work_ = nullptr;
    float* tmp_  = nullptr;
#endif
};

}  // namespace vibedab
