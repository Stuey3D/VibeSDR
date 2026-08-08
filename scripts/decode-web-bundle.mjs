// ★★★ THE CLIENT JS IS BASE64 INSIDE THE HTML — grep the file and every source string returns 0.
//     That looks exactly like "my change did not build", and it has now sent me chasing a
//     non-existent bug FOUR separate times. Decode first, always. Usage:
//        node scripts/decode-web-bundle.mjs [pattern]
//
// ★★ PASS THE PATTERN TO THIS SCRIPT — do not pipe the output to grep. The bundle carries NUL
//    bytes (the embedded wasm), so grep treats it as binary and reports 0 matches for strings that
//    are demonstrably there. That is the SAME false negative as grepping the HTML, one level down,
//    and it cost an hour on 2026-08-08. If you must use grep, it needs `-a`. The pattern mode
//    below counts in JS and is immune.
import fs from 'node:fs';
const html = fs.readFileSync(new URL('../web/dist/vibesdr.html', import.meta.url), 'utf8');
// ★★ THERE IS MORE THAN ONE PAYLOAD. This matched only the FIRST atob() block, so a string that
//    lived in any other chunk reported 0 occurrences — the very false negative this script was
//    written to stop, one level down (2026-08-08).
const blocks = [...html.matchAll(/atob\("([A-Za-z0-9+/=]+)"\)/g)];
if (!blocks.length) { console.error('no base64 script payload found — has build-web.mjs changed?'); process.exit(2); }
const js = blocks.map((b) => Buffer.from(b[1], 'base64').toString('utf8')).join('\n//── next payload ──\n');
const pat = process.argv[2];
if (!pat) { process.stdout.write(js); process.exit(0); }
const n = js.split(pat).length - 1;
console.log(`${pat}: ${n} occurrence(s) in the DECODED bundle`);
process.exit(n ? 0 : 1);
