/**
 * THE MAP PAGE'S PACK READER (src/services/mapPackRead.ts) — b96-satgrace, Mac Ha's AIS map: ships and NO LAND.
 *
 * beta96's app reads the bundled basemap by byte range over the React Native bridge with a 10 s limit per piece; one
 * timed-out first read (a busy bridge) and the basemap was abandoned silently. Proves, with the page's own reader (the
 * very source string MapOverlay embeds, evaluated here) on short simulated timeouts:
 *   • a piece answered at once: no retry;
 *   • the first try never answered (a busy bridge): asked again after a pause, the second answer wins;
 *   • an empty answer (a failed read): asked again;
 *   • a LATE answer to an earlier try still counts — and no further try is started after it;
 *   • every try failing: null (the page's GPU map then rejects → glfail), after exactly the set number of tries;
 *   • the source has no backtick (it lives inside MapOverlay's page template literal);
 * and, on MapOverlay.tsx's source: the page tells the app when the GPU basemap fails (glfail), the app rebuilds the map
 * on Leaflet (setGlOk(false)) and SHOWS the reason (glNote) — never only a console line.
 *
 * Run: node --no-warnings scripts/test_map_pack_read.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { PACK_READ_JS, PACK_READ_TIMEOUTS_MS } from '../src/services/mapPackRead.ts';

let fails = 0, passes = 0;
const ok = (c: boolean, what: string) => { if (c) passes++; else { fails++; console.error(`FAIL ${what}`); } };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

type Cb = (b: ArrayBuffer | null) => void;
type Post = (name: string, off: number, len: number, cb: Cb) => void;
// eslint-disable-next-line no-new-func
const make = new Function('return ' + PACK_READ_JS)() as (post: Post, opts: object) => (n: string, o: number, l: number) => Promise<ArrayBuffer | null>;
const buf = (n: number) => new Uint8Array(n).buffer;
const OPTS = { timeouts: [60, 90, 120], pauses: [10, 20] };

async function run() {
  ok(!PACK_READ_JS.includes('`'), 'no backtick in the page source');
  ok(PACK_READ_TIMEOUTS_MS.length >= 3 && PACK_READ_TIMEOUTS_MS.every((t, i) => i === 0 || t > PACK_READ_TIMEOUTS_MS[i - 1]),
     `a few tries, each waiting longer (${PACK_READ_TIMEOUTS_MS.join(', ')} ms)`);

  { // answered at once
    let sent = 0, retries = 0;
    const read = make((_n, _o, l, cb) => { sent++; cb(buf(l)); }, { ...OPTS, onRetry: () => retries++ });
    const r = await read('vibemap-basic.pmtiles', 0, 7);
    ok(!!r && r.byteLength === 7 && sent === 1 && retries === 0, `answered at once: one request, no retry (${sent}, ${retries})`);
  }
  { // the first try never answered — the bridge was busy
    let sent = 0; const whys: string[] = [];
    const t0 = Date.now();
    const read = make((_n, _o, l, cb) => { sent++; if (sent >= 2) setTimeout(() => cb(buf(l)), 5); }, { ...OPTS, onRetry: (_n: string, k: number, w: string) => whys.push(`${k}:${w}`) });
    const r = await read('vibemap-basic.pmtiles', 0, 7);
    const took = Date.now() - t0;
    ok(!!r && r.byteLength === 7, '★ a timed-out first read is asked again and the bytes arrive');
    ok(sent === 2 && whys.length === 1 && /^2:no answer in/.test(whys[0]), `one retry, said why (${whys.join(' | ')})`);
    ok(took >= 60 + 10, `the retry waited the first try's limit and a pause (${took} ms)`);
  }
  { // an empty answer
    let sent = 0;
    const read = make((_n, _o, l, cb) => { sent++; cb(sent === 1 ? null : buf(l)); }, OPTS);
    const r = await read('vibemap-basic.pmtiles', 100, 64);
    ok(!!r && r.byteLength === 64 && sent === 2, `an empty answer is asked again (${sent} requests)`);
  }
  { // a LATE answer to the first try still counts
    let sent = 0; let firstCb: Cb | null = null;
    const read = make((_n, _o, l, cb) => { sent++; if (sent === 1) { firstCb = cb; setTimeout(() => firstCb && firstCb(buf(l)), 85); } }, OPTS);
    const r = await read('vibemap-basic.pmtiles', 0, 16);
    await sleep(250);
    ok(!!r && r.byteLength === 16, '★ the first try answering late (after the second was sent) is taken');
    ok(sent === 2, `…and no third try was started (${sent} requests)`);
  }
  { // every try fails
    let sent = 0;
    const read = make((_n, _o, _l, _cb) => { sent++; }, OPTS);
    const t0 = Date.now();
    const r = await read('vibemap-basic.pmtiles', 0, 7);
    ok(r === null, '★ every try failing gives null (the page then reports glfail)');
    ok(sent === 3, `exactly the set number of tries (${sent})`);
    ok(Date.now() - t0 >= 60 + 10 + 90 + 20 + 120 - 5, `…each waited its own limit (${Date.now() - t0} ms)`);
  }

  // ── MapOverlay: the failure is shown, and the map falls back ──
  const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
  const src = readFileSync(path.join(root, 'src/components/MapOverlay.tsx'), 'utf8');
  ok(/var packRange=window\.ReactNativeWebView\?\(\$\{PACK_READ_JS\}\)\(/.test(src), "the page's packRange is the retrying reader");
  ok(/GLVM\.ready\.catch\(function\(e\)\{[^}]*postMessage\(JSON\.stringify\(\{t:'glfail'/.test(src.replace(/\n\s*/g, '')), 'the page tells the app when the GPU basemap fails (glfail)');
  const h = src.indexOf("(m as any).t === 'glfail'");
  const hb = src.slice(h, h + 700);
  ok(h > 0 && /setGlNote\(/.test(hb) && /setGlOk\(false\)/.test(hb), 'the app shows the reason and rebuilds on Leaflet');
  ok(/\{!!glNote && !packError && !!pageUri && \(/.test(src) && /\{glNote\}/.test(src), 'the reason is drawn on the map');
  ok(/'The map could not be prepared on this device\.\\n' \+ packError/.test(src), 'packError (the pack could not be prepared at all) is still shown full-screen');

  console.log(`${passes} passed, ${fails} failed`);
  process.exit(fails ? 1 : 0);
}
run().catch((e) => { console.error(e); process.exit(1); });
