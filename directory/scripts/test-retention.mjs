// Test RETAIN_SEC (directory/src/index.js): nothing about a server is kept more than 90 days after it was last
// seen — against a REAL SQLite database built from schema.sql + every migration, as test-gone.mjs does.
//
//   node directory/scripts/test-retention.mjs
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync } from 'node:fs';

const dir = new URL('../', import.meta.url);
const db = new DatabaseSync(':memory:');
db.exec(readFileSync(new URL('schema.sql', dir), 'utf8'));
for (const f of readdirSync(new URL('migrations/', dir)).filter((f) => f.endsWith('.sql')).sort())
  db.exec(readFileSync(new URL('migrations/' + f, dir), 'utf8'));
db.exec("ALTER TABLE servers ADD COLUMN verify_note TEXT NOT NULL DEFAULT ''");
const stmt = (sql, args = []) => ({
  bind: (...a) => stmt(sql, a),
  first: async () => db.prepare(sql).get(...args) ?? null,
  all: async () => ({ results: db.prepare(sql).all(...args) }),
  run: async () => ({ meta: { changes: db.prepare(sql).run(...args).changes } }),
});
const env = { DB: { prepare: (sql) => stmt(sql),
  batch: async (l) => { db.exec('BEGIN'); try { for (const s of l) await s.run(); db.exec('COMMIT'); } catch (e) { db.exec('ROLLBACK'); throw e; } } } };
const { purgeStale } = await import(new URL('src/index.js', dir));

const T = 2_000_000_000, DAY = 86400;
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const add = (id, updated, expires) => db.prepare(
  `INSERT INTO servers (id,key_hash,name,url,kind,grid,lat,lon,country,status_json,created_at,updated_at,expires_at,slug,verified,until)
   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`).run(id, 'h', id, 'https://' + id + '.example', 'tunnel', 'IO92NH', 52.3, -0.9, 'GB', '{}',
   T - 400 * DAY, updated, expires, id, 1, 0);
const note = (slug, at) => db.prepare('INSERT INTO gone_slugs (slug, lat, lon, country, bands, gone_at) VALUES (?,?,?,?,?,?)')
  .run(slug, 52.5, -1, 'GB', '[]', at);

add('live', T - 60, T + 1800);                      // pinging now
add('away-a-week', T - 7 * DAY, T - 7 * DAY + 1800); // Nick's Pixel, off for a week
add('away-89d', T - 89 * DAY, T - 89 * DAY + 1800);
add('away-91d', T - 91 * DAY, T - 91 * DAY + 1800);
add('long-until', T - 120 * DAY, T + 3600);          // ★ odd but defensive: never delete a row still within its TTL
note('note-89d', T - 89 * DAY);
note('note-91d', T - 91 * DAY);

await purgeStale(env, T);
const ids = new Set(db.prepare('SELECT id FROM servers').all().map((r) => r.id));
const notes = new Set(db.prepare('SELECT slug FROM gone_slugs').all().map((r) => r.slug));
ok(ids.has('live'), 'a live server is kept');
ok(ids.has('away-a-week'), 'a server away a week keeps its row (temporary servers unchanged)');
ok(ids.has('away-89d'), 'a server last seen 89 days ago is kept');
ok(!ids.has('away-91d'), 'a server last seen 91 days ago is deleted');
ok(ids.has('long-until'), 'a row still inside its own TTL is never deleted');
ok(notes.has('note-89d') && !notes.has('note-91d'), 'address notes go at 90 days, not 180');
console.log(`directory retention: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
