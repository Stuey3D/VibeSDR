// probe-mw-carrier.mjs — tune an MW carrier and report what the spectrum shows around it.
//   node scripts/probe-mw-carrier.mjs http://192.168.86.88:48001 [--freq 693k] [--secs 6]
// Takes the dial: run it only when the radio has no listeners.
import WebSocket from 'ws';
const base = (process.argv[2] || '').replace(/\/+$/, '');
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const hz = (s) => { const m = String(s).match(/^([\d.]+)([kM]?)$/i); return parseFloat(m[1]) * ({ '': 1, k: 1e3, m: 1e6 }[m[2].toLowerCase()]); };
const FREQ = hz(arg('--freq', '693k')), SECS = Number(arg('--secs', 6));
const SID = 'mwprobe-' + Date.now();
const wsBase = base.replace(/^http/, 'ws');
const ws = new WebSocket(`${wsBase}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=4096`);
const audio = new WebSocket(`${wsBase}/ws/audio?user_session_id=${SID}&codec=opus`);
audio.on('error', () => {}); audio.on('message', () => {});
let frames = [], centre = 0, binHz = 0, cfg = null, adc = [], hw = null;
ws.on('open', () => {
  ws.send(JSON.stringify({ type: 'tune', frequency: FREQ, mode: 'am' }));
  setTimeout(() => { frames = []; adc = []; }, 2500);
  setTimeout(report, 2500 + SECS * 1000);
});
ws.on('message', (d) => {
  const b = Buffer.isBuffer(d) ? d : Buffer.from(d);
  if (b.length >= 22 && b[0] === 0x53 && b[1] === 0x50 && b[2] === 0x45 && b[3] === 0x43) {
    const dv = new DataView(b.buffer, b.byteOffset, b.length);
    if (dv.getUint8(5) !== 0x03) return;
    centre = Number(dv.getBigUint64(14, true));
    const n = b.length - 22, half = n >> 1, f = new Float32Array(n);
    for (let i = 0; i < n; i++) f[i] = b[22 + ((i + half) % n)] - 256;
    frames.push(f); return;
  }
  let j; try { j = JSON.parse(String(d)); } catch { return; }
  if (j.type === 'config') { cfg = j; binHz = Number(j.binBandwidth) || binHz; }
  if (j.type === 'hwinfo') hw = j;
  if (j.type === 'adc') adc.push(j.peak);
});
function report() {
  if (!frames.length) { console.log('no spectrum frames'); process.exit(1); }
  const n = frames[0].length, avg = new Float32Array(n);
  for (const f of frames) for (let i = 0; i < n; i++) avg[i] += f[i] / frames.length;
  const bw = binHz || (cfg && cfg.binBandwidth);
  const at = (f) => Math.round((f - centre) / bw + n / 2);
  const sorted = Array.from(avg).sort((a, b) => a - b), floor = sorted[n >> 1];
  console.log(`frames ${frames.length}  centre ${(centre / 1e3).toFixed(1)} kHz  bin ${bw?.toFixed?.(1)} Hz  span ±${(bw * n / 2e3).toFixed(0)} kHz  floor(median) ${floor.toFixed(1)} dB`);
  console.log(`config: freq ${cfg?.frequency} mode ${cfg?.mode} ds=${hw?.ds ?? hw?.directSampling} gain=${hw?.gainNow} agcLocked=${hw?.agcLocked} adcPeak≈${adc.length ? Math.max(...adc).toFixed(1) : hw?.adcPeak}`);
  for (const k of [6950, 7050, 7100, 7150, 7300, 531, 603, 648, 693, 756, 909, 990, 1053, 1089, 1215, 1458]) {
    const i = at(k * 1e3); if (i < 3 || i >= n - 3) continue;
    let pk = -999; for (let x = i - 3; x <= i + 3; x++) pk = Math.max(pk, avg[x]);
    console.log(`  ${String(k).padStart(5)} kHz  ${pk.toFixed(1)} dB  (${(pk - floor).toFixed(1)} over floor)`);
  }
  const top = Array.from(avg).map((v, i) => [v, i]).sort((a, b) => b[0] - a[0]).slice(0, 8);
  console.log('strongest bins:', top.map(([v, i]) => `${((centre + (i - n / 2) * bw) / 1e3).toFixed(1)}k:${v.toFixed(0)}`).join('  '));
  ws.close(); audio.close(); process.exit(0);
}
setTimeout(() => { console.log('timeout'); process.exit(2); }, 30000);
