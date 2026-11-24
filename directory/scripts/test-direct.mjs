// THE WORKER ONLY ESTABLISHES THE CONNECTION (beta93, Stuart 2026-11-24) — what <slug>.vibeserver.vibesdr.net does to a
// page now that the page reads the receiver directly (web/client/src/origin.ts), against the real Worker and a real
// SQLite database behind test-lan.mjs's minimal D1 shim.
//
//   node directory/scripts/test-direct.mjs
//
// ★★ UNDER TEST: the subrequest names the receiver's own host (x-vibesdr-direct-host) beside x-vibesdr-via; the metas go
//    at the TOP of the head (after <meta charset>) so the page's head loader can read them; <meta name="vibe-direct-
//    assets"> is written ONLY when the receiver's own Content-Security-Policy names that host in script-src (so an older
//    server's page — whose policy does not — keeps every script on the Worker, unbroken); a page without the charset meta
//    still gets the metas before </head> as before; and every other path is STILL proxied (older servers' pages fetch
//    through it — nothing is refused yet).
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';

const dir = new URL('../', import.meta.url);
const db = new DatabaseSync(':memory:');
db.exec(readFileSync(new URL('schema.sql', dir), 'utf8'));
for (const f of readdirSync(new URL('migrations/', dir)).filter((f) => f.endsWith('.sql')).sort()) db.exec(readFileSync(new URL('migrations/' + f, dir), 'utf8'));
db.exec("ALTER TABLE servers ADD COLUMN verify_note TEXT NOT NULL DEFAULT ''");
const stmt = (sql, args = []) => ({
  bind: (...a) => stmt(sql, a),
  first: async () => db.prepare(sql).get(...args) ?? null,
  all: async () => ({ results: db.prepare(sql).all(...args) }),
  run: async () => { const r = db.prepare(sql).run(...args); return { meta: { changes: r.changes } }; },
});
const env = { DB: { prepare: (sql) => stmt(sql), batch: async (l) => Promise.all(l.map((s) => s.run())) }, ASSETS: { fetch: async () => new Response('asset') } };
const worker = (await import(new URL('src/index.js', dir))).default;

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('   ok  ', m); } else { fail++; console.log('   FAIL', m); } };
const T = Math.floor(Date.now() / 1000);
const TUN = 'lenovo-tun.trycloudflare.com';
db.prepare(`INSERT INTO servers (id,key_hash,name,url,kind,grid,lat,lon,country,status_json,created_at,updated_at,expires_at,slug,verified,until)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`)
  .run('srv1', createHash('sha256').update('k').digest('hex'), 'Lenovo', `https://${TUN}`, 'tunnel', 'IO92NH', 52.3, -0.9, 'GB', '{}',
       T - 86400, T - 60, T + 1800, 'lenovo', 1, 0);

// ★ The "tunnel": what reached it, and a page whose policy the case under test chooses.
let seen = [], csp = '', body = '';
globalThis.fetch = async (input, init = {}) => {
  const u = new URL(typeof input === 'string' ? input : input.url);
  const h = new Headers(init.headers || {});
  seen.push({ path: u.pathname, via: h.get('x-vibesdr-via'), direct: h.get('x-vibesdr-direct-host') });
  if (u.pathname === '/vibeserver.json') return new Response('{"name":"Lenovo"}', { headers: { 'content-type': 'application/json' } });
  if (u.pathname.startsWith('/vibeserver/')) return new Response('{"items":[]}', { headers: { 'content-type': 'application/json' } });
  return new Response(body, { headers: { 'content-type': 'text/html; charset=utf-8', ...(csp ? { 'content-security-policy': csp } : {}) } });
};
const get = async (path, accept = 'text/html') => worker.fetch(new Request(`https://lenovo.vibeserver.vibesdr.net${path}`,
  { headers: { accept, 'cf-connecting-ip': '81.2.69.160' } }), env);

const PAGE = '<meta charset="utf-8"><title>x</title><script>/*loader*/</script></head><body>receiver</body>';

console.log('\na beta93 server: its policy names the tunnel host');
csp = `default-src 'self'; script-src 'self' https://${TUN} 'unsafe-inline' 'wasm-unsafe-eval' blob:; style-src 'self' https://${TUN}`;
body = PAGE; seen = [];
let html = await (await get('/')).text();
ok(seen[0]?.via === 'directory' && seen[0]?.direct === TUN, `★ the subrequest names the receiver's own host (x-vibesdr-direct-host: ${seen[0]?.direct})`);
ok(html.startsWith(`<meta charset="utf-8"><meta name="vibe-direct-host" content="${TUN}"><meta name="vibe-direct-assets" content="${TUN}">`),
   '★★ the metas sit at the TOP of the head, vibe-direct-assets among them');
ok(html.indexOf('vibe-direct-host') < html.indexOf('/*loader*/'), 'before the page\'s own head loader');
ok(html.includes(`window.__VIBE_DIRECT_HOST__="${TUN}"`), 'the inline host is still there for older pages');

console.log('\nan older server: its policy does not');
csp = "default-src 'self'; script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval' blob:";
html = await (await get('/')).text();
ok(html.includes('vibe-direct-host') && !html.includes('vibe-direct-assets'), '★★★ no vibe-direct-assets: its scripts stay on the Worker, never blocked');
csp = `default-src 'self'; style-src 'self' https://${TUN}; script-src 'self'`;
html = await (await get('/')).text();
ok(!html.includes('vibe-direct-assets'), 'the host named in another directive only: still no');
csp = '';
html = await (await get('/')).text();
ok(!html.includes('vibe-direct-assets'), 'no policy at all: no');

console.log('\na page without the charset meta (an older build, someone else\'s)');
body = '<html><head><title>x</title></head><body>receiver</body></html>';
html = await (await get('/')).text();
ok(/<title>x<\/title><meta name="vibe-direct-host"[^>]*>.*<\/head>/.test(html), 'the metas go before </head>, as they always have');

console.log('\nevery other path is still proxied (older servers\' pages fetch through it)');
seen = [];
const g = await get('/vibeserver/gallery.json?limit=5', '*/*');
ok(g.status === 200 && (await g.json()).items && seen.some((s) => s.path === '/vibeserver/gallery.json'), '/vibeserver/gallery.json answered, not refused');
const s = await get('/vibeserver.json', 'application/json');
const sj = await s.json();
ok(s.status === 200 && sj.directHost === TUN && s.headers.get('access-control-allow-origin') === '*', 'vibeserver.json still carries directHost (the page re-learns a moved tunnel from it)');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
