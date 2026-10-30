/**
 * rsp-vibeagc-probe.mjs — hold an RSP on one station and record what its gain loop does and what the station sounds like
 * to the meters: LNA state, IF reduction, ADC peak, whether VibeAGC for SDRplay is running (rspstat), pilot lock, MPX S/N,
 * RDS block errors, stereo. The before/after tool for VibeAGC (SDRplay), 2026-10-30.
 *
 *   node scripts/rsp-vibeagc-probe.mjs ws://192.168.86.13:48001 96.1M 120 [label]
 *   DAB=1 node scripts/rsp-vibeagc-probe.mjs ws://host:port 10D 60      # a DAB block: gain only
 *   STEP=1 node scripts/rsp-vibeagc-probe.mjs ws://host:port 5A 60      # the rapid channel-step repro: 5A → 13F, 0.8 s a hop
 *
 * ★ Opens the audio socket too — a spectrum-only client has no demodulator, so no RDS, no pilot and no MPX meter.
 * ★ Silent: the audio is received and discarded, never played.
 * ★ It TUNES the radio — check the listener count first (ss -tn … sport 48000-48010) and never on a server someone is on.
 */
import WebSocket from 'ws';
const [base, what, secsArg, label = ''] = process.argv.slice(2);
if (!base || !what) { console.error('usage: node scripts/rsp-vibeagc-probe.mjs ws://host:port 96.1M|10D [seconds] [label]'); process.exit(1); }
const SECS = Number(secsArg || 120);
const hz = (t) => { const m = String(t).match(/^([\d.]+)\s*([kKmMgG])?$/); return m ? parseFloat(m[1]) * ({ k: 1e3, m: 1e6, g: 1e9 }[(m[2] || '').toLowerCase()] || 1) : NaN; };
const SID = 'vagcprobe-' + Math.floor(Date.now() / 1000);
const ws = new WebSocket(`${base}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024`);
const audio = new WebSocket(`${base}/ws/audio?user_session_id=${SID}&codec=opus`);
audio.on('error', () => {}); audio.on('message', () => {});
const send = (o) => ws.send(JSON.stringify(o));
let rsp = null, rds = null, rdsx = null;
const samples = [];
ws.on('message', (d) => {
  const s = String(d);
  if (s.startsWith('SPEC') || s[0] !== '{') return;
  let j; try { j = JSON.parse(s); } catch { return; }
  if (j.type === 'rspstat') rsp = j;
  if (j.type === 'rds') rds = j;
  if (j.type === 'rdsx') rdsx = j;
});
ws.on('error', (e) => { console.error('socket:', e.message); process.exit(1); });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const med = (a) => { const b = a.filter(Number.isFinite).sort((x, y) => x - y); return b.length ? b[b.length >> 1] : NaN; };
const DAB_BLOCKS = ['5A','5B','5C','5D','6A','6B','6C','6D','7A','7B','7C','7D','8A','8B','8C','8D','9A','9B','9C','9D',
  '10A','10N','10B','10C','10D','11A','11N','11B','11C','11D','12A','12N','12B','12C','12D','13A','13B','13C','13D','13E','13F'];

(async () => {
  await new Promise((r) => ws.on('open', r));
  send({ type: 'rdsx', on: 1 });   // ★ a NUMBER: the server reads on with jsonNum, and true is not one
  const isDab = !!process.env.DAB || !!process.env.STEP || /^\d+[A-FN]$/.test(what);
  if (process.env.STEP) {
    // ★ the 2026-10-19 repro: DAB, then step rapidly down the channels — every hop a retune, many a rate change
    send({ type: 'dab', on: 1, channel: DAB_BLOCKS.indexOf('5A') });
    await sleep(4000);
    const t0 = Date.now();
    for (let i = 0; Date.now() - t0 < SECS * 1000; ++i) {
      const b = DAB_BLOCKS[i % DAB_BLOCKS.length];
      send({ type: 'dab', on: 1, channel: DAB_BLOCKS.indexOf(b) });
      await sleep(800);
      if (rsp) samples.push({ t: (Date.now() - t0) / 1000, lna: rsp.lna, ifgr: rsp.ifgr, pk: rsp.adcPeak, vagc: rsp.vagc });
    }
    send({ type: 'dab', on: 1, channel: DAB_BLOCKS.indexOf('11D') });   // somewhere real to finish on
    await sleep(20000);
  } else {
    if (isDab) send({ type: 'dab', on: 1, channel: DAB_BLOCKS.indexOf(what) });
    else { const F = hz(what); send({ type: 'zoom', frequency: F, binBandwidth: 1200 }); send({ type: 'tune', frequency: F, mode: 'wfm' }); }
    const t0 = Date.now();
    while (Date.now() - t0 < SECS * 1000) {
      await sleep(1000);
      samples.push({ t: (Date.now() - t0) / 1000, lna: rsp?.lna, ifgr: rsp?.ifgr, pk: rsp?.adcPeak, vagc: rsp?.vagc,
                     sys: rsp?.sysGain, ov: rsp?.overload, snr: rdsx?.mpxSnr, lock: rdsx?.pilotLock, ber: rds?.ber,
                     st: rds?.stereo, ps: rds?.ps });
    }
  }
  // ── summary: the settled half only ──
  const late = samples.filter((s) => s.t >= SECS / 2);
  const moves = (k) => samples.reduce((n, s, i) => n + (i && s[k] !== samples[i - 1][k] ? 1 : 0), 0);
  console.log(`${label || what}  ${base}`);
  console.log(`  VibeAGC: ${late.some((s) => s.vagc) ? 'ON' : 'off'} · LNA ${med(late.map((s) => s.lna))} (moves ${moves('lna')}) · IF ${med(late.map((s) => s.ifgr))} dB (moves ${moves('ifgr')}) · ADC peak ${med(late.map((s) => s.pk))?.toFixed?.(1)} dBFS · overload ${late.filter((s) => s.ov).length}/${late.length}`);
  if (!isDab) console.log(`  MPX S/N ${med(late.map((s) => s.snr))?.toFixed?.(1)} dB · pilot locked ${late.filter((s) => s.lock).length}/${late.length} · RDS BER ${med(late.map((s) => s.ber))} % · stereo ${late.filter((s) => s.st).length}/${late.length} · PS "${(late.at(-1)?.ps || '').trim()}"`);
  if (process.env.CSV) for (const s of samples) console.log(JSON.stringify(s));
  ws.close(); audio.close(); setTimeout(() => process.exit(0), 300);
})();
