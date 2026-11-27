// test-web-orbcomm-follow.mjs — ★★★ beta94 THE BOX FOLLOWS THE DECODER, the web window (web/client/src/satellites.ts; the rule is
// src/services/orbcomm.ts orbFollowChoice). Stuart 11-25: "i left an expired pass open and the next pass started decoding as
// expected but when i selected the correct pass to see the half globe correctly it reset the pass that was decoding."
// A FAKE sat_list and orbcomm_status (no server, no network) into the SatPanel on a bare page, headless Edge with --mute-audio
// (tests are silent):
//   1. ORBCOMM FM117 picked, its pass not over: the box is FM117's.
//   2. the server says the pass is over and the decoder hears FM115: the box follows FM115 — its name, "following ORBCOMM FM115
//      — now decoding", FM115's track asked for and accepted, the half-globe drawn — and NO pick is sent.
//   3. nothing decoding: the next ORBCOMM bird to rise (FM119, before FM118 and never the ISS).
//   4. FM115 picked from the list ("picked" for an ORBCOMM bird while the ORBCOMM box runs): the decoder's birds, packets and
//      sky dots stay (the server kept the decoder), the view is not zoomed again; a non-ORBCOMM pick clears them.
//   5. ★★ b97 an unnamed carrier on FM116's predicted channel: the card says "probably FM116?" in amber; named, no guess.
// Screenshots to $SAT_SHOTS (default $TMPDIR).     node scripts/test-web-orbcomm-follow.mjs
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
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
const SHOTS = process.env.SAT_SHOTS || os.tmpdir();
fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const bundle = (await esbuild.build({ stdin: { contents: "export { SatPanel } from './web/client/src/satellites.ts'; export { parseSatList, parseSatPicked, parseSatTrack } from './src/services/satellites.ts';", resolveDir: root, loader: 'ts' },
  bundle: true, write: false, format: 'iife', globalName: '__Sat', platform: 'browser', target: 'es2020', logLevel: 'silent' })).outputFiles[0].text;

// a bare page: the window brings its own CSS; #decImgWrap is the decoder canvas it may borrow
const PAGE = '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width"></head>'
  + '<body style="background:#111;color:#ddd;margin:0"><div><div id="decImgWrap"><canvas id="decImage"></canvas></div></div></body></html>';
const port = await freePort();
const srv = http.createServer((q, r) => { r.writeHead(200, { 'content-type': 'text/html' }); r.end(PAGE); }).listen(port, '127.0.0.1');

const now = Math.floor(Date.now() / 1000);
const orbTx = [{ label: 'ORBCOMM', hz: 137200000, hiHz: 137800000, mode: 'orbcomm', loop: 'none', reach: true, use: 'packet', decoder: 'orbcomm' }];
const P = (aos, los, maxEl = 30) => ({ aos, tca: (aos + los) / 2, los, maxEl, aosAz: 10, losAz: 190 });
const A = 41188, B = 41186, C = 41190, D = 41191;
const list = {
  type: 'sat_list', time: 'ok', located: true, now,
  source: { orbits: 'Orbits: CelesTrak (celestrak.org) GP data', via: 'vibesdr.net', fetched: now - 7200, synced: now - 7200, fetching: false },
  sats: [
    { id: 'ORBCOMM-FM117', name: 'ORBCOMM FM117', norad: A, note: '', age: 1, ageState: 'fresh', el: -20, az: 300, rangeKm: 5000, lat: 30, lon: -40, tx: orbTx, passes: [P(now + 5400, now + 6000)] },
    { id: 'ORBCOMM-FM115', name: 'ORBCOMM FM115', norad: B, note: '', age: 1, ageState: 'fresh', el: 25, az: 140, rangeKm: 1600, lat: 45, lon: 8, inRangeFor: 420, tx: orbTx, passes: [P(now - 180, now + 420, 41)] },
    { id: 'ORBCOMM-FM118', name: 'ORBCOMM FM118', norad: C, note: '', age: 1, ageState: 'fresh', el: -3, az: 20, rangeKm: 3500, lat: 68, lon: 5, tx: orbTx, passes: [P(now + 240, now + 800)] },
    { id: 'ORBCOMM-FM119', name: 'ORBCOMM FM119', norad: D, note: '', age: 1, ageState: 'fresh', el: -2, az: 220, rangeKm: 3400, lat: 38, lon: -20, tx: orbTx, passes: [P(now + 120, now + 700)] },
    { id: 'ISS', name: 'ISS', norad: 25544, note: '', age: 0.2, ageState: 'fresh', el: -1, az: 200, lat: 40, lon: -10,
      tx: [{ label: 'FM repeater', hz: 437800000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' }], passes: [P(now + 30, now + 600)] },
  ],
};
const bird = (norad, name) => ({ norad, how: 'position', carrier: 0, rf: 137287500, snr: 11, dop: -1200, pred: -1180, az: 140, el: 25, dcn: 5, nominal: 137287500,
  scId: 26, ok: 120, messages: 2, age: 1, name, eph: { lat: 45, lon: 8, altKm: 742, kmS: 7.08, clock: now } });
const status = (o) => ({ t: now, good: 341, bad: 12, messages: 5, dropped: 0, maxChans: 4, located: true, birds: 8, counts: { ephemeris: 12 }, sats: [], carriers: [],
  pick: 0, up: -1, dots: [], ...o });
// a track reply for a bird: a short arc near the bird
const track = (norad, lat, lon) => ({ type: 'sat_track', norad, aos: now - 180, los: now + 420, pts: [[140, 5], [150, 40], [200, 5]],
  geo: Array.from({ length: 30 }, (_, i) => [lat - 15 + i, lon - 10 + i * 0.7, 742]), geoT0: now - 780, geoStep: 60, tca: now + 120 });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-orbfol-'));
const dbg = await freePort();
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${dir}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
const deadline = setTimeout(() => { console.log('   FAIL deadline (90 s)'); try { edge.kill('SIGKILL'); } catch {} process.exit(1); }, 90000);
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
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, `orbfollow-web-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  for (let i = 0; i < 50 && (await ev(`document.readyState`)) !== 'complete'; i++) await sleep(100);
  await ev(`(() => { ${bundle}; window.__Sat = __Sat; return true; })()`);
  await ev(`(() => {
    window.__sent = [];
    window.__p = new __Sat.SatPanel({
      list: () => {}, pick: (norad, tx, doppler) => { window.__sent.push({ op: 'pick', norad, tx }); return true; },
      doppler: () => {}, drop: () => window.__sent.push({ op: 'drop' }), rxPos: () => ({ lat: 52.24, lon: -0.9 }),
      follow: (d) => window.__sent.push({ follow: d }), left: () => {}, image: () => {},
      zoomView: (hz, span) => window.__sent.push({ zoom: [hz, span] }),
      track: (norad, aos, los) => window.__sent.push({ track: norad, aos, los }),
      rxName: () => 'Northampton', serverKey: () => 'test-server', connected: () => true, enter: () => {},
      skyGet: () => Promise.resolve(null),
      satLabels: () => window.__satLabels || [],   // ★★ b97 the server's sat_labels (the card's guesses come from them)
    });
    window.__p.setCap({ ok: true, why: '' });
    window.__p.show();
    window.__p.onList(__Sat.parseSatList(${JSON.stringify(list)}));
    return true; })()`);
  const box = async () => JSON.parse(await ev(`JSON.stringify({ name: document.querySelector('#satPanel [data-s="name"]').textContent, cd: document.querySelector('#satPanel [data-s="cd"]').textContent,
    st: document.querySelector('#satPanel [data-s="state"]').textContent, f: window.__p.followNow() })`));
  const orbStatus = (o) => ev(`window.__p.orbcomm('orbcomm_status', ${JSON.stringify(status(o))}); true`);

  console.log('— 1. FM117 picked, its pass not over');
  await ev(`window.__p.onPicked(__Sat.parseSatPicked({ type: 'sat', state: 'picked', norad: ${A}, tx: 0, decoder: 'orbcomm', view: 700000, viewCentre: 137500000, params: { width: 640000, centre: 137500000 } })); true`);
  await orbStatus({ sats: [bird(B, 'ORBCOMM FM115')], ended: false, follow: 0 });
  await ev(`window.__p.orbcomm('orbcomm_pkt', ${JSON.stringify({ t: now, norad: B, type: 'ephemeris', snr: 11, rf: 137287500, sat: 'ORBCOMM FM115', text: 'position 45.00N 8.00E', hex: '1B 00' })}); true`);
  let b = await box();
  ok(/^ORBCOMM FM117/.test(b.name) && b.f.why === 'pick', `the box is the pick's (${b.name} · ${b.f.why})`);
  ok(/Next pass in/.test(b.cd), `its next pass (${b.cd})`);
  ok((await ev(`JSON.stringify(window.__sent.filter(m => m.track))`)).includes(`"track":${A}`), 'FM117\'s track asked for');

  console.log('— 2. the pass is over; the decoder hears FM115');
  await ev(`window.__sent.length = 0; true`);
  await orbStatus({ sats: [bird(B, 'ORBCOMM FM115')], ended: true, follow: B, followName: 'ORBCOMM FM115' });
  b = await box();
  ok(/^ORBCOMM FM115 · ORBCOMM/.test(b.name), `the box follows FM115 (${b.name})`);
  ok(/^following ORBCOMM FM115 — now decoding/.test(b.st), `the line says so (${b.st})`);
  ok(/^In range for/.test(b.cd), `FM115's own pass: "${b.cd}"`);
  const sent2 = JSON.parse(await ev(`JSON.stringify(window.__sent)`));
  ok(sent2.some((m) => m.track === B), 'FM115\'s track asked for');
  ok(!sent2.some((m) => m.op === 'pick' || m.op === 'drop'), 'no pick, no drop sent: the decoder is untouched');
  await ev(`window.__p.onTrack(__Sat.parseSatTrack(${JSON.stringify(track(B, 45, 8))})); true`);
  await sleep(200);
  const ink = await ev(`(() => { const c = document.querySelector('#satPanel canvas.sat-globe'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 3; i < d.length; i += 4) if (d[i]) n++; return n; })()`);
  ok(ink > 2000, `the half-globe drawn with FM115's track (${ink} px)`);
  ok(/ORBCOMM FM115/.test(await ev(`document.querySelector('#satPanel .sat-orb').textContent`)), 'the decoder\'s birds are still listed');
  await shot('following');

  console.log('— 3. nothing decoding: the next ORBCOMM bird to rise');
  const noB = { ...list, sats: list.sats.filter((s) => s.norad !== B) };
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(noB)})); true`);
  await orbStatus({ sats: [], ended: true, follow: 0 });
  b = await box();
  ok(/^ORBCOMM FM119/.test(b.name) && /^following ORBCOMM FM119 — next to rise/.test(b.st), `FM119 rises first (${b.name} · ${b.st})`);
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(list)})); true`);

  console.log('— 4. FM115 picked from the list: the decoder\'s state stays');
  await orbStatus({ sats: [bird(B, 'ORBCOMM FM115')], ended: true, follow: B, followName: 'ORBCOMM FM115' });
  await ev(`window.__sent.length = 0; document.querySelector('#satPanel details').open = true; [...document.querySelectorAll('#satPanel .sat-row')].find(r => /^ORBCOMM FM115/.test(r.textContent)).click(); true`);
  ok(JSON.stringify(JSON.parse(await ev(`JSON.stringify(window.__sent.filter(m => m.op))`))) === `[{"op":"pick","norad":${B},"tx":0}]`, 'the row sends the ordinary pick (the server keeps its decoder)');
  const before = JSON.parse(await ev(`JSON.stringify({ pk: window.__p.orbNow()?.pkts.length ?? 0, sats: window.__p.orbNow()?.status?.sats.length ?? 0 })`));
  await ev(`window.__p.onPicked(__Sat.parseSatPicked({ type: 'sat', state: 'picked', norad: ${B}, tx: 0, decoder: 'orbcomm', view: 700000, viewCentre: 137500000, params: { width: 640000, centre: 137500000 } })); true`);
  const after = JSON.parse(await ev(`JSON.stringify({ pk: window.__p.orbNow()?.pkts.length ?? 0, sats: window.__p.orbNow()?.status?.sats.length ?? 0 })`));
  ok(before.pk === 1 && after.pk === 1 && after.sats === 1, `the packets and birds stay (${JSON.stringify(before)} → ${JSON.stringify(after)})`);
  ok(!JSON.parse(await ev(`JSON.stringify(window.__sent)`)).some((m) => m.zoom), 'the view is not zoomed again');
  await orbStatus({ sats: [bird(B, 'ORBCOMM FM115')], ended: false, follow: 0 });
  b = await box();
  ok(/^ORBCOMM FM115/.test(b.name) && b.f.why === 'pick' && !/following/.test(b.st), `FM115 is the pick now: no "following" (${b.st})`);
  await shot('picked-fm115');

  /* ★★ b97 CONFIRMED vs GUESSED on the card (src/utils/orbcommView.ts orbLabelPlan — the waterfall's labels read the same):
   *  an unnamed carrier on FM116's PREDICTED channel reads "probably FM116?" in amber; once the decoder names it, the row is
   *  FM116's own and no guess is left. */
  console.log('— 5. b97 an unnamed carrier on a predicted channel: the card guesses, amber; confirmed, it does not');
  await ev(`window.__satLabels = [{ id: 'ORBCOMM-FM116', n: 'ORBCOMM FM116', norad: 41187, tx: 'ORBCOMM', hz: 137800400, r: -20, el: 30, m: 'nfm', b: false }]; true`);
  const unnamed = { norad: 0, how: '', carrier: 2, rf: 137800000, snr: 8, dop: 0, pred: 0, az: 0, el: -90, dcn: 0, nominal: 0, scId: -1, ok: 3, messages: 0, age: 1, name: 'ORBCOMM (not yet named)' };
  await orbStatus({ sats: [bird(B, 'ORBCOMM FM115'), unnamed], carriers: [{ id: 2, rf: 137800000, snr: 8, lock: true, ok: 3, bad: 0, fixed: 0 }], ended: false, follow: 0 });
  const g = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#satPanel .sat-guess')].map(e => ({ t: e.textContent, c: getComputedStyle(e).color })))`));
  ok(g.length === 1 && g[0].t === 'ORBCOMM (not yet named) · not yet named · probably FM116?', `the unnamed row: "${g[0]?.t}"`);
  ok(g[0]?.c === 'rgb(255, 184, 51)', `…in amber (${g[0]?.c})`);
  await shot('guess-fm116');
  await orbStatus({ sats: [bird(B, 'ORBCOMM FM115'), { ...unnamed, norad: 41187, how: 'position', name: 'ORBCOMM FM116' }],
                    carriers: [{ id: 2, rf: 137800000, snr: 8, lock: true, ok: 9, bad: 0, fixed: 0, sat: 'ORBCOMM FM116', norad: 41187, how: 'position' }], ended: false, follow: 0 });
  const g2 = JSON.parse(await ev(`JSON.stringify({ n: document.querySelectorAll('#satPanel .sat-guess').length, t: document.querySelector('#satPanel .sat-orb').textContent })`));
  ok(g2.n === 0 && /ORBCOMM FM116 · named by its own position report/.test(g2.t) && !/probably/.test(g2.t), 'the decoder names that carrier: FM116\'s own row, no guess left');
  await ev(`window.__satLabels = []; true`);
  await ev(`window.__p.onPicked(__Sat.parseSatPicked({ type: 'sat', state: 'picked', norad: 25544, tx: 0, decoder: '' })); true`);
  ok(await ev(`window.__p.orbNow() === null`), 'a non-ORBCOMM pick clears the ORBCOMM state');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(200);
  const sw = await ev(`document.documentElement.scrollWidth <= innerWidth + 1`);
  ok(sw === true, '390 px: no sideways scroll');
} catch (e) { ok(false, `driver: ${e?.stack || e}`); }
finally {
  clearTimeout(deadline);
  try { wsC?.close(); } catch {}
  try { edge.kill(); } catch {}
  srv.close();
  await sleep(600);
  try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {}
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed · screenshots: ${SHOTS}/orbfollow-web-*.png`);
process.exit(fail ? 1 : 0);
