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

/** ★ V12 a full-capture row as a VibeServer sends it (SPWB v2 — android/app/src/main/cpp/vibe_wide_row.h encode(), in
 *  TS: the test only needs real-shaped frames, not the server's exact bytes). `db` = the whole capture, low → high. */
function encodeWide(db: Float32Array, centreHz: number, spanHz: number): Uint8Array {
  const bins = db.length, F = 128, C = 1024, MAXP = 110, PEAK_DB = 6, PEAK_SIG = 3.5, WIDTH_DB = 12;
  const toByte = (v: number) => Math.max(1, Math.min(255, Math.round(v + 256)));
  const fl = new Float64Array(F), sg = new Float64Array(F);
  for (let f = 0; f < F; f++) {
    const lo = Math.floor(f * bins / F), hi = Math.floor((f + 1) * bins / F), step = Math.max(1, Math.floor((hi - lo) / 64));
    const t: number[] = []; for (let i = lo; i < hi; i += step) t.push(db[i]);
    t.sort((a, b) => a - b);
    fl[f] = t[t.length >> 1]; sg[f] = Math.max(0, fl[f] - t[Math.floor(t.length * 16 / 100)]);
  }
  const sigAt = (bin: number) => sg[Math.max(0, Math.min(F - 1, Math.floor(bin / bins * F)))];
  const floorAtBin = (bin: number) => { const x = bin / bins * F - 0.5; let i = Math.floor(x), t = x - i;
    if (i < 0) { i = 0; t = 0; } if (i >= F - 1) { i = F - 2; t = 1; } return fl[i] + (fl[i + 1] - fl[i]) * t; };
  let pk: { cell: number; ex: number; lvl: number; shape: number }[] = [];
  const per = bins / C;
  for (let c = 0; c < C; c++) {
    const lo = c * per; let bi = lo, mx = db[lo];
    for (let i = lo + 1; i < lo + per; i++) if (db[i] > mx) { mx = db[i]; bi = i; }
    const f0 = floorAtBin(bi + 0.5), ex = mx - f0, need = Math.max(PEAK_DB, PEAK_SIG * sigAt(bi + 0.5));
    if (ex < need) continue;
    const cut = Math.max(f0 + need, mx - WIDTH_DB);
    let n = 0, first = -1, last = -1, sum = 0;
    for (let i = lo; i < lo + per; i++) if (db[i] >= cut) { n++; sum += db[i]; if (first < 0) first = i; last = i; }
    const centre = (first + last + 1) * 0.5 - lo;
    const off = Math.max(0, Math.min(15, Math.floor(centre / per * 16))), w16 = Math.max(1, Math.min(16, Math.round(n / per * 16)));
    pk.push({ cell: c, ex, lvl: toByte(n <= 2 ? mx : 0.5 * (mx + sum / n)), shape: off | ((w16 - 1) << 4) });
  }
  if (pk.length > MAXP) pk = pk.sort((a, b) => b.ex - a.ex).slice(0, MAXP).sort((a, b) => a.cell - b.cell);
  const trip: number[] = []; let prev = -1;
  for (const p of pk) { let d = p.cell - prev; while (d > 255) { trip.push(255, 0, 0); d -= 255; } trip.push(d, p.lvl, p.shape); prev = p.cell; }
  const out = Buffer.alloc(32 + 2 * F + trip.length);
  out.write('SPWB', 0, 'latin1'); out[4] = 2; out[5] = 0;
  out.writeBigUInt64LE(0n, 6); out.writeBigUInt64LE(BigInt(Math.round(centreHz)), 14); out.writeUInt32LE(Math.round(spanHz), 22);
  out.writeUInt16LE(F, 26); out.writeUInt16LE(C, 28); out.writeUInt16LE(trip.length / 3, 30);
  for (let f = 0; f < F; f++) { out[32 + f] = toByte(fl[f]); out[32 + F + f] = Math.min(255, Math.round(sg[f] * 10)); }
  for (let i = 0; i < trip.length; i++) out[32 + 2 * F + i] = trip[i];
  return new Uint8Array(out);
}
/** The whole capture for a wide row: the band, plus carriers only the capture sees (LF and 2.25 MHz — the last one more
 *  than 255 cells past the band's top station, so the frame carries an escape triple). */
function wideDb(centre: number, span: number): Float32Array {
  const n = 8192, v = bandDb(centre, span, n), hz = span / n, lo = centre - span / 2;
  for (const [f, lvl] of [[60_000, -60], [198_000, -55], [2_250_000, -58]]) {
    const c = Math.round((f - lo) / hz);
    for (let k = -1; k <= 1; k++) if (c + k >= 0 && c + k < n) v[c + k] = Math.max(v[c + k], lvl - (k === 0 ? 0 : 6));
  }
  return v;
}

type Ev = { t: number; k: 'frame'; c: number; span: number; n: number } | { t: number; k: 'view'; c: number; span: number }
        | { t: number; k: 'touch' } | { t: number; k: 'tick' } | { t: number; k: 'wide'; c: number; span: number };
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
    // ★ V12 the full-capture layer row written for this slot (only once a layer exists)
    const L = p.fan.layer;
    if (L) {
      const np = L.pkN[slot], po = slot * L.P;
      out.u8(76 /*L*/); out.f64(L.k[slot]); out.f64(L.m[slot]); out.f64(L.fadeTx); out.i32(L.F); out.bytes(L.floor.subarray(slot * L.F, slot * L.F + L.F));
      out.i32(np); for (let i = 0; i < np; i++) { out.i32(L.pkCell[po + i]); out.u8(L.pkIdx[po + i]); out.u8(L.pkShape[po + i]); }
    }
  };
  let lastTick = 0, rows = 0, ticks = 0;
  for (const e of evs) {
    if (e.k === 'frame') {
      const db = bandDb(e.c, e.span, e.n), wire = toWire(db);
      inp.u8(1); inp.f64(e.t); inp.f64(e.c); inp.f64(e.span); inp.i32(e.n); inp.bytes(wire);
      P.pushFrameU8(wire, true, e.c, e.span, e.t);
    } else if (e.k === 'view') {
      inp.u8(2); inp.f64(e.t); inp.f64(e.c); inp.f64(e.span); P.setView(e.c, e.span);
    } else if (e.k === 'wide') {
      const fr = encodeWide(wideDb(e.c, e.span), e.c, e.span);
      inp.u8(5); inp.f64(e.t); inp.i32(fr.length); inp.bytes(fr);
      const ok = P.pushWide(fr, e.t), w = P.wideSt;
      out.u8(87 /*W*/); out.u8(ok ? 1 : 0); out.f64(w ? w.off : NaN); out.f64(w ? w.gap : NaN); out.f64(P.detP50);
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
  const nw = evs.filter(e => e.k === 'wide').length;
  console.log(`  ${name}: ${evs.filter(e => e.k === 'frame').length} frames${nw ? `, ${nw} SPWB rows` : ''}, ${P.frameCount} rows placed, ${ticks} display frames`);
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

// 6. ★ V12 THE FULL-CAPTURE LAYER — WIDE GLIDE: a 150 kHz view on a 2.4 MHz capture; a finger drags 700 → 1000 kHz (TWO
//    view widths) over 5 s with SPWB rows after every 4th frame; the capture recentres mid-way (a new picture, no time
//    blend); after a rest the SPWB rows stop, and a second glide 1000 → 850 kHz runs past the 4 s staleness (its last
//    eased rows get no layer: the edges continue the row below again).
{
  const evs: Ev[] = [], span = 150_000, n = 2048, end = T0 + 18000;
  const a1 = T0 + 3000, b1 = T0 + 8000, a2 = T0 + 13000, b2 = T0 + 17000;
  const glide = (t: number, a: number, b: number, f0: number, f1: number) => f0 + (f1 - f0) * (0.5 - 0.5 * Math.cos(Math.PI * (t - a) / (b - a)));
  const viewAt = (t: number) => t < a1 ? 700_000 : t <= b1 ? glide(t, a1, b1, 700_000, 1_000_000) : t < a2 ? 1_000_000
                              : t <= b2 ? glide(t, a2, b2, 1_000_000, 850_000) : 850_000;
  const capAt = (t: number) => t < T0 + 6500 ? 1_200_000 : 1_000_000;
  ticksTo(evs, end);
  evs.push({ t: T0 - 1, k: 'view', c: 700_000, span });
  for (const [a, b] of [[a1, b1], [a2, b2]]) for (let t = a; t <= b + TICK; t += TICK) { evs.push({ t, k: 'view', c: Math.round(viewAt(t)), span }); evs.push({ t, k: 'touch' }); }
  seed = 606;
  let k = 0;
  for (let t = T0 + 5; t < end; t += 100) {
    const tj = t + (rnd() - 0.5) * 16;
    evs.push({ t: tj, k: 'frame', c: Math.round(viewAt(tj - 150)), span, n });
    if (++k % 4 === 0 && tj < T0 + 11000) evs.push({ t: tj + 1, k: 'wide', c: capAt(tj), span: 2_400_000 });
  }
  run('wide_glide', { scroll: 'default' }, evs);
}
// 7. ★ V12 WIDE ZOOM: ×√2 keys in ×3 then out ×3 on the same capture — a zoom-in's eased rows are drawn WIDER than their
//    data (a > 1, the shrunk path): their edges come from the layer, with the soft edge where the row's edge is on screen.
{
  const evs: Ev[] = [], c = 700_000, n = 2048, end = T0 + 12000;
  ticksTo(evs, end);
  const presses = [3000, 4000, 5000, 7500, 8500, 9500].map((x) => T0 + x);
  const spanAt = (t: number) => { let s = 150_000; for (let i = 0; i < presses.length; i++) if (t >= presses[i]) s = i < 3 ? s / Math.SQRT2 : s * Math.SQRT2; return s; };
  evs.push({ t: T0 - 1, k: 'view', c, span: 150_000 });
  for (const p of presses) { evs.push({ t: p, k: 'view', c, span: spanAt(p) }); evs.push({ t: p, k: 'touch' }); }
  seed = 707;
  let k = 0;
  for (let t = T0 + 5; t < end; t += 100) {
    const tj = t + (rnd() - 0.5) * 16;
    evs.push({ t: tj, k: 'frame', c, span: spanAt(tj - 120), n });
    if (++k % 4 === 0) evs.push({ t: tj + 1, k: 'wide', c: 1_200_000, span: 2_400_000 });
  }
  run('wide_zoom', { scroll: 'default' }, evs);
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
