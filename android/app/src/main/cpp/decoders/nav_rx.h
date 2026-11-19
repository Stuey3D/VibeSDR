// decoders/nav_rx.h — V12 VOR bearing and ILS localiser / glideslope deviation from a listener's channel IQ. Our own receiver,
// from the published signal definitions (ICAO Annex 10 Vol. I §3.1 ILS and §3.3 VOR, as described in the public literature).
// docs/v12/VOR-ILS.md.
//
// ★★★ WHAT THE SIGNALS ARE
//   VOR (108–117.975 MHz, AM): the carrier is amplitude-modulated by
//     • the VARIABLE 30 Hz (≈ 30 % depth) whose phase, seen from the receiver, lags by the magnetic bearing FROM the station;
//     • a 9960 Hz SUBCARRIER (≈ 30 %) frequency-modulated ±480 Hz by the REFERENCE 30 Hz, the same at every bearing;
//     • the ident, Morse on 1020 Hz (≈ 10 %), and sometimes voice.
//     The radial = phase(reference) − phase(variable). A Doppler VOR swaps which part the transmitter makes "variable" and
//     turns the other way, so what reaches the receiver is the same signal: one receiver for both.
//   ILS localiser (108.10–111.95 MHz, odd tenths) and glideslope (328.6–335.4 MHz, paired): AM by 90 Hz and 150 Hz tones.
//     The DIFFERENCE IN DEPTH OF MODULATION (DDM = m90 − m150) is zero on the course / glide path. Localiser: 90 Hz
//     predominates LEFT of the course seen from an approaching aircraft (so "fly right"); glideslope: 90 Hz above the path
//     ("fly down"). Nominal depths: localiser 20 % + 20 %, glideslope 40 % + 40 %; full scale 0.155 (LOC), 0.175 (GS) DDM.
//
// ★★ THE CHAIN — every measured phase goes through the SAME filters, so no filter can bias a bearing:
//   at the channel rate fs:  e = |z| (the true envelope — |z|² would mix speech into the subcarrier); three products of e:
//     low (e itself), sub (e − dc)·LO(−9960 Hz), id (e − dc)·LO(−1020 Hz); one Hamming low-pass (flat to ±650 Hz) decimates
//     all five real streams by D to fs1 ≈ 3.1 kHz.
//   at fs1:  the subcarrier's FM discriminator (its half-sample delay is put back analytically), the ident's narrow
//     low-pass and keying envelope; a second low-pass (identical for the discriminator and the envelope) decimates by 4
//     to fs2 ≈ 780 Hz.
//   at fs2:  one-second Hann-windowed correlations at 30 / 90 / 150 Hz against ONE time base → depths, deviation, phases.
//   The `dc` high-pass sits only on the subcarrier and ident paths (it moves 9960 Hz by 0.006°; the 30 Hz variable never
//   passes through it).
//
// ★★ WHEN A BEARING IS BELIEVED (0 false bearings on noise, on plain airband AM speech, on an ILS):
//   a second is a VOR second when the discriminator's 30 Hz holds ≥ 50 % of its variance (coherence), the deviation is
//   300–650 Hz (480 ± 16 nominal, room for receiver filters), the subcarrier depth 10–50 %, the variable depth 10–50 %;
//   a BEARING is reported after 3 such seconds running whose bearings agree (circular σ ≤ 4°), as their vector average.
//   An ILS second: 90 + 150 Hz carry ≥ 60 % of the 20–390 Hz envelope variance, each ≥ 2 %, no VOR subcarrier; reported
//   after 3 running whose DDM agrees within 0.02.
//
// ★ SIMD (docs/v12/DECODER-SIMD-AUDIT.md): the two hot loops at fs — the envelope (four |z| a lane: SSE2 / AArch64 sqrt,
//   ARMv7 NEON lane-wise sqrtf — every path the correctly-rounded IEEE square root) and the decimating dot products
//   (q65_simd.h, products in order, (l0+l1)+(l2+l3)) — have scalar twins with the same operations in the same order;
//   test-vor checks them BIT FOR BIT. Everything after decimation is a few thousand operations a second, scalar.
// ★ Memory: fixed — the FIR histories (2 × taps × 5 streams), one second of fs2 samples, 30 s of 100 Hz keying.
#pragma once
#include "decoders/digimodes/q65_simd.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace nav {

inline const char* simdName() {
#if Q65V_NEON
    return "NEON";
#elif Q65V_SSE2
    return "SSE2";
#else
    return "scalar";
#endif
}

// ── the hot loops ─────────────────────────────────────────────────────────────────────────────────────────────────
/** |z| for n interleaved complex floats. Scalar twin: the same IEEE operations (no fused multiply-add). */
inline void envelopeScalar(const float* iq, int n, float* out) {
    for (int i = 0; i < n; ++i) {
        volatile float a = iq[2 * i] * iq[2 * i], b = iq[2 * i + 1] * iq[2 * i + 1];
        out[i] = std::sqrt(a + b);
    }
}
inline void envelope(const float* iq, int n, float* out) {
#if Q65V_NEON || Q65V_SSE2
    using namespace q65v;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        V re, im; ld2(iq + 2 * i, re, im);
        const V p = add(mul(re, re), mul(im, im));
#if Q65V_SSE2
        st(out + i, _mm_sqrt_ps(p));
#elif defined(__aarch64__)
        st(out + i, vsqrtq_f32(p));
#else
        float l[4]; st(l, p);
        for (int k = 0; k < 4; ++k) out[i + k] = std::sqrt(l[k]);
#endif
    }
    if (i < n) envelopeScalar(iq + 2 * i, n - i, out + i);
#else
    envelopeScalar(iq, n, out);
#endif
}
/** Σ x[t]·h[t], t < taps (a multiple of 4): four lanes, (l0+l1)+(l2+l3). */
inline float dotScalar(const float* x, const float* h, int taps) {
    volatile float l0 = x[0] * h[0], l1 = x[1] * h[1], l2 = x[2] * h[2], l3 = x[3] * h[3];
    for (int t = 4; t < taps; t += 4) {
        volatile float p0 = x[t] * h[t], p1 = x[t + 1] * h[t + 1], p2 = x[t + 2] * h[t + 2], p3 = x[t + 3] * h[t + 3];
        l0 = l0 + p0; l1 = l1 + p1; l2 = l2 + p2; l3 = l3 + p3;
    }
    return (l0 + l1) + (l2 + l3);
}
inline float dot(const float* x, const float* h, int taps) {
#if Q65V_NEON || Q65V_SSE2
    using namespace q65v;
    V a = mul(ld(x), ld(h));
    for (int t = 4; t < taps; t += 4) a = add(a, mul(ld(x + t), ld(h + t)));
    return hsum(a);
#else
    return dotScalar(x, h, taps);
#endif
}

/** A Hamming-windowed sinc low-pass, unity gain at DC, `taps` rounded up to a multiple of 4 (odd-length centre kept by
 *  a zero last tap, so the group delay is (taps − 2) / 2 for the even padded length — returned in `delay`). */
inline std::vector<float> lowpass(double fcOverFs, int taps, double& delay) {
    taps = std::max(8, (taps + 3) / 4 * 4);
    const int n = taps - 1;            // odd, symmetric
    std::vector<float> h((size_t)taps, 0.f);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double m = i - (n - 1) / 2.0;
        const double s = m == 0 ? 2 * fcOverFs : std::sin(2 * M_PI * fcOverFs * m) / (M_PI * m);
        const double w = 0.54 - 0.46 * std::cos(2 * M_PI * i / (n - 1));
        h[(size_t)i] = (float)(s * w); sum += s * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    delay = (n - 1) / 2.0;
    return h;
}

/** ★ One-second figures — the test's window into the receiver, and what the run decides from. */
struct Second {
    double carrier = 0;        ///< mean envelope (input units)
    double var30 = 0;          ///< variable 30 Hz depth (0..1)
    double sub = 0;            ///< 9960 Hz subcarrier depth (0..1)
    double dev = 0;            ///< reference 30 Hz deviation (Hz)
    double refCoh = 0;         ///< share of the discriminator's variance in its 30 Hz
    double bearing = NAN;      ///< this second's bearing (deg, 0..360), NAN when not a VOR second
    double m90 = 0, m150 = 0;  ///< ILS tone depths (0..1)
    double ilsCoh = 0;         ///< share of the 20–390 Hz envelope variance in 90 + 150 Hz
    bool vor = false, ils = false;
};

/** ★ What is reported once a second (a believed result, or nothing). */
struct Report {
    enum Kind { None = 0, Vor = 1, Ils = 2 } kind = None;
    double bearing = NAN;      ///< VOR radial FROM the station, magnetic as transmitted (deg)
    double sigma = NAN;        ///< circular spread of the last seconds' bearings (deg)
    double ddm = 0, sdm = 0;   ///< ILS: m90 − m150, m90 + m150
    int seconds = 0;           ///< seconds running in this state
    Second last;
};

/** The ident's keying, 100 samples a second, read into words; a word = the run between pauses ≥ max(7 dots, 1.2 s). */
class IdentKeyer {
public:
    std::function<void(const std::string& word, double wpm)> onWord;
    void push(float p) {
        hist_.push_back(p);
        if (hist_.size() > 3000) hist_.pop_front();   // 30 s
        if (++sinceLevels_ >= 50 || hist_.size() < 200) { sinceLevels_ = 0; levels_(); }
        const bool on = keyed_ && p > thr_;
        run(on);
    }
    bool keyed() const { return keyed_; }
    double snrDb() const { return snrDb_; }
private:
    void levels_() {
        if (hist_.size() < 200) { keyed_ = false; return; }
        std::vector<float> v(hist_.begin(), hist_.end());
        const size_t n = v.size();
        std::nth_element(v.begin(), v.begin() + n / 5, v.end());
        const double lo = v[n / 5];
        std::nth_element(v.begin(), v.begin() + n * 95 / 100, v.end());
        const double hi = v[n * 95 / 100];
        snrDb_ = (lo > 0 && hi > 0) ? 10 * std::log10(hi / lo) : 0;
        keyed_ = snrDb_ > 8.0;                 // marks stand ≥ 8 dB (power) over the spaces
        thr_ = (float)std::sqrt(std::max(lo, 1e-30) * hi);   // half-way in dB
    }
    void run(bool on) {
        if (on == cur_) { ++len_; if (!on && len_ == wordGap_() && !els_.empty()) endWord_(); return; }
        // debounce: a run under 30 ms is absorbed into the one before
        if (len_ < 3 && !els_.empty()) { cur_ = on; len_ += els_.back().second; els_.pop_back(); ++len_; return; }
        if (len_ > 0) els_.push_back({ cur_, len_ });
        if (els_.size() > 200) els_.clear();
        cur_ = on; len_ = 1;
    }
    int wordGap_() const { return std::max(120, (int)std::lround(7 * dot_)); }
    void endWord_() {
        // marks of this word, dots learnt from them: the shortest-mark cluster (2-means, ratio 2–4.5) else the last dot
        std::vector<int> marks;
        for (auto& e : els_) if (e.first) marks.push_back(e.second);
        if (marks.empty()) { els_.clear(); return; }
        double a = *std::min_element(marks.begin(), marks.end()), b = *std::max_element(marks.begin(), marks.end());
        if (b > 1.8 * a) {
            for (int it = 0; it < 8; ++it) {
                double sa = 0, sb = 0; int na = 0, nb = 0;
                for (int m : marks) { if (std::fabs(m - a) <= std::fabs(m - b)) { sa += m; ++na; } else { sb += m; ++nb; } }
                if (na) a = sa / na;
                if (nb) b = sb / nb;
            }
            if (b / a >= 2.0 && b / a <= 4.5) dot_ = 0.5 * dot_ + 0.5 * a;
        } else if (std::fabs(a - dot_) > 0.5 * dot_ && std::fabs(a - 3 * dot_) < std::fabs(a - dot_)) {
            // all one length and nearer a dash: all dashes ("MOT"), the dot stays as learnt
        } else {
            dot_ = 0.7 * dot_ + 0.3 * a;
        }
        std::string word;
        unsigned code = 1;
        bool ok = true;
        auto flush = [&] {
            if (code > 1) {
                const char* t = morse(code);
                if (!t) ok = false; else word += t;
            }
            code = 1;
        };
        for (size_t i = 0; i < els_.size(); ++i) {
            const auto& e = els_[i];
            if (e.first) {
                code = code * 2 + (e.second > 2.0 * dot_ ? 1u : 0u);
                if (code >= 512) { ok = false; code = 1; }
            } else if (i > 0 && e.second > 2.0 * dot_) {
                flush();
            }
        }
        flush();
        els_.clear();
        if (!ok || word.empty() || word.size() > 6) return;
        if (onWord) onWord(word, 1.2 / (dot_ / 100.0));
    }
    /** Morse tree index → letter or figure (ITU-R M.1677): 1 is the root, dot ×2, dash ×2+1. */
    static const char* morse(unsigned c) {
        static const char* const t[64] = {
            nullptr, nullptr, "E", "T", "I", "A", "N", "M", "S", "U", "R", "W", "D", "K", "G", "O",
            "H", "V", "F", nullptr, "L", nullptr, "P", "J", "B", "X", "C", "Y", "Z", "Q", nullptr, nullptr,
            "5", "4", nullptr, "3", nullptr, nullptr, nullptr, "2", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "1",
            "6", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "7", nullptr, nullptr, nullptr, "8", nullptr, "9", "0" };
        return c < 64 ? t[c] : nullptr;
    }
    std::deque<float> hist_;
    std::vector<std::pair<bool, int>> els_;
    bool cur_ = false, keyed_ = false;
    int len_ = 0, sinceLevels_ = 0;
    float thr_ = 0;
    double snrDb_ = 0, dot_ = 17.0;   // samples (10 ms) — 7 WPM, ICAO's nominal
};

/** ★ The receiver. Feed complex channel IQ at `fs` (≥ 24 kHz: the subcarrier sits at 9.96 ± 0.5 kHz). */
class NavRx {
public:
    std::function<void(const Report&)> onSecond;
    IdentKeyer keyer;

    explicit NavRx(double fs, bool forceScalar = false) : fs_(fs), scalar_(forceScalar) {
        D1_ = std::max(1, (int)std::lround(fs / 3125.0));
        fs1_ = fs / D1_;
        const double pass = 650.0, stop = fs1_ - 650.0;
        int taps = (int)std::ceil(3.3 * fs / std::max(200.0, stop - pass)) + 1;
        h1_ = lowpass(0.5 * (pass + stop) / fs, taps, delay1_);
        T1_ = (int)h1_.size();
        fs2_ = fs1_ / kD2;
        const double p2 = 200.0, s2 = fs2_ - 200.0;
        h2_ = lowpass(0.5 * (p2 + s2) / fs1_, (int)std::ceil(3.3 * fs1_ / (s2 - p2)) + 1, delay2_);
        T2_ = (int)h2_.size();
        for (auto& b : hist1_) b.assign((size_t)(2 * T1_), 0.f);
        for (auto& b : hist2_) b.assign((size_t)(2 * T2_), 0.f);
        stepSub_ = (uint32_t)std::llround(9960.0 / fs * 4294967296.0);
        stepId_ = (uint32_t)std::llround(1020.0 / fs * 4294967296.0);
        static const std::vector<float> cosT = [] { std::vector<float> t(kLut); for (int i = 0; i < kLut; ++i) t[(size_t)i] = (float)std::cos(2 * M_PI * i / kLut); return t; }();
        lut_ = cosT.data();
        dcA_ = (float)(1.0 - std::exp(-2 * M_PI * 1.0 / fs));
        n2_ = (int)std::lround(fs2_);   // one second
        win_.resize((size_t)n2_);
        for (int i = 0; i < n2_; ++i) win_[(size_t)i] = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / n2_);
        idLen_ = std::max(4, (int)std::lround(fs1_ / 60.0));   // ≈ 17 ms moving average: the ident's ±50 Hz
        idHop_ = std::max(1, (int)std::lround(fs1_ / 100.0));
    }
    double rate() const { return fs_; }
    double rate1() const { return fs1_; }
    double rate2() const { return fs2_; }

    void feed(const std::complex<float>* x, int n) {
        if (n <= 0) return;
        env_.resize((size_t)n);
        if (scalar_) envelopeScalar(reinterpret_cast<const float*>(x), n, env_.data());
        else envelope(reinterpret_cast<const float*>(x), n, env_.data());
        for (int i = 0; i < n; ++i) {
            const float e = env_[(size_t)i];
            dc_ += dcA_ * (e - dc_);
            const float ac = e - dc_;
            const float cs = lut_[phSub_ >> kShift], sn = -lut_[(phSub_ - 0x40000000u) >> kShift];
            const float ci = lut_[phId_ >> kShift], si = -lut_[(phId_ - 0x40000000u) >> kShift];
            phSub_ += stepSub_; phId_ += stepId_;
            const float s[5] = { e, ac * cs, ac * sn, ac * ci, ac * si };
            for (int k = 0; k < 5; ++k) { hist1_[k][(size_t)pos1_] = s[k]; hist1_[k][(size_t)(pos1_ + T1_)] = s[k]; }
            if (++pos1_ == T1_) pos1_ = 0;
            if (++ph1_ < D1_) continue;
            ph1_ = 0;
            float y[5];
            for (int k = 0; k < 5; ++k) {
                const float* w = &hist1_[k][(size_t)pos1_];
                y[k] = scalar_ ? dotScalar(w, h1_.data(), T1_) : dot(w, h1_.data(), T1_);
            }
            stage1_(y);
        }
    }
    /** For the SIMD test: the decimated streams so far are summed into this (bit-exactness check). */
    double checksum() const { return check_; }

private:
    static constexpr int kD2 = 4, kLut = 4096, kShift = 20;
    void stage1_(const float* y) {
        check_ += (double)y[0] + y[1] + y[2] + y[3] + y[4];
        // the subcarrier's FM discriminator (Hz); its value belongs half a sample back (put right in the phase)
        const std::complex<double> z(y[1], y[2]);
        const std::complex<double> p = z * std::conj(zPrev_);
        zPrev_ = z;
        const double f = std::arg(p) * fs1_ / (2 * M_PI);
        subMag_ += std::abs(z);
        // the ident: a moving average of the 1020 Hz product, its power every 10 ms
        idRe_ += y[3]; idIm_ += y[4];
        idBuf_.push_back({ y[3], y[4] });
        if ((int)idBuf_.size() > idLen_) { idRe_ -= idBuf_.front().first; idIm_ -= idBuf_.front().second; idBuf_.pop_front(); }
        if (++idPh_ >= idHop_) {
            idPh_ = 0;
            const double a = idRe_ / idLen_, b = idIm_ / idLen_;
            keyer.push((float)(a * a + b * b));
        }
        // second decimation: the discriminator and the envelope through the SAME filter
        const float s2[2] = { (float)f, y[0] };
        for (int k = 0; k < 2; ++k) { hist2_[k][(size_t)pos2_] = s2[k]; hist2_[k][(size_t)(pos2_ + T2_)] = s2[k]; }
        if (++pos2_ == T2_) pos2_ = 0;
        ++n1_;
        if (++ph2_ < kD2) return;
        ph2_ = 0;
        const double dv = dotScalar(&hist2_[0][(size_t)pos2_], h2_.data(), T2_);
        const double lv = dotScalar(&hist2_[1][(size_t)pos2_], h2_.data(), T2_);
        bufD_.push_back(dv); bufL_.push_back(lv);
        if ((int)bufD_.size() == n2_) { second_(); bufD_.clear(); bufL_.clear(); subMag_ = 0; }
    }
    void second_() {
        // time of sample i of this block (s), the same for both streams; the absolute count keeps the time base continuous
        const double t0 = (double)k2_ / fs2_;
        k2_ += n2_;
        double sw = 0, mean = 0, meanD = 0;
        for (int i = 0; i < n2_; ++i) { sw += win_[(size_t)i]; mean += win_[(size_t)i] * bufL_[(size_t)i]; meanD += win_[(size_t)i] * bufD_[(size_t)i]; }
        mean /= sw; meanD /= sw;
        std::complex<double> pv(0, 0), pr(0, 0), p90(0, 0), p150(0, 0);
        double varL = 0, varD = 0;
        for (int i = 0; i < n2_; ++i) {
            const double w = win_[(size_t)i], t = t0 + i / fs2_;
            const double l = bufL_[(size_t)i] - mean, d = bufD_[(size_t)i] - meanD;
            const std::complex<double> e30 = std::polar(1.0, -2 * M_PI * 30.0 * t);
            pv += w * l * e30; pr += w * d * e30;
            p90 += w * l * std::polar(1.0, -2 * M_PI * 90.0 * t);
            p150 += w * l * std::polar(1.0, -2 * M_PI * 150.0 * t);
            varL += w * l * l; varD += w * d * d;
        }
        // ★ A tone A·cos(ωt + φ) in the window gives |P| = A·Σw/2 and a windowed variance Σw·x² = A²/2·Σw, so its
        //   amplitude is 2|P|/Σw and its share of the variance (2|P|²/Σw) / var — both exact for the Hann window used.
        Second s;
        s.carrier = mean;
        const double c = mean > 1e-20 ? mean : 1e-20;
        s.var30 = 2 * std::abs(pv) / sw / c;
        s.dev = 2 * std::abs(pr) / sw;
        s.m90 = 2 * std::abs(p90) / sw / c;
        s.m150 = 2 * std::abs(p150) / sw / c;
        s.sub = 2 * (subMag_ / (n2_ * kD2)) / c;
        s.refCoh = varD > 0 ? std::min(1.0, 2 * std::norm(pr) / sw / varD) : 0;
        s.ilsCoh = varL > 0 ? std::min(1.0, 2 * (std::norm(p90) + std::norm(p150)) / sw / varL) : 0;
        // ★ the discriminator's half-sample (at fs1) delay, put back
        const std::complex<double> fix = std::polar(1.0, 2 * M_PI * 30.0 * 0.5 / fs1_);
        s.vor = s.refCoh >= 0.5 && s.dev >= 300 && s.dev <= 650 && s.sub >= 0.10 && s.sub <= 0.5 && s.var30 >= 0.10 && s.var30 <= 0.5;
        if (s.vor) {
            double b = (std::arg(pr * fix) - std::arg(pv)) * 180.0 / M_PI;
            b = std::fmod(b, 360.0); if (b < 0) b += 360.0;
            s.bearing = b;
        }
        s.ils = !s.vor && s.refCoh < 0.2 && s.ilsCoh >= 0.6 && s.m90 >= 0.02 && s.m150 >= 0.02 && (s.m90 + s.m150) <= 1.05;
        decide_(s);
    }
    void decide_(const Second& s) {
        Report r; r.last = s;
        if (s.vor) {
            vorRun_.push_back(s.bearing);
            if (vorRun_.size() > 5) vorRun_.pop_front();
            ilsRun_.clear();
            ++vorSec_; ilsSec_ = 0;
        } else if (s.ils) {
            ilsRun_.push_back(s.m90 - s.m150);
            if (ilsRun_.size() > 5) ilsRun_.pop_front();
            sdm_ = s.m90 + s.m150;
            vorRun_.clear();
            ++ilsSec_; vorSec_ = 0;
        } else { vorRun_.clear(); ilsRun_.clear(); vorSec_ = ilsSec_ = 0; }
        if (vorRun_.size() >= 3) {
            std::complex<double> u(0, 0);
            for (double b : vorRun_) u += std::polar(1.0, b * M_PI / 180.0);
            u /= (double)vorRun_.size();
            const double R = std::min(1.0, std::abs(u));
            const double sig = R > 0 ? std::sqrt(std::max(0.0, -2 * std::log(R))) * 180.0 / M_PI : 180.0;
            if (sig <= 4.0) {
                r.kind = Report::Vor;
                double b = std::arg(u) * 180.0 / M_PI; if (b < 0) b += 360.0;
                r.bearing = b; r.sigma = sig; r.seconds = vorSec_;
            }
        } else if (ilsRun_.size() >= 3) {
            const auto mm = std::minmax_element(ilsRun_.begin(), ilsRun_.end());
            if (*mm.second - *mm.first <= 0.02) {
                double a = 0; for (double v : ilsRun_) a += v;
                r.kind = Report::Ils; r.ddm = a / ilsRun_.size(); r.sdm = sdm_; r.seconds = ilsSec_;
            }
        }
        if (onSecond) onSecond(r);
    }

    double fs_, fs1_ = 0, fs2_ = 0, delay1_ = 0, delay2_ = 0;
    bool scalar_;
    int D1_ = 1, T1_ = 0, T2_ = 0, pos1_ = 0, ph1_ = 0, pos2_ = 0, ph2_ = 0, n2_ = 0;
    std::vector<float> h1_, h2_, env_;
    std::vector<float> hist1_[5], hist2_[2];
    const float* lut_ = nullptr;
    uint32_t phSub_ = 0, phId_ = 0, stepSub_ = 0, stepId_ = 0;
    float dc_ = 0, dcA_ = 0;
    std::complex<double> zPrev_{ 1, 0 };
    double subMag_ = 0, check_ = 0;
    std::vector<double> bufD_, bufL_, win_;
    int64_t k2_ = 0, n1_ = 0;
    double idRe_ = 0, idIm_ = 0;
    std::deque<std::pair<float, float>> idBuf_;
    int idLen_ = 8, idHop_ = 31, idPh_ = 0;
    std::deque<double> vorRun_, ilsRun_;
    int vorSec_ = 0, ilsSec_ = 0;
    double sdm_ = 0;
};

// ── the band plan (ICAO Annex 10 channelling) ─────────────────────────────────────────────────────────────────────
/** The 50 kHz channel nearest `hz`. */
inline int64_t channelHz(double hz) { return (int64_t)std::llround(hz / 50e3) * 50000; }
/** What the channel is for: 1 VOR, 2 ILS localiser, 3 ILS glideslope, 0 neither. 108–111.975: odd tenths are localisers,
 *  even tenths VORs; 112–117.975 VOR; 328.6–335.4 glideslope. */
inline int channelKind(double hz) {
    const int64_t ch = channelHz(hz);
    if (ch >= 108000000 && ch <= 117950000) {
        if (ch >= 112000000) return 1;
        const int tenth = (int)((ch / 100000) % 10);
        return (tenth % 2) ? 2 : 1;
    }
    if (hz >= 328.55e6 && hz <= 335.45e6) return 3;
    return 0;
}
/** ILS localiser ↔ glideslope pairs (ICAO Annex 10 Vol. I; the same table in the FAA AIM "Frequency Pairs Allocated for
 *  ILS"): kHz. */
struct IlsPair { int locKhz, gsKhz; };
inline const IlsPair* ilsPairs(size_t& n) {
    static const IlsPair t[] = {
        { 108100, 334700 }, { 108150, 334550 }, { 108300, 334100 }, { 108350, 333950 }, { 108500, 329900 }, { 108550, 329750 },
        { 108700, 330500 }, { 108750, 330350 }, { 108900, 329300 }, { 108950, 329150 }, { 109100, 331400 }, { 109150, 331250 },
        { 109300, 332000 }, { 109350, 331850 }, { 109500, 332600 }, { 109550, 332450 }, { 109700, 333200 }, { 109750, 333050 },
        { 109900, 333800 }, { 109950, 333650 }, { 110100, 334400 }, { 110150, 334250 }, { 110300, 335000 }, { 110350, 334850 },
        { 110500, 329600 }, { 110550, 329450 }, { 110700, 330200 }, { 110750, 330050 }, { 110900, 330800 }, { 110950, 330650 },
        { 111100, 331700 }, { 111150, 331550 }, { 111300, 332300 }, { 111350, 332150 }, { 111500, 332900 }, { 111550, 332750 },
        { 111700, 333500 }, { 111750, 333350 }, { 111900, 331100 }, { 111950, 330950 } };
    n = sizeof t / sizeof t[0];
    return t;
}
/** The glideslope paired with a localiser (kHz), or the localiser paired with a glideslope; 0 = none. */
inline int pairedKhz(double hz) {
    size_t n; const IlsPair* t = ilsPairs(n);
    const int khz = (int)std::lround(hz / 1e3);
    for (size_t i = 0; i < n; ++i) {
        if (std::abs(t[i].locKhz - khz) <= 20) return t[i].gsKhz;
        if (std::abs(t[i].gsKhz - khz) <= 20) return t[i].locKhz;
    }
    return 0;
}

} // namespace nav
} // namespace vibe
