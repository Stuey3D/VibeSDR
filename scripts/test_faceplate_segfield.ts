/**
 * The VCR mode box's fixed 14-segment fields (src/constants/segField.ts, 2026-10-06):
 *   ★ every label the app composes fits the ten-cell mode field WHOLE — no marquee, nothing cut;
 *   ★ WFM keeps the stereo rings' slot; every other mode gets those cells back;
 *   ★ the readout "-88+88 dB F S" lights only electrodes its ghost has, in every meter mode, for every value
 *     the meters can produce — and its width never changes.
 *
 * Run: node --no-warnings scripts/test_faceplate_segfield.ts
 */
import { readFileSync } from 'node:fs';
import { segModeCells, segReadingCells, segUnitCells, SEG_MODE_CELLS, SEG_READ_GHOST, SEG_STEREO_CELLS, SEG_UNIT_CELLS } from '../src/constants/segField.ts';
import { toSegCells, segCellList } from '../src/constants/displayText.ts';
import {
  composeModeLabel, modeLabelCandidates, modeBoxFit, segReadingGeometry, segModeFieldWidth, SEG_MODE_FIELD_CELLS,
  SEG_LEGEND, MODE_BOX, segUnitFont, segCellsWidth, SEG_UNIT_FIELD_CELLS, SEG14_ADV, SEG14_PITCH,
} from '../src/constants/modeBox.ts';
import { formatReading, sMeterText } from '../src/constants/meters.ts';
import { WHOLE_PROFILE_MODES } from '../src/services/dataModes.ts';
import { statusParts, statusSegSlots, statusSegWidth, SEG_LOWER_D, STATUS_SEG_ADV, STATUS_SEG_PITCH } from '../src/constants/statusField.ts';
import * as SR from './lib_status_row.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const cells = (t: string) => segCellList(toSegCells(t)).map(c => c.replace(/^!/, ''));

// ── One rule, two readers: the copies that cannot import each other at runtime ──
eq('the field is ten cells in both copies', SEG_MODE_CELLS, SEG_MODE_FIELD_CELLS);
{
  const src = readFileSync(new URL('../src/components/AnnunciatorLegend.tsx', import.meta.url), 'utf8');
  ok('modeBox SEG_LEGEND = AnnunciatorLegend\'s letter cell',
     src.includes(`const CELL_W = ${SEG_LEGEND.cellW}, CELL_H = ${SEG_LEGEND.cellH}, GAP = ${SEG_LEGEND.gap},`));
}

// ── The mode field ──
// Every label the code composes, plus decoder names an OpenWebRX server sends that the app cannot list.
const OWRX_DECODERS = ['ft4', 'wspr', 'jt65', 'jt9', 'fst4', 'fst4w', 'q65', 'js8', 'packet', 'pocsag', 'bpsk31', 'bpsk63',
                       'rtty', 'navtex', 'sitorb', 'dsc', 'cwskimmer', 'hfdl', 'vdl2', 'acars', 'ais', 'page', 'selcall', 'eas'];
const labels = [
  ...modeLabelCandidates(WHOLE_PROFILE_MODES),
  ...['usb', 'lsb', 'nfm', 'am'].flatMap(m => OWRX_DECODERS.map(d => ({ label: composeModeLabel(m, d), stereo: false }))),
  { label: 'LORA-SF12', stereo: false },
];
let longest = { label: '', n: 0 };
const overflow: string[] = [];
for (const c of labels) {
  const stereo = c.stereo || /^WFM\b/.test(c.label);
  const L = segModeCells(c.label, stereo, cells);
  if (L.marquee) { overflow.push(c.label); continue; }
  const n = L.cells.length;
  if (n > longest.n) longest = { label: c.label, n };
  ok(`"${c.label}": fits ${L.textCells} cells (${n})`, n <= L.textCells);
}
console.log(`  longest mode legend: "${longest.label}" (${longest.n} cells); stepped (too long for the field): ${overflow.length ? overflow.join(', ') : 'none'}`);
// ★ OpenWebRX+'s CW Skimmer ("…: CWSKIMMER", 12 cells) is the one known name past ten: it STEPS through the
//   field like the VTS strip, whole. Everything the app composes itself fits without moving.
eq('only CW Skimmer is too long for the field — and it steps, never cut', overflow.filter(l => !/CWSKIMMER$/.test(l)), []);
eq('USB:RTTY — demod, lit colon in the gap, decoder', segModeCells('USB: RTTY', false, cells),
   { cells: ['U', 'S', 'B', 'R', 'T', 'T', 'Y'], colon: true, split: true, stereoSlot: false, textCells: 10, marquee: false });
// ★ 2026-10-06 ("WF M"): a plain mode is ONE TIGHT WORD — from the first cell, and not split, so SegModeReadout
//   ghosts no colon electrode after it.
eq('AM — a plain mode starts at the first cell', segModeCells('AM', false, cells).cells, ['A', 'M']);
eq('plain modes are never split (no colon electrode)', ['AM', 'WFM', 'USB', 'MESHTASTIC', 'DAB'].map(l => segModeCells(l, /^WFM/.test(l), cells).split),
   [false, false, false, false, false]);
eq('a decoder splits the field', ['USB: RTTY', 'AM: FT8', 'WFM: WHISPER'].map(l => segModeCells(l, /^WFM/.test(l), cells).split), [true, true, true]);
eq('AM: RTTY — the demod right-aligned before the colon', segModeCells('AM: RTTY', false, cells).cells.slice(0, 3), ['', 'A', 'M']);
eq('MESHTASTIC — ten cells, no colon', segModeCells('MESHTASTIC', false, cells).cells.length, 10);
eq('WFM — the rings keep their slot', segModeCells('WFM', true, cells),
   { cells: ['W', 'F', 'M'], colon: false, split: false, stereoSlot: true, textCells: SEG_MODE_CELLS - SEG_STEREO_CELLS, marquee: false });
eq('WFM: WHISPER — the decoder takes the rings\' slot, shown whole', segModeCells('WFM: WHISPER', true, cells).marquee, false);
eq('DAB', segModeCells('DAB', false, cells).cells, ['D', 'A', 'B']);
{
  const L = segModeCells('USB: AVERYLONGDECODER', false, cells);
  ok('an unforeseen long name steps through the field, never cut', L.marquee && L.cells.join('').includes('AVERYLONGDECODER'));
  ok('…its colon taking a cell of its own, so it moves with the text', L.cells.includes(':') && !L.colon && !L.split);
}

// ── The readout ──
eq('S9+27', segReadingCells('S9+27', 'smeter'), { cells: ['', '5', '9', '+', '2', '7'], dB: true, F: false, S: false });
eq('S7 — no over, no unit', segReadingCells('S7', 'smeter'), { cells: ['', '5', '7', '', '', ''], dB: false, F: false, S: false });
eq('S9+5', segReadingCells('S9+5', 'smeter').cells, ['', '5', '9', '+', '', '5']);
eq('-73 dBFS', segReadingCells('-73dB', 'dbfs'), { cells: ['-', '7', '3', '', '', ''], dB: true, F: true, S: true });
eq('-7 dBFS — the minus beside its digit', segReadingCells('-7dB', 'dbfs').cells, ['', '-', '7', '', '', '']);
eq('-105 dBFS — the ±1 half-digit', segReadingCells('-105dB', 'dbfs').cells, ['-1', '0', '5', '', '', '']);
eq('24 dB SNR', segReadingCells('24db', 'snr'), { cells: ['', '2', '4', '', '', ''], dB: true, F: false, S: false });
eq('120 dBf (FM-DX)', segReadingCells('120 dBf', 'snr'), { cells: ['1', '2', '0', '', '', ''], dB: true, F: true, S: false });
eq('no reading — dark, no unit', segReadingCells('', 'snr'), { cells: ['', '', '', '', '', ''], dB: false, F: false, S: false });

// Every value the meters can format lights only electrodes its cell has.
const ALLOWED: Record<string, Set<string>> = {
  '-1': new Set(['', '-', '1', '-1']),
  '8': new Set(['', '-', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9']),
  '+': new Set(['', '+', '-']),
};
const units = ['snr', 'smeter', 'dbfs', 'dbf'] as const;
for (const u of units) for (let v = -150; v <= 150; v++) {
  const t = formatReading(u, v);
  const r = segReadingCells(t, u === 'dbf' ? 'snr' : u);
  eq(`${u} ${v} ("${t}"): six cells`, r.cells.length, 6);
  r.cells.forEach((c, i) => ok(`${u} ${v} ("${t}"): cell ${i} "${c}" is inside its electrodes "${SEG_READ_GHOST[i]}"`,
                               ALLOWED[SEG_READ_GHOST[i]].has(c)));
  if (t) ok(`${u} ${v} ("${t}"): reads something`, r.cells.some(c => c !== ''));
}
for (let db = -130; db <= 0; db++) {
  const t = sMeterText(db);
  ok(`S-meter ${db} ("${t}") parses`, segReadingCells(t, 'smeter').cells[1] === '5');
}

// ── The box holds both fields, at every portrait width (test_faceplate_meters covers the label side) ──
for (const W of [320, 375, 390, 430, 768, 1024]) {
  const scale = Math.max(0.75, Math.min(1.45, W / 390));
  const r = (n: number) => Math.round(n * scale);
  const winW = W - 2 * 8 - 2 * r(14);
  const f = modeBoxFit({ label: 'USB: RTTY', stereo: false, face: 'seg', fontSize: r(15), letterSpacing: 2, readingFont: r(11),
                         minW: r(MODE_BOX.minW), padH: r(MODE_BOX.padH), windowW: winW });
  const need = Math.max(segModeFieldWidth(f.fontSize), segReadingGeometry(r(11)).width) + 2 * r(MODE_BOX.padH);
  ok(`${W} pt: the box (${f.width}) holds the mode field and the readout (${need.toFixed(1)})`, need <= f.width + 1e-9);
}

// ── The frequency unit (VCR window): capitals in a fixed three cells, right-aligned ──
eq('unit cells agree in both copies', SEG_UNIT_CELLS, SEG_UNIT_FIELD_CELLS);
eq('kHz → KHZ', segUnitCells('kHz'), ['K', 'H', 'Z']);
eq('MHz → MHZ', segUnitCells('MHz'), ['M', 'H', 'Z']);
eq('Hz → right-aligned HZ', segUnitCells('Hz'), ['', 'H', 'Z']);
for (const u of ['Hz', 'kHz', 'MHz']) {
  ok(`${u}: every lit cell is a 14-segment glyph`, segUnitCells(u).every(c => c === '' || cells(c).join('') === c));
  eq(`${u}: three cells`, segUnitCells(u).length, SEG_UNIT_CELLS);
}
// The field fits the label column the plain-text unit had (DisplayFreq: round(2.6 × size), 3 pt right padding),
// at every unit size the bar, the LED / analogue window and landscape use — so the digits never move.
for (let size = 6; size <= 22; size += 0.5) {
  const colW = Math.round(size * 2.6) - 3;
  const fs = segUnitFont(size, colW);
  ok(`unit ${size} pt: three cells (${segCellsWidth(SEG_UNIT_CELLS, fs).toFixed(2)}) fit the column (${colW})`,
     segCellsWidth(SEG_UNIT_CELLS, fs) <= colW + 1e-9);
}

// ── The status row in 14-segment cells (2026-10-06: constants/statusField, components/StatusField) ──
{
  const SEG14 = readFileSync(new URL('../src/components/SegField.tsx', import.meta.url), 'utf8');
  const alphaSrc = /const ALPHABET = ("(?:[^"\\]|\\.)*");/.exec(SEG14)?.[1];
  const alpha: string = alphaSrc ? JSON.parse(alphaSrc) : '';
  ok('SegField\'s atlas alphabet found', alpha.length > 40);
  eq('statusField\'s DSEG14 cell copies = modeBox\'s', [STATUS_SEG_ADV, STATUS_SEG_PITCH], [SEG14_ADV, SEG14_PITCH]);
  eq('a run is n cells at the fixed pitch', [1, 2, 10].map(n => statusSegWidth(n, 10)), [1, 2, 10].map(n => segCellsWidth(n, 10)));
  const S = (t: string) => statusSegSlots(statusParts(t), SR.segCells);
  eq('17:03 UTC — four digits, the colon in the GAP after "7", a blank, UTC', S('17:03 UTC'),
     { cells: ['1', '7', '0', '3', '', 'U', 'T', 'C'], ghost: Array(8).fill('~'), colons: [1], logos: [] });
  eq('0:12:34 — two gap colons', S('0:12:34').colons, [0, 2]);
  // ★ 2026-10-06: the unit's d is the classic VFD lower-case d — 'J' (b c d e) + '-' (g) in one cell; the B stays B.
  eq('44.5dB — the point on its cell, the unit\'s d lower case (J + centre bar), B the 14-seg B', S('44.5dB').cells,
     ['4', '4.', '5', 'J-', 'B']);
  eq('SEG_LOWER_D is J + the centre bar', SEG_LOWER_D, 'J-');
  eq('GAIN 25.4dB — no longer "25433"', S('GAIN 25.4dB').cells.join('|'), 'G|A|I|N||2|5.|4|J-|B');
  eq('dBFS / dBm / dBf keep the lower-case d', ['-73 dBFS', '-60dBm', '28 dBf'].map(t => S(t).cells.find(c => c.startsWith('J'))),
     ['J-', 'J-', 'J-']);
  eq('DAB is unchanged (capital D)', S('DAB').cells, ['D', 'A', 'B']);
  eq('BBC / D in a word / "d" glued to letters / a lone "db" — the 14-seg alphabet', ['BBC', 'DAB+ 12D', 'odB', 'AdB', 'db', 'dBx']
     .map(t => S(t).cells.includes('J-')), [false, false, false, false, false, false]);
  eq('44.5dB\'s ghost carries the point, and the d\'s cell keeps every segment', S('44.5dB').ghost, ['~', '~.', '~', '~', '~']);
  eq('23k/s 10fps → 23K/S 10FPS, "/" is DSEG14\'s own', S('23k/s 10fps').cells, ['2', '3', 'K', '/', 'S', '', '1', '0', 'F', 'P', 'S']);
  eq('IF 1400k auto → AUTO', S('· IF 1400k auto').cells.join('|'), '·|I|F||1|4|0|0|K||A|U|T|O');
  eq('"·" swallows its spaces and is the raised point, alone in its cell', S('TUNER · FREE'),
     { cells: ['T', 'U', 'N', 'E', 'R', '·', 'F', 'R', 'E', 'E'], ghost: ['~', '~', '~', '~', '~', '·', '~', '~', '~', '~'], colons: [], logos: [] });
  eq('+ and ( ) as themselves', S('(You+2)').cells, ['(', 'Y', 'O', 'U', '+', '2', ')']);
  eq('symbols are logos: ⚡ ⚿ 👤 ⛛ ↑ ↓', statusParts('⚡ STORMS ⚿ 👤 ⛛ ↑↓').filter(p => typeof p !== 'string').map(p => (p as any).kind),
     ['bolt', 'key', 'person', 'node', 'arrow', 'arrow']);
  eq('the server mark takes two cells, no segments', statusSegSlots([{ kind: 'node' }, '18:03'], SR.segCells).ghost.slice(0, 3), ['', '', '~']);
  // Every lit cell of every run the row can show is a glyph SegField can light (or its point / the raised point).
  for (const [k, parts] of Object.entries(SR.runs('seg'))) {
    const s = statusSegSlots(parts, SR.segCells);
    eq(`${k}: one ghost per cell`, s.ghost.length, s.cells.length);
    for (const c of s.cells) ok(`${k}: cell "${c}" is drawable`, c === '' || c === '·' || [...c.replace(/\.$/, '')].every(ch => alpha.includes(ch)));
  }
  // ★ No jitter: a reading changing its digits keeps the cell count; a run only grows when its text does.
  eq('GAIN ↓44.5dB and ↑ 9.0dB… same cells; a steady gain keeps the arrow\'s cell (dark)',
     [{ dir: 'down', v: '44.5dB' }, { dir: 'up', v: '12.0dB' }, { dir: null, v: '44.5dB' }].map(g =>
       statusSegSlots([...statusParts('· GAIN'), { kind: 'arrow', dir: g.dir as any }, ...statusParts(g.v)], SR.segCells).cells.length),
     [11, 11, 11]);
  // ── It fits, at every width ──
  for (const W of SR.LANDSCAPE_WIDTHS) {
    const f = SR.landscapeFit('seg', W);
    ok(`VCR landscape ${W} pt: the row fits after its drops`, f.fits);
    ok(`VCR landscape ${W} pt: the connection bars and the recording timer are never dropped`, !f.hidden.includes('rec'));
    console.log(`  VCR landscape ${W} pt (status ${f.size.toFixed(1)} pt): drops ${f.hidden.length ? f.hidden.join(', ') : 'nothing'}${f.sharedShort ? ' (SHARED TUNER → SHARED)' : ''}`);
  }
  ok('VCR: a full-screen Mac shows everything', SR.landscapeFit('seg', 1920).hidden.length === 0);
  for (const W of SR.PORTRAIT_WIDTHS) {
    const p = SR.portraitStatsFit('seg', W), c = SR.portraitClockRow('seg', W);
    ok(`VCR portrait ${W} pt: the stats line fits after its drops`, p.fits);
    ok(`VCR portrait ${W} pt: clocks + recording + three DSP badges fit row 4 (${c.need.toFixed(0)} of ${c.avail})`, c.need <= c.avail);
    console.log(`  VCR portrait ${W} pt: stats line drops ${p.hidden.length ? p.hidden.join(', ') : 'nothing'}`);
  }
}

console.log(fails ? `FAIL faceplate segfield: ${passes} passed, ${fails} failed` : `ok  faceplate segfield: ${passes} passed, 0 failed`);
process.exit(fails ? 1 : 0);
