/* web-tuned-label-driver.ts — the page half of scripts/test-web-tuned-label.mjs: the REAL web Waterfall
 * (web/client/src/waterfall.ts) with its spectrum, needle and LISTEN readout, fed a synthetic band, drawing the tuned
 * station's label (src/utils/tunedStation.ts) where main.ts would hand it over. Bundled by the test; nothing here ships. */
import { Waterfall } from '../web/client/src/waterfall';

const N = 1024;
const CARRIERS = [9_560_000, 9_575_000, 9_580_000, 9_590_000, 9_610_000, 9_635_000];
let seed = 7;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
function frame(centre: number, span: number, out: Float32Array) {
  const bin = span / N, lo = centre - span / 2;
  for (let i = 0; i < N; i++) {
    const f0 = lo + i * bin, f1 = f0 + bin;
    let v = -112 + (rnd() - 0.5) * 6;
    for (const c of CARRIERS) {
      const d = c < f0 ? f0 - c : c > f1 ? c - f1 : 0;
      const p = -60 - (d / 1500) * (d / 1500) * 8;
      if (p > v) v = p;
    }
    out[i] = v;
  }
}

const S: any = (window as any).__tl = {
  wf: null as Waterfall | null,
  bins: new Float32Array(N),
  centre: 9_600_000, span: 96_000,
  setup(w: number, h: number, palette: string, specRatio: number, vfoColor: string) {
    const cv = document.getElementById('wf') as HTMLCanvasElement;
    cv.style.width = w + 'px'; cv.style.height = h + 'px';
    const wf = new Waterfall(cv, { specRatio, palette: palette as any });
    wf.applySettings({ manualRange: { minDb: -125, maxDb: -50 }, wfSharpness: 0 });
    wf.vfoColor = vfoColor;
    wf.filterLow = -4500; wf.filterHigh = 4500;
    S.wf = wf;
    for (let i = 0; i < 120; i++) { frame(S.centre, S.span, S.bins); wf.push(S.bins, S.centre, S.span); wf.tick(10000 + i * 50); }
    return true;
  },
  /** Put the needle at `hz` with one candidate (`name` '' = none) of `source` and draw one frame; `marker` = the spectrum's
   *  own label for it is on (a personal / eibi known-signal label at `hz`). Returns what was drawn, in device px. */
  draw(hz: number, name: string, source = 'eibi', marker = false) {
    const wf = S.wf as any;
    wf.vfoHz = hz;
    wf.tunedCand = name ? { key: String(Math.random()), rows: [{ frequency: hz, name, source }], passband: { lo: -4500, hi: 4500 } } : null;
    wf.knownSignals = marker ? [{ hz, label: name, dec: '', dialHz: hz, mode: 'am', prio: 4, src: source === 'eibi' ? 'eibi' : 'personal' }] : null;
    wf.knownGen = (wf.knownGen || 0) + 1;
    frame(S.centre, S.span, S.bins); wf.push(S.bins, S.centre, S.span);
    wf.tick(performance.now()); wf.draw();
    const W = wf.canvas.width, H = wf.canvas.height;
    const nX = wf.hzToX(hz, W);
    // ★★ the VFO's box (tunedStation.ts vfoLabelLayout): the name over the readout, centred on the needle
    const box = wf.vfoPlan?.box ?? null;
    return { W, H, nX, tuned: wf.tunedDrawn, highlight: wf.tunedHighlight, box, readout: wf.liveLabel(),
             placed: (wf.knownPlaced || []).map((p: any) => p.sig.label) };
  },
  png() { return (document.getElementById('wf') as HTMLCanvasElement).toDataURL('image/png'); },
};
