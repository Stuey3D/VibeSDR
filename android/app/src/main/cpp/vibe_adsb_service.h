// vibe_adsb_service.h — ADS-B mode's decoder service: vibe_adsb.h on its own thread, fed from the DSP loop,
// with the aircraft report clients draw and the measurements ADS-B's gain rule steers by.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS. Thread priority on this project is Network > Audio
//     > Spectrum > Decoders (memory: feedback_thread_priority_order). feed() copies a block into a bounded
//     queue and returns; a full queue loses the OLDEST block and counts it. A Pi that cannot keep up loses
//     some replies — never the waterfall, never the socket.
// ★★ The decoder works at whatever rate the radio is ALREADY running (≥ 2.4 MS/s): ADS-B mode never changes
//    the sample rate (Stuart, 2026-10-24 — the 32-bit re-rate teardown, memory dab_crash_was_the_dongle_not_
//    the_code). See vsAdsbCapableHw in local_sdr_shim.cpp.
#pragma once
#include "vibe_adsb.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibeadsb {

/** One second's measurements, for the gain rule and the status line. */
struct Health {
    double   noiseDbfs = -99;     ///< the demodulator's noise floor (mean power)
    double   msgsPerSec = 0;      ///< believed messages
    double   strongFrac = 0;      ///< share of believed messages within 3 dB of full scale
    uint64_t dropped = 0;         ///< capture blocks lost because the decoder was behind (since start)
    bool     valid = false;
};

class Service {
public:
    static constexpr double kMinRateHz = 2.4e6 - 1000;   ///< below this ADS-B is not offered — see test-adsb

    ~Service() { stop(); }

    /** Start decoding a capture at `rateHz`. lat/lon NAN when the server has no position. */
    void start(double rateHz, double lat, double lon) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        rate_ = rateHz;
        demod_.reset(new Demod(rateHz));
        tracker_ = Tracker();
        tracker_.setMaxRangeNm(360);                      // as dump1090-fa: a position further than this is a bad decode
        if (lat == lat && lon == lon) tracker_.setReceiver(lat, lon);
        queue_.clear();
        dropped_ = 0;
        t0_ = std::chrono::steady_clock::now();
        winMsgs_ = winStrong_ = 0; lastGood_ = 0; winStart_ = 0;
        health_ = Health{};
        run_ = true;
        worker_ = std::thread([this] { loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!run_) return;
            run_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool running() const { return run_.load(std::memory_order_relaxed); }
    double rate() const { return rate_; }

    /** From the DSP thread: copy and return. Never blocks on the decoder. */
    void feed(const std::complex<float>* iq, size_t n) {
        if (!run_.load(std::memory_order_relaxed) || !n) return;
        std::vector<std::complex<float>> b(iq, iq + n);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.size() >= kMaxQueued) { queue_.pop_front(); ++dropped_; }
            queue_.push_back(std::move(b));
        }
        cv_.notify_one();
    }

    /** The aircraft report: {"type":"adsb", …Tracker::json…, "rate":…, "noise":…, "dropped":…}. */
    std::string report() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string t = tracker_.json(now());
        char tail[160];
        std::snprintf(tail, sizeof tail, ",\"rate\":%.0f,\"noise\":%.1f,\"msgRate\":%.0f,\"dropped\":%llu}",
                      rate_, health_.noiseDbfs, health_.msgsPerSec, (unsigned long long)dropped_);
        // Tracker::json is {"now":…,"messages":…,"aircraft":[…]} — open it up and add the type and the tail
        return "{\"type\":\"adsb\"," + t.substr(1, t.size() - 2) + tail;
    }

    Health health() {
        std::lock_guard<std::mutex> lk(mtx_);
        Health h = health_;
        h.dropped = dropped_;
        return h;
    }

    size_t aircraftCount() {
        std::lock_guard<std::mutex> lk(mtx_);
        return tracker_.aircraft().size();
    }

private:
    static constexpr size_t kMaxQueued = 16;               // ~0.1–0.4 s of capture, by the radio's block size

    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }

    void loop() {
        std::unique_lock<std::mutex> lk(mtx_);
        while (run_) {
            cv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !queue_.empty(); });
            while (run_ && !queue_.empty()) {
                std::vector<std::complex<float>> b = std::move(queue_.front());
                queue_.pop_front();
                Demod* d = demod_.get();
                lk.unlock();
                // ★ The tracker is touched only on this thread and under mtx_ for report(); the demodulator
                //   only here. A frame is judged as soon as it is cut, so `known` sees the latest aircraft.
                const double t = now();
                d->feed(b.data(), b.size(),
                        [&](const Frame& f) {
                            std::lock_guard<std::mutex> g(mtx_);
                            if (tracker_.accept(f, t)) { ++winMsgs_; if (f.level > -3.0f) ++winStrong_; }
                        },
                        [&](uint32_t a) { std::lock_guard<std::mutex> g(mtx_); return tracker_.known(a, t); });
                lk.lock();
            }
            const double t = now();
            if (t - winStart_ >= 1.0) {
                const double span = t - winStart_;
                health_.msgsPerSec = winMsgs_ / span;
                health_.strongFrac = winMsgs_ ? double(winStrong_) / winMsgs_ : 0;
                health_.noiseDbfs = demod_ ? demod_->noiseDbfs() : -99;
                health_.valid = true;
                winMsgs_ = winStrong_ = 0;
                winStart_ = t;
                tracker_.expire(t, 60);
            }
        }
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> run_{false};
    double rate_ = 0;
    std::unique_ptr<Demod> demod_;
    Tracker tracker_;
    std::deque<std::vector<std::complex<float>>> queue_;
    uint64_t dropped_ = 0;
    std::chrono::steady_clock::time_point t0_;
    uint64_t winMsgs_ = 0, winStrong_ = 0, lastGood_ = 0;
    double winStart_ = 0;
    Health health_;
};

/**
 * ADS-B's own gain rule — the same idea as dump1090-fa's adaptive range, on our measurements.
 *
 * ★★★ NOT VibeAGC. VibeAGC sets gain from the capture's average level; ADS-B is mostly noise with short strong
 *     bursts, so an average-level rule either leaves the noise floor far below the ADC's range (weak aircraft
 *     lost) or lets nearby aircraft clip. ADS-B wants the noise floor high enough to use the ADC's range and
 *     backs off only when replies start to clip.
 * ★ Works on the radio's OWN gain list (LocalSdrShim::getTunerGains: the RTL's table, SDRplay 0–49 dB, the
 *   Airspy/HackRF lists), so it is one rule for every radio rather than a control that works on one.
 */
struct GainRule {
    double noiseLow = -32, noiseHigh = -22;   ///< dBFS: aim for the noise floor between these
    double strongMax = 0.05;                  ///< more than 5 % of replies within 3 dB of full scale: too hot

    /** -1 = step down, +1 = step up, 0 = stay. */
    int decide(const Health& h) const {
        if (!h.valid) return 0;
        if (h.strongFrac > strongMax || h.noiseDbfs > noiseHigh) return -1;
        if (h.noiseDbfs < noiseLow && h.strongFrac < strongMax / 4) return +1;
        return 0;
    }
};

} // namespace vibeadsb
