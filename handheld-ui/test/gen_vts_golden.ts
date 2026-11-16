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
 *   FV json | ok sub sb sbSel sync mode snr text last vts  parseFreeDv + freedvVts (src/services/freedv.ts; snr '-' = none)
 *   FO json | subs                              freedvOffered (subs comma-joined, '-' = not offered)
 *   FS hz sb sel | side lo hi                   freedvSidebandAt(hz, sel) + freedvPassband(sb)
 *   MV json | ok vts info line                  parseM17 + m17Vts / m17Info / m17Line (src/services/m17.ts)
 *   MO json | 0/1                               m17Offered
 *   PS json | ok text                           parsePagingStatus + pagingStatusText (src/services/paging.ts; V12 PAGE)
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
import { parseFreeDv, freedvVts, freedvOffered, freedvSidebandAt, freedvPassband } from '../../src/services/freedv.ts';
import { parseM17, m17Vts, m17Info, m17Line, m17Offered } from '../../src/services/m17.ts';
import { parsePagingStatus, pagingStatusText } from '../../src/services/paging.ts';

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

// ── V12 FreeDV (freedv.ts → handheld-ui/src/freedv.h) ──
const FDV: unknown[] = [
  { type: 'freedv', sub: 'auto', sb: 'usb', state: 'search', mode: '' },
  { type: 'freedv', sub: '1600', sb: 'lsb', state: 'search', mode: '' },
  { type: 'freedv', sub: '700E', sb: 'usb', state: 'search', mode: '700D', snr: 4 },
  { type: 'freedv', sub: 'auto', sb: 'usb', state: 'sync', mode: '700D', snr: 6, text: 'G4ABC' },
  { type: 'freedv', sub: 'auto', sb: 'lsb', state: 'sync', mode: '700E', snr: 5.5 },
  { type: 'freedv', sub: '700D', sb: 'usb', state: 'sync', mode: '700D', snr: -2.5, text: '  M0XYZ  ' },
  { type: 'freedv', sub: 'auto', sb: 'usb', state: 'sync', mode: '1600', snr: 12.49, text: 'Ö3 G4\u0007ABC Ärzte' },
  { type: 'freedv', sub: 'auto', sb: 'usb', state: 'sync', mode: '1600', text: 'x'.repeat(60) },
  { type: 'freedv', sub: 'auto', sb: 'usb', state: 'sync', mode: '2020', snr: 3 },
  { type: 'freedv', sub: 'nonsense', sb: 'xsb', state: 'sync', mode: '700E', snr: '7', text: 42 },
  { type: 'freedv', state: 'sync', mode: '700D', text: '   ' },
  { type: 'freedv' },
  { type: 'freedv', sub: 'auto', sb: 'usb', sbSel: 'usb', state: 'search' },
  { type: 'freedv', sub: '700E', sb: 'lsb', sbSel: 'lsb', state: 'search' },
  { type: 'freedv', sub: 'auto', sb: 'lsb', sbSel: 'auto', state: 'search' },
  { type: 'freedv', sub: '1600', sb: 'usb', sbSel: 'xsb', state: 'search' },
  { type: 'freedv', sub: 'auto', sb: 'lsb', sbSel: 'lsb', state: 'sync', mode: '700D', snr: 4 },
  // ★ V12 RADE: the sub-mode, and the end-of-over callsign as "last" — shown only while searching
  { type: 'freedv', sub: 'auto', sb: 'usb', sbSel: 'auto', state: 'sync', mode: 'RADE', snr: 3, last: 'M0ABC' },
  { type: 'freedv', sub: 'auto', sb: 'lsb', sbSel: 'auto', state: 'search', mode: '', last: 'G4ABC' },
  { type: 'freedv', sub: 'RADE', sb: 'lsb', sbSel: 'usb', state: 'search', last: '  DG4\u0007SDX  ' },
  { type: 'freedv', sub: 'RADEV2', sb: 'usb', state: 'sync', mode: 'RADEV2', snr: 1, last: 'X'.repeat(30) },
  { type: 'drm', sub: 'auto' },
  { sub: 'auto' },
];
for (const m of FDV) {
  const st = parseFreeDv(m);
  L.push(`FV ${hex(JSON.stringify(m))} | ` + (st
    ? `1 ${st.sub} ${st.sb} ${st.sbSel} ${st.sync ? 1 : 0} ${hex(st.mode)} ${st.snr === undefined ? '-' : st.snr} ${hex(st.text)} ${hex(st.last)} ${hex(freedvVts(st))}`
    : `0 ${hex(freedvVts(null))}`));
}
const OFFERS: unknown[] = [
  { modes: ['usb', 'freedv'], freedvSubs: ['700D', '700E', '1600'] },
  { modes: ['usb', 'freedv'], freedvSubs: ['RADE', '700D', '700E', '1600'] },
  { modes: ['usb', 'freedv'], freedvSubs: ['1600', 'RADE'] },
  { modes: ['usb', 'freedv'], freedvSubs: ['RADE'] },
  { modes: ['usb', 'freedv'], freedvSubs: ['1600', '700D'] },
  { modes: ['usb', 'freedv'], freedvSubs: ['2020', '700E'] },
  { modes: ['usb', 'freedv'] },
  { modes: ['usb', 'lsb'], freedvSubs: ['700D', '700E', '1600'] },
  { freedvSubs: ['700D'] },
  {},
];
for (const j of OFFERS) L.push(`FO ${hex(JSON.stringify(j))} | ${freedvOffered(j as never).join(',') || '-'}`);
for (const hz of [0, -5, 1, 3573000, 5249999, 5250000, 5368500, 5450000, 5450001, 7177000, 9999999, 10000000, 14236000, 28330000])
  for (const sb of ['usb', 'lsb'] as const)
    for (const sel of ['auto', 'usb', 'lsb'] as const) { const [lo, hi] = freedvPassband(sb); L.push(`FS ${hz} ${sb} ${sel} | ${freedvSidebandAt(hz, sel)} ${lo} ${hi}`); }

// ── V12 M17 (m17.ts → handheld-ui/src/m17.h) ──
const M17C: unknown[] = [
  { type: 'm17', state: 'search' },
  { type: 'm17', state: 'search', last: 'G4ABC' },
  { type: 'm17', state: 'sync', kind: 'voice', src: 'G4ABC', dst: '@ALL', can: 0, ber: 0, off: 0 },
  { type: 'm17', state: 'sync', kind: 'voice', src: 'G4ABC/P', dst: 'M17-M17 C', ber: 4.6, text: 'VIBE TEST' },
  { type: 'm17', state: 'sync', kind: 'voicedata', src: 'M0XYZ', dst: 'ECHO', ber: 7.4, via: 'G7XYZ via M17-GBR A' },
  { type: 'm17', state: 'sync', kind: 'voice', src: 'G4ABC', enc: 'aes' },
  { type: 'm17', state: 'sync', kind: 'packet', src: 'G4ABC', dst: 'M0XYZ' },
  { type: 'm17', state: 'sync', kind: 'data' },
  { type: 'm17', state: 'sync', kind: 'bogus', src: '  AB\u0007CD  ', dst: 'x'.repeat(30), enc: 'rot13', ber: -3 },
  { type: 'm17', state: 'search', last: 'G4ABC', text: 'Hello\tthere   world', gnss: { lat: 52.62346, lon: -1.12346, alt: 120.5, spd: 48.5, brg: 271, r: 8 } },
  { type: 'm17', state: 'search', gnss: { lat: -33.86882, lon: 151.20929 } },
  { type: 'm17', state: 'search', gnss: { lat: 95, lon: 0 } },
  { type: 'm17', state: 'search', gnss: { lat: 10.5, lon: 20.25, spd: 0, alt: -12.5 } },
  { type: 'm17', state: 'search', last: 'G4ABC', msg: { id: 3, src: 'G4ABC', dst: 'M0XYZ', ptype: 5, bytes: 23, text: 'Hello from VibeServer' } },
  { type: 'm17', state: 'search', msg: { id: 4, src: 'M0XYZ', ptype: 2, bytes: 41, text: 'not shown' } },
  { type: 'm17', state: 'search', msg: { id: 5, ptype: 5, bytes: 3, text: 'Ö3 Ärzte \u2003 73' } },
  { type: 'm17', state: 'search', msg: { id: 6, dst: 'M0XYZ', ptype: 5, bytes: 3, text: '' } },
  { type: 'm17', state: 'sync', kind: 'voice', src: 'ABCDEFGHIJKLMNOP', dst: 'QRSTUVWXYZ0123456789', text: 'y'.repeat(70) },
  { type: 'm17', state: 'sync', kind: 'voice', src: 'G4 ABC DEF GHIJ' },
  { type: 'm17' },
  { type: 'freedv', state: 'sync' },
  { state: 'sync' },
];
for (const m of M17C) {
  const st = parseM17(m);
  L.push(`MV ${hex(JSON.stringify(m))} | ` + (st ? `1 ${hex(m17Vts(st))} ${hex(m17Info(st))} ${hex(m17Line(st))}` : `0 ${hex(m17Vts(null))}`));
}
for (const j of [{ modes: ['nfm', 'm17'] }, { modes: ['nfm', 'freedv'] }, {}, { modes: 'm17' }]) L.push(`MO ${hex(JSON.stringify(j))} | ${m17Offered(j as never) ? 1 : 0}`);

// ── V12 PAGE (paging.ts → handheld-ui/src/paging.h) ──
const PSC: unknown[] = [
  { mode: 'auto', detected: '', pages: 0, rate: 31250, short: false },
  { mode: 'auto', detected: 'FLEX 3200/4', pages: 1, rate: 31250, short: false },
  { mode: 'auto', detected: 'POCSAG 1200', pages: 3, rate: 20000, short: true },
  { mode: 'pocsag1200', detected: '', pages: 0, rate: 20000, short: true },
  { mode: 'flex', detected: '', pages: 12, rate: 48000 },
  { detected: 'X'.repeat(40), pages: 2 },
  { mode: 7, detected: 5, pages: 'many', rate: null, short: 'yes' },
  { mode: 'auto', detected: 'FLEX 6400/4 \u00b7 \u{1F600}\u{1F600}', pages: 2.5 },
  { mode: 'auto', detected: '\u{1F600}'.repeat(20), pages: -1 },
  {},
  null,
  'paging',
  [],
];
for (const d of PSC) {
  const st = parsePagingStatus(d);
  L.push(`PS ${hex(JSON.stringify(d))} | ` + (st ? `1 ${hex(pagingStatusText(st))}` : `0 ${hex(pagingStatusText(null))}`));
}

writeFileSync(outFile, L.join('\n') + '\n');
console.error(`vts golden: ${L.length} lines → ${outFile}`);
