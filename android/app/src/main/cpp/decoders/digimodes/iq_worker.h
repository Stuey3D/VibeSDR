// decoders/digimodes/iq_worker.h — the shared half of Band's IQ modes (V12 beta91, docs/v12/BAND-SKIM-CW-RTTY.md): an
// Engine (decoders/digi_mode.h) whose input is a window of a Band slice's complex IQ, decoded on ITS OWN worker thread.
//
// ★★ WHY ITS OWN THREAD (memory: "split heavy multi-channel decoders across cores — hottest-thread scoring"). Band's decode
//    thread already carries every tuner and the gate; a 40–96 kHz CW or RTTY skimmer on it would make it the hottest thread
//    on the box and push the slotted modes' audio late. Here the decode thread only COPIES the block into a bounded queue
//    (processIq returns at once); the worker decodes at the decoders' LAG rung (vibeDecoderLagThread — one notch below
//    FT8's workers: on a single core, what gives way is the stage built to give way).
// ★ BOUNDED. At most kMaxBacklogSec (2 s) of input waits; past that the newest block is dropped and counted (drop, never wait —
//   the decode thread is never held up). The backlog is what Band's pause rule reads (backlogSec): an engine that cannot
//   keep up is the first thing paused, before any slotted mode loses a slot.
// ★ CPU. The worker's own thread clock around each decode, reported through Engine::onCpu (the host's health figure).
#pragma once
#include "../digi_mode.h"
#include "../../vibe_thread.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace vibe {
namespace digi {

class IqWorkerEngine : public Engine {
public:
    using cf32 = std::complex<float>;
    static constexpr double kMaxBacklogSec = 2.0;   // ★ the pause rule acts at 1.5 s; ≤ 2.4 MB at a 150 kHz slice

    IqWorkerEngine(double loHz, double hiHz) : lo_(loHz), hi_(hiHz) {}
    ~IqWorkerEngine() override { stopWorker(); }

    /** An IQ engine takes no audio. */
    void process(const float*, int, double) override {}
    void processIq(const cf32* x, int n, double rate, double centreHz, double captureUtc) override {
        if (!x || n <= 0 || !(rate > 0)) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (stop_) return;
            if (queuedSec_ + n / rate > kMaxBacklogSec) { dropped_ += (uint64_t)n; return; }   // ★ drop, never wait
            Block b;
            if (!spare_.empty()) { b.v.swap(spare_.back()); spare_.pop_back(); }   // ★ reuse a buffer: no allocation per block
            b.v.assign(x, x + n); b.rate = rate; b.centre = centreHz; b.utc = captureUtc;
            q_.push_back(std::move(b));
            queuedSec_ += n / rate;
        }
        cv_.notify_one();
    }
    double backlogSec() const override { std::lock_guard<std::mutex> lk(m_); return queuedSec_; }
    void idle() override {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& b : q_) if (spare_.size() < 4) spare_.push_back(std::move(b.v));
        q_.clear(); queuedSec_ = 0;
    }
    uint64_t samplesDropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    uint64_t blocksDecoded() const { return decoded_.load(std::memory_order_relaxed); }
    double loHz() const { return lo_; }
    double hiHz() const { return hi_; }

protected:
    /** The worker's work: one block, in order. Never concurrently with itself. */
    virtual void consume(const cf32* x, int n, double rate, double centreHz, double captureUtc) = 0;
    /** Start the worker — the LAST line of the derived constructor (its members exist by then). */
    void startWorker(const char* name) { th_ = std::thread([this, name] { run(name); }); }
    /** Stop and join — the FIRST line of the derived destructor (before its members go). Idempotent. */
    void stopWorker() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    static uint64_t threadCpuNs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
        timespec ts{}; if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
        return 0;
    }
    const double lo_, hi_;

private:
    struct Block { std::vector<cf32> v; double rate = 0, centre = 0, utc = 0; };
    void run(const char* name) {
        vibeDecoderLagThread(name);
        for (;;) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
                if (stop_) return;
                b = std::move(q_.front()); q_.pop_front();
                queuedSec_ = std::max(0.0, queuedSec_ - (double)b.v.size() / b.rate);
            }
            const uint64_t c0 = threadCpuNs();
            consume(b.v.data(), (int)b.v.size(), b.rate, b.centre, b.utc);
            decoded_.fetch_add(1, std::memory_order_relaxed);
            if (onCpu) onCpu(threadCpuNs() - c0);
            std::lock_guard<std::mutex> lk(m_);
            if (spare_.size() < 4) spare_.push_back(std::move(b.v));
        }
    }
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Block> q_;
    std::vector<std::vector<cf32>> spare_;
    double queuedSec_ = 0;
    uint64_t dropped_ = 0;
    std::atomic<uint64_t> decoded_{0};
    bool stop_ = false;
    std::thread th_;
};

} // namespace digi
} // namespace vibe
