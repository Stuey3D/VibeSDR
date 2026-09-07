// clientdrive.mjs — drive the real web client in headless Edge over CDP: open the page, press the
// DAB mode button, wait, and report what the CLIENT shows (station list, signal pane, locked
// controls). usage: node clientdrive.mjs http://host:port [secs]
import { spawn } from 'node:child_process';
const URL0 = process.argv[2] || 'http://192.168.86.88:48001/';
const SECS = Number(process.argv[3] || 40);
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const port = 9333;
const edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${port}`, '--no-first-run', '--no-default-browser-check',
  '--user-data-dir=/tmp/vibesdr-edgeprof', '--window-size=1280,900', '--autoplay-policy=no-user-gesture-required', 'about:blank'],
  { stdio: 'ignore' });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
await sleep(2500);
const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = list.find(t => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
let id = 0; const pend = new Map();
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
const send = (method, params = {}) => new Promise(res => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
await new Promise(r => ws.onopen = r);
await send('Page.enable'); await send('Runtime.enable');
const ev = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
await send('Page.navigate', { url: URL0 });
await sleep(6000);
console.log('title:', await ev('document.title'));
// the splash: pick the first (free) radio card and press START, as a listener does
console.log('splash:', await ev(`(() => { const card = document.querySelector('.radioCard, [class*=radio]'); const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); if (!b) return 'no START'; b.click(); return 'START pressed'; })()`));
await sleep(7000);
// open the mode picker from the card's mode pill, as a user does, then find DAB in it
console.log('pill:', await ev(`(() => { const m = document.getElementById('mMode'); if (!m) return 'no mMode'; m.click(); return 'clicked mMode (' + m.textContent + ')'; })()`));
await sleep(800);
{ const shot = await send('Page.captureScreenshot', { format: 'png' }); (await import('node:fs')).writeFileSync('/tmp/vibesdr-client.png', Buffer.from(shot.result.data, 'base64')); }
console.log('picker options:', await ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].map(b => b.textContent.trim()).join(', ')`));
console.log('dab capable per client:', await ev(`fetch('/vibeserver.json').then(r => r.json()).then(j => JSON.stringify({dab: j.dab, dabBoost: j.dabBoost, dabBoostUseful: j.dabBoostUseful}))`));
const found = await ev(`(() => { const b = [...document.querySelectorAll('button')].filter(x => /^DAB$/i.test(x.textContent.trim())); return b.map(x => x.id + '|' + x.className + '|' + (x.offsetParent ? 'visible' : 'hidden')); })()`);
console.log('DAB buttons:', JSON.stringify(found));
const clicked = await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DAB$/i.test(x.textContent.trim()) && x.offsetParent); if (!b) return 'none visible'; b.click(); return 'clicked ' + b.id; })()`);
console.log('click:', clicked);
for (let t = 5; t <= SECS; t += 5) {
  await sleep(5000);
  const st = await ev(`(() => { const st = document.getElementById('dabStations'); const lbl = document.getElementById('dabMuxLbl'); const rows = st ? [...st.querySelectorAll('.dabSvc')].map(r => r.querySelector('.nm')?.firstChild?.textContent?.trim() + ' [' + r.querySelector('.cod')?.textContent + ']') : []; const z = document.getElementById('zoomIn'); const mz = document.getElementById('mZoomIn'); const held = document.getElementById('decStatus')?.textContent; return JSON.stringify({ mux: lbl?.textContent, n: rows.length, first: rows.slice(0,3), dls: st?.querySelector('.dls')?.textContent, zoomDisabled: z?.disabled, mZoomDisabled: mz?.disabled, status: held, decTitle: document.getElementById('decTitle')?.textContent, boxOpen: document.getElementById('decBox')?.classList.contains('open') }); })()`);
  console.log(`t=${t}s`, st);
}
// pick the first station through the UI and read the signal pane
await ev(`(() => { const r = document.querySelector('#dabStations .dabSvc'); if (r) r.click(); return !!r; })()`);
await sleep(8000);
console.log('after selecting a station:', await ev(`(() => { const st = document.getElementById('dabStations'); return JSON.stringify({ playing: st?.querySelector('.dabSvc.on .nm')?.firstChild?.textContent?.trim(), dls: st?.querySelector('.dls')?.textContent }); })()`));
await ev(`document.getElementById('dabPane')?.click()`);
await sleep(1500);
console.log('signal pane:', (await ev(`document.getElementById('dabSignal')?.innerText`))?.replace(/\n+/g, ' | ').slice(0, 900));
// try to zoom and tune from the keyboard/wheel path, then check the mux did not move
await ev(`(() => { document.getElementById('zoomIn')?.click(); window.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp' })); document.getElementById('tuneUp')?.dispatchEvent(new PointerEvent('pointerdown', { button: 0, bubbles: true })); return 1; })()`);
await sleep(4000);
console.log('after zoom+arrow presses:', await ev(`document.getElementById('dabMuxLbl')?.textContent`));
try { await send('Browser.close'); } catch {}
edge.kill();
process.exit(0);
