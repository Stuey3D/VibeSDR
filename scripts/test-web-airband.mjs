/**
 * test-web-airband.mjs — the web client's airband behaves like an aviation radio, and a bookmark
 * brings back the passband it was saved with.
 *
 *   node scripts/build-web.mjs && node scripts/test-web-airband.mjs
 *
 * Drives the REAL built page in headless Edge against the mock VibeServer, through the controls a
 * listener uses (the entry box, the step menu, the tune arrows, the mode picker, the bandwidth
 * slider, the bookmarks panel), and checks what reached the "server" (GET /debug/controls):
 *   1. typing a channel NAME tunes its true frequency (118.010 → 118008333 Hz) and the readout shows
 *      the name with "8.33 · 118.0083" beside it; the passband default follows the spacing (±2.8k);
 *   2. an unused name (118.020) is refused with the reason, and nothing is tuned;
 *   3. in 8.33 mode the arrows walk names — 118.015, 118.025 (25 kHz, ±8.5k), 118.030 — to the hertz;
 *   4. 121.5 is the 25 kHz channel 121.500;
 *   5. a bookmark saved with ±3 kHz comes back with ±3 kHz (NickB, 2026-09-29).
 * ★ SILENT: --mute-audio. One throwaway profile dir under the OS temp dir, deleted at the end.
 */
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';

const PORT = 48011;
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const CDP = 9346;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const prof = mkdtempSync(path.join(tmpdir(), 'vibesdr-airband-'));

const mock = spawn(process.execPath, ['scripts/mock-vibeserver.mjs', '--no-pin', '--port', String(PORT)], { stdio: 'ignore' });
const edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${CDP}`, '--no-first-run', '--no-default-browser-check',
  '--mute-audio', `--user-data-dir=${prof}`, '--window-size=1280,900', '--autoplay-policy=no-user-gesture-required', 'about:blank'],
  { stdio: 'ignore' });
let fails = 0;
const ok = (c, m) => { console.log((c ? '  ok   ' : '  FAIL ') + m); if (!c) fails++; };
const cleanup = () => { try { edge.kill(); } catch (e) { console.error(e); } try { mock.kill(); } catch (e) { console.error(e); }
                        setTimeout(() => { try { rmSync(prof, { recursive: true, force: true }); } catch (e) { console.error('profile cleanup', e); } }, 800); };

try {
  await sleep(2500);
  const page = (await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).find((t) => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const uncaught = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); return; }
    if (m.method === 'Runtime.exceptionThrown') uncaught.push(m.params.exceptionDetails?.exception?.description ?? m.params.exceptionDetails?.text);
  };
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  await new Promise((r) => { ws.onopen = r; });
  await send('Page.enable'); await send('Runtime.enable');
  const ev = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
  const controls = async () => (await fetch(`http://127.0.0.1:${PORT}/debug/controls`)).json();
  const text = (id) => ev(`document.getElementById(${JSON.stringify(id)})?.textContent ?? null`);

  await send('Page.navigate', { url: `http://localhost:${PORT}/index.html?join` });
  await sleep(5000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await sleep(3000);

  const pickMode = async (m) => {
    await ev(`document.getElementById('mMode').click()`);
    await sleep(200);
    const hit = await ev(`(() => { const b = [...document.querySelectorAll('#mModeMenu .mModeOpt')].find(x => x.textContent.trim() === ${JSON.stringify(m)}); if (b) b.click(); return !!b; })()`);
    await ev(`document.getElementById('mModeMenu')?.remove()`);
    await sleep(300);
    return hit;
  };
  const enter = async (v) => {
    await ev(`(() => { const p = document.getElementById('freqPanel'); if (!p || !p.classList.contains('open')) document.getElementById('pill').click(); })()`);
    await sleep(150);
    await ev(`(() => { const i = document.getElementById('freqInput'); i.value = ${JSON.stringify(v)}; i.dispatchEvent(new Event('input')); document.getElementById('freqGo').click(); })()`);
    await sleep(400);
  };
  const pickStep = async (label) => {
    await ev(`document.getElementById('stepBtn').click()`);
    await sleep(150);
    const hit = await ev(`(() => { const b = [...document.querySelectorAll('#stepMenu button')].find(x => x.textContent.trim() === ${JSON.stringify(label)}); if (b) b.click(); return !!b; })()`);
    await sleep(200);
    return hit;
  };
  const up = async () => {
    await ev(`(() => { const el = document.getElementById('tuneUp'); el.dispatchEvent(new PointerEvent('pointerdown', { button: 0, bubbles: true })); window.dispatchEvent(new PointerEvent('pointerup', { bubbles: true })); })()`);
    await sleep(250);
  };
  const tunedHz = async () => (await controls()).tune?.frequency;
  const bw = async () => { const b = (await controls()).bandwidth; return b ? [b.bandwidthLow, b.bandwidthHigh] : null; };

  ok(await pickMode('AM'), 'AM chosen from the mode picker');

  // 1. a channel name
  await enter('118.010');
  ok(await tunedHz() === 118008333, `1. "118.010" tuned 118008333 Hz (got ${await tunedHz()})`);
  ok(await text('freq') === '118.010', `1. readout shows the channel name (${await text('freq')})`);
  const tag = await text('freqChan');
  ok(/8\.33/.test(tag || '') && /118\.0083/.test(tag || ''), `1. spacing + true frequency beside it (${tag})`);
  ok(JSON.stringify(await bw()) === '[-2800,2800]', `1. passband default followed 8.33 spacing (${JSON.stringify(await bw())})`);

  // 2. a name that does not exist
  await enter('118.020');
  const msg = await text('freqMsg');
  ok(/not an aviation channel/.test(msg || '') && /118\.015/.test(msg || '') && /118\.025/.test(msg || ''), `2. 118.020 refused with its neighbours (${msg})`);
  ok(await tunedHz() === 118008333, '2. and nothing was tuned');
  await ev(`document.getElementById('freqClose')?.click()`);

  // 3. the 8.33 knob walk
  ok(await pickStep('8.33kHz'), '3. 8.33kHz is on the airband step ladder');
  const walk = [];
  for (let i = 0; i < 3; i++) { await up(); walk.push([await text('freq'), await tunedHz()]); }
  ok(JSON.stringify(walk) === JSON.stringify([['118.015', 118016667], ['118.025', 118025000], ['118.030', 118025000]]),
     `3. walked 118.015 → 118.025 (25 kHz) → 118.030 (8.33), same Hz for the last two: ${JSON.stringify(walk)}`);
  ok(JSON.stringify(await bw()) === '[-2800,2800]', `3. back on an 8.33 name the passband is ±2.8k again (${JSON.stringify(await bw())})`);

  // 4. the emergency channel
  await enter('121.5');
  ok(await tunedHz() === 121500000 && await text('freq') === '121.500', `4. 121.5 → 121.500 (${await text('freq')}, ${await tunedHz()})`);
  ok((await text('freqChan')) === '25 kHz', `4. tagged as a 25 kHz channel (${await text('freqChan')})`);
  ok(JSON.stringify(await bw()) === '[-8500,8500]', `4. passband default for 25 kHz ±8.5k (${JSON.stringify(await bw())})`);
  await ev(`document.getElementById('freqClose')?.click()`);

  // 5. a bookmark keeps its passband
  await ev(`(() => { const s = document.getElementById('bwSync'); return s ? s.classList.contains('on') : null; })()`);
  await ev(`(() => { const hi = document.getElementById('bwHi'); hi.value = '3000'; hi.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await sleep(300);
  const narrow = await bw();
  ok(JSON.stringify(narrow) === '[-3000,3000]', `5. narrowed to ±3 kHz before saving (${JSON.stringify(narrow)})`);
  await ev(`document.getElementById('bookmarksBtn').click()`);
  await sleep(200);
  await ev(`(() => { document.getElementById('bmName').value = 'WEAK AM'; document.getElementById('bmAdd').click(); })()`);
  await sleep(400);
  await ev(`document.getElementById('bmClose')?.click()`);
  // Somewhere else, at a default width.
  await enter('124.725');
  await ev(`document.getElementById('freqClose')?.click()`);
  await pickMode('AM');
  const before = await bw();
  ok(JSON.stringify(before) !== '[-3000,3000]', `5. moved away at a different width (${JSON.stringify(before)})`);
  await ev(`document.getElementById('bookmarksBtn').click()`);
  await sleep(300);
  const clicked = await ev(`(() => { const r = [...document.querySelectorAll('#bmList .sres')].find(x => /WEAK AM/.test(x.textContent)); if (r) r.click(); return !!r; })()`);
  await sleep(500);
  ok(clicked, '5. bookmark row found and opened');
  ok(await tunedHz() === 121500000, `5. bookmark tuned 121.500 (${await tunedHz()})`);
  ok(JSON.stringify(await bw()) === '[-3000,3000]', `5. bookmark restored ±3 kHz (${JSON.stringify(await bw())})`);
  const hiVal = await ev(`document.getElementById('bwHi').value`);
  ok(hiVal === '3000', `5. and the slider shows it (${hiVal})`);

  ok(uncaught.length === 0, `no uncaught exception (${uncaught.length})${uncaught.length ? ': ' + uncaught.slice(0, 3).join(' | ') : ''}`);
  try { await send('Browser.close'); } catch (e) { console.error(e); }
} catch (e) {
  console.error(e); fails++;
} finally {
  cleanup();
}
console.log(fails ? `\n${fails} FAILED` : '\nall passed');
setTimeout(() => process.exit(fails ? 1 : 0), 1200);
