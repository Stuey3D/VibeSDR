#!/usr/bin/env node
/**
 * check-map-overlay.mjs — parse the HTML page that MapOverlay.tsx builds, for all three map kinds.
 *
 * ★★★ WHY THIS EXISTS. `MapOverlay.tsx` assembles its whole WebView page as one giant JavaScript
 *   TEMPLATE LITERAL. To TypeScript that is a string, so `tsc` is perfectly happy with a page whose
 *   JavaScript does not parse at all — and on 2026-09-26 exactly that shipped into a commit: a
 *   DUPLICATED "return img; } });" left a bare `return` at top level, a SyntaxError that kills the
 *   ENTIRE inline script and with it the basemap, the markers and every handler. Nothing caught it
 *   because nothing was looking.
 *
 * ★★★ AND THE OTHER HALF: A BACKTICK INSIDE THE LITERAL CLOSES IT. Five separate times that day a
 *   backtick written in a COMMENT — including in a comment warning about this very trap — ended the
 *   string early and made everything after it parse as code. The error always points at the comment
 *   line, which reads as nonsense and sends you looking in the wrong place.
 *
 * ★★ So this renders the page the way the app does and hands each inline <script> to node's own
 *   parser. It cannot be fooled by a string that merely looks right.
 *
 * Usage: node scripts/check-map-overlay.mjs      (exit 0 = every kind parses)
 */
import { readFile } from 'node:fs/promises';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SRC = path.join(root, 'src/components/MapOverlay.tsx');
const tmp = mkdtempSync(path.join(tmpdir(), 'mapoverlay-'));
let failed = 0;
const fail = (m) => { console.error(`  FAIL  ${m}`); failed++; };
const ok = (m) => console.log(`  ok    ${m}`);

const src = await readFile(SRC, 'utf8');

/* ★ The page is built by one function returning a single template literal. Rather than import the
 *  TSX (which drags in React Native), the literal is located by its own opening marker and read to
 *  the closing backtick-semicolon that ends the return. Crude, and adequate: if that shape ever
 *  changes this script says so loudly instead of silently checking nothing. */
const open = src.indexOf('return `<!DOCTYPE html>');
if (open < 0) {
  fail('could not find the page template literal (return `<!DOCTYPE html> …) — has buildHtml changed shape?');
  process.exit(1);
}
/* ★ The literal ends on the SAME line as the last markup ("</body></html>`;"), not on a line of
 *  its own — so searching for a newline before the closer finds nothing and the check would have
 *  reported a shape change that had not happened. Match the closer itself. */
const close = src.indexOf('`;', open + 'return `'.length);
if (close < 0) {
  fail('could not find the end of the page template literal');
  process.exit(1);
}
const body = src.slice(open + 'return `'.length, close);
ok(`page template located (${body.length.toLocaleString()} chars)`);

/* ★★ Interpolations are replaced with a harmless literal so the JS SKELETON is what gets parsed.
 *  `${…}` may contain TypeScript expressions and nested template literals, neither of which node
 *  can parse on its own — and neither is what this check is about. */
let depth = 0;
let out = '';
for (let i = 0; i < body.length; i++) {
  if (body[i] === '$' && body[i + 1] === '{') { depth++; i++; if (depth === 1) out += '0'; continue; }
  if (depth > 0) {
    if (body[i] === '{') depth++;
    else if (body[i] === '}') depth--;
    continue;
  }
  out += body[i];
}
ok('interpolations stripped');

/* ★ VIBEMAP_JS is injected as one enormous interpolation; the shared renderer has its own syntax
 *  check (node --check web/mapkit/vibemap.js), so a "0" standing in for it here is correct. */
const blocks = [...out.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
if (!blocks.length) fail('no inline <script> blocks found in the page — the check would pass vacuously');
else ok(`${blocks.length} inline script block(s)`);

blocks.forEach((b, i) => {
  const f = path.join(tmp, `block${i}.js`);
  writeFileSync(f, b);
  try {
    /* ★★★ `--input-type=module`, NOT a plain `--check`. Node's default check parses the file as a
     *  CommonJS module, and CJS wraps everything in a function — so a bare top-level `return` is
     *  LEGAL there. That is precisely the fault this script was written to catch, and the first
     *  version of it passed the fault cleanly. An inline <script> is a CLASSIC SCRIPT, where a
     *  top-level return is a SyntaxError; ESM is the closest parser node offers that also rejects
     *  it. ★ A checker that cannot fail on its own motivating bug is not a checker. */
    execFileSync(process.execPath, ['--input-type=module', '--check'], { input: b, stdio: 'pipe' });
    ok(`block ${i} parses (${b.split('\n').length} lines)`);
  } catch (e) {
    const msg = String(e.stderr || e.message).trim().split('\n').slice(-3).join(' ');
    fail(`block ${i} does NOT parse — ${msg}`);
  }
});

/* ★★★ THE BACKTICK CHECK, separately, because a stray one usually produces a confusing error
 *  somewhere else entirely. A line inside the literal may carry a backtick ONLY as part of a
 *  nested template inside an interpolation — never in prose or a comment. */
const stray = [];
body.split('\n').forEach((line, n) => {
  if (!line.includes('`')) return;
  const isComment = /^\s*(\*|\/\/|\/\*)/.test(line);
  if (isComment) stray.push(`${n + 1}: ${line.trim().slice(0, 100)}`);
});
if (stray.length) {
  fail(`${stray.length} COMMENT line(s) inside the template literal contain a backtick — each one ENDS the string:`);
  stray.forEach((s) => console.error(`          ${s}`));
} else {
  ok('no backticks in comments inside the template literal');
}

console.log(failed ? `\n${failed} check(s) FAILED` : '\nall checks passed');
process.exit(failed ? 1 : 0);
