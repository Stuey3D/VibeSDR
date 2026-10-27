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
// ★★★ AND ON THE PI 2 IT COULD NOT KEEP UP (0~band2, 2026-10-27: "tap dropped 87 → 435 → 724 blocks", the main slice fed
//     47.5–50.9 s a minute, FT8 never a whole slot). PROFILED on the Pi 2's own build (armhf -O3 -mfpu=neon-vfpv4, qemu-arm
//     with a per-address instruction counter, 40 m at 2.048 MS/s, FFT 32768, the JTTY extra slice): 214 M instructions a
//     second of capture, of which the 32768-point forward FFT was 72 % (passf4/passf2/finalize), memcpy 8 % (the overlap
//     carried every block), integer division 6 % (`% n` per bin in the gather and the gate's power), the slice inverse
//     FFTs and taper 3 %, the power 2.5 %. A transform of the WHOLE 2 MHz to keep 80 kHz of it.
//     ★ So for Band the tap DECIMATES FIRST (Pre below): the slices' union (40 m: 7.0286–7.1078, 79 kHz) is mixed to 0 Hz
//       and halved, a short NEON FIR per halving, down to the smallest power-of-two rate that still holds it inside 35 % —
//       2.048 MS/s → 128 kS/s — and the channeliser runs THERE, at fftSize / D points. The bin width is unchanged
//       (rate/D over fft/D), so every slice has exactly the bins, taper, rate and centre it had; only the work moves.
//     ★ MEASURED the same way after (tapcost 2048000 band drain, the harness's own 16.7 M a second taken off both): 197 →
//       65 M instructions a second of capture, 3.0× less — half-band stages (HalfbandDecimator, NEON with the taps in
//       registers and each shifted view a VEXT of quads loaded once) 0.37 G, the 2048-point channeliser and the slices'
//       inverses ≈ 0.18 G, the NCO (mixing at 512 kS/s, not 2 MS/s) 0.035 G per 10 s. On a Pi 2 that is ≈ 10–15 % of a
//       core (estimate: ~0.5–0.7 instructions a cycle at 900 MHz). Through the tap: every 40 m passband edge within
//       0.003 dB, anything outside the slices folded in at −80.6 dB or less (test-band-slice).
//     ★ The skimmer/NDB slices keep the old route (a quarter's roll-off, one slice: no union to speak of).
//     ★ And what it still drops is BRIDGED: each chunk carries how many capture samples were dropped before it, and the
//       Band sink is handed a null block of exactly that many slice samples (DecoderHost → Spotter::feedGap: zeros).
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
        { std::lock_guard<std::mutex> lk(qm_); run_.store(false); q_.clear(); qn_ = 0; pendingGap_ = 0; }
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
            q_.push_back({ std::vector<cf32>(iq, iq + n), captureUtc, pendingGap_ });
            pendingGap_ = 0;
            qn_ += (size_t)n;
            const size_t cap = (size_t)std::max(65536.0, rate * 0.5);
            // ★ the oldest goes — and the next chunk carries how much went, so the slices can be bridged (see gapBefore)
            while (qn_ > cap && q_.size() > 1) {
                const size_t lost = q_.front().v.size() + q_.front().gapBefore;
                qn_ -= q_.front().v.size(); q_.pop_front(); dropped_.fetch_add(1);
                q_.front().gapBefore += lost;
            }
        }
        cv_.notify_one();
    }
    uint64_t droppedBlocks() const { return dropped_.load(); }
    /** ★ `n` capture samples lost before the next chunk (what the queue's overflow does; tests call it to drop on purpose). */
    void feedDropped(size_t n) { std::lock_guard<std::mutex> lk(qm_); pendingGap_ += n; }
    /** ★ Band's front-end decimation now in use (1 = the whole capture is transformed). */
    int decimation() const { return decim_.load(std::memory_order_relaxed); }
    /** Capture samples waiting for the tap thread (a test's drain). */
    size_t queuedSamples() { std::lock_guard<std::mutex> lk(qm_); return qn_; }

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
            const double capRate = g.sampleRate, gapEndUtc = ch.utc - (double)buf.size() / g.sampleRate;   // ★ before decimation
            // ★★ Band: decimate to the slices' union first (see the file note) — g is rewritten to the decimated capture
            double chunkUtc = ch.utc;
            if (g.band && pre_.plan(g)) {
                pre_.run(buf);
                chunkUtc -= pre_.delaySec;   // ★ the FIR cascade's group delay: the stamps stay CAPTURE times
                g.sampleRate = pre_.rate; g.hwCentreHz = pre_.mid; g.fftSize = pre_.fft;
            } else if (pre_.D != 1) { pre_.reset(); }
            decim_.store(pre_.D, std::memory_order_relaxed);
            if (!chan_ || chan_->fftSize() != g.fftSize || pre_.gen != chanGen_) {
                chan_.reset(new vibedsp::Channelizer(g.fftSize)); fedIn_ = 0; chanGen_ = pre_.gen;
                ctx = vibedsp::Channelizer::ExtractCtx(); for (X& x : xs) x.ctx = vibedsp::Channelizer::ExtractCtx();
            }
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
            /* ★★ A GAP THIS TAP DROPPED (the queue overflowed — a Pi 2 that could not keep up): every slice is told exactly
             *  how many of its samples are missing, ending just before this chunk, and the decoders bridge it with zeros
             *  (digi::Spotter::feedGap — a null block of k samples is the signal). The channeliser just continues. */
            if (ch.gapBefore > 0 && bandSink_) {
                auto gapTo = [&](int sl, double r, double c) {
                    const double want = (double)ch.gapBefore * r / capRate + gapCarry_[sl];
                    const int k = (int)std::floor(want);
                    gapCarry_[sl] = want - k;
                    if (k > 0) bandSink_(nullptr, k, r, c, nullptr, 0, binHz, gapEndUtc, sl);
                };
                gapTo(0, rate, centre);
                for (int k = 0; k < g.nExtras && k < kMaxExtraSlices; ++k) {
                    const int eb = bandSliceBins(g.sampleRate, g.fftSize, g.extras[k].widthHz);
                    const int ecb = (int)std::lround((g.extras[k].centreHz - g.hwCentreHz) / binHz);
                    const double er = g.sampleRate * eb / g.fftSize, ec = g.hwCentreHz + ecb * binHz;
                    if (captureHolds(g.sampleRate, g.hwCentreHz, ec, er)) gapTo(k + 1, er, ec);
                }
            }
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
                const double utc = chunkUtc - (double)(inEnd - endIdx) / g.sampleRate;
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
    /** ★★ Band's front end: mix the slices' union to 0 Hz and halve the rate D times (see the file note). */
    struct Pre {
        int D = 1, fft = 0, gen = 0, mixAt = -1;
        double rate = 0, mid = 0, delaySec = 0;
        double kRate = 0, kHw = 0, kWant = 0; int kFft = 0, kN = -1; double kX[kMaxExtraSlices * 2] = {};
        vibedsp::NCO nco;
        std::vector<std::unique_ptr<vibedsp::HalfbandDecimator>> st;
        std::vector<cf32> a, b;
        void reset() { D = 1; st.clear(); kN = -1; ++gen; }
        /** (Re)build for this geometry if it changed; false = no decimation worth having (D 1). */
        bool plan(const Geometry& g) {
            bool same = g.sampleRate == kRate && g.hwCentreHz == kHw && g.wantCentreHz == kWant && g.fftSize == kFft && g.nExtras == kN;
            for (int k = 0; same && k < g.nExtras; ++k) same = g.extras[k].centreHz == kX[2 * k] && g.extras[k].widthHz == kX[2 * k + 1];
            if (same) return D > 1;
            kRate = g.sampleRate; kHw = g.hwCentreHz; kWant = g.wantCentreHz; kFft = g.fftSize; kN = g.nExtras;
            for (int k = 0; k < g.nExtras; ++k) { kX[2 * k] = g.extras[k].centreHz; kX[2 * k + 1] = g.extras[k].widthHz; }
            ++gen; D = 1; st.clear();
            const double fs = g.sampleRate, binHz = fs / g.fftSize;
            // the slices this capture holds: their extents and their bin counts
            const int mb = bandSliceBins(fs, g.fftSize, g.widthHz);
            const double mr = fs * mb / g.fftSize;
            double lo = g.wantCentreHz - mr / 2, hi = g.wantCentreHz + mr / 2; int maxBins = mb;
            for (int k = 0; k < g.nExtras && k < kMaxExtraSlices; ++k) {
                const int eb = bandSliceBins(fs, g.fftSize, g.extras[k].widthHz);
                const double er = fs * eb / g.fftSize;
                if (!captureHolds(fs, g.hwCentreHz, g.extras[k].centreHz, er)) continue;
                lo = std::min(lo, g.extras[k].centreHz - er / 2); hi = std::max(hi, g.extras[k].centreHz + er / 2);
                maxBins = std::max(maxBins, eb);
            }
            // ★ the new centre on a whole bin from the old: every slice keeps exactly its centre
            mid = g.hwCentreHz + std::round((0.5 * (lo + hi) - g.hwCentreHz) / binHz) * binHz;
            const double ext = std::max(hi - mid, mid - lo) + binHz;
            int d = 1;
            while (d < 64 && fs / (2.0 * d) * 0.35 >= ext && g.fftSize / (2 * d) >= std::max(256, maxBins)) d *= 2;
            if (d < 2) return false;
            D = d; fft = g.fftSize / d; rate = fs / d;
            /* ★ MIX AS LATE AS IT IS CHEAP: the union sits off the capture's centre (40 m: 42.6 kHz below), and the first
             *  halvings are so relaxed that they pass |offset| + ext as easily as ext — so the NCO runs at the rate where
             *  the next half-band would need longer taps (transition < 0.3) instead of at the full 2 MS/s (40 m: 512 kS/s,
             *  a quarter of the mixing). */
            const double off = mid - g.hwCentreHz, extOff = std::fabs(off) + ext;
            mixAt = -1; delaySec = 0;
            for (double rin = fs; rin > rate * 1.5; rin /= 2) {
                // ★ a half-band (cutoff rin/4): flat to the reach, and what would fold onto the slices (rout − reach …) stopped
                const double rout = rin / 2;
                if (mixAt < 0 && (rout - 2.0 * extOff) / rin < 0.3) { mixAt = (int)st.size(); nco = vibedsp::NCO(off / rin); }
                const double reach = mixAt < 0 ? extOff : ext;
                st.emplace_back(new vibedsp::HalfbandDecimator(vibedsp::HalfbandDecimator::halfTapsFor((rout - 2.0 * reach) / rin)));
                delaySec += (double)st.back()->delaySamples() / rin;
            }
            if (mixAt < 0) { mixAt = (int)st.size(); nco = vibedsp::NCO(off / rate); }
            return true;
        }
        /** In place: `v` (the capture) becomes the decimated, centred stream. */
        void run(std::vector<cf32>& v) {
            int n = (int)v.size();
            std::vector<cf32>* in = &v; std::vector<cf32>* out = &a;   // ★ the first stage reads the chunk itself: no copy
            for (size_t s2 = 0; s2 <= st.size(); ++s2) {
                if ((int)s2 == mixAt) nco.mix(in->data(), in->data(), n);   // ★ in place (the NCO reads each sample before writing it)
                if (s2 == st.size()) break;
                if (out->size() < (size_t)n / 2 + 2) out->resize((size_t)n / 2 + 2);
                n = st[s2]->process(in->data(), n, out->data());
                in = out; out = (out == &a) ? &b : &a;
            }
            if (in != &v) v.assign(in->begin(), in->begin() + n); else v.resize((size_t)n);
        }
    };
    Pre pre_;
    int chanGen_ = -1;
    std::atomic<int> decim_{1};
    struct Chunk { std::vector<cf32> v; double utc = 0; size_t gapBefore = 0; };
    size_t pendingGap_ = 0;   // ★ dropped before the next chunk is queued (feedDropped)
    double gapCarry_[1 + kMaxExtraSlices] = {};   // ★ the fractional slice samples a bridged gap left over
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
