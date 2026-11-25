// vibe_rtty_skim_run.h — the RTTY SKIMMER inside a decoder host (vibe_decoder_host.h): every RTTY signal across a 24–96 kHz
// segment of IQ centred on the listener's dial, each with its own settings (decoders/rtty_skimmer.h, docs/v12/RTTY-SKIMMER.md).
// The shape is the PSK skimmer's (vibe_psk_run.h), so the clients draw it with the CW / PSK list.
//
// ★★ THE WORKERS ARE HERE: the engine queues each listed signal's parameter search; this run keeps `workers` threads at
//    DECODER priority (vibe_thread.h — Network > Audio > Spectrum > Decoders) that take them, strongest first. The
//    benchmark sets the default count per box (Pi 2 / R36S / old phones 1, Pi 4-5 / phones 2, PCs / Macs 4); the owner
//    can change it (vibeserver.json "rttySkimWorkers").
//
// Wire (all on the decoder socket — see web/client/src/decoders.ts):
//   0x03 | u8 state                           0 nothing heard · 1 RTTY listed, none set yet · 3 decoding
//   {"type":"rtty_signals","data":{skimmer:true,centre,width,primary:0,signals:[{id,hz,shift,set,baud,reverse,settings,
//        snr,q,active,last,first,call,country,text}]}}   — at most once a second, only when it changed
//   {"type":"rtty_spot","data":{callsign,frequency,mode,snr,how,country,timestamp}}
//
// ★ Everything sent is built HERE from what the engine decoded, and decoded text comes off the air: printable ASCII only,
//   JSON-escaped (PskRun::jsonText).
#pragma once
#include "decoders/rtty_skimmer.h"
#include "vibe_callsign_country.h"
#include "vibe_psk_run.h"          // ★ PskRun::jsonText — one escaper for every list
#include "vibe_thread.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <ctime>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vibe {

/** ★ How many search workers a box gets when the owner has not said: the hardware's class, until the benchmark measures it
 *  (Stuart: "slower devices one at a time, more powerful devices more workers"). Pi 2 / R36S / 32-bit ARM / old phones 1,
 *  Pi 4-5 and phones 2, PCs and Macs 4. */
inline int rttySkimDefaultWorkers() {
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
#if defined(__ANDROID__)
    return n >= 8 ? 2 : 1;
#elif defined(__arm__)
    (void)n; return 1;
#elif defined(__aarch64__) && defined(__linux__)
    long khz = 0;
    if (FILE* f = std::fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r")) {
        if (std::fscanf(f, "%ld", &khz) != 1) khz = 0;
        std::fclose(f);
    }
    return (n < 4 || (khz > 0 && khz < 1600000)) ? 1 : 2;   // ★ the R36S (1.5 GHz A35) and Pi 3 one; Pi 4/5 two
#else
    return n >= 4 ? 4 : n >= 2 ? 2 : 1;
#endif
}

class RttySkimRun {
public:
    using cf32 = std::complex<float>;
    struct Out {
        std::function<void(uint8_t)> state;                 ///< 0x03
        std::function<void(const std::string&)> json;       ///< a JSON text frame
        std::function<void(const rttysk::Spot&, double rfHz)> spot;
        std::function<void(uint64_t ns)> cpu;               ///< the workers' CPU, added to the host's (health)
        std::function<void(const std::string&)> log;        ///< ★ one line per signal set (which settings are on the air)
        /** ★★★ beta94 NOBODY LISTENING (vibe_bg_fullcpu.h): how many search workers this run may use NOW — asked every
         *  2 s of stream; more are started as needed (up to 8), and the ones above the figure stop taking jobs. Empty = the
         *  constructor's figure for the whole run (a listener's skimmer). */
        std::function<int()> workersNow;
        bool background = false;                            ///< ★ background decoding's: its workers on the back seat
    };
    RttySkimRun(double widthHz, int workers, Out out) : width_(widthHz), out_(std::move(out)) {
        nWorkers_ = std::max(1, std::min(8, workers));
        active_ = nWorkers_;
        for (int i = 0; i < nWorkers_; ++i) th_.emplace_back([this, i] { worker(i); });
    }
    /** ★ beta94: let `n` search workers take jobs (1…8). Threads are added, never removed mid-run: one above the figure
     *  finishes the job in hand and then waits. Any thread. */
    void setWorkers(int n) {
        n = std::max(1, std::min(8, n));
        std::lock_guard<std::mutex> lk(m_);
        if (stop_ || n == active_) return;
        while ((int)th_.size() < n) { const int i = (int)th_.size(); th_.emplace_back([this, i] { worker(i); }); }
        active_ = n; nWorkers_ = n;
        cv_.notify_all();
    }
    ~RttySkimRun() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; } cv_.notify_all();
        for (auto& t : th_) t.join();
    }
    RttySkimRun(const RttySkimRun&) = delete;
    RttySkimRun& operator=(const RttySkimRun&) = delete;

    int workers() const { std::lock_guard<std::mutex> lk(m_); return nWorkers_; }

    /** The segment's IQ: `rate` Hz, centred on `centreRfHz`. A different rate (or a jump of half the segment) rebuilds. */
    void feedIq(const cf32* x, int n, double rate, double centreRfHz) {
        if (n <= 0 || !(rate > 0)) return;
        if (!sk_ || std::fabs(rate - rate_) > 0.5) { rate_ = rate; build(); }
        else if (centre_ != 0 && std::fabs(centreRfHz - centre_) > 0.5) {
            // ★ THE SEGMENT MOVED (as the CW / PSK skimmers, 2026-11-11): a band change starts afresh; a small move keeps
            //   the list and each signal already known keeps the RF it was heard at.
            if (std::fabs(centreRfHz - centre_) >= half() ) { build(); lastListAt_ = 0; }
            else {
                std::unordered_map<int, double> keep;
                for (auto& sv : sk_->signals(true)) { const auto it = pinned_.find(sv.id); keep.emplace(sv.id, it != pinned_.end() ? it->second : centre_ + sv.offsetHz); }
                pinned_.swap(keep);
            }
        }
        centre_ = centreRfHz;
        sk_->feedIq(x, n);
        if (out_.workersNow) {
            const int64_t now = sk_->nowMs();
            if (now - workersAt_ >= 2000 || workersAt_ == 0) { workersAt_ = now; setWorkers(out_.workersNow()); }
        }
        after();
    }
    double searchedHz() const { return sk_ ? sk_->binsSearched() * sk_->binHz() : 0.0; }
    int listed() const { return sk_ ? sk_->listed() : 0; }
    int settled() const { return sk_ ? sk_->settled() : 0; }
    unsigned long spots() const { return spots_; }

private:
    double half() const { return std::min(width_, rate_ * 0.94) / 2.0; }   // ★ the slice's outer 3 % is its filter's roll-off
    void build() {
        rttysk::Skimmer::Config c;
        c.rate = rate_; c.loHz = -half(); c.hiHz = half();
        c.workers = std::max(1, nWorkers_);   // ★ > 0 = jobs go to the workers (how many take them is active_)
        // ★ THE CAP: RTTY at 170 Hz shift needs ≈ 400 Hz; 64 channels hold a busy contest's 96 kHz
        c.maxSignals = std::max(16, std::min(64, (int)(2.0 * half() / 400.0)));
        std::shared_ptr<rttysk::Skimmer> s(new rttysk::Skimmer(c));
        s->onSpot = [this](const rttysk::Spot& sp) { onSpot(sp); };
        // ★ ONE LOG LINE PER SIGNAL SET (Stuart, 2026-11-15: "pull the logs off the lenovo to confirm" 45/170 is what
        //   everybody uses — the log had nothing to count): its frequency, settings, and whether the first try found them
        s->onSet = [this](const rttysk::SignalView& v, bool firstTry, double secs) {
            if (!out_.log) return;
            char b[160];
            std::snprintf(b, sizeof b, "RTTY skimmer: %.2f kHz %s — set %.1f s after it was listed (%s)", rf(v.offsetHz) / 1000.0,
                          v.settings.c_str(), secs, firstTry ? "first try, 45 baud" : "full search");
            out_.log(b);
        };
        // ★ notify_all: a worker above active_ (beta94 setWorkers) would swallow a notify_one and go back to sleep
        s->onWork = [this] { { std::lock_guard<std::mutex> lk(m_); ++wake_; } cv_.notify_all(); };
        { std::lock_guard<std::mutex> lk(m_); sk_ = s; }   // ★ workers hold their own reference: the old one dies with its last job
        lastList_.clear(); pinned_.clear();
    }
    /** ★ A search worker: decoder priority, takes queued jobs from whichever engine is current. */
    void worker(int i) {
        char name[16]; std::snprintf(name, sizeof name, out_.background ? "vibe-bgrttysk%d" : "vibe-rttysk%d", i);
        if (out_.background) vibeBackgroundThread(name); else vibeDecoderThread(name);
        for (;;) {
            std::shared_ptr<rttysk::Skimmer> s;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || (wake_ > 0 && i < active_); });
                if (stop_) return;
                --wake_;
                s = sk_;
            }
            if (!s) continue;
            const uint64_t c0 = threadCpuNs_();
            while (!stopFlag() && s->workOnce()) {}
            if (out_.cpu) out_.cpu(threadCpuNs_() - c0);
        }
    }
    bool stopFlag() { std::lock_guard<std::mutex> lk(m_); return stop_; }
    static uint64_t threadCpuNs_() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
        timespec ts{}; if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
        return 0;
    }
    double rf(double off) const { return centre_ + off; }
    double rfOf(int id, double off) const { const auto it = pinned_.find(id); return it != pinned_.end() ? it->second : rf(off); }
    void onSpot(const rttysk::Spot& s) {
        ++spots_;
        const double hz = rf(s.offsetHz);
        if (out_.spot) out_.spot(s, hz);
        char b[512];
        const std::string call = PskRun::jsonText(s.call, 16), how = PskRun::jsonText(s.how, 8), mode = PskRun::jsonText(s.mode, 16),
                          ctry = PskRun::jsonText(callsign::country(s.call), 40);
        const int n = std::snprintf(b, sizeof b,
            "{\"type\":\"rtty_spot\",\"data\":{\"callsign\":\"%s\",\"frequency\":%.0f,\"mode\":\"%s\",\"snr\":%.0f,"
            "\"how\":\"%s\",\"country\":\"%s\",\"timestamp\":%lld}}",
            call.c_str(), hz, mode.c_str(), s.snrDb, how.c_str(), ctry.c_str(), (long long)s.utcMs);
        if (n > 0 && out_.json) out_.json(std::string(b, (size_t)std::min(n, (int)sizeof b - 1)));
    }
    /** ★ Ticks on the STREAM's clock, as PskRun: a test feeding faster than real time still sees a list a second. */
    void after() {
        const int64_t now = sk_->nowMs();
        if (now - lastTick_ < 250) return;
        lastTick_ = now;
        const uint8_t st = sk_->settled() > 0 ? 3 : sk_->listed() > 0 ? 1 : 0;
        if (st != state_) { state_ = st; if (out_.state) out_.state(st); }
        if (now - lastListAt_ >= 1000) { lastListAt_ = now; sendList(sk_->signals(true)); }
    }
    void sendList(const std::vector<rttysk::SignalView>& v) {
        // ★ Active signals, then the recently gone — at most 60 rows, each with its last 160 characters.
        std::string j = "{\"type\":\"rtty_signals\",\"data\":{\"skimmer\":true";
        char b[400];
        std::snprintf(b, sizeof b, ",\"centre\":%.0f,\"width\":%.0f,\"workers\":%d,\"primary\":0,\"signals\":[", centre_, searchedHz(), nWorkers_); j += b;
        int n = 0;
        for (auto& s : v) {
            if (n >= 60) break;
            if (n) j += ',';
            ++n;
            std::snprintf(b, sizeof b, "{\"id\":%d,\"hz\":%.0f,\"shift\":%.0f,\"set\":%s,\"baud\":%.2f,\"reverse\":%s,\"snr\":%.0f,\"q\":\"%s\","
                          "\"active\":%s,\"last\":%lld,\"first\":%lld,",
                          s.id, rfOf(s.id, s.offsetHz), s.shiftHz, s.set ? "true" : "false", s.baud, s.reverse ? "true" : "false",
                          s.snrDb, rttysk::qualityWord(s.quality), s.active ? "true" : "false", (long long)s.lastMs, (long long)s.firstMs);
            j += b;
            j += "\"settings\":\"" + PskRun::jsonText(s.settings, 40) + "\",\"call\":\"" + PskRun::jsonText(s.call, 16) + "\",\"country\":\""
               + PskRun::jsonText(s.call.empty() ? std::string() : callsign::country(s.call), 40)
               + "\",\"text\":\"" + PskRun::jsonText(s.text, 160) + "\"}";
        }
        j += "]}}";
        if (j == lastList_) return;
        lastList_ = j;
        if (out_.json) out_.json(j);
    }

    double width_;
    Out out_;
    int nWorkers_ = 1, active_ = 1;   // ★ m_: threads started (th_.size()) ≥ active_ = those taking jobs
    int64_t workersAt_ = 0;
    std::shared_ptr<rttysk::Skimmer> sk_;
    double rate_ = 0, centre_ = 0;
    std::unordered_map<int, double> pinned_;
    std::string lastList_;
    int64_t lastTick_ = 0, lastListAt_ = 0;
    uint8_t state_ = 255;
    unsigned long spots_ = 0;
    // workers
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    int wake_ = 0;
    std::vector<std::thread> th_;
};

} // namespace vibe
