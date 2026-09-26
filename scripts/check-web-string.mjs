#!/usr/bin/env node
/**
 * check-web-string.mjs — is a string actually in the COMPILED web client?
 *
 * ★★★ WHY THIS EXISTS: THE PAGE HIDES ITS JAVASCRIPT TWICE. `vibe_web_page.h` stores the page as
 *   base64 across many C string literals; the page then stores its own bundle as base64 AGAIN,
 *   inside an `atob(...)` call. So `strings` on the binary finds nothing, `grep` on the header
 *   finds nothing, and — this is the dangerous part — `scripts/decode-web-bundle.mjs` reports
 *   ZERO for CSS because it only ever looks at the JS layer.
 *
 * ★★ A CHECK THAT SAYS "NOT THERE" WHEN IT MEANS "I CANNOT SEE IT" IS WORSE THAN NO CHECK.
 *   On 2026-09-26 that cost an hour: a CSS fix was verified against a tool that could not have
 *   seen it either way, and a JS change looked absent from a header that had it.
 *
 * This decodes BOTH layers and reports each separately, so an answer of 0/0 is a real absence.
 *
 * Usage: node scripts/check-web-string.mjs "close to the limit" [more strings…]
 */
import { readFile } from 'node:fs/promises';
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
const at = raw.indexOf('kVibeWebPageB64');
if (at < 0) { console.error('!! kVibeWebPageB64 not found — has build-web.mjs changed shape?'); process.exit(1); }
/* ★ Every quoted chunk AFTER the declaration, with no length filter: an earlier version required
 *  16+ chars and searched the whole file, which both dropped short chunks and swept in quoted
 *  text from the comments above. */
const b64 = [...raw.slice(at).matchAll(/"([^"]*)"/g)].map((m) => m[1]).join('');
const page = Buffer.from(b64, 'base64');

const m = /atob\("([A-Za-z0-9+/=]+)"\)/.exec(page.toString('latin1'));
const js = m ? Buffer.from(m[1], 'base64') : Buffer.alloc(0);
if (!m) console.error('!! no inner atob() bundle found — the JS layer could not be read');

console.log(`page (HTML+CSS): ${page.length.toLocaleString()} bytes`);
console.log(`bundle (JS):     ${js.length.toLocaleString()} bytes\n`);

let missing = 0;
for (const w of wanted) {
  const needle = Buffer.from(w, 'utf8');
  const inPage = page.includes(needle) ? page.toString('latin1').split(w).length - 1 : 0;
  const inJs = js.includes(needle) ? js.toString('latin1').split(w).length - 1 : 0;
  const ok = inPage + inJs > 0;
  if (!ok) missing++;
  console.log(`  ${ok ? 'ok  ' : 'MISS'}  ${JSON.stringify(w)}  —  page ${inPage}, bundle ${inJs}`);
}
console.log(missing ? `\n${missing} string(s) NOT in the compiled client` : '\nall present');
process.exit(missing ? 1 : 0);
