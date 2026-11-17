// vibe_dab_bank.h — V12 DAB PHASE 2 §11.1 "LARGE N": ONE channeliser pass over a wide capture for every multiplex in use
// (Stuart, 2026-11-17: "I want the locked range to be as efficient as possible on the b210 at 56mhz with all the
// multiplexes"). An OVERLAP-SAVE FFT FILTER BANK:
//   · ONE forward FFT of N points per hop of H new input samples — shared by every multiplex, the only full-rate work;
//   · per multiplex IN USE: the M = N / D bins around its block (D a power of two: Fs / D the mid rate the per-mux DDC's own
//     half-band chain ends at — 2.4…4.8 MS/s), times the channel filter's response, ONE inverse FFT of M points, the
//     last H / D outputs kept (overlap-save), the block's whole-bin offset and its sub-bin residual taken out by a per-block
//     phasor and a phase-continuous oscillator, then the SAME rational step to 2.048 MS/s as vibe_dab_ddc.h's tail.
//   · a block with nobody on it costs nothing but its share of the forward FFT.
// OUTPUT CONTRACT = vibe_dab_ddc.h DabDdc::process: interleaved complex at 2.048 MS/s centred on the block, appended — a
// multiplex decoder cannot tell which front end fed it (DabService::feedChannelised).
// Block frequencies come from the caller (kBandIII), never from a spacing. SIMD: pffft (NEON / SSE). Bounded: the spectra
// are N complex floats per hop, nothing grows with time. No threads here: vibe_dab_bank_runner.h owns them.
#pragma once
#include "vibe_dab_ddc.h"
#include "vibedsp/third_party/pffft/pffft.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <memory>
#include <vector>

namespace vibedab {
namespace bank {

/** The bank's shape at a capture rate: FFT size N, hop H, decimation D (output mid rate Fs / D), filter length L. */
struct Plan {
    int N = 0, H = 0, D = 1, L = 0;
    double fs = 0, mid = 0;
    bool ok() const { return N > 0 && H > 0; }
};
/** ★ D: the largest power of two keeping Fs / D at or above 2.4 MS/s (the DDC's own half-band rule, kMidMin) — so the
 *  rational tail is the DDC's. L: the channel filter (pass ±0.80 MHz, stop ±1.248 MHz, 80 dB — DabDdc's figures) as a
 *  Kaiser FIR at the full rate. N: the smallest power of two ≥ `minN` with N − H ≥ L − 1 at an overlap of no more than a
 *  quarter (`ovFrac`); H a multiple of D. `nPref` > 0 forces N (the benchmark sweep). */
inline Plan plan(double fs, int nPref = 0, double ovFrac = 0.25, int minN = 1024) {
    Plan p; p.fs = fs;
    if (!(fs >= DabDdc::kOutHz - 1.0)) return p;
    int D = 1; while (fs / double(D * 2) >= DabDdc::kMidMin - 1.0) D *= 2;
    p.D = D; p.mid = fs / D;
    const double tw = (DabDdc::kStopHz - DabDdc::kPassHz) / fs;
    p.L = int(std::ceil((DabDdc::kAttDb - 7.95) / (14.36 * tw))) + 1;
    int N = nPref > 0 ? nPref : minN;
    if (nPref <= 0) while (double(p.L - 1) > ovFrac * N || N / D < 32) N *= 2;
    if (N / D < 32 || N - (p.L - 1) < D) return p;
    p.N = N;
    p.H = ((N - (p.L - 1)) / D) * D;
    return p;
}

/** One multiplex's slice of the bank: its bins, its filter weights, its inverse FFT and its tail. */
class Slice {
public:
    /** `shiftHz` = the block centre's offset from the capture's centre. False when the block runs off the capture. */
    bool configure(const Plan& p, const std::vector<float>& hf /* N complex: the filter's full-rate response */, double shiftHz) {
        ok_ = false;
        if (!p.ok() || std::fabs(shiftHz) + DabDdc::kPassHz > p.fs / 2) return false;
        P_ = p; M_ = p.N / p.D;
        k0_ = int(std::lround(shiftHz * p.N / p.fs));
        const double resid = shiftHz - double(k0_) * p.fs / p.N;
        ncoStep_ = std::polar(1.0, -2.0 * M_PI * resid * p.D / p.fs);
        nco_ = 1.0; blk_ = 0;
        // per-hop phasor for the whole-bin shift: e^{-j 2π k0 H / N}, accumulated per block
        hopRot_ = std::polar(1.0, -2.0 * M_PI * double(k0_) * double(p.H) / double(p.N));
        hopPh_ = 1.0;
        // the selected bins (baseband bin b = -M/2 … M/2-1 at IFFT index b mod M) and their weights (1/N folded in)
        src_.resize(size_t(M_)); w_.resize(size_t(2 * M_));
        for (int b = -M_ / 2; b < M_ / 2; ++b) {
            const int dst = (b + M_) % M_;
            src_[size_t(dst)] = ((k0_ + b) % p.N + p.N) % p.N;
            const int hb = (b + p.N) % p.N;
            w_[size_t(2 * dst)] = hf[size_t(2 * hb)] / float(p.N);
            w_[size_t(2 * dst + 1)] = hf[size_t(2 * hb + 1)] / float(p.N);
        }
        if (setup_) { pffft_destroy_setup(setup_); setup_ = nullptr; }
        setup_ = pffft_new_setup(M_, PFFFT_COMPLEX);
        if (!setup_) return false;
        y_.reset((float*)pffft_aligned_malloc(sizeof(float) * 2 * size_t(M_)));
        t_.reset((float*)pffft_aligned_malloc(sizeof(float) * 2 * size_t(M_)));
        wk_.reset((float*)pffft_aligned_malloc(sizeof(float) * 2 * size_t(M_)));
        rat_ = ddc::Rational();
        passthrough_ = std::fabs(p.mid - DabDdc::kOutHz) < 1.0;
        /* ★★ A WIDER TAIL THAN THE DDC'S, BECAUSE THE BANK HAS ALREADY DONE ITS JOB. The DDC's half-band output still carries
         *  the neighbours, so its rational stage must stop at 1.248 MHz. Here the bank's own filter has taken everything past
         *  ±1.248 MHz down 80 dB, so the tail only has to stop the images, which begin at mid − 1.248 MHz (2.25 MHz at 3.5
         *  MS/s): a transition three times wider, a third of the taps. What survives to ±1.248 folds, at 2.048 MS/s, no nearer
         *  the ensemble than 0.8 MHz — outside its ±0.768. ★ (Tried: pffft's unordered layout to skip both reorder passes —
         *  under 2 % of a core at 56 MS/s × 26, and its internal layout is not a per-complex permutation. Not worth it.) */
        if (!passthrough_) rat_.design(p.mid, DabDdc::kOutHz, DabDdc::kPassHz, std::max(DabDdc::kStopHz, p.mid - DabDdc::kStopHz), DabDdc::kAttDb);
        mid_.clear();
        ok_ = true;
        return true;
    }
    ~Slice() { if (setup_) pffft_destroy_setup(setup_); }
    Slice() = default;
    Slice(const Slice&) = delete; Slice& operator=(const Slice&) = delete;
    bool ok() const { return ok_; }
    int k0() const { return k0_; }
    /** One hop: the N-point spectrum `X` (ordered, interleaved complex) → H / D mid-rate samples → APPENDS 2.048 MS/s. */
    void hop(const float* X, std::vector<float>& out) {
        if (!ok_) return;
        float* y = y_.get();
        const float* w = w_.data();
        for (int j = 0; j < M_; ++j) {
            const float* x = X + 2 * size_t(src_[size_t(j)]);
            const float xr = x[0], xi = x[1], wr = w[2 * j], wi = w[2 * j + 1];
            y[2 * j] = xr * wr - xi * wi; y[2 * j + 1] = xr * wi + xi * wr;
        }
        pffft_transform_ordered(setup_, y, t_.get(), wk_.get(), PFFFT_BACKWARD);
        // the last H / D outputs are the valid ones (overlap-save); the whole-bin and residual shifts taken out
        const int keep = P_.H / P_.D, from = M_ - keep;
        const size_t base = mid_.size();
        mid_.resize(base + size_t(2 * keep));
        float* m = mid_.data() + base;
        std::complex<double> ph = hopPh_ * nco_;
        const std::complex<double> st = ncoStep_;
        const float* t = t_.get() + 2 * size_t(from);
        for (int i = 0; i < keep; ++i) {
            const float c = float(ph.real()), s = float(ph.imag());
            const float a = t[2 * i], b = t[2 * i + 1];
            m[2 * i] = a * c - b * s; m[2 * i + 1] = a * s + b * c;
            ph *= st;
        }
        nco_ *= std::pow(st, keep);
        nco_ /= std::abs(nco_);
        hopPh_ *= hopRot_; hopPh_ /= std::abs(hopPh_);
        ++blk_;
        if (passthrough_) { out.insert(out.end(), mid_.begin(), mid_.end()); mid_.clear(); }
        else if (mid_.size() >= size_t(2 * 1024)) { rat_.run(mid_.data(), mid_.size() / 2, out); mid_.clear(); }
    }
    /** Push what the rational stage still holds (the end of a run). */
    void flush(std::vector<float>& out) { if (!passthrough_ && !mid_.empty()) { rat_.run(mid_.data(), mid_.size() / 2, out); mid_.clear(); } }
private:
    struct AlignedFree { void operator()(float* p) const { if (p) pffft_aligned_free(p); } };
    Plan P_; int M_ = 0, k0_ = 0; uint64_t blk_ = 0; bool ok_ = false, passthrough_ = false;
    std::complex<double> ncoStep_{1, 0}, nco_{1, 0}, hopRot_{1, 0}, hopPh_{1, 0};
    std::vector<int> src_; std::vector<float> w_, mid_;
    PFFFT_Setup* setup_ = nullptr;
    std::unique_ptr<float, AlignedFree> y_, t_, wk_;
    ddc::Rational rat_;
};

/** The bank: the input ring, the shared forward FFT, the filter's response, and one Slice per multiplex in use. */
class Bank {
public:
    bool configure(double fs, int nPref = 0) {
        P_ = plan(fs, nPref);
        if (!P_.ok()) return false;
        if (setup_) { pffft_destroy_setup(setup_); setup_ = nullptr; }
        setup_ = pffft_new_setup(P_.N, PFFFT_COMPLEX);
        if (!setup_) return false;
        buf_.reset((float*)pffft_aligned_malloc(sizeof(float) * 2 * size_t(P_.N)));
        X_.reset((float*)pffft_aligned_malloc(sizeof(float) * 2 * size_t(P_.N)));
        wk_.reset((float*)pffft_aligned_malloc(sizeof(float) * 2 * size_t(P_.N)));
        std::memset(buf_.get(), 0, sizeof(float) * 2 * size_t(P_.N));
        fill_ = P_.N - P_.H;   // ★ the first hop's overlap is silence
        // the channel filter at the full rate: a Kaiser low-pass, its N-point response (ordered)
        const std::vector<double> h = ddc::kaiserLowpass(P_.L, 0.5 * (DabDdc::kPassHz + DabDdc::kStopHz) / P_.fs,
                                                         ddc::kaiserBeta(DabDdc::kAttDb), 1.0);
        std::vector<float> ht(size_t(2 * P_.N), 0.0f);
        float* hin = (float*)pffft_aligned_malloc(sizeof(float) * 2 * size_t(P_.N));
        float* hout = (float*)pffft_aligned_malloc(sizeof(float) * 2 * size_t(P_.N));
        std::memset(hin, 0, sizeof(float) * 2 * size_t(P_.N));
        for (int i = 0; i < P_.L; ++i) hin[2 * i] = float(h[size_t(i)]);
        pffft_transform_ordered(setup_, hin, hout, wk_.get(), PFFFT_FORWARD);
        hf_.assign(hout, hout + 2 * P_.N);
        pffft_aligned_free(hin); pffft_aligned_free(hout);
        for (auto& s : slices_) s.reset();
        return true;
    }
    ~Bank() { if (setup_) pffft_destroy_setup(setup_); }
    const Plan& plan_() const { return P_; }
    /** Slot `i` cuts the block `shiftHz` from centre (or is empty with shiftHz NaN). */
    bool setSlice(size_t i, double shiftHz) {
        if (i >= slices_.size()) slices_.resize(i + 1);
        if (!std::isfinite(shiftHz)) { slices_[i].reset(); return true; }
        auto s = std::make_unique<Slice>();
        if (!s->configure(P_, hf_, shiftHz)) { slices_[i].reset(); return false; }
        slices_[i] = std::move(s);
        return true;
    }
    size_t slices() const { return slices_.size(); }
    /** Feed `n` complex input samples; every Slice appends its 2.048 MS/s output to `outs[i]` (outs.size() ≥ slices()).
     *  `onHop` (optional) runs per hop with the spectrum — the runner hands slices to its workers through it. */
    template <class OnHop>
    void processHops(const float* in, size_t n, OnHop&& onHop) {
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(size_t(P_.N - fill_), n - i);
            std::memcpy(buf_.get() + 2 * size_t(fill_), in + 2 * i, sizeof(float) * 2 * take);
            fill_ += int(take); i += take;
            if (fill_ < P_.N) break;
            pffft_transform_ordered(setup_, buf_.get(), X_.get(), wk_.get(), PFFFT_FORWARD);
            onHop(X_.get());
            // keep the last N - H samples as the next block's overlap
            std::memmove(buf_.get(), buf_.get() + 2 * size_t(P_.H), sizeof(float) * 2 * size_t(P_.N - P_.H));
            fill_ = P_.N - P_.H;
        }
    }
    /** The simple single-thread form: every slice, every hop. */
    void process(const float* in, size_t n, std::vector<std::vector<float>>& outs) {
        if (outs.size() < slices_.size()) outs.resize(slices_.size());
        processHops(in, n, [&](const float* X) { for (size_t k = 0; k < slices_.size(); ++k) if (slices_[k]) slices_[k]->hop(X, outs[k]); });
    }
    Slice* slice(size_t i) { return i < slices_.size() ? slices_[i].get() : nullptr; }
private:
    struct AlignedFree { void operator()(float* p) const { if (p) pffft_aligned_free(p); } };
    Plan P_;
    PFFFT_Setup* setup_ = nullptr;
    std::unique_ptr<float, AlignedFree> buf_, X_, wk_;
    std::vector<float> hf_;
    int fill_ = 0;
    std::vector<std::unique_ptr<Slice>> slices_;
};

}  // namespace bank
}  // namespace vibedab
