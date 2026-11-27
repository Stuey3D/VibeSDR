// decoders/sat_skim.cpp — see sat_skim.h (the satellite digital-mode skimmer, beta97).
#include "sat_skim.h"
#include "../vibe_clock.h"
#include "../vibe_thread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace vibe {
namespace satskim {

namespace {
constexpr float kPcmScale = 16384.0f;   ///< float audio (the tuner's 0.05 RMS) ↔ the queue's int16
uint64_t threadCpuNs() {
    timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
std::string hhmmss(double utc) {
    const time_t t = (time_t)std::floor(utc);
    struct tm g{};
    gmtime_r(&t, &g);
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}
std::string jsonText(const std::string& in, size_t max) {   // printable ASCII, quotes escaped (already whitelisted upstream)
    std::string o;
    for (char ch : in) {
        const unsigned char u = (unsigned char)ch;
        if (u < 0x20 || u >= 0x7f) continue;
        if (o.size() >= max) break;
        if (u == '"' || u == '\\') o.push_back('\\');
        o.push_back(ch);
    }
    return o;
}
} // namespace

std::vector<double> windowDials(double loHz, double hiHz) {
    std::vector<double> d;
    if (!(hiHz > loHz)) return d;
    const double a = loHz - kEdgeHz, b = hiHz + kEdgeHz;
    const double use = kWinAudioHiHz - kWinAudioLoHz;
    int n = 1;
    if (b - a > use) n = (int)std::ceil((b - a - use) / kWinStepHz) + 1;
    n = std::min(n, kMaxWindows);
    const double covered = (n - 1) * kWinStepHz + use;
    const double first = a - 0.5 * (covered - (b - a)) - kWinAudioLoHz;   // ★ centred: the spare split both ends
    for (int i = 0; i < n; ++i) d.push_back(first + i * kWinStepHz);
    return d;
}

// ── buffers, recycled (a window-slot buffer is 360 KB; 30 windows make 120 of them a minute) ──────────────────────────
struct Skimmer::Pool {
    std::mutex m;
    std::vector<std::vector<int16_t>> free;
    size_t keep = 8;
    std::vector<int16_t> get() {
        std::lock_guard<std::mutex> lk(m);
        if (free.empty()) return std::vector<int16_t>((size_t)kSlotSamples, 0);
        std::vector<int16_t> v = std::move(free.back());
        free.pop_back();
        std::fill(v.begin(), v.end(), (int16_t)0);
        return v;
    }
    void put(std::vector<int16_t>&& v) {
        std::lock_guard<std::mutex> lk(m);
        if (free.size() < keep && v.size() == (size_t)kSlotSamples) free.push_back(std::move(v));
    }
};
struct Skimmer::Buf {
    int win = 0;
    double slotUtc = 0;
    int validFrom = 0;            ///< samples before this were never heard (zeros)
    int pos = 0;                  ///< the next sample written (the decode thread's)
    bool ft4aQueued = false;
    int pending = 0;              ///< tasks waiting on it (q_)
    Tag tag;
    std::vector<int16_t> a;
    std::weak_ptr<Pool> pool;
    ~Buf() { if (auto p = pool.lock()) p->put(std::move(a)); }
};
struct Skimmer::Worker {
    std::unique_ptr<Ft8SlotDecoder> d8, d4;
    std::vector<float> scratch;
};

Skimmer::Skimmer(Config c) : c_(std::move(c)), pool_(std::make_shared<Pool>()), ladder_(std::make_shared<digi::LoadLadder>()) {
    double lo = c_.loHz, hi = c_.hiHz;
    if (c_.lsb) { lo = 2 * c_.centreHz - c_.hiHz; hi = 2 * c_.centreHz - c_.loHz; }   // ★ the mirrored frame (see the header)
    dials_ = windowDials(lo, hi);
    for (double d : dials_) {
        std::unique_ptr<Win> w(new Win);
        w->dial = d;
        w->book8.reset(new FtxApBook(false, 40));   // ★ ten minutes of slots, as Ft8Decoder keeps
        w->book4.reset(new FtxApBook(true, 80));
        wins_.push_back(std::move(w));
    }
    capBytes_ = c_.capBytes ? c_.capBytes : defaultCapBytes();
    pool_->keep = std::min<size_t>(2 * dials_.size() + 4, capBytes_ / kBufBytes + dials_.size());
    if (c_.workers > 0) nWorkers_ = c_.workers;
    else if (c_.mode == Mode::CatchUp) nWorkers_ = 1;
    else {
        const long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
        nWorkers_ = (int)std::max(1L, std::min(4L, cores - 1));
    }
    nWorkers_ = std::max(1, std::min(8, nWorkers_));
    const size_t tps = std::max<size_t>(1, tasksPerSlot_());
    perTaskMs_ = kSlotSec * 1000.0 * nWorkers_ / (double)tps;
    ladder_->setLog([this](const std::string& l) { if (c_.log) c_.log("SAT SKIM: " + l.substr(l.rfind("Band: ", 0) == 0 ? 6 : 0)); });
    for (int i = 0; i < nWorkers_; ++i) workers_.emplace_back(new Worker);
    for (int i = 0; i < nWorkers_; ++i) threads_.emplace_back([this, i] { workerLoop_(i); });
    if (c_.log) {
        char b[260];
        std::snprintf(b, sizeof b, "SAT SKIM: %d windows over %.4f\xe2\x80\x93%.4f MHz (%s), FT8%s, %s, %d worker%s, catch-up queue up to %.0f MB",
                      (int)dials_.size(), c_.loHz / 1e6, c_.hiHz / 1e6, c_.lsb ? "LSB downlink, mirrored" : "USB downlink",
                      c_.ft4 ? " + FT4" : "", modeName(c_.mode), nWorkers_, nWorkers_ == 1 ? "" : "s", capBytes_ / 1048576.0);
        c_.log(b);
    }
}

Skimmer::~Skimmer() {
    abort_.store(true);
    {
        std::lock_guard<std::mutex> lk(q_);
        stop_ = true;
        tasks_.clear();
        queuedBytes_ = 0;
    }
    qcv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    ladder_->forget(this);
}

size_t Skimmer::defaultCapBytes() {
    const long pages = ::sysconf(_SC_PHYS_PAGES), page = ::sysconf(_SC_PAGESIZE);
    double ram = pages > 0 && page > 0 ? (double)pages * (double)page : 512.0 * 1048576.0;
    const double cap = std::max(8.0 * 1048576.0, std::min(64.0 * 1048576.0, ram / 24.0));
    return (size_t)cap;
}

Skimmer::Mode Skimmer::defaultMode(double budgetUnits, int windows) {
    if (!std::isfinite(budgetUnits) || budgetUnits <= 0) return Mode::Live;   // ★ no benchmark: live (the governor still governs)
    const double need = std::max(1, windows) * 2.0 * 0.6;   // ★ FT8 + FT4, each at its light cost (digimodes/ft8.h)
    return budgetUnits >= need ? Mode::Live : Mode::CatchUp;
}

double Skimmer::rfOf(int window, double audioHz) const {
    if (window < 0 || window >= (int)dials_.size()) return 0;
    const double f = dials_[(size_t)window] + audioHz;
    return c_.lsb ? 2 * c_.centreHz - f : f;
}

void Skimmer::build_(double rate) {
    rate_ = rate;
    for (size_t i = 0; i < wins_.size(); ++i) {
        Win& w = *wins_[i];
        w.cur.reset();
        if (digi::DigiTuner::fits(w.dial, kTunerLoHz, kTunerHiHz, rate, c_.centreHz)) w.tuner.reset(new digi::DigiTuner(w.dial, kTunerLoHz, kTunerHiHz, kRate));
        else {
            w.tuner.reset();
            if (c_.log) { char b[160]; std::snprintf(b, sizeof b, "SAT SKIM: window %d (%.4f MHz) lies outside the %.0f kS/s slice \xe2\x80\x94 not skimmed", (int)i, w.dial / 1e6, rate / 1000); c_.log(b); }
        }
    }
    inTotal_ = 0; outTotal_ = 0; anchored_ = false;
}

void Skimmer::startBuf_(Win& w, int wi, double slotUtc, int pos) {
    auto b = std::make_shared<Buf>();
    b->win = wi; b->slotUtc = slotUtc;
    b->pos = std::max(0, std::min(kSlotSamples, pos));
    b->validFrom = b->pos;
    b->pool = pool_;
    b->a = pool_->get();
    if (c_.tag) b->tag = c_.tag();
    w.cur = std::move(b);
}

void Skimmer::feed(const cf32* x, int n, double rate, double captureUtc) {
    if (!x || n <= 0 || !(rate > 0) || finished_ || !on_.load(std::memory_order_relaxed)) return;
    if (!std::isfinite(captureUtc)) captureUtc = vibeUtcNow();
    if (std::fabs(rate - rate_) > 0.5) build_(rate);
    // ★ the anchor: capture UTC of input sample 0 (= output sample 0); an earlier stamp is better (taken at once), a later
    //   one leaks in over 30 s; a jump of a second either way is lost audio or a clock step — re-anchored, slots abandoned
    const double est = captureUtc - (double)(inTotal_ + n) / rate;
    bool reanchor = !anchored_;
    if (anchored_ && std::fabs(est - utc0_) > 1.0) {
        reanchor = true;
        if (c_.log) c_.log("SAT SKIM: the stream's clock jumped (" + std::to_string((int)std::lround((est - utc0_) * 1000)) + " ms) \xe2\x80\x94 the slots being cut start again");
    }
    if (reanchor) {
        // ★ output k ↔ utc0_ + k / 12000 (the tuners make exactly 12000/rate of each input sample from the same start)
        utc0_ = est;
        anchored_ = true;
        for (auto& w : wins_) w->cur.reset();
    } else if (est < utc0_) utc0_ = est;
    else utc0_ += (est - utc0_) * std::min(1.0, (double)n / rate / 30.0);
    inTotal_ += n;
    lastCapture_ = captureUtc;

    const cf32* in = x;
    if (c_.lsb) {   // ★ an LSB downlink: mirror about 0 Hz (the span's centre) so it is cut as USB
        if (mir_.size() < (size_t)n) mir_.resize((size_t)n);
        for (int i = 0; i < n; ++i) mir_[(size_t)i] = std::conj(x[i]);
        in = mir_.data();
    }
    std::vector<Task> ready;
    int64_t produced = -1;
    for (size_t wi = 0; wi < wins_.size(); ++wi) {
        Win& w = *wins_[wi];
        if (!w.tuner) continue;
        w.out.clear();
        w.tuner->feed(in, n, rate, c_.centreHz, w.out);
        const int m = (int)w.out.size();
        if (produced < 0) produced = m;
        // ★ every window's tuner runs at the same rate from the same start, so output index k is the same instant in each
        for (int k = 0; k < m;) {
            const double t = utc0_ + (double)(outTotal_ + k) / kRate;
            if (!w.cur) {
                const double s = std::floor(t / kSlotSec) * kSlotSec;
                const int pos = (int)std::lround((t - s) * kRate);
                startBuf_(w, (int)wi, s, pos >= kSlotSamples ? kSlotSamples - 1 : pos);
            }
            Buf& b = *w.cur;
            const int room = kSlotSamples - b.pos;
            const int take = std::min(room, m - k);
            int16_t* dst = b.a.data() + b.pos;
            const float* src = w.out.data() + k;
            for (int i = 0; i < take; ++i) {
                float v = src[i] * kPcmScale;   // ★ the tuner levels to 0.05 RMS: 40× headroom before int16 clips
                v = v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v;
                dst[i] = (int16_t)std::lrint(v);
            }
            const int before = b.pos;
            b.pos += take; k += take;
            // FT4's first slot complete
            if (c_.ft4 && !b.ft4aQueued && before < kHalfSamples && b.pos >= kHalfSamples) {
                b.ft4aQueued = true;
                if (b.validFrom <= kStartSlackSamples) ready.push_back(Task{w.cur, kFt4a});
            }
            if (b.pos >= kSlotSamples) {
                if (c_.ft8 && b.validFrom <= kStartSlackSamples) ready.push_back(Task{w.cur, kFt8});
                if (c_.ft4 && b.validFrom <= kHalfSamples + kStartSlackSamples) ready.push_back(Task{w.cur, kFt4b});
                const double next = b.slotUtc + kSlotSec;
                // ★ the sample clock against UTC: the next slot follows on sample for sample, unless the anchor says the
                //   count has wandered a tenth of a second — then it is placed from the anchor afresh (the next loop turn)
                const double tNext = utc0_ + (double)(outTotal_ + k) / kRate;
                const int expect = (int)std::lround((tNext - next) * kRate);
                if (std::abs(expect) > kRate / 10) w.cur.reset();
                else startBuf_(w, (int)wi, next, 0);
            }
        }
    }
    if (produced > 0) outTotal_ += produced;
    if (!ready.empty()) enqueue_(ready);
    if (captureUtc - lastJudge_ >= 1.0) { lastJudge_ = captureUtc; judge_(captureUtc); }
}

void Skimmer::gap() {
    for (auto& w : wins_) w->cur.reset();
    anchored_ = false;
}

void Skimmer::setOn(bool on) {
    const bool was = on_.exchange(on);
    if (was && !on) gap();
    if (was != on && c_.log) c_.log(on ? "SAT SKIM: on" : "SAT SKIM: off (what is queued is still decoded)");
}

void Skimmer::finish() {
    on_.store(false);
    finished_ = true;
    for (auto& w : wins_) w->cur.reset();
}

bool Skimmer::idle() const {
    std::lock_guard<std::mutex> lk(q_);
    return tasks_.empty() && busy_ == 0;
}
bool Skimmer::waitIdle(double sec) const {
    std::unique_lock<std::mutex> lk(q_);
    return idleCv_.wait_for(lk, std::chrono::milliseconds((long long)(sec * 1000)), [this] { return tasks_.empty() && busy_ == 0; });
}

void Skimmer::enqueue_(std::vector<Task>& ts) {
    {
        std::lock_guard<std::mutex> lk(q_);
        if (stop_) return;
        for (auto& t : ts) {
            if (t.buf->pending++ == 0) queuedBytes_ += kBufBytes;
            tasks_.push_back(std::move(t));
        }
        while (queuedBytes_ > capBytes_ && !tasks_.empty()) dropOldestLocked_();
        peakBytes_ = std::max(peakBytes_, queuedBytes_);
    }
    qcv_.notify_all();
}

void Skimmer::dropOldestLocked_() {
    double oldest = 1e300;
    for (auto& t : tasks_) oldest = std::min(oldest, t.buf->slotUtc);
    size_t n = 0;
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->buf->slotUtc == oldest) {
            if (--it->buf->pending == 0) queuedBytes_ -= kBufBytes;
            it = tasks_.erase(it); ++n;
        } else ++it;
    }
    {
        std::lock_guard<std::mutex> rl(r_);
        tasksDropped_ += n; slotsDropped_++;
    }
    if (c_.log) {
        char b[220];
        std::snprintf(b, sizeof b, "SAT SKIM: the catch-up queue is full (%.0f MB) \xe2\x80\x94 the oldest slot, %s UTC, dropped undecoded (%zu decodes)",
                      capBytes_ / 1048576.0, hhmmss(oldest).c_str(), n);
        c_.log(b);
    }
}

void Skimmer::judge_(double captureUtc) {
    double oldestEnd = 1e300; size_t qb = 0;
    {
        std::lock_guard<std::mutex> lk(q_);
        for (auto& t : tasks_) oldestEnd = std::min(oldestEnd, t.buf->slotUtc + (t.kind == kFt4a ? kSlotSec / 2 : kSlotSec));
        qb = queuedBytes_;
    }
    bool behind;
    std::string why;
    if (c_.mode == Mode::CatchUp) {
        behind = qb * 2 > capBytes_;
        why = "the catch-up queue over half full";
    } else {
        behind = oldestEnd < 1e299 && captureUtc - oldestEnd > kSlotSec + 2.0;
        why = "a slot still waiting a slot later";
    }
    if (behind) ladder_->behind(captureUtc, why);
    else ladder_->calm(captureUtc, this, 0);
}

void Skimmer::workerLoop_(int idx) {
    if (c_.background) vibeBackgroundThread("vibe-satskim");
    else vibeDecoderLagThread("vibe-satskim");
    Worker& wk = *workers_[(size_t)idx];
    std::unique_lock<std::mutex> lk(q_);
    for (;;) {
        qcv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
        if (stop_) return;
        Task t = std::move(tasks_.front());
        tasks_.pop_front();
        if (--t.buf->pending == 0) queuedBytes_ -= kBufBytes;
        ++busy_;
        lk.unlock();
        decode_(wk, t);
        t.buf.reset();
        lk.lock();
        --busy_;
        if (tasks_.empty() && busy_ == 0) idleCv_.notify_all();
    }
}

void Skimmer::decode_(Worker& wk, const Task& t) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = threadCpuNs();
    const bool ft4 = t.kind != kFt8;
    std::unique_ptr<Ft8SlotDecoder>& dp = ft4 ? wk.d4 : wk.d8;
    if (!dp) dp.reset(new Ft8SlotDecoder(kRate, ft4 ? FtxMode::FT4 : FtxMode::FT8));
    Ft8SlotDecoder& d = *dp;
    const int off = t.kind == kFt4b ? kHalfSamples : 0;
    const int nWin = std::min(d.windowSamples(), kSlotSamples - off);
    if ((int)wk.scratch.size() < d.windowSamples()) wk.scratch.resize((size_t)d.windowSamples());
    const int16_t* src = t.buf->a.data() + off;
    for (int i = 0; i < nWin; ++i) wk.scratch[(size_t)i] = (float)src[i] * (1.0f / kPcmScale);   // ★ back to the tuner's own level (what Band mode decodes)
    for (int i = nWin; i < d.windowSamples(); ++i) wk.scratch[(size_t)i] = 0.0f;
    const double slotUtc = t.buf->slotUtc + (t.kind == kFt4b ? kSlotSec / 2 : 0.0);
    const int depth = ladder_->depth();
    const bool loaded = c_.loaded ? c_.loaded() : false;
    Win& w = *wins_[(size_t)t.buf->win];
    std::vector<Ft8Result> res;
    Ft8SlotStats st;
    {
        std::lock_guard<std::mutex> bl(w.bookMtx);   // ★ the window's books are this worker's while it decodes
        FtxApBook* book = ft4 ? w.book4.get() : w.book8.get();
        Ft8SlotOptions o = Ft8Decoder::liveOptions(ft4 ? FtxMode::FT4 : FtxMode::FT8, depth, loaded, 3, book);
        o.abort = &abort_;
        const double budget = perTaskMs_;
        o.mayRunPass = [&](int, double elapsedMs, double lastMs) {
            const bool ld = (c_.loaded ? c_.loaded() : false) || depth >= 3;
            return !ld && elapsedMs + 1.5 * lastMs < Ft8Decoder::kPassBudget * budget;   // ★ Ft8Decoder::passAllowed's rule, on this task's share
        };
        d.decode(wk.scratch.data(), d.windowSamples(), o, res, st);
    }
    if (abort_.load()) return;
    const double now = vibeUtcNow();
    const bool late = now - (slotUtc + (ft4 ? kSlotSec / 2 : kSlotSec)) >= kLateSec;
    for (const Ft8Result& r : res) {
        std::string call, grid, text;
        if (!Ft8Decoder::spotFields(r, call, grid, text)) continue;
        const double rf = rfOf(t.buf->win, r.hz);
        const std::string mode = ft4 ? "FT4" : "FT8";
        {
            std::lock_guard<std::mutex> rl(r_);
            char kb[48]; std::snprintf(kb, sizeof kb, "%s|%.1f|", mode.c_str(), slotUtc);
            const std::string key = kb + text;
            if (seen_.count(key)) { ++dupes_; continue; }   // ★ the neighbouring window's copy of the same transmission
            seen_[key] = slotUtc;
            for (auto it = seen_.begin(); it != seen_.end();) { if (it->second < slotUtc - 600) it = seen_.erase(it); else ++it; }
            ++spots_; if (late) ++late_;
            Heard h; h.mode = mode; h.call = call; h.grid = grid; h.text = text; h.snr = r.snr; h.rfHz = rf;
            h.slotUtc = slotUtc; h.decodedUtc = now; h.late = late;
            heard_.push_front(h);
            while (heard_.size() > kHeardMax) heard_.pop_back();
        }
        if (c_.emit) {
            digi::Spot sp;
            sp.mode = mode; sp.call = call; sp.grid = grid; sp.snr = r.snr; sp.text = text;
            sp.slotUtc = slotUtc; sp.dt = r.dt; sp.rfHz = rf;
            sp.audioHz = (float)r.hz;
            c_.emit(sp, rf, t.buf->tag);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const uint64_t cpu = threadCpuNs() - c0;
    std::lock_guard<std::mutex> rl(r_);
    ++tasksDecoded_; lastTaskMs_ = ms; sumTaskMs_ += ms; cpuNs_ += cpu;
}

Skimmer::Stats Skimmer::stats() const {
    Stats s;
    s.mode = c_.mode; s.windows = (int)dials_.size(); s.workers = nWorkers_; s.depth = ladder_->depth(); s.capBytes = capBytes_;
    {
        std::lock_guard<std::mutex> lk(q_);
        s.queuedTasks = tasks_.size(); s.queuedBytes = queuedBytes_; s.peakBytes = peakBytes_;
        std::set<double> slots;
        double oldestEnd = 1e300;
        for (auto& t : tasks_) { slots.insert(t.buf->slotUtc); oldestEnd = std::min(oldestEnd, t.buf->slotUtc + (t.kind == kFt4a ? kSlotSec / 2 : kSlotSec)); }
        s.queuedSlots = slots.size();
        s.lagSec = oldestEnd < 1e299 ? std::max(0.0, lastCapture_ - oldestEnd) : 0.0;
    }
    std::lock_guard<std::mutex> rl(r_);
    s.tasksDecoded = tasksDecoded_; s.tasksDropped = tasksDropped_; s.slotsDropped = slotsDropped_;
    s.spots = spots_; s.lateSpots = late_; s.dupes = dupes_;
    s.lastTaskMs = lastTaskMs_;
    s.meanTaskMs = tasksDecoded_ ? sumTaskMs_ / (double)tasksDecoded_ : 0.0;
    const size_t tps = tasksPerSlot_();
    s.cpuMsPerSlot = tasksDecoded_ ? (double)cpuNs_ / 1e6 / (double)tasksDecoded_ * (double)tps : 0.0;
    return s;
}

std::vector<Heard> Skimmer::heard(size_t max) const {
    std::lock_guard<std::mutex> rl(r_);
    std::vector<Heard> v;
    for (auto& h : heard_) { if (v.size() >= max) break; v.push_back(h); }
    return v;
}

std::string Skimmer::statusJson(bool waitingForLock) const {
    const Stats s = stats();
    char b[400];
    std::snprintf(b, sizeof b,
        "{\"on\":%s,\"mode\":\"%s\",\"windows\":%d,\"workers\":%d,\"depth\":%d,\"queued\":%zu,\"qslots\":%zu,\"qmb\":%.1f,\"capmb\":%.0f,"
        "\"lag\":%.0f,\"dropped\":%llu,\"decoded\":%llu,\"spots\":%llu,\"late\":%llu%s",
        on_.load() ? "true" : "false", modeName(s.mode), s.windows, s.workers, s.depth, s.queuedTasks, s.queuedSlots,
        s.queuedBytes / 1048576.0, s.capBytes / 1048576.0, s.lagSec, (unsigned long long)s.slotsDropped,
        (unsigned long long)s.tasksDecoded, (unsigned long long)s.spots, (unsigned long long)s.lateSpots,
        waitingForLock ? ",\"wait\":\"doppler\"" : "");
    std::string j = b;
    j += ",\"heard\":[";
    bool first = true;
    for (const Heard& h : heard(20)) {
        char e[160];
        std::snprintf(e, sizeof e, "%s{\"t\":%.1f,\"m\":\"%s\",\"c\":\"%s\",\"g\":\"%s\",\"s\":%d,\"f\":%.0f%s,\"x\":\"",
                      first ? "" : ",", h.slotUtc, h.mode.c_str(), jsonText(h.call, 12).c_str(), jsonText(h.grid, 6).c_str(), h.snr, h.rfHz,
                      h.late ? ",\"late\":true" : "");
        j += e;
        j += jsonText(h.text, 40);
        j += "\"}";
        first = false;
    }
    j += "]}";
    return j;
}

void Skimmer::retire(std::shared_ptr<Skimmer> s, double maxSec) {
    if (!s) return;
    s->finish();
    if (s->idle()) return;
    std::thread([s, maxSec]() mutable {
        vibeBackgroundThread("vibe-satskim-r");
        s->waitIdle(maxSec);
        s.reset();
    }).detach();
}

} // namespace satskim
} // namespace vibe
