// decoders/tone_id.cpp — see tone_id.h.
#include "tone_id.h"
#include "digimodes/vmath4.h"
#include "vibedsp/third_party/pffft/pffft.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vibe {

namespace {
constexpr int kFftN = 2048;
constexpr float kCtcssSnrDb = 15.0f;       // the peak over the 60–260 Hz median (noise peaks reach ≈ 9.5 dB)
constexpr float kCtcssLoneDb = 10.0f;      // …and over the band's second peak (a DCS word's line spectrum fails this)
constexpr double kDcsBaud = 134.4;
constexpr uint32_t kGolay = 0xC75;         // x¹¹+x¹⁰+x⁶+x⁵+x⁴+x²+1 (tone_id.h: how it was pinned)

uint32_t golayRem(uint32_t v) {
    for (int i = 22; i >= 11; --i) if ((v >> i) & 1u) v ^= kGolay << (i - 11);
    return v & 0x7FFu;
}
uint32_t dcsEncode(int code) {
    const uint32_t info = ((uint32_t)code & 0x1FFu) | (0x4u << 9);   // the 9-bit code under "100"
    return (info << 11) | golayRem(info << 11);
}
/** A 23-bit word (bit 22 first … as an integer) → the code it is, or −1. */
int dcsDecodeWord(uint32_t cw) {
    if (golayRem(cw) != 0) return -1;
    if (((cw >> 20) & 7u) != 4u) return -1;
    return (int)((cw >> 11) & 0x1FFu);
}
} // namespace

struct ToneId::Fft {
    PFFFT_Setup* setup = nullptr;
    float *in = nullptr, *out = nullptr, *work = nullptr;
    std::vector<float> win;
    Fft() {
        setup = pffft_new_setup(kFftN, PFFFT_REAL);
        in = (float*)pffft_aligned_malloc(sizeof(float) * kFftN);
        out = (float*)pffft_aligned_malloc(sizeof(float) * kFftN);
        work = (float*)pffft_aligned_malloc(sizeof(float) * kFftN);
        win.resize(kFftN);
        for (int i = 0; i < kFftN; ++i) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kFftN));
    }
    ~Fft() { pffft_destroy_setup(setup); pffft_aligned_free(in); pffft_aligned_free(out); pffft_aligned_free(work); }
};

const std::vector<float>& ToneId::ctcssTones() {
    static const std::vector<float> t = {67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f, 94.8f, 97.4f,
        100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f, 131.8f, 136.5f, 141.3f, 146.2f, 150.0f, 151.4f,
        156.7f, 159.8f, 162.2f, 165.5f, 167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f, 192.8f, 196.6f,
        199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f, 233.6f, 241.8f, 250.3f, 254.1f};
    return t;
}

const std::vector<int>& ToneId::dcsCodes() {
    static const std::vector<int> c = {23, 25, 26, 31, 32, 36, 43, 47, 51, 53, 54, 65, 71, 72, 73, 74, 114, 115, 116, 122,
        125, 131, 132, 134, 143, 145, 152, 155, 156, 162, 165, 172, 174, 205, 212, 223, 225, 226, 243, 244, 245, 246, 251,
        252, 255, 261, 263, 265, 266, 271, 274, 306, 311, 315, 325, 331, 332, 343, 346, 351, 356, 364, 365, 371, 411, 412,
        413, 423, 431, 432, 445, 446, 452, 454, 455, 462, 464, 465, 466, 503, 506, 516, 523, 526, 532, 546, 565, 606, 612,
        624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743, 754};
    return c;
}

static int octalOf(int digits) { return ((digits / 100) % 10) * 64 + ((digits / 10) % 10) * 8 + digits % 10; }
static int digitsOf(int octal) { return ((octal >> 6) & 7) * 100 + ((octal >> 3) & 7) * 10 + (octal & 7); }

uint32_t ToneId::dcsWord(int code) { return dcsEncode(octalOf(code)); }

std::string ToneId::State::label() const {
    char b[64];
    if (ctcss) { std::snprintf(b, sizeof b, "CTCSS %.1f Hz", ctcssHz); return b; }
    if (dcs) { std::snprintf(b, sizeof b, "DCS %03d%c (= %03d%c)", dcsCode, dcsInverted ? 'I' : 'N', dcsAlt, dcsInverted ? 'N' : 'I'); return b; }
    return "";
}

ToneId::ToneId(double rateHz) : rate_(rateHz), fft_(new Fft) {
    cicD_ = std::max(1, (int)std::lround(rate_ / 2000.0));
    const double r2 = rate_ / cicD_;                 // ≈ 2 kHz
    rate1_ = r2 / 2;
    // 63-tap windowed-sinc low-pass at 300 Hz (Blackman)
    const int K = 63;
    fir_.resize(K);
    const double fc = 300.0 / r2;
    double sum = 0;
    for (int i = 0; i < K; ++i) {
        const double m = i - (K - 1) / 2.0;
        const double s = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (K - 1)) + 0.08 * std::cos(4 * M_PI * i / (K - 1));
        fir_[(size_t)i] = (float)(s * w); sum += s * w;
    }
    for (float& v : fir_) v = (float)(v / sum);
    firHist_.assign((size_t)K, 0.f);
    low_.reserve((size_t)(rate1_ * 6));
}
ToneId::~ToneId() = default;

void ToneId::reset() {
    last_ = {1, 0}; i1_ = i2_ = i3_ = c1_ = c2_ = c3_ = 0; cicN_ = 0;
    std::fill(firHist_.begin(), firHist_.end(), 0.f); firPhase_ = 0;
    low_.clear(); lowTotal_ = lastCtcss_ = lastDcs_ = 0;
    candTone_ = -1; candRuns_ = missRuns_ = 0; candDcs_ = -1; candDcsRuns_ = dcsMiss_ = 0;
    publish(State());
}

void ToneId::feedIq(const std::complex<float>* x, int n) {
    using namespace q65v;
    if (n <= 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    disc_.resize((size_t)n);
    const float* xf = reinterpret_cast<const float*>(x);
    const float k = (float)(rate_ / (2 * M_PI));
    // ★ the first sample against the last block's last; then four at a time: p = x[n]·conj(x[n−1]), Hz = atan2·rate/2π
    {
        const std::complex<float> p = x[0] * std::conj(last_);
        V yv = set1(p.imag()), xv = set1(p.real());
        float l[4]; st(l, vatan2(yv, xv));
        disc_[0] = l[0] * k;
    }
    int i = 1;
    const V kv = set1(k);
    for (; i + 4 <= n; i += 4) {
        V ar, ai, br, bi;
        ld2(xf + 2 * i, ar, ai);
        ld2(xf + 2 * (i - 1), br, bi);
        const V pr = add(mul(ar, br), mul(ai, bi));
        const V pi = sub(mul(ai, br), mul(ar, bi));
        st(&disc_[(size_t)i], mul(vatan2(pi, pr), kv));
    }
    for (; i < n; i += 4) {   // the tail through the same lanes (identical arithmetic), padded
        float yr[4] = {0, 0, 0, 0}, yi[4] = {0, 0, 0, 0}, o[4];
        for (int q = 0; q < 4 && i + q < n; ++q) { const std::complex<float> p = x[i + q] * std::conj(x[i + q - 1]); yr[q] = p.real(); yi[q] = p.imag(); }
        st(o, mul(vatan2(ld(yi), ld(yr)), kv));
        for (int q = 0; q < 4 && i + q < n; ++q) disc_[(size_t)(i + q)] = o[q];
    }
    last_ = x[n - 1];
    ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    feedDisc(disc_.data(), n);
}

void ToneId::feedDisc(const float* d, int n) {
    const auto t0 = std::chrono::steady_clock::now();
    lowBand(d, n);
    const int hop = (int)std::lround(rate1_ * 0.5);
    while (lowTotal_ - lastCtcss_ >= hop) { lastCtcss_ += hop; if ((int)low_.size() >= kFftN) analyseCtcss(); }
    const int hopD = (int)std::lround(rate1_ * 1.0);
    while (lowTotal_ - lastDcs_ >= hopD) { lastDcs_ += hopD; analyseDcs(); }
    ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void ToneId::lowBand(const float* d, int n) {
    const double g = 1.0 / ((double)cicD_ * cicD_ * cicD_);
    const int K = (int)fir_.size();
    const size_t keep = (size_t)(rate1_ * 3.0);           // ★ the last 3 s are all an analysis reads
    for (int i = 0; i < n; ++i) {
        i1_ += d[i]; i2_ += i1_; i3_ += i2_;
        if (++cicN_ < cicD_) continue;
        cicN_ = 0;
        const double a = i3_ - c1_; c1_ = i3_;
        const double b = a - c2_; c2_ = a;
        const double c = b - c3_; c3_ = b;
        // the FIR's delay line (newest last), ÷2
        std::memmove(firHist_.data(), firHist_.data() + 1, sizeof(float) * (size_t)(K - 1));
        firHist_[(size_t)K - 1] = (float)(c * g);
        if (++firPhase_ < 2) continue;
        firPhase_ = 0;
        float acc = 0;
        for (int k = 0; k < K; ++k) acc += fir_[(size_t)k] * firHist_[(size_t)k];
        low_.push_back(acc);
        ++lowTotal_;
        if (low_.size() > 2 * keep) low_.erase(low_.begin(), low_.begin() + (long)(low_.size() - keep));
    }
    // ★ the integrators run in double and are re-centred now and then (a long session must not lose precision)
    if (std::fabs(i3_) > 1e12) { i1_ = i2_ = i3_ = c1_ = c2_ = c3_ = 0; }
}

void ToneId::publish(const State& s) {
    const bool changed = s.ctcss != st_.ctcss || s.dcs != st_.dcs || (s.ctcss && s.ctcssHz != st_.ctcssHz) || (s.dcs && (s.dcsCode != st_.dcsCode || s.dcsInverted != st_.dcsInverted));
    const bool first = !st_.ctcss && !st_.dcs && !s.ctcss && !s.dcs;
    st_ = s;
    if (changed && !first && onChange) onChange(st_);
}

void ToneId::analyseCtcss() {
    Fft& f = *fft_;
    const float* src = low_.data() + low_.size() - kFftN;
    for (int i = 0; i < kFftN; ++i) f.in[i] = src[i] * f.win[(size_t)i];
    pffft_transform_ordered(f.setup, f.in, f.out, f.work, PFFFT_FORWARD);
    const double binHz = rate1_ / kFftN;
    const int k0 = (int)std::ceil(60.0 / binHz), k1 = std::min(kFftN / 2 - 2, (int)std::floor(260.0 / binHz));
    std::vector<float> p((size_t)(k1 + 2));
    for (int k = 1; k <= k1 + 1; ++k) p[(size_t)k] = f.out[2 * k] * f.out[2 * k] + f.out[2 * k + 1] * f.out[2 * k + 1];
    int pk = k0;
    for (int k = k0; k <= k1; ++k) if (p[(size_t)k] > p[(size_t)pk]) pk = k;
    std::vector<float> rest;
    float second = 0;
    for (int k = k0; k <= k1; ++k) { if (std::abs(k - pk) <= 4) continue; rest.push_back(p[(size_t)k]); second = std::max(second, p[(size_t)k]); }
    std::nth_element(rest.begin(), rest.begin() + rest.size() / 2, rest.end());
    const float med = std::max(1e-20f, rest[rest.size() / 2]);
    const float snr = 10.f * std::log10(p[(size_t)pk] / med);
    const float lone = 10.f * std::log10(p[(size_t)pk] / std::max(1e-20f, second));
    int tone = -1; float meas = 0;
    if (snr >= kCtcssSnrDb && lone >= kCtcssLoneDb && pk > k0 && pk < k1) {
        const double a = std::log(p[(size_t)pk - 1] + 1e-30), b = std::log(p[(size_t)pk] + 1e-30), c = std::log(p[(size_t)pk + 1] + 1e-30);
        const double den = a - 2 * b + c;
        const double dl = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0.0;
        meas = (float)((pk + dl) * binHz);
        const std::vector<float>& T = ctcssTones();
        for (size_t j = 0; j < T.size(); ++j) {
            const float gapLo = j > 0 ? T[j] - T[j - 1] : 10.f, gapHi = j + 1 < T.size() ? T[j + 1] - T[j] : 10.f;
            const float tol = std::min(0.006f * T[j], 0.45f * std::min(gapLo, gapHi));
            if (std::fabs(meas - T[j]) <= tol) { tone = (int)j; break; }
        }
    }
    State s = st_;
    if (tone >= 0) {
        missRuns_ = 0;
        if (tone == candTone_) ++candRuns_; else { candTone_ = tone; candRuns_ = 1; }
        if (candRuns_ >= 3) { s.ctcss = true; s.ctcssHz = ctcssTones()[(size_t)tone]; s.measuredHz = meas; s.snrDb = snr; s.dcs = false; }
    } else {
        candRuns_ = 0; candTone_ = -1;
        if (++missRuns_ >= 5) s.ctcss = false;
    }
    publish(s);
}

void ToneId::analyseDcs() {
    const double spb = rate1_ / kDcsBaud;
    const int nbits = 23 * 12;                          // ≈ 2.05 s
    const int need = (int)std::ceil(nbits * spb) + 4;
    if ((int)low_.size() < need) return;
    const float* src = low_.data() + low_.size() - need;
    // the best of 16 bit phases: the eye's opening, Σ|soft|
    auto at = [&](double t) { const int i = (int)t; const float u = (float)(t - i); return i + 1 < need ? src[i] * (1 - u) + src[i + 1] * u : src[need - 1]; };
    double best = -1; int bestPh = 0;
    for (int ph = 0; ph < 16; ++ph) {
        double a = 0;
        for (int k = 0; k < nbits; ++k) a += std::fabs(at(1 + (k + ph / 16.0 + 0.5) * spb));
        if (a > best) { best = a; bestPh = ph; }
    }
    double fold[23] = {0};
    double mag = 0;
    for (int k = 0; k < nbits; ++k) { const float v = at(1 + (k + bestPh / 16.0 + 0.5) * spb); fold[k % 23] += v; mag += std::fabs(v); }
    // ★ the fold must be consistent: |Σ| over the repeats near Σ|·| (a real word repeats; noise does not)
    double fm = 0; for (double v : fold) fm += std::fabs(v);
    const bool steady = mag > 0 && fm / mag > 0.7;
    int found = -1; bool inv = false;
    if (steady) {
        uint32_t w = 0;
        for (int j = 0; j < 23; ++j) if (fold[j] > 0) w |= 1u << j;
        const std::vector<int>& codes = dcsCodes();
        for (int pol = 0; pol < 2 && found < 0; ++pol)
            for (int order = 0; order < 2 && found < 0; ++order)
                for (int sh = 0; sh < 23 && found < 0; ++sh) {
                    uint32_t b = pol ? (~w & 0x7FFFFFu) : w;
                    b = ((b >> sh) | (b << (23 - sh))) & 0x7FFFFFu;
                    uint32_t cw = 0;
                    if (order == 0) cw = b;                                  // LSB first
                    else for (int j = 0; j < 23; ++j) if ((b >> j) & 1u) cw |= 1u << (22 - j);
                    const int c = dcsDecodeWord(cw);
                    if (c < 0) continue;
                    const int dg = digitsOf(c);
                    if (std::find(codes.begin(), codes.end(), dg) == codes.end()) continue;
                    found = dg; inv = pol == 1;
                }
    }
    State s = st_;
    const int key = found < 0 ? -1 : found * 2 + (inv ? 1 : 0);
    if (key >= 0) {
        dcsMiss_ = 0;
        if (key == candDcs_) ++candDcsRuns_; else { candDcs_ = key; candDcsRuns_ = 1; }
        if (candDcsRuns_ >= 2 && !st_.ctcss) {
            s.dcs = true; s.dcsCode = found; s.dcsInverted = inv;
            // the other reading: the complement's code
            uint32_t w2 = ~dcsEncode(octalOf(found)) & 0x7FFFFFu; int alt = 0;
            for (int sh = 0; sh < 23 && !alt; ++sh) {
                const uint32_t b = ((w2 >> sh) | (w2 << (23 - sh))) & 0x7FFFFFu;
                const int c = dcsDecodeWord(b);
                if (c >= 0) { const int dg = digitsOf(c); if (std::find(dcsCodes().begin(), dcsCodes().end(), dg) != dcsCodes().end()) alt = dg; }
            }
            s.dcsAlt = alt;
        }
    } else {
        candDcsRuns_ = 0; candDcs_ = -1;
        if (++dcsMiss_ >= 3) s.dcs = false;
    }
    publish(s);
}

} // namespace vibe
