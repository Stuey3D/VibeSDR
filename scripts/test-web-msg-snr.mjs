// test-web-msg-snr.mjs — beta96 (Stuart 11-26) the colour-coded SNR on every decoded message, as the web client DRAWS it
// (src/utils/snrColour.ts snrChipHtml through the real row builders), in headless Edge (--mute-audio):
//   · (the old ADS-B list's rows went with the map purge, 11-27 — the map's LIST rows carry the same snrChipHtml)
//   · AIS ship rows (web/client/src/tracks.ts aisRows) — only from a server that says snrv 2;
//   · LoRa messages (web/client/src/ism.ts ismMsgsHtml) — per spreading factor; an SNR below zero shown;
//   each in its own real CSS (index.html's .snrChip block, tracks.ts's and ism.ts's CSS, copied out of the
//   files so a change there is what is tested), on a dark AND a light (silver-like) page, at 900 and 390 px:
//   the chip's fill is the band's colour for THAT mode's margin, its text dark and the number, it sits wholly inside its
//   row and never pushes the row sideways; an older server's rows have no chip.
//   node scripts/test-web-msg-snr.mjs     [VIBE_SHOT_DIR=… for the screenshots]
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

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-msgsnr-'));
const { build } = await import(path.join(root, 'node_modules/esbuild/lib/main.js'));
const J = (p) => JSON.stringify(path.join(root, p));
fs.writeFileSync(path.join(dir, 'entry.ts'), `
import { aisRows, aisSanitise } from ${J('web/client/src/tracks.ts')};
import { ismMsgsHtml, ismSanitise } from ${J('web/client/src/ism.ts')};
import { snrBandFor } from ${J('src/utils/snrColour.ts')};
(window as any).T = { aisRows, aisSanitise, ismMsgsHtml, ismSanitise, snrBandFor };
`);
await build({ entryPoints: [path.join(dir, 'entry.ts')], bundle: true, format: 'iife', outfile: path.join(dir, 'bundle.js'), logLevel: 'error',
              loader: { '.png': 'dataurl', '.svg': 'text', '.webp': 'dataurl' } });
// ── the CSS, from the files themselves ──
const html = fs.readFileSync(path.join(root, 'web/client/index.html'), 'utf8');
const between = (s, a, b) => { const i = s.indexOf(a); const j = s.indexOf(b, i + a.length); return i >= 0 && j > i ? s.slice(i + a.length, j) : ''; };
const chipCss = html.slice(html.indexOf('/* ★★ beta96 THE MESSAGE SNR CHIP'), html.indexOf('.snrChip.green') + 200).split('\n').slice(0, 6).join('\n');
const trkCss = between(fs.readFileSync(path.join(root, 'web/client/src/mapChrome.ts'), 'utf8'), 'const CSS = `', '`;');
const ismCss = between(fs.readFileSync(path.join(root, 'web/client/src/ism.ts'), 'utf8'), 'const CSS = `', '`;');
ok(chipCss.includes('.snrChip.red') && trkCss.includes('.trkRow') && ismCss.includes('.ismSnr'), 'the CSS comes from the files');
fs.writeFileSync(path.join(dir, 'page.html'), `<!doctype html><meta charset="utf-8"><style>
:root { --text-dim: #9aa; --btn-text: #eee; --btn-active-bg: rgba(255,200,0,.1); }
body { background: #111; color: #eee; margin: 16px; font: 12px system-ui, sans-serif; } body.light { background: #c9ccd1; color: #111; }
.box { width: 900px; margin-bottom: 12px; position: relative; }
${chipCss}
${trkCss}
${ismCss}
</style>
<div class="box trkView" style="position:relative;inset:auto"><div class="trkList" style="position:relative;left:0;bottom:0;width:auto;max-height:none"><div class="rows" id="ais"></div></div></div>
<div class="box" id="ism"></div>
<script src="bundle.js"></script>`);

const dbg = await freePort();
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1000,1000', 'about:blank'], { stdio: 'ignore' });
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
  ok(await ev('!!window.T'), 'the bundle loaded');

  // ── the rows: three modes, a strong, a middling and a marginal message each, and one with no SNR ──
  await ev(`(() => {
    const rx = { lat: 51.5, lon: -0.1 };
    const ais = { type: 'ais', rate: 2.4e6, now: 1, msgs: 9, msgRate: 3, chA: 5, chB: 4, repaired: 0, dropped: 0, capture: 100, noise: -40, snrv: 2, safety: [],
      vessels: [ { mmsi: 235000001, kind: 'A', name: 'STRONG SHIP', family: 'cargo', lat: 51.4, lon: -0.2, sog: 10, cog: 90, ch: 'A', snr: 24, seen: 1, age: 9, posAge: 1, count: 4 },
                 { mmsi: 235000002, kind: 'A', name: 'A SHIP WITH A VERY LONG NAME INDEED', family: 'tanker', lat: 51.3, lon: -0.3, ch: 'B', snr: 13, seen: 2, age: 9, posAge: 2, count: 2 },
                 { mmsi: 235000003, kind: 'B', name: 'EDGE', family: 'leisure', ch: 'A', snr: 9.5, seen: 3, age: 9, posAge: -1, count: 1 } ] };
    document.getElementById('ais').innerHTML = T.aisRows(T.aisSanitise(ais), rx);
    window.aisOld = T.aisRows(T.aisSanitise({ ...ais, snrv: undefined }), rx);
    const ism = { type: 'ism', lora: { on: 1 }, devices: [], msgs: [
      { ago: 3, net: 'meshtastic', from: 'Bob', ch: 'LongFast', text: 'hello from the hill', freq: 869525000, snr: -9.5, sf: 11 },
      { ago: 8, net: 'meshcore', from: 'Al', text: 'testing', freq: 869618000, snr: -10.5, sf: 8 },
      { ago: 20, net: 'meshtastic', from: 'Cy', text: 'no snr from an older server', freq: 869525000 } ] };
    document.getElementById('ism').innerHTML = T.ismMsgsHtml(T.ismSanitise(ism));
    return true; })()`);

  const audit = `(() => {
    const rgb = { red: 'rgb(229, 72, 77)', amber: 'rgb(240, 160, 32)', green: 'rgb(61, 187, 95)' };
    const out = [];
    for (const [hostId, rowSel, mode] of [['ais', '.trkRow', 'ais'], ['ism', '.ismRow', 'lora']]) {
      for (const row of document.querySelectorAll('#' + hostId + ' ' + rowSel)) {
        const c = row.querySelector('.snrChip');
        const rr = row.getBoundingClientRect();
        const o = { host: hostId, text: row.textContent.slice(0, 30), chip: !!c, rowOverflow: row.scrollWidth > row.clientWidth + 1 };
        if (c) {
          const cs = getComputedStyle(c), cr = c.getBoundingClientRect();
          o.num = c.textContent; o.bg = cs.backgroundColor; o.fg = cs.color; o.band = c.classList[1]; o.want = rgb[o.band];
          o.inside = cr.left >= rr.left - 0.5 && cr.right <= rr.right + 0.5 && cr.width > 10 && cr.height > 8;
          o.title = c.title;
        }
        out.push(o);
      }
    }
    return out; })()`;
  for (const [w, theme] of [[900, 'dark'], [390, 'dark'], [390, 'light']]) {
    await ev(`document.body.className = '${theme === 'light' ? 'light' : ''}'; for (const b of document.querySelectorAll('.box')) b.style.width = '${w}px'; true`);
    const rows = await ev(audit);
    const chips = rows.filter((r) => r.chip);
    ok(chips.length === 5, `${w} px ${theme}: 5 chips (3 ships, 2 LoRa messages) — got ${chips.length}`);
    ok(chips.every((c) => c.bg === c.want && c.fg === 'rgb(17, 17, 17)'), `${w} px ${theme}: every chip filled with its band's colour, dark text` + (chips.find((c) => c.bg !== c.want) ? ' — ' + JSON.stringify(chips.find((c) => c.bg !== c.want)) : ''));
    ok(chips.every((c) => c.inside), `${w} px ${theme}: every chip wholly inside its row` + (chips.find((c) => !c.inside) ? ' — ' + JSON.stringify(chips.find((c) => !c.inside)) : ''));
    ok(rows.every((r) => !r.rowOverflow), `${w} px ${theme}: no row pushed sideways` + (rows.find((r) => r.rowOverflow) ? ' — ' + JSON.stringify(rows.find((r) => r.rowOverflow)) : ''));
    if (w === 900 && theme === 'dark') {
      const by = (h) => chips.filter((c) => c.host === h).map((c) => `${c.num}:${c.band}`);
      ok(JSON.stringify(by('ais')) === JSON.stringify(['24 dB:green', '13 dB:amber', '10 dB:red']), 'AIS: 24 green, 13 amber, 9.5 red (threshold 9 dB): ' + by('ais'));
      ok(JSON.stringify(by('ism')) === JSON.stringify(['−9 dB:green', '−10 dB:red']), 'LoRa: −9.5 (shown −9) at SF11 green, −10.5 (−10) at SF8 red — the same dB, per spreading factor: ' + by('ism'));
      ok(rows.filter((r) => !r.chip).length === 1, 'the message with no SNR (an older server) has no chip');
      ok(chips[0].title.includes('over where this mode stops decoding') && chips[0].title.includes('last few') === false, 'the tooltip says the margin: ' + chips[0].title.slice(0, 90));
      ok(!(await ev('aisOld')).includes('snrChip'), 'AIS from an older server (no snrv): no chips');
    }
    console.log('   screenshot', await shot(`msg-snr-${w}-${theme}.png`));
  }
} catch (e) { ok(false, 'threw: ' + (e?.stack || e)); }
finally {
  try { wsC?.close(); } catch {}
  edge.kill('SIGKILL');
  clearTimeout(watchdog);
  await sleep(300);
  try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {}
}
console.log(`message snr (web): ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
