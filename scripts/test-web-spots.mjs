// test-web-spots.mjs — V12 DIGITAL SPOTS IN THE WEB CLIENT, through a real vibeserver and the REAL built web page
// (2026-10-26). A fake rtl_tcp replays the UTC-anchored 20 m clip (`test-digi-spots clip`: FT8, FT4, FT2 and JS8 stations
// on their own dials, and since 2026-10-27 a JTTY CQ and a PSK31 CQ); the server's dial is 20 m's Band centre. Headless Edge then:
//   • both buttons are offered — DIGITAL SPOTS: SINGLE MODE and DIGITAL SPOTS: BAND (vibeserver.json lists 20 m);
//   • BAND: the readout names the band ("20 m BAND"), the box title says so, and spots arrive in the list with a MODE
//     column in each mode's colour and letter — FT8, FT4, FT2 and JS8 from their own dials;
//   • the tuning arrow steps BANDS (20 m → 17 m: the dial moves to 17 m's centre, the readout follows) and back;
//   • the MAP is coloured by MODE: every marker's fill and letter is its mode's (src/utils/digiModes.ts), and the legend
//     lists the modes this server decodes;
//   • the box header carries a chip per mode (V12 Band governor: active / light / idle / shed / off);
//   • the box header SCROLLS at 1440 px and 375 px (wheel, trackpad, finger, the › arrow; ✕ reachable), its title is one
//     line, and its MAP key opens the spots map (2026-10-27);
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
//   Screenshots go to VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_DIGI_BIN=…/test-digi-spots node scripts/test-web-spots.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '', DIGI = process.env.VIBE_DIGI_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !DIGI || !fs.existsSync(DIGI)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_DIGI_BIN)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000, CENTRE = 14083700;   // ★ 20 m's Band centre with FT8 FT4 FT2 JS8 WSPR JTTY PSK registered
// ★ The modes' styles, from the one table both clients draw with.
const STYLE = { FT8: { colour: '#E69F00', letter: '8' }, FT4: { colour: '#56B4E9', letter: '4' },
                FT2: { colour: '#F0E442', letter: '2' }, JS8: { colour: '#009E73', letter: 'J' }, WSPR: { colour: '#CC79A7', letter: 'W' },
                JTTY: { colour: '#FFFFFF', letter: 'T' }, PSK: { colour: '#AA3377', letter: 'P' } };   // ★ V12 2026-10-27
const styleOf = (m) => STYLE[m] || STYLE[String(m).replace(/[0-9]+$/, '')];   // ★ PSK31 / PSK63 → PSK
const styleSrc = fs.readFileSync(path.join(root, 'src/utils/digiModes.ts'), 'utf8');
for (const [m, s] of Object.entries(STYLE)) if (!styleSrc.includes(`${m}: `) || !styleSrc.includes(`colour: '${s.colour}'`)) { console.log(`   FAIL the test's ${m} style is not digiModes.ts's`); process.exit(1); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webspots-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const CLIP = path.join(dir, '20m.cu8');
execFileSync(DIGI, ['clip', CLIP, String(RATE), String(CENTRE)], { stdio: 'ignore' });
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Spots web test', locator: 'IO92nh', adminPass: 'wsp-admin',
  radios: [{ serial: 'wsp', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE), '--hf-utc'], { stdio: 'ignore' });
await sleep(400);
// ★ The GPU map's files from the repo (as the .deb installs them), so the map window can run its GPU renderer here too.
const env = { ...process.env, VIBESERVER_MAPGL_DIR: path.join(root, 'directory/public/mapgl'), VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webspots-edge-'));
let edge = null;
// ★ A watchdog: a CDP call that never answers must FAIL the run, never leave Edge, the server and the fake radio running.
const watchdog = setTimeout(() => { console.log('   FAIL driver: no answer within 300 s');
  for (const c of [edge, srv, rtl]) { try { c?.kill('SIGKILL'); } catch { /* gone */ } }
  try { fs.rmSync(dir, { recursive: true, force: true }); fs.rmSync(prof, { recursive: true, force: true }); } catch { /* best effort */ }
  process.exit(1); }, 300000);
// ★ And a memory ceiling for the driver itself.
const rssGuard = setInterval(() => { if (process.memoryUsage().rss > 1024 * 1024 * 1024) { console.log('   FAIL driver RSS over 1 GB'); clearTimeout(watchdog);
  for (const c of [edge, srv, rtl]) { try { c?.kill('SIGKILL'); } catch { /* gone */ } } process.exit(1); } }, 2000);
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.spotsBand === true && (st.spotBands || []).some((b) => b.id === '20m'), `vibeserver.json: spotsBand ${st.spotsBand}, ${(st.spotBands || []).length} bands`);

  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', `--user-data-dir=${prof}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (x, ms) => { for (let t = 0; t < ms; t += 250) { if (await ev(x)) return true; await sleep(250); } return !!(await ev(x)); };
  const shot = async (name) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    const p = path.join(SHOTS, name);
    fs.writeFileSync(p, Buffer.from(r.result.data, 'base64'));
    return p;
  };
  const trustedClick = async (sel) => {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || {x:0,y:0,width:0,height:0})`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await trustedClick('#audioGate button');   // ★ a trusted click starts audio (silent: --mute-audio)
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000),
     'START RADIO pressed (the splash is gone)');
  await sleep(2500);

  const freqHz = async () => {
    const main = await ev(`document.getElementById('mFreq').textContent + '|' + document.getElementById('mFreqFine').textContent`);
    const [a, b] = String(main).split('|'); return Math.round((parseFloat(a) * 1e6) + (parseInt(b || '0', 10) * 10));
  };
  const chan = () => ev(`(() => { const c = document.getElementById('mChan'); return c && !c.hidden ? c.textContent : ''; })()`);

  /* ★★ THE MODE MENU DRAWS NO DEAD WHOLE-RADIO MODES (Stuart, 2026-10-27, his RSP1B: "those dead buttons can be hidden").
   *  ADS-B / ISM / NDB / AIS are drawn exactly where vibeserver.json says the mode can run (true); a refusal (false, with
   *  its reason) and an older server's silence both draw nothing. This server runs at 0.96 MS/s, below ADS-B's 2.4. */
  console.log('── the mode menu: no dead modes ──');
  await ev(`document.getElementById('mMode').click()`);
  ok(await until(`!!document.querySelector('#mModeMenu .mModeOpt')`, 4000), 'the mode menu opened');
  const opts = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#mModeMenu .mModeOpt')].map(b => b.textContent.trim()))`));
  for (const [k, label] of [['adsb', 'ADS-B'], ['ism', 'ISM'], ['ndb', 'NDB'], ['ais', 'AIS']]) {
    ok(opts.includes(label) === (st[k] === true), `${label}: ${st[k] === true ? 'can run here — drawn' : `refused or absent (${st[k]}${st[k + 'Why'] ? ': ' + st[k + 'Why'] : ''}) — not drawn`}`);
  }
  ok(await ev(`!document.querySelector('#mModeMenu .mModeOpt.dis')`), `nothing in the menu is drawn greyed (${opts.join(' ')})`);
  await ev(`(() => { const m = document.getElementById('mModeMenu'); const x = m && [...m.querySelectorAll('button')].find(b => /CLOSE|DONE|✕/.test(b.textContent)); if (x) x.click(); else m?.remove(); })()`);
  await sleep(300);

  console.log('── the two buttons ──');
  ok(await until(`!document.getElementById('spotsBandBtn').hidden`, 8000), 'DIGITAL SPOTS: BAND is offered (this radio reaches 20 m)');
  ok(await ev(`document.getElementById('spotsBtn').textContent.trim()`) === 'DIGITAL SPOTS: SINGLE MODE', 'the old button is DIGITAL SPOTS: SINGLE MODE');
  ok(!(await ev(`document.getElementById('spotsBandBtn').classList.contains('dis')`)), 'Band is not greyed (the owner has it on)');

  console.log('── BAND ──');
  await ev(`document.getElementById('spotsBandBtn').click()`);
  ok(await until(`document.getElementById('spotsBandBtn').classList.contains('on') && !document.getElementById('spotsBtn').classList.contains('on')`, 3000), 'BAND lit, SINGLE MODE not');
  ok(await until(`/20 M BAND/.test(document.getElementById('decTitle').textContent)`, 3000), `the box title: "${await ev(`document.getElementById('decTitle').textContent`)}"`);
  ok(await until(`/20 m BAND/.test((document.getElementById('mChan')||{}).textContent||'')`, 5000), `the readout names the band: "${await chan()}"`);
  ok(await until(`['FT8', 'FT4', 'FT2', 'JS8', 'JTTY', 'PSK31'].every(md => [...document.querySelectorAll('#spotList .sres.spot .mode')].some(m => m.textContent.includes(md)))`, 60000),
     `spots of all SIX modes on the air listed (JTTY and PSK31 among them) (${await ev(`[...document.querySelectorAll('#spotList .sres.spot')].map(r => r.querySelector('.mode').textContent + ' ' + r.querySelector('.call').textContent).join(', ')`)})`);
  const rows = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#spotList .sres.spot')].map(r => ({ mode: r.querySelector('.mode').textContent.replace(/^./, ''), letter: r.querySelector('.mode i').textContent,
      bg: getComputedStyle(r.querySelector('.mode i')).backgroundColor, call: r.querySelector('.call').textContent, band: r.querySelector('.band').textContent })))`));
  const hex = (c) => '#' + (c.match(/\d+/g) || []).slice(0, 3).map((x) => Number(x).toString(16).padStart(2, '0')).join('').toUpperCase();
  ok(rows.length >= 8 && rows.every((r) => styleOf(r.mode) && r.letter === styleOf(r.mode).letter && hex(r.bg) === styleOf(r.mode).colour && r.band === '20m'),
     `every row's MODE column in its mode's colour and letter: ${rows.map((r) => `${r.mode}/${r.letter}/${hex(r.bg)} ${r.call}`).join(', ')}`);
  ok(!rows.some((r) => r.call === 'ZL3LSB'), 'the lower-sideband station is never listed');
  // ★ M7EIW: the time is HH:MM:SS UTC — the slot's start (FT8/JS8 :00/:15/:30/:45; FT4 7.5 s → :00/:07/:15/:22 …;
  //   FT2 3.75 s → :00/:03/:07/:11 within each 15 s)
  const times = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#spotList .sres.spot')].map(r => [r.querySelector('.mode').textContent.replace(/^./, ''), r.querySelector('.t').textContent]))`));
  const onGrid = (m, t) => { const sec = Number(t.slice(6)) % 15; return m === 'FT8' || m === 'JS8' ? sec === 0 : m === 'FT4' ? sec === 0 || sec === 7 : m === 'FT2' ? [0, 3, 7, 11].includes(sec) : true; };
  ok(times.length >= 6 && times.every(([m, t]) => /^\d\d:\d\d:\d\d$/.test(t) && onGrid(m, t)),
     `every row's time is HH:MM:SS on its mode's slot grid: ${times.map(([m, t]) => m + ' ' + t).join(', ')}`);
  // ★★ V12 BAND GOVERNOR: a chip per mode in the box header — its letter in its colour, and what it is doing
  ok(await until(`document.querySelectorAll('#spotModeChips .smc').length === 7 && !document.getElementById('spotModeChips').hidden`, 20000),
     `the mode chips: ${await ev(`[...document.querySelectorAll('#spotModeChips .smc')].map(c => c.textContent).join(' · ')`)}`);
  const chips = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#spotModeChips .smc')].map(c => ({ m: c.dataset.mode, st: c.className.replace('smc ', ''),
      l: c.querySelector('i').textContent, bg: getComputedStyle(c.querySelector('i')).backgroundColor, t: c.title })))`));
  ok(chips.every((c) => styleOf(c.m) && c.l === styleOf(c.m).letter && hex(c.bg) === styleOf(c.m).colour && ['active', 'light', 'idle', 'shed', 'off'].includes(c.st) && c.t.length > 8),
     `every chip in its mode's colour and letter, with a state and its words (${chips.map((c) => c.m + ':' + c.st).join(' ')})`);
  ok(chips.some((c) => c.m === 'FT8' && (c.st === 'active' || c.st === 'light')) && chips.some((c) => c.m === 'WSPR' && c.st === 'idle'),
     'FT8 (stations on air) decoding, WSPR (nothing in its window) idle');
  const shotBand = await shot('spots-band-desktop.png');

  /* ★★ THE BOX HEADER (Stuart, 2026-10-27: "i also cannot scroll the header" — Band's mode chips ran off the end of a
   *  560 px box, which only became a scroller below 420 px, so they, MINIMISE and ✕ were cut off; and "the header needs
   *  a map button too"). At a wide window AND a phone: the row is a scroller, the title is one line, a WHEEL, the ›
   *  arrow and (phone) a finger all move it, the last control (✕) can be scrolled into the box, and MAP is there and
   *  opens the spots map. Screenshots go to SHOTS. */
  console.log('── the box header scrolls; MAP in it ──');
  /** A CDP call with a deadline: null if the browser has not answered by then (never hang the run). */
  const sendT = (method, params, ms) => Promise.race([send(method, params), sleep(ms).then(() => null)]);
  // ★ A HIDDEN headless page runs no smooth scroll and no scroll gesture (visibilityState was 'hidden' here): bring it
  //   to the front, or the › arrow and a finger drag "fail" for a reason that is the browser's, not the page's.
  await sendT('Page.bringToFront', {}, 5000);
  await sendT('Emulation.setFocusEmulationEnabled', { enabled: true }, 5000);
  await sleep(300);
  const head = async () => JSON.parse(await ev(`JSON.stringify((() => { const h = document.getElementById('decBoxHead'), b = document.getElementById('decBox').getBoundingClientRect(),
      t = document.getElementById('decTitle').getBoundingClientRect(), x = document.getElementById('decHide').getBoundingClientRect(), m = document.getElementById('sfMap');
      return { ox: getComputedStyle(h).overflowX, sw: h.scrollWidth, cw: h.clientWidth, sl: h.scrollLeft, titleH: t.height, lineH: parseFloat(getComputedStyle(document.getElementById('decTitle')).lineHeight) || 16,
               xIn: x.left >= b.left - 1 && x.right <= b.right + 1, map: !!m && !m.hidden && !!m.offsetParent, boxW: b.width,
               mid: { x: (h.getBoundingClientRect().left + h.getBoundingClientRect().right) / 2, y: (h.getBoundingClientRect().top + h.getBoundingClientRect().bottom) / 2 } }; })())`));
  const headerAt = async (w, h, mobile, tag) => {
    await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: mobile ? 2 : 1, mobile });
    await send('Emulation.setTouchEmulationEnabled', { enabled: mobile, maxTouchPoints: mobile ? 5 : 1 });
    await sleep(1200);
    await ev(`document.getElementById('decBoxHead').scrollLeft = 0`);
    await sleep(200);
    let s = await head();
    ok(s.ox === 'auto' && s.sw > s.cw + 20, `${tag}: the header is a scroller and has more than fits (box ${Math.round(s.boxW)} px, row ${s.sw} > ${s.cw})`);
    ok(s.titleH <= s.lineH * 1.5, `${tag}: the title is one line (${Math.round(s.titleH)} px; it stood one word per line)`);
    ok(s.map, `${tag}: MAP is in the header (spots with grids are listed)`);
    await shot(`spots-header-${w}.png`);
    if (!mobile) {
      // ★ The wheel as a page event: headless Edge's Input.dispatchMouseEvent 'mouseWheel' never answered here (it waits
      //   for a compositor ack), and a hung driver is worse than a narrower test. The handler is what is under test.
      await ev(`document.getElementById('decBoxHead').dispatchEvent(new WheelEvent('wheel', { deltaY: 240, bubbles: true, cancelable: true }))`);
      await sleep(300);
      const s2 = await head();
      ok(s2.sl > 50, `${tag}: a mouse wheel scrolls it sideways (scrollLeft 0 → ${Math.round(s2.sl)})`);
      await ev(`document.getElementById('decBoxHead').scrollLeft = 0`);
      // ★ A trackpad's sideways swipe: the browser's own scrolling, which only works on a real scroll container.
      const g = await sendT('Input.synthesizeScrollGesture', { x: Math.round(s.mid.x), y: Math.round(s.mid.y), xDistance: -240, yDistance: 0, gestureSourceType: 'mouse', speed: 1600 }, 15000);
      await sleep(400);
      if (g === null) console.log(`   (not run — ${tag}: the browser did not answer a sideways scroll gesture)`);
      else ok((await head()).sl > 50, `${tag}: a sideways (trackpad) scroll moves it (scrollLeft ${Math.round((await head()).sl)})`);
    } else {
      // ★ A real finger through the input pipeline: down, eight moves leftward, up.
      const fy = Math.round(s.mid.y), fx0 = Math.round(s.mid.x + 120);
      await sendT('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: fx0, y: fy }] }, 5000);
      for (let i = 1; i <= 8; i++) { await sendT('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: fx0 - i * 30, y: fy }] }, 5000); await sleep(16); }
      await sendT('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] }, 5000);
      await sleep(400);
      ok((await head()).sl > 50, `${tag}: a finger drag scrolls it (scrollLeft ${Math.round((await head()).sl)})`);
    }
    await ev(`document.getElementById('decBoxHead').scrollLeft = 0`);
    await sleep(300);
    const arrDiag = await ev(`(() => { const a = document.querySelector('.decHeadArr.right'), r = a.getBoundingClientRect(), e = document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2);
        return document.getElementById('decHeadWrap').className + ' disp=' + getComputedStyle(a).display + ' at=' + (e ? e.tagName + '.' + e.className + '#' + e.id : 'none'); })()`);
    await trustedClick('.decHeadArr.right');
    await sleep(1500);
    ok((await head()).sl > 50, `${tag}: the › arrow scrolls it (scrollLeft ${Math.round((await head()).sl)}; ${arrDiag})`);
    await ev(`(() => { const h = document.getElementById('decBoxHead'); h.scrollLeft = h.scrollWidth; })()`);
    await sleep(300);
    ok((await head()).xIn, `${tag}: scrolled to the end, ✕ (the last control) is inside the box`);
    await shot(`spots-header-${w}-end.png`);
    await ev(`document.getElementById('decBoxHead').scrollLeft = 0`);
  };
  await headerAt(1440, 900, false, '1440 px');
  await headerAt(375, 812, true, '375 px');
  // ★ The listener's control colours (MENU › CONTROLS): a green key, white text, fully solid — MAP must match its neighbours.
  await ev(`(() => { const c = (id, i) => document.querySelectorAll('#' + id + ' button')[i]?.click(); c('ctlBtnSw', 1); c('ctlFontSw', 1);
      const s = document.getElementById('ctlSolid'); s.value = '100'; s.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await sleep(500);
  ok(await ev(`(() => { const a = getComputedStyle(document.getElementById('sfMap')), b = getComputedStyle(document.getElementById('sfAge'));
      return a.backgroundColor === b.backgroundColor && a.color === b.color && a.borderColor === b.borderColor && a.fontFamily === b.fontFamily; })()`),
     'with the listener\'s own control colours, MAP looks exactly like its neighbours (same background, text, border, font)');
  await shot('spots-header-375-colours.png');
  await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
  await send('Emulation.setTouchEmulationEnabled', { enabled: false });
  await sleep(800);
  await shot('spots-header-1440-colours.png');
  await ev(`document.getElementById('ctlReset').click()`);
  // ★ MAP in the header opens the spots map (a real click: the browser lets only a gesture open a window).
  {
    const before2 = new Set((await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).map((t) => t.id));
    await trustedClick('#sfMap');
    let t2 = null;
    for (let i = 0; i < 40 && !t2; i++) { await sleep(250); t2 = (await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).find((t) => t.type === 'page' && !before2.has(t.id)); }
    ok(!!t2, `the header's MAP opened the spots map${t2 ? ` ("${t2.title || t2.url}")` : ''}`);
    if (t2) await fetch(`http://127.0.0.1:${cdp}/json/close/${t2.id}`).catch(() => {});
  }
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await sleep(800);

  console.log('── the arrows step bands ──');
  await trustedClick('#mVfoUp');
  ok(await until(`/17 m BAND/.test((document.getElementById('mChan')||{}).textContent||'')`, 5000), `▲: 17 m ("${await chan()}")`);
  const f17 = await freqHz();
  const b17 = st.spotBands.find((b) => b.id === '17m');
  ok(b17 && Math.abs(f17 - b17.hz) <= 20, `the dial is 17 m's centre (${f17} vs ${b17 && b17.hz})`);
  await trustedClick('#mVfoDown');
  ok(await until(`/20 m BAND/.test((document.getElementById('mChan')||{}).textContent||'')`, 5000), `▼: back to 20 m ("${await chan()}")`);
  ok(Math.abs((await freqHz()) - CENTRE) <= 20, `the dial is 20 m's centre again (${await freqHz()})`);

  console.log('── the map, coloured by mode ──');
  const before = new Set((await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).map((t) => t.id));
  await ev(`document.getElementById('mapBtn').click()`);
  let mapT = null;
  for (let i = 0; i < 40 && !mapT; i++) { await sleep(250); mapT = (await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).find((t) => t.type === 'page' && !before.has(t.id)); }
  ok(!!mapT, 'the map window opened');
  if (mapT) {
    const mws = new WebSocket(mapT.webSocketDebuggerUrl);
    let mid = 0; const mp = new Map();
    mws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && mp.has(m.id)) { mp.get(m.id)(m); mp.delete(m.id); } };
    await new Promise((r) => { mws.onopen = r; });
    const msend = (method, params = {}) => new Promise((res) => { const i = ++mid; mp.set(i, res); mws.send(JSON.stringify({ id: i, method, params })); });
    const mev = async (x) => (await msend('Runtime.evaluate', { expression: x, returnByValue: true })).result?.result?.value;
    for (let i = 0; i < 40; i++) { if (await mev(`typeof spots !== 'undefined' && document.querySelectorAll('#legend .lrow[data-mode]').length >= 7`)) break; await sleep(250); }
    const pts = JSON.parse(await mev(`JSON.stringify(spots.map(s => ({ m: s.mode, c: s.colour, l: s.letter })))`) || '[]');
    ok(pts.length >= 3 && pts.every((p) => styleOf(p.m) && p.c.toUpperCase() === styleOf(p.m).colour && p.l === styleOf(p.m).letter),
       `every marker is its MODE's colour and letter (${pts.map((p) => `${p.m}:${p.c}:${p.l}`).join(' ')})`);
    const legend = JSON.parse(await mev(`JSON.stringify([...document.querySelectorAll('#legend .lrow[data-mode]')].map(r => ({ m: r.dataset.mode, l: r.querySelector('.mk').textContent, bg: getComputedStyle(r.querySelector('.mk')).backgroundColor })))`) || '[]');
    ok(legend.map((x) => x.m).join(' ') === 'FT8 FT4 FT2 JS8 WSPR JTTY PSK' && legend.every((x) => x.l === STYLE[x.m].letter && hex(x.bg) === STYLE[x.m].colour),
       `the legend lists the server's modes with their colours and letters (${legend.map((x) => `${x.m}/${x.l}/${hex(x.bg)}`).join(' ')})`);
    ok(/BY MODE/.test(await mev(`document.getElementById('stats').textContent`)), 'the stats panel counts BY MODE');
    // ★ On the GPU map the letter rides the circle layer (vibemapgl-compat 'lc-circle-labels'); on the Leaflet fallback (no
    //   WebGL 2 in this headless browser) the legend and the colours are the cue.
    const glKit = await mev(`typeof GL !== 'undefined' && !!GL`);
    for (let i = 0; glKit && i < 40; i++) { if (await mev(`typeof vm !== 'undefined' && !!vm && !!vm.map && !!vm.map.getLayer('lc-circle-labels')`)) break; await sleep(250); }
    const gl = glKit && await mev(`typeof vm !== 'undefined' && !!vm && !!vm.map && !!vm.map.getLayer('lc-circle-labels')`);
    if (glKit) {
      ok(gl, 'GPU map: the letter layer (lc-circle-labels) is on the map');
      const feats = JSON.parse(await mev(`JSON.stringify((vm.map.getSource('lc-circles')._data.features || []).map(f => [f.properties.fillColor, f.properties.label, f.properties.labelColor]))`) || '[]');
      ok(feats.length >= 3 && feats.every(([c, l]) => Object.values(STYLE).some((st) => st.colour === String(c).toUpperCase() && st.letter === l)),
         `GPU map: every circle carries its mode's fill and letter (${feats.map((f) => f.join('/')).join(' ')})`);
    } else {
      // ★ The Leaflet fallback (no WebGL 2 in this headless browser): its canvas circles carry no text, so the letters ride
      //   small non-interactive markers above them.
      const ls = JSON.parse(await mev(`JSON.stringify([...document.querySelectorAll('.mkl b')].map(b => [b.textContent, getComputedStyle(b).color]))`) || '[]');
      ok(ls.length >= 3 && ls.every(([l]) => Object.values(STYLE).some((st) => st.letter === l)), `Leaflet fallback: each marker carries its mode's letter (${ls.map((x) => x[0]).join(' ')})`);
    }
    const r = await msend('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(path.join(SHOTS, 'spots-map-by-mode.png'), Buffer.from(r.result.data, 'base64'));
    mws.close();
    await fetch(`http://127.0.0.1:${cdp}/json/close/${mapT.id}`).catch(() => {});
  }

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#spotList .sres.spot')].map(r => r.getBoundingClientRect().right)),
    rows: document.querySelectorAll('#spotList .sres.spot').length })`));
  ok(fit.doc <= 391 && fit.rowMax <= 391 && fit.rows >= 1, `nothing scrolls sideways (page ${fit.doc}, rows end ${Math.round(fit.rowMax)} px, ${fit.rows} rows)`);
  const shotPhone = await shot('spots-band-phone.png');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });

  console.log('── back to SINGLE MODE ──');
  await ev(`document.getElementById('spotsBtn').click()`);
  ok(await until(`document.getElementById('spotsBtn').classList.contains('on') && !document.getElementById('spotsBandBtn').classList.contains('on')`, 3000), 'SINGLE MODE lit, BAND not');
  ok(await ev(`document.getElementById('spotModeChips').hidden`), 'Single: the mode chips are gone (they are Band\'s)');
  ok(await until(`!/BAND/.test((document.getElementById('mChan')||{}).textContent||'') && document.getElementById('decTitle').textContent.includes('DIGITAL SPOTS') && !/BAND/.test(document.getElementById('decTitle').textContent)`, 8000),
     `the readout no longer names a band ("${await chan()}"); the box is "${await ev(`document.getElementById('decTitle').textContent`)}"`);
  await trustedClick('#mVfoUp');
  await sleep(600);
  ok(Math.abs((await freqHz()) - CENTRE) < 20000, `▲ in Single tunes by the step again, not a band (${await freqHz()})`);
  ok(/digital spots: Band started/.test(fs.readFileSync(logPath, 'utf8')) && /digital spots: Single mode started/.test(fs.readFileSync(logPath, 'utf8')),
     'the server ran Band, then Single');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[shotBand, path.join(SHOTS, 'spots-map-by-mode.png'), shotPhone].join('  ')}`);
} finally {
  clearTimeout(watchdog); clearInterval(rssGuard);
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(300);
  fs.rmSync(dir, { recursive: true, force: true });
  fs.rmSync(prof, { recursive: true, force: true });
}
console.log(`\n${fail ? '✗' : '✓'} test-web-spots: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
