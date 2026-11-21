// test-web-satellite-mode.mjs — ★★★ V12 SATELLITE MODE, the web window (web/client/src/satellites.ts) against a FAKE sat_list
// (the contract in docs/v12/SATELLITE-MODE.md — no server, no network), inside the BUILT page (web/dist, so the real CSS
// variables and the real #decImgWrap image canvas), in headless Edge with --mute-audio (tests are silent):
//   the timetable's order (up now shortest-remaining first, then AOS) and its words, greyed rows (why, no pick sent), the
//   synced line (synced / fetching — re-asked every 3 s — / none + why), a pick → {"op":"pick",doppler:true}, "picked"
//   → the COMPACT box (corrected frequency, "In range for", the half-globe drawn), AUTO DOPPLER → {"op":"doppler"},
//   METEOR (image) → the BIG box with the decoder image canvas lent into it, ISS 145.800 (sstv) compact → big on a
//   picture, a packet bird's APRS lines, EXIT → {"op":"drop"} and the canvas home again; desktop and 390 px, dark and light,
//   no sideways scroll. Needs web/dist (node scripts/build-web.mjs). Screenshots to $SAT_SHOTS (default $TMPDIR).
//     node scripts/test-web-satellite-mode.mjs
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
const bundle = (await esbuild.build({ stdin: { contents: "export { SatPanel } from './web/client/src/satellites.ts'; export { parseSatList, parseSatTelemetry, parseSatTrack } from './src/services/satellites.ts';", resolveDir: root, loader: 'ts' },
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
      tx: [{ label: 'FM repeater', hz: 437800000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' },
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

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-satmode-'));
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
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, `sat-web-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
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
    });
    window.__p.setCap({ ok: true, why: '' });
    return true; })()`);
  // a picture in the decoder canvas, as the LRPT preview would draw it
  await ev(`(() => { const c = document.getElementById('decImage'); c.width = 392; c.height = 300; const x = c.getContext('2d');
    const g = x.createLinearGradient(0, 0, 392, 300); g.addColorStop(0, '#1d4f8a'); g.addColorStop(.5, '#d8e4ee'); g.addColorStop(1, '#3b6b2e');
    x.fillStyle = g; x.fillRect(0, 0, 392, 300); return true; })()`);

  console.log('— fetching, then the list');
  await ev(`window.__p.show(); true`);
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(list({ sats: [], source: { synced: 0, fetching: true } }))})); true`);
  ok(await ev(`!document.getElementById('satPanel').hidden`) === true, 'the window opens with the mode');
  ok(/Fetching orbit data…/.test(await ev(`document.querySelector('#satPanel .sat-sync').textContent`)), 'source.fetching → "Fetching orbit data…"');
  const l0 = await ev('window.__lists');
  await sleep(3600);
  ok(await ev('window.__lists') > l0, `re-asked every 3 s while fetching (${l0} → ${await ev('window.__lists')})`);
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(list({ sats: [], why: 'vibesdr.net did not answer — trying again in 15 min', source: { synced: 0, fetching: false } }))})); true`);
  ok(/^No orbit data — vibesdr\.net did not answer/.test(await ev(`document.querySelector('#satPanel .sat-sync').textContent`)), 'none → "No orbit data — why"');
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(list())})); true`);
  const sync = await ev(`document.querySelector('#satPanel .sat-sync').textContent`);
  ok(/^Orbits synced \d\d:\d\d · 2 h ago$/.test(sync), `the synced line: "${sync}"`);
  const rows = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#satPanel .sat-row')].map(b => ({ t: b.textContent, grey: b.classList.contains('grey'), dis: b.getAttribute('aria-disabled') })))`));
  ok(rows.length === 6, `every downlink listed (${rows.length})`);
  ok(/^ISS · FM repeater/.test(rows[0].t) && /^METEOR-M2 3/.test(rows[3].t) && /^SO-50/.test(rows[4].t) && /^SONATE-2/.test(rows[5].t),
     `up now SHORTEST remaining first (ISS 6:12 before METEOR 9:55), then AOS: ${rows.map((r) => r.t.split('·')[0].trim()).join(' / ')}`);
  ok(/UP · 6:1\d left/.test(rows[0].t) && /UP · 9:5\d left/.test(rows[3].t), `"UP · 6:12 left": ${rows[0].t.match(/UP · [\d:]+ left/)}`);
  ok(/AOS \d\d:\d\d \(in 0:38\)/.test(rows[4].t), `"AOS 16:42 (in 0:38)": ${rows[4].t.match(/AOS .*?\)/)}`);
  ok(rows[4].grey && rows[4].dis === 'true' && /locked to 130–170 MHz/.test(rows[4].t), 'out of reach: GREYED with its why');
  ok(rows.filter((r) => r.grey).length === 1, 'only that one greyed');
  ok(/Orbits: CelesTrak · 2 h old/.test(await ev(`document.querySelector('#satPanel .sat-credit').textContent`)), 'the CelesTrak credit stays');
  await shot('list-desktop');
  await ev(`document.querySelectorAll('#satPanel .sat-row')[4].click()`);
  ok(JSON.stringify(await ev('window.__sent')) === '[]', 'a greyed row sends nothing…');
  ok(/locked to 130–170 MHz/.test(await ev(`document.querySelector('#satPanel .sat-why').textContent`)), '…and says why');

  console.log('— a voice pick: the compact box');
  await ev(`document.querySelectorAll('#satPanel .sat-row')[0].click()`);
  ok(JSON.stringify((await ev('window.__sent'))[0]) === '{"op":"pick","norad":25544,"tx":0,"doppler":true}', `op pick with AUTO DOPPLER on: ${JSON.stringify((await ev('window.__sent'))[0])}`);
  ok(/Tuning…/.test(await ev(`document.querySelector('#satPanel .sat-box').textContent`)), 'pending: "Tuning…"');
  await ev(`window.__p.onPicked({ norad: 25544, tx: 0, decoder: '' }); window.__p.onReadout({ state: 'TRACK', name: 'ISS', norad: 25544, nominal: 437800000, offset: -8123, rate: -150, time: 'ok' }); true`);
  const box = JSON.parse(await ev(`JSON.stringify({ big: document.getElementById('satPanel').classList.contains('big'), hz: document.querySelector('#satPanel [data-s="hz"]').textContent, cd: document.querySelector('#satPanel [data-s="cd"]').textContent, img: document.querySelector('#satPanel .sat-img').hidden, st: document.querySelector('#satPanel [data-s="state"]').textContent, list: document.querySelector('#satPanel details').open })`));
  ok(!box.big && box.img, 'voice → COMPACT (no picture area)');
  ok(box.hz === '437.791 88 MHz', `the corrected frequency (437.800 − 8.123 kHz): ${box.hz}`);
  ok(/^In range for 6:\d\d$/.test(box.cd), `the countdown: ${box.cd}`);
  ok(/^TRACK/.test(box.st), `the lock's word: ${box.st}`);
  ok(box.list === false, 'the timetable folds away under the pick');
  const ink = await ev(`(() => { const c = document.querySelector('#satPanel canvas.sat-globe'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 3; i < d.length; i += 4) if (d[i]) n++; return n; })()`);
  ok(ink > 2000, `the half-globe is drawn (${ink} px inked)`);
  ok((await ev('window.__sent')).some((m) => m.track === 25544), '1.7 the pass\'s path is asked once for the pick');
  ok((await ev('window.__sent')).filter((m) => m.track === 25544).length === 1, '…once (not every second)');
  // ★ 1.7 a path over the dome: the dotted orbit adds ink
  const geo = Array.from({ length: 80 }, (_, k) => [62 - k * 0.5, -25 + k * 0.45, 420]);
  await ev(`window.__p.onTrack(__Sat.parseSatTrack(${JSON.stringify({ type: 'sat_track', norad: 25544, aos: now - 200, los: now + 372, pts: [], geo })})); true`);
  const ink2 = await ev(`(() => { const c = document.querySelector('#satPanel canvas.sat-globe'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 3; i < d.length; i += 4) if (d[i]) n++; return n; })()`);
  ok(ink2 > ink + 150, `1.7 the dotted orbit is inked (${ink} → ${ink2} px)`);
  // ★ 1.8 the receiver's dot is GREEN (#5BE36B) somewhere on the canvas
  const green = await ev(`(() => { const c = document.querySelector('#satPanel canvas.sat-globe'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i+3] > 200 && Math.abs(d[i]-0x5B) < 12 && Math.abs(d[i+1]-0xE3) < 12 && Math.abs(d[i+2]-0x6B) < 12) n++; return n; })()`);
  ok(green > 60, `1.8 the receiver's dot is green (${green} px of #5BE36B)`);
  ok(await ev(`document.querySelectorAll('#satPanel .sat-rem').length === 0 && !document.querySelector('#satPanel .sat-lead')`), '1.6 an older server (no pickPrivate): no REMIND key anywhere');
  ok(await ev(`document.querySelector('#satPanel [data-act="map"]').hidden`), '1.9 a voice pick: no MAP key');
  await shot('compact-desktop');
  await ev(`document.querySelector('#satPanel [data-act="doppler"]').click()`);
  ok(JSON.stringify((await ev('window.__sent')).slice(-1)[0]) === '{"op":"doppler","on":false}', 'AUTO DOPPLER off → op doppler false');
  ok(/DOPPLER OFF/.test(await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`)) && await ev(`!document.querySelector('#satPanel [data-act="doppler"]').classList.contains('on')`), 'the key unlit, the box says so');
  await ev(`document.querySelector('#satPanel [data-act="doppler"]').click()`);

  console.log('— an image pick: the big box');
  await ev(`document.querySelector('#satPanel details').open = true; true`);
  await ev(`[...document.querySelectorAll('#satPanel .sat-row')].find(b => /^METEOR/.test(b.textContent)).click()`);
  ok(JSON.stringify((await ev('window.__sent')).slice(-1)[0]) === '{"op":"pick","norad":57166,"tx":0,"doppler":true}', 'METEOR picked (doppler on again)');
  await ev(`window.__p.onPicked({ norad: 57166, tx: 0, decoder: 'lrpt' }); window.__p.status('METEOR · locked · 120 lines · 3 channels'); true`);
  ok((await ev('window.__sent')).slice(-3).some((m) => JSON.stringify(m) === '{"follow":"lrpt"}'), 'the page follows the server\'s decoder (lrpt)');
  ok((await ev('window.__sent')).slice(-1)[0].track === 57166, '…and asks for its pass\'s path (1.7)');
  // ★★ 11-21 (Stuart: the box took too much room) — an image pick opens SMALL, like any decoder box; the size key or the
  //    first picture makes it BIG
  ok(await ev(`!document.getElementById('satPanel').classList.contains('big') && document.querySelector('#satPanel .sat-img').hidden`), 'image pick → SMALL until a picture (no empty picture area)');
  ok(/it opens BIG/.test(await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`)) || /120 lines/.test(await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`)), 'says where the picture will be');
  await ev(`window.__p.pictureStart(); true`);
  const big = JSON.parse(await ev(`JSON.stringify({ big: document.getElementById('satPanel').classList.contains('big'), img: !document.querySelector('#satPanel .sat-img').hidden, lent: !!document.querySelector('#satPanel .sat-img #decImage'), w: document.getElementById('satPanel').getBoundingClientRect().width, ih: document.querySelector('#satPanel #decImage').getBoundingClientRect().height })`));
  ok(big.big && big.img && big.lent, 'a picture starts → BIG, the decoder image canvas lent into it');
  ok(big.w > 600 && big.ih > 150, `bigger: ${Math.round(big.w)} px wide, picture ${Math.round(big.ih)} px tall`);
  ok(/shaded: where it can be heard/.test(await ev(`document.querySelector('#satPanel [data-s="key"]').textContent`)) && await ev(`!document.querySelector('#satPanel [data-s="key"]').hidden`), '1.8 BIG: the one-line key under the globe');
  ok(/120 lines/.test(await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`)), 'the LRPT line in the box');
  await shot('big-desktop');
  await ev(`document.querySelector('#satPanel [data-act="size"]').click(); true`);
  ok(await ev(`!document.getElementById('satPanel').classList.contains('big') && document.querySelector('#satPanel .sat-img').hidden`), 'the size key → SMALL (the picture keeps drawing, out of sight)');
  await ev(`document.querySelector('#satPanel [data-act="size"]').click(); true`);
  ok(await ev(`document.getElementById('satPanel').classList.contains('big') && !document.querySelector('#satPanel .sat-img').hidden`), '…and BIG again');
  await ev(`document.querySelector('#satPanel [data-act="size"]').click(); true`);   // SMALL by hand: the next pick opens SMALL
  await ev(`window.__p.pictureEnd(); true`);
  ok(/picture complete — saved to the gallery/.test(await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`)), 'pass end: the finished picture stays, "saved to the gallery"');

  console.log('— ISS 145.800 (sstv): compact, big on a picture');
  await ev(`document.querySelector('#satPanel details').open = true; true`);
  await ev(`[...document.querySelectorAll('#satPanel .sat-row')].find(b => /Voice \\/ SSTV/.test(b.textContent)).click(); window.__p.onPicked({ norad: 25544, tx: 1, decoder: 'sstv' }); true`);
  ok(await ev(`!document.getElementById('satPanel').classList.contains('big') && document.querySelector('#satPanel .sat-img').hidden && !document.querySelector('#satPanel .sat-img #decImage')`), 'opens compact, the canvas back home');
  ok(await ev(`document.getElementById('decImgWrap').parentNode === window.__home`), '(#decImgWrap returned to the decoder box)');
  ok(/an SSTV picture opens here/.test(await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`)), 'says a picture will open here');
  await ev(`window.__p.pictureStart(); true`);
  ok(await ev(`document.getElementById('satPanel').classList.contains('big') && !!document.querySelector('#satPanel .sat-img #decImage')`), 'an SSTV picture starts → BIG');

  console.log('— a packet pick: the APRS lines');
  await ev(`document.querySelector('#satPanel details').open = true; true`);
  await ev(`[...document.querySelectorAll('#satPanel .sat-row')].find(b => /^ISS · APRS/.test(b.textContent)).click(); window.__p.onPicked({ norad: 25544, tx: 2, decoder: 'aprs' }); true`);
  ok(await ev(`!document.getElementById('satPanel').classList.contains('big') && !document.querySelector('#satPanel .sat-pk').hidden`), 'packet → COMPACT with its lines area');
  await ev(`window.__p.packet('RS0ISS>APRS: ARISS digipeater'); window.__p.packet('M0ABC-7>APRS,RS0ISS*: !5214.40N/00054.00W-'); true`);
  ok(/M0ABC-7.*\n?.*RS0ISS>APRS/s.test(await ev(`document.querySelector('#satPanel .sat-pk').innerText`)), 'the decoded lines, newest first');
  ok(await ev(`document.querySelector('#satPanel [data-act="map"]').hidden`), '1.9 no station with a position yet: no MAP key (no dead key)');
  await ev(`window.__stations = 1; window.__p.stationsChanged(); true`);
  ok(await ev(`!document.querySelector('#satPanel [data-act="map"]').hidden`), '1.9 a station with a position heard: MAP in the header');
  await ev(`document.querySelector('#satPanel [data-act="map"]').click(); true`);
  const mp = (await ev('window.__sent')).slice(-1)[0].map;
  ok(mp && mp.layer === 'aprs' && mp.overlay?.label === 'ISS PASS' && mp.overlay.circles?.[0].lat === 47 && mp.overlay.points?.[0].label === 'ISS',
     `1.9 MAP → the DIGI MODES map's APRS layer with the pass overlay: footprint + the bird (${JSON.stringify(mp && { layer: mp.layer, label: mp.overlay?.label })})`);
  ok(mp.overlay.lines === undefined || Array.isArray(mp.overlay.lines), '…its orbit once the path has come');
  await ev(`window.__stations = 0; true`);
  await shot('packet-desktop');

  console.log('— phone width, light');
  await phone(); await sleep(500);
  let f = await fits();
  ok(fitOk(f), `390 px packet box: fits, no sideways scroll (${JSON.stringify(f)})`);
  await shot('packet-phone');
  await ev(`document.querySelector('#satPanel details').open = true; true`); await sleep(200);
  f = await fits(); ok(fitOk(f), `390 px with the timetable open (${JSON.stringify(f)})`);
  await shot('list-phone');
  await ev(`[...document.querySelectorAll('#satPanel .sat-row')].find(b => /^METEOR/.test(b.textContent)).click(); window.__p.onPicked({ norad: 57166, tx: 0, decoder: 'lrpt' }); true`); await sleep(300);
  f = await fits(); ok(fitOk(f), `390 px big box (${JSON.stringify(f)})`);
  await shot('big-phone');
  await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'light' }] }); await sleep(300);
  await ev(`[...document.querySelectorAll('#satPanel .sat-row')].find(b => /^ISS · FM/.test(b.textContent)).click(); window.__p.onPicked({ norad: 25544, tx: 0, decoder: '' }); true`); await sleep(1200);
  f = await fits(); ok(fitOk(f), `390 px compact, light (${JSON.stringify(f)})`);
  await shot('compact-phone-light');
  await desk(); await sleep(300); await shot('compact-desktop-light');
  // the text on the window's own background is never the same colour as it (a light page must not hide it)
  const contrast = await ev(`(() => { const p = document.getElementById('satPanel'); const cs = getComputedStyle(p); return cs.color + ' on ' + cs.backgroundColor; })()`);
  ok(!/^(rgb\(0, 0, 0\)) on rgba?\(0, 0, 0/.test(contrast) && contrast.split(' on ')[0] !== contrast.split(' on ')[1], `the window's text and ground differ (${contrast})`);

  console.log('— minimise, and the pass ratings');
  await ev(`document.querySelector('#satPanel [data-act="min"]').click(); true`);
  const mn = JSON.parse(await ev(`JSON.stringify({ min: document.getElementById('satPanel').classList.contains('min'), body: getComputedStyle(document.querySelector('#satPanel .sat-body')).display, hs: document.querySelector('#satPanel .sat-hs').textContent, h: document.getElementById('satPanel').getBoundingClientRect().height })`));
  ok(mn.min && mn.body === 'none' && mn.h < 60, `– folds it to the header (${Math.round(mn.h)} px)`);
  ok(/^ISS · UP/.test(mn.hs), `the folded header still says the pick: "${mn.hs}"`);
  await ev(`document.querySelector('#satPanel [data-act="min"]').click(); true`);
  ok(await ev(`!document.getElementById('satPanel').classList.contains('min')`), '□ restores it');
  await ev(`document.querySelector('#satPanel details').open = true; true`); await sleep(1100);
  const qs = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#satPanel .sat-row:not(.grey) .sat-q')].map(e => e.className + '|' + e.textContent))`));
  ok(qs.length >= 3 && qs.every(q => /^sat-q q-(excellent|good|marginal|poor)\|max \d+° · (EXCELLENT|GOOD|MARGINAL|POOR)$/.test(q)), `every hearable row rates its pass: ${qs.join(' / ')}`);
  ok(await ev(`[...document.querySelectorAll('#satPanel .sat-row.grey')].every(b => !b.querySelector('.sat-q'))`), 'a greyed row is not rated');
  await shot('ratings-desktop');

  console.log('— leaving the mode');
  await ev(`document.querySelector('#satPanel [data-act="exit"]').click()`);
  ok(JSON.stringify((await ev('window.__sent')).slice(-1)[0]) === '{"op":"drop"}', 'EXIT → op drop');
  ok(await ev(`document.getElementById('satPanel').hidden && window.__left === 1 && !window.__p.active`), 'the window closes, the page told (decoder stops)');
  ok(await ev(`document.getElementById('decImgWrap').parentNode === window.__home`), 'the image canvas is home');
  await ev(`window.__p.exit(); true`);
  ok((await ev('window.__sent')).filter((m) => m.op === 'drop').length === 1, 'leaving twice sends one drop');

  console.log('— 1.6 REMIND (a fake clock)');
  await desk(); await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'dark' }] });
  const T0 = Date.now();
  const t0 = Math.floor(T0 / 1000);
  const so50Aos = now + 38 * 60 + 20, son2Aos = now + 3 * 3600;
  const priv = (pp = true, over = {}) => { const l = list(over); l.source.pickPrivate = pp; l.sats = l.sats.map((x) => x.id === 'SO-50' ? { ...x, tx: [{ ...x.tx[0], reach: true, why: undefined }] } : x); return l; };
  await ev(`window.__fake = ${T0}; window.__p.show(); window.__p.onList(__Sat.parseSatList(${JSON.stringify(priv(false))})); document.querySelector('#satPanel details').open = true; true`);
  ok(await ev(`document.querySelectorAll('#satPanel .sat-rem').length === 0`), 'a shared dial (pickPrivate false): REMIND hidden, not greyed');
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(priv(true))})); true`);
  const rem = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#satPanel .sat-rem')].map(b => b.dataset.norad + ':' + b.textContent))`));
  ok(rem.length === 2 && rem.includes('27607:REMIND') && rem.includes('59112:REMIND'), `private: a REMIND key on each pass still to come, none on the up-now rows (${rem.join(', ')})`);
  ok(/REMIND.*1.*2.*5.*10.*min before it rises/.test(await ev(`document.querySelector('#satPanel .sat-lead').textContent`)), 'the lead choice 1 / 2 / 5 / 10 min');
  await ev(`document.querySelector('#satPanel .sat-rem[data-norad="27607"]').click(); true`);
  ok(await ev(`document.querySelector('#satPanel .sat-rem[data-norad="27607"]').textContent`) === 'REMINDING · 2 min' && await ev('window.__p.reminders().length') === 1, 'pressed: "REMINDING · 2 min", one held');
  await shot('remind-list');
  await ev(`document.querySelector('#satPanel [data-act="exit"]').click(); true`);
  ok(await ev('window.__p.reminders().length') === 1, 'leaving the mode keeps it (this page still on this server)');
  await ev(`window.__fake = ${(so50Aos - 121) * 1000}; window.__p.remTick(); true`);
  ok(await ev(`document.getElementById('satRemind').hidden`), '2:01 before AOS: nothing yet');
  await ev(`window.__fake = ${(so50Aos - 120) * 1000}; window.__p.remTick(); true`);
  const toast = await ev(`document.querySelector('#satRemind .sat-toast')?.textContent || ''`);
  ok(/^SO-50 rises in 2:00 — LISTEN\?/.test(toast) && /LISTEN/.test(toast) && /DISMISS/.test(toast), `2:00 before AOS: the pop-up (${toast})`);
  ok(await ev('window.__p.reminders().length') === 0, 'fired once (out of the book)');
  await shot('remind-toast');
  await ev(`document.querySelector('#satRemind [data-act="listen"]').click(); true`);
  ok(await ev('window.__entered') === 1 && await ev('window.__p.active'), 'LISTEN enters SATELLITE');
  ok(JSON.stringify((await ev('window.__sent')).slice(-1)[0]) === '{"op":"pick","norad":27607,"tx":0,"doppler":true}', 'LISTEN sends the ordinary pick');
  ok(await ev(`document.getElementById('satRemind').hidden`), 'the pop-up goes');
  // the re-check: shared by the time it fires
  await ev(`window.__fake = ${T0}; window.__p.onPicked({ norad: 27607, tx: 0, decoder: '' }); document.querySelector('#satPanel details').open = true; true`);
  await ev(`document.querySelector('#satPanel .sat-rem[data-norad="59112"]').click(); true`);
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(priv(false))})); window.__fake = ${(son2Aos - 120) * 1000}; window.__p.remTick(); true`);
  const why = await ev(`document.querySelector('#satRemind .sat-toast')?.textContent || ''`);
  ok(/This radio is now shared — a pass cannot be picked for everyone/.test(why) && !(await ev(`!!document.querySelector('#satRemind [data-act="listen"]')`)), `can no longer be picked: says why, no LISTEN (${why})`);
  await ev(`document.querySelector('#satRemind [data-act="dismiss"]').click(); true`);
  ok(await ev(`document.getElementById('satRemind').hidden`), 'DISMISS');
  // several at once — and "picking this ends SO-50"
  const many = priv(true); many.sats = many.sats.concat([1, 2, 3, 4].map((k) => ({ id: 'B' + k, name: 'BIRD-' + k, norad: 90000 + k, note: '', age: 1, ageState: 'fresh', el: -20,
    tx: [{ label: 'FM', hz: 145900000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice', band: 'U/v', modeName: 'Mode B' }], passes: [{ aos: now + 1200 + k, tca: now + 1500, los: now + 1800, maxEl: 40, aosAz: 0, losAz: 180 }] })));
  await ev(`window.__fake = ${T0}; window.__p.onList(__Sat.parseSatList(${JSON.stringify(many)})); document.querySelector('#satPanel details').open = true; true`);
  ok(/145\.900 MHz FM · Mode B · U\/v/.test(await ev(`document.querySelector('#satPanel .sat-row[data-norad="90001"]').textContent`)), '1.4 the row shows the mode label "Mode B · U/v"');
  for (const k of [1, 2, 3, 4]) await ev(`document.querySelector('#satPanel .sat-rem[data-norad="${90000 + k}"]').click(); true`);
  ok(await ev('window.__p.reminders().length') === 4, 'four held');
  await ev(`window.__fake = ${(now + 1200 + 4 - 120) * 1000}; window.__p.remTick(); true`);
  const st = JSON.parse(await ev(`JSON.stringify({ n: document.querySelectorAll('#satRemind .sat-toast').length, more: document.querySelector('#satRemind .sat-more')?.textContent, first: document.querySelector('#satRemind .sat-toast').textContent })`));
  ok(st.n === 3 && st.more === '+1 more', `several: three shown, "+1 more" (${st.n}, ${st.more})`);
  ok(/^BIRD-4/.test(st.first) && /picking this ends SO-50/.test(st.first), `newest on top; "picking this ends SO-50" (${st.first})`);
  await shot('remind-stack');
  for (let k = 0; k < 4; k++) await ev(`document.querySelector('#satRemind [data-act="dismiss"]')?.click(); true`);
  // no connection at fire time: never fires
  await ev(`window.__fake = ${T0}; document.querySelector('#satPanel .sat-rem[data-norad="90001"]').click(); window.__conn = false; window.__fake = ${(now + 1201 - 60) * 1000}; window.__p.remTick(); true`);
  ok(await ev(`document.getElementById('satRemind').hidden && window.__p.reminders().length === 0`), 'no connection when it is due: dropped silently');
  await ev(`window.__conn = true; window.__fake = ${T0}; window.__p.render(); document.querySelector('#satPanel .sat-rem[data-norad="90002"]').click(); window.__p.remindClear(); true`);
  ok(await ev('window.__p.reminders().length') === 0, 'the session ending / leaving clears them');
  // 1.3 telemetry in the box
  const ao73 = priv(true); ao73.sats.push({ id: 'AO-73', name: 'AO-73', norad: 39444, note: '', age: 1, ageState: 'fresh', el: 20, inRangeFor: 400, lat: 50, lon: 0, rangeKm: 1500,
    tx: [{ label: 'BPSK telemetry', hz: 145935000, mode: 'usb', loop: 'fll', reach: true, use: 'packet' }], passes: [{ aos: now - 100, tca: now + 100, los: now + 400, maxEl: 30, aosAz: 0, losAz: 180 }] });
  await ev(`window.__fake = ${T0}; window.__p.onList(__Sat.parseSatList(${JSON.stringify(ao73)})); window.__p.onPicked({ norad: 39444, tx: 0, decoder: 'fctlm' }); true`);
  ok(/TELEMETRY · waiting for a frame/.test(await ev(`document.querySelector('#satPanel .sat-tlm').textContent`)), '1.3 fctlm pick: the telemetry block waits for a frame');
  ok(await ev(`document.querySelector('#satPanel .sat-pk').hidden`), '…and no empty packet lines under it');
  // the server's two messages (docs/v12/SATELLITE-BEST.md Contract additions): fctlm_status counts, then a sat_tlm frame
  await ev(`window.__p.telemetry(__Sat.parseSatTelemetry({ sync: true, frames: 3, good: 2, failed: 1, afcHz: -20, snrDb: 6.5, centreHz: 1200, auto: true })); true`);
  await ev(`window.__p.telemetry(__Sat.parseSatTelemetry({ sat: 'AO-73', format: 'funcube-rt', t: 1763700000, mode: 'Safe', hex: '0a1b2c3d4e5f',
    values: [{ k: 'eps.vbat', name: 'Battery voltage', v: 8.21, unit: 'V' }, { k: 'pa.t', name: 'PA temp', v: 930, unit: 'C', raw: true }] })); true`);
  const tl = await ev(`document.querySelector('#satPanel .sat-tlm').innerText`);
  ok(/3 frames · 2 good · 1 failed · Safe/.test(tl) && /Battery voltage 8\.21 V/.test(tl) && /PA temp 930 raw/.test(tl) && !/930 C/.test(tl), `1.3 values with units, raw labelled with no unit (${tl.replace(/\n/g, ' / ')})`);
  await ev(`document.querySelector('#satPanel [data-act="tlmraw"]').click(); true`);
  ok(/0a1b2c3d 4e5f/.test(await ev(`document.querySelector('#satPanel .sat-tlm').innerText`)), '1.3 RAW shows the frame\'s bytes');
  await ev(`document.querySelector('#satPanel [data-act="tlmraw"]').click(); true`);
  await shot('telemetry');
} catch (e) { ok(false, `driver: ${e?.stack || e}`); }
finally {
  clearTimeout(deadline);
  try { wsC?.close(); } catch {}
  try { edge.kill(); } catch {}
  srv.close();
  await sleep(600);
  try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {}
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed · screenshots: ${SHOTS}/sat-web-*.png`);
process.exit(fail ? 1 : 0);
