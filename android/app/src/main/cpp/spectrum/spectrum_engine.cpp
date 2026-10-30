// VibeSDR — SpectrumEngine. See spectrum_engine.h; the behaviour it reproduces is specified, with WaterfallView.tsx
// line numbers, in docs/NATIVE-SPECTRUM-ENGINE.md ("WaterfallView per-frame behaviour"). Names follow the TS.
#include "spectrum_engine.h"
#include "wf_row_target.h"
#include "wf_ring_grow.h"   // ★★ the growing ring
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
    traceDrop();   // ★ V12 peak paths still waiting for their rows
}

double SpectrumEngine::now() const { return clock_ ? clock_() : wallMs(); }

// ── From JS ─────────────────────────────────────────────────────────────────────────────────────────────────────

void SpectrumEngine::pushFrameNative(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz, double trueBwHz) {
    if (!bins || n <= 0) return;
    {
        std::lock_guard<std::mutex> lk(mx_);
        nativeFedUntil_ = now() + NATIVE_FED_MS;
    }
    pushFrameImpl(bins, n, centerHz, bwHz, trueCenterHz, trueBwHz);
}

void SpectrumEngine::pushFrame(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz, double trueBwHz) {
    if (!bins || n <= 0) return;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (now() < nativeFedUntil_) return;   // the native socket already gave us this frame, on time
    }
    pushFrameImpl(bins, n, centerHz, bwHz, trueCenterHz, trueBwHz);
}

void SpectrumEngine::pushFrameImpl(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz, double trueBwHz) {
    std::lock_guard<std::mutex> lk(mx_);
    Frame f;
    if (!framePool_.empty()) { f = std::move(framePool_.back()); framePool_.pop_back(); }
    f.bins.assign(bins, bins + n);
    f.centerHz = centerHz; f.bwHz = bwHz; f.trueCenterHz = trueCenterHz; f.trueBwHz = trueBwHz;
    f.arrivedMs = now();
    inbox_.push_back(std::move(f));
    // A runaway producer (the engine thread starved) must not grow memory: keep the newest few.
    while (inbox_.size() > 8) { framePool_.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
    cv_.notify_one();
}

void SpectrumEngine::pushWideNative(const uint8_t* p, size_t len) {
    if (!p || len < vibewf::WIDE_HDR) return;
    pushWideImpl(p, len);
}
void SpectrumEngine::pushWide(const uint8_t* p, size_t len) {
    if (!p || len < vibewf::WIDE_HDR) return;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (now() < nativeFedUntil_) return;   // the native socket gave us this row already
    }
    pushWideImpl(p, len);
}
void SpectrumEngine::pushWideImpl(const uint8_t* p, size_t len) {
    std::lock_guard<std::mutex> lk(mx_);
    WideIn w; w.bytes.assign(p, p + len); w.at = now();
    wideInbox_.push_back(std::move(w));
    while (wideInbox_.size() > 4) wideInbox_.pop_front();   // only ever the newest picture matters
    // ★ no wake: it is applied with the next frame, timed by its arrival (no extra engine work of its own)
}

void SpectrumEngine::setView(double c, double bw) { std::lock_guard<std::mutex> lk(mx_); viewCenterHz_ = c; viewBwHz_ = bw; }
void SpectrumEngine::setConfig(const EngineConfig& c) { std::lock_guard<std::mutex> lk(mx_); cfg_ = c; }
void SpectrumEngine::setSettings(const vibedsp::SpectrumProcessorSettings& s) {
    std::lock_guard<std::mutex> lk(mx_); pendingSettings_ = s; settingsDirty_ = true; cv_.notify_one();
}
void SpectrumEngine::setLastInteractAt(double ms) { std::lock_guard<std::mutex> lk(mx_); lastInteractAt_ = ms; }
void SpectrumEngine::setAudioDelay(double ms) { std::lock_guard<std::mutex> lk(mx_); audioMs_ = ms; audioNew_ = true; }
void SpectrumEngine::setBackground(bool bg) { std::lock_guard<std::mutex> lk(mx_); bg_ = bg; cv_.notify_one(); }
void SpectrumEngine::setCovered(bool c) { std::lock_guard<std::mutex> lk(mx_); covered_ = c; cv_.notify_one(); }

bool SpectrumEngine::liveRange(int& dbMin, int& dbMax) {
    std::lock_guard<std::mutex> lk(mx_);
    dbMin = liveMin_; dbMax = liveMax_;
    const bool c = liveChanged_; liveChanged_ = false;
    return c;
}

// ── From the UI thread ──────────────────────────────────────────────────────────────────────────────────────────

bool SpectrumEngine::rowGeom(double frame, double& lo, double& span) {
    std::lock_guard<std::mutex> lk(mx_);
    if (rowGeo_.empty() || !(frame >= 0)) return false;
    const RowGeo& g = rowGeo_[(size_t)std::fmod(frame, (double)rowGeo_.size())];
    if (g.frame != frame || !(g.span > 0)) return false;
    lo = g.lo; span = g.span;
    return true;
}

bool SpectrumEngine::pollUi(UiUpdate& out) {
    std::lock_guard<std::mutex> lk(mx_);
    if (outVer_ == uiSeenVer_) return false;
    uiSeenVer_ = outVer_;
    if (outImage_) {                                        // the motion range + screen move ride with the image
        out.moLo = outMoLo_; out.moHi = outMoHi_; out.fanK = outK_; out.fanC = outC_;
        out.ringRows = outRingRows_;
        outK_ = 1; outC_ = 0;
    }
    out.image = outImage_; outImage_ = nullptr;            // ownership moves to the UI
    out.head = outHead_; out.texReady = texReady_;
    out.quant = outQuant_; out.avgFrameMs = outAvgMs_; out.rowRate = outRowRate_;
    out.scrollGen = scrollGen_; out.glide = glide_; out.glideDur = glideDur_;
    out.specGen = specGen_; out.specHidden = specHidden_;
    if (specGen_ != uiSpec_) { out.specTarget = outSpec_; uiSpec_ = specGen_; }
    out.peakGen = peakGen_; out.peakPath = outPeak_; outPeak_ = nullptr;
    out.stampGen = stampGen_;
    if (stampGen_ != uiStamp_) { out.stamps = outStamps_; out.stampsNew = true; uiStamp_ = stampGen_; }
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
        if (!traceQ_.empty() && (next < 0 || traceQ_.front().at < next)) next = traceQ_.front().at;   // ★ V12 the trace's wait
        // ★ V12 the fixed output clock's next tick (engine-thread state, read here under mx_ as the deadlines above are)
        if (rowClock_.rate > 0 && !vibewf::clockIdle(rowClock_, t)) { const double c = vibewf::clockNextAt(rowClock_); if (c >= 0 && (next < 0 || c < next)) next = c; }
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
    std::deque<WideIn> wides;
    bool applySettings = false;
    vibedsp::SpectrumProcessorSettings ps;
    bool wasCov = covNow_, wasBg = bgNow_;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (audioNew_) { vibewf::avNoteAudio(av_, audioMs_, now); audioNew_ = false; }   // ★ V12 (wf_av_sync.h)
        cfgNow_ = cfg_; viewC_ = viewCenterHz_; viewBw_ = viewBwHz_; interactAt_ = lastInteractAt_;
        bgNow_ = bg_; covNow_ = covered_;
        arrived.swap(inbox_);
        wides.swap(wideInbox_);
        if (settingsDirty_) { ps = pendingSettings_; settingsDirty_ = false; applySettings = true; }
    }
    if (applySettings) proc_.applySettings(ps);
    // ★ Background / cover STOP the repeat rows (the JS timer kept pushing up to 8 after either — and after unmount).
    if ((bgNow_ && !wasBg) || (covNow_ && !wasCov)) { extrasLeft_ = 0; extraDueAt_ = -1; clockOff(); traceDrop(); }
    if (bgNow_ && !wasBg) {                                     // drainFrame's background branch, at once
        for (auto& f : jbQueue_) framePool_.push_back(std::move(f));
        jbQueue_.clear(); jbPrefill_ = true; jbDueAt_ = -1;
    }
    // ★★★ THE RING GROWS (wf_ring_grow.h — JS asked for a deeper one: the view became taller than the ring holds). Here,
    //     between rows, on this thread; the next image carries the new depth.
    if (cfgNow_.ringRows > ringRows_) growRingTo(std::min(cfgNow_.ringRows, (int)RING_MAX_ROWS));
    if (!covNow_ && wasCov && ringDirty_) publishRing();        // uncover: ONE texture from the ring as it stands

    // ★★ A WALL CLOCK PUT BACK strands every deadline in the future: the next drain would wait the whole
    //    step (minutes) with frames queued behind it — a frozen waterfall. No deadline is ever legitimately
    //    more than a second ahead (drain caps dur at 1000 ms), so one that is was set by the old clock.
    //  ★ The clock stays wall time because lastInteractAt arrives from JS as Date.now(); moving both to a
    //    monotonic clock is the proper follow-up.
    if (jbDueAt_ > now + 1000) jbDueAt_ = now;
    if (extraDueAt_ > now + 1000) extraDueAt_ = now;
    for (auto& it : traceQ_) if (it.at > now + 1000) it.at = now;
    if (rowClock_.rate > 0 && vibewf::clockNextAt(rowClock_) > now + 1000) { rowClock_.t0 = now; rowClock_.k = 0; }
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (nativeFedUntil_ > now + NATIVE_FED_MS) nativeFedUntil_ = 0;
    }
    for (auto& w : wides) applyWide(w.bytes, w.at);   // ★ V12 before the frames: a row drawn now uses the newest picture
    for (auto& f : arrived) enqueue(std::move(f), f.arrivedMs);
    if (jbDueAt_ >= 0 && now >= jbDueAt_) { jbDueAt_ = -1; drain(now); }
    if (extraDueAt_ >= 0 && now >= extraDueAt_) {
        emitRepeatRow();
        extrasLeft_--;
        extraDueAt_ = extrasLeft_ > 0 ? now + extraSpacing_ : -1;
    }
    if (rowClock_.rate > 0 && !bgNow_ && !covNow_) { const double c = vibewf::clockNextAt(rowClock_); if (c >= 0 && now >= c) runClock(now); }
    if (!traceQ_.empty() && now >= traceQ_.front().at) runTrace(now);
    std::lock_guard<std::mutex> lk(mx_);
    while (framePool_.size() > 6) framePool_.pop_back();
}

// ★★ V12 a full-capture row arrived (at `at`): the newest picture of the capture, and the layer ring at the first one.
void SpectrumEngine::applyWide(const std::vector<uint8_t>& bytes, double at) {
    if (!vibewf::parseWide(bytes.data(), bytes.size(), wideRow_)) return;
    if (!wide_) wide_ = std::make_unique<vibewf::WideState>();
    vibewf::wideArrive(*wide_, wideRow_, at, placedLo_, placedSpan_, detP50_);
    const int F = (int)wideRow_.floor.size(), C = wideRow_.cells;
    // ★ allocated at the FIRST row only (~0.8 MB), and again only if the server's grid ever changed
    if (!fan_.layer || fan_.layer->F != F || fan_.layer->C != C) fan_.layer = std::make_unique<vibewf::LayerRing>(ringRows_, F, C);
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
    // ★★★ V12 2026-10-29 (v12-wf-no-jb, WaterfallView enqueueFrame's twin): ON THE FIXED CLOCK, straight through on arrival —
    //     `now` is the frame's real arrival (arrivedMs), and the clock's own presentation delay is the only de-jitter.
    //     Stuart: "if the buffer isnt needed for the waterfall improve the latency there too please". The clock running
    //     means the last frame was clocked; the first frame, a covered view or a feed too slow to fill comes through the
    //     buffer once and starts it. NATIVE (targetRows 0) keeps the buffer. Anything banked from before goes.
    if (rowClock_.rate > 0 && cfgNow_.targetRows > 0) {
        jbDueAt_ = -1;
        { std::lock_guard<std::mutex> lk(mx_); for (auto& q : jbQueue_) framePool_.push_back(std::move(q)); }
        jbQueue_.clear();
        jbPrefill_ = true;
        if (jbLastArrival_ > 0) jbArrivalMs_ = jbArrivalMs_ * 0.8 + (now - jbLastArrival_) * 0.2;
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
    if (wide_) detP50_ = vibewf::medianOf(f.bins.data(), n, p50Scratch_);   // ★ V12 the layer's floor match

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
    // ★★ …divided into the FRAME RATE cap below 60 (Stuart 2026-10-28: "in 30fps lock mode no point running a 60fps waterfall").
    // ★★★ V12 2026-10-29: every mode but NATIVE runs on the FIXED OUTPUT CLOCK (wf_row_clock.h, runClock) at its latched
    //     rate; NATIVE (targetRows 0) keeps its rule exactly as it was ("pure unmodified lines direct from the server").
    const bool clocked = cfg.targetRows > 0;
    const double baseRowTarget = clocked
        ? (avgFrameMs_ > 0 ? vibewf::clockRateUnderCap(vibewf::clockRowRate(rowTargetSt_, cfg.targetRows, 1000.0 / avgFrameMs_), cfg.displayCapHz) : 0)
        : vibewf::rowTargetUnderCap(avgFrameMs_ > 0 ? vibewf::stepRowTarget(rowTargetSt_, cfg.targetRows, 1000.0 / avgFrameMs_)
                                                    : rowTargetSt_.target,
                                    avgFrameMs_ > 0 ? 1000.0 / avgFrameMs_ : 0, cfg.displayCapHz);
    // ★★★ THE INTERACTION BOOST (wf_row_clock.h boostRowRate — WaterfallView's twin; Stuart 2026-10-29: "increase during
    //     interaction … even if it means a 12x frame generation" / "if the app is set to 120 we do 120"): while a hand is on
    //     the dial or an ease is still drawing, the clock's rate steps up to the display's maximum as set, ≤ 120, ≤ 12 × the
    //     feed. Frames already here, nothing asked of the server; NATIVE (not clocked) untouched.
    //     (Also while the fan eases a move this hand did not make — a shared dial, a recentre: the boost is local.)
    //     The tail runs from the last input or the START of the last ease; the boost then holds until the ease lands.
    const bool easing = fan_.easeLeft > 0, easeStarted = fan_.easeLeft > lastEaseLeft_;
    lastEaseLeft_ = fan_.easeLeft;
    // (the input's OWN time — noting `now` while the input is recent would add a second tail)
    if (interactAt_ > boost_.lastMove && now - interactAt_ < vibewf::BOOST_TAIL_MS) vibewf::boostNote(boost_, interactAt_);
    if (easeStarted) vibewf::boostNote(boost_, now);
    const bool boostOn = clocked && baseRowTarget > 0 && vibewf::boostActive(boost_, now, easing);
    const double dispHz = cfg.displayCapHz > 0 ? (cfg.panelHz > 0 ? std::min(cfg.displayCapHz, cfg.panelHz) : cfg.displayCapHz)
                                               : (cfg.panelHz > 0 ? cfg.panelHz : 60);
    const double rowTarget = boostOn ? vibewf::boostRowRate(baseRowTarget, avgFrameMs_ > 0 ? 1000.0 / avgFrameMs_ : 0, dispHz)
                                     : baseRowTarget;
    const bool repeatsCarry = rowTarget > 0;
    // ★★ The zoom/tune ease is by TIME (wf_zoom_fan.h zoomEaseRows): the rows/s the waterfall runs at — the clock's rate, or
    //    one row a frame in Native.
    fan_.rowRate = rowTarget > 0 ? rowTarget : (avgFrameMs_ > 0 ? 1000.0 / avgFrameMs_ : 20.0);
    const bool wfBoost = boost || (cfg.smoothTune && lowFps && !repeatsCarry);

    if (lastWfPushAt_ > 0) {
        const double wfDt = now - lastWfPushAt_;
        // ★ Not while interacting (2026-10-28, WaterfallView's twin): a re-subscribe's pause folded in here lengthened
        //   `dur`, so every arrival after it earned extra repeat rows and the scroll sped up for a beat after a tune.
        if (wfDt > 0 && wfDt < 2000 && !boost) avgWfGapMs_ = avgWfGapMs_ > 0 ? avgWfGapMs_ * 0.8 + wfDt * 0.2 : wfDt;
    }
    lastWfPushAt_ = now;
    const double rowCentre = std::isnan(f.trueCenterHz) ? f.centerHz : f.trueCenterHz;
    // ★★ The row's OWN span (WaterfallView's twin, 2026-10-29): bwHz is the predicted view while a zoom is in flight, the
    //   bins are at the server's rung — placed by the prediction, the fan eased rows by ratios the data never had.
    const double rowBw = (!std::isnan(f.trueBwHz) && f.trueBwHz > 0) ? f.trueBwHz : f.bwHz;
    const double rowHzBin = (rowBw > 0 && n > 0) ? rowBw / n : 0;
    // ★★ The row is published TOGETHER with the scroll state below (one lock, one version): the UI must never see
    //   the new head without the glide restart that goes with it. Apart, a poll between the two showed the head
    //   one row on with the old glide still at 1 — the whole waterfall a row forward for one frame, then back —
    //   at every real frame: the judder on an 8 fps feed (XCover 4s, 2026-10-12). The JS path does both in one
    //   synchronous call, so it never could.
    const double revealMs = avgWfGapMs_ > 0 ? avgWfGapMs_ : avgFrameMs_;
    const double dur = std::max(15.0, std::min(1000.0, revealMs));   // ★ 15, not 50 (2026-10-29): a > 20 fps feed over-ran the target
    // ★★★ FILLED IN OR THINNED (2026-10-29, WaterfallView's twin): 0 = this frame is DISCARDED from the waterfall (a feed
    //     faster than the rate, thinned evenly); the trace below still gets it. At most 9 (the stall guard).
    const bool onClock = clocked && rowTarget > 0 && !covNow_;
    const int rowsNow = onClock ? 0 : vibewf::rowsForArrival(rowCarry_, dur, rowTarget);
    if (rowsNow > 0) pushRow(fr.row, n, rowCentre, rowHzBin, false);
    lastRow_.assign(fr.row, fr.row + n);
    lastRowCenterHz_ = rowCentre; lastRowHzPerBin_ = rowHzBin;
    { std::lock_guard<std::mutex> lk(mx_); outRowRate_ = onClock ? rowTarget : 0; }

    if (onClock) {
        // ★★★ THE FIXED CLOCK (WaterfallView's twin): the frame goes to the clock — presented on the feed's de-jittered
        //     schedule, at once during a gesture — and the clock makes the rows (runClock, on its ticks).
        extrasLeft_ = 0; extraDueAt_ = -1;
        const long id = ++clockSeq_;
        ClockRow* slot = nullptr;
        for (auto& r : clockRows_) if (r.id < 0) { slot = &r; break; }
        if (!slot && (int)clockRows_.size() >= vibewf::COMBINE_MAX_FRAMES + 2) {
            // ★ The store is full of frames not yet shown (a feed far faster than the row rate): the oldest that is not
            //   held or queued goes — it is dropped, as every surplus frame was before 2026-10-30.
            for (auto& r : clockRows_) {
                if (r.id == rowClock_.cur) continue;
                bool queued = false;
                for (auto& q : rowClock_.q) if (q.id == r.id) { queued = true; break; }
                if (!queued && (!slot || r.id < slot->id)) slot = &r;
            }
        }
        if (!slot) { clockRows_.emplace_back(); slot = &clockRows_.back(); }
        slot->id = id; slot->row.assign(fr.row, fr.row + n); slot->c = rowCentre; slot->h = rowHzBin;
        slot->k = vibewf::dbPerUnit(fr.dbMin, fr.dbMax);
        vibewf::clockSetRate(rowClock_, rowTarget, now);
        // ★★★ THE WATERFALL WAITS FOR THE SOUND (wf_av_sync.h — WaterfallView's twin, Stuart 2026-10-30): the frame enters
        //     the clock `hold` ms after it arrived (the audio's playout delay less this route's own), so row and trace come
        //     on screen as the sound is heard. "…simply duplicate the previous lines if needed and then the waterfall still
        //     reacts to the audio in time but still stays smooth": a growing hold is the clock's repeats, a shrinking one
        //     its discards. ZERO for 1 s after any tune or zoom input (smoothTune or not) — a moving dial is never held.
        // ★ …and while the picture is still moving — the ease and the boost run until a zoom or tune has landed (Stuart,
        //   2026-10-30: "noticable delay when operating the zoom controls"). WaterfallView's avPass, the same rule.
        const bool avPass = now - interactAt_ < 1000 || boostOn || easing;
        const double holdWas = av_.hold;
        const double hold = vibewf::avHold(av_, vibewf::clockOwnMs(rowClock_.ivl, rowTarget), avPass);
        const double pAt = vibewf::clockArrive(rowClock_, id, now, boost, hold);
        runClock(now);
        {
            std::lock_guard<std::mutex> lk(mx_);
            outQuant_ = wfBoost ? 0 : 1;
            outAvgMs_ = avgFrameMs_;
            outVer_++;
        }
        // ★★ The trace goes with its row: at its presentation + the rows the UI holds in hand (traceLagMs); at once in a
        //   gesture.
        buildSpectrum(fr, boost, boost ? 0 : pAt + vibewf::traceLagMs(rowTarget), now, hold < holdWas);
        return;
    }
    if (rowClock_.rate > 0) clockOff();   // off the clock (NATIVE, a feed too slow to fill): the next clocked frame starts afresh

    if (covNow_) return;
    if (rowsNow == 0) { buildSpectrum(fr, boost); return; }   // nothing new in the ring: no image, no scroll step
    const int extras = rowsNow - 1;
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
    if (ring_.empty()) ring_.assign((size_t)RING_W * (ringRows_ + vibewf::motionImageRows(ringRows_, RING_W)), 0);
    if (srcN != RING_W && rowHzPerBin > 0) rowHzPerBin = rowHzPerBin * srcN / RING_W;
    // Placed against the view (rowViewOffset/writeRow), then:
    // ★★ MOTION (2026-10-28): the row's placement and its move from the row before are noted beside the ring (two image
    //    rows), and the SHADER draws the move between rows (motion-compensated) — nothing stored is altered (wf_motion.h;
    //    the JS path's utils/wfRowPlace.ts). It replaced the glide streak. A jump is re-mapped like a zoom (wf_zoom_fan.h).
    // ★★★ THE ZOOM FAN (2026-10-28): a row at a different SPAN re-maps every stored row to the new view, so a zoom fans
    //    the history in and out instead of leaving slices of the old scale. The stored ring is never rewritten; the
    //    image is made from the displayed ring (wf_zoom_fan.h fanImage), regenerated through per-row maps on a zoom.
    double start = NAN;
    const int slot = vibewf::placeRow(ring_.data(), ringRows_, n, frameCount_, row, rowCenterHz, rowHzPerBin, viewC_, viewBw_,
                                      motion_, fan_, -1, &start);
    placedLo_ = start; placedSpan_ = rowHzPerBin * n;
    // ★★ SCALE STAMPS (wf_stamps.h; Stuart, 2026-10-30, #57): the row as DISPLAYED (through its fan map — eased rows too)
    //    to the detector; a jump moved every row, so the stamps move with it. The UI hears only when the list changes.
    {
        const int viewRows = cfgNow_.stampViewRows > 0 ? cfgNow_.stampViewRows : 600;
        if (fan_.jumpHz != 0) vibewf::stampJump(stamps_, fan_.jumpHz);
        stamps_.minGapRows = cfgNow_.stampMinGapRows > 0 ? cfgNow_.stampMinGapRows : 45;
        stamps_.sweepRows = std::max((int)std::ceil((fan_.rowRate > 0 ? fan_.rowRate : 30) * 2.5), (int)std::ceil(viewRows * 0.25));
        double P = 0, Q = 0;
        const bool known = vibewf::displayMap(motion_, fan_.xfA.data(), fan_.xfB.data(), slot, n, P, Q);
        if (known) vibewf::stampNoteRow(stamps_, frameCount_, P, Q * n, vibewf::stampSettleRows(fan_.rowRate));
        {   // ★★ the row's own scale for a tap on it (rowGeom) — fixed once written now that a jump keeps the history as drawn
            std::lock_guard<std::mutex> lk(mx_);
            if ((int)rowGeo_.size() != ringRows_) rowGeo_.resize(ringRows_);
            rowGeo_[slot] = known ? RowGeo{ frameCount_, P, Q * n } : RowGeo{};
        }
        if (!stamps_.stamps.empty()) vibewf::stampPrune(stamps_, frameCount_, std::min(ringRows_ - 8, viewRows + 8));
        if (stamps_.gen != stampSeen_) {
            stampSeen_ = stamps_.gen;
            std::lock_guard<std::mutex> lk(mx_);
            outStamps_ = stamps_.stamps; stampGen_++; outVer_++;
        }
    }
    // ★★ V12 THE FULL-CAPTURE LAYER: this row's layer row at the same slot, in capture space (wf_wide.h) — only once a
    //    server has sent one. Through the detailed rows' own dB → index mapping (proc_'s range and brightness).
    if (fan_.layer) {
        const double w = cfgNow_.width;
        fan_.layer->fadeTx = w > 0 ? std::max(2.0, std::min(24.0, 6.0 * RING_W / w)) : 6.0;   // ~6 pt soft edge
        const vibewf::ToIdx toIdx(proc_.dbMin(), proc_.dbMax(), proc_.settings().wfBrightness, wide_ ? wide_->off : 0);
        vibewf::layerWriteRow(*fan_.layer, slot, wide_.get(), placedLo_, placedSpan_, now(), toIdx);
        vibewf::fanRow(fan_, ring_.data(), n, slot, ringRows_);   // ★ a zoom's eased row: again, now its layer row exists
    }
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
    vibewf::takeFanStep(fan_, imgK_, imgC_);
    imgMoLo_ = motion_.lo; imgMoHi_ = motion_.hi;
    imgRows_ = ringRows_;
    return makeImage_(vibewf::wfImageBytes(fan_, motion_, ring_.data(), RING_W, ringRows_), RING_W,
                      ringRows_ + vibewf::motionImageRows(ringRows_, RING_W), RING_W);
}
// ★★★ GROW THE RING (wf_ring_grow.h; the JS path's twin is WaterfallView's growth effect + utils/wfRingGrow.ts): every row
//     and its per-row state copied into the deeper ring, the tap lookup (rowGeo_) too; then one image at the new depth.
void SpectrumEngine::growRingTo(int rows) {
    if (!(rows > ringRows_)) return;
    const int old = ringRows_;
    vibewf::growRing(ring_, motion_, fan_, RING_W, old, rows, frameCount_);
    ringRows_ = rows;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (!rowGeo_.empty()) {
            std::vector<RowGeo> g2((size_t)rows);
            for (double f = std::max(0.0, frameCount_ - old); f < frameCount_; f++)
                g2[(size_t)std::fmod(f, (double)rows)] = rowGeo_[(size_t)std::fmod(f, (double)old)];
            rowGeo_.swap(g2);
        }
    }
    if (ring_.empty()) return;
    if (covNow_ || bgNow_) { ringDirty_ = true; return; }
    publishRing();
}
void SpectrumEngine::storeRingImageLocked(void* img) {
    if (outImage_) releaseImage_(outImage_);   // superseded before the UI took it: never wrapped
    outImage_ = img;
    outHead_ = frameCount_;
    outC_ += outK_ * imgC_; outK_ *= imgK_;           // composed until the UI takes an image (a superseded one's move kept)
    imgK_ = 1; imgC_ = 0;
    outMoLo_ = imgMoLo_; outMoHi_ = imgMoHi_;
    outRingRows_ = imgRows_;
    texReady_ = true;
}

// emitRepeatRows(1) (1003-1008): the last row again, aligned against the view as it is NOW
void SpectrumEngine::emitRepeatRow() {
    if (lastRow_.empty() || bgNow_) return;
    pushRow(lastRow_.data(), (int)lastRow_.size(), lastRowCenterHz_, lastRowHzPerBin_);
}

// ★★★ V12 THE FIXED OUTPUT CLOCK's ticks (wf_row_clock.h): every row due by `now`, each the newest real frame presented by
//     its tick, into the ring — then ONE image for them all (a late wake-up makes up its ticks, at most 6).
void SpectrumEngine::runClock(double now) {
    clockOut_.clear();
    vibewf::clockRun(rowClock_, now, clockOut_);
    bool any = false;
    auto find = [&](long id) -> ClockRow* { for (auto& r : clockRows_) if (r.id == id) return &r; return nullptr; };
    for (long id : clockOut_) {
        ClockRow* t = find(id);
        // ★★ A frame shown for the first time takes in the frames the clock passed over since the last one shown —
        //    combined in linear power with the peaks kept (wf_row_combine.h; WaterfallView combineInto), not dropped.
        if (t && id > clockShown_ + 1) {
            vibewf::combineReset(clockComb_);
            for (long j = std::max(clockShown_ + 1, id - vibewf::COMBINE_MAX_FRAMES + 1); j <= id; j++)
                if (ClockRow* f = find(j)) vibewf::combineAdd(clockComb_, f->row.data(), f->row.size(), f->c, f->h, f->k);
            vibewf::combineTake(clockComb_, t->row.data(), t->row.size());
        }
        if (id > clockShown_) clockShown_ = id;
        if (t) { pushRow(t->row.data(), (int)t->row.size(), t->c, t->h, false); any = true; }
    }
    // Free the frames the clock can no longer show or merge: shown already, but the one it holds.
    for (auto& r : clockRows_) if (r.id >= 0 && r.id <= clockShown_ && r.id != rowClock_.cur) r.id = -1;
    if (!any || covNow_ || bgNow_) return;
    void* img = makeRingImage();
    std::lock_guard<std::mutex> lk(mx_);
    if (img) storeRingImageLocked(img);
    outVer_++;
}
void SpectrumEngine::clockOff() {
    vibewf::clockReset(rowClock_);
    vibewf::clockSetRate(rowClock_, 0, 0);
    for (auto& r : clockRows_) r.id = -1;
}

// Spectrum target (1493-1526) + peak path (1528-1552) + hidden (1553-1571)
void SpectrumEngine::buildSpectrum(const vibedsp::SpectrumFrameOut& fr, bool boost, double at, double now, bool shrunk) {
    const EngineConfig& cfg = cfgNow_;
    if (!(cfg.specShow && cfg.specH > 4)) {
        traceDrop();
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
    if (at > now) {   // ★ V12 waits for its row (never out of order; as many waiting as the clock's queue)
        // ★★ V12 2026-10-30 the A/V hold shrank: traces still waiting later than this one went with their rows.
        if (shrunk) while (!traceQ_.empty() && traceQ_.back().at > at) { if (traceQ_.back().peak) releasePath_(traceQ_.back().peak); traceQ_.pop_back(); }
        if (!traceQ_.empty() && at < traceQ_.back().at) at = traceQ_.back().at;
        traceQ_.push_back(TraceItem{at, next, peak});
        const size_t cap = std::max<size_t>(4, rowClock_.q.size() + 1);   // ★ + the frames an A/V hold keeps waiting
        while (traceQ_.size() > cap) { if (traceQ_.front().peak) releasePath_(traceQ_.front().peak); traceQ_.pop_front(); }
        return;
    }
    traceDrop();   // now: anything still waiting is older
    publishTrace(next, peak);
}
void SpectrumEngine::publishTrace(const std::vector<float>& spec, void* peak) {
    std::lock_guard<std::mutex> lk(mx_);
    outSpec_ = spec;
    specGen_++; specHidden_ = false;
    if (outPeak_) releasePath_(outPeak_);
    outPeak_ = peak;
    peakGen_++;
    outVer_++;
}
// ★★★ V12 the trace whose row is now on screen: every one due, the newest published (older ones' peak paths never shown).
void SpectrumEngine::runTrace(double now) {
    if (bgNow_ || covNow_) { traceDrop(); return; }
    bool any = false; TraceItem due;
    while (!traceQ_.empty() && traceQ_.front().at <= now) {
        if (any && due.peak) releasePath_(due.peak);
        due = std::move(traceQ_.front()); traceQ_.pop_front(); any = true;
    }
    if (any) publishTrace(due.spec, due.peak);
}
void SpectrumEngine::traceDrop() {
    for (auto& it : traceQ_) if (it.peak) releasePath_(it.peak);
    traceQ_.clear();
}

}  // namespace vibespec
