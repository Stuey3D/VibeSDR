// The aerial's ranges and filters on the directory (2026-10-06): a radio's `antennaRanges` /
// `antennaFilters` are accepted on ping, cleaned (cleanBandsText in src/index.js), stored, and
// returned in the listing — and a server too old to send them lists exactly as before.
// Against a REAL SQLite database built from schema.sql + every migration, as test-gone.mjs does;
// register → ping → GET /api/directory through the Worker's own fetch(), the path a server drives.
//
//   node directory/scripts/test-antenna-bands.mjs
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync } from 'node:fs';
import { createHmac } from 'node:crypto';

const dir = new URL('../', import.meta.url);
const db = new DatabaseSync(':memory:');
db.exec(readFileSync(new URL('schema.sql', dir), 'utf8'));
for (const f of readdirSync(new URL('migrations/', dir)).filter((f) => f.endsWith('.sql')).sort())
  db.exec(readFileSync(new URL('migrations/' + f, dir), 'utf8'));
// ★ verify_note exists in production (ping() writes it) but in no migration file — added by hand.
db.exec("ALTER TABLE servers ADD COLUMN verify_note TEXT NOT NULL DEFAULT ''");
const stmt = (sql, args = []) => ({
  bind: (...a) => stmt(sql, a),
  first: async () => db.prepare(sql).get(...args) ?? null,
  all: async () => ({ results: db.prepare(sql).all(...args) }),
  run: async () => ({ meta: { changes: db.prepare(sql).run(...args).changes } }),
});
const env = {
  DB: { prepare: (sql) => stmt(sql),
    batch: async (l) => { db.exec('BEGIN'); try { for (const s of l) await s.run(); db.exec('COMMIT'); } catch (e) { db.exec('ROLLBACK'); throw e; } } },
  ASSETS: { fetch: async () => new Response('asset', { status: 200 }) },
};
const mod = await import(new URL('src/index.js', dir));
const worker = mod.default;
const { cleanBandsText, capRadio, BANDS_TEXT_MAX } = mod;

let pass = 0, fail = 0;
const ok = (c, m, extra) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m + (extra !== undefined ? '  ' + JSON.stringify(extra) : '')); } };

// ── The cleaner ───────────────────────────────────────────────────────────────────────────────
ok(cleanBandsText('0-300MHz Wideband loop; [B] 144-146MHz 2 m') === '0-300MHz Wideband loop; [B] 144-146MHz 2 m',
   'ordinary text passes untouched (brackets for the socket survive)');
ok(cleanBandsText('bandstop 87.5–108 MHz FM band-stop') === 'bandstop 87.5–108 MHz FM band-stop', 'en dash survives');
ok(cleanBandsText('1-2MHz <script>alert(1)</script>') === '1-2MHz script alert(1) /script', 'no markup characters',
   cleanBandsText('1-2MHz <script>alert(1)</script>'));
ok(!/["\\`]/.test(cleanBandsText('1-2MHz a "b" \\c `d`')), 'no quotes, backslashes or backticks');
ok(cleanBandsText('1-2MHz\nx\u0000y\u202Ez') === '1-2MHz x y z', 'controls and direction overrides become spaces');
ok(cleanBandsText('  1-2MHz;  ') === '1-2MHz', 'trimmed, trailing separator dropped');
ok(cleanBandsText(42) === '' && cleanBandsText(null) === '' && cleanBandsText(['1-2MHz']) === '', 'non-strings say nothing');
{
  const long = Array.from({ length: 100 }, (_, i) => `${i}-${i + 1}MHz band ${i}`).join('; ');
  const c = cleanBandsText(long);
  ok(c.length <= BANDS_TEXT_MAX, 'capped at the server\'s 800', c.length);
  ok(c.split('; ').every((e) => /^\d+-\d+MHz band \d+$/.test(e)), '★ cut at a whole entry — no half-entry survives the cap');
  ok(cleanBandsText('x'.repeat(2000)) === '', 'one over-long entry with nowhere to cut is dropped, not truncated');
}
// ── capRadio: the two fields, and nothing else changed ────────────────────────────────────────
{
  const r = capRadio({ name: 'V4', antenna: 'Loop', antennaRanges: '0-30MHz <b>', antennaFilters: '   ', centreHz: 1 });
  ok(r.antennaRanges === '0-30MHz b', 'capRadio cleans antennaRanges', r);
  ok(!('antennaFilters' in r), 'empty after cleaning = omitted', r);
  ok(r.name === 'V4' && r.antenna === 'Loop' && r.centreHz === 1, 'other fields as before');
  const n = capRadio({ antennaRanges: 7, antennaFilters: { a: 1 } });
  ok(!('antennaRanges' in n) && !('antennaFilters' in n), 'a non-string is not stored', n);
}

// ── Round trip: register → ping → listing ─────────────────────────────────────────────────────
let currentKey = '';
globalThis.fetch = async (url) => {
  const nonce = new URL(String(url)).searchParams.get('dirNonce') || '';
  const dirProof = createHmac('sha256', currentKey).update(nonce).digest('hex');
  return new Response(JSON.stringify({ dirProof }), { status: 200, headers: { 'content-type': 'application/json' } });
};
const call = async (path, body) => {
  const req = new Request('https://vibeserver.vibesdr.net' + path, body === undefined ? {}
    : { method: 'POST', body: JSON.stringify(body), headers: { 'content-type': 'application/json' } });
  const res = await worker.fetch(req, env);
  return { status: res.status, j: JSON.parse(await res.text()) };
};
const listed = async (slug) => (await call('/api/directory?proto=99')).j.servers.find((s) => s.slug === slug);

const R1 = { id: 'r1', name: 'RTL-SDR Blog V4', driver: 'rtlsdr', antenna: 'Wideband loop',
  antennaRanges: '0-300MHz Wideband loop; [B] 144-146MHz 2 m',
  antennaFilters: 'bandstop 87.5-108MHz FM band-stop; highpass 1.7MHz' };
const OLD = { id: 'r9', name: 'RTL-SDR Blog V4', driver: 'rtlsdr', antenna: 'Discone', centreHz: 96600000 };

async function register(slug, radios, grid) {
  const reg = await call('/api/directory/register', { name: slug, grid, url: `https://${slug}.example.com`, slug });
  ok(reg.status === 200 && reg.j.key, `register ${slug}`, reg);
  currentKey = reg.j.key;
  const p = await call('/api/directory/ping', { id: reg.j.id, key: reg.j.key, url: `https://${slug}.example.com`,
    status: { radios, listeners: 0, maxListeners: 1 } });
  ok(p.status === 200, `ping ${slug}`, p);
  return reg.j;
}

await register('bands-new', [R1, { ...R1, id: 'r2', antennaRanges: '1-2MHz "evil" <img src=x>', antennaFilters: '' }], 'IO92NH');
{
  const s = await listed('bands-new');
  ok(!!s, 'the new server is listed');
  const [a, b] = s ? s.radios : [];
  ok(a && a.antennaRanges === R1.antennaRanges && a.antennaFilters === R1.antennaFilters, '★ round trip: both fields come back as sent', a);
  ok(a && a.antenna === 'Wideband loop', 'the antenna line is untouched', a);
  ok(b && b.antennaRanges === '1-2MHz evil img src=x', '★ round trip: markup cleaned on the way out', b);
  ok(b && !('antennaFilters' in b), 'an empty field is not listed at all', b);
  // ★ What is STORED is the server's own text — cleaning happens on the way out, like every radio field,
  //   so a later fix to the cleaner applies to every listing without waiting for the next ping.
  const row = db.prepare("SELECT status_json FROM servers WHERE slug='bands-new'").get();
  ok(JSON.parse(row.status_json).radios[0].antennaRanges === R1.antennaRanges, 'stored as sent');
}

const before = capRadio(OLD);
await register('bands-old', [OLD], 'IO81QL');
{
  const s = await listed('bands-old');
  const r = s && s.radios[0];
  ok(r && !('antennaRanges' in r) && !('antennaFilters' in r), '★ an old server sends nothing and gets nothing', r);
  ok(JSON.stringify(r) === JSON.stringify(before), '★ an old server\'s radio lists exactly as before', [r, before]);
}

console.log(`directory antenna bands: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
