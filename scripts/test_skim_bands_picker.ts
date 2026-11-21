// test_skim_bands_picker.ts — the skimmers' bands and windows (src/utils/skimBands.ts — one copy for the web decoder box,
// the app's ModeSelector and the Android server screen's background rows). V12 beta90, Stuart 2026-11-21: "cw/rtty
// skimmer could have a band selection in the decoder box header and the normal tune buttons could fine tune it", and
// "maybe list multiple centre points per band".
import { parseSkimBands, skimDial, skimPickable, skimBandAtDial, skimOffLabel, skimPoints, skimPointAt, skimPointLabel } from '../src/utils/skimBands.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
console.log('Skimmer bands and windows (web + app)');

const P = (...hz: (number | [number, string])[]) => hz.map((h) => (Array.isArray(h) ? { hz: h[0], note: h[1] } : { hz: h }));
// the server's skimBands (vibe_skim_bands.h points(): tiles from the bottom, then practice segments) — Region 2
const sb = parseSkimBands({ region: 2,
  cw: [{ id: '160m', name: '160 m', lo: 1810000, hi: 1840000, points: { 24000: P(1822000, 1846000), 48000: P(1834000) } },
       { id: '60m', name: '60 m', lo: 5351500, hi: 5366500, points: { 24000: P([5363500, '']) } },
       { id: '40m', name: '40 m', lo: 7000000, hi: 7040000, points: { 24000: P(7012000, 7036000), 48000: P(7024000), 96000: P(7048000) } },
       { id: '20m', name: '20 m', lo: 14000000, hi: 14070000, points: { 24000: P(14012000, 14036000, 14060000) } }],
  rtty: [{ id: '40m', name: '40 m', lo: 7040000, hi: 7050000, points: { 48000: P(7064000, [7104000, 'US RTTY']) } }, { id: 'bad', name: 'x', lo: 5, hi: 1 }], psk: [] })!;
ok(!!sb && sb.cw.length === 4 && sb.rtty.length === 1 && sb.psk.length === 0, 'the server\'s skimBands read with their windows (a malformed band dropped)');
ok(parseSkimBands(undefined) === null && parseSkimBands({ cw: [], rtty: [], psk: [] }) === null, 'an older server (none): null — no picker drawn');
const b40 = sb.cw[2];
ok(skimPoints(b40, 24000).map((q) => q.hz).join() === '7012000,7036000' && skimPoints(b40, 48000)[0].hz === 7024000 && skimPoints(b40, 96000)[0].hz === 7048000,
   '40 m CW: 24 kHz 7012 + 7036, 48 kHz 7024, 96 kHz 7048 (the skimmer\'s CURRENT width)');
ok(skimPointLabel(skimPoints(b40, 24000)[1]) === '7024–7048 kHz' && skimPointLabel(skimPoints(sb.rtty[0], 48000)[1]) === '7080–7128 kHz · US RTTY',
   'labels: "7024–7048 kHz", a practice window "7080–7128 kHz · US RTTY"');
ok(skimPointLabel(skimPoints(sb.cw[1], 24000)[0]) === '5351.5–5375.5 kHz', '60 m: "5351.5–5375.5 kHz" — no licence note (we only receive)');
ok(skimDial(b40, 24000) === 7012000, 'a band pick goes to its first window');
ok(skimPoints(parseSkimBands({ region: 1, cw: [{ id: '40m', name: '40 m', lo: 7000000, hi: 7040000 }] })!.cw[0], 48000)[0].hz === 7024000,
   'an older server (no points): one window, lo + width / 2');
ok(skimPointAt(sb.cw, 24000, 7036000)?.band.id === '40m' && skimPointAt(sb.cw, 24000, 7030000) === null, 'a dial on 7036 is 40 m\'s second window; 7030 is none');
// reach: a radio locked to 6.95–7.06 MHz
const locked = (hz: number) => hz >= 6950000 && hz <= 7060000;
const lk = skimPickable(sb.cw, 24000, locked);
ok(lk.map((x) => x.band.id).join() === '40m' && lk[0].points.map((q) => q.hz).join() === '7012000,7036000', 'locked 6.95–7.06 MHz: only 40 m, both its 24 kHz windows');
ok(skimPickable(sb.cw, 96000, locked).length === 0, '…a 96 kHz window that does not fit the lock: no band (the row is not drawn)');
ok(skimPickable(sb.cw, 24000, (hz) => hz >= 14000000 && hz <= 14050000)[0].points.length === 2, 'a range to 14.050: 20 m\'s third window (14048–14072) is left out');
ok(skimPickable(sb.cw, 24000, () => true).length === 4, 'no limits: every band');
// the dial
const ex = skimBandAtDial(sb.cw, 24000, 7036000);
ok(ex.exact && ex.band?.id === '40m' && ex.point?.hz === 7036000, 'dial 7036 at 24 kHz: 40 m, its second window, exactly');
const tuned = skimBandAtDial(sb.cw, 48000, 7026500);
ok(!tuned.exact && tuned.band?.id === '40m' && skimOffLabel(tuned) === '40 m · tuned', 'fine-tuned to 7026.5: "40 m · tuned" — it never snaps back');
const off = skimBandAtDial(sb.cw, 48000, 10000000);
ok(!off.exact && off.band === null && skimOffLabel(off) === '—', 'on no band (10 MHz): "—"');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
