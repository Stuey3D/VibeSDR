/* web-wf-fan-driver.ts — the page half of scripts/test-web-wf-fan.mjs: the REAL web Waterfall (web/client/src/waterfall.ts,
 * its WebGL renderer or its 2D fallback) fed a synthetic band on a VIRTUAL clock, so a zoom, a tune, a jump and a resize
 * draw the same picture on every run, on any machine. Bundled by the test with esbuild; nothing here ships.
 *
 * The band: a noise floor that is a fixed function of FREQUENCY (so it moves with the view, like a real band) and carriers
 * at fixed frequencies with a physical width (~250 Hz), so a zoom in widens them in bins as a real carrier does. Frames are
 * sent at the "server's" own geometry — on `rungs`, the span snaps to UberSDR-style fixed Hz/bin rungs whatever was asked.
 * Each row the waterfall writes is recorded with the view it was DRAWN against (its own geometry), so the test can check
 * every carrier, in every row, lands at the column its own geometry puts it. */
import { Waterfall } from '../web/client/src/waterfall';

const realNow = performance.now.bind(performance);
let T = 10000;
(performance as any).now = () => T;

const N = 1024;
const CARRIERS = [7105000, 7118000, 7131500, 7142000, 7150000, 7157000, 7166000, 7181000, 7196000, 7212000];
let seed = 1;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
// The floor: a fixed function of frequency (hashed per 100 Hz) plus a little per-frame jitter.
const floorAt = (f: number) => { const k = Math.floor(f / 100); const h = Math.sin(k * 12.9898) * 43758.5453; return -112 + 6 * (h - Math.floor(h)); };

function frame(centre: number, span: number, out: Float32Array) {
  const bin = span / N, lo = centre - span / 2;
  for (let i = 0; i < N; i++) {
    const f0 = lo + i * bin, f1 = f0 + bin;
    // the bin's power: the floor at its centre, and the strongest carrier overlapping it (peak over the bin, like an FFT)
    let v = floorAt(f0 + bin / 2) + (rnd() - 0.5) * 2;
    for (const c of CARRIERS) {
      const d = c < f0 ? f0 - c : c > f1 ? c - f1 : 0;   // distance from the bin
      const p = -55 - (d / 120) * (d / 120) * 8;
      if (p > v) v = p;
    }
    out[i] = v;
  }
}

type View = { centre: number; span: number };
/** UberSDR-style rungs (Hz a bin): a zoom request lands on the nearest. */
const RUNGS = [50, 100, 200, 300, 500, 1000, 2000];
const snap = (span: number) => { const b = span / N; let best = RUNGS[0]; for (const r of RUNGS) if (Math.abs(Math.log(r / b)) < Math.abs(Math.log(best / b))) best = r; return best * N; };

const S: any = (window as any).__wfDrv = {
  wf: null as Waterfall | null,
  view: { centre: 7150000, span: 96000 } as View,   // what the "server" sends next
  rungs: false,
  feedMs: 50,
  nextFeed: 0,
  rows: [] as { lo: number; span: number; t: number; dLo: number; dHi: number }[],   // every row written, oldest first
  bins: new Float32Array(N),
  cost: [] as number[],
  mode: 'webgl',
  setup(mode: string, w: number, h: number, scroll = 'default') {
    S.mode = mode;
    const cv = document.getElementById('wf') as HTMLCanvasElement;
    cv.style.width = w + 'px'; cv.style.height = h + 'px';
    const wf = new Waterfall(cv, { specRatio: 0, palette: 'gqrx' });
    (wf as any)._drawVfo = () => {};
    if (mode === '2d') { (wf as any).gl = null; (wf as any).glCanvas = null; (wf as any).wf.width = 0; wf.resize(); }
    wf.setScroll(scroll as any);
    // a fixed range with the floor well above the palette's 0, and no unsharp mask: palette index 0 (black) is then ONLY
    // a pixel with no data — what the test counts
    wf.applySettings({ manualRange: { minDb: -125, maxDb: -50 }, wfSharpness: 0 });
    // record each row's DRAWN geometry (the view it was drawn against) in ring order
    const note = (wf as any)._noteRowView.bind(wf);
    (wf as any)._noteRowView = (lo: number, span: number) => { const w = wf as any;
      // the frame the row is drawn from: a carrier outside it is the row below continued (a zoom-in's or a tune's edge)
      const uc = w.pacer.useCur, dLo = uc ? w.curLo : w.prevLo, dHi = dLo + (uc ? w.curSpan : w.prevSpan);
      S.rows.push({ lo, span, t: T, dLo, dHi }); if (S.rows.length > 6000) S.rows.splice(0, 2000); note(lo, span); };
    S.wf = wf;
    return !!(wf as any).gl === (mode === 'webgl');
  },
  /** Ask for a view (a tune or a zoom). On rungs the span the frames carry snaps. */
  ask(centre: number, span: number) { S.view = { centre, span: S.rungs ? snap(span) : span }; },
  /** Run `ms` of display frames at 60 Hz, the feed at feedMs. */
  run(ms: number) {
    const wf = S.wf as Waterfall;
    const end = T + ms;
    while (T < end) {
      T += 1000 / 60;
      if (T >= S.nextFeed) {
        S.nextFeed = (S.nextFeed || T) + S.feedMs;
        frame(S.view.centre, S.view.span, S.bins);
        wf.push(S.bins, S.view.centre, S.view.span);
      }
      const t0 = realNow();
      wf.tick(T);
      wf.draw();
      S.cost.push(realNow() - t0);
    }
    return true;
  },
  /** A sweep of asks: `steps` of (dc, kSpan) every `every` ms. */
  sweep(steps: number, dc: number, k: number, every: number) {
    for (let i = 0; i < steps; i++) { S.ask(S.view.centre + dc, S.view.span * k); S.run(every); }
    return true;
  },
  resize(w: number, h: number) {
    const cv = document.getElementById('wf') as HTMLCanvasElement;
    cv.style.width = w + 'px'; cv.style.height = h + 'px';
    S.wf.resize();
    return true;
  },
  /** The visible picture: per screen row, its pixels' palette brightness (r·2+g+b) — and the row's geometry on screen. */
  picture() {
    const wf = S.wf as any;
    const cv = document.getElementById('wf') as HTMLCanvasElement;
    const W = cv.width, H = cv.height;
    const d = (cv.getContext('2d') as CanvasRenderingContext2D).getImageData(0, 0, W, H).data;
    // brightness r·2+g+b per pixel, sent as base64 of a Uint16Array (a Retina picture is 2.4 M of them)
    const all = new Uint16Array(W * H);
    for (let i = 0; i < W * H; i++) { const p = i * 4; all[i] = d[p] * 2 + d[p + 1] + d[p + 2]; }
    const u8 = new Uint8Array(all.buffer); let bin = '';
    for (let i = 0; i < u8.length; i += 0x8000) bin += String.fromCharCode.apply(null, Array.from(u8.subarray(i, i + 0x8000)));
    S.lum = btoa(bin); const lum = S.lum.length;   // fetched in pieces by the test (a CDP reply has a size limit)
    // the screen geometry of display row y: ring row (head + y), its map u = a·x + b, its drawn view
    const rows = wf.xfA.length, geo: ([number, number] | null)[] = [];
    const n = S.rows.length;
    for (let y = 0; y < H; y++) {
      const rec = S.rows[n - 1 - y];
      if (!rec) { geo.push(null); continue; }
      const r = (wf.head + y) % rows;
      const a = wf.xfA[r], b = wf.xfB[r];
      // screen x (0..1) shows Hz = rec.lo + rec.span·(a·x + b)
      // + where the row's own stored data is on screen (x0..x1, fractions) — outside it there is no data (a jump's edge)
      geo.push([rec.lo + rec.span * b, rec.span * a, rec.dLo, rec.dHi, -b / a, (1 - b) / a] as any);
    }
    return { W, H, lum, geo, carriers: CARRIERS, black: (() => { const lut = (wf.lut as Uint8Array); return lut[0] * 2 + lut[1] + lut[2]; })() };
  },
  png() { return (document.getElementById('wf') as HTMLCanvasElement).toDataURL('image/png'); },
  costStats() { const c = S.cost.slice().sort((a: number, b: number) => a - b); const m = c.reduce((s: number, v: number) => s + v, 0) / Math.max(1, c.length);
    return { n: c.length, mean: m, p50: c[c.length >> 1], p95: c[Math.floor(c.length * 0.95)], max: c[c.length - 1] }; },
  resetCost() { S.cost = []; return true; },
};

/** ★ CPU: the same Waterfall on the REAL clock and requestAnimationFrame for `ms` — `pattern` steady (nothing moves), zoom
 *  (×2 in and out every 700 ms) or tune (a 500 Hz step every 60 ms, back and forth). Records each frame's tick + draw time
 *  on the main thread. Resolves with the cost stats. (WFFAN_CPU in the test.) */
(S as any).live = (ms: number, pattern: string) => new Promise((res) => {
  (performance as any).now = realNow;
  const wf = S.wf as Waterfall;
  S.cost = [];
  const t0 = realNow();
  let nextFeed = t0, nextMove = t0, dir = 1, k = 0;
  const step = (ts: number) => {
    const now = realNow();
    if (now >= nextFeed) { nextFeed += S.feedMs; frame(S.view.centre, S.view.span, S.bins); wf.push(S.bins, S.view.centre, S.view.span); }
    if (pattern === 'zoom' && now >= nextMove) { nextMove += 700; S.ask(S.view.centre, (k++ & 1) ? S.view.span * 2 : S.view.span / 2); }
    if (pattern === 'tune' && now >= nextMove) { nextMove += 60; if (++k % 40 === 0) dir = -dir; S.ask(S.view.centre + 500 * dir, S.view.span); }
    const a = realNow();
    wf.tick(ts);
    wf.draw();
    S.cost.push(realNow() - a);
    if (now - t0 < ms) requestAnimationFrame(step); else res(S.costStats());
  };
  requestAnimationFrame(step);
});
