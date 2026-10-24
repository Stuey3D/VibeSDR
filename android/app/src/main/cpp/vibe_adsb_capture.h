// vibe_adsb_capture.h — is the radio delivering every sample ADS-B is promised? (Stuart, 2026-10-24: the Pi 2 at
// 3.2 MS/s heard 7 aircraft and ~90 msg/s against 19 and 200-300 at 2.4, with the decoder dropping nothing and the CPU
// calm — the samples were being lost BEFORE the decoder.) The live "capture N %" in the ADS-B report.
// ★ Pure — vibeserver/test-adsb-capture.cpp holds it to known answers.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <utility>

namespace vibeadsb {

/** ★★ LIVE CAPTURE %, measured where the decoder RECEIVES the IQ. What it sees is everything lost upstream of the
 *  ADS-B service: the dongle's FIFO overflowing / USB transfers not resubmitted in time (fewer bytes than the rate,
 *  the server's usbSamples), and the server's own IQ queue dropping a block (iqDrops). What it does NOT see is the
 *  service's own queue dropping because the decoder fell behind — that is counted separately ("dropped"), because
 *  it means "this CPU is too slow", a different fault with a different cure.
 *  ★ Judged against the QUERY time, not the last arrival: a stream that stops must read as falling capture, not as
 *    the last good figure frozen. -1 until there are 3 s of history (a fresh start has nothing honest to say). */
class CaptureMeter {
public:
    static constexpr double kWindowSec = 10.0, kMinSpanSec = 3.0, kStepSec = 0.5;

    void reset(double rateHz, double t) { rate_ = rateHz; cum_ = 0; marks_.clear(); marks_.push_back({ t, 0 }); }
    /** `n` complex samples arrived at time `t` (seconds, any monotonic origin). */
    void add(size_t n, double t) {
        cum_ += n;
        // ★ A checkpoint is exact — (when, how many by then) — and never edited afterwards: one moved forward in count
        //   but not in time once made every figure read 5 % short.
        if (marks_.empty() || t - marks_.back().first >= kStepSec) marks_.push_back({ t, cum_ });
        while (marks_.size() > 2 && t - marks_[1].first >= kWindowSec) marks_.pop_front();
    }
    /** 0-100, or -1 when there is not yet enough history. */
    int percent(double t) const {
        if (rate_ <= 0 || marks_.empty()) return -1;
        // the oldest checkpoint still inside the window (or the oldest we have)
        size_t i = 0;
        while (i + 1 < marks_.size() && t - marks_[i + 1].first >= kWindowSec) ++i;
        const double span = t - marks_[i].first;
        if (span < kMinSpanSec) return -1;
        const double got = double(cum_ - marks_[i].second);
        const double pct = 100.0 * got / (span * rate_);
        return int(std::max(0.0, std::min(100.0, std::floor(pct + 0.5))));
    }
private:
    double rate_ = 0;
    uint64_t cum_ = 0;
    std::deque<std::pair<double, uint64_t>> marks_;   // (time, samples received by then)
};

} // namespace vibeadsb
