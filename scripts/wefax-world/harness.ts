// harness.ts — the WEFAX auto-align (src/utils/wefaxAlign.ts chartAlignStep) run over charts from EVERY station
// format we can get, not only DWD/DDK and Northwood.
//
// ★★★ WHY (Stuart, 2026-10-07): "we have tested against Northwood and DDK but these are very eurocentric, can you
//     research any other WEFAX providers and make sure we don't break their format, especially as this is a
//     worldwide app." See docs/WEFAX-WORLD-STATIONS.md for the station table this corpus is drawn from.
//
// Run:  node --no-warnings scripts/wefax-world/harness.ts [--quick] [--only <substring>] [--verbose]
//       WEFAX_WORLD_LOCAL=<dir>  adds a local, uncommitted corpus (<dir>/corpus.json, same schema) — charts whose
//                                licence does not let us commit them (Crown copyright, DWD, JMA …).
// ★ It imports wefaxAlign from the tree it runs in (../../src/utils/wefaxAlign.ts), so re-running it after a change
//   to the aligner measures the change. Exit status: 1 if any FAIL.
//
// ★ For every chart (stored PHASED and UPRIGHT — `truth` in the manifest says how to make it so, for an off-air one)
//   it simulates, each at noise σ 0 and σ 30:
//   (a) PHASED       — the chart as sent, from line 0 — must NOT be moved into the map;
//   (b) LATE JOIN    — only the last 400 / 700 / 900 lines, every line rolled by a random offset — must be cut
//                      where the chart has no picture (paper, margin) or left alone; never cut through the map;
//   (c) NATIVE SLANT — phased, but leaning ±0.02 px/line (a receiver's clock) — must not be "corrected" into a
//                      big wrong slant, and must not be cut into the map.
//   A station's own slant (wefaxPreset — Northwood −0.06) is added on top, as it is on air.
// ★ The verdict is judged on WHERE THE SEAM LANDS in the true chart, line by line: the drawn line y shows received
//   column x + D(y), which is true column x + D(y) − P(y) (D = drawn shift + slant·y, P = the simulated roll), so
//   the seam (drawn x = 0) sits at true column D(y) − P(y). A seam within EDGE_OK px of the true edge, or in a
//   column with no picture in it (see safeColumns), is a sensible cut.
import { readFileSync, existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chartAlignStep, wefaxFormat, wefaxPreset, type ChartAlignState } from '../../src/utils/wefaxAlign.ts';
import { readPngGrey, toWidth, type Grey } from './png.ts';
import { synthetic } from './synthetic.ts';

const W = 1809;
const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const QUICK = args.includes('--quick'), VERBOSE = args.includes('--verbose');
const ONLY = args.includes('--only') ? args[args.indexOf('--only') + 1] : '';
/** ★ A seam this close (px) to the chart's true edge is a correct phase, whatever is there. */
const EDGE_OK = 12;
/** ★ Share of drawn lines whose seam may land in the picture before the cut counts as "through the map". */
const BAD_SHARE = 0.05;
/** ★ Slant error (px/line) that is a wrong correction: 0.03 = 30 px of lean over a 1000-line chart. */
const SLANT_FAIL = 0.03, SLANT_WARN = 0.01;

export interface Entry {
  file?: string; synth?: string; station: string; call: string; dialHz: number; product?: string;
  style: string[]; source?: string; licence?: string;
  /** The correction that makes the stored image phased and upright (off-air charts), default none. */
  truth?: { shift: number; slant: number };
  /** ★ The slant the TRANSMITTER puts on its lines (px/line), as received on a perfect clock — Northwood −0.06,
   *  everyone else 0 as far as is known. Kept apart from wefaxPreset (what the CODE assumes for that dial) so a
   *  preset that fires on the wrong station shows up — KVM70's 11090 kHz is within 5 kHz of Northwood's 11086.5. */
  txSlant?: number;
}
interface Chart { e: Entry; rows: Uint8Array[]; where: string; variant: string }

/** ★ PUBLISHED IMAGE → ON-AIR LINE, TWO WAYS (2026-10-07). A NOAA product is a 1728-px scan; the 120 LPM / IOC 576
 *  line is 1809 px. Whether the 81 px left over arrive as a blank strip (the WMO phasing pulse's ~5 % of the line)
 *  or the receiver's line is filled edge to edge is not known for every station, and it decides which rule fires
 *  (81 px is under BORDER_MIN), so every published image is run both ways:
 *    'full'    — scaled to the whole line (prep-images.py), the map reaching both ends;
 *    'blank5'  — the picture in the first 1728 px, 81 px of white paper after it (SVJ4's and DDK's white margins);
 *    'black5'  — the same with 81 px of BLACK: the NWS family, JMH, HLL2 (since ~2019), XSG and JFX all show black
 *                margin strips in the picture area (docs/WEFAX-WORLD-STATIONS.md, risk 1) — the commonest on-air
 *                geometry in the world, and neither DDK's white frame nor Northwood's thin line. */
function variants(e: Entry, img: Grey): Array<{ variant: string; rows: Uint8Array[] }> {
  const out = [{ variant: 'full', rows: img.rows }];
  if (e.file && !e.style.includes('off-air')) {
    const pic = toWidth(img, 1728);
    out.push({ variant: 'blank5', rows: pic.rows.map((r) => { const o = new Uint8Array(W).fill(250); o.set(r); return o; }) });
    out.push({ variant: 'black5', rows: pic.rows.map((r) => { const o = new Uint8Array(W).fill(8); o.set(r); return o; }) });
  }
  return out;
}

function loadManifest(path: string, where: string): Chart[] {
  if (!existsSync(path)) return [];
  const list = JSON.parse(readFileSync(path, 'utf8')) as Entry[];
  const out: Chart[] = [];
  for (const e of list) {
    if (ONLY && !`${e.station} ${e.call} ${e.file ?? e.synth}`.toLowerCase().includes(ONLY.toLowerCase())) continue;
    let img: Grey;
    if (e.synth) img = synthetic(e.synth, W);
    else img = toWidth(readPngGrey(resolve(dirname(path), e.file!)), W);
    let rows = img.rows;
    if (e.truth && (e.truth.shift || e.truth.slant)) {
      rows = rows.map((r, y) => { const off = ((Math.round(e.truth!.shift + e.truth!.slant * y) % W) + W) % W;
        const o = new Uint8Array(W); for (let x = 0; x < W; x++) o[x] = r[(x + off) % W]; return o; });
    }
    for (const v of variants(e, { width: W, height: rows.length, rows })) out.push({ e, rows: v.rows, where, variant: v.variant });
  }
  return out;
}

/** ★ Columns of the TRUE chart a seam may sit in: within EDGE_OK of the edge, or a column with no picture — no ink
 *  that differs from the chart's own paper on more than 3 % of its lines (or ink on ≥ 97 %: a solid strip), inside a run of ≥ 16 such columns. The
 *  paper is the chart's commonest grey, so a black-background chart's paper is black and its picture is light. */
function safeColumns(rows: Uint8Array[]): Uint8Array {
  const hist = new Uint32Array(16);
  for (const r of rows) for (let x = 0; x < W; x += 3) hist[r[x] >> 4]++;
  let mode = 0; for (let i = 1; i < 16; i++) if (hist[i] > hist[mode]) mode = i;
  const paper = mode * 16 + 8;
  const dark = new Uint32Array(W);
  let n = 0;
  for (const r of rows) {
    // ★ A line inked right across (DDK's closing bar, a stop tone drawn black) says nothing about columns: counted, it
    //   inked every column on 5 % of a 400-line join and no column was paper any more (2026-10-07, Stuart's HF+ copies).
    let across = 0;
    for (let x = 0; x < W; x += 3) if (Math.abs(r[x] - paper) > 80) across++;
    if (across > 0.9 * W / 3) continue;
    let prev = r[W - 1];
    for (let x = 0; x < W; x++) { const v = r[x]; if (Math.abs(v - paper) > 80 && Math.abs(prev - paper) > 80) { dark[x]++; dark[(x - 1 + W) % W]++; } prev = v; }
    n++;
  }
  const blank = new Uint8Array(W);
  // ★ …or SOLID (inked on ≥ 98 % of lines): a black margin strip is as good a place for the seam as white paper
  for (let x = 0; x < W; x++) blank[x] = dark[x] <= 0.03 * n || dark[x] >= 0.97 * n ? 1 : 0;
  const safe = new Uint8Array(W);
  for (let x = 0; x < W; x++) {
    if (!blank[x]) continue;
    let l = 0, r = 0;
    while (l < 16 && blank[(x - l - 1 + W) % W]) l++;
    while (r < 16 && blank[(x + r + 1) % W]) r++;
    if (l + r + 1 >= 16) safe[x] = 1;
  }
  for (let k = -EDGE_OK; k <= EDGE_OK; k++) safe[(k + W) % W] = 1;
  return safe;
}

let seed = 12345;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
const gauss = () => { let s = 0; for (let i = 0; i < 6; i++) s += rnd(); return (s - 3) / Math.sqrt(0.5); };

/** The received lines: true rows [from, to) rolled by roll + k·y (y counted from the join), plus noise. */
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

type Verdict = 'PASS' | 'LEFT' | 'WARN' | 'FAIL';
interface Result { verdict: Verdict; why: string }

function run(c: Chart, safe: Uint8Array, kind: 'phased' | 'late' | 'slant', join: number, roll: number, extraK: number, sigma: number): Result {
  const station = wefaxPreset(c.e.dialHz).slant;          // what the code centres on for this dial
  const from = kind === 'late' ? Math.max(0, c.rows.length - join) : 0;
  // ★ Judged on the lines this listener RECEIVED: a header bar across the whole line at the top of the chart is not
  //   in a late join, and must not make the border beside the map below it count as picture.
  if (from) safe = safeColumns(c.rows.slice(from));
  const k = (c.e.txSlant ?? 0) + extraK;                     // what the lines really carry
  const rx = receive(c.rows, from, roll, k, sigma);
  const st: ChartAlignState = {};
  const fmt = wefaxFormat(c.e.dialHz);                     // the formats the code acts on for this dial
  for (let y = 0; y < rx.length; y++) chartAlignStep(st, () => rx.slice(0, y + 1), W, station, rx[y], fmt);
  // what the clients draw (DecoderImageCanvas effAlign, auto on, nothing saved): the chart's shift, its measured slant
  // or else the station's
  const D = { shift: st.al?.shift ?? 0, slant: st.slant ?? station };
  let bad = 0;
  for (let y = 0; y < rx.length; y++) {
    const s = Math.round(D.shift + D.slant * y) - Math.round(roll + k * y);
    if (!safe[((s % W) + W) % W]) bad++;
  }
  const badShare = bad / rx.length, slantErr = Math.abs(D.slant - k);
  const moved = !!st.al && st.al.shift !== 0;
  const tag = `drawn shift ${D.shift} slant ${D.slant.toFixed(3)} (sent roll ${roll} slant ${k.toFixed(3)}), seam in picture on ${(100 * badShare).toFixed(0)} % of lines, via ${st.via ?? '-'}`;
  // ★ LEFT, not FAIL, when the aligner did nothing at all (shift 0, the station's slant): the chart is drawn exactly
  //   as received, so whatever is wrong with it is the reception's, and the aligner did no harm. A FAIL is always
  //   something the aligner DID.
  const untouched = !moved && D.slant === station;
  if (untouched && (badShare > BAD_SHARE || slantErr >= SLANT_WARN))
    return { verdict: 'LEFT', why: `drawn as received (nothing found to measure) — ${tag}` };
  if (slantErr >= SLANT_FAIL) return { verdict: 'FAIL', why: `slant off by ${slantErr.toFixed(3)} — ${tag}` };
  if (badShare > BAD_SHARE) return { verdict: 'FAIL', why: `${moved ? 'cut' : 'slanted'} through the picture — ${tag}` };
  if (slantErr >= SLANT_WARN) return { verdict: 'WARN', why: `slant off by ${slantErr.toFixed(3)} — ${tag}` };
  return { verdict: 'PASS', why: tag };
}

const charts = [
  ...loadManifest(join(HERE, 'corpus', 'corpus.json'), 'committed'),
  ...loadManifest(join(HERE, 'synthetic.json'), 'synthetic'),
  ...(process.env.WEFAX_WORLD_LOCAL ? loadManifest(join(process.env.WEFAX_WORLD_LOCAL, 'corpus.json'), 'local') : []),
];
const byStation = new Map<string, Record<Verdict, number>>();
const fails: string[] = [];
let total = 0;
for (const c of charts) {
  const safe = safeColumns(c.rows);
  if (VERBOSE) {   // the seam-safe columns, as runs
    const runs: string[] = []; let a = -1;
    for (let x = 0; x <= W; x++) { const on = x < W && safe[x]; if (on && a < 0) a = x; if (!on && a >= 0) { runs.push(`${a}…${x - 1}`); a = -1; } }
    console.log(`  ${c.e.call} ${c.e.file ?? c.e.synth} [${c.variant}] seam-safe columns: ${runs.join(' ')}`);
  }
  const name = `${c.e.call} ${c.e.file ?? c.e.synth}${c.variant === 'full' ? '' : ' [' + c.variant + ']'}`;
  const tally = byStation.get(`${c.e.call} ${c.variant} (${c.where})`) ?? { PASS: 0, LEFT: 0, WARN: 0, FAIL: 0 };
  const sigmas = QUICK ? [30] : [0, 30];
  const cases: Array<[string, 'phased' | 'late' | 'slant', number, number, number]> = [];
  cases.push(['(a) phased', 'phased', 0, 0, 0]);
  for (const j of QUICK ? [700] : [400, 700, 900]) cases.push([`(b) join last ${j}, rolled`, 'late', j, 50 + Math.floor(rnd() * (W - 100)), 0]);
  for (const kk of QUICK ? [0.02] : [0.02, -0.02]) cases.push([`(c) slant ${kk > 0 ? '+' : ''}${kk}`, 'slant', 0, 0, kk]);
  for (const sigma of sigmas) for (const [label, kind, j, roll, kk] of cases) {
    const r = run(c, safe, kind, j, roll, kk, sigma);
    tally[r.verdict]++; total++;
    const line = `${r.verdict.padEnd(4)} ${name} ${label} σ${sigma}: ${r.why}`;
    if (r.verdict === 'FAIL') fails.push(line);
    if (VERBOSE || r.verdict === 'FAIL' || r.verdict === 'WARN') console.log('  ' + line);
  }
  byStation.set(`${c.e.call} ${c.variant} (${c.where})`, tally);
}
console.log('\nper station  PASS LEFT WARN FAIL');
for (const [s, t] of byStation) console.log(`  ${s.padEnd(34)} ${String(t.PASS).padStart(4)} ${String(t.LEFT).padStart(4)} ${String(t.WARN).padStart(4)} ${String(t.FAIL).padStart(4)}`);
console.log(`wefax-world: ${charts.length} charts, ${total} runs, ${fails.length} FAIL`);
if (fails.length) process.exit(1);
