// THE DIRECTORY PAGE'S LIVE OCCUPANCY READS GO TO EACH RECEIVER'S OWN TUNNEL, NOT THE WORKER (Stuart 2026-11-25).
//
//   node directory/scripts/test-live-direct.mjs
//
// ★★ UNDER TEST, against the page's OWN code (sliced out of public/index.html, not a copy): vibeserver.json and every
//    /r/<id>/vibeserver.json are read from the listing's `url` (its tunnel), never the slug, while that answers; a direct
//    read that fails is asked at the slug ONCE for that load and the direct address then backs off; a rotated tunnel
//    (a new host) is tried directly at once; the host allow-list refuses foreign hosts and the Worker's own names; the
//    "not responding" and radio-down rules still hold; and a hidden tab refreshes nothing, then refreshes once when seen.
import { readFileSync } from 'node:fs';

const html = readFileSync(new URL('../public/index.html', import.meta.url), 'utf8');
const slice = (from, to) => {
  const a = html.indexOf(from), b = html.indexOf(to, a);
  if (a < 0 || b < 0) throw new Error(`index.html: cannot find ${from} … ${to}`);
  return html.slice(a, b + to.length);
};
const ADDRESS = html.match(/^const ADDRESS_OK = .*$/m)[0];
const LIVE = slice('async function fetchStatus(host)', '/* ══ LIVE-READS END').replace(/\/\* ══ LIVE-READS END$/, '');
const COUNTS = slice('async function withLiveCounts(servers)', '\n  return servers;\n}\n');
const REFRESH = slice('/* REFRESH-BEGIN */', '/* REFRESH-END */');

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('   ok  ', m); } else { fail++; console.log('   FAIL', m); } };

// ── the page's live-read code, with a fetch that answers from a table ────────────────────────────────────────────────
let seen = [], answer = () => ({ status: 200, body: { listeners: 1, maxUsers: 5 } });
const fakeFetch = async (url) => {
  seen.push(url);
  const a = answer(new URL(url));
  if (a === 'throw') throw new TypeError('Failed to fetch');
  return { ok: a.status >= 200 && a.status < 300, status: a.status, json: async () => a.body };
};
const page = new Function('fetch', `${ADDRESS}\n${LIVE}\n${COUNTS}\nreturn { withLiveCounts, directHostOf, DIRECT_OK, ADDRESS_OK, directBackoff, liveRead };`)(fakeFetch);

const TUN = 'heel-dont-districts-excited.trycloudflare.com';
const server = (o = {}) => ({ id: 's1', name: 'Lenovo', url: `https://${TUN}`, address: 'stuey3d-lenovoi5.vibeserver.vibesdr.net',
  radios: [{ id: '59a1a7d0', listeners: 0, maxListeners: 10 }], ...o });
const hostsOf = (l) => l.map((u) => new URL(u).host);

console.log('\nthe allow-list');
ok(page.directHostOf(server()) === TUN, 'a quick tunnel (*.trycloudflare.com) is a direct host');
ok(page.directHostOf(server({ url: 'https://demo.vibesdr.net' })) === 'demo.vibesdr.net', 'a named tunnel on vibesdr.net is a direct host');
ok(page.directHostOf(server({ url: `https://${TUN}/` })) === TUN, 'a trailing slash is tolerated');
for (const bad of ['https://evil.example.com', 'https://almere.websdr.fm-tuner.nl', 'http://' + TUN, `https://${TUN}:8443`,
                   `https://${TUN}/x`, `https://${TUN}?a=1`, `https://user@${TUN}`, 'https://trycloudflare.com',
                   'https://a.b.trycloudflare.com', 'https://vibeserver.vibesdr.net', 'https://x.vibeserver.vibesdr.net',
                   'https://trycloudflare.com.evil.net', 'https://x.trycloudflare.com.evil.net', 'javascript:alert(1)', '', null])
  ok(page.directHostOf(server({ url: bad })) === null, `refused as a direct host: ${JSON.stringify(bad)}`);
ok(!page.DIRECT_OK.test('stuey3d-lenovoi5.vibeserver.vibesdr.net'), 'a slug (the Worker) is never a direct host');
let threw = false;
try { await page.liveRead(server({ url: 'https://evil.example.com', address: 'evil.example.com' }), '/vibeserver.json', true); } catch { threw = true; }
ok(threw && seen.length === 0, 'a listing naming foreign hosts for both addresses makes NO request');
threw = false; seen = [];
try { await page.liveRead(server(), '/../admin', false); } catch { threw = true; }
ok(threw && seen.length === 0, 'a path that is not a status path makes NO request');

console.log('\nthe direct route, answering');
seen = []; answer = () => ({ status: 200, body: { listeners: 2, maxUsers: 10 } });
let [s] = await page.withLiveCounts([server()]);
ok(seen.length === 2 && hostsOf(seen).every((h) => h === TUN), `both reads went to the tunnel (${hostsOf(seen).join(', ')})`);
ok(seen.some((u) => u.endsWith('/vibeserver.json')) && seen.some((u) => u.endsWith('/r/59a1a7d0/vibeserver.json')), 'the machine and the radio were each read');
ok(s.live && s.listeners === 2 && s.radios[0].listeners === 2 && !s.unreachable, 'the counts are the live answer');

const multi = server({ radios: [{ id: 'a1' }, { id: 'b2' }, { id: 'c3' }], url: 'https://demo.vibesdr.net', address: 'pi500.vibeserver.vibesdr.net' });
seen = [];
await page.withLiveCounts([multi]);
ok(seen.length === 4 && hostsOf(seen).every((h) => h === 'demo.vibesdr.net'), `a multi-radio server: 1 + 3 reads, all direct (${seen.length})`);

console.log('\nthe direct route failing (a rotated tunnel): the slug ONCE, then back off');
page.directBackoff.clear();
const gone = 'old-tunnel-name.trycloudflare.com';
answer = (u) => (u.host === gone ? 'throw' : { status: 200, body: { listeners: 3, maxUsers: 10 } });
seen = [];
[s] = await page.withLiveCounts([server({ url: `https://${gone}` })]);
const machine = seen.filter((u) => u.endsWith('.net/vibeserver.json') || u.endsWith('.com/vibeserver.json'));
ok(machine.length === 2 && new URL(machine[0]).host === gone && new URL(machine[1]).host === 'stuey3d-lenovoi5.vibeserver.vibesdr.net',
   `machine read: direct, then the slug once (${hostsOf(machine).join(' → ')})`);
ok(seen.length === 4, `four requests in all, no more (${seen.length})`);
ok(s.live && s.listeners === 3 && !s.unreachable, 'the slug answer is used: occupancy still shown');
seen = [];
await page.withLiveCounts([server({ url: `https://${gone}` })]);
ok(seen.length === 2 && hostsOf(seen).every((h) => h.endsWith('.vibeserver.vibesdr.net')), 'next load: the failed direct address is left alone (backed off), the slug answers');
const b = page.directBackoff.get(gone + '/vibeserver.json');
ok(b && b.fails === 1 && b.until - Date.now() > 4 * 60e3 && b.until - Date.now() <= 5 * 60e3, 'back-off: five minutes after one failure');
seen = [];
[s] = await page.withLiveCounts([server()]);
ok(seen.length === 2 && hostsOf(seen).every((h) => h === TUN), 'the listing names the new tunnel: read directly at once');

console.log('\nan owner\'s firewall (403 on a foreign Origin) and a dead tunnel (530): the slug');
for (const st of [403, 530]) {
  page.directBackoff.clear(); seen = [];
  answer = (u) => (u.host === TUN ? { status: st, body: {} } : { status: 200, body: { listeners: 4, maxUsers: 10 } });
  [s] = await page.withLiveCounts([server()]);
  ok(s.listeners === 4 && !s.unreachable && seen.filter((u) => u.includes('vibeserver.vibesdr.net')).length === 2, `${st}: each read went to the slug once, occupancy shown`);
}

console.log('\nthe rules that stay');
page.directBackoff.clear(); seen = [];
answer = () => 'throw';
[s] = await page.withLiveCounts([server()]);
ok(s.unreachable === true && !s.live, 'a server that answers neither route is NOT RESPONDING');
ok(seen.length === 2, `…after exactly two tries: direct + slug — the one retry (${seen.length})`);
page.directBackoff.clear(); seen = [];
answer = (u) => (u.pathname.startsWith('/r/') ? { status: 503, body: {} } : { status: 200, body: { listeners: 0, maxUsers: 10 } });
[s] = await page.withLiveCounts([server()]);
ok(s.radios[0].radioDown === true && seen.length === 2 && hostsOf(seen).every((h) => h === TUN), 'a 503 from the radio route is the receiver speaking: radio down, no slug read');
page.directBackoff.clear(); seen = [];
answer = () => ({ status: 200, body: {} });
[s] = await page.withLiveCounts([server({ radios: [{ id: 'q', listeners: 7 }] })]);
ok(s.listeners === undefined && s.radios[0].listeners === 7, 'occupancy is said or it is not shown: an answer with no figures changes none');
page.directBackoff.clear(); seen = [];
await page.withLiveCounts([server({ address: null })]);
ok(seen.length === 0, 'a server with no slug address is skipped, as before');
// ★ a server whose own address is not on the allow-list reads through the slug exactly as before (one retry kept)
seen = [];
answer = () => ({ status: 200, body: { listeners: 1, maxUsers: 2 } });
await page.withLiveCounts([server({ url: 'https://almere.websdr.fm-tuner.nl' })]);
ok(seen.length === 2 && hostsOf(seen).every((h) => h === 'stuey3d-lenovoi5.vibeserver.vibesdr.net'), 'an owner\'s own domain: read through the slug as before');

console.log('\nthe refresh loop: a hidden tab refreshes nothing');
{
  let tick = null, onVis = null, loads = 0, now = 1_000_000;
  const doc = { hidden: false, addEventListener: (ev, f) => { if (ev === 'visibilitychange') onVis = f; } };
  const D = { now: () => now };
  new Function('document', 'setInterval', 'load', 'Date', REFRESH)(doc, (f, ms) => { if (ms === 60000) tick = f; }, () => { loads++; }, D);
  ok(typeof tick === 'function' && typeof onVis === 'function', 'a minute timer and a visibility listener are set');
  now += 60000; tick(); ok(loads === 1, 'visible: the minute refreshes');
  doc.hidden = true;
  for (let i = 0; i < 10; i++) { now += 60000; tick(); }
  ok(loads === 1, 'hidden for ten minutes: no refresh at all');
  doc.hidden = false; onVis();
  ok(loads === 2, 'seen again: one refresh at once');
  onVis(); ok(loads === 2, 'a second visibility event inside the minute: no extra refresh');
  doc.hidden = true; onVis(); ok(loads === 2, 'becoming hidden: no refresh');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
