// test_balloon_app.ts — V12 balloons, the client half (src/services/balloons.ts — shared by the app and the web client): the
// server's report sanitised, the 0x0B picture frame read, the words, the attach parameters for AUTO and the manual pins,
// and the keys wired in both clients (only where the server offers "balloon"). Run: npx tsx scripts/test_balloon_app.ts
import { readFileSync } from 'node:fs';
import { BALLOON_AUTO, BALLOON_BAUDS, BALLOON_HINT, balloonIsAuto, balloonLine, balloonMerge, balloonParams, balloonPicture, balloonPictureText,
         balloonPinLabel, balloonSanitise, balloonStatus, balloonWhen } from '../src/services/balloons';
import { decoderNear, DECODER_PLACES } from '../src/utils/decoderBands';

let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`FAIL ${what}\n   got  ${g}\n   want ${w}`); }
};
const ok = (what: string, c: boolean) => eq(what, c, true);

// ★ the server's shape (vibe_balloon_run.h sendJson_)
const msg = {
  now: 1794840600, state: 3,
  rtty: { centre: 1500, shift: 600, settings: '300 Bd · 8N1 · 600 Hz', candidates: 24, rejected: 1 },
  horus: { tones: [1100, 1370, 1640, 1910], baud: 100, packets: 2 },
  payloads: [
    { call: 'HORUS-256', src: 'horus', count: 502, time: '10:00:35', lat: 52.211, lon: 0.121, alt: 31250, speed: 42, sats: 11, temp: -31, batt: 2.94,
      heard: 1794840590, n: 2, fec: 3, sentence: '$$HORUS-256,502,10:00:35,52.21100,0.12100,31250,42,11,-31,2.94*4A58', extra: [] },
    { call: 'VIBE1', src: 'ukhas', count: 2, time: '10:00:05', lat: 52.20601, lon: 0.1195, alt: 31050, speed: null, sats: null, temp: null, batt: null,
      heard: 1794840480, n: 1, fec: 0, sentence: '$$VIBE1,2,10:00:05,52.20601,0.11950,31050,9,-12*F2AB', extra: ['9', '-12'] },
    { call: 'NOFIX', src: 'ukhas', count: 7, time: '', lat: null, lon: null, alt: 0, heard: 1794840000, n: 1, fec: 0, sentence: '', extra: [] },
    { call: 'BAD', src: 'ukhas', count: 1, lat: 95, lon: 200, heard: 1, n: 1 },
    'junk', { src: 'ukhas' },
  ],
  images: [{ key: 'VIBE1#7', call: 'VIBE1', image: 7, have: 12, expected: 30, complete: false, mcus: 100, total: 240 }, { nokey: 1 }],
};
const r = balloonSanitise(msg)!;
ok('sanitised', !!r && r.state === 3 && r.rtty.settings === '300 Bd · 8N1 · 600 Hz' && r.horus.tones!.length === 4);
eq('payloads: junk and nameless dropped, order kept', r.payloads.map((p) => p.call), ['HORUS-256', 'VIBE1', 'NOFIX', 'BAD']);
eq('an impossible position is no position', [r.payloads[3].lat, r.payloads[3].lon], [null, null]);
eq('images: one, the keyless dropped', r.images.map((i) => i.key), ['VIBE1#7']);
eq('not an object', balloonSanitise('x'), null);
eq('a state it does not know is 0', balloonSanitise({ ...msg, state: 7 })!.state, 0);
eq('tones not four numbers: none', balloonSanitise({ ...msg, horus: { tones: [1, 2, 'x', 4] } })!.horus.tones, null);
eq('the Horus line', balloonLine(r.payloads[0]), '52.21100, 0.12100 · 31,250 m · 42 km/h · 11 sats · -31 °C · 2.94 V');
eq('the UKHAS line', balloonLine(r.payloads[1]), '52.20601, 0.11950 · 31,050 m');
eq('no fix', balloonLine(r.payloads[2]).startsWith('no GPS fix'), true);
eq('when', balloonWhen(r.payloads[1], r.now), '#2 · 10:00:05 · heard 2 min ago · UKHAS');
eq('when (Horus, seconds)', balloonWhen(r.payloads[0], r.now), '#502 · 10:00:35 · heard 10 s ago · Horus');
eq('status: both found', balloonStatus(r), 'decoding · RTTY 300 Bd · 8N1 · 600 Hz · Horus 4-FSK (270 Hz spacing)');
eq('status: before a report', balloonStatus(null), 'listening for a balloon…');
eq('status: tones, no settings yet', balloonStatus({ ...r, state: 1, rtty: { ...r.rtty, settings: '' }, horus: { ...r.horus, tones: null } }), 'RTTY tones 600 Hz apart — working out its settings…');
eq('status: nothing', balloonStatus({ ...r, state: 0, rtty: { ...r.rtty, settings: '', shift: 0 }, horus: { ...r.horus, tones: null } }).startsWith('listening — tune a payload in USB'), true);
eq('picture text', balloonPictureText(r.images[0]), 'VIBE1 picture 7 · 12 of 30 packets');
// ★ the box keeps every payload since it opened (a re-attach starts the server's list again)
const kept = balloonMerge([], r);
eq('merge: newest heard first', kept.map((p) => p.call), ['HORUS-256', 'VIBE1', 'NOFIX', 'BAD']);
const fresh = balloonSanitise({ ...msg, now: msg.now + 30, payloads: [{ ...msg.payloads[1] as object, count: 3, heard: msg.now + 25 }] })!;
const k2 = balloonMerge(kept, fresh);
eq('merge: an empty-ish fresh run keeps the rest, the newer report replaces its own', [k2.length, k2[0].call, k2[0].count], [4, 'VIBE1', 3]);
eq('merge: an older report never replaces a newer one', balloonMerge(k2, r).find((p) => p.call === 'VIBE1')!.count, 3);
eq('merge: nothing in, nothing lost', balloonMerge(k2, null).length, 4);

// ── the 0x0B frame ──
const key = 'VIBE1#7';
const jpeg = [0xff, 0xd8, 1, 2, 3, 0xff, 0xd9];
const f = new Uint8Array([0x0b, key.length, ...[...key].map((c) => c.charCodeAt(0)), 0, 12, 0, 30, 0, 0, 96, 0, 64, ...jpeg]);
const p = balloonPicture(f)!;
eq('the frame', [p.key, p.have, p.expected, p.complete, p.w, p.h, [...p.jpeg]], [key, 12, 30, false, 96, 64, jpeg]);
eq('…from an ArrayBuffer too', balloonPicture(f.buffer)!.key, key);
eq('a short frame is refused', balloonPicture(f.subarray(0, 10)), null);
eq('not a JPEG: refused', balloonPicture(new Uint8Array([...f.subarray(0, f.length - jpeg.length), 0x89, 0x50, 0x4e, 0x47, 0, 0])), null);
eq('another opcode: refused', balloonPicture(new Uint8Array([0x01, ...f.subarray(1)])), null);

// ── AUTO and the pins ──
eq('AUTO sends nothing', balloonParams(BALLOON_AUTO), {});
ok('AUTO is AUTO', balloonIsAuto(BALLOON_AUTO));
eq('pins', balloonParams({ baud: 300, bits: 8, reverse: 1, shift: 600, horusSpacing: 270 }), { baud_rate: 300, bits: 8, reverse: 1, shift: 600, horus_spacing: 270 });
eq('reverse 0 is a pin (normal), -1 is AUTO', [balloonParams({ ...BALLOON_AUTO, reverse: 0 }), balloonParams({ ...BALLOON_AUTO, reverse: -1 })], [{ reverse: 0 }, {}]);
eq('labels', [balloonPinLabel('baud', 0), balloonPinLabel('baud', 300), balloonPinLabel('bits', 7), balloonPinLabel('reverse', -1), balloonPinLabel('reverse', 0), balloonPinLabel('reverse', 1), balloonPinLabel('shift', 425)],
   ['AUTO', '300', '7-bit', 'AUTO', 'NORM', 'REV', '425 Hz']);
eq('speeds offered', [...BALLOON_BAUDS], [0, 50, 75, 100, 150, 300, 600]);
ok('the hint says where to tune and in what', BALLOON_HINT.includes('USB') && BALLOON_HINT.includes('434') && BALLOON_HINT.includes('SSDV'));
ok('the key sits on 70 cm (decoder tabs)', decoderNear('balloon', 434.2e6) && !decoderNear('balloon', 144.8e6));
ok('…under Amateur on the UHF tab', DECODER_PLACES.uhf.some((g) => g.group === 'amateur' && g.ids.includes('balloon')));

// ── both clients wired, and only where the server offers it ──
const src = (q: string) => readFileSync(new URL('../' + q, import.meta.url), 'utf8');
const html = src('web/client/index.html'), main = src('web/client/src/main.ts'), dec = src('web/client/src/decoders.ts');
ok('web: the BALLOONS key, hidden until the server says so', /data-dec="balloon" hidden/.test(html));
ok('web: shown only where "balloon"', main.includes('balloonCap = j.balloon === true || !!decWhy.balloon'));
ok('web: the rows, the pictures and the pins', html.includes('id="balloonRows"') && html.includes('id="balloonPics"') && html.includes('id="balloonBaud"') && main.includes('function renderBalloon'));
ok('web: the JSON and the pictures routed', dec.includes("msg.type === 'balloon'") && dec.includes("balloon: 'balloon'") && dec.includes('onBalloonPicture'));
ok('web: the pins are what is attached', main.includes("if (mode === 'balloon') return balloonParams(balloonSet);"));
ok('web: the rows are kept across a re-attach', main.includes('balloonSeen = balloonMerge(balloonSeen, r)'));
ok('web: the gallery shows a .jpg as a picture', main.includes('/\\.(png|jpg)$/i.test(it.f)') && html.includes('data-kind="balloon"'));
const ms = src('src/components/ModeSelector.tsx'), sd = src('src/screens/SDRScreen.tsx'), dc = src('src/services/DecoderClient.ts');
ok('app: the key only where offered', ms.includes("const balloonShown = !!decoderControls?.balloonAvail && !isBlocked('balloon')") && sd.includes('balloonAvail: balloonCapable'));
ok('app: the pins under the key', ms.includes("row('RTTY SPEED', 'baud', BALLOON_BAUDS)") && sd.includes('balloonSettings, onBalloonSettings'));
ok('app: a pin re-attaches in place, the rows kept', sd.includes("decoderClient.current.start('balloon')") && sd.includes('setBalloonSeen((prev) => balloonMerge(prev, r))'));
ok('app: attach + JSON + pictures', dc.includes("case 'balloon': return { extension_name: 'balloon', params: balloonParams(this.balloonSet) };")
   && dc.includes("m.type === 'balloon'") && dc.includes("name === 'balloon' && t === 0x0b"));
ok('app: the gallery shows a .jpg as a picture', src('src/components/GalleryOverlay.tsx').includes('/\\.(png|jpg)$/i'));
ok('app: the phone\'s server has the switch', src('src/screens/ServerModeScreen.tsx').includes("{ id: 'balloon', label:"));
ok('About: the V12 note', src('src/components/AboutOverlay.tsx').includes('BALLOONS \\u2014 HIGH-ALTITUDE BALLOON'));
ok('the tour says where the key is', sd.includes('on the UHF tab, Amateur holds BALLOONS'));
ok('the web tutorial says where the key is', src('web/client/src/tutorial.ts').includes('its Amateur group holds BALLOONS'));

console.log(`${fail ? 'FAIL' : 'ok '}  test_balloon_app — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
