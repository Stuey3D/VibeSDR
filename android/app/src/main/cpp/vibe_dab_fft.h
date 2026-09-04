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

namespace vibedab {

class Fft {
public:
    explicit Fft(size_t n) : n_(n) {
        // Bit-reversal permutation and twiddles, built once.
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
    }

    /** In-place forward transform of `n` complex samples. */
    void forward(std::complex<float>* x) const {
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
};

}  // namespace vibedab
