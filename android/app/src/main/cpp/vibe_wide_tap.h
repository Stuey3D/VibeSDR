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
// ★★★ V12 BAND SPOTS ON STUART'S PI 2 (2026-10-27: 40 m in Band, nothing decoded for minutes while Single at 7.074
//     decoded 13 stations a slot). Two faults of this tap, both measured with test-band-slice on the real 40 m recording:
//     1. ITS BLOCKS WERE STAMPED WHEN IT DELIVERED THEM (DecoderHost::feedIq took vibeUtcNow()), not when they were
//        captured. A tap held off by the slot's decodes waking on a small box hands over a burst of late blocks, and the
//        FT8 engine reads a stamp a second late as LOST AUDIO and re-anchors — never cutting the slot. Held off 1.2 s
//        once a slot: FT8 336 spots → 0 (and 0.8 s: → 85). Now every chunk carries the DSP thread's stamp of its last
//        sample (feed(…, captureUtc)) and each block's is worked out from it by the sample count.
//     2. IT RAN AT DECODER PRIORITY (nice 10) beside the engines' workers. It is not a decoder: it is the radio-rate front
//        end, bounded work that must keep up or drop. It now runs at SPECTRUM priority (Network > Audio > Spectrum >
//        Decoders), measured 0.67 % of an M5 core at 2.4 MS/s (≈ 13–20 % of a Pi 2 core, estimated).
//     It also cuts Band's EXTRA slices (vibe_band_slice.h) off the same transform — one small inverse FFT each a block.
#pragma once
#include "vibedsp/vibedsp.h"
#include "vibe_band_slice.h"
#include "vibe_clock.h"
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
    /** ★ V12 Band spots (2026-10-27): the same with the block's CAPTURE UTC (its last sample's) and which slice it is
     *  (0 = the main one, 1… = Geometry::extras). */
    using BandSink = std::function<void(const cf32* x, int n, double rate, double centreRfHz, const float* pw, int nb, double binHz,
                                        double captureUtc, int slice)>;
    ~WideTap() { stop(); }

    /** Where to cut: the capture's rate, its hardware centre (RF at baseband 0), the slice's wanted centre and width. */
    struct Geometry {
        double sampleRate = 0, hwCentreHz = 0, wantCentreHz = 0, widthHz = 0; int fftSize = 16384; bool bins = false;
        /** ★ Band spots: sized for the slice's FLAT part with a sixteenth's roll-off (vibe_band_slice.h); false = the
         *  skimmer/NDB slice as before (a quarter's roll-off, w / 0.92). */
        bool band = false;
        ExtraSlice extras[kMaxExtraSlices]; int nExtras = 0;   ///< Band's other clusters (only with `band`)
    };
    bool running() const { return run_.load(std::memory_order_relaxed); }

    void start(Sink sink) {
        if (run_.load()) return;
        sink_ = std::move(sink); binSink_ = nullptr;
        run_.store(true);
        th_ = std::thread([this] { loop(); });
    }
    void start(BinSink sink) {
        if (run_.load()) return;
        binSink_ = std::move(sink); sink_ = nullptr; bandSink_ = nullptr;
        run_.store(true);
        th_ = std::thread([this] { loop(); });
    }
    void start(BandSink sink) {
        if (run_.load()) return;
        bandSink_ = std::move(sink); sink_ = nullptr; binSink_ = nullptr;
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
    /** The DSP thread: copy and go. ★ `captureUtc`: the capture time of the chunk's LAST sample, stamped by the caller on
     *  the DSP thread (NaN = now, here — which is the DSP thread too). */
    void feed(const cf32* iq, int n, double captureUtc = NAN) {
        if (!run_.load(std::memory_order_relaxed) || n <= 0) return;
        if (!std::isfinite(captureUtc)) captureUtc = vibeUtcNow();
        double rate; { std::lock_guard<std::mutex> lk(gm_); rate = g_.sampleRate; }
        {
            std::lock_guard<std::mutex> lk(qm_);
            q_.push_back({ std::vector<cf32>(iq, iq + n), captureUtc });
            qn_ += (size_t)n;
            const size_t cap = (size_t)std::max(65536.0, rate * 0.5);
            while (qn_ > cap && q_.size() > 1) { qn_ -= q_.front().v.size(); q_.pop_front(); dropped_.fetch_add(1); }
        }
        cv_.notify_one();
    }
    uint64_t droppedBlocks() const { return dropped_.load(); }

private:
    void loop() {
        // ★ SPECTRUM priority for a Band slice (see the file note); the skimmer and NDB taps stay at decoder priority.
        bool bandPrio = false;
        { std::lock_guard<std::mutex> lk(gm_); bandPrio = g_.band || bandSink_ != nullptr; }
        if (bandPrio) vibeSpectrumThread("vibe-bandtap"); else vibeDecoderThread("vibe-widetap");
        std::vector<cf32> slice;
        std::vector<float> pw;
        vibedsp::Channelizer::ExtractCtx ctx;
        struct X { std::vector<cf32> slice; std::vector<float> pw; vibedsp::Channelizer::ExtractCtx ctx; };
        X xs[kMaxExtraSlices];
        for (;;) {
            Chunk ch;
            {
                std::unique_lock<std::mutex> lk(qm_);
                cv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return !q_.empty() || !run_.load(); });
                if (!run_.load()) return;
                if (q_.empty()) continue;
                ch = std::move(q_.front()); q_.pop_front(); qn_ -= ch.v.size();
            }
            std::vector<cf32>& buf = ch.v;
            Geometry g; { std::lock_guard<std::mutex> lk(gm_); g = g_; }
            if (!(g.sampleRate > 0) || g.fftSize < 64) continue;
            if (!chan_ || chan_->fftSize() != g.fftSize) { chan_.reset(new vibedsp::Channelizer(g.fftSize)); fedIn_ = 0; }
            const double binHz = g.sampleRate / g.fftSize;
            int bins;
            if (g.band) bins = bandSliceBins(g.sampleRate, g.fftSize, g.widthHz);
            else {
                bins = 64;
                while (bins < g.fftSize && g.sampleRate * bins / g.fftSize < g.widthHz / 0.92) bins <<= 1;
                bins = std::min(bins, g.fftSize);
            }
            const int taper = g.band ? kBandTaperDiv : 4;
            const int centreBin = (int)std::lround((g.wantCentreHz - g.hwCentreHz) / binHz);
            const double rate = g.sampleRate * bins / g.fftSize, centre = g.hwCentreHz + centreBin * binHz;
            if ((int)slice.size() != bins) slice.assign((size_t)bins, cf32{0, 0});
            /* ★ THE CAPTURE TIME OF EACH BLOCK: the channeliser completes block j when its (j+1)·hop-th input sample is in,
             *  and this chunk's last sample was captured at ch.utc — so a block ending k samples before the chunk's end
             *  was captured k / rate earlier. */
            const long long inStart = fedIn_;
            const long long inEnd = fedIn_ + (long long)buf.size();
            fedIn_ = inEnd;
            const int hop = chan_->hop();
            chan_->feed(buf.data(), (int)buf.size(), [&](const cf32* b, int) {
                const long long bi = chan_->blockIndex();
                const long long endIdx = std::min(inEnd, std::max(inStart, (bi + 1) * (long long)hop));
                const double utc = ch.utc - (double)(inEnd - endIdx) / g.sampleRate;
                const int got = chan_->extract(b, centreBin, bins, slice.data(), ctx, bi, taper);
                if (got <= 0) return;
                if (sink_) sink_(slice.data(), got, rate, centre);
                const bool wantPw = g.bins;
                if (wantPw) { pw.resize((size_t)bins); digi::slicePower(b, g.fftSize, centreBin, bins, pw.data()); }
                if (binSink_) binSink_(slice.data(), got, rate, centre, wantPw ? pw.data() : nullptr, wantPw ? bins : 0, binHz);
                if (!bandSink_) return;
                bandSink_(slice.data(), got, rate, centre, wantPw ? pw.data() : nullptr, wantPw ? bins : 0, binHz, utc, 0);
                // ★ Band's extra slices: one more small inverse FFT each, off this SAME block
                for (int k = 0; k < g.nExtras && k < kMaxExtraSlices; ++k) {
                    const ExtraSlice& e = g.extras[k];
                    const int eb = bandSliceBins(g.sampleRate, g.fftSize, e.widthHz);
                    const int ecb = (int)std::lround((e.centreHz - g.hwCentreHz) / binHz);
                    const double erate = g.sampleRate * eb / g.fftSize, ecentre = g.hwCentreHz + ecb * binHz;
                    if (!captureHolds(g.sampleRate, g.hwCentreHz, ecentre, erate)) continue;   // ★ beyond this capture
                    X& x = xs[k];
                    if ((int)x.slice.size() != eb) x.slice.assign((size_t)eb, cf32{0, 0});
                    const int eg = chan_->extract(b, ecb, eb, x.slice.data(), x.ctx, bi, taper);
                    if (eg <= 0) continue;
                    if (wantPw) { x.pw.resize((size_t)eb); digi::slicePower(b, g.fftSize, ecb, eb, x.pw.data()); }
                    bandSink_(x.slice.data(), eg, erate, ecentre, wantPw ? x.pw.data() : nullptr, wantPw ? eb : 0, binHz, utc, k + 1);
                }
            });
        }
    }
    struct Chunk { std::vector<cf32> v; double utc = 0; };
    long long fedIn_ = 0;        // ★ capture samples fed to chan_ since it was made (its block j ends at (j+1)·hop)
    BandSink bandSink_;
    Sink sink_;
    BinSink binSink_;
    std::unique_ptr<vibedsp::Channelizer> chan_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::mutex qm_, gm_;
    std::condition_variable cv_;
    std::deque<Chunk> q_;
    size_t qn_ = 0;
    Geometry g_;
    std::atomic<uint64_t> dropped_{0};
};

} // namespace vibe
