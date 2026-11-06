/**
 * THE APP'S WATERFALL PIPELINE, IN NODE — the golden reference for handheld-ui/src/wf/wf_model.cpp.
 * WaterfallView.tsx's glue (handleFrame → pumpClock → pushRow, and the UI-thread paceTick / easeStep / specTweenTick) is
 * not an importable module, so it is transliterated here, statement for statement, but EVERY decision it makes is the
 * app's real TS module: signalProcessor.ts, wfClock.ts, wfRowTarget.ts, wfCombine.ts, wfRowPlace.ts, wfPace.ts.
 * The C++ port composes its ports of those modules the same way (wf_model.cpp); this file is what it is checked against.
 * Deliberately the same simplifications as the port: no A/V hold (wfAvSync — the handheld has no audio playout delay),
 * no scale stamps, the clock timer fired at its own tick times. The full-capture layer (SPWB, wfWide.ts) is the app's JS
 * path: the wide sink (pushWide), the floor match's median in handleFrame, and layerWriteRow + fanRow after placeRow —
 * with the pipeline's own `now` where WaterfallView reads Date.now().
 */
import { SignalProcessor, DEFAULT_PROCESSOR_SETTINGS } from '../../src/assets/signalProcessor.ts';
import { boostActive, boostNote, boostRowRate, boostTaper, BOOST_TAIL_MS, clockArrive, clockIdle, clockNextAt, clockRateUnderCap,
         clockReset, clockRowRate, clockRun, clockSetRate, newBoost, newRowClock } from '../../src/assets/wfClock.ts';
import { newRowTargetState, rowTargetUnderCap, rowsForArrival, scrollForCap, stepRowTarget, type WfScroll } from '../../src/assets/wfRowTarget.ts';
import { combineInto, dbPerUnit, newRowCombiner, COMBINE_MAX_FRAMES, type CombFrame } from '../../src/assets/wfCombine.ts';
import { fanRow, newFanState, newMotionState, placeRow, takeFanStep, motionImageRows, wfImageBytes } from '../../src/utils/wfRowPlace.ts';
import { layerWriteRow, makeToIdx, medianOf, newLayerRing, newWideState, parseWide, wideArrive, type WideState } from '../../src/utils/wfWide.ts';
import { contAtWholeRow, contPace, easeAt, easeCompose, paceToCont, paceToSettled, rowPace, rowRate, shownRate, traceLagMs } from '../../src/utils/wfPace.ts';

// The processor reads performance.now(): the harness sets the time each call sees.
export const fakeClock = { t: 1 };
(globalThis as any).performance.now = () => fakeClock.t;

export const RING_W = 1024, RING_ROWS = 1024;
const SMOOTH_TUNE_TAIL_MS = 1000, LOW_FPS_GLIDE_MS = 66, SPEC_TWEEN_MS = 33, SPEC_PX_STEP = 2;
const TARGET_ROWS: Record<string, number> = { smooth: 60, default: 20, eco: 10, low: 5, min2: 2, sharp: 0 };

export interface RefCfg { scroll: WfScroll; smoothTune: boolean; boostOn: boolean; displayHz: number; frameCapHz: number;
                          wfSharpness: number; wfContrast: number; peakHold: boolean; specShow: boolean; traceW: number }
export const DEFAULT_CFG: RefCfg = { scroll: 'default', smoothTune: true, boostOn: true, displayHz: 60, frameCapHz: 0,
                                     wfSharpness: 5, wfContrast: 0, peakHold: true, specShow: true, traceW: 640 };

type Sv = { value: number };
const sv = (v: number): Sv => ({ value: v });

export class RefPipeline {
  cfg: RefCfg;
  proc = new SignalProcessor();
  ring = new Uint8Array(RING_W * (RING_ROWS + motionImageRows(RING_ROWS, RING_W)));
  rowBuf = new Uint8Array(RING_W);
  ms = newMotionState(RING_ROWS, RING_W);
  fan = newFanState(RING_ROWS);
  frameCount = 0;
  viewC = 0; viewBw = 0; viewSet = false; lastInteractAt = 0;
  lastFrameTs = 0; avgFrameMs = 150; lastWfPushAt = 0; avgWfGapMs = 0; rowCarry = { current: 0 };
  rowTargetSt = newRowTargetState(); rowTargetCap = -1;
  rc = newRowClock(); bs = newBoost(); cri = { base: 0, boosted: 0, hz: 60 }; lastEaseLeft = 0;
  clockSeq = 0; clockShown = 0; clockFrames = new Map<number, CombFrame>(); clockComb = newRowCombiner(); clockOut: number[] = [];
  clockArmed = false; lastPumpAt = 0;
  // the display driver (WaterfallView's shared values)
  pShown = sv(0); pAcc = sv(0); pLead = sv(0); pCool = sv(0); pMode = sv(0); pRate = sv(0.03); pTgt = sv(0); pClocked = false;
  pRateT = sv(0); pRateN = sv(0); pPos = sv(0); pCLead = sv(0); pCont = false; pCTgt = sv(1);
  pSRate = sv(0); pSN = sv(0); pST = sv(0); pWrote = 0;
  zA0 = 1; zB0 = 0; zT0 = 0; zDur = 0; zOn = false;
  uHead = 0; scrollFrac = 1; uQuantDraw = 1; uZA = 1; uZB = 0; interacting = false; uQuantAsk = 1;
  pend = { avail: 0, moLo: 1, moHi: 0, k: 1, c: 0 }; pendGen = 0; jsSeen = 0; jsAvail = 0; takenMoLo = 1; takenMoHi = 0;
  traceQ: { at: number; spec: number[]; peak: number[] | null }[] = [];
  specTo: number[] = []; specDisp: number[] = []; peakShown: number[] = []; specAcc = 0; tweenActive = false;
  // the full-capture layer (WaterfallView wideSt / detP50 / p50Scratch / placedRef; the ring is fan.layer)
  wideSt: WideState | null = null; detP50 = NaN; p50Scratch = { buf: new Float32Array(0) }; placed = { startHz: NaN, spanHz: 0 };
  onRow?: (p: RefPipeline, slot: number) => void;
  onFrame?: (f: { row: Uint8Array; dbMin: number; dbMax: number }) => void;

  constructor(cfg: Partial<RefCfg> = {}) {
    this.cfg = { ...DEFAULT_CFG, ...cfg };
    const sharpBase = this.cfg.scroll === 'smooth' ? 3 : this.cfg.scroll === 'default' ? 2 : this.cfg.scroll === 'eco' ? 1.75 : 1.5;
    this.proc.applySettings({ ...DEFAULT_PROCESSOR_SETTINGS, peakHold: this.cfg.peakHold, wfContrast: this.cfg.wfContrast,
                              wfSharpness: Math.min(12, sharpBase * (this.cfg.wfSharpness / 5) * 2) });
  }
  setView(c: number, bw: number) { this.viewC = c; this.viewBw = bw; this.viewSet = true; }
  noteInteraction(t: number) { this.lastInteractAt = t; }
  uniforms() {
    const sharpBase = this.cfg.scroll === 'smooth' ? 3 : this.cfg.scroll === 'default' ? 2 : this.cfg.scroll === 'eco' ? 1.75 : 1.5;
    return { headF: this.uHead, frac: this.scrollFrac, quant: this.uQuantDraw, moLo: this.takenMoLo, moHi: this.takenMoHi,
             zA: this.uZA, zB: this.uZB, sharp: Math.min(2.0, (this.cfg.wfSharpness / 10) * 1.5 * (sharpBase / 1.5)),
             contrast: Math.max(-1, Math.min(1, this.cfg.wfContrast / 10)) };
  }
  image(): Uint8Array { return wfImageBytes(this.fan, this.ms, this.ring, RING_W, RING_ROWS); }

  pushFrameU8(bins: Uint8Array, fftOrder: boolean, c: number, span: number, t: number) {
    const n = bins.length, h = fftOrder ? n >> 1 : 0, db = new Float32Array(n);
    for (let i = 0; i < n; i++) db[i] = bins[(i + h) % n] - 256;
    this.pushFrameDb(db, c, span, t);
  }

  // ── handleFrame ──
  pushFrameDb(fbins: Float32Array, centreHz: number, spanHz: number, now: number) {
    const cfg = this.cfg;
    const vC = this.viewSet ? this.viewC : centreHz, vBw = this.viewSet ? this.viewBw : spanHz;
    if (!this.viewSet) { this.viewC = centreHz; this.viewBw = spanHz; }
    const interacting = cfg.smoothTune && now - this.lastInteractAt < SMOOTH_TUNE_TAIL_MS;
    fakeClock.t = now;
    const frame = this.proc.process(fbins, vC, vBw, interacting);
    this.onFrame?.(frame);
    if (this.wideSt) this.detP50 = medianOf(fbins, this.p50Scratch);
    const boost = interacting;
    if (this.lastFrameTs > 0 && !boost) this.avgFrameMs = this.avgFrameMs * 0.8 + (now - this.lastFrameTs) * 0.2;
    this.lastFrameTs = now;
    this.interacting = boost;
    const lowFps = this.avgFrameMs > LOW_FPS_GLIDE_MS;
    const targetRows = TARGET_ROWS[cfg.scroll];
    if (this.rowTargetCap !== targetRows) { this.rowTargetSt = newRowTargetState(); this.rowTargetCap = targetRows; }
    const wfScroll = scrollForCap(targetRows);
    const feedFps = 1000 / Math.max(1, this.avgFrameMs);
    const clocked = wfScroll !== 'sharp';
    const baseRowTarget = clocked
      ? (this.avgFrameMs > 0 ? clockRateUnderCap(clockRowRate(this.rowTargetSt, wfScroll, feedFps), cfg.frameCapHz) : 0)
      : rowTargetUnderCap(this.avgFrameMs > 0 ? stepRowTarget(this.rowTargetSt, wfScroll, 1000 / this.avgFrameMs) : this.rowTargetSt.target,
                          feedFps, cfg.frameCapHz);
    const ptNow = now;
    const easeLeft = this.fan.easeLeft, easing = easeLeft > 0;
    const easeStarted = easeLeft > this.lastEaseLeft; this.lastEaseLeft = easeLeft;
    const inAt = this.lastInteractAt;
    if (inAt > this.bs.lastMove && ptNow - inAt < BOOST_TAIL_MS) boostNote(this.bs, inAt);
    if (easeStarted) boostNote(this.bs, ptNow);
    const boostOn = clocked && baseRowTarget > 0 && boostActive(this.bs, ptNow, easing); void boostOn;
    const dispHz = cfg.displayHz;
    const boostRate = clocked && baseRowTarget > 0 ? boostRowRate(baseRowTarget, feedFps, dispHz, cfg.boostOn) : baseRowTarget;
    const rowTarget = clocked && baseRowTarget > 0 ? boostTaper(this.bs, baseRowTarget, boostRate, ptNow, dispHz) : baseRowTarget;
    this.cri.base = clocked ? baseRowTarget : 0; this.cri.boosted = boostRate; this.cri.hz = dispHz;
    const repeatsCarry = rowTarget > 0;
    const wfBoost = boost || (cfg.smoothTune && lowFps && !repeatsCarry);
    let traceAt = 0;
    if (this.lastWfPushAt > 0) {
      const wfDt = now - this.lastWfPushAt;
      if (wfDt > 0 && wfDt < 2000 && !boost) this.avgWfGapMs = this.avgWfGapMs > 0 ? this.avgWfGapMs * 0.8 + wfDt * 0.2 : wfDt;
    }
    this.lastWfPushAt = now;
    const rowCentre = centreHz, rowHzBin = spanHz > 0 ? spanHz / frame.row.length : 0;
    const revealMs = this.avgWfGapMs > 0 ? this.avgWfGapMs : this.avgFrameMs;
    const dur = Math.max(15, Math.min(1000, revealMs));
    const onClock = clocked && rowTarget > 0;
    const rowsNow = onClock ? 0 : rowsForArrival(this.rowCarry, dur, rowTarget);
    this.fan.rowRate = rowTarget > 0 ? rowTarget : (this.avgFrameMs > 0 ? 1000 / this.avgFrameMs : 20);
    if (rowsNow > 0) this.pushRow(frame.row, rowCentre, rowHzBin, now);
    if (onClock) {
      const id = ++this.clockSeq;
      const slot: CombFrame = { row: new Uint8Array(frame.row), c: rowCentre, h: rowHzBin, k: dbPerUnit(frame.dbMin, frame.dbMax) };
      this.clockFrames.set(id, slot);
      clockSetRate(this.rc, rowTarget, now);
      const pAt = clockArrive(this.rc, id, now, boost, 0);
      this.pumpClock(now);
      if (!boost) traceAt = pAt + traceLagMs(rowTarget);
    } else if (this.rc.rate > 0) {
      clockReset(this.rc); clockSetRate(this.rc, 0, 0); this.clockFrames.clear();
    }
    this.uQuantAsk = wfBoost ? 0 : 1;
    this.pTgt.value = rowTarget > 0 ? rowTarget / 1000 : 0;
    this.pClocked = onClock;
    if (cfg.specShow) {
      const fs = frame.spec, fn = fs.length, width = cfg.traceW;
      let next: number[];
      if (width > 0 && fn > Math.ceil(width / SPEC_PX_STEP)) {
        const m = Math.ceil(width / SPEC_PX_STEP);
        next = new Array(m);
        for (let k = 0; k < m; k++) next[k] = fs[Math.floor(((k * SPEC_PX_STEP) / width) * fn)];
      } else next = Array.from(fs);
      let peak: number[] | null = null;
      if (cfg.peakHold && !boost) {
        const count = Math.ceil(Math.floor(width) / SPEC_PX_STEP); peak = new Array(count);
        for (let k = 0; k < count; k++) peak[k] = frame.peak[Math.floor(((k * SPEC_PX_STEP) / width) * frame.peak.length)];
      } else if (boost) this.proc.resetPeakHold();
      if (!(traceAt > now)) traceAt = 0;
      this.traceShow(next, peak, traceAt === 0 ? now : traceAt);
    }
  }
  private traceApply(spec: number[], peak: number[] | null) {
    this.specTo = spec;
    if (this.specDisp.length !== spec.length) this.specDisp = Array.from(spec);
    this.tweenActive = true;
    this.peakShown = peak ?? [];
  }
  private traceShow(spec: number[], peak: number[] | null, at: number) {
    const now = this.lastFrameTs;
    if (!(at > now)) { this.traceQ.length = 0; this.traceApply(spec, peak); return; }
    const q = this.traceQ;
    if (q.length && at < q[q.length - 1].at) at = q[q.length - 1].at;
    q.push({ at, spec, peak });
    const cap = Math.max(4, this.rc.q.length + 1);
    while (q.length > cap) q.shift();
  }
  private traceRun(now: number) {
    let due: { at: number; spec: number[]; peak: number[] | null } | null = null;
    while (this.traceQ.length && this.traceQ[0].at <= now + 1) due = this.traceQ.shift()!;
    if (due) this.traceApply(due.spec, due.peak);
  }

  // ── pumpClock ──
  pumpClock(now: number) {
    const rc = this.rc, out = this.clockOut; out.length = 0;
    const cri = this.cri, bs = this.bs;
    if (cri.base > 0 && rc.rate > 0 && (bs.on || (bs.peak ?? 0) > 0)) {
      boostActive(bs, now, this.fan.easeLeft > 0);
      const r = boostTaper(bs, cri.base, cri.boosted, now, cri.hz);
      if (r > 0 && r !== rc.rate) { clockSetRate(rc, r, now); this.pTgt.value = r / 1000; this.fan.rowRate = r; }
    }
    clockRun(rc, now, out);
    const fr = this.clockFrames;
    for (let i = 0; i < out.length; i++) {
      if (out[i] > this.clockShown) { combineInto(this.clockComb, fr, this.clockShown, out[i]); this.clockShown = out[i]; }
      const f = fr.get(out[i]);
      if (f) this.pushRow(f.row, f.c, f.h, now);
    }
    if (fr.size > 6) {
      for (const [id] of [...fr]) if (id <= this.clockShown && id !== rc.cur) fr.delete(id);
      if (fr.size > COMBINE_MAX_FRAMES + 2) {
        const keep = new Set<number>(rc.q.map(f => f.id)); keep.add(rc.cur);
        for (const [id] of [...fr]) { if (fr.size <= COMBINE_MAX_FRAMES + 2) break; if (!keep.has(id)) fr.delete(id); }
      }
    }
    this.lastPumpAt = now;
    this.clockArmed = !clockIdle(rc, now) && clockNextAt(rc) >= 0;
  }

  // ── pushRow ──
  // ── WaterfallView's wide sink (the JS path) ──
  pushWide(u8: Uint8Array, now: number): boolean {
    const row = parseWide(u8);
    if (!row) return false;
    const st = this.wideSt ?? (this.wideSt = newWideState());
    wideArrive(st, row, now, this.placed.startHz, this.placed.spanHz, this.detP50);
    const fan = this.fan, L = fan.layer;
    if (!L || L.F !== row.floor.length || L.C !== row.cells) fan.layer = newLayerRing(RING_ROWS, row.floor.length, row.cells);
    return true;
  }

  pushRow(srcRow: Uint8Array, rowCenterHz: number, rowHzPerBin: number, now: number) {
    const srcN = srcRow.length;
    let row = srcRow, n = srcN;
    if (srcN !== RING_W) {
      const dst = this.rowBuf;
      if (srcN > RING_W) {
        const step = srcN / RING_W;
        for (let i = 0; i < RING_W; i++) {
          const a = (i * step) | 0, b = Math.min(srcN, ((i + 1) * step) | 0) || a + 1;
          let m = 0;
          for (let k = a; k < b; k++) if (srcRow[k] > m) m = srcRow[k];
          dst[i] = m;
        }
      } else {
        const step = srcN / RING_W;
        for (let i = 0; i < RING_W; i++) dst[i] = srcRow[(i * step) | 0];
      }
      row = dst; n = RING_W;
    }
    if (srcN !== RING_W && rowHzPerBin > 0) rowHzPerBin = rowHzPerBin * srcN / RING_W;
    const slot = placeRow(this.ring, RING_ROWS, n, this.frameCount, row, rowCenterHz, rowHzPerBin, this.viewC, this.viewBw,
                          this.ms, this.fan, RING_ROWS, this.placed);
    const layer = this.fan.layer;
    if (layer) {
      const w = this.cfg.traceW, placed = this.placed;
      layer.fadeTx = w > 0 ? Math.max(2, Math.min(24, 6 * RING_W / w)) : 6;
      const r = this.proc.getRange(), ws = this.wideSt;
      layerWriteRow(layer, slot, ws, placed.startHz, placed.spanHz, now,
                    makeToIdx(r.dbMin, r.dbMax, this.proc.getSettings().wfBrightness || 0, ws ? ws.off : 0));
      fanRow(this.fan, this.ring, n, slot, RING_ROWS);
    }
    this.frameCount += 1;
    const [k, c] = takeFanStep(this.fan);
    this.pend = { avail: this.frameCount, moLo: this.ms.lo, moHi: this.ms.hi, k, c };
    this.pendGen++;
    this.onRow?.(this, slot);
  }

  // ── one display frame ──
  tick(now: number, gap: number) {
    for (let guard = 0; guard < 64 && this.clockArmed; guard++) {
      const nx = clockNextAt(this.rc);
      if (nx < 0) { this.clockArmed = false; break; }
      if (nx > now) break;
      this.pumpClock(Math.max(nx, this.lastPumpAt));
    }
    if (this.traceQ.length && this.traceQ[0].at <= now + 1) this.traceRun(now);
    if (this.pendGen !== this.jsSeen) {
      this.jsSeen = this.pendGen; this.jsAvail = this.pend.avail;
      this.takenMoLo = this.pend.moLo; this.takenMoHi = this.pend.moHi;
      this.easeStep(this.pend.k, this.pend.c, now);
    }
    if (this.jsAvail > 0) this.paceTick(this.jsAvail, now, gap);
    if (this.tweenActive) {
      this.specAcc += gap;
      const to = this.specTo, disp = this.specDisp;
      if (this.specAcc >= SPEC_TWEEN_MS && to.length && disp.length === to.length) {
        const dtAcc = this.specAcc; this.specAcc = 0;
        const k = 1 - Math.exp(-dtAcc / Math.max(40, this.avgFrameMs * 0.35));
        let maxDelta = 0;
        for (let i = 0; i < to.length; i++) { const d = to[i] - disp[i]; disp[i] += d * k; const a = d < 0 ? -d : d; if (a > maxDelta) maxDelta = a; }
        if (maxDelta < 0.002) { this.specAcc = 0; this.tweenActive = false; }
      }
    }
  }
  // WaterfallView easeStep
  private easeStep(k: number, c: number, ts: number) {
    if (k === 1 && c === 0) return;
    let a = 1, b = 0;
    if (this.zOn) { const e = easeAt(this.zA0, this.zB0, this.zT0, this.zDur, ts); a = e[0]; b = e[1]; }
    const nx = easeCompose(a, b, k, c);
    this.zA0 = nx[0]; this.zB0 = nx[1]; this.zT0 = ts;
    this.zDur = Math.max(40, Math.min(250, 1 / Math.max(1e-4, this.pRate.value)));
    this.zOn = true;
  }
  // WaterfallView paceTick
  private paceTick(avail: number, ts: number, gap: number) {
    const interacting = this.interacting;
    rowRate(avail, ts, this.pRate, this.pRateN, this.pRateT, interacting);
    const wantCont = this.uQuantAsk < 0.5;
    const cRate = this.pClocked && this.pTgt.value > 0 ? this.pTgt.value : (this.pSRate.value > 0 ? this.pSRate.value : this.pRate.value);
    const pShown = this.pShown;
    if (wantCont && !this.pCont) { paceToCont(pShown, this.pPos, this.pLead, this.pCLead, this.pCTgt); this.pCont = true; this.uQuantDraw = 0; }
    if (this.pCont) {
      const land = !wantCont && contAtWholeRow(this.pPos.value, cRate, gap);
      const pos = contPace(avail, this.pPos, this.pCLead, cRate, gap, this.pCTgt.value);
      if (land) {
        paceToSettled(avail, this.pPos, pShown, this.pAcc, this.pLead, this.pCLead, this.pMode);
        if (this.pClocked && this.pTgt.value > 0) this.pAcc.value = Math.min(0.5, this.pTgt.value * gap / 2);
        this.pCont = false; this.uQuantDraw = 1; this.uHead = pShown.value; this.scrollFrac = 1;
      } else if (interacting || ts - this.pWrote >= 12) {
        this.pWrote = ts;
        const w = Math.floor(pos);
        this.uHead = w + 1; this.scrollFrac = pos - w;
      }
    } else {
      const shown = rowPace(avail, pShown, this.pAcc, this.pLead, this.pCool, this.pMode, this.pTgt.value > 0 ? this.pTgt.value : this.pRate.value,
                            gap, true, this.pClocked && this.pTgt.value > 0);
      shownRate(shown, ts, this.pSRate, this.pSN, this.pST);
      this.uHead = shown; this.scrollFrac = 1;
    }
    if (this.zOn) {
      const e = easeAt(this.zA0, this.zB0, this.zT0, this.zDur, ts);
      this.uZA = e[0]; this.uZB = e[1];
      if (e[0] === 1 && e[1] === 0) this.zOn = false;
    }
  }
}
