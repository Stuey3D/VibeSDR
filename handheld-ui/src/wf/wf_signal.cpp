// wf_signal.cpp — port of src/assets/signalProcessor.ts. Float32Array stores are float here, the arithmetic is done in
// double and rounded on store, exactly as JS does. Design notes live in the TS file.
#include "wf_signal.h"
#include "wf_row_place.h"   // jsRound
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace wf {

static constexpr double CLIP_THRESHOLD = 14.97;
static constexpr double PEAK_DECAY_DB_PER_S = 10;
static constexpr double BAND_FLUSH_FRAC = 0.4;
static constexpr double RANGE_MARGIN = 5;
static constexpr double NOISE_PERCENTILE = 0.10;
static constexpr double EDGE_EXCLUDE_FRAC = 0.09;
static constexpr double NO_DATA_DBFS = -200;
static constexpr double PEAK_EXCLUDE_FRAC = 0.004;
static constexpr double MIN_HISTORY_MS = 2000;
static constexpr double MAX_HISTORY_MS = 5000;
static constexpr double FLOOR_FREEZE_SPAN_HZ = 25000;
static constexpr double FLOOR_CLIMB_MAX = 12;
static constexpr double MIN_RANGE_DB = 30;
static constexpr double MAX_SLEW_DB_PER_SEC = 40;

static inline double sgn(double x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }

void SignalProcessor::applySettings(const SignalSettings& p) {
  const bool rangeChanged = p.autoContrast != s_.autoContrast || p.manual != s_.manual ||
      (p.manual && (p.manualMinDb != s_.manualMinDb || p.manualMaxDb != s_.manualMaxDb)) ||
      p.wfBrightness != s_.wfBrightness || p.wfContrast != s_.wfContrast || p.wfSharpness != s_.wfSharpness ||
      p.minRangeDb != s_.minRangeDb || p.hasClipFloor != s_.hasClipFloor || (p.hasClipFloor && p.clipFloorDb != s_.clipFloorDb);
  s_ = p;
  if (!p.peakHold) peakOn_ = false;
  if (rangeChanged) settingsVer_++;
}

double SignalProcessor::slewFloor(double want, double now) {
  if (lastMinAt_ == 0 || !std::isfinite(actualMinDb_) || adoptRange_) { lastMinAt_ = now; return want; }
  const double dt = std::max(0.0, std::min(0.25, (now - lastMinAt_) / 1000));
  lastMinAt_ = now;
  const double step = MAX_SLEW_DB_PER_SEC * dt;
  const double d = want - actualMinDb_;
  return actualMinDb_ + (std::fabs(d) <= step ? d : sgn(d) * step);
}

void SignalProcessor::reset() {
  haveBufs_ = false; peakOn_ = false;
  minHistory_.clear(); maxHistory_.clear();
  lastFrameMs_ = 0; prevCenterHz_ = 0;
}

ProcessedFrame SignalProcessor::process(const float* bins, int n, double centerHz, double bwHz, bool interacting, double now) {
  const SignalSettings& s = s_;
  const double dtSec = lastFrameMs_ != 0 ? std::min(0.5, std::max(0.01, (now - lastFrameMs_) / 1000)) : 0.1;
  lastFrameMs_ = now;

  // 0. view change: drop stale range samples, adopt the next measurement outright
  if (centerHz > 0 && bwHz > 0) {
    const bool moved = std::fabs(centerHz - lastCenterHz_) > bwHz * 0.25 || std::fabs(bwHz - lastBwHz_) > lastBwHz_ * 0.05;
    if (moved && lastBwHz_ > 0) { maxHistory_.clear(); minHistory_.clear(); adoptRange_ = true; }
    lastCenterHz_ = centerHz; lastBwHz_ = bwHz;
  }
  // 0a. buffers
  if (!haveBufs_ || (int)dbAvg_.size() != n) {
    dbAvg_.assign(bins, bins + n); specSmooth_.assign(bins, bins + n);
    peakOn_ = false;
    tmp_.assign(n, 0); normRow_.assign(n, 0); outRow_.assign(n, 0); outSpec_.assign(n, 0); outPeak_.assign(n, 0);
    peakLine_.assign(n, 0);
    minHistory_.clear(); maxHistory_.clear(); hasFrozen_ = false;
    haveBufs_ = true;
  }
  // 0b. settings flush
  if (settingsVerApp_ != settingsVer_) {
    settingsVerApp_ = settingsVer_;
    std::memcpy(dbAvg_.data(), bins, n * sizeof(float));
    minHistory_.clear(); maxHistory_.clear(); hasFrozen_ = false;
  }
  // 0c. band flush
  if (centerHz != 0 && prevCenterHz_ != 0 && bwHz > 0 && std::fabs(centerHz - prevCenterHz_) > bwHz * BAND_FLUSH_FRAC) {
    std::memcpy(dbAvg_.data(), bins, n * sizeof(float));
    std::memcpy(specSmooth_.data(), bins, n * sizeof(float));
    peakOn_ = false;
    minHistory_.clear(); maxHistory_.clear(); hasFrozen_ = false;
  }
  if (centerHz != 0) prevCenterHz_ = centerHz;

  // 1a. FFT smoothing (OWRX-style weight; off while interacting)
  const double w = (interacting || s.avgFrames <= 0) ? 0 : std::min(0.9, s.avgFrames);
  if (w > 0) {
    const double nw = 1 - w;
    for (int i = 0; i < n; i++) dbAvg_[i] = (float)((double)dbAvg_[i] * w + (double)bins[i] * nw);
  } else std::memcpy(dbAvg_.data(), bins, n * sizeof(float));

  // 2. auto-range
  if (s.manual) { actualMinDb_ = s.manualMinDb; actualMaxDb_ = s.manualMaxDb; }
  else {
    uint32_t* hist = dbHist_;
    std::memset(hist, 0, sizeof(dbHist_));
    double absoluteMax = -std::numeric_limits<double>::infinity();
    int count = 0;
    const float* src = dbAvg_.data();
    int edge = (int)std::floor(n * EDGE_EXCLUDE_FRAC);
    if (n - 2 * edge < 16) edge = 0;
    const double clipAt = s.hasClipFloor ? s.clipFloorDb + 0.5 : -std::numeric_limits<double>::infinity();
    for (int i = edge; i < n - edge; i++) {
      const double db = src[i];
      if (!std::isfinite(db)) continue;
      if (bins[i] <= NO_DATA_DBFS || bins[i] <= clipAt) continue;
      count++;
      if (db > absoluteMax) absoluteMax = db;
      int b = (int)(db + 280);   // JS (x | 0): truncation toward zero
      if (b < 0) b = 0; else if (b > 299) b = 299;
      hist[b]++;
    }
    if (count > 0) {
      const double target = std::floor(count * NOISE_PERCENTILE);
      double acc = 0, floorDb = -120;
      for (int b = 0; b < 300; b++) { acc += hist[b]; if (acc > target) { floorDb = b - 280; break; } }
      const double targetMin = std::floor(floorDb - RANGE_MARGIN);
      const double drop = std::floor(count * PEAK_EXCLUDE_FRAC);
      double peakDb = absoluteMax;
      if (drop >= 1) {
        double accTop = 0;
        for (int b = 299; b >= 0; b--) { accTop += hist[b]; if (accTop > drop) { peakDb = b - 280; break; } }
      }
      const double targetMax = std::ceil(peakDb + RANGE_MARGIN);

      maxHistory_.push_back({ targetMax, now });
      while (!maxHistory_.empty() && (now - maxHistory_.front().ts > MAX_HISTORY_MS || maxHistory_.front().ts > now)) maxHistory_.pop_front();
      double sumMax = 0; for (const Sample& x : maxHistory_) sumMax += x.value;
      const double wantMaxDb = sumMax / maxHistory_.size() - s.autoContrast;
      if (lastMaxAt_ == 0 || !std::isfinite(actualMaxDb_) || adoptRange_) actualMaxDb_ = wantMaxDb;
      else {
        const double dt = lastMaxAt_ != 0 ? std::max(0.0, std::min(0.25, (now - lastMaxAt_) / 1000)) : 0.05;
        const double maxStep = MAX_SLEW_DB_PER_SEC * dt;
        const double delta = wantMaxDb - actualMaxDb_;
        actualMaxDb_ += std::fabs(delta) <= maxStep ? delta : sgn(delta) * maxStep;
      }
      lastMaxAt_ = now;

      const bool floorTrust = !(bwHz > 0 && bwHz < FLOOR_FREEZE_SPAN_HZ);
      if (floorTrust || !hasFrozen_) {
        minHistory_.push_back({ targetMin, now });
        while (!minHistory_.empty() && (now - minHistory_.front().ts > MIN_HISTORY_MS || minHistory_.front().ts > now)) minHistory_.pop_front();
        double sumMin = 0; for (const Sample& x : minHistory_) sumMin += x.value;
        const double avgMin = sumMin / minHistory_.size();
        actualMinDb_ = slewFloor(avgMin + s.autoContrast, now);
        if (floorTrust) { frozenFloorDb_ = avgMin; hasFrozen_ = true; }
      } else {
        const double cappedFloor = std::min(targetMin, frozenFloorDb_ + FLOOR_CLIMB_MAX);
        actualMinDb_ = slewFloor(cappedFloor + s.autoContrast, now);
      }
      adoptRange_ = false;
    }
  }
  const double minRange = std::isfinite(s.minRangeDb) && s.minRangeDb > 0 ? s.minRangeDb : MIN_RANGE_DB;
  if (actualMaxDb_ - actualMinDb_ < minRange) actualMaxDb_ = actualMinDb_ + minRange;
  const double dbRange = actualMaxDb_ - actualMinDb_;

  // 4. spatial 5-tap [1,2,3,2,1]/9
  float* tmp = tmp_.data();
  const float* a = dbAvg_.data();
  if (s.spatialSmooth && n >= 5) {
    tmp[0]     = (float)(((double)a[0] * 3 + (double)a[1] * 2) / 5);
    tmp[1]     = (float)(((double)a[0] + (double)a[1] * 2 + (double)a[2] * 2) / 5);
    tmp[n - 1] = (float)(((double)a[n - 2] * 2 + (double)a[n - 1] * 3) / 5);
    tmp[n - 2] = (float)(((double)a[n - 3] + (double)a[n - 2] * 2 + (double)a[n - 1] * 2) / 5);
    for (int k = 2; k < n - 2; k++)
      tmp[k] = (float)(((double)a[k - 2] + (double)a[k - 1] * 2 + (double)a[k] * 3 + (double)a[k + 1] * 2 + (double)a[k + 2]) / 9);
  } else std::memcpy(tmp, a, n * sizeof(float));

  // 5. spectrum EMA (time-normalised, rise 4× fall)
  float* spec = specSmooth_.data();
  const bool noSmooth = s.smoothingFrames <= 1;
  const double tcSec = noSmooth ? 0 : (s.smoothingFrames - 1) / 20;
  const double fallAlpha = noSmooth ? 1.0 : std::min(0.95, 1.0 - std::exp(-dtSec / std::max(0.01, tcSec)));
  const double riseAlpha = noSmooth ? 1.0 : std::min(0.95, fallAlpha * 4);

  // 6. peak seed
  if (s.peakHold && !peakOn_) {
    bool hasSignal = false;
    for (int i = 0; i < n; i++) if (spec[i] != 0) { hasSignal = true; break; }
    if (hasSignal) { std::memcpy(peakLine_.data(), spec, n * sizeof(float)); peakOn_ = true; }
  }
  float* pk = peakOn_ ? peakLine_.data() : nullptr;

  float* norm = normRow_.data();
  const double brightDb = s.wfBrightness;
  for (int j = 0; j < n; j++) {
    const double ta = tmp[j] > spec[j] ? riseAlpha : fallAlpha;
    spec[j] = (float)((double)spec[j] + ta * ((double)tmp[j] - (double)spec[j]));
    if (s.peakHold && pk) {
      const double cur = spec[j];
      pk[j] = (float)(cur > pk[j] ? cur : (double)pk[j] - PEAK_DECAY_DB_PER_S * dtSec);
    }
    const double nrm = std::max(0.0, std::min(1.0, ((double)tmp[j] + brightDb - actualMinDb_) / dbRange));
    double mag = nrm * 255;
    mag = mag < CLIP_THRESHOLD ? 0 : ((mag - CLIP_THRESHOLD) / (255 - CLIP_THRESHOLD)) * 255;
    norm[j] = (float)(mag / 255);
  }
  uint8_t* out = outRow_.data();
  for (int j = 0; j < n; j++) out[j] = (uint8_t)std::max(0.0, std::min(255.0, jsRound((double)norm[j] * 255)));

  const double sf = dbRange > 0 ? s.specFloor / dbRange : 0;
  const double sp = s.specPeakScale / 10;
  for (int j = 0; j < n; j++) {
    const double ns = std::max(0.0, std::min(1.0, ((double)spec[j] - actualMinDb_) / dbRange));
    outSpec_[j] = (float)std::max(0.0, std::min(1.0, (ns + sf) * sp));
    if (s.peakHold && pk) {
      const double np = std::max(0.0, std::min(1.0, ((double)pk[j] - actualMinDb_) / dbRange));
      outPeak_[j] = (float)std::max(0.0, std::min(1.0, (np + sf) * sp));
    } else outPeak_[j] = 0;
  }
  return { out, outSpec_.data(), outPeak_.data(), n, actualMinDb_, actualMaxDb_ };
}

}  // namespace wf
