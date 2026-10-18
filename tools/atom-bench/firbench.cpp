// firbench: FirDecimator / RealFir accuracy vs a double-precision reference, and time per sample.
#include "vibedsp.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <cmath>
using namespace vibedsp;
int main(int argc, char** argv) {
    std::mt19937 rng(7); std::uniform_real_distribution<float> u(-1.f, 1.f);
    double worstC = 0, worstR = 0;
    for (int K : {7, 15, 31, 32, 33, 47, 63, 101, 137, 275}) for (int D : {1, 2, 3, 4, 5, 8}) {
        std::vector<float> taps(K); for (auto& t : taps) t = u(rng) / K;
        FirDecimator fd(taps, D); RealFir rf(taps, D);
        std::vector<cf32> x(20000); for (auto& v : x) v = cf32(u(rng), u(rng));
        std::vector<float> xr(20000); for (auto& v : xr) v = u(rng);
        std::vector<cf32> y(x.size()); std::vector<float> yr(x.size());
        int on = 0, onr = 0, pos = 0;
        while (pos < (int)x.size()) { int n = std::min((int)x.size() - pos, 1 + (int)(rng() % 3000));
            on += fd.process(x.data() + pos, n, y.data() + on); onr += rf.process(xr.data() + pos, n, yr.data() + onr); pos += n; }
        // reference: outputs at i = D-1, 2D-1, ... over the full stream with zero history
        int idx = 0;
        for (int i = D - 1; i < (int)x.size(); i += D, ++idx) {
            double re = 0, im = 0, r = 0, mag = 0, magr = 0;
            for (int k = 0; k < K; ++k) { int s = i - k; if (s < 0) break;
                re += (double)taps[k] * x[s].real(); im += (double)taps[k] * x[s].imag(); r += (double)taps[k] * xr[s];
                mag += std::fabs(taps[k]); magr += std::fabs(taps[k]); }
            if (idx < on) worstC = std::max(worstC, std::max(std::fabs(re - y[idx].real()), std::fabs(im - y[idx].imag())) / mag);
            if (idx < onr) worstR = std::max(worstR, std::fabs(r - yr[idx]) / magr);
        }
        if (idx != on || idx != onr) { printf("COUNT MISMATCH K=%d D=%d %d %d %d\n", K, D, idx, on, onr); return 1; }
    }
    printf("worst relative error: complex %.3g real %.3g (float eps %.3g)\n", worstC, worstR, 1.19e-7);
    // timing: typical WFM first stage and audio filter shapes
    for (auto [K, D] : {std::pair{31, 4}, {63, 8}, {113, 2}, {275, 1}, {419, 1}, {1117, 1}, {1117, 2}, {1117, 3}}) {
        std::vector<float> taps(K, 0.01f); FirDecimator fd(taps, D); RealFir rf(taps, D);
        std::vector<cf32> x(65536, cf32(0.1f, 0.2f)), y(65536); std::vector<float> xr(65536, 0.1f), yr(65536);
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < 200; ++r) fd.process(x.data(), 65536, y.data());
        auto t1 = std::chrono::steady_clock::now();
        for (int r = 0; r < 200; ++r) rf.process(xr.data(), 65536, yr.data());
        auto t2 = std::chrono::steady_clock::now();
        printf("K=%3d D=%d  complex %.2f ns/in  real %.2f ns/in\n", K, D,
               std::chrono::duration<double, std::nano>(t1 - t0).count() / (200 * 65536.0),
               std::chrono::duration<double, std::nano>(t2 - t1).count() / (200 * 65536.0));
    }
}
