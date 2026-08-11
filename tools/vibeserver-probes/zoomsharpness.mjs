// How many REAL data points are in a zoomed waterfall row?
//
// ★★★ "BLOCKY" IS A COUNT, NOT AN OPINION. A row is always `bins` wide on the wire, but when the
//     view is narrower than the underlying FFT resolution the same source bin is written to many
//     adjacent output bins — a staircase. Counting the PLATEAUS (runs of identical values) gives
//     the number of genuine points actually on screen, and dividing the view span by it gives the
//     true resolution as opposed to the advertised one.
//
//     Interpolated:  1024 bins on the wire, ~24 plateaus  -> 223 Hz of real resolution
//     Real bins:     1024 bins on the wire, ~1024 plateaus -> the span/1024 it claims
//
// ★★ This is the number to compare between two machines. "It looks less blocky on the Mac" and
//    "it looks blocky on the Pi" cannot be held side by side; 24 plateaus versus 900 can.
//    Run it against each host with the SAME radio and the same view span.
//
// ★ The signal content does not matter — plateau count is a property of the DSP path, not of what
//   is on the air, so a quiet band and a busy one give the same answer.
//
// Usage: RADIO=<id> FREQ=60000 SPAN=5000 node zoomsharpness.mjs wss://demo.vibesdr.net
//        FREQ=9410000 SPAN=5000 node zoomsharpness.mjs ws://127.0.0.1:48000     (a Mac host)
import crypto from 'node:crypto';
const BASE = process.argv[2] || 'ws://127.0.0.1:48000';
const RADIO = process.env.RADIO || '';
const FREQ = Number(process.env.FREQ || 9410000);
const SPAN = Number(process.env.SPAN || 5000);      // the view width to zoom to, Hz
const MODE = process.env.MODE || 'am';
const BINS = Number(process.env.BINS || 1024);

const SID = 'zs' + crypto.randomBytes(4).toString('hex');
const ws = new WebSocket(`${BASE}${RADIO ? `/r/${RADIO}` : ''}`
  + `/ws/user-spectrum?user_session_id=${SID}&bins=${BINS}&mode=binary8`,
  // ★ Identify ourselves — see the note in jitterprobe.mjs. A probe filed as an unknown
  //   client inflates the very statistics an owner reads to judge real usage.
  { headers: { 'User-Agent': 'VibeSDR-probe/1.0 (zoomsharpness)' } });
ws.binaryType = 'arraybuffer';

let cfg = null; const rows = [];
ws.onmessage = m => {
  if (typeof m.data === 'string') {
    try { const j = JSON.parse(m.data);
      if (j.binBandwidth || j.centerFreq) cfg = j;
      if (j.type === 'busy') console.log(`  server says BUSY (queue ${j.queuePos}) — the radio is in use`);
    } catch {}
    return;
  }
  const b = new Uint8Array(m.data);
  if (b.length < 22 || b[0] !== 0x53 || b[1] !== 0x50 || b[2] !== 0x45 || b[3] !== 0x43) return;
  rows.push(b.slice(22));
};
ws.onerror = e => console.log('  ws error:', e.message || e.type);

const wait = ms => new Promise(r => setTimeout(r, ms));
ws.onopen = async () => {
  await wait(400);
  ws.send(JSON.stringify({ type: 'tune', frequency: FREQ, mode: MODE }));
  await wait(2500);
  // Zoom to the requested view. binBandwidth is the wire's way of expressing a span: span/bins.
  ws.send(JSON.stringify({ type: 'zoom', frequency: FREQ, binBandwidth: SPAN / BINS }));
  await wait(1500);
  rows.length = 0;                       // discard everything from before the zoom settled
  await wait(4000);
  report();
};

function report() {
  console.log(`\n=== zoom sharpness: ${BASE} radio=${RADIO || '(single)'} ===`);
  if (!rows.length) { console.log('  no frames (radio busy, or it never tuned)'); process.exit(1); }
  // ★ Median plateau count over several rows: one row could be flat because the band is quiet,
  //   and a single noisy row could break plateaus that are really there.
  const counts = rows.slice(-12).map(r => {
    let p = 1;
    for (let i = 1; i < r.length; i++) if (r[i] !== r[i - 1]) p++;
    return p;
  }).sort((a, b) => a - b);
  const plateaus = counts[Math.floor(counts.length / 2)];
  const n = rows[rows.length - 1].length;
  console.log(`  view ${(SPAN / 1000).toFixed(2)} kHz across ${n} wire bins`);
  console.log(`  server says binBandwidth ${cfg?.binBandwidth ?? '?'} Hz`);
  console.log(`  distinct plateaus: ${plateaus} of ${n}   (${(plateaus / n * 100).toFixed(1)}% of the wire is real)`);
  console.log(`  TRUE resolution:  ${(SPAN / plateaus).toFixed(1)} Hz per real point`
    + `   vs ${(SPAN / n).toFixed(1)} Hz advertised`);
  console.log(`  ${plateaus > n * 0.7 ? '★ SHARP — real bins at this zoom'
    : plateaus < n * 0.1 ? `★ BLOCKY — each real point is stretched over ~${Math.round(n / plateaus)} pixels`
    : 'partially interpolated'}`);
  process.exit(0);
}
