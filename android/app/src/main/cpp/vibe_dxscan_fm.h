#pragma once
// ★★★ THE DX SCANNER'S FM HOP — RDS on every station of one wide capture at once (V12, 2026-11-04). docs/v12/DX-SCANNER.md.
//
// One hop = the radio parked on one capture centre for a few seconds. The brief (§4): settle, FFT, peak-pick on the channel
// raster, then demodulate each peak far enough to decode RDS. Built from the parts every listener already runs, nothing new
// in the signal path:
//
//   DSP thread ──copy──► queue (≤ 0.5 s, oldest dropped and counted as a hole)
//                          │
//   vibe-dxfm (decoder priority)
//     settle (discard ~50 ms after the retune)
//     Channelizer: ONE forward FFT of the capture (overlap-save, vibedsp) ──► power per bin for ~0.3 s ──► peaks on the raster
//     every later block: the bins copied into a batch of 8 ──► the WORKERS (vibe-dxfm1…, decoder priority, one per core
//       to spare, a few peaks each): per peak extract() its ~300 kHz channel from the SAME bins (each worker its own
//       ExtractCtx) → FmDemod (NEON / SSE2) → StereoPLL (the 57 kHz reference + bit clock) → RdsDemod → counts per PI
//
// ★★ THE CONFIRMATION COUNT IS THE DX LOG'S, NOT THE LISTENER'S: RdsDecoder's group hook reports every group whose block A
//    passed its checkword; only CLEAN ones (no correction) are counted toward "PI in ≥ 3 CRC-valid groups" (vibe_dxscan.h
//    fmConfirmed). A phantom PI from a weak signal does not repeat cleanly three times in a dwell.
// ★★ CPU, BOUNDED BY DESIGN: the forward FFT is shared; each peak costs one inverse FFT of chanBins per block plus the
//    listener's own discriminator, pilot loop and RDS front end at ~300 kHz. More peaks than `peaksPerRound` (the benchmark's
//    budget for a weak box) are decoded in ROUNDS within the same hop — the hop takes longer, the box never falls behind.
// ★ Nothing here touches the radio: the shim tunes, waits, feeds and reads the result; abort() discards everything (a visit
//   cut short is never logged).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "vibedsp/vibedsp.h"
#include "vibedsp/simd_internal.h"
#include "vibe_thread.h"
#include "vibe_dxscan.h"

namespace vibe {
namespace dx {

using vibedsp::cf32;

/** acc[i] += |x[i]|² — the hop's spectrum, summed over blocks. NEON / SSE2, scalar fallback. */
inline void accPower(const cf32* x, float* acc, int n) {
    int i = 0;
    const float* z = reinterpret_cast<const float*>(x);
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t a = vld2q_f32(z + 2 * i);
        float32x4_t p = vmulq_f32(a.val[0], a.val[0]);
        p = vmlaq_f32(p, a.val[1], a.val[1]);
        vst1q_f32(acc + i, vaddq_f32(vld1q_f32(acc + i), p));
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        const __m128 a0 = _mm_loadu_ps(z + 2 * i), a1 = _mm_loadu_ps(z + 2 * i + 4);   // r0 i0 r1 i1 | r2 i2 r3 i3
        const __m128 s0 = _mm_mul_ps(a0, a0), s1 = _mm_mul_ps(a1, a1);
        const __m128 re = _mm_shuffle_ps(s0, s1, _MM_SHUFFLE(2, 0, 2, 0));
        const __m128 im = _mm_shuffle_ps(s0, s1, _MM_SHUFFLE(3, 1, 3, 1));
        _mm_storeu_ps(acc + i, _mm_add_ps(_mm_loadu_ps(acc + i), _mm_add_ps(re, im)));
    }
#endif
    for (; i < n; ++i) acc[i] += z[2 * i] * z[2 * i] + z[2 * i + 1] * z[2 * i + 1];
}

class FmHopEngine {
public:
    struct Params {
        double rateHz = 2.4e6;
        double centreHz = 0;       // where the capture is tuned
        double dcHz = 0;           // where its DC sits (centre + the radio's offset)
        int    rasterKhz = 100;
        double loHz = 0, hiHz = 0; // the raster channels this hop owns (planFm)
        double settleSec = 0.05;   // ★ brief §4: discard after every retune (tune per device)
        double specSec = 0.30;     // the spectrum's integration before peaks are picked
        double dwellSec = 4.0;     // ★ brief §4: "about 3–5 s per chunk"; tune from field results
        double minDwellSec = 1.5;  // a round may end early once every peak is identified, never before this
        int    maxPeaks = 24;
        int    peaksPerRound = 64; // ★ the benchmark's budget (a weak box decodes the peaks in rounds)
        int    workers = 0;        // 0 = one per core to spare, at most 4
        float  minSnrDb = 8.0f;    // a channel this far over the hop's floor is a peak
    };
    struct Stats { int peaks = 0, rounds = 0, workers = 0; double wallSec = 0, cpuSec = 0; long droppedBlocks = 0; float floorDb = -200; };

    FmHopEngine() = default;
    ~FmHopEngine() { abort(); }
    FmHopEngine(const FmHopEngine&) = delete;
    FmHopEngine& operator=(const FmHopEngine&) = delete;

    /** Start a hop (on its own thread). The caller tunes the radio first; samples fed before start() are not wanted. */
    void start(const Params& p) {
        abort();
        done_ = false; aborted_ = false; stop_ = false;
        // ★ under the queue's lock: a feed() already past its running_ test reads p_.rateHz there
        { std::lock_guard<std::mutex> lk(qm_); p_ = p; q_.clear(); qSamples_ = 0; dropped_ = 0; gap_ = false; }
        results_.clear(); stats_ = Stats{};
        running_ = true;
        th_ = std::thread([this] { run(); });
    }
    /** ★ The DSP thread: a COPY into the bounded queue, nothing else. */
    void feed(const cf32* x, int n) {
        if (!running_.load(std::memory_order_relaxed) || n <= 0) return;
        std::lock_guard<std::mutex> lk(qm_);
        // ★ ≤ 0.5 s and ≤ 4 MB (512 k samples): an Airspy at 10 MS/s must not hold 40 MB on a 256 MB board
        const size_t cap = std::min((size_t)(p_.rateHz * 0.5), (size_t)524288);
        while (!q_.empty() && qSamples_ + (size_t)n > cap) { qSamples_ -= q_.front().size(); q_.pop_front(); ++dropped_; gap_ = true; }
        q_.emplace_back(x, x + n);
        qSamples_ += (size_t)n;
        qcv_.notify_one();
    }
    bool running() const { return running_.load(); }
    /** Seconds of input waiting (a test feeds faster than real time and waits on this; the server never needs to). */
    double queuedSec() { std::lock_guard<std::mutex> lk(qm_); return qSamples_ / p_.rateHz; }
    bool done() const { return done_.load(); }
    /** Stop now and throw the hop away (a listener arrived). Joins. */
    void abort() {
        stop_ = true;
        qcv_.notify_all();
        if (th_.joinable()) th_.join();
        running_ = false;
        if (!done_) aborted_ = true;
    }
    /** The hop's stations — valid only once done() (an aborted hop has none, by design). */
    std::vector<FmHit> results() const { return done_ && !aborted_ ? results_ : std::vector<FmHit>{}; }
    Stats stats() const { return stats_; }

private:
    struct Chan {
        double hz = 0;
        int centreBin = 0;
        float levelDb = -200, snrDb = 0;
        vibedsp::Channelizer::ExtractCtx ctx;
        vibedsp::FmDemod fm;
        vibedsp::StereoPLL pll;
        vibedsp::RdsDemod rds;
        std::vector<cf32> iq;
        std::vector<float> mpx, lmr, r57, r57q, clk;
        std::map<uint16_t, int> clean;     // PI → clean CRC-valid groups this dwell
        uint16_t psPi = 0;
        std::string ps;
        bool active = false;
        bool identified() const {
            int best = 0; for (const auto& kv : clean) best = std::max(best, kv.second);
            const auto* b = rds.best();
            return best >= kFmMinGroups && psPi != 0 && b && b->psComplete();
        }
    };

    Params p_;
    std::thread th_;
    std::atomic<bool> running_{false}, done_{false}, aborted_{false}, stop_{false};
    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<std::vector<cf32>> q_;
    size_t qSamples_ = 0;
    long dropped_ = 0;
    bool gap_ = false;
    std::vector<FmHit> results_;
    Stats stats_;

    // ── the worker pool: one generation per batch ──
    struct Pool {
        std::vector<std::thread> th;
        std::mutex m; std::condition_variable cv, doneCv;
        uint64_t gen = 0; int left = 0; bool quit = false;
        std::function<void(int)> job;
        void start(int n) {
            uint64_t g0; { std::lock_guard<std::mutex> lk(m); g0 = gen; }
            for (int w = 0; w < n; ++w) th.emplace_back([this, w, g0] {
                char nm[16]; std::snprintf(nm, sizeof nm, "vibe-dxfm%d", w + 1);
                vibeDecoderThread(nm);
                uint64_t seen = g0;
                for (;;) {
                    std::function<void(int)> j;
                    { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return quit || gen != seen; }); if (quit) return; seen = gen; j = job; }
                    j(w);
                    { std::lock_guard<std::mutex> lk(m); if (--left == 0) doneCv.notify_all(); }
                }
            });
        }
        void runAll(const std::function<void(int)>& f) {
            std::unique_lock<std::mutex> lk(m);
            job = f; left = (int)th.size(); ++gen; cv.notify_all();
            doneCv.wait(lk, [&] { return left == 0; });
        }
        void stopAll() { { std::lock_guard<std::mutex> lk(m); quit = true; } cv.notify_all(); for (auto& t : th) if (t.joinable()) t.join(); th.clear(); }
    };

    static double procCpu() {
        timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9;
    }

    /** Pull the next piece of input (waits). false = stop. `gap` = something was dropped before it. */
    bool pull(std::vector<cf32>& out, bool& gap) {
        std::unique_lock<std::mutex> lk(qm_);
        qcv_.wait_for(lk, std::chrono::milliseconds(200), [&] { return stop_ || !q_.empty(); });
        if (stop_) return false;
        if (q_.empty()) { out.clear(); gap = false; return true; }
        out.swap(q_.front()); q_.pop_front(); qSamples_ -= out.size();
        gap = gap_; gap_ = false;
        return true;
    }

    void run() {
        vibeDecoderThread("vibe-dxfm");
        const auto w0 = std::chrono::steady_clock::now();
        const double c0 = procCpu();
        const double rate = p_.rateHz;
        int N = 1024; while (N < 65536 && rate / N > 300.0) N <<= 1;            // ~300 Hz bins
        const double binHz = rate / N;
        int chanBins = 64; while (chanBins < N && rate * chanBins / N < 240e3) chanBins <<= 1;   // ≥ 240 kHz channels
        const double chanRate = rate * chanBins / N;
        vibedsp::Channelizer cz(N);
        std::vector<float> pw((size_t)N, 0.0f);
        vibedsp::ComplexFFT fft(N);
        std::vector<cf32> frame((size_t)N), spec((size_t)N);
        std::vector<float> hann((size_t)N);
        double hannPow = 0;
        for (int i = 0; i < N; ++i) { hann[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N)); hannPow += (double)hann[(size_t)i] * hann[(size_t)i]; }
        int fN = 0;
        int specBlocks = 0;
        long long settleLeft = (long long)(p_.settleSec * rate);
        const long long specN = (long long)(p_.specSec * rate);
        long long specDone = 0;
        enum { Spec, Decode } phase = Spec;

        std::vector<std::unique_ptr<Chan>> chans;
        size_t roundStart = 0, roundEnd = 0;
        long long roundSamples = 0;
        const long long dwellN = (long long)(p_.dwellSec * rate), minDwellN = (long long)(p_.minDwellSec * rate);

        Pool pool;
        int nWorkers = 0;
        constexpr int kBatch = 8;
        std::vector<std::vector<cf32>> batch(kBatch);
        std::vector<long long> batchIdx(kBatch);
        int nb = 0;
        bool holeInBatch = false;

        auto flush = [&] {
            if (!nb) return;
            const bool hole = holeInBatch;
            const int nbNow = nb;
            pool.runAll([&, hole, nbNow](int w) {
                for (size_t c = roundStart + (size_t)w; c < roundEnd; c += (size_t)nWorkers) {
                    Chan& ch = *chans[c];
                    if (hole) ch.rds.noteGap();
                    for (int b = 0; b < nbNow; ++b) {
                        const int n = cz.extract(batch[(size_t)b].data(), ch.centreBin, chanBins, ch.iq.data(), ch.ctx, batchIdx[(size_t)b]);
                        if (n <= 0) continue;
                        ch.fm.process(ch.iq.data(), ch.mpx.data(), n);
                        ch.pll.processBlock(ch.mpx.data(), n, ch.lmr.data(), ch.r57.data(), ch.r57q.data(), ch.clk.data());
                        ch.rds.setPilotRef(ch.pll.lockAmp());
                        ch.rds.process(ch.mpx.data(), ch.r57.data(), ch.r57q.data(), ch.clk.data(), n);
                    }
                }
            });
            nb = 0; holeInBatch = false;
        };

        auto pickPeaks = [&] {
            // channel power on the raster: the bins within ±80 kHz of each channel, from the summed spectrum
            const double r = p_.rasterKhz * 1000.0;
            struct C { double hz; double p; };
            std::vector<C> cs;
            const int w = std::max(1, (int)std::lround(80e3 / binHz));
            for (double f = p_.loHz; f <= p_.hiHz + 1; f += r) {
                const int k = (int)std::lround((f - p_.centreHz) / binHz);
                double s = 0;
                for (int d = -w; d <= w; ++d) s += pw[(size_t)(((k + d) % N + N) % N)];
                cs.push_back({ f, s / std::max(1, specBlocks) });
            }
            // also the channels just outside the hop, so a peak at the hop's edge is judged against its real neighbours
            if (cs.empty()) return;
            /* ★ THE FLOOR FROM THE BINS, NOT THE CHANNELS: on a busy band every raster channel is within 200 kHz of some
             *  station and a channel's ±80 kHz holds part of its skirt, so a percentile of CHANNEL powers measured skirts
             *  (−39 dBFS against −63 of noise). The quietest tenth of the BINS inside the hop's window is the noise; a
             *  channel's floor is that × its width. */
            std::vector<float> bins;
            {
                const int k0 = (int)std::lround((p_.loHz - 100e3 - p_.centreHz) / binHz), k1 = (int)std::lround((p_.hiHz + 100e3 - p_.centreHz) / binHz);
                for (int k = k0; k <= k1; ++k) bins.push_back(pw[(size_t)((k % N + N) % N)]);
            }
            std::nth_element(bins.begin(), bins.begin() + bins.size() / 10, bins.end());
            const double floorP = std::max(1e-30, (double)bins[bins.size() / 10] / std::max(1, specBlocks) * (2 * w + 1));
            const double n2 = (double)N * (double)N / 4.0;   // ★ a full-scale tone through the Hann window: (Σw)² = (N/2)²
            stats_.floorDb = (float)(10.0 * std::log10(floorP / n2));
            const int reach = p_.rasterKhz == 50 ? 2 : 1;   // a peak is the highest within ±100 kHz
            std::vector<C> peaks;
            for (size_t i = 0; i < cs.size(); ++i) {
                const double snr = 10.0 * std::log10(cs[i].p / floorP);
                if (snr < p_.minSnrDb) continue;
                bool top = true;
                for (int d = -reach; d <= reach && top; ++d) {
                    if (!d) continue;
                    const long j = (long)i + d;
                    if (j < 0 || j >= (long)cs.size()) continue;
                    if (cs[(size_t)j].p > cs[i].p) top = false;
                }
                if (top) peaks.push_back(cs[i]);
            }
            std::sort(peaks.begin(), peaks.end(), [](const C& a, const C& b) { return a.p > b.p; });
            if ((int)peaks.size() > p_.maxPeaks) peaks.resize((size_t)p_.maxPeaks);
            const int keep = chanBins - chanBins / vibedsp::Channelizer::OVERLAP_DIV;
            for (const auto& pk : peaks) {
                auto ch = std::make_unique<Chan>();
                ch->hz = pk.hz;
                ch->centreBin = (int)std::lround((pk.hz - p_.centreHz) / binHz);
                ch->levelDb = (float)(10.0 * std::log10(pk.p / n2));
                ch->snrDb = (float)(10.0 * std::log10(pk.p / floorP));
                chans.push_back(std::move(ch));
            }
            std::sort(chans.begin(), chans.end(), [](const std::unique_ptr<Chan>& a, const std::unique_ptr<Chan>& b) { return a->hz < b->hz; });
            for (auto& c : chans) {
                c->iq.resize((size_t)chanBins);   // ★ extract() writes the whole inverse transform, then keeps the tail
                c->mpx.resize((size_t)keep); c->lmr.resize((size_t)keep);
                c->r57.resize((size_t)keep); c->r57q.resize((size_t)keep); c->clk.resize((size_t)keep);
            }
            stats_.peaks = (int)chans.size();
        };

        auto armChan = [&](Chan& c) {
            c.fm.setGain((float)(chanRate / (2.0 * M_PI * 75000.0)));
            c.fm.reset();
            c.pll.configure(19000.0, chanRate); c.pll.reset();
            vibedsp::RdsDecoder::Callbacks cb;
            cb.ctx = &c;
            cb.group = [](void* x, uint16_t pi, int clean) { if (clean) ++static_cast<Chan*>(x)->clean[pi]; };
            cb.ps = [](void* x, uint16_t pi, const char* ps) { auto* ch = static_cast<Chan*>(x); ch->psPi = pi; ch->ps = ps ? ps : ""; };
            c.rds.configure(chanRate, cb);
            c.active = true;
        };
        auto startRound = [&]() -> bool {
            roundStart = roundEnd;
            if (roundStart >= chans.size()) return false;
            roundEnd = std::min(chans.size(), roundStart + (size_t)std::max(1, p_.peaksPerRound));
            for (size_t i = roundStart; i < roundEnd; ++i) armChan(*chans[i]);
            roundSamples = 0;
            ++stats_.rounds;
            const int spare = std::max(1, (int)std::thread::hardware_concurrency() - 2);
            const int want = std::max(1, std::min({ p_.workers > 0 ? p_.workers : std::min(4, spare), (int)(roundEnd - roundStart) }));
            if (want != nWorkers) { pool.stopAll(); pool.quit = false; pool.start(want); nWorkers = want; stats_.workers = std::max(stats_.workers, want); }
            return true;
        };
        auto roundIdentified = [&] {
            for (size_t i = roundStart; i < roundEnd; ++i) if (!chans[i]->identified()) return false;
            return true;
        };

        std::vector<cf32> in;
        bool finished = false;
        while (!finished) {
            bool gap = false;
            if (!pull(in, gap)) break;
            if (in.empty()) continue;
            const cf32* x = in.data(); int n = (int)in.size();
            if (settleLeft > 0) {
                const int k = (int)std::min<long long>(settleLeft, n);
                settleLeft -= k; x += k; n -= k;
                if (n <= 0) continue;
            }
            if (gap && phase == Decode) holeInBatch = true;
            /* ★ THE SPECTRUM IS WINDOWED (Hann), on its own frames of the raw input — not the channeliser's bins. Those are
             *  an overlap-save transform with no window, whose sidelobes put a strong local's skirt 30 dB up across the
             *  whole hop: the floor read −40 dBFS where the noise was −63, and a weak station between locals vanished
             *  under it (test-dxscan-fm, the first run). Costs one N-point FFT per N samples for 0.3 s. */
            if (phase == Spec) {
                for (int i = 0; i < n; ++i) {
                    frame[(size_t)fN] = x[i] * hann[(size_t)fN];
                    if (++fN == N) {
                        fft.forward(frame.data(), spec.data());
                        accPower(spec.data(), pw.data(), N);
                        ++specBlocks; fN = 0;
                        specDone += N;
                        if (specDone >= specN) {
                            pickPeaks();
                            phase = Decode;
                            if (!startRound()) finished = true;
                            // the rest of this piece goes to the decoders
                            x += i + 1; n -= i + 1;
                            break;
                        }
                    }
                }
                if (phase == Spec || finished || n <= 0) continue;
            }
            cz.feed(x, n, [&](const cf32* bins, int nbins) {
                if (finished) return;
                batch[(size_t)nb].assign(bins, bins + nbins);
                batchIdx[(size_t)nb] = cz.blockIndex();
                ++nb;
                roundSamples += cz.hop();
                if (nb == kBatch) {
                    flush();
                    const bool long_enough = roundSamples >= dwellN || (roundSamples >= minDwellN && roundIdentified());
                    if (long_enough) {
                        if (!startRound()) finished = true;
                    }
                }
            });
        }
        if (!stop_ && finished) {
            flush();
            for (const auto& c : chans) {
                FmHit h;
                h.khz = (int)std::lround(c->hz / 1e3);
                int best = 0; uint16_t bp = 0;
                for (const auto& kv : c->clean) if (kv.second > best) { best = kv.second; bp = kv.first; }
                h.pi = bp; h.groups = best;
                if (c->psPi == bp && bp) {
                    h.ps = c->ps;
                    const auto* b = c->rds.best();
                    h.psComplete = b && b->psComplete();
                }
                h.levelDb = c->levelDb; h.snrDb = c->snrDb;
                results_.push_back(h);
            }
        }
        pool.stopAll();
        { std::lock_guard<std::mutex> lk(qm_); stats_.droppedBlocks = dropped_; q_.clear(); qSamples_ = 0; }
        stats_.wallSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        stats_.cpuSec = procCpu() - c0;   // ★ the PROCESS's CPU over the hop — in the server that includes everything else too
        if (!stop_ && finished) done_ = true; else aborted_ = true;
        running_ = false;
    }
};

}  // namespace dx
}  // namespace vibe
