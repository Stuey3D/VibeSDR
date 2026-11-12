/**
 * GOLDEN DATA for the handheld VTS port (handheld-ui/src/vts_core.h): the APP's own TS — src/services/vtsLine.ts,
 * src/services/psStabiliser.ts, src/utils/cleanLines.ts, src/services/dabTypes.ts, src/services/rdsCountry.ts,
 * src/constants/bandPlan.ts + src/services/stations.ts, unmodified — run over scripted and seeded-random inputs, written
 * as one text file of inputs and outputs. vts_golden_test.cpp replays the inputs through the C++ and must produce the
 * identical outputs. Strings are UTF-8 hex ('-' = empty); numbers decimal.
 *
 *   L id label name text | line | segs          vtsLine / vtsLineSegments (segs = kind:hex,…)
 *   H value | hex                               vtsHex
 *   M seg,seg,… | merged                        mergeSegments
 *   PR                                          PsStabiliser.reset()
 *   PF key ps now | shown rotating settled due  PsStabiliser.feed (due = nextDueIn, -1 = null)
 *   PT now | shown rotating settled due         PsStabiliser.tick
 *   B hz region | key primary secondary         the band announcement (SDRScreen showBandNotif's text, vtsCheck's order)
 *   E ecc pi | iso                              eccPiToIso
 *   C max s | out   ·   D max s | out           cleanKeepSpacing / dabSafeText
 * Run: node --no-warnings --import ./ts_resolve.mjs --import ./ts_transform.mjs gen_vts_golden.ts <outFile>
 */
import { writeFileSync } from 'node:fs';
import { vtsHex, vtsLine, vtsLineSegments, type VtsIdLabel } from '../../src/services/vtsLine.ts';
import { PsStabiliser, mergeSegments } from '../../src/services/psStabiliser.ts';
import { cleanKeepSpacing } from '../../src/utils/cleanLines.ts';
import { dabSafeText } from '../../src/services/dabTypes.ts';
import { eccPiToIso } from '../../src/services/rdsCountry.ts';
import { getBandsAtRegion, type Band } from '../../src/constants/bandPlan.ts';
import { fmtBandFreq } from '../../src/services/stations.ts';

const outFile = process.argv[2] ?? 'vts_golden.txt';
const L: string[] = [];
const hex = (s: string | undefined) => (s ? Buffer.from(s, 'utf8').toString('hex') : '-');
let seed = 0x5eed1234;
const rnd = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296; };
const pick = <T,>(a: T[]) => a[Math.floor(rnd() * a.length)];

// ── the line ──
const IDS = ['', 'C363', ' C201 ', 'E2A1'];
const NAMES = ['', 'BBC R6', 'Radio 2', '  ', 'NRJ', 'Ö3', 'BBC Radio Northampton'];
const TEXTS = ['', 'BBC R6', 'A Tribe Called Quest - Can I Kick it?', ' Now: Nirvana ', 'Ö3 Hitradio: Ärzte'];
for (const id of IDS) for (const label of ['PI', 'SId'] as VtsIdLabel[]) for (const name of NAMES) for (const text of TEXTS) {
  const segs = vtsLineSegments({ id, idLabel: label, name, text });
  L.push(`L ${hex(id)} ${label} ${hex(name)} ${hex(text)} | ${hex(vtsLine({ id, idLabel: label, name, text }))} | `
    + (segs.map((s) => `${s.kind}:${hex(s.s)}`).join(',') || '-'));
}
for (const v of [-1, 0, 1, 0xC363, 0xFFFF, 0x10000, 0xFFFFFFFF, 0x100000000, 51.4, 12.5]) L.push(`H ${v} | ${hex(vtsHex(v))}`);

// ── mergeSegments ──
const MERGES: string[][] = [
  ['RADIO MA', 'ADIO MAS', 'DIO MASS', 'IO MASSA'], ['UMUARAMA', 'ALINE', 'RADIO'], ['UMUARAMA', 'MASSA'],
  ['ing pump', 'umpkins', 'ins cher'], ['ABC', 'ABC'], ['ABCDEF', 'CDE'], ['', 'X', ''], ['LAZER', '99.3', 'PLAYING'],
  ['Ärzte H', 'te Hits', 'Hits 24'], ['WLZX-FM', 'ZX-FM Le', 'Lenny Kr'],
];
for (let i = 0; i < 60; i++) {
  const base = pick(['RADIO MASSA FM ', 'LAZER 99.3 PLAYING NIRVANA IN BLOOM ', 'HITRADIO OE3 ', 'ROCK 102 WAQY ']);
  const w = 8, segs: string[] = [];
  let p = Math.floor(rnd() * base.length);
  for (let k = 0; k < 2 + Math.floor(rnd() * 8); k++) { segs.push((base + base).slice(p, p + w).trim()); p = (p + 1 + Math.floor(rnd() * 3)) % base.length; }
  MERGES.push(segs);
}
for (const m of MERGES) L.push(`M ${m.map(hex).join(',')} | ${hex(mergeSegments(m))}`);

// ── the PS stabiliser ──
const ps = new PsStabiliser();
const out = (shown: string, now: number) => {
  const due = ps.nextDueIn(now);
  return `${hex(shown)} ${ps.rotating ? 1 : 0} ${ps.settled ? 1 : 0} ${due == null ? -1 : due}`;
};
/** A feed schedule run as the app runs it: feed on each frame, and tick when nextDueIn elapses (+20, armPsTick). */
function runPs(events: Array<{ t: number; key: string; ps: string }>) {
  L.push('PR');
  ps.reset();
  let tickAt: number | null = null;
  const arm = (now: number) => { const d = ps.nextDueIn(now); tickAt = d == null ? null : now + d + 20; };
  for (const e of events) {
    while (tickAt != null && tickAt <= e.t) {
      const now: number = tickAt;
      const shown = ps.tick(now);
      L.push(`PT ${now} | ${out(shown, now)}`);
      arm(now);
    }
    const shown = ps.feed(e.key, e.ps, e.t);
    L.push(`PF ${hex(e.key)} ${hex(e.ps)} ${e.t} | ${out(shown, e.t)}`);
    arm(e.t);
  }
  for (let k = 0; k < 4 && tickAt != null; k++) {
    const now: number = tickAt;
    const shown = ps.tick(now);
    L.push(`PT ${now} | ${out(shown, now)}`);
    arm(now);
  }
}
const sched = (key: string, list: string[], t0: number, dt: number) => list.map((p, i) => ({ t: t0 + i * dt, key, ps: p }));
runPs(sched('pi:C202', Array(12).fill('BBC R2  '), 0, 1000));
runPs([...sched('pi:C202', Array(25).fill('BBC R2'), 0, 1000), ...sched('pi:C202', Array(10).fill('BBC R2 HD'), 25000, 1000)]);
runPs(sched('pi:9301', Array.from({ length: 60 }, (_, i) => ['UMUARAMA', 'ALINE', 'RADIO'][i % 3]), 0, 1500));
runPs(sched('pi:9302', Array.from({ length: 50 }, (_, i) => ['UMUARAMA', 'MASSA'][i % 2]), 3000, 1100));
{ const b = 'RADIO MASSA FM '; runPs(sched('f:941', Array.from({ length: 80 }, (_, i) => (b + b).slice(i % b.length, i % b.length + 8)), 0, 700)); }
{ const b = 'Lazer 99.3 Playing Nirvana - In Bloom Festival 10/17 '; runPs(sched('pi:1F2E', Array.from({ length: 120 }, (_, i) => (b + b).slice((i * 4) % b.length, (i * 4) % b.length + 8)), 0, 900)); }
runPs([...sched('pi:C201', ['RADIO 1', '', '  ', 'RADIO 1', ''], 0, 2000), ...sched('pi:C202', ['RADIO 2', 'RADIO 2'], 11000, 500)]);
runPs(sched('f:960', ['A', 'B', 'A', 'B', 'C', 'A', 'B', 'C', 'A'], 0, 400));
for (let r = 0; r < 12; r++) {
  const pool = pick([['NRJ', 'HITS', 'NONSTOP'], ['A', 'B'], ['KISS FM', 'KISS FM', 'KISS  FM'], ['Ö3', 'HITRADIO'], ['ROCK 102', 'WAQY', 'NOW', 'METALLICA', 'ONE']]);
  const keys = pick([['pi:A1'], ['pi:A1', 'pi:A2'], ['f:881']]);
  const ev: Array<{ t: number; key: string; ps: string }> = [];
  let t = Math.floor(rnd() * 1000);
  const n = 20 + Math.floor(rnd() * 80);
  for (let i = 0; i < n; i++) {
    ev.push({ t, key: rnd() < 0.03 ? pick(keys) : keys[0], ps: rnd() < 0.08 ? '' : pick(pool) });
    t += 150 + Math.floor(rnd() * (rnd() < 0.1 ? 40000 : 3500));
  }
  runPs(ev);
}

// ── the band announcement ──
const order: Record<string, number> = { ham: 0, broadcast: 1, utility: 2 };
const bandLine = (hz: number, region: number) => {
  const bands = getBandsAtRegion(hz, region).sort((a: Band, b: Band) => (order[a.type] ?? 9) - (order[b.type] ?? 9));
  if (!bands.length) return '- - -';
  const key = bands.map((b: Band) => b.name).join('|');
  const primary = bands[0];
  const range = `${fmtBandFreq(primary.lo)}–${fmtBandFreq(primary.hi)}`;
  const primaryMsg = `BAND: ${range} · ${primary.name}` + (bands.length > 1 && region ? ` (ITU R${region})` : '');
  const secondary = bands.slice(1).map((b: Band) => b.name).join('  │  ');
  return `${hex(key)} ${hex(primaryMsg)} ${hex(secondary)}`;
};
const HZ = [10000, 136000, 150000, 200000, 530000, 1000000, 1610000, 1850000, 3600000, 3850000, 3950000, 5350000, 6000000, 7100000,
  7250000, 7350000, 14200000, 27000000, 28500000, 50100000, 87500000, 96100000, 108000000, 118000000, 145000000, 174928000,
  225648000, 433920000, 446000000, 1090000000, 1296000000, 10489750000];
for (const region of [0, 1, 2, 3]) {
  for (const hz of HZ) L.push(`B ${hz} ${region} | ${bandLine(hz, region)}`);
  for (let i = 0; i < 150; i++) { const hz = Math.round(Math.exp(Math.log(9000) + rnd() * (Math.log(2.5e9) - Math.log(9000)))); L.push(`B ${hz} ${region} | ${bandLine(hz, region)}`); }
}

// ── country, off-air text ──
for (const ecc of [0, 0xA1, 0xE0, 0xE1, 0xE2, 0xE3, 0xF0, 0xF4, 0xB0]) for (const pi of ['', 'C201', '1234', 'D3A1', 'F0F0', '0ABC', 'e123']) L.push(`E ${ecc} ${hex(pi)} | ${hex(eccPiToIso(ecc, pi || undefined))}`);
const RAW = ['BBC R2  ', '\u0001PS\u0007', ' Now‮playing​ ', 'Ärzte\u0085', 'x'.repeat(80), '\tTAB\t', '﻿BOM'];
for (const s of RAW) for (const max of [16, 64]) { L.push(`C ${max} ${hex(s)} | ${hex(cleanKeepSpacing(s, max))}`); L.push(`D ${max} ${hex(s)} | ${hex(dabSafeText(s, max))}`); }

writeFileSync(outFile, L.join('\n') + '\n');
console.error(`vts golden: ${L.length} lines → ${outFile}`);
