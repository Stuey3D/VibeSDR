/**
 * The DOT mode box's fixed dot-matrix fields (src/constants/dotField.ts, 2026-10-06) — test_faceplate_segfield's twin:
 *   ★ the glyphs are Doto's own dots (the font's outlines, not a hand-drawn copy), each a 5 × 7 cell;
 *   ★ every label the app composes fits the ten-cell mode field WHOLE, lighting only glyphs the field has;
 *   ★ WFM keeps the stereo rings' slot — rings in dots, on whole columns of the same grid; other modes get the cells;
 *   ★ the readout "S9+27 dBFS" keeps every value in its columns, for every value the meters can produce, and its
 *     units light per meter mode (dBFS, dBf with a real lower-case f, dB);
 *   ★ the unit "kHz" / "MHz" keeps its case and fits the column it replaces;
 *   ★ none of the fields changes width, whatever it shows.
 *
 * Run: node --no-warnings scripts/test_faceplate_dotfield.ts
 */
import { execFileSync } from 'node:child_process';
import {
  bitmapDots, dotChar, dotGlyph, dotModeCellCol, dotPitch, dotReadingCells, dotUnitCells, dotUnitPitch, toDotCells,
  DOTO_GLYPHS, DOT_ADV, DOT_ALPHABET, DOT_COLON, DOT_COLON_COL, DOT_COLON_W, DOT_COLS, DOT_DEMOD_CELLS, DOT_MODE_CELLS,
  DOT_MODE_COLS, DOT_READ_CELLS, DOT_READ_COLS, DOT_READ_SQL, DOT_RINGS, DOT_RINGS_COL, DOT_RINGS_W, DOT_ROWS,
  DOT_STEREO_CELLS, DOT_UNIT_CELLS, DOT_UNIT_COLS,
} from '../src/constants/dotField.ts';
import { segModeCells, SEG_MODE_CELLS, SEG_DEMOD_CELLS, SEG_STEREO_CELLS } from '../src/constants/segField.ts';
import {
  composeModeLabel, modeLabelCandidates, modeBoxFit, modeTextWidth, dotModeFieldWidth, dotReadFieldWidth, MODE_BOX,
  DOT_MODE_FIELD_COLS, DOT_READ_FIELD_COLS, DOTO_EM,
} from '../src/constants/modeBox.ts';
import { formatReading, sMeterText } from '../src/constants/meters.ts';
import { WHOLE_PROFILE_MODES } from '../src/services/dataModes.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const isBitmap = (rows: string, w: number, h: number) => {
  const r = rows.split(' ');
  return r.length === h && r.every(x => x.length === w && /^[#.]+$/.test(x));
};

// ── The glyphs are Doto's own dots ──
{
  let out = '', code = 0;
  try { out = execFileSync(process.execPath, [new URL('./gen_dot_glyphs.mjs', import.meta.url).pathname, '--check'], { encoding: 'utf8' }); }
  catch (e: any) { code = e.status ?? 1; out = String(e.stdout ?? '') + String(e.stderr ?? ''); }
  ok(`DOTO_GLYPHS is what Doto-Black.ttf draws (gen_dot_glyphs --check: ${out.trim()})`, code === 0);
}
eq('Doto\'s cell: 5 × 7 dots, one dead column (its 600-unit advance on a 100-unit pitch)', [DOT_COLS, DOT_ROWS, DOT_ADV], [5, 7, 6]);
eq("…its advance in pitches is DOTO_EM in em", DOT_ADV / 10, DOTO_EM);
for (const ch of [...DOT_ALPHABET]) ok(`glyph ${JSON.stringify(ch)}: a 5 × 7 bitmap with something lit`,
  isBitmap(dotGlyph(ch)!, DOT_COLS, DOT_ROWS) && bitmapDots(dotGlyph(ch)!).length > 0);
ok('a real lower case: k, z, d, f, and B, F, S, H, M', [...'kzdfBFSHM'].every(c => DOTO_GLYPHS[c]));
ok('a real S, not a 5', DOTO_GLYPHS.S !== DOTO_GLYPHS['5']);
ok('a real colon, plus and minus', !!dotGlyph(':') && !!DOTO_GLYPHS['+'] && !!DOTO_GLYPHS['-']);
eq('a descender falls back to its capital; a space is dark; the unknown is ?', [dotChar('g'), dotChar(' '), dotChar('€')], ['G', '', '?']);

// ── One rule, two readers ──
eq('mode field: ten cells, three demod, three for the rings — segField\'s numbers', [DOT_MODE_CELLS, DOT_DEMOD_CELLS, DOT_STEREO_CELLS],
   [SEG_MODE_CELLS, SEG_DEMOD_CELLS, SEG_STEREO_CELLS]);
eq('modeBox\'s copy of the mode field\'s width', DOT_MODE_FIELD_COLS, DOT_MODE_COLS);
eq('modeBox\'s copy of the readout\'s width', DOT_READ_FIELD_COLS, DOT_READ_COLS);

// ── The mode field's geometry: one grid ──
eq('the demod cells sit on the cell pitch', [0, 1, 2].map(dotModeCellCol), [0, 6, 12]);
eq('the colon column follows the demod\'s dead column', DOT_COLON_COL, 18);
ok('the colon is two dots wide, 2 × 2 blocks', isBitmap(DOT_COLON, DOT_COLON_W, DOT_ROWS) && bitmapDots(DOT_COLON).length === 8);
eq('cell 3 follows the colon\'s own dead column', dotModeCellCol(3), DOT_COLON_COL + DOT_COLON_W + 1);
eq('the field: 62 dots (3 cells, the colon, 7 cells)', DOT_MODE_COLS, 62);
ok('the rings: 11 × 7 dots', isBitmap(DOT_RINGS, DOT_RINGS_W, DOT_ROWS));
ok('the rings sit inside the last three cells\' room', DOT_RINGS_COL >= dotModeCellCol(DOT_MODE_CELLS - DOT_STEREO_CELLS)
   && DOT_RINGS_COL + DOT_RINGS_W <= DOT_MODE_COLS);
ok('…on whole columns of the same grid', Number.isInteger(DOT_RINGS_COL));
{
  const rows = DOT_RINGS.split(' ');
  ok('the rings cross: the lens columns are lit top and bottom of both rings', rows[1][5] === '#' && rows[2][4] === '#' && rows[2][6] === '#');
  eq('the rings are mirror-symmetric', rows.map(r => [...r].reverse().join('')), rows);
}

// ── The mode field: every label ──
const OWRX_DECODERS = ['ft4', 'wspr', 'jt65', 'jt9', 'fst4', 'fst4w', 'q65', 'js8', 'packet', 'pocsag', 'bpsk31', 'bpsk63',
                       'rtty', 'navtex', 'sitorb', 'dsc', 'cwskimmer', 'hfdl', 'vdl2', 'acars', 'ais', 'page', 'selcall', 'eas'];
const labels = [
  ...modeLabelCandidates(WHOLE_PROFILE_MODES),
  ...['usb', 'lsb', 'nfm', 'am'].flatMap(m => OWRX_DECODERS.map(d => ({ label: composeModeLabel(m, d), stereo: false }))),
  { label: 'LORA-SF12', stereo: false },
];
let longest = { label: '', n: 0 };
const overflow: string[] = [];
const widths = new Set<number>();
for (const c of labels) {
  const stereo = c.stereo || /^WFM\b/.test(c.label);
  const L = segModeCells(c.label, stereo, toDotCells, DOT_MODE_CELLS);
  widths.add(modeTextWidth(c.label, 15, 2, 'dot'));
  ok(`"${c.label}": every character is a glyph the display has (no '?')`, L.cells.every(x => x === '' || (x !== '?' && !!dotGlyph(x))));
  if (L.marquee) { overflow.push(c.label); continue; }
  const n = L.cells.length;
  if (n > longest.n) longest = { label: c.label, n };
  ok(`"${c.label}": fits ${L.textCells} cells (${n})`, n <= L.textCells);
}
console.log(`  longest mode legend: "${longest.label}" (${longest.n} cells); stepped: ${overflow.length ? overflow.join(', ') : 'none'}`);
eq('only CW Skimmer is too long for the field — and it steps, never cut', overflow.filter(l => !/CWSKIMMER$/.test(l)), []);
eq('★ the field is ONE width whatever the label', widths.size, 1);
eq('USB:RTTY — demod, lit colon column, decoder', segModeCells('USB: RTTY', false, toDotCells, DOT_MODE_CELLS),
   { cells: ['U', 'S', 'B', 'R', 'T', 'T', 'Y'], colon: true, split: true, stereoSlot: false, textCells: 10, marquee: false });
eq('USB:WHISPER — ten cells, whole', segModeCells('USB: WHISPER', false, toDotCells, DOT_MODE_CELLS).cells.length, 10);
// ★ 2026-10-06: a plain mode is one tight word — no leading dark cell, no colon column (DotModeReadout puts the cells on
//   the plain pitch unless `split`).
eq('AM — a plain mode starts at the first cell', segModeCells('AM', false, toDotCells, DOT_MODE_CELLS).cells, ['A', 'M']);
eq('AM: RTTY — split: the demod right-aligned before the colon column',
   segModeCells('AM: RTTY', false, toDotCells, DOT_MODE_CELLS).cells.slice(0, 3), ['', 'A', 'M']);
eq('MESHTASTIC / MESHCORE — no colon, not split (no colon column inside the word)',
   ['MESHTASTIC', 'MESHCORE'].map(l => { const L = segModeCells(l, false, toDotCells, DOT_MODE_CELLS); return [L.colon, L.split]; }),
   [[false, false], [false, false]]);
eq('WFM — the rings keep their slot', segModeCells('WFM', true, toDotCells, DOT_MODE_CELLS),
   { cells: ['W', 'F', 'M'], colon: false, split: false, stereoSlot: true, textCells: 7, marquee: false });
ok('plain cells on the plain pitch (DOT_ADV) fit the field and clear the rings',
   (DOT_MODE_CELLS - 1) * DOT_ADV + DOT_COLS <= DOT_MODE_COLS && (DOT_MODE_CELLS - DOT_STEREO_CELLS - 1) * DOT_ADV + DOT_COLS < DOT_RINGS_COL);
eq('WFM: WHISPER — the decoder takes the rings\' slot, shown whole', segModeCells('WFM: WHISPER', true, toDotCells, DOT_MODE_CELLS).marquee, false);

// ── The readout ──
const R = (t: string, u: 'snr' | 'smeter' | 'dbfs' | 'dbf') => dotReadingCells(t, u).cells.join('|');
eq('S9+27 dB', R('S9+27', 'smeter'), 'S|9|+|2|7||d|B||');
eq('S7 — no over, no unit', R('S7', 'smeter'), 'S|7||||||||');
eq('S9+5 — the units digit stays in its column', R('S9+5', 'smeter'), 'S|9|+||5||d|B||');
eq('-73 dBFS', R('-73dB', 'dbfs'), '||-|7|3||d|B|F|S');
eq('-7 dBFS — the minus beside its digit', R('-7dB', 'dbfs'), '|||-|7||d|B|F|S');
eq('-105 dBFS — a full cell, no half-digit', R('-105dB', 'dbfs'), '|-|1|0|5||d|B|F|S');
eq('24 dB SNR', R('24db', 'snr'), '|||2|4||d|B||');
eq('120 dBf (FM-DX) — a real lower-case f, no S', R('120 dBf', 'snr'), '||1|2|0||d|B|f|');
eq('dBf by setting', R('120', 'dbf'), '||1|2|0||d|B|f|');
eq('no reading — dark, no unit', R('', 'snr'), '|||||||||');
eq('SQL in the number\'s columns', DOT_READ_SQL.join('|'), '||S|Q|L|||||');

const ALLOWED_UNIT = [new Set(['', 'd']), new Set(['', 'B']), new Set(['', 'F', 'f']), new Set(['', 'S'])];
for (const u of ['snr', 'smeter', 'dbfs', 'dbf'] as const) for (let v = -150; v <= 150; v++) {
  const t = formatReading(u, v);
  const r = dotReadingCells(t, u === 'dbf' ? 'snr' : u).cells;
  const tag = `${u} ${v} ("${t}")`;
  eq(`${tag}: ten cells`, r.length, DOT_READ_CELLS);
  ok(`${tag}: every lit cell is a glyph`, r.every(c => c === '' || !!dotGlyph(c)));
  eq(`${tag}: cell 5 is the space`, r[5], '');
  r.slice(6).forEach((c, i) => ok(`${tag}: unit cell ${6 + i} "${c}" is its legend`, ALLOWED_UNIT[i].has(c)));
  ok(`${tag}: F/f and S only with dB`, (!r[8] && !r[9]) || (r[6] === 'd' && r[7] === 'B'));
  ok(`${tag}: S only with F (dBFS)`, !r[9] || r[8] === 'F');
  if (t && !/^S/.test(t)) eq(`${tag}: the number ends in cell 4`, /\d/.test(r[4]), true);
  if (t) ok(`${tag}: reads something`, r.slice(0, 5).some(c => c !== ''));
  if (u === 'dbfs') ok(`${tag}: dBFS lights d B F S`, r.slice(6).join('') === 'dBFS');
}
for (let db = -130; db <= 0; db++) {
  const t = sMeterText(db);
  const r = dotReadingCells(t, 'smeter').cells;
  ok(`S-meter ${db} ("${t}"): a real S in cell 0, its digit in cell 1`, r[0] === 'S' && /\d/.test(r[1]));
}

// ── The frequency unit: case kept, three cells, right-aligned ──
eq('kHz', dotUnitCells('kHz'), ['k', 'H', 'z']);
eq('MHz', dotUnitCells('MHz'), ['M', 'H', 'z']);
eq('Hz → right-aligned', dotUnitCells('Hz'), ['', 'H', 'z']);
eq('the field: 17 dots', DOT_UNIT_COLS, DOT_UNIT_CELLS * DOT_ADV - 1);
// The label column the plain-text unit had (DisplayFreq: round(2.6 × size), 3 pt right padding), at every unit size,
// beside every Doto digit size the windows use — the field fits it, and its pitch is never coarser than half the digits'.
for (let size = 6; size <= 22; size += 0.5) for (const freqFont of [17, 19, 22, 23, 24, 27, 28, 32, 40]) {
  const colW = Math.round(size * 2.6) - 3;
  const p = dotUnitPitch(freqFont, colW);
  ok(`unit ${size} pt beside Doto ${freqFont}: 17 dots (${(DOT_UNIT_COLS * p).toFixed(2)}) fit the column (${colW})`,
     DOT_UNIT_COLS * p <= colW + 1e-9);
  ok(`unit ${size} pt beside Doto ${freqFont}: at most half the digits' pitch`, p <= dotPitch(freqFont) / 2 + 1e-12);
}

// ── The box holds both fields, at every portrait width ──
for (const W of [320, 375, 390, 430, 768, 1024]) {
  const scale = Math.max(0.75, Math.min(1.45, W / 390));
  const r = (n: number) => Math.round(n * scale);
  const winW = W - 2 * 8 - 2 * r(14);
  for (const c of labels) {
    const f = modeBoxFit({ label: c.label, stereo: c.stereo, face: 'dot', fontSize: r(15), letterSpacing: 2, readingFont: r(11),
                           minW: r(MODE_BOX.minW), padH: r(MODE_BOX.padH), windowW: winW });
    const need = Math.max(dotModeFieldWidth(f.fontSize), dotReadFieldWidth(r(11))) + 2 * r(MODE_BOX.padH);
    ok(`${W} pt "${c.label}": the box (${f.width}) holds the mode field and the readout (${need.toFixed(1)})`, need <= f.width + 1e-9);
    ok(`${W} pt "${c.label}": full type`, !f.squeezed);
  }
}
eq('the mode field is the Doto label\'s height: 7 dots at 0.1 em = 0.7 em', DOT_ROWS * dotPitch(15), 15 * 0.7);

console.log(fails ? `FAIL faceplate dotfield: ${passes} passed, ${fails} failed` : `ok  faceplate dotfield: ${passes} passed, 0 failed`);
process.exit(fails ? 1 : 0);
