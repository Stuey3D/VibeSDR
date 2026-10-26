/**
 * The app's CW box (src/services/cwList.ts) against the web client's CW panel — ONE RULE, TWO READERS (2026-10-25):
 *   • the list's order — the web's renderCw sort, cut out of main.ts and run on the same rows;
 *   • the confidence colours — read out of the web page's CSS (#cwList .cwr.q-<q> .tx);
 *   • the wire's sanitiser, the spots kept, age / frequency / SNR text, the skimmer's drawn passband, the scroll pin.
 *
 * Run: node --no-warnings scripts/test_cw_list.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  CW_Q_COLOURS, cwAge, cwDrawnPassband, cwFreqText, cwHintText, cwPinFromScroll, cwPinRestore, cwSnrText, cwWidthOf,
  mergeCwSpot, parseCwSignals, parseCwSpot, sortCwSignals, type CwSignal, type CwSpot,
} from '../src/services/cwList.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

const sig = (o: Partial<CwSignal>): CwSignal => ({
  id: 0, hz: 0, audio: 0, wpm: 20, snr: 10, q: 'high', active: true, last: 0, first: 0, call: '', country: '', text: '', ...o,
});

// ── 1. The order: lowest frequency first, ties by id; audio-only rows by their tone ──
{
  const rows = [sig({ id: 3, hz: 7_031_000 }), sig({ id: 1, hz: 7_012_500 }), sig({ id: 2, hz: 7_031_000 }), sig({ id: 9, hz: 7_001_000 })];
  eq('skimmer rows: lowest RF first, ties by id', sortCwSignals(rows).map((s) => s.id), [9, 1, 2, 3]);
  const one = [sig({ id: 5, audio: 900 }), sig({ id: 4, audio: 600 }), sig({ id: 6, audio: 600 })];
  eq('one-signal rows: by tone', sortCwSignals(one).map((s) => s.id), [4, 6, 5]);
  eq('the input is left alone', rows.map((s) => s.id), [3, 1, 2, 9]);
  // The web's own comparator, cut out of main.ts — the same order on a mixed set.
  const main = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
  const m = main.match(/const sigs = \[\.\.\.\(l\?\.signals \?\? \[\]\)\]\.sort\((\(a, b\) => [^\n]*?)\);\n/);
  if (!m) { fails++; console.error('FAIL the web renderCw sort was not found in main.ts'); }
  else {
    const webCmp = new Function(`return ${m[1]};`)() as (a: CwSignal, b: CwSignal) => number;
    const mixed = [sig({ id: 7, hz: 14_040_000 }), sig({ id: 2, hz: 14_011_000 }), sig({ id: 4, hz: 14_040_000 }), sig({ id: 1, hz: 14_025_300 })];
    eq('the web sort and the app sort agree', sortCwSignals(mixed).map((s) => s.id), [...mixed].sort(webCmp).map((s) => s.id));
  }
}

// ── 2. The colours are the web page's ──
{
  const html = readFileSync(new URL('../web/client/index.html', import.meta.url), 'utf8');
  for (const q of ['high', 'medium', 'low', 'poor'] as const) {
    const m = html.match(new RegExp(`#cwList \\.cwr\\.q-${q} \\.tx \\{ color: (#[0-9A-Fa-f]{6}); \\}`));
    eq(`colour ${q} matches the web CSS`, m ? m[1].toUpperCase() : null, CW_Q_COLOURS[q].toUpperCase());
  }
  eq('the health pill colours (Stuart)', CW_Q_COLOURS, { poor: '#FF4B4B', low: '#FF8A3D', medium: '#E8C547', high: '#5BE36B' });
}

// ── 3. The wire ──
{
  const l = parseCwSignals({ skimmer: true, centre: 7_020_000, width: 48000, primary: 0,
    signals: [{ id: 1, hz: 7_012_500, audio: 0, wpm: 22.4, snr: 14, q: 'medium', active: true, last: 5, first: 1,
                call: 'G4ABC', country: 'England', text: 'CQ CQ DE G4ABC', extra: 'dropped' },
              { id: 'x', q: 'bogus', text: 42 }] });
  eq('a list parses', l?.skimmer, true);
  eq('fields kept, extras dropped', Object.keys(l!.signals[0]).sort(),
     ['active', 'audio', 'call', 'country', 'first', 'hz', 'id', 'last', 'q', 'snr', 'text', 'wpm']);
  eq('bad fields become zero / empty / poor', [l!.signals[1].id, l!.signals[1].q, l!.signals[1].text], [0, 'poor', '']);
  eq('not a list', parseCwSignals({ signals: 'no' }), null);
  eq('80 rows at most', parseCwSignals({ signals: Array.from({ length: 200 }, (_, i) => ({ id: i })) })!.signals.length, 80);
  eq('long text cut to 160', parseCwSignals({ signals: [{ text: 'E'.repeat(400) }] })!.signals[0].text.length, 160);
  eq('a spot without a call is dropped', parseCwSpot({ frequency: 7e6 }, 1), null);
  eq('a spot keeps its fields', parseCwSpot({ callsign: 'DL1XYZ', frequency: 7_011_000, wpm: 25, snr: 9, how: 'CQ', country: 'Germany' }, 99),
     { callsign: 'DL1XYZ', frequency: 7_011_000, audio: 0, wpm: 25, snr: 9, how: 'CQ', country: 'Germany', timestamp: 99 });
}

// ── 4. Spots: one row per call, newest first, capped ──
{
  const sp = (c: string, t: number): CwSpot => ({ callsign: c, frequency: 7e6, audio: 0, wpm: 20, snr: 5, how: 'CQ', country: '', timestamp: t });
  let s: CwSpot[] = [];
  s = mergeCwSpot(s, sp('A', 1)); s = mergeCwSpot(s, sp('B', 2)); s = mergeCwSpot(s, sp('A', 3));
  eq('heard again moves to the top, once', s.map((x) => `${x.callsign}${x.timestamp}`), ['A3', 'B2']);
  let big: CwSpot[] = [];
  for (let i = 0; i < 250; i++) big = mergeCwSpot(big, sp(`C${i}`, i));
  eq('capped at 200', big.length, 200);
  eq('the newest kept', big[0].callsign, 'C249');
}

// ── 5. Text ──
eq('age seconds', cwAge(1000, 13_400), '12s');
eq('age minutes', cwAge(0, 4 * 60_000), '4m');
eq('age hours', cwAge(0, 2 * 3600_000), '2h');
eq('age never negative', cwAge(5000, 1000), '0s');
eq('RF in kHz', cwFreqText(7_012_480, 700), '7012.48');
eq('audio tone', cwFreqText(0, 713.6), '714 Hz');
eq('snr plus', cwSnrText(12.4), '+12 dB');
eq('snr minus', cwSnrText(-3.2), '-3 dB');
eq('width: one of the three', [cwWidthOf(24000), cwWidthOf('96000'), cwWidthOf(5), cwWidthOf(undefined)], [24000, 96000, 48000, 48000]);
eq('the skimmer hint names half the segment', cwHintText(true, 96000).startsWith('Every CW signal 48 kHz either side'), true);

// ── 6. The VFO drawn while the skimmer runs is its segment ──
eq('skimming: ±width/2', cwDrawnPassband(true, 96000, 300, 900), { low: -48000, high: 48000 });
eq('not skimming: the listener’s own', cwDrawnPassband(false, 96000, 300, 900), { low: 300, high: 900 });

// ── 7. The scroll pin ──
{
  eq('at the top: not pinned', cwPinFromScroll(0, 400), null);
  const pin = cwPinFromScroll(250, 400);          // heading 150 pt below the view's top
  eq('scrolled: the heading’s place in the view', pin, 150);
  eq('three rows (60 pt) arrive above: follow the heading', cwPinRestore(pin, 460, 250), 310);
  eq('nothing moved: leave the view', cwPinRestore(pin, 400, 250), null);
  eq('not pinned: leave the view', cwPinRestore(null, 900, 0), null);
  eq('never above the top', cwPinRestore(300, 100, 50), 0);
}

console.log(`cw list: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
