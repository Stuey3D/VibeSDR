/**
 * agc-framing.mjs — does moving the RF CENTRE away from the loudest interferer actually pay,
 *
 * ★ binBandwidth 2400, not 1200: 1024 bins x 1200 Hz is a 1.23 MHz VIEW, so pushing the centre
 *   960 kHz put the station being measured off the screen and every separation reading came back
 *   empty. The CAPTURE is 2.4 MHz; the view has to be able to hold the station wherever the centre
 *   goes.
 * across the band, or only at the one station where it was found?
 *
 * ★★★ THE RULE UNDER TEST. The capture is centre +/- rate/2 and the VFO must stay inside it with
 *     margin, so the centre can move about +/-960 kHz at 2.4 MS/s. Within that, put it as FAR
 *     from the strongest signal as possible: push UP when the offender is below, DOWN when above.
 * ★★★ MEASURED AT A PINNED GAIN, so the only thing that changes is where the tuner sits. Reports
 *     the ADC peak (what the converter is being asked to swallow) and the channel-minus-shoulder
 *     separation (what the listener gets).
 */
import WebSocket from 'ws';
const base = process.argv[2];
const SID = 'fr-' + Date.now();
const ws = new WebSocket(`${base}/ws/user-spectrum?sessionId=${SID}`);
const audio = new WebSocket(`${base}/ws/audio?user_session_id=${SID}&codec=opus`);
audio.on('error', () => {}); audio.on('message', () => {});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const send = (o) => ws.send(JSON.stringify(o));
let frames = [], binHz = 0, peak = null, clip = null, rfC = 0;
const U8_OFF = -256;
ws.on('message', (d) => {
  const b = Buffer.isBuffer(d) ? d : Buffer.from(d);
  if (b.length >= 22 && b[0] === 0x53 && b[1] === 0x50 && b[2] === 0x45 && b[3] === 0x43) {
    const dv = new DataView(b.buffer, b.byteOffset, b.length);
    if (dv.getUint8(5) !== 0x03) return;
    const n = b.length - 22, half = n >> 1;
    const a = new Float32Array(n);
    for (let i = 0; i < n; i++) a[i] = b[22 + ((i + half) % n)] + U8_OFF;
    frames.push(a); return;
  }
  try { const j = JSON.parse(String(d));
    if (j.type === 'config') binHz = Number(j.binBandwidth) || binHz;
    if (j.type === 'adc') { peak = j.peak; clip = j.clip; }
    if (j.type === 'hwinfo' && Number.isFinite(j.rfCentre)) rfC = j.rfCentre;
  } catch {}
});
const measure = (vfoHz, viewCentreHz) => {
  if (!frames.length || !binHz) return null;
  const n = frames[0].length, avg = new Float64Array(n);
  for (const f of frames) for (let i = 0; i < n; i++) avg[i] += f[i];
  for (let i = 0; i < n; i++) avg[i] /= frames.length;
  const mid = n >> 1;
  const at = (hz) => mid + Math.round((hz - viewCentreHz) / binHz);
  const band = (loHz, hiHz) => { const o = [];
    for (let k = Math.max(0, at(loHz)); k <= Math.min(n - 1, at(hiHz)); k++) o.push(avg[k]); return o; };
  const pct = (a, p) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length * p)] ?? -120; };
  const chan = band(vfoHz - 100e3, vfoHz + 100e3);
  const sh = [...band(vfoHz - 400e3, vfoHz - 120e3), ...band(vfoHz + 120e3, vfoHz + 400e3)];
  if (!chan.length || !sh.length) return null;
  const c = chan.reduce((a, b) => a + b, 0) / chan.length;
  // The loudest thing in the capture that is NOT the wanted channel — the offender.
  let worst = -999, worstHz = 0;
  for (let k = 0; k < n; k++) {
    const hz = viewCentreHz + (k - mid) * binHz;
    if (Math.abs(hz - vfoHz) < 250e3) continue;
    if (avg[k] > worst) { worst = avg[k]; worstHz = hz; }
  }
  return { sep: c - pct(sh, 0.5), worstHz, worstDb: worst };
};
(async () => {
  await sleep(2500);
  send({ type: 'adcstats', seconds: 600 });
  send({ type: 'gain', value: 250 });
  send({ type: 'tunerbw', value: 0 });
  console.log('  gain pinned 25.0 dB, IF wide. "pushed" = RF centre moved 960 kHz away from the offender.\n');
  console.log('  station    offender     centre        peak    sep      centre       peak    sep     gain');
  for (const f of [96.1e6, 97.2e6, 103.6e6, 104.7e6, 105.4e6, 105.7e6, 106.0e6]) {
    send({ type: 'zoom', frequency: f, binBandwidth: 2400 });
    send({ type: 'tune', frequency: f, mode: 'wfm' });
    frames = []; await sleep(6000);
    const a = measure(f, f);
    if (!a) { console.log(`  ${(f/1e6).toFixed(1)} — no spectrum`); continue; }
    const aPeak = peak, aSep = a.sep;
    const push = a.worstHz < f ? +960e3 : -960e3;
    const vc = f + push;
    send({ type: 'zoom', frequency: vc, binBandwidth: 2400 });
    send({ type: 'tune', frequency: f, mode: 'wfm' });
    frames = []; await sleep(7000);
    const b = measure(f, vc);
    const bPeak = peak, bSep = b ? b.sep : NaN;
    const d = (x) => Number.isFinite(x) ? x.toFixed(1).padStart(5) : '  -  ';
    console.log(`  ${(f/1e6).toFixed(1)}     ${(a.worstHz/1e6).toFixed(2)}      `
      + `${(f/1e6).toFixed(2)}   ${String(aPeak).padStart(6)}  ${d(aSep)}    `
      + `${(vc/1e6).toFixed(2)}   ${String(bPeak).padStart(6)}  ${d(bSep)}   ${d(bSep - aSep)}`);
  }
  send({ type: 'gain', auto: true });
  await sleep(600); ws.close(); audio.close(); process.exit(0);
})();
