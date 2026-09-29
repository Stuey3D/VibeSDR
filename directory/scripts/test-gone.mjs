// Test the dead-address pages (serveBySlug) against a REAL SQLite database built from schema.sql and
// every migration, behind a minimal D1 shim — the SQL the Worker sends is executed, not mocked.
//
//   node directory/scripts/test-gone.mjs [outDir]
//
// ★ outDir (optional) receives the rendered pages as .html, for screenshots.
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync, writeFileSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';

const dir = new URL('../', import.meta.url);
const outDir = process.argv[2] || '';
if (outDir) mkdirSync(outDir, { recursive: true });

// ── The database: schema + migrations in order, + the column production gained by hand ──────────
const db = new DatabaseSync(':memory:');
db.exec(readFileSync(new URL('schema.sql', dir), 'utf8'));
for (const f of readdirSync(new URL('migrations/', dir)).filter((f) => f.endsWith('.sql')).sort()) {
  db.exec(readFileSync(new URL('migrations/' + f, dir), 'utf8'));
}
// ★ verify_note exists in production (ping() writes it) but in no migration file — added by hand.
db.exec("ALTER TABLE servers ADD COLUMN verify_note TEXT NOT NULL DEFAULT ''");

// ── A minimal D1: prepare().bind().first()/all()/run(), and batch() ────────────────────────────
function stmt(sql, args = []) {
  return {
    bind: (...a) => stmt(sql, a),
    first: async () => db.prepare(sql).get(...args) ?? null,
    all: async () => ({ results: db.prepare(sql).all(...args) }),
    run: async () => { const r = db.prepare(sql).run(...args); return { meta: { changes: r.changes } }; },
  };
}
const env = {
  DB: {
    prepare: (sql) => stmt(sql),
    batch: async (list) => {
      db.exec('BEGIN');
      try { const out = []; for (const s of list) out.push(await s.run()); db.exec('COMMIT'); return out; }
      catch (e) { db.exec('ROLLBACK'); throw e; }
    },
  },
  ASSETS: { fetch: async () => new Response('asset', { status: 200 }) },
};

const worker = (await import(new URL('src/index.js', dir))).default;

// ── Seed ───────────────────────────────────────────────────────────────────────────────────────
const T = Math.floor(Date.now() / 1000);
const DAY = 86400;
const sha = (s) => createHash('sha256').update(s).digest('hex');
const V4 = { name: 'RTLSDRBlog Blog V4', driver: 'rtlsdr', coverage: [[500000, 1766000000]] };
const add = (o) => {
  const r = {
    id: o.slug, key_hash: sha('key-' + o.slug), name: o.name, url: o.url || `https://${o.slug}.trycloudflare.com`,
    kind: 'tunnel', grid: o.grid, country: o.country || 'GB',
    status_json: JSON.stringify(o.status || { radios: [V4] }),
    created_at: o.created ?? T - 30 * DAY, updated_at: o.updated ?? T - 60,
    expires_at: o.expires ?? T + 1800, slug: o.slug, verified: o.verified ?? 1, until: 0,
  };
  const g = o.grid.toUpperCase();
  // centre of the square, as gridToLatLon does
  let lon = (g.charCodeAt(0) - 65) * 20 - 180 + Number(g[2]) * 2, lat = (g.charCodeAt(1) - 65) * 10 - 90 + Number(g[3]);
  if (g.length === 6) { lon += (g.charCodeAt(4) - 65) / 12 + 1 / 24; lat += (g.charCodeAt(5) - 65) / 24 + 1 / 48; }
  else { lon += 1; lat += 0.5; }
  db.prepare(`INSERT INTO servers (id,key_hash,name,url,kind,grid,lat,lon,country,status_json,created_at,
              updated_at,expires_at,slug,verified,until) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`)
    .run(r.id, r.key_hash, r.name, r.url, r.kind, g, lat, lon, r.country, r.status_json, r.created_at,
         r.updated_at, r.expires_at, r.slug, r.verified, r.until);
};

const HOSTILE = `<img src=x onerror=alert(1)>"'&<script>alert(2)</script>`;
// live candidates
add({ slug: 'evil',       name: HOSTILE,                grid: 'IO92NH' });                         // 0 km, all bands
add({ slug: 'oxford-v4',  name: 'Oxford V4',            grid: 'IO91IS' });                         // ~60 km, all bands
add({ slug: 'madrid-v4',  name: 'Madrid V4',            grid: 'IN80DK', country: 'ES' });           // far, all bands
add({ slug: 'locked-v4',  name: 'Club (PIN)',           grid: 'IO92NH', status: { pin: true, radios: [V4], listeners: 2, maxListeners: 2 } });
add({ slug: 'hf-only',    name: 'Airspy HF+ box',       grid: 'IO92NH',
      status: { radios: [{ name: 'Airspy HF+', driver: 'airspyhf' }], listeners: 1, maxListeners: 4 } });
add({ slug: 'fm-phone',   name: 'FM phone',             grid: 'IO92NH',
      status: { radios: [{ name: 'Phone', driver: 'rtl', coverage: ['FM Broadcast Band'], ranges: [[87500000, 108000000]] }],
                listeners: 1, maxListeners: 1 } });
// not live: must never be suggested
add({ slug: 'asleep',     name: 'Asleep',               grid: 'IO92NH', expires: T - 60 });
add({ slug: 'unproven',   name: 'Unproven',             grid: 'IO92NH', verified: 0 });

// the addresses under test
add({ slug: 'old-sony', name: 'Stuey3D SonyTV', grid: 'IO92NH', created: T - 20 * DAY, updated: T - 10 * DAY,
      expires: T - 10 * DAY + 1800,
      status: { radios: [{ name: 'RTLSDRBlog Blog V4', driver: 'rtl' }] } });   // no coverage: driver fallback
add({ slug: 'holiday',  name: 'On Holiday', grid: 'IO92NH', created: T - 30 * DAY, updated: T - 3 * DAY,
      expires: T - 3 * DAY + 1800 });
add({ slug: 'rebooting', name: 'Rebooting', grid: 'IO92NH', created: T - 30 * DAY, updated: T - 3600,
      expires: T - 1800 });
add({ slug: 'to-delist', name: 'Private Person', grid: 'IO92NH', status: { radios: [{ name: 'Airspy HF+', driver: 'airspyhf' }] } });
add({ slug: 'nickb',    name: 'NickB SDR', grid: 'IO81QL', url: 'https://nickb.example.com' });

// ── Upstream (the tunnel) ──────────────────────────────────────────────────────────────────────
let upstream = 'ok';
globalThis.fetch = async (url) => {
  if (upstream === 'throw') throw new Error('connection refused');
  if (upstream === '530') return new Response('<html>cloudflare 1033</html>', { status: 530, headers: { 'content-type': 'text/html' } });
  if (upstream === '503') return new Response('{"error":"radio not up"}', { status: 503, headers: { 'content-type': 'application/json' } });
  return new Response('<html><head></head><body>receiver</body></html>', { status: 200, headers: { 'content-type': 'text/html' } });
};

// ── Driver ─────────────────────────────────────────────────────────────────────────────────────
const get = async (url, { cf, method = 'GET', body, headers } = {}) => {
  const req = new Request(url, { method, body, headers });
  if (cf) Object.defineProperty(req, 'cf', { value: cf });
  const res = await worker.fetch(req, env);
  return { status: res.status, type: res.headers.get('content-type') || '', headers: res.headers, text: await res.text() };
};
const slugUrl = (s, p = '/') => `https://${s}.vibeserver.vibesdr.net${p}`;
const order = (html) => [...html.matchAll(/href="https:\/\/([a-z0-9-]+)\.vibeserver\.vibesdr\.net\/"/g)].map((m) => m[1]);

let pass = 0, fail = 0;
const check = (name, ok, extra = '') => {
  if (ok) { pass++; console.log('  ok  ', name); } else { fail++; console.log('  FAIL', name, extra); }
};
const save = (name, html) => { if (outDir) writeFileSync(`${outDir}/${name}.html`, html); };

console.log('known gone (past its hold, row still present)');
{
  const r = await get(slugUrl('old-sony'));
  save('gone', r.text);
  check('410', r.status === 410, r.status);
  check('html + noindex', r.type.startsWith('text/html') && r.headers.get('x-robots-tag') === 'noindex'
        && r.text.includes('<meta name="robots" content="noindex">'));
  check('wording', r.text.includes('Sorry — this server is no longer available at this address.')
        && r.text.includes('Here are some other servers you may like'));
  check('names the old server (escaped)', r.text.includes('Stuey3D SonyTV used to be here'));
  const o = order(r.text);
  check('order: same bands, open before PIN, then nearest',
        JSON.stringify(o) === JSON.stringify(['evil', 'oxford-v4', 'nickb', 'madrid-v4', 'locked-v4', 'hf-only']),
        JSON.stringify(o));
  check('never suggests itself, an offline or an unproven server',
        !o.includes('old-sony') && !o.includes('asleep') && !o.includes('unproven'));
  check('links the directory', r.text.includes('href="https://vibeserver.vibesdr.net/"'));
  check('shows distance + listeners + full', /Oxford V4.*km away/s.test(r.text) && r.text.includes('ONLINE · 1 LISTENING')
        && r.text.includes('FULL RIGHT NOW'));
  check('no script anywhere, CSP says so', !/<script/i.test(r.text)
        && (r.headers.get('content-security-policy') || '').includes("default-src 'none'"));
}

console.log('escaping a hostile server name');
{
  const r = await get(slugUrl('old-sony'));
  check('raw markup absent', !r.text.includes('<img src=x') && !r.text.includes('<script>alert'));
  check('escaped text present', r.text.includes('&lt;img src=x onerror=alert(1)&gt;&quot;&#39;&amp;&lt;script&gt;'));
}

console.log('gone, non-root paths');
{
  const a = await get(slugUrl('old-sony', '/vibeserver.json'));
  check('410 plain text for JSON path', a.status === 410 && a.type.startsWith('text/plain'), `${a.status} ${a.type}`);
  const w = await get(slugUrl('old-sony', '/ws'), { headers: { upgrade: 'websocket' } });
  check('410 plain text for a socket', w.status === 410 && w.type.startsWith('text/plain'), `${w.status} ${w.type}`);
}

console.log('delisted: note kept, row deleted, page from the note');
{
  const d = await get('https://vibeserver.vibesdr.net/api/directory/delist',
    { method: 'POST', body: JSON.stringify({ id: 'to-delist', key: 'key-to-delist' }), headers: { 'content-type': 'application/json' } });
  check('delist 200', d.status === 200, d.text);
  check('row gone', !db.prepare("SELECT 1 FROM servers WHERE id='to-delist'").get());
  const n = db.prepare("SELECT * FROM gone_slugs WHERE slug='to-delist'").get();
  check('note: coarse 4-char square centre, bands, no name', n && n.lat === 52.5 && n.lon === -1
        && JSON.parse(n.bands).includes('hf') && !('name' in n), JSON.stringify(n));
  const r = await get(slugUrl('to-delist'));
  save('delisted', r.text);
  check('410 page', r.status === 410 && r.text.includes('no longer available at this address'));
  check('does not name the delisted owner', !r.text.includes('Private Person'));
  check('same-band ranking from the note (HF box among the first)', order(r.text).slice(0, 5).includes('hf-only'),
        JSON.stringify(order(r.text)));
  // re-registering the slug clears the note
  await get('https://vibeserver.vibesdr.net/api/directory/register', { method: 'POST',
    body: JSON.stringify({ name: 'To Delist', grid: 'IO92', url: 'https://new.example.com', slug: 'to-delist' }),
    headers: { 'content-type': 'application/json' } });
  check('re-registration clears the note', !db.prepare("SELECT 1 FROM gone_slugs WHERE slug='to-delist'").get());
  // a note older than 180 days is pruned on the next write
  db.prepare("INSERT INTO gone_slugs (slug,lat,lon,country,bands,gone_at) VALUES ('ancient',52.5,-1,'GB','[]',?)").run(T - 200 * DAY);
  const r2 = await get(slugUrl('ancient'));
  check('an expired note reads as never-seen (404)', r2.status === 404, r2.status);
  await get('https://vibeserver.vibesdr.net/api/directory/delist',
    { method: 'POST', body: JSON.stringify({ id: 'hf-only', key: 'key-hf-only' }), headers: { 'content-type': 'application/json' } });
  check('pruned on delist', !db.prepare("SELECT 1 FROM gone_slugs WHERE slug='ancient'").get());
  add({ slug: 'hf-only', name: 'Airspy HF+ box', grid: 'IO92NH',
        status: { radios: [{ name: 'Airspy HF+', driver: 'airspyhf' }], listeners: 1, maxListeners: 4 } });
  db.prepare("DELETE FROM gone_slugs WHERE slug='hf-only'").run();
}

console.log('never seen: nearest the VISITOR');
{
  const r = await get(slugUrl('no-such-thing'), { cf: { latitude: '40.42', longitude: '-3.70', country: 'ES' } });
  save('unknown', r.text);
  check('404 html', r.status === 404 && r.type.startsWith('text/html'));
  check('wording', r.text.includes('Sorry — there is no server at this address.') && r.text.includes('Servers near you'));
  check('Madrid first for a visitor in Madrid', order(r.text)[0] === 'madrid-v4', JSON.stringify(order(r.text)));
  const p = await get(slugUrl('no-such-thing', '/api/x'));
  check('non-root 404 plain', p.status === 404 && p.type.startsWith('text/plain'));
}

console.log('offline but held');
{
  const a = await get(slugUrl('rebooting'));
  check('briefly offline: today\'s plain 503', a.status === 503 && a.type.startsWith('text/plain')
        && a.text === 'Rebooting is not online at the moment.\nIts address stays reserved, so this link will work again when it returns.',
        JSON.stringify(a.text));
  const b = await get(slugUrl('holiday'));
  save('away', b.text);
  check('away 3 days: 503 page with alternatives', b.status === 503 && b.type.startsWith('text/html')
        && b.text.includes('On Holiday has not been online for 3 days') && order(b.text).length === 6);
  const c = await get(slugUrl('holiday', '/vibeserver.json'));
  check('away, non-root: plain 503', c.status === 503 && c.type.startsWith('text/plain'));
}

console.log('listed, live');
{
  upstream = 'ok';
  const a = await get(slugUrl('nickb'));
  check('proxied as before', a.status === 200 && a.text.includes('__VIBE_DIRECT_HOST__'));
  upstream = 'throw';
  const b = await get(slugUrl('nickb'));
  save('down', b.text);
  check('tunnel throws: 503 down page + Retry-After', b.status === 503 && b.type.startsWith('text/html')
        && b.headers.get('retry-after') === '120'
        && b.text.includes("This server isn&#39;t responding right now")
        && b.text.includes('NickB SDR appears to be experiencing technical difficulties')
        && b.text.includes('>Try again</a>') && b.text.includes('>Back to the directory</a>'));
  check('down page ranks from THIS server (Oxford first)', order(b.text)[0] === 'oxford-v4', JSON.stringify(order(b.text)));
  upstream = '530';
  const c = await get(slugUrl('nickb'));
  check('Cloudflare 530: down page, not Cloudflare\'s', c.status === 503 && !c.text.includes('1033') && c.text.includes('responding'));
  const d = await get(slugUrl('nickb', '/vibeserver.json'));
  check('530 on a JSON path: plain 503', d.status === 503 && d.type.startsWith('text/plain') && d.headers.get('retry-after') === '120');
  upstream = '503';
  const e = await get(slugUrl('nickb', '/r/abc/connection'));
  check('the server\'s OWN 503 passes through untouched', e.status === 503 && e.text === '{"error":"radio not up"}');
  upstream = 'ok';
}

console.log('the directory list still reads the same live set');
{
  const r = await get('https://vibeserver.vibesdr.net/api/directory');
  const j = JSON.parse(r.text);
  const slugs = j.servers.map((s) => s.slug).sort();
  check('list = live + verified only', !slugs.includes('asleep') && !slugs.includes('unproven') && !slugs.includes('old-sony')
        && slugs.includes('madrid-v4'), JSON.stringify(slugs));
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
