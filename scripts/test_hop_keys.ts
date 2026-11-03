/**
 * The tune keys in the modes whose decoder moves the radio itself — DAB, ISM, DIGI BAND (src/services/hopKeys.ts), and
 * the coalesced stepper they share (src/services/dabStepper.ts DabBlockStepper).
 *
 * ★★★ Stuart, 2026-11-03: "any modes that switch frequencies like DAB blocks do so ISM and All band digi spots need to have
 *     the drum replaced with the buttons like we do for DAB in the app." And the labels: the current block / band bold
 *     over the keys, where each key goes ON the key, "Block 11C. Previous 11B, next 11D" for a screen reader.
 *
 * Run: npx tsx scripts/test_hop_keys.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  hopKeysMode, hopVfoKeys, hopWrap, hopStrip, dabHopStrip, ismHopIndex, ismHopStrip, spotsHopIndex, spotsHopNext,
  spotsHopStrip, spotsShortName, HOP_SWEEP_PER_SEC,
} from '../src/services/hopKeys';
import { DabBlockStepper, DabAskGuard, DAB_ASK_HOLD_MS } from '../src/services/dabStepper';
import { DAB_BLOCKS } from '../src/services/dabBlocks';
import type { IsmCap, IsmReport } from '../src/services/ism';
import type { SpotBand } from '../src/utils/digiModes';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

// ── Which mode owns the keys, and the preference is only read ────────────────────────────────────────────────────────
eq('nothing on: no hop mode', hopKeysMode({ dab: false, ism: false, spotsBand: false, spotsOnActiveVfo: true }), null);
eq('DAB', hopKeysMode({ dab: true, ism: false, spotsBand: false, spotsOnActiveVfo: true }), 'dab');
eq('ISM', hopKeysMode({ dab: false, ism: true, spotsBand: false, spotsOnActiveVfo: true }), 'ism');
eq('DIGI BAND on the VFO the controls tune', hopKeysMode({ dab: false, ism: false, spotsBand: true, spotsOnActiveVfo: true }), 'spots');
eq('DIGI BAND on the OTHER VFO: that VFO tunes as ever', hopKeysMode({ dab: false, ism: false, spotsBand: true, spotsOnActiveVfo: false }), null);
eq('DAB wins (one whole-radio mode at a time)', hopKeysMode({ dab: true, ism: true, spotsBand: true, spotsOnActiveVfo: true }), 'dab');
for (const pref of [false, true]) {
  for (const m of [null, 'dab', 'ism', 'spots'] as const) {
    eq(`keys drawn: pref ${pref}, mode ${m}`, hopVfoKeys(pref, m), pref || m !== null);
  }
}
// ★ Leaving the mode hands the preference back untouched: the user's `false` is still `false`.
eq('preference false, mode left → drum again', hopVfoKeys(false, null), false);
eq('preference true, mode left → keys still', hopVfoKeys(true, null), true);
// ★ And SDRScreen never writes it from a hop mode: the only writer of lsv_vfo_keys is the user's own switch.
{
  const src = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  const writes = src.match(/setVfoKeys\(/g) ?? [];
  eq('setVfoKeys is called only by the preference switch and the stored value (2 places)', writes.length, 2);
  eq('lsv_vfo_keys is written in one place', (src.match(/setItem\('lsv_vfo_keys'/g) ?? []).length, 1);
  eq('the ControlsBar gets hopVfoKeys(vfoKeys, hopMode)', src.includes('vfoKeys={hopVfoKeys(vfoKeys, hopMode)}'), true);
  eq('held keys walk bands at the hop pace in ISM / DIGI BAND', /ismOnRef\.current \|\| \(spotsBandOnRef\.current[^\n]*return HOP_SWEEP_PER_SEC/.test(src), true);
  eq('the hop pace is DAB\'s', HOP_SWEEP_PER_SEC, 4);
}

// ── Wrap and the strip ─────────────────────────────────────────────────────────────────────────────────────────────
eq('wrap up from the top', hopWrap(5, 5), 0);
eq('wrap down from the bottom', hopWrap(-1, 5), 4);
eq('wrap, no list', hopWrap(3, 0), -1);
eq('strip: one band, no neighbours', hopStrip(['433'], 0, 'ISM', 'Band'), { legend: 'ISM', cur: '433', spoken: 'Band 433' });
eq('strip: off the list', hopStrip(['a', 'b'], -1, 'BAND', 'Band', '7.300'), { legend: 'BAND', cur: '7.300', spoken: 'Band 7.300' });
eq('strip: empty list', hopStrip([], 0, 'MUX', 'Block'), undefined);
// ★ The web's arrows clamp: the key at an end names nothing (its syncDialTips rule) and is read as "none".
eq('clamp: bottom', hopStrip(['a', 'b', 'c'], 0, 'BAND', 'Band', undefined, 'clamp'),
   { legend: 'BAND', cur: 'a', next: 'b', spoken: 'Band a. Previous none, next b' });
eq('clamp: top', hopStrip(['a', 'b', 'c'], 2, 'BAND', 'Band', undefined, 'clamp'),
   { legend: 'BAND', prev: 'b', cur: 'c', spoken: 'Band c. Previous b, next none' });
eq('clamp: middle as wrap', hopStrip(['a', 'b', 'c'], 1, 'BAND', 'Band', undefined, 'clamp')?.spoken, 'Band b. Previous a, next c');

// ── DAB: Stuart's own example, and the ends of Band III wrap (the app's DAB rule) ──────────────────────────────────────
{
  const names = DAB_BLOCKS.map((b) => b.name);
  const i11c = names.indexOf('11C');
  eq('DAB 11C', dabHopStrip(names, i11c),
     { legend: 'MUX', prev: '11B', cur: '11C', next: '11D', spoken: 'Block 11C. Previous 11B, next 11D' });
  eq('DAB 5A: previous wraps to the top of Band III', dabHopStrip(names, 0)?.prev, names[names.length - 1]);
  eq('DAB 13F: next wraps to 5A', dabHopStrip(names, names.length - 1)?.next, '5A');
  eq('DAB not known yet', dabHopStrip(names, -1), { legend: 'MUX', cur: '—', spoken: 'Block —' });
}

// ── ISM ────────────────────────────────────────────────────────────────────────────────────────────────────────────
const ISM: IsmCap = { ok: true, why: '', landing: '433', bands: [
  { id: '315', name: '315 MHz ISM', hz: 315e6 }, { id: '433', name: '433.92 MHz ISM', hz: 433.92e6 },
  { id: '868', name: '868 MHz ISM', hz: 868.3e6 }, { id: '915', name: '915 MHz ISM', hz: 915e6 },
] };
const rep = (band: string, extra: Partial<IsmReport> = {}): IsmReport => ({ type: 'ism', band, centre: 0, devices: [], ...extra });
eq('ISM index from the report', ismHopIndex(ISM, rep('868')), 2);
eq('ISM index, a band this radio does not list', ismHopIndex(ISM, rep('169')), -1);
eq('ISM 868', ismHopStrip(ISM, rep('868'), -1),
   { legend: 'ISM', prev: '433', cur: '868', next: '915', spoken: 'Band 868. Previous 433, next 915' });
eq('ISM 915: next wraps to 315', ismHopStrip(ISM, rep('915'), -1)?.next, '315');
eq('ISM 315: previous wraps to 915', ismHopStrip(ISM, rep('315'), -1)?.prev, '915');
eq('ISM: the pending band wins over the report (moves on the press)', ismHopStrip(ISM, rep('433'), 3)?.cur, '915');
eq('ISM: no report yet', ismHopStrip(ISM, null, -1), { legend: 'ISM', cur: '—', spoken: 'Band not known yet' });
eq('ISM: an exact frequency shows itself, its band\'s neighbours either side',
   ismHopStrip(ISM, rep('433', { custom: 1, centre: 433_420_000 }), -1),
   { legend: 'ISM', prev: '315', cur: '433.420', next: '868', spoken: 'Band 433.420. Previous 315, next 868' });
eq('ISM: only the bands the server lists (an RTL with no 315)',
   ismHopStrip({ ...ISM, bands: ISM.bands.slice(1) }, rep('433'), -1)?.prev, '915');
eq('ISM: no capability', ismHopStrip(undefined, rep('433'), -1), undefined);

// ── DIGI BAND ──────────────────────────────────────────────────────────────────────────────────────────────────────
const sb = (id: string, name: string, hz: number, lo: number, hi: number): SpotBand =>
  ({ id, name, hz, lo: hz - 10e3, hi: hz + 10e3, bandLo: lo, bandHi: hi, modes: ['FT8'] } as unknown as SpotBand);
const BANDS: SpotBand[] = [
  sb('60m', '60 m', 5_357_000, 5_250_000, 5_450_000),
  sb('40m', '40 m', 7_076_000, 7_000_000, 7_200_000),
  sb('20m', '20 m', 14_080_000, 14_000_000, 14_350_000),
];
eq('short name', spotsShortName(BANDS[1]), '40m');
eq('DIGI BAND 40m (names only — never "40 m FT8")', spotsHopStrip(BANDS, 7_074_000, -1),
   { legend: 'BAND', prev: '60m', cur: '40m', next: '20m', spoken: 'Band 40m. Previous 60m, next 20m' });
eq('DIGI BAND: pending wins', spotsHopStrip(BANDS, 7_074_000, 2)?.cur, '20m');
eq('DIGI BAND: between bands shows the dial', spotsHopStrip(BANDS, 10_136_000, -1),
   { legend: 'BAND', cur: '10.136', spoken: 'Band 10.136' });
eq('index on a band', spotsHopIndex(BANDS, 14_074_000), 2);
eq('index between', spotsHopIndex(BANDS, 10_136_000), -1);
eq('next up from 40m', spotsHopNext(BANDS, 7_074_000, 1), 2);
eq('next up from 20m wraps to 60m', spotsHopNext(BANDS, 14_074_000, 1), 0);
eq('next down from 60m wraps to 20m', spotsHopNext(BANDS, 5_357_000, -1), 2);
eq('from a burst in flight, not the dial', spotsHopNext(BANDS, 7_074_000, 1, 2), 0);
eq('between bands, up: the nearest above', spotsHopNext(BANDS, 10_136_000, 1), 2);
eq('between bands, down: the nearest below', spotsHopNext(BANDS, 10_136_000, -1), 1);
eq('above every band, up: wraps to the first', spotsHopNext(BANDS, 21_074_000, 1), 0);
eq('below every band, down: wraps to the last', spotsHopNext(BANDS, 3_573_000, -1), 2);
eq('no bands', spotsHopNext([], 7e6, 1), -1);

// ── The shared stepper as ISM and DIGI BAND drive it: label on each press, ONE send when settled, wraps ──────────────
{
  let now = 0;
  let queue: { at: number; fn: () => void; id: number }[] = [];
  let nextId = 1;
  const timers = {
    set: (fn: () => void, ms: number) => { const id = nextId++; queue.push({ at: now + ms, fn, id }); return id; },
    clear: (h: unknown) => { queue = queue.filter((t) => t.id !== h); },
  };
  const advance = (ms: number) => {
    now += ms;
    for (;;) {
      const due = queue.filter((t) => t.at <= now).sort((a, b) => a.at - b.at)[0];
      if (!due) break;
      queue = queue.filter((t) => t !== due); due.fn();
    }
  };
  const sent: number[] = [], shown: number[] = [];
  const st = new DabBlockStepper(ISM.bands.length, (i) => sent.push(i), (i) => shown.push(i), 450, timers);
  // 868 (2): > > — the label goes 915, 315 at once (wraps); the server hears 315 once.
  st.step(2, 1); st.step(2, 1);
  eq('ISM double press: shown on each press, wrapping', shown, [3, 0]);
  eq('ISM double press: nothing sent yet', sent, []);
  advance(450);
  eq('ISM double press: ONE send, the band stopped on', sent, [0]);
  // there and back: nothing sent
  sent.length = 0; shown.length = 0;
  st.step(0, -1); st.step(0, 1); advance(450);
  eq('ISM < >: the label moved and came back', shown, [3, 0]);
  eq('ISM < >: nothing sent (the band the server is on)', sent, []);

  // ★ The ask guard (display only): a stale report naming the band we left is held off; a refusal snaps back after
  //   DAB_ASK_HOLD_MS; somebody else's band is adopted at once.
  const g = new DabAskGuard();
  g.sent(0, 2, 1000);
  eq('stale report (the band we left) held off', g.accept(2, 1500), false);
  eq('the band we asked for is believed', g.accept(0, 1600), true);
  g.sent(3, 0, 2000);
  eq('refused: still held just before the bound', g.accept(0, 2000 + DAB_ASK_HOLD_MS - 1), false);
  eq('refused: the server wins at the bound (the label snaps back)', g.accept(0, 2000 + DAB_ASK_HOLD_MS), true);
  g.sent(3, 0, 10_000);
  eq('a THIRD band (someone else moved the shared dial): adopted at once', g.accept(1, 10_100), true);
}

console.log(`hop keys: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
