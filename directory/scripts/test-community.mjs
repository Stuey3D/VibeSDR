// test-community.mjs — THE COMMUNITY MAP, directory side (Stuart, 2026-11-25).
//   1. The Worker: /api/community lists ONLY servers that say "communityMap":true, with a usable https address, live and
//      verified; the grid cut to four characters and its centre (never the listing's own lat/lon); the layers whitelisted;
//      no LAN, nothing per visitor; edge-cached. /api/directory carries communityMap + communityLayers through.
//   2. The page's logic (public/community-map.js, run as the browser runs it): the server formats read; the SAME aircraft,
//      ship and DSC call from two receivers merged into one with "heard by 2"; the filters (scope, modes, age, one
//      receiver); the modes offered = what somebody shares; the directory's LISTEN rule; data only from the server's own
//      https address; the polling floor and back-off.
//   node directory/scripts/test-community.mjs
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import vm from 'node:vm';
import { answers, rebase, contributors } from './community-fixtures.mjs';

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const dir = new URL('../', import.meta.url);

// ── 1. the Worker, against a real SQLite built from schema + migrations (test-gone.mjs's D1 shim) ──
console.log('== 1. the Worker: who contributes');
{
  const db = new DatabaseSync(':memory:');
  db.exec(readFileSync(new URL('schema.sql', dir), 'utf8'));
  for (const f of readdirSync(new URL('migrations/', dir)).filter((f) => f.endsWith('.sql')).sort()) db.exec(readFileSync(new URL('migrations/' + f, dir), 'utf8'));
  db.exec("ALTER TABLE servers ADD COLUMN verify_note TEXT NOT NULL DEFAULT ''");
  const stmt = (sql, args = []) => ({ bind: (...a) => stmt(sql, a), first: async () => db.prepare(sql).get(...args) ?? null,
    all: async () => ({ results: db.prepare(sql).all(...args) }), run: async () => ({ meta: { changes: db.prepare(sql).run(...args).changes } }) });
  const env = { DB: { prepare: (sql) => stmt(sql), batch: async (l) => { for (const s of l) await s.run(); } }, ASSETS: { fetch: async () => new Response('asset') } };
  let cachePuts = 0; const cacheStore = new Map();
  globalThis.caches = { default: { match: async (k) => { const r = cacheStore.get(k.url); return r ? r.clone() : undefined; }, put: async (k, r) => { cachePuts++; cacheStore.set(k.url, r.clone()); } } };
  const worker = (await import(new URL('src/index.js', dir))).default;
  const T = Math.floor(Date.now() / 1000);
  const add = (id, o) => db.prepare(`INSERT INTO servers (id,key_hash,name,url,kind,grid,lat,lon,country,status_json,created_at,updated_at,expires_at,slug,verified,until)
      VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`).run(id, createHash('sha256').update(id).digest('hex'), o.name || id, o.url ?? `https://${id}-x.trycloudflare.com`, 'tunnel',
      o.grid || 'IO92NH', 52.32, -1.38, 'GB', JSON.stringify(o.status), T - 86400, T - 60, o.expires ?? T + 1800, id, o.verified ?? 1, 0);
  add('sharer', { name: 'Sharer', status: { communityMap: true, communityLayers: ['adsb', 'xyz', 'ndb', '<b>'], listeners: 1, maxListeners: 4, radios: [] } });
  add('quiet', { name: 'Not sharing', status: { radios: [], listeners: 0 } });
  add('saidfalse', { name: 'Said false', status: { communityMap: false, communityLayers: ['adsb'] } });
  add('plainhttp', { name: 'Port-forwarded', url: 'http://203.0.113.9:8073', status: { communityMap: true, communityLayers: ['ais'] } });
  add('expired', { name: 'Gone', expires: T - 10, status: { communityMap: true, communityLayers: ['ais'] } });
  add('unverified', { name: 'Unproven', verified: 0, status: { communityMap: true, communityLayers: ['ais'] } });
  add('noalias', { name: 'No alias', status: { communityMap: true, communityLayers: ['ais', 'dsc'] } });
  try { db.prepare("UPDATE servers SET alias = 0 WHERE id = 'noalias'").run(); } catch { /* no alias column: migration absent */ }
  const req = (p) => new Request('https://vibeserver.vibesdr.net' + p, { headers: { 'cf-connecting-ip': '198.51.100.7' } });
  const r = await worker.fetch(req('/api/community'), env, {});
  const j = await r.json();
  const ids = j.servers.map((s) => s.id).sort();
  ok(JSON.stringify(ids) === JSON.stringify(['noalias', 'sharer']), `only live, verified, https, sharing servers (${ids})`);
  const s = j.servers.find((x) => x.id === 'sharer');
  ok(s && JSON.stringify(s.layers) === JSON.stringify(['adsb', 'ndb']), `layers whitelisted (${s && s.layers})`);
  ok(s && s.grid === 'IO92' && s.lat === 52.5 && s.lon === -1, `grid cut to four characters, its centre (${s && s.grid} ${s && s.lat},${s && s.lon})`);
  ok(s && s.address === 'sharer.vibeserver.vibesdr.net' && s.url === 'https://sharer-x.trycloudflare.com', 'the slug address for LISTEN, the tunnel for data');
  ok(s && !('lan' in s) && !('status_json' in s) && !('key_hash' in s), 'no LAN, no raw status, no key');
  const na = j.servers.find((x) => x.id === 'noalias');
  ok(!na || (na.address === null || na.address === 'noalias.vibeserver.vibesdr.net'), 'an owner without the vibesdr.net address gets none (where the column exists)');
  ok(/max-age=120/.test(r.headers.get('cache-control') || '') && r.headers.get('access-control-allow-origin') === '*', 'cacheable for two minutes, CORS open');
  await worker.fetch(req('/api/community'), env, {});
  ok(cachePuts === 1, 'the second ask is the edge cache (one D1 read)');
  const d = await (await worker.fetch(req('/api/directory'), env, {})).json();
  const ds = d.servers.find((x) => x.id === 'sharer'), dq = d.servers.find((x) => x.id === 'quiet'), df = d.servers.find((x) => x.id === 'saidfalse');
  ok(ds && ds.communityMap === true && JSON.stringify(ds.communityLayers) === JSON.stringify(['adsb', 'ndb']), '/api/directory carries communityMap + layers');
  ok(dq && !('communityMap' in dq) && df && !('communityMap' in df), '…and nothing for a server that does not share');
}

// ── 2. the page's logic ──
console.log('== 2. the page logic (public/community-map.js)');
{
  const sandbox = {};
  vm.runInNewContext(readFileSync(new URL('public/community-map.js', dir), 'utf8'), sandbox);
  const CM = sandbox.CommunityMap;
  const now = Math.floor(Date.now() / 1000);
  const A = answers();
  const list = contributors((id) => `https://${id}-x.trycloudflare.com`).servers;
  const per = list.map((s) => CM.itemsOf(rebase(A[s.id], now), s));
  ok(per[0].some((i) => i.kind === 'dsc') && per[0].some((i) => i.kind === 'navtex') && per[0].some((i) => i.kind === 'ndb'), 'the REAL server answer reads: DSC, NAVTEX, NDB');
  ok(per[1].filter((i) => i.kind === 'aircraft').length === 3, 'the REAL aircraft rows read (3)');
  ok(per[2].some((i) => i.kind === 'ship' && i.d.mmsi === '235000001') && !per[2].some((i) => i.d && i.d.mmsi === '0'), 'ships read');
  const items = CM.merge(per);
  const baw = items.find((i) => i.key === 'a:4CA000');
  ok(baw && baw.heard.length === 2 && baw.heard.map((h) => h.sid).sort().join() === 'pi500,xcover', `★ the same aircraft from two receivers = ONE marker, heard by 2 (${baw && baw.heard.length})`);
  const dsc = items.filter((i) => i.kind === 'dsc');
  ok(dsc.length === 1 && dsc[0].heard.length === 2, `★ the same DSC call from two receivers (40 s apart) = one, heard by 2 (${dsc.length})`);
  const hf = items.find((i) => i.key === 'a:A1B2C3');
  ok(hf && hf.modes.join() === 'hfdl', 'ADS-C rides its bearer: an HFDL aircraft is HFDL');
  const all = new Set(CM.MODES.map((m) => m.id));
  const n = (f) => CM.filter(items, { scope: 'both', modes: all, maxAge: 86400, ...f }, now).length;
  const total = n({});
  ok(total === items.length, `everything in 24 h (${total})`);
  const avi = CM.filter(items, { scope: 'aviation', modes: all, maxAge: 86400 }, now);
  ok(avi.length > 0 && avi.every((i) => i.kind === 'aircraft' || i.kind === 'ndb'), `AVIATION: aircraft and NDB only (${avi.length})`);
  const mar = CM.filter(items, { scope: 'maritime', modes: all, maxAge: 86400 }, now);
  ok(mar.length > 0 && mar.every((i) => ['ship', 'dsc', 'navtex'].includes(i.kind)), `MARITIME: ships, DSC, NAVTEX (${mar.length})`);
  ok(CM.filter(items, { scope: 'both', modes: new Set(['ais']), maxAge: 86400 }, now).every((i) => i.kind === 'ship'), 'one mode on: AIS only');
  ok(n({ maxAge: 300 }) < total && n({ maxAge: 300 }) > 0, `age 5 min drops the older items (${n({ maxAge: 300 })} of ${total})`);
  const onlyLenovo = CM.filter(items, { scope: 'both', modes: all, maxAge: 86400, server: 'lenovo' }, now);
  ok(onlyLenovo.length > 0 && onlyLenovo.every((i) => i.heard.some((h) => h.sid === 'lenovo')) && !onlyLenovo.some((i) => i.kind === 'aircraft'), 'ONLY THIS receiver: what it heard');
  const counts = CM.perServer(items);
  ok(counts.get('lenovo') === 3 && counts.get('pi500') === 4 && counts.get('xcover') === 4, `items per receiver (${[...counts].map((x) => x.join('=')).join(' ')})`);
  ok(JSON.stringify(CM.modesPresent(list, items)) === JSON.stringify(['adsb', 'hfdl', 'ndb', 'ais', 'dsc', 'navtex']), `modes offered = what is shared (${CM.modesPresent(list, items)})`);
  ok(JSON.stringify(CM.modesPresent([], [])) === '[]', 'nobody shares → no mode filters');
  // hostile input: nothing breaks, nothing unplaced, text stays text
  const evil = CM.itemsOf({ on: true, now, aircraft: { aircraft: [{ icao: '<img>', key: '"x', lat: 'NaN', lon: 1, src: ['ADS-B'] }, { icao: 'ABCDEF', lat: 91, lon: 0, src: ['ADS-B'] },
    { icao: '123456', flight: '<script>', lat: 10, lon: 10, src: ['ADS-B'] }] }, ships: { vessels: [{ mmsi: -1, lat: 1, lon: 1 }, null, 7] }, ndb: { log: 'x' } }, { id: 'e', name: 'E' });
  ok(evil.length === 1 && CM.esc(evil[0].title) === '&lt;script&gt;', 'hostile rows: unplaced dropped, text escaped when drawn');
  ok(CM.itemsOf({ on: false }, { id: 'x' }).length === 0, 'an "on":false answer draws nothing');
  // the directory's link and data rules
  ok(CM.listenUrl({ address: 'stuey.vibeserver.vibesdr.net', url: 'https://a.trycloudflare.com' }) === 'https://stuey.vibeserver.vibesdr.net/', 'LISTEN: the slug page, as the directory');
  ok(CM.listenUrl({ address: 'evil.example.com', url: 'https://a.trycloudflare.com' }) === 'https://a.trycloudflare.com/', 'LISTEN: a non-VibeServer address is never used');
  ok(CM.dataUrl({ url: 'https://a.trycloudflare.com' }, 'https:') === 'https://a.trycloudflare.com/vibeserver/community.json', 'DATA: the server’s own tunnel');
  ok(CM.dataUrl({ url: 'http://203.0.113.9:8073' }, 'https:') === '' && CM.dataUrl({ url: 'https://x.y/path?q' }, 'https:') === '', 'DATA: no plain http from an https page, no paths');
  ok(!/vibeserver\.vibesdr\.net/.test(CM.dataUrl({ url: 'https://a.trycloudflare.com', address: 'stuey.vibeserver.vibesdr.net' }, 'https:')), '★★★ DATA never goes through the slug (the Worker)');
  ok(CM.stateOf({ listeners: 4, maxListeners: 4 }).label === 'no free slot' && CM.stateOf({ unreachable: true }).label === 'not responding'
     && CM.stateOf({ listeners: 1, maxListeners: 4 }).label === 'free slot', "the directory's state words");
  // the floor and the back-off
  const sv = {};
  ok(CM.due(sv, 0), 'a new server is due');
  CM.noteResult(sv, true, 1000);
  ok(!CM.due(sv, 1000 + 59000) && CM.due(sv, 1000 + 60000), '★★★ answered: not again for 60 s');
  CM.noteResult(sv, false, 0); ok(sv._next === 300000 && sv.unreachable, 'failed: left alone 5 min');
  CM.noteResult(sv, false, 0); ok(sv._next === 600000, '…10 min');
  for (let i = 0; i < 5; i++) CM.noteResult(sv, false, 0);
  ok(sv._next === 1800000, '…never more than 30 min apart');
  ok(CM.REFRESH_SEC >= 60 && CM.LIST_SEC >= 600, 'floors: receivers ≥ 60 s, the list ≥ 10 min');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
