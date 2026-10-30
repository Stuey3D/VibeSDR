/**
 * KNOWN-SIGNAL LABELS (src/utils/knownSignals.ts) — Stuart, 2026-10-30: "just use EiBi and what we know about known
 * signals so 4582 for DDK 4608.1 for Northwood labelled as RTTY WEFAX etc, this is more for a user to know what decoder
 * to use". Proves:
 *   • the curated table: DWD's 4583 is RTTY with its dial at 4582 USB, Northwood's 4610 WEFAX at 4608.1, NAVTEX, DSC
 *     (dscChannels' own tune rule), the digital dials AGREE with digi_bands.h (read from the header), time signals;
 *   • EiBi rows are classed by EiBi's codes / the name, filtered to on-air, named shortly, de-duplicated against ours;
 *     broadcasts never label;
 *   • the visible set is exactly the view; the layout keeps priority, then nearness to the dial; overlaps drop to row 2,
 *     then out; a blocked box (LISTEN) is never overdrawn; the cap holds; a label near the right edge flips left.
 * Run: node --no-warnings scripts/test_known_signals.ts
 */
import { readFileSync } from 'node:fs';
import { KNOWN_SIGNALS, DIGI_DIALS, eibiKnownSignals, eibiPlace, eibiOnAir, knownSignalList, signalsInView, labelLayout,
         estimateLabelWidth, bookmarkSignals, bookmarkLabel, isLearntFm, LABEL_MAX, PRIO_SAFETY, PRIO_PERSONAL, PRIO_SERVER,
         PRIO_LEARNT, PRIO_EIBI, PRIO_TABLE, type KnownSignal } from '../src/utils/knownSignals.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const at = (hz: number, label?: string) => KNOWN_SIGNALS.find(s => s.hz === hz && (!label || s.label.startsWith(label)));

console.log('curated table');
const ddk = at(4583000, 'RTTY');
ok(!!ddk && ddk.label === 'RTTY · DWD' && ddk.dec === 'rtty' && ddk.dialHz === 4582000 && ddk.mode === 'usb', 'DWD 4583 kHz: RTTY · DWD, dial 4582 USB (Stuart\'s number)');
const nw = at(4610000, 'WEFAX');
ok(!!nw && nw.label === 'WEFAX · Northwood' && nw.dec === 'wefax' && nw.dialHz === 4608100, 'Northwood 4610: WEFAX · Northwood, dial 4608.1 USB (Stuart\'s number)');
ok(!!at(10100800, 'RTTY') && !!at(7646000, 'RTTY'), 'DWD 7646 and 10100.8 kHz are RTTY');
ok(!!at(518000, 'NAVTEX') && !!at(490000, 'NAVTEX') && !!at(4209500, 'NAVTEX') && at(518000)!.dialHz === 517500, 'NAVTEX 490 / 518 / 4209.5 kHz, dial 500 Hz below');
const dsc = at(2187500, 'DSC');
ok(!!dsc && dsc.dialHz === 2185800 && dsc.prio === PRIO_SAFETY && dsc.label === 'DSC · distress', 'DSC 2187.5: distress, dial 1.7 kHz below (dscTuneFor)');
ok(at(156525000, 'DSC')?.mode === 'nfm', 'DSC channel 70 is NFM on the channel');
const ft8 = at(14074000, 'FT8');
ok(!!ft8 && ft8.dec === 'spots' && ft8.lo === 14074000 && ft8.hi === 14077000, 'FT8 14.074: marker at the dial, signals 0–3 kHz above');
ok(at(14097100, 'WSPR')?.dialHz === 14095600, 'WSPR 14.0956: marker on its 1400–1600 Hz window');
ok(at(60000, 'TIME')?.dec === 'time' && at(77500, 'TIME')?.label === 'TIME · DCF77' && at(10000000, 'TIME')?.dec === 'time', 'MSF/DCF77/WWV are TIME');
ok(at(3330000)?.dec === '' && !at(3330000)!.label.startsWith('TIME'), 'CHU: labelled, but NOT sent to the TIME decoder (it does not read CHU)');
ok(at(3414500, 'VOLMET')?.dialHz === 3413000 && at(3414500)?.dec === '', 'Shannon VOLMET 3413 USB: speech, no decoder');
ok(KNOWN_SIGNALS.every((s, i) => i === 0 || KNOWN_SIGNALS[i - 1].hz <= s.hz), 'the table is sorted by frequency');

console.log('digital dials agree with digi_bands.h');
const h = readFileSync(new URL('../android/app/src/main/cpp/decoders/digi_bands.h', import.meta.url), 'utf8');
for (const m of ['FT8', 'FT4', 'WSPR', 'JS8'] as const) {
  const re = new RegExp(`\\{"${m}",\\s*"[0-9a-z]+",\\s*(\\d+)\\}`, 'g');
  const hdr = [...h.matchAll(re)].map(x => +x[1]).sort((a, b) => a - b);
  const ours = [...DIGI_DIALS[m]].sort((a, b) => a - b);
  ok(hdr.length > 0 && JSON.stringify(hdr) === JSON.stringify(ours), `${m}: ${ours.length} dials, the same as publishedDials()`);
}

console.log('EiBi');
const rows = [
  { name: 'GYA Northwood Meteo Fax', frequency: 2618500, comment: '2200-0500', source: 'eibi' },       // ours already
  { name: 'NOJ Fax Kodiak', frequency: 2054000, comment: '0340-0610', source: 'eibi' },
  { name: 'Shannon HFDL (ID7)', frequency: 2998000, comment: '0000-2400 · -HF', source: 'eibi' },
  { name: 'VFF Iqaluit Radio', frequency: 4177500, comment: '-TY', source: 'eibi' },
  { name: 'RDL Russian Navy', frequency: 18100, comment: '-CW', source: 'eibi' },
  { name: 'Tokyo Volmet x10,x40', frequency: 2863000, comment: 'E', source: 'eibi' },
  { name: 'BBC World Service', frequency: 5875000, comment: 'E', source: 'eibi' },                     // broadcast
  { name: 'Halifax fax off air', frequency: 4271000, comment: '1200-1300 · -TY', source: 'eibi' },      // not now
];
const now = 4 * 60;   // 04:00 UTC
const e = eibiKnownSignals(rows, now);
const lab = (hz: number) => e.find(s => Math.abs(s.hz - hz) < 2000)?.label;
ok(!lab(2618500), 'Northwood from EiBi is dropped — the curated label covers it');
ok(lab(2054000) === 'WEFAX · Kodiak' && e.find(s => s.hz === 2054000)!.dialHz === 2052100, 'NOJ Fax Kodiak → WEFAX · Kodiak, dial 1.9 kHz below');
ok(lab(2999440) === 'HFDL · Shannon' && e.find(s => s.label === 'HFDL · Shannon')!.dialHz === 2998000, 'Shannon HFDL → marker 1440 Hz above its listed dial');
ok(lab(4177500) === 'RTTY · Iqaluit', '-TY → RTTY · Iqaluit');
ok(lab(18100) === 'CW · Russian Navy', '-CW → CW');
ok(lab(2864500) === 'VOLMET · Tokyo', 'Tokyo Volmet → VOLMET · Tokyo (no decoder)');
ok(!lab(5875000), 'a broadcast is never a label');
ok(!e.some(s => s.hz === 4271000), 'an entry not on air now is not drawn');
ok(eibiOnAir('2200-0500', 4 * 60) && !eibiOnAir('2200-0500', 12 * 60) && eibiOnAir(undefined, 0), 'on-air windows wrap midnight; no time = on air');
ok(eibiPlace('DWD 2 Pinneberg RTTY') === 'Pinneberg' && eibiPlace('NSS US Navy') === 'US Navy', 'short places: call sign and numbers dropped');

console.log('bookmarks: personal, saved to the server, learnt FM');
ok(PRIO_SAFETY > PRIO_TABLE && PRIO_TABLE > PRIO_PERSONAL && PRIO_PERSONAL > PRIO_SERVER && PRIO_SERVER > PRIO_LEARNT && PRIO_LEARNT > PRIO_EIBI,
   'priority: safety > curated > personal > server-saved > learnt FM > EiBi');
const personal = [{ name: 'My local net', frequency: 3615000, mode: 'lsb' }, { name: 'Radio 2 (mine)', frequency: 89100000, mode: 'wfm' }];
const server = [
  { name: 'Club net Sunday', frequency: 7130000, mode: 'lsb', source: 'server', manual: true },
  { name: 'BBC R2', frequency: 89100000, mode: 'wfm', source: 'server', manual: false, pi: 0xC202 },     // under the personal one
  { name: 'Classic FM', frequency: 100900000, mode: 'wfm', source: 'server', manual: false, pi: 0xC203 },
  { name: 'Heart', frequency: 96400000, mode: 'wfm', source: 'server', manual: false, pi: 0xC3A1, nameSrc: 3 },
  { name: 'PI4322 93.7MHz', frequency: 93700000, mode: 'wfm', source: 'server', manual: false, pi: 0x4322, nameSrc: 0 },
  { name: 'Smooth?', frequency: 102200000, mode: 'wfm', source: 'server', manual: false, pi: 0xC4D1, nameSrc: 1 },
  { name: 'BBC R4 DAB', frequency: 225648000, mode: 'dab', source: 'server', manual: false, sid: 0xC224 },
  { name: 'Learnt on AM', frequency: 909000, mode: 'am', source: 'server', manual: false },
  { name: 'Voice of Somewhere', frequency: 9410000, source: 'eibi' },
  { name: 'An UberSDR row', frequency: 5000000, source: 'server' },
];
const bs = bookmarkSignals(personal, server, false);
const bl = (hz: number) => bs.find(s => s.hz === hz);
ok(bl(3615000)?.prio === PRIO_PERSONAL && bl(3615000)?.label === 'My local net', 'a personal bookmark is labelled by its name');
ok(bl(7130000)?.prio === PRIO_SERVER, 'a bookmark the owner saved to the server ("manual": true) is labelled');
ok(bl(100900000)?.prio === PRIO_LEARNT && bl(100900000)?.label === 'Classic FM', 'a learnt FM RDS name is labelled');
ok(bl(96400000)?.label === 'Heart', 'a RadioDNS name (nameSrc 3) counts as heard');
ok(bl(89100000)?.label === 'Radio 2 (mine)' && bs.filter(s => s.hz === 89100000).length === 1, 'one label a frequency: the personal name beats the learnt one');
ok(!bl(93700000) && !bl(102200000), 'a provisional "PI4322" or an unverified guess is not a name');
ok(!bl(225648000) && !bl(909000), 'a DAB service and a learnt row outside FM are not labels');
ok(!bl(9410000), 'an EiBi broadcast row is never a label');
ok(!bl(5000000) && bookmarkSignals([], server, true).some(s => s.hz === 5000000), 'an unmarked server row: a label only where the list is the owner\'s (not UberSDR\'s mixed list)');
ok(bookmarkLabel('A very long bookmark name indeed') === 'A very long boo…' && bookmarkLabel('  Short  ') === 'Short', 'long names are shortened to 16 characters');
ok(isLearntFm({ name: 'X', frequency: 98e6, source: 'server', manual: false }) && !isLearntFm({ name: 'X', frequency: 98e6, source: 'server', manual: true }), 'learnt = manual false, FM band');
const fmList = knownSignalList([], bs);
const F = labelLayout(fmList, 88e6, 20e6, 1200, measure0 => estimateLabelWidth(measure0, 9), 100.9e6);
ok(F.map(p => p.sig.label).includes('Classic FM') && F.map(p => p.sig.label).includes('Heart'), 'the FM band lays out its learnt names');

console.log('in view and layout');
const list = knownSignalList(e);
const v = signalsInView(list, 4500000, 4700000);
ok(v.some(s => s.label === 'WEFAX · Northwood') && v.every(s => s.hz >= 4500000 && s.hz <= 4700000), '4.5–4.7 MHz holds Northwood, nothing outside');
ok(signalsInView(list, 4500000, 4500000).length === 0, 'an empty view holds nothing');
const W = 1000, measure = (l: string) => estimateLabelWidth(l, 9);
// 4.205–4.215 MHz: DSC distress 4207.5, three routine, NAVTEX 4209.5 — all within 2 kHz of each other
const L = labelLayout(list, 4205000, 10000, W, measure, 4206000);
ok(L.length > 0 && L.length <= 2 * 3, `a crowded 10 kHz: ${L.length} labels on two rows, the rest dropped`);
ok(L[0].sig.prio === PRIO_SAFETY, 'the first placed is safety-of-life (DSC distress / NAVTEX)');
ok(L.every((a, i) => L.every((b, j) => i === j || a.row !== b.row || a.lx + a.w + 6 <= b.lx || b.lx + b.w + 6 <= a.lx)), 'no two labels on one row overlap');
ok(L.every(p => p.row < 2), 'never more than two rows');
// a blocked box at row 0 over the whole width → everything goes to row 1
const B = labelLayout(list, 4500000, 200000, W, measure, 4600000, [{ row: 0, x0: 0, x1: W }]);
ok(B.length > 0 && B.every(p => p.row === 1), 'a blocked row (LISTEN\'s box) is never drawn over');
// cap
const many: KnownSignal[] = Array.from({ length: 50 }, (_, i) => ({ hz: 1e6 + i * 10000, label: 'X', dec: 'rtty', dialHz: 0, mode: 'usb', prio: 3, src: 'table' }));
ok(labelLayout(many, 1e6, 500000, 10000, () => 10, 1.2e6).length === LABEL_MAX, `capped at ${LABEL_MAX}`);
// nearer the dial wins a tie
const two: KnownSignal[] = [
  { hz: 1000000, label: 'A', dec: 'rtty', dialHz: 0, mode: 'usb', prio: 3, src: 'table' },
  { hz: 1000100, label: 'B', dec: 'rtty', dialHz: 0, mode: 'usb', prio: 3, src: 'table' },
];
const T = labelLayout(two, 990000, 20000, 400, () => 60, 1000100, [], 3, 1);
ok(T.length === 1 && T[0].sig.label === 'B', 'equal priority: the one nearer the dial keeps the row');
const edge = labelLayout(two.slice(0, 1), 990000, 10100, 400, () => 60, 0);
ok(edge.length === 1 && edge[0].lx + edge[0].w <= edge[0].x, 'near the right edge a label flips to the left of its marker');

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
