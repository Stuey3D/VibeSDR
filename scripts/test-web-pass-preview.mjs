// test-web-pass-preview.mjs — ★★★ beta97 PASS PREVIEW in the web page's SATELLITE box (web/client/src/satPreviewWeb.ts,
// the rules in src/services/satPreview.ts), against the SERVER'S OWN pass track (test/fixtures/sat-preview/passes.json —
// vibeserver/gen-sat-preview-fixture.cpp ran the real engine) and a fake sat_list, inside the BUILT page (web/dist, the
// real CSS variables) in headless Edge with --mute-audio (tests are silent):
//   the ▶ on every future hearable row and the help line; ▶ on the LAST row of a long list → the preview is scrolled into
//   view within 500 ms and the bird starts moving only after it (the beat); the bird moves along the track (the sky plot's
//   dot transform), the readouts change (clock, elevation / azimuth, range, Doppler), MAX is marked (the sky plot and the
//   moments line), the verdict from a history that hears this pass's side of the sky is "good"; ❚❚ pauses (no frames),
//   2× plays to LOS and stops (▶ replays); ✕ restores the live view and NOTHING animates afterwards; ENTER on a focused ▶
//   plays; a live pass on the pick takes priority (the note, no preview); 390 px: no sideways scroll. Screenshots mid-pass.
// Needs web/dist (node scripts/build-web.mjs). Screenshots to $SAT_SHOTS (default $TMPDIR).
//     node scripts/test-web-pass-preview.mjs
import * as esbuild from 'esbuild';
import http from 'node:http';
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const DIST = process.env.WEB_DIST || path.join(root, 'web/dist');
if (!fs.existsSync(EDGE) || !fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.log('   not run — Edge and web/dist needed'); process.exit(3); }
const SHOTS = process.env.SAT_SHOTS || os.tmpdir();
fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const fx = JSON.parse(fs.readFileSync(path.join(root, 'test/fixtures/sat-preview/passes.json'), 'utf8'));
const iss = fx.cases.find((c) => c.name === 'ISS Sao Paulo 56');
const bundle = (await esbuild.build({ stdin: { contents: "export { SatPanel } from './web/client/src/satellites.ts'; export { parseSatList, parseSatTrack } from './src/services/satellites.ts'; export { parseSkyCells, skyCellIndex } from './src/utils/skyPlot.ts'; export { passTrackFrom, passSampleAt } from './src/services/satPreview.ts';", resolveDir: root, loader: 'ts' },
  bundle: true, write: false, format: 'iife', globalName: '__Sat', platform: 'browser', target: 'es2020', logLevel: 'silent' })).outputFiles[0].text;

const port = await freePort();
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.webp': 'image/webp', '.png': 'image/png', '.svg': 'image/svg+xml', '.json': 'application/json' };
const srv = http.createServer((q, r) => {
  const u = decodeURIComponent((q.url || '/').split('?')[0]);
  const f = u === '/' ? path.join(DIST, 'vibesdr.html') : path.join(DIST, path.normalize(u).replace(/^(\.\.[/\\])+/, ''));
  if (!f.startsWith(DIST) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { r.writeHead(404); r.end(); return; }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' }); fs.createReadStream(f).pipe(r);
}).listen(port, '127.0.0.1');

// the clock: 20 minutes before the fixture pass's AOS
const NOW = Math.floor(iss.pass.aos) - 1200;
const trackAos = iss.track.aos, trackLos = iss.track.los;
// a LONG list: 24 birds with passes before the ISS one, so its row is the last
const filler = Array.from({ length: 24 }, (_, k) => ({
  id: `F${k}`, name: `FILLER-${String(k).padStart(2, '0')}`, norad: 90000 + k, note: '', age: 1, ageState: 'fresh', el: -20,
  tx: [{ label: 'FM', hz: 145900000 + k * 1000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' }],
  passes: [{ aos: NOW + 60 + k * 30, tca: NOW + 300 + k * 30, los: NOW + 600 + k * 30, maxEl: 10 + k, aosAz: 0, losAz: 180 }],
}));
const list = (extra = []) => ({
  type: 'sat_list', time: 'ok', located: true, now: NOW,
  source: { orbits: 'Orbits: CelesTrak (celestrak.org) GP data', via: 'vibesdr.net', fetched: NOW - 600, synced: NOW - 600, fetching: false },
  sats: [...filler, ...extra,
    { id: 'ISS', name: 'ISS', norad: 25544, note: '', age: 0.2, ageState: 'fresh', el: -30,
      tx: [{ label: 'Voice', hz: 145800000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' }],
      passes: [{ aos: trackAos, tca: Math.round(iss.pass.tca), los: trackLos, maxEl: iss.pass.maxEl, aosAz: iss.pass.aosAz, losAz: iss.pass.losAz }] }],
});

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-passpv-'));
const dbg = await freePort();
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${dir}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
const deadline = setTimeout(() => { console.log('   FAIL deadline (150 s)'); try { edge.kill('SIGKILL'); } catch {} process.exit(1); }, 150000);
let wsC;
try {
  let tabs = [];
  for (let i = 0; i < 40 && !tabs.some((t) => t.type === 'page'); i++) { await sleep(250); try { tabs = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json(); } catch {} }
  wsC = new WebSocket(tabs.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.exception?.description ?? r.result?.exceptionDetails?.text; };
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, `pass-preview-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 50; i++) { if (await ev(x)) return true; await sleep(50); } return false; };
  const desk = () => send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  const phone = () => send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await send('Page.enable'); await send('Runtime.enable');
  await desk();
  await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'dark' }] });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  ok(await until(`document.readyState === 'complete' && !!document.getElementById('decImgWrap')`, 15000), 'the built page loads');
  await sleep(800);
  await ev(`(() => { const s = document.getElementById('splash'); if (s) s.style.display = 'none'; return true; })()`);
  await ev(`(() => { ${bundle}; window.__Sat = __Sat; return true; })()`);
  // the station's history: this pass's side of the sky heard well, the other side tracked and never decoded
  await ev(`(() => {
    const tr = __Sat.parseSatTrack(${JSON.stringify(iss.track)});
    const pt = __Sat.passTrackFrom(tr, ${JSON.stringify(iss.rx)});
    const mid = __Sat.passSampleAt(pt, pt.tca).az;
    const grid = { elStep: 10, az: [72, 72, 64, 56, 48, 40, 28, 16, 4] };
    const c0 = __Sat.parseSkyCells({ ...grid, c: [] });
    const near = (az) => Math.abs(((az - mid + 540) % 360) - 180) < 100;
    const rows = [];
    for (let b = 0; b < grid.az.length; b++) for (let k = 0; k < grid.az[b]; k++) {
      const az = (k + 0.5) * 360 / grid.az[b], el = (b + 0.5) * 10;
      rows.push([__Sat.skyCellIndex(c0, az, el), near(az) ? 9 : 0, near(az) ? 1 : 6, 120, 15, ${NOW}]);
    }
    window.__cellsJson = JSON.stringify({ v: 1, kind: 'orbcomm', dots: [], count: 0, cells: { ...grid, c: rows } });
    window.__mid = mid;
    return true; })()`);
  await ev(`(() => {
    window.__sent = []; window.__sky = []; window.__fake = ${NOW * 1000};
    window.__p = new __Sat.SatPanel({
      list: () => {}, pick: (norad, tx, doppler) => { window.__sent.push({ op: 'pick', norad, tx }); return true; },
      doppler: () => {}, drop: () => window.__sent.push({ op: 'drop' }),
      rxPos: () => (${JSON.stringify(iss.rx)}), follow: () => {}, left: () => {}, image: () => {},
      // ★ the server answers a track ask for the ISS pass with ITS OWN reply (the fixture); other passes get none
      track: (norad, aos, los) => { window.__sent.push({ track: norad, aos, los });
        if (norad === 25544) setTimeout(() => window.__p.onTrack(__Sat.parseSatTrack(${JSON.stringify(iss.track)})), 30); },
      rxName: () => 'São Paulo', serverKey: () => 'test', connected: () => true,
      now: () => window.__fake,
      skyGet: (p) => { window.__sky.push(p); return Promise.resolve(window.__cellsJson); },
      skyRadio: () => '',
    });
    window.__p.setCap({ ok: true, why: '' });
    window.__p.show();
    window.__p.onList(__Sat.parseSatList(${JSON.stringify(list())}));
    return true; })()`);
  await sleep(400);

  console.log('— the rows');
  const r0 = JSON.parse(await ev(`JSON.stringify({ rows: document.querySelectorAll('#satPanel .sat-row').length, pv: document.querySelectorAll('#satPanel [data-act="pv"]').length,
    hint: document.querySelector('#satPanel [data-s="pvhint"]')?.textContent || '', last: [...document.querySelectorAll('#satPanel .sat-row')].pop()?.textContent || '' })`));
  ok(r0.rows === 25 && r0.pv === 25, `a ▶ on every future hearable row (${r0.pv} of ${r0.rows})`);
  ok(/^▶ on a pass plays it out first/.test(r0.hint), `the help line: "${r0.hint}"`);
  ok(/^ISS/.test(r0.last), 'the ISS pass is the last row of the long list');
  ok(await ev(`[...document.querySelectorAll('#satPanel [data-act="pv"]')].every((b) => b.tabIndex >= 0 && b.getAttribute('aria-label'))`), 'each ▶ is focusable with a label (controller / keyboard)');
  await sleep(1200);
  ok((await ev('window.__sent')).some((m) => m.track && m.track !== 25544), 'the first rows\' tracks are asked (one a second) for their verdicts');
  ok((await ev('window.__sky')).every((p) => /dots=0/.test(p)) && (await ev('window.__sky')).length >= 1, `the history is read as cells only: ${(await ev('window.__sky'))[0]}`);

  console.log('— ▶ on the last row: into view, then play');
  await ev(`(() => { const b = document.querySelector('#satPanel .sat-body'); b.scrollTop = b.scrollHeight; return true; })()`);
  const before = JSON.parse(await ev(`JSON.stringify((() => { const b = document.querySelector('#satPanel .sat-body'), r = b.getBoundingClientRect(), g = document.querySelector('#satPanel .sat-pv canvas').getBoundingClientRect(); return { st: b.scrollTop, inView: !document.querySelector('#satPanel .sat-pv').hidden && g.top >= r.top }; })())`));
  ok(before.st > 200, `scrolled to the foot of the list first (scrollTop ${before.st})`);
  await ev(`(() => { window.__t0 = performance.now(); [...document.querySelectorAll('#satPanel [data-act="pv"]')].pop().click(); return true; })()`);
  const inView = await until(`(() => { const b = document.querySelector('#satPanel .sat-body').getBoundingClientRect(), pv = document.querySelector('#satPanel .sat-pv');
    if (pv.hidden) return false; const g = pv.querySelector('canvas').getBoundingClientRect(), s = pv.querySelector('.sat-pvsky').getBoundingClientRect();
    const inside = (x) => x.height > 20 && x.top >= b.top - 1 && x.bottom <= b.bottom + 1 && x.top >= 0 && x.bottom <= innerHeight;
    window.__tView = performance.now() - window.__t0; return inside(g) && inside(s); })()`, 500);
  const tView = await ev('window.__tView');
  ok(inView && tView <= 500, `the globe and the sky plot are wholly in view within 500 ms (${Math.round(tView)} ms)`);
  ok(await ev(`document.getElementById('satPanel').classList.contains('pv') && getComputedStyle(document.querySelector('#satPanel .sat-box')).display === 'none'`), 'the preview takes the live box\'s place');
  await until('window.__p.pv.state()?.ready === true', 250);   // the server's track reply (30 ms in this harness)
  const armed = JSON.parse(await ev('JSON.stringify(window.__p.pv.state())'));
  const tArmed = await ev('performance.now() - window.__t0');
  ok(armed && armed.ready && !armed.playing && armed.frac === 0, `held at AOS for the beat after the scroll (${Math.round(tArmed)} ms: ${JSON.stringify(armed)})`);
  ok(await until(`window.__p.pv.state()?.playing === true`, 1500), 'then it plays');
  const tPlay = await ev('performance.now() - window.__t0');
  ok(tPlay >= 300, `it started after the scroll had settled (${Math.round(tPlay)} ms after the press)`);
  const read = () => ev(`JSON.stringify({ clock: document.querySelector('#satPanel [data-p="clock"]').textContent, look: document.querySelector('#satPanel [data-p="look"]').textContent,
    range: document.querySelector('#satPanel [data-p="range"]').textContent, dop: document.querySelector('#satPanel [data-p="dop"]').textContent,
    dot: document.querySelector('#satPanel [data-p="dot"]').getAttribute('transform'), st: window.__p.pv.state(), frames: window.__p.pv.frames })`);
  const a = JSON.parse(await read());
  await sleep(1500);
  const b2 = JSON.parse(await read());
  await shot('desktop-mid-pass');
  ok(a.dot !== b2.dot, `the bird moves along the track (sky plot dot ${a.dot} → ${b2.dot})`);
  ok(a.clock !== b2.clock && /UTC · .* local$/.test(b2.clock), `the clock runs in pass time: ${a.clock} → ${b2.clock}`);
  ok(a.look !== b2.look && /^EL \d+° · AZ \d+° [NESW]{1,2}$/.test(b2.look), `elevation / azimuth change: ${a.look} → ${b2.look}`);
  ok(a.range !== b2.range && /km$/.test(b2.range), `range changes: ${a.range} → ${b2.range}`);
  ok(/^Doppler [+−]\d\.\d kHz at 145\.800 MHz$/.test(b2.dop), `the Doppler at the downlink: ${b2.dop}`);
  ok(b2.st.t - a.st.t > 60 && b2.st.t - a.st.t < 200, `≈ a whole pass in ~10 s: ${Math.round(b2.st.t - a.st.t)} s of pass in 1.5 s`);
  ok(b2.frames - a.frames > 30, `drawn at frame rate while playing (${b2.frames - a.frames} frames in 1.5 s)`);
  const marks = JSON.parse(await ev(`JSON.stringify({ max: document.querySelector('#satPanel [data-p="max"]')?.textContent || '', mom: document.querySelector('#satPanel [data-p="moments"]').textContent,
    strip: document.querySelector('#satPanel [data-p="strip"]').textContent, v: document.querySelector('#satPanel [data-p="verdict"]').textContent, g: document.querySelector('#satPanel [data-p="verdict"]').dataset.grade,
    src: document.querySelector('#satPanel [data-p="src"]').textContent, heat: !!document.querySelector('#satPanel [data-p="heat"]'),
    runs: [...document.querySelectorAll('#satPanel .sat-pvsky [data-grade]')].map((p) => p.dataset.grade) })`));
  ok(marks.max === 'MAX 57°' || marks.max === 'MAX 56°', `MAX marked on the sky plot: "${marks.max}"`);
  ok(/^AOS \d\d:\d\d [NESW]{1,2} · MAX 5[67]° \d\d:\d\d · LOS \d\d:\d\d [NESW]{1,2} · 1?\d:\d\d$/.test(marks.mom), `the moments: "${marks.mom}"`);
  ok(/^▶ PREVIEW ISS · \d\d:\d\d UTC$/.test(marks.strip), `the strip by the globe: "${marks.strip}"`);
  ok(marks.g === 'good' || /^Mixed: good/.test(marks.v), `a pass over the heard side of the sky: "${marks.v}" (${marks.g})`);
  ok(marks.heat && marks.runs.includes('good'), `the history under the sky plot, the route coloured by it (${[...new Set(marks.runs)].join(', ')})`);
  ok(/ORBCOMM reception history/.test(marks.src), `which history: "${marks.src}"`);
  const rowV = await ev(`[...document.querySelectorAll('#satPanel .sat-vd')].map((e) => e.textContent).join(' | ')`);
  ok(/max 5[67]°/.test(rowV), `the verdict on the ISS row too: "${rowV.slice(0, 120)}"`);
  ok(await ev(`(() => { const c = document.querySelector('#satPanel .sat-pv canvas'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let g = 0; for (let i = 0; i < d.length; i += 4) if (d[i+3] > 200 && Math.abs(d[i]-60) < 20 && Math.abs(d[i+1]-200) < 25 && Math.abs(d[i+2]-80) < 25) g++; return g > 80; })()`), 'the trail on the globe is green where this station hears');

  console.log('— pause, speed, LOS, replay');
  await ev(`document.querySelector('#satPanel [data-act="pvplay"]').click(); true`);
  const f0 = await ev('window.__p.pv.frames'); await sleep(600); const f1 = await ev('window.__p.pv.frames');
  ok(f1 === f0 && (await ev(`document.querySelector('#satPanel [data-act="pvplay"]').textContent`)) === '▶', `❚❚ pauses: no frames (${f0} → ${f1})`);
  await ev(`document.querySelector('#satPanel [data-act="pvspeed"][data-v="2"]').click(); document.querySelector('#satPanel [data-act="pvplay"]').click(); true`);
  ok(await until(`window.__p.pv.state()?.ended === true`, 8000), '2× plays on to LOS and stops');
  const f2 = await ev('window.__p.pv.frames'); await sleep(500);
  ok((await ev('window.__p.pv.frames')) === f2 && (await ev(`document.querySelector('#satPanel [data-act="pvplay"]').getAttribute('aria-label')`)) === 'Play the pass again', 'at LOS: no frames, ▶ replays');
  await ev(`document.querySelector('#satPanel [data-act="pvplay"]').click(); true`);
  ok(await ev(`window.__p.pv.state().playing && window.__p.pv.state().frac < 0.2`), 'replay from AOS');

  console.log('— ✕ back to the live view');
  await ev(`document.querySelector('#satPanel [data-act="pvx"]').click(); true`);
  const fx0 = await ev('window.__p.pv.frames'); await sleep(800);
  ok(await ev(`document.querySelector('#satPanel .sat-pv').hidden && !document.getElementById('satPanel').classList.contains('pv') && window.__p.pv.state() === null`), '✕ hides the preview and gives the box back');
  ok((await ev('window.__p.pv.frames')) === fx0, 'nothing animates afterwards');

  console.log('— ENTER on a focused ▶ (controller / keyboard)');
  await ev(`(() => { const b = [...document.querySelectorAll('#satPanel [data-act="pv"]')].pop(); b.focus(); return document.activeElement === b; })()`);
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
  ok(await until(`!document.querySelector('#satPanel .sat-pv').hidden`, 800), 'ENTER on the focused ▶ opens the preview');
  ok(await until(`window.__p.pv.state()?.playing === true`, 1500), '…and plays it');
  await phone(); await sleep(500);
  await shot('phone-mid-pass');
  const fit = JSON.parse(await ev(`JSON.stringify({ sw: document.documentElement.scrollWidth, w: innerWidth, pw: document.getElementById('satPanel').scrollWidth, pcw: document.getElementById('satPanel').clientWidth })`));
  ok(fit.sw <= fit.w + 1 && fit.pw <= fit.pcw + 1, `390 px: no sideways scroll (${JSON.stringify(fit)})`);
  await desk(); await sleep(300);
  await ev(`[...document.querySelectorAll('#satPanel [data-act="pv"]')].pop().click(); true`);
  ok(await ev(`document.querySelector('#satPanel .sat-pv').hidden`), 'the row\'s ▶ again returns to the live view');

  console.log('— a live pass takes priority');
  // pick a bird whose pass is happening now, then ▶
  const liveBird = { id: 'LIVE', name: 'LIVEBIRD', norad: 80001, note: '', age: 1, ageState: 'fresh', el: 20,
    tx: [{ label: 'FM', hz: 145990000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' }], passes: [{ aos: NOW - 60, tca: NOW + 100, los: NOW + 400, maxEl: 40, aosAz: 0, losAz: 180 }] };
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(list([liveBird]))})); window.__p.onPicked({ norad: 80001, tx: 0, decoder: '' }); true`);
  await sleep(300);
  await ev(`[...document.querySelectorAll('#satPanel [data-act="pv"]')].pop().click(); true`);
  await sleep(200);
  ok(await ev(`document.querySelector('#satPanel .sat-pv').hidden && !document.querySelector('#satPanel .sat-pvnote').hidden && /^LIVE PASS NOW/.test(document.querySelector('#satPanel .sat-pvnote').textContent)`), 'a pass happening on the pick: no preview, "LIVE PASS NOW" says why');
  ok(await ev(`getComputedStyle(document.querySelector('#satPanel .sat-box')).display !== 'none'`), 'the live box stays');
} catch (e) { ok(false, `driver: ${e?.stack || e}`); }
finally {
  clearTimeout(deadline);
  try { wsC?.close(); } catch {}
  try { edge.kill(); } catch {}
  srv.close();
  await sleep(600);
  try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {}
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed · screenshots: ${SHOTS}/pass-preview-*.png`);
process.exit(fail ? 1 : 0);
