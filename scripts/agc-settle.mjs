/**
 * agc-settle.mjs — tune a receiver and watch where its AGC actually settles.
 *
 *   node scripts/agc-settle.mjs ws://192.168.86.88:48001 96.1M 90.1M 105.4M 106.0M
 *
 * ★★ The companion to agc-sweep.mjs, and the other half of the question. The sweep turns the AGC
 *    OFF and measures what every gain does; this leaves it ON and records what it CHOOSES. Put the
 *    two side by side and "is the loop finding the right answer?" stops being a matter of opinion.
 * ★ It opens the audio socket as well, because a spectrum-only client has no demodulator and
 *   therefore no RDS, no stereo and no separation worth reading.
 */
import WebSocket from 'ws';
const [base, ...freqs] = process.argv.slice(2);
if (!base || !freqs.length) { console.error('usage: node scripts/agc-settle.mjs ws://host:port 96.1M [more…]'); process.exit(1); }
const hz = (t) => { const m = String(t).match(/^([\d.]+)\s*([kKmMgG])?$/); return m ? parseFloat(m[1]) * ({k:1e3,m:1e6,g:1e9}[(m[2]||'').toLowerCase()]||1) : NaN; };
const SID = 'agcsettle-' + Math.floor(Date.now()/1000);
const SETTLE_MS = 45000;

const ws = new WebSocket(`${base}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024`);
const audio = new WebSocket(`${base}/ws/audio?user_session_id=${SID}&codec=opus`);
audio.on('error', () => {}); audio.on('message', () => {});
const send = (o) => ws.send(JSON.stringify(o));
let gain = null, rds = null, sig = null, stereo = null;
ws.on('open', () => {
  send({ type: 'rdsx', on: true });
  // ★★ ASSERT THE THING UNDER TEST. A previous sweep may have left the AGC off, and then this tool
  //    records four stations sitting at the resting gain and reads as "the loop has stopped
  //    working" — which is what happened on the Pi (2026-08-23). Never measure a mode you have not
  //    put the receiver into.
  send({ type: 'gain', auto: true });
});
ws.on('message', (d) => {
  const s = String(d);
  if (s.startsWith('SPEC')) return;
  let j; try { j = JSON.parse(s); } catch { return; }
  if (j.type === 'hwinfo' && Number.isFinite(j.gainNow)) gain = j.gainNow;
  if (j.type === 'rds') { rds = j; if (typeof j.stereo === 'boolean') stereo = j.stereo; }
  if (j.type === 'sig') sig = j;
});
ws.on('error', (e) => { console.error('socket:', e.message); process.exit(1); });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

(async () => {
  await sleep(2000);
  console.log(`${base}\n  station    settles at   SNR    RDS    stereo   trajectory`);
  for (const f of freqs) {
    const F = hz(f);
    send({ type: 'zoom', frequency: F, binBandwidth: 1200 });
    send({ type: 'tune', frequency: F, mode: 'wfm' });
    rds = null; stereo = null;
    const seen = [];
    const t0 = Date.now();
    while (Date.now() - t0 < SETTLE_MS) {
      await sleep(2000);
      if (gain !== null && (!seen.length || seen[seen.length-1] !== gain)) seen.push(gain);
    }
    const snr = sig ? (sig.chan - sig.floor).toFixed(1) : '—';
    const ber = rds && Number.isFinite(rds.ber) ? rds.ber + '%' : '—';
    const ps  = rds && rds.ps ? String(rds.ps).trim() : '';
    console.log(`  ${f.padEnd(8)}  ${((gain ?? 0)/10).toFixed(1).padStart(6)} dB  ${String(snr).padStart(5)}  `
      + `${ber.padStart(5)}  ${(stereo === null ? '—' : stereo ? 'ST' : '·').padStart(6)}   `
      + seen.map(g => (g/10).toFixed(1)).join(' → ') + (ps ? `   [${ps}]` : ''));
  }
  ws.close(); audio.close(); process.exit(0);
})();
