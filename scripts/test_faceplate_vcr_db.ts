/**
 * test_faceplate_vcr_db.ts — ★★★ EVERY dB ON THE VCR DISPLAY HAS A LOWER-CASE d (2026-10-06).
 *
 * Stuart: "make sure that in the next build any dB icons on the VCR VFD display are correctly set with the lower case
 * d. I know it's a ballache but it's one thing where nitpickers will have us for it."
 *
 * DSEG14 has no lower case, so the d of a dB unit is lit as two of its glyphs in one cell, 'J' (b c d e) + '-' (g):
 *   • plain DSEG14 lines (the VTS strip, the DAB meter, the notices) — toSegCells()/toSegRun()/screenString('seg') mark
 *     it 'd', and SegLowerDText draws 'J' with a '-' over it (segLitText / segBarRuns);
 *   • SegField lines (the status row, its chips, the mode box) — the cell is 'J-' (segFieldCells, statusSegSlots);
 *   • the mode box's readout — its printed dB legend (segReadingCells .dB), a real lower-case d already.
 * This walks every producer that can put a dB on the VCR, and every word with a D that must NOT change.
 * Plain node: node --no-warnings scripts/test_faceplate_vcr_db.ts
 */
import {
  isDbUnitDAt, screenString, segBarRuns, segCellList, segFieldCells, segHasLowerD, segLitText, statusGainParts,
  toSegCells, toSegRun, toUpperDisplay, SEG_D_BAR, SEG_D_LIT, SEG_D_MARK, foldToAscii,
} from '../src/constants/displayText.ts';
import { isDbUnitD, SEG_LOWER_D, statusParts, statusSegSlots, statusTags } from '../src/constants/statusField.ts';
import { segReadingCells } from '../src/constants/segField.ts';
import { formatReading, METER_SCALES } from '../src/constants/meters.ts';
import { classifyDabWindow, dabQualityLine, MER_STRONG, MER_WEAK } from '../src/utils/dabQuality.ts';
import { filterLabel } from '../src/utils/antennaBands.ts';
import { vtsIdText, vtsJoin, vtsStationText } from '../src/services/vtsLine.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++; console.error(`✗ ${what}\n    got  ${g}\n    want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);

/** How many dB units a text holds, by the rule itself (on the folded text, as the producers see it). */
const dbUnits = (t: string) => { const c = [...foldToAscii(t)]; return c.filter((_, i) => isDbUnitDAt(c, i)).length; };
const marks = (cells: string) => [...cells].filter(c => c === SEG_D_MARK).length;

/** A plain DSEG14 line, end to end: the cells mark each unit's d and nothing else; the two layers SegLowerDText draws
 *  are the same length as the line (same pens); the bar sits exactly where each d is; no other lower case. */
function plainLine(what: string, text: string, cells: string, units: number) {
  eq(`${what}: ${units} dB unit(s) found`, dbUnits(text), units);
  eq(`${what}: one lower-case d per unit`, marks(cells), units);
  ok(`${what}: nothing else in the cells is lower case`, ![...cells].some(c => c !== SEG_D_MARK && c !== c.toUpperCase()));
  const lit = segLitText(cells);
  const bar = segBarRuns(cells).map(r => r.text).join('');
  eq(`${what}: the lit layer has the same characters (pens)`, [...lit].length, [...cells].length);
  eq(`${what}: the bar layer has the same characters (pens)`, [...bar].length, [...cells].length);
  ok(`${what}: no 'd' left for DSEG14 to draw as D`, !lit.includes(SEG_D_MARK) && !bar.includes(SEG_D_MARK));
  const dAt = [...cells].flatMap((c, i) => (c === SEG_D_MARK ? [i] : []));
  eq(`${what}: lit 'J' on every d`, dAt.map(i => [...lit][i]), dAt.map(() => SEG_D_LIT));
  eq(`${what}: the bar '-' on every d`, dAt.map(i => [...bar][i]), dAt.map(() => SEG_D_BAR));
  eq(`${what}: a bar run per d, and only there`, segBarRuns(cells).filter(r => r.bar).length, units);
  eq(`${what}: SegLowerDText adds its overlay only when there is a d`, segHasLowerD(cells), units > 0);
}

// ── The rule ─────────────────────────────────────────────────────────────────────────────────────────────────────
const S = (t: string) => toSegCells(t);
eq('MER 12.3 dB', S('MER 12.3 dB'), 'MER!12.3!dB');
eq('GAIN 25.4dB — glued to its number', S('GAIN 25.4dB'), 'GAIN!25.4dB');
eq('-73 dBFS / -60dBm / 28 dBf / +6dB', ['-73 dBFS', '-60dBm', '28 dBf', '+6dB'].map(S), ['-73!dBFS', '-60dBM', '28!dBF', '+6dB']);
eq('dBu / dBuV', ['0 dBu', '40 dBuV'].map(S), ['0!dBU', '40!dBUV']);
eq('a dB at the very start, and after punctuation', ['dB', '(dB)', '12/dB', '·dB'].map(S).map(marks), [1, 1, 1, 1]);
// ★★ Never a D in a word.
const WORDS = ['DAB', 'DAB+', 'BBC', 'AUDIO', 'MODE', 'DX', 'SDR', 'ID', 'D', 'dab', 'audio', 'mode', 'sdr', 'id', 'Radio',
  'AdB', 'odB', 'db', 'DB', 'Db', 'dBx', 'dBFSx', 'dBmW', 'Ddb', 'kHz', 'MHz', 'Hz', 'DSP', 'RDS', 'RADIO D', 'd',
  'odd', 'dd', 'SHARED TUNER · FREE TO TUNE', 'Shared Tuner - Ask Before Tuning (You+2)', 'SHARED TUNER · ASK TO TUNE · 3/8 👤',
  'FM band-stop filter fitted', 'Outside antenna range', 'Direct Sample', 'searching', 'no station list'];
for (const w of WORDS) {
  eq(`"${w}" — no lower-case d`, marks(S(w)), 0);
  eq(`"${w}" — the VTS strip agrees`, toSegRun(w).cells.filter(c => c === SEG_D_MARK).length, 0);
  eq(`"${w}" — the status row agrees`, statusSegSlots(statusParts(w), segFieldCells).cells.includes(SEG_LOWER_D), false);
}
eq('DAB stays three capitals', segCellList(S('DAB')), ['D', 'A', 'B']);
eq('"DAB 12.3 dB" — only the unit', S('DAB 12.3 dB'), 'DAB!12.3!dB');

// ── ONE RULE, TWO READERS: displayText isDbUnitDAt ≡ statusField isDbUnitD ─────────────────────────────────────────
const CORPUS = [...WORDS, 'MER 12.3 dB', 'GAIN ↓ 25.4 dB (held)', '-73 dBFS', '-60dBm', '28 dBf', '0 dBu', '40 dBuV', 'dB',
  '(dB)', 'x dB', 'xdB', '5dBFS!', 'dBF', 'dBFSS', 'dB dB dB', 'SNR 12db', 'd B', '1.5 dB/div'];
for (const t of CORPUS) {
  const c = [...t];
  eq(`"${t}": the two copies of the rule agree`, c.map((_, i) => isDbUnitD(c, i)), c.map((_, i) => isDbUnitDAt(c, i)));
}

// ── Producer 1: the DAB meter (dabQuality → DabMeter → screenString('seg')) ──────────────────────────────────────
const W = { locked: true, fibRate: 1, fibNow: 1, sfTried: 10, sfOk: 10, mp2In: 0, mp2Bad: 0, frames: 10, erased: 0 };
const dabDetails = [MER_WEAK - 2.3, (MER_WEAK + MER_STRONG) / 2, 4.04].map(mer => classifyDabWindow({ ...W, mer }).detail);
ok('the DAB meter can say "MER … dB"', dabDetails.some(d => /^MER [\d.]+ dB$/.test(d ?? '')));
for (const d of dabDetails.filter((x): x is string => !!x && /dB/.test(x))) {
  plainLine(`DAB meter "${d}"`, d, screenString('seg', d), 1);
  plainLine(`DAB meter "${d}" (wrap)`, d, screenString('seg', d, { wrap: true }), 1);
  for (const lvl of [1, 2, 3] as const) {
    const line = dabQualityLine({ level: lvl, label: 'Weak signal', short: 'Weak', advice: 'May break up', detail: d } as any);
    plainLine(`DAB line "${line}"`, line, screenString('seg', line), 1);
  }
}
// ★ The other displays keep the text as written — dB is already dB.
eq('DAB meter on DOT: real lower case kept', screenString('dot', 'MER 12.3 dB'), 'MER 12.3 dB');
eq('DAB meter on Hyperlegible / Nixie: untouched', ['hyper', 'nixie'].map(s => screenString(s as any, 'MER 12.3 dB · DAB')),
   ['MER 12.3 dB · DAB', 'MER 12.3 dB · DAB']);

// ── Producer 2: the VTS strip (toSegRun — notices, gain, AGC, station text, server messages) ─────────────────────
const VTS = ['GAIN ↓ 25.4 dB', 'GAIN · 3.0 dB (held)', 'Overload: gain cut 6.0 dB', 'IF filter would cost 3.2 dB',
  'SNR 18 dB', 'MPX +1.2 dB', 'RDS deviation 2.1 kHz', 'Signal -73 dBFS', 'Pilot -22 dBm', 'Field 48 dBf',
  "This receiver's gain is at minimum — on a strong local signal that may be deliberate",
  'SDRplay AGC initialising: noise floor and signals will bounce until it settles (approx. 30 seconds)',
  'AGC: On  |  IF Filter: Auto', 'This profile is locked, keeping current', 'BBC Radio 4 - DAB+ 12D 225.648 MHz'];
for (const t of VTS) {
  const run = toSegRun(t);
  plainLine(`VTS "${t}"`, t, run.cells.join(''), dbUnits(t));
  ok(`VTS "${t}": one cell per mark (the window steps whole cells)`, run.cells.every(c => c.replace(/\.$/, '').length === 1));
}
eq('VTS: GAIN 25.4 dB → the d is its own cell before B', toSegRun('GAIN 25.4 dB').cells.slice(-2), [SEG_D_MARK, 'B']);
eq('VTS: units still go through the segments in capitals (MHZ, KHZ)', toSegRun('225.648 MHz 6 kHz').cells.join(''), '225.648!MHZ!6!KHZ');
eq('VTS: a station line built by vtsLine keeps DAB/BBC capitals',
   marks(toSegRun(vtsJoin(vtsIdText('C363', 'PI', true), vtsStationText('BBC R4', 'DAB+ 12D MODE AUDIO', true))).cells.join('')), 0);
eq('VTS on DOT: units keep their case (dB), words upper-cased', toUpperDisplay('gain 25.4dB dab'), 'GAIN 25.4dB DAB');

// ── Producer 3: the antenna notice (antennaBands → DisplayFontText → screenString('seg', …, { wrap })) ──────────
for (const name of ['FM band-stop', '20 dB pad', 'Notch -30dB', 'DAB']) {
  const label = filterLabel({ kind: 'bandstop', loHz: 88e6, hiHz: 108e6, unit: 'MHz', name } as any);
  for (const t of [`${label} fitted — reception here is deliberately reduced`, `${label} fitted`]) {
    plainLine(`antenna "${t}"`, t, screenString('seg', t, { wrap: true }), dbUnits(t));
  }
}

// ── Producer 4: the status row and its chips (StatusField / SegField: 'J-') ──────────────────────────────────────
const statusCells = (t: string) => statusSegSlots(statusParts(t), segFieldCells).cells;
for (const t of ['GAIN 25.4dB', 'GAIN ↓25.4dB', 'IF 1400k', '-73 dBFS', 'SNR 12 dB · 23k/s']) {
  const c = statusCells(t);
  eq(`status "${t}": one 'J-' per unit`, c.filter(x => x === SEG_LOWER_D).length, dbUnits(t));
  ok(`status "${t}": no plain d or D for a unit`, !c.includes(SEG_D_MARK));
}
const gp = statusGainParts('GAIN ↓ 25.4 dB');
ok('the gain reading splits to "25.4dB"', gp?.value === '25.4dB');
eq('…and its cells end J- B', statusCells(gp!.value).slice(-2), [SEG_LOWER_D, 'B']);
eq('segFieldCells (the mode box\'s / status run\'s cells): the d is J-', segFieldCells('12.3 dB'), ['1', '2.', '3', '', SEG_LOWER_D, 'B']);
eq('segFieldCells: a point after the d rides on its cell', segFieldCells('5dB.')[1], SEG_LOWER_D);
eq('the chips (NR NB AN) — no d', statusCells(statusTags(['NR', 'NB', 'AN'], 'seg').text).includes(SEG_LOWER_D), false);
eq('SEG_LOWER_D is the lit glyph + the bar', SEG_LOWER_D, SEG_D_LIT + SEG_D_BAR);

// ── Producer 5: the mode box readout (segReadingCells — a printed lower-case dB legend, never a 14-seg D) ─────────
for (const [unit, v] of [['dbfs', -73], ['dbf', 48], ['snr', 24], ['smeter', -40]] as const) {
  const text = formatReading(unit, v);
  const r = segReadingCells(text, unit);
  ok(`readout "${text}": the digits only — no letter cells`, r.cells.every(c => /^-?[0-9+]*$/.test(c)));
}
ok('readout -73dB lights the dB legend', segReadingCells(formatReading('dbfs', -73), 'dbfs').dB);
ok('readout S9+20 lights the dB legend', segReadingCells('S9+20', 'smeter').dB);

// ── Not on the VCR glass: the meter scale legends are Hyperlegible SVG text (EdgeMeter / ControlsBar FONT_HYPER) ─
ok('meter scale titles keep their real dB (Hyperlegible, not DSEG14)', METER_SCALES.dbfs.title === 'SIGNAL dB' && METER_SCALES.dbf.title === 'SIGNAL dBf');

console.log(`${fails ? '✗' : 'ok'}  faceplate VCR dB: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
