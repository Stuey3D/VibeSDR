// test-web-wf-fan.mjs — THE WEB WATERFALL'S HISTORY on a zoom, a tune, a jump and a resize, on its REAL drawing path
// (web/client/src/waterfall.ts — the WebGL renderer, then the 2D fallback), in headless Edge (--mute-audio: silent).
//
// Stuart (2026-10-29): "yes please we want to preserve the history across tunes and zooms" — the app's rule, ported:
//   "I want no slicing when zooming or tuning, I want minimal black gaps only when absolutely necessary due to the SDR
//    catching up from a large move or rapid tuning, and I want the waterfall to be preserved on orientation change and
//    resizing. This is a fully buffered waterfall with a ring buffer." (memory feedback_waterfall_core_rules)
//
// The page (scripts/web-wf-fan-driver.ts) feeds the real Waterfall a synthetic band on a virtual clock — 20 fps frames, a
// 60 Hz display — so every run draws the same picture. Every row the waterfall writes is recorded with the view it was
// DRAWN against, and the test reads the canvas back and checks, for every carrier in every row on screen:
//   • it is drawn at the column its row's own geometry puts it (its TRUE frequency in that row's scale) — through the fan;
//   • no slicing: between one row and the next a carrier's column never jumps (> 4 % of the width) — the trail is
//     continuous across a zoom in, a zoom out (also on UberSDR-style snapped rungs), a small tune, a large tune and a jump;
//   • black only where the row has no data (a jump's newly exposed edge), never inside a row;
//   • history preserved: the rows from before each move are still on screen, still at their own geometry;
//   • a resize and a rotate keep the history (the same trails, rescaled to the new size).
// Pictures of each stage go to WFFAN_PNG=<dir> (the evidence images).
//   node scripts/test-web-wf-fan.mjs            (CPU: WFFAN_CPU=1 adds a per-frame cost line per stage)
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
const PNG = process.env.WFFAN_PNG || '';
if (PNG) fs.mkdirSync(PNG, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-wffan-'));
// ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
//   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
await build({ entryPoints: [path.join(root, 'scripts/web-wf-fan-driver.ts')], bundle: true, format: 'iife', target: ['chrome110'],
              outfile: path.join(dir, 'drv.js'), logLevel: 'error' });
fs.writeFileSync(path.join(dir, 'index.html'), '<!doctype html><html><body style="margin:0;background:#000"><canvas id="wf" style="display:block"></canvas><script src="drv.js"></script></body></html>');

const dbg = await freePort();
fs.mkdirSync(path.join(dir, 'edge'));
// ★ WFFAN_GPU=1: the Mac's own GPU (for the CPU figures — SwiftShader draws WebGL on the CPU and swamps them)
const GL_FLAGS = process.env.WFFAN_GPU ? ['--use-angle=metal', '--enable-gpu'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'];
const edge = spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', ...GL_FLAGS,
  `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check', '--allow-file-access-from-files',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1400,1000', 'about:blank'], { stdio: 'ignore' });
const deadline = setTimeout(() => { console.log('   FAIL deadline (600 s)'); try { edge.kill(); } catch {} process.exit(1); }, 600000);

// ── the checks, on one picture ───────────────────────────────────────────────────────────────────────────────────────────
/** Every carrier in every row with a known geometry: where its geometry puts it, and where the picture has it. */
function analyse(p, from = 0, to = p.H, scaleSeamsOk = false) {
  const { W, lum, geo, carriers, black } = p;
  // the picture's floor and its top, over every drawn row: a carrier stands well above the floor
  const all = [];
  for (let y = from; y < to; y++) if (geo[y]) for (let x = 0; x < W; x += 7) all.push(lum[y][x]);
  all.sort((a, b) => a - b);
  const fl = all[all.length >> 1] ?? 0, top = all[Math.floor(all.length * 0.999)] ?? 255;
  const isLit = (v) => v - fl > Math.max(60, (top - fl) * 0.3);
  let placed = 0, misplaced = 0, slices = 0, blackIn = 0, rowsSeen = 0, joins = 0;
  const worst = [];
  const note = (s) => { if (worst.length < 6) worst.push(s); };
  for (let y = from; y < to; y++) {
    const g = geo[y];
    if (!g) continue;
    rowsSeen++;
    const [L, S, dLo, dHi, x0, x1] = g, row = lum[y];
    // black inside the row's own data (its stored extent on screen); outside it is a jump's newly exposed edge
    // ★ …and inside the row BELOW's too: the shader shows each line as a 50/50 blend of a row and the one below (the soft
    //   vertical look), so the last row before blank (the oldest drawn, or a jump's edge) is half-dark by design.
    const gb = geo[y + 1];
    if (!gb) continue;
    const bx0 = Math.max(0, Math.ceil(Math.max(x0, gb[4]) * W) + 1), bx1 = Math.min(W, Math.floor(Math.min(x1, gb[5]) * W) - 1);
    for (let x = bx0; x < bx1; x++) if (row[x] <= black + 2) { if (!blackIn) note(`black y${y} x${x} (data ${bx0}..${bx1})`); blackIn++; }
    const g1 = geo[y + 1];
    for (const c of carriers) {
      const fx = (c - L) / S * W;
      if (!(fx >= 4 && fx < W - 4) || fx < x0 * W + 4 || fx > x1 * W - 4) continue;
      const inData = c > dLo + 300 && c < dHi - 300;   // outside the frames' data the row continues the one below
      if (!inData) continue;
      // the carrier in the picture: lit within ±4 px of where its row's geometry puts it
      let bv = -1;
      for (let x = Math.max(0, Math.floor(fx) - 4); x <= Math.min(W - 1, Math.ceil(fx) + 4); x++) if (row[x] > bv) bv = row[x];
      if (isLit(bv)) placed++; else { misplaced++; note(`y${y}:${(c / 1e3).toFixed(1)}k@${fx.toFixed(0)}`); }
      // ★ NO SLICING: where the trail moves between this row and the one below, this row (or the one below) is lit ALL
      //   the way across the move — one solid line, no gap wider than 3 px.
      if (!g1) continue;
      const [L1, S1, dLo1, dHi1, x01, x11] = g1;
      const fx1 = (c - L1) / S1 * W;
      if (!(fx1 >= 4 && fx1 < W - 4) || fx1 < x01 * W + 4 || fx1 > x11 * W - 4 || !(c > dLo1 + 300 && c < dHi1 - 300)) continue;
      if (Math.abs(fx1 - fx) <= 3) continue;
      // ★ After a JUMP every row moves by the jump's Hz in ITS OWN scale (the app's rule — a row drawn zoomed out keeps
      //   its centre on the dial), so two rows drawn at different zooms move by different amounts: the seam there is the
      //   jump's, not a slice. (Only the jump-back stage asks for this.)
      if (scaleSeamsOk && Math.abs(S1 / S - 1) > 1e-6) continue;
      joins++;
      const a = Math.round(Math.min(fx, fx1)) + 2, b = Math.round(Math.max(fx, fx1)) - 2, r1 = lum[y + 1];
      let run = 0, worstRun = 0;
      for (let x = a; x <= b; x++) { if (isLit(row[x]) || isLit(r1[x])) run = 0; else if (++run > worstRun) worstRun = run; }
      if (worstRun > 3) { slices++; note(`slice y${y} ${(c / 1e3).toFixed(1)}k ${fx1.toFixed(0)}→${fx.toFixed(0)} (gap ${worstRun})`); }
    }
  }
  return { placed, misplaced, slices, blackIn, rowsSeen, joins, worst };
}
/** Brightest-carrier columns (fractions of the width) of a sample of rows — for the resize check. */
function trailFractions(p, rows) {
  return rows.map((y) => { const r = p.lum[Math.min(p.H - 1, y)]; const out = [];
    const srt = Array.from(r).sort((a, b) => a - b), fl = srt[srt.length >> 1], top = srt[srt.length - 1];
    for (let x = Math.ceil(p.W * 0.01); x < p.W * 0.99; x++) if (r[x] - fl > (top - fl) * 0.6 && r[x] >= r[x - 1] && r[x] > r[x + 1]) out.push(x / p.W);
    return out; });
}

try {
  await sleep(2500);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  const wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true });
    if (r.result?.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails).slice(0, 600)); return r.result?.result?.value; };
  await send('Page.enable'); await send('Runtime.enable');
  const cpu = {};
  for (const MODE of (process.env.WFFAN_MODES || 'webgl,2d').split(',')) {
    console.log(`   ── ${MODE} renderer ──`);
    // ★ WFFAN_DPR=2: a Retina canvas (two device rows a screen row — each its own step of an ease)
    await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 1000, deviceScaleFactor: Number(process.env.WFFAN_DPR) || 1, mobile: false });
    await send('Page.navigate', { url: 'file://' + path.join(dir, 'index.html') });
    await sleep(800);
    ok(await ev(`__wfDrv.setup(${JSON.stringify(MODE)}, 1000, 600)`), `drawing with the ${MODE} renderer`);
    const D = (x) => ev(`__wfDrv.${x}`);
    const pic = async () => { const p = await ev('__wfDrv.picture()'); let b64 = '';
      for (let o = 0; o < p.lum; o += 1 << 20) b64 += await ev(`__wfDrv.lum.slice(${o}, ${o + (1 << 20)})`);
      const b = Buffer.from(b64, 'base64');
      const all = new Uint16Array(b.buffer, b.byteOffset, b.length / 2); p.lum = [];
      for (let y = 0; y < p.H; y++) p.lum.push(all.subarray(y * p.W, (y + 1) * p.W)); return p; };
    const shot = async (name) => { if (!PNG) return; const u = await ev('__wfDrv.png()'); fs.writeFileSync(path.join(PNG, `${MODE}-${name}.png`), Buffer.from(u.split(',')[1], 'base64')); };
    const stage = async (name, act, what, histGone = false, seams = false) => {
      await D('resetCost()');
      const before = await ev('__wfDrv.rows.length');
      await act();
      await D('run(2200)');   // the new rows settle and the move scrolls into the middle of the picture
      const p = await pic();
      const a = analyse(p, 0, p.H, seams);
      if (process.env.WFFAN_DEBUG === name) for (let y = 55; y < 75; y++) console.log('      dbg', y, JSON.stringify(p.geo[y]), p.lum[y].slice(0, 6).join(','), JSON.stringify(await ev(`(() => { const w = __wfDrv.wf; const r = (w.head + ${y}) % w.xfA.length; return [w.xfA[r], w.xfB[r], w.xfS[r]]; })()`)));
      const after = await ev('__wfDrv.rows.length');
      const c = await D('costStats()'); cpu[`${MODE} ${name}`] = c;
      await shot(name);
      // ★ HISTORY PRESERVED: the rows from before the move are still on screen with their carriers at their own geometry
      //   (`histGone`: a stage whose old rows are expected off screen — none since a jump keeps the history, 2026-10-30)
      const newRows = after - before, old = analyse(p, Math.min(p.H, newRows + 2), p.H);
      const histOk = name === 'start' || (histGone ? old.placed === 0 : old.placed > 0);
      ok(a.misplaced <= a.placed * 0.01 && a.slices === 0 && a.blackIn === 0 && histOk,
         `${what}: ${a.placed} carrier-rows at their true frequency, ${a.misplaced} not; ${a.joins} moves, ${a.slices} sliced; ${a.blackIn} black px inside data; ` +
         `history from before: ${old.placed} carrier-rows in place${a.worst.length ? ' — ' + a.worst.join(', ') : ''}`);
      if (process.env.WFFAN_CPU) console.log(`        cpu ${name}: ${c.mean.toFixed(3)} ms/frame mean, p95 ${c.p95.toFixed(3)}, max ${c.max.toFixed(2)} (${c.n} frames)`);
      return a;
    };
    await stage('start', () => D('run(8000)'), 'a steady band');
    // ── zoom in, ×2 twice about the centre (the wheel / buttons), then out ×2 twice ──
    await stage('zoom-in', async () => { await D('ask(7150000, 48000)'); await D('run(700)'); await D('ask(7150000, 24000)'); }, 'zoom in ×2, ×2');
    await stage('zoom-out', async () => { await D('ask(7150000, 48000)'); await D('run(700)'); await D('ask(7150000, 96000)'); }, 'zoom out ×2, ×2');
    // ── a zoom about a point that is not the centre (a pinch / wheel at the pointer: the anchor 7.131 MHz holds) ──
    await stage('zoom-anchor', async () => { const a = 7131500, lo = 7102000, s0 = 96000, s1 = 64000;
      const x = (a - lo) / s0, c1 = a - x * s1 + s1 / 2; await D(`ask(${c1}, ${s1})`); await D('run(700)');
      await D(`ask(7150000, 96000)`); }, 'zoom about a pointer and back');
    // ── UberSDR-style rungs: the asks are a pinch's small steps, the frames come at the fixed Hz/bin rungs ──
    await D('rungs = true'); await D('ask(7150000, 102400)'); await D('run(1500)');
    await stage('rungs-in', () => D('sweep(10, 0, 0.87, 90)'), 'pinch in on snapped rungs (100 → 50 Hz a bin)');
    await stage('rungs-out', () => D('sweep(16, 0, 1.15, 90)'), 'pinch out on snapped rungs (→ 200 Hz a bin)');
    await D('rungs = false'); await D('ask(7150000, 96000)'); await D('run(1500)');
    // ── tunes ──
    await stage('tune-step', () => D('ask(7159000, 96000)'), 'a 9 kHz tune step');
    await stage('tune-spin', () => D('sweep(30, -500, 1, 60)'), 'a drum spin (500 Hz every 60 ms)');
    await stage('tune-large', () => D('ask(7104000, 96000)'), 'a large tune (40 kHz, inside the view)');
    // ★★★ A JUMP KEEPS THE HISTORY AS DRAWN (Stuart, 2026-10-30, #57: "can we preserve the history on longer tunes and zooms
    //     so it is not lost") — it used to move off with the band (histGone); now the old rows stay, stamped with their scale.
    await stage('jump', () => D('ask(7254000, 96000)'), 'a jump of more than the view (150 kHz): the history from before stays as drawn', false);
    await stage('jump-back', () => D('ask(7150000, 96000)'), 'and back: the history from before the jump is in place again', false, true);
    // ── resize, and a rotate ──
    { const p0 = await pic();
      // sample rows on STEADY stretches (the same trails 6 rows above and below — not a knee, where a row or two of
      // scroll during the resize would change what is there), spread down the picture
      const cand = []; for (let y = 10; y < 590; y += 12) cand.push(y);
      const fc = trailFractions(p0, cand), fu = trailFractions(p0, cand.map((y) => y - 6)), fd = trailFractions(p0, cand.map((y) => y + 6));
      const eq = (a, b) => a.length > 0 && a.length === b.length && a.every((v, i) => Math.abs(v - b[i]) < 0.004);
      const rowsAt = cand.filter((_, i) => eq(fc[i], fu[i]) && eq(fc[i], fd[i]));
      const f0 = trailFractions(p0, rowsAt);
      await D('resize(700, 600)'); await D('run(17)');   // one frame: the same history, narrower
      const p1 = await pic(); const f1 = trailFractions(p1, rowsAt);
      // the same trails: every peak in one has a peak within 0.6 % of the width in the other (a 2D rescale may merge two
      // peaks a pixel apart into one)
      const near = (a, b) => a.every((v) => b.some((w) => Math.abs(v - w) < 0.006));
      const same = (a, b) => a.length > 0 && b.length > 0 && near(a, b) && near(b, a);
      const n1 = rowsAt.filter((_, i) => same(f0[i], f1[i])).length;
      ok(rowsAt.length >= 15, `${rowsAt.length} steady rows down the picture to compare`);
      if (n1 !== rowsAt.length) rowsAt.forEach((y, i) => { if (!same(f0[i], f1[i])) console.log(`      row ${y}: ${f0[i].map((v) => v.toFixed(3)).join(' ')} | ${f1[i].map((v) => v.toFixed(3)).join(' ')}`); });
      await shot('resize');
      ok(n1 >= rowsAt.length * 0.9, `a resize 1000 → 700 px wide keeps the history: ${n1}/${rowsAt.length} sampled rows show the same trails at the same fractions`);
      await D('resize(600, 1000)'); await D('run(17)');   // a rotate: narrower and taller
      const p2 = await pic(); await shot('rotate');
      // GPU: the ring is taller than the display — the same rows stay at the top, more show below. 2D: the history is rescaled
      // to the new height, so row y now shows what row y·600/1000 did.
      const scale = MODE === '2d' ? 1000 / 600 : 1;
      const f2 = trailFractions(p2, rowsAt.map((y) => Math.round(y * scale)));
      const n2 = rowsAt.filter((_, i) => same(f0[i], f2[i])).length;
      if (n2 < rowsAt.length - 1) rowsAt.forEach((y, i) => { if (!same(f0[i], f2[i])) console.log(`      row ${y}: ${f0[i].map((v) => v.toFixed(3)).join(' ')} | ${f2[i].map((v) => v.toFixed(3)).join(' ')}`); });
      ok(n2 >= rowsAt.length * 0.9, `a rotate to portrait (600 × 1000) keeps the history: ${n2}/${rowsAt.length} sampled rows show the same trails`);
      await D('resize(1000, 600)'); await D('run(17)');
      const p3 = await pic(); const f3 = trailFractions(p3, rowsAt);
      const n3 = rowsAt.filter((_, i) => same(f0[i], f3[i])).length;
      ok(n3 >= rowsAt.length * 0.9, `…and back to landscape: ${n3}/${rowsAt.length} sampled rows as before`);
    }
  }
  // ★ CPU (WFFAN_LIVE=<seconds>): the real clock and rAF, a 1000 × 600 canvas, the WebGL renderer — main-thread ms a frame
  //   (tick + draw) and the whole browser's CPU over the same window (ps), steady, zooming and tuning.
  if (process.env.WFFAN_LIVE) {
    const secs = Number(process.env.WFFAN_LIVE) || 20;
    const cpuOf = () => { try { const out = (spawnSync('ps', ['-A', '-o', 'pid=,ppid=,time=']).stdout || '').toString();
        const rows = out.trim().split('\n').map((l) => l.trim().split(/\s+/)); const kids = new Map();
        for (const [p, pp] of rows) { if (!kids.has(pp)) kids.set(pp, []); kids.get(pp).push(p); }
        const tree = new Set([String(edge.pid)]); for (let ch = true; ch;) { ch = false; for (const p of [...tree]) for (const c of kids.get(p) || []) if (!tree.has(c)) { tree.add(c); ch = true; } }
        let t = 0; for (const [p, , tm] of rows) if (tree.has(p)) { const m = tm.split(':').map(Number); t += m.length === 3 ? m[0] * 3600 + m[1] * 60 + m[2] : m[0] * 60 + m[1]; }
        return t; } catch { return NaN; } };
    for (const pat of ['steady', 'zoom', 'tune']) {
      await send('Page.navigate', { url: 'file://' + path.join(dir, 'index.html') });
      await sleep(800);
      await ev(`__wfDrv.setup('webgl', 1000, 600)`);
      await ev(`__wfDrv.live(3000, 'steady')`);   // warm up: the ring fills, the JIT settles
      const c0 = cpuOf(), w0 = Date.now();
      const st = await ev(`__wfDrv.live(${secs * 1000}, ${JSON.stringify(pat)})`);
      const cpuPct = (cpuOf() - c0) / ((Date.now() - w0) / 1000) * 100;
      cpu[`live ${pat}`] = { ...st, browserCpuPct: cpuPct };
      console.log(`   cpu ${pat}: main thread ${st.mean.toFixed(3)} ms/frame mean, p95 ${st.p95.toFixed(2)}, max ${st.max.toFixed(2)} (${st.n} frames); browser ${cpuPct.toFixed(0)} % CPU`);
    }
  }
  if (process.env.WFFAN_CPU_JSON) fs.writeFileSync(process.env.WFFAN_CPU_JSON, JSON.stringify(cpu, null, 1));
} catch (e) { console.log('   FAIL', e?.message || e); fail++; }
clearTimeout(deadline);
try { edge.kill(); } catch {}
fs.rmSync(dir, { recursive: true, force: true });
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
