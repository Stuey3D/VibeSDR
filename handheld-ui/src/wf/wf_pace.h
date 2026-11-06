// wf_pace.h — WHICH ROWS THE WATERFALL SHOWS on the display's clock: the settled release (clockPace for clocked rows,
// rowPace for NATIVE), the continuous pace while interacting (contPace) and the hand-overs, and the screen ease
// (uZA/uZB). A port of src/utils/wfPace.ts (worklets in the app; plain functions here). The app's SharedValues are
// plain doubles in PaceState. No GL; tested against the TS by test/run_golden.sh.
#pragma once

namespace wf {

constexpr double PLAYOUT_MS = 40;

double traceLagMs(double rate);
void rowRate(double avail, double t, double& rateSv, double& nSv, double& tSv, bool frozen = false);
double clockPace(double avail, double& shownSv, double& accSv, double& leadSv, double& coolSv, double& modeSv, double rate,
                 double gap, bool settled);
double rowPace(double avail, double& shownSv, double& accSv, double& leadSv, double& coolSv, double& modeSv, double rate,
               double gap, bool settled, bool clocked = false);
double contPace(double avail, double& posSv, double& leadSv, double rate, double gap, double target);
void paceToCont(double& shownSv, double& posSv, double& leadSv, double& contLeadSv, double& targetSv);
void shownRate(double shown, double t, double& rateSv, double& lastNSv, double& lastTSv);
bool contAtWholeRow(double pos, double rate, double gap);
void paceToSettled(double avail, double& posSv, double& shownSv, double& accSv, double& leadSv, double& contLeadSv, double& modeSv);
void easeAt(double a0, double b0, double t0, double dur, double ts, double& a, double& b);
void easeCompose(double a, double b, double k, double c, double& na, double& nb);

/** WaterfallView's display-driver state (pShown … zOn) and its paceTick, ported as one struct. */
struct PaceState {
  double pShown = 0, pAcc = 0, pLead = 0, pCool = 0, pMode = 0, pRate = 0.03, pTgt = 0;
  bool pClocked = false;
  double pRateT = 0, pRateN = 0, pPos = 0, pCLead = 0; bool pCont = false; double pCTgt = 1;
  double pSRate = 0, pSN = 0, pST = 0, pWrote = 0;
  double zA0 = 1, zB0 = 0, zT0 = 0, zDur = 0; bool zOn = false;
  // the uniforms it drives
  double uHead = 0, scrollFrac = 1, uQuantDraw = 1, uZA = 1, uZB = 0;
  // inputs the producer sets
  bool interacting = false;   // interactSv
  double uQuantAsk = 1;       // uQuantSv: 0 = continuous (a gesture)
};
/** A new image's screen move (x_old = c + k·x_new): start or extend the ease (WaterfallView easeStep). */
void easeStep(PaceState& p, double k, double c, double ts);
/** One display frame of the driver (WaterfallView paceTick). Returns the rows advanced. */
double paceTick(PaceState& p, double avail, double ts, double gap);

}  // namespace wf
