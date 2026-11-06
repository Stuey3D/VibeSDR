// wf_pace.cpp — port of src/utils/wfPace.ts and WaterfallView's paceTick/easeStep. Design notes in the TS.
#include "wf_pace.h"
#include "wf_row_place.h"   // jsRound
#include <algorithm>
#include <cmath>

namespace wf {

double traceLagMs(double rate) {
  return rate > 0 ? std::max(1.0, rate * PLAYOUT_MS / 1000) * 1000 / rate : 0;
}

void rowRate(double avail, double t, double& rateSv, double& nSv, double& tSv, bool frozen) {
  const double dt = t - tSv;
  if (frozen || tSv <= 0 || dt > 2000 || avail < nSv) { nSv = avail; tSv = t; return; }
  if (dt < 500) return;
  const double inst = (avail - nSv) / dt;
  if (inst > 0) rateSv = std::fabs(inst / rateSv - 1) > 0.25 ? inst : rateSv * 0.6 + inst * 0.4;
  nSv = avail; tSv = t;
}

double clockPace(double avail, double& shownSv, double& accSv, double& leadSv, double& coolSv, double& modeSv, double rate,
                 double gap, bool settled) {
  double shown = shownSv;
  const double target = std::max(1.0, rate * PLAYOUT_MS);
  const double expect = rate * gap;
  if (!settled || shown <= 0 || shown > avail || avail - shown > target + 6) {
    shown = avail; accSv = std::min(0.5, expect / 2); leadSv = 0; coolSv = 0; modeSv = 0;
  } else {
    const double lead0 = avail - shown;
    double lead = leadSv;
    const double cool = coolSv + 1;
    const bool far = std::fabs(lead - target) > 1.5;
    const bool slipOk = cool >= (far ? 6 : 30);
    const bool filling = modeSv == 0;
    if (filling && lead >= target - 0.1) modeSv = 1;
    const double shortBy = filling ? 0.1 : 0.7;
    if (!filling && lead < target - shortBy) modeSv = 0;
    double inc = expect;
    if ((filling ? cool >= 6 : slipOk) && lead < target - shortBy) { inc = 0; coolSv = 0; lead += expect; }
    else if (slipOk && lead > target + 0.7 && lead0 >= 2) { inc = 2 * expect; coolSv = 0; lead -= expect; }
    else coolSv = cool;
    double acc = accSv + inc;
    const double maxPer = std::max(1.0, jsRound(expect));
    double n = 0;
    while (acc >= 1 && shown < avail && n < maxPer) { shown++; acc -= 1; n++; }
    if (acc >= 1) acc = std::min(acc, 1 + expect / 2);
    accSv = acc;
    const double a = std::min(1.0, gap / 500);
    leadSv = lead + (avail - shown - lead) * a;
  }
  shownSv = shown;
  return shown;
}

double rowPace(double avail, double& shownSv, double& accSv, double& leadSv, double& coolSv, double& modeSv, double rate,
               double gap, bool settled, bool clocked) {
  if (clocked) return clockPace(avail, shownSv, accSv, leadSv, coolSv, modeSv, rate, gap, settled);
  double shown = shownSv;
  const double target = std::max(1.0, rate * PLAYOUT_MS);
  if (!settled || shown <= 0 || shown > avail || avail - shown > target + 6) {
    shown = avail; accSv = 0; leadSv = 0; coolSv = 0; modeSv = 0;
  } else {
    const double lead0 = avail - shown;
    double lead = leadSv;
    const double expect = rate * gap;
    const double whole = jsRound(expect);
    const double m = expect > 0 ? jsRound(1 / expect) : 0;
    const double cool = coolSv + 1;
    const double prevMode = modeSv;
    double mode = 0;
    if (expect >= 0.75) { const double e = std::fabs(expect - whole); if (e < 0.15 || (prevMode == whole && e < 0.3)) mode = whole; }
    else if (m >= 2) { const double e = std::fabs(expect * m - 1); if (e < 0.15 || (prevMode == -m && e < 0.3)) mode = -m; }
    if (mode != prevMode) { modeSv = mode; accSv = 0; }
    const bool far = std::fabs(lead - target) > 1.5;
    const bool slipOk = cool >= (far ? 6 : 30);
    if (mode > 0) {
      double step = mode;
      if (slipOk && lead > target + 0.6 && lead0 >= mode + 1) { step = mode + 1; coolSv = 0; lead -= 1; }
      else if (slipOk && lead < target - 0.6) { step = 0; coolSv = 0; lead += mode; }
      else coolSv = cool;
      if (lead0 >= step) shown += step;
      accSv = 0;
    } else if (mode < 0) {
      const double mm = -mode;
      const double n = accSv + 1;
      double due = mm, slip = 0;
      if (slipOk && lead > target + 0.6 && lead0 >= 2) { due = mm - 1; slip = -1; }
      else if (slipOk && lead < target - 0.6) { due = mm + 1; slip = 1; }
      if (n >= due && lead0 >= 1) {
        shown++; accSv = 0;
        if (slip != 0) { coolSv = 0; lead += slip / mm; } else coolSv = cool;
      } else { accSv = std::min(n, 2 * mm); coolSv = cool; }
    } else {
      const double k = std::max(0.85, std::min(1.25, 1 + 0.15 * (lead - target)));
      double acc = accSv + expect * k;
      while (acc >= 1 && shown < avail) { shown++; acc -= 1; }
      accSv = std::min(acc, shown == avail ? 1.0 : 2.0);
      coolSv = cool;
    }
    const double a = std::min(1.0, gap / 500);
    leadSv = lead + (avail - shown - lead) * a;
  }
  shownSv = shown;
  return shown;
}

double contPace(double avail, double& posSv, double& leadSv, double rate, double gap, double target) {
  const double tgt = std::max(0.75, target);
  double pos = posSv;
  if (!(pos > 0) || pos > avail || avail - pos > tgt + 6) {
    pos = std::max(avail > 0 ? 1.0 : 0.0, avail - tgt);
    leadSv = avail - pos;
  } else {
    const double lead = leadSv;
    const double k = std::max(0.97, std::min(1.03, 1 + 0.02 * (lead - tgt)));
    pos = std::min(avail, pos + rate * gap * k);
    const double a = std::min(1.0, gap / 500);
    leadSv = lead + (avail - pos - lead) * a;
  }
  posSv = pos;
  return pos;
}

void paceToCont(double& shownSv, double& posSv, double& leadSv, double& contLeadSv, double& targetSv) {
  posSv = shownSv; contLeadSv = leadSv; targetSv = leadSv;
}

void shownRate(double shown, double t, double& rateSv, double& lastNSv, double& lastTSv) {
  if (lastTSv <= 0 || shown < lastNSv || t - lastTSv > 3000) { lastNSv = shown; lastTSv = t; return; }
  const double d = shown - lastNSv;
  if (d <= 0) return;
  const double per = (t - lastTSv) / d;
  const double r = 1 / per;
  rateSv = rateSv > 0 && std::fabs(r / rateSv - 1) < 0.5 ? rateSv * 0.9 + r * 0.1 : r;
  lastNSv = shown; lastTSv = t;
}

bool contAtWholeRow(double pos, double rate, double gap) {
  const double frac = pos - std::floor(pos);
  return frac < 1e-6 || 1 - frac <= rate * gap * 1.05;
}

void paceToSettled(double avail, double& posSv, double& shownSv, double& accSv, double& leadSv, double& contLeadSv, double& modeSv) {
  shownSv = std::min(avail, jsRound(posSv));
  accSv = 0; modeSv = 0;
  leadSv = contLeadSv;
}

void easeAt(double a0, double b0, double t0, double dur, double ts, double& a, double& b) {
  const double p = dur > 0 ? std::min(1.0, std::max(0.0, (ts - t0) / dur)) : 1;
  a = 1 + (a0 - 1) * (1 - p); b = b0 * (1 - p);
}
void easeCompose(double a, double b, double k, double c, double& na, double& nb) {
  if (!(k > 0) || !std::isfinite(c)) { na = 1; nb = 0; return; }
  na = a / k; nb = (b - c) / k;
}

// WaterfallView easeStep
void easeStep(PaceState& p, double k, double c, double ts) {
  if (k == 1 && c == 0) return;
  double a = 1, b = 0;
  if (p.zOn) easeAt(p.zA0, p.zB0, p.zT0, p.zDur, ts, a, b);
  double na, nb; easeCompose(a, b, k, c, na, nb);
  p.zA0 = na; p.zB0 = nb; p.zT0 = ts;
  p.zDur = std::max(40.0, std::min(250.0, 1 / std::max(1e-4, p.pRate)));
  p.zOn = true;
}

// WaterfallView paceTick
double paceTick(PaceState& p, double avail, double ts, double gap) {
  const bool interacting = p.interacting;
  rowRate(avail, ts, p.pRate, p.pRateN, p.pRateT, interacting);
  const double before = p.uHead;
  const bool wantCont = p.uQuantAsk < 0.5;
  const double cRate = p.pClocked && p.pTgt > 0 ? p.pTgt : (p.pSRate > 0 ? p.pSRate : p.pRate);
  if (wantCont && !p.pCont) {
    paceToCont(p.pShown, p.pPos, p.pLead, p.pCLead, p.pCTgt);
    p.pCont = true;
    p.uQuantDraw = 0;
  }
  if (p.pCont) {
    const bool land = !wantCont && contAtWholeRow(p.pPos, cRate, gap);
    const double pos = contPace(avail, p.pPos, p.pCLead, cRate, gap, p.pCTgt);
    if (land) {
      paceToSettled(avail, p.pPos, p.pShown, p.pAcc, p.pLead, p.pCLead, p.pMode);
      if (p.pClocked && p.pTgt > 0) p.pAcc = std::min(0.5, p.pTgt * gap / 2);
      p.pCont = false;
      p.uQuantDraw = 1;
      p.uHead = p.pShown;
      p.scrollFrac = 1;
    } else if (interacting || ts - p.pWrote >= 12) {
      p.pWrote = ts;
      const double w = std::floor(pos);
      p.uHead = w + 1;
      p.scrollFrac = pos - w;
    }
  } else {
    const double shown = rowPace(avail, p.pShown, p.pAcc, p.pLead, p.pCool, p.pMode, p.pTgt > 0 ? p.pTgt : p.pRate, gap, true,
                                 p.pClocked && p.pTgt > 0);
    shownRate(shown, ts, p.pSRate, p.pSN, p.pST);
    p.uHead = shown;
    p.scrollFrac = 1;
  }
  if (p.zOn) {
    double a, b; easeAt(p.zA0, p.zB0, p.zT0, p.zDur, ts, a, b);
    p.uZA = a; p.uZB = b;
    if (a == 1 && b == 0) p.zOn = false;
  }
  return p.uHead - before;
}

}  // namespace wf
