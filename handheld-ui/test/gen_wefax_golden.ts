/**
 * GOLDEN DATA for the handheld's WEFAX port (handheld-ui/src/dec_pic_align.h): the APP's own src/utils/wefaxAlign.ts
 * and wefaxCrisp.ts, unmodified, run line by line (chartAlignStep, as both clients call it) over the WEFAX world corpus
 * (scripts/wefax-world: NOAA's charts in three line geometries + the synthetic formats) — phased, joined late and
 * rolled with noise, and leaning. wefax_golden_test.cpp replays the same received rows through the C++ and must agree
 * on every state change, and on the crisp rendering's bytes.
 *   <out>.rows.gz — per scenario: u32 nameLen, name, f64 dialHz, u32 W, u32 n, then n rows of W bytes (gzip)
 *   <out>.txt     — per scenario: "S|<name>", then "E|y|ret|<state>" on every change, "C|<hash>" (crisp of rows 0..79)
 * Run: node --no-warnings --import ./ts_resolve.mjs --import ./ts_transform.mjs gen_wefax_golden.ts <outPrefix> [--quick]
 */
import { readFileSync, writeFileSync, existsSync, createWriteStream } from 'node:fs';
import { createGzip } from 'node:zlib';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chartAlignStep, wefaxFormat, wefaxPreset, type ChartAlignState } from '../../src/utils/wefaxAlign.ts';
import { addToHist, crispLevels, crispLine, newHist } from '../../src/utils/wefaxCrisp.ts';
import { readPngGrey, toWidth, type Grey } from '../../scripts/wefax-world/png.ts';
import { synthetic } from '../../scripts/wefax-world/synthetic.ts';

const W = 1809;
const HERE = dirname(fileURLToPath(import.meta.url));
const WORLD = join(HERE, '../../scripts/wefax-world');
const QUICK = process.argv.includes('--quick');
interface Entry { file?: string; synth?: string; station: string; call: string; dialHz: number; style: string[]; truth?: { shift: number; slant: number }; txSlant?: number }

let seed = 12345;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
const gauss = () => { let s = 0; for (let i = 0; i < 6; i++) s += rnd(); return (s - 3) / Math.sqrt(0.5); };
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
function load(path: string): Array<{ e: Entry; name: string; rows: Uint8Array[] }> {
  if (!existsSync(path)) return [];
  const out: Array<{ e: Entry; name: string; rows: Uint8Array[] }> = [];
  for (const e of JSON.parse(readFileSync(path, 'utf8')) as Entry[]) {
    const img: Grey = e.synth ? synthetic(e.synth, W) : toWidth(readPngGrey(resolve(dirname(path), e.file!)), W);
    let rows = img.rows;
    if (e.truth && (e.truth.shift || e.truth.slant))
      rows = rows.map((r, y) => { const off = ((Math.round(e.truth!.shift + e.truth!.slant * y) % W) + W) % W;
        const o = new Uint8Array(W); for (let x = 0; x < W; x++) o[x] = r[(x + off) % W]; return o; });
    out.push({ e, name: `${e.call} ${e.file ?? e.synth}`, rows });
    if (e.file && !e.style.includes('off-air') && out.length % 3 === 1) {   // ★ a third of them in the other two geometries too
      const pic = toWidth({ width: W, height: rows.length, rows }, 1728);
      out.push({ e, name: `${e.call} ${e.file} [black5]`, rows: pic.rows.map((r) => { const o = new Uint8Array(W).fill(8); o.set(r); return o; }) });
      out.push({ e, name: `${e.call} ${e.file} [blank5]`, rows: pic.rows.map((r) => { const o = new Uint8Array(W).fill(250); o.set(r); return o; }) });
    }
  }
  return out;
}
const f6 = (x: number) => (Math.abs(x) < 5e-7 ? 0 : x).toFixed(6);
const sig = (st: ChartAlignState) => [st.al === undefined ? 'u' : st.al === null ? 'null' : `${f6(st.al.shift)},${f6(st.al.slant)}`,
  st.via ?? '-', st.atEdge ? 1 : 0, st.slant === undefined ? 'u' : f6(st.slant), st.n ?? 0, st.refined ? 1 : 0, st.retried ? 1 : 0, st.flat ?? 0].join('|');

const charts = [...load(join(WORLD, 'corpus', 'corpus.json')), ...load(join(WORLD, 'synthetic.json'))];
const out = process.argv[2];
const gz = createGzip({ level: 1 });
const done = new Promise<void>((ok) => gz.pipe(createWriteStream(out + '.rows.gz')).on('finish', () => ok()));
const bins = { push: (b: Buffer) => { gz.write(Buffer.from(b)); } };
const txt: string[] = [];
let nScen = 0;
for (const c of charts) {
  const station = wefaxPreset(c.e.dialHz).slant, fmt = wefaxFormat(c.e.dialHz), k0 = c.e.txSlant ?? 0;
  const cases: Array<[string, number, number, number, number]> = [['phased', 0, 0, k0, 0]];
  cases.push(['late700', Math.max(0, c.rows.length - 700), 50 + Math.floor(rnd() * (W - 100)), k0, 30]);
  if (!QUICK) { cases.push(['slant+0.02', 0, 0, k0 + 0.02, 30]); cases.push(['late400', Math.max(0, c.rows.length - 400), 50 + Math.floor(rnd() * (W - 100)), k0, 0]); }
  for (const [label, from, roll, k, sigma] of cases) {
    const rx = receive(c.rows, from, roll, k, sigma);
    const name = `${c.name} ${label} σ${sigma}`;
    const nb = Buffer.from(name, 'utf8');
    const h = Buffer.alloc(4 + nb.length + 8 + 8);
    h.writeUInt32LE(nb.length, 0); nb.copy(h, 4); h.writeDoubleLE(c.e.dialHz, 4 + nb.length);
    h.writeUInt32LE(W, 12 + nb.length); h.writeUInt32LE(rx.length, 16 + nb.length);
    bins.push(h);
    for (const r of rx) bins.push(Buffer.from(r.buffer, r.byteOffset, r.byteLength));
    txt.push(`S|${name}`);
    const st: ChartAlignState = {};
    let last = '';
    for (let y = 0; y < rx.length; y++) {
      const ret = chartAlignStep(st, () => rx.slice(0, y + 1), W, station, rx[y], fmt);
      const s = sig(st);
      // ★ every return of true, and every change but the per-line counters (n, flat)
      const core = s.split('|').filter((_, i) => i !== 4 && i !== 7).join('|');
      if (ret || core !== last) txt.push(`E|${y}|${ret ? 1 : 0}|${s}`);
      last = core;
    }
    // the crisp rendering of the first 80 rows, as received
    const hist = newHist();
    const top = rx.slice(0, 80);
    for (const r of top) addToHist(hist, r);
    const lv = crispLevels(hist);
    let hsh = 2166136261;
    if (lv) {
      const o = new Uint8Array(W);
      for (let y = 0; y < top.length; y++) { crispLine((j) => top[j], y, W, lv, o); for (let x = 0; x < W; x++) hsh = Math.imul(hsh ^ o[x], 16777619) >>> 0; }
    }
    txt.push(`C|${lv ? `${f6(lv[0])},${f6(lv[1])}` : 'none'}|${hsh}`);
    nScen++;
  }
}
gz.end();
await done;
writeFileSync(out + '.txt', txt.join('\n') + '\n');
console.log(`gen_wefax_golden: ${charts.length} charts, ${nScen} scenarios`);
