// wf_model.cpp — WaterfallView.tsx's JS-path glue, ported: handleFrame (the processor, the feed-following rate, the
// interaction boost + taper, the fixed clock), pumpClock (clock ticks → combined rows → pushRow), pushRow (resample to
// RING_W, placeRow), the UI-thread pacer (jsPending → paceTick) and the spectrum trace (traceShow / specTweenTick).
// Names follow the TSX so the two can be read side by side.
#include "wf_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wf {

static constexpr double SMOOTH_TUNE_TAIL_MS = 1000;
static constexpr double LOW_FPS_GLIDE_MS = 66;
static constexpr double SPEC_TWEEN_MS = 33;
static constexpr int SPEC_PX_STEP = 2;

// WaterfallView WF_TARGET_ROWS: the cap the engine knows as targetRows
static int targetRowsOf(Scroll s) {
  switch (s) {
    case Scroll::Smooth: return 60; case Scroll::Default: return 20; case Scroll::Eco: return 10;
    case Scroll::Low: return 5; case Scroll::Min2: return 2; default: return 0;
  }
}

WaterfallModel::WaterfallModel(const WaterfallConfig& cfg)
    : ring_((size_t)RING_ROWS * RING_W, 0), rowBuf_(RING_W, 0), ms_(newMotionState(RING_ROWS, RING_W)), fan_(newFanState(RING_ROWS)) {
  setConfig(cfg);
}

void WaterfallModel::setConfig(const WaterfallConfig& cfg) {
  cfg_ = cfg;
  // WaterfallView's settings effect: the processor's wfSharpness is min(12, sharpBase × sharpMul × 2)
  const double sharpBase = cfg.scroll == Scroll::Smooth ? 3 : cfg.scroll == Scroll::Default ? 2 : cfg.scroll == Scroll::Eco ? 1.75 : 1.5;
  SignalSettings s = cfg.sig;
  s.peakHold = cfg.peakHold;
  s.wfContrast = cfg.wfContrast;
  s.wfSharpness = std::min(12.0, sharpBase * (cfg.wfSharpness / 5) * 2);
  proc_.applySettings(s);
}

WfUniforms WaterfallModel::uniforms() const {
  WfUniforms u;
  u.headF = pace_.uHead; u.frac = pace_.scrollFrac; u.quant = pace_.uQuantDraw;
  u.moLo = takenMoLo_; u.moHi = takenMoHi_;   // (as taken with the image — see tick)
  u.zA = pace_.uZA; u.zB = pace_.uZB;
  const double sharpBase = cfg_.scroll == Scroll::Smooth ? 3 : cfg_.scroll == Scroll::Default ? 2 : cfg_.scroll == Scroll::Eco ? 1.75 : 1.5;
  u.sharp = std::min(2.0, (cfg_.wfSharpness / 10) * 1.5 * (sharpBase / 1.5));
  u.contrast = std::max(-1.0, std::min(1.0, cfg_.wfContrast / 10));
  return u;
}

void WaterfallModel::takeDirty(std::vector<int>& rows, std::vector<int>& motionSlots) {
  rows.swap(dirtyRows_); dirtyRows_.clear();
  motionSlots.swap(ms_.dirty); ms_.dirty.clear();
}

void WaterfallModel::pushFrameU8(const uint8_t* bins, int n, bool fftOrder, double centreHz, double spanHz, double tMs) {
  if ((int)dbScratch_.size() != n) dbScratch_.assign(n, 0);
  const int h = fftOrder ? n / 2 : 0;
  for (int i = 0; i < n; i++) dbScratch_[i] = (float)bins[(i + h) % n] - 256.0f;   // u8 0 = −256 dBFS = no data
  pushFrameDb(dbScratch_.data(), n, centreHz, spanHz, tMs);
}

// ── handleFrame ──
void WaterfallModel::pushFrameDb(const float* db, int n, double centreHz, double spanHz, double now) {
  if (n <= 0) return;
  const double vC = viewSet_ ? viewC_ : centreHz, vBw = viewSet_ ? viewBw_ : spanHz;
  if (!viewSet_) { viewC_ = centreHz; viewBw_ = spanHz; }
  // 1. the processor (FFT smoothing bypassed while interacting)
  const bool interacting = cfg_.smoothTune && now - lastInteractAt_ < SMOOTH_TUNE_TAIL_MS;
  const ProcessedFrame frame = proc_.process(db, n, vC, vBw, interacting, now);
  if (onFrame) onFrame(onRowCtx, frame);
  // 2. frame interval + interaction
  const bool boost = interacting;
  if (lastFrameTs_ > 0 && !boost) avgFrameMs_ = avgFrameMs_ * 0.8 + (now - lastFrameTs_) * 0.2;
  lastFrameTs_ = now;
  pace_.interacting = boost;
  const bool lowFps = avgFrameMs_ > LOW_FPS_GLIDE_MS;
  const int targetRows = targetRowsOf(cfg_.scroll);
  if (rowTargetCap_ != targetRows) { rowTargetSt_ = RowTargetState(); rowTargetCap_ = targetRows; }
  const Scroll wfScroll = scrollForCap(targetRows);
  const double feedFps = 1000 / std::max(1.0, avgFrameMs_);
  const bool clocked = wfScroll != Scroll::Sharp;
  const double baseRowTarget = clocked
      ? (avgFrameMs_ > 0 ? clockRateUnderCap(clockRowRate(rowTargetSt_, wfScroll, feedFps), cfg_.frameCapHz) : 0)
      : rowTargetUnderCap(avgFrameMs_ > 0 ? stepRowTarget(rowTargetSt_, wfScroll, 1000 / avgFrameMs_) : rowTargetSt_.target,
                          feedFps, cfg_.frameCapHz);
  // the interaction boost
  const double ptNow = now;
  const int easeLeft = fan_.easeLeft; const bool easing = easeLeft > 0;
  const bool easeStarted = easeLeft > lastEaseLeft_; lastEaseLeft_ = easeLeft;
  const double inAt = lastInteractAt_;
  if (inAt > boost_.lastMove && ptNow - inAt < BOOST_TAIL_MS) boostNote(boost_, inAt);
  if (easeStarted) boostNote(boost_, ptNow);
  if (clocked && baseRowTarget > 0) boostActive(boost_, ptNow, easing);
  const double dispHz = cfg_.displayHz;
  const double boostRate = clocked && baseRowTarget > 0 ? boostRowRate(baseRowTarget, feedFps, dispHz, cfg_.boostOn) : baseRowTarget;
  const double rowTarget = clocked && baseRowTarget > 0 ? boostTaper(boost_, baseRowTarget, boostRate, ptNow, dispHz) : baseRowTarget;
  cri_.base = clocked ? baseRowTarget : 0; cri_.boosted = boostRate; cri_.hz = dispHz;
  const bool repeatsCarry = rowTarget > 0;
  const bool wfBoost = boost || (cfg_.smoothTune && lowFps && !repeatsCarry);
  double traceAt = 0;
  // 3. the waterfall
  if (lastWfPushAt_ > 0) {
    const double wfDt = now - lastWfPushAt_;
    if (wfDt > 0 && wfDt < 2000 && !boost) avgWfGapMs_ = avgWfGapMs_ > 0 ? avgWfGapMs_ * 0.8 + wfDt * 0.2 : wfDt;
  }
  lastWfPushAt_ = now;
  const double rowCentre = centreHz, rowHzBin = spanHz > 0 ? spanHz / frame.n : 0;   // rowGeometry: the bins' own
  const double revealMs = avgWfGapMs_ > 0 ? avgWfGapMs_ : avgFrameMs_;
  const double dur = std::max(15.0, std::min(1000.0, revealMs));
  const bool onClock = clocked && rowTarget > 0;
  const int rowsNow = onClock ? 0 : rowsForArrival(rowCarry_, dur, rowTarget);
  fan_.rowRate = rowTarget > 0 ? rowTarget : (avgFrameMs_ > 0 ? 1000 / avgFrameMs_ : 20);
  if (rowsNow > 0) pushRow(frame.row, frame.n, rowCentre, rowHzBin);
  if (onClock) {
    const long long id = ++clockSeq_;
    CombFrame& slot = clockFrames_[id];
    slot.row.assign(frame.row, frame.row + frame.n); slot.c = rowCentre; slot.h = rowHzBin; slot.k = dbPerUnit(frame.dbMin, frame.dbMax);
    clockSetRate(clock_, rowTarget, now);
    // (no A/V hold here: the handheld has no audio playout delay to wait for — wfAvSync's hold is 0)
    const double pAt = clockArrive(clock_, id, now, boost, 0);
    pumpClock(now);
    if (!boost) traceAt = pAt + traceLagMs(rowTarget);
  } else if (clock_.rate > 0) {
    clockReset(clock_); clockSetRate(clock_, 0, 0); clockFrames_.clear();
  }
  pace_.uQuantAsk = wfBoost ? 0 : 1;
  { const double tg = rowTarget > 0 ? rowTarget / 1000 : 0; pace_.pTgt = tg; }
  pace_.pClocked = onClock;
  // 4. the spectrum trace + peak (sampled at SPEC_PX_STEP)
  if (cfg_.specShow) {
    const float* fs = frame.spec; const int fn = frame.n;
    const double width = traceW_;
    std::vector<double> next;
    if (width > 0 && fn > (int)std::ceil(width / SPEC_PX_STEP)) {
      const int m = (int)std::ceil(width / SPEC_PX_STEP);
      next.resize(m);
      for (int k = 0; k < m; k++) next[k] = fs[(int)std::floor(((double)(k * SPEC_PX_STEP) / width) * fn)];
    } else next.assign(fs, fs + fn);
    std::vector<double> peak; bool hasPeak = false;
    if (cfg_.peakHold && !boost) {
      const int count = (int)std::ceil(std::floor(width) / SPEC_PX_STEP);
      peak.resize(count);
      for (int k = 0; k < count; k++) peak[k] = frame.peak[(int)std::floor(((double)(k * SPEC_PX_STEP) / width) * frame.n)];
      hasPeak = true;
    } else if (boost) proc_.resetPeakHold();
    // (traceShow's `at` is in the same clock as `now`)
    if (!(traceAt > now)) traceAt = 0;
    traceShow(std::move(next), std::move(peak), hasPeak, traceAt == 0 ? now : traceAt, false);
  }
}

// ── traceShow / traceRun / traceApply ──
void WaterfallModel::traceShow(std::vector<double>&& spec, std::vector<double>&& peak, bool hasPeak, double at, bool shrunk) {
  const double now = lastFrameTs_;
  auto apply = [&](TraceItem& it) {
    specTo_ = it.spec;
    if (specDisp_.size() != specTo_.size()) specDisp_ = specTo_;
    tweenActive_ = true;
    if (it.hasPeak) peakShown_ = it.peak; else peakShown_.clear();
  };
  TraceItem item{ at, std::move(spec), std::move(peak), hasPeak };
  if (!(at > now)) { traceQ_.clear(); apply(item); return; }
  if (shrunk) while (!traceQ_.empty() && traceQ_.back().at > at) traceQ_.pop_back();
  if (!traceQ_.empty() && at < traceQ_.back().at) item.at = traceQ_.back().at;
  traceQ_.push_back(std::move(item));
  const size_t cap = std::max<size_t>(4, clock_.q.size() + 1);
  while (traceQ_.size() > cap) traceQ_.pop_front();
}
void WaterfallModel::traceRun(double now) {
  TraceItem* due = nullptr; TraceItem keep;
  while (!traceQ_.empty() && traceQ_.front().at <= now + 1) { keep = std::move(traceQ_.front()); traceQ_.pop_front(); due = &keep; }
  if (due) {
    specTo_ = due->spec;
    if (specDisp_.size() != specTo_.size()) specDisp_ = specTo_;
    tweenActive_ = true;
    if (due->hasPeak) peakShown_ = due->peak; else peakShown_.clear();
  }
}

// ── pumpClock ──
void WaterfallModel::pumpClock(double now) {
  clockOut_.clear();
  if (cri_.base > 0 && clock_.rate > 0 && (boost_.on || boost_.peak > 0)) {
    boostActive(boost_, now, fan_.easeLeft > 0);
    const double r = boostTaper(boost_, cri_.base, cri_.boosted, now, cri_.hz);
    if (r > 0 && r != clock_.rate) {
      clockSetRate(clock_, r, now);
      pace_.pTgt = r / 1000;
      fan_.rowRate = r;
    }
  }
  clockRun(clock_, now, clockOut_);
  auto get = [this](long long id) -> CombFrame* { auto it = clockFrames_.find(id); return it == clockFrames_.end() ? nullptr : &it->second; };
  for (size_t i = 0; i < clockOut_.size(); i++) {
    if (clockOut_[i] > clockShown_) { combineInto(clockComb_, get, clockShown_, clockOut_[i]); clockShown_ = clockOut_[i]; }
    CombFrame* f = get(clockOut_[i]);
    if (f) pushRow(f->row.data(), (int)f->row.size(), f->c, f->h);
  }
  if (clockFrames_.size() > 6) {
    for (auto it = clockFrames_.begin(); it != clockFrames_.end();) {
      if (it->first <= clockShown_ && it->first != clock_.cur) it = clockFrames_.erase(it); else ++it;
    }
    if (clockFrames_.size() > (size_t)COMBINE_MAX_FRAMES + 2) {
      for (auto it = clockFrames_.begin(); it != clockFrames_.end() && clockFrames_.size() > (size_t)COMBINE_MAX_FRAMES + 2;) {
        bool held = it->first == clock_.cur;
        for (const ClockFrame& q : clock_.q) if (q.id == it->first) { held = true; break; }
        if (!held) it = clockFrames_.erase(it); else ++it;
      }
    }
  }
  // the timer: stops on a dead link (clockIdle), the next arrival restarts it
  lastPumpAt_ = now;
  clockArmed_ = !clockIdle(clock_, now) && clockNextAt(clock_) >= 0;
}

// ── pushRow ──
void WaterfallModel::pushRow(const uint8_t* srcRow, int srcN, double rowCenterHz, double rowHzPerBin) {
  const uint8_t* row = srcRow;
  if (srcN != RING_W) {
    uint8_t* dst = rowBuf_.data();
    const double step = (double)srcN / RING_W;
    if (srcN > RING_W) {   // peak-hold downsample
      for (int i = 0; i < RING_W; i++) {
        const int a = (int)(i * step);
        int b = std::min(srcN, (int)((i + 1) * step)); if (b == 0) b = a + 1;
        uint8_t m = 0;
        for (int k = a; k < b; k++) if (srcRow[k] > m) m = srcRow[k];
        dst[i] = m;
      }
    } else for (int i = 0; i < RING_W; i++) dst[i] = srcRow[(int)(i * step)];
    row = dst;
    if (rowHzPerBin > 0) rowHzPerBin = rowHzPerBin * srcN / RING_W;
  }
  const int slot = placeRow(ring_, RING_ROWS, RING_W, frameCount_, row, rowCenterHz, rowHzPerBin, viewC_, viewBw_, ms_, fan_, RING_ROWS);
  frameCount_ += 1;
  dirtyRows_.push_back(slot);
  // publishRing → jsPending: the image, its newest row, its motion range, the fans' screen move
  double k, c; takeFanStep(fan_, k, c);
  pend_.avail = (double)frameCount_; pend_.moLo = ms_.lo; pend_.moHi = ms_.hi; pend_.k = k; pend_.c = c;
  pendGen_++;
  if (onRow) onRow(onRowCtx, *this, slot);
}

// ── one display frame ──
void WaterfallModel::tick(double now, double gap) {
  // the clock's timer (pumpClock re-arms at clockNextAt until the clock is idle)
  for (int guard = 0; guard < 64 && clockArmed_; guard++) {
    const double nx = clockNextAt(clock_);
    if (nx < 0) { clockArmed_ = false; break; }
    if (nx > now) break;
    pumpClock(std::max(nx, lastPumpAt_));
  }
  if (!traceQ_.empty() && traceQ_.front().at <= now + 1) traceRun(now);
  // the JS path's frame callback: take the newest image, then the display driver
  if (pendGen_ != jsSeenGen_) {
    jsSeenGen_ = pendGen_;
    jsAvail_ = pend_.avail;
    takenMoLo_ = pend_.moLo; takenMoHi_ = pend_.moHi;
    easeStep(pace_, pend_.k, pend_.c, now);
  }
  if (jsAvail_ > 0) paceTick(pace_, jsAvail_, now, gap);
  // the spectrum tween (specTweenTick)
  if (tweenActive_) {
    specAcc_ += gap;
    if (specAcc_ >= SPEC_TWEEN_MS && !specTo_.empty() && specDisp_.size() == specTo_.size()) {
      const double dtAcc = specAcc_;
      specAcc_ = 0;
      const double k = 1 - std::exp(-dtAcc / std::max(40.0, avgFrameMs_ * 0.35));
      double maxDelta = 0;
      for (size_t i = 0; i < specTo_.size(); i++) {
        const double d = specTo_[i] - specDisp_[i];
        specDisp_[i] += d * k;
        const double a = d < 0 ? -d : d;
        if (a > maxDelta) maxDelta = a;
      }
      if (maxDelta < 0.002) { specAcc_ = 0; tweenActive_ = false; }
    }
  }
}

}  // namespace wf
