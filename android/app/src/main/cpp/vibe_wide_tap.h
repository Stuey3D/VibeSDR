// vibe_wide_tap.h — one complex SLICE of the capture, cut off the DSP thread: a CW skimmer's segment on a radio that
// has no channeliser running (a personal receiver: one listener, one pipeline — no ClientDsp, see the shim's
// "ONLY IN SHARED/LOCKED MODE").
//
// ★★ WHY ITS OWN FFT. On a shared radio a listener's skimmer is one more slice of the forward FFT every listener's
//    channel already comes from — nearly free. A one-pipeline radio has no such transform: the pipeline tunes and
//    decimates its own narrow channel. So this runs the same Channelizer background decoding runs, over the whole
//    capture, ONLY while a skimmer is open there, on a thread of its own at decoder priority.
//    ★ COST, stated (Stuart: "state the trade-off first"): one fftSize-point forward FFT per 3/4 block of the capture
//      — at 2.4 MS/s with 16384 points, 195 transforms a second; on a Pi 2 that is the same price background decoding
//      already pays, and nothing on a radio with nobody skimming.
// ★ The DSP thread's whole cost is a copy into a bounded queue (half a second; the oldest goes, never waited for).
#pragma once
#include "vibedsp/vibedsp.h"
#include "vibe_thread.h"
#include "decoders/digi_activity.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vibe {

class WideTap {
public:
    using cf32 = vibedsp::cf32;
    /** n samples of the slice at `rate`, centred on `centreRfHz`. Called on the tap's thread. */
    using Sink = std::function<void(const cf32* x, int n, double rate, double centreRfHz)>;
    /** ★ V12 (2026-10-27): the same, with the slice's FFT bins' power (digi::slicePower) when Geometry::bins asks — Band
     *  spots' activity gate reads it, so it needs no transform of its own. */
    using BinSink = std::function<void(const cf32* x, int n, double rate, double centreRfHz, const float* pw, int nb, double binHz)>;
    ~WideTap() { stop(); }

    /** Where to cut: the capture's rate, its hardware centre (RF at baseband 0), the slice's wanted centre and width. */
    struct Geometry { double sampleRate = 0, hwCentreHz = 0, wantCentreHz = 0, widthHz = 0; int fftSize = 16384; bool bins = false; };
    bool running() const { return run_.load(std::memory_order_relaxed); }

    void start(Sink sink) {
        if (run_.load()) return;
        sink_ = std::move(sink); binSink_ = nullptr;
        run_.store(true);
        th_ = std::thread([this] { loop(); });
    }
    void start(BinSink sink) {
        if (run_.load()) return;
        binSink_ = std::move(sink); sink_ = nullptr;
        run_.store(true);
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(qm_); run_.store(false); q_.clear(); qn_ = 0; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        chan_.reset();
    }
    /** ★ Re-aim the slice (the dial moved, the radio retuned). Cheap; read by the tap thread per block. */
    void aim(const Geometry& g) { std::lock_guard<std::mutex> lk(gm_); g_ = g; }
    /** The DSP thread: copy and go. */
    void feed(const cf32* iq, int n) {
        if (!run_.load(std::memory_order_relaxed) || n <= 0) return;
        double rate; { std::lock_guard<std::mutex> lk(gm_); rate = g_.sampleRate; }
        {
            std::lock_guard<std::mutex> lk(qm_);
            q_.emplace_back(iq, iq + n);
            qn_ += (size_t)n;
            const size_t cap = (size_t)std::max(65536.0, rate * 0.5);
            while (qn_ > cap && q_.size() > 1) { qn_ -= q_.front().size(); q_.pop_front(); dropped_.fetch_add(1); }
        }
        cv_.notify_one();
    }
    uint64_t droppedBlocks() const { return dropped_.load(); }

private:
    void loop() {
        vibeDecoderThread("vibe-widetap");
        std::vector<cf32> slice;
        std::vector<float> pw;
        vibedsp::Channelizer::ExtractCtx ctx;
        for (;;) {
            std::vector<cf32> buf;
            {
                std::unique_lock<std::mutex> lk(qm_);
                cv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return !q_.empty() || !run_.load(); });
                if (!run_.load()) return;
                if (q_.empty()) continue;
                buf = std::move(q_.front()); q_.pop_front(); qn_ -= buf.size();
            }
            Geometry g; { std::lock_guard<std::mutex> lk(gm_); g = g_; }
            if (!(g.sampleRate > 0) || g.fftSize < 64) continue;
            if (!chan_ || chan_->fftSize() != g.fftSize) chan_.reset(new vibedsp::Channelizer(g.fftSize));
            const double binHz = g.sampleRate / g.fftSize;
            int bins = 64;
            while (bins < g.fftSize && g.sampleRate * bins / g.fftSize < g.widthHz / 0.92) bins <<= 1;
            bins = std::min(bins, g.fftSize);
            const int centreBin = (int)std::lround((g.wantCentreHz - g.hwCentreHz) / binHz);
            const double rate = g.sampleRate * bins / g.fftSize, centre = g.hwCentreHz + centreBin * binHz;
            if ((int)slice.size() != bins) slice.assign((size_t)bins, cf32{0, 0});
            chan_->feed(buf.data(), (int)buf.size(), [&](const cf32* b, int) {
                const int got = chan_->extract(b, centreBin, bins, slice.data(), ctx, chan_->blockIndex());
                if (got <= 0) return;
                if (sink_) sink_(slice.data(), got, rate, centre);
                if (binSink_) {
                    const bool wantPw = g.bins;
                    if (wantPw) { pw.resize((size_t)bins); digi::slicePower(b, g.fftSize, centreBin, bins, pw.data()); }
                    binSink_(slice.data(), got, rate, centre, wantPw ? pw.data() : nullptr, wantPw ? bins : 0, binHz);
                }
            });
        }
    }
    Sink sink_;
    BinSink binSink_;
    std::unique_ptr<vibedsp::Channelizer> chan_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::mutex qm_, gm_;
    std::condition_variable cv_;
    std::deque<std::vector<cf32>> q_;
    size_t qn_ = 0;
    Geometry g_;
    std::atomic<uint64_t> dropped_{0};
};

} // namespace vibe
