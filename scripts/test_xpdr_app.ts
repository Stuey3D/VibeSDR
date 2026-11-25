// test_xpdr_app.ts — ★★★ beta94 THE TRANSPONDER SIGNAL FINDER, the clients' half (src/services/xpdr.ts, the overlay through
// src/utils/orbcommView.ts, the pick in src/services/satellites.ts): xpdr_status parsed defensively (junk → null or dropped,
// never a throw), the rows' order and words, LISTEN only on voices (USB, the follow message), the waterfall overlay
// (ORBCOMM's: the slice titled, an arrow per signal, faded ones hollow). The server side is vibeserver/test-xpdr.cpp and
// scripts/test-server-xpdr.mjs. Silent. Run: npx tsx scripts/test_xpdr_app.ts
import { parseXpdrStatus, xpdrAgo, xpdrFaded, xpdrFollowText, xpdrFreqText, xpdrListen, xpdrOverlayFrom, xpdrRowsSorted, xpdrWhat,
         XPDR_FADE_SEC, XPDR_SLICE_LABEL } from '../src/services/xpdr';
import { orbOverlayKey, orbOverlayLayout, ORB_SLICE_LABEL } from '../src/utils/orbcommView';
import { parseSatPicked, parseSatReadout, satChanText, satDecView, satDecWord, satDopplerBadge, satFollowHz, satMarkerShiftHz,
         SatFollowDial, SAT_FOLLOW_TUNE_GUARD_MS } from '../src/services/satellites';
import { shouldForwardTune } from '../src/services/audioTuneForward';
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

console.log('── the dial follows the followed station (Stuart 11-25: "yes") ──');
{
  const NOM = 435626765;
  const sat = (follow: number | undefined, extra: Record<string, unknown> = {}) => parseSatReadout({ type: 'sat', state: 'TRACK', name: 'RS-44', norad: 44909,
    nominal: NOM, offset: -4804 + (follow ?? 0), rate: -12, ...(follow != null ? { follow } : {}), ...extra })!;
  const r = sat(-1234.5);
  ok(r.follow === -1234.5 && satFollowHz(r) === -1234.5, 'the sat message\'s follow parses');
  ok(parseSatReadout({ type: 'sat', state: 'TRACK', nominal: NOM, offset: 1, follow: 'x' })!.follow === undefined
     && parseSatReadout({ type: 'sat', state: 'TRACK', nominal: NOM, offset: 1, follow: 9e9 })!.follow === undefined
     && parseSatReadout({ type: 'sat', state: 'TRACK', nominal: NOM, offset: 1, follow: 0 })!.follow === undefined, 'junk / wild / zero follow = none');
  ok(satFollowHz(parseSatReadout({ type: 'sat', state: 'ARMED', nominal: NOM, offset: 0, follow: 50 })) === 0, 'no follow outside TRACK / COAST');
  ok(satMarkerShiftHz(r) === -4804, `★ the marker: offset less the follow — the dial carries it, never counted twice (${satMarkerShiftHz(r)})`);
  ok(satMarkerShiftHz(sat(undefined)) === -4804 && satMarkerShiftHz(null) === 0, '…no follow: the offset as before; no lock: 0');
  ok(/▼ 4\.80 kHz/.test(satChanText(r)) && /4\.80 kHz/.test(satDopplerBadge(r)!.label) && satDopplerBadge(r)!.dir === -1, 'the Doppler words say the Doppler, not Doppler + follow');

  const d = new SatFollowDial();
  let dial = NOM;   // LISTEN put the dial here
  const step = (m: ReturnType<typeof sat> | null, since = 60000) => { const hz = d.onReadout(m, since, dial); if (hz != null) dial = hz; return hz; };
  ok(step(sat(undefined)) === null && dial === NOM, 'no follow yet: the dial stays where LISTEN put it');
  ok(step(sat(-300.4)) === NOM - 300 && dial === NOM - 300, `★ following: the digits read nominal + follow (${dial})`);
  ok(step(sat(-300.2)) === null, 'unchanged to the hertz: nothing to redraw');
  ok(step(sat(-375)) === NOM - 375, 'the follow slews: the digits move with it');
  ok(d.holds(NOM) && !d.holds(NOM + 5000), "a config's vfo = the lock's nominal is the server restating its dial, not a move to adopt");
  // the follow ends WITHOUT a tune (the finder stopped / clearFollow: the channel eases back) → the nominal
  ok(step(sat(-150)) === NOM - 150 && step(sat(undefined)) === NOM && dial === NOM && !d.holds(NOM), '★ the follow ends without a tune: the digits go back to the nominal');
  // a manual step from the followed position, then the race
  step(sat(-800)); const followed = dial;
  ok(followed === NOM - 800, 'following again');
  dial = followed + 100;   // ★ the person steps +100 from where the station is (the step base IS the following dial)
  const NEWNOM = dial;     // the server's nudge re-bases the nominal at the tune and ends the follow
  ok(step(sat(-800), 200) === null && dial === followed + 100, '★ a sat message in flight with the old follow, just after our tune: ignored (no snap back)');
  ok(step(sat(undefined, { nominal: NEWNOM, offset: -4804 }), SAT_FOLLOW_TUNE_GUARD_MS + 10) === null && dial === followed + 100,
     '★ the step landed from the followed position (station + 100 Hz) and stays: the follow ended by OUR tune');
  // somebody else moved the dial while it followed (a config adopt): the end of the follow does not pull it back
  step(sat(-500)); dial = NOM + 20000;
  ok(step(sat(undefined)) === null && dial === NOM + 20000, 'the dial moved by something else: the end of the follow leaves it alone');
  // the lock goes while following: back to the nominal (only while the digits still show the follow)
  step(sat(-600));
  ok(step(parseSatReadout({ type: 'sat', state: 'OFF', why: 'x' })) === NOM && dial === NOM, 'the lock ends while following: the digits back to the nominal');

  // ★★ the app's audio socket never sends what the follow put on the screen: serverDial carries the followed value
  const asked = { frequency: NOM - 375, mode: 'usb', bandwidthLow: 0, bandwidthHigh: 2800 };
  const base = { asked, seqAdvanced: false, sealed: false, helloTune: '', askedJson: JSON.stringify(asked), msSinceSend: 60000 };
  ok(!shouldForwardTune({ ...base, serverDial: { frequency: NOM - 375, mode: 'usb' } }).send, '★ the app: a follow-driven dial is NOT forwarded as a tune (serverDial says it)');
  ok(!shouldForwardTune({ ...base, sealed: true, serverDial: { frequency: NOM - 375, mode: 'usb' } }).send, '…nor by a sealed joiner on a shared dial');
  ok(shouldForwardTune({ ...base, seqAdvanced: true, asked: { ...asked, frequency: NOM - 275 }, serverDial: { frequency: NOM - 375, mode: 'usb' } }).send,
     '…while a person\'s step from it is');
}

console.log(`\n${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
