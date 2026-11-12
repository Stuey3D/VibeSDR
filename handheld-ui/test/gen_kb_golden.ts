/**
 * GOLDEN DATA for the handheld controller-keyboard port (handheld-ui/src/kb): scripted and seeded-random action
 * sequences run through the APP's own TS — src/utils/kbLayouts.ts and src/utils/controllerKeyboard.ts, unmodified —
 * written as one text file of inputs and the full state after every one. kb_golden_test.cpp replays the inputs through
 * the C++ and must produce the identical lines.
 *
 * The SESSION here (open / move / press / back / edit / submit with a refusal) is web/client/src/padKeyboard.ts's own
 * logic with the DOM taken out — keysFor, render's focus-follows-the-id, apply's same-object check, padKbSubmit,
 * padKbBack, padKbPress, padKbEdit — line for line; padKeyboard.ts itself needs a document to run.
 *
 * Lines (hex = UTF-8 bytes in hex; doubles as their 16-hex-digit IEEE bits):
 *   O mode unit|- caret|- unitSettable secure valueHex          open a session
 *   A move dir | A press | A back | A edit act | A submit       an input
 *   K keys                                                      the session's keys, whenever they change
 *   S state                                                     the session after every O / A
 *   KS mode layer unitLocked unitLabelHex|- revealLabelHex|- keys   a layout, every combination
 *   MV cur dir result                                           kbMove on the last KS, every key × every way
 *   PN hex bits · FN bits dec hex · IN mode valueHex unit|- caret|- state   kbParseNumber / kbFormatNumber / kbInit
 *   R mode valueHex unit|- caret|- · RA act [hex] · RS replaced state       the bare reducer, any character
 * Run: node --no-warnings --import ./ts_resolve.mjs gen_kb_golden.ts <outFile>
 */
import { writeFileSync } from 'node:fs';
import { kbKeys, kbMove, kbStartKey, layoutFor, type KbMode, type KbKey, type KbLayer, type KbKeysOpts } from '../../src/utils/kbLayouts.ts';
import { kbInit, kbReduce, kbDisplay, kbParseNumber, kbFormatNumber, type KbState, type KbUnit, type KbAction } from '../../src/utils/controllerKeyboard.ts';
import type { Dir } from '../../src/utils/gamepadNav.ts';

const outFile = process.argv[2] ?? 'kb_golden.txt';
const lines: string[] = [];
const hex = (s: string) => Buffer.from(s, 'utf8').toString('hex');
const bits = (n: number) => { const b = Buffer.alloc(8); b.writeDoubleLE(n); return b.readBigUInt64LE().toString(16).padStart(16, '0'); };
const opt = (v: string | number | undefined) => (v === undefined ? '-' : String(v));

const MODES: KbMode[] = ['frequency', 'pin', 'search', 'text', 'password', 'url'];
const LAYERS: KbLayer[] = ['lower', 'upper', 'caps', 'symbols', 'accents', 'accentsUp'];
const DIRS: Dir[] = ['up', 'down', 'left', 'right'];

const keysSer = (keys: KbKey[]) => keys.map((k) =>
  [k.id, hex(k.label), k.act.t, k.act.t === 'ch' ? hex(k.act.ch) : '', bits(k.x), bits(k.y), bits(k.w), bits(k.h)].join(',')).join(';');
const stSer = (s: KbState) =>
  `v=${hex(s.value)} c=${s.caret} L=${s.layer} m=${s.mode} u=${s.unit ?? '-'} r=${s.reveal ? 1 : 0}`;

// ── The session (padKeyboard.ts without the DOM) ────────────────────────────────────────────────────────────────────

interface Sess { mode: KbMode; unitSettable: boolean; secure: boolean; st: KbState; keys: KbKey[]; focus: number; err: string; orig: string }
let s: Sess | null = null;
let last: Sess | null = null;   // stays readable after a close, as the C++ state() does
let lastKeys = '';

function keysFor(x: Sess): KbKey[] {
  return kbKeys(x.mode, x.st.layer, {
    unitLocked: !x.unitSettable,
    unitLabel: (x.st.unit ?? 'mhz') === 'mhz' ? 'kHz' : 'MHz',
    revealLabel: x.st.reveal ? 'HIDE' : 'SHOW',
  });
}
function render() {
  if (!s) return;
  const was = s.keys[s.focus]?.id;
  s.keys = keysFor(s);
  const same = was ? s.keys.findIndex((k) => k.id === was) : -1;
  s.focus = same >= 0 ? same : Math.max(0, Math.min(s.focus, s.keys.length - 1));
}
function open(mode: KbMode, value: string, unit: KbUnit | undefined, caret: number | undefined, unitSettable: boolean, secure: boolean) {
  const st = kbInit(mode, value, { unit, caret });
  s = { mode, unitSettable, secure, st, keys: [], focus: 0, err: '', orig: value };
  s.keys = keysFor(s);
  s.focus = Math.max(0, s.keys.findIndex((k) => k.id === kbStartKey(mode)));
  last = s;
}
type Out = 'none' | 'changed' | 'submit' | 'refused' | 'cancel';
function apply(a: KbAction): Out {
  if (!s) return 'none';
  const prev = s.st;
  const next = kbReduce(prev, a);
  if (next === prev) return 'none';
  s.st = next;
  s.err = '';
  render();
  return 'changed';
}
/** The test's field commit: a frequency out of 10 kHz – 2 GHz, a PIN under 4 digits, or the text "bad" is refused. */
function commit(st: KbState): string {
  if (st.mode === 'frequency') {
    const u = st.unit ?? 'mhz';
    const hz = kbParseNumber(st.value) * (u === 'mhz' ? 1e6 : u === 'khz' ? 1e3 : 1);
    if (!Number.isFinite(hz) || hz < 10e3 || hz > 2e9) return `Out of range: ${st.value} ${u} — 10 kHz–2 GHz`;
    return '';
  }
  if (st.mode === 'pin') return st.value.length < 4 ? 'PIN too short' : '';
  return st.value === 'bad' ? 'Nope ✗' : '';
}
function submit(): Out {
  if (!s) return 'none';
  const refused = commit(s.st);
  if (refused) { s.err = refused; render(); return 'refused'; }
  s = null;
  return 'submit';
}
function back(): Out {
  if (!s) return 'none';
  if (s.st.layer !== 'lower') return apply({ t: 'abc' });
  s = null;
  return 'cancel';
}
function move(d: Dir): Out {
  if (!s) return 'none';
  const n = kbMove(s.keys, s.focus, d);
  if (n >= 0 && n !== s.focus) { s.focus = n; render(); return 'changed'; }
  return 'none';
}
function press(): Out {
  if (!s) return 'none';
  const k = s.keys[s.focus];
  if (!k) return 'none';
  if (k.act.t === 'enter') return submit();
  if (k.act.t === 'back') return back();
  return apply(k.act as KbAction);
}
function edit(a: 'bs' | 'space' | 'shift' | 'symbols' | 'unit' | 'left' | 'right'): Out {
  if (!s) return 'none';
  if (a === 'symbols') return apply({ t: s.st.layer === 'symbols' ? 'abc' : 'symbols' });
  return apply({ t: a } as KbAction);
}

function emitState(out: Out) {
  const x = last!;
  const ks = keysSer(x.keys);
  if (ks !== lastKeys) { lines.push('K ' + ks); lastKeys = ks; }
  const shown = x.mode === 'pin' && x.secure && !x.st.reveal ? '•'.repeat(x.st.value.length) : kbDisplay(x.st);
  const unitText = x.mode === 'frequency' ? ((x.st.unit ?? 'mhz') === 'mhz' ? 'MHz' : x.st.unit === 'khz' ? 'kHz' : 'Hz') : '';
  const lit = x.keys.map((k) => (k.act.t === 'enter'
    || (k.act.t === 'shift' && (x.st.layer === 'upper' || x.st.layer === 'caps' || x.st.layer === 'accentsUp'))
    || (k.act.t === 'reveal' && x.st.reveal) ? '1' : '0')).join('');
  lines.push(`S open=${s ? 1 : 0} out=${out} ${stSer(x.st)} f=${x.focus} fid=${x.keys[x.focus]?.id ?? ''} err=${hex(x.err)} ` +
    `disp=${hex(shown)} bef=${hex(shown.slice(0, x.st.caret))} aft=${hex(shown.slice(x.st.caret))} ut=${unitText} lit=${lit} ` +
    `orig=${hex(x.orig)} lay=${layoutFor(x.mode)}`);
}

let nSessionSteps = 0;
function doOpen(mode: KbMode, value: string, unit?: KbUnit, caret?: number, unitSettable = false, secure = false) {
  lines.push(`O ${mode} ${opt(unit)} ${opt(caret)} ${unitSettable ? 1 : 0} ${secure ? 1 : 0} ${hex(value)}`);
  open(mode, value, unit, caret, unitSettable, secure);
  emitState('none');
}
type Act = ['move', Dir] | ['press'] | ['back'] | ['edit', 'bs' | 'space' | 'shift' | 'symbols' | 'unit' | 'left' | 'right'] | ['submit'];
function doAct(a: Act) {
  lines.push('A ' + a.join(' '));
  nSessionSteps++;
  const out = a[0] === 'move' ? move(a[1]) : a[0] === 'press' ? press() : a[0] === 'back' ? back() : a[0] === 'edit' ? edit(a[1]) : submit();
  emitState(out);
}

// ── 1. Every layout: keys, and kbMove from every key every way (and from out of range) ──────────────────────────────

let nLayouts = 0, nMoves = 0;
const optSets: KbKeysOpts[] = [{}, { unitLocked: true }, { unitLabel: 'kHz', revealLabel: 'HIDE' }, { unitLabel: 'MHz', revealLabel: 'SHOW' }];
for (const mode of MODES) for (const layer of LAYERS) for (const o of optSets) {
  const keys = kbKeys(mode, layer, o);
  nLayouts++;
  lines.push(`KS ${mode} ${layer} ${o.unitLocked ? 1 : 0} ${o.unitLabel === undefined ? '-' : hex(o.unitLabel)} ` +
    `${o.revealLabel === undefined ? '-' : hex(o.revealLabel)} ${keysSer(keys)}`);
  for (let cur = -1; cur <= keys.length; cur++) for (const d of DIRS) { lines.push(`MV ${cur} ${d} ${kbMove(keys, cur, d)}`); nMoves++; }
}
lines.push('SK ' + MODES.map((m) => kbStartKey(m) + ':' + layoutFor(m)).join(','));

// ── 2. Numbers: our parse, toFixed's rounding (ties go UP — the spec's "larger n"), init ─────────────────────────────

let nNum = 0;
const parseIn = ['', '.', '0', '00', '5.', '.5', '9.41', '9.410', '1.2.3', '12a', '-1', '1e3', ' 1', '007.0700', '99999.999999',
  '123456789012345678901234567890', '0.1', '0.0000625', '7.8125', '62.5', '1000000', '2147483648.5', '0.000001', '٣'];
for (const v of parseIn) { lines.push(`PN ${hex(v)} ${bits(kbParseNumber(v))}`); nNum++; }
const fmtIn: number[] = [0, -0, 1, 0.5, 1.5, 2.5, -2.5, 0.0625, 0.0078125, 1.0005, 1.0015, 9.41, 9410, 0.00941, 123.456, 1e-7, 5e-7,
  4.9999995e-7, 0.1 + 0.2, 1 / 3, 2 / 3, 99999.9999995, 1e20, 1e21, 1.2345678901234567e23, -1e22, 123456789.123456789,
  0.000999999, 7812.5 / 1e6, 62.5 / 1e3, 0.0000625 * 1e6 / 1e3, NaN, Infinity, -Infinity, 1e-300, 5e-324, 1.7976931348623157e308];
for (const n of fmtIn) for (const dec of [0, 3, 6]) { lines.push(`FN ${bits(n)} ${dec} ${hex(kbFormatNumber(n, dec))}`); nNum++; }
// seeded random values in kHz / MHz range, every decimals
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
for (let i = 0; i < 3000; i++) {
  const k = Math.floor(rnd() * 2_000_000_000 * (rnd() < 0.5 ? 1 : 0.001));
  const n = (k + Math.floor(rnd() * 8) / 8) / (rnd() < 0.5 ? 1e3 : 1e6);
  for (const dec of [3, 6]) { lines.push(`FN ${bits(n)} ${dec} ${hex(kbFormatNumber(n, dec))}`); nNum++; }
}
const initIn: Array<[KbMode, string, KbUnit | undefined, number | undefined]> = [];
const initVals = ['', '9,410', '9.41 MHz', '1.2.3', '12ab34', '1234567890123456', 'héllo wörld', 'a😀b', '  sp  ', 'http://x.y/z'];
for (const m of MODES) for (const v of initVals) for (const u of [undefined, 'khz', 'hz'] as const) for (const c of [undefined, -3, 0, 2, 99])
  initIn.push([m, v, u, c]);
for (const [m, v, u, c] of initIn) { lines.push(`IN ${m} ${hex(v)} ${opt(u)} ${opt(c)} ${stSer(kbInit(m, v, { unit: u, caret: c }))}`); nNum++; }

// ── 3. The bare reducer: every action, any character (beyond the keys), every mode ──────────────────────────────────

let nRed = 0;
const redActs: KbAction['t'][] = ['ch', 'bs', 'left', 'right', 'shift', 'symbols', 'accents', 'abc', 'space', 'clear', 'unit', 'reveal'];
const odd = ['a', 'Z', '1', '0', '.', ',', ' ', '\t', ' ', 'ñ', 'ß', 'Ω', 'ж', '😀', '日', '-', '/', 'ab', '١', '　', 'é', '5'];
for (const m of MODES) for (const [v0, u0] of [['', undefined], ['9.41', 'mhz'], ['7812.5', 'khz'], ['123', 'hz'], ['x😀y', undefined], ['y'.repeat(254), undefined]] as const) {
  lines.push(`R ${m} ${hex(v0)} ${opt(u0)} -`);
  let st = kbInit(m, v0, { unit: u0 as KbUnit | undefined });
  for (let i = 0; i < 300; i++) {
    const t = redActs[Math.floor(rnd() * redActs.length)];
    const a: KbAction = t === 'ch' ? { t, ch: odd[Math.floor(rnd() * odd.length)] } : ({ t } as KbAction);
    lines.push(`RA ${t}${t === 'ch' ? ' ' + hex((a as { ch: string }).ch) : ''}`);
    const nx = kbReduce(st, a);
    lines.push(`RS ${nx === st ? 0 : 1} ${stSer(nx)} d=${hex(kbDisplay(nx))}`);
    st = nx;
    nRed++;
  }
}

// ── 4. Sessions: scripted, then seeded random walks across every mode ───────────────────────────────────────────────

// Scripted: the frequency keypad as the web shows it ("9.410 MHz"), unit switches both ways, a refusal, a re-entry.
doOpen('frequency', '9.410', 'mhz', undefined, true, false);
for (const a of [['edit', 'unit'], ['edit', 'unit'], ['edit', 'unit'], ['edit', 'left'], ['edit', 'left'], ['edit', 'bs'],
  ['press'], ['move', 'right'], ['move', 'right'], ['move', 'right'], ['press'], ['press'], ['move', 'up'], ['move', 'up'],
  ['press'], ['move', 'down'], ['press'], ['submit'], ['edit', 'right'], ['edit', 'right'], ['edit', 'right'], ['edit', 'right'],
  ['edit', 'bs'], ['edit', 'bs'], ['edit', 'bs'], ['edit', 'bs'], ['edit', 'bs'], ['edit', 'bs'], ['edit', 'bs'], ['submit'],
  ['move', 'left'], ['move', 'left'], ['move', 'left'], ['move', 'left'], ['move', 'down'], ['move', 'down'], ['move', 'down'],
  ['move', 'down'], ['press'], ['press'], ['move', 'up'], ['press'], ['submit']] as Act[]) doAct(a);
// Frequency with the unit LOCKED (no unit key — but the shoulder still sends it, as the TS does).
doOpen('frequency', '1,215', 'khz', 2, false, false);
for (const a of [['edit', 'unit'], ['edit', 'unit'], ['move', 'right'], ['move', 'right'], ['move', 'down'], ['move', 'down'],
  ['press'], ['back']] as Act[]) doAct(a);
// Shift / caps / accents / symbols cycling with Back one layer at a time.
doOpen('text', 'Hi', undefined, 1, false, false);
for (const a of [['edit', 'shift'], ['press'], ['press'], ['edit', 'shift'], ['edit', 'shift'], ['press'], ['press'],
  ['edit', 'shift'], ['move', 'down'], ['move', 'down'], ['move', 'down'], ['move', 'right'], ['move', 'right'], ['press'],
  ['move', 'up'], ['press'], ['edit', 'shift'], ['press'], ['back'], ['edit', 'symbols'], ['move', 'up'], ['press'],
  ['edit', 'symbols'], ['edit', 'symbols'], ['back'], ['back']] as Act[]) doAct(a);
// Password: SHOW / HIDE, dots, and a secure PIN.
doOpen('password', 'pässwörd', undefined, undefined, false, true);
for (const a of [['move', 'down'], ['move', 'down'], ['move', 'down'], ['move', 'right'], ['move', 'right'], ['press'],
  ['edit', 'left'], ['edit', 'bs'], ['press'], ['edit', 'space'], ['submit']] as Act[]) doAct(a);
doOpen('pin', '12', undefined, undefined, false, true);
for (const a of [['press'], ['submit'], ['press'], ['press'], ['edit', 'left'], ['submit']] as Act[]) doAct(a);

// Random walks.
const openings: Array<[KbMode, string, KbUnit | undefined, number | undefined, boolean, boolean]> = [
  ['frequency', '9.410', 'mhz', undefined, true, false], ['frequency', '', undefined, undefined, true, false],
  ['frequency', '7.8125', 'khz', undefined, true, false], ['frequency', '0.0000625', 'mhz', 3, true, false],
  ['frequency', '1.2.3', 'mhz', 1, true, false], ['frequency', '99999.999999', 'mhz', 0, true, false],
  ['frequency', '1215', 'khz', undefined, false, false], ['frequency', '1234567890', 'hz', undefined, true, false],
  ['frequency', '62.5', 'khz', undefined, true, false],
  ['pin', '', undefined, undefined, false, false], ['pin', '98765', undefined, 2, false, true],
  ['search', 'radio', undefined, undefined, false, false], ['text', '', undefined, undefined, false, false],
  ['text', 'a😀b', undefined, 2, false, false], ['text', 'x'.repeat(250), undefined, 100, false, false],
  ['password', 'secret', undefined, undefined, false, true], ['password', '', undefined, undefined, false, true],
  ['url', 'http://host', undefined, 4, false, false], ['url', '', undefined, undefined, false, false],
];
const EDITS = ['bs', 'space', 'shift', 'symbols', 'unit', 'left', 'right'] as const;
for (let round = 0; round < 30; round++) for (const op of openings) {
  doOpen(...op);
  for (let i = 0; i < 160; i++) {
    if (!s) { if (rnd() < 0.2) break; doOpen(...op); continue; }
    const r = rnd();
    const a: Act = r < 0.42 ? ['move', DIRS[Math.floor(rnd() * 4)]] : r < 0.74 ? ['press'] : r < 0.9 ? ['edit', EDITS[Math.floor(rnd() * EDITS.length)]]
      : r < 0.96 ? ['back'] : ['submit'];
    doAct(a);
  }
}
// An input with nothing open does nothing (and the state stays the last session's).
if (s) doAct(['back']);
doAct(['press']); doAct(['move', 'up']); doAct(['edit', 'bs']); doAct(['submit']); doAct(['back']);

writeFileSync(outFile, lines.join('\n') + '\n');
console.log(`kb golden: ${nLayouts} layouts, ${nMoves} moves, ${nNum} number/init cases, ${nRed} reducer steps, ` +
  `${nSessionSteps} session steps → ${outFile}`);
