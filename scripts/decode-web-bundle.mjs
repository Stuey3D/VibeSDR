// ★★★ WHERE THE CLIENT JS IS. Since 2026-09-30 the page no longer carries its script: build-web.mjs
//     writes it beside the page as web/dist/vs/app-<hash>.js plus split chunks (c-<hash>.js, loaded
//     on demand), and the HTML shell holds only a loader. So grepping vibesdr.html for a source
//     string finds NOTHING — that looks exactly like "my change did not build", and (in the old
//     base64 layout) sent me chasing a non-existent bug FOUR separate times. Use this. Usage:
//        node scripts/decode-web-bundle.mjs [pattern]
//
// ★★ PASS THE PATTERN TO THIS SCRIPT — do not pipe the output to grep. The bundle carries NUL
//    bytes (the embedded wasm), so grep treats it as binary and reports 0 matches for strings that
//    are demonstrably there. If you must use grep, it needs `-a`. The pattern mode below counts in
//    JS, per file, and is immune.
// ★★ THERE IS MORE THAN ONE FILE. A string that lives in a lazily loaded chunk (the map renderer,
//    the admin panel, the WASM Opus decoder) is not in app-*.js — so every file is searched and each
//    is reported, never just the entry.
// ★ For what the SERVER will actually send (the compiled header), see check-web-string.mjs.
import fs from 'node:fs';
const dir = new URL('../web/dist/vs/', import.meta.url);
let names = [];
try { names = fs.readdirSync(dir).filter((n) => n.endsWith('.js')).sort((a, b) => (a.startsWith('app-') ? -1 : b.startsWith('app-') ? 1 : a.localeCompare(b))); } catch {}
if (!names.length) { console.error('no web/dist/vs/*.js — run node scripts/build-web.mjs (has build-web.mjs changed?)'); process.exit(2); }
const files = names.map((n) => [n, fs.readFileSync(new URL(n, dir), 'utf8')]);
const pat = process.argv[2];
if (!pat) {
  process.stdout.write(files.map(([n, t]) => `//── ${n} ──\n${t}`).join('\n'));
  process.exit(0);
}
let total = 0;
for (const [n, t] of files) {
  const k = t.split(pat).length - 1;
  total += k;
  if (k) console.log(`  ${n}: ${k}`);
}
console.log(`${pat}: ${total} occurrence(s) in the bundle (${files.length} file(s))`);
process.exit(total ? 0 : 1);
