// test-web-sat-single.mjs — ★★★ beta96 SINGLE MODE: SATELLITES, the web window's WATCH view (web/client/src/satellites.ts +
// src/services/satSingle.ts) against fake sat_single / sat_hybrid frames, inside the BUILT page (web/dist), headless Edge with
// --mute-audio: watching hides EXIT (the note in its place) and the all-satellites list, refuses the way out and a pick (in
// words, nothing sent), shows NEXT with its countdown between passes, LIVE during one, the last pass + GALLERY + the
// timetable; the hybrid brings AUTO DOPPLER back and says the window; the admin (setWatch false) gets EXIT back; desktop and
// 390 px, no sideways scroll. Needs web/dist (node scripts/build-web.mjs).   node scripts/test-web-sat-single.mjs
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
const DIST = path.join(root, 'web/dist');
if (!fs.existsSync(EDGE) || !fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.log('   not run — Edge and web/dist needed'); process.exit(3); }
const SHOTS = process.env.SAT_SHOTS || os.tmpdir();
fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// the window, bundled on its own (the shared src/services/satellites.ts inside), exposed as window.__Sat
const bundle = (await esbuild.build({ stdin: { contents: "export { SatPanel } from './web/client/src/satellites.ts'; export { parseSatList } from './src/services/satellites.ts'; export { parseSatSingle, parseSatHybrid } from './src/services/satSingle.ts';", resolveDir: root, loader: 'ts' },
  bundle: true, write: false, format: 'iife', globalName: '__Sat', platform: 'browser', target: 'es2020', logLevel: 'silent' })).outputFiles[0].text;

// the built page, served plain (its sockets find nobody — the page sits at its connect screen, CSS and all)
const port = await freePort();
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.webp': 'image/webp', '.png': 'image/png', '.svg': 'image/svg+xml', '.json': 'application/json' };
const srv = http.createServer((q, r) => {
  const u = decodeURIComponent((q.url || '/').split('?')[0]);
  const f = u === '/' ? path.join(DIST, 'vibesdr.html') : path.join(DIST, path.normalize(u).replace(/^(\.\.[/\\])+/, ''));
  if (!f.startsWith(DIST) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { r.writeHead(404); r.end(); return; }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' }); fs.createReadStream(f).pipe(r);
}).listen(port, '127.0.0.1');

const now = Math.floor(Date.now() / 1000);
const list = (over = {}) => ({
  type: 'sat_list', time: 'ok', located: true, now,
  source: { orbits: 'Orbits: CelesTrak (celestrak.org) GP data', via: 'vibesdr.net', fetched: now - 7200, synced: now - 2 * 3600 - 120, fetching: false },
  sats: [
    { id: 'METEOR-M2-3', name: 'METEOR-M2 3', norad: 57166, note: '', age: 0.4, ageState: 'fresh', el: 31, az: 10, rangeKm: 1450, lat: 58, lon: 3, inRangeFor: 595,
      tx: [{ label: 'LRPT', hz: 137900000, mode: 'lrpt', loop: 'costas', reach: true, use: 'image' }], passes: [{ aos: now - 300, tca: now + 100, los: now + 595, maxEl: 62, aosAz: 0, losAz: 180 }] },
    { id: 'ISS', name: 'ISS', norad: 25544, note: '', age: 0.2, ageState: 'fresh', el: 14, az: 220, rangeKm: 1700, lat: 47, lon: -9, inRangeFor: 372,
      tx: [{ label: 'FM repeater', hz: 437800000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice', upHz: 145990000, ctcss: 67 },
           { label: 'Voice / SSTV', hz: 145800000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice', sstv: true },
           { label: 'APRS', hz: 145825000, mode: 'ax25-1200', loop: 'fll', reach: true, use: 'packet' }],
      passes: [{ aos: now - 200, tca: now + 80, los: now + 372, maxEl: 40, aosAz: 230, losAz: 60 }] },
    { id: 'SO-50', name: 'SO-50', norad: 27607, note: '', age: 1, ageState: 'fresh', el: -9,
      tx: [{ label: 'FM repeater', hz: 436795000, mode: 'nfm', loop: 'fll', reach: false, use: 'voice', why: 'Out of this receiver’s range — it is locked to 130–170 MHz' }],
      passes: [{ aos: now + 38 * 60 + 20, tca: now + 43 * 60, los: now + 48 * 60, maxEl: 56, aosAz: 10, losAz: 170 }] },
    { id: 'SONATE-2', name: 'SONATE-2', norad: 59112, note: '', age: 1, ageState: 'fresh', el: -30,
      tx: [{ label: 'APRS', hz: 145825000, mode: 'ax25-1200', loop: 'fll', reach: true, use: 'packet' }],
      passes: [{ aos: now + 3 * 3600, tca: now + 3 * 3600 + 300, los: now + 3 * 3600 + 600, maxEl: 22, aosAz: 10, losAz: 170 }] },
  ],
  ...over,
});

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-satsingle-'));
const dbg = await freePort();
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${dir}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
const deadline = setTimeout(() => { console.log("   FAIL deadline (150 s)"); try { edge.kill("SIGKILL"); } catch {} process.exit(1); }, 150000);
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
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, `sat-single-web-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 200; i++) { if (await ev(x)) return true; await sleep(200); } return false; };
  const desk = () => send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  const phone = () => send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  const fits = async () => JSON.parse(await ev(`JSON.stringify((() => { const r = document.getElementById('satPanel').getBoundingClientRect(); return { l: Math.round(r.left), r: Math.round(r.right), t: Math.round(r.top), b: Math.round(r.bottom), w: innerWidth, h: innerHeight, sw: document.documentElement.scrollWidth, pw: document.getElementById('satPanel').scrollWidth, pcw: document.getElementById('satPanel').clientWidth }; })())`));
  const fitOk = (f) => f.l >= 0 && f.r <= f.w && f.t >= 0 && f.b <= f.h && f.sw <= f.w + 1 && f.pw <= f.pcw + 1;
  await send('Page.enable'); await send('Runtime.enable');
  await desk();
  await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'dark' }] });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  ok(await until(`document.readyState === 'complete' && !!document.getElementById('decImgWrap')`, 15000), 'the built page loads (with its #decImgWrap)');
  await sleep(800);
  // ★ quiet the page under the window (its connect screen) so the screenshots show the window over the app's own backdrop
  await ev(`(() => { const s = document.getElementById('splash'); if (s) s.style.display = 'none'; return true; })()`);   // the connect screen sits on top
  await ev(`(() => { ${bundle}; window.__Sat = __Sat; return true; })()`);
  await ev(`(() => {
    window.__sent = []; window.__lists = 0; window.__left = 0; window.__conn = true; window.__entered = 0; window.__stations = 0; window.__fake = null;
    const home = document.getElementById('decImgWrap').parentNode;
    window.__home = home;
    window.__p = new __Sat.SatPanel({
      list: () => { window.__lists++; },
      pick: (norad, tx, doppler) => { window.__sent.push({ op: 'pick', norad, tx, doppler }); return true; },
      doppler: (on) => window.__sent.push({ op: 'doppler', on }),
      drop: () => window.__sent.push({ op: 'drop' }),
      rxPos: () => ({ lat: 52.24, lon: -0.9 }),
      follow: (d) => window.__sent.push({ follow: d }),
      left: () => { window.__left++; },
      image: (slot) => { const w = document.getElementById('decImgWrap'); if (slot) { slot.appendChild(w); document.getElementById('decImage').classList.add('on'); } else home.appendChild(w); },
      track: (norad, aos, los) => window.__sent.push({ track: norad, aos, los }),
      rxName: () => 'Northampton',
      serverKey: () => 'test-server',
      connected: () => window.__conn,
      enter: () => { window.__entered++; window.__p.show(); },
      mapStations: () => window.__stations,
      openMap: (o) => window.__sent.push({ map: o }),
      now: () => window.__fake ?? Date.now(),
      galleryGet: (p) => (window.__galGet ? window.__galGet(p) : Promise.resolve(null)),   // ★★ beta91 the saved passes
      vfobOpen: (norad, tx) => window.__sent.push({ vfob: [norad, tx] }),                 // ★★★ beta92 OPEN IN VFO B
      openGallery: () => window.__sent.push({ gallery: 1 }),                                 // ★★★ beta96 the watch view's GALLERY
    });
    window.__p.setCap({ ok: true, why: '' });
    return true; })()`);

  const now = Math.floor(Date.now() / 1000);
  const P = (norad, name, label, aosS, durS, maxEl, decoder) => ({ norad, tx: 0, name, label, aos: now + aosS, los: now + aosS + durS, maxEl, ...(decoder ? { decoder } : {}) });
  const q = (sel) => `document.querySelector('#satPanel ${sel}')`;
  console.log('— watch on, between passes');
  await ev(`window.__p.show(); window.__p.onList(__Sat.parseSatList(${JSON.stringify(list())})); window.__p.setWatch(true); true`);
  await ev(`window.__p.onSingle(__Sat.parseSatSingle(${JSON.stringify({ type: 'sat_single', on: true, t: now, last: 'Meteor-M2-3 LRPT, max 41° — ended (LOS)',
    plan: [P(44909, 'RS-44', 'Transponder', 300, 1200, 46), P(57166, 'Meteor-M2-3', 'LRPT', 3600, 840, 62)] })})); true`);
  const w0 = JSON.parse(await ev(`JSON.stringify({ exit: ${q('[data-act="exit"]')}.hidden, only: !${q('[data-s="only"]')}.hidden, onlyT: ${q('[data-s="only"]')}.textContent,
    list: ${q('[data-s="list"]')}.hidden, dop: ${q('[data-act="doppler"]')}.hidden, watch: !${q('[data-s="watch"]')}.hidden, txt: ${q('[data-s="watch"]')}.textContent })`));
  ok(w0.exit && w0.only && w0.onlyT === 'SATELLITES only on this receiver', `EXIT hidden, the note in its place ("${w0.onlyT}")`);
  ok(w0.list && w0.dop, 'the all-satellites list and AUTO DOPPLER hidden (the radio makes the picks)');
  ok(w0.watch && /^NEXT · RS-44 · Transponder · AOS \d\d:\d\d \(in 0:0[45]\) · max 46°/.test(w0.txt), `NEXT with its countdown: "${w0.txt.slice(0, 70)}"`);
  ok(/Between passes/.test(w0.txt) && /Last pass: Meteor-M2-3 LRPT, max 41° — ended \(LOS\)/.test(w0.txt), 'between passes: the line and the last pass');
  ok(/THIS RECEIVER'S PASSES/.test(w0.txt) && /RS-44 · Transponder · max 46°/.test(w0.txt) && /Meteor-M2-3 · LRPT · max 62°/.test(w0.txt), 'the radio\'s timetable');
  await shot('between');
  await ev(`${q('[data-act="gallery"]')}.click(); true`);
  ok((await ev('window.__sent')).some((m) => m.gallery), 'GALLERY opens the gallery');
  await ev(`window.__p.exit(); true`);
  ok(await ev(`window.__p.active && !document.getElementById('satPanel').hidden`) === true, 'the way out is refused (the mode stays)');
  ok(/follows the satellite passes itself/.test(await ev(`${q('[data-s="why"]')}.textContent`)), '…and says why');
  ok(!(await ev('window.__sent')).some((m) => m.op === 'drop'), '…nothing sent (no drop)');
  await phone(); await sleep(300); { const f = await fits(); ok(fitOk(f), `390 px: fits, no sideways scroll (${JSON.stringify(f)})`); } await shot('between-phone'); await desk();

  console.log('— a pass running (Meteor, the picture)');
  await ev(`window.__p.onSingle(__Sat.parseSatSingle(${JSON.stringify({ type: 'sat_single', on: true, t: now, now: P(57166, 'Meteor-M2-3', 'LRPT', -120, 840, 62, 'lrpt'), plan: [P(57166, 'Meteor-M2-3', 'LRPT', -120, 840, 62)] })})); true`);
  await ev(`window.__p.onPicked({ norad: 57166, tx: 0, decoder: 'lrpt', shared: true }); true`);
  const w1 = await ev(`${q('[data-s="watch"]')}.textContent`);
  ok(/^LIVE · Meteor-M2-3 · LRPT · 1[0-2]:\d\d left/.test(w1) && /picture builds below/.test(w1), `LIVE with the time left and what to watch: "${w1.slice(0, 90)}"`);
  ok(await ev(`!${q('[data-s="box"]')}.hidden`) === true && (await ev('window.__sent')).some((m) => m.follow === 'lrpt'), 'the room\'s pick: the box shows it and follows its decoder');
  ok(await ev(`${q('.sat-wp.live')}?.textContent || ''`) !== '', 'the running pass marked in the timetable');
  await shot('live');

  console.log('— the hybrid (a linear transponder pass)');
  const H = { type: 'sat_hybrid', on: true, norad: 44909, tx: 0, name: 'RS-44', label: 'Transponder', mode: 'usb', lo: 435595737, hi: 435684263, passLo: 435610000, passHi: 435670000, centre: 435640000, los: now + 900 };
  await ev(`window.__p.onHybrid(__Sat.parseSatHybrid(${JSON.stringify(H)})); true`);
  const w2 = JSON.parse(await ev(`JSON.stringify({ dop: ${q('[data-act="doppler"]')}.hidden, txt: ${q('[data-s="watch"]')}.textContent })`));
  ok(!w2.dop, 'AUTO DOPPLER back (the listener\'s own VFO)');
  ok(/Your own VFO on RS-44 · Transponder: tune anywhere in 435\.596–435\.684 MHz, SSB or CW, with your own AUTO DOPPLER · 1[45]:\d\d to LOS\./.test(w2.txt), `the window in words: "${(w2.txt.match(/Your own VFO[^.]*\./) || [''])[0]}"`);
  ok(await ev(`window.__p.hybridNow()?.centre`) === 435640000, 'the hybrid is the panel\'s (main.ts reads it)');
  await ev(`window.__p.onHybrid(null); true`);
  ok(await ev(`${q('[data-act="doppler"]')}.hidden`) === true, 'LOS: AUTO DOPPLER hidden again');

  console.log('— the admin');
  await ev(`window.__p.setWatch(false); true`);
  const w3 = JSON.parse(await ev(`JSON.stringify({ exit: ${q('[data-act="exit"]')}.hidden, only: ${q('[data-s="only"]')}.hidden, list: ${q('[data-s="list"]')}.hidden, watch: ${q('[data-s="watch"]')}.hidden })`));
  ok(!w3.exit && w3.only && !w3.list && w3.watch, `not watching: EXIT, the list back, no watch block (${JSON.stringify(w3)})`);
  await ev(`window.__p.exit(); true`);
  ok(await ev(`window.__p.active`) === false && (await ev('window.__sent')).some((m) => m.op === 'drop'), 'the admin may leave (drop sent)');
} catch (e) { ok(false, `driver: ${e?.stack || e}`); }
finally {
  clearTimeout(deadline);
  try { wsC?.close(); } catch {}
  try { edge.kill(); } catch {}
  srv.close();
  await sleep(600);
  try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {}
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed · screenshots: ${SHOTS}/sat-single-web-*.png`);
process.exit(fail ? 1 : 0);
