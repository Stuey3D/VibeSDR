// ★★★ THE CLIENT JS IS BASE64 INSIDE THE HTML — grep the file and every source string returns 0.
//     That looks exactly like "my change did not build", and it has now sent me chasing a
//     non-existent bug FOUR separate times. Decode first, always. Usage:
//        node scripts/decode-web-bundle.mjs [pattern]
import fs from 'node:fs';
const html = fs.readFileSync(new URL('../web/dist/vibesdr.html', import.meta.url), 'utf8');
const m = html.match(/atob\("([A-Za-z0-9+/=]+)"\)/);
if (!m) { console.error('no base64 script payload found — has build-web.mjs changed?'); process.exit(2); }
const js = Buffer.from(m[1], 'base64').toString('utf8');
const pat = process.argv[2];
if (!pat) { process.stdout.write(js); process.exit(0); }
const n = js.split(pat).length - 1;
console.log(`${pat}: ${n} occurrence(s) in the DECODED bundle`);
process.exit(n ? 0 : 1);
