// test-web-cw.mjs — V12 CW IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (2026-10-25).
//   A fake rtl_tcp replays the synthetic HF clip (`cw_bench clip`: five CW stations around 7.030 MHz, each calling
//   "CQ CQ DE <call> <call> K"); the server's dial is on G4ABC in CW. Headless Edge then:
//   • DECODERS shows CW (this server's vibeserver.json says "cw": true), and the gallery has a CW chip;
//   • ONE SIGNAL: G4ABC's text arrives in the decoder box, its row in the list is the primary, and it is spotted;
//   • SKIMMER (48 kHz): every station listed with its call, and every one spotted at its own kHz;
//   • at PHONE WIDTH (390 px) the rows wrap and nothing scrolls sideways; no exception on the page.
//   • V12 (2026-10-29): the speed reads "21 wpm" or a range "17–21 wpm"; PLAIN in the box's header lights, shows the rows
//     (and ONE SIGNAL's text) in plain English, reverts on a second press, and is remembered after a reload; a callsign
//     is a QRZ.COM link (qrz.com/db/<CALL>, a new tab — intercepted here, never loaded) whose click does NOT tune; the
//     mode menu's QRZ.COM ↗ and CW TERMS GUIDE keys (the guide opens, its search filters, nothing scrolls sideways at
//     390 px, CLOSE closes it).
//   Screenshots (desktop and 390 px, both views) are written to VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_CW_CLIP=…/clip.cu8 node scripts/test-web-cw.mjs
//   (make the clip: cw_bench clip clip.cu8 960000 40 — scripts/test-cw.sh does)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_CW_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_CW_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000, CENTRE = 7030000;
// ★ The clip's stations (tools/cw-bench/cw_bench.cpp, clip()): offset from CENTRE, call.
const STATIONS = [[600, 'G4ABC'], [-7200, 'DL1XYZ'], [9800, 'EA3QP'], [16300, 'OK1RR'], [-15600, 'K1ABC']];

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webcw-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'CW web test', locator: 'IO92nh', adminPass: 'wcw-admin',
  radios: [{ serial: 'wcw', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
// ★ The dial at the clip's centre in CW: every station is off it, so the SKIMMER sees all five; a tap on G4ABC's row then
//   tunes to it for ONE SIGNAL — the way a listener goes from the list to one station.
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'cwu', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webcw-edge-'));
// ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
//   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
process.on('exit', () => { try { fs.rmSync(prof, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
let edge = null;
const shotsV12 = [];
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.cw === true, `vibeserver.json: cw ${JSON.stringify(st.cw)}`);

  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
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

  console.log('── DECODERS: the CW button, SKIMMER (48 kHz) ──');
  ok(await until(`!document.querySelector('#decodersPanel [data-dec="cw"]').hidden`, 8000), 'the CW button is offered (this server has CW)');
  // ★ open DECODERS as a listener does (the mode card's door), else straight in
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     document.getElementById('decodersPanel').classList.add('open'); return false; })()`);
  await sleep(500);
  await ev(`document.querySelector('#decodersPanel [data-dec="cw"]').click()`);
  ok(await until(`!document.getElementById('cwSettings').hidden && document.getElementById('cwList').classList.contains('on')`, 5000),
     'CW chosen: its settings (ONE SIGNAL / SKIMMER) and the signals list are shown');
  await ev(`document.querySelector('#cwView [data-cwview="skim"]').click()`);
  ok(await until(`!document.getElementById('cwWidthRow').hidden && document.querySelector('#cwWidth .on')?.dataset.cwwidth === '48000'`, 3000),
     'SKIMMER shows the segment row, 48 kHz chosen');
  const settings = await shot('cw-settings-desktop.png');
  // ★ the panel closed, so the decoder box is what the screenshots show
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  const allSpotted = `(() => { const t = [...document.querySelectorAll('#cwSpots .cws')].map(r => r.textContent).join('|');
                               return ${JSON.stringify(STATIONS.map(([, c]) => c))}.every(c => t.includes(c)); })()`;
  ok(await until(allSpotted, 60000), `every station spotted (${await ev(`[...document.querySelectorAll('#cwSpots .cws .c')].map(c => c.textContent).join(' ')`)})`);
  await sleep(1500);   // ★ the list goes once a second: let the row that names the last call arrive
  // ★★ Scrolled down to SPOTS, the view stays there while the rows above are rebuilt (Stuart, 2026-10-25: "the new text
  //    above kept pulling the view up again"). A short box so it must scroll; three list updates.
  //    ★ AS SAFARI SEES IT: no native scroll anchoring (Edge's would hide the fault), and the content ABOVE grows — new
  //      signals arriving on a busy band — by a 90 px block before the list, after the view was set.
  await ev(`(() => { const b = document.getElementById('decBody'); b.style.maxHeight = '140px'; b.style.height = '140px';
                     b.style.overflowAnchor = 'none';
                     b.scrollTop = document.getElementById('cwSpotsHead').offsetTop - b.offsetTop;
                     b.dispatchEvent(new Event('scroll'));   // ★ headless fires none for a scripted scroll; a finger always does
                     return 1; })()`);
  await sleep(300);
  const headY = () => ev(`Math.round(document.getElementById('cwSpotsHead').getBoundingClientRect().top - document.getElementById('decBody').getBoundingClientRect().top)`);
  const y0 = await headY(), top0 = await ev(`document.getElementById('decBody').scrollTop`);
  await ev(`(() => { window.__cwRenders = 0; new MutationObserver(() => { window.__cwRenders++; }).observe(document.getElementById('cwRows'), { childList: true });
                     return 1; })()`);
  await ev(`(() => { const g = document.createElement('div'); g.id = 'cwGrowTest'; g.style.height = '90px';
                     const l = document.getElementById('cwList'); l.parentNode.insertBefore(g, l); return 1; })()`);
  await sleep(3200);
  const y1 = await headY(), top1 = await ev(`document.getElementById('decBody').scrollTop`);
  const renders = await ev(`window.__cwRenders`);
  ok(renders >= 2, `the list was redrawn while we waited (${renders} times) — so the check below means something`);
  ok(top0 > 0 && top1 > 0 && Math.abs(y1 - y0) <= 2, `scrolled to SPOTS, the view stays put through the list updates (heading at ${y0} → ${y1} px, scrollTop ${top0} → ${top1})`);
  await ev(`(() => { const b = document.getElementById('decBody'); b.style.maxHeight = ''; b.style.height = ''; b.style.overflowAnchor = '';
                     document.getElementById('cwGrowTest')?.remove(); b.scrollTop = 0; return 1; })()`);
  // ★★ Lowest frequency first, whatever the confidence; the message coloured by it (Stuart, 2026-10-25).
  const order = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#cwRows .cwr')].map(r => ({ f: parseFloat(r.querySelector('.f').textContent), q: (r.className.match(/q-(\\w+)/) || [])[1], col: getComputedStyle(r.querySelector('.tx')).color })))`));
  ok(order.length >= 2 && order.every((r, i) => i === 0 || r.f >= order[i - 1].f), `the list runs lowest frequency first (${order.map((r) => r.f).join(' ')})`);
  const want = { high: 'rgb(91, 227, 107)', medium: 'rgb(232, 197, 71)', low: 'rgb(255, 138, 61)', poor: 'rgb(255, 75, 75)' };
  ok(order.every((r) => want[r.q] === r.col), `each message in its confidence colour (${order.map((r) => r.q + ' ' + r.col).join(', ')})`);
  const rows = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#cwRows .cwr')].map(r => ({ f: r.querySelector('.f').textContent, c: r.querySelector('.c').textContent, tx: r.querySelector('.tx').textContent })))`));
  for (const [off, call] of STATIONS) {
    const r = rows.find((x) => x.c === call);
    ok(!!r && Math.abs(parseFloat(r.f) * 1e3 - (CENTRE + off)) <= 60 && r.tx.includes(`${call} ${call}`),
       `${call} listed at ${r ? r.f : '—'} kHz (sent at ${((CENTRE + off) / 1e3).toFixed(2)}), its text "${r ? r.tx.slice(-28) : ''}"`);
  }
  ok(!document_has_false(rows), `no row with an invented call (${rows.filter((x) => x.c !== '—').map((x) => x.c).join(' ')})`);
  const s2 = await shot('cw-skimmer-desktop.png');

  console.log('── V12: the speed, PLAIN, the QRZ links ──');
  const speeds = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#cwRows .cwr .w, #cwSpots .cws .w')].map(w => w.textContent))`));
  ok(speeds.length >= 5 && speeds.every((w) => /^\d+(\u2013\d+)? wpm$/.test(w)),
     `every speed reads "21 wpm" or "17–21 wpm" (${speeds.slice(0, 8).join(', ')}; ${speeds.filter((w) => w.includes('\u2013')).length} ranges)`);
  ok(await ev(`getComputedStyle(document.getElementById('cwPlain')).display !== 'none' && !document.getElementById('cwPlain').classList.contains('on')`),
     'PLAIN is in the CW box header, beside CLR, not lit');
  const rawTx = () => ev(`[...document.querySelectorAll('#cwRows .cwr')].map(r => r.querySelector('.tx').textContent).join(' | ')`);
  const tx0 = await rawTx();
  await ev(`document.getElementById('cwPlain').click()`);
  ok(await ev(`document.getElementById('cwPlain').classList.contains('on')`), 'PLAIN pressed: the key lights');
  const tx1 = await rawTx();
  ok(/calling anyone/.test(tx1) && /this is/.test(tx1) && / over\b/.test(tx1) && !/\bCQ\b/.test(tx1),
     `the rows read in plain English ("${tx1.slice(0, 70)}…")`);
  ok(/G4ABC G4ABC/.test(tx1), 'a callsign sent twice stays exactly as sent');
  ok(await ev(`[...document.querySelectorAll('#cwRows .cwr .c')].some(c => c.textContent === 'G4ABC')`), 'the call column is unchanged');
  await ev(`document.getElementById('cwPlain').click()`);
  ok(await ev(`!document.getElementById('cwPlain').classList.contains('on')`) && /\bCQ\b/.test(await rawTx()) && !/calling anyone/.test(await rawTx()),
     `pressed again: unlit, the CW as sent ("${(await rawTx()).slice(0, 50)}…"; before: "${tx0.slice(0, 30)}…")`);
  await ev(`document.getElementById('cwPlain').click()`);   // ★ left ON: ONE SIGNAL below, and the reload at the end
  ok(await ev(`(() => { try { return localStorage.getItem('vibe.cwPlain') === '1'; } catch { return false; } })()`), 'PLAIN saved in this browser');
  // ★ A callsign is a QRZ link. The click is intercepted (no navigation, no new tab — qrz.com is never loaded) and must
  //   not reach the row, whose click tunes.
  const link = await ev(`(document.querySelector('#cwRows .cwr .c a.qrz') || {}).href || ''`);
  ok(/^https:\/\/www\.qrz\.com\/db\/[A-Z0-9]+$/.test(link), `a row's callsign is a QRZ link (${link})`);
  ok(await ev(`[...document.querySelectorAll('#cwSpots .cws .c a.qrz')].length >= 5 && [...document.querySelectorAll('#cwSpots .cws .c a.qrz')].every(a => a.target === '_blank' && /noopener/.test(a.rel))`),
     'every spot\'s callsign is a QRZ link, opening a new tab (noopener)');
  const dialNow = () => ev(`document.getElementById('mFreq').textContent + '|' + (document.getElementById('mFreqFine')?.textContent || '')`);
  const dial0 = await dialNow();
  await ev(`(() => { window.__qrz = []; document.addEventListener('click', (e) => { const a = e.target.closest && e.target.closest('a.qrz');
                     if (a) { e.preventDefault(); window.__qrz.push(a.href); } }, true); return 1; })()`);
  await trustedClick('#cwRows .cwr:nth-child(2) .c a.qrz');
  await sleep(900);
  const qrzHits = JSON.parse(await ev(`JSON.stringify(window.__qrz)`));
  const dial1 = await dialNow();
  ok(qrzHits.length === 1 && /qrz\.com\/db\//.test(qrzHits[0]), `a tap on a callsign opens its QRZ page (${qrzHits.join(' ')})`);
  ok(dial1 === dial0, `…and does not tune (dial ${dial0} → ${dial1})`);

  console.log('── a tap on G4ABC\'s row tunes to it; ONE SIGNAL ──');
  await ev(`[...document.querySelectorAll('#cwRows .cwr')].find(r => r.querySelector('.c').textContent === 'G4ABC').click()`);
  await sleep(800);
  await ev(`document.querySelector('#cwView [data-cwview="one"]').click()`);
  ok(await until(`/G4ABC G4ABC/.test(document.getElementById('decText').textContent)`, 45000),
     `ONE SIGNAL: G4ABC's text in the decoder box ("${(await ev(`document.getElementById('decText').textContent`) || '').trim().slice(-48)}")`);
  ok(await until(`!!document.querySelector('#cwRows .cwr.primary')`, 10000), 'its row is the primary (highlighted)');
  const one = await ev(`(document.querySelector('#cwRows .cwr.primary')||{}).textContent || ''`);
  ok(/7030\.6\d/.test(one), `the primary row is G4ABC's carrier: ${one.slice(0, 90)}`);
  ok(await until(`[...document.querySelectorAll('#cwSpots .cws')].some(r => /G4ABC/.test(r.textContent))`, 30000), 'G4ABC spotted in SPOTS');
  const s1 = await shot('cw-one-signal-desktop.png');
  ok(await until(`getComputedStyle(document.getElementById('cwPlainText')).display === 'block' && getComputedStyle(document.getElementById('decText')).display === 'none'
                  && /calling anyone/.test(document.getElementById('cwPlainText').textContent)`, 8000),
     `PLAIN on, ONE SIGNAL: the text in plain English ("${(await ev(`document.getElementById('cwPlainText').textContent`) || '').trim().slice(-48)}")`);

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    box: document.getElementById('decBox').getBoundingClientRect().right,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#cwRows .cwr, #cwSpots .cws')].map(r => r.getBoundingClientRect().right)),
    rows: document.querySelectorAll('#cwRows .cwr').length })`));
  ok(fit.doc <= 391 && fit.box <= 391 && fit.rowMax <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)}, rows end ${Math.round(fit.rowMax)} px)`);
  ok(fit.rows >= 1, `the list is still there (${fit.rows} rows)`);
  const s3 = await shot('cw-one-signal-phone.png');
  await ev(`document.querySelector('#cwView [data-cwview="skim"]').click()`);
  ok(await until(`document.querySelectorAll('#cwRows .cwr').length >= 5`, 30000), 'SKIMMER again at phone width: the stations listed');
  const fit2 = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#cwRows .cwr, #cwSpots .cws')].map(r => r.getBoundingClientRect().right)) })`));
  ok(fit2.doc <= 391 && fit2.rowMax <= 391, `…and nothing scrolls sideways (page ${fit2.doc}, rows end ${Math.round(fit2.rowMax)} px)`);
  const s4 = await shot('cw-skimmer-phone.png');

  console.log('── V12: QRZ.COM ↗ and the CW TERMS GUIDE (390 px) ──');
  await ev(`document.getElementById('mMode').click()`);
  await sleep(400);
  ok(await ev(`(document.getElementById('mModeQrz') || {}).href === 'https://www.qrz.com/' && document.getElementById('mModeQrz').target === '_blank'`),
     'the mode menu has QRZ.COM ↗ (a new tab), under the Signal ID Wiki link');
  ok(await ev(`/signed in/.test(document.querySelector('#mModeMenu .mModeNote')?.textContent || '')`), '…with the "signed in" note');
  const menuFit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, right: document.getElementById('mModeMenu').getBoundingClientRect().right })`));
  ok(menuFit.doc <= 391 && menuFit.right <= 391, `the menu fits (page ${menuFit.doc}, menu ends ${Math.round(menuFit.right)} px)`);
  await ev(`document.getElementById('mModeCwTerms').click()`);
  ok(await until(`!!document.getElementById('cwGuide') && !document.getElementById('mModeMenu')`, 3000), 'CW TERMS GUIDE opens the guide (the menu closes)');
  const n0 = await ev(`document.querySelectorAll('#cwGuide .cwgTerm').length`);
  ok(n0 > 50 && await ev(`/readability/.test(document.getElementById('cwGuideList').textContent) && /EASILY CONFUSED/.test(document.getElementById('cwGuideList').textContent)`),
     `every term (${n0} entries), the RST line and the look-alikes`);
  await ev(`(() => { const i = document.getElementById('cwGuideSearch'); i.value = 'qth'; i.dispatchEvent(new Event('input')); return 1; })()`);
  const found = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#cwGuide .cwgCode')].map(c => c.textContent))`));
  ok(found.length >= 1 && found.length < 10 && found[0] === 'QTH', `the search filters: "qth" → ${found.join(', ')}`);
  await ev(`(() => { const i = document.getElementById('cwGuideSearch'); i.value = 'german'; i.dispatchEvent(new Event('input')); return 1; })()`);
  ok(await ev(`/DE is not a German callsign/.test(document.getElementById('cwGuideList').textContent)`), 'the search finds a look-alike ("german" → DE)');
  await ev(`(() => { const i = document.getElementById('cwGuideSearch'); i.value = ''; i.dispatchEvent(new Event('input')); return 1; })()`);
  const gFit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, card: document.querySelector('#cwGuide .cwgCard').getBoundingClientRect().right,
    list: document.getElementById('cwGuideList').scrollWidth - document.getElementById('cwGuideList').clientWidth })`));
  ok(gFit.doc <= 391 && gFit.card <= 391 && gFit.list <= 0, `the guide fits 390 px, nothing sideways (page ${gFit.doc}, card ends ${Math.round(gFit.card)}, list overflow ${gFit.list})`);
  const s5 = await shot('cw-terms-guide-phone.png');
  await ev(`document.querySelector('#cwGuide .cwgClose').click()`);
  ok(await until(`!document.getElementById('cwGuide')`, 2000), 'CLOSE closes the guide');
  shotsV12.push(s5);

  console.log('── the gallery has a CW chip ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await ev(`document.getElementById('galleryBtn').click()`);
  await sleep(800);
  const chips = await ev(`[...document.querySelectorAll('#galFilter [data-kind]')].map(b => b.textContent).join(' ')`);
  ok(/\bCW\b/.test(chips), `the chips: ${chips}`);

  console.log('── V12: PLAIN is remembered after a reload ──');
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await trustedClick('#audioGate button');
  await sleep(2500);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     document.getElementById('decodersPanel').classList.add('open'); return false; })()`);
  await sleep(500);
  await ev(`document.querySelector('#decodersPanel [data-dec="cw"]').click()`);
  ok(await until(`getComputedStyle(document.getElementById('cwPlain')).display !== 'none' && document.getElementById('cwPlain').classList.contains('on')`, 5000),
     'after a reload, PLAIN is still on (lit) when CW opens');
  await ev(`document.getElementById('cwPlain').click()`);
  ok(await ev(`(() => { try { return localStorage.getItem('vibe.cwPlain') === '0'; } catch { return false; } })()`), '…and off again is remembered too');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[settings, s2, s1, s3, s4, ...shotsV12].join('  ')}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
}
function document_has_false(rows) { return rows.some((x) => x.c !== '—' && !STATIONS.some(([, c]) => c === x.c)); }
console.log(`\n${fail ? '✗' : '✓'} test-web-cw: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
