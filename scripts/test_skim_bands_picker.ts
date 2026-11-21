// test_skim_bands_picker.ts — the listener's skimmer BAND picker (src/utils/skimBands.ts — one copy for the web decoder box
// and the app's ModeSelector). V12 beta90, Stuart 2026-11-21: "cw/rtty skimmer could have a band selection in the decoder
// box header and the normal tune buttons could fine tune it if needed".
import { parseSkimBands, skimDial, skimPickable, skimBandAtDial, skimOffLabel } from '../src/utils/skimBands.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
console.log('Skimmer band picker (web + app)');

const sb = parseSkimBands({ region: 1,
  cw: [{ id: '160m', name: '160 m', lo: 1810000, hi: 1838000 }, { id: '40m', name: '40 m', lo: 7000000, hi: 7040000 }, { id: '20m', name: '20 m', lo: 14000000, hi: 14070000 }],
  rtty: [{ id: '40m', name: '40 m', lo: 7040000, hi: 7050000 }, { id: 'bad', name: 'x', lo: 5, hi: 1 }], psk: [] })!;
ok(!!sb && sb.cw.length === 3 && sb.rtty.length === 1 && sb.psk.length === 0, 'the server\'s skimBands read (a malformed band dropped)');
ok(parseSkimBands(undefined) === null && parseSkimBands({ cw: [], rtty: [], psk: [] }) === null, 'an older server (none): null — no picker drawn');
const b40 = sb.cw[1];
ok(skimDial(b40, 24000) === 7012000 && skimDial(b40, 48000) === 7024000 && skimDial(b40, 96000) === 7048000, '40 m CW: 24 → 7012, 48 → 7024, 96 → 7048 (the skimmer\'s CURRENT width)');
ok(skimDial(sb.rtty[0], 48000) === 7064000, '40 m RTTY at 48 kHz → 7064');
// reach: a radio locked to 6.95–7.15 MHz
const locked = (hz: number) => hz >= 6950000 && hz <= 7150000;
ok(skimPickable(sb.cw, 48000, locked).map((b) => b.id).join() === '40m', 'locked 6.95–7.15 MHz: only 40 m is offered');
ok(skimPickable(sb.cw, 96000, (hz) => hz >= 6950000 && hz <= 7060000).length === 0, '…a 96 kHz window that does not fit the lock: no band (the row is not drawn)');
ok(skimPickable(sb.cw, 48000, () => true).length === 3, 'no limits: every band');
// the dial
ok(skimBandAtDial(sb.cw, 48000, 7024000).exact && skimBandAtDial(sb.cw, 48000, 7024000).band?.id === '40m', 'dial 7024 at 48 kHz: 40 m, exactly');
const tuned = skimBandAtDial(sb.cw, 48000, 7026500);
ok(!tuned.exact && tuned.band?.id === '40m' && skimOffLabel(tuned) === '40 m · tuned', 'fine-tuned to 7026.5: "40 m · tuned" — it never snaps back');
const off = skimBandAtDial(sb.cw, 48000, 10000000);
ok(!off.exact && off.band === null && skimOffLabel(off) === '—', 'on no band (10 MHz): "—"');
ok(skimBandAtDial(sb.cw, 96000, 7024000).exact === false, 'the same dial at another width is not that width\'s centre (7024 at 96 kHz → tuned)');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
