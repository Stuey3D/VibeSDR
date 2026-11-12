// ★★ THE vibesdr.net ADDRESS IS OPTIONAL FOR A DIRECT SERVER (2026-11-12, migrations/0009-alias.sql) — against a REAL
// SQLite database built from schema.sql and every migration, behind the same minimal D1 shim test-lan.mjs uses.
//
//   node directory/scripts/test-alias.mjs
//
// The rule under test:
//   • a direct server that registers or pings with alias:false gets NO address: list() carries no slug/address (so
//     every client links to its url), the slug host answers 404 (not a redirect), and the slug stays held on the row;
//   • alias:true (or switching back) serves the slug again, under the SAME name;
//   • a TUNNEL server can never opt out — the alias is how it is reached;
//   • a server that never sends `alias` (every one older than this) keeps today's behaviour;
//   • a Worker running before migration 0009 serves every alias exactly as before.
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync } from 'node:fs';
import { createHmac } from 'node:crypto';

const dir = new URL('../', import.meta.url);

function makeDb(withAlias) {
  const db = new DatabaseSync(':memory:');
  db.exec(readFileSync(new URL('schema.sql', dir), 'utf8'));
  for (const f of readdirSync(new URL('migrations/', dir)).filter((f) => f.endsWith('.sql')).sort()) {
    if (!withAlias && f.startsWith('0009')) continue;
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

const worker = (await import(new URL('src/index.js', dir))).default;

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('   ok  ', m); } else { fail++; console.log('   FAIL', m); } };

// ★ Every server answers the directory's challenge as a real shim does, with whatever key it was issued.
let KEY = '';
const hmac = (k, m) => createHmac('sha256', k).update(m).digest('hex');
globalThis.fetch = async (input) => {
  const u = new URL(typeof input === 'string' ? input : input.url);
  const nonce = u.searchParams.get('dirNonce');
  if (u.pathname === '/vibeserver.json') {
    return new Response(JSON.stringify({ name: 'Loft', ...(nonce ? { dirProof: hmac(KEY, nonce) } : {}) }),
                        { headers: { 'content-type': 'application/json' } });
  }
  return new Response('<html><head><title>x</title></head><body>receiver</body></html>',
                      { headers: { 'content-type': 'text/html; charset=utf-8' } });
};

let ipN = 1;   // ★ a fresh source address per registration, so the per-address rate limit never decides a case
const post = (env, path, body) => worker.fetch(new Request(`https://vibeserver.vibesdr.net${path}`, {
  method: 'POST', headers: { 'content-type': 'application/json', 'cf-connecting-ip': `81.2.69.${ipN++}` },
  body: JSON.stringify(body),
}), env);
const register = async (env, extra) => {
  const r = await post(env, '/api/directory/register', { name: 'Loft RSP', grid: 'IO92NH', locator: 'IO92NH', status: {}, ...extra });
  const j = await r.json();
  KEY = j.key || KEY;
  return j;
};
const ping = async (env, reg, extra = {}) => (await post(env, '/api/directory/ping', { id: reg.id, key: reg.key, ...extra })).json();
const list = async (env) => (await (await worker.fetch(new Request('https://vibeserver.vibesdr.net/api/directory?proto=9'), env)).json()).servers;
const slugGet = (env, slug, accept = 'text/html') => worker.fetch(new Request(`https://${slug}.vibeserver.vibesdr.net/`, { headers: { accept } }), env);

const OWN = 'https://sdr.example.org';

console.log('\nA direct server that opts out at registration');
{
  const db = makeDb(true), env = envFor(db);
  const reg = await register(env, { url: OWN, kind: 'direct', alias: false });
  ok(reg.key && reg.slug === 'loft-rsp', 'registered, and the name is still allotted');
  ok(reg.address === null && reg.alias === false, 'no vibesdr.net address in the answer');
  const p = await ping(env, reg, { url: OWN, kind: 'direct', alias: false });
  ok(p.verified === true && p.address === null && p.alias === false, 'the ping verifies and still gives no address');
  const s = (await list(env)).find((x) => x.id === reg.id);
  ok(s && s.url === OWN && s.address === null && s.slug === null, '★★ list(): no slug, no address — the link is the owner\'s url');
  const r = await slugGet(env, 'loft-rsp');
  ok(r.status === 404 && !r.headers.get('location'), '★★ the slug host answers 404, not a redirect');
  const t = await r.text();
  ok(t.includes('does not use a vibesdr.net address') && !t.includes(OWN), '…saying why, and not doubling as a link to the server');
  ok(db.prepare('SELECT slug, alias FROM servers WHERE id = ?').get(reg.id).slug === 'loft-rsp', 'the slug is still held on the row');

  console.log('…and switches the address back on');
  const p2 = await ping(env, reg, { url: OWN, kind: 'direct', alias: true });
  ok(p2.address === 'loft-rsp.vibeserver.vibesdr.net' && p2.alias === true, 'the same name comes back');
  ok((await list(env)).find((x) => x.id === reg.id).address === 'loft-rsp.vibeserver.vibesdr.net', 'list() publishes it again');
  ok((await (await slugGet(env, 'loft-rsp')).text()).includes('receiver'), 'and the slug host serves the page again');

  console.log('…and off again by ping alone');
  await ping(env, reg, { url: OWN, kind: 'direct', alias: false });
  ok((await slugGet(env, 'loft-rsp', '*/*')).status === 404, 'off again: 404');
  console.log('…a ping that says nothing keeps the owner\'s choice');
  await ping(env, reg, { url: OWN });
  ok((await list(env)).find((x) => x.id === reg.id).address === null, 'still no address');
}

console.log('\nA tunnel can never opt out');
{
  const db = makeDb(true), env = envFor(db);
  const TUN = 'https://abc-def.trycloudflare.com';
  const reg = await register(env, { url: TUN, kind: 'tunnel', alias: false });
  ok(reg.address === 'loft-rsp.vibeserver.vibesdr.net', 'registration: the address is given regardless');
  const p = await ping(env, reg, { url: TUN, kind: 'tunnel', alias: false });
  ok(p.address === 'loft-rsp.vibeserver.vibesdr.net', 'ping: still given');
  ok((await (await slugGet(env, 'loft-rsp')).text()).includes('receiver'), 'and served');

  console.log('…but once the owner switches it to their own address, it can');
  const p2 = await ping(env, reg, { url: OWN, kind: 'direct', alias: false });
  ok(p2.address === null && db.prepare('SELECT kind FROM servers WHERE id = ?').get(reg.id).kind === 'direct',
     'the ping moved it to direct and the opt-out took');
}

console.log('\nAn older server (never sends `alias` or a kind on its ping) — exactly as before');
{
  const db = makeDb(true), env = envFor(db);
  const reg = await register(env, { url: OWN, kind: 'direct' });
  ok(reg.address === 'loft-rsp.vibeserver.vibesdr.net', 'registration gives the address');
  const p = await ping(env, reg, { url: OWN });
  ok(p.address === 'loft-rsp.vibeserver.vibesdr.net' && p.verified === true, 'the ping keeps it');
  ok((await list(env)).find((x) => x.id === reg.id).address === 'loft-rsp.vibeserver.vibesdr.net', 'list() publishes it');
  ok((await (await slugGet(env, 'loft-rsp')).text()).includes('receiver'), 'the slug host serves it');
}

console.log('\nA Worker deployed BEFORE migration 0009 — nothing breaks, every alias as before');
{
  const db = makeDb(false), env = envFor(db);
  const reg = await register(env, { url: OWN, kind: 'direct', alias: false });
  ok(reg.key && reg.address === 'loft-rsp.vibeserver.vibesdr.net', 'registration succeeds (aliased, the opt-out cannot be stored yet)');
  const p = await ping(env, reg, { url: OWN, kind: 'direct', alias: false });
  ok(p.verified === true && p.address === 'loft-rsp.vibeserver.vibesdr.net', 'the ping succeeds');
  const l = await list(env);
  ok(l.length === 1 && l[0].address === 'loft-rsp.vibeserver.vibesdr.net', 'the listing works');
  ok((await (await slugGet(env, 'loft-rsp')).text()).includes('receiver'), 'the slug host serves it');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
