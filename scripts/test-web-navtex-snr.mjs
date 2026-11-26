// test-web-navtex-snr.mjs — beta96 (Stuart 11-26): NAVTEX's SNR strip and bars as the web client DRAWS them
// (web/client/src/navtexSnrDom.ts over src/utils/navtex.ts + navtexQuality.ts), in headless Edge (--mute-audio).
//   · a message with a fade in the middle (slices as op 0x08 brings them), drawn in the live card's own markup and CSS
//     (copied out of web/client/index.html, so a change there is what is tested);
//   · the strip FOLLOWS WRAPPING: at 640 px and at 170 px wide, every rendered line's strip colour = the worst slice of
//     the characters on that line (read back from the layout, character by character), and the faded lines are red;
//   · the horizontal timeline (a segment per slice, red in the middle) and the 3-colour quality bar;
//   · an older server (no slices, no split): no strip, no gutter, no bars.
//   node scripts/test-web-navtex-snr.mjs     [VIBE_SHOT_DIR=… for the screenshots]
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 90 s'); process.exit(1); }, 90_000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-navsnr-'));
// ── the page: the bundle (esbuild), the live card's markup, and index.html's beta96 CSS block verbatim ──
const { build } = await import(path.join(root, 'node_modules/esbuild/lib/main.js'));
fs.writeFileSync(path.join(dir, 'entry.ts'), `
import { NavtexAssembler, navtexBody } from ${JSON.stringify(path.join(root, 'src/utils/navtex.ts'))};
import { bodyTextBase, snrRanges, lineWorst, snrBand, navtexQualityLine, navtexSnrLine } from ${JSON.stringify(path.join(root, 'src/utils/navtexQuality.ts'))};
import { paintSnrStrip, qualityBar, snrHBar } from ${JSON.stringify(path.join(root, 'web/client/src/navtexSnrDom.ts'))};
(window as any).T = { NavtexAssembler, navtexBody, bodyTextBase, snrRanges, lineWorst, snrBand, navtexQualityLine, navtexSnrLine, paintSnrStrip, qualityBar, snrHBar };
`);
await build({ entryPoints: [path.join(dir, 'entry.ts')], bundle: true, format: 'iife', outfile: path.join(dir, 'bundle.js'), logLevel: 'error' });
const html = fs.readFileSync(path.join(root, 'web/client/index.html'), 'utf8');
const a = html.indexOf('/* ★★ beta96 NAVTEX decode quality + SNR'), b = html.indexOf('#nvSnrBar {', a);
const css = html.slice(a, html.indexOf('}', b) + 1);
ok(a > 0 && b > a && css.includes('.snrStrip'), 'the CSS comes from web/client/index.html');
fs.writeFileSync(path.join(dir, 'page.html'), `<!doctype html><meta charset="utf-8"><style>
:root { --text-dim: #9aa; --btn-text: #eee; } body { background: #111; color: #eee; margin: 16px; font: 12px ui-monospace, Menlo, monospace; }
#nvText { margin: 0; color: var(--btn-text); font-size: 12px; white-space: pre-wrap; word-break: break-word; }
#box { width: 640px; }
${css}
</style><div id="box"><div id="nvQual" hidden><span class="nqBar" id="nvQBar" hidden></span><span id="nvQText"></span>
<span class="snrBar" id="nvSnrBar" hidden></span><span id="nvSnrText"></span></div>
<div class="snrWrap" id="nvWrap"><div class="snrStrip" id="nvStrip"></div><pre id="nvText"></pre></div></div>
<script src="bundle.js"></script>`);

const dbg = await freePort();
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=900,900', 'about:blank'], { stdio: 'ignore' });
let wsC;
try {
  let list = null;
  for (let i = 0; i < 100 && !list; i++) { try { list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json(); } catch { await sleep(100); } }
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.exception?.description; };
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, name); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: 'file://' + path.join(dir, 'page.html') });
  for (let i = 0; i < 40 && !(await ev('!!window.T')); i++) await sleep(100);

  // a message: header, two green lines, a 3-second fade over two long lines, two green lines, NNNN; then the 0x07 split
  await ev(`(() => {
    const a = new T.NavtexAssembler(); let t = 1764150000000;
    const line = (s, sig, noise) => { a.push(s + '\\r\\r\\n', t += 1000); a.slice(sig, noise); };
    a.push('ZCZC GA42\\r\\r\\n', t); a.slice(-30, -50);
    line('GALE WARNING ISSUED 261200 UTC NOV', -30, -48);
    line('DOVER STRAIT WIND SOUTHWEST 6 TO GALE 8 VEERING WEST LATER', -30, -47);
    line('THAMES SOUTHWEST 7 TO SEVERE GALE 9 OCCASIONALLY STORM 10 LATER', -30, -32);
    line('WIGHT PORTLAND PLYMOUTH WEST 8 TO STORM 10 VIOLENT STORM 11 LATER', -30, -33);
    line('SEA STATE ROUGH OR VERY ROUGH OCCASIONALLY HIGH LATER', -30, -46);
    line('VIS GOOD OCCASIONALLY POOR', -30, -45);
    a.push('NNNN\\r\\r\\n', t += 1000);
    a.setFec({ clean: 300, repaired: 12, lost: 2, agreed: 290, disagreed: 3 });
    window.M = a.live; return true; })()`);
  const paint = `(() => { const m = M, body = T.navtexBody(m), pre = document.getElementById('nvText'); pre.textContent = body;
    T.qualityBar(document.getElementById('nvQBar'), m.split); T.snrHBar(document.getElementById('nvSnrBar'), m.snr);
    document.getElementById('nvQual').hidden = false; document.getElementById('nvQText').textContent = T.navtexQualityLine(m.split);
    document.getElementById('nvSnrText').textContent = T.navtexSnrLine(m.snr);
    T.paintSnrStrip(document.getElementById('nvWrap'), document.getElementById('nvStrip'), pre, m.snr ? T.snrRanges(m.snr, T.bodyTextBase(body), body.length) : null);
    return true; })()`;
  /** Read the layout back: each rendered line (by its characters' tops), its text, the worst slice over its characters (the
   *  rule), and the colour the strip shows at its middle. */
  const audit = `(() => {
    const pre = document.getElementById('nvText'), node = pre.firstChild, strip = document.getElementById('nvStrip');
    const body = node.data, base = T.bodyTextBase(body), ranges = T.snrRanges(M.snr, base, body.length);
    const r = document.createRange(), lines = [];
    for (let c = 0; c < body.length; c++) {
      if (body[c] === '\\n') continue;
      r.setStart(node, c); r.setEnd(node, c + 1);
      const rc = r.getClientRects()[0]; if (!rc) continue;
      const L = lines[lines.length - 1];
      if (L && Math.abs(L.top - rc.top) < 2) { L.end = c + 1; L.bottom = Math.max(L.bottom, rc.bottom); }
      else lines.push({ start: c, end: c + 1, top: rc.top, bottom: rc.bottom });
    }
    const worst = T.lineWorst(lines, ranges);
    const s0 = strip.getBoundingClientRect().top;
    const segs = [...strip.children].map((i) => ({ top: s0 + parseFloat(i.style.top), h: parseFloat(i.style.height), c: getComputedStyle(i).backgroundColor }));
    const rgb = { red: 'rgb(229, 72, 77)', amber: 'rgb(240, 160, 32)', green: 'rgb(61, 187, 95)' };
    return lines.map((l, i) => { const mid = (l.top + l.bottom) / 2; const seg = segs.find((s) => mid >= s.top && mid <= s.top + s.h);
      return { text: body.slice(l.start, l.end), want: worst[i] === null ? null : rgb[T.snrBand(worst[i])], got: seg ? seg.c : null }; });
  })()`;
  const check = async (w) => {
    await ev(`document.getElementById('box').style.width = '${w}px'; true`);
    await ev(paint);
    const lines = await ev(audit);
    const bad = lines.filter((l) => l.want !== l.got);
    ok(lines.length > 0 && bad.length === 0, `${w} px: ${lines.length} rendered lines, each strip colour = the worst slice of its characters` + (bad.length ? ` — wrong: ${JSON.stringify(bad.slice(0, 3))}` : ''));
    const red = lines.filter((l) => l.got === 'rgb(229, 72, 77)').map((l) => l.text).join('');
    ok(red.replace(/\s/g, '').includes('THAMESSOUTHWEST7') && red.includes('VIOLENT') && !red.includes('DOVER') && !red.includes('SEA STATE'),
       `${w} px: the red lines are the faded ones (THAMES…, WIGHT…), nothing else`);
    return lines.length;
  };
  const wide = await check(640);
  console.log('   screenshot', await shot('navtex-snr-640.png'));
  const narrow = await check(170);
  console.log('   screenshot', await shot('navtex-snr-170.png'));
  ok(narrow > wide + 4, `re-wrapped: ${wide} lines at 640 px, ${narrow} at 170 px`);
  ok(await ev(`getComputedStyle(document.getElementById('nvText')).paddingLeft`) === '10px', 'the text sits 10 px in (4 px strip + 6 px gap)');
  const hb = await ev(`[...document.getElementById('nvSnrBar').children].map((i) => getComputedStyle(i).backgroundColor)`);
  ok(hb.length === 7 && hb[3] === 'rgb(229, 72, 77)' && hb[4] === 'rgb(229, 72, 77)' && hb[1] === 'rgb(61, 187, 95)', 'the horizontal timeline: 7 segments, red in the middle');
  const qb = await ev(`[...document.getElementById('nvQBar').children].map((i) => [getComputedStyle(i).backgroundColor, +i.style.flexGrow.slice(0, 5)])`);
  ok(qb.length === 3 && qb[0][0] === 'rgb(61, 187, 95)' && qb[0][1] > 90, 'the quality bar: green (clean) first and widest, then amber (FEC), red (lost)');
  ok(/^Decode quality: 92\.4 % clean · 7\.0 % FEC · 0\.6 % lost$/.test(await ev(`document.getElementById('nvQText').textContent`)), 'the quality line');
  // an older server: nothing
  await ev(`(() => { const a = new T.NavtexAssembler(); a.push('ZCZC GA43\\r\\r\\nGALE\\r\\r\\nNNNN\\r\\r\\n', 1); a.setFec({ clean: 10, repaired: 1, lost: 0 }); window.M = a.live;
    const pre = document.getElementById('nvText'); pre.textContent = T.navtexBody(M);
    T.qualityBar(document.getElementById('nvQBar'), M.split); T.snrHBar(document.getElementById('nvSnrBar'), M.snr);
    T.paintSnrStrip(document.getElementById('nvWrap'), document.getElementById('nvStrip'), pre, null); return true; })()`);
  ok(await ev(`!document.getElementById('nvWrap').classList.contains('snr') && document.getElementById('nvStrip').children.length === 0
     && document.getElementById('nvQBar').hidden && document.getElementById('nvSnrBar').hidden && getComputedStyle(document.getElementById('nvText')).paddingLeft === '0px'`),
     'an older server: no strip, no gutter, no bars');
} catch (e) { ok(false, 'threw: ' + (e?.stack || e)); }
finally {
  try { wsC?.close(); } catch {}
  edge.kill('SIGKILL');
  clearTimeout(watchdog);
  await sleep(300);
  try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {}
}
console.log(`navtex snr (web): ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
