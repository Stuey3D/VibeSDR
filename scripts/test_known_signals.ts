/**
 * KNOWN-SIGNAL LABELS (src/utils/knownSignals.ts) — Stuart, 2026-10-30: "just use EiBi and what we know about known
 * signals so 4582 for DDK 4608.1 for Northwood labelled as RTTY WEFAX etc, this is more for a user to know what decoder
 * to use". Proves:
 *   • the curated table: DWD's 4583 is RTTY with its dial at 4582 USB, Northwood's 4610 WEFAX at 4608.1, NAVTEX, DSC
 *     (dscChannels' own tune rule), the digital dials AGREE with digi_bands.h (read from the header), time signals;
 *   • EiBi rows are classed by EiBi's codes / the name, filtered to on-air, named shortly, de-duplicated against ours;
 *     broadcasts never label;
 *   • the visible set is exactly the view; the layout keeps priority, then nearness to the dial; overlaps drop to row 2,
 *     then out; a blocked box (LISTEN) is never overdrawn; the cap holds; every label CENTRED on its carrier (Stuart,
 *     2026-11-16), shifted inward at both edges; a crowded cluster (the 0.9 MHz BBC stations) stacks without overlaps;
 *   • TAPPED (Stuart, 2026-10-30: "can they be tapped to tune and chose the decoder?"): every curated entry's dial, mode
 *     and decoder preset are valid; the plan opens only a decoder the receiver runs; a tap hits a box over ≥ 30 pt, the
 *     nearer box where two rows' areas overlap; a drag or a hold is never a tap.
 * Run: node --no-warnings scripts/test_known_signals.ts
 */
import { readFileSync } from 'node:fs';
import { KNOWN_SIGNALS, DIGI_DIALS, eibiKnownSignals, eibiPlace, eibiOnAir, knownSignalList, signalsInView, labelLayout,
         estimateLabelWidth, bookmarkSignals, bookmarkLabel, isLearntFm, LABEL_MAX, PRIO_SAFETY, PRIO_PERSONAL, PRIO_SERVER,
         PRIO_LEARNT, PRIO_EIBI, PRIO_TABLE, labelStems, LABEL_ROWS_MAX, knownTapPlan, knownLabelHit, isLabelTap, LABEL_HIT_MIN_H, type KnownSignal,
         type PlacedLabel, dxDialSignals, dxDialAgo, dxDialHint, dxServerNow, isDxLabel, PRIO_DX, DX_DIAL_MAX, DX_DIAL_MAX_AGE_S,
         DX_LABEL_COLOUR, KNOWN_LABEL_COLOUR, type DxDialEntry } from '../src/utils/knownSignals.ts';
import { dabBlockSignals, withDabBlocks, dabBlockHint, DAB_GRADE_COLOUR } from '../src/utils/dabBlockView.ts';
import { RTTY_WEATHER, RTTY_SHIFTS, RTTY_BAUD_ROWS } from '../src/utils/rttySpec.ts';
import { dscTuneFor, DSC_ALL } from '../src/utils/dscChannels.ts';

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
ok(!KNOWN_SIGNALS.some((s) => /\bCHU\b/.test(s.label)), 'CHU: no label anywhere (NRC ended the shortwave broadcast 2026-06-22)');
ok(at(3414500, 'VOLMET')?.dialHz === 3413000 && at(3414500)?.dec === '', 'Shannon VOLMET 3413 USB: speech, no decoder');
ok(KNOWN_SIGNALS.every((s, i) => i === 0 || KNOWN_SIGNALS[i - 1].hz <= s.hz), 'the table is sorted by frequency');

console.log('digital dials agree with digi_bands.h');
const h = readFileSync(new URL('../android/app/src/main/cpp/decoders/digi_bands.h', import.meta.url), 'utf8');
for (const m of ['FT8', 'FT4', 'WSPR', 'JS8', 'Q65'] as const) {
  const re = new RegExp(`\\{"${m}",\\s*"[0-9a-z]+",\\s*(\\d+)\\}`, 'g');
  const hdr = [...h.matchAll(re)].map(x => +x[1]).sort((a, b) => a - b);
  const ours = [...DIGI_DIALS[m]].sort((a, b) => a - b);
  ok(hdr.length > 0 && JSON.stringify(hdr) === JSON.stringify(ours), `${m}: ${ours.length} dials, the same as publishedDials()`);
}

{ // ★ V12: Q65's tiled dials are one label per joined run (17 labels on 23 cm would crowd out the rest)
  const q = KNOWN_SIGNALS.filter(s => s.label === 'Q65');
  const at = (lo: number, hi: number) => q.filter(s => s.hz >= lo && s.hz <= hi);
  const r23 = at(1240e6, 1300e6), r70 = at(420e6, 450e6), r2 = at(144e6, 148e6);
  ok(r23.length === 1 && r23[0].lo === 1296050200 && r23[0].hi === 1296126800, `Q65 23 cm: one label, its bar 1296.0502–1296.1268 MHz (${r23.map(s => `${s.lo}-${s.hi}`)})`);
  ok(r70.length === 1 && r70[0].hi === 432100800, `Q65 70 cm: one label to 432.1008 (${r70.map(s => `${s.lo}-${s.hi}`)})`);
  ok(r2.length === 2, `Q65 2 m: the EME run and 144.180 (${r2.length})`);
  ok(q.every(s => s.dec === 'spots' && s.preset?.spots === 'Q65' && s.dialHz === s.hz), 'Q65 labels: tune the run\'s first dial and open DIGI SPOTS');
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
ok(L.length > 0 && L.length <= LABEL_MAX, `a crowded 10 kHz: ${L.length} labels stacked`);
ok(L[0].sig.prio === PRIO_SAFETY, 'the first placed is safety-of-life (DSC distress / NAVTEX)');
ok(L.every((a, i) => L.every((b, j) => i === j || a.row !== b.row || a.lx + a.w + 6 <= b.lx || b.lx + b.w + 6 <= a.lx)), 'no two labels on one row overlap');
ok(L.every(p => p.row < LABEL_ROWS_MAX), 'never more than LABEL_ROWS_MAX rows');
// a blocked box at row 0 over the whole width → everything goes to row 1
const B = labelLayout(list, 4500000, 200000, W, measure, 4600000, [{ row: 0, x0: 0, x1: W }]);
ok(B.length > 0 && B.every(p => p.row >= 1), 'a blocked row (LISTEN\'s box) is never drawn over');
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
ok(edge.length === 1 && edge[0].lx === 400 - 60 && edge[0].lx < edge[0].x, 'near the right edge a label shifts inward, wholly on screen');

console.log('centred on the carrier (Stuart, 2026-11-16: "centre all signal labels on their carrier frequency")');
{
  const sg = (hz: number, label: string, prio = PRIO_SERVER): KnownSignal => ({ hz, label, dec: '', dialHz: hz, mode: 'am', prio, src: 'server' });
  const one = labelLayout([sg(700000, 'Radio Caroline')], 400000, 550000, 1200, measure, 0);
  ok(one.length === 1 && Math.abs(one[0].lx + one[0].w / 2 - one[0].x) < 1e-9, 'a label\'s box is centred on its carrier');
  const lft = labelLayout([sg(401000, 'WSPR · 474k')], 400000, 550000, 1200, measure, 0);
  ok(lft[0].lx === 0 && lft[0].lx + lft[0].w > lft[0].x, 'near the left edge: shifted inward, starts at 0');
  const rgt = labelLayout([sg(949000, 'BBC Radio 5')], 400000, 550000, 1200, measure, 0);
  ok(rgt[0].lx + rgt[0].w === 1200, 'near the right edge: shifted inward, ends at the edge');
  // ★ Stuart's screenshot (Lenovo RSP1A, 0.4–0.95 MHz on a 1400-px page): the labels he saw, and the BBC cluster near 0.9 MHz
  const am = [sg(518000, 'NAVTEX', PRIO_SAFETY), sg(474200, 'WSPR', PRIO_TABLE), sg(558000, 'Panjab Radio'), sg(648000, 'Radio Caroline'),
              sg(810000, 'BBC Radio Scotl…'), sg(837000, 'BBC Asian Netwo…'), sg(882000, 'BBC Radio Wales'), sg(855000, 'BBC Radio Norfo…'),
              sg(873000, 'BBC Radio Norfo…'), sg(909000, 'BBC Radio 5'), sg(891000, 'Absolute Radio'), sg(927000, 'BBC Radio 5'), sg(936000, 'Smooth')];
  am.sort((a, b) => a.hz - b.hz);
  const textOverlap = (P: PlacedLabel[]) => P.some((a, i) => P.some((b, j) => i !== j && a.row === b.row && a.lx < b.lx + b.w + 6 && b.lx < a.lx + a.w + 6));
  // ★★ the marker line ABOVE each label (Stuart, 2026-11-16): rows 14 apart (row 0 lowest), boxes 13 tall, 8 above the stack
  const rowTop = (r: number) => 100 - 14 * r;
  /** Every label's line rises from its top edge at its TRUE carrier x, past the whole stack, crossing no other box. */
  const linesClear = (P: PlacedLabel[]) => {
    const ST = labelStems(P, rowTop, 13, 8), stackTop = Math.min(...P.map(p => rowTop(p.row)));
    return ST.length === P.length && ST.every((st, i) => st.x === Math.round(P[i].x) && st.y1 === rowTop(P[i].row) && st.y0 === stackTop - 8
      && !P.some((q, j) => j !== i && st.x >= q.lx && st.x <= q.lx + q.w && st.y0 < rowTop(q.row) + 13 && st.y1 > rowTop(q.row)));
  };
  for (const Wd of [390, 1400]) {
    const P = labelLayout(am, 400000, 550000, Wd, measure, 648000);
    const onScreen = P.every(p => p.lx >= 0 && p.lx + p.w <= Wd);
    ok(!textOverlap(P) && onScreen && P.every(p => p.row < LABEL_ROWS_MAX),
       `${Wd} px, the AM band 0.4–0.95 MHz: ${P.length} of ${am.length} placed (cap ${LABEL_MAX}), on screen, none overlapping, ≤ ${LABEL_ROWS_MAX} rows`);
    ok(P.every(p => p.lx - 1 <= p.x && p.x <= p.lx + p.w + 1), `${Wd} px: every box sits over its own carrier`);
    const cen = P.filter(p => Math.abs(p.lx + p.w / 2 - p.x) < 1e-6 || p.lx === 0 || p.lx + p.w === Wd).length;
    if (Wd === 1400) ok(cen >= P.length * 0.6, `${Wd} px: most centred on their carrier (${cen} of ${P.length}; the rest slid along their row to stack)`);
    ok(linesClear(P), `${Wd} px: every label's line rises from its top edge at its TRUE carrier x past the stack, crossing no other label`);
    if (Wd === 1400) ok(P.length === Math.min(LABEL_MAX, am.length), '1400 px: nothing dropped but the cap');
  }
  // ★★★ THE PILE-UP (Stuart, 2026-11-16): BBC Radio Norfolk 873, BBC Radio Wales 882, BBC Radio 5 909 at 0.4–0.95 MHz on
  //     a 1400-px page — three labels within a label-width of each other: all visible, stacked, no text overlapping, each
  //     line reaching its own carrier
  {
    const pile = [sg(873000, 'BBC Radio Norfo…'), sg(882000, 'BBC Radio Wales'), sg(909000, 'BBC Radio 5')];
    const P = labelLayout(pile, 400000, 550000, 1400, measure, 648000);
    const xs = pile.map(p => (p.hz - 400000) / 550000 * 1400);
    const wW = measure('BBC Radio Wales');
    ok(xs[1] - xs[0] < wW && xs[2] - xs[1] < wW, `the pile-up: Wales within a label's width of each neighbour (${(xs[1] - xs[0]).toFixed(0)} / ${(xs[2] - xs[1]).toFixed(0)} px < ${wW} px)`);
    ok(P.length === 3 && P.every(p => !p.lines), 'the pile-up: all three visible, none shortened (there are rows to spare)');
    const row = (l: string) => P.find(p => p.sig.label === l)!.row;
    ok(row('BBC Radio Wales') !== row('BBC Radio Norfo…') && row('BBC Radio Wales') !== row('BBC Radio 5'),
       `the pile-up: Wales stacked off both its neighbours' rows (${P.map(p => p.sig.label + ' → row ' + p.row).join(', ')})`);
    ok(!textOverlap(P), 'the pile-up: no text overlapping');
    ok(linesClear(P), 'the pile-up: each line reaches past the whole stack at its own carrier x, crossing no other label');
  }
  // shortened only once the rows are used up (one row here): a name squeezed by two others is cut with "…" to fit
  {
    const three = [sg(600000, 'Station number 1', PRIO_TABLE), sg(627500, 'Station number 2'), sg(655000, 'Station number 3', PRIO_TABLE)];
    const P = labelLayout(three, 400000, 550000, 1400, measure, 0, [], 3, 1);
    const cut = P.find(p => p.lines);
    ok(P.length === 3 && !!cut?.lines?.[0]?.endsWith('…') && cut.lines[0].length >= 4 && cut.lx <= cut.x && cut.x <= cut.lx + cut.w && !textOverlap(P),
       `one row, three names close together: all three shown, one shortened to "${cut?.lines?.[0]}" over its carrier, nothing overlapping`);
    const P4 = labelLayout(three, 400000, 550000, 1400, measure, 0);
    ok(P4.length === 3 && P4.every(p => !p.lines), 'the same with rows to spare: stacked whole, none shortened');
  }
  // ★★ ZOOMED IN (Stuart's 0.59–0.80 MHz screenshot, tuned to 693): Radio Caroline sat RIGHT of its own peak
  {
    const z = [sg(648000, 'Radio Caroline'), sg(603000, 'Panjab Radio'), sg(756000, 'BBC Radio Cornw…')];
    z.sort((a, b) => a.hz - b.hz);
    const P = labelLayout(z, 590000, 210000, 1400, measure, 693000);
    const car = P.find(p => p.sig.label === 'Radio Caroline')!;
    ok(Math.abs(car.lx + car.w / 2 - car.x) < 1e-9 && car.x === (648000 - 590000) / 210000 * 1400,
       `0.59–0.80 MHz: "Radio Caroline" centred on 648 kHz (box ${car.lx.toFixed(0)}–${(car.lx + car.w).toFixed(0)}, carrier ${car.x.toFixed(0)})`);
    // zooming in and out: every label stays exactly centred on its carrier at every step — no lag, no jump
    let worst = 0;
    for (let k = 0; k <= 40; k++) {
      const spanZ = 210000 * Math.pow(4, k / 40 - 0.5), loZ = 693000 - spanZ / 2;
      for (const p of labelLayout(z, loZ, spanZ, 1400, measure, 693000))
        if (p.lx > 0 && p.lx + p.w < 1400) worst = Math.max(worst, Math.abs(p.lx + p.w / 2 - (p.sig.hz - loZ) / spanZ * 1400));
    }
    ok(worst < 1e-6, `zooming 105 → 420 kHz in 41 steps: every label centred on its carrier at every step (worst ${worst.toExponential(1)} px)`);
  }
  const cl = labelLayout([sg(401000, 'WSPR · 474k')], 400000, 550000, 1200, measure, 0);
  const cs = labelStems(cl, (r) => 100 - 14 * r, 13, 8)[0];
  ok(cs.x === Math.round(cl[0].x) && cs.x < cl[0].lx + cl[0].w / 2, 'an edge-clamped label: the line stays at the carrier, off the box\'s centre');
}

console.log('tapped: the preset table');
const MODES = new Set(['usb', 'lsb', 'am', 'sam', 'fm', 'nfm', 'cwu', 'cwl', 'wfm', 'drm']);   // sdrTypes.ts SDRMode
const DECS = new Set(['rtty', 'wefax', 'navtex', 'dsc', 'spots', 'time', 'hfdl', 'cw', '']);
const TIME_ST = new Set(['msf', 'dcf77', 'wwv', 'wwvb', 'rwm', 'jjy', 'tdf']);   // DecoderClient TimeStation (JJY: V12 wave 2)
const bad: string[] = [];
for (const s of KNOWN_SIGNALS) {
  const why = !MODES.has(s.mode) ? `mode ${s.mode}` : !DECS.has(s.dec) ? `decoder ${s.dec}` : !(s.dialHz > 0) ? 'no dial'
    : Math.abs(s.hz - s.dialHz) > 3000 ? 'dial > 3 kHz from the marker'
    : s.dec === 'rtty' && s.src === 'table' && !s.preset?.rtty ? 'curated RTTY without its preset'
    : s.preset?.rtty && (!(RTTY_SHIFTS as readonly number[]).includes(s.preset.rtty.shift) || !RTTY_BAUD_ROWS.flat().includes(s.preset.rtty.baud as never)) ? 'RTTY preset off the menu'
    : s.dec === 'wefax' && (s.preset?.wefaxLpm !== 120 || s.preset?.wefaxIoc !== 576) ? 'WEFAX not 120 LPM / IOC 576'
    : s.dec === 'time' && !(s.preset?.time?.length && s.preset.time.every(t => TIME_ST.has(t))) ? 'TIME without its stations'
    : s.dec === 'spots' && !(s.preset?.spots && s.label === s.preset.spots) ? 'spots without its mode'
    : s.preset && !s.dec ? 'a preset with no decoder' : '';
  if (why) bad.push(`${s.label} @ ${s.hz}: ${why}`);
}
ok(bad.length === 0, `every curated entry (${KNOWN_SIGNALS.length}) has a valid dial, mode, decoder and preset${bad.length ? ' — ' + bad.slice(0, 4).join('; ') : ''}`);
ok(JSON.stringify(ddk!.preset?.rtty) === JSON.stringify(RTTY_WEATHER) && RTTY_WEATHER.baud === 50 && RTTY_WEATHER.shift === 450 && RTTY_WEATHER.encoding === 'ITA2',
   'DWD RTTY: the WEATHER preset — 50 Bd, 450 Hz, ITA2 (rttySpec.ts RTTY_WEATHER, the menu\'s own)');
ok(nw!.preset?.wefaxLpm === 120 && nw!.preset?.wefaxIoc === 576, 'Northwood WEFAX: 120 LPM, IOC 576');
ok(at(60000, 'TIME')?.preset?.time?.join() === 'msf,wwvb,jjy' && at(60000)!.mode === 'cwu' && at(10000000, 'TIME')?.mode === 'am',
   'TIME 60 kHz: MSF, WWVB or JJY, tuned CW (cwu); WWV in AM');
ok(DSC_ALL.every(c => { const k = KNOWN_SIGNALS.find(s => s.dec === 'dsc' && s.hz === c.hz); const t = dscTuneFor(c); return !!k && k.dialHz === t.hz && k.mode === t.mode; }),
   'DSC: every channel tunes by dscChannels\' own rule');
const ALL = { rtty: true, wefax: true, navtex: true, dsc: true, spots: true, time: true, hfdl: true, cw: true };
const pd = knownTapPlan(ddk!, ALL);
ok(pd.hz === 4582000 && pd.mode === 'usb' && pd.dec === 'rtty' && pd.preset?.rtty?.baud === 50 && pd.preset?.rtty?.shift === 450,
   'tap DWD 4583: tune 4582 USB, open RTTY at 50 Bd / 450 Hz');
const pn = knownTapPlan(ddk!, { ...ALL, rtty: false });
ok(pn.hz === 4582000 && pn.dec === '' && !pn.preset, 'RTTY not offered here (blocked / not on this server): the tap still tunes, opens nothing');
ok(knownTapPlan(at(3414500, 'VOLMET')!, ALL).dec === '' && knownTapPlan(at(3414500)!, ALL).hz === 3413000, 'VOLMET: just tune (3413 USB), no decoder');
ok(knownTapPlan(ft8!, ALL).dec === 'spots' && knownTapPlan(ft8!, ALL).hz === 14074000, 'FT8 14.074: tune the dial, open DIGI SPOTS');
const bmk = bs.find(s => s.label === 'Classic FM')!;
const pb = knownTapPlan(bmk, ALL);
ok(pb.dec === '' && pb.mode === 'wfm' && pb.hz === 100900000, 'a learnt FM name: just tune, in WFM');
ok(knownTapPlan({ ...bmk, mode: 'CW' }, ALL).mode === 'cwu', 'a bookmark saved as "cw" tunes CW upper');
const hfdl = e.find(s => s.label === 'HFDL · Shannon')!;
ok(knownTapPlan(hfdl, ALL).dec === 'hfdl' && knownTapPlan(hfdl, { ...ALL, hfdl: false }).dec === '', 'HFDL: its decoder only where the receiver has it');

console.log('tapped: hit-testing, tap vs drag');
// two rows: row 0's box top at 100, row 1's at 86 (14 above), 13 tall — the app's geometry
const rowTop = (r: number) => (r === 0 ? 100 : 86);
const P: PlacedLabel[] = [
  { sig: ddk!, x: 200, lx: 203, w: 60, row: 0 },
  { sig: nw!, x: 220, lx: 223, w: 80, row: 1 },
  { sig: ft8!, x: 500, lx: 503, w: 30, row: 0 },
];
const hitL = (x: number, y: number) => knownLabelHit(P, x, y, rowTop, 13)?.sig.label ?? null;
ok(hitL(230, 106) === 'RTTY · DWD' && hitL(250, 92) === 'WEFAX · Northwood', 'inside a box: that label');
ok(hitL(515, 106 + LABEL_HIT_MIN_H / 2 - 1) === 'FT8' && hitL(515, 106 - LABEL_HIT_MIN_H / 2 + 1) === 'FT8', `a lone box is hit over ${LABEL_HIT_MIN_H} pt, centred on it`);
ok(hitL(515, 106 + LABEL_HIT_MIN_H / 2 + 2) === null, 'beyond that: not a label (tap-to-tune)');
ok(hitL(240, 99.7) === 'RTTY · DWD' && hitL(240, 98.5) === 'WEFAX · Northwood' && hitL(240, 118) === 'RTTY · DWD' && hitL(240, 80) === 'WEFAX · Northwood', 'where two rows\' areas overlap, the nearer box wins');
ok(hitL(203 - 3, 106) === 'RTTY · DWD' && hitL(203 - 6, 106) === null, 'a few points of slop either side, no more');
ok(hitL(400, 106) === null && knownLabelHit(null, 230, 106, rowTop, 13) === null, 'between labels, or with none drawn: no hit');
ok(isLabelTap(0, 0, 80) && isLabelTap(6, -5, 250), 'a tap: still, short');
ok(!isLabelTap(14, 0, 80) && !isLabelTap(0, 30, 120), 'a drag starting on a label is not a tap (it pans)');
ok(!isLabelTap(0, 0, 900), 'a long hold is not a tap');

console.log('DX catches on the dial (Stuart, 2026-11-16)');
{
  const NOW = 1_800_000_000, H = 3600, D = 86400;
  const E = (o: Partial<DxDialEntry>): DxDialEntry => ({ band: 'FM', class: 'dx', lastHeard: NOW - 2 * H, ...o });
  const entries: DxDialEntry[] = [
    E({ freqKhz: 98800, pi: '5202', ps: 'RNE 1', rds: true, range: 'fm' }),
    E({ freqKhz: 98800, pi: '5201', ps: 'RAI R1', lastHeard: NOW - 5 * H }),                 // older on the same channel
    E({ freqKhz: 69800, pi: '0000', ps: '', rds: false, range: 'oirt', lastHeard: NOW - 3 * D }),
    E({ freqKhz: 86650, pi: '0000', ps: '', rds: false, range: 'ext', lastHeard: NOW - 40 * 60 }),
    E({ freqKhz: 99300, pi: 'C363', ps: '', lastHeard: NOW - 30 }),
    E({ freqKhz: 101100, ps: 'SUSPECT', class: 'suspect' }),
    E({ freqKhz: 101300, ps: 'BASELINE', class: 'provisional' }),
    E({ freqKhz: 101500, ps: 'LOCAL', class: 'local' }),
    E({ freqKhz: 102100, ps: 'WEEK IN', lastHeard: NOW - DX_DIAL_MAX_AGE_S }),
    E({ freqKhz: 102300, ps: 'WEEK OUT', lastHeard: NOW - DX_DIAL_MAX_AGE_S - 1 }),
    E({ freqKhz: 95800, ps: 'MY BM' }),                                                      // under a personal bookmark
    E({ freqKhz: 94600, ps: 'NO LABEL' }),                                                   // under a bookmark with no label
    E({ band: 'DAB', block: '12C', eid: 'E0F1', ensemble: 'Far Away', freqKhz: 227360, lastHeard: NOW - D - H }),
  ];
  const bms = bookmarkSignals([{ name: 'Flex FM', frequency: 95_800_000, mode: 'wfm' }], [], false);
  const known = knownSignalList([], bms);
  const dx = dxDialSignals(entries, NOW, known, [94_600_000]);
  const by = (khz: number) => dx.find((s) => s.hz === khz * 1000);
  ok(by(98800)?.label === 'RNE 1 · DX · 2 h ago', `the look: name · DX · when ("${by(98800)?.label}")`);
  ok(dx.filter((s) => s.hz === 98_800_000).length === 1, 'one label a channel: the most recently heard of two PIs');
  ok(by(69800)?.label === '69.80 · DX · 3 days ago', `★ OIRT without RDS: two decimals ("${by(69800)?.label}")`);
  ok(by(86650)?.label === '86.65 · DX · 40 min ago' && by(99300)?.label === 'PI C363 · DX · just now',
     'the extended range to two decimals off the 100 kHz raster; no PS → the PI; "just now"');
  ok(!by(101100) && !by(101300) && !by(101500), '★ only class dx — never a Suspect, a Provisional or a Local');
  ok(!!by(102100) && !by(102300), '★ the WEEK: shown up to 7 days after last heard, gone a second later');
  ok(!by(95800) && known.some((s) => s.hz === 95_800_000 && s.src === 'personal'), '★ a bookmark on the same channel WINS (no DX label)');
  ok(!by(94600), '★ …and so does a bookmark that draws no label (a local in the discovered bookmarks)');
  ok(dx.every((s) => s.src === 'dx' && isDxLabel(s) && s.prio === PRIO_DX && s.prio < PRIO_LEARNT && s.prio < PRIO_EIBI + 0.5),
     'every one is flagged DX (italic, its own colour) and ranks below every bookmark');
  ok(DX_LABEL_COLOUR.toLowerCase() !== KNOWN_LABEL_COLOUR.toLowerCase() && DX_LABEL_COLOUR.toLowerCase() !== '#ff2020'
     && !Object.values(DAB_GRADE_COLOUR).includes(DX_LABEL_COLOUR), 'its colour is not the labels\' lilac, the VFO\'s red or a DAB grade');
  const dab = dx.find((s) => s.mode === 'dab');
  ok(!!dab && dab.hz === 227_360_000 && dab.label === '12C Far Away · DX · 1 day ago' && dab.dx?.block === '12C' && dab.dx?.ensemble === 'Far Away',
     `DAB: on its block's centre ("${dab?.label}")`);
  ok(dxDialSignals([E({ band: 'DAB', block: '12C', eid: 'E0F1', ensemble: '', freqKhz: 0 })], NOW)[0]?.hz === 227_360_000,
     'a DAB catch without freqKhz finds its block in Band III\'s table, named by its EId');
  ok(dxDialAgo(NOW - 59 * 60, NOW) === '59 min ago' && dxDialAgo(NOW - 23 * H - 59 * 60, NOW) === '23 h ago'
     && dxDialAgo(NOW - D, NOW) === '1 day ago' && dxDialAgo(NOW - 6 * D, NOW) === '6 days ago', 'ages: min, h, 1 day, days');
  ok(dxDialSignals(null, NOW).length === 0 && dxDialSignals([E({ freqKhz: 145500 })], NOW).length === 0
     && dxDialSignals([E({ freqKhz: 98800, lastHeard: NOW + 2 * H })], NOW).length === 0,
     'nothing from nothing; nothing outside the FM ranges; nothing heard in the future (a clock wrong by hours)');
  const many = Array.from({ length: 150 }, (_, k) => E({ freqKhz: 88000 + k * 100, ps: `S${k}`, lastHeard: NOW - k }));
  ok(dxDialSignals(many, NOW).length === DX_DIAL_MAX && !dxDialSignals(many, NOW).some((s) => s.dx!.name === 'S149'),
     `bounded at ${DX_DIAL_MAX}, the most recently heard kept`);
  ok(Math.abs(dxServerNow(NOW, 1_000_000, 1_060_000) - (NOW + 60)) < 1e-6, 'the age runs on the server\'s clock');
  // placement: the same layout as every label — centred on its carrier, a line to it, stacked; below a bookmark
  const all = knownSignalList([], bms, dx);
  const m = (t: string) => estimateLabelWidth(t, 9);
  const pl = labelLayout(all, 98_000_000, 2_000_000, 400, m, 98_800_000);
  const pDx = pl.find((p) => p.sig.hz === 98_800_000);
  ok(!!pDx && Math.abs(pDx.lx + pDx.w / 2 - pDx.x) < 0.5 && pDx.sig.src === 'dx', 'a DX label is centred on its carrier');
  const crowd = knownSignalList([], bookmarkSignals([{ name: 'Flex FM', frequency: 98_700_000, mode: 'wfm' }], [], false),
                                dxDialSignals([E({ freqKhz: 98800, ps: 'RNE 1' }), E({ freqKhz: 98900, ps: 'RAI 1' })], NOW));
  const pc = labelLayout(crowd, 98_000_000, 2_000_000, 400, m, 98_000_000);
  const pBm = pc.find((p) => p.sig.src === 'personal');
  ok(pc[0].sig.src === 'personal' && pBm!.row === 0 && pc.filter((p) => p.sig.src === 'dx').every((p) => p.row > 0),
     'crowded: the bookmark is placed first, the DX catches stack above it');
  const tight = labelLayout(crowd, 98_000_000, 2_000_000, 200, m, 98_000_000, [], 3, 1);
  ok(tight.length === 1 && tight[0].sig.src === 'personal' && !tight[0].lines,
     'one row, no room: the bookmark keeps its whole name, the DX catches give way');
  const tap = knownTapPlan(by(98800)!, {});
  ok(tap.hz === 98_800_000 && tap.mode === 'wfm' && tap.dec === '', 'tap: tunes the channel in WFM');
  ok(knownTapPlan(dab!, {}).mode === 'dab', 'tap on a DAB catch: the block (the clients\' DAB road)');
  ok(/^DX catch 69\.80 MHz, 69\.80, last heard 3 days ago$/.test(dxDialHint(by(69800)!, NOW)), `the hint: "${dxDialHint(by(69800)!, NOW)}"`);
  // DAB: a wide view folds the catch into its block
  const names = new Map([['12B', 'Digital One']]);
  const blocks = dabBlockSignals(names, 32, null, dx);   // 12B (index 32) being received
  const b12c = blocks.find((b) => b.block === '12C')!;
  ok(b12c.mux === 'DX · Far Away' && isDxLabel(b12c), `★ a wide DAB view: an unnamed block a DX catch names reads "${b12c.mux}", flagged DX`);
  const named = dabBlockSignals(new Map([['12C', 'Known Mux']]), -1, null, dx).find((b) => b.block === '12C')!;
  ok(named.mux === 'Known Mux' && !isDxLabel(named), '★ a name the receiver knows wins over the DX catch');
  ok(!dabBlockSignals(names, 33, null, dx).find((b) => b.block === '12C')!.dx, 'the block being received keeps its live name, no DX');
  const wl = withDabBlocks(all, blocks, 56e6)!;
  ok(!wl.some((s) => s.src === 'dx' && s.mode === 'dab') && wl.some((s) => s.src === 'dx' && s.mode === 'wfm'),
     'withDabBlocks: on a wide DAB view the separate DAB DX label is left out (its block names it)');
  ok(dabBlockHint(33, names, 32, null, NOW, dx) === 'go to 12C — DX catch Far Away, last heard 1 day ago', 'the hover hint names the catch');
}

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
