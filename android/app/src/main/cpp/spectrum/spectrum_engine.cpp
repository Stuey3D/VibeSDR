// VibeSDR — SpectrumEngine. See spectrum_engine.h; the behaviour it reproduces is specified, with WaterfallView.tsx
// line numbers, in docs/NATIVE-SPECTRUM-ENGINE.md ("WaterfallView per-frame behaviour"). Names follow the TS.
#include "spectrum_engine.h"
#include "wf_row_target.h"
#include "../vibedsp/vibe_denormals.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#if defined(__ANDROID__) || defined(__linux__)
#include <sys/prctl.h>
#include <sys/resource.h>
#endif

namespace vibespec {

namespace {
inline double jround(double x) { return std::floor(x + 0.5); }          // Math.round
inline int32_t toInt32(double x) { return std::isfinite(x) ? (int32_t)std::trunc(x) : 0; }   // `| 0` (in-range values)
double wallMs() {
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() / 1000.0;
}
}  // namespace

SpectrumEngine::SpectrumEngine(MakeImage mi, MakePath mp, Release ri, Release rp, Clock clock)
    : makeImage_(mi), makePath_(mp), releaseImage_(ri), releasePath_(rp), clock_(clock) {
    thread_ = std::thread([this] { loop(); });
}

SpectrumEngine* SpectrumEngine::makeForTest(MakeImage mi, MakePath mp, Release ri, Release rp, Clock clock) {
    // Same object, no thread: the test drives runDue() through testStep().
    struct NoThread {};
    auto* e = new SpectrumEngine(mi, mp, ri, rp, clock);
    {
        std::lock_guard<std::mutex> lk(e->mx_);
        e->stop_ = true;
    }
    e->cv_.notify_all();
    if (e->thread_.joinable()) e->thread_.join();
    e->stop_ = false;
    e->threaded_ = false;
    return e;
}

SpectrumEngine::~SpectrumEngine() {
    {
        std::lock_guard<std::mutex> lk(mx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (outImage_) releaseImage_(outImage_);
    if (outPeak_) releasePath_(outPeak_);
}

double SpectrumEngine::now() const { return clock_ ? clock_() : wallMs(); }

// ── From JS ─────────────────────────────────────────────────────────────────────────────────────────────────────

void SpectrumEngine::pushFrameNative(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz) {
    if (!bins || n <= 0) return;
    {
        std::lock_guard<std::mutex> lk(mx_);
        nativeFedUntil_ = now() + NATIVE_FED_MS;
    }
    pushFrameImpl(bins, n, centerHz, bwHz, trueCenterHz);
}

void SpectrumEngine::pushFrame(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz) {
    if (!bins || n <= 0) return;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (now() < nativeFedUntil_) return;   // the native socket already gave us this frame, on time
    }
    pushFrameImpl(bins, n, centerHz, bwHz, trueCenterHz);
}

void SpectrumEngine::pushFrameImpl(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz) {
    std::lock_guard<std::mutex> lk(mx_);
    Frame f;
    if (!framePool_.empty()) { f = std::move(framePool_.back()); framePool_.pop_back(); }
    f.bins.assign(bins, bins + n);
    f.centerHz = centerHz; f.bwHz = bwHz; f.trueCenterHz = trueCenterHz;
    f.arrivedMs = now();
    inbox_.push_back(std::move(f));
    // A runaway producer (the engine thread starved) must not grow memory: keep the newest few.
    while (inbox_.size() > 8) { framePool_.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
    cv_.notify_one();
}

void SpectrumEngine::setView(double c, double bw) { std::lock_guard<std::mutex> lk(mx_); viewCenterHz_ = c; viewBwHz_ = bw; }
void SpectrumEngine::setConfig(const EngineConfig& c) { std::lock_guard<std::mutex> lk(mx_); cfg_ = c; }
void SpectrumEngine::setSettings(const vibedsp::SpectrumProcessorSettings& s) {
    std::lock_guard<std::mutex> lk(mx_); pendingSettings_ = s; settingsDirty_ = true; cv_.notify_one();
}
void SpectrumEngine::setLastInteractAt(double ms) { std::lock_guard<std::mutex> lk(mx_); lastInteractAt_ = ms; }
void SpectrumEngine::setBackground(bool bg) { std::lock_guard<std::mutex> lk(mx_); bg_ = bg; cv_.notify_one(); }
void SpectrumEngine::setCovered(bool c) { std::lock_guard<std::mutex> lk(mx_); covered_ = c; cv_.notify_one(); }

bool SpectrumEngine::liveRange(int& dbMin, int& dbMax) {
    std::lock_guard<std::mutex> lk(mx_);
    dbMin = liveMin_; dbMax = liveMax_;
    const bool c = liveChanged_; liveChanged_ = false;
    return c;
}

// ── From the UI thread ──────────────────────────────────────────────────────────────────────────────────────────

bool SpectrumEngine::pollUi(UiUpdate& out) {
    std::lock_guard<std::mutex> lk(mx_);
    if (outVer_ == uiSeenVer_) return false;
    uiSeenVer_ = outVer_;
    out.image = outImage_; outImage_ = nullptr;            // ownership moves to the UI
    out.head = outHead_; out.texReady = texReady_;
    out.quant = outQuant_; out.avgFrameMs = outAvgMs_;
    out.scrollGen = scrollGen_; out.glide = glide_; out.glideDur = glideDur_;
    out.specGen = specGen_; out.specHidden = specHidden_;
    if (specGen_ != uiSpec_) { out.specTarget = outSpec_; uiSpec_ = specGen_; }
    out.peakGen = peakGen_; out.peakPath = outPeak_; outPeak_ = nullptr;
    return true;
}

// ── Engine thread ───────────────────────────────────────────────────────────────────────────────────────────────

void SpectrumEngine::loop() {
    vibe::vibeFlushDenormals();   // ★ its smoothing is recursive — see vibedsp/vibe_denormals.h
#if defined(__ANDROID__) || defined(__linux__)
    prctl(PR_SET_NAME, "vibe-spectrum");
    setpriority(PRIO_PROCESS, 0, -5);   // vibe_thread.h's SPECTRUM rank: below network and audio, above decoders
#endif
    std::unique_lock<std::mutex> lk(mx_);
    while (!stop_) {
        const double t = now();
        double next = -1;
        if (jbDueAt_ >= 0) next = jbDueAt_;
        if (extraDueAt_ >= 0 && (next < 0 || extraDueAt_ < next)) next = extraDueAt_;
        const bool work = !inbox_.empty() || settingsDirty_ || bg_ != bgNow_ || covered_ != covNow_ || (next >= 0 && next <= t);
        if (!work) {
            const double waitMs = next < 0 ? 1000 : std::max(1.0, next - t);
            cv_.wait_for(lk, std::chrono::microseconds((long long)(waitMs * 1000)));
            continue;
        }
        lk.unlock();
        runDue(now());
        lk.lock();
    }
}

void SpectrumEngine::testStep(double nowMs) { runDue(nowMs); }

void SpectrumEngine::runDue(double now) {
    std::deque<Frame> arrived;
    bool applySettings = false;
    vibedsp::SpectrumProcessorSettings ps;
    bool wasCov = covNow_, wasBg = bgNow_;
    {
        std::lock_guard<std::mutex> lk(mx_);
        cfgNow_ = cfg_; viewC_ = viewCenterHz_; viewBw_ = viewBwHz_; interactAt_ = lastInteractAt_;
        bgNow_ = bg_; covNow_ = covered_;
        arrived.swap(inbox_);
        if (settingsDirty_) { ps = pendingSettings_; settingsDirty_ = false; applySettings = true; }
    }
    if (applySettings) proc_.applySettings(ps);
    // ★ Background / cover STOP the repeat rows (the JS timer kept pushing up to 8 after either — and after unmount).
    if ((bgNow_ && !wasBg) || (covNow_ && !wasCov)) { extrasLeft_ = 0; extraDueAt_ = -1; }
    if (bgNow_ && !wasBg) {                                     // drainFrame's background branch, at once
        for (auto& f : jbQueue_) framePool_.push_back(std::move(f));
        jbQueue_.clear(); jbPrefill_ = true; jbDueAt_ = -1;
    }
    if (!covNow_ && wasCov && ringDirty_) publishRing();        // uncover: ONE texture from the ring as it stands

    // ★★ A WALL CLOCK PUT BACK strands every deadline in the future: the next drain would wait the whole
    //    step (minutes) with frames queued behind it — a frozen waterfall. No deadline is ever legitimately
    //    more than a second ahead (drain caps dur at 1000 ms), so one that is was set by the old clock.
    //  ★ The clock stays wall time because lastInteractAt arrives from JS as Date.now(); moving both to a
    //    monotonic clock is the proper follow-up.
    if (jbDueAt_ > now + 1000) jbDueAt_ = now;
    if (extraDueAt_ > now + 1000) extraDueAt_ = now;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (nativeFedUntil_ > now + NATIVE_FED_MS) nativeFedUntil_ = 0;
    }
    for (auto& f : arrived) enqueue(std::move(f), f.arrivedMs);
    if (jbDueAt_ >= 0 && now >= jbDueAt_) { jbDueAt_ = -1; drain(now); }
    if (extraDueAt_ >= 0 && now >= extraDueAt_) {
        emitRepeatRow();
        extrasLeft_--;
        extraDueAt_ = extrasLeft_ > 0 ? now + extraSpacing_ : -1;
    }
    std::lock_guard<std::mutex> lk(mx_);
    while (framePool_.size() > 6) framePool_.pop_back();
}

// enqueueFrame (WaterfallView 1612-1637)
void SpectrumEngine::enqueue(Frame&& f, double now) {
    if (bgNow_) { std::lock_guard<std::mutex> lk(mx_); framePool_.push_back(std::move(f)); return; }
    const bool interacting = now - interactAt_ < 1000;   // ★ NOT gated by smoothTune, as in the TS
    if (interacting) {
        jbDueAt_ = -1;
        { std::lock_guard<std::mutex> lk(mx_); for (auto& q : jbQueue_) framePool_.push_back(std::move(q)); }
        jbQueue_.clear();
        jbPrefill_ = true;
        jbLastArrival_ = now;
        handleFrame(f, now);
        std::lock_guard<std::mutex> lk(mx_); framePool_.push_back(std::move(f));
        return;
    }
    if (jbLastArrival_ > 0) jbArrivalMs_ = jbArrivalMs_ * 0.8 + (now - jbLastArrival_) * 0.2;
    jbLastArrival_ = now;
    jbQueue_.push_back(std::move(f));
    if (jbQueue_.size() > 3) {
        std::lock_guard<std::mutex> lk(mx_);
        framePool_.push_back(std::move(jbQueue_.front()));
        jbQueue_.pop_front();
    }
    if (jbDueAt_ < 0) jbDueAt_ = now;   // setTimeout(drainFrame, 0)
}

// drainFrame (1594-1610)
void SpectrumEngine::drain(double now) {
    if (bgNow_) {
        std::lock_guard<std::mutex> lk(mx_);
        for (auto& f : jbQueue_) framePool_.push_back(std::move(f));
        jbQueue_.clear(); jbPrefill_ = true; return;
    }
    if (jbPrefill_) {
        if (!jbQueue_.empty()) jbPrefill_ = false;
        else return;
    }
    if (jbQueue_.empty()) { jbPrefill_ = true; return; }
    Frame f = std::move(jbQueue_.front());
    jbQueue_.pop_front();
    handleFrame(f, now);
    { std::lock_guard<std::mutex> lk(mx_); framePool_.push_back(std::move(f)); }
    const double dur = std::max(40.0, std::min(1000.0, jbQueue_.size() > 1 ? jbArrivalMs_ * 0.6 : jbArrivalMs_));
    jbDueAt_ = now + dur;
}

// handleFrame (1271-1574)
void SpectrumEngine::handleFrame(const Frame& f, double now) {
    if (bgNow_) return;
    const EngineConfig& cfg = cfgNow_;
    const int n = (int)f.bins.size();
    if (n == 0 || cfg.width < 4) return;
    const bool interacting = cfg.smoothTune && now - interactAt_ < 1000;
    const vibedsp::SpectrumFrameOut fr = proc_.process(f.bins.data(), n, f.centerHz, f.bwHz, interacting, now);

    {   // setLiveRange, 2 dB hysteresis
        const int rMin = (int)jround(fr.dbMin), rMax = (int)jround(fr.dbMax);
        std::lock_guard<std::mutex> lk(mx_);
        if (!(std::abs(liveMin_ - rMin) < 2 && std::abs(liveMax_ - rMax) < 2)) {
            liveMin_ = rMin; liveMax_ = rMax; liveChanged_ = true;
        }
    }
    const bool boost = interacting;
    if (lastFrameTs_ > 0 && !boost) avgFrameMs_ = avgFrameMs_ * 0.8 + (now - lastFrameTs_) * 0.2;
    lastFrameTs_ = now;
    const bool lowFps = avgFrameMs_ > 66;
    // ★★★ NO GLIDE WHILE REPEAT ROWS CARRY THE SCROLL (Stuart, XCover 4s on an 8 fps server, 2026-10-12: "about 5
    //   seconds of beautiful smooth scroll then juddering mess again"). Those 5 s were the frame-time average still
    //   below 66 ms: whole-row steps only, evenly spaced. Once it settled at 125 ms the glide switched on as well, and
    //   a glide over the frame interval with whole rows landing on top of it is two motions fighting. When the setting
    //   asks for more rows than the feed sends (Default 30, Smooth 40 from 8 fps), the repeat rows ARE the motion —
    //   even one-row steps, as the Mac shows on the same server. Sharp (no repeat rows) keeps its glide; a gesture
    //   always glides.
    // ★★★ THE TARGET FOLLOWS THE FEED (Stuart 10-20, wf_row_target.h): cfg.targetRows is the setting's CAP (20 Default,
    //     60 Smooth); the rows actually aimed for are min(cap, 3x / 6x the measured feed), snapped to a divisor of 60,
    //     with hysteresis. 0 = actual rows. The fractional carry (rowCarry_) runs on across a change: no jump.
    if (cfg.targetRows != rowTargetCap_) { rowTargetSt_ = {}; rowTargetCap_ = cfg.targetRows; }   // a new setting decides afresh
    const double rowTarget = avgFrameMs_ > 0 ? vibewf::stepRowTarget(rowTargetSt_, cfg.targetRows, 1000.0 / avgFrameMs_)
                                             : rowTargetSt_.target;
    const bool repeatsCarry = rowTarget > 0;
    const bool wfBoost = boost || (cfg.smoothTune && lowFps && !repeatsCarry);

    if (lastWfPushAt_ > 0) {
        const double wfDt = now - lastWfPushAt_;
        if (wfDt > 0 && wfDt < 2000) avgWfGapMs_ = avgWfGapMs_ > 0 ? avgWfGapMs_ * 0.8 + wfDt * 0.2 : wfDt;
    }
    lastWfPushAt_ = now;
    const double rowCentre = std::isnan(f.trueCenterHz) ? f.centerHz : f.trueCenterHz;
    const double rowHzBin = (f.bwHz > 0 && n > 0) ? f.bwHz / n : 0;
    // ★★ The row is published TOGETHER with the scroll state below (one lock, one version): the UI must never see
    //   the new head without the glide restart that goes with it. Apart, a poll between the two showed the head
    //   one row on with the old glide still at 1 — the whole waterfall a row forward for one frame, then back —
    //   at every real frame: the judder on an 8 fps feed (XCover 4s, 2026-10-12). The JS path does both in one
    //   synchronous call, so it never could.
    pushRow(fr.row, n, rowCentre, rowHzBin, false);
    lastRow_.assign(fr.row, fr.row + n);
    lastRowCenterHz_ = rowCentre; lastRowHzPerBin_ = rowHzBin;

    if (covNow_) return;
    const double revealMs = avgWfGapMs_ > 0 ? avgWfGapMs_ : avgFrameMs_;
    const double dur = std::max(50.0, std::min(1000.0, revealMs));
    const double rowMs = rowTarget > 0 ? 1000.0 / rowTarget : 0;
    int extras = 0;
    if (rowMs > 0) {
        rowCarry_ += dur / rowMs;
        extras = std::max(0, (int)std::floor(rowCarry_) - 1);
        rowCarry_ -= std::floor(rowCarry_);
        // ★ The rule never asks for more than 6 rows a frame (Stuart 10-20, wf_row_target.h), so 8 is headroom
        //   again — kept as the stall guard it always was.
        if (extras > 8) extras = 8;
    }
    // scheduleExtraRows: always clear the pending chain first
    extrasLeft_ = extras;
    extraSpacing_ = dur / (extras + 1);
    extraDueAt_ = (extras > 0 && extraSpacing_ > 0) ? now + extraSpacing_ : -1;

    void* img = covNow_ ? nullptr : makeRingImage();
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (img) storeRingImageLocked(img);
        outQuant_ = wfBoost ? 0 : 1;
        outAvgMs_ = avgFrameMs_;
        scrollGen_++; glide_ = wfBoost; glideDur_ = std::max(1.0, dur);
        outVer_++;
    }
    buildSpectrum(fr, boost);
}

// pushRow (851-941)
void SpectrumEngine::pushRow(const uint8_t* src, int srcN, double rowCenterHz, double rowHzPerBin, bool publish) {
    const uint8_t* row = src;
    int n = srcN;
    if (srcN != RING_W) {
        rowBuf_.resize(RING_W);
        uint8_t* dst = rowBuf_.data();
        const double step = (double)srcN / RING_W;
        if (srcN > RING_W) {
            for (int i = 0; i < RING_W; i++) {
                const int a = toInt32(i * step);
                int b = std::min(srcN, (int)toInt32((i + 1) * step));
                if (b == 0) b = a + 1;   // `|| a+1`
                uint8_t m = 0;
                for (int j = a; j < b; j++) if (src[j] > m) m = src[j];
                dst[i] = m;
            }
        } else {
            for (int i = 0; i < RING_W; i++) dst[i] = src[toInt32(i * step)];
        }
        row = dst; n = RING_W;
    }
    if (ring_.empty()) ring_.assign((size_t)RING_W * RING_ROWS, 0);
    if (srcN != RING_W && rowHzPerBin > 0) rowHzPerBin = rowHzPerBin * srcN / RING_W;
    const size_t slot = (size_t)std::fmod(frameCount_, (double)RING_ROWS) * n;
    double off = 0;
    if (rowHzPerBin > 0 && rowCenterHz > 0 && viewC_ > 0 && viewBw_ > 0) {
        const double viewHzPerBin = viewBw_ / n;
        if (std::fabs(rowHzPerBin - viewHzPerBin) <= viewHzPerBin * 1e-6) {
            off = jround((rowCenterHz - viewC_) / rowHzPerBin);
            if (std::fabs(off) >= n) off = 0;
        }
    }
    uint8_t* dst = ring_.data() + slot;
    if (off == 0) {
        std::memcpy(dst, row, n);
    } else {
        std::memset(dst, 0, n);   // vacated edge = floor
        const int o = (int)off;
        if (o > 0) std::memcpy(dst + o, row, n - o);
        else       std::memcpy(dst, row - o, n + o);
    }
    // ★★ GLIDE (2026-10-28): a row placed somewhere else than the one before streaks its signals back to there, so a
    //    tune's trail is one continuous line — this row only, once (wf_glide.h; the JS path's utils/wfRowPlace.ts).
    vibewf::glideRow(dst, n, rowCenterHz, rowHzPerBin, off, trail_);
    frameCount_ += 1;
    if (covNow_ || !publish) { ringDirty_ = true; return; }
    publishRing();
}

// publishRing (944-962): the image is made HERE, on the engine thread — never on JS or the UI
void SpectrumEngine::publishRing() {
    void* img = makeRingImage();
    if (!img) return;
    std::lock_guard<std::mutex> lk(mx_);
    storeRingImageLocked(img);
    outVer_++;
}
void* SpectrumEngine::makeRingImage() {
    if (ring_.empty()) return nullptr;
    ringDirty_ = false;
    return makeImage_(ring_.data(), RING_W, RING_ROWS, RING_W);
}
void SpectrumEngine::storeRingImageLocked(void* img) {
    if (outImage_) releaseImage_(outImage_);   // superseded before the UI took it: never wrapped
    outImage_ = img;
    outHead_ = frameCount_;
    texReady_ = true;
}

// emitRepeatRows(1) (1003-1008): the last row again, aligned against the view as it is NOW
void SpectrumEngine::emitRepeatRow() {
    if (lastRow_.empty() || bgNow_) return;
    pushRow(lastRow_.data(), (int)lastRow_.size(), lastRowCenterHz_, lastRowHzPerBin_);
}

// Spectrum target (1493-1526) + peak path (1528-1552) + hidden (1553-1571)
void SpectrumEngine::buildSpectrum(const vibedsp::SpectrumFrameOut& fr, bool boost) {
    const EngineConfig& cfg = cfgNow_;
    if (!(cfg.specShow && cfg.specH > 4)) {
        std::lock_guard<std::mutex> lk(mx_);
        specHidden_ = true; specGen_++;
        if (outPeak_) { releasePath_(outPeak_); outPeak_ = nullptr; }
        peakGen_++; outVer_++;
        return;
    }
    const int fn = fr.n;
    const double width = cfg.width;
    const int wDraw = (int)std::floor(width);
    std::vector<float>& next = specScratch_;
    const int half = (int)std::ceil(width / 2);
    if (wDraw > 0 && fn > half) {
        next.resize(half);
        for (int k = 0; k < half; k++) {
            int idx = (int)std::floor(((k * 2) / width) * fn);
            if (idx >= fn) idx = fn - 1;
            next[k] = fr.spec[idx];
        }
    } else {
        next.assign(fr.spec, fr.spec + fn);
    }
    void* peak = nullptr;
    if (cfg.peakHold && !boost) {
        const int w = wDraw;
        const int count = (int)std::ceil(w / 2.0);
        peakXY_.resize((size_t)count * 2);
        for (int k = 0; k < count; k++) {
            const double px = k * 2;
            int idx = (int)std::floor((px / width) * fr.n);
            if (idx >= fr.n) idx = fr.n - 1;
            peakXY_[2 * k] = (float)px;
            peakXY_[2 * k + 1] = (float)(cfg.wfTop - fr.peak[idx] * cfg.specH);
        }
        peak = count > 0 ? makePath_(peakXY_.data(), count, false) : nullptr;
    } else if (boost) {
        proc_.resetPeakHold();
    }
    std::lock_guard<std::mutex> lk(mx_);
    outSpec_ = next;
    specGen_++; specHidden_ = false;
    if (outPeak_) releasePath_(outPeak_);
    outPeak_ = peak;
    peakGen_++;
    outVer_++;
}

}  // namespace vibespec
