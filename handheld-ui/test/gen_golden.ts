/**
 * GOLDEN DATA for the handheld waterfall port: synthetic scenarios run through the APP's TS modules (ref_pipeline.ts),
 * written as <name>.in.bin (the events: frames as the engine sends them — u8 dBFS+256 in FFT order — view changes,
 * interactions, display ticks) and <name>.out.bin (what the app's code produced: every processed row, every ring row
 * placed with its motion texels and fan map, every display frame's uniforms and spectrum trace). The C++ test
 * (wf_golden_test.cpp) replays the .in and must reproduce the .out.
 * Also palettes.bin: every built-in LUT + the stops/positioned palettes' inputs, for the C++ lutStops/lutPos ports.
 * Run: node --no-warnings --import ./ts_resolve.mjs gen_golden.ts <outDir>
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { RefPipeline, RING_W, RING_ROWS, type RefCfg } from './ref_pipeline.ts';
import { COLORMAP_NAMES, getColorLUT } from '../../src/assets/colormapUtils.ts';
import { COLORMAPS_256, COLORMAPS_STOPS, COLORMAPS_POS } from '../../src/assets/colormaps.ts';
import type { WfScroll } from '../../src/assets/wfRowTarget.ts';

const outDir = process.argv[2] ?? 'golden';
mkdirSync(outDir, { recursive: true });

class Bin {
  private chunks: Buffer[] = [];
  u8(v: number) { const b = Buffer.alloc(1); b.writeUInt8(v); this.chunks.push(b); }
  i32(v: number) { const b = Buffer.alloc(4); b.writeInt32LE(v); this.chunks.push(b); }
  f64(v: number) { const b = Buffer.alloc(8); b.writeDoubleLE(v); this.chunks.push(b); }
  bytes(a: Uint8Array) { this.chunks.push(Buffer.from(a)); }   // (a COPY: the rows are reused buffers)
  f32s(a: ArrayLike<number>) { const b = Buffer.alloc(4 * a.length); for (let i = 0; i < a.length; i++) b.writeFloatLE(a[i], 4 * i); this.chunks.push(b); }
  buf() { return Buffer.concat(this.chunks); }
}

// ── the band: AM stations every 9 kHz, each its own level and audio; a speckled floor (dBFS) ──
let seed = 7;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
const stations: { f: number; lvl: number }[] = [];
for (let f = 531_000; f <= 1_602_000; f += 9_000) if (rnd() < 0.55) stations.push({ f, lvl: -95 + rnd() * 45 });
function bandDb(centre: number, span: number, n: number): Float32Array {
  const v = new Float32Array(n), hz = span / n, lo = centre - span / 2;
  for (let i = 0; i < n; i++) v[i] = -112 + rnd() * 8;
  for (const s of stations) {
    const c = (s.f - lo) / hz;
    if (c < -n * 0.2 || c > n * 1.2) continue;
    const half = 4_500 / hz;
    for (let i = Math.max(0, Math.floor(c - half)); i <= Math.min(n - 1, Math.ceil(c + half)); i++) {
      const d = Math.abs(i - c) / half;
      v[i] = Math.max(v[i], s.lvl - 14 - 10 * d + rnd() * 5);
    }
    const ci = Math.round(c);
    for (let k = -1; k <= 1; k++) if (ci + k >= 0 && ci + k < n) v[ci + k] = Math.max(v[ci + k], s.lvl - (k === 0 ? 0 : 4));
  }
  return v;
}
/** The engine's wire form: u8 = dBFS + 256 (0 = no data), FFT order (DC first). */
function toWire(db: Float32Array): Uint8Array {
  const n = db.length, h = n >> 1, out = new Uint8Array(n);
  for (let i = 0; i < n; i++) out[(i + h) % n] = Math.max(1, Math.min(255, Math.round(db[i] + 256)));
  return out;
}

type Ev = { t: number; k: 'frame'; c: number; span: number; n: number } | { t: number; k: 'view'; c: number; span: number }
        | { t: number; k: 'touch' } | { t: number; k: 'tick' };
const SCROLL_CODE: Record<WfScroll, number> = { min2: 0, low: 1, sharp: 2, eco: 3, default: 4, smooth: 5 };
const T0 = 1000, TICK = 1000 / 60;

function run(name: string, cfg: Partial<RefCfg>, evs: Ev[]) {
  evs.sort((a, b) => a.t - b.t || (a.k === 'tick' ? 1 : 0) - (b.k === 'tick' ? 1 : 0));
  const inp = new Bin(), out = new Bin();
  const scroll = cfg.scroll ?? 'default';
  inp.bytes(Buffer.from('WFIN')); inp.u8(SCROLL_CODE[scroll]); inp.f64(cfg.displayHz ?? 60);
  const P = new RefPipeline(cfg);
  P.onFrame = (f) => { out.u8(70 /*F*/); out.i32(f.row.length); out.bytes(f.row); out.f64(f.dbMin); out.f64(f.dbMax); };
  P.onRow = (p, slot) => {
    const prev = (slot + RING_ROWS - 1) % RING_ROWS, b = p.ms.bytes, n = RING_W;
    out.u8(82 /*R*/); out.i32(slot); out.f64(p.frameCount); out.f64(p.fan.xfA[slot]); out.f64(p.fan.xfB[slot]); out.f64(p.fan.easeLeft);
    out.f64(p.ms.lo); out.f64(p.ms.hi);
    out.bytes(new Uint8Array([b[slot], b[n + slot], b[2 * n + slot], b[3 * n + slot], b[prev], b[n + prev], b[2 * n + prev], b[3 * n + prev]]));
    out.bytes(p.image().subarray(slot * n, slot * n + n));
  };
  let lastTick = 0, rows = 0, ticks = 0;
  for (const e of evs) {
    if (e.k === 'frame') {
      const db = bandDb(e.c, e.span, e.n), wire = toWire(db);
      inp.u8(1); inp.f64(e.t); inp.f64(e.c); inp.f64(e.span); inp.i32(e.n); inp.bytes(wire);
      P.pushFrameU8(wire, true, e.c, e.span, e.t);
    } else if (e.k === 'view') {
      inp.u8(2); inp.f64(e.t); inp.f64(e.c); inp.f64(e.span); P.setView(e.c, e.span);
    } else if (e.k === 'touch') {
      inp.u8(3); inp.f64(e.t); P.noteInteraction(e.t);
    } else {
      const gap = lastTick > 0 ? e.t - lastTick : 16; lastTick = e.t;
      inp.u8(4); inp.f64(e.t); inp.f64(gap);
      const h0 = P.uHead;
      P.tick(e.t, gap); ticks++; if (P.uHead !== h0) rows++;
      const u = P.uniforms();
      out.u8(68 /*D*/); for (const v of [u.headF, u.frac, u.quant, u.moLo, u.moHi, u.zA, u.zB, P.rc.rate, P.pTgt.value, P.jsAvail]) out.f64(v);
      out.i32(P.specDisp.length); out.f32s(P.specDisp); out.i32(P.peakShown.length); out.f32s(P.peakShown);
    }
  }
  inp.u8(0); out.u8(69 /*E*/);
  writeFileSync(join(outDir, `${name}.in.bin`), inp.buf());
  writeFileSync(join(outDir, `${name}.out.bin`), out.buf());
  console.log(`  ${name}: ${evs.filter(e => e.k === 'frame').length} frames, ${P.frameCount} rows placed, ${ticks} display frames`);
}
const ticksTo = (evs: Ev[], tEnd: number) => { for (let t = T0; t <= tEnd; t += TICK) evs.push({ t, k: 'tick' }); };

// 1. SWEEP: a finger drags 860 → 670 kHz over 7 s (VibeServer: the frames' tuner centre lags the view by 150 ms), 10 fps ±8 ms
{
  const evs: Ev[] = [], span = 150_000, n = 2048, a = T0 + 3000, b = a + 7000, end = T0 + 13000;
  const viewAt = (t: number) => t < a ? 860_000 : t > b ? 670_000 : 860_000 - 190_000 * (0.5 - 0.5 * Math.cos(Math.PI * (t - a) / (b - a)));
  ticksTo(evs, end);
  for (let t = a; t <= b + TICK; t += TICK) { evs.push({ t, k: 'view', c: Math.round(viewAt(t)), span }); evs.push({ t, k: 'touch' }); }
  evs.push({ t: T0 - 1, k: 'view', c: 860_000, span });
  seed = 101;
  for (let t = T0 + 5; t < end; t += 100) { const tj = t + (rnd() - 0.5) * 16; evs.push({ t: tj, k: 'frame', c: Math.round(viewAt(tj - 150)), span, n }); }
  run('sweep', { scroll: 'default' }, evs);
}
// 2. ZOOM: ×√2 keys — three presses in (1 s apart), three back out; the frames take the new span 120 ms after a press
{
  const evs: Ev[] = [], c = 700_000, n = 1024, end = T0 + 12000;
  ticksTo(evs, end);
  const presses = [3000, 4000, 5000, 7500, 8500, 9500].map((x) => T0 + x);
  const spanAt = (t: number) => { let s = 150_000; for (let i = 0; i < presses.length; i++) if (t >= presses[i]) s = i < 3 ? s / Math.SQRT2 : s * Math.SQRT2; return s; };
  evs.push({ t: T0 - 1, k: 'view', c, span: 150_000 });
  for (const p of presses) { evs.push({ t: p, k: 'view', c, span: spanAt(p) }); evs.push({ t: p, k: 'touch' }); }
  seed = 202;
  for (let t = T0 + 5; t < end; t += 100) { const tj = t + (rnd() - 0.5) * 16; evs.push({ t: tj, k: 'frame', c, span: spanAt(tj - 120), n }); }
  run('zoom', { scroll: 'default' }, evs);
}
// 3. JUMP: a bookmark 700 → 1200 kHz (a whole view and more): the history keeps its place, the new rows arrive
{
  const evs: Ev[] = [], span = 150_000, n = 1024, end = T0 + 8000, tj0 = T0 + 4000;
  ticksTo(evs, end);
  evs.push({ t: T0 - 1, k: 'view', c: 700_000, span });
  evs.push({ t: tj0, k: 'view', c: 1_200_000, span }); evs.push({ t: tj0, k: 'touch' });
  seed = 303;
  for (let t = T0 + 5; t < end; t += 100) { const tj = t + (rnd() - 0.5) * 16; evs.push({ t: tj, k: 'frame', c: tj - 200 >= tj0 ? 1_200_000 : 700_000, span, n }); }
  run('jump', { scroll: 'default' }, evs);
}
// 4. AN IRREGULAR 9–11 fps FEED (Stuart's UberSDR), steady, at Default 30 / Smooth 60 / Eco 10 / Native
for (const scroll of ['default', 'smooth', 'eco', 'sharp'] as WfScroll[]) {
  const evs: Ev[] = [], n = 4096, end = T0 + 12000;
  ticksTo(evs, end);
  evs.push({ t: T0 - 1, k: 'view', c: 7_100_000, span: 400_000 });
  seed = 404;
  for (let t = T0 + 5; t < end;) { evs.push({ t, k: 'frame', c: 7_100_000, span: 400_000, n }); t += 1000 / (9 + 2 * rnd()); }
  run(`irregular_${scroll}`, { scroll }, evs);
}

// 5. THE PICTURE (render_app.ts / wf_render.cpp draw it): a steady band, a drag 700 → 760 kHz, a rest, three ×√2 zoom keys
//    in, three back out, a rest — everything the hard rule covers in one history.
{
  const evs: Ev[] = [], n = 2048, end = T0 + 11000, a = T0 + 2000, b = T0 + 5000;
  const viewAt = (t: number) => t < a ? 700_000 : t > b ? 760_000 : 700_000 + 60_000 * (0.5 - 0.5 * Math.cos(Math.PI * (t - a) / (b - a)));
  const presses = [6000, 6500, 7000, 8000, 8500, 9000].map((x) => T0 + x);
  const spanAt = (t: number) => { let s = 100_000; for (let i = 0; i < presses.length; i++) if (t >= presses[i]) s = i < 3 ? s / Math.SQRT2 : s * Math.SQRT2; return s; };
  ticksTo(evs, end);
  evs.push({ t: T0 - 1, k: 'view', c: 700_000, span: 100_000 });
  for (let t = a; t <= b + TICK; t += TICK) { evs.push({ t, k: 'view', c: Math.round(viewAt(t)), span: 100_000 }); evs.push({ t, k: 'touch' }); }
  for (const p of presses) { evs.push({ t: p, k: 'view', c: 760_000, span: spanAt(p) }); evs.push({ t: p, k: 'touch' }); }
  seed = 505;
  for (let t = T0 + 5; t < end; t += 100) { const tj = t + (rnd() - 0.5) * 16; evs.push({ t: tj, k: 'frame', c: Math.round(viewAt(tj - 150)), span: spanAt(tj - 120), n }); }
  run('picture', { scroll: 'default' }, evs);
}

// ── palettes ──
{
  const b = new Bin();
  b.i32(COLORMAP_NAMES.length);
  for (const name of COLORMAP_NAMES) { b.i32(name.length); b.bytes(Buffer.from(name)); b.bytes(getColorLUT(name)); }
  // (getColorLUT looks in 256 → POS → STOPS: only the names it actually builds from stops / positions)
  const stops = Object.entries(COLORMAPS_STOPS).filter(([k]) => !(k in COLORMAPS_256) && !(k in COLORMAPS_POS));
  const pos = Object.entries(COLORMAPS_POS).filter(([k]) => !(k in COLORMAPS_256));
  b.i32(stops.length);
  for (const [name, st] of stops) { b.i32(st.length); for (const h of st) b.i32(parseInt(h.replace('#', ''), 16)); b.bytes(getColorLUT(name)); }
  b.i32(pos.length);
  for (const [name, st] of pos) { b.i32(st.length); for (const [p, h] of st) { b.i32(p); b.i32(parseInt(h.replace('#', ''), 16)); } b.bytes(getColorLUT(name)); }
  writeFileSync(join(outDir, 'palettes.bin'), b.buf());
  console.log(`  palettes: ${COLORMAP_NAMES.length} built-in, ${stops.length} stop lists, ${pos.length} positioned`);
}
