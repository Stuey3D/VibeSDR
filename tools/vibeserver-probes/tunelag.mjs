// Does tuning or zooming STALL the waterfall?
//
// ★★★ WHAT THE HEADER'S FREQUENCY FIELD ACTUALLY IS. `[14..21]` is the VIEW CENTRE
//     (`f = llround(view.centre)` in onSpectrum), NOT the tuned VFO. On a centre-locked shared
//     radio a `tune` does not move it at all. An earlier probe read it as "the frequency this
//     frame is for" and reported a 4.1 s tune latency that was nothing of the kind — it was the
//     shared view centre moving for an unrelated reason. Measure the stall, which is what the eye
//     actually sees, and treat the centre as corroboration rather than as the clock.
//
// Three cases, because they fail differently:
//   1. a single TUNE      — the demodulator retunes; does the picture keep coming?
//   2. a single ZOOM      — the view pipeline REBUILDS, the documented hitch risk (viewPriming)
//   3. a DRAG            — 20 messages in 2 s, which is what a finger on the tuner really sends,
//                          and the case a per-message rebuild turns into a stutter
//
// Usage: RADIO=<id> node tunelag.mjs wss://demo.vibesdr.net
import crypto from 'node:crypto';
const BASE = process.argv[2] || 'wss://demo.vibesdr.net';
const RADIO = process.env.RADIO || '';
const A = Number(process.env.TUNE_A || 6070000);
const MODE = process.env.MODE || 'usb';
const SPAN = Number(process.env.SPAN || 200000);      // view span for the zoom case

const SID = 'tl' + crypto.randomBytes(4).toString('hex');
const ws = new WebSocket(`${BASE}${RADIO ? `/r/${RADIO}` : ''}`
  + `/ws/user-spectrum?user_session_id=${SID}&bins=1024&mode=binary8`);
ws.binaryType = 'arraybuffer';

const frames = [];      // {t, centre}
const marks = [];       // {t, what}
let t0 = 0;
ws.onmessage = m => {
  const t = performance.now();
  if (typeof m.data === 'string') return;
  const b = new Uint8Array(m.data);
  if (b.length < 22 || b[0] !== 0x53 || b[1] !== 0x50 || b[2] !== 0x45 || b[3] !== 0x43) return;
  const dv = new DataView(m.data);
  if (!t0) t0 = t;
  frames.push({ t: t - t0, centre: Number(dv.getBigUint64(14, true)) });
};
ws.onerror = e => console.log('  ws error:', e.message || e.type);
ws.onclose = e => { if (!done) console.log(`  closed early: ${e.code}`); };

const send = o => ws.send(JSON.stringify(o));
const wait = ms => new Promise(r => setTimeout(r, ms));
const mark = what => { marks.push({ t: performance.now() - t0, what }); };

let done = false;
ws.onopen = async () => {
  await wait(500);
  send({ type: 'tune', frequency: A, mode: MODE });
  await wait(8000);                                     // settle: the baseline cadence

  mark('TUNE (single)');
  send({ type: 'tune', frequency: A + 40000, mode: MODE });
  await wait(6000);

  mark('ZOOM (single)');
  send({ type: 'zoom', frequency: A + 40000, binBandwidth: SPAN / 1024 });
  await wait(6000);

  // ★ The real complaint is a finger on the dial, not one message in isolation.
  mark('DRAG (20 msgs / 2s)');
  for (let i = 0; i < 20; i++) {
    send({ type: 'zoom', frequency: A + 40000 + i * 2000, binBandwidth: SPAN / 1024 });
    await wait(100);
  }
  await wait(6000);

  done = true; ws.close(); report();
};

const pct = (a, p) => a.length ? a.slice().sort((x, y) => x - y)[Math.min(a.length - 1, Math.floor(a.length * p))] : 0;
const mean = a => a.reduce((s, v) => s + v, 0) / (a.length || 1);

function report() {
  console.log(`\n=== tune/zoom lag: ${BASE} radio=${RADIO || '(single)'} — ${frames.length} frames ===`);
  if (frames.length < 30 || !marks.length) { console.log('  too few frames'); process.exit(1); }

  // Baseline: the undisturbed cadence before the first command.
  const base = frames.filter(f => f.t < marks[0].t);
  const bg = []; for (let i = 1; i < base.length; i++) bg.push(base[i].t - base[i - 1].t);
  const bm = mean(bg);
  console.log(`\n  baseline cadence: mean ${bm.toFixed(1)} ms   p95 ${pct(bg, .95).toFixed(1)}   MAX ${Math.max(...bg).toFixed(1)} ms`);

  for (let k = 0; k < marks.length; k++) {
    const from = marks[k].t;
    const to = k + 1 < marks.length ? marks[k + 1].t : Infinity;
    const win = frames.filter(f => f.t >= from && f.t < to);
    console.log(`\n  ${marks[k].what}`);
    if (win.length < 2) { console.log('    no frames in the window'); continue; }
    const g = []; for (let i = 1; i < win.length; i++) g.push(win[i].t - win[i - 1].t);
    // ★ The gap ACROSS the command matters most: a rebuild shows up as one long hole between the
    //   last frame before the message and the first one after it, not as a raised average.
    const prev = frames.filter(f => f.t < from).pop();
    const across = prev ? win[0].t - prev.t : 0;
    console.log(`    gap across the command: ${across.toFixed(0)} ms   (baseline ${bm.toFixed(0)} ms)`);
    console.log(`    cadence after: mean ${mean(g).toFixed(1)} ms   MAX ${Math.max(...g).toFixed(1)} ms`);
    const stalls = g.filter(x => x > 2 * bm);
    console.log(`    stalls >2x baseline: ${stalls.length}`
      + (stalls.length ? `  (worst ${Math.max(...stalls).toFixed(0)} ms)` : ''));
    const moved = win.find(f => prev && f.centre !== prev.centre);
    console.log(`    view centre ${moved ? `moved after ${(moved.t - from).toFixed(0)} ms` : 'did NOT move (expected for tune on a centre-locked radio)'}`);
    // The verdict a person cares about: would this have been visible?
    const worst = Math.max(across, ...g);
    console.log(`    ${worst > 250 ? '★ VISIBLE STALL' : worst > 2 * bm ? 'brief hitch' : 'no stall — smooth through it'}`
      + ` (worst hole ${worst.toFixed(0)} ms)`);
  }
  process.exit(0);
}
