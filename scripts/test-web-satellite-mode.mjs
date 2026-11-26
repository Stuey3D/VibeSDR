// test-web-satellite-mode.mjs — ★★★ V12 SATELLITE MODE, the web window (web/client/src/satellites.ts) against a FAKE sat_list
// (the contract in docs/v12/SATELLITE-MODE.md — no server, no network), inside the BUILT page (web/dist, so the real CSS
// variables and the real #decImgWrap image canvas), in headless Edge with --mute-audio (tests are silent):
//   the timetable's order (up now shortest-remaining first, then AOS) and its words, greyed rows (why, no pick sent), the
//   synced line (synced / fetching — re-asked every 3 s — / none + why), a pick → {"op":"pick",doppler:true}, "picked"
//   → the COMPACT box (corrected frequency, "In range for", the half-globe drawn), AUTO DOPPLER → {"op":"doppler"},
//   METEOR (image) → the BIG box with the decoder image canvas lent into it, ISS 145.800 (sstv) compact → big on a
//   picture, a packet bird's APRS lines, EXIT → {"op":"drop"} and the canvas home again; desktop and 390 px, dark and light,
//   no sideways scroll; ★★★ beta92 OPEN IN VFO B (RS-44 transponder → its CW beacon in B: the key, B's line with the
//   Doppler, ONE box; a CW beacon on A says its CW decoder listens and shows the text; ISS 145.800 SSTV + 145.825 APRS in
//   B: the line switch chooses the decoder area, B off ends it). Needs web/dist (node scripts/build-web.mjs). Screenshots to $SAT_SHOTS (default $TMPDIR).
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
const bundle = (await esbuild.build({ stdin: { contents: "export { SatPanel } from './web/client/src/satellites.ts'; export { parseSatList, parseSatTelemetry, parseSatTrack, parseSatVfoB, satHzText, satVfoBHz } from './src/services/satellites.ts';", resolveDir: root, loader: 'ts' },
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

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-satmode-'));
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
      galleryGet: (p) => (window.__galGet ? window.__galGet(p) : Promise.resolve(null)),   // ★★ beta91 the saved passes
      vfobOpen: (norad, tx) => window.__sent.push({ vfob: [norad, tx] }),                 // ★★★ beta92 OPEN IN VFO B
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
  // ★ beta91 the time left counts down to the pass's LOS (the AOS-glitch fix, satModeRows) — the page has run a few seconds
  ok(/UP · 6:[01]\d left/.test(rows[0].t) && /UP · 9:[45]\d left/.test(rows[3].t), `"UP · 6:12 left" (counting down): ${rows[0].t.match(/UP · [\d:]+ left/)}`);
  /* ★★ beta91 (Stuart, Safari: stutters with the box open) the timetable is NOT rebuilt every second: the same row
   *  elements a tick later, their countdowns rewritten in place */
  {
    await ev(`window.__rowMark = document.querySelector('#satPanel .sat-row'); window.__rowMark.dataset.mark = '1'; true`);
    const t0 = await ev(`document.querySelector('#satPanel .sat-row .sat-w').textContent`);
    await new Promise((r) => setTimeout(r, 2300));
    const kept = await ev(`document.querySelector('#satPanel .sat-row') === window.__rowMark`);
    const t1 = await ev(`document.querySelector('#satPanel .sat-row .sat-w').textContent`);
    ok(kept && t0 !== t1, `beta91 the rows are kept between ticks, only the countdown changes (${t0} → ${t1})`);
  }
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
  ok(/^In range for 6:\d\d · highest in 1:\d\d \(40°\)$/.test(box.cd), `the countdown, with the time to its highest (beta92): ${box.cd}`);
  {   // ★ beta91 which is which: the uplink under the big (downlink) frequency
    const up = await ev(`(() => { const e = document.querySelector('#satPanel [data-s="up"]'); return e && !e.hidden ? e.textContent : ''; })()`);
    ok(up === 'Uplink 145.990 MHz · 67 Hz tone · the frequency above is the downlink', `beta91 the box names the uplink: ${up}`);
  }
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
  // ★★ beta96 SMALL draws the orbit without its arrows / tick (globeLayers): the dotted line alone adds fewer pixels
  ok(ink2 > ink + 50, `1.7 the dotted orbit is inked (${ink} → ${ink2} px)`);
  // ★ beta91 the direction: the path runs north → south in time, the bird (47 N, 9 W) is on it — the countdown says so
  const cdH = await ev(`document.querySelector('#satPanel [data-s="cd"]').textContent`);
  ok(/^In range for 6:\d\d · highest in 1:\d\d \(40°\) · heading south$/.test(cdH), `beta91 the countdown says which way: ${cdH}`);
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
  await ev(`window.__p.onTrack(__Sat.parseSatTrack(${JSON.stringify({ type: 'sat_track', norad: 57166, aos: now - 300, los: now + 595, pts: [], geo })})); true`);
  ok(/dotted: this pass.s orbit · arrows: its direction · shaded/.test(await ev(`document.querySelector('#satPanel [data-s="key"]').textContent`)), 'beta91 BIG with an orbit: the key names the arrows');
  ok(/120 lines/.test(await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`)), 'the LRPT line in the box');
  // ★★ beta92 (Stuart 11-23: "any chance of making the lines drawn full width of the box?"): a pass grows TALL — the
  //    picture stays the box's full width and the box scrolls, instead of shrinking to fit 440 px
  const fw = JSON.parse(await ev(`(() => { const c = document.getElementById('decImage'), x = c.getContext('2d'); const img = x.getImageData(0, 0, 392, 300);
    c.height = 1600; x.putImageData(img, 0, 0); x.fillStyle = '#556'; x.fillRect(0, 300, 392, 1300);
    const w = document.querySelector('#satPanel #decImgWrap'), r = c.getBoundingClientRect(), wr = w.getBoundingClientRect();
    return JSON.stringify({ iw: r.width, ww: w.clientWidth, ih: r.height, wh: wr.height, sh: w.scrollHeight }); })()`));   // ★ 11-24: the box's INSIDE width — a classic (always-shown) scrollbar is not picture room
  ok(Math.abs(fw.iw - fw.ww) <= 2, `a 1600-line pass is drawn at the box's full width (${Math.round(fw.iw)} of ${Math.round(fw.ww)} px)`);
  ok(fw.wh <= 445 && fw.sh > fw.wh + 400, `…and the box scrolls (${Math.round(fw.wh)} px tall, ${Math.round(fw.sh)} px of picture)`);
  await ev(`(() => { const c = document.getElementById('decImage'), x = c.getContext('2d'); const img = x.getImageData(0, 0, 392, 300); c.height = 300; x.putImageData(img, 0, 0); return true; })()`);
  await shot('big-desktop');
  await ev(`document.querySelector('#satPanel [data-act="size"]').click(); true`);
  ok(await ev(`!document.getElementById('satPanel').classList.contains('big') && document.querySelector('#satPanel .sat-img').hidden`), 'the size key → SMALL (the picture keeps drawing, out of sight)');
  await ev(`document.querySelector('#satPanel [data-act="size"]').click(); true`);
  ok(await ev(`document.getElementById('satPanel').classList.contains('big') && !document.querySelector('#satPanel .sat-img').hidden`), '…and BIG again');
  await ev(`document.querySelector('#satPanel [data-act="size"]').click(); true`);   // SMALL by hand: the next pick opens SMALL
  await ev(`window.__p.pictureEnd(); true`);
  // ★ beta91 on LRPT the decoder's own line is the live state and wins over the words (Stuart's Meteor-M2-4 pass 11-22)
  ok(/120 lines/.test(await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`)), 'pass end (LRPT): the LRPT line stays — it says a filed pass itself');

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
  // ★★★ beta96 (Stuart 11-26): REMIND is the SHARED DIAL's key now (pickPrivate false, nobody said to be alone); a private
  //    radio gets SCHEDULE (its own section below). So this section runs on a shared dial: priv(false).
  const priv = (pp = false, over = {}) => { const l = list(over); l.source.pickPrivate = pp; l.sats = l.sats.map((x) => x.id === 'SO-50' ? { ...x, tx: [{ ...x.tx[0], reach: true, why: undefined }] } : x); return l; };
  await ev(`window.__fake = ${T0}; window.__p.show(); window.__p.onList(__Sat.parseSatList(${JSON.stringify(priv(true))})); document.querySelector('#satPanel details').open = true; true`);
  ok(await ev(`document.querySelectorAll('#satPanel [data-act="remind"]').length === 0 && document.querySelectorAll('#satPanel [data-act="sched"]').length === 2`), 'beta96 a private radio: SCHEDULE keys, no REMIND');
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(priv(false))})); true`);
  const rem = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#satPanel .sat-rem')].map(b => b.dataset.norad + ':' + b.textContent))`));
  ok(rem.length === 2 && rem.includes('27607:REMIND') && rem.includes('59112:REMIND'), `beta96 a shared dial: a REMIND key on each pass still to come, none on the up-now rows (${rem.join(', ')})`);
  ok(/A shared dial — tuning moves everyone/.test(await ev(`document.querySelector('#satPanel .sat-sline')?.textContent || ''`)), 'beta96 the line says why it is REMIND here');
  ok(/REMIND.*1.*2.*5.*10.*min before it rises/.test(await ev(`document.querySelector('#satPanel .sat-lead').textContent`)), 'the lead choice 1 / 2 / 5 / 10 min');
  await ev(`document.querySelector('#satPanel .sat-rem[data-norad="27607"]').click(); true`);
  ok(await ev(`document.querySelector('#satPanel .sat-rem[data-norad="27607"]').textContent`) === 'FORGET' && await ev('window.__p.reminders().length') === 1, 'pressed: lit "FORGET", one held');
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
  // the re-check: out of range by the time it fires (★★★ beta96: a shared dial no longer blocks LISTEN — it is REMIND's home)
  await ev(`window.__fake = ${T0}; window.__p.onPicked({ norad: 27607, tx: 0, decoder: '' }); document.querySelector('#satPanel details').open = true; true`);
  await ev(`document.querySelector('#satPanel .sat-rem[data-norad="59112"]').click(); true`);
  const gone = priv(false); gone.sats = gone.sats.map((x) => x.id === 'SONATE-2' ? { ...x, tx: [{ ...x.tx[0], reach: false, why: 'Out of this receiver’s range — it is locked to 430–440 MHz' }] } : x);
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(gone)})); window.__fake = ${(son2Aos - 120) * 1000}; window.__p.remTick(); true`);
  const why = await ev(`document.querySelector('#satRemind .sat-toast')?.textContent || ''`);
  ok(/locked to 430–440 MHz/.test(why) && !(await ev(`!!document.querySelector('#satRemind [data-act="listen"]')`)), `can no longer be picked: says why, no LISTEN (${why})`);
  await ev(`document.querySelector('#satRemind [data-act="dismiss"]').click(); true`);
  ok(await ev(`document.getElementById('satRemind').hidden`), 'DISMISS');
  // several at once — and "picking this ends SO-50"
  const many = priv(false); many.sats = many.sats.concat([1, 2, 3, 4].map((k) => ({ id: 'B' + k, name: 'BIRD-' + k, norad: 90000 + k, note: '', age: 1, ageState: 'fresh', el: -20,
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

  console.log('— ★★★ beta96 SCHEDULE (a private radio; a fake clock)');
  {
    await ev(`window.__fake = ${T0}; localStorage.removeItem('vibe.satSchedule'); window.__back = []; window.__sess = {};
      window.__p.deps.session = () => window.__sess; window.__p.deps.here = () => ({ hz: 101100000, mode: 'wfm' });
      window.__p.deps.back = (hz, mode) => window.__back.push([hz, mode]); true`);
    const sl = priv(true);
    sl.sats.push({ id: 'OV-1', name: 'OV-1', norad: 90100, note: '', age: 1, ageState: 'fresh', el: -20,
      tx: [{ label: 'FM', hz: 145900000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' }], passes: [{ aos: now + 40 * 60, tca: now + 45 * 60, los: now + 50 * 60, maxEl: 70, aosAz: 0, losAz: 180 }] });
    await ev(`window.__p.show(); window.__p.onList(__Sat.parseSatList(${JSON.stringify(sl)})); document.querySelector('#satPanel details').open = true; true`);
    ok(/Only you on this radio: SCHEDULE tunes it/.test(await ev(`document.querySelector('#satPanel .sat-sline')?.textContent || ''`))
       && /only while this page is open and connected/.test(await ev(`document.querySelector('#satPanel .sat-sline')?.textContent || ''`)), 'the line: why SCHEDULE, and only while the page is open');
    await ev(`document.querySelector('#satPanel [data-act="sched"][data-norad="27607"]').click(); true`);
    ok(await ev(`document.querySelector('#satPanel [data-act="sched"][data-norad="27607"]').textContent`) === 'CANCEL' && await ev('window.__p.scheduled().length') === 1, 'SCHEDULE pressed: lit CANCEL, one scheduled');
    ok(/SCHEDULED · tunes \d\d:\d\d–\d\d:\d\d, then back/.test(await ev(`document.querySelector('#satPanel .sat-row[data-norad="27607"] .sat-s').textContent`)), 'the row says when it tunes');
    ok(/27607/.test(await ev(`localStorage.getItem('vibe.satSchedule') || ''`)), 'kept in this browser (per server)');
    await ev(`document.querySelector('#satPanel [data-act="sched"][data-norad="90100"]').click(); true`);
    ok(await ev(`document.querySelectorAll('#satPanel [data-act="prio"]').length`) === 2, 'two overlapping scheduled passes: PRIORITISE on each');
    ok(/only — OV-1 is the higher pass/.test(await ev(`document.querySelector('#satPanel .sat-row[data-norad="27607"] .sat-s').textContent`)), 'no choice: the higher pass wins, said on the other');
    ok(await ev(`[...document.querySelectorAll('#satPanel .sat-sline')].some(e => /the higher pass wins unless you PRIORITISE/.test(e.textContent))`), 'the overlap rule is stated');
    await shot('schedule-overlap');
    await ev(`document.querySelector('#satPanel [data-act="prio"]').closest('.sat-rw').querySelector('[data-act="prio"]').click(); true`);
    const pr = await ev(`[...document.querySelectorAll('#satPanel [data-act="prio"]')].map(b => b.textContent).sort().join(',')`);
    ok(pr === 'PRIORITISE,PRIORITISED', `PRIORITISE: one lit (${pr})`);
    await ev(`document.querySelector('#satPanel [data-act="prio"].on').click(); true`);
    ok(await ev(`document.querySelectorAll('#satPanel [data-act="prio"].on').length`) === 0, 'pressed again: back to the default');
    // the runner — out of the mode first: the schedule enters it
    await ev(`document.querySelector('#satPanel [data-act="exit"]').click(); window.__sent = []; window.__entered = 0; true`);
    await ev(`window.__fake = ${(so50Aos - 5) * 1000}; window.__p.remTick(); true`);
    ok((await ev('window.__sent')).length === 0, 'before AOS: nothing sent');
    await ev(`window.__fake = ${so50Aos * 1000}; window.__p.remTick(); true`);
    ok(await ev('window.__entered') === 1 && JSON.stringify((await ev('window.__sent')).slice(-1)[0]) === '{"op":"pick","norad":27607,"tx":0,"doppler":true}', 'AOS: into SATELLITE and the ordinary pick');
    await ev(`window.__p.onPicked({ norad: 27607, tx: 0, decoder: '' }); window.__fake = ${(so50Aos + 30) * 1000}; window.__p.remTick(); true`);
    ok((await ev('window.__sent')).filter((m) => m.op === 'pick').length === 1, 'tuned once, not every second');
    const stl = await ev(`[...document.querySelectorAll('#satPanel .sat-sline')].map(e => e.textContent).join(' | ')`);
    ok(/SCHEDULE · on SO-50 until .*then back to 101\.100[\d ]* MHz WFM/.test(stl), `the status line: on SO-50, then back to 101.1 WFM (${stl})`);
    await ev(`window.__fake = ${(now + 40 * 60) * 1000}; window.__p.remTick(); true`);
    ok(JSON.stringify((await ev('window.__sent')).slice(-1)[0]) === '{"op":"pick","norad":90100,"tx":0,"doppler":true}', 'the higher overlapping pass takes over at its AOS (no back in between)');
    ok(!(await ev('window.__back')).length, '…nothing put back yet');
    await ev(`window.__p.onPicked({ norad: 90100, tx: 0, decoder: '' }); window.__fake = ${(now + 50 * 60) * 1000}; window.__p.remTick(); true`);
    ok(JSON.stringify(await ev('window.__back')) === '[[101100000,"wfm"]]' && (await ev('window.__sent')).slice(-1)[0].op === 'drop' && !(await ev('window.__p.active')), 'after LOS: out of SATELLITE and back to 101.1 WFM');
    ok(await ev('window.__p.scheduled().length') === 0, 'passes over leave the book');
    // degrade: someone else on the dial at fire time
    await ev(`window.__fake = ${T0}; window.__p.show(); window.__p.onList(__Sat.parseSatList(${JSON.stringify(sl)})); document.querySelector('#satPanel details').open = true;
      document.querySelector('#satPanel [data-act="sched"][data-norad="59112"]').click(); window.__p.exit(); window.__sent = []; true`);
    ok(await ev('window.__p.scheduled().length') === 1, 'SONATE-2 scheduled');
    await ev(`window.__sess = { sharedDial: true, listeners: 2 }; window.__p.render(); true`);
    ok(/your 1 scheduled pass will remind you/.test(await ev(`window.__p.show(), [...document.querySelectorAll('#satPanel .sat-sline')].map(e => e.textContent).join(' | ')`)), 'another listener on the dial: the line says scheduled passes remind instead');
    const lead0 = Number(await ev('window.__p.lead')) || 2;
    await ev(`window.__p.exit(); window.__sent = []; window.__fake = ${(son2Aos - lead0 * 60) * 1000}; true`);
    await ev(`window.__p.remTick(); true`);
    const dt = await ev(`document.querySelector('#satRemind .sat-toast')?.textContent || ''`);
    ok(/SONATE-2 rises in/.test(dt) && /did not tune by itself/.test(dt) && !(await ev('window.__sent')).some((m) => m.op === 'pick'), `degraded: a reminder (saying why), no tune (${dt})`);
    await ev(`document.querySelector('#satRemind [data-act="dismiss"]')?.click(); window.__sess = {}; true`);
    await ev(`window.__fake = ${son2Aos * 1000}; window.__p.onList(__Sat.parseSatList(${JSON.stringify(sl)})); window.__p.remTick(); true`);
    ok((await ev('window.__sent')).some((m) => m.op === 'pick' && m.norad === 59112), 'alone again before AOS: it tunes');
    await ev(`window.__sess = { sharedDial: true, listeners: 3 }; window.__p.onPicked({ norad: 59112, tx: 0, decoder: 'aprs' }); window.__back = []; window.__fake = ${(son2Aos + 60) * 1000}; window.__p.remTick(); true`);
    await ev(`window.__fake = ${(son2Aos + 700) * 1000}; window.__p.remTick(); true`);
    ok(!(await ev('window.__back')).length && await ev('window.__p.active'), 'someone joins mid-pass: nothing put back, the radio stays where it is');
    // ★ left as the REMIND section left it (the mode on), for the telemetry section after
    await ev(`window.__p.exit(); window.__sess = {}; localStorage.removeItem('vibe.satSchedule'); window.__fake = ${T0}; window.__p.show(); true`);
  }
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
  // ★★ beta94 a frame whose EPS data is not valid: said, its values not shown
  await ev(`window.__p.telemetry(__Sat.parseSatTelemetry({ sat: 'JO-97', format: 'funcube-rt', t: 1795600005, absent: ['EPS', 'iMTQ'],
    values: [{ k: 'rf.temp', name: 'RF temperature', v: -1.6, unit: '\u00b0C' }, { k: 'rf.rssi', name: 'Receiver RSSI', v: 255, raw: true, note: 'no conversion' }] })); true`);
  const tla = await ev(`document.querySelector('#satPanel .sat-tlm').innerText`);
  ok(/No EPS or iMTQ data in this frame \(their data-valid flags are off\)/.test(tla) && /RF temperature -1\.6 \u00b0C/.test(tla) && !/Battery voltage/.test(tla),
     `a board with no valid data is said, not shown as zeros (${tla.replace(/\n/g, ' / ')})`);
  await ev(`window.__p.telemetry(__Sat.parseSatTelemetry({ frames: 3, good: 2, failed: 1, sync: true, sat: 'AO-73', format: 'funcube-rt', mode: 'Safe', t: 1795600010, absent: [],
    values: [{ k: 'eps.vbat', name: 'Battery voltage', v: 8.21, unit: 'V' }, { k: 'pa.t', name: 'PA temp', v: 930, unit: 'C', raw: true }] })); true`);
  await ev(`document.querySelector('#satPanel [data-act="tlmraw"]').click(); true`);
  ok(/0a1b2c3d 4e5f/.test(await ev(`document.querySelector('#satPanel .sat-tlm').innerText`)), '1.3 RAW shows the frame\'s bytes');
  await ev(`document.querySelector('#satPanel [data-act="tlmraw"]').click(); true`);
  await shot('telemetry');

  // ★★ beta91 TELEMETRY CHARTS — THIS PASS from the live frames, LAST PASSES from the gallery. The frames and the gallery's
  //    files are scripts/fixtures/fctlm/ (the server's own writer on SYNTHETIC readings — not on-air data).
  console.log('— beta91 telemetry charts');
  const FIX = path.join(root, 'scripts/fixtures/fctlm');
  const fx = (n) => fs.readFileSync(path.join(FIX, `pass-${n}.csv`), 'utf8');
  const framesOf = (csv) => {
    const L = csv.split('\n').filter((l) => l && !l.startsWith('#'));
    const head = L[0].split(',');
    // ★ beta94 as the server sends them: a blank cell (the board's data-valid flag off) is NOT sent, and named in "absent"
    return L.slice(1).map((l) => { const c = l.split(','); const eps = c[7 + head.slice(7).indexOf('Battery voltage [V]')] === '';
      return { sat: 'AO-73', format: 'funcube-rt', t: Number(c[1]), mode: c[4], absent: eps ? ['EPS'] : [],
      values: head.slice(7).map((h, k) => { const m = /^(.*?) \[(.*)\]$/.exec(h); return c[7 + k] === '' ? null : { k: `c${k}`, name: m ? m[1] : h, v: Number(c[7 + k]), ...(m && m[2] === 'raw' ? { raw: true } : m ? { unit: m[2] } : {}) }; }).filter(Boolean) }; });
  };
  const galItems = [1, 2, 3].map((n) => { const t = Number(fx(n).split('\n').find((l) => /^\d{4}-/.test(l)).split(',')[1]);
    return { f: `p${n}.csv`, k: 'fctlm', t, title: `AO-73 telemetry · pass ${n} UTC · x frames` }; });
  await ev(`window.__galFiles = ${JSON.stringify({ 'p1.csv': fx(1), 'p2.csv': fx(2), 'p3.csv': fx(3) })}; window.__galAsks = [];
    window.__galGet = async (p) => { window.__galAsks.push(p); await new Promise((r) => setTimeout(r, 5));
      if (p.startsWith('/vibeserver/gallery.json')) return JSON.stringify({ items: ${JSON.stringify(galItems)} });
      return window.__galFiles[decodeURIComponent(p.replace('/vibeserver/gallery/file/', ''))] ?? null; }; true`);
  // a new pass on the air: the box's history starts again (a fresh pick), then the frames of pass 3 arrive
  await ev(`window.__p.onPicked({ norad: 39444, tx: 0, decoder: 'fctlm' }); true`);
  for (const fr of framesOf(fx(3))) await ev(`window.__p.telemetry(__Sat.parseSatTelemetry(${JSON.stringify(fr)})); true`);
  ok(!!(await ev(`!!document.querySelector('#satPanel [data-act="tlmcharts"]')`)), 'CHARTS beside RAW');
  await ev(`document.querySelector('#satPanel [data-act="tlmcharts"]').click(); true`);
  ok(await until(`/LAST 3 PASSES/.test(document.querySelector('#satPanel .sat-tlm').textContent)`, 5000), 'the last passes come from the gallery: LAST 3 PASSES');
  const ch = JSON.parse(await ev(`JSON.stringify((() => { const t = document.querySelector('#satPanel .sat-tlm'); const c = [...t.querySelectorAll('.tlm-c')];
    return { n: c.length, first: c[0]?.querySelector('.tlm-n')?.textContent, firstV: c[0]?.querySelector('.tlm-v')?.textContent,
      heads: [...t.querySelectorAll('.tlm-h')].map((h) => h.textContent), more: t.querySelector('[data-act="tlmall"]')?.textContent,
      lines: t.querySelectorAll('path.tlm-l').length, bands: t.querySelectorAll('path.tlm-b').length, label: c[0]?.getAttribute('aria-label'),
      text: t.innerText }; })())`));
  ok(ch.n === 16 && ch.lines === 16 && ch.bands === 8, `8 charts this pass + 8 across passes, a line each, the across ones banded (${ch.n}/${ch.lines}/${ch.bands})`);
  ok(ch.first === 'Battery voltage' && /^\d+\.\d+ V$/.test(ch.firstV || ''), `★ beta94 the power system first, in volts (${ch.first} ${ch.firstV})`);
  ok(/^THIS PASS · 24 frames · 17:2\d–17:2\d UTC$/.test(ch.heads[0] || ''), `this pass's head (${ch.heads[0]})`);
  ok(/^\+ \d+ MORE CHANNELS$/.test(ch.more || ''), `the rest behind one key (${ch.more})`);
  ok(/^Battery voltage: 21 readings, [\d.]+–[\d.]+ V, latest [\d.]+ V$/.test(ch.label || ''), `each chart says itself, in its unit — the 3 EPS-less frames are no readings, not zeros (${ch.label})`);
  ok(!/Reboot count/.test(ch.text) && /raw \(no published conversion\): Receiver Doppler, Receiver RSSI/.test(ch.text) && !/uncalibrated/.test(ch.text),
     'counts are not charted; the raw channels are named (no blanket footnote)');
  const tl3 = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#satPanel .tlm-c')].map((c) => c.querySelector('.tlm-v')?.textContent))`));
  ok(tl3.slice(0, 8).every((v) => / (V|mA|\u00b0C|mW|lux)$/.test(v)), `the first eight charts' readings carry their unit (${tl3.slice(0, 8).join(' · ')})`);
  const asks = await ev(`JSON.stringify(window.__galAsks)`);
  ok(JSON.parse(asks).length === 4 && /kind=fctlm/.test(asks), `one list + one file a pass, one at a time (${JSON.parse(asks).length} asks)`);
  // the geometry inside its boxes
  const tgeo = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#satPanel .tlm svg')].every((s) => { const b = s.getBBox(); return b.x >= -0.5 && b.y >= -0.5 && b.x + b.width <= s.width.baseVal.value + 0.5 && b.y + b.height <= s.height.baseVal.value + 0.5; }))`));
  ok(tgeo === true, 'every line and band inside its chart');
  await shot('charts-desktop');
  await ev(`[...document.querySelectorAll('#satPanel .tlm-h')].pop().scrollIntoView({ block: 'start' }); true`); await sleep(150);
  await shot('charts-desktop-across');
  await ev(`document.querySelector('#satPanel [data-act="tlmall"]').click(); true`);
  const all = await ev(`document.querySelectorAll('#satPanel .tlm-c').length`);
  ok(all > 16 && all % 2 === 0, `+ MORE: every analogue channel, both sections (${all} charts)`);
  await ev(`document.querySelector('#satPanel [data-act="tlmall"]').click(); true`);
  // a new frame repaints this pass (25 frames); the across set is not fetched again
  const one = framesOf(fx(3)).slice(-1)[0]; one.t += 5;
  await ev(`window.__p.telemetry(__Sat.parseSatTelemetry(${JSON.stringify(one)})); true`);
  ok(/THIS PASS · 25 frames/.test(await ev(`document.querySelector('#satPanel .sat-tlm').textContent`)) && JSON.parse(await ev(`JSON.stringify(window.__galAsks)`)).length === 4,
     'a new frame joins this pass; the gallery is not asked again');
  await phone(); await sleep(400);
  await ev(`window.__p.telemetry(__Sat.parseSatTelemetry(${JSON.stringify({ ...one, t: one.t + 5 })})); true`); await sleep(100);
  f = await fits(); ok(fitOk(f), `390 px with the charts: fits, no sideways scroll (${JSON.stringify(f)})`);
  const pc = await ev(`getComputedStyle(document.querySelector('#satPanel .tlm-g')).gridTemplateColumns.split(' ').length`);
  ok(pc === 2, `390 px: two charts a row (${pc})`);
  await shot('charts-phone-light');
  await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'dark' }] }); await sleep(300);
  await shot('charts-phone-dark');
  const tink = await ev(`(() => { const l = document.querySelector('#satPanel path.tlm-l'); const n = document.querySelector('#satPanel .tlm-n'); return getComputedStyle(l).stroke + ' | ' + getComputedStyle(n).color + ' | ' + getComputedStyle(document.getElementById('satPanel')).backgroundColor; })()`);
  ok(!/none/.test(tink.split(' | ')[0]) && tink.split(' | ')[1] !== tink.split(' | ')[2], `the line has the page's hue and the names a readable ink (${tink})`);
  // no gallery: this pass still drawn, the across head says why
  await ev(`window.__galGet = null; document.querySelector('#satPanel [data-act="tlmcharts"]').click(); document.querySelector('#satPanel [data-act="tlmcharts"]').click(); true`);
  ok(await until(`/LAST PASSES · none saved/.test(document.querySelector('#satPanel .sat-tlm').textContent) && document.querySelectorAll('#satPanel .tlm-c').length === 8`, 3000),
     'no gallery (or an empty one): this pass drawn, "none saved in the gallery yet"');
  await desk();

  // ── ★★★ beta92 OPEN IN VFO B ──
  console.log('— beta92 OPEN IN VFO B: RS-44 transponder → its CW beacon');
  await desk(); await sleep(200);
  await ev(`window.__fake = null; window.__p.exit(); window.__p.show(); true`);
  const rsList = list();
  rsList.sats.unshift({ id: 'RS-44', name: 'RS-44', norad: 44909, note: '', age: 0.3, ageState: 'fresh', el: 36, az: 120, rangeKm: 2100, lat: 44, lon: 8, inRangeFor: 640,
    tx: [{ label: 'Transponder', hz: 435610000, hiHz: 435670000, mode: 'usb', loop: 'none', reach: true, use: 'voice', upHz: 145935000 },
         { label: 'CW beacon', hz: 435605000, mode: 'cw', loop: 'none', reach: true, use: 'packet', decoder: 'cw' }],
    passes: [{ aos: now - 400, tca: now + 100, los: now + 640, maxEl: 48, aosAz: 300, losAz: 120 }] });
  await ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(rsList)})); true`);
  // one at a time, as before: a pick with nothing offered draws no VFO B part at all
  await ev(`window.__p.onPicked({ norad: 44909, tx: 0, decoder: '' }); window.__p.onReadout({ state: 'TRACK', name: 'RS-44', norad: 44909, tx: 'Transponder', nominal: 435640000, offset: -4745, rate: 12, time: 'ok' }); true`);
  ok(await ev(`document.querySelector('#satPanel .sat-vb').hidden && !document.querySelector('#satPanel [data-act="vfob"]')`), 'a pick with no vfob offer: no key, no B line (the one-downlink box)');
  // the server offers the beacon
  await ev(`window.__p.onPicked({ norad: 44909, tx: 0, decoder: '', vfob: [1] }); true`);
  const key = await ev(`document.querySelector('#satPanel [data-act="vfob"]')?.textContent || ''`);
  ok(key === 'OPEN IN VFO B · CW beacon', `the key for the beacon: "${key}"`);
  await shot('vfob-offer-desktop');
  await ev(`window.__sent = []; document.querySelector('#satPanel [data-act="vfob"]').click(); true`);
  ok(JSON.stringify(await ev('window.__sent')) === '[{"vfob":[44909,1]}]', `the key sends sat_vfob for that downlink (${JSON.stringify(await ev('window.__sent'))})`);
  await ev(`window.__p.onVfoB(__Sat.parseSatVfoB({ type: 'sat_vfob', on: true, norad: 44909, tx: 1, hz: 435605000, mode: 'cw', use: 'packet', label: 'CW beacon', decoder: 'cw', params: {}, shift: -4744.6 })); true`);
  const bHz = await ev(`__Sat.satHzText(__Sat.satVfoBHz(435605000, { state: 'TRACK', nominal: 435640000, offset: -4745, time: 'ok' }))`);
  const vb = JSON.parse(await ev(`JSON.stringify({ line: document.querySelector('#satPanel .sat-vb .sat-ln')?.textContent, keys: document.querySelectorAll('#satPanel [data-act="vfob"]').length,
    boxes: document.querySelectorAll('#satPanel .sat-box').length, globes: document.querySelectorAll('#satPanel canvas.sat-globe').length,
    sw: document.querySelectorAll('#satPanel .sat-ln.sw').length, cw: !document.querySelector('#satPanel .sat-cw').hidden && document.querySelector('#satPanel .sat-cw').textContent,
    view: window.__p.viewNow() })`));
  ok(vb.line === `VFO B · CW beacon · ${bHz} · CW decoder listening for the beacon`, `B's line with its Doppler: "${vb.line}" (want ${bHz})`);
  ok(vb.boxes === 1 && vb.globes === 1, 'ONE box, one globe');
  ok(vb.keys === 0, 'the key goes once the beacon is open');
  ok(vb.sw === 0 && vb.view === 'B' && vb.cw === 'no characters yet', `A (a transponder) has no decoder: no switch, the area is B's CW (${JSON.stringify(vb)})`);
  await ev(`window.__p.bTextAdd('RS44 RS44 '); window.__p.bTextAdd('DE RS44 K'); window.__p.bState('CW decoder reading the beacon'); true`);
  ok(/RS44 RS44 DE RS44 K/.test(await ev(`document.querySelector('#satPanel .sat-cw').textContent`)) && /reading the beacon/.test(await ev(`document.querySelector('#satPanel .sat-vb').textContent`)),
     'B\'s decoded CW in the area, its own words on its line');
  await shot('vfob-cw-desktop');
  await phone(); await sleep(400);
  f = await fits(); ok(fitOk(f), `390 px with VFO B open: fits, no sideways scroll (${JSON.stringify(f)})`);
  await shot('vfob-cw-phone');
  await desk(); await sleep(200);
  await ev(`window.__p.onVfoB(__Sat.parseSatVfoB({ type: 'sat_vfob', on: false, norad: 44909, tx: 1, why: 'VFO B was switched off' })); true`);
  ok(await ev(`!document.querySelector('#satPanel .sat-vb .sat-ln') && document.querySelector('#satPanel .sat-cw').hidden && !!document.querySelector('#satPanel [data-act="vfob"]')`),
     'B off: its line and area go, the key comes back');
  await ev(`window.__p.onVfoB(__Sat.parseSatVfoB({ type: 'sat_vfob', on: false, refused: true, norad: 44909, tx: 1, why: 'Outside the radio’s window here' })); true`);
  ok(/Outside the radio/.test(await ev(`document.querySelector('#satPanel .sat-why').textContent`)), 'a refusal says why');

  console.log('— beta92 fix: a CW beacon on VFO A');
  await ev(`window.__p.onPicked({ norad: 44909, tx: 1, decoder: 'cw', vfob: [0] }); true`);
  const cwSt = await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`);
  ok(/CW decoder listening for the beacon/.test(cwSt) && !/packets/.test(cwSt), `a CW beacon says its CW decoder is listening ("${cwSt}")`);
  ok(await ev(`document.querySelector('#satPanel .sat-pk').hidden && document.querySelector('#satPanel .sat-cw').textContent === 'no characters yet'`), 'no "no packets yet" — the CW text area instead');
  await ev(`window.__p.cwText('VVV DE RS44'); true`);
  ok(await ev(`document.querySelector('#satPanel .sat-cw').textContent`) === 'VVV DE RS44', 'its decoded text, as CW shows it');

  // ★★ beta92 a SKIMMED beacon (cwskim — Stuart's RS-44 pass 11-23, the beacon ~2 kHz below the prediction): the box lists
  //    the skimmer's signals in its 24 kHz, nearest the prediction first, each with its offset from it
  console.log('— beta92 a SKIMMED CW beacon (the CW skimmer over 24 kHz around the prediction)');
  await ev(`window.__p.onPicked({ norad: 44909, tx: 1, decoder: 'cwskim', vfob: [0] }); window.__p.onReadout({ state: 'TRACK', name: 'RS-44', norad: 44909, tx: 'CW beacon', nominal: 435605000, offset: 4010, rate: -40, time: 'ok' }); true`);
  const skSt = await ev(`document.querySelector('#satPanel [data-s="state"]').textContent`);
  ok(/CW skimmer listening 24 kHz around the beacon/.test(skSt) && !/packets/.test(skSt), `a skimmed beacon says its skimmer listens 24 kHz around it ("${skSt}")`);
  ok(await ev(`document.querySelector('#satPanel .sat-pk').hidden && document.querySelector('#satPanel .sat-cw').textContent === 'no CW in the 24 kHz around the beacon yet'`),
     'nothing read yet: says so (no "no packets yet")');
  await ev(`window.__p.beaconList({ centre: 435609010, signals: [
    { hz: 435613500, text: 'CQ DE G4ABC', active: true, wpm: 24 },
    { hz: 435606910, text: 'RS44 RS44 BEACON TLM 123', active: true, wpm: 20 },
    { hz: 435609200, text: '', active: true },
    { hz: 435609360, text: 'E E', active: false } ] }); true`);
  const bt = await ev(`document.querySelector('#satPanel .sat-cw').textContent`);
  ok(bt === '+350 Hz \u00b7 E E\n\u22122.1 kHz \u00b7 RS44 RS44 BEACON TLM 123\n+4.5 kHz \u00b7 CQ DE G4ABC', `nearest the prediction first, each with its offset (${JSON.stringify(bt)})`);
  ok(!/435609200/.test(bt) && bt.split('\n').length === 3, 'a carrier with no text yet is not listed');
  await ev(`window.__p.cwText('IGNORED'); true`);
  ok(await ev(`document.querySelector('#satPanel .sat-cw').textContent`) === bt, 'one-signal CW text is not the skimmed beacon\'s');
  await shot('beacon-skim-desktop');
  await phone(); await sleep(400);
  { const f2 = await fits(); ok(fitOk(f2), `390 px with a skimmed beacon: fits, no sideways scroll (${JSON.stringify(f2)})`); }
  await shot('beacon-skim-phone');
  await desk(); await sleep(200);
  // …and a beacon opened in VFO B is skimmed too: B's area lists ITS window
  await ev(`window.__p.onPicked({ norad: 44909, tx: 0, decoder: '', vfob: [1] }); window.__p.onVfoB(__Sat.parseSatVfoB({ type: 'sat_vfob', on: true, norad: 44909, tx: 1, hz: 435605000, mode: 'cw', use: 'packet', label: 'CW beacon', decoder: 'cwskim', params: { width: 24000 }, shift: -4744.6 })); true`);
  const vbs = await ev(`document.querySelector('#satPanel .sat-vb .sat-ln')?.textContent || ''`);
  ok(/CW skimmer listening 24 kHz around the beacon$/.test(vbs), `B's line: "${vbs}"`);
  ok(await ev(`window.__p.vfobNow()?.params?.width === 24000`), 'B carries the skimmer\'s width (its socket joins with it)');
  await ev(`window.__p.bBeaconList({ centre: 435600255, signals: [{ hz: 435598050, text: 'RS44 BEACON', active: true }] }); true`);
  const bb = await ev(`document.querySelector('#satPanel .sat-cw').hidden ? '' : document.querySelector('#satPanel .sat-cw').textContent`);
  ok(bb === '\u22122.2 kHz \u00b7 RS44 BEACON', `B's area: its skimmer's signal and offset (${JSON.stringify(bb)})`);
  await ev(`window.__p.onVfoB(__Sat.parseSatVfoB({ type: 'sat_vfob', on: false, norad: 44909, tx: 1, why: 'VFO B was switched off' })); true`);

  console.log('— beta92 the line switch: ISS 145.800 SSTV on A + 145.825 APRS on B');
  await ev(`window.__p.onPicked({ norad: 25544, tx: 1, decoder: 'sstv', vfob: [2] }); window.__p.onReadout({ state: 'TRACK', name: 'ISS', norad: 25544, tx: 'Voice / SSTV', nominal: 145800000, offset: 2210, rate: -40, time: 'ok' }); true`);
  ok(await ev(`document.querySelector('#satPanel [data-act="vfob"]')?.textContent`) === 'OPEN IN VFO B · APRS', 'APRS offered');
  await ev(`window.__p.onVfoB(__Sat.parseSatVfoB({ type: 'sat_vfob', on: true, norad: 25544, tx: 2, hz: 145825000, mode: 'nfm', use: 'packet', label: 'APRS', decoder: 'aprs', params: { packet: '1200', baud_rate: 1200 } })); true`);
  await ev(`window.__p.bPacket('RS0ISS>APRS: ARISS'); true`);
  const s1 = JSON.parse(await ev(`JSON.stringify({ sw: document.querySelectorAll('#satPanel .sat-ln.sw').length, on: document.querySelector('#satPanel .sat-ln.sw.on')?.dataset.v,
    pk: !document.querySelector('#satPanel .sat-pk').hidden && document.querySelector('#satPanel .sat-pk').textContent, a: document.querySelector('#satPanel [data-s="state"]').textContent })`));
  ok(s1.sw === 2 && s1.on === 'B' && /RS0ISS>APRS: ARISS/.test(s1.pk || ''), `both lines tappable, B shown: its packets (${JSON.stringify(s1)})`);
  ok(/^VFO A · /.test(s1.a), `A's line says it is VFO A (${s1.a})`);
  await shot('vfob-switch-b');
  await ev(`document.querySelector('#satPanel [data-s="state"]').click(); window.__p.pictureStart(); true`);
  const s2 = JSON.parse(await ev(`JSON.stringify({ view: window.__p.viewNow(), pk: document.querySelector('#satPanel .sat-pk').hidden, img: !document.querySelector('#satPanel [data-s="img"]').hidden,
    lent: !!document.querySelector('#satPanel #decImgWrap'), big: document.getElementById('satPanel').classList.contains('big') })`));
  ok(s2.view === 'A' && s2.pk && s2.img && s2.lent && s2.big, `a tap on A's line: A's picture (the lent canvas), B's packets hidden (${JSON.stringify(s2)})`);
  await shot('vfob-switch-a');
  await ev(`document.querySelector('#satPanel .sat-vb .sat-ln').click(); true`);
  const s3 = JSON.parse(await ev(`JSON.stringify({ view: window.__p.viewNow(), pk: !document.querySelector('#satPanel .sat-pk').hidden, img: document.querySelector('#satPanel [data-s="img"]').hidden })`));
  ok(s3.view === 'B' && s3.pk && s3.img, `a tap on B's line: B's packets again, A's picture hidden — only what the area shows changed (${JSON.stringify(s3)})`);
  ok(JSON.stringify((await ev('window.__sent')).filter((m) => m.op === 'pick' || m.vfob)) === '[{"vfob":[44909,1]}]', 'switching sends nothing (no pick, no retune)');
  await ev(`window.__p.onVfoB(__Sat.parseSatVfoB({ type: 'sat_vfob', on: false, norad: 25544, tx: 2, why: 'VFO B was switched off' })); true`);
  const s4 = JSON.parse(await ev(`JSON.stringify({ sw: document.querySelectorAll('#satPanel .sat-ln.sw').length, view: window.__p.viewNow(), img: !document.querySelector('#satPanel [data-s="img"]').hidden, a: document.querySelector('#satPanel [data-s="state"]').textContent })`));
  ok(s4.sw === 0 && s4.view === 'A' && s4.img && !/^VFO A/.test(s4.a), `B closed: the switch ends, A's box exactly as before (${JSON.stringify(s4)})`);
  await phone(); await sleep(300); f = await fits(); ok(fitOk(f), `390 px after: fits (${JSON.stringify(f)})`); await desk();
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
