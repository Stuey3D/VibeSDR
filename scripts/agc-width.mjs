/**
 * agc-width.mjs — how WIDE is a station, as the gain changes?
 *
 * ★★★ WHY. Every metric the AGC steers by is a RATIO — channel over shoulders, peak over floor —
 *     and every one is blind to the failure that matters here: when the front end compresses, the
 *     channel AND its surroundings rise together, so the ratio holds while the band turns to
 *     porridge. Stuart's screenshots show stations 2-3x wider than a 200 kHz FM channel while the
 *     AGC reports everything is fine.
 * ★★★ WIDTH IS NOT A RATIO. A real broadcast FM channel is 200 kHz wide and STAYS 200 kHz wide
 *     however the gain moves. If it grows, that growth is ours. Nothing about the band can make a
 *     station wider.
 */
import WebSocket from 'ws';
const base = process.argv[2], FREQ = parseFloat(process.argv[3]) * 1e6, SID = 'w-' + Date.now();
const ws = new WebSocket(`${base}/ws/user-spectrum?sessionId=${SID}`);
const audio = new WebSocket(`${base}/ws/audio?user_session_id=${SID}&codec=opus`);
audio.on('error', () => {}); audio.on('message', () => {});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const send = (o) => ws.send(JSON.stringify(o));
let frames = [], binHz = 0, gains = [], pk = null;
ws.on('message', (d) => {
  const b = Buffer.isBuffer(d) ? d : Buffer.from(d);
  if (b.length >= 22 && b[0] === 0x53 && b[1] === 0x50 && b[2] === 0x45 && b[3] === 0x43) {
    const dv = new DataView(b.buffer, b.byteOffset, b.length);
    if (dv.getUint8(5) !== 0x03) return;
    const n = b.length - 22, half = n >> 1, a = new Float32Array(n);
    for (let i = 0; i < n; i++) a[i] = b[22 + ((i + half) % n)] - 256;
    frames.push(a); return;
  }
  try { const j = JSON.parse(String(d));
    if (j.type === 'config') binHz = Number(j.binBandwidth) || binHz;
    if (j.type === 'hwinfo' && Array.isArray(j.gains) && j.gains.length) gains = j.gains.slice();
    if (j.type === 'adc') pk = j.peak;
  } catch {}
});
(async () => {
  await sleep(2500);
  send({ type: 'adcstats', seconds: 600 });
  send({ type: 'zoom', frequency: FREQ, binBandwidth: 1200 });
  send({ type: 'tune', frequency: FREQ, mode: 'wfm' });
  send({ type: 'tunerbw', value: 0 });
  await sleep(3000);
  console.log(`  ${(FREQ/1e6).toFixed(1)} MHz — a broadcast FM channel is 200 kHz wide.\n`);
  console.log('   gain      peak     -3dB width   -10dB width   verdict');
  for (const g of gains.filter((x) => x % 1 === 0)) {
    send({ type: 'gain', value: g });
    await sleep(600); frames = []; await sleep(3500);
    if (!frames.length || !binHz) { console.log(`  ${(g/10).toFixed(1)} — no spectrum`); continue; }
    const n = frames[0].length, avg = new Float64Array(n);
    for (const f of frames) for (let i = 0; i < n; i++) avg[i] += f[i];
    for (let i = 0; i < n; i++) avg[i] /= frames.length;
    const mid = n >> 1;
    let top = -999; for (let k = mid - 40; k <= mid + 40; k++) if (avg[k] > top) top = avg[k];
    const w = (drop) => {
      let lo = mid, hi = mid;
      while (lo > 1 && avg[lo] > top - drop) lo--;
      while (hi < n - 2 && avg[hi] > top - drop) hi++;
      return (hi - lo) * binHz / 1e3;
    };
    const w3 = w(3), w10 = w(10);
    const bad = w3 > 320 ? '  <-- SMEARED' : '';
    console.log(`  ${(g/10).toFixed(1).padStart(5)}   ${String(pk).padStart(6)}    ${w3.toFixed(0).padStart(6)} kHz   ${w10.toFixed(0).padStart(6)} kHz${bad}`);
  }
  send({ type: 'gain', auto: true });
  await sleep(600); ws.close(); audio.close(); process.exit(0);
})();
