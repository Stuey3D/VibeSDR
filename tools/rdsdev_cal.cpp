// Derive the RDS-deviation calibration constant for RdsDemod::rdsDeviationKHz().
//
// Build and run:
//   V=android/app/src/main/cpp/vibedsp
//   clang++ -O2 -std=c++17 -I$V tools/rdsdev_cal.cpp $V/ddc.cpp -o /tmp/rdsdev_cal && /tmp/rdsdev_cal
//
// Result (2026-07-27): K = 1.520, stable to +/-0.2% from 192 to 320 kHz. This replaced a
// hard-coded 1.41421356 (a sinusoid's RMS->peak factor) that was never the right constant
// for this quantity. Validated against HansVanEijsden's Pira FM analyser over six Dutch
// stations; a ~1.3 dB residual remains and is believed to be real subcarrier loss in the
// WFM channel filter, NOT a scaling error — see rds.cpp.
//
// We synthesise a spec-shaped RDS subcarrier of KNOWN peak deviation, push it through the
// EXACT receive chain rds.cpp uses (designLowpass(2400/fs, 2400/fs) + decimating RealFir),
// and measure the mean envelope the way process() does. The constant we want is
// peak_deviation / (mean_envelope * 75).
#include "vibedsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <random>

using vibedsp::designLowpass;
using vibedsp::RealFir;

static const double FB = 1187.5;        // RDS bit rate
static const double FSC = 57000.0;      // subcarrier

// IEC 62106 transmit data-shaping filter: Ht(f) = cos(pi*f/(4*fb)) for |f| <= 2*fb.
// Impulse response by direct numerical integration (one-off, accuracy over speed).
static std::vector<double> shapingTaps(double fs, int halfLenBits) {
    const int half = (int)std::round(halfLenBits * fs / FB);
    std::vector<double> h(2 * half + 1);
    const int NF = 4000;
    const double fmax = 2.0 * FB, df = fmax / NF;
    for (int n = -half; n <= half; ++n) {
        const double t = n / fs;
        double acc = 0.0;
        for (int k = 0; k <= NF; ++k) {          // even spectrum -> 2*cos integral
            const double f = k * df;
            const double w = (k == 0 || k == NF) ? 0.5 : 1.0;
            acc += w * std::cos(M_PI * f / (4.0 * FB)) * std::cos(2.0 * M_PI * f * t) * df;
        }
        h[n + half] = 2.0 * acc;
    }
    return h;
}

static double calibrate(double fs, unsigned seed, double* kOut = nullptr) {
    const int nbits = 20000;
    std::mt19937 rng(seed);
    std::vector<int> bits(nbits);
    for (auto& b : bits) b = (rng() & 1) ? 1 : -1;

    // Every bit carries the SAME biphase symbol, so shape that one pulse and superpose it
    // — identical to convolving the whole stream, for a fraction of the work.
    const std::vector<double> h = shapingTaps(fs, 4);
    const int half = (int)(h.size() / 2);
    const int spb = (int)std::round(fs / FB);           // samples per bit
    std::vector<double> sym(spb);                        // +1 first half-bit, -1 second
    for (int i = 0; i < spb; ++i) sym[i] = (i < spb / 2) ? 1.0 : -1.0;
    std::vector<double> p(spb + h.size() - 1, 0.0);      // the shaped pulse
    for (int i = 0; i < spb; ++i)
        for (int k = 0; k < (int)h.size(); ++k) p[i + k] += sym[i] * h[k];

    const long nsamp = (long)nbits * spb;
    std::vector<double> s(nsamp + p.size(), 0.0);
    for (int n = 0; n < nbits; ++n) {
        const long off = (long)n * spb;
        for (int k = 0; k < (int)p.size(); ++k) s[off + k] += bits[n] * p[k];
    }
    s.resize(nsamp);

    // Normalise to unit PEAK — this is what an analyser calls the deviation.
    double pk = 0.0;
    for (long i = spb; i < nsamp - (long)p.size(); ++i) pk = std::max(pk, std::fabs(s[i]));
    for (auto& v : s) v /= pk;

    // Known peak deviation, in MPX units where 1.0 == 75 kHz.
    const double devKHz = 4.0;
    const double amp = devKHz / 75.0;

    // ── The receive chain, exactly as rds.cpp does it ────────────────────────
    const double cut = 2400.0 / fs;
    std::vector<float> taps = designLowpass(cut, cut);
    const int decim = std::max(1, (int)std::floor(fs / 40000.0));
    RealFir lpfI(taps, decim), lpfQ(taps, decim);

    std::vector<float> xI(nsamp), xQ(nsamp);
    for (long i = 0; i < nsamp; ++i) {
        const double t = i / fs;
        const double mpx = amp * s[i] * std::cos(2.0 * M_PI * FSC * t);
        xI[i] = (float)(mpx * std::cos(2.0 * M_PI * FSC * t) * 2.0);
        xQ[i] = (float)(mpx * -std::sin(2.0 * M_PI * FSC * t) * 2.0);
    }
    std::vector<float> oI(lpfI.maxOut(nsamp)), oQ(lpfQ.maxOut(nsamp));
    const int ns = std::min(lpfI.process(xI.data(), nsamp, oI.data()),
                            lpfQ.process(xQ.data(), nsamp, oQ.data()));

    // ★ BOTH constants. The mean envelope is what the old estimator used; the RMS is what the
    // noise-subtracting one needs, because noise removes in POWER and only an RMS is a power.
    const int skip = ns / 10;
    double sum = 0.0, sumSq = 0.0; long cnt = 0;
    for (int i = skip; i < ns; ++i) {
        const double m2 = (double)oI[i] * oI[i] + (double)oQ[i] * oQ[i];
        sum += std::sqrt(m2); sumSq += m2;
        ++cnt;
    }
    const double meanEnv = sum / cnt;
    const double rms     = std::sqrt(sumSq / cnt);
    if (kOut) *kOut = devKHz / (rms * 75.0);          // peak / RMS
    return devKHz / (meanEnv * 75.0);                 // peak / mean-envelope
}

int main() {
    printf("  fs (Hz)   decim   K peak/mean   K peak/RMS\n");
    for (double fs : {192000.0, 240000.0, 250000.0, 256000.0, 300000.0, 320000.0}) {
        double K = 0.0, Kr = 0.0;
        for (unsigned s = 1; s <= 3; ++s) { double kr = 0; K += calibrate(fs, s, &kr); Kr += kr; }
        K /= 3.0; Kr /= 3.0;
        printf("  %8.0f  %5d   %10.4f   %10.4f\n",
               fs, std::max(1, (int)std::floor(fs / 40000.0)), K, Kr);
    }
    return 0;
}
