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
import { segModeCells, segReadingCells, SEG_MODE_CELLS, SEG_READ_GHOST, SEG_STEREO_CELLS } from '../src/constants/segField.ts';
import { toSegCells, segCellList } from '../src/constants/displayText.ts';
import {
  composeModeLabel, modeLabelCandidates, modeBoxFit, segReadingGeometry, segModeFieldWidth, SEG_MODE_FIELD_CELLS,
  SEG_LEGEND, MODE_BOX,
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
   { cells: ['U', 'S', 'B', 'R', 'T', 'T', 'Y'], colon: true, stereoSlot: false, textCells: 10, marquee: false });
eq('AM sits where it does beside a decoder', segModeCells('AM', false, cells).cells, ['', 'A', 'M']);
eq('AM: RTTY — the demod right-aligned before the colon', segModeCells('AM: RTTY', false, cells).cells.slice(0, 3), ['', 'A', 'M']);
eq('MESHTASTIC — ten cells, no colon', segModeCells('MESHTASTIC', false, cells).cells.length, 10);
eq('WFM — the rings keep their slot', segModeCells('WFM', true, cells),
   { cells: ['W', 'F', 'M'], colon: false, stereoSlot: true, textCells: SEG_MODE_CELLS - SEG_STEREO_CELLS, marquee: false });
eq('WFM: WHISPER — the decoder takes the rings\' slot, shown whole', segModeCells('WFM: WHISPER', true, cells).marquee, false);
eq('DAB', segModeCells('DAB', false, cells).cells, ['D', 'A', 'B']);
{
  const L = segModeCells('USB: AVERYLONGDECODER', false, cells);
  ok('an unforeseen long name steps through the field, never cut', L.marquee && L.cells.join('').includes('AVERYLONGDECODER'));
  ok('…its colon taking a cell of its own, so it moves with the text', L.cells.includes(':') && !L.colon);
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

console.log(fails ? `FAIL faceplate segfield: ${passes} passed, ${fails} failed` : `ok  faceplate segfield: ${passes} passed, 0 failed`);
process.exit(fails ? 1 : 0);
