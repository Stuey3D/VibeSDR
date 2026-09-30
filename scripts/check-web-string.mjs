#!/usr/bin/env node
/**
 * check-web-string.mjs — is a string actually in the COMPILED web client?
 *
 * ★★★ WHY THIS EXISTS: THE SERVER HIDES THE CLIENT. `vibe_web_page.h` stores every asset as base64
 *   across many C string literals — the page, the front door's copy of it, and each script file
 *   (web/dist/vs/*.js: the entry and the chunks it loads on demand), each in up to three encodings.
 *   So `strings` on the binary finds nothing and `grep` on the header finds nothing.
 *
 * ★★ A CHECK THAT SAYS "NOT THERE" WHEN IT MEANS "I CANNOT SEE IT" IS WORSE THAN NO CHECK.
 *   On 2026-09-26 that cost an hour: a CSS fix was verified against a tool that could not have
 *   seen it either way, and a JS change looked absent from a header that had it.
 *
 * This decodes the UNCOMPRESSED copy of every asset in the header and reports the page (HTML+CSS)
 * and each script separately, so an answer of 0 everywhere is a real absence.
 * ★ It also proves each compressed copy decodes to the uncompressed one — a mismatch there is a
 *   header the server would serve wrong to one class of browser only.
 *
 * Usage: node scripts/check-web-string.mjs "close to the limit" [more strings…]
 */
import { readFile } from 'node:fs/promises';
import zlib from 'node:zlib';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const HEADER = path.join(root, 'android/app/src/main/cpp/vibe_web_page.h');
const wanted = process.argv.slice(2);
if (!wanted.length) {
  console.error('usage: node scripts/check-web-string.mjs "<string>" [more…]');
  process.exit(2);
}

const raw = await readFile(HEADER, 'utf8');
/* ★ Every `static const char kVibeWebA<i>_<enc>[] = "…" "…";` — the literal's quoted chunks, joined.
 *  Anchored on the declaration so quoted text in the comments above can never be swept in. */
const lits = new Map();
for (const m of raw.matchAll(/static const char (kVibeWebA\d+_(?:raw|gz|br))\[\] =((?:\s*"[^"]*")+);/g)) {
  lits.set(m[1], Buffer.from([...m[2].matchAll(/"([^"]*)"/g)].map((q) => q[1]).join(''), 'base64'));
}
const rows = [...raw.matchAll(/^\s*\{ ("[^"]*"), "[^"]*", \{ (\w+), (\w+), (\w+) \}, \{ (\d+), (\d+), (\d+) \} \},$/gm)];
if (!rows.length) { console.error('!! no kVibeWebAssets table found — has build-web.mjs changed shape?'); process.exit(1); }

const assets = rows.map((r, i) => {
  const name = i === 0 ? 'page' : i === 1 ? 'page (front door)' : JSON.parse(r[1]);
  const body = lits.get(r[2]);
  if (!body || body.length !== Number(r[5])) { console.error(`!! ${name}: uncompressed copy missing or the wrong length`); process.exit(1); }
  for (const [n, dec] of [[r[3], zlib.gunzipSync], [r[4], zlib.brotliDecompressSync]]) {
    if (n === 'nullptr') continue;
    if (Buffer.compare(dec(lits.get(n)), body) !== 0) { console.error(`!! ${name}: ${n} does not decode to the uncompressed copy`); process.exit(1); }
  }
  return { name, body };
}).filter((a) => a.name !== 'page (front door)');

for (const a of assets) console.log(`${a.name.padEnd(28)} ${a.body.length.toLocaleString('en-GB').padStart(10)} bytes`);
console.log();

let missing = 0;
for (const w of wanted) {
  const needle = Buffer.from(w, 'utf8');
  const hits = assets.map((a) => [a.name, a.body.includes(needle) ? a.body.toString('latin1').split(Buffer.from(w, 'utf8').toString('latin1')).length - 1 : 0]);
  const ok = hits.some(([, n]) => n > 0);
  if (!ok) missing++;
  console.log(`  ${ok ? 'ok  ' : 'MISS'}  ${JSON.stringify(w)}  —  ${hits.filter(([, n]) => n).map(([a, n]) => `${a} ${n}`).join(', ') || 'nowhere'}`);
}
console.log(missing ? `\n${missing} string(s) NOT in the compiled client` : '\nall present');
process.exit(missing ? 1 : 0);
