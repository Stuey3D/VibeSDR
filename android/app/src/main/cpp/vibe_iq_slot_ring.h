// vibe_iq_slot_ring.h — the OVER-BUFFER between a radio's receive thread and the DSP (Stuart, 2026-11-14: "can we make
// an over buffer?", after a B210 at 56 MS/s lost ~2 chunks a second with our CPU at 17 %).
//
// ★★★ WHY. A radio's receive thread must be back in recv() before the radio's own small FIFO fills — at 56 MS/s UHD's
//     USB frames hold well under a millisecond. It used to call the DSP's sink itself between recv()s, so the hand-off
//     (a 2 MB copy into the DSP queue, its lock) and any pause behind it were time the radio was not being read. Now
//     the receive thread only recv()s into a free slot here and moves on; a second thread hands full slots to the DSP.
//     The radio is never kept waiting by anything downstream of this ring.
// ★★ Single producer (the receive thread), single consumer (the hand-off thread). Slots are filled IN PLACE — recv()
//    writes straight into them, so the ring costs no copy of its own.
// ★★ A HOLE IS CARRIED WITH THE SAMPLES AFTER IT. A radio overflow (or this ring being full) is recorded on the next
//    slot committed, and handed over just before that slot — so the DSP marks the gap at the right block, not at
//    whichever block happened to be in hand when the overflow was counted.
// ★ Full ring = the DSP itself is behind by the whole ring (~100 ms): the block is recv()'d into a spare and dropped,
//   counted, and the next committed slot carries the hole. The radio is still read on time.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace vibe {

class IqSlotRing {
public:
    /** `slots` buffers of `floatsPerSlot` floats (interleaved I/Q). Call before either thread starts. */
    void init(size_t slots, size_t floatsPerSlot) {
        std::lock_guard<std::mutex> lk(m_);
        slots_.assign(slots < 2 ? 2 : slots, Slot{});
        for (auto& s : slots_) s.buf.assign(floatsPerSlot, 0.0f);
        spare_.assign(floatsPerSlot, 0.0f);
        head_ = count_ = 0; pendingHoles_ = 0; stopped_ = false; full_ = 0;
    }
    size_t floatsPerSlot() const { return spare_.size(); }
    size_t slots() const { return slots_.size(); }

    /** Producer: where the next recv() should write. Never null — when the ring is full it is the spare, and the
     *  matching commit() drops it. */
    float* acquire() {
        std::lock_guard<std::mutex> lk(m_);
        writingSpare_ = count_ == slots_.size();
        return writingSpare_ ? spare_.data() : slots_[(head_ + count_) % slots_.size()].buf.data();
    }
    /** Producer: `nSamples` complex samples are in the buffer acquire() gave; `holesBefore` radio overflows happened
     *  since the last commit. */
    void commit(size_t nSamples, uint64_t holesBefore) {
        {
            std::lock_guard<std::mutex> lk(m_);
            pendingHoles_ += holesBefore;
            if (writingSpare_) { ++full_; ++pendingHoles_; return; }   // the DSP is a whole ring behind: dropped, a hole
            Slot& s = slots_[(head_ + count_) % slots_.size()];
            s.n = nSamples; s.holes = pendingHoles_; pendingHoles_ = 0;
            ++count_;
        }
        cv_.notify_one();
    }
    /** Producer: a hole with nothing to carry it yet (an overflow with no samples) — goes on the next commit. */
    void noteHole(uint64_t n = 1) { std::lock_guard<std::mutex> lk(m_); pendingHoles_ += n; }

    /** Consumer: waits up to `timeoutMs` for a full slot. True = `data`/`nSamples`/`holes` describe it; call release()
     *  when done with it. False = timed out or stopped. */
    bool pop(const float*& data, size_t& nSamples, uint64_t& holes, int timeoutMs) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return count_ > 0 || stopped_; })) return false;
        if (count_ == 0) return false;
        const Slot& s = slots_[head_];
        data = s.buf.data(); nSamples = s.n; holes = s.holes;
        return true;
    }
    /** Consumer: the slot pop() gave is free again. */
    void release() {
        std::lock_guard<std::mutex> lk(m_);
        if (count_ == 0) return;
        head_ = (head_ + 1) % slots_.size();
        --count_;
    }
    /** Wakes the consumer for good (its loop then sees its own run flag). */
    void stop() { { std::lock_guard<std::mutex> lk(m_); stopped_ = true; } cv_.notify_all(); }
    /** Blocks dropped because the ring was full — the DSP, not the radio, was behind. */
    uint64_t fullDrops() const { std::lock_guard<std::mutex> lk(m_); return full_; }
    size_t waiting() const { std::lock_guard<std::mutex> lk(m_); return count_; }

private:
    struct Slot { std::vector<float> buf; size_t n = 0; uint64_t holes = 0; };
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<Slot> slots_;
    std::vector<float> spare_;
    size_t head_ = 0, count_ = 0;
    uint64_t pendingHoles_ = 0, full_ = 0;
    bool writingSpare_ = false, stopped_ = false;
};

}  // namespace vibe
