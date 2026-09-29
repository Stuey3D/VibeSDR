/**
 * test-web-playout-burst.mjs — does the web client's playout node come back CLEAN by itself after
 * the server has been delivering audio in bursts?
 *
 * ★ The field case (Stuart, Sony, V11 b4, 2026-09-29): a narrow WFM passband pushed the server's
 *   DSP to 161-276 % of real time — "bursts of fast noisy audio with about a 1 second gap". Widening
 *   fixed the server (load back to 31 %, backlog 0), yet the audio still sounded wrong until a
 *   browser refresh. So the question for the CLIENT is whether anything the burst episode did to
 *   it can outlive the episode: a grown buffer that keeps trimming, a node stuck re-arming, a
 *   target that never comes down.
 * ★ This runs the REAL worklet source (WORKLET_SRC, cut out of web/client/src/audio.ts with its
 *   constants substituted), against a stub AudioWorkletProcessor, driven sample-exactly: the
 *   device pulls 128 frames per quantum at 48 kHz, the server pushes 20 ms Opus frames. Every
 *   sample carries its own index, so a trim, a skip or a re-arm shows up as a discontinuity in
 *   what comes out — the thing a listener hears — not as a counter we have to trust.
 * ★ SILENT: nothing touches an audio device.
 *
 * Phases: 20 s normal → 20 s of the overload (audio produced at ~40 % of real time, delivered as a
 *   burst every ~1 s, the server's 256 ms backlog) → normal again. After a settling window the
 *   last 30 s must be perfectly continuous: no underrun, no skip, every sample in order.
 */
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const src = readFileSync(path.join(root, 'web/client/src/audio.ts'), 'utf8');

const num = (name) => {
  const m = src.match(new RegExp(`const ${name}\\s*=\\s*([0-9.]+)`));
  if (!m) throw new Error(`audio.ts: ${name} not found`);
  return Number(m[1]);
};
const m = src.match(/const WORKLET_SRC = `([\s\S]*?)`;/);
if (!m) throw new Error('audio.ts: WORKLET_SRC not found');
const body = m[1].replace(/\$\{(\w+)\}/g, (_, n) => String(num(n)));

let failures = 0, checks = 0;
const ok = (cond, what) => { checks++; if (!cond) failures++; console.log(`   ${cond ? 'ok  ' : 'FAIL'} ${what}`); };

function makeNode() {
  let Klass = null;
  const posted = [];
  class AudioWorkletProcessor { constructor() { this.port = { postMessage: (d) => posted.push(d), onmessage: null }; } }
  const registerProcessor = (_n, k) => { Klass = k; };
  new Function('AudioWorkletProcessor', 'registerProcessor', body)(AudioWorkletProcessor, registerProcessor);
  const node = new Klass();
  return { node, posted };
}

const RATE = 48000, Q = 128, FRAME = 960;

/** Run one scenario. `produce(tMs)` says how many 20 ms frames the server hands over at that ms. */
function run(name, schedule, totalMs, cleanFromMs, drift = 1.0) {
  console.log(`\n── ${name}`);
  const { node, posted } = makeNode();
  let nextSample = 0;                        // the server's sample counter (its content)
  let expect = -1;                           // what the listener should hear next
  let underruns = 0, skips = 0, maxTarget = 0;
  let cleanDisc = 0, cleanSilentQ = 0, cleanUnder = 0, cleanSkip = 0;
  const outL = new Float32Array(Q), outR = new Float32Array(Q);
  let deviceT = 0;                           // device samples played
  let serverCredit = 0;                      // frames owed by the server's clock
  for (let ms = 0; ms < totalMs; ms++) {
    // server side: frames produced this millisecond (drift = server clock / device clock)
    serverCredit += schedule(ms) * drift;
    while (serverCredit >= 1) {
      serverCredit -= 1;
      const l = new Float32Array(FRAME), r = new Float32Array(FRAME);
      for (let i = 0; i < FRAME; i++) { l[i] = r[i] = (nextSample + i) / 1e7; }
      nextSample += FRAME;
      node.onAudioMsg({ data: { l, r } });
    }
    // device side: 48 samples per ms, in quanta of 128
    const due = Math.floor(((ms + 1) * RATE) / 1000);
    while (deviceT + Q <= due) {
      node.process([], [[outL, outR]]);
      deviceT += Q;
      const inClean = ms >= cleanFromMs;
      let silent = true;
      for (let i = 0; i < Q; i++) {
        const v = Math.round(outL[i] * 1e7);
        if (outL[i] !== 0) silent = false;
        if (outL[i] === 0) continue;
        if (expect >= 0 && v !== expect && inClean) cleanDisc++;
        expect = v + 1;
      }
      if (silent && inClean) cleanSilentQ++;
    }
    for (const d of posted.splice(0)) {
      if (typeof d.underruns === 'number') { if (ms >= cleanFromMs && d.underruns > underruns) cleanUnder += d.underruns - underruns; underruns = d.underruns; }
      if (typeof d.skips === 'number') { if (ms >= cleanFromMs && d.skips > skips) cleanSkip += d.skips - skips; skips = d.skips; }
    }
    maxTarget = Math.max(maxTarget, node.target);
  }
  const finalMs = Math.round(node.target / 48);
  console.log(`   episode: ${underruns} underruns, ${skips} skips, cushion peaked at ${Math.round(maxTarget / 48)} ms, ends at ${finalMs} ms`);
  ok(cleanUnder === 0, `no underrun in the last ${(totalMs - cleanFromMs) / 1000} s (got ${cleanUnder})`);
  ok(cleanSkip === 0, `no skip/trim in the last ${(totalMs - cleanFromMs) / 1000} s (got ${cleanSkip})`);
  ok(cleanDisc === 0, `every sample in order in the last ${(totalMs - cleanFromMs) / 1000} s (${cleanDisc} discontinuities)`);
  ok(cleanSilentQ === 0, `no silent quanta in the last ${(totalMs - cleanFromMs) / 1000} s (got ${cleanSilentQ})`);
  ok(node.started, 'the node is playing at the end');
  return node;
}

// Normal: one frame every 20 ms. Overload: ~40 % of real time, held back and released as one burst
// roughly every second (the server's queue flushing), with a jittery period.
const normal = (ms) => (ms % 20 === 0 ? 1 : 0);
let burstAt = 0;
const overload = (ms) => {
  if (ms >= burstAt) { burstAt = ms + 900 + ((ms * 7919) % 400); return 20; }   // 20 frames = 400 ms of audio
  return 0;
};
const phases = (a, b) => (ms) => (ms < a || ms >= b ? normal(ms) : overload(ms));

run('burst episode, then normal flow (server clock = device clock)', phases(20000, 40000), 110000, 80000);
burstAt = 0;
run('burst episode, then normal flow (server clock 300 ppm FAST)', phases(20000, 40000), 110000, 80000, 1.0003);
burstAt = 0;
run('burst episode, then normal flow (server clock 300 ppm SLOW)', phases(20000, 40000), 110000, 80000, 0.9997);

/* ── The main-thread FALLBACK (ScriptProcessor + AudioPlayer's own ring) ──────────────────────
 * ★ The path a page served over plain http to a LAN address runs (no AudioWorklet there). It had
 *   neither of the worklet's recoveries: no latency bound (only the 2 s overflow) and no decay of
 *   the grown cushion. The real AudioPlayer methods are driven against a stub AudioContext. */
const { build } = await import('esbuild');
const bundled = await build({
  stdin: { contents: `export { AudioPlayer } from './web/client/src/audio';`, resolveDir: root, loader: 'ts' },
  bundle: true, format: 'esm', platform: 'neutral', write: false, logLevel: 'error',
  // ★ Packages (the WASM decoders) are never reached by these two methods — stub them out.
  plugins: [{ name: 'stub', setup(b) {
    b.onResolve({ filter: /^[^./]/ }, (a) => ({ path: a.path, namespace: 'stub' }));
    b.onLoad({ filter: /.*/, namespace: 'stub' }, () => ({ contents: 'module.exports = {};', loader: 'js' }));
  } }],
});
const { AudioPlayer } = await import('data:text/javascript;base64,' + Buffer.from(bundled.outputFiles[0].text).toString('base64'));

function runFallback(name, schedule, totalMs, cleanFromMs) {
  console.log(`\n── fallback path: ${name}`);
  const p = Object.create(AudioPlayer.prototype);
  Object.assign(p, { cap: 48000 * 2, wPos: 0, rPos: 0, filled: 0, playing: false, armedByFlush: false,
                     cleanFor: 0, playedTotal: 0, underruns: 0, skips: 0, jitterMs: Math.round(num('JITTER_SEC') * 1000),
                     gain: {}, _connectOutput() {} });
  let sp = null;
  p.ctx = { createScriptProcessor: () => (sp = { connect() {} }) };
  p._startScriptProcessor();
  const BLK = 4096;
  const outL = new Float32Array(BLK), outR = new Float32Array(BLK);
  const ev = { outputBuffer: { getChannelData: (c) => (c ? outR : outL) } };
  let nextSample = 0, expect = -1, deviceT = 0, peakMs = 0, maxLagMs = 0;
  let cleanDisc = 0, cleanSilent = 0, u0 = 0, s0 = 0;
  for (let ms = 0; ms < totalMs; ms++) {
    for (let k = schedule(ms); k > 0; k--) {
      const l = new Float32Array(FRAME), r = new Float32Array(FRAME);
      for (let i = 0; i < FRAME; i++) l[i] = r[i] = (nextSample + i) / 1e7;
      nextSample += FRAME;
      p._pushRing(l, r);
    }
    if (ms === cleanFromMs) { u0 = p.underruns; s0 = p.skips; }
    const due = Math.floor(((ms + 1) * RATE) / 1000);
    while (deviceT + BLK <= due) {
      sp.onaudioprocess(ev);
      deviceT += BLK;
      let silent = true;
      for (let i = 0; i < BLK; i++) {
        if (outL[i] === 0) continue;
        silent = false;
        const v = Math.round(outL[i] * 1e7);
        if (expect >= 0 && v !== expect && ms >= cleanFromMs) cleanDisc++;
        expect = v + 1;
      }
      if (silent && ms >= cleanFromMs) cleanSilent++;
    }
    peakMs = Math.max(peakMs, p.jitterMs);
    if (ms >= cleanFromMs) maxLagMs = Math.max(maxLagMs, p.filled / 48);
  }
  const win = (totalMs - cleanFromMs) / 1000;
  console.log(`   episode: cushion peaked at ${peakMs} ms, ends at ${p.jitterMs} ms; worst queue in the last ${win} s ${Math.round(maxLagMs)} ms`);
  ok(p.underruns === u0, `no underrun in the last ${win} s (got ${p.underruns - u0})`);
  // ★ ONE trim is allowed here, and it is the recovery itself: when the cushion steps down, a queue
  //   the catch-up burst left above the new ceiling is dropped back to the target — one deliberate
  //   jump to give the lag back, exactly what the worklet does. More than one is a node fighting.
  ok(p.skips - s0 <= 1, `at most one latency trim in the last ${win} s (got ${p.skips - s0})`);
  ok(cleanDisc <= p.skips - s0 && cleanSilent === 0, `no silence and no jump but that trim in the last ${win} s (${cleanDisc} jumps, ${cleanSilent} silent blocks)`);
  // ★ The lingering fault this guards: a burst left seconds queued and nothing drained it.
  ok(maxLagMs <= 2.5 * peakMs + 4096 / 48, `queue bounded by the latency rule afterwards (${Math.round(maxLagMs)} ms)`);
  ok(p.jitterMs < peakMs, `the grown cushion comes back down (${peakMs} -> ${p.jitterMs} ms)`);
}

// The jittery overload, ending in one big catch-up burst (the server's backlog plus held audio released at
// once), then the jittery overload, then normal flow long enough for one decay step.
burstAt = 0;
const bigBurst = (ms) => (ms === 40000 ? 75 : 0);     // 1.5 s of audio in one go, as the overload ends
runFallback('burst episode, then normal flow',
  (ms) => bigBurst(ms) + (ms < 20000 || ms >= 40000 ? normal(ms) : overload(ms)), 110000, 80000);

console.log(`\n${failures ? 'FAILED' : 'all ok'} (${checks - failures}/${checks})`);
process.exit(failures ? 1 : 0);
