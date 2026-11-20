// vibe_navtex_warm.h — THE SHIPPING MAP KEPT BUILT IN THE BACKGROUND (V12, 2026-11-20).
//
// ★★ Stuart, 2026-11-20 (the Lenovo, "Building the map from 14,717 saved messages… 88 %"): "the navtex shipping map
//    needs to continually build in the background so it opens instantly instead of waiting, only when free cpu to do so
//    so on the lenovo it can do it all the time as there is an abundance of cpu". And: "if there has been no spare cpu
//    time to build the map then we revert to building on open like we do now".
//
// What this header holds is only the RULE and the THREAD — never a second copy of the index logic. The work itself
// (navwarn::sync into gallery/navtex-warn.idx + navtex-met.idx, the "placed" cache, the stations map's scan) is the
// shim's, the same functions the HTTP handlers call, under the same mutexes (local_sdr_shim.cpp, "THE NAVTEX MAP'S
// SHARED STATE"). The on-request build is untouched and stays the fallback: whatever the warm-up has not done yet, a
// request does as before (400 messages a request, answered "pending" → "Building … %").
//
//   ★ WHERE IT RUNS — its own thread, vibe-navwarm, at the LOWEST rung there is (vibeIdleThread: SCHED_IDLE + nice 19 on
//     Linux/Android, QOS_CLASS_BACKGROUND on macOS, THREAD_PRIORITY_IDLE on Windows) — below the decoders and below bulk
//     HTTP, because nobody hears a map late. The housekeeping tick (every ~2 s) KICKS it with the gate's verdict; the
//     housekeeping thread itself never does the work (it runs the AGC and the overload steps, which must not wait).
//   ★ THE CPU GATE (gate() below) — work only when ALL of: the health badge's CPU level is OK (its combined pressure
//     under 50 %: machine total, the hottest core, the run queue, dropped IQ — vibe_health.h), the box is not
//     throttling, no benchmark is running (benchProgress().running — a benchmark measures this machine and must have
//     it to itself), and the server has been up a grace period (the first health sample, the radio's own start).
//     Checked before EVERY pass, so a listener arriving shuts it within one pass.
//   ★ THE SLICE (Slicer) — a pass syncs at most `budget` messages; the budget adapts so a pass's work stays near
//     kTargetMs (25 ms), between 1 and 400 (the request's own slice). After a pass the thread sleeps at least as long as the pass
//     took (≥ kMinRestMs): at most half of one core, at idle priority.
//   ★ WHEN THERE IS NOTHING TO DO — a pass is skipped while the gallery's NAVTEX fingerprint (Gallery::kindStamp) and
//     the index file are unchanged since everything was last brought up to date: one in-memory walk every ~2 s.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace vibe { namespace navwarm {

/** Why the warm-up is (not) working — the JSON's "state", and the test's handle on it. */
enum class State : int { Starting = 0, Idle = 1, Working = 2, PausedCpu = 3, PausedBench = 4, PausedThrottle = 5, PausedTest = 6, Off = 7 };
inline const char* stateName(State s) {
    switch (s) {
        case State::Starting: return "starting";
        case State::Idle: return "idle";
        case State::Working: return "working";
        case State::PausedCpu: return "paused-cpu";
        case State::PausedBench: return "paused-benchmark";
        case State::PausedThrottle: return "paused-throttle";
        case State::PausedTest: return "paused-test";
        case State::Off: return "off";
    }
    return "?";
}

/** ★ THE CPU GATE — one rule, read before every pass. cpuLevel = vibehealth::Level (0 OK, 1 WARM, 2 HIGH, 3 CRIT). */
struct GateIn { int cpuLevel = 0; bool throttled = false, benchRunning = false, testBusy = false, inGrace = false, off = false; };
inline State gate(const GateIn& in) {
    if (in.off) return State::Off;
    if (in.testBusy) return State::PausedTest;
    if (in.benchRunning) return State::PausedBench;
    if (in.inGrace) return State::Starting;
    if (in.throttled) return State::PausedThrottle;
    if (in.cpuLevel > 0) return State::PausedCpu;
    return State::Working;
}

/** ★ THE SLICE — messages per pass, adapted to the pass's measured time. */
struct Slicer {
    static constexpr size_t kMinBudget = 1, kMaxBudget = 400, kStartBudget = 8;
    static constexpr double kTargetMs = 25.0;
    size_t budget = kStartBudget;
    /** done = messages parsed this pass, ms = the pass's wall time. A pass that parsed nothing says nothing. */
    void update(size_t done, double ms) {
        if (done == 0 || ms <= 0) return;
        const double perMsg = ms / (double)done;
        const double want = kTargetMs / std::max(perMsg, 1e-3);
        // ★ move halfway towards the target (a single slow file read must not halve the next pass)
        const double next = 0.5 * (double)budget + 0.5 * want;
        budget = (size_t)std::max<double>((double)kMinBudget, std::min<double>((double)kMaxBudget, next));
    }
};

/** ★ THE THREAD — kicked by the housekeeping tick; runs passes while the gate allows and the pass says there is more. */
class Worker {
public:
    static constexpr int kMinRestMs = 20;
    /** pass(budget, &parsed) → true while there is more to do. gateNow() → the gate's verdict this instant. */
    using PassFn = std::function<bool(size_t budget, size_t* parsed)>;
    using GateFn = std::function<State()>;
    using PrioFn = std::function<void()>;
    Worker(PassFn pass, GateFn g, PrioFn prio) : pass_(std::move(pass)), gate_(std::move(g)), prio_(std::move(prio)) {}
    /** ★ Never joined: it lives as long as the process (it is held by a never-destroyed pointer in the shim). */
    void start() {
        if (started_.exchange(true)) return;
        std::thread([this] { if (prio_) prio_(); loop(); }).detach();
    }
    void kick() { { std::lock_guard<std::mutex> lk(m_); kicked_ = true; } cv_.notify_one(); }
    State state() const { return (State)state_.load(); }
    size_t budget() const { return budget_.load(); }
    uint64_t passes() const { return passes_.load(); }
    uint64_t parsed() const { return parsed_.load(); }
    double lastPassMs() const { return lastMs_.load(); }
    double maxPassMs() const { return maxMs_.load(); }
    double busyMs() const { return busyMs_.load(); }
    uint64_t pauses() const { return pauses_.load(); }
private:
    void loop() {
        Slicer sl;
        for (;;) {
            { std::unique_lock<std::mutex> lk(m_); cv_.wait(lk, [this] { return kicked_; }); kicked_ = false; }
            for (;;) {
                const State g = gate_();
                if (g != State::Working) {
                    if (state_.load() == (int)State::Working) pauses_++;
                    state_.store((int)g);
                    break;
                }
                state_.store((int)State::Working);
                const auto t0 = std::chrono::steady_clock::now();
                size_t done = 0;
                const bool more = pass_(sl.budget, &done);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                passes_++; parsed_ += done; lastMs_.store(ms); busyMs_.store(busyMs_.load() + ms);
                if (ms > maxMs_.load()) maxMs_.store(ms);
                sl.update(done, ms);
                budget_.store(sl.budget);
                if (!more) { state_.store((int)State::Idle); break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max<int>(kMinRestMs, (int)ms)));
            }
        }
    }
    PassFn pass_; GateFn gate_; PrioFn prio_;
    std::mutex m_; std::condition_variable cv_; bool kicked_ = false;
    std::atomic<bool> started_{false};
    std::atomic<int> state_{(int)State::Starting};
    std::atomic<size_t> budget_{Slicer::kStartBudget};
    std::atomic<uint64_t> passes_{0}, parsed_{0}, pauses_{0};
    std::atomic<double> lastMs_{0}, maxMs_{0}, busyMs_{0};
};

}}  // namespace vibe::navwarm
