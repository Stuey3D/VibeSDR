/**
 * gen_dot_glyphs.mjs — reads Doto's OWN dots out of assets/fonts/Doto-Black.ttf (2026-10-06), so the DOT Display's
 * drawn fields (src/constants/dotField.ts DOTO_GLYPHS) light exactly the dots the Doto font lights in the frequency
 * digits and the station strip beside them.
 *
 * ★ Doto is a dot grid in the font's own units: every dot a 90 × 90 contour centred on 50 + 100 k, so a glyph is
 *   columns 0 … 4 (x 50 … 450) and rows 0 … 6 above the baseline (y 50 … 650), on a 600-unit advance — a 5 × 7
 *   cell, one dead column, a dot pitch of 0.1 em. This prints every printable ASCII glyph that sits inside that
 *   5 × 7 box as rows of '#' / '.', top row first; the ones with descenders or Black's heavy clusters (: . ;) are
 *   listed as skipped — dotField.ts draws those itself.
 *
 * Run: node scripts/gen_dot_glyphs.mjs [--check]   (plain Node, no dependencies; --check compares with dotField.ts)
 */
import { readFileSync } from 'node:fs';

const b = readFileSync(new URL('../assets/fonts/Doto-Black.ttf', import.meta.url));
const u16 = (o) => b.readUInt16BE(o), i16 = (o) => b.readInt16BE(o), u32 = (o) => b.readUInt32BE(o);
const T = {};
for (let i = 0; i < u16(4); i++) { const o = 12 + 16 * i; T[b.toString('ascii', o, o + 4)] = u32(o + 8); }
const locFmt = i16(T.head + 50);
const loca = (i) => (locFmt ? u32(T.loca + 4 * i) : 2 * u16(T.loca + 2 * i));
let sub = 0;
for (let i = 0; i < u16(T.cmap + 2); i++) { const o = u32(T.cmap + 8 + 8 * i); if (u16(T.cmap + o) === 4) sub = T.cmap + o; }
function gid(code) {
  const seg = u16(sub + 6) / 2, ends = sub + 14, starts = ends + 2 * seg + 2, deltas = starts + 2 * seg, ros = deltas + 2 * seg;
  for (let k = 0; k < seg; k++) {
    if (code > u16(ends + 2 * k)) continue;
    const st = u16(starts + 2 * k); if (code < st) return 0;
    const d = i16(deltas + 2 * k), ro = u16(ros + 2 * k);
    if (!ro) return (code + d) & 0xffff;
    const g = u16(ros + 2 * k + ro + 2 * (code - st));
    return g ? (g + d) & 0xffff : 0;
  }
  return 0;
}
/** Each contour's centre (the glyph's dots), or null for a composite glyph. */
function dots(g) {
  const o = T.glyf + loca(g);
  if (loca(g + 1) === loca(g)) return [];
  const nc = i16(o); if (nc < 0) return null;
  const ends = []; for (let i = 0; i < nc; i++) ends.push(u16(o + 10 + 2 * i));
  const np = ends[nc - 1] + 1; let p = o + 10 + 2 * nc; p += 2 + u16(p);
  const flags = [];
  while (flags.length < np) { const f = b[p++]; flags.push(f); if (f & 8) { let r = b[p++]; while (r--) flags.push(f); } }
  const xs = [], ys = []; let x = 0, y = 0;
  for (const f of flags) { if (f & 2) { const d = b[p++]; x += f & 16 ? d : -d; } else if (!(f & 16)) { x += i16(p); p += 2; } xs.push(x); }
  for (const f of flags) { if (f & 4) { const d = b[p++]; y += f & 32 ? d : -d; } else if (!(f & 32)) { y += i16(p); p += 2; } ys.push(y); }
  const out = []; let s = 0;
  for (const e of ends) {
    let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (let i = s; i <= e; i++) { x0 = Math.min(x0, xs[i]); x1 = Math.max(x1, xs[i]); y0 = Math.min(y0, ys[i]); y1 = Math.max(y1, ys[i]); }
    out.push([(x0 + x1) / 2, (y0 + y1) / 2]); s = e + 1;
  }
  return out;
}

const table = {}, skipped = [];
for (let c = 33; c < 127; c++) {
  const ch = String.fromCharCode(c), ds = dots(gid(c));
  if (!ds) { skipped.push(ch); continue; }
  const rows = Array.from({ length: 7 }, () => Array(5).fill('.'));
  let fits = ds.length > 0;
  for (const [x, y] of ds) {
    const col = (x - 50) / 100, row = 6 - (y - 50) / 100;
    if (!Number.isInteger(col) || !Number.isInteger(row) || col < 0 || col > 4 || row < 0 || row > 6) { fits = false; break; }
    rows[row][col] = '#';
  }
  if (fits) table[ch] = rows.map(r => r.join('')).join(' '); else skipped.push(ch);
}

if (process.argv.includes('--check')) {
  const src = readFileSync(new URL('../src/constants/dotField.ts', import.meta.url), 'utf8');
  let bad = 0, seen = 0;
  const m = /export const DOTO_GLYPHS[^{]*\{([\s\S]*?)\n\};/.exec(src);
  for (const line of (m ? m[1] : '').split('\n')) {
    const g = /^\s*("(?:[^"\\]|\\.)*"): '([#. ]+)',?/.exec(line);
    if (!g) continue;
    const ch = JSON.parse(g[1]); seen++;
    if (table[ch] !== g[2]) { bad++; console.error(`MISMATCH ${g[1]}: dotField '${g[2]}' vs Doto '${table[ch]}'`); }
  }
  if (seen !== Object.keys(table).length) { bad++; console.error(`dotField carries ${seen} glyphs, Doto has ${Object.keys(table).length}`); }
  console.log(bad ? `FAIL ${bad} glyph(s) differ from Doto` : `ok  all ${seen} DOTO_GLYPHS entries are Doto's own dots`);
  process.exit(bad ? 1 : 0);
}
for (const [ch, rows] of Object.entries(table)) console.log(`  ${JSON.stringify(ch)}: '${rows}',`);
console.log(`// skipped (outside 5 × 7, or composite): ${skipped.join(' ')}`);
