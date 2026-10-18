// firshape: the narrow-USB chain's filter shapes at the pipeline's own block sizes, old vs new kernel.
#include "vibedsp.h"
#include <chrono>
#include <cstdio>
#include <random>
using namespace vibedsp;
template <class F> double ns(F f, int reps, double samples) {
    auto t0 = std::chrono::steady_clock::now(); for (int r = 0; r < reps; ++r) f();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (reps * samples);
}
int main() {
    std::mt19937 rng(1); std::uniform_real_distribution<float> u(-1, 1);
    struct S { int K, D, n; bool cplx; } shapes[] = { {341, 101, 20480, true}, {1117, 2, 203, true}, {419, 1, 101, false},
                                                      {79, 4, 20480, true}, {1105, 2, 2048, true}, {149, 2, 203, true} };
    for (auto s : shapes) {
        std::vector<float> taps(s.K); for (auto& t : taps) t = u(rng) / s.K;
        std::vector<cf32> x(s.n); for (auto& v : x) v = cf32(u(rng), u(rng));
        std::vector<float> xr(s.n); for (auto& v : xr) v = u(rng);
        std::vector<cf32> y(s.n + 1); std::vector<float> yr(s.n + 1);
        FirDecimator fd(taps, s.D); RealFir rf(taps, s.D);
        const int reps = std::max(20, 4000000 / s.n);
        double t = s.cplx ? ns([&] { fd.process(x.data(), s.n, y.data()); }, reps, s.n)
                          : ns([&] { rf.process(xr.data(), s.n, yr.data()); }, reps, s.n);
        printf("%s K=%4d D=%3d n=%5d  %.2f ns/in\n", s.cplx ? "cplx" : "real", s.K, s.D, s.n, t);
    }
}
