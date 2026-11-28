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
  add('noalias', { name: 'No alias', status: { communityMap: true, communityLayers: ['ais', 'dsc'], radios: [{ id: '8c02e830', driver: 'sdrplay' }, { id: '370b773e' }, { id: '370b773e' }, { id: '<b>' }, { name: 'no id' }] } });
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
  ok(na && JSON.stringify(na.radios) === '["8c02e830","370b773e"]' && s && !('radios' in s), `a MULTI-RADIO server lists its radio ids (deduped, checked); a single-radio one none (${na && na.radios})`);
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

  // ── the pills' totals: the BREAKDOWN of the status line's (Stuart 11-25) ──
  {
    const its = CM.merge([CM.itemsOf({ on: true, now, a: [['406A91', 51.5, -0.4, 3, 5000, 90, 200, 1 | 4, 'EZY47TW'], ['4CA000', 52, 0, 3, 35000, 92, 441, 1, 'BAW696'], ['A1B2C3', 54, -30, 200, 37000, null, null, 8, 'UAL917']],
      s: [[235000001, 50.79, -1.31, now - 25, 12, 181, 'cargo', 'A', 'X', '']] }, { id: 'a', name: 'A' })]);
    const cnt = CM.modeCounts(its, { maxAge: 86400, server: '', nav: {}, off: new Set() }, now);
    ok(cnt.by.adsb === 2 && cnt.by.vdl2 === 0 && cnt.by.hfdl === 1 && cnt.by.adsb + cnt.by.acars + cnt.by.vdl2 + cnt.by.hfdl === 3 && cnt.by.ais === 1,
       `★ an aircraft on ADS-B AND VDL2 counts ONCE, under the source it is drawn in — the pills add up to the 3 aircraft (${JSON.stringify(cnt.by)})`);
    const off = CM.modeCounts(its, { maxAge: 86400, server: '', nav: {}, off: new Set(['adsb']) }, now);
    const sh = CM.merge([CM.itemsOf({ on: true, now, s: [[235000001, 50.79, -1.31, now - 25, 12, 181, 'cargo', 'A', 'X', ''], [235000002, 50.7, -1.2, now - 25, 0, 0, 'cargo', 'A', 'Y', '']],
      d: [[now - 100, 50.8, -1.3, 3, 'DISTRESS · MMSI 235000001 (UK)', 'a.txt'], [now - 50, 50.8, -1.3, 0, 'Call · MMSI 235000001', 'b.txt'], [now - 60, 40, 1, 3, 'DISTRESS · MMSI 215402000 (Malta)', 'c.txt'], [now - 70, 41, 2, 1, 'safety, no MMSI', 'd.txt']] }, { id: 'a', name: 'A' })]);
    const sc = CM.shipsCombined(sh), shc = CM.modeCounts(sh, { maxAge: 86400, server: '', nav: {}, off: new Set() }, now);
    ok(sc.ships === 4 && sc.both === 1 && shc.by.ais === 2 && shc.by.dsc === 2 && shc.by.ais + shc.by.dsc === sc.ships,
       `★ a ship on AIS AND DSC is ONE ship: 2 AIS + 2 more by DSC = ${sc.ships} ships combined; the pills (AIS ${shc.by.ais}, DSC ${shc.by.dsc}) add up to it`);
    ok(CM.modeCounts(sh, { maxAge: 86400, server: '', nav: {}, off: new Set(['ais']) }, now).by.dsc === 3, '…AIS switched off: the DSC pill counts that ship again');
    ok(off.by.vdl2 === 1 && off.by.adsb === 1 && CM.primaryMode(its[0], new Set(['adsb'])) === 'vdl2', `…ADS-B switched off: it is drawn and counted as VDL2 (${JSON.stringify(off.by)})`);
  }
  // ── MULTI-RADIO servers (Stuart 11-25: "pi 500 should be sending adsb too") ──
  const one = { url: 'https://a.trycloudflare.com', layers: ['ais'] };
  ok(CM.targets(one).length === 1 && CM.targets(one)[0] === one && CM.targets({ ...one, radios: ['abc'] }).length === 1, 'a single-radio server: ONE target, itself');
  const multi = { url: 'https://demo.example.net', layers: ['dsc'], radios: ['8c02e830', '370b773e', 'bad/id', '370b72d8'] };
  const tg = CM.targets(multi);
  ok(tg.length === 3 && tg.map((t) => t.rid).join() === '8c02e830,370b773e,370b72d8' && CM.targets(multi)[1] === tg[1], 'a multi-radio server: a target per radio (bad ids dropped), kept across list reloads');
  ok(CM.dataUrl(multi, 'https:', ['adsb'], '370b773e') === 'https://demo.example.net/r/370b773e/vibeserver/community.json?s=adsb' && CM.dataUrl(multi, 'https:', ['adsb'], '../x') === 'https://demo.example.net/vibeserver/community.json?s=adsb'
     && /\/r\/370b773e\/vibeserver\/community\/navtex\?f=/.test(CM.textUrl(multi, 'https:', 'a.txt', '370b773e')), "each radio at /r/<id>/ on the server's own tunnel");
  ok(CM.askFor(tg[1], multi, ['adsb', 'dsc', 'navtex'], 0).join() === 'adsb,dsc,navtex', 'a radio not heard from yet: asked for every layer switched on');
  tg[1].layers = ['adsb', 'dsc', 'mesh']; CM.noteResult(tg[1], true, 0, ['adsb', 'dsc']);
  ok(CM.askFor(tg[1], multi, ['adsb', 'dsc', 'navtex'], 30000).length === 0 && CM.askFor(tg[1], multi, ['adsb', 'dsc', 'navtex'], 60000).join() === 'adsb,dsc', '…then only what it shares, under its own 60 s floor');
  ok(CM.askFor(one, one, ['adsb', 'ais'], 0).join() === 'ais', 'a single-radio server: its listing’s layers, as before');
  ok(CM.serverLayers(multi).sort().join() === 'adsb,dsc,mesh', 'what a multi-radio server shares: its front door’s and its radios’');
  const ra = CM.itemsOf({ on: true, now, a: [['4CA2C1', 51.7, -1.8, 2, 26000, 298, 437, 1, 'RYR6VG']] }, { id: 'pi', name: 'Pi', rid: '370b773e' });
  const rb = CM.itemsOf({ on: true, now, a: [['4CA2C1', 51.7, -1.8, 2, 26000, 298, 437, 1, 'RYR6VG']] }, { id: 'pi', name: 'Pi', rid: '370b72d8' });
  const mr = CM.mergeRadios([ra, rb]);
  ok(mr.length === 1 && mr[0].heard.length === 1 && mr[0].heard[0].rid, 'two radios answering the same aircraft: one item, the server heard once');
  ok(CM.merge([mr, CM.itemsOf({ on: true, now, a: [['4CA2C1', 51.7, -1.8, 2, 26000, 298, 437, 1, 'RYR6VG']] }, { id: 'other', name: 'O' })])[0].heard.length === 2, '…and with another server, "heard by 2"');

  // ── the SEA AREAS (Stuart 11-25: "can we draw all the shipping areas onto it like we do for navtex?") ──
  ok(CM.seaSeverity(null) === 0 && CM.seaSeverity({ gale: 8, fmax: 6 }) === 2 && CM.seaSeverity({ gale: 0, fmax: 7 }) === 1 && CM.seaSeverity({ gale: 0, fmax: 6, wind: { sev: 2, sev2: 0 } }) === 2
     && CM.seaSeverity({ gale: 0, fmax: 5, wind: { sev: 0, sev2: 1 } }) === 1 && CM.seaSeverity({ gale: 0, fmax: 6 }) === 0, 'sea-area tint: the server map’s rule (red at a gale, amber at 7)');
  const fcs = CM.seaForecasts([{ kind: 'navmet', d: { id: 'dogger', stale: false, fc: [{ gale: 9, fmax: 9 }] } }, { kind: 'navmet', d: { id: 'shannon', stale: true, fc: [{ gale: 9, fmax: 9 }] } },
    { kind: 'navmet', d: { id: 'rockall', stale: false, fc: [{ gale: 0, fmax: 7 }] } }, { kind: 'ship', d: {} }]);
  ok(fcs.get('dogger') === 2 && fcs.get('rockall') === 1 && fcs.get('shannon') === 0 && fcs.size === 3, '…an area with an arrow drawn loses its own name; a stale forecast tints nothing');
  const P = ['ais', 'navtex', 'adsb'];
  ok(CM.seaShown(new Set(['maritime']), P, undefined) && !CM.seaShown(new Set(['maritime']), P, false) && !CM.seaShown(new Set(['aviation']), P, true) && !CM.seaShown(new Set(['maritime']), ['adsb'], true),
     'SEA AREAS: on by default with MARITIME, off when switched off, MARITIME off, or no maritime row to hold the toggle');
  const SEAJ = JSON.parse(readFileSync(new URL('public/community-sea-areas.json', dir), 'utf8'));
  const seaSize = readFileSync(new URL('public/community-sea-areas.json', dir)).length;
  const polys = SEAJ.features.filter((f) => f.geometry.type === 'MultiPolygon'), labs = SEAJ.features.filter((f) => f.geometry.type === 'Point');
  ok(SEAJ.type === 'FeatureCollection' && polys.length > 800 && labs.length === polys.length, `the sea-areas file: GeoJSON, ${polys.length} areas, each with a label point`);
  ok(['viking', 'dogger', 'fisher', 'gbight', 'shannon', 'rockall', 'malin', 'irishsea', 'fastnet', 'ie-12-valentia', 'no-inner-skagerrak', 'de-hel'].every((id) => polys.some((f) => f.properties.id === id)),
     '…the UK shipping areas, Ireland’s, Norway’s and the German Bight’s are in it');
  ok(polys.every((f) => ['s', 'o', 'c'].includes(f.properties.k) && f.geometry.coordinates.every((pg) => pg[0].length >= 4 && pg[0].every(([x, y]) => Math.abs(x) <= 360 && Math.abs(y) <= 90)
     && pg[0][0][0] === pg[0][pg[0].length - 1][0] && pg[0][0][1] === pg[0][pg[0].length - 1][1])), '…every ring closed and on the globe (past ±180° where an area crosses the date line, as MapLibre draws it)');
  const dog = polys.find((f) => f.properties.id === 'dogger'), dogL = labs.find((f) => f.properties.id === 'dogger');
  ok(dogL.properties.n === 'DOGGER' && dogL.geometry.coordinates[0] > 0 && dogL.geometry.coordinates[0] < 4 && dogL.geometry.coordinates[1] > 54 && dogL.geometry.coordinates[1] < 56.5 && dog, 'Dogger: named in capitals, its label in the North Sea');
  ok(seaSize < 700000, `…small: ${seaSize} bytes (one static file, compressed on the wire)`);
  const srcs = Object.keys(SEAJ.attribution || {});
  ok(['uk', 'ie', 'no', 'de', 'us', 'ca'].every((s) => srcs.includes(s)) && /Met Office/.test(CM.seaAttribution(SEAJ)) && /Open Government Licence – Canada/.test(CM.seaAttribution(SEAJ))
     && /MET Norway/.test(CM.seaAttribution(SEAJ)) && (CM.seaAttribution(SEAJ).match(/Sea areas:/g) || []).length === 1, '…every source credited, as on the server map (one "Sea areas:" line)');
  ok(CM.SEA.url === '/community-sea-areas.json' && !/^https?:/.test(CM.SEA.url), 'served beside the page (no other request)');
}

// ── 3. an aircraft's ROUTE and TRACK (Stuart 11-26: "show the flight route in the cards … the tracked route when clicked") ──
console.log('== 3. an aircraft\'s route and track');
{
  const sandbox = {};
  vm.runInNewContext(readFileSync(new URL('public/community-map.js', dir), 'utf8'), sandbox);
  const CM = sandbox.CommunityMap;
  ok(CM.callsignOf('EZY82YZ ') === 'EZY82YZ' && CM.callsignOf('baw696') === 'BAW696' && CM.callsignOf('GEZAB') === '' && CM.callsignOf('N123AB') === '' && CM.callsignOf('') === '' && CM.callsignOf(7) === '',
     'a lookup only for an AIRLINE callsign (three letters, a number) — never a registration flown as one');
  // adsbdb's real answer shape (2026-11-27, trimmed)
  const real = { response: { flightroute: { callsign: 'EZY82YZ', airline: { name: 'easyJet' },
    origin: { iata_code: 'BRS', icao_code: 'EGGD', latitude: 51.3827, longitude: -2.71909, municipality: 'Bristol', name: 'Bristol Airport' },
    destination: { iata_code: 'KRK', icao_code: 'EPKK', latitude: 50.0777, longitude: 19.7848, municipality: 'Kraków', name: 'Kraków John Paul II International Airport' } } } };
  const r = CM.parseAdsbdb(real);
  ok(r && r.src === 'adsbdb' && r.from.iata === 'BRS' && r.to.code === 'EPKK' && r.airline === 'easyJet', 'adsbdb\'s answer read: origin, destination, airline');
  ok(CM.routeLine(r) === 'Bristol (BRS) → Kraków (KRK)', `…in the card's words: "${CM.routeLine(r)}"`);
  ok(CM.parseAdsbdb({ response: 'unknown callsign' }) === null && CM.parseAdsbdb(null) === null && CM.parseAdsbdb({ response: { flightroute: { origin: { name: '<b>' } } } }) === null,
     '"unknown callsign" (its 404) and anything odd → no route');
  // ★ the server's judge (vibe_adsb_data.h Db::judge): within max(60 NM, a quarter of the leg) of the leg
  ok(CM.routeJudge(r, 51.0, 5.0) === true, 'over Belgium: on Bristol → Kraków');
  ok(CM.routeJudge(r, 40.0, -3.7) === false, 'over Madrid: far from it — the card strikes it through');
  ok(CM.routeJudge(r, null, null) === null && CM.routeJudge({ from: { lat: null }, to: { lat: null } }, 51, 0) === null, 'no position, or ends with no place: not judged');
  ok(Math.abs(CM.gcNm(51.47, -0.45, 40.64, -73.78) - 2999) < 15, `great circle LHR–JFK ≈ 2 999 NM (${CM.gcNm(51.47, -0.45, 40.64, -73.78).toFixed(0)})`);
  // the server's own data (aircraft.json rows)
  const dl = CM.serverRoute({ key: 'A1B2C3', from: 'EGLL', to: 'KIAD', route: { from: 'EGKK', to: 'LEMD' } });
  ok(dl && dl.src === 'datalink' && dl.from.code === 'EGLL' && dl.to.code === 'KIAD', '★ the aircraft\'s OWN datalink from/to wins over a filed route');
  const fl = CM.serverRoute({ key: '4CA000', route: { from: 'EGLL', to: 'EDDF', fromIata: 'LHR', toIata: 'FRA', fromPlace: 'London', toPlace: 'Frankfurt', plausible: false } });
  ok(fl && fl.src === 'filed' && fl.plausible === false && CM.routeLine(fl) === 'London (LHR) → Frankfurt (FRA)', 'a filed route from the server\'s aircraft data, with ITS verdict');
  ok(CM.serverRoute({ key: 'X', from: '<script>', to: 'KIAD' }) === null && CM.serverRoute({}) === null && CM.serverRoute(null) === null, 'nothing usable → nothing');
  ok(CM.tableRow({ aircraft: [{ key: '4CA000' }, { key: 'A1B2C3', from: 'EGLL' }] }, 'a:A1B2C3').from === 'EGLL' && CM.tableRow(null, 'a:X') === null, 'the table row by the item\'s key');
  // where the trail is asked
  const servers = [{ id: 'one', name: 'One', url: 'https://one-x.trycloudflare.com' },
                   { id: 'pi', name: 'Pi', url: 'https://pi-x.trycloudflare.com', radios: ['r1', 'r2', 'r3'] },
                   { id: 'slug', name: 'Slug only', address: 'slug.vibeserver.vibesdr.net' }];
  const ac = (modes, heard, icao = '4CA000') => ({ kind: 'aircraft', key: 'a:' + icao, modes, d: { icao }, heard });
  const u1 = CM.trailUrls(ac(['adsb'], [{ sid: 'pi', rid: 'r2' }, { sid: 'one' }]), servers, 'https:');
  ok(JSON.stringify(u1) === JSON.stringify(['https://pi-x.trycloudflare.com/r/r2/adsb/trail/4ca000', 'https://pi-x.trycloudflare.com/r/r1/adsb/trail/4ca000',
     'https://pi-x.trycloudflare.com/r/r3/adsb/trail/4ca000', 'https://one-x.trycloudflare.com/adsb/trail/4ca000']), `the trail: the radio that answered first, then the server's others, then the next receiver — at most ${CM.TRAIL_FETCH_MAX} (${u1.length})`);
  ok(u1.every((u) => !/vibeserver\.vibesdr\.net/.test(u)), '★★★ never the directory\'s Worker (the slug) — the server\'s own tunnel');
  ok(CM.trailUrls(ac(['hfdl'], [{ sid: 'one' }]), servers, 'https:').length === 0 && CM.trailUrls(ac(['adsb'], [{ sid: 'slug' }]), servers, 'https:').length === 0
     && CM.trailUrls({ kind: 'aircraft', key: 'a:r:GABC', modes: ['adsb'], d: { icao: '' }, heard: [{ sid: 'one' }] }, servers, 'https:').length === 0,
     'no trail asked for a datalink-only aircraft, one without an ICAO address, or a server with no address of its own');
  ok(CM.aircraftTableUrl(servers[1], 'https:') === 'https://pi-x.trycloudflare.com/vibeserver/aircraft.json?since=1800' && CM.aircraftTableUrl(servers[2], 'https:') === '',
     'the route table: the server\'s own tunnel (its door answers for every radio), the last 30 min');
  const tr = CM.parseTrail({ type: 'adsb_trail', hex: '4ca000', pts: [[51, 0, 35000], [51.01, 0.01, null], ['x', 1], [0, 0, 0]] });
  ok(tr.length === 2 && tr[0][2] === null, 'a server trail read (bad points dropped)');
  // ★ the client's own fixes: bounded, aged out, the same fix not twice
  const T = 1800000000, tracks = new Map();
  const fix = (key, lat, lon, t) => ({ kind: 'aircraft', key, lat, lon, t });
  CM.trackNote(tracks, [fix('a:1', 50, 0, T), Object.assign(fix('s:9', 50, 0, T), { kind: 'ship' })], T);
  CM.trackNote(tracks, [fix('a:1', 50, 0, T)], T + 60);                 // the same datalink fix reread
  CM.trackNote(tracks, [fix('a:1', 50.1, 0.1, T + 60)], T + 60);
  ok(tracks.size === 1 && tracks.get('a:1').length === 2, 'aircraft only; a fix reread is not a new point');
  for (let i = 0; i < 700; i++) CM.trackNote(tracks, [fix('a:1', 50 + i * 0.01, 0, T + 120 + i)], T + 120 + i);
  ok(tracks.get('a:1').length === CM.TRACK_MAX, `at most ${CM.TRACK_MAX} points an aircraft (${tracks.get('a:1').length})`);
  CM.trackNote(tracks, [], T + 120 + 700 + CM.TRACK_KEEP_SEC + 1);
  ok(!tracks.has('a:1'), `…and nothing older than ${CM.TRACK_KEEP_SEC / 60} min`);
  const many = new Map();
  for (let i = 0; i < CM.TRACK_AIRCRAFT_MAX + 50; i++) CM.trackNote(many, [fix('a:' + i, 50, 0, T + i)], T + i);
  ok(many.size === CM.TRACK_AIRCRAFT_MAX && !many.has('a:0') && many.has('a:' + (CM.TRACK_AIRCRAFT_MAX + 49)), `at most ${CM.TRACK_AIRCRAFT_MAX} aircraft, the longest unheard dropped`);
  // ★ the gaps: a datalink track (times) and a server trail (no times — by distance at its speed)
  const dlTrack = [[50, -20, T], [50.5, -18, T + 240], [51, -10, T + 1500], [51.2, -9, T + 1700]];
  const L = CM.trackLines(dlTrack, '#c084fc', 480);
  ok(L.length === 3 && L.map((f) => f.properties.gap).join() === '0,1,0' && L.every((f) => f.properties.c === '#c084fc'), `fixes 4 min apart solid, a 21-min gap DASHED, in the mode's colour (${L.map((f) => f.properties.gap)})`);
  ok(L[1].geometry.coordinates.length === 2 && L[1].geometry.coordinates[0][0] === -18, '…the dash runs straight between the two fixes either side (lon, lat)');
  const sv = [[51, 0, null], [51, 0.005, null], [51, 0.01, null], [51, 1.5, null], [51, 1.505, null]];
  const S = CM.trackLines(sv, '#ffb000', 450);
  ok(S.map((f) => f.properties.gap).join() === '0,1,0', 'a server trail (no times): a 56 NM hole at 450 kt (> 5 min flown) dashed; its 0.3 NM steps solid');
  ok(CM.trackLines([[51, 0, null]], '#fff').length === 0 && CM.trackLines(null).length === 0, 'one point or none: nothing drawn');
  ok(CM.ROUTE.api.startsWith('https://api.adsbdb.com/') && !/vibesdr/.test(CM.ROUTE.api), '★★★ the route source is called direct — never the Worker');
}

// ── 4. Stuart 11-28: each mode's FURTHEST from its own fixes; aviation in NM; MeshCore's colour is the map's ──
console.log('== 4. furthest by fix source, units');
{
  const sandbox = {};
  vm.runInNewContext(readFileSync(new URL('public/community-map.js', dir), 'utf8'), sandbox);
  const CM = sandbox.CommunityMap;
  const now = 1_800_000_000;
  const sv = [{ id: 'uk', name: 'UK', lat: 52, lon: 0 }, { id: 'is', name: 'Iceland', lat: 64, lon: -22 }];
  ok(CM.fixSrcOf(1 | 8, 8) === 'hfdl' && CM.fixSrcOf(1 | 8, 1) === 'adsb' && CM.fixSrcOf(8 | 16, 16) === 'hfdl' && CM.fixSrcOf(1 | 4 | 8 | 16, 16) === '',
     'field 12 names the position’s source (ADS-C = its one datalink bearer, else nobody’s)');
  ok(CM.fixSrcOf(1 | 8, undefined) === '' && CM.fixSrcOf(1, undefined) === 'adsb' && CM.fixSrcOf(8, null) === 'hfdl',
     'an older server’s row (no field 12): the mask’s ONLY source, else nobody’s — a mixed mask is never ADS-B’s');
  // EIN1: heard on ADS-B 30 NM from the UK receiver; minutes later its only position is an HFDL fix far out over the Atlantic
  const early = CM.itemsOf({ on: true, now: now - 240, a: [['4CA123', 52.5, 0, 0, 35000, 270, 450, 1, 'EIN1', '', '', '', 1]] }, sv[0]);
  const late = CM.itemsOf({ on: true, now, a: [['4CA123', 50, -40, 10, 37000, 270, 470, 1 | 8, 'EIN1', '', '', '', 8]] }, sv[0]);
  const near = CM.itemsOf({ on: true, now, a: [['406A91', 51.8, 0, 2, 5000, 90, 200, 1, 'EZY47', '', '', '', 1]] }, sv[0]);
  let it = CM.merge([early, late, near]);
  const ein = it.find((x) => x.key === 'a:4CA123');
  ok(ein.lat === 50 && ein.modes.join() === 'adsb,hfdl' && ein.fix.adsb.length === 1 && ein.fix.hfdl.length === 1, 'one aircraft: drawn at its newest (HFDL) fix, its ADS-B and HFDL fixes kept apart');
  let far = CM.furthest(it, 'adsb', sv);
  ok(far && far.it.key === 'a:4CA123' && Math.round(far.km / 1.852) === 30 && far.lat === 52.5,
     `★★ ADS-B FURTHEST ignores the HFDL fix: EIN1 at its ADS-B fix, ${far && Math.round(far.km / 1.852)} NM (not ~1 650)`);
  const fh = CM.furthest(it, 'hfdl', sv);
  ok(fh && fh.it.key === 'a:4CA123' && fh.km > 2500, `…HFDL's FURTHEST is the HFDL fix (${fh && Math.round(fh.km)} km)`);
  // the same hex: one server ADS-B only (old server, no field 12), another HFDL far away (field 12) — ADS-B never takes it
  it = CM.merge([CM.itemsOf({ on: true, now, a: [['4CA999', 52.2, 0, 5, 30000, 0, 400, 1, 'RYR9']] }, sv[0]),
                 CM.itemsOf({ on: true, now, a: [['4CA999', 40, -50, 1, 30000, 0, 400, 1 | 8, 'RYR9', '', '', '', 8]] }, sv[1])]);
  far = CM.furthest(it, 'adsb', sv);
  ok(far && Math.round(far.km / 1.852) === 12 && far.s.id === 'uk', `…across servers too: ADS-B ${far && Math.round(far.km / 1.852)} NM from UK, never the HFDL fix Iceland heard`);
  ok(!CM.furthest(CM.merge([CM.itemsOf({ on: true, now, a: [['4CA777', 40, -50, 1, 30000, 0, 400, 1 | 8, 'X']] }, sv[0])]), 'adsb', sv),
     'an older server’s mixed row (ADS-B + HFDL, no field 12) is no mode’s fix');
  const mr = CM.mergeRadios([CM.itemsOf({ on: true, now: now - 60, a: [['4CA555', 52.3, 0, 0, 1, 0, 1, 1, 'A', '', '', '', 1]] }, sv[0]),
                              CM.itemsOf({ on: true, now, a: [['4CA555', 45, -30, 0, 1, 0, 1, 1 | 8, 'A', '', '', '', 8]] }, sv[0])]);
  ok(mr.length === 1 && mr[0].fix.adsb && mr[0].fix.hfdl, 'a multi-radio server’s copies keep each radio’s fix');
  ok(CM.distWords(1852 * 1.5, 'adsb') === '1,500 NM' && CM.distWords(100, 'hfdl') === '54 NM' && CM.distWords(185.2, 'ndb') === '100 NM' && CM.distWords(100, 'ais') === '100 km' && CM.distWords(12, 'mesh') === '12 km',
     '★★ aviation distances in NAUTICAL MILES (ADS-B, datalink, NDB, VOR); the rest in km');
  ok(CM.MODE.mesh.colour === '#ff9f43', 'MeshCore’s colour is the map marker’s own orange (ism.ts MAP_COLOURS.meshcore), not a green');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
