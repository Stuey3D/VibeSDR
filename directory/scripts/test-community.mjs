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
  add('sharer', { name: 'Sharer', status: { communityMap: true, communityLayers: ['adsb', 'xyz', 'ndb', '<b>', 'sondes', 'mesh'], listeners: 1, maxListeners: 4, radios: [] } });
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
  ok(s && JSON.stringify(s.layers) === JSON.stringify(['adsb', 'ndb', 'sondes', 'mesh']), `layers whitelisted (${s && s.layers})`);
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
  ok(ds && ds.communityMap === true && JSON.stringify(ds.communityLayers) === JSON.stringify(['adsb', 'ndb', 'sondes', 'mesh']), '/api/directory carries communityMap + layers');
  ok(dq && !('communityMap' in dq) && df && !('communityMap' in df), '…and nothing for a server that does not share');
}

// ── 2. the page's logic ──
console.log('== 2. the page logic (public/community-map.js + the server maps\' own kit, community-kit.js)');
{
  const sandbox = {};
  vm.runInNewContext(readFileSync(new URL('public/community-kit.js', dir), 'utf8') + ';globalThis.CommunityKit = CommunityKit;', sandbox);
  vm.runInNewContext(readFileSync(new URL('public/community-map.js', dir), 'utf8'), sandbox);
  const CM = sandbox.CommunityMap;
  const now = Math.floor(Date.now() / 1000);
  const A = answers();
  const list = contributors((id) => `https://${id}-x.trycloudflare.com`).servers;
  const per = list.map((s) => CM.itemsOf(rebase(A[s.id], now), s));
  const kinds = (l) => [...new Set(l.map((i) => i.kind))].sort().join();
  ok(kinds(per[0]) === 'aprs,dsc,mesh,navmet,navmsg,navst,navwarn,ndb,sonde', `the REAL stored answer reads every section (${kinds(per[0])})`);
  ok(per[0].filter((i) => i.kind === 'navwarn').length >= 10 && per[0].some((i) => i.kind === 'navwarn' && i.d.shape === 'area'), 'NAVTEX warnings, with an AREA among them, through the NAVTEX map\'s own sanitiser');
  ok(per[0].filter((i) => i.kind === 'navmsg').length >= 30 && per[0].every((i) => i.kind !== 'navmsg' || (i.lat === null && i.title.length <= 80)), 'NAVTEX messages: titles, no position');
  ok(per[1].filter((i) => i.kind === 'aircraft').length === 3, 'the REAL aircraft rows read (3)');
  ok(per[2].some((i) => i.kind === 'ship' && i.d.mmsi === '235000001' && i.d.family === 'passenger'), 'ships read');
  const items = CM.merge(per);
  const baw = items.find((i) => i.key === 'a:4CA000');
  ok(baw && baw.heard.length === 2 && baw.lat === 52.02, `★ the same aircraft from two receivers = ONE item, heard by 2, the newest position (${baw && baw.heard.length})`);
  ok(items.filter((i) => i.kind === 'dsc').length === 1 && items.find((i) => i.kind === 'dsc').heard.length === 2, '★ the same DSC call (40 s apart) = one, heard by 2');
  const w0 = items.find((i) => i.kind === 'navwarn' && i.heard.length === 2);
  ok(!!w0, `★ the same NAVTEX warning (station + id) from two receivers = one, heard by 2 (${w0 && w0.key})`);
  const nx0 = A.lenovo.nx[0], m0 = items.find((i) => i.kind === 'navmsg' && i.d.id === nx0[0] && i.station === nx0[6] && i.heard.length === 2);
  ok(m0 && m0.d.lost === nx0[5] && CM.textSource(m0).sid === 'lenovo' && m0.d.n === nx0[4] + 1, `★ the same message from two receivers = one; the text from the CLEANEST copy (${m0 && CM.textSource(m0).sid}, ${m0 && m0.d.lost} lost)`);
  ok(items.find((i) => i.key === 'p:G4ABC-9').heard.length === 2, '★ the same APRS station from two receivers = one');
  const hf = items.find((i) => i.key === 'a:A1B2C3');
  ok(hf && hf.modes.join() === 'hfdl' && hf.d.adsc, 'ADS-C rides its bearer: an HFDL aircraft is HFDL');
  const allModes = new Set(CM.MODES.map((m) => m.id)), allGroups = new Set(CM.GROUPS.map((g) => g.id));
  const F = (f) => Object.assign({ groups: allGroups, modes: allModes, maxAge: 86400 }, f);
  const n = (f) => CM.filter(items, F(f), now).length;
  const total = n({});
  ok(total > 50, `all three groups on at once: ${total} items`);
  const kindsOf = (f) => kinds(CM.filter(items, F(f), now));
  ok(kindsOf({ groups: new Set(['aviation']) }) === 'aircraft,ndb', `AVIATION alone: ${kindsOf({ groups: new Set(['aviation']) })}`);
  ok(kindsOf({ groups: new Set(['maritime']) }) === 'dsc,navmet,navmsg,navst,navwarn,ship', `MARITIME alone: ${kindsOf({ groups: new Set(['maritime']) })}`);
  ok(kindsOf({ groups: new Set(['misc']) }) === 'aprs,mesh,sonde', `MISC alone: ${kindsOf({ groups: new Set(['misc']) })}`);
  ok(kindsOf({ groups: new Set(['aviation', 'misc']) }) === 'aircraft,aprs,mesh,ndb,sonde', 'any combination of groups');
  ok(CM.filter(items, F({ modes: new Set(['ais']) }), now).every((i) => i.kind === 'ship'), 'one chip on: AIS only');
  ok(n({ maxAge: 900 }) < total && n({ maxAge: 900 }) > 0, `age 15 min drops the older items (${n({ maxAge: 900 })} of ${total})`);
  // NAVTEX's own filters
  const nav = (o) => CM.filter(items, F({ modes: new Set(['navtex']), nav: Object.assign({ layers: new Set(['stations', 'warnings', 'weather', 'messages']) }, o) }), now);
  ok(nav({ layers: new Set(['warnings']) }).every((i) => i.kind === 'navwarn'), 'NAVTEX layer WARNINGS alone');
  const facets = CM.navtexFacets(items);
  ok(facets.types.includes('A') && facets.types.includes('E') && facets.stations.includes('Niton'), `NAVTEX facets: types ${facets.types.join('')}, ${facets.stations.length} stations`);
  ok(nav({ types: new Set(['E']) }).filter((i) => i.b2).every((i) => i.b2 === 'E') && nav({ types: new Set(['E']) }).some((i) => i.kind === 'navmsg'), 'TYPE E (forecasts) alone');
  ok(nav({ station: 'Niton' }).every((i) => (i.kind === 'navst' ? i.d.name : i.station) === 'Niton') && nav({ station: 'Niton' }).length > 3, 'STATION Niton alone');
  ok(!nav({}).some((i) => i.kind === 'navwarn' && (i.d.expired || i.d.cancelled || i.d.superseded)), 'a cancelled / replaced / expired warning is never drawn (the NAVTEX map\'s rule)');
  const onlyX = CM.filter(items, F({ server: 'xcover' }), now);
  ok(onlyX.length > 0 && onlyX.every((i) => i.heard.some((h) => h.sid === 'xcover')), 'ONLY THIS receiver: what it heard');
  ok(JSON.stringify(CM.modesPresent(list, items)) === JSON.stringify(['adsb', 'hfdl', 'ndb', 'ais', 'dsc', 'navtex', 'sondes', 'aprs', 'mesh']), `modes offered = what is shared (${CM.modesPresent(list, items)})`);
  ok(CM.MODES.every((m) => /shared for the last/.test(CM.modeTip(m))), 'every chip says its window in its tooltip');
  // hostile input
  const evil = CM.itemsOf({ on: true, now, a: [['<img>', 'NaN', 1, 0, null, null, null, 1], ['ABCDEF', 91, 0, 0, null, null, null, 1], ['123456', 10, 10, 0, null, null, null, 1, '<script>']], s: [[-1, 1, 1, now]], p: 'x', nx: [[1, 2]] }, { id: 'e', name: 'E' });
  ok(evil.length === 1 && CM.esc(evil[0].title) === '&lt;script&gt;', 'hostile rows: unplaced dropped, text escaped when drawn');
  ok(CM.itemsOf({ on: false }, { id: 'x' }).length === 0, 'an "on":false answer draws nothing');
  // the directory's link and data rules
  ok(CM.listenUrl({ address: 'stuey.vibeserver.vibesdr.net', url: 'https://a.trycloudflare.com' }) === 'https://stuey.vibeserver.vibesdr.net/', 'LISTEN: the slug page, as the directory');
  ok(CM.listenUrl({ address: 'evil.example.com', url: 'https://a.trycloudflare.com' }) === 'https://a.trycloudflare.com/', 'LISTEN: a non-VibeServer address is never used');
  ok(CM.dataUrl({ url: 'https://a.trycloudflare.com' }, 'https:', ['adsb', 'navtex', 'bogus']) === 'https://a.trycloudflare.com/vibeserver/community.json?s=adsb,navtex', 'DATA: the server’s own tunnel, only the layers switched on');
  ok(CM.dataUrl({ url: 'http://203.0.113.9:8073' }, 'https:') === '' && CM.dataUrl({ url: 'https://x.y/path?q' }, 'https:') === '', 'DATA: no plain http from an https page, no paths');
  ok(!/vibeserver\.vibesdr\.net/.test(CM.dataUrl({ url: 'https://a.trycloudflare.com', address: 'stuey.vibeserver.vibesdr.net' }, 'https:')), '★★★ DATA never goes through the slug (the Worker)');
  ok(CM.textUrl({ url: 'https://a.trycloudflare.com' }, 'https:', '../x') === '' && /navtex\?f=20261101/.test(CM.textUrl({ url: 'https://a.trycloudflare.com' }, 'https:', '20261101-1.txt')), 'a message’s text: a gallery name only');
  ok(CM.stateOf({ listeners: 4, maxListeners: 4 }).label === 'no free slot' && CM.stateOf({ unreachable: true }).label === 'not responding', "the directory's state words");
  // the floor: fast layers every minute, slow every 5 min, only what is on; back-off
  const sv = {};
  const on = ['adsb', 'ais', 'navtex', 'ndb'];
  ok(CM.dueLayers(sv, on, 0).join() === 'adsb,ais,navtex,ndb', 'a new receiver: everything switched on');
  CM.noteResult(sv, true, 1000, on);
  ok(CM.dueLayers(sv, on, 1000 + 59000).length === 0 && CM.dueLayers(sv, on, 1000 + 60000).join() === 'adsb,ais', '★★★ after 60 s only the live layers');
  ok(CM.dueLayers(sv, on, 1000 + 300000).join() === 'adsb,ais,navtex,ndb', '…the slow ones every 5 min');
  ok(CM.dueLayers(sv, ['adsb'], 1000 + 60000).join() === 'adsb', 'a layer switched off is not asked for');
  CM.noteResult(sv, false, 0, on); ok(sv._retryAt === 300000 && sv.unreachable && CM.dueLayers(sv, on, 299000).length === 0, 'failed: left alone 5 min');
  for (let i = 0; i < 6; i++) CM.noteResult(sv, false, 0, on);
  ok(sv._retryAt === 1800000, '…never more than 30 min apart');
  ok(CM.FAST_SEC >= 30 && CM.SLOW_SEC >= 300 && CM.LIST_SEC >= 600 && CM.CONCURRENT <= 6, 'floors: live ≥ 30 s, slow ≥ 5 min, the list ≥ 10 min, ≤ 6 at once');
  ok(CM.fade({ modes: ['navtex'], t: now }, now) === 1 && CM.fade({ modes: ['navtex'], t: now - 86000 }, now) < 0.4, 'old things fade, as the servers’ maps do');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
