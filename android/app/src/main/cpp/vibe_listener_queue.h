// vibe_listener_queue.h — how many channelizer blocks a listener's own DSP thread may have waiting.
//
// ★★★ SIZED BY TIME, NOT BY BLOCK COUNT (2026-10-07). The hand-off to each per-VFO listener's thread
//     held FOUR blocks and dropped the oldest beyond that. A block is one hop of the channelizer's
//     forward FFT, and that FFT is capped at 32768 points — so a block is a fixed number of SAMPLES,
//     and its length in TIME shrinks as the capture rate rises:
//         RTL 2.4 MS/s   24576-sample hop = 10.2 ms   → 4 blocks = 41 ms of slack
//         RSP 8 MS/s     24576-sample hop =  3.1 ms   → 4 blocks = 12 ms of slack
//     "Four is ~12 ms at 8 MSPS" was written down when it was chosen; what was not written down is
//     that the RTLs it was proven on had three and a half times as long. On the Pi 500's RSP1B any
//     hiccup past 12 ms — the listener's thread descheduled, or the DSP thread catching up a backlog
//     and posting several blocks back to back — threw blocks away: chanDrops reached 10, and the
//     listener's audio (measured on its own channel, 2026-10-07) stepped by 3-6 ms, one or two
//     blocks, exactly as often.
// ★★ The slack is now the ~40 ms an RTL listener always had, at every rate: never fewer than the
//    old four blocks, so nothing that worked loses headroom. It adds NO latency in steady state —
//    the queue is empty whenever the listener keeps up, which is nearly always; it only lets a
//    listener that fell briefly behind catch up instead of losing audio. A listener that is
//    genuinely stuck still drops, now after 40 ms rather than 12.
// ★ Memory: the blocks are shared by every listener (one copy per round, held by reference), so a
//   deeper queue costs pointers, not copies, unless a listener is actually behind.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace vibe {

/** The slack, in seconds of signal, a listener's block queue holds before it drops the oldest. */
constexpr double kListenerQueueSlackSec = 0.040;
/** Never fewer blocks than the hand-off always had. */
constexpr size_t kListenerQueueMinBlocks = 4;

/** Blocks of `hopSamples` (at `sampleRate`) that make up kListenerQueueSlackSec. */
inline size_t listenerQueueBlocks(double sampleRate, int hopSamples) {
    if (!(sampleRate > 0.0) || hopSamples <= 0) return kListenerQueueMinBlocks;
    const double blockSec = (double)hopSamples / sampleRate;
    const double n = std::ceil(kListenerQueueSlackSec / blockSec - 1e-9);
    // ★ Bounded above too: a pathological tiny hop must not turn "slack" into seconds of backlog.
    return std::min<size_t>(64, std::max<size_t>(kListenerQueueMinBlocks, (size_t)n));
}

}  // namespace vibe
