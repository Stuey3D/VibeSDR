// test-web-waterfall-history.mjs — GitHub #57 (Gesiima; Stuart 2026-10-27): the waterfall must stay ONE continuous trail
// per station while you tune and zoom — "diagonal when tuning then straight again with the signals in the new spot so they
// read as one continuous signal during a tune", and the fan of trails on a zoom — never "broken up" into rows that disagree.
//
// The fault was in the SERVER (local_sdr_shim.cpp onSpectrum): with the IF filter on Auto the tuner follows a locked VFO on
// every step, and a wide row averaged from frames taken at the OLD tuner position was cropped as if from the new one — so
// every few rows the station jumped back by a whole tuner move and forward again (a zig-zag, the "chopped up view").
//
// Here, against the REAL server and a fake rtl_tcp with one steady carrier, in headless Edge (--mute-audio; tests are silent):
//   1. the wire: every spectrum row the page receives puts the carrier where it is (row centre + bin = the carrier ±2 bins)
//      through a locked VFO tuned up in 1 kHz steps (the tuner moving under it), and through zooms in and out;
//   2. the picture: the waterfall the page drew (its own canvas, the needle hidden) — the carrier's column, row by row, moves
//      SMOOTHLY: no row that jumps away from both its neighbours (a zig-zag) through the tune or the zooms, and the trail
//      really did travel (a tune that drew nothing would pass the first check);
//   3. a window resize keeps the history (the rows from before the resize are still on screen, rescaled to the new width);
//   4. THE ZOOM FAN (Stuart 2026-10-27 23:51: zooming SLICED the web waterfall into bands of different zoom): after a
//      straight trail, a zoom IN and then OUT — every row of that trail, old and new, shows the carrier at the column the
//      NEW span puts it (the history fanned with the zoom), never at the old one.
//   All of it twice: the WebGL renderer, then the 2D fallback (old phones without WebGL draw the same picture).
//   VIBESERVER_BIN=…/vibeserver node --max-old-space-size=512 scripts/test-web-waterfall-history.mjs
//   (the page is the one compiled into that binary — rebuild it after `node scripts/build-web.mjs`)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
const CENTRE = 1000000, CARRIER = 1050000, SPAN = 200000, STEP = 1000, STEPS = 40;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// ★ Every SPEC row the page receives: its centre, span and where its brightest bin says the carrier is — recorded in the page
//   before its own scripts run, so nothing the client does to the row afterwards can hide a wrong one.
const SNIFF = `(() => { window.__rows = []; const O = WebSocket;
  window.WebSocket = function (u, p) { const w = p ? new O(u, p) : new O(u);
    if (String(u).includes('user-spectrum')) w.addEventListener('message', (e) => { const b = e.data;
      if (typeof b === 'string') { try { const m = JSON.parse(b); if (m.type === 'config') window.__rows.push({ t: performance.now(), cfg: m.binBandwidth }); } catch {} return; }
      if (!(b instanceof ArrayBuffer) && !(b instanceof Blob)) return;
      const take = (ab) => { const dv = new DataView(ab); if (ab.byteLength < 23 || dv.getUint32(0, true) !== 0x43455053) return;
        const c = Number(dv.getBigUint64(14, true)), n = ab.byteLength - 22, u8 = new Uint8Array(ab, 22, n), h = n >> 1; let best = -1, bi = 0;
        for (let i = 0; i < n; i++) { const v = u8[(i + h) % n]; if (v > best) { best = v; bi = i; } }
        window.__rows.push({ t: performance.now(), c, n, bi }); if (window.__rows.length > 4000) window.__rows.splice(0, 1000); };
      if (b instanceof ArrayBuffer) take(b); else b.arrayBuffer().then(take); });
    return w; };
  window.WebSocket.prototype = O.prototype; Object.assign(window.WebSocket, O); })();`;

// ★ The carrier's column in every waterfall row on screen, newest first: the centre of the brightest plateau of the row.
//   (The carrier is strong enough to clip the palette over a few pixels — more where a zoom has drawn a row narrower —
//   so "the first brightest pixel" is its left edge, which moves with the plateau's width and reads as a jitter.)
const COLUMNS = `(() => { const h = window.__vibeWf(); if (!h) return null; const wf = h.wf; const cv = document.getElementById('wf');
  const W = cv.width, H = cv.height, wfH = wf.wf.height, top = H - wfH; const d = cv.getContext('2d').getImageData(0, top, W, wfH).data;
  const at = (o, x) => { const p = o + x * 4; return d[p] * 2 + d[p + 1] + d[p + 2]; };
  const cols = [], ls = [], rs = [], blank = []; for (let y = 0; y < wfH; y++) { let best = -1, bx = 0, lo = 1e9; const o = y * W * 4;
    let sum = 0; for (let x = 2; x < W - 2; x++) { const v = at(o, x); sum += v; if (v < lo) lo = v; if (v > best) { best = v; bx = x; } }
    blank.push(best - lo < 30);
    let l = bx, r = bx; while (l > 2 && at(o, l - 1) >= best - 24) l--; while (r < W - 3 && at(o, r + 1) >= best - 24) r++;
    const lit = best - sum / (W - 4) > 120; cols.push(lit ? Math.round((l + r) / 2) : -1); ls.push(lit ? l : -1); rs.push(lit ? r : -1); }
  return { W, wfH, cols, ls, rs, blank }; })()`;

/** Rows (y, newest first) whose column jumps away from BOTH neighbours by more than `px` the same way — a torn row (it
 *  leaves the trail and comes back). Except a trail that TURNS (the fast spin reversing: 1118 1174 1196 1178 1136): it
 *  keeps moving away on both sides, at the same pace as the step to the turn.
 *  (-1 = a row with no clear carrier, e.g. not drawn yet — skipped, and so are its neighbours.) */
function zigzags(cols, px) {
  const out = [];
  for (let y = 1; y < cols.length - 1; y++) {
    if (cols[y] < 0 || cols[y - 1] < 0 || cols[y + 1] < 0) continue;
    const a = cols[y] - cols[y - 1], b = cols[y] - cols[y + 1];
    if (!(Math.sign(a) === Math.sign(b) && Math.abs(a) > px && Math.abs(b) > px)) continue;
    // ★ A TURN: the trail keeps going away from this row on BOTH sides (monotonic), at a pace like the step to it.
    //   An alternating zig-zag fails the first half, a lone torn row in a slow trail the second.
    const c2 = cols[y - 2] ?? -1, c3 = cols[y + 2] ?? -1;
    const away = (n1, n2) => n2 >= 0 && Math.abs(cols[y] - n2) >= Math.abs(cols[y] - n1);
    const turn = away(cols[y - 1], c2) && away(cols[y + 1], c3)
              && Math.min(Math.abs(a), Math.abs(b)) <= px + 2 * Math.max(Math.abs(cols[y - 1] - c2), Math.abs(c3 - cols[y + 1]));
    if (!turn) out.push(y);
  }
  return out;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-wfhist-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Waterfall history test', locator: 'IO92nh', adminPass: 'wfhist-web-admin',
  radios: [{ serial: 'wfhist-web', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000',
                                     '--tones', '0', '--carrier', String(CARRIER), '--carrier-amp', '20'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', String(CENTRE), '--mode', 'am', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
let wsC;
const deadline = setTimeout(() => { console.log('   FAIL deadline (360 s)'); try { edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } catch {} process.exit(1); }, 360000);
try {
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 200; i++) { if (await ev(x)) return true; await sleep(200); } return false; };
  const click = async (sel) => {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || null)`) || 'null');
    if (!r) return false;
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
    return true;
  };
  const columns = () => ev(`new Promise(r => requestAnimationFrame(() => requestAnimationFrame(() => r(${COLUMNS}))))`);
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
  if (process.env.WFHIST_DEBUG) wsC.addEventListener('message', (e) => { const m = JSON.parse(e.data);
    if (m.method === 'Runtime.exceptionThrown') console.log('   page exception', JSON.stringify(m.params.exceptionDetails).slice(0, 400));
    if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') console.log('   page error', JSON.stringify(m.params.args).slice(0, 300)); });
 for (const MODE of ['webgl', '2d']) {
  console.log(`   ── ${MODE} renderer ──`);
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await click('#audioGate button');
  ok(await until(`!!window.__vibeWf() && window.__rows.length > 20`, 15000), `the page is drawing (${await ev('window.__rows.length')} rows in)`);
  if (process.env.WFHIST_DEBUG) console.log('   page', JSON.stringify(await ev(`({ rows: window.__rows.length, url: location.href, text: document.body.innerText.slice(0, 300), wf: !!window.__vibeWf() })`)));
  if (MODE === '2d') await ev(`(() => { const w = window.__vibeWf().wf; w.gl = null; w.glCanvas = null; w.wf.width = 0; w.resize(); return true; })()`);
  ok(MODE === 'webgl' ? await ev('!!window.__vibeWf().wf.gl') : await ev('!window.__vibeWf().wf.gl'), `drawing with the ${MODE} renderer`);
  // ★ The needle, its passband and the RF-centre line are drawn over the waterfall — hide them so the brightest pixel is the carrier.
  await ev(`(() => { const { wf, spec } = window.__vibeWf(); wf._drawVfo = () => {}; spec.followVfo = true;
    spec.tune(${CENTRE}); spec.zoom(${CENTRE}, ${SPAN} / (spec.cfg.binCount || 1024)); return true; })()`);
  await sleep(10000);  // a straight trail to start from — it fills the lower part of the picture (the resize check reads it)

  // ── 1+2. A LOCKED VFO tuned up in 1 kHz steps — the view follows the dial, the tuner follows the view ──
  const r0 = await ev('window.__rows.length');
  for (let i = 1; i <= STEPS; i++) { await ev(`window.__vibeWf().spec.tune(${CENTRE + i * STEP})`); await sleep(80); }
  await sleep(1500);
  if (process.env.WFHIST_DEBUG) await ev(`(() => { const p = window.__vibeWf().wf.proc; const o = p.process.bind(p); window.__dbg = [];
    p.process = (b, c, bw, i) => { const f = o(b, c, bw, i); let m = 0, bi = 0; for (let k = 0; k < f.row.length; k++) if (f.row[k] > m) { m = f.row[k]; bi = k; }
      const r = p.getRange(); window.__dbg.push([Math.round(performance.now()), c, bw, m, bi, Math.round(r.minDb), Math.round(r.maxDb)]); return f; }; return true; })()`);
  // ── …then a zoom in and out round the dial (the fan), in steps as a pinch or the wheel sends them ──
  const fEnd = CENTRE + STEPS * STEP;
  for (const span of [160000, 130000, 100000, 80000, 100000, 130000, 160000, 200000]) {
    await ev(`(() => { const s = window.__vibeWf().spec; s.zoom(${fEnd}, ${span} / (s.cfg.binCount || 1024)); return true; })()`);
    await sleep(250);
  }
  await sleep(1500);
  // ── …and a FAST SPIN down (4 kHz every 40 ms — faster than the tuner's 120 ms retune cooldown, so the radio lags the
  //    dial) and back: the trail glides across and must still never tear (Stuart: a fast spin may leave a slight gap at
  //    a newly exposed edge while the radio catches up, never a torn or cleared history)
  for (let i = 1; i <= 20; i++) { await ev(`window.__vibeWf().spec.tune(${fEnd - i * 4000})`); await sleep(40); }
  for (let i = 19; i >= 0; i--) { await ev(`window.__vibeWf().spec.tune(${fEnd - i * 4000})`); await sleep(40); }
  await sleep(1500);
  if (process.env.WFHIST_DEBUG) for (const d of (await ev('window.__dbg')) || []) console.log('   dbg', d.join(' '));
  const all = (await ev(`window.__rows.slice(${r0})`)) || [];
  if (process.env.WFHIST_DEBUG) { let lab = 0; for (const r of all) { if (r.cfg) { lab = r.cfg; console.log('   cfg', Math.round(r.t), lab); continue; }
    const fit = [SPAN, 160000, 130000, 100000, 80000].filter((s) => Math.abs(r.c + (r.bi - (r.n >> 1)) * (s / r.n) - CARRIER) <= 2.5 * (s / r.n));
    console.log('   row', Math.round(r.t), r.c, 'label', Math.round(lab * r.n), 'fits', fit.join('/'), fit.length && !fit.some((s) => Math.abs(s - lab * r.n) < 50) ? '  <<< MISLABELLED' : ''); } }
  const rows = all.filter((r) => !r.cfg);
  // ★ A row's span is not in the frame header; a row is RIGHT if SOME span the page asked for (or the default) puts the carrier
  //   on it — a torn row is off by a whole tuner move (kilohertz) at every one of them.
  const spans = [SPAN, 160000, 130000, 100000, 80000];
  const wrong = rows.filter((r) => !spans.some((s) => Math.abs(r.c + (r.bi - (r.n >> 1)) * (s / r.n) - CARRIER) <= 2.5 * (s / r.n)));
  ok(rows.length > 60 && wrong.length === 0,
     `the wire: ${rows.length} rows through the tune and the zooms, ${wrong.length} with the carrier somewhere it is not` +
     (wrong.length ? ` (e.g. centre ${wrong[0].c}, bin ${wrong[0].bi} of ${wrong[0].n})` : ''));

  const pic = await columns();
  ok(!!pic && pic.cols.length > 100, `read the waterfall (${pic?.W}×${pic?.wfH})`);
  // the newest ~ (tune + zooms) rows sit at the top; the column of the carrier there must move smoothly
  const recent = pic.cols.slice(0, Math.min(pic.cols.length, 600));
  // ★ Out of the rows DRAWN — the bottom of the picture may still be the blank ring from before the page started.
  const drawn = recent.filter((x, y) => !pic.blank[y]);
  const lit = drawn.filter((x) => x >= 0);
  const zz = zigzags(recent, 4);
  const travel = lit.length ? Math.max(...lit) - Math.min(...lit) : 0;
  ok(drawn.length > 200 && lit.length > drawn.length * 0.7, `the carrier stands out in ${lit.length} of the ${drawn.length} rows drawn`);
  if (process.env.WFHIST_SHOT) { const s = await send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(process.env.WFHIST_SHOT, Buffer.from(s.result.data, 'base64')); }
  if (process.env.WFHIST_DEBUG) { console.log('   cols', recent.slice(0, 260).join(' '));
    console.log('   runs', JSON.stringify(await ev(`(() => { const w = window.__vibeWf().wf; const out = []; const R = w.runs.buf; for (let i = 0; i < 40 && i * 4 < R.length; i++) out.push([R[i*4], R[i*4+1], +R[i*4+2].toFixed(4), +R[i*4+3].toFixed(4)]); return out; })()`))); }
  ok(zz.length === 0, `the picture: the carrier's trail never zig-zags (${zz.length} torn row(s)${zz.length ? ' at y ' + zz.slice(0, 6).join(', ') : ''})`);
  // ★ 40 kHz of tuning across a 200 kHz view = 20 % of the width: the trail must have bent that far — one continuous line
  ok(travel >= pic.W * 0.15, `the trail bends with the tune (${travel} px of travel across ${pic.W} px)`);

  // ── 3. A resize keeps the history (Stuart: resize and rotate must never lose it) ──
  // ★ The OLDEST rows with the carrier in them — the straight trail from before the tune — and where they sit.
  const before = await columns();
  const old = before.cols.map((x, y) => [y, x]).filter(([, x]) => x >= 0).slice(-40);
  await send('Emulation.setDeviceMetricsOverride', { width: 900, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(150);
  const after = await columns();
  const k = after.W / before.W;
  // the picture keeps scrolling between the two reads (~15 rows/s) — a row may have moved down a few lines
  const kept = old.filter(([y, x]) => [0, 1, 2, 3, 4, 5, 6].some((dy) => Math.abs((after.cols[y + dy] ?? -99) - x * k) <= 4)).length;
  ok(old.length >= 30 && after.W !== before.W && kept >= old.length * 0.8,
     `a resize (${before.W} → ${after.W} px) keeps the history: ${kept}/${old.length} of the oldest rows still show the carrier, at its rescaled column`);

  // ── 4. THE ZOOM FAN — a straight trail, then a zoom in, then back out: the whole trail follows the new span ──
  // ★ The carrier sits 10 kHz above the dial: at 200 kHz it is 5 % right of centre, at 80 kHz 12.5 %. A history that does
  //   not fan keeps the trail's older rows at the OLD column under the new rows at the new one — the slicing.
  const fanCase = async (fromSpan, toSpan) => {
    await sleep(4000);   // a straight trail at fromSpan (~80 rows)
    const pre = await columns();
    const colAt = (span, W) => Math.round((0.5 + (CARRIER - fEnd) / span) * W);
    const c0 = colAt(fromSpan, pre.W);
    let L = 0; while (L < pre.cols.length && (pre.cols[L] < 0 || Math.abs(pre.cols[L] - c0) <= 3)) L++;
    await ev(`(() => { const s = window.__vibeWf().spec; s.zoom(${fEnd}, ${toSpan} / (s.cfg.binCount || 1024)); return true; })()`);
    await sleep(1200);
    const post = await columns();
    const c1 = colAt(toSpan, post.W);
    // the straight trail has moved down by the rows drawn since; check the top L rows (new + old), a few in-flight rows excused
    const top = post.cols.slice(0, Math.min(L, 200)).filter((x) => x >= 0);
    const at = top.filter((x) => Math.abs(x - c1) <= 4).length, atOld = top.filter((x) => Math.abs(x - c0) <= 3).length;
    ok(L >= 40 && top.length >= 40 && at >= top.length * 0.92,
       `zoom ${fromSpan / 1000} → ${toSpan / 1000} kHz fans the history: ${at}/${top.length} rows of the ${L}-row trail at the new column ${c1}` +
       ` (${atOld} still at the old column ${c0})`);
  };
  await fanCase(SPAN, 80000);
  await fanCase(80000, SPAN);

  // ── 5. GLIDE (Stuart 2026-10-28): a FAST tune — 3 kHz every 60 ms, ~16 px a row at 200 kHz across 1280 px — must draw
  //    ONE continuous diagonal: every row's carrier touches the row above's, no dashes with gaps between them ──
  {
    await sleep(1500);
    for (let i = 1; i <= 16; i++) { await ev(`window.__vibeWf().spec.tune(${fEnd + i * 3000})`); await sleep(60); }
    await sleep(1500);
    const g = await columns();
    const cStart = Math.round((0.5 + (CARRIER - fEnd) / SPAN) * g.W), cEnd = Math.round((0.5 + (CARRIER - fEnd - 48000) / SPAN) * g.W);
    // the moving part of the trail, newest first: rows strictly between where it started and where it settled
    const ys = []; for (let y = 0; y < Math.min(400, g.cols.length); y++) { const c = g.cols[y]; if (c >= 0 && c < cStart - 3 && c > cEnd + 3) ys.push(y); }
    let gaps = 0, worst = 0, pairs = 0;
    const y0 = ys.length ? ys[0] - 1 : 0, y1 = ys.length ? ys[ys.length - 1] + 1 : 0;
    for (let y = Math.max(0, y0); y < y1; y++) {
      if (g.ls[y] < 0 || g.ls[y + 1] < 0) continue;
      pairs++;
      const gap = Math.max(g.ls[y] - g.rs[y + 1], g.ls[y + 1] - g.rs[y]) - 1;   // empty pixels between the two rows' carriers
      if (gap > 1) { gaps++; worst = Math.max(worst, gap); }
    }
    ok(ys.length >= 6 && pairs >= 6 && gaps <= 1,
       `a fast tune (${cStart} → ${cEnd} px) draws one continuous trail: ${gaps} gap(s) between ${pairs} row pairs along ${ys.length} moving rows` +
       (gaps ? ` (widest ${worst} px)` : ''));
  }
 }
} catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
finally {
  clearTimeout(deadline);
  try { wsC?.close(); } catch {}
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
