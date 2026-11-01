// vibe_avi_workers.h — V12 aviation: a channel decoder's CHANNELS split across worker threads (VDL2; ACARS can use it).
// docs/v12/AVIATION.md §Threads.
//
// ★★★ ONE FRONT END, N WORKERS. The caller's thread runs the shared front end only (the half-bands and the one forward
//     FFT, vibeavi::MultiChannel::feedBins); every block's bins go to each worker through its own lock-free single-
//     producer / single-consumer ring; a worker slices ITS channels (the inverse FFTs, the responses) and runs their
//     demodulators. Channels are dealt round-robin. A channel's state is only ever touched by its worker.
// ★★★ THE RING DROPS THE OLDEST, AND COUNTS IT (Network > Audio > Spectrum > Decoders): the producer never waits — it
//     overwrites the oldest slot; a worker that finds its next slot overwritten (a sequence number per slot, checked
//     before and after the copy) skips to the oldest block still there and adds the gap to dropped(). `lossless` (the
//     offline tools only) makes the producer wait instead, so a replay decodes every block.
// ★★ WORKER COUNT FROM THE CORES: up to 4 on a quad core, as many as there are cores below that, 1 on a single core
//    (a worker thread still, so the radio's thread never runs a demodulator). Workers run at decoder priority
//    (vibeDecoderThread), below the network, audio and spectrum threads.
#pragma once
#include "vibe_avi_front.h"
#include "vibe_thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vibeavi {

inline int defaultWorkers() {
    const unsigned n = std::thread::hardware_concurrency();
    return n <= 1 ? 1 : int(std::min(4u, n));
}

/** A single-producer single-consumer ring of whole blocks of bins; the producer overwrites the oldest when full. */
class BlockRing {
public:
    BlockRing(int slots, int nBins) : cap_(slots), n_(nBins), data_(size_t(slots) * size_t(nBins)), seq_(size_t(slots)), bi_(size_t(slots)) {
        for (auto& s : seq_) s.store(0);
    }
    /** Producer. `lossless`: wait for room instead of overwriting. */
    void push(const cf32* bins, long long bi, bool lossless, const std::atomic<bool>& run) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (lossless) while (run.load(std::memory_order_relaxed) && h - tail_.load(std::memory_order_acquire) >= uint64_t(cap_)) std::this_thread::sleep_for(std::chrono::microseconds(200));
        const size_t s = size_t(h % uint64_t(cap_));
        seq_[s].store(2 * h + 1, std::memory_order_release);          // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&data_[s * size_t(n_)], bins, size_t(n_) * sizeof(cf32));
        bi_[s] = bi;
        seq_[s].store(2 * h + 2, std::memory_order_release);          // even: block h is complete
        head_.store(h + 1, std::memory_order_release);
    }
    /** Consumer: copy the next block into `out`; false = nothing yet. Overwritten blocks are skipped and counted. */
    bool pop(cf32* out, long long& bi) {
        for (;;) {
            uint64_t t = tail_.load(std::memory_order_relaxed);
            const uint64_t h = head_.load(std::memory_order_acquire);
            if (t == h) return false;
            if (h - t > uint64_t(cap_)) { dropped_.fetch_add(h - uint64_t(cap_) - t, std::memory_order_relaxed); t = h - uint64_t(cap_); tail_.store(t, std::memory_order_release); }
            const size_t s = size_t(t % uint64_t(cap_));
            const uint64_t s1 = seq_[s].load(std::memory_order_acquire);
            if (s1 != 2 * t + 2) { dropped_.fetch_add(1, std::memory_order_relaxed); tail_.store(t + 1, std::memory_order_release); continue; }
            std::memcpy(out, &data_[s * size_t(n_)], size_t(n_) * sizeof(cf32));
            bi = bi_[s];
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_[s].load(std::memory_order_relaxed) != s1) { dropped_.fetch_add(1, std::memory_order_relaxed); tail_.store(t + 1, std::memory_order_release); continue; }
            tail_.store(t + 1, std::memory_order_release);
            return true;
        }
    }
    bool empty() const { return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire); }
    bool full() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire) >= uint64_t(cap_); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    const int cap_, n_;
    std::vector<cf32> data_;
    std::vector<std::atomic<uint64_t>> seq_;
    std::vector<long long> bi_;
    std::atomic<uint64_t> head_{0}, tail_{0}, dropped_{0};
};

/** The workers. `work(channel, const cf32* z, int n)` runs on the channel's worker thread. `workers` = 0: no threads,
 *  everything on the caller's thread (the reference the threaded run must equal). */
class Workers {
public:
    using Work = std::function<void(int, const cf32*, int)>;
    Workers(MultiChannel& mc, int workers, Work work, bool lossless = false, int slots = 64)
        : mc_(mc), work_(std::move(work)), lossless_(lossless) {
        const int nCh = int(mc.channels());
        nW_ = std::max(0, std::min(workers, nCh));
        if (nW_ == 0) { inline_.reset(new MultiChannel::Scratch); return; }
        run_.store(true);
        for (int w = 0; w < nW_; ++w) {
            auto W = std::unique_ptr<Worker>(new Worker(slots, mc.fftSize()));
            for (int c = w; c < nCh; c += nW_) W->chans.push_back(c);
            ws_.push_back(std::move(W));
        }
        for (int w = 0; w < nW_; ++w) ws_[size_t(w)]->th = std::thread([this, w] { loop_(w); });
    }
    ~Workers() { stop(); }
    void stop() {
        run_.store(false);
        for (auto& w : ws_) { std::lock_guard<std::mutex> lk(w->m); w->cv.notify_all(); w->roomCv.notify_all(); }
        for (auto& w : ws_) if (w->th.joinable()) w->th.join();
    }
    int workers() const { return nW_; }
    /** The caller's thread: the front end, then each block to every worker (or inline). */
    void feed(const cf32* iq, size_t n) {
        if (nW_ == 0) {
            ob_.resize(size_t(mc_.chanBins()));
            mc_.feedBins(iq, n, [&](const cf32* bins, long long bi) {
                for (size_t c = 0; c < mc_.channels(); ++c) {
                    const int k = mc_.slice(c, bins, bi, ob_.data(), *inline_);
                    if (k > 0) work_(int(c), ob_.data(), k);
                }
            });
            return;
        }
        mc_.feedBins(iq, n, [&](const cf32* bins, long long bi) {
            for (auto& w : ws_) {
                if (lossless_) { std::unique_lock<std::mutex> lk(w->m); w->roomCv.wait(lk, [&] { return !w->ring.full() || !run_.load(); }); }
                w->ring.push(bins, bi, false, run_);
                if (w->sleeping.load(std::memory_order_acquire)) { std::lock_guard<std::mutex> lk(w->m); w->cv.notify_one(); }
            }
        });
    }
    /** Wait until every worker has drained its ring (offline tools, tests). */
    void flush() {
        for (auto& w : ws_) while (!w->ring.empty() || w->busy.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    uint64_t dropped() const { uint64_t d = 0; for (auto& w : ws_) d += w->ring.dropped(); return d; }

private:
    struct Worker {
        Worker(int slots, int nBins) : ring(slots, nBins), bins(size_t(nBins)) {}
        BlockRing ring;
        std::vector<int> chans;
        std::vector<cf32> bins, out;
        MultiChannel::Scratch sc;
        std::atomic<bool> busy{false}, sleeping{false};
        std::mutex m;
        std::condition_variable cv, roomCv;
        std::thread th;
    };
    void loop_(int w) {
        char name[16]; std::snprintf(name, sizeof name, "vibe-avi%d", w);
        vibeDecoderThread(name);
        Worker& W = *ws_[size_t(w)];
        W.out.resize(size_t(mc_.chanBins()));
        long long bi = 0;
        while (run_.load(std::memory_order_relaxed)) {
            W.busy.store(true);
            if (!W.ring.pop(W.bins.data(), bi)) {
                W.busy.store(false);
                // ★ sleep until the front end pushes (a wake per block, ~400 a second), never a poll loop
                std::unique_lock<std::mutex> lk(W.m);
                W.sleeping.store(true, std::memory_order_release);
                if (W.ring.empty() && run_.load()) W.cv.wait_for(lk, std::chrono::milliseconds(20));
                W.sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
            if (lossless_) { std::lock_guard<std::mutex> lk(W.m); W.roomCv.notify_one(); }
            for (int c : W.chans) {
                const int k = mc_.slice(size_t(c), W.bins.data(), bi, W.out.data(), W.sc);
                if (k > 0) work_(c, W.out.data(), k);
            }
        }
        W.busy.store(false);
    }
    MultiChannel& mc_;
    Work work_;
    bool lossless_;
    int nW_ = 0;
    std::atomic<bool> run_{false};
    std::vector<std::unique_ptr<Worker>> ws_;
    std::unique_ptr<MultiChannel::Scratch> inline_;
    std::vector<cf32> ob_;
};

}  // namespace vibeavi
