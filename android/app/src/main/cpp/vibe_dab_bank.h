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
// are N complex floats per hop, nothing grows with time. Runner (below) owns the threads.
#pragma once
#include "vibe_dab_ddc.h"
#include "vibedsp/third_party/pffft/pffft.h"
#include "vibe_thread.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
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
        contiguous_ = src_[0] + M_ / 2 <= p.N && src_[size_t(M_ / 2)] + M_ / 2 <= p.N;
        {   // the residual NCO's per-sample rotations within one hop, and the step across a whole hop
            const int keep = p.H / p.D;
            rot_.resize(size_t(2 * keep));
            for (int i = 0; i < keep; ++i) { const auto z = std::polar(1.0, std::arg(ncoStep_) * double(i)); rot_[size_t(2 * i)] = float(z.real()); rot_[size_t(2 * i + 1)] = float(z.imag()); }
            ncoKeep_ = std::polar(1.0, std::arg(ncoStep_) * double(keep));
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
        // ★ the selected bins are two contiguous runs of the spectrum (unless the block wraps past bin N-1): straight
        //   loops the compiler vectorises, not a gather through src_
        auto run = [&](int dst, const float* x, int n) {
            float* yy = y + 2 * dst; const float* ww = w + 2 * dst;
            for (int j = 0; j < n; ++j) {
                const float xr = x[2 * j], xi = x[2 * j + 1], wr = ww[2 * j], wi = ww[2 * j + 1];
                yy[2 * j] = xr * wr - xi * wi; yy[2 * j + 1] = xr * wi + xi * wr;
            }
        };
        if (contiguous_) { run(0, X + 2 * size_t(src_[0]), M_ / 2); run(M_ / 2, X + 2 * size_t(src_[size_t(M_ / 2)]), M_ / 2); }
        else for (int j = 0; j < M_; ++j) run(j, X + 2 * size_t(src_[size_t(j)]), 1);
        pffft_transform_ordered(setup_, y, t_.get(), wk_.get(), PFFFT_BACKWARD);
        // the last H / D outputs are the valid ones (overlap-save); the whole-bin and residual shifts taken out
        const int keep = P_.H / P_.D, from = M_ - keep;
        const size_t base = mid_.size();
        mid_.resize(base + size_t(2 * keep));
        float* m = mid_.data() + base;
        /* ★★ THE RESIDUAL NCO FROM A TABLE. It was `ph *= st` per sample in complex<double> — a serial chain the compiler
         *  cannot vectorise (and under gcc without fast-math, a NaN-checked multiply each time), 27 blocks × 3.5 M outputs a
         *  second. Now: this hop's start phase once, in double; per sample rot_[i] = st^i (built at configure) and two
         *  independent float multiplies. Same phase to ~1e-7. */
        const std::complex<double> ph = hopPh_ * nco_;
        const float pc = float(ph.real()), ps = float(ph.imag());
        const float* t = t_.get() + 2 * size_t(from);
        const float* r = rot_.data();
        for (int i = 0; i < keep; ++i) {
            const float a = t[2 * i], b = t[2 * i + 1], rc = r[2 * i], rs = r[2 * i + 1];
            const float u = a * rc - b * rs, v = a * rs + b * rc;
            m[2 * i] = u * pc - v * ps; m[2 * i + 1] = u * ps + v * pc;
        }
        nco_ *= ncoKeep_;
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
    std::complex<double> ncoStep_{1, 0}, nco_{1, 0}, hopRot_{1, 0}, hopPh_{1, 0}, ncoKeep_{1, 0};
    std::vector<int> src_; std::vector<float> w_, mid_, rot_; bool contiguous_ = false;
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
        auto s = std::make_shared<Slice>();
        if (!s->configure(P_, hf_, shiftHz)) { slices_[i].reset(); return false; }
        slices_[i] = std::move(s);
        return true;
    }
    /** ★ Build a slice for a block WITHOUT touching the running bank (any thread: hf_ is fixed after configure) — the runner
     *  builds new ones off its own thread, so opening a multiplex never stalls the hops. Null when it does not fit. */
    std::unique_ptr<Slice> makeSlice(double shiftHz) const {
        auto s = std::make_unique<Slice>();
        if (!s->configure(P_, hf_, shiftHz)) return nullptr;
        return s;
    }
    void putSlice(size_t i, std::shared_ptr<Slice> s) { if (i >= slices_.size()) slices_.resize(i + 1); slices_[i] = std::move(s); }
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
    std::vector<std::shared_ptr<Slice>> slices_;
};


/** ★★★ THE BANK ON ITS OWN THREADS (vibe-dab-bank + workers, the DDCs' rung). vibe-dsp hands it each block ONCE — a copy and
 *  a notify, never the DSP — into a BOUNDED queue (past ~8 MB the OLDEST block is dropped and counted, never the reader
 *  held). Per block: vibe-dab-bank computes every hop's spectrum (the only full-rate work), then the multiplexes are split
 *  between it and its workers (each takes every W-th), each running its slice over all of the block's hops and handing its
 *  2.048 MS/s to its sink — the decoder's queue, as the DDC thread does. Outputs change between blocks only (setOutputs). */
class Runner {
public:
    /** `sink(samples2048, n, nRaw)` — nRaw: the wide samples this output stands for (the decoder counts what ARRIVED). */
    using Sink = std::function<void(const float*, size_t, size_t)>;
    struct Out { int key = -1; double shiftHz = 0; Sink sink; std::shared_ptr<Slice> ready; };
    Runner(double fs, int workers) : fs_(fs), workers_(std::max(0, workers)) {
        bank_.configure(fs);
        thr_ = std::thread([this] { loop_(); });
        for (int w = 0; w < workers_; ++w) pool_.emplace_back([this, w] { work_(w + 1); });
    }
    ~Runner() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        { std::lock_guard<std::mutex> lk(wm_); wstop_ = true; }
        wcv_.notify_all();
        if (thr_.joinable()) thr_.join();
        for (auto& t : pool_) if (t.joinable()) t.join();
    }
    Runner(const Runner&) = delete; Runner& operator=(const Runner&) = delete;
    double rate() const { return fs_; }
    int workers() const { return workers_; }
    const Plan& plan() const { return bank_.plan_(); }
    /** The multiplexes to cut (by key = block index), applied before the next block. A key kept keeps its slice's state. */
    void setOutputs(std::vector<Out> outs) {
        // ★ new blocks get their slice built HERE, on the caller's thread — the bank thread only swaps pointers between blocks
        std::vector<std::pair<int, double>> live;
        { std::lock_guard<std::mutex> lk(m_); live = live_; }
        for (auto& o : outs)
            if (std::find(live.begin(), live.end(), std::make_pair(o.key, o.shiftHz)) == live.end()) o.ready = std::shared_ptr<Slice>(bank_.makeSlice(o.shiftHz));
        std::lock_guard<std::mutex> lk(m_); pending_ = std::move(outs); havePending_ = true;
        cv_.notify_one();
    }
    void feed(const float* iq, size_t n) {
        if (!n) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            std::vector<float> b;
            if (!free_.empty()) { b.swap(free_.front()); free_.pop_front(); }
            b.assign(iq, iq + 2 * n);
            qFloats_ += b.size();
            q_.push_back(std::move(b));
            // ★ the cap is TIME, not bytes: 8 MB was 19 ms at 56 MS/s — one late block dropped a whole block. 150 ms of input
            //   (67 MB at 56 MS/s, 9.6 MB at 8) — still a hard bound, and only filled when the bank falls behind
            const size_t capFloats = std::max<size_t>((8u << 20) / sizeof(float), size_t(fs_ * 0.15) * 2);
            while (qFloats_ > capFloats && q_.size() > 1) {
                dropped_.fetch_add(q_.front().size() / 2, std::memory_order_relaxed);
                qFloats_ -= q_.front().size();
                free_.push_back(std::move(q_.front())); q_.pop_front();
            }
        }
        cv_.notify_one();
    }
    /** Spectra held per hand-out to the slices (bench knob; the default is the measured best). */
    void chunkBytes(size_t b) { chunkBytes_.store(std::max<size_t>(b, 1), std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t blocks() const { return blocks_.load(std::memory_order_relaxed); }
private:
    struct Cut { int key; double shiftHz; Sink sink; size_t slot; std::vector<float> out; };
    void apply_() {
        std::vector<Out> want;
        { std::lock_guard<std::mutex> lk(m_); if (!havePending_) return; want.swap(pending_); havePending_ = false; }
        std::vector<Cut> next;
        std::vector<char> used;
        auto mark = [&](size_t slot) { if (used.size() <= slot) used.resize(slot + 1, 0); used[slot] = 1; };
        std::vector<char> kept(want.size(), 0), stays(cuts_.size(), 0);
        for (size_t w = 0; w < want.size(); ++w)                      // ★ kept: same block, same place — its state goes on
            for (size_t c = 0; c < cuts_.size(); ++c)
                if (!stays[c] && cuts_[c].key == want[w].key && cuts_[c].shiftHz == want[w].shiftHz) {
                    stays[c] = 1; kept[w] = 1; cuts_[c].sink = want[w].sink; mark(cuts_[c].slot); next.push_back(std::move(cuts_[c])); break;
                }
        for (size_t c = 0; c < cuts_.size(); ++c) if (!stays[c]) bank_.putSlice(cuts_[c].slot, nullptr);   // gone: emptied
        for (size_t w = 0; w < want.size(); ++w) {
            if (kept[w] || !want[w].ready) continue;                    // ★ not built = the block does not fit this capture
            size_t slot = 0; while (slot < used.size() && used[slot]) ++slot;
            bank_.putSlice(slot, want[w].ready);
            mark(slot);
            next.push_back({ want[w].key, want[w].shiftHz, want[w].sink, slot, {} });
        }
        cuts_.swap(next);
        std::lock_guard<std::mutex> lk(m_);
        live_.clear(); for (const auto& c : cuts_) live_.push_back({ c.key, c.shiftHz });
    }
    void loop_() {
        vibeAudioThread("vibe-dab-bank");
        std::vector<float> b;
        while (true) {
            {
                std::unique_lock<std::mutex> lk(m_);
                if (b.capacity()) { if (free_.size() < 8) free_.push_back(std::move(b)); b = std::vector<float>(); }
                cv_.wait(lk, [this] { return stop_ || !q_.empty() || havePending_; });
                if (stop_) return;
                if (q_.empty()) { lk.unlock(); apply_(); continue; }
                b.swap(q_.front()); q_.pop_front(); qFloats_ -= b.size();
            }
            apply_();
            const size_t n = b.size() / 2, N2 = size_t(2 * bank_.plan_().N);
            blocks_.fetch_add(1, std::memory_order_relaxed);
            /* ★★ HOPS ARE HANDED TO THE SLICES IN CHUNKS of ≤ chunkBytes_ of spectra — but the default is the whole block.
             *  MEASURED (M5, Linux, 56 MS/s × 27, bank CPU): 256 KB 120 %, 1 MB 84 %, 4 MB 76 %, whole block 74 % with 3 workers
             *  (79 / 73 / 70 / 69 % on the bank thread alone). Keeping the spectra in cache was the theory; the hand-off per
             *  chunk costs more than the cache saves. The knob stays for the bench and bounds spec_ for a huge feed. */
            const size_t chunk = std::max<size_t>(1, chunkBytes_.load(std::memory_order_relaxed) / (N2 * sizeof(float)));
            size_t rawLeft = n;
            hops_ = 0;
            auto run = [&] {
                if (!hops_) return;
                // ★ nRaw: the wide samples these hops stand for, the block's remainder going with the last chunk
                const size_t raw = std::min(rawLeft, hops_ * size_t(bank_.plan_().H));
                nRaw_ = raw; rawLeft -= raw;
                cutHops_();
                hops_ = 0;
            };
            bank_.processHops(b.data(), n, [&](const float* X) {
                if (spec_.size() < (hops_ + 1) * N2) spec_.resize((hops_ + 1) * N2);   // ★ grows to the block's hops, never the cap
                std::memcpy(spec_.data() + hops_ * N2, X, sizeof(float) * N2);
                if (++hops_ == chunk) run();
            });
            run();
            // anything not stood for by a hop (the overlap still filling) is told with the next — the decoder counts arrivals
            if (rawLeft) carryRaw_ += rawLeft;
        }
    }
    /** The cuts for the hops in spec_: split between this thread and the workers, or all here. */
    void cutHops_() {
        if (cuts_.empty()) return;
        nRaw_ += carryRaw_; carryRaw_ = 0;
        if (workers_ > 0 && cuts_.size() > 1) {
            { std::lock_guard<std::mutex> lk(wm_); ++gen_; busy_ = workers_; }
            wcv_.notify_all();
            runShare_(0);
            std::unique_lock<std::mutex> lk(wm_);
            dcv_.wait(lk, [this] { return busy_ == 0; });
        } else runShare_(-1);
    }
    /** `who` = 0 … workers (the bank thread is 0), or -1 = every cut. */
    void runShare_(int who) {
        const size_t N2 = size_t(2 * bank_.plan_().N), step = size_t(workers_ + 1);
        for (size_t k = 0; k < cuts_.size(); ++k) {
            if (who >= 0 && k % step != size_t(who)) continue;
            Cut& c = cuts_[k];
            Slice* s = bank_.slice(c.slot);
            if (!s) continue;
            c.out.clear();
            for (size_t h = 0; h < hops_; ++h) s->hop(spec_.data() + h * N2, c.out);
            if (!c.out.empty() && c.sink) c.sink(c.out.data(), c.out.size() / 2, nRaw_);
        }
    }
    void work_(int who) {
        vibeAudioThread("vibe-dab-bankw");
        uint64_t seen = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lk(wm_);
                wcv_.wait(lk, [&] { return wstop_ || gen_ != seen; });
                if (wstop_) return;
                seen = gen_;
            }
            runShare_(who);
            { std::lock_guard<std::mutex> lk(wm_); if (--busy_ == 0) dcv_.notify_one(); }
        }
    }
    double fs_; int workers_;
    Bank bank_;
    std::vector<Cut> cuts_;
    std::atomic<size_t> chunkBytes_{64u << 20};
    std::vector<float> spec_; size_t hops_ = 0, nRaw_ = 0, carryRaw_ = 0;
    std::mutex m_; std::condition_variable cv_;
    std::deque<std::vector<float>> q_, free_; size_t qFloats_ = 0; bool stop_ = false;
    std::vector<Out> pending_; bool havePending_ = false;
    std::vector<std::pair<int, double>> live_;   // (key, shift) of the cuts running — read by setOutputs under m_
    std::mutex wm_; std::condition_variable wcv_, dcv_; uint64_t gen_ = 0; int busy_ = 0; bool wstop_ = false;
    std::atomic<uint64_t> dropped_{0}, blocks_{0};
    std::thread thr_; std::vector<std::thread> pool_;
};

}  // namespace bank
}  // namespace vibedab
