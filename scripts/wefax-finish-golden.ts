// wefax-finish-golden.ts — what a listener's CLIENT draws for a finished WEFAX chart, for vibeserver/test-wefax-finish.cpp
// to hold the server's port (android/app/src/main/cpp/vibe_wefax_finish.h) to, pixel for pixel (Stuart, 2026-11-23:
// "background wefax is not applying our auto align and margin or contrast tweaks like they do for users").
//
// ★ The client pipeline, exactly as web/client/src/main.ts runs it (the app's DecoderPanel is the same rules): every line
//   as it arrives — addToHist, chartAlignStep(getRows = the lines so far, the STATION's slant, wefaxFormat(dial)) — then
//   the final redraw (redrawDecAlign): shift = this chart's, slant = the one measured on it else the station's (nothing
//   saved), decAlignRow, fillLostLines, crispLine with the chart's levels (or the aligned line when there are none).
//
// Run:  node --no-warnings scripts/wefax-finish-golden.ts --list
//       node --no-warnings scripts/wefax-finish-golden.ts --case <name> <dir>   → <dir>/<name>.in and <dir>/<name>.out
// .in : "VWF1" u32 W, u32 H, f64 dial, H bytes received (0/1), W×H raw greys (a row not received = 255)
// .out: "VWF1" u8 looked, u8 hasAl, i32 shift, f64 slant (drawn), u8 via (0 - 1 border 2 strip 3 margin), u8 atEdge,
//       u8 crisp, f64 lo, f64 hi, W×H drawn greys (a row not drawn = 255)
// All little-endian. Silent but for --list.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chartAlignStep, wefaxFormat, wefaxOffset, wefaxPreset, type ChartAlignState } from '../src/utils/wefaxAlign.ts';
import { addToHist, crispLevels, crispLine, fillLostLines, newHist } from '../src/utils/wefaxCrisp.ts';
import { readPngGrey, toWidth } from './wefax-world/png.ts';
import { synthetic } from './wefax-world/synthetic.ts';

const W = 1809;
const HERE = dirname(fileURLToPath(import.meta.url));
type Rows = (Uint8Array | undefined)[];

// ── the client, line by line, then its final redraw ──
export function clientFinish(rx: Rows, w: number, dial: number) {
  const st: ChartAlignState = {};
  const hist = newHist();
  const station = wefaxPreset(dial), fmt = wefaxFormat(dial);
  const raw: Rows = [];
  for (let y = 0; y < rx.length; y++) {
    const r = rx[y];
    if (!r) continue;                       // never received: a hole, as decLiveRaw keeps it
    raw[y] = r;
    addToHist(hist, r);
    chartAlignStep(st, () => raw, w, station.slant, r, fmt);
  }
  raw.length = rx.length;
  const a = { shift: st.al?.shift ?? 0, slant: st.slant !== undefined ? st.slant : station.slant };
  const al: Rows = [];
  for (let y = 0; y < raw.length; y++) {
    const r = raw[y];
    if (!r) continue;
    const off = wefaxOffset(a, y, w);
    if (!off) { al[y] = r; continue; }
    const o = new Uint8Array(w);
    for (let x = 0; x < w; x++) o[x] = r[(x + off) % w] ?? 0;
    al[y] = o;
  }
  fillLostLines((j) => !!raw[j], (y) => { al[y] = al[y - 1]; }, 0, raw.length - 1);
  const lv = crispLevels(hist);
  const last = raw.length - 1;
  const row = (j: number) => (j >= 0 && j <= last ? al[j] : undefined);
  const out: Uint8Array[] = [];
  for (let y = 0; y < raw.length; y++) {
    const o = new Uint8Array(w).fill(255);
    if (al[y]) { if (lv) crispLine(row, y, w, lv, o); else o.set(al[y]!.subarray(0, w)); }
    out.push(o);
  }
  const via = st.via === 'border' ? 1 : st.via === 'strip' ? 2 : st.via === 'margin' ? 3 : 0;
  return { looked: st.al !== undefined, hasAl: !!st.al, shift: a.shift, slant: a.slant, via, atEdge: !!st.atEdge, lv, out };
}

// ── the inputs ──
let seed = 1;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
const gauss = () => { let s = 0; for (let i = 0; i < 6; i++) s += rnd(); return (s - 3) / Math.sqrt(0.5); };
/** test_wefax_align's Northwood-style chart: speckled page, curved isobars, a 7-px margin drifting with `slant`. */
function chart(lines: number, margin: number | null, slant: number): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W);
    for (let x = 0; x < W; x++) r[x] = 200 + Math.floor(rnd() * 55) - (rnd() < 0.08 ? 150 : 0);
    for (const c of [300, 700, 1200]) { const cx = Math.round(c + 80 * Math.sin((y + c) / 90)); for (let k = -1; k <= 1; k++) r[(cx + k + W) % W] = 20; }
    if (margin !== null) { const m = Math.round(margin + slant * y); for (let k = -3; k <= 3; k++) r[((m + k) % W + W) % W] = 0; }
    rows.push(r);
  }
  return rows;
}
/** test_wefax_align's DDK-style chart: a framed map on a white border, rolled `roll` px. */
function bordered(lines: number, roll: number): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W).fill(252);
    for (let x = 55; x <= 1745; x++) r[x] = 215 + Math.floor(rnd() * 40) - (rnd() < 0.08 ? 150 : 0);
    r[55] = r[56] = r[1744] = r[1745] = 10;
    for (const c of [500, 1100]) { const cx = Math.round(c - 0.17 * y); r[cx] = r[cx + 1] = 20; }
    const o = new Uint8Array(W);
    for (let x = 0; x < W; x++) o[x] = r[(x + roll) % W];
    rows.push(o);
  }
  return rows;
}
/** The world harness's reception: true rows [from…) rolled by roll + k·y, plus noise σ. */
function receive(rows: Uint8Array[], from: number, roll: number, k: number, sigma: number): Uint8Array[] {
  const out: Uint8Array[] = [];
  for (let y = 0; y + from < rows.length; y++) {
    const r = rows[y + from], o = new Uint8Array(W), off = Math.round(roll + k * y);
    for (let x = 0; x < W; x++) {
      const v = r[(((x - off) % W) + W) % W] + (sigma ? gauss() * sigma : 0);
      o[x] = v < 0 ? 0 : v > 255 ? 255 : Math.round(v);
    }
    out.push(o);
  }
  return out;
}
/** A grey-speckle chart (paper ~138 ± 60, as measured on Northwood 4610 before crisp) with lines and shading. */
function speckle(lines: number): Uint8Array[] {
  const rows: Uint8Array[] = [];
  for (let y = 0; y < lines; y++) {
    const r = new Uint8Array(W);
    for (let x = 0; x < W; x++) {
      let v = 138 + (rnd() - 0.5) * 120;
      if (x % 40 >= 20 && x % 40 < 24) v = 5;
      if (x >= 100 && x < 160) v = 90 + (rnd() - 0.5) * 60;
      r[x] = Math.max(0, Math.min(255, Math.round(v)));
    }
    rows.push(r);
  }
  return rows;
}
const pad = (rows: Uint8Array[], fill: number) => {
  const pic = toWidth({ width: rows[0].length, height: rows.length, rows }, 1728).rows;
  return pic.map((r) => { const o = new Uint8Array(W).fill(fill); o.set(r); return o; });
};

interface Entry { file?: string; synth?: string; call: string; dialHz: number; style: string[]; txSlant?: number; truth?: { shift: number; slant: number } }
const corpus = (JSON.parse(readFileSync(join(HERE, 'wefax-world', 'corpus', 'corpus.json'), 'utf8')) as Entry[]);
const synth = (JSON.parse(readFileSync(join(HERE, 'wefax-world', 'synthetic.json'), 'utf8')) as Entry[]);
const loadEntry = (e: Entry): Uint8Array[] => e.synth ? synthetic(e.synth, W).rows
  : toWidth(readPngGrey(join(HERE, 'wefax-world', 'corpus', e.file!)), W).rows;
const tag = (e: Entry) => (e.file ? e.file.replace(/^.*\//, '').replace(/\.png$/, '') : e.synth!) + (e.dialHz ? '@' + e.dialHz : '');

/** Every case: name → (rows as received, dial). Deterministic: each case seeds the generator from its own name. */
const cases: Record<string, () => { rows: Rows; dial: number }> = {
  'gya-margin42': () => ({ rows: chart(1291, 42, -0.06), dial: 4608100 }),
  'gya-margin400-slant09': () => ({ rows: chart(1291, 400, -0.09), dial: 4608100 }),
  'gya-margin-noisy': () => ({ rows: receive(chart(1291, 370, -0.06), 0, 0, 0, 40), dial: 4608100 }),
  'ddk-phased': () => ({ rows: bordered(1291, 0), dial: 7878100 }),
  'ddk-late900': () => ({ rows: bordered(1291, 900), dial: 7878100 }),
  'ddk-tone-then-late': () => {
    const tone = Array.from({ length: 270 }, () => { const r = new Uint8Array(W); for (let x = 0; x < W; x++) r[x] = 100 + Math.floor(rnd() * 20); return r; });
    return { rows: [...tone, ...bordered(1000, 900)], dial: 7878100 };
  },
  'speckle-grey': () => ({ rows: speckle(800), dial: 0 }),
  'flat-page': () => ({ rows: Array.from({ length: 400 }, () => new Uint8Array(W).fill(200)), dial: 0 }),
  'lost-lines': () => {
    const rows: Rows = chart(1291, 42, -0.06);
    rows[10] = undefined;                                       // one lost line
    for (let y = 500; y < 505; y++) rows[y] = undefined;        // a run of 5 (filled)
    for (let y = 900; y < 912; y++) rows[y] = undefined;        // an outage of 12 (left)
    for (let y = 0; y < 3; y++) rows[y] = undefined;            // the top (nothing above)
    return { rows, dial: 4608100 };
  },
  'hll2-strip-late': () => {
    const e = corpus[0];
    return { rows: receive(pad(loadEntry(e), 8), 300, 700, 0.032, 30), dial: 3583100 };
  },
  'short-chart-ended': () => ({ rows: [...chart(200, 42, -0.06), ...Array.from({ length: 60 }, () => new Uint8Array(W).fill(250))], dial: 4608100 }),
};
for (const e of [...corpus, ...synth]) {
  const k = e.txSlant ?? 0;
  cases[`${tag(e)}-phased-s30`] = () => ({ rows: receive(loadEntry(e), 0, 0, k, 30), dial: e.dialHz });
  cases[`${tag(e)}-late900`] = () => ({ rows: receive(loadEntry(e), Math.max(0, loadEntry(e).length - 900), 611, k, 0), dial: e.dialHz });
  if (e.file) {
    cases[`${tag(e)}-black5-late700-s30`] = () => { const r = pad(loadEntry(e), 8); return { rows: receive(r, Math.max(0, r.length - 700), 420, k + 0.02, 30), dial: e.dialHz }; };
    cases[`${tag(e)}-blank5-slant`] = () => ({ rows: receive(pad(loadEntry(e), 250), 0, 0, k + 0.02, 0), dial: e.dialHz });
  }
}

function writeCase(name: string, dir: string) {
  seed = [...name].reduce((a, c) => (a * 31 + c.charCodeAt(0)) & 0x7fffffff, 11) || 1;
  const { rows, dial } = cases[name]();
  const H = rows.length;
  const inb = Buffer.alloc(4 + 4 + 4 + 8 + H + W * H);
  let p = 0;
  inb.write('VWF1', 0, 'latin1'); p = 4;
  inb.writeUInt32LE(W, p); p += 4; inb.writeUInt32LE(H, p); p += 4; inb.writeDoubleLE(dial, p); p += 8;
  for (let y = 0; y < H; y++) inb[p++] = rows[y] ? 1 : 0;
  for (let y = 0; y < H; y++) { const r = rows[y]; if (r) inb.set(r, p); else inb.fill(255, p, p + W); p += W; }
  writeFileSync(join(dir, name + '.in'), inb);
  const f = clientFinish(rows, W, dial);
  const ob = Buffer.alloc(4 + 1 + 1 + 4 + 8 + 1 + 1 + 1 + 8 + 8 + W * H);
  p = 0; ob.write('VWF1', 0, 'latin1'); p = 4;
  ob[p++] = f.looked ? 1 : 0; ob[p++] = f.hasAl ? 1 : 0; ob.writeInt32LE(f.shift, p); p += 4; ob.writeDoubleLE(f.slant, p); p += 8;
  ob[p++] = f.via; ob[p++] = f.atEdge ? 1 : 0; ob[p++] = f.lv ? 1 : 0;
  ob.writeDoubleLE(f.lv ? f.lv[0] : 0, p); p += 8; ob.writeDoubleLE(f.lv ? f.lv[1] : 0, p); p += 8;
  for (const r of f.out) { ob.set(r, p); p += W; }
  writeFileSync(join(dir, name + '.out'), ob);
}

const args = process.argv.slice(2);
if (args[0] === '--list') console.log(Object.keys(cases).join('\n'));
else if (args[0] === '--case' && args[1] && args[2] && cases[args[1]]) writeCase(args[1], args[2]);
else { console.error('usage: --list | --case <name> <dir>'); process.exit(2); }
