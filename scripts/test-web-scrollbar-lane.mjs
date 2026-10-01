// test-web-scrollbar-lane.mjs — a PERMANENT scrollbar never covers or squeezes a panel's controls.
//
// ★★★ WHY. Stuart, 2026-10-01: "make sure the scrollbar doesn't cover the buttons — not a big issue in
//     Safari as the bar disappears, but on my work laptop with Windows and Edge the bar stays on
//     screen." A classic scrollbar (Windows; macOS set to "Show scroll bars: Always") takes a lane
//     out of the panel only once its content overflows — so a panel re-lays itself as it grows, and a
//     row that fitted no longer does. index.html gives every scrolling panel `scrollbar-gutter:
//     stable`; this measures the result with a classic bar FORCED ON.
// ★ How the bar is forced: a `::-webkit-scrollbar { width: 15px }` style injected into the page. In
//   Chromium a styled scrollbar is a classic one — it takes layout space on every OS, so the Mac's
//   overlay bars cannot hide the fault. 15 px is Windows' own width.
// ★ Each panel is opened the way the client opens it (the `open` class, one at a time) and capped
//   short (220 px) so it MUST scroll, at a phone width (390) and a laptop width (1280). Checked by
//   geometry: the bar is really there, nothing overflows sideways, every visible button / input /
//   select ends inside the scrollport (left of the bar), and the lane is the same with the panel
//   short or tall (the reserved gutter: the rows do not move when the bar appears).
// ★ Needs a vibeserver BUILT FROM THIS TREE (VIBESERVER_BIN=…) — it serves the page compiled into it.
//   Without one: "not run" (exit 3). SILENT: --mute-audio, a throwaway profile deleted at the end.
//   SHOTS=<dir> also saves a screenshot of each panel.
//
//   usage: node scripts/build-web.mjs && cmake --build <build> && \
//          VIBESERVER_BIN=<build>/vibeserver node scripts/test-web-scrollbar-lane.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   not run — Microsoft Edge is not installed'); process.exit(3); }
const SHOTS = process.env.SHOTS || '';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const PANELS = ['menu', 'audioPanel', 'decodersPanel', 'recordingsPanel', 'bookmarksPanel', 'freqPanel', 'chatPanel'];
const WIDTHS = [390, 1280];

// ── a single-radio server on the fake dongle ────────────────────────────────────────────────────
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-lane-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, adminPass: 'testadmin123', name: 'Lane test', mode: 'single', sharing: 'local', users: 4, web: true }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--wfm', '100.0'], { stdio: 'ignore' });
await sleep(300);
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port)], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
         VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') }, stdio: ['ignore', log, log] });
const edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check',
  '--mute-audio', `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', '--autoplay-policy=no-user-gesture-required', 'about:blank'], { stdio: 'ignore' });

// ★ A watchdog: a driver that hangs (a CDP call that never answers) must FAIL, never sit forever.
let hung = false;
const watchdog = setTimeout(() => { hung = true; console.log('   FAIL driver: no answer within 240 s');
  for (const c of [edge, srv, rtl]) { try { c.kill(); } catch { /* gone */ } }
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* best effort */ } process.exit(1); }, 240000);
try {
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  let page = null;
  for (let i = 0; i < 50 && !page; i++) {
    try { page = (await (await fetch(`http://127.0.0.1:${cdp}/json/list`)).json()).find((t) => t.type === 'page'); } catch { /* not up yet */ }
    if (!page) await sleep(200);
  }
  if (!page) throw new Error('headless Edge never offered a page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  await new Promise((r) => { ws.onopen = r; });
  await send('Page.enable'); await send('Runtime.enable');
  const ev = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description ?? r.result.exceptionDetails.text);
    return r.result?.result?.value;
  };

  for (const w of WIDTHS) {
    await send('Emulation.setDeviceMetricsOverride', { width: w, height: 860, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await sleep(4000);
    // Past the start screen, as a listener would.
    // ★ A real press (CDP mouse events), as a listener's: the gate takes itself down on the gesture.
    let at = null;
    for (let i = 0; i < 40 && !at; i++) { if (i) await sleep(250); at = await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START( |$)/i.test(x.textContent.trim()) && x.getClientRects().length);
      if (!b) return null; const r = b.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`); }
    const started = !!at;
    if (at) for (const type of ['mousePressed', 'mouseReleased']) await send('Input.dispatchMouseEvent', { type, x: at.x, y: at.y, button: 'left', clickCount: 1 });
    await sleep(800);
    ok(started, `${w}px: START RADIO pressed`);
    // ★ A headless, muted browser can keep its AudioContext suspended, and then the gate (rightly)
    //   comes back on the next tick. It is not what is under test and it covers the panels, so it is
    //   hidden for the measurement — hidden, not removed, so the page's own state is left alone.
    await ev(`(() => { const s = document.createElement('style'); s.textContent = '#audioGate{display:none!important}'; document.head.appendChild(s); return true; })()`);
    await sleep(1500);
    // ★ A classic, layout-taking scrollbar on every OS (see the header).
    await ev(`(() => { const s = document.createElement('style'); s.id = 'laneTestBars';
      s.textContent = '::-webkit-scrollbar{width:15px;height:15px;background:#222}::-webkit-scrollbar-thumb{background:#999;border-radius:0}';
      document.head.appendChild(s); return true; })()`);

    for (const p of PANELS) {
      const r = await ev(`(async () => {
        const ids = ${JSON.stringify(PANELS)};
        for (const i of ids) document.getElementById(i)?.classList.remove('open');
        const el = document.getElementById(${JSON.stringify(p)});
        if (!el) return { missing: true };
        el.classList.add('open');
        el.style.transition = 'none';
        const lane = () => {
          const cs = getComputedStyle(el);
          return el.offsetWidth - el.clientWidth - parseFloat(cs.borderLeftWidth) - parseFloat(cs.borderRightWidth);
        };
        const firstRight = () => { const b = [...el.querySelectorAll('button,input,select')].find(x => x.offsetParent && x.getClientRects().length);
                                   return b ? Math.round(b.getBoundingClientRect().right * 10) / 10 : null; };
        // Tall first: the content fits (or nearly), then capped short so it MUST scroll.
        el.style.maxHeight = '95vh';
        await new Promise(r => setTimeout(r, 80));
        const laneTall = lane(), rightTall = firstRight(), overflowedTall = el.scrollHeight > el.clientHeight + 1;
        // ★ A short list (no recordings, no bookmarks yet) would not scroll at any cap, so a test-only
        //   filler guarantees the overflow; it is removed when the panel is closed.
        const filler = document.createElement('div'); filler.id = 'laneTestFiller'; filler.style.height = '600px';
        el.appendChild(filler);
        el.style.maxHeight = '220px';
        await new Promise(r => setTimeout(r, 80));
        const box = el.getBoundingClientRect();
        const cs = getComputedStyle(el);
        const portRight = box.left + parseFloat(cs.borderLeftWidth) + el.clientWidth;
        const bad = [];
        for (const b of el.querySelectorAll('button,input,select,[role=button]')) {
          if (!b.offsetParent || !b.getClientRects().length) continue;
          const rr = b.getBoundingClientRect();
          if (rr.width === 0) continue;
          if (rr.right > portRight + 0.5) bad.push((b.id || b.textContent.trim().slice(0, 16) || b.tagName) + ' @' + Math.round(rr.right) + '>' + Math.round(portRight));
        }
        const wide = [];
        for (const d of el.querySelectorAll('*')) {
          if (!d.getClientRects().length) continue;
          const rr = d.getBoundingClientRect();
          if (rr.right > portRight + 1) wide.push((d.id ? '#' + d.id : d.tagName.toLowerCase() + (d.className && typeof d.className === 'string' ? '.' + d.className.split(' ')[0] : '')) + ' ' + Math.round(rr.right - portRight) + 'px');
        }
        return { wide, lane: lane(), laneTall, overflowedTall, rightTall, rightShort: firstRight(),
                 scrolls: el.scrollHeight > el.clientHeight + 1, sideways: el.scrollWidth - el.clientWidth,
                 gutter: cs.scrollbarGutter, bad };
      })()`);
      const tag = `${w}px ${p}`;
      if (r.missing) { ok(false, `${tag}: panel exists`); continue; }
      ok(r.scrolls && r.lane >= 14, `${tag}: capped at 220 px it scrolls, with a classic bar ${r.lane}px wide`);
      ok(/stable/.test(r.gutter), `${tag}: scrollbar-gutter is stable (${r.gutter})`);
      ok(r.sideways <= 1, `${tag}: nothing overflows sideways (scrollWidth − clientWidth = ${r.sideways})${r.wide.length ? ' — ' + r.wide.slice(0, 6).join(', ') : ''}`);
      ok(r.bad.length === 0, `${tag}: every control ends left of the bar${r.bad.length ? ' — ' + r.bad.slice(0, 6).join(', ') : ''}`);
      ok(r.lane === r.laneTall && r.rightTall === r.rightShort,
         `${tag}: the lane is reserved whether or not it scrolls (lane ${r.laneTall}→${r.lane}, first control's right ${r.rightTall}→${r.rightShort})`);
      if (SHOTS) {
        const shot = await send('Page.captureScreenshot', { format: 'png' });
        fs.mkdirSync(SHOTS, { recursive: true });
        fs.writeFileSync(path.join(SHOTS, `${w}-${p}.png`), Buffer.from(shot.result.data, 'base64'));
      }
      await ev(`(() => { const el = document.getElementById(${JSON.stringify(p)}); document.getElementById('laneTestFiller')?.remove(); el.classList.remove('open'); el.style.maxHeight = ''; el.style.transition = ''; return true; })()`);
    }
  }
  ws.close();
} catch (e) {
  ok(false, `driver: ${e?.message ?? e}`);
} finally {
  try { edge.kill(); } catch (e) { console.error(e); }
  try { srv.kill(); } catch (e) { console.error(e); }
  try { rtl.kill(); } catch (e) { console.error(e); }
  await sleep(800);
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) { console.error('cleanup', e); }
}
clearTimeout(watchdog);
console.log(`   scrollbar lane: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
