/**
 * ★★★ SHARED DECODERS — the clients' half (src/services/sharedDecoders.ts; Stuart, 2026-11-24: "if the aprs decoder is open
 * for user 1 then user 2 connecting it should be open for … if user 2 closes it then it closes for them only").
 * The parser, the server-extension → box map, the follow / close-remember rule, and that the web page and the app both wire
 * it the same way (follow with the server's own extension and parameters; a close remembered; a press wins).
 * The server half is test-shared-decoders (C++) and scripts/test-server-shared-decoders.mjs (the real server).
 */
import { readFileSync } from 'node:fs';
import { parseDecShared, sharedDecoderKey, SharedFollower, OWN_PRESS_MS, type SharedDecoder, type SharedNow } from '../src/services/sharedDecoders';

let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) { pass++; console.log('   ok  ', m); } else { fail++; console.log('   FAIL', m); } };

const APRS = { slot: 'decoder', ext: 'aprs', params: { baud_rate: 1200, packet: 'auto' }, key: 'aprs|k1', held: false, mine: false, n: 1 };
const msg = (...l: object[]) => ({ type: 'dec_shared', list: l });

console.log('the message');
{
  const l = parseDecShared(msg(APRS))!;
  ok(l.length === 1 && l[0].ext === 'aprs' && l[0].params.baud_rate === 1200 && l[0].params.packet === 'auto' && !l[0].held, 'parsed');
  ok(parseDecShared({ type: 'sat' }) === null, 'not ours = null');
  ok(parseDecShared(msg())!.length === 0, 'an empty list = nothing runs');
  const bad = parseDecShared(msg({ ...APRS, ext: '<img>' }, { ...APRS, slot: 'x' }, { ...APRS, key: '' }))!;
  ok(bad.length === 0, 'a bad extension, slot or key is dropped');
  const p = parseDecShared(msg({ ...APRS, params: { ok: 1, 'bad-name': 2, nest: { a: 1 }, long: 'x'.repeat(200), f: Infinity } }))!;
  ok(JSON.stringify(p[0].params) === '{"ok":1}', `only flat, plain, bounded parameters are kept (${JSON.stringify(p[0].params)})`);
  ok(parseDecShared(msg(...Array.from({ length: 50 }, () => APRS)))!.length <= 4, 'bounded list');
}

console.log('the server\'s extension → the box');
{
  const m: Record<string, string | null> = { fsk: 'rtty', rttyskim: 'rtty', cwskim: 'cw', cw: 'cw', pskskim: 'psk', msf: 'time', wwv: 'time',
    tdf: 'time', time: 'time', aprs: 'aprs', lrpt: 'lrpt', navtex: 'navtex', wefax: 'wefax', paging: null, rds: null, dab: null, nope: null };
  for (const [ext, box] of Object.entries(m)) ok(sharedDecoderKey(ext) === box, `${ext} → ${box}`);
}

const now = (o: Partial<SharedNow> = {}): SharedNow => ({ box: null, busy: false, spotsOn: false, canShow: () => true, now: 100000, ...o });
const L = (...e: Partial<SharedDecoder>[]) => e.map((x) => ({ ...APRS, ...x }) as SharedDecoder);

console.log('the rule');
{
  const f = new SharedFollower();
  let a = f.receive(L({}), now());
  ok(a.length === 1 && a[0].kind === 'follow' && a[0].kind === 'follow' && a[0].box === 'aprs', 'listener 2 arrives: the room\'s APRS is followed');
  if (a[0].kind === 'follow') ok(a[0].entry.params.baud_rate === 1200 && a[0].entry.ext === 'aprs', '…with the server\'s own extension and parameters');
  f.noteFollowed('decoder', 'aprs|k1');
  ok(f.receive(L({}), now({ box: 'aprs' })).length === 0, 'a broadcast before the join is confirmed does not open it twice');
  ok(f.receive(L({ held: true, n: 2 }), now({ box: 'aprs' })).length === 0, 'held: nothing to do');
  f.noteClosed('decoder', 'aprs');
  ok(f.closed('decoder') === 'aprs|k1', 'listener 2 closes the box: that run is remembered');
  ok(f.receive(L({ held: false, n: 1 }), now()).length === 0, '…and NOT reopened on the next broadcast (it still runs for listener 1)');
  a = f.receive(L({ ext: 'navtex', key: 'navtex|k2' }), now());
  ok(a.length === 1 && a[0].kind === 'follow' && a[0].box === 'navtex', 'a DIFFERENT decoder starts in the room: followed');
  f.noteFollowed('decoder', 'navtex|k2');
  f.noteClosed('decoder', 'navtex');
  ok(f.receive([], now()).length === 0 && f.closed('decoder') === '', 'the room\'s decoder stops: the close is forgotten');
  ok(f.receive(L({ ext: 'navtex', key: 'navtex|k2' }), now()).length === 1, '…so the same decoder started again later is a new run, followed');
}
{
  const f = new SharedFollower();
  f.receive(L({}), now());
  f.noteClosed('decoder', 'aprs');
  f.noteOwnOpen('decoder', 100000);
  ok(f.closed('decoder') === '', 'opening a decoder themselves forgets what they had closed');
  ok(f.receive(L({ key: 'aprs|k9' }), now({ now: 100000 + OWN_PRESS_MS - 1 })).length === 0, 'their own press wins over a broadcast already on its way');
  ok(f.receive(L({ key: 'aprs|k9' }), now({ now: 100000 + OWN_PRESS_MS + 1 })).length === 1, '…for a moment only');
}
{
  const f = new SharedFollower();
  ok(f.receive(L({}), now({ busy: true })).length === 0, 'a busy box (Advanced RDS, SATELLITE, a whole-radio mode, VFO B) is not followed into');
  ok(f.receive(L({}), now({ canShow: (b) => b !== 'aprs' })).length === 0, 'a decoder this client cannot show here is not followed');
  ok(f.receive(L({ ext: 'paging', key: 'paging|x' }), now()).length === 0, 'PAGING is never followed');
  ok(f.receive(L({}), now()).length === 1, 'free again: followed');
  f.noteClosed('decoder', 'rtty');
  ok(f.closed('decoder') === '', 'closing ANOTHER box does not mark the room\'s run closed');
}
console.log('the spots run');
{
  const f = new SharedFollower();
  const S = { slot: 'spots', ext: 'spots', params: { scope: 'band' }, key: 'spots|band' } as Partial<SharedDecoder>;
  let a = f.receive(L(S), now());
  ok(a.length === 1 && a[0].kind === 'spots' && a[0].scope === 'band', 'the room\'s Band spots are followed with their scope');
  f.noteFollowed('spots', 'spots|band');
  f.noteClosed('spots', null);
  ok(f.receive(L(S), now()).length === 0, 'closed by me: stays closed');
  a = f.receive(L({ ...S, params: {}, key: 'spots|single' }), now({ }));
  ok(a.length === 1 && a[0].kind === 'spots' && a[0].scope === 'single', 'the room switches to Single (a new run): followed');
  ok(f.receive(L(S), now({ canSpots: () => false })).length === 0, 'spots this client cannot run here are not followed');
  a = f.receive(L({}, S), now());
  ok(a.some((x) => x.kind === 'follow') && a.some((x) => x.kind === 'spots'), 'decoder and spots both offered (the app takes the decoder first)');
}

console.log('the web page and the app wire it alike');
{
  const web = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
  const wspec = readFileSync(new URL('../web/client/src/spectrum.ts', import.meta.url), 'utf8');
  const wdec = readFileSync(new URL('../web/client/src/decoders.ts', import.meta.url), 'utf8');
  const app = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  const vsc = readFileSync(new URL('../src/services/VibeServerClient.ts', import.meta.url), 'utf8');
  const dc = readFileSync(new URL('../src/services/DecoderClient.ts', import.meta.url), 'utf8');
  ok(/case 'dec_shared'.*parseDecShared/.test(wspec) && /case 'dec_shared'.*parseDecShared/.test(vsc), 'both spectrum clients read dec_shared');
  ok(/onDecShared: \(l\) => roomDecIn\(l\)/.test(web) && /onDecShared: \(l\) =>.*roomInRef\.current\(l\)/.test(app), 'both hand it to the follower');
  ok(/decoders\.attach\(mode, \{ \.\.\.a\.entry\.params \} as DecoderParams, 'A', a\.entry\.ext\)/.test(web), 'web: joins with the server\'s extension and parameters, on VFO A');
  ok(/extension_name: this\.extOverride \|\| EXT\[mode\]/.test(wdec), 'web: the decoder socket sends the server\'s extension when following');
  ok(/startShared\(type, roomJoin\.ext, roomJoin\.params\)/.test(app) && /extension_name: this\.follow\.ext, params: this\.follow\.params/.test(dc), 'app: joins with the server\'s extension and parameters');
  ok(/const v = roomJoin \? 'A'/.test(app), 'app: the room\'s decoder runs on VFO A');
  ok(/function stopDecoder\(\) \{\s*\/\/[^\n]*\n\s*try \{ roomFollow\.noteClosed\('decoder', activeDec\)/.test(web), 'web: every close of the box is remembered');
  ok(/const closeDecoder = useCallback\(\(\) => \{\s*\/\/[^\n]*\n\s*roomFollowRef\.current\.noteClosed\('decoder', activeDecRef\.current\)/.test(app), 'app: every close of the box is remembered');
  ok(/roomFollow\.noteOwnOpen\('decoder'/.test(web) && /roomFollowRef\.current\.noteOwnOpen\('decoder'/.test(app), 'both: a press is the listener\'s own');
  ok((web.match(/roomFollow\.noteClosed\('spots'/g) || []).length >= 3 && /noteClosed\('spots', null\)/.test(app), 'both: closing the spots is remembered');
  ok(/this\.follow = null;\s*\n\s*this\.active = name;/.test(dc), 'app: an ordinary start drops the follow (its own settings again)');
}

console.log(`\ntest_shared_decoders: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
