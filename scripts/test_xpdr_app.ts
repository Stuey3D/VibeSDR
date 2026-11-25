// test_xpdr_app.ts — ★★★ beta94 THE TRANSPONDER SIGNAL FINDER, the clients' half (src/services/xpdr.ts, the overlay through
// src/utils/orbcommView.ts, the pick in src/services/satellites.ts): xpdr_status parsed defensively (junk → null or dropped,
// never a throw), the rows' order and words, LISTEN only on voices (USB, the follow message), the waterfall overlay
// (ORBCOMM's: the slice titled, an arrow per signal, faded ones hollow). The server side is vibeserver/test-xpdr.cpp and
// scripts/test-server-xpdr.mjs. Silent. Run: npx tsx scripts/test_xpdr_app.ts
import { parseXpdrStatus, xpdrAgo, xpdrFaded, xpdrFollowText, xpdrFreqText, xpdrListen, xpdrOverlayFrom, xpdrRowsSorted, xpdrWhat,
         XPDR_FADE_SEC, XPDR_SLICE_LABEL } from '../src/services/xpdr';
import { orbOverlayKey, orbOverlayLayout, ORB_SLICE_LABEL } from '../src/utils/orbcommView';
import { parseSatPicked, satDecView, satDecWord } from '../src/services/satellites';
import { CLIENT_DECODERS } from '../src/constants/modeBox';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const noThrow = (f: () => unknown) => { try { f(); return true; } catch { return false; } };

console.log('── xpdr_status, parsed defensively ──');
const real = {
  slice: { lo: 435597250, hi: 435677750 }, span: { lo: 435605000, hi: 435670000 }, beacon: 435605000, dop: -4804.4,
  rows: [
    { id: 'c14', kind: 'cw', hz: 435600196, dial: 435605000, snr: 36.7, ago: 1, on: true, beacon: true, wpm: 16, text: 'RS44 RS44 BEACON' },
    { id: 'v1', kind: 'voice', hz: 435618643, dial: 435626765, snr: 36.9, ago: 0, on: true },
    { id: 'c13', kind: 'cw', hz: 435645195, dial: 435649999, snr: 35.2, ago: 1, on: true, wpm: 22, call: 'G4ABC', text: 'CQ SAT DE G4ABC G4ABC K' },
    { id: 'v2', kind: 'voice', hz: 435660000, dial: 435658500, snr: 12, ago: 95, on: false },
  ],
  follow: { id: 'v1', src: 'voice', off: 54 },
};
const st = parseXpdrStatus(real);
ok(!!st && st.rows.length === 4 && st.slice?.lo === 435597250 && st.beacon === 435605000 && st.dop === -4804.4, 'a real status parses');
ok(st?.follow?.id === 'v1' && st.follow.src === 'voice' && st.follow.off === 54, 'the follow');
ok(parseXpdrStatus(null) === null && parseXpdrStatus('x') === null && parseXpdrStatus(42) === null, 'not an object → null');
ok(noThrow(() => parseXpdrStatus({ rows: 'x', slice: [1, 2], follow: 7 })), 'junk fields: no throw');
const junk = parseXpdrStatus({ slice: { lo: 5, hi: 3 }, rows: [{ id: 'v9', kind: 'speech', hz: 1, dial: 1 }, { id: '', kind: 'cw', hz: 1, dial: 1 },
  { id: 'c1', kind: 'cw', hz: -1, dial: 1 }, { id: 'c2', kind: 'cw', hz: 435e6, dial: 435e6, text: 'A\u0007BéC', call: 'G4ABC<script>' }] });
ok(!!junk && junk.slice === null && junk.rows.length === 1, 'a backwards slice and bad rows dropped');
ok(junk?.rows[0].text === 'ABC' && junk.rows[0].call === 'G4ABCSCRIPT', 'text whitelisted to printable ASCII; a call to callsign characters');
ok(parseXpdrStatus({ rows: Array.from({ length: 200 }, (_, i) => ({ id: `c${i}`, kind: 'cw', hz: 435e6 + i, dial: 435e6 + i })) })!.rows.length === 40, '40 rows at most');
ok(parseXpdrStatus({ rows: [], follow: { id: 'v1', src: 'magic' } })?.follow?.src === '', 'an unknown follow source = ""');

console.log('── the rows: order and words ──');
const rows = xpdrRowsSorted(st!.rows);
ok(rows[0].id === 'v1' && rows[rows.length - 1].id === 'v2', `voices on air first, the long-silent last (${rows.map((r) => r.id).join(' ')})`);
ok(xpdrWhat(st!.rows[0]) === 'BEACON 16 wpm' && xpdrWhat(st!.rows[2]) === 'CW 22 wpm G4ABC' && xpdrWhat(st!.rows[1]) === 'VOICE', 'BEACON / CW … G4ABC / VOICE');
ok(xpdrFreqText(st!.rows[1]) === '435.6268', 'the frequency is the DIAL (satellite frame) — what the readout shows after LISTEN');
ok(xpdrAgo(st!.rows[1]) === 'now' && xpdrAgo(st!.rows[3]) === '2 min ago', 'now / 2 min ago');
ok(!xpdrFaded(st!.rows[1]) && xpdrFaded(st!.rows[3]) && XPDR_FADE_SEC === 60, 'a row unheard for a minute fades (Stuart)');
ok(xpdrFollowText(st!.follow) === 'VFO A follows · measured from the voice' && xpdrFollowText(null) === '', 'the follow in words');
ok(xpdrFollowText({ id: 'v1', src: 'held', off: 0 }).includes('held between overs'), '…held between overs');

console.log('── LISTEN ──');
const l = xpdrListen(st!.rows[1]);
ok(!!l && l.hz === 435626765 && l.mode === 'usb' && l.follow.type === 'xpdr_follow' && l.follow.id === 'v1', 'a voice: VFO A to its carrier in USB, then follow it');
ok(xpdrListen(st!.rows[0]) === null && xpdrListen(st!.rows[2]) === null, 'CW rows have no LISTEN (Stuart: "CW simply decodes")');
ok(xpdrListen(st!.rows[1], 'lsb')?.mode === 'lsb', 'an LSB downlink listens in LSB');

console.log('── the waterfall overlay (ORBCOMM\'s) ──');
const ov = xpdrOverlayFrom(st);
ok(!!ov && ov.title === XPDR_SLICE_LABEL && ov.title !== ORB_SLICE_LABEL && ov.lo === 435597250 && ov.hi === 435677750, 'the slice, titled for the finder');
ok(ov!.carriers.length === 4 && ov!.carriers.find((c) => c.rf === 435618643)?.label === 'VOICE' && ov!.carriers.find((c) => c.rf === 435645195)?.label === 'G4ABC'
   && ov!.carriers.find((c) => c.rf === 435600196)?.label === 'BEACON', 'an arrow per signal at its RF: VOICE, the call once known, BEACON');
ok(ov!.carriers.find((c) => c.rf === 435660000)?.locked === false && ov!.carriers.find((c) => c.rf === 435618643)?.locked === true, 'heard = solid, faded = hollow');
ok(xpdrOverlayFrom(null) === null && xpdrOverlayFrom({ ...st!, slice: null }) === null, 'no slice → nothing drawn');
ok(orbOverlayKey(ov) !== orbOverlayKey({ ...ov!, title: undefined }), 'the redraw key changes with the title');
const L = orbOverlayLayout(ov, (hz) => (hz - 435590000) / 100, 1000, 120);
ok(!!L.band && L.marks.length === 4, 'laid out by ORBCOMM\'s layout: the band and four marks');

console.log('── the pick ──');
const pk = parseSatPicked({ type: 'sat', state: 'picked', norad: 44909, tx: 0, decoder: 'xpdr', params: { width: 98524 } });
ok(pk?.decoder === 'xpdr', 'picked decoder "xpdr" is understood (an older client reads "" — voice, as before)');
ok(satDecView('xpdr') === 'xpdr' && satDecWord('xpdr', 'voice', false, false).length > 0, 'the box has a view and words for it');
ok((CLIENT_DECODERS as readonly string[]).includes('xpdr'), 'the mode box knows the decoder');

console.log(`\n${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
