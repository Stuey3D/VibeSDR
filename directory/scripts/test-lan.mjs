// SAME NETWORK → GO DIRECT (V12, Stuart 2026-10-30) — the LAN-address disclosure rule, against a REAL SQLite
// database built from schema.sql and every migration, behind the same minimal D1 shim test-gone.mjs uses.
//
//   node directory/scripts/test-lan.mjs
//
// ★★★ THE RULE UNDER TEST: a server's LAN address goes ONLY to a visitor whose CF-Connecting-IP is the server's own
//     public address (IPv4) or in its /64 (IPv6). A different address gets NO `lan` field and NO vibe-lan meta —
//     checked on /api/directory and on <slug>.vibeserver.vibesdr.net, for IPv4 and IPv6, and the stored row is
//     checked to hold no public address at all. Plus: the proof the client checks is the server's real HMAC
//     answer; an old server (no `lan`) and a Worker running before migration 0008 behave exactly as before.
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync } from 'node:fs';
import { createHash, createHmac } from 'node:crypto';

const dir = new URL('../', import.meta.url);

function makeDb(withLan) {
  const db = new DatabaseSync(':memory:');
  db.exec(readFileSync(new URL('schema.sql', dir), 'utf8'));
  for (const f of readdirSync(new URL('migrations/', dir)).filter((f) => f.endsWith('.sql')).sort()) {
    if (!withLan && f.startsWith('0008')) continue;
    db.exec(readFileSync(new URL('migrations/' + f, dir), 'utf8'));
  }
  db.exec("ALTER TABLE servers ADD COLUMN verify_note TEXT NOT NULL DEFAULT ''");   // production has it (see test-gone)
  return db;
}
function envFor(db) {
  const stmt = (sql, args = []) => ({
    bind: (...a) => stmt(sql, a),
    first: async () => db.prepare(sql).get(...args) ?? null,
    all: async () => ({ results: db.prepare(sql).all(...args) }),
    run: async () => { const r = db.prepare(sql).run(...args); return { meta: { changes: r.changes } }; },
  });
  return {
    DB: {
      prepare: (sql) => stmt(sql),
      batch: async (list) => { db.exec('BEGIN'); try { const o = []; for (const s of list) o.push(await s.run()); db.exec('COMMIT'); return o; } catch (e) { db.exec('ROLLBACK'); throw e; } },
    },
    ASSETS: { fetch: async () => new Response('asset', { status: 200 }) },
  };
}

const mod = await import(new URL('src/index.js', dir));
const worker = mod.default;
const { cleanLan, netKeyOf } = mod;

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('   ok  ', m); } else { fail++; console.log('   FAIL', m); } };

const T = Math.floor(Date.now() / 1000);
const KEY = 'k'.repeat(64);
const sha = (s) => createHash('sha256').update(s).digest('hex');
const hmac = (k, m) => createHmac('sha256', k).update(m).digest('hex');
const seed = (db, id, slug) => db.prepare(`INSERT INTO servers (id,key_hash,name,url,kind,grid,lat,lon,country,status_json,
    created_at,updated_at,expires_at,slug,verified,until) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`)
  .run(id, sha(KEY), 'Lenovo', `https://${slug}-tun.trycloudflare.com`, 'tunnel', 'IO92NH', 52.3, -0.9, 'GB',
       JSON.stringify({ radios: [{ id: 'r1', name: 'RSP1A', driver: 'sdrplay' }] }), T - 86400, T - 60, T + 1800, slug, 1, 0);

// ★ The tunnel answers the directory's own challenge as a real shim does (dirProofFor), and serves a page.
globalThis.fetch = async (input) => {
  const u = new URL(typeof input === 'string' ? input : input.url);
  const nonce = u.searchParams.get('dirNonce');
  if (u.pathname === '/vibeserver.json') {
    return new Response(JSON.stringify({ name: 'Lenovo', ...(nonce ? { dirProof: hmac(KEY, nonce) } : {}) }),
                        { headers: { 'content-type': 'application/json' } });
  }
  return new Response('<html><head><title>x</title></head><body>receiver</body></html>',
                      { headers: { 'content-type': 'text/html; charset=utf-8' } });
};

const HOME4 = '81.2.69.160', AWAY4 = '81.2.69.161';
const HOME6_SERVER = '2a02:c7c:1234:5600::13', HOME6_PHONE = '2a02:0c7c:1234:5600:a1b2:c3d4:e5f6:1', AWAY6 = '2a02:c7c:1234:5601::1';

const req = (url, ip, init = {}) => new Request(url, { ...init, headers: { ...(init.headers || {}), ...(ip ? { 'cf-connecting-ip': ip } : {}) } });
const ping = (env, ip, lan) => worker.fetch(req('https://vibeserver.vibesdr.net/api/directory/ping', ip, {
  method: 'POST', headers: { 'content-type': 'application/json' },
  body: JSON.stringify({ id: 'srv1', key: KEY, url: 'https://lenovo-tun.trycloudflare.com', ...(lan ? { lan } : {}) }),
}), env);
const list = async (env, ip) => {
  const r = await worker.fetch(req('https://vibeserver.vibesdr.net/api/directory?proto=9', ip), env);
  return { r, j: await r.json() };
};
const page = async (env, ip, path = '/') => (await worker.fetch(
  req(`https://lenovo.vibeserver.vibesdr.net${path}`, ip, { headers: { accept: 'text/html' } }), env)).text();

console.log('\ncleanLan — only private IPv4 with a port survives');
ok(JSON.stringify(cleanLan(['192.168.86.13:48001', '10.0.0.5:80', '172.20.1.1:8073'])) === '["192.168.86.13:48001","10.0.0.5:80","172.20.1.1:8073"]', 'RFC 1918 kept');
ok(cleanLan(['8.8.8.8:80', '127.0.0.1:48001', '100.64.1.1:80', '169.254.1.1:80', '172.32.0.1:80', 'evil.com:80',
             '192.168.1.1', '192.168.1.1:0', '192.168.1.1:70000', '192.168.300.1:80', '[fe80::1]:80', '<b>:1', 5]).length === 0,
   'public, loopback, CGNAT, link-local, hostnames, portless, bad ports and junk all dropped');
ok(cleanLan(['10.0.0.1:1', '10.0.0.1:1', '10.0.0.2:1', '10.0.0.3:1', '10.0.0.4:1', '10.0.0.5:1']).length === 4, 'deduped, at most four');
ok(cleanLan('192.168.1.1:80').length === 0 && cleanLan(null).length === 0, 'not an array → nothing');

console.log('\nnetKeyOf — IPv4 exact, IPv6 by /64');
ok(netKeyOf(HOME4) === HOME4 && netKeyOf(AWAY4) !== netKeyOf(HOME4), 'IPv4: the address itself; the next address is a different network');
ok(netKeyOf(HOME6_SERVER) === netKeyOf(HOME6_PHONE), 'IPv6: two devices in one /64 are one network (compressed and full forms)');
ok(netKeyOf(AWAY6) !== netKeyOf(HOME6_SERVER), 'IPv6: the neighbouring /64 is not');
ok(netKeyOf('::ffff:81.2.69.160') === HOME4, 'a v4-mapped literal is its IPv4 address');
ok(netKeyOf('') === '' && netKeyOf('nonsense') === '', 'unreadable → no network (never matches)');

for (const fam of ['IPv4', 'IPv6']) {
  const SERVER = fam === 'IPv4' ? HOME4 : HOME6_SERVER;
  const SAME   = fam === 'IPv4' ? HOME4 : HOME6_PHONE;
  const OTHER  = fam === 'IPv4' ? AWAY4 : AWAY6;
  console.log(`\n${fam}: a server pings with its LAN address`);
  const db = makeDb(true), env = envFor(db);
  seed(db, 'srv1', 'lenovo');
  const pr = await ping(env, SERVER, ['192.168.86.13:48001', '8.8.8.8:1']);
  ok(pr.status === 200 && (await pr.json()).verified === true, 'the ping is accepted and verified as ever');
  const row = db.prepare("SELECT * FROM servers WHERE id='srv1'").get();
  ok(row.lan_json === '["192.168.86.13:48001"]', 'only the private address is stored');
  ok(row.lan_proof === hmac(KEY, row.lan_nonce), 'the stored proof is HMAC(key, nonce) — what the real shim answers');
  ok(!JSON.stringify(row).includes(SERVER) && !JSON.stringify(row).includes(netKeyOf(SERVER)),
     '★★ the server\'s public address (and its /64) is NOT stored — only a hash');

  const same = await list(env, SAME);
  const s1 = same.j.servers.find((s) => s.id === 'srv1');
  ok(s1 && s1.lan && s1.lan.hosts[0] === '192.168.86.13:48001' && s1.lan.nonce === row.lan_nonce && s1.lan.proof === row.lan_proof,
     '★★★ SAME network → the listing carries lan {hosts, nonce, proof}');
  ok(/private/.test(same.r.headers.get('cache-control') || '') && /no-store/.test(same.r.headers.get('cache-control') || ''),
     'and that answer is private, no-store');

  const other = await list(env, OTHER);
  const s2 = other.j.servers.find((s) => s.id === 'srv1');
  ok(s2 && !('lan' in s2), '★★★ DIFFERENT network → NO lan field');
  ok(!JSON.stringify(other.j).includes('192.168.86.13'), '★★★ …and the LAN address appears nowhere in the response');
  ok(!(other.r.headers.get('cache-control') || '').includes('private'), 'an ordinary listing keeps its ordinary headers');
  const none = await list(env, '');
  ok(!JSON.stringify(none.j).includes('192.168.86.13'), 'no CF-Connecting-IP at all → nothing');

  const pSame = await page(env, SAME), pOther = await page(env, OTHER);
  ok(pSame.includes(`<meta name="vibe-lan" content="192.168.86.13:48001|${row.lan_nonce}|${row.lan_proof}">`),
     '★★★ shareable address, SAME network → the page carries the vibe-lan meta');
  ok(!pOther.includes('vibe-lan') && !pOther.includes('192.168.86.13'), '★★★ shareable address, DIFFERENT network → no meta, no address');
  ok((await page(env, SAME, '/r/r1/')).includes('vibe-lan'), 'a radio\'s own page (/r/<id>/) carries it too');

  console.log(`${fam}: the server stops sending it (an older build, or the owner moved it)`);
  await ping(env, SERVER, null);
  const row2 = db.prepare("SELECT * FROM servers WHERE id='srv1'").get();
  ok(row2.lan_json === '' && row2.lan_net === '' && row2.lan_proof === '', 'a ping without `lan` clears every LAN column');
  ok(!JSON.stringify((await list(env, SAME)).j).includes('192.168.86.13'), 'and even the same network gets nothing');
}

console.log('\nThe nonce is kept across pings, so an older listing still proves');
{
  const db = makeDb(true), env = envFor(db);
  seed(db, 'srv1', 'lenovo');
  await ping(env, HOME4, ['192.168.86.13:48001']);
  const a = db.prepare("SELECT lan_nonce, lan_proof FROM servers WHERE id='srv1'").get();
  await ping(env, HOME4, ['192.168.86.13:48001']);
  const b = db.prepare("SELECT lan_nonce, lan_proof FROM servers WHERE id='srv1'").get();
  ok(a.lan_nonce === b.lan_nonce && a.lan_proof === b.lan_proof, 'same nonce, same proof');
  await ping(env, AWAY4, ['192.168.86.13:48001']);
  ok(!JSON.stringify((await list(env, HOME4)).j).includes('192.168.86.13'),
     'the server moved network (pinged from elsewhere) → the old network no longer gets it');
  ok(JSON.stringify((await list(env, AWAY4)).j).includes('192.168.86.13'), '…and the new one does');
}

console.log('\nAn unverified or expired listing never hands out its LAN address');
{
  const db = makeDb(true), env = envFor(db);
  seed(db, 'srv1', 'lenovo');
  await ping(env, HOME4, ['192.168.86.13:48001']);
  db.prepare("UPDATE servers SET verified = 0 WHERE id='srv1'").run();
  ok(!JSON.stringify((await list(env, HOME4)).j).includes('192.168.86.13'), 'unverified → not listed, no address');
  db.prepare("UPDATE servers SET verified = 1, expires_at = ? WHERE id='srv1'").run(T - 10);
  ok(!JSON.stringify((await list(env, HOME4)).j).includes('192.168.86.13'), 'expired → no address');
}

console.log('\nA Worker deployed BEFORE migration 0008 — nothing breaks');
{
  const db = makeDb(false), env = envFor(db);
  seed(db, 'srv1', 'lenovo');
  const pr = await ping(env, HOME4, ['192.168.86.13:48001']);
  ok(pr.status === 200, 'the ping still succeeds');
  const l = await list(env, HOME4);
  ok(l.r.status === 200 && l.j.servers.length === 1 && !('lan' in l.j.servers[0]), 'the listing still works, without lan');
  ok((await page(env, HOME4)).includes('receiver'), 'the shareable address still serves the page');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
