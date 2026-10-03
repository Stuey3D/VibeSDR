// Gzip-bomb ceiling on the spectrum socket's JSON frames (src/utils/boundedInflate)
// run: node --no-warnings scripts/test_bounded_inflate.ts
import { gzip } from 'pako';
import { ungzipToStringCapped } from '../src/utils/boundedInflate.ts';
let fails = 0, passes = 0;
const ok = (what: string, cond: boolean) => { if (cond) passes++; else { fails++; console.log(`✗ ${what}`); } };

const msg = JSON.stringify({ type: 'config', centerFreq: 96_600_000, name: 'Ünïcödé 😀' });
ok('round trip', ungzipToStringCapped(gzip(msg)) === msg);

const big = 'x'.repeat(256 * 1024);
ok('exactly the size of a 256 KB frame passes under the default cap', ungzipToStringCapped(gzip(big)).length === big.length);

const bomb = gzip(new Uint8Array(20 * 1024 * 1024));          // 20 MB of zeros -> ~20 KB on the wire
ok('bomb is small on the wire', bomb.length < 64 * 1024);
let threw = false;
try { ungzipToStringCapped(bomb); } catch { threw = true; }
ok('bomb past the 8 MB cap throws', threw);

threw = false;
try { ungzipToStringCapped(gzip(big), 1000); } catch { threw = true; }
ok('custom cap honoured', threw);

threw = false;
try { ungzipToStringCapped(new Uint8Array([0x1f, 0x8b, 1, 2, 3])); } catch { threw = true; }
ok('corrupt gzip throws', threw);

console.log(`bounded inflate: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
